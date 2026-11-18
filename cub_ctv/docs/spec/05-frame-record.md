# 프레임·기록 형식 명세 (Frame and Record Format Specification)

대상 모듈: `src/ctv_frame.c`(프레임 캡처와 렌더러용 질의), `src/ctv_rec.c`(기록 파일 쓰기·읽기·탐색 색인), 그리고 `src/ctv.h` 의 관련 형·상수. 이 문서만으로 두 모듈을 다시 작성해 같은 동작을 하고, 바이트 단위로 호환되는 기록 파일(`CTVREC 5`)을 쓰고 읽을 수 있어야 한다. 소스와 다르면 소스가 기준이다.

관련 문서: 샘플러(`LIVE_WIN` 채움)는 [06-display-run.md](06-display-run.md), 트랜잭션 신원 판독은 [03-transactions.md](03-transactions.md), 스레드 엔트리·대기 체인·대기 사유 집계는 [04-threads.md](04-threads.md), 풀 그룹 수집은 [02-cs-pool-dump.md](02-cs-pool-dump.md), FNV·텍스트 정화는 [01-base.md](01-base.md).

---

## 1. 개념 정의

| 용어 | 정의 |
|---|---|
| 프레임 (frame) | 화면 한 장을 그리는 데 필요한 모든 답을 한 순간에 담은 전역 구조체 `VIEW_FRAME view`. 렌더러(요약 화면·행·JSON·판정)는 대상 메모리를 읽지 않고 이것만 읽는다 |
| 스냅샷 프레임 | `-i` 없이 한 번 찍은 프레임. 창(window)이 없어 `view.st.samples == 0`, `dt == 0` |
| 라이브 프레임 | 라이브 실행에서 구간(window, 기본 `--interval`) 하나가 끝날 때 찍은 프레임. 샘플러가 모은 `WIN_STATS` 를 통째로 복사해 담는다 |
| 기록 (record) | 프레임을 줄 단위 텍스트로 이어 쓴 파일. 첫 줄 매직 `CTVREC 5`, 둘째 줄 헤더 `H`, 이후 프레임마다 `F` … `E` |
| 재생 (replay) | 기록에서 프레임을 하나씩 `view` 로 다시 채워 같은 렌더러로 그리는 것 |
| 신원 키프레임 | 그 프레임의 트랜잭션 신원(`I` 줄)을 전부 담은 프레임(`K 1`). 그 외 프레임(`K 0`)은 직전 프레임 대비 바뀐 신원(`I`)과 사라진 신원(`X`)만 담는다 |
| 칸 (slot) | 스레드 엔트리 배열의 0 기반 인덱스. 화면에는 `THR_INDEX(slot) = slot + 1` 로 표시된다 |

---

## 2. 용량 상수

| 상수 | 값 | 의미 |
|---|---|---|
| `MAX_CS` | 64 | CS 행 최대 수 (헤더 `count` 상한) |
| `NAME_MAX_LEN` | 64 | CS·풀 그룹 이름 버퍼(종료 NUL 포함) |
| `IDENT_LEN` | 64 | `TRAN_IDENT` 문자열 1개 버퍼 |
| `THR_WAITERS_MAX` | 64 | 대기 체인 1개의 최대 칸 수(순환 방지) |
| `THR_MAX` | 65536 | 스레드 칸 번호 상한(프레임 중복 표시 배열 크기) |
| `THR_RESUME_MAX` | 28 | `thread_resume_suspend_status` 코드 0..27, `R` 줄 코드 수 |
| `VIEW_MAX_TIDS` | 128 (`2*MAX_CS`) | 프레임의 소유자 tid → 칸 표 크기 |
| `VIEW_MAX_CHAINS` | 128 (`2*MAX_CS`) | 프레임의 대기 체인 수 |
| `VIEW_MAX_SLOTS` | 8192 (`VIEW_MAX_CHAINS*THR_WAITERS_MAX`) | 프레임의 칸 → tran 표 크기 |
| `ACT_MAX` | 4096 | 구간 활동 표(`ACT`) 크기 |
| `ACT_HELD_RESERVE` | 256 | `ACT_MAX` 중 보유(HELD) 목격 전용 예약분. 대기 목격은 3840칸까지만 |
| `ACT_HASH` | 16384 | 샘플러의 활동 해시 칸 수(2의 거듭제곱, `> 2*ACT_MAX`) |
| `ACT_HELD` / `ACT_WAIT` | 0 / 1 | 활동 종류 |
| `OS_MAX` | 64 | 프레임·구간당 OS 상태를 보관하는 스레드 수 |
| `OS_COMM_LEN` | 16 | 스레드 이름(`comm`) 버퍼 |
| `OS_SAMPLE_MIN_S` | 0.05 s | 스레드당 procfs 상태 샘플 최소 간격, 새 대기 큐 머리 걷기 최소 간격, 프레임의 체인 재사용 신선도 |
| `CHAIN_REWALK_S` | 0.25 s | 같은 큐 머리 재걷기 간격 |
| `POOL_MAX_GROUPS` | 1024 | 프레임의 풀 그룹 수 |
| `VERDICT_HISTORY` | 10 | 판정 지속성 창(이번 구간 + 이전 9 구간). `V` 줄 거리 1..9 |
| `HIST_CAP` | 4096 | (ctv_verdict.c) 판정 이력 원형 버퍼 크기 |
| `REC_MAGIC` | `"CTVREC 5"` | 기록 첫 줄 |
| `REC_KEYFRAME` | 60 | 기록의 N번째 프레임마다 신원 키프레임 |
| `REC_DT_MAX` | 1e7 s | 기록이 주장할 수 있는 최대 구간 길이 |
| `REC_SAMPLES_MAX` | 36010000 | 구간 샘플 수 상한 (= `--hz` 최대 10000 × (`--interval` 최대 3600 + 1초 여유)) |
| `INTERVAL_MIN` / `INTERVAL_MAX` | 0.1 / 3600.0 s | 라이브 헤더의 `interval` 허용 범위 |
| `REC_TOKENS_MAX` | 68 (`4 + THR_WAITERS_MAX`) | 프레임 줄 토큰 수 상한(가장 긴 줄: `Q head n slots…`) |
| 줄 버퍼 | 16384 바이트 | 읽기 시 `fgets` 버퍼(헤더·프레임·색인 공통) |
| 헤더 토큰 상한 | 16 | 헤더 토큰화 상한(정확히 13 이어야 함) |
| `POOL_GROUPS_CAPPED` / `POOL_NO_MEMORY` / `POOL_CHAIN_LOOP` | −2 / −3 / −4 | `pool_truncated` 특수값 |

---

## 3. 자료 구조

### 3.1 `VIEW_FRAME` (전역 `view`)

| 필드 | 형 | 의미 | 채우는 곳 (라이브/스냅샷 · 재생) |
|---|---|---|---|
| `ts` | `long` | 프레임 순간의 벽시계(`time(NULL)`, 초) | `view_capture`, `view_capture_trans` · `F` tok1 |
| `seq` | `long` | 프레임 서수. 라이브는 캡처마다 +1(첫 프레임 1), 재생은 `VERDICT_HISTORY + 기록 내 서수` | 캡처 함수 · `rd_F` |
| `count` | `int` | CS 행 수(`lo->count`) | `view_capture` · `F` tok2 |
| `rows[MAX_CS]` | `CS_ROW` | 구간 끝(또는 스냅샷 순간) CS 행 | `view_capture` 가 인자 `rows` 복사 · `C` |
| `dt` | `double` | 구간 실제 길이(s). 스냅샷 0 | 캡처 인자 · `F` tok4 |
| `st` | `WIN_STATS` | 구간이 본 것 전부(§3.2). 스냅샷은 `win_stats_clear` 상태 | `view_capture` 가 `w->st` 통째 복사 · `F`/`C`/`P`/`W`/`A` |
| `prev[MAX_CS]` | `CS_ROW` | 구간 시작 행. 현재 행과 같되 `nenter`·`nwait`·`total_ms` 3개만 구간 시작 값(스냅샷은 0) | `view_capture` · `C` tok19,20,22 |
| `tran_avail` | `int` | 트랜잭션 표 가용: `tran_geom.all_tdes != 0 && !(degraded & DEG_TRAN)` | `view_capture_idents` · `F` tok5 |
| `cid_locked` | `int` | CLIENTIDS 문자열 지오메트리 확정(`tran_geom.cid_stride != 0`) | 같음 · `F` tok6 |
| `has_trid` | `int` | trid 검증(I-8) 통과 | 같음 · `F` tok7 |
| `ntran` | `int` | 트랜잭션 디스크립터 수(가용 아니면 0) | 같음 · `F` tok8 |
| `ident` | `VIEW_IDENT *` | 신원 배열, `idx` 오름차순(이진 탐색 전제). 힙, 용량 64 에서 2배씩 증가 | `view_add_ident` · `I` + 키프레임 병합 |
| `nident`, `ident_cap` | `int` | 개수, 용량 | 같음 |
| `gone` | `VIEW_IDENT *` | 구간에 목격됐으나 프레임 순간에는 그 슬롯을 떠난 클라이언트(`idx` = tran, `id.cpid` = 목격 pid). 직전 캡처의 신원 그대로. 순서 = 발견 순(정렬 없음). 힙, 용량 16 에서 2배씩 | `view_capture_gone`(§4.1a) · `J` |
| `ngone`, `gone_cap` | `int` | 개수, 용량 | 같음. `view_capture_trans`·`rd_F` 가 `ngone = 0` |
| `tid[VIEW_MAX_TIDS]`, `tid_slot[]`, `ntid` | `uint64_t`, `int` | 소유자 스레드 id(pthread_t) → 칸(모르면 −1) | `view_add_tid` · `T` |
| `chain[VIEW_MAX_CHAINS]`, `nchain` | `VIEW_CHAIN` | 현재 대기 큐 머리별 체인 | `view_add_chain` · `Q` |
| `slot[VIEW_MAX_SLOTS]`, `slot_tran[]`, `slot_cpid[]`, `nslot` | `int` | 칸 → 그 칸이 기다릴 때의 tran index 와 그 tran 의 클라이언트 pid(−1 모름) | `view_add_slot_tran` · `S` |
| `os[OS_MAX]`, `nos` | `OS_THR` | 스레드 OS 상태(샘플된 writer 스레드 먼저, 그다음 소유자 이름만) | `view_add_os` · `O` |
| `disk_avail` | `int` | 로그 장치 수치 있음 | `view_disk_end` · `D` 유무 |
| `disk_await_ms`, `disk_util_pct` | `double` | 쓰기 1건 평균 대기 ms, 장치 사용률 % | 같음 · `D` tok1,2 |
| `disk_dev[32]` | `char` | 로그 장치 이름 | 같음 · `D` tok3 |
| `wait_avail` | `int` | 대기 사유 집계 있음(`thr_wait_counts == 0`) | `view_capture` · `R` 유무 |
| `wait_live` | `int` | 읽은 살아 있는 스레드 엔트리 수 | 같음 · `R` tok1 |
| `wait_cnt[THR_RESUME_MAX]` | `int` | 코드별 대기 스레드 수. `[7]` = CS 배열 `wr`(읽기 대기자) 합 | 같음 · `R` tok3.. |
| `pool_state` | `int` | 1 수집됨, 0 수집 안 함, −1 검증 실패 | `view_capture`(0), `view_capture_pool` · `F` tok9 |
| `npool`, `pool[POOL_MAX_GROUPS]` | `int`, `POOL_GROUP` | 풀 그룹 | `view_capture_pool`(또는 디테일 덤프의 풀 걷기) · `G` |
| `pool_truncated` | `int` | −1 완전, ≥0 체인이 끊긴 청크 번호, −2/−3/−4 §2 | 같음 · `F` tok10 |
| `degraded` | `unsigned` | 이 순간의 강등 비트(`DEG_*`) | 캡처 끝에 전역 `degraded` 복사 · `B` |
| `hist_known` | `int` | 이전 구간 판정 이력이 있음(요약 화면이 이력을 쌓는 실행) | `view_capture` · `V` 유무 |
| `hist_n` | `int` | 이력 항목 수(0..9) | 같음 · `V` tok1 |
| `hist_back[VERDICT_HISTORY]` | `int` | 몇 구간 전인지(1..9) | 같음 · `V` 쌍 첫째 |
| `hist_top[VERDICT_HISTORY]` | `int` | 그 구간의 최상위 CS 인덱스, −1 = 경합 없음 | 같음 · `V` 쌍 둘째 |

