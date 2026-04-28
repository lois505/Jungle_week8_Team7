#include "ShadowRenderer.h"

#include "FrameContext.h"
#include "ShadowPassContext.h"
#include "Render/Proxy/FScene.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Resource/ShaderManager.h"

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

	bool IsShadowViewReady(const FShadowViewData& View, const FShadowRuntimeOptions& ShadowOptions)
	{
		if (View.bAtlasAllocated)
		{
			return View.AtlasSizeX > 0 && View.AtlasSizeY > 0;
		}

		if ((ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM))
		{
			return View.DepthMap.Texture && View.DepthMap.RTV && View.DepthMap.SRV
				&& View.DepthMap.Width > 0 && View.DepthMap.Height > 0;
		}

		return (View.DepthMap.DepthTexture || View.DepthMap.Texture) && View.DepthMap.DSV && View.DepthMap.SRV
			&& View.DepthMap.Width > 0 && View.DepthMap.Height > 0;
	}

	bool IsShadowAtlasReady(const FShadowAtlasResource& Atlas, const FShadowRuntimeOptions& ShadowOptions)
	{
		if ((ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM))
		{
			return Atlas.Map.Texture && Atlas.Map.RTV && Atlas.Map.SRV
				&& Atlas.Map.DepthTexture && Atlas.Map.DSV
				&& Atlas.Map.Width > 0 && Atlas.Map.Height > 0;
		}

		return Atlas.Map.DepthTexture && Atlas.Map.DSV && Atlas.Map.SRV && Atlas.Map.Width > 0 && Atlas.Map.Height > 0;
	}

	void AssignAtlasRect(FShadowViewData& View, const FAtlasResourceInfo& Info)
	{
		View.AtlasOffsetX = Info.OffsetX;
		View.AtlasOffsetY = Info.OffsetY;
		View.AtlasSizeX = Info.Width;
		View.AtlasSizeY = Info.Height;
		View.AtlasIndex = Info.Index;
		View.bAtlasAllocated = Info.bAllocated;
	}

	void UpdateCacades(FDirectionalShadowData& ShadowData, const FVector& LightDirection, const FFrameContext& MainFrame)
	{
		FVector F = LightDirection.Normalized();

		FVector worldUp = FVector(0.0f, 0.0f, 1.0f);
		if (std::abs(F.Z) > 0.99f)
			worldUp = FVector(1.0f, 0.0f, 0.0f);

		FVector R = worldUp.Cross(F).Normalized();
		FVector U = F.Cross(R).Normalized();

		FMatrix LightView = FMatrix::MakeViewMatrix(F, R, U, FVector(0.0f, 0.0f, 0.0f));

		FMatrix CamViewInv = MainFrame.View.GetInverse();

		float TanHalfHFov = 1.0f / MainFrame.Proj.M[0][0];
		float TanHalfVFov = 1.0f / MainFrame.Proj.M[1][1];

		ShadowData.CasCadeEnds[0] = MainFrame.NearClip;
		ShadowData.CasCadeEnds[ShadowData.NUM_CASCADES] = MainFrame.FarClip;

		for (int i = 1; i < ShadowData.NUM_CASCADES; i++)
		{
			float t = static_cast<float>(i) / static_cast<float>(ShadowData.NUM_CASCADES);
			float uniform = MainFrame.NearClip + (MainFrame.FarClip - MainFrame.NearClip) * t;
			float log = MainFrame.NearClip * std::pow(MainFrame.FarClip / MainFrame.NearClip, t);
			ShadowData.CasCadeEnds[i] = ShadowData.DistributeExponent * log
				+ (1.0f - ShadowData.DistributeExponent) * uniform;
		}

		for (int i = 0; i < ShadowData.NUM_CASCADES; i++)
		{
			float Zn = ShadowData.CasCadeEnds[i];
			float Zf = ShadowData.CasCadeEnds[i + 1];

			FVector4 Corners[8] = {
			   { Zn * TanHalfHFov,  Zn * TanHalfVFov, Zn, 1.f},
			   {-Zn * TanHalfHFov,  Zn * TanHalfVFov, Zn, 1.f},
			   { Zn * TanHalfHFov, -Zn * TanHalfVFov, Zn, 1.f},
			   {-Zn * TanHalfHFov, -Zn * TanHalfVFov, Zn, 1.f},
			   { Zf * TanHalfHFov,  Zf * TanHalfVFov, Zf, 1.f},
			   {-Zf * TanHalfHFov,  Zf * TanHalfVFov, Zf, 1.f},
			   { Zf * TanHalfHFov, -Zf * TanHalfVFov, Zf, 1.f},
			   {-Zf * TanHalfHFov, -Zf * TanHalfVFov, Zf, 1.f},
			};

			float MinX = FLT_MAX, MaxX = -FLT_MAX;
			float MinY = FLT_MAX, MaxY = -FLT_MAX;
			float MinZ = FLT_MAX, MaxZ = -FLT_MAX;

			for (auto& C : Corners)
			{
				FVector4 World = CamViewInv.TransformVector4(C);
				FVector4 Light = LightView.TransformVector4(World);

				MinX = std::min(MinX, Light.X); MaxX = std::max(MaxX, Light.X);
				MinY = std::min(MinY, Light.Y); MaxY = std::max(MaxY, Light.Y);
				MinZ = std::min(MinZ, Light.Z); MaxZ = std::max(MaxZ, Light.Z);
			}

			float Resolution = ShadowData.Settings.ShadowResolutionScale * 1024.0f;
			float WorldUnitsPerTexel = (MaxX - MinX) / Resolution;

			MinX = std::floor(MinX / WorldUnitsPerTexel) * WorldUnitsPerTexel;
			MaxX = MinX + (MaxX - MinX);

			MinY = std::floor(MinY / WorldUnitsPerTexel) * WorldUnitsPerTexel;
			MaxY = MinY + (MaxY - MinY);

			FShadowViewData& View = ShadowData.View[i];
			View.LightView = LightView;
			View.LightProj = FMatrix::MakeOrtho(MinX, MaxX, MinY, MaxY, MinZ, MaxZ);
			View.LightViewProj = View.LightView * View.LightProj;

			FVector4 VClip = MainFrame.Proj.TransformVector4(FVector4(0.0f, 0.0f, Zf, 1.0f));
			ShadowData.CascadeEndClipZ[i] = VClip.Z / VClip.W;
		}
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
		if (Env.GetGlobalDirectionalLightParams().ShadowData.Settings.bCastShadows)
		{
			RenderDirectionalShadow(Device, Resources, Env.GetGlobalDirectionalLightParams(), Scene, MainFrame);
		}
	}

	for (uint32 i = 0; i < Env.GetNumPointLights(); i++)
	{
		FPointLightParams& Params = Env.GetPointLight(i);
		if (Params.ShadowData.Settings.bCastShadows)
		{
			for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
			{
				//Get Available Atals Infomation(Offset, Size) From ShadowResourceManager. After that, Assign to PointLightParam
				AssignAtlasRect(Params.ShadowData.View[FaceIndex], Resources.ShadowResourceManager.AllocateFromAtlas());
			}
		}
		//Draw To Shadow Atlas | (Inside) RenderShadowView Decide if Light will drawn to Atals or just single DSV
		RenderPointShadow(Device, Resources, Env.GetPointLight(i), Scene);
	}

	for (uint32 i = 0; i < Env.GetNumSpotLights(); i++)
	{
		FSpotLightParams& Params = Env.GetSpotLight(i);
		if (Params.ShadowData.Settings.bCastShadows)
		{
			//Get Available Atals Infomation(Offset, Size) From ShadowResourceManager and Assign to SpotLightParams
			AssignAtlasRect(Params.ShadowData.View, Resources.ShadowResourceManager.AllocateFromAtlas());
		}
		//Draw To Shadow Atlas | (Inside)  RenderShadowView Decide if Light will drawn to Atals or just single DSV
		RenderSpotShadow(Device, Resources, Env.GetSpotLight(i), Scene);
	}

	//	RS의 RT 원상태 복구
	RestoreMainViewport(Device, MainFrame);
}

