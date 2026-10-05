#pragma once


// Central DSi-only chunk streaming/cache knobs.
//
// Included from platform/PlatformTuning.h AFTER the desktop baseline table, and
// only when PLATFORM_DSI is set. This is an override file, not a third copy of
// the ~60-knob table: it only touches the subset that decides how many chunks
// stay resident and how aggressively they get written out to the SD card and
// freed. The much larger group of render/mesh-timing knobs (PLATFORM_MESH_BUDGET,
// PLATFORM_CHUNK_BUILD_BUDGET_MS, PLATFORM_RENDERER_UPDATE_CANDIDATES_PER_FRAME,
// ...) is deliberately left on the desktop defaults for now -- tuning those needs
// real frame-time measurements on device/in an emulator, which nothing in this
// repo can produce yet. Revisit once src/dsi/Makefile actually links (see the
// bring-up notes in DsiEarlyMemory.cpp/DsiBringup.cpp).
//
// The budget these numbers are sized against, and why they are tighter than PS2
// ------------------------------------------------------------------------------
// DsiEarlyMemory.cpp enforces a 12 MB heap ceiling total (code, textures, GL
// state, entities, the Java-side object heap -- everything, not just chunks).
// PS2's own comment in ps2/tuning/Ps2CoreTuning.h records a MEASURED number for
// its chunk cache: radius 2 with 3 vertical sections (25 columns x 3 = 75
// renderer slots) cost ~8 MB BY ITSELF, out of a 32 MB total budget. Even
// against DSi's larger 12 MB (raised from an initial 8, see DsiEarlyMemory.cpp)
// that is most of the whole-game allowance for chunks alone, so this file still
// does not copy PS2's radius as-is.
//
// It reuses PS2's eviction-rate constants (which are already the tightest in
// the table and are not a function of how much RAM the console has, just how
// much I/O stall per tick is tolerable), but not its radius/vertical-count
// values. The radius/vertical-count numbers below are a first estimate, NOT a
// measurement: scaled from PS2's one real data point by slot count --
// (1 chunk radius, 3x3=9 columns) x (2 vertical sections) = 18 slots, against
// PS2's 75 -- i.e. roughly a quarter of PS2's ~8 MB, call it ~2 MB, leaving
// comfortable headroom in the 12 MB budget for everything else. That headroom
// is exactly why radius 2 (PS2's own value) is the first thing worth trying
// once there is a real measured run to check it against -- see the corresponding
// comment in DsiEarlyMemory.cpp -- rather than guessing it up front.

#if PLATFORM_DSI
#define PLATFORM_BOUNDED_WORLD 1

// 3x3 columns rendered/cached around the player, 2 vertical sections centred on
// them. This is a genuinely short view distance -- shorter than PS2's already
// short 5x5 -- and is the direct, visible cost of an 8 MB total budget rather
// than PS2's 32 MB. If a real run shows headroom, raising this to PS2's radius 2
// is the first knob to try, one step at a time, watching the enforced heap
// ceiling in DsiEarlyMemory.cpp as the fitness function.
#undef  PLATFORM_VISIBLE_CHUNK_RADIUS
#define PLATFORM_VISIBLE_CHUNK_RADIUS            1
#undef  PLATFORM_VERTICAL_CHUNK_COUNT
#define PLATFORM_VERTICAL_CHUNK_COUNT            2
#undef  PLATFORM_CENTER_VERTICAL_RENDERERS
#define PLATFORM_CENTER_VERTICAL_RENDERERS       1

// PLATFORM_RANDOM_TICK_CHUNK_RADIUS/MOB_SPAWN_CHUNK_RADIUS do NOT inherit the
// PLATFORM_VISIBLE_CHUNK_RADIUS override two lines up: PlatformGameTuning.h
// aliases both to PS2_RANDOM_TICK_CHUNK_RADIUS/PS2_MOB_SPAWN_CHUNK_RADIUS
// (Ps2WorldTuning.h), which are themselves #define'd as the PS2-specific
// PS2_VISIBLE_CHUNK_RADIUS macro (=2, Ps2CoreTuning.h) -- a different token
// than the generic PLATFORM_VISIBLE_CHUNK_RADIUS this file overrides above,
// so DSi's radius-1 override never reaches these two. World::
// updateBlocksAndPlayCaveSounds() (World.cpp) builds its random-tick chunk
// set from this radius every tick: at 2, that is a 5x5=25-chunk set even
// though only DSi's real ~9 resident chunks (radius 1) exist, inflating
// randomTickScale's denominator and concentrating PLATFORM_RANDOM_BLOCK_
// TICKS_PER_CHUNK's per-visit attempts into bursts instead of spreading them
// evenly -- the same shape of tick-time variance/spike this file's own
// PLATFORM_MAX_SCHEDULED_TICK_UPDATES comment already documents chasing.
// Referencing PLATFORM_VISIBLE_CHUNK_RADIUS (not a literal 1) so this stays
// correct automatically if that radius is ever raised.
#undef  PLATFORM_RANDOM_TICK_CHUNK_RADIUS
#define PLATFORM_RANDOM_TICK_CHUNK_RADIUS        PLATFORM_VISIBLE_CHUNK_RADIUS
#undef  PLATFORM_MOB_SPAWN_CHUNK_RADIUS
#define PLATFORM_MOB_SPAWN_CHUNK_RADIUS          PLATFORM_VISIBLE_CHUNK_RADIUS

