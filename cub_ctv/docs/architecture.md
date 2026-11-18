# 구조 (Architecture)

## 1. 판독 경로

```text
/proc/<pid>/maps ──► libcubrid.so 매핑·경로 ──► 자체 ELF 파서(.dynsym + .symtab)
                                                  csectgl_Critical_sections, sync_Stats,
                                                  log_Gl, cubthread::Manager
/proc/<pid>/mem  ──► mem_pread() ──► 프로브·판독
                                        │
                                        ▼
                      view_capture(): 화면 한 장에 필요한 답을 한 번에 수집 (VIEW_FRAME)
                        CS 행, 라이브 집계, 트랜잭션 신원, 소유자 tid→스레드 칸, 대기 체인
                      view_capture_pool(): 풀 그룹 (스냅샷에서만, 라이브는 pool_state=0)
                         │                         │                          │
                         ▼                         ▼                          ▼
                  렌더러 (rows/screen/json)     --record: 프레임을 텍스트로   --dump: 프레임 + 오프셋·전 슬롯·
                  질의는 프레임에서 답    (구간마다 flush)               전 스레드·전 풀 엔트리 (JSON)
                         ▲
--replay FILE ──► view_replay_next(): 기록된 프레임을 VIEW_FRAME 으로 복원 ──┘
                  (터미널: view_replay_index()로 프레임 위치를 색인해 임의 구간으로 이동)
```

렌더러는 서버도 샘플러 상태(`LIVE_WIN`)도 읽지 않고 프레임(`VIEW_FRAME`)에서만 답을 받는다(행·직전 행·점유/대기 집계·활동은 필드로, 신원·스레드·체인은 `view_identity`, `view_identity_seen`, `view_slot_of_tid`, `view_waiters`, `view_tran_of`, `view_tran_cpid_of`). 예외는 트랜잭션 표의 거부 사유로, 전역 `tran_refused` 를 읽는다. `LIVE_WIN` 은 샘플러가 채우고(디스크 기준값 `dk0`/`dk_ok` 만 프레임의 `view_disk_begin` 이 씀) 구간 끝에 `view_capture` 가 프레임으로 한 방향 복사한다. 그래서 라이브 실행과 재생이 같은 출력 코드를 지나며, 같은 보기 옵션이면 출력이 바이트 단위로
같다(tests/t7). 디테일 덤프는 재생용이 아니며 `--replay` 가 첫 줄의 형식 표시로 알아보고 거부한다.

대상 메모리 판독은 `ctv_mem.c` 한 곳을 지난다(단건 `/proc/<pid>/mem` pread, 묶음은 `process_vm_readv`; 둘 다 같은 ptrace 읽기 권한). 큰 영역(스레드 엔트리 배열 5.2MB, TDES)은 이미지로 읽고 버퍼에서 검사한다. 스레드 배열은 시작 시 한 번 읽어 개수 세기·검증·첫 m_id 표에 함께 쓰고, 라이브에서는 `/proc/<pid>/task` 목록이 바뀐 초에만 m_id·tid 표를 gather 2회로 재구성한다(m_id 는 스레드가 나고 들 때만 바뀐다). 이미지는 미확정 필드가 있을 때만 재프로빙 일정대로 다시 읽는다(§4). 작은 범위 여럿(CS stats 블록 64B × 18, 대기 사유·`m_status` 4B × 실스레드, tid 4B × 실스레드)은 `mem_gather_ok`(`process_vm_readv`, 호출 1회에 최대 1,024개 범위, 불가 시 pread)로 모아 읽는다. 트랜잭션 신원은 구간마다 디스크립터 포인터 배열을 1회, trid 배열을 gather 1회로 읽고 사용 중인 칸만 CLIENTIDS 구간을 gather 1회로 읽어 (trid, CLIENTIDS) 가 지난 해독과 같으면 재사용하고, 바뀐 칸(또는 3구간 경과)만 문자열을 다시 해독한다(판독량은 PR.md 부록 B.2). libcubrid.so 는 읽기 전용 mmap 1회로 심볼을 찾는다.

## 2. 모듈

