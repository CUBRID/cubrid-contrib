# 표시·실행 명세 (Display and Run Specification)

## 0. 범위와 표기

| 항목 | 내용 |
|---|---|
| 대상 모듈 | `src/cub_ctv.c`(명령행·기동·모드 분기), `src/ctv_verdict.c`(판정 수치), `src/ctv_screen.c`(요약 화면), `src/ctv_rows.c`(텍스트 행), `src/ctv_json.c`(JSON), `src/ctv_present.c`(프레임 출력 선택), `src/ctv_term.c`(터미널), `src/ctv_sampler.c`(라이브 샘플러), `src/ctv_live.c`(라이브 루프), `src/ctv_replay.c`(재생 루프), 그리고 `src/ctv.h` 의 관련 상수·구조체 |
| 함께 인용하는 보조 함수 | `ctv_text.c`(폭 규칙: `dw_pad`, `str_cols`, `seq_cols`, `utf8_cols`, `buf_cat`, `text_sanitize`), `ctv_msg.c`(`die`/`warn`/`warnf`, `deg_names`), `ctv_frame.c`(`resume_name`, `view_*` 질의), `ctv_cs.c`(`state_str`), `ctv_pool.c`(`pool_print_groups`), `ctv_proc.c`(서버 목록), `ctv_elf.c`(`find_lib_base`) — 출력 바이트에 영향을 주는 부분만 |
| 범위 밖 | 메모리 판독·프로브(I-0..I-20), 기록 파일(CTVREC) 형식, 디테일 덤프 내용, 프레임 캡처 내부(`view_capture`) — 별도 명세 |
| 표기 | `M(en, ko)` = `use_ko` 가 1 이면 한국어, 아니면 영어 문자열. 문자열 리터럴은 C 이스케이프 그대로(`\n`, `\033`). `␠` 표기는 쓰지 않고 공백 수는 숫자로 적는다 |
| 원칙 | 아래 문자열·서식·임계값은 소스 그대로다. `docs/usage.md` 예시와 다르면 이 문서(=소스)가 맞다 |

---

## 1. 전역 상태와 상수 (ctv.h 관련분)

### 1.1 표시·실행에 쓰이는 상수

| 상수 | 값 | 쓰임 |
|---|---|---|
| `LIVE_TRANS_ROWS` | 24 | 요약 화면 세션 패널 최대 행 수 |
| `VERDICT_HISTORY` | 10 | 지속(persistence) 집계 창: 현재 포함 최근 10 프레임 |
| `VERDICT_WAITERS_LISTED` | 4 | 판정 `who suffers` 근거에 나열하는 대기 세션 수 |
| `HIST_CAP` (verdict.c 내부) | 4096 | 프레임 번호→1위 CS 링 테이블 크기 |
| `CHAIN_REWALK_S` | 0.25 | 같은 큐 헤드 재순회 최소 간격(초) |
| `OS_SAMPLE_MIN_S` | 0.05 | 새 큐 헤드 순회 및 procfs 스레드 상태 샘플 최소 간격(초) |
| `OS_MAX` | 64 | 윈도우당 OS 상태를 유지하는 스레드 수 |
| `OS_COMM_LEN` | 16 | 스레드 이름 버퍼 |
| `ACT_MAX` | 4096 | 활동(ACT) 표 크기 |
| `ACT_HELD_RESERVE` | 256 | 그중 보유(HELD) 목격 전용 예약분 |
| `ACT_HASH` | 16384 | 활동 해시(2의 거듭제곱, > 2×ACT_MAX) |
| `ACT_HELD` / `ACT_WAIT` | 0 / 1 | 활동 종류 |
| `THR_WAITERS_MAX` | 64 | 대기 체인 최대 홉 |
| `THR_RESUME_MAX` | 28 | resume_status 코드 0..27 |
| `THR_RESUME_CS_MIN` / `MAX` | 7 / 12 | CS 대기로 묶는 resume 코드 범위(양끝 포함) |
| `TAG_LEN` | 1024 | 소유자 블록 버퍼 크기 |
| `TAG_TAIL_RESERVE` | 48 | 체인 끝 `[n of W waiting]` 자리 예약 |
| `INTERVAL_MIN` / `INTERVAL_MAX` | 0.1 / 3600.0 | `--interval` 범위 |
| `SERVERS_MAX` | 1024 | 나열할 cub_server 최대 수 |
| `LAYOUT_AUTO`/`WIDE`/`NARROW` | 0/1/2 | 소유자 블록 배치 |
| `TRAN_TEXT_SHORT`/`LABEL` | 0/1 | `tran_text` 형식 |
| `THR_INDEX(slot)` | `slot + 1` | 화면에 보이는 스레드 번호 |
| `KEY_IGNORED`..`KEY_FWD` | 256..260 | 키 코드(§5.4) |
| `TRANS_TABLE_ONLY(trans, record)` | `trans && record == NULL` | 표 단독 모드 판정 |

### 1.2 VB_* (판정 원인 분류)

| 상수 | 값 | 의미 |
|---|---|---|
| `VB_NONE` | 0 | 스레드 상태도, 로그 장치도 모름 |
| `VB_DISK` | 1 | 디스크 쓰기 지연 |
| `VB_CPU` | 2 | 쥔 스레드 CPU 포화 |
| `VB_SHORT` | 3 | 둘 다 포화 아님(짧은 보유가 잦음) |
| `VB_UNKNOWN` | 4 | 장치는 아는데 스레드 상태는 모름 |

### 1.3 표시 관련 전역 변수

| 변수 | 정의 위치 | 초기값 | 설정 |
|---|---|---|---|
| `use_ko` | ctv_msg.c | 0 | main 의 언어 사전 스캔, `--ko`/`--en`, 라이브·재생의 `l` 키 |
| `color_on` | ctv_term.c | 0 | main: `!plain && !json && isatty(STDOUT_FILENO)` |
| `layout_mode` | ctv_rows.c | `LAYOUT_AUTO` | `--wide`/`--narrow`(마지막 지정이 유효) |
| `layout_detail` | ctv_rows.c | 0 | `--detail`, `--detail=X` |
| `detail_filter` | ctv_rows.c | NULL | main 이 `o.detail_filter` 를 복사 |
| `live_footer` | ctv_present.c | NULL | 라이브 루프/대화형 재생이 화면 하단 문구로 설정, 종료 시 NULL |
| `target_pid` | 다른 모듈 | — | main 이 대상 pid 로 설정(procfs 경로용) |
| `degraded`, `tran_refused` | ctv_msg.c | 0, NULL | 프로브·재생 헤더 |

### 1.4 구조체 (ctv.h)

`CS_ROW`, `LIVE_WIN` 은 05 §3.7·§3.8 에 있다. 여기는 표시·실행 모듈이 소유한 형이다. 필드 순서는 선언 순서.

**`OPTS`** (명령행 결과, `parse_args` 가 0 으로 채운 뒤 `hz = 100`, `interval = 1.0`, `replay_speed = 1.0`):

| 필드 | 형 | 의미 |
|---|---|---|
| `target` | `const char *` | 대상 단어(db 이름 또는 pid 문자열), 없으면 NULL |
| `dump` | `const char *` | `--dump FILE` |
| `record` | `const char *` | `--record FILE` |
| `replay` | `const char *` | `--replay FILE` |
| `detail_filter` | `const char *` | `--detail=X` 또는 위치 단어 필터 |
| `live` | `int` | `-i` |
| `trans` | `int` | `--trans` |
| `json` | `int` | `--json` |
| `hz` | `int` | `--hz` |
| `plain` | `int` | `--plain` |
| `interval` | `double` | `--interval` |
| `replay_speed` | `double` | `--replay-speed` |
| `list` | `int` | `-l` |

`help` 는 `OPTS` 필드가 아니라 `parse_args` 지역 변수다. `--ko`/`--en`, `--wide`/`--narrow`, `--detail` 플래그는 전역(`use_ko`, `layout_mode`, `layout_detail`)에 바로 쓴다.

**`CS_RATES`** (`cs_rates` 결과):

| 필드 | 형 | 의미 |
|---|---|---|
| `enter_s`, `wait_s` | `double` | 구간 초당 진입·대기 건수 |
| `occ_pct`, `wait_pct` | `double` | 점유·대기 샘플 비율(%) |
| `de`, `dw` | `uint32_t` | `nenter`·`nwait` 의 구간 차(u32 랩 안전) |

선언 순서는 `enter_s, wait_s, occ_pct, wait_pct, de, dw`.

**`CS_FIG`** (`cs_figures` 결과, 순위 표 1행):

| 필드 | 형 | 의미 |
|---|---|---|
| `ix` | `int` | CS 인덱스 |
| `occ_pct`, `wait_pct`, `enter_s`, `wait_s` | `double` | `CS_RATES` 와 같음 |
| `avg_ms` | `double` | 진입당 획득 ms |
| `lost_s` | `double` | 초당 잃은 초(s/s) |
| `severity` | `int` | 0 유휴 .. 4 |

선언 순서는 `ix, occ_pct, wait_pct, enter_s, wait_s, avg_ms, lost_s, severity`.

**`VERDICT`** (`verdict_compute` 결과, `memset` 0 에서 시작):

| 필드 | 형 | 의미 |
|---|---|---|
| `top`, `second` | `CS_FIG` | 1위 CS, 2위 CS(`has_second` 일 때만 채움) |
| `has_second` | `int` | 2위도 심각도 ≥ 2 |
| `same`, `span` | `int` | 지속성: 최근 10 프레임 중 같은 1위 수 / 알려진 프레임 수 |
| `holder` | `int` | −2 없음(reader 또는 미관측), 0 엔진, ≥1 클라이언트 tran |
| `holder_cpid` | `int` | 그 보유자가 목격될 때의 클라이언트 pid, −1 모름 |
| `one_thr` | `int` | 엔진 보유자를 한 스레드로 특정함 |
| `held_pct`, `thr_pct` | `double` | 보유자 몫(%), 그 한 스레드 몫(%) |
| `nwaiters` | `int` | 대기한 클라이언트 세션 수 |
| `waited_pct` | `double` | 세션당 평균 대기 몫(%) |
| `wtran[4]`, `wsamples[4]` | `int` | 나열할 대기 세션 tran 과 목격 샘플 수(`VERDICT_WAITERS_LISTED`) |
| `nlisted`, `more_waiters` | `int` | 나열 수(≤4), 5번째 이상 있음 |
| `os` | `const OS_THR *` | 가장 많이 샘플된 보유 스레드, 없으면 NULL(`view.os` 안을 가리킴) |
| `dsk_pct`, `run_pct`, `cpu_pct` | `double` | 그 스레드의 D·R 상태 비율, CPU 사용률(%) |
| `bound` | `int` | `VB_*` |

---

## 2. 명령행 (src/cub_ctv.c)

### 2.1 언어 사전 스캔 (parse_args 이전)

1. `LANG` 환경변수가 NULL 이 아니고 앞 2글자가 대소문자 무시로 `ko` 이면 `use_ko = 1`, 아니면 0. 코드: `use_ko = lang != NULL && strncasecmp(lang, "ko", 2) == 0` — `ko_KR.UTF-8`, `KO_kr`, `Ko` 모두 한국어.
2. `argv[1..argc-1]` 을 순서대로 보며 정확히 `--ko` 이면 1, `--en` 이면 0 으로 덮는다(마지막이 이김). 다른 인자의 값 위치에 있어도 문자열이 같으면 반영된다.
3. 이 결과는 인자 오류 메시지·도움말 언어에 바로 쓰인다.
4. 이어 `prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0)` (결과 무시) 후 `parse_args`.

### 2.2 옵션 표

`OPTS` 초기값: 전부 0/NULL, `hz = 100`, `interval = 1.0`, `replay_speed = 1.0`. 비교는 모두 정확 일치(`strcmp`)다. `--opt=값` 형식은 `--detail=` 외에는 없다(예: `--interval=1` 은 알 수 없는 옵션).

| 인자(별칭 포함) | 동작 | 값 | 비고 |
|---|---|---|---|
| `-h`, `--help` | `help = 1` | — | 루프는 계속 진행. 뒤의 인자 오류는 여전히 die |
| `-i`, `--interactive`, `--live` | `o->live = 1` | — | `--live` 는 숨은 옛 이름 |
| `-l`, `--list` | `o->list = 1` | — | |
| `--trans` | `o->trans = 1` | — | |
| `--json` | `o->json = 1` | — | |
| `--detail` | `layout_detail = 1` | — | 필터는 `--detail=` 또는 위치 단어(§2.4) |
| `--detail=X` (X 비어있지 않음) | `layout_detail = 1`, `o->detail_filter = X` | 문자열 | `--detail=` (빈 값)은 알 수 없는 옵션으로 die |
| `--pool` | 무시 | — | 숨은 옛 옵션(풀은 항상 표시) |
| `--plain` | `o->plain = 1` | — | |
| `--wide` | `layout_mode = LAYOUT_WIDE` | — | |
| `--narrow` | `layout_mode = LAYOUT_NARROW` | — | |
| `--dump FILE` | `o->dump` | 파일명(`opt_str`) | |
| `--replay FILE` | `o->replay` | 파일명(`opt_str`) | |
| `--record FILE` | `o->record` | 파일명(`opt_str`) | |
| `--replay-speed N` | `o->replay_speed` | 실수(`opt_num`) | 숨은 옵션(도움말에 없음) |
| `--interval SEC` | `o->interval` | 실수(`opt_num`) | |
| `--hz N` | `o->hz` | 정수(`opt_int`) | |
| `--ko` | `use_ko = 1` | — | |
| `--en` | `use_ko = 0` | — | |
| `-` 로 시작하지 않는 단어 | 단어 배열에 보관(최대 2개) | — | §2.4 |
| 그 밖의 `-…` | die `unknown option: %s (see --help)` / `알 수 없는 옵션: %s (--help 참고)` | — | `-`, `--` 단독도 여기 |

값 옵션 검사 순서(한 인자에 대해): `--dump`, `--replay`, `--record`, `--replay-speed`, `--interval`, `--hz` 순으로 이름을 맞춘다.

### 2.3 값 옵션 공통 규칙

| 함수 | 규칙 | 실패 메시지(EN / KO) |
|---|---|---|
| `opt_take` | 이름이 같을 때만 처리. 다음 인자가 없거나 빈 문자열이면 die. 값을 소비(i 증가) | `%s needs a value` / `%s 에 값이 필요합니다` (`%s` = 옵션 이름) |
| `opt_str` | `opt_take` 후, 값이 `-` 로 시작하면 die | 같은 메시지 |
| `opt_num` | `strtod` 로 전체 문자열이 소비되어야 함(`end == v` 또는 `*end != '\0'` 이면 die). `inf`, `nan`, 16진 실수도 strtod 가 받아들이면 통과 → 범위 검사에서 걸러짐 | `%s needs a number` / `%s 에 숫자가 필요합니다` |
| `opt_int` | `strtol(…, 10)` 전체 소비, `INT_MIN..INT_MAX` | `%s needs an integer` / `%s 에 정수가 필요합니다` |

### 2.4 위치 단어 규칙

1. 루프 안: `-` 로 시작하지 않는 인자는 `nword < 2` 인 동안 `word[]` 에 쌓는다. 세 번째 단어는 즉시 die: `unexpected extra argument: %s (target already given)` / `잉여 인자: %s (대상은 이미 지정됨)`. 빈 문자열 `""` 도 단어다.
2. 루프 뒤 배치(`w = 0`):
   1. `--replay` 가 없으면 `target = word[w++]` (단어가 있을 때).
   2. 단어가 남아 있고 `layout_detail` 이 1 이며 `detail_filter` 가 아직 NULL 이면 `detail_filter = word[w++]`.
   3. 그래도 남으면 die. `--replay` 가 있으면 `--replay takes no target: %s` / `--replay 에는 대상을 주지 않습니다: %s`, 없으면 `unexpected extra argument: %s (target already given)` / `잉여 인자: %s (대상은 이미 지정됨)`.
3. 결과: `--replay` 에서는 첫 단어가 필터, 그 밖에는 첫 단어가 대상이고 두 번째가 필터. 단어 순서를 서버 이름·CS 이름으로 추측해 바꾸지 않는다(§2.8 경고 참조).

### 2.5 인자 없음·대상 없음 규칙

`target == NULL && !list && replay == NULL && !help` 일 때:

1. `argv[1..]` 이 전부 `--ko`/`--en` 이거나 인자가 하나도 없으면 `help = 1`.
2. 그 외(다른 옵션만 있고 대상 없음)는 die: `no target: give a db name or a pid (-l lists them, -h for help)` / `대상 없음: DB 이름 또는 pid 를 주십시오 (-l 목록, -h 도움말)`.

`help` 이면 `print_usage()` 를 stdout 에 쓰고 `exit(0)`. 이 판정은 아래 §2.6 검사보다 앞이므로 `-h` 와 잘못된 `--hz` 를 같이 주면 도움말이 나온다(단, 루프 안의 값 형식 오류는 그보다 먼저 die).

### 2.6 사후 검증 (순서대로, 첫 실패에서 die)

