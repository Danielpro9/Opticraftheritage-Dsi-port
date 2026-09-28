#include "platform/Input.h"

#ifdef DSI_PLATFORM

#include <cstdint>
#include <nds.h>

#include "dsi/DsiEarlyInit.h"
#include "lwjgl/Display.h"
#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"

// Menu-navigation input: the D-pad and A/B drive the console-style menu list
// (GuiMainMenu.cpp's updateScreen() PLATFORM_PS2 || PLATFORM_WII branch, now
// also PLATFORM_DSI), the same mechanism PS2/Wii already use --
// InputBackend_PS2.cpp is the reference this mirrors.
//
// In-game controls, added directly on request. Full scheme:
//   D-pad            movement (platformGamepadSnapshot()'s leftX/leftY, see
//                     below)
//   Touch screen      camera (rightX/rightY, see below) -- a fixed-anchor
//                     virtual stick, not a raw drag: see the
//                     g_touchAnchorX/g_touchStickX comment below for why.
//   L                 place block  (keyBindUseItem) with no screen open;
//                     place ONE item from the held stack (vanilla's
//                     right-click) inside a container screen -- see
//                     ContainerSlotNavigator.cpp
//   R                 break block  (keyBindAttack) with no screen open;
//                     drop the held stack (vanilla's click-outside-the-
//                     inventory) inside a container screen -- see
//                     ContainerSlotNavigator.cpp
//   A                 jump         (keyBindJump)
//   X                 inventory    (keyBindInventory)
//   Y                 chat         (keyBindChat -- multiplayer only, see
//                     dsiPushGameplayKeyEvents()'s own comment)
//   B + D-pad Left/Right   step the hotbar selection
//   START             pause/menu (synthesizes Escape -- see
//                     dsiPushGameplayKeyEvents()'s own comment and
//                     Display_dsi.cpp's header comment for why this replaced
//                     START-quits-the-game)
//
// Movement/camera feed MovementInputFromOptions.cpp / EntityRenderer.cpp's
// PLATFORM_DIRECT_ANALOG_MOVEMENT / PLATFORM_DIRECT_CAMERA_ENABLED paths, the
// same ones PS2's analog stick already uses (see DsiInputTuning.h) -- nothing
// new was invented for how a "stick" value turns into movement/camera motion,
// only how the D-pad/touchscreen produce that value.
//
// L/R/A/X/Y/hotbar instead go through dsiPushGameplayKeyEvents() below,
// which synthesizes the same lwjgl::Keyboard/Mouse events a real keyboard or
// mouse press would queue (Keyboard_dsi.cpp/Mouse_dsi.cpp now carry a real
// event queue, fed the same way PS2's Keyboard_ps2.cpp/Mouse_ps2.cpp already
// are) -- Minecraft.cpp's existing input-processing loop (KeyBinding state,
// clickMouse(), changeCurrentItem() for the wheel) already does the right
// thing with those events unmodified, so this reuses that instead of
// duplicating its logic.
//
// Does not call scanKeys() here: Display_dsi.cpp's processMessages() already
// calls it exactly once per frame (its isCloseRequested()/KEY_START check
// depends on that). Calling it again here would reset libnds's
// press/release edge-detection state mid-frame and make keysDown() miss
// presses -- this only ever reads back the state that scan already captured.
namespace
{
// Touch-as-analog-stick camera state. Updated once a frame by
// dsiUpdateTouchCameraDelta() (called from Display_dsi.cpp's
// processMessages(), see that function's own comment on why this can only
// happen once a frame), read as often as needed by
// platformGamepadSnapshot() afterwards without disturbing it.
//
// REDESIGNED from a frame-to-frame drag delta to a fixed-anchor virtual
// stick, on real-hardware feedback: the old scheme reported the pixel
// distance the finger moved SINCE LAST FRAME, which drops back to zero the
// instant the finger stops moving even if it is still held down and away
// from where it started -- turning the camera meant repeatedly swipe,
// (motion stops, turn stops), lift, swipe again, exactly like nudging a
// D-pad instead of holding a stick over. Consulted ClassiCube's own DS
// port (src/nds/Window_NDS.c's ProcessTouchInput()) for comparison: it
// only forwards raw touch positions into its generic multi-touch
// abstraction and does not implement anything DS-specific here either, so
// there was no existing "reference implementation" to copy -- this is a
// fresh design, not a port.
//
// New model: the FIRST touched position each press becomes a fixed anchor
// (like a stick's center); every frame after that while still touching
// reports the CURRENT offset from that anchor, not from the previous
// frame's position. Holding the finger stationary at an offset keeps
// reporting that same nonzero offset every frame -- exactly the "hold in a
// direction to keep turning" feel EntityRenderer.cpp's direct-camera path
// already expects (it already treats rightX/rightY as a continuous
// per-frame rate, scaled by elapsed time -- see its own comment -- not a
// one-shot delta to consume; only the OLD production side here was
// mismatched with that consumer, not the consumer itself). Releasing and
// touching down again picks a new anchor whereever the finger lands next,
// so there is no stale offset from a finished gesture.
bool g_touchWasDown = false;
int g_touchAnchorX = 0;
int g_touchAnchorY = 0;
float g_touchStickX = 0.0f;
float g_touchStickY = 0.0f;

// The anchor-to-finger offset, in pixels, that reads as a fully deflected
// stick (matches the [-1, 1] range Ps2AnalogFilter::apply() produces for
// PS2's real stick, which PLATFORM_DIRECT_CAMERA_SCALE and friends were
// tuned against -- see DsiInputTuning.h). Larger than the old drag-delta
// version's 24px on purpose: that number sized a single FRAME's worth of
// motion, this one sizes a SUSTAINED hold offset a thumb can comfortably
// keep still at without drifting past the physical screen edge from a
// reasonably-centred touch-down point. Unverified against real hardware
// feel, same caveat as DsiInputTuning.h's own -- the first real-hardware
// test of this redesign is what confirms whether 40px feels right.
constexpr float kTouchStickPixelsForFullDeflection = 40.0f;

// Previous frame's held mask for the L/R/A/X/Y action buttons, so
// dsiPushGameplayKeyEvents() can tell a fresh press/release apart from a
// button still being held (a real keyboard/mouse only sends one down event
// per press, not one every frame it stays held).
std::uint32_t g_prevActionButtons = 0;

// Menu-pointer ownership state -- see dsiUpdateMenuPointer()'s own comment
// and DsiEarlyInit.h's declaration for the full picture. Separate from the
// touch-drag camera state above: that one tracks a delta for gameplay,
// this tracks an absolute position and a tap-to-click gesture for menu
// input, and the two run in different situations (inMenu false vs true) so
// they never fight over the same touchRead() sample in a way that matters.
enum class DsiMenuInputOwner { Pad, Pointer };
DsiMenuInputOwner g_dsiMenuInputOwner = DsiMenuInputOwner::Pad;
bool g_dsiMenuTouchWasDown = false;
// Cursor position at the last frame the screen was actually touched, in
// lwjgl::Mouse's coordinate space (already Y-flipped -- see
// dsiUpdateMenuPointer()). Real-hardware report this exists to fix: reading
// a fresh touchRead() sample on the exact release frame (the panel is no
// longer pressed by then) made the cursor jump to wherever the digitizer's
// last raw/settling reading happened to be instead of staying where the
// player actually lifted their finger -- looked like the pointer vanishing.
// Every use of "the cursor's position" outside an active touch (the
// release-triggered click, the cursor staying visible afterward) reads
// this instead of touchRead() directly.
int g_dsiMenuLastTouchX = 0;
int g_dsiMenuLastTouchY = 0;

// Synthesizes a full click (press immediately followed by release, both at
// the last known touch position) into lwjgl::Mouse -- see
// dsiUpdateMenuPointer()'s own comment for when this fires. Pushing both
// halves lets GuiButton's normal mousePressed()/mouseReleased() pair run
// exactly as it would for a real mouse click, rather than leaving
// GuiScreen::selectedButton latched with no matching release.
void dsiFireMenuPointerClick()
{
	lwjgl::Mouse::detail::pushButton(0, true, g_dsiMenuLastTouchX, g_dsiMenuLastTouchY);
	lwjgl::Mouse::detail::pushButton(0, false, g_dsiMenuLastTouchX, g_dsiMenuLastTouchY);
}

float normalizeStickOffset(float offsetPixels)
{
	float value = offsetPixels / kTouchStickPixelsForFullDeflection;
	if (value < -1.0f) value = -1.0f;
	if (value > 1.0f) value = 1.0f;
	return value;
}

std::uint32_t mapTextButtons(std::uint32_t bits)
{
	std::uint32_t value = 0;
	if (bits & KEY_LEFT)  value |= PLATFORM_TEXT_LEFT;
	if (bits & KEY_RIGHT) value |= PLATFORM_TEXT_RIGHT;
	if (bits & KEY_UP)    value |= PLATFORM_TEXT_UP;
	if (bits & KEY_DOWN)  value |= PLATFORM_TEXT_DOWN;
	if (bits & KEY_A)     value |= PLATFORM_TEXT_TYPE;
	// B is this hardware's one "cancel/go back" button, so it stands in for
	// both flags other platforms split across two buttons (PS2: Square for
	// BACK -- see LegacyOptionsScreen.cpp/GuiIngameMenu.cpp's separate BACK
	// checks -- Circle for CLOSE, the "leave this screen" check most legacy
	// screens gate on, e.g. LegacyOptionsScreen.cpp's CLOSE|SHIFT check).
	if (bits & KEY_B)     value |= PLATFORM_TEXT_BACK | PLATFORM_TEXT_CLOSE;
	// Real-hardware request: L/R (world "place block"/"break block" while no
	// screen is open -- see dsiPushGameplayKeyEvents() below, which stops
	// synthesizing those while a screen IS open specifically so this doesn't
	// double up) double as dedicated container-screen actions instead:
	// ContainerSlotNavigator.cpp reads these two bits to place one item from
	// the held stack (L) or drop the whole held stack (R), independent of
	// whichever slot A/B's existing primary/secondary click targets.
	if (bits & KEY_L)     value |= PLATFORM_TEXT_SECONDARY;
	if (bits & KEY_R)     value |= PLATFORM_TEXT_DROP;
	return value;
}
}