void FShadowRenderer::RenderDirectionalShadow(FD3DDevice& Device, FSystemResources& Resources, FGlobalDirectionalLightParams& Light, FScene& Scene, const FFrameContext MainFrame)
{
	const TStaticArray<FVector, 8> WorldFrustumCorners = MainFrame.FrustumVolume.GetFrustumCorners();
	const FMatrix MainViewProjection = MainFrame.View * MainFrame.Proj;
	TStaticArray<FVector4, 8> PSMFrustumCorners = {};

	for (int32 i = 0; i < static_cast<int32>(WorldFrustumCorners.size()); ++i)
	{
		FVector4 ProjectedCorner = MainViewProjection.TransformVector4(FVector4(WorldFrustumCorners[i], 1.0f));
		if (std::abs(ProjectedCorner.W) > 1e-6f)
		{
			ProjectedCorner.X /= ProjectedCorner.W;
			ProjectedCorner.Y /= ProjectedCorner.W;
			ProjectedCorner.Z /= ProjectedCorner.W;
			ProjectedCorner.W = 1.0f;
		}

		PSMFrustumCorners[i] = ProjectedCorner;
	}

	UpdateCacades(Light.ShadowData, Light.Direction, MainFrame);
	const FDirectionalShadowArray& DirectionalArray = Resources.ShadowResourceManager.GetShadowArray();

	for (int i = 0; i < Light.ShadowData.NUM_CASCADES; i++)
	{
		FShadowMapResource& DepthMap = Light.ShadowData.View[i].DepthMap;
		DepthMap.DSV = DirectionalArray.DSVs[i + 1]; // slices 1~4, slot 0 reserved for PSM
		DepthMap.DepthTexture = DirectionalArray.Texture;

		if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM)
		{
			DepthMap.Texture = DirectionalArray.MomentTexture;
			DepthMap.RTV = DirectionalArray.MomentRTVs[i + 1];
			DepthMap.SRV = DirectionalArray.MomentSRV;
		}
		else
		{
			DepthMap.Texture = DirectionalArray.Texture;
			DepthMap.RTV = nullptr;
			DepthMap.SRV = DirectionalArray.SRV;
		}

		DepthMap.Width = static_cast<uint32>(DirectionalArray.Width);
		DepthMap.Height = static_cast<uint32>(DirectionalArray.Height);

		RenderShadowView(Device, Resources, Light.ShadowData.View[i], Scene);
	}
}


