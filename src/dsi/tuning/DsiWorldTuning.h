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

#endif // PLATFORM_DSI
