#include "dsi/render/DsiGreedyMesh.h"

#ifdef DSI_PLATFORM

#include "platform/PlatformTuning.h"

#include "net/minecraft/src/Block.h"
#include "net/minecraft/src/BlockGrass.h"
#include "net/minecraft/src/BlockLeaves.h"
#include "net/minecraft/src/Chunk.h"
#include "net/minecraft/src/ChunkCache.h"
#include "net/minecraft/src/Tessellator.h"
#include "dsi/render/DsiBlockRenderInfo.h"

namespace
{
// See DsiGreedyMesh.h's own comment on dsiGetTotalGreedyQuads()/
// dsiGetTotalGreedyFacesCovered() for why these exist and what they prove.
unsigned long g_dsiGreedyQuadCount = 0ul;
unsigned long g_dsiGreedyFacesCoveredCount = 0ul;

struct FaceKey
{
	int_t texture;
	int_t brightness;
	unsigned char red;
	unsigned char green;
	unsigned char blue;
	bool valid;
};

static bool isOpaqueBlockId(int_t id)
{
	if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE)
		return false;

	Block *block = Block::blocksList[id];
	if (block == nullptr)
		return false;
	if (Block::staticOpaqueCubeLookupSafe[id])
		return Block::opaqueCubeLookup[id];
	return block->isOpaqueCube();
}

// Matches Ps2GreedyMesh.cpp's SectionReader::isOpaque: a position outside a
// resident chunk column counts as opaque so a section at the edge of loaded
// terrain never merges a face into an unstreamed neighbour.
static bool isOpaque(ChunkCache &cc, int_t x, int_t y, int_t z)
{
	if (y >= 0 && y < Chunk::WORLD_HEIGHT && !cc.hasResidentChunkAtBlock(x, z))
		return true;

	return isOpaqueBlockId(cc.getBlockId(x, y, z));
}

static bool sameKey(const FaceKey &a, const FaceKey &b)
{
	return a.valid && b.valid && a.texture == b.texture &&
	       a.brightness == b.brightness &&
	       a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static unsigned char quantizeColor(float value)
{
	int_t component = (int_t)(value * 255.0f);
	if (component < 0)
		component = 0;
	else if (component > 255)
		component = 255;
	return (unsigned char)component;
}

static FaceKey makeFaceKey(ChunkCache &cc, int_t face, int_t x, int_t y, int_t z)
{
	FaceKey key = { 0, 0, 0, 0, 0, false };
	const int_t id = cc.getBlockId(x, y, z);
	if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE)
		return key;

	Block *block = Block::blocksList[id];
	if (!dsi_is_greedy_cube(block))
		return key;

	int_t nx = x;
	int_t ny = y;
	int_t nz = z;
	switch (face)
	{
		case 0: --ny; break;
		case 1: ++ny; break;
		case 2: --nz; break;
		case 3: ++nz; break;
		case 4: --nx; break;
		case 5: ++nx; break;
		default: return key;
	}

	if (isOpaque(cc, nx, ny, nz))
		return key;

	const DsiBlockRenderInfo &renderInfo = dsiGetBlockRenderInfo(id);
	int_t texture = -1;
	if (renderInfo.staticTextureBySide)
	{
		texture = renderInfo.textureBySide[static_cast<std::size_t>(face)];
	}
	else
	{
		texture = block->getBlockTexture(&cc, x, y, z, face);
	}
	if (texture < 0)
		return key;

	const int_t tint = renderInfo.defaultWhiteColorMultiplier
		? 0xffffff
		: block->colorMultiplier(&cc, x, y, z);
	const float tintR = (float)(tint >> 16 & 0xff) / 255.0f;
	const float tintG = (float)(tint >> 8 & 0xff) / 255.0f;
	const float tintB = (float)(tint & 0xff) / 255.0f;
	static const float directionalShade[6] =
	{
		0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f
	};
	const float shade = directionalShade[face];

	key.texture = texture;
	key.brightness = block->getMixedBrightnessForBlock(&cc, nx, ny, nz);
	key.red = quantizeColor(shade * tintR);
	key.green = quantizeColor(shade * tintG);
	key.blue = quantizeColor(shade * tintB);
	key.valid = true;
	return key;
}

static void addVertex(Tessellator &t, tess_coord_t x, tess_coord_t y, tess_coord_t z,
	                  tess_coord_t u, tess_coord_t v)
{
	t.addVertexWithUV(x, y, z, u, v);
}

