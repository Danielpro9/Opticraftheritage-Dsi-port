#ifndef RENDERITEM_H
#define RENDERITEM_H

#include "Render.h"
#include "java/Random.h"

class AxisAlignedBB;
class EntityItem;
class FontRenderer;
class RenderEngine;
class ItemStack;
struct RenderCapturedMesh;

class RenderItem : public Render {
public:
    RenderItem();

    void doRenderItem(EntityItem* entityitem, double d, double d1, double d2, float f, float f1);

    static void renderAABB(AxisAlignedBB *aabb);
    // captureOut/captureAppend: threaded down to renderTexturedQuad()/
    // RenderBlocks::renderBlockOnInventory() below -- see those functions'
    // own comments. Only the flat-icon and simple-full-cube render paths
    // honour it (the common cases); anything else (the end portal's special
    // icon, rarer block shapes) always draws immediately regardless, same
    // as before this parameter existed. A caller that wants a cached icon
    // must already know it is one of the eligible cases before passing a
    // non-null captureOut -- this function does not re-check eligibility
    // on its caller's behalf for the branches where that would be unsafe.
    //
    // capturedAsBlock (optional out-param): set to true iff captureOut was
    // actually honoured through the 3D-block branch. A caller needs to know
    // this because that branch's capture only stores the cube's LOCAL
    // (pre-transform) vertex data -- the renderPushMatrix()/renderTranslate()/
    // renderScale()/renderRotate() calls that position, scale and rotate it
    // into a recognisable isometric icon at (l, i1) are issued straight to
    // the GPU's matrix stack and popped again before this function returns,
    // never captured into the mesh itself. Replaying that mesh later without
    // redoing the identical matrix sequence draws the bare unit cube with
    // whatever transform happens to be active at replay time instead --
    // typically the plain GUI projection, which puts it at the origin,
    // unscaled and unrotated: for all practical purposes invisible. The
    // flat-icon branch (renderTexturedQuad()) has no such hazard -- it bakes
    // (l, i1) straight into the vertex positions it adds, with no wrapping
    // transform at all -- so capturedAsBlock is only ever set for the block
    // branch; a caller that doesn't care about this distinction may pass
    // nullptr and lose nothing it was using before.
    void drawItemIntoGui(FontRenderer* fontrenderer, RenderEngine* renderengine, int i, int j, int k, int l, int i1,
        RenderCapturedMesh* captureOut = nullptr, bool captureAppend = false, bool* capturedAsBlock = nullptr);
    void renderItemIntoGUI(FontRenderer* fontrenderer, RenderEngine* renderengine, ItemStack* itemstack, int i, int j,
        RenderCapturedMesh* captureOut = nullptr, bool captureAppend = false);
    void renderItemOverlayIntoGUI(FontRenderer* fontrenderer, RenderEngine* renderengine, ItemStack* itemstack, int i, int j);
    // Split out of renderItemIntoGUI() -- see that function's own comment --
    // so a caller replaying a cached icon can still show this every frame
    // without going through drawItemIntoGui() again.
    void renderItemGlintOverlayIfNeeded(RenderEngine* renderengine, ItemStack* itemstack, int i, int j);
    void renderTexturedQuad(int i, int j, int k, int l, int i1, int j1,
        RenderCapturedMesh* captureOut = nullptr, bool captureAppend = false);

    virtual void doRender(Entity* entity, double d, double d1, double d2, float f, float f1) override;

    bool field_27004_a;
    float zLevel;

private:
    void renderQuad(Tessellator* tessellator, int i, int j, int k, int l, int i1);
    void renderGuiItemGlint(int seed, int x, int y, int width, int height);

    Random random;
};

#endif
