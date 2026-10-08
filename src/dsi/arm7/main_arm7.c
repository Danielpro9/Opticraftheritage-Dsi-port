// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2005 Michael Noland (joat)
// Copyright (C) 2005 Jason Rogers (Dovoto)
// Copyright (C) 2005-2015 Dave Murphy (WinterMute)
//
// This is libnds/BlocksDS's own standard default ARM7 core (the same shape
// every devkitPro/BlocksDS NDS homebrew project starts from, and the same
// one ClassiCube's NDS port builds verbatim as its own misc/nds/main_arm7.c)
// with the Wifi_Update() pump added to the vblank handler and installWifiFIFO()
// added to bring DSWiFi online. See Makefile.game's own ARM7ELF comment for
// why this project needs its own ARM7 build at all now instead of the
// prebuilt arm7_minimal.elf it used before: that one has no DSWiFi support
// compiled in, and multiplayer needs the ARM7 side of the WiFi chip driver
// (only ARM7 can talk to the WiFi hardware on this console -- not a software
// choice). Everything else here (sound/touch/power/RTC init) is unchanged
// from the stock template; this file deliberately adds the one thing missing
// rather than writing a new ARM7 core from scratch.
#include <dswifi7.h>
#include <nds.h>

volatile bool exit_loop = false;

static void power_button_callback(void)
{
    exit_loop = true;
}

static void vblank_handler(void)
{
    inputGetAndSend();
    Wifi_Update();
}

int main(int argc, char *argv[])
{
    enableSound();
    readUserSettings();
    ledBlink(0);
    touchInit();

    irqInit();
    irqSet(IRQ_VBLANK, vblank_handler);

    fifoInit();
    installWifiFIFO();
    installSoundFIFO();
    installSystemFIFO();

    setPowerButtonCB(power_button_callback);

    initClockIRQTimer(3);
    irqEnable(IRQ_VBLANK | IRQ_RTC);

    while (!exit_loop)
    {
        const uint16_t key_mask = KEY_SELECT | KEY_START | KEY_L | KEY_R;
        uint16_t keys_pressed = ~REG_KEYINPUT;

        if ((keys_pressed & key_mask) == key_mask)
            exit_loop = true;

        swiWaitForVBlank();
    }

    return 0;
}
