# Metal → Vulkan 호환성 대응표

[English](metal-vulkan-compatibility.md) · [한국어](metal-vulkan-compatibility.ko.md) · [README](../README.ko.md)

기준: `research/android-pfn-shift` 브랜치, 소스 리비전 `6a29b1d` (2026-11-26).
원본 기준: [MattJackson/libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan) `7574eac`.

이 대응표는 원본 README의 주장이 아니라 이 포크의 소스와 커밋 이력을 기준으로 작성했습니다.
지원 범위를 과장하지 않기 위한 문서입니다. Opcode를 인식하거나 로그를 남기거나 Stamp를 반환하는 것은 **Metal 지원이 아닙니다**.
기능이나 지원 상태를 바꾸는 커밋·PR에서 이 문서도 함께 갱신하십시오.

## 1. 문서 읽는 법

### 지원 상태

| 상태 | 의미 |
| --- | --- |
| 구현 및 검증 완료 (Implemented & Verified) | 코드가 있고 이 포크에서 시험이 통과함(단위 시험 실행 또는 Android 기기 시험). 시험 범위를 함께 적음. |
| 구현 완료·미검증 (Implemented, Untested) | 설명한 동작을 수행하는 코드가 있으나 이 포크에서 실행한 시험이 없음. |
| 부분 구현 (Partially Implemented) | 일부만 동작하고 나머지는 없거나 로그만 남김. |
| 임시 구현 (Stub / Placeholder) | 명령을 해석·기록·응답만 하며 Vulkan 작업이나 상태 변경이 없음. |
| 미구현 (Not Implemented) | 일반 "알 수 없는 opcode" 처리뿐이거나 명시적 미구현 반환. |
| 실험적 지원 (Experimental) | 제한된 환경에서 조사 중인 구현. 동작이 틀리거나 바뀔 수 있음. |

원본 측 보고 결과(예: lavapipe 위 macOS 게스트 실시간 렌더링)는 근거 열에 표기하되, 이 포크가 재현하지 않았으므로 그것만으로 *검증 완료*로 올리지 않습니다.

### 출처

| 출처 | 의미 |
| --- | --- |
| 원본 구현 (Upstream) | `7574eac` 시점 원본(Matthew Jackson)에 이미 있던 기능. 이 포크가 작성한 것이 아님. |
| 포크 구현 (Fork) | 이 포크가 `7574eac` 이후 새로 추가한 기능. |
| 포크 수정 (Fork modification) | 원본 기능을 이 포크가 수정·확장한 부분. |
| 계획 (Planned) | 아직 구현하지 않음. 누락을 드러내기 위해 표기. |

### 구분(Kind)

| 구분 | 의미 |
| --- | --- |
| Vulkan | Vulkan 객체나 명령(파이프라인, 이미지, Draw, 복사 등)을 생성함. |
| Protocol | Vulkan 대응이 없는 PVG 와이어 프로토콜 해석·응답(링, Stamp, 정보 응답, 레지스트리). |

### 근거 출처

- **Android 기기** — 라이브러리 `7574eac`에서 Galaxy S26 Ultra / Adreno 840으로 수행한 개발자 보고 시험(README 참조). 실제 PVG 세션이 아닌 Henge의 독립 시험 실행 파일입니다.
- **단위 시험 2026-11-26** — x86-64 Linux, gcc 13.3, **Vulkan 로더 없음** 상태의 `meson test`(74개 중 72 통과, 0 실패, 2 건너뜀). `LAGFX_HAVE_VULKAN` 코드는 컴파일·실행되지 않았으므로 프로토콜과 파싱 로직만 입증합니다.
- **원본 보고** — 원본 README·변경 이력의 주장. 여기서 재현하지 않음.
- **없음** — 이 포크에서 알려진 시험 없음.

## 2. 대응표

