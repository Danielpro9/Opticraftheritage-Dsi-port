#include "GuiSlotStatsGeneral.h"
#include "GuiStats.h"
#include "StatList.h"
#include "StatBase.h"
#include "StatFileWriter.h"
#include "FontRenderer.h"
#include "Minecraft.h"
#include "Tessellator.h"

GuiSlotStatsGeneral::GuiSlotStatsGeneral(GuiStats *guistats)
	: GuiSlot(GuiStats::getMc(guistats), guistats->width, guistats->height, 32, guistats->height - 64, 10)
	, parentGui(guistats)
{
	setShowSelectionBox(false);
}

int_t GuiSlotStatsGeneral::getSize()
{
	return (int_t)StatList::generalStats.size();
}

void GuiSlotStatsGeneral::elementClicked(int_t i, bool flag)
{
}

bool GuiSlotStatsGeneral::isSelected(int_t i)
{
	return false;
}

int_t GuiSlotStatsGeneral::getContentHeight()
{
	return getSize() * 10;
}

void GuiSlotStatsGeneral::drawBackground()
{
	parentGui->drawDefaultBackground();
}

void GuiSlotStatsGeneral::drawSlot(int_t i, int_t x, int_t y, int_t h, Tessellator *tess)
{
	StatBase *statbase = StatList::generalStats[i];
	FontRenderer *fr   = GuiStats::getFontRenderer(parentGui);
	StatFileWriter *sfw = GuiStats::getStatFileWriter(parentGui);
	int_t color = (i % 2 != 0) ? 0x909090 : 0xffffff;
	parentGui->drawString(fr, statbase->statName, x + 2, y + 1, color);

	if (cachedRows.size() != StatList::generalStats.size())
		cachedRows.assign(StatList::generalStats.size(), CachedRow());

	CachedRow &cached = cachedRows[(std::size_t)i];
	const int_t value = sfw->writeStat(statbase);
	if (!cached.populated || cached.lastValue != value)
	{
		cached.lastValue = value;
		cached.text = statbase->format(value);
		cached.width = fr->getStringWidth(cached.text);
		cached.populated = true;
	}
	parentGui->drawString(fr, cached.text, (x + 2 + 213) - cached.width, y + 1, color);
}
