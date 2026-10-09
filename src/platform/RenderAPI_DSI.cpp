#ifdef DSI_PLATFORM

#include "platform/RenderAPI.h"
#include "platform/Log.h"
#include "platform/PlatformCompat.h"
#include "dsi/minecraft/DsiCapturedMeshRepack.h"
#include "dsi/DsiEarlyInit.h"

#include <nds.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
// DSi RenderAPI backend -- maps the shared GL-shaped platform/RenderAPI.h
// surface onto libnds's videoGL wrapper around the DS 3D engine.
// -----------------------------------------------------------------------------
// Confidence varies a lot across this file, and the comment on each section
// says which of these it is:
//
//   VERIFIED    -- matches a libnds function/macro read directly from
//                   blocksds/libnds source (nds/arm9/videoGL.h, nds/arm9/
//                   video.h). High confidence, but still UNTESTED: nothing in
//                   this port has run on real hardware/an emulator yet (see
//                   DsiEarlyMemory.cpp/DsiBringup.cpp for why -- this sandbox
//                   cannot reach the BlocksDS toolchain servers, only GitHub
//                   Actions can, and this file hasn't been through that build
//                   yet at the time it was written).
//   APPROXIMATED -- the DS 3D engine genuinely cannot do what desktop
//                   OpenGL/GX/GS do here (no generalised glBlendFunc, no
//                   multitexturing, a table-based fog model instead of a
//                   parametric one, a very different lighting model), so this
//                   is a deliberate, commented substitute, not an oversight.
//   ASSUMED      -- a specific behavior (like the lightmap's UV-to-index
//                   convention) inferred from how the shared engine calls
//                   this API rather than confirmed against its source line by
//                   line. Flagged so it's the first thing to check against a
//                   real screenshot once this can run.
//
// Scope cut relative to RenderAPI.h: the display-list/occlusion-query group
// (PLATFORM_PC only), the persistent-mesh group (PLATFORM_PERSISTENT_RENDER_
// MESH || PLATFORM_MODEL_PERSISTENT_MESH, both 0 for DSi -- see PlatformConfig.h),
// the native-terrain-pipeline function (PLATFORM_NATIVE_TERRAIN_PIPELINE,
// Wii-only) and renderApplyTextureQuality/renderReadPixelsRgb (both gated off
// for DSi too) are all compiled out by RenderAPI.h's own #if guards, so none
// of them are implemented here -- DSi uses the same captured/immediate mesh
// replay path PS2 uses instead of native persistent geometry.

namespace
{

// -----------------------------------------------------------------------------
// DMA wait diagnostic -- added to answer a real question before committing to
// a much riskier change, not a permanent feature.
// -----------------------------------------------------------------------------
// drawInterleavedMeshFast() and drawCapturedMeshFast() (further down this
// file) each kick one GX-FIFO DMA transfer and then busy-wait for it to
// finish before returning -- the CPU does nothing useful for however long
// that transfer takes, every single call. The naive fix (just stop waiting)
// was investigated and rejected: BlocksDS's own libnds glCallList() -- the
// function this file's own comments say drawCapturedMeshFast() mirrors --
// keeps the IDENTICAL busy-wait after kicking its own DMA, despite calling
// the transfer "asynchronous" in its comment, and separately documents "a
// hardware bug... when there are multiple DMA channels active" as the reason
// for the OTHER wait (the one before the kick). That is strong evidence the
// wait is load-bearing for a real hardware reason, not just caution worth
// second-guessing from a sandbox with no real DSi to test against. A safe
// version would have to overlap the wait with other CPU work instead of
// removing it outright -- a much larger restructuring, not worth attempting
// blind. This measures the actual cost first, with zero behaviour change:
// both draw functions already bracket their own wait loops; this just also
// records how long each one took, and reports a 20-frame-averaged line
// (same shape as Profiler_DSI.cpp's own renderphase line) so the next real-
// hardware debug.log gives a real number to decide with instead of a guess.
long long g_dsiDmaWaitSumUs = 0;
long long g_dsiDmaWaitMaxUs = 0;
int g_dsiDmaWaitFrames = 0;

void dsiRecordDmaWait(long long elapsedUs)
{
	g_dsiDmaWaitSumUs += elapsedUs;
	if (elapsedUs > g_dsiDmaWaitMaxUs)
		g_dsiDmaWaitMaxUs = elapsedUs;
}

// Called once per frame from renderSubmitFrame() below.
void dsiReportDmaWaitIfDue()
{
	if (++g_dsiDmaWaitFrames < 20)
		return;
	MC_LOG_INFO("dsi.perf", "dmawait avgUsPerFrame=%ld maxSingleCallUs=%ld n=%d\n",
		(long)(g_dsiDmaWaitSumUs / g_dsiDmaWaitFrames), (long)g_dsiDmaWaitMaxUs, g_dsiDmaWaitFrames);
	g_dsiDmaWaitSumUs = 0;
	g_dsiDmaWaitMaxUs = 0;
	g_dsiDmaWaitFrames = 0;
}

// -----------------------------------------------------------------------------
// Poly format shadow state -- VERIFIED mechanism, APPROXIMATED mapping
// -----------------------------------------------------------------------------
// Unlike desktop GL/GX, the DS 3D engine has no per-feature enable toggles for
// culling, lighting or shading: they are all bits of one word set together by
// glPolyFmt(). Every setter below that touches one of these fields just
// updates the shadow and marks it dirty; applyPolyFormat() re-issues glPolyFmt
// lazily, right before the next draw call, so a burst of state changes costs
// one glPolyFmt() instead of one per call.
struct PolyFormatState
{
	RenderFace cullFace = RenderFace::Back;
	bool cullEnabled = false; // DS default: nothing is culled.
	bool light0 = false;
	bool light1 = false;
	RenderShadeModel shade = RenderShadeModel::Smooth;
	std::uint8_t alpha31 = 31; // POLY_ALPHA is 5 bits (0-31); 31 = opaque.
	bool fogEnabled = false;
	// APPROXIMATED: DS opaque polygons always write depth; there is no
	// general glDepthMask() equivalent. POLY_TRANS_SET_DEPTH only affects
	// TRANSLUCENT polygons (POLY_ALPHA < 31) writing depth, which is the
	// closest available control and matches Minecraft's actual use of
	// depth-mask-off (translucent water/glass/GUI overlays).
	bool depthMaskEnabled = true;
	// POLY_DEPTH_TEST_EQUAL vs. the hardware default POLY_DEPTH_TEST_LESS
	// (bit value 0 -- see renderDepthFunc() below for the real-hardware
	// evidence this corrects). false = hardware default (strictly less).
	bool depthTestEqual = false;
	bool dirty = true;
};

PolyFormatState g_poly;

void applyPolyFormatIfDirty()
{
	if (!g_poly.dirty)
		return;
	g_poly.dirty = false;

	u32 bits = POLY_ALPHA(g_poly.alpha31);
	// g_poly.shade is intentionally NOT translated into POLY_DECAL/POLY_MODULATION
	// here (an earlier version of this function did, believing POLY_DECAL was
	// "the closest DS shading mode to no interpolation"). That mapping was wrong
	// on two counts, and real-hardware evidence (reported: solid black boxes
	// behind every menu/dialog text glyph, and a general loss of PNG
	// transparency across the game) traced back to it:
	//   1. POLY_DECAL/POLY_MODULATION (POLYGON_ATTR bits 4-5, libnds's own
	//      comment literally calls the enum "shading" but it is the texture
	//      COMBINER mode -- how texture colour and vertex colour combine --
	//      not a Gouraud/flat vertex-colour-interpolation toggle. The DS 3D
	//      engine has no such toggle: vertex colours are always hardware-
	//      interpolated across a polygon, so there is nothing here for
	//      RenderShadeModel::Flat to correctly map onto.
	//   2. Decal mode's actual effect on a texture's "transparent" texels
	//      (alpha 0, e.g. GL_TEXTURE_COLOR0_TRANSPARENT's index 0, used by
	//      every paletted texture including font/default.png and font/
	//      alternate.png) is the opposite of transparent: in Decal mode, a
	//      texel with TexAlpha == 0 outputs FinalColor = VertexColor and
	//      FinalAlpha = VertexAlpha (the polygon's OWN alpha, opaque for
	//      ordinary text/UI draws) instead of Modulation mode's FinalAlpha =
	//      VertexAlpha * TexAlpha == 0. So every "transparent" pixel of a
	//      glyph's quad rendered fully OPAQUE, painted in whatever colour the
	//      draw's vertex colour happened to be -- for FontRenderer's drop-
	//      shadow pass that colour is near-black, which is exactly the
	//      reported "solid black box behind every character" (offset by the
	//      shadow pass's +1,+1 px, sized to each glyph's own quad). Gui::
	//      drawGradientRect (the menu/dialog darken overlay almost every
	//      screen draws before its own text/widgets) leaves shade set to
	//      Flat as its own post-draw state restore, so this was live for
	//      most GUI text and, by the same mechanism, for every other
	//      textured draw made while RenderGlobal.cpp/EntityRenderer.cpp/
	//      RenderHelper.cpp/TileEntityRendererPiston.cpp's own Flat/Smooth
	//      toggling left shade on Flat -- matching the broader "all PNG
	//      textures lack transparency" report too. Leaving this bit
	//      permanently unset (always POLY_MODULATION, value 0, the correct
	//      default for every textured draw in this engine) fixes both;
	//      renderShadeModel() below stays a real state setter (other
	//      platforms need it) but g_poly.shade no longer feeds glPolyFmt().
	if (g_poly.light0)
		bits |= POLY_FORMAT_LIGHT0;
	if (g_poly.light1)
		bits |= POLY_FORMAT_LIGHT1;
	if (g_poly.fogEnabled)
		bits |= POLY_FOG;

	if (g_poly.cullEnabled)
		bits |= (g_poly.cullFace == RenderFace::Front) ? POLY_CULL_FRONT : POLY_CULL_BACK;
	else
		bits |= POLY_CULL_NONE;

	if (g_poly.depthMaskEnabled)
		bits |= POLY_TRANS_SET_DEPTH;

	if (g_poly.depthTestEqual)
		bits |= POLY_DEPTH_TEST_EQUAL;

	glPolyFmt(bits);
}

void markPolyDirty() { g_poly.dirty = true; }

// -----------------------------------------------------------------------------
// Texture registry -- VERIFIED upload mechanism (glTexImageNtr2D/GL_RGBA),
// APPROXIMATED sub-image support (see renderTextureSubImageRgba below).
// -----------------------------------------------------------------------------
// glTexImageNtr2D() always replaces a texture's data wholesale; there is no
// partial/sub-rectangle upload on this hardware. Minecraft's atlas
// stitching and animated/procedural textures (lava, fire, the colormap-tinted
// leaves/water icons, ItemRenderer's dynamic icons) rely on
// renderTextureSubImageRgba() patching a region of an already-uploaded
// texture, so each texture keeps its own RGBA8 CPU-side copy here and a
// sub-image write patches that copy and re-uploads the whole thing. This
// costs real RAM (width*height*4 bytes per texture, held for the life of the
// texture) -- worth watching in DsiEarlyMemory.cpp's committed-heap number
// once real textures are loading.
struct DsiTexture
{
	int width = 0;
	int height = 0;
	bool blur = false;
	bool clamp = false;
	bool allocated = false;
	bool paletted = false; // Set by uploadTexture(): which VRAM format `allocated` actually used.
	// Set by renderTextureBeginUpload()'s highPrecision argument (RenderEngine.cpp
	// requests this for terrain.png/gui/items.png -- see isTileAtlasResource()):
	// skip the paletted GL_RGB256 attempt entirely and go straight to GL_RGBA.
	// Those two atlases are large, multi-material, gradient-heavy sheets that
	// routinely exceed the format's 256-colour-per-texture ceiling and were
	// getting crushed to as few as ~60 shared colours for the whole atlas
	// (tryUploadPaletted()'s quantShift loop bottoming out at shift=3) --
	// real-hardware reports described this as "all textures look flat, no
	// detail". Costs 2 bytes/pixel instead of 1 (e.g. +64KB for a 256x256
	// atlas) out of the 512KB texture-image budget; not applied to every
	// texture, just these two, so mob skins/gui.png/particles.png (which
	// already fit the exact palette) are unaffected.
	bool forceHighPrecision = false;
	std::vector<std::uint8_t> rgba; // width*height*4, tightly packed RGBA8
	// Established once by a genuine full-image upload (renderTextureImageRgba)
	// and then REUSED, not rebuilt, by every later sub-image patch
	// (renderTextureSubImageRgba) -- see the long comment on
	// tryUploadWithStablePalette() below for why rebuilding from scratch on
	// every patch was the actual cause of the reported per-tile colour
	// flicker/cycling on terrain.png. stableQuantShift < 0 means "not
	// established yet, next upload must do a full (re)build".
	std::vector<std::uint16_t> stablePalette;
	int stableQuantShift = -1;

	// Real-hardware evidence (investigated this round: reported frame-time
	// spikes tracking chunk-heavy exploration): renderTextureSubImageRgba()
	// used to re-upload this texture's ENTIRE pixel data to the GPU on every
	// single call -- unavoidable on this hardware (glTexImageNtr2D() has no
	// partial upload; see that function's own comment), but
	// RenderEngine.cpp's updateDynamicTextures() calls it once per animated
	// TextureFX targeting this texture (lava/water/fire/portal can all patch
	// terrain.png in the same tick), each a full re-conversion (RGBA555 or
	// the palette-quantization scan) and DMA of up to 128KB, when at most one
	// upload is ever needed to show the tick's final combined state. This
	// flag defers that expensive re-upload: the cheap part (patching the CPU
	// shadow copy in tex.rgba) still happens immediately in
	// renderTextureSubImageRgba(), but the actual GPU upload is done once,
	// lazily, the next time this texture is actually bound for real drawing
	// (renderBindTexture() below) -- by then every patch this tick already
	// landed in the shadow copy, so one upload there shows the same final
	// pixels N separate uploads would have, for a fraction of the cost.
	bool pendingUploadFlush = false;
};

// How many bytes of the four 128 KB texture-image banks (DsiEarlyVideo.cpp)
// this slot's current upload actually holds: 0 if nothing is allocated, else
// 1 byte/pixel for the GL_RGB256 paletted path or 2 for GL_RGBA. Exists so a
// texture-upload failure (RenderEngine.cpp's DSi-only warning) can report how
// much of the 512 KB budget was already spoken for at that moment instead of
// just the one texture's own size -- real hardware already showed a valid
// power-of-two texture (gui/items.png, 256x256) failing this way, which only
// happens when the banks are full, and guessing what filled them wastes a
// round-trip to real hardware that a number in the log line does not.
std::size_t textureVramBytes(const DsiTexture& tex)
{
	if (!tex.allocated)
		return 0;
	return static_cast<std::size_t>(tex.width) * static_cast<std::size_t>(tex.height) * (tex.paletted ? 1u : 2u);
}

std::vector<DsiTexture> g_textures; // index 0 unused (0 means "no texture" in GL)
int g_boundTexture = 0;

// Same dedup shape as g_boundTexture above, for the three capabilities that
// go straight to glEnable()/glDisable() instead of through g_poly's dirty-
// flag batching. 209 call sites across the engine toggle these (GUI draws,
// item/particle rendering, translucent terrain passes), many consecutively
// in the same state -- e.g. a run of opaque quads all calling
// renderDisable(Blend) between each other for no reason. -1 means "unknown"
// so the first real call for each always goes through; 0/1 afterwards skip
// the hardware write when the state already matches.
int g_texture2DEnabled = -1;
int g_alphaTestEnabled = -1;
int g_blendEnabled = -1;
// Fog belongs in this group too: unlike CullFace/Light0/Light1 (pure g_poly
// flags, no direct GL call at all), Fog's renderEnable/renderDisable cases
// call glEnable(GL_FOG)/glDisable(GL_FOG) unconditionally on every call --
// the exact gap the three above were added to close, just missed for this
// one. RenderGlobal.cpp alone toggles it several times a frame (sky pass,
// terrain pass, back to GUI/HUD), on top of every GuiScreen/FontRenderer/
// GuiIngame call that disables it before drawing 2D -- the same "many
// consecutive calls already in the right state" pattern as Blend.
int g_fogEnabled = -1;

DsiTexture* textureSlot(int name)
{
	if (name <= 0)
		return nullptr;
	if (static_cast<std::size_t>(name) >= g_textures.size())
		g_textures.resize(name + 1);
	return &g_textures[name];
}

// RGBA8 -> DS GL_RGBA (15-bit direct colour, 1-bit alpha). Bit layout VERIFIED
// against nds/arm9/video.h's ARGB16() macro: bit15=alpha, bits0-4=R, 5-9=G,
// 10-14=B. The alpha channel only has one bit on this hardware, so anything at
// or above the halfway point becomes fully opaque and anything below becomes
// fully transparent -- there is no partial-coverage alpha in this texture
// format (translucency for a whole polygon is a separate mechanism, see
// renderColor4f/POLY_ALPHA below).
void convertRgba8ToDs(const std::uint8_t* src, std::uint16_t* dst, int pixelCount)
{
	for (int i = 0; i < pixelCount; ++i)
	{
		const std::uint8_t r = src[i * 4 + 0];
		const std::uint8_t g = src[i * 4 + 1];
		const std::uint8_t b = src[i * 4 + 2];
		const std::uint8_t a = src[i * 4 + 3];
		dst[i] = static_cast<std::uint16_t>(
			((a >= 128) ? 0x8000 : 0) |
			(r >> 3) |
			((g >> 3) << 5) |
			((b >> 3) << 10));
	}
}

// Paletted (GL_RGB256) upload -- HALF the VRAM cost of GL_RGBA (1 byte per
// pixel instead of 2), tried before it below. Real-hardware data this
// session traced the remaining main-menu lag and the complete loss of 3D
// world rendering to the same root cause: the DS 3D engine's texture image
// VRAM is a hard 512 KB ceiling (DsiEarlyVideo.cpp's four 128 KB banks,
// already the hardware maximum for this data), and terrain.png/gui/items.png/
// gui/icons.png -- all confirmed exactly 256x256, a valid power-of-two size,
// so the earlier power-of-two fixes do not apply here -- are 128 KB each in
// GL_RGBA. Those three alone are 384 KB, and once font/gui.png and whatever
// else a real scene needs are also resident, gameplay's own essential
// textures were failing to fit at all, silently falling back to the same
// "never cached, retried from scratch on every bind" path already fixed for
// the menu's decorative textures -- except every failed bind here is a real
// SD decode+upload attempt in the middle of drawing a frame, not once at
// menu idle.
//
// GL_RGB256 stores one 8-bit palette index per pixel instead of a 16-bit
// colour: half the memory for any texture with 255 or fewer distinct opaque
// colours, which describes most block/item/icon pixel art (a handful of
// shades per material) even if it does not describe a photo-sourced
// panorama. Index 0 is reserved for GL_TEXTURE_COLOR0_TRANSPARENT ("this
// index is fully transparent"), the same 1-bit alpha convertRgba8ToDs()
// already uses for GL_RGBA -- no loss of transparency capability, just a
// different place the one alpha bit lives. If the source image has more
// than 255 distinct opaque colours (fails partway through the scan below),
// this returns false and the caller falls through to the existing GL_RGBA
// path unchanged -- trying this first can only help, never break a texture
// that does not fit it.
//
// O(pixel count) time: one pass building a 32768-entry (all possible RGB555
// values) direct-lookup table of "which palette index is this colour",
// rather than a linear search of the growing palette per pixel. Runs once
// per texture load (and once per animated sub-image update, same as
// convertRgba8ToDs() already does for every upload today -- not a new cost
// pattern, the same one this file already accepts), not per frame.
//
// quantShift clears that many low bits off each 5-bit RGB555 channel before
// dedup, so colours that only differ in a shade or two of anti-aliasing
// collapse onto the same palette entry: shift 0 is exact (today's original
// behaviour, unchanged for any texture that already fits in 255 colours --
// gui.png/icons.png/particles.png all measured fitting exactly, so none of
// them are affected), shift 4 leaves 2 levels per channel (8 colours total),
// which always fits and is the last resort.
enum class PalettedUploadResult { Success, ColorOverflow, SpaceExhausted };

// 4x4 Bayer ordered-dither matrix (values 0..15, i.e. sixteenths of one
// quantization step). Only used once quantShift > 0 -- a texture that fits
// the exact 255-colour palette (the common case: gui.png/icons.png/
// particles.png, and terrain.png/items.png most of the time now that the
// fire/portal re-upload waste is gone) is never touched by this, so nothing
// about the normal path changes. Once quantShift kicks in, each channel is
// rounded down to a multiple of a 2^quantShift step (see channelMask
// below); without dithering that shows up as visible banding -- flat
// colour steps instead of a smooth gradient, most noticeable exactly on the
// large gradient-heavy atlases (terrain.png/items.png) real-hardware logs
// show hitting shift 2-3 routinely. Biasing each pixel by its position in
// this fixed 4x4 pattern before truncating spreads that rounding error into
// a stipple instead of a hard edge -- same total palette size and upload
// cost (still exactly 1 byte/pixel, still one pass), just a less visually
// obvious loss.
constexpr std::uint8_t kBayer4x4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 },
};