// Same shape of gap as the two radii just above, caught the same way (grep
// every PLATFORM_* this file does not already override, then check what it
// actually aliases to): PlatformGameTuning.h's PLATFORM_PS2||PLATFORM_DSI
// branch sets PLATFORM_VISIBLE_CHUNK_DIAMETER = PS2_VISIBLE_CHUNK_DIAMETER,
// a PS2-only token already baked to (PS2_VISIBLE_CHUNK_RADIUS*2+1) = 5 in
// Ps2CoreTuning.h -- a completely different expression from the generic
// PLATFORM_PC_LEGACY branch's own (PLATFORM_VISIBLE_CHUNK_RADIUS*2+1), so
// DSi's radius-1 override four lines up never reaches this one either.
// RenderGlobal.cpp's initRenderers() uses this diameter directly as
// renderChunksWide/renderChunksDeep and `new WorldRenderer(...)`-allocates
// one real object per grid cell (renderChunksWide*renderChunksTall*
// renderChunksDeep): at the inherited 5, that is a 5x2x5=50-renderer grid
// for a platform whose actual loaded chunk count this file's own radius
// override caps at ~9 (radius 1, i.e. a 3x3 chunk footprint) -- roughly 2.8x
// more WorldRenderer objects than DSi's world can ever populate, each one
// still paying a frustum test, a distance calculation and an update-queue
// check every single frame for the rest of the session, for a slot that
// can structurally never hold a chunk. Re-deriving from
// PLATFORM_VISIBLE_CHUNK_RADIUS (already overridden above) instead of the
// PS2-only token gives the correct 3, matching this file's own ~9-chunk
// footprint everywhere else already assumes.
#undef  PLATFORM_VISIBLE_CHUNK_DIAMETER
#define PLATFORM_VISIBLE_CHUNK_DIAMETER          (PLATFORM_VISIBLE_CHUNK_RADIUS * 2 + 1)

// Streaming window equal to the visible radius (PS2 settled on the same choice
// for the same reason -- see the "Back at radius 2 for memory" note in
// Ps2CoreTuning.h): a bigger cache radius buys lead time for prefetching, but
// every extra ring is columns resident in RAM, and RAM is what is missing here.
// Unload radius keeps PS2's one-larger hysteresis margin so crossing a chunk
// border does not immediately delete/reload the column just left.
#undef  PLATFORM_CHUNK_CACHE_RADIUS
#define PLATFORM_CHUNK_CACHE_RADIUS              1
#undef  PLATFORM_CHUNK_UNLOAD_RADIUS
#define PLATFORM_CHUNK_UNLOAD_RADIUS             2
#undef  PLATFORM_CHUNK_MAP_RESERVE
#define PLATFORM_CHUNK_MAP_RESERVE               16

// EntityLiving::shouldRunEntityDecisionAI() (EntityLiving.cpp) throttles a
// mob's expensive per-tick AI/pathfinding decisions based on distance from
// the nearest player: full rate inside PLATFORM_ENTITY_AI_NEAR_RADIUS_BLOCKS,
// PLATFORM_ENTITY_AI_MID_TICK_DIVISOR inside FAR_RADIUS, and the much
// cheaper FAR_TICK_DIVISOR beyond it. PlatformGameTuning.h aliases these to
// PS2's values (16/40 blocks) unmodified for DSi, but PS2 sized that 40-block
// far threshold against ITS OWN chunk footprint: PS2_CHUNK_CACHE_RADIUS=2 /
// PS2_CHUNK_UNLOAD_RADIUS=3 (32/48-block radii, Ps2CoreTuning.h), so a mob
// actually reaches PS2's far bucket before its chunk unloads. DSi's own
// radii just above are half of PS2's (cache 1 / unload 2, i.e. 16/32
// blocks): a DSi mob's chunk unloads well before it could ever cross PS2's
// 40-block far threshold, so PLATFORM_ENTITY_AI_FAR_TICK_DIVISOR -- the
// biggest AI-cost cut this mechanism offers -- was structurally unreachable
// on DSi, and the far/near split was rarely enough separated from a mob's
// max possible distance to do much either. Halved in step with DSi's own
// halved cache/unload radii above, the same proportion PS2's own thresholds
// keep against its radii, so a mob near DSi's unload boundary now actually
// reaches the cheapest tick rate instead of every mob in a loaded chunk
// always paying full AI cost. Same reasoning as this file's other numbers:
// derived from the radius ratio already established above, not a measured
// frame-time run -- revisit if a real profile suggests otherwise.
//
// (Reapplied: a CI workflow bug -- git reset --soft racing a mid-build
// source push, fixed in a later commit -- silently deleted this whole block
// once already; see that fix's own commit for the full account.)
#undef  PLATFORM_ENTITY_AI_NEAR_RADIUS_BLOCKS
#define PLATFORM_ENTITY_AI_NEAR_RADIUS_BLOCKS    8.0f
#undef  PLATFORM_ENTITY_AI_FAR_RADIUS_BLOCKS
#define PLATFORM_ENTITY_AI_FAR_RADIUS_BLOCKS     20.0f

// Same class of gap as the AI radii just above, found the same way (checked
// every PLATFORM_* distance this fork inherits from PS2 unmodified against
// DSi's own much smaller world): RenderGlobal.cpp's entity distance cull
// (PLATFORM_LIMIT_ENTITY_RENDER_DISTANCE, already on) was still using PS2's
// own PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS=20.0f verbatim -- LARGER than
// DSi's own fog-matched terrain cutoff (PLATFORM_VISIBLE_CHUNK_RADIUS*16 =
// 16 blocks, dsiSectionBeyondFog()'s own cull distance above), so a mob
// between 16 and 20 blocks out paid full render cost (3D model, texture,
// animation) in a band DSi's own terrain was already not drawing past.
// EntityLiving::shouldRunEntityDecisionAI()'s PLATFORM_ENTITY_PUSH_COLLISION_
// RADIUS_BLOCKS has the identical shape of problem: PS2's 24.0f, also
// unmodified, also larger than DSi's loaded-chunk footprint has any business
// reaching. Halved for both, the same ratio -- and the same "derived from
// PS2's own ratio against its radii, not a frame-time measurement" caveat --
// as the AI radii immediately above, not a fresh rationale per value.
#undef  PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS
#define PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS     10.0f
#undef  PLATFORM_ENTITY_PUSH_COLLISION_RADIUS_BLOCKS
#define PLATFORM_ENTITY_PUSH_COLLISION_RADIUS_BLOCKS 12.0f

