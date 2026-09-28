#include "platform/Profiler.h"

#ifdef DSI_PLATFORM

#include "platform/PlatformCompat.h"
#include "platform/Log.h"
#include <cstring>
#include <cstdio>

// Named tick phases and (as of 2026-09-28) render phases are wired up; the
// rest are still no-ops. Wii's/PS2's equivalents accumulate more of this into
// an on-screen/logged performance window (see ClientProfilerBackend_WII.cpp);
// the extra pieces there (present-slip detection, GX/EE-specific swap
// timing) have no DSi equivalent to measure yet and are not worth adding
// blind.
std::uint32_t platformProfileRenderPhaseBegin()
{
	return static_cast<std::uint32_t>(PlatformCompat::getMonotonicMicros());
}

namespace
{
// Render-phase breakdown (see Profiler.h's PlatformRenderPhase), same
// accumulate-and-report-periodically shape as the tick-phase table below.
// Added to chase dsi.perf's flat ~36ms "render" figure, which (unlike
// "tick", broken down by the tickphase line below) had no further
// attribution -- see PlatformConfig.h's PLATFORM_PROFILE_RENDER_PHASES
// comment for why turning this on is expected to be cheap.
//
// PlatformRenderPhase::Frustum is used as the once-per-frame counter rather
// than Sky: EntityRenderer.cpp only brackets Sky when renderDistance < 2,
// which DSi's default of 3 never satisfies, while Frustum's bracket
// (clipRenderersByFrustrum) is unconditional every frame.
constexpr int kRenderPhaseSlots = 13; // PlatformRenderPhase's enum count
constexpr const char* const kRenderPhaseNames[kRenderPhaseSlots] = {
	"sky", "frustum", "build", "opaque", "ents", "transl", "hand", "hud",
	"entDraw", "tileDraw", "hudItems", "hudText", "hudHints"
};
long long g_renderPhaseSumUs[kRenderPhaseSlots] = {};
long long g_renderPhaseMaxUs[kRenderPhaseSlots] = {};
int g_renderFrameCount = 0;

void reportRenderPhasesIfDue()
{
	if (g_renderFrameCount < 20)
		return;

	char line[256];
	int len = 0;
	for (int i = 0; i < kRenderPhaseSlots && len < (int)sizeof(line) - 32; ++i)
	{
		if (g_renderPhaseSumUs[i] == 0 && g_renderPhaseMaxUs[i] == 0)
			continue;
		len += std::snprintf(line + len, sizeof(line) - len, " %s=%ld/%ldms",
			kRenderPhaseNames[i], (long)(g_renderPhaseSumUs[i] / g_renderFrameCount / 1000LL),
			(long)(g_renderPhaseMaxUs[i] / 1000LL));
	}
	MC_LOG_INFO("dsi.perf", "renderphase%s\n", line);

	for (int i = 0; i < kRenderPhaseSlots; ++i)
	{
		g_renderPhaseSumUs[i] = 0;
		g_renderPhaseMaxUs[i] = 0;
	}
	g_renderFrameCount = 0;
}
}

void platformProfileRenderPhaseEnd(std::uint32_t start, PlatformRenderPhase phase)
{
	const int index = static_cast<int>(phase);
	if (index < 0 || index >= kRenderPhaseSlots)
		return;
	const std::uint32_t now = static_cast<std::uint32_t>(PlatformCompat::getMonotonicMicros());
	const long long elapsedUs = (long long)(now - start);
	g_renderPhaseSumUs[index] += elapsedUs;
	if (elapsedUs > g_renderPhaseMaxUs[index])
		g_renderPhaseMaxUs[index] = elapsedUs;
	if (phase == PlatformRenderPhase::Frustum)
		++g_renderFrameCount;
	reportRenderPhasesIfDue();
}