PalettedUploadResult tryUploadPalettedAtDepth(int name, const DsiTexture& tex, int param,
	int quantShift, std::size_t& outColorCount, std::vector<std::uint16_t>& outPalette)
{
	const std::size_t pixelCount = static_cast<std::size_t>(tex.width) * tex.height;
	const std::uint8_t channelMask = static_cast<std::uint8_t>(~((1u << quantShift) - 1u) & 0x1Fu);
	// Size, in 8-bit channel units, of the step that quantShift will round
	// away -- the amount kBayer4x4's bias needs to cover so it can push a
	// pixel into either neighbouring palette level depending on position.
	const int ditherStep = quantShift > 0 ? (1 << (quantShift + 3)) : 0;

	// A fixed 32768-entry (64KB) table regardless of image size -- always the
	// same size, so a fresh std::vector here means a fresh malloc/free of the
	// same 64KB block every single call. tryUploadPaletted() above calls this
	// up to 5 times in a row (quantShift 0..4) for one texture load, and a
	// texture load itself happens repeatedly over a session (world entry/
	// exit, texture pack switch) -- real-hardware investigation this round
	// into why returning to the menu stays slow even after the SD-commit
	// fixes traced part of that cost to texture decode/upload work exactly
	// like this. Static and reused instead: std::fill() resets it to the
	// same -1 state a fresh vector would start at, without the allocator
	// round-trip. Not threaded (single ARM9 core, no concurrent texture
	// uploads), so a shared static is safe here.
	static std::vector<std::int16_t> s_dsiColorToIndex(32768, -1);
	std::fill(s_dsiColorToIndex.begin(), s_dsiColorToIndex.end(), -1);
	std::vector<std::int16_t> &colorToIndex = s_dsiColorToIndex;
	std::vector<std::uint16_t>& palette = outPalette;
	palette.clear();
	palette.reserve(256);
	palette.push_back(0); // Index 0's actual colour is irrelevant -- GL_TEXTURE_COLOR0_TRANSPARENT hides it.
	std::vector<std::uint8_t> indices(pixelCount);

	const std::uint8_t* src = tex.rgba.data();
	for (std::size_t i = 0; i < pixelCount; ++i)
	{
		const std::uint8_t a = src[i * 4 + 3];
		if (a < 128)
		{
			indices[i] = 0;
			continue;
		}

		std::uint8_t r8 = src[i * 4 + 0];
		std::uint8_t g8 = src[i * 4 + 1];
		std::uint8_t b8 = src[i * 4 + 2];
		if (ditherStep > 0)
		{
			const int x = static_cast<int>(i % static_cast<std::size_t>(tex.width));
			const int y = static_cast<int>(i / static_cast<std::size_t>(tex.width));
			const int bias = (kBayer4x4[y & 3][x & 3] * ditherStep) / 16;
			r8 = static_cast<std::uint8_t>(std::min(255, r8 + bias));
			g8 = static_cast<std::uint8_t>(std::min(255, g8 + bias));
			b8 = static_cast<std::uint8_t>(std::min(255, b8 + bias));
		}
		const std::uint8_t r = (r8 >> 3) & channelMask;
		const std::uint8_t g = (g8 >> 3) & channelMask;
		const std::uint8_t b = (b8 >> 3) & channelMask;
		const std::uint16_t color15 = static_cast<std::uint16_t>(r | (g << 5) | (b << 10));

		std::int16_t index = colorToIndex[color15];
		if (index < 0)
		{
			if (palette.size() >= 256)
			{
				outColorCount = palette.size();
				return PalettedUploadResult::ColorOverflow;
			}
			index = static_cast<std::int16_t>(palette.size());
			colorToIndex[color15] = index;
			palette.push_back(color15);
		}
		indices[i] = static_cast<std::uint8_t>(index);
	}

	glBindTexture(0, name);
	const int uploaded = glTexImage2D(0, 0, GL_RGB256, tex.width, tex.height, 0,
		param | GL_TEXTURE_COLOR0_TRANSPARENT, indices.data());
	outColorCount = palette.size();
	if (!uploaded)
		return PalettedUploadResult::SpaceExhausted;

	// Real-hardware investigation (still-open "no PNG has transparency" report,
	// after ruling out every upload/bind/draw-time GPU state this file
	// controls -- see the draw-time GFX_CONTROL/GFX_ALPHA_TEST diagnostic
	// above): this return value used to be discarded. libnds's own
	// glColorTableNtr() (videoGL.c) allocates from PALETTE VRAM -- banks E/F/G,
	// a separate and much smaller budget than the 512KB texture-IMAGE budget
	// (banks A-D) everything else in this file tracks -- and returns 0,
	// silently leaving NO palette bound to this texture (GFX_PAL_FORMAT left
	// at 0), if that separate budget is exhausted. Every other failure path
	// in this file already falls through to a fallback; this one was
	// reporting Success regardless, so a texture whose pixel DATA upload
	// worked but whose PALETTE upload silently failed would draw with no
	// colour lookup for any index -- including index 0, whose
	// GL_TEXTURE_COLOR0_TRANSPARENT "clear" behaviour this whole mechanism
	// depends on. Unlike the image-data VRAM above (SpaceExhausted, not
	// worth retrying at a smaller quantShift -- a coarser quantization does
	// not change that byte footprint), a coarser quantShift DOES shrink the
	// palette's own footprint here, so this reports ColorOverflow instead:
	// the same "retry at a coarser quantization" path already used when the
	// palette does not fit in 256 entries applies just as well when it does
	// not fit in palette VRAM. The warning below turns the next real-hardware
	// log into a direct answer instead of another guess.
	//
	// NOTE: this exact check was accidentally reverted by a later commit in
	// the same round that meant only to remove two unrelated diagnostics
	// (git history: added in one commit, silently dropped by the next one's
	// edit, restored here) -- keeping this note so that mistake is visible
	// in the code itself, not just buried in commit history.
	if (!glColorTableNtr(palette.size(), palette.data()))
	{
		MC_LOG_WARN("dsi", "paletted upload's colour table rejected: %dx%d, %u colours, palette VRAM (banks E/F/G) exhausted\n",
			tex.width, tex.height, (unsigned)palette.size());
		return PalettedUploadResult::ColorOverflow;
	}
	return PalettedUploadResult::Success;
}

// Nearest existing palette entry by RGB555 channel distance -- used once
// tex.stablePalette is full (256 entries) and a patch introduces a colour
// that genuinely is not in it yet. Never returns index 0 (the reserved
// transparent slot): an opaque pixel landing there would render as a hole
// in the texture regardless of what colour index 0 actually stores, since
// GL_TEXTURE_COLOR0_TRANSPARENT hides it unconditionally.
int nearestPaletteIndex(const std::vector<std::uint16_t>& palette, std::uint16_t color15)
{
	if (palette.size() <= 1)
		return 0; // No real colour to match against yet; only reachable for a texture that was 100% transparent until now.

	const int r = color15 & 0x1F;
	const int g = (color15 >> 5) & 0x1F;
	const int b = (color15 >> 10) & 0x1F;
	int bestIndex = 1;
	int bestDist = INT_MAX;
	for (std::size_t i = 1; i < palette.size(); ++i)
	{
		const int pr = palette[i] & 0x1F;
		const int pg = (palette[i] >> 5) & 0x1F;
		const int pb = (palette[i] >> 10) & 0x1F;
		const int dr = r - pr, dg = g - pg, db = b - pb;
		const int dist = dr * dr + dg * dg + db * db;
		if (dist < bestDist)
		{
			bestDist = dist;
			bestIndex = static_cast<int>(i);
		}
	}
	return bestIndex;
}

// Patches indices against tex.stablePalette/tex.stableQuantShift as they
// stand -- established once by a full-image upload (see tryUploadPaletted
// below) -- instead of rebuilding the palette from this call's pixels.
//
// Real-hardware evidence this fixes: glTexImageNtr2D() has no partial
// upload (see the DsiTexture struct comment above), so every animated-tile
// sub-image patch (lava/water/fire flicker, leaf/water colormap tinting,
// any TextureFX still enabled) used to re-run the FULL establishing scan
// below on the *entire* 256x256 atlas, every single time. That scan is
// scan-order-greedy: whichever pixel reaches a given quantized colour
// bucket first wins that palette slot's stored value, and with dithering
// (kBayer4x4 above) that winning value depends on the pixel's (x, y)
// position too. So a one-tile patch changing what the scan sees first could
// silently reassign the stored colour of a palette slot shared by
// thousands of unrelated pixels elsewhere on the atlas -- e.g. a tree tile
// nowhere near the animated tile -- every time that patch ran. That matches
// a real-hardware report of tree textures visibly cycling between colours
// and textures flickering white for a frame: not a random glitch, the
// palette for the whole atlas was being non-deterministically re-derived
// on every dynamic-tile update. Keeping the palette stable once established
// removes both the per-patch full-atlas rescan (a real CPU cost paid on
// every dynamic tile update) and the colour instability in one change.
bool tryUploadWithStablePalette(int name, DsiTexture& tex, int param)
{
	const std::size_t pixelCount = static_cast<std::size_t>(tex.width) * tex.height;
	const int quantShift = tex.stableQuantShift;
	const std::uint8_t channelMask = static_cast<std::uint8_t>(~((1u << quantShift) - 1u) & 0x1Fu);
	const int ditherStep = quantShift > 0 ? (1 << (quantShift + 3)) : 0;

	std::vector<std::int16_t> colorToIndex(32768, -1);
	for (std::size_t i = 0; i < tex.stablePalette.size(); ++i)
		colorToIndex[tex.stablePalette[i]] = static_cast<std::int16_t>(i);

	std::vector<std::uint8_t> indices(pixelCount);
	const std::uint8_t* src = tex.rgba.data();
	for (std::size_t i = 0; i < pixelCount; ++i)
	{
		const std::uint8_t a = src[i * 4 + 3];
		if (a < 128)
		{
			indices[i] = 0;
			continue;
		}

		std::uint8_t r8 = src[i * 4 + 0];
		std::uint8_t g8 = src[i * 4 + 1];
		std::uint8_t b8 = src[i * 4 + 2];
		if (ditherStep > 0)
		{
			const int x = static_cast<int>(i % static_cast<std::size_t>(tex.width));
			const int y = static_cast<int>(i / static_cast<std::size_t>(tex.width));
			const int bias = (kBayer4x4[y & 3][x & 3] * ditherStep) / 16;
			r8 = static_cast<std::uint8_t>(std::min(255, r8 + bias));
			g8 = static_cast<std::uint8_t>(std::min(255, g8 + bias));
			b8 = static_cast<std::uint8_t>(std::min(255, b8 + bias));
		}
		const std::uint8_t r = (r8 >> 3) & channelMask;
		const std::uint8_t g = (g8 >> 3) & channelMask;
		const std::uint8_t b = (b8 >> 3) & channelMask;
		const std::uint16_t color15 = static_cast<std::uint16_t>(r | (g << 5) | (b << 10));

		std::int16_t index = colorToIndex[color15];
		if (index < 0)
		{
			if (tex.stablePalette.size() < 256)
			{
				index = static_cast<std::int16_t>(tex.stablePalette.size());
				colorToIndex[color15] = index;
				tex.stablePalette.push_back(color15);
			}
			else
			{
				index = static_cast<std::int16_t>(nearestPaletteIndex(tex.stablePalette, color15));
			}
		}
		indices[i] = static_cast<std::uint8_t>(index);
	}

	glBindTexture(0, name);
	const int uploaded = glTexImage2D(0, 0, GL_RGB256, tex.width, tex.height, 0,
		param | GL_TEXTURE_COLOR0_TRANSPARENT, indices.data());
	if (!uploaded)
		return false;

	// See tryUploadPalettedAtDepth()'s identical check (and its note on this
	// exact fix having been accidentally reverted once already) for why this
	// return value matters: glColorTableNtr() can silently fail (palette
	// VRAM, banks E/F/G, exhausted) while leaving the texture's pixel DATA
	// upload above looking successful, previously reported as success
	// regardless.
	if (!glColorTableNtr(tex.stablePalette.size(), tex.stablePalette.data()))
	{
		MC_LOG_WARN("dsi", "stable-palette re-upload's colour table rejected: %dx%d, %u colours, palette VRAM (banks E/F/G) exhausted\n",
			tex.width, tex.height, (unsigned)tex.stablePalette.size());
		return false;
	}
	return true;
}

// Diagnostic for the VRAM-exhaustion investigation confirmed items.png/
// inventory.png specifically overflow the exact-colour palette (256x256,
// thousands of distinct anti-aliased shades across a large item atlas) --
// that is what forced them onto the costlier GL_RGBA path, which then did
// not fit in whatever VRAM remained ("texture VRAM must be full"). Retrying
// at coarser quantization keeps them on the cheap 1 byte/pixel path instead:
// visibly flatter shading on that one texture, but a real, legible item
// icon instead of the checkerboard placeholder real hardware was showing
// before this. Bounded to 5 attempts (shift 0..4); shift 4 always fits (at
// most 8 colours), so this cannot fail here -- SpaceExhausted is the only
// early exit, since a coarser quantization does not change the byte
// footprint (still 1 byte/pixel regardless of how many of the 256 slots are
// used), only whether the colour count fits, so retrying after a genuine
// GPU-out-of-space rejection would just fail again identically.
//
// forceRebuild is true only for a genuine full-image (re)load
// (renderTextureImageRgba -- texture pack switch, first load, ...): that
// always re-establishes tex.stablePalette from scratch, since it may be
// wholly different content now. A sub-image patch (renderTextureSubImageRgba)
// passes false and reuses whatever was already established -- see
// tryUploadWithStablePalette above for why.
bool tryUploadPaletted(int name, DsiTexture& tex, int param, bool forceRebuild)
{
	if (!forceRebuild && tex.stableQuantShift >= 0)
		return tryUploadWithStablePalette(name, tex, param);

	tex.stablePalette.clear();
	tex.stableQuantShift = -1;
	for (int quantShift = 0; quantShift <= 4; ++quantShift)
	{
		std::size_t colorCount = 0;
		std::vector<std::uint16_t> palette;
		const PalettedUploadResult result = tryUploadPalettedAtDepth(name, tex, param, quantShift, colorCount, palette);
		if (result == PalettedUploadResult::Success)
		{
			// Info, not Warning: this is the retry loop working as designed
			// (see this function's header comment -- bounded, self-resolving,
			// shift 4 always fits), not a failure. A Warning/Error always
			// forces Log.cpp's writeFile() to fclose()+fopen() the SD-card
			// log file immediately, regardless of MC_LOG_COMMIT_EVERY; real
			// hardware showed a texture that needed a few retries here
			// costing several of those forced commits back to back during
			// world load, measured as multi-second frame/tick spikes with
			// no named tick/render phase accounting for the time (it was
			// spent in the SD filesystem, not in game logic). Genuine
			// failures below (actually out of VRAM/palette space) keep
			// Warning, since those are rare and worth the immediate commit.
			if (quantShift > 0)
				MC_LOG_INFO("dsi", "paletted upload used colour quantization (shift=%d, %u colours) to fit: %dx%d id=%d\n",
					quantShift, (unsigned)colorCount, tex.width, tex.height, name);
			tex.stablePalette = std::move(palette);
			tex.stableQuantShift = quantShift;
			return true;
		}
		if (result == PalettedUploadResult::SpaceExhausted)
		{
			MC_LOG_WARN("dsi", "paletted upload rejected: %dx%d, %u colours, GPU out of texture VRAM space, id=%d\n",
				tex.width, tex.height, (unsigned)colorCount, name);
			return false;
		}
		// Info, not Warning -- see the Success branch's comment above: this
		// is one expected step of a bounded, self-resolving retry, not a
		// failure worth forcing an immediate SD-card log commit for.
		MC_LOG_INFO("dsi", "palette overflow at shift=%d: %dx%d texture exceeds 255 distinct opaque colours id=%d%s\n",
			quantShift, tex.width, tex.height, name, quantShift < 4 ? "; retrying with coarser colour quantization" : "");
	}
	return false; // Unreachable in practice: shift 4 always fits (at most 8 colours).
}