PlatformTextInputSnapshot platformTextInputSnapshot(int)
{
	PlatformTextInputSnapshot out;
	out.connected = true; // The D-pad/buttons are built into the hardware, always "there".
	out.held = mapTextButtons(keysHeld());
	out.pressed = mapTextButtons(keysDown());
	return out;
}

void dsiUpdateTouchCameraDelta()
{
	touchPosition touch;
	touchRead(&touch);
	const bool touching = (keysHeld() & KEY_TOUCH) != 0;

	// A fresh touch-down (touching now, was NOT touching last frame) plants
	// a new anchor right where the finger landed -- this press's stick
	// center, unrelated to wherever the previous press's anchor was.
	if (touching && !g_touchWasDown)
	{
		g_touchAnchorX = touch.px;
		g_touchAnchorY = touch.py;
	}

	if (touching)
	{
		g_touchStickX = static_cast<float>(static_cast<int>(touch.px) - g_touchAnchorX);
		g_touchStickY = static_cast<float>(static_cast<int>(touch.py) - g_touchAnchorY);
	}
	else
	{
		g_touchStickX = 0.0f;
		g_touchStickY = 0.0f;
	}

	g_touchWasDown = touching;
}

// See DsiEarlyInit.h's declaration for the full picture. inMenu comes from
// Display_dsi.cpp's processMessages(), computed the same way
// Display_wii.cpp's does (Minecraft::getMinecraft()->currentScreen != nullptr).
void dsiUpdateMenuPointer(bool inMenu)
{
	if (!inMenu)
	{
		// No screen open: reset ownership and the touch-edge tracker so the
		// next screen that opens starts on Pad ownership with no stale
		// "was touching" carried over from whatever the player was doing on
		// the previous screen (or in gameplay, where KEY_TOUCH drives the
		// camera instead -- see dsiUpdateTouchCameraDelta() above).
		g_dsiMenuInputOwner = DsiMenuInputOwner::Pad;
		g_dsiMenuTouchWasDown = false;
		return;
	}

	touchPosition touch;
	touchRead(&touch);
	const bool touching = (keysHeld() & KEY_TOUCH) != 0;

	if (touching)
	{
		g_dsiMenuInputOwner = DsiMenuInputOwner::Pointer;

		// LWJGL's Mouse::getY() is bottom-left origin (see PlatformCompat.h's
		// getMouseState()), but touchRead()'s px/py -- like every other pixel
		// coordinate in this codebase (e.g. DsiEarlyVideo.cpp's glViewport) --
		// are top-left origin. Flip once here, at the one place a real touch
		// position enters lwjgl::Mouse, rather than asking every reader of
		// Mouse::getY() to know DSi needs special handling.
		g_dsiMenuLastTouchX = touch.px;
		g_dsiMenuLastTouchY = lwjgl::Display::getHeight() - 1 - static_cast<int>(touch.py);
		lwjgl::Mouse::detail::pushMotion(g_dsiMenuLastTouchX, g_dsiMenuLastTouchY, 0, 0);
	}

	// Tap-to-click, real-hardware request: touching and dragging only
	// repositions the cursor (no click on touch-down, unlike the first
	// version of this function), so lining the cursor up doesn't activate
	// whatever it happened to start on top of. The click itself fires on
	// release, at g_dsiMenuLastTouchX/Y rather than a fresh touchRead()
	// sample -- see that variable's own comment for why the sample cannot
	// be trusted once the panel is no longer pressed.
	if (!touching && g_dsiMenuTouchWasDown)
		dsiFireMenuPointerClick();
	g_dsiMenuTouchWasDown = touching;

	const std::uint32_t pressedEdge = keysDown();

	// A also clicks, at the same last-touched position lifting the finger
	// would -- the other real-hardware request, so the cursor can be
	// aimed and then confirmed without needing to lift and re-tap. Only
	// while the pointer currently owns menu input: with nothing touched
	// yet this screen (ownership still Pad), A falls through unchanged to
	// handleConsoleJavaUiNavigation()'s normal keyboard-selection
	// activation below. Deliberately does NOT also hand ownership back to
	// the pad the way a D-pad direction does (see below) -- doing so would
	// make handleConsoleJavaUiNavigation() see platformMenuPointerActive()
	// as already false this same frame and run its own
	// activateKeyboardSelection() too, double-firing whatever both paths
	// agree on.
	if (g_dsiMenuInputOwner == DsiMenuInputOwner::Pointer && (pressedEdge & KEY_A))
		dsiFireMenuPointerClick();

	// A D-pad direction or B hands ownership back to the pad, mirroring
	// Wii's updateMenuInputOwner() (WiiPadState.cpp) -- "aim to use the
	// cursor, press a direction to go back to navigation" (GuiScreen.cpp's
	// menuPointerInputSuppressed() comment). A is excluded (see just above):
	// it stays a pointer action while the pointer owns input, not a
	// navigation handoff.
	if (pressedEdge & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_B))
		g_dsiMenuInputOwner = DsiMenuInputOwner::Pad;
}

