#include "dsi/system/DsiArm7Compress.h"

#ifdef DSI_PLATFORM

#include <nds.h>

#include <cstring>
#include <vector>

#include "dsi/arm7/DsiArm7ChunkCompress.h"
#include "platform/PlatformCompat.h"

#include <zlib.h> // compressBound() only -- sizing the output buffer, no compression happens on this side

namespace
{
bool g_handlerInstalled = false;

// Owned entirely by this file -- never the caller's own buffer. The one
// caller (McRegionChunkLoader.cpp) builds its raw NBT into a
// std::vector<byte_t> that a later cancelSlicedSave()/beginSlicedSave()
// for a DIFFERENT chunk can clear() or reassign (potentially reallocating,
// i.e. freeing, its backing storage) while this ARM7 job is still mid-
// flight reading it -- a real cross-CPU use-after-free if this file handed
// the ARM7 that pointer directly instead of a private copy. Copying costs
// a memcpy of a Beta chunk's ~85KB raw NBT, which is noise next to the
// compression work itself, on either CPU.
std::vector<std::uint8_t> g_inputBuf;
std::vector<std::uint8_t> g_outputBuf;

// Set by onCompressResponse(), which -- like every FifoDatamsgHandlerFunc --
// runs in an interrupt context (nds/fifocommon.h's own doc comment), so
// these three are the only channel of communication with the non-IRQ code
// in dsiArm7PollCompress() that acts on them. volatile, not std::atomic:
// this is a single producer (the IRQ handler) and single consumer (the
// one call site that polls, never from an IRQ itself), each field is a
// plain 32-bit-or-smaller value, and this project's own existing FIFO
// handlers elsewhere (e.g. the sound/wifi ones libnds installs) use the
// same plain-volatile-flag shape for the identical reason -- no torn
// reads/writes are possible on this hardware for a single aligned word.
volatile bool g_requestPending = false;
volatile std::uint32_t g_responseStatus = DSI_ARM7_COMPRESS_STATUS_OK;
volatile std::uint32_t g_responseLength = 0;

std::uint64_t g_requestStartUs = 0;

void onCompressResponse(int numBytes, void *userdata)
{
    (void)userdata;
    DsiArm7CompressResponse resp;
    if (numBytes != (int)sizeof(resp))
    {
        fifoGetDatamsg(DSI_ARM7_COMPRESS_RESPONSE_CHANNEL, 0, nullptr);
        // Treat a malformed response as a failure rather than leaving
        // g_requestPending stuck true forever (which would wedge this
        // feature off for the rest of the session -- every later
        // beginSlicedSave() would see "previous request still pending"
        // and decline permanently). dsiArm7PollCompress()'s caller already
        // has a per-chunk ARM9 fallback for a Failed result.
        g_responseStatus = DSI_ARM7_COMPRESS_STATUS_ZLIB_ERROR;
        g_responseLength = 0;
        g_requestPending = false;
        return;
    }
    fifoGetDatamsg(DSI_ARM7_COMPRESS_RESPONSE_CHANNEL, (int)sizeof(resp), (std::uint8_t *)&resp);
    g_responseStatus = resp.status;
    g_responseLength = resp.compressedLen;
    g_requestPending = false;
}

void ensureHandlerInstalled()
{
    if (g_handlerInstalled)
        return;
    fifoSetDatamsgHandler(DSI_ARM7_COMPRESS_RESPONSE_CHANNEL, onCompressResponse, nullptr);
    g_handlerInstalled = true;
}
} // namespace

bool dsiArm7BeginCompress(const void *data, std::size_t length)
{
    if (data == nullptr || length == 0)
        return false;
    if (g_requestPending)
        return false; // previous job not finished -- never double-book, see this file's own header comment

    ensureHandlerInstalled();

    g_inputBuf.assign(static_cast<const std::uint8_t *>(data), static_cast<const std::uint8_t *>(data) + length);

    const uLong outCapBound = compressBound(static_cast<uLong>(length));
    if (g_outputBuf.size() < outCapBound)
        g_outputBuf.resize(static_cast<std::size_t>(outCapBound));

    // The ARM7 reads g_inputBuf and writes g_outputBuf directly off main
    // RAM, bypassing the ARM9's data cache entirely -- flush before
    // sending the addresses so it sees bytes actually written, not stale
    // RAM behind still-dirty ARM9 cache lines. No flush needed for
    // g_outputBuf here: nothing has written anything meaningful into it
    // yet this round (dsiArm7PollCompress() invalidates it before THIS
    // side ever reads it back, which is the direction that actually
    // matters).
    DC_FlushRange(g_inputBuf.data(), g_inputBuf.size());

    DsiArm7CompressRequest req;
    req.inPtr = (std::uint32_t)(std::uintptr_t)g_inputBuf.data();
    req.inLen = (std::uint32_t)g_inputBuf.size();
    req.outPtr = (std::uint32_t)(std::uintptr_t)g_outputBuf.data();
    req.outCapacity = (std::uint32_t)g_outputBuf.size();

    g_requestPending = true;
    g_requestStartUs = PlatformCompat::getMonotonicMicros();

    if (!fifoSendDatamsg(DSI_ARM7_COMPRESS_REQUEST_CHANNEL, (std::uint32_t)sizeof(req), (std::uint8_t *)&req))
    {
        // FIFO send itself failed (queue full/link down) -- back out
        // rather than leave g_requestPending stuck true with nothing ever
        // going to clear it.
        g_requestPending = false;
        return false;
    }
    return true;
}

DsiArm7CompressResult dsiArm7PollCompress(std::size_t &compressedLengthOut)
{
    if (g_requestPending)
        return DsiArm7CompressResult::Pending;
    if (g_responseStatus != DSI_ARM7_COMPRESS_STATUS_OK)
        return DsiArm7CompressResult::Failed;

    // Discard any stale ARM9-cached copy of this range before reading the
    // bytes the ARM7 -- which has no data cache of its own, so its writes
    // are visible in main RAM immediately -- actually wrote.
    DC_InvalidateRange(g_outputBuf.data(), g_responseLength);
    compressedLengthOut = static_cast<std::size_t>(g_responseLength);
    return DsiArm7CompressResult::Done;
}

const void *dsiArm7CompressedData()
{
    return g_outputBuf.data();
}

std::uint64_t dsiArm7CompressElapsedUs()
{
    return PlatformCompat::getMonotonicMicros() - g_requestStartUs;
}

#endif // DSI_PLATFORM
