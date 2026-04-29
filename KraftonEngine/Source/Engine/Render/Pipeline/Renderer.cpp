#include "Renderer.h"

#include "Render/Types/RenderTypes.h"
#include "Render/Resource/ShaderManager.h"
#include "Core/Log.h"
#include "Render/Proxy/FScene.h"
#include "Profiling/Stats.h"
#include "Profiling/GPUProfiler.h"
#include "Materials/MaterialManager.h"
#include "Profiling/StartupTrace.h"


void FRenderer::Create(HWND hWindow)
{
	STARTUP_TRACE_SCOPE("FRenderer::Create");
	{
		STARTUP_TRACE_SCOPE("D3DDevice.Create");
		Device.Create(hWindow);
	}

	if (Device.GetDevice() == nullptr)
	{
		UE_LOG("Failed to create D3D Device.");
	}

	{
		STARTUP_TRACE_SCOPE("ShaderManager.Initialize");
		FShaderManager::Get().Initialize(Device.GetDevice());
	}
	{
		STARTUP_TRACE_SCOPE("SystemResources.Create");
		Resources.Create(Device.GetDevice(), Device.GetDeviceContext());
	}

	{
		STARTUP_TRACE_SCOPE("TileBasedCulling.Initialize");
		TileBasedCulling.Initialize(Device.GetDevice());
	}
	{
		STARTUP_TRACE_SCOPE("ClusteredLightCuller.Initialize");
		ClusteredLightCuller.Initialize(Device.GetDevice(), Device.GetDeviceContext());
	}

	{
		STARTUP_TRACE_SCOPE("PassRenderStateTable.Initialize");
		PassRenderStateTable.Initialize();
	}

	{
		STARTUP_TRACE_SCOPE("DrawCommandBuilder.Create");
		Builder.Create(Device.GetDevice(), Device.GetDeviceContext(), &PassRenderStateTable);
	}

	{
		STARTUP_TRACE_SCOPE("ShadowRenderer.Create");
		ShadowRenderer.Create(Device.GetDevice(), Device.GetDeviceContext());
	}
	ShadowDepthPreviewCB.Create(Device.GetDevice(), sizeof(FShadowDepthPreviewConstants));

	// GPU Profiler 초기화
	{
		STARTUP_TRACE_SCOPE("GPUProfiler.Initialize");
		FGPUProfiler::Get().Initialize(Device.GetDevice(), Device.GetDeviceContext());
	}
}

void FRenderer::Release()
{
	FGPUProfiler::Get().Shutdown();

	ShadowRenderer.Release();
	ShadowDepthPreviewCB.Release();
	ReleaseShadowDepthPreviewTargets();

	Builder.Release();

	Resources.Release();
	TileBasedCulling.Release();
	ClusteredLightCuller.Release();
	FShaderManager::Get().Release();
	FMaterialManager::Get().Release();

	Device.Release();
}

//	스왑체인 백버퍼 복귀 — ImGui 합성 직전에 호출
void FRenderer::BeginFrame()
{
	Device.BeginFrame();
}