// Returns whether the DS GPU actually accepted this texture. glTexImage2D()
// (really glTexImageNtr2D(), see nds/arm9/videoGL.h) requires each dimension
// to be an EXACT power of two from 8 to 1024 and returns 0 -- uploading
// nothing -- for anything else (also on VRAM exhaustion). That return value
// used to be discarded here, so a real-hardware asset with, say, a
// non-power-of-two panorama.png silently uploaded nothing while every CPU-
// side book-keeping struct (DsiTexture::allocated, renderTextureIsValid())
// still reported success: the polygon still drew, just textureless, at
// whatever flat vertex colour the caller set -- solid white for
// LegacyPanorama.cpp's renderColor4f(1,1,1,1). Checking it here lets
// renderTextureImageRgba() report the failure truthfully, so RenderEngine.cpp's
// existing missing-texture fallback (the black/white checkerboard,
// createMissingTexture()) actually engages the way it already does for a
// texture that fails to *decode* -- a recognisable placeholder instead of an
// invisible, silent failure.
bool uploadTexture(int name, DsiTexture& tex, bool forceRebuildPalette)
{
	if (tex.width <= 0 || tex.height <= 0 || tex.rgba.empty())
		return false;

	int param = 0;
	if (!tex.clamp)
		param |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;

	if (!tex.forceHighPrecision && tryUploadPaletted(name, tex, param, forceRebuildPalette))
	{
		glTexParameter(0, param); // wrap bits already set above; kept for parity with callers that only touch params later
		tex.paletted = true;
		return true;
	}

	std::vector<std::uint16_t> converted(static_cast<std::size_t>(tex.width) * tex.height);
	convertRgba8ToDs(tex.rgba.data(), converted.data(), tex.width * tex.height);

	glBindTexture(0, name);
	const int uploaded = glTexImage2D(0, 0, GL_RGBA, tex.width, tex.height, 0, param, converted.data());
	if (uploaded)
	{
		glTexParameter(0, param); // wrap bits already set above; kept for parity with callers that only touch params later
		tex.paletted = false;
		return true;
	}

	// Real-hardware regression this fixes: forceHighPrecision (terrain.png/
	// gui/items.png, see the DsiTexture field comment) used to mean "GL_RGBA
	// or nothing" -- fine for terrain.png, which fits, but gui/items.png
	// loads AFTER terrain.png/gui.png/font/mob skins are already resident,
	// and its own 128KB GL_RGBA request routinely lost that race by a few
	// KB ("~392KB/512KB already resident" in one real log). With no
	// fallback, that took down EVERY item icon at once -- the whole atlas
	// fell back to RenderEngine.cpp's checkerboard placeholder, which is
	// what real hardware reported as "leather and raw beef are solid white"
	// and "no item has transparency" (the checkerboard has neither the
	// right colours nor real alpha). Retrying at the paletted path here
	// costs the same quantization quality items.png already had before
	// forceHighPrecision existed -- worse than a guaranteed-fit RGBA
	// upload, but a real, recognisable, correctly-transparent icon beats a
	// checkerboard placeholder for the whole atlas.
	//
	// Diagnostic added on request: a real-hardware log (predating this repo's
	// vram= tracking, see Minecraft.cpp's memtrend line) showed a 256x256
	// texture -- almost certainly terrain.png, the only forceHighPrecision
	// atlas that size touched this early in a session -- landing on this
	// exact fallback and needing shift=3 (61 colours total) to fit its
	// palette, right at the start of gameplay. 61 colours spread across the
	// whole block atlas would show as flat, undetailed textures -- possibly
	// THE "blocks have their colour but no texture detail" report this is
	// chasing, not yet confirmed because that log predates dsiTotalTextureVramBytes()
	// being wired into the memtrend line. This makes the next log say so
	// directly: how full the 512KB texture-image budget was at the exact
	// moment terrain.png's real-quality RGBA upload lost the room it needed.
	MC_LOG_WARN("dsi", "high-precision upload failed for %dx%d, falling back to paletted; vram=%u/512KB\n",
		tex.width, tex.height, (unsigned)(dsiTotalTextureVramBytes() / 1024u));

	if (tryUploadPaletted(name, tex, param, forceRebuildPalette))
	{
		glTexParameter(0, param);
		tex.paletted = true;
		return true;
	}

	tex.paletted = false;
	return false;
}

// -----------------------------------------------------------------------------
// Lightmap-as-vertex-colour -- ASSUMED UV convention.
// -----------------------------------------------------------------------------
// The DS 3D engine has exactly one texture unit -- no multitexture combiner
// stage -- unlike the Wii's GX (see RenderAPI_GX_WII.cpp, which uses real
// hardware multitexturing for this same lightmap). PS2's GS does have one but
// this backend takes the other approach PS2 could have taken too: bake the
// lightmap into the per-vertex colour instead. Minecraft's standard shading
// mode is POLY_MODULATION (texture x vertex colour, the libnds default), so
// setting the vertex colour to the looked-up lightmap colour right before each
// vertex achieves the same on-screen result as sampling a second texture and
// multiplying, at the cost of losing hardware bilinear filtering across
// lightmap texel boundaries (a soft lighting gradient becomes bands instead) --
// a real, visible quality loss worth revisiting once this can be seen running.
//
// OpenGlHelper::setLightmapTextureCoords() forwards normalized OpenGL texture
// coordinates (u, v in [0, 1]) exactly as the caller computed them for a real
// GL texture sampler; renderSetLightmapColors() is handed that same texture's
// full pixel data (count entries). This assumes the standard row-major square
// layout a 2D texture lookup implies -- index = round(v * (side-1)) * side +
// round(u * (side-1)), side = sqrt(count) -- rather than tracing the exact
// lightmap texture's dimensions through Minecraft.cpp/EntityRenderer.cpp,
// which is the first thing to check if lighting looks wrong (banded/blocky is
// expected and is the filtering loss above; discontinuous/scrambled would mean
// this indexing assumption is wrong).
std::vector<std::uint32_t> g_lightmapColors; // RGBA8, one std::uint32_t per texel
int g_lightmapUnit = -1; // OpenGL's GL_TEXTURE1_ARB-style enum for the lightmap unit, once seen
// sqrt(g_lightmapColors.size()), cached by renderSetLightmapColors() below --
// see lightmapColorAt()'s own comment for why this must not be recomputed
// there. 0 means "not a perfect square" (lightmapColorAt() no-ops on that),
// matching the old inline check's behaviour.
int g_lightmapSide = 0;

// Looks up the baked lightmap colour at (u, v) without touching GL state.
// Split out of applyLightmapColorAt() so a caller that ALSO has a per-vertex
// tint colour (see drawInterleavedMesh's hasBrightness+hasColor combine
// below) can multiply the two together before the one glColor call this
// hardware's single texture unit gets, instead of one silently overwriting
// the other. Returns false (leaving r8/g8/b8 untouched) exactly when there is
// no lightmap data to look up, same condition applyLightmapColorAt used to
// silently no-op on.
//
// Real-hardware performance concern raised this round, and correct: this now
// runs once per vertex on every lit/tinted mesh drawn, every frame (it used
// to no-op on the very first line, back when g_lightmapColors was never
// populated on DSi at all -- see renderSetLightmapColors()'s own history).
// The integer sqrt used to be recomputed HERE, per vertex -- up to 16
// iterations of multiply-and-compare on an FPU-less ARM9, thousands of times
// a frame for a chunk-heavy scene, for a value (g_lightmapColors' size,
// always exactly 256) that never changes between one call and the next.
// Hoisted into g_lightmapSide, computed once per frame in
// renderSetLightmapColors() below instead of once per vertex here.
bool lightmapColorAt(float u, float v, std::uint8_t& r8, std::uint8_t& g8, std::uint8_t& b8)
{
	if (g_lightmapColors.empty() || g_lightmapSide <= 0)
		return false;

	const int sqrtSide = g_lightmapSide;

	auto clampIndex = [](float value, int maxIndex) -> int
	{
		if (value < 0.0f) value = 0.0f;
		if (value > 1.0f) value = 1.0f;
		const int result = static_cast<int>(value * (maxIndex) + 0.5f);
		return result < 0 ? 0 : (result > maxIndex ? maxIndex : result);
	};

	const int col = clampIndex(u, sqrtSide - 1);
	const int row = clampIndex(v, sqrtSide - 1);
	const std::uint32_t packed = g_lightmapColors[static_cast<std::size_t>(row) * sqrtSide + col];

	r8 = static_cast<std::uint8_t>((packed >> 0) & 0xFF);
	g8 = static_cast<std::uint8_t>((packed >> 8) & 0xFF);
	b8 = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
	return true;
}

void applyLightmapColorAt(float u, float v)
{
	std::uint8_t r8, g8, b8;
	if (!lightmapColorAt(u, v, r8, g8, b8))
		return;
	// glColor3b() takes the same 0-255 bytes lightmapColorAt() already
	// produced -- glColor3f() would mean dividing each by 255.0f here only
	// for libnds to multiply by 255 again internally to get back to the byte
	// it started from, and float division has no hardware support on this
	// ARM9 (arm946e-s+nofp): a software-float call per channel, per vertex,
	// on what was already an exact integer.
	glColor3b(r8, g8, b8);
}

// -----------------------------------------------------------------------------
// Fog -- APPROXIMATED. The DS fog is a 32-entry density lookup table indexed
// by (depth >> shift) - offset, not GL's parametric density/start/end/mode.
// Rebuilding the table from GL-style parameters on every renderFogf() call
// would be the faithful approach; for now this only forwards the colour
// (renderFogColor -> glFogColor) and leaves the table at libnds's default
// (set once in dsiEnsureEarlyVideo()-equivalent init below), which is a
// reasonable fixed falloff but does not track Minecraft's actual fog
// start/end distance options yet. Flagged rather than silently wrong: fog
// will render as *some* distance fog, just not necessarily at the requested
// distance.
float g_fogDensity = 1.0f;
float g_fogStart = 0.0f;
float g_fogEnd = 1.0f;

} // namespace

// Sum of textureVramBytes() across every currently-allocated texture: how much
// of the 512 KB texture-image budget (DsiEarlyVideo.cpp's four banks) is
// actually spoken for right now. See dsi/DsiEarlyInit.h's declaration for who
// calls this and why.
std::size_t dsiTotalTextureVramBytes()
{
	std::size_t total = 0;
	for (const DsiTexture& tex : g_textures)
		total += textureVramBytes(tex);
	return total;
}

// Same cost query as above, for one texture name's already-resolved GL id.
// Lets RenderEngine.cpp (which only knows resource-name -> id via its own
// textureMap, not this file's internals) print a per-texture breakdown, not
// just the running total.
std::size_t dsiTextureVramBytes(int name)
{
	const DsiTexture* tex = textureSlot(name);
	return tex ? textureVramBytes(*tex) : 0;
}

// See DsiCapturedMeshRepack.h's own comment for who this is for and why.
int dsiGetBoundTexture()
{
	return g_boundTexture;
}

void dsiAdvanceMeshRepack(DsiMeshRepackState &state, RenderStaticMesh &live, bool recompiled)
{
	if (recompiled)
	{
		// live.captured is the freshly-compiled, not-yet-repacked mesh --
		// mirror it into staging to repack there, leaving `live` itself
		// alone (and still correctly drawable via the ordinary float path)
		// until the repack finishes.
		state.staging.captured = live.captured;
		state.stage = 0;
		state.cursor = 0;
	}

	if (state.stage >= 2)
		return;

	// vertexBudget 0 -> dsiRepackCapturedMeshStep() covers the whole mesh in
	// one call for whichever stage runs this frame. Fine for the small
	// meshes this is meant for (a HUD element's few dozen vertices, one
	// entity model part's handful of cube faces) -- nowhere near the
	// several-thousand-vertex terrain sections that motivated that
	// function's own per-call budget in the first place, so there is no
	// spike to budget against here. Still always at least 2 frames end to
	// end -- the function itself yields once between stages by design.
	if (dsiRepackCapturedMeshStep(state.staging.captured, dsiGetBoundTexture(), 0, state.stage, state.cursor))
	{
		// Fully repacked: this is now what every later frame actually draws.
		live.captured = state.staging.captured;
	}
}

// -----------------------------------------------------------------------------
// Static / captured mesh replay -- VERIFIED per-call mapping (glBegin/
// glVertex3f/glTexCoord2f/glColor3b), still true for drawInterleavedMeshSlow()
// below: the per-vertex fallback for ONE-SHOT/dynamic meshes (GUI, particles,
// anything rebuilt most frames anyway), kept as the correctness reference and
// as the fallback drawInterleavedMeshFast() below defers to when it cannot
// safely pack a call (see that function's own eligibility checks).
//
// Both the captured/static path (dsiRepackCapturedMeshStep()/
// drawCapturedMeshFast() further below, for chunk section meshes that replay
// UNCHANGED across many frames between rebuilds) and drawInterleavedMeshFast()
// immediately below this banner (for geometry that is DIFFERENT every single
// call) now use the same underlying trick: pack every vertex's commands into
// a buffer once and replay the whole call with a single DMA into the
// geometry engine's FIFO port, instead of one glColor3b()/glTexCoord2f()/
// glNormal3f()/glVertex3f() libnds call per vertex. The saving for the
// one-shot path does NOT come from reusing the pack across frames (there is
// nothing to reuse -- the content really is different every call) -- it
// comes from replacing N separate function calls and N separate FIFO
// register writes (each a potential stall if the FIFO is momentarily full)
// with one bulk transfer the DMA controller paces on its own. See
// drawInterleavedMeshFast()'s own comment for the eligibility checks that
// keep this safe, and dsiRepackCapturedMeshStep()'s own banner for the
// original design and the real-hardware/ClassiCube validation this reuses.
// -----------------------------------------------------------------------------
namespace
{

// Every vertex position this engine submits goes through the same v16
// pre-scale dance -- see the full story where drawInterleavedMeshSlow() uses
// these below. Hoisted to file scope so renderCaptureInterleaved()'s
// capture-time conversion (below) and this function's own draw-time
// conversion (for one-shot/dynamic meshes, which have no earlier "capture"
// step to move the cost into) apply the exact same constants.
constexpr float kVertexScale = 256.0f;
constexpr float kInvVertexScale = 1.0f / kVertexScale;

// A Minecraft interleaved vertex is always float3 position, float2 texcoord,
// RGBA8 colour, signed-byte3 normal, in whatever subset the mesh's hasTexture/
// hasColor/hasNormals/hasBrightness flags declare -- see RenderInterleavedMesh
// in RenderAPI.h. This is the correctness reference and fallback: it walks
// that buffer and issues one glVertex3f() (plus whatever glTexCoord2f()/
// glColor3b()/glNormal3f() precede it) per vertex -- see
// drawInterleavedMeshFast() below for the packed-DMA path this now falls
// back from, and drawInterleavedMesh() (the dispatcher every other call site
// actually calls, at the end of this namespace block) for how the two are
// wired together.
bool drawInterleavedMeshSlow(const RenderInterleavedMesh& mesh)
{
	if (!mesh.data || mesh.count <= 0 || mesh.stride <= 0)
		return false;

	const std::uint8_t* base = static_cast<const std::uint8_t*>(mesh.data) + (std::size_t)mesh.first * mesh.stride;

	// The DS GPU has one alpha value per polygon batch (POLY_ALPHA, see
	// renderColor4f above), not per vertex -- there is no generalised
	// per-fragment blend the way a per-vertex alpha byte would imply. A
	// translucent Tessellator draw (GuiMainMenu's drawGradientRect vignette,
	// drawPanorama's sample cross-fade) carries its alpha in mesh.hasColor's
	// rgba[3], which the per-vertex glColor3b() call below drops on the
	// floor -- glColor3b has no alpha parameter, and nothing else in this
	// function ever reads rgba[3]. Net effect on real hardware: every such
	// draw came out fully opaque regardless of what alpha the caller asked
	// for. Average every vertex's alpha and latch that for the whole batch
	// before applyPolyFormatIfDirty() below picks it up: an approximation
	// for an actual gradient (the fade across the quad is lost, every vertex
	// renders at one flat alpha), but it turns "always opaque" into
	// "actually translucent", which is what every caller here expects and
	// previously never got. Averaging rather than sampling one vertex
	// matters concretely for drawGradientRect: its two calls for the menu
	// vignette are top-transparent/bottom-opaque and top-opaque/bottom-
	// transparent, so reading only the first vertex would render one of
	// them at alpha 0 -- effectively deleting it -- instead of the
	// in-between value the average gives both.
	//
	// Sampling first+last (not a full per-vertex scan) is enough: every
	// caller of this path is either uniform alpha across the whole mesh
	// (glyph batches -- a full menu screen's worth of text lands in one
	// draw call here, easily hundreds of vertices, all sharing FontRenderer's
	// one currentA -- and most other UI/terrain draws), where first == last
	// gives the exact value for free, or a straight linear gradient like
	// drawGradientRect's two quads, where the two endpoints alone already
	// average to the exact in-between value a full scan would compute (the
	// two middle vertices share one or the other endpoint's alpha, so they
	// add nothing a full scan wouldn't already cancel out). A full O(n) scan
	// here previously ran on every hundred-plus-vertex text batch every
	// frame on a CPU with no hardware float unit (arm946e-s+nofp) -- real,
	// measurable overhead for zero difference in the result for anything
	// this engine actually draws.
	if (mesh.hasColor)
	{
		std::uint8_t alphaFirst;
		std::memcpy(&alphaFirst, base + mesh.colorOffset + 3, sizeof(alphaFirst));
		std::uint8_t alphaLast = alphaFirst;
		if (mesh.count > 1)
			std::memcpy(&alphaLast, base + (std::size_t)(mesh.count - 1) * mesh.stride + mesh.colorOffset + 3, sizeof(alphaLast));
		const unsigned int alphaSum = static_cast<unsigned int>(alphaFirst) + static_cast<unsigned int>(alphaLast);
		const float averageAlpha = static_cast<float>(alphaSum) / (255.0f * 2.0f);
		g_poly.alpha31 = static_cast<std::uint8_t>(averageAlpha * 31.0f + 0.5f);
		markPolyDirty();
	}

	applyPolyFormatIfDirty();

	GL_GLBEGIN_ENUM glPrimitive = GL_TRIANGLES;
	switch (mesh.primitive)
	{
		case RenderPrimitive::Triangles:
		case RenderPrimitive::TriangleStrip: // No native strip primitive is used
		case RenderPrimitive::TriangleFan:   // here; each vertex still gets emitted,
			glPrimitive = GL_TRIANGLES;       // just not connected as a strip/fan --
			break;                            // acceptable for Quads/Triangles, the
		case RenderPrimitive::Quads:          // only two primitives Minecraft's
			glPrimitive = GL_QUADS;           // Tessellator actually emits.
			break;
		default:
			return false;
	}

	// libnds's glVertex3f() converts straight to the DS vertex hardware's
	// native format: v16, a 16-bit signed 4.12 fixed-point value (see
	// floattov16() in nds/arm9/videoGL.h) that only represents roughly -8.0
	// to +7.9998 -- and the conversion happens on the raw float passed in,
	// BEFORE any modelview/projection matrix multiply (those run in much
	// wider 20.12 fixed point, in hardware, after this). Every caller of
	// this function submits vertices far outside +-8: GuiMainMenu's 2D
	// draws use screen-pixel coordinates directly (0 to 256/192, the logo
	// geometry out to 265), and nothing in the shared Tessellator/GuiScreen
	// code has ever had to think about this, because no other backend
	// (PC/PS2/Wii) has anything like it. Passed through unscaled, a
	// coordinate like this either silently wraps -- 256.0 and 192.0 are
	// both exact multiples of the v16 step (4096 units/px), so they wrap to
	// exactly 0.0 -- or lands somewhere else nonsensical. This is the real
	// hardware bug behind the DSi main menu photo: full-screen quads (the
	// darkening vignette) painting nothing, and the logo/splash text
	// collapsing into a garbled blob near the coordinate origin instead of
	// spanning the screen.
	//
	// Fix: scale every vertex down by kVertexScale before glVertex3f() sees
	// it, and push a matching glScalef(kVertexScale, ...) first, so the
	// GPU's own matrix multiply -- done in that wider fixed point, not v16
	// -- puts the geometry back where the caller meant it. kVertexScale is
	// 8x the screen's own longer dimension (256): comfortably covers every
	// 2D coordinate this engine draws (menus go out to a few hundred px at
	// most) and every chunk-local 3D vertex offset (well under 256/8 = 32
	// units) with a wide safety margin, while still leaving a v16 step of
	// 256/4096 = 1/16 unit -- far finer than this console's 256x192 screen
	// can show. Assumes GL_MODELVIEW is the active matrix mode, true for
	// every vertex-submitting draw in this engine (GL_PROJECTION is only
	// touched briefly for camera/ortho setup, never held across a
	// Tessellator draw).
	// kVertexScale/kInvVertexScale: file-scope now (see above) -- multiplying
	// by the reciprocal instead of dividing below is the same result
	// (kVertexScale is an exact power of two, so this reciprocal is exact
	// too, no precision lost), but division is one of the slower operations
	// a *hardware* FPU has, and this ARM9 (arm946e-s+nofp) has no FPU at all
	// -- every one of these was a soft-float library call, three per vertex,
	// every vertex, every draw call in the whole engine. Real cost on a menu
	// screen's text batch alone (a few hundred vertices).
	glPushMatrix();
	glScalef(kVertexScale, kVertexScale, kVertexScale);

	// Real-hardware performance regression this fixes: wiring up the lightmap
	// (see the hasBrightness/hasColor combine below) means a real glColor3b()
	// GPU FIFO write now happens for essentially every terrain vertex --
	// before that fix, an untinted lit vertex (mesh.hasBrightness but not
	// mesh.hasColor -- most block faces: dirt, stone, wood, anything without
	// a biome tint) issued NO colour command at all, since g_lightmapColors
	// was always empty and the whole branch below was unreachable. Frame
	// times roughly tripled once every one of those vertices started writing
	// GFX_COLOR. glColor3b() itself is one cheap register write, but this
	// hardware's geometry engine processes its command FIFO asynchronously
	// and can stall the CPU on a write if that FIFO is full -- plausible
	// given the FIFO now receives one extra command per vertex across an
	// entire visible chunk radius, every frame. Many adjacent vertices in a
	// flat, uniformly-lit face share the exact same final colour (same
	// lightmap bucket, no per-vertex tint), so track the last colour this
	// draw call actually issued and skip re-sending it when the next vertex
	// computes the identical value -- redundant state changes cost real FIFO
	// bandwidth for zero visual difference.
	bool haveLastColor = false;
	std::uint8_t lastColorR = 0, lastColorG = 0, lastColorB = 0;
	auto emitColorIfChanged = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b)
	{
		if (haveLastColor && r == lastColorR && g == lastColorG && b == lastColorB)
			return;
		glColor3b(r, g, b);
		lastColorR = r;
		lastColorG = g;
		lastColorB = b;
		haveLastColor = true;
	};

