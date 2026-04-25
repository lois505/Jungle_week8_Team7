#pragma once
#include "Math/Matrix.h"
#include "Core/CoreTypes.h"
struct FLocalShadowInfo
{
	FMatrix LightViewProj[6]; //64
	FVector4 AtlasRect[6]; //16
	uint32 CastShadow; //4
	uint32 ShadowType; //4
	float Bias; //4
	float Padding;//4
};

static_assert(sizeof(FLocalShadowInfo) % 16 == 0, "FLocalShadowInfo must be 16-byte aligned for StructuredBuffer");
