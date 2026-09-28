#pragma once

#include "GuiContainer.h"

class InventoryPlayer;
class TileEntityFurnace;

// net.minecraft.src.GuiFurnace
class GuiFurnace : public GuiContainer
{
public:
	GuiFurnace(InventoryPlayer *player, TileEntityFurnace *furnace);

protected:
	void drawGuiContainerForegroundLayer() override;
	void drawGuiContainerBackgroundLayer(float_t partialTick) override;

private:
	TileEntityFurnace *furnaceInventory;
	// Translated once at screen-open time (same "cache invalidated by
	// reopening the screen" granularity GuiStats.cpp's own title cache
	// already uses) instead of every frame this screen is open --
	// StatCollector::translateToLocal() is a std::map lookup + string copy,
	// and drawGuiContainerForegroundLayer() runs every rendered frame.
	std::string cachedTitle;
	std::string cachedInventoryLabel;
};
