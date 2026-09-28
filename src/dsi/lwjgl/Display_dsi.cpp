// Display_dsi.cpp — DSi implementation of lwjgl::Display.
//
// Thin wrapper, same shape as Display_wii.cpp: DsiEarlyVideo.cpp owns the
// actual video/GL setup. What lives here is the lwjgl-shaped surface plus:
//
//   * START used to map straight to "close requested" (every DsiBringup.cpp
//     demo still does, for that standalone smoke-test context). In the real
//     game that meant the only way to reach the pause/options menu -- to
//     quit cleanly, change settings, etc. -- was to power off the console,
//     since nothing else ever requested a menu. Real hardware feedback asked
//     for START to behave like desktop Minecraft's Escape key instead: pause
//     when nothing else is open, close whatever menu/screen IS open
//     otherwise. Synthesizing a KEY_ESCAPE keyboard event on press does
//     exactly that for free, reusing Minecraft.cpp's existing
//     currentScreen==nullptr -> displayInGameMenu() branch and every
//     GuiScreen's base keyTyped() (key==1 closes back to the game) --
//     nothing DSi-specific needed on the menu side. See
//     InputBackend_DSI.cpp's dsiPushGameplayKeyEvents() for where that push
//     actually happens, right alongside A/X/Y. There is still no real
//     in-game way to quit -- powering off the console remains the only
//     exit, now a deliberate choice instead of an accident of START having
//     nothing else to do.
//
//   * The video mode is not a real choice: the DS/DSi top screen is always
//     256x192, so setDisplayMode() has nothing to apply.
#ifdef DSI_PLATFORM

#include "lwjgl/Display.h"

#include <nds.h>

#include "dsi/DsiEarlyInit.h"
#include "platform/Log.h"

#include "client/Minecraft.h"

namespace
{
bool g_created = false;
bool g_closeRequested = false;

// No debugger and no serial console on real hardware, and a hang leaves
// nothing else on screen to say the game is still alive -- the difference
// between "drawing the same frame forever" and "stuck in an infinite loop
// with vblank never reached again" is otherwise invisible from outside the
// console. This writes one line to sd:/OptiCraft/debug.log (see
// DsiEarlyStorage.cpp) roughly once a second; if that line stops advancing,
// whatever ran between it and the next one is where things stopped. Cheap
// enough to leave on permanently: MC_LOG_SYNC_WRITES commits it immediately
// either way (see Log.h), so this adds one open/write/flush per ~60 frames,
// not per frame.
void heartbeat()
{
	static unsigned int frame = 0;
	++frame;
	if (frame % 60 != 0)
		return;
	MC_LOG_INFO("dsi", "heartbeat frame=%u heap=%u/%uKB\n",
		frame, (unsigned)(dsiGetHeapCommitted() / 1024u), (unsigned)(dsiGetHeapCeiling() / 1024u));
}
}

namespace lwjgl
{
namespace Display
{

void create()
{
	if (g_created)
		return;

	dsiEnsureEarlyVideo();
	g_created = true;
}

void setDisplayMode(const DisplayMode&)
{
	// The screen resolution is fixed. See the header comment.
}

DisplayMode getDisplayMode()
{
	return DisplayMode(getWidth(), getHeight());
}

void setTitle(const jstring&) {}
void setFullscreen(bool) {}

bool isCloseRequested() { return g_closeRequested; }
bool isVisible() { return true; }
bool isActive() { return true; }

// START no longer requests a close -- see the header comment above. Nothing
// on this console requests one yet (there is no real menu/HOME-button
// affordance the way Wii has), so g_closeRequested now only exists so
// isCloseRequested() has a well-defined, always-false answer; the game loop
// keeps running until the console is powered off.
void processMessages()
{
	scanKeys();
	dsiUpdateTouchCameraDelta();

	// Same "ask the game directly" approach Display_wii.cpp's processMessages()
	// already uses (see its own comment): the input layer cannot infer
	// whether a GuiScreen is open, and getting it wrong either feeds the menu
	// pointer gameplay camera-drag touches or feeds gameplay a stray menu
	// click. Computed once and shared: dsiPushGameplayKeyEvents() also needs
	// it now, to know whether L/R currently mean world place/break or a
	// container screen's place-one/drop actions (see its own comment).
	Minecraft *mc = Minecraft::getMinecraft();
	const bool inMenu = (mc != nullptr && mc->currentScreen != nullptr);
	dsiPushGameplayKeyEvents(inMenu);
	dsiUpdateMenuPointer(inMenu);
}

void swapBuffers()
{
	// Real-hardware evidence this addresses: a screenshot showed the top
	// screen split cleanly in two -- one half correctly textured (tree bark,
	// the wood-plank hotbar icon's grain all visible), the other half flat,
	// untextured colour -- getting worse the longer a session ran, tracking
	// the same window where committed heap/texture VRAM pressure keeps
	// climbing (see Minecraft.cpp's memtrend diagnostic). glFlush(0) is
	// documented as "waits for vblank and swaps", which the comment this one
	// replaces (and DsiBringup.cpp's matching one) took at face value -- but
	// libnds's actual implementation is `GFX_FLUSH = mode;`, a plain register
	// write with a memory barrier, not a blocking wait. It tells the GPU
	// where this frame's polygon list ends and to swap at the NEXT vblank;
	// it does not stop the CPU from immediately going on to build and submit
	// the FOLLOWING frame's polygon list. With frame times measured at
	// 100-400ms+ under load (dsi.perf's own render= figures), vastly longer
	// than one ~16.7ms vblank period, nothing in this loop stopped the CPU
	// from getting several logical frames ahead of whatever the GPU had
	// actually finished rasterizing and displayed -- a real hardware/software
	// race between CPU-side texture VRAM reuse (evicting a texture that
	// in-flight, already-submitted-but-not-yet-rasterized polygons from an
	// earlier frame may still be sampling) and the GPU's own progress,
	// exactly the kind of bug that would show as "part of this frame drew
	// correctly, part did not" and get worse as VRAM turnover increases with
	// playtime. swiWaitForVBlank() is libnds's actual blocking wait (used
	// correctly elsewhere, e.g. DsiBringup.cpp's own colour-cycle demo loop)
	// -- added here to genuinely pace the CPU to one submitted frame per
	// display refresh instead of assuming glFlush() already did that.
	glFlush(0);
	swiWaitForVBlank();
	heartbeat();
}

void update(bool doProcessMessages)
{
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

int_t getX() { return 0; }
int_t getY() { return 0; }
int_t getWidth() { return 256; }
int_t getHeight() { return 192; }

} // namespace Display
} // namespace lwjgl

#endif // DSI_PLATFORM
