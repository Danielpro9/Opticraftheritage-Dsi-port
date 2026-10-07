#include "platform/Log.h"
#include "ChunkProvider.h"

#include <cstdio>
#include <algorithm>
#include <sstream>

#include "World.h"
#include "WorldProvider.h"
#include "Chunk.h"
#include "EmptyChunk.h"
#include "IChunkLoader.h"
#include "IProgressUpdate.h"
#include "ThreadedFileIOBase.h"
#include "ChunkCoordIntPair.h"
#include "ChunkProviderLoadOrGenerate.h"
#include "ChunkProviderGenerate.h"
#include "McRegionChunkLoader.h"
#include "AnvilChunkLoader.h"
#include "java/Arithmetic.h"
#include "java/String.h"
#include "java/System.h"
#include "platform/PlatformTuning.h"
#include "platform/WorldLoadTrace.h"
#include "platform/chunks/ChunkGenerationScheduler.h"
#include "platform/chunks/ChunkMemoryPolicy.h"
#include "platform/world/StreamingFrameBudget.h"
#include "ISaveHandler.h"

#include "platform/Profiler.h"

namespace
{
	int_t clampInt(int_t value, int_t minValue, int_t maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

	// Only the unbounded (desktop) path uses this; PLATFORM_BOUNDED_WORLD takes
	// both radii straight from the tuning table instead of deriving one.
	const int_t CACHE_RADIUS_MARGIN = 4;


#if PLATFORM_DEFERRED_POPULATE
	const int_t POPULATION_FOOTPRINT_AXIS = 2;
	const int_t POPULATION_SECTION_COUNT = 8;
#endif
}

ChunkProvider::ChunkProvider(World *world, IChunkLoader *ichunkloader, IChunkProvider *ichunkprovider)
#if PLATFORM_ASYNC_CHUNK_GENERATION
	: asyncGenerationScheduler(nullptr)
	, droppedChunksSet()
#else
	: droppedChunksSet()
#endif
	, blankChunk(nullptr)
	, chunkProvider(ichunkprovider)
	, chunkLoader(ichunkloader)
	, chunkMap()
	, chunkList()
	, lastChunk(nullptr)
	, lastChunkX(0)
	, lastChunkZ(0)
	, worldObj(world)
	, curChunkX(0)
	, curChunkZ(0)
	, chunkLoadRadius(15)
	, chunkUnloadRadius(15)
	, chunkTopologyVersion(1)
{
	blankChunk = new EmptyChunk(world, std::vector<byte_t>(32768, 0), 0, 0);
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
	generationMoveX = 0;
	generationMoveZ = 0;
	generationCenterInitialized = false;
#endif
#if PLATFORM_BOUNDED_WORLD
	{
		// A resident region (the End) sits on top of the sliding window.
		std::size_t reserve = PLATFORM_CHUNK_MAP_RESERVE;
		const int_t residentRadius = world != nullptr && world->worldProvider != nullptr
			? world->worldProvider->getResidentChunkRadius() : -1;
		if (residentRadius >= 0)
			reserve += static_cast<std::size_t>(residentRadius * 2 + 1) * (residentRadius * 2 + 1);
		chunkMap.reserve(reserve);
		chunkList.reserve(reserve);
	}
	genChunksThisTick = 0;
	setChunkLoadRadius(PLATFORM_CHUNK_CACHE_RADIUS);
#else
	chunkMap.reserve(256);
	chunkList.reserve(256);
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION
	asyncGenerationScheduler = nullptr;
	asyncSavedChunkProbe = nullptr;
	McRegionChunkLoader *asyncRegionLoader = dynamic_cast<McRegionChunkLoader *>(ichunkloader);
	AnvilChunkLoader *asyncAnvilLoader = dynamic_cast<AnvilChunkLoader *>(ichunkloader);
	// The worker reads region files only through McRegionChunkLoader. With the
	// Anvil loader it runs generation alone (a nullptr region loader), and
	// requestChunkDetailed() checks the save first so nothing on disk is ever
	// handed to it. Release worlds are Anvil, so without this branch the
	// worker never existed and every chunk generated on the game thread.
	if (dynamic_cast<ChunkProviderGenerate *>(chunkProvider) != nullptr && worldObj != nullptr &&
	    (asyncRegionLoader != nullptr || asyncAnvilLoader != nullptr))
	{
		asyncSavedChunkProbe = asyncRegionLoader == nullptr ? asyncAnvilLoader : nullptr;
#if PLATFORM_PC_LEGACY || PLATFORM_WII
		// The worker only builds terrain/cave buffers. Structure discovery,
		// decoration, Chunk construction, lighting and publication stay on the
		// game thread: the per-biome BiomeDecorator and the chunk-local
		// decoration scope on World are shared state, and a worker running
		// provideChunk() to completion raced the game thread's own decoration
		// ("Already decorating!!" on Wii).
		asyncGenerationScheduler = new ChunkGenerationScheduler(
			new ChunkProviderGenerate(
				worldObj, worldObj->getRandomSeed(), false,
				PLATFORM_ASYNC_ISOLATED_BIOME_SOURCE != 0),
			asyncRegionLoader, worldObj);
#else
		asyncGenerationScheduler = new ChunkGenerationScheduler(
			new ChunkProviderGenerate(worldObj, worldObj->getRandomSeed()),
			asyncRegionLoader, worldObj);
#endif
		if (!asyncGenerationScheduler->start())
		{
			delete asyncGenerationScheduler;
			asyncGenerationScheduler = nullptr;
			asyncSavedChunkProbe = nullptr;
		}
	}
#endif
}

ChunkProvider::~ChunkProvider()
{
#if PLATFORM_DSI
	// Last-resort net: normally empty by now (saveChunks() already flushes
	// this queue on every exit/autosave path that can reach a destructor),
	// but chunkLoader is about to be deleted below, so any stragglers must be
	// written out (or at least attempted) while it is still valid rather than
	// silently dropped.
	flushPendingSaves();
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION
	delete asyncGenerationScheduler;
	asyncGenerationScheduler = nullptr;
#endif
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
	if (ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider))
		generator->cancelGenerationTask();
	generationQueue.clear();
	generationQueued.clear();
#endif
	std::unordered_set<Chunk *> uniqueChunks;
	uniqueChunks.reserve(chunkMap.size());
	for (auto &entry : chunkMap)
	{
		Chunk *chunk = entry.second;
		if (chunk != nullptr && chunk != blankChunk)
			uniqueChunks.insert(chunk);
	}

	for (Chunk *chunk : uniqueChunks)
		delete chunk;

	chunkMap.clear();
	chunkList.clear();
	droppedChunksSet.clear();

	delete blankChunk;
	delete chunkLoader;
	delete chunkProvider;

	blankChunk = nullptr;
	chunkLoader = nullptr;
	chunkProvider = nullptr;
	worldObj = nullptr;
}

std::uint64_t ChunkProvider::chunkKey(int_t i, int_t j)
{
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32)
	     | static_cast<std::uint32_t>(j);
}

void ChunkProvider::markChunkTopologyChanged()
{
	++chunkTopologyVersion;
	if (chunkTopologyVersion == 0)
		++chunkTopologyVersion;
}

void ChunkProvider::setCurrentChunkOver(int_t i, int_t j)
{
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
	if (generationCenterInitialized)
	{
		const int_t deltaX = JavaArithmetic::intSub(i, curChunkX);
		const int_t deltaZ = JavaArithmetic::intSub(j, curChunkZ);
		if (deltaX != 0 || deltaZ != 0)
		{
			generationMoveX = deltaX > 0 ? 1 : (deltaX < 0 ? -1 : 0);
			generationMoveZ = deltaZ > 0 ? 1 : (deltaZ < 0 ? -1 : 0);
		}
	}
	else
	{
		generationMoveX = 0;
		generationMoveZ = 0;
		generationCenterInitialized = true;
	}
#endif
	curChunkX = i;
	curChunkZ = j;

	// Propagate the player-centred unload origin to the inner provider. Without
	// this the inner ChunkProviderLoadOrGenerate keeps curChunkX/Z at its initial
	// (spawn) value, so its distance-based unload100OldestChunks() never evicts the
	// chunks it generates around the moving player -- they accumulate and leak RAM
	// as you walk (the 32 MB "OOM after ~30 s of moving"). Both cache tiers must be
	// centred on the player. dynamic_cast because setCurrentChunkOver is not on the
	// IChunkProvider interface.
	if (ChunkProviderLoadOrGenerate *inner = dynamic_cast<ChunkProviderLoadOrGenerate *>(chunkProvider))
		inner->setCurrentChunkOver(i, j);
	else if (ChunkProvider *inner2 = dynamic_cast<ChunkProvider *>(chunkProvider))
		inner2->setCurrentChunkOver(i, j);
}

#if PLATFORM_BOUNDED_WORLD || PLATFORM_ASYNC_CHUNK_GENERATION
void ChunkProvider::notifyChunkPublished(Chunk *chunk)
{
	if (worldObj == nullptr || chunk == nullptr || chunk == blankChunk)
		return;

	// A console renderer at the edge of the sliding world cache is allowed to
	// build against EmptyChunk for source chunks outside the current load radius.
	// If that chunk later becomes resident, its completed empty column is stale.
	// Chunk publication is the authoritative transition, so invalidate that
	// column here. Use the interior range because RenderGlobal expands dirty
	// ranges by one block; this reaches the chunk bounds without rebuilding all
	// eight horizontal neighbours for every streamed chunk. Active neighbouring
	// builds detect source availability changes themselves in WorldRenderer.
	const int_t minX = JavaArithmetic::intMul(chunk->xPosition, 16);
	const int_t minZ = JavaArithmetic::intMul(chunk->zPosition, 16);
	worldObj->markBlocksDirty(minX + 1, 1, minZ + 1,
	                          minX + 14, 126, minZ + 14);
#if PLATFORM_PS2
	worldObj->notifyChunkPublishedForRender(chunk->xPosition, chunk->zPosition);
#endif
}
#endif

