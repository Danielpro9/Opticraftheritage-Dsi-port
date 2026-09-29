#pragma once

#include <vector>

#include "java/Type.h"

class Entity;
class EntityLiving;

// net.minecraft.src.EntitySenses
class EntitySenses
{
public:
	explicit EntitySenses(EntityLiving *entity);
	void clearSensingCache();
	bool canSee(Entity *target);

private:
	EntityLiving *entity;
	std::vector<Entity *> canSeeCachePositive;
	std::vector<Entity *> canSeeCacheNegative;
	// Ticks left before the cache above must actually clear; see
	// PLATFORM_CAN_SEE_CACHE_TICKS and clearSensingCache()'s own comment.
	int_t clearCountdown = 0;
};
