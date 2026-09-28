#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "java/Type.h"

#include "platform/RenderTerrainAPI.h"
#include "platform/PlatformConfig.h"
#include "Tessellator.h"

class World;
class TileEntity;
class ICamera;
class Entity;
class AxisAlignedBB;
#if PLATFORM_PC_LEGACY
struct PcLegacyTerrainStaging;
#endif

// net.minecraft.src.WorldRenderer
class WorldRenderer
{
public:
	WorldRenderer(World *world, std::vector<TileEntity *> *tileEntities, int_t posX, int_t posY, int_t posZ, int_t size, int_t glListId);
	~WorldRenderer();

	void setPosition(int_t x, int_t y, int_t z);
	void updateInFrustrum(ICamera *icamera);
	void updateRenderer();
	void markDirty();
#ifdef WII_PLATFORM
	// Wii mesh-cache eviction. Returns true only if this renderer was holding
	// recorded terrain geometry that has now been released.
	bool releaseDisplayListsForCache();

	// Whether releaseDisplayListsForCache() would free anything, without the
	// side effects of calling it. The eviction scan has to ask before it picks a
	// victim, because it keeps only the farthest candidate and gives up when
	// that one turns out to have nothing to release.
	//
	// Stricter than releaseDisplayListsForCache()'s own guard by the
	// isInitialized term: a renderer that has never been meshed has both
	// _skipRenderPass entries still false from the constructor, so it reads as
	// holding geometry and releasing it only marks it dirty again. That is a
	// rebuild of an empty distant section bought with an eviction slot and no
	// memory returned. setDontDraw() wants that clearing behaviour and keeps
	// calling releaseDisplayListsForCache() directly, so its guard is unchanged.
	bool holdsRecordedTerrain() const;
#endif
#if PLATFORM_PC
	void callOcclusionQueryList();
	int_t getGLCallListForPass(int_t pass);
#endif
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(DSI_PLATFORM)
	void renderExtraTerrainMeshes(int_t pass);
#endif
#ifdef DSI_PLATFORM
	// Draws this section's already-captured terrain mesh for one pass (the
	// generic RenderStaticMesh path -- see platform/RenderStaticMesh.cpp --
	// since the DS 3D engine has neither OpenGL display lists nor a native
	// terrain pipeline like Wii's GX one). Applies the same posXClip/Y/Z
	// chunk-local translate PC bakes into its display list and Wii applies in
	// renderExtraTerrainMeshes(); RenderList::render() (src/dsi/minecraft/
	// RenderList.cpp) has already applied the coarser origin-to-viewer
	// translate around every renderer in its batch, exactly as it does for PC
	// and Wii. Returns false (and draws nothing) when the section is out of
	// frustum, was never built, or this pass has no geometry.
	bool drawCapturedTerrain(int_t pass);
#endif
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(DSI_PLATFORM) || PLATFORM_PC_LEGACY
	bool isTerrainBuildInProgress() const;
#ifdef PS2_PLATFORM
	// Drops an in-flight build and returns its staging lease. The renderer
	// stays dirty and restarts from scratch on a later scheduler step.
	void abandonTerrainBuild();
#endif
	// A dirty mark caused by a light value change. With
	// PLATFORM_COALESCE_MESH_REBUILDS an active build keeps going and is
	// rebuilt once more after it completes, instead of restarting on every
	// frame of a light propagation (a torch is several frames of them).
	void markDirtyFromLighting();
	// Set by RenderGlobal for a block change next to the player; the scheduler
	// runs these ahead of streaming work and to completion.
	bool urgentRebuild = false;
#if PLATFORM_PS2 && MC_LOG_LEVEL >= 2
	// Monotonic microseconds at the edit that set urgentRebuild; the urgent lane
	// logs the edit-to-publish latency against it.
	uint64_t urgentMarkUs = 0;
	// Times an active (partial) build was discarded; the urgent-lane log reads
	// the delta to tell a restarted build from a slow one.
	unsigned int ps2BuildRestarts = 0;
#endif
	bool lastTerrainBuildStepDidWork() const;
#if PLATFORM_PC_LEGACY || PLATFORM_PS2 || PLATFORM_WII || PLATFORM_DSI
	bool hasPublishedTerrain() const { return isInitialized; }
#endif
#if PLATFORM_PC_LEGACY
	std::uint8_t pcLegacyVisibleFacesFrom(int_t face) const;
	bool pcLegacyCpuVisible = true;
#endif
#endif
#ifdef PS2_PLATFORM
	bool terrainSourcesReady() const;
	bool needsRebuildForPublishedChunk(int_t chunkX, int_t chunkZ) const;
	// The renderer whose completed build currently owns the single publish
	// pipeline (face sort + VU0 pack), or null. Another completed section
	// waits at publish until it is released; the urgent lane in
	// RenderGlobal::updateRenderers steps the owner first so an edit is not
	// held behind a streaming section's pack.
	static WorldRenderer *ps2PublishOwnerRenderer();
	std::uint8_t ps2VisibleFacesFrom(int_t face) const;
	bool ps2CpuVisible = true;
#endif
#ifdef WII_PLATFORM
	int_t getTerrainHandleForPass(int_t pass) const;

