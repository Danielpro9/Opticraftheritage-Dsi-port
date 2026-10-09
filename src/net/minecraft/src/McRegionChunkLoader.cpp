#include "platform/WorkProfiler.h"
#include "platform/Log.h"
#include "McRegionChunkLoader.h"

#include "RegionFileCache.h"
#include "RegionFile.h"
#include "CompressedStreamTools.h"
#include "NBTTagCompound.h"
#include "ChunkLoader.h"
#include "Chunk.h"
#include "World.h"
#include "WorldInfo.h"
#include "java/System.h"
#include <sstream>
#include <cstdio>
#include <cstring>
#include <memory>
#if PLATFORM_DSI
#include "dsi/system/DsiArm7Compress.h"
#endif

namespace
{
    const char *regionReadErrorName(RegionFile::ReadStatus status)
    {
        switch (status)
        {
        case RegionFile::ReadStatus::InvalidCoordinates: return "invalid coordinates";
        case RegionFile::ReadStatus::InvalidSector: return "invalid sector";
        case RegionFile::ReadStatus::InvalidLength: return "invalid payload length";
        case RegionFile::ReadStatus::TruncatedData: return "truncated payload";
        case RegionFile::ReadStatus::UnsupportedCompression: return "unsupported compression";
        case RegionFile::ReadStatus::DecompressionFailed: return "decompression failed";
        case RegionFile::ReadStatus::IoError: return "I/O error";
        default: return "unknown error";
        }
    }

    // Non-owning streams over the loader's persistent vectors.  NBT parsing and
    // writing are synchronous, so their backing storage remains valid for the
    // whole operation and does not need an allocation-bearing stringstream.
    class VectorInputBuf : public std::streambuf
    {
    public:
        explicit VectorInputBuf(std::vector<byte_t> &bytes)
        {
            char *begin = reinterpret_cast<char *>(bytes.data());
            setg(begin, begin, begin + bytes.size());
        }
    };

    class VectorInputStream : public std::istream
    {
    public:
        explicit VectorInputStream(std::vector<byte_t> &bytes)
            : std::istream(nullptr), buffer(bytes)
        {
            rdbuf(&buffer);
        }
    private:
        VectorInputBuf buffer;
    };

    class VectorOutputBuf : public std::streambuf
    {
    public:
        explicit VectorOutputBuf(std::vector<byte_t> &bytes) : bytes(bytes)
        {
            bytes.clear();
        }

    protected:
        int_type overflow(int_type ch) override
        {
            if (traits_type::eq_int_type(ch, traits_type::eof()))
                return traits_type::not_eof(ch);
            bytes.push_back((byte_t)ch);
            return ch;
        }

        std::streamsize xsputn(const char *src, std::streamsize count) override
        {
            if (count > 0)
                bytes.insert(bytes.end(), reinterpret_cast<const byte_t *>(src),
                             reinterpret_cast<const byte_t *>(src) + count);
            return count;
        }

    private:
        std::vector<byte_t> &bytes;
    };

    class VectorOutputStream : public std::ostream
    {
    public:
        explicit VectorOutputStream(std::vector<byte_t> &bytes)
            : std::ostream(nullptr), buffer(bytes)
        {
            rdbuf(&buffer);
        }
    private:
        VectorOutputBuf buffer;
    };
}

McRegionChunkLoader::McRegionChunkLoader(const std::string &dir) :
    worldDir(dir)
{
}

Chunk* McRegionChunkLoader::loadChunk(World *world, int_t x, int_t z,
                                     ChunkLoadStatus *status)
{
    if (!readChunkData(x, z, readScratch, status))
        return nullptr;
    return loadChunkFromData(world, x, z, readScratch, status);
}