	glBegin(glPrimitive);
	for (int i = 0; i < mesh.count; ++i)
	{
		const std::uint8_t* vertex = base + (std::size_t)i * mesh.stride;

		// Real-hardware evidence this fixes: vanilla's block renderer (see
		// RenderBlocks.cpp) calls setBrightness() (the lightmap UV, i.e. the
		// world's actual light level at this vertex) and THEN setColorOpaque_F()
		// (the per-face shade / biome-tint / ambient-occlusion colour) for
		// essentially every face it emits -- that is the normal vanilla call
		// order, meant for a real second texture unit where the two combine by
		// GL_MODULATE hardware multiply. This backend has only one texture
		// unit, so applyLightmapColorAt() bakes the lightmap into glColor
		// instead -- but issuing that call and then unconditionally calling
		// glColor3b() again for hasColor used to make the SECOND call win,
		// discarding the lightmap colour outright. Every tinted/shaded face
		// (which is nearly all of them -- water, foliage, and every per-face
		// top/side shade) rendered at its raw tint regardless of actual light
		// level: water always "fully lit" white even at night, and every block
		// losing its day/night and torch-light darkening, matching the
		// real-hardware reports of water looking flat white and terrain
		// looking flat/undetailed. Fix: when a vertex carries both, multiply
		// the two colours together (texture x lightmap x tint, the same
		// three-way combine GL_MODULATE would have done) and issue ONE glColor
		// call with the result, instead of one silently overwriting the other.
		bool haveLightmapColor = false;
		std::uint8_t lightmapR = 255, lightmapG = 255, lightmapB = 255;
		if (mesh.hasBrightness)
		{
			// FIXED (was reading 8 bytes -- a float[2] u,v pair -- from a
			// field Tessellator only ever writes 4 bytes into): Tessellator::
			// setBrightness(int) stores World::getLightBrightnessForSkyBlocks()'s
			// packed int, (skyLight << 20) | (blockLight << 4) (see that
			// function; confirmed by Tessellator.cpp's own raw-buffer write,
			// rawBuffer[rawBufferIndex + 7] = brightness -- a single word, not
			// two), the exact same packed value vanilla OpenGL's lightmap path
			// sends via glMultiTexCoord2f(brightness & 0xffff, brightness >>
			// 16) with a texture-matrix scale of 1/256. The 8-byte read here
			// used to run 4 bytes past this field on every vertex (into the
			// next vertex's position, or -- for a mesh's last vertex -- past
			// the end of the whole captured buffer, undefined behaviour) and
			// fed lightmapColorAt() garbage u/v. Harmless in effect only
			// because PLATFORM_DSI_LIGHTMAP_ENABLED is 0 (DsiWorldTuning.h --
			// measured slower than lighting off, stays off on purpose), so
			// g_lightmapColors is always empty and lightmapColorAt() returns
			// false before ever using u/v -- fixed for correctness/robustness
			// regardless, not because anything currently reads the result.
			std::int32_t packedBrightness = 0;
			std::memcpy(&packedBrightness, vertex + mesh.brightnessOffset, sizeof(packedBrightness));
			const float lightU = static_cast<float>(packedBrightness & 0xffff) / 256.0f;
			const float lightV = static_cast<float>((packedBrightness >> 16) & 0xffff) / 256.0f;
			haveLightmapColor = lightmapColorAt(lightU, lightV, lightmapR, lightmapG, lightmapB);
		}
		if (mesh.hasColor)
		{
			std::uint8_t rgba[4];
			std::memcpy(rgba, vertex + mesh.colorOffset, sizeof(rgba));
			if (haveLightmapColor)
			{
				emitColorIfChanged(static_cast<std::uint8_t>((static_cast<int>(lightmapR) * rgba[0]) / 255),
				                   static_cast<std::uint8_t>((static_cast<int>(lightmapG) * rgba[1]) / 255),
				                   static_cast<std::uint8_t>((static_cast<int>(lightmapB) * rgba[2]) / 255));
			}
			else
			{
				emitColorIfChanged(rgba[0], rgba[1], rgba[2]);
			}
		}
		else if (haveLightmapColor)
		{
			// glColor3b(), not glColor3f(): lightmapR/G/B are already the
			// exact 0-255 bytes this needs -- see applyLightmapColorAt()'s
			// own comment for why converting to float and back is a real,
			// avoidable software-float cost on this FPU-less ARM9, paid on
			// every untinted (no mesh.hasColor) lit vertex, i.e. most block
			// faces in the world every single frame.
			emitColorIfChanged(lightmapR, lightmapG, lightmapB);
		}
		else
		{
			// Same fix as drawCapturedMeshFast()'s identical branch below --
			// see its comment for the real-hardware "black hearts" bug this
			// closes. Unreached for ordinary world terrain in practice
			// (every real block face calls setBrightness() and/or
			// setColorOpaque_F(), per this function's own comments above),
			// but present here too so nothing drawn through this path can
			// silently inherit a stale GFX_COLOR either.
			emitColorIfChanged(255, 255, 255);
		}
		if (mesh.hasNormals)
		{
			constexpr float kInvNormalScale = 1.0f / 127.0f;
			std::int8_t normal[3];
			std::memcpy(normal, vertex + mesh.normalOffset, sizeof(normal));
			glNormal3f(normal[0] * kInvNormalScale, normal[1] * kInvNormalScale, normal[2] * kInvNormalScale);
		}
		if (mesh.hasTexture)
		{
			float uv[2];
			std::memcpy(uv, vertex + mesh.texCoordOffset, sizeof(uv));
			glTexCoord2f(uv[0], uv[1]);
		}

		float position[3];
		std::memcpy(position, vertex, sizeof(position));
		if (mesh.positionShort)
		{
			// positionShort means the source buffer stores the position as a
			// compact GL_SHORT triple instead of 3 floats (see RenderAPI.h --
			// a PC vertex-buffer bandwidth optimisation, unrelated to the v16
			// hardware format discussed above). Nothing DSi-specific ever
			// creates such a mesh today (only the PC/PS2 backends set this
			// flag), so this is dead code on this backend; the memcpy above
			// always reads 3 floats. Left in so a future DSi caller that does
			// set it fails loudly (garbage values, not silently) instead of
			// this comment going stale.
		}
		glVertex3f(position[0] * kInvVertexScale, position[1] * kInvVertexScale, position[2] * kInvVertexScale);
	}
	glEnd();
	glPopMatrix(1);

	return true;
}

// Packed-DMA path for one-shot/dynamic meshes: same underlying trick as
// dsiRepackCapturedMeshStep()/drawCapturedMeshFast() (pack every vertex's
// commands into a buffer, replay with one DMA instead of N per-vertex libnds
// calls), generalised to a mesh whose hasTexture/hasNormals/hasColor/
// hasBrightness combination is only known at this call, not baked in ahead
// of time by an earlier capture step. The combination is still fixed FOR THE
// WHOLE CALL (RenderInterleavedMesh's flags are per-mesh, not per-vertex), so
// the same four-command-slot FIFO_COMMAND_PACK() header this file already
// uses for the captured path still applies -- just with COLOR and VERTEX16
// always present (drawInterleavedMeshSlow()'s own per-vertex logic always
// emits SOME colour, even a fallback white, and always emits a vertex) and
// TEX_COORD/NORMAL only when the mesh actually carries them. VERTEX16 is
// always the LAST real command in every variant: on this hardware, writing
// GFX_VERTEX16 is what finalises a vertex and submits it to the rasteriser,
// so every other attribute for that vertex must already be latched by the
// time it's written -- the same ordering dsiRepackCapturedMeshStep()'s own
// header choices already rely on.
//
// Returns false (does nothing -- caller falls back to drawInterleavedMeshSlow())
// when it cannot safely build a packed call: a primitive this backend has no
// native grouping for, a position buffer in the PC-only short-vector format
// (never produced on this backend -- see drawInterleavedMeshSlow()'s own
// comment), or a texture the mesh asks for that is not yet resident (no
// texture dimensions to convert a float UV into t16 against -- the exact
// same residency gate dsiRepackCapturedMeshStep()'s stage 0 already applies
// before committing to texCoordIsT16).
bool drawInterleavedMeshFast(const RenderInterleavedMesh& mesh)
{
	if (!mesh.data || mesh.count <= 0 || mesh.stride <= 0)
		return false;

	if (mesh.positionShort)
		return false;

	// Safety ceiling on the reused scratch buffer below: it never shrinks
	// once grown, so without a cap, one unusually large one-shot batch (a
	// big inventory/creative grid, a long chat/credits screen, ...) would
	// permanently reserve that much memory for the rest of the session --
	// a real concern on this platform's ~12-13.5MB total heap budget (see
	// DsiWorldTuning.h's own banner). 4096 vertices x 6 words/vertex x 4
	// bytes is a 96KB worst case, comfortably small against that budget;
	// anything bigger falls back to the slow path instead of growing
	// scratch further -- correct either way, just not the faster one for
	// whatever unusually large draw call hit this.
	constexpr int kMaxFastVertices = 4096;
	if (mesh.count > kMaxFastVertices)
		return false;

	GL_GLBEGIN_ENUM glPrimitive = GL_TRIANGLES;
	switch (mesh.primitive)
	{
		case RenderPrimitive::Triangles:
		case RenderPrimitive::TriangleStrip:
		case RenderPrimitive::TriangleFan:
			glPrimitive = GL_TRIANGLES;
			break;
		case RenderPrimitive::Quads:
			glPrimitive = GL_QUADS;
			break;
		default:
			return false;
	}

	const std::uint8_t* base = static_cast<const std::uint8_t*>(mesh.data) + (std::size_t)mesh.first * mesh.stride;

	// Texture residency is a per-call question (the bound texture cannot
	// change mid-call), resolved once here -- same approach as
	// dsiRepackCapturedMeshStep()'s stage 0. A requested-but-not-yet-resident
	// texture falls back to the slow path rather than packing a UV against a
	// width/height of zero.
	const DsiTexture* boundTex = mesh.hasTexture ? textureSlot(g_boundTexture) : nullptr;
	const bool texResident = boundTex && boundTex->allocated && boundTex->width > 0 && boundTex->height > 0;
	if (mesh.hasTexture && !texResident)
		return false;
	const float texW = texResident ? static_cast<float>(boundTex->width) : 0.0f;
	const float texH = texResident ? static_cast<float>(boundTex->height) : 0.0f;

	// Same translucency approximation as drawInterleavedMeshSlow() -- see its
	// own comment on why first+last vertex alpha is sampled, not scanned.
	if (mesh.hasColor)
	{
		std::uint8_t alphaFirst;
		std::memcpy(&alphaFirst, base + mesh.colorOffset + 3, sizeof(alphaFirst));
		std::uint8_t alphaLast = alphaFirst;
		if (mesh.count > 1)
			std::memcpy(&alphaLast, base + (std::size_t)(mesh.count - 1) * mesh.stride + mesh.colorOffset + 3, sizeof(alphaLast));
		const unsigned int alphaSum = static_cast<unsigned int>(alphaFirst) + static_cast<unsigned int>(alphaLast);
		const float averageAlpha = static_cast<float>(alphaSum) / (255.0f * 2.0f);
		g_poly.alpha31 = static_cast<std::uint8_t>(averageAlpha * 31.0f + 0.5f);
		markPolyDirty();
	}
	applyPolyFormatIfDirty();

	const bool wantTexCoord = mesh.hasTexture; // already proven resident above
	const bool wantNormal = mesh.hasNormals;
	const std::size_t wordsPerVertex = 2 /* header + colour */
		+ (wantTexCoord ? 1u : 0u) + (wantNormal ? 1u : 0u) + 2u /* vertex16 */;

	std::uint32_t header;
	if (wantTexCoord && wantNormal)
		header = FIFO_COMMAND_PACK(FIFO_COLOR, FIFO_TEX_COORD, FIFO_NORMAL, FIFO_VERTEX16);
	else if (wantTexCoord)
		header = FIFO_COMMAND_PACK(FIFO_COLOR, FIFO_TEX_COORD, FIFO_VERTEX16, FIFO_NOP);
	else if (wantNormal)
		header = FIFO_COMMAND_PACK(FIFO_COLOR, FIFO_NORMAL, FIFO_VERTEX16, FIFO_NOP);
	else
		header = FIFO_COMMAND_PACK(FIFO_COLOR, FIFO_VERTEX16, FIFO_NOP, FIFO_NOP);

	// Reused scratch buffer across calls instead of a fresh heap allocation
	// every call -- this path runs every frame for the GUI, particles, the
	// held item, the block-breaking overlay and the selection box, so a
	// malloc/free per draw would undercut the saving this exists to get.
	// Never shrinks, same growth shape as RenderCapturedMesh::compiledCommands;
	// grows at most a handful of times total (to the largest batch this
	// session ever draws) and then stays put.
	static std::vector<std::uint32_t> scratch;
	const std::size_t totalWords = (std::size_t)mesh.count * wordsPerVertex;
	if (scratch.size() < totalWords)
		scratch.resize(totalWords);
	std::uint32_t* out = scratch.data();

	for (int i = 0; i < mesh.count; ++i)
	{
		const std::uint8_t* vertex = base + (std::size_t)i * mesh.stride;

		// Identical colour/lightmap combine to drawInterleavedMeshSlow()'s own
		// per-vertex loop -- kept in sync by hand, same as that function's own
		// relationship to drawCapturedMeshFast()'s per-vertex fallback.
		std::uint8_t r = 255, g = 255, b = 255;
		bool haveLightmapColor = false;
		std::uint8_t lightmapR = 255, lightmapG = 255, lightmapB = 255;
		if (mesh.hasBrightness)
		{
			std::int32_t packedBrightness = 0;
			std::memcpy(&packedBrightness, vertex + mesh.brightnessOffset, sizeof(packedBrightness));
			const float lightU = static_cast<float>(packedBrightness & 0xffff) / 256.0f;
			const float lightV = static_cast<float>((packedBrightness >> 16) & 0xffff) / 256.0f;
			haveLightmapColor = lightmapColorAt(lightU, lightV, lightmapR, lightmapG, lightmapB);
		}
		if (mesh.hasColor)
		{
			std::uint8_t rgba[4];
			std::memcpy(rgba, vertex + mesh.colorOffset, sizeof(rgba));
			if (haveLightmapColor)
			{
				r = static_cast<std::uint8_t>((static_cast<int>(lightmapR) * rgba[0]) / 255);
				g = static_cast<std::uint8_t>((static_cast<int>(lightmapG) * rgba[1]) / 255);
				b = static_cast<std::uint8_t>((static_cast<int>(lightmapB) * rgba[2]) / 255);
			}
			else
			{
				r = rgba[0]; g = rgba[1]; b = rgba[2];
			}
		}
		else if (haveLightmapColor)
		{
			r = lightmapR; g = lightmapG; b = lightmapB;
		}
		// else: r=g=b=255 (white) -- same fallback as drawInterleavedMeshSlow()'s
		// final else branch, so a mesh with neither colour nor brightness still
		// gets an explicit white command instead of inheriting whatever
		// GFX_COLOR the previous draw call happened to leave latched.

		out[0] = header;
		out[1] = static_cast<std::uint32_t>(RGB15(r >> 3, g >> 3, b >> 3));
		int outIdx = 2;
		if (wantTexCoord)
		{
			float uv[2];
			std::memcpy(uv, vertex + mesh.texCoordOffset, sizeof(uv));
			out[outIdx++] = static_cast<std::uint32_t>(TEXTURE_PACK(
				static_cast<t16>(floattot16(uv[0] * texW)),
				static_cast<t16>(floattot16(uv[1] * texH))));
		}
		if (wantNormal)
		{
			constexpr float kInvNormalScale = 1.0f / 127.0f;
			std::int8_t normal[3];
			std::memcpy(normal, vertex + mesh.normalOffset, sizeof(normal));
			out[outIdx++] = static_cast<std::uint32_t>(NORMAL_PACK(
				floattov10(normal[0] * kInvNormalScale),
				floattov10(normal[1] * kInvNormalScale),
				floattov10(normal[2] * kInvNormalScale)));
		}

		float position[3];
		std::memcpy(position, vertex, sizeof(position));
		const std::int32_t posV16[3] = {
			floattov16(position[0] * kInvVertexScale),
			floattov16(position[1] * kInvVertexScale),
			floattov16(position[2] * kInvVertexScale),
		};
		out[outIdx++] = (static_cast<std::uint32_t>(static_cast<std::uint16_t>(posV16[1])) << 16)
		       | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(posV16[0])) & 0xFFFFu);
		out[outIdx++] = static_cast<std::uint32_t>(static_cast<std::uint16_t>(posV16[2]));

		out += wordsPerVertex;
	}

	// CACHE COHERENCY: the loop above just wrote `scratch` through the ARM9's
	// data cache -- the DMA controller that reads it next sits on the system
	// bus and does NOT go through that cache, so without an explicit
	// flush-to-RAM here it could read stale/partial data sitting in dirty
	// cache lines instead of what was just written. This matters more here
	// than for the captured-mesh path below (whose compiledCommands buffer
	// was typically written whole frames earlier, giving the cache more
	// chances to have already evicted/written it back on its own by
	// coincidence): this buffer is read back by DMA within the same
	// function call it was written in, with nothing in between to force
	// that writeback.
	DC_FlushRange(scratch.data(), totalWords * sizeof(std::uint32_t));

	glPushMatrix();
	glScalef(kVertexScale, kVertexScale, kVertexScale);
	glBegin(glPrimitive);
	const std::uint64_t dmaWaitStartUs = PlatformCompat::getMonotonicMicros();
	while (dmaBusy(0) || dmaBusy(1) || dmaBusy(2) || dmaBusy(3))
		;
	dmaSetParams(0, scratch.data(), (void*)&GFX_FIFO, DMA_FIFO | static_cast<std::uint32_t>(totalWords));
	while (dmaBusy(0))
		;
	dsiRecordDmaWait((long long)(PlatformCompat::getMonotonicMicros() - dmaWaitStartUs));
	glEnd();
	glPopMatrix(1);

	return true;
}