void ChunkProvider::setChunkLoadRadius(int_t radius)
{
	// Use a slightly larger RAM cache than the strict visible radius.  This avoids
	// unloading/reloading the same chunks when the player moves a few blocks,
	// without going back to the old fixed 1024-slot cache.
#if PLATFORM_BOUNDED_WORLD
	(void)radius;
	ISaveHandler *saveHandler = worldObj != nullptr ? worldObj->getSaveHandler() : nullptr;
	const ChunkMemoryPolicy::RetentionPolicy policy =
		ChunkMemoryPolicy::retentionPolicy(saveHandler != nullptr && saveHandler->isReadOnly());
	chunkLoadRadius = policy.loadRadius;
	chunkUnloadRadius = policy.unloadRadius;
#elif PLATFORM_PC_LEGACY
	(void)radius;
	chunkLoadRadius = PLATFORM_CHUNK_CACHE_RADIUS;
	chunkUnloadRadius = PLATFORM_CHUNK_UNLOAD_RADIUS;
#else
	chunkLoadRadius = clampInt(radius, 2, 15);
	chunkUnloadRadius = clampInt(chunkLoadRadius + CACHE_RADIUS_MARGIN, chunkLoadRadius, 15);
#endif
}

void ChunkProvider::setChunkLoadRadiusFromRenderDistance(int_t renderDistance)
{
#if PLATFORM_BOUNDED_WORLD || PLATFORM_PC_LEGACY
	(void)renderDistance;
	setChunkLoadRadius(0);
#else
	renderDistance &= 3;
	int_t blocks = 64 << (3 - renderDistance);
	if (blocks > 400)
		blocks = 400;

	const int_t renderChunksWide = blocks / 16 + 1;
	setChunkLoadRadius(renderChunksWide / 2 + 2);
#endif
}

bool ChunkProvider::canChunkExist(int_t i, int_t j) const
{
	// Ported from upstream OptiCraft Heritage Edition's finite "Classic
	// 256x256" world-size option (WorldInfo::isLimitedWorld(), set from
	// GuiCreateWorld/LegacyCreateWorldScreen's world-size toggle): a hard,
	// absolute +-8 chunk (256x256 block) border, layered on top of this
	// fork's own sliding chunk-cache-window logic below -- a chunk inside
	// the current streaming window is still rejected if it falls outside
	// the world's finite border. Unrelated to, and does not replace,
	// PLATFORM_BOUNDED_WORLD (this fork's own DSi/PS2 chunk-cache-window
	// mechanism, active on every world regardless of size); a name
	// collision between the two is coincidental.
	if (worldObj != nullptr && worldObj->isLimitedWorld())
	{
		if (i < -8 || i > 7 || j < -8 || j > 7)
			return false;
	}
	const int_t minX = JavaArithmetic::intSub(curChunkX, chunkLoadRadius);
	const int_t minZ = JavaArithmetic::intSub(curChunkZ, chunkLoadRadius);
	const int_t maxX = JavaArithmetic::intAdd(curChunkX, chunkLoadRadius);
	const int_t maxZ = JavaArithmetic::intAdd(curChunkZ, chunkLoadRadius);
	if (i >= minX && j >= minZ && i <= maxX && j <= maxZ)
		return true;
	if (worldObj != nullptr && worldObj->isChunkResident(i, j))
		return true;
#if PLATFORM_ENTITY_CHUNK_RETENTION
	return worldObj != nullptr && worldObj->isChunkRetainedByEntity(i, j);
#else
	return false;
#endif
}

long_t ChunkProvider::currentWorldTime() const
{
	return worldObj != nullptr ? worldObj->getWorldTime() : 0LL;
}

bool ChunkProvider::isOutsideUnloadRadius(int_t i, int_t j) const
{
	if (worldObj != nullptr && worldObj->isChunkResident(i, j))
		return false;
#if PLATFORM_ENTITY_CHUNK_RETENTION
	if (worldObj != nullptr && worldObj->isChunkRetainedByEntity(i, j))
		return false;
#endif
	const long_t dx = static_cast<long_t>(i) - static_cast<long_t>(curChunkX);
	const long_t dz = static_cast<long_t>(j) - static_cast<long_t>(curChunkZ);
	const long_t radius = static_cast<long_t>(chunkUnloadRadius);
	return dx < -radius || dz < -radius || dx > radius || dz > radius;
}

bool ChunkProvider::chunkExists(int_t i, int_t j)
{
	return chunkMap.count(chunkKey(i, j)) != 0;
}

Chunk *ChunkProvider::getChunkIfExists(int_t i, int_t j)
{
	const std::uint64_t key = chunkKey(i, j);
	auto it = chunkMap.find(key);
	if (it == chunkMap.end())
	{
#if PLATFORM_DSI
		// See reclaimPendingSave()'s own comment: a chunk whose deferred
		// unload-save has not landed yet is not in chunkMap, but it is also
		// not gone -- hand the same live object back instead of reporting
		// "does not exist".
		if (Chunk *reclaimed = reclaimPendingSave(key))
			return reclaimed;
#endif
		return nullptr;
	}

#if PLATFORM_BOUNDED_WORLD
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return blankChunk;
#endif
	if (lastChunk != nullptr && i == lastChunkX && j == lastChunkZ)
		return lastChunk;

	Chunk *chunk = it->second;
	if (chunk != nullptr)
		chunk->lastAccessTick = currentWorldTime();
	if (chunk != nullptr && chunk != blankChunk)
	{
		lastChunk = chunk;
		lastChunkX = i;
		lastChunkZ = j;
	}
	return chunk;
}

#if PLATFORM_ASYNC_CHUNK_GENERATION
ChunkProvider::ChunkRequestStatus ChunkProvider::requestChunkDetailed(int_t i, int_t j)
{
	if (asyncGenerationScheduler == nullptr || !asyncGenerationScheduler->active())
		return ChunkRequestStatus::Inactive;
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return ChunkRequestStatus::OutOfRange;
	if (chunkMap.count(chunkKey(i, j)) != 0)
		return ChunkRequestStatus::AlreadyLoaded;
	if (asyncSavedChunkProbe != nullptr && asyncSavedChunkProbe->isChunkSaved(i, j))
		return ChunkRequestStatus::SavedOnDisk;

	switch (asyncGenerationScheduler->requestDetailed(i, j, PLATFORM_ASYNC_GENERATION_QUEUE_LIMIT))
	{
	case ChunkGenerationScheduler::RequestStatus::Accepted:
		return ChunkRequestStatus::Accepted;
	case ChunkGenerationScheduler::RequestStatus::AlreadyQueued:
		return ChunkRequestStatus::AlreadyQueued;
	case ChunkGenerationScheduler::RequestStatus::QueueFull:
		return ChunkRequestStatus::QueueFull;
	case ChunkGenerationScheduler::RequestStatus::Inactive:
	default:
		return ChunkRequestStatus::Inactive;
	}
}

bool ChunkProvider::requestChunk(int_t i, int_t j)
{
	return requestChunkDetailed(i, j) == ChunkRequestStatus::Accepted;
}

void ChunkProvider::serviceAsyncChunkStreaming()
{
	if (asyncGenerationScheduler == nullptr || !asyncGenerationScheduler->active())
		return;

	asyncGenerationScheduler->setFocus(curChunkX, curChunkZ);
	if (PLATFORM_ASYNC_GENERATION_PUBLISH_PER_FRAME > 0)
		drainAsyncGeneratedChunks(PLATFORM_ASYNC_GENERATION_PUBLISH_PER_FRAME);
	if (PLATFORM_ASYNC_GENERATION_REQUESTS_PER_FRAME > 0)
		drainAsyncGenerationRequests(PLATFORM_ASYNC_GENERATION_REQUESTS_PER_FRAME);
}

bool ChunkProvider::acceptAsyncGenerationCoordinate(void* context, int_t i, int_t j)
{
	ChunkProvider* self = static_cast<ChunkProvider*>(context);
	if (self == nullptr)
		return false;
	if (self->chunkMap.count(chunkKey(i, j)) != 0)
		return false;
	return self->worldObj == nullptr || self->worldObj->findingSpawnPoint || self->canChunkExist(i, j);
}

bool ChunkProvider::drainAsyncGenerationRequests(int_t budget)
{
	return asyncGenerationScheduler != nullptr &&
		asyncGenerationScheduler->dispatch(budget, &ChunkProvider::acceptAsyncGenerationCoordinate, this);
}

