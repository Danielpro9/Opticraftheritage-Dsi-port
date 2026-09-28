#include "GuiSkinSelector.h"
#include "SkinManager.h"
#include "net/minecraft/src/GuiButton.h"
#include "net/minecraft/src/Minecraft.h"
#include "net/minecraft/src/FontRenderer.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/StringTranslate.h"
#include "net/minecraft/src/EntityPlayerSP.h"
#include "net/minecraft/src/RenderEngine.h"
#include "net/minecraft/src/SoundManager.h"
#include "net/minecraft/src/Tessellator.h"
#include "net/minecraft/src/legacy/LegacyMenuHints.h"
#include "platform/Input.h"
#include "platform/RenderAPI.h"

#include "pc/lwjgl/Keyboard.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
constexpr int_t BUTTON_ID_TAB_DEFAULT = 1;
constexpr int_t BUTTON_ID_TAB_CUSTOM = 2;

std::string toUpperString(const std::string &str)
{
    std::string result = str;
    for (char &c : result)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}
}

GuiSkinSelector::GuiSkinSelector(GuiScreen *parent)
    : parentScreen(parent)
    , initializedSelection(false)
    , currentPackIndex(0)
    , currentSkinIndex(0)
    , scrollOffset(0.0f)
    , dialogLeft(0)
    , dialogTop(0)
    , dialogWidth(0)
    , dialogHeight(0)
    , leftPanelWidth(0)
    , rightPanelX(0)
    , rightPanelWidth(0)
    , carouselCenterX(0)
    , carouselGroundY(0)
    , nameplateY(0)
    , nameplateHeight(0)
    , buttonTabDefault(nullptr)
    , buttonTabCustom(nullptr)
{
    SkinManager::init();
    currentPackIndex = SkinManager::getSelectedPackIndex();
    currentSkinIndex = SkinManager::getSelectedIndex();
}

void GuiSkinSelector::initGui()
{
    controlList.clear();

    StringTranslate *tr = StringTranslate::getInstance();
    const bool isEs = (tr != nullptr && tr->getCurrentLanguage().rfind("es_", 0) == 0);

    if (!initializedSelection)
    {
        if (mc != nullptr && mc->gameSettings != nullptr && !mc->gameSettings->selectedSkin.empty())
        {
            SkinManager::setSelectedSkinId(mc->gameSettings->selectedSkin);
            currentPackIndex = SkinManager::getSelectedPackIndex();
            currentSkinIndex = SkinManager::getSelectedIndex();
        }
        initializedSelection = true;
    }

    if (currentPackIndex >= SkinManager::getPackCount())
        currentPackIndex = 0;

    const int totalInPack = SkinManager::getSkinCountForPack(currentPackIndex);
    if (totalInPack > 0)
    {
        if (currentSkinIndex < 0 || currentSkinIndex >= totalInPack)
            currentSkinIndex = 0;
    }

    // Dialog layout sizing. No title banner reserved above this (unlike the
    // scene-anchored screens, e.g. LegacyCreateWorldScreen) -- this is a
    // floating dialog over the darkened background, so its own dialogTop/
    // dialogHeight budget is the only thing that has to clear
    // legacyHintRowY() at the bottom.
    dialogWidth = std::min<int_t>(width - 16, 384);
    dialogHeight = std::min<int_t>(height - 48, 172);
    dialogLeft = (width - dialogWidth) / 2;
    dialogTop = 12;

    leftPanelWidth = dialogWidth * 30 / 100;
    rightPanelX = dialogLeft + leftPanelWidth + 4;
    rightPanelWidth = dialogWidth - leftPanelWidth - 4;

    carouselCenterX = rightPanelX + rightPanelWidth / 2;
    nameplateHeight = 20;
    nameplateY = dialogTop + dialogHeight - nameplateHeight - 8;
    carouselGroundY = nameplateY - 6;

    // Tabs in left panel
    const int_t packBtnX = dialogLeft + 6;
    const int_t packBtnW = leftPanelWidth - 12;
    const int_t tabY = dialogTop + 78;

    std::string defBase = isEs ? "Skins Originales" : "Default Skins";
    std::string defLabel = (currentPackIndex == 0) ? ("> " + defBase + " <") : defBase;
    buttonTabDefault = new GuiButton(BUTTON_ID_TAB_DEFAULT, packBtnX, tabY, packBtnW, 18, defLabel);
    controlList.push_back(buttonTabDefault);

    if (SkinManager::getPackCount() > 1)
    {
        std::string customBase = isEs ? "Personalizado" : "Custom";
        std::string customLabel = customBase + " (" + std::to_string(SkinManager::getSkinCountForPack(1)) + ")";
        if (currentPackIndex == 1)
            customLabel = "> " + customLabel + " <";
        buttonTabCustom = new GuiButton(BUTTON_ID_TAB_CUSTOM, packBtnX, tabY + 22, packBtnW, 18, customLabel);
        controlList.push_back(buttonTabCustom);
    }
    else
    {
        buttonTabCustom = nullptr;
    }
}

