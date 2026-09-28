#include "GuiDispenser.h"
#include "ContainerDispenser.h"
#include "InventoryPlayer.h"
#include "TileEntityDispenser.h"
#include "FontRenderer.h"
#include "RenderEngine.h"
#include "Minecraft.h"
#include "StatCollector.h"
#include "platform/RenderAPI.h"

GuiDispenser::GuiDispenser(InventoryPlayer *player, TileEntityDispenser *dispenser)
	: GuiContainer(new ContainerDispenser(player, dispenser), true)
	, cachedTitle(StatCollector::translateToLocal("container.dispenser"))
	, cachedInventoryLabel(StatCollector::translateToLocal("container.inventory"))
{
}

void GuiDispenser::drawGuiContainerForegroundLayer()
{
	fontRenderer->drawString(cachedTitle,           60, 6,             0x404040);
	fontRenderer->drawString(cachedInventoryLabel,   8, (ySize - 96) + 2, 0x404040);
}

void GuiDispenser::drawGuiContainerBackgroundLayer(float_t partialTick)
{
	// Cached rather than looked up fresh every frame this screen is open --
	// same pattern/reasoning as this session's other texture-id-caching fixes.
	// CachedTextureId re-resolves itself if this id was ever invalidated by a
	// releaseTexture() call -- see its own comment in RenderEngine.h.
	static CachedTextureId cachedTrapTextureId;
	int_t tex = cachedTrapTextureId.get(mc->renderEngine, "/gui/trap.png");
	renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	mc->renderEngine->bindTexture(tex);
	int_t guiX = (width  - xSize) / 2;
	int_t guiY = (height - ySize) / 2;
	drawTexturedModalRect(guiX, guiY, 0, 0, xSize, ySize);
}