// Dispatcher every call site in the engine actually reaches (via
// renderDrawInterleaved() below): try the packed-DMA path first, fall back
// to the per-vertex path for anything drawInterleavedMeshFast() declined.
bool drawInterleavedMesh(const RenderInterleavedMesh& mesh)
{
	if (drawInterleavedMeshFast(mesh))
		return true;
	return drawInterleavedMeshSlow(mesh);
}

} // namespace

// renderStaticMeshCreate/Destroy/Compile/Draw are NOT defined here: they are
// the generic platform/RenderStaticMesh.cpp implementation (shared by every
// backend that doesn't set PLATFORM_PERSISTENT_RENDER_MESH -- DSi doesn't;
// see PlatformConfig.h). platform/RenderStaticMesh.cpp is compiled
// unconditionally by Makefile.game's SOURCEDIRS (it has no per-backend
// filename suffix, so the RenderAPI_(PC|GL|GX_WII|GS_PS2).cpp exclusion
// grep never touches it). Defining them again here previously duplicated
// those four symbols and would fail at link time with a multiple-definition
// error against RenderStaticMesh.cpp's copies -- removed rather than kept as
// a second, coincidentally-identical implementation.

bool renderDrawInterleaved(const RenderInterleavedMesh& mesh)
{
	return drawInterleavedMesh(mesh);
}

// Verbatim byte-copy of the source interleaved buffer -- deliberately NOT
// pre-converting position here (see dsiRepackCapturedMeshStep() below for
// where that actually happens and why not here). This function has two
// different callers with two different expectations of its output format,
// and only one of them wants a converted result:
//   * platform/RenderStaticMesh.cpp's renderStaticMeshCompile(), for a
//     FINISHED mesh a caller is about to start replaying every frame --
//     this is the one dsiRepackCapturedMeshStep() targets, but it does so
//     as an explicit extra step the caller takes AFTER this returns (see
//     WorldRendererDsi.cpp's dsiBuildRendererStep()), not inside here.
//   * Tessellator::capture() (Tessellator.cpp), used by
//     WorldRendererDsi.cpp's incremental chunk builder purely to pull one
//     BOUNDED STEP's worth of vertices out of the live Tessellator buffer
//     into a vector it accumulates across many steps (dsiBuildRawBuffer) --
//     then re-wraps that accumulated buffer as a fresh RenderInterleavedMesh
//     with hardcoded stride/offsets (32/12/20/28, Tessellator's own fixed
//     layout) and feeds it back through renderStaticMeshCompile(). If this
//     function pre-converted position, that reused raw buffer would already
//     be in the converted format and the hardcoded offsets/stride on the
//     second pass would misread it -- silently, since both are just
//     same-sized int32 words to memcpy. Keeping this a pure byte mirror
//     keeps both callers' existing assumptions intact.
bool renderCaptureInterleaved(const RenderInterleavedMesh& mesh, RenderCapturedMesh& out, bool append)
{
	if (!mesh.data || mesh.count <= 0 || mesh.stride <= 0)
		return false;

	if (!append)
		out.clear();

	const std::size_t bytes = (std::size_t)mesh.count * mesh.stride;
	const std::size_t words = (bytes + sizeof(std::int32_t) - 1) / sizeof(std::int32_t);
	const std::size_t oldSize = out.raw.size();
	out.raw.resize(oldSize + words);
	std::memcpy(out.raw.data() + oldSize,
	            static_cast<const std::uint8_t*>(mesh.data) + (std::size_t)mesh.first * mesh.stride,
	            bytes);

	out.vertexCount += mesh.count;
	out.stride = mesh.stride;
	out.primitive = mesh.primitive;
	out.positionShort = mesh.positionShort;
	out.hasTexture = mesh.hasTexture;
	out.texCoordOffset = mesh.texCoordOffset;
	out.hasColor = mesh.hasColor;
	out.colorOffset = mesh.colorOffset;
	out.hasNormals = mesh.hasNormals;
	out.normalOffset = mesh.normalOffset;
	out.hasBrightness = mesh.hasBrightness;
	out.brightnessOffset = mesh.brightnessOffset;
	// positionIsV16 deliberately left at its default (false): see the
	// function banner above and dsiRepackCapturedMeshStep() below.

	return true;
}

// Converts a captured mesh's position field from float3 to an already-GPU-
// native v16 triple, and its texcoord field from normalized float2 to t16
// texel-space, in place -- called only by WorldRendererDsi.cpp, only on a
// section's finished, about-to-be-published mesh (dsiStagingMesh[pass].captured
// right after renderStaticMeshCompile() succeeds), never on the intermediate
// per-step buffers renderCaptureInterleaved() above also serves. Safe to call
// blind: positionIsV16/texCoordIsT16 make each half idempotent, and position
// is always exactly the first 12 bytes of every vertex in this engine's
// interleaved convention (there is no separate "position offset" field
// anywhere in RenderAPI.h -- drawInterleavedMesh() itself always reads it
// from vertex+0), so this changes nothing about the mesh's stride or any
// other field's offset: 3 floats in, 3 pre-widened v16 ints out, both exactly
// 12 bytes (texcoord: 2 floats/2 t16 ints, same idea).
//
// Why this matters: floattov16()/floattot16() (nds/arm9/videoGL.h: `(v16)((n)
// * (1<<12))`) are genuine float multiplies -- soft-float on this ARM9, which
// has no FPU at all (arm946e-s+nofp) -- and drawInterleavedMesh()'s
// glVertex3f()/glTexCoord2f() do that same work (position: 3 multiplies via
// kInvVertexScale then floattov16 again inside libnds; texcoord: scaled by
// the CURRENTLY BOUND texture's width/height, libnds's videoGL.c) for EVERY
// vertex, EVERY frame a mesh is drawn. A captured terrain section's geometry
// never changes between this call and the next rebuild (a block edit, or the
// section streaming in/out of render distance) -- typically hundreds of
// frames later -- so paying both conversions once here instead of every one
// of those frames is a straight win with no behaviour change: glVertex3v16()/
// glTexCoord2t16() (libnds's own doc comment on glVertex3f(): "Float version!
// Please, use glVertex3v16() instead.") reproduce the identical GPU-side
// value the float calls would have computed fresh each time. Texcoord is only
// safe to bake in here because the caller already confirmed this mesh is
// always drawn against terrainTextureId specifically (RenderExtraTerrainMeshes'
// CTM overlays rebind a different texture per group and must stay on the
// float glTexCoord2f() path instead -- see RenderCapturedMesh::texCoordIsT16's
// own comment) -- guarded on that texture actually being resident (width/
// height known); failing safe (leaving the float path in place) rather than
// baking in a bogus 0x0 scale is worth the one check.
//
// Once both conversions are done, also pre-packs the mesh's per-vertex GX
// FIFO command stream, so replaying it every frame is a single DMA transfer
// straight into the geometry engine's command port instead of this file's own
// per-vertex libnds call sequence in drawCapturedMeshFast() below
// (glColor3b()/glTexCoord2t16()/glVertex3v16(), each a real ARM9 function
// call with its own argument packing, run vertexCount times EVERY frame a
// section is drawn). Validated two ways before writing this, not guessed:
//   (1) libnds's own headers give the exact on-the-wire format the FIFO
//       register (GFX_FIFO, nds/arm9/video.h) expects: FIFO_COMMAND_PACK()
//       (nds/arm9/videoGL.h) packs up to 4 command IDs into one header word
//       (one byte per queued command, REG2ID()'d from each command
//       register's own address), immediately followed by each command's
//       parameter words in order -- COLOR takes 1 (glColor3b()'s
//       RGB15(r>>3,g>>3,b>>3)), TEX_COORD takes 1 (glTexCoord2t16()'s
//       TEXTURE_PACK(u,v)), VERTEX16 takes 2 (glVertex3v16()'s
//       ((u32)(u16)y<<16)|(x&0xFFFF) then z), NOP takes 0 -- so one
//       COLOR+TEX_COORD+VERTEX16+NOP record is exactly 5 words: the header
//       plus those 4 real parameter words.
//   (2) ClassiCube's own shipped, working DS/DSi backend
//       (github.com/ClassiCube/ClassiCube, src/nds/Graphics_NDS.c's
//       PreprocessTexturedVertices()/DSTexturedVertex/CallDrawList())
//       independently implements this exact technique, on the exact same
//       hardware, confirming both the format above and that this is a
//       real, proven win rather than a hopeful guess -- including the
//       detail that follows: ClassiCube's CallDrawList() does NOT go
//       through libnds's own glCallList() (which expects a leading word-
//       count prefix inside the list itself, since it can't otherwise know
//       how long a caller-built list is); it DMAs the pre-packed buffer
//       straight into GFX_FIFO via dmaSetParams(..., DMA_FIFO | wordCount),
//       passing the already-known word count directly instead of paying
//       for a prefix word this call site never needs. drawCapturedMeshFast()
//       below mirrors that exact DMA call, not glCallList().
//
// Fixed 5-word-per-vertex layout, one self-contained command header per
// vertex (matching ClassiCube's DSTexturedVertex struct, NOT this file's own
// emitColorIfChanged() run-length colour dedup in drawCapturedMeshFast()
// below): a per-vertex header must stand alone for a blind DMA replay to
// work, so a redundant GFX_COLOR word between two same-coloured vertices
// costs one packed word, not a function call -- cheap next to the per-vertex
// call overhead this removes. Only ever built for a mesh already fully
// converted to v16 position + t16 texcoord, with no normals (this path never
// emits a NORMAL command) and GL_QUADS primitives (this port's only
// captured-mesh shape).
//
// Budgeted and resumable across several calls (see
// DsiCapturedMeshRepack.h's own comment on this function for the full why --
// real-hardware evidence of a single-call "build" spike as high as 172ms when
// this used to run as one unconditional pass over a whole section's mesh --
// and the safety argument for calling it on a staging, unpublished mesh
// only): stage 0 does the position+texcoord conversion above, merged into one
// per-vertex pass (a given vertex's own conversion never depends on any
// other vertex, so nothing is lost combining them); stage 1 does the FIFO
// command compile above. The per-vertex colour/lightmap-combine logic in
// stage 1 below is kept in sync by hand with drawCapturedMeshFast()'s own
// identical loop (same existing precedent this file already follows for that
// pair) -- see that copy's own comments for why each branch exists.
bool dsiRepackCapturedMeshStep(RenderCapturedMesh& mesh, int terrainTextureId, int vertexBudget,
	int& stage, int& cursor)
{
	if (mesh.empty())
	{
		stage = 2;
		cursor = 0;
		return true;
	}
	if (stage >= 2)
		return true;
	if (vertexBudget <= 0)
		vertexBudget = mesh.vertexCount;

	std::uint8_t* raw = reinterpret_cast<std::uint8_t*>(mesh.raw.data());

	if (stage == 0)
	{
		// Texture residency is a per-mesh question, not a per-vertex one --
		// resolved once here, outside the per-vertex loop below.
		const DsiTexture* terrainTex = mesh.hasTexture ? textureSlot(terrainTextureId) : nullptr;
		const bool texResident = terrainTex && terrainTex->allocated && terrainTex->width > 0 && terrainTex->height > 0;
		const float texW = texResident ? static_cast<float>(terrainTex->width) : 0.0f;
		const float texH = texResident ? static_cast<float>(terrainTex->height) : 0.0f;

		const int endVertex = std::min(mesh.vertexCount, cursor + vertexBudget);
		for (int i = cursor; i < endVertex; ++i)
		{
			std::uint8_t* vertex = raw + (std::size_t)i * mesh.stride;

			if (!mesh.positionIsV16)
			{
				float position[3];
				std::memcpy(position, vertex, sizeof(position));
				const std::int32_t posV16[3] = {
					floattov16(position[0] * kInvVertexScale),
					floattov16(position[1] * kInvVertexScale),
					floattov16(position[2] * kInvVertexScale),
				};
				std::memcpy(vertex, posV16, sizeof(posV16));
			}

			if (mesh.hasTexture && texResident && !mesh.texCoordIsT16)
			{
				float uv[2];
				std::memcpy(uv, vertex + mesh.texCoordOffset, sizeof(uv));
				const std::int32_t uvT16[2] = {
					floattot16(uv[0] * texW),
					floattot16(uv[1] * texH),
				};
				std::memcpy(vertex + mesh.texCoordOffset, uvT16, sizeof(uvT16));
			}
		}
		cursor = endVertex;
		if (cursor < mesh.vertexCount)
			return false;

		mesh.positionIsV16 = true;
		if (mesh.hasTexture && texResident)
			mesh.texCoordIsT16 = true;
		// Texture not resident yet: fail safe -- texCoordIsT16 stays false,
		// this build's mesh keeps the (already v16-position) float-texcoord
		// draw path, and the next rebuild this section gets tries the
		// conversion again from scratch.
		stage = 1;
		cursor = 0;
		// Yield here rather than falling through to stage 1 in the same
		// call: keeps this call's added cost bounded to one stage's own
		// vertexBudget, the same shape as the per-block loop's own budget
		// above.
		return false;
	}

	// stage == 1: compile the GX FIFO command stream. Anything that fails
	// this eligibility guard (non-quad primitive, texture never came
	// resident above) has nothing left to do and falls back to the ordinary
	// per-vertex draw path forever for this build.
	if (mesh.primitive != RenderPrimitive::Quads || !mesh.positionIsV16 || !mesh.texCoordIsT16)
	{
		mesh.compiledCommands.clear();
		stage = 2;
		cursor = 0;
		return true;
	}

	// Entity/mob model meshes (ModelRenderer's cube-based ModelBox/TexturedQuad
	// geometry, cached the same "replay this many frames unchanged" way
	// terrain sections and the HUD caches are -- see ModelRenderer.cpp's own
	// PLATFORM_MODEL_IMMEDIATE comment) carry per-vertex normals; terrain
	// never does (block faces get their shading from hasBrightness/hasColor
	// instead, never from a GL normal -- confirmed by RenderBlocks.cpp never
	// calling setNormal()), so this branch is unreached for every terrain
	// mesh today and changes nothing about that already-shipped path. Six
	// words/vertex instead of five: NORMAL joins COLOR/TEX_COORD/VERTEX16 as
	// a fourth real packed command (replacing the no-normal record's FIFO_NOP
	// pad), still exactly filling FIFO_COMMAND_PACK's 4-command header, plus
	// its own one parameter word.
	const std::size_t wordsPerVertex = mesh.hasNormals ? 6 : 5;
	const std::uint32_t kHeader = mesh.hasNormals
		? FIFO_COMMAND_PACK(FIFO_COLOR, FIFO_TEX_COORD, FIFO_NORMAL, FIFO_VERTEX16)
		: FIFO_COMMAND_PACK(FIFO_COLOR, FIFO_TEX_COORD, FIFO_VERTEX16, FIFO_NOP);
	const int endVertex = std::min(mesh.vertexCount, cursor + vertexBudget);

	if (cursor == 0)
		mesh.compiledCommands.assign((std::size_t)mesh.vertexCount * wordsPerVertex, 0u);
	std::uint32_t* out = mesh.compiledCommands.data() + (std::size_t)cursor * wordsPerVertex;

	for (int i = cursor; i < endVertex; ++i)
	{
		const std::uint8_t* vertex = raw + (std::size_t)i * mesh.stride;

		std::uint8_t r = 255, g = 255, b = 255;
		bool haveLightmapColor = false;
		std::uint8_t lightmapR = 255, lightmapG = 255, lightmapB = 255;
		if (mesh.hasBrightness)
		{
			std::int32_t packedBrightness = 0;
			std::memcpy(&packedBrightness, vertex + mesh.brightnessOffset, sizeof(packedBrightness));
			const float lightU = static_cast<float>(packedBrightness & 0xffff) / 256.0f;
			const float lightV = static_cast<float>((packedBrightness >> 16) & 0xffff) / 256.0f;
			haveLightmapColor = lightmapColorAt(lightU, lightV, lightmapR, lightmapG, lightmapB);
		}
		if (mesh.hasColor)
		{
			std::uint8_t rgba[4];
			std::memcpy(rgba, vertex + mesh.colorOffset, sizeof(rgba));
			if (haveLightmapColor)
			{
				r = static_cast<std::uint8_t>((static_cast<int>(lightmapR) * rgba[0]) / 255);
				g = static_cast<std::uint8_t>((static_cast<int>(lightmapG) * rgba[1]) / 255);
				b = static_cast<std::uint8_t>((static_cast<int>(lightmapB) * rgba[2]) / 255);
			}
			else
			{
				r = rgba[0]; g = rgba[1]; b = rgba[2];
			}
		}
		else if (haveLightmapColor)
		{
			r = lightmapR; g = lightmapG; b = lightmapB;
		}

		std::int32_t uvT16[2];
		std::memcpy(uvT16, vertex + mesh.texCoordOffset, sizeof(uvT16));
		std::int32_t posV16[3];
		std::memcpy(posV16, vertex, sizeof(posV16));

		out[0] = kHeader;
		out[1] = static_cast<std::uint32_t>(RGB15(r >> 3, g >> 3, b >> 3));
		out[2] = static_cast<std::uint32_t>(TEXTURE_PACK(static_cast<t16>(uvT16[0]), static_cast<t16>(uvT16[1])));
		int outIdx = 3;
		if (mesh.hasNormals)
		{
			// Same int8 -> float -> v10 path glNormal3f() does at draw time
			// in the per-vertex fallback below (and in drawInterleavedMesh()
			// above) -- matched here bit-for-bit (not a shortcut from int8
			// straight to v10) so a mesh that becomes eligible for this fast
			// path shades identically to one that does not.
			constexpr float kInvNormalScale = 1.0f / 127.0f;
			std::int8_t normal[3];
			std::memcpy(normal, vertex + mesh.normalOffset, sizeof(normal));
			out[outIdx++] = static_cast<std::uint32_t>(NORMAL_PACK(
				floattov10(normal[0] * kInvNormalScale),
				floattov10(normal[1] * kInvNormalScale),
				floattov10(normal[2] * kInvNormalScale)));
		}
		out[outIdx++] = (static_cast<std::uint32_t>(static_cast<std::uint16_t>(posV16[1])) << 16)
		       | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(posV16[0])) & 0xFFFFu);
		out[outIdx++] = static_cast<std::uint32_t>(static_cast<std::uint16_t>(posV16[2]));
		out += wordsPerVertex;
	}
	cursor = endVertex;
	if (cursor < mesh.vertexCount)
		return false;

	stage = 2;
	cursor = 0;
	return true;
}

