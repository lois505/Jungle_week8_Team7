#include "LocalShadowAllocationExecutor.h"

#include "Render/Culling/ConvexVolume.h"
#include "Render/Proxy/FScene.h"
#include "Render/Proxy/PrimitiveSceneProxy.h"
#include "Render/Proxy/SceneEnvironment.h"
#include "Engine/Core/EngineTypes.h"
#include <algorithm>

namespace
{
	bool IsValidShadowCasterProxy(const FPrimitiveSceneProxy& Proxy)
	{
		if (!Proxy.IsVisible())
		{
			return false;
		}

		if (Proxy.HasProxyFlag(EPrimitiveProxyFlags::EditorOnly | EPrimitiveProxyFlags::Decal | EPrimitiveProxyFlags::FontBatched))
		{
			return false;
		}

		if (Proxy.HasProxyFlag(EPrimitiveProxyFlags::PerViewportUpdate | EPrimitiveProxyFlags::NeverCull))
		{
			return false;
		}

		if (Proxy.GetRenderPass() != ERenderPass::Opaque)
		{
			return false;
		}

		if (!Proxy.GetMeshBuffer() || Proxy.GetSectionDraws().empty())
		{
			return false;
		}

		return true;
	}

	bool HasAnyCasterInShadowView(const FScene& Scene, const FShadowViewData& ShadowView)
	{
		FConvexVolume ShadowFrustum = {};
		ShadowFrustum.UpdateFromMatrix(ShadowView.LightViewProj);

		for (FPrimitiveSceneProxy* Proxy : Scene.GetAllProxies())
		{
			if (!Proxy || !IsValidShadowCasterProxy(*Proxy))
			{
				continue;
			}

			if (ShadowFrustum.IntersectAABB(Proxy->GetCachedBounds()))
			{
				return true;
			}
		}

		return false;
	}
}

void FLocalShadowAllocationExecutor::ClearLocalShadowViewMetadata(FShadowViewData& View) const
{
	View.bAtlasAllocated = false;
	View.AtlasOffsetX = 0;
	View.AtlasOffsetY = 0;
	View.AtlasSizeX = 0;
	View.AtlasSizeY = 0;
	View.AtlasIndex = 0;
}

void FLocalShadowAllocationExecutor::ResetLocalShadowViewAllocationState(FSceneEnvironment& Environment) const
{
	for (uint32 i = 0; i < Environment.GetNumSpotLights(); ++i)
	{
		ClearLocalShadowViewMetadata(Environment.GetSpotLight(i).ShadowData.View);
	}

	for (uint32 i = 0; i < Environment.GetNumPointLights(); ++i)
	{
		FPointShadowData& PointShadow = Environment.GetPointLight(i).ShadowData;
		for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
		{
			ClearLocalShadowViewMetadata(PointShadow.View[FaceIndex]);
		}
	}
}

void FLocalShadowAllocationExecutor::ResetLocalShadowRequestAllocationState(FLocalShadowRequest& Request) const
{
	Request.bAllocated = false;
	Request.AtlasOffsetX = 0;
	Request.AtlasOffsetY = 0;
	Request.AtlasSizeX = 0;
	Request.AtlasSizeY = 0;
	Request.Resolution.AppliedResolution = 0;
}

void FLocalShadowAllocationExecutor::PruneInvalidOrEmptyPointFaceRequests(FSceneEnvironment& Environment, const FScene& Scene, TArray<FLocalShadowRequest>& InOutRequests) const
{
	for (FLocalShadowRequest& Request : InOutRequests)
	{
		if (Request.RequestType != ELocalShadowRequestType::PointFace)
		{
			continue;
		}

		if (Request.LightIndex >= Environment.GetNumPointLights() || Request.FaceIndex >= 6)
		{
			Request.bNeedsRender = false;
			continue;
		}

		const FPointShadowData& PointShadow = Environment.GetPointLight(Request.LightIndex).ShadowData;
		const FShadowViewData& FaceView = PointShadow.View[Request.FaceIndex];
		if (!HasAnyCasterInShadowView(Scene, FaceView))
		{
			Request.bNeedsRender = false;
		}
	}
}

