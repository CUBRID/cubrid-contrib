# 기반 모듈 명세 (Base Modules Specification)

## 0. 범위와 표기

### 0.1 대상

| 파일 | 모듈 이름 | 책임 한 줄 |
|---|---|---|
| `src/ctv.h` | 공용 헤더 | 모든 모듈이 포함하는 유일한 헤더: 시스템 include, 상수, 공용 타입, 전역 변수 선언, 모듈별 프로토타입 |
| `src/ctv_text.c` | text | UTF-8 길이·검증, 터미널 열(column) 폭, 열 단위 자르기·패딩, 외부 문자열 정화, FNV-1a 해시, 경계 안전 문자열 덧붙이기 |
| `src/ctv_mem.c` | target memory | 대상 프로세스 메모리의 **유일한** 판독자: `/proc/<pid>/mem` pread, `process_vm_readv` 일괄 판독, C 문자열·이름 판독, 리틀엔디언 정수 디코더 `rd_*` |
| `src/ctv_field.c` | field | 탐침(probe)으로 찾은 필드 오프셋 하나의 상태 기계(미확정/잠정/확정), 런타임 앵커 집계, 거부 오프셋 이력 |
| `src/ctv_sched.c` | sched | 비용 큰 재시도의 타이밍(빠른 배증 라운드 → 느린 고정 간격), 최소 간격 게이트 `every` |
| `src/ctv_msg.c` | messages | 한/영 메시지 출력(`die`/`warn`/`warnf`), 저하(degraded) 비트의 유일한 기록자, `deg_names` 표 |
| `src/ctv_os.c` | OS view | procfs 텍스트만 사용: 태스크 목록, 스레드 이름·상태·CPU 틱, 클럭 틱, 활성 로그 볼륨의 블록 장치와 diskstats |
| `src/ctv_proc.c` | servers | 실행 중 `cub_server` 프로세스 탐색(`/proc/<pid>/comm`, `cmdline`) |
| `src/ctv_elf.c` | ELF | 대상에 매핑된 `libcubrid.so`의 위치(`/proc/<pid>/maps`)와 디스크 ELF 파일의 심볼 조회 |
| `build.sh` | 빌드 | 정적(우선)/동적 링크 빌드, 소스 목록 출력 |

### 0.2 표기 규칙

- "외부 문자열": 대상 프로세스 메모리, procfs, 녹화 파일, 명령행 인자 등 cub_ctv 가 통제하지 않는 출처의 텍스트. 터미널에 도달하기 전에 반드시 `text_sanitize` 를 거친다.
- 정수 크기는 x86-64 Linux(LP64) 기준: `int` 32비트, `long`/`size_t`/`uint64_t` 64비트, 리틀엔디언.
- "정적(static) 상태"의 수명은 별도 언급이 없으면 프로세스 전체이며, 해제 경로가 없다(종료 시 OS 회수).

---

## 1. 공용 헤더 `ctv.h`

### 1.1 구조

| 순서 | 내용 |
|---|---|
| 1 | Apache 2.0 라이선스 헤더, 파일 설명 주석(불변식 id `I-0`..`I-20` 은 `PR.md` 의 원장 참조) |
| 2 | include 가드 `_CTV_H_` |
| 3 | `#define _GNU_SOURCE` (모든 시스템 헤더보다 먼저) |
| 4 | 시스템 헤더 include |
| 5 | 상수 매크로(모듈 공용) |
| 6 | 저하 비트 `DEG_*`, 매크로 `M()`, `REFUSE()` |
| 7 | 공용 타입(`SERVER`, ELF/LIB 반환 코드 enum, `VALID_PTR`, CS/트랜잭션/스레드/윈도/프레임/녹화 타입) |
| 8 | 전역 변수 `extern` 선언, `DEG_NAME`·`deg_names` |
| 9 | 키 코드, 색 역할 enum, `FIELD`, `SCHED`, `VERDICT` 타입 |
| 10 | 모듈별 프로토타입(모듈 주석 한 줄 + 함수들) |

### 1.2 include 목록 (순서 그대로)

`<ctype.h>`, `<dirent.h>`, `<elf.h>`, `<errno.h>`, `<fcntl.h>`, `<signal.h>`, `<stdarg.h>`, `<stddef.h>`, `<stdint.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<sys/ioctl.h>`, `<sys/prctl.h>`, `<sys/select.h>`, `<termios.h>`, `<sys/mman.h>`, `<sys/uio.h>`, `<limits.h>`, `<math.h>`, `<sys/stat.h>`, `<time.h>`, `<unistd.h>`.

추가 직접 include: `ctv_os.c` → `<sys/sysmacros.h>` (`major()`/`minor()`), `ctv_dump.c` → `<sys/utsname.h>` (`uname`).

헤더가 의존하는 시스템 상수 값(glibc/x86-64 실측): `IOV_MAX` = 1024, `PATH_MAX` = 4096, `sizeof (Elf64_Ehdr)` = 64, `sizeof (Elf64_Phdr)` = 56, `sizeof (Elf64_Shdr)` = 64, `sizeof (Elf64_Sym)` = 24, `SHT_SYMTAB` = 2, `SHT_DYNSYM` = 11, `SHN_UNDEF` = 0, `PT_LOAD` = 1.

### 1.3 공용 매크로

| 매크로 | 정의 | 의미 |
|---|---|---|
| `M(en, ko)` | `(use_ko ? (ko) : (en))` | 사용자 표시 문자열의 언어 선택. 두 인자 모두 평가 가능한 식이어야 하며 선택된 쪽만 사용된다 |
| `REFUSE(id)` | `return (id)` | 탐침 함수가 불변식 위반 시 거부 사유 문자열(예: `"I-4"`)을 반환하는 관용구 |
| `VALID_PTR(p)` | `((p) > 0x1000 && (p) < 0x800000000000ULL)` | x86-64 사용자 공간 포인터 타당성: 첫 페이지(0..0x1000) 제외, 47비트 사용자 공간 상한 0x0000_8000_0000_0000 미만. 경계값 0x1000 자체는 무효 |
| `THR_INDEX(slot)` | `((slot) + 1)` | 스레드 엔트리 배열 슬롯 → 엔진의 `entry.index` (0번 인덱스는 배열 밖) |
| `CHUNK_BYTES(n)` | `((size_t)(n) * STATS_SIZE + CHUNK_TAIL)` | sync_Stats 청크 바이트 수 |
| `CHUNK_OFF_NEXT(n)` | `((size_t)(n) * STATS_SIZE)` | 청크의 next 포인터 오프셋 |
| `CHUNK_OFF_INUSE(n)` | `((size_t)(n) * STATS_SIZE + 12)` | 청크의 사용 수 오프셋 |
| `CLIENTIDS_BYTES(s)` | `((s) * CLIENTIDS_NSTR + 4)` | CLIENTIDS 문자열 6개 + `int` 1개 바이트 수 |
| `ST_TYPE_OK(t)` | `((t) >= ST_TYPE_CSECT && (t) <= ST_TYPE_MUTEX)` | SYNC_STATS 타입 1..4 |
| `TRANS_TABLE_ONLY(trans, record)` | `((trans) && (record) == NULL)` | `--trans` 이면서 녹화 없음 → 트랜잭션 표만(샘플링·스레드 엔트리 없음) |
| `DEG_THR_SUB` | `DEG_THR_TRAN \| DEG_THR_ID \| DEG_THR_TID \| DEG_THR_RESUME \| DEG_THR_NEXT \| DEG_THR_STATUS` (= 0x3D8) | 스레드 엔트리 하위 필드 저하 비트 묶음 |
| `FNV_SEED` | `14695981039346656037ULL` | FNV-1a 64비트 오프셋 기저 |

### 1.4 상수 전체 표

의미의 상세는 해당 소유 모듈의 명세가 정본이며, 여기서는 값과 한 줄 의미를 고정한다.

**목록·버퍼 크기**

| 상수 | 값 | 의미 |
|---|---|---|
| `SERVERS_MAX` | 1024 | 나열할 cub_server 후보 최대 수 |
| `MAX_CS` | 64 | CS 배열 탐지 시 상한 |
| `CS_MIN_SAMPLES` | 4 | 시퀀스 탐침이 합의해야 하는 디스크립터 수 |
| `NAME_MAX_LEN` | 64 | CS/동기 프리미티브 이름 버퍼 |
| `IDENT_LEN` | 64 | CLIENTIDS 문자열 1개 버퍼 |
| `TAG_LEN` | 1024 | 소유자/대기 체인 텍스트 버퍼 |
| `TAG_TAIL_RESERVE` | 48 | 끝의 `"[n of W waiting]"` 를 위해 남겨 두는 바이트 |
| `PID_MAX_LIMIT` | 4194304 | 커널 pid_max 상한(2^22) |
| `TRAN_SLOTS_MAX` | 65536 | 트랜잭션 디스크립터 수 상한 |
| `POOL_MAX_CHUNKS` | 1024 | sync_Stats 체인 홉 상한 |
| `POOL_MAX_ENTRIES` | 65536 | sync_Stats 엔트리 방문 상한 |
| `POOL_MAX_GROUPS` | 1024 | (type, name) 그룹 표시 상한 |
| `LOG_GL_SCAN_MAX` | 65536 | `all_tdes` 를 찾기 위해 스캔하는 `log_Gl` 바이트 |
| `TDES_HEAD_SCAN` | 512 | `tran_index` 를 찾기 위해 스캔하는 TDES 앞부분 바이트 |
| `TDES_SCAN_MAX` | 32768 | TDES 간격 타당 상한 |
| `TDES_SCAN_FALLBACK` | 8192 | 간격을 못 쓸 때의 스캔 상한 |
| `NWITNESS` | 3 | 기하(geometry) 교차 검증 디스크립터 수 |
| `LAZY_LOCK_BUDGET` | 16 | 지연 재시도당 디스크립터 수(trid 없음) |
| `LAZY_LOCK_TRIES` | 4 | 지연 재시도당 새 트랜잭션 시도 수 |
| `LAZY_RETRY_S` | 10 | 실패 디스크립터 재시도 간격(초) |
| `LIVE_TRANS_ROWS` | 24 | 요약 화면 세션 행 수(짧은 터미널이면 더 적게) |

**SYNC_STATS 레이아웃**

| 상수 | 값 | 의미 |
|---|---|---|
| `STATS_SIZE` | 64 | SYNC_STATS 1개 크기 |
| `ST_OFF_TYPE` | 0 | `int32 type` |
| `ST_OFF_NAME` | 8 | `const char *name` |
| `ST_OFF_NENTER` | 16 | `uint32 nenter` |
| `ST_OFF_NWAIT` | 20 | `uint32 nwait` |
| `ST_OFF_NREENTER` | 24 | `uint32 nreenter` |
| `ST_OFF_TOTAL_TV` | 32 | 총 경과 timeval |
| `ST_OFF_MAX_TV` | 48 | 최대 경과 timeval |
| `ST_TYPE_CSECT` | 1 | 타입: 크리티컬 섹션 |
| `ST_TYPE_MUTEX` | 4 | 타입: 뮤텍스(허용 범위 상한) |
| `CHUNK_TAIL` | 16 | 청크 꼬리(next 8 + 기타) |

**CS 레이아웃**