bool ChunkProvider::drainAsyncGeneratedChunks(int_t budget)
{
	if (asyncGenerationScheduler == nullptr || budget <= 0)
		return false;

	bool published = false;
	for (int_t n = 0; n < budget; ++n)
	{
		ChunkGenerationScheduler::Result result;
		if (!asyncGenerationScheduler->popResult(result))
			break;

		const std::uint64_t key = chunkKey(result.x, result.z);
		const bool wanted = chunkMap.count(key) == 0
			&& (worldObj == nullptr || worldObj->findingSpawnPoint || canChunkExist(result.x, result.z));

		Chunk *chunk = nullptr;
		if (wanted)
		{
			switch (result.kind)
			{
			case ChunkGenerationScheduler::ResultKind::LoadedData:
			{
				McRegionChunkLoader* regionLoader = dynamic_cast<McRegionChunkLoader*>(chunkLoader);
				if (regionLoader != nullptr)
				{
					ChunkLoadStatus loadStatus = ChunkLoadStatus::ReadError;
					chunk = regionLoader->loadChunkFromData(worldObj, result.x, result.z, result.data, &loadStatus);
					if (chunk != nullptr)
						chunk->lastSaveTime = currentWorldTime();
					else if (loadStatus == ChunkLoadStatus::ReadError)
						chunk = blankChunk;
				}
				break;
			}
			case ChunkGenerationScheduler::ResultKind::LoadedChunk:
				chunk = result.chunk;
				result.chunk = nullptr;
				McRegionChunkLoader::attachChunkEntities(worldObj, chunk, result.nbt.get());
				chunk->lastSaveTime = currentWorldTime();
				break;
			case ChunkGenerationScheduler::ResultKind::GeneratedData:
			{
				ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider);
				if (generator != nullptr)
					chunk = generator->finishAsyncChunkData(result.x, result.z, result.data);
				break;
			}
			case ChunkGenerationScheduler::ResultKind::Generated:
				chunk = result.chunk;
				result.chunk = nullptr;
				break;
			case ChunkGenerationScheduler::ResultKind::ReadError:
				chunk = blankChunk;
				break;
			}
		}

		if (chunk != nullptr)
		{
			published = true;
			chunkMap[key] = chunk;
			markChunkTopologyChanged();
			chunkList.push_back(chunk);
			chunk->lastAccessTick = currentWorldTime();
			if (chunk != blankChunk)
			{
				chunk->onChunkLoadData();
				chunk->onChunkLoad();
				notifyChunkPublished(chunk);
#if PLATFORM_DEFERRED_POPULATE
				const int_t westX = JavaArithmetic::intSub(result.x, 1);
				const int_t northZ = JavaArithmetic::intSub(result.z, 1);
				enqueuePopulate(result.x, result.z);
				enqueuePopulate(westX, result.z);
				enqueuePopulate(result.x, northZ);
				enqueuePopulate(westX, northZ);
#endif
			}
		}

		delete result.chunk;
		asyncGenerationScheduler->complete(result.x, result.z);
	}
	return published;
}
#endif

void ChunkProvider::publishPreparedChunk(int_t i, int_t j, Chunk *chunk)
{
	if (chunk == nullptr)
		return;

	const std::uint64_t key = chunkKey(i, j);
	chunkMap[key] = chunk;
	markChunkTopologyChanged();
	chunkList.push_back(chunk);
	chunk->lastAccessTick = currentWorldTime();
	chunk->onChunkLoadData();
	chunk->onChunkLoad();

#if PLATFORM_BOUNDED_WORLD
	if (chunk != blankChunk)
		notifyChunkPublished(chunk);
#endif

	if (chunk == blankChunk)
		return;

	const int_t eastX = JavaArithmetic::intAdd(i, 1);
	const int_t westX = JavaArithmetic::intSub(i, 1);
	const int_t southZ = JavaArithmetic::intAdd(j, 1);
	const int_t northZ = JavaArithmetic::intSub(j, 1);
#if PLATFORM_DEFERRED_POPULATE
	const bool deferPopulate =
#if PLATFORM_PC_LEGACY
		worldObj == nullptr || !worldObj->findingSpawnPoint;
#else
		true;
#endif
	if (deferPopulate)
	{
		enqueuePopulate(i, j);
		enqueuePopulate(westX, j);
		enqueuePopulate(i, northZ);
		enqueuePopulate(westX, northZ);
		return;
	}
#endif

	if (!chunk->isTerrainPopulated
		&& chunkExists(eastX, southZ)
		&& chunkExists(i, southZ)
		&& chunkExists(eastX, j))
	{
		populate(this, i, j);
	}
	if (chunkExists(westX, j) && !provideChunk(westX, j)->isTerrainPopulated
		&& chunkExists(westX, southZ)
		&& chunkExists(i, southZ)
		&& chunkExists(westX, j))
	{
		populate(this, westX, j);
	}
	if (chunkExists(i, northZ) && !provideChunk(i, northZ)->isTerrainPopulated
		&& chunkExists(eastX, northZ)
		&& chunkExists(i, northZ)
		&& chunkExists(eastX, j))
	{
		populate(this, i, northZ);
	}
	if (chunkExists(westX, northZ) && !provideChunk(westX, northZ)->isTerrainPopulated
		&& chunkExists(westX, northZ)
		&& chunkExists(i, northZ)
		&& chunkExists(westX, j))
	{
		populate(this, westX, northZ);
	}
}

Chunk *ChunkProvider::prepareChunk(int_t i, int_t j)
{
	return prepareChunkInternal(i, j, false);
}

Chunk *ChunkProvider::prepareChunkInternal(int_t i, int_t j, bool deferGeneration)
{
#if !PLATFORM_INCREMENTAL_CHUNK_GENERATION
	(void)deferGeneration;
#endif
#if PLATFORM_BOUNDED_WORLD
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return blankChunk;
#endif
	const std::uint64_t key = chunkKey(i, j);
	droppedChunksSet.erase(key);

	auto it = chunkMap.find(key);
	Chunk *chunk = (it != chunkMap.end()) ? it->second : nullptr;
	if (chunk != nullptr)
	{
		chunk->lastAccessTick = currentWorldTime();
		return chunk;
	}

	WORLD_LOAD_STAGE("prepareChunk");
	bool readFailed = false;
#if PLATFORM_PROFILE_STREAMING
	const long_t chunkLoadStartNs = System::nanoTime();
#endif
	WorldLoadTrace::step("loadChunkFromFile");
	chunk = loadChunkFromFile(i, j, readFailed);
#if PLATFORM_PROFILE_STREAMING
	platformProfileChunkLoad(System::nanoTime() - chunkLoadStartNs);
#endif
	if (chunk == nullptr && readFailed)
	{
		MC_LOG_ERROR("chunk", "ChunkProvider: refusing to regenerate unreadable chunk %d,%d\n", i, j);
		chunk = blankChunk;
	}
	else if (chunk == nullptr)
	{
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
		if (deferGeneration)
		{
			enqueueGeneration(i, j);
			return blankChunk;
		}
#endif
		if (chunkProvider == nullptr)
		{
			chunk = blankChunk;
		}
		else
		{
#if PLATFORM_PROFILE_STREAMING
			const long_t generateStartNs = System::nanoTime();
#endif
			WorldLoadTrace::step("generate");
			chunk = chunkProvider->provideChunk(i, j);
#if PLATFORM_PROFILE_STREAMING
			platformProfileGenerate(System::nanoTime() - generateStartNs);
#endif
		}
	}
	if (chunk == nullptr)
		chunk = blankChunk;

	publishPreparedChunk(i, j, chunk);
	return chunk;
}

#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
void ChunkProvider::enqueueGeneration(int_t i, int_t j)
{
	const std::uint64_t key = chunkKey(i, j);
	if (generationQueued.insert(key).second)
		generationQueue.emplace_back(i, j);
}

void ChunkProvider::cancelQueuedGeneration(int_t i, int_t j)
{
	const std::uint64_t key = chunkKey(i, j);
	generationQueued.erase(key);
	generationQueue.erase(
		std::remove_if(generationQueue.begin(), generationQueue.end(),
			[i, j](const std::pair<int_t, int_t> &coord)
			{
				return coord.first == i && coord.second == j;
			}),
		generationQueue.end());

	ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider);
	if (generator != nullptr && generator->hasGenerationTask()
		&& generator->generationTaskX() == i && generator->generationTaskZ() == j)
	{
		generator->cancelGenerationTask();
	}
}

int_t ChunkProvider::countLoadedGenerationNeighbours(int_t i, int_t j) const
{
	int_t loaded = 0;
	for (int_t dx = -1; dx <= 1; ++dx)
	{
		for (int_t dz = -1; dz <= 1; ++dz)
		{
			if (dx == 0 && dz == 0)
				continue;

			const int_t neighbourX = JavaArithmetic::intAdd(i, dx);
			const int_t neighbourZ = JavaArithmetic::intAdd(j, dz);
			auto it = chunkMap.find(chunkKey(neighbourX, neighbourZ));
			if (it != chunkMap.end() && it->second != nullptr && it->second != blankChunk)
				++loaded;
		}
	}
	return loaded;
}

std::deque<std::pair<int_t, int_t>>::iterator ChunkProvider::selectNextGenerationCoordinate()
{
	auto best = generationQueue.begin();
	if (best == generationQueue.end())
		return best;

	auto chebyshevDistance = [this](const std::pair<int_t, int_t> &coord)
	{
		long_t dx = static_cast<long_t>(coord.first) - static_cast<long_t>(curChunkX);
		long_t dz = static_cast<long_t>(coord.second) - static_cast<long_t>(curChunkZ);
		if (dx < 0) dx = -dx;
		if (dz < 0) dz = -dz;
		return dx > dz ? dx : dz;
	};

	auto movementAlignment = [this](const std::pair<int_t, int_t> &coord)
	{
		const long_t dx = static_cast<long_t>(coord.first) - static_cast<long_t>(curChunkX);
		const long_t dz = static_cast<long_t>(coord.second) - static_cast<long_t>(curChunkZ);
		return dx * static_cast<long_t>(generationMoveX)
		     + dz * static_cast<long_t>(generationMoveZ);
	};

	long_t bestDistance = chebyshevDistance(*best);
	for (auto it = generationQueue.begin() + 1; it != generationQueue.end(); ++it)
	{
		const long_t distance = chebyshevDistance(*it);
		if (distance < bestDistance)
		{
			best = it;
			bestDistance = distance;
		}
	}

	int_t bestNeighbours = countLoadedGenerationNeighbours(best->first, best->second);
	long_t bestAlignment = movementAlignment(*best);
	for (auto it = generationQueue.begin(); it != generationQueue.end(); ++it)
	{
		if (it == best || chebyshevDistance(*it) != bestDistance)
			continue;

		const int_t neighbours = countLoadedGenerationNeighbours(it->first, it->second);
		if (neighbours < bestNeighbours)
			continue;

		const long_t alignment = movementAlignment(*it);
		if (neighbours > bestNeighbours || alignment > bestAlignment)
		{
			best = it;
			bestNeighbours = neighbours;
			bestAlignment = alignment;
		}
	}

	return best;
}