// Eviction rate: reused from PS2 as-is. These bound how many chunks unload
// (write to SD + free) per tick, not how much RAM the cache holds, so they do
// not scale with the smaller radius above -- they exist to keep a save-to-SD
// burst out of a single frame regardless of cache size. PS2_MIN_UNUSED_TICKS_
// BEFORE_UNLOAD is halved from PS2's 60: DSi's cache is already a third the
// size, so a stale column needs to leave sooner to keep the resident set small,
// at the cost of writing to the SD card somewhat more often. Unverified without
// a measured SD write latency -- raise it if that turns out to matter.
#undef  PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK
#define PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK      1
#undef  PLATFORM_EMERGENCY_CHUNK_UNLOADS_PER_TICK
#define PLATFORM_EMERGENCY_CHUNK_UNLOADS_PER_TICK 2
#undef  PLATFORM_MIN_UNUSED_TICKS_BEFORE_UNLOAD
#define PLATFORM_MIN_UNUSED_TICKS_BEFORE_UNLOAD  30

// Two more region-file knobs (RegionFile.cpp) that were PS2/Wii-only
// (PlatformConfig.h's #ifndef defaults) and had never been evaluated for
// DSi, found while chasing the same unload-save cost the comment just above
// already flags as real but unmeasured. Both fire on exactly the call path
// above: a gameplay-edited chunk's synchronous unload save.
//
// PLATFORM_FAST_REGION_COMPRESSION: RegionFile::write()'s own comment says
// it plainly -- "console chunk writes run synchronously inside a world
// tick, so the deflate level is frame time, not disk space." Z_BEST_SPEED
// (zlib level 1) is several times faster to compute than the
// Z_DEFAULT_COMPRESSION (level 6) DSi was defaulting to, for sectors only
// 10-15% larger -- space the region format absorbs for free in its 4KB
// sector rounding anyway, per that same comment. DSi's ARM9 has no FPU and
// is weaker than either console this was already proven safe on, so the
// CPU-time case for the cheaper level is at least as strong here.
#undef  PLATFORM_FAST_REGION_COMPRESSION
#define PLATFORM_FAST_REGION_COMPRESSION         1
// PLATFORM_SMALL_REGION_SCRATCH: inflateChunkData()'s own comment says a
// Beta chunk's raw NBT is ~85KB, yet the default starting scratch buffer
// for decompressing one on load is 256KB (PS2 already dropped to 64KB,
// growing on demand only if a chunk genuinely needs more, up to the same
// 1MB cap either way). DSi's heap ceiling (~13.5MB committed, observed) is
// smaller than PS2's 32MB this 256KB default was deemed safe for, so the
// transient over-allocation this causes on every single chunk load is
// proportionally far more expensive here, for no benefit: the buffer still
// grows past 64KB on request for the rare chunk that needs it.
#undef  PLATFORM_SMALL_REGION_SCRATCH
#define PLATFORM_SMALL_REGION_SCRATCH             1

// Read-only worlds (prebuilt maps) never need to keep a dirty chunk resident
// for a later save, so they can use a faster unload throttle than a normal
// world -- same values PS2 uses, since this is again about I/O-stall shape, not
// the radius.
#undef  PLATFORM_READ_ONLY_CHUNK_CACHE_RADIUS
#define PLATFORM_READ_ONLY_CHUNK_CACHE_RADIUS              PLATFORM_CHUNK_CACHE_RADIUS
#undef  PLATFORM_READ_ONLY_CHUNK_UNLOAD_RADIUS
#define PLATFORM_READ_ONLY_CHUNK_UNLOAD_RADIUS             PLATFORM_CHUNK_UNLOAD_RADIUS
#undef  PLATFORM_READ_ONLY_MAX_CHUNK_UNLOADS_PER_TICK
#define PLATFORM_READ_ONLY_MAX_CHUNK_UNLOADS_PER_TICK      4
#undef  PLATFORM_READ_ONLY_MIN_UNUSED_TICKS_BEFORE_UNLOAD
#define PLATFORM_READ_ONLY_MIN_UNUSED_TICKS_BEFORE_UNLOAD  10

// The End dimension eagerly generates every chunk within
// PLATFORM_END_RESIDENT_CHUNK_RADIUS in one synchronous pass on arrival
// (World::isChunkResident() never lets those columns unload -- ChunkProvider.cpp)
// and keeps them ALL resident for the whole visit, not just the normal cache
// radius above. PS2 sizes this at radius 5 (121 columns) against its own
// comment's "~3-3.5 MB out of ~14.5 MB free" measurement -- a real number for
// PS2's 32 MB total budget, not DSi's. DSi's entire NORMAL chunk cache
// (radius 1, 9 columns) is deliberately budgeted at only ~2 MB out of this
// file's own 12-13.5 MB total heap ceiling (see the file banner); inheriting
// PS2's End radius unmodified would eagerly generate 121 columns -- over 13x
// DSi's entire normal chunk-cache footprint -- against a heap roughly a
// third the size PS2 sized 121 columns against. Disabled outright (-1, the
// same sentinel PC's own default branch in PlatformGameTuning.h already
// uses -- World::isChunkResident() treats any negative radius as "nothing is
// forced resident") rather than picked as a smaller-but-still-eager number:
// unmeasured on real DSi hardware (nobody has visited the End on this port
// yet), so the conservative choice is to let it stream like every other
// dimension instead of guessing a radius that might still be too large.
#undef  PLATFORM_END_RESIDENT_CHUNK_RADIUS
#define PLATFORM_END_RESIDENT_CHUNK_RADIUS       -1

// New-world preload: one chunk each way (16 blocks), half of PS2's 32 -- the
// preload window should not be bigger than the cache radius it is filling.
#undef  PLATFORM_PRELOAD_RADIUS_BLOCKS
#define PLATFORM_PRELOAD_RADIUS_BLOCKS           16

// Vanilla will not tick an entity unless a 32-block area around it is loaded.
// With a 3x3 chunk cache that guard can starve the local player's own ticking
// (see the identical PS2 comment in Ps2CoreTuning.h -- same cause, same fix):
// only require the entity's own chunk to be loaded.
#undef  PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS
#define PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS 0