namespace
{

// Draw-time counterpart of drawInterleavedMesh() above, for captured meshes:
// identical per-vertex colour/lightmap/normal/texcoord handling (kept in
// sync by hand -- the two diverge only in how position reaches the GPU).
// mesh.positionIsV16 tells this which of two sources drove the capture: a
// section dsiRepackCapturedMeshStep() already converted (take the pre-
// converted v16 triple straight off the buffer, glVertex3v16(), no
// per-frame float math), or anything else that goes through the generic
// RenderStaticMesh path without that extra step -- RenderGlobal.cpp's sky/
// star meshes, GuiIngame.cpp's HUD caches -- which still carries plain
// float position and needs the same conversion drawInterleavedMesh() does.
bool drawCapturedMeshFast(const RenderCapturedMesh& mesh)
{
	if (mesh.empty())
		return false;

	const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(mesh.raw.data());

	// Two one-shot diagnostics that used to live here -- the original "terrain
	// has no texture pattern at all" check, and the grass/leaves grey-tint
	// brightness-decoding investigation -- were removed once both bugs they
	// were chasing were confirmed fixed by the user on real hardware. See git
	// history if either investigation is needed again.

	// Same translucency approximation as drawInterleavedMesh() -- see its
	// own comment on why first+last vertex alpha is sampled, not scanned.
	if (mesh.hasColor)
	{
		std::uint8_t alphaFirst;
		std::memcpy(&alphaFirst, base + mesh.colorOffset + 3, sizeof(alphaFirst));
		std::uint8_t alphaLast = alphaFirst;
		if (mesh.vertexCount > 1)
			std::memcpy(&alphaLast, base + (std::size_t)(mesh.vertexCount - 1) * mesh.stride + mesh.colorOffset + 3, sizeof(alphaLast));
		const unsigned int alphaSum = static_cast<unsigned int>(alphaFirst) + static_cast<unsigned int>(alphaLast);
		const float averageAlpha = static_cast<float>(alphaSum) / (255.0f * 2.0f);
		g_poly.alpha31 = static_cast<std::uint8_t>(averageAlpha * 31.0f + 0.5f);
		markPolyDirty();
	}

	applyPolyFormatIfDirty();

	GL_GLBEGIN_ENUM glPrimitive = GL_TRIANGLES;
	switch (mesh.primitive)
	{
		case RenderPrimitive::Triangles:
		case RenderPrimitive::TriangleStrip:
		case RenderPrimitive::TriangleFan:
			glPrimitive = GL_TRIANGLES;
			break;
		case RenderPrimitive::Quads:
			glPrimitive = GL_QUADS;
			break;
		default:
			return false;
	}

	glPushMatrix();
	glScalef(kVertexScale, kVertexScale, kVertexScale);

	if (!mesh.compiledCommands.empty())
	{
		// Fast path: dsiRepackCapturedMeshStep() (see its own banner comment
		// above) already pre-packed every vertex's COLOR+TEX_COORD+VERTEX16
		// command record once, back when this section's mesh was repacked --
		// replay it as a single DMA transfer straight into the geometry
		// engine's FIFO port instead of the per-vertex libnds call loop below
		// (which still handles every mesh this backend draws that ISN'T
		// eligible for pre-compilation: anything with normals, a non-quad
		// primitive, or not yet through dsiRepackCapturedMeshStep() at all).
		//
		// Mirrors ClassiCube's own CallDrawList() (src/nds/Graphics_NDS.c),
		// not libnds's higher-level glCallList(): glCallList() expects a
		// leading word-count prefix baked into the list, since it can't
		// otherwise know how long a general caller-built list is. This call
		// site already knows the exact word count -- mesh.compiledCommands
		// is always a whole number of fixed 5-word records, one per vertex,
		// by construction in dsiRepackCapturedMeshStep() above -- so it
		// skips that prefix and DMAs the buffer directly -- the same
		// technique, minus a redundant word, and
		// validated against ClassiCube's real, shipped implementation of
		// exactly this DMA call rather than assumed.
		//
		// CACHE COHERENCY FIX: this DMA reads compiledCommands straight off
		// the system bus, bypassing the ARM9's data cache -- any of its words
		// still sitting in a dirty cache line (last written by
		// dsiRepackCapturedMeshStep() or dsiAdvanceMeshRepack()'s swap,
		// possibly just one call earlier on the very frame a repack finishes)
		// would read stale/partial data instead of what was actually written.
		// Previously missing here; added alongside drawInterleavedMeshFast()
		// above, which has the identical gap for the same reason -- see that
		// function's own comment for the fuller explanation.
		DC_FlushRange(mesh.compiledCommands.data(), mesh.compiledCommands.size() * sizeof(std::uint32_t));
		glBegin(glPrimitive);
		const std::uint64_t dmaWaitStartUs = PlatformCompat::getMonotonicMicros();
		while (dmaBusy(0) || dmaBusy(1) || dmaBusy(2) || dmaBusy(3))
			;
		dmaSetParams(0, mesh.compiledCommands.data(), (void*)&GFX_FIFO,
		             DMA_FIFO | static_cast<std::uint32_t>(mesh.compiledCommands.size()));
		while (dmaBusy(0))
			;
		dsiRecordDmaWait((long long)(PlatformCompat::getMonotonicMicros() - dmaWaitStartUs));
		glEnd();
		glPopMatrix(1);
		return true;
	}

	bool haveLastColor = false;
	std::uint8_t lastColorR = 0, lastColorG = 0, lastColorB = 0;
	auto emitColorIfChanged = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b)
	{
		if (haveLastColor && r == lastColorR && g == lastColorG && b == lastColorB)
			return;
		glColor3b(r, g, b);
		lastColorR = r;
		lastColorG = g;
		lastColorB = b;
		haveLastColor = true;
	};

	const bool positionIsV16 = mesh.positionIsV16;
	const bool texCoordIsT16 = mesh.texCoordIsT16;

	glBegin(glPrimitive);
	for (int i = 0; i < mesh.vertexCount; ++i)
	{
		const std::uint8_t* vertex = base + (std::size_t)i * mesh.stride;

		bool haveLightmapColor = false;
		std::uint8_t lightmapR = 255, lightmapG = 255, lightmapB = 255;
		if (mesh.hasBrightness)
		{
			// See drawInterleavedMesh()'s identical block above for the full
			// explanation: this field is Tessellator's single packed
			// (skyLight << 20) | (blockLight << 4) int, not a float[2] u,v
			// pair -- reading 8 bytes here used to run past this field (into
			// the next vertex, or past the buffer's end for a mesh's last
			// vertex).
			std::int32_t packedBrightness = 0;
			std::memcpy(&packedBrightness, vertex + mesh.brightnessOffset, sizeof(packedBrightness));
			const float lightU = static_cast<float>(packedBrightness & 0xffff) / 256.0f;
			const float lightV = static_cast<float>((packedBrightness >> 16) & 0xffff) / 256.0f;
			haveLightmapColor = lightmapColorAt(lightU, lightV, lightmapR, lightmapG, lightmapB);
		}
		if (mesh.hasColor)
		{
			std::uint8_t rgba[4];
			std::memcpy(rgba, vertex + mesh.colorOffset, sizeof(rgba));
			if (haveLightmapColor)
			{
				emitColorIfChanged(static_cast<std::uint8_t>((static_cast<int>(lightmapR) * rgba[0]) / 255),
				                   static_cast<std::uint8_t>((static_cast<int>(lightmapG) * rgba[1]) / 255),
				                   static_cast<std::uint8_t>((static_cast<int>(lightmapB) * rgba[2]) / 255));
			}
			else
			{
				emitColorIfChanged(rgba[0], rgba[1], rgba[2]);
			}
		}
		else if (haveLightmapColor)
		{
			emitColorIfChanged(lightmapR, lightmapG, lightmapB);
		}
		else
		{
			// FIXED: a mesh with neither per-vertex colour nor a lightmap
			// value (mesh.hasColor and haveLightmapColor both false -- e.g.
			// GuiIngame.cpp's renderPlayerStatusHudGeometry(), which emits
			// hearts/food/armor icons via addVertexWithUV() alone and never
			// calls setColorOpaque()/setBrightness()) used to leave this
			// branch entirely unreached: no glColor3b() call at all, so
			// GFX_COLOR simply kept whatever value the previous, unrelated
			// draw call last set it to. This engine always runs in
			// POLY_MODULATION (applyPolyFormatIfDirty() above never sets
			// POLY_DECAL -- see its own comment), where
			// FinalColor = TexColor * VertexColor: if that inherited colour
			// happened to be dark or black (a plausible leftover from the
			// world's own terrain draw call immediately preceding the HUD
			// overlay pass), an otherwise-correct red heart/food icon
			// (icons.png's pixel data confirmed correct via this port's own
			// per-rect opacity scan) rendered solid black regardless of its
			// real texture colour -- the exact "hearts render black" report.
			// Vanilla OpenGL's own default vertex colour when glColor is
			// never called is opaque white (a no-op multiplier), so default
			// to that explicitly here instead of silently inheriting
			// whatever colour happens to still be resident from an earlier,
			// unrelated draw.
			emitColorIfChanged(255, 255, 255);
		}
		if (mesh.hasNormals)
		{
			constexpr float kInvNormalScale = 1.0f / 127.0f;
			std::int8_t normal[3];
			std::memcpy(normal, vertex + mesh.normalOffset, sizeof(normal));
			glNormal3f(normal[0] * kInvNormalScale, normal[1] * kInvNormalScale, normal[2] * kInvNormalScale);
		}
		if (mesh.hasTexture)
		{
			if (texCoordIsT16)
			{
				std::int32_t uvT16[2];
				std::memcpy(uvT16, vertex + mesh.texCoordOffset, sizeof(uvT16));
				glTexCoord2t16(static_cast<t16>(uvT16[0]), static_cast<t16>(uvT16[1]));
			}
			else
			{
				float uv[2];
				std::memcpy(uv, vertex + mesh.texCoordOffset, sizeof(uv));
				glTexCoord2f(uv[0], uv[1]);
			}
		}

		if (positionIsV16)
		{
			std::int32_t posV16[3];
			std::memcpy(posV16, vertex, sizeof(posV16));
			glVertex3v16(static_cast<v16>(posV16[0]), static_cast<v16>(posV16[1]), static_cast<v16>(posV16[2]));
		}
		else
		{
			float position[3];
			std::memcpy(position, vertex, sizeof(position));
			glVertex3f(position[0] * kInvVertexScale, position[1] * kInvVertexScale, position[2] * kInvVertexScale);
		}
	}
	glEnd();
	glPopMatrix(1);

	return true;
}

} // namespace

bool renderDrawCaptured(const RenderCapturedMesh& mesh)
{
	return drawCapturedMeshFast(mesh);
}

// -----------------------------------------------------------------------------
// State -- VERIFIED unless noted.
// -----------------------------------------------------------------------------

void renderEnable(RenderCapability capability)
{
	switch (capability)
	{
		case RenderCapability::Texture2D:
			if (g_texture2DEnabled == 1) break;
			g_texture2DEnabled = 1;
			glEnable(GL_TEXTURE_2D);
			break;
		case RenderCapability::AlphaTest:
			if (g_alphaTestEnabled == 1) break;
			g_alphaTestEnabled = 1;
			glEnable(GL_ALPHA_TEST);
			break;
		case RenderCapability::Blend:
			if (g_blendEnabled == 1) break;
			g_blendEnabled = 1;
			glEnable(GL_BLEND);
			break;
		case RenderCapability::Fog:
			g_poly.fogEnabled = true;
			markPolyDirty();
			if (g_fogEnabled == 1) break;
			g_fogEnabled = 1;
			glEnable(GL_FOG);
			break;
		case RenderCapability::CullFace:  g_poly.cullEnabled = true; markPolyDirty(); break;
		case RenderCapability::Light0:    g_poly.light0 = true; markPolyDirty(); break;
		case RenderCapability::Light1:    g_poly.light1 = true; markPolyDirty(); break;
		// APPROXIMATED: DS has no ColorMaterial/DepthTest-disable/Normalize/
		// RescaleNormal/PolygonOffsetFill/Lighting-as-a-toggle equivalents.
		// Lighting itself is implied by Light0/Light1 being on; the rest are
		// safe no-ops on this hardware (DepthTest is always on for opaque
		// polygons -- see renderDepthMask for the closest available control).
		default: break;
	}
}

void renderDisable(RenderCapability capability)
{
	switch (capability)
	{
		case RenderCapability::Texture2D:
			if (g_texture2DEnabled == 0) break;
			g_texture2DEnabled = 0;
			glDisable(GL_TEXTURE_2D);
			break;
		case RenderCapability::AlphaTest:
			if (g_alphaTestEnabled == 0) break;
			g_alphaTestEnabled = 0;
			glDisable(GL_ALPHA_TEST);
			break;
		case RenderCapability::Blend:
			if (g_blendEnabled == 0) break;
			g_blendEnabled = 0;
			glDisable(GL_BLEND);
			break;
		case RenderCapability::Fog:
			g_poly.fogEnabled = false;
			markPolyDirty();
			if (g_fogEnabled == 0) break;
			g_fogEnabled = 0;
			glDisable(GL_FOG);
			break;
		case RenderCapability::CullFace:  g_poly.cullEnabled = false; markPolyDirty(); break;
		case RenderCapability::Light0:    g_poly.light0 = false; markPolyDirty(); break;
		case RenderCapability::Light1:    g_poly.light1 = false; markPolyDirty(); break;
		default: break;
	}
}