| 층 | 파일 | 책임 | 불변식 |
|---|---|---|---|
| 기반 | `ctv_text.c` | UTF-8 길이·열 폭·열 기준 자르기/채우기, 외부 문자열 정화, FNV 해시 | — |
| 기반 | `ctv_mem.c` | 대상 메모리 판독의 유일한 경로(pread, gather, 필드 gather, 문자열, 이름 캐시) | — |
| 기반 | `ctv_field.c` | **프로브 필드 상태기계** `FIELD`: 미확정/잠정/확정, 런타임 집계(`field_sight`), 기각 목록(`field_reject`), 재기회 프로빙(`field_probe`) | — |
| 기반 | `ctv_sched.c` | **재시도 일정** `SCHED`(빠른 회차 2→64초, 느린 단계 5분, 앞당김, 재시작)과 최소 간격 게이트 `every` | — |
| 기반 | `ctv_msg.c` | 한/영 메시지, `die`/`warn`/`warnf`, 강등 비트의 유일한 쓰기 함수 `deg_set` | — |
| 기반 | `ctv_os.c / ctv_proc.c / ctv_elf.c` | procfs(task·stat·comm·로그 장치), cub_server 목록, ELF 심볼·매핑 베이스 | I-13 |
| 대상 해석 | `ctv_cs.c` | CS 배열 레이아웃 프로브(거부 사유를 반환), 행 해독, 대기 큐 머리(promoter→writer) | I-0~I-5 |
| 대상 해석 | `ctv_tran.c` | TDES·CLIENTIDS 프로브, 신원 판독·캐시 | I-6~I-11 |
| 대상 해석 | `ctv_thr_probe.c` | 배열 위치를 찾고(I-14, `thr_geom` 의 base·stride·off_index 기록) 각 필드의 후보 오프셋을 고르는 판독·탐색 함수. 대상 메모리(매니저 객체, struct pthread 표본 등)와 배열 이미지를 읽고, FIELD 상태는 바꾸지 않음 | I-14~I-20 탐색 |
| 대상 해석 | `ctv_thr.c` | 스레드 필드의 수명(FIELD 6개 + SCHED), m_id·tid 표, 대기 체인, 대기 사유, 덤프. `thr_geom` 의 배열 위치(base·count·off_tran 초기값)는 `thr_probe` 가, 필드 유효성·오프셋과 `DEG_THR_*` 비트는 `thr_publish` 가 씀 | I-14~I-20 유지 |
| 대상 해석 | `ctv_pool.c` | sync_Stats 풀 체인 순회·집계 | I-12 |
| 프레임 | `ctv_frame.c` | 프레임(`VIEW_FRAME`) 캡처(스냅샷·라이브 구간·트랜잭션 표만, 풀 그룹은 `view_capture_pool`)와 렌더러용 질의(`view_*`) | — |
| 프레임 | `ctv_rec.c` | 기록 형식: **줄 규칙 표**로 읽기·검증·색인·이동, 쓰기는 `view_record_frame`(줄마다 직접 출력), 신원 키프레임 | — |
| 표시 | `ctv_verdict.c` | 판정 계산: CS 지표·순위, 보유자, 지속성 이력, `VERDICT` 구조체(텍스트 없음) | — |
| 표시 | `ctv_term.c` | 터미널: 크기·색·raw 모드·키·신호·대기(모든 tty 상태 소유) | — |
| 표시 | `ctv_rows.c` | 표 행·소유자 블록·세로 레코드·트랜잭션 표(텍스트) | — |
| 표시 | `ctv_json.c` | JSON 문서 | — |
| 표시 | `ctv_screen.c` | 요약 화면: `VERDICT` 를 그림, 터미널 폭·높이에 맞춤 | — |
| 표시 | `ctv_present.c` | 한 프레임을 모드별로 출력(요약 화면·스크롤 행·트랜잭션 표·JSON) | — |
| 표시 | `ctv_dump.c` | `--dump` 디테일 덤프 | — |
| 실행 | `ctv_sampler.c` | 라이브 표본(`LIVE_WIN`): 점유·대기·활동·큐 순회·OS 표본 | — |
| 실행 | `ctv_live.c` | 라이브 루프: 시작 / 표본 1회 / 구간 마감 / 키 / 종료 단계 함수 | — |
| 실행 | `ctv_replay.c` | 재생 루프(스크롤·대화형) | — |
| 실행 | `cub_ctv.c` | 인자, 대상 선택, 흐름 조립 | — |

의존은 대체로 위 층에서 아래 층으로 흐른다. 아래 층이 위 층을 부르는 예외는 다음과 같다.