// SD card write latency through BlocksDS's FatFs has not been measured yet on
// this port. Default to PS2's conservative answer (no background autosave,
// explicit save only) rather than guessing that a microSD card is "obviously
// fast enough" -- if a measured run shows the incremental save path is cheap
// here, this is the knob to flip back on.
#undef  PLATFORM_DISABLE_RUNTIME_AUTOSAVE
#define PLATFORM_DISABLE_RUNTIME_AUTOSAVE        1
#undef  PLATFORM_SKIP_NEW_WORLD_FULL_SAVE
#define PLATFORM_SKIP_NEW_WORLD_FULL_SAVE        1
#undef  PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT
#define PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT    2

// Real-hardware evidence (reported: 80-190ms "worldTick" spikes, worst right
// after a burst of chunks streams in): World::TickUpdates() drains its
// scheduledTickTreeSet (fluid spread, leaf decay, redstone, crop growth) in
// one synchronous call, capped only at vanilla's own 1000 entries with no
// time bound beyond that count -- see Ps2WorldTuning.h's
// PS2_MAX_SCHEDULED_TICK_UPDATES for the full explanation, kept at vanilla's
// 1000 there since PS2 has real headroom for it. A newly streamed-in DSi
// chunk with active water/lava/crops can schedule far more than this
// console's own per-tick budget can absorb in one call, all due the same
// world tick. 100 is a first estimate (a tenth of vanilla's cap, the same
// conservative-first-measurement shape as PS2_RANDOM_TICK_CHUNKS_PER_TICK's
// own round-robin fix for the analogous updateBlocksAndPlayCaveSounds spike)
// -- not yet measured against a real chunk-load-heavy session; the backlog
// simply continues over more world ticks rather than being dropped, so this
// trades tick-processing latency (fluids/redstone catching up a few ticks
// later under heavy load) for removing the synchronous stall.
#undef  PLATFORM_MAX_SCHEDULED_TICK_UPDATES
#define PLATFORM_MAX_SCHEDULED_TICK_UPDATES      100

// The other half of the fix above, same real-hardware shape as
// PLATFORM_LIGHTING_BUDGET_US (World::updatingLighting(), PlatformGameTuning.h):
// a real-hardware log taken AFTER the 100-entry cap above still showed
// "worldTick" spikes of 174/327/361/821ms -- worse than the 80-190ms the cap
// was sized against, not better. The cap bounds how many scheduled ticks
// World::TickUpdates() drains per call, but never how long each one takes:
// every one of those 100 runs an arbitrary Block::updateTick(), and fluid
// flow / redstone propagation are exactly the kind of call whose cost swings
// by an order of magnitude with what it actually touches (same reasoning
// PS2_LIGHTING_BUDGET_US's own comment gives for lighting jobs) -- a run of
// chunks with active water or a redstone contraption can make 100 of those
// average several ms each instead of the usual fraction of one.
//
// 3000us (3ms) adds a wall-clock ceiling on top of the existing count cap:
// TickUpdates() now stops and returns as soon as EITHER limit is hit,
// exactly like updatingLighting() already does with its own budget. Not a
// correctness risk: an entry not reached this call is simply still in
// scheduledTickTreeSet for the next world tick, the same "catches up a few
// ticks later instead of stalling" trade the 100-entry cap above already
// made, just with a bound that actually tracks wall-clock time instead of
// hoping a fixed count stays cheap. First estimate, same as every other
// unmeasured budget in this file -- revisit once a real-hardware log with
// this change confirms "worldTick" no longer spikes past a few ms.
#undef  PLATFORM_TICK_UPDATES_BUDGET_US
#define PLATFORM_TICK_UPDATES_BUDGET_US          3000

// Ambient world particles (torch flame, lava drip, portal sparkle, ...):
// World::randomDisplayUpdates() probes PLATFORM_RANDOM_DISPLAY_PROBES nearby
// block positions every tick (6 RNG draws + a block lookup each) purely to
// decide whether to spawn one of these. PS2 already cut this from vanilla's
// 1000 probes to 250 (see the "347 ms slowTick=randomDisplay spike" comment
// in Ps2WorldTuning.h) but left the feature itself on, and DSi inherited
// that 250-probe value wholesale. Real-hardware reports from this port
// (also on Wii, which has more headroom than either of these) still show
// randomDisplay costing 110+ ms a tick even at 250 probes -- user-reported,
// specifically noticeable while falling through the world (the still-open
// fall-through-the-floor bug this session is chasing), on hardware with by
// far the least CPU/memory budget of the three. PLATFORM_SKIP_WORLD_PARTICLES
// already exists for exactly this: World::randomDisplayUpdates() returns
// immediately when it is set, skipping the probe loop entirely rather than
// running it and discarding every result. DSi-only: PS2/Wii keep whatever
// ambient particle density they already have, since this is not a change
// either of those platforms asked for.
#undef  PLATFORM_SKIP_WORLD_PARTICLES
#define PLATFORM_SKIP_WORLD_PARTICLES             1

// Toggle for day/night/torch lighting (EntityRenderer.cpp's updateLightmap()
// -> renderSetLightmapColors(), consumed per-vertex by RenderAPI_DSI.cpp's
// drawInterleavedMesh()). Real-hardware A/B test, both with
// emitColorIfChanged()'s redundant-colour dedup in place: lighting on was
// still noticeably slower than lighting off, not just "a bit off" -- off
// stays the default until a further optimization pass (greedy meshing,
// fewer draw calls, ...) closes more of that gap. Flip to 1 to re-test; the
// EntityRenderer.cpp/RenderAPI_DSI.cpp wiring itself is unaffected either
// way. See EntityRenderer.cpp's
// `defined(PS2_PLATFORM) || (defined(DSI_PLATFORM) && PLATFORM_DSI_LIGHTMAP_ENABLED)`
// guard, the only place this is read.
#undef  PLATFORM_DSI_LIGHTMAP_ENABLED
#define PLATFORM_DSI_LIGHTMAP_ENABLED              0

