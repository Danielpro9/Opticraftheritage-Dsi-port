#ifndef RENDERMANAGER_H
#define RENDERMANAGER_H

#include <unordered_map>
#include <typeindex>
#include "java/Type.h"

class Render;
class World;
class RenderEngine;
class FontRenderer;
class EntityLiving;
class GameSettings;
class Entity;
class ItemRenderer;

class RenderManager {
public:
    static RenderManager* instance;

    RenderManager();
    ~RenderManager();

    Render* getEntityClassRenderObject(std::type_index classType);
    Render* getEntityRenderObject(Entity* entity);

    void cacheActiveRenderInfo(World* world, RenderEngine* renderengine, FontRenderer* fontrenderer, EntityLiving* entityliving, GameSettings* gamesettings, float f);
    void renderEntity(Entity* entity, float f);
    void renderEntityWithPosYaw(Entity* entity, double d, double d1, double d2, float f, float f1);
    void setWorld(World* world);
    double getDistanceToCamera(double d, double d1, double d2);
    FontRenderer* getFontRenderer();

    // getEntityRenderObject()/getEntityClassRenderObject() are looked up once
    // per visible entity every single frame (RenderGlobal's per-entity render
    // loop). std::type_index's operator< falls back to a type_info name-
    // string comparison on typical libstdc++/ARM builds, making a std::map
    // (red-black tree) descent real, avoidable per-frame CPU work for what is
    // otherwise a pure point-lookup with a small, bounded key set (~50 entries
    // from registerRenderers() plus subclass-resolution memoization) and no
    // iteration-order dependency (verified: both loops that iterate this --
    // the destructor's dedupe pass and registerRenderers()'s setRenderManager
    // pass -- are order-independent). std::type_index already has a
    // std::hash specialization, so this is a pure container-type swap.
    std::unordered_map<std::type_index, Render*> entityRenderMap;
    FontRenderer* fontRenderer;
    static double renderPosX;
    static double renderPosY;
    static double renderPosZ;
    RenderEngine* renderEngine;
    ItemRenderer* itemRenderer;
    World* worldObj;
    EntityLiving* livingPlayer;
    float playerViewY;
    float playerViewX;
    GameSettings* options;
    double field_1222_l;
    double field_1221_m;
    double field_1220_n;

private:
    void registerRenderers();
};

#endif