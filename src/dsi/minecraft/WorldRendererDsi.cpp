// WorldRenderer::updateRenderer() for DSi.
//
// The shared implementation in net/minecraft/src/WorldRenderer.cpp excludes
// DSI_PLATFORM (see its own #if guard) because it records a chunk's mesh into
// an OpenGL display list (renderBeginDisplayList/renderEndDisplayList), and
// the DS 3D engine has no such concept -- RenderAPI_DSI.cpp does not, and
// cannot, implement those two calls. src/wii/minecraft/WorldRendererWii.cpp
// and src/ps2/minecraft/WorldRendererPs2.cpp are excluded from that same
// function for an analogous reason (native GX display lists / VU0 packed
// meshes), and each provides its own updateRenderer() instead. This file is
// DSi's counterpart.
//
// Chosen mesh path: platform/RenderStaticMesh.cpp's generic captured-mesh
// mechanism (RenderStaticMesh + renderStaticMeshCompile/Draw), the same one
// RenderGlobal.cpp already uses for the sky/star meshes (skyMesh, skyMesh2,
// starMesh) and GuiIngame.cpp uses for the hotbar/crosshair/status HUD
// caches. It captures a Tessellator batch into a plain RenderCapturedMesh
// (position/texcoord/color/brightness interleaved, exactly like Wii's
// non-native fallback and PS2's own captured path) and replays it with
// immediate-mode calls every frame -- see RenderAPI_DSI.cpp's
// renderCaptureInterleaved()/renderDrawCaptured(). PLATFORM_PERSISTENT_RENDER_
// MESH is PLATFORM_WII only (PlatformConfig.h), so on DSi renderStaticMeshCompile/
// Draw always take that captured-replay branch, never a native persistent
// handle -- there is no "DSi native terrain pipeline" to bypass here at all.
//
// Incremental build, not single-shot: DsiWorldTuning.h / PlatformGameTuning.h
// fold DSi into PS2's per-frame chunk-build budget (PLATFORM_CHUNK_BUILD_
// BLOCKS_PER_STEP, PLATFORM_CHUNK_BUILD_BUDGET_MS, PLATFORM_COALESCE_MESH_
// REBUILDS, PLATFORM_MESH_BUDGET's scheduler in RenderGlobal::updateRenderers)
// rather than the desktop/PC_LEGACY single-call one. "Incremental" here means
// only "resumed across several updateRenderer() calls on the single main
// thread", the same cooperative-resumption meaning WII_PLATFORM's build state
// machine gives it below -- DSi genuinely has no working std::thread (see
// src/dsi/compat's shims), and nothing in this build is a background job.
// This file's structure mirrors src/wii/minecraft/WorldRendererWii.cpp
// closely for that reason (the simpler of the two incremental builders).
//
// PS2's greedy face merging IS reproduced here (dsiBuildRendererStep()'s
// PLATFORM_ENABLE_GREEDY_MESH-guarded block, forwarding through
// platform/RenderTerrainAPI.h to src/dsi/render/DsiGreedyMesh.cpp -- a port of
// src/ps2/render/Ps2GreedyMesh.cpp with one deliberate deviation, covered in
// that file's own header comment: DSi's version does not scale a merged
// quad's UV span by width/height, because the DS 3D engine has no hardware
// region-repeat wrap to tile a scaled span against the way the PS2 GS does.
// PS2's VU0 face-sorted publish is still not reproduced -- that is a
// GS/VU-specific optimisation with no DS 3D engine equivalent.
//
// Deliberately NOT reproduced from PS2/Wii (documented simplifications, not
// oversights):
//   * Wii's WiiBlockRenderInfo / renderSimpleOpaqueCubeWii fast opaque-cube
//     path, and PS2's matching PLATFORM_FAST_SIMPLE_CUBE_RENDER branch: both
//     replace renderBlockByRenderType() with a direct per-face emitter driven
//     by an exposed-face mask, which only pays off with a per-face-direction
//     replay to hand that mask's output to at draw time (PLATFORM_NATIVE_
//     TERRAIN_PIPELINE, Wii-only / PS2's VU1 path). This file's generic
//     RenderStaticMesh path has no such replay, so porting the fast emitter
//     here would only reorder which code builds the same quads, not skip any
//     work. PLATFORM_MESH_FACE_SORT is the same story: reordering a finished
//     mesh's vertices has nothing to exploit without a face-sorted replay
//     either, so it stays unused here too. A real profile once this runs on
//     hardware is what should decide whether either is worth porting on top
//     of greedy meshing.
//   * PLATFORM_SKIP_ENCLOSED_OPAQUE_CUBES, unlike the two above, does not
//     need a native replay -- it is a plain "don't call
//     renderBlockByRenderType() at all" pre-check, so dsiBuildRendererStep()
//     below DOES use it (dsiFullyEnclosedOpaqueCube()), independent of
//     whether the greedy pass is active: the greedy pass already skips its
//     own eligible blocks' buried interiors via its own per-face neighbour
//     check, but grass (excluded from greedy for its tint path) and every
//     simpleOpaqueCube block whenever Config::isConnectedTextures()/
//     isNaturalTextures() force dsiAllowGreedyMesh off still went through the
//     full per-block render path with no fast reject at all before this.
//   * Wii's alpha-test-aware early-depth batching in RenderList (submitting
//     opaque-and-not-alpha-tested sections before the rest so GX can reject
//     fragments before texturing). src/dsi/minecraft/RenderList.cpp submits
//     everything in one pass; it is a fill-rate optimisation, not a
//     correctness requirement, and the DS 3D engine's fixed-function
//     rasteriser/alpha-test path was not the part this port has verified yet.
//   * PS2's shared terrain-staging pool (RenderTerrainStaging.h/
//     Ps2MeshStagingPool). renderTerrainStagingHasFreeSlot() and friends
//     already default to "no pool, always free" outside PLATFORM_PS2 (see
//     RenderTerrainStaging.cpp), which is exactly Wii's behaviour too: each
//     renderer owns its own staging buffers directly, as this file does.
#ifdef DSI_PLATFORM