### 2.1 Shader

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| Metal AIR → SPIR-V 변환(정점, 프래그먼트) | 원본 구현 | Vulkan | 구현 및 검증 완료 | Android 기기: 로그인 화면 모듈 11종·함수 66개 변환, SPIR-V가 호스트 SHA-256과 일치하고 `spirv-val`(vulkan1.2) 통과. 단위 시험: `air2spv` 전체 통과. |
| `.metallib` 추출, 진입점 재작성, 시그니처 변환 | 원본 구현 | Vulkan | 구현 및 검증 완료 | 같은 변환 시험 묶음에서 실행됨. |
| 변환기에 대한 포크 변경 | 포크 수정 | Vulkan | 구현 완료·미검증 | `LAGFX_NO_PROCESS` 옵션이 `system()`/`popen()` 디버그 및 `llc` 탐색 호출을 제거(Android 실행기는 프로세스 생성 금지). `shader_translate.c`의 4줄 변경. |
| 변환된 셰이더의 GPU 실행 | 원본 구현 | Vulkan | 부분 구현 | Android 기기: `ColorFill`, `TextureCopy` 프래그먼트가 정상 출력(각 4,096/4,096 픽셀). `UberComposite`, `Vfx`, `Xgc` 및 로그인 정점 셰이더는 변환만 확인, GPU 실행 **미시험**. |
| 기본 셰이더 카탈로그(클리어·블릿 보조) | 원본 구현 | Vulkan | 구현 완료·미검증 | 카탈로그 시험은 Vulkan 없이 실행되어 Vulkan 의존 부분은 건너뜀. SPIR-V 파일은 저장소에 포함. |
| 원자 연산(`air.atomic.*` → `OpAtomic*`) | 포크 | Vulkan | 구현 및 빌드 검증 완료(변환기 + spirv-val + lavapipe, 직접 작성한 AIR), 실기기(실제 GPU) 검증 없음, 실제 Metal과의 동등성 검증 없음, 철자 일부는 미검증 | 장치(StorageBuffer, Scope Device)·스레드그룹(Workgroup, Scope Workgroup) 메모리의 32비트 정수 load / store / exchange / compare-exchange / add sub and or xor min max(부호 있음·없음). `OpMemoryModel Logical GLSL450` 아래에서 동작하며 Vulkan 메모리 모델 capability는 필요 없음. **엄격한 피연산자 문법(기본값):** 실제 Apple 모듈에서 본 조합 `load(ptr,0,2,true)`, `store(ptr,v,0,2,true)`, `rmw(ptr,v,0,2,0,true)`만 변환하고, 순서·scope·extra·flag 리터럴이나 피연산자 개수가 다르면 이유 이름과 이유별 카운터를 남기며 변환을 **실패**시킴. `LAGFX_ATOMIC_LENIENT=1`(기본 꺼짐)은 실험용으로 이전의 느슨한 문법을 되살림. float 원자 연산은 철자를 본 적이 없어 lenient가 아니면 거부하고, 64비트 원자 연산은 반환형이 void로 선언된 경우만 받음. 64비트 정수 min/max: `LAGFX_VK_CAP_ATOMIC64_BUF`(`shaderBufferInt64Atomics`, 장치 메모리)와 `LAGFX_VK_CAP_ATOMIC64_SHARED`(`shaderSharedInt64Atomics`, 스레드그룹 메모리)는 별개의 cap이며 둘 다 `shaderInt64`가 필요. float add: `VK_EXT_shader_atomic_float`의 기능별 cap `atomic_f32_{buf,buf_add,shared,shared_add,image}`(`lagfx_vk_init`은 지원되는 것만 켬). compare-exchange: 호스트 의미는 구현됨, 실제 AIR와의 연결은 미확인(합성 포인터 + i1 형태만 받고 다른 형태는 거부). 도우미 함수 안의 원자 연산: 스레드그룹 정적 변수는 동작하고, 장치 포인터 매개변수를 받는 도우미는 조용히 버리지 않고 거부. 그 밖의 연산과 relaxed가 아닌 메모리 순서는 변환을 **실패**시킴(셈, 흉내 내지 않음). 시험: `air2spv atomics on lavapipe`, `air2spv atomics strict operand grammar (unit)`, `air2spv atomics SPIR-V consistency + spirv-val (unit)`, `atomic device capabilities (unit, injected features, lavapipe)`. 철자: Apple이 컴파일한 모듈에서는 `air.atomic.global.{load,store,xchg}.i32`와 `{add,and,or}.u.i32`만 확인했고, `local`, `sub`/`xor`/`min`/`max`, `cmpxchg.weak`, `.i64`, `.f32`는 이 문법에서 유추한 것으로 **미검증**. 캡처·고정 입력 고유 모듈 190개 중 원자 연산 사용은 0개이므로 원자 연산은 -13이나 부팅 정지의 원인이 아님. `docs/spec-audit.md`의 "Atomics review pass 2" 참조. |
| Compute 셰이더(MSL 커널 → SPIR-V compute) | — | Vulkan | 미구현 | 소스 전체에 `vkCreateComputePipelines`, `vkCmdDispatch` 없음. |
| Tessellation, Mesh, Ray Tracing | — | Vulkan | 미구현 | Patch Draw opcode는 로그만 남김(2.6 참조). |

