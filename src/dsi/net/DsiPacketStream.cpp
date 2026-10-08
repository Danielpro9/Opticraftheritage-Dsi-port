#include "DsiPacketStream.h"

#include <cerrno>

#include <sys/socket.h>

std::streambuf::int_type DsiRecvStreamBuf::underflow()
{
    throw DsiPacketIncomplete();
}

std::streamsize DsiSendStreamBuf::xsputn(const char *s, std::streamsize n)
{
    if (n > 0)
    {
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(s);
        pending.insert(pending.end(), bytes, bytes + n);
    }
    return n;
}

std::streambuf::int_type DsiSendStreamBuf::overflow(int_type ch)
{
    if (traits_type::eq_int_type(ch, traits_type::eof()))
        return traits_type::not_eof(ch);
    pending.push_back(static_cast<unsigned char>(traits_type::to_char_type(ch)));
    return ch;
}

bool DsiSendStreamBuf::sendPending(int fd)
{
    if (fd < 0 || pending.empty())
        return true;

    const int sent = send(fd, pending.data(), pending.size(), 0);
    if (sent > 0)
    {
        pending.erase(pending.begin(), pending.begin() + sent);
        return true;
    }
    if (sent == 0)
        return true; // nothing accepted this call; try again next poll

    // EWOULDBLOCK/EAGAIN: the socket's send buffer is full right now, not a
    // real failure -- same "try again next poll" contract recvAvailable()
    // has on the read side.
    return errno == EWOULDBLOCK || errno == EAGAIN;
}
