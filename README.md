# libapplegfx-vulkan — Android compatibility research

[English](#english) · [한국어](#korean) · [Original README / 기존 README](docs/archive/README-upstream.md) · [Upstream](https://github.com/MattJackson/libapplegfx-vulkan) · [MIT License](LICENSE)

<a id="english"></a>

## English

An experimental Android ARM64 validation fork of Matthew Jackson's [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan), a clean-room host-side reimplementation of Apple's ParavirtualizedGraphics.framework with a Vulkan backend. The upstream library decodes PVG commands and translates Apple AIR shader IR to SPIR-V. This fork documents Android compatibility work performed with the independent [Henge Android port](https://github.com/muns5965/Henge/tree/research/android-port).

**Status: 2026-11-25 — Android ARM64 translation and selected GPU rendering verified. Full Metal API and live PVG compatibility are not established.**

### Test environment and reproducibility

- Samsung Galaxy S26 Ultra, Android 17, Qualcomm Adreno 840.
- Tested library revision: [7574eac](https://github.com/muns5965/libapplegfx-vulkan/tree/7574eac0d7396a9f49366f007c7fbeb44245826c).
- Android NDK r27c, arm64-v8a, API 30 minimum; Vulkan 1.2 SPIR-V host validation.
- Standalone Henge GPU test APK 4.4; separate from the VM. Android build glue and test harness currently live in Henge, not in this fork.
- [Test source](https://github.com/muns5965/Henge/tree/fe6064e5ad415efd400572489216731855bfe0cc/tests/gpu-android-gpt) · [Successful CI build](https://github.com/muns5965/Henge/actions/runs/45575251001) · [Download APK 4.4](https://github.com/muns5965/Henge/actions/runs/45575251001/artifacts/15004136515).
- Install and run the six-stage GPU test, the 11-module corpus translation test, then the ColorFill / TextureCopy GPU output test. Artifacts may expire or require sign-in.

### Android compatibility results

PASS = observed success (including expected negative-control behavior); FAIL = unexpected test failure; FIXED = previously observed issue resolved by a documented change; NOT TESTED = no completed verification. Results are developer-reported physical-device tests, not universal guarantees. Expected negative-control failures demonstrate that the draw is required and are not functional regressions.

| Test | Result | Evidence |
| --- | --- | --- |
| ARM64 NDK build and native execution | ✅ PASS | Android 17 |
| Vulkan dynamic dispatch | ✅ PASS | Adreno 840 |
| 1. AIR triangle | ✅ PASS | 3,724/3,724 |
| 2. Texture sampling (GLSL) | ✅ PASS | 4,096/4,096 |
| 3. Alpha blending (GLSL) | ✅ PASS | 4,096/4,096 |
| 4. FP16 render and conversion (GLSL) | ✅ PASS | 4,096 raw + 4,096 converted |
| 5. Fresh on-device AIR translation and draw | ✅ PASS | SHA-256 match; 3,724/3,724 |
| 6. Library render target clear/readback | ✅ PASS | 4,096/4,096 |
| 7. Real Metal ColorFill | ✅ PASS | 4,096/4,096; 00FF00FF |
| 8. Real Metal TextureCopy | ✅ PASS | 4,096/4,096; 4080FFFF |
| Android APK AIR asset naming (.bc → .airdata) | ✅ FIXED | APK installation rejected with .bc assets (4.2 / control B); identical AIR bytes renamed to .airdata install and execute successfully (4.3/4.4). Packaging limitation, not an AIR translator or GPU failure. |
| Neutral .airdata APK assets (4.3/4.4) | ✅ PASS | Installed / 설치 성공 |
| ColorFill negative control (draw omitted) | ✅ PASS | Expected 4,096/4,096 mismatches and exit 1 observed |
| TextureCopy negative control (draw omitted) | ✅ PASS | Expected 4,096/4,096 mismatches and exit 1 observed |
| Real login vertex + fragment pair | ⬜ NOT TESTED | GLSL reference vertex |
| Patterned Metal texture / UV accuracy | ⬜ NOT TESTED | Uniform texture only |
| UberComposite, Vfx, Xgc GPU rendering | ⬜ NOT TESTED | Translation only |
| Live Henge PVG / desktop integration | ⬜ NOT TESTED | Standalone APK |

### Real macOS login AIR shader corpus

11 modules and 66 defined-function translations reported successful. Emitted SPIR-V matched the Linux host SHA-256 and passed host `spirv-val --target-env vulkan1.2`. Xgc's size is its main function; the other 55 reported functions were translated separately, not independently GPU-executed.

| Function/module | Stage | Translation | Host hash / validation | Main SPIR-V bytes | GPU render |
| --- | --- | --- | --- | ---: | --- |
| ViewportToNDC | Vertex | ✅ 1/1 PASS | ✅ PASS | 2,056 | ⬜ NOT TESTED |
| UberCompositeVertex | Vertex | ✅ 1/1 PASS | ✅ PASS | 1,952 | ⬜ NOT TESTED |
| Vfx | Vertex | ✅ 1/1 PASS | ✅ PASS | 2,136 | ⬜ NOT TESTED |
| ColorFill | Fragment | ✅ 1/1 PASS | ✅ PASS | 900 | ✅ PASS |
| TextureCopy | Fragment | ✅ 1/1 PASS | ✅ PASS | 1,136 | ✅ PASS |
| UberCompositeFragment A | Fragment | ✅ 1/1 PASS | ✅ PASS | 1,544 | ⬜ NOT TESTED |
| UberCompositeFragment B | Fragment | ✅ 1/1 PASS | ✅ PASS | 1,524 | ⬜ NOT TESTED |
| VfxU11 | Vertex | ✅ 1/1 PASS | ✅ PASS | 2,940 | ⬜ NOT TESTED |
| TvcmXc_Isrc | Fragment | ✅ 1/1 PASS | ✅ PASS | 6,740 | ⬜ NOT TESTED |
| VfxXgb | Vertex | ✅ 1/1 PASS | ✅ PASS | 10,208 | ⬜ NOT TESTED |
| Xgc | Fragment | ✅ 56/56 PASS | ✅ PASS | 337,892 | ⬜ NOT TESTED |

### Actual GPU rendering

ColorFill reads a StorageBuffer; TextureCopy samples an image with a separate sampler. Both use freshly translated Metal fragment shaders with a GLSL reference vertex. Each produced 4,096/4,096 matching pixels, totaling 8,192/8,192. Omitting the draw produced 8,192/8,192 mismatches and exit code 1 in both controls.

The uniform-color TextureCopy test does not establish general UV accuracy. Successful translation and SPIR-V validation do not prove full Metal semantics. Real login vertex pairing, complex composition, live PVG commands, and macOS desktop rendering remain unverified.

### Build and upstream information

The upstream Linux build uses C11, Meson, Ninja and Vulkan headers/loader:

```sh
meson setup builddir
meson compile -C builddir
meson test -C builddir --print-errorlogs
```

For the Android cross-build, use the pinned [Henge workflow](https://github.com/muns5965/Henge/blob/fe6064e5ad415efd400572489216731855bfe0cc/.github/workflows/gpt-gpu-test.yml). It builds the actual Vulkan-enabled static library and packages the native test executables. AIR fixtures use a neutral `.airdata` suffix. APKs containing the original `.bc`-suffixed assets were rejected on the tested Android device; renaming the assets without changing their bytes resolved installation. Android's legacy RenderScript `.bc` detection is the likely explanation, but the exact package-manager rejection code was not captured. This is an APK asset filename-extension / packaging issue, not a demonstrated filesystem path or AIR conversion defect. The same AIR contents passed translation and selected GPU rendering tests after renaming; `.bc` packaging itself remains unverified as installable.

For the original Linux/QEMU architecture, API, dependencies, milestones, contributing rules, and upstream test descriptions, see the [archived original README](docs/archive/README-upstream.md). Upstream Linux results are not Android test results.

### Credits and license

Original library by Matthew Jackson ([MattJackson](https://github.com/MattJackson)); Android test integration is developed in the Henge fork. Library source is [MIT-licensed](LICENSE). Third-party Apple shader fixtures are test inputs; no separate license grant for Apple's material is asserted. See upstream [contribution guidelines](CONTRIBUTING.md) and [security policy](SECURITY.md).

---

<a id="korean"></a>

## 한국어

Matthew Jackson의 [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan)을 기반으로 Android ARM64 호환성을 검증하는 실험적 포크입니다. 원본은 Apple ParavirtualizedGraphics.framework의 호스트 측 기능을 클린룸 방식으로 재구현하고 Vulkan 백엔드를 사용합니다. PVG 명령을 해석하고 Apple AIR 셰이더 중간 표현을 SPIR-V로 변환합니다. Android 검증은 독립 [Henge Android 포팅 프로젝트](https://github.com/muns5965/Henge/tree/research/android-port)와 연계하여 수행했습니다.

**기준일: 2026-11-25 — Android ARM64 변환 및 일부 실제 GPU 렌더링 검증 완료. Metal 전체 API 및 실제 PVG 호환은 미확인입니다.**

### 시험 환경 및 재현 방법

- 삼성 갤럭시 S26 Ultra, Android 17, Qualcomm Adreno 840.
- 시험 라이브러리 리비전: [7574eac](https://github.com/muns5965/libapplegfx-vulkan/tree/7574eac0d7396a9f49366f007c7fbeb44245826c).
- Android NDK r27c, arm64-v8a, 최소 API 30, 호스트 Vulkan 1.2 SPIR-V 검증.
- VM과 분리된 Henge GPU 시험 APK 4.4. Android 빌드 구성과 시험 코드는 현재 Henge에 위치합니다.
- [시험 소스](https://github.com/muns5965/Henge/tree/fe6064e5ad415efd400572489216731855bfe0cc/tests/gpu-android-gpt) · [성공한 CI 빌드](https://github.com/muns5965/Henge/actions/runs/45575251001) · [4.4 APK 다운로드](https://github.com/muns5965/Henge/actions/runs/45575251001/artifacts/15004136515).
- 설치 후 6단계 GPU 시험, 로그인 셰이더 11종 변환 시험, ColorFill / TextureCopy GPU 출력 시험을 각각 실행합니다. 아티팩트는 만료되거나 로그인이 필요할 수 있습니다.

### Android 호환성 검증 결과

PASS는 실제 시험 통과(음성 대조군의 예상 결과 포함), FAIL은 예상하지 않은 시험 실패, FIXED는 기존 문제를 명시된 변경으로 해결한 상태, NOT TESTED는 검증 미완료를 의미합니다. 개발자가 제공한 실기기 시험 결과이며 모든 Android 기기의 호환성을 보장하지 않습니다. Draw 생략 대조군의 예상 실패는 정상 기능의 실패와 구분합니다.

| 시험 | 결과 | 근거 |
| --- | --- | --- |
| ARM64 NDK 빌드 및 네이티브 실행 | ✅ PASS | Android 17 |
| Vulkan 동적 함수 연결 | ✅ PASS | Adreno 840 |
| 1. AIR 삼각형 | ✅ PASS | 3,724/3,724 |
| 2. 텍스처 샘플링(GLSL) | ✅ PASS | 4,096/4,096 |
| 3. 알파 블렌딩(GLSL) | ✅ PASS | 4,096/4,096 |
| 4. FP16 렌더링 및 변환(GLSL) | ✅ PASS | 4,096 raw + 4,096 converted |
| 5. 기기 내부 AIR 변환 및 그리기 | ✅ PASS | SHA-256 match; 3,724/3,724 |
| 6. 라이브러리 렌더 타겟 초기화 및 읽기 | ✅ PASS | 4,096/4,096 |
| 7. 실제 Metal ColorFill | ✅ PASS | 4,096/4,096; 00FF00FF |
| 8. 실제 Metal TextureCopy | ✅ PASS | 4,096/4,096; 4080FFFF |
| Android APK AIR 데이터 파일명(.bc → .airdata) | ✅ FIXED | .bc 확장자 포함 시 설치 거부(4.2 / B 대조군). 원본 바이트를 유지하고 .airdata로 변경한 후 설치·실행 성공(4.3/4.4). 패키징 제약이며 AIR 변환기 또는 GPU 기능의 오류가 아님. |
| 개선된 .airdata APK 데이터(4.3/4.4) | ✅ PASS | Installed / 설치 성공 |
| ColorFill 음성 대조군(Draw 생략) | ✅ PASS | 예상대로 4,096/4,096개 픽셀 불일치 및 종료 코드 1 확인 |
| TextureCopy 음성 대조군(Draw 생략) | ✅ PASS | 예상대로 4,096/4,096개 픽셀 불일치 및 종료 코드 1 확인 |
| 실제 로그인 정점·프래그먼트 조합 | ⬜ NOT TESTED | GLSL reference vertex |
| 패턴 텍스처 및 UV 정확성 | ⬜ NOT TESTED | Uniform texture only |
| UberComposite·Vfx·Xgc GPU 렌더링 | ⬜ NOT TESTED | Translation only |
| Henge 실제 PVG·데스크톱 통합 | ⬜ NOT TESTED | Standalone APK |

### 실제 macOS 로그인 AIR 셰이더 검증

AIR 모듈 11종, 정의된 함수 66개 변환 성공이 보고되었습니다. 생성된 SPIR-V는 Linux 호스트 SHA-256과 일치하며 호스트 `spirv-val --target-env vulkan1.2`를 통과했습니다. Xgc 크기는 주 함수 기준이고 나머지 55개 함수는 별도 변환되었으나 GPU 개별 실행은 미검증입니다.

| 함수/모듈 | 단계 | 변환 | 호스트 해시·규격 | 주 SPIR-V 바이트 | GPU 출력 |
| --- | --- | --- | --- | ---: | --- |
| ViewportToNDC | 정점 | ✅ 1/1 PASS | ✅ PASS | 2,056 | ⬜ NOT TESTED |
| UberCompositeVertex | 정점 | ✅ 1/1 PASS | ✅ PASS | 1,952 | ⬜ NOT TESTED |
| Vfx | 정점 | ✅ 1/1 PASS | ✅ PASS | 2,136 | ⬜ NOT TESTED |
| ColorFill | 프래그먼트 | ✅ 1/1 PASS | ✅ PASS | 900 | ✅ PASS |
| TextureCopy | 프래그먼트 | ✅ 1/1 PASS | ✅ PASS | 1,136 | ✅ PASS |
| UberCompositeFragment A | 프래그먼트 | ✅ 1/1 PASS | ✅ PASS | 1,544 | ⬜ NOT TESTED |
| UberCompositeFragment B | 프래그먼트 | ✅ 1/1 PASS | ✅ PASS | 1,524 | ⬜ NOT TESTED |
| VfxU11 | 정점 | ✅ 1/1 PASS | ✅ PASS | 2,940 | ⬜ NOT TESTED |
| TvcmXc_Isrc | 프래그먼트 | ✅ 1/1 PASS | ✅ PASS | 6,740 | ⬜ NOT TESTED |
| VfxXgb | 정점 | ✅ 1/1 PASS | ✅ PASS | 10,208 | ⬜ NOT TESTED |
| Xgc | 프래그먼트 | ✅ 56/56 PASS | ✅ PASS | 337,892 | ⬜ NOT TESTED |

### 실제 GPU 렌더링

ColorFill은 StorageBuffer를 읽으며 TextureCopy는 이미지와 별도 샘플러를 이용합니다. 두 시험은 기기에서 새로 변환한 실제 Metal 프래그먼트와 GLSL 기준 정점을 사용합니다. 각각 4,096개 픽셀 전부 일치하여 총 8,192/8,192를 기록했습니다. Draw 생략 대조군은 총 8,192개 전부 불일치했으며 각 종료 코드는 1입니다.

균일 색상 TextureCopy 시험만으로 일반적인 UV 좌표 정확성을 확인할 수 없습니다. 변환과 규격 검증은 Metal 전체 의미론을 보장하지 않습니다. 실제 로그인 정점·프래그먼트 연결, 복합 화면 합성, 실제 PVG 명령 및 macOS 데스크톱 출력은 미검증입니다.

### 빌드 및 원본 프로젝트

원본 Linux 빌드는 C11, Meson, Ninja, Vulkan 헤더 및 로더를 사용합니다.

```sh
meson setup builddir
meson compile -C builddir
meson test -C builddir --print-errorlogs
```

Android 교차 빌드는 [Henge 시험 워크플로](https://github.com/muns5965/Henge/blob/fe6064e5ad415efd400572489216731855bfe0cc/.github/workflows/gpt-gpu-test.yml)를 참조하십시오. 실제 Vulkan을 활성화한 정적 라이브러리 및 네이티브 시험 실행 파일을 빌드합니다. AIR 시험 데이터에는 `.airdata` 확장자를 사용합니다. 시험 기기에서는 원본 `.bc` 확장자가 포함된 APK의 설치가 거부되었으나 데이터 바이트를 변경하지 않고 확장자만 변경한 후 설치에 성공했습니다. Android의 기존 RenderScript `.bc` 감지 로직이 유력한 원인이지만 패키지 관리자의 정확한 설치 거부 코드는 확보하지 못했습니다. 이는 파일 경로 자체의 오류로 확인된 것이 아니라 APK 내부 파일명 확장자 및 패키징 문제이며, AIR 변환 기능의 결함으로 확인된 사항은 아닙니다. 확장자 변경 후 동일 데이터의 변환 및 일부 GPU 렌더링 시험을 통과했습니다. `.bc` 확장자를 그대로 사용한 APK의 설치 가능성은 확인되지 않았습니다.

기존 Linux/QEMU 아키텍처, API, 의존성, 개발 이력, 기여 및 시험 방법은 [보존된 원본 README](docs/archive/README-upstream.md)를 참조하십시오. 원본 Linux 검증 결과는 Android 검증 결과와 별개입니다.

### 출처 및 라이선스

원본 라이브러리: Matthew Jackson ([MattJackson](https://github.com/MattJackson)). Android 시험 통합은 Henge 포크에서 개발 중입니다. 라이브러리 소스는 [MIT 라이선스](LICENSE)를 따릅니다. Apple 셰이더 시험 데이터에 대해 별도의 이용허락을 주장하지 않습니다. 원본 [기여 지침](CONTRIBUTING.md) 및 [보안 정책](SECURITY.md)을 참조하십시오.
