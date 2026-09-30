#include "platform/ClientPlatformPolicy.h"

#ifdef DSI_PLATFORM

#include "platform/Log.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/RenderEngine.h"
#include "net/minecraft/src/legacy/LegacyUiPolicy.h"
#include "dsi/DsiEarlyInit.h"

namespace ClientPlatformPolicy
{
int initialWidth()
{
	return 256; // DS/DSi screen resolution -- see DsiEarlyVideo.cpp's glViewport().
}

int initialHeight()
{
	return 192;
}

std::string minecraftDirectory()
{
	return dsiGetSaveDir();
}

bool saveConverterUsesSavesSubdirectory()
{
	return true;
}

// Same simplifications PS2 applies for the same reason: an equally
// memory/GPU-constrained fixed-function 3D engine gains nothing from
// animating water/lava/fire/portal/redstone/explosion/flame/smoke textures,
// and neither "advanced" GL features nor fancy occlusion exist on this
// hardware to enable.
void applyGameSettingsDefaults(GameSettings* settings)
{
	if (settings == nullptr)
		return;

	settings->ofAnimatedWater = 2;
	settings->ofAnimatedLava = 2;
	settings->ofAnimatedFire = false;
	settings->ofAnimatedPortal = false;
	settings->ofAnimatedRedstone = false;
	settings->ofAnimatedExplosion = false;
	settings->ofAnimatedFlame = false;
	settings->ofAnimatedSmoke = false;
	settings->advancedOpengl = false;
	settings->ofOcclusionFancy = false;
}

// Left as a no-op for now rather than guessed at: preloading/releasing
// specific textures is a real-hardware memory-pressure optimisation (see the
// PS2/Wii comments on their equivalents), and nothing has been measured yet
// on this backend to say it is needed -- see DsiEarlyMemory.cpp for the
// running theme of "measure before tuning" on this port.
void preloadStartupTextures(RenderEngine*)
{
}

// Now measured, not guessed: RenderEngine.cpp's DSi-only upload-failure
// warning (added to chase the menu perf bug) came back naming /terrain.png
// and /gui/items.png -- BOTH already exactly 256x256, a perfectly valid
// power-of-two size -- as failing to upload once a world was entered, with
// render time then dominated by the resulting every-bind retry (RenderAPI_
// DSI.cpp's uploadTexture() returning 0 covers both "not power-of-two" AND
// "the DS's texture VRAM banks are full", and a correctly-sized texture
// failing only leaves the latter). DsiEarlyVideo.cpp already dedicates all
// four texture-capable banks (A-D, 128KB each = 512KB total, the hardware
// maximum for texture data on this GPU) to textures; a 256x256 texture in
// the DS's native 16-bit format is itself 128KB, a whole bank on its own,
// and the main menu's own textures (the legacy panorama plus the title
// banner, both resized to fit within 256x256 by LegacyPanoramaUpload.cpp)
// are exactly the kind of thing PS2's equivalent below already found worth
// releasing early for the same reason on the same constrained-VRAM problem.
// getTexture() reloads either lazily if the main menu is shown again later.
void releaseWorldEntryAssets(RenderEngine* renderEngine)
{
	if (renderEngine == nullptr)
		return;

	renderEngine->releaseTexture("/legacy/panorama.png");
	renderEngine->releaseTexture(legacyUiTitleResourcePath());
	// StartupPresentation.cpp's playLegacyLogo() binds these once each, at
	// boot, and nothing ever draws them again after that -- but nothing ever
	// released them either, so they sat resident in VRAM for the rest of the
	// run. Real-hardware data (the next test after the panorama/title release
	// above) showed gui/items.png -- confirmed exactly 256x256, a valid
	// power-of-two size -- still failing to upload once in a world, which
	// with a valid size can only mean the four texture VRAM banks were still
	// full (see the comment above this function). These two splash images
	// are exactly the same class of "menu-only, safe to drop once gameplay
	// starts" asset as panorama/title, just missed the first time.
	renderEngine->releaseTexture("/legacy/logo1.png");
	renderEngine->releaseTexture("/legacy/logo2.png");
	// Real-hardware evidence (debug.log's resident-texture dump, taken mid-
	// gameplay after a /gui/inventory.png upload failure): these three menu-
	// only legacy UI widgets (the options-list scrollbar arrow and the
	// legacy-style checkbox on/off graphics) were STILL resident well after
	// entering the world, alongside panorama/title/logo1/logo2 which this
	// function already correctly drops. Small individually (~5KB combined),
	// but the 512KB budget is tight enough post-terrain.png-high-precision
	// that every KB matters for whether gui.png/mob skins/the hand texture
	// stay resident without an eviction-and-retry cycle -- the most likely
	// explanation for the reported brief white flash on the hotbar, hand and
	// mob models real hardware now shows with lighting off (so not caused by
	// the lightmap work). Same lazy-reload-if-a-menu-needs-them-again pattern
	// as the four releases above; legacy/tick.png is the on-state graphic
	// (tickbox.png/tickbox_hovered.png are the off-state boxes).
	renderEngine->releaseTexture("/legacy/scroll_down.png");
	renderEngine->releaseTexture("/legacy/tick.png");
	renderEngine->releaseTexture("/legacy/tickbox.png");
	renderEngine->releaseTexture("/legacy/tickbox_hovered.png");
	// Real-hardware evidence (reported: hundreds of repeated "GPU out of
	// texture VRAM space" / "falling back to paletted" / "colour
	// quantization" warnings throughout an entire play session, plus a
	// performance regression): this function only ever released the LEGACY
	// menu's own textures (/legacy/*). When Legacy UI is off, GuiMainMenu's
	// non-legacy menu instead loads six /title/bg/panorama0.png..5.png
	// cubemap faces plus /title/mclogo.png -- ClientPlatformPolicy_PS2.cpp's
	// own releaseWorldEntryAssets() already releases these exact paths for
	// the identical reason (PS2 supports both menu styles too), but this
	// DSi copy never picked that up. A player who visits the non-legacy menu
	// even once before entering a world leaves all seven of those textures
	// resident for the rest of the session, permanently eating into the
	// hard 512 KB texture VRAM ceiling gameplay's own terrain/items/icons/
	// mob-skin textures compete for -- exactly the kind of chronic pressure
	// that turns every later texture bind into a failed-upload retry.
	for (int face = 0; face < 6; ++face)
		renderEngine->releaseTexture("/title/bg/panorama" + std::to_string(face) + ".png");
	renderEngine->releaseTexture("/title/mclogo.png");
	renderEngine->clearDecodedTextureCache();
}

// Real-hardware evidence (reported: hotbar/HUD elements and other GUI
// textures intermittently missing or falling back to a checkerboard while
// exploring, worst right after heavy chunk loading -- and, separately, the
// same "GPU out of texture VRAM space" spam that motivated the menu-texture
// additions above): releaseWorldEntryAssets() frees menu-only textures
// before a world loads, but nothing on this platform ever freed the WORLD's
// own textures when leaving it. terrain.png/gui/items.png/gui/icons.png/
// mob skins/gui/inventory.png all stayed resident indefinitely once loaded,
// so returning to the menu (legacy or not) had to fit menu textures into
// whatever fraction of the 512 KB budget gameplay's textures had not
// already claimed -- and a second play session compounded it further, since
// nothing ever gave that space back. getTexture() reloads any of these
// lazily the next time something actually needs them (a new world, or the
// inventory screen being reopened), matching the same lazy-reload pattern
// releaseWorldEntryAssets() already relies on for menu textures.
void releaseWorldExitAssets(RenderEngine* renderEngine)
{
	if (renderEngine == nullptr)
		return;

	renderEngine->releaseTexture("/terrain.png");
	renderEngine->releaseTexture("/gui/items.png");
	renderEngine->releaseTexture("/gui/icons.png");
	renderEngine->releaseTexture("/gui/inventory.png");
	// "/gui/gui.png" (HUD icons, GuiIngame.cpp, bound every HUD frame) is
	// exactly as gameplay-only as the three above, but was missing from this
	// list.
	renderEngine->releaseTexture("/gui/gui.png");
	// Sky/weather textures: clouds render whenever clouds are enabled (i.e.
	// session-scoped, same as terrain.png), and rain/snow only bind while
	// precipitation is actually rendering, but neither had a release path at
	// all -- add them here rather than attempting mid-session eviction for
	// weather, which recurs often enough during ordinary exploration that
	// the reload churn could cost more than it saves.
	renderEngine->releaseTexture("/environment/clouds.png");
	renderEngine->releaseTexture("/environment/rain.png");
	renderEngine->releaseTexture("/environment/snow.png");
	// Was just "/mob/char.png" (the player skin) -- real-hardware evidence
	// (a debug.log showing texture VRAM pinned at 501/512KB for the rest of
	// a session the moment several mob types had been seen, sourcing an
	// 862ms palette-quantization stall) confirmed every OTHER /mob/*.png
	// (zombie, pig, cow, skeleton, creeper, wolf, villager/*, ...) a session
	// happened to load was never released here at all. See RenderEngine::
	// releaseTexturesWithPrefix()'s own comment for why a prefix match
	// replaces the old single hardcoded name instead of listing all ~40.
	renderEngine->releaseTexturesWithPrefix("/mob/");
	// Same gap, same fix, for the OTHER family of entity/tile-entity textures
	// that go through the same CachedTextureId pattern: chests, signs, the
	// enchanting table's book, minecarts, boats, arrows, XP orbs
	// (/item/chest.png, /item/largechest.png, /item/sign.png, /item/book.png,
	// /item/cart.png, /item/boat.png, /item/arrows.png, /item/xporb.png).
	// These had NO release path at all -- not even this one, until now -- so
	// the first chest/sign/cart/etc. a session ever saw permanently reserved
	// its VRAM slot (and its RAM-side RGBA mirror, see DsiTexture) for the
	// rest of the process's life, across world exits and new sessions alike.
	renderEngine->releaseTexturesWithPrefix("/item/");
	// Two more one-off entity textures with the same "never released" gap:
	// paintings (RenderPainting.cpp, "/art/kz.png") and the charged-creeper
	// lightning overlay (RenderCreeper.cpp, "/armor/power.png"). Both are
	// also covered by the mid-session eviction in Minecraft.cpp now; this is
	// the same belt-and-suspenders world-exit coverage the /mob/ and /item/
	// prefixes already get.
	renderEngine->releaseTexture("/art/kz.png");
	renderEngine->releaseTexture("/armor/power.png");
	renderEngine->clearDecodedTextureCache();
}

int panoramaSampleGrid()
{
	// 1: a single, unblurred draw per cube face (6 quads/frame total), not
	// PS2's already-reduced 2x2 grid (24 quads/frame). Requested directly
	// after a real-hardware report of severe main-menu lag (menu barely
	// responding to input) -- the sampleGrid loop in GuiMainMenu.cpp's
	// drawPanorama() is a deliberate motion-blur-style effect: N*N samples
	// per face, each a separate alpha-blended draw, stacked to fake a soft
	// trail as the cube rotates. Translucent-polygon rendering is one of
	// the more expensive, easier-to-misconfigure paths on the DS's fixed-
	// function GPU (see RenderAPI_DSI.cpp's own POLY_ALPHA/blending notes),
	// so 24 blended draws a frame just for the background -- before the
	// menu buttons, text, or anything else -- is a real, plausible cost on
	// this hardware, not just PS2's GS. Dropping to a single opaque draw
	// per face removes both the blend stacking AND 18 of the 24 draws.
	return 1;
}

void reportCrash(const std::string& description)
{
	MC_LOG_ERROR("crash", "%s\n", description.c_str());
}
}

#endif // DSI_PLATFORM
