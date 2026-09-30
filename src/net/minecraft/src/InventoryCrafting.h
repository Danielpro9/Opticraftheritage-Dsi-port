#pragma once

#include <string>

#include "IInventory.h"
#include "java/Type.h"

class Container;
class EntityPlayer;
class ItemStack;

// net.minecraft.src.InventoryCrafting
class InventoryCrafting : public IInventory
{
public:
	InventoryCrafting(Container *container, int_t i, int_t j);
	~InventoryCrafting() override;

	int_t getSizeInventory() override;
	ItemStack *getStackInSlot(int_t i) override;
	ItemStack *getStackInSlotOnClosing(int_t i) override;
	// Java: func_21103_b(row, col) — readable name.
	ItemStack *getStackInRowAndColumn(int_t i, int_t j);
	std::string getInvName() override;
	ItemStack *decrStackSize(int_t i, int_t j) override;
	void setInventorySlotContents(int_t i, ItemStack *itemstack) override;
	int_t getInventoryStackLimit() override;
	void onInventoryChanged() override;
	bool canInteractWith(EntityPlayer *entityplayer) override;

	// ShapedRecipes is the only legacy caller for `getStackAt`; alias for clarity.
	ItemStack *getStackAt(int_t i, int_t j) { return getStackInRowAndColumn(i, j); }

	// SlotCrafting::onPickupFromSlot() consumes every filled ingredient slot
	// one at a time; each decrStackSize()/setInventorySlotContents() call
	// below normally re-triggers onCraftMatrixChanged(), which re-scans all
	// ~130 registered recipes to rebuild the (about-to-be-overwritten-again)
	// craft result -- wasted work for every ingredient but the last one,
	// since nothing reads the craft result in between. Suppress the
	// callback for the duration of that loop and fire it once at the end
	// instead; same end state, one recipe scan instead of up to nine.
	void setEventsSuppressed(bool suppressed) { eventsSuppressed = suppressed; }
	void notifyCraftMatrixChanged();

private:
	ItemStack **stackList;
	int_t stackListSize;
	int_t inventoryWidth;
	Container *eventHandler;
	bool eventsSuppressed = false;
};