	// Whether this section's mesh for that pass can contain texels the alpha
	// test rejects, as reported by RenderBlocks while it was built. False means
	// the pass may run with the alpha test off, which is what lets GX reject
	// fragments against Z before texturing them. Conservative: an unbuilt or
	// out-of-range pass answers true.
	bool terrainPassNeedsAlphaTest(int_t pass) const;
#endif

#ifdef PS2_PLATFORM
	void renderPassImmediate(int_t pass);
	void renderPassCached(int_t pass);
	// Interpolated eye position for this terrain pass, in world coordinates. It
	// feeds both face-bucket culling and the section-to-camera translation. The
	// native backend also snapshots projection*modelview here once, before the
	// visible-section loop, so individual chunks never touch the GL matrix stack.
	static void setTerrainViewerPosition(double x, double y, double z);

	// True when the last completed build step actually meshed blocks. False
	// means it returned early on the generation gate (a source chunk is still
	// missing), which costs almost nothing and must not consume a slot of the
	// per-frame meshing budget. See RenderGlobal::updateRenderers.
	// Bytes this renderer holds for mesh storage, counting CAPACITY rather than
	// size: setDontDraw() and the completion swap both deliberately retain
	// capacity to avoid re-allocating on a heap that fragments, so capacity is
	// what the allocator is really keeping out of circulation.
	size_t terrainMeshRamBytes() const;

	// Adds this renderer's four owned buckets to out; does not clear it, so a
	// caller can accumulate over the grid. The summed figure could not answer
	// the question it was added for -- it tracked the whole of the arena's
	// growth without saying whether that was geometry (scales with vertex
	// count) or face-group ranges (scale with atlas fragmentation).
	// RenderTerrainMeshRam::stagingPool is pool-wide and is filled by
	// RenderGlobal, never here.
	void addTerrainMeshRam(RenderTerrainMeshRam &out) const;
	static size_t sharedOpaquePublishScratchRamBytes();

	// Release retained capacity that belongs to a renderer with no published
	// terrain. This never touches an active build or a visible live mesh.
	size_t trimPs2UnusedMeshCapacity();

	// Drop the published mesh of an off-screen renderer selected by RenderGlobal's
	// global mesh budget. The renderer stays at the same world position and is
	// marked dirty so it can rebuild when it becomes relevant again.
	size_t releasePs2PublishedMeshForBudget();
#endif

	bool skipAllRenderPasses();
	bool skipRenderPass(int_t pass);
	float distanceToEntitySquared(Entity *entity);
	void setDontDraw();
	void detachFromWorld();
	void cleanup();

	static int_t chunksUpdated;

