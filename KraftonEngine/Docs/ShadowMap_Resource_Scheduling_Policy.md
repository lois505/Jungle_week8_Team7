# Shadow Map Resource / Scheduling / Caster Culling Policy

## 목적
- Shadow 시스템을 PCF / VSM / ESM, Directional / Local, PSM / CSM, Atlas Allocation, Scheduling, Caster Culling까지 확장하기 위한 기준 문서.
- 컨텍스트가 바뀌거나 담당자가 바뀌어도 이 문서를 기준으로 일관되게 개발.

## 최우선 원칙
- 현재 Render Pass / Renderer / Shadow 호출 흐름을 최대한 유지.
- 기존 구조 위에 정책과 경계를 점진적으로 추가.
- 가독성 우선: 역할 기반 class/struct/member function 중심으로 작성.
- `FShadowResourceManager`가 생성/리사이즈/재할당 + 스케줄링 정책을 담당.

---

## 필수 보정 사항 (반영 완료)
1. **VSM/ESM Ping-Pong SRV/RTV 규칙**
   - MomentA/MomentB는 pass마다 SRV/RTV 역할이 바뀜.
   - 같은 pass에서 동일 texture를 SRV+RTV로 동시에 바인딩 금지.
   - Pass boundary에서 unbind/bind를 명시적으로 관리.

2. **Directional은 Local Atlas Scheduler 대상 아님**
   - Directional PSM/CSM은 전용 Resource Policy로 관리.
   - Local Atlas Allocator/Eviction/Repack 대상은 Spot/Point만 포함.
   - Local `ShadowRequest`에는 Directional을 포함하지 않음.

3. **Directional Texture2DArray[5]는 1차 단순화 정책**
   - Slice 0 = PSM, Slice 1~4 = CSM.
   - 후속 최적화에서 PSM 분리 + CSM Array[4] 분리 가능.

4. **ESM 포맷 최적화 경로 유지**
   - 초기에는 VSM 경로 공유를 위해 `R32G32_FLOAT` 사용 가능.
   - 후속에서 `R32_FLOAT` 단일 채널 최적화 가능하도록 설계.

---

## 현재 프로젝트 매핑

### 핵심 파일
- `KraftonEngine/Source/Engine/Render/Pipeline/Renderer.cpp`
- `KraftonEngine/Source/Engine/Render/Pipeline/ShadowRenderer.h`
- `KraftonEngine/Source/Engine/Render/Pipeline/ShadowRenderer.cpp`
- `KraftonEngine/Source/Engine/Render/Resource/ShadowResourceManager.h`
- `KraftonEngine/Source/Engine/Render/Resource/ShadowResourceManager.cpp`
- `KraftonEngine/Source/Engine/Render/Resource/ShadowAtlasResource.h`
- `KraftonEngine/Source/Engine/Render/Types/ShadowData.h`
- `KraftonEngine/Source/Engine/Render/Resource/RenderResources.cpp`
- `KraftonEngine/Shaders/Common/SystemResources.hlsli`
- `KraftonEngine/Shaders/Common/ForwardLighting.hlsli`
- `KraftonEngine/Shaders/Shadow/CommonShadowMap.hlsl`
- `KraftonEngine/Shaders/Shadow/MomentShadowMap.hlsl`

### 핵심 클래스/구조체
- `FShadowResourceManager`
- `FShadowRenderer`
- `FShadowAtlasResource`, `FShadowMapResource`
- `FDirectionalShadowArray`
- `FShadowViewData`, `FDirectionalShadowData`, `FPointShadowData`, `FSpotShadowData`
- `FShadowRuntimeOptions`

### 현재 실행 흐름
1. `FRenderer::Render`
   - `Resources.UnbindSystemTextures(...)`
   - `Resources.UpdateShadowResources(...)`
   - `Resources.ShadowResourceManager.ClearAtlas(...)`
   - `ShadowRenderer.RenderShadows(...)`
2. `FShadowResourceManager`
   - Local Atlas/Directional Array 자원 생성, 리사이즈, 유지.
3. `FShadowRenderer`
   - View별 RT/DS 바인딩 + Shadow Draw.
