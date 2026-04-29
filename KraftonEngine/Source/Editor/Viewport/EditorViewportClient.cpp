#include "Editor/Viewport/EditorViewportClient.h"

#include "Editor/UI/EditorConsoleWidget.h"
#include "Editor/Subsystem/OverlayStatSystem.h"
#include "Editor/Settings/EditorSettings.h"
#include "Engine/Input/InputSystem.h"
#include "Engine/Profiling/PlatformTime.h"
#include "Engine/Runtime/WindowsWindow.h"

#include "Component/CameraComponent.h"
#include "Component/BillboardComponent.h"
#include "Component/Light/DirectionalLightComponent.h"
#include "Component/Light/PointLightComponent.h"
#include "Component/Light/SpotLightComponent.h"
#include "Viewport/Viewport.h"
#include "GameFramework/World.h"
#include "Engine/Runtime/Engine.h"
#include "Render/Proxy/SceneEnvironment.h"
#include "Render/Types/ShadowData.h"

UWorld* FEditorViewportClient::GetWorld() const
{
	return GEngine ? GEngine->GetWorld() : nullptr;
}
#include "Component/GizmoComponent.h"
#include "Component/PrimitiveComponent.h"
#include "Collision/RayUtils.h"
#include "Object/Object.h"
#include "Editor/Selection/SelectionManager.h"
#include "Editor/EditorEngine.h"
#include "GameFramework/AActor.h"
#include "ImGui/imgui.h"
#include <cmath>

void FEditorViewportClient::Initialize(FWindowsWindow* InWindow)
{
	Window = InWindow;
}

void FEditorViewportClient::CreateCamera()
{
	DestroyCamera();
	Camera = UObjectManager::Get().CreateObject<UCameraComponent>();
}

void FEditorViewportClient::DestroyCamera()
{
	//RestoreBillboardVisibilityAfterLightOverride();
	ClearDirectionalOverrideSnapshot();
	bLightPerspectiveOverrideActive = false;
	bHasSavedCameraStateForLightOverride = false;

	if (Camera)
	{
		UObjectManager::Get().DestroyObject(Camera);
		Camera = nullptr;
	}
}

void FEditorViewportClient::ResetCamera()
{
	if (!Camera || !Settings) return;
	Camera->SetWorldLocation(Settings->InitViewPos);
	Camera->LookAt(Settings->InitLookAt);
}

void FEditorViewportClient::SetViewportType(ELevelViewportType NewType)
{
	if (!Camera) return;

	RenderOptions.ViewportType = NewType;

	if (NewType == ELevelViewportType::Perspective)
	{
		Camera->SetOrthographic(false);
		return;
	}

	// FreeOrthographic: 현재 카메라 위치/회전 유지, 투영만 Ortho로 전환
	if (NewType == ELevelViewportType::FreeOrthographic)
	{
		Camera->SetOrthographic(true);
		return;
	}

	// 고정 방향 Orthographic: 카메라를 프리셋 방향으로 설정
	Camera->SetOrthographic(true);

	constexpr float OrthoDistance = 50.0f;
	FVector Position = FVector(0, 0, 0);
	FVector Rotation = FVector(0, 0, 0); // (Roll, Pitch, Yaw)

	switch (NewType)
	{
	case ELevelViewportType::Top:
		Position = FVector(0, 0, OrthoDistance);
		Rotation = FVector(0, 90.0f, 0);	// Pitch down (positive pitch = look -Z)
		break;
	case ELevelViewportType::Bottom:
		Position = FVector(0, 0, -OrthoDistance);
		Rotation = FVector(0, -90.0f, 0);	// Pitch up (negative pitch = look +Z)
		break;
	case ELevelViewportType::Front:
		Position = FVector(OrthoDistance, 0, 0);
		Rotation = FVector(0, 0, 180.0f);	// Yaw to look -X
		break;
	case ELevelViewportType::Back:
		Position = FVector(-OrthoDistance, 0, 0);
		Rotation = FVector(0, 0, 0.0f);		// Yaw to look +X
		break;
	case ELevelViewportType::Left:
		Position = FVector(0, -OrthoDistance, 0);
		Rotation = FVector(0, 0, 90.0f);	// Yaw to look +Y
		break;
	case ELevelViewportType::Right:
		Position = FVector(0, OrthoDistance, 0);
		Rotation = FVector(0, 0, -90.0f);	// Yaw to look -Y
		break;
	default:
		break;
	}

	Camera->SetRelativeLocation(Position);
	Camera->SetRelativeRotation(Rotation);
}