// APPROXIMATED: the DS 3D engine has no generalised src/dst blend-factor
// pipeline, only "this polygon has alpha N" (POLY_ALPHA, applied through
// renderColor4f below) plus the GL_BLEND on/off toggle above. Minecraft only
// ever calls this with (SrcAlpha, OneMinusSrcAlpha) or (SrcAlpha,
// OneMinusSrcAlpha)-equivalent pairs for normal translucency, which is exactly
// what POLY_ALPHA already does, so the factors themselves are intentionally
// ignored rather than rejected.
void renderBlendFunc(RenderBlendFactor source, RenderBlendFactor destination)
{
	(void)source;
	(void)destination;
}

void renderDepthMask(bool enabled)
{
	g_poly.depthMaskEnabled = enabled;
	markPolyDirty();
}

// FIXED, then found to have over-corrected into a worse real-hardware bug --
// see the second half of this comment for the actual current behaviour.
//
// This used to be an unconditional no-op on the claim that "DS depth
// comparison is fixed at less-or-equal" -- checked against libnds's own
// videoGL.h, that is wrong: POLY_DEPTH_TEST_LESS (strictly less) is bit
// value 0, the hardware DEFAULT, and POLY_DEPTH_TEST_EQUAL is a separate,
// opt-in glPolyFmt bit -- there is no combined less-or-equal mode on this
// GPU at all, only these two.
//
// The first fix treated BOTH RenderCompare::Equal and RenderCompare::LessEqual
// as "turn POLY_DEPTH_TEST_EQUAL on". That is correct for Equal -- every call
// site that passes it (RenderLiving.cpp's armor glint, RenderDragon.cpp,
// ItemRenderer.cpp's glint pass) is deliberately asking for "accept only a
// fragment at the exact same depth as what's already there" and always pairs
// it with a LessEqual call right after to switch back. It is NOT correct for
// LessEqual on its own: that value is this codebase's overwhelmingly common
// BASELINE depth mode -- Minecraft.cpp's one-time startup call
// (renderEnable(DepthTest); renderDepthFunc(RenderCompare::LessEqual);,
// mirroring vanilla's GL11.glDepthFunc(GL_LEQUAL)), GuiIngame.cpp's
// resetOverlayGLState()/finishOverlayGLState() bracketing the entire HUD
// pass, GuiContainer.cpp's 3D item-icon depth sort, and every "switch back
// to normal" call after an explicit Equal pass -- none of which mean "stay
// in same-depth-only mode". Mapping LessEqual to POLY_DEPTH_TEST_EQUAL put
// the hardware in strict-equal mode for virtually the whole game, including
// the very first polygon drawn after any depth clear (which needs its depth
// to exactly equal GL_MAX_DEPTH, the clear value, to pass at all -- normal
// geometry never does). Real-hardware symptom: the OptiCraft splash/logo
// screens still displayed (LegacyStartup::run() draws them BEFORE
// Minecraft.cpp's startup block ever calls renderDepthFunc(), so depth test
// wasn't yet in its broken state for them), then the screen went solid black
// the moment the main menu tried to draw -- the panorama and every UI
// element failed this impossible-to-satisfy equal-depth test and never
// reached the colour/blend stage, while the CPU-side game loop kept ticking
// normally (nothing crashed; every fragment was just silently discarded).
//
// Fixed again: only Equal now drives POLY_DEPTH_TEST_EQUAL. LessEqual goes
// back to the hardware default (strictly less), the same behaviour this
// function had before it did anything at all. This reintroduces the
// original, narrower "hearts/food/xp/armor overlay at identical zLevel"
// invisibility this function was first written to fix -- GuiIngame.cpp's
// status-row draws rely on the ambient LessEqual mode alone (no explicit
// Equal bracket) to accept their same-depth overlay quads, and DSi bakes
// that whole row into one static mesh/one draw call, so it cannot toggle
// depth mode mid-draw the way RenderLiving.cpp etc. do around their own
// Equal passes. That narrower, purely cosmetic regression is fixed
// separately and locally in GuiIngame.cpp's dsiRenderPlayerStatusHud()/
// dsiRenderHotbarFrame() (depth test disabled for just those two draws --
// flat same-Z 2D icon rows have no real occlusion to resolve, so correct
// compositing only ever needed submission order plus alpha blending, not
// depth testing at all) rather than reintroducing this function's
// catastrophic whole-scene failure mode to chase it.
void renderDepthFunc(RenderCompare compare)
{
	const bool wantEqual = compare == RenderCompare::Equal;
	if (wantEqual == g_poly.depthTestEqual)
		return;
	g_poly.depthTestEqual = wantEqual;
	markPolyDirty();
}

void renderAlphaFunc(RenderCompare, float reference)
{
	// glAlphaFunc() takes a single 0-31 threshold and always compares as
	// "greater than"; RenderCompare's function is ignored because Minecraft
	// only ever requests alpha-greater-than-threshold (leaf/glass cutouts).
	int threshold = static_cast<int>(reference * 31.0f + 0.5f);
	if (threshold < 0) threshold = 0;
	if (threshold > 31) threshold = 31;
	glAlphaFunc(threshold);
}

void renderCullFace(RenderFace face)
{
	g_poly.cullFace = face; // FrontAndBack has no DS equivalent; treated as Back.
	markPolyDirty();
}

// APPROXIMATED: DS has no per-channel colour write mask; always writes all
// four. Minecraft only ever disables all four together (never a partial
// mask), so this degrades to "do nothing" rather than "wrong".
void renderColorMask(bool, bool, bool, bool)
{
}

// Defined below, near renderTextureSubImageRgba() (the deferred-upload
// mechanism it belongs to); forward-declared here so this function can call
// it. See DsiTexture::pendingUploadFlush's comment for why this exists.
//
// NOTE: this declaration must stay at file (external-linkage) scope, same as
// the real definition below -- it was originally placed a few hundred lines
// up, inside the first anonymous namespace, which gave IT internal linkage
// while the definition below (outside any anonymous namespace) has external
// linkage; two distinct symbols with the same name, so the call below bound
// to the (never-defined) internal one and the real definition went unused --
// caught by the DsiBringup.nds link step: "undefined reference to
// `(anonymous namespace)::dsiFlushPendingUpload(...)'".
void dsiFlushPendingUpload(int name, DsiTexture& tex);

// Real cost this avoids: WorldRendererDsi.cpp rebinds /terrain.png after
// every rendered section (its own comment: "Terrain state assumes /terrain.png
// is still bound after each section") -- up to ~18 times a frame for DSi's
// whole resident world, virtually always re-binding the SAME already-bound
// texture. Every one of those was an unconditional glBindTexture() -- a real
// GX-FIFO command write -- for no state change at all. Wii's equivalent path
// already skips this (WiiNativeDraw.cpp's loadTextureIfNeeded(), a memcmp
// against the last-loaded texture object); DSi never had the analogous
// guard. renderDeleteTextures() above keeps g_boundTexture from going stale
// across a delete-then-reuse-the-same-id cycle, so this comparison alone is
// enough to know whether the hardware is already showing this exact texture.
void renderBindTexture(int texture)
{
	const bool alreadyBound = texture > 0 && texture == g_boundTexture;
	g_boundTexture = texture;
	if (texture > 0 && !alreadyBound)
		glBindTexture(0, texture);

	// See DsiTexture::pendingUploadFlush's comment: a sub-image patch
	// (renderTextureSubImageRgba() above) only updates the CPU-side shadow
	// copy and marks this flag, deferring the real GPU re-upload to here --
	// the next time this exact texture is actually bound for drawing, which
	// naturally coalesces every patch a tick made into the one re-upload
	// that is ever visibly needed, and skips the re-upload entirely on a
	// tick where the patched texture (e.g. gui/items.png, only relevant
	// while an inventory-style screen is open) is never bound for drawing
	// at all.
	DsiTexture* tex = textureSlot(texture);
	if (tex && tex->pendingUploadFlush)
	{
		tex->pendingUploadFlush = false;
		dsiFlushPendingUpload(texture, *tex);
	}
}

void renderSetActiveTextureUnit(int textureUnit)
{
	// DS has one texture unit; remembered only so renderSetMultiTextureCoord
	// can tell the lightmap unit apart from the block-texture unit.
	g_lightmapUnit = (textureUnit != 0x84C0) ? textureUnit : -1;
}

void renderSetClientActiveTextureUnit(int)
{
}

// See the "Lightmap-as-vertex-colour" block above.
void renderSetMultiTextureCoord(int textureUnit, float u, float v)
{
	if (textureUnit == g_lightmapUnit || g_lightmapUnit == -1)
		applyLightmapColorAt(u, v);
}

void renderSetLightmapColors(const std::uint32_t* colors, int count)
{
	if (!colors || count <= 0)
	{
		g_lightmapColors.clear();
		g_lightmapSide = 0;
		return;
	}
	g_lightmapColors.assign(colors, colors + count);

	// Computed once here (once per frame -- see updateLightmap()'s call site)
	// instead of once per vertex in lightmapColorAt(), which is what this
	// value exists to avoid. count is always 256 in practice (EntityRenderer.cpp's
	// updateLightmap() hardcodes a 256-entry table), so this loop runs at
	// most 16 times a frame, not 16 times per vertex.
	int side = 0;
	while ((side + 1) * (side + 1) <= count)
		++side;
	g_lightmapSide = (side * side == count) ? side : 0;
}

void renderColor4f(float r, float g, float b, float a)
{
	g_poly.alpha31 = static_cast<std::uint8_t>(a * 31.0f + 0.5f);
	markPolyDirty();
	glColor3f(r, g, b);
}

void renderColor3f(float r, float g, float b)
{
	glColor3f(r, g, b);
}

void renderNormal3f(float x, float y, float z)
{
	glNormal3f(x, y, z);
}

void renderGenerateTextures(int count, int* textures)
{
	glGenTextures(count, textures);
	for (int i = 0; i < count; ++i)
		if (DsiTexture* tex = textureSlot(textures[i]))
			*tex = DsiTexture{};
}

void renderDeleteTextures(int count, const int* textures)
{
	std::vector<int> mutableCopy(textures, textures + count);
	glDeleteTextures(count, mutableCopy.data());
	for (int i = 0; i < count; ++i)
	{
		if (textures[i] > 0 && static_cast<std::size_t>(textures[i]) < g_textures.size())
			g_textures[textures[i]] = DsiTexture{};
		// A deleted name can be handed back out by a later glGenTextures()
		// (mob-skin eviction -- releaseTexturesWithPrefix() -- does exactly
		// this: delete now, lazily re-upload a DIFFERENT image under the
		// same reused id later). renderBindTexture()'s own dedup guard below
		// compares against this value; leaving it pointing at a deleted id
		// would make that guard wrongly skip the real glBindTexture() call
		// the first time that reused id is bound again, rendering with
		// whatever texture the hardware still happened to have loaded.
		if (textures[i] > 0 && textures[i] == g_boundTexture)
			g_boundTexture = 0;
	}
}

// The actual GPU re-upload half of a sub-image patch -- see DsiTexture::
// pendingUploadFlush's own comment for why this is now separated from the
// cheap CPU-side patch below and deferred to the texture's next real bind
// instead of running once per renderTextureSubImageRgba() call.
void dsiFlushPendingUpload(int name, DsiTexture& tex)
{
	// Real-hardware evidence (the vram= diagnostic added alongside this):
	// once VRAM pressure first forces a forceHighPrecision texture
	// (terrain.png) onto the paletted fallback, it never gets room back --
	// "high-precision upload failed for 256x256... vram=484/512KB" fired on
	// every single tick's water/lava/fire animation patch for the rest of a
	// whole session, hundreds of times in a row, the number never moving.
	// uploadTexture() tries the RGBA path first unconditionally for a
	// forceHighPrecision texture on every call regardless of whether the
	// exact same attempt failed a tick ago -- a wasted, doomed
	// glTexImage2D() call every tick with zero chance of succeeding while
	// nothing frees VRAM in between, before falling through to the exact
	// same tryUploadPaletted() call this reaches directly below anyway.
	// Skip straight to it once a texture is already resident as paletted: a
	// full reload (renderTextureImageRgba() -- a texture pack switch, the
	// resource's first load) still gets a fresh RGBA attempt from scratch,
	// this only stops a patch from re-litigating a fit that just failed a
	// moment ago for no reason it would not fail again this moment too.
	if (tex.paletted)
	{
		int param = 0;
		if (!tex.clamp)
			param |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
		// forceRebuild=false: reuses tex's already-established stable
		// palette (tryUploadWithStablePalette()) instead of re-deriving it
		// from this patch alone -- same reasoning uploadTexture()'s own
		// call below already documented, just reached without the doomed
		// RGBA attempt in front of it.
		tex.allocated = tryUploadPaletted(name, tex, param, false);
		return;
	}

	// Same dimensions as the already-successful initial upload, so this can't
	// newly fail the power-of-two check -- but propagate honestly anyway
	// rather than assume, in case VRAM pressure is what fails it this time.
	// forceRebuildPalette=false: reuse tex's already-established stable
	// palette instead of re-deriving it from this patch alone -- see
	// tryUploadWithStablePalette()'s comment for why.
	tex.allocated = uploadTexture(name, tex, false);
}

void renderTextureSubImageRgba(int level, int x, int y, int width, int height, const void* pixels)
{
	// Mip levels other than 0 are not tracked in the CPU-side shadow copy yet
	// (see the struct comment); only the base level can be patched correctly.
	if (level != 0)
		return;

	DsiTexture* tex = textureSlot(g_boundTexture);
	if (!tex || !tex->allocated || tex->rgba.empty())
		return;

	for (int row = 0; row < height; ++row)
	{
		const std::uint8_t* srcRow = static_cast<const std::uint8_t*>(pixels) + (std::size_t)row * width * 4;
		std::uint8_t* dstRow = tex->rgba.data() + ((std::size_t)(y + row) * tex->width + x) * 4;
		std::memcpy(dstRow, srcRow, (std::size_t)width * 4);
	}

	// Defer the expensive GPU re-upload to this texture's next real bind
	// (renderBindTexture() below) instead of doing it here -- see
	// DsiTexture::pendingUploadFlush's comment for the real-hardware cost
	// this batches away when several TextureFX patch the same texture
	// (typically terrain.png) within one updateDynamicTextures() tick.
	tex->pendingUploadFlush = true;
}

void renderTextureImageRgba(int level, int width, int height, const void* pixels)
{
	if (level != 0)
		return; // See the mip-level note above.

	DsiTexture* tex = textureSlot(g_boundTexture);
	if (!tex)
		return;

	tex->width = width;
	tex->height = height;
	tex->rgba.assign(static_cast<const std::uint8_t*>(pixels),
	                  static_cast<const std::uint8_t*>(pixels) + (std::size_t)width * height * 4);
	// A genuine full reload always uploads immediately below, so any pending
	// deferred patch upload (DsiTexture::pendingUploadFlush) this content
	// replaces is moot -- clear it rather than leave a stale flag that would
	// otherwise cost one redundant (harmless, but wasted) re-upload at this
	// texture's next bind.
	tex->pendingUploadFlush = false;
	// See uploadTexture()'s own comment: a real hardware upload can fail
	// (most likely a non-power-of-two source image) where the old code here
	// always reported success. renderTextureIsValid() reads this flag, and
	// RenderEngine.cpp's caller treats "invalid" the same as "failed to
	// decode" -- binding the checkerboard placeholder instead of leaving a
	// textureless polygon at its flat vertex colour.
	// forceRebuildPalette=true: this is a genuine full-image (re)load, so the
	// old stable palette (if any -- e.g. a texture pack switch reusing this
	// same GL texture name) may not apply to this content at all.
	tex->allocated = uploadTexture(g_boundTexture, *tex, true);
}

// Real-hardware symptom this fixes: once the panorama texture upload above
// started actually succeeding, the main menu collapsed to roughly one
// redraw every few seconds, with a white line visibly "climbing" the
// screen as each one happened. legacyDrawPanorama() calls this every
// single frame with the same fixed arguments (true, false, true) purely to
// (re-)apply wrap/clamp -- but this used to call the full uploadTexture()
// (CPU RGBA->DS pixel-format conversion over every pixel, then a real
// glTexImage2D VRAM DMA of the whole texture) on every one of those calls.
// While the panorama upload was still silently failing (before the fix
// above), that cost nothing: glTexImageNtr2D() rejects a non-power-of-two
// size in its first few instructions, before touching VRAM. Once it
// started succeeding, "every frame" became "one full texture reupload to
// VRAM every frame" -- for a background image, not a 2-frame animated
// tile -- which is exactly the kind of sustained VRAM bus traffic that
// would visibly race the display controller's own scanout of that same
// VRAM bank, matching the reported "line climbing the screen" artifact.
//
// Wrap mode is a texture FORMAT FLAG (GL_TEXTURE_WRAP_S/T, set via
// glTexParameter() below), not pixel data -- libnds's glTexParameter()
// (nds/arm9/video/videoGL.c) just ORs a few bits into the active texture's
// already-uploaded format register, no VRAM write at all. So re-applying
// wrap mode every frame never needed a reupload in the first place; it
// only reupload-ed because uploadTexture() bundled both operations
// together. "blur" is tracked in DsiTexture but never turned into a GPU
// parameter bit anywhere in this file (bilinear filtering isn't wired up
// on this backend yet), so there is nothing to reapply for it here either.
void renderTextureParameters(bool blur, bool, bool clamp)
{
	if (DsiTexture* tex = textureSlot(g_boundTexture))
	{
		tex->blur = blur;
		tex->clamp = clamp;
		if (tex->allocated)
		{
			int param = 0;
			if (!tex->clamp)
				param |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
			glBindTexture(0, g_boundTexture);
			glTexParameter(0, param);
		}
	}
}

int renderGetMaxAnisotropy() { return 1; } // No anisotropic filtering on this hardware.
int renderGetMaxSamples() { return 1; }    // No MSAA on this hardware.

bool renderTextureBeginUpload(int texture, int width, int height, int, bool blur, bool clamp, bool, bool highPrecision)
{
	renderBindTexture(texture);
	DsiTexture* tex = textureSlot(texture);
	if (!tex)
		return false;
	tex->width = width;
	tex->height = height;
	tex->blur = blur;
	tex->clamp = clamp;
	tex->forceHighPrecision = highPrecision;
	return true;
}

bool renderTextureIsValid(int texture)
{
	DsiTexture* tex = textureSlot(texture);
	return tex && tex->allocated;
}

void renderResetResources()
{
	glResetTextures();
	g_textures.clear();
	g_boundTexture = 0;
}

