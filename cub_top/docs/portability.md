# 이식성 — C 구현 (Portability, C implementation)

`src/cub_top.c`는 **단일 파일 C99, 외부 라이브러리 의존 0**입니다. `<elf.h>`는 헤더만 사용하며 libelf/libdw에 링크하지 않습니다.

## 두 가지 배포 방식

| 방식 | 산출물 | 실행 가능 범위 | 용도 |
|---|---|---|---|
| **정적 단독** | `cub_top` (약 1.0MB, 의존 0) | **커널 ≥ 빌드 환경 glibc의 하한** | 배포 편의. 한 번 빌드해 여러 서버에 복사 |
| **대상에서 컴파일** | 동일 소스 | **CentOS 6(커널 2.6.32, gcc 4.4)까지** | 구 OS. `gcc cub_top.c` 한 줄 |

```sh
./build.sh            # 동적 (현재 시스템)
./build.sh static     # 정적 단독
./build.sh legacy      # 구 환경(CentOS 6 등) — 대상 머신에서 실행
```

## 정적 바이너리의 커널 하한 (중요)

**정적 링크는 구 OS 호환을 보장하지 않습니다.** glibc를 정적으로 넣으면 그 glibc가 요구하는 커널 버전이 그대로 따라옵니다.

이 저장소 빌드 환경(glibc 2.34)에서 만든 정적 바이너리는:

```
$ readelf -n cub_top | grep -A1 'ABI version'
    OS: Linux, ABI: 3.2.0
```

→ **커널 3.2 이상 필요 = CentOS 7 이상.** CentOS 6(커널 2.6.32)에서는 이 바이너리가 실행되지 않습니다.

`build.sh static`은 빌드 후 이 하한을 자동으로 출력합니다. 배포 전 반드시 확인하십시오.

**CentOS 6를 정적 단독으로 지원해야 한다면** 선택지는 둘입니다.
1. **CentOS 6 환경에서 정적 빌드** — 그 glibc(2.12) 하한이 적용되어 2.6.32에서 동작
2. **musl 등 대체 libc로 정적 빌드** — `musl-gcc -static`. musl은 구 커널 호환 폭이 넓습니다

두 방법 모두 **해당 툴체인이 있는 환경**이 필요하며, 이 저장소 빌드 환경에는 없습니다.

## 런타임 기능 탐지와 폴백

구 커널에 없는 인터페이스는 실행 시 탐지해 폴백합니다. 시작 시 `caps:` 줄에 상태가 표시됩니다.

```
caps: vm_readv=yes rollup=yes proc_io=yes                         ← 최신 커널
caps: vm_readv=no(pread 폴백) rollup=no(smaps 합산) proc_io=yes … ← 구 커널
```

| 인터페이스 | 도입 | 없을 때 폴백 | 영향 |
|---|---|---|---|
| `process_vm_readv` | 커널 3.2 / glibc 2.15 | `pread(/proc/pid/mem)` | **없음** — 방법 B 정상 동작(폴백 상태에서 자기검증 통과 확인). 단 2.6.39 미만은 `/proc/pid/mem` 을 tracer 만 읽을 수 있어 폴백도 거부되고 엔진 직독이 꺼진다(사유에 커널 때문임을 적는다) |
| `/proc/pid/comm` | 커널 2.6.33 | 쓰지 않음 — 프로세스 이름은 `/proc/pid/stat` 의 `(comm)` 에서 읽는다 | 없음 |
| `smaps_rollup` | 커널 4.14 | `smaps` 전체 합산 | 비용만 증가(수 ms) |
| `/proc/pid/io` | `CONFIG_TASK_IO_ACCOUNTING` | I/O 섹션 생략 | I/O 지표 없음 |
| `/proc/pid/pagemap` | 커널 2.6.25 | 상주 확인 없이 헤더 표본을 읽음 | `pgbuf.hdr_pagemap_unchecked` 로 표기, 스왑된 표본 페이지가 다시 적재될 수 있음 |
| `MemAvailable` | 커널 3.14 | `MemFree` 사용 | 가용량이 보수적으로 표시 |
| `RssFile`(`/proc/pid/status`) | 커널 4.5 | `/proc/pid/statm` 의 shared 필드(파일 상주 페이지; 최신 커널에서는 shmem 포함, cub_server 는 0) | 인스턴스 행의 `cache` 열·`instance.<db>.file_bytes` 가 0 이 아니라 측정값 |

`process_vm_readv` 와 `/proc/pid/mem` 은 시작 시(대상 전환·재시작 때도) 대상의 매핑된 주소 1바이트를 실제로 읽어 시험합니다.
EPERM/EACCES(Yama `ptrace_scope>=1`, 2.6.39 미만의 tracer 전용 mem)는 폴백이 아니라 권한 제한이므로 `perm.limited=1` 과 사유를 내고
엔진 직독을 끕니다([limitations §15](limitations.md)).

2.6.32(CentOS 6) 실기에서는 검증하지 않았다 — 위 동작은 커널 소스와 문서 기준이며, `tools/check-oldkernel.sh` 는 이름 읽기와
커널 판정 논리만 이 호스트에서 확인한다. 2.6.32 에서 나오는 것은 `/proc` 만으로 되는 축(메모리 영역·스레드·CPU·I/O·용량)이다.

