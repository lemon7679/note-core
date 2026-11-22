# InkEngine 설계

## 전체 흐름

```
 UITouch (240 Hz, coalesced / predicted / estimated)
        │  InputPoint
        ▼
 ┌────────────── StrokeBuilder ──────────────┐
 │ 1€ 필터 → (손떨림 보정) → 중복 병합          │
 │ → 속도 EMA → 필압(EMA+곡선 / 시뮬레이션)     │
 │ → 기울기 → 굵기·불투명도·시작 테이퍼          │   펜을 떼면: 따라잡기, 갈고리 제거,
 │ → StrokePoint (다이내믹스가 구워진 제어점)    │   끝 테이퍼, (멈춤 시) 도형 스냅
 └───────────────────────────────────────────┘
        │ points (+ predicted)
        ▼
 Tessellator: Centripetal Catmull–Rom 샘플링 → 프리미티브
   CapsulePrim (둥근 팁) / NibPrim (평닙) / StampPrim (연필)
        │
        ├─► GPU (Metal): 인스턴스 쿼드 + SDF 프래그먼트 → MAX 커버리지 → 합성
        └─► CPU Rasterizer: 같은 수식 (내보내기, 썸네일, 테스트 기준)

 PageEditor = Page(Layers of immutable Strokes) + History(ChangeSet) + dirty rect
```

## 핵심 설계 결정

### 1. 다이내믹스를 제어점에 "굽는다"
`StrokePoint`에는 필압 곡선·속도·테이퍼·기울기가 모두 반영된 최종 굵기/불투명도가 저장됩니다.
렌더링은 획 데이터만의 순수 함수이므로

- 지우개로 잘라도 조각이 원래 모양을 그대로 유지하고 (테이퍼가 새로 생기지 않음),
- 브러시 프리셋을 나중에 바꿔도 기존 노트가 변하지 않으며,
- 어느 플랫폼에서 열어도 같은 결과가 나옵니다.

### 2. 1€ 필터 + 뷰 공간 기준
고정 이동평균은 떨림을 줄이는 만큼 지연(고무줄 느낌)을 만듭니다. 1€ 필터는 속도에 따라 차단
주파수를 올려 느릴 때만 강하게 다듬습니다. 필터·속도·최소 거리·갈고리 길이는 모두 **뷰 포인트**
기준(`setViewScale`)이라 확대 상태에서도 손에 느껴지는 감각이 같습니다
(테스트 `builder_feel_is_zoom_invariant`).

### 3. Centripetal Catmull–Rom
균일 매개변수 Catmull–Rom은 점 간격이 불균일할 때(빠른 획) 고리·첨점이 생깁니다.
centripetal(α=0.5)은 이것이 수학적으로 일어나지 않으며, 제어점을 정확히 통과합니다.
도형 스냅의 꼭짓점은 `kPointSharp`로 표시해 둥글게 깎이지 않게 합니다.

### 4. SDF 프리미티브 + 획 단위 MAX 합성
- 둥근 팁은 반지름이 다른 두 원을 잇는 **uneven capsule**의 정확한 거리 함수로 그립니다.
  인접 캡슐이 끝 원을 공유하므로 이음매가 없고, 픽셀 단위 커버리지 `clamp(0.5 - d)`로
  어떤 배율에서도 깨끗한 안티앨리어싱이 됩니다. 0.5 px보다 가는 선은 굵기를 0.5 px로 두고
  알파를 줄여 끊기지 않고 옅어지게 합니다.
- 평닙(형광펜 치즐, 캘리그라피)은 닙 선분을 두 위치 사이로 쓸어낸 사다리꼴 + 모서리 반경입니다.
- 한 획의 프리미티브는 먼저 커버리지 마스크에 **MAX**로 누적한 뒤 한 번만 색/불투명도/블렌드로
  합성합니다. 그래서 반투명 펜이 자기 자신과 겹쳐도 얼룩(구슬 모양)이 생기지 않고,
  별개의 획끼리만 진해집니다 — 실제 형광펜과 같은 동작입니다.

