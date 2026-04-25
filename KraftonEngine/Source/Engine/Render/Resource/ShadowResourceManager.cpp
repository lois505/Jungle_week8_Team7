#include "ShadowResourceManager.h"

#include "Render/Pipeline/RenderConstants.h"
#include "Render/Proxy/SceneEnvironment.h"

//	Resolution 관련 Helper
namespace
{
	constexpr uint32 GBaseShadowResolution = 1024;
	constexpr uint32 GMinShadowResolution = 64;
	constexpr uint32 GMaxShadowResolution = 4096;

	uint32 ComputeShadowResolution(const FLightShadowSettings& Settings, uint32 BaseResolution)
	{
		float Scale = Settings.ShadowResolutionScale;
		if (Scale <= 1e-4f)
		{
			//	1로 고정
			Scale = 1.0f;
		}

		uint32 Resolution = static_cast<uint32>(Scale * BaseResolution);

		Resolution = std::max(Resolution, static_cast<uint32>(GMinShadowResolution));
		Resolution = std::min(Resolution, static_cast<uint32>(GMaxShadowResolution));

		return Resolution;
	}
}

void FShadowResourceManager::Initialize(ID3D11Device* Device, ID3D11DeviceContext* Context)
{
	CachedDevice = Device;
	CachedContext = Context;
	CreateShadowMapAtlas(4096, 4096);
}

void FShadowResourceManager::Release()
{
	if (Atlas.Texture)
	{
		Atlas.Texture->Release();
		Atlas.Texture = nullptr;
	}
	if (Atlas.DSV)
	{
		Atlas.DSV->Release();
		Atlas.DSV = nullptr;
	}
	if (Atlas.SRV)
	{
		Atlas.SRV->Release();
		Atlas.SRV = nullptr;
	}
	Atlas.Width = 0;
	Atlas.Height = 0;
	Atlas.CursorX = 0;
	Atlas.CursorY = 0;

	CachedDevice = nullptr;
	CachedContext = nullptr;
}

void FShadowResourceManager::UpdateShadowResources(FSceneEnvironment& Environment, const FShadowRuntimeOptions& ShadowOptions)
{
	if (Environment.HasGlobalDirectionalLight())
	{
		EnsureDirectionalShadow(Environment.GetGlobalDirectionalLightParams().ShadowData, GBaseShadowResolution, ShadowOptions);
	}

	const uint32 NumPointLights = Environment.GetNumPointLights();
	for (uint32 i = 0; i < NumPointLights; ++i)
	{
		EnsurePointShadow(Environment.GetPointLight(i).ShadowData, GBaseShadowResolution, ShadowOptions);
	}

	const uint32 NumSpotLights = Environment.GetNumSpotLights();
	for (uint32 i = 0; i < NumSpotLights; ++i)
	{
		EnsureSpotShadow(Environment.GetSpotLight(i).ShadowData, GBaseShadowResolution, ShadowOptions);
	}
}


void FShadowResourceManager::EnsureDirectionalShadow(FDirectionalShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions)
{
	if (!Shadow.Settings.bCastShadows)
	{
		ReleaseShadowMapResource(Shadow.View.DepthMap);
		return;
	}

	const uint32 Resolution = ComputeShadowResolution(Shadow.Settings, BaseResolution);

	if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM)
	{
		ResizeVSMShadowMapResource(Shadow.View.DepthMap, Resolution);
	}
	else
	{
		ResizeDepthShadowMapResource(Shadow.View.DepthMap, Resolution);
	}
}


void FShadowResourceManager::EnsureSpotShadow(FSpotShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions)
{
	if (!Shadow.Settings.bCastShadows)
	{
		ReleaseShadowMapResource(Shadow.View.DepthMap);
		Shadow.View.bAtlasAllocated = false;
		return;
	}

	ReleaseShadowMapResource(Shadow.View.DepthMap);
}


void FShadowResourceManager::EnsurePointShadow(FPointShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions)
{
	if (!Shadow.Settings.bCastShadows)
	{
		for (int32 i = 0; i < 6; ++i)
		{
			ReleaseShadowMapResource(Shadow.View[i].DepthMap);
			Shadow.View[i].bAtlasAllocated = false;
		}
		return;
	}

	for (int32 i = 0; i < 6; ++i)
	{
		ReleaseShadowMapResource(Shadow.View[i].DepthMap);
	}
}

