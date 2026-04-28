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
