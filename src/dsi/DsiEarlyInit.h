#pragma once
#ifdef DSI_PLATFORM

#include <cstddef>
#include <nds/ndstypes.h>

// Brings up video (top screen 3D via the libnds GL wrapper, bottom screen flat
// black), at most once. Safe to call again. See DsiEarlyVideo.cpp for what
// "flat black" means in terms of VRAM banks and display power.
void dsiEnsureEarlyVideo();

// Upper bound of the malloc heap right now, in bytes. Built against
// dsi_arm9.specs (see src/dsi/Makefile) and launched DSi-enhanced, this is the
// enforced 12 MB budget from DsiEarlyMemory.cpp (reduceHeapSize() caps it there
// even though the DSi genuinely has 16 MB -- see that file for why it is 12 and
// not the full 16). Built/launched as plain NDS-compatible instead, it is the
// natural ~3.5-4 MB ceiling.
u32 dsiGetHeapCeiling();

// Bytes currently committed to the malloc heap (getHeapEnd() - getHeapStart()).
u32 dsiGetHeapCommitted();

// Mounts the DSi's SD card and creates the save directory, at most once. Safe
// and cheap to call again; the first call does the work. Returns whether the
// card mounted and the save directory exists.
bool dsiEnsureStorage();

// Where world saves (and therefore streamed-out chunk region files -- see
// DsiWorldTuning.h) live: "sd:/OptiCraft". Always returns a usable path, even
// when nothing mounted, so callers get a path that fails to open rather than a
// null pointer to check. Calls dsiEnsureStorage() for you.
const char* dsiGetSaveDir();

// Reads the touch screen and, while a drag is in progress and no menu is
// open, pushes this frame's raw pixel delta into lwjgl::Mouse (the same
// pushMotion() dsiUpdateMenuPointer() below uses for the menu cursor) so
// EntityRenderer.cpp's ordinary PC-style mouse-look path drives the camera
// from it -- see InputBackend_DSI.cpp's own comment on this function for why
// a direct pixel delta replaced the earlier fixed-anchor "virtual stick"
// scheme (real-hardware feedback: holding a small offset from an anchor
// point turned the camera at a slow, laggy rate instead of tracking the
// finger the way a drag naturally suggests it should -- ClassiCube's own DS
// port turned out to use exactly this direct-delta model too, once the
// search widened from Window_NDS.c to the generic Camera.c/Input.c layer
// its touch events actually feed).
//
// inMenu (computed the same way dsiPushGameplayKeyEvents()'s own caller
// does, see Display_dsi.cpp) suppresses this while a screen is open, so a
// menu drag does not also feed the gameplay camera through the same
// lwjgl::Mouse motion staging dsiUpdateMenuPointer() is using that frame for
// the cursor.
//
// Must run exactly once per frame -- Display_dsi.cpp's processMessages() is
// the one call site, same place scanKeys() already runs once per frame for
// the same reason (see that function's own comment). Calling this more than
// once a frame, or from platformGamepadSnapshot() itself, would push the
// same touch sample's delta twice.
void dsiUpdateTouchCameraDelta(bool inMenu);

// Turns the L/R/A/X/Y/hotbar-chord action buttons into the same
// lwjgl::Keyboard/Mouse events a real keyboard/mouse press would queue (see
// InputBackend_DSI.cpp's header comment for the full button scheme). Must
// run once a frame, same place and for the same reason as
// dsiUpdateTouchCameraDelta() above. inMenu (computed the same way
// dsiUpdateMenuPointer()'s own caller does, see Display_dsi.cpp) suppresses
// L/R's world place/break synthesis specifically while a screen is open, so
// they can double as ContainerSlotNavigator.cpp's dedicated place-one/drop
// container actions instead without the two meanings firing at once.
void dsiPushGameplayKeyEvents(bool inMenu);

