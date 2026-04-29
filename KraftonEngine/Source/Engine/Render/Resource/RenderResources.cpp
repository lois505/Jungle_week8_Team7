#include "RenderResources.h"
#include "Render/Device/D3DDevice.h"
#include "Materials/MaterialManager.h"
#include "Render/Pipeline/ForwardLightData.h"
#include "Render/Pipeline/FrameContext.h"
#include "Render/Proxy/FScene.h"
#include "Engine/Runtime/Engine.h"
#include "Profiling/Timer.h"

namespace
{
	constexpr float kLocalESMExponent = 1000.0f;
	constexpr float kDirectionalPSMESMExponent = 80.0f;
	constexpr float kDirectionalCSMESMExponent[4] = { 40.0f, 60.0f, 80.0f, 300.0f };
}

void FTileCullingResource::Create(ID3D11Device* Dev, uint32 InTileCountX, uint32 InTileCountY)
{
	Release();
	TileCountX = InTileCountX;
	TileCountY = InTileCountY;
	const uint32 NumTiles = TileCountX * TileCountY;

	auto MakeStructured = [&](
		uint32 ElemCount, uint32 Stride,
		ID3D11Buffer** OutBuf,
		ID3D11UnorderedAccessView** OutUAV,
		ID3D11ShaderResourceView** OutSRV)
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = ElemCount * Stride;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = Stride;
			Dev->CreateBuffer(&bd, nullptr, OutBuf);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {};
			uavd.Format = DXGI_FORMAT_UNKNOWN;
			uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavd.Buffer.NumElements = ElemCount;
			Dev->CreateUnorderedAccessView(*OutBuf, &uavd, OutUAV);

			if (OutSRV)
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
				srvd.Format = DXGI_FORMAT_UNKNOWN;
				srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				srvd.Buffer.NumElements = ElemCount;
				Dev->CreateShaderResourceView(*OutBuf, &srvd, OutSRV);
			}
		};

	// t9 / u0: TileLightIndices — uint per slot per tile
	MakeStructured(ETileCulling::MaxLightsPerTile * NumTiles, sizeof(uint32),
		&IndicesBuffer, &IndicesUAV, &IndicesSRV);

	// t10 / u1: TileLightGrid — uint2 (offset, count) per tile
	MakeStructured(NumTiles, sizeof(uint32) * 2,
		&GridBuffer, &GridUAV, &GridSRV);

	// u2: GlobalLightCounter — single atomic uint
	MakeStructured(1, sizeof(uint32),
		&CounterBuffer, &CounterUAV, nullptr);
}

void FTileCullingResource::Release()
{
	if (IndicesSRV) { IndicesSRV->Release();  IndicesSRV = nullptr; }
	if (GridSRV) { GridSRV->Release();     GridSRV = nullptr; }
	if (IndicesUAV) { IndicesUAV->Release();  IndicesUAV = nullptr; }
	if (GridUAV) { GridUAV->Release();     GridUAV = nullptr; }
	if (CounterUAV) { CounterUAV->Release();  CounterUAV = nullptr; }
	if (IndicesBuffer) { IndicesBuffer->Release();  IndicesBuffer = nullptr; }
	if (GridBuffer) { GridBuffer->Release();     GridBuffer = nullptr; }
	if (CounterBuffer) { CounterBuffer->Release();  CounterBuffer = nullptr; }
	TileCountX = TileCountY = 0;
}

void FSystemResources::Create(ID3D11Device* InDevice, ID3D11DeviceContext* Context)
{
	FrameBuffer.Create(InDevice, sizeof(FFrameConstants));
	LightingConstantBuffer.Create(InDevice, sizeof(FLightingCBData));
	DirectionalShadowBuffer.Create(InDevice, sizeof(FDirectionalConstants));
	PSMShadowBuffer.Create(InDevice, sizeof(FPSMShadowConstants));
	ForwardLights.Create(InDevice, 32);
	LocalShadows.Create(InDevice, 32);

	ShadowResourceManager.Initialize(InDevice, Context);

	RasterizerStateManager.Create(InDevice);
	DepthStencilStateManager.Create(InDevice);
	BlendStateManager.Create(InDevice);
	SamplerStateManager.Create(InDevice);

	FMaterialManager::Get().Initialize(InDevice);
}