| 순서 | 조건 | 메시지(EN / KO) |
|---|---|---|
| 1 | `replay == NULL` 이고 (`hz < 1` 또는 `hz > 10000` 또는 `!(interval >= 0.1)` 또는 `!(interval <= 3600.0)` 또는 `interval * hz < 1.0`) | `bad --hz/--interval` / `--hz/--interval 값 오류` |
| 2 | `!(replay_speed >= 0.0)` 또는 `!isfinite(replay_speed)` | `bad --replay-speed (0 = no pacing, or a positive factor)` / `--replay-speed 값 오류 (0 = 대기 없음, 또는 양수 배율)` |
| 3 | `live && dump != NULL` | `--dump writes one snapshot; it cannot be combined with -i/--interactive` / `--dump는 스냅샷 1회를 기록합니다; -i/--interactive 와 함께 쓸 수 없습니다` |
| 4 | `dump`, `record` 둘 다 있고 `same_file(dump, record)` | `--dump and --record name the same file: %s` / `--dump 와 --record 가 같은 파일입니다: %s` (`%s` = dump) |
| 5 | `replay != NULL` 이고 (`live` 또는 `dump` 또는 `record`) | `--replay cannot be combined with -i/--interactive, --dump or --record` / `--replay는 -i/--interactive/--dump/--record와 함께 쓸 수 없습니다` |

NaN 은 1·2 의 `!(x >= …)` 형태로 걸러진다. `--replay` 에서는 `--hz`/`--interval` 값을 검사하지 않는다(기록 헤더 값을 쓴다).

`same_file(a, b)`:
1. 두 경로가 모두 `stat` 성공 → `st_dev`·`st_ino` 가 같으면 동일.
2. 아니면 `path_canon` 결과 문자열 비교. `path_canon(path)`: 마지막 `/` 앞을 디렉터리(없으면 `.`, 경로가 `/x` 형태면 `/`)로 잡아 `realpath` (실패 시 원문), 결과 = `<realdir>/<basename>`.

### 2.7 print_usage 원문

stdout 에 그대로 출력한다(`fputs`). 영어(`use_ko == 0`):

```text
usage: cub_ctv [options] [<db-name> | <pid>]

  (no arguments)       this help
  -l, --list           list running cub_server processes (pid, db)
  -i, --interactive    live mode: sample at --hz, print every --interval
                       (full screen on a tty, scrolling on a pipe);
                       [r] starts/stops a record <pid>_<db>_<time>.ctv,
                       [l] toggles the language (KO/EN), [q] quits
      --interval SEC   live display period (default 1, 0.1 .. 3600, at least
                       one sample: >= 1/--hz)
      --hz N           live sampling rate, 1..10000 (default 100)
      --detail[=NAME|IX]
                       one vertical record per CS (all, or one by
                       name/index; a word after the target works too:
                       cub_ctv demodb --detail LOG); with -i on a pipe,
                       rows that moved (a terminal shows the summary)
      --trans          transaction table only (no SQL connection);
                       with -i or a live replay, every window
      --wide           owner block always on the row
      --narrow         owner block always on indented lines
                       (default: split by terminal width)
      --plain          no ANSI colors (default: colors on a terminal);
                       the sessions panel keeps reverse/underline
      --json           JSON: one document, or one line per live window
      --record FILE    also write what is shown (snapshot, or every
                       live window) to FILE, compact text
      --replay FILE    show a --record file again, anywhere, no server;
                       view options (--detail, --trans, --json, ...)
                       may differ from the recording; on a terminal
                       keys: space pause, < > one window, + - speed,
                       l language, q quit
      --dump FILE      detail dump: everything cub_ctv can read at this
                       moment, as JSON (snapshot only; not replayable)
      --ko | --en      message language (default: LANG)
  -h, --help           show this help
```

한국어(`use_ko == 1`):

```text
사용법: cub_ctv [옵션] [<DB명> | <pid>]

  (인자 없음)          이 도움말
  -l, --list           실행 중인 cub_server 목록 (pid, db)
  -i, --interactive    라이브 모드: --hz 로 샘플링, --interval 마다 출력
                       (터미널이면 전체 화면, 파이프면 스크롤);
                       [r] 로 <pid>_<db>_<시각>.ctv 녹화 시작/중지,
                       [l] 한/영 전환, [q] 종료
      --interval SEC   라이브 표시 주기 (기본 1, 0.1 .. 3600, 샘플 1회
                       이상: >= 1/--hz)
      --hz N           라이브 샘플링 주파수, 1..10000 (기본 100)
      --detail[=NAME|IX]
                       CS별 세로 레코드 (전체, 또는 이름/번호로 하나;
                       대상 뒤 단어도 됨: cub_ctv demodb --detail LOG);
                       파이프 -i 와 함께면 움직인 행만 (터미널은
                       요약 화면)
      --trans          트랜잭션 표만 (SQL 접속 없음); -i·라이브
                       재생에서는 구간마다
      --wide           소유자 정보를 항상 행 끝에
      --narrow         소유자 정보를 항상 들여쓴 줄로
                       (기본: 터미널 폭에 따라)
      --plain          ANSI 색상 없이 (기본: 터미널이면 색상); 세션 표의
                       반전·밑줄은 유지
      --json           JSON: 1문서, 라이브는 구간마다 1줄
      --record FILE    화면에 보인 내용(스냅샷, 또는 라이브 구간마다)을
                       FILE 에 함께 기록 (작은 텍스트)
      --replay FILE    --record 파일을 어디서나 다시 표시, 서버 불필요;
                       보기 옵션(--detail, --trans, --json 등)은
                       기록 때와 달라도 됨; 터미널 키: space 일시정지,
                       < > 구간 이동, + - 속도, l 한/영, q 종료
      --dump FILE      디테일 덤프: 이 순간 cub_ctv 가 읽을 수 있는 정보
                       전부를 JSON 으로 (스냅샷 전용, 재생 불가)
      --ko | --en      메시지 언어 (기본: LANG)
  -h, --help           이 도움말
```

두 문안 모두 마지막 줄 뒤에 개행 1개로 끝나며 줄 끝 공백은 없다.

### 2.8 서버 목록과 대상 해석

**서버 목록(`list_servers`)**: `/proc` 를 `readdir` 순서(정렬 없음)로 훑어 각 항목 이름을 `atoi` 해 `server_by_pid` 에 넘긴다. 최대 `SERVERS_MAX`(1024)개.

`server_by_pid(pid)`:
1. `pid <= 0` 또는 `/proc/<pid>/comm` 판독 실패 → 아님.
2. comm 의 첫 줄이 정확히 `cub_server` 가 아니면 아님.
3. `/proc/<pid>/cmdline` 을 읽어 첫 NUL 다음 문자열(=두 번째 argv)을 db 로(`text_sanitize` 적용), 없으면 `?`. db 버퍼 128바이트(초과 절단).

**pid 판정(`pid_of_target`)**: `strtol(target, 10)` 이 전체를 소비하고 `0 < p <= INT32_MAX` 이면 pid, 아니면 0(=db 이름).

**`server_match`**: 목록 앞에서부터 `(want > 0 && pid == want) || strcmp(db, target) == 0` 인 첫 항목. 숫자 대상은 같은 이름의 db 와도 맞을 수 있다.

**`resolve_target`**:
1. `found = server_match(...)`.
2. `found >= 0` 이고 대상이 db 이름(`want == 0`)이면 `found` 뒤의 같은 db 이름 수를 `dup` 으로 센다.
3. `found < 0`: 대상이 pid 이고 `server_by_pid(want)` 가 성공하면(목록 한도 밖의 서버) 그 서버 사용. 아니면 실패(-1).
4. `dup > 0`: 대상 문자열(256바이트로 절단 후 `text_sanitize`)로 경고 `'%s': %d servers; using pid %d (give a pid to choose)` / `'%s' 서버가 %d개 있어 pid %d 를 사용합니다 (선택하려면 pid 지정)` — 인자 = 대상, `dup + 1`, 첫 서버 pid. 첫 서버 사용.

**실패 처리(main)**: 해석 실패 시 `o.detail_filter` 가 있고 그 문자열이 서버와 맞으면(`server_match >= 0`) 먼저 경고 `bare words are the target then the --detail filter: use --detail=NAME, or put the target first` / `단어는 대상, 그다음 --detail 필터 순입니다: --detail=NAME 을 쓰거나 대상을 먼저 주십시오`, 이어서 die `no cub_server process found (%s)` / `cub_server 프로세스를 찾지 못했습니다 (%s)` (인자 = 대상).

### 2.9 라이브러리 위치 (`locate_lib`)

`find_lib_base(pid)` 의 규칙:
1. `/proc/<pid>/maps` 를 연다. 실패 → `LIB_NOMAPS`.
2. 줄이 버퍼보다 길면 그 줄 전체를 건너뛴다.
3. `libcubrid.so` 를 포함하고 시작 주소가 파싱되는 줄만. 6번째 필드(공백 포함 가능)가 `/` 로 시작해야 하며 끝의 개행·공백을 뗀다. 끝이 ` (deleted)` 면 떼고 삭제 표시.
4. basename 이 `libcubrid.so` 로 시작하고 바로 뒤가 끝이거나 `.` 이어야 한다.
5. 삭제되지 않은 매핑 중 시작 주소가 가장 낮은 것의 경로·범위(`start-end` 첫 필드)를 채택 → `LIB_OK`.
6. 삭제되지 않은 것이 하나도 없고 삭제된 것만 있으면 그중 최저 주소 → `LIB_DELETED`. 아무것도 없으면 `LIB_NONE`.

`locate_lib` 단계:

| 결과 | 열 파일 |
|---|---|
| `LIB_OK` | `/proc/<pid>/root<lib>` (대상의 마운트 네임스페이스 경유) |
| `LIB_DELETED` | `/proc/<pid>/map_files/<range>` |
| `LIB_NOMAPS` | die `cannot read /proc/<pid>/maps: %s (run as the server's OS user or root)` / `/proc/<pid>/maps 판독 실패: %s (서버의 OS 사용자 또는 root 로 실행)` (인자 = `strerror(errno)`) |
| `LIB_NONE` | die `libcubrid.so not mapped in target` / `대상에 libcubrid.so가 매핑되어 있지 않습니다` |

1. `elf_lookup(file, "csectgl_Critical_sections")`.
2. 결과가 `ELF_NOFILE` 이고 `access(file, F_OK) != 0` 이며 file 에 `/root/` 가 있으면 경로 `lib` 자체로 다시 조회(이후 선택 심볼도 이 경로로).
3. 결과별:

| 결과 | 처리 |
|---|---|
| `ELF_OK` | 계속 |
| `ELF_NOFILE` + file 에 `/map_files/` | die `mapped libcubrid.so was deleted/replaced on disk and its mapping is not readable (map_files needs root) - restart the server or run as root` / `매핑된 libcubrid.so가 디스크에서 교체/삭제되었고 매핑도 읽을 수 없습니다(map_files 는 root 필요) - 서버 재시작 또는 root 로 실행` |
| `ELF_NOFILE` (그 밖) | die `cannot open %s (permission? replaced?)` / `%s 열기 실패 (권한? 교체됨?)` (인자 = lib) |
| `ELF_BAD` | die `%s is not a sane ELF64 image (truncated/corrupt) - refusing` / `%s가 정상 ELF64가 아님(절단/손상) - 중단` |
| 그 밖(`ELF_NOSYM`) | die `csectgl_Critical_sections not found in %s - unsupported build` / `%s에서 csectgl_Critical_sections 심볼 없음 - 미지원 빌드` |

4. `bias = lib_base - load0`.
5. 선택 심볼: `sync_Stats`, `log_Gl`, `_ZN9cubthreadL7ManagerE` — 조회 실패 시 0(해당 뷰가 저하).

### 2.10 기동 순서 (main)

1. 언어 사전 스캔(§2.1), THP 끔.
2. `parse_args` (§2.2~2.6).
3. `color_on = !o.plain && !o.json && isatty(STDOUT_FILENO)`; `detail_filter = o.detail_filter`.
4. `--replay` 이면 `replay_main` 으로 가서 그 반환값으로 종료(§2.13).
5. `nserv = list_servers(...)`. `nserv == SERVERS_MAX` 이면 경고 `more cub_server processes than can be listed: give a pid if yours is missing` / `cub_server 프로세스가 목록 한도를 넘음: 없으면 pid 로 지정하십시오`.
6. `o.list` 또는 대상 없음: `nserv == 0` 이면 die `no cub_server process found` / `cub_server 프로세스를 찾지 못했습니다`. 아니면 `candidate cub_server processes:\n` / `cub_server 후보 프로세스:\n` 후 서버마다 `"  %8d  %s\n"`(pid, db), 반환 0. (`-l` 과 대상을 같이 줘도 목록이 이긴다.)
7. `resolve_target` (§2.8).
8. `locate_lib` (§2.9).
9. `mem_open(pid)` 실패 → die `cannot open /proc/<pid>/mem: %s` / `/proc/<pid>/mem 열기 실패: %s` (strerror).
10. `!json` 이면 대상 줄: lib 를 4096바이트로 복사·`text_sanitize` 후 `target: pid=%d db=%s lib=%s\n` / `대상: pid=%d db=%s lib=%s\n`.
11. `cs_layout_probe` 실패 → die `cannot validate SYNC_CRITICAL_SECTION layout (%s) - refusing to guess` / `SYNC_CRITICAL_SECTION 레이아웃 검증 실패 (%s) - 추측하지 않고 중단` (인자 = 거부 사유 id).
12. `tran_subsystem_init(bias, sym_loggl)`: `why = sym_loggl == 0 ? "log_Gl symbol" : tran_probe(bias + sym_loggl)`; `why == NULL` 이면 `tran_lock_clientids(tran_geom.ntran, 0)`; `tran_geom.all_tdes == 0` 이면 `DEG_TRAN` 설정, `tran_refused = why` (NULL 이면 `"transaction table probe"`).
13. `target_pid = pid`.
14. `thr_skipped = TRANS_TABLE_ONLY(o.trans, o.record) && o.dump == NULL`. 아니면 `thr_keep_image = (o.dump != NULL)`, `thr_probe(sym_thr ? bias + sym_thr : 0, pid)`.
15. `!json` 이면 레이아웃 줄(§2.12), 스레드 수 = `thr_skipped ? -1 : thr_geom.count`.
16. 기록 헤더 `VIEW_HDR h` 를 0 으로 채우고 `pid, live, hz, interval, thr_count = thr_geom.count, degraded, lo, db, lib(원문, 미정화), tran_refused(없으면 "")` 설정.
17. `--record` 이면 `view_record_open(record, &h, 0)` 실패 시 die `cannot create record %s` / `기록 파일 %s 생성 실패`.
18. 모드 분기(§2.11).

### 2.11 모드 분기와 실행 경로

| 조건(위에서부터 첫 일치) | 경로 | 출력 | 반환 |
|---|---|---|---|
| `--replay` | `replay_main` | §2.13 | §2.14 |
| `-l` 또는 대상 없음 | 서버 목록 | §2.10-6 | 0 |
| `-i` | `live_mode(bias+sym_cs, &h, json, trans, record, thr_skipped)` | §12 | 0 / 1 |
| `thr_skipped` (=`--trans`, 기록·덤프 없음) | `view_capture_trans(0.0)` → `render_snapshot` | 트랜잭션 표만 | 0 / 1 |
| 스냅샷 | 아래 단계 | `render_snapshot` | 0 / 1 |

스냅샷 단계:
1. `cs_read_rows` 실패 → `die_target_gone()` = die `target read failed (server gone?)` / `대상 판독 실패(서버 종료?)`.
2. `view_capture(&lo, rows, NULL, 0.0)`.
3. `--dump` 이면 `detail_dump_write(...)` (풀 순회가 프레임 풀 그룹도 채움). 아니면 `sym_pool != 0` 이고 (`--record` 또는 (`!trans && !json && !layout_detail`)) 일 때만 `view_capture_pool(bias + sym_pool)`.
4. `view_record_frame()` (기록이 없으면 무동작). `--record` 이면 `view_record_close() != 0` 시 die `cannot write record %s` / `기록 파일 %s 쓰기 실패`.
5. `render_snapshot`.

**`render_snapshot(o, lo, pid, db)`**:

| 조건 | 출력 | 반환 |
|---|---|---|
| `trans && json` | `json_doc(pid, db, lo, window=0, rows=0, trans=1)` | `view.tran_avail ? 0 : 1` |
| `trans` | `print_trans()` | 0 이면 0, 아니면 1 |
| `json` | `json_doc(pid, db, lo, 0, 1, 1)` | 0 |
| `layout_detail` | `print_detail(lo, view.rows)` | 0 |
| 그 밖 | `print_snapshot` → `print_thread_waits(stdout, 0)` → `printf("\n")` → `print_trans()`(결과 무시) → `print_view_pool()` | 0 |

`--dump` 는 덤프 파일을 쓴 뒤에도 위의 일반 출력을 한다.

### 2.12 레이아웃 줄 (`print_layout_line(lo, threads)`)

1. `layout: stride=%dB count=%d stats_off=%d (runtime-probed)` / `레이아웃: stride=%dB count=%d stats_off=%d (런타임 검증 통과)` (stride, count, off_stats).
2. `threads >= 0` 이고 `DEG_THREAD` 가 없으면 `"  threads=%d"`.
3. `deg_names` 순서대로 켜진 비트마다 `" [%s (%s)"`(label, id); `DEG_TRAN` 이고 `tran_refused != NULL` 이면 `": %s"`; `DEG_TRAN_CAP` 이면 `": 65536"`; 그리고 `"]"`.
4. `"\n"`.

`deg_names` (영어 고정, 이 순서):

