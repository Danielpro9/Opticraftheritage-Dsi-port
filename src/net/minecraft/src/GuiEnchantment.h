#pragma once

#include "GuiContainer.h"
#include "java/Random.h"


class ContainerEnchantment;
class InventoryPlayer;
class ItemStack;
class ModelBook;
class World;

// net.minecraft.src.GuiEnchantment
class GuiEnchantment : public GuiContainer
{
public:
    GuiEnchantment(InventoryPlayer *inventory, World *world, int_t x, int_t y, int_t z);
    ~GuiEnchantment() override;

    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void updateScreen() override;

protected:
    void mouseClicked(int_t mouseX, int_t mouseY, int_t button) override;
    void drawGuiContainerForegroundLayer() override;
    void drawGuiContainerBackgroundLayer(float_t partialTick) override;

private:
    void updateBookAnimation();
    void drawBook(float_t partialTick);

    Random animationRandom;
    ContainerEnchantment *containerEnchantment;
    int_t tickCount;
    float pageFlip;
    float pageFlipPrev;
    float pageFlipTarget;
    float pageFlipVelocity;
    float bookSpreadPrev;
    float bookSpread;
    ItemStack *lastStackSnapshot;
    const ItemStack *lastStackIdentity;
    int_t lastMouseX;
    int_t lastMouseY;
    // Translated once at screen-open time instead of every frame this screen
    // is open -- see GuiFurnace.h's identical fix for the full reasoning.
    std::string cachedTitle;
    std::string cachedInventoryLabel;

    // drawGuiContainerBackgroundLayer() reseeds EnchantmentNameParts to
    // containerEnchantment->nameSeed every frame before generating the 3
    // option names, so the generated names are deterministic and identical
    // frame to frame as long as nameSeed itself doesn't change (it only
    // does in ContainerEnchantment::onCraftMatrixChanged(), i.e. when the
    // item in the enchant slot changes) -- yet generateRandomEnchantName()
    // still re-ran its rand.nextInt() calls + string concatenation 3x every
    // single frame regardless. Cached here, invalidated only when nameSeed
    // changes.
    long_t cachedNameSeed = 0;
    bool cachedNamesValid = false;
    std::string cachedEnchantNames[3];
};
