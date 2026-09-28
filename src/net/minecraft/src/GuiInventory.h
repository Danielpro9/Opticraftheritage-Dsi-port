#pragma once

#include "GuiContainer.h"

class EntityPlayer;

// net.minecraft.src.GuiInventory
class GuiInventory : public GuiContainer
{
public:
	GuiInventory(EntityPlayer *player);

	void initGui() override;
	void updateScreen() override;

protected:
	void drawGuiContainerForegroundLayer() override;

public:
	void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;

protected:
	void drawGuiContainerBackgroundLayer(float_t partialTick) override;
	void actionPerformed(GuiButton *button) override;

private:
	void displayDebuffEffects();

	float_t xSize_lo;
	float_t ySize_lo;
	// Translated once at screen-open time instead of every frame this screen
	// is open -- see GuiFurnace.h's identical fix for the full reasoning.
	std::string cachedTitle;
};