bool McRegionChunkLoader::readChunkData(int_t x, int_t z, std::vector<byte_t> &data,
                                        ChunkLoadStatus *status)
{
    if (status != nullptr)
        *status = ChunkLoadStatus::Missing;

    std::shared_ptr<RegionFile> rf = RegionFileCache::acquireRegionFile(worldDir, x, z);
    RegionFile::ReadStatus readStatus = RegionFile::ReadStatus::Missing;
    if (!rf->getChunkData(x & 0x1f, z & 0x1f, data, &readStatus))
    {
        if (status != nullptr && readStatus != RegionFile::ReadStatus::Missing)
            *status = ChunkLoadStatus::ReadError;
        if (readStatus != RegionFile::ReadStatus::Missing)
            MC_LOG_INFO("chunk", "Chunk %d,%d exists but its region data could not be read (%s); preserving it\n",
                   x, z, regionReadErrorName(readStatus));
        return false;
    }

    if (status != nullptr)
        *status = ChunkLoadStatus::Loaded;
    return true;
}

Chunk* McRegionChunkLoader::loadChunkFromData(World *world, int_t x, int_t z,
                                               std::vector<byte_t> &data,
                                               ChunkLoadStatus *status)
{
    std::unique_ptr<NBTTagCompound> root;
    Chunk *chunk = decodeChunkBlocksFromData(world, x, z, data, root, status);
    if (chunk != nullptr)
        attachChunkEntities(world, chunk, root.get());
    return chunk;
}

void McRegionChunkLoader::attachChunkEntities(World *world, Chunk *chunk, NBTTagCompound *root)
{
    if (chunk == nullptr || root == nullptr)
        return;
    NBTTagCompound *level = root->getCompoundTag("Level");
    if (level != nullptr)
        ChunkLoader::loadChunkEntitiesFromCompound(world, chunk, level);
}

Chunk* McRegionChunkLoader::decodeChunkBlocksFromData(World *world, int_t x, int_t z,
                                                      std::vector<byte_t> &data,
                                                      std::unique_ptr<NBTTagCompound> &rootOut,
                                                      ChunkLoadStatus *status)
{
    rootOut.reset();
    if (status != nullptr)
        *status = ChunkLoadStatus::ReadError;

    VectorInputStream is(data);

    std::unique_ptr<NBTTagCompound> nbttagcompound;
    try
    {
        PlatformLoadWorkScope nbtWork(PlatformLoadWork::Nbt);
        nbttagcompound.reset(CompressedStreamTools::readCompound(is));
    }
    catch (...)
    {
        MC_LOG_ERROR("chunk", "Invalid NBT in chunk %d,%d; preserving region data\n", x, z);
        return nullptr;
    }
    if (!nbttagcompound || !nbttagcompound->hasKey("Level"))
    {
        MC_LOG_WARN("chunk", "Chunk file at %d,%d is missing level data, skipping\n", x, z);
        return nullptr;
    }

    NBTTagCompound *level = nbttagcompound->getCompoundTag("Level");
    if (level == nullptr || !level->hasKey("Blocks"))
    {
        MC_LOG_ERROR("chunk", "Chunk file at %d,%d is missing block data, skipping\n", x, z);
        return nullptr;
    }

    PlatformLoadWorkScope decodeWork(PlatformLoadWork::ChunkDecode);
    Chunk *chunk = ChunkLoader::loadChunkBlocksFromCompound(world, level);

    if (chunk != nullptr && !chunk->isAtLocation(x, z))
    {
        // Do not "repair" a wrong-location region chunk by changing only xPos/zPos.
        // Its block array belongs to the other coordinates; relocating it creates
        // a valid-looking duplicate chunk and the next save makes the corruption
        // permanent. Treat this exactly like an unreadable region entry so the
        // provider preserves the file instead of regenerating/overwriting it.
        MC_LOG_ERROR("chunk", "Chunk file at %d,%d is in the wrong location; rejecting it. "
               "(Expected %d, %d, got %d, %d) Region data preserved.\n",
               x, z, x, z, chunk->xPosition, chunk->zPosition);
        delete chunk;
        return nullptr;
    }

    if (chunk != nullptr)
    {
        // Java McRegionChunkLoader calls Chunk.func_25124_i() here.  In this
        // port that method is remapBlocks(); onChunkLoadData() is func_4143_d()
        // and belongs to ChunkProviderLoadOrGenerate after the chunk is installed.
#if !PLATFORM_PS2
        chunk->remapBlocks();
#endif
        if (status != nullptr)
            *status = ChunkLoadStatus::Loaded;
        rootOut = std::move(nbttagcompound);
    }
    return chunk;
}