	// Public fields matching Java
	World *worldObj;
	bool needsUpdate;
	bool queuedForUpdate = false;
	bool isChunkLit;
#if PLATFORM_PC
	bool isWaitingOnOcclusionQuery;
#endif
	bool isVisible;
	bool isInFrustum;
#if PLATFORM_PC || PLATFORM_PS2
	// Stronger than isInFrustum: PS2 uses it for its clip fast path and desktop
	// Fancy Occlusion uses it to avoid querying boxes that cross a frustum plane.
	bool isFullyInFrustum;
#endif
#if PLATFORM_PC
	bool isVisibleFromPosition;
	double visibleFromX;
	double visibleFromY;
	double visibleFromZ;
	int_t glOcclusionQuery;
#endif
	int_t chunkIndex;

	int_t posX;
	int_t posY;
	int_t posZ;
	int_t sizeWidth;
	int_t sizeHeight;
	int_t sizeDepth;

	// Center positions (posX + size/2) for distance calc
	int_t posXPlus;
	int_t posYPlus;
	int_t posZPlus;

	// Clip positions (i & 0x3ff)
	int_t posXClip;
	int_t posYClip;
	int_t posZClip;

	// Offsets (i - posXClip) — used by RenderGlobal for translation
	int_t posXMinus;
	int_t posYMinus;
	int_t posZMinus;

	AxisAlignedBB *rendererBoundingBox;

	// Tile entities with special renderers collected during updateRenderer()
	std::vector<TileEntity *> tileEntityRenderers;
#if PLATFORM_PC_LEGACY
	std::vector<TileEntity *> pcLegacyStaticTileEntityRenderers;
#endif

private:
#if PLATFORM_PC
	int_t glRenderList;
	bool needsOcclusionBoxUpdate;
	void updateOcclusionBox();
#endif
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(DSI_PLATFORM)
	// OptiFine CTM atlases are kept as backend-neutral captured meshes. The
	// normal terrain mesh remains on /terrain.png; these groups are replayed
	// after it with their own texture binding.
	std::vector<TessellatorTextureMesh> extraTextureMeshes[2];
#endif
#ifdef WII_PLATFORM
	// Native GX geometry uses an opaque handle namespace independent from
	// OpenGL display-list names. Two handles are live and two are staging so a
	// rebuild can publish atomically without touching the currently visible mesh.
	RenderTerrainChunkHandles terrainChunkHandles;

	// Published alongside terrainChunkHandles.live, and staged next to the mesh
	// it describes so the two can never disagree about the same geometry.
	bool wiiPassNeedsAlphaTest[2];
	bool wiiBuildNeedsAlphaTest[2];

	// Incremental Wii mesh build state. CPU-side vertices accumulate over bounded
	// block steps while the currently published GX lists remain visible. Staging
	// lists are compiled only after a whole pass is complete and swapped atomically.
	std::vector<int_t> wiiBuildRawBuffer[2];
	std::vector<TessellatorTextureMesh> wiiBuildExtraTextureMeshes[2];
	int_t wiiBuildVertexCount[2];
	bool wiiBuildHasTexture[2];
	bool wiiBuildHasColor[2];
	bool wiiBuildHasBrightness[2];
	bool wiiBuildDrew[2];
	bool wiiBuildActive;
	// Snapshot of the ChunkCache source columns used by the staging mesh. If a
	// streamed neighbour appears or disappears between incremental steps, the
	// partial mesh must restart instead of mixing both source states.
	unsigned int wiiBuildSourceAvailability;
	bool wiiBuildSourceAvailabilityValid;
	int_t wiiBuildPass;
	int_t wiiBuildCursor;
	bool wiiBuildHasPass1;
	bool wiiBuildChunkLit;
	bool wiiBuildDirtyDuringBuild;
	bool wiiStepDidWork;
	std::vector<TileEntity *> wiiBuildTileEntityRenderers;

	void wiiResetBuildState();
	void wiiBeginBuildState();
	bool wiiBuildRendererStep(int_t blockBudget);
#endif
	bool isInitialized;
	bool _skipRenderPass[2];
#ifdef PS2_PLATFORM
	std::vector<int_t> ps2RawBuffer[2];
	int_t ps2VertexCount[2];
	int_t ps2DrawMode[2];
	bool ps2HasTexture[2];
	bool ps2HasColor[2];
	bool ps2HasNormals[2];

