#pragma once
#include "Render/Device/D3DDevice.h"
#include "Render/Types/ShadowData.h"
#include "Render/Resource/ShadowAtlasResource.h"
struct FSpotShadowData;
struct FDirectionalShadowData;
class FSceneEnvironment;


class FShadowResourceManager
{
public:
	void Initialize(ID3D11Device* Device, ID3D11DeviceContext* Context);
	void Release();

	void UpdateShadowResources(FSceneEnvironment& Environment, const FShadowRuntimeOptions& ShadowOptions);

	//Functions Related To ShadowMap Atlas
	FShadowAtlasResource& GetAtlas() { return Atlas; }
	const FShadowAtlasResource& GetAtlas() const { return Atlas; }
	FAtlasResourceInfo AllocateFromAtlas();
	bool ClearAtlas();

	FDirectionalShadowArray& GetShadowArray() { return DirShadowArray; }
	const FDirectionalShadowArray& GetShadowArray() const { return DirShadowArray; }

private:
	bool CreateShadowMapAtlas(int Width, int Height);
	void EnsureDirectionalShadow(FDirectionalShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsureSpotShadow(FSpotShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsurePointShadow(FPointShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);

	bool CreateDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	bool CreateVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	void CreateDirectionalShadowArray(uint32 Resolution, int NumCascades);

	void ResizeDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	void ResizeVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	void ResizeDirectionalShadowArray(uint32 Resolution, int NumCascades);

	void ReleaseShadowMapResource(FShadowMapResource& InMap);
	void ReleaseDirectionalShadowArray();

private:
	ID3D11Device* CachedDevice = nullptr;
	ID3D11DeviceContext* CachedContext = nullptr;
	
	FDirectionalShadowArray DirShadowArray;
	FShadowAtlasResource Atlas;

	uint32 Level = 0;
	uint32 AtlasAllocSizeX = 512;
	uint32 AtlasAllocSizeY = 512;
};