void McRegionChunkLoader::saveChunk(World *world, Chunk *chunk)
{
    world->checkSessionLock();

    try
    {
        std::unique_ptr<NBTTagCompound> nbttagcompound(new NBTTagCompound());
        NBTTagCompound *nbttagcompound1 = new NBTTagCompound();
        nbttagcompound->setTag("Level", nbttagcompound1);
        ChunkLoader::storeChunkInCompound(chunk, world, nbttagcompound1);

        // Serialize straight into reusable loader-owned scratch.  This is the
        // Wii equivalent of the PS2 port's fixed/bounded I/O staging: no
        // ostringstream growth, no string copy, and no per-save heap churn.
        VectorOutputStream os(writeScratch);
        CompressedStreamTools::writeCompound(nbttagcompound.get(), os);

        // Write to region file
        std::shared_ptr<RegionFile> rf = RegionFileCache::acquireRegionFile(
            worldDir, chunk->xPosition, chunk->zPosition);
        rf->write(chunk->xPosition & 0x1f, chunk->zPosition & 0x1f,
                  writeScratch.data(), (int_t)writeScratch.size());

        WorldInfo *worldinfo = world->getWorldInfo();
        worldinfo->setSizeOnDisk(worldinfo->getSizeOnDisk() +
            (long_t)rf->getSizeDelta());
    }
    catch (...)
    {
        // silently catch, matching Java behavior
    }
}

void McRegionChunkLoader::saveExtraChunkData(World *world, Chunk *chunk)
{
}

void McRegionChunkLoader::addRandomArmor()
{
}

void McRegionChunkLoader::saveExtraData()
{
    // Flush and close cached region files on full saves / world shutdown.
    // RegionFile::write() intentionally avoids flushing every individual chunk
    // to prevent gameplay stutter; this keeps the durable sync point here.
    RegionFileCache::clearCache();
}

#if PLATFORM_DSI
McRegionChunkLoader::~McRegionChunkLoader()
{
    // Defensive only: every normal path (continueSlicedSave() reaching
    // Z_STREAM_END, cancelSlicedSave(), finishSlicedSaveNow()) already calls
    // this. Only reached if the loader is torn down with a task mid-flight
    // some other way.
    endSlicedDeflateStream();
    // Same defensive cleanup for the load side's inflate stream.
    endSlicedInflateStream();
}

void McRegionChunkLoader::endSlicedDeflateStream()
{
    if (slicedSave.streamInitialized)
    {
        deflateEnd(&slicedSave.zs);
        slicedSave.streamInitialized = false;
    }
}