void FSystemResources::Release()
{
	SamplerStateManager.Release();
	BlendStateManager.Release();
	DepthStencilStateManager.Release();
	RasterizerStateManager.Release();

	FrameBuffer.Release();
	LightingConstantBuffer.Release();
	DirectionalShadowBuffer.Release();
	PSMShadowBuffer.Release();
	ForwardLights.Release();
	LocalShadows.Release();
	TileCullingResource.Release();

	ShadowResourceManager.Release();
}

void FSystemResources::UpdateFrameBuffer(FD3DDevice& Device, const FFrameContext& Frame)
{
	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();

	FFrameConstants frameConstantData = {};
	frameConstantData.View = Frame.View;
	frameConstantData.Projection = Frame.Proj;
	frameConstantData.InvProj = Frame.Proj.GetInverse();
	frameConstantData.InvViewProj = (Frame.View * Frame.Proj).GetInverse();
	frameConstantData.bIsWireframe = (Frame.RenderOptions.ViewMode == EViewMode::Wireframe);
	frameConstantData.WireframeColor = Frame.WireframeColor;
	frameConstantData.CameraWorldPos = Frame.CameraPosition;

	if (GEngine && GEngine->GetTimer())
	{
		frameConstantData.Time = static_cast<float>(GEngine->GetTimer()->GetTotalTime());
	}

	FrameBuffer.Update(Ctx, &frameConstantData, sizeof(FFrameConstants));
	ID3D11Buffer* b0 = FrameBuffer.GetBuffer();
	Ctx->VSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
	Ctx->PSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
	Ctx->CSSetConstantBuffers(ECBSlot::Frame, 1, &b0);
}