void FEditorViewportClient::SetViewportSize(float InWidth, float InHeight)
{
	if (InWidth > 0.0f)
	{
		WindowWidth = InWidth;
	}

	if (InHeight > 0.0f)
	{
		WindowHeight = InHeight;
	}

	if (Camera)
	{
		Camera->OnResize(static_cast<int32>(WindowWidth), static_cast<int32>(WindowHeight));
	}
}

void FEditorViewportClient::Tick(float DeltaTime)
{
	if (!bIsActive) return;

	ApplyLightPerspectiveOverride();
	TickEditorShortcuts();
	TickInput(DeltaTime);
	TickInteraction(DeltaTime);
}

void FEditorViewportClient::TickEditorShortcuts()
{
	UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEngine);
	if (!EditorEngine)
	{
		return;
	}

	// PIE 중 ESC로 종료 (UE 동작과 동일)
	if (EditorEngine->IsPlayingInEditor() && InputSystem::Get().GetKeyDown(VK_ESCAPE))
	{
		EditorEngine->RequestEndPlayMap();
	}

	// Ctrl+D — 선택된 액터 복제
	if (SelectionManager && InputSystem::Get().GetKey(VK_CONTROL) && InputSystem::Get().GetKeyDown('D'))
	{
		const TArray<AActor*> ToDuplicate = SelectionManager->GetSelectedActors();
		if (!ToDuplicate.empty())
		{
			TArray<AActor*> NewSelection;
			for (AActor* Src : ToDuplicate)
			{
				if (!Src) continue;
				AActor* Dup = Cast<AActor>(Src->Duplicate(nullptr));
				if (Dup)
				{
					NewSelection.push_back(Dup);
				}
			}
			SelectionManager->ClearSelection();
			for (AActor* Actor : NewSelection)
			{
				SelectionManager->ToggleSelect(Actor);
			}
		}
	}
}

void FEditorViewportClient::TickInput(float DeltaTime)
{
	if (!Camera)
	{
		return;
	}

	if (InputSystem::Get().GetGuiInputState().bUsingKeyboard == true)
	{
		return;
	}

	if (bLightPerspectiveOverrideActive)
	{
		if (InputSystem::Get().GetKeyUp(VK_SPACE))
		{
			Gizmo->SetNextMode();
		}
		return;
	}

	const FCameraState& CameraState = Camera->GetCameraState();
	const bool bIsOrtho = CameraState.bIsOrthogonal;

	const float MoveSensitivity = RenderOptions.CameraMoveSensitivity;
	const float CameraSpeed = (Settings ? Settings->CameraSpeed : 10.f) * MoveSensitivity;
	const float PanMouseScale = CameraSpeed * 0.01f;

	if (!bIsOrtho)
	{
		// ── Perspective: 키보드 이동 + 중클릭 로컬 팬 ──
		FVector LocalMove = FVector(0, 0, 0);
		float WorldVerticalMove = 0.0f;

		if (InputSystem::Get().GetKey('W'))
			LocalMove.X += CameraSpeed;
		if (InputSystem::Get().GetKey('A'))
			LocalMove.Y -= CameraSpeed;
		if (InputSystem::Get().GetKey('S'))
			LocalMove.X -= CameraSpeed;
		if (InputSystem::Get().GetKey('D'))
			LocalMove.Y += CameraSpeed;
		if (InputSystem::Get().GetKey('Q'))
			WorldVerticalMove -= CameraSpeed;
		if (InputSystem::Get().GetKey('E'))
			WorldVerticalMove += CameraSpeed;

		LocalMove *= DeltaTime;
		Camera->MoveLocal(LocalMove);
		if (WorldVerticalMove != 0.0f)
		{
			Camera->AddWorldOffset(FVector(0.0f, 0.0f, WorldVerticalMove * DeltaTime));
		}

		//pan 패닝
		if (InputSystem::Get().GetKey(VK_MBUTTON))
		{
			float DeltaX = static_cast<float>(InputSystem::Get().MouseDeltaX());
			float DeltaY = static_cast<float>(InputSystem::Get().MouseDeltaY());
			Camera->MoveLocal(FVector(0.0f, -DeltaX * PanMouseScale * 0.05f , DeltaY * PanMouseScale * 0.05f));
		}

		// ── Perspective: 키보드 회전 ──
		FVector Rotation = FVector(0, 0, 0);

		const float RotateSensitivity = RenderOptions.CameraRotateSensitivity;
		const float AngleVelocity = (Settings ? Settings->CameraRotationSpeed : 60.f) * RotateSensitivity;
		if (InputSystem::Get().GetKey(VK_UP))
			Rotation.Z -= AngleVelocity;
		if (InputSystem::Get().GetKey(VK_LEFT))
			Rotation.Y -= AngleVelocity;
		if (InputSystem::Get().GetKey(VK_DOWN))
			Rotation.Z += AngleVelocity;
		if (InputSystem::Get().GetKey(VK_RIGHT))
			Rotation.Y += AngleVelocity;

		// ── Perspective: 마우스 우클릭 → 회전 ──
		FVector MouseRotation = FVector(0, 0, 0);
		float MouseRotationSpeed = 0.15f * RotateSensitivity;

		if (InputSystem::Get().GetKey(VK_RBUTTON))
		{
			float DeltaX = static_cast<float>(InputSystem::Get().MouseDeltaX());
			float DeltaY = static_cast<float>(InputSystem::Get().MouseDeltaY());

			MouseRotation.Y += DeltaX * MouseRotationSpeed;
			MouseRotation.Z += DeltaY * MouseRotationSpeed;
		}

		Rotation *= DeltaTime;
		Camera->Rotate(Rotation.Y + MouseRotation.Y, Rotation.Z + MouseRotation.Z);
	}
	else
	{
		// ── Orthographic: 마우스 우클릭 드래그 → 평행이동 (Pan) ──
		if (InputSystem::Get().GetKey(VK_RBUTTON))
		{
			float DeltaX = static_cast<float>(InputSystem::Get().MouseDeltaX());
			float DeltaY = static_cast<float>(InputSystem::Get().MouseDeltaY());

			// OrthoWidth 기준으로 감도 스케일 (줌 레벨에 비례)
			float PanScale = CameraState.OrthoWidth * 0.002f * MoveSensitivity;

			// 카메라 로컬 Right/Up 방향으로 이동
			Camera->MoveLocal(FVector(0, -DeltaX * PanScale, DeltaY * PanScale));
		}
	}

	if (InputSystem::Get().GetKeyUp(VK_SPACE))
		Gizmo->SetNextMode();
}