bool FShadowResourceManager::CreateDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution)
{
	if (!CachedDevice || Resolution == 0)
	{
		return false;
	}

	ReleaseShadowMapResource(OutMap);

	HRESULT hr = S_OK;

	D3D11_TEXTURE2D_DESC TexDesc = {};
	TexDesc.Width = Resolution;
	TexDesc.Height = Resolution;
	TexDesc.MipLevels = 1;
	TexDesc.ArraySize = 1;
	TexDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	TexDesc.SampleDesc.Count = 1;
	TexDesc.SampleDesc.Quality = 0;
	TexDesc.Usage = D3D11_USAGE_DEFAULT;
	TexDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	TexDesc.CPUAccessFlags = 0;
	TexDesc.MiscFlags = 0;

	hr = CachedDevice->CreateTexture2D(&TexDesc, nullptr, &OutMap.Texture);
	if (FAILED(hr) || !OutMap.Texture)
	{
		ReleaseShadowMapResource(OutMap);
		return false;
	}

	D3D11_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
	DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	DSVDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	DSVDesc.Texture2D.MipSlice = 0;

	hr = CachedDevice->CreateDepthStencilView(OutMap.Texture, &DSVDesc, &OutMap.DSV);
	if (FAILED(hr) || !OutMap.DSV)
	{
		ReleaseShadowMapResource(OutMap);
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	SRVDesc.Texture2D.MostDetailedMip = 0;
	SRVDesc.Texture2D.MipLevels = 1;

	hr = CachedDevice->CreateShaderResourceView(OutMap.Texture, &SRVDesc, &OutMap.SRV);
	if (FAILED(hr) || !OutMap.SRV)
	{
		ReleaseShadowMapResource(OutMap);
		return false;
	}

	OutMap.Width = Resolution;
	OutMap.Height = Resolution;
	return true;
}



bool FShadowResourceManager::CreateVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution)
{
	if (!CachedDevice || Resolution == 0)
	{
		return false;
	}

	ReleaseShadowMapResource(OutMap);

	HRESULT hr = S_OK;

	D3D11_TEXTURE2D_DESC TexDesc = {};
	TexDesc.Width = Resolution;
	TexDesc.Height = Resolution;
	TexDesc.MipLevels = 1;
	TexDesc.ArraySize = 1;
	TexDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
	TexDesc.SampleDesc.Count = 1;
	TexDesc.SampleDesc.Quality = 0;
	TexDesc.Usage = D3D11_USAGE_DEFAULT;
	TexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	TexDesc.CPUAccessFlags = 0;
	TexDesc.MiscFlags = 0;

	hr = CachedDevice->CreateTexture2D(&TexDesc, nullptr, &OutMap.Texture);
	if (FAILED(hr) || !OutMap.Texture)
	{
		ReleaseShadowMapResource(OutMap);
		return false;
	}

	D3D11_RENDER_TARGET_VIEW_DESC RTVDesc = {};
	RTVDesc.Format = TexDesc.Format;
	RTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	RTVDesc.Texture2D.MipSlice = 0;

	hr = CachedDevice->CreateRenderTargetView(OutMap.Texture, &RTVDesc, &OutMap.RTV);
	if (FAILED(hr) || !OutMap.RTV)
	{
		ReleaseShadowMapResource(OutMap);
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Format = TexDesc.Format;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	SRVDesc.Texture2D.MostDetailedMip = 0;
	SRVDesc.Texture2D.MipLevels = 1;

	hr = CachedDevice->CreateShaderResourceView(OutMap.Texture, &SRVDesc, &OutMap.SRV);
	if (FAILED(hr) || !OutMap.SRV)
	{
		ReleaseShadowMapResource(OutMap);
		return false;
	}

	OutMap.Width = Resolution;
	OutMap.Height = Resolution;
	return true;
}


void FShadowResourceManager::ResizeDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution)
{
	if (OutMap.Texture && OutMap.Width == Resolution && OutMap.Height == Resolution && OutMap.DSV)
	{
		return;
	}

	CreateDepthShadowMapResource(OutMap, Resolution);
}

void FShadowResourceManager::ResizeVSMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution)
{
	if (OutMap.Texture && OutMap.Width == Resolution && OutMap.Height == Resolution && OutMap.RTV)
	{
		return;
	}

	CreateVSMShadowMapResource(OutMap, Resolution);
}


