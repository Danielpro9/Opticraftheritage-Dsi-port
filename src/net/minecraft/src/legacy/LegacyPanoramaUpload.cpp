#include "LegacyPanoramaUpload.h"

#include <algorithm>
#include <utility>

#include "java/BufferedImage.h"
#include "java/Type.h"
#include "platform/Log.h"
#include "LegacyUiPolicy.h"

constexpr int_t LEGACY_PANORAMA_PS2_MAX_WIDTH = 512;
constexpr int_t LEGACY_PANORAMA_WII_MAX_WIDTH = 1024;

namespace
{

// Shared bilinear resample, factored out of the old single PS2/WII-only path
// below so the DSi power-of-two path (which needs a different width AND
// height, not just a width clamp) can reuse it instead of duplicating the
// loop. 16.16 fixed point keeps this one-time console path cheap and avoids
// introducing another image-resize dependency. Releases the source pixels
// as soon as the resized image is returned, before setupTexture() allocates
// its own upload staging buffer.
std::unique_ptr<BufferedImage> resamplePanoramaBilinear(
	const BufferedImage &image, int_t targetWidth, int_t targetHeight)
{
	const int_t sourceWidth = image.getWidth();
	const int_t sourceHeight = image.getHeight();
	const unsigned char *src = image.getRawPixels();
	std::unique_ptr<unsigned char[]> dst(
		new unsigned char[BufferedImage::checkedRgbaByteCount(targetWidth, targetHeight)]);

	for (int_t y = 0; y < targetHeight; ++y)
	{
		const unsigned int yFixed = targetHeight > 1
			? static_cast<unsigned int>((static_cast<unsigned long long>(y) *
				static_cast<unsigned long long>(sourceHeight - 1) << 16) /
				static_cast<unsigned int>(targetHeight - 1))
			: 0u;
		const int_t y0 = static_cast<int_t>(yFixed >> 16);
		const int_t y1 = std::min<int_t>(y0 + 1, sourceHeight - 1);
		const unsigned int fy = yFixed & 0xffffu;

		for (int_t x = 0; x < targetWidth; ++x)
		{
			const unsigned int xFixed = targetWidth > 1
				? static_cast<unsigned int>((static_cast<unsigned long long>(x) *
					static_cast<unsigned long long>(sourceWidth - 1) << 16) /
					static_cast<unsigned int>(targetWidth - 1))
				: 0u;
			const int_t x0 = static_cast<int_t>(xFixed >> 16);
			const int_t x1 = std::min<int_t>(x0 + 1, sourceWidth - 1);
			const unsigned int fx = xFixed & 0xffffu;

			const std::size_t p00 = (static_cast<std::size_t>(y0) * sourceWidth + x0) * 4u;
			const std::size_t p10 = (static_cast<std::size_t>(y0) * sourceWidth + x1) * 4u;
			const std::size_t p01 = (static_cast<std::size_t>(y1) * sourceWidth + x0) * 4u;
			const std::size_t p11 = (static_cast<std::size_t>(y1) * sourceWidth + x1) * 4u;
			const std::size_t out = (static_cast<std::size_t>(y) * targetWidth + x) * 4u;

			for (int_t channel = 0; channel < 4; ++channel)
			{
				const unsigned int top =
					(static_cast<unsigned int>(src[p00 + channel]) * (0x10000u - fx) +
					 static_cast<unsigned int>(src[p10 + channel]) * fx + 0x8000u) >> 16;
				const unsigned int bottom =
					(static_cast<unsigned int>(src[p01 + channel]) * (0x10000u - fx) +
					 static_cast<unsigned int>(src[p11 + channel]) * fx + 0x8000u) >> 16;
				dst[out + channel] = static_cast<unsigned char>(
					(top * (0x10000u - fy) + bottom * fy + 0x8000u) >> 16);
			}
		}
	}

	return std::unique_ptr<BufferedImage>(
		new BufferedImage(targetWidth, targetHeight, std::move(dst)));
}

#if defined(DSI_PLATFORM)
// Largest power of two <= value, clamped to the DS 3D texture unit's valid
// 8-1024 range (nds/arm9/videoGL.h's glTexImageNtr2D()). Rounds DOWN rather
// than padding up to the next power of two: this is a decorative background
// image, a little softness from downscaling is invisible next to the
// legacy blur this same panorama used to (and, on DSi, no longer does --
// see LegacyPanorama.cpp) apply anyway, and padding would need the extra
// border pixels filled with *something* (transparent stretches the UVs,
// edge-clamped needs its own copy loop) for zero practical benefit.
int_t roundDownToPowerOfTwo(int_t value)
{
	if (value <= 8)
		return 8;
	int_t p = 8;
	while (p * 2 <= value && p < 1024)
		p *= 2;
	return p;
}

// Opposite rounding direction, same clamp range, for the small Legacy UI
// sprites below: the checkbox art and the in-game tip panel's nine-slice
// border are a couple dozen pixels across to begin with, so rounding DOWN
// like the decorative images above would throw away a third or more of an
// already-tiny asset. Rounding UP costs a slightly larger (still trivially
// small -- at most 32x32) paletted texture instead, which is not a VRAM
// concern next to panorama/terrain.
int_t roundUpToPowerOfTwo(int_t value)
{
	if (value <= 8)
		return 8;
	int_t p = 8;
	while (p < value && p < 1024)
		p *= 2;
	return p;
}
#endif

}

