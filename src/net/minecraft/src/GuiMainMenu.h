#pragma once

#include "GuiScreen.h"
#include <string>

class GuiButton;

// net.minecraft.src.GuiMainMenu
class GuiMainMenu : public GuiScreen
{
public:
    GuiMainMenu();
    ~GuiMainMenu() override;

    void updateScreen() override;
    bool doesGuiPauseGame() override;

protected:
    bool usesSpecializedMenuNavigation() const override;
    void keyTyped(char_t c, int_t key) override;

public:
    void initGui() override;

protected:
    void actionPerformed(GuiButton *button) override;

public:
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;

private:
    void drawPanorama(int_t mouseX, int_t mouseY, float_t partialTick, float_t aspectRatio);
    void rotateAndBlurSkybox(float_t partialTick, bool copyFramebuffer = true);
    void renderSkybox(int_t mouseX, int_t mouseY, float_t partialTick);
    void syncLegacySelection();
    void moveLegacySelection(int_t direction);
    void activateLegacySelection();

    float_t updateCounter;
    std::string splashText;
    // splashText is picked once (constructor, or a date-based special
    // message) and never changes per frame, but drawScreen() used to
    // re-measure it three separate times every frame (twice directly, once
    // inside drawCenteredString()) via FontRenderer::getStringWidth() -- which
    // heap-allocates a UTF-16 std::vector and scans it per character every
    // call. Cached the same way GuiButton.h's own cachedWidthString is.
    std::string cachedSplashWidthText;
    int_t cachedSplashWidth = 0;
    GuiButton *multiplayerButton;
    int_t panoramaTimer;
    int_t viewportTexture;
    bool legacyPanoramaAvailable;
    int_t selectedControlIndex;
    int_t hoveredControlIndex;
};
