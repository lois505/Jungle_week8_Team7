#pragma once
#include "Render/Device/D3DDevice.h"
#include "Render/Resource/LocalShadowAllocationExecutor.h"
#include "Render/Resource/LocalShadowRequestPlanner.h"
#include "Render/Types/ShadowData.h"
#include "Render/Resource/ShadowAtlasResource.h"
struct FSpotShadowData;
struct FDirectionalShadowData;
class FSceneEnvironment;
class FScene;
struct FFrameContext;

struct FShadowTelemetry
{
	uint32 RequestedLocalViewCount = 0;
	uint32 AllocatedLocalViewCount = 0;
	uint32 FailedShadowViewCount = 0;
	uint64 UsedLocalShadowAtlasAreaPerFrame = 0;
	uint64 LocalAtlasTotalArea = 0;
	uint64 EstimatedLocalShadowVRAMBytes = 0;
	uint64 EstimatedDirectionalShadowVRAMBytes = 0;
	uint64 EstimatedShadowVRAMBytes = 0;
	uint32 DirectionalShadowCascadeSliceCount = 0;
	uint32 DirectionalShadowArraySliceCount = 0;
	uint32 NumDirectionalLights = 0;
	uint32 NumPointLights = 0;
	uint32 NumSpotLights = 0;
};

class FShadowResourceManager
{
public:
	void Initialize(ID3D11Device* Device, ID3D11DeviceContext* Context);
	void Release();

	void UpdateShadowResources(FSceneEnvironment& Environment, const FShadowRuntimeOptions& ShadowOptions, const FFrameContext& Frame);

	//Functions Related To ShadowMap Atlas
	FShadowAtlasResource& GetAtlas() { return Atlas; }
	const FShadowAtlasResource& GetAtlas() const { return Atlas; }
	FAtlasResourceInfo AllocateFromAtlas(uint32 RequestWidth, uint32 RequestHeight);
	void AllocateLocalShadowViews(FSceneEnvironment& Environment, const FScene& Scene);
	bool ClearAtlas(const FShadowRuntimeOptions& ShadowOptions);
	bool ClearAtlasTexturesOnly(const FShadowRuntimeOptions& ShadowOptions);
	void ResetAtlasAllocationStateForFrame();

	FDirectionalShadowArray& GetShadowArray() { return DirShadowArray; }
	const FDirectionalShadowArray& GetShadowArray() const { return DirShadowArray; }

	const FShadowTelemetry& GetTelemetry() const { return Telemetry; }
	const TArray<FLocalShadowRequest>& GetLocalShadowRequests() const { return LocalShadowRequests; }

	void SetMaxLocalShadowViewsPerFrame(uint32 MaxViewsPerFrame) { MaxLocalShadowViewsPerFrame = MaxViewsPerFrame; }
	uint32 GetMaxLocalShadowViewsPerFrame() const { return MaxLocalShadowViewsPerFrame; }

	void SetMaxLocalShadowAtlasAreaPerFrame(uint64 MaxAreaPerFrame) { MaxLocalShadowAtlasAreaPerFrame = MaxAreaPerFrame; }
	uint64 GetMaxLocalShadowAtlasAreaPerFrame() const { return MaxLocalShadowAtlasAreaPerFrame; }

	void SetLocalShadowAlignment(uint32 InAlignment);
	uint32 GetLocalShadowAlignment() const { return LocalResolutionPolicy.Alignment; }

private:
	bool EnsureShadowMapAtlas(uint32 Width, uint32 Height, const FShadowRuntimeOptions& ShadowOptions);
	bool CreateShadowMapAtlas(uint32 Width, uint32 Height, const FShadowRuntimeOptions& ShadowOptions);

	void EnsureDirectionalShadow(FDirectionalShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsureSpotShadow(FSpotShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsurePointShadow(FPointShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);

	void UpdateTelemetry(const FSceneEnvironment& Environment);

	bool ClearAtlasTextures(const FShadowRuntimeOptions& ShadowOptions);
	void ResetAtlasAllocationState();

	uint32 ComputeRequestedResolution(const FLightShadowSettings& Settings, const FShadowResolutionPolicy& Policy) const;

	bool CreateDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Width, uint32 Height, bool bCreateSRV);
	bool CreateVSMESMShadowMapResource(FShadowMapResource& OutMap, uint32 Width, uint32 Height);
	void CreateDirectionalShadowArray(uint32 Resolution, int NumCascades, bool bUseMomentResources);

	void ResizeDirectionalShadowArray(uint32 Resolution, int NumCascades, bool bUseMomentResources);

	void ReleaseShadowMapResource(FShadowMapResource& InMap);
	void ReleaseDirectionalShadowArray();

private:
	ID3D11Device* CachedDevice = nullptr;
	ID3D11DeviceContext* CachedContext = nullptr;

	FDirectionalShadowArray DirShadowArray;
	FShadowAtlasResource Atlas;

	TArray<FLocalShadowRequest> LocalShadowRequests;

	FShadowTelemetry Telemetry;

	EShadowFilterMode CurrentAtlasFilterMode = EShadowFilterMode::None;

	uint32 CurrentAtlasWidth = 0;
	uint32 CurrentAtlasHeight = 0;
	bool bDirectionalMomentResourcesEnabled = false;

	FLocalShadowAtlasAllocator LocalAtlasAllocator;
	FLocalShadowRequestPlanner LocalShadowRequestPlanner;
	FLocalShadowAllocationExecutor LocalShadowAllocationExecutor;

	uint32 MaxLocalShadowViewsPerFrame = 0; // 0 means no budget cap
	uint64 MaxLocalShadowAtlasAreaPerFrame = 0; // 0 means no area budget cap

	FShadowResolutionPolicy DirectionalResolutionPolicy = {};
	FShadowResolutionPolicy LocalResolutionPolicy = {};
};