// Greedy meshing (src/dsi/render/DsiGreedyMesh.cpp): PLATFORM_ENABLE_GREEDY_MESH
// is already 1 here via PlatformGameTuning.h's PLATFORM_PS2||PLATFORM_DSI branch
// (PS2_ENABLE_GREEDY_MESH) -- these two are DSi's own counterparts to PS2's
// PS2_GREEDY_MAX_MERGE/PS2_GREEDY_SLICES_PER_STEP (Ps2MeshTuning.h), not aliased
// through the generic PLATFORM_ table because nothing else reads them.
//
// DSI_GREEDY_MAX_MERGE matches PS2's own value (2, not the algorithm's 16-wide
// ceiling): DsiGreedyMesh.cpp's merged-quad UV span does not scale with
// width/height (see its header comment -- DS has no hardware region-repeat to
// tile a scaled span against), so a merged quad's single source tile is
// stretched across the merged area instead of tiled. Capping merges at 2x2
// bounds that stretch to something unnoticeable rather than letting a long
// flat run (a 16-wide floor, say) stretch one 16px tile across 16 blocks.
#undef  DSI_GREEDY_MAX_MERGE
#define DSI_GREEDY_MAX_MERGE                     2

// DSI_GREEDY_SLICES_PER_STEP: how many of a face direction's up-to-16 planes
// the greedy sub-phase scans in one dsiBuildRendererStep() call before
// returning to let the rest of the frame run. Half of PS2's 4 -- DSi's ARM9 has
// no hardware FPU at all (PS2's EE does), the same reasoning DsiWorldTuning.h's
// own banner gives for every other budget in this file being tighter than
// PS2's, and this is unmeasured on real hardware yet (see that banner): a
// smaller slice keeps the worst case per-step cost bounded while there is
// still no frame-time measurement to size it against directly.
#undef  DSI_GREEDY_SLICES_PER_STEP
#define DSI_GREEDY_SLICES_PER_STEP               2

// DSI_GREEDY_BATCHES_PER_CALL: how many DSI_GREEDY_SLICES_PER_STEP-sized
// batches WorldRendererDsi.cpp's dsiBuildRendererStep() runs back-to-back in
// ONE call before yielding, instead of exactly one. Real-hardware evidence
// (see that call site's own comment): at 1 batch/call, a section's whole
// 6-face greedy phase needed 48 separate calls -- 48 frames -- to finish,
// and PLATFORM_CHUNK_BUILD_STEP_US (the elapsed-time budget that would
// otherwise cap a call's cost) is 0 for DSi, inherited from PS2 unmodified,
// so nothing was actually bounding how long that window could stretch. A
// world never sits still that long without SOMETHING marking a section
// dirty (fluid flow, a scheduled tick), and dsiResetBuildState() discards a
// build's entire progress -- greedy phase included -- on any such interrupt,
// so sections were restarting before they ever finished: confirmed via
// memtrend's rebuilds= counter climbing tens of times a second while
// chunks/entities/heap stayed completely flat.
//
// 6 batches/call cuts the frames-to-finish by the same factor (48 -> 8),
// without jumping straight to "the whole phase in one call", which risks
// reproducing the single-frame spike slicing was introduced to avoid in the
// first place (see Ps2GreedyMesh.h's own account of that exact problem on
// PS2 -- whose EE has no FPU gap to DSi's ARM9 at all, this platform's
// weakest link). Unmeasured against real per-step timing, same as
// DSI_GREEDY_SLICES_PER_STEP itself; revisit both together once a real
// build-time profile exists.
#undef  DSI_GREEDY_BATCHES_PER_CALL
#define DSI_GREEDY_BATCHES_PER_CALL              6

// EMERGENCY KILL SWITCH, real-hardware evidence: Config::isConnectedTextures()
// defaulted to true on DSi for this whole session (GameSettingsBackend_DSI.cpp
// returned 0, not 3, from platformGameSettingsDefaultConnectedTextures() --
// now fixed), and WorldRendererDsi.cpp's dsiAllowGreedyMesh gates the whole
// greedy phase on !isConnectedTextures(). That means the greedy-mesh code
// path in DsiGreedyMesh.cpp had NEVER ACTUALLY EXECUTED on real hardware
// before the default fix landed -- every earlier "greedy mesh" build this
// session ran the plain per-block path the whole time.
//
// The very first real-hardware run with the default fix applied (user debug
// log, same session) showed a catastrophic steady-state regression: render
// time pinned at ~212-240ms EVERY frame (not a spike -- min and max nearly
// identical across a long, otherwise-idle stretch with rebuilds= frozen, i.e.
// no rebuilding in flight, just steady replay cost), matching the user's own
// "4 fps" report almost exactly (1000/213 =~ 4.7 fps). That is far worse than
// this platform's established non-greedy baseline (~130-165ms/frame render
// under comparable load, from this same session's earlier logs).
//
// ROOT-CAUSED: not a replay-cost regression at all, and not a bug in
// DsiGreedyMesh.cpp's algorithm (which really is sound -- bounded 16x16
// mask, no double-render against the per-block loop's skip check, same
// capture/repack/replay path as ordinary terrain, all independently
// re-confirmed while chasing this). The real bug was a step-accounting gap
// in WorldRendererDsi.cpp's dsiBuildRendererStep(): the greedy branch did
// real, non-trivial work (up to DSI_GREEDY_BATCHES_PER_CALL batches of
// DSI_GREEDY_SLICES_PER_STEP mask slices each call) but never incremented
// `processed`, the one signal dsiStepDidWork -- and therefore
// RenderGlobal.cpp's MeshBudget::run(), via lastTerrainBuildStepDidWork()
// -- uses to decide whether a call's real measured wall-clock cost counts
// toward the per-frame build budget (PLATFORM_CHUNK_BUILD_BUDGET_MS, ~6ms)
// and the per-frame update-count cap (~10). Every greedy-only call looked
// like "did nothing" to the scheduler, so neither cap ever tripped: up to
// PLATFORM_RENDERER_UPDATE_CANDIDATES_PER_FRAME (32) sections mid-greedy-
// build could each get a full, uncapped greedy call in the SAME frame,
// with zero budget enforcement -- exactly the "render time pinned at a
// scene-independent constant, rebuilds frozen" signature the original
// report described (it depends on how many sections are mid-build, not
// what's actually visible), not a per-vertex replay problem. Fixed by
// charging `processed += dsiGreedyBatchesThisCall` at the end of the
// greedy branch, restoring the same accounting the per-block loop's own
// `++processed` already gives the scheduler.
//
// Re-enabled. Needs a real-hardware frame-time confirmation like every
// other fix in this file -- if it regresses again, the restarts=/rebuilds=
// memtrend counters (Minecraft.cpp) and a per-frame "greedy calls this
// frame" count are the next diagnostic to reach for before assuming the
// algorithm itself is at fault again.
#undef  DSI_GREEDY_MESH_RUNTIME_ENABLED
#define DSI_GREEDY_MESH_RUNTIME_ENABLED           1

