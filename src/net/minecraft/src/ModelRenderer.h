#pragma once

#include <string>
#include <vector>

#include "java/Type.h"
#include "platform/PlatformConfig.h"
#if PLATFORM_MODEL_IMMEDIATE
#include "platform/RenderAPI.h"
#include "dsi/minecraft/DsiCapturedMeshRepack.h"
#endif

class ModelBase;
class ModelBox;
class Tessellator;

// net.minecraft.src.ModelRenderer
class ModelRenderer
{
public:
    ModelRenderer(int_t i, int_t j);
    explicit ModelRenderer(ModelBase* baseModel);
    ModelRenderer(ModelBase* baseModel, int_t i, int_t j);
    ModelRenderer(ModelBase* baseModel, const std::string& boxName);
    ~ModelRenderer();

    ModelRenderer* addBox(float f, float f1, float f2, int_t i, int_t j, int_t k);
    ModelRenderer* addBox(float f, float f1, float f2, int_t i, int_t j, int_t k, float f3);
    ModelRenderer* addBox(const std::string& name, float f, float f1, float f2, int_t i, int_t j, int_t k);
    void addChild(ModelRenderer* child);
    ModelRenderer* setTextureOffset(int_t x, int_t y);
    ModelRenderer* setTextureSize(int_t width, int_t height);
    void setRotationPoint(float f, float f1, float f2);
    void render(float f);
    void renderWithRotation(float f);
    void postRender(float f);
    void ensureCompiled(float scale);

private:
    void init(ModelBase* baseModel, const std::string& boxName, int_t textureX, int_t textureY);
    void invalidateCompiledGeometry();
    void compileDisplayList(float f);
    void drawGeometry(float f);
    void renderChildren(float f);
#if PLATFORM_MODEL_IMMEDIATE || PLATFORM_MODEL_PERSISTENT_MESH
    void renderImmediate(float f);
#endif

    int_t textureOffsetX;
    int_t textureOffsetY;
    bool compiled;
    float compiledScale;
    int_t displayList;
    ModelBase* baseModel;
    std::string boxName;
    std::vector<ModelBox*> cubeList;
    std::vector<ModelRenderer*> childModels;
#if PLATFORM_MODEL_IMMEDIATE
    // DSi has no true persistent-mesh handle (RenderAPI_DSI.cpp implements
    // neither renderCreatePersistentMesh() nor renderDrawPersistentMesh()),
    // but RenderStaticMesh's captured-RAM-buffer fallback -- the same
    // mechanism WorldRendererDsi.cpp uses for terrain and GuiIngame.cpp for
    // the HUD -- works here too: box geometry (position/UV/normal) is fixed
    // once built, and the only thing that varies per entity/frame is the
    // matrix transform applied around drawGeometry(), which this mesh never
    // captures. See compileDisplayList()/drawGeometry()'s own comments.
    RenderStaticMesh immediateMesh;
    // Drives immediateMesh's v16/t16 + compiled GX FIFO command list repack
    // across several frames after a (re)compile -- see
    // dsiAdvanceMeshRepack()'s own comment (DsiCapturedMeshRepack.h, shared
    // with GuiIngame.cpp's HUD caches) for the full why. Entity model parts
    // carry per-vertex normals (TexturedQuad::emitInto() always calls
    // setNormal()), which dsiRepackCapturedMeshStep() only recently learned
    // to pack -- see that function's own comment on the format this adds.
    DsiMeshRepackState immediateMeshRepack;
#endif

public:
    float textureWidth;
    float textureHeight;
    float rotationPointX;
    float rotationPointY;
    float rotationPointZ;
    float rotateAngleX;
    float rotateAngleY;
    float rotateAngleZ;
    bool mirror;
    bool showModel;
    bool field_1402_i;
};
