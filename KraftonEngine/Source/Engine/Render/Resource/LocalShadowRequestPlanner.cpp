#include "LocalShadowRequestPlanner.h"

#define NOMINMAX

#include "Render/Pipeline/FrameContext.h"
#include "Render/Proxy/SceneEnvironment.h"
#include "Render/Types/ShadowData.h"
#include <algorithm>
#include <cmath>

namespace
{
	float ComputeCameraDistanceTerm(const FFrameContext& Frame, const FVector& WorldCenter, float InfluenceRadius)
	{
		const float Dist = FVector::Distance(Frame.CameraPosition, WorldCenter);
		const float EffectiveRange = (std::max)((std::max)(Frame.FarClip, InfluenceRadius * 2.0f), 1.0f);
		float Normalized = 1.0f - (Dist / EffectiveRange);
		Normalized = (std::max)(0.0f, (std::min)(Normalized, 1.0f));
		return Normalized;
	}

	float ComputeViewForwardSign(const FFrameContext& Frame)
	{
		FVector CameraForward = Frame.CameraForward;
		if (CameraForward.Dot(CameraForward) <= 1e-8f)
		{
			return 1.0f;
		}
		CameraForward.Normalize();

		const FVector ProbePoint = Frame.CameraPosition + (CameraForward * 10.0f);
		const FVector ProbeView = Frame.View.TransformPositionWithW(ProbePoint);
		return (ProbeView.Z >= 0.0f) ? 1.0f : -1.0f;
	}

	bool IsLocalLightRelevantToCameraView(const FFrameContext& Frame, const FVector& LightPosition, float InfluenceRadius)
	{
		const float Radius = (std::max)(InfluenceRadius, 1.0f);
		const float Inflate = (std::max)(Radius * 0.1f, 16.0f);
		const float Extent = Radius + Inflate;
		const FVector MinP(LightPosition.X - Extent, LightPosition.Y - Extent, LightPosition.Z - Extent);
		const FVector MaxP(LightPosition.X + Extent, LightPosition.Y + Extent, LightPosition.Z + Extent);
		const FBoundingBox LightBounds(MinP, MaxP);
		if (!Frame.FrustumVolume.IntersectAABB(LightBounds))
		{
			return false;
		}

		const float ForwardSign = ComputeViewForwardSign(Frame);
		const FVector LightView = Frame.View.TransformPositionWithW(LightPosition);
		if ((LightView.Z * ForwardSign) < (-Radius))
		{
			return false;
		}

		const FMatrix ViewProj = Frame.View * Frame.Proj;
		const FVector4 CenterClip = ViewProj.TransformVector4(FVector4(LightPosition.X, LightPosition.Y, LightPosition.Z, 1.0f));
		if (std::fabs(CenterClip.W) <= 1e-5f)
		{
			return false;
		}

		const float CenterNdcX = CenterClip.X / CenterClip.W;
		const float CenterNdcY = CenterClip.Y / CenterClip.W;

		const FVector OffsetWorld = LightPosition + (Frame.CameraRight * Radius);
		const FVector4 OffsetClip = ViewProj.TransformVector4(FVector4(OffsetWorld.X, OffsetWorld.Y, OffsetWorld.Z, 1.0f));
		float RadiusNdc = 0.0f;
		if (std::fabs(OffsetClip.W) > 1e-5f)
		{
			const float OffsetNdcX = OffsetClip.X / OffsetClip.W;
			const float OffsetNdcY = OffsetClip.Y / OffsetClip.W;
			const float Dx = OffsetNdcX - CenterNdcX;
			const float Dy = OffsetNdcY - CenterNdcY;
			RadiusNdc = std::sqrt((Dx * Dx) + (Dy * Dy));
		}

		if (CenterNdcX < (-1.0f - RadiusNdc) || CenterNdcX > (1.0f + RadiusNdc)
			|| CenterNdcY < (-1.0f - RadiusNdc) || CenterNdcY > (1.0f + RadiusNdc))
		{
			return false;
		}

		return true;
	}
}

