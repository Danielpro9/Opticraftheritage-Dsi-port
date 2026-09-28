#include "LegacyPanorama.h"

#include "net/minecraft/src/Minecraft.h"
#include "net/minecraft/src/RenderEngine.h"
#include "net/minecraft/src/Tessellator.h"
#include "platform/RenderAPI.h"

namespace
{
void drawPanoramaQuad(int_t screenWidth, int_t screenHeight, float_t zLevel,
    const LegacyPanoramaUv &uv, float_t uOffset, float_t vOffset, float_t alpha)
{
    Tessellator *tess = &Tessellator::instance;
    tess->startDrawingQuads();
    tess->setColorRGBA_F(1.0f, 1.0f, 1.0f, alpha);
    tess->addVertexWithUV(0.0, screenHeight, zLevel, uv.u0 + uOffset, uv.v1 + vOffset);
    tess->addVertexWithUV(screenWidth, screenHeight, zLevel, uv.u1 + uOffset, uv.v1 + vOffset);
    tess->addVertexWithUV(screenWidth, 0.0, zLevel, uv.u1 + uOffset, uv.v0 + vOffset);
    tess->addVertexWithUV(0.0, 0.0, zLevel, uv.u0 + uOffset, uv.v0 + vOffset);
    tess->draw();
}
}

bool legacyDrawPanorama(Minecraft *mc, int_t screenWidth, int_t screenHeight,
    int_t panoramaTimer, float_t partialTick, float_t zLevel)
{
    if (mc == nullptr || mc->renderEngine == nullptr || screenWidth <= 0 || screenHeight <= 0)
        return false;

    // Cached rather than looked up fresh every frame -- legacyPanoramaResourcePath()
    // always returns the same constant string, and this is the actual DSi
    // panorama path (GuiMainMenu, LoadingScreenRenderer, LegacyPlayGameScreen,
    // LegacyCreateWorldScreen, LegacyOptionsScreen each call this once per
    // frame while their panorama background is shown).
    // CachedTextureId re-resolves itself if this id was ever invalidated by a
    // releaseTexture() call ("/legacy/panorama.png" is released by name in
    // ClientPlatformPolicy_DSI.cpp on world exit) -- see its own comment in
    // RenderEngine.h.
    static CachedTextureId cachedPanoramaTextureId;
    const int_t texture = cachedPanoramaTextureId.get(mc->renderEngine, legacyPanoramaResourcePath());
    int_t textureWidth = 0;
    int_t textureHeight = 0;
    if (!mc->renderEngine->getTextureDimensions(texture, &textureWidth, &textureHeight) ||
        textureWidth <= 0 || textureHeight <= 0)
        return false;

    const float_t ticks = static_cast<float_t>(panoramaTimer) + partialTick;
    const LegacyPanoramaUv uv = legacyPanoramaUv(screenWidth, screenHeight,
        textureWidth, textureHeight, legacyPanoramaLoopOffset(ticks));

    renderBindTexture(texture);
    renderTextureParameters(true, false, true);
    renderEnable(RenderCapability::Blend);
    renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);
    renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, 0.0f, 0.0f, 1.0f);

#ifndef DSI_PLATFORM
    // Requested directly after a real-hardware report of severe main-menu
    // lag on DSi (many D-pad/A presses needed before anything registered).
    // This is a real, named "blur": 4 extra full-screen alpha-blended
    // draws every frame (9 on PS2, see the diagonal block below), purely
    // to soften the background image. Translucent-polygon rendering is one
    // of the more expensive, easier-to-misconfigure paths on the DS's
    // fixed-function GPU (see RenderAPI_DSI.cpp's POLY_ALPHA/blending
    // notes), so skip it here entirely rather than tune the sample count --
    // DSi gets just the single opaque base draw above.
    const float_t texelU = 1.0f / static_cast<float_t>(textureWidth);
    const float_t texelV = 1.0f / static_cast<float_t>(textureHeight);
    const float_t blurAlpha = 0.18f;
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, -texelU, 0.0f, blurAlpha);
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, texelU, 0.0f, blurAlpha);
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, 0.0f, -texelV, blurAlpha);
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, 0.0f, texelV, blurAlpha);
#endif
#ifdef PS2_PLATFORM
    // The start menu can afford four extra taps, and the diagonal samples make
    // the bilinear result read as a blur instead of a one-axis softening.
    const float_t blurDiagonalAlpha = 0.10f;
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, -texelU, -texelV, blurDiagonalAlpha);
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, texelU, -texelV, blurDiagonalAlpha);
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, -texelU, texelV, blurDiagonalAlpha);
    drawPanoramaQuad(screenWidth, screenHeight, zLevel, uv, texelU, texelV, blurDiagonalAlpha);
#endif
    return true;
}
