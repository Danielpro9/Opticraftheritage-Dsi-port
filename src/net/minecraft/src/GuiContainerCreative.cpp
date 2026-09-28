#include "GuiContainerCreative.h"

#include "AchievementList.h"
#include "ContainerCreative.h"
#include "EntityPlayer.h"
#include "EntityPlayerSP.h"
#include "FontRenderer.h"
#include "GameSettings.h"
#include "GuiAchievements.h"
#include "GuiButton.h"
#include "GuiInventory.h"
#include "GuiStats.h"
#include "InventoryBasic.h"
#include "InventoryPlayer.h"
#include "ItemStack.h"
#include "Minecraft.h"
#include "PlayerController.h"
#include "RenderEngine.h"
#include "Slot.h"
#include "StatCollector.h"
#include "pc/lwjgl/Mouse.h"
#include "platform/PlatformConfig.h"
#include "platform/RenderAPI.h"

InventoryBasic GuiContainerCreative::inventory("tmp", 72, false);

GuiContainerCreative::GuiContainerCreative(EntityPlayer *player)
    : GuiContainer(new ContainerCreative(player), true)
    , currentScroll(0.0f)
    , isScrolling(false)
    , wasClicking(false)
{
    player->craftingInventory = inventorySlots;
    field_948_f = true;
    player->addStat(AchievementList::openInventory, 1);
    ySize = 208;
}

InventoryBasic *GuiContainerCreative::getInventory()
{
    return &inventory;
}

void GuiContainerCreative::updateScreen()
{
    if (!mc->playerController->isInCreativeMode())
    {
        mc->displayGuiScreen(new GuiInventory(mc->thePlayer));
        return;
    }
    GuiContainer::updateScreen();
}

void GuiContainerCreative::handleMouseClick(Slot *slot, int_t slotId, int_t button, bool shift)
{
    InventoryPlayer *playerInventory = mc->thePlayer->inventory;

    if (slot != nullptr)
    {
        if (slot->getInventory() == &inventory)
        {
            ItemStack *held = playerInventory->getItemStack();
            ItemStack *listed = slot->getStack();

            if (held != nullptr && listed != nullptr && held->itemID == listed->itemID)
            {
                if (button == 0)
                {
                    if (shift)
                        held->stackSize = held->getMaxStackSize();
                    else if (held->stackSize < held->getMaxStackSize())
                        ++held->stackSize;
                }
                else if (held->stackSize <= 1)
                {
                    delete held;
                    playerInventory->setItemStack(nullptr);
                }
                else
                {
                    --held->stackSize;
                }
            }
            else if (held != nullptr)
            {
                delete held;
                playerInventory->setItemStack(nullptr);
            }
            else if (listed == nullptr)
            {
                playerInventory->setItemStack(nullptr);
            }
            else
            {
                ItemStack *copy = ItemStack::copyItemStack(listed);
                if (copy != nullptr && shift)
                    copy->stackSize = copy->getMaxStackSize();
                playerInventory->setItemStack(copy);
            }
            return;
        }

        inventorySlots->slotClick(slot->slotNumber, button, shift, mc->thePlayer);
        ItemStack *stack = inventorySlots->getSlot(slot->slotNumber)->getStack();
        int_t packetSlot = slot->slotNumber - (int_t)inventorySlots->slots.size() + 45;
        mc->playerController->sendSlotPacket(stack, packetSlot);
        return;
    }

    ItemStack *held = playerInventory->getItemStack();
    if (held == nullptr)
        return;

    if (button == 0)
    {
        playerInventory->setItemStack(nullptr);
        mc->thePlayer->dropPlayerItem(held);
        mc->playerController->sendPacketDropItem(held);
    }
    else if (button == 1)
    {
        ItemStack *dropped = held->splitStack(1);
        mc->thePlayer->dropPlayerItem(dropped);
        mc->playerController->sendPacketDropItem(dropped);
        if (held->stackSize == 0)
        {
            delete held;
            playerInventory->setItemStack(nullptr);
        }
    }
}

void GuiContainerCreative::initGui()
{
    if (!mc->playerController->isInCreativeMode())
    {
        mc->displayGuiScreen(new GuiInventory(mc->thePlayer));
        return;
    }

#if PLATFORM_DSI
    // Same release onGuiClosed() below already does, moved to also run on
    // OPEN: that fix only helped the SECOND time this screen was entered in
    // a session -- freeing tunnel.png/particlefield.png (the End Portal
    // item-icon assets, almost never actually needed) on close came too
    // late for the very FIRST open, when this screen's own '/gui/allitems.png'
    // and every real item icon it draws (via '/gui/items.png') still had to
    // compete with whatever those two were holding. Real-hardware report:
    // opening creative for the first time in a session could still show a
    // blank inventory (VRAM already tight from a freshly-loaded world's
    // terrain.png/mob skins) -- releasing here too gives that first attempt
    // the same headroom every later one already gets.
    if (mc != nullptr && mc->renderEngine != nullptr)
    {
        mc->renderEngine->releaseTexture("/misc/tunnel.png");
        mc->renderEngine->releaseTexture("/misc/particlefield.png");
    }
#endif

    GuiContainer::initGui();
    for (GuiButton *button : controlList)
        delete button;
    controlList.clear();
}