void FSystemResources::UpdateLightAndShadowBuffer(FD3DDevice& Device, const FScene& Scene, const FFrameContext& Frame, const FShadowRuntimeOptions& ShadowOptions, const FClusterCullingState* ClusterState)
{
	ID3D11Device* Dev = Device.GetDevice();
	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();

	FLightingCBData GlobalLightingData = {};
	const FSceneEnvironment& Env = Scene.GetEnvironment();
	if (Env.HasGlobalAmbientLight())
	{
		FGlobalAmbientLightParams DirLightParams = Env.GetGlobalAmbientLightParams();
		GlobalLightingData.Ambient.Intensity = DirLightParams.Intensity;
		GlobalLightingData.Ambient.Color = DirLightParams.LightColor;
	}
	else
	{
		// 폴백: 씬에 AmbientLight 없으면 최소 ambient 보장 (검정 방지)
		GlobalLightingData.Ambient.Intensity = 0.15f;
		GlobalLightingData.Ambient.Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	}

	const FShadowAtlasResource& ShadowAtlas = ShadowResourceManager.GetAtlas();

	if (Env.HasGlobalDirectionalLight())
	{
		FGlobalDirectionalLightParams DirLightParams = Env.GetGlobalDirectionalLightParams();
		GlobalLightingData.Directional.Intensity = DirLightParams.Intensity;
		GlobalLightingData.Directional.Color = DirLightParams.LightColor;
		GlobalLightingData.Directional.Direction = DirLightParams.Direction;

		if (DirLightParams.ShadowData.Settings.bCastShadows)
		{
			const FDirectionalShadowData& ShadowData = DirLightParams.ShadowData;
			FDirectionalConstants DirectionalCB = {};
			DirectionalCB.NumcasCade = (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::PSM) ? 1 : ShadowData.NUM_CASCADES;

			for (int i = 0; i < DirectionalCB.NumcasCade; i++)
			{
				const FShadowViewData& View = ShadowData.View[i];
				DirectionalCB.DirLightViewProj[i] = View.LightViewProj.ConvertToPOD();
				DirectionalCB.CascadeEndClip[i] = ShadowData.CascadeEndClipZ[i];
			}

			DirectionalCB.ShadowBias = DirLightParams.ShadowData.Settings.ShadowBias;
			DirectionalCB.ShadowSlopeBias = DirLightParams.ShadowData.Settings.ShadowSlopeBias;
			DirectionalCB.ShadowSharpen = DirLightParams.ShadowData.Settings.ShadowSharpen;
			DirectionalCB.PSMMainViewProjection = ShadowData.MainViewProjection.ConvertToPOD();
			DirectionalCB.PSMLightViewProj = ShadowData.PSMView.LightViewProj.ConvertToPOD();
			DirectionalCB.UsePSMShadow = (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::PSM) ? 1u : 0u;
			DirectionalCB.DirectionalESMExponentPSM = kDirectionalPSMESMExponent;
			for (int32 Cascade = 0; Cascade < 4; ++Cascade)
			{
				DirectionalCB.DirectionalESMExponentCSM[Cascade] = kDirectionalCSMESMExponent[Cascade];
			}

			// 추가 팁: 셰이더에서 texelSize를 계산할 수 있도록 해상도 정보를 CB에 포함하세요.
			// 현재 Resolution이 2048이라면, 셰이더는 이 값을 받아 PCF 필터링 보폭을 정하게 됩니다.
			//DirectionalCB.ShadowResolution = (float)ShadowResourceManager.GetShadowArray().CurrentWidth;

			const FDirectionalShadowArray& DirectionalArray = ShadowResourceManager.GetShadowArray();
			const bool bUseMomentDirectional = (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM
				|| ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM);
			ID3D11ShaderResourceView* DirShadowSRV = bUseMomentDirectional
				? DirectionalArray.MomentSRV
				: DirectionalArray.SRV;
			Ctx->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &DirShadowSRV);

			DirectionalShadowBuffer.Update(Ctx, &DirectionalCB, sizeof(FDirectionalConstants));
			ID3D11Buffer* buf = DirectionalShadowBuffer.GetBuffer();
			Ctx->PSSetConstantBuffers(ECBSlot::DirectionalShadow, 1, &buf);
		}

	}

	const uint32 NumPointLights = Env.GetNumPointLights();
	const uint32 NumSpotLights = Env.GetNumSpotLights();
	GlobalLightingData.NumActivePointLights = NumPointLights;
	GlobalLightingData.NumActiveSpotLights = NumSpotLights;

	TArray<FLightInfo> LightInfos;
	TArray<FLocalShadowInfo> ShadowInfos;
	LightInfos.reserve(NumPointLights + NumSpotLights);
	ShadowInfos.reserve(NumPointLights + NumSpotLights);

	for (uint32 i = 0; i < NumPointLights; ++i)
	{
		const FPointLightParams& PointLight = Env.GetPointLight(i);

		LightInfos.emplace_back(PointLight.ToLightInfo());
		ShadowInfos.emplace_back(PointLight.ShadowData.ConvertToLocalShadowInfo(ShadowAtlas));
	}
	for (uint32 i = 0; i < NumSpotLights; ++i)
	{
		const FSpotLightParams& SpotLight = Env.GetSpotLight(i);

		LightInfos.emplace_back(SpotLight.ToLightInfo());
		ShadowInfos.emplace_back(SpotLight.ShadowData.ConvertToLocalShadowInfo(ShadowAtlas));
	}

	LastNumLights = static_cast<uint32>(LightInfos.size());

	GlobalLightingData.LightCullingMode = static_cast<uint32>(Frame.RenderOptions.LightCullingMode);
	GlobalLightingData.VisualizeLightCulling = Frame.RenderOptions.ViewMode == EViewMode::LightCulling ? 1u : 0u;
	GlobalLightingData.HeatMapMax = Frame.RenderOptions.HeatMapMax;
	GlobalLightingData.ShadowFilterMode = static_cast<uint32>(ShadowOptions.ShadowFilterMode);
	GlobalLightingData.LocalESMExponent = kLocalESMExponent;
	GlobalLightingData.DebugCascades = ShadowOptions.bDebugCascades ? 1u : 0u;
	GlobalLightingData.EnableShadows = Frame.RenderOptions.ShowFlags.bShadow ? 1u : 0u;
	
	if (ClusterState)
	{
		GlobalLightingData.ClusterCullingState = *ClusterState;
	}

	// 이전 프레임 타일 컬링 결과에서 타일 수 읽기 (1-frame latent)
	GlobalLightingData.NumTilesX = TileCullingResource.TileCountX;
	GlobalLightingData.NumTilesY = TileCullingResource.TileCountY;

	LightingConstantBuffer.Update(Ctx, &GlobalLightingData, sizeof(FLightingCBData));
	ID3D11Buffer* b4 = LightingConstantBuffer.GetBuffer();
	Ctx->VSSetConstantBuffers(ECBSlot::Lighting, 1, &b4);
	Ctx->PSSetConstantBuffers(ECBSlot::Lighting, 1, &b4);
	Ctx->CSSetConstantBuffers(ECBSlot::Lighting, 1, &b4);

	ForwardLights.Update(Dev, Ctx, LightInfos);
	Ctx->VSSetShaderResources(ELightTexSlot::AllLights, 1, &ForwardLights.LightBufferSRV);
	Ctx->PSSetShaderResources(ELightTexSlot::AllLights, 1, &ForwardLights.LightBufferSRV);

	LocalShadows.Update(Dev, Ctx, ShadowInfos);
	Ctx->VSSetShaderResources(ELightTexSlot::LocalLights, 1, &LocalShadows.ShadowBufferSRV);
	Ctx->PSSetShaderResources(ELightTexSlot::LocalLights, 1, &LocalShadows.ShadowBufferSRV);

	ID3D11ShaderResourceView* ShadowAtlasSRV = ShadowAtlas.Map.SRV;
	Ctx->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &ShadowAtlasSRV);

	if (Frame.RenderOptions.LightCullingMode == ELightCullingMode::Tile)
	{
		BindTileCullingBuffers(Device);
	}
	else
	{
		UnbindTileCullingBuffers(Device);
	}
}

