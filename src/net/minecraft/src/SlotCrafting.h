#pragma once

#include "Slot.h"

class EntityPlayer;
class IInventory;
class InventoryCrafting;
class ItemStack;

// net.minecraft.src.SlotCrafting
class SlotCrafting : public Slot
{
public:
	SlotCrafting(EntityPlayer *entityplayer, IInventory *iinventory, IInventory *iinventory1, int_t i, int_t j, int_t k);

	bool isItemValid(ItemStack *itemstack) override;
	ItemStack *decrStackSize(int_t amount) override;
	void onPickupFromSlot(ItemStack *itemstack) override;

protected:
	void onCrafting(ItemStack *itemstack, int_t amount) override;
	void onCrafting(ItemStack *itemstack) override;

private:
	// Both real construction sites (ContainerWorkbench.cpp, ContainerPlayer.cpp)
	// pass their own InventoryCrafting* here (declared IInventory* only because
	// the base Slot/IInventory API predates this class); narrowed so
	// onPickupFromSlot() can suppress/batch its recipe-rescan notifications --
	// see InventoryCrafting::setEventsSuppressed()'s own comment.
	InventoryCrafting *craftMatrix;
	EntityPlayer *thePlayer;
	int_t removeCount = 0;
};
