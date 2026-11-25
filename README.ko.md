# libapplegfx-vulkan — Android / ARM64 연구용 포크

[English](README.md) · [한국어](README.ko.md) · [호환성 대응표](docs/metal-vulkan-compatibility.ko.md) · [기존 README](docs/archive/README-upstream.md) · [원본](https://github.com/MattJackson/libapplegfx-vulkan) · [MIT 라이선스](LICENSE)

## 프로젝트 개요

Matthew Jackson의 [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan)을 기반으로 한 실험적 포크입니다. 원본은 Apple ParavirtualizedGraphics(PVG) 프레임워크의 호스트 측 기능을 클린룸 방식으로 재구현하고 Vulkan 백엔드를 사용합니다. PVG 와이어 프로토콜을 해석하고, 게스트 페이지 테이블을 탐색하며, Metal 명령과 Apple AIR 셰이더(AIR → SPIR-V)를 Vulkan으로 변환해 임의의 Vulkan 드라이버로 렌더링합니다.

이 포크의 목표는 두 가지입니다. 라이브러리를 **Android ARM64**에서 실행하는 것, 그리고 기존 x86 동작과 별도로 **ARM64 vmapple** 프로토콜 모드를 추가하는 것입니다. 무엇이 동작하는지, 프로토콜 수준에서만 인식되는지, 누락되었는지를 문서화합니다. Metal-to-Vulkan 변환 자체는 **원본의 구현**이며 이 포크가 작성한 것이 아닙니다.

**브랜치:** `research/android-pfn-shift`. **문서 기준 소스 리비전:** `6a29b1d`(2026-11-26). **원본 기준:** `7574eac`.

읽는 법: 이 문서에서 *구현 완료*는 코드가 있다는 뜻이며, *구현 및 검증 완료*만 이 포크에서 시험이 통과했다는 뜻입니다. Opcode 인식, 로그 출력, Stamp 반환은 **Metal 지원이 아닙니다**. [호환성 대응표](docs/metal-vulkan-compatibility.ko.md)는 이를 구분해 기록합니다.

## 원본과 포크의 구분

| 출처 | 의미 | 예 |
| --- | --- | --- |
| 원본 구현 | `7574eac`에 이미 있음 | AIR → SPIR-V 변환기, 렌더·Draw 경로, Blit 보조 함수, Display, Cursor, IOSurface 레지스트리, Stamp, 링 디코더 |
| 포크 구현 | 이 포크가 추가 | ARM64 vmapple 프로토콜 모드, 16 KiB 링·PFN 선택, arm64 Device Info 표, 컴퓨트 채널 `0x38`/`0x44`, 진단 기능 |
| 포크 수정 | 원본 기능을 포크가 수정 | Vulkan 메모리 정확성 강화, Cursor 글리프 메모리 처리, `LAGFX_NO_PROCESS` |
| 계획 | 미구현 | *향후 계획* 참조 |

기능별 출처는 [호환성 대응표](docs/metal-vulkan-compatibility.ko.md)에 있습니다.

## 포크 변경사항

원본 `7574eac` 이후 변경(커밋 23개, 앞의 3개는 README만 변경):

- **ARM64 vmapple 프로토콜 모드** — `lagfx_device_descriptor_t.protocol_mode`(`LAGFX_PROTO_X86` = 0 기본값, `LAGFX_PROTO_ARM64_VMAPPLE` = 1, 다른 값은 `lagfx_device_new` 실패). Apple Henge `pvg_lite.c` / `pvg_metal.m`의 동작을 따릅니다.
- **16 KiB 링 페이지·PFN** — `lagfx_device_descriptor_t.ring_page_shift`(0 또는 12 = 4 KiB 기본값, 14 = 16 KiB, 다른 값은 실패). Root·Shared·Channel PFN, 링 페이지 배열 색인, 페이지 내 오프셋, Stamp 셀에 적용됩니다. PFN을 4 KiB 또는 16 KiB로 읽을 때 링 시작 위치에 무엇이 있는지 한 번 기록하는 진단이 있습니다.
- **MMIO 및 Device Info** — arm64 모드에서 MMIO `0x122c`는 1(x86은 9)을 반환합니다. arm64 모드는 Apple Henge의 `CmdGetDeviceInfo` 표를 그대로 사용합니다.
- **컴퓨트 채널 opcode** — 컴퓨트 채널에서 `0x38`(호스트 작업 정의)과 `0x44`(객체·배치 목록)를 처리합니다. arm64 모드에서 `0x25`는 `{task, ref}` 객체를 제거하고 `0x20`은 작업의 객체 캐시를 비우며 Command Buffer를 실행하지 않습니다.
- **진단** — 명령 순번·시간, `0x20`/`0x22`/`0x25` 원본 바이트, 채널별 마지막 `0x44`, Display 제출 사유, 로그 바이트 카운터. 동작은 이에 의존하지 않습니다.
- **Android Vulkan 메모리 정확성** — 업로드 버퍼, 대체 Uniform 버퍼, Cursor 글리프 메모리의 host-visible·coherent 요구, map·flush 실패 전파, 리소스 해제 전 Cursor·Display Fence 오류 처리.
- **`LAGFX_NO_PROCESS`** — 프로세스 생성을 금지하는 실행기를 위해 `system()`/`popen()` 호출 4개(디버그 덤프, `llc` 탐색)를 제거합니다.
- **시험·문서** — 신규 단위 시험(`pfn-shift`, `protocol-mode`, `task-define`, `diag`, `android-memory-mock`), 이 문서, [ARM64 대응 문서](docs/arm64-vmapple-correspondence.md).

## Metal-to-Vulkan 지원 요약

세부 opcode 상태와 근거: [docs/metal-vulkan-compatibility.ko.md](docs/metal-vulkan-compatibility.ko.md).

| 영역 | 상태 | 비고 |
| --- | --- | --- |
| Shader (AIR → SPIR-V) | 구현 및 검증 완료 | Android: 11개 모듈·66개 함수 변환 및 규격 검증. GPU 실행 검증은 `ColorFill`, `TextureCopy`만. |
| Render Pipeline | 부분 구현 | 파이프라인 생성, 블렌딩, Viewport·Scissor는 구현. 대부분의 고정 기능 상태 설정은 로그만. |
| Compute Pipeline | 미구현 | Compute 파이프라인·Dispatch 없음. |
| Buffer | 구현 완료·미검증 | 정점·프래그먼트 버퍼 바인딩. 버퍼 간 복사는 임시 구현. |
| Texture | 부분 구현 | 프래그먼트 텍스처는 구현. 정점 텍스처와 Sampler 바인딩은 로그만. |
| Render Pass | 구현 완료·미검증 | Color Attachment. Depth/Stencil, MSAA는 미검증. |
| Blit | 부분 구현 | 24개 중 2개만 동작(`CopyFromTextureToTexture`, `FillTextureWithColor`), 22개는 응답만. |
| Draw | 부분 구현 | 5개 Draw 형태가 `vkCmdDraw`/`vkCmdDrawIndexed`에 도달. Instanced-Base, Patch, Indirect Draw는 로그만. |
| IOSurface | 부분 구현 | 레지스트리와 필요 시 생성되는 `VkImage` 표면. opcode 핸들러는 `VkImage`를 할당하지 않음. |
| Synchronization | 임시 구현 | Stamp·IRQ는 프로토콜 수준에서만 동작. Metal Fence·Barrier는 로그만, `CmdSynchronizeResources`는 미구현 반환. |
| Display | 구현 완료·미검증 | Swap Mapping, Scanout, 읽기. 모드 반영(`0x11`)은 로그·응답만. |
| Cursor | 구현 완료·미검증 | 글리프 업로드와 오버레이. MMIO `0x1220` 커서 이동은 미구현. |

정보 응답(Device Info, Compute Info, Pipeline Info)은 게스트가 사용할 수 있는 기능을 알리는 프로토콜 응답이며, Vulkan이 해당 기능을 수행할 수 있다는 **증거가 아닙니다**.

## ARM64 vmapple 지원

**상태: 실험적 지원.** arm64 동작은 단위 시험만 수행했습니다(단위 시험은 Vulkan 로더가 없는 빌드에서 실행되므로 Vulkan 의존 코드는 시험되지 않습니다). 실제 ARM64 게스트에서 실행한 적이 없고 어떤 APK에도 포함되지 않았습니다.

| 항목 | x86 모드(기본값) | arm64 vmapple 모드 |
| --- | --- | --- |
| 선택 | `protocol_mode = 0` | `protocol_mode = 1` |
| 링 페이지 크기 | 4 KiB | 별개 설정: `ring_page_shift` 12 또는 14 |
| MMIO `0x122c` | 9 | 1 |
| `CmdGetDeviceInfo` | x86 표 | Apple Henge 표(그대로) |
| Compute `0x20` | Command Buffer 탐색기(`CmdExecIndirect2`) | 작업 객체 캐시 삭제, 실행하지 않음 |
| Compute `0x25` | 첫 번째 live 작업에 객체 등록(대체 동작) | `{task, ref}` 제거 |
| Compute `0x37` | Command Buffer 탐색기 | Command Buffer 탐색기(변경 없음) |
| Compute `0x38`, `0x44` | 처리 | 처리 |

`0x44` 객체·배치 목록은 기록만 하며 아직 사용하는 곳이 없습니다. arm64 모드에서 Metal 명령 실행은 `0x37`을 통해서만 도달할 수 있으며, ARM64 게스트가 Metal Command Buffer를 제출하는 방식은 이 라이브러리에서 아직 파악하지 못했습니다.

### 호환성 및 제한사항: x86

- 기본 서술자(`protocol_mode = 0`, `ring_page_shift = 0`)에서 x86 동작은 바뀌지 않도록 했으며, `protocol-mode-unit`이 `0x122c`, `0x25`의 x86 쪽을 확인합니다. x86 `0x20` 실행 경로는 이 시험이 다루지 않습니다.
- 두 변경은 **양쪽 모드 모두**에 적용되며 x86 게스트로 검증하지 않았습니다. `CmdDefineHostTask`(`0x38`)가 `arg_count_8b × 8`바이트의 Barrier 슬롯 뒤에서 인자를 읽고 작업 ID 512 이상을 거부합니다. 컴퓨트 디스패처가 `0x38`, `0x44`도 처리합니다.
- 진단 훅은 양쪽 모드에서 실행됩니다.

### 호환성 및 제한사항: ARM64

- Apple Henge와의 불일치가 기록되어 있으며 **수정되지 않았습니다**([대응 문서](docs/arm64-vmapple-correspondence.md)): MMIO `0x100c` 읽기(Henge는 읽을 때 Root 링을 실행하고 누적 읽기 포인터를 반환), `0x1014`·`0x1018` 의미, `0x1220` 커서 이동, 레지스터 `0x1000–0x1fff` 저장(여기서는 `0x1000–0x103f`만 유지), Root 읽기 포인터 계산(Henge는 누적 `u32`, 여기서는 링 크기로 순환).
- arm64 모드의 Device Info 표는 Henge와 같지만 39개 키 중 26개가 x86 표와 다르며, 이를 사용하는 게스트로 실행한 적이 없습니다.
- 실제 게스트에 맞는 링 페이지 shift는 아직 조사 중입니다(그래서 일회성 PFN 진단이 있습니다).

## Android 지원

- **2026-11-25 기준:** Android ARM64 변환 및 일부 실제 GPU 렌더링은 검증되었으며, Metal 전체 API와 실제 PVG 호환은 확인되지 않았습니다.
- Samsung Galaxy S26 Ultra, Android 17, Adreno 840, NDK r27c, arm64-v8a, API 30 이상에서 시험했습니다. 아래 결과는 개발자가 보고한 독립 GPU 시험이며 실제 PVG 세션이 **아닙니다**.
- Android 빌드 구성과 시험 코드는 이 포크가 아니라 [Henge](https://github.com/muns5965/Henge/tree/research/android-port) 저장소에 있습니다. Henge 자체의 개발 현황은 이 문서의 범위가 아닙니다.
- Android를 위한 포크 변경: 메모리 정확성 강화, `LAGFX_NO_PROCESS`, 16 KiB PFN 지원. 시험 기기에 설치하려면 APK의 `.bc` 자산 확장자를 `.airdata`로 바꿔야 합니다.

## 호환성 대응표

출처·구분·상태·근거를 포함한 기능별 전체 표: [docs/metal-vulkan-compatibility.ko.md](docs/metal-vulkan-compatibility.ko.md) ([English](docs/metal-vulkan-compatibility.md)).

## 알려진 제한사항

- Compute 셰이더·Dispatch 없음. Tessellation, Indirect, 중첩 Command Buffer 없음.
- Blit opcode 24개 중 22개, 모든 Metal Fence·Barrier, 대부분의 고정 기능 상태 설정, 정점 텍스처, Sampler 바인딩은 로그만 남김.
- `CmdSynchronizeResources`는 미구현을 반환하며 Stamp는 GPU 완료 후가 아니라 해석 직후 신호.
- `CmdDisplaySetProperties`(모드 반영)는 로그·응답만.
- 픽셀 포맷 범위, Depth/Stencil Attachment, MSAA는 미검증. 패턴 텍스처·UV 정확성은 미시험.
- `ColorFill`, `TextureCopy` 외 로그인 화면 셰이더는 GPU에서 실행한 적 없음. 실제 로그인 정점·프래그먼트 조합은 미시험.
- ARM64 모드는 실험적 지원이며 위의 MMIO 불일치가 남아 있고 arm64 모드의 Metal 명령 실행 경로는 파악되지 않음.
- Android에서 실제 PVG 또는 macOS 데스크톱 렌더링은 없음.

## 시험 현황

| 구분 | 상태 | 내용 |
| --- | --- | --- |
| 단위 시험 | 부분 검증 | 2026-11-26, x86-64 Linux, gcc 13.3, **Vulkan 로더 없음**: 74개 중 72 통과, 0 실패, 2 건너뜀. Vulkan 의존 코드는 컴파일·실행되지 않음. 포크가 추가한 `pfn-shift`, `protocol-mode`, `task-define`, `diag` 단위 시험 통과. `android-memory-mock`은 Vulkan 헤더가 필요해 실행하지 못함. |
| CI | 미검증 | 워크플로(Ubuntu debug/release + lavapipe, Alpine, macOS, 새니타이저)는 있으나 `main`과 `research/android-memory-correctness` 푸시, `main` 대상 PR, 수동 실행에서만 동작. 2026-11-26 기준 이 포크의 워크플로 실행 기록은 **0건**. |
| APK 빌드 | APK 4.4 검증 | Henge [CI 실행 45575251001](https://github.com/muns5965/Henge/actions/runs/45575251001)이 라이브러리 `7574eac` 기준으로 성공. 이 실행은 VM 실행용이 아닌 독립 GPU 시험 APK(라이브러리 `7574eac`)입니다. VM 실행용 APK는 라이브러리 `c53dcb5`로 빌드했고, 여기에는 16 KiB PFN 지원이 이미 들어 있습니다(clone-001 실행에서 기기 확인). arm64 모드, Device Info, task 검색, 0x20/0x25 변경은 아직 어떤 APK에도 없습니다. |
| 실기기 | 부분 검증 | Galaxy S26 Ultra 독립 GPU 시험(아래). macOS 게스트의 실제 PVG 트래픽(16 KiB PFN ring, 명령 수신)은 VM 복제본 실행에서 관찰했으나, arm64 모드·실제 PVG 렌더링·데스크톱 출력은 미검증. |

## 향후 계획

소스와 문서에 남은 미해결 항목에서 도출한 것이며 일정이 아닙니다.

- 남은 ARM64 MMIO 불일치(`0x100c`, `0x1014`/`0x1018`, `0x1220`, 레지스터 저장, Root 읽기 포인터)를 수정하거나 결정하고 ARM64의 Command Buffer 제출 방식을 파악.
- 실제 ARM64 게스트에서 arm64 모드와 16 KiB 링을 실행해 링 페이지 shift 확정.
- 남은 Blit opcode, Metal Fence·Barrier, `CmdSynchronizeResources` 구현. Sampler 상태·정점 텍스처 바인딩. 고정 기능 상태 설정 연결.
- Compute 파이프라인과 Dispatch.
- 나머지 로그인 화면 셰이더, 패턴 텍스처, UV 정확성의 GPU 실행 시험.
- 이 브랜치에서 CI 실행(워크플로 트리거에 추가), arm64 모드 변경을 포함한 APK 빌드.
- 기능이나 상태를 바꾸는 커밋·PR에서 이 README와 호환성 대응표를 함께 갱신.

## Android 호환성 검증 결과

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

## 출처 및 라이선스

원본 라이브러리: Matthew Jackson ([MattJackson](https://github.com/MattJackson)). 소스 파일의 저작권 표기와 [MIT 라이선스](LICENSE)는 그대로 유지합니다. 이 포크는 위에 기록한 변경을 추가했습니다. Android 시험 통합은 Henge 포크에서 개발 중입니다. Apple 셰이더 시험 데이터에 대해 별도의 이용허락을 주장하지 않습니다. 원본 [기여 지침](CONTRIBUTING.md) 및 [보안 정책](SECURITY.md)을 참조하십시오.
