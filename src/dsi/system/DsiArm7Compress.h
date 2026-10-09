#pragma once

#ifdef DSI_PLATFORM

#include <cstddef>
#include <cstdint>

// ARM9-side API for offloading region-file chunk-save deflate compression
// onto the ARM7, which this port otherwise leaves completely idle (no
// audio implemented yet; DSWiFi/dswifi7 only does anything during an
// actual multiplayer session). See DsiArm7ChunkCompress.h (src/dsi/arm7/)
// for the wire format both CPUs agree on, and McRegionChunkLoader.cpp's
// beginSlicedSave()/continueSlicedSave() for the one call site that uses
// this -- deliberately the ONLY one: this is scoped to the SAVE path, never
// load/decompress (see that header's own comment on why).
//
// Real-hardware measured cost this exists to move off the ARM9's own
// budget: a single compress2() call for one chunk measured 100-700ms+ on
// this ARM9 (DsiWorldTuning.h's PLATFORM_CHUNK_SAVE_SLICE_US comment) --
// already spread across several 3ms slices so it never shows up as one
// visible hitch, but still 100-700ms+ of TOTAL ARM9 CPU time paid for every
// edited chunk that unloads, time this engine's rendering and game logic
// are also competing for. The ARM7 is roughly 4x slower in raw clock
// (33MHz fixed vs. the ARM9's 133MHz in DSi-enhanced mode) and has no data
// cache, so the SAME job takes longer in wall-clock terms there -- an
// accepted trade, since the point is paying zero ARM9 budget for it, not
// finishing the compression faster.
//
// Protocol, end to end: dsiArm7BeginCompress() copies the caller's raw
// bytes into a buffer this file owns (never the caller's own buffer --
// see its own comment on why a copy, not just a pointer, is the safe
// choice here) and DC_FlushRange()s it before handing its address to the
// ARM7 over FIFO, so the ARM7 -- which reads main RAM directly, bypassing
// the ARM9's data cache -- sees what was actually written instead of
// stale RAM behind still-dirty ARM9 cache lines. The caller polls
// dsiArm7PollCompress() once per its own tick/call interval (matching how
// continueSlicedSave() already polls pumpSlicedDeflate() today); once it
// reports Done, dsiArm7CompressedData() hands back a pointer into this
// file's own output buffer (DC_InvalidateRange()'d first, so the ARM9 does
// not read a stale cached copy of memory the ARM7 has since written) --
// valid only until the next dsiArm7BeginCompress() call.
//
// Only one request is ever in flight: dsiArm7BeginCompress() returns false
// immediately if a previous one is still pending, rather than queuing or
// overwriting it. Every caller already has its own fallback for "declined"
// (McRegionChunkLoader.cpp's existing ARM9 deflate path, unchanged) and
// this project's own save pipeline already serializes chunk saves one at a
// time (ChunkProvider's pendingSaveQueue), so there is no real queuing
// need, and "decline rather than double-book" is the one failure mode that
// can never corrupt or lose anything.

enum class DsiArm7CompressResult
{
    Pending,
    Done,
    Failed,
};

// Copies [data, data+length) into this file's own buffer, DC_FlushRange()s
// it, and sends the compression request to the ARM7. Returns false (does
// nothing) if a previous request is still pending or the ARM7 link has not
// come up -- the caller should fall back to its own ARM9 compression in
// that case, exactly as if this function did not exist.
bool dsiArm7BeginCompress(const void *data, std::size_t length);

// Polls for completion -- cheap, just reads a flag the response handler
// already set; safe to call every tick. Returns Done (and fills
// compressedLengthOut) once the ARM7 finishes successfully; Failed if the
// ARM7 reported an error (the caller should fall back, same as a decline
// above); Pending otherwise.
DsiArm7CompressResult dsiArm7PollCompress(std::size_t &compressedLengthOut);

// Valid only after dsiArm7PollCompress() returns Done, and only until the
// next dsiArm7BeginCompress() call (which reuses this same buffer).
const void *dsiArm7CompressedData();

// Monotonic microseconds since the current request was sent -- callers
// use this to decide when to stop waiting and fall back to their own ARM9
// path instead (see McRegionChunkLoader.cpp's own timeout constant and
// comment on why a generous bound is correct here, not a sign of trouble).
uint64_t dsiArm7CompressElapsedUs();

#endif // DSI_PLATFORM
