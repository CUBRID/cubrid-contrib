# cub_ctv 재구현 명세 개요 (Reimplementation Specification Overview)

이 명세 묶음(`docs/spec/`)은 소스 없이 문서만으로 `cub_ctv` 를 같은 동작·같은 출력·같은 기록 형식으로 다시 작성할 수 있도록 쓴 것이다. 각 문서는 소스(`src/`)에서 직접 옮겼고, 소스와 다르면 소스가 기준이다.

## 1. 도구의 정의

| 항목 | 내용 |
|---|---|
| 목적 | 실행 중인 CUBRID `cub_server` 의 크리티컬 섹션(CS) 경합을 대상 프로세스에 접속하지 않고 보여 준다: 누가 쥐고 누가 기다리는지, 얼마나 잃는지, 어느 세션인지 |
| 판독 방법 | `/proc/<pid>/maps`(라이브러리 위치), `/proc/<pid>/mem` pread, `process_vm_readv`(작은 범위 묶음), procfs 텍스트(task·stat·comm·fd·diskstats) |
| 하지 않는 것 | ptrace attach, 대상 메모리 쓰기, 대상 잠금 획득, SQL 접속, csql/CAS/tranlist 실행, 엔진 소스 변경 |
| 엔진 정보 출처 | CUBRID 헤더를 쓰지 않는다. 심볼은 대상이 매핑한 `libcubrid.so` 의 ELF 에서 찾고, 구조체 오프셋은 런타임 불변식(I-0~I-20)으로 찾거나 그것에서 유도해 교차검증한다 |
| 검증 실패 시 | 필수 구조(CS 배열)는 불변식 id 와 함께 거부(종료 1), 부가 정보(트랜잭션·스레드·풀)는 그 기능만 강등하고 레이아웃 줄에 표시 |
| 산출물 | 단일 실행 파일 `cub_ctv`(정적 링크 우선) |

## 2. 모듈 층과 문서 대응

의존은 대체로 위 층에서 아래 층으로 흐른다. 예외: `die` 의 화면 복원(`screen_restore`), 덤프 경로의 JSON·색 출력(`json_fstr`·`json_ident`·`clr`)과 프레임 풀 필드 기록, 프레임의 판정 이력·시계 사용(`history_export`·`mono_s`). 필드 검증 상태·재시도 일정·강등 비트·tty 상태·기록 파일 상태는 각각 한 모듈만 쓴다.

| 층 | 모듈 | 명세 문서 |
|---|---|---|
| 기반 | `ctv_text.c` `ctv_mem.c` `ctv_field.c` `ctv_sched.c` `ctv_msg.c` `ctv_os.c` `ctv_proc.c` `ctv_elf.c`, `build.sh`, `ctv.h` 공통부 | [01-base.md](01-base.md) |
| 대상 해석 | `ctv_cs.c` `ctv_pool.c` (+ `ctv_dump.c`) | [02-cs-pool-dump.md](02-cs-pool-dump.md) |
| 대상 해석 | `ctv_tran.c` | [03-transactions.md](03-transactions.md) |
| 대상 해석 | `ctv_thr_probe.c` `ctv_thr.c` | [04-threads.md](04-threads.md) |
| 프레임 | `ctv_frame.c` `ctv_rec.c` | [05-frame-record.md](05-frame-record.md) |
| 표시·실행 | `ctv_verdict.c` `ctv_screen.c` `ctv_rows.c` `ctv_json.c` `ctv_present.c` `ctv_term.c` `ctv_sampler.c` `ctv_live.c` `ctv_replay.c` `cub_ctv.c` | [06-display-run.md](06-display-run.md) |

엔진 쪽 배경(CS 가 무엇을 지키는지, 진입·대기 규칙, 통계의 뜻)은 `docs/critical-sections.md`, 불변식 원장은 `PR.md` 부록 A.

## 3. 데이터 흐름

```text
/proc/<pid>/maps ─► ELF(libcubrid.so) ─► 심볼: csectgl_Critical_sections, sync_Stats, log_Gl, cubthread::Manager
                                              │
/proc/<pid>/mem ─► ctv_mem ─► 프로브(불변식) ─► CS_LAYOUT / TRAN_GEOM / THR_GEOM (+ FIELD 6개, SCHED)
                                              │
              라이브: 샘플러(LIVE_WIN, --hz) ──┤── 스냅샷: CS 행 1회
                                              ▼
                       view_capture: VIEW_FRAME (화면 한 장의 모든 답, 한 시점)
                        │                │                       │
                        ▼                ▼                       ▼
                 표시(rows/screen/json)  --record (CTVREC 5)     --dump (JSON, 재생 불가)
                        ▲
--replay ─► ctv_rec: 기록 줄 → VIEW_FRAME 복원 (같은 표시 코드)
```

규칙: 표시 코드는 프레임에서만 답을 받는다. 그래서 같은 보기 옵션이면 라이브 화면과 그 기록의 재생 화면이 바이트 단위로 같다.

## 4. 실행 모드

