#pragma once

#include "Viewport/ViewportClient.h"
#include "Render/Types/RenderTypes.h"
#include "Render/Types/ViewTypes.h"

#include "UI/SWindow.h"
#include <string>
#include "Core/RayTypes.h"
#include "Core/CollisionTypes.h"
#include "Math/Matrix.h"
class UWorld;
class UCameraComponent;
class UBillboardComponent;
class UGizmoComponent;
class FEditorSettings;
class FWindowsWindow;
class FSelectionManager;
class FViewport;
class FOverlayStatSystem;
class AActor;
class FSceneEnvironment;
struct FShadowViewData;

class FEditorViewportClient : public FViewportClient
{
public:
	void Initialize(FWindowsWindow* InWindow);
	void SetOverlayStatSystem(FOverlayStatSystem* InOverlayStatSystem) { OverlayStatSystem = InOverlayStatSystem; }
	// World는 더 이상 저장하지 않는다 — GetWorld()는 GEngine->GetWorld()를 경유하여
	// ActiveWorldHandle을 따르므로 PIE 전환 시 자동으로 올바른 월드를 반환한다.
	UWorld* GetWorld() const;
	void SetGizmo(UGizmoComponent* InGizmo) { Gizmo = InGizmo; }
	void SetSettings(const FEditorSettings* InSettings) { Settings = InSettings; }
	void SetSelectionManager(FSelectionManager* InSelectionManager) { SelectionManager = InSelectionManager; }
	UGizmoComponent* GetGizmo() { return Gizmo; }

	// 뷰포트별 렌더 옵션
	FViewportRenderOptions& GetRenderOptions() { return RenderOptions; }
	const FViewportRenderOptions& GetRenderOptions() const { return RenderOptions; }

	// 뷰포트 타입 전환 (Perspective / Ortho 방향)
	void SetViewportType(ELevelViewportType NewType);
	void SetViewportSize(float InWidth, float InHeight);

	// Camera lifecycle
	void CreateCamera();
	void DestroyCamera();
	void ResetCamera();
	UCameraComponent* GetCamera() const { return Camera; }

	void Tick(float DeltaTime);

	// 활성 상태 — 활성 뷰포트만 입력 처리
	void SetActive(bool bInActive) { bIsActive = bInActive; }
	bool IsActive() const { return bIsActive; }

	// FViewport 소유
	void SetViewport(FViewport* InViewport) { Viewport = InViewport; }
	FViewport* GetViewport() const { return Viewport; }

	// 뷰포트 스크린 좌표 (ImGui screen space)
	const FRect& GetViewportScreenRect() const { return ViewportScreenRect; }

	// 마우스가 뷰포트 안에 있으면 뷰포트 로컬 좌표 반환 (시각화용)
	bool GetCursorViewportPosition(uint32& OutX, uint32& OutY) const;

	// SWindow 레이아웃 연결 — SSplitter 리프 노드
	void SetLayoutWindow(SWindow* InWindow) { LayoutWindow = InWindow; }
	SWindow* GetLayoutWindow() const { return LayoutWindow; }

	// SWindow Rect → ViewportScreenRect 갱신 + FViewport 리사이즈 요청
	void UpdateLayoutRect();

	// ImDrawList에 자신의 SRV를 SWindow Rect 위치에 렌더 (활성 테두리 포함)
	void RenderViewportImage(bool bIsActiveViewport);

private:
	struct FBillboardVisibilityBackup
	{
		UBillboardComponent* Component = nullptr;
		bool bWasVisible = true;
	};

	void TickEditorShortcuts();
	void TickInput(float DeltaTime);
	void TickInteraction(float DeltaTime);
	void HandleDragStart(const FRay& Ray); //픽킹 시작
	void ApplyLightPerspectiveOverride();
	void ApplyBillboardVisibilityForLightOverride(AActor* SelectedActor);
	void RestoreBillboardVisibilityAfterLightOverride();
	void SaveCameraStateForLightOverride();
	void RestoreCameraStateAfterLightOverride();
	void ApplyCameraFromShadowView(const FShadowViewData& ShadowView, bool bOrthographic);
	bool TryApplyDirectionalLightOverride(AActor* SelectedActor, const FSceneEnvironment& Environment);
	bool TryApplySpotLightOverride(AActor* SelectedActor, const FSceneEnvironment& Environment);
	bool TryApplyPointLightOverride(AActor* SelectedActor, const FSceneEnvironment& Environment);
	void ClearDirectionalOverrideSnapshot();

private:
	FViewport* Viewport = nullptr;
	SWindow* LayoutWindow = nullptr;
	FWindowsWindow* Window = nullptr;
	FOverlayStatSystem* OverlayStatSystem = nullptr;
	UCameraComponent* Camera = nullptr;
	UGizmoComponent* Gizmo = nullptr;
	const FEditorSettings* Settings = nullptr;
	FSelectionManager* SelectionManager = nullptr;
	FViewportRenderOptions RenderOptions;
	bool bLightPerspectiveOverrideActive = false;
	bool bHasSavedCameraStateForLightOverride = false;
	FVector SavedCameraWorldLocation = FVector(0.0f, 0.0f, 0.0f);
	FVector SavedCameraEulerRotation = FVector(0.0f, 0.0f, 0.0f); // X=Roll, Y=Pitch, Z=Yaw
	float SavedCameraFOV = 3.14159265358979f / 3.0f;
	float SavedCameraOrthoWidth = 10.0f;
	bool bSavedCameraIsOrthographic = false;
	AActor* BillboardOverrideActor = nullptr;
	TArray<FBillboardVisibilityBackup> BillboardVisibilityBackups;

	bool bHasDirectionalOverrideSnapshot = false;
	AActor* DirectionalOverrideSnapshotActor = nullptr;
	int32 DirectionalOverrideSnapshotViewIndex = -1;
	FMatrix DirectionalOverrideSnapshotLightView = FMatrix::Identity;
	FMatrix DirectionalOverrideSnapshotLightProj = FMatrix::Identity;
	FMatrix DirectionalOverrideSnapshotLightViewProj = FMatrix::Identity;

	float WindowWidth = 1920.f;
	float WindowHeight = 1080.f;

	bool bIsActive = false;
	// 뷰포트 슬롯의 스크린 좌표 (ImGui screen space = 윈도우 클라이언트 좌표)
	FRect ViewportScreenRect;
};