4. `ForwardLighting.hlsli`
   - Filter mode 기반 sampling 분기.

---

## Phase 계획 요약
1. Resource Path / Pass Boundary / Filtering Policy 골격
2. Local Light PCF/VSM/ESM 안정화
3. Directional PSM 안정화
4. CSM 확장
5. 단순 Atlas Allocator
6. Scheduling / Budget / Priority
7. 고급 Allocator / Eviction / Repack
8. Shadow Caster Culling / Light별 Proxy Selection

---

## Phase 1 (현재 진행)

### 목표
- 동작을 크게 바꾸지 않고 골격 추가:
  - Local ShadowRequest 구조
  - RequestedResolution / AppliedResolution 분리 기록
  - Shadow Telemetry 기본 집계
  - Directional vs Local 책임 분리의 코드 기반 마련

### 이번 단계 수정 범위
- `ShadowResourceManager` 내부에 아래 개념 추가
  - `ELocalShadowRequestType` (Spot / PointFace)
  - `FShadowResolutionRecord`
  - `FLocalShadowRequest` (Local 전용)
  - `FShadowTelemetry`
  - `BuildLocalShadowRequests`
  - `ResolveLocalShadowRequests`
  - `UpdateTelemetry`

### 이번 단계에서 하지 않는 것
- Free-Rect allocator
- Eviction/Repack/Hysteresis
- Point face 최적화
- Ping-Pong blur pass 본 구현
- CSM 경계 블렌딩

### 위험/주의
- Telemetry는 1차 버전으로, 일부 수치(예: 실제 렌더 수)는 다음 단계에서 정밀화 필요.
- 기존 렌더 결과를 바꾸지 않도록 scheduler는 현재 정렬/기록 중심으로만 동작.

---

## 작업 순서 규칙
1. 현재 구조 조사
2. 수정 범위 제안
3. 사용자 확인
4. 구현
5. 빌드/동작 검증
6. 문서 업데이트

## 구현 스타일 규칙
- 무분별한 lambda/free function 추가 지양.
- 역할이 명확한 class/struct/member function 중심.
- Resource 생성/스케줄링/렌더링/디버그를 한 함수에 과도하게 혼합하지 않음.

---

## Phase 1 진행 로그

### 완료
- `ShadowResourceManager`에 Local 전용 request/telemetry 골격 추가.
  - `ELocalShadowRequestType`
  - `FShadowResolutionRecord`
  - `FLocalShadowRequest`
  - `FShadowTelemetry`
- `UpdateShadowResources`에서 아래 순서로 Phase 1 골격 연결.
  - `BuildLocalShadowRequests`
  - `ResolveLocalShadowRequests`
  - 기존 Ensure*Shadow 경로
  - `UpdateTelemetry`
- `ClearAtlas` 의미 분리(동작 유지).
  - `ClearAtlasTextures(...)`
  - `ResetAtlasAllocationState()`
- `ShadowRenderer` pass boundary 명시화.
  - Shadow 쓰기 직전 `ShadowMapAtlas` / `DirectionalShadowArray` SRV를 VS/PS/CS에서 unbind.
  - Shadow view 렌더 직후 OM 타깃 unbind 처리.
  - 목적: 동일 texture의 SRV/RTV(또는 DSV) 동시 바인딩 hazard 방지.
- Phase 1 telemetry 조회 경로를 Editor Overlay Stat에 연결.
  - `Renderer`에서 `GetShadowTelemetry()` accessor 제공.
  - Overlay에서 ShadowTechnique / DirectionalMode / Resolution / LocalViewCount / EstimatedShadowVRAM 표시.
- Resolution Policy 골격 추가.
  - `FShadowResolutionPolicy` 도입( Base / Min / Max / Alignment ).
  - `ComputeRequestedResolution(...)`로 Directional/Local 분리 계산 경로 확보.
  - 현재는 동작 안정성을 위해 기본값을 기존 수준(정렬 1)으로 유지.
