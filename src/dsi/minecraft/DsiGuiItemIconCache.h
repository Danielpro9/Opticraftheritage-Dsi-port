#pragma once
#ifdef DSI_PLATFORM

#include "java/Type.h"
#include "platform/RenderAPI.h"
#include "dsi/minecraft/DsiCapturedMeshRepack.h"

class RenderItem;
class FontRenderer;
class RenderEngine;
class ItemStack;

// Per-slot cache for RenderItem::drawItemIntoGui()'s icon (the 3D block cube
// or the flat 2D texture -- see that function's own comment on exactly
// which render paths this covers), for any GUI screen that redraws the same
// fixed set of item slots every frame: the hotbar (GuiIngame.cpp, always
// visible during play) and a container's slot grid (GuiContainer.cpp, while
// open). Real-hardware evidence this exists for: GuiIngame.cpp's own
// "hudItems" render-phase measured 6ms/frame average (up to 18% of a 30fps
// frame, from a real debug.log) for just the 9 always-visible hotbar icons,
// redrawn from scratch -- full per-vertex float math plus its own DMA kick,
// six faces' worth for a block item -- every single frame, even the vast
// majority where nothing in the hotbar changed at all.
//
// Does NOT cover the stack-count/durability-bar overlay
// (RenderItem::renderItemOverlayIntoGUI()) or the enchant glint
// (RenderItem::renderItemGlintOverlayIfNeeded()) -- both stay on their
// existing per-frame path. The overlay is individually far cheaper than the
// icon itself (a couple of characters, one bar quad, vs. up to six full
// textured+lit faces), and the glint genuinely must animate every frame
// (it scrolls with real time) -- see that function's own comment. A caller
// using this still calls renderItemGlintOverlayIfNeeded() itself, same as
// it always did via the ordinary renderItemIntoGUI() path.
struct DsiCachedGuiIcon
{
	RenderStaticMesh mesh;
	DsiMeshRepackState repack;
	// false until the first attempt; distinct from cacheable below, which
	// only matters once this is true.
	bool signatureValid = false;
	// Set alongside signatureValid on every (re)build attempt: true if
	// drawItemIntoGui()'s capture actually produced something (the item's
	// render path supports it -- see its own comment on which do). false
	// means this exact item/damage/stackSize combination was tried and
	// found NOT cacheable (a rarer block shape, or similar); the cache
	// remembers that so dsiDrawCachedItemIcon() does not keep re-attempting
	// a capture that will not work every single frame -- it just reports
	// "not handled" immediately once the signature is known, same cost as
	// if this cache did not exist for that one item.
	bool cacheable = false;
	int itemID = -1;
	int damage = -1;
	int stackSize = -1;
	// The GL texture (dsiGetBoundTexture()) that was bound while this mesh
	// was captured -- a 3D block icon is captured against terrain.png, a
	// flat icon against items.png, and this cache can hold either kind from
	// one call to the next. drawCapturedMeshFast()'s compiled command
	// stream never carries a texture bind of its own (same invariant
	// WorldRendererDsi.cpp's terrain meshes and GuiIngame.cpp's HUD caches
	// rely on for their own single, never-changing texture) -- it just
	// samples whatever is currently bound. A cache replay skips
	// drawItemIntoGui() entirely, so nothing else rebinds this between
	// slots; dsiDrawCachedItemIcon() must do it explicitly before replaying.
	int textureId = 0;
	// True iff this mesh came from the 3D-block capture branch (see
	// RenderItem::drawItemIntoGui()'s capturedAsBlock out-param). That
	// branch's capture only stores the cube's LOCAL, pre-transform geometry
	// -- the renderPushMatrix()/Translate()/Scale()/Rotate() dance that
	// positions, scales and rotates it into a recognisable isometric icon is
	// issued straight to the GPU and popped again before drawItemIntoGui()
	// returns, never captured into the mesh. A replay must redo that exact
	// transform itself (dsiDrawCachedItemIcon() does, when this is true) or
	// the cube draws at roughly the origin, unscaled and unrotated --
	// effectively invisible. The flat-icon branch has no such hazard (it
	// bakes (x, y) straight into the vertex positions, no wrapping
	// transform at all), so this is always false for that kind of icon.
	bool is3DBlock = false;
};

// Draws stack's icon at (x, y) using/updating cache, WITHOUT the stack-
// count/durability overlay or the enchant glint -- see this header's own
// comment on why those are not included. Returns true if this call fully
// handled the icon (a cached replay, or a freshly-captured rebuild -- both
// already drawn to the screen by the time this returns). Returns false if
// stack is null, the pop-in animation (ItemStack::animationsToGo) is
// active, or this exact item turned out not to support capture -- in every
// false case nothing was drawn, and the caller must fall back to its own
// ordinary itemRenderer->renderItemIntoGUI() call for this slot, exactly
// the same call it would have made before this cache existed.
bool dsiDrawCachedItemIcon(RenderItem *itemRenderer, FontRenderer *fontRenderer, RenderEngine *renderEngine,
	ItemStack *stack, int_t x, int_t y, DsiCachedGuiIcon &cache);

#endif // DSI_PLATFORM