void GuiSkinSelector::switchPack(int newPackIndex)
{
    if (newPackIndex == currentPackIndex)
        return;

    if (newPackIndex == 1 && SkinManager::getCustomSkins().empty())
        return;

    currentPackIndex = newPackIndex;
    SkinManager::setSelectedPackIndex(currentPackIndex);
    currentSkinIndex = 0;
    scrollOffset = 0.0f;

    if (mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.click", 1.0f, 1.1f);

    initGui();
}

void GuiSkinSelector::handleSpecializedMenuInput()
{
    const PlatformTextInputSnapshot pad = platformTextInputSnapshot(platformMenuPad());
    if (!pad.connected)
        return;

    if ((pad.pressed & PLATFORM_TEXT_BACK) != 0)
    {
        cancelAndReturn();
        return;
    }

    if ((pad.pressed & PLATFORM_TEXT_LEFT) != 0)
        prevSkin();
    else if ((pad.pressed & PLATFORM_TEXT_RIGHT) != 0)
        nextSkin();

    if ((pad.pressed & (PLATFORM_TEXT_UP | PLATFORM_TEXT_DOWN)) != 0 && SkinManager::getPackCount() > 1)
    {
        switchPack(1 - currentPackIndex);
        return;
    }

    // Touch pointer plays the same role as Wii's IR cursor here -- do not let
    // a tap that is really aiming the pointer also confirm a selection. See
    // LegacyCreateWorldScreen.cpp's updateScreen() for the same guard.
    if (!platformMenuPointerActive() && (pad.pressed & PLATFORM_TEXT_TYPE) != 0)
    {
        selectAndConfirm();
        return;
    }
}

void GuiSkinSelector::updateScreen()
{
    GuiScreen::updateScreen();

    // Smoothly dampen carousel scrolling transition
    if (std::fabs(scrollOffset) > 0.001f)
    {
        scrollOffset *= 0.65f;
        if (std::fabs(scrollOffset) < 0.002f)
            scrollOffset = 0.0f;
    }
}

void GuiSkinSelector::keyTyped(char_t c, int_t key)
{
    if (key == 1 || key == lwjgl::Keyboard::KEY_ESCAPE)
    {
        cancelAndReturn();
        return;
    }
    if (key == lwjgl::Keyboard::KEY_RETURN || key == lwjgl::Keyboard::KEY_NUMPADENTER)
    {
        selectAndConfirm();
        return;
    }
    if (key == lwjgl::Keyboard::KEY_LEFT || key == lwjgl::Keyboard::KEY_A)
    {
        prevSkin();
        return;
    }
    if (key == lwjgl::Keyboard::KEY_RIGHT || key == lwjgl::Keyboard::KEY_D)
    {
        nextSkin();
        return;
    }
    if (key == lwjgl::Keyboard::KEY_TAB)
    {
        if (SkinManager::getPackCount() > 1)
            switchPack(1 - currentPackIndex);
        return;
    }

    GuiScreen::keyTyped(c, key);
}

void GuiSkinSelector::nextSkin()
{
    const int total = SkinManager::getSkinCountForPack(currentPackIndex);
    if (total <= 0)
        return;

    currentSkinIndex = (currentSkinIndex + 1) % total;
    scrollOffset -= 1.0f;
    if (scrollOffset < -1.5f)
        scrollOffset = -1.5f;

    if (mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.click", 1.0f, 1.2f);
}

void GuiSkinSelector::prevSkin()
{
    const int total = SkinManager::getSkinCountForPack(currentPackIndex);
    if (total <= 0)
        return;

    currentSkinIndex = ((currentSkinIndex - 1) % total + total) % total;
    scrollOffset += 1.0f;
    if (scrollOffset > 1.5f)
        scrollOffset = 1.5f;

    if (mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.click", 1.0f, 1.2f);
}

void GuiSkinSelector::selectAndConfirm()
{
    const SkinEntry *skin = SkinManager::getSkin(currentPackIndex, currentSkinIndex);
    if (skin != nullptr)
    {
        SkinManager::setSelectedPackIndex(currentPackIndex);
        SkinManager::setSelectedSkinId(skin->id);
        if (mc != nullptr && mc->gameSettings != nullptr)
        {
            mc->gameSettings->selectedSkin = skin->id;
            mc->gameSettings->saveOptions();
        }
        if (mc != nullptr && mc->thePlayer != nullptr)
        {
            mc->thePlayer->setEntityTexture(skin->modelPath);
        }
    }

    if (mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.action", 1.0f, 1.0f);

    mc->displayGuiScreen(parentScreen);
}

void GuiSkinSelector::cancelAndReturn()
{
    if (mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.back", 1.0f, 1.0f);

    mc->displayGuiScreen(parentScreen);
}

void GuiSkinSelector::mouseClicked(int_t mouseX, int_t mouseY, int_t button)
{
    GuiScreen::mouseClicked(mouseX, mouseY, button);

    if (button != 0)
        return;

    for (GuiButton *btn : controlList)
    {
        if (btn != nullptr && btn->enabled && btn->mousePressed(mc, mouseX, mouseY))
            return;
    }

    if (mouseY >= nameplateY && mouseY <= nameplateY + nameplateHeight &&
        mouseX >= rightPanelX && mouseX <= rightPanelX + rightPanelWidth)
    {
        selectAndConfirm();
        return;
    }

    if (mouseX >= carouselCenterX - 25 && mouseX <= carouselCenterX + 25 &&
        mouseY >= carouselGroundY - 75 && mouseY <= carouselGroundY)
    {
        selectAndConfirm();
        return;
    }

    if (mouseX >= carouselCenterX - 85 && mouseX < carouselCenterX - 25 &&
        mouseY >= carouselGroundY - 65 && mouseY <= carouselGroundY)
    {
        prevSkin();
        return;
    }

    if (mouseX > carouselCenterX + 25 && mouseX <= carouselCenterX + 85 &&
        mouseY >= carouselGroundY - 65 && mouseY <= carouselGroundY)
    {
        nextSkin();
        return;
    }
}

void GuiSkinSelector::actionPerformed(GuiButton *button)
{
    if (!button->enabled)
        return;

    if (button->id == BUTTON_ID_TAB_DEFAULT)
        switchPack(0);
    else if (button->id == BUTTON_ID_TAB_CUSTOM)
        switchPack(1);
}

bool GuiSkinSelector::doesGuiPauseGame()
{
    return true;
}

void GuiSkinSelector::drawInsetPanel(int_t left, int_t top, int_t right, int_t bottom, int_t fillColor)
{
    drawRect(left, top, right, bottom, fillColor);
    drawRect(left, top, right, top + 1, 0xFF000000);
    drawRect(left, top, left + 1, bottom, 0xFF000000);
    drawRect(left, bottom - 1, right, bottom, 0x30FFFFFF);
    drawRect(right - 1, top, right, bottom, 0x30FFFFFF);
}

void GuiSkinSelector::drawFeetShadow(float centerX, float groundY, float radiusX, float radiusY, float alpha)
{
    renderEnable(RenderCapability::Blend);
    renderDisable(RenderCapability::Texture2D);
    renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);

    int a = static_cast<int>(alpha * 90.0f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    int color = (a << 24) | 0x000000;

    int_t x1 = static_cast<int_t>(centerX - radiusX);
    int_t x2 = static_cast<int_t>(centerX + radiusX);
    int_t y1 = static_cast<int_t>(groundY - radiusY * 0.5f);
    int_t y2 = static_cast<int_t>(groundY + radiusY * 0.5f);

    drawRect(x1 + 2, y1, x2 - 2, y2, color);
    renderDisable(RenderCapability::Blend);
}

void GuiSkinSelector::drawFrontPreview(const SkinEntry *skin, float x, float y, float w, float h, float alpha)
{
    if (skin == nullptr || mc == nullptr || mc->renderEngine == nullptr)
        return;

    if (!skin->isCustom && !skin->frontPath.empty())
    {
        int texId = mc->renderEngine->getTexture(skin->frontPath);
        if (texId >= 0)
        {
            mc->renderEngine->bindTexture(texId);
            renderEnable(RenderCapability::Texture2D);
            renderEnable(RenderCapability::Blend);
            renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);
            renderColor4f(alpha, alpha, alpha, alpha);

            Tessellator &tess = Tessellator::instance;
            tess.startDrawingQuads();
            tess.addVertexWithUV(x,     y + h, zLevel, 0.0f, 1.0f);
            tess.addVertexWithUV(x + w, y + h, zLevel, 1.0f, 1.0f);
            tess.addVertexWithUV(x + w, y,     zLevel, 1.0f, 0.0f);
            tess.addVertexWithUV(x,     y,     zLevel, 0.0f, 0.0f);
            tess.draw();
            renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            return;
        }
    }

    int texId = -1;
    if (!skin->frontPath.empty())
        texId = mc->renderEngine->getTexture(skin->frontPath);

    if (texId < 0 && !skin->skinPath.empty())
        texId = mc->renderEngine->getTexture(skin->skinPath);

    if (texId < 0 && !skin->modelPath.empty())
        texId = mc->renderEngine->getTexture(skin->modelPath);

    if (texId < 0)
        return;

    mc->renderEngine->bindTexture(texId);
    renderEnable(RenderCapability::Texture2D);
    renderEnable(RenderCapability::Blend);
    renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);
    renderColor4f(alpha, alpha, alpha, alpha);

    int_t tw = 64, th = 32;
    mc->renderEngine->getTextureDimensions(texId, &tw, &th);
    if (th <= 0) th = 32;

    if (tw == 16 && th == 32)
    {
        Tessellator &tess = Tessellator::instance;
        tess.startDrawingQuads();
        tess.addVertexWithUV(x,     y + h, zLevel, 0.0f, 1.0f);
        tess.addVertexWithUV(x + w, y + h, zLevel, 1.0f, 1.0f);
        tess.addVertexWithUV(x + w, y,     zLevel, 1.0f, 0.0f);
        tess.addVertexWithUV(x,     y,     zLevel, 0.0f, 0.0f);
        tess.draw();
        renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    const float uScale = 1.0f / 64.0f;
    const float vScale = 1.0f / static_cast<float>(th);
    const float unitW = w / 16.0f;
    const float unitH = h / 32.0f;

    Tessellator &tess = Tessellator::instance;
    tess.startDrawingQuads();

    auto drawQuad = [&](float qx, float qy, float qw, float qh, float su0, float sv0, float su1, float sv1) {
        tess.addVertexWithUV(qx,      qy + qh, zLevel, su0, sv1);
        tess.addVertexWithUV(qx + qw, qy + qh, zLevel, su1, sv1);
        tess.addVertexWithUV(qx + qw, qy,      zLevel, su1, sv0);
        tess.addVertexWithUV(qx,      qy,      zLevel, su0, sv0);
    };

    drawQuad(x + 4.0f * unitW, y, 8.0f * unitW, 8.0f * unitH, 8.0f * uScale, 8.0f * vScale, 16.0f * uScale, 16.0f * vScale);
    drawQuad(x + 3.5f * unitW, y - 0.5f * unitH, 9.0f * unitW, 9.0f * unitH, 40.0f * uScale, 8.0f * vScale, 48.0f * uScale, 16.0f * vScale);
    drawQuad(x + 4.0f * unitW, y + 8.0f * unitH, 8.0f * unitW, 12.0f * unitH, 20.0f * uScale, 20.0f * vScale, 28.0f * uScale, 32.0f * vScale);
    drawQuad(x, y + 8.0f * unitH, 4.0f * unitW, 12.0f * unitH, 44.0f * uScale, 20.0f * vScale, 48.0f * uScale, 32.0f * vScale);

    if (th == 64)
        drawQuad(x + 12.0f * unitW, y + 8.0f * unitH, 4.0f * unitW, 12.0f * unitH, 36.0f * uScale, 52.0f * vScale, 40.0f * uScale, 64.0f * vScale);
    else
        drawQuad(x + 12.0f * unitW, y + 8.0f * unitH, 4.0f * unitW, 12.0f * unitH, 48.0f * uScale, 20.0f * vScale, 44.0f * uScale, 32.0f * vScale);

    drawQuad(x + 4.0f * unitW, y + 20.0f * unitH, 4.0f * unitW, 12.0f * unitH, 4.0f * uScale, 20.0f * vScale, 8.0f * uScale, 32.0f * vScale);

    if (th == 64)
        drawQuad(x + 8.0f * unitW, y + 20.0f * unitH, 4.0f * unitW, 12.0f * unitH, 20.0f * uScale, 52.0f * vScale, 24.0f * uScale, 64.0f * vScale);
    else
        drawQuad(x + 8.0f * unitW, y + 20.0f * unitH, 4.0f * unitW, 12.0f * unitH, 8.0f * uScale, 20.0f * vScale, 4.0f * uScale, 32.0f * vScale);

    tess.draw();
    renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void GuiSkinSelector::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    (void)partialTick;

    drawDefaultBackground();

    const int_t leftX1 = dialogLeft;
    const int_t leftY1 = dialogTop;
    const int_t leftX2 = dialogLeft + leftPanelWidth;
    const int_t leftY2 = dialogTop + dialogHeight;
    drawInsetPanel(leftX1, leftY1, leftX2, leftY2, 0xD0101010);

    const int_t rightX1 = rightPanelX;
    const int_t rightY1 = dialogTop;
    const int_t rightX2 = rightPanelX + rightPanelWidth;
    const int_t rightY2 = dialogTop + dialogHeight;
    drawInsetPanel(rightX1, rightY1, rightX2, rightY2, 0xD0101010);

    drawRect(rightX1 + 1, rightY1 + 1, rightX2 - 1, rightY1 + 18, 0x80282828);

    std::string headerName = SkinManager::getPackName(currentPackIndex);
    fontRenderer->drawStringWithShadow(headerName, rightX1 + 10, rightY1 + 5, 0xFFFFFF);

    const int totalSkins = SkinManager::getSkinCountForPack(currentPackIndex);
    if (totalSkins > 0)
    {
        std::string countStr = std::to_string(currentSkinIndex + 1) + " / " + std::to_string(totalSkins);
        int_t countW = fontRenderer->getStringWidth(countStr);
        fontRenderer->drawStringWithShadow(countStr, rightX2 - countW - 10, rightY1 + 5, 0xAAAAAA);
    }

    if (totalSkins > 0)
    {
        const float spacing = static_cast<float>(rightPanelWidth) * 0.22f;

        const int slotOrder[] = { -3, 3, -2, 2, -1, 1, 0 };
        for (int k : slotOrder)
        {
            float t = static_cast<float>(k) - scrollOffset;
            float dist = std::fabs(t);
            if (dist > 2.6f)
                continue;

            int skinIndex = ((currentSkinIndex + k) % totalSkins + totalSkins) % totalSkins;
            const SkinEntry *skin = SkinManager::getSkin(currentPackIndex, skinIndex);
            if (skin == nullptr)
                continue;

            float scale = 3.4f - 1.05f * std::min(dist, 2.0f);
            if (scale < 1.0f) scale = 1.0f;

            float alpha = 1.0f - 0.20f * std::min(dist, 2.0f);
            if (alpha < 0.35f) alpha = 0.35f;

            float skinW = 16.0f * scale;
            float skinH = 32.0f * scale;
            float skinX = static_cast<float>(carouselCenterX) + t * spacing - skinW * 0.5f;
            float skinY = static_cast<float>(carouselGroundY) - skinH;

            drawFeetShadow(static_cast<float>(carouselCenterX) + t * spacing, static_cast<float>(carouselGroundY),
                           skinW * 0.45f, 4.0f, alpha);

            drawFrontPreview(skin, skinX, skinY, skinW, skinH, alpha);
        }
    }

    const int_t npPadding = 8;
    const int_t npW = rightPanelWidth - npPadding * 2;
    const int_t npX = rightX1 + npPadding;
    const int_t npY = nameplateY;
    drawInsetPanel(npX, npY, npX + npW, npY + nameplateHeight, 0xFF181818);

    const SkinEntry *selectedSkin = SkinManager::getSkin(currentPackIndex, currentSkinIndex);
    if (selectedSkin != nullptr)
    {
        std::string displayName = toUpperString(selectedSkin->name);
        drawCenteredString(fontRenderer, displayName, npX + npW / 2, npY + 6, 0xFFFFAA);
    }

    drawLegacyMenuHints(fontRenderer, width, height, true);

    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