| 상수 | 값 | 의미 |
|---|---|---|
| `CS_OFF_NAME` | 0 | CS 의 `const char *name` |
| `CS_OFF_INDEX` | 8 | `int cs_index` (I-1 앵커) |
| `CS_PAIR_RETRIES` | 3 | I-4 **총 시도 수**(`tries = 0..2`: 첫 시도는 I-3 까지 쓴 배열 이미지 그대로, 2·3번째 시도 앞에서만 재판독 → 재판독은 최대 2회). 02 §3.4 6단계가 정본 |
| `CS_STATS_TO_RWLOCK` | 96 | stats 포인터 → rwlock 3종까지 거리(cond48 + queues16 + owner8 + tran/pad8 + trio/pad16) |
| `CS_WAIT_SANE_MAX` | 1000000 | 대기 카운터 타당 상한 |
| `CS_STATS_TO_OWNER` | 16 | stats − 16 = writer thread id |
| `CS_STATS_TO_WQ` | 32 | stats − 32 = `waiting_writers_queue` |
| `CS_STATS_TO_PQ` | 24 | stats − 24 = `waiting_promoters_queue` |

**C++ std::string ABI**

| 상수 | 값 | 의미 |
|---|---|---|
| `COW_STRIDE` | 8 | 구 ABI: 힙을 가리키는 `char*` 1개 |
| `SSO_STRIDE` | 32 | 신 ABI: `{ptr, size, buf16\|cap}` |
| `COW_REP_LEN` | 24 | 데이터 앞의 `_Rep{len, cap, refcount}` |
| `SSO_LOCAL_OFF` | 16 | 객체 내부 로컬 버퍼 오프셋 |
| `SSO_LOCAL_MAX` | 15 | 로컬 버퍼에 들어가는 최장 길이 |
| `STR_LEN_MAX` | 255 | 문자열 길이 타당 상한 |
| `STR_CAP_MAX` | 4096 | 용량 타당 상한 |
| `STR_REF_MAX` | 65536 | refcount 타당 상한 |
| `CLIENTIDS_NSTR` | 6 | client_info .. client_ip_addr |

**스레드 엔트리**

| 상수 | 값 | 의미 |
|---|---|---|
| `THR_STRIDE_MIN` / `THR_STRIDE_MAX` | 256 / 65536 | `sizeof (entry)` 탐색 범위 |
| `THR_INDEX_SCAN` | 2048 | `index` 탐색 바이트 |
| `THR_MAX` | 65536 | 엔트리 수 상한 |
| `THR_TRAN_REL_MAX` | 28 | `tran_index` = index+28(신) 또는 +20(구) |
| `THR_NEXT_SPAN` | 64 | resume_status 뒤 next_wait_thrd 탐색 폭(I-19) |
| `THR_NEXT_CANDS` | 8 | next 후보 수 |
| `THR_ID_SLACK` | 8 | 엔트리 풀 밖 스레드 허용 수 |
| `THR_WAITERS_MAX` | 64 | 대기 체인 추적 홉 상한(순환 방지) |
| `THR_TID_SCAN` | 1024 | OS tid 탐색 바이트(struct pthread, I-17) |
| `THR_RESUME_MAX` | 28 | `thread_resume_suspend_status` 값 0..27 |
| `THR_RESUME_CS_MIN` / `THR_RESUME_CS_MAX` | 7 / 12 | CS 대기자의 resume 값: 7 = `CSECT_READER_SUSPENDED`, 큐 대기 중 7..11; 12 = `..CSECT_PROMOTER_RESUMED` (큐 표본 직후 이미 재개된 대기자 허용). 경계 포함 여부는 소유 모듈(ctv_thr_probe) 명세가 정본 |
| `THR_ID_CANDS` | 64 | m_id 후보 수 |
| `THR_TID_SAMPLE` | 32 | tid 표본 수 |
| `TS_WAIT` | 3 | 대기 중 `entry.status` |

**윈도·화면·녹화**

| 상수 | 값 | 의미 |
|---|---|---|
| `VERDICT_HISTORY` | 10 | 지속성 판정 윈도 이력 |
| `VERDICT_WAITERS_LISTED` | 4 | 판정문에 나열하는 대기자 수 |
| `CHAIN_REWALK_S` | 0.25 | 같은 헤드 큐의 재추적 간격(초) |
| `OS_SAMPLE_MIN_S` | 0.05 | procfs 상태 샘플링 최소 간격(스레드당 20 Hz) |
| `OS_MAX` | 64 | 프레임당 OS 상태 보관 스레드 수 |
| `OS_COMM_LEN` | 16 | 스레드 이름 버퍼(커널 `TASK_COMM_LEN` 과 동일) |
| `LAYOUT_AUTO` / `LAYOUT_WIDE` / `LAYOUT_NARROW` | 0 / 1 / 2 | 행 배치 모드 |
| `ACT_MAX` | 4096 | 윈도 활동 표 크기 |
| `ACT_HELD_RESERVE` | 256 | 보유자 관측용 예약분 |
| `ACT_HASH` | 16384 | 활동 해시(2의 거듭제곱 > 2×ACT_MAX) |
| `ACT_HELD` / `ACT_WAIT` | 0 / 1 | 활동 종류 |
| `VIEW_MAX_TIDS` | 128 (`2*MAX_CS`) | 프레임 tid 표 |
| `VIEW_MAX_CHAINS` | 128 | 프레임 체인 수 |
| `VIEW_MAX_SLOTS` | 8192 (`128*64`) | 프레임 체인 슬롯 총수 |
| `REC_MAGIC` | `"CTVREC 5"` | 녹화 파일 매직 |
| `INTERVAL_MIN` / `INTERVAL_MAX` | 0.1 / 3600.0 | `--interval` 범위(초) |
| `REC_SAMPLES_MAX` | 36010000 | 윈도 샘플 수 상한 |
| `REC_KEYFRAME` | 60 | 매 N 번째 프레임이 모든 identity 포함 |
| `REC_DT_MAX` | 1e7 | 녹화가 주장할 수 있는 최장 윈도(초) |
| `REC_TOKENS_MAX` | 68 (`4+THR_WAITERS_MAX`) | 녹화 한 줄 최대 토큰 |
| `POOL_GROUPS_CAPPED` / `POOL_NO_MEMORY` / `POOL_CHAIN_LOOP` | −2 / −3 / −4 | `pool_truncated` 사유 |
| `DETAIL_MAGIC` | `"cub_ctv detail dump"` | 상세 덤프 식별자 |
| `TRAN_TEXT_SHORT` / `TRAN_TEXT_LABEL` | 0 / 1 | `tran_text` 스타일 |
| `KEY_IGNORED` / `KEY_EOF` / `KEY_RESIZE` / `KEY_BACK` / `KEY_FWD` | 256 / 257 / 258 / 259 / 260 | `key_read` 반환 센티넬(바이트 범위 밖) |
| `VB_NONE` / `VB_DISK` / `VB_CPU` / `VB_SHORT` / `VB_UNKNOWN` | 0 / 1 / 2 / 3 / 4 | 판정 병목 분류 |

**field / sched (본 문서 소유)**

| 상수 | 값 | 의미 |
|---|---|---|
| `FIELD_NONE` | 0 | 미확정(다시 찾아야 함) |
| `FIELD_WEAK` | 1 | 잠정 규칙으로만 통과, 재탐침 대상 |
| `FIELD_SET` | 2 | 확정 |
| `FIELD_REJECTS` | 8 | 기억하는 거부 오프셋 수 |
| `SCHED_FIRST` | 2 | 첫 간격(초) |
| `SCHED_FAST_MAX` | 64 | 배증 상한(초) |
| `SCHED_FAST_ROUNDS` | 6 | 빠른 라운드 수(시작 2초 + 간격 4·8·16·32·64·64초 ≈ 3분, §5.1) |
| `SCHED_SLOW` | 300 | 이후 고정 간격(초, 5분) |

### 1.5 저하(degraded) 비트

| 비트 | 값 | 불변식 | 의미(꺼진 기능) |
|---|---|---|---|
| `DEG_TRID` | 0x1 | I-8 | trid 열과 해제 슬롯 필터 |
| `DEG_TRAN_CAP` | 0x2 | — | 디스크립터 표가 `TRAN_SLOTS_MAX`(65536) 칸까지 모두 확인됨 = 상한 도달(그 이상은 보지 않음; 03 §2) |
| `DEG_THREAD` | 0x4 | I-14 | 스레드 엔트리 표 사용 불가 |
| `DEG_THR_TRAN` | 0x8 | I-15 | `entry.tran_index` 미검증 |
| `DEG_THR_ID` | 0x10 | I-16 | `entry.m_id` 미검증 |
| `DEG_TRAN` | 0x20 | I-6..I-7 | 트랜잭션 표 사용 불가 |
| `DEG_THR_TID` | 0x40 | I-17 | OS tid 오프셋 미검증 |
| `DEG_THR_RESUME` | 0x80 | I-18 | `entry.resume_status` 미검증 |
| `DEG_THR_NEXT` | 0x100 | I-19 | `entry.next_wait_thrd` 미검증 |
| `DEG_THR_STATUS` | 0x200 | I-20 | `entry.m_status` 미검증 |

### 1.6 전역 변수 (선언은 `ctv.h`, 정의 위치는 표 기준)

| 이름 | 타입 | 정의 파일 | 초기값 | 의미 |
|---|---|---|---|---|
| `use_ko` | `int` | `ctv_msg.c` | 0 | 1 이면 `M()` 이 한국어 선택. 설정 주체는 아래 "use_ko 설정 규칙" |
| `degraded` | `unsigned` | `ctv_msg.c` | 0 | 현재 저하 비트 합. 기록은 `deg_set` 만 |
| `tran_refused` | `const char *` | `ctv_msg.c` | `NULL` | 트랜잭션 표를 끈(`DEG_TRAN`) 불변식 id 문자열. 기록 지점은 아래 "tran_refused 수명" |
| `deg_names[]` | `const DEG_NAME[]` | `ctv_msg.c` | 1.7 표 | 비트 → id/라벨, `bit == 0` 항목으로 종료 |
| `target_pid` | `int` | `ctv_os.c` | 0 | 대상 cub_server pid(`main` 이 `tran_subsystem_init` 직후, `thr_probe` 이전에 설정; 06 §2.10) |
| `color_on` | `int` | `ctv_term.c` | 0 | ANSI 색(tty 이고 `--plain` 아님) |
| `live_footer` | `const char *` | `ctv_present.c` | `NULL` | 화면 꼬리말, `NULL` 이면 `"[q] quit"` |
| `layout_mode` | `int` | `ctv_rows.c` | `LAYOUT_AUTO` | 행 배치 |
| `layout_detail` | `int` | `ctv_rows.c` | 0 | 상세 배치 |
| `detail_filter` | `const char *` | `ctv_rows.c` | `NULL` | `--detail NAME\|IX`, `NULL`=전체 |
| `tran_geom` | `TRAN_GEOM` | `ctv_tran.c` | 0 채움(`all_tdes == 0` = 사용 불가) | 트랜잭션 기하 |
| `thr_geom` | `THR_GEOM` | `ctv_thr.c` | 0 채움(`base == 0` = 사용 불가) | 스레드 엔트리 기하 |
| `thr_keep_image` | `int` | `ctv_thr.c` | 0 | `--dump` 시 탐침 이미지 유지 |
| `view` | `VIEW_FRAME` | `ctv_frame.c` | 0 채움 | 렌더러가 읽는 현재 프레임 |

**use_ko 설정 규칙** (정본: 06 §2.1, §12.5, §13.2)