// ============================================================
// Render — 정렬 + GPU 제출
// BeginCollect + Collector + BuildDynamicCommands 이후에 호출.
// ============================================================
void FRenderer::Render(const FFrameContext& Frame, FScene& Scene)
{
	FDrawCallStats::Reset();

	{
		SCOPE_STAT_CAT("UpdateFrameBuffer", "4_ExecutePass");
		Resources.UpdateFrameBuffer(Device, Frame);
	}

	/*	Shadow Pass	*/
	FShadowRuntimeOptions EffectiveShadowOptions = ShadowRenderer.GetRuntimeOptions();
	if (!Frame.RenderOptions.ShowFlags.bShadow)
	{
		EffectiveShadowOptions.ShadowFilterMode = EShadowFilterMode::None;
		EffectiveShadowOptions.bDebugCascades = false;
	}
	{
		SCOPE_STAT_CAT("Shadow Pass", "4_ExecutePass");
		const bool bSkipByShowFlag = !Frame.RenderOptions.ShowFlags.bShadow;
		const bool bSkipShadowPassInUnlit = (Frame.RenderOptions.ViewMode == EViewMode::Unlit) && EffectiveShadowOptions.bSkipShadowPassInUnlit;

		if (!bSkipByShowFlag && !bSkipShadowPassInUnlit)
		{
			{
				SCOPE_STAT_CAT("Shadow.UnbindSystemTextures", "4_ExecutePass");
				Resources.UnbindSystemTextures(Device);
			}
			{
				SCOPE_STAT_CAT("Shadow.UpdateResources", "4_ExecutePass");
				Resources.UpdateShadowResources(Scene, EffectiveShadowOptions, Frame);
			}
			{
				SCOPE_STAT_CAT("Shadow.ClearAtlasTextures", "4_ExecutePass");
				Resources.ShadowResourceManager.ClearAtlasTexturesOnly(EffectiveShadowOptions);
			}
			{
				SCOPE_STAT_CAT("Shadow.ResetAtlasAllocState", "4_ExecutePass");
				Resources.ShadowResourceManager.ResetAtlasAllocationStateForFrame();
			}
			{
				SCOPE_STAT_CAT("Shadow.RenderViews", "4_ExecutePass");
				ShadowRenderer.RenderShadows(Device, Resources, Scene, Frame);
			}

			//	Restore
			Resources.UpdateFrameBuffer(Device, Frame);
		}
	}

	{
		SCOPE_STAT_CAT("UpdateLightBuffer", "4_ExecutePass");

		FClusterCullingState& ClusterState = ClusteredLightCuller.GetCullingState();
		ClusterState.NearZ = Frame.NearClip;
		ClusterState.FarZ = Frame.FarClip;
		ClusterState.ScreenWidth = static_cast<uint32>(Frame.ViewportWidth);
		ClusterState.ScreenHeight = static_cast<uint32>(Frame.ViewportHeight);

		Resources.UpdateLightAndShadowBuffer(Device, Scene, Frame, EffectiveShadowOptions, &ClusterState);
	}

	// 시스템 샘플러 영구 바인딩 (s0-s2)
	Resources.BindSystemSamplers(Device);

	FDrawCommandList& CommandList = Builder.GetCommandList();

	// 커맨드 정렬 + 패스별 오프셋 빌드
	CommandList.Sort();

	// 단일 StateCache — 패스 간 상태 유지 (DSV Read-Only 전환 등)
	FStateCache Cache;
	Cache.Reset();
	Cache.RTV = Frame.ViewportRTV;
	Cache.DSV = Frame.ViewportDSV;

	// ── Pre/Post 패스 이벤트 등록 ──
	TArray<FPassEvent> PrePassEvents;
	TArray<FPassEvent> PostPassEvents;
	PassEventBuilder.Build(Device, Frame, Cache, this, PrePassEvents, PostPassEvents);

	// ── 패스 루프 ──
	for (uint32 i = 0; i < (uint32)ERenderPass::MAX; ++i)
	{
		ERenderPass CurPass = static_cast<ERenderPass>(i);

		for (auto& PrePassEvent : PrePassEvents)
		{
			PrePassEvent.TryExecute(CurPass);
		}

		uint32 Start, End;
		CommandList.GetPassRange(CurPass, Start, End);
		if (Start >= End) continue;

		const char* PassName = GetRenderPassName(CurPass);
		SCOPE_STAT_CAT(PassName, "4_ExecutePass");
		GPU_SCOPE_STAT(PassName);

		CommandList.SubmitRange(Start, End, Device, Resources, Cache);

		for (auto& PostPassEvent : PostPassEvents)
		{
			PostPassEvent.TryExecute(CurPass);
		}
	}

	CleanupPassState(Cache);
}

// ============================================================
// CleanupPassState — 패스 루프 종료 후 시스템 텍스처 언바인딩 + 캐시 정리
// ============================================================
void FRenderer::CleanupPassState(FStateCache& Cache)
{
	Resources.UnbindSystemTextures(Device);
	Resources.UnbindTileCullingBuffers(Device);
	UnbindClusterCullingResources();

	Cache.Cleanup(Device.GetDeviceContext());
	Builder.GetCommandList().Reset();
}