void FEditorViewportClient::TickInteraction(float DeltaTime)
{
	(void)DeltaTime;

	if (!Camera || !Gizmo || !GetWorld())
	{
		return;
	}

	if (bLightPerspectiveOverrideActive)
	{
		if (Gizmo->IsHolding())
		{
			Gizmo->SetHolding(false);
		}
		Gizmo->SetPressedOnHandle(false);
		Gizmo->SetVisibility(false);
		return;
	}

	if (!Gizmo->IsVisible())
	{
		if (SelectionManager)
		{
			AActor* PrimarySelection = SelectionManager->GetPrimarySelection();
			if (PrimarySelection)
			{
				Gizmo->SetSelectedActors(&SelectionManager->GetSelectedActors());
				Gizmo->SetTarget(PrimarySelection);
			}
		}
	}

	//기즈모 비활성화하는 설정. 일단은 PIE 중에도 기즈모가 생김.
	//UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEngine);
	//if (EditorEngine && EditorEngine->IsPlayingInEditor())
	//{
	//	Gizmo->Deactivate();
	//	return;
	//}

	Gizmo->ApplyScreenSpaceScaling(Camera->GetWorldLocation(),
		Camera->IsOrthogonal(), Camera->GetOrthoWidth());

	// LineTrace 히트 판정용 AxisMask 갱신 (렌더링은 Proxy가 per-viewport로 직접 계산)
	Gizmo->SetAxisMask(UGizmoComponent::ComputeAxisMask(RenderOptions.ViewportType, Gizmo->GetMode()));

	// 기즈모 드래그 중에는 마우스가 뷰포트 밖으로 나가도 드래그 종료를 처리해야 함
	if (InputSystem::Get().GetGuiInputState().bUsingMouse && !Gizmo->IsHolding())
	{
		return;
	}

	const float ZoomSpeed = Settings ? Settings->CameraZoomSpeed : 300.f;

	float ScrollNotches = InputSystem::Get().GetScrollNotches();
	if (!bLightPerspectiveOverrideActive && ScrollNotches != 0.0f)
	{
		if (Camera->IsOrthogonal())
		{
			float NewWidth = Camera->GetOrthoWidth() - ScrollNotches * ZoomSpeed * DeltaTime;
			Camera->SetOrthoWidth(Clamp(NewWidth, 0.1f, 1000.0f));
		}
		else
		{
			//foot zoom 발줌은 절대 delta time를 곱하지 않음. 노치당 이동 거리가 일정해야 하기 때문.
			Camera->MoveLocal(FVector(ScrollNotches * ZoomSpeed*0.015f, 0.0f, 0.0f));
		}
	}

	// 마우스 좌표를 뷰포트 슬롯 로컬 좌표로 변환
	// (ImGui screen space = 윈도우 클라이언트 좌표)
	POINT MousePoint = InputSystem::Get().GetMousePos();
	MousePoint = Window->ScreenToClientPoint(MousePoint);

	float LocalMouseX = static_cast<float>(MousePoint.x) - ViewportScreenRect.X;
	float LocalMouseY = static_cast<float>(MousePoint.y) - ViewportScreenRect.Y;

	// 커서 숨김 제거: ShowCursor는 전역 레퍼런스 카운터라 멀티 뷰포트에서
	// active 전환 시 GetKeyUp이 처리되지 않아 커서가 영구 숨김될 수 있음

	// FViewport 크기 기준으로 디프로젝션 (슬롯 크기와 동기화됨)
	float VPWidth = Viewport ? static_cast<float>(Viewport->GetWidth()) : WindowWidth;
	float VPHeight = Viewport ? static_cast<float>(Viewport->GetHeight()) : WindowHeight;
	//성능 향상을 위해 필요할 때만 아래 분기에서 Ray 생성.
	FRay Ray = Camera->DeprojectScreenToWorld(LocalMouseX, LocalMouseY, VPWidth, VPHeight);
	FHitResult HitResult;

	// 기즈모 hovering 효과를 주석처리해 일단 fps를 개선합니다
	FRayUtils::RaycastComponent(Gizmo, Ray, HitResult);

	if (InputSystem::Get().GetKeyDown(VK_LBUTTON))
	{
		HandleDragStart(Ray);
	}
	else if (InputSystem::Get().GetLeftDragging())
	{
		//	눌려있고, Holding되지 않았다면 다음 Loop부터 드래그 업데이트 시작
		if (Gizmo->IsPressedOnHandle() && !Gizmo->IsHolding())
		{
			Gizmo->SetHolding(true);
		}

		if (Gizmo->IsHolding())
		{
			Gizmo->UpdateDrag(Ray);
		}
	}
	else if (InputSystem::Get().GetLeftDragEnd())
	{
		Gizmo->DragEnd();
	}
	else if (InputSystem::Get().GetKeyUp(VK_LBUTTON))
	{
		// 드래그 threshold 미달로 DragEnd가 호출되지 않는 경우 처리
		Gizmo->SetPressedOnHandle(false);
	}
}

