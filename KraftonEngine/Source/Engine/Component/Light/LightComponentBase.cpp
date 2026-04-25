#include "LightComponentBase.h"
#include "Serialization/Archive.h"
#include "Object/ObjectFactory.h"

IMPLEMENT_ABSTRACT_CLASS(ULightComponentBase, USceneComponent)

void ULightComponentBase::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	USceneComponent::GetEditableProperties(OutProps);
	OutProps.push_back({"Intensity", EPropertyType::Float, &Intensity, 0.0f, 50.f, 0.05f});
	OutProps.push_back({"Color", EPropertyType::Color4, &LightColor});
	OutProps.push_back({"Visible", EPropertyType::Bool, &bVisible});
	OutProps.push_back({"Cast Shadows", EPropertyType::Bool, &bCastShadows});
}

void ULightComponentBase::Serialize(FArchive& Ar)
{
	USceneComponent::Serialize(Ar);
	Ar << Intensity;
	Ar << LightColor;
	Ar << bVisible;
	Ar << bCastShadows;
}

FMatrix ULightComponentBase::GetViewMatrix() const
{
	UpdateWorldMatrix();

	auto F = GetForwardVector();
	auto R = GetRightVector();
	auto U = GetUpVector();
	auto E = GetWorldLocation();

	return FMatrix(
		R.X, U.X, F.X, 0,
		R.Y, U.Y, F.Y, 0,
		R.Z, U.Z, F.Z, 0,
		-E.Dot(R), -E.Dot(U), -E.Dot(F), 1
	);
}

FMatrix ULightComponentBase::GetProjMatrix() const
{
	//	Light 별로 다른 Proj Matrix
	return FMatrix();
}

FMatrix ULightComponentBase::GetViewProjMatrix() const
{
	return GetViewMatrix() * GetProjMatrix();
}