bool ChunkProvider::drainPendingGeneration(int_t stepBudget, bool &publishedChunk)
{
	return drainPendingGeneration(stepBudget,
		static_cast<long_t>(PLATFORM_GENERATION_BUDGET_US) * 1000LL, publishedChunk);
}

void ChunkProvider::serviceFrameGeneration()
{
	if (PLATFORM_GENERATION_STEPS_PER_FRAME <= 0)
		return;
	bool publishedChunk = false;
	drainPendingGeneration(PLATFORM_GENERATION_STEPS_PER_FRAME,
		static_cast<long_t>(PLATFORM_GENERATION_FRAME_BUDGET_US) * 1000LL, publishedChunk);
}

bool ChunkProvider::drainPendingGeneration(int_t stepBudget, long_t budgetNs, bool &publishedChunk)
{
	// publishedChunk reports the subset of the work below that actually adds a
	// chunk to chunkMap, which is a different question from the return value.
	// See the decoration gate in unload100OldestChunks().
	publishedChunk = false;
	if (stepBudget <= 0)
		return false;

	ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider);
	if (generator == nullptr)
		return false;

	bool didWork = false;
	int_t steps = 0;
	// Both slices (tick and frame) come through here, so the shared frame
	// allowance is applied once, in one place.
	PlatformStreamingFrameBudgetScope frameBudgetScope;
	budgetNs = PlatformStreamingFrameBudget::clampUs(budgetNs / 1000LL) * 1000LL;
	const long_t budgetStartNs = budgetNs > 0 ? System::nanoTime() : 0;

	while (steps < stepBudget)
	{
		if (!generator->hasGenerationTask())
		{
			bool started = false;
			while (!generationQueue.empty())
			{
				auto coordIt = selectNextGenerationCoordinate();
				if (coordIt == generationQueue.end())
					break;
				const std::pair<int_t, int_t> coord = *coordIt;
				generationQueue.erase(coordIt);
				const std::uint64_t key = chunkKey(coord.first, coord.second);
				if (generationQueued.count(key) == 0)
					continue;

				const bool wanted = chunkMap.count(key) == 0
					&& (worldObj == nullptr || worldObj->findingSpawnPoint
						|| canChunkExist(coord.first, coord.second));
				if (!wanted)
				{
					generationQueued.erase(key);
					continue;
				}

				started = generator->beginGenerationTask(coord.first, coord.second);
				if (started)
					break;
				generationQueue.emplace_back(coord);
				return didWork;
			}
			if (!started)
				break;
		}

		const int_t taskX = generator->generationTaskX();
		const int_t taskZ = generator->generationTaskZ();
		const std::uint64_t key = chunkKey(taskX, taskZ);
		const bool wanted = chunkMap.count(key) == 0
			&& (worldObj == nullptr || worldObj->findingSpawnPoint || canChunkExist(taskX, taskZ));
		if (!wanted)
		{
			generator->cancelGenerationTask();
			generationQueued.erase(key);
			continue;
		}

#if PLATFORM_DSI
		// Diagnostic only, not a fix: chasing an uninstrumented "worldTick"
		// spike (up to 467ms in a real-hardware log) that TickUpdates()'s own
		// wall-clock budget cannot explain and this session's chunk-unload-
		// save diagnostic (ChunkProvider::unloadChunk()) never fired for in
		// that same log -- ruling out the only two previously-suspected
		// causes. Ps2WorldTuning.h's own PS2_GENERATION_SOURCE_COLUMNS_PER_
		// STEP comment already names the remaining structural gap: the
		// per-call wall-clock budget below (PLATFORM_GENERATION_BUDGET_US)
		// "can only stop the generator BETWEEN steps... a step that is
		// internally atomic sets the floor." Caves/Ravines are sliced by
		// source column, but "one [column] that passes carves a WHOLE
		// TUNNEL" in that one still-atomic step regardless -- cheap enough
		// to not need further attention on PS2's EE, not necessarily on an
		// FPU-less ARM9. One nanoTime() pair around the single call this
		// session's other wall-clock budgets already treat as the smallest
		// unit says definitively whether this is where the spike's time
		// goes, and roughly how much of it, without guessing further.
		const long_t dsiStepStartNs = System::nanoTime();
		// Captured before advanceGenerationTask() runs: the task's stage field
		// is advanced to the NEXT stage once the current one finishes, so this
		// is "which stage just ran", not "which stage runs next".
		const char *dsiStepStageName = generator->generationTaskStageName();
#endif
		if (!generator->advanceGenerationTask())
			break;
#if PLATFORM_DSI
		{
			const long_t dsiStepUs = (System::nanoTime() - dsiStepStartNs) / 1000;
			if (dsiStepUs >= 20000)
				MC_LOG_INFO("dsi", "generation step (%s) at %d,%d took %lldus\n",
					dsiStepStageName, (int)taskX, (int)taskZ, (long long)dsiStepUs);
		}
#endif
		didWork = true;
		++steps;

		Chunk *completed = generator->takeGeneratedChunk();
		if (completed != nullptr)
		{
			generationQueued.erase(key);
			const bool stillWanted = chunkMap.count(key) == 0
				&& (worldObj == nullptr || worldObj->findingSpawnPoint || canChunkExist(taskX, taskZ));
			if (stillWanted)
			{
				publishPreparedChunk(taskX, taskZ, completed);
				publishedChunk = true;
			}
			else
			{
				delete completed;
			}
		}

		if (budgetNs > 0 && System::nanoTime() - budgetStartNs >= budgetNs)
			break;
	}
	return didWork;
}

bool ChunkProvider::isChunkGenerationPending(int_t i, int_t j) const
{
	return generationQueued.count(chunkKey(i, j)) != 0;
}
#endif

Chunk *ChunkProvider::provideChunk(int_t i, int_t j)
{
#if PLATFORM_BOUNDED_WORLD
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return blankChunk;
#endif
	// Fast path: same chunk as the previous lookup. Skips the chunkMap hash
	// lookup entirely and, same as before, does not bother updating the access
	// timestamp here -- a chunk only reached through this path is the one the
	// player is standing in, always inside the unload radius regardless of how
	// stale lastAccessTick is. Last-access tracking lives on the Chunk itself
	// now (see Chunk::lastAccessTick), not a side map keyed by chunk coordinate.
	if (lastChunk != nullptr && i == lastChunkX && j == lastChunkZ)
		return lastChunk;

	const std::uint64_t key = chunkKey(i, j);
	auto it = chunkMap.find(key);
	if (it == chunkMap.end())
	{
#if PLATFORM_DSI
		// See reclaimPendingSave()'s own comment: a chunk whose deferred
		// unload-save has not landed yet is not in chunkMap, but it is also
		// not gone -- hand the same live object back instead of regenerating
		// or reloading the last-saved (pre-edit) copy from disk underneath
		// the write this queue still owes it.
		if (Chunk *reclaimed = reclaimPendingSave(key))
			return reclaimed;
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION && PLATFORM_PC_LEGACY
		if (worldObj == nullptr || !worldObj->findingSpawnPoint)
		{
			long_t dcx = static_cast<long_t>(i) - static_cast<long_t>(curChunkX);
			long_t dcz = static_cast<long_t>(j) - static_cast<long_t>(curChunkZ);
			if (dcx < 0) dcx = -dcx;
			if (dcz < 0) dcz = -dcz;
			const long_t cheb = dcx > dcz ? dcx : dcz;
			const bool critical = cheb <= PLATFORM_GENERATE_SYNC_RADIUS;
			if (!critical && asyncGenerationScheduler != nullptr && asyncGenerationScheduler->active())
			{
				const ChunkRequestStatus requestStatus = requestChunkDetailed(i, j);
				if (requestStatus == ChunkRequestStatus::Accepted ||
					requestStatus == ChunkRequestStatus::AlreadyQueued ||
					requestStatus == ChunkRequestStatus::QueueFull)
					return blankChunk;
			}
		}
#endif
#if PLATFORM_BOUNDED_WORLD && PLATFORM_GENERATE_CHUNKS_PER_TICK > 0
		if (worldObj == nullptr || !worldObj->findingSpawnPoint)
		{
			long_t dcx = static_cast<long_t>(i) - static_cast<long_t>(curChunkX);
			long_t dcz = static_cast<long_t>(j) - static_cast<long_t>(curChunkZ);
			if (dcx < 0) dcx = -dcx;
			if (dcz < 0) dcz = -dcz;
			const long_t cheb = dcx > dcz ? dcx : dcz;
			bool critical = cheb <= PLATFORM_GENERATE_SYNC_RADIUS;
#if PLATFORM_ENTITY_CHUNK_RETENTION
			critical = critical || (worldObj != nullptr && worldObj->isChunkRequiredByRetainedEntity(i, j));
#endif
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
			ChunkProviderGenerate *incrementalGenerator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider);
			if (incrementalGenerator != nullptr)
			{
				if (critical)
				{
					cancelQueuedGeneration(i, j);
				}
				else
				{
					if (generationQueued.count(key) != 0)
						return blankChunk;
					return prepareChunkInternal(i, j, true);
				}
			}
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION
			// Non-critical terrain can be queued on a low-priority generation service.
			// Minecraft keeps rendering the current frame until the completed chunk is
			// published on a later tick.
			if (!critical && asyncGenerationScheduler != nullptr && asyncGenerationScheduler->active())
			{
				const ChunkRequestStatus requestStatus = requestChunkDetailed(i, j);
				if (requestStatus == ChunkRequestStatus::Accepted ||
					requestStatus == ChunkRequestStatus::AlreadyQueued)
					return blankChunk;
			}
#endif
			if (!critical && genChunksThisTick >= PLATFORM_GENERATE_CHUNKS_PER_TICK)
				return blankChunk;
			genChunksThisTick++;
		}
#endif
		return prepareChunk(i, j);
	}

	if (it->second != nullptr)
		it->second->lastAccessTick = currentWorldTime();
	if (it->second != nullptr && it->second != blankChunk)
	{
		lastChunk  = it->second;
		lastChunkX = i;
		lastChunkZ = j;
	}
	return it->second;
}

