#include "RenderHelper.h"
#include "Vec3D.h"
#include "platform/PlatformTuning.h"
#include "platform/RenderAPI.h"
#include "platform/RenderLightingProfile.h"

#include <cmath>

std::vector<float> RenderHelper::field_1695_a(16);

RenderHelper::RenderHelper() {
}

void RenderHelper::disableStandardItemLighting() {
    renderDisable(RenderCapability::Lighting);
    renderDisable(RenderCapability::Light0);
    renderDisable(RenderCapability::Light1);
    renderDisable(RenderCapability::ColorMaterial);
}

namespace
{
	// enableStandardItemLighting() below used to recompute this whole struct
	// -- two Vec3D::normalize() calls (a heap-allocated Vec3D plus a sqrt
	// each) or a sqrt+divide under PLATFORM_FLOAT_VERTEX_MATH, and NINE
	// std::vector<float> heap allocations (createFloatBuffer()'s own
	// std::vector<float> return) -- from scratch on every single call. It is
	// called from the main per-frame world render path (EntityRenderer.cpp),
	// every first-person held-item frame (ItemRenderer.cpp), and every GUI
	// item render (GuiContainer.cpp/GuiInventory.cpp/GuiEnchantment.cpp/...),
	// so this ran many times a frame for a result that never changes: every
	// platform's renderGetStandardItemLightingProfile() (RenderLightingProfile_
	// {DSI,PS2,WII,PC}.cpp) returns the same hardcoded {0.4f, 0.6f} literal,
	// unconditionally, with no time-of-day or other input -- there is nothing
	// here that could ever produce a different result from one call to the
	// next. Computed once, lazily, and cached for the rest of the process.
	struct StandardItemLightingData
	{
		float light0Pos[4];
		float light0Diffuse[4];
		float light0Ambient[4];
		float light0Specular[4];
		float light1Pos[4];
		float light1Diffuse[4];
		float light1Ambient[4];
		float light1Specular[4];
		float lightModelAmbient[4];
	};

	const StandardItemLightingData &standardItemLightingData()
	{
		static const StandardItemLightingData data = []() {
			StandardItemLightingData d{};
			const RenderLightingProfile lightingProfile = renderGetStandardItemLightingProfile();
			const float f = lightingProfile.ambient;
			const float f1 = lightingProfile.diffuse;
			const float f2 = 0.0f;
#if PLATFORM_FLOAT_VERTEX_MATH
			const float lightX = 0.2f;
			const float lightY = 1.0f;
			const float lightZ = -0.7f;
			const float inverseLightLength = 1.0f / std::sqrt(lightX * lightX + lightY * lightY + lightZ * lightZ);
			d.light0Pos[0] = lightX * inverseLightLength;
			d.light0Pos[1] = lightY * inverseLightLength;
			d.light0Pos[2] = lightZ * inverseLightLength;
			d.light0Pos[3] = 0.0f;
			d.light1Pos[0] = -lightX * inverseLightLength;
			d.light1Pos[1] = lightY * inverseLightLength;
			d.light1Pos[2] = -lightZ * inverseLightLength;
			d.light1Pos[3] = 0.0f;
#else
			Vec3D *vec3d = Vec3D::createVector(0.20000000298023224, 1.0, -0.69999998807907104);
			vec3d = vec3d->normalize();
			d.light0Pos[0] = (float)vec3d->xCoord;
			d.light0Pos[1] = (float)vec3d->yCoord;
			d.light0Pos[2] = (float)vec3d->zCoord;
			d.light0Pos[3] = 0.0f;
			vec3d = Vec3D::createVector(-0.20000000298023224, 1.0, 0.69999998807907104);
			vec3d = vec3d->normalize();
			d.light1Pos[0] = (float)vec3d->xCoord;
			d.light1Pos[1] = (float)vec3d->yCoord;
			d.light1Pos[2] = (float)vec3d->zCoord;
			d.light1Pos[3] = 0.0f;
#endif
			d.light0Diffuse[0] = d.light0Diffuse[1] = d.light0Diffuse[2] = f1; d.light0Diffuse[3] = 1.0f;
			d.light0Ambient[0] = d.light0Ambient[1] = d.light0Ambient[2] = 0.0f; d.light0Ambient[3] = 1.0f;
			d.light0Specular[0] = d.light0Specular[1] = d.light0Specular[2] = f2; d.light0Specular[3] = 1.0f;
			d.light1Diffuse[0] = d.light1Diffuse[1] = d.light1Diffuse[2] = f1; d.light1Diffuse[3] = 1.0f;
			d.light1Ambient[0] = d.light1Ambient[1] = d.light1Ambient[2] = 0.0f; d.light1Ambient[3] = 1.0f;
			d.light1Specular[0] = d.light1Specular[1] = d.light1Specular[2] = f2; d.light1Specular[3] = 1.0f;
			d.lightModelAmbient[0] = d.lightModelAmbient[1] = d.lightModelAmbient[2] = f; d.lightModelAmbient[3] = 1.0f;
			return d;
		}();
		return data;
	}
}

void RenderHelper::enableStandardItemLighting() {
    renderEnable(RenderCapability::Lighting);
    renderEnable(RenderCapability::Light0);
    renderEnable(RenderCapability::Light1);
    renderEnable(RenderCapability::ColorMaterial);
    renderColorMaterial(RenderFace::FrontAndBack, RenderColorMaterialMode::AmbientAndDiffuse);
    const StandardItemLightingData &data = standardItemLightingData();
    renderLightfv(0, RenderLightParameter::Position, data.light0Pos);
    renderLightfv(0, RenderLightParameter::Diffuse, data.light0Diffuse);
    renderLightfv(0, RenderLightParameter::Ambient, data.light0Ambient);
    renderLightfv(0, RenderLightParameter::Specular, data.light0Specular);
    renderLightfv(1, RenderLightParameter::Position, data.light1Pos);
    renderLightfv(1, RenderLightParameter::Diffuse, data.light1Diffuse);
    renderLightfv(1, RenderLightParameter::Ambient, data.light1Ambient);
    renderLightfv(1, RenderLightParameter::Specular, data.light1Specular);
    renderShadeModel(RenderShadeModel::Flat);
    renderLightModelAmbient(data.lightModelAmbient);
}

void RenderHelper::enableGUIStandardItemLighting() {
    renderPushMatrix();
    renderRotate(-30.0f, 0.0f, 1.0f, 0.0f);
    renderRotate(165.0f, 1.0f, 0.0f, 0.0f);
    enableStandardItemLighting();
    renderPopMatrix();
}

