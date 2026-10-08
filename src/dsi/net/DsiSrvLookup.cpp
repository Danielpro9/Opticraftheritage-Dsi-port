#include "DsiSrvLookup.h"

#include "platform/Log.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <dswifi9.h>
#include <nds.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{

constexpr int kDnsPort = 53;
// ~1s at 60fps: generous for one UDP round trip over WiFi, short enough
// that a server with no SRV record (the common case -- this runs before
// every connection attempt, not just ones that turn out to need it) does
// not make "Connecting..." noticeably longer than it already was.
constexpr int kQueryTimeoutVblanks = 60;
constexpr std::uint16_t kTypeSRV = 33;
constexpr std::uint16_t kClassIN = 1;

void appendDnsName(std::vector<std::uint8_t> &buf, const std::string &name)
{
    std::size_t start = 0;
    while (start <= name.size())
    {
        const std::size_t dot = name.find('.', start);
        const std::size_t end = (dot == std::string::npos) ? name.size() : dot;
        const std::size_t labelLen = end - start;
        // A label longer than 63 bytes is invalid DNS; a 0-length one
        // (e.g. a leading/trailing/doubled dot) is just skipped rather
        // than rejecting the whole query over it.
        if (labelLen > 0 && labelLen <= 63)
        {
            buf.push_back(static_cast<std::uint8_t>(labelLen));
            for (std::size_t i = start; i < end; ++i)
                buf.push_back(static_cast<std::uint8_t>(name[i]));
        }
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    buf.push_back(0);
}

void appendU16(std::vector<std::uint8_t> &buf, std::uint16_t value)
{
    buf.push_back(static_cast<std::uint8_t>(value >> 8));
    buf.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t readU16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

// Skips one DNS name's ENCODING starting at buf[offset] (RFC 1035 4.1.4
// compression included), returning the offset just past it -- not the
// name's text, which nothing here needs for the question section this is
// used on (it is the query this same process just built, so its content
// is already known). Returns SIZE_MAX on anything that would read past
// `len`, so every caller bails out instead of reading out of bounds on a
// malformed response.
std::size_t skipDnsName(const std::uint8_t *buf, std::size_t len, std::size_t offset)
{
    if (offset >= len)
        return SIZE_MAX;
    const std::uint8_t lengthByte = buf[offset];
    if ((lengthByte & 0xC0) == 0xC0)
        return (offset + 1 < len) ? offset + 2 : SIZE_MAX;
    if (lengthByte == 0)
        return offset + 1;
    if ((lengthByte & 0xC0) != 0)
        return SIZE_MAX; // reserved label-length bits
    const std::size_t next = offset + 1 + lengthByte;
    return (next <= len) ? skipDnsName(buf, len, next) : SIZE_MAX;
}

// Decodes a DNS name starting at buf[offset] into `out`, following
// compression pointers (hop-capped so a corrupt/circular response cannot
// loop forever). Returns the offset just past the name's encoding at the
// ORIGINAL offset (never past a followed pointer's target), or SIZE_MAX on
// a malformed name.
std::size_t decodeDnsName(const std::uint8_t *buf, std::size_t len, std::size_t offset, std::string &out)
{
    out.clear();
    std::size_t cursor = offset;
    std::size_t result = SIZE_MAX;
    int hops = 0;
    while (true)
    {
        if (cursor >= len || hops++ > 32)
            return SIZE_MAX;
        const std::uint8_t lengthByte = buf[cursor];
        if ((lengthByte & 0xC0) == 0xC0)
        {
            if (cursor + 1 >= len)
                return SIZE_MAX;
            if (result == SIZE_MAX)
                result = cursor + 2;
            cursor = static_cast<std::size_t>(((lengthByte & 0x3F) << 8) | buf[cursor + 1]);
            continue;
        }
        if ((lengthByte & 0xC0) != 0)
            return SIZE_MAX;
        if (lengthByte == 0)
        {
            if (result == SIZE_MAX)
                result = cursor + 1;
            return result;
        }
        const std::size_t labelStart = cursor + 1;
        const std::size_t labelEnd = labelStart + lengthByte;
        if (labelEnd > len)
            return SIZE_MAX;
        if (!out.empty())
            out.push_back('.');
        out.append(reinterpret_cast<const char *>(buf + labelStart), lengthByte);
        cursor = labelEnd;
    }
}

} // namespace

bool dsiResolveMinecraftSrv(const std::string &host, std::string &outHost, int &outPort)
{
    in_addr gateway{};
    in_addr snmask{};
    in_addr dns1{};
    in_addr dns2{};
    Wifi_GetIPInfo(&gateway, &snmask, &dns1, &dns2);
    if (dns1.s_addr == 0 && dns2.s_addr == 0)
        return false; // no DNS server known (e.g. a manual static-IP setup with none set)

    std::vector<std::uint8_t> query;
    query.reserve(32 + host.size());
    const std::uint16_t queryId = 0x4d43; // "MC" -- arbitrary, only this socket ever sees the reply.
    appendU16(query, queryId);
    appendU16(query, 0x0100); // standard query, recursion desired
    appendU16(query, 1); // QDCOUNT
    appendU16(query, 0); // ANCOUNT
    appendU16(query, 0); // NSCOUNT
    appendU16(query, 0); // ARCOUNT
    appendDnsName(query, "_minecraft._tcp." + host);
    appendU16(query, kTypeSRV);
    appendU16(query, kClassIN);

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;

    int nonBlocking = 1;
    ioctl(fd, FIONBIO, &nonBlocking);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(kDnsPort);
    serverAddr.sin_addr = (dns1.s_addr != 0) ? dns1 : dns2;

    const bool sent = sendto(fd, query.data(), query.size(), 0,
        reinterpret_cast<sockaddr *>(&serverAddr), sizeof(serverAddr)) == static_cast<int>(query.size());

    // A second attempt at the secondary DNS server, only if the primary
    // simply never answers (down, or silently drops an SRV query type it
    // does not like) -- distinct from sendto() itself failing above, which
    // retrying here would not fix either.
    bool triedSecondary = false;
    std::uint8_t response[512];
    int responseLen = -1;

    if (sent)
    {
        for (int frame = 0; frame < kQueryTimeoutVblanks; ++frame)
        {
            swiWaitForVBlank();
            sockaddr_in fromAddr{};
            socklen_t fromLen = sizeof(fromAddr);
            const int received = recvfrom(fd, response, sizeof(response), 0,
                reinterpret_cast<sockaddr *>(&fromAddr), &fromLen);
            if (received > 0)
            {
                responseLen = received;
                break;
            }
            if (!triedSecondary && frame == kQueryTimeoutVblanks / 2 &&
                dns2.s_addr != 0 && dns2.s_addr != dns1.s_addr)
            {
                triedSecondary = true;
                serverAddr.sin_addr = dns2;
                sendto(fd, query.data(), query.size(), 0,
                    reinterpret_cast<sockaddr *>(&serverAddr), sizeof(serverAddr));
            }
        }
    }
    closesocket(fd);

    if (responseLen < 12)
        return false;

    const std::uint16_t respId = readU16(response);
    const std::uint16_t flags = readU16(response + 2);
    const std::uint16_t ancount = readU16(response + 6);
    if (respId != queryId || (flags & 0x8000) == 0 || ancount == 0)
        return false; // not a reply to our query, or a real answer with zero records (no SRV configured)

    std::size_t offset = skipDnsName(response, static_cast<std::size_t>(responseLen), 12);
    if (offset == SIZE_MAX || offset + 4 > static_cast<std::size_t>(responseLen))
        return false;
    offset += 4; // QTYPE + QCLASS, echoed back from the question we sent

    for (std::uint16_t i = 0; i < ancount; ++i)
    {
        std::string unusedName;
        offset = decodeDnsName(response, static_cast<std::size_t>(responseLen), offset, unusedName);
        if (offset == SIZE_MAX || offset + 10 > static_cast<std::size_t>(responseLen))
            return false;

        const std::uint16_t type = readU16(response + offset);
        const std::uint16_t rdlength = readU16(response + offset + 8);
        const std::size_t rdataStart = offset + 10;
        if (rdataStart + rdlength > static_cast<std::size_t>(responseLen))
            return false;

        // SRV RDATA (RFC 2782): PRIORITY(2) WEIGHT(2) PORT(2) TARGET(name).
        // The first SRV record wins rather than sorting by priority/weight
        // -- a real Minecraft SRV setup is overwhelmingly one record, and
        // getting a second-choice target on the rare multi-record case
        // still connects, just not to the most-preferred one.
        if (type == kTypeSRV && rdlength >= 6)
        {
            const std::uint16_t srvPort = readU16(response + rdataStart + 4);
            std::string target;
            if (srvPort != 0 &&
                decodeDnsName(response, static_cast<std::size_t>(responseLen), rdataStart + 6, target) != SIZE_MAX &&
                !target.empty())
            {
                outHost = target;
                outPort = srvPort;
                MC_LOG_INFO("network", "DSi: SRV _minecraft._tcp.%s -> %s:%d\n",
                    host.c_str(), outHost.c_str(), outPort);
                return true;
            }
        }
        offset = rdataStart + rdlength;
    }
    return false;
}