uint32 FLocalShadowAllocationExecutor::ClampAndAlignResolution(uint32 Resolution, const FShadowResolutionPolicy& Policy) const
{
	Resolution = std::max(Resolution, Policy.MinResolution);
	Resolution = std::min(Resolution, Policy.MaxResolution);

	if (Policy.Alignment > 1)
	{
		const uint32 Align = Policy.Alignment;
		Resolution = ((Resolution + Align - 1) / Align) * Align;
		Resolution = std::min(Resolution, Policy.MaxResolution);
		Resolution = std::max(Resolution, Policy.MinResolution);
	}

	return Resolution;
}

uint32 FLocalShadowAllocationExecutor::ComputeNextLowerResolution(uint32 Resolution, const FShadowResolutionPolicy& Policy) const
{
	if (Resolution <= Policy.MinResolution)
	{
		return Policy.MinResolution;
	}

	const uint32 Step = (Policy.Alignment > 0) ? Policy.Alignment : 1u;
	uint32 NextResolution = (Resolution > Step) ? (Resolution - Step) : Policy.MinResolution;

	if (Policy.Alignment > 1)
	{
		const uint32 Align = Policy.Alignment;
		NextResolution = (NextResolution / Align) * Align;
		NextResolution = std::max(NextResolution, Policy.MinResolution);
	}

	if (NextResolution >= Resolution)
	{
		NextResolution = (Resolution > Step) ? (Resolution - Step) : Policy.MinResolution;
		if (Policy.Alignment > 1)
		{
			const uint32 Align = Policy.Alignment;
			NextResolution = (NextResolution / Align) * Align;
		}
		NextResolution = std::max(NextResolution, Policy.MinResolution);
	}

	return NextResolution;
}

bool FLocalShadowAllocationExecutor::TryAllocateAtlasRectForRequest(
	const FShadowResolutionPolicy& LocalResolutionPolicy,
	FLocalShadowAtlasAllocator& LocalAtlasAllocator,
	FLocalShadowRequest& Request,
	FAtlasResourceInfo& OutInfo) const
{
	if (Request.Resolution.AppliedResolution == 0)
	{
		return false;
	}

	uint32 AttemptResolution = ClampAndAlignResolution(Request.Resolution.AppliedResolution, LocalResolutionPolicy);
	while (AttemptResolution >= LocalResolutionPolicy.MinResolution)
	{
		OutInfo = LocalAtlasAllocator.Allocate(AttemptResolution, AttemptResolution);
		if (OutInfo.bAllocated)
		{
			return true;
		}

		if (AttemptResolution == LocalResolutionPolicy.MinResolution)
		{
			break;
		}

		uint32 NextResolution = ComputeNextLowerResolution(AttemptResolution, LocalResolutionPolicy);
		if (NextResolution >= AttemptResolution)
		{
			NextResolution = LocalResolutionPolicy.MinResolution;
		}

		if (NextResolution == AttemptResolution)
		{
			break;
		}

		AttemptResolution = NextResolution;
	}

	return false;
}

void FLocalShadowAllocationExecutor::ApplyAtlasAllocationToShadowView(FSceneEnvironment& Environment, FLocalShadowRequest& Request, const FAtlasResourceInfo& Info) const
{
	if (Request.RequestType == ELocalShadowRequestType::Spot)
	{
		if (Request.LightIndex >= Environment.GetNumSpotLights())
		{
			Request.bAllocated = false;
			Request.Resolution.AppliedResolution = 0;
			return;
		}

		FShadowViewData& View = Environment.GetSpotLight(Request.LightIndex).ShadowData.View;
		View.AtlasOffsetX = Info.OffsetX;
		View.AtlasOffsetY = Info.OffsetY;
		View.AtlasSizeX = Info.Width;
		View.AtlasSizeY = Info.Height;
		View.AtlasIndex = Info.Index;
		View.bAtlasAllocated = true;
		return;
	}

	if (Request.LightIndex >= Environment.GetNumPointLights() || Request.FaceIndex >= 6)
	{
		Request.bAllocated = false;
		Request.Resolution.AppliedResolution = 0;
		return;
	}

	FShadowViewData& View = Environment.GetPointLight(Request.LightIndex).ShadowData.View[Request.FaceIndex];
	View.AtlasOffsetX = Info.OffsetX;
	View.AtlasOffsetY = Info.OffsetY;
	View.AtlasSizeX = Info.Width;
	View.AtlasSizeY = Info.Height;
	View.AtlasIndex = Info.Index;
	View.bAtlasAllocated = true;
}

