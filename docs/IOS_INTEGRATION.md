# iPad 연동 가이드

엔진은 C API(`engine/include/ink/ink_c.h`)로 Swift에 노출됩니다.
가장 쉬운 방법은 저장소 루트의 `Package.swift`를 SwiftPM 의존성으로 추가하는 것입니다
(`.product(name: "InkEngine", package: "note-core")`). xtool(Linux/WSL)과 Xcode 모두 같은 방식으로
빌드됩니다. 직접 연결하려면 `engine/include`를 Header Search Paths에 추가하면 함께 들어 있는
`module.modulemap` 덕분에 `import InkEngine`으로 바로 쓸 수 있습니다.
정적 라이브러리는 README의 iOS 빌드 명령으로 만들거나, `engine/src/*.cpp`를 Xcode 타깃에
직접 추가해도 됩니다 (C++17).

## 1. 입력: UITouch → InkInputPoint

필기감의 절반은 입력 처리입니다. 다음 네 가지를 모두 엔진에 전달하세요.

1. **coalesced touches** — Apple Pencil은 240 Hz로 측정하지만 `touchesMoved`는 화면 주사율로
   옵니다. 사이의 샘플을 버리면 곡선이 각지고 필압이 거칠어집니다.
2. **predicted touches** — 다음 프레임까지의 예상 경로. 체감 지연을 크게 줄입니다.
   최종 획에는 저장되지 않습니다.
3. **estimated properties** — 필압·기울기의 첫 값은 추정치이고 몇 ms 뒤 정확한 값이 옵니다.
   `estimationUpdateIndex`로 엔진이 해당 점을 찾아 굵기를 다시 계산합니다.
4. **preciseLocation** — `location(in:)`보다 정밀한 위치.

```swift
import UIKit
import InkEngine

final class InkCanvasView: UIView {
    let editor = ink_editor_create(595, 842)!          // A4 (pt)
    let builder = ink_builder_create(INK_BRUSH_BALLPOINT)!
    var pageScale: CGFloat = 1                          // view pt per page pt (줌)
    var pageOrigin: CGPoint = .zero                     // 페이지 좌상단의 뷰 좌표
    var pencilOnly = true                               // 손가락은 스크롤/줌에 사용

    deinit {
        ink_builder_destroy(builder)
        ink_editor_destroy(editor)
    }

    private func makePoint(_ t: UITouch) -> InkInputPoint {
        let v = t.preciseLocation(in: self)
        var p = InkInputPoint()
        p.x = Float((v.x - pageOrigin.x) / pageScale)
        p.y = Float((v.y - pageOrigin.y) / pageScale)
        p.time = t.timestamp
        if t.type == .pencil {
            p.pressure = Float(t.force / max(t.maximumPossibleForce, 0.0001))
            p.altitude = Float(t.altitudeAngle)
            p.azimuth = Float(t.azimuthAngle(in: self))
            p.source = INK_SOURCE_PENCIL.rawValue
        } else {
            p.pressure = -1          // 엔진이 속도로 필압을 시뮬레이션
            p.altitude = -1
            p.source = INK_SOURCE_TOUCH.rawValue
        }
        if let idx = t.estimationUpdateIndex { p.update_id = idx.uint64Value }
        var flags: UInt32 = 0
        if t.estimatedPropertiesExpectingUpdates.contains(.force) {
            flags |= INK_INPUT_PRESSURE_EXPECTS_UPDATE.rawValue
        }
        if t.estimatedPropertiesExpectingUpdates.contains(.altitude) {
            flags |= INK_INPUT_TILT_EXPECTS_UPDATE.rawValue
        }
        p.flags = flags
        return p
    }

    private func drawingTouch(_ touches: Set<UITouch>) -> UITouch? {
        touches.first { !pencilOnly || $0.type == .pencil }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = drawingTouch(touches) else { return }
        ink_builder_set_view_scale(builder, Float(pageScale))
        var p = makePoint(t)
        ink_builder_begin(builder, &p)
        feed(t, event)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = drawingTouch(touches), ink_builder_is_active(builder) else { return }
        feed(t, event)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = drawingTouch(touches), ink_builder_is_active(builder) else { return }
        feed(t, event)
        ink_builder_end(builder)
        ink_editor_commit(editor, builder)
        setNeedsRender()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        ink_builder_cancel(builder)
        setNeedsRender()
    }

    override func touchesEstimatedPropertiesUpdated(_ touches: Set<UITouch>) {
        for t in touches {
            guard let idx = t.estimationUpdateIndex else { continue }
            ink_builder_update_estimated(builder, idx.uint64Value,
                                         Float(t.force / max(t.maximumPossibleForce, 0.0001)),
                                         Float(t.altitudeAngle),
                                         Float(t.azimuthAngle(in: self)))
        }
        setNeedsRender()
    }

    private func feed(_ t: UITouch, _ event: UIEvent?) {
        let coalesced = event?.coalescedTouches(for: t) ?? [t]
        let pts = coalesced.map(makePoint)
        ink_builder_add_points(builder, pts, pts.count)
        let predicted = (event?.predictedTouches(for: t) ?? []).map(makePoint)
        ink_builder_set_predicted(builder, predicted, predicted.count)
        setNeedsRender()
    }

    // CADisplayLink 콜백에서 매 프레임 호출: 그리고-멈추기 도형 보정 타이머.
    func onDisplayLink(_ link: CADisplayLink) {
        if ink_builder_is_active(builder), ink_builder_tick(builder, CACurrentMediaTime()) {
            UIImpactFeedbackGenerator(style: .light).impactOccurred()   // 스냅 햅틱
        }
        render()
    }

    func setNeedsRender() { /* 다음 디스플레이 링크 프레임에서 render() */ }
    func render() { /* 아래 2절 참고 */ }
}
```

