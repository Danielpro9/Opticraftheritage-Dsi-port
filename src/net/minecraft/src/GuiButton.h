#pragma once

#include "Gui.h"
#include <string>

class Minecraft;

// net.minecraft.src.GuiButton
class GuiButton : public Gui
{
public:
	GuiButton(int_t id, int_t x, int_t y, const std::string &text);
	GuiButton(int_t id, int_t x, int_t y, int_t w, int_t h, const std::string &text);
	virtual ~GuiButton() = default;

protected:
	virtual int_t getHoverState(bool hovered);

public:
	virtual void drawButton(Minecraft *mc, int_t mouseX, int_t mouseY);

protected:
	virtual void mouseDragged(Minecraft *mc, int_t mouseX, int_t mouseY);

public:
	virtual void mouseReleased(int_t mouseX, int_t mouseY);
	virtual bool mousePressed(Minecraft *mc, int_t mouseX, int_t mouseY);
	virtual void setKeyboardSelected(bool selected);
	virtual bool adjustKeyboard(Minecraft *mc, int_t direction) { (void)mc; (void)direction; return false; }

protected:
	int_t width;
	int_t height;

public:
	int_t xPosition;
	int_t yPosition;
	std::string displayString;
	int_t id;
	bool enabled;
	bool enabled2;

protected:
	bool keyboardSelected;

private:
	// drawButton() used to measure displayString's width fresh every frame
	// via Gui::drawCenteredString() -> FontRenderer::getStringWidth(), which
	// heap-allocates a UTF-16 std::vector and scans it per character -- for
	// every button on screen, every frame, while any menu is open. displayString
	// is a public field written directly by a handful of call sites (option/
	// slider labels) rather than through a setter, so instead of intercepting
	// every writer, the width is just re-measured whenever it no longer
	// matches this cached copy -- a cheap string compare on every unchanged
	// frame instead of the full measurement.
	std::string cachedWidthString;
	int_t cachedWidth = 0;

public:

	// Accesores para deteccion de hover (tooltips OptiFine); width/height son protected.
	int_t getButtonWidth() const { return width; }
	int_t getButtonHeight() const { return height; }
};
