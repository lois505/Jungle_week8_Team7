#pragma once
#include "Math/Matrix.h"
#include "Core/CoreTypes.h"
//This will be uploaded to StructuredBuffer.
struct FLocalShadowInfo
{
	FMatrixPOD LightViewProj[6]; //384
	FVector4 AtlasRect[6]; //96
	uint32 CastShadow; //4
	uint32 ShadowType; //4
	float Bias; //4
	float SlopeBias;//4
	float Sharpen;//4
	float pad[3];
};

static_assert(sizeof(FLocalShadowInfo) % 16 == 0, "FLocalShadowInfo must be 16-byte aligned for StructuredBuffer");
static_assert(sizeof(FLocalShadowInfo) == 512, "FLocalShadowInfo size mismatch with HLSL");
static_assert(alignof(FLocalShadowInfo) <= 16, "FLocalShadowInfo must not inherit SIMD alignment padding");
