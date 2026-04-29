#include "ShadowRenderer.h"

#include "FrameContext.h"
#include "ShadowPassContext.h"
#include "Render/Culling/ConvexVolume.h"
#include "Render/Proxy/FScene.h"
#include "Render/Proxy/PrimitiveSceneProxy.h"
#include "Render/Pipeline/RenderConstants.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Resource/ShaderManager.h"
#include "Profiling/Stats.h"

#include <algorithm>
#include <cmath>

namespace
{
	struct FShadowFilterConstants
	{
		float InvTextureSize[2] = { 0.0f, 0.0f };
		float BlurDirection[2] = { 0.0f, 0.0f };
		float RectMin[2] = { 0.0f, 0.0f };
		float RectSize[2] = { 1.0f, 1.0f };
		uint32 SourceKind = 0; // 0: local atlas(t21), 1: directional array(t22)
		uint32 SourceSlice = 0;
		uint32 _Pad0 = 0;
		uint32 _Pad1 = 0;
	};

	constexpr float GPSMVirtualSlideBack = 10.0f;

	//	Shadow Map??洹몃┛ ??Main Viewport???곹깭瑜?蹂듦뎄??(?ㅼ쓬 Pass瑜??꾪빐)
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

	uint32 GetActiveDirectionalCascadeCount(const FShadowRuntimeOptions& ShadowOptions, uint32 MaxCascadeCount)
	{
		if (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::Single)
		{
			return (MaxCascadeCount > 0) ? 1u : 0u;
		}

		return MaxCascadeCount;
	}

	uint32 GetDirectionalShadowSliceIndex(const FShadowRuntimeOptions& ShadowOptions, uint32 CascadeIndex)
	{
		if (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::Single)
		{
			return 0u; // PSM slice
		}

		return CascadeIndex + 1u; // CSM slices
	}

	bool HasValidDirectionalViews(const FDirectionalShadowData& ShadowData, int32 ActiveCascadeCount)
	{
		for (int32 Index = 0; Index < ActiveCascadeCount; ++Index)
		{
			const FShadowViewData& View = ShadowData.View[Index];
			if (View.LightView.IsIdentity() || View.LightProj.IsIdentity() || View.LightViewProj.IsIdentity())
			{
				return false;
			}
		}
		return true;
	}

	bool IsShadowCasterReceiverProxy(const FPrimitiveSceneProxy& Proxy)
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