참고
- `UITouch.timestamp`와 `CACurrentMediaTime()`은 같은 시간축(부팅 이후 초)입니다.
- 지우개 모드에서는 builder 대신 `ink_editor_erase_begin / _move / _end`를 호출합니다
  (`kind_mask`에 `1 << INK_BRUSH_HIGHLIGHTER`를 주면 "형광펜만 지우기").
- 펜 변경: `ink_builder_set_brush(builder, INK_BRUSH_PENCIL, color, width)` 후
  `set_opacity`, `set_stabilizer`, `set_pressure_curve` 등으로 사용자 설정을 덧입힙니다.
- ProMotion 기기에서는 `CADisplayLink.preferredFrameRateRange`를 120 Hz로 설정하세요.

### 도형 그리기와 글씨 보정 연결

```swift
// 펜 설정 화면의 "글씨 보정" 슬라이더 (0 = 끔).
ink_builder_set_correction(builder, Float(correctionSlider.value))
// 세부 항목: 실시간 곡선 보정(펜 뒤에서 바로 자리 잡음), 곡선 다듬기, 직선 펴기, 수평·수직 맞춤
ink_builder_set_correction_options(builder, true, true, true, true)

// 그리고 멈추기: 기본 켜짐(볼펜·만년필·형광펜·마커). 펜별로 끄려면:
ink_builder_set_snap_on_hold(builder, false)

// 스냅 후 두 번째 손가락을 대면 완벽한 도형(원·정사각형·정삼각형·정다각형·15° 직선).
override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
    if ink_builder_is_active(builder), touches.contains(where: { $0.type == .direct }) {
        ink_builder_set_shape_constraint(builder, true)
        return
    }
    // ... 기존 처리
}
// 손가락을 떼면 ink_builder_set_shape_constraint(builder, false)
```

- 스냅 순간 `ink_builder_tick`이 `true`를 반환하므로 그때 가벼운 햅틱을 주면 좋습니다(1절 예제).
- 스냅된 뒤에는 펜을 떼지 않고 움직이면 도형 크기·회전이 따라옵니다. 별도 처리는 필요 없습니다.
- 올가미 메뉴에 "줄 정렬"과 "글씨 다듬기"를 두고 다음을 호출합니다.

```swift
ink_editor_straighten(editor, selectedIds, selectedIds.count, 1.0)   // 줄 기울기·단어 높이 정렬
ink_editor_correct(editor, selectedIds, selectedIds.count, 0.6)      // 이미 쓴 글씨 다듬기
// 둘 다 실행 취소 1단계로 기록되고, ink_editor_take_dirty에 변경 영역이 잡힙니다.
```

## 2. 렌더링

### 빠른 시작 (CPU)
프로토타입 단계에서는 CPU 렌더러로 바로 화면에 그릴 수 있습니다.
`ink_editor_take_dirty`가 돌려주는 영역만 다시 그리면 충분히 빠릅니다.

```swift
let w = Int(bounds.width * contentScaleFactor), h = Int(bounds.height * contentScaleFactor)
var pixels = [UInt8](repeating: 0, count: w * h * 4)
let s = Float(pageScale * contentScaleFactor)
ink_editor_render_rgba(editor, &pixels, Int32(w), Int32(h), w * 4, s,
                       Float(pageOrigin.x * contentScaleFactor), Float(pageOrigin.y * contentScaleFactor),
                       InkColor(r: 1, g: 1, b: 1, a: 1), builder)   // 진행 중인 획도 함께
// pixels는 straight-alpha RGBA8. 배경이 불투명하면 알파가 항상 255이므로
// CGImage(bitmapInfo: .noneSkipLast)로 만들어 layer.contents에 넣으면 됩니다.
```

