#include "GuiCrafting.h"
#include "ContainerWorkbench.h"
#include "InventoryPlayer.h"
#include "World.h"
#include "FontRenderer.h"
#include "RenderEngine.h"
#include "Minecraft.h"
#include "StatCollector.h"
#include "EntityPlayerSP.h"
#include "platform/RenderAPI.h"

GuiCrafting::GuiCrafting(InventoryPlayer *player, World *world, int_t x, int_t y, int_t z)
	: GuiContainer(new ContainerWorkbench(player, world, x, y, z), true)
{
}

void GuiCrafting::onGuiClosed()
{
	GuiContainer::onGuiClosed();
}

void GuiCrafting::drawGuiContainerForegroundLayer()
{
	fontRenderer->drawString(StatCollector::translateToLocal("container.crafting"),  28, 6,             0x404040);
	fontRenderer->drawString(StatCollector::translateToLocal("container.inventory"),  8, (ySize - 96) + 2, 0x404040);
}

void GuiCrafting::drawGuiContainerBackgroundLayer(float_t partialTick)
{
	// Cached rather than looked up fresh every frame this screen is open --
	// same pattern/reasoning as this session's other texture-id-caching fixes.
	// CachedTextureId re-resolves itself if this id was ever invalidated by a
	// releaseTexture() call -- see its own comment in RenderEngine.h.
	static CachedTextureId cachedCraftingTextureId;
	int_t tex = cachedCraftingTextureId.get(mc->renderEngine, "/gui/crafting.png");
	renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	mc->renderEngine->bindTexture(tex);
	int_t guiX = (width  - xSize) / 2;
	int_t guiY = (height - ySize) / 2;
	drawTexturedModalRect(guiX, guiY, 0, 0, xSize, ySize);
}
