#pragma once
#include "Render/Device/D3DDevice.h"
#include "Render/Types/ShadowData.h"

struct FSpotShadowData;
struct FDirectionalShadowData;
class FSceneEnvironment;

struct FShadowAtlasResource
{
	ID3D11Texture2D* Texture = nullptr;
	ID3D11DepthStencilView* DSV = nullptr;
	ID3D11ShaderResourceView* SRV = nullptr;

	uint32 Width = 0;
	uint32 Height = 0;

	uint32 CursorX = 0;
	uint32 CursorY = 0;
};

struct FAtlasResourceInfo
{
	uint32 OffsetX = 0;
	uint32 OffsetY = 0;
	uint32 Width = 0;
	uint32 Height = 0;
	uint32 Index = 0;
	bool bAllocated = false;
};

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
private:
	bool CreateShadowMapAtlas(int Width, int Height);
	void EnsureDirectionalShadow(FDirectionalShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsureSpotShadow(FSpotShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);
	void EnsurePointShadow(FPointShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions);

	bool CreateDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	bool CreateVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);

	void ResizeDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);
	void ResizeVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution);

	void ReleaseShadowMapResource(FShadowMapResource& InMap);





private:
	ID3D11Device* CachedDevice = nullptr;
	ID3D11DeviceContext* CachedContext = nullptr;


	FShadowAtlasResource Atlas;
	uint32 Level = 0;
	uint32 AtlasAllocSizeX = 512;
	uint32 AtlasAllocSizeY = 512;
};
