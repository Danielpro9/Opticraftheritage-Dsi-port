#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "platform/PlatformConfig.h"


enum class RenderPrimitive
{
    Points = 0x0000,
    Lines = 0x0001,
    LineLoop = 0x0002,
    LineStrip = 0x0003,
    Triangles = 0x0004,
    TriangleStrip = 0x0005,
    TriangleFan = 0x0006,
    Quads = 0x0007
};

enum class RenderCapability
{
    Texture2D,
    ColorMaterial,
    CullFace,
    AlphaTest,
    Blend,
    DepthTest,
    Fog,
    Lighting,
    Normalize,
    RescaleNormal,
    Light0,
    Light1,
    PolygonOffsetFill
};

enum class RenderCompare
{
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class RenderBlendFactor
{
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    DstColor,
    OneMinusDstColor
};

enum class RenderFace
{
    Front,
    Back,
    FrontAndBack
};

enum class RenderFogParameter
{
    Density,
    Start,
    End,
    Mode,
    Color,
    DistanceMode
};

enum class RenderFogMode
{
    Exp,
    Exp2,
    Linear,
    EyeRadial
};

enum class RenderLightParameter
{
    Ambient,
    Diffuse,
    Specular,
    Position
};


enum class RenderColorMaterialMode
{
    Ambient,
    AmbientAndDiffuse
};

enum class RenderShadeModel
{
    Flat,
    Smooth
};

enum class RenderMatrixQuery
{
    ModelView,
    Projection,
    Texture
};

enum class RenderStringQuery
{
    Vendor,
    Renderer,
    Version,
    Extensions
};

enum class RenderHintMode
{
    Fastest,
    Nicest
};

enum class RenderFeature
{
    FancyFogDistance,
    OcclusionQuery,
    Mipmaps,
    AnisotropicFiltering,
    MultisampleAntialiasing
};


enum class RenderMatrixMode
{
    ModelView,
    Projection,
    Texture
};

constexpr int renderPrimitiveValue(RenderPrimitive primitive) { return static_cast<int>(primitive); }

struct RenderInterleavedMesh
{
    const void* data = nullptr;
    int stride = 0;
    int first = 0;
    int count = 0;
    RenderPrimitive primitive = RenderPrimitive::Triangles;
    bool positionShort = false;

    bool hasTexture = false;
    int texCoordOffset = 0;

    bool hasColor = false;
    int colorOffset = 0;

    bool hasNormals = false;
    int normalOffset = 0;

    bool hasBrightness = false;
    int brightnessOffset = 0;

};

struct RenderCapturedMesh
{
    std::vector<std::int32_t> raw;
    int vertexCount = 0;
    int stride = 0;
    RenderPrimitive primitive = RenderPrimitive::Triangles;
    bool positionShort = false;
    bool hasTexture = false;
    int texCoordOffset = 0;
    bool hasColor = false;
    int colorOffset = 0;
    bool hasNormals = false;
    int normalOffset = 0;
    bool hasBrightness = false;
    int brightnessOffset = 0;

    // DSi-only (RenderAPI_DSI.cpp's dsiRepackCapturedMeshFast()): true once
    // this mesh's position field has been converted from float3 to the DS
    // GPU's native v16 fixed-point format in place. Every other backend
    // leaves this at its default (false) and ignores it; harmless dead
    // weight there (one bool per captured mesh), same as positionShort
    // already is for backends that never set it.
    bool positionIsV16 = false;

    // DSi-only, same precedent as positionIsV16 immediately above: true once
    // this mesh's texcoord field has been pre-converted from normalized
    // float2 to the DS GPU's native t16 texel-space fixed-point format
    // (glTexCoord2t16()), skipping the per-frame float multiply-by-texture-
    // size glTexCoord2f() does internally on every vertex, every frame a
    // section replays. Only ever set for a mesh dsiRepackCapturedMeshFast()
    // confirmed is always drawn against a specific, already-resident texture
    // (main terrain sections against terrain.png) -- never safe to bake in
    // generally, since a captured mesh drawn against different textures on
    // different frames needs the actual bound texture's size at draw time,
    // which is what the float path still correctly provides. Ignored on
    // every other backend, same as positionIsV16.
    bool texCoordIsT16 = false;

    // DSi-only (RenderAPI_DSI.cpp's dsiCompileCapturedMeshCommands()): a
    // pre-packed GX FIFO command stream -- built once, same "pay the cost
    // at build time, not on every one of the hundreds of frames this mesh
    // replays before its next rebuild" precedent as positionIsV16/
    // texCoordIsT16 above, just one step further (skips the per-vertex
    // libnds wrapper-function calls entirely, not just the float math
    // inside them: see that function's own comment, and
    // drawCapturedMeshFast()'s, for the real-hardware reference this is
    // modelled on). Empty means "not compiled" -- raw is always kept as the
    // fallback, so an empty list here just means drawCapturedMeshFast()
    // uses its ordinary per-vertex path, the same as if this field did not
    // exist. Ignored on every other backend, same as positionIsV16/
    // texCoordIsT16.
    std::vector<std::uint32_t> compiledCommands;

