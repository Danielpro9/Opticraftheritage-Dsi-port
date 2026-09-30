#pragma once

#include "GuiContainer.h"

class IInventory;

// net.minecraft.src.GuiChest
class GuiChest : public GuiContainer
{
public:
	GuiChest(IInventory *upper, IInventory *lower);

protected:
	void drawGuiContainerForegroundLayer() override;
	void drawGuiContainerBackgroundLayer(float_t partialTick) override;
	void onGuiClosed() override;

private:
	IInventory *upperChestInventory;
	IInventory *lowerChestInventory;
	int_t inventoryRows;
	// Translated once at screen-open time instead of every frame this screen
	// is open -- see GuiFurnace.h's identical fix for the full reasoning.
	// getInvName() is fixed for these two IInventory pointers' lifetime (a
	// chest can't be renamed while its own screen is open -- that needs the
	// anvil, a different screen), so this is still correct to cache per
	// screen-open even though the label is instance-specific, not a shared
	// translation key like the other container screens.
	std::string cachedLowerLabel;
	std::string cachedUpperLabel;
};
