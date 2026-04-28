#include "ShadowResourceManager.h"

#include "Render/Pipeline/RenderConstants.h"
#include "Render/Proxy/SceneEnvironment.h"

namespace
{
	constexpr uint32 GBaseShadowResolution = 1024;
	constexpr uint32 GMinShadowResolution = 64;
	constexpr uint32 GMaxShadowResolution = 4096;
	constexpr uint32 GLocalShadowAtlasResolution = 4096;

	uint32 ComputeShadowResolution(const FLightShadowSettings& Settings, uint32 BaseResolution)
	{
		float Scale = Settings.ShadowResolutionScale;
		if (Scale <= 1e-4f)
		{
			Scale = 1.0f;
		}

		uint32 Resolution = static_cast<uint32>(Scale * BaseResolution);
		Resolution = std::max(Resolution, static_cast<uint32>(GMinShadowResolution));
		Resolution = std::min(Resolution, static_cast<uint32>(GMaxShadowResolution));
		return Resolution;
	}

	void ReleaseDepthPart(FShadowMapResource& InMap, bool bReleaseSRV)
	{
		if (InMap.DSV)
		{
			InMap.DSV->Release();
			InMap.DSV = nullptr;
		}

		if (InMap.DepthTexture)
		{
			InMap.DepthTexture->Release();
			InMap.DepthTexture = nullptr;
		}

		if (bReleaseSRV && InMap.SRV)
		{
			InMap.SRV->Release();
			InMap.SRV = nullptr;
		}
	}

	void ReleaseColorPart(FShadowMapResource& InMap)
	{
		if (InMap.RTV)
		{
			InMap.RTV->Release();
			InMap.RTV = nullptr;
		}

		if (InMap.Texture)
		{
			InMap.Texture->Release();
			InMap.Texture = nullptr;
		}

		if (InMap.SRV)
		{
			InMap.SRV->Release();
			InMap.SRV = nullptr;
		}
	}
}

void FShadowResourceManager::Initialize(ID3D11Device* Device, ID3D11DeviceContext* Context)
{
	CachedDevice = Device;
	CachedContext = Context;
	FShadowRuntimeOptions DefaultShadowOptions;
	CreateShadowMapAtlas(GLocalShadowAtlasResolution, GLocalShadowAtlasResolution, DefaultShadowOptions);
}

void FShadowResourceManager::Release()
{
	ReleaseShadowMapResource(Atlas.Map);
	Atlas.CursorX = 0;
	Atlas.CursorY = 0;
	CurrentAtlasFilterMode = EShadowFilterMode::None;
	CurrentAtlasWidth = 0;
	CurrentAtlasHeight = 0;

	ReleaseDirectionalShadowArray();

	CachedDevice = nullptr;
	CachedContext = nullptr;
}

void FShadowResourceManager::UpdateShadowResources(FSceneEnvironment& Environment, const FShadowRuntimeOptions& ShadowOptions)
{
	EnsureShadowMapAtlas(GLocalShadowAtlasResolution, GLocalShadowAtlasResolution, ShadowOptions);

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
	(void)ShadowOptions;

	if (!Shadow.Settings.bCastShadows)
	{
		ReleaseDirectionalShadowArray();
		return;
	}

	const uint32 Resolution = ComputeShadowResolution(Shadow.Settings, BaseResolution);
	ResizeDirectionalShadowArray(Resolution, Shadow.NUM_CASCADES);
}

void FShadowResourceManager::EnsureSpotShadow(FSpotShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions)
{
	(void)BaseResolution;
	(void)ShadowOptions;

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
	(void)BaseResolution;
	(void)ShadowOptions;

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

bool FShadowResourceManager::CreateDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Width, uint32 Height, bool bCreateSRV)
{
	if (!CachedDevice || Width == 0 || Height == 0)
	{
		return false;
	}

	ReleaseDepthPart(OutMap, bCreateSRV);

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

	hr = CachedDevice->CreateTexture2D(&TexDesc, nullptr, &OutMap.DepthTexture);
	if (FAILED(hr) || !OutMap.DepthTexture)
	{
		ReleaseDepthPart(OutMap, bCreateSRV);
		return false;
	}

	D3D11_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
	DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	DSVDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	DSVDesc.Texture2D.MipSlice = 0;

	hr = CachedDevice->CreateDepthStencilView(OutMap.DepthTexture, &DSVDesc, &OutMap.DSV);
	if (FAILED(hr) || !OutMap.DSV)
	{
		ReleaseDepthPart(OutMap, bCreateSRV);
		return false;
	}

	if (bCreateSRV)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		SRVDesc.Texture2D.MostDetailedMip = 0;
		SRVDesc.Texture2D.MipLevels = 1;

		hr = CachedDevice->CreateShaderResourceView(OutMap.DepthTexture, &SRVDesc, &OutMap.SRV);
		if (FAILED(hr) || !OutMap.SRV)
		{
			ReleaseDepthPart(OutMap, bCreateSRV);
			return false;
		}
	}

	OutMap.Width = Width;
	OutMap.Height = Height;
	return true;
}