namespace
{
// Real-hardware data (GuiMainMenu.cpp's own [dsi.perf] section timing, added
// to chase the render-side cost) showed the title-texture reupload fix
// dropped "render" from ~2s/frame to a few hundred ms -- and the very next
// test showed "tick" had become the new dominant cost instead, at up to
// several seconds a frame, with no breakdown of what inside Minecraft::
// runTick() is spending it. Minecraft.cpp already calls
// ClientProfiler::tickPhase(name, ns) once per named phase every tick
// ("stats", "mouseOver", "dynTex", ...) -- this was just discarding it. Same
// window-and-report shape as ClientProfilerBackend_DSI.cpp's frame/tick/
// render line and GuiMainMenu.cpp's menu-section line: accumulate per name,
// log one averaged line every 20 ticks (using "stats" as the once-per-tick
// marker -- it is the first call runTick() makes, unconditionally, every
// single tick).
constexpr int kTickPhaseSlots = 16;
constexpr int kTickPhaseNameChars = 16;
struct DsiTickPhase
{
	char name[kTickPhaseNameChars] = {};
	long long sumNs = 0;
	long long maxNs = 0;
};
DsiTickPhase g_tickPhases[kTickPhaseSlots];
int g_tickPhaseCount = 0;
int g_tickCount = 0;

void recordTickPhaseSample(const char* name, long long ns)
{
	for (int i = 0; i < g_tickPhaseCount; ++i)
	{
		DsiTickPhase& slot = g_tickPhases[i];
		if (std::strncmp(slot.name, name, kTickPhaseNameChars - 1) == 0)
		{
			slot.sumNs += ns;
			if (ns > slot.maxNs) slot.maxNs = ns;
			return;
		}
	}
	if (g_tickPhaseCount >= kTickPhaseSlots)
		return;
	DsiTickPhase& slot = g_tickPhases[g_tickPhaseCount++];
	std::strncpy(slot.name, name, kTickPhaseNameChars - 1);
	slot.name[kTickPhaseNameChars - 1] = '\0';
	slot.sumNs = ns;
	slot.maxNs = ns;
}

void reportTickPhasesIfDue()
{
	if (g_tickCount < 20)
		return;

	char line[512];
	int len = 0;
	for (int i = 0; i < g_tickPhaseCount && len < (int)sizeof(line) - 40; ++i)
	{
		const DsiTickPhase& slot = g_tickPhases[i];
		len += std::snprintf(line + len, sizeof(line) - len, " %s=%ld/%ldms",
			slot.name, (long)(slot.sumNs / g_tickCount / 1000000LL), (long)(slot.maxNs / 1000000LL));
	}
	MC_LOG_INFO("dsi.perf", "tickphase%s\n", line);

	for (int i = 0; i < g_tickPhaseCount; ++i)
		g_tickPhases[i] = DsiTickPhase{};
	g_tickPhaseCount = 0;
	g_tickCount = 0;
}
}

void platformProfileTickPhase(const char* name, long long ns)
{
	if (name == nullptr)
		return;
	if (ns < 0)
		ns = 0;
	if (std::strcmp(name, "stats") == 0)
		++g_tickCount;
	recordTickPhaseSample(name, ns);
	reportTickPhasesIfDue();
}
void platformProfileChunkBuild(long long, int) {}
void platformProfileChunkMeshPass(int, long long, int) {}
void platformProfileSnowColumn(bool, bool, int) {}
void platformProfilePopulatePhase(PlatformPopulatePhase, long long) {}
void platformProfileChunkLoad(long long) {}
void platformProfilePopulate(long long) {}
void platformProfileGenerate(long long) {}
void platformProfileMesh(long long) {}
void platformProfileUnloadSave(long long) {}
void platformProfileTickUpdates(long long) {}
void platformProfileTickQueue(long long) {}
void platformProfileMobSpawn(long long) {}
void platformProfileSaveWorldInfo(long long) {}
void platformProfileMapStorage(long long) {}
void platformProfileChunkEvict(long long) {}

#endif // DSI_PLATFORM
