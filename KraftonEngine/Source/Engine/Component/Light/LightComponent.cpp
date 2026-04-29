#include "Component/Light/LightComponent.h"
#include "Serialization/Archive.h"
#include "Object/ObjectFactory.h"


IMPLEMENT_ABSTRACT_CLASS(ULightComponent, ULightComponentBase)

void ULightComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	ULightComponentBase::GetEditableProperties(OutProps);
	
	OutProps.push_back({"Shadow Resolution Scale" ,EPropertyType::Float, &ShadowResolutionScale, 0.0f, 10.f, 0.05f});
	OutProps.push_back({"Shadow Bias" ,EPropertyType::Float, &ShadowBias, 0.0f, 10.f, 0.05f});
	OutProps.push_back({"Shadow Slope Bias" ,EPropertyType::Float, &ShadowSlopeBias, 0.0f, 10.f, 0.05f});
	OutProps.push_back({"Shadow Sharpen" ,EPropertyType::Float, &ShadowSharpen, 0.0f, 10.f, 0.05f});
}

void ULightComponent::Serialize(FArchive& Ar)
{
	ULightComponentBase::Serialize(Ar);
	
	Ar << ShadowResolutionScale;
	Ar << ShadowBias;
	Ar << ShadowSlopeBias;
	Ar << ShadowSharpen;
	Ar << bOverrideCameraWithLightPerspective;
}
