#pragma once

#include <string>

#include "GuiScreen.h"

// net.minecraft.src.GuiGameOver
class GuiGameOver : public GuiScreen
{
public:
	GuiGameOver();

	void initGui() override;
	void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
	bool doesGuiPauseGame() override;
	void updateScreen() override;

protected:
	int_t cooldownTimer;
	void keyTyped(char_t c, int_t key) override;
	void actionPerformed(GuiButton *button) override;

private:
	// Death is a one-time event for the lifetime of this screen instance, so
	// title/hardcoreInfo (hardcore state never changes while shown) and the
	// score text (frozen once thePlayer is confirmed non-null -- see
	// drawScreen()'s own comment on why that check can't be dropped) are
	// translated/formatted once instead of every rendered frame this screen
	// is up (can sit on screen for seconds while the player reads it).
	bool textCached = false;
	bool cachedHardcore = false;
	std::string cachedTitle;
	std::string cachedHardcoreInfo;
	bool cachedScoreValid = false;
	std::string cachedScoreText;
};
