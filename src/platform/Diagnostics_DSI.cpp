#include "platform/Diagnostics.h"

#ifdef DSI_PLATFORM

#include "platform/Log.h"
#include "dsi/DsiEarlyInit.h"

#include <malloc.h>

// No last-bad-alloc capture exists yet for DSi (WiiHeap.h's equivalent tracks
// the source line of the allocation that returned null); nothing calls
// platformCaptureBadAlloc() to populate one, so this always reports "none".
const char* platformOomDiagnosticLine(int index)
{
	(void)index;
	return nullptr;
}

void platformMemoryCheckpoint(const char* tag)
{
	// dsiGetHeapCommitted() is sbrk's break point (getHeapEnd() - heapStart):
	// a high-water mark that newlib's allocator never moves back down on
	// free(), so a world that pushed it up and a subsequent "world delete"
	// that frees every Chunk/Entity in that world can leave it exactly where
	// it was -- freed memory goes back onto malloc's own free list for the
	// NEXT allocation to reuse, not back to this counter. Real-hardware
	// evidence (a debug.log comparing world-entry vs. world-exit checkpoints)
	// showed exactly that: "world delete post" bit-for-bit equal to "world
	// delete pre", committed ~1.7MB above where the same world started,
	// reported by the player as "RAM stays at 9MB after exiting" -- which
	// this number alone cannot tell apart from a real leak (reachable memory
	// nothing ever frees). mallinfo() can: uordblks is bytes actually handed
	// out and not yet freed right now (same pattern already used on PS2/Wii's
	// equivalent diagnostics, Runtime_ps2.cpp/WiiEarlyMemory.cpp), independent
	// of where sbrk's break happens to sit. If uordblks drops back down after
	// a world unloads while committed stays flat, the committed number was
	// always just this harmless watermark; if uordblks stays elevated too,
	// something genuinely reachable is being kept alive past world exit.
	const struct mallinfo mi = mallinfo();
	MC_LOG_INFO("dsi", "heap checkpoint (%s): %u KB committed / %u KB ceiling, malloc used=%uKB free=%uKB (%u blocks)\n",
	            tag ? tag : "?",
	            (unsigned)(dsiGetHeapCommitted() / 1024u),
	            (unsigned)(dsiGetHeapCeiling() / 1024u),
	            (unsigned)(mi.uordblks / 1024),
	            (unsigned)(mi.fordblks / 1024),
	            (unsigned)mi.ordblks);
}

void platformHardwareCheckpoint(const char* tag)
{
	(void)tag;
}

long platformHeapFreeKb()
{
	const long ceiling = (long)(dsiGetHeapCeiling() / 1024u);
	const long committed = (long)(dsiGetHeapCommitted() / 1024u);
	return ceiling - committed;
}

void platformCaptureBadAlloc()
{
	// See platformOomDiagnosticLine() above -- nothing to capture into yet.
}

#endif // DSI_PLATFORM