void GuiContainerCreative::handleMouseInput()
{
    GuiContainer::handleMouseInput();
    int_t wheel = lwjgl::Mouse::getEventDWheel();
    if (wheel == 0)
        return;

    ContainerCreative *container = static_cast<ContainerCreative *>(inventorySlots);
    int_t rows = (int_t)container->itemList.size() / 8 - 8 + 1;
    if (rows <= 0)
        return;

    wheel = wheel > 0 ? 1 : -1;
    currentScroll -= (float_t)wheel / (float_t)rows;
    if (currentScroll < 0.0f) currentScroll = 0.0f;
    if (currentScroll > 1.0f) currentScroll = 1.0f;
    container->scrollTo(currentScroll);
}

void GuiContainerCreative::onGuiClosed()
{
    GuiContainer::onGuiClosed();

#if PLATFORM_DSI
    // Real-hardware evidence (the vram= diagnostic added this round): a
    // session's resident-texture dump, taken while just exploring an
    // ordinary overworld far from any end portal, still showed
    // '/misc/tunnel.png' and '/misc/particlefield.png' resident -- the two
    // textures renderEndPortalGuiIcon() (RenderItem.cpp) uses to draw the
    // End Portal block's animated-swirl ITEM ICON, needed only because this
    // screen renders every placeable block's icon, including that one, in
    // its scrollable grid. Nothing in normal gameplay (not being near a
    // real end portal) ever touches them again, but nothing ever released
    // them either -- once loaded here, they sat in the 512KB texture-image
    // budget for the rest of the session. That log also caught
    // terrain.png's own real-quality (non-paletted) upload failing for
    // want of exactly the kind of room these two textures (plus this
    // screen's own '/gui/allitems.png') were quietly still holding:
    // releasing all three here, the same pattern
    // ClientPlatformPolicy_DSI.cpp's releaseWorldEntryAssets() already uses
    // for menu-only textures, gives that budget the best real chance it can
    // get without this screen ever having been opened at all.
    if (mc != nullptr && mc->renderEngine != nullptr)
    {
        mc->renderEngine->releaseTexture("/gui/allitems.png");
        mc->renderEngine->releaseTexture("/misc/tunnel.png");
        mc->renderEngine->releaseTexture("/misc/particlefield.png");
    }
#endif
}

Slot *GuiContainerCreative::getControllerNavigationTarget(Slot *selected, int_t dirX, int_t dirY)
{
#if PLATFORM_PS2 || PLATFORM_WII || PLATFORM_DSI
	// DSi report: the D-pad could not reach items past the first visible page
	// of the creative inventory -- this override (row-to-row navigation, plus
	// scrollRows()/hotbar-step at the grid's top/bottom edge) is what PS2/Wii
	// already use for exactly that, missing DSi from this guard only. No
	// DSi-specific chord needed: the D-pad is free while any GuiScreen is
	// open (gameplay movement is not processed then), so plain Up/Down here
	// behaves identically to PS2/Wii -- consistent with the rest of
	// ContainerSlotNavigator's DSi wiring, which already reuses this same
	// mechanism unmodified.
    if (selected == nullptr || selected->getInventory() != &inventory || inventorySlots == nullptr)
        return nullptr;

    const int_t selectedIndex = selected->slotNumber;
    if (selectedIndex < 0 || selectedIndex >= 72)
        return nullptr;

    const int_t column = selectedIndex % 8;
    const int_t row = selectedIndex / 8;
    if (dirX != 0)
    {
        const int_t targetColumn = column + dirX;
        if (targetColumn < 0 || targetColumn >= 8)
            return selected;
        return inventorySlots->slots[row * 8 + targetColumn];
    }

    if (dirY < 0)
    {
        if (row > 0)
            return inventorySlots->slots[(row - 1) * 8 + column];
        scrollRows(-1);
        return selected;
    }

    if (dirY > 0)
    {
        if (row < 8)
            return inventorySlots->slots[(row + 1) * 8 + column];
#if PLATFORM_DSI
        // Real-hardware report: pressing Down at the last visible row jumped
        // straight to the hotbar and could never reach items further down the
        // list -- the "step onto the hotbar instead of scrolling" choice below
        // (PS2/Wii's own comment: "scrolling is the right stick's job") relies
        // on an analog stick DSi does not have, so nothing else ever advanced
        // currentScroll downward for it. Asymmetric with the dirY < 0 branch
        // above, which already tries scrollRows(-1) before giving up -- mirror
        // that here: scroll first, and only step onto the hotbar once
        // scrollRows(1) reports there is nothing left to reveal (already on
        // the last row).
        if (scrollRows(1))
            return selected;
#endif
        // Below the last grid row sits the hotbar (slots 72..80, same column
        // pitch). Step onto it instead of scrolling the list; scrolling is the
        // right stick's job, so the D-pad can always reach the hotbar.
        const int_t hotbarIndex = 72 + column;
        if (hotbarIndex < (int_t)inventorySlots->slots.size())
            return inventorySlots->slots[hotbarIndex];
        return selected;
    }
#endif
    return nullptr;
}

