#pragma once

#include <string>
#include "GuiSlot.h"
#include <vector>
#include <functional>
#include <unordered_map>

class GuiStats;
class StatCrafting;
class Tessellator;

// net.minecraft.src.GuiSlotStats
class GuiSlotStats : public GuiSlot
{
public:
	GuiSlotStats(GuiStats *guistats);
	int_t getSize() override;

protected:
	void elementClicked(int_t index, bool doubleClicked) override;
	bool isSelected(int_t index) override;
	void drawBackground() override;

	void drawHeader(int_t x, int_t y, Tessellator *tess) override;
	void clickedHeader(int_t x, int_t y) override;
	void renderHoverState(int_t mouseX, int_t mouseY) override;

	StatCrafting *getStatAt(int_t index);
	virtual std::string getColumnLabel(int_t column) = 0;

	void drawStatValue(StatCrafting *statcrafting, int_t x, int_t y, bool highlight);
	void drawItemTooltip(StatCrafting *statcrafting, int_t mouseX, int_t mouseY);
	void toggleSort(int_t column);

public:
	// Public so SorterStats* comparator classes can access them
	int_t hoveredColumn;
	std::vector<StatCrafting *> statEntries;
	std::function<int(StatCrafting *, StatCrafting *)> statComparator;
	int_t sortColumn;
	int_t sortDirection;
	GuiStats *parentGui;

private:
	// drawStatValue() used to call StatCrafting::format() (StatBase::
	// numberFormat(), which constructs an std::ostringstream and calls
	// oss.imbue(std::locale("")) -- a real, non-trivial locale construction,
	// not just a cheap lookup) plus FontRenderer::getStringWidth() every
	// call, 3x per visible row (one per column), every frame either the
	// items or blocks stats screen is open. statcrafting entries are static/
	// long-lived (StatList's registry), so caching by pointer identity needs
	// no list-rebuild invalidation hook, only a per-entry value check --
	// same reasoning as GuiSlotStatsGeneral's already-fixed row cache.
	struct CachedStatValue
	{
		int_t lastValue = 0;
		std::string text;
		int_t width = 0;
		bool populated = false;
	};
	std::unordered_map<StatCrafting *, CachedStatValue> cachedStatValues;
};
