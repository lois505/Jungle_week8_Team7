#include "PointLightComponent.h"
#include "Engine/Serialization/Archive.h"
#include "GameFramework/AActor.h"
#include "GameFramework/World.h"
#include "Math/MathUtils.h"

namespace
{
	void AddWireCircle(FScene& Scene, const FVector& Center, const FVector& AxisA, const FVector& AxisB, float Radius,
		int32 Segments, const FColor& Color)
	{
		if (Radius <= 0.0f || Segments < 3)
		{
			return;
		}

		const float Step = 2.0f * FMath::Pi / static_cast<float>(Segments);
		FVector Prev = Center + AxisA * Radius;

		for (int32 Index = 1; Index <= Segments; ++Index)
		{
			const float Angle = Step * static_cast<float>(Index);
			const FVector Next = Center + (AxisA * cosf(Angle) + AxisB * sinf(Angle)) * Radius;
			Scene.AddDebugLine(Prev, Next, Color);
			Prev = Next;
		}
	}
}

IMPLEMENT_CLASS(UPointLightComponent, ULightComponent)

void UPointLightComponent::ContributeSelectedVisuals(FScene& Scene) const
{
	if (IsOverrideCameraWithLightPerspective())
	{
		return;
	}

	const FVector Center = GetWorldLocation();
	constexpr int32 Segments = 24;

	AddWireCircle(Scene, Center, FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f), AttenuationRadius, Segments,
		FColor::Yellow());
	AddWireCircle(Scene, Center, FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 0.0f, 1.0f), AttenuationRadius, Segments,
		FColor::Yellow());
	AddWireCircle(Scene, Center, FVector(0.0f, 1.0f, 0.0f), FVector(0.0f, 0.0f, 1.0f), AttenuationRadius, Segments,
		FColor::Yellow());
}

void UPointLightComponent::PushToScene()
{
	if (!Owner) return;
	UWorld* World = Owner->GetWorld();
	if (!World) return;

	FPointLightParams Params;
	Params.AttenuationRadius = AttenuationRadius;
	Params.bVisible = bVisible;
	Params.Intensity = Intensity;
	Params.LightColor = LightColor;
	Params.LightFalloffExponent = LightFalloffExponent;
	Params.LightType = ELightType::Point;
	Params.Position = GetWorldLocation();

	Params.ShadowData.Settings.bCastShadows = bCastShadows;
	Params.ShadowData.Settings.ShadowResolutionScale = ShadowResolutionScale;
	Params.ShadowData.Settings.ShadowBias = ShadowBias;
	Params.ShadowData.Settings.ShadowSlopeBias = ShadowSlopeBias;
	Params.ShadowData.Settings.ShadowSharpen = ShadowSharpen;
	Params.ShadowData.bOverrideCameraWithLight = IsOverrideCameraWithLightPerspective();


	FVector FRU[6][3] = {
	{ FVector(1,  0,  0), FVector(0,  1,  0), FVector(0,  0,  1) }, // +X
	{ FVector(-1,  0,  0), FVector(0, -1,  0), FVector(0,  0,  1) }, // -X
	{ FVector(0,  1,  0), FVector(-1,  0,  0), FVector(0,  0,  1) }, // +Y
	{ FVector(0, -1,  0), FVector(1,  0,  0), FVector(0,  0,  1) }, // -Y
	{ FVector(0,  0,  1), FVector(1,  0,  0), FVector(0,  1,  0) }, // +Z
	{ FVector(0,  0, -1), FVector(1,  0,  0), FVector(0, -1,  0) }, // -Z
	};

	for (int32 i = 0; i < 6; i++)
	{
		Params.ShadowData.View[i].DepthMap = {};
		//	TODO : View, Proj, ViewProj 넣기
		Params.ShadowData.View[i].LightView = FMatrix::MakeViewMatrix(FRU[i][0], FRU[i][1], FRU[i][2], GetWorldLocation());
		Params.ShadowData.View[i].LightProj = FMatrix::MakeProjectionMatrix(FMath::Pi * 0.5f, 0.1f, AttenuationRadius, 1.f);
		Params.ShadowData.View[i].LightViewProj = Params.ShadowData.View[i].LightView * Params.ShadowData.View[i].LightProj;
	}

	//	Default view in UI : 0
	Params.ShadowData.PreviewViewIndex = 0;

	World->GetScene().GetEnvironment().AddPointLight(this, Params);
}

void UPointLightComponent::DestroyFromScene()
{
	if (!Owner) return;
	UWorld* World = Owner->GetWorld();
	if (!World) return;
	World->GetScene().GetEnvironment().RemovePointLight(this);
}

void UPointLightComponent::Serialize(FArchive& Ar)
{
	ULightComponent::Serialize(Ar);
	Ar << AttenuationRadius;
	Ar << LightFalloffExponent;
}

void UPointLightComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	ULightComponent::GetEditableProperties(OutProps);
	OutProps.push_back({ "AttenuationRadius", EPropertyType::Float, &AttenuationRadius, 0.05f, 1000.f, 0.01f });
	OutProps.push_back({ "LightFalloffExponent", EPropertyType::Float, &LightFalloffExponent, 0.05f, 10.f, 0.01f });
}