### 2.2 Render Pipeline

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| `SetRenderPipelineState`(0x74) → `VkPipeline` 생성 및 작업별 캐시 | 원본 구현 | Vulkan | 구현 완료·미검증 | 변환된 정점+프래그먼트 SPIR-V로 생성. Android 기기 시험은 독립 시험 실행 파일이며 이 실시간 경로가 아님. |
| 사전 곱셈 source-over 블렌딩 | 원본 구현 | Vulkan | 구현 완료·미검증 | Android "알파 블렌딩" 시험은 GLSL 파이프라인이며 게스트 경로가 아님. |
| 고정 기능 상태 설정: `SetCullMode`, `SetFrontFacingWinding`, `SetDepthBias`, `SetDepthClipMode`, `SetTriangleFillMode`, `SetStencilRef`, `SetBlendColor`, `SetDepthStencilState`, `SetVisibilityResultMode`, `SetLineWidth`, `SetPointSize` 등 | 원본 구현 | Vulkan | 임시 구현 | `render_inner_ops.c` 핸들러가 해석과 로그만 수행. |
| Store Action 설정(`SetColorStoreAction`, `SetDepthStoreAction`, `SetStencilStoreAction`, `…Options`) | 원본 구현 | Vulkan | 임시 구현 | 해석과 로그만 수행. |
| Draw별 Viewport·Scissor(`SetViewport` 0x82, `SetScissorRect` 0x75) | 원본 구현 | Vulkan | 구현 완료·미검증 | 작업별로 저장해 Draw에 적용. 다중 Viewport·Scissor(`SetViewports`, `SetScissorRects`)는 로그만 남김. |

### 2.3 Compute Pipeline

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| Compute 인코더 명령 및 Dispatch | — | Vulkan | 미구현 | 인코더 타입 0/1에 쓰이는 `compute_inner_ops.c` 표는 **Render 계열** opcode를 처리합니다. Dispatch 경로 없음. |
| `DispatchThreadsPerTile*`, Tile 버퍼·텍스처 | 원본 구현 | Vulkan | 임시 구현 | 해석과 로그만 수행. |

### 2.4 Buffer

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| 게스트 작업 메모리 매핑(memfd + `mmap(MAP_FIXED)` + `mremap` 별칭), 3단계 radix VA→GPA | 원본 구현 | Protocol | 구현 및 검증 완료 | 단위 시험: `memory task`, `memory coherence`, `page-walked translation regression`. |
| 정점·프래그먼트 버퍼 바인딩(`SetVertexBuffers` 0x7d, `SetVertexBufferOffset` 0x7e, `SetFragmentBuffers` 0x6e, `SetFragmentBufferOffset` 0x6f) | 원본 구현 | Vulkan | 구현 완료·미검증 | 작업 페이지 테이블을 거쳐 호스트 버퍼로 해석. |
| Host-visible 업로드 메모리: coherent 요구, map/flush 오류 전파(버퍼 보조 함수 3개, 대체 UBO) | 포크 수정 | Vulkan | 구현 완료·미검증 | Mock 오류 주입 시험(`tests/android-memory-mock.c`)이 있으나 2026-11-26 작업에서는 실행하지 못함(Vulkan 헤더 필요). CI 실행 기록도 없음. 실제 Vulkan 할당 경로는 실행되지 않음. |
| 정점·인덱스 버퍼 `Set…WithStride`, Tessellation Factor 버퍼 | 원본 구현 | Vulkan | 임시 구현 | 해석과 로그만 수행. |
| 버퍼↔버퍼, 버퍼↔텍스처 복사(Blit 인코더) | 원본 구현 | Vulkan | 임시 구현 | 2.7 참조. |