| 호출하는 곳 | 호출되는 곳 | 용도 |
|---|---|---|
| `ctv_thr.c`, `ctv_pool.c`, `ctv_tran.c` | `ctv_json.c` (`json_fstr`, `json_ident`) | 덤프 JSON 출력 |
| `ctv_msg.c` (`die`) | `ctv_term.c` (`screen_restore`) | 종료 전 화면 복원 |
| `ctv_pool.c` | `ctv_term.c` (`clr`) | 풀 표의 색 |
| `ctv_frame.c` | `ctv_verdict.c` (`history_export`), `ctv_term.c` (`mono_s`) | 이력 내보내기, 큐 순회 시각 확인 |

상태는 대체로 한 모듈이 소유한다: 스레드 필드는 `ctv_thr.c`(FIELD·SCHED), 강등 비트는 `deg_set`, tty 상태는 `ctv_term.c`, 기록 파일 상태는 `ctv_rec.c`. 남의 상태를 직접 쓰는 예외는 `thr_probe_entries` 의 `thr_geom` 배열 위치, `pool_dump` 의 프레임 풀 필드(`view.pool_state/npool/pool_truncated`), `view_disk_begin` 의 `LIVE_WIN` 디스크 기준값, `cub_ctv.c` 의 `color_on` 이다. 프로브는 거부 사유를 반환값으로 돌려준다(전역 없음).

## 3. 기하 결정 방식

| 대상 | 방식 |
|---|---|
| CS stride / count | `cs_index` 수열(0,1,2,3…)과 이름 인쇄가능성 |
| stats 슬롯 | `*stats.type == CSECT ∧ stats->name == cs->name` 인 유일 오프셋 |
| rwlock / owner / 대기큐 | stats 슬롯에서 유도(rwlock −96, owner −16, writer 큐 −32, promoter 큐 −24, tran −8) 후 `rwlock<0 ⇒ owner≠0` 한 방향(강등은 owner/tran 을 남기므로 역방향은 성립하지 않음. LOG 강등은 HA 모드에서만 일어남. 위반 1회 허용) + 대기 수 범위·큐 포인터 유효성. tran(−8)은 별도 검사 없음 |
| SYNC_STATS 카운터 | 고정 오프셋(nenter 16, nwait 20, nreenter 24, total 32, max 48; 크기 64). type(0)·name(8)만 검증 |
| 트랜잭션 테이블 | `log_Gl` 내 포인터로서 `tdes[k].tran_index == k` |
| CLIENTIDS | std::string 6개 + pid 패턴 (libstdc++ COW/SSO 양 ABI), 테이블 3등분 증인 투표 |
| 스레드 엔트리 배열 | `Manager` 객체 내 포인터로서 `index == 1,2,3,4` |
| `m_id` | 비0·고유·사용자주소 값 개수가 스레드 수 근처인 후보들 중 I-17(OS tid) 이 확인하는 것; I-17 불가 시 개수 = 스레드 수 − 1(여유 8)인 유일 후보 |
| 대기 체인 | 엔진이 깨우는 순서대로 promoter 대기 큐, 이어서 writer 대기 큐(한 체인으로, 겹치는 칸에서 중단). `next_wait_thrd` 오프셋(양 끝이 CS 대기 코드인 링크만 셈): resume_status 뒤 64B 에서 전 엔트리 값이 0 또는 다른 엔트리 포인터인 후보 중 이미지에서 포인터를 보인 유일 후보, 또는 큐 홉 3회에서 단독으로 포인터를 보이고 다른 후보는 한 번도 보이지 않은 후보로 확정(I-19); 매 홉이 배열 내 엔트리인지 확인, 순환 시 중단 |
| OS tid | `m_id` → struct pthread 안에서 표본(최대 32개, 4개 이상) 중 최대 8개를 뺀 나머지가 `/proc/<pid>/task` 의 서로 다른 tid 인 유일 오프셋, 전 실스레드로 확인 (I-17) |
| 대기 사유 | `resume_status`: 전 엔트리 0..27, 실스레드에서 3종 이상·CS 범위 도달·희소(카운터 배제)한 유일 오프셋(여럿이면 suspended 코드를 가진 것; 유휴로 2종뿐이면 유일 후보를 잠정 채택); CS 대기 큐 머리가 7..12 를 읽는지로 런타임 확인, 잠정이면 재프로빙 일정대로 다시 찾음(스레드 생멸 시 앞당김) (I-18) |

