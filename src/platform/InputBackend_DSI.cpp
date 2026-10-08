#include "platform/Input.h"

#ifdef DSI_PLATFORM

#include <cmath>
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
//   Touch screen      camera -- a direct frame-to-frame drag delta fed
//                     through lwjgl::Mouse, the same path a real mouse
//                     drives on PC: see dsiUpdateTouchCameraDelta()'s own
//                     comment for the two earlier designs this replaced.
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
//   Y                 F3 debug overlay (see dsiPushGameplayKeyEvents()'s own
//                     comment on why Y doesn't go to chat instead)
//   B + D-pad Left/Right   step the hotbar selection
//   B + D-pad Up           chat (keyBindChat's default key T) -- now that
//                     multiplayer is live (see NetworkManager.h's own
//                     DSI_PLATFORM comment), chat has somewhere to open
//   START             pause/menu (synthesizes Escape -- see
//                     dsiPushGameplayKeyEvents()'s own comment and
//                     Display_dsi.cpp's header comment for why this replaced
//                     START-quits-the-game)
//
// Movement feeds MovementInputFromOptions.cpp's PLATFORM_DIRECT_ANALOG_MOVEMENT
// path, the same one PS2's analog stick already uses for its D-pad-shaped
// leftX/leftY (see DsiInputTuning.h) -- nothing new was invented for how
// that value turns into movement, only how the D-pad produces it. The
// camera is different: PlatformInputTuning.h routes DSi through
// EntityRenderer.cpp's ordinary PC-style mouse-delta path instead of PS2's
// PLATFORM_DIRECT_CAMERA_ENABLED stick-rate one (see
// dsiUpdateTouchCameraDelta()'s own comment for why), so rightX/rightY
// below are not part of that path at all any more.
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
// Touch-drag camera state. Updated once a frame by
// dsiUpdateTouchCameraDelta() (called from Display_dsi.cpp's
// processMessages(), see that function's own comment on why this can only
// happen once a frame).
//
// SECOND redesign of this function, on further real-hardware feedback: the
// first version (a fixed-anchor virtual stick -- touch down plants a
// center, holding the finger at an offset from it reported that offset
// every frame as a stick deflection) fixed the original drag-delta
// version's problem (turning required repeated swipe-lift-swipe, since a
// delta that resets to zero the instant the finger stops moving cannot
// sustain a turn) but overcorrected: reported feel was "tosca" (crude) and
// laggy, because EntityRenderer.cpp's direct-camera path treats that
// deflection as a RATE to integrate over time (exactly right for a
// self-centering analog stick, which is what that path was built for), not
// as a direct position -- so a small, natural hold-offset near the anchor
// turned the camera only slowly, no matter how far the finger had actually
// moved to get there.
//
// Re-checked ClassiCube's DS port with that framing in mind: Window_NDS.c's
// own ProcessTouchInput() is indeed just a thin forward into ClassiCube's
// generic multi-touch abstraction, as found before, but that abstraction's
// actual camera consumer (src/Input.c's TryUpdateTouch(), src/Camera.c's
// Camera_OnRawMovement()/PerspectiveCamera_GetMouseDelta()) turns out to be
// exactly the reference this needed: touch drag is fed as a raw FRAME-TO-
// FRAME PIXEL DELTA (current sample minus the PREVIOUS frame's, not minus a
// fixed anchor), consumed once and reset, precisely the way a mouse's
// hardware delta already works -- not integrated as a sustained rate.
//
// This fork already has that exact consumption path, just never wired to
// touch: EntityRenderer.cpp's PLATFORM_DIRECT_CAMERA_ENABLED-off branch
// reads mc->mouseHelper->deltaX/deltaY (fed by lwjgl::Mouse::getDX/getDY,
// PlatformInputTuning.h's PC leg) with the exact same sensitivity formula
// PC's real mouse uses. So the fix is not a new formula, it is switching
// DSi onto that existing path (PlatformInputTuning.h) and, here, feeding it
// a real frame-to-frame pixel delta the same way dsiUpdateMenuPointer()
// already feeds the menu cursor's position -- through lwjgl::Mouse::detail
// ::pushMotion(), whose xrel/yrel arguments stage into
// getDX()/getDY()'s accumulator and reset it on read, same shape as
// ClassiCube's stage-then-consume.
//
// g_touchLastX/Y hold last frame's raw touch sample so this frame's delta
// against it is well-defined; g_touchWasDown gates the very first frame of
// a fresh touch-down (no prior sample yet, so no delta to report -- an
// implicit small dead zone, similar in spirit to ClassiCube's own
// MovedFromBeg() gate before it locks a touch to camera control, without
// needing to port that state machine: DSi's touch screen has no on-screen
// buttons to disambiguate against during gameplay, unlike a phone's
// touch-over-the-viewport controls, so every drag is a camera drag here).
bool g_touchWasDown = false;
int g_touchLastX = 0;
int g_touchLastY = 0;

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

