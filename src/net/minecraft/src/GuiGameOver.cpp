#include "GuiGameOver.h"
#include "GuiButton.h"
#include "GuiMainMenu.h"
#include "FontRenderer.h"
#include "Minecraft.h"
#include "EntityPlayerSP.h"
#include "World.h"
#include "WorldInfo.h"
#include "ISaveHandler.h"
#include "ISaveFormat.h"
#include "StatCollector.h"
#include "platform/RenderAPI.h"
#include <string>

GuiGameOver::GuiGameOver() :
    cooldownTimer(0)
{
}

void GuiGameOver::initGui()
{
    controlList.clear();
    cooldownTimer = 0;

    const bool hardcore = mc->theWorld != nullptr && mc->theWorld->getWorldInfo() != nullptr &&
                          mc->theWorld->getWorldInfo()->isHardcoreModeEnabled();
    if (hardcore)
    {
        controlList.push_back(new GuiButton(1, width / 2 - 100, height / 4 + 96,
                                            StatCollector::translateToLocal("deathScreen.deleteWorld")));
    }
    else
    {
        controlList.push_back(new GuiButton(1, width / 2 - 100, height / 4 + 72,
                                            StatCollector::translateToLocal("deathScreen.respawn")));
        controlList.push_back(new GuiButton(2, width / 2 - 100, height / 4 + 96,
                                            StatCollector::translateToLocal("deathScreen.titleScreen")));
        if (mc->session == nullptr && controlList.size() > 1)
            controlList[1]->enabled = false;
    }

    for (GuiButton *button : controlList)
        button->enabled = false;
}

void GuiGameOver::keyTyped(char_t c, int_t i)
{
}

void GuiGameOver::actionPerformed(GuiButton *guibutton)
{
    if (guibutton == nullptr || !guibutton->enabled)
        return;

    switch (guibutton->id)
    {
        case 1:
        {
            const bool hardcore = mc->theWorld != nullptr && mc->theWorld->getWorldInfo() != nullptr &&
                                  mc->theWorld->getWorldInfo()->isHardcoreModeEnabled();
            if (hardcore)
            {
                ISaveHandler *saveHandler = mc->theWorld->getSaveHandler();
                const std::string saveDirectoryName = saveHandler != nullptr ? saveHandler->getSaveDirectoryName() : std::string();
                mc->changeWorld2(nullptr, "Deleting world");

                ISaveFormat *saveFormat = mc->getSaveLoader();
                if (saveFormat != nullptr && !saveDirectoryName.empty())
                {
                    saveFormat->flushCache();
                    saveFormat->deleteWorldDirectory(saveDirectoryName);
                }
                mc->displayGuiScreen(new GuiMainMenu());
            }
            else if (mc->thePlayer != nullptr)
            {
                mc->thePlayer->respawnPlayer();
                mc->displayGuiScreen(nullptr);
            }
            break;
        }

        case 2:
            if (mc->isMultiplayerWorld() && mc->theWorld != nullptr)
                mc->theWorld->sendQuittingDisconnectingPacket();
            mc->changeWorld1(nullptr);
            mc->displayGuiScreen(new GuiMainMenu());
            break;
    }
}

void GuiGameOver::drawScreen(int_t i, int_t j, float_t f)
{
    if (!textCached)
    {
        cachedHardcore = mc->theWorld != nullptr && mc->theWorld->getWorldInfo() != nullptr &&
                         mc->theWorld->getWorldInfo()->isHardcoreModeEnabled();
        cachedTitle = StatCollector::translateToLocal(cachedHardcore ? "deathScreen.title.hardcore" : "deathScreen.title");
        if (cachedHardcore)
            cachedHardcoreInfo = StatCollector::translateToLocal("deathScreen.hardcoreInfo");
        textCached = true;
    }

    drawGradientRect(0, 0, width, height, 0x60500000, 0xa0803030);
    renderPushMatrix();
    renderScale(2.0f, 2.0f, 2.0f);
    drawCenteredString(fontRenderer, cachedTitle, width / 2 / 2, 30, 0xffffff);
    renderPopMatrix();

    if (cachedHardcore)
        drawCenteredString(fontRenderer, cachedHardcoreInfo, width / 2, 144, 0xffffff);

    // mc->thePlayer is checked everywhere else this screen touches it
    // (actionPerformed()'s respawn/delete-world branches both guard it) --
    // this was the one place that didn't, dereferencing it unconditionally
    // right on the frame this screen first appears (the moment thePlayer
    // died). Not proven to be the real-hardware crash reported alongside a
    // death (a plain dark-red gradient screen with the game otherwise
    // frozen -- this function's own drawGradientRect() background colour,
    // not a distinct crash/exception screen, so whatever hung did so during
    // or right after this draw), but it is a real, unguarded null
    // dereference on exactly the code path that was running, so worth
    // closing regardless of whether it turns out to be the whole story.
    //
    // cachedScoreValid latches true the first frame thePlayer is non-null and
    // is never cleared -- the score can't legitimately change afterward while
    // this screen is showing, so once captured it just redraws from cache.
    if (!cachedScoreValid && mc->thePlayer != nullptr)
    {
        cachedScoreText = StatCollector::translateToLocal("deathScreen.score") + ": §e" + std::to_string(mc->thePlayer->getScore());
        cachedScoreValid = true;
    }
    if (cachedScoreValid)
        drawCenteredString(fontRenderer, cachedScoreText, width / 2, 100, 0xffffff);
    GuiScreen::drawScreen(i, j, f);
}

bool GuiGameOver::doesGuiPauseGame()
{
    return false;
}

void GuiGameOver::updateScreen()
{
    GuiScreen::updateScreen();
    ++cooldownTimer;
    if (cooldownTimer == 20)
    {
        for (GuiButton *button : controlList)
            button->enabled = true;
    }
}