`process_vm_readv`는 **glibc 래퍼가 아니라 `syscall()` 직접 호출**입니다. 따라서 glibc 2.15 미만에서도 컴파일되며, 커널이 지원하지 않으면 `ENOSYS`로 폴백합니다. 시스템콜 번호는 `<asm/unistd.h>` 매크로를 우선 쓰고 없을 때만 아키텍처별 상수로 보완합니다.

## 구 컴파일러 호환 (gcc 4.4 / CentOS 6)

- `-std=gnu99`만 사용. C11 기능(`_Static_assert`, `_Generic`, 익명 union 등) 미사용
- 시각은 `gettimeofday` 사용 → glibc < 2.17에서 `-lrt` 링크 불필요 (`tools/check-librt.sh`가 바이너리의 librt 전용 함수 참조를 검사)
- `-std=c99 -pedantic -Wall -Wextra`에서 error·ISO C 위반 0 확인

## 방법 B의 오프셋 처리 — libdw 대신 자기검증

런타임 DWARF 해석(libdw)을 쓰지 않습니다. 이유는 셋입니다.

1. RHEL/Rocky는 **`libdw.a` 정적판을 배포하지 않아** 단독 바이너리와 양립하지 않습니다
2. CentOS 6의 libdw는 너무 낮아 최신 DWARF를 해석하지 못합니다
3. 외부 라이브러리 의존 0 원칙에 어긋납니다

대신 **오프셋 표 + 자기검증**을 씁니다.

```
pgbuf_Pool.num_buffers  vs  paramdump data_buffer_pages
```

두 값이 일치하면 오프셋 표가 이 바이너리에 유효하다고 판정하고 방법 B를 활성화합니다. 불일치하면 **방법 B 전체를 비활성화**하고 이유를 표시합니다.

```
heapB.enabled=0 heapB.reason="오프셋 불일치(num_buffers=... != data_buffer_pages=...) → 방법B 비활성"
```

이 방식은 잘못된 오프셋으로 쓰레기 값을 내보내는 사고(개발 중 실제로 76.7GB 오독 사례가 있었음)를 구조적으로 막습니다. 심볼 주소는 ELF `.symtab`을 직접 파싱해 얻으므로 `nm`·binutils도 필요하지 않습니다.

## C 구현의 현재 범위

| 기능 | 상태 |
|---|---|
| `/proc` 수집·영역 분류(7종)·파라미터 귀속 | ✅ |
| 방법 B (루트 전역 순회 + 4층 하니스) | ✅ |
| I/O 지표 (게이지 절대 하한·유휴 판정 포함) | ✅ |
| 드릴다운 트리 · 덤프 | ✅ |
| btop 대시보드 · 라이브 TUI (`-b`) | ✅ |
| 시계열 플롯(`-p`, 4패널 + 줌) | ✅ |
| 경량 프레임 교차 | ❌ (1샘플=1창) |

추가할 때는 **수집 로직을 `sample_once()` 단일 출처로 유지**하십시오 —
수집 경로를 이중으로 두면 같은 지표가 화면마다 다른 값이 됩니다
(`tools/check-hist.sh` 가 그 불일치를 회귀로 막습니다).

### 라이브 대시보드 이식 메모

6점 글리프 게이지(전경=보기 지표·배경=두 번째 비율),
값 4열 `alloc|rss|conf|grow` 고정, 게이지 = 상주/할당, 관측창 약 3분, 하단 24시간제 시각 눈금, I/O 게이지 절대 하한).

| 항목 | 구현 |
|---|---|
| 캔버스 | `canvas_t` 정적 배열(`[80][120]`, 약 240KB) — 표시 폭 인식 `cv_put`(전각은 다음 칸 sentinel) |
| 폭 계산 | `u8len`/`u8cp`/`u8wide` — 한글·CJK 범위 직접 판정(`wcwidth` 미사용 → 로케일 무관) |
| 패딩 | `padf(s,n,right)` — 표시 폭 기준 |
| 그래프 | `cv_chart` 2×4 브라유 + 브레젠험, **관측창 전체 리샘플**(꼬리 절단 금지) |
| TUI | `termios` raw + 커서 숨김, `atexit`/`SIGINT` 복원. **대체 화면 미사용** — 종료 후 마지막 프레임이 화면에 남는다 |
| 키 | `1~3` 보기 · `<`/`>` 인스턴스 · `space` 정지 · `p` 시계열 · `h` 도움말 · `q` 종료 |

구 glibc 안전성: `termios`·`ioctl(TIOCGWINSZ)`·`signal`·`atexit` 는 모두 glibc 2.12 에 존재하며
새로 도입한 헤더는 `<termios.h>`·`<signal.h>`·`<sys/ioctl.h>` 뿐입니다. legacy 빌드로 확인했습니다.

## 성능 비교 (동일 서버)

| 단계 | C | Python |
|---|---|---|
| ELF 심볼 조회 | **137 ms** (자체 파싱) | `nm` 셸아웃 |
| 전역 상태 읽기(방법 B) | **0.012 ms** | 동급 |