	// Incremental PS2 mesh build state.  Active cache remains visible while the
	// staging buffers are filled over multiple frames.
	//
	// The staging pair is leased from terrain staging pool rather than owned here:
	// only a renderer with a build in progress needs one, and the grid is far
	// larger than the number of concurrent builds. ps2BuildStagingSlot is
	// RENDER_TERRAIN_STAGING_INVALID_SLOT whenever no build is active, and every path
	// that abandons a build must return the slot or terrain streaming stalls
	// for good. ps2BuildBuffers() is null exactly when no slot is held, so a
	// missing lease faults at the use site instead of corrupting another
	// renderer's staging mesh.
	int ps2BuildStagingSlot;
	std::vector<int_t> *ps2BuildBuffers() const;
	std::vector<TessellatorTextureMesh> ps2BuildExtraTextureMeshes[2];
	int_t ps2BuildVertexCount[2];
	int_t ps2BuildDrawMode[2];
	bool ps2BuildHasTexture[2];
	bool ps2BuildHasColor[2];
	bool ps2BuildHasNormals[2];
	bool ps2BuildDrew[2];
	bool ps2BuildActive;
	// Bitset of the 2x2--3x3 ChunkCache source columns that existed when the
	// current staging mesh began. A change invalidates only this active build.
	unsigned int ps2BuildSourceAvailability;
	bool ps2BuildSourceAvailabilityValid;
	unsigned int ps2MissingNeighbourMask;
	int_t ps2BuildPass;
	int_t ps2BuildCursor;
	// Which greedy face direction the opaque pass will sweep next, 0..6.
	// PS2_GREEDY_FACE_COUNT means the greedy stage of this build is finished and
	// the per-block loop owns the remaining budget.
	int_t ps2BuildGreedyFace;
	// First unprocessed plane (0..15) inside ps2BuildGreedyFace. Greedy merging
	// never crosses face planes, so this can time-slice a direction exactly.
	int_t ps2BuildGreedySlice;
	std::vector<TileEntity *> ps2BuildTileEntityRenderers;

	// Atlas-tile / face-direction / spatial-cluster ranges of the live opaque
	// mesh, produced by ps2_mesh_sort_faces at build completion. Only pass 0 is
	// reordered; pass 1 is blended and needs its original back-to-front order.
	RenderTerrainBackendCache ps2TerrainCache;

	// Set during the pass-0 sweep when any block in the section renders in pass
	// 1. When it stays false the pass-1 sweep is skipped entirely instead of
	// walking all 4096 blocks to emit nothing -- which is what the FRAME log's
	// "terrain pass1 withGeom=0" meant it was doing on every section.
	bool ps2BuildHasPass1;
	bool ps2BuildDirtyDuringBuild;
	std::array<std::uint64_t, 64> ps2BuildOpaqueBits;
	int_t ps2BuildOpaqueCount;
	std::uint8_t ps2PublishedVisibility[6];

	// See lastTerrainBuildStepDidWork().
	bool ps2StepDidWork;

	void ps2ResetBuildState();
	// False when the staging pool is full. The caller must not treat the build
	// as started in that case; it retries on a later frame.
	bool ps2BeginBuildState();
	bool ps2BuildRendererStep(int_t blockBudget);
#endif
#ifdef DSI_PLATFORM
	// DSi terrain build state. Incremental like Wii's/PS2's (see
	// PLATFORM_CHUNK_BUILD_BLOCKS_PER_STEP -- DsiWorldTuning.h/PlatformGameTuning.h
	// deliberately fold DSi into PS2's per-frame chunk-build budget, not the
	// desktop's single-shot one), but there is no native GX/VU/VRAM-handle path
	// to publish into: the compiled mesh per pass is the generic captured-RAM
	// RenderStaticMesh (platform/RenderStaticMesh.cpp), the same mechanism
	// RenderGlobal already uses for the sky/star meshes. "Incremental" here
	// means only "resumed across several updateRenderer() calls on the single
	// main thread" -- src/dsi/compat's <thread>/<mutex> shims are not real
	// threads, and nothing here spawns one; this is cooperative resumption
	// exactly like WII_PLATFORM's build state below, not background work.
	//
	// dsiLiveMesh is what drawCapturedTerrain() replays every frame.
	// dsiStagingMesh is compiled per pass as that pass's block loop finishes,
	// but only swapped into dsiLiveMesh once BOTH passes of a build are done
	// (see the end of dsiBuildRendererStep()), so a section either shows its
	// old mesh whole or its new mesh whole -- never a mix of an updated pass 0
	// against a stale pass 1.
	RenderStaticMesh dsiLiveMesh[2];
	RenderStaticMesh dsiStagingMesh[2];

