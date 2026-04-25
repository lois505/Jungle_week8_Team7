#pragma once
#include "Render/Device/D3DDevice.h"
#include "Render/Types/ShadowData.h"

struct FSpotShadowData;
struct FDirectionalShadowData;
class FSceneEnvironment;

class FShadowResourceManager
{
public:
	void Initialize(ID3D11Device* Device);
	void Release();
	
	void UpdateShadowResources(FSceneEnvironment & Environment, const FShadowRuntimeOptions& ShadowOptions);
	
private:
	void EnsureDirectionalShadow(FDirectionalShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsureSpotShadow(FSpotShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsurePointShadow(FPointShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);

	bool CreateDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	bool CreateVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);

	void ResizeDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	void ResizeVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);

	void ReleaseShadowMapResource(FShadowMapResource& InMap);

	
private:
	ID3D11Device * CachedDevice = nullptr;
};
