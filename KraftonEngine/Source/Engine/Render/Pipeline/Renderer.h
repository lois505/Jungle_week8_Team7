#pragma once

/*
	실제 렌더링을 담당하는 Class 입니다. (Rendering 최상위 클래스)
*/

#include "ShadowRenderer.h"
#include "Render/Types/RenderTypes.h"

#include "Render/Pipeline/FrameContext.h"
#include "Render/Pipeline/DrawCommandBuilder.h"
#include "Render/Pipeline/PassRenderStateTable.h"
#include "Render/Pipeline/PassEventBuilder.h"
#include "Render/Device/D3DDevice.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Culling/TileBasedLightCulling.h"
#include "Render/Culling/ClusteredLightCuller.h"

class FScene;

enum class EShadowDepthPreviewSlot : uint32
{
	LightProperty = 0,
	LocalAtlas,
	Count
};

class FRenderer
{
public:
	void Create(HWND hWindow);
	void Release();

	// --- Render phase: 정렬 + GPU 제출 ---
	void BeginFrame();
	void Render(const FFrameContext& Frame, FScene& Scene);
	void EndFrame();

	FD3DDevice& GetFD3DDevice() { return Device; }

	// Collect 페이즈에서 커맨드 빌드를 담당하는 Builder
	FDrawCommandBuilder& GetBuilder() { return Builder; }

	// 뷰포트 리사이즈 후 렌더 상태 캐시 초기화
	void ResetRenderStateCache() { Resources.ResetRenderStateCache(); }

	// TileBasedLightCulling Dispatch에 필요한 리소스 접근자
	ID3D11Buffer*             GetFrameBuffer()         { return Resources.FrameBuffer.GetBuffer(); }
	ID3D11ShaderResourceView* GetLightBufferSRV()      { return Resources.ForwardLights.LightBufferSRV; }
	FTileCullingResource&     GetTileCullingResource() { return Resources.TileCullingResource; }
	uint32                    GetNumLights()    const  { return Resources.LastNumLights; }
	FTileBasedLightCulling&   GetTileBaseCulling()     { return TileBasedCulling; }
	const FShadowAtlasResource& GetShadowAtlas() const { return Resources.ShadowResourceManager.GetAtlas(); }
	const FDirectionalShadowArray& GetDirShadowArray() const { return Resources.ShadowResourceManager.GetShadowArray(); }
	const FShadowTelemetry& GetShadowTelemetry() const { return Resources.ShadowResourceManager.GetTelemetry(); }
	const TArray<FLocalShadowRequest>& GetLocalShadowRequests() const { return Resources.ShadowResourceManager.GetLocalShadowRequests(); }
	ID3D11ShaderResourceView* RenderShadowDepthPreview(
		EShadowDepthPreviewSlot Slot,
		ID3D11ShaderResourceView* SourceSRV,
		float U0, float V0, float U1, float V1,
		bool bSourceArray);

	void BindTileCullingResources() { Resources.BindTileCullingBuffers(Device); }
	void UnbindTileCullingResources() { Resources.UnbindTileCullingBuffers(Device); }
	void DispatchClusterCullingResources();
	void BindClusterCullingResources();
	void UnbindClusterCullingResources();
	
	//	Wrapper for Shadow
	const FShadowRuntimeOptions& GetRuntimeOptions() const { return ShadowRenderer.GetRuntimeOptions(); }
	
	void SetShadowFilterMode(EShadowFilterMode ShadowFilterMode) { ShadowRenderer.SetShadowFilterMode(ShadowFilterMode); }
	void SetDirectionalShadowMode(EDirectionalShadowMode ShadowMode) { ShadowRenderer.SetDirectionalShadowMode(ShadowMode); }
	void SetSkipShadowPassInUnlit(bool bSkip) { ShadowRenderer.SetSkipShadowPassInUnlit(bSkip); }
	void SetDebugCascades(bool bEnable) { ShadowRenderer.SetDebugCascades(bEnable); }
	void SetMaxLocalShadowViewsPerFrame(uint32 MaxViewsPerFrame) { Resources.ShadowResourceManager.SetMaxLocalShadowViewsPerFrame(MaxViewsPerFrame); }
	uint32 GetMaxLocalShadowViewsPerFrame() const { return Resources.ShadowResourceManager.GetMaxLocalShadowViewsPerFrame(); }
	void SetMaxLocalShadowAtlasAreaPerFrame(uint64 MaxAreaPerFrame) { Resources.ShadowResourceManager.SetMaxLocalShadowAtlasAreaPerFrame(MaxAreaPerFrame); }
	uint64 GetMaxLocalShadowAtlasAreaPerFrame() const { return Resources.ShadowResourceManager.GetMaxLocalShadowAtlasAreaPerFrame(); }
	void SetLocalShadowAlignment(uint32 InAlignment) { Resources.ShadowResourceManager.SetLocalShadowAlignment(InAlignment); }
	uint32 GetLocalShadowAlignment() const { return Resources.ShadowResourceManager.GetLocalShadowAlignment(); }

private:
	// 패스 루프 종료 후 시스템 텍스처 언바인딩 + 캐시 정리
	void CleanupPassState(FStateCache& Cache);
	bool EnsureShadowDepthPreviewTarget(uint32 SlotIndex);
	void ReleaseShadowDepthPreviewTargets();

private:
	FD3DDevice Device;

	FSystemResources Resources;
	FDrawCommandBuilder Builder;
	FPassRenderStateTable PassRenderStateTable;
	FPassEventBuilder PassEventBuilder;
	
	//	Shadow
	FShadowRenderer ShadowRenderer;
	FConstantBuffer ShadowDepthPreviewCB;
	static constexpr uint32 ShadowDepthPreviewSlotCount = static_cast<uint32>(EShadowDepthPreviewSlot::Count);
	ID3D11Texture2D* ShadowDepthPreviewTextures[ShadowDepthPreviewSlotCount] = {};
	ID3D11RenderTargetView* ShadowDepthPreviewRTVs[ShadowDepthPreviewSlotCount] = {};
	ID3D11ShaderResourceView* ShadowDepthPreviewSRVs[ShadowDepthPreviewSlotCount] = {};
	static constexpr uint32 ShadowDepthPreviewSize = 256;
	
	FTileBasedLightCulling TileBasedCulling;
	FClusteredLightCuller ClusteredLightCuller;
};
