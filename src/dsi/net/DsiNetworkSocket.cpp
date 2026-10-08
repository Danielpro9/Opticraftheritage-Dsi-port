#include "DsiNetworkSocket.h"

#include "dsi/net/DsiSrvLookup.h"
#include "platform/Log.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dswifi9.h>
#include <nds.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{

// Confirmed against ClassiCube's own NDS port (src/nds/Platform_NDS.c,
// InitNetworking()): Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE) +
// Wifi_AutoConnect() is the one-call sequence that reuses the console's own
// saved WFC settings instead of this project inventing its own WiFi setup
// screen. ClassiCube waits for association with cothread_yield_irq(), a
// cooperative-threading primitive this project has no equivalent of (see
// NetworkManager.h's own PLATFORM_DSI comment on why real threads do not
// work here); a plain swiWaitForVBlank() loop does the same "yield to the
// next vblank interrupt instead of busy-spinning the CPU" job without it.
bool g_wifiReady = false;
bool g_wifiAttempted = false;

} // namespace

bool DsiNetworkSocket::ensureWifiReady()
{
    if (g_wifiReady)
        return true;
    if (g_wifiAttempted)
        return false; // already tried once this run and failed; do not retry forever

    g_wifiAttempted = true;

    if (!Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE))
    {
        MC_LOG_ERROR("network", "DSi: Wifi_InitDefault failed\n");
        return false;
    }

    Wifi_AutoConnect();

    // 60 frames/sec * 10 sec, same ceiling ClassiCube's reference
    // implementation uses -- long enough for a real association, short
    // enough not to leave "Connecting..." on screen forever if the
    // console's saved WFC settings do not actually reach a network.
    constexpr int kMaxVblanks = 60 * 10;
    for (int i = 0; i < kMaxVblanks; i++)
    {
        const int status = Wifi_AssocStatus();
        if (status == ASSOCSTATUS_ASSOCIATED)
        {
            g_wifiReady = true;
            MC_LOG_INFO("network", "DSi: WiFi associated\n");
            return true;
        }
        if (status == ASSOCSTATUS_CANNOTCONNECT)
        {
            MC_LOG_ERROR("network", "DSi: WiFi cannot connect (no saved WFC settings?)\n");
            return false;
        }
        swiWaitForVBlank();
    }

    MC_LOG_ERROR("network", "DSi: WiFi association timed out\n");
    return false;
}

void DsiNetworkSocket::releaseWifi()
{
    if (!g_wifiAttempted)
        return; // ensureWifiReady() never ran -- nothing to tear down

    // Wifi_DisconnectAP() drops the association; Wifi_DisableWifi() then
    // powers the WiFi hardware itself down (the ARM7 side stops needing to
    // service it at all -- see arm7/main_arm7.c's Wifi_Update() call, which
    // keeps running every vblank regardless of this call, but has nothing
    // left to do once the chip is disabled). Safe to call even if
    // association never actually completed (g_wifiReady == false but
    // g_wifiAttempted == true, e.g. a timed-out or refused connect
    // attempt): Wifi_InitDefault() still brought the hardware up in that
    // case, so it still needs to be told to shut back down.
    Wifi_DisconnectAP();
    Wifi_DisableWifi();

    g_wifiReady = false;
    g_wifiAttempted = false;

    MC_LOG_INFO("network", "DSi: WiFi released\n");
}

