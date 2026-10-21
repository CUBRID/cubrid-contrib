# 빌드 (Build)

## 1. 세 가지 경로

| 경로 | 필요한 것 | 쓰는 때 |
|---|---|---|
| **헤더 fetch** | `curl`, `g++` | 소스 체크아웃 없이 빌드한다. 버전 지정 가능 |
| **소스 체크아웃** | CUBRID 소스 + CMake 한 번 실행 | 이미 개발 트리가 있을 때 |

## 2. 왜 헤더가 필요한가

volmap 은 페이지·볼륨 구조체를 엔진 헤더(`file_io.h`·`storage_common.h` 등)에서 `#include` 한다 —
오프셋을 하드코딩하지 않고 컴파일러가 계산한다.

단 **파일 디스크립터 구조체(`FILE_HEAP_DES` 등)와 `FILE_TYPE` 은 사본**이다
(`src/storage_ondisk_layout.hpp`). 엔진의 `disk_manager.c`·`file_manager.h`·`slotted_page.h`
는 받지 않는다 — 이 파일들은 헤더가 아니거나 내부 의존이 커서 단독 include 가 어렵기 때문이다.

> 사본인 부분은 엔진에서 구조체가 바뀌어도 **컴파일 단계에서 드러나지 않는다.**
> 포맷 변경은 [ondisk-format.md](ondisk-format.md) 의 버전표와 대조해 확인해야 한다.

```c
#include "file_io.h"    /* FILEIO_PAGE_RESERVED - prv 헤더 레이아웃 */
#include "oid.h"
#include "storage_common.h"
#include "storage_ondisk_layout.hpp"
```

오프셋을 컴파일러가 계산하므로 포맷이 바뀌면 **컴파일 단계에서 드러난다**.
손으로 `offset 24` 같은 값을 적어 두는 방식이었다면 소스 없이 빌드되겠지만,
엔진이 바뀐 뒤에도 조용히 틀린 지도를 그리게 된다.

이것이 "오프셋 하드코딩 0" 원칙의 대가이고, 아래 fetch 방식이 그 대가를 실무적으로 줄인다.

## 3. 헤더 fetch 빌드

```sh
sh tools/build_fetch.sh                    # 기본 ref = develop
REF=v11.4.6.1963 sh tools/build_fetch.sh   # 특정 태그
KEEP=1 sh tools/build_fetch.sh             # 받은 헤더를 남긴다(.fetch/)
```

동작:

1. volmap 이 포함하는 헤더 **23개**를 `raw.githubusercontent.com` 에서 받는다
   (버전에 따라 선택 헤더 1개 추가).
2. CMake 생성물인 `config.h`·`version.h` 를 스크립트가 직접 만든다.
   전자는 feature-test 매크로 모음이고, 후자는 volmap 이 쓰지 않는 버전 문자열이다.
3. 구버전 호환 처리(§5)를 적용한 뒤 컴파일한다.

clone 도, cmake 도, 빌드 트리도 필요 없다.

### 받는 헤더

```
include/system.h
src/base/        cubrid_getopt.h  databases_file.h  dynamic_array.h
                 environment_variable.h  error_code.h  memory_alloc.h
                 memory_hash.h  message_catalog.h
                 object_representation_constants.h  porting.h
                 porting_inline.hpp  release_string.h  sha1.h  util_func.h
src/compat/      cache_time.h  dbtype_def.h
src/executables/ utility.h
src/storage/     file_io.h  oid.h  storage_common.h
src/thread/      thread_compat.hpp
src/transaction/ log_lsa.hpp

선택: src/base/memory_cwrapper.h   (11.4+ 에만 존재)
```

목록은 `g++ -MM` 으로 실제 의존을 추출해 확정했다. 늘리거나 줄이면 빌드가 즉시 깨지므로
추측이 섞일 여지가 없다.

## 4. 검증된 버전

`tools/check-releases.sh` 가 GitHub Releases API 에서 각 major.minor 의 최신 태그를 읽어
전부 빌드하고, 온디스크 구조체가 바뀌었는지 함께 본다.

```sh
sh tools/check-releases.sh              # 구조체 + 빌드
sh tools/check-releases.sh --structs    # 구조체만 (컴파일러 불요, 수십 초)
sh tools/check-releases.sh --builds     # 빌드만
```

| 종료 코드 | 뜻 |
|---|---|
| 0 | 이상 없음 |
| 1 | 빌드 실패 |
| 2 | **구조체 변화** — 사본·문서 갱신 필요 |
| 3 | 둘 다 |

**구조체 검사가 따로 있는 이유**: volmap 이 include 할 수 없는 파일
(`disk_manager.c`·`file_manager.c/h`·`slotted_page.h`)의 구조체는
`src/storage_ondisk_layout.hpp` 에 사본으로 둔다. 사본은 엔진이 바뀌어도 **컴파일에서
드러나지 않으므로**, 빌드가 통과해도 오프셋이 어긋난 채 돌 수 있다. 이 검사는 릴리스
사이의 변화와 **사본 자체의 노후**를 모두 본다.

검출 예 — 11.4 가 `DISK_VOLUME_HEADER` 에 필드를 끼워 넣은 변화:

```
CHANGED disk_volume_header: v11.3.5.1280 -> v11.4.6.1963
    +INT64 vol_creation;
```

아래 표는 그 검사를 통과한 시점의 기록이다.

