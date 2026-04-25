#pragma once

#include <d3d11.h>
#include "Math/Matrix.h"

struct FShadowPassContext
{
	FMatrix View;
	FMatrix Proj;
	FMatrix ViewProj;
	
	ID3D11RenderTargetView* RTV;
	D3D11_VIEWPORT Viewport;
};