bool GuiContainerCreative::scrollRows(int_t direction)
{
    if (direction == 0 || inventorySlots == nullptr)
        return false;

    ContainerCreative *container = static_cast<ContainerCreative *>(inventorySlots);
    const int_t rows = (int_t)container->itemList.size() / 8 - 8 + 1;
    if (rows <= 0)
        return false;

    const int_t firstRow = (int_t)((double)(currentScroll * (float_t)rows) + 0.5);
    int_t targetRow = firstRow + direction;
    if (targetRow < 0) targetRow = 0;
    if (targetRow > rows) targetRow = rows;
    if (targetRow == firstRow)
        return false;

    currentScroll = (float_t)targetRow / (float_t)rows;
    container->scrollTo(currentScroll);
    return true;
}

void GuiContainerCreative::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    bool pointerScrollingAllowed = true;
#if PLATFORM_PS2
    pointerScrollingAllowed = mc == nullptr || mc->gameSettings == nullptr || !mc->gameSettings->legacyUI;
#endif
    bool clicking = pointerScrollingAllowed && lwjgl::Mouse::isButtonDown(0);
    int_t guiLeft = (width - xSize) / 2;
    int_t guiTop = (height - ySize) / 2;
    int_t scrollLeft = guiLeft + 155;
    int_t scrollTop = guiTop + 17;
    int_t scrollRight = scrollLeft + 14;
    int_t scrollBottom = scrollTop + 162;

    if (!wasClicking && clicking && mouseX >= scrollLeft && mouseY >= scrollTop
        && mouseX < scrollRight && mouseY < scrollBottom)
        isScrolling = true;
    if (!clicking)
        isScrolling = false;
    wasClicking = clicking;

    if (isScrolling)
    {
        currentScroll = (float_t)(mouseY - (scrollTop + 8)) / ((float_t)(scrollBottom - scrollTop) - 16.0f);
        if (currentScroll < 0.0f) currentScroll = 0.0f;
        if (currentScroll > 1.0f) currentScroll = 1.0f;
        static_cast<ContainerCreative *>(inventorySlots)->scrollTo(currentScroll);
    }

    GuiContainer::drawScreen(mouseX, mouseY, partialTick);
    renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    renderDisable(RenderCapability::Lighting);
}

void GuiContainerCreative::drawGuiContainerForegroundLayer()
{
    fontRenderer->drawString(StatCollector::translateToLocal("container.creative"), 8, 6, 0x404040);
}

void GuiContainerCreative::drawGuiContainerBackgroundLayer(float_t)
{
    // Cached rather than looked up fresh every frame -- same pattern/reasoning
    // as this session's other texture-id-caching fixes. This runs every frame
    // the creative inventory screen is open. CachedTextureId (RenderEngine.h)
    // re-resolves itself if this id was ever invalidated by a
    // releaseTexture() call -- onGuiClosed() right below releases
    // "/gui/allitems.png" by this exact name every time this screen closes,
    // so a bare cached id here would go stale the very first time this
    // screen is closed and reopened; see CachedTextureId's own comment.
    static CachedTextureId cachedAllItemsTextureId;
    renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    mc->renderEngine->bindTexture(cachedAllItemsTextureId.get(mc->renderEngine, "/gui/allitems.png"));
    int_t guiLeft = (width - xSize) / 2;
    int_t guiTop = (height - ySize) / 2;
    drawTexturedModalRect(guiLeft, guiTop, 0, 0, xSize, ySize);
    int_t scrollTop = guiTop + 17;
    int_t scrollBottom = scrollTop + 162;
    drawTexturedModalRect(guiLeft + 154,
                          guiTop + 17 + (int_t)((float_t)(scrollBottom - scrollTop - 17) * currentScroll),
                          0, 208, 16, 16);
}

void GuiContainerCreative::actionPerformed(GuiButton *button)
{
    if (button->id == 0)
        mc->displayGuiScreen(new GuiAchievements(mc->statFileWriter));
    else if (button->id == 1)
        mc->displayGuiScreen(new GuiStats(this, mc->statFileWriter));
}