void FEditorViewportClient::ApplyLightPerspectiveOverride()
{
	const bool bWasOverrideActive = bLightPerspectiveOverrideActive;
	bLightPerspectiveOverrideActive = false;

	if (!Camera || !SelectionManager)
	{
		if (bWasOverrideActive)
		{
			RestoreCameraStateAfterLightOverride();
			RestoreBillboardVisibilityAfterLightOverride();
			ClearDirectionalOverrideSnapshot();
		}
		return;
	}

	AActor* SelectedActor = SelectionManager->GetPrimarySelection();
	if (!SelectedActor)
	{
		if (bWasOverrideActive)
		{
			RestoreCameraStateAfterLightOverride();
			RestoreBillboardVisibilityAfterLightOverride();
			ClearDirectionalOverrideSnapshot();
		}
		return;
	}

	UWorld* World = SelectedActor->GetWorld();
	if (!World)
	{
		if (bWasOverrideActive)
		{
			RestoreCameraStateAfterLightOverride();
			RestoreBillboardVisibilityAfterLightOverride();
			ClearDirectionalOverrideSnapshot();
		}
		return;
	}

	if (!bWasOverrideActive)
	{
		SaveCameraStateForLightOverride();
	}

	const FSceneEnvironment& Environment = World->GetScene().GetEnvironment();
	if (TryApplyDirectionalLightOverride(SelectedActor, Environment))
	{
		bLightPerspectiveOverrideActive = true;
		ApplyBillboardVisibilityForLightOverride(SelectedActor);
		return;
	}

	if (TryApplySpotLightOverride(SelectedActor, Environment))
	{
		ClearDirectionalOverrideSnapshot();
		bLightPerspectiveOverrideActive = true;
		ApplyBillboardVisibilityForLightOverride(SelectedActor);
		return;
	}

	if (TryApplyPointLightOverride(SelectedActor, Environment))
	{
		ClearDirectionalOverrideSnapshot();
		bLightPerspectiveOverrideActive = true;
		ApplyBillboardVisibilityForLightOverride(SelectedActor);
		return;
	}

	if (bWasOverrideActive)
	{
		RestoreCameraStateAfterLightOverride();
		RestoreBillboardVisibilityAfterLightOverride();
		ClearDirectionalOverrideSnapshot();
	}
}