- Local Atlas 할당 책임 이동.
  - `ShadowRenderer`에서 직접 `AllocateFromAtlas()` 하던 로직 제거.
  - `ShadowResourceManager::AllocateLocalShadowViews(...)`에서 Spot/PointFace 할당 수행.
  - Renderer는 할당된 결과를 소비해 렌더만 수행.
- Unlit Shadow Pass 옵션화.
  - `FShadowRuntimeOptions::bSkipShadowPassInUnlit` 추가.
  - `Renderer`에서 Unlit + 옵션 ON일 때 shadow pass 전체 skip.
  - UI/설정(`FLevelViewportLayout`, `EditorSettings`)에서 토글 및 저장/복원 지원.
  - skip 프레임에서 per-frame shadow telemetry 카운터 초기화 처리.
- Telemetry 정밀화.
  - `SubmittedShadowViewCount`, `FailedByAtlasAllocationCount`,
    `FailedByInvalidViewCount`, `FailedByBudgetCount`,
    `ExecutedShadowPassCount`, `bSkippedInUnlitView` 추가.
  - 현재 budget 값(`MaxLocalShadowViewsPerFrame`)도 telemetry에 포함.
  - Overlay Stat에 위 항목 표시.
- Scheduler 확장 준비.
  - `ComputeLocalShadowPriority(...)` 추가.
  - 이전 프레임 할당 유지 보너스(residency bonus) 반영.
  - `MaxLocalShadowViewsPerFrame`(기본 0=무제한) 골격 추가.
  - Editor UI/Settings 연동으로 런타임 budget 값 조절 가능.
  - Console 명령 추가: `shadow show`, `shadow filter`, `shadow dirmode`, `shadow skip_unlit`, `shadow max_views`.
- Phase 2 준비 (비동작 골격).
  - `FShadowAtlasResource`에 Ping-Pong temp atlas 슬롯(`FilterTempMap`) placeholder 추가.
  - VSM/ESM 모드에서 `FilterTempMap` 생성/해제/클리어까지 연결 완료.
  - 아직 Blur pass에서 읽기/쓰기 경로는 미연결(자원 생명주기만 준비).
- Shadow pass 병목 추적 보강.
  - `Shadow.UnbindSystemTextures`
  - `Shadow.UpdateResources`
  - `Shadow.ClearAtlas`
  - `Shadow.RenderViews`
  - 위 구간별 STAT 분리 추가.
- 빌드 검증 시도 완료.
  - `MSBuild` 실행은 되었으나 로컬 환경 변수 충돌(`Path` vs `PATH`)로 컴파일 단계 진입 전 실패.
  - 코드 컴파일 오류 확인 단계는 환경 정리 후 재실행 필요.

### 아직 안 한 것
- Local request 기반 실제 budget cut/scheduler 적용.
- Ping-Pong blur pass(H/V) 구현.
- Free-Rect allocator / eviction / repack.
- Directional PSM/CSM 분리 최적화.
## Phase 1 Progress Update (2026-04-28)

### Completed in this step
- Local atlas allocation policy switched from fixed 512 tiles to request-driven variable tiles.
  - requested resolution now comes from `FShadowResolutionPolicy`
  - allocation retries with downgrade (requested -> ... -> min)
  - row-based packing now supports variable tile sizes
- Local/Directional default resolution policies aligned to current team baseline:
  - Local: base 512 / min 256 / max 1024 / align 256
  - Directional: base 1024 / min 1024 / max 2048 / align 256

- Directional mode wiring (`Single` vs `CSM`) now affects runtime path counts consistently:
  - directional render cascade count
  - directional moment blur pass count
  - submitted shadow view telemetry count
  - directional shadow constant buffer cascade count
- VSM/ESM lighting sampling removed duplicated 3x3 Gaussian in `ForwardLighting.hlsli`.
  - blur responsibility remains in shadow filter pass (H/V), not in lighting sampling.
- Shadow telemetry split was added:
  - `LocalBlurPassCount`
  - `DirectionalBlurPassCount`
  - `BlurPassCount` (total)
- Overlay stat line added:
  - `Blur Passes (Total/Local/Directional)`