bool DsiNetworkSocket::connect(const std::string &host, int port)
{
    close();

    if (!ensureWifiReady())
        return false;

    // Real-hardware report: a server confirmed reachable from another
    // device (same hostname, same port) always got an active TCP refusal
    // from the DSi. Root cause: that server is hosted on a shared/budget
    // provider (HolyHosting) that only exposes a non-default port through
    // an SRV record, not at the literal typed host:port -- the real
    // client-side behavior this port's Beta 1.2.5 base predates entirely
    // (see DsiSrvLookup.h's own comment). Best-effort: a server that
    // never needed this is unaffected beyond one extra bounded UDP round
    // trip before falling through to the host:port exactly as typed.
    std::string connectHost = host;
    int connectPort = port;
    std::string srvHost;
    int srvPort = 0;
    if (dsiResolveMinecraftSrv(host, srvHost, srvPort))
    {
        connectHost = srvHost;
        connectPort = srvPort;
    }

    hostent *resolved = gethostbyname(connectHost.c_str());
    if (resolved == nullptr || resolved->h_addr_list == nullptr || resolved->h_addr_list[0] == nullptr)
    {
        MC_LOG_ERROR("network", "DSi: gethostbyname(%s) failed\n", connectHost.c_str());
        return false;
    }

    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0)
        return false;

    sockaddr_in target;
    std::memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<unsigned short>(connectPort));
    std::memcpy(&target.sin_addr, resolved->h_addr_list[0], sizeof(target.sin_addr));

    // Real-hardware report: a server confirmed reachable (same address,
    // same port) from another device got "Connection refused" from this
    // port specifically. ECONNREFUSED is a real RST from *something* at the
    // IP this resolved to -- it does not by itself say whether that IP is
    // actually the server's. Logging it (dotted-decimal, not just the
    // hostname the player typed) is what the next real-hardware log needs
    // to tell a DNS problem (dswifi9's gethostbyname() resolving to a
    // different address than the player's own PC gets for the same name --
    // its DNS server comes from the WFC AP's DHCP, which some routers hand
    // out differently to different clients) apart from a TCP-level one (the
    // right IP, but something about this console's TCP stack specifically
    // getting an active reject a normal client's connection does not).
    // Formatted by hand from the raw address bytes rather than inet_ntop():
    // this toolchain's socket support is a minimal BSD-compatible shim over
    // dswifi9 (see this class's own header comment), and this session has
    // already found more than one standard libc/POSIX symbol missing from
    // it at link time -- not worth risking on a pure diagnostic.
    const std::uint8_t *addrBytes = reinterpret_cast<const std::uint8_t *>(&target.sin_addr);
    char resolvedIp[16];
    std::snprintf(resolvedIp, sizeof(resolvedIp), "%u.%u.%u.%u",
        addrBytes[0], addrBytes[1], addrBytes[2], addrBytes[3]);
    MC_LOG_INFO("network", "DSi: %s resolved to %s\n", connectHost.c_str(), resolvedIp);

    // Deliberately still blocking for this one call -- matches every other
    // platform's NetworkManager constructor (a synchronous connect(), the
    // same "Connecting..." UX vanilla Minecraft already has). Only the
    // ongoing per-tick reads/writes after this need to be non-blocking.
    if (::connect(socketFd, reinterpret_cast<sockaddr *>(&target), sizeof(target)) < 0)
    {
        MC_LOG_ERROR("network", "DSi: connect(%s [%s]:%d) failed: %s\n",
            connectHost.c_str(), resolvedIp, connectPort, strerror(errno));
        ::closesocket(socketFd);
        return false;
    }

    int nonBlocking = 1;
    if (ioctl(socketFd, FIONBIO, &nonBlocking) < 0)
    {
        ::closesocket(socketFd);
        return false;
    }

    fd = socketFd;
    remoteAddr = host + ":" + std::to_string(port);
    return true;
}

void DsiNetworkSocket::close()
{
    if (fd < 0)
        return;
    shutdown(fd, 2); // SHUT_RDWR
    closesocket(fd);
    fd = -1;
}

bool DsiNetworkSocket::recvAvailable(std::vector<byte_t> &out)
{
    if (fd < 0)
        return false;

    // One read's worth per poll call, not a loop-until-EWOULDBLOCK: a
    // single Minecraft packet is at most a few KB (map chunk packets, the
    // biggest kind, are already size-capped upstream), so draining more
    // than this in one call would mean one tick doing many ticks' worth of
    // recv() work -- the same per-call-budget reasoning this session's
    // chunk-load/save slicing already applies elsewhere, just sized for a
    // socket read instead of a zlib call.
    byte_t chunk[2048];
    const int count = recv(fd, chunk, sizeof(chunk), 0);
    if (count > 0)
    {
        out.insert(out.end(), chunk, chunk + count);
        return true;
    }
    if (count == 0)
        return false; // orderly close by the peer

    // EWOULDBLOCK/EAGAIN: nothing available right now, not an error --
    // the normal case for a non-blocking socket polled once per tick.
    return errno == EWOULDBLOCK || errno == EAGAIN;
}