void dsiPushGameplayKeyEvents(bool inMenu)
{
	const std::uint32_t held = keysHeld();
	// keysDown(): bits newly pressed since the scanKeys() call Display_dsi.cpp's
	// processMessages() already made this frame (see this file's header
	// comment on why nothing here calls scanKeys() itself).
	const std::uint32_t pressedEdge = keysDown();
	const std::uint32_t changed = held ^ g_prevActionButtons;

	// L/R: place/break, but ONLY with no screen open. keyBindUseItem/
	// keyBindAttack default to mouse buttons 1/0 (GameSettings.cpp's
	// -99/-100 keyCodes), so a real mouse press is what these need to look
	// like, not a keyboard key -- and that is exactly the problem while a
	// screen IS open: GuiContainer::mouseClicked() reacts to those same
	// button 0/1 events as a real click at wherever the cursor currently
	// is, which would double up with (or fire instead of) the dedicated L=
	// place-one/R=drop container actions mapTextButtons() now derives from
	// these same two physical buttons (see ContainerSlotNavigator.cpp).
	// Gating on inMenu the same way dsiUpdateMenuPointer() already does
	// keeps L/R meaning exactly one thing at a time -- world block place/
	// break with nothing open, dedicated inventory actions with a container
	// screen open -- instead of both at once.
	if (!inMenu)
	{
		if (changed & KEY_L)
			lwjgl::Mouse::detail::pushButton(1, (held & KEY_L) != 0, 0, 0);
		if (changed & KEY_R)
			lwjgl::Mouse::detail::pushButton(0, (held & KEY_R) != 0, 0, 0);
	}

	// A/X/Y: jump/inventory/chat, keyBindJump/keyBindInventory/keyBindChat's
	// default keyboard keys (space/E/T).
	if (changed & KEY_A)
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_SPACE, (held & KEY_A) != 0);
	if (changed & KEY_X)
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_E, (held & KEY_X) != 0);
	// START: pause/menu, not quit -- see Display_dsi.cpp's header comment.
	// Escape is exactly the right synthesized key: Minecraft.cpp already
	// opens the pause menu on Escape when no screen is up, and every
	// GuiScreen's base keyTyped() already closes back to the game on Escape
	// when one is, so this needs no DSi-specific menu handling at all.
	if (changed & KEY_START)
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_ESCAPE, (held & KEY_START) != 0);
	// Y: temporarily mapped to F3 (toggles Minecraft.cpp's showDebugInfo
	// overlay) on request -- chat (its original binding, keyBindChat's
	// default KEY_T) only opens in a multiplayer world, which this
	// NO_NETWORK build can never have, so Y was doing nothing. Revisit if
	// chat ever becomes reachable and Y needs to go back to it.
	if (changed & KEY_Y)
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_F3, (held & KEY_Y) != 0);

	// B + Left/Right: step the hotbar selection, the same mouse-wheel path
	// a real scroll wheel drives (Minecraft.cpp's
	// thePlayer->inventory->changeCurrentItem(wheel)). A chord, not its own
	// binding, since B alone already means "back/close" in menu navigation
	// (see mapTextButtons() above) -- the two never conflict, since a menu
	// being open and gameplay both consuming input at once can't happen.
	//
	// InventoryPlayer::changeCurrentItem() does `currentItem -= direction`,
	// so a positive wheel value DECREASES the slot index (moves the
	// highlight left on screen) and a negative one INCREASES it (moves
	// right). This was originally backwards here -- Left pushed -1 (which
	// increases the index, moving the highlight right) and Right pushed +1
	// (which decreases it, moving left) -- confirmed on real hardware:
	// pressing Right visibly moved the selection left. Ps2InputMapper.cpp's
	// R1/L1 mapping (pushWheel(-1) for the right-side button, pushWheel(1)
	// for the left) has the correct sign for each direction; swapped to
	// match it.
	if (held & KEY_B)
	{
		if (pressedEdge & KEY_LEFT)
			lwjgl::Mouse::detail::pushWheel(1, 0, 0);
		if (pressedEdge & KEY_RIGHT)
			lwjgl::Mouse::detail::pushWheel(-1, 0, 0);
	}

	g_prevActionButtons = held;
}