#include "net/minecraft/src/WorldRenderer.h"
#include "java/Arithmetic.h"

#include "platform/RenderAPI.h"
#include "platform/PlatformTuning.h"
#include "platform/PlatformCompat.h"
#include "net/minecraft/src/World.h"
#include "net/minecraft/src/ConnectedTextures.h"
#include "net/minecraft/src/Block.h"
#include "net/minecraft/src/RenderBlocks.h"
#include "net/minecraft/src/Tessellator.h"
#include "net/minecraft/src/Chunk.h"
#include "net/minecraft/src/ChunkCache.h"
#include "net/minecraft/src/ExtendedBlockStorage.h"
#include "net/minecraft/src/TileEntity.h"
#include "net/minecraft/src/TileEntityRenderer.h"
#include "net/minecraft/src/Config.h"
#include "net/minecraft/src/EntityLiving.h"
#include "client/Minecraft.h"
#include "platform/RenderTerrainAPI.h"
#include "dsi/minecraft/DsiCapturedMeshRepack.h"
#include "dsi/render/DsiBlockRenderInfo.h"
#include "dsi/render/DsiWaterMerge.h"
#include "dsi/DsiEarlyInit.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace
{
	// How many source ChunkCache columns (of the up to 3x3 a section's 1-block
	// margin can touch) this renderer will request per incremental step.
	// PLATFORM_WII_RENDERER_DEPENDENCY_REQUESTS_PER_STEP is Wii's own tuned
	// value (3); no PLATFORM_-generic equivalent exists (PS2's builder does not
	// have this section at all -- see src/ps2/minecraft/WorldRendererPs2.cpp),
	// so this follows PC_LEGACY_RENDERER_DEPENDENCY_REQUESTS_PER_STEP's lead
	// (pc/tuning/PcLegacyTuning.h) and picks the most conservative value, 1:
	// DSi's per-frame budget is the tightest of any platform this engine
	// targets, and a missing chunk here does not block drawing (the section
	// simply stays on its previous mesh and retries next step).
	constexpr int_t kDsiRendererDependencyRequestsPerStep = 1;

#if PLATFORM_SKIP_ENCLOSED_OPAQUE_CUBES
	// Mirrors WorldRendererPs2.cpp's ps2OpaqueNeighbour/ps2ExposedCubeFaceMask
	// (and DsiGreedyMesh.cpp's own identical isOpaque/isOpaqueBlockId pair) --
	// kept as its own local copy rather than a shared helper, matching how PS2
	// already keeps its version file-local. A position outside a resident
	// chunk column counts as opaque, same reasoning as both of those: a
	// section at the edge of loaded terrain must never treat an unstreamed
	// neighbour as "exposed" and draw a face into it.
	static bool dsiOpaqueNeighbour(ChunkCache &cache, int_t x, int_t y, int_t z)
	{
		if (y >= 0 && y < Chunk::WORLD_HEIGHT && !cache.hasResidentChunkAtBlock(x, z))
			return true;

		const int_t id = cache.getBlockId(x, y, z);
		if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE)
			return false;

		Block *block = Block::blocksList[id];
		if (block == nullptr)
			return false;
		if (Block::staticOpaqueCubeLookupSafe[id])
			return Block::opaqueCubeLookup[id];
		return block->isOpaqueCube();
	}

	// True only if all six neighbours are opaque, i.e. this cube can never
	// contribute a visible face -- renderBlockByRenderType() would tessellate
	// all six sides just to have RenderBlocks' own per-face visibility check
	// discard every one of them. Checked only for simpleOpaqueCube blocks
	// (plain full cubes with default face culling): the same shape of block
	// DsiGreedyMesh.cpp's greedy pass already eligibility-gates on, so this
	// reuses that same safe, narrow precondition rather than a new one.
	static bool dsiFullyEnclosedOpaqueCube(ChunkCache &cache, int_t x, int_t y, int_t z)
	{
		return dsiOpaqueNeighbour(cache, x, y - 1, z) && dsiOpaqueNeighbour(cache, x, y + 1, z) &&
			dsiOpaqueNeighbour(cache, x, y, z - 1) && dsiOpaqueNeighbour(cache, x, y, z + 1) &&
			dsiOpaqueNeighbour(cache, x - 1, y, z) && dsiOpaqueNeighbour(cache, x + 1, y, z);
	}
#endif

	// See dsiGetTotalRendererRebuilds()'s own comment (DsiEarlyInit.h).
	unsigned int g_dsiTotalRendererRebuilds = 0u;
	// See dsiGetTotalBuildRestarts()'s own comment (DsiEarlyInit.h).
	unsigned int g_dsiTotalBuildRestarts = 0u;
}

unsigned int dsiGetTotalRendererRebuilds()
{
	return g_dsiTotalRendererRebuilds;
}