void FShadowResourceManager::ReleaseShadowMapResource(FShadowMapResource& InMap)
{
	if (InMap.Texture)
	{
		InMap.Texture->Release();
		InMap.Texture = nullptr;
	}

	if (InMap.RTV)
	{
		InMap.RTV->Release();
		InMap.RTV = nullptr;
	}

	if (InMap.DSV)
	{
		InMap.DSV->Release();
		InMap.DSV = nullptr;
	}

	if (InMap.SRV)
	{
		InMap.SRV->Release();
		InMap.SRV = nullptr;
	}

	InMap.Width = 0;
	InMap.Height = 0;
}

bool FShadowResourceManager::CreateShadowMapAtlas(int Width, int Height)
{
	if (!CachedDevice || Width <= 0 || Height <= 0)
	{
		return false;
	}

	if (Atlas.Texture)
	{
		Atlas.Texture->Release();
		Atlas.Texture = nullptr;
	}
	if (Atlas.DSV)
	{
		Atlas.DSV->Release();
		Atlas.DSV = nullptr;
	}
	if (Atlas.SRV)
	{
		Atlas.SRV->Release();
		Atlas.SRV = nullptr;
	}
	Atlas.Width = 0;
	Atlas.Height = 0;
	Atlas.CursorX = 0;
	Atlas.CursorY = 0;

	HRESULT hr = S_OK;

	D3D11_TEXTURE2D_DESC TexDesc = {};
	TexDesc.Width = Width;
	TexDesc.Height = Height;
	TexDesc.MipLevels = 1;
	TexDesc.ArraySize = 1;
	TexDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	TexDesc.SampleDesc.Count = 1;
	TexDesc.SampleDesc.Quality = 0;
	TexDesc.Usage = D3D11_USAGE_DEFAULT;
	TexDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	TexDesc.CPUAccessFlags = 0;
	TexDesc.MiscFlags = 0;

	hr = CachedDevice->CreateTexture2D(&TexDesc, nullptr, &Atlas.Texture);
	if (FAILED(hr) || !Atlas.Texture)
	{
		return false;
	}

	D3D11_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
	DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	DSVDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	DSVDesc.Texture2D.MipSlice = 0;

	hr = CachedDevice->CreateDepthStencilView(Atlas.Texture, &DSVDesc, &Atlas.DSV);
	if (FAILED(hr) || !Atlas.DSV)
	{
		if (Atlas.Texture)
		{
			Atlas.Texture->Release();
			Atlas.Texture = nullptr;
		}
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	SRVDesc.Texture2D.MostDetailedMip = 0;
	SRVDesc.Texture2D.MipLevels = 1;

	hr = CachedDevice->CreateShaderResourceView(Atlas.Texture, &SRVDesc, &Atlas.SRV);
	if (FAILED(hr) || !Atlas.SRV)
	{
		if (Atlas.DSV)
		{
			Atlas.DSV->Release();
			Atlas.DSV = nullptr;
		}
		if (Atlas.Texture)
		{
			Atlas.Texture->Release();
			Atlas.Texture = nullptr;
		}
		return false;
	}

	Atlas.Width = Width;
	Atlas.Height = Height;
	Atlas.CursorX = 0;
	Atlas.CursorY = 0;
	return true;
}

FAtlasResourceInfo FShadowResourceManager::AllocateFromAtlas()
{
	FAtlasResourceInfo Info;

	if (!Atlas.Texture || Atlas.Width == 0 || Atlas.Height == 0)
	{
		return Info;
	}

	if (Atlas.CursorX + AtlasAllocSizeX > Atlas.Width)
	{
		Atlas.CursorX = 0;
		Atlas.CursorY += AtlasAllocSizeY;
		Level++;
	}

	if (Atlas.CursorY + AtlasAllocSizeY > Atlas.Height)
	{
		return Info;
	}

	Info.OffsetX = Atlas.CursorX;
	Info.OffsetY = Atlas.CursorY;
	Info.Width = AtlasAllocSizeX;
	Info.Height = AtlasAllocSizeY;
	Info.Index = (Atlas.CursorY / AtlasAllocSizeY) * (Atlas.Width / AtlasAllocSizeX) + (Atlas.CursorX / AtlasAllocSizeX);
	Info.bAllocated = true;

	Atlas.CursorX += AtlasAllocSizeX;

	return Info;
}

bool FShadowResourceManager::ClearAtlas()
{
	if (!CachedContext || !Atlas.DSV)
	{
		return false;
	}

	CachedContext->ClearDepthStencilView(Atlas.DSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
	Atlas.CursorX = 0;
	Atlas.CursorY = 0;
	Level = 0;
	return true;
}