// Same atlas-tile math RenderBlocks.cpp uses for every non-merged block face
// (see e.g. its renderStandardBlockWithColorMultiplier neighbours): tile index
// -> (tileX, tileY) in 16px units, normalised against the atlas's logical
// 256x256 grid. Unlike Ps2GreedyMesh.cpp's emitQuad, u1/v1 do NOT scale with
// width/height -- see this file's header comment on why (no DS hardware
// region-repeat to tile a scaled UV span against).
static const tess_coord_t kDsiGreedyUvGuard = (tess_coord_t)0.01;

static void emitQuad(int_t face, const FaceKey &key,
	                 int_t x, int_t y, int_t z,
	                 int_t width, int_t height)
{
	++g_dsiGreedyQuadCount;
	g_dsiGreedyFacesCoveredCount += (unsigned long)(width * height);

	Tessellator &t = Tessellator::instance;
	t.setBrightness(key.brightness);
	t.setColorOpaque((int_t)key.red, (int_t)key.green, (int_t)key.blue);

	const tess_coord_t atlasU = (tess_coord_t)((key.texture & 0xf) << 4);
	const tess_coord_t atlasV = (tess_coord_t)(key.texture & 0xf0);
	const tess_coord_t u0 = atlasU / 256.0f;
	const tess_coord_t v0 = atlasV / 256.0f;
	const tess_coord_t u1 = (atlasU + (tess_coord_t)16.0f - kDsiGreedyUvGuard) / 256.0f;
	const tess_coord_t v1 = (atlasV + (tess_coord_t)16.0f - kDsiGreedyUvGuard) / 256.0f;

	const tess_coord_t fx = (tess_coord_t)x;
	const tess_coord_t fy = (tess_coord_t)y;
	const tess_coord_t fz = (tess_coord_t)z;
	const tess_coord_t fw = (tess_coord_t)width;
	const tess_coord_t fh = (tess_coord_t)height;
	// No addAxisAlignedFaceWithUVFast() here: that fast path is PLATFORM_PC_
	// LEGACY/PS2_PLATFORM-only (Tessellator.h's own guard on the declaration),
	// not available on DSi -- always take the manual per-vertex path below,
	// exactly like every other DSi block-face emitter in RenderBlocks.cpp.
	switch (face)
	{
		case 0: // Y-, width=X, height=Z
			addVertex(t, fx,      fy, fz + fh, u0, v1);
			addVertex(t, fx,      fy, fz,      u0, v0);
			addVertex(t, fx + fw, fy, fz,      u1, v0);
			addVertex(t, fx + fw, fy, fz + fh, u1, v1);
			break;
		case 1: // Y+, width=X, height=Z
			addVertex(t, fx + fw, fy + 1.0f, fz + fh, u1, v1);
			addVertex(t, fx + fw, fy + 1.0f, fz,      u1, v0);
			addVertex(t, fx,      fy + 1.0f, fz,      u0, v0);
			addVertex(t, fx,      fy + 1.0f, fz + fh, u0, v1);
			break;
		case 2: // Z-, width=X, height=Y; texture U is mirrored
			addVertex(t, fx,      fy + fh, fz, u1, v0);
			addVertex(t, fx + fw, fy + fh, fz, u0, v0);
			addVertex(t, fx + fw, fy,      fz, u0, v1);
			addVertex(t, fx,      fy,      fz, u1, v1);
			break;
		case 3: // Z+, width=X, height=Y
			addVertex(t, fx,      fy + fh, fz + 1.0f, u0, v0);
			addVertex(t, fx,      fy,      fz + 1.0f, u0, v1);
			addVertex(t, fx + fw, fy,      fz + 1.0f, u1, v1);
			addVertex(t, fx + fw, fy + fh, fz + 1.0f, u1, v0);
			break;
		case 4: // X-, width=Z, height=Y
			addVertex(t, fx, fy + fh, fz + fw, u1, v0);
			addVertex(t, fx, fy + fh, fz,      u0, v0);
			addVertex(t, fx, fy,      fz,      u0, v1);
			addVertex(t, fx, fy,      fz + fw, u1, v1);
			break;
		case 5: // X+, width=Z, height=Y; texture U is mirrored
			addVertex(t, fx + 1.0f, fy,      fz + fw, u0, v1);
			addVertex(t, fx + 1.0f, fy,      fz,      u1, v1);
			addVertex(t, fx + 1.0f, fy + fh, fz,      u1, v0);
			addVertex(t, fx + 1.0f, fy + fh, fz + fw, u0, v0);
			break;
	}
}

static void faceCellCoordinates(int_t face, int_t slice, int_t u, int_t v,
	                            int_t x0, int_t y0, int_t z0,
	                            int_t &x, int_t &y, int_t &z)
{
	if (face <= 1)
	{
		x = x0 + u;
		y = y0 + slice;
		z = z0 + v;
	}
	else if (face <= 3)
	{
		x = x0 + u;
		y = y0 + v;
		z = z0 + slice;
	}
	else
	{
		x = x0 + slice;
		y = y0 + v;
		z = z0 + u;
	}
}
} // namespace

