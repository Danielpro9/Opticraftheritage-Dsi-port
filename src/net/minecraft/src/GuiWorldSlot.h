#pragma once

#include "GuiSlot.h"
#include <string>
#include <vector>

class GuiSelectWorld;
class Tessellator;

// net.minecraft.src.GuiWorldSlot
class GuiWorldSlot : public GuiSlot
{
public:
	GuiWorldSlot(GuiSelectWorld *parent);

	// Called by GuiSelectWorld::loadSaves() whenever saveList is rebuilt (e.g.
	// after deleting a world) so this row-text cache never outlives the
	// SaveFormatComparator entries it was built from. Safe/harmless to call
	// when the cache is already empty (first ever load).
	void invalidateCache();

protected:
	int_t getSize() override;
	void elementClicked(int_t index, bool doubleClicked) override;
	bool isSelected(int_t index) override;
	int_t getContentHeight() override;
	void drawBackground() override;
	void drawSlot(int_t index, int_t x, int_t y, int_t height, Tessellator *tess) override;

	GuiSelectWorld *parentWorldGui;

private:
	// drawSlot() used to rebuild these 3 lines from scratch every frame for
	// every visible row -- including a localtime_r()+strftime() call for the
	// "last played" date -- even though a save entry's name/date/game-mode
	// text never changes while the list is displayed. Lazily populated on
	// first draw per row, cleared only when saveList itself changes.
	struct CachedRowText
	{
		std::string name;
		std::string line2;
		std::string line3;
		// Ported from upstream OptiCraftHeritageEdition (commits
		// 308782d/9ec548f): the world's seed, shown as its own line so a
		// player can see/record it without digging into the save file.
		std::string line4;
		bool populated = false;
	};
	std::vector<CachedRowText> cachedRows;
};
