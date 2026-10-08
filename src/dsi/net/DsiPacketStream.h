#pragma once

// DSi-only: makes the shared, 150+-file Packet::readPacketData() tree
// (net/minecraft/src/Packet*.cpp) usable from a socket that must never
// block the one real CPU thread this toolchain has (see NetworkManager.h's
// own PLATFORM_DSI comment for why -- std::thread silently never runs its
// callable here, same for Wii's PlatformThread, so the blocking
// background-read-thread design every other platform uses is not available).
//
// None of those 150+ readPacketData() overrides were written to be
// pausable mid-field, and rewriting them to be would be a huge, invasive
// change for what this is. Instead: parse speculatively against whatever
// bytes are already buffered, and if a read runs out of buffered data
// partway through a packet, abort the WHOLE attempt and retry it from byte
// 0 on a later poll, once more bytes have arrived over WiFi. This needs no
// changes to Packet::readPacketData() at all -- DsiRecvStreamBuf is what
// makes "ran out of data" raise an exception instead of looking like EOF or
// blocking, and NetworkManager's DSi poll function is what catches it and
// retries. The persistent receive buffer is never mutated by a failed
// attempt (this streambuf only ever reads it), so there is nothing to roll
// back: trying again later with more bytes appended IS the rollback.
#include <cstddef>
#include <streambuf>
#include <vector>

#include "java/Type.h"

// Thrown by DsiRecvStreamBuf::underflow() instead of returning eof() --
// every call site up the chain (IOUtil::readByte, Packet::readPacket, Packet
// subclasses' own readPacketData()) treats eof()/a short read as a protocol
// error or a closed connection, which "the rest of this packet just hasn't
// arrived yet" is neither. Deliberately NOT derived from std::exception:
// NetworkManager::readPacket()'s existing catch(std::exception&) is shared
// with every other platform and must keep treating a real parse error as a
// network fault; this type gets its own catch clause instead (PLATFORM_DSI
// only) so the two can never be confused.
class DsiPacketIncomplete
{
};

// Wraps a read-only view of NetworkManager's persistent receive buffer.
// Never mutates it -- underflow() throws rather than advancing past what is
// already there. rebind() re-points the get area at the same buffer after
// NetworkManager appends freshly-received bytes to it, without allocating a
// new streambuf (or a new owning istream) every poll.
class DsiRecvStreamBuf : public std::streambuf
{
public:
    void rebind(const byte_t *data, std::size_t size)
    {
        char *base = reinterpret_cast<char *>(const_cast<byte_t *>(data));
        setg(base, base, base + size);
    }

protected:
    int_type underflow() override;
};

// The send-side counterpart. Packet::writePacketData() serializes into this
// like any other std::ostream; sendPending() is what actually hands bytes to
// the (non-blocking) socket, writing as many as it will currently accept and
// keeping the rest buffered for the next poll instead of looping until
// everything is sent (what Wii's/PC's SocketOutputBuffer do, safe only
// because their socket::write() is allowed to block). Resuming a partially
// sent run of raw bytes needs no exception trick -- unlike a parse, there is
// no structured field to restart, just a byte offset to remember.
class DsiSendStreamBuf : public std::streambuf
{
public:
    explicit DsiSendStreamBuf(std::vector<byte_t> &pending) : pending(pending) {}

    // Sends as much of `pending`'s front as the socket will currently
    // accept (fd < 0 or a would-block return both leave it untouched -- the
    // caller's own connection-liveness check is NetworkManager's, not this
    // class's). Returns false only on a real send() error (the connection
    // is gone), matching DsiNetworkSocket::sendPartial()'s own contract.
    bool sendPending(int fd);

protected:
    std::streamsize xsputn(const char *s, std::streamsize n) override;
    int_type overflow(int_type ch) override;

private:
    std::vector<byte_t> &pending;
};
