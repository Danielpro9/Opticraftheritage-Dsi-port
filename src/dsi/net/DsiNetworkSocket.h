#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "java/Type.h"

// DSi-only: brings up DSWiFi and owns one non-blocking TCP connection. See
// DsiPacketStream.h for why reads/writes have to stay non-blocking at all
// (no background thread to put a blocking call on), and arm7/main_arm7.c's
// own comment for why only the ARM7 CPU can drive the WiFi chip in the
// first place -- this class is the ARM9-side half of that split.
class DsiNetworkSocket
{
public:
    // Brings DSWiFi up using the DSi's own saved WFC connection settings
    // (System Settings -> Internet, same as every Nintendo WFC game --
    // there is no in-game WiFi setup UI here, same as ClassiCube's NDS
    // port) and blocks (via swiWaitForVBlank(), not a real sleep) until
    // associated or ~10 seconds pass. Safe to call before every connection
    // attempt: returns immediately once WiFi is already associated from an
    // earlier call. Returns false if association never completes.
    static bool ensureWifiReady();

    // Drops the WiFi association brought up by ensureWifiReady(), on
    // request: a singleplayer world (the overwhelmingly common case) never
    // touches this class at all, but once a multiplayer attempt has turned
    // WiFi on, it stayed on for the rest of the process -- the ARM7 side's
    // Wifi_Update() pump (arm7/main_arm7.c) keeps servicing it every vblank
    // whether or not anything is still using it. Called from
    // NetworkManager::networkShutdown(), which every disconnect path (a
    // clean quit, a connection error, the player leaving the server) and
    // ~NetworkManager() itself already funnel through, so this does not
    // need its own call site anywhere else. Safe to call even if WiFi was
    // never brought up (ensureWifiReady() never ran) or already released.
    static void releaseWifi();

    DsiNetworkSocket() = default;
    ~DsiNetworkSocket() { close(); }
    DsiNetworkSocket(const DsiNetworkSocket &) = delete;
    DsiNetworkSocket &operator=(const DsiNetworkSocket &) = delete;

    // Blocking DNS lookup + TCP connect (the one deliberately-blocking step
    // in this whole class -- matches every platform's existing
    // NetworkManager constructor contract, and vanilla Minecraft's own
    // blocking-connect-with-a-spinner UX). Flips the socket to non-blocking
    // itself once connected, before returning, so every later call here is
    // safe to poll from the main loop.
    bool connect(const std::string &host, int port);
    void close();
    bool isOpen() const { return fd >= 0; }
    int rawFd() const { return fd; }

    // Non-blocking. Appends whatever is currently available (possibly
    // nothing) to out and returns true, or returns false if the connection
    // itself is gone (a real error/orderly close, not "nothing right now").
    bool recvAvailable(std::vector<byte_t> &out);

    const std::string &remoteAddress() const { return remoteAddr; }

private:
    int fd = -1;
    std::string remoteAddr;
};