### 2.5 Texture

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| 서술자 기반 텍스처 생성(RGBA16F, R8_UNORM 글리프 아틀라스, bpp 반영 크기, 크기 상한) | 원본 구현 | Vulkan | 구현 완료·미검증 | 프래그먼트 텍스처 바인딩에서 사용. |
| `SetFragmentTextures`(0x72) | 원본 구현 | Vulkan | 구현 완료·미검증 | Android 기기: `TextureCopy`는 **균일 색상** 텍스처만 샘플링. 패턴 텍스처·UV 정확성 **미시험**. |
| `SetVertexTextures`(0x81) 및 Tile 텍스처 | 원본 구현 | Vulkan | 임시 구현 | 해석과 로그만 수행. |
| Sampler 상태(`SetFragmentSamplerStates` 0x70, 정점·Tile 변형, LOD Clamp) | 원본 구현 | Vulkan | 임시 구현 | 앞쪽 항목만 로그로 남기고 반환. 디스크립터 바인딩은 `TODO`. 변환된 셰이더는 모듈 기본 샘플러를 사용. |
| 렌더 타깃·Blit의 픽셀 포맷 변환 | 원본 구현 | Vulkan | 부분 구현 | RGBA16F 및 8비트 경로가 있음. 모든 Metal 픽셀 포맷 지원 범위는 **확인되지 않음**. |

### 2.6 Render Pass 및 Draw

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| `RenderDescribeRenderPass`(0x1a): 584바이트 서술자 해석, Color Attachment 해석 | 원본 구현 | Vulkan | 구현 완료·미검증 | Depth/Stencil Attachment 처리 및 MSAA는 미확인. |
| Draw: `DrawPrimitives16/64`(0x01/0x00), `DrawInstancedPrimitives16`(0x03), `DrawIndexedPrimitives16/64`(0x07/0x06) | 원본 구현 | Vulkan | 구현 완료·미검증 | `lagfx_emit_pending_draw`를 통해 `vkCmdDraw` / `vkCmdDrawIndexed` 호출. 원본은 lavapipe에서 게스트 지오메트리의 실시간 렌더링을 보고. Android에서는 실제 PVG 경로로 실행하지 않음. |
| 기타 Draw: `DrawInstancedPrimitives64`, `…Base…`, `DrawIndexedInstanced…`, `…_2` 변형 | 원본 구현 | Vulkan | 임시 구현 | 해석과 로그만 수행. 인코더 타입 0/1 표에 없어 그 경로에서는 버려짐. |
| Patch Draw(`DrawPatches*`, `DrawIndexedPatches*`) | 원본 구현 | Vulkan | 임시 구현 | 해석과 로그만 수행. |
| Indirect Draw(`Draw*Indirect`) | 원본 구현 | Vulkan | 임시 구현 | 해석과 로그만 수행. |
| `ExecuteCommandsInBuffer`, 중첩 Command Buffer(0x3c) | 원본 구현 | Vulkan | 임시 구현 | 재귀 처리 보류(소스에 TODO). |

### 2.7 Blit

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| `CopyFromTextureToTexture`(0x12f) | 원본 구현 | Vulkan | 구현 완료·미검증 | `lagfx_translate_blit_copy_texture_to_texture`가 `vkCmdCopyImage` 기록. 단위 시험은 Vulkan 없이 실행되어 이 경로는 실행되지 않음. |
| `FillTextureWithColor`(0x141) | 원본 구현 | Vulkan | 구현 완료·미검증 | `lagfx_translate_blit_fill_texture_color`가 `vkCmdClearColorImage` 기록. 단위 시험에서 실행되지 않음. |
| `CopyFromBufferToTexture`, `CopyFromBufferToBuffer`, `CopyFromTextureToBuffer`, `FillBuffer`, `FillBufferWithPattern`, `FillTextureWithBytes`, `GenerateMipmaps`, `…WithOptions`, `…NumSliceLevel` | 원본 구현 | Vulkan | 임시 구현 | `op_ack_stub`: 응답만 하고 아무 작업도 하지 않음. |
| `Optimize*`, `Synchronize*`, `Invalidate*`, Indirect Command Buffer 관련 | 원본 구현 | Vulkan | 임시 구현 | `op_ack_stub`. |

