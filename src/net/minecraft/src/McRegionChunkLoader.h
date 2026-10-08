#pragma once

#include "java/Type.h"

#include "IChunkLoader.h"
#include "platform/PlatformConfig.h"
#include <memory>
#include <string>
#include <vector>
#if PLATFORM_DSI
#include <zlib.h>
#endif

class World;
class Chunk;
class NBTTagCompound;

// net.minecraft.src.McRegionChunkLoader
class McRegionChunkLoader : public IChunkLoader
{
public:
    explicit McRegionChunkLoader(const std::string &worldDir);
#if PLATFORM_DSI
    ~McRegionChunkLoader() override;
#endif

    Chunk* loadChunk(World *world, int_t x, int_t z,
                     ChunkLoadStatus *status = nullptr) override;
    bool readChunkData(int_t x, int_t z, std::vector<byte_t> &data,
                       ChunkLoadStatus *status = nullptr);
    Chunk* loadChunkFromData(World *world, int_t x, int_t z,
                             std::vector<byte_t> &data,
                             ChunkLoadStatus *status = nullptr);
    // loadChunkFromData in two halves, for a streaming worker. The first
    // parses the NBT and builds the chunk's blocks, light and heightmap (all
    // of it private to the new Chunk, like the generator's provideChunk) and
    // hands the parsed root back so the second, on the game thread, can
    // construct the entities and tile entities it holds.
    Chunk* decodeChunkBlocksFromData(World *world, int_t x, int_t z,
                                     std::vector<byte_t> &data,
                                     std::unique_ptr<NBTTagCompound> &rootOut,
                                     ChunkLoadStatus *status = nullptr);
    static void attachChunkEntities(World *world, Chunk *chunk, NBTTagCompound *root);
    void saveChunk(World *world, Chunk *chunk) override;
    void saveExtraChunkData(World *world, Chunk *chunk) override;
    void addRandomArmor() override;
    void saveExtraData() override;

#if PLATFORM_DSI
    // Sliced save: the console-only alternative to saveChunk() above, for
    // ChunkProvider's deferred-unload-save queue. saveChunk()'s single
    // compress2() call measured 100-700ms+ on real hardware for one chunk
    // (see ChunkProvider::drainPendingSaves()'s own comment) -- all of it
    // CPU time spent inside zlib deflate on this FPU-less ARM9, blocking
    // whichever tick it landed in. NBT building and the final sector/file
    // write stay single atomic steps (both measured cheap relative to
    // compression, and the file write is not CPU-bound); only the deflate
    // loop itself is resumable, via zlib's own incremental streaming API
    // (the same deflateInit2/deflate/deflateEnd shape CompressedStreamTools.cpp's
    // PS2 GzipInflateStreamBuf already uses for incremental decompression).
    //
    // beginSlicedSave() does the NBT-build-and-serialize step and starts the
    // deflate stream. continueSlicedSave(budgetUs) runs deflate() for up to
    // budgetUs of wall-clock CPU time per call and returns true once the
    // chunk is fully compressed AND written to its region file; the caller
    // then collects the Chunk via takeSlicedSaveChunk() to finish unloading
    // it exactly as the old saveChunk() path did. Only one task is active at
    // a time -- ChunkProvider's own pendingSaveQueue already serializes
    // chunks one at a time onto this loader, same as before.
    bool beginSlicedSave(World *world, Chunk *chunk);
    bool continueSlicedSave(long_t budgetUs);
    bool hasSlicedSaveTask() const;
    // For ChunkProvider::reclaimPendingSave(): the chunk currently mid-save,
    // or nullptr if no task is active.
    Chunk *slicedSaveChunk() const;
    // Abandons the active task without writing anything (deflateEnd only,
    // no partial/corrupt data ever reaches the region file). Used when the
    // player walks back into this exact chunk before its save finishes --
    // same "hand the live object back, it re-queues normally next time if
    // still dirty" contract ChunkProvider's queue-based reclaim already has.
    void cancelSlicedSave();
    // Finishes the active task right now, blocking for however long is
    // left. Used by flushPendingSaves()/full saves, where "done right now"
    // is already the expected cost (the "Saving world" screen).
    void finishSlicedSaveNow();
    // Collects the chunk once continueSlicedSave()/finishSlicedSaveNow() has
    // returned true. Returns nullptr if the task is still in progress.
    Chunk *takeSlicedSaveChunk();