Chunk *ChunkProvider::loadChunkFromFile(int_t i, int_t j, bool &readFailed)
{
	readFailed = false;
	if (chunkLoader == nullptr) return nullptr;
	try
	{
		ChunkLoadStatus status = ChunkLoadStatus::Missing;
		Chunk *chunk = chunkLoader->loadChunk(worldObj, i, j, &status);
		readFailed = status == ChunkLoadStatus::ReadError;
		if (chunk != nullptr)
			chunk->lastSaveTime = worldObj->getWorldTime();
		return chunk;
	}
	catch (...)
	{
		readFailed = true;
		MC_LOG_ERROR("chunk", "ChunkProvider::loadChunkFromFile - exception loading %d,%d\n", i, j);
	}
	return nullptr;
}

void ChunkProvider::saveExtraChunkData(Chunk *chunk)
{
	if (chunkLoader == nullptr || chunk == nullptr || chunk == blankChunk) return;
	try
	{
		chunkLoader->saveExtraChunkData(worldObj, chunk);
	}
	catch (...)
	{
		MC_LOG_ERROR("chunk", "ChunkProvider::saveExtraChunkData - exception\n");
	}
}

void ChunkProvider::saveChunkToFile(Chunk *chunk)
{
	if (chunkLoader == nullptr || chunk == nullptr || chunk == blankChunk) return;
	try
	{
		chunk->lastSaveTime = worldObj->getWorldTime();
		chunkLoader->saveChunk(worldObj, chunk);
	}
	catch (...)
	{
		MC_LOG_ERROR("chunk", "ChunkProvider::saveChunkToFile - exception\n");
	}
}

void ChunkProvider::unloadChunk(std::uint64_t key, Chunk *chunk)
{
	(void)key; // no longer needed: last-access tracking moved onto Chunk itself
	if (chunk == nullptr || chunk == blankChunk)
		return;

	// Drop the single-entry cache if it points at the chunk being freed.
	if (lastChunk == chunk)
		lastChunk = nullptr;

#if PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_UNLOAD
	// PS2 serializes only gameplay-edited chunks here. Generated/lighting-only
	// dirtiness stays memory-only so walking does not create continuous writes.
	if (!chunk->neverSave && chunk->isRuntimeSaveRequired())
	{
#if PLATFORM_PROFILE_STREAMING
		const long_t unloadSaveStartNs = System::nanoTime();
#endif
#if PLATFORM_DSI
		// Real-hardware evidence (the diagnostic right below this comment,
		// added to measure exactly this call): this synchronous NBT-serialize
		// -plus-SD-write averaged 262ms and peaked at 805ms across a real
		// exploration session, with up to three landing back-to-back in one
		// tick during an emergency chunk-unload burst (ChunkMemoryPolicy's
		// PLATFORM_EMERGENCY_CHUNK_UNLOADS_PER_TICK) -- 2.2 seconds of
		// straight blocking in one observed case. Region-file fragmentation
		// was ruled out (a freshly-created world showed the same cost).
		// Deferring the write itself (below) rather than lowering the
		// emergency limit keeps that limit's actual job -- bounding how many
		// chunks sit fully resident in chunkMap/chunkList -- intact.
		//
		// Deliberately NOT calling chunk->onChunkUnload() here before queuing
		// it, unlike the old inline save below -- that call deletes the
		// chunk's own TileEntity objects (chests, furnaces, ...), and the
		// vanilla order was always save-first-then-unload specifically so
		// their NBT gets written before they are destroyed. Queuing the
		// now-detached-from-chunkMap Chunk* as-is keeps that same order, just
		// with "later" instead of "immediately" between the two; onChunkUnload()
		// runs in drainPendingSaves()/flushPendingSaves(), right after the
		// deferred save actually happens, same relative order as before.
		//
		// The caller (unload100OldestChunks()) erases this chunk from
		// chunkMap/chunkList unconditionally right after this function
		// returns, exactly as it always did -- so if the player walks back
		// into this exact position before the queued save lands, provideChunk()
		// /getChunkIfExists() would otherwise find nothing and reload the
		// last-saved (pre-edit) copy from disk, racing the pending write and
		// silently losing whichever edit loses that race. reclaimPendingSave()
		// (called from both of those on a chunkMap miss) checks this queue
		// first and hands the same still-live Chunk* back instead, which
		// cancels the now-unnecessary deferred save by construction (the
		// reclaimed chunk simply unloads and re-queues normally, next time,
		// if it is still dirty then).
		pendingSaveQueue.push_back(chunk);
		// Absolute cap, not expected to matter in practice (see
		// drainPendingSaves()'s own per-tick rate vs. how rarely a dirty
		// chunk unloads in the real-hardware logs this was measured against):
		// if something pathological ever outpaces the drain, fall back to the
		// old inline cost for the overflow rather than let queued Chunk
		// objects grow the heap without bound.
		constexpr std::size_t PENDING_SAVE_QUEUE_HARD_CAP = 16;
		while (pendingSaveQueue.size() > PENDING_SAVE_QUEUE_HARD_CAP)
		{
			Chunk *overflow = pendingSaveQueue.front();
			pendingSaveQueue.pop_front();
			saveChunkToFile(overflow);
			overflow->isModified = false;
			overflow->clearRuntimeSaveRequired();
			saveExtraChunkData(overflow);
			overflow->onChunkUnload();
			delete overflow;
		}
		return;
#else
		saveChunkToFile(chunk);
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		saveExtraChunkData(chunk);
#if PLATFORM_PROFILE_STREAMING
		platformProfileUnloadSave(System::nanoTime() - unloadSaveStartNs);
#endif
#endif
	}
#elif !PLATFORM_CONSOLE_LOW
	// Unloading must not force a disk write for clean chunks. The v10 cache
	// optimization saved every chunk on eviction, which caused heavy IO spikes
	// while walking. Save only chunks that Java would consider dirty/stale.
	if (!chunk->neverSave && chunk->needsSaving(false))
	{
#if PLATFORM_PROFILE_STREAMING
		const long_t unloadSaveStartNs = System::nanoTime();
#endif
		saveChunkToFile(chunk);
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		saveExtraChunkData(chunk);
#if PLATFORM_PROFILE_STREAMING
		platformProfileUnloadSave(System::nanoTime() - unloadSaveStartNs);
#endif
	}
#endif

	chunk->onChunkUnload();
	delete chunk;
}

#if PLATFORM_DSI
// Writes out at most `budget` queued chunks (same 1/tick this platform
// already ran unload-saves at before they were deferred -- see
// PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK -- so this does not raise the SD
// commit rate, only decouples it from the exact tick a dirty chunk crossed
// the unload radius). Called once per tick from unload100OldestChunks()
// regardless of whether that tick unloaded anything new, so a backlog from
// an emergency burst keeps draining on quiet ticks too.
void ChunkProvider::drainPendingSaves(int_t budget)
{
	for (int_t i = 0; i < budget && !pendingSaveQueue.empty(); ++i)
	{
		Chunk *chunk = pendingSaveQueue.front();
		pendingSaveQueue.pop_front();
		const long_t startNs = System::nanoTime();
		saveChunkToFile(chunk);
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		saveExtraChunkData(chunk);
		MC_LOG_INFO("dsi", "deferred chunk save took %lldus (queue depth now %u)\n",
			(long long)((System::nanoTime() - startNs) / 1000), (unsigned)pendingSaveQueue.size());
		chunk->onChunkUnload();
		delete chunk;
	}
}

// Full/exit saves (World::saveChunks(true, ...), the "Saving world" screen)
// walk chunkList for dirty chunks -- but a chunk in this queue was already
// removed from chunkList the moment it was queued, so without this it would
// be silently skipped and the pending edit lost on quit. Called
// unconditionally at the top of ChunkProvider::saveChunks() regardless of
// the full/autosave flag, and again from the destructor as a last-resort net
// if the provider is torn down some other way.
void ChunkProvider::flushPendingSaves()
{
	while (!pendingSaveQueue.empty())
	{
		Chunk *chunk = pendingSaveQueue.front();
		pendingSaveQueue.pop_front();
		saveChunkToFile(chunk);
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		saveExtraChunkData(chunk);
		chunk->onChunkUnload();
		delete chunk;
	}
}

