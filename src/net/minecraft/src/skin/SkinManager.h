#pragma once

#include <string>
#include <vector>

// Ported from upstream OptiCraft Heritage Edition (src/net/minecraft/src/skin/)
// with one deliberate scope cut for this fork: no in-game "install from SD
// browser" flow (GuiLoadSkinsMenu/GuiLoadSkinsList/GuiConfirmSkinInstall
// upstream ship alongside this). The player manages the skins folder from a
// computer, same as every other file this project already expects on the SD
// card -- scanCustomSkins() below just picks up whatever is there. See
// GuiSkinSelector.h for the DSi-side selection screen this backs.
struct SkinEntry
{
    std::string id;          // Internal ID, e.g. "Steve", "custom_Goku"
    std::string name;        // Display name, e.g. "Steve", "Goku"
    std::string skinPath;    // 64x64 or full skin path
    std::string modelPath;   // 64x32 model texture path
    std::string frontPath;   // 16x32 2D front preview path (or empty if custom/none)
    bool isCustom = false;
    std::string filePath;    // Source file path on disk
};

class SkinManager
{
public:
    static void init();
    static void scanCustomSkins();

    // Pack / Tab support
    static int getPackCount();
    static std::string getPackName(int packIndex);
    static const std::vector<SkinEntry>& getSkinsForPack(int packIndex);
    static int getSkinCountForPack(int packIndex);
    static const SkinEntry* getSkin(int packIndex, int skinIndex);

    static int getSelectedPackIndex();
    static void setSelectedPackIndex(int packIndex);

    // Custom skin management
    static std::string getSkinsDir();
    static bool deleteCustomSkin(const std::string &id);
    static const std::vector<SkinEntry>& getCustomSkins();

    // Backward-compatible methods operating on current pack
    static const std::vector<SkinEntry>& getSkins();
    static int getSkinCount();
    static const SkinEntry* getSkin(int index);
    static const SkinEntry* getSkinById(const std::string& id);
    static int getIndexById(const std::string& id);

    static std::string getSelectedSkinId();
    static void setSelectedSkinId(const std::string& id);
    static int getSelectedIndex();
    static void setSelectedIndex(int index);

    // Returns the texture path for the player model (e.g. "/mob/char.png" or
    // a custom scanned path)
    static std::string getActiveSkinTexture();
    static std::string getDefaultSkinTexture();
};