`view` 는 정적 전역이며 크기가 크다(`st.act` 만 4096×24 B). 프레임 사이에 해제하지 않는다.

### 3.2 `WIN_STATS` (구간이 본 것)

샘플러의 `LIVE_WIN.st` 와 프레임의 `view.st` 가 같은 형을 쓰고, 프레임은 통째로 복사한다.

| 필드 | 형 | 의미 | 샘플러에서의 갱신 규칙 |
|---|---|---|---|
| `samples` | `int` | 구간의 샘플 수(구간을 닫는 샘플 포함) | `live_accumulate` 끝에 +1 |
| `occ[MAX_CS]` | `int` | `rwlock != 0` 이었던 샘플 수 | 샘플마다 |
| `wai[MAX_CS]` | `int` | `wr != 0 \|\| ww != 0` 이었던 샘플 수 | 샘플마다 |
| `seen_tran[MAX_CS]` | `int` | 구간 중 목격한 보유자(writer 또는 강등된 owner, 06 §8.1 `row_held`)의 tran. −1 = 없음 | `row_held` 일 때: tran<0 이면 0(엔진 스레드)로 보고, `tran >= 1` 이면 항상 덮어쓰고, `tran == 0` 은 아직 −1 일 때만 기록(클라이언트 tran 우선) |
| `seen_cpid[MAX_CS]` | `int` | `seen_tran` 의 클라이언트 pid(그 샘플에서 `client_pid(tran)`, 06 §11.2a). −1 = 모름 | `seen_tran` 과 같은 순간에 |
| `seen_owner[MAX_CS]` | `uint64_t` | 위 `seen_tran` 을 기록한 샘플의 owner thread id | `seen_tran` 과 같은 순간에 |
| `seen_wq[MAX_CS]` | `uint64_t` | 구간 최초의(비어 있지 않게 걸어진) 대기 큐 머리. 0 = 없음 | 큐를 걸었고(`ch_n > 0`) `seen_wq == 0` 또는 같은 머리일 때 설정 |
| `seen_n[MAX_CS]` | `int` | 그 머리의 체인 칸 수(같은 머리를 다시 걸으면 최신 걷기로 갱신). −1 = 없음 | 위와 같은 조건 |
| `seen_slot[MAX_CS][64]` | `int` | 그 체인의 칸들 | 위와 같은 조건 |
| `seen_tran_of[MAX_CS][64]` | `int` | 그 체인 각 칸의 tran(링크와 같은 읽기에서) | 위와 같은 조건. **기록되지 않는다**(재생은 `S` 줄로 대신) |
| `seen_cpid_of[MAX_CS][64]` | `int` | 각 칸 tran 의 클라이언트 pid(걷기 직후 `client_pid`) | 위와 같은 조건(`ch_cpid` 복사). **기록되지 않는다**(`S` tok3 으로 대신) |
| `act[ACT_MAX]` | `ACT` | 누가 어떤 CS 를 보유/대기했는지와 목격 샘플 수 | `act_add` |
| `nact` | `int` | `act` 사용 수 | 같음 |
| `act_dropped` | `int` | 표가 차서 보관하지 못한 목격 수 | 같음 |

`win_stats_clear(st)`:
1. `st` 의 시작부터 `act` 직전까지 0 으로 채운다(`samples`, `occ`, `wai`, `seen_*` 모두).
2. 모든 CS `i` 에 `seen_tran[i] = -1`, `seen_cpid[i] = -1`, `seen_n[i] = -1`.
3. `nact = act_dropped = 0`. `act[]` 배열 내용은 지우지 않는다(`nact` 만 의미).

### 3.3 `ACT`

| 필드 | 의미 |
|---|---|
| `cs` | CS 인덱스 |
| `kind` | `ACT_HELD`(0) 보유(writer, 또는 강등되어 reader 로 쥔 owner — 06 §8.1 `row_held`) / `ACT_WAIT`(1) 대기 큐에 있음 |
| `tran` | tran index. 0 = 시스템(엔진 스레드 포함: writer 의 tran 이 음수면 0 으로 셈) |
| `thr` | 칸. 모르면 −1. `tran != 0` 항목은 처음 알려진 칸으로 채워짐 |
| `samples` | 목격 샘플 수 |
| `cpid` | 목격 당시 그 tran 슬롯의 클라이언트 pid(`client_pid`, 06 §11.2a). −1 = 모름(tran 0 포함) |

동일 항목 판정(샘플러 `act_add`): `(cs, kind, tran, cpid)` 가 같고, `tran <= 0` 이면 `thr` 까지 같아야 같은 항목(엔진 스레드(0)와 tran 미상(−1)은 스레드별로 나눔 — 한 샘플에 한 항목이 두 번 세어지지 않으므로 항목의 `samples` 는 구간 `samples` 를 넘지 않는다). 같은 슬롯이라도 구간 중 클라이언트가 바뀌면(pid 다름) 다른 항목이다. 해시 `h = (cs*31 + kind*7 + tran*2654435761 + (key_thr+1)*40503 + (cpid+1)*97) & (ACT_HASH-1)`(unsigned 산술, `key_thr = tran<=0 ? thr : -1`), 선형 탐사. 새 항목은 `kind == ACT_HELD` 면 `nact < ACT_MAX`, 대기면 `nact < ACT_MAX - ACT_HELD_RESERVE` 일 때만 추가, 아니면 `act_dropped++`. 항목 순서 = 최초 목격 순서이며 이것이 `W` 줄 순서다.

### 3.4 `OS_THR`

| 필드 | 형 | 의미 |
|---|---|---|
| `slot` | `int` | 스레드 칸 |
| `tid` | `int` | OS tid(LWP) |
| `samples` | `int` | procfs 상태를 읽은 횟수 |
| `run` / `dsk` / `oth` | `int` | 상태 `R` / `D` / 그 외 샘플 수 |
| `ticks0` / `ticks1` | `long` | 첫/마지막 샘플의 CPU 틱(utime+stime) |
| `t_first` / `t_last` | `double` | 첫/마지막 샘플 단조 시각(s). **기록되지 않음**(재생 시 0) |
| `span_ms` | `int` | `t_last − t_first` 를 ms 로 반올림(`(now − t_first)*1000 + 0.5`). 틱 차이가 덮는 시간 |
| `comm[16]` | `char` | `/proc/<pid>/task/<tid>/comm` |

샘플러는 writer 스레드(보유 중인 CS 의 owner 칸)만 샘플한다. 스레드당 `OS_SAMPLE_MIN_S` 안에 다시 오면 건너뛰며, 읽기에 실패해도 `t_last` 는 갱신된다(실패도 간격 제한).

### 3.5 `VIEW_IDENT`, `TRAN_IDENT`, `VIEW_CHAIN`, `POOL_GROUP`

| 형 | 필드 | 의미 |
|---|---|---|
| `VIEW_IDENT` | `int idx` | tran index |
| | `int32_t trid` | 트랜잭션 id(검증 안 됐으면 0) |
| | `TRAN_IDENT id` | 신원 |
| `TRAN_IDENT` | `info, user, program, login, host, ip` | 각 `char[64]`, CLIENTIDS 문자열(client_info, db_user, program_name, login_name, host_name, client_ip_addr) |
| | `int cpid` | 클라이언트 프로세스 id |
| `VIEW_CHAIN` | `uint64_t head` | 대기 큐 머리(스레드 엔트리 주소) |
| | `int n` | 칸 수(≤64) |
| | `int slot[64]` | 큐 순서의 칸들 |
| `POOL_GROUP` | `int type` | sync stats 종류(1 CSECT … 4 MUTEX 범위) |
| | `char name[64]` | 이름 |
| | `int cnt` | 그 (type, name) 의 엔트리 수 |
| | `uint64_t nenter` | nenter 합 |
| | `double max_ms` | max_ms 최대 |

### 3.6 `VIEW_HDR` (기록 헤더)

| 필드 | 의미 | `H` 토큰 |
|---|---|---|
| `pid` | 대상 cub_server pid | 1 |
| `live` | 1 = 라이브 기록(구간 프레임), 0 = 스냅샷 | 2 |
| `hz` | 샘플 주파수(`--hz`) | 3 |
| `interval` | 구간 길이(`--interval`, s) | 4 |
| `thr_count` | 스레드 엔트리 수(`thr_geom.count`) | 5 |
| `degraded` | 강등 비트. `--record` 는 시작 시점, `[r]` 은 녹화를 연 시점 값 | 6 |
| `lo.stride`, `lo.count`, `lo.off_stats` | CS 배열 레이아웃 3개 | 7, 8, 9 |
| `db[128]` | DB 이름 | 10 |
| `lib[4096]` | 매핑된 libcubrid 경로 | 11 |
| `tran_refused[64]` | 트랜잭션 기능 강등 사유(없으면 빈 문자열) | 12 |

`CS_LAYOUT` 의 나머지 오프셋(`off_tran`, `off_rwlock`, `off_owner`, `off_wq`, `off_pq`)은 기록하지 않는다. 재생은 대상 메모리를 읽지 않으므로 필요 없고, 읽은 헤더에서는 0 이다.

### 3.7 `CS_ROW` (CS 한 행)

`cs_read_rows`/`cs_rows_from_blob` 가 채우고, 프레임(`view.rows`, `view.prev`)과 샘플러(`LIVE_WIN.prev`/`cur`)가 같은 형을 쓴다. 필드 순서는 선언 순서다.

| 필드 | 형 | 의미 | `C` 토큰 |
|---|---|---|---|
| `name[NAME_MAX_LEN]` | `char[64]` | CS 이름 | 21 |
| `rwlock` | `int32_t` | >0 읽기 보유 수, −1 writer, 0 비어 있음 | 2 |
| `wr`, `ww` | `uint32_t` | 대기 reader 수, 대기 writer 수 | 3, 4 |
| `tran` | `int32_t` | 보유자의 tran index(`row_held` 일 때만 의미: writer, 또는 강등 중 남은 값) | 5 |
| `owner` | `uint64_t` | 보유 스레드 id(pthread_t), 없으면 0(`row_held` 일 때만 의미) | 6 |
| `wq` | `uint64_t` | 첫 대기 큐 머리(promoters 큐가 있으면 그것, 아니면 writers 큐), 없으면 0 | 7 |
| `wq2` | `uint64_t` | promoters 가 앞에 있을 때의 writers 큐(라이브 전용). **기록되지 않음** → 재생에서는 `rd_F` 의 `memset` 으로 항상 0 | — |
| `nenter`, `nwait`, `nreenter` | `uint32_t` | 통계 블록 카운터 | 9, 10, 11 |
| `total_ms`, `max_ms` | `double` | 통계 블록 누적·최대 대기 ms | 12, 13 |
| `has_stats` | `int` | 통계 블록 검증 통과(0/1) | 8 |

