#include "ShadowRenderer.h"

#include "FrameContext.h"
#include "ShadowPassContext.h"
#include "Render/Proxy/FScene.h"
#include "Render/Resource/RenderResources.h"

namespace
{
	//	Shadow Map을 그린 후 Main Viewport의 상태를 복구함 (다음 Pass를 위해)
	void RestoreMainViewport(FD3DDevice& Device, const FFrameContext& MainFrame)
	{
		ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();

		ID3D11RenderTargetView* RTV = MainFrame.ViewportRTV;
		ID3D11DepthStencilView* DSV = MainFrame.ViewportDSV;

		D3D11_VIEWPORT Viewport = {};
		Viewport.TopLeftX = 0.0f;
		Viewport.TopLeftY = 0.0f;
		Viewport.Width = MainFrame.ViewportWidth;
		Viewport.Height = MainFrame.ViewportHeight;
		Viewport.MinDepth = 0.0f;
		Viewport.MaxDepth = 1.0f;

		DeviceContext->OMSetRenderTargets(1, &RTV, DSV);
		DeviceContext->RSSetViewports(1, &Viewport);
	}

	//	View의 준비 상태를 체크
	bool IsShadowViewReady(const FShadowViewData& View, const FShadowRuntimeOptions& ShadowOptions)
	{
		if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM)
		{
			return View.DepthMap.Texture && View.DepthMap.RTV && View.DepthMap.SRV
				&& View.DepthMap.Width > 0 && View.DepthMap.Height > 0;
		}

		return View.DepthMap.Texture && View.DepthMap.DSV && View.DepthMap.SRV
			&& View.DepthMap.Width > 0 && View.DepthMap.Height > 0;
	}
}

void FShadowRenderer::Create(ID3D11Device* Device, ID3D11DeviceContext* DeviceContext)
{
	Builder.Create(Device, DeviceContext);
}

void FShadowRenderer::Release()
{
	Builder.Release();
}

void FShadowRenderer::RenderShadows(FD3DDevice& Device, FSystemResources& Resources, FScene& Scene,
                                    const FFrameContext& MainFrame)
{
	if (MainFrame.RenderOptions.ViewMode == EViewMode::Unlit)
	{
		return;
	}

	FSceneEnvironment& Env = Scene.GetEnvironment();

	if (Scene.GetEnvironment().HasGlobalDirectionalLight())
	{
		RenderDirectionalShadow(Device, Resources, Env.GetGlobalDirectionalLightParams(), Scene);
	}

	for (uint32 i = 0; i < Env.GetNumPointLights(); i++)
	{
		RenderPointShadow(Device, Resources, Env.GetPointLight(i), Scene);
	}

	for (uint32 i = 0; i < Env.GetNumSpotLights(); i++)
	{
		RenderSpotShadow(Device, Resources, Env.GetSpotLight(i), Scene);
	}

	//	RS의 RT 원상태 복구
	RestoreMainViewport(Device, MainFrame);
}

void FShadowRenderer::RenderDirectionalShadow(FD3DDevice& Device, FSystemResources& Resources, FGlobalDirectionalLightParams& Light, FScene& Scene)
{
	if (!Light.ShadowData.Settings.bCastShadows)
	{
		return;
	}

	if (!IsShadowViewReady(Light.ShadowData.View, ShadowOptions))
	{
		return;
	}

	RenderShadowView(Device, Resources, Light.ShadowData.View, Scene);
}


void FShadowRenderer::RenderPointShadow(FD3DDevice& Device, FSystemResources & Resources, FPointLightParams& Light, FScene& Scene)
{
	if (!Light.ShadowData.Settings.bCastShadows)
	{
		return;
	}

	for (int32 i = 0; i < 6; i++)
	{
		if (!IsShadowViewReady(Light.ShadowData.View[i], ShadowOptions))
		{
			continue;
		}

		RenderShadowView(Device, Resources, Light.ShadowData.View[i], Scene);
	}
}

void FShadowRenderer::RenderSpotShadow(FD3DDevice& Device, FSystemResources & Resources, FSpotLightParams& Light, FScene& Scene)
{
	if (!Light.ShadowData.Settings.bCastShadows)
	{
		return;
	}

	if (!IsShadowViewReady(Light.ShadowData.View, ShadowOptions))
	{
		return;
	}

	RenderShadowView(Device, Resources, Light.ShadowData.View, Scene);
}

