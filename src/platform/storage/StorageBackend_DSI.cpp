#include "platform/Storage.h"
#include "platform/storage/PosixFileSystem.h"

#include <cstdio>

// BlocksDS's FatFs-backed filesystem exposes the same POSIX surface
// PosixFileSystem.cpp already uses for PS2/Wii (fopen/stat/opendir/mkdir/
// rename/unlink -- see docs/guides/filesystem.md in the BlocksDS repo), so this
// backend is a straight delegation, no DSi-specific I/O of its own. There is no
// pak/asset-archive story for DSi yet (see StorageBackend_WII.cpp for what that
// looks like once one exists), so unlike that file this one has no
// AssetPak::isPakPath() branch to take.
namespace PlatformStorage
{
bool exists(const std::string& path) { return posixExists(path); }
bool mkdirs(const std::string& path) { return makeDirectories(path); }
bool removeFile(const std::string& path) { return removePath(path); }
bool renameFile(const std::string& from, const std::string& to) { return renamePath(from, to); }
bool supportsAtomicRename() { return true; }
// Session locks (SaveHandler::checkSessionLock()) exist to detect a save
// folder being opened from ANOTHER location (a second process, a PC tool, a
// cloud-sync conflict) while this one is running -- not a scenario a single-
// process handheld console with local SD storage only can hit. PS2 already
// disables this outright for the same reason (StorageBackend_PS2.cpp).
// checkSessionLock() runs on every chunk save (AnvilChunkLoader::saveChunk(),
// ChunkLoader.cpp, McRegionChunkLoader.cpp) plus once per full World::save-
// World() -- each call is a synchronous session.lock read from the SD card
// (no background I/O thread on DSi, PLATFORM_ASYNC_FILE_IO is off), and that
// per-chunk cost got materially more frequent once unloadChunk() started
// saving edited chunks on stream-out (PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_
// UNLOAD, DsiWorldTuning.h). Both writeSessionLock() and checkSessionLock()
// are already gated identically on this flag (SaveHandler.cpp), so turning
// it off cleanly disables the whole mechanism rather than leaving it
// half-armed.
bool supportsSessionLocks() { return false; }
bool readFile(const std::string& path, std::vector<unsigned char>& out) { return posixReadFile(path, out); }
std::int64_t getFileSize(const std::string& path) { return fileSize(path); }
bool readFileRange(const std::string& path, std::size_t offset, void* out, std::size_t length)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    const bool ok = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
                    std::fread(out, 1, length, file) == length;
    std::fclose(file);
    return ok;
}
bool writeFile(const std::string& path, const void* data, std::size_t length) { return posixWriteFile(path, data, length); }
bool appendFile(const std::string& path, const void* data, std::size_t length) { return posixAppendFile(path, data, length); }
bool pathIsDirectory(const std::string& path) { return isDirectory(path); }
bool listPathEntries(const std::string& path, std::vector<std::string>& out) { return listEntries(path, out); }

bool listDirs(const std::string& path, std::vector<std::string>& out)
{
    std::vector<std::string> entries;
    if (!listEntries(path, entries))
    {
        out.clear();
        return false;
    }

    out.clear();
    for (const std::string& entry : entries)
    {
        if (isDirectory(join(path, entry)))
            out.push_back(entry);
    }
    return true;
}
}