- Shadow blur CPU stat scopes were added in `ShadowRenderer`:
  - `Shadow.LocalBlur`, `Shadow.LocalBlur.H`, `Shadow.LocalBlur.V`
  - `Shadow.DirectionalBlur`, `Shadow.DirectionalBlur.H`, `Shadow.DirectionalBlur.V`

### Build verification
- `Debug|x64` build succeeded after this change set (errors: 0).
- Existing `C4099` warnings from `ShadowAtlasResource.h` remain unchanged.

### Next step (safe, phase-1 aligned)
- keep behavior stable and only add observability:
  - validate runtime stat values over several scenes and light counts
  - add brief troubleshooting notes for interpreting blur stats
- defer allocator/scheduler behavior changes to next phase.

## Phase 1 Progress Update (2026-04-28, follow-up)

### Completed in this step
- Shadow settings popup now exposes local atlas area budget directly:
  - `Max Local Shadow Atlas Area Per Frame (0=NoLimit, Pixels)`
  - live telemetry line: `Local Atlas Area Used/Max`
- Scheduler helper logic was normalized without behavior change:
  - added `ClampAndAlignResolution(...)`
  - added `ComputeNextLowerResolution(...)`
  - removed duplicated inline/lambda resolution downgrade logic from allocation loop
- Added scheduler observability for policy tuning:
  - `DowngradedLocalViewCount`
  - `LocalAllocationRetryCount`
  - overlay + console output wiring (`shadow show`)
- Added minimum hysteresis/grace-frame stabilization skeleton for local requests:
  - per-request persistent state map (`allocated/fail streak`)
  - grace-window allocation residency signal for sorting stability
  - telemetry: `GraceProtectedRequestCount`
- Added runtime tuning path for stabilization policy:
  - UI (`Light Culling > Shadow`): `Local Shadow Grace Frames`, `Local Shadow Grace Bonus`
  - console: `shadow grace_frames <N>`, `shadow grace_bonus <Float>`
  - settings serialization/load for both fields
- Upgraded local request scoring model (policy step):
  - priority now reflects `LightIntensity`, `AttenuationRadius`, `RequestedResolution`, `RequestType`, and residency
  - budget score now uses area-like cost (`resolution^2`) instead of linear resolution cost

### Why this step
- keeps current behavior stable while improving readability and policy traceability
- reduces friction before next allocator/scheduler phase (`align 64`, free-rect, hysteresis)

### Build verification
- `Debug|x64` build success (errors: 0)
- existing repeated `C4099` warnings remain unchanged

## Phase 1 Progress Update (2026-04-28, score-gating)

### Completed in this step
- Added scheduler score-threshold policy knobs for local shadow requests:
  - `LocalShadowAcquireBudgetScoreThreshold`
  - `LocalShadowRetainBudgetScoreThreshold`
- Allocation flow now supports score-based early rejection before budget/atlas attempts:
  - non-grace requests below threshold are skipped
  - grace-protected requests can bypass threshold within grace window
- Added explicit failure reason telemetry for score gating:
  - `FailedByScoreCount`
- Runtime control wiring completed:
  - UI (`Light Culling > Shadow`): acquire/retain score thresholds
  - Console:
    - `shadow score_acquire <Float>`
    - `shadow score_retain <Float>`
  - Settings save/load for both values
  - Overlay stat lines for `Failed By Score` and `Local Score Threshold (Acquire/Retain)`

### Why this step
- This keeps allocator behavior simple while making scheduling policy tunable and observable.
- It provides a safe bridge toward next-phase hysteresis/budget refinement without introducing free-rect/eviction complexity yet.

## Phase 1 Progress Update (2026-04-28, score normalization + draw-call telemetry)

### Completed in this step
- Local budget score formula was normalized to policy scale:
  - before: `Priority / (Resolution^2 * TypeCost)` (too small to tune in practice)
  - now: `Priority / ((Resolution/BaseResolution)^2 * TypeCost)` (stable, human-tunable range)
- Added shadow draw-call telemetry path:
  - count shadow draw submissions in `ShadowRenderer`
  - propagate to telemetry as `ShadowDrawCallCount`
  - show in overlay and `shadow show` console output

