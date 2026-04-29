#pragma once

#include "Core/CoreTypes.h"

enum class ELocalShadowRequestType : uint8
{
	Spot,
	PointFace,
};

struct FShadowResolutionRecord
{
	uint32 RequestedResolution = 0;
	uint32 AppliedResolution = 0;
};

struct FShadowResolutionPolicy
{
	uint32 BaseResolution = 1024;
	uint32 MinResolution = 64;
	uint32 MaxResolution = 4096;
	uint32 Alignment = 1;
};

struct FLocalShadowRequest
{
	ELocalShadowRequestType RequestType = ELocalShadowRequestType::Spot;
	uint32 LightIndex = 0;
	uint32 FaceIndex = 0;
	uint64 RequestKey = 0;
	FShadowResolutionRecord Resolution;
	float Priority = 0.0f;
	bool bForceRenderPriority = false;
	bool bNeedsRender = false;
	bool bAllocated = false;
	uint32 AtlasOffsetX = 0;
	uint32 AtlasOffsetY = 0;
	uint32 AtlasSizeX = 0;
	uint32 AtlasSizeY = 0;
};

struct FAtlasFreeRect
{
	uint32 OffsetX = 0;
	uint32 OffsetY = 0;
	uint32 Width = 0;
	uint32 Height = 0;
};
