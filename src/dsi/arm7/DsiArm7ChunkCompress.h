#pragma once

// Shared ARM9<->ARM7 wire format for offloading region-file chunk-save
// deflate compression onto the ARM7, which this port otherwise leaves
// completely idle (no audio implemented yet; DSWiFi/dswifi7 only does
// anything during an actual multiplayer session) -- see src/dsi/system/
// DsiArm7Compress.h for the full design/rationale and the ARM9-side API;
// this header only defines the byte-for-byte layout both sides must agree
// on. Deliberately plain C (no C++, no libnds includes beyond <stdint.h>)
// so both main_arm7.c and the ARM9-side .cpp can include it unchanged.
//
// Real-hardware measured cost this exists to move off the ARM9's own
// budget: RegionFile::write()'s single compress2() call (what this
// replaces) measured 100-700ms+ for one chunk on real hardware (see
// DsiWorldTuning.h's PLATFORM_CHUNK_SAVE_SLICE_US comment) -- all of it
// CPU time on the ARM9, the shared budget rendering and game logic also
// need. The ARM7 is roughly 4x slower in raw clock (33MHz fixed vs. the
// ARM9's 133MHz in DSi-enhanced mode) and has no data cache, so the SAME
// job takes longer in wall-clock terms there -- an accepted trade, since
// the point is paying zero ARM9 budget for it, not finishing faster. This
// is scoped to the SAVE path only, never load/decompress: a save that
// never lands still has the original uncompressed Chunk object safely in
// memory (see McRegionChunkLoader.cpp's own fallback), while a corrupted
// LOAD would be reading back already-trusted on-disk data -- load staying
// on the proven ARM9 path entirely is a deliberate scope boundary, not an
// oversight.

#include <stdint.h>

// Fixed FIFO channels for this one feature, picked from libnds's user-
// reserved FIFO_USER_01..08 range (nds/fifocommon.h) -- not used by
// anything else in this project (DSWiFi/sound/system each already own
// their own channels).
#define DSI_ARM7_COMPRESS_REQUEST_CHANNEL  8  // FIFO_USER_01
#define DSI_ARM7_COMPRESS_RESPONSE_CHANNEL 9  // FIFO_USER_02

// Sent ARM9 -> ARM7 as one atomic fifoSendDatamsg() (16 bytes, comfortably
// under FIFO_MAX_DATA_BYTES == 128). inPtr/outPtr are addresses in ORDINARY
// shared main RAM -- not ARM7-exclusive memory -- which the ARM7 reads/
// writes directly as a plain pointer (the ARM7 CPU can address all of main
// RAM, just without a cache in front of it); only zlib's own internal
// working state lives in ARM7-exclusive RAM (see arm7_chunk_compress.c).
// The ARM9 side must DC_FlushRange() the input bytes before sending this
// request, so the ARM7 -- which reads main RAM directly, bypassing the
// ARM9's data cache -- sees what was actually written rather than stale
// RAM sitting behind still-dirty ARM9 cache lines. This is exactly the
// class of bug this same session already found and fixed for the GX FIFO
// DMA path (RenderAPI_DSI.cpp's DC_FlushRange additions) -- same mechanism,
// different buffer.
typedef struct
{
    uint32_t inPtr;
    uint32_t inLen;
    uint32_t outPtr;
    uint32_t outCapacity;
} DsiArm7CompressRequest;

// Sent ARM7 -> ARM9 once compression finishes (or fails) -- same datamsg
// shape as the request. The ARM9 side must DC_InvalidateRange() the output
// buffer before reading it, so it does not read a stale ARM9-cached copy of
// memory the ARM7 has since written (the ARM7 itself has no data cache, so
// its own writes are visible in main RAM immediately -- this invalidate is
// purely about the ARM9's OWN cache, not a synchronization primitive the
// ARM7 side needs to participate in).
typedef struct
{
    uint32_t status;       // DSI_ARM7_COMPRESS_STATUS_*
    uint32_t compressedLen;
} DsiArm7CompressResponse;

#define DSI_ARM7_COMPRESS_STATUS_OK          0
#define DSI_ARM7_COMPRESS_STATUS_ZLIB_ERROR  1
// Compressed output would not have fit in outCapacity -- should not happen
// in practice (the ARM9 side sizes the output buffer with zlib's own
// compressBound(), the same call compress2() relies on internally), but
// checked explicitly rather than trusted, since this crosses a CPU
// boundary into a fixed-size arena with no room to grow.
#define DSI_ARM7_COMPRESS_STATUS_TOO_BIG     2
