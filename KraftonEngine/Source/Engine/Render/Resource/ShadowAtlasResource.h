#pragma once
#include "Core/CoreTypes.h"
class ID3D11Texture2D;
class ID3D11DepthStencilView;
class ID3D11ShaderResourceView;

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