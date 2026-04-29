#include "ShadowResourceManager.h"

#include "Render/Pipeline/RenderConstants.h"
#include "Render/Proxy/SceneEnvironment.h"
#include <algorithm>

namespace
{
	constexpr uint32 GBaseShadowResolution = 1024;
	constexpr uint32 GLocalShadowAtlasResolution = 4096;

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

void FLocalShadowAtlasAllocator::Reset(uint32 AtlasWidth, uint32 AtlasHeight)
{
	NextIndex = 0;
	FreeRects.clear();

	if (AtlasWidth > 0 && AtlasHeight > 0)
	{
		InsertFreeRect(0, 0, AtlasWidth, AtlasHeight);
	}
}

bool FLocalShadowAtlasAllocator::IsContainedIn(const FAtlasFreeRect& A, const FAtlasFreeRect& B)
{
	return A.OffsetX >= B.OffsetX
		&& A.OffsetY >= B.OffsetY
		&& (A.OffsetX + A.Width) <= (B.OffsetX + B.Width)
		&& (A.OffsetY + A.Height) <= (B.OffsetY + B.Height);
}

void FLocalShadowAtlasAllocator::RemoveAtSwap(TArray<FAtlasFreeRect>& InFreeRects, size_t Index)
{
	const size_t LastIndex = InFreeRects.size() - 1;
	if (Index != LastIndex)
	{
		InFreeRects[Index] = InFreeRects[LastIndex];
	}
	InFreeRects.pop_back();
}

FAtlasResourceInfo FLocalShadowAtlasAllocator::Allocate(uint32 RequestWidth, uint32 RequestHeight)
{
	FAtlasResourceInfo Info = {};

	if (RequestWidth == 0 || RequestHeight == 0 || FreeRects.empty())
	{
		return Info;
	}

	uint32 BestRectIndex = UINT32_MAX;
	uint64 BestWastedArea = UINT64_MAX;

	for (uint32 i = 0; i < static_cast<uint32>(FreeRects.size()); ++i)
	{
		const FAtlasFreeRect& Rect = FreeRects[i];
		if (Rect.Width < RequestWidth || Rect.Height < RequestHeight)
		{
			continue;
		}

		const uint64 WastedArea = static_cast<uint64>(Rect.Width) * static_cast<uint64>(Rect.Height)
			- static_cast<uint64>(RequestWidth) * static_cast<uint64>(RequestHeight);

		if (WastedArea < BestWastedArea)
		{
			BestRectIndex = i;
			BestWastedArea = WastedArea;
		}
	}

	if (BestRectIndex == UINT32_MAX)
	{
		return Info;
	}

	const FAtlasFreeRect Chosen = FreeRects[BestRectIndex];
	RemoveAtSwap(FreeRects, BestRectIndex);

	Info.OffsetX = Chosen.OffsetX;
	Info.OffsetY = Chosen.OffsetY;
	Info.Width = RequestWidth;
	Info.Height = RequestHeight;
	Info.Index = NextIndex++;
	Info.bAllocated = true;

	const uint32 RemainingWidth = Chosen.Width - RequestWidth;
	const uint32 RemainingHeight = Chosen.Height - RequestHeight;
	const bool bSplitVerticalFirst = RemainingWidth > RemainingHeight;

	if (bSplitVerticalFirst)
	{
		InsertFreeRect(Chosen.OffsetX + RequestWidth, Chosen.OffsetY, RemainingWidth, Chosen.Height);
		InsertFreeRect(Chosen.OffsetX, Chosen.OffsetY + RequestHeight, RequestWidth, RemainingHeight);
	}
	else
	{
		InsertFreeRect(Chosen.OffsetX + RequestWidth, Chosen.OffsetY, RemainingWidth, RequestHeight);
		InsertFreeRect(Chosen.OffsetX, Chosen.OffsetY + RequestHeight, Chosen.Width, RemainingHeight);
	}

	return Info;
}

void FLocalShadowAtlasAllocator::InsertFreeRect(uint32 OffsetX, uint32 OffsetY, uint32 Width, uint32 Height)
{
	if (Width == 0 || Height == 0)
	{
		return;
	}

	FAtlasFreeRect Rect = {};
	Rect.OffsetX = OffsetX;
	Rect.OffsetY = OffsetY;
	Rect.Width = Width;
	Rect.Height = Height;

	for (size_t i = 0; i < FreeRects.size();)
	{
		if (IsContainedIn(FreeRects[i], Rect))
		{
			RemoveAtSwap(FreeRects, i);
			continue;
		}

		if (IsContainedIn(Rect, FreeRects[i]))
		{
			return;
		}

		++i;
	}

	FreeRects.push_back(Rect);
}

void FShadowResourceManager::Initialize(ID3D11Device* Device, ID3D11DeviceContext* Context)
{
	CachedDevice = Device;
	CachedContext = Context;

	DirectionalResolutionPolicy.BaseResolution = 1024;
	DirectionalResolutionPolicy.MinResolution = 1024;
	DirectionalResolutionPolicy.MaxResolution = 2048;
	DirectionalResolutionPolicy.Alignment = 256;

	LocalResolutionPolicy.BaseResolution = 512;
	LocalResolutionPolicy.MinResolution = 64;
	LocalResolutionPolicy.MaxResolution = 1024;
	LocalResolutionPolicy.Alignment = 64;

	FShadowRuntimeOptions DefaultShadowOptions;
	CreateShadowMapAtlas(GLocalShadowAtlasResolution, GLocalShadowAtlasResolution, DefaultShadowOptions);
}

void FShadowResourceManager::Release()
{
	ReleaseShadowMapResource(Atlas.Map);
	ReleaseShadowMapResource(Atlas.FilterTempMap);
	Atlas.bUsePingPongFilterPath = false;
	ResetAtlasAllocationState();
	CurrentAtlasFilterMode = EShadowFilterMode::None;
	CurrentAtlasWidth = 0;
	CurrentAtlasHeight = 0;

	ReleaseDirectionalShadowArray();
	bDirectionalMomentResourcesEnabled = false;

	CachedDevice = nullptr;
	CachedContext = nullptr;
}

void FShadowResourceManager::UpdateShadowResources(FSceneEnvironment& Environment, const FShadowRuntimeOptions& ShadowOptions, const FFrameContext& Frame)
{
	LocalShadowRequestPlanner.BuildRequests(Environment, Frame, LocalResolutionPolicy, LocalShadowRequests);
	LocalShadowRequestPlanner.SortRequests(LocalShadowRequests);

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
	(void)BaseResolution;

	if (!Shadow.Settings.bCastShadows)
	{
		ReleaseDirectionalShadowArray();
		return;
	}

	const uint32 Resolution = ComputeRequestedResolution(Shadow.Settings, DirectionalResolutionPolicy);
	const int32 DesiredNumCascades = (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::Single) ? 0 : Shadow.NUM_CASCADES;
	const bool bUseMomentResources = (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM);
	ResizeDirectionalShadowArray(Resolution, DesiredNumCascades, bUseMomentResources);
}

void FShadowResourceManager::EnsureSpotShadow(FSpotShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions)
{
	(void)BaseResolution;
	(void)ShadowOptions;

	if (!Shadow.Settings.bCastShadows)
	{
		Shadow.View.bAtlasAllocated = false;
		Shadow.View.AtlasOffsetX = 0;
		Shadow.View.AtlasOffsetY = 0;
		Shadow.View.AtlasSizeX = 0;
		Shadow.View.AtlasSizeY = 0;
		Shadow.View.AtlasIndex = 0;
	}
}

void FShadowResourceManager::EnsurePointShadow(FPointShadowData& Shadow, uint32 BaseResolution, const FShadowRuntimeOptions& ShadowOptions)
{
	(void)BaseResolution;
	(void)ShadowOptions;

	if (!Shadow.Settings.bCastShadows)
	{
		for (int32 i = 0; i < 6; ++i)
		{
			Shadow.View[i].bAtlasAllocated = false;
			Shadow.View[i].AtlasOffsetX = 0;
			Shadow.View[i].AtlasOffsetY = 0;
			Shadow.View[i].AtlasSizeX = 0;
			Shadow.View[i].AtlasSizeY = 0;
			Shadow.View[i].AtlasIndex = 0;
		}
	}
}

void FShadowResourceManager::UpdateTelemetry(const FSceneEnvironment& Environment)
{
	Telemetry.NumDirectionalLights = Environment.HasGlobalDirectionalLight() ? 1u : 0u;
	Telemetry.NumPointLights = Environment.GetNumPointLights();
	Telemetry.NumSpotLights = Environment.GetNumSpotLights();

	Telemetry.RequestedLocalViewCount = 0;
	Telemetry.AllocatedLocalViewCount = 0;
	Telemetry.FailedShadowViewCount = 0;
	Telemetry.UsedLocalShadowAtlasAreaPerFrame = 0;
	for (const FLocalShadowRequest& Request : LocalShadowRequests)
	{
		if (!Request.bNeedsRender)
		{
			continue;
		}

		++Telemetry.RequestedLocalViewCount;
		if (Request.bAllocated)
		{
			++Telemetry.AllocatedLocalViewCount;
			Telemetry.UsedLocalShadowAtlasAreaPerFrame += static_cast<uint64>(Request.AtlasSizeX) * static_cast<uint64>(Request.AtlasSizeY);
		}
	}

	Telemetry.FailedShadowViewCount = Telemetry.RequestedLocalViewCount - Telemetry.AllocatedLocalViewCount;
	Telemetry.LocalAtlasTotalArea = static_cast<uint64>(Atlas.Map.Width) * static_cast<uint64>(Atlas.Map.Height);

	Telemetry.EstimatedLocalShadowVRAMBytes = 0;
	if (Atlas.Map.DepthTexture)
	{
		Telemetry.EstimatedLocalShadowVRAMBytes += static_cast<uint64>(Atlas.Map.Width) * static_cast<uint64>(Atlas.Map.Height) * 4ull;
	}
	if (Atlas.Map.Texture)
	{
		Telemetry.EstimatedLocalShadowVRAMBytes += static_cast<uint64>(Atlas.Map.Width) * static_cast<uint64>(Atlas.Map.Height) * 8ull;
	}
	if (Atlas.FilterTempMap.Texture)
	{
		Telemetry.EstimatedLocalShadowVRAMBytes += static_cast<uint64>(Atlas.FilterTempMap.Width) * static_cast<uint64>(Atlas.FilterTempMap.Height) * 8ull;
	}

	Telemetry.DirectionalShadowCascadeSliceCount = DirShadowArray.NumElements;
	Telemetry.DirectionalShadowArraySliceCount = DirShadowArray.Texture ? DirShadowArray.NumElements + 1u : 0u;

	Telemetry.EstimatedDirectionalShadowVRAMBytes = 0;
	if (DirShadowArray.Texture)
	{
		const uint64 DirectionalSliceCount = static_cast<uint64>(Telemetry.DirectionalShadowArraySliceCount);
		Telemetry.EstimatedDirectionalShadowVRAMBytes += static_cast<uint64>(DirShadowArray.Width) * static_cast<uint64>(DirShadowArray.Height) * DirectionalSliceCount * 4ull;
		if (DirShadowArray.MomentTexture)
		{
			Telemetry.EstimatedDirectionalShadowVRAMBytes += static_cast<uint64>(DirShadowArray.Width) * static_cast<uint64>(DirShadowArray.Height) * DirectionalSliceCount * 8ull;
		}
		if (DirShadowArray.MomentFilterTempTexture)
		{
			Telemetry.EstimatedDirectionalShadowVRAMBytes += static_cast<uint64>(DirShadowArray.Width) * static_cast<uint64>(DirShadowArray.Height) * DirectionalSliceCount * 8ull;
		}
	}

	Telemetry.EstimatedShadowVRAMBytes = Telemetry.EstimatedLocalShadowVRAMBytes + Telemetry.EstimatedDirectionalShadowVRAMBytes;
}

uint32 FShadowResourceManager::ComputeRequestedResolution(const FLightShadowSettings& Settings, const FShadowResolutionPolicy& Policy) const
{
	const float Scale = (Settings.ShadowResolutionScale <= 1e-4f) ? 1.0f : Settings.ShadowResolutionScale;
	uint32 Resolution = static_cast<uint32>(Scale * static_cast<float>(Policy.BaseResolution));
	Resolution = std::max(Resolution, Policy.MinResolution);
	Resolution = std::min(Resolution, Policy.MaxResolution);

	if (Policy.Alignment > 1)
	{
		const uint32 Align = Policy.Alignment;
		Resolution = ((Resolution + Align - 1) / Align) * Align;
		Resolution = std::min(Resolution, Policy.MaxResolution);
		Resolution = std::max(Resolution, Policy.MinResolution);
	}

	return Resolution;
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

void FShadowResourceManager::CreateDirectionalShadowArray(uint32 Resolution, int NumCascades, bool bUseMomentResources)
{
	if (!CachedDevice || Resolution == 0)
	{
		return;
	}

	ReleaseDirectionalShadowArray();
	DirShadowArray.Width = static_cast<float>(Resolution);
	DirShadowArray.Height = static_cast<float>(Resolution);
	DirShadowArray.NumElements = NumCascades;
	bDirectionalMomentResourcesEnabled = bUseMomentResources;

	D3D11_TEXTURE2D_DESC Desc = {};
	Desc.Width = Resolution;
	Desc.Height = Resolution;
	Desc.ArraySize = NumCascades + 1;
	Desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	Desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	Desc.MipLevels = 1;
	Desc.SampleDesc = { 1, 0 };
	CachedDevice->CreateTexture2D(&Desc, nullptr, &DirShadowArray.Texture);

	for (int i = 0; i <= NumCascades; ++i)
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

	if (bUseMomentResources)
	{
		D3D11_TEXTURE2D_DESC MomentDesc = {};
		MomentDesc.Width = Resolution;
		MomentDesc.Height = Resolution;
		MomentDesc.ArraySize = NumCascades + 1;
		MomentDesc.MipLevels = 1;
		MomentDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		MomentDesc.SampleDesc = { 1, 0 };
		MomentDesc.Usage = D3D11_USAGE_DEFAULT;
		MomentDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		CachedDevice->CreateTexture2D(&MomentDesc, nullptr, &DirShadowArray.MomentTexture);

		for (int i = 0; i <= NumCascades; ++i)
		{
			D3D11_RENDER_TARGET_VIEW_DESC RTVDesc = {};
			RTVDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
			RTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			RTVDesc.Texture2DArray.MipSlice = 0;
			RTVDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(i);
			RTVDesc.Texture2DArray.ArraySize = 1;
			CachedDevice->CreateRenderTargetView(DirShadowArray.MomentTexture, &RTVDesc, &DirShadowArray.MomentRTVs[i]);
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC MomentSRVDesc = {};
		MomentSRVDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		MomentSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		MomentSRVDesc.Texture2DArray.MostDetailedMip = 0;
		MomentSRVDesc.Texture2DArray.MipLevels = 1;
		MomentSRVDesc.Texture2DArray.FirstArraySlice = 0;
		MomentSRVDesc.Texture2DArray.ArraySize = static_cast<UINT>(NumCascades + 1);
		CachedDevice->CreateShaderResourceView(DirShadowArray.MomentTexture, &MomentSRVDesc, &DirShadowArray.MomentSRV);

		CachedDevice->CreateTexture2D(&MomentDesc, nullptr, &DirShadowArray.MomentFilterTempTexture);

		for (int i = 0; i <= NumCascades; ++i)
		{
			D3D11_RENDER_TARGET_VIEW_DESC TempRTVDesc = {};
			TempRTVDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
			TempRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			TempRTVDesc.Texture2DArray.MipSlice = 0;
			TempRTVDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(i);
			TempRTVDesc.Texture2DArray.ArraySize = 1;
			CachedDevice->CreateRenderTargetView(DirShadowArray.MomentFilterTempTexture, &TempRTVDesc, &DirShadowArray.MomentFilterTempRTVs[i]);
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC TempMomentSRVDesc = {};
		TempMomentSRVDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		TempMomentSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		TempMomentSRVDesc.Texture2DArray.MostDetailedMip = 0;
		TempMomentSRVDesc.Texture2DArray.MipLevels = 1;
		TempMomentSRVDesc.Texture2DArray.FirstArraySlice = 0;
		TempMomentSRVDesc.Texture2DArray.ArraySize = static_cast<UINT>(NumCascades + 1);
		CachedDevice->CreateShaderResourceView(DirShadowArray.MomentFilterTempTexture, &TempMomentSRVDesc, &DirShadowArray.MomentFilterTempSRV);
	}
}

void FShadowResourceManager::ResizeDirectionalShadowArray(uint32 Resolution, int NumCascades, bool bUseMomentResources)
{
	if (DirShadowArray.Texture
		&& static_cast<uint32>(DirShadowArray.Width) == Resolution
		&& DirShadowArray.NumElements == static_cast<uint32>(NumCascades)
		&& bDirectionalMomentResourcesEnabled == bUseMomentResources)
	{
		return;
	}

	CreateDirectionalShadowArray(Resolution, NumCascades, bUseMomentResources);
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

	if (DirShadowArray.MomentTexture)
	{
		DirShadowArray.MomentTexture->Release();
		DirShadowArray.MomentTexture = nullptr;
	}

	if (DirShadowArray.MomentSRV)
	{
		DirShadowArray.MomentSRV->Release();
		DirShadowArray.MomentSRV = nullptr;
	}

	if (DirShadowArray.MomentFilterTempTexture)
	{
		DirShadowArray.MomentFilterTempTexture->Release();
		DirShadowArray.MomentFilterTempTexture = nullptr;
	}

	if (DirShadowArray.MomentFilterTempSRV)
	{
		DirShadowArray.MomentFilterTempSRV->Release();
		DirShadowArray.MomentFilterTempSRV = nullptr;
	}

	for (int i = 0; i < 5; ++i)
	{
		if (DirShadowArray.DSVs[i])
		{
			DirShadowArray.DSVs[i]->Release();
			DirShadowArray.DSVs[i] = nullptr;
		}

		if (DirShadowArray.MomentRTVs[i])
		{
			DirShadowArray.MomentRTVs[i]->Release();
			DirShadowArray.MomentRTVs[i] = nullptr;
		}

		if (DirShadowArray.MomentFilterTempRTVs[i])
		{
			DirShadowArray.MomentFilterTempRTVs[i]->Release();
			DirShadowArray.MomentFilterTempRTVs[i] = nullptr;
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
	bDirectionalMomentResourcesEnabled = false;
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
	ReleaseShadowMapResource(Atlas.FilterTempMap);
	Atlas.bUsePingPongFilterPath = false;
	ResetAtlasAllocationState();

	if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM)
	{
		// Moment path:
		// Atlas.Map holds both moment color resource (Texture/RTV/SRV) and depth resource (DepthTexture/DSV).
		// FilterTempMap is a ping-pong moment target used only by VSM/ESM filtering passes.
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

		if (!CreateVSMESMShadowMapResource(Atlas.FilterTempMap, Width, Height))
		{
			ReleaseShadowMapResource(Atlas.Map);
			ReleaseShadowMapResource(Atlas.FilterTempMap);
			return false;
		}
		Atlas.bUsePingPongFilterPath = true;
	}
	else
	{
		// PCF path:
		// Atlas.Map uses depth-only resource (DepthTexture/DSV/SRV), no filter temp required.
		if (!CreateDepthShadowMapResource(Atlas.Map, Width, Height, true))
		{
			ReleaseShadowMapResource(Atlas.Map);
			return false;
		}

		ReleaseShadowMapResource(Atlas.FilterTempMap);
		Atlas.bUsePingPongFilterPath = false;
	}

	ResetAtlasAllocationState();
	CurrentAtlasFilterMode = ShadowOptions.ShadowFilterMode;
	CurrentAtlasWidth = Width;
	CurrentAtlasHeight = Height;
	return true;
}

FAtlasResourceInfo FShadowResourceManager::AllocateFromAtlas(uint32 RequestWidth, uint32 RequestHeight)
{
	FAtlasResourceInfo Info;

	if (Atlas.Map.Width == 0 || Atlas.Map.Height == 0)
	{
		return Info;
	}

	if (RequestWidth == 0 || RequestHeight == 0)
	{
		return Info;
	}

	if (RequestWidth > Atlas.Map.Width || RequestHeight > Atlas.Map.Height)
	{
		return Info;
	}

	return LocalAtlasAllocator.Allocate(RequestWidth, RequestHeight);
}

void FShadowResourceManager::AllocateLocalShadowViews(FSceneEnvironment& Environment, const FScene& Scene)
{
	LocalShadowAllocationExecutor.AllocateViews(
		Environment,
		Scene,
		LocalResolutionPolicy,
		MaxLocalShadowViewsPerFrame,
		MaxLocalShadowAtlasAreaPerFrame,
		Atlas,
		LocalAtlasAllocator,
		LocalShadowRequests);

	UpdateTelemetry(Environment);
}

void FShadowResourceManager::SetLocalShadowAlignment(uint32 InAlignment)
{
	if (InAlignment == 0)
	{
		InAlignment = 1;
	}

	InAlignment = std::min(InAlignment, 4096u);
	LocalResolutionPolicy.Alignment = InAlignment;
}

bool FShadowResourceManager::ClearAtlas(const FShadowRuntimeOptions& ShadowOptions)
{
	if (!ClearAtlasTextures(ShadowOptions))
	{
		return false;
	}

	ResetAtlasAllocationState();
	return true;
}

bool FShadowResourceManager::ClearAtlasTexturesOnly(const FShadowRuntimeOptions& ShadowOptions)
{
	// Public API kept for call-site readability (texture clear only).
	return ClearAtlasTextures(ShadowOptions);
}

void FShadowResourceManager::ResetAtlasAllocationStateForFrame()
{
	// Public API kept for explicit frame-boundary allocation reset.
	ResetAtlasAllocationState();
}

bool FShadowResourceManager::ClearAtlasTextures(const FShadowRuntimeOptions& ShadowOptions)
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
		if (Atlas.bUsePingPongFilterPath && Atlas.FilterTempMap.RTV)
		{
			CachedContext->ClearRenderTargetView(Atlas.FilterTempMap.RTV, ClearColor);
		}
	}

	return true;
}

void FShadowResourceManager::ResetAtlasAllocationState()
{
	LocalAtlasAllocator.Reset(Atlas.Map.Width, Atlas.Map.Height);
}
