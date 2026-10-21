# CUBRID 소스 트리 통합 (In-Tree Integration)

> **참고 문서** — 이 프로젝트의 유지관리 대상은 단독(standalone) 바이너리다.
> 아래 내용은 트리에 정식 유틸리티로 통합하려는 경우를 위한 기록이다.

`cubrid volmap`을 정식 유틸리티로 등록하는 변경 목록. `src/`의 3개 파일을 트리에 복사한 뒤
아래 등록 변경을 적용한다.

## 1. 파일 배치

| 저장소 파일 | 트리 위치 |
|---|---|
| `src/volmap.c` | `src/executables/volmap.c` |
| `src/volmap_standalone.cpp` | `src/executables/volmap_standalone.cpp` (트리 빌드에는 미포함 — 단독 빌드 전용) |
| `src/storage_ondisk_layout.hpp` | `src/storage/storage_ondisk_layout.hpp` |

## 2. 온디스크 구조체 이동 (storage_ondisk_layout.hpp)

온디스크 레이아웃 타입 9종은 원래 `.c` 내부 또는 서버 전용 헤더(`file_manager.h`·
`slotted_page.h`는 log_manager.h 등을 연쇄 include)에 있어 CS 모듈·단독 빌드에서 사용할
수 없다. 이를 의존이 `storage_common.h`+`log_lsa.hpp`뿐인 단일 헤더로 모으고, 기존 소스가
이 헤더를 include하게 한다. **정의 위치만 옮긴 것으로 동작 변경은 없다.**

| 기존 파일 | 변경 |
|---|---|
| `src/storage/disk_manager.c` | `DISK_VOLUME_HEADER` 정의를 layout 헤더로 이동 |
| `src/storage/file_manager.c` | `FILE_HEADER`, `FILE_EXTENSIBLE_DATA` 이동 |
| `src/storage/file_manager.h` | `FILE_TYPE`, descriptors, `FILE_TABLESPACE`, `FILE_PARTIAL_SECTOR` 이동 |
| `src/storage/slotted_page.h` | `SPAGE_HEADER`, `SPAGE_SLOT` 이동 |

## 3. 유틸리티 등록

| 파일 | 변경 |
|---|---|
| `src/executables/utility.h` | `VOLMAP` enum 값·옵션 상수(`VOLMAP_*_S/L`)·extern 선언 추가 |
| `src/executables/util_admin.c` | `ua_Volmap_Option` 테이블 + `ua_Utility_Map` 등록 |
| `src/executables/util_front.c` | 런처 등록 |
| `src/executables/util_service.c` | `cubrid volmap` 서브커맨드 등록 |
| `cs/CMakeLists.txt`, `sa/CMakeLists.txt` | `volmap.c`를 양 라이브러리 소스 목록에 추가 (util_cs.c 옆) |

**주의 — enum 값 = 배열 인덱스 계약**: `ua_Utility_Map`은 enum 값을 배열 첨자로 쓴다
(`util_get_utility_index`). 중간 삽입은 기존 유틸리티 전부의 인덱스를 어긋나게 해
`cub_admin` segfault를 유발한다. **반드시 배열 끝에 추가**하고 enum도 대응 위치에 둔다.

## 4. 빌드

```bash
# 트리 내 (릴리스/디버그)
./build.sh -m release
./build.sh -m debug

# 단독 정적 바이너리 (glibc-static 필요 — RHEL9: dnf --enablerepo=crb install glibc-static)
SRC=/path/to/cubrid-src BUILD=/path/to/cubrid-src/build_release sh tools/build_standalone.sh
```

단독 빌드는 `-DVOLMAP_STANDALONE` 가드로 Pass 2(서버 오버레이)를 컴파일에서 제외하고,
CUBRID 라이브러리에 전혀 링크하지 않는다 (프레임워크 심볼은 `volmap_standalone.cpp`가 제공).
