#pragma once

#include "GuiContainer.h"

class InventoryPlayer;
class TileEntityBrewingStand;

// net.minecraft.src.GuiBrewingStand
class GuiBrewingStand : public GuiContainer
{
public:
    GuiBrewingStand(InventoryPlayer *inventory, TileEntityBrewingStand *brewingStand);

protected:
    void drawGuiContainerForegroundLayer() override;
    void drawGuiContainerBackgroundLayer(float_t partialTick) override;

private:
    TileEntityBrewingStand *brewingStand;
    // Translated once at screen-open time instead of every frame this screen
    // is open -- see GuiFurnace.h's identical fix for the full reasoning.
    std::string cachedTitle;
    std::string cachedInventoryLabel;
};
