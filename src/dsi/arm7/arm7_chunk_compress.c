// ARM7 half of the chunk-save compression offload -- see DsiArm7ChunkCompress.h
// for the wire format and src/dsi/system/DsiArm7Compress.h for the full
// design/rationale. This file provides dsiArm7ChunkCompressInit() (called
// once from main_arm7.c's main(), registers the request handler) and
// dsiArm7ChunkCompressPoll() (called every iteration of main_arm7.c's main
// loop, outside any interrupt context -- see below for why that matters).
//
// SPDX-License-Identifier: Zlib

#include <nds.h>
#include <string.h>

#include "DsiArm7ChunkCompress.h"
#include "zlib.h" // -I../../../external/zlib -I build/zconf, see this dir's Makefile

// zlib's own working state (deflate_state: the match-finding hash chains
// and the sliding window) is normally heap-allocated via malloc, through
// zalloc/zfree. This ARM7 build has never used a heap before this file
// (main_arm7.c's own stock template does no dynamic allocation at all), so
// rather than gamble on newlib's heap being correctly sized/configured for
// a first-ever user here, this hands zlib a FIXED static arena instead and
// implements zalloc as a trivial bump allocator over it -- deterministic,
// no fragmentation risk, and sidesteps the "does ARM7 malloc even work"
// unknown entirely. Safe because only one compression job ever runs at a
// time (see dsiArm7ChunkCompressPoll()'s own comment): the arena is reset
// to empty at the start of every job, so there is nothing to free between
// jobs and nothing to fragment.
//
// TWL_BSS (nds/ndstypes.h) places this in the DSi-enhanced-mode-only
// twl_iwram region (256 KB, see sys/crts/ds_arm7.ld) instead of the
// default, much tighter 96 KB iwram region this ARM7 binary's code and
// libnds7/dswifi7's own runtime already share -- this project's ARM7 side
// has never used anything in the twl_iwram region before, so it is
// otherwise sitting completely unused. windowBits=12/memLevel=6 below cost
// zlib roughly (1<<14)+(1<<15) = 48 KB by its own documented formula
// (zlib.h's deflateInit2() comment); 80 KB leaves real headroom over that
// estimate without even approaching the 256 KB ceiling. First estimate,
// not a measurement -- see this file's own windowBits/memLevel comment.
#define ARM7_ZLIB_ARENA_SIZE (80 * 1024)
static TWL_BSS uint8_t s_zlibArena[ARM7_ZLIB_ARENA_SIZE];
static uint32_t s_zlibArenaUsed;

static voidpf arm7ZlibAlloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    // 4-byte alignment: zlib's internal structs are plain C structs of
    // 32-bit fields/pointers: this ARM (ARM7TDMI) traps on unaligned
    // multi-byte accesses rather than silently handling them, so every
    // allocation must land on a 4-byte boundary, not just the first.
    uint32_t bytes = (uint32_t)items * (uint32_t)size;
    uint32_t aligned = (s_zlibArenaUsed + 3u) & ~3u;
    if (aligned + bytes > ARM7_ZLIB_ARENA_SIZE)
        return (voidpf)0; // zlib treats a NULL return as out-of-memory and fails the call cleanly
    s_zlibArenaUsed = aligned + bytes;
    return (voidpf)&s_zlibArena[aligned];
}

static void arm7ZlibFree(voidpf opaque, voidpf address)
{
    // No-op: see this file's own banner comment on why a bump allocator
    // reset once per job (not a real free) is safe here.
    (void)opaque;
    (void)address;
}

// Set by the request datamsg handler (runs in IRQ context -- libnds's own
// FifoDatamsgHandlerFunc doc comment says so explicitly), read/cleared by
// dsiArm7ChunkCompressPoll() from the ordinary main loop. volatile: this is
// the only channel of communication between an IRQ handler and the
// non-IRQ code that acts on it.
static volatile int s_jobPending;
static DsiArm7CompressRequest s_job;