재생에서 `rd_F` 가 `view.rows` 전체를 0 으로 채우고 `C` 가 `wq2` 를 뺀 모든 필드를 덮으므로, 재생 프레임에서 0 으로 남는 필드는 `wq2` 하나다.

### 3.8 `LIVE_WIN` (샘플러 구간 상태)

라이브 샘플러(06 §11) 전용. 렌더러는 이것을 읽지 않고 `view_capture` 가 필요한 것만 프레임으로 옮긴다. `live_win_new` 가 `calloc` 하므로 모든 필드의 초기값은 0 이다.

| 필드 | 형 | 의미 | 리셋(`live_win_reset`) |
|---|---|---|---|
| `prev[MAX_CS]` | `CS_ROW` | 구간 시작 행(직전 구간의 `cur`, 첫 구간은 `run_start` 의 `cs_read_rows`) | 안 함(구간 닫기에서 `prev = cur` 복사) |
| `cur[MAX_CS]` | `CS_ROW` | 구간 끝 행(`cs_rows_from_blob` 결과) | 안 함 |
| `st` | `WIN_STATS` | 구간이 본 것(§3.2). 프레임이 통째 복사 | `win_stats_clear` |
| `wq_last[MAX_CS]` | `uint64_t` | 직전 샘플에서 걸은 큐 머리(걷기 실패면 0) | 0 |
| `ch_n[MAX_CS]` | `int` | 그 머리 아래 체인 칸 수 | 0 |
| `ch_slot[MAX_CS][64]`, `ch_tran[MAX_CS][64]`, `ch_cpid[MAX_CS][64]` | `int` | 체인 칸, 각 칸의 tran, 걷기 직후 읽은 그 tran 의 클라이언트 pid | 안 함(`ch_n` 만 의미) |
| `ch_walked[MAX_CS]` | `double` | 마지막 걷기 시각(mono s). `every()` 스로틀 기준 | **안 함**(구간을 넘어 유지) |
| `own_last[MAX_CS]` | `uint64_t` | 직전 샘플의 owner tid(칸 해석 캐시 키) | 0 |
| `own_slot[MAX_CS]` | `int` | 그 tid 의 칸(−1 = 모름) | −1 |
| `blob` | `unsigned char *` | 최신 샘플의 CS 배열 원시 이미지(`stride*count` 바이트, `malloc`) | 안 함 |
| `act_ix[ACT_HASH]` | `unsigned short` | 활동 해시: `st.act` 인덱스 + 1, 0 = 빈칸 | 전부 0 |
| `os[OS_MAX]`, `nos` | `OS_THR`, `int` | 구간에 샘플한 writer 스레드 OS 상태 | `nos = 0` |
| `dk0[3]` | `uint64_t` | 구간 시작 diskstats(`wr_ios`, `wr_ms`, `io_ms`) | `view_disk_begin` 이 채움 |
| `dk_ok` | `int` | `dk0` 유효 | 같음 |
| `no_disk` | `int` | 디스크 수치를 쓰지 않는 실행(`!sampled`). 1 이면 `view_disk_begin` 을 부르지 않음 | 안 함(생성 직후 1회 설정) |
| `cs_base` | `uint64_t` | 대상의 CS 배열 주소(큐 머리 재확인용) | 안 함 |

---

## 4. 프레임 캡처

### 4.1 `view_capture(lo, rows, w, dt)` — 스냅샷(`w == NULL`) 또는 라이브 구간 끝

호출: 스냅샷은 `cs_read_rows` 직후 `view_capture(&lo, rows, NULL, 0.0)`. 라이브는 구간을 닫을 때 마지막 샘플 이미지에서 만든 행으로 `view_capture(lo, w->cur, w, now − t_prev)`.

단계(순서가 결과에 영향을 준다):

1. `tran_geom_refresh()`, `thr_cache_refresh()` — 각자 내부에서 초당 1회로 제한되는 지오메트리·스레드 캐시 갱신.
2. `view.ts = time(NULL)`, `view.seq++`.
3. 판정 이력 내보내기: `view.hist_n = history_export(view.seq, hist_back, hist_top)`. 반환이 −1(이 프로세스에서 요약 화면이 아직 이력을 쌓지 않음: 파이프·`--json`·`--trans`·첫 프레임)이면 `hist_known = 0`, `hist_n = 0`. 아니면 `hist_known = 1`. 내보내는 항목은 `b = 1..9` 에 대해 `seq − b` 의 이력이 있는 것만, `b` 오름차순(§6.6).
4. `view.count = lo->count`, `rows` 를 `view.rows` 로 복사, `view.dt = dt`.
5. `w != NULL` 이면 `view.st = w->st`(구간 통계 통째 복사), 아니면 `win_stats_clear(&view.st)`.
6. 각 CS `i`: `view.prev[i] = rows[i]` 후 `nenter`, `nwait`, `total_ms` 만 `w->prev[i]` 의 값(스냅샷은 0, 0, 0.0)으로 바꾼다. **실시간 판독 없음**(복사만).
7. `view_capture_idents()` (§4.3) — **실시간 판독**(TDES·CLIENTIDS).
8. 직전 프레임이 표시한 칸 표시 해제: `i < view.nslot` 에 대해 `slot_seen[view.slot[i]] = 0`. 그다음 `ntid = nchain = nslot = 0`.
9. 각 CS `i` 에 대해 차례로:
   1. `row_held(&rows[i])` 이면 `view_add_tid(rows[i].owner)` (아니면 owner 필드는 0 이거나 쓰이지 않으므로 추가하지 않음).
   2. `view_add_tid(view.st.seen_owner[i])`.
   3. `view_add_chain(rows[i].wq, rows[i].wq2, w, i)`.
10. 구간 중 이전 목격: 각 CS `i`, `k < view.st.seen_n[i]` 에 대해 `view_add_slot_tran(seen_slot[i][k], seen_tran_of[i][k], seen_cpid_of[i][k])`. 이미 현재 큐(9단계)에서 추가된 칸은 무시되므로 **현재 큐의 tran 이 우선**한다.
10a. `view_capture_gone()` (§4.1a) — 판독 없음(7단계 신원과 직전 캡처 신원 비교).
11. OS 측: `view.nos = 0`. 먼저 `w->os[0..w->nos)` 를 순서대로 `view_add_os(w->os[i].slot, &w->os[i])`(샘플러 기록 복사), 그다음 `view.tid_slot[0..ntid)` 를 순서대로 `view_add_os(tid_slot[i], NULL)`(이름만 실시간 판독).
12. 대기 사유: `readers = Σ rows[i].wr`(i < count). `view.wait_avail = thr_wait_counts(view.wait_cnt, &view.wait_live, readers) == 0` — **실시간 판독**(스레드 엔트리 `resume_status`/`m_status`). `wait_cnt[7]` 은 `readers` 로 채워진다.
13. `view_disk_end(w, dt)` (§4.5) — **실시간 판독**(`/proc/diskstats`).
14. `view.pool_state = 0`. `npool`·`pool[]` 은 건드리지 않는다(이후 `view_capture_pool` 이 부르면 채움; 기록은 `pool_state == 1` 일 때만 `G` 를 씀).
15. `view.degraded = degraded` — 이번 캡처의 모든 프로브가 끝난 뒤의 전역 비트.

보조 함수 규칙:

| 함수 | 규칙 |
|---|---|
| `view_add_tid(tid)` | `tid == 0` 또는 `ntid >= 128` 이면 무시. 이미 있으면 무시. 아니면 `tid[ntid] = tid`, `tid_slot[ntid] = thr_index_of_tid_mem(tid)`(실시간 tid 정렬표 이진 탐색, 없으면 −1), `ntid++` |
| `view_add_chain(head, head2, w, cs)` | `head == 0` 또는 `nchain >= 128` 이면 무시. 같은 `head` 체인이 이미 있으면 무시(먼저 나온 CS 가 이김). 새 체인 `c` 를 추가하고 `c->head = head`. **재사용 조건** `w != NULL && w->wq_last[cs] == head && mono_s() − w->ch_walked[cs] < OS_SAMPLE_MIN_S` 이면 샘플러 걷기(`ch_n`, `ch_slot`, `ch_tran`, `ch_cpid`)를 복사, 아니면 `thr_waiters_mem2(head, head2, c->slot, tran, 64, 0)` 로 새로 걷고(앵커 0 = 불변식 표결 안 함) 모든 칸 `cpid = -1`(이번 캡처 신원이 곧 현재 클라이언트). 이어서 모든 칸 `k` 에 `view_add_slot_tran(c->slot[k], tran[k], cpid[k])` |
| `view_add_slot_tran(slot, tran, cpid)` | `nslot >= 8192`, `slot < 0`, `slot >= THR_MAX`, 또는 이번 프레임에 이미 표시된 칸(`slot_seen[slot]`)이면 무시. 아니면 표시하고 `slot[nslot] = slot`, `slot_tran[nslot] = tran`, `slot_cpid[nslot] = cpid`, `nslot++` |
| `view_add_os(slot, src)` | `slot < 0` 또는 `nos >= 64` 면 무시. 같은 칸이 이미 있으면 무시. `src != NULL` 이면 통째 복사. 아니면 0 으로 채우고 `slot`, `tid = thr_tid_of(slot)`, `tid > 0` 이면 `os_task_comm` 으로 `comm`. **`tid > 0` 인 경우에만** `nos++`(확정) |

`thr_waiters_mem2` 는 첫 큐(`head`)를 걷고, `head2 != 0` 이고 자리가 남으면 둘째 큐를 이어 걷되 첫 부분에 이미 있는 칸을 만나면 거기서 둘째 부분을 끊는다(찢긴 읽기 방지). 자세한 것은 04 문서.

### 4.1a `view_capture_gone()` — 슬롯을 떠난 클라이언트

슬롯은 재사용된다(엔진은 해제된 가장 낮은 슬롯부터 다시 준다: `log_tran_table.c` `hint_free_index`). 구간에 목격한 (tran, cpid) 가 프레임 순간에는 그 슬롯에 없으면, 그 클라이언트의 신원을 **직전 캡처의 신원**(모듈 정적 `prev_ident[0..prev_n)`, = 구간 시작 시점 신원)에서 찾아 `view.gone` 에 남긴다.

1. `ngone = 0`.
2. 차례로 `gone_note(tran, cpid)`: `st.act[0..nact)` 의 `(tran, cpid)`, CS `i < count` 의 `(seen_tran[i], seen_cpid[i])`, `k < nslot` 의 `(slot_tran[k], slot_cpid[k])`.
3. `gone_note(tran, cpid)`: `tran < 1`, `cpid < 0`, 또는 `view_identity(tran)` 가 있고 그 `id.cpid == cpid`(현재 클라이언트)면 무시. `gone` 에 같은 `(idx, id.cpid)` 가 이미 있으면 무시. `prev_ident` 에서 `idx == tran && id.cpid == cpid` 인 첫 항목이 있으면 `view_add_gone(tran, trid, &id)`. 없으면 아무것도 남기지 않는다(렌더러는 pid 만 보임, `view_identity_seen` 2).
4. 끝으로 이번 `view.ident` 를 `prev_ident` 로 복사(`prev_n = nident`; 용량은 `nident` 로 `realloc`, 실패하면 `prev_n = 0` 후 반환) — 다음 구간의 시작 신원.