### 5. 연필: 페이지 고정 종이 결
결 텍스처는 획이 아니라 **페이지 좌표**에 고정됩니다. 그래서 여러 획을 겹쳐 칠해도 같은 종이
돌기가 드러나고, 그리는 중에 무늬가 흘러다니지 않습니다. 텍스처는 히스토그램 평활화되어 있어
임계값 t는 정확히 (1-t) 비율의 돌기에 흑연을 묻힙니다. 필압이 낮으면 돌기 꼭대기만,
높으면 골까지 채우며, 골에도 옅은 막(floor)이 남습니다. 눕힌 펜은 타원 스탬프 + 얕은 침투로
넓고 옅은 음영을 만듭니다.

### 6. 증분 실시간 렌더링
`StrokeBuilder::updateGeometry`는 다음을 돌려줍니다.
- `stable`: 더 이상 바뀌지 않는 프리미티브(단조 증가). 렌더러는 새로 추가된 부분만
  실시간 획 커버리지 텍스처에 한 번 구워 넣습니다.
- `tail`: 마지막 몇 구간 + 예측점. 매 프레임 다시 그립니다.

점이 "안정"이 되는 조건: 스플라인 지지점 4개가 모두 확정되고, 필압 추정치 갱신을 기다리는
점이 아닐 것(최대 250 ms). 그래서 프레임 비용은 획 길이와 무관합니다
(테스트 `builder_live_stable_geometry_is_prefix_of_final`).

### 7. 가역적인 편집 (ChangeSet)
모든 편집은 `{제거된 획(이전 인덱스), 추가된 획(이후 인덱스)}` 하나의 형태로 표현됩니다.
추가·삭제·정밀 지우개(1획 → N조각, z-순서 유지)·변형·색 변경이 모두 같은 코드로
적용/되돌리기 됩니다. 획은 불변(`shared_ptr<const Stroke>`)이라 실행 취소 기록과 렌더 스레드가
잉크를 복사하지 않습니다.

## 튜닝 포인트

펜별 손맛은 `engine/src/brush.cpp`의 프리셋에 모여 있습니다.

| 파라미터 | 의미 |
|---|---|
| `dynamics.width` | 기준 굵기(pt). `mmToPt(0.5f)` = 0.5 mm |
| `pressureCurve` | 필압 반응 곡선 (베지어 제어점 2개) |
| `pressureWidthMin/Max` | 필압 0/1일 때 굵기 배율 |
| `pressureOpacityMin/Max` | 필압 0/1일 때 불투명도 (연필) |
| `velocityThinning`, `velocityReference` | 빠를수록 가늘어지는 정도 / 기준 속도(뷰 pt/s) |
| `taper.startLength/endLength` | 테이퍼 길이(굵기의 배수), `velocityScaledEnd`로 들어올리는 속도 반영 |
| `tilt.*` | 연필 눕힘 음영 시작/최대 각도, 굵기·불투명도 배율 |
| `smoothing.pencil/touch` | 1€ 필터 (minCutoff, beta) — 스타일러스/손가락 별도 |
| `smoothing.stabilizer` | 손떨림 보정 (당김줄 길이 또는 이동평균 창) |
| `hookLength` | 갈고리로 간주할 최대 길이(뷰 pt) |
| `snapOnHold`, `holdDuration` | 그리고 멈추기 도형 보정 |
| `style.hardness/spacing/grainStrength/grainScale` | 연필 스탬프 경도·간격·결 강도·결 크기 |
| `style.nibAngle/nibThickness` | 평닙 각도·두께 |

실기기에서 조정할 때는 `ink_demo`의 합성 입력 대신 실제 녹화한 터치 시퀀스를 넣어 비교하는 것을
권장합니다 (InputPoint 배열을 저장/재생하면 됩니다).

## 스레드 모델

엔진 객체는 스레드 안전하지 않습니다. 권장 구성:
- 메인 스레드: 입력 → `StrokeBuilder`, 편집 → `PageEditor`
- 렌더 스레드: 프레임 시작 시 필요한 획의 `StrokePtr`/지오메트리를 넘겨받아 사용
  (획은 불변이므로 포인터만 넘기면 안전합니다)