검증 실패는 `REFUSE(id)` 로 거부하거나 해당 정보만 강등한다(`layout:` 줄 끝에 강등 항목마다 ` [<이름> (<불변식 id>)]`, 트랜잭션 표 거부·상한 항목만 `: <사유>` 를 붙임, 덤프는 `degraded_features`).
불변식 원장 I-0~I-20 는 PR.md 에 있다.

## 4. 라이브 샘플링

- 100Hz 로 CS 배열(18 × 160B)을 pread. 대기자가 있으면 큐 홉도 읽는다(새 머리는 CS당 최대 20Hz, 같은 머리는 0.25초마다 재순회, 새 머리는 닻 재확인 8B 포함). 윈도우마다 occ%/wait% (샘플 비율), enter/s·wait/s (u32 wrap 보정 미분).
- 윈도우 중 목격된 writer 소유자(tran·thr)와 대기 큐 머리를 기억해 표시한다. 샘플마다 "누가 쥐고/기다리나"를 (cs, kind, tran, 클라이언트 pid) 단위로 세어(`ACT`) 요약 화면의 세션 블록과 판정에 쓴다; 대기자의 tran 은 목격 즉시 읽는다.
- 트랜잭션 슬롯은 재사용된다(엔진은 해제된 가장 낮은 슬롯부터 재배정). 그래서 목격한 클라이언트 tran(보유는 샘플마다, 대기 홉은 걷기 때)마다 그 슬롯 CLIENTIDS 의 pid 4B 를 tran 당 최대 0.05초에 1회 읽어 함께 남긴다(`client_pid` → `tran_client_pid`). 구간 끝 캡처는 목격한 (tran, pid) 가 지금 그 슬롯에 없으면 직전 캡처(= 구간 시작)의 신원을 프레임의 `gone` 목록(기록 `J` 줄)으로 옮기고, 렌더러는 `view_identity_seen` 으로 현재 클라이언트 대신 그 신원(`disconnected` 표시)을 보인다. 측정(같은 경합 부하, 동시 실행): read 시스템 호출 301 → 328회/s(+27), 판독량 약 305KB/s 그대로, 유휴는 100회/s 그대로.
- 스레드 `m_id`·tid 캐시는 1초에 1회 procfs 의 task 목록과 비교한다. 두 동작은 서로 독립이다.

  | 동작 | 조건 | 판독 |
  |---|---|---|
  | 표 재구성 | task 목록이 바뀐 초 | m_id 8B × 엔트리, tid 4B × 실스레드의 gather 2회(약 50KB) |
  | 이미지 재프로빙 | 미확정 필드(I-18 강한 판정·I-20, 빠른 회차 동안은 I-15~I-17 도)가 있고 일정이 된 때(스레드 변화와 무관) | 배열 이미지 1회(5.2MB), 표도 이 이미지로 재구성 |

  재프로빙 일정은 2초부터 두 배씩 늘려 64초에서 멈추고(2·4·8·16·32·64·64초, 7회), 그 뒤에도 미확정이면(유휴 서버) 5분마다 1회로 계속한다. 느린 단계에서 스레드 구성이 바뀌면 다음 1회를 1분 이내로 앞당긴다. 구조 필드(I-15~I-17)는 빠른 회차에서만 다시 찾고, 런타임 닻이 필드를 내리면 빠른 회차를 다시 시작한다. 변화가 없고 확정돼 있으면 이 단계의 대상 판독은 0. 소유자 조회는 그 캐시에서 찾는다.
- 터미널이면 stdin 을 raw 모드로 두고 샘플 사이에 키를 기다린다. `r` 은 현재 디렉터리에 `<pid>_<db>_<시각>.ctv` 기록을 열거나 닫는다(`--record` 로 연 파일은 건드리지 않음).
- 점유된 CS 의 writer 스레드는 샘플마다(스레드당 최대 20Hz, 구간당 64개까지) `/proc/<pid>/task/<tid>/stat` 로 R/D/기타 상태와 CPU 틱을 세고(`OS_THR`), 구간 끝에 로그 장치의 `diskstats` 차로 await·util 을 구한다. 대기 사유 히스토그램은 구간마다 실스레드의 `resume_status`·`m_status` 4B 로 만든다.
- 프로세스 THP 를 끈다(`prctl(PR_SET_THP_DISABLE)`) — 큰 판독 버퍼 할당이 커널 컴팩션을 기다리지 않게.
