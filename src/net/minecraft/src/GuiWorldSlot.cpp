#include "GuiWorldSlot.h"
#include "GuiSelectWorld.h"
#include "GuiButton.h"
#include "SaveFormatComparator.h"
#include "MathHelper.h"
#include "StatCollector.h"

GuiWorldSlot::GuiWorldSlot(GuiSelectWorld *parent)
	: GuiSlot(parent->mc, parent->width, parent->height, 32, parent->height - 64, 46)
	, parentWorldGui(parent)
{
}

int_t GuiWorldSlot::getSize()
{
	return (int_t)parentWorldGui->saveList.size();
}

void GuiWorldSlot::elementClicked(int_t index, bool doubleClicked)
{
	parentWorldGui->selectedWorld = index;
	bool valid = parentWorldGui->selectedWorld >= 0
	          && parentWorldGui->selectedWorld < getSize();
	parentWorldGui->buttonSelect->enabled = valid;
	parentWorldGui->buttonRename->enabled = valid;
	parentWorldGui->buttonDelete->enabled = valid;
	if (doubleClicked && valid)
		parentWorldGui->selectWorld(index);
}

bool GuiWorldSlot::isSelected(int_t index)
{
	return index == parentWorldGui->selectedWorld;
}

int_t GuiWorldSlot::getContentHeight()
{
	return getSize() * 46;
}

void GuiWorldSlot::drawBackground()
{
	parentWorldGui->drawDefaultBackground();
}

void GuiWorldSlot::invalidateCache()
{
	cachedRows.clear();
}

void GuiWorldSlot::drawSlot(int_t i, int_t x, int_t y, int_t h, Tessellator *tess)
{
	if (cachedRows.size() != parentWorldGui->saveList.size())
		cachedRows.assign(parentWorldGui->saveList.size(), CachedRowText());

	CachedRowText &cached = cachedRows[(std::size_t)i];
	if (!cached.populated)
	{
		SaveFormatComparator *entry = parentWorldGui->saveList[i];
		cached.name = entry->getDisplayName();
		if (cached.name.empty())
			cached.name = parentWorldGui->worldLabel + " " + std::to_string(i + 1);

		cached.line2 = entry->getFileName() + " (" +
			parentWorldGui->formatDate(entry->getLastTimePlayed()) + ")";

		if (entry->requiresConversion())
		{
			cached.line3 = parentWorldGui->conversionLabel;
		}
		else
		{
			const int_t gameType = entry->getGameType();
			if (gameType >= 0 && gameType < 2)
				cached.line3 = parentWorldGui->gameModeLabels[gameType];
			if (entry->isHardcoreModeEnabled())
				cached.line3 = StatCollector::translateToLocal("gameMode.hardcore");
		}
		cached.line4 = "Seed: " + std::to_string(entry->getSeed());
		cached.populated = true;
	}

	parentWorldGui->drawString(parentWorldGui->fontRenderer, cached.name,  x + 2, y + 1,  0xffffff);
	parentWorldGui->drawString(parentWorldGui->fontRenderer, cached.line2, x + 2, y + 12, 0x808080);
	parentWorldGui->drawString(parentWorldGui->fontRenderer, cached.line3, x + 2, y + 22, 0x808080);
	parentWorldGui->drawString(parentWorldGui->fontRenderer, cached.line4, x + 2, y + 32, 0x808080);
}