		return Proxy.GetMeshBuffer() && !Proxy.GetSectionDraws().empty() && Proxy.GetCachedBounds().IsValid();
	}

	FBoundingBox BuildShadowReceiverBounds(const FScene& Scene, const FFrameContext& MainFrame)
	{
		FBoundingBox Bounds;

		for (const FPrimitiveSceneProxy* Proxy : Scene.GetAllProxies())
		{
			if (!Proxy || !IsShadowCasterReceiverProxy(*Proxy))
			{
				continue;
			}

			const FBoundingBox& ProxyBounds = Proxy->GetCachedBounds();
			if (MainFrame.FrustumVolume.ClassifyAABB(ProxyBounds) == EAABBFrustumClassify::Outside)
			{
				continue;
			}

			Bounds.Expand(ProxyBounds.Min);
			Bounds.Expand(ProxyBounds.Max);
		}

		return Bounds;
	}

	void AppendBoundingBoxCorners(TArray<FVector4>& OutPoints, const FBoundingBox& Bounds, const FMatrix& Transform)
	{
		FVector Corners[8];
		Bounds.GetCorners(Corners);

		for (const FVector& Corner : Corners)
		{
			FVector4 ProjectedCorner = Transform.TransformVector4(FVector4(Corner, 1.0f));
			if (std::abs(ProjectedCorner.W) > 1e-6f)
			{
				const float InvW = 1.0f / ProjectedCorner.W;
				ProjectedCorner.X *= InvW;
				ProjectedCorner.Y *= InvW;
				ProjectedCorner.Z *= InvW;
				ProjectedCorner.W = 1.0f;
			}

			OutPoints.push_back(ProjectedCorner);
		}
	}

	void AppendIntersectingSubBoxes(TArray<FVector4>& OutPoints, const FBoundingBox& Bounds,
		const FFrameContext& MainFrame, const FMatrix& Transform)
	{
		const FVector Size = Bounds.Max - Bounds.Min;
		const int32 SplitX = Size.X > 1.0f ? 4 : 1;
		const int32 SplitY = Size.Y > 1.0f ? 4 : 1;
		const int32 SplitZ = Size.Z > 1.0f ? 4 : 1;

		for (int32 z = 0; z < SplitZ; ++z)
		{
			for (int32 y = 0; y < SplitY; ++y)
			{
				for (int32 x = 0; x < SplitX; ++x)
				{
					const FVector Min(
						Bounds.Min.X + Size.X * (static_cast<float>(x) / static_cast<float>(SplitX)),
						Bounds.Min.Y + Size.Y * (static_cast<float>(y) / static_cast<float>(SplitY)),
						Bounds.Min.Z + Size.Z * (static_cast<float>(z) / static_cast<float>(SplitZ)));
					const FVector Max(
						Bounds.Min.X + Size.X * (static_cast<float>(x + 1) / static_cast<float>(SplitX)),
						Bounds.Min.Y + Size.Y * (static_cast<float>(y + 1) / static_cast<float>(SplitY)),
						Bounds.Min.Z + Size.Z * (static_cast<float>(z + 1) / static_cast<float>(SplitZ)));

					const FBoundingBox SubBox(Min, Max);
					if (MainFrame.FrustumVolume.ClassifyAABB(SubBox) != EAABBFrustumClassify::Outside)
					{
						AppendBoundingBoxCorners(OutPoints, SubBox, Transform);
					}
				}
			}
		}
	}

	TArray<FVector4> BuildPostPerspectiveReceiverPoints(const FScene& Scene, const FFrameContext& MainFrame,
		const FMatrix& VirtualCameraViewProjection)
	{
		TArray<FVector4> Points;
		Points.reserve(Scene.GetProxyCount() * 8);

		for (const FPrimitiveSceneProxy* Proxy : Scene.GetAllProxies())
		{
			if (!Proxy || !IsShadowCasterReceiverProxy(*Proxy))
			{
				continue;
			}

			const FBoundingBox& Bounds = Proxy->GetCachedBounds();
			const EAABBFrustumClassify Classify = MainFrame.FrustumVolume.ClassifyAABB(Bounds);
			if (Classify == EAABBFrustumClassify::Outside)
			{
				continue;
			}

			if (Classify == EAABBFrustumClassify::Contains)
			{
				AppendBoundingBoxCorners(Points, Bounds, VirtualCameraViewProjection);
			}
			else
			{
				AppendIntersectingSubBoxes(Points, Bounds, MainFrame, VirtualCameraViewProjection);
			}
		}

		if (Points.empty())
		{
			Points = {
				FVector4(-1.0f, -1.0f, 0.0f, 1.0f),
				FVector4( 1.0f, -1.0f, 0.0f, 1.0f),
				FVector4(-1.0f,  1.0f, 0.0f, 1.0f),
				FVector4( 1.0f,  1.0f, 0.0f, 1.0f),
				FVector4(-1.0f, -1.0f, 1.0f, 1.0f),
				FVector4( 1.0f, -1.0f, 1.0f, 1.0f),
				FVector4(-1.0f,  1.0f, 1.0f, 1.0f),
				FVector4( 1.0f,  1.0f, 1.0f, 1.0f),
			};
		}

		return Points;
	}

	float ComputeCameraFitNear(const FFrameContext& MainFrame, const FBoundingBox& ReceiverBounds)
	{
		if (!ReceiverBounds.IsValid())
		{
			return MainFrame.NearClip;
		}

		FVector Corners[8];
		ReceiverBounds.GetCorners(Corners);

		float MinZ = FLT_MAX;
		for (const FVector& Corner : Corners)
		{
			const FVector ToCorner = Corner - MainFrame.CameraPosition;
			MinZ = std::min(MinZ, MainFrame.CameraForward.Dot(ToCorner));
		}

		const float MaxNear = std::max(MainFrame.NearClip, MainFrame.FarClip - 1.0f);
		if (MinZ > MainFrame.NearClip && MinZ < MaxNear)
		{
			return MinZ;
		}

		return MainFrame.NearClip;
	}

	FVector ComputeCenter(const TArray<FVector4>& Points)
	{
		FVector Center;
		for (const FVector4& Point : Points)
		{
			Center += FVector(Point.X, Point.Y, Point.Z);
		}

		return Center / static_cast<float>(Points.size());
	}

	float ComputeBoundingSphereRadius(const TArray<FVector4>& Points, const FVector& Center)
	{
		float Radius = 0.0f;
		for (const FVector4& Point : Points)
		{
			const FVector Delta = FVector(Point.X, Point.Y, Point.Z) - Center;
			Radius = std::max(Radius, Delta.Length());
		}

		return std::max(Radius, 0.001f);
	}

	FMatrix MakeViewFromLocationAndTarget(const FVector& Location, const FVector& Target)
	{
		FVector Forward = (Target - Location).Normalized();
		if (Forward.Length() <= 1e-4f)
		{
			Forward = FVector(0.0f, 0.0f, 1.0f);
		}

		FVector UpCandidate(0.0f, 1.0f, 0.0f);
		if (std::abs(Forward.Dot(UpCandidate)) > 0.99f)
		{
			UpCandidate = FVector(1.0f, 0.0f, 0.0f);
		}

		const FVector Right = UpCandidate.Cross(Forward).Normalized();
		const FVector Up = Forward.Cross(Right).Normalized();
		return FMatrix::MakeViewMatrix(Forward, Right, Up, Location);
	}

	FMatrix MakeOrthoFitToPoints(const FMatrix& View, const TArray<FVector4>& Points)
	{
		float MinX = FLT_MAX, MaxX = -FLT_MAX;
		float MinY = FLT_MAX, MaxY = -FLT_MAX;
		float MinZ = FLT_MAX, MaxZ = -FLT_MAX;

		for (const FVector4& Point : Points)
		{
			const FVector4 ViewPoint = View.TransformVector4(Point);
			MinX = std::min(MinX, ViewPoint.X); MaxX = std::max(MaxX, ViewPoint.X);
			MinY = std::min(MinY, ViewPoint.Y); MaxY = std::max(MaxY, ViewPoint.Y);
			MinZ = std::min(MinZ, ViewPoint.Z); MaxZ = std::max(MaxZ, ViewPoint.Z);
		}

		constexpr float MinExtent = 0.001f;
		if (MaxX - MinX < MinExtent)
		{
			const float Center = (MinX + MaxX) * 0.5f;
			MinX = Center - MinExtent;
			MaxX = Center + MinExtent;
		}
		if (MaxY - MinY < MinExtent)
		{
			const float Center = (MinY + MaxY) * 0.5f;
			MinY = Center - MinExtent;
			MaxY = Center + MinExtent;
		}
		if (MaxZ - MinZ < MinExtent)
		{
			const float Center = (MinZ + MaxZ) * 0.5f;
			MinZ = Center - MinExtent;
			MaxZ = Center + MinExtent;
		}

		return FMatrix::MakeOrtho(MinX, MaxX, MinY, MaxY, MinZ, MaxZ);
	}

	struct FPSMBoundingCone
	{
		FMatrix View = FMatrix::Identity;
		float FovX = 0.0f;
		float FovY = 0.0f;
		float NearZ = 0.001f;
		float FarZ = 1.0f;
		bool bValid = false;
	};

	FPSMBoundingCone BuildBoundingCone(const TArray<FVector4>& Points, const FVector& Apex)
	{
		FPSMBoundingCone Cone;
		if (Points.empty())
		{
			return Cone;
		}

		const FVector Center = ComputeCenter(Points);
		FVector Direction = Center - Apex;
		if (Direction.Length() <= 1e-4f)
		{
			Direction = FVector(0.0f, 0.0f, 1.0f);
		}
		Direction.Normalize();

		Cone.View = MakeViewFromLocationAndTarget(Apex, Apex + Direction);
		float MaxAbsXOverZ = 0.0f;
		float MaxAbsYOverZ = 0.0f;
		Cone.NearZ = FLT_MAX;
		Cone.FarZ = -FLT_MAX;

		for (const FVector4& Point : Points)
		{
			const FVector4 ViewPoint = Cone.View.TransformVector4(Point);
			if (ViewPoint.Z <= 1e-4f)
			{
				continue;
			}

			MaxAbsXOverZ = std::max(MaxAbsXOverZ, std::abs(ViewPoint.X / ViewPoint.Z));
			MaxAbsYOverZ = std::max(MaxAbsYOverZ, std::abs(ViewPoint.Y / ViewPoint.Z));
			Cone.NearZ = std::min(Cone.NearZ, ViewPoint.Z);
			Cone.FarZ = std::max(Cone.FarZ, ViewPoint.Z);
		}

		if (MaxAbsXOverZ <= 1e-6f || MaxAbsYOverZ <= 1e-6f || Cone.NearZ == FLT_MAX || Cone.FarZ <= Cone.NearZ)
		{
			return Cone;
		}

		Cone.FovX = std::atan(MaxAbsXOverZ);
		Cone.FovY = std::atan(MaxAbsYOverZ);
		Cone.bValid = true;
		return Cone;
	}

	FMatrix MakeReversedInversePerspectiveFitToPoints(const FMatrix& View, const TArray<FVector4>& Points)
	{
		float MaxAbsXOverZ = 0.0f;
		float MaxAbsYOverZ = 0.0f;
		float MinAbsZ = FLT_MAX;

		for (const FVector4& Point : Points)
		{
			const FVector4 ViewPoint = View.TransformVector4(Point);
			const float AbsZ = std::abs(ViewPoint.Z);
			if (AbsZ <= 1e-4f)
			{
				continue;
			}

			MaxAbsXOverZ = std::max(MaxAbsXOverZ, std::abs(ViewPoint.X / ViewPoint.Z));
			MaxAbsYOverZ = std::max(MaxAbsYOverZ, std::abs(ViewPoint.Y / ViewPoint.Z));
			MinAbsZ = std::min(MinAbsZ, AbsZ);
		}

		if (MaxAbsXOverZ <= 1e-6f || MaxAbsYOverZ <= 1e-6f || MinAbsZ == FLT_MAX)
		{
			return MakeOrthoFitToPoints(View, Points);
		}

		const float ScaleX = 1.0f / MaxAbsXOverZ;
		const float ScaleY = 1.0f / MaxAbsYOverZ;
		const float A = std::max(0.001f, MinAbsZ * 0.3f);

		// Reversed-Z variant of the PracticalPSM inverse projection.
		// z = -A maps to 1, both infinities map to 0.5, and z = +A maps to 0.
		return FMatrix(
			ScaleX, 0.0f,   0.0f,      0.0f,
			0.0f,   ScaleY, 0.0f,      0.0f,
			0.0f,   0.0f,   0.5f,      1.0f,
			0.0f,   0.0f,  -0.5f * A,  0.0f
		);
	}

	void BuildPSMViewProjection(const FFrameContext& MainFrame, const FScene& Scene, const FVector& LightDirection,
		FMatrix& OutVirtualCameraViewProjection, FMatrix& OutPSMLightView, FMatrix& OutPSMLightProj)
	{
		const FBoundingBox ReceiverBounds = BuildShadowReceiverBounds(Scene, MainFrame);
		const float CameraFitNear = ComputeCameraFitNear(MainFrame, ReceiverBounds);

		const float AspectRatio = (MainFrame.ViewportHeight > 1.0f)
			? MainFrame.ViewportWidth / MainFrame.ViewportHeight
			: 1.0f;
		const float FovY = 2.0f * std::atan(1.0f / MainFrame.Proj.M[1][1]);
		const float VirtualSlideBack = MainFrame.bIsOrtho ? 0.0f : GPSMVirtualSlideBack;
		const float VirtualNear = std::max(0.001f, CameraFitNear + VirtualSlideBack);
		const float VirtualFar = std::max(VirtualNear + 1.0f, MainFrame.FarClip + VirtualSlideBack);

		const FMatrix VirtualCameraView = MainFrame.bIsOrtho
			? MainFrame.View
			: FMatrix::MakeViewMatrix(
				MainFrame.CameraForward.Normalized(),
				MainFrame.CameraRight.Normalized(),
				MainFrame.CameraUp.Normalized(),
				MainFrame.CameraPosition - MainFrame.CameraForward.Normalized() * VirtualSlideBack);
		const FMatrix VirtualCameraProj = MainFrame.bIsOrtho
			? MainFrame.Proj
			: FMatrix::MakeProjectionMatrix(FovY, VirtualNear, VirtualFar, AspectRatio);

		OutVirtualCameraViewProjection = VirtualCameraView * VirtualCameraProj;

		const TArray<FVector4> PostPerspectiveReceiverPoints =
			BuildPostPerspectiveReceiverPoints(Scene, MainFrame, OutVirtualCameraViewProjection);
		const FVector PPCenter = ComputeCenter(PostPerspectiveReceiverPoints);
		const float PPRadius = ComputeBoundingSphereRadius(PostPerspectiveReceiverPoints, PPCenter);

		const FVector LightToSceneDirection = LightDirection.Normalized();
		const FVector SceneToLightDirection = -LightToSceneDirection;
		const FVector4 EyeLightDirection = VirtualCameraView.TransformVector4(FVector4(SceneToLightDirection, 0.0f));
		const FVector4 LightPP = VirtualCameraProj.TransformVector4(EyeLightDirection);
		const bool bLightAtInfinity = std::abs(LightPP.W) <= 0.001f;
		const bool bLightBehindEye = LightPP.W < 0.0f;

		if (bLightAtInfinity)
		{
			FVector LightDirectionPP(LightPP.X, LightPP.Y, LightPP.Z);
			if (LightDirectionPP.Length() <= 1e-4f)
			{
				LightDirectionPP = FVector(0.0f, 0.0f, 1.0f);
			}
			LightDirectionPP.Normalize();

			const FVector LightPositionPP = PPCenter + LightDirectionPP * (PPRadius * 2.0f);
			OutPSMLightView = MakeViewFromLocationAndTarget(LightPositionPP, PPCenter);
			OutPSMLightProj = MakeOrthoFitToPoints(OutPSMLightView, PostPerspectiveReceiverPoints);
			return;
		}

		const float InvLightW = 1.0f / LightPP.W;
		const FVector LightPositionPP(LightPP.X * InvLightW, LightPP.Y * InvLightW, LightPP.Z * InvLightW);
		const float DistanceToCenter = FVector::Distance(LightPositionPP, PPCenter);
		const FPSMBoundingCone ReceiverCone = BuildBoundingCone(PostPerspectiveReceiverPoints, LightPositionPP);

		OutPSMLightView = ReceiverCone.bValid
			? ReceiverCone.View
			: MakeViewFromLocationAndTarget(LightPositionPP, PPCenter);

		if (bLightBehindEye || DistanceToCenter <= PPRadius * 1.05f)
		{
			OutPSMLightProj = bLightBehindEye
				? MakeReversedInversePerspectiveFitToPoints(OutPSMLightView, PostPerspectiveReceiverPoints)
				: MakeOrthoFitToPoints(OutPSMLightView, PostPerspectiveReceiverPoints);
			return;
		}

		const float FovPP = ReceiverCone.bValid
			? std::min(2.8f, ReceiverCone.FovY * 2.0f)
			: std::min(2.8f, 2.0f * std::atan(PPRadius / DistanceToCenter));
		const float AspectPP = ReceiverCone.bValid
			? std::max(0.001f, std::tan(ReceiverCone.FovX) / std::max(std::tan(ReceiverCone.FovY), 0.001f))
			: 1.0f;
		const float NearPP = ReceiverCone.bValid
			? std::max(0.001f, ReceiverCone.NearZ * 0.6f)
			: std::max(0.001f, DistanceToCenter - PPRadius);
		const float FarPP = ReceiverCone.bValid
			? std::max(NearPP + 0.001f, ReceiverCone.FarZ)
			: std::max(NearPP + 0.001f, DistanceToCenter + PPRadius);
		OutPSMLightProj = FMatrix::MakeProjectionMatrix(FovPP, NearPP, FarPP, AspectPP);
	}

	void UpdateCascades(FDirectionalShadowData& ShadowData, const FVector& LightDirection, const FFrameContext& MainFrame, const FShadowRuntimeOptions& ShadowOptions)
	{
		const bool bSingleMode = (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::Single);
		const int32 ActiveCascadeCount = bSingleMode ? 1 : ShadowData.NUM_CASCADES;

		if (ShadowData.bOverrideCameraWithLight && HasValidDirectionalViews(ShadowData, ActiveCascadeCount))
		{
			return;
		}

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

		if (bSingleMode)
		{
			ShadowData.CasCadeEnds[1] = MainFrame.FarClip;
			for (int i = 2; i <= ShadowData.NUM_CASCADES; ++i)
			{
				ShadowData.CasCadeEnds[i] = MainFrame.FarClip;
			}
		}

		for (int i = 0; i < ActiveCascadeCount; i++)
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
			
			float WorldUnitsPerTexelX = (MaxX - MinX) / Resolution;
			float RangeX = MaxX - MinX;
			MinX = std::floor(MinX / WorldUnitsPerTexelX) * WorldUnitsPerTexelX;
			MaxX = MinX + RangeX;

			float WorldUnitsPerTexelY = (MaxY - MinY) / Resolution;
			float RangeY = MaxY - MinY;
			MinY = std::floor(MinY / WorldUnitsPerTexelY) * WorldUnitsPerTexelY;
			MaxY = MinY + RangeY;

			FShadowViewData& View = ShadowData.View[i];
			View.LightView = LightView;
			View.LightProj = FMatrix::MakeOrtho(MinX, MaxX, MinY, MaxY, MinZ - 20.0f, MaxZ);
			View.LightViewProj = View.LightView * View.LightProj;

			FVector4 VClip = MainFrame.Proj.TransformVector4(FVector4(0.0f, 0.0f, Zf, 1.0f));
			ShadowData.CascadeEndClipZ[i] = VClip.Z / VClip.W;
		}

		for (int i = ActiveCascadeCount; i < ShadowData.NUM_CASCADES; ++i)
		{
			ShadowData.CascadeEndClipZ[i] = ShadowData.CascadeEndClipZ[ActiveCascadeCount - 1];
		}
	}
}

