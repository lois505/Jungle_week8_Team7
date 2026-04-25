#pragma once

#include "Editor/UI/EditorWidget.h"
#include "Object/Object.h"

class UActorComponent;
class AActor;
class UPointLightComponent;
class USpotLightComponent;
struct FLightShadowSettings;
struct FShadowViewData;

class FEditorPropertyWidget : public FEditorWidget
{
public:
	virtual void Render(float DeltaTime) override;

private:
	void RenderComponentTree(AActor* Actor);
	void RenderSceneComponentNode(class USceneComponent* Comp);
	void RenderDetails(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors);
	void RenderComponentProperties(AActor* Actor, const TArray<AActor*>& SelectedActors);
	void RenderActorProperties(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors);
	void RenderPointLightShadowPreview(UPointLightComponent* PointLight);
	void RenderSpotLightShadowPreview(USpotLightComponent* SpotLight);
	void RenderShadowMapPreviewImage(const FLightShadowSettings& Settings, const FShadowViewData& View);
	bool RenderPropertyWidget(TArray<struct FPropertyDescriptor>& Props, int32& Index);

	void PropagatePropertyChange(const FString& PropName, const TArray<AActor*>& SelectedActors);

	static FString OpenObjFileDialog();

	UActorComponent* SelectedComponent = nullptr;
	AActor* LastSelectedActor = nullptr;
	bool bActorSelected = true; // true: Actor details, false: Component details
};