void FLocalShadowAllocationExecutor::TryAllocateLocalShadowRequest(
	FSceneEnvironment& Environment,
	const FShadowResolutionPolicy& LocalResolutionPolicy,
	FLocalShadowAtlasAllocator& LocalAtlasAllocator,
	FLocalShadowRequest& Request) const
{
	if (Request.Resolution.AppliedResolution == 0)
	{
		Request.bAllocated = false;
		return;
	}

	FAtlasResourceInfo Info = {};
	const bool bAllocated = TryAllocateAtlasRectForRequest(LocalResolutionPolicy, LocalAtlasAllocator, Request, Info);
	Request.bAllocated = bAllocated;
	Request.Resolution.AppliedResolution = bAllocated ? Info.Width : 0;

	if (!bAllocated)
	{
		Request.AtlasOffsetX = 0;
		Request.AtlasOffsetY = 0;
		Request.AtlasSizeX = 0;
		Request.AtlasSizeY = 0;
		return;
	}

	Request.AtlasOffsetX = Info.OffsetX;
	Request.AtlasOffsetY = Info.OffsetY;
	Request.AtlasSizeX = Info.Width;
	Request.AtlasSizeY = Info.Height;
	ApplyAtlasAllocationToShadowView(Environment, Request, Info);
}

void FLocalShadowAllocationExecutor::AllocateViews(
	FSceneEnvironment& Environment,
	const FScene& Scene,
	const FShadowResolutionPolicy& LocalResolutionPolicy,
	uint32 MaxLocalShadowViewsPerFrame,
	uint64 MaxLocalShadowAtlasAreaPerFrame,
	const FShadowAtlasResource& Atlas,
	FLocalShadowAtlasAllocator& LocalAtlasAllocator,
	TArray<FLocalShadowRequest>& InOutRequests) const
{
	ResetLocalShadowViewAllocationState(Environment);

	for (FLocalShadowRequest& Request : InOutRequests)
	{
		ResetLocalShadowRequestAllocationState(Request);
	}
	PruneInvalidOrEmptyPointFaceRequests(Environment, Scene, InOutRequests);

	const uint64 AtlasTotalArea = static_cast<uint64>(Atlas.Map.Width) * static_cast<uint64>(Atlas.Map.Height);
	const uint64 AreaBudgetLimit = (MaxLocalShadowAtlasAreaPerFrame > 0)
		? (std::min)(MaxLocalShadowAtlasAreaPerFrame, AtlasTotalArea)
		: AtlasTotalArea;
	// Planning stage uses a higher floor (256) for quality stability.
	// Actual allocation fallback floor still uses LocalResolutionPolicy.MinResolution (e.g. 64)
	// through TryAllocateAtlasRectForRequest().
	const uint32 PlanningMinResolution = ClampAndAlignResolution(256u, LocalResolutionPolicy);
	const uint64 PlanningMinResolutionArea = static_cast<uint64>(PlanningMinResolution) * static_cast<uint64>(PlanningMinResolution);
	const uint32 ViewBudgetLimit = (MaxLocalShadowViewsPerFrame > 0) ? MaxLocalShadowViewsPerFrame : UINT32_MAX;

	TArray<FLocalShadowRequest*> CandidateRequests;
	CandidateRequests.reserve(InOutRequests.size());
	for (FLocalShadowRequest& Request : InOutRequests)
	{
		if (!Request.bNeedsRender)
		{
			continue;
		}

		Request.Resolution.AppliedResolution = 0;
		Request.bAllocated = false;
		CandidateRequests.push_back(&Request);
	}

	uint32 AdmittedCount = static_cast<uint32>(CandidateRequests.size());
	if (AdmittedCount > ViewBudgetLimit)
	{
		AdmittedCount = ViewBudgetLimit;
	}

	if (PlanningMinResolutionArea > 0)
	{
		const uint64 AreaAdmitLimit = static_cast<uint64>(AreaBudgetLimit / PlanningMinResolutionArea);
		if (AdmittedCount > AreaAdmitLimit)
		{
			AdmittedCount = static_cast<uint32>(AreaAdmitLimit);
		}
	}

	for (uint32 RequestIndex = 0; RequestIndex < static_cast<uint32>(CandidateRequests.size()); ++RequestIndex)
	{
		FLocalShadowRequest& Request = *CandidateRequests[RequestIndex];
		if (RequestIndex < AdmittedCount)
		{
			Request.Resolution.AppliedResolution = PlanningMinResolution;
		}
		else
		{
			Request.bNeedsRender = true;
		}
	}

	uint64 PlannedUsedArea = static_cast<uint64>(AdmittedCount) * PlanningMinResolutionArea;
	uint64 RemainingUpgradeArea = (PlannedUsedArea < AreaBudgetLimit) ? (AreaBudgetLimit - PlannedUsedArea) : 0;
	if (AdmittedCount > 0 && RemainingUpgradeArea > 0)
	{
		for (uint32 RequestIndex = 0; RequestIndex < AdmittedCount; ++RequestIndex)
		{
			FLocalShadowRequest& Request = *CandidateRequests[RequestIndex];
			const uint32 TargetResolution = ClampAndAlignResolution(Request.Resolution.RequestedResolution, LocalResolutionPolicy);
			uint32 CurrentResolution = Request.Resolution.AppliedResolution;
			if (CurrentResolution == 0 || CurrentResolution >= TargetResolution)
			{
				continue;
			}

			while (CurrentResolution < TargetResolution)
			{
				uint32 NextResolution = CurrentResolution * 2;
				if (NextResolution > TargetResolution)
				{
					NextResolution = TargetResolution;
				}
				NextResolution = ClampAndAlignResolution(NextResolution, LocalResolutionPolicy);
				if (NextResolution <= CurrentResolution)
				{
					break;
				}

				const uint64 CurrentArea = static_cast<uint64>(CurrentResolution) * static_cast<uint64>(CurrentResolution);
				const uint64 NextArea = static_cast<uint64>(NextResolution) * static_cast<uint64>(NextResolution);
				const uint64 DeltaArea = NextArea - CurrentArea;
				if (DeltaArea > RemainingUpgradeArea)
				{
					break;
				}

				Request.Resolution.AppliedResolution = NextResolution;
				CurrentResolution = NextResolution;
				RemainingUpgradeArea -= DeltaArea;
			}
		}
	}

	TArray<FLocalShadowRequest*> AllocationRequests;
	AllocationRequests.reserve(InOutRequests.size());
	for (FLocalShadowRequest& Request : InOutRequests)
	{
		if (!Request.bNeedsRender || Request.Resolution.AppliedResolution == 0)
		{
			continue;
		}
		AllocationRequests.push_back(&Request);
	}

	std::stable_sort(AllocationRequests.begin(), AllocationRequests.end(),
		[](const FLocalShadowRequest* A, const FLocalShadowRequest* B)
		{
			if (A->Resolution.AppliedResolution != B->Resolution.AppliedResolution)
			{
				return A->Resolution.AppliedResolution > B->Resolution.AppliedResolution;
			}
			return A->RequestKey < B->RequestKey;
		});

	for (FLocalShadowRequest* RequestPtr : AllocationRequests)
	{
		TryAllocateLocalShadowRequest(Environment, LocalResolutionPolicy, LocalAtlasAllocator, *RequestPtr);
	}
}
