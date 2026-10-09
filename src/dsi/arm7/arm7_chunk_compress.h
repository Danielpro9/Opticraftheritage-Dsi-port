#pragma once

// ARM7-only prototypes for arm7_chunk_compress.c -- see that file and
// DsiArm7ChunkCompress.h for the design. Not included from the ARM9 side.

void dsiArm7ChunkCompressInit(void);
void dsiArm7ChunkCompressPoll(void);