// Ported from upstream OptiCraft Heritage Edition's PS2 fix (Ps2MeshTuning.h's
// PS2_RAIN_SPLASH_PARTICLES_PER_TICK/PS2_ENTITY_FIRE_MAX_LAYERS/
// PS2_ENTITY_FIRE_LAYER_STEP), which this fork's own PS2 code predates and
// therefore never had either -- these are DSi's own counterparts, not
// aliased through the generic PLATFORM_ table because nothing else reads
// them, same reasoning as DSI_GREEDY_MAX_MERGE above.
//
// EntityRenderer::addRainParticles() still runs vanilla's full 100-attempt
// ground-splash burst every tick even with the rain/snow curtains themselves
// already skipped (PLATFORM_SKIP_WORLD_PARTICLES only covers ambient
// randomDisplayUpdates() particles, a separate system) -- each attempt that
// lands can spawn an EntityRainFX with its own lifetime, alpha-tested quad,
// and Tessellator vertex work. Originally reused PS2's own measured value
// (4/tick). Upstream later found even that 4/tick could "keep effects around
// 3-4 ms/tick" and push its GPU queue past 80% on real PS2 hardware, and
// tightened its own PS2_RAIN_SPLASH_PARTICLES_PER_TICK to 2/tick (upstream
// commit 6456969, "perf(ps2) tighten world particle budget" -- not ported to
// this fork's own Ps2MeshTuning.h, since PS2 isn't this fork's target and
// that file is otherwise untouched here; only the DSi-relevant reasoning is
// applied below). Following the same reasoning here: DSi's ARM9 has no FPU
// at all, strictly weaker than PS2's EE
// for this same per-particle float work, so if 4/tick was too much on PS2 it
// is not obviously safe on DSi either. This is an EXTRAPOLATION from PS2's
// measurement, not a DSi-hardware confirmation of its own -- flag for
// real-hardware re-measurement (does rain near open water still feel smooth
// at 2/tick, and did it actually help) rather than treating 2 as settled.
#define DSI_RAIN_SPLASH_PARTICLES_PER_TICK       2

// The other half of that same upstream commit (6456969, "perf(ps2) tighten
// world particle budget") that the comment above never ported: besides the
// rain-splash rate it also cut PS2_MAX_PARTICLES_PER_LAYER from 256 to 128
// (EffectRenderer.cpp's hard cap on how many particles -- block-break smoke,
// splashes, bubbles, everything in EffectRenderer's own layers -- can exist
// at once before new ones stop spawning), citing real PS2 hardware evidence
// of a saturated particle workload costing 3-4 ms/tick and pushing its GS
// queue past 80% even once chunk rebuilding had stopped. Unlike the
// rain-splash constant, PLATFORM_MAX_PARTICLES_PER_LAYER already is read
// through the generic PLATFORM_ table (EffectRenderer.cpp), and DSi was
// still inheriting PS2's PRE-fix 256 wholesale (PlatformGameTuning.h's
// PLATFORM_PS2||PLATFORM_DSI alias block, unconditionally) -- this was
// simply missed when the rain-splash half was ported, not a deliberate
// choice to keep the looser cap. Matching PS2's own now-tightened value
// here, same "DSi's ARM9 has no FPU at all, extrapolating from PS2's own
// measurement on stronger hardware" reasoning as DSI_RAIN_SPLASH_PARTICLES_
// PER_TICK above -- flag for the same real-hardware re-confirmation.
#undef  PLATFORM_MAX_PARTICLES_PER_LAYER
#define PLATFORM_MAX_PARTICLES_PER_LAYER         128

// Render::renderEntityOnFire() draws a stack of heavily overlapping fire
// billboards -- vanilla's 0.45 step produces about five layers per burning
// mob. Same reasoning and same values as PS2's fix: three broader-spaced
// layers keep the silhouette covered while cutting fill-rate and generic
// Tessellator vertex work for every burning entity on screen.
#define DSI_ENTITY_FIRE_MAX_LAYERS                3
#define DSI_ENTITY_FIRE_LAYER_STEP                0.70f

// How many vertices WorldRendererDsi.cpp's dsiRepackCapturedMeshStep() phase
// converts/compiles per dsiBuildRendererStep() call. Real-hardware evidence
// (renderphase log's "build" column): converting a whole finished section's
// mesh to the DS GPU's v16/t16 fixed-point layout and compiling its GX FIFO
// command stream in one unconditional, unbudgeted shot -- as this used to run
// the instant a pass's block loop finished -- spiked a single call as high as
// 172ms on a section with enough visible geometry, because
// PLATFORM_CHUNK_BUILD_BLOCKS_PER_STEP (512) only ever bounded the per-block
// loop before it, never this repack work, and PLATFORM_CHUNK_BUILD_STEP_US
// (the elapsed-time budget that would otherwise have caught it) is 0 for DSi,
// inherited from PS2 unmodified.
//
// 256 is a first estimate, not a measurement, same as this file's other
// unmeasured knobs (see its own banner): each vertex here costs a handful of
// soft-float multiplies on this FPU-less ARM9 (floattov16()/floattot16()),
// roughly the same order of magnitude per-unit cost as a block in the
// per-block loop above pays, so starting at half PLATFORM_CHUNK_BUILD_BLOCKS_
// PER_STEP errs conservative until a real per-step timing measurement can
// size it properly. Revisit together with that budget once one exists.
#define DSI_MESH_REPACK_VERTICES_PER_STEP         256