| 태그 | 빌드 | 실행 | 비고 |
|---|---|---|---|
| `v10.2.18` | OK | OK | lzo typedef + pflag 매핑 필요 |
| `v11.0.16` | OK | OK | lz4 스텁 필요 |
| `v11.1.0.0441` | OK | OK | 〃 |
| `v11.2.9.0868` | OK | OK | 〃 |
| `v11.3.5.1280` | OK | OK | 〃 |
| `v11.4.6.1963` | OK | OK | 〃 |
| `develop` | OK | OK | 스텁 불요(헤더에서 압축 include 제거됨) |

10.1 태그로는 빌드하지 않는다 — volmap 이 포함하는 헤더 23개 중 7개(`cubrid_getopt.h`,
`dbtype_def.h` 등)가 10.1 트리에 없다. `check-releases.sh` 도 10.2 미만은 빌드를 건너뛴다.
10.1 볼륨은 위 버전으로 빌드한 바이너리가 읽는다.

산출 바이너리의 md5 는 세 묶음으로 나뉜다 — **헤더가 실제로 바뀐 경계**와 일치한다.

| md5 묶음 | 버전 |
|---|---|
| A | 10.2, 11.0, 11.1 |
| B | 11.2, 11.3 |
| C | 11.4, develop |

## 5. 구버전 호환 — 자동 처리되는 세 가지

### 5.1 `memory_cwrapper.h` 부재 (≤11.3)

11.4 에서 도입된 헤더다. 선택 목록에 두고 404 면 건너뛴다.

### 5.2 압축 헤더 include (10.2~11.4)

`file_io.h` 가 `lz4.h`(11.0~11.4) 또는 `lzo/lzoconf.h`(10.2) 를 include 한다.
volmap 은 **압축 심볼을 하나도 쓰지 않으므로**(열거형 값만 헤더에 함께 선언돼 있다)
빈 스텁으로 충족한다.

10.2 만은 구조체 필드가 LZO 타입이라 typedef 가 필요하다.

```c
typedef unsigned long  lzo_uint;
typedef unsigned char  lzo_byte;
typedef unsigned char *lzo_bytep;
```

### 5.3 TDE 플래그 필드명 (10.2 vs 11.0+)

```c
/* 10.2  */ unsigned char pflag_reserve_1;   /* unused - Reserved field */
/* 11.0+ */ unsigned char pflag;             /* TDE 도입하며 이름 부여 */
```

**레이아웃은 동일**하다 — `prv` 는 양쪽 다 32바이트다. 이름만 다르므로 스크립트가
헤더를 확인해 `-Dpflag=pflag_reserve_1` 을 붙인다. volmap 은 이 바이트를 TDE 페이지
감지에만 쓴다.

## 6. 소스 체크아웃 빌드

```sh
SRC=/path/to/cubrid-src bash tools/build_standalone.sh
BUILD=/path/to/build SRC=/path/to/cubrid-src bash tools/build_standalone.sh
```

`BUILD` 기본값은 `$SRC/build_release`. **CMake 를 한 번은 돌려야 한다** — `config.h` 가
소스 트리에 없고 빌드 디렉터리에만 생성되기 때문이다. 돌리지 않으면 이렇게 끝난다.

```
fatal error: config.h: No such file or directory
```

## 7. 산출물

| 바이너리 | 크기 | 링크 | 용도 |
|---|---:|---|---|
| `cub_volmap` | 1.3MB | 완전 정적(`glibc-static` 이 있을 때) | 파일 하나로 복사해 쓴다 |

## 구 환경에서 실행하기

빌드 호스트에 `glibc-static` 이 있으면 완전 정적 바이너리가 나오고 **glibc 요구가 없다**.
없으면 두 스크립트 모두 **같은 경로에 glibc 동적 바이너리**를 만든다. 어느 쪽인지는 빌드
끝에 출력되며, 직접 확인하려면 `ldd cub_volmap` 을 쓴다.

| 산출물 | 확인 | 대상 요구 |
|---|---|---|
| 완전 정적 | `ldd` → `not a dynamic executable` | glibc 무관. 커널 하한만 본다 |
| **동적 폴백** | `ldd` 가 라이브러리를 나열 | **빌드 호스트의 glibc 이상** — 구 배포판에 복사하면 실행되지 않는다 |

동적 폴백본을 구 환경에 쓰려면 그 장비에서 다시 빌드하거나, 빌드 호스트에
`glibc-static` 을 설치하고 다시 빌드한다.

정적 바이너리의 제약은 glibc 가 아니라 **커널**이다. 정적 링크된 glibc 가 자기 커널 하한을
바이너리에 박아 넣는다 — `readelf -n cub_volmap` 의 `NT_GNU_ABI_TAG` 로 확인한다.
glibc 2.34 호스트에서 빌드하면 **커널 3.2 이상**이 된다.

| 대상 | glibc | 커널 | 이 호스트 빌드본 |
|---|---|---|---|
| RHEL 7 이상 | 2.17+ | 3.10+ | 동작 |
| RHEL 6 / Ubuntu 10.04 | 2.12 / 2.11 | **2.6.32** | **커널 하한에 걸림** |

그보다 낮은 환경이 대상이면 **그 장비에서 직접 빌드**한다. C++17 컴파일러가 필요하므로
RHEL 6 에서는 devtoolset(GCC 7 이상)을 쓴다.