| 비트 | id | label |
|---|---|---|
| `DEG_TRID` 0x1 | `I-8` | `trid not validated` |
| `DEG_TRAN_CAP` 0x2 | `cap` | `tran table capped` |
| `DEG_TRAN` 0x20 | `I-6/I-7` | `no transaction table` |
| `DEG_THREAD` 0x4 | `I-14` | `no thread entries` |
| `DEG_THR_TRAN` 0x8 | `I-15` | `no tran` |
| `DEG_THR_ID` 0x10 | `I-16` | `no owner id` |
| `DEG_THR_TID` 0x40 | `I-17` | `no os tid` |
| `DEG_THR_RESUME` 0x80 | `I-18` | `no wait reason` |
| `DEG_THR_NEXT` 0x100 | `I-19` | `no wait chain` |
| `DEG_THR_STATUS` 0x200 | `I-20` | `no thread state` |

### 2.13 재생 진입 (`replay_main`)

1. `view_replay_open(replay, &h)` (형식 오류 처리는 기록 명세).
2. `deg_set(~0u, 0)` 후 `deg_set(h.degraded, 1)`; `tran_refused = h.tran_refused[0] ? h.tran_refused : NULL`.
3. `!json` 이면 대상 줄 `target: pid=%d db=%s lib=%s\n` / `대상: pid=%d db=%s lib=%s\n`(라이브 §2.10-10 과 같은 `M` 쌍, `--ko` 재생이면 `대상:`; 인자 = `h.pid`, `h.db`, `h.lib` — 헤더를 읽을 때 `rec_unescape` 가 이미 `text_sanitize` 했으므로 이 경로에서는 추가 정화 없음) 과 `print_layout_line(&h.lo, h.thr_count)`.
4. `h.live` 이면 `live_replay(&h, replay_speed, json, trans)` 반환(§13).
5. 스냅샷 기록: `view_replay_next() != 0` 이면 die `record %s holds no frame` / `기록 %s 에 프레임이 없습니다`. 아니면 `render_snapshot(o, &h.lo, h.pid, h.db)`.

`-l` 과 `--replay` 를 같이 주면 재생이 실행된다(목록 분기보다 앞).

### 2.14 종료 코드

| 코드 | 경우 |
|---|---|
| 0 | 도움말, 서버 목록, 정상 스냅샷, 라이브 정상 종료(q, Ctrl-C/TERM/HUP 로 루프 탈출), 재생 정상 종료, 대화형 재생 `q`/stdin EOF |
| 1 | 모든 `die` (`exit(1)`), `--trans` 인데 트랜잭션 표를 읽을 수 없음(스냅샷·라이브·재생 모두), 스냅샷 `--trans` 텍스트에서 `print_trans` 가 -1 |
| 128 + sig | `restore_on_exit` 가 설치된 상태(전체 화면, 대화형 재생, 표 단독 키 모드)에서 SIGINT/SIGTERM/SIGHUP/SIGPIPE 를 `on_signal` 이 받은 경우(`_exit`). 단 라이브 루프는 이후 INT/TERM/HUP 를 `on_stop` 으로 덮으므로 SIGPIPE 만 해당 |
| 시그널 기본 동작 | 핸들러가 없는 경로(파이프 스크롤 재생, 스냅샷)에서의 시그널 |

---

## 3. 메시지 계층 (ctv_msg.c)

| 함수 | 출력 | 종료 |
|---|---|---|
| `die(fmt_en, fmt_ko, arg)` | arg(NULL 이면 "")를 4096바이트로 복사·`text_sanitize` → `screen_restore()` → stderr 에 `"cub_ctv: "` + `M(fmt_en, fmt_ko)` 를 `safe` 로 서식화 + `"\n"` | `exit(1)` |
| `warn(en, ko)` | stderr `"cub_ctv: %s\n"` | — |
| `warnf(fmt_en, fmt_ko, ...)` | stderr `"cub_ctv: "` + 서식 + `"\n"` (인자 정화는 호출자 책임) | — |

`die` 의 서식은 `%s` 하나만 받으며 인자가 NULL 이어도 빈 문자열로 서식화된다(서식에 `%s` 가 없으면 무시).

---

## 4. 텍스트 폭 규칙 (ctv_text.c, ctv_rows.c 의 visible_len)

### 4.1 UTF-8 판정

| 함수 | 규칙 |
|---|---|
| `utf8_seq_len(c)` | `c < 0x80` → 1; `(c & 0xE0) == 0xC0 && c >= 0xC2` → 2; `(c & 0xF0) == 0xE0` → 3; `(c & 0xF8) == 0xF0 && c <= 0xF4` → 4; 그 밖 0 |
| `utf8_valid_len(s)` | 위 길이 n 에 대해 이후 n-1 바이트가 모두 `10xxxxxx` 이면 n, 아니면 0 (과잉 길이·서로게이트 검사 없음) |
| `printable_seq_len(s)` | ASCII 는 `0x20..0x7E` 만 1(그 밖 0). 비ASCII 는 valid 길이 n 이 2 미만이거나 C1(`C2 80..C2 9F`)이면 0, 아니면 n |

### 4.2 칸 수

| 함수 | 규칙 |
|---|---|
| `utf8_cols(s, len)` | `len < 3` → 1. 3/4바이트는 코드포인트를 복원해 아래 범위면 2, 아니면 1 |
| `seq_cols(s, &len)` | valid 길이 0 이면 `len = 1`, 1칸; 아니면 `len = n`, `utf8_cols` |
| `str_cols(s)` | 문자열 전체의 `seq_cols` 합 |
| `visible_len(s)` (rows.c) | `\033` 를 만나면 다음 `m` 까지(포함) 0칸으로 건너뜀(`m` 없이 끝나면 종료), 나머지는 `seq_cols` 합 |

2칸 범위(양끝 포함): `1100–115F`, `2E80–303E`, `3041–33FF`, `3400–4DBF`, `4E00–9FFF`, `A000–A4CF`, `AC00–D7A3`, `F900–FAFF`, `FE30–FE4F`, `FF00–FF60`, `FFE0–FFE6`, `1F300–1F64F`, `1F900–1F9FF`, `20000–3FFFD`. 2바이트 문자(예: `·` U+00B7)는 항상 1칸.

### 4.3 dw_pad (칸 기준 채우기·자르기)

`dw_pad(out, n, s, width)`:
1. `o = 0, cols = 0`. `*s != 0 && o + 4 < n` 인 동안: `w = seq_cols(s, &len)`; `cols + w > width` 이면 중단; 아니면 len 바이트 복사, `cols += w`.
2. `cols < width && o + 1 < n` 인 동안 공백 추가.
3. NUL 종료, 바이트 수 반환.

결과: 넓은 글자가 경계에 걸리면 그 글자를 빼고 공백 1칸으로 채운다. 폭보다 짧으면 오른쪽 공백. 깨진 바이트는 1바이트 1칸으로 그대로 복사.

### 4.4 buf_cat, utf8_cut

`buf_cat(buf, n, &pos, fmt, ...)`: `pos + 1 >= n` 이면 무동작. `vsnprintf(buf + pos, n - pos)`. 다 들어가면 `pos += w`. 넘치면 `pos = utf8_cut(buf, n - 1)`, `buf[pos] = 0` (UTF-8 시퀀스 중간에서 자르지 않음).

`utf8_cut(s, ln)`: 끝에서 최대 3개의 연속 바이트를 거슬러 리드 바이트를 찾고, 그 시퀀스가 `ln` 을 넘으면 리드 앞에서 자른다.

### 4.5 text_sanitize

외부 문자열(서버 db 이름, 라이브러리 경로, die 인자, 대상 문자열 경고)에 적용. `printable_seq_len` 이 0 인 위치는: valid UTF-8 시퀀스(C1)이면 그 길이만큼(문자열 끝 전까지) 각 바이트를 `?` 로, 아니면 1바이트를 `?` 로.

---

## 5. 터미널 (src/ctv_term.c)

### 5.1 크기

| 함수 | 규칙 |
|---|---|
| `term_columns()` | 정적 캐시. 처음 호출이거나 SIGWINCH 후(`winch_cols`)에만 다시 계산: 기본 0; stdout 이 tty 가 아니면 0(파이프는 폭 무제한); tty 이면 `COLUMNS` 의 `atoi > 0` 값, 아니면 `TIOCGWINSZ` 의 `ws_col > 0`, 아니면 0 |
| `term_rows()` | 정적 캐시, 재계산 조건 동일(`winch_rows`). stdout 이 tty 이고 `TIOCGWINSZ` 의 `ws_row > 0` 이면 그 값, 아니면 0. `LINES` 는 보지 않음 |

`COLUMNS` 는 stdout 이 tty 일 때만 효력이 있고, 캐시되므로 실행 중 환경 변경은 다음 SIGWINCH 전까지 반영되지 않는다.

### 5.2 색상 팔레트 (`clr(which)`)

`color_on == 0` 이면 항상 `""`. 켜져 있으면:

| 역할 | 값 | 쓰임 |
|---|---|---|
| `CLR_OFF` | `\033[0m` | 해제 |
| `CLR_HEAD` | `\033[1;38;5;73m` | 헤더(굵은 청록) |
| `CLR_WRITER` | `\033[38;5;150m` | `1 writer` 상태 |
| `CLR_WAIT` | `\033[1;38;5;203m` | 대기·심각도 ≥3·`[REC]` |
| `CLR_OWNER` | `\033[38;5;222m` | 소유자 블록·심각도 2 |
| `CLR_WQ` | `\033[38;5;183m` | 대기 큐 |
| `CLR_MUTED` | `\033[38;5;245m` | 안내 |
| `CLR_BAR_LOW` | `\033[38;5;150m` | 점유 막대 < 50% |
| `CLR_BAR_MID` | `\033[38;5;221m` | 50% ≤ 점유 < 80% |
| `CLR_BAR_HIGH` | `\033[38;5;203m` | 점유 ≥ 80% |
| `CLR_PANEL_OWNER` | `\033[7;38;5;222m` | 세션 패널: 보유만(반전) |
| `CLR_PANEL_WAITER` | `\033[4;38;5;183m` | 세션 패널: 대기(밑줄) |

### 5.3 원시 모드·전체 화면

| 함수 | 동작 |
|---|---|
| `tty_raw_on()` | stdin `tcgetattr` 저장 → `c_lflag &= ~(ICANON\|ECHO)`, `VMIN = 0`, `VTIME = 0` 로 `TCSANOW` 적용. 성공 시 `tty_raw = 1`, 0 반환. ISIG 는 유지(Ctrl-C 는 시그널) |
| `keys_enter()` | `isatty(STDIN_FILENO) && tty_raw_on() == 0` 이면 0, 아니면 -1 |
| `screen_enter()` | `write(1, "\033[?1049h\033[?25l", 14)` (대체 화면 + 커서 숨김), `screen_active = 1`, `restore_on_exit()`, `SIGWINCH → on_winch` |
| `screen_restore()` | `tty_raw` 면 저장된 termios 복원·플래그 해제; `screen_active` 면 `write(1, "\033[?25h\033[?1049l", 14)`·플래그 해제. 재진입 안전(플래그 기반) |
| `restore_on_exit()` | 최초 1회만: `atexit(screen_restore)`; SIGINT/SIGTERM/SIGHUP/SIGPIPE → `on_signal`; SIGTSTP → `on_tstp`; SIGCONT → `on_cont` |
| `live_stop_on_signals()` | SIGINT/SIGTERM/SIGHUP → `on_stop` (덮어씀) |

시그널 핸들러:

| 핸들러 | 동작 |
|---|---|
| `on_winch` | `winch_cols = winch_rows = winch_key = 1` |
| `on_signal(sig)` | `screen_restore()` → `_exit(128 + sig)` |
| `on_stop` | `live_stop = 1` (라이브 루프가 다음 틱 판단에서 종료) |
| `on_tstp` (Ctrl-Z) | 현재 `screen_active`·`tty_raw` 를 `screen_wanted`·`keys_wanted` 에 기억 → `screen_restore()` → SIGTSTP 기본 동작으로 되돌리고 `raise(SIGTSTP)` |
| `on_cont` (fg) | SIGTSTP 를 다시 `on_tstp` 로; `keys_wanted && !tty_raw` 면 `tty_raw_on()`; `screen_wanted && !screen_active` 면 `\033[?1049h\033[?25l` 쓰고 `screen_active = 1`; 두 wanted 를 0 으로(이후 단독 SIGCONT 는 무효과); `winch_key = winch_cols = winch_rows = 1` (다음 키 대기에서 KEY_RESIZE → 재그리기) |

`term_stop_requested()` = `live_stop != 0`.

### 5.4 키 읽기 (`key_read(wait)`)

반환: 바이트 값, `KEY_*`, 또는 0(시간 초과/정지).

1. `wait >= 0` 이면 그 초를 timespec 으로, 음수면 무한 대기.
2. SIGWINCH, SIGINT, SIGTERM, SIGHUP 을 `sigprocmask(SIG_BLOCK)`.
3. 이미 `winch_key` 또는 `live_stop` 이 켜져 있으면: `winch_key = 0`, 마스크 복원, `live_stop` 이면 0, 아니면 `KEY_RESIZE`.
4. `pselect(stdin, 원래 마스크)` 로 대기(대기 중에만 시그널 허용) 후 마스크 복원.
5. `rc < 0 && errno == EINTR && winch_key` → `winch_key = 0`, `KEY_RESIZE`.
6. `rc < 0` (그 밖) → `live_stop ? 0 : KEY_IGNORED`.
7. `rc == 0` → 0 (시간 초과).
8. 1바이트 `read`. `n < 0` 이고 EINTR/EAGAIN → `KEY_IGNORED`. `n <= 0` → `KEY_EOF`.
9. ESC(0x1b): 2바이트를 각각 `select` 50 ms 후 1바이트씩 읽음. 어느 하나라도 실패하면 `KEY_IGNORED`(단독 Esc). `c[1] == '['` 이고 `c[2] == 'C'` → `KEY_FWD`, `'D'` → `KEY_BACK`, 그 밖 `KEY_IGNORED`.
10. 바이트 매핑:

| 바이트 | 반환 |
|---|---|
| `,` `<` `b` | `KEY_BACK` (259) |
| `.` `>` `n` | `KEY_FWD` (260) |
| `=` | `'+'` |
| `_` | `'-'` |
| `< 0x20` 또는 `0x7f` | `KEY_IGNORED` (256) |
| 그 밖 | 그 바이트(대소문자 구분: `Q` 는 `q` 가 아님) |

| 코드 | 값 | 의미 |
|---|---|---|
| `KEY_IGNORED` | 256 | 동작 없음(재그리기, 진행 안 함) |
| `KEY_EOF` | 257 | stdin 닫힘/실패 |
| `KEY_RESIZE` | 258 | 크기 변경(또는 fg 복귀) |
| `KEY_BACK` | 259 | 한 구간 뒤로 |
| `KEY_FWD` | 260 | 한 구간 앞으로 |

### 5.5 시간

| 함수 | 규칙 |
|---|---|
| `mono_s()` | `CLOCK_MONOTONIC` 초(double) |
| `sleep_until(t)` | SIGINT/TERM/HUP 차단 → `rest = t - mono_s() > 0 && !live_stop` 이면 `pselect(0, …, rest, 원래 마스크)` → 마스크 복원 |
| `clock_hms(ts)` | `localtime_r` 후 `"%H:%M:%S"` |

---

## 6. 판정 수치 (src/ctv_verdict.c)

모든 계산은 현재 프레임 `view` 만 사용한다.

### 6.1 CS 설명·점검 항목 표 (`cs_info`)

`cs_info` 의 설명(en/ko)은 요약 화면 "보호 대상" 열 폭(`dw_pad(…, 24)`) 안에 들도록 24칸 이하로 둔다(한글 1자 = 2칸, `·`·`→` = 1칸).

| name | 설명 EN | 설명 KO | 점검 EN | 점검 KO |
|---|---|---|---|---|
| `WFG` | wait-for graph (unused) | 대기 그래프 (미사용) | - | - |
| `LOG` | log flush/WAL/reads | 로그 플러시·WAL·읽기 | log disk write latency (await), log_buffer_size, group commit | 로그 디스크 쓰기 지연(await), log_buffer_size, 그룹 커밋 |
| `LOCATOR_CLASSNAME_TABLE` | table name lookup | 테이블 이름 찾기 | connection rate, DDL frequency | 접속 빈도, DDL 빈도 |
| `QPROC_QUERY_TABLE` | query table alloc (boot) | 질의 표 할당 (기동 시만) | - | - |
| `QPROC_LIST_CACHE` | query result cache | 질의 결과 캐시 | list cache hit rate | 리스트 캐시 적중률 |
| `DISK_CHECK` | sector reserve/checkdb | 섹터 예약 (checkdb 배타) | file/temp growth, checkdb running | 파일·임시파일 확장, checkdb 실행 |
| `CNV_FMT_LEXER` | format lexer (unused) | 포맷 렉서 (미사용) | - | - |
| `HEAP_CHNGUESS` | class version guess | 클래스 캐시 버전 추측 | class fetches by many short transactions, DDL | 짧은 트랜잭션 다수의 클래스 객체 전달, DDL |
| `TRAN_TABLE` | transaction slots | 트랜잭션 슬롯 관리 | connection rate, pooling | 접속 빈도, 커넥션 풀 |
| `CT_OID_TABLE` | class to catalog OID map | 클래스→카탈로그 OID 캐시 | DDL frequency | DDL 빈도 |
| `HA_SERVER_STATE` | HA state | HA 상태 | HA role changes | HA 역할 변경 |
| `COMPACTDB_ONE_INSTANCE` | compactdb | compactdb | compactdb running | compactdb 실행 중 |
| `ACL` | allowed client IP list | 접속 허용 IP 목록 | access_ip_control reload | access_ip_control 재적재 |
| `PARTITION_CACHE` | partition cache | 파티션 캐시 | partition DDL/DML | 파티션 DDL/DML |
| `EVENT_LOG_FILE` | event log file | 이벤트 로그 파일 | lock timeout/deadlock/many-ioreads events | 락 타임아웃·교착·대량 I/O 이벤트 기록 |
| `TRACE_LOG_FILE` | trace log file | 트레이스 로그 파일 | slow queries logged (sql_trace_slow_msecs) | 슬로 쿼리 기록 (sql_trace_slow_msecs) |
| `LOG_ARCHIVE` | archive log access | 아카이브 로그 접근 | archive readers (backup, CDC), log_max_archives | 아카이브 소비자(백업, CDC), log_max_archives |
| `ACCESS_STATUS` | login record | 로그인 기록 | connection rate | 접속 빈도 |

