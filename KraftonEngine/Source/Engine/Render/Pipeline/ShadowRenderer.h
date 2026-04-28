#pragma once
#include "ShadowDrawCommandBuilder.h"
#include "Render/Device/D3DDevice.h"
#include "Render/Types/GlobalLightParams.h"

struct FShadowPassContext;
struct FGlobalDirectionalLightParams;
struct FFrameContext;
struct FSystemResources;
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

private:
	void RenderDirectionalShadow(FD3DDevice& Device, FSystemResources& Resources, FGlobalDirectionalLightParams& Light,
		FScene& Scene, const FFrameContext MainFrame);
	void RenderPointShadow(FD3DDevice& Device, FSystemResources& Resources, FPointLightParams& Light, FScene& Scene);
	void RenderSpotShadow(FD3DDevice& Device, FSystemResources& Resources, FSpotLightParams& Light, FScene& Scene);

	void RenderShadowView(FD3DDevice& Device, FSystemResources& Resources, FShadowViewData& View, FScene& Scene,
		bool bUsePSMShader = false, const FMatrix* PSMMainViewProjection = nullptr);

	void BindShadowFrameConstants(FD3DDevice& Device, FSystemResources& Resources, const FShadowPassContext& Context);

	void SubmitShadowCommand(FD3DDevice& Device, const FShadowDrawCommand& Cmd);

private:
	FShadowRuntimeOptions ShadowOptions;

	FShadowDrawCommandBuilder Builder;
};
