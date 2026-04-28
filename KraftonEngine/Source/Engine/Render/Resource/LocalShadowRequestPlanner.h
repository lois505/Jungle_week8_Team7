#pragma once

#include "Render/Resource/LocalShadowTypes.h"

class FSceneEnvironment;
struct FFrameContext;
struct FLightShadowSettings;

class FLocalShadowRequestPlanner
{
public:
	void BuildRequests(
		FSceneEnvironment& Environment,
		const FFrameContext& Frame,
		const FShadowResolutionPolicy& LocalResolutionPolicy,
		TArray<FLocalShadowRequest>& OutRequests) const;

	void SortRequests(TArray<FLocalShadowRequest>& InOutRequests) const;

private:
	uint32 ComputeRequestedResolution(const FLightShadowSettings& Settings, const FShadowResolutionPolicy& Policy) const;
	uint32 ClampAndAlignResolution(uint32 Resolution, const FShadowResolutionPolicy& Policy) const;
	uint64 MakeLocalShadowRequestKey(ELocalShadowRequestType RequestType, uint32 LightIndex, uint32 FaceIndex) const;
	float ComputeLocalShadowPriority(
		ELocalShadowRequestType RequestType,
		float LightIntensity,
		float CameraDistanceTerm) const;
};
