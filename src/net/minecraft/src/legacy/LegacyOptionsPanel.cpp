#include "LegacyOptionsPanel.h"

#include "LegacyOptionStyle.h"
#include "LegacyUiTheme.h"

void LegacyOptionsPanel::draw(const LegacyOptionsLayout &layout)
{
    const LegacyUiTheme &theme = legacyUiTheme();
    const LegacyPanelColors colors = {
        theme.panelFillColor, theme.panelBorderColor, theme.panelHighlightColor,
        theme.panelShadowColor, theme.panelDropShadowColor
    };
    drawFrame(layout, colors);
}

void LegacyOptionsPanel::drawFrame(const LegacyOptionsLayout &layout, const LegacyPanelColors &colors)
{
    const LegacyUiTheme &theme = legacyUiTheme();
    const int_t left = layout.panelX;
    const int_t top = layout.panelY;
    const int_t right = left + layout.panelWidth;
    const int_t bottom = top + layout.panelHeight;
    const int_t cut = theme.panelCornerCut;

    // Real-hardware report: this panel rendered completely see-through on
    // DSi specifically on every screen that shows the animated panorama
    // background (main-menu Options, the world list) while rendering
    // correctly on screens that use a flat dimmed-world overlay instead (the
    // pause-reached Debug Cheats screen) -- same panel code, same colours,
    // only the preceding background draw differs. Nothing conclusive was
    // found by auditing state cleanup in the panorama/title draw calls that
    // run immediately before this (both were confirmed to leave Blend/
    // Texture2D in a state drawRect() already tolerates), so rather than
    // guess further at the exact mechanism, this switches the fill from
    // drawRect() (Gui.cpp: sets colour via external render state,
    // RenderAPI_DSI.cpp's renderColor4f(), applied only when the next
    // drawInterleavedMesh() runs) to drawGradientRect() with identical top/
    // bottom colours -- visually an ordinary flat fill, but through the one
    // draw helper in this file that already manages its own Blend/
    // AlphaTest/ShadeModel state completely self-contained, confirmed
    // working on real hardware immediately after much busier draws (the
    // pause-menu vignette, layered directly over live 3D world rendering).
    // If this does not fix it, the bug is not state leaking from the
    // panorama/title draws specifically, and the next real-hardware log
    // needs to isolate something else.
    const auto roundedFill = [this, cut](int_t l, int_t t, int_t r, int_t b, int_t color)
    {
        drawGradientRect(l + cut, t, r - cut, b, color, color);
        drawGradientRect(l, t + cut, r, b - cut, color, color);
        drawGradientRect(l + 1, t + 1, r - 1, t + cut, color, color);
        drawGradientRect(l + 1, b - cut, r - 1, b - 1, color, color);
    };

    roundedFill(left + 2, top + 2, right + 2, bottom + 2, colors.dropShadow);
    roundedFill(left - 1, top - 1, right + 1, bottom + 1, colors.border);
    roundedFill(left, top, right, bottom, colors.fill);

    drawGradientRect(left + 2, top + 1, right - 2, top + 2, colors.highlight, colors.highlight);
    drawGradientRect(left + 1, top + 2, left + 2, bottom - 2, colors.highlight, colors.highlight);
    drawGradientRect(left + 2, bottom - 2, right - 2, bottom - 1, colors.shadow, colors.shadow);
    drawGradientRect(right - 2, top + 2, right - 1, bottom - 2, colors.shadow, colors.shadow);
}