unsigned int dsiGetTotalBuildRestarts()
{
	return g_dsiTotalBuildRestarts;
}

void dsiRecordBuildRestart()
{
	++g_dsiTotalBuildRestarts;
}

void WorldRenderer::updateRenderer()
{
	if (!needsUpdate)
		return;

	dsiBuildRendererStep(PLATFORM_CHUNK_BUILD_BLOCKS_PER_STEP);
}

void WorldRenderer::dsiResetBuildState()
{
	dsiBuildActive = false;
	dsiBuildSourceAvailability = 0u;
	dsiBuildSourceAvailabilityValid = false;
	dsiBuildPass = 0;
	dsiBuildCursor = 0;
	dsiBuildGreedyFace = 0;
	dsiBuildGreedySlice = 0;
	dsiBuildHasPass1 = false;
	dsiBuildChunkLit = false;
	dsiBuildDirtyDuringBuild = false;
	dsiStepDidWork = false;
	dsiBuildTileEntityRenderers.clear();
	for (int_t p = 0; p < 2; ++p)
	{
		std::vector<int_t>().swap(dsiBuildRawBuffer[p]);
		dsiBuildVertexCount[p] = 0;
		dsiBuildHasTexture[p] = false;
		dsiBuildHasColor[p] = false;
		dsiBuildHasBrightness[p] = false;
		dsiBuildDrew[p] = false;
		dsiBuildExtraTextureMeshes[p].clear();
		// A build being abandoned/restarted only ever has a half-finished
		// staging mesh, never a published live one -- dsiLiveMesh is untouched
		// here, exactly like Wii's terrainChunkHandlesClearStaging() leaves
		// the live GX handles alone on a restart.
		renderStaticMeshDestroy(dsiStagingMesh[p]);
		dsiRepackStage[p] = 0;
		dsiRepackCursor[p] = 0;
	}
}

void WorldRenderer::dsiBeginBuildState()
{
	dsiResetBuildState();
	dsiBuildActive = true;
}

bool WorldRenderer::isTerrainBuildInProgress() const
{
	return dsiBuildActive;
}

bool WorldRenderer::lastTerrainBuildStepDidWork() const
{
	return dsiStepDidWork;
}