`view_add_gone(idx, trid, id)`: `ngone == gone_cap` 이면 용량 `gone_cap ? 2*gone_cap : 16` 으로 `realloc`(실패 시 "out of memory" 로 종료), 끝에 추가. 재생은 `J` 줄이 부른다.

한계: 라이브 실행의 첫 구간은 `prev_ident` 가 비어 신원이 없다. 한 구간 안에 접속했다 떠난 클라이언트도 직전 캡처에 없으므로 신원이 없다(둘 다 pid 만).

### 4.2 `view_capture_trans(dt)` — 트랜잭션 표만

`--trans` 이고 기록이 없을 때(`TRANS_TABLE_ONLY`) 쓰는 경량 캡처. CS 행·스레드·큐·OS·디스크·이력은 읽지도 바꾸지도 않는다.

1. `tran_geom_refresh()`.
2. `view.ts = time(NULL)`, `view.seq++`.
3. `view.st.samples = 0`, `view.dt = dt`.
4. `view_capture_idents()`.
5. `view.ngone = 0`.
6. `view.degraded = degraded`.

이 경로는 기록이 없을 때만 쓰이므로(`[r]` 도 이 모드에서는 거부) 이 프레임이 기록 파일에 쓰이는 일은 없다.

### 4.3 `view_capture_idents()`

1. `tran_avail = (tran_geom.all_tdes != 0) && !(degraded & DEG_TRAN)`.
2. `cid_locked = (tran_geom.cid_stride != 0)`, `has_trid = tran_geom.has_trid`.
3. `ntran = tran_avail ? tran_geom.ntran : 0`, `nident = 0`.
4. `tran_identity_prefetch()`(전 디스크립터 묶음 판독).
5. `t = 0 .. ntran−1` 오름차순으로 `tran_identity_cached(t, &id, &trid) == 0` 이면 `view_add_ident(t, trid, &id)`. 실패(지오메트리 미확정, 해제된 슬롯 trid == −1, 판독 실패)는 건너뜀. 결과는 `idx` 오름차순.

`view_add_ident(idx, trid, id)`: `nident == ident_cap` 이면 용량을 `ident_cap ? 2*ident_cap : 64` 로 `realloc`(실패 시 "out of memory" 로 종료), 끝에 추가.

### 4.4 `view_capture_pool(pool_addr)`

1. `npool = 0`, `pool_truncated = -1`.
2. `pool_state = pool_collect(pool_addr, view.pool, POOL_MAX_GROUPS, &npool, &pool_truncated) == 0 ? 1 : -1`.
3. `view.degraded = degraded`.

스냅샷에서 풀 심볼이 있고, 기록하거나 일반 스냅샷 표(비 `--trans`, 비 `--json`, 비 `--detail`)를 보일 때만 부른다. `--dump` 는 대신 디테일 덤프의 풀 걷기가 `view.pool`/`npool`/`pool_truncated` 를 채우고 `pool_state = 1`(실패 시 −1)로 둔다. 라이브 프레임에서는 부르지 않으므로 라이브 기록의 `F` 는 `pool_state 0`, `pool_truncated` 는 마지막 값(초기 0)이다.

### 4.4a `win_stats_clear(st)`

§3.2 참조. 스냅샷 캡처와 샘플러 구간 초기화(`live_win_reset`)가 부른다.

### 4.5 로그 장치 수치 (diskstats 이월과 역행 규칙)

모듈 상태: `dk_state`(−1 미프로브, 0 없음, 1 알려짐), `dk_major`, `dk_minor`, `dk_name[32]`, `dk_end[3]`, `dk_carry`.

`disk_probe()`: `dk_state < 0` 일 때 한 번만 `os_log_device(target_pid, …)` 로 로그 볼륨의 장치를 찾고(성공 1, 실패 0), 이후 캐시값 반환.

`view_disk_begin(w)` — 구간 시작(`live_win_reset`, 단 `w->no_disk` 면 호출 안 함):
1. `dk_carry` 가 서 있으면 `w->dk0 = dk_end`, `w->dk_ok = 1`, `dk_carry = 0` 후 반환(같은 순간을 두 번 읽지 않음: 직전 구간 끝값이 이번 시작값).
2. 아니면 `w->dk_ok = (disk_probe() == 1) && os_diskstats(major, minor, &dk0[0], &dk0[1], &dk0[2]) == 0`.

`dk0`/`v` 의 세 값 = (쓰기 완료 수 `wr_ios`, 쓰기 소요 ms `wr_ms`, I/O 소요 ms `io_ms`).

`view_disk_end(w, dt)` — `view_capture` 13단계:
1. `disk_avail = 0`, `disk_dev = ""`.
2. `w == NULL`, `!w->dk_ok`, `dt <= 0`, 또는 diskstats 판독 실패면 반환(이월 없음).
3. `dk_end = v`, `dk_carry = 1` — 아래 역행 검사보다 **먼저** 저장한다(역행해도 다음 구간의 시작값은 이번 끝값).
4. 세 값 중 하나라도 `v[k] < dk0[k]`(장치 재등록 등으로 카운터가 되돌아감)면 수치 없이 반환.
5. `disk_avail = 1`; `disk_await_ms = (v[0] > dk0[0]) ? (v[1]−dk0[1]) / (v[0]−dk0[0]) : 0.0`; `disk_util_pct = (v[2]−dk0[2]) / (dt * 10.0)`(ms/(dt·1000)·100); `disk_dev = dk_name`.

### 4.6 대기 사유 집계(R 의 출처)

`thr_wait_counts(cnt, &live, reader_cap)`: `cnt[0..27] = 0`, `live = 0`. `resume_status` 미검증이거나 판독 실패면 −1(→ `wait_avail = 0`). 아니면 tid 가 있고 판독이 성공한 엔트리마다 `live++`, 코드 `v`(0..27)가 7 이 아니고 현재 중단 상태(`m_status` 검증 시 `TS_WAIT` 이고 중단 코드, 미검증 시 중단 코드)면 `cnt[v]++`. 마지막에 `cnt[7] = reader_cap`. 상세는 04 문서.

---

## 5. 렌더러용 질의

모두 `view` 만 읽는다(대상 판독 없음). 라이브와 재생에서 같은 답을 준다.

| 함수 | 계약 |
|---|---|
| `view_os_of(slot)` | `view.os[0..nos)` 에서 `slot` 이 같은 첫 항목 포인터, 없으면 `NULL` |
| `resume_name(v)` | 코드 → 대기 사유 이름(언어 `use_ko` 반영): 3 page latch/페이지 래치, 5 job queue/작업 큐, 7·9·11 critical section/크리티컬 섹션, 13 connection queue/접속 큐, 15 class repr/클래스 표현, 17 lock/락, 19 log writer/로그 writer, 21 free buffer/빈 버퍼, 23 DWB queue/DWB 큐, 25 sleep/sleep, 26 concurrency slot/동시성 슬롯, 그 외 `""` |
| `view_identity(tran, &id, &trid)` | `nident == 0` 이면 −1. `view.ident` 를 `idx` 로 이진 탐색(비교는 뺄셈 없이 `<`/`>`: 재생 값은 임의 int). 찾으면 `*id` 복사, `trid != NULL` 이면 `*trid`, 0 반환. 없으면 −1. **전제: `ident` 는 `idx` 오름차순** |
| `view_identity_seen(tran, cpid, &id, &trid)` | (tran, cpid) 로 목격된 클라이언트에 보일 신원. `view_identity(tran)` 가 있고 `cpid < 0 \|\| id.cpid == cpid` 면 0(현재 클라이언트, 또는 pid 모름). 아니면 `tran < 1 \|\| cpid < 0` 이면 −1(없음). `gone` 에서 `idx == tran && id.cpid == cpid` 를 찾으면 그 신원·trid 로 1(떠난 클라이언트). 없으면 `*id` 를 0 으로 채우고 `id->cpid = cpid` 로 2(떠났고 신원 없음, pid 만). 2 일 때 `trid` 는 쓰지 않는다 |
| `view_slot_of_tid(tid)` | `tid != 0` 이고 `view.tid[k] == tid` 인 첫 `k` 의 `tid_slot[k]`, 없으면 −1 |
| `view_tran_of(slot)` | `view.slot[k] == slot` 인 첫 `k` 의 `slot_tran[k]`, 없으면 −1 |
| `view_tran_cpid_of(slot)` | 같은 `k` 의 `slot_cpid[k]`, 없으면 −1 |
| `view_waiters(head, idx, max)` | `head != 0` 이고 `chain[k].head == head` 인 첫 체인의 칸을 `min(n, max)` 개 `idx` 에 복사하고 그 수 반환, 없으면 0 |
| `view_seen_chain(cs, idx, max)` | `cs` 가 `[0, count)` 밖이거나 `st.seen_n[cs] < 0` 이면 0. 아니면 `st.seen_slot[cs]` 를 `min(seen_n, max)` 개 복사하고 그 수 반환(구간 중 그 CS 큐 머리를 처음 본 때의 체인) |

---

## 6. 기록 형식 `CTVREC 5`

### 6.1 줄·토큰 규칙

| 규칙 | 내용 |
|---|---|
| 줄 | `\n` 으로 끝나는 텍스트 1줄 = 레코드 1개. 쓰기는 항상 `\n`(CR 없음) |
| 토큰 | 단일 공백 `' '` 으로 구분. 읽기는 `strtok_r(" ")` 이므로 연속 공백은 하나로, 앞뒤 공백은 무시. 탭은 구분자가 아니다. 읽을 때 첫 `\n` 에서 줄을 자른다. `\r` 은 매직 줄에서만 제거되고 다른 줄에서는 마지막 토큰의 일부가 된다 |
| 빈 줄 | 토큰 0개 줄은 건너뛴다(프레임 안팎 모두, 색인도 무시) |
| 정수 | 쓰기 `%d`/`%u`/`%ld`/`%llu`(10진). 읽기 `atoi`/`atol`/`strtoul(…,10)`/`strtoull(…,10)` — **문법 검증 없음**: 숫자가 아닌 토큰은 0, `strtoul("-1")` 은 `UINT32` 로 잘려 `0xFFFFFFFF` |
| 16진 | 쓰기 `%llx`(소문자, `0x` 없음). 읽기 `strtoull(s, NULL, 16)` |
| 실수 | 쓰기 `%.17g`(왕복 정확; 0 은 `0`, 정수값은 소수점 없이 예 `3.5`, `22`). 읽기 `rec_real` = `strtod` 후 `isfinite` 가 아니면(nan·inf) `rec_bad`(그 줄 번호). 도구가 쓰는 값은 모두 유한하므로 손상·수정된 기록만 해당 |
| 문자열 | `rec_str`: **먼저 구분 공백 1개(`' '`)를 쓰고**, 빈 문자열은 `~`. 아니면 각 바이트 `c` 가 `c <= 0x20`, `c >= 0x7f`, `'%'`, `'~'` 이면 `%XX`(대문자 16진 2자리), 그 외는 그대로. 따라서 UTF-8 다바이트 문자는 모두 `%XX` 로 나간다 |
| `rec_str` 와 공백 | 이 문서에서 `"… %u" + rec_str(x)` 처럼 적은 형식은 `rec_str` 가 앞 공백을 스스로 쓰므로 서식 문자열 쪽에는 공백이 없다. 예: `fprintf("I %d %d %d", …)` + `rec_str(info)` → `I 1 2 3 ~` |
| 문자열 읽기 | `rec_unescape`: 토큰이 정확히 `~` 이면 빈 문자열. 아니면 `%` 뒤 두 글자가 모두 16진수면 1바이트로 복원(대소문자 무관), 아니면 `%` 를 글자 그대로 둔다(거부하지 않음). 대상 버퍼 크기 −1 바이트에서 자르고, 마지막에 `text_sanitize`(제어문자·C1·깨진 UTF-8 을 `?` 로)를 적용한다. `%00` 은 문자열을 거기서 끝낸다 |

