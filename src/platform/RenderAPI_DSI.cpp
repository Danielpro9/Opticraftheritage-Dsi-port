#ifdef DSI_PLATFORM

#include "platform/RenderAPI.h"
#include "platform/Log.h"
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

	std::vector<std::int16_t> colorToIndex(32768, -1);
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
			if (quantShift > 0)
				MC_LOG_WARN("dsi", "paletted upload used colour quantization (shift=%d, %u colours) to fit: %dx%d id=%d\n",
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
		MC_LOG_WARN("dsi", "palette overflow at shift=%d: %dx%d texture exceeds 255 distinct opaque colours id=%d%s\n",
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

// -----------------------------------------------------------------------------
// Static / captured mesh replay -- VERIFIED per-call mapping (glBegin/
// glVertex3f/glTexCoord2f/glColor3b), APPROXIMATED overall performance: this
// still re-issues one GL call per vertex per frame instead of compiling to a
// native GPU command list (glCallList() takes a pre-packed FIFO buffer this
// engine does not build -- that would be the next step up, replaying a whole
// section with one async DMA'd call instead of N immediate-mode ones), the
// same limitation PS2's non-native-terrain-pipeline path has (see
// PLATFORM_NATIVE_TERRAIN_PIPELINE in PlatformConfig.h, left off for DSi).
// What IS precomputed now: a captured mesh's position data is converted to
// the GPU's native v16 fixed-point format once, when the mesh is captured
// (a chunk build), not on every one of the frames it is replayed across
// before the next rebuild -- see renderCaptureInterleaved()/
// drawCapturedMeshFast() below for why and the exact split.
// -----------------------------------------------------------------------------
namespace
{

// Every vertex position this engine submits goes through the same v16
// pre-scale dance -- see the full story where drawInterleavedMesh() uses
// these below. Hoisted to file scope so renderCaptureInterleaved()'s
// capture-time conversion (below) and this function's own draw-time
// conversion (for one-shot/dynamic meshes, which have no earlier "capture"
// step to move the cost into) apply the exact same constants.
constexpr float kVertexScale = 256.0f;
constexpr float kInvVertexScale = 1.0f / kVertexScale;

// A Minecraft interleaved vertex is always float3 position, float2 texcoord,
// RGBA8 colour, signed-byte3 normal, in whatever subset the mesh's hasTexture/
// hasColor/hasNormals/hasBrightness flags declare -- see RenderInterleavedMesh
// in RenderAPI.h. renderDrawInterleaved()'s job is to walk that buffer and
// issue one glVertex3f() (plus whatever glTexCoord2f()/glColor3b()/glNormal3f()
// precede it) per vertex.
bool drawInterleavedMesh(const RenderInterleavedMesh& mesh)
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
// pre-converting position here (see dsiRepackCapturedMeshFast() below for
// where that actually happens and why not here). This function has two
// different callers with two different expectations of its output format,
// and only one of them wants a converted result:
//   * platform/RenderStaticMesh.cpp's renderStaticMeshCompile(), for a
//     FINISHED mesh a caller is about to start replaying every frame --
//     this is the one dsiRepackCapturedMeshFast() targets, but it does so
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
	// function banner above and dsiRepackCapturedMeshFast() below.

	return true;
}

// The actual optimization: converts a captured mesh's position field from
// float3 to an already-GPU-native v16 triple, in place, ONE TIME -- called
// only by WorldRendererDsi.cpp, only on a section's finished, about-to-be-
// published mesh (dsiStagingMesh[pass].captured right after
// renderStaticMeshCompile() succeeds), never on the intermediate per-step
// buffers renderCaptureInterleaved() above also serves. Safe to call blind:
// positionIsV16 makes it idempotent, and position is always exactly the
// first 12 bytes of every vertex in this engine's interleaved convention
// (there is no separate "position offset" field anywhere in RenderAPI.h --
// drawInterleavedMesh() itself always reads it from vertex+0), so this
// changes nothing about the mesh's stride or any other field's offset: 3
// floats in, 3 pre-widened v16 ints out, both exactly 12 bytes.
//
// Why this matters: floattov16() (nds/arm9/videoGL.h: `(v16)((n) * (1<<12))`)
// is a genuine float multiply -- soft-float on this ARM9, which has no FPU
// at all (arm946e-s+nofp) -- and drawInterleavedMesh()'s glVertex3f() does
// three of them (via kInvVertexScale then floattov16 again inside libnds)
// for EVERY vertex, EVERY frame a mesh is drawn. A captured terrain
// section's positions never change between this call and the next rebuild
// (a block edit, or the section streaming in/out of render distance) --
// typically hundreds of frames later -- so paying that conversion once here
// instead of every one of those frames is a straight win with no behaviour
// change: glVertex3v16() (libnds's own doc comment on glVertex3f(): "Float
// version! Please, use glVertex3v16() instead.") reproduces the identical
// GPU-side value from the pre-converted ints that glVertex3f() would have
// computed fresh each time.
//
// Texture coordinates are NOT converted the same way here: glTexCoord2f()
// scales by the CURRENTLY BOUND texture's width/height (libnds's videoGL.c),
// which is not necessarily fixed at repack time for every captured mesh
// this engine has (RenderExtraTerrainMeshes rebinds a different texture per
// group) -- baking that in now without also plumbing the bound texture's
// size through would be a real, if less frequent, correctness risk. Left as
// the float path (drawCapturedMeshFast() below still calls glTexCoord2f()
// per vertex) -- a real but smaller remaining cost than position (1 call vs.
// 3 per vertex) and the next thing to revisit if a texture-size-aware
// version of this is worth the added complexity.
void dsiRepackCapturedMeshFast(RenderCapturedMesh& mesh, int terrainTextureId)
{
	if (mesh.empty())
		return;

	std::uint8_t* raw = reinterpret_cast<std::uint8_t*>(mesh.raw.data());

	if (!mesh.positionIsV16)
	{
		for (int i = 0; i < mesh.vertexCount; ++i)
		{
			std::uint8_t* vertex = raw + (std::size_t)i * mesh.stride;
			float position[3];
			std::memcpy(position, vertex, sizeof(position));
			const std::int32_t posV16[3] = {
				floattov16(position[0] * kInvVertexScale),
				floattov16(position[1] * kInvVertexScale),
				floattov16(position[2] * kInvVertexScale),
			};
			std::memcpy(vertex, posV16, sizeof(posV16));
		}
		mesh.positionIsV16 = true;
	}

	// See RenderCapturedMesh::texCoordIsT16's own comment: this is only safe
	// because the caller already confirmed this mesh is always drawn against
	// terrainTextureId specifically. Guarded on the texture actually being
	// resident (width/height known) -- normally always true by the time a
	// section finishes building, but failing safe (leaving the float path in
	// place) rather than baking in a bogus 0x0 scale is worth the one check.
	if (mesh.hasTexture && !mesh.texCoordIsT16)
	{
		const DsiTexture* terrainTex = textureSlot(terrainTextureId);
		if (terrainTex && terrainTex->allocated && terrainTex->width > 0 && terrainTex->height > 0)
		{
			const float texW = static_cast<float>(terrainTex->width);
			const float texH = static_cast<float>(terrainTex->height);
			for (int i = 0; i < mesh.vertexCount; ++i)
			{
				std::uint8_t* vertex = raw + (std::size_t)i * mesh.stride;
				float uv[2];
				std::memcpy(uv, vertex + mesh.texCoordOffset, sizeof(uv));
				const std::int32_t uvT16[2] = {
					floattot16(uv[0] * texW),
					floattot16(uv[1] * texH),
				};
				std::memcpy(vertex + mesh.texCoordOffset, uvT16, sizeof(uvT16));
			}
			mesh.texCoordIsT16 = true;
		}
	}
}

namespace
{

// Draw-time counterpart of drawInterleavedMesh() above, for captured meshes:
// identical per-vertex colour/lightmap/normal/texcoord handling (kept in
// sync by hand -- the two diverge only in how position reaches the GPU).
// mesh.positionIsV16 tells this which of two sources drove the capture: a
// section dsiRepackCapturedMeshFast() already converted (take the pre-
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
		case RenderCapability::Texture2D: glEnable(GL_TEXTURE_2D); break;
		case RenderCapability::AlphaTest: glEnable(GL_ALPHA_TEST); break;
		case RenderCapability::Blend:     glEnable(GL_BLEND); break;
		case RenderCapability::Fog:       glEnable(GL_FOG); g_poly.fogEnabled = true; markPolyDirty(); break;
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
		case RenderCapability::Texture2D: glDisable(GL_TEXTURE_2D); break;
		case RenderCapability::AlphaTest: glDisable(GL_ALPHA_TEST); break;
		case RenderCapability::Blend:     glDisable(GL_BLEND); break;
		case RenderCapability::Fog:       glDisable(GL_FOG); g_poly.fogEnabled = false; markPolyDirty(); break;
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

// APPROXIMATED: DS depth comparison is fixed at less-or-equal in the standard
// pipeline; POLY_DEPTH_TEST_EQUAL (already available via glPolyFmt, not
// wired up here) is the only other option and does not match any
// RenderCompare value Minecraft actually requests, so this is a no-op.
void renderDepthFunc(RenderCompare)
{
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

void renderBindTexture(int texture)
{
	g_boundTexture = texture;
	if (texture > 0)
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
		if (textures[i] > 0 && static_cast<std::size_t>(textures[i]) < g_textures.size())
			g_textures[textures[i]] = DsiTexture{};
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

void renderColorMaterial(RenderFace, RenderColorMaterialMode)
{
	// DS materials are set with glMaterialf() per material type, not toggled
	// on/off per face the way desktop GL_COLOR_MATERIAL is; nothing here maps
	// cleanly, and Minecraft's lighting mostly comes from the vertex colours
	// already carried by the mesh (see the lightmap block above), so this is
	// a no-op rather than a guess.
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
}

void renderClearColor(float r, float g, float b, float a)
{
	const auto toChannel = [](float v) -> std::uint8_t
	{
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		return static_cast<std::uint8_t>(v * 31.0f + 0.5f);
	};
	glClearColor(toChannel(r), toChannel(g), toChannel(b), toChannel(a));
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