- `cs_desc(name)`: 정확 일치 항목의 설명, 없으면 `name` 그대로.
- `cs_hint(name)`: 항목의 점검, 없으면 `the holding session` / `쥔 세션`.

### 6.2 구간 비율 (`cs_rates`)

입력: 현재 행 c, 구간 시작 행 p, 점유 샘플 occ, 대기 샘플 wai, 샘플 수, dt.

| 필드 | 식 |
|---|---|
| `de` | `counter_delta(c->nenter, p->nenter, reset)` |
| `dw` | `counter_delta(c->nwait, p->nwait, reset)` |
| `enter_s` | `dt > 0 ? r->de / dt : 0.0` (u32 ÷ double → 실수 나눗셈) |
| `wait_s` | `dt > 0 ? r->dw / dt : 0.0` |
| `occ_pct` | `samples > 0 ? 100.0 * occ / samples : 0.0` |
| `wait_pct` | `samples > 0 ? 100.0 * wai / samples : 0.0` |

`reset = stats_reset(c, p)` = `c->total_ms < p->total_ms ∧ c->nenter < p->nenter`. 진입 수는 리셋과 32bit 넘침 모두에서 줄고, 누적 획득 시간(넘침 없음)은 리셋에서만 준다. 둘 다 줄어야 리셋으로 보므로 누적 시간만 찢겨 작게 읽힌 경우는 리셋이 아니다. `counter_delta(now, before, reset)`(static): `reset` 이면 `now`, 아니면 `(uint32_t)(now - before)`(32bit 넘침 포함). 리셋 직전 값이 2^31 을 넘었어도 리셋으로 판정한다.

**나눗셈 규칙(이 절과 §6.3·§6.7 전체)**: 백분율·비율은 모두 `100.0`(double) 리터럴로 시작하므로 C 의 왼쪽 결합에 따라 첫 곱셈부터 double 로 승격되어 **실수 나눗셈**이다. 정수 나눗셈은 판정 수치 어디에도 없다. 예: occ 2, samples 3 → 66.666…(`%5.1f` 로 ` 66.7`, JSON `%.2f` 로 `66.67`).

### 6.3 CS 수치 (`cs_figures(i, dt)`) 와 심각도

`c = view.rows[i]`, `p = view.prev[i]`, `dms = stats_reset(c, p) ? c->total_ms : c->total_ms - p->total_ms` (double).

| 필드 | 식 |
|---|---|
| `ix` | i |
| `occ_pct`, `wait_pct`, `enter_s`, `wait_s` | `cs_rates(c, p, view.st.occ[i], view.st.wai[i], view.st.samples, dt)` |
| `avg_ms` | `de > 0 && dms > 0 ? dms / de : 0.0` (진입당 획득 ms, double ÷ u32) |
| `lost_s` | `dms > 0 ? dms / 1000.0 / dt : 0.0` (dt = 0 이면 무한대) |

심각도(위에서부터 첫 일치):

| 값 | 조건 |
|---|---|
| 4 | `lost_s >= 1.0` 또는 `wait_pct >= 75.0` |
| 3 | `lost_s >= 0.3` 또는 `wait_pct >= 40.0` |
| 2 | `dw > 0` 또는 `view.st.wai[i] > 0` 또는 `lost_s >= 0.05` |
| 1 | `de > 0` 또는 `view.st.occ[i] > 0` |
| 0 | 그 밖(유휴) |

### 6.4 순위 (`cs_figures_ranked`, `cmp_fig`)

모든 CS(0..count-1) 의 수치를 구해 `qsort`. 비교 순서:
1. severity 내림차순
2. `lost_s` 내림차순
3. `wait_s` 내림차순
4. `occ_pct` 내림차순
5. `ix` 오름차순 (완전 순서이므로 qsort 불안정성 영향 없음)

### 6.5 보유자 (`holders_build`, 프레임당 1회, 캐시 키 = `view.seq`)

초기화(모든 CS): `hold_tran = -2`, `hold_cpid = -1`, `hold_samples = 0`, `hold_thr0 = -1`, `hold_thr0_samples = 0`, `first = ACT_MAX`, `bs = 0`.

1. `view.st.act[k]` 를 k 순으로 보며 `kind == ACT_HELD` 이고 `0 <= cs < MAX_CS` 인 것만 인덱스 목록에 넣는다.
2. 같은 순회에서 엔진 스레드 보유자: `tran == 0 && thr >= 0 && samples > bs[cs]` (엄격 초과 → 먼저 본 것이 동점 승) 이면 `bs = hold_thr0_samples = samples`, `hold_thr0 = thr`.
3. 인덱스 목록을 (cs, tran, cpid, 원래 위치) 오름차순 정렬.
4. 같은 (cs, tran, cpid) 묶음의 samples 를 합산(long long). `sum > hold_samples[cs]` 이거나 (`sum ==` 이고 묶음의 첫 위치 < `first[cs]`) 이면 갱신: `hold_samples = sum`, `hold_tran = tran`, `hold_cpid = cpid`, `first = 묶음 첫 위치`. → 합계 최대, 동점이면 구간에서 먼저 목격된 클라이언트. 슬롯이 구간 중 다른 클라이언트에게 넘어가면 두 클라이언트는 따로 합산된다.

질의 함수:

| 함수 | 반환 |
|---|---|
| `holder_of(i, &s, &cpid)` | `hold_tran[i]` (-2 = 없음, 0 = 엔진, ≥1 = 클라이언트 tran). `s` = 합계(INT_MAX 로 포화), `cpid = hold_cpid[i]` |
| `holder_thread_of(i, &s)` | `hold_thr0[i]` (-1 = 없음), `s = hold_thr0_samples[i]` |
| `daemon_name(i)` | `hold_thr0[i] >= 0` 이고 `view_os_of(slot)` 가 있으면 그 `comm`, 아니면 `""` |

### 6.6 지속 이력 (프레임 번호 표)

| 요소 | 정의 |
|---|---|
| 저장 | `hist_top[HIST_CAP]`, `hist_seq[HIST_CAP]` (seq+1 저장, 0 = 빈칸), `hist_kept` (한 번이라도 push 하면 1) |
| `history_push(seq, top)` | `seq < 0` 이면 무시. 칸 `seq % 4096` 에 `hist_seq = seq + 1`, `hist_top = top`, `hist_kept = 1` |
| `history_at(s)` | `s >= 0` 이고 `hist_seq[s % 4096] == s + 1` 이면 `hist_top`, 아니면 -2(모름) |
| `history_export(seq, back, top)` | `hist_kept == 0` 이면 -1. 아니면 b = 1..9 에서 `history_at(seq - b) != -2` 인 것을 (b, top) 으로 채워 개수 반환 |
| `history_import(seq, n, back, top)` | `seq - b` (b = 1..9, `seq - b >= 0`) 칸을 비운 뒤, 각 (back, top) 을 `history_push(seq - back, top)`. "비운다" = `hist_seq[(seq-b) % 4096] = 0` 만(`hist_top` 은 그대로, 읽히지 않음). import 자체는 `hist_kept` 를 바꾸지 않으며 `n == 0` 이면 push 가 없어 `hist_kept` 도 그대로다 |
| `history_count(seq, top_ix, &span)` | b = 0..9 에서 알려진 칸 수 = span, 그중 top 이 `top_ix` 인 수 반환 (현재 프레임 포함) |

push 되는 값: 요약 화면이 그려질 때 `fig[0].severity >= 2 ? fig[0].ix : -1` (-1 = 유휴 구간도 한 칸 차지). 같은 seq 를 다시 그리면(재그리기·일시정지) 같은 칸을 덮어쓸 뿐 새 항목이 아니다. `view_capture` 가 캡처 시점에 `history_export` 를 프레임(`hist_known`, `hist_n`, `hist_back`, `hist_top`)에 담아 기록·재생에 실어 나른다(`hist_known = 반환 >= 0`, 음수면 `hist_n = 0`).

### 6.7 판정 (`verdict_compute(top, second, &v)`)

`c = view.rows[top->ix]`. v 를 0 으로 채운 뒤:

| 필드 | 계산 |
|---|---|
| `top` | `*top` |
| `has_second` / `second` | `second != NULL && second->severity >= 2` 이면 1 과 `*second` |
| `same`, `span` | `history_count(view.seq, top->ix, &span)` |
| `holder`, `holder_cpid` | `holder_of(top->ix, &hs, &v->holder_cpid)` |
| `held_pct` | `view.st.samples > 0 ? 100.0 * hs / view.st.samples : 0.0` (samples 0 이면 0, NaN 없음) |
| `one_thr` | `holder == 0 && holder_thread_of(top->ix, &thr_samples) >= 0` |
| `thr_pct` | `one_thr && view.st.samples > 0 ? 100.0 * thr_samples / view.st.samples : held_pct` (samples 0 이면 `held_pct` = 0) |
| `nwaiters` | act 중 `cs == top->ix && kind == ACT_WAIT && tran >= 1` 인 항목 수 (클라이언트 tran 은 (cs,kind,tran,cpid) 당 1항목 — 구간 중 슬롯이 재사용되면 두 클라이언트가 따로 셈) |
| `wtran[]`, `wsamples[]`, `nlisted` | 위 항목을 act 순서로 앞에서 4개 |
| `more_waiters` | 5번째 이상이 있으면 1 |
| `waited_pct` | `nwaiters > 0 && view.st.samples > 0` 이면 `100.0 * waited / nwaiters / view.st.samples`, 아니면 0(memset 값). `waited`(double) = **위 조건의 모든 항목**(`nwaiters` 개 전부, 나열된 4개만이 아님)의 `samples` 합 |
| `os` | act 를 k 순으로 보며 `cs == top->ix && kind == ACT_HELD && thr >= 0` 인 항목의 `o = view_os_of(thr)` 가 NULL 이 아니고 `o->samples > bs`(bs 초기 0, 엄격 초과 → 먼저 본 것이 동점 승)이면 `os = o`, `bs = o->samples`. 끝에 `os != NULL && os->samples <= 0` 이면 NULL(초기 bs 0 때문에 실제로는 생기지 않음) |
| `dsk_pct` | `100.0 * os->dsk / os->samples` (실수) |
| `run_pct` | `100.0 * os->run / os->samples` (실수) |
| `cpu_pct` | `seen_s = os->span_ms / 1000.0`; `seen_s >= 0.05` 이면 `100.0 * (double)(ticks1 - ticks0) / os_clock_ticks() / seen_s`, 아니면 0.0. `os_clock_ticks()` 는 `long` = `sysconf(_SC_CLK_TCK)` (≤0 이면 100) |

`dsk_pct`·`run_pct`·`cpu_pct` 는 `os != NULL` 일 때만 계산되고, 아니면 0(memset). samples 0 프레임(스냅샷형)에서도 위 가드 덕분에 NaN 이 생기지 않는다(요약 화면은 라이브·대화형 재생에서만 그려지므로 samples ≥ 1 이 정상이다).

`bound` (위에서부터 첫 일치):

| 값 | 조건 |
|---|---|
| `VB_NONE` | `os == NULL && !view.disk_avail` |
| `VB_DISK` | (`os != NULL && dsk_pct >= 50.0`) 또는 (CS 이름이 `LOG` 로 시작(`strncmp(name, "LOG", 3)`, `LOG_ARCHIVE` 포함) && `view.disk_avail` && `view.disk_await_ms >= 5.0`) |
| `VB_CPU` | `os != NULL && (run_pct >= 80.0 \|\| cpu_pct >= 80.0)` |
| `VB_SHORT` | `os != NULL` |
| `VB_UNKNOWN` | 그 밖(장치만 앎) |

---

## 7. 요약 화면 (src/ctv_screen.c `screen_summary`)

호출: `screen_summary(lo, dt, hz, interval, pid, db, hhmmss)`. 터미널 전체 화면(라이브·대화형 재생)에서만 쓰인다.

### 7.1 상단 블록 측정

1. `rows = term_rows()`.
2. `open_memstream` 으로 메모리 스트림을 연다. 실패하면 stdout 에 직접 쓴다(측정 없음).
3. 상단 블록(§7.2~7.6)을 그 스트림에 쓴다.
4. 스트림이 있으면:
   1. `keep = rows > 0 ? rows - 1 - 2 - (view.tran_avail ? 1 : 0) - 1 : -1`.
   2. `rows > 0 && keep < 1` 이면 `keep = 1`, `tiny = 1` (아주 작은 터미널: 제목과 하단만).
   3. 스트림을 닫고 버퍼의 `\n` 을 앞에서부터 센다(`top`). `top == keep` 이 되는 `\n` 뒤에 아직 내용이 있으면 그 다음 위치를 `cut` 으로. 루프 끝의 `top` = 버퍼 전체의 개행 수.
   4. `cut` 바이트만 stdout 에 쓴다.
   5. 잘렸고(`cut < top_len`) `!tiny` 이면 MUTED `"  ... (terminal too short: enlarge it for the rest)"` / `"  ... (터미널이 작음: 늘리면 나머지 표시)"` OFF `"\n"` 을 쓰고 `top = keep + 1`.
5. 세션 패널(§7.7): `view.tran_avail && !tiny` 일 때만.
   - `panel = (스트림 없음 && rows > 0) ? -1 : rows > 0 ? rows - 1 - top - 2 - 4 : LIVE_TRANS_ROWS`.
   - `panel > 24` 이면 24, `panel < 1` 이면 -1(개수 줄만).
6. 하단(§7.8).

### 7.2 제목 줄

```
\033[H\033[2J
[REC 표시] HEAD "cub_ctv - %s (pid %d)  %s  window %.1fs  (%dHz, samples=%d)" OFF "\n\n"
```

- 첫 출력은 `\033[H\033[2J` (커서 홈 + 지우기, 개행 없음).
- `view_record_active()` 이면(=`[r]` 녹화 또는 `--record` 로 기록 중) `clr(CLR_WAIT)` + `[REC]`/`[녹화중]` + `clr(CLR_OFF)` + 공백 1.
- 제목: EN `cub_ctv - %s (pid %d)  %s  window %.1fs  (%dHz, samples=%d)`, KO `cub_ctv - %s (pid %d)  %s  구간 %.1fs  (%dHz, samples=%d)`. 인자: db, pid, hhmmss, `view.dt > 0 ? view.dt : interval`, hz, `view.st.samples`.
- 그 뒤 `clr(CLR_OFF)` + `"\n\n"`.

### 7.3 판정 블록

1. `cs_figures_ranked(lo->count, dt, fig)`.
2. `view.hist_known` 이면 `history_import(view.seq, view.hist_n, view.hist_back, view.hist_top)`.
3. `history_push(view.seq, fig[0].severity >= 2 ? fig[0].ix : -1)`.
4. `fig[0].severity >= 2` 이면 `print_verdict(out, &fig[0], lo->count > 1 ? &fig[1] : NULL)` (§7.4).
5. 아니면 HEAD `[verdict] no critical section contention` / `[판정] 크리티컬 섹션 경합 없음` OFF `\n`. 이어서 `view.wait_avail` 이고 `lk = wait_cnt[17]`, `pg = wait_cnt[3]`, `fb = wait_cnt[21]` 의 합이 0 보다 크면: `"  "` + OWNER + `outside the CS layer: lock waits %d (cubrid lockdb), page latch %d, free buffer %d` / `CS 밖에서 대기: 락 %d (cubrid lockdb), 페이지 래치 %d, 빈 버퍼 %d` + OFF + `\n`.

### 7.4 판정 표 (`print_verdict`)

심각도 단어 `sev`: `severity >= 4` → `high`/`높음`; `== 3` → `moderate`/`중간`; 그 밖 → `low`/`낮음`.
점 표시 `dots`: 4글자, k < severity 이면 `*`, 아니면 `.` (예: 3 → `***.`).

**머리 줄**:
1. `clr(severity >= 3 ? CLR_WAIT : CLR_HEAD)`.
2. EN `[verdict] %s (%s) bottleneck  severity %s %s` / KO `[판정] %s (%s) 병목  심각도 %s %s` — `cs_desc(name)`, name, dots, sev.
3. `clr(CLR_OFF)` `clr(CLR_MUTED)`.
4. EN `   persistence: %d of the last %d windows` / KO `   지속: %d구간 (최근 %d구간 중)` — `v.same`, `v.span`.
5. `clr(CLR_OFF)` `\n`.