### Why this step
- Score-threshold policy (`acquire/retain`) now has practical numeric behavior.
- Scheduler tuning can now observe not only view count/area budget but actual shadow draw-call pressure.

## Phase 1 Progress Update (2026-04-28, retain-first scheduling)

### Completed in this step
- Local request queue now explicitly uses retain-first scheduling order:
  - partition requests into retain candidates and acquire candidates
  - sort each group by budget score / priority / requested resolution
  - process retain group before acquire group for better temporal stability
- Added scheduler telemetry breakdown:
  - candidate counts: retain/acquire
  - allocated counts: retain/acquire
  - rejected counts: retain/acquire
  - exposed in overlay and `shadow show`

### Why this step
- Reduces frame-to-frame shadow churn under tight budget.
- Keeps current allocator intact while making scheduling behavior observable and tunable.

## Phase 1 Progress Update (2026-04-28, alignment runtime policy)

### Completed in this step
- Local shadow alignment is now a runtime policy value (not hardcoded-only):
  - exposed in UI, console, and settings serialization
  - applied through `ShadowResourceManager::SetLocalShadowAlignment`
  - reflected in telemetry (`LocalShadowAlignment`)

### Why this step
- Enables safe 256->64 alignment experiments without additional code edits.
- Prepares allocator policy transition while preserving current row allocator behavior.

## Phase 1 Progress Update (2026-04-28, row-allocator quality telemetry)

### Completed in this step
- Added row-atlas quality telemetry for Local Shadow Atlas:
  - `LocalAtlasRowFootprintArea`
  - `LocalAtlasRowWasteArea`
  - `LocalAtlasLargestFreeRectArea` (row-allocator proxy)
  - `LocalAtlasPackingEfficiency`
  - `LocalAtlasRowCount`
- Exposed values to overlay + `shadow show` console output.

### Why this step
- Provides practical criteria for deciding when row allocator starts to degrade.
- Gives objective baseline before moving to free-rect allocator in next phase.

## Phase 1 Progress Update (2026-04-28, allocator-mode runtime skeleton)

### Completed in this step
- Added explicit local allocator mode skeleton:
  - `ELocalShadowAllocatorMode { Row, FreeRect }`
  - current `AllocateFromAtlas(...)` switches by mode
  - `FreeRect` is intentionally mapped to row fallback for now (phase-safe, behavior-stable)
- Added runtime wiring for allocator mode:
  - renderer wrapper API
  - settings save/load
  - viewport shadow settings combo
  - console command:
    - `shadow alloc_mode <row|freerect>`
  - `shadow show` now prints `shadow.alloc_mode=...`
- Added telemetry field + overlay line:
  - `LocalShadowAllocatorMode`
  - overlay shows current mode

### Why this step
- Keeps phase-1 behavior stable while exposing the mode boundary needed for phase-4 free-rect rollout.
- Lets the team validate control paths now, before allocator behavior diverges.

## Phase 1 Progress Update (2026-04-28, policy-first free-rect allocator)

### Completed in this step
- Local atlas `FreeRect` mode now has real allocation behavior (no longer row fallback).
  - best-short-side-fit style free-rect pick
  - guillotine split into residual rects
  - contained-rect prune
  - adjacent-rect merge (horizontal/vertical)
- FreeRect maintenance cost was batched:
  - removed per-allocation prune/merge
  - added frame-level compaction pass after local allocation loop
  - keeps insert path light and moves heavy cleanup to one batched step
- Local scheduler execution is now explicitly batched by request class:
  - pass 1: retain candidates
  - pass 2: acquire candidates
  - policy intent is now independent from incidental sort order
- `ResetAtlasAllocationState()` now initializes free-rect pool from full atlas size.
- `SetLocalShadowAllocatorMode(...)` now resets allocation state on mode change.
- Allocation telemetry path now branches by allocator mode:
  - Row mode keeps existing row metrics
  - FreeRect mode reports largest free rect from actual free-rect pool

### Why this step
- This is a direct policy implementation step (allocator behavior), not a UI/debug expansion.
- It enables next scheduling policies to run on top of real free-rect allocation results.