static void onCompressRequest(int numBytes, void *userdata)
{
    (void)userdata;
    DsiArm7CompressRequest req;
    if (numBytes != (int)sizeof(req))
    {
        // Wrong-sized message -- drain it so it doesn't wedge the FIFO,
        // but don't act on garbage.
        fifoGetDatamsg(DSI_ARM7_COMPRESS_REQUEST_CHANNEL, 0, 0);
        return;
    }
    fifoGetDatamsg(DSI_ARM7_COMPRESS_REQUEST_CHANNEL, (int)sizeof(req), (uint8_t *)&req);
    // Deliberately NOT doing the actual deflate work here: handlers run in
    // an interrupt context (see FifoDatamsgHandlerFunc's own doc comment in
    // nds/fifocommon.h), and a chunk's worth of deflate is a potentially
    // long-running loop (hundreds of ms on the much faster ARM9 -- see
    // DsiArm7ChunkCompress.h's own cost comment) that would block every
    // other interrupt on this CPU for that whole time, including the vblank
    // handler driving input/WiFi polling. Just latch the request and let
    // the ordinary main loop below pick it up outside any IRQ context.
    //
    // Single in-flight slot only: the ARM9 side (DsiArm7Compress.cpp) never
    // sends a second request while one is still pending, so overwriting
    // s_job unconditionally here is safe -- there is nothing to preserve.
    s_job = req;
    s_jobPending = 1;
}

void dsiArm7ChunkCompressInit(void)
{
    fifoSetDatamsgHandler(DSI_ARM7_COMPRESS_REQUEST_CHANNEL, onCompressRequest, 0);
}

static void runCompressJob(const DsiArm7CompressRequest *req)
{
    DsiArm7CompressResponse resp;
    resp.status = DSI_ARM7_COMPRESS_STATUS_ZLIB_ERROR;
    resp.compressedLen = 0;

    s_zlibArenaUsed = 0; // reset the bump arena -- see its own comment on why this is safe

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.zalloc = arm7ZlibAlloc;
    zs.zfree = arm7ZlibFree;
    zs.opaque = 0;

    // windowBits=12 (4 KB base window), memLevel=6: a deliberate reduction
    // from the ARM9 path's windowBits=MAX_WBITS(15)/memLevel=8 (zlib's
    // defaults -- see McRegionChunkLoader.cpp's beginSlicedSave(), which
    // this mirrors otherwise) to fit this CPU's own tiny RAM instead of the
    // ARM9's much larger heap -- see this file's own arena-sizing comment
    // for the memory-budget math. A real DEFLATE stream is self-describing
    // about its own window size up to 32 KB, so a decoder (this project's
    // own inflate path, or any other zlib) with a large-enough window can
    // always decode a stream encoded with a smaller one -- this changes
    // nothing about save-file compatibility, only how well THIS compressor
    // finds long-distance matches. Z_BEST_SPEED, not the ARM9 fallback's
    // level (which itself already picks Z_BEST_SPEED on this platform --
    // see PLATFORM_FAST_REGION_COMPRESSION): the whole point of running
    // this on the otherwise-idle ARM7 is spending zero ARM9 budget, not
    // minimizing this CPU's own wall-clock time, but there is no reason to
    // spend extra cycles here either when nothing needs the extra ratio.
    if (deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 12, 6, Z_DEFAULT_STRATEGY) != Z_OK)
    {
        fifoSendDatamsg(DSI_ARM7_COMPRESS_RESPONSE_CHANNEL, (uint32_t)sizeof(resp), (uint8_t *)&resp);
        return;
    }

    zs.next_in = (Bytef *)(uintptr_t)req->inPtr;
    zs.avail_in = req->inLen;
    zs.next_out = (Bytef *)(uintptr_t)req->outPtr;
    zs.avail_out = req->outCapacity;

    const int status = deflate(&zs, Z_FINISH);
    if (status == Z_STREAM_END)
    {
        resp.status = DSI_ARM7_COMPRESS_STATUS_OK;
        resp.compressedLen = req->outCapacity - zs.avail_out;
    }
    else if (status == Z_OK && zs.avail_out == 0)
    {
        // Ran out of output room before finishing -- should not happen in
        // practice (the ARM9 side sizes outCapacity with zlib's own
        // compressBound()), but checked rather than trusted; see this
        // header's own status comment.
        resp.status = DSI_ARM7_COMPRESS_STATUS_TOO_BIG;
    }
    deflateEnd(&zs);

    fifoSendDatamsg(DSI_ARM7_COMPRESS_RESPONSE_CHANNEL, (uint32_t)sizeof(resp), (uint8_t *)&resp);
}

void dsiArm7ChunkCompressPoll(void)
{
    // Called from main_arm7.c's ordinary main loop, never from IRQ context
    // -- see onCompressRequest()'s own comment for why the actual work is
    // deferred to here instead of running inline in the handler.
    if (!s_jobPending)
        return;
    s_jobPending = 0;
    runCompressJob(&s_job);
}