**열 머리**: `"%s  %s %s %s%s\n"` = HEAD, `dw_pad(item/항목, 11)`, `dw_pad(judgement/판단, 46)`, `evidence`/`근거`, OFF.

**`verdict_row(out, item, judge, evidence)`**:
1. `c1 = dw_pad(item, 11)`.
2. evidence 가 `""` → `"  %s %s\n"` (c1, judge; judge 는 채우거나 자르지 않음).
3. `str_cols(judge) > 46` → `"  %s %s\n"` (c1, judge) 를 먼저 쓰고, 다음 줄은 c1 = 공백 11칸, c2 = 공백 46칸.
4. 아니면 `c2 = dw_pad(judge, 46)`.
5. `"  %s %s %s%s%s\n"` = c1, c2, MUTED, evidence, OFF.

**행 목록** (순서대로; judge 버퍼 160B, ev 버퍼 200B, `snprintf` 절단):

| 항목 EN/KO | 조건 | judge | evidence |
|---|---|---|---|
| `where` / `어디서` | 항상 | `"%s (%s)"` = cs_desc, name | `has_second`: `lost %.2f s/s; next %s %.2f s/s` / `손실 %.2f s/s; 2위 %s %.2f s/s` (top lost, 2위 name, 2위 lost). 아니면 `lost %.2f s/s; no other CS waiting` / `손실 %.2f s/s; 다른 CS는 대기 없음` |
| `who` / `누가` | 항상 | holder==0: `daemon_name` 이 있으면 `engine thread %s` / `엔진 스레드 %s`; 없으면 name == `LOG` 일 때 `engine log flush daemon` / `엔진 로그 플러시 데몬`, 그 밖 `engine daemon (system tran)` / `엔진 데몬 (system tran)`. holder>0: `client session %s` / `클라이언트 세션 %s` (`tran_text(holder, holder_cpid, LABEL)`, 96B). holder==-2: `view.st.occ[ix] > 0` 이면 `readers (no single owner)` / `reader 다수 (단일 소유자 없음)`, 아니면 `not observed` / `관측 안 됨` | `one_thr && thr_pct < held_pct - 0.5`: `held %.0f%% of the window (engine threads together %.0f%%)` / `구간의 %.0f%% 보유 (엔진 스레드 합계 %.0f%%)` (thr_pct, held_pct). 아니면 `held %.0f%% of the window (occupied %.0f%%)` / `구간의 %.0f%% 보유 (점유 %.0f%%)` (held_pct, occ_pct) |
| `how bad` / `얼마나` | 항상 | `"%s (%d/4)"` = sev, severity | `avg_ms > 0`: `%.0f of %.0f entries/s waited (%.0f%%), acquire %.3f ms/entry` (wait_s, enter_s, 비율, avg_ms) / KO `대기 %.0f건/s (진입 %.0f건/s 의 %.0f%%), 진입당 획득 %.3f ms` (같은 순서). 비율 = `enter_s > 0 ? 100*wait_s/enter_s : 0`. 아니면 `%.0f of %.0f entries/s waited; waiters seen %.0f%% of samples` / `대기 %.0f건/s (진입 %.0f건/s); 대기자 목격 %.0f%% 샘플` (wait_s, enter_s, wait_pct) |
| `capacity` / `처리량` | holder == 0 일 때만 | `thr_pct >= 60` → `near the daemon's capacity` / `데몬 처리 상한 근접`, 아니면 `headroom remains` / `여유 있음` | `daemon holds %.0f%% -> %.0f%% headroom at %.0f entries/s` / `데몬 보유 %.0f%% -> 여유 %.0f%%, 진입 %.0f건/s` (thr_pct, 100-thr_pct, enter_s) |
| `who suffers` / `피해` | nwaiters > 0 | `%d client sessions, waited ~%.0f%% of the window each` / `클라이언트 세션 %d개, 각 구간의 약 %.0f%% 대기` (nwaiters, waited_pct) | 나열 항목마다 `buf_cat(ev, 200, &o, "%stran %d (%d)", k ? ", " : "", wtran[k], wsamples[k])`, more_waiters 면 `buf_cat(…, ", ...")`. **언어 공통**(M 쌍 아님, 한국어에서도 `tran`·`, `·`, ...`). `buf_cat` 이므로 넘치면 UTF-8 경계에서 잘리고 이후 호출은 무동작(`", ..."` 도 생략). 최장 4 항목 × 약 30 B 라 200 B 에서 실제로는 잘리지 않는다 |
| `who suffers` / `피해` | nwaiters == 0 | `no client session identified` / `식별된 클라이언트 세션 없음` | `waiters were engine threads or not resolved` / `대기자가 엔진 스레드이거나 미식별` |
| `action` / `조치` | holder == 0 | `not a session problem -> look at the engine side` / `세션 조치 불필요 -> 엔진 쪽을 볼 것` | `holder is an engine daemon` / `쥔 쪽이 엔진 데몬` |
| 〃 | holder > 0 | `inspect the holding session` / `쥔 세션을 확인할 것` | `a client holds it longest` / `클라이언트가 가장 오래 쥠` |
| 〃 | holder == -2 | `reduce the entry rate` / `진입 빈도를 줄일 것` | `no writer holds it; contention is by volume` / `writer 없음; 횟수에 의한 경합` |
| `check` / `확인할 것` | 항상 | `cs_hint(name)` | `""` (한 칸 행) |
| `not shown` / `화면 밖` | bound == VB_NONE (여기서 표 종료) | `disk vs daemon CPU (needs disk await); SQL text; service impact (SLA)` / `디스크 vs 데몬 CPU 구분(디스크 await 필요); SQL 문장; 서비스 영향(SLA)` | `""` |
| `disk/cpu` / `디스크/CPU` | bound != VB_NONE | DISK: `disk write latency` / `디스크 쓰기 지연`; CPU: `holder thread CPU-bound` / `쥔 스레드 CPU 포화`; SHORT: `neither disk nor CPU saturated -> short holds, high rate` / `디스크·CPU 모두 여유 -> 짧은 보유가 잦음`; UNKNOWN: `holder thread state unknown` / `쥔 스레드 상태 미확보` | `ev[0] = 0`. os 있으면 `snprintf(ev, 200, "%s: D %.0f%% R %.0f%% cpu %.0f%%", os->comm[0] ? os->comm : "thr", dsk_pct, run_pct, cpu_pct)` — comm 이 비면 **리터럴 3글자 `thr`**(번호 없음). `view.disk_avail` 이면 `o = strlen(ev)` 뒤에 `snprintf(ev+o, 200-o, "%s%s await %.1f ms util %.0f%%", o ? "; " : "", disk_dev, disk_await_ms, disk_util_pct)`. 언어 공통(EN·KO 서식이 같음) |
| `not shown` / `화면 밖` | bound != VB_NONE | `SQL text; service impact (SLA)` / `SQL 문장; 서비스 영향(SLA)` | `""` |

### 7.5 CS 순위 표

1. `"\n"` HEAD `[critical sections by waiting]` / `[대기 발생 순위]` OFF `"\n"`.
2. 열 머리: HEAD + `"   %-23s %s %5s %6s %7s %*s%s  %s"` + OFF + `"\n"`. 인자: `"name"`, `dw_pad(guards/보호 대상, 24)`, `"occ%"`, `"wait%"`, `"wait/s"`, 폭 `gap = max(0, 7 - str_cols(avg))` 의 `""`, avg 머리(`avg ms` / `평균ms`, 둘 다 6칸 → gap 1), holder 머리(`holder` / `쥔 쪽`).
3. 행: fig 순서대로, `severity == 0` 은 건너뛰며 idle 로 센다. 나머지는 n 을 1부터 매겨:
   - `"%s%2d %-23s %s %5.0f %6.0f %7.0f %s  %s%s\n"` = 색(심각도 ≥3 → WAIT, ==2 → OWNER, 1 → `""`), n, name(23 초과 시 그대로 넘침), `dw_pad(cs_desc, 24)`, occ_pct, wait_pct, wait_s, avg, who, OFF.
   - avg: `avg_ms > 0` 이면 `"%7.3f"`, 아니면 `"%7s"` 의 `-`.
   - who: holder==0 && daemon_name 있음 → `engine %s` / `엔진 %s`; holder != -2 → `tran_text(holder, hcpid, LABEL)` (`holder_of(ix, &hs, &hcpid)`); 그 밖 → occ > 0 이면 `readers` / `reader 다수`, 아니면 `-`.
4. idle > 0 이면 MUTED `   %d others: idle` / `   나머지 %d개: 활동 없음` OFF `\n`.
5. `print_thread_waits(out, 1)` (§7.6), 그리고 `"\n"`.

### 7.6 스레드 대기 (`print_thread_waits(out, screen)`)

`view.wait_avail == 0` 이면 아무것도 쓰지 않는다.

1. 코드 순서 `{17, 3, 21, 7, 9, 11, 19, 13, 15, 23, 25, 26, 5}` 로 `n = wait_cnt[v]` 를 본다. 0 은 건너뜀.
2. `7 <= v <= 12` 이면 `csw += n` (CS 대기는 한 항목으로).
3. 그 밖은 `waiting += n`, 줄에 `"%s%s %d"` (앞에 내용이 있으면 ` · ` (U+00B7 앞뒤 공백), `resume_name(v)`, n).
4. `csw > 0` 이면 마지막에 `resume_name(9)` 항목으로 추가, `waiting += csw`.
5. 목록에 없는 코드(예: 1, 8, 10, 12, 27)는 세지 않는다.

`resume_name` 표:

| 코드 | EN | KO |
|---|---|---|
| 3 | page latch | 페이지 래치 |
| 5 | job queue | 작업 큐 |
| 7, 9, 11 | critical section | 크리티컬 섹션 |
| 13 | connection queue | 접속 큐 |
| 15 | class repr | 클래스 표현 |
| 17 | lock | 락 |
| 19 | log writer | 로그 writer |
| 21 | free buffer | 빈 버퍼 |
| 23 | DWB queue | DWB 큐 |
| 25 | sleep | sleep |
| 26 | concurrency slot | 동시성 슬롯 |
| 그 밖 | `""` | `""` |

출력:

| screen | 형식 |
|---|---|
| 1 (요약 화면) | `"\n"` HEAD `[thread waits]  %d live threads, %d waiting` / `[스레드 대기]  실스레드 %d개, 대기 중 %d개` OFF `"\n"`; `waiting > 0` 이면 `"   %s\n"` (줄) |
| 0 (스냅샷) | MUTED `thread waits: %d live, %d waiting%s%s` / `스레드 대기: 실스레드 %d개, 대기 중 %d개%s%s` (wait_live, waiting, waiting 이면 `" - "` 와 줄) OFF `"\n"` |

### 7.7 세션 패널 (`print_sessions(panel_rows)`)

1. act 중 `tran >= 1` 인 것만 인덱스 목록 → (tran, cpid, act 위치) 오름차순 정렬.
2. 같은 (tran, cpid) 의 연속 구간이 한 세션(구간 중 슬롯이 다른 클라이언트에게 넘어가면 클라이언트마다 한 세션): `waits` = ACT_WAIT samples 합, `holds` = 그 밖 합, `first`, `n`. `view_identity_seen(tran, cpid) == 0`(슬롯의 현재 클라이언트)인 세션만 `strans[0..nst)` 에 tran 을 오름차순으로 기록(9단계 idle 계산용; 떠난 클라이언트의 세션은 현재 신원을 활동 있음으로 세지 않는다).
   - `SESS` = `{int tran, cpid, waits, holds; int first, n;}`. `first` 는 정렬된 인덱스 배열 `act_by_tran[]` 안의 시작 위치, `n` 은 그 세션의 항목 수 — 7단계 활동 줄이 `act_by_tran[first .. first+n)` 을 이 순서(= 구간 최초 목격 순)로 훑는 데 쓴다.
3. 세션 정렬: waits 내림차순 → holds 내림차순 → tran 오름차순.
4. 개수 줄: HEAD `[sessions]  %d client transactions, %d active on a CS` / `[세션]  클라이언트 트랜잭션 %d개, CS 활동 %d개` (`view.nident`, 세션 수). `act_dropped > 0` 이면 `  (+%d sightings not kept: activity table full)` / `  (목격 %d건 미보관: 활동 표 가득 참)`. OFF `"\n"`.
5. 세션 0개 또는 `panel_rows < 0` 이면 여기서 끝(남은 세션 줄 없음).
6. 열 머리: HEAD + `"%5s %9s %s %s"` (`tran`, `pid`, `dw_pad(client/클라이언트, 14)`, `dw_pad(what it did in the window/이 구간에 한 일, 60)` — 60칸까지 공백으로 채워 줄 끝 공백이 생긴다) + OFF + `"\n"`.
7. 정렬 순으로 최대 `panel_rows` 개:
   - `r = view_identity_seen(tran, cpid, &id)` (05 §5), `have = r >= 0`. r = 0 현재 클라이언트, 1 떠난 클라이언트(구간 시작 신원), 2 떠났고 신원 없음(`id` 는 cpid 만).
   - 클라이언트 종류(`client_kind`): 입력 program 은 `r == 0 || r == 1` 일 때만, 그 밖은 `""`(→ `-`). program 에 `_cub_cas_` 가 있고 그것이 맨 앞이 아니며 바로 뒤 글자가 숫자이면 `"<앞부분>:cas<뒤부분>"` (예: `broker1_cub_cas_2` → `broker1:cas2`). 아니면 program, 빈 문자열이면 `-` (identity 없으면 `-`).
     - 정확한 규칙: `p = strstr(program, "_cub_cas_")` — **첫 번째** 출현. 조건 `p != NULL && p > program && isdigit(p[9])`. 결과 `snprintf(buf, 72, "%.*s:cas%s", (int)(p - program), program, p + 9)` — 뒤부분은 `p + 9` 부터 **문자열 끝까지 전부**(숫자만이 아님; `b_cub_cas_3x_cub_cas_4` → `b:cas3x_cub_cas_4`). 버퍼는 `IDENT_LEN + 8` = 72 B.
     - 아니면 `snprintf(buf, 72, "%s", program[0] ? program : "-")`. identity 가 없으면 `client_kind("")` 를 불러 `-`.
   - 활동 줄(`char line[512]`, `o = 0`, `line[0] = 0`): `r == 1 || r == 2` 이면 먼저 `buf_cat(line, 512, &o, "%s", M("(disconnected) ", "(접속 종료) "))`. 이어서 `k = first .. first+n−1` 을 `o + 2 < 512` 인 동안 돌며 매번
     1. `buf_cat(line, 512, &o, "%s", k > first ? ", " : "")`,
     2. `buf_cat(line, 512, &o, M(…), view.rows[a->cs].name, a->samples)` — 서식 `waits %s (%d samples)` / `%s 대기 (%d샘플)`(ACT_WAIT) 또는 `holds %s (%d samples)` / `%s 보유 (%d샘플)`(그 밖),
     3. `o + 1 >= 512` 이면 중단.
     `buf_cat`(§4.4)이므로 512 B 를 넘으면 UTF-8 시퀀스 경계에서 잘리고(`pos = utf8_cut(buf, 511)`), 그 뒤 호출은 무동작이다. `snprintf` 로 통째 쓰는 것과 잘린 끝이 다를 수 있으니 반드시 이 순서를 지킨다. 구분자 `", "` 는 언어 공통.
   - pid 버퍼: `snprintf(pidb, 16, "%d", have ? id.cpid : 0)`; 출력은 `have ? pidb : "-"` — 떠난 클라이언트(r 1/2)는 목격된 그 클라이언트의 pid.
   - 줄 자르기: `cols = term_columns()`, `room = cols - 31 - 1`. `cols > 39` 이고 `str_cols(line) > room` 이면 `dw_pad(line, room)` 결과로 바꾼다(칸 경계 자르기, 넓은 글자 탈락 시 공백 보충).
   - 행: `"%s%5d %9s %s %s\033[0m\n"` = 속성, tran, pid(`have` 면 cpid 10진, 아니면 `-`), `dw_pad(kind, 14)`, 줄. 속성: waits > 0 → `color_on ? CLR_PANEL_WAITER : "\033[4m"`, 아니면 `color_on ? CLR_PANEL_OWNER : "\033[7m"`. 끝의 `\033[0m` 은 `--plain` 에서도 항상 출력.
8. 표시 못 한 세션이 있으면 MUTED `  ... %d more active sessions` / `  ... 활동 세션 %d개 더` OFF `\n`.
9. `view.ident[k].idx >= 1` 이면서 `strans[0..nst)` 에 없는 수(idle) > 0 이면 MUTED `  %d other sessions: no CS activity in this window` / `  나머지 %d개 세션: 이 구간에 CS 활동 없음` OFF `\n`.

### 7.8 하단

`clr(CLR_MUTED)` → `live_footer != NULL` 이면 `"\n%s"` (footer), 아니면 `"\n[q] quit"` / `"\n[q] 종료"` → `clr(CLR_OFF)` `"\n"` → `fflush(stdout)`.

---

## 8. 텍스트 행 (src/ctv_rows.c)

### 8.1 보조 규칙