Blit opcode 24개 중 22개는 응답만 합니다.

### 2.8 IOSurface

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| IOSurface opcode 0x26–0x2a: 와이어 해석, 리소스 레지스트리 등록·조회·해제 | 원본 구현 | Protocol | 구현 완료·미검증 | 레지스트리 단위 시험 통과. 일부 레이아웃은 원본에서 추정값. |
| IOSurface opcode 핸들러 내부의 `VkImage` 할당 | 원본 구현 | Vulkan | 미구현 | `handlers/iosurface/iosurface.c` 머리말에 해당 경로를 복원하지 않았다고 명시. |
| 필요 시 생성되는 `VkImage` 기반 표면(텍스처 생성, 렌더 타깃) | 원본 구현 | Vulkan | 구현 완료·미검증 | Android 기기: 렌더 타깃 초기화·읽기 4,096/4,096. |
| 게스트 작업과 호스트 화면 간 IOSurface 공유 | 원본 구현 | Vulkan | 부분 구현 | 레지스트리가 ID를 호스트 메타데이터에 대응. 무복사 공유는 미검증. |

### 2.9 Synchronization

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| Stamp 셀(단조 증가, `>=` 비교), MSI 벡터 0, Pending Display 비트마스크 | 원본 구현 | Protocol | 구현 및 검증 완료 | 단위 시험: Stamp·Deadlock 시험. **프로토콜 수준에 한정**: Stamp는 해석 직후 신호하며 GPU 완료 후가 아님. Metal 기능이 아님. |
| `CmdSynchronizeResources`(0x42) | 원본 구현 | Protocol | 미구현 | 로그 후 `NOT_IMPLEMENTED` 반환("cache flush not implemented yet"). |
| `CmdSynchronizeDiscard`(0x23) | 원본 구현 | Protocol | 임시 구현 | 로그만 남김. |
| Metal Barrier: `RenderBarrierScope`(0x17), `RenderBarrierResources`, `TextureBarrier` | 원본 구현 | Vulkan | 임시 구현 | 로그 또는 응답만. Vulkan 보조 함수 내부에 이미지 레이아웃 Barrier가 있으나 Metal Barrier를 변환한 것이 아님. |
| Metal Fence: `RenderUpdateFence`/`WaitForFence`, `BlitUpdateFence`/`WaitForFence` | 원본 구현 | Vulkan | 임시 구현 | 로그 또는 응답만. |
| Draw별 호스트 Fence 대기(기본 30초, `LAGFX_DRAW_FENCE_MS`) | 원본 구현 | Vulkan | 구현 완료·미검증 | 내부 직렬화이며 Metal Fence의 변환이 아님. |

### 2.10 Display

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| Display 생성·해제, `lagfx_display_read_frame` 읽기 | 원본 구현 | Vulkan | 구현 완료·미검증 | Android 기기: 렌더 타깃 읽기만 시험. Display 세션 없음. |
| `CmdDisplaySwapMapping`(0x12), Scanout Blit 및 Composite | 원본 구현 | Vulkan | 구현 완료·미검증 | |
| `CmdDisplaySetProperties`(0x11) 모드 반영 | 원본 구현 | Protocol | 임시 구현 | 로그·응답만. `TODO: Stage 30 mode commit`. |
| 제출된 Display 리소스 해제 전 Fence 오류 처리 | 포크 수정 | Vulkan | 구현 완료·미검증 | 커밋 `8f36a76`. |
| MMIO `0x122c` 읽기("cap gate") | 포크 수정 | Protocol | 실험적 지원 | x86 모드는 기존대로 9, arm64 모드는 1(Apple Henge는 디스플레이 수를 응답). 단위 시험만 수행. |

### 2.11 Cursor