| 시점 | 주체 | 동작 |
|---|---|---|
| `main` 첫 블록(인자 해석 전) | `cub_ctv.c` `main` | `lang = getenv ("LANG")`; `use_ko = lang != NULL && strncasecmp (lang, "ko", 2) == 0`. **`LANG` 만 본다**(`LC_ALL`·`LC_MESSAGES` 는 보지 않음). 앞 2글자 대소문자 무시 비교이므로 `ko_KR.UTF-8`, `KO`, `kor` 모두 한국어 |
| 같은 블록, 이어서 | `main` | `argv[1..argc-1]` 을 차례로 보며 정확히 `"--ko"` 면 1, `"--en"` 이면 0 (마지막 것이 이김, 값 위치 인자라도 문자열이 같으면 반영). 인자 오류 메시지가 이미 이 언어로 나오게 하려는 사전 스캔 |
| `parse_args` | `cub_ctv.c` | `--ko`/`--en` 을 만날 때 같은 값으로 다시 설정(결과 동일) |
| 실행 중 | 라이브 루프·대화형 재생 | `l` 키가 `use_ko = !use_ko` 로 토글 |

**tran_refused 수명**

| 단계 | 위치 | 동작 |
|---|---|---|
| 1 | `main` → `tran_subsystem_init` | `why = sym_loggl == 0 ? "log_Gl symbol" : tran_probe (bias + sym_loggl)`. 그 뒤 `tran_geom.all_tdes == 0` 이면 `deg_set (DEG_TRAN, 1)`, `tran_refused = why != NULL ? why : "transaction table probe"`. 이후 이 전역을 다시 쓰는 곳은 없다(라이브 실행 중 해제·변경 없음) |
| 2 | `main` 의 `VIEW_HDR h` 구성 | `snprintf (h.tran_refused, 64, "%s", tran_refused != NULL ? tran_refused : "")` — 63바이트 절단 복사. `view_capture`·`detail_dump_write` 보다 **먼저** 수행 |
| 3 | `detail_dump_write` | 전역이 아니라 복사본 `h->tran_refused` 를 쓴다. 거부 사유는 모두 64바이트 미만의 정적 리터럴이므로 1단계 전역과 같은 값이다(없으면 `""`) |
| 4 | `replay_main` | `tran_refused = h.tran_refused[0] != '\0' ? h.tran_refused : NULL` (헤더 버퍼를 가리킴). `--replay` 와 `--dump` 는 함께 쓸 수 없으므로(die) 재생 경로에서 덤프 값이 갈리는 경우는 없다 |
| 표시 | `print_layout_line` | 전역 `tran_refused` 를 쓴다(06 §2.12) |

### 1.7 `DEG_NAME` 와 `deg_names`

`DEG_NAME` 필드:

| 필드 | 타입 | 의미 |
|---|---|---|
| `bit` | `unsigned` | `DEG_*` 비트 하나 |
| `id` | `const char *` | 불변식 id 등 짧은 식별자 |
| `label` | `const char *` | 영어 라벨(번역 없음) |

`deg_names` 의 정확한 항목과 순서(레이아웃 줄과 상세 덤프가 이 순서로 이름을 낸다):

| # | bit | id | label |
|---|---|---|---|
| 0 | `DEG_TRID` | `"I-8"` | `"trid not validated"` |
| 1 | `DEG_TRAN_CAP` | `"cap"` | `"tran table capped"` |
| 2 | `DEG_TRAN` | `"I-6/I-7"` | `"no transaction table"` |
| 3 | `DEG_THREAD` | `"I-14"` | `"no thread entries"` |
| 4 | `DEG_THR_TRAN` | `"I-15"` | `"no tran"` |
| 5 | `DEG_THR_ID` | `"I-16"` | `"no owner id"` |
| 6 | `DEG_THR_TID` | `"I-17"` | `"no os tid"` |
| 7 | `DEG_THR_RESUME` | `"I-18"` | `"no wait reason"` |
| 8 | `DEG_THR_NEXT` | `"I-19"` | `"no wait chain"` |
| 9 | `DEG_THR_STATUS` | `"I-20"` | `"no thread state"` |
| 10 | 0 | `NULL` | `NULL` (종료자) |

### 1.8 본 문서 범위의 공용 타입

**`SERVER`** (ctv_proc)

| 필드 | 타입 | 의미 |
|---|---|---|
| `pid` | `int` | cub_server pid |
| `db` | `char[128]` | DB 이름(cmdline 첫 인자, 정화됨), 없으면 `"?"` |

**ELF 반환 코드** (anonymous enum)

| 이름 | 값 | 의미 |
|---|---|---|
| `ELF_OK` | 0 | 성공 |
| `ELF_NOFILE` | −1 | 디스크의 라이브러리 파일을 열 수 없음 |
| `ELF_BAD` | −2 | 정상 ELF64 이미지 아님(절단/손상) |
| `ELF_NOSYM` | −3 | 읽을 수 있으나 심볼 없음 |

**LIB 반환 코드** (anonymous enum)

| 이름 | 값 | 의미 |
|---|---|---|
| `LIB_OK` | 0 | 매핑 발견 |
| `LIB_NONE` | −1 | `libcubrid.so` 미매핑 |
| `LIB_DELETED` | −2 | `" (deleted)"` 로만 매핑됨(디스크에서 교체됨) |
| `LIB_NOMAPS` | −3 | `/proc/<pid>/maps` 판독 불가(`errno` 보존) |

**`FIELD`** (ctv_field)

| 필드 | 타입 | 의미 |
|---|---|---|
| `state` | `int` | `FIELD_NONE`/`FIELD_WEAK`/`FIELD_SET` |
| `off` | `int` | 잠긴 오프셋(`state != FIELD_NONE` 일 때 유효) |
| `hits` | `int` | 런타임 앵커 일치 관측 수(감쇠) |
| `misses` | `int` | 런타임 앵커 불일치 관측 수(감쇠) |
| `rejected` | `int[8]` | 앵커가 거부한 오프셋, 오래된 것부터 |
| `nrejected` | `int` | `rejected` 사용 수(0..8) |

초기 상태는 0 채움(= `FIELD_NONE`, 이력 없음)을 전제한다(정적 객체로 둔다).

**`SCHED`** (ctv_sched)

| 필드 | 타입 | 의미 |
|---|---|---|
| `at` | `time_t` | 다음 시도 예정 시각(초) |
| `last` | `time_t` | 마지막 시도 시각 |
| `gap` | `int` | 현재 간격(초), 0 = 재시작 직후 |
| `rounds` | `int` | `sched_ran` 호출 횟수(빠른 라운드 계수) |

**`OS_THR`** (ctv_os 결과를 담는 쪽은 sampler/frame; 필드 의미만 고정)

| 필드 | 타입 | 의미 |
|---|---|---|
| `slot` | `int` | 스레드 엔트리 슬롯 |
| `tid` | `int` | OS 스레드 id |
| `samples`, `run`, `dsk`, `oth` | `int` | 상태 샘플 수와 R / D / 기타 횟수 |
| `ticks0`, `ticks1` | `long` | 첫/마지막 관측 시 user+system 틱 |
| `t_first`, `t_last` | `double` | 첫/마지막 procfs 샘플 시각(단조 초) |
| `span_ms` | `int` | `t_last − t_first` (틱 차가 덮는 구간) |
| `comm` | `char[16]` | 스레드 이름 |

그 밖의 타입은 각 소유 모듈 명세에서 정의한다.

| 타입 | 정의 문서·절 |
|---|---|
| `STATS_VIEW`, `CS_LAYOUT`, `CS_ROW`, `POOL_GROUP` | 02 §1.2 |
| `TRAN_GEOM`, `TRAN_IDENT` | 03 (`TRAN_IDENT` 필드는 05 §3.5 에도 있음) |
| `THR_GEOM` | 04 |
| `VIEW_FRAME` | 05 §3.1 |
| `WIN_STATS` | 05 §3.2 |
| `ACT` | 05 §3.3 |
| `VIEW_IDENT`, `VIEW_CHAIN` | 05 §3.5 |
| `VIEW_HDR` | 05 §3.6 (필드: `int pid, live, hz, thr_count; double interval; char db[128], lib[4096], tran_refused[64]; unsigned degraded; CS_LAYOUT lo;` 이 순서) |
| `LIVE_WIN` | 05 §3.8 |
| `OPTS` | 06 §1.4 (옵션 의미는 §2.2) |
| `CS_RATES`, `CS_FIG`, `VERDICT` | 06 §1.4 (계산은 §6) |
| 색 역할 enum `CLR_*` | 06 §5.2 (enum 순서: `CLR_OFF, CLR_HEAD, CLR_WRITER, CLR_WAIT, CLR_OWNER, CLR_WQ, CLR_MUTED, CLR_BAR_LOW, CLR_BAR_MID, CLR_BAR_HIGH, CLR_PANEL_OWNER, CLR_PANEL_WAITER`) |

### 1.9 `rd_*` 헬퍼

헤더에는 프로토타입만 있고 정의는 `ctv_mem.c` 에 있다(3.11 참조).

---

## 2. text 모듈 (`ctv_text.c`)

### 2.1 책임과 상태

- 상태 없음(정적 변수 없음). 모든 함수가 재진입 가능.
- 입력은 NUL 종료 문자열을 전제한다. 다중 바이트 판정 중 NUL 을 만나면 NUL(0x00)은 연속 바이트(10xxxxxx)가 아니므로 시퀀스가 "깨짐"으로 판정되어 버퍼 끝을 넘어 읽지 않는다.
- **바이트 부호**: 이 모듈의 모든 바이트 비교·비트 연산은 `unsigned char` 값(0..255) 기준이다. 아래 절의 `s[k]` 는 모두 `(unsigned char) s[k]` 로 읽는다.

| 함수 | 부호 처리(코드 그대로) |
|---|---|
| `utf8_valid_len`, `printable_seq_len`, `utf8_cols`, `text_sanitize`, `is_printable_text` | 입력을 `const unsigned char *p = (const unsigned char *) s` 로 바꿔서 비교 |
| `utf8_cut` | `(unsigned char) s[p - 1]` 로 캐스트해 비교, 선두 바이트도 `unsigned char c` 에 담아 `utf8_seq_len` 에 전달 |
| `utf8_seq_len` | 인자 타입 자체가 `unsigned char` |
| `fnv` | `const unsigned char *b` |
| `seq_cols`, `str_cols`, `dw_pad` | 직접 비교 없음(`utf8_valid_len`·`utf8_cols` 경유), `*s != '\0'` 만 검사 |

대조: 02 §3.4 의 `ascii_name` (ctv_cs.c) 만 부호 있는 `char` 를 그대로 `< 0x21 \|\| > 0x7e` 로 비교한다(x86-64 gcc 에서 `char` 는 부호 있음 → 0x80 이상 바이트가 음수가 되어 거부됨). 이 모듈의 함수를 부호 있는 `char` 로 재구현하면 0x80 이상 바이트가 모두 `< 0x80` 분기로 빠져 정화·열 폭 결과가 달라진다.

### 2.2 `static int utf8_seq_len (unsigned char c)`

선두 바이트 `c` 로부터 기대 시퀀스 길이.

| 조건(위에서부터 첫 일치) | 반환 |
|---|---|
| `c < 0x80` | 1 |
| `(c & 0xE0) == 0xC0` 이고 `c >= 0xC2` | 2 |
| `(c & 0xF0) == 0xE0` | 3 |
| `(c & 0xF8) == 0xF0` 이고 `c <= 0xF4` | 4 |
| 그 외(연속 바이트 0x80..0xBF, 0xC0, 0xC1, 0xF5..0xFF) | 0 |

주의: 3바이트의 과잉 표현(0xE0 0x80..)과 서로게이트(0xED 0xA0..)는 거르지 않는다. 0xF0/0xF4 의 둘째 바이트 범위도 검사하지 않는다.

### 2.3 `int utf8_valid_len (const char *s)`

1. `n = utf8_seq_len (s[0])`.
2. `j = 1..n-1` 에 대해 `(s[j] & 0xC0) != 0x80` 이면 0 반환.
3. `n` 반환.