// Real-hardware feedback on the first mouse-delta build: up/down was
// inverted, and the drag felt like it needed much more sensitivity than a
// real mouse would to turn the same amount.
//
// Inversion: touch.py increases DOWN the screen (the DS panel's own raster
// convention, top-left origin), so a finger dragging up (wanting to look
// up) reports a NEGATIVE raw delta -- but LWJGL's mouse delta convention is
// the opposite sign (positive deltaY = moved UP; see MouseHelper.cpp's own
// callers and PS2_DIRECT_CAMERA_INVERT_Y's identical note for the pad
// path). Negating dy here before it reaches pushMotion() is what makes a
// raw touch sample match that convention -- the same fix DSI_DIRECT_CAMERA_
// INVERT_Y made for the old stick-rate path, now needed at the source
// instead of after the fact because that whole INVERT_Y knob no longer
// exists on this path (PlatformInputTuning.h routes DSi through the plain
// mouse-delta branch, which has no invert knob of its own -- it assumes
// whatever feeds it already matches real mouse sign, same as PC's own
// mouseHelper does).
//
// Sensitivity: a thumb drag across the DS's ~6.5cm touch panel physically
// covers far fewer screen pixels per frame than the same "flick" gesture
// with a real mouse on a desk, even at the same felt speed -- the panel is
// tiny and DSi only samples it once a frame, unlike a mouse's much higher
// native poll rate. Scaling the raw delta up before it enters the shared
// mouse-sensitivity formula (EntityRenderer.cpp, unmodified, exactly PC's)
// compensates for that physical difference at the source rather than
// changing the shared formula itself, which PC still relies on for its own
// feel. The original 2.5x (applied equally to both axes) was a first
// real-hardware-informed guess, not a measurement -- first hands-on
// feedback round: overall feel was too slow, yaw (turning left/right)
// noticeably more so than pitch (looking up/down), which already felt
// close to right. Split into two knobs instead of one shared scale so yaw
// can be raised more without over-shooting pitch; still just an informed
// estimate from that one feedback round, not a measurement -- keep tuning
// from further hands-on rounds.
constexpr float kTouchCameraSensitivityScaleX = 3.5f;
constexpr float kTouchCameraSensitivityScaleY = 2.8f;

