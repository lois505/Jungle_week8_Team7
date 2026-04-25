#pragma once
#include "Core/CoreTypes.h"
#include "Math/Vector.h"

struct FLocalShadowInfo
{
	float LightViewProj[6][4][4];
	FVector4 AtlasRect[6];
	uint32 CastShadow;
	uint32 ShadowType;
	float Bias;
	float Padding;
};

static_assert(sizeof(FLocalShadowInfo) % 16 == 0, "FLocalShadowInfo must be 16-byte aligned for StructuredBuffer");
static_assert(sizeof(FLocalShadowInfo) == 496, "FLocalShadowInfo size mismatch with HLSL");