//	각각의 View Rendering
void FShadowRenderer::RenderShadowView(FD3DDevice& Device, FSystemResources& Resources, FShadowViewData& View, FScene& Scene)
{
	//	Preparing for Rendering
    ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();

    FShadowPassContext PassContext = {};
    PassContext.View = View.LightView;
    PassContext.Proj = View.LightProj;
    PassContext.ViewProj = View.LightViewProj;
    PassContext.RTV = View.DepthMap.RTV;

    PassContext.Viewport.TopLeftX = 0.0f;
    PassContext.Viewport.TopLeftY = 0.0f;
    PassContext.Viewport.Width = static_cast<float>(View.DepthMap.Width);
    PassContext.Viewport.Height = static_cast<float>(View.DepthMap.Height);
    PassContext.Viewport.MinDepth = 0.0f;
    PassContext.Viewport.MaxDepth = 1.0f;

    DeviceContext->RSSetViewports(1, &PassContext.Viewport);

    if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM)
    {
        ID3D11RenderTargetView* RTV = View.DepthMap.RTV;
        DeviceContext->OMSetRenderTargets(1, &RTV, nullptr);

        const float ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        DeviceContext->ClearRenderTargetView(RTV, ClearColor);

        Resources.SetDepthStencilState(Device, EDepthStencilState::NoDepth);
    }
    else
    {
        DeviceContext->OMSetRenderTargets(0, nullptr, View.DepthMap.DSV);
        DeviceContext->ClearDepthStencilView(View.DepthMap.DSV, D3D11_CLEAR_DEPTH, 0.0f, 0);

        Resources.SetDepthStencilState(Device, EDepthStencilState::DepthGreaterEqual);
    }

    Resources.SetBlendState(Device, EBlendState::Opaque);
    Resources.SetRasterizerState(Device, ERasterizerState::SolidBackCull);

    Builder.BeginBuild(Scene.GetProxyCount());
    Builder.BuildCommands(Scene);

    BindShadowFrameConstants(Device, Resources, PassContext);

    // TODO: 팀원 구현
    //	Depth shadow shader VSM shader bind
	
    for (const FShadowDrawCommand& Cmd : Builder.GetCommands())
    {
        SubmitShadowCommand(Device, Cmd);
    }
}


//	RenderResources.cpp의 UpdateFrameBuffer() 참고
void FShadowRenderer::BindShadowFrameConstants(FD3DDevice& Device, FSystemResources& Resources,
	const FShadowPassContext& Context)
{
	ID3D11DeviceContext * DeviceContext = Device.GetDeviceContext();
	
	FFrameConstants FrameData = {};
	FrameData.View = Context.View;
	FrameData.Projection = Context.Proj;
	FrameData.InvProj = Context.Proj.GetInverse();
	FrameData.InvViewProj = Context.ViewProj.GetInverse();
	FrameData.bIsWireframe = 0.0f;
	FrameData.WireframeColor = FVector(1.0f,1.0f,1.0f);
	FrameData.Time = 0.0f;
	FrameData.CameraWorldPos = FVector(0.0f, 0.0f, 0.0f);
	
	Resources.FrameBuffer.Update(DeviceContext, &FrameData, sizeof(FFrameConstants));
	
	ID3D11Buffer * b0 = Resources.FrameBuffer.GetBuffer();
	DeviceContext->VSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
	DeviceContext->PSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
	DeviceContext->CSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
}

//	Draw Binding 로직 분리용
void FShadowRenderer::SubmitShadowCommand(FD3DDevice& Device, const FShadowDrawCommand& Cmd)
{
	ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();

	DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (Cmd.Buffer.VB)
	{
		uint32 Offset = 0;
		DeviceContext->IASetVertexBuffers(0, 1, &Cmd.Buffer.VB, &Cmd.Buffer.VBStride, &Offset);
	}
	if (Cmd.Buffer.IB)
	{
		DeviceContext->IASetIndexBuffer(Cmd.Buffer.IB, DXGI_FORMAT_R32_UINT, 0);
	}
	if (Cmd.PerObjectCB)
	{
		ID3D11Buffer* CB = Cmd.PerObjectCB->GetBuffer();
		if (CB)
		{
			DeviceContext->VSSetConstantBuffers(ECBSlot::PerObject, 1, &CB);
		}
	}

	if (Cmd.Buffer.IndexCount > 0)
	{
		DeviceContext->DrawIndexed(Cmd.Buffer.IndexCount, Cmd.Buffer.FirstIndex, Cmd.Buffer.BaseVertex);
	}
	else if (Cmd.Buffer.VertexCount > 0)
	{
		DeviceContext->Draw(Cmd.Buffer.VertexCount, 0);
	}
}