bool dsi_is_greedy_cube(Block *block)
{
	if (block == nullptr || block->blockID < 0 || block->blockID >= Block::BLOCK_REGISTRY_SIZE)
		return false;

	// Grass uses a side overlay/tint path and stays in RenderBlocks. Leaves are
	// different: ported from upstream OptiCraft Heritage Edition's PS2 fix
	// (ps2_is_greedy_cube) -- in Fast graphics BlockLeaves::isOpaqueCube()
	// returns true (== !graphicsLevel) and already culls leaf-to-leaf faces
	// the same as any other opaque cube, so it is safe to let Fast leaves
	// through the same conservative greedy path as terrain: fewer quads and
	// faster rebuilds for dense canopies. DsiEarlyInit's GameDefaults.cpp
	// already defaults DSi to Fast graphics (fancyGraphics = false) for
	// exactly this culling behaviour, so this applies on every DSi world
	// unless the player explicitly switches to Fancy. Fancy leaves
	// (isOpaqueCube() false, translucent) still fall through to RenderBlocks
	// below, unmerged, same as before.
	if (block == static_cast<Block *>(Block::grass) ||
		Block::isBlockContainer[block->blockID])
	{
		return false;
	}

	if (block == static_cast<Block *>(Block::leaves))
		return block->isOpaqueCube();

	return dsiGetBlockRenderInfo(block->blockID).simpleOpaqueCube;
}

bool dsi_greedy_mesh_face(ChunkCache &cc, int face,
	                      int x0, int y0, int z0,
	                      int x1, int y1, int z1)
{
	if (face < 0 || face >= DSI_GREEDY_FACE_COUNT)
		return false;

	const int_t sliceCount = face <= 1 ? y1 - y0 : (face <= 3 ? z1 - z0 : x1 - x0);
	const int_t uCount = face <= 3 ? x1 - x0 : z1 - z0;
	const int_t vCount = face <= 1 ? z1 - z0 : y1 - y0;
	if (sliceCount <= 0 || sliceCount > 16 || uCount <= 0 || uCount > 16 ||
	    vCount <= 0 || vCount > 16)
		return false;

	FaceKey mask[16 * 16];
	bool used[16 * 16];
	bool emittedAny = false;
	const int_t configuredMerge = DSI_GREEDY_MAX_MERGE;
	const int_t maxMerge = configuredMerge < 1 ? 1 :
	                       (configuredMerge > 16 ? 16 : configuredMerge);

	for (int_t slice = 0; slice < sliceCount; ++slice)
	{
		for (int_t index = 0; index < 16 * 16; ++index)
		{
			mask[index].valid = false;
			used[index] = false;
		}

		for (int_t v = 0; v < vCount; ++v)
		{
			for (int_t u = 0; u < uCount; ++u)
			{
				int_t x, y, z;
				faceCellCoordinates(face, slice, u, v, x0, y0, z0, x, y, z);
				mask[v * 16 + u] = makeFaceKey(cc, face, x, y, z);
			}
		}

		for (int_t v = 0; v < vCount; ++v)
		{
			for (int_t u = 0; u < uCount; ++u)
			{
				const int_t first = v * 16 + u;
				if (used[first] || !mask[first].valid)
					continue;

				int_t width = 1;
				while (width < maxMerge && u + width < uCount)
				{
					const int_t candidate = v * 16 + u + width;
					if (used[candidate] || !sameKey(mask[first], mask[candidate]))
						break;
					++width;
				}

				int_t height = 1;
				while (height < maxMerge && v + height < vCount)
				{
					bool rowMatches = true;
					for (int_t du = 0; du < width; ++du)
					{
						const int_t candidate = (v + height) * 16 + u + du;
						if (used[candidate] || !sameKey(mask[first], mask[candidate]))
						{
							rowMatches = false;
							break;
						}
					}
					if (!rowMatches)
						break;
					++height;
				}

				for (int_t dv = 0; dv < height; ++dv)
					for (int_t du = 0; du < width; ++du)
						used[(v + dv) * 16 + u + du] = true;

				int_t x, y, z;
				faceCellCoordinates(face, slice, u, v, x0, y0, z0, x, y, z);
				emitQuad(face, mask[first], x, y, z, width, height);
				emittedAny = true;
			}
		}
	}

	return emittedAny;
}

unsigned long dsiGetTotalGreedyQuads()
{
	return g_dsiGreedyQuadCount;
}

unsigned long dsiGetTotalGreedyFacesCovered()
{
	return g_dsiGreedyFacesCoveredCount;
}

#endif // DSI_PLATFORM