void FEditorViewportClient::ApplyBillboardVisibilityForLightOverride(AActor* SelectedActor)
{
	if (!SelectedActor)
	{
		return;
	}

	if (BillboardOverrideActor == SelectedActor && !BillboardVisibilityBackups.empty())
	{
		return;
	}

	RestoreBillboardVisibilityAfterLightOverride();

	for (UActorComponent* Component : SelectedActor->GetComponents())
	{
		UBillboardComponent* Billboard = Cast<UBillboardComponent>(Component);
		if (!Billboard)
		{
			continue;
		}

		FBillboardVisibilityBackup Backup;
		Backup.Component = Billboard;
		Backup.bWasVisible = Billboard->IsVisible();
		BillboardVisibilityBackups.push_back(Backup);

		if (Backup.bWasVisible)
		{
			Billboard->SetVisibility(false);
		}
	}

	if (!BillboardVisibilityBackups.empty())
	{
		BillboardOverrideActor = SelectedActor;
	}
}

void FEditorViewportClient::RestoreBillboardVisibilityAfterLightOverride()
{
	for (const FBillboardVisibilityBackup& Backup : BillboardVisibilityBackups)
	{
		if (Backup.Component)
		{
			Backup.Component->SetVisibility(Backup.bWasVisible);
		}
	}

	BillboardVisibilityBackups.clear();
	BillboardOverrideActor = nullptr;
}

void FEditorViewportClient::SaveCameraStateForLightOverride()
{
	if (!Camera)
	{
		return;
	}

	const FCameraState& CameraState = Camera->GetCameraState();
	SavedCameraWorldLocation = Camera->GetWorldLocation();
	SavedCameraEulerRotation = Camera->GetRelativeRotation().ToVector();
	SavedCameraFOV = CameraState.FOV;
	SavedCameraOrthoWidth = CameraState.OrthoWidth;
	bSavedCameraIsOrthographic = CameraState.bIsOrthogonal;
	bHasSavedCameraStateForLightOverride = true;
}

void FEditorViewportClient::RestoreCameraStateAfterLightOverride()
{
	if (!Camera || !bHasSavedCameraStateForLightOverride)
	{
		return;
	}

	Camera->SetWorldLocation(SavedCameraWorldLocation);
	Camera->SetRelativeRotation(SavedCameraEulerRotation);

	FCameraState CameraState = Camera->GetCameraState();
	CameraState.FOV = SavedCameraFOV;
	CameraState.OrthoWidth = SavedCameraOrthoWidth;
	CameraState.bIsOrthogonal = bSavedCameraIsOrthographic;
	Camera->SetCameraState(CameraState);

	bHasSavedCameraStateForLightOverride = false;
}

