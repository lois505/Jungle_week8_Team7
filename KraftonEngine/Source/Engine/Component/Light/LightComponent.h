#pragma once
#include "Component/Light/LightComponentBase.h"

class ULightComponent : public ULightComponentBase
{
public:
	DECLARE_CLASS(ULightComponent, ULightComponentBase)
	
	void GetEditableProperties(TArray<FPropertyDescriptor>& OutProps) override;
	void PostEditProperty(const char* PropertyName) override { ULightComponentBase::PostEditProperty(PropertyName); PushToScene(); }
	void Serialize(FArchive& Ar) override;
	bool IsOverrideCameraWithLightPerspective() const { return bOverrideCameraWithLightPerspective; }
	void SetOverrideCameraWithLightPerspective(bool bEnable) { bOverrideCameraWithLightPerspective = bEnable; PushToScene(); }
	
protected:
	float ShadowResolutionScale = 1.f;
	float ShadowBias = 0.2f;
	float ShadowSlopeBias = 0.2f;
	float ShadowSharpen = 0.35f;
	bool bOverrideCameraWithLightPerspective = false;
};
