#pragma once

#include "Render/Resource/LocalShadowAtlasAllocator.h"
#include "Render/Resource/ShadowAtlasResource.h"

class FScene;
class FSceneEnvironment;
struct FShadowViewData;

class FLocalShadowAllocationExecutor
{
public:
	void AllocateViews(
		FSceneEnvironment& Environment,
		const FScene& Scene,
		const FShadowResolutionPolicy& LocalResolutionPolicy,
		uint32 MaxLocalShadowViewsPerFrame,
		uint64 MaxLocalShadowAtlasAreaPerFrame,
		const FShadowAtlasResource& Atlas,
		FLocalShadowAtlasAllocator& LocalAtlasAllocator,
		TArray<FLocalShadowRequest>& InOutRequests) const;

private:
	void ClearLocalShadowViewMetadata(FShadowViewData& View) const;
	void ResetLocalShadowViewAllocationState(FSceneEnvironment& Environment) const;
	void ResetLocalShadowRequestAllocationState(FLocalShadowRequest& Request) const;
	void PruneInvalidOrEmptyPointFaceRequests(FSceneEnvironment& Environment, const FScene& Scene, TArray<FLocalShadowRequest>& InOutRequests) const;
	uint32 ClampAndAlignResolution(uint32 Resolution, const FShadowResolutionPolicy& Policy) const;
	uint32 ComputeNextLowerResolution(uint32 Resolution, const FShadowResolutionPolicy& Policy) const;
	bool TryAllocateAtlasRectForRequest(
		const FShadowResolutionPolicy& LocalResolutionPolicy,
		FLocalShadowAtlasAllocator& LocalAtlasAllocator,
		FLocalShadowRequest& Request,
		FAtlasResourceInfo& OutInfo) const;
	void ApplyAtlasAllocationToShadowView(FSceneEnvironment& Environment, FLocalShadowRequest& Request, const FAtlasResourceInfo& Info) const;
	void TryAllocateLocalShadowRequest(
		FSceneEnvironment& Environment,
		const FShadowResolutionPolicy& LocalResolutionPolicy,
		FLocalShadowAtlasAllocator& LocalAtlasAllocator,
		FLocalShadowRequest& Request) const;
};
