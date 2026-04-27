#pragma once

#include "Core/CoreTypes.h"
#include "Math/Matrix.h"
#include "Render/Resource/ShadowAtlasResource.h"
#include "Render/Resource/LocalShadowInfo.h"
#include "Render/Pipeline/ForwardLightData.h"

struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

struct FLightShadowSettings
{
	bool bCastShadows = false;
	float ShadowResolutionScale = 1.0f;
	float ShadowBias = 0.0f;
	float ShadowSlopeBias = 0.0f;
	float ShadowSharpen = 1.0f;
};

struct FShadowMapResource
{
	ID3D11Texture2D* Texture = nullptr;
	//	RTV, DSV 중 뭘 쓸지 고민해보아야 함 -> 아마 RTV 쓸 듯?
	ID3D11RenderTargetView* RTV = nullptr;
	ID3D11DepthStencilView* DSV = nullptr;
	ID3D11ShaderResourceView* SRV = nullptr;

	uint32 Width = 0;
	uint32 Height = 0;
};

struct FShadowViewData
{
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

struct FShadowCommonData
{
public:

	FLightShadowSettings Settings;
	bool bOverrideCameraWithLight = false;

public:
	FVector4 MakeAtlasRect(const FShadowViewData& View, const FShadowAtlasResource& Atlas) const
	{
		if (!View.bAtlasAllocated || Atlas.Width == 0 || Atlas.Height == 0)
		{
			return FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		}

		return FVector4(
			static_cast<float>(View.AtlasOffsetX) / static_cast<float>(Atlas.Width),
			static_cast<float>(View.AtlasOffsetY) / static_cast<float>(Atlas.Height),
			static_cast<float>(View.AtlasSizeX) / static_cast<float>(Atlas.Width),
			static_cast<float>(View.AtlasSizeY) / static_cast<float>(Atlas.Height));
	}
	virtual FLocalShadowInfo ConvertToLocalShadowInfo(const FShadowAtlasResource& Atlas) const { return FLocalShadowInfo(); }
};

struct FDirectionalShadowData : FShadowCommonData
{
	FShadowViewData View;

	//	TODO : CSM 관련은 이 곳에 작성하는게 맞지 않을까요?
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
			Info.CastShadow &= ViewData.bAtlasAllocated ? 1u : 0u;
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