void FShadowRenderer::Create(ID3D11Device* Device, ID3D11DeviceContext* DeviceContext)
{
	Builder.Create(Device, DeviceContext);
	ShadowFilterConstantBuffer.Create(Device, sizeof(FShadowFilterConstants));
}

void FShadowRenderer::Release()
{
	ShadowFilterConstantBuffer.Release();
	Builder.Release();
}

void FShadowRenderer::RenderShadows(FD3DDevice& Device, FSystemResources& Resources, FScene& Scene,
	const FFrameContext& MainFrame)
{
	if (ShadowOptions.bSkipShadowPassInUnlit && MainFrame.RenderOptions.ViewMode == EViewMode::Unlit)
	{
		return;
	}

	FSceneEnvironment& Env = Scene.GetEnvironment();
	ShadowDrawCallCountThisFrame = 0;
	Resources.ShadowResourceManager.AllocateLocalShadowViews(Env, Scene);
	uint32 InvalidViewCount = 0;
	uint32 SubmittedShadowViewCount = 0;

	if (Scene.GetEnvironment().HasGlobalDirectionalLight())
	{
		if (Env.GetGlobalDirectionalLightParams().ShadowData.Settings.bCastShadows)
		{
			const FShadowRenderResult Result = RenderDirectionalShadow(Device, Resources, Env.GetGlobalDirectionalLightParams(), Scene, MainFrame);
			SubmittedShadowViewCount += Result.SubmittedViewCount;
			InvalidViewCount += Result.InvalidViewCount;
		}
	}

	for (uint32 i = 0; i < Env.GetNumPointLights(); i++)
	{
		// Draw To Shadow Atlas | (Inside) RenderShadowView Decide if Light will drawn to Atlas or just single DSV
		const FShadowRenderResult Result = RenderPointShadow(Device, Resources, Env.GetPointLight(i), Scene);
		SubmittedShadowViewCount += Result.SubmittedViewCount;
		InvalidViewCount += Result.InvalidViewCount;
	}

	for (uint32 i = 0; i < Env.GetNumSpotLights(); i++)
	{
		// Draw To Shadow Atlas | (Inside) RenderShadowView Decide if Light will drawn to Atlas or just single DSV
		const FShadowRenderResult Result = RenderSpotShadow(Device, Resources, Env.GetSpotLight(i), Scene);
		SubmittedShadowViewCount += Result.SubmittedViewCount;
		InvalidViewCount += Result.InvalidViewCount;
	}

	//	RS??RT ?먯긽??蹂듦뎄
	RenderDirectionalMomentBlurPass(Device, Resources, Env);
	RenderAtlasMomentBlurPass(Device, Resources, Env);
	RestoreMainViewport(Device, MainFrame);
	(void)SubmittedShadowViewCount;
	(void)InvalidViewCount;
}