| 함수 | 규칙 |
|---|---|
| `row_tran(r)` | `r->rwlock < 0 ? r->tran : -1` |
| `row_owner(r)` | `r->rwlock < 0 ? r->owner : 0` |
| `owner_is_system(tran, thr)` | `tran == 0 \|\| (tran < 0 && thr >= 0)` |
| `state_str(rwlock)` (ctv_cs.c) | `> 0` → `"%d readers"`, `< 0` → `"1 writer"`, 0 → `"none"` |
| `chain_of(wq, seen_cs, widx, tran, cpid)` | `seen_cs >= 0` 이면 `view_seen_chain(seen_cs)` (구간 중 처음 목격 체인), 아니면 `view_waiters(wq)` (현재 큐). 각 홉의 tran = `view_tran_of(slot)` (-1 = 모름), `cpid != NULL` 이면 각 홉의 클라이언트 pid = `view_tran_cpid_of(slot)` (-1 = 모름; JSON 은 `NULL`) |
| `chain_count(nw, waitw, seen)` | `!seen && waitw >= nw` → `"[%d of %u waiting]"`, 아니면 `"[%d seen]"` |
| `tran_text(tran, cpid, style)` 공통 | `r = tran >= 1 ? view_identity_seen(tran, cpid, &id) : -1` (05 §5). identity 있음 = r 0/1. `gone` = r 1/2 이면 `", disconnected"` / `", 접속 종료"`, 아니면 `""` |
| `tran_text(tran, cpid, SHORT)` | r == 2 → `" pid %d%s"` (cpid, gone); identity 있으면 `" %s %s%s"` (user, program, gone); 없으면 tran == 0 이면 `" system"`, 아니면 `""` |
| `tran_text(tran, cpid, LABEL)` | tran == 0 → `engine (system tran 0)` / `엔진 (system tran 0)`; r == 2 → `tran %d (pid %d%s)`; identity 있음 → `tran %d (%s pid %d%s)` (tran, program, cpid, gone); tran > 0 → `tran %d`; 그 밖 → `-`. **M 쌍은 `engine …`/`엔진 …` 와 `gone` 뿐**이고 나머지 틀(`tran %d (…)`, `tran %d`, `-`)은 언어 공통. SHORT 형의 `" system"`, `" pid %d"` 도 언어 공통 |

**언어 공통(항상 영어) 고정 문자열** — 00 §5 의 "표 헤더·상태 토큰" 외에도 다음은 `M` 쌍이 없어 `--ko` 에서도 바뀌지 않는다:

| 위치 | 문자열 |
|---|---|
| §8.1 `state_str` | `%d readers`, `1 writer`, `none` |
| §8.1 `chain_count` | `[%d of %u waiting]`, `[%d seen]` |
| §8.2 소유자 블록 | `  [system`, `owner  system`, `owner  %s@%s %s pid=%d login=%s ip=%s`, `owner  tran %d`, `owner  tran %d pid=%d`, `  [tran %d`, `  [tran %d pid=%d`, ` thr=%d`, `  thr=%d`, ` trid=%d`, `  trid=%d`, `  {wq:`, `wq     `, ` -> `, `(tran %d)`, `(tran %d%s)`, `,..`, ` -> ..` |
| §8.4 스냅샷 표 | 열 머리 줄 전체 |
| §8.5 스크롤 행 | `  <-- WAIT`, 막대 `#`/`.` |
| §8.6 세로 레코드 | 별표 머리 줄, `state            : `, `waitR/waitW : `, `owner tran       : `, `(trid %d)`, `owner client     : … program … login … ip … pid …`, `0 (system%s%s)`, `owner thread     : `, `waiting writers  :`, ` thr %d`, ` (tran %d%s)`, `(chain not readable)`, `nenter / nwait   : `, `reenter : `, `total / max wait : … ms / … ms`, `stats            : - (block did not validate)` |
| §8.7 트랜잭션 표 | 열 머리(`idx trid user …`) |
| §8.8 풀 | 열 머리(`name n nenter max_ms`), 유형명(`RWLOCK` 등) |
| §10 스크롤 | `--- %s  (samples=%d, dt=%.2fs)`, 열 머리(`ix name occ% …`) |

`M` 쌍이 있는 것은 이 문서가 `EN / KO` 로 적은 것뿐이다(예: §8.6 의 `(no critical section matches %s)` / `(일치하는 크리티컬 섹션 없음: %s)`).

### 8.2 소유자 블록 (`owner_block`)

입력: tran_index, cpid(목격 당시 클라이언트 pid, −1 모름), owner_tid, wq, seen_cs, waitw. 출력 세 문자열(각 `TAG_LEN`): `one`(한 줄형), `l1`(owner 연속행), `l2`(wq 연속행). `thr = view_slot_of_tid(owner_tid)`.

1. `r = tran_index >= 1 ? view_identity_seen(tran_index, cpid, &id, &trid) : -1`. r 0/1 이면 `have_id`(1 = 슬롯을 떠난 클라이언트의 구간 시작 신원).
2. 소유자 머리:

| 조건 | one | l1 |
|---|---|---|
| `owner_is_system(tran_index, thr)` | `"  [system"` + (comm 있으면 `" "` + comm) | `"owner  system"` + (comm 있으면 `" "` + comm) |
| `have_id` | `"  [%s@%s %s pid=%d"` (user, host, program, cpid) | `"owner  %s@%s %s pid=%d login=%s ip=%s"` |
| r == 2 (떠났고 신원 없음) | `"  [tran %d pid=%d"` | `"owner  tran %d pid=%d"` |
| `tran_index >= 1` | `"  [tran %d"` | `"owner  tran %d"` |
| 그 밖 | (없음) | (없음) |

comm = `thr >= 0` 일 때 `view_os_of(thr)->comm` (없거나 빈 문자열이면 생략).

3. one 에 머리가 있으면: `thr >= 0` → one `" thr=%d"`, l1 `"  thr=%d"` (THR_INDEX); `r == 0 && view.has_trid` → one `" trid=%d"`, l1 `"  trid=%d"`(떠난 클라이언트는 trid 생략); `r == 1 || r == 2` → one `" disconnected"` / `" 접속 종료"`, l1 `"  disconnected"` / `"  접속 종료"`; one 에 `"]"`.
4. 체인 `nw = chain_of(wq, seen_cs, widx, wtran, wcpid)`. 홉 i 마다:
   - h1 = `"%s thr=%d"` (i == 0 → `"  {wq:"`, 아니면 `","`), h2 = `"%sthr=%d"` (i == 0 → `"wq     "` (wq + 공백 5), 아니면 `" -> "`). **`%d` 는 `THR_INDEX(widx[i])` = 칸 + 1** 이다(소유자 `thr=` 와 같은 표시 번호). §8.6 세로 레코드의 `" thr %d"` 와 JSON `wq[].thr` 도 모두 `THR_INDEX` 이며, 원시 칸 번호를 화면에 내는 곳은 없다. 버퍼: h1 64 B, h2 256 B, SHORT 문자열 160 B(`snprintf` 절단).
   - 홉 tran t >= 0 이면 h1 에 `"(tran %d)"`, h2 에 `"(tran %d%s)"` (`tran_text(t, wcpid[i], SHORT)` — 슬롯을 떠난 클라이언트면 `, disconnected`/`, 접속 종료` 또는 `pid N`).
   - `o + strlen(h1) + 48 >= TAG_LEN` 또는 `b + strlen(h2) + 48 >= TAG_LEN` 이면 one 에 `",.."`, l2 에 `" -> .."` 를 붙이고 중단.
   - 아니면 h1, h2 를 붙임.
5. `nw > 0` 이면 one 에 `" %s}"`, l2 에 `"  %s"` (`chain_count(nw, waitw, seen_cs >= 0)`).

### 8.3 행 배치 (`print_row_tagged(row, one, l1, l2)`)

- `split = layout_mode == NARROW || (layout_mode == AUTO && cols > 0 && visible_len(row) + visible_len(one) > cols)`, `cols = term_columns()`. 파이프(cols 0)에서 AUTO 는 나누지 않는다. WIDE 는 나누지 않는다.
- `one` 이 비었거나 나누지 않을 때:
  - `color_on` 이고 one 에 `"  {wq:"` 가 있으면 `row + OWNER + one[0..wq) + WQ + one[wq..] + OFF + "\n"`.
  - 아니면 `row + (one 있으면 OWNER) + one + (one 있으면 OFF) + "\n"`.
- 나눌 때: `row + "\n"`; l1 이 있으면 `"      | "` (공백 6) + OWNER + l1 + OFF + `"\n"`; l2 가 있으면 `"      | "` + WQ + l2 + OFF + `"\n"`.

### 8.4 스냅샷 CS 표 (`print_snapshot`)

머리: `"%six  name                      state        waitR waitW  tran     nenter     nwait  reenter   total_ms    max_ms%s\n"` (HEAD, OFF).

구분 줄: `print_snapshot` 은 머리 앞에 빈 줄을 쓰지 않는다. 스냅샷 출력은 레이아웃 줄(§2.12, `\n` 으로 끝남) 바로 다음 줄이 이 머리다. 이어지는 순서는 §2.11 `render_snapshot` 그대로: CS 표 → `print_thread_waits(stdout, 0)`(있으면 1줄) → `printf("\n")`(빈 줄 1개, 유일한 구분자) → `print_trans()` 제목(앞 빈 줄 없음) → `print_view_pool()`(각 유형 블록은 자체 `"\n"` 로 시작). 실측(`demodb-snapshot.rec`):

```text
layout: stride=160B count=18 stats_off=152 (runtime-probed)  threads=4314
ix  name                      state        waitR waitW  tran     nenter ...
...
17  ACCESS_STATUS            none             0     0     -      12945 ...
thread waits: 138 live, 5 waiting - page latch 1 · critical section 4

transactions (from memory, no SQL connection):
```

행마다(0..count-1):
- tr = `row_tran` 이 -1 이면 `-`, 아니면 10진.
- st = has_stats → `"%10u %9u %8u %10.1f %9.3f"` (nenter, nwait, nreenter, total_ms, max_ms); 아니면 `"%10s %9s %8s %10s %9s"` 모두 `-`.
- row = `"%2d  %-24s %s%-12s%s %5u %5u %5s %s"` = i, name, (rwlock<0 ? WRITER : ""), state_str, (rwlock<0 ? OFF : ""), wr, ww, tr, st. (행 버퍼 256B)
- `owner_block(row_tran, -1, row_owner, r->wq, -1, r->ww)` → `print_row_tagged`. (현재 writer 이므로 cpid −1 = 현재 클라이언트)

### 8.5 라이브 스크롤 CS 행 (`live_print_row(i, dt)`)

- `tg = rwlock < 0 ? c->tran : view.st.seen_tran[i]`, `tg_cpid = rwlock < 0 ? -1 : view.st.seen_cpid[i]`, `owner = rwlock < 0 ? c->owner : view.st.seen_owner[i]`, `wq = c->wq`, `seen_cs = (c->wq == 0 && view.st.seen_wq[i] != 0) ? i : -1`.
- `cs_rates(c, prev, occ, wai, samples, dt)` → de = enter_s, dw = wait_s, occ_pct.
- 막대: `fill = (int)(occ_pct / 10 + 0.5)`, 10칸, 앞 fill 칸 `#`, 나머지 `.`.
- `waiting = wai[i] > 0 || dw > 0`; `idle = !waiting && occ[i] == 0 && de == 0 && rwlock == 0`.
- 막대 색: occ_pct ≥ 80 → BAR_HIGH, ≥ 50 → BAR_MID, 그 밖 BAR_LOW.
- 행(320B): `"%s%2d  %-24s %5.1f %s%s%s %6.1f %9.1f %9.1f %5u %5u  %s%s%s%s%s"` = (idle ? MUTED : ""), i, name, occ_pct, 막대색, 막대, (idle ? MUTED : OFF), wait_pct, de, dw, wr, ww, (rwlock<0 ? WRITER : ""), state_str, (waiting ? WAIT : ""), (waiting ? `"  <-- WAIT"` : ""), ((idle || waiting || rwlock<0) ? OFF : "").
- `owner_block(tg, tg_cpid, owner, wq, seen_cs, c->ww)` → `print_row_tagged`.

`row_moved(i)` = `nenter` 또는 `nwait` 가 구간 시작과 다르거나, `occ[i] != 0`, `wai[i] != 0`.

### 8.6 세로 레코드 (`print_detail_row(i, r)`)

`thr = view_slot_of_tid(row_owner(r))`, `tran = row_tran(r)`.

1. `"%s*************************** %d. %s ***************************%s\n"` (HEAD, i, name, OFF; 별표 27개씩).
2. `"state            : %-16s waitR/waitW : %u / %u\n"` (state_str, wr, ww).
3. 소유자:
   - `tran >= 1` 이고 identity 있음: `"owner tran       : %d"` + (has_trid 면 `" (trid %d)"`) + `"\n"`; `"owner client     : %s@%s  program %s  login %s  ip %s  pid %d\n"` (user, host, program, login, ip, cpid).
   - `owner_is_system(tran, thr)`: `"owner tran       : 0 (system%s%s)\n"` (comm 있으면 `" "` 와 comm).
   - `tran > 0`: `"owner tran       : %d\n"`; 그 밖 `"owner tran       : -\n"`.
4. `thr >= 0` 이면 `"owner thread     : %d\n"` (THR_INDEX).
5. `seen_cs = (r->wq == 0 && view.st.seen_wq[i] != 0) ? i : -1`, `nw = chain_of(r->wq, seen_cs, widx, wtran, wcpid)`. 소유자(3단계)는 현재 writer 의 현재 신원이다.
   - nw > 0: 홉마다 `"%s thr %d"` (k == 0 → `"waiting writers  :"`, 그 밖 공백 18칸) + (t >= 0 이면 `" (tran %d%s)"` (`tran_text(t, wcpid[k], SHORT)`)) + `"\n"`; 끝에 공백 19칸 + `chain_count(nw, r->ww, seen_cs >= 0)` + `"\n"`.
   - nw == 0 && `ww > 0`: `"waiting writers  : %u (chain not readable)\n"`.
6. has_stats: `"nenter / nwait   : %u / %u      reenter : %u\n"`, `"total / max wait : %.1f ms / %.3f ms\n"`. 아니면 `"stats            : - (block did not validate)\n"`.

`detail_match(i, name)`: 필터 NULL → 참; 아니면 필터가 i 의 10진 문자열과 같거나 name 과 대소문자 무시로 같으면 참.

`print_detail(lo, rows)`: 일치 CS 마다 세로 레코드. 하나도 없으면 `(no critical section matches %s)\n` / `(일치하는 크리티컬 섹션 없음: %s)\n` (필터, NULL 이면 ""). 필터는 256바이트 버퍼(`char f[256]`)에 `snprintf` 로 복사(초과 절단)한 뒤 `text_sanitize` 를 거쳐 `%s` 로 나간다(실측 `--detail=$'ZZ\x01Q'` → `(no critical section matches ZZ?Q)`).

구분 줄: 세로 레코드 사이·앞·뒤 어디에도 빈 줄이 없다. 각 레코드는 별표 머리 줄로 시작하고 마지막 줄(`total / max wait : …` 또는 `stats            : - …`)의 `\n` 으로 끝나며, 다음 레코드의 별표 줄이 곧바로 이어진다. 스냅샷 `--detail` 은 레이아웃 줄 바로 다음 줄이 첫 별표 줄이다. 라이브/재생 스크롤 `--detail`(§10 4단계)도 마지막 CS 행(또는 유휴 줄) 바로 다음 줄에 첫 별표 줄이 오고, 다음 구간은 §10 1단계의 `"\n--- …"` 가 빈 줄 1개를 만든다.

### 8.7 트랜잭션 표 (`print_trans`)

1. HEAD `transactions (from memory, no SQL connection):` / `트랜잭션 (메모리 판독, SQL 무접속):` OFF `\n`.
2. `!view.tran_avail`: `    (transaction table unavailable: %s)\n` / `    (트랜잭션 테이블 사용 불가: %s)\n` (tran_refused, NULL 이면 `-`), 반환 -1.
3. 머리: `"%s%4s %8s  %-10s %-16s %-10s %-16s %-15s %8s%s\n"` = HEAD, `idx`, `trid`, `user`, `program`, `login`, `host`, `client_ip`, `cpid`, OFF.
4. identity 마다: tb = has_trid ? trid 10진 : `-`; `dw_pad` 폭 user 10, program 16, login 10, host 16, ip 15 (칸 기준 채움·자름); `"%4d %8s  %s %s %s %s %s %8d\n"` (idx, tb, …, cpid).
5. `nident == 0 && !cid_locked`: `    (no client identity: no client attached, or the CLIENTIDS layout not recognised)\n` / `    (클라이언트 신원 없음: 접속 클라이언트가 없거나 CLIENTIDS 레이아웃 미인식)\n`. `nident == 0` (잠김): `    (no client transaction)\n` / `    (클라이언트 트랜잭션 없음)\n`.
6. 반환 0.

### 8.8 풀 (`print_view_pool`)

| `view.pool_state` | 출력 |
|---|---|
| 1 | `pool_print_groups(view.pool, view.npool, view.pool_truncated)` |
| < 0 | `warn()` 경유 stderr `cub_ctv: stats pool did not validate - skipping pool view\n` / `cub_ctv: 통계 풀 검증 실패 - 풀 뷰 생략\n` (`cub_ctv: ` 접두 포함, stdout 에는 아무것도 없음) |
| 0 | `\n[pool] not collected (sync_Stats not exported)\n` / `\n[풀] 미수집 (sync_Stats 미노출)\n` |