| 기능 | 출처 | 구분 | 상태 | 근거 / 비고 |
| --- | --- | --- | --- | --- |
| Cursor 글리프 업로드 및 오버레이 그리기(`vkCmdDraw`) | 원본 구현 | Vulkan | 구현 완료·미검증 | |
| Host-visible·coherent 메모리 선택, 비 coherent flush, 레이아웃·오류 상태 추적 | 포크 수정 | Vulkan | 구현 완료·미검증 | 커밋 `a746845`, `dcd6ae4`, `f59bad4`, `f139872`, `8f36a76`. Mock 수준 시험만. |
| MMIO `0x1220`(`cursor_moved`)에 의한 위치 갱신 | — | Protocol | 미구현 | `docs/arm64-vmapple-correspondence.md`에 미구현으로 기록. |

## 3. Capability 응답과 Vulkan 실제 지원의 구분

정보 응답(`CmdGetDeviceInfo` 0x0a, `GetDeviceInfo2` 0x3a, Compute Info 0x0b, 인코더 타입 4 정보 계열 0x1c2–0x1d0)은 가상 GPU가 무엇을 지원한다고 게스트에 알리는 **프로토콜 응답**입니다. 관찰된 게스트 동작을 근거로 구현했으며, 응답한 기능을 Vulkan 백엔드가 실제로 수행할 수 있다는 뜻이 아닙니다.

| 응답 | 출처 | 비고 |
| --- | --- | --- |
| x86 Device Info 표(`CmdGetDeviceInfo`) | 원본 구현 | macOS 게스트가 진행하도록 정한 값이며 Vulkan 장치에서 도출한 값이 아님. |
| arm64 Device Info 표 | 포크 구현 | Apple Henge `devices/pvg_lite.c` 표를 그대로 복사. 39개 키 중 26개가 x86 표와 값이 다르고 13개는 같으며, 3개 키는 x86 표에만 있음(대응 문서 참조). Vulkan 능력 점검은 하지 않음. |
| 파이프라인·호스트 리소스 정보 응답(0x1c2–0x1d0) | 원본 구현 | 0이 아닌 응답으로 게스트가 `MetalShader::CopyPipelineState`에서 중단되지 않게 함. |

## 4. 알려진 공백 요약

- Compute Dispatch, Tessellation, Indirect 및 중첩 Command Buffer: 미구현.
- Blit opcode 24개 중 22개, 모든 Metal Fence와 Barrier, 대부분의 고정 기능 상태 설정, 정점 텍스처 및 Sampler 바인딩: 로그만 남김.
- `CmdSynchronizeResources`는 미구현을 반환.
- 픽셀 포맷 범위와 Depth/Stencil Attachment는 실기에서 미검증이며, MSAA는 디코더 기준으로만 구현, 게스트 캡처 없음(5절).
- Android에서 실제 PVG 경로를 시험한 기록이 없음. arm64 모드는 어떤 APK에도 포함되지 않음(README *시험 현황* 참조).

관련 문서: [arm64-vmapple-correspondence.md](arm64-vmapple-correspondence.md).

## 5. 렌더 호환성: Framebuffer Fetch, 포맷, MSAA

아래 내용은 lavapipe와 Khronos 검증 레이어 1.3.275에서만 확인했다. 이 레이어는 VK_KHR_dynamic_rendering_local_read보다 오래되어 해당 Barrier에 거짓 오류를 내므로 그 판정에는 쓸 수 없고, 어떤 결과도 Adreno 드라이버를 예측하지 않는다.