std::unique_ptr<BufferedImage> legacyPreparePanoramaForUpload(
	const std::string &name, std::unique_ptr<BufferedImage> image)
{
	// Real-hardware data (GuiMainMenu.cpp's per-section [dsi.perf] timing,
	// added specifically to find this): the menu's "title" section alone was
	// costing ~1-2 SECONDS on every single frame, indefinitely -- not once at
	// boot, every frame, for as long as the menu stayed on screen. That shape
	// (constant cost, no periodic pattern, no plateau after the first frame)
	// matches this exact bug already fixed once for panorama.png below: a
	// texture whose glTexImage2D() upload fails DS's exact-power-of-two check
	// is never inserted into RenderEngine's textureMap (see that file's
	// getTexture(), "Do not cache a broken texture id; retry the whole load
	// on the next bind") -- so every call to legacyDrawTitleTexture() (once
	// per menu frame) re-opens legacyUiTitleResourcePath() from the SD card,
	// re-decodes the PNG, and re-attempts (and re-fails) the upload from
	// scratch, forever. This function already existed to hand exactly this
	// hardware limitation a texture it can accept for panorama.png; the
	// legacy title banner is the same kind of real-photo-sourced, non-power-
	// of-two, purely decorative image (explicitly flagged as such --
	// legacyUiTitleResourcePath()'s own "hardcoded badd" comment), so it
	// needs the identical treatment, not a second copy of it.
	// Real-hardware confirmation (the one-shot RenderEngine.cpp texture-upload
	// warning added specifically to answer this): /legacy/logo1.png and
	// /legacy/logo2.png -- the two "OptiCraft Heritage Edition" startup splash
	// images StartupPresentation.cpp's playLegacyLogo() draws full-screen,
	// once each, via drawFullscreenTexture() -- fail this same power-of-two
	// upload check too. Lower-severity than panorama/title (each is bound
	// once per boot, not once per menu frame, so this was never a sustained
	// per-frame cost), but the visible result is the same class of bug: a
	// blank/white splash screen instead of the actual logo. Same treatment
	// applies cleanly since these are also simple decorative full-screen
	// draws, not tile atlases whose UV math depends on exact pixel dimensions.
	const bool isDsiLegacyDecorativeTexture =
#if defined(DSI_PLATFORM)
		name == "/legacy/panorama.png" || name == legacyUiTitleResourcePath() ||
		name == "/legacy/logo1.png" || name == "/legacy/logo2.png";
#else
		name == "/legacy/panorama.png";
#endif

	// Real-hardware confirmation (the same one-shot RenderEngine.cpp warning
	// that caught logo1.png/logo2.png above): /legacy/pointer_panel.png (24x24,
	// LegacyTipHud.cpp's in-game tutorial-tip nine-slice border) and
	// /legacy/tick.png (28x24, LegacyOptionCheckbox.cpp's checkbox mark) also
	// fail the exact-power-of-two upload check. Unlike the decorative images
	// above, a texture that never gets inserted into RenderEngine's textureMap
	// does not just retry forever -- RenderEngine.cpp's getTexture() now binds
	// the checkerboard placeholder in its place, since real hardware showed
	// every unguarded renderBindTexture(getTexture(...)) call site in the game
	// would otherwise draw whatever was last left in that VRAM address instead
	// (the "a box appears over the text, coloured like the background" bug).
	// The checkerboard is a safe fallback, not a good look for a tiny UI
	// decoration, so still worth fixing at the source: resize the whole
	// sprite up to a valid power of two, same as the panorama/title/logo
	// path below. tickbox.png/tickbox_hovered.png are included pre-emptively
	// (the resize below is a no-op if they already happen to be power-of-two)
	// since they are the same kind of small Legacy4J UI art and share this
	// exact failure mode if they ever turn out not to be.
	const bool isDsiLegacyUiSprite =
#if defined(DSI_PLATFORM)
		name == "/legacy/pointer_panel.png" || name == "/legacy/tick.png" ||
		name == "/legacy/tickbox.png" || name == "/legacy/tickbox_hovered.png";
#else
		false;
#endif

	// Real-hardware log evidence: with Legacy UI (the button layout) off,
	// GuiMainMenu's renderSkybox() binds all six of these every frame it is
	// on screen. At their native 256x256, even the cheap paletted GL_RGB256
	// path is a fixed 64KB each (RenderAPI_DSI.cpp always stores one byte per
	// pixel there, regardless of how few colours survive quantisation) --
	// 6 * 64KB = 384KB of the DSi's fixed 512KB texture budget for the
	// background alone, before the menu's own gui.png/font/items.png atlases
	// (which have to be resident at the same time) get any room. A real log
	// showed exactly that: by the time panorama2.png loaded, ~380KB was
	// already resident from the rest of the menu, and it plus three more
	// faces (and, as a knock-on, the logo right after them) all failed to
	// upload and fell back to RenderEngine.cpp's checkerboard placeholder --
	// the white/checkerboard screen flicker reported with Legacy UI off. The
	// background has to stay the original vanilla artwork (unlike
	// LEGACY_PANORAMA_*, no swap to a different image), so shrink it the same
	// way terrain.png and gui/items.png already are for the identical VRAM
	// reason: halve it. Down to 128x128, the same paletted upload needs
	// 16KB, 6 * 16KB = 96KB total -- the freed 288KB comfortably covers the
	// logo's own 64KB too, so it is not included here.
	//
	// /title/mclogo.png deliberately excluded despite failing in the same
	// log: GuiMainMenu.cpp draws it with Gui::drawTexturedModalRect(), which
	// hardcodes a 1/256 UV scale (Gui.cpp) rather than deriving it from the
	// texture's actual size the way terrain.png/items.png's tile math does
	// (Config::iconWidthTerrain/iconWidthItems) -- resizing it here would
	// have every one of its sprite-sheet slices (the logo's "oo" vs "nooo"
	// variant, each a hardcoded pixel rect) sample the wrong region of a now-
	// smaller texture. Freeing the panorama faces' 288KB already gives its
	// unresized 64KB request room to fit.
	const bool isDsiVanillaPanoramaAsset =
#if defined(DSI_PLATFORM)
		name == "/title/bg/panorama0.png" || name == "/title/bg/panorama1.png" ||
		name == "/title/bg/panorama2.png" || name == "/title/bg/panorama3.png" ||
		name == "/title/bg/panorama4.png" || name == "/title/bg/panorama5.png";
#else
		false;
#endif

	if (!image || (!isDsiLegacyDecorativeTexture && !isDsiLegacyUiSprite && !isDsiVanillaPanoramaAsset))
		return image;

	const int_t sourceWidth = image->getWidth();
	const int_t sourceHeight = image->getHeight();
	if (sourceWidth <= 0 || sourceHeight <= 0)
		return image;

	// Defaults mean "no resize needed"; only a platform branch below that
	// actually wants a resize overwrites them. Declaring these unconditionally
	// (rather than inside each #if arm) keeps the shared resample call below
	// building on every platform, including ones that take neither branch.
	int_t targetWidth = sourceWidth;
	int_t targetHeight = sourceHeight;

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	const int_t maxWidth =
#if defined(PS2_PLATFORM)
		LEGACY_PANORAMA_PS2_MAX_WIDTH;
#else
		LEGACY_PANORAMA_WII_MAX_WIDTH;
#endif
	if (sourceWidth > maxWidth)
	{
		targetWidth = maxWidth;
		const long long scaledHeight =
			static_cast<long long>(sourceHeight) * static_cast<long long>(targetWidth);
		targetHeight = std::max<int_t>(1, static_cast<int_t>(
			(scaledHeight + sourceWidth / 2) / sourceWidth));
	}
#elif defined(DSI_PLATFORM)
	// Unlike PS2's GS or Wii's GX, the DS 3D texture unit requires an EXACT
	// power of two per dimension -- not just "small enough". A real photo-
	// sourced panorama.png (and, since real hardware just proved it, the
	// legacy title banner too) is essentially never power-of-two, so without
	// this it silently failed to upload at all (RenderAPI_DSI.cpp's
	// uploadTexture() now reports that failure instead of hiding it, but
	// the actual fix is to hand it a texture the hardware can accept in the
	// first place). For panorama.png that showed up as a blank white
	// background; for the title banner it showed up as a ~1-2s-per-frame
	// stall, since RenderEngine.cpp never caches a texture that fails this
	// validity check, so the full SD decode+upload was retried from scratch
	// on every single frame instead of once. 256 is also this console's own
	// screen width, so there is no benefit to keeping either image bigger
	// than that resident.
	//
	// Width and height used to be capped to 256 and rounded down to the
	// nearest power of two INDEPENDENTLY of each other. That is invisible on
	// the panorama (legacyDrawPanorama() maps it with screen-relative UVs
	// that already don't preserve its native aspect ratio, so a bit more
	// distortion is lost in the noise), but the very next real-hardware test
	// after extending this to the title banner reported it now rendering
	// "muito estrecho" -- squashed -- because legacyFitTitleRect() DOES fit
	// the banner to the texture's own (width, height) aspect ratio, and
	// independent per-axis rounding can drift that ratio a long way from the
	// source image's. Scaling both axes by the same factor first, then
	// rounding the already-proportional result down to a power of two, keeps
	// the quantisation error the same order of magnitude per axis instead of
	// letting it compound into a visibly wrong aspect ratio.
	if (isDsiLegacyUiSprite)
	{
		// These sprites are drawn either as a single UV-(0,0)-(1,1) quad
		// (legacyDrawUiTexture(), tick.png/tickbox*.png) or as a nine-slice
		// grid whose border/corner UVs are FRACTIONS of the whole texture
		// (legacyDrawUiTextureNineSlice(), pointer_panel.png -- see
		// LegacyUiTexture.cpp). Both read the entire texture as UV space
		// 0..1, so resizing the whole image keeps every existing ratio
		// exactly as it was; nothing downstream needs to know this resize
		// happened. That is NOT true of the aspect-fit-to-256 branch below
		// (legacyFitTitleRect() fits the draw rect to the image's aspect
		// ratio, so THAT resize has to preserve it) -- these sprites are
		// drawn into a caller-chosen rectangle independent of their own
		// aspect ratio, so each axis can round up on its own with no
		// distortion risk either way.
		targetWidth = roundUpToPowerOfTwo(sourceWidth);
		targetHeight = roundUpToPowerOfTwo(sourceHeight);
	}
	else if (isDsiVanillaPanoramaAsset)
	{
		// All six are resident at once (see the VRAM-budget comment above),
		// unlike panorama.png/the title banner below which are each the only
		// one of their kind on screen -- a 256 cap is not tight enough on its
		// own, so halve on top of it. A plain source-size halving
		// (not a fixed 128 target) so a texture pack with a non-256 panorama
		// still gets the same proportional VRAM win instead of an arbitrary
		// upscale or a no-op.
		targetWidth = roundDownToPowerOfTwo(std::max<int_t>(1, sourceWidth / 2));
		targetHeight = roundDownToPowerOfTwo(std::max<int_t>(1, sourceHeight / 2));
	}
	else
	{
		const int_t longSide = std::max<int_t>(sourceWidth, sourceHeight);
		const double scale = longSide > 256 ? 256.0 / static_cast<double>(longSide) : 1.0;
		const int_t scaledWidth = std::max<int_t>(1, static_cast<int_t>(sourceWidth * scale + 0.5));
		const int_t scaledHeight = std::max<int_t>(1, static_cast<int_t>(sourceHeight * scale + 0.5));
		targetWidth = roundDownToPowerOfTwo(scaledWidth);
		targetHeight = roundDownToPowerOfTwo(scaledHeight);
	}
#endif

	if (targetWidth == sourceWidth && targetHeight == sourceHeight)
		return image;

	std::unique_ptr<BufferedImage> resized =
		resamplePanoramaBilinear(*image, targetWidth, targetHeight);
	MC_LOG_DEBUG("render", "legacy panorama upload resize %dx%d -> %dx%d\n",
		(int)sourceWidth, (int)sourceHeight, (int)targetWidth, (int)targetHeight);
	return resized;
}
