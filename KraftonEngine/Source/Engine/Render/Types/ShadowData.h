#pragma once

#include "Core/CoreTypes.h"
#include "Math/Matrix.h"
#include "Render/Resource/ShadowAtlasResource.h"
#include "Render/Resource/LocalShadowInfo.h"
#include "Render/Pipeline/ForwardLightData.h"

struct FLightShadowSettings
{
	bool bCastShadows = false;
	float ShadowResolutionScale = 1.f;
	float ShadowBias = 0.0f;
	float ShadowSlopeBias = 0.0f;
	float ShadowSharpen = 1.0f;
};

struct FShadowViewData
{
	// Local/Directional shadow view payload.
	// - PCF path: DepthTexture/DSV/SRV fields are used.
	// - VSM/ESM path: color Moment texture(RTV/SRV) and DepthTexture/DSV can coexist.
	// This container is intentionally reused for both paths.
	FShadowMapResource DepthMap;

	FMatrix LightView = FMatrix::Identity;
	FMatrix LightProj = FMatrix::Identity;
	FMatrix LightViewProj = FMatrix::Identity;

	uint32 AtlasOffsetX = 0;
	uint32 AtlasOffsetY = 0;
	uint32 AtlasSizeX = 0;
	uint32 AtlasSizeY = 0;
	uint32 AtlasIndex = 0;
	bool bAtlasAllocated = false;
};

struct FDirectionalShadowArray
{
	ID3D11Texture2D* Texture = nullptr;

	ID3D11DepthStencilView* DSVs[5] = {};
	ID3D11ShaderResourceView* SRV = nullptr;

	ID3D11ShaderResourceView* PreviewSRVs[5] = {};

	ID3D11Texture2D* MomentTexture = nullptr;
	ID3D11RenderTargetView* MomentRTVs[5] = {};
	ID3D11ShaderResourceView* MomentSRV = nullptr;
	ID3D11Texture2D* MomentFilterTempTexture = nullptr;
	ID3D11RenderTargetView* MomentFilterTempRTVs[5] = {};
	ID3D11ShaderResourceView* MomentFilterTempSRV = nullptr;

	float Width = 0.0f;
	float Height = 0.0f;

	uint32 NumElements = 0;
};

struct FShadowCommonData
{
public:

	FLightShadowSettings Settings;
	bool bOverrideCameraWithLight = false;

public:
	FVector4 MakeAtlasRect(const FShadowViewData& View, const FShadowAtlasResource& Atlas) const
	{
		if (!View.bAtlasAllocated || Atlas.Map.Width == 0 || Atlas.Map.Height == 0)
		{
			return FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		}

		return FVector4(
			static_cast<float>(View.AtlasOffsetX) / static_cast<float>(Atlas.Map.Width),
			static_cast<float>(View.AtlasOffsetY) / static_cast<float>(Atlas.Map.Height),
			static_cast<float>(View.AtlasSizeX) / static_cast<float>(Atlas.Map.Width),
			static_cast<float>(View.AtlasSizeY) / static_cast<float>(Atlas.Map.Height));
	}
	virtual FLocalShadowInfo ConvertToLocalShadowInfo(const FShadowAtlasResource& Atlas) const { return FLocalShadowInfo(); }
};

struct FDirectionalShadowData : FShadowCommonData
{
	static constexpr  int NUM_CASCADES = 4;

	float DistributeExponent = 0.95f;
	FShadowViewData PSMView;
	FMatrix MainViewProjection = FMatrix::Identity;
	FShadowViewData View[NUM_CASCADES];
	float CasCadeEnds[NUM_CASCADES + 1];
	float CascadeEndClipZ[NUM_CASCADES];
};

struct FPointShadowData : FShadowCommonData
{
public:

	FShadowViewData View[6];
	int32 PreviewViewIndex = 0;

public:

	virtual FLocalShadowInfo ConvertToLocalShadowInfo(const FShadowAtlasResource& Atlas) const override
	{
		FLocalShadowInfo Info = {};
		Info.CastShadow = Settings.bCastShadows ? 1u : 0u;
		Info.ShadowType = ELightType::Point;
		Info.Bias = Settings.ShadowBias;
		Info.SlopeBias = Settings.ShadowSlopeBias;
		Info.Sharpen = Settings.ShadowSharpen;

		for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
		{
			const FShadowViewData& ViewData = View[FaceIndex];
			Info.LightViewProj[FaceIndex] = ViewData.LightViewProj.ConvertToPOD();
			Info.AtlasRect[FaceIndex] = MakeAtlasRect(ViewData, Atlas);
		}

		return Info;
	}

};

struct FSpotShadowData : FShadowCommonData
{
public:
	FShadowViewData View;


public:
	virtual FLocalShadowInfo ConvertToLocalShadowInfo(const FShadowAtlasResource& Atlas) const override
	{
		FLocalShadowInfo Info = {};
		Info.CastShadow = (Settings.bCastShadows && View.bAtlasAllocated) ? 1u : 0u;
		Info.ShadowType = ELightType::Spot;
		Info.Bias = Settings.ShadowBias;
		Info.SlopeBias = Settings.ShadowSlopeBias;
		Info.Sharpen = Settings.ShadowSharpen;
		Info.LightViewProj[0] = View.LightViewProj.ConvertToPOD();
		Info.AtlasRect[0] = MakeAtlasRect(View, Atlas);
		return Info;
	}
};