bool WorldRenderer::dsiBuildRendererStep(int_t blockBudget)
{
	dsiStepDidWork = false;
	if (worldObj == nullptr)
	{
		dsiResetBuildState();
		needsUpdate = false;
		return true;
	}

	const int_t x0 = posX;
	const int_t y0 = posY;
	const int_t z0 = posZ;
	const int_t x1 = posX + sizeWidth;
	const int_t y1 = posY + sizeHeight;
	const int_t z1 = posZ + sizeDepth;

	// Do not bake temporary air into a partial mesh. Snapshot every source
	// column ChunkCache can sample so a streamed neighbour appearing or
	// disappearing between two bounded steps restarts only this staging mesh
	// instead of mixing two source states -- see the identical comment on
	// WII_PLATFORM's wiiBuildRendererStep() this mirrors.
	unsigned int sourceAvailability = 0u;
	{
		const int_t ccx0 = JavaArithmetic::intShr(x0 - 1, 4);
		const int_t ccx1 = JavaArithmetic::intShr(x1 + 1, 4);
		const int_t ccz0 = JavaArithmetic::intShr(z0 - 1, 4);
		const int_t ccz1 = JavaArithmetic::intShr(z1 + 1, 4);
		unsigned int sourceBit = 1u;
		for (int_t ccx = ccx0; ccx <= ccx1; ++ccx)
		{
			for (int_t ccz = ccz0; ccz <= ccz1; ++ccz, sourceBit <<= 1)
			{
				if (worldObj->chunkExists(ccx, ccz))
					sourceAvailability |= sourceBit;
			}
		}

		if (dsiBuildSourceAvailabilityValid &&
			dsiBuildSourceAvailability != sourceAvailability)
		{
			dsiBeginBuildState();
			dsiBuildSourceAvailability = sourceAvailability;
			dsiBuildSourceAvailabilityValid = true;
			return false;
		}

		int_t requestedDependencies = 0;
		sourceBit = 1u;
		for (int_t ccx = ccx0; ccx <= ccx1; ++ccx)
		{
			for (int_t ccz = ccz0; ccz <= ccz1; ++ccz, sourceBit <<= 1)
			{
				if ((sourceAvailability & sourceBit) != 0u ||
					!worldObj->isChunkInLoadRadius(ccx, ccz))
					continue;

				worldObj->getChunkFromChunkCoords(ccx, ccz);
				++requestedDependencies;
				if (requestedDependencies >= kDsiRendererDependencyRequestsPerStep)
					return false;
			}
		}
		if (requestedDependencies > 0)
			return false;
	}

	if (!dsiBuildActive)
	{
		dsiBeginBuildState();
		dsiBuildSourceAvailability = sourceAvailability;
		dsiBuildSourceAvailabilityValid = true;
	}

	const int_t totalBlocks = sizeWidth * sizeHeight * sizeDepth;
	if (blockBudget <= 0)
		blockBudget = totalBlocks;

	// Decorative-only distance cull: grass/flowers/crops etc. (the purely
	// decorative crossed-quad render types -- never a full face a neighbour
	// needs for its own occlusion) are hard to make out once DSi's short-
	// range fog has mostly obscured them anyway (EntityRenderer.cpp's
	// PLATFORM_CONSOLE_LOW formula puts fogStart at ~55% of the loaded
	// edge -- see DsiWorldTuning.h's DSI_DECORATIVE_CULL_DISTANCE_SQ), so
	// this skips baking their geometry into the mesh near that edge instead
	// of building faces nobody can really see. Computed once per build step
	// (not per block): cheap, and a build step's geometry decisions only
	// need to be approximately right, not synced to the sub-tick
	// interpolated eye position an actual frame's draw uses.
	//
	// Same gating as RenderGlobal.cpp's dsiSectionBeyondFog() call (isFogOff/
	// isNether), and a smaller threshold than that function's own full
	// render-edge distance: dsiSectionBeyondFog() already drops a section's
	// ENTIRE mesh once nothing in it could possibly show; this only thins
	// what a still-submitted section bakes, well before that point, for the
	// specific shapes fog hides first. Solid blocks (every other render
	// type) are never skipped by this.
	bool dsiCullDecorationByFog = false;
	float dsiDecorEyeX = 0.0f, dsiDecorEyeY = 0.0f, dsiDecorEyeZ = 0.0f;
	{
		Minecraft *dsiMc = Minecraft::getMinecraft();
		if (dsiMc != nullptr && dsiMc->renderViewEntity != nullptr && dsiMc->theWorld != nullptr &&
			dsiMc->theWorld->worldProvider != nullptr && !dsiMc->theWorld->worldProvider->isNether &&
			!Config::isFogOff())
		{
			EntityLiving *dsiDecorViewEntity = dsiMc->renderViewEntity;
			dsiCullDecorationByFog = true;
			dsiDecorEyeX = static_cast<float>(dsiDecorViewEntity->posX);
			dsiDecorEyeY = static_cast<float>(dsiDecorViewEntity->posY + dsiDecorViewEntity->getEyeHeight());
			dsiDecorEyeZ = static_cast<float>(dsiDecorViewEntity->posZ);
		}
	}

	const uint64_t stepStartUs = PlatformCompat::getMonotonicMicros();
	int_t processed = 0;

	while (dsiBuildPass < 2)
	{
		if (dsiBuildPass == 1 && !dsiBuildHasPass1)
		{
			dsiBuildPass = 2;
			dsiBuildCursor = 0;
			break;
		}

		Chunk::isLit = false;
		ChunkCache chunkcache(worldObj, x0 - 1, y0 - 1, z0 - 1, x1 + 1, y1 + 1, z1 + 1);
		RenderBlocks renderblocks(&chunkcache);
		Tessellator *tessellator = &Tessellator::instance;
		tessellator->startDrawingQuads();
		tessellator->setTranslationD(-(double)posX, -(double)posY, -(double)posZ);

		bool stepDrew = false;
#if PLATFORM_ENABLE_GREEDY_MESH
		// Greedy pass: a bounded run of independent planes from one face
		// direction per build step, not all six directions at once -- mirrors
		// WorldRendererPs2.cpp's own identically-shaped guard (see its comment
		// there on why: the whole-section-in-one-step version is what produced
		// PS2's original build-time spikes). A plane lies on exactly one face
		// direction's slice, so planes can never merge across directions, which
		// is what makes slicing safe to pause/resume without ever re-emitting
		// or skipping a merge. DsiGreedyMesh.cpp's own header comment covers
		// the one deviation from PS2's version (merged-quad UV does not scale
		// with width/height -- no DS hardware region-repeat to tile against).
		//
		// Config::isConnectedTextures()/isNaturalTextures() mirror PS2's own
		// allowOptiFineGreedyMesh guard: both features vary a block's texture
		// per-neighbour/per-position, which the greedy pass's FaceKey (one
		// texture id per merged run) cannot represent.
		// DSI_GREEDY_MESH_RUNTIME_ENABLED (DsiWorldTuning.h): emergency kill
		// switch. The first real-hardware run with the greedy path actually
		// reachable (isConnectedTextures() used to default to true on DSi,
		// which forced dsiAllowGreedyMesh off below -- now fixed) hit a
		// catastrophic steady-state regression (~4-5 fps). That turned out
		// to be a build-budget accounting bug in this file, not the greedy
		// algorithm; see the macro's own comment in DsiWorldTuning.h for the
		// full account. Fixed and re-enabled (currently 1) -- still worth
		// re-reading that comment before assuming this path is at fault if
		// a similar regression ever shows up again. Independent of the
		// (correct, staying fixed) isConnectedTextures()/isNaturalTextures()
		// checks below.
		const bool dsiAllowGreedyMesh = DSI_GREEDY_MESH_RUNTIME_ENABLED &&
			!Config::isConnectedTextures() && !Config::isNaturalTextures();
		if (dsiAllowGreedyMesh && dsiBuildPass == 0 && dsiBuildGreedyFace < RENDER_TERRAIN_GREEDY_FACE_COUNT)
		{
			// Real-hardware evidence (memtrend's rebuilds= counter climbing
			// tens of times a second while chunks/entities/heap stayed
			// completely flat): a fixed one-slice-batch-per-call budget meant
			// the whole 6-face greedy phase needed dozens of separate
			// dsiBuildRendererStep() calls -- dozens of FRAMES -- to finish
			// for one section. dsiResetBuildState() discards a build's entire
			// progress, greedy phase included, the moment anything marks the
			// section dirty during that window (fluid flow, a scheduled
			// tick, anything -- completely ordinary, ever-present world
			// activity). Stretching the greedy phase across that many frames
			// made hitting that window on every single attempt close to
			// certain, so sections never finished: an interrupted restart
			// discards even the greedy geometry already computed and starts
			// the whole phase over, forever.
			//
			// Fix: process several slice-batches back-to-back within THIS
			// call instead of unconditionally yielding after exactly one.
			// PLATFORM_CHUNK_BUILD_STEP_US (the elapsed-time budget the
			// per-block loop below checks) is 0 or DSi -- inherited from PS2
			// unmodified, confirmed by grep, meaning that check is already a
			// permanent no-op here and was never actually bounding per-call
			// cost on this platform to begin with -- so this uses a fixed
			// batch-count bound instead of leaning on a mechanism that would
			// not fire. DSI_GREEDY_BATCHES_PER_CALL batches (each
			// DSI_GREEDY_SLICES_PER_STEP slices) run per call, cutting the
			// number of frames a section's greedy phase needs to finish by
			// the same factor -- shrinking the dirty-during-build interrupt
			// window that was causing the restart loop -- without going all
			// the way to "the whole 6-face phase in one call", which risks
			// reintroducing the single-frame spike slicing existed to avoid
			// in the first place (see Ps2GreedyMesh.h's own history of that
			// exact problem on PS2, whose CPU has no FPU gap to DSi's at
			// all). Picking the right multiplier without a real per-step
			// timing measurement is still a judgement call; this errs
			// conservative over "finish instantly, risk a new stutter".
			int_t dsiGreedyBatchesThisCall = 0;
			const int_t dsiGreedyBatchLimit = DSI_GREEDY_BATCHES_PER_CALL < 1 ? 1 : DSI_GREEDY_BATCHES_PER_CALL;
			while (dsiBuildGreedyFace < RENDER_TERRAIN_GREEDY_FACE_COUNT &&
				dsiGreedyBatchesThisCall < dsiGreedyBatchLimit)
			{
				++dsiGreedyBatchesThisCall;
				const int_t slicesPerStep = DSI_GREEDY_SLICES_PER_STEP < 1 ? 1 :
					(DSI_GREEDY_SLICES_PER_STEP > 16 ? 16 : DSI_GREEDY_SLICES_PER_STEP);
				const int_t sliceBegin = dsiBuildGreedySlice;
				int_t sliceEnd = sliceBegin + slicesPerStep;
				if (sliceEnd > 16)
					sliceEnd = 16;

				int_t greedyX0 = x0, greedyY0 = y0, greedyZ0 = z0;
				int_t greedyX1 = x1, greedyY1 = y1, greedyZ1 = z1;
				if (dsiBuildGreedyFace <= 1)
				{
					greedyY0 = y0 + sliceBegin;
					greedyY1 = y0 + sliceEnd;
				}
				else if (dsiBuildGreedyFace <= 3)
				{
					greedyZ0 = z0 + sliceBegin;
					greedyZ1 = z0 + sliceEnd;
				}
				else
				{
					greedyX0 = x0 + sliceBegin;
					greedyX1 = x0 + sliceEnd;
				}

				stepDrew |= renderTerrainGreedyMeshFace(chunkcache, (int)dsiBuildGreedyFace,
					(int)greedyX0, (int)greedyY0, (int)greedyZ0, (int)greedyX1, (int)greedyY1, (int)greedyZ1);

				dsiBuildGreedySlice = sliceEnd;
				if (dsiBuildGreedySlice >= 16)
				{
					dsiBuildGreedySlice = 0;
					dsiBuildGreedyFace++;
				}
			}

			// Real root cause of the catastrophic steady-state regression this
			// emergency-disabled path was blamed for (see
			// DSI_GREEDY_MESH_RUNTIME_ENABLED's own comment in
			// DsiWorldTuning.h): `processed` is what RenderGlobal.cpp's
			// MeshBudget::run() uses (via lastTerrainBuildStepDidWork(), set
			// from dsiStepDidWork below, which is `processed > 0`) to decide
			// whether this call's real measured wall-clock cost counts toward
			// the per-frame build budget (PLATFORM_CHUNK_BUILD_BUDGET_MS,
			// ~6ms) and the per-frame update-count cap (~10). The greedy
			// branch above does real, non-trivial work -- up to
			// DSI_GREEDY_BATCHES_PER_CALL batches of DSI_GREEDY_SLICES_PER_STEP
			// 16-wide mask slices each, every cell running several block-
			// registry/texture/brightness lookups -- but never touched
			// `processed`, so every one of those calls looked like "did
			// nothing" to the scheduler. Nothing ever capped how many
			// sections' worth of greedy work could run in one frame: up to
			// PLATFORM_RENDERER_UPDATE_CANDIDATES_PER_FRAME (32) candidates
			// could each get a full, uncapped greedy call the same frame,
			// which is exactly the "render time pinned at a scene-independent
			// constant, rebuilds frozen" signature the incident report
			// describes -- not a real per-vertex replay cost regression.
			// Charging the batch count actually run restores the same
			// accounting the per-block loop's `++processed` above already
			// gives the scheduler, so the existing 6ms/10-update caps apply
			// to greedy calls too.
			processed += dsiGreedyBatchesThisCall;
		}
		else
#endif
		{
			// Fast reject for a section with no blocks at all. Every position
			// the loop below would visit calls chunkcache.getBlockId() and,
			// for a fully-air section, always finds id<=0 -- a no-op per the
			// `if (id > 0)` guard a few lines down -- so the outcome is
			// already identical to running the loop; this only reaches that
			// same outcome without paying for 4096 chunk-cache lookups per
			// pass. blockRefCount (ExtendedBlockStorage::getIsEmpty(), also
			// how Chunk's own random-tick scan already skips empty sections,
			// see World.cpp) is already maintained on every block set/clear,
			// so this is a cheap existing counter, not new bookkeeping.
			//
			// Pass 1 is not checked here: it only ever starts at all when
			// some pass-0 block already set dsiBuildHasPass1 above (see the
			// bail a few lines up this same while(dsiBuildPass<2) loop), which
			// by construction means this section is not empty by the time
			// pass 1 runs -- so the check would never fire there anyway.
			//
			// processed must still advance by the full skipped span, not just
			// a token amount: dsiStepDidWork (lastTerrainBuildStepDidWork(),
			// `processed > 0`) only needs it to be positive, but MeshBudget's
			// caller-side accounting treats a call that reports no processed
			// work as regression bait -- see 588e5b6's history on exactly
			// that class of bug (the greedy branch above was once fixed for
			// the identical mistake). An empty section still has to report
			// a full pass worth of "work" this call so the scheduler does not
			// misread it as spinning without progress.
			bool sectionConfirmedEmpty = false;
			if (dsiBuildPass == 0)
			{
				Chunk *selfChunk = worldObj->getChunkFromChunkCoords(
					JavaArithmetic::intShr(x0, 4), JavaArithmetic::intShr(z0, 4));
				const ExtendedBlockStorage *selfSection = selfChunk != nullptr
					? selfChunk->getBlockStorage(JavaArithmetic::intShr(y0, 4))
					: nullptr;
				sectionConfirmedEmpty = selfSection == nullptr || selfSection->getIsEmpty();
			}

			if (sectionConfirmedEmpty)
			{
				processed += totalBlocks - dsiBuildCursor;
				dsiBuildCursor = totalBlocks;
			}
			else
			while (dsiBuildCursor < totalBlocks && processed < blockBudget)
			{
				const int_t cursor = dsiBuildCursor++;
				// sizeWidth/sizeDepth are always exactly 16: RenderGlobal.cpp's
				// only WorldRenderer constructor call hardcodes size=16, and
				// sizeWidth/sizeHeight/sizeDepth are all set from that single
				// parameter (WorldRenderer.cpp). The ARM946E-S has no hardware
				// integer divide, so cursor % sizeWidth / cursor / sizeWidth
				// each compiled to a software-division library call here --
				// this loop's own cursor decomposition, run up to 4096 times
				// per pass. & 15 / >> 4 are the same result for a divisor
				// that is always exactly 16, at a single shift/mask each.
				const int_t lx = cursor & 15;
				const int_t yz = cursor >> 4;
				const int_t lz = yz & 15;
				const int_t ly = yz >> 4;
				const int_t x = x0 + lx;
				const int_t y = y0 + ly;
				const int_t z = z0 + lz;
				++processed;

				const int_t id = chunkcache.getBlockId(x, y, z);
				if (id > 0)
				{
					Block *block = Block::blocksList[id];
					if (block == nullptr)
						continue;

#if PLATFORM_ENABLE_GREEDY_MESH
					// Already emitted by the greedy pass above for the opaque pass.
					if (dsiAllowGreedyMesh && dsiBuildPass == 0 && renderTerrainIsGreedyCube(block))
						continue;
#endif

					if (dsiBuildPass == 0 && Block::isBlockContainer[id])
					{
						TileEntity *te = chunkcache.getBlockTileEntity(x, y, z);
						if (te != nullptr && TileEntityRenderer::instance.hasSpecialRenderer(te) &&
							std::find(dsiBuildTileEntityRenderers.begin(), dsiBuildTileEntityRenderers.end(), te) == dsiBuildTileEntityRenderers.end())
							dsiBuildTileEntityRenderers.push_back(te);
					}

					const int_t blockPass = block->getRenderBlockPass();
					if (dsiBuildPass == 0 && blockPass != 0)
						dsiBuildHasPass1 = true;
					if (blockPass != dsiBuildPass)
						continue;

#if PLATFORM_SKIP_ENCLOSED_OPAQUE_CUBES
					// Reaches here whenever the greedy pass didn't already claim this
					// block above -- either dsiAllowGreedyMesh is off this build
					// (connected/natural textures, or the kill switch), or it is a
					// simpleOpaqueCube block the greedy pass itself excludes (grass,
					// for its side-tint path). Either way a fully buried instance is
					// exactly the same wasted tessellate-six-invisible-faces work the
					// greedy pass's own per-face neighbour check already avoids for
					// its own blocks, so give this one the same cheap reject.
					if (dsiBuildPass == 0 && dsiGetBlockRenderInfo(id).simpleOpaqueCube &&
						dsiFullyEnclosedOpaqueCube(chunkcache, x, y, z))
						continue;
#endif

					if (dsiCullDecorationByFog)
					{
						const int_t renderType = block->getRenderType();
						if (renderType == 1 || renderType == 6)
						{
							const float dx = dsiDecorEyeX - (static_cast<float>(x) + 0.5f);
							const float dy = dsiDecorEyeY - (static_cast<float>(y) + 0.5f);
							const float dz = dsiDecorEyeZ - (static_cast<float>(z) + 0.5f);
							if (dx * dx + dy * dy + dz * dz > DSI_DECORATIVE_CULL_DISTANCE_SQ)
								continue;
						}
					}

					stepDrew |= renderblocks.renderBlockByRenderType(block, x, y, z);
				}

				if ((processed & 15) == 0 && PLATFORM_CHUNK_BUILD_STEP_US > 0)
				{
					const uint64_t nowUs = PlatformCompat::getMonotonicMicros();
					if (nowUs > stepStartUs && nowUs - stepStartUs >= (uint64_t)PLATFORM_CHUNK_BUILD_STEP_US)
						break;
				}
			}
		}

		tessellator->captureTextureGroups(dsiBuildExtraTextureMeshes[dsiBuildPass], true);
		dsiBuildDrew[dsiBuildPass] = dsiBuildDrew[dsiBuildPass] || stepDrew;

		static RenderCapturedMesh stepMesh;
		stepMesh.clear();
		if (tessellator->capture(stepMesh))
		{
#if PLATFORM_ENABLE_GREEDY_MESH
			// Still-water top-face merge, scoped to this step's own quads only
			// (not the whole accumulated dsiBuildRawBuffer) -- same bounded-
			// per-step cost shape as the greedy pass above, and mirrors
			// WorldRendererPs2.cpp's own hook point for the fork's water merge
			// this was ported from (right after capture, before the quads are
			// appended to the persistent build buffer). See DsiWaterMerge.h's
			// header comment for the full design (must run before
			// dsiRepackCapturedMeshStep() converts this data to fixed-point).
			if (dsiBuildPass == 1 && Block::waterStill != nullptr &&
				stepMesh.primitive == RenderPrimitive::Quads &&
				stepMesh.hasTexture && stepMesh.hasColor && stepMesh.hasBrightness)
			{
				const int_t waterTile = Block::waterStill->getBlockTextureFromSideAndMetadata(1, 0);
				const unsigned mergedQuads = dsi_merge_water_top_pairs(stepMesh.raw, waterTile,
					chunkcache, x0, y0, z0);
				if (mergedQuads != 0)
					stepMesh.vertexCount -= (int_t)(mergedQuads * 4u);
			}
#endif
			dsiBuildRawBuffer[dsiBuildPass].insert(dsiBuildRawBuffer[dsiBuildPass].end(),
				stepMesh.raw.begin(), stepMesh.raw.end());
			dsiBuildVertexCount[dsiBuildPass] += stepMesh.vertexCount;
			dsiBuildHasTexture[dsiBuildPass] |= stepMesh.hasTexture;
			dsiBuildHasColor[dsiBuildPass] |= stepMesh.hasColor;
			dsiBuildHasBrightness[dsiBuildPass] |= stepMesh.hasBrightness;
		}
		tessellator->setTranslationD(0.0, 0.0, 0.0);
		dsiBuildChunkLit |= Chunk::isLit;
		dsiStepDidWork |= processed > 0;

		if (dsiBuildCursor < totalBlocks)
			return false;

		// This pass's whole block loop is done: compile whatever it emitted
		// into the DSi captured-mesh backend (RenderStaticMesh, generic --
		// see the file banner). Not published to dsiLiveMesh yet: that only
		// happens once BOTH passes finish, below, so a section never shows an
		// updated pass 0 next to a stale pass 1.
		if (dsiBuildVertexCount[dsiBuildPass] > 0 && !dsiBuildRawBuffer[dsiBuildPass].empty())
		{
			const RenderPrimitive primitive = Tessellator::convertQuadsToTriangles
				? RenderPrimitive::Triangles : RenderPrimitive::Quads;

			RenderInterleavedMesh mesh;
			mesh.data = dsiBuildRawBuffer[dsiBuildPass].data();
			mesh.stride = 32;
			mesh.count = dsiBuildVertexCount[dsiBuildPass];
			mesh.primitive = primitive;
			mesh.hasTexture = dsiBuildHasTexture[dsiBuildPass];
			mesh.texCoordOffset = 12;
			mesh.hasColor = dsiBuildHasColor[dsiBuildPass];
			mesh.colorOffset = 20;
			mesh.hasBrightness = dsiBuildHasBrightness[dsiBuildPass];
			mesh.brightnessOffset = 28;
			renderStaticMeshCompile(dsiStagingMesh[dsiBuildPass], mesh);
			// Fixed-point repack (position/texcoord -> v16/t16, plus the GX
			// command compile) no longer happens here -- see the budgeted
			// dsiRepackCapturedMeshStep() phase right after this while loop
			// for why running it unconditionally at this point used to be a
			// real-hardware "build" spike source (up to 172ms in one call,
			// unbounded by blockBudget) and why it is now spread across
			// several calls instead.
		}
		else
		{
			renderStaticMeshDestroy(dsiStagingMesh[dsiBuildPass]);
		}

		std::vector<int_t>().swap(dsiBuildRawBuffer[dsiBuildPass]);
		dsiBuildPass++;
		dsiBuildCursor = 0;
		if (processed >= blockBudget)
			return false;
		if (PLATFORM_CHUNK_BUILD_STEP_US > 0)
		{
			const uint64_t nowUs = PlatformCompat::getMonotonicMicros();
			if (nowUs > stepStartUs && nowUs - stepStartUs >= (uint64_t)PLATFORM_CHUNK_BUILD_STEP_US)
				return false;
		}
	}

	// Both passes' block loops are done and both dsiStagingMesh[p] are
	// compiled (or destroyed, if that pass drew nothing) -- but not yet
	// converted to the DS GPU's native v16/t16 fixed-point layout or
	// compiled into a GX FIFO command stream. That conversion used to run
	// unconditionally, for a whole pass's mesh in one shot, the instant its
	// block loop crossed its last block (see the comment above where that
	// call used to sit): real-hardware evidence (the renderphase log's
	// "build" column) showed single dsiBuildRendererStep() calls spiking as
	// high as 172ms on a section with enough visible geometry -- several
	// full per-vertex passes over however many thousand vertices that
	// section's pass accumulated, each vertex paying multiple soft-float
	// multiplies (floattov16()/floattot16(), ARM9 has no FPU at all), with
	// nothing bounding it: PLATFORM_CHUNK_BUILD_BLOCKS_PER_STEP only ever
	// bounded the per-block loop above, never this repack work, and
	// PLATFORM_CHUNK_BUILD_STEP_US (the elapsed-time budget that would
	// otherwise have caught it) is 0 for DSi, inherited from PS2 unmodified.
	//
	// dsiRepackCapturedMeshStep() below is the same conversion, budgeted and
	// resumable across several calls via dsiRepackStage/dsiRepackCursor.
	// Safe to spread across multiple frames because it only ever touches
	// dsiStagingMesh (not yet published to dsiLiveMesh -- see the per-pass
	// compile block above): a section simply keeps drawing its OLD published
	// mesh, unconverted meshes are always safe to draw via the plain float
	// path (RenderCapturedMesh::positionIsV16/texCoordIsT16 default false),
	// for a few more frames while this finishes, exactly like the per-block
	// loop above already lets the rest of a build take several frames.
	for (int p = 0; p < 2; ++p)
	{
		if (dsiRepackStage[p] >= 2)
			continue;
		if (!dsiRepackCapturedMeshStep(dsiStagingMesh[p].captured, ConnectedTextures::getTerrainTextureId(),
				DSI_MESH_REPACK_VERTICES_PER_STEP, dsiRepackStage[p], dsiRepackCursor[p]))
			return false;
	}

	// Linear scans instead of hash sets -- see the PS2/Wii build paths in
	// net/minecraft/src/WorldRenderer.cpp for why (small lists, called rarely
	// relative to the per-block loop above).
	for (TileEntity *te : dsiBuildTileEntityRenderers)
	{
		if (std::find(tileEntityRenderers.begin(), tileEntityRenderers.end(), te) == tileEntityRenderers.end())
			pushUniqueTileEntityRef(tileEntities, te);
	}
	for (TileEntity *te : tileEntityRenderers)
	{
		if (std::find(dsiBuildTileEntityRenderers.begin(), dsiBuildTileEntityRenderers.end(), te) == dsiBuildTileEntityRenderers.end())
			eraseAllTileEntityRefs(tileEntities, te);
	}

	// Publish: swap each pass's freshly compiled staging mesh into the live
	// slot drawCapturedTerrain() reads, then free what used to be live (now
	// sitting in the staging slot after the swap). Both passes swap together,
	// here, after the loop above has finished both -- never one at a time.
	for (int_t p = 0; p < 2; ++p)
	{
		std::swap(dsiLiveMesh[p], dsiStagingMesh[p]);
		renderStaticMeshDestroy(dsiStagingMesh[p]);
		extraTextureMeshes[p].swap(dsiBuildExtraTextureMeshes[p]);
		dsiBuildExtraTextureMeshes[p].clear();
		_skipRenderPass[p] = !(dsiBuildDrew[p] &&
			(dsiBuildVertexCount[p] > 0 || !extraTextureMeshes[p].empty()));
	}
	tileEntityRenderers = dsiBuildTileEntityRenderers;
	const bool dirtyDuringBuild = dsiBuildDirtyDuringBuild;
	isChunkLit = dsiBuildChunkLit;
	isInitialized = true;
	needsUpdate = dirtyDuringBuild;
	chunksUpdated++;
	++g_dsiTotalRendererRebuilds;
	dsiResetBuildState();
	return true;
}