PlatformGamepadSnapshot platformGamepadSnapshot(int)
{
	PlatformGamepadSnapshot out;
	out.connected = true; // Built into the hardware, always "there".

	const std::uint32_t held = keysHeld();
	// Digital D-pad: each axis is -1, 0 or +1, no deadzone to apply.
	// MovementInputFromOptions.cpp computes moveForward += -leftY and
	// moveStrafe += -leftX, so up/left need to be the negative direction for
	// forward/strafe-left to come out positive the way that file expects.
	//
	// Real-hardware report: holding B to step the hotbar (dsiPushGameplayKeyEvents()'s
	// own B+Left/Right chord above) also strafed the player, because this
	// function read Left/Right unconditionally into leftX with no knowledge
	// of what B was doing with the same two buttons. While B is held, Left/
	// Right belong to the hotbar chord instead -- suppress them here so the
	// player holds still during hotbar selection, matching the "the two
	// never conflict" assumption the B+Left/Right comment above already
	// states (menu navigation's mouseWheel path has no such conflict either,
	// since it is never live at the same time as gameplay movement).
	const bool hotbarChordActive = (held & KEY_B) != 0;
	out.leftX = hotbarChordActive ? 0.0f : (held & KEY_LEFT) ? -1.0f : (held & KEY_RIGHT) ? 1.0f : 0.0f;
	out.leftY = (held & KEY_UP)   ? -1.0f : (held & KEY_DOWN)  ? 1.0f : 0.0f;

	// Touch-as-stick offset, normalized to the same [-1, 1] "stick
	// deflection" range EntityRenderer.cpp's direct-camera path expects. See
	// dsiUpdateTouchCameraDelta() for where this is actually computed --
	// exactly once a frame, not here, so calling this more than once in the
	// same frame (movement and camera each call it separately) reads a
	// stable value instead of consuming it.
	out.rightX = normalizeStickOffset(g_touchStickX);
	out.rightY = normalizeStickOffset(g_touchStickY);

	return out;
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int)
{
	return PlatformGamepadSnapshot{};
}

int platformMenuPad()
{
	return 0;
}

// See dsiUpdateMenuPointer() (called once a frame from Display_dsi.cpp) for
// where g_dsiMenuInputOwner actually changes.
bool platformMenuPointerActive()
{
	return g_dsiMenuInputOwner == DsiMenuInputOwner::Pointer;
}

bool platformMenuCursorVisible()
{
	return g_dsiMenuInputOwner == DsiMenuInputOwner::Pointer;
}

// Unlike Wii's IR pointer or PS2's stick-driven cursor, a touch position
// cannot be programmatically warped -- it only changes when the player
// physically touches the screen -- so there is nothing for this to do.
// legacyMoveMenuCursorToSelection() (LegacyMenuNavigation.cpp) already knows
// to skip calling this outside PLATFORM_WII for exactly this reason.
void platformSetMenuCursor(int, int)
{
}

const PlatformKeyboardHints& platformKeyboardHints()
{
	static const PlatformKeyboardHints hints{};
	return hints;
}

const char* platformInputDebugLine()
{
	return "";
}

#endif // DSI_PLATFORM