| 모드 | 입력 | 동작 | 명세 |
|---|---|---|---|
| 도움말 | 인자 없음, `--ko`/`--en` 만, `-h` | 사용법 출력, 종료 0 | 06 |
| 목록 | `-l` | cub_server pid·DB 목록 | 01, 06 |
| 스냅샷 | `<db>` 또는 `<pid>` | CS 표 + 트랜잭션 표 + 풀 1회 | 02~06 |
| 표만 | `--trans` (기록·덤프 없음) | 트랜잭션 표만, 스레드 프로브 생략 | 03, 06 |
| 라이브 | `-i` | `--hz` 샘플링, `--interval` 마다 출력(터미널=요약 화면, 파이프=스크롤) | 06 |
| 기록 | `--record FILE`, 라이브 `[r]` | 프레임을 CTVREC 5 로 | 05 |
| 재생 | `--replay FILE` | 기록을 같은 표시 코드로(터미널=대화형) | 05, 06 |
| 덤프 | `--dump FILE` | 읽은 것 전부 JSON(재생 불가) | 02 |

## 5. 공통 규약

| 항목 | 규약 |
|---|---|
| 언어 | 모든 사용자 메시지는 `M("영문", "한글")` 쌍. 선택은 `LC_ALL` → `LC_MESSAGES` → `LANG` 의 앞 2글자가 대소문자 무시로 `ko` 이면 한글, `--ko`/`--en` 이 우선(명령줄 어디에 있든, 마지막 것). 메시지는 UTF-8 이므로 한글은 UTF-8 로캘에서만: 문자셋(`LC_ALL` → `LC_CTYPE` → `LANG` 의 코드셋)이 UTF-8 이 아니면 영문(경고 1줄). 영문 출력은 ASCII 만 쓴다. 표 헤더·상태 토큰은 정렬을 위해 항상 영문 |
| 외부 문자열 | 대상 메모리·procfs·경로·DB 이름에서 온 문자열은 출력 전 정화(`text_sanitize`: 제어 문자·깨진 UTF-8 을 `?`) |
| 폭 | 터미널 열 폭은 UTF-8 코드 포인트 단위(동아시아 전각·이모지 2열, 그 밖 1열) |
| 오류 | `die()` 는 대체 화면을 먼저 복구하고 stderr 에 `cub_ctv: ` 접두로 출력, 종료 1 |
| 강등 | 강등 비트(`DEG_*`)는 `deg_set` 만 바꾼다. 레이아웃 줄과 덤프의 `degraded_features` 가 같은 이름표(`deg_names`)를 쓴다 |
| 파일 생성 | 기록·덤프 파일은 0600, 심볼릭 링크 거부, 정규 파일·링크 1개·소유자 확인 |
| 코드 형식 | C, GCC 기본 표준(`-std` 미지정 = gnu17), `_GNU_SOURCE`, GNU 들여쓰기(함수 이름 0열, 중괄호 별행), 주석은 현재 동작만 1~2줄 |

## 6. 재구현 수용 기준

재구현은 아래 시험을 모두 통과해야 같은 것으로 본다. 서버가 필요 없는 시험은 고정물(`tests/fixtures/`)만으로 돈다.

| 시험 | 확인하는 것 | 서버 |
|---|---|---|
| `tests/check_msg_pairs.py` | 한/영 메시지 쌍의 서식 지시자 동일 | 불필요 |
| `tests/t5_replay.sh` | 고정물 재생 6보기 == 골든 (바이트) + JSON 유효 + 비 UTF-8 로캘은 영문 | 불필요 |
| `tests/t6_view_roundtrip.sh` | 주입한 프레임 기록 → 재생 동일 | 불필요 |
| `tests/t8_replay_keys.py` | 재생 키·요약 화면 (pty) | 불필요 |
| `tests/t11_ident_delta.sh` | 신원 키프레임·변화분 재생, 구간 이동 | 불필요 |
| `tests/t12_field_sched.sh` | FIELD·SCHED 규칙 | 불필요 |
| `tests/t13_verdict.sh` | 판정 수치 골든 | 불필요 |
| `tests/t14_slot_reuse.sh` | 구간 중 재사용된 슬롯: 소유자·대기 홉·세션이 목격된 클라이언트(떠난 클라이언트) 표시 | 불필요 |
| `tests/t15_screen_fit.py` | 요약 화면이 터미널 크기에 맞음: 스크롤 없음, 80칸 이상은 폭 초과 줄 없음 | 불필요 |
| `tests/t2_show_compare.py` | 엔진 `SHOW CRITICAL SECTIONS` 값이 전후 판독 사이 + 자기영향 0 | 필요 |
| `tests/t4_trans_check.sh` | 트랜잭션 표 == `cubrid tranlist` | 필요 |
| `tests/t7_record_equiv.sh` | 라이브 화면 == 그 기록의 재생 | 필요 |
| `tests/t9_quick_trace.sh` | 부하 중 판정·대기 세션 식별 | 필요 |
| `tests/t10_os_probe.sh` | OS tid·스레드 이름·대기 사유 | 필요 |

## 7. 읽는 순서

1. 이 개요 → 2. `01-base.md`(판독·텍스트·필드·일정 규칙) → 3. `02`~`04`(대상 구조와 불변식) → 4. `05`(프레임과 기록 형식) → 5. `06`(판정 계산, 출력 형식, 루프와 키).