결과: ASCII(NUL 포함) 1, 완전한 다중 바이트 2..4, 고립 연속 바이트·잘못된 선두·잘린 시퀀스 0.

### 2.4 `static int printable_seq_len (const char *s)`

1. `s[0] < 0x80` 이면: `0x20 <= s[0]` 이고 `s[0] != 0x7F` 이면 1, 아니면 0 (C0 제어문자와 DEL, NUL 은 0).
2. 아니면 `n = utf8_valid_len (s)`.
3. `n < 2` 이거나 (`s[0] == 0xC2` 이고 `s[1] < 0xA0`, 즉 C1 제어 U+0080..U+009F) 이면 0, 아니면 `n`.

### 2.5 `int seq_cols (const char *s, int *len)`

- `n = utf8_valid_len (s)`.
- `*len = (n == 0) ? 1 : n` (항상 1 이상 → 호출자가 전진할 수 있음).
- 반환: `n == 0` 이면 1(깨진 바이트는 1바이트 1열), 아니면 `utf8_cols (s, n)`.

### 2.6 `int utf8_cols (const char *s, int len)`

1. `len < 3` 이면 1 (ASCII, 2바이트 문자는 모두 1열).
2. 코드 포인트 복원:
   - `len == 3`: `((s0 & 0x0F) << 12) | ((s1 & 0x3F) << 6) | (s2 & 0x3F)`
   - 그 외(4): `((s0 & 0x07) << 18) | ((s1 & 0x3F) << 12) | ((s2 & 0x3F) << 6) | (s3 & 0x3F)`
3. 아래 닫힌 구간 중 하나에 들면 2, 아니면 1.

| 구간 | 대략의 문자군 |
|---|---|
| 0x1100..0x115F | 한글 자모(초성) |
| 0x2E80..0x303E | CJK 부수, 기호·구두점 |
| 0x3041..0x33FF | 히라가나, 가타카나, CJK 호환 |
| 0x3400..0x4DBF | CJK 확장 A |
| 0x4E00..0x9FFF | CJK 통합 한자 |
| 0xA000..0xA4CF | 이 음절 |
| 0xAC00..0xD7A3 | 한글 음절 |
| 0xF900..0xFAFF | CJK 호환 한자 |
| 0xFE30..0xFE4F | CJK 호환 형태 |
| 0xFF00..0xFF60 | 전각 형태 |
| 0xFFE0..0xFFE6 | 전각 기호 |
| 0x1F300..0x1F64F | 그림 기호, 이모티콘 |
| 0x1F900..0x1F9FF | 보충 기호·그림 |
| 0x20000..0x3FFFD | CJK 확장 B 이후(SIP/TIP) |

### 2.7 `int str_cols (const char *s)`

NUL 까지 `seq_cols` 로 전진하며 열 수 합계를 반환. 빈 문자열 0.

### 2.8 `size_t utf8_cut (const char *s, size_t ln)`

`s[0..ln)` 의 끝에 걸쳐 잘린 UTF-8 시퀀스가 있으면 그 선두 앞에서 끊은 길이를 반환한다.

1. `p = ln`.
2. `p > 0` 이고 `s[p-1]` 이 연속 바이트(`& 0xC0 == 0x80`)이며 `ln - p < 3` 인 동안 `p--` (최대 3바이트 후퇴).
3. `p == 0` 이면 `ln` 반환(전부 연속 바이트: 판단 불가, 그대로).
4. `c = s[p-1]`, `need = utf8_seq_len (c)`.
5. `need > 1` 이고 `(p - 1) + need > ln` 이면 `p - 1` 반환(불완전 시퀀스 제거), 아니면 `ln`.

선두가 ASCII 또는 무효 선두(need ≤ 1)이면 자르지 않는다.

### 2.9 `uint64_t fnv (uint64_t h, const void *p, size_t n)`

FNV-1a 64비트. 각 바이트 `b` 에 대해 `h = (h ^ b) * 1099511628211` (mod 2^64). 시작값은 호출자가 `FNV_SEED` (14695981039346656037) 로 준다. 연속 호출로 이어 붙일 수 있다.

### 2.10 `void text_sanitize (char *s)`

외부 문자열을 제자리에서 정화한다. 길이는 변하지 않는다.

1. `p` 가 NUL 이 아닌 동안 반복.
2. `len = printable_seq_len (p)`. `len > 0` 이면 `p += len`, 계속.
3. 출력 불가인 경우:
   1. `j = utf8_valid_len (p)`.
   2. `j > 1` 이고 `p[1] != '\0'` 인 동안 `*p++ = '?'`, `j--`.
   3. 마지막으로 `*p++ = '?'`.

결과:

| 입력 | 결과 |
|---|---|
| C0 제어(0x01..0x1F), DEL(0x7F) | `?` 1개 |
| C1 제어(0xC2 0x80..0x9F, 2바이트 유효 시퀀스) | `??` |
| 깨진 UTF-8(무효 선두, 고립 연속, 잘린 시퀀스) | 해당 바이트마다 `?` 1개(뒤 바이트는 다음 반복에서 개별 판정) |
| 출력 가능 ASCII, 유효 UTF-8(C1 제외) | 그대로 |

### 2.11 `int is_printable_text (const char *s)`

NUL 까지 `printable_seq_len` 으로 전진. 한 번이라도 0 이면 0, 끝까지 통과하면 1. 빈 문자열은 1.

### 2.12 `size_t dw_pad (char *out, size_t n, const char *s, int width)`

`s` 를 `width` 터미널 열에 맞춰 잘라 복사하고 공백으로 채운다. 반환: `out` 에 쓴 바이트 수(NUL 제외). `n >= 1` 전제.

1. `o = 0`, `cols = 0`.
2. `*s != '\0'` 이고 `o + 4 < n` 인 동안:
   1. `w = seq_cols (s, &len)`.
   2. `cols + w > width` 이면 중단(넓은 문자가 마지막 1열에 걸리면 넣지 않음).
   3. `len` 바이트 복사, `o += len`, `s += len`, `cols += w`.
3. `cols < width` 이고 `o + 1 < n` 인 동안 공백 추가, `cols++`.
4. `out[o] = '\0'`, `o` 반환.

비고: 문자열 복사 단계는 4바이트 여유(최장 UTF-8 시퀀스)를 남기며 멈춘다. 깨진 바이트는 그대로 1바이트 1열로 복사된다(정화는 호출자 책임). 열 계산은 `utf8_cols` 표를 따른다.

### 2.13 `void buf_cat (char *buf, size_t n, size_t *pos, const char *fmt, ...)`

`printf` 형식 검사 속성(`format (printf, 4, 5)`)이 선언에 붙는다. `buf[*pos]` 에 덧붙인다.

1. `*pos + 1 >= n` 이면 아무것도 하지 않음(이미 가득).
2. `w = vsnprintf (buf + *pos, n - *pos, fmt, ap)`.
3. `w < 0` 이면 반환(`*pos` 불변).
4. `*pos + w < n` 이면 `*pos += w`, 반환.
5. 넘침: `*pos = utf8_cut (buf, n - 1)`, `buf[*pos] = '\0'`. 이후 호출은 1단계에서 무시되거나(위치가 n−1) 남은 몇 바이트만 다시 시도한다.

불변식: `buf` 는 항상 NUL 종료, 잘림은 UTF-8 시퀀스 중간에서 일어나지 않는다.

---

## 3. target memory 모듈 (`ctv_mem.c`)

### 3.1 책임과 상태

대상 서버 메모리를 읽는 유일한 경로. 쓰기는 하지 않는다.

| 정적 변수 | 타입 | 초기값 | 의미 |
|---|---|---|---|
| `mem_fd` | `int` | −1 | `/proc/<pid>/mem` 읽기 전용 fd. 닫지 않음(프로세스 수명) |
| `mem_pid` | `int` | 0 | `process_vm_readv` 대상 pid |
| `gather_ok` | `int` | 1 | `process_vm_readv` 사용 가능 여부. 한 번 0 이 되면 다시 1 이 되지 않음 |
| `mem_gather_field` 내 `addrs` | `uint64_t *` | `NULL` | 주소 배열 재사용 버퍼(커지기만 함) |
| `mem_gather_field` 내 `addrs_n` | `size_t` | 0 | `addrs` 용량 |
| `name_of` 내 `keys` | `uint64_t[1024]` | 0 | 이름 캐시 키(0 = 빈 칸) |
| `name_of` 내 `vals` | `char[1024][64]` | — | 이름 캐시 값 |
| `name_of` 내 `nkeys` | `int` | 0 | 캐시 항목 수(최대 512) |

| 상수 | 값 | 의미 |
|---|---|---|
| `NAME_CACHE_MAX` | 1024 | 이름 캐시 칸 수(2의 거듭제곱), 최대 절반까지만 채움 |

Linux 사실:
- `/proc/<pid>/mem` 의 open/pread 는 대상에 대한 ptrace 읽기 권한(같은 uid 이고 dumpable, Yama `ptrace_scope` 허용, 또는 `CAP_SYS_PTRACE`)이 필요하다. 파일 오프셋 = 가상 주소. 매핑되지 않은 주소에서 시작하면 −1(`EIO`), 매핑 끝에 걸치면 경계까지의 짧은 읽기가 가능하다.
- `process_vm_readv` 도 같은 ptrace 접근 검사를 받는다. 미지원 커널(`CONFIG_CROSS_MEMORY_ATTACH` 없음)은 `ENOSYS`, seccomp/권한 정책은 `EPERM`. 부분 전송은 **원격 iovec 요소 단위**로만 일어나며(한 요소를 쪼개지 않음), 첫 판독 불가 요소에서 멈추고 그때까지의 바이트 수를 반환한다. 첫 요소부터 실패하면 −1(`EFAULT` 등).

### 3.2 `int mem_open (int pid)`

1. `"/proc/%d/mem"` 경로(버퍼 64)를 `O_RDONLY` 로 연다 → `mem_fd`.
2. 성공 여부와 무관하게 `mem_pid = pid`.
3. 반환: fd 가 음수면 −1, 아니면 0. 실패 시 `errno` 는 `open` 의 것.

두 번 호출하면 이전 fd 는 닫히지 않는다(설계상 1회 호출).

### 3.3 `static ssize_t mem_pread (uint64_t addr, void *buf, size_t n)` / `int mem_read (uint64_t addr, void *buf, size_t n)`

- `mem_pread`: `pread (mem_fd, buf, n, (off_t) addr)` 그대로.
- `mem_read`: 정확히 `n` 바이트를 읽었으면 0, 짧은 읽기·오류면 −1. 실패 시 `buf` 내용은 부분적으로 덮였을 수 있다(초기화하지 않음).

### 3.4 `static void pread_one (uint64_t addr, char *dst, size_t each, unsigned char *ok)`

`each` 바이트를 pread. 정확히 읽히지 않으면 `dst` 를 0 으로 채우고 `ok` 는 건드리지 않음. 읽히면 `ok != NULL` 일 때 `*ok = 1`.

### 3.5 `int mem_gather_ok (const uint64_t *addrs, size_t n, size_t each, void *buf, unsigned char *ok)`

`addrs[i]` 에서 `each` 바이트씩 읽어 `buf[i*each]` 에 둔다. 항상 0 반환.

사후 조건:
- 주소 0 인 항목과 읽지 못한 항목은 0 으로 채워지고 `ok[i] = 0`.
- 읽힌 항목은 `ok[i] = 1` (`ok == NULL` 이면 기록 생략).

알고리즘:

1. `buf` 전체(`n*each`)를 0 으로, `ok != NULL` 이면 `ok[0..n)` 을 0 으로.
2. `i = 0`. `i < n` 인 동안:
   1. 스택에 `lv[IOV_MAX]`, `rv[IOV_MAX]`, `idx[IOV_MAX]` (IOV_MAX=1024; 약 40 KB).
   2. `scan = i` 부터 `scan < n` 이고 `cnt < IOV_MAX` 인 동안: `addrs[scan] == 0` 이면 건너뛰고, 아니면 `idx[cnt] = scan`, 로컬 iovec = `buf + scan*each` (`each`), 원격 iovec = `addrs[scan]` (`each`), `cnt++`.
   3. `cnt == 0` 이면 종료.
   4. `gather_ok` 이면 `got = process_vm_readv (mem_pid, lv, cnt, rv, cnt, 0)`:
      - `got < 0` 이고 `errno` 가 `ENOSYS` 또는 `EPERM`: `gather_ok = 0` 으로 영구 전환, 5단계로.
      - 그 외: `done = got > 0 ? got / each : 0`. `d < done` (그리고 `d < cnt`)인 `idx[d]` 에 `ok = 1`.
        - `done >= cnt`: `i = scan`, 다음 배치.
        - 아니면 실패 요소 `k = idx[done]` 하나를 `pread_one` 으로 단독 판독, `i = k + 1` 로 재개(배치의 나머지는 다음 반복에서 다시 모음).
   5. (`gather_ok == 0` 경로) 배치의 모든 요소를 각각 `pread_one`, `i = scan`.

비고: `got < 0` 의 다른 errno(예: `EFAULT` — 첫 요소가 판독 불가, `ESRCH` — 대상 종료)는 `done = 0` 으로 처리되어 첫 요소만 pread 후 재개한다. 따라서 대상이 사라지면 요소마다 시스템 호출이 반복된다(결과는 0 채움).

실패 처리 정리(02 §1.3 의 요약도 이 규칙을 따른다): `process_vm_readv` 가 배치 중간에서 멈추면 **멈춘 요소 하나만** `pread` 로 단독 판독하고, 그 뒤 요소들은 단독 재시도하지 않고 다음 반복에서 **다시 모아** `process_vm_readv` 로 읽는다(`i = k + 1`). "실패 범위 전체를 pread" 하는 것이 아니다. 따라서 한 배치 안의 요소들은 서로 다른 시스템 호출 시점에 판독될 수 있다.

### 3.6 `int mem_gather_field (const uint64_t *bases, size_t n, uint64_t off, size_t width, const unsigned char *skip, void *out, unsigned char *ok)`

객체 배열의 같은 필드 하나를 한 번의 gather 로 읽는다.

1. `addrs_n < n` 이면 기존 `addrs` 해제 후 `n` 개로 `malloc`. 실패 시 `addrs_n = 0`, `out` 의 `n * width` 바이트를 0 으로, `ok != NULL` 이면 `ok[0..n)` 을 0 으로 채운 뒤 −1 반환.
2. 각 `i`: `VALID_PTR (bases[i])` 이고 (`skip == NULL` 또는 `!skip[i]`) 이면 `addrs[i] = bases[i] + off`, 아니면 0.
3. `mem_gather_ok (addrs, n, width, out, ok)` 결과(0) 반환.

### 3.7 `size_t mem_read_shrink (uint64_t addr, void *buf, size_t max, size_t min)`

매핑 끝 근처 객체를 위한 축소 판독.

1. `n = max`.
2. `n >= min` 이고 `n > 0` 인 동안: `mem_read (addr, buf, n) == 0` 이면 `n` 반환; 아니면 `n /= 2` (정수 나눗셈).
3. 0 반환.

예: `max = 4096, min = 512` → 4096, 2048, 1024, 512 순으로 시도.

### 3.8 `int mem_read_ptr (uint64_t addr, uint64_t *p)`

8바이트 판독이 성공하고 `VALID_PTR (*p)` 이면 0, 아니면 −1 (`*p` 는 읽힌 값이 남을 수 있음).

### 3.9 `int mem_cstr (uint64_t addr, char *out, size_t outn)`

비어 있지 않고 NUL 로 끝나는 출력 가능 문자열 판독.

1. `addr == 0` 이면 −1.
2. `got = pread (addr, out, outn)` (짧은 읽기 허용: 문자열이 매핑 끝에 있을 수 있음).
3. `got < 2` 이거나 `out[0..got)` 안에 NUL 이 없으면 −1 (버퍼보다 긴 출력 가능 영역은 이름이 아니라 영역으로 간주).
4. `out[0] != '\0'` 이고 `is_printable_text (out)` 이면 0, 아니면 −1.

비고: 1바이트 읽기(`got == 1`)는 실패. 실패 시 `out` 은 부분 데이터가 남은 미정 상태.

### 3.10 `const char *name_of (uint64_t ptr, char *out, size_t n)`

정적 이름 포인터(CS·동기 프리미티브 이름)의 문자열. 항상 `out` 을 반환.

1. 해시: `h = (unsigned) ((ptr >> 3) * 2654435761u) & 1023` (64비트 곱의 하위 32비트를 취한 뒤 마스크).
2. 선형 탐사: `keys[h] != 0` 인 동안 `keys[h] == ptr` 이면 `snprintf (out, n, "%s", vals[h])` 로 복사하고 반환; 아니면 `h = (h + 1) & 1023`.
3. 빈 칸 도달: `ptr == 0` 이거나 `mem_cstr (ptr, out, n) != 0` 이면 `snprintf (out, n, "?")` 후 반환(캐시하지 않음 → 다음에 다시 시도).
4. 성공: `nkeys < 512` 이면 `keys[h] = ptr`, `snprintf (vals[h], 64, "%s", out)` (최대 63바이트), `nkeys++`. 512 이상이면 캐시하지 않고 반환.

복사 규칙: 모든 복사는 `snprintf (dst, size, "%s", src)` 이다(항상 NUL 종료, `size − 1` 바이트에서 **바이트 단위** 절단 — UTF-8 경계는 보지 않음, `strncpy` 아님). 현재 모든 호출자(`cs_rows_from_blob`, `pool_walk`)가 `n = 64` 로 부르므로 캐시 값(≤ 63바이트)이 잘리는 경우는 없다. `n < 64` 로 부르면 적중 시와 첫 판독 시 결과가 다를 수 있다(첫 판독은 `mem_cstr` 가 `n` 바이트 안에 NUL 이 없으면 실패 → `"?"`, 적중은 앞 `n − 1` 바이트).

절반까지만 채우므로 탐사는 항상 빈 칸에서 끝난다. 캐시 무효화는 없다(이름 포인터는 정적 데이터라는 전제).

### 3.11 `rd_u64`, `rd_i64`, `rd_u32`, `rd_i32`

`(const unsigned char *b, size_t off)` → `b + off` 에서 8/8/4/4 바이트를 `memcpy` 로 읽어 호스트(리틀엔디언) 정수로 반환. 정렬 요구 없음, 경계 검사 없음(호출자 책임).

| 함수 | 반환 타입 | 바이트 |
|---|---|---|
| `rd_u64` | `uint64_t` | 8 |
| `rd_i64` | `int64_t` | 8 |
| `rd_u32` | `uint32_t` | 4 |
| `rd_i32` | `int32_t` | 4 |

---

## 4. field 모듈 (`ctv_field.c`)

### 4.1 정의

"필드"는 탐침으로 찾은 구조체 내부 오프셋 하나(예: 스레드 엔트리의 `resume_status`, `m_status`)다. 상태 전이는 반드시 이 모듈 함수로만 한다. 모듈 자체의 정적 상태는 없고 상태는 호출자가 가진 `FIELD` 객체에 있다.

상태 기계:

| 현재 | 사건 | 다음 | 부수 효과 |
|---|---|---|---|
| 임의 | `field_lock (off, weak=0)` | `FIELD_SET` | `off` 변경 또는 이전 `NONE` 이면 `hits = misses = 0` |
| 임의 | `field_lock (off, weak=1)` | `FIELD_WEAK` | 위와 같음 |
| 임의 | `field_clear` | `FIELD_NONE` | 거부 이력 유지, `hits/misses` 유지 |
| `WEAK`/`SET` | `field_reject` | `FIELD_NONE` | 현재 `off` 를 거부 이력에 추가 |
| `NONE` | `field_reject` | `FIELD_NONE` | 이력 변화 없음 |

### 4.2 `void field_lock (FIELD *f, int off, int weak)`

1. `f->state == FIELD_NONE` 이거나 `f->off != off` 이면 `hits = misses = 0`.
2. `f->off = off`, `f->state = weak ? FIELD_WEAK : FIELD_SET`.

같은 오프셋으로 WEAK→SET 승격 시 집계는 유지된다.

### 4.3 `void field_clear (FIELD *f)`

`state = FIELD_NONE` 만 설정.

### 4.4 `void field_reject (FIELD *f)`

1. `state != FIELD_NONE` 이면:
   1. `nrejected == 8` 이면 `rejected[1..7]` 을 `[0..6]` 으로 당기고 `nrejected--` (가장 오래된 것 폐기).
   2. `rejected[nrejected++] = off`.
2. `state = FIELD_NONE`.

같은 오프셋이 중복 기록될 수 있다(중복 제거 없음).

### 4.5 `int field_skips (const FIELD *f, int off, int ignore)`

`ignore == 0` 이고 `off` 가 `rejected[0..nrejected)` 에 있으면 1, 아니면 0. 탐침 pass 함수가 후보 오프셋마다 호출한다.

### 4.6 `int field_sight (FIELD *f, int ok)`

런타임 앵커 관측 1회. 반환 1 = 필드를 버려야 함.

1. `ok` 이면 `hits++`, 아니면 `misses++`.
2. `hits + misses >= 64` 이면 둘 다 2로 나눔(정수, 오래된 관측 감쇠).
3. `misses >= 4` 이고 `misses > hits` 이면 1, 아니면 0.

이 함수는 상태를 바꾸지 않는다(버리기는 호출자가 `field_reject` 로).

### 4.7 `int field_probe (FIELD *f, int (*pass) (const FIELD *, const unsigned char *, int ignore, int *weak), const unsigned char *img)`

`pass` 계약: 이미지 `img` 에서 오프셋을 찾아 반환(없으면 −1), 잠정 규칙으로만 통과했으면 `*weak = 1`. `ignore == 0` 이면 `field_skips` 로 거부 오프셋을 건너뛴다.

1. `weak = 0`, `off = pass (f, img, 0, &weak)`.
2. `off < 0` 이고 `f->state == FIELD_NONE` 이고 `nrejected > 0` 이면 `off = pass (f, img, 1, &weak)` (거부 오프셋에 한 번 더 기회: 거부가 헤드가 막 서비스된 직후의 판독에서 왔을 수 있음). 이번에 `off >= 0` 이면 `nrejected = 0` (이력 전체 삭제).
3. `off >= 0` 이면 `field_lock (f, off, weak)`.
4. `off >= 0` 이면 1, 아니면 0 반환(실패 시 상태 불변).

---

## 5. sched 모듈 (`ctv_sched.c`)

### 5.1 정의

비용 큰 작업(재탐침 등)의 재시도 시각 계산. 시간 단위는 `time_t` 초(호출자가 `time (NULL)` 등으로 공급). 모듈 정적 상태 없음.

간격 수열:

| 사건 | rounds | gap(초) |
|---|---|---|
| `sched_start` | 0 | 2 |
| `sched_ran` 1회 | 1 | 4 |
| 2회 | 2 | 8 |
| 3회 | 3 | 16 |
| 4회 | 4 | 32 |
| 5회 | 5 | 64 |
| 6회 | 6 | 64 |
| 7회 이후 | 7.. | 300 |
| `sched_restart` 후 `sched_ran` 1회 | 1 | 2 (gap 0 → `SCHED_FIRST`) |
| 그 다음 | 2..5 | 4, 8, 16, 32 |
| 6회 | 6 | 64 |

