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
#endif
};