void FEditorViewportClient::ApplyCameraFromShadowView(const FShadowViewData& ShadowView, bool bOrthographic)
{
	if (!Camera)
	{
		return;
	}

	// LightView는 MakeViewMatrix(F, R, U, E)로 만들어지며,
	// 회전축은 column(0=R, 1=U, 2=F)에 들어간다.
	// 카메라 월드 회전은 USceneComponent 컨벤션(row0=Forward, row1=Right, row2=Up)에 맞춰
	// 축을 직접 복원해서 적용해야 축/회전 불일치가 발생하지 않는다.
	FVector Right(
		ShadowView.LightView.M[0][0],
		ShadowView.LightView.M[1][0],
		ShadowView.LightView.M[2][0]);
	FVector Up(
		ShadowView.LightView.M[0][1],
		ShadowView.LightView.M[1][1],
		ShadowView.LightView.M[2][1]);
	FVector Forward(
		ShadowView.LightView.M[0][2],
		ShadowView.LightView.M[1][2],
		ShadowView.LightView.M[2][2]);

	Right.Normalize();
	Up.Normalize();
	Forward.Normalize();

	// View matrix의 translation은 [-E·R, -E·U, -E·F] 이므로
	// 월드 위치 E는 축 기저로 역복원한다.
	const float DotER = ShadowView.LightView.M[3][0];
	const float DotEU = ShadowView.LightView.M[3][1];
	const float DotEF = ShadowView.LightView.M[3][2];
	const FVector CameraWorldLocation =
		(Right * -DotER) +
		(Up * -DotEU) +
		(Forward * -DotEF);

	FMatrix CameraWorldRotation = FMatrix::Identity;
	CameraWorldRotation.M[0][0] = Forward.X;
	CameraWorldRotation.M[0][1] = Forward.Y;
	CameraWorldRotation.M[0][2] = Forward.Z;
	CameraWorldRotation.M[1][0] = Right.X;
	CameraWorldRotation.M[1][1] = Right.Y;
	CameraWorldRotation.M[1][2] = Right.Z;
	CameraWorldRotation.M[2][0] = Up.X;
	CameraWorldRotation.M[2][1] = Up.Y;
	CameraWorldRotation.M[2][2] = Up.Z;

	Camera->SetWorldLocation(CameraWorldLocation);
	Camera->SetRelativeRotation(CameraWorldRotation.ToRotator());

	FCameraState CameraState = Camera->GetCameraState();
	CameraState.bIsOrthogonal = bOrthographic;

	if (bOrthographic)
	{
		const float OrthoScaleX = ShadowView.LightProj.M[0][0];
		if (std::abs(OrthoScaleX) > 1e-5f)
		{
			CameraState.OrthoWidth = std::abs(2.0f / OrthoScaleX);
		}
	}
	else
	{
		const float CotHalfFov = ShadowView.LightProj.M[1][1];
		if (std::abs(CotHalfFov) > 1e-5f)
		{
			CameraState.FOV = 2.0f * std::atan(1.0f / CotHalfFov);
		}
	}

	Camera->SetCameraState(CameraState);
}

