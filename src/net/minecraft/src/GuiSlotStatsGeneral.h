#pragma once

#include "GuiSlot.h"
#include <string>
#include <vector>

class GuiStats;
class Tessellator;

// net.minecraft.src.GuiSlotStatsGeneral
class GuiSlotStatsGeneral : public GuiSlot
{
public:
	GuiSlotStatsGeneral(GuiStats *guistats);

protected:
	int_t getSize() override;
	void elementClicked(int_t index, bool doubleClicked) override;
	bool isSelected(int_t index) override;
	int_t getContentHeight() override;
	void drawBackground() override;
	void drawSlot(int_t index, int_t x, int_t y, int_t height, Tessellator *tess) override;

	GuiStats *parentGui;

private:
	// drawSlot() used to call StatBase::format() (string formatting) and
	// FontRenderer::getStringWidth() (a heap-allocating UTF-16 conversion +
	// per-char scan) every frame for every visible row, even though a stat's
	// formatted value only changes when the underlying counter does --
	// StatList::generalStats itself never resizes at runtime, only the
	// per-stat values do, so this is invalidated per row by comparing
	// StatFileWriter::writeStat()'s returned value (a cheap map lookup) each
	// frame instead of a list-rebuild hook like GuiWorldSlot's row cache
	// needs.
	struct CachedRow
	{
		int_t lastValue = 0;
		std::string text;
		int_t width = 0;
		bool populated = false;
	};
	std::vector<CachedRow> cachedRows;
};
