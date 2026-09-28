#pragma once

#ifdef DSI_PLATFORM

#include <vector>
#include "java/Type.h"

class ChunkCache;

// Post-process merge for DSi's terrain build: scans one incremental build
// step's just-captured translucent-pass raw vertex buffer (platform/
// RenderAPI.h's RenderCapturedMesh interleaved layout -- 8 int_t/32-byte
// stride per vertex: x,y,z,u,v,colour,<unused>,brightness, see RenderAPI.h's
// texCoordOffset=12/colorOffset=20/brightnessOffset=28) for consecutive,
// coplanar, identically shaded still-water TOP-face quads (the ones
// RenderBlocks::renderBlockFluids emits for Block::waterStill, metadata 0)
// and merges horizontally/vertically-adjacent pairs into 2x1/1x2/2x2 quads in
// place, shrinking the buffer. Returns the number of quads removed.
//
// Ported from TheBrokenWaLL's PS2 fork (commits fafc028/06c17aa,
// src/ps2/render/Ps2WaterSurfaceMerge.h's ps2MergeWaterTops), adapted to
// DSi's 8-word vertex stride (PS2 packs brightness into the colour word via
// ps2_lighting_apply_packed_brightness; DSi keeps a separate brightness
// slot, so eligibility/merge-run continuation also compares brightness, not
// just colour and position) and, like DsiGreedyMesh.cpp, without UV scaling
// by width/depth -- the DS 3D engine has no hardware region-repeat to tile a
// scaled UV span against, so a merged quad's geometry stretches across the
// merged area while its UV stays the single source tile's (see
// DsiGreedyMesh.h's header comment for the full reasoning; this reuses that
// same file's DSI_GREEDY_MAX_MERGE=2 cap to bound the stretch).
//
// Still water's top-face UV happens to reduce to the plain per-tile rect
// (traced through RenderBlocks::renderBlockFluids's flow-rotation math by
// hand: rotU/rotV cancel out to zero net rotation once f12 -- the flow
// direction -- is forced to 0 for a still, non-flowing source block), so the
// same corner pattern DsiGreedyMesh.cpp's emitQuad uses (dx,dz =
// {(0,0),(0,1),(1,1),(1,0)}) applies here too; this is NOT re-derived from
// scratch here, it is read back off whatever renderBlockFluids already wrote,
// the same "verify against the real emitted geometry" approach the pixel
// diagnostics elsewhere in this port use, since a wrong assumption here would
// silently just find nothing to merge (see mergedQuads below) rather than
// corrupt anything -- must run on the still-float raw buffer BEFORE
// RenderAPI_DSI.cpp's dsiRepackCapturedMeshStep() converts position/texcoord
// to the DS GPU's native fixed-point formats in place.
//
// originX/Y/Z: this build step's section origin in world block coordinates
// (WorldRenderer::posX/posY/posZ) -- the raw buffer's positions are already
// section-local (Tessellator::setTranslationD() applied before capture), so
// this is added back on to re-query ChunkCache at world coordinates, the same
// safety gate ps2MergeWaterTops uses (geometry+UV pattern alone is not
// proof two adjacent quads are really still water at the same level; only
// the block/metadata check is).
unsigned dsi_merge_water_top_pairs(std::vector<std::int32_t> &raw, int_t tile,
                                    ChunkCache &cc,
                                    int_t originX, int_t originY, int_t originZ);

#endif // DSI_PLATFORM