bool McRegionChunkLoader::beginSlicedSave(World *world, Chunk *chunk)
{
    if (slicedSave.active || world == nullptr || chunk == nullptr)
        return false;

    try
    {
        world->checkSessionLock();

        std::unique_ptr<NBTTagCompound> nbttagcompound(new NBTTagCompound());
        NBTTagCompound *nbttagcompound1 = new NBTTagCompound();
        nbttagcompound->setTag("Level", nbttagcompound1);
        ChunkLoader::storeChunkInCompound(chunk, world, nbttagcompound1);

        // Raw (uncompressed) NBT, same as saveChunk()'s own writeScratch --
        // this loader's own buffer is reused there, so a separate vector here
        // instead of writeScratch avoids the two save paths fighting over the
        // same scratch if they were ever both reachable at once.
        VectorOutputStream os(slicedSave.rawNbt);
        CompressedStreamTools::writeCompound(nbttagcompound.get(), os);
        if (slicedSave.rawNbt.empty())
            return false;

        slicedSave.compressedOut.clear();
        slicedSave.finished = false;
        slicedSave.chunk = chunk;
        slicedSave.world = world;
        slicedSave.active = true;
        slicedSave.viaArm7 = false;

#if PLATFORM_DSI
        // Try the ARM7 first -- see DsiArm7Compress.h for the full design
        // and the real-hardware-measured cost (100-700ms+/chunk) this
        // tries to move off the ARM9's own budget entirely rather than
        // just slicing it thinner. Declines immediately (false) if a
        // previous request is still in flight or the link never came up;
        // continueSlicedSave() then falls through to the exact ARM9
        // deflate stream below, unchanged, for this one chunk.
        slicedSave.viaArm7 = dsiArm7BeginCompress(slicedSave.rawNbt.data(), slicedSave.rawNbt.size());
        if (slicedSave.viaArm7)
            return true;
#endif

        std::memset(&slicedSave.zs, 0, sizeof(slicedSave.zs));
        // Same level choice as RegionFile::write()'s own compress2() call --
        // see PLATFORM_FAST_REGION_COMPRESSION's comment there. MAX_WBITS
        // (not 16+MAX_WBITS) matches compress2()'s zlib-wrapper format, which
        // RegionFile::writeSector() already hardcodes as "version 2" on read.
#if PLATFORM_FAST_REGION_COMPRESSION
        const int compressionLevel = Z_BEST_SPEED;
#else
        const int compressionLevel = Z_DEFAULT_COMPRESSION;
#endif
        if (deflateInit2(&slicedSave.zs, compressionLevel, Z_DEFLATED, MAX_WBITS,
                          8, Z_DEFAULT_STRATEGY) != Z_OK)
        {
            slicedSave.active = false;
            slicedSave.chunk = nullptr;
            slicedSave.world = nullptr;
            return false;
        }

        slicedSave.streamInitialized = true;
        slicedSave.zs.next_in = reinterpret_cast<Bytef *>(slicedSave.rawNbt.data());
        slicedSave.zs.avail_in = static_cast<uInt>(slicedSave.rawNbt.size());
        slicedSave.compressedOut.reserve(slicedSave.rawNbt.size() / 2 + 64);
        return true;
    }
    catch (...)
    {
        endSlicedDeflateStream();
        slicedSave.active = false;
        slicedSave.finished = false;
        slicedSave.viaArm7 = false;
        slicedSave.chunk = nullptr;
        slicedSave.world = nullptr;
        return false;
    }
}