### 5.2 함수

| 함수 | 동작 |
|---|---|
| `void sched_start (SCHED *s, time_t now)` | 첫 시도가 방금 이루어졌음: `rounds = 0`, `gap = 2`, `last = now`, `at = now + 2` |
| `int sched_due (const SCHED *s, time_t now)` | `now >= at` 이면 1 |
| `void sched_ran (SCHED *s, time_t now)` | 시도를 지금 수행: `++rounds > 6` 이면 `gap = 300`; 아니면 `gap == 0` 이면 2, `gap < 64` 이면 `gap*2`, 그 외 64. `at = now + gap`, `last = now` |
| `void sched_restart (SCHED *s)` | 빠른 라운드로 복귀하고 즉시 기한: `rounds = gap = 0`, `at = 0` (`last` 는 유지) |
| `int sched_slow (const SCHED *s)` | `rounds > 6` 이면 1(빠른 라운드 소진) |
| `void sched_sooner (SCHED *s, int min_gap)` | 변화 감지 시 앞당김: `at > last + min_gap` 이면 `at = last + min_gap` (늦추지는 않음, 마지막 시도로부터 `min_gap` 미만으로는 앞당기지 않음) |
| `int every (double *last, double now, double gap)` | 최소 간격 게이트: `now - *last < gap` 이면 0; 아니면 `*last = now`, 1. `*last` 초기값 0 이면 첫 호출은 `now >= gap` 일 때 통과 |

---

## 6. messages 모듈 (`ctv_msg.c`)

### 6.1 소유 상태

`use_ko`, `deg_names`, `tran_refused`, `degraded` (1.6, 1.7 참조).

### 6.2 `void die (const char *fmt_en, const char *fmt_ko, const char *arg)` — `noreturn`

1. `safe[4096]` 에 `arg` 복사(`NULL` 이면 빈 문자열, 4095자에서 절단).
2. `text_sanitize (safe)`.
3. `screen_restore ()` (대체 화면을 먼저 벗어남; 그렇지 않으면 atexit 의 화면 복원이 메시지를 지운다). 부수 출력(정본 06 §5.3): 원시 키 모드(`tty_raw`)였으면 저장된 termios 를 `tcsetattr (STDIN_FILENO, TCSANOW, …)` 로 되돌리고(출력 없음), 전체 화면(`screen_active`)이었으면 `write (STDOUT_FILENO, "\033[?25h\033[?1049l", 14)` 로 **stdout** 에 14바이트를 쓴다. 둘 다 아니면(스냅샷·파이프·`--json` 등 대체 화면이 아닌 모든 경로) **아무것도 쓰지 않는다**. 플래그를 먼저 0 으로 내리므로 두 번째 호출(atexit)도 무출력.
4. stderr 에 `"cub_ctv: "` + `fprintf (stderr, M (fmt_en, fmt_ko), safe)` + `"\n"`.
5. `exit (1)`.

형식 문자열은 `%s` 를 최대 1개 가진다(인자는 항상 `safe` 하나만 전달됨). 형식에 `%s` 가 없으면 `arg` 는 무시된다.

### 6.3 `void warn (const char *en, const char *ko)`

stderr 에 `"cub_ctv: %s\n"` 로 `M (en, ko)` 출력. 종료하지 않음.

### 6.4 `void warnf (const char *fmt_en, const char *fmt_ko, ...)`

stderr 에 `"cub_ctv: "` + `vfprintf (M (fmt_en, fmt_ko), 가변 인자)` + `"\n"`. 인자를 정화하지 않으므로 외부 문자열 인자는 호출자가 미리 `text_sanitize` 해서 넘긴다.

### 6.5 `void deg_set (unsigned bits, int on)`

`on` 이면 `degraded |= bits`, 아니면 `degraded &= ~bits`. `degraded` 의 유일한 기록자.

---

## 7. OS view 모듈 (`ctv_os.c`)

### 7.1 책임과 상태

procfs 텍스트만 읽는다(대상 메모리 판독 없음).

| 상태 | 타입 | 초기값 | 의미 |
|---|---|---|---|
| `target_pid` (전역) | `int` | 0 | 대상 pid |
| `os_task_comm` 내 `cache[256]` | `{int tid; time_t at; char comm[16];}` | 0 채움 | tid → 스레드 이름 직접 매핑 캐시 |

| 상수 | 값 | 의미 |
|---|---|---|
| `COMM_CACHE` | 256 | 이름 캐시 칸 수(`tid % 256` 직접 매핑, 충돌 시 덮어씀) |
| `COMM_TTL` | 5 | 이름 재사용 시간(초) |

### 7.2 `int cmp_int (const void *a, const void *b)`

`qsort`/`bsearch` 용 `int` 비교: `x < y` −1, `x > y` 1, 같으면 0 (뺄셈 없이 오버플로 안전).

### 7.3 `int os_task_list (int pid, int **out)`

`/proc/<pid>/task` 의 엔트리 이름이 OS 스레드 id(TID)다.

1. `opendir ("/proc/%d/task")` 실패 시 −1 (`*out` 불변).
2. `readdir` 반복: 이름이 `.` 으로 시작하면 건너뜀. 배열이 차면 용량을 256 에서 시작해 2배씩 `realloc`; 실패 시 해제·`closedir` 후 −1. `atoi (d_name)` 추가.
3. `closedir`.
4. 0 개면 `*out = NULL`, 0 반환.
5. 오름차순 `qsort`, `*out` 에 배열(호출자가 `free`), 개수 반환.

### 7.4 `int os_task_known (const int *tasks, int n, int tid)`

`n > 0` 이고 정렬된 `tasks` 에서 `bsearch` 로 `tid` 발견 시 1.

### 7.5 `int read_small (const char *path, char *buf, size_t n)`

1. `open (O_RDONLY)` 실패 시 −1.
2. `read` 1회로 최대 `n - 1` 바이트, `close`.
3. `got <= 0` 이면 −1 (빈 파일도 실패).
4. `buf[got] = '\0'`, `got` 반환.

procfs 의 작은 파일은 한 번의 read 로 전체가 반환된다는 전제. `n - 1` 을 넘는 내용은 잘린다.

### 7.6 `int os_task_comm (int pid, int tid, char *out, size_t n)`

엔진이 OS 스레드에 붙인 이름(예: `"pgbuf-page-flus"`). Linux 사실: `/proc/<pid>/task/<tid>/comm` 은 최대 15자 + `'\n'` (`TASK_COMM_LEN` 16).

1. `now = time (NULL)`, `h = (unsigned) tid % 256`.
2. `cache[h].tid == tid` 이고 `now - cache[h].at < 5` 이면 `snprintf (out, n, "%s", cache[h].comm)` (`n − 1` 바이트에서 절단, NUL 종료), 0 반환.
3. `/proc/%d/task/%d/comm` (경로 버퍼 80)을 `read_small` (버퍼 64). 실패 시 `out[0] = '\0'`, −1 (캐시 불변).
4. `len = strcspn (buf, "\n")` (첫 `'\n'` 앞까지; `'\n'` 이 없으면 읽은 내용 전체 = 최대 63바이트), `len >= n` 이면 `len = n − 1`. `memcpy (out, buf, len)`, `out[len] = '\0'`.
5. `text_sanitize (out)` (스레드는 자기 이름을 임의로 정할 수 있음).
6. 캐시 갱신: `tid`, `at = now`, `snprintf (cache[h].comm, 16, "%s", out)` (최대 15바이트).
7. 0 반환.

캐시 키에 pid 가 없다(대상은 프로세스당 하나라는 전제).

### 7.7 `int os_task_state (int pid, int tid, char *state, long *ticks)`

Linux 사실(`proc(5)`): `/proc/<pid>/task/<tid>/stat` 은 `pid (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime ...`. `comm` 에 공백·`)` 가 들어갈 수 있으므로 마지막 `)` 뒤부터 파싱한다. `utime`/`stime` 은 클럭 틱 단위.

1. `/proc/%d/task/%d/stat` 을 `read_small` (버퍼 512). 실패 시 −1.
2. `p = strrchr (buf, ')')`. 없으면 −1.
3. `sscanf (p + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", state, &ut, &st)` 가 3 이 아니면 −1. (상태 문자 1개, 정수 5개·부호 없는 수 5개 건너뜀, utime, stime.)
4. `*ticks = ut + st`, 0 반환.

상태 문자 예: `R` 실행, `S` 수면, `D` 디스크 대기 등. `*state` 는 문자 1개(종료 NUL 없음).

### 7.8 `long os_clock_ticks (void)`

`sysconf (_SC_CLK_TCK)` 가 양수면 그 값, 아니면 100.

### 7.9 `int os_log_device (int pid, unsigned *major_out, unsigned *minor_out, char *name, size_t n)`

활성 로그 볼륨(파일 이름이 `_lgat` 로 끝남)이 놓인 블록 장치. 반환 1 = 장치 이름까지 확인, 0 = 블록 장치 아님/찾지 못함.

Linux 사실: `/proc/<pid>/fd/<n>` 은 열린 파일 경로를 가리키는 심볼릭 링크이며, 이를 `stat` 하면 링크를 따라가 실제 파일의 `st_dev` 를 얻는다. overlay·tmpfs·NFS 등은 익명 장치로 major 0 이다. fd 디렉터리 판독도 ptrace 수준 권한이 필요하다.

1. `opendir ("/proc/%d/fd")` 실패 시 0.
2. 엔트리마다(`.` 시작 제외; 루프 조건은 `while (!found && (e = readdir (d)) != NULL)` 이지만 아래 3에서 무조건 `break` 하므로 실질 종료 조건은 "첫 `_lgat` fd 도달"이다):
   1. `fdp = "/proc/<pid>/fd/<name>"` (버퍼 384), `readlink` (버퍼 4096, 최대 4095). `got <= 0` 이면 다음.
   2. 링크 문자열 길이 < 5 이거나 끝 5자가 `"_lgat"` 가 아니면 다음.
   3. 첫 `_lgat` fd 에서: `stat (fdp)` 성공이고 `major (st_dev) != 0` 이면 `*major_out`, `*minor_out` 설정, `found = 1`. **결과와 무관하게 `break`** (첫 `_lgat` fd 만 본다; 그 fd 가 익명 장치면 뒤의 `_lgat` fd 는 보지 않고 0 반환).

"첫" 의 순서: `readdir` 순서 그대로이며 정렬하지 않는다. Linux procfs 의 `/proc/<pid>/fd` 는 fd 번호 오름차순으로 나열되므로(커널 `proc_readfd_common` 이 fd 테이블을 0 부터 순회) 실제로는 **가장 작은 번호의 `_lgat` fd** 다. 엔진은 활성 로그 볼륨을 한 번만 열므로 보통 `_lgat` fd 는 1개다.
3. `closedir`. `!found` 이면 0.
4. `name[0] = '\0'` 후 `os_diskstats (major, minor, …, name, n)` 호출(카운터는 버림).
5. `name[0] != '\0'` 이면 1, 아니면 0. (반환 0 이어도 `major_out`/`minor_out` 은 설정되었을 수 있다.)

### 7.10 `int os_diskstats (unsigned major_n, unsigned minor_n, uint64_t *wr_ios, uint64_t *wr_ms, uint64_t *io_ms, char *name, size_t namen)`

Linux 사실: `/proc/diskstats` 한 줄 = `major minor name` + 통계 필드(커널 버전에 따라 11, 15, 17 개). 앞 11 개 의미:

