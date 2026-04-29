#pragma once
#include "Core/CoreTypes.h"
class ID3D11Texture2D;
class ID3D11DepthStencilView;
class ID3D11RenderTargetView;
class ID3D11ShaderResourceView;

struct FShadowMapResource
{
	//	R32G32 Format의 VSM, ESM 전용 Texture
	ID3D11Texture2D* Texture = nullptr;
	ID3D11RenderTargetView* RTV = nullptr;
	ID3D11ShaderResourceView* SRV = nullptr;

	//	R24G8 Format의 DSV 전용 Depth Texture
	ID3D11Texture2D* DepthTexture = nullptr;
	ID3D11DepthStencilView* DSV = nullptr;

	uint32 Width = 0;
	uint32 Height = 0;
};

struct FShadowAtlasResource
{
	FShadowMapResource Map;
	// Phase 2 placeholder: Moment Ping-Pong temp atlas (B). Not allocated in Phase 1.
	FShadowMapResource FilterTempMap;
	bool bUsePingPongFilterPath = false;
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
