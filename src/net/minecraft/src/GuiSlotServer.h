#pragma once

#include <string>
#include <vector>

#include "GuiSlot.h"

class GuiMultiplayer;

// net.minecraft.src.GuiSlotServer
class GuiSlotServer : public GuiSlot
{
public:
    explicit GuiSlotServer(GuiMultiplayer *parent);

    // Console D-pad navigation (GuiMultiplayer::handleConsoleServerListNavigation()):
    // selects `index` the same way clicking its row would, then scrolls it
    // into view -- a D-pad move has no mouse position of its own to hit-test
    // against, unlike elementClicked()'s normal caller (GuiSlot::drawScreen()).
    void selectIndex(int_t index);

protected:
    int_t getSize() override;
    void elementClicked(int_t index, bool doubleClicked) override;
    bool isSelected(int_t index) override;
    int_t getContentHeight() override;
    void drawBackground() override;
    void drawSlot(int_t index, int_t x, int_t y, int_t height, Tessellator *tessellator) override;

private:
    GuiMultiplayer *parentGui;

    // The one real per-frame cost drawSlot() used to pay for every visible
    // row (getStringWidth() heap-allocates a UTF-16 buffer per call) --
    // server->playerCount only changes when a background ping thread
    // finishes (ThreadPollServers), a rare event compared to every rendered
    // frame this list is on screen.
    struct CachedRow
    {
        bool populated = false;
        std::string playerCount;
        int_t playerCountWidth = 0;
    };
    std::vector<CachedRow> cachedRows;
};
