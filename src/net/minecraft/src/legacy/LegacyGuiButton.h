#pragma once

#include "net/minecraft/src/GuiButton.h"

class LegacyGuiButton : public GuiButton
{
public:
    LegacyGuiButton(int_t id, int_t x, int_t y, int_t width, int_t height, const std::string &text,
        float_t opacity = 1.0f);

    void drawButton(Minecraft *mc, int_t mouseX, int_t mouseY) override;
    void setSelected(bool selectedValue);
    void setKeyboardSelected(bool selectedValue) override;

private:
    float_t opacity;
    bool selected;

    // Same fix as GuiButton.h's own cachedWidthString/cachedWidth, for the
    // same reason: this class draws its own text directly (legacyDrawCenteredOptionText)
    // instead of going through the base class's cached measurement, so it never
    // got that fix. getStringWidth() heap-allocates a UTF-16 vector and scans it
    // per character -- for every button on screen, every frame, while this
    // (the active Legacy UI button class) is on screen.
    std::string cachedWidthString;
    int_t cachedWidth = 0;
};
