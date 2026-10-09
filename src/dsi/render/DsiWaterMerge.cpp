#include "dsi/render/DsiWaterMerge.h"

#ifdef DSI_PLATFORM

#include <cmath>
#include <cstring>

#include "platform/PlatformTuning.h"
#include "net/minecraft/src/Block.h"
#include "net/minecraft/src/ChunkCache.h"
#include "platform/Log.h"

namespace
{
	constexpr unsigned kStride = 8; // words/vertex: x,y,z,u,v,color,<unused>,brightness
	constexpr unsigned kQuad = kStride * 4;

	// See DsiWaterMerge.h's own comment on dsiGetTotalWaterQuadsMerged() for
	// why this exists: the only prior signal (the MC_LOG_DEBUG call below)
	// has never been able to print on this platform's actual log level.
	unsigned long g_dsiWaterQuadsMergedCount = 0ul;

	inline float readFloat(const std::int32_t *p)
	{
		float f;
		std::memcpy(&f, p, sizeof(f));
		return f;
	}

	inline void writeFloat(std::int32_t *p, float f)
	{
		std::memcpy(p, &f, sizeof(f));
	}

	// 0 = eligible shape+material, 1 = wrong shape (not this function's quad
	// to touch), 2 = right shape, wrong material (do not merge, but still a
	// safe barrier -- matches ps2MergeWaterTops's own reason split).
	int classifyQuad(const std::int32_t *p, float u0, float v0, float span,
	                  ChunkCache &cc, int_t originX, int_t originY, int_t originZ)
	{
		const float x = readFloat(p + 0);
		const float y = readFloat(p + 1);
		const float z = readFloat(p + 2);
		// Still water's own vertex Y is j+fluidHeight with fluidHeight < 1.0
		// (vanilla's source-block height is short of a full block), so a
		// whole-number Y here means this quad is not a fluid top face at all.
		if (!(x >= 0.0f && x < 16.0f && y > 0.0f && y < 16.0f && z >= 0.0f && z < 16.0f) ||
			x != std::floor(x) || z != std::floor(z) || y == std::floor(y))
			return 1;

		static const int cdx[4] = { 0, 0, 1, 1 };
		static const int cdz[4] = { 0, 1, 1, 0 };
		for (unsigned i = 0; i < 4; ++i)
		{
			const std::int32_t *a = p + i * kStride;
			if (readFloat(a + 0) != x + (float)cdx[i] ||
				readFloat(a + 1) != y ||
				readFloat(a + 2) != z + (float)cdz[i] ||
				readFloat(a + 3) != u0 + (float)cdx[i] * span ||
				readFloat(a + 4) != v0 + (float)cdz[i] * span ||
				a[5] != p[5] || // color must match across all 4 corners
				a[7] != p[7])   // brightness must match across all 4 corners
				return 1;
		}

		const int_t wx = originX + (int_t)x;
		const int_t wy = originY + (int_t)std::floor(y);
		const int_t wz = originZ + (int_t)z;
		if (cc.getBlockId(wx, wy, wz) != Block::waterStill->blockID ||
			cc.getBlockMetadata(wx, wy, wz) != 0)
			return 2;
		return 0;
	}
}

