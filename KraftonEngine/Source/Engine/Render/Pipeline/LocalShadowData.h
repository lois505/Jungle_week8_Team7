#pragma once
#include "Math/Matrix.h"
#include "Core/CoreTypes.h"
struct FShadowInfo
{
	FMatrix LightViewProj[6]; //64
	FVector4 AtlasRect[6]; //16
	uint32 CastShadow; //4
	uint32 ShadowType; //4
	float Bias; //4
	float Padding;//4
};