// The repack spike above and the greedy-mesh regression DSI_GREEDY_MESH_
// RUNTIME_ENABLED's own comment describes both trace back to the same root
// cause: PLATFORM_CHUNK_BUILD_STEP_US, the ONE generic elapsed-time check
// dsiBuildRendererStep() already has wired in (its per-block loop's own
// `if ((processed & 15) == 0 && PLATFORM_CHUNK_BUILD_STEP_US > 0) { ... break
// ...}`, WorldRendererDsi.cpp), is 0 here -- inherited from PS2 unmodified --
// which makes that check a permanent no-op. Both regressions needed their own
// specific, narrowly-targeted budget (DSI_MESH_REPACK_VERTICES_PER_STEP
// above, DSI_GREEDY_BATCHES_PER_CALL) because the one mechanism meant to
// catch an overlong call generically was never actually doing anything.
//
// Upstream has since enabled this same budget for PS2 itself (PS2_CHUNK_
// BUILD_STEP_US, 0 -> 4000, PS2_CHUNK_BUILD_TIME_CHECK_BLOCKS 32): a
// real-hardware log walking around open water showed the build phase
// averaging 6-10 ms/frame but individual chunk-build samples still reaching
// 22-27 ms, coinciding with a 20-25 FPS oscillation -- on the EE, which
// unlike DSi's ARM9 has a real FPU. Not ported to this fork's own
// Ps2MeshTuning.h (PS2 isn't this fork's target, same reasoning as every
// other upstream PS2 commit this file cites); only the DSi-relevant value
// below.
//
// This is a GENERIC safety net, not a replacement for the two specific
// fixes above: unlike those, it catches any call that overruns for ANY
// reason, including ones not yet profiled. 3000us (3ms, tighter than PS2's
// 4ms for the same weaker-CPU reasoning as every other unmeasured knob in
// this file) with DSi's own existing 16-block check granularity (finer than
// PS2's new 32-block one, so no PLATFORM_CHUNK_BUILD_TIME_CHECK_BLOCKS
// override is needed here) -- a first estimate, flagged like every other
// knob in this file for real-hardware re-measurement once a frame-time log
// exists to size it against directly.
#undef  PLATFORM_CHUNK_BUILD_STEP_US
#define PLATFORM_CHUNK_BUILD_STEP_US               3000

// The other half of the same real-hardware finding above: upstream also
// tightened PS2_CHUNK_BUILD_BUDGET_MS, the shared PER-FRAME ceiling on total
// mesh-build time across however many dsiBuildRendererStep() calls
// RenderGlobal::updateRenderers() makes that frame (checked between calls,
// not inside one -- PLATFORM_CHUNK_BUILD_STEP_US above is the per-call
// bound), from 6 to 4, citing the same open-water profiling run. DSi was
// still inheriting the pre-fix 6 wholesale (this file's own banner
// originally left every render/mesh-timing knob on the desktop/PS2 default
// "until real frame-time measurements exist" -- several now do, this file's
// own growing set of real-hardware-cited fixes above among them). 3, not
// matching PS2's new 4, for the same weaker-ARM9 reasoning as every other
// unmeasured knob here -- but not lower: PLATFORM_CHUNK_BUILD_STEP_US above
// already needs up to 3ms for one call's own sub-budget, and a per-frame
// ceiling below what a single call is allowed to spend would leave calls
// unable to use their own granted time.
#undef  PLATFORM_CHUNK_BUILD_BUDGET_MS
#define PLATFORM_CHUNK_BUILD_BUDGET_MS              3