// Called from provideChunk()/getChunkIfExists() on a chunkMap miss, before
// either falls through to loading/regenerating from disk. See the comment in
// unloadChunk() on why: the queued chunk is still the live, fully up to date
// object (its deferred save just has not reached the SD card yet), so handing
// it straight back -- same as a normal chunkMap hit -- is both cheaper than a
// disk round trip and the only way to not race that pending write with
// whatever the player does next. The queue is small (capped at 16, typically
// far less), so a linear scan here costs nothing next to the disk read this
// replaces.
Chunk *ChunkProvider::reclaimPendingSave(std::uint64_t key)
{
	for (auto it = pendingSaveQueue.begin(); it != pendingSaveQueue.end(); ++it)
	{
		Chunk *chunk = *it;
		if (chunk == nullptr || chunkKey(chunk->xPosition, chunk->zPosition) != key)
			continue;
		pendingSaveQueue.erase(it);
		chunkMap[key] = chunk;
		chunkList.push_back(chunk);
		markChunkTopologyChanged();
		return chunk;
	}
	return nullptr;
}
#endif

bool ChunkProvider::isChunkPopulationPending(int_t i, int_t j) const
{
#if PLATFORM_DEFERRED_POPULATE
	return populateQueued.find(chunkKey(i, j)) != populateQueued.end();
#else
	(void)i;
	(void)j;
	return false;
#endif
}

void ChunkProvider::populate(IChunkProvider *ichunkprovider, int_t i, int_t j)
{
#if PLATFORM_DEFERRED_POPULATE
	while (!populateDeferredStep(i, j))
	{
	}
#else
	Chunk *chunk = provideChunk(i, j);
	if (chunk != nullptr && chunk != blankChunk && !chunk->isTerrainPopulated)
	{
		chunk->isTerrainPopulated = true;
		if (chunkProvider != nullptr)
		{
			chunkProvider->populate(ichunkprovider, i, j);
			chunk->setChunkModified();
		}
	}
#endif
}

#if PLATFORM_DEFERRED_POPULATE
bool ChunkProvider::populateDeferredBatch(int_t i, int_t j, int_t maxSteps, long_t deadlineNs, int_t &stepsRun)
{
	stepsRun = 0;
	Chunk *chunk = provideChunk(i, j);
	if (chunk == nullptr || chunk == blankChunk || chunk->isTerrainPopulated)
		return true;
	if (chunkProvider == nullptr)
	{
		chunk->isTerrainPopulated = true;
		stepsRun = 1;
		return true;
	}

	Chunk *populationChunks[POPULATION_FOOTPRINT_AXIS * POPULATION_FOOTPRINT_AXIS] = {};
	std::uint32_t before[POPULATION_FOOTPRINT_AXIS * POPULATION_FOOTPRINT_AXIS]
	                    [POPULATION_SECTION_COUNT] = {};
	for (int_t dz = 0; dz < POPULATION_FOOTPRINT_AXIS; ++dz)
	{
		for (int_t dx = 0; dx < POPULATION_FOOTPRINT_AXIS; ++dx)
		{
			const int_t footprintIndex = dz * POPULATION_FOOTPRINT_AXIS + dx;
			Chunk *footprintChunk = getLoadedChunk(JavaArithmetic::intAdd(i, dx), JavaArithmetic::intAdd(j, dz));
			populationChunks[footprintIndex] = footprintChunk;
			for (int_t sectionY = 0; sectionY < POPULATION_SECTION_COUNT; ++sectionY)
			{
				before[footprintIndex][sectionY] = footprintChunk != nullptr
					? footprintChunk->getBlockSectionRevision(sectionY)
					: 0u;
			}
		}
	}

	if (worldObj != nullptr)
		worldObj->beginPopulationFastPath(i, j);

	bool complete = false;
	try
	{
		while (stepsRun < maxSteps && !complete)
		{
			complete = chunkProvider->populateStep(this, i, j);
			++stepsRun;
			if (deadlineNs > 0 && System::nanoTime() >= deadlineNs)
				break;
		}
	}
	catch (...)
	{
		if (worldObj != nullptr)
			worldObj->endPopulationFastPath();
		throw;
	}

	if (worldObj != nullptr)
		worldObj->endPopulationFastPath();

	if (worldObj != nullptr)
	{
		for (int_t footprintIndex = 0;
			 footprintIndex < POPULATION_FOOTPRINT_AXIS * POPULATION_FOOTPRINT_AXIS;
			 ++footprintIndex)
		{
			Chunk *footprintChunk = populationChunks[footprintIndex];
			if (footprintChunk == nullptr || footprintChunk == blankChunk)
				continue;

			for (int_t sectionY = 0; sectionY < POPULATION_SECTION_COUNT; ++sectionY)
			{
				if (before[footprintIndex][sectionY] ==
					footprintChunk->getBlockSectionRevision(sectionY))
					continue;

				const int_t minX = JavaArithmetic::intMul(footprintChunk->xPosition, 16);
				const int_t minY = sectionY << 4;
				const int_t minZ = JavaArithmetic::intMul(footprintChunk->zPosition, 16);
				worldObj->markBlocksDirty(minX, minY, minZ,
				                          JavaArithmetic::intAdd(minX, 15), minY + 15,
				                          JavaArithmetic::intAdd(minZ, 15));
			}
		}
	}

	if (complete)
	{
		chunk->isTerrainPopulated = true;
		chunk->setChunkModified();
	}
	return complete;
}

bool ChunkProvider::populateDeferredStep(int_t i, int_t j)
{
	int_t stepsRun = 0;
	return populateDeferredBatch(i, j, 1, 0, stepsRun);
}

Chunk *ChunkProvider::getLoadedChunk(int_t i, int_t j)
{
	auto it = chunkMap.find(chunkKey(i, j));
	return it != chunkMap.end() ? it->second : nullptr;
}

bool ChunkProvider::canPopulateChunk(int_t i, int_t j)
{
	// Read straight from the map -- never call provideChunk() here, or the gate
	// would force-generate the very neighbours we are only meant to test for.
	Chunk *c = getLoadedChunk(i, j);
	if (c == nullptr || c == blankChunk || c->isTerrainPopulated)
		return false;
	// Decoration writes up to +8 blocks into the +x/+z neighbours, so they (and
	// the diagonal) must already exist. Same invariant the inline path enforced.
	const int_t eastX = JavaArithmetic::intAdd(i, 1);
	const int_t southZ = JavaArithmetic::intAdd(j, 1);
	return chunkExists(eastX, j)
		&& chunkExists(i, southZ)
		&& chunkExists(eastX, southZ);
}

void ChunkProvider::enqueuePopulate(int_t i, int_t j)
{
#if PLATFORM_POPULATE_CHUNKS_PER_TICK <= 0
	// Deferred decoration is disabled on this platform; do not let the queue grow
	// (drainPendingPopulate never runs, so entries would accumulate forever).
	(void)i; (void)j;
	return;
#else
	Chunk *c = getLoadedChunk(i, j);
	if (c == nullptr || c == blankChunk || c->isTerrainPopulated)
		return;
	const std::uint64_t key = chunkKey(i, j);
	if (populateQueued.insert(key).second)
		populateQueue.emplace_back(i, j);
#endif
}

void ChunkProvider::drainPendingPopulate(int_t budget)
{
	if (budget <= 0)
		return;
	// Bound the scan to the current queue length so dropping not-yet-ready entries
	// cannot spin. A dropped chunk is re-enqueued when its last missing neighbour
	// is generated (that prepareChunk enqueues this chunk as one of its four).
	int_t scan = (int_t)populateQueue.size();
	int_t steps = 0;
#if PLATFORM_POPULATE_BUDGET_US > 0
	PlatformStreamingFrameBudgetScope frameBudgetScope;
	const long_t budgetStartNs = System::nanoTime();
	const long_t budgetNs =
		PlatformStreamingFrameBudget::clampUs((long_t)PLATFORM_POPULATE_BUDGET_US) * 1000LL;
	const long_t deadlineNs = budgetStartNs + budgetNs;
#else
	const long_t deadlineNs = 0;
#endif
	while (steps < budget && scan-- > 0 && !populateQueue.empty())
	{
		const std::pair<int_t, int_t> coord = populateQueue.front();
		populateQueue.pop_front();
		const std::uint64_t key = chunkKey(coord.first, coord.second);

		// Keep the key registered while the step runs. Population writes dirty
		// render sections before populateDeferredStep() returns, and the Legacy
		// renderer uses this set to coalesce those mutations instead of restarting
		// an active staging mesh. It also prevents recursive enqueue attempts from
		// creating a duplicate queue entry for the chunk currently being processed.
		if (!canPopulateChunk(coord.first, coord.second))
		{
			populateQueued.erase(key);
			continue;
		}

		int_t batchSteps = 0;
		const bool complete = populateDeferredBatch(
			coord.first, coord.second, budget - steps, deadlineNs, batchSteps);
		steps += batchSteps;
		if (!complete)
		{
			populateQueue.emplace_front(coord);
			scan++;
		}
		else
		{
			populateQueued.erase(key);
		}
#if PLATFORM_POPULATE_BUDGET_US > 0
		if (System::nanoTime() - budgetStartNs >= budgetNs)
			break;
#endif
		if (!complete)
			continue;
	}
}
#endif