- **Framebuffer Fetch 순서(이슈 11).** 이전 색을 읽는 프래그먼트는 같은 픽셀의 앞선 프래그먼트가 끝나 있어야 한다. VK_EXT/ARM_rasterization_order_attachment_access가 없으면 이 보장은 Draw 사이(프레임버퍼 로컬 Barrier)에서만 성립한다. 장치에 기능이 없을 때 프리미티브나 인스턴스가 겹칠 수 있는 Draw는 그 Barrier를 사이에 두고 프리미티브(또는 인스턴스) 단위로 기록한다(`fbf_draw_split`, 상한 16384, `fbf_split_*` 카운터). Triangle Strip은 삼각형별 감김 방향을 유지한다. lavapipe는 한 Draw를 어차피 순서대로 그리므로 순서 보장 부재가 잘못된 픽셀로 나타나지 않는다. 그래서 시험은 기록된 하위 Draw 수와, 픽셀이 단일 Draw 기준과 같음을 확인한다. 셰이더의 Input Attachment 수치 클래스(float/uint/sint)가 Attachment 포맷과 다르면 거부하고 `fbf_class_mismatch`로 센다.
- **포맷, 사용처, 블렌드(이슈 13).** `src/vulkan/format_caps.c`가 vkGetPhysicalDeviceFormatProperties2와 vkGetPhysicalDeviceImageFormatProperties2로 모든 미러 이미지의 usage 마스크를 묻는다. 거부된 조합은 선택 usage를 하나씩 빼고 센다(`img_trimmed`, `dropped[]`). 필수 usage가 없으면 거부를 센다. COLOR_ATTACHMENT_BLEND가 없는 포맷은 블렌드를 끄고 `blend_off_format`으로 센다. 컬러 Attachment가 될 수 없는 대상은 `target_not_attachable`로 센다.
- **MSAA(이슈 12), 디코더 기준 구현, 게스트 캡처 없음.** 증거 [V]: 가진 덤프(/tmp/x231501, /tmp/vm2, /tmp/run3/z/dump)에는 BGRA8(80)·RGBA16F(115) Attachment, Mip 1개만 있고 태그 30..36, 102..106, 150..152나 멀티샘플 텍스처 타입이 없다. 즉 멀티샘플 Pass의 게스트 캡처는 존재하지 않는다. [I] 읽는 필드: 텍스처 디스크립터 타입 4(2D 멀티샘플, Metal 번호)와 description +18의 샘플 수 u16(Mip 1, 레이어 1; 타입 8 등 다른 조합은 거부하고 `ms_tex_bad_shape`로 센다), Pass 색상 `store_action`(+0x16)과 `resolve_texture_ref`(+0x04)에 Metal 표(0 DontCare, 1 Store, 2 MultisampleResolve, 3 StoreAndMultisampleResolve, 4 이상 미지로 집계), 지연 태그 102/150(x86 시기 페이로드 배치, ARM64 미검증). [N] 미해독: 파이프라인 rasterSampleCount(파이프라인 샘플 수는 Pass Attachment에서 가져옴), alpha-to-coverage, sample mask, 태그 30/32 페이로드, 깊이/스텐실 Resolve. 구현(`src/protocol/arm64_msaa.*`, `src/vulkan/msaa.*`): 이미지는 허용된 샘플 수로 만들고(`lagfx_msaa_plan`: 포맷 능력과 sampleCounts, 지원하는 최대 수로 낮추거나 1x로 명시적 대체 후 복사로 Resolve), 파이프라인은 Pass에서 `raster_samples`를 받고, Resolve는 Store Action별 dynamic rendering resolve attachment로 한다. 추측하지 않고 거부하며 센다: 멀티샘플 Attachment에 대한 Framebuffer Fetch·타일 셰이더·Blit·캡처, 멀티샘플 텍스처의 셰이더 샘플링(AIR의 texture2d_ms 읽기, get_num_samples, sample_id, sample_mask 철자는 `src/air2spv/msaa_air_scan.c`에서 이름으로만 탐지해 `air_ms_ops`/`air_sample_builtins`로 세며 변환하지 않음), 한 Pass의 혼합 샘플 수, 깊이/스텐실 Resolve. 시험(lavapipe 1|4 샘플 + Validation Layer): `vulkan-msaa-coverage-unit`(표준 샘플 위치의 CPU 기준, 샘플별 Fetch, Store Action과 vkCmdResolveImage Resolve, BGRA8·RGBA16F), `arm64-msaa-guest-unit`(Exec 경로의 게스트 디스크립터). 상태: 구현·lavapipe 빌드 검증, 실기 검증 아님. 다음 실행에서 실제 멀티샘플 Pass(원시 태그 26 레코드, 레이아웃 길이를 포함한 텍스처 디스크립터, 태그 102/150, 파이프라인 디스크립터 blob, texture2d_ms 읽기가 있는 AIR 비트코드)와 `g7-gpu-msaa` 줄을 캡처해야 한다. Device Info의 MSAASamples는 호스트 최대값(최소 1)으로 낮추고 `devinfo_msaa_clamped`로 센다.
