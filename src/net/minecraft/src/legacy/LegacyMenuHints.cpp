#include "LegacyMenuHints.h"

#include <string>

#include "net/minecraft/src/FontRenderer.h"
#include "platform/PlatformConfig.h"

void drawLegacyMenuHints(FontRenderer *font, int_t, int_t screenHeight, bool showBack)
{
    if (font == nullptr)
        return;

#if PLATFORM_PS2
    static const std::string navigate = "[D-Pad] Navigate";
    static const std::string select = "[X] Select";
    static const std::string back = "[O] Back";
#elif PLATFORM_WII
    static const std::string navigate = "[D-Pad] Navigate";
    static const std::string select = "[A] Select";
    static const std::string back = "[B] Back";
#else
    static const std::string navigate = "[Up/Down] Navigate";
    static const std::string select = "[Enter] Select";
    static const std::string back = "[Esc] Back";
#endif
    // Every string above is a fixed literal, never changing at runtime -- this
    // runs every frame on every Legacy UI screen that shows hints, so
    // measuring their width is a one-time cost, not a per-frame one.
    static const int_t navigateWidth = font->getStringWidth(navigate);
    static const int_t selectWidth = font->getStringWidth(select);

    const int_t y = legacyHintRowY(screenHeight);
    int_t x = LEGACY_HINT_MARGIN;
    font->drawStringWithShadow(navigate, x, y, 0xf0f0f0);
    x += navigateWidth + LEGACY_HINT_GAP;
    font->drawStringWithShadow(select, x, y, 0xf0f0f0);
    if (showBack)
    {
        x += selectWidth + LEGACY_HINT_GAP;
        font->drawStringWithShadow(back, x, y, 0xf0f0f0);
    }
}