bool FRenderer::EnsureShadowDepthPreviewTarget(uint32 SlotIndex)
{
	if (SlotIndex >= ShadowDepthPreviewSlotCount)
	{
		return false;
	}

	if (ShadowDepthPreviewTextures[SlotIndex] && ShadowDepthPreviewRTVs[SlotIndex] && ShadowDepthPreviewSRVs[SlotIndex])
	{
		return true;
	}

	if (ShadowDepthPreviewSRVs[SlotIndex])
	{
		ShadowDepthPreviewSRVs[SlotIndex]->Release();
		ShadowDepthPreviewSRVs[SlotIndex] = nullptr;
	}
	if (ShadowDepthPreviewRTVs[SlotIndex])
	{
		ShadowDepthPreviewRTVs[SlotIndex]->Release();
		ShadowDepthPreviewRTVs[SlotIndex] = nullptr;
	}
	if (ShadowDepthPreviewTextures[SlotIndex])
	{
		ShadowDepthPreviewTextures[SlotIndex]->Release();
		ShadowDepthPreviewTextures[SlotIndex] = nullptr;
	}

	D3D11_TEXTURE2D_DESC TextureDesc = {};
	TextureDesc.Width = ShadowDepthPreviewSize;
	TextureDesc.Height = ShadowDepthPreviewSize;
	TextureDesc.MipLevels = 1;
	TextureDesc.ArraySize = 1;
	TextureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	TextureDesc.SampleDesc.Count = 1;
	TextureDesc.Usage = D3D11_USAGE_DEFAULT;
	TextureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	ID3D11Device* D3D = Device.GetDevice();
	if (!D3D || FAILED(D3D->CreateTexture2D(&TextureDesc, nullptr, &ShadowDepthPreviewTextures[SlotIndex])))
	{
		UE_LOG("Failed to create shadow depth preview texture.");
		return false;
	}

	if (FAILED(D3D->CreateRenderTargetView(ShadowDepthPreviewTextures[SlotIndex], nullptr, &ShadowDepthPreviewRTVs[SlotIndex]))
		|| FAILED(D3D->CreateShaderResourceView(ShadowDepthPreviewTextures[SlotIndex], nullptr, &ShadowDepthPreviewSRVs[SlotIndex])))
	{
		UE_LOG("Failed to create shadow depth preview views.");
		if (ShadowDepthPreviewSRVs[SlotIndex])
		{
			ShadowDepthPreviewSRVs[SlotIndex]->Release();
			ShadowDepthPreviewSRVs[SlotIndex] = nullptr;
		}
		if (ShadowDepthPreviewRTVs[SlotIndex])
		{
			ShadowDepthPreviewRTVs[SlotIndex]->Release();
			ShadowDepthPreviewRTVs[SlotIndex] = nullptr;
		}
		if (ShadowDepthPreviewTextures[SlotIndex])
		{
			ShadowDepthPreviewTextures[SlotIndex]->Release();
			ShadowDepthPreviewTextures[SlotIndex] = nullptr;
		}
		return false;
	}

	return true;
}

void FRenderer::ReleaseShadowDepthPreviewTargets()
{
	for (uint32 SlotIndex = 0; SlotIndex < ShadowDepthPreviewSlotCount; ++SlotIndex)
	{
		if (ShadowDepthPreviewSRVs[SlotIndex])
		{
			ShadowDepthPreviewSRVs[SlotIndex]->Release();
			ShadowDepthPreviewSRVs[SlotIndex] = nullptr;
		}
		if (ShadowDepthPreviewRTVs[SlotIndex])
		{
			ShadowDepthPreviewRTVs[SlotIndex]->Release();
			ShadowDepthPreviewRTVs[SlotIndex] = nullptr;
		}
		if (ShadowDepthPreviewTextures[SlotIndex])
		{
			ShadowDepthPreviewTextures[SlotIndex]->Release();
			ShadowDepthPreviewTextures[SlotIndex] = nullptr;
		}
	}
}

