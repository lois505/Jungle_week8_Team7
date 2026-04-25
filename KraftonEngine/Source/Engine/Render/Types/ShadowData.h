#pragma once

#include "Core/CoreTypes.h"
#include "Math/Matrix.h"

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
	FLightShadowSettings Settings;
	bool bOverrideCameraWithLight = false;
};

struct FDirectionalShadowData : FShadowCommonData
{
	FShadowViewData View;

	//	TODO : CSM 관련은 이 곳에 작성하는게 맞지 않을까요?
};

struct FPointShadowData : FShadowCommonData
{
	FShadowViewData View[6];
	int32 PreviewViewIndex = 0;
};

struct FSpotShadowData : FShadowCommonData
{
	FShadowViewData View;
};

