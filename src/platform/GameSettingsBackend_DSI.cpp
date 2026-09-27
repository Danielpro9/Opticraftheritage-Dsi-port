#include "platform/GameSettingsBackend.h"

#include <algorithm>
#include "net/minecraft/src/GameSettings.h"
#include "platform/PlatformTuning.h"

#ifdef DSI_PLATFORM

// Key bindings are left at their built-in defaults: DSi input/controls are
// explicitly deferred (see InputBackend_DSI.cpp), so there is no DSi key-code
// scheme yet to assign here the way PS2's does with PS2_KEY_DPAD_UP etc.
void platformGameSettingsInitialize(GameSettings&)
{
}

void platformGameSettingsResetControlBindings(GameSettings&)
{
}

int_t platformGameSettingsDefaultChunkUpdates() { return (int_t)PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME; }
// Connected textures blend across an atlas seam using neighbouring block
// state, which costs extra texture lookups per face; off by default given
// the same texture-memory/GPU-time budget reasoning as the rest of this
// port's tuning.
//
// FIXED: this returned 0, not 3. Config::isConnectedTextures() (Config.cpp)
// checks ofConnectedTextures != 3 -- 1=Fast and 2=Fancy are the only "on"
// values, matching GameSettings.cpp's own cycle-through-options logic
// (ofConnectedTextures++, wrapping 3 back to 1) and its options-string
// display (GameSettings.cpp: only 1 and 2 print a name, everything else,
// 0 included, prints "OFF") -- so the menu already correctly displayed
// "Connected Textures: OFF" on a fresh DSi settings object while 0 != 3
// made Config::isConnectedTextures() itself return true, the opposite of
// what was displayed and clearly the opposite of this comment's own stated
// intent. This setting is never read from or written to options.txt
// (grep confirms ofConnectedTextures is only ever assigned here at
// construction and in GameSettings.cpp's own toggle handler), so unlike a
// persisted setting, correcting this default is a complete fix on its own
// -- no stale options.txt value can keep reproducing the bug.
//
// Real consequence beyond the extra per-face ConnectedTextures::
// getConnectedTexture() lookup RenderBlocks.cpp's resolveFaceTexture()
// pays for every block face regardless (cheap on its own when a resource
// pack has no CTM *.properties files to match, as OptiCraft's bundled ones
// do not): WorldRendererDsi.cpp's greedy-mesh phase and DsiWaterMerge.cpp's
// still-water merge both gate themselves on
// !Config::isConnectedTextures() (mirroring PS2's own identical
// allowOptiFineGreedyMesh guard, since a merged run's single FaceKey/tile
// cannot represent connected-texture's per-neighbour texture variation) --
// with isConnectedTextures() incorrectly true, both were silently
// disabled entirely on every DSi build up to and including this one.
int_t platformGameSettingsDefaultConnectedTextures() { return 3; }
int_t platformGameSettingsCycleRenderDistance(int_t, int_t) { return PLATFORM_DEFAULT_RENDER_DISTANCE; }
int_t platformGameSettingsClampRenderDistance(int_t) { return PLATFORM_DEFAULT_RENDER_DISTANCE; }

// PS2's equivalent clamps to [32, radius*16], which only works because its
// radius (2) makes both ends of that range the same value (32). DSi's radius
// (1, see DsiWorldTuning.h) would make that an inverted, nonsensical range
// (min 32 > max 16), so this returns the fixed distance outright instead of
// clamping towards it -- render distance is not fine-adjustable on DSi any
// more than it is on PS2, just for a different-looking reason in the code.
int_t platformGameSettingsClampFineRenderDistance(int_t)
{
	return PLATFORM_VISIBLE_CHUNK_RADIUS * 16;
}

void platformGameSettingsUpdateRenderDistanceFromFine(int_t, int_t&) {}

// Anaglyph (red/cyan) 3D needs a double render pass with per-channel colour
// masking that RenderAPI_DSI.cpp does not implement; force it off rather than
// accept a request nothing will act on.
bool platformGameSettingsAnaglyphValue(bool, bool) { return false; }

bool platformGameSettingsLoadOption(GameSettings&, const std::string&, const std::string&) { return false; }

void platformGameSettingsFinalizeLoad(GameSettings& settings)
{
	settings.ofChunkUpdates = std::max(settings.ofChunkUpdates, (int_t)PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME);
}

void platformGameSettingsSyncControllerBindings(const GameSettings&) {}
void platformGameSettingsAddKnownKeys(std::unordered_set<std::string>&) {}
void platformGameSettingsWriteOptions(const GameSettings&, std::ostream&) {}

#endif // DSI_PLATFORM
