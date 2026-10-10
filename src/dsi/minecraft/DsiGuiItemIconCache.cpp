#include "dsi/minecraft/DsiGuiItemIconCache.h"

#ifdef DSI_PLATFORM

#include "net/minecraft/src/RenderItem.h"
#include "net/minecraft/src/ItemStack.h"

bool dsiDrawCachedItemIcon(RenderItem *itemRenderer, FontRenderer *fontRenderer, RenderEngine *renderEngine,
	ItemStack *stack, int_t x, int_t y, DsiCachedGuiIcon &cache)
{
	if (stack == nullptr)
		return false;

	// GuiIngame::renderInventorySlot()'s pop-in scale animation must
	// visibly change every frame it is active -- never cacheable while it
	// runs (both the hotbar and the container slot grid drive the same
	// ItemStack::animationsToGo field, so this one check covers both
	// callers).
	if (stack->animationsToGo > 0)
		return false;

	const int damage = stack->getItemDamage();
	const int stackSize = stack->stackSize;
	const bool signatureMatches = cache.signatureValid &&
		cache.itemID == stack->itemID && cache.damage == damage && cache.stackSize == stackSize;

	bool handled = false;
	if (!signatureMatches)
	{
		cache.mesh.captured.clear();
		// drawItemIntoGui(), not renderItemIntoGUI(): the glint overlay
		// renderItemIntoGUI() would also draw here always draws immediately
		// regardless of captureOut (see its own comment) -- calling it
		// through here would draw the glint now AND again below, since this
		// function always calls renderItemGlintOverlayIfNeeded() itself
		// exactly once, covering both the rebuild and the cache-hit path
		// with the same line.
		itemRenderer->drawItemIntoGui(fontRenderer, renderEngine, stack->itemID, damage,
			stack->getIconIndex(), (int)x, (int)y, &cache.mesh.captured, false);
		cache.cacheable = !cache.mesh.captured.empty();
		cache.signatureValid = true;
		cache.itemID = stack->itemID;
		cache.damage = damage;
		cache.stackSize = stackSize;
		if (cache.cacheable)
		{
			// drawItemIntoGui() just bound whichever texture this item's
			// render path uses (terrain.png for a 3D block, items.png for a
			// flat icon) -- capture it now so a later cache-hit replay (which
			// never calls drawItemIntoGui() again) knows what to rebind.
			cache.textureId = dsiGetBoundTexture();
			dsiAdvanceMeshRepack(cache.repack, cache.mesh, true);
			handled = renderStaticMeshDraw(cache.mesh);
		}
		else
		{
			// drawItemIntoGui()'s own internal fallback already drew this
			// correctly on its own (this item's render path does not
			// support capture -- see that function's own comment on which
			// do) -- nothing left for this call to do but still show the
			// glint below if it applies.
			handled = true;
		}
	}
	else if (cache.cacheable)
	{
		// Must rebind before dsiAdvanceMeshRepack() too, not just before the
		// draw below: an in-progress repack's stage 0 (texcoord conversion)
		// still reads the currently-bound texture's width/height, and this
		// replay path is the only one that can run across several frames
		// without drawItemIntoGui() ever rebinding anything in between.
		renderBindTexture(cache.textureId);
		dsiAdvanceMeshRepack(cache.repack, cache.mesh, false);
		handled = renderStaticMeshDraw(cache.mesh);
	}
	// else: signature matches but this exact item/damage/stackSize was
	// already found not cacheable -- handled stays false, caller falls back
	// to its own uncached renderItemIntoGUI() call, same as every frame
	// before this cache existed.

	if (handled)
		itemRenderer->renderItemGlintOverlayIfNeeded(renderEngine, stack, (int)x, (int)y);
	return handled;
}

#endif // DSI_PLATFORM