### 6.2 파일 구조와 쓰기 순서

```
CTVREC 5                 ← 매직 (1줄)
H …                      ← 헤더 (1줄)
F …  (프레임 1 시작)
B …
[V …]
C 0 … C count-1          ← CS 행, 인덱스 순
K 0|1
I …*                     ← 신원(키프레임: 전부 / 아니면 바뀐 것만), idx 오름차순
X …*                     ← 사라진 신원 (K 0 일 때만), idx 오름차순
J …*                     ← 구간에 슬롯을 떠난 클라이언트, view.gone 순 (매 프레임 전부)
T …*                     ← view.tid 순
Q …*                     ← view.chain 순
P …*                     ← CS 인덱스 순, seen_n >= 0 인 CS 만
S …*                     ← view.slot 순
[A …]                    ← act_dropped > 0 일 때만
W …*                     ← st.act 순 (최초 목격 순)
O …*                     ← view.os 순
[D …]                    ← disk_avail 일 때만
[R …]                    ← wait_avail 일 때만
G …*                     ← pool_state == 1 일 때만, view.pool 순
E                        ← 프레임 끝
F … (프레임 2) …
```

읽기는 `F` 로 시작해 `E` 로 끝나는 것만 요구하며 프레임 안 줄 순서는 자유다(아래 검증의 상호 의존: `W` 의 samples 상한, `P`/`W`/`V`/`C` 의 CS 범위는 모두 이미 읽은 `F` 값만 쓴다). 같은 종류가 두 번 나오면 `B`/`D`/`R`/`V`/`K`/`A` 는 나중 값이 이기고, `C` 같은 인덱스 중복과 `I` 비증가는 거부된다. `J` 는 순서·중복 검증 없이 쌓인다.

### 6.3 매직 줄

0. `rec_path = path`, `fopen(path, "r")`. 실패(ENOENT, EACCES 등 원인 무관)하면 `die("cannot open record %s", "기록 파일 %s 열기 실패", path)`.
1. `rec_line = 1`. 첫 줄을 읽지 못하면(빈 파일) `rec_bad`(줄 1).
2. 첫 `\r` 또는 `\n` 에서 자른 뒤(`line[strcspn(line, "\r\n")] = 0`) 정확히 `CTVREC 5` 와 같아야 한다.
3. 다르면 다음 순서로 판정해 `die`(종료 1). 모두 `die(fmt_en, fmt_ko, path)` 이므로 경로가 `%s` 자리에 들어가고, `die` 의 규칙(01 §6)대로 4096 바이트로 잘라 `text_sanitize` 한 뒤 stderr 에 `cub_ctv: ` 접두와 개행을 붙여 쓴다:

| 조건 | EN | KO |
|---|---|---|
| `strncmp(line, "CTVREC ", 7) == 0` | `%s is a cub_ctv record of another format version (this build reads CTVREC 5)` | `%s 는 다른 형식 버전의 cub_ctv 기록입니다 (이 빌드는 CTVREC 5 만 읽음)` |
| `line[0] == '{'` 이고 `strstr(line, "cub_ctv detail dump")` | `%s is a detail dump (--dump): it holds everything cub_ctv read but is not a replay file - record with --record` | `%s 는 디테일 덤프(--dump)입니다: 판독한 정보를 모두 담지만 재생용이 아닙니다 - 재생하려면 --record 로 기록하십시오` |
| 그 외 | `%s is not a cub_ctv record (--record)` | `%s 는 cub_ctv 기록 파일(--record)이 아닙니다` |

출력 예(EN): `cub_ctv: /tmp/v4.rec is a cub_ctv record of another format version (this build reads CTVREC 5)`.

### 6.4 헤더 `H` (정확히 13 토큰, 반드시 2번째 줄)

쓰기 형식: `"H %d %d %d %.17g %d %u %d %d %d"` + `rec_str(db)` + `rec_str(lib)` + `rec_str(tran_refused)` + `\n`, 직후 `fflush`.

| tok | 필드 | 형식 | 읽기 |
|---|---|---|---|
| 0 | `H` | 리터럴 | 다르면 bad |
| 1 | `pid` | `%d` | `atoi` |
| 2 | `live` | `%d` 0/1 | `atoi` |
| 3 | `hz` | `%d` | `atoi` |
| 4 | `interval` | `%.17g` | `rec_real` |
| 5 | `thr_count` | `%d` | `atoi` |
| 6 | `degraded` | `%u` | `strtoul(…,10)` |
| 7 | `lo.stride` | `%d` | `atoi` |
| 8 | `lo.count` | `%d` | `atoi` |
| 9 | `lo.off_stats` | `%d` | `atoi` |
| 10 | `db` | 문자열 | `rec_unescape` → 128 B |
| 11 | `lib` | 문자열 | → 4096 B |
| 12 | `tran_refused` | 문자열(없으면 `~`) | → 64 B |

실제 쓰기는 매직과 헤더를 한 번에 낸다: `fprintf(rec_out, "%s\nH %d %d %d %.17g %d %u %d %d %d", REC_MAGIC, pid, live, hz, interval, thr_count, degraded, stride, count, off_stats)` → `rec_str(db)`, `rec_str(lib)`, `rec_str(tran_refused)` → `fputc('\n')` → `fflush`.

검증(위반 시 `rec_bad`, `rec_line = 2`): 줄이 없음, 토큰 수 ≠ 13(16개까지만 세므로 그 이상도 ≠ 13), tok0 ≠ `H`, `lo.count < 1` 또는 `> 64`, `live` 가 참인데 `interval` 이 `[0.1, 3600]` 밖(NaN 포함). 헤더 앞에 빈 줄을 둘 수 없다(2번째 줄이 곧 헤더). 이후 `rec_count = lo.count`, `rec_live = live` 가 프레임 검증에 쓰인다.

재생 시작 시 `replay_main`(06 §2.13)은 전역 강등 비트를 헤더 값으로 바꾸고(`deg_set(~0,0)` 후 `deg_set(h.degraded,1)`), `tran_refused` 가 비어 있지 않으면 그것을 강등 사유로 쓴다. 스냅샷 기록(`live == 0`)은 첫 프레임 하나만 읽어 그린다.

### 6.5 프레임 줄 상세

표의 "토큰"은 태그를 포함한 개수(최소/최대)이며 아래 §6.7 의 줄 규칙표와 같다.

#### `F` — 프레임 시작 (11/11)

쓰기: `"F %ld %d %d %.17g %d %d %d %d %d %d\n"`

| tok | 값 | 비고 |
|---|---|---|
| 1 | `ts` | `%ld` |
| 2 | `count` | 헤더 `count` 와 같아야 함 |
| 3 | `st.samples` | 0 ≤ samples ≤ `REC_SAMPLES_MAX`; 라이브 헤더면 ≥ 1 |
| 4 | `dt` | 쓰기 시 `min(dt, REC_DT_MAX)`. 라이브 헤더면 `0 < dt <= 1e7`(NaN·inf 거부) |
| 5 | `tran_avail` | |
| 6 | `cid_locked` | |
| 7 | `has_trid` | |
| 8 | `ntran` | |
| 9 | `pool_state` | 1/0/−1 |
| 10 | `pool_truncated` | −1, ≥0, −2/−3/−4 |

읽기(`rd_F`):
1. 이미 프레임 안이면 bad(프레임 안의 프레임).
2. `view.seq = VERDICT_HISTORY + rec_seq++` (§8).
3. 위 필드를 `view` 에 넣고(`ts` 는 `atol`, `dt` 는 `rec_real`(유한값만), 나머지는 `atoi` — 문법 검증 없음) 검증한다. 조건식은 정확히 다음과 같다:
   ```c
   if (view.count != rec_count
       || view.st.samples < 0 || view.st.samples > REC_SAMPLES_MAX
       || (rec_live && (view.st.samples < 1 || !(view.dt > 0.0)
                        || !(view.dt <= REC_DT_MAX))))
     rec_bad ();
   ```
   - `count` 는 `atoi` 이므로 `18x` 도 18 로 통과한다(헤더 `count` 와 같기만 하면 됨).
   - **스냅샷 헤더(`live == 0`)는 `dt` 를 전혀 검사하지 않는다**: 음수·`nan`·`inf` 도 그대로 `view.dt` 에 들어간다. samples 는 0..`REC_SAMPLES_MAX` 범위만 검사(≥1 요구 없음).
   - `rec_seq++` 는 검증보다 먼저 일어나지만, 실패하면 곧바로 종료하므로 관측되지 않는다.
4. 초기화: `nident = ntid = nchain = nslot = npool = 0`, `st.nact = nos = 0`, `disk_avail = wait_avail = 0`, `st.act_dropped = 0`, `hist_known = hist_n = 0`, 제거 목록 `nrm = 0`, `rd.keyframe = 1`(`K` 없는 프레임은 키프레임), `rows`·`st.occ`·`st.wai`·`st.seen_tran`·`st.seen_owner`·`st.seen_wq` 전부 0, 모든 `st.seen_n[k] = -1`·`st.seen_cpid[k] = -1`, `ngone = 0`(`J` 는 프레임마다 새로), 행 수신 표시·수 0, `in_frame = 1`.
5. **초기화되지 않는 것**: `view.degraded`(그 프레임에 `B` 가 없으면 직전 값 유지), `wait_cnt`·`disk_*` 값(가용 플래그만 0), `prev`(모든 `C` 가 덮음), `st.seen_slot`·`st.seen_tran_of`·`st.seen_cpid_of`.

#### `B` — 강등 비트 (2/2)
쓰기: `fprintf(f, "B %u\n", view.degraded)` — 모든 프레임에 정확히 1개, `F` 바로 뒤. 읽기: `view.degraded = (unsigned) strtoul(tok1, NULL, 10)`.

#### `V` — 판정 지속성 이력 (2/20)
쓰기(`hist_known` 일 때만): `fprintf(f, "V %d", hist_n)` → `i = 0..hist_n−1` 마다 `fprintf(f, " %d %d", hist_back[i], hist_top[i])` → `fputc('\n')`. `hist_known && hist_n == 0` 이면 `V 0` 한 줄을 쓴다(이력은 쌓고 있으나 앞 9 구간이 비어 있음). 예 `V 3 1 2 2 2 3 -1`.
읽기 검증(`n` = `atoi(tok1)`): 토큰 수 = `2 + 2n`, `n < VERDICT_HISTORY`(≤9), 각 `back` ∈ [1, 9], 각 `top` ∈ [−1, count−1]. 통과하면 `hist_known = 1`, `hist_n = n`, 배열 채움. `V` 가 없는 프레임은 `hist_known = 0` → 재생 화면은 이력을 가져오지 않고 재생 중 쌓은 이력을 쓴다.

