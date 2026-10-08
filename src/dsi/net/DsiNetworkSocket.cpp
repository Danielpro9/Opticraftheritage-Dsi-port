#include "DsiNetworkSocket.h"

#include "platform/Log.h"

#include <cerrno>
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

bool DsiNetworkSocket::connect(const std::string &host, int port)
{
    close();

    if (!ensureWifiReady())
        return false;

    hostent *resolved = gethostbyname(host.c_str());
    if (resolved == nullptr || resolved->h_addr_list == nullptr || resolved->h_addr_list[0] == nullptr)
    {
        MC_LOG_ERROR("network", "DSi: gethostbyname(%s) failed\n", host.c_str());
        return false;
    }

    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0)
        return false;

    sockaddr_in target;
    std::memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<unsigned short>(port));
    std::memcpy(&target.sin_addr, resolved->h_addr_list[0], sizeof(target.sin_addr));

    // Deliberately still blocking for this one call -- matches every other
    // platform's NetworkManager constructor (a synchronous connect(), the
    // same "Connecting..." UX vanilla Minecraft already has). Only the
    // ongoing per-tick reads/writes after this need to be non-blocking.
    if (::connect(socketFd, reinterpret_cast<sockaddr *>(&target), sizeof(target)) < 0)
    {
        MC_LOG_ERROR("network", "DSi: connect(%s:%d) failed: %s\n", host.c_str(), port, strerror(errno));
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
