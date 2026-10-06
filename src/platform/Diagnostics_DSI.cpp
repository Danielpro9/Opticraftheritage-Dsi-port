#include "platform/Diagnostics.h"

#ifdef DSI_PLATFORM

#include "platform/Log.h"
#include "dsi/DsiEarlyInit.h"

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
	MC_LOG_INFO("dsi", "heap checkpoint (%s): %u KB committed / %u KB ceiling\n",
	            tag ? tag : "?",
	            (unsigned)(dsiGetHeapCommitted() / 1024u),
	            (unsigned)(dsiGetHeapCeiling() / 1024u));
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