bool ChunkProvider::saveChunks(bool flag, IProgressUpdate *iprogressupdate)
{
	// Chunk persistence is region-file backed. Runtime autosaves call this with
	// flag=false from inside World::tick(), so keep that path deliberately small:
	// compressing and writing a large dirty batch synchronously produces a visible
	// libfat stall. Full/menu saves still drain every dirty chunk and flush the
	// RegionFile cache through saveExtraData() below.
#if PLATFORM_DSI
	// A chunk queued by unloadChunk()'s deferred save (see that function's own
	// comment) was already removed from chunkMap/chunkList, so the chunkList
	// sweep below would never see it -- without this, a pending edit could be
	// silently dropped if the world saves (autosave or exit) before the
	// per-tick drain gets to it. Periodic autosave only runs every
	// PLATFORM_AUTOSAVE_PERIOD_TICKS ticks (minutes apart, not every tick),
	// and the queue is normally near-empty by then anyway from the steady
	// 1/tick drain, so this is a rare, small cost, not a reintroduction of
	// the stall this whole mechanism exists to avoid.
	flushPendingSaves();
#endif
	int_t saved = 0;
	int_t totalToSave = 0;
	if (iprogressupdate != nullptr)
	{
		for (Chunk *chunk : chunkList)
		{
			if (chunk != nullptr && chunk != blankChunk && chunk->needsSaving(flag))
				totalToSave++;
		}
	}

	int_t progress = 0;
	for (Chunk *chunk : chunkList)
	{
		if (chunk == nullptr || chunk == blankChunk) continue;
		if (flag && !chunk->neverSave)
			saveExtraChunkData(chunk);
		if (!chunk->needsSaving(flag))
			continue;
		saveChunkToFile(chunk);
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		++saved;

		// Java can let the threaded writer queue grow because the desktop JVM has
		// a large GC heap.  In this C++ port every queued chunk owns a complete NBT
		// tree until the writer consumes it.  A new-world full save can otherwise
		// retain hundreds of chunk NBT trees at once and exhaust/fragment the heap
		// while the loading screen says "Saving chunks".  Drain in bounded batches
		// without changing which chunks are saved or the on-disk format.
		if (flag && (saved % std::max<int_t>(1, PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT)) == 0)
			ThreadedFileIOBase::threadedIOInstance.waitForFinish();

		// Release 1.2.5 uses 24 here. Platform tuning may lower the batch on
		// storage-constrained consoles without changing desktop/parity behavior.
		if (saved == PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT && !flag)
			return false;
		if (iprogressupdate != nullptr && totalToSave > 0 &&
		    ++progress % PLATFORM_SAVE_PROGRESS_UPDATE_INTERVAL == 0)
			iprogressupdate->setLoadingProgress((progress * 100) / totalToSave);
	}
	if (flag)
	{
		if (chunkLoader == nullptr) return true;
		chunkLoader->saveExtraData();
	}
	return true;
}