    // Sliced load: the loading counterpart to the sliced save above.
    // loadChunk()'s readChunkData() -> RegionFile::getChunkData() ran a
    // single inflate() call to completion (same cost class the save side's
    // compress2() had, 100-700ms+), and -- unlike generation, which defers
    // through the incremental generationTask queue when the chunk does not
    // yet exist -- ChunkProvider::prepareChunkInternal() called this
    // unconditionally and synchronously for EVERY chunk whose file already
    // exists, including every ordinary re-entry into previously-visited
    // terrain. beginSlicedLoad() reads the still-compressed sector bytes
    // (RegionFile::readCompressedChunkSector(), a cheap I/O read -- not the
    // expensive part) and starts an inflate stream. continueSlicedLoad(
    // budgetUs) runs inflate() for up to budgetUs of wall-clock CPU time per
    // call and returns true once the chunk's raw NBT is fully decompressed;
    // the caller then collects the Chunk via takeSlicedLoadChunk(), which
    // runs the same NBT-parse-and-build decodeChunkBlocksFromData()/
    // attachChunkEntities() this loader's own synchronous path already used,
    // unsliced (both measured cheap relative to decompression). Only one
    // task is active at a time, same as the save side.
    // Cheap existence check for ChunkProvider's deferred-load queue: true if
    // this chunk has a region-file entry worth queuing a sliced load for
    // (RegionFile::hasChunk() -- a header-only check, no sector read). Used
    // instead of attempting a load first, since "never existed" and "existed
    // but should now generate" need different queues (deferred load vs.
    // deferred generation).
    bool hasChunkOnDisk(int_t x, int_t z);
    bool beginSlicedLoad(int_t x, int_t z);
    bool continueSlicedLoad(long_t budgetUs);
    bool hasSlicedLoadTask() const;
    int_t slicedLoadTaskX() const;
    int_t slicedLoadTaskZ() const;
    // Collects the chunk once continueSlicedLoad() has returned true.
    // Returns nullptr (with *status left at whatever continueSlicedLoad()'s
    // own failure path set) if the task never had data to parse -- a
    // missing/corrupt region entry is a normal outcome here, same as
    // loadChunk()'s existing readFailed contract.
    Chunk *takeSlicedLoadChunk(World *world, ChunkLoadStatus *status);
    void cancelSlicedLoad();
#endif

private:
    std::string worldDir;
    // Synchronous I/O scratch, retained for the loader lifetime.  A beta
    // chunk's NBT typically fits in 85KB; reusing these removes the repeated
    // grow/free cycles that fragment newlib's heap during world streaming.
    std::vector<byte_t> readScratch;
    std::vector<byte_t> writeScratch;

#if PLATFORM_DSI
    struct SlicedSaveTask
    {
        bool active = false;
        bool finished = false;
        bool streamInitialized = false;
        World *world = nullptr;
        Chunk *chunk = nullptr;
        std::vector<byte_t> rawNbt;
        std::vector<byte_t> compressedOut;
        z_stream zs{};
    };
    SlicedSaveTask slicedSave;

    void endSlicedDeflateStream();
    // Runs deflate() until budgetUs of wall-clock time elapses or the
    // stream reaches Z_STREAM_END (or errors out); budgetUs <= 0 means run
    // to completion unconditionally. Returns true once there is nothing
    // left to do (finished or abandoned on error), false if more work
    // remains and the budget ran out first.
    bool pumpSlicedDeflate(long_t budgetUs);

    struct SlicedLoadTask
    {
        bool active = false;
        bool finished = false;
        bool streamInitialized = false;
        bool dataReady = false; // false => missing/corrupt; takeSlicedLoadChunk() returns nullptr
        int_t chunkX = 0;
        int_t chunkZ = 0;
        std::vector<byte_t> compressedIn;
        std::vector<byte_t> rawOut;
        z_stream zs{};
    };
    SlicedLoadTask slicedLoad;

    void endSlicedInflateStream();
    // Same shape as pumpSlicedDeflate(), for inflate() instead.
    bool pumpSlicedInflate(long_t budgetUs);
#endif
};
