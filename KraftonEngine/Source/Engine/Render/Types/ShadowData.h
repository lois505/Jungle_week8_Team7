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
};

struct FShadowCommonData
{
	FLightShadowSettings Settings;
	bool bOverrideCameraWithLight = false;
};

struct FDirectionalShadowData : FShadowCommonData
{
	FShadowViewData View;

	// TODO: Add CSM data here when cascaded shadows are implemented.
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