#### `C` — CS 한 행 (24/24)
쓰기: `"C %d %d %u %u %d %llx %llx %d %u %u %u %.17g %.17g %d %d %d %llx %llx %u %u"` + `rec_str(name)`(앞 공백 포함) + `" %.17g %d"`(prev total_ms, seen_cpid) + `fputc('\n')`. CS 인덱스 `0..count−1` 순서, 모든 CS 를 쓴다.

| tok | 필드 | 형식 |
|---|---|---|
| 1 | CS 인덱스 `i` | `%d` |
| 2 | `rows[i].rwlock` | `%d` (>0 읽기 보유 수, −1 writer, 0 비어 있음) |
| 3 | `rows[i].wr` (대기 reader) | `%u` |
| 4 | `rows[i].ww` (대기 writer) | `%u` |
| 5 | `rows[i].tran` | `%d` |
| 6 | `rows[i].owner` | `%llx` |
| 7 | `rows[i].wq` | `%llx` |
| 8 | `rows[i].has_stats` | `%d` |
| 9 | `rows[i].nenter` | `%u` |
| 10 | `rows[i].nwait` | `%u` |
| 11 | `rows[i].nreenter` | `%u` |
| 12 | `rows[i].total_ms` | `%.17g` |
| 13 | `rows[i].max_ms` | `%.17g` |
| 14 | `st.occ[i]` | `%d` |
| 15 | `st.wai[i]` | `%d` |
| 16 | `st.seen_tran[i]` | `%d` (−1 없음) |
| 17 | `st.seen_owner[i]` | `%llx` |
| 18 | `st.seen_wq[i]` | `%llx` |
| 19 | `prev[i].nenter` | `%u` |
| 20 | `prev[i].nwait` | `%u` |
| 21 | `rows[i].name` | 문자열(64 B) |
| 22 | `prev[i].total_ms` | `%.17g` |
| 23 | `st.seen_cpid[i]` | `%d` (−1 모름) |

읽기 검증: `0 <= i < count`, 같은 프레임에서 `i` 가 처음. 읽은 뒤 `prev[i] = rows[i]` 로 복사하고 `nenter`/`nwait`/`total_ms` 만 tok19/20/22 로 바꾼다(캡처 6단계와 같은 모양). `st.seen_cpid[i] = atoi(tok23)`. `rows[i].wq2` 는 기록되지 않아 재생에서는 0(체인은 `Q` 가 이미 두 큐를 합친 결과를 담음). 행 수신 수 +1.

#### `K` — 신원 키프레임 플래그 (2/2)
`K 1` 또는 `K 0`. tok1 이 정확히 `"0"` 또는 `"1"` 이 아니면 bad(색인 중에도 같은 검사). 쓰기 `fprintf(f, "K %d\n", key)` — 모든 프레임에 정확히 1개를 C 행 뒤에 쓴다.

#### `I` — 신원 (10/10)
쓰기: `fprintf(f, "I %d %d %d", idx, trid, cpid)` + `rec_str` × 6 (info, user, program, login, host, ip; 각자 앞 공백) + `fputc('\n')`.
읽기: 이 프레임에서 이미 읽은 마지막 `I` 의 idx 보다 tok1 이 **엄격히 커야** 한다(아니면 bad). `TRAN_IDENT` 를 0 으로 채우고 cpid, 6 문자열(각 64 B)을 넣어 `view_add_ident(idx, trid, &id)`.

#### `J` — 슬롯을 떠난 클라이언트 (10/10)
토큰은 `I` 와 같다(`rec_ident_line(f, tag, v)` 가 태그만 바꿔 씀): `J idx trid cpid info user program login host ip`. 쓰기: `rec_idents`(`K`/`I`/`X`) 직후 `view.gone[0..ngone)` 순으로 — 키프레임과 무관하게 매 프레임 그 프레임의 전부. 읽기(`rd_J`): `TRAN_IDENT` 를 0 으로 채우고 cpid, 6 문자열을 넣어 `view_add_gone(idx, trid, &id)`. 순서·범위·중복 검증 없음. 신원 병합(§7.2)에 참여하지 않는다.

#### `X` — 사라진 신원 (2/2)
쓰기 `fprintf(f, "X %d\n", i)`. 읽기: `X idx`(`atoi`). 제거 목록 `rm` 에 추가(64 에서 2배 증가). 순서·범위 검증 없음. `K 1` 프레임의 `X` 는 효과가 없다(§7.2).

#### `T` — 소유자 tid → 칸 (3/3)
쓰기 `fprintf(f, "T %llx %d\n", tid, tid_slot)` (`view.tid` 순). 읽기 tid = `strtoull(…,16)`, slot = `atoi`. `ntid < 128` 일 때만 저장, 넘치면 조용히 버림.

#### `Q` — 대기 큐 머리와 칸 (3/68)
쓰기 `fprintf(f, "Q %llx %d", head, n)` → `k = 0..n−1` 마다 `fprintf(f, " %d", slot[k])` → `fputc('\n')` (`n == 0` 이면 `Q <head> 0`). 읽기 `n = atoi(tok2)`. 검증: `n >= 0`, 토큰 수 = `3 + n`. `n <= 64` 이고 `nchain < 128` 일 때만 저장(토큰 상한 68 때문에 n = 65 는 통과하지만 저장되지 않음; n ≥ 66 은 토큰 수 불일치로 bad).

#### `P` — 구간 최초 대기 큐 (3/68)
쓰기 `fprintf(f, "P %d %d", i, seen_n[i])` → `k < seen_n[i]` 마다 `fprintf(f, " %d", seen_slot[i][k])` → `fputc('\n')`. 읽기 `P cs n slot…`(모두 `atoi`). 검증: `0 <= cs < count`, `0 <= n <= 64`, 토큰 수 = `3 + n`. `st.seen_n[cs] = n`, `st.seen_slot[cs][*]`. 쓰기는 `seen_n >= 0` 인 CS 만(`P cs 0` 도 가능).

#### `S` — 칸 → tran (4/4)
쓰기 `fprintf(f, "S %d %d %d\n", slot, slot_tran, slot_cpid)`. 읽기 `S slot tran cpid`(`atoi`, cpid −1 = 모름). `nslot < 8192` 일 때만 저장.

#### `A` — 보관 못한 활동 목격 수 (2/2)
쓰기 `fprintf(f, "A %d\n", act_dropped)` — `act_dropped > 0` 일 때만. 읽기 `A n` → `st.act_dropped = atoi(tok1)`.

#### `W` — 활동 목격 (7/7)
쓰기 `fprintf(f, "W %d %d %d %d %d %d\n", cs, kind, tran, thr, samples, cpid)` (`st.act[0..nact)` 순). 읽기 `W cs kind tran thr samples cpid`(모두 `atoi`, cpid −1 = 모름). 검증(이 순서, 둘 다 bad): `0 <= samples <= F 의 samples`, `0 <= cs < count`, `kind ∈ {0, 1}`. `nact < 4096` 일 때만 저장. 스냅샷(samples 0)은 `nact == 0` 이므로 `W` 가 없다.

#### `O` — 스레드 OS 상태 (11/11)
쓰기: `"O %d %d %d %d %d %d %ld %ld"` (slot, tid, samples, run, dsk, oth, ticks0, ticks1) + `rec_str(comm)` + `" %d\n"` (span_ms). 읽기: 0 으로 채운 뒤 넣고(`t_first`/`t_last` = 0; ticks 는 `atol`, 나머지 `atoi`), `comm` 은 16 B, `nos < 64` 일 때만 저장. 값 검증 없음.

#### `D` — 로그 장치 (4/4)
쓰기 `fprintf(f, "D %.17g %.17g", await_ms, util_pct)` + `rec_str(dev)` + `fputc('\n')` — `disk_avail` 일 때만. 읽으면 `disk_avail = 1`, 두 값 `rec_real`, dev 는 32 B.

#### `R` — 대기 사유 히스토그램 (31/31)
쓰기 `fprintf(f, "R %d %d", wait_live, THR_RESUME_MAX)` → `i = 0..27` 마다 `fprintf(f, " %d", wait_cnt[i])` → `fputc('\n')` — `wait_avail` 일 때만. 읽기 `R live 28 c0 … c27`. 검증: **`atoi(tok2) != 28` 이면 bad** — 문법 검증이 아니라 `atoi` 값 비교이므로 `28x`, `028`, `+28` 도 통과한다. 토큰 수는 줄 규칙표가 정확히 31 로 강제. `wait_avail = 1`, `wait_live = atoi(tok1)`, `wait_cnt[k] = atoi(tok(3+k))`.

#### `G` — 풀 그룹 (6/6)
쓰기 `fprintf(f, "G %d %d %llu %.17g", type, cnt, nenter, max_ms)` + `rec_str(name)` + `fputc('\n')`. 읽기: type·cnt `atoi`, nenter `strtoull(…,10)`, max_ms `rec_real`, name 64 B. `npool < 1024` 일 때만 저장. 쓰기는 `pool_state == 1` 일 때 `view.pool[0..npool)`.

#### `E` — 프레임 끝 (1/1)
쓰기 `fputs("E\n", f)` 후 `fflush(f)`. 검증: 받은 `C` 행 수 = `count`(아니면 bad). 이어서 `idents_settle(rd.keyframe)`(§7.2), 프레임 완료.

### 6.6 판정 이력 `V` 의 출처 (ctv_verdict.c 요약)

이력은 `HIST_CAP = 4096` 원형 버퍼 `hist_seq[s % 4096] = s + 1`(0 = 비어 있음), `hist_top[…] = 최상위 CS 또는 −1`.
- `history_push(seq, top)`: `seq < 0` 이면 무시, 아니면 저장하고 `hist_kept = 1`. 요약 화면이 프레임마다(`fig[0].severity >= 2 ? fig[0].ix : -1`) 부른다.
- `history_export(seq, back, top)`: `hist_kept == 0` 이면 −1. 아니면 `b = 1..9` 에 대해 `seq − b` 에 항목이 있으면 `(b, top)` 추가, 개수 반환.
- `history_import(seq, n, back, top)`: `b = 1..9` 를 오름차순으로 돌며 `seq − b >= 0` 인 동안 칸을 비운 뒤(첫 음수에서 멈춤) `k = 0..n−1` 순으로 `history_push(seq − back[k], top[k])`. 요약 화면은 `hist_known` 이면 그리기 전에 import, 그다음 이번 프레임을 push.
  - "비운다" = `hist_seq[(seq − b) % 4096] = 0` 만. `hist_top` 은 건드리지 않는다(`hist_seq == 0` 이면 `history_at` 이 −2 를 돌려주므로 남은 `hist_top` 값은 읽히지 않음).
  - `hist_kept` 는 import 자체가 바꾸지 않는다. `n == 0` 이면 `history_push` 가 불리지 않으므로 import 만으로는 `hist_kept` 가 1 이 되지 않는다(요약 화면은 바로 이어서 이번 프레임을 push 하므로 결과적으로 1 이 됨).

### 6.7 줄 규칙표 `REC_LINE`

```c
typedef struct { const char *tag; int min_tok, max_tok, mark;
                 void (*read)(char **tok, int n, REC_RD *rd); } REC_LINE;
```