void WorldRenderer::renderExtraTerrainMeshes(int_t pass)
{
	if (pass < 0 || pass > 1 || _skipRenderPass[pass] || extraTextureMeshes[pass].empty())
		return;

	renderPushMatrix();
	// RenderList already applied the coarser origin-bucket-to-viewer
	// translate; captured CTM vertices are section-local (Tessellator's
	// setTranslationD(-posX,-posY,-posZ) during the build), so this adds the
	// same clip-origin translate drawCapturedTerrain() uses for the main mesh.
	renderTranslate((float)posXClip, (float)posYClip, (float)posZClip);
	for (const TessellatorTextureMesh &group : extraTextureMeshes[pass])
	{
		renderBindTexture(group.textureId);
		(void)renderDrawCaptured(group.mesh);
	}
	renderPopMatrix();

	// Terrain state assumes /terrain.png is still bound after each section.
	renderBindTexture(ConnectedTextures::getTerrainTextureId());
}

bool WorldRenderer::drawCapturedTerrain(int_t pass)
{
	if (pass < 0 || pass > 1)
		return false;
	if (!isInFrustum || !isInitialized || _skipRenderPass[pass])
		return false;

	renderPushMatrix();
	renderTranslate((float)posXClip, (float)posYClip, (float)posZClip);
	const bool drew = renderStaticMeshDraw(dsiLiveMesh[pass]);
	renderPopMatrix();
	return drew;
}

#endif // DSI_PLATFORM
