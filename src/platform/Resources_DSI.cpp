#include "platform/Resources.h"

#ifdef DSI_PLATFORM

#include "dsi/DsiEarlyInit.h"
#include "platform/storage/AssetPak.h"

#include <cstdlib>
#include <fstream>

// Resolves under sd:/OptiCraft (DsiEarlyStorage.cpp) for now, matching where
// world saves already live. NitroFS ("nitro:/", read-only, bundled inside the
// .nds itself -- see docs/guides/filesystem.md in the BlocksDS repo) is the
// natural home for shipped assets once src/dsi/Makefile actually stages a
// NITROFSDIR; nothing does that yet (see the Makefile's own note on this),
// so there is nothing to prefer it over the SD card for yet.
std::string PlatformResources::baseDir()
{
	return dsiGetSaveDir();
}

std::string PlatformResources::assetsDir()
{
	return baseDir() + "/data/assets";
}

std::string PlatformResources::audioDir()
{
	return baseDir() + "/data/resources";
}

std::string PlatformResources::resolveExisting(const std::string& path)
{
	// sd:/OptiCraft/assets.pak, keyed the same way the loose data/assets tree
	// is ("assets/gui/items.png", "resources/sound/..."). A hit answers with a
	// "pak://<key>" string that GameResources::openPath() already knows how
	// to read (AssetPak::isPakPath(), checked there unconditionally on every
	// platform) -- mirrors Resources_WII.cpp, the only other platform this
	// was ever wired up for. A missing assets.pak, or a key it does not hold,
	// falls through to the loose file exactly as before, so an install with
	// only the data/ tree keeps working and a pak can be partial.
	if (AssetPak::mountFrom(baseDir()) && AssetPak::exists(path))
		return AssetPak::makePath(path);

	std::string resolved;
	if (path.rfind("assets/", 0) == 0)
		resolved = assetsDir() + "/" + path.substr(7);
	else
		resolved = baseDir() + "/" + path;

	std::ifstream file(resolved, std::ios::binary);
	return file.good() ? resolved : std::string();
}

std::string PlatformResources::resolveAsset(const std::string& input)
{
	std::string path = input;
	if (!path.empty() && path[0] == '/')
		path.erase(path.begin());
	return resolveExisting("assets/" + path);
}

long PlatformResources::fileSize(const std::string& path)
{
	if (AssetPak::isPakPath(path))
		return AssetPak::size(AssetPak::keyOf(path));
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	return file ? static_cast<long>(file.tellg()) : -1L;
}

unsigned char* PlatformResources::loadFile(const std::string& path, unsigned int* outSize)
{
	if (AssetPak::isPakPath(path))
		return AssetPak::load(AssetPak::keyOf(path), outSize);
	if (outSize)
		*outSize = 0;
	const long size = fileSize(path);
	if (size <= 0)
		return nullptr;

	unsigned char* data = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(size)));
	if (!data)
		return nullptr;

	std::ifstream file(path, std::ios::binary);
	if (!file.read(reinterpret_cast<char*>(data), size))
	{
		std::free(data);
		return nullptr;
	}
	if (outSize)
		*outSize = static_cast<unsigned int>(size);
	return data;
}

#endif // DSI_PLATFORM
