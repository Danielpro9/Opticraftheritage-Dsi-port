#include "ModelSkeleton.h"

#include "ModelRenderer.h"
#include "platform/PlatformConfig.h"
#include "platform/RenderAPI.h"

ModelSkeleton::ModelSkeleton()
{
	float f = 0.0f;
	delete bipedRightArm; // Java reassignment relied on GC
	bipedRightArm = new ModelRenderer(40, 16);
	bipedRightArm->addBox(-1.0f, -2.0f, -1.0f, 2, 12, 2, f);
	bipedRightArm->setRotationPoint(-5.0f, 2.0f, 0.0f);
	delete bipedLeftArm; // Java reassignment relied on GC
	bipedLeftArm = new ModelRenderer(40, 16);
	bipedLeftArm->mirror = true;
	bipedLeftArm->addBox(-1.0f, -2.0f, -1.0f, 2, 12, 2, f);
	bipedLeftArm->setRotationPoint(5.0f, 2.0f, 0.0f);
	delete bipedRightLeg; // Java reassignment relied on GC
	bipedRightLeg = new ModelRenderer(0, 16);
	bipedRightLeg->addBox(-1.0f, 0.0f, -1.0f, 2, 12, 2, f);
	bipedRightLeg->setRotationPoint(-2.0f, 12.0f, 0.0f);
	delete bipedLeftLeg; // Java reassignment relied on GC
	bipedLeftLeg = new ModelRenderer(0, 16);
	bipedLeftLeg->mirror = true;
	bipedLeftLeg->addBox(-1.0f, 0.0f, -1.0f, 2, 12, 2, f);
	bipedLeftLeg->setRotationPoint(2.0f, 12.0f, 0.0f);
	aimedBow = true;
}

void ModelSkeleton::render(float f, float f1, float f2, float f3, float f4, float f5)
{
#if PLATFORM_DSI
	// RenderLiving disables face culling globally before rendering any model,
	// for legacy-model compatibility (some mirrored/odd-wound parts would
	// otherwise render with missing faces). ModelBox always closes that gap
	// itself though, mirrored or not: its constructor calls flipFace() on
	// every quad of a mirrored box specifically to keep winding consistent
	// (confirmed by reading ModelBox.cpp directly, not assumed) -- so a
	// skeleton, built entirely from ModelBox parts, is safe to cull. Doing so
	// submits only the visible half of every closed box instead of both,
	// cutting this model's vertex count roughly in half for every skeleton
	// on screen -- real savings on this platform's own established
	// bottleneck (per-vertex GPU processing of whatever gets submitted).
	// Scoped to DSi only, ported as a general technique rather than copying
	// PS2's own version (which overrides the Entity*-taking render()
	// overload instead of this one and targets PS2's native draw calls
	// directly) -- this override matches the one ModelBiped itself already
	// provides, which ModelBase::render(Entity*, ...)'s default
	// implementation already forwards to via virtual dispatch.
	renderCullFace(RenderFace::Back);
	renderEnable(RenderCapability::CullFace);
	ModelBiped::render(f, f1, f2, f3, f4, f5);
	renderDisable(RenderCapability::CullFace);
#else
	ModelBiped::render(f, f1, f2, f3, f4, f5);
#endif
}