void FShadowRenderer::RenderPointShadow(FD3DDevice& Device, FSystemResources& Resources, FPointLightParams& Light, FScene& Scene)
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

void FShadowRenderer::RenderSpotShadow(FD3DDevice& Device, FSystemResources& Resources, FSpotLightParams& Light, FScene& Scene)
{
	if (!Light.ShadowData.Settings.bCastShadows)
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

	FShadowAtlasResource& Atlas = Resources.ShadowResourceManager.GetAtlas();
	const bool bUseAtlas = View.bAtlasAllocated && IsShadowAtlasReady(Atlas, ShadowOptions);

	if (View.bAtlasAllocated && !bUseAtlas)
	{
		return;
	}

	if (!bUseAtlas && !IsShadowViewReady(View, ShadowOptions))
	{
		return;
	}

	//아틀라스를 쓰냐 마냐에 따라 Viewport가 바뀜
	PassContext.Viewport.TopLeftX = bUseAtlas ? static_cast<float>(View.AtlasOffsetX) : 0.0f;
	PassContext.Viewport.TopLeftY = bUseAtlas ? static_cast<float>(View.AtlasOffsetY) : 0.0f;
	PassContext.Viewport.Width = bUseAtlas ? static_cast<float>(View.AtlasSizeX) : static_cast<float>(View.DepthMap.Width);
	PassContext.Viewport.Height = bUseAtlas ? static_cast<float>(View.AtlasSizeY) : static_cast<float>(View.DepthMap.Height);
	PassContext.Viewport.MinDepth = 0.0f;
	PassContext.Viewport.MaxDepth = 1.0f;

	DeviceContext->RSSetViewports(1, &PassContext.Viewport);

	if (bUseAtlas)
	{
		if ((ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM))
		{
			ID3D11RenderTargetView* RTV = Atlas.Map.RTV;
			DeviceContext->OMSetRenderTargets(1, &RTV, Atlas.Map.DSV);
		}
		else
		{
			DeviceContext->OMSetRenderTargets(0, nullptr, Atlas.Map.DSV);
		}

		Resources.SetDepthStencilState(Device, EDepthStencilState::DepthGreaterEqual);
	}
	else if ((ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM))
	{
		ID3D11RenderTargetView* RTV = View.DepthMap.RTV;
		ID3D11DepthStencilView* DSV = View.DepthMap.DSV;
		DeviceContext->OMSetRenderTargets(1, &RTV, DSV);

		const float ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
		DeviceContext->ClearRenderTargetView(RTV, ClearColor);

		if (DSV)
		{
			DeviceContext->ClearDepthStencilView(DSV, D3D11_CLEAR_DEPTH, 0.0f, 0);
			Resources.SetDepthStencilState(Device, EDepthStencilState::DepthGreaterEqual);
		}
		else
		{
			Resources.SetDepthStencilState(Device, EDepthStencilState::NoDepth);
		}
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

	if ((ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM))
	{
		FShaderManager::Get().GetOrCreate(EShaderPath::MomentShadowMap)->Bind(DeviceContext);
	}
	else
	{
		FShaderManager::Get().GetOrCreate(EShaderPath::CommonShadowMap)->Bind(DeviceContext);
	}

	for (const FShadowDrawCommand& Cmd : Builder.GetCommands())
	{
		SubmitShadowCommand(Device, Cmd);
	}
}


//	RenderResources.cpp의 UpdateFrameBuffer() 참고
void FShadowRenderer::BindShadowFrameConstants(FD3DDevice& Device, FSystemResources& Resources,
	const FShadowPassContext& Context)
{
	ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();

	FFrameConstants FrameData = {};
	FrameData.View = Context.View;
	FrameData.Projection = Context.Proj;
	FrameData.InvProj = Context.Proj.GetInverse();
	FrameData.InvViewProj = Context.ViewProj.GetInverse();
	FrameData.bIsWireframe = 0.0f;
	FrameData.WireframeColor = FVector(1.0f, 1.0f, 1.0f);
	FrameData.Time = 0.0f;
	FrameData.CameraWorldPos = FVector(0.0f, 0.0f, 0.0f);

	Resources.FrameBuffer.Update(DeviceContext, &FrameData, sizeof(FFrameConstants));

	ID3D11Buffer* b0 = Resources.FrameBuffer.GetBuffer();
	DeviceContext->VSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
	DeviceContext->PSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
	DeviceContext->CSSetConstantBuffers(ECBSlot::Frame, 1, &b0);

	FLightingCBData ShadowPassLightingData = {};
	ShadowPassLightingData.ShadowFilterMode = static_cast<uint32>(ShadowOptions.ShadowFilterMode);
	Resources.LightingConstantBuffer.Update(DeviceContext, &ShadowPassLightingData, sizeof(FLightingCBData));

	ID3D11Buffer* b4 = Resources.LightingConstantBuffer.GetBuffer();
	DeviceContext->PSSetConstantBuffers(ECBSlot::Lighting, 1, &b4);
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