// APPROXIMATED -- see the fog comment block above; only colour is forwarded
// for now, not the density table.
void renderFogf(RenderFogParameter parameter, float value)
{
	switch (parameter)
	{
		case RenderFogParameter::Density: g_fogDensity = value; break;
		case RenderFogParameter::Start:   g_fogStart = value; break;
		case RenderFogParameter::End:     g_fogEnd = value; break;
		default: break;
	}
}

void renderFogi(RenderFogParameter, RenderFogMode)
{
	// DS fog has no exp/exp2/linear mode switch -- it is whatever shape the
	// density table encodes. See the fog comment block above.
}

void renderFogColor(const float* values)
{
	const auto toChannel = [](float v) -> std::uint8_t
	{
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		return static_cast<std::uint8_t>(v * 31.0f + 0.5f);
	};
	glFogColor(toChannel(values[0]), toChannel(values[1]), toChannel(values[2]), 31);
}

// APPROXIMATED: DS lighting is direction-only (no positional/attenuated
// lights) and takes one packed colour per light, not separate ambient/
// diffuse/specular terms -- see the glLight() doc in videoGL.h. Diffuse is
// forwarded as that colour since it is the dominant visual term for the flat-
// shaded blocky lighting Minecraft's console ports already use elsewhere;
// ambient/specular/position are accepted but not separately representable.
void renderLightfv(int lightIndex, RenderLightParameter parameter, const float* values)
{
	if (lightIndex != 0 && lightIndex != 1)
		return;

	if (parameter == RenderLightParameter::Diffuse)
	{
		const rgb color = RGB15(
			static_cast<int>(values[0] * 31.0f),
			static_cast<int>(values[1] * 31.0f),
			static_cast<int>(values[2] * 31.0f));
		// Same dedup shape as renderColorMaterial()'s own glMaterialf guard
		// just above: this function's only caller, RenderHelper::
		// enableStandardItemLighting(), passes the same cached, lazily-
		// computed light0Diffuse/light1Diffuse every single time it runs
		// (at least once a frame for as long as anything lit is on screen),
		// and nothing else anywhere in this codebase ever calls this with a
		// different colour for either light index. A real glLight() past
		// the first call per index is pure repetition.
		static rgb s_dsiLastLightColor[2] = {0xFFFFu, 0xFFFFu}; // sentinel: no valid RGB15 is all-1s
		if (s_dsiLastLightColor[lightIndex] == color)
			return;
		s_dsiLastLightColor[lightIndex] = color;
		// Direction defaults to "straight down" until a Position update
		// supplies a real one; good enough for a first pass on top-down
		// block lighting.
		glLight(lightIndex, color, 0, floattov10(-1.0f), 0);
	}
}

void renderLightModelAmbient(const float*)
{
	// No global ambient term on this hardware's fixed lighting model.
}

void renderColorMaterial(RenderFace, RenderColorMaterialMode mode)
{
	// DS materials are set with glMaterialf() per material type, not toggled
	// on/off per face the way desktop GL_COLOR_MATERIAL is, so this was left
	// a no-op rather than a guess -- but that no-op had a real consequence
	// this round's real-hardware report traced down: glMaterialf() is never
	// called ANYWHERE else in this codebase either, and libnds's own
	// glMaterialf() (videoGL.c) only ever writes GFX_DIFFUSE_AMBIENT /
	// GFX_SPECULAR_EMISSION when called -- its static diffuse_ambient/
	// specular_emission locals both start at 0, and the hardware register is
	// left untouched (at the GPU's own true zero) otherwise. RenderHelper.cpp's
	// enableStandardItemLighting() -- shared by every entity (RenderLiving),
	// the held item (ItemRenderer), and every GUI item/block icon
	// (GuiContainer/GuiInventory/GuiEnchantment/...) -- turns Light0/Light1 on
	// and calls exactly this function expecting it to establish SOME usable
	// material, the role glColorMaterial(..., GL_AMBIENT_AND_DIFFUSE) plays on
	// desktop. Left a pure no-op, material diffuse AND ambient stayed at zero
	// for the whole session: Emission (also never set) + Ambient_light*0 +
	// Diffuse_light*0*max(N.L,0) = 0,0,0 regardless of the light colours
	// RenderHelper.cpp computes or the vertex/texture colour underneath --
	// every lit draw this mechanism touches came out solid black. Matches
	// exactly: mobs in-world, the held item, and GUI item/block icons all
	// share this one lighting path, and nothing else that draws (terrain,
	// particles, text) ever enables Light0/Light1 at all.
	//
	// Fix: set the material to white on the one mode this engine actually
	// uses this way (AmbientAndDiffuse). White makes the material a no-op
	// multiplier -- the closest this hardware has to "let the light colour
	// and vertex/texture colour through unscaled", not pixel-identical to
	// desktop's per-vertex-colour-tracking GL_COLOR_MATERIAL, but turns
	// "always black" into actually shaded and visible, which is what every
	// caller here needs. RenderColorMaterialMode::Ambient (EntityRenderer.cpp's
	// fogMode==999 lava/suffocation overlay) does not enable Light0/Light1 at
	// all, so it is left alone rather than guessed at.
	//
	// Same dedup shape as g_boundTexture/g_texture2DEnabled/g_fogEnabled
	// above: RenderHelper::enableStandardItemLighting() -- the only caller
	// that ever reaches this with AmbientAndDiffuse -- runs at least once a
	// frame for as long as anything lit is on screen (world entities, the
	// held item, an open inventory/container/enchanting/stats/achievements
	// screen), every single one issuing this exact same white value with no
	// other call anywhere in this codebase ever writing a different one. A
	// real glMaterialf() past the first call is pure repetition.
	static bool s_dsiMaterialIsWhite = false;
	if (mode == RenderColorMaterialMode::AmbientAndDiffuse && !s_dsiMaterialIsWhite)
	{
		glMaterialf(GL_AMBIENT_AND_DIFFUSE, RGB15(31, 31, 31));
		s_dsiMaterialIsWhite = true;
	}
}

void renderShadeModel(RenderShadeModel model)
{
	g_poly.shade = model;
	markPolyDirty();
}

void renderClear(unsigned int mask)
{
	// libnds clears both colour and depth every frame as part of the render
	// pipeline itself (there is no separate "clear now" call); RenderClearMask
	// bits are accepted for API parity but the actual clearing happens via
	// renderClearColor()/renderClearDepth() below, applied at the next
	// glFlush(). Reject nothing: a caller clearing only Depth or only Color on
	// this hardware still gets both, which is a behavior difference worth
	// knowing about once something visible needs just one of them cleared.
	//
	// CONFIRMED real-hardware consequence (traced this round): EntityRenderer.cpp
	// calls renderClear(Depth) between drawing the world/HUD and drawing the
	// current GuiScreen, on every other backend genuinely resetting the depth
	// buffer there so the screen is never occluded by that same frame's 3D
	// geometry. On this backend that call does nothing -- the whole frame's
	// world+HUD+screen geometry shares ONE depth buffer pass, cleared only
	// once, at the next glFlush(). There is no cheap mid-frame "clear now" on
	// this GPU (the rear-plane/clear values apply once, at frame start) -- a
	// real fix would need a manual full-viewport depth-only quad draw here
	// (write depth, disable colour write) instead of trusting this no-op.
	//
	// GuiIngame.cpp's hotbar item icons staying visible over the pause menu/
	// creative inventory drawn afterward was SUSPECTED to be exactly this
	// (icons write depth with the test on, never cleared, screen's own redraw
	// fails against it) -- but bracketing the icon loop with renderEnable/
	// renderDisable(DepthTest) the way GuiContainer.cpp does for the same
	// class of draw was confirmed by the user to change nothing, and tracing
	// why found renderEnable/renderDisable(DepthTest) and renderDepthFunc()
	// are BOTH unconditional no-ops on this backend (opaque polygons always
	// depth-test at a fixed less-or-equal comparison; see those functions'
	// own comments) -- so DepthTest was never actually toggled by that fix in
	// the first place, on or off. This no-op clear is still real and still
	// worth knowing about for whatever the actual mechanism turns out to be,
	// but is NOT confirmed to be that mechanism for the hotbar case
	// specifically; see GuiIngame.cpp's own comment for the current best lead
	// (a Z-value mismatch between the icon's 3D geometry and the screen's
	// flat 2D overlay, not a depth-STATE leak).
	(void)mask;
}

void renderFinishGpu()
{
	// glFlush() below is already the synchronization point (it waits for
	// vblank); nothing extra to force here.
}

void renderSubmitFrame()
{
	// See RenderAPI.h's own doc comment on this function: DS has no
	// asynchronous present to kick off early, so this is a no-op and the
	// caller's own swap (glFlush()) remains the real synchronization point.
	// Called exactly once per frame, which is why the DMA-wait diagnostic's
	// own 20-frame-averaged report (see this file's top-of-file comment on
	// g_dsiDmaWaitSumUs) is flushed from here.
	dsiReportDmaWaitIfDue();
}

// ASSUMED (real-hardware report: in-world sky renders solid black instead of
// the expected day-sky blue; needs confirmation like everything else tagged
// ASSUMED in this file). Root cause traced by reading, not by a hardware
// probe -- not yet independently confirmed by a real-hardware A/B test.
//
// This backend's caller (EntityRenderer.cpp's updateFogColor(), shared,
// vanilla, cross-platform code -- not DSi-specific) calls this every frame
// as `renderClearColor(fogColorRed, fogColorGreen, fogColorBlue, 0.0f)`:
// alpha is ALWAYS 0. On every other backend that is harmless -- clear-colour
// alpha is not part of what ends up on screen, since each of those is
// either a single-layer framebuffer or otherwise ignores it. It is NOT
// harmless here: glClearColor()'s alpha sets the DS 3D engine's REAR-PLANE
// alpha (GFX_CLEAR_COLOR, nds/arm9/video.h), and the rear plane is what the
// hardware shows wherever no polygon was actually drawn -- exactly the open
// sky above the world once render distance is >=2 and RenderGlobal::
// renderSky()'s dome mesh is skipped (see that function's own
// `renderDistance < 2` gate; DSi inherits PS2's PLATFORM_DEFAULT_RENDER_
// DISTANCE=3, so that dome is never drawn on this platform -- the "sky" the
// player sees the rest of the time really is just this rear plane). Engine
// A composites that alpha against whatever 2D background layer sits behind
// the 3D layer, and dsiEnsureEarlyVideo() (DsiEarlyVideo.cpp) never sets one
// up for the top/main screen (MODE_0_3D, no bgInit() call) -- the exact same
// "nothing to composite against" gap that same file's own comment already
// documents for why the BOTTOM screen reads flat black. An alpha-0 rear
// plane compositing against nothing very plausibly falls through to that
// same hardware black, regardless of what colour was requested -- matching
// the report exactly (grass/trees/water still show correctly, since those
// are real opaque polygons with their own alpha, untouched by this).
//
// dsiEnsureEarlyVideo()'s own one-time startup clear
// (`glClearColor(0, 0, 0, 31)`) already uses a real alpha (31, opaque) for
// exactly this reason, which is what this fix makes every frame's clear
// consistent with: DSi's rear plane has nothing behind it to ever
// legitimately blend with, so it should always be fully opaque regardless
// of what alpha a cross-platform caller happens to pass in.
void renderClearColor(float r, float g, float b, float a)
{
	(void)a;
	const auto toChannel = [](float v) -> std::uint8_t
	{
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		return static_cast<std::uint8_t>(v * 31.0f + 0.5f);
	};
	glClearColor(toChannel(r), toChannel(g), toChannel(b), 31);
}

void renderClearDepth(double depth)
{
	if (depth < 0.0) depth = 0.0;
	if (depth > 1.0) depth = 1.0;
	glClearDepth(static_cast<fixed12d3>(depth * GL_MAX_DEPTH));
}

void renderPolygonOffset(float, float)
{
	// No polygon-offset-fill equivalent on this hardware's rasterizer.
}

void renderLineWidth(float)
{
	// DS lines are always 1 pixel wide; no width control exists.
}

void renderViewport(int x, int y, int width, int height)
{
	glViewport(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y),
	           static_cast<std::uint8_t>(x + width - 1), static_cast<std::uint8_t>(y + height - 1));
}

void renderGetViewport(int* values)
{
	// glGetInt(GL_GET_VIEWPORT, ...) is a wider libnds call libnds exposes
	// for GFX_VIEWPORT read-back; not wired up because nothing in the shared
	// engine currently calls renderGetViewport() outside of screenshot code
	// this backend does not implement (PLATFORM_FRAMEBUFFER_READBACK is 0 for
	// DSi -- see PlatformConfig.h). Returns 0s rather than uninitialised
	// memory if that changes.
	values[0] = values[1] = values[2] = values[3] = 0;
}

void renderGetMatrix(RenderMatrixQuery query, float* values)
{
	GL_GET_ENUM which = GL_GET_MATRIX_POSITION;
	switch (query)
	{
		case RenderMatrixQuery::ModelView:  which = GL_GET_MATRIX_POSITION; break;
		case RenderMatrixQuery::Projection: which = GL_GET_MATRIX_PROJECTION; break;
		case RenderMatrixQuery::Texture:    which = GL_GET_MATRIX_POSITION; break; // No texture-matrix readback on this hardware.
	}
	int fixed[16];
	glGetFixed(which, fixed);
	for (int i = 0; i < 16; ++i)
		values[i] = static_cast<float>(fixed[i]) / (1 << 12); // f32 -> float
}

const unsigned char* renderGetString(RenderStringQuery query)
{
	switch (query)
	{
		case RenderStringQuery::Vendor:   return reinterpret_cast<const unsigned char*>("Nintendo");
		case RenderStringQuery::Renderer: return reinterpret_cast<const unsigned char*>("DS/DSi 3D engine (BlocksDS libnds)");
		case RenderStringQuery::Version:  return reinterpret_cast<const unsigned char*>("1.0 DSi");
		case RenderStringQuery::Extensions: return reinterpret_cast<const unsigned char*>("");
	}
	return reinterpret_cast<const unsigned char*>("");
}

bool renderSupportsFeature(RenderFeature feature)
{
	switch (feature)
	{
		// Real-hardware bug found this round, likely contributing to the
		// "blocks show a flat colour, no texture detail" reports: this used
		// to claim Mipmaps support because glTexImageNtr2D() (the libnds
		// function) CAN take mip levels -- but nothing in this file actually
		// wires that up. renderTextureBeginUpload()'s nativeMaxLevel argument
		// is unnamed/ignored, uploadTexture()'s glTexImage2D() calls never
		// set any mip-count bit in their param, and renderTextureImageRgba()
		// explicitly drops every call with level != 0. Claiming support here
		// is what made Config::getMipmapLevel() return a real nonzero level
		// at all: DSi has no PLATFORM_DEFAULT_MIPMAP_LEVEL override of its
		// own (see PlatformGameTuning.h's `#if PLATFORM_PS2 || PLATFORM_DSI`
		// block), so it silently inherited PS2's default of 2 -- a value
		// that only makes sense on PS2's real native mip-level upload path.
		// On DSi that default did two things, both bad: RenderEngine.cpp
		// spent real CPU building a 2-level downsampled mip chain for
		// terrain.png/gui/items.png on every load, for levels this backend
		// then throws away unread (wasted work, not what the user asked to
		// optimize but exactly that kind of cost); and it left the DS GPU's
		// texture object configured with size/format bytes computed off an
		// unfulfilled multi-level expectation instead of the single flat
		// level this backend actually uploads, which is the kind of
		// mismatch that produces wrong/flat-looking sampled colour, not
		// missing geometry or a crash -- consistent with what was reported.
		// Reporting no support here makes Config::getMipmapLevel() return 0
		// on DSi regardless of the inherited default, the honest answer
		// until mip levels are genuinely uploaded here.
		case RenderFeature::Mipmaps: return false;
		default: return false; // No fancy fog distance, occlusion queries, anisotropic filtering or MSAA.
	}
}

unsigned int renderGetError()
{
	return 0; // libnds's GL wrapper has no glGetError()-equivalent to forward.
}

void renderFogHint(RenderHintMode)
{
	// No fastest/nicest fog quality switch on this hardware.
}

void renderMatrixMode(RenderMatrixMode mode)
{
	switch (mode)
	{
		case RenderMatrixMode::ModelView:  glMatrixMode(GL_MODELVIEW); break;
		case RenderMatrixMode::Projection: glMatrixMode(GL_PROJECTION); break;
		case RenderMatrixMode::Texture:    glMatrixMode(GL_TEXTURE); break;
	}
}

void renderLoadIdentity() { glLoadIdentity(); }
void renderPushMatrix() { glPushMatrix(); }
// glPopMatrix() takes a count (how many matrices to pop); RenderAPI.h's
// renderPopMatrix() always pops exactly one, matching every call site in the
// shared engine, which always pairs one renderPushMatrix() with one
// renderPopMatrix().
void renderPopMatrix() { glPopMatrix(1); }
void renderTranslate(float x, float y, float z) { glTranslatef(x, y, z); }
void renderRotate(float angle, float x, float y, float z) { glRotatef(angle, x, y, z); }
void renderScale(float x, float y, float z) { glScalef(x, y, z); }
void renderScaleDouble(double x, double y, double z)
{
	glScalef(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}
void renderFrustum(double left, double right, double bottom, double top, double nearValue, double farValue)
{
	glFrustumf32(floattof32((float)left), floattof32((float)right), floattof32((float)bottom),
	             floattof32((float)top), floattof32((float)nearValue), floattof32((float)farValue));
}
void renderOrtho(double left, double right, double bottom, double top, double nearValue, double farValue)
{
	glOrthof32(floattof32((float)left), floattof32((float)right), floattof32((float)bottom),
	           floattof32((float)top), floattof32((float)nearValue), floattof32((float)farValue));
}

bool renderCopyFramebufferToBoundTexture(int, int, int, int)
{
	// No framebuffer-to-texture copy path implemented yet; nothing in the
	// shared engine calls this unless a backend advertises support for it
	// through renderSupportsFeature(), which this one does not.
	return false;
}

void renderSetLegacyPresentationGamma(bool)
{
	// See the doc comment on this function in RenderAPI.h: Wii maps it to a
	// GX copy gamma, PS2 to its LUT fallback. Neither has an equivalent wired
	// up here yet; the DS 3D engine's own gamma/brightness fade registers
	// (see nds/arm9/video.h) are the place to add it if the Legacy Look
	// preset needs it later.
}

#endif // DSI_PLATFORM
