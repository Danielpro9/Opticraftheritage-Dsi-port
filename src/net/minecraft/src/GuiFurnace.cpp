#include "GuiFurnace.h"
#include "ContainerFurnace.h"
#include "TileEntityFurnace.h"
#include "InventoryPlayer.h"
#include "FontRenderer.h"
#include "RenderEngine.h"
#include "Minecraft.h"
#include "StatCollector.h"
#include "platform/RenderAPI.h"

GuiFurnace::GuiFurnace(InventoryPlayer *player, TileEntityFurnace *furnace)
	: GuiContainer(new ContainerFurnace(player, furnace), true)
	, furnaceInventory(furnace)
	, cachedTitle(StatCollector::translateToLocal("container.furnace"))
	, cachedInventoryLabel(StatCollector::translateToLocal("container.inventory"))
{
}

void GuiFurnace::drawGuiContainerForegroundLayer()
{
	fontRenderer->drawString(cachedTitle,           60, 6,             0x404040);
	fontRenderer->drawString(cachedInventoryLabel,   8, (ySize - 96) + 2, 0x404040);
}

void GuiFurnace::drawGuiContainerBackgroundLayer(float_t partialTick)
{
	// Cached rather than looked up fresh every frame this screen is open --
	// same pattern/reasoning as this session's other texture-id-caching fixes.
	// CachedTextureId re-resolves itself if this id was ever invalidated by a
	// releaseTexture() call -- see its own comment in RenderEngine.h.
	static CachedTextureId cachedFurnaceTextureId;
	int_t tex = cachedFurnaceTextureId.get(mc->renderEngine, "/gui/furnace.png");
	renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	mc->renderEngine->bindTexture(tex);
	int_t guiX = (width  - xSize) / 2;
	int_t guiY = (height - ySize) / 2;
	drawTexturedModalRect(guiX, guiY, 0, 0, xSize, ySize);

	if (furnaceInventory->isBurning())
	{
		int_t fireH = furnaceInventory->getBurnTimeRemainingScaled(12);
		drawTexturedModalRect(guiX + 56, (guiY + 36 + 12) - fireH, 176, 12 - fireH, 14, fireH + 2);
	}
	int_t cookW = furnaceInventory->getCookProgressScaled(24);
	drawTexturedModalRect(guiX + 79, guiY + 34, 176, 14, cookW + 1, 16);
}
