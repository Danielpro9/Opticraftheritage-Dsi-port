#pragma once

#include "ModelZombie.h"

// net.minecraft.src.ModelSkeleton
class ModelSkeleton : public ModelZombie
{
public:
	ModelSkeleton();
	void render(float f, float f1, float f2, float f3, float f4, float f5) override;
};