## Phase 1 Progress Update (2026-04-28, policy batch: directional boundary + local fallback)

### Completed in this step
- Directional slice policy was wired through render/blur/sampling paths:
  - `Single` mode uses slice `0` (PSM slot)
  - `CSM` mode uses slices `1~4`
- Directional array resource creation now builds slice views for `0..NumCascades`
  - DSV / preview SRV / moment RTV / moment temp RTV
- Single directional projection now uses full near~far range in cascade build path.
- Local point shadow fallback policy fixed:
  - removed all-face AND gate (`CastShadow &= faceAllocated`)
  - unallocated face now falls back via invalid atlas rect path (`visibility=1.0`) per-face

### Why this step
- Closes two core policy gaps without adding new tuning surface:
  - directional PSM/CSM slice ownership
  - point face-level allocation failure fallback

## Phase 1 Progress Update (2026-04-28, policy batch: lifecycle + clear semantics)

### Completed in this step
- Directional resource lifecycle policy was applied to allocation/recreate path:
  - `Single` mode requests directional array with `NumCascades=0` (PSM-only slice set)
  - `CSM` mode requests directional array with `NumCascades=4`
  - moment resources are now recreated by filter policy (`PCF` no-moment, `VSM/ESM` with-moment)
- Recreate trigger now includes directional moment-resource mode change.
- Clear semantics were split at render call site:
  - `ClearAtlasTexturesOnly(...)`
  - `ResetAtlasAllocationStateForFrame()`
  - old `ClearAtlas(...)` remains as compatibility wrapper.
- Local failure-state rule was tightened:
  - when request is rejected/fails, `AppliedResolution` is forced to `0`
  - keeps Requested/Applied semantics consistent for fallback paths.
- Local view state reset rule was fixed:
  - Spot/Point atlas rect fields are reset to zero at frame allocation start
  - avoids stale atlas rect state on allocation failure.

### Why this step
- Finishes policy boundaries for resource lifetime and per-frame clear intent
  without introducing new tuning logic.

## Follow-up Tasks (2026-04-28)

- Directional Light Caster Culling
  - Local scheduler 대상이 아닌 Directional 전용 경로에서 caster culling을 별도 적용.
  - 현재 진행된 local(point/spot) 얇은 caster 필터링과 책임이 섞이지 않도록 분리.

- Directional/Local Caster 수집 자료구조 정리
  - Main Pass의 `visible proxy` 수집 흐름과 일관된 형태로, shadow 전용 caster 집합을 구조화.
  - Directional용과 Local용을 구분하되, 공통 필드(Proxy 참조, bounds/flags, 통계 카운트)는 재사용 가능하게 설계.

## Phase 1 Progress Update (2026-04-28, local priority coverage + ui/stat trim)

### Completed in this step
- Local shadow priority에 화면 점유율(screen coverage) 항목 추가.
  - Point/Spot의 `Position + AttenuationRadius`를 camera view-proj로 투영해 coverage(0~1) 산출.
  - coverage를 priority의 주 가중치로 반영하고, intensity/radius/resolution/type/residency와 함께 합성.
- Shadow 리소스 업데이트 경로에 frame context를 전달하도록 시그니처 정리.
  - `Renderer -> SystemResources -> ShadowResourceManager` 경로로 `FFrameContext` 전달.
- Shadow 설정 UI를 정책 검증 중심으로 축소.
  - 유지: Filter mode, Directional mode, Unlit skip, local budget(views/atlas area), atlas panel toggle
  - 숨김: grace/allocator/alignment/score-threshold 등 내부 튜닝 항목
- Overlay Shadow Stat 라인을 최소화.
  - 유지: technique, directional mode, light type counts(dir/point/spot), local views(req/alloc/fail), estimated shadow VRAM
  - 제거: 내부 스케줄러/allocator 디버그 수치 다수

### Why this step
- 정책의 핵심(화면 중요도 반영)을 먼저 안정화하고,
  팀 테스트 단계에서 UI/Stat 노이즈를 줄여 디버깅 집중도를 높이기 위함.