`pool_print_groups`: 유형 t = 2..4 (`RWLOCK`, `RMUTEX`, `MUTEX`; CSECT 제외) 마다, 그 유형 그룹이 있으면 처음에 `"\n"` HEAD `[pool] %s: %d entries (grouped by name)` / `[풀] %s: %d엔트리 (이름별 그룹)` (유형명, cnt 합) OFF `"\n"` HEAD `"  %-36s %5s %12s %10s"` (`name`, `n`, `nenter`, `max_ms`) OFF `"\n"`; 그룹마다 `"  %-36s %5d %12lu %10.3f\n"`. 끝에 truncated: `>= 0` → `  (pool chain truncated at chunk %d - counts incomplete)\n` / `  (풀 체인이 청크 %d에서 끊김 - 집계 불완전)\n`; `-3` → `  (pool not grouped: out of memory in cub_ctv)\n` / `  (풀 집계 생략: cub_ctv 메모리 부족)\n`; `-4` → `  (pool chain came back to a chunk already read - stopped there)\n` / `  (풀 체인이 이미 읽은 청크로 돌아옴 - 그 지점에서 멈춤)\n`; `-2` → `  (more than %d distinct names - the rest not grouped)\n` / `  (서로 다른 이름이 %d개를 넘음 - 나머지는 묶지 않음)\n` (1024); -1 → 없음.

`pool_print_groups` 의 세부:
- 유형 순서는 `t = ST_TYPE_CSECT + 1 .. ST_TYPE_MUTEX` (= 2, 3, 4) 고정. 각 유형 안에서 그룹은 **`view.pool` 배열 순서 그대로**(정렬 없음; 라이브는 `pool_collect` 가 만든 순서, 재생은 `G` 줄 순서).
- 유형 머리의 `%d entries` = 그 유형 모든 그룹의 `cnt` 합(int).
- 머리 블록 정확한 출력: `printf("\n%s", HEAD)` → `printf(M("[pool] %s: %d entries (grouped by name)", "[풀] %s: %d엔트리 (이름별 그룹)"), pool_type_names[t], tt)` → `printf("%s\n%s  %-36s %5s %12s %10s%s\n", OFF, HEAD, "name", "n", "nenter", "max_ms", OFF)`. 그룹 행 `printf("  %-36s %5d %12lu %10.3f\n", name, cnt, (unsigned long) nenter, max_ms)`.
- 끝의 절단 안내는 그룹 유무와 무관하게 `truncated` 값만 보고 출력한다. 따라서 표시할 그룹이 하나도 없어도(예: −3 메모리 부족, 또는 ≥0 인데 그룹 0) 안내 한 줄은 나온다. 안내 앞에 빈 줄은 없다.

---

## 9. JSON (src/ctv_json.c)

### 9.1 문자열 (`json_fstr`)

`"` 로 감싼다. 바이트별: `"` 와 `\` → 백슬래시 접두; `< 0x20` → `\u%04x` (소문자 16진); 유효 UTF-8 시퀀스 → 그대로; 그 밖(깨진 바이트) → `\ufffd` 후 1바이트 진행. 0x7F 는 그대로.

깨진 바이트 치환은 **ASCII 6바이트 이스케이프 `\ufffd`**(`fputs("\\ufffd", f)`: 백슬래시, `u`, `f`, `f`, `f`, `d`)이며, U+FFFD 의 원시 UTF-8 3바이트(`EF BF BD`)가 아니다. 판정 순서: `"`/`\` → `< 0x20` → `utf8_valid_len(s) == 0`(깨짐) → 유효 시퀀스 `len` 바이트 복사.

### 9.2 문서 (`json_doc(pid, db, lo, window, rows, trans)`)

```
{"tool":"cub_ctv","pid":<pid>,"db":<str>,"ts":<view.ts>,"layout":{"stride":<n>,"count":<n>,"stats_off":<n>},"degraded":<u>,
  [window] "samples":<view.st.samples>,"dt":<%.3f>,
  [rows]   "cs":[...] ,"thread_wait":... ,"log_disk":...
  [rows && trans] ,
  [trans]  "trans_available":<bool>,"identities_locked":<bool>,"trans":[...]
}\n
```

(공백·줄바꿈 없음. 헤더는 항상 `,` 로 끝나고 이어지는 내용이 문서를 닫는다.) 끝에 `}\n` 과 `fflush`.

정확한 조립(모두 stdout `printf`):

```c
/* json_header */
printf ("{\"tool\":\"cub_ctv\",\"pid\":%d,\"db\":", pid);
json_fstr (stdout, db);
printf (",\"ts\":%ld,\"layout\":{\"stride\":%d,\"count\":%d,"
        "\"stats_off\":%d},\"degraded\":%u,", view.ts,
        lo->stride, lo->count, lo->off_stats, view.degraded);
/* json_doc */
if (window) printf ("\"samples\":%d,\"dt\":%.3f,", view.st.samples, view.dt);
if (rows)   json_cs_rows (stdout, lo, view.rows,
                          window ? view.st.occ : NULL, window ? view.st.wai : NULL,
                          window ? view.st.samples : 0, window ? view.prev : NULL,
                          window ? view.dt : 0.0, 0);   /* "cs":[..] + json_os */
if (rows && trans) printf (",");
if (trans)  json_trans ();
printf ("}\n");
fflush (stdout);
```

| 항목 | 규칙 |
|---|---|
| `"ts"` | `%ld` 의 `view.ts`(유닉스 초 정수, 따옴표 없음) |
| `"degraded"` | **프레임 값 `view.degraded`**(`%u`). 전역 `degraded` 가 아니다. 라이브는 캡처 끝의 전역 비트 복사본, 재생은 그 프레임의 `B` 줄(없으면 직전 프레임 값, 첫 프레임 전 초기값 0). 재생 헤더 `H` 의 degraded 는 레이아웃 줄(§2.12)에만 쓰이고 JSON 에는 나오지 않는다 |
| `"dt"` | `%.3f` 의 `view.dt`(렌더러 인자 `dt` 아님). 라이브는 자르지 않은 `now − t_prev`, 재생은 기록값(≤ `REC_DT_MAX`) |
| 쉼표 | 헤더 끝 `,` 와 window 조각 끝 `,` 는 항상 붙는다. 그래서 window 다음에는 곧바로 `"cs"` 또는 `"trans_available"` 이 온다. rows 와 trans 사이에만 별도 `,` 하나 |
| `window=1, rows=0, trans=1` | `…"degraded":N,"samples":S,"dt":D,"trans_available":…,"trans":[…]}` |
| `window=0, rows=1, trans=1` | `…"degraded":N,"cs":[…],"thread_wait":…,"log_disk":…,"trans_available":…}` |
| `cs` 배열 | `i = 0 .. lo->count−1` **모든 CS**(유휴 포함, 거르지 않음). 원소 구분 `,`, 첫 원소 앞에는 없음 |
| 점유 수치 | `samples > 0` 일 때만(스냅샷 `window=0` 은 samples 인자 0 이므로 생략) |

`json_trans` 원소 구분도 첫 원소 앞에는 없고 이후 `,`. `json_os` 의 `counts` 키는 `"%d"` 따옴표 정수.

| 호출 경로 | window | rows | trans |
|---|---|---|---|
| 스냅샷 `--json` | 0 | 1 | 1 |
| 스냅샷 `--trans --json` | 0 | 0 | 1 |
| 라이브/재생 `--json` (구간당 1줄) | 1 | 1 | 0 |
| 라이브/재생 `--trans --json` | 1 | 0 | 1 |

### 9.3 CS 원소 (`json_cs_rows`, raw = 0 일반 / raw = 1 덤프)

원소 키 순서(조건부 키는 해당 시에만):

| 순서 | 키 | 값 |
|---|---|---|
| 1 | `ix` | i |
| 2 | `name` | 문자열 |
| 3 | `rwlock` | %d |
| 4 | `waitR`, `waitW` | %u |
| 5 | `tran` | raw ? `r->tran` : `row_tran(r)` |
| 6 | `thr` | slot = raw ? `thr_index_of_tid_mem(r->owner)` : `view_slot_of_tid(row_owner(r))`; slot ≥ 0 이면 `slot+1`, 아니면 -1 |
| 7 | `owner` (시스템) | `!raw && owner_is_system(row_tran, slot)` 일 때 `{"system":true,"comm":<str>}` (comm 없으면 `""`) |
| 8 | `owner_tid`, `wq_head`, `has_stats` | raw 일 때 `"0x%llx"` 문자열 둘과 %d |
| 9 | `owner` (클라이언트) | `row_tran >= 1` 이고 identity 있을 때 `{"tran":N,"user":…,"program":…,"login":…,"host":…,"client_ip":…,"cpid":N}` |
| 10 | `wq` | `[{"thr":slot+1,"tran":t},…]` — `seen_cs = (!raw && r->wq == 0 && seen_wq[i] != 0) ? i : -1` 로 `chain_of`. 원소 서식 `"%s{\"thr\":%d,\"tran\":%d}"`(첫 원소 앞 `""`, 이후 `","`). `t` 는 `view_tran_of(slot)` 그대로이므로 **모르면 `"tran":-1` 로 출력**(키를 빼지 않음). 체인이 없으면 `"wq":[]` 는 항상 나온다 |
| 11 | `wq_seen` | `seen_cs >= 0 && nw > 0` 일 때 `true` |
| 12 | `nenter`,`nwait`,`nreenter`,`total_ms`,`max_ms` | has_stats 일 때; ms 는 `%.3f` |
| 13 | `occ_pct`,`wait_pct`,`enter_s`,`wait_s` | samples > 0 일 때 `%.2f` (cs_rates) |

배열 뒤 `json_os`:
- `,"thread_wait":` → `wait_avail` 아니면 `null`, 맞으면 `{"live":N,"counts":{"<code>":N,…}}` (코드 0..27 중 값 > 0 만, 오름차순).
- `,"log_disk":` → `disk_avail` 아니면 `null`, 맞으면 `{"dev":<str>,"await_ms":%.3f,"util_pct":%.1f}`.

### 9.4 트랜잭션 (`json_trans`)

`"trans_available":true|false,"identities_locked":true|false,"trans":[` + 각 `{"idx":N,"trid":<N 또는 null>,"user":…,"program":…,"login":…,"host":…,"client_ip":…,"cpid":N}` + `]`. trid 는 `has_trid` 일 때만 숫자, 아니면 `null`. `json_ident(with_info=1)` 은 맨 앞에 `"info"` 를 추가하지만 이 모듈의 호출은 모두 0.

---

## 10. 프레임 출력 선택 (src/ctv_present.c)

| 함수 | 규칙 |
|---|---|
| `live_banner(hz, interval)` | `hz <= 0`: `live: transaction table every %.1fs; Ctrl-C to stop\n` / `라이브: %.1fs 마다 트랜잭션 표; Ctrl-C로 종료\n`. 그 밖: `live: %dHz sampling, %.1fs display; Ctrl-C to stop\n` / `라이브: 샘플링 %dHz, 표시 %.1fs; Ctrl-C로 종료\n`. `fflush` |
| `live_render(lo, screen, json, dt, hz, interval, pid, db)` | hhmmss = `clock_hms(view.ts)`. json → `json_doc(…, 1, 1, 0)`. screen → `screen_summary`. 그 밖 스크롤(아래) |
| `trans_window_out(pid, db, lo, json)` | json → `json_doc(…, 1, 0, 1)`; 아니면 `"\n--- %s  (samples=%d, dt=%.2fs)\n"` (hh:mm:ss, samples, `view.dt`) 후 `print_trans()`. `fflush` |
| `replay_pace(h)` | `h->live && view.dt > h->interval ? h->interval : view.dt` |
| `replay_frame_out(h, screen, json, trans)` | trans → `trans_window_out(h->pid, h->db, &h->lo, json)`; 아니면 `live_render(&h->lo, screen, json, view.dt, h->hz, h->interval, h->pid, h->db)` |

스크롤 출력(`live_render`, 비화면·비JSON):
1. `"\n--- %s  (samples=%d, dt=%.2fs)\n"` (hhmmss, samples, 인자 dt).
2. `row_moved(i)` 인 행만. 첫 행 앞에 머리 1회: `"%s%2s  %-24s %5s %-10s %6s %9s %9s %5s %5s  %s%s\n"` = HEAD, `ix`, `name`, `occ%`, `occ-bar`, `wait%`, `enter/s`, `wait/s`, `waitR`, `waitW`, `state`, OFF. 행은 `live_print_row`.
3. 하나도 없으면 `    (idle - no critical section activity)\n` / `    (유휴 - 크리티컬 섹션 활동 없음)\n`.
4. `layout_detail` 이면 이어서 `row_moved(i) && detail_match(i, name)` 인 CS 의 `print_detail_row` (구분 빈 줄 없음, §8.6). 일치가 하나도 없어도 §8.6 의 "no critical section matches" 문구는 내지 않는다(스냅샷 `print_detail` 전용).
5. `fflush`.

`dt` 의 출처: 라이브는 `run_close_window` 가 `now − t_prev`(자르지 않음)를 인자로 넘기고 `view.dt` 도 같은 값이다. 재생은 `replay_frame_out` 이 `view.dt`(기록 시 `REC_DT_MAX` = 1e7 s 로 잘린 값)를 넘긴다. 따라서 구간이 1e7 s 를 넘는 극단적 경우에만 라이브와 재생의 `dt=%.2fs`·비율이 달라진다(05 §11). 라이브도 자르지 않는다.

---

## 11. 라이브 샘플러 (src/ctv_sampler.c)

### 11.1 윈도우 수명

| 함수 | 동작 |
|---|---|
| `live_win_new(count, stride)` | `LIVE_WIN` calloc, `blob = malloc(stride * count)`. 실패 시 die `out of memory` / `메모리 부족` |
| `live_win_free` | blob, w 해제 |
| `live_win_reset(w, count)` | `win_stats_clear(&w->st)` (occ/wai/seen_* 0, `seen_tran = -1`, `seen_cpid = -1`, `seen_n = -1`, `nact = act_dropped = 0`); CS 마다 `wq_last = 0`, `ch_n = 0`, `own_last = 0`, `own_slot = -1`; `act_ix` 전부 0; `nos = 0`; `!no_disk` 이면 `view_disk_begin(w)`. `ch_walked[]` 는 리셋하지 않는다(윈도우를 넘어 유지) |

### 11.2 샘플 1회 (`live_accumulate(w, lo)`)

blob 은 직전 `mem_read` 로 읽은 CS 배열 전체 이미지. CS i 마다:

1. `cs_decode_raw` 로 rwlock, wr, ww, tran.
2. `rwlock != 0` → `st.occ[i]++`.
3. `rwlock < 0 && tran < 0` → `tran = 0` (트랜잭션 없는 writer 스레드 = 엔진).
4. `rwlock < 0`:
   1. `owner = rd_u64(blob, off + off_owner)`, `cpid = client_pid(tran)` (§11.2a).
   2. `tran >= 1 || (tran == 0 && seen_tran[i] < 0)` → `seen_tran[i] = tran`, `seen_cpid[i] = cpid`, `seen_owner[i] = owner` (클라이언트 tran 우선, 엔진은 아직 없을 때만).
   3. owner == 0 이면 k = -1. 아니면 `owner != own_last[i]` 일 때만 `own_last = owner`, `own_slot = thr_index_of_tid_mem(owner)` 갱신, `k = own_slot[i]`.
   4. `act_add(w, i, ACT_HELD, tran, k, cpid)`.
   5. `k >= 0` 이면 `os_sample(w, k)`.
5. `wr != 0 || ww != 0`:
   1. `head = cs_wait_queues(blob, lo, i, &head2, &field)`; `st.wai[i]++`.
   2. head != 0:
      - `fresh = head != wq_last[i]`, `t = mono_s()`.
      - `every(&ch_walked[i], t, fresh ? OS_SAMPLE_MIN_S : CHAIN_REWALK_S)` (= 0.05 / 0.25 초) 가 참이면:
        - `at = cs_base + i * stride + field`.
        - `ch_n[i] = thr_waiters_mem2(head, head2, ch_slot[i], ch_tran[i], 64, fresh ? at : 0)`, 이어서 홉마다 `ch_cpid[i][q] = client_pid(ch_tran[i][q])`.
        - `wq_last[i] = ch_n[i] > 0 ? head : 0` (순회 실패한 헤드는 다음 기회에 새 헤드로 재시도).
        - `(seen_wq[i] == 0 || seen_wq[i] == head) && ch_n[i] > 0` 이면 `seen_wq = head`, `seen_n = ch_n`, `seen_slot`/`seen_tran_of`/`seen_cpid_of` 복사(구간의 첫 큐 보관).
      - `head == wq_last[i]` 인 동안 q = 0..ch_n-1 에 `act_add(w, i, ACT_WAIT, ch_tran[i][q], ch_slot[i][q], ch_cpid[i][q])` (현재 헤드의 체인만 집계; 새 헤드가 스로틀되면 이 샘플은 대기 집계 없음).
   3. head == 0 → `wq_last[i] = 0`.
6. 대기자 없음 → `wq_last[i] = 0`.

끝에 `st.samples++`.

`every(&last, now, gap)` (ctv_sched.c): `now - last < gap` 이면 0, 아니면 `last = now` 후 1.

### 11.2a 클라이언트 pid (`client_pid(tran)`)

