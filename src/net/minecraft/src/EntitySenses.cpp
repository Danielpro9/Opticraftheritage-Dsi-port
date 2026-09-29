#include "EntitySenses.h"

#include <algorithm>

#include "Entity.h"
#include "EntityLiving.h"
#include "platform/PlatformConfig.h"

EntitySenses::EntitySenses(EntityLiving *entity) : entity(entity)
{
}

void EntitySenses::clearSensingCache()
{
	// Called unconditionally every tick from EntityLiving::updateAITasks(),
	// before PLATFORM_THROTTLE_ENTITY_AI's shouldRunEntityDecisionAI() check
	// -- and that throttle only gates picking a NEW task anyway, not an
	// already-active one, so clearing this every tick forced a fresh
	// World::rayTraceBlocks() block-march (canEntityBeSeen(), EntityLiving.cpp)
	// for every actively-chasing/attacking mob every tick, regardless of
	// distance. PLATFORM_CAN_SEE_CACHE_TICKS keeps a tick's results valid for
	// that many ticks instead of one; see its own comment (PlatformConfig.h)
	// for the existing AI-tolerance windows this is checked safe against.
	if (--clearCountdown > 0)
		return;
	clearCountdown = PLATFORM_CAN_SEE_CACHE_TICKS;
	canSeeCachePositive.clear();
	canSeeCacheNegative.clear();
}

bool EntitySenses::canSee(Entity *target)
{
	if (target == nullptr || entity == nullptr)
		return false;
	if (std::find(canSeeCachePositive.begin(), canSeeCachePositive.end(), target) != canSeeCachePositive.end())
		return true;
	if (std::find(canSeeCacheNegative.begin(), canSeeCacheNegative.end(), target) != canSeeCacheNegative.end())
		return false;
	bool visible = entity->canEntityBeSeen(target);
	(visible ? canSeeCachePositive : canSeeCacheNegative).push_back(target);
	return visible;
}