FShadowRenderer::FShadowRenderResult FShadowRenderer::RenderDirectionalShadow(FD3DDevice& Device, FSystemResources& Resources, FGlobalDirectionalLightParams& Light, FScene& Scene, const FFrameContext MainFrame)
{
	FShadowRenderResult Result = {};

	FMatrix PSMMainViewProjection = FMatrix::Identity;
	FMatrix PSMLightView = FMatrix::Identity;
	FMatrix PSMLightProj = FMatrix::Identity;
	BuildPSMViewProjection(MainFrame, Scene, Light.Direction, PSMMainViewProjection, PSMLightView, PSMLightProj);

	const FMatrix PSMLightViewProj = PSMLightView * PSMLightProj;
	Light.ShadowData.MainViewProjection = PSMMainViewProjection;
	Light.ShadowData.PSMView.LightView = PSMLightView;
	Light.ShadowData.PSMView.LightProj = PSMLightProj;
	Light.ShadowData.PSMView.LightViewProj = PSMLightViewProj;

	UpdateCascades(Light.ShadowData, Light.Direction, MainFrame, ShadowOptions);
	const FDirectionalShadowArray& DirectionalArray = Resources.ShadowResourceManager.GetShadowArray();

	if (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::Single)
	{
		FShadowMapResource& DepthMap = Light.ShadowData.PSMView.DepthMap;
		DepthMap.DSV = DirectionalArray.DSVs[0];
		DepthMap.DepthTexture = DirectionalArray.Texture;

		if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM)
		{
			DepthMap.Texture = DirectionalArray.MomentTexture;
			DepthMap.RTV = DirectionalArray.MomentRTVs[0];
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

		if (!RenderShadowView(Device, Resources, Light.ShadowData.PSMView, Scene, true, &Light.ShadowData.MainViewProjection))
		{
			++Result.InvalidViewCount;
			return Result;
		}

		++Result.SubmittedViewCount;
		return Result;
	}

	const uint32 ActiveCascadeCount = GetActiveDirectionalCascadeCount(ShadowOptions, Light.ShadowData.NUM_CASCADES);
	for (uint32 i = 0; i < ActiveCascadeCount; ++i)
	{
		const uint32 SliceIndex = GetDirectionalShadowSliceIndex(ShadowOptions, i);
		FShadowMapResource& DepthMap = Light.ShadowData.View[i].DepthMap;
		DepthMap.DSV = DirectionalArray.DSVs[SliceIndex];
		DepthMap.DepthTexture = DirectionalArray.Texture;

		if (ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM)
		{
			DepthMap.Texture = DirectionalArray.MomentTexture;
			DepthMap.RTV = DirectionalArray.MomentRTVs[SliceIndex];
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

		if (!RenderShadowView(Device, Resources, Light.ShadowData.View[i], Scene))
		{
			++Result.InvalidViewCount;
			continue;
		}

		++Result.SubmittedViewCount;
	}

	return Result;
}


FShadowRenderer::FShadowRenderResult FShadowRenderer::RenderPointShadow(FD3DDevice& Device, FSystemResources& Resources, FPointLightParams& Light, FScene& Scene)
{
	FShadowRenderResult Result = {};
	if (!Light.ShadowData.Settings.bCastShadows)
	{
		return Result;
	}

	for (int32 i = 0; i < 6; i++)
	{
		if (!IsShadowViewReady(Light.ShadowData.View[i], ShadowOptions))
		{
			++Result.InvalidViewCount;
			continue;
		}

		if (!RenderShadowView(Device, Resources, Light.ShadowData.View[i], Scene))
		{
			++Result.InvalidViewCount;
			continue;
		}

		++Result.SubmittedViewCount;
	}

	return Result;
}

FShadowRenderer::FShadowRenderResult FShadowRenderer::RenderSpotShadow(FD3DDevice& Device, FSystemResources& Resources, FSpotLightParams& Light, FScene& Scene)
{
	FShadowRenderResult Result = {};
	if (!Light.ShadowData.Settings.bCastShadows)
	{
		return Result;
	}

	if (!RenderShadowView(Device, Resources, Light.ShadowData.View, Scene))
	{
		++Result.InvalidViewCount;
		return Result;
	}

	++Result.SubmittedViewCount;
	return Result;
}

//	媛곴컖??View Rendering
bool FShadowRenderer::RenderShadowView(FD3DDevice& Device, FSystemResources& Resources, FShadowViewData& View, FScene& Scene,
	bool bUsePSMShader, const FMatrix* PSMMainViewProjection)
{
	//	Preparing for Rendering
	ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();
	UnbindShadowReadResourcesForWrite(Device);

	FShadowPassContext PassContext = {};
	PassContext.View = View.LightView;
	PassContext.Proj = View.LightProj;
	PassContext.ViewProj = View.LightViewProj;

	FShadowAtlasResource& Atlas = Resources.ShadowResourceManager.GetAtlas();
	const bool bUseAtlas = View.bAtlasAllocated && IsShadowAtlasReady(Atlas, ShadowOptions);

	if (View.bAtlasAllocated && !bUseAtlas)
	{
		return false;
	}

	if (!bUseAtlas && !IsShadowViewReady(View, ShadowOptions))
	{
		return false;
	}

	//?꾪??쇱뒪瑜??곕깘 留덈깘???곕씪 Viewport媛 諛붾?
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
	Resources.SetRasterizerState(Device, bUsePSMShader ? ERasterizerState::SolidNoCull : ERasterizerState::SolidBackCull);

	Builder.BeginBuild(Scene.GetProxyCount());
	Builder.SetCullingViewProjection(PassContext.ViewProj, true);
	Builder.BuildCommands(Scene);

	BindShadowFrameConstants(Device, Resources, PassContext);

	if (bUsePSMShader && PSMMainViewProjection)
	{
		FPSMShadowConstants PSMData = {};
		PSMData.MainViewProjection = PSMMainViewProjection->ConvertToPOD();
		PSMData.LightViewProj = View.LightViewProj.ConvertToPOD();
		Resources.PSMShadowBuffer.Update(DeviceContext, &PSMData, sizeof(FPSMShadowConstants));

		ID3D11Buffer* PSMBuffer = Resources.PSMShadowBuffer.GetBuffer();
		DeviceContext->VSSetConstantBuffers(ECBSlot::PerShader0, 1, &PSMBuffer);
	}

	if ((ShadowOptions.ShadowFilterMode == EShadowFilterMode::VSM || ShadowOptions.ShadowFilterMode == EShadowFilterMode::ESM))
	{
		FShaderManager::Get().GetOrCreate(bUsePSMShader ? EShaderPath::PSMMomentShadowMap : EShaderPath::MomentShadowMap)->Bind(DeviceContext);
	}
	else
	{
		FShaderManager::Get().GetOrCreate(bUsePSMShader ? EShaderPath::PSMCommonShadowMap : EShaderPath::CommonShadowMap)->Bind(DeviceContext);
	}

	for (const FShadowDrawCommand& Cmd : Builder.GetCommands())
	{
		SubmitShadowCommand(Device, Cmd);
		++ShadowDrawCallCountThisFrame;
	}

	UnbindShadowWriteTargets(Device);
	return true;
}

void FShadowRenderer::RenderAtlasMomentBlurPass(FD3DDevice& Device, FSystemResources& Resources, const FSceneEnvironment& Environment)
{
	SCOPE_STAT_CAT("Shadow.LocalBlur", "4_ExecutePass");

	if (ShadowOptions.ShadowFilterMode != EShadowFilterMode::VSM
		&& ShadowOptions.ShadowFilterMode != EShadowFilterMode::ESM)
	{
		return;
	}

	FShadowAtlasResource& Atlas = Resources.ShadowResourceManager.GetAtlas();
	if (!Atlas.bUsePingPongFilterPath
		|| !Atlas.Map.RTV || !Atlas.Map.SRV
		|| !Atlas.FilterTempMap.RTV || !Atlas.FilterTempMap.SRV
		|| Atlas.Map.Width == 0 || Atlas.Map.Height == 0)
	{
		return;
	}

	struct FBlurRect
	{
		uint32 OffsetX = 0;
		uint32 OffsetY = 0;
		uint32 SizeX = 0;
		uint32 SizeY = 0;
	};

	const TArray<FLocalShadowRequest>& Requests = Resources.ShadowResourceManager.GetLocalShadowRequests();
	TArray<FBlurRect> BlurRects;
	BlurRects.reserve(Requests.size());
	for (const FLocalShadowRequest& Request : Requests)
	{
		if (!Request.bAllocated)
		{
			continue;
		}

		const FShadowViewData* View = nullptr;
		if (Request.RequestType == ELocalShadowRequestType::Spot)
		{
			if (Request.LightIndex >= Environment.GetNumSpotLights())
			{
				continue;
			}

			View = &Environment.GetSpotLight(Request.LightIndex).ShadowData.View;
		}
		else
		{
			if (Request.LightIndex >= Environment.GetNumPointLights() || Request.FaceIndex >= 6)
			{
				continue;
			}

			View = &Environment.GetPointLight(Request.LightIndex).ShadowData.View[Request.FaceIndex];
		}

		if (!View || !View->bAtlasAllocated || View->AtlasSizeX == 0 || View->AtlasSizeY == 0)
		{
			continue;
		}

		FBlurRect Rect = {};
		Rect.OffsetX = View->AtlasOffsetX;
		Rect.OffsetY = View->AtlasOffsetY;
		Rect.SizeX = View->AtlasSizeX;
		Rect.SizeY = View->AtlasSizeY;
		BlurRects.push_back(Rect);
	}

	if (BlurRects.empty())
	{
		return;
	}

	ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();
	FShader* BlurShader = FShaderManager::Get().GetOrCreate(EShaderPath::ShadowMomentBlur);
	if (!BlurShader)
	{
		return;
	}

	Resources.SetDepthStencilState(Device, EDepthStencilState::NoDepth);
	Resources.SetBlendState(Device, EBlendState::Opaque);
	Resources.SetRasterizerState(Device, ERasterizerState::SolidNoCull);

	ID3D11Buffer* BlurCB = ShadowFilterConstantBuffer.GetBuffer();
	if (!BlurCB)
	{
		return;
	}

	BlurShader->Bind(DeviceContext);
	DeviceContext->PSSetConstantBuffers(ECBSlot::PerShader0, 1, &BlurCB);
	DeviceContext->IASetInputLayout(nullptr);
	DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	DeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	DeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

	FShadowFilterConstants BlurParams = {};
	BlurParams.InvTextureSize[0] = 1.0f / static_cast<float>(Atlas.Map.Width);
	BlurParams.InvTextureSize[1] = 1.0f / static_cast<float>(Atlas.Map.Height);
	BlurParams.SourceKind = 0;
	BlurParams.SourceSlice = 0;

	{
		SCOPE_STAT_CAT("Shadow.LocalBlur.H", "4_ExecutePass");
		UnbindShadowReadResourcesForWrite(Device);
		ID3D11RenderTargetView* HorizontalRTV = Atlas.FilterTempMap.RTV;
		DeviceContext->OMSetRenderTargets(1, &HorizontalRTV, nullptr);
		ID3D11ShaderResourceView* HorizontalSource = Atlas.Map.SRV;
		DeviceContext->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &HorizontalSource);
		for (const FBlurRect& Rect : BlurRects)
		{
			D3D11_VIEWPORT RectViewport = {};
			RectViewport.TopLeftX = static_cast<float>(Rect.OffsetX);
			RectViewport.TopLeftY = static_cast<float>(Rect.OffsetY);
			RectViewport.Width = static_cast<float>(Rect.SizeX);
			RectViewport.Height = static_cast<float>(Rect.SizeY);
			RectViewport.MinDepth = 0.0f;
			RectViewport.MaxDepth = 1.0f;
			DeviceContext->RSSetViewports(1, &RectViewport);

			BlurParams.BlurDirection[0] = 1.0f;
			BlurParams.BlurDirection[1] = 0.0f;
			BlurParams.RectMin[0] = static_cast<float>(Rect.OffsetX) * BlurParams.InvTextureSize[0];
			BlurParams.RectMin[1] = static_cast<float>(Rect.OffsetY) * BlurParams.InvTextureSize[1];
			BlurParams.RectSize[0] = static_cast<float>(Rect.SizeX) * BlurParams.InvTextureSize[0];
			BlurParams.RectSize[1] = static_cast<float>(Rect.SizeY) * BlurParams.InvTextureSize[1];
			ShadowFilterConstantBuffer.Update(DeviceContext, &BlurParams, sizeof(FShadowFilterConstants));
			DeviceContext->Draw(3, 0);
		}
	}

	ID3D11ShaderResourceView* NullSRV = nullptr;
	DeviceContext->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &NullSRV);

	{
		SCOPE_STAT_CAT("Shadow.LocalBlur.V", "4_ExecutePass");
		UnbindShadowReadResourcesForWrite(Device);
		ID3D11RenderTargetView* VerticalRTV = Atlas.Map.RTV;
		DeviceContext->OMSetRenderTargets(1, &VerticalRTV, nullptr);
		ID3D11ShaderResourceView* VerticalSource = Atlas.FilterTempMap.SRV;
		DeviceContext->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &VerticalSource);
		for (const FBlurRect& Rect : BlurRects)
		{
			D3D11_VIEWPORT RectViewport = {};
			RectViewport.TopLeftX = static_cast<float>(Rect.OffsetX);
			RectViewport.TopLeftY = static_cast<float>(Rect.OffsetY);
			RectViewport.Width = static_cast<float>(Rect.SizeX);
			RectViewport.Height = static_cast<float>(Rect.SizeY);
			RectViewport.MinDepth = 0.0f;
			RectViewport.MaxDepth = 1.0f;
			DeviceContext->RSSetViewports(1, &RectViewport);

			BlurParams.BlurDirection[0] = 0.0f;
			BlurParams.BlurDirection[1] = 1.0f;
			BlurParams.RectMin[0] = static_cast<float>(Rect.OffsetX) * BlurParams.InvTextureSize[0];
			BlurParams.RectMin[1] = static_cast<float>(Rect.OffsetY) * BlurParams.InvTextureSize[1];
			BlurParams.RectSize[0] = static_cast<float>(Rect.SizeX) * BlurParams.InvTextureSize[0];
			BlurParams.RectSize[1] = static_cast<float>(Rect.SizeY) * BlurParams.InvTextureSize[1];
			ShadowFilterConstantBuffer.Update(DeviceContext, &BlurParams, sizeof(FShadowFilterConstants));
			DeviceContext->Draw(3, 0);
		}
	}

	DeviceContext->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &NullSRV);
	UnbindShadowWriteTargets(Device);
}