bool FShadowResourceManager::CreateVSMESMShadowMapResource(FShadowMapResource& OutMap, uint32 Width, uint32 Height)
{
	if (!CachedDevice || Width == 0 || Height == 0)
	{
		return false;
	}

	ReleaseColorPart(OutMap);

	HRESULT hr = S_OK;

	D3D11_TEXTURE2D_DESC TexDesc = {};
	TexDesc.Width = Width;
	TexDesc.Height = Height;
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
		ReleaseColorPart(OutMap);
		return false;
	}

	D3D11_RENDER_TARGET_VIEW_DESC RTVDesc = {};
	RTVDesc.Format = TexDesc.Format;
	RTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	RTVDesc.Texture2D.MipSlice = 0;

	hr = CachedDevice->CreateRenderTargetView(OutMap.Texture, &RTVDesc, &OutMap.RTV);
	if (FAILED(hr) || !OutMap.RTV)
	{
		ReleaseColorPart(OutMap);
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
		ReleaseColorPart(OutMap);
		return false;
	}

	OutMap.Width = Width;
	OutMap.Height = Height;
	return true;
}

void FShadowResourceManager::CreateDirectionalShadowArray(uint32 Resolution, int NumCascades)
{
	if (!CachedDevice || Resolution == 0)
	{
		return;
	}

	ReleaseDirectionalShadowArray();
	DirShadowArray.Width = static_cast<float>(Resolution);
	DirShadowArray.Height = static_cast<float>(Resolution);
	DirShadowArray.NumElements = NumCascades;

	D3D11_TEXTURE2D_DESC Desc = {};
	Desc.Width = Resolution;
	Desc.Height = Resolution;
	Desc.ArraySize = NumCascades + 1;
	Desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	Desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	Desc.MipLevels = 1;
	Desc.SampleDesc = { 1, 0 };
	CachedDevice->CreateTexture2D(&Desc, nullptr, &DirShadowArray.Texture);

	for (int i = 1; i <= NumCascades; ++i)
	{
		D3D11_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
		DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		DSVDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		DSVDesc.Texture2DArray = { 0, static_cast<UINT>(i), 1 };
		CachedDevice->CreateDepthStencilView(DirShadowArray.Texture, &DSVDesc, &DirShadowArray.DSVs[i]);

		D3D11_SHADER_RESOURCE_VIEW_DESC PreviewSRVDesc = {};
		PreviewSRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		PreviewSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		PreviewSRVDesc.Texture2DArray.MostDetailedMip = 0;
		PreviewSRVDesc.Texture2DArray.MipLevels = 1;
		PreviewSRVDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(i);
		PreviewSRVDesc.Texture2DArray.ArraySize = 1;
		CachedDevice->CreateShaderResourceView(DirShadowArray.Texture, &PreviewSRVDesc, &DirShadowArray.PreviewSRVs[i]);
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	SRVDesc.Texture2DArray.MostDetailedMip = 0;
	SRVDesc.Texture2DArray.MipLevels = 1;
	SRVDesc.Texture2DArray.FirstArraySlice = 0;
	SRVDesc.Texture2DArray.ArraySize = static_cast<UINT>(NumCascades + 1);
	CachedDevice->CreateShaderResourceView(DirShadowArray.Texture, &SRVDesc, &DirShadowArray.SRV);
}

void FShadowResourceManager::ResizeDepthShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution)
{
	if (OutMap.DepthTexture && OutMap.Width == Resolution && OutMap.Height == Resolution && OutMap.DSV)
	{
		return;
	}

	CreateDepthShadowMapResource(OutMap, Resolution, Resolution, true);
}

void FShadowResourceManager::ResizeVSMESMShadowMapResource(FShadowMapResource& OutMap, uint32 Resolution)
{
	if (OutMap.Texture && OutMap.Width == Resolution && OutMap.Height == Resolution && OutMap.RTV)
	{
		return;
	}

	CreateVSMESMShadowMapResource(OutMap, Resolution, Resolution);
}

void FShadowResourceManager::ResizeDirectionalShadowArray(uint32 Resolution, int NumCascades)
{
	if (DirShadowArray.Texture
		&& static_cast<uint32>(DirShadowArray.Width) == Resolution
		&& DirShadowArray.NumElements == static_cast<uint32>(NumCascades))
	{
		return;
	}

	CreateDirectionalShadowArray(Resolution, NumCascades);
}