	std::vector<int_t> dsiBuildRawBuffer[2];
	std::vector<TessellatorTextureMesh> dsiBuildExtraTextureMeshes[2];
	int_t dsiBuildVertexCount[2];
	bool dsiBuildHasTexture[2];
	bool dsiBuildHasColor[2];
	bool dsiBuildHasBrightness[2];
	bool dsiBuildDrew[2];
	bool dsiBuildActive;
	// Snapshot of the ChunkCache source columns the active staging mesh was
	// built against -- see the identical field on WII_PLATFORM above for why.
	unsigned int dsiBuildSourceAvailability;
	bool dsiBuildSourceAvailabilityValid;
	int_t dsiBuildPass;
	int_t dsiBuildCursor;
	// Which greedy face direction the opaque pass will sweep next, 0..6.
	// DSI_GREEDY_FACE_COUNT means the greedy stage of this build is finished
	// and the per-block loop owns the remaining budget. Mirrors PS2's
	// ps2BuildGreedyFace/ps2BuildGreedySlice above -- see DsiGreedyMesh.h for
	// why DSi's version has no section-cache/raw-layout counterpart to those.
	int_t dsiBuildGreedyFace;
	// First unprocessed plane (0..15) inside dsiBuildGreedyFace. Greedy merging
	// never crosses face planes, so this can time-slice a direction exactly.
	int_t dsiBuildGreedySlice;
	bool dsiBuildHasPass1;
	bool dsiBuildChunkLit;
	bool dsiBuildDirtyDuringBuild;
	// See lastTerrainBuildStepDidWork().
	bool dsiStepDidWork;
	std::vector<TileEntity *> dsiBuildTileEntityRenderers;

	// Resumable/budgeted repack state, per pass -- see dsiRepackCapturedMeshStep()'s
	// own comment (RenderAPI_DSI.cpp) for the full why. 0 = converting position/
	// texcoord, 1 = compiling the GX FIFO command stream, 2 = done for this
	// build. dsiRepackCursor is the next unprocessed vertex index within the
	// current stage. Plain int, not int_t: pure internal bookkeeping, never a
	// Java-semantic value, and RenderAPI_DSI.cpp (a different translation unit)
	// takes these by reference without pulling in java/Type.h.
	int dsiRepackStage[2];
	int dsiRepackCursor[2];

	void dsiResetBuildState();
	void dsiBeginBuildState();
	bool dsiBuildRendererStep(int_t blockBudget);
#endif

	static void eraseAllTileEntityRefs(std::vector<TileEntity *> *list, TileEntity *te);
	static void pushUniqueTileEntityRef(std::vector<TileEntity *> *list, TileEntity *te);
	void removeTileEntityRenderersFromGlobalList();
#if PLATFORM_PC_LEGACY
	bool pcLegacyBuildActive;
	unsigned int pcLegacyBuildSourceAvailability;
	bool pcLegacyBuildSourceAvailabilityValid;
	int_t pcLegacyBuildPass;
	int_t pcLegacyBuildCursor;
	bool pcLegacyBuildHasPass1;
	bool pcLegacyBuildChunkLit;
	bool pcLegacyStepDidWork;
	bool pcLegacyBuildDirtyDuringBuild;
	std::uint8_t pcLegacyPublishedVisibility[6];

	void pcLegacyResetBuildState();
	bool pcLegacyBeginBuildState();
	bool pcLegacyBuildRendererStep(int_t blockBudget);
	void pcLegacyPublishBuild(PcLegacyTerrainStaging &staging);
#endif
	std::vector<TileEntity *> *tileEntities;
};