| 인덱스 `v[i]` | diskstats 필드 | 사용 |
|---|---|---|
| 0 | reads completed | |
| 1 | reads merged | |
| 2 | sectors read | |
| 3 | ms spent reading | |
| 4 | writes completed | `*wr_ios` |
| 5 | writes merged | |
| 6 | sectors written | |
| 7 | ms spent writing | `*wr_ms` |
| 8 | I/Os currently in progress | |
| 9 | ms spent doing I/Os (장치 busy 시간) | `*io_ms` |
| 10 | weighted ms doing I/Os | |

1. `fopen ("/proc/diskstats")`. 실패면 −1.
2. 줄마다(버퍼 256): `sscanf (" %u %u %63s %llu×11")` 가 14 개를 채우고 major/minor 가 일치하면 위 세 값 설정, `name != NULL` 이면 장치 이름(최대 63자, `namen` 에 맞춰 절단) 복사, 0 반환 준비 후 중단.
3. 닫고 결과 반환(일치 없으면 −1). 255 바이트를 넘는 줄은 두 조각으로 읽혀 일치하지 않는다.

---

## 8. servers 모듈 (`ctv_proc.c`)

### 8.1 책임

procfs 로 실행 중 `cub_server` 를 찾는다. 상태 없음.

Linux 사실: `/proc/<pid>/comm` 은 실행 파일 이름(최대 15자) + `'\n'`. `/proc/<pid>/cmdline` 은 argv 를 NUL 로 구분해 이어 붙인 것이며, cub_server 는 `argv[1]` 이 DB 이름이다.

### 8.2 `int server_by_pid (int pid, SERVER *out)`

반환 0 = cub_server 이고 `out` 채움, −1 = 아님/판독 불가(`out` 미변경 또는 미정).

1. `pid <= 0` 이면 −1.
2. `/proc/%d/comm` (경로 버퍼 300) 을 `read_small` (버퍼 4096). 실패 시 −1. 첫 `'\n'` 에서 종료.
3. 정확히 `"cub_server"` 가 아니면 −1.
4. `/proc/%d/cmdline` 을 `read_small` (최대 4095 바이트). 실패(빈 cmdline 포함, 커널 스레드·좀비) 시 −1.
5. `out->pid = pid`, `out->db = "?"`.
6. `i = 0..got-2` 에서 첫 NUL 을 찾으면 `buf + i + 1` (argv[1]) 를 `out->db` 에 복사(최대 127자), `text_sanitize`, 중단. NUL 이 마지막 바이트뿐이면(인자 없음) `"?"` 유지.
7. 0 반환.

### 8.3 `int list_servers (SERVER *out, int max)`

1. `opendir ("/proc")` 실패 시 0.
2. `readdir` 하며 `n < max` 인 동안 `server_by_pid (atoi (d_name), &out[n])` 가 0 이면 `n++`. 숫자가 아닌 이름은 `atoi` 0 → 1단계에서 거부.
3. `closedir`, `n` 반환.

순서는 `readdir` 순서(정렬하지 않음). 호출자는 `SERVERS_MAX` (1024) 크기 배열을 준다. 루프 조건은 `(e = readdir (d)) != NULL && n < max` 순서로 평가된다(가득 찬 뒤에도 `readdir` 1회가 더 불린다; 결과 영향 없음).

### 8.4 목록 모드(`-l`) 출력 — 정본 06 §2.10 6단계

`list_servers` 결과를 출력 단계에서도 **정렬하지 않고** `readdir` 순서 그대로 쓴다.

| 상황 | 스트림 | 바이트 |
|---|---|---|
| `nserv == SERVERS_MAX` (목록 전에) | stderr (`warn`) | `cub_ctv: more cub_server processes than can be listed: give a pid if yours is missing\n` / `cub_ctv: cub_server 프로세스가 목록 한도를 넘음: 없으면 pid 로 지정하십시오\n` |
| `nserv == 0` | stderr (`die`, exit 1) | `cub_ctv: no cub_server process found\n` / `cub_ctv: cub_server 프로세스를 찾지 못했습니다\n` |
| 머리 줄 | stdout | `candidate cub_server processes:\n` / `cub_server 후보 프로세스:\n` (색 없음) |
| 서버마다 | stdout | `printf ("  %8d  %s\n", pid, db)` — 공백 2 + pid 8자리 오른쪽 정렬 + 공백 2 + db(정화된 값, 폭 지정 없음) |

반환 0. `-l` 과 대상을 함께 주어도 목록이 실행된다(`o.list || o.target == NULL`).

---

## 9. ELF 모듈 (`ctv_elf.c`)

### 9.1 책임과 상태

대상에 매핑된 `libcubrid.so` 의 로드 주소를 maps 에서 찾고, 디스크 파일의 심볼 표에서 링크 시점 주소를 찾는다. 런타임 주소 = 링크 주소 + 로드 바이어스, 바이어스 = (가장 낮은 매핑 시작) − (파일 오프셋 0 인 `PT_LOAD` 의 `p_vaddr`) — 이 계산은 호출자(`cub_ctv.c` 의 `locate_lib`)가 한다.

| 정적 상태 `elf_file` | 타입 | 초기값 | 의미 |
|---|---|---|---|
| `path` | `char[4096]` | `""` | 현재 매핑된 파일 경로 |
| `m` | `unsigned char *` | `NULL` | 파일 전체의 읽기 전용 mmap |
| `size` | `uint64_t` | 0 | 파일 크기 |

매핑은 같은 경로의 다음 조회에서 재사용되고, 다른 경로가 오면 해제 후 교체된다. 마지막 매핑은 프로세스 종료까지 유지.

### 9.2 `static int elf_map (const char *lib)`

1. `m != NULL` 이고 `path == lib` (문자열 비교) 이면 `ELF_OK`.
2. `m != NULL` 이면 `munmap`, `m = NULL`.
3. `open (lib, O_RDONLY)` 실패 → `ELF_NOFILE`.
4. `fstat` 실패 또는 `st_size < 64` (`sizeof (Elf64_Ehdr)`) → fd 닫고 `ELF_BAD`.
5. `mmap (NULL, st_size, PROT_READ, MAP_PRIVATE, fd, 0)`, fd 닫음. 실패 → `ELF_BAD`.
6. `path`, `m`, `size` 기록, `ELF_OK`.

### 9.3 `int elf_lookup (const char *lib, const char *want, uint64_t *value, uint64_t *load0_vaddr)`

정의된 심볼 `want` 의 `st_value` 와, 파일 오프셋 0 의 `PT_LOAD` 가상 주소. `.dynsym` 과 `.symtab` 모두 대상. 모든 표 오프셋을 파일 크기 대비 검사(불변식 I-13).

1. `rc = elf_map (lib)`; `ELF_OK` 가 아니면 그대로 반환.
2. ELF 헤더 검사 — 하나라도 위반이면 `ELF_BAD`:
   - `e_ident[0..4) == "\x7fELF"` (`ELFMAG`, `SELFMAG` = 4)
   - `e_ident[EI_CLASS] == ELFCLASS64`
   - `e_phentsize == 56`, `e_shentsize == 64`
   - `e_phoff <= fsz` 이고 `e_phnum * 56 <= fsz - e_phoff`
   - `e_shoff <= fsz` 이고 `e_shnum * 64 <= fsz - e_shoff`
   (엔디언, `e_machine`, `e_type` 은 검사하지 않음.)
3. `*load0_vaddr = 0`; 프로그램 헤더를 순서대로 보며 `p_type == PT_LOAD` 이고 `p_offset == 0` 인 첫 항목의 `p_vaddr` 를 기록. (심볼을 못 찾아도 이 값은 설정된다.)
4. 섹션 헤더를 순서대로(`found` 가 될 때까지):
   1. `sh_type` 이 `SHT_SYMTAB` 도 `SHT_DYNSYM` 도 아니면 다음.
   2. `sh_entsize != 24`, `sh_link >= e_shnum`, `sh_offset > fsz`, `sh_size > fsz - sh_offset` 중 하나면 다음.
   3. 연결된 문자열 표 `stroff = sh[sh_link].sh_offset`, `strsz = sh[sh_link].sh_size`; `stroff > fsz`, `strsz > fsz - stroff`, `strsz == 0` 중 하나면 다음.
   4. 심볼 `k = 0..sh_size/24-1`:
      - `st_shndx == SHN_UNDEF`, `st_name == 0`, `st_name >= strsz` 이면 건너뜀.
      - `strncmp (strtab + st_name, want, strsz - st_name) == 0` 이고 `strlen (want) < strsz - st_name` 이면 일치: `*value = st_value`, `found = 1`, 중단. (두 조건이 합쳐 "표 경계 안에서 NUL 까지 정확히 같음"을 보장한다.)
5. `found` 면 `ELF_OK`, 아니면 `ELF_NOSYM`.

비고: 심볼 종류(`STT_*`)·바인딩(`STB_*`)은 보지 않는다. 같은 이름이 여러 표에 있으면 섹션 헤더 순서상 먼저 나온 표의 첫 항목이 이긴다. 스트립된 라이브러리도 `.dynsym` 에 전역 심볼이 남아 있으면 찾는다.

### 9.4 `int find_lib_base (int pid, uint64_t *base, char *path, size_t pathn, char *range, size_t rangen)`

Linux 사실: `/proc/<pid>/maps` 한 줄 = `start-end perms offset dev inode [pathname]`, 주소는 16진, 필드는 공백(정렬용 다중 공백 포함)으로 구분. 경로에는 공백이 들어갈 수 있고, 디스크에서 지워지거나 교체된 파일은 끝에 `" (deleted)"` 가 붙는다. 교체된 매핑은 `/proc/<pid>/map_files/<start-end>` 로 여전히 읽을 수 있다(보통 root 필요).

지역 버퍼: `mp[64]`, `line[PATH_MAX + 512]` (= 4608), `dpath[4608]`, `drange[64]`.

1. `fopen ("/proc/%d/maps")` 실패 → `LIB_NOMAPS` (`errno` 보존; 호출자가 `strerror (errno)` 표시).
2. `range[0] = '\0'`.
3. 줄마다:
   1. 줄에 `'\n'` 이 없고 EOF 도 아니면(버퍼보다 긴 줄) 줄 끝까지 `fgetc` 로 버리고 다음 줄(어떤 경로보다 길어 우리 것이 아님).
   2. `"libcubrid.so"` 부분 문자열이 없거나 `sscanf ("%lx", &start)` 실패면 다음.
   3. 공백/탭으로 구분된 필드 5개를 건너뛰어 `p` 를 6번째 필드(경로)에 둔다. `*p != '/'` 이면 다음.
   4. 끝의 `'\n'` 과 공백을 제거.
   5. 길이 > 10 이고 끝이 `" (deleted)"` 이면 `is_del = 1`, 그 접미사를 잘라 원래 경로로.
   6. 기본 이름 `bn` (마지막 `/` 뒤)이 `"libcubrid.so"` 로 시작하고 그 다음 문자가 NUL 또는 `'.'` 이 아니면 다음. (`libcubrid.so`, `libcubrid.so.11`, `libcubrid.so.11.5.0` 허용; `libcubrid.so_x` 등 거부.)
   7. `is_del`: 아직 deleted 후보가 없거나 `start < dbest` 이면 `dbest = start`, `drange` = 줄의 첫 공백 앞 토큰(`start-end`), `dpath = p`. `deleted = 1`. 다음.
   8. 정상: 아직 없거나 `start < best` 이면 `best = start`, `got = 1`, `path = p` (`pathn` 절단), `range` = 첫 토큰(`rangen` 절단).