void FLocalShadowRequestPlanner::BuildRequests(
	FSceneEnvironment& Environment,
	const FFrameContext& Frame,
	const FShadowResolutionPolicy& LocalResolutionPolicy,
	TArray<FLocalShadowRequest>& OutRequests) const
{
	OutRequests.clear();

	const uint32 NumSpotLights = Environment.GetNumSpotLights();
	for (uint32 LightIndex = 0; LightIndex < NumSpotLights; ++LightIndex)
	{
		const FSpotLightParams& SpotLight = Environment.GetSpotLight(LightIndex);
		const FSpotShadowData& Shadow = SpotLight.ShadowData;
		const bool bForceRenderPriority = Shadow.bOverrideCameraWithLight;
		if (!Shadow.Settings.bCastShadows || (!bForceRenderPriority && !IsLocalLightRelevantToCameraView(Frame, SpotLight.Position, SpotLight.AttenuationRadius)))
		{
			continue;
		}

		FLocalShadowRequest Request = {};
		Request.RequestType = ELocalShadowRequestType::Spot;
		Request.LightIndex = LightIndex;
		Request.FaceIndex = 0;
		Request.RequestKey = MakeLocalShadowRequestKey(Request.RequestType, Request.LightIndex, Request.FaceIndex);
		Request.Resolution.RequestedResolution = ComputeRequestedResolution(Shadow.Settings, LocalResolutionPolicy);
		Request.Resolution.AppliedResolution = Request.Resolution.RequestedResolution;
		const float CameraDistanceTerm = ComputeCameraDistanceTerm(Frame, SpotLight.Position, SpotLight.AttenuationRadius);
		Request.Priority = ComputeLocalShadowPriority(
			Request.RequestType,
			SpotLight.Intensity,
			CameraDistanceTerm);
		Request.bForceRenderPriority = bForceRenderPriority;
		Request.bNeedsRender = true;
		Request.bAllocated = false;
		OutRequests.push_back(Request);
	}

	const uint32 NumPointLights = Environment.GetNumPointLights();
	for (uint32 LightIndex = 0; LightIndex < NumPointLights; ++LightIndex)
	{
		const FPointLightParams& PointLight = Environment.GetPointLight(LightIndex);
		const FPointShadowData& Shadow = PointLight.ShadowData;
		const bool bForceLightRenderPriority = Shadow.bOverrideCameraWithLight;
		if (!Shadow.Settings.bCastShadows || (!bForceLightRenderPriority && !IsLocalLightRelevantToCameraView(Frame, PointLight.Position, PointLight.AttenuationRadius)))
		{
			continue;
		}

		const uint32 Requested = ComputeRequestedResolution(Shadow.Settings, LocalResolutionPolicy);
		const uint32 ForcedFaceIndex = static_cast<uint32>((Shadow.PreviewViewIndex < 0) ? 0 : ((Shadow.PreviewViewIndex > 5) ? 5 : Shadow.PreviewViewIndex));
		for (uint32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
		{
			FLocalShadowRequest Request = {};
			Request.RequestType = ELocalShadowRequestType::PointFace;
			Request.LightIndex = LightIndex;
			Request.FaceIndex = FaceIndex;
			Request.RequestKey = MakeLocalShadowRequestKey(Request.RequestType, Request.LightIndex, Request.FaceIndex);
			Request.Resolution.RequestedResolution = Requested;
			Request.Resolution.AppliedResolution = Requested;
			const float CameraDistanceTerm = ComputeCameraDistanceTerm(Frame, PointLight.Position, PointLight.AttenuationRadius);
			Request.Priority = ComputeLocalShadowPriority(
				Request.RequestType,
				PointLight.Intensity,
				CameraDistanceTerm);
			Request.bForceRenderPriority = bForceLightRenderPriority && (FaceIndex == ForcedFaceIndex);
			Request.bNeedsRender = true;
			Request.bAllocated = false;
			OutRequests.push_back(Request);
		}
	}
}

void FLocalShadowRequestPlanner::SortRequests(TArray<FLocalShadowRequest>& InOutRequests) const
{
	auto CompareRequests = [](const FLocalShadowRequest& A, const FLocalShadowRequest& B)
	{
		if (A.bForceRenderPriority != B.bForceRenderPriority)
		{
			return A.bForceRenderPriority;
		}

		if (A.Priority != B.Priority)
		{
			return A.Priority > B.Priority;
		}

		if (A.Resolution.RequestedResolution != B.Resolution.RequestedResolution)
		{
			return A.Resolution.RequestedResolution > B.Resolution.RequestedResolution;
		}

		if (A.LightIndex != B.LightIndex)
		{
			return A.LightIndex < B.LightIndex;
		}

		return A.FaceIndex < B.FaceIndex;
	};

	std::stable_sort(InOutRequests.begin(), InOutRequests.end(), CompareRequests);
}

uint32 FLocalShadowRequestPlanner::ComputeRequestedResolution(const FLightShadowSettings& Settings, const FShadowResolutionPolicy& Policy) const
{
	const float Scale = (Settings.ShadowResolutionScale <= 1e-4f) ? 1.0f : Settings.ShadowResolutionScale;
	const uint32 Resolution = static_cast<uint32>(Scale * static_cast<float>(Policy.BaseResolution));
	return ClampAndAlignResolution(Resolution, Policy);
}

uint32 FLocalShadowRequestPlanner::ClampAndAlignResolution(uint32 Resolution, const FShadowResolutionPolicy& Policy) const
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

uint64 FLocalShadowRequestPlanner::MakeLocalShadowRequestKey(ELocalShadowRequestType RequestType, uint32 LightIndex, uint32 FaceIndex) const
{
	const uint64 TypePart = static_cast<uint64>(RequestType) & 0xFFull;
	const uint64 LightPart = static_cast<uint64>(LightIndex) & 0x00FFFFFFull;
	const uint64 FacePart = static_cast<uint64>(FaceIndex) & 0xFFull;
	return (TypePart << 56) | (LightPart << 8) | FacePart;
}

float FLocalShadowRequestPlanner::ComputeLocalShadowPriority(
	ELocalShadowRequestType RequestType,
	float LightIntensity,
	float CameraDistanceTerm) const
{
	const float SafeIntensity = std::max(0.0f, LightIntensity);
	float IntensityTerm = SafeIntensity / 8.0f;
	IntensityTerm = std::max(0.0f, std::min(IntensityTerm, 1.5f));

	const float DistanceTerm = std::max(0.0f, std::min(CameraDistanceTerm, 1.0f));
	const float TypeBase = (RequestType == ELocalShadowRequestType::Spot) ? 1.0f : 0.8f;
	
	return
		(0.80f * DistanceTerm) +
		(0.18f * IntensityTerm) +
		(0.02f * TypeBase);
}