// Real-hardware evidence (two debug.log comparisons, one from before a round
// of GUI/render caching work and one from after -- both showing the same
// pattern, so this isn't something that round introduced): memtrend's
// rebuilds= counter climbs continuously through a whole play session (12->1061
// in one log, 11->463 in another) while getLoadedChunkCount() stays flat at
// 9-16 the entire time -- the same handful of already-built sections getting
// fully rebuilt over and over. dsiGetTotalBuildRestarts() (an interrupted,
// discarded-and-restarted build, DSI_GREEDY_BATCHES_PER_CALL's fix above
// already addresses the worst of that class) only accounts for ~13-15% of
// that total in both logs, so most of it is something else: full rebuilds
// that DID complete, immediately followed by another one for the same
// section.
//
// Root cause: World::updatingLighting() (World.cpp) runs once per rendered
// frame and drains only PLATFORM_LIGHTING_UPDATES_PER_FRAME (128, inherited
// from PS2) jobs from lightingToUpdate before returning -- but a chunk newly
// streamed in at this platform's tiny PLATFORM_VISIBLE_CHUNK_RADIUS=1 edge
// schedules thousands of lighting jobs at once (its own comment: "fed
// heavily when chunks arrive... thousands of jobs at a time"), so draining
// one such flood takes dozens of frames. Each frame's LightingDirtyRegions
// batch (World.cpp) flushed at the end of that SAME frame's call -- so every
// one of those dozens of frames re-marked whichever already-built neighbor
// section the flood was currently touching, and since a DSi incremental
// build (with DSI_GREEDY_BATCHES_PER_CALL=6 above) often finishes well
// inside that many frames, the section would complete, get marked dirty
// again by the still-draining flood, and rebuild again -- repeatedly, from
// one streamed-in border chunk.
//
// This does not risk stale/incorrect lighting: World.h's markDirtyFromLighting()
// (vs. markDirty()) already coalesces rather than restarting an in-progress
// build for exactly this kind of mark (RenderGlobal.cpp's
// isMarkingFromLighting() branch), and the final flush once the queue
// actually empties is unconditional regardless of this window -- widening the
// window only delays a mid-flood section's visual light update by up to this
// many extra frames, it never drops one. 4 is a first estimate (unmeasured on
// real hardware, same as this file's other knobs -- see its own banner):
// small enough to keep that extra staleness imperceptible even at this
// platform's low framerate, while still cutting the number of separate
// rebuild cycles during a flood by roughly the same factor. Revisit once a
// real-hardware log with this change confirms rebuilds= climbing much more
// slowly relative to getLoadedChunkCount() staying flat.
//
// That revisit happened: a later real-hardware log (chunks=12 the entire
// session -- player stayed near one streaming edge instead of walking past
// it) showed rebuilds=12->93+ and restarts=0->17 still climbing continuously
// at 4, with frame time dominated by the opaque replay pass growing in step
// (54ms -> 140ms+) as each restart/rebuild re-submits that section's geometry.
// 4 frames was not enough to get ahead of a sustained flood (thousands of
// jobs queued at once, draining PLATFORM_LIGHTING_UPDATES_PER_FRAME=128 at a
// time -- dozens of calls per flood regardless of this window). Doubled to 8:
// still bounded, non-regressive staleness (the final flush once the queue
// actually empties is unconditional, per this function's own comment --
// World::updatingLighting() in World.cpp -- so this only changes how many
// extra frames a mid-flood section's visible light can lag behind, never
// whether it catches up), and at this platform's ~7-14fps, 8 frames is still
// well under a second.
//
// That revisit happened too: a real-hardware log at 8 (chunks=12->15, the
// player walking rather than standing still at one edge -- a lighter flood
// than the stationary case that motivated 4->8, but a sustained one) still
// showed rebuilds=9->53 (restarts stayed flat at 0 throughout, confirming
// the interrupted-build class DSI_GREEDY_BATCHES_PER_CALL's fix already
// handles is fully gone -- what is left is entirely the complete-then-
// immediately-redo class this knob targets) and the opaque replay pass
// growing in step, 11ms -> 72ms worst-case, over a session short enough
// that a user report of "~3 fps worse than before" traced back to this log
// specifically. Same shape as the 4->93+ case that justified 4->8, just
// less extreme because this session's flood was smaller -- not yet "ahead
// of the flood" the way restarts=0 suggests the interrupted-build side now
// is. Doubled again to 16, same reasoning as the first doubling (bounded,
// non-regressive staleness, well under a second even at this platform's
// framerate): if a real-hardware log at 16 still shows rebuilds climbing
// this fast relative to a flat/near-flat chunk count, the bottleneck is the
// flood's drain rate (PLATFORM_LIGHTING_UPDATES_PER_FRAME=128, inherited
// from PS2 and never tuned for DSi -- see this knob's own comment above)
// and not this window, and that is the next knob to revisit instead of
// widening this one further.
#undef  PLATFORM_LIGHTING_DIRTY_FLUSH_INTERVAL_FRAMES
#define PLATFORM_LIGHTING_DIRTY_FLUSH_INTERVAL_FRAMES 16

// CORRECTNESS FIX, not a performance tuning value: PLATFORM_SAVE_RUNTIME_
// CHUNK_EDITS_ON_UNLOAD defaults to PLATFORM_PS2 only (PlatformConfig.h), and
// ChunkProviderLoadOrGenerate::unloadChunk()/ChunkProvider::unloadChunk()'s
// alternate save path is gated on `!PLATFORM_CONSOLE_LOW`, which DSi also
// fails (PLATFORM_CONSOLE_LOW is on for DSi too -- see PlatformGameTuning.h's
// own banner). Neither branch compiled for DSi, so unloadChunk() has never
// saved an edited chunk before deleting it: any block break/place, container
// content change, etc. in a chunk that streams out of this platform's tiny
// cache (PLATFORM_CHUNK_CACHE_RADIUS=1, PLATFORM_CHUNK_UNLOAD_RADIUS=2 below
// -- chunks stream in/out constantly just from ordinary walking) was silently
// lost unless an explicit full-world save happened to run first. The
// producer side of this same flag (Chunk::setBlockIDWithMetadata/
// setBlockMetadata's markRuntimeSaveRequired() calls, Chunk.cpp) was already
// present and correctly excludes world-gen population
// (isPopulationFastPathChunk), just never wired up to anything that read it
// on this platform -- turning this on requires no other change; it re-enables
// an already-implemented, already-shipping-on-PS2 code path.
//
// Trade-off: DSi has no background I/O thread (PLATFORM_ASYNC_FILE_IO is off
// for DSi), so this save runs synchronously, inline, at the moment an edited
// chunk unloads -- a real stall on the SD card exactly during ordinary
// exploration, not just during an explicit "Saving..." screen. That is the
// same calling convention every other DSi save already uses (nothing here
// introduces a new threading assumption), and losing player progress
// silently is a worse failure mode than an occasional save-stall, so this is
// the right trade -- but it's a real, user-visible behavior change worth
// watching for in the next real-hardware test (a brief hitch right as you
// walk away from an edited area).
#undef  PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_UNLOAD
#define PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_UNLOAD 1

// Real-hardware evidence (this session's own investigation, see
// PlatformConfig.h's PLATFORM_CAN_SEE_CACHE_TICKS comment for the mechanism):
// an actively-chasing/attacking hostile mob (zombie, skeleton, creeper) pays
// a full World::rayTraceBlocks() block-march plus two heap-allocated Vec3D
// every single tick via EntitySenses::clearSensingCache() wiping its cache
// before PLATFORM_THROTTLE_ENTITY_AI's shouldRunEntityDecisionAI() check ever
// runs -- and that throttle doesn't apply to an already-active task anyway,
// so it fires hardest exactly when several hostile mobs are near the player
// (combat), the worst possible moment on this weak ARM9. 5 ticks (a quarter
// of a second at the intended 20 TPS) is comfortably inside the existing
// tolerance windows this was checked against: EntityAITarget allows up to 60
// ticks of "not seen" before giving up a target, and EntityAIArrowAttack only
// needs its seeTime counter to cross a threshold of 20 -- a few ticks of
// cached sight-check staleness can only delay reacting to a sight change by
// that same handful of ticks, well short of either. Unmeasured against a
// real multi-mob-combat frame-time log, same as this file's other knobs (see
// its own banner); revisit once one exists.
#undef  PLATFORM_CAN_SEE_CACHE_TICKS
#define PLATFORM_CAN_SEE_CACHE_TICKS 5

#endif // PLATFORM_DSI