    bool empty() const { return vertexCount <= 0 || raw.empty(); }
    std::size_t byteSize() const { return raw.size() * sizeof(std::int32_t); }
    void clear()
    {
        raw.clear();
        vertexCount = 0;
        stride = 0;
        primitive = RenderPrimitive::Triangles;
        positionShort = false;
        hasTexture = false;
        texCoordOffset = 0;
        hasColor = false;
        colorOffset = 0;
        hasNormals = false;
        normalOffset = 0;
        hasBrightness = false;
        brightnessOffset = 0;
        positionIsV16 = false;
        // FIXED: texCoordIsT16 used to be left unreset here (positionIsV16
        // right above it was correctly reset, this one was just missed) --
        // a RenderCapturedMesh cleared and refilled in place (not every
        // caller replaces the object outright) would carry a stale "true"
        // into fresh raw data that is not actually t16-converted yet,
        // which drawCapturedMeshFast() would then misread as pre-converted
        // fixed-point ints instead of the floats actually there. Fixed
        // alongside adding compiledCommands below, since a stale compiled
        // command list surviving a clear() would be the same class of bug
        // one step further along -- replaying commands built from a
        // previous, unrelated fill of this same mesh object.
        texCoordIsT16 = false;
        compiledCommands.clear();
    }
};

struct RenderStaticMesh
{
    int persistentHandle = 0;
    bool persistentReady = false;
    RenderCapturedMesh captured;
};

void renderStaticMeshCreate(RenderStaticMesh& mesh);
void renderStaticMeshDestroy(RenderStaticMesh& mesh);
bool renderStaticMeshCompile(RenderStaticMesh& mesh, const RenderInterleavedMesh& source);
bool renderStaticMeshDraw(const RenderStaticMesh& mesh);

// Per-face-direction split of an opaque section's quads, in the order a backend
// face sort emits them. A backend that can replay parts of a section separately
// uses it to skip the directions facing away from the eye; one that cannot
// ignores it and records the section whole.
struct RenderTerrainFaceGroups
{
    static const int kGroupCount = 7;
    int quadCount[kGroupCount] = {};
    float planeMin[kGroupCount] = {};
    float planeMax[kGroupCount] = {};
};

struct RenderTerrainCompileInfo
{
    float translateX = 0.0f;
    float translateY = 0.0f;
    float translateZ = 0.0f;
    float sectionSize = 16.0f;