void FShadowResourceManager::ReleaseShadowMapResource(FShadowMapResource& InMap)
{
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

	if (InMap.DepthTexture)
	{
		InMap.DepthTexture->Release();
		InMap.DepthTexture = nullptr;
	}

	if (InMap.Texture)
	{
		InMap.Texture->Release();
		InMap.Texture = nullptr;
	}

	InMap.Width = 0;
	InMap.Height = 0;
}

void FShadowResourceManager::ReleaseDirectionalShadowArray()
{
	if (DirShadowArray.Texture)
	{
		DirShadowArray.Texture->Release();
		DirShadowArray.Texture = nullptr;
	}

	if (DirShadowArray.SRV)
	{
		DirShadowArray.SRV->Release();
		DirShadowArray.SRV = nullptr;
	}

	for (int i = 0; i < 5; ++i)
	{
		if (DirShadowArray.DSVs[i])
		{
			DirShadowArray.DSVs[i]->Release();
			DirShadowArray.DSVs[i] = nullptr;
		}

		if (DirShadowArray.PreviewSRVs[i])
		{
			DirShadowArray.PreviewSRVs[i]->Release();
			DirShadowArray.PreviewSRVs[i] = nullptr;
		}
	}

	DirShadowArray.Width = 0.0f;
	DirShadowArray.Height = 0.0f;
	DirShadowArray.NumElements = 0;
}

bool FShadowResourceManager::EnsureShadowMapAtlas(uint32 Width, uint32 Height, const FShadowRuntimeOptions& ShadowOptions)
{
	if (Atlas.Map.Width == Width
		&& Atlas.Map.Height == Height
		&& CurrentAtlasWidth == Width
		&& CurrentAtlasHeight == Height
		&& CurrentAtlasFilterMode == ShadowOptions.ShadowFilterMode)
	{
		return true;
	}

	return CreateShadowMapAtlas(Width, Height, ShadowOptions);
}

bool FShadowResourceManager::CreateShadowMapAtlas(uint32 Width, uint32 Height, const FShadowRuntimeOptions& ShadowOptions)
{
	if (!CachedDevice || Width == 0 || Height == 0)
	{
		return false;
	}

	ReleaseShadowMapResource(Atlas.Map);
	Atlas.CursorX = 0;
	Atlas.CursorY = 0;

	if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM)
	{
		if (!CreateVSMESMShadowMapResource(Atlas.Map, Width, Height))
		{
			ReleaseShadowMapResource(Atlas.Map);
			return false;
		}

		if (!CreateDepthShadowMapResource(Atlas.Map, Width, Height, false))
		{
			ReleaseShadowMapResource(Atlas.Map);
			return false;
		}
	}
	else
	{
		if (!CreateDepthShadowMapResource(Atlas.Map, Width, Height, true))
		{
			ReleaseShadowMapResource(Atlas.Map);
			return false;
		}
	}

	Atlas.CursorX = 0;
	Atlas.CursorY = 0;
	CurrentAtlasFilterMode = ShadowOptions.ShadowFilterMode;
	CurrentAtlasWidth = Width;
	CurrentAtlasHeight = Height;
	Level = 0;
	return true;
}

FAtlasResourceInfo FShadowResourceManager::AllocateFromAtlas()
{
	FAtlasResourceInfo Info;

	if (!Atlas.Map.DepthTexture || Atlas.Map.Width == 0 || Atlas.Map.Height == 0)
	{
		return Info;
	}

	if (Atlas.CursorX + AtlasAllocSizeX > Atlas.Map.Width)
	{
		Atlas.CursorX = 0;
		Atlas.CursorY += AtlasAllocSizeY;
		Level++;
	}

	if (Atlas.CursorY + AtlasAllocSizeY > Atlas.Map.Height)
	{
		return Info;
	}

	Info.OffsetX = Atlas.CursorX;
	Info.OffsetY = Atlas.CursorY;
	Info.Width = AtlasAllocSizeX;
	Info.Height = AtlasAllocSizeY;
	Info.Index = (Atlas.CursorY / AtlasAllocSizeY) * (Atlas.Map.Width / AtlasAllocSizeX) + (Atlas.CursorX / AtlasAllocSizeX);
	Info.bAllocated = true;

	Atlas.CursorX += AtlasAllocSizeX;
	return Info;
}

bool FShadowResourceManager::ClearAtlas(const FShadowRuntimeOptions& ShadowOptions)
{
	if (!CachedContext || !Atlas.Map.DSV)
	{
		return false;
	}

	CachedContext->ClearDepthStencilView(Atlas.Map.DSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);

	if ((ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM)
		&& Atlas.Map.RTV)
	{
		const float ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
		CachedContext->ClearRenderTargetView(Atlas.Map.RTV, ClearColor);
	}

	Atlas.CursorX = 0;
	Atlas.CursorY = 0;
	Level = 0;
	return true;
}
