#include "ScaledResolution.h"

#include <cmath>
#include "java/Arithmetic.h"
#include "GameSettings.h"
#include "platform/ConsoleAspectRatio.h"
#include "platform/PlatformTuning.h"

ScaledResolution::ScaledResolution(GameSettings *gamesettings, int_t i, int_t j)
{
	const bool widescreen = gamesettings != nullptr && gamesettings->widescreen;
	const bool legacyUIForCache = gamesettings != nullptr && gamesettings->legacyUI;
	const int_t guiScaleForCache = gamesettings != nullptr ? gamesettings->guiScale : 0;

	// Constructed several times every single frame (EntityRenderer's HUD/
	// overlay setup, every GuiIngame overlay draw, every settings screen),
	// almost always with the exact same inputs -- the display size never
	// changes mid-session on a fixed-resolution console, and guiScale/
	// widescreen/legacyUI only change when the player visits the options
	// screen. The two std::ceil() calls below are soft-float on the ARM9
	// (no FPU), on top of the scale-factor search loop's own integer
	// divisions (also soft on this CPU) -- real, repeated, avoidable cost
	// for a result that is identical call to call with the same inputs.
	// Safe for every platform, not just DSi: a cache hit reproduces exactly
	// what the uncached path below would have computed for the same inputs,
	// and a settings change is simply a cache miss like any other.
	static int_t s_cachedI = 0, s_cachedJ = 0, s_cachedGuiScale = 0;
	static bool s_cachedWidescreen = false, s_cachedLegacyUI = false, s_hasCache = false;
	static int_t s_cachedScaledWidth = 0, s_cachedScaledHeight = 0, s_cachedScaleFactor = 1;
	static double s_cachedExactScaleFactor = 1.0, s_cachedFieldA = 0.0, s_cachedFieldB = 0.0;

	if (s_hasCache && s_cachedI == i && s_cachedJ == j && s_cachedGuiScale == guiScaleForCache &&
		s_cachedWidescreen == widescreen && s_cachedLegacyUI == legacyUIForCache)
	{
		scaledWidth = s_cachedScaledWidth;
		scaledHeight = s_cachedScaledHeight;
		scaleFactor = s_cachedScaleFactor;
		exactScaleFactor = s_cachedExactScaleFactor;
		field_25121_a = s_cachedFieldA;
		field_25120_b = s_cachedFieldB;
		return;
	}

	scaledWidth = ConsoleAspectRatio::getLogicalWidth(i, j, widescreen);
	scaledHeight = ConsoleAspectRatio::getLogicalHeight(j);
	scaleFactor = 1;
	exactScaleFactor = 1.0;
	if (gamesettings != nullptr && gamesettings->legacyUI && PLATFORM_LEGACY_GUI_SCALE > 0.0)
	{
		exactScaleFactor = PLATFORM_LEGACY_GUI_SCALE;
	}
	else
	{
#if PLATFORM_CONSOLE_LOW && PLATFORM_FORCE_GUI_SCALE > 0
		scaleFactor = PLATFORM_FORCE_GUI_SCALE;
		while (scaleFactor > 1 && (scaledWidth / scaleFactor < 1 || scaledHeight / scaleFactor < 1))
			scaleFactor--;
		exactScaleFactor = static_cast<double>(scaleFactor);
#else
		int_t k = gamesettings->guiScale;
		if (k == 0)
			k = 1000;
		for (; scaleFactor < k && scaledWidth / (scaleFactor + 1) >= 320 && scaledHeight / (scaleFactor + 1) >= 240; scaleFactor++)
		{
		}
		exactScaleFactor = static_cast<double>(scaleFactor);
#endif
	}
	field_25121_a = (double)scaledWidth / exactScaleFactor;
	field_25120_b = (double)scaledHeight / exactScaleFactor;
	scaledWidth = JavaArithmetic::doubleToInt(std::ceil(field_25121_a));
	scaledHeight = JavaArithmetic::doubleToInt(std::ceil(field_25120_b));

	s_cachedI = i;
	s_cachedJ = j;
	s_cachedGuiScale = guiScaleForCache;
	s_cachedWidescreen = widescreen;
	s_cachedLegacyUI = legacyUIForCache;
	s_hasCache = true;
	s_cachedScaledWidth = scaledWidth;
	s_cachedScaledHeight = scaledHeight;
	s_cachedScaleFactor = scaleFactor;
	s_cachedExactScaleFactor = exactScaleFactor;
	s_cachedFieldA = field_25121_a;
	s_cachedFieldB = field_25120_b;
}

int_t ScaledResolution::getScaledWidth() const
{
	return scaledWidth;
}

int_t ScaledResolution::getScaledHeight() const
{
	return scaledHeight;
}


double ScaledResolution::getScaleFactorExact() const
{
	return exactScaleFactor;
}
