#pragma once
#include "ShadowDrawCommandBuilder.h"
#include "Render/Device/D3DDevice.h"
#include "Render/Types/GlobalLightParams.h"
#include "Render/Resource/Buffer.h"

struct FShadowPassContext;
struct FGlobalDirectionalLightParams;
struct FFrameContext;
struct FSystemResources;
class FSceneEnvironment;
class FScene;

class FShadowRenderer
{
public:
	void Create(ID3D11Device* Device, ID3D11DeviceContext* DeviceContext);
	void Release();

	void RenderShadows(FD3DDevice& Device, FSystemResources& Resources, FScene& Scene, const FFrameContext& MainFrame);

	/* Getter, Setter */
	const FShadowRuntimeOptions& GetRuntimeOptions() const { return ShadowOptions; }

	void SetShadowFilterMode(EShadowFilterMode ShadowFilterMode) { ShadowOptions.ShadowFilterMode = ShadowFilterMode; }
	void SetDirectionalShadowMode(EDirectionalShadowMode ShadowMode) { ShadowOptions.DirectionalShadowMode = ShadowMode; }
	void SetSkipShadowPassInUnlit(bool bSkip) { ShadowOptions.bSkipShadowPassInUnlit = bSkip; }
	void SetDebugCascades(bool bEnable) { ShadowOptions.bDebugCascades = bEnable; }

private:
	struct FShadowRenderResult
	{
		uint32 SubmittedViewCount = 0;
		uint32 InvalidViewCount = 0;
	};

	FShadowRenderResult RenderDirectionalShadow(FD3DDevice& Device, FSystemResources& Resources, FGlobalDirectionalLightParams& Light,
		FScene& Scene, const FFrameContext MainFrame);
	FShadowRenderResult RenderPointShadow(FD3DDevice& Device, FSystemResources& Resources, FPointLightParams& Light, FScene& Scene);
	FShadowRenderResult RenderSpotShadow(FD3DDevice& Device, FSystemResources& Resources, FSpotLightParams& Light, FScene& Scene);

	bool RenderShadowView(FD3DDevice& Device, FSystemResources& Resources, FShadowViewData& View, FScene& Scene,
		bool bUsePSMShader = false, const FMatrix* PSMMainViewProjection = nullptr);
	void UnbindShadowReadResourcesForWrite(FD3DDevice& Device);
	void UnbindShadowWriteTargets(FD3DDevice& Device);

	void BindShadowFrameConstants(FD3DDevice& Device, FSystemResources& Resources, const FShadowPassContext& Context);
	void RenderAtlasMomentBlurPass(FD3DDevice& Device, FSystemResources& Resources, const FSceneEnvironment& Environment);
	void RenderDirectionalMomentBlurPass(FD3DDevice& Device, FSystemResources& Resources, const FSceneEnvironment& Environment);

	void SubmitShadowCommand(FD3DDevice& Device, const FShadowDrawCommand& Cmd);

private:
	FShadowRuntimeOptions ShadowOptions;
	FConstantBuffer ShadowFilterConstantBuffer;

	FShadowDrawCommandBuilder Builder;
	uint32 ShadowDrawCallCountThisFrame = 0;
};