bool FEditorViewportClient::TryApplyDirectionalLightOverride(AActor* SelectedActor, const FSceneEnvironment& Environment)
{
	UDirectionalLightComponent* SelectedDirectional = nullptr;
	for (UActorComponent* Component : SelectedActor->GetComponents())
	{
		SelectedDirectional = Cast<UDirectionalLightComponent>(Component);
		if (SelectedDirectional)
		{
			break;
		}
	}

	if (!SelectedDirectional || !Environment.HasGlobalDirectionalLight())
	{
		return false;
	}

	const FGlobalDirectionalLightParams& Directional = Environment.GetGlobalDirectionalLightParams();
	if (!Directional.ShadowData.Settings.bCastShadows || !Directional.ShadowData.bOverrideCameraWithLight)
	{
		ClearDirectionalOverrideSnapshot();
		return false;
	}

	const FShadowRuntimeOptions& ShadowOptions = GEngine->GetRenderer().GetRuntimeOptions();
	const int32 ActiveCascadeCount = (ShadowOptions.DirectionalShadowMode == EDirectionalShadowMode::Single)
		? 1
		: FDirectionalShadowData::NUM_CASCADES;

	int32 SelectedCascadeIndex = Directional.ShadowData.PreviewViewIndex;
	if (SelectedCascadeIndex < 0)
	{
		SelectedCascadeIndex = 0;
	}
	else if (SelectedCascadeIndex >= ActiveCascadeCount)
	{
		SelectedCascadeIndex = ActiveCascadeCount - 1;
	}

	const FShadowViewData& LiveView = Directional.ShadowData.View[SelectedCascadeIndex];
	auto IsFiniteMatrix = [](const FMatrix& Matrix) -> bool
	{
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Col = 0; Col < 4; ++Col)
			{
				if (!std::isfinite(Matrix.M[Row][Col]))
				{
					return false;
				}
			}
		}
		return true;
	};

	const FVector Right(
		LiveView.LightView.M[0][0],
		LiveView.LightView.M[1][0],
		LiveView.LightView.M[2][0]);
	const FVector Up(
		LiveView.LightView.M[0][1],
		LiveView.LightView.M[1][1],
		LiveView.LightView.M[2][1]);
	const FVector Forward(
		LiveView.LightView.M[0][2],
		LiveView.LightView.M[1][2],
		LiveView.LightView.M[2][2]);

	const bool bLiveViewValid =
		!LiveView.LightView.IsIdentity() &&
		!LiveView.LightProj.IsIdentity() &&
		!LiveView.LightViewProj.IsIdentity() &&
		IsFiniteMatrix(LiveView.LightView) &&
		IsFiniteMatrix(LiveView.LightProj) &&
		IsFiniteMatrix(LiveView.LightViewProj) &&
		Right.Length() > 1e-5f &&
		Up.Length() > 1e-5f &&
		Forward.Length() > 1e-5f;

	// Directional override 안정화:
	// Override 진입 시점의 마지막 유효 View/Proj를 스냅샷으로 저장하고,
	// Override 활성 중에는 그 스냅샷을 카메라에 재적용해 카메라-그림자 상호 피드백을 끊는다.
	if (!bHasDirectionalOverrideSnapshot
		|| DirectionalOverrideSnapshotActor != SelectedActor
		|| DirectionalOverrideSnapshotViewIndex != SelectedCascadeIndex)
	{
		if (!bLiveViewValid)
		{
			// 아직 directional shadow view가 계산되지 않은 프레임에서는
			// 초기값(identity) 스냅샷을 잡지 않는다. (원점 점프 방지)
			return false;
		}

		DirectionalOverrideSnapshotLightView = LiveView.LightView;
		DirectionalOverrideSnapshotLightProj = LiveView.LightProj;
		DirectionalOverrideSnapshotLightViewProj = LiveView.LightViewProj;
		DirectionalOverrideSnapshotActor = SelectedActor;
		DirectionalOverrideSnapshotViewIndex = SelectedCascadeIndex;
		bHasDirectionalOverrideSnapshot = true;
	}
	else if (DirectionalOverrideSnapshotLightViewProj.IsIdentity() && bLiveViewValid)
	{
		// 과거에 잘못된 스냅샷(identity)이 잡힌 경우 자동 교정
		DirectionalOverrideSnapshotLightView = LiveView.LightView;
		DirectionalOverrideSnapshotLightProj = LiveView.LightProj;
		DirectionalOverrideSnapshotLightViewProj = LiveView.LightViewProj;
		DirectionalOverrideSnapshotViewIndex = SelectedCascadeIndex;
	}

	FShadowViewData SnapshotView = {};
	SnapshotView.LightView = DirectionalOverrideSnapshotLightView;
	SnapshotView.LightProj = DirectionalOverrideSnapshotLightProj;
	SnapshotView.LightViewProj = DirectionalOverrideSnapshotLightViewProj;
	ApplyCameraFromShadowView(SnapshotView, true);
	return true;
}

bool FEditorViewportClient::TryApplySpotLightOverride(AActor* SelectedActor, const FSceneEnvironment& Environment)
{
	USpotLightComponent* SelectedSpot = nullptr;
	for (UActorComponent* Component : SelectedActor->GetComponents())
	{
		SelectedSpot = Cast<USpotLightComponent>(Component);
		if (SelectedSpot)
		{
			break;
		}
	}

	if (!SelectedSpot)
	{
		return false;
	}

	const FSpotLightParams* Spot = Environment.FindSpotLight(SelectedSpot);
	if (!Spot || !Spot->ShadowData.Settings.bCastShadows || !Spot->ShadowData.bOverrideCameraWithLight)
	{
		return false;
	}

	ApplyCameraFromShadowView(Spot->ShadowData.View, false);
	return true;
}

bool FEditorViewportClient::TryApplyPointLightOverride(AActor* SelectedActor, const FSceneEnvironment& Environment)
{
	UPointLightComponent* SelectedPoint = nullptr;
	for (UActorComponent* Component : SelectedActor->GetComponents())
	{
		SelectedPoint = Cast<UPointLightComponent>(Component);
		if (SelectedPoint)
		{
			break;
		}
	}

	if (!SelectedPoint)
	{
		return false;
	}

	const FPointLightParams* Point = Environment.FindPointLight(SelectedPoint);
	if (!Point || !Point->ShadowData.Settings.bCastShadows || !Point->ShadowData.bOverrideCameraWithLight)
	{
		return false;
	}

	int32 FaceIndex = Point->ShadowData.PreviewViewIndex;
	if (FaceIndex < 0)
	{
		FaceIndex = 0;
	}
	else if (FaceIndex > 5)
	{
		FaceIndex = 5;
	}

	ApplyCameraFromShadowView(Point->ShadowData.View[FaceIndex], false);
	return true;
}

