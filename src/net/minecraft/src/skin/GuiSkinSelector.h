#pragma once

#include "net/minecraft/src/GuiScreen.h"
#include "SkinManager.h"
#include <string>

class GuiButton;

// Trimmed port of upstream's skin selector for this fork's scope: no Player 2
// skin (no split-screen here), no in-game "Load Skins" SD browser (see
// SkinManager.h), and no per-skin Delete button (the player manages the
// skins folder from a computer, deletion included). Just: pick between the
// bundled default and whatever SkinManager::scanCustomSkins() found.
class GuiSkinSelector : public GuiScreen
{
public:
    explicit GuiSkinSelector(GuiScreen *parent);
    ~GuiSkinSelector() override = default;

    void initGui() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void updateScreen() override;
    void keyTyped(char_t c, int_t key) override;
    void mouseClicked(int_t mouseX, int_t mouseY, int_t button) override;
    void actionPerformed(GuiButton *button) override;
    bool doesGuiPauseGame() override;

protected:
    bool allowsPlatformPointerInput() const override { return true; }
    void handleSpecializedMenuInput() override;

private:
    void nextSkin();
    void prevSkin();
    void switchPack(int newPackIndex);
    void selectAndConfirm();
    void cancelAndReturn();

    void drawInsetPanel(int_t left, int_t top, int_t right, int_t bottom, int_t fillColor);
    void drawFrontPreview(const SkinEntry *skin, float x, float y, float w, float h, float alpha);
    void drawFeetShadow(float centerX, float groundY, float radiusX, float radiusY, float alpha);

    GuiScreen *parentScreen;
    bool initializedSelection;
    int currentPackIndex;
    int currentSkinIndex;
    float scrollOffset; // smooth transition offset: -1.0 (moving right) to +1.0 (moving left), dampens to 0.0

    // Stored dimensions for click hit-testing
    int_t dialogLeft;
    int_t dialogTop;
    int_t dialogWidth;
    int_t dialogHeight;
    int_t leftPanelWidth;
    int_t rightPanelX;
    int_t rightPanelWidth;
    int_t carouselCenterX;
    int_t carouselGroundY;
    int_t nameplateY;
    int_t nameplateHeight;

    GuiButton *buttonTabDefault;
    GuiButton *buttonTabCustom;

    // "n / total" counter text/width: recomputed only when the current skin
    // or pack count changes, not every frame this screen is drawn.
    int cachedCounterSkinIndex = -1;
    int cachedCounterTotalSkins = -1;
    std::string cachedCounterText;
    int_t cachedCounterWidth = 0;
};