unsigned dsi_merge_water_top_pairs(std::vector<std::int32_t> &raw, int_t tile,
                                    ChunkCache &cc,
                                    int_t originX, int_t originY, int_t originZ)
{
	if (tile < 0 || tile > 255 || raw.empty() || raw.size() % kQuad != 0)
		return 0;
	if (Block::waterStill == nullptr)
		return 0;

	const float u0 = (float)((tile & 0xf) << 4) / 256.0f;
	const float v0 = (float)(tile & 0xf0) / 256.0f;
	constexpr float span = 16.0f / 256.0f;
	const int_t configuredMerge = DSI_GREEDY_MAX_MERGE;
	const bool allowPairs = configuredMerge >= 2;

	unsigned merged = 0;
	std::size_t out = 0, in = 0;
	while (in < raw.size())
	{
		const int reason = classifyQuad(raw.data() + in, u0, v0, span, cc, originX, originY, originZ);
		if (reason != 0)
		{
			std::memmove(raw.data() + out, raw.data() + in, kQuad * sizeof(std::int32_t));
			out += kQuad;
			in += kQuad;
			continue;
		}

		// Run of consecutive eligible same-height/color/brightness quads,
		// indexed onto a 16x16 local-cell grid so adjacency can be found
		// without re-scanning ChunkCache. Bounded to 256 cells == one section
		// layer, matching ps2MergeWaterTops's own bound.
		short cells[256];
		for (short &cell : cells)
			cell = -1;
		bool consumed[256] = {};
		unsigned count = 0;
		const int_t height = raw[in + 1];
		const int_t color = raw[in + 5];
		const int_t brightness = raw[in + 7];
		while (count < 256 && in + count * kQuad < raw.size())
		{
			const std::int32_t *p = raw.data() + in + count * kQuad;
			if (count &&
				(p[1] != height || p[5] != color || p[7] != brightness ||
				 classifyQuad(p, u0, v0, span, cc, originX, originY, originZ) != 0))
				break;
			const unsigned cellX = (unsigned)readFloat(p + 0);
			const unsigned cellZ = (unsigned)readFloat(p + 2);
			const unsigned cell = cellX + 16u * cellZ;
			if (cells[cell] >= 0)
				break; // duplicate/overlapping surface: barrier, matches PS2's own rule
			cells[cell] = (short)(count++);
		}

		for (unsigned q = 0; q < count; ++q)
		{
			if (consumed[q])
				continue;
			std::int32_t *p = raw.data() + in + q * kQuad;
			const int x = (int)readFloat(p + 0);
			const int z = (int)readFloat(p + 2);
			auto available = [&](int cx, int cz) -> int
			{
				if (cx >= 16 || cz >= 16)
					return -1;
				const int n = cells[cx + 16 * cz];
				return (n > (int)q && !consumed[n]) ? n : -1;
			};
			const int right = available(x + 1, z);
			const int below = available(x, z + 1);
			const int diagonal = available(x + 1, z + 1);
			int width = 1, depth = 1;
			if (allowPairs && right >= 0 && below >= 0 && diagonal >= 0)
			{
				consumed[right] = consumed[below] = consumed[diagonal] = true;
				width = depth = 2;
				merged += 3;
			}
			else if (allowPairs && right >= 0)
			{
				consumed[right] = true;
				width = 2;
				merged += 1;
			}
			else if (allowPairs && below >= 0)
			{
				consumed[below] = true;
				depth = 2;
				merged += 1;
			}

			if (width == 2 || depth == 2)
			{
				// Geometry stretches to cover the merged footprint; UV is left
				// untouched (still the single source tile's rect) -- see this
				// file's header comment on why (no DS hardware region-repeat).
				static const int cdx[4] = { 0, 0, 1, 1 };
				static const int cdz[4] = { 0, 1, 1, 0 };
				for (unsigned i = 0; i < 4; ++i)
				{
					std::int32_t *vtx = p + i * kStride;
					writeFloat(vtx + 0, (float)(x + cdx[i] * width));
					writeFloat(vtx + 2, (float)(z + cdz[i] * depth));
				}
			}

			std::memmove(raw.data() + out, p, kQuad * sizeof(std::int32_t));
			out += kQuad;
		}
		in += count * kQuad;
	}

	raw.resize(out);
	if (merged != 0)
	{
		MC_LOG_DEBUG("dsi", "water merge: pairs/squares removed=%u\n", merged);
		g_dsiWaterQuadsMergedCount += merged;
	}
	return merged;
}

unsigned long dsiGetTotalWaterQuadsMerged()
{
	return g_dsiWaterQuadsMergedCount;
}

#endif // DSI_PLATFORM
