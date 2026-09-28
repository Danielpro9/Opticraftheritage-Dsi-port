#pragma once

#include "GuiContainer.h"

class InventoryPlayer;
class World;

// net.minecraft.src.GuiCrafting
class GuiCrafting : public GuiContainer
{
public:
	GuiCrafting(InventoryPlayer *player, World *world, int_t x, int_t y, int_t z);

	void onGuiClosed() override;

protected:
	void drawGuiContainerForegroundLayer() override;
	void drawGuiContainerBackgroundLayer(float_t partialTick) override;

private:
	// Translated once at screen-open time instead of every frame this screen
	// is open -- see GuiFurnace.h's identical fix for the full reasoning.
	std::string cachedTitle;
	std::string cachedInventoryLabel;
};
