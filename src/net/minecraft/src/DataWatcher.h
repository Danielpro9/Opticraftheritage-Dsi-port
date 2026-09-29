#pragma once

#include <array>
#include <vector>
#include <any>
#include <iostream>
#include "java/Type.h"
#include "java/HashSet.h"

class WatchableObject;
class ItemStack;
class ChunkCoordinates;

struct DataWatcherIdHash
{
    int_t operator()(int_t value) const { return value; }
};

struct DataWatcherIdEqual
{
    bool operator()(int_t lhs, int_t rhs) const { return lhs == rhs; }
};

// net.minecraft.src.DataWatcher
class DataWatcher
{
public:
    DataWatcher();
    ~DataWatcher();

    void addObject(int_t id, std::any obj);
    bool hasObject(int_t id) const;

    byte_t getWatchableObjectByte(int_t id);
    short_t getWatchableObjectShort(int_t id);
    int_t getWatchableObjectInt(int_t id);
    std::string getWatchableObjectString(int_t id);

    void updateObject(int_t id, std::any obj);

    static void writeObjectsInListToStream(const std::vector<WatchableObject*> &list, std::ostream &os);
    void writeWatchableObjects(std::ostream &os);

    static std::vector<WatchableObject*> readWatchableObjects(std::istream &is);
    void updateWatchedObjectsFromList(const std::vector<WatchableObject*> &list);

private:
    static void writeWatchableObject(std::ostream &os, WatchableObject *obj);
    static int_t getTypeId(const std::any &obj);

    // addObject() enforces id <= 31 (matching vanilla's "Data value id is too
    // big" check), so the key space is a small, dense, bounded set of at most
    // 32 slots -- a std::map (a red-black tree with per-node allocation) was
    // pure overhead for it. This backs every entity's runtime flags (on-fire,
    // sneaking, sprinting, health, mob-specific state) via Entity::
    // getEntityFlag()/setEntityFlag() and dozens of mob subclasses' own
    // fields, read and written from both per-tick AI/update logic and
    // per-frame render code, so it is not a cold container.
    // getSlot() centralizes the bounds/null check every accessor needs.
    WatchableObject *getSlot(int_t id) const;
    std::array<WatchableObject*, 32> watchedObjects{};
    JavaHashSet<int_t, DataWatcherIdHash, DataWatcherIdEqual> watchedObjectOrder;
    bool objectChanged;
};
