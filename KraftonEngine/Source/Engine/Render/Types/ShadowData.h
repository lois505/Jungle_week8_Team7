#pragma once

struct FLightShadowSettings
{
	bool bCastShadows = false;
	float ShadowResolutionScale;
	float ShadowBias;
	float ShadowSlopeBias;
	float ShadowSharpen;
};

struct FShadowMapResource
{
	ID3D11Texture2D * Texture = nullptr;
	//	RTV, DSV 중 뭘 쓸지 고민해보아야 함 -> 아마 RTV 쓸 듯?
	ID3D11RenderTargetView * RTV = nullptr;
	ID3D11DepthStencilView * DSV = nullptr;
	ID3D11ShaderResourceView * SRV = nullptr;
	
	uint32 Width;
	uint32 Height;
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

	FMatrix PSMMatrix = FMatrix::Identity;
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