void dsiUpdateTouchCameraDelta(bool inMenu)
{
	touchPosition touch;
	touchRead(&touch);
	const bool touching = (keysHeld() & KEY_TOUCH) != 0;

	// A menu is using the touch screen as a pointer this frame instead (see
	// dsiUpdateMenuPointer(), called right after this one) -- do not also
	// stage camera motion into the same lwjgl::Mouse accumulator underneath
	// it. Still track g_touchLastX/Y below regardless, so leaving a menu
	// mid-drag does not report one huge delta from a stale pre-menu sample
	// on the first gameplay frame afterward.
	if (touching && g_touchWasDown && !inMenu)
	{
		const int rawDx = static_cast<int>(touch.px) - g_touchLastX;
		const int rawDy = static_cast<int>(touch.py) - g_touchLastY;
		if (rawDx != 0 || rawDy != 0)
		{
			const int dx = static_cast<int>(std::lround(rawDx * kTouchCameraSensitivityScaleX));
			const int dy = static_cast<int>(std::lround(-rawDy * kTouchCameraSensitivityScaleY));
			lwjgl::Mouse::detail::pushMotion(touch.px, touch.py, dx, dy);
		}
	}

	if (touching)
	{
		g_touchLastX = touch.px;
		g_touchLastY = touch.py;
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

	// Real-hardware report: typing a player name through the on-screen
	// keyboard (A = type the selected key, B = backspace/close) also typed
	// a stray space on every A press and backspaced-then-closed the
	// keyboard on every B press -- this whole block was never checking
	// platformTextInputExclusive() before synthesizing anything, so every
	// physical button VirtualKeyboard::tick() reads for its own purpose
	// (mapTextButtons() above maps the same A/B to PLATFORM_TEXT_TYPE/
	// PLATFORM_TEXT_BACK|PLATFORM_TEXT_CLOSE) fired BOTH handlers at once.
	// PS2's Ps2InputMapper.cpp and Wii's WiiPadState.cpp already guard their
	// own equivalent of this block the same way (WiiPadState.cpp zeroes
	// state.keys/mouse/wheel outright while it is set); this was simply
	// missing here. While the keyboard owns input, VirtualKeyboard::tick()
	// (called every frame from GuiScreen.cpp regardless of this function)
	// is the sole source of keyboard/mouse events -- nothing below this
	// point should also react to the same physical buttons.
	if (!platformTextInputExclusive())
	{
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
		// Y: mapped to F3 (toggles Minecraft.cpp's showDebugInfo overlay) rather
		// than back to chat now that multiplayer is live -- B+Up (below) is the
		// dedicated chat chord instead, the same reasoning that put hotbar
		// stepping on a B chord rather than its own button: there is no spare
		// single button left, and chat is not needed often enough to deserve
		// one of the few there are.
		if (changed & KEY_Y)
			lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_F3, (held & KEY_Y) != 0);

		// B + Left/Right: step the hotbar selection, the same mouse-wheel path
		// a real scroll wheel drives (Minecraft.cpp's
		// thePlayer->inventory->changeCurrentItem(wheel)). A chord, not its own
		// binding, since B alone already means "back/close" in menu navigation
		// (see mapTextButtons() above).
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
		//
		// B + Up: open chat (keyBindChat's default key T), added on request the
		// same way as the hotbar chord above -- B has no meaning of its own in
		// gameplay, so pairing it with a D-pad direction costs no existing
		// binding. Pushed as an immediate down+up tap rather than tracked with
		// `changed` like A/X/Y above: those three are keys Minecraft expects to
		// see genuinely held (space for repeated jumping, E/F3 toggles checked
		// on their own down edge), but a synthesized T only needs to exist for
		// the one instant that opens GuiChat -- once that screen is up, further
		// typing comes from the on-screen keyboard, not from T staying "held".
		if (held & KEY_B)
		{
			if (pressedEdge & KEY_LEFT)
				lwjgl::Mouse::detail::pushWheel(1, 0, 0);
			if (pressedEdge & KEY_RIGHT)
				lwjgl::Mouse::detail::pushWheel(-1, 0, 0);
			if (pressedEdge & KEY_UP)
			{
				lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_T, true);
				lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_T, false);
			}
		}
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
	// of what B was doing with the same two buttons. While B is held, the
	// whole D-pad belongs to B's chords instead (Left/Right = hotbar, Up =
	// chat) -- suppress all four here so the player holds still during any
	// of them, matching the "the two never conflict" assumption the
	// B+Left/Right comment above already states (menu navigation's
	// mouseWheel path has no such conflict either, since it is never live
	// at the same time as gameplay movement). Down has no chord of its own
	// yet, but gets held to the same "B means stand still" rule rather than
	// left as a special case that only some directions follow.
	const bool bChordActive = (held & KEY_B) != 0;
	out.leftX = bChordActive ? 0.0f : (held & KEY_LEFT) ? -1.0f : (held & KEY_RIGHT) ? 1.0f : 0.0f;
	out.leftY = bChordActive ? 0.0f : (held & KEY_UP)   ? -1.0f : (held & KEY_DOWN)  ? 1.0f : 0.0f;

	// The touch screen no longer reports as a stick deflection here --
	// PlatformInputTuning.h now routes DSi's camera through EntityRenderer.cpp's
	// ordinary mouse-delta path instead (see dsiUpdateTouchCameraDelta()'s own
	// comment for why), which reads mc->mouseHelper->deltaX/deltaY rather than
	// this snapshot's rightX/rightY. Left at 0 rather than removed: PS2 still
	// has a real analog stick reporting through this same field, so
	// PlatformGamepadSnapshot keeps the member either way.
	out.rightX = 0.0f;
	out.rightY = 0.0f;

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
