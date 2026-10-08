#pragma once
#ifdef DSI_PLATFORM

struct RenderCapturedMesh;

// Converts a finished captured mesh's position field from float3 to the GPU's
// native v16 fixed-point format, and its texcoord field to t16 texel-space,
// in place -- plus pre-packs its GX FIFO command stream once both are done.
// Defined in platform/RenderAPI_DSI.cpp (same precedent as dsi/DsiEarlyInit.h's
// dsiTotalTextureVramBytes()/dsiTextureVramBytes(): declared under dsi/,
// implemented where the RenderAPI_DSI-internal state it needs already lives).
// See that definition's own comment for the full why -- WorldRendererDsi.cpp
// is meant to be the only caller, right after a section's mesh finishes
// compiling (dsiStagingMesh, never dsiLiveMesh -- see below for why that
// matters), never on an in-progress build step.
//
// terrainTextureId: the GL texture name this mesh is always drawn against
// (WorldRendererDsi.cpp's drawCapturedTerrain() never rebinds a texture
// itself -- see renderExtraTerrainMeshes()'s own "state assumes terrain.png
// is still bound" comment for why that invariant holds -- so the caller
// already knows this, typically ConnectedTextures::getTerrainTextureId()).
// Texcoord conversion only actually happens if that texture is already
// resident (its width/height known); see RenderCapturedMesh::texCoordIsT16's
// own comment for why this is only safe when the mesh's draw-time texture is
// known fixed in advance, which callers other than the main terrain section
// mesh should NOT assume for themselves.
//
// Budgeted and resumable across several calls, for a caller that cannot
// afford this mesh's full repack cost in one call (WorldRendererDsi.cpp's
// dsiBuildRendererStep(): a section's mesh can hold several thousand
// vertices, each costing multiple soft-float multiplies on this FPU-less
// ARM9, and running all of this unconditionally the instant a build's block
// loop finished was a measured real-hardware "build" spike of up to 172ms in
// one call).
//
// stage/cursor persist a caller-owned resume point across calls (0/0 for a
// mesh that has not started repacking yet): stage 0 converts up to
// vertexBudget vertices' worth of position+texcoord this call, stage 1
// compiles up to vertexBudget vertices' worth of the GX FIFO command stream,
// stage 2 means fully done. Returns true once stage reaches 2 (mesh fully
// repacked, or determined to need no repacking -- e.g. empty), false if there
// is still work left for a future call with the same stage/cursor pair.
// Idempotent and safe to call repeatedly on an already-finished mesh (stage
// already 2): returns true immediately, does nothing.
//
// Only ever safe to call on a mesh nothing draws yet (WorldRendererDsi.cpp
// calls this on dsiStagingMesh, never dsiLiveMesh). positionIsV16/
// texCoordIsT16 are single whole-mesh flags: this function only flips either
// once every vertex in the mesh has actually been converted (cursor reaches
// vertexCount), never partway, so a mesh mid-repack always still reads as
// "not yet converted" to anything that draws it in the meantime -- but a mesh
// being drawn every frame WHILE mid-repack would still be wrong in a subtler
// way (mesh.raw's bytes for already-visited vertices are overwritten with
// v16/t16 ints in place, read back as floats by anything using the
// still-false flags). WorldRendererDsi.cpp is safe because it never publishes
// dsiStagingMesh into dsiLiveMesh (the only mesh drawCapturedTerrain() ever
// reads) until this function has returned true for both passes.
bool dsiRepackCapturedMeshStep(RenderCapturedMesh& mesh, int terrainTextureId, int vertexBudget,
	int& stage, int& cursor);

// The GL texture name RenderAPI_DSI.cpp's own renderBindTexture() most
// recently bound (0 if none yet). Added for callers like GuiIngame.cpp's
// HUD caches that want to repack a just-compiled static mesh against
// "whatever texture the caller already bound before compiling it" instead
// of threading an explicit texture id through their own call chain --
// correct precisely because none of those call sites rebind a texture
// between binding it and compiling the mesh that draws against it, the
// same invariant dsiRepackCapturedMeshStep()'s own terrainTextureId
// parameter already relies on for WorldRendererDsi.cpp's terrain meshes.
int dsiGetBoundTexture();

#endif // DSI_PLATFORM
