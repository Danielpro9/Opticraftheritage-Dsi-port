#include "GuiChest.h"
#include "ContainerChest.h"
#include "IInventory.h"
#include "InventoryLargeChest.h"
#include "InventoryBasic.h"
#include "FontRenderer.h"
#include "RenderEngine.h"
#include "Minecraft.h"
#include "StatCollector.h"
#include "platform/PlatformConfig.h"
#include "platform/RenderAPI.h"

GuiChest::GuiChest(IInventory *upper, IInventory *lower)
	: GuiContainer(new ContainerChest(upper, lower, (dynamic_cast<InventoryLargeChest *>(lower) != nullptr || dynamic_cast<InventoryBasic *>(lower) != nullptr)), true)
	, upperChestInventory(upper)
	, lowerChestInventory(lower)
	, inventoryRows(0)
	, cachedLowerLabel(StatCollector::translateToLocal(lower->getInvName()))
	, cachedUpperLabel(StatCollector::translateToLocal(upper->getInvName()))
{
	field_948_f = false;
	inventoryRows = lower->getSizeInventory() / 9;
	ySize = 114 + inventoryRows * 18;
}

void GuiChest::drawGuiContainerForegroundLayer()
{
	fontRenderer->drawString(cachedLowerLabel, 8, 6, 0x404040);
	fontRenderer->drawString(cachedUpperLabel, 8, (ySize - 96) + 2, 0x404040);
}

void GuiChest::drawGuiContainerBackgroundLayer(float_t partialTick)
{
	// Cached rather than looked up fresh every frame this screen is open --
	// same pattern/reasoning as this session's other texture-id-caching fixes.
	// CachedTextureId re-resolves itself if this id was ever invalidated by a
	// releaseTexture() call -- see its own comment in RenderEngine.h.
	static CachedTextureId cachedContainerTextureId;
	int_t tex = cachedContainerTextureId.get(mc->renderEngine, "/gui/container.png");
	renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	mc->renderEngine->bindTexture(tex);
	int_t guiX = (width  - xSize) / 2;
	int_t guiY = (height - ySize) / 2;
	drawTexturedModalRect(guiX, guiY,                             0,   0,   xSize, inventoryRows * 18 + 17);
	drawTexturedModalRect(guiX, guiY + inventoryRows * 18 + 17,   0, 126,   xSize, 96);
}

void GuiChest::onGuiClosed()
{
	GuiContainer::onGuiClosed();

#if PLATFORM_DSI
	// Same pattern as GuiContainerCreative::onGuiClosed() -- see
	// GuiFurnace::onGuiClosed()'s comment for the full reasoning.
	if (mc != nullptr && mc->renderEngine != nullptr)
		mc->renderEngine->releaseTexture("/gui/container.png");
#endif
}
