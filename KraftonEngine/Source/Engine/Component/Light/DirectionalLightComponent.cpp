#include "DirectionalLightComponent.h"
#include "Render/Types/GlobalLightParams.h"
#include "GameFramework/AActor.h"
#include "GameFramework/World.h"
#include "Engine/Serialization/Archive.h"
#include <cmath>
#include <algorithm>
#include <cfloat>

IMPLEMENT_CLASS(UDirectionalLightComponent, ULightComponent)

namespace
{
	void AddDirectionalLightArrow(FScene& Scene, const FVector& Origin, const FVector& Direction)
	{
		const FVector Forward = Direction.Normalized();
		if (Forward.Length() <= 0.001f)
		{
			return;
		}

		constexpr float ArrowLength = 2.2f;
		constexpr float HeadLength = 0.55f;
		constexpr float HeadRadius = 0.22f;
		constexpr int32 RingSegments = 12;

		FVector ReferenceUp(0.0f, 0.0f, 1.0f);
		if (std::abs(Forward.Dot(ReferenceUp)) > 0.98f)
		{
			ReferenceUp = FVector(0.0f, 1.0f, 0.0f);
		}

		const FVector Right = Forward.Cross(ReferenceUp).Normalized();
		const FVector Up = Right.Cross(Forward).Normalized();
		const FVector Tip = Origin + Forward * ArrowLength;
		const FVector HeadBase = Tip - Forward * HeadLength;
		const FColor ShaftColor = FColor::Red();
		const FColor HeadColor = FColor(255, 180, 80);

		Scene.AddDebugLine(Origin, Tip, ShaftColor);

		FVector PreviousRingPoint = HeadBase + Right * HeadRadius;
		for (int32 i = 1; i <= RingSegments; ++i)
		{
			const float Angle = (static_cast<float>(i) / static_cast<float>(RingSegments)) * 2.0f * 3.1415926535f;
			const FVector RingOffset = Right * std::cos(Angle) * HeadRadius + Up * std::sin(Angle) * HeadRadius;
			const FVector RingPoint = HeadBase + RingOffset;

			Scene.AddDebugLine(PreviousRingPoint, RingPoint, HeadColor);
			Scene.AddDebugLine(Tip, RingPoint, HeadColor);
			PreviousRingPoint = RingPoint;
		}

		Scene.AddDebugLine(HeadBase - Right * HeadRadius, HeadBase + Right * HeadRadius, HeadColor);
		Scene.AddDebugLine(HeadBase - Up * HeadRadius, HeadBase + Up * HeadRadius, HeadColor);
	}
}

FMatrix UDirectionalLightComponent::GetViewMatrix() const
{
	UpdateWorldMatrix();

	FVector LightDir = GetForwardVector().Normalized();

	FVector RefUp = (std::abs(LightDir.Z) < 0.99f) ? FVector(0.f, 0.f, 1.f) : FVector(0.f, 1.f, 0.f);

	FVector Right = RefUp.Cross(LightDir).Normalized();
	FVector Up    = LightDir.Cross(Right).Normalized();

	const float LightDistance = 1000.f;
	FVector EyePos = -LightDir * LightDistance;

	return FMatrix::MakeViewMatrix(LightDir, Right, Up, EyePos);
}

FMatrix UDirectionalLightComponent::GetProjMatrix() const
{
	return FMatrix::Identity;
}

void UDirectionalLightComponent::UpdatePSMMatrices(const FMatrix& CamVP, FMatrix& OutView, FMatrix& OutProj) const
{
	UpdateWorldMatrix();
	FVector LightDir = GetForwardVector().Normalized();

	constexpr float LightDist = 1000.f;
	FVector NDCLightPos = CamVP.TransformPositionWithW(-LightDir * LightDist);

	const FVector NDCCenter(0.f, 0.f, 0.5f);
	FVector LookDir = (NDCCenter - NDCLightPos).Normalized();

	FVector RefUp = (std::abs(LookDir.Z) < 0.99f) ? FVector(0.f, 0.f, 1.f) : FVector(0.f, 1.f, 0.f);
	FVector Right = RefUp.Cross(LookDir).Normalized();
	FVector Up    = LookDir.Cross(Right).Normalized();

	FMatrix PSMView = FMatrix::MakeViewMatrix(LookDir, Right, Up, NDCLightPos);

	FMatrix PSMProj = FMatrix::MakeProjectionMatrix(0.f, 0.01f, 1000.f, 1.f, true, 4.f);

	OutView = PSMView;
	OutProj = PSMProj;
	PSMCamViewProj = OutView * OutProj;
}

void UDirectionalLightComponent::ContributeSelectedVisuals(FScene& Scene) const
{
	FVector WorldPos = GetWorldLocation();
	AddDirectionalLightArrow(Scene, WorldPos, GetForwardVector());
}

void UDirectionalLightComponent::PushToScene()
{
	if (!Owner) return;
	UWorld* World = Owner->GetWorld();
	if (!World) return;

	FGlobalDirectionalLightParams Params;
	Params.Direction = GetForwardVector();
	Params.Intensity = Intensity;
	Params.LightColor = LightColor;
	Params.bVisible = bVisible;

	Params.ShadowData.Settings.bCastShadows = bCastShadows;
	Params.ShadowData.Settings.ShadowResolutionScale = ShadowResolutionScale;
	Params.ShadowData.Settings.ShadowBias = ShadowBias;
	Params.ShadowData.Settings.ShadowSlopeBias = ShadowSlopeBias;
	Params.ShadowData.Settings.ShadowSharpen = ShadowSharpen;
	//	bOverrideCameraWithLight는 나중에 고려

	Params.ShadowData.View.DepthMap = {};
	Params.ShadowData.View.LightView = GetViewMatrix();
	Params.ShadowData.View.LightProj = GetProjMatrix();

	World->GetScene().GetEnvironment().AddGlobalDirectionalLight(this, Params);
}

void UDirectionalLightComponent::DestroyFromScene()
{
	if (!Owner) return;
	UWorld* World = Owner->GetWorld();
	if (!World) return;

	World->GetScene().GetEnvironment().RemoveGlobalDirectionalLight(this);
}