bool McRegionChunkLoader::pumpSlicedDeflate(long_t budgetUs)
{
    const long_t startNs = System::nanoTime();
    constexpr std::size_t kOutputChunk = 4096;
    for (;;)
    {
        const std::size_t writePos = slicedSave.compressedOut.size();
        slicedSave.compressedOut.resize(writePos + kOutputChunk);
        slicedSave.zs.next_out = reinterpret_cast<Bytef *>(slicedSave.compressedOut.data() + writePos);
        slicedSave.zs.avail_out = static_cast<uInt>(kOutputChunk);

        const int flushMode = (slicedSave.zs.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH;
        const int ret = deflate(&slicedSave.zs, flushMode);

        const std::size_t produced = kOutputChunk - slicedSave.zs.avail_out;
        slicedSave.compressedOut.resize(writePos + produced);

        if (ret == Z_STREAM_END)
            return true;
        if (ret != Z_OK && ret != Z_BUF_ERROR)
        {
            // zlib error on well-formed input should not happen; abandon
            // rather than risk writing a truncated/corrupt stream.
            MC_LOG_ERROR("chunk", "McRegionChunkLoader sliced save: deflate error %d\n", ret);
            slicedSave.compressedOut.clear();
            return true;
        }

        if (budgetUs > 0 && (System::nanoTime() - startNs) / 1000 >= budgetUs)
            return false;
    }
}

bool McRegionChunkLoader::continueSlicedSave(long_t budgetUs)
{
    if (!slicedSave.active)
        return false;
    if (slicedSave.finished)
        return true;

    try
    {
#if PLATFORM_DSI
        if (slicedSave.viaArm7)
        {
            // Generous: the ARM7 runs this ~4x slower in wall-clock terms
            // than the ARM9 would (33MHz fixed vs. 133MHz, no data cache)
            // -- an accepted trade for paying zero ARM9 budget while it
            // works, not a sign anything is wrong. 4 seconds is well past
            // worst-case even for an unusually large chunk; this exists to
            // catch a stuck/crashed ARM7, not to second-guess ordinary
            // slowness. See DsiArm7Compress.h's own cost comment.
            constexpr std::uint64_t kArm7CompressTimeoutUs = 4'000'000;

            std::size_t compressedLength = 0;
            DsiArm7CompressResult result = dsiArm7PollCompress(compressedLength);

            // finishSlicedSaveNow() calls this with budgetUs<=0 and expects
            // the task to be fully done before it returns (used only for
            // the "Saving world" screen / a full exit save, where blocking
            // is already the accepted cost -- same as the old unsliced
            // saveChunk() path). Busy-wait for the ARM7 response rather
            // than abandoning real progress that may already be most of
            // the way done.
            while (budgetUs <= 0 && result == DsiArm7CompressResult::Pending &&
                   dsiArm7CompressElapsedUs() < kArm7CompressTimeoutUs)
            {
                result = dsiArm7PollCompress(compressedLength);
            }

            const bool timedOut = result == DsiArm7CompressResult::Pending &&
                                   dsiArm7CompressElapsedUs() >= kArm7CompressTimeoutUs;
            if (result == DsiArm7CompressResult::Pending && !timedOut)
                return false; // still waiting, well within budget -- try again next gated call

            if (result == DsiArm7CompressResult::Done)
            {
                const byte_t *bytes = reinterpret_cast<const byte_t *>(dsiArm7CompressedData());
                slicedSave.compressedOut.assign(bytes, bytes + compressedLength);
                slicedSave.viaArm7 = false;
            }
            else
            {
                // Failed, or timed out (including inside the budgetUs<=0
                // busy-wait above) -- fall back to the ARM9 path for this
                // same chunk, starting the stream fresh exactly as
                // beginSlicedSave() would have. slicedSave.rawNbt is still
                // exactly what it was when sent -- the ARM7 side only ever
                // reads its own private copy of it (DsiArm7Compress.cpp's
                // own comment) -- so this loses nothing.
                slicedSave.viaArm7 = false;
                std::memset(&slicedSave.zs, 0, sizeof(slicedSave.zs));
#if PLATFORM_FAST_REGION_COMPRESSION
                const int compressionLevel = Z_BEST_SPEED;
#else
                const int compressionLevel = Z_DEFAULT_COMPRESSION;
#endif
                if (deflateInit2(&slicedSave.zs, compressionLevel, Z_DEFLATED, MAX_WBITS,
                                  8, Z_DEFAULT_STRATEGY) != Z_OK)
                {
                    slicedSave.finished = true;
                    return true;
                }
                slicedSave.streamInitialized = true;
                slicedSave.zs.next_in = reinterpret_cast<Bytef *>(slicedSave.rawNbt.data());
                slicedSave.zs.avail_in = static_cast<uInt>(slicedSave.rawNbt.size());
                slicedSave.compressedOut.reserve(slicedSave.rawNbt.size() / 2 + 64);
            }
        }

        if (!slicedSave.viaArm7 && slicedSave.streamInitialized)
        {
            if (!pumpSlicedDeflate(budgetUs))
                return false;
            endSlicedDeflateStream();
        }
#else
        if (!pumpSlicedDeflate(budgetUs))
            return false;

        endSlicedDeflateStream();
#endif
        if (!slicedSave.compressedOut.empty())
        {
            std::shared_ptr<RegionFile> rf = RegionFileCache::acquireRegionFile(
                worldDir, slicedSave.chunk->xPosition, slicedSave.chunk->zPosition);
            rf->writeAlreadyCompressed(slicedSave.chunk->xPosition & 0x1f,
                                       slicedSave.chunk->zPosition & 0x1f,
                                       slicedSave.compressedOut.data(),
                                       slicedSave.compressedOut.size());
            WorldInfo *worldinfo = slicedSave.world->getWorldInfo();
            worldinfo->setSizeOnDisk(worldinfo->getSizeOnDisk() + (long_t)rf->getSizeDelta());
        }
    }
    catch (...)
    {
        // silently catch, matching saveChunk()'s own behavior
    }
    slicedSave.finished = true;
    return true;
}

bool McRegionChunkLoader::hasSlicedSaveTask() const
{
    return slicedSave.active;
}

Chunk *McRegionChunkLoader::slicedSaveChunk() const
{
    return slicedSave.active ? slicedSave.chunk : nullptr;
}

void McRegionChunkLoader::cancelSlicedSave()
{
    if (!slicedSave.active)
        return;
    endSlicedDeflateStream();
    slicedSave.active = false;
    slicedSave.finished = false;
    // Safe to clear rawNbt even if an ARM7 job for this task is still
    // in flight: DsiArm7Compress.cpp copies the bytes it sends into its
    // own buffer rather than reading this vector directly, specifically
    // so a cancel here can never race an in-flight read on the ARM7 side
    // (see that file's own comment). The abandoned job simply finishes in
    // the background with its result left unread.
    slicedSave.viaArm7 = false;
    slicedSave.chunk = nullptr;
    slicedSave.world = nullptr;
    slicedSave.rawNbt.clear();
    slicedSave.compressedOut.clear();
}

void McRegionChunkLoader::finishSlicedSaveNow()
{
    if (!slicedSave.active || slicedSave.finished)
        return;
    continueSlicedSave(0); // budgetUs <= 0: unbounded, runs to completion
}

Chunk *McRegionChunkLoader::takeSlicedSaveChunk()
{
    if (!slicedSave.active || !slicedSave.finished)
        return nullptr;
    Chunk *chunk = slicedSave.chunk;
    slicedSave.active = false;
    slicedSave.finished = false;
    slicedSave.chunk = nullptr;
    slicedSave.world = nullptr;
    slicedSave.rawNbt.clear();
    slicedSave.compressedOut.clear();
    return chunk;
}

void McRegionChunkLoader::endSlicedInflateStream()
{
    if (slicedLoad.streamInitialized)
    {
        inflateEnd(&slicedLoad.zs);
        slicedLoad.streamInitialized = false;
    }
}

bool McRegionChunkLoader::hasChunkOnDisk(int_t x, int_t z)
{
    std::shared_ptr<RegionFile> rf = RegionFileCache::acquireRegionFile(worldDir, x, z);
    return rf->hasChunk(x & 0x1f, z & 0x1f);
}

bool McRegionChunkLoader::beginSlicedLoad(int_t x, int_t z)
{
    if (slicedLoad.active)
        return false;

    std::shared_ptr<RegionFile> rf = RegionFileCache::acquireRegionFile(worldDir, x, z);
    byte_t version = 0;
    RegionFile::ReadStatus readStatus = RegionFile::ReadStatus::Missing;
    if (!rf->readCompressedChunkSector(x & 0x1f, z & 0x1f, slicedLoad.compressedIn, version, &readStatus))
    {
        // Missing or unreadable -- the caller treats this exactly like
        // loadChunk() returning nullptr immediately; no task to slice for a
        // chunk that was never there (or whose region data is corrupt).
        slicedLoad.compressedIn.clear();
        return false;
    }

    std::memset(&slicedLoad.zs, 0, sizeof(slicedLoad.zs));
    // version is already validated to be 1 or 2 by readCompressedChunkSector()
    // (anything else fails that call) -- same two formats RegionFile::write()
    // can produce (plain write() always picks version 2; an older region
    // written some other way could still have a version 1 entry).
    const int windowBits = (version == 1) ? (16 + MAX_WBITS) : MAX_WBITS;
    if (inflateInit2(&slicedLoad.zs, windowBits) != Z_OK)
    {
        slicedLoad.compressedIn.clear();
        return false;
    }

    slicedLoad.streamInitialized = true;
    slicedLoad.zs.next_in = reinterpret_cast<Bytef *>(slicedLoad.compressedIn.data());
    slicedLoad.zs.avail_in = static_cast<uInt>(slicedLoad.compressedIn.size());
    slicedLoad.rawOut.clear();
    // A Beta chunk's raw NBT is ~85KB (readScratch's own sizing comment).
    slicedLoad.rawOut.reserve(96 * 1024);
    slicedLoad.dataReady = false;
    slicedLoad.finished = false;
    slicedLoad.chunkX = x;
    slicedLoad.chunkZ = z;
    slicedLoad.active = true;
    return true;
}

bool McRegionChunkLoader::pumpSlicedInflate(long_t budgetUs)
{
    constexpr std::size_t kOutputChunk = 4096;
    // Same cap inflateChunkData()'s own one-shot path refuses to exceed.
    constexpr std::size_t kMaxOut = 1024 * 1024;
    const long_t startNs = System::nanoTime();
    for (;;)
    {
        if (slicedLoad.rawOut.size() >= kMaxOut)
        {
            slicedLoad.rawOut.clear();
            return true; // abandon: over-cap, same outcome as the one-shot path
        }

        const std::size_t writePos = slicedLoad.rawOut.size();
        slicedLoad.rawOut.resize(writePos + kOutputChunk);
        slicedLoad.zs.next_out = reinterpret_cast<Bytef *>(slicedLoad.rawOut.data() + writePos);
        slicedLoad.zs.avail_out = static_cast<uInt>(kOutputChunk);

        const int ret = inflate(&slicedLoad.zs, Z_NO_FLUSH);
        const std::size_t produced = kOutputChunk - slicedLoad.zs.avail_out;
        slicedLoad.rawOut.resize(writePos + produced);

        if (ret == Z_STREAM_END)
            return true;
        if (ret != Z_OK && ret != Z_BUF_ERROR)
        {
            // zlib error: truncated/corrupt sector. Same "fail, no partial
            // data" contract inflateChunkData() uses.
            slicedLoad.rawOut.clear();
            return true;
        }
        if (slicedLoad.zs.avail_out != 0)
        {
            // Not full, not finished, not erred: inflate made no further
            // progress with the input it has -- the one-shot path treats
            // this the same as a truncated stream rather than retrying.
            slicedLoad.rawOut.clear();
            return true;
        }

        if (budgetUs > 0 && (System::nanoTime() - startNs) / 1000 >= budgetUs)
            return false;
    }
}

bool McRegionChunkLoader::continueSlicedLoad(long_t budgetUs)
{
    if (!slicedLoad.active)
        return false;
    if (slicedLoad.finished)
        return true;

    if (!pumpSlicedInflate(budgetUs))
        return false;

    endSlicedInflateStream();
    slicedLoad.dataReady = !slicedLoad.rawOut.empty();
    slicedLoad.finished = true;
    return true;
}

bool McRegionChunkLoader::hasSlicedLoadTask() const
{
    return slicedLoad.active;
}

int_t McRegionChunkLoader::slicedLoadTaskX() const
{
    return slicedLoad.chunkX;
}

int_t McRegionChunkLoader::slicedLoadTaskZ() const
{
    return slicedLoad.chunkZ;
}

Chunk *McRegionChunkLoader::takeSlicedLoadChunk(World *world, ChunkLoadStatus *status)
{
    if (!slicedLoad.active || !slicedLoad.finished)
        return nullptr;

    Chunk *chunk = nullptr;
    if (slicedLoad.dataReady)
    {
        std::unique_ptr<NBTTagCompound> root;
        chunk = decodeChunkBlocksFromData(world, slicedLoad.chunkX, slicedLoad.chunkZ,
            slicedLoad.rawOut, root, status);
        if (chunk != nullptr)
            attachChunkEntities(world, chunk, root.get());
    }
    else if (status != nullptr)
    {
        *status = ChunkLoadStatus::ReadError;
    }

    slicedLoad.active = false;
    slicedLoad.finished = false;
    slicedLoad.dataReady = false;
    slicedLoad.chunkX = 0;
    slicedLoad.chunkZ = 0;
    slicedLoad.compressedIn.clear();
    slicedLoad.rawOut.clear();
    return chunk;
}

void McRegionChunkLoader::cancelSlicedLoad()
{
    if (!slicedLoad.active)
        return;
    endSlicedInflateStream();
    slicedLoad.active = false;
    slicedLoad.finished = false;
    slicedLoad.dataReady = false;
    slicedLoad.chunkX = 0;
    slicedLoad.chunkZ = 0;
    slicedLoad.compressedIn.clear();
    slicedLoad.rawOut.clear();
}
#endif
