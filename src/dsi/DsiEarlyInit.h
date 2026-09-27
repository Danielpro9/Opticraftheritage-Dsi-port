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

// Reads the touch screen and updates the per-frame stylus-drag delta
// InputBackend_DSI.cpp's platformGamepadSnapshot() reports as the camera's
// "right stick". Must run exactly once per frame -- Display_dsi.cpp's
// processMessages() is the one call site, same place scanKeys() already runs
// once per frame for the same reason (see that function's own comment).
// Calling this more than once a frame, or from platformGamepadSnapshot()
// itself, would compute the delta against a position already moved past by
// an earlier call the same frame, undercounting fast drags.
void dsiUpdateTouchCameraDelta();

// Turns the L/R/A/X/Y/hotbar-chord action buttons into the same
// lwjgl::Keyboard/Mouse events a real keyboard/mouse press would queue (see
// InputBackend_DSI.cpp's header comment for the full button scheme). Must
// run once a frame, same place and for the same reason as
// dsiUpdateTouchCameraDelta() above.
void dsiPushGameplayKeyEvents();

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