void FSystemResources::BindTileCullingBuffers(FD3DDevice& Device)
{
	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();
	Ctx->VSSetShaderResources(ELightTexSlot::TileLightIndices, 1, &TileCullingResource.IndicesSRV);
	Ctx->VSSetShaderResources(ELightTexSlot::TileLightGrid, 1, &TileCullingResource.GridSRV);
	Ctx->PSSetShaderResources(ELightTexSlot::TileLightIndices, 1, &TileCullingResource.IndicesSRV);
	Ctx->PSSetShaderResources(ELightTexSlot::TileLightGrid, 1, &TileCullingResource.GridSRV);
	Ctx->VSSetShaderResources(ELightTexSlot::AllLights, 1, &ForwardLights.LightBufferSRV);
	Ctx->PSSetShaderResources(ELightTexSlot::AllLights, 1, &ForwardLights.LightBufferSRV);
}

void FSystemResources::UnbindTileCullingBuffers(FD3DDevice& Device)
{
	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();
	ID3D11ShaderResourceView* NullSRVs[2] = {};
	Ctx->VSSetShaderResources(ELightTexSlot::TileLightIndices, 2, NullSRVs);
	Ctx->PSSetShaderResources(ELightTexSlot::TileLightIndices, 2, NullSRVs);
	Ctx->CSSetShaderResources(ELightTexSlot::TileLightIndices, 2, NullSRVs);
}

void FSystemResources::BindSystemSamplers(FD3DDevice& Device)
{
	SamplerStateManager.BindSystemSamplers(Device.GetDeviceContext());
}

void FSystemResources::SetDepthStencilState(FD3DDevice& Device, EDepthStencilState InState)
{
	DepthStencilStateManager.Set(Device.GetDeviceContext(), InState);
}

void FSystemResources::SetBlendState(FD3DDevice& Device, EBlendState InState)
{
	BlendStateManager.Set(Device.GetDeviceContext(), InState);
}

void FSystemResources::SetRasterizerState(FD3DDevice& Device, ERasterizerState InState)
{
	RasterizerStateManager.Set(Device.GetDeviceContext(), InState);
}

void FSystemResources::UpdateShadowResources(FScene& Scene, const FShadowRuntimeOptions& ShadowOptions, const FFrameContext& Frame)
{
	ShadowResourceManager.UpdateShadowResources(Scene.GetEnvironment(), ShadowOptions, Frame);
}

void FSystemResources::ResetRenderStateCache()
{
	RasterizerStateManager.ResetCache();
	DepthStencilStateManager.ResetCache();
	BlendStateManager.ResetCache();
}

void FSystemResources::UnbindSystemTextures(FD3DDevice& Device)
{
	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();
	ID3D11ShaderResourceView* nullSRV = nullptr;
	Ctx->PSSetShaderResources(ESystemTexSlot::SceneDepth, 1, &nullSRV);
	Ctx->PSSetShaderResources(ESystemTexSlot::SceneColor, 1, &nullSRV);
	Ctx->PSSetShaderResources(ESystemTexSlot::GBufferNormal, 1, &nullSRV);
	Ctx->PSSetShaderResources(ESystemTexSlot::Stencil, 1, &nullSRV);
	Ctx->PSSetShaderResources(ESystemTexSlot::CullingHeatmap, 1, &nullSRV);
	Ctx->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &nullSRV);
	Ctx->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &nullSRV);
}