| tag | min | max | mark | handler |
|---|---|---|---|---|
| `F` | 11 | 11 | `REC_FRAME_START`(1) | `rd_F` |
| `C` | 24 | 24 | 0 | `rd_C` |
| `I` | 10 | 10 | 0 | `rd_I` |
| `J` | 10 | 10 | 0 | `rd_J` |
| `T` | 3 | 3 | 0 | `rd_T` |
| `Q` | 3 | 68 | 0 | `rd_Q` |
| `P` | 3 | 68 | 0 | `rd_P` |
| `W` | 7 | 7 | 0 | `rd_W` |
| `O` | 11 | 11 | 0 | `rd_O` |
| `V` | 2 | 20 | 0 | `rd_V` |
| `K` | 2 | 2 | `REC_FRAME_KEY`(2) | `rd_K` |
| `X` | 2 | 2 | 0 | `rd_X` |
| `A` | 2 | 2 | 0 | `rd_A` |
| `B` | 2 | 2 | 0 | `rd_B` |
| `D` | 4 | 4 | 0 | `rd_D` |
| `R` | 31 | 31 | 0 | `rd_R` |
| `S` | 4 | 4 | 0 | `rd_S` |
| `G` | 6 | 6 | 0 | `rd_G` |
| `E` | 1 | 1 | `REC_FRAME_END`(3) | `rd_E` |

`rec_line_of(tok, n)`: 표를 순서대로 훑어 `tok[0]` 과 태그가 같은 항목을 찾고, `min <= n <= max` 면 그 항목, 아니면 `NULL`. 태그가 없어도 `NULL`. 토큰화는 `REC_TOKENS_MAX`(68)개까지만 세므로 실제 토큰이 그보다 많은 줄의 `n` 은 68 이다.

`REC_RD`(프레임 읽기 상태): `seen[MAX_CS]`(행 수신 표시), `nrows`, `keyframe`, `in_frame`, `done`.

### 6.8 프레임 읽기 `view_replay_next()` — 0 = 프레임 적재, 1 = 더 없음

1. `REC_RD` 를 0 으로, `keyframe = 1`.
2. 줄을 하나씩 읽으며(`rec_line++`):
   1. 줄에 `\n` 이 없고 파일 끝이면(기록 중단으로 잘린 줄) 루프 종료.
   2. 토큰화, 0개면 다음 줄.
   3. `spec = rec_line_of(tok, n)`. `spec == NULL`(모르는 태그·토큰 수 위반)이거나, `spec` 이 프레임 시작이 아닌데 프레임 밖이면 `rec_bad`.
   4. `spec->read(tok, n, &rd)`. `rd.done` 이면 0 반환.
3. 파일 끝에서 프레임 안이었으면 `rec_tail_warn()`. 1 반환.

`rec_bad()`: 경로와 줄 번호를 먼저 한 문자열로 만든 뒤 `die` 의 단일 `%s` 인자로 넘긴다.

```c
char where[4200];
snprintf (where, sizeof (where), "%s:%ld", rec_path, rec_line);
die ("malformed record file at %s", "기록 파일 형식 오류: %s", where);
```

- 따라서 `die` 가 `where` 전체(경로 + `:` + 줄 번호)를 4096 바이트로 잘라 `text_sanitize` 한다. 출력은 stderr `cub_ctv: malformed record file at <path>:<line>\n` / `cub_ctv: 기록 파일 형식 오류: <path>:<line>\n`, 종료 코드 1.
- `<line>` = 전역 `rec_line`(`long`). 매직 단계 1, 헤더 단계 2, 이후 `fgets` 호출마다 +1. 즉 **`fgets` 덩어리 번호**다: 16383 바이트 이하인 정상 줄에서는 1 기반 물리 줄 번호와 같고, 그보다 긴 줄은 덩어리마다 번호가 하나씩 올라간다. 빈 줄도 센다.
- 실측: `tests/fixtures/demodb-live.rec` 의 앞 5줄 뒤에 `Z 1` 을 붙인 파일 → `cub_ctv: malformed record file at …/bad.rec:6`.

`rec_tail_warn()`: 프로세스당 한 번만 `"last frame of the record is incomplete (recording interrupted) - ignored"`(한글 `기록의 마지막 프레임이 불완전합니다(기록 중단) - 무시`)를 stderr 에 경고. 불완전한 마지막 프레임은 버려진다(오류 아님).

16383 바이트를 넘는 줄은 `fgets` 가 나눠 읽으므로 나머지가 다음 줄로 해석되어 대개 bad 가 된다(정상 기록의 최장 줄은 이보다 훨씬 짧다).

---

## 7. 신원 키프레임

### 7.1 쓰기 (`rec_idents`, `view_record_frame` 이 C 행 뒤에 호출)

상태: `wr_hash[idx]`(마지막으로 쓴 신원 줄의 해시, 0 = 안 씀), `wr_seen[idx]`(그 신원을 마지막으로 보인 프레임 번호), 용량 `wr_cap`. `wr_grow(idx)`: `idx >= wr_cap` 이면 용량을 `idx + 64` 로 늘리고 새 부분을 0 으로(실패 시 −1). 줄어들지 않으며 녹화를 새로 열어도 초기화하지 않는다(첫 프레임이 항상 키프레임이라 무해).

해시 `ident_hash(v)` — FNV-1a 64(`h = (h ^ byte) * 1099511628211`, 시드 `14695981039346656037`):
1. `trid` 4바이트(호스트 바이트 순서), 2. `cpid` 4바이트, 3. `info`, `user`, `program`, `login`, `host`, `ip` 각 문자열을 종료 NUL 포함으로, 이 순서로 누적. 결과 `| 1`(0 이 되지 않게).

해시는 파일에 나가지 않는다. 같은 출력을 내려면 "(trid, cpid, 6 문자열) 중 하나라도 바뀌면 `I` 를 다시 쓴다"를 구현하면 된다(해시 충돌로 갱신이 누락될 가능성만 다르다).

절차(`rec_frames` 는 이미 이번 프레임까지 센 1 기반 번호):
1. `key = ((rec_frames − 1) % 60 == 0)` — 기록의 1, 61, 121, … 번째 프레임(0 기반 0, 60, 120, …).
2. `key` 가 아니면 모든 신원에 `wr_grow(idx)`; 하나라도 실패하면 `key = 1`(메모리 부족 시 전부 씀).
3. `K <key>` 출력.
4. `key` 이고 `wr_cap > 0` 이면 `wr_hash` 전부 0.
5. `view.ident` 순서(idx 오름차순)로 각 `v`: `h = ident_hash(v)`. `key` 이거나 `wr_hash[idx] != h` 면 `I` 줄 출력. `wr_grow` 성공 시 `wr_hash[idx] = h`, `wr_seen[idx] = rec_frames`.
6. `key` 가 아니면 `i = 0 .. wr_cap−1` 에서 `wr_hash[i] != 0 && wr_seen[i] != rec_frames` 인 것마다 `X i` 출력 후 `wr_hash[i] = 0`.

### 7.2 읽기 (`idents_settle`, `E` 에서 호출)

상태: 이어받은 신원 `car[0..ncar)`(idx 오름차순), 제거 목록 `rm[0..nrm)`.

- `keyframe` 이면: 이번 프레임의 `I` 줄들(`view.ident`)이 곧 전체 집합. `X` 는 무시.
- 아니면 병합: `upd` = 이번 `I` 줄 복사, `rm` 정렬, `view.nident = 0` 후 두 정렬 목록을 idx 순으로 합친다.
  1. `upd` 가 소진됐거나 `car[i].idx < upd[j].idx` 면 `car[i]` 를 택하되 `rm` 에 있으면 버린다.
  2. 아니면 `upd[j]` 를 택하고, `car[i].idx == upd[j].idx` 면 `car[i]` 를 건너뛴다(갱신).
  3. 택한 것을 `view_add_ident`.
  `rm` 은 이어받은 항목에만 적용된다(같은 프레임의 `I` 가 이긴다).
- 마지막으로 결과(`view.ident`)를 `car` 로 복사해 다음 프레임에 넘긴다.

### 7.3 탐색 색인 (`view_replay_index`, `view_replay_goto`)

`view_replay_index()` — 현재 위치(헤더 직후)부터 끝까지 훑어 완전한 프레임(`F`…`E`)마다 `rec_off`(F 줄의 파일 오프셋), `rec_lno`(F 줄 번호), `rec_kf`(키프레임 여부)를 기록하고 프레임 수를 반환, 파일 위치는 원래대로 되돌린다. 프레임 읽기와 **같은 토큰화와 같은 줄 규칙표**를 쓰되 `mark` 만 본다:
1. 잘린 마지막 줄(`\n` 없음 + EOF)에서 멈춤.
2. `spec` 이 프레임 시작: 이미 열린 프레임이 있으면 그 줄 번호로 `rec_bad`. 아니면 `f_off = 줄 시작 오프셋`, `f_lno`, `f_kf = 1`.
3. `spec` 이 키 표시: `f_kf = rec_key_value(tok)`(값이 0/1 이 아니면 bad).
4. `spec` 이 프레임 끝이고 열린 프레임이 있으면 항목 추가(배열 256 에서 2배씩), 프레임 닫음.
5. 모르는 태그·토큰 수 위반 줄과 프레임 밖 `E` 는 색인에서는 무시한다(그 프레임을 실제로 읽을 때 `rec_bad`). 단 `K` 줄은 프레임 안팎과 무관하게 값을 검사해 0/1 이 아니면 그 줄 번호로 `rec_bad`(읽기 경로와 같은 줄 번호).
6. 끝에 열린 프레임이 남아 있으면 `rec_tail_warn()`.

색인 함수가 쓰는 지역 상태와 끝난 뒤 복원되는 것:

| 상태 | 색인 중 | 색인 후 |
|---|---|---|
| 파일 위치 | 시작 시 `pos = ftell(rec_in)` 저장, 줄마다 `at = ftell` | `fseek(rec_in, pos, SEEK_SET)` 으로 **복원** |
| 줄 번호 | 지역 `lno = rec_line` 에서 시작해 `fgets` 마다 +1 | 시작 시 `rec_line` 을 `line0` 에 보관하고, 끝에서 파일 위치와 함께 `rec_line = line0` 으로 **복원한다**. 색인 중 `rec_line = lno` 를 대입하는 곳은 프레임 안 `F`(곧바로 `rec_bad`)와 `K` 줄(`rec_key_value` 가 `rec_bad` 할 때 그 줄을 보고하기 위함) 두 군데뿐이다 |
| `rec_seq` | 건드리지 않음 | 그대로(헤더 직후 0) |
| `ncar`/`car`, `view` | 건드리지 않음 | 그대로 |
| `rec_off`/`rec_lno`/`rec_kf`, `rec_nframes` | 새로 채움(이전 내용은 덮어씀, `realloc` 재사용) | 색인 결과 |

결과: 대화형 재생은 색인 직후 첫 프레임(`i = 0 = loaded + 1`)을 `view_replay_next()` 로 읽고, `seq`(10)와 `rec_bad` 줄 번호가 스크롤 재생과 같다. 실측: 8번째 줄에 `W` samples 초과를 둔 2 프레임 기록(`K` 는 7·14번째 줄)에서 `--json` 스크롤 재생과 터미널 대화형 재생 모두 `…:8` 을 보고한다.

`view_replay_goto(i)`:
1. `i` 가 `[0, rec_nframes)` 밖이면 −1.
2. `j = i` 에서 `j > 0 && !rec_kf[j]` 인 동안 `j--`(가장 가까운 앞쪽 키프레임, 없으면 0).
3. `fseek(rec_off[j])`, `rec_line = rec_lno[j] − 1`, `rec_seq = j`, `ncar = 0`(이어받은 신원 폐기).
4. `j .. i` 프레임을 `view_replay_next()` 로 차례로 읽는다(하나라도 실패하면 −1). 결과 `view` = 프레임 `i`, 신원 완전.