슬롯은 재사용되므로(엔진은 해제된 가장 낮은 슬롯부터 재배정) tran index 만으로는 구간 중 클라이언트가 바뀐 것을 구분할 수 없다. 목격마다 그 슬롯의 클라이언트 pid 를 함께 남긴다.

- `tran < 1` 또는 `tran_geom.ntran <= 0` → −1.
- 모듈 정적 캐시 `pid[n]`, `at[n]`(`n = tran_geom.ntran`, 바뀌면 재할당; 할당 실패 → −1).
- `tran >= n` → −1.
- `at[tran] == 0 || mono_s() − at[tran] >= OS_SAMPLE_MIN_S`(0.05 초) 이면 `pid[tran] = tran_client_pid(tran)` (03 §8, CLIENTIDS 의 pid 4 B), `at[tran] = now`. 아니면 캐시값.
- 결과: tran 당 판독은 최대 20 Hz. 0.05 초 안에 재배정되고 다시 목격된 슬롯은 그 간격 동안 앞 클라이언트의 pid 를 쓴다.

### 11.3 활동 표 (`act_add(w, cs, kind, tran, thr, cpid)`)

- 키: (cs, kind, tran, cpid) + tran == 0 일 때만 thr (엔진 스레드를 스레드별로 구분). `key_thr = tran == 0 ? thr : -1`.
- 해시(unsigned 32비트 산술): `h = (cs*31 + kind*7 + tran*2654435761 + (key_thr+1)*40503 + (cpid+1)*97) & (ACT_HASH - 1)`; 선형 탐사; `act_ix[h]` = act 인덱스 + 1 (0 = 빈칸). 해시 함수는 조회용일 뿐 출력 순서에 영향이 없다(act 는 최초 목격 순서로 쌓임).
- 일치(`cs, kind, tran, cpid` 같고 `tran != 0 || a->thr == key_thr`) → `samples++`; `a->thr < 0` 이면 `a->thr = thr`.
- 새 항목: `nact < (kind == ACT_HELD ? 4096 : 4096 - 256)` 이면 추가(`samples = 1`, `cpid`), 아니면 `act_dropped++`. 대기 항목은 마지막 256칸을 쓰지 못한다(보유 목격 예약).

### 11.4 OS 샘플 (`os_sample(w, k)`)

1. `tid = thr_tid_of(k)`; ≤ 0 이면 무시.
2. `w->os[]` 에서 slot == k 를 찾고, 없으면 `nos < 64` 일 때 새로 만들어 slot, tid, `os_task_comm` 으로 comm(16B) 을 채움. 가득 차면 무시.
3. `now = mono_s()`. `t_last > 0 && now - t_last < 0.05` 이면 무시(스레드당 20 Hz 상한).
4. `t_last = now` (판독 실패도 스로틀 적용).
5. `os_task_state(target_pid, tid)` (`/proc/<pid>/task/<tid>/stat` 의 상태 문자와 utime+stime) 실패 → 무시.
6. 첫 성공이면 `ticks0 = ticks`, `t_first = now`. 매번 `ticks1 = ticks`, `span_ms = (int)((now - t_first) * 1000 + 0.5)`, `samples++`, 상태 `R` → run++, `D` → dsk++, 그 밖 oth++.

---

## 12. 라이브 루프 (src/ctv_live.c)

### 12.1 LIVE_RUN

| 필드 | 의미 |
|---|---|
| `h`, `lo`, `cs_base` | 헤더, 레이아웃, CS 배열 주소 |
| `json`, `trans` | 옵션 |
| `screen` | 전체 화면 여부 |
| `keys` | 키 입력 사용 여부 |
| `sampled` | `!table_only` (표 단독이면 샘플링 없음) |
| `have_frame` | 첫 구간이 닫혔는가 |
| `rec_fail` | 직전 [r] 기록 실패(하단에 1회 표시) |
| `said_r` | 표 단독에서 [r] 경고를 이미 했는가 |
| `record` | `--record` 파일 또는 NULL |
| `recname[256]` | [r] 기록 파일명, 없으면 "" |
| `footer[320]` | 하단 문구 |
| `w` | LIVE_WIN |
| `period`, `interval`, `t_prev` | `1/hz`, 표시 주기, 구간 시작 시각(mono) |

### 12.2 시작 (`live_mode` → `run_start`)

1. `r` 를 0 으로, `sampled = !table_only`, `screen = !json && !trans && isatty(STDOUT_FILENO)`, `period = 1.0 / hz`, `interval = h->interval`.
2. `w = live_win_new(count, stride)`, `w->cs_base = cs_base`, `w->no_disk = !sampled`, `live_win_reset`.
3. `run_start`:
   1. screen 이면 `keys = keys_enter() == 0`.
      - keys: `layout_detail` 이면 경고 `the terminal shows the summary screen; --detail applies to the scrolling (pipe) live output` / `터미널은 요약 화면을 보입니다; --detail 은 스크롤(파이프) 라이브 출력에 적용`. 그리고 `screen_enter()`.
      - 아니면(stdin 이 tty 아님) `screen = 0`, `live_banner(sampled ? hz : 0, interval)`.
   2. **else if**(1 의 `if (screen)` 에 대한 else) `!json` 이면 `live_banner(sampled ? hz : 0, interval)`. 판정은 진입 시점의 `screen` 값으로 한 번만 하므로, 1 에서 `screen` 이 0 으로 바뀌어도 2 는 실행되지 않는다 → 배너는 **최대 1회**.
      ```c
      if (r->screen) { …; else { r->screen = 0; live_banner (…); } }
      else if (!r->json) live_banner (…);
      ```
   3. sampled 이면 `cs_read_rows(cs_base, lo, w->prev)` 실패 시 `die_target_gone`.
   4. `t_prev = mono_s()`.
   5. `!sampled && !json && isatty(STDOUT_FILENO) && keys_enter() == 0` 이면 `restore_on_exit()`, `keys = 1` (전체 화면 없이 키만).
   6. `live_stop_on_signals()` (INT/TERM/HUP → 정지 플래그).
4. `trans` 이면 즉시 `view_capture_trans(0.0)`; `!view.tran_avail` 이면 `trans_window_out(...)` 후 `rc = 1` (루프 진입 안 함).

### 12.3 틱 루프

`rc == 0 && !term_stop_requested()` 인 동안:

1. `tick_end = sampled ? mono_s() + period : t_prev + interval`.
2. sampled 이면 `run_sample`: `mem_read(cs_base, blob, stride*count)` 실패 → `die_target_gone`; `live_accumulate`; `thr_cache_refresh()`.
3. `now = mono_s()`. `now - t_prev >= interval` 이면 `run_close_window(now)`; 1 이면 `rc = 1` 후 탈출.
4. keys 이면 `run_keys(tick_end)` 가 1 이면 탈출(`q`). 아니면 `sleep_until(tick_end)`.

루프 후 `run_finish`, `rc` 반환. 정지 시그널은 진행 중 틱을 마친 뒤 루프 조건에서 끝나며, 닫히지 않은 구간은 표시·기록되지 않는다.

### 12.4 구간 닫기 (`run_close_window(now)`)

1. `!sampled`: `stat("/proc/<pid>")` 실패 → `die_target_gone`; `view_capture_trans(now - t_prev)`.
2. sampled: `cs_rows_from_blob(blob, lo, w->cur)` (마지막 샘플 이미지에서 행), `view_capture(lo, w->cur, w, now - t_prev)`.
3. `view_record_frame()`. `view_record_ok() != 0` 이면: `--record` 면 die `cannot write record %s` / `기록 파일 %s 쓰기 실패`; 아니면 `view_record_close()`, `recname = ""`, `rec_fail = 1`.
4. keys 이면 `live_footer_text(...)`, `live_footer = footer`, `rec_fail = 0`.
5. `have_frame = 1`.
6. trans 이면 `trans_window_out(pid, db, lo, json)`; `!view.tran_avail` 이면 1 반환. 아니면 `live_render(lo, screen, json, now - t_prev, hz, interval, pid, db)`.
7. `w->prev = w->cur` (count 행), `live_win_reset`, `t_prev = now`, 0 반환.

### 12.5 키 (`run_keys(tick_end)`)

`polled = 0` 에서 시작해 반복:
1. `rest = tick_end - mono_s()`.
2. `(rest <= 0 && polled)` 이거나 `key_read(rest > 0 ? rest : 0) == 0` 이면 0 반환(첫 반복은 지연된 틱이라도 1회 폴링).
3. 키별:

| 키 | 조건 | 동작 |
|---|---|---|
| `KEY_EOF` | — | `keys = 0` (이후 sleep 으로 진행), 0 반환 |
| `r` | `record == NULL && sampled` | `rec_fail = live_record_toggle(...) != 0`; `have_frame && screen` 이면 하단 문구 갱신 후 `replay_frame_out(h, screen, json, trans)` 로 즉시 재그리기 |
| `r` | `!sampled && !said_r` | `said_r = 1`, 경고 `[r] records the sampled live view: run -i without --trans` / `[r] 은 샘플링 라이브 화면을 기록합니다: --trans 없이 -i 로 실행` |
| `r` | `--record` 사용 중 | 무시 |
| `l` | — | `use_ko = !use_ko`; 하단 문구 갱신(rec_fail 0); `have_frame && screen` 이면 재그리기(스크롤 출력은 다음 구간부터 반영) |
| `KEY_RESIZE` | `have_frame && screen` | 재그리기 |
| `q` | — | 1 반환(정상 종료, 완성된 구간은 기록에 남음) |
| 그 밖 | — | 무시하고 반복 |

재그리기는 현재 `view` 를 다시 렌더링하므로 seq 가 같아 이력은 같은 칸을 덮는다.

### 12.6 하단 문구 (`live_footer_text`)

| 상태 | 문구 EN | 문구 KO |
|---|---|---|
| `--record` 사용 | `REC %s (--record)   [l] KO/EN  [q] quit` (파일명) | `녹화 중 %s (--record)   [l] 한/영  [q] 종료` |
| [r] 기록 중 | `REC %s   [r] stop recording  [l] KO/EN  [q] quit` (recname) | `녹화 중 %s   [r] 녹화 중지  [l] 한/영  [q] 종료` |
| 기록 없음 | `[r] record to <pid>_<db>_<time>.ctv  [l] KO/EN  [q] quit` + (rec_fail 이면 `   (record file could not be written)`) | `[r] 녹화 (<pid>_<db>_<시각>.ctv)  [l] 한/영  [q] 종료` + (`   (기록 파일 쓰기 실패)`) |

버퍼 320B (`snprintf` 절단).

- 기록 없음 줄의 `<pid>_<db>_<time>` / `<pid>_<db>_<시각>` 은 **리터럴 텍스트**다(값을 채우지 않음). 서식은 `snprintf(footer, n, "%s%s", M(위 문구), rec_fail ? M(실패 문구) : "")`.
- `--record` 줄의 `%s` 는 `o.record` 를 `char f[PATH_MAX]` 에 `snprintf` 로 복사한 뒤 `text_sanitize` 한 값. `[r]` 줄의 `%s` 는 `recname`(§12.7 이 만든 이름: db 를 영숫자·`_`·`-`·`.` 로 치환했으므로 이미 안전).

### 12.7 [r] 기록 토글 (`live_record_toggle`)

기록 중이면(중지):
1. `empty = view_record_frames() == 0`, `rc = view_record_close()`.
2. empty 이면 `unlink(name)`.
3. `name = ""`, rc 반환.

기록 중이 아니면(시작):
1. `stamp = strftime("%Y%m%d-%H%M%S", localtime(now))`.
2. `safe` = db 를 128B 로 복사 후, 각 바이트가 영숫자·`_`·`-`·(위치 > 0 인 `.`) 가 아니면 `_` 로 치환.
3. `name = "%d_%s_%s.ctv"` (pid, safe, stamp). 헤더 사본 `hh = *h`, `hh.degraded = degraded` (현재 비트).
4. `view_record_open(name, &hh, excl=1)` 실패 시: `errno != EEXIST` 이거나 k > 9 이면 `name = ""`, -1 반환. 아니면 `"%d_%s_%s-%d.ctv"` (k = 2..9) 로 재시도.
5. 성공 0.

### 12.8 마무리 (`run_finish`)

1. 기록 활성: `empty = record == NULL && frames == 0`. `view_record_close() != 0` 이고 `--record` 이면 die `cannot write record %s`. empty 면 `unlink(recname)`.
2. `live_footer = NULL`, `live_win_free`.

---

## 13. 재생 루프 (src/ctv_replay.c)

### 13.1 진입 (`live_replay(h, speed, json, trans)`)

1. `screen = !json && !trans && isatty(STDOUT_FILENO)`.
2. screen 이고 `!view_replay_seekable()` 이면 경고 `the record is not seekable (pipe): scrolling replay without keys` / `기록을 되감을 수 없습니다(파이프): 키 없이 스크롤 재생`, `screen = 0`.
3. screen 이면 `n = view_replay_index()` (전체 프레임 색인, 화면 전환 전 경고 가능). `keys_enter() == 0` 이면 `layout_detail` 경고 `the terminal shows the summary screen; --detail applies to the scrolling (pipe) replay` / `터미널은 요약 화면을 보입니다; --detail 은 스크롤(파이프) 재생에 적용`, `screen_enter()`, `replay_interactive(h, n, speed)`, 0 반환. 아니면 `screen = 0`.
4. 스크롤 재생: `!json` 이면 `live_banner(h->hz, h->interval)`.
5. `pace = -1`. `view_replay_next() == 0` 인 동안:
   1. `speed > 0 && pace >= 0` 이면 `sleep_until(mono_s() + pace / speed)` (첫 프레임은 대기 없음; 각 프레임은 자기 길이만큼 머문 뒤 다음).
   2. `replay_frame_out(h, 0, json, trans)`.
   3. `trans && !view.tran_avail` 이면 1 반환.
   4. `pace = replay_pace(h)`.
6. 끝까지 프레임이 없었으면(pace < 0) die `record holds no complete frame` / `기록에 완성된 프레임이 없습니다`. 아니면 0.

`speed == 0` 은 대기 없음.

### 13.2 대화형 재생 (`replay_interactive(h, n, speed)`)

1. `n == 0` 이면 die `record holds no complete frame` / `기록에 완성된 프레임이 없습니다`.
2. `speed <= 0` 이면 1.
3. `i = 0, paused = 0, loaded = -1`. 반복:
   1. `i != loaded` 이면 `i == loaded + 1` 은 `view_replay_next()`, 그 밖은 `view_replay_goto(i)` (키프레임부터 다시 읽음). 실패 시 die `cannot read frame` / `프레임 판독 실패`. `loaded = i`.
   2. 하단: `REPLAY %d/%d  x%g%s   [space] pause  [<] [>] window  [+] [-] speed  [l] KO/EN  [q] quit` / `재생 %d/%d  x%g%s   [space] 일시정지  [<] [>] 구간  [+] [-] 속도  [l] 한/영  [q] 종료` (i+1, n, speed, paused 면 `  PAUSED` / `  일시정지`). 버퍼 200B. `live_footer = footer`.
   3. `replay_frame_out(h, screen=1, json=0, trans=0)`.
   4. `key = key_read(paused ? -1.0 : replay_pace(h) / speed)`.
   5. 키별:

| 키 | 동작 |
|---|---|
| `q`, `KEY_EOF` | `live_footer = NULL`, 반환(→ 0) |
| ` ` (space) | `paused = !paused` |
| `KEY_BACK` | `i = max(i-1, 0)`, `paused = 1` |
| `KEY_FWD` | `i < n-1` 이면 `i+1`, `paused = 1` |
| `+` (`=` 포함) | `speed < 64` 이면 `speed *= 2` |
| `-` (`_` 포함) | `speed > 1/16` 이면 `speed /= 2` |
| `l` | `use_ko = !use_ko` (같은 프레임 재그리기) |
| `KEY_RESIZE`, `KEY_IGNORED`, 그 밖 | 재그리기만 |
| 0 (시간 초과) | 일시정지가 아니면: `i < n-1` → `i++`, 마지막이면 `paused = 1` |

대화형 재생에는 `on_stop` 이 설치되지 않으므로 Ctrl-C 는 `on_signal` 로 화면 복원 후 `_exit(130)`.

---

## 14. 재구현 점검표

1. 모든 사용자 문자열은 §2~§13 의 EN/KO 쌍 그대로, 언어는 렌더링 시점의 `use_ko`.
2. 색은 `clr()` 경유(꺼지면 빈 문자열). 예외: 세션 패널의 `\033[4m`/`\033[7m`/`\033[0m` 은 색과 무관하게 출력.
3. 폭 계산은 바이트가 아닌 칸(§4). `printf` 의 `%-Ns` 를 쓰는 열(CS name 등)은 바이트 기준이므로 그대로 유지.
4. 요약 화면은 상단 블록을 메모리에 먼저 써서 개행 수로 자르고, 남은 행으로 세션 패널 크기를 정한다(§7.1).
5. 판정 이력은 프레임 번호(seq) 기준이며 재그리기는 새 항목이 아니다. 재생은 프레임에 실린 이력을 import 한다.
6. 보유자·대기 세션·OS 스레드의 동점 규칙은 모두 "구간에서 먼저 목격된 쪽"(act 배열 순서).
7. 종료 코드: die 1, `--trans` 표 없음 1, 신호 처리 경로 128+sig, 그 밖 0.