bool ChunkProvider::unload100OldestChunks()
{
#if PLATFORM_DEFERRED_POPULATE
	bool publishedThisTick = false;
#endif
#if PLATFORM_BOUNDED_WORLD
	// Per-tick hook (World::tick calls this once). Refill the synchronous-generation
	// budget for the new tick. Deferred decoration has its own feature gate below
	// so unbounded low-end desktop profiles can reuse it without adopting the
	// console chunk-cache policy.
	genChunksThisTick = 0;
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
#if PLATFORM_PROFILE_STREAMING
	const long_t generationStartNs = System::nanoTime();
#endif
	bool incrementalPublishedChunk = false;
#if PLATFORM_DSI
	// This function runs once per WORLD TICK, and Minecraft.cpp's runTick() can
	// run up to 10 of those back-to-back in one rendered frame to catch up after
	// a stall (Timer.cpp clamps elapsedTicks to 10) -- the same situation
	// drainPendingSaves() below already guards against for chunk saves. Without
	// this gate, each catch-up tick calls drainPendingGeneration() in turn, and
	// PlatformStreamingFrameBudget's clampUs() guarantees at least one step per
	// call even once the shared per-frame allowance is exhausted (see its own
	// comment) -- so a burst of catch-up ticks forces one atomic generation step
	// per tick regardless of budget. A single atomic step (BaseTerrain, Decorate
	// or Skylight; see ChunkProviderGenerate::advanceGenerationTask) measured
	// 20-143ms on real hardware, so 10 of those forced into one rendered frame
	// is 0.2-1.4s on its own, on top of whatever the stall that caused the
	// catch-up already cost. Real-hardware evidence: a debug.log frame of
	// tick=358/6506ms right after a preloadWorld() warmup cap left 17/18
	// sections unbuilt and dumped them into the first post-load catch-up burst.
	// Capping to roughly one drain per real tick interval (50ms) means a
	// catch-up burst still drains the generation queue, just no faster than a
	// single real frame could have anyway -- same shape as drainPendingSaves().
	static long_t s_lastGenDrainMs = 0;
	const long_t genNowMs = System::currentTimeMillis();
	const bool allowGenerationDrainThisTick = genNowMs - s_lastGenDrainMs >= 50;
	if (allowGenerationDrainThisTick)
		s_lastGenDrainMs = genNowMs;
	const bool incrementalGenerationWorked = allowGenerationDrainThisTick &&
		drainPendingGeneration(PLATFORM_GENERATION_STEPS_PER_TICK, incrementalPublishedChunk);
#else
	const bool incrementalGenerationWorked =
		drainPendingGeneration(PLATFORM_GENERATION_STEPS_PER_TICK, incrementalPublishedChunk);
#endif
#if PLATFORM_PROFILE_STREAMING
	if (incrementalGenerationWorked)
		platformProfileGenerate(System::nanoTime() - generationStartNs);
#endif
#if PLATFORM_DEFERRED_POPULATE
	// Advancing a stage does NOT reduce the decoration budget, publishing does
	// -- the same rule the async block below documents at length, applied to the
	// incremental generator.
	//
	// drainPendingGeneration() reports work for any advanced stage, and a chunk
	// takes five to eight of them (ChunkProviderGenerate::advanceGenerationTask).
	// While the player explores the generation queue is never empty, so gating on
	// the old flag closed this branch on essentially every tick: decoration did
	// not slow down, it stopped, and chunks stayed terrain-complete and bare for
	// as long as the walk lasted.
	//
	// Publishing is the tick that actually adds the column to chunkMap and builds
	// its skylight, so that one decorates on the reduced budget instead. Every
	// other generation tick now pays at most one extra PLATFORM_POPULATE_BUDGET_US.
	publishedThisTick = incrementalPublishedChunk;
#endif
#endif
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION
	const bool asyncPublishedChunk = drainAsyncGeneratedChunks(PLATFORM_ASYNC_GENERATION_PUBLISH_PER_TICK);
	// Dispatching does NOT reduce the decoration budget, publishing does.
	//
	// ChunkGenerationScheduler::dispatch() moves a coordinate from the pending
	// queue to the worker's queue under a mutex and returns; the generation
	// itself runs on ChunkGenerationScheduler::runWorker(). Letting dispatch close
	// this gate costs the whole decoration budget of every tick that
	// queued a request -- while the player explores, nearly all of them. The
	// decorator is one feature per step (see BiomeDecorator's stage machine), so
	// starving it does not slow decoration down, it stops it: chunks stayed
	// terrain-complete and undecorated for as long as the player kept walking.
	// Trees are what you notice, because they sit late in that order, after the
	// ores, clay and sand that are underground or unremarkable.
	//
	// Publishing is a real generation frame -- drainAsyncGeneratedChunks() is
	// what adds the ~80 KB column to chunkMap and regenerates its skylight -- so
	// that one decorates on PLATFORM_POPULATE_STEPS_AFTER_PUBLISH instead of the
	// full budget. It is a smaller share, not a stand-down: once the worker is
	// fed at its queue limit a result is ready on nearly every tick, and a gate
	// that closes on every publish is a gate that never opens.
	drainAsyncGenerationRequests(PLATFORM_ASYNC_GENERATION_REQUESTS_PER_TICK);
#if PLATFORM_DEFERRED_POPULATE
	publishedThisTick = publishedThisTick || asyncPublishedChunk;
#endif
#endif

#if PLATFORM_DEFERRED_POPULATE
	{
		const int_t populateSteps = publishedThisTick
			? PLATFORM_POPULATE_STEPS_AFTER_PUBLISH
			: PLATFORM_POPULATE_STEPS_PER_TICK;
#if PLATFORM_PROFILE_STREAMING
		const long_t populateStartNs = System::nanoTime();
#endif
#if PLATFORM_DSI
		// Diagnostic only, not a fix: with the catch-up-tick generation drain
		// now gated (see unload100OldestChunks()'s own drainPendingGeneration
		// call above), a real-hardware log still showed a single tick costing
		// ~7s with no "generation step" line anywhere nearby. drainPendingPopulate()
		// is the other unsliced-feeling per-tick drain this function runs --
		// structures/lakes/dungeons/springs/animal groups via populateDeferredBatch()
		// -- and has zero DSi timing instrumentation today, unlike generation.
		// Same nanoTime() pair and 20ms threshold as that diagnostic, to find out
		// whether this is where the still-unexplained time goes.
		const long_t dsiPopulateStartNs = System::nanoTime();
#endif
		// A budget of 0 returns immediately, which is what the platforms that
		// keep PLATFORM_POPULATE_STEPS_AFTER_PUBLISH at 0 rely on.
		drainPendingPopulate(populateSteps);
#if PLATFORM_DSI
		{
			const long_t dsiPopulateUs = (System::nanoTime() - dsiPopulateStartNs) / 1000;
			if (dsiPopulateUs >= 20000)
				MC_LOG_INFO("dsi", "populate drain took %lldus\n", (long long)dsiPopulateUs);
		}
#endif
#if PLATFORM_PROFILE_STREAMING
		platformProfilePopulate(System::nanoTime() - populateStartNs);
#endif
	}
#endif

#if PLATFORM_PROFILE_STREAMING
	const long_t chunkEvictStartNs = System::nanoTime();
#endif
	int_t unloaded = 0;
	ISaveHandler *saveHandler = worldObj != nullptr ? worldObj->getSaveHandler() : nullptr;
	const ChunkMemoryPolicy::RetentionPolicy retentionPolicy =
		ChunkMemoryPolicy::retentionPolicy(saveHandler != nullptr && saveHandler->isReadOnly());

	// Old explicit drop queue, kept for compatibility with the decompiled layout.
	while (!droppedChunksSet.empty() && unloaded < retentionPolicy.maxUnloadsPerTick)
	{
		std::uint64_t key = *droppedChunksSet.begin();
		droppedChunksSet.erase(droppedChunksSet.begin());

		auto it = chunkMap.find(key);
		if (it == chunkMap.end())
			continue;

		Chunk *chunk = it->second;
		unloadChunk(key, chunk);
		chunkMap.erase(it);
		markChunkTopologyChanged();
		chunkList.erase(std::remove(chunkList.begin(), chunkList.end(), chunk), chunkList.end());
		unloaded++;
	}

	// Real distance-based unload: the previous port never inserted into
	// droppedChunksSet, so chunks stayed in memory forever. This keeps the visible
	// radius plus margin, and removes only chunks that are already outside it.
	//
	// EMERGENCY UNLOAD: if the resident chunk map has grown well past what the
	// unload radius should ever hold, ignore MAX_UNLOADS_PER_TICK and the time
	// gate and drain out-of-radius chunks at an accelerated bounded rate. Without this, a burst
	// that gets chunks into the map faster than 8/tick can drain them (e.g. a
	// stall or a hitch during initial load bunching several ticks' worth of
	// requests together) lets the map grow without bound until the heap is
	// exhausted -- this mirrors the equivalent fix in
	// ChunkProviderLoadOrGenerate::unload100OldestChunks; both cache tiers need
	// it since ChunkProvider is the one actually in the World::getChunkProvider()
	// path.
	const size_t maxResidentChunks = (size_t)((chunkUnloadRadius * 2 + 1) * (chunkUnloadRadius * 2 + 1));
	// chunksOutsideRadius only matters when chunkMap is already over budget (the
	// `emergency` check below is short-circuited by size otherwise) -- the full
	// scan over every resident chunk to compute it is skipped in the common
	// case instead of running unconditionally every tick.
	size_t chunksOutsideRadius = 0;
	if (chunkMap.size() > maxResidentChunks)
	{
		for (const auto &entry : chunkMap)
		{
			Chunk *chunk = entry.second;
			if (chunk != nullptr && chunk != blankChunk
				&& isOutsideUnloadRadius(chunk->xPosition, chunk->zPosition))
			{
				chunksOutsideRadius++;
			}
		}
	}
	const bool emergency = chunkMap.size() > maxResidentChunks && chunksOutsideRadius > 0;
	const int_t unloadLimit = ChunkMemoryPolicy::unloadLimit(retentionPolicy, emergency);

	const long_t now = currentWorldTime();
	for (auto it = chunkMap.begin(); it != chunkMap.end() && unloaded < unloadLimit; )
	{
		Chunk *chunk = it->second;
		if (chunk == nullptr)
		{
			it = chunkMap.erase(it);
			markChunkTopologyChanged();
			chunkList.erase(std::remove(chunkList.begin(), chunkList.end(), chunk), chunkList.end());
			continue;
		}
		if (chunk == blankChunk)
		{
			// Real-hardware evidence (2026-09-24): a chunk whose region-file entry
			// is corrupt gets prepareChunkInternal()'s readFailed branch, which
			// logs "refusing to regenerate unreadable chunk" and caches blankChunk
			// at that key specifically so the position is never retried -- see its
			// own comment. This loop used to erase EVERY blankChunk entry
			// unconditionally, every single tick, regardless of distance or the
			// unload budget above (no isOutsideUnloadRadius check, doesn't even
			// count toward `unloaded`). For a position inside the load radius --
			// exactly where a corrupt chunk near the player or spawn sits -- that
			// silently undid the caching the very same tick it was written:
			// next tick's chunk request found nothing in chunkMap, re-ran the
			// full failed load, and re-logged the error, forever. A real-hardware
			// log showed this exact cycle: the same handful of positions logging
			// "refusing to regenerate" every tick, which (with sync=1/commit=1
			// logging forcing a blocking SD write per line) is also a real,
			// self-inflicted performance cost. blankChunk's own xPosition/
			// zPosition are not meaningful here -- it is one shared sentinel
			// object reused at every failed/placeholder position, not a real
			// per-chunk object -- so decode the key this entry actually lives at
			// instead, and keep the same distance gate real chunks use: still
			// prune it immediately once it drifts outside the unload radius
			// (unbounded growth stays bounded, same as before), but stop wiping
			// it every tick while it is still near the player.
			const int_t entryX = static_cast<int_t>(static_cast<std::int32_t>(it->first >> 32));
			const int_t entryZ = static_cast<int_t>(static_cast<std::int32_t>(it->first & 0xffffffffu));
			if (!isOutsideUnloadRadius(entryX, entryZ))
			{
				++it;
				continue;
			}
			it = chunkMap.erase(it);
			markChunkTopologyChanged();
			chunkList.erase(std::remove(chunkList.begin(), chunkList.end(), chunk), chunkList.end());
			if (chunksOutsideRadius > 0)
				chunksOutsideRadius--;
			continue;
		}

		const long_t lastAccess = chunk->lastAccessTick;

		if (isOutsideUnloadRadius(chunk->xPosition, chunk->zPosition)
			&& (emergency || JavaArithmetic::longSub(now, lastAccess) >= retentionPolicy.minUnusedTicksBeforeUnload))
		{
			unloadChunk(it->first, chunk);
			it = chunkMap.erase(it);
			markChunkTopologyChanged();
			chunkList.erase(std::remove(chunkList.begin(), chunkList.end(), chunk), chunkList.end());
			unloaded++;
			if (chunksOutsideRadius > 0)
				chunksOutsideRadius--;
			if (emergency && chunksOutsideRadius == 0)
				break;
		}
		else
		{
			++it;
		}
	}

#if PLATFORM_PROFILE_STREAMING
	platformProfileChunkEvict(System::nanoTime() - chunkEvictStartNs);
#endif
#if PLATFORM_DSI
	// Same rate unload-saves already ran at before they were deferred (see
	// unloadChunk()); called every tick regardless of whether this tick
	// unloaded anything, so a backlog left over from an emergency burst
	// keeps draining on the quiet ticks that follow it.
	//
	// Gated to real wall-clock time, not just "once per call", because this
	// is called once per WORLD TICK and Minecraft.cpp's runTick() can run
	// several of those back-to-back in a single rendered frame to catch up
	// after a stall (timer->elapsedTicks > 1) -- exactly the kind of stall
	// a slow world load or a burst of generation steps already causes on
	// this platform. Real-hardware evidence (a debug.log taken right after
	// a ~6.8s world-entry stall): three "deferred chunk save" lines back to
	// back with no frame boundary between them, 148-504ms each, because all
	// three catch-up ticks' worth of drainPendingSaves(1) landed in the one
	// real frame that ran them. That defeats the entire point of deferring
	// these saves in the first place -- the queue was still spreading them
	// out one PER TICK, just not one per RENDERED FRAME, which is what
	// actually matters for not freezing the display. Capping to roughly one
	// drain per real tick interval (50ms, vanilla's 20 ticks/sec) means a
	// catch-up burst still drains the backlog, just no faster than a single
	// real frame could have anyway -- same total time to empty the queue,
	// no multi-save pileup within one visible frame.
	static long_t s_lastDrainMs = 0;
	const long_t nowMs = System::currentTimeMillis();
	if (nowMs - s_lastDrainMs >= 50)
	{
		drainPendingSaves(retentionPolicy.maxUnloadsPerTick);
		s_lastDrainMs = nowMs;
	}
#endif
	if (chunkLoader != nullptr)
		chunkLoader->chunkTick();

	const bool childUnloaded = chunkProvider != nullptr && chunkProvider->unload100OldestChunks();
	return unloaded > 0 || childUnloaded;
}

bool ChunkProvider::canSave()
{
	return true;
}

jstring ChunkProvider::makeString()
{
	jstring result = "ServerChunkCache: " + String::fromInt((int_t)chunkMap.size())
		+ " Radius: " + String::fromInt(chunkLoadRadius)
		+ " UnloadRadius: " + String::fromInt(chunkUnloadRadius)
		+ " Drop: " + String::fromInt((int_t)droppedChunksSet.size());
#if PLATFORM_ASYNC_CHUNK_GENERATION
	if (asyncGenerationScheduler != nullptr)
	{
		int_t pending = 0, completed = 0;
		asyncGenerationScheduler->queueSizes(pending, completed);
		result += " GenQ: " + String::fromInt(pending)
		       + " GenDone: " + String::fromInt(completed);
	}
#endif
	return result;
}

std::vector<SpawnListEntry> *ChunkProvider::getPossibleCreatures(const EnumCreatureType &type, int_t x, int_t y, int_t z)
{
	return chunkProvider != nullptr ? chunkProvider->getPossibleCreatures(type, x, y, z) : nullptr;
}

ChunkPosition *ChunkProvider::findClosestStructure(World *world, const jstring &name, int_t x, int_t y, int_t z)
{
	return chunkProvider != nullptr ? chunkProvider->findClosestStructure(world, name, x, y, z) : nullptr;
}

void ChunkProvider::removeEntityFromLoadedChunks(Entity *entity)
{
	for (const auto &entry : chunkMap)
	{
		Chunk *chunk = entry.second;
		if (chunk != nullptr && chunk != blankChunk)
			chunk->removeEntityFromAllSections(entity);
	}
}