void FEditorViewportClient::ClearDirectionalOverrideSnapshot()
{
	bHasDirectionalOverrideSnapshot = false;
	DirectionalOverrideSnapshotActor = nullptr;
	DirectionalOverrideSnapshotViewIndex = -1;
	DirectionalOverrideSnapshotLightView = FMatrix::Identity;
	DirectionalOverrideSnapshotLightProj = FMatrix::Identity;
	DirectionalOverrideSnapshotLightViewProj = FMatrix::Identity;
}

/**
 * Picking , 마우스 좌클릭 시 Gizmo 핸들과의 충돌을 우선적으로 검사하며 드래그 시작 여부 결정
 * 
 * \param Ray
 */
void FEditorViewportClient::HandleDragStart(const FRay& Ray)
{
	FScopeCycleCounter PickCounter; //시간측정용 카운터 시작

	FHitResult HitResult{};
	//먼저 Ray와 기즈모의 충돌을 감지하고 
	if (FRayUtils::RaycastComponent(Gizmo, Ray, HitResult))
	{
		Gizmo->SetPressedOnHandle(true);
	}
	else
	{
		//기즈모와 충돌하지 않았다면 월드 BVH를 통해 가장 가까운 프리미티브를 찾음
		AActor* BestActor = nullptr;
		if (UWorld* W = GetWorld())
		{
			W->RaycastPrimitives(Ray, HitResult, BestActor); //BVH 시작
		}

		//멀티픽킹은 성능을 위해 일단 비활성화
		//bool bCtrlHeld = InputSystem::Get().GetKey(VK_CONTROL);

		if (BestActor == nullptr)
		{
				SelectionManager->ClearSelection();
		}
		else
		{
				// if (bCtrlHeld)
				// {
				// 	SelectionManager->ToggleSelect(BestActor);
				// }
				// else
				{
					SelectionManager->Select(BestActor);
				}
		}
	}

	if (OverlayStatSystem)
	{
		const uint64 PickCycles = PickCounter.Finish();
		const double ElapsedMs = FPlatformTime::ToMilliseconds(PickCycles);
		OverlayStatSystem->RecordPickingAttempt(ElapsedMs);
	}
}

void FEditorViewportClient::UpdateLayoutRect()
{
	if (!LayoutWindow) return;

	const FRect& R = LayoutWindow->GetRect();
	ViewportScreenRect = R;

	// FViewport 리사이즈 요청 (슬롯 크기와 RT 크기 동기화)
	if (Viewport)
	{
		uint32 SlotW = static_cast<uint32>(R.Width);
		uint32 SlotH = static_cast<uint32>(R.Height);
		if (SlotW > 0 && SlotH > 0 && (SlotW != Viewport->GetWidth() || SlotH != Viewport->GetHeight()))
		{
			Viewport->RequestResize(SlotW, SlotH);
		}
	}
}

void FEditorViewportClient::RenderViewportImage(bool bIsActiveViewport)
{
	if (!Viewport || !Viewport->GetSRV()) return;

	const FRect& R = ViewportScreenRect;
	if (R.Width <= 0 || R.Height <= 0) return;

	ImDrawList* DrawList = ImGui::GetWindowDrawList();
	ImVec2 Min(R.X, R.Y);
	ImVec2 Max(R.X + R.Width, R.Y + R.Height);

	DrawList->AddImage((ImTextureID)Viewport->GetSRV(), Min, Max);

	// 활성 뷰포트 테두리 강조
	if (bIsActiveViewport)
	{
		DrawList->AddRect(Min, Max, IM_COL32(255, 200, 0, 200), 0.0f, 0, 2.0f);
	}
}

bool FEditorViewportClient::GetCursorViewportPosition(uint32& OutX, uint32& OutY) const
{
	if (!bIsActive) return false;

	ImVec2 MousePos = ImGui::GetIO().MousePos;
	float LocalX = MousePos.x - ViewportScreenRect.X;
	float LocalY = MousePos.y - ViewportScreenRect.Y;

	if (LocalX >= 0 && LocalY >= 0 && LocalX < ViewportScreenRect.Width && LocalY < ViewportScreenRect.Height)
	{
		OutX = static_cast<uint32>(LocalX);
		OutY = static_cast<uint32>(LocalY);
		return true;
	}
	return false;
}