4. `fclose`.
5. `got == 0`:
   - `deleted` 면 `*base = dbest`, `path = dpath`, `range = drange`, `LIB_DELETED`.
   - 아니면 `LIB_NONE` (`*base`, `path` 미설정; `range` 는 빈 문자열).
6. `*base = best`, `LIB_OK`.

정상 매핑과 deleted 매핑이 섞여 있으면 정상 매핑이 우선한다. 가장 낮은 시작 주소의 매핑이 ELF 헤더(파일 오프셋 0)를 담는다는 전제.

### 9.5 호출자 측 사용(참고, `cub_ctv.c`)

정본은 06 §2.9 (`locate_lib`). 재구현에 필요한 사실을 여기에도 모두 적는다. `locate_lib` 지역 버퍼: `range[64]`, `file[4352]`; `lib` 는 `main` 의 `char lib[4096]` (`pathn = 4096`).

| `find_lib_base` 결과 | 판독할 파일(`file`) |
|---|---|
| `LIB_OK` | `snprintf (file, 4352, "/proc/%d/root%s", pid, lib)` (다른 마운트 네임스페이스·컨테이너 대응) |
| `LIB_DELETED` | `snprintf (file, 4352, "/proc/%d/map_files/%s", pid, range)`. **알림 없음**(경고·표시 줄을 따로 찍지 않는다; 대상 줄의 `lib=` 는 ` (deleted)` 를 뗀 원래 경로) |
| `LIB_NOMAPS` | `die ("cannot read /proc/<pid>/maps: %s (run as the server's OS user or root)", "/proc/<pid>/maps 판독 실패: %s (서버의 OS 사용자 또는 root 로 실행)", strerror (errno))` |
| `LIB_NONE` (그 밖) | `die ("libcubrid.so not mapped in target", "대상에 libcubrid.so가 매핑되어 있지 않습니다", NULL)` |

`<pid>` 표기: 위 두 `die` 형식(그리고 06 §2.10 의 `cannot open /proc/<pid>/mem: %s`)에서 `<pid>` 는 **글자 그대로** `<pid>` 다. pid 숫자를 미리 끼워 넣지 않으며 `%s` 자리에는 `strerror (errno)` 만 들어간다. 예: `cub_ctv: cannot read /proc/<pid>/maps: Permission denied (run as the server's OS user or root)\n` (stderr).

필수 심볼 조회와 재시도:

1. `r = elf_lookup (file, "csectgl_Critical_sections", sym_cs, &load0)`.
2. 재시도 조건(세 가지 모두): `r == ELF_NOFILE` **그리고** `access (file, F_OK) != 0` **그리고** `strstr (file, "/root/") != NULL`. 판정은 `errno` 가 아니라 `access(F_OK)` 의 성공 여부다 — 따라서 `ENOENT` 뿐 아니라 `EACCES`(다른 사용자의 `/proc/<pid>/root` 통과 불가) 등 **모든** `access` 실패가 재시도를 일으킨다. `access` 가 성공하는데(파일은 보이는데) `open` 이 실패한 경우는 재시도하지 않는다. 세 번째 조건 때문에 `LIB_DELETED`(`/map_files/` 경로)는 재시도하지 않는다.
3. 재시도: `src = lib` (maps 경로 그 자체), `r = elf_lookup (lib, …)`. 이후 선택 심볼 조회도 `src` 를 쓴다.
4. 결과별 처리(모두 stderr, `die` → exit 1; 메시지 인자는 `file` 이 아니라 `lib`):

| `r` | 조건 | 영어 | 한국어 |
|---|---|---|---|
| `ELF_OK` | — | (계속) | |
| `ELF_NOFILE` | `strstr (file, "/map_files/") != NULL` | `mapped libcubrid.so was deleted/replaced on disk and its mapping is not readable (map_files needs root) - restart the server or run as root` (인자 NULL) | `매핑된 libcubrid.so가 디스크에서 교체/삭제되었고 매핑도 읽을 수 없습니다(map_files 는 root 필요) - 서버 재시작 또는 root 로 실행` |
| `ELF_NOFILE` | 그 밖 | `cannot open %s (permission? replaced?)` | `%s 열기 실패 (권한? 교체됨?)` |
| `ELF_BAD` | — | `%s is not a sane ELF64 image (truncated/corrupt) - refusing` | `%s가 정상 ELF64가 아님(절단/손상) - 중단` |
| `ELF_NOSYM` (그 밖) | — | `csectgl_Critical_sections not found in %s - unsupported build` | `%s에서 csectgl_Critical_sections 심볼 없음 - 미지원 빌드` |

5. `*bias = lib_base − load0`.
6. 선택 심볼(`src` 에서): `sync_Stats` → `sym_pool`, `log_Gl` → `sym_loggl`, `_ZN9cubthreadL7ManagerE` → `sym_thr`. 각각 `ELF_OK` 가 아니면(어떤 실패든) 0, **메시지 없음**.

`mem_open` 실패(호출자 `main`, `locate_lib` 다음): `die ("cannot open /proc/<pid>/mem: %s", "/proc/<pid>/mem 열기 실패: %s", strerror (errno))`.

---

## 10. 빌드 (`build.sh`)

### 10.1 정의

POSIX `sh` 스크립트, `set -e`. 산출물은 저장소 루트의 `cub_ctv` 단일 바이너리. 완전 정적 링크를 우선해 어느 배포판에서도 한 파일로 실행되게 한다.

| 항목 | 값 |
|---|---|
| `HERE` | 스크립트 디렉터리의 절대 경로(`cd "$(dirname "$0")" && pwd`) |
| `CFLAGS` | `-O2 -Wall -Wextra -Wshadow` |
| 포함 경로 | `-I.` (`src/` 안에서 컴파일) |
| 추가 라이브러리 | 없음(`-l` 미지정) |
| 출력 | `$HERE/cub_ctv` |

### 10.2 소스 목록 `SRCS` (순서 그대로, 26개)

`cub_ctv.c ctv_cs.c ctv_dump.c ctv_elf.c ctv_text.c ctv_field.c ctv_sched.c ctv_mem.c ctv_msg.c ctv_os.c ctv_pool.c ctv_proc.c ctv_screen.c ctv_verdict.c ctv_thr_probe.c ctv_thr.c ctv_tran.c ctv_frame.c ctv_rec.c ctv_term.c ctv_rows.c ctv_json.c ctv_sampler.c ctv_present.c ctv_live.c ctv_replay.c`

목록에 없는 `src/*.c` 는 링크되지 않는다. 파일 이름에 공백이 없어야 한다(단어 분리 사용). 컴파일은 `src/` 안에서 하므로 저장소 경로에 공백이 있어도 된다.

### 10.3 동작

인자 판정은 **첫 인자 `$1` 하나만** 본다. 맨 앞의 `case "${1:-}"` 가 `""`·`--list-sources`·`--dynamic` 외의 값이면 stderr 에 `usage: build.sh [--dynamic | --list-sources]` 를 쓰고 `exit 2`. 둘째 이후 인자는 읽지 않는다: `--list-sources --dynamic` 은 목록만 출력하고 0 종료, `--dynamic --list-sources` 는 동적 빌드다.

1. 인자 `--list-sources`: `for f in $SRCS; do printf '%s\n' "$HERE/src/$f"; done` — 각 소스의 전체 경로를 stdout 에 한 줄씩 출력하고 `exit 0` (테스트가 같은 모듈을 링크할 때 사용). `cd`·`mktemp` 보다 앞이라 임시 파일을 만들지 않는다.
2. `cd "$HERE/src"`, 임시 로그 파일 `LOG=$(mktemp)`, `trap 'rm -f "$LOG"' EXIT`.
3. 인자 `--dynamic`: `gcc $CFLAGS -I. -o "$HERE/cub_ctv" $SRCS` (libc 동적 링크; NOTICE 의 LGPL 목적 파일 제공 의무 회피용, 진단은 gcc 그대로 stderr), `echo "built (dynamic): $HERE/cub_ctv"` (stdout), `! command -v file >/dev/null || file "$HERE/cub_ctv"`, `exit 0`.
4. 기본: `LC_ALL=C gcc $CFLAGS -I. -static -o "$HERE/cub_ctv" $SRCS 2>"$LOG"`.
   - 성공: `cat "$LOG" >&2` (경고를 stderr 로), `echo "built (full static): $HERE/cub_ctv"` (stdout).
   - 실패이고 `grep -Eq "cannot find -lc|unable to find library -lc|library not found for -lc" "$LOG"` (GNU ld / gold / lld 문구) 성공: `echo "full-static unavailable - building dynamic (libc only)"` (stdout, 실패한 정적 링크의 로그는 **출력하지 않고 버림**) 후 `gcc $CFLAGS -I. -o "$HERE/cub_ctv" $SRCS` (진단은 stderr 직결, `LC_ALL=C` 없음). 이 컴파일이 실패하면 `set -e` 로 스크립트가 그 종료 코드로 끝난다. 성공해도 **`built (dynamic): …` 줄은 찍지 않고** 바로 5단계로 간다.
   - 그 외 실패: `cat "$LOG" >&2`, `exit 1`.
5. (4단계의 성공·대체 경로 공통) `! command -v file >/dev/null || file "$HERE/cub_ctv"` — `file` 명령이 있으면 그 출력(stdout), 없으면 생략. 이 관용구는 `set -e` 하에서도 실패하지 않는다. 스크립트 종료 코드 0.

진행 문구(`built …`, `full-static unavailable …`)는 `echo` 로 stdout, usage 오류 줄은 stderr, 소스 목록(`--list-sources`)은 `printf` 로 stdout, 컴파일러 진단은 stderr 에 나간다. 문구에 색·접두어는 없다.

`LC_ALL=C` 는 링커 오류 문구를 영어로 고정해 4단계의 grep 이 동작하게 한다.

---

## 11. 정리표: 실패 반환 규약

| 함수 | 성공 | 실패 | 실패 시 출력 버퍼 |
|---|---|---|---|
| `mem_open` | 0 | −1 | — |
| `mem_read` | 0 | −1 | 부분 덮임 가능 |
| `mem_gather_ok` | 0 (항상) | — | 실패 항목 0 채움, `ok=0` |
| `mem_gather_field` | 0 | −1 (malloc) | `out`·`ok` 0 채움 |
| `mem_read_shrink` | 읽은 바이트 | 0 | 미정 |
| `mem_read_ptr` | 0 | −1 | 읽힌 값 잔존 가능 |
| `mem_cstr` | 0 | −1 | 미정 |
| `name_of` | `out` | `out = "?"` | — |
| `field_probe` | 1 | 0 | 상태 불변 |
| `os_task_list` | 개수(≥0) | −1 | `*out` 불변 |
| `read_small` | 길이(>0) | −1 | 미정 |
| `os_task_comm` | 0 | −1 | `out = ""` |
| `os_task_state` | 0 | −1 | `*state` 부분 설정 가능 |
| `os_log_device` | 1 | 0 | major/minor 설정됐을 수 있음 |
| `os_diskstats` | 0 | −1 | 미변경 |
| `server_by_pid` | 0 | −1 | 미변경 |
| `list_servers` | 개수 | 0 | — |
| `elf_lookup` | `ELF_OK` | `ELF_NOFILE`/`ELF_BAD`/`ELF_NOSYM` | `ELF_NOSYM` 시에도 `*load0_vaddr` 설정 |
| `find_lib_base` | `LIB_OK` | `LIB_NONE`/`LIB_DELETED`/`LIB_NOMAPS` | `LIB_DELETED` 시 base/path/range 설정 |
| `die` | — | 반환 안 함, `exit (1)` | — |