// Feeds the touch screen into lwjgl::Mouse as an absolute-position pointer
// while a GuiScreen is open (inMenu), the same role the Wiimote IR pointer
// plays on Wii (see WiiPointer.cpp) -- touching the bottom screen moves a
// cursor drawn on the top screen, since DSi's GUI canvas is the same native
// 256x192 as the touch screen's own pixel space (DsiPresentationTuning.h),
// no scaling needed.
//
// Tap-to-click, not click-on-touch: dragging across the screen only
// repositions the cursor. The click itself fires on release (at wherever
// the finger lifted), or on an A press while the pointer owns input (at
// the cursor's current position) -- real-hardware request, so lining the
// cursor up over something doesn't activate it before the player meant to.
//
// Also owns the pointer-vs-D-pad handoff platformMenuPointerActive()
// reports: touching claims pointer ownership and it stays claimed after
// release (the cursor keeps showing at the last touched position, not
// wherever a stale touchRead() sample after release would land -- another
// real-hardware request), until a D-pad direction or B hands it back to
// the pad, mirroring Wii's "aim to use the cursor, press a direction to go
// back to navigation" behaviour (see GuiScreen.cpp's
// menuPointerInputSuppressed() comment). A does not hand ownership back --
// it is a pointer action in its own right, not navigation.
//
// Call with inMenu=false does nothing but reset that ownership state, so a
// fresh GuiScreen never inherits a previous one's pointer/pad ownership.
// Must run once a frame, same place and for the same reason as
// dsiUpdateTouchCameraDelta() above -- Display_dsi.cpp's processMessages()
// is the one call site.
void dsiUpdateMenuPointer(bool inMenu);

// Bytes of the 512 KB texture-image VRAM budget (four 128 KB banks, see
// DsiEarlyVideo.cpp) currently spoken for by successfully-uploaded textures.
// Defined in RenderAPI_DSI.cpp; RenderEngine.cpp calls this from its DSi-only
// upload-failure warning so that log line reports the real budget state
// instead of just the one texture that failed -- see that call site's own
// comment for the real-hardware case (a valid power-of-two texture failing
// to upload, which can only mean the banks were already full) that made a
// guess not good enough here.
std::size_t dsiTotalTextureVramBytes();

// Same cost query, for one already-resolved GL texture id. 0 if `name` holds
// nothing right now. See dsiTotalTextureVramBytes() above for the budget this
// is measured against.
std::size_t dsiTextureVramBytes(int name);

// Cumulative count of every WorldRenderer::dsiBuildRendererStep() call across
// the whole process that finished a section's build and published it (the
// chunksUpdated++ right before dsiResetBuildState() at the end of that
// function -- see WorldRendererDsi.cpp). One real completed rebuild, not one
// incremental step. Surfaced on Minecraft.cpp's memtrend line so a report of
// "performance is bad even standing still, nothing streaming in" can show
// whether this keeps climbing while getLoadedChunkCount() stays flat --
// which would mean the same already-loaded chunk(s) are being rebuilt over
// and over rather than replayed cheaply, instead of guessing at the cause.
unsigned int dsiGetTotalRendererRebuilds();

// Cumulative count of every time WorldRenderer::markDirty() discarded an
// ACTIVE build's progress (WorldRenderer.cpp's DSI branch: dsiBuildActive
// true and isChunkPopulationPendingForRendering() false, so it calls
// dsiResetBuildState() instead of coalescing) -- an ordinary gameplay tick
// (flowing water/lava, a growing crop, redstone, any markDirty() while a
// section is still incrementally building) landing mid-build, not a
// section being recycled/torn down (dsiResetBuildState() has other,
// unrelated callers this does NOT count -- see its own call sites). Only
// ever increments, never reflects a completed build (dsiGetTotalRenderer
// Rebuilds() above is that counter). Exists to test a specific hypothesis:
// that this coalescing gap (present, identically, in PS2's and Wii's own
// markDirty() branches too -- not a DSi-only oversight) costs disproportion-
// ately more on DSi's much weaker ARM9 every time it fires, since each
// restart re-does a section's entire scanned-so-far build cost from zero.
// Surfaced on Minecraft.cpp's memtrend line next to rebuilds=: if this
// climbs quickly while rebuilds= stays flat/low, sections are being
// interrupted and restarted far more than they ever finish -- the
// mechanism a real-hardware test needs to confirm or rule out before
// deciding whether to loosen this coalescing condition for DSi specifically
// (PS2/Wii apparently don't need it, likely because their faster CPUs make
// a wasted restart cheap enough not to matter).
unsigned int dsiGetTotalBuildRestarts();

// Called only from WorldRenderer.cpp's markDirty() DSI branch, the one call
// site this counter measures -- see dsiGetTotalBuildRestarts()'s own
// comment just above for what "a restart" means here and why.
void dsiRecordBuildRestart();

#endif // DSI_PLATFORM