대화형 재생은 다음 프레임(`i == loaded + 1`)이면 `view_replay_next`, 그 외(뒤로·건너뛰기)는 `view_replay_goto` 를 쓴다. 한 번의 `goto` 는 키프레임을 포함해 최대 60 프레임(`REC_KEYFRAME`)을 읽는다(`K` 없는 기록은 모든 프레임이 키프레임이라 1 프레임).

`view_replay_seekable()`: `rec_in != NULL && ftell(rec_in) >= 0 && fseek(rec_in, 0, SEEK_CUR) == 0`. 거짓(파이프·FIFO)이면 터미널 재생도 경고 후 키 없는 스크롤 재생으로 바뀐다.

---

## 8. 재생 번호 매기기

- 라이브: `view.seq` 는 0 에서 시작해 캡처마다 +1 → 첫 프레임 1.
- 재생: `rd_F` 가 `view.seq = VERDICT_HISTORY + rec_seq++`. `rec_seq` 는 기록 안 0 기반 프레임 서수(파일을 처음부터 읽으면 0, `goto` 는 `j` 로 맞춤). 따라서 첫 프레임은 10.

이유:
1. `V` 는 거리(1..9 구간 전)로 이력을 담고, 화면은 `history_import(seq, …)` 로 `seq − back` 칸에 넣는다. `history_push` 는 음수 seq 를 버리고 `history_import` 는 `seq − b >= 0` 칸만 비우므로, 재생 seq 가 0 부터면 중간에 시작한 기록의 처음 9 프레임 이력이 사라진다. 10 을 더하면 `seq − back >= 1` 이 항상 성립한다.
2. 이력은 seq 를 열쇠로 하므로, 같은 프레임을 다시 그리거나(일시정지·언어 전환) 뒤로 갔다 오면 같은 칸을 덮어쓸 뿐 새 항목이 생기지 않는다. 같은 프레임은 어떤 경로(`next`/`goto`)로 읽어도 같은 seq 를 받는다.

---

## 9. 기록 파일 관리

### 9.1 `private_create(path, excl)` — 0600 비공개 파일

1. `open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (excl ? O_EXCL : 0), 0600)`.
   - `O_NOFOLLOW`: 이름이 심볼릭 링크면 실패.
   - `O_NONBLOCK`: 이름이 FIFO 여도 열기에서 막히지 않게(다음 검사에서 거부).
2. 이어서 하나라도 실패하면 fd 를 닫고 `NULL`:
   `fstat` 성공, `S_ISREG`(정규 파일), `st_nlink == 1`, `st_uid == geteuid()`, `fchmod(fd, 0600)`, `ftruncate(fd, 0)`(기존 파일은 모드 변경 **뒤에야** 비움), `fcntl(fd, F_SETFL, 0)`(비차단 해제).
3. `fdopen(fd, "w")`; 실패하면 fd 를 닫고 `NULL`.

디테일 덤프(`--dump`)도 같은 함수(`excl = 0`)를 쓴다.

### 9.2 열기·쓰기·닫기

| 함수 | 동작 |
|---|---|
| `view_record_open(path, h, excl)` | `private_create`, `rec_frames = 0`. 실패면 −1. 성공하면 매직 줄과 `H` 줄을 쓰고 `fflush`, 0 |
| `view_record_frame()` | 열린 기록이 없으면 아무것도 안 함. `rec_frames++` 후 §6.2 순서로 한 프레임을 쓰고 `fflush`(중단돼도 완성된 프레임은 파일에 남음) |
| `view_record_ok()` | 열린 기록이 없거나 `ferror` 가 아니면 0, 쓰기 오류가 났으면 −1 |
| `view_record_active()` | 열린 기록이 있으면 1 |
| `view_record_close()` | 열려 있으면 `ferror` → −1, `fclose` 실패 → −1, 핸들 해제. 반환 코드 |
| `view_record_frames()` | 이번 기록에 쓴 프레임 수 |

호출 규칙:
- `--record FILE`(`excl = 0`): 헤더는 시작 직후. 스냅샷은 캡처(와 풀) 뒤 1프레임 쓰고 닫는다. 라이브는 구간을 닫을 때마다 `view_capture` → `view_record_frame` → `view_record_ok`; 실패하면 "cannot write record" 로 종료. 닫기 실패도 종료. 프레임 0 개여도 파일은 지우지 않는다.
- `[r]` 녹화(라이브 요약 화면, 샘플링 실행에서만, `--record` 가 없을 때만): 이름 `<pid>_<db>_<YYYYmmdd-HHMMSS>.ctv`(현재 디렉터리, 지역 시각). `db` 는 128 B 로 자른 뒤 영숫자·`_`·`-`·(첫 글자가 아닌) `.` 외 바이트를 `_` 로 바꾼다. `excl = 1` 로 열고, 실패 원인이 `EEXIST` 면 `<…>-2.ctv` … `<…>-9.ctv` 를 차례로 시도, 그 외 실패나 `-9` 도 실패면 포기(−1, 푸터에 실패 표시). 헤더의 `degraded` 는 녹화를 연 시점의 전역 값. 쓰기 실패 시 조용히 닫고 녹화 중지(푸터에 1회 표시). 다시 `[r]` 이나 종료로 닫을 때 쓴 프레임이 0 이면 파일을 지운다.

---

## 10. 예시 기록

### 10.1 실제 기록에서 발췌 (`tests/fixtures/`)

라이브 기록 첫 줄들(`demodb-live.rec`, 18 CS, 100 Hz, 0.5 s). 이 픽스처는 3번째 줄이 빈 줄이고 `K` 줄이 없으므로 모든 프레임이 신원 키프레임으로 읽힌다:

```
CTVREC 5
H 3912964 1 100 0.5 4314 0 160 18 152 demodb /opt/cubrid/lib/libcubrid.so.11.5 ~

F 1793193213 18 46 0.50033167004585266 1 1 1 102 0 0
B 0
C 0 0 0 0 -1 0 0 1 0 0 0 0 0 0 0 -1 0 0 0 0 WFG 0 -1
C 1 -1 0 0 0 7f7badffb640 0 1 3450205 823841 19872 529156.09199999995 232.00200000000001 29 15 29 7f7b61feb640 7f7bf59ada40 3448962 823479 LOG 528854.09100000001 -1
I 1 13985814 3214132 ~ DBA csql cubrid dbhost01 10.0.0.79
W 1 0 0 4300 24 -1
O 4300 3913067 9 6 3 0 20464 20472 log-flush 450
```

스냅샷 기록(`demodb-snapshot.rec`): `F` 의 samples·dt 가 0, 풀 수집됨(`pool_state 1`, `pool_truncated -1`), `O` 는 이름만(샘플 0):

```
F 1793193204 18 0 0 1 1 1 102 1 -1
Q 7f7bf59b0970 5 4237 4226 4257 4300 4230
O 4221 3913152 0 0 0 0 0 0 transaction 0
G 3 408 739786400 11.000999999999999 CONN_ENTRY
```

### 10.2 완전한 작은 기록 (CS 2개, 2 프레임, 키프레임 + 델타)

아래 파일은 이 빌드의 `cub_ctv --replay … --json` 으로 오류 없이 읽힌다.

```
CTVREC 5
H 4242 1 100 0.5 512 0 160 2 152 demodb /opt/cubrid/lib/libcubrid.so.11.5 ~
F 1793193213 2 50 0.50033167004585266 1 1 1 4 0 0
B 0
C 0 0 0 0 -1 0 0 1 120 0 0 3.5 1 0 0 -1 0 0 100 0 WFG 3 -1
C 1 -1 0 1 3 7f7badffb640 7f7bf59ada40 1 3450205 823841 19872 529156.09199999995 232.00200000000001 29 15 3 7f7badffb640 7f7bf59ada40 3448962 823479 LOG 528854.09100000001 3214140
K 1
I 1 13985814 3214132 ~ DBA csql cubrid dbhost01 10.0.0.79
I 3 13985820 3214140 ~ DBA my%20app cubrid dbhost01 10.0.0.80
T 7f7badffb640 4300
Q 7f7bf59ada40 1 4227
P 1 1 4227
S 4227 1 3214132
W 1 0 3 4300 29 3214140
W 1 1 1 4227 15 3214132
O 4300 3913067 9 6 3 0 20464 20472 log-flush 450
D 1.25 12.5 sda
R 143 28 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0
E
F 1793193214 2 49 0.50688785314559937 1 1 1 4 0 0
B 0
V 1 1 1
C 0 0 0 0 -1 0 0 1 130 0 0 3.5 1 0 0 -1 0 0 120 0 WFG 3.5 -1
C 1 0 0 0 -1 0 0 1 3460000 823900 19872 529400 232.00200000000001 10 2 3 7f7badffb640 0 3450205 823841 LOG 529156.09199999995 3214140
K 0
I 3 13985821 3214140 ~ DBA batch cubrid dbhost01 10.0.0.80
X 1
J 1 13985814 3214132 ~ DBA csql cubrid dbhost01 10.0.0.79
W 1 0 3 4300 10 3214140
W 1 1 1 4227 2 3214132
R 140 28 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
E
```

해석:
- 프레임 1(키프레임): CS 1(`LOG`)을 tran 3 의 칸 4300 이 writer 로 보유(`rwlock -1`, owner `7f7badffb640` → `T` 로 칸 4300), writer 1명 대기(`ww 1`), 큐 머리 `7f7bf59ada40` 에 칸 4227(`Q`), 칸 4227 은 tran 1 을 위해 대기(`S`). 구간 50 샘플 중 29 샘플 보유, 15 샘플 대기(`W`; 끝 토큰은 목격 당시 그 슬롯의 클라이언트 pid). 프로그램 이름의 공백은 `%20`. `R` 은 코드 17(lock) 대기 1. 로그 장치 `sda` await 1.25 ms, 사용률 12.5 %.
- 프레임 2(`K 0`): 이어받은 신원 {1, 3} 에서 tran 3 은 `I` 로 갱신(program `batch`, trid 변경), tran 1 은 `X 1` 로 제거 → 결과 {3}. 구간 중 tran 1(pid 3214132)이 대기했으나 지금은 슬롯을 떠났으므로 `J 1 …` 이 그 클라이언트의 신원을 남긴다(`view_identity_seen(1, 3214132)` = 1). `V 1 1 1` = 1 구간 전 최상위 CS 는 1. `P`·`Q`·`D` 가 없으므로 그 CS 의 구간 큐 없음, 디스크 수치 없음(`disk_avail 0`).
- 재생 seq: 프레임 1 = 10, 프레임 2 = 11.

---

## 11. 라이브 `dt` 와 재생 `dt`

`view_record_frame` 은 `F` tok4 에 `min(view.dt, REC_DT_MAX)` 를 쓰지만, 라이브의 `view.dt` 와 렌더러 인자 `dt` 는 자르지 않은 `now − t_prev` 다(06 §12.4). 따라서 구간이 `1e7` s(약 116 일)를 넘는 극단적 경우에만 라이브 화면과 재생 화면의 `dt`·`enter/s`·`wait/s`·`lost_s` 가 달라진다. 정상 실행(최대 `--interval` 3600 s, 일시정지 포함)에서는 차이가 없다.