### 본 렌더러 (Metal)
`engine/shaders/ink_stroke.metal`에 CPU 래스터라이저와 같은 수식의 셰이더가 있습니다.

1. **확정된 획**: 페이지를 타일(예: 512×512 px)로 나누고 타일 텍스처에 캐시합니다.
   `ink_editor_take_dirty` 영역에 걸리는 타일만 다시 그립니다.
   - `ink_editor_query(region)` → 칠할 순서대로 획 id
   - `ink_editor_stroke_geometry(id, pixelsPerUnit)` → 캐시된 프리미티브 배열 + 스타일
     (줌이 2의 거듭제곱 구간을 넘을 때만 다시 테셀레이션)
2. **획 하나 그리기**: 프리미티브 배열을 인스턴스 버퍼로 올리고
   - 커버리지 패스: R16Float 텍스처에 **MAX 블렌딩** (`ink_coverage_capsule / _nib / _stamp`)
   - 합성 패스: 스타일 색·불투명도로 Normal 또는 Multiply (`ink_composite_fragment`)
   - `InkStyleInfo.method == 0`, `opacity == 1`, 모든 점 알파 1인 볼펜·마커는 커버리지 패스를
     생략하고 바로 그려도 됩니다.
3. **실시간 획**: `ink_builder_update_geometry(builder, pixelsPerUnit, &live)`
   - `live.stable`의 `new_*` 인덱스 이후만 실시간 커버리지 텍스처에 누적
     (`stable_reset`이면 텍스처를 비우고 전부 다시)
   - `live.tail`은 매 프레임 별도 텍스처에 그리고, 합성 시 두 커버리지의 max를 사용
4. **연필 결**: `ink_grain_texture(&size)`를 R8Unorm 256×256, `.repeat` 주소 모드, 선형 필터,
   밉맵 없음으로 업로드합니다.

### 타일 CPU 렌더러 (note-ipad가 사용하는 방식)
GPU 렌더러를 붙이기 전에도 120 Hz 필기가 가능하도록, 화면을 512 px 안팎의 타일로 나누고
타일 단위로 CPU 렌더링합니다. 두 함수가 이를 위해 있습니다.

```swift
// 1) 확정된 획: 타일 버퍼에 종이 템플릿/PDF 배경을 Core Graphics로 먼저 그린 뒤 (premultipliedLast, RGBA)
ink_editor_render_over_rgba(editor, tile.bytes, w, h, stride, scale, -tileOriginX, -tileOriginY)
// 형광펜 Multiply가 실제 배경(PDF 글자 포함)에 적용되고, 테셀레이션은 줌 구간별로 캐시됩니다.

// 2) 실시간 획: 매 프레임 ink_builder_update_geometry 후 live.dirty에 걸리는 오버레이 타일만
memset(overlay.bytes, 0, overlay.count)
ink_builder_render_over_rgba(builder, overlay.bytes, w, h, stride, scale, -tileOriginX, -tileOriginY)
// 직전 update_geometry 결과(stable + tail + 예측점)를 그리며, 증분 상태는 바꾸지 않습니다.
```

올가미 편집용으로 `ink_editor_bounds_of`(선택 영역 사각형), `ink_editor_duplicate`,
`ink_editor_copy_strokes` / `ink_editor_paste_strokes`(페이지 간 복사·붙여넣기, 실행 취소 1단계),
선택 영역을 끌 때 미리보기용 `ink_editor_set_hidden` + `ink_editor_render_strokes_over_rgba`가 있습니다.

셰이더는 Linux CI에서 컴파일되지 않으므로, 연동 시 같은 페이지를
`ink_editor_render_rgba`(CPU)와 Metal로 각각 그려 픽셀 비교 테스트를 만들어 두기를 권장합니다.

## 3. 저장

```swift
var size = 0
if let data = ink_editor_save(editor, &size) {
    let blob = Data(bytes: data, count: size)
    ink_free(data)
    try blob.write(to: pageURL)
}
// 불러오기
let blob = try Data(contentsOf: pageURL)
blob.withUnsafeBytes { raw in
    _ = ink_editor_load(editor, raw.bindMemory(to: UInt8.self).baseAddress, raw.count)
}
```

페이지 단위 파일로 저장하면 큰 노트북도 필요한 페이지만 열 수 있습니다.
SVG(`ink_editor_export_svg`)는 벡터 외곽선을 담고 있어 PDF 내보내기(Core Graphics 경로)로
그대로 옮길 수 있습니다.