void FShadowRenderer::RenderDirectionalMomentBlurPass(FD3DDevice& Device, FSystemResources& Resources, const FSceneEnvironment& Environment)
{
	SCOPE_STAT_CAT("Shadow.DirectionalBlur", "4_ExecutePass");

	if (ShadowOptions.ShadowFilterMode != EShadowFilterMode::VSM
		&& ShadowOptions.ShadowFilterMode != EShadowFilterMode::ESM)
	{
		return;
	}

	if (!Environment.HasGlobalDirectionalLight())
	{
		return;
	}

	const FGlobalDirectionalLightParams& DirLight = Environment.GetGlobalDirectionalLightParams();
	if (!DirLight.ShadowData.Settings.bCastShadows)
	{
		return;
	}

	FDirectionalShadowArray& DirArray = Resources.ShadowResourceManager.GetShadowArray();
	if (!DirArray.MomentTexture || !DirArray.MomentSRV
		|| !DirArray.MomentFilterTempTexture || !DirArray.MomentFilterTempSRV
		|| DirArray.Width <= 0.0f || DirArray.Height <= 0.0f)
	{
		return;
	}

	ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();
	FShader* BlurShader = FShaderManager::Get().GetOrCreate(EShaderPath::ShadowMomentBlur);
	if (!BlurShader)
	{
		return;
	}

	Resources.SetDepthStencilState(Device, EDepthStencilState::NoDepth);
	Resources.SetBlendState(Device, EBlendState::Opaque);
	Resources.SetRasterizerState(Device, ERasterizerState::SolidNoCull);

	ID3D11Buffer* BlurCB = ShadowFilterConstantBuffer.GetBuffer();
	if (!BlurCB)
	{
		return;
	}

	BlurShader->Bind(DeviceContext);
	DeviceContext->PSSetConstantBuffers(ECBSlot::PerShader0, 1, &BlurCB);
	DeviceContext->IASetInputLayout(nullptr);
	DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	DeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	DeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

	FShadowFilterConstants BlurParams = {};
	BlurParams.InvTextureSize[0] = 1.0f / DirArray.Width;
	BlurParams.InvTextureSize[1] = 1.0f / DirArray.Height;
	BlurParams.RectMin[0] = 0.0f;
	BlurParams.RectMin[1] = 0.0f;
	BlurParams.RectSize[0] = 1.0f;
	BlurParams.RectSize[1] = 1.0f;
	BlurParams.SourceKind = 1;

	D3D11_VIEWPORT Viewport = {};
	Viewport.TopLeftX = 0.0f;
	Viewport.TopLeftY = 0.0f;
	Viewport.Width = DirArray.Width;
	Viewport.Height = DirArray.Height;
	Viewport.MinDepth = 0.0f;
	Viewport.MaxDepth = 1.0f;
	DeviceContext->RSSetViewports(1, &Viewport);

	const uint32 ActiveCascadeCount = GetActiveDirectionalCascadeCount(ShadowOptions, DirLight.ShadowData.NUM_CASCADES);
	for (uint32 CascadeIndex = 0; CascadeIndex < ActiveCascadeCount; ++CascadeIndex)
	{
		const uint32 SliceIndex = GetDirectionalShadowSliceIndex(ShadowOptions, CascadeIndex);
		if (!DirArray.MomentRTVs[SliceIndex] || !DirArray.MomentFilterTempRTVs[SliceIndex])
		{
			continue;
		}

		{
			SCOPE_STAT_CAT("Shadow.DirectionalBlur.H", "4_ExecutePass");
			UnbindShadowReadResourcesForWrite(Device);
			ID3D11RenderTargetView* HorizontalRTV = DirArray.MomentFilterTempRTVs[SliceIndex];
			DeviceContext->OMSetRenderTargets(1, &HorizontalRTV, nullptr);
			ID3D11ShaderResourceView* HorizontalSource = DirArray.MomentSRV;
			DeviceContext->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &HorizontalSource);
			BlurParams.BlurDirection[0] = 1.0f;
			BlurParams.BlurDirection[1] = 0.0f;
			BlurParams.SourceSlice = SliceIndex;
			ShadowFilterConstantBuffer.Update(DeviceContext, &BlurParams, sizeof(FShadowFilterConstants));
			DeviceContext->Draw(3, 0);
		}

		ID3D11ShaderResourceView* NullDirSRV = nullptr;
		DeviceContext->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &NullDirSRV);

		{
			SCOPE_STAT_CAT("Shadow.DirectionalBlur.V", "4_ExecutePass");
			UnbindShadowReadResourcesForWrite(Device);
			ID3D11RenderTargetView* VerticalRTV = DirArray.MomentRTVs[SliceIndex];
			DeviceContext->OMSetRenderTargets(1, &VerticalRTV, nullptr);
			ID3D11ShaderResourceView* VerticalSource = DirArray.MomentFilterTempSRV;
			DeviceContext->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &VerticalSource);
			BlurParams.BlurDirection[0] = 0.0f;
			BlurParams.BlurDirection[1] = 1.0f;
			BlurParams.SourceSlice = SliceIndex;
			ShadowFilterConstantBuffer.Update(DeviceContext, &BlurParams, sizeof(FShadowFilterConstants));
			DeviceContext->Draw(3, 0);
		}

		DeviceContext->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &NullDirSRV);
	}

	UnbindShadowWriteTargets(Device);
}

void FShadowRenderer::UnbindShadowReadResourcesForWrite(FD3DDevice& Device)
{
	ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();

	ID3D11ShaderResourceView* NullSystemSRV = nullptr;
	DeviceContext->PSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &NullSystemSRV);
	DeviceContext->PSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &NullSystemSRV);
	DeviceContext->VSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &NullSystemSRV);
	DeviceContext->VSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &NullSystemSRV);
	DeviceContext->CSSetShaderResources(ESystemTexSlot::ShadowMapAtlas, 1, &NullSystemSRV);
	DeviceContext->CSSetShaderResources(ESystemTexSlot::DirectionalShadowArray, 1, &NullSystemSRV);
}

void FShadowRenderer::UnbindShadowWriteTargets(FD3DDevice& Device)
{
	ID3D11DeviceContext* DeviceContext = Device.GetDeviceContext();
	DeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
}


//	RenderResources.cpp??UpdateFrameBuffer() 李멸퀬
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

//	Draw Binding 濡쒖쭅 遺꾨━??
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
