#pragma once

#include <cstdint>

enum PlatformTextAction : std::uint32_t
{
    PLATFORM_TEXT_LEFT   = 1u << 0,
    PLATFORM_TEXT_RIGHT  = 1u << 1,
    PLATFORM_TEXT_UP     = 1u << 2,
    PLATFORM_TEXT_DOWN   = 1u << 3,
    PLATFORM_TEXT_TYPE   = 1u << 4,
    PLATFORM_TEXT_BACK   = 1u << 5,
    PLATFORM_TEXT_SPACE  = 1u << 6,
    PLATFORM_TEXT_SHIFT  = 1u << 7,
    PLATFORM_TEXT_ENTER  = 1u << 8,
    PLATFORM_TEXT_CLOSE  = 1u << 9,
    // Container-screen-only actions: "place one item from the held stack"
    // (vanilla's right-click-on-a-slot) and "drop the whole held stack"
    // (vanilla's left-click outside the inventory). Only DSi's
    // mapTextButtons() sets these today (from L and R -- see
    // InputBackend_DSI.cpp and ContainerSlotNavigator.cpp), added generically
    // here rather than as a DSi-only constant in case another platform ever
    // wants a dedicated binding too; PS2/WII's own snapshot builders simply
    // never set them, so this is purely additive and changes nothing there.
    PLATFORM_TEXT_SECONDARY = 1u << 10,
    PLATFORM_TEXT_DROP      = 1u << 11,
};

struct PlatformTextInputSnapshot
{
    bool connected = false;
    std::uint32_t held = 0;
    std::uint32_t pressed = 0;
    bool pointerValid = false;
    int pointerX = 0;
    int pointerY = 0;
    int pointerWidth = 0;
    int pointerHeight = 0;
};

struct PlatformGamepadSnapshot
{
    bool connected = false;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
};

struct PlatformKeyboardHints
{
    const char* lines[3] = { nullptr, nullptr, nullptr };
    int lineCount = 0;
};

PlatformTextInputSnapshot platformTextInputSnapshot(int port = 0);
PlatformGamepadSnapshot platformGamepadSnapshot(int port = 0);
PlatformGamepadSnapshot platformRawGamepadSnapshot(int port = 0);
int platformMenuPad();
bool platformMenuPointerActive();
bool platformMenuCursorVisible();
void platformSetMenuCursor(int x, int y);

// Shared console GUI routing state. Game code owns these modes; platform
// backends only use them to decide whether normal gameplay bindings should be
// emitted while a text field or container navigation owns controller buttons.
void platformSetTextInputExclusive(bool active);
bool platformTextInputExclusive();
void platformSetContainerNavigationActive(bool active);
bool platformContainerNavigationActive();

// One-shot flag: set when a focused text field unfocuses (VirtualKeyboard
// closing on a BACK/CLOSE press). platformTextInputExclusive() already stops
// a screen's own navigation from reading pad state while a field is focused,
// but on DSi that guard alone is not enough for the specific press that closes
// the keyboard: DSi's single B button sets both PLATFORM_TEXT_BACK and
// PLATFORM_TEXT_CLOSE at once (see InputBackend_DSI.cpp's mapTextButtons()),
// and GuiScreen::handleInput() drives VirtualKeyboard::tick() (which consumes
// CLOSE, clearing exclusivity) strictly before a screen's own updateScreen()
// runs in the same tick (see Minecraft::runTick()) -- so a screen reading
// PLATFORM_TEXT_BACK right after exclusivity clears sees the very same press
// that just closed the keyboard, not a fresh one. A screen whose own
// navigation reacts to BACK should check this before doing so, and it is
// consumed (reset to false) on read so it never affects a later, real BACK
// press.
void platformNotifyTextInputClosed();
bool platformConsumeTextInputJustClosed();

// Set while a Controls-menu binding is "listening" for a new key. Console pad
// pollers check this to suspend their normal button-to-UI synthesis (menu
// confirm/cancel/scroll) and instead report a raw button press as a synthetic
// key event, so the existing keyboard-capture flow can store it unchanged.
void platformSetPadRebindExclusive(bool active);
bool platformPadRebindExclusive();

const PlatformKeyboardHints& platformKeyboardHints();
const char* platformInputDebugLine();