    // Optional; null records the section as one undivided mesh. The plane
    // extents in it are section-local, so worldOrigin below is what a backend
    // needs to put the eye in the same space -- translateX/Y/Z cannot serve,
    // having been through WorldRenderer::setPosition()'s 1024-block wrap.
    const RenderTerrainFaceGroups* faceGroups = nullptr;
    float worldOriginX = 0.0f;
    float worldOriginY = 0.0f;
    float worldOriginZ = 0.0f;
};

// Submit one interleaved mesh using the fixed Minecraft vertex semantics:
// position=float3, texcoord=float2, color=RGBA8, normal=signed byte3.
// Returns false only when the backend cannot represent the supplied layout.
bool renderDrawInterleaved(const RenderInterleavedMesh& mesh);

// Capture/replay geometry without exposing a backend-specific mesh type to Minecraft.
// append=true is intended for bounded chunk-build steps that concatenate batches.
bool renderCaptureInterleaved(const RenderInterleavedMesh& mesh, RenderCapturedMesh& out, bool append);
bool renderDrawCaptured(const RenderCapturedMesh& mesh);

#if PLATFORM_PERSISTENT_RENDER_MESH || PLATFORM_MODEL_PERSISTENT_MESH
// Backend-owned immutable geometry. Wii maps this to native GX display lists;
// PS2 maps it to captured RAM meshes held in Ps2ModelGeometryCache. Model
// geometry is persistent on more backends than terrain is, which is why the
// model flag opens these declarations on its own.
int renderCreatePersistentMesh();
void renderDestroyPersistentMesh(int handle);
bool renderCompilePersistentMesh(int handle, const RenderInterleavedMesh& mesh);
bool renderDrawPersistentMesh(int handle);
#endif

#if PLATFORM_NATIVE_TERRAIN_PIPELINE
bool renderCompileTerrainMesh(int handle, const RenderInterleavedMesh& mesh,
                              const RenderTerrainCompileInfo& info);
#endif

namespace RenderClearMask { constexpr unsigned int Depth = 0x00000100u; constexpr unsigned int Color = 0x00004000u; }

void renderEnable(RenderCapability capability);
void renderDisable(RenderCapability capability);
void renderBlendFunc(RenderBlendFactor source, RenderBlendFactor destination);
void renderDepthMask(bool enabled);
void renderDepthFunc(RenderCompare function);
void renderAlphaFunc(RenderCompare function, float reference);
void renderCullFace(RenderFace face);
void renderColorMask(bool red, bool green, bool blue, bool alpha);
void renderBindTexture(int texture);
void renderSetActiveTextureUnit(int textureUnit);
void renderSetClientActiveTextureUnit(int textureUnit);
void renderSetMultiTextureCoord(int textureUnit, float u, float v);
void renderSetLightmapColors(const std::uint32_t* colors, int count);
void renderColor4f(float r, float g, float b, float a);
void renderColor3f(float r, float g, float b);
void renderNormal3f(float x, float y, float z);
void renderGenerateTextures(int count, int *textures);
void renderDeleteTextures(int count, const int *textures);
void renderTextureSubImageRgba(int level, int x, int y, int width, int height, const void *pixels);
void renderTextureImageRgba(int level, int width, int height, const void *pixels);
void renderTextureParameters(bool blur, bool mipmaps, bool clamp);
#if PLATFORM_TEXTURE_QUALITY_CONTROLS
void renderApplyTextureQuality(bool blur, int mipmapLevel, bool mipmapLinear, int anisotropy);
#endif
int renderGetMaxAnisotropy();
int renderGetMaxSamples();
bool renderTextureBeginUpload(int texture, int width, int height, int maxLevel,
                              bool blur, bool clamp, bool tileAtlas = false, bool highPrecision = false);
bool renderTextureIsValid(int texture);
void renderResetResources();
void renderFogf(RenderFogParameter parameter, float value);
void renderFogi(RenderFogParameter parameter, RenderFogMode value);
void renderFogColor(const float* values);
void renderLightfv(int lightIndex, RenderLightParameter parameter, const float* values);
void renderLightModelAmbient(const float* values);
void renderColorMaterial(RenderFace face, RenderColorMaterialMode mode);
void renderShadeModel(RenderShadeModel model);
void renderClear(unsigned int mask);
void renderFinishGpu();

// Announce that nothing else will be drawn into this frame, without waiting for
// the GPU. Backends that present asynchronously use it to start the work a
// buffer swap would otherwise begin, so it overlaps whatever the game does
// between the last draw and the next swap -- on Wii, the EFB->XFB copy.
//
// Optional: a backend whose swap is already the only synchronisation point
// implements it as a no-op, and callers must still swap normally afterwards.
void renderSubmitFrame();
void renderClearColor(float r, float g, float b, float a);
void renderClearDepth(double depth);
void renderPolygonOffset(float factor, float units);
void renderLineWidth(float width);
void renderViewport(int x, int y, int width, int height);
void renderGetViewport(int* values);
void renderGetMatrix(RenderMatrixQuery query, float* values);
const unsigned char* renderGetString(RenderStringQuery query);
bool renderSupportsFeature(RenderFeature feature);
unsigned int renderGetError();
void renderFogHint(RenderHintMode mode);
void renderMatrixMode(RenderMatrixMode mode);
void renderLoadIdentity();
void renderPushMatrix();
void renderPopMatrix();
void renderTranslate(float x, float y, float z);
void renderRotate(float angle, float x, float y, float z);
void renderScale(float x, float y, float z);
void renderScaleDouble(double x, double y, double z);
void renderFrustum(double left, double right, double bottom, double top, double nearValue, double farValue);
void renderOrtho(double left, double right, double bottom, double top, double nearValue, double farValue);

#if PLATFORM_PC
// Desktop-only retained-mode compatibility used by the original 1.2.5 GL renderer.
int renderGenerateDisplayLists(int count);
void renderDeleteDisplayLists(int first, int count);
void renderBeginDisplayList(int list);
void renderEndDisplayList();
void renderCallDisplayList(int list);
void renderCallDisplayLists(int count, const int* lists);

// Desktop Advanced OpenGL occlusion queries. Native console terrain never uses them.
void renderGenerateOcclusionQueries(int count, int* queries);
void renderBeginOcclusionQuery(int query);
void renderEndOcclusionQuery();
bool renderOcclusionQueryResultAvailable(int query);
unsigned int renderOcclusionQueryResult(int query);
#endif

#if PLATFORM_FRAMEBUFFER_READBACK
// Framebuffer readback used by screenshot backends.
bool renderReadPixelsRgb(int x, int y, int width, int height, void* pixels);
#endif
bool renderCopyFramebufferToBoundTexture(int x, int y, int width, int height);
// Requests the cheapest platform-native Legacy Look presentation gamma for the current frame.
// Desktop ignores this because it runs the real framebuffer shader; Wii maps it to GX copy gamma;
// PS2 keeps the existing lightmap/fog/sky LUT fallback.
void renderSetLegacyPresentationGamma(bool enabled);
