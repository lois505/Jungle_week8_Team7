#include "ShadowRenderer.h"

#include "FrameContext.h"
#include "ShadowPassContext.h"
#include "Render/Culling/ConvexVolume.h"
#include "Render/Proxy/FScene.h"
#include "Render/Proxy/PrimitiveSceneProxy.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Resource/ShaderManager.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float GPSMVirtualSlideBack = 10.0f;

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

	FBoundingBox BuildShadowReceiverBounds(const FScene& Scene)
	{
		FBoundingBox Bounds;

		for (const FPrimitiveSceneProxy* Proxy : Scene.GetAllProxies())
		{
			if (!Proxy || !IsShadowCasterReceiverProxy(*Proxy))
			{
				continue;
			}

			const FBoundingBox& ProxyBounds = Proxy->GetCachedBounds();
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
		const FBoundingBox ReceiverBounds = BuildShadowReceiverBounds(Scene);
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
	FMatrix PSMMainViewProjection = FMatrix::Identity;
	FMatrix PSMLightView = FMatrix::Identity;
	FMatrix PSMLightProj = FMatrix::Identity;
	BuildPSMViewProjection(MainFrame, Scene, Light.Direction, PSMMainViewProjection, PSMLightView, PSMLightProj);

	const FMatrix PSMLightViewProj = PSMLightView * PSMLightProj;
	Light.ShadowData.MainViewProjection = PSMMainViewProjection;
	Light.ShadowData.PSMView.LightView = PSMLightView;
	Light.ShadowData.PSMView.LightProj = PSMLightProj;
	Light.ShadowData.PSMView.LightViewProj = PSMLightViewProj;

	UpdateCacades(Light.ShadowData, Light.Direction, MainFrame);
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

		RenderShadowView(Device, Resources, Light.ShadowData.PSMView, Scene, true, &Light.ShadowData.MainViewProjection);
	}

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
void FShadowRenderer::RenderShadowView(FD3DDevice& Device, FSystemResources& Resources, FShadowViewData& View, FScene& Scene,
	bool bUsePSMShader, const FMatrix* PSMMainViewProjection)
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
	Resources.SetRasterizerState(Device, bUsePSMShader ? ERasterizerState::SolidNoCull : ERasterizerState::SolidBackCull);

	Builder.BeginBuild(Scene.GetProxyCount());
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
