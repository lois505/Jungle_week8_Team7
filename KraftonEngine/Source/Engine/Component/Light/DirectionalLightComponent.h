#pragma once
#include "Component/Light/LightComponent.h"
#include "Math/Matrix.h"

class UDirectionalLightComponent : public ULightComponent
{
public:
	DECLARE_CLASS(UDirectionalLightComponent, ULightComponent)

	void ContributeSelectedVisuals(FScene& Scene) const;
	virtual void PushToScene() override;
	virtual void DestroyFromScene() override;

	void UpdatePSMMatrices(const FMatrix& CamVP, FMatrix& OutView, FMatrix& OutProj) const;
	FMatrix GetPSMViewProj() const { return PSMCamViewProj; }

protected:
	virtual FMatrix GetViewMatrix() const override;
	virtual FMatrix GetProjMatrix() const override;

private:
	mutable FMatrix PSMCamViewProj = FMatrix::Identity;
};