ID3D11ShaderResourceView* FRenderer::RenderShadowDepthPreview(
	EShadowDepthPreviewSlot Slot,
	ID3D11ShaderResourceView* SourceSRV,
	float U0, float V0, float U1, float V1,
	bool bSourceArray)
{
	const uint32 SlotIndex = static_cast<uint32>(Slot);
	if (!SourceSRV || !EnsureShadowDepthPreviewTarget(SlotIndex))
	{
		return SourceSRV;
	}

	FShader* PreviewShader = FShaderManager::Get().GetOrCreate(EShaderPath::ShadowDepthPreview);
	if (!PreviewShader)
	{
		return SourceSRV;
	}

	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();
	if (!Ctx)
	{
		return SourceSRV;
	}

	UINT OldViewportCount = 1;
	D3D11_VIEWPORT OldViewport = {};
	Ctx->RSGetViewports(&OldViewportCount, &OldViewport);

	ID3D11RenderTargetView* OldRTV = nullptr;
	ID3D11DepthStencilView* OldDSV = nullptr;
	Ctx->OMGetRenderTargets(1, &OldRTV, &OldDSV);

	D3D11_VIEWPORT PreviewViewport = {};
	PreviewViewport.Width = static_cast<float>(ShadowDepthPreviewSize);
	PreviewViewport.Height = static_cast<float>(ShadowDepthPreviewSize);
	PreviewViewport.MinDepth = 0.0f;
	PreviewViewport.MaxDepth = 1.0f;
	Ctx->RSSetViewports(1, &PreviewViewport);

	Resources.SetDepthStencilState(Device, EDepthStencilState::NoDepth);
	Resources.SetBlendState(Device, EBlendState::Opaque);
	Resources.SetRasterizerState(Device, ERasterizerState::SolidNoCull);
	Resources.BindSystemSamplers(Device);

	const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	Ctx->OMSetRenderTargets(1, &ShadowDepthPreviewRTVs[SlotIndex], nullptr);
	Ctx->ClearRenderTargetView(ShadowDepthPreviewRTVs[SlotIndex], ClearColor);

	FShadowDepthPreviewConstants Constants = {};
	Constants.U0 = U0;
	Constants.V0 = V0;
	Constants.U1 = U1;
	Constants.V1 = V1;
	Constants.bSourceArray = bSourceArray ? 1u : 0u;
	ShadowDepthPreviewCB.Update(Ctx, &Constants, sizeof(Constants));

	ID3D11Buffer* CB = ShadowDepthPreviewCB.GetBuffer();
	Ctx->PSSetConstantBuffers(ECBSlot::PerShader0, 1, &CB);

	ID3D11ShaderResourceView* NullSRV = nullptr;
	if (bSourceArray)
	{
		Ctx->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &NullSRV);
		Ctx->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &SourceSRV);
	}
	else
	{
		Ctx->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &SourceSRV);
		Ctx->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &NullSRV);
	}

	PreviewShader->Bind(Ctx);
	Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	Ctx->Draw(3, 0);

	ID3D11ShaderResourceView* NullSRVs[2] = {};
	Ctx->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 2, NullSRVs);

	Ctx->OMSetRenderTargets(1, &OldRTV, OldDSV);
	if (OldViewportCount > 0)
	{
		Ctx->RSSetViewports(1, &OldViewport);
	}

	if (OldRTV)
	{
		OldRTV->Release();
	}
	if (OldDSV)
	{
		OldDSV->Release();
	}

	Resources.ResetRenderStateCache();
	return ShadowDepthPreviewSRVs[SlotIndex];
}

void FRenderer::DispatchClusterCullingResources()
{
	if (!ClusteredLightCuller.IsInitialized())
	{
		return;
	}

	Resources.UnbindTileCullingBuffers(Device);
	UnbindClusterCullingResources();

	/*{
		GPU_SCOPE_STAT_CAT("ClutserCulling AABB Creation", "AABBCreation");
		ClusteredLightCuller.DispatchViewSpaceAABB();
	}*/
	{
		GPU_SCOPE_STAT_CAT("Cluster Culling Dispatch", "Culling Dispatch");
		ClusteredLightCuller.DispatchLightCullingCS(Resources.ForwardLights.LightBufferSRV);
	}

	BindClusterCullingResources();
}

void FRenderer::BindClusterCullingResources()
{
	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();
	ID3D11ShaderResourceView* LightIndexList = ClusteredLightCuller.GetLightIndexListSRV();
	ID3D11ShaderResourceView* LightGridList = ClusteredLightCuller.GetLightGridSRV();
	Ctx->VSSetShaderResources(ELightTexSlot::ClusterLightIndexList, 1, &LightIndexList);
	Ctx->VSSetShaderResources(ELightTexSlot::ClusterLightGrid, 1, &LightGridList);
	Ctx->PSSetShaderResources(ELightTexSlot::ClusterLightIndexList, 1, &LightIndexList);
	Ctx->PSSetShaderResources(ELightTexSlot::ClusterLightGrid, 1, &LightGridList);
}

void FRenderer::UnbindClusterCullingResources()
{
	ID3D11DeviceContext* Ctx = Device.GetDeviceContext();
	ID3D11ShaderResourceView* NullSRVs[2] = {};
	Ctx->VSSetShaderResources(ELightTexSlot::ClusterLightIndexList, 2, NullSRVs);
	Ctx->PSSetShaderResources(ELightTexSlot::ClusterLightIndexList, 2, NullSRVs);
	Ctx->CSSetShaderResources(ELightTexSlot::ClusterLightIndexList, 2, NullSRVs);
}

//	Present the rendered frame to the screen. 반드시 Render 이후에 호출되어야 함.
void FRenderer::EndFrame()
{
	Device.Present();
}
