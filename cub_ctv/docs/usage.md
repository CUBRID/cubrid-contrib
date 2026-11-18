# 사용법 (Usage)

## 0. 누구를 위한, 어느 상황을 위한 도구인가

크리티컬 섹션 자체가 무엇이고 어떻게 동작하는지는 `docs/critical-sections.md`(신입 엔지니어용)를 먼저 읽는다.

README 의 같은 절을 요약하면: 커밋·접속·아카이브 경로에서 서버가 직렬화되거나 멈췄을 때 접속 없이 "누가 무엇을 쥐고 누가 기다리는지"를 판정으로 내놓는 기술지원용 도구이고, 그 밖의 성능 문제(느린 질의·행 락·페이지 래치·버퍼 부족)에서는 "CS 경합 없음"으로 범위를 지워 주는 배제 도구다. 상황별 CS 와 답은 README 표 참조.

## 1. 옵션 레퍼런스

```text
cub_ctv [옵션] [<db-name> | <pid>]

  -h, --help            도움말 (--ko/--en 위치와 무관하게 언어 반영). 인자가 없으면(--ko/--en 만 있어도) 도움말,
                        다른 옵션만 있고 대상이 없으면 "대상 없음" 오류(종료 코드 1)
  -l, --list            실행 중인 cub_server 목록 (pid, db)
  -i, --interactive     라이브 모드: 100Hz 샘플링. tty = 풀스크린 제자리 갱신, 파이프 = 스크롤.
                        tty 에서 [r] 로 현재 디렉터리에 <pid>_<db>_<YYYYmmdd-HHMMSS>.ctv 녹화 시작/중지, [l] 한/영 전환, [q] 종료
      --interval SEC    라이브 표시 주기 (기본 1, 0.1 .. 3600, 샘플 1회 이상: >= 1/--hz)
      --hz N            라이브 샘플링 주파수 (기본 100, 1 .. 10000). --hz/--interval 위반은 "--hz/--interval 값 오류"(종료 코드 1); --replay 는 기록값 사용
      --trans           트랜잭션 신원 표만 출력 (무접속 tranlist); 표를 못 읽으면 사유 출력 후 종료 코드 1
                        -i 와 라이브 기록 재생에서는 구간마다 표를 출력(화면 없음)
      --detail[=NAME|IX] CS별 세로 레코드. 필터 없으면 전 CS. 필터는 `--detail=LOG` 또는 대상 뒤 두 번째 단어(`cub_ctv demodb --detail LOG`). -i 와 조합하면 파이프(스크롤) 출력에만 적용(터미널은 요약 화면, 경고)
      --wide            소유자 블록을 항상 행 끝 한 줄에 (폭 무시)
      --narrow          소유자 블록을 항상 연속행(| owner / | wq)으로
      --plain           ANSI 색상 없이 (기본: 터미널이면 색상, 파이프·JSON 은 항상 없음). 세션 표의 반전·밑줄 속성은 유지
      --json            JSON 출력. 스냅샷/--trans = 1문서, -i = 윈도우당 1줄
      --record FILE     보이는 내용(스냅샷 1장, 라이브는 구간마다)을 FILE 에 함께 기록
      --replay FILE     --record 파일을 다시 표시 (서버·설치본 불필요). 형식 오류는 줄 번호와 함께 거부.
                        터미널이면 키로 조작(§2.7). 숨은 옵션 --replay-speed N = 시작 속도 (0 = 파이프에서 대기 없음)
      --dump FILE       디테일 덤프: 이 순간 읽을 수 있는 정보 전부를 JSON 으로 (스냅샷 전용, 재생 불가)
      --ko | --en       메시지 언어 (기본 LC_ALL/LC_MESSAGES/LANG 자동; 한국어는 UTF-8 로캘에서만)
```

대상은 DB 이름 또는 PID. `--dump` 는 `-i`(`--interactive`) 와, `--replay` 는 `-i`/`--dump`/`--record` 와 함께 쓸 수 없다. `--dump` 와 `--record` 는 같은 파일을 가리킬 수 없다.

## 2. 출력 모드별 예시

### 2.1 스냅샷 (기본)

```text
대상: pid=1229138 db=demodb lib=/opt/cubrid/lib/libcubrid.so.11.5
레이아웃: stride=160B count=18 stats_off=152 (런타임 검증 통과)  threads=4314
ix  name                      state        waitR waitW  tran     nenter     nwait  reenter   total_ms    max_ms
 0  WFG                      none             0     0     -          0         0        0        0.0     0.000
 1  LOG                      1 writer         0     3     7    1088174    234930     6050   326364.1  1617.016
      | owner  DBA@dbhost01 csql pid=2355439 login=cubrid ip=10.0.0.79  thr=4138  trid=1860847
      | wq     thr=4136(tran 13 DBA csql)  [1 of 3 waiting]
 2  LOCATOR_CLASSNAME_TABLE  none             0     0     -    6910702     90887        0     1811.0    11.000
 3  QPROC_QUERY_TABLE        none             0     0     -          3         0        0        0.0     0.000
 4  QPROC_LIST_CACHE         none             0     0     -          0         0        0        0.0     0.000
 5  DISK_CHECK               none             0     0     -    1048523         0        0       84.0     1.001
 6  CNV_FMT_LEXER            none             0     0     -          0         0        0        0.0     0.000
 7  HEAP_CHNGUESS            none             0     0     -    2095291      4877   749286      289.0     3.000
 8  TRAN_TABLE               none             0     0     -       2224         1      562        0.0     0.000
 9  CT_OID_TABLE             none             0     0     -    1198891      3013        0      179.0     1.001
10  HA_SERVER_STATE          none             0     0     -          2         0        1        0.0     0.000
11  COMPACTDB_ONE_INSTANCE   none             0     0     -          0         0        0        0.0     0.000
12  ACL                      none             0     0     -          0         0        0        0.0     0.000
13  PARTITION_CACHE          none             0     0     -     299693       156        0       41.0     1.001
14  EVENT_LOG_FILE           none             0     0     -          0         0        0        0.0     0.000
15  TRACE_LOG_FILE           none             0     0     -          0         0        0        0.0     0.000
16  LOG_ARCHIVE              none             0     0     -         20         0        4        0.0     0.000
17  ACCESS_STATUS            none             0     0     -        580         0        0        0.0     0.000
스레드 대기: 실스레드 129개, 대기 중 4개 - 크리티컬 섹션 4

트랜잭션 (메모리 판독, SQL 무접속):
 idx     trid  user       program          login      host             client_ip           cpid
   1  1860839  DBA        csql             cubrid     dbhost01         10.0.0.79        2355406
   2  1860840  DBA        csql             cubrid     dbhost01         10.0.0.79        2355410
   3  1860845  DBA        csql             cubrid     dbhost01         10.0.0.79        2355414
   (… 18행)

[풀] RWLOCK: 2엔트리 (이름별 그룹)
   (… §2.4)
```

(위는 `tests/demo_contention.sh demodb 45 6 1,2,9` 부하 중, `COLUMNS=120`, `--ko`. 아래 예시도 같은 부하다.)

| 컬럼 | 의미 |
|---|---|
| state | 순간 점유 `none` / `N readers` / `1 writer` |
| waitR / waitW | 대기 중 reader / writer 수 (순간값) |
| tran | writer 점유 트랜잭션 인덱스 (`-` = 없음) |
| nenter / nwait / reenter | 누적 획득 시도 / 무한대기 발생 / 재진입 |
| total_ms / max_ms | 누적 / 최대 **획득 지연** (보유 시간 아님) |
| `-` | 통계 블록이 검증을 통과하지 못한 행 |

소유자 블록: 행 끝 `  [user@host program pid=N thr=T trid=X]  {wq: thr=A(tran 1), thr=B(tran 2) [n of waitW waiting]}`. login·ip 와 대기자의 user·program 은 연속행(`| owner`, `| wq`)과 `--detail` 에만 나온다.
행+블록이 터미널 폭(COLUMNS 또는 tty 크기)을 넘으면 `| owner`, `| wq` 연속행으로 내린다.
파이프 출력은 폭 무제한(`--narrow` 가 아니면 한 줄). `| owner  system <스레드명>  thr=T`(한 줄 형식 `[system <스레드명> thr=T]`)는 엔진 스레드(로그 플러시·체크포인트 등)가 쥔 경우다.
`--detail` 의 필터는 `--detail=NAME`(또는 번호), 아니면 첫 단어(대상) 뒤의 두 번째 단어다. 실행 중인 서버나 CS 이름을 보고 추측하지 않으므로 `cub_ctv demodb --detail LOG`, `cub_ctv --detail demodb LOG`, `cub_ctv --detail=LOG demodb` 는 같고, `cub_ctv --detail LOG demodb` 는 LOG 를 대상으로 읽는다. `--replay` 에서는 첫 단어가 필터다. 맞는 CS 가 없으면 `(일치하는 크리티컬 섹션 없음: …)` 을 찍는다.
트랜잭션 표는 항상 붙는다. 클라이언트 신원 레이아웃(CLIENTIDS)은 첫 클라이언트를 볼 때 확정되므로, 클라이언트를 한 번도 보지 못했으면 `(클라이언트 신원 없음: 접속 클라이언트가 없거나 CLIENTIDS 레이아웃 미인식)`, 확정 후 지금 0개면 `(클라이언트 트랜잭션 없음)`, 표를 읽을 수 없으면 사유 한 줄.
CS 표 바로 아래 `스레드 대기: 실스레드 N개, 대기 중 N개 - 락 N · 크리티컬 섹션 N` 한 줄은 지금 엔진 스레드들이 무엇을 기다리는지다(I-18 이 검증된 서버만). 지금 suspend 상태(I-20 의 TS_WAIT)인 스레드만 세고, CS reader 대기는 CS 들의 waitR 합을 넘지 않게 센다(엔진의 resume_status 는 깨어난 뒤에도 마지막 사유를 남기기 때문).

### 2.2 라이브 (`-i`)

```text
라이브: 샘플링 100Hz, 표시 2.0s; Ctrl-C로 종료

--- 11:26:49  (samples=199, dt=2.00s)
ix  name                      occ% occ-bar     wait%   enter/s    wait/s waitR waitW  state
 1  LOG                       96.5 ##########   65.3     730.1     295.3     0     3  1 writer  <-- WAIT
      | owner  DBA@dbhost01 csql pid=2355443 login=cubrid ip=10.0.0.79  thr=4134  trid=1868964
      | wq     thr=4270(tran 14 DBA csql)  [1 of 3 waiting]
 2  LOCATOR_CLASSNAME_TABLE    1.5 ..........    0.0    8035.5     171.4     0     0  none  <-- WAIT
      | owner  DBA@dbhost01 csql pid=2355463 login=cubrid ip=10.0.0.79  thr=4274  trid=1868992
 5  DISK_CHECK                 0.0 ..........    0.0    1242.3       0.0     0     0  none
 7  HEAP_CHNGUESS              0.5 ..........    0.0    3055.3      17.5     0     0  none  <-- WAIT
      | owner  DBA@dbhost01 csql pid=2355478 login=cubrid ip=10.0.0.79  thr=4148  trid=1868991
 9  CT_OID_TABLE               0.0 ..........    0.0    1424.7       6.5     0     0  none  <-- WAIT
13  PARTITION_CACHE            0.0 ..........    0.0     357.3       1.5     0     0  none  <-- WAIT
```

| 컬럼 | 의미 |
|---|---|
| occ% / occ-bar | 윈도우 중 점유 상태로 관측된 샘플 비율 |
| wait% | 대기자가 있던 샘플 비율 |
| enter/s, wait/s | 누적 카운터의 구간 미분; 대기 발생 행은 `<-- WAIT` |
| 소유자 블록 | 윈도우 중 목격된 writer 소유자·대기 큐. 끝은 현재 큐면 `[n of N waiting]`, 지금 큐가 비어 구간 중 목격한 체인을 보이거나 큐가 waitW 보다 길면 `[n seen]`. 목격한 클라이언트가 구간 중 접속을 끊고 그 트랜잭션 슬롯을 다른 클라이언트가 받았으면, 지금 슬롯의 클라이언트가 아니라 목격한 클라이언트를 보인다: 소유자는 구간 시작 때 신원 + ` disconnected`(한글 `접속 종료`), 대기 홉은 `(tran N user program, disconnected)`. 그 클라이언트의 신원이 남지 않았으면(구간 안에서 접속했다 끊음, 라이브 첫 구간) `pid N` 만 |

위는 `-i --narrow` 의 파이프(또는 리다이렉트) 출력이다. `--narrow` 없이 파이프로 내보내면 폭 제한이 없어 소유자 블록이 행 끝에 한 줄로 붙는다(`  [user@host …]  {wq: …}`). 카운터·점유·대기가 모두 없던 행은 생략하고, 전부 없으면 `(유휴 - 크리티컬 섹션 활동 없음)` 한 줄이다.
`-i --detail` 은 파이프에서 움직인 행을 §2.3 세로 레코드로 덧붙인다(`-i --detail LOG` 처럼 이름·번호를 주면 그 CS 만; 라이브 기록 재생도 같음). `-i --trans` 는 §2.5. 풀 집계(§2.4)는 스냅샷에만 있다.

#### 터미널 전체 화면 (라이브·재생 공통) — 요약 화면

터미널에서는 구간마다 아래 세 블록을 제자리 갱신한다. 숫자는 같은 구간의 집계이고, 해석(판정·설명)은 도구가 붙인다.

```text
cub_ctv - demodb (pid 3912964)  02:42:23  구간 1.0s  (100Hz, samples=101)

[판정] 로그 플러시·WAL·읽기 (LOG) 병목  심각도 ***. 중간   지속: 1구간 (최근 1구간 중)
  항목        판단                                           근거
  어디서      로그 플러시·WAL·읽기 (LOG)                     손실 0.78 s/s; 2위 LOCATOR_CLASSNAME_TABLE 0.00 s/s
  누가        엔진 스레드 log-flush                          구간의 64% 보유 (점유 94%)
  얼마나      중간 (3/4)                                     대기 197건/s (진입 934건/s 의 21%), 진입당 획득 0.835 ms
  처리량      데몬 처리 상한 근접                            데몬 보유 64% -> 여유 36%, 진입 934건/s
  피해        클라이언트 세션 4개, 각 구간의 약 5% 대기      tran 6 (3), tran 8 (4), tran 7 (5), tran 5 (9)
  조치        세션 조치 불필요 -> 엔진 쪽을 볼 것            쥔 쪽이 엔진 데몬
  확인할 것   로그 디스크 쓰기 지연(await), log_buffer_size, 그룹 커밋
  디스크/CPU  디스크 쓰기 지연                               log-flush: D 94% R 6% cpu 4%
  화면 밖     SQL 문장; 서비스 영향(SLA)

[대기 발생 순위]
   name                    보호 대상                 occ%  wait%  wait/s  평균ms  쥔 쪽
 1 LOG                     로그 플러시·WAL·읽기        94     50     197   0.835  엔진 log-flush
 2 LOCATOR_CLASSNAME_TABLE 테이블 이름 찾기             2      0      58       -  reader 다수
 3 CT_OID_TABLE            클래스→카탈로그 OID 캐시     0      0       1       -  -
 4 DISK_CHECK              섹터 예약 (checkdb 배타)     0      0       0       -  -
 5 HEAP_CHNGUESS           클래스 캐시 버전 추측        0      0       0       -  -
 6 PARTITION_CACHE         파티션 캐시                  0      0       0       -  -
   나머지 12개: 활동 없음

[스레드 대기]  실스레드 126개, 대기 중 0개

[세션]  클라이언트 트랜잭션 8개, CS 활동 4개
 tran       pid 클라이언트     이 구간에 한 일
    5   3872689 csql           LOG 보유 (6샘플), LOG 대기 (9샘플)
    7   3872701 csql           LOG 보유 (6샘플), LOG 대기 (5샘플)
    8   3872704 csql           LOG 보유 (9샘플), LOG 대기 (4샘플)
    6   3872697 csql           LOG 보유 (9샘플), LOG 대기 (3샘플)
  나머지 4개 세션: 이 구간에 CS 활동 없음

[r] 녹화 (<pid>_<db>_<시각>.ctv)  [l] 한/영  [q] 종료
```

| 블록 | 내용 |
|---|---|
| 판정 | 심각도가 가장 높은 CS(같으면 손실이 큰 순) 하나를 **항목 · 판단 · 근거** 표로 정리한다. `어디서`(손실 1위와 2위 비교) · `누가`(쥔 쪽: 엔진 데몬 / 클라이언트 세션 / reader 다수, 보유율) · `얼마나`(심각도, 대기 건수·비율, **진입당 획득 ms** = 엔진 누적 획득 시간(`total_elapsed`, 진입마다 뮤텍스·대기 큐를 지나 획득하기까지)의 구간 차 ÷ 진입 횟수) · `처리량`(데몬이 쥘 때: 보유율로 본 여유) · `피해`(대기한 클라이언트 세션 수와 각 세션이 그 CS 를 기다리는 것으로 관측된 샘플 비율의 평균 = 대기 관측 샘플 합 ÷ 세션 수 ÷ 구간 샘플 수, 세션 목록) · `조치`(쥔 쪽에 따라 세션/엔진/진입 빈도) · `확인할 것`(CS 별 점검 항목) · `디스크/CPU`(쥔 스레드의 OS 상태: D = 디스크 I/O 대기 비율, R = 실행 비율, cpu = 그 스레드의 CPU 사용률; 로그 볼륨이 블록 장치면 `await`·`util` 도) · `화면 밖`(이 화면으로 알 수 없는 것). `손실`은 그 누적 획득 시간의 구간 차 ÷ 구간 길이(초당 몇 초를 CS 획득에 썼나; 대기 큐에 서지 않은 내부 뮤텍스 경합도 포함). 심각도 `*`: 손실 ≥1초/s 또는 대기율 ≥75% = 4(높음), ≥0.3초/s 또는 ≥40% = 3(중간), 대기 있음 또는 손실 ≥0.05초/s = 2(낮음). `지속`은 최근 10구간 중 같은 CS가 1위였던 구간 수 |
| 대기 발생 순위 | 활동이 있던 CS 를 심각도→손실→대기/s→점유 순으로. `보호 대상`은 CS 가 지키는 것의 한글/영문 설명. `쥔 쪽`은 구간 중 가장 자주 writer 로 목격된 트랜잭션, 엔진 스레드면 OS 스레드 이름(`엔진 log-flush`), writer 없이 점유만 있으면 `reader 다수` |
| 스레드 대기 | 실스레드 수와 지금 무엇을 기다리는 스레드 수를 사유별로(락·페이지 래치·빈 버퍼·크리티컬 섹션·로그 writer·접속 큐 …). 스레드 엔트리의 `resume_status` 를 구간마다 읽는다(I-18). CS 경합이 없을 때는 판정 줄 아래에 "CS 밖에서 대기: 락 N (cubrid lockdb), 페이지 래치 N, 빈 버퍼 N" 으로 다음에 볼 곳을 준다 |
| 세션 | 구간 중 CS 를 쥐거나 기다린 트랜잭션만, 대기 많은 순. 한 행 = 트랜잭션 슬롯 × 그 슬롯의 클라이언트: 구간 중 접속을 끊은 클라이언트는 `이 구간에 한 일` 앞에 `(접속 종료) `(영문 `(disconnected) `)를 붙여 따로 한 줄로, `pid` 는 그 클라이언트의 pid. `클라이언트` 열은 접속 프로그램: `csql`, CAS 는 `<브로커>:cas<번호>`(CAS 가 서버에 등록한 이름 `<브로커>_cub_cas_<n>` 에서), 그 밖은 프로그램명. 대기한 세션은 **밑줄**, 보유만 한 세션은 **반전**. 나머지는 건수. 행 수는 터미널 높이에 맞춤(최대 24) |

CAS 접속이 섞인 구간의 `[세션]` 블록(브로커 broker1 의 CAS 3개가 DDL 을 돌리는 중):

```text
[세션]  클라이언트 트랜잭션 9개, CS 활동 3개
 tran       pid 클라이언트     이 구간에 한 일
    1   3577839 broker1:cas2   LOG 보유 (2샘플), LOG 대기 (1샘플)
    3   3577840 broker1:cas3   LOG 대기 (1샘플), LOCATOR_CLASSNAME_TABLE 보유 (1샘플), LOG 보유 (1샘플)
    2   3577838 broker1:cas1   LOG 대기 (1샘플), LOG 보유 (1샘플)
  나머지 6개 세션: 이 구간에 CS 활동 없음
```

화면 크기: 그리는 순간의 터미널 크기에 맞춰 배치한다. 창 크기를 바꾸면 다음 갱신부터, 기록 재생은 재생하는 터미널 기준이다. 약 120칸 이상이면 위 예시 그대로다. 그보다 좁으면 판정 표의 근거를 판단 바로 뒤나 다음 줄로 내리고(단어 단위 줄바꿈), `[대기 발생 순위]` 의 `보호 대상` 열을 빼고 `쥔 쪽` 은 화면 끝에서 자르며, `[세션]` 줄도 화면 끝에서 자른다. 80칸 이상이면 터미널 폭을 넘는 줄이 없다. 높이가 모자라면 아랫부분을 `... (터미널이 작음: 늘리면 나머지 표시)` 로 자른다(줄바꿈된 줄도 화면 줄 수로 센다).

녹화: 화면에서 `r` 를 누르면 현재 디렉터리에 `<pid>_<db>_<YYYYmmdd-HHMMSS>.ctv` 를 만들어 구간마다 기록하고, 좌측 상단에 `[녹화중]`(영문 `[REC]`), 하단에 파일명과 `[r] 녹화 중지` 가 표시된다. 다시 `r` 로 중지(파일은 완성된 구간까지 유지; 첫 구간이 끝나기 전에 중지하면 빈 파일을 지운다). 같은 초에 이름이 겹치면 `-2`..`-9` 접미사를 붙이고, 파일 이름의 DB 이름은 영숫자·`_`·`-`·`.` 외 문자를 `_` 로 바꾼다. `-i --trans`(`--record` 없음)에서는 샘플링하지 않으므로 `r` 은 녹화하지 않고 경고만 낸다. `--record FILE` 로 시작했으면 그 파일이 끝까지 기록되고 `r` 은 무시된다(푸터에 `(--record)` 표시). 파일 형식은 `--record` 와 같아 `--replay` 로 재생한다(§2.7).

대기 세션은 **목격한 순간** 트랜잭션 번호를 읽어 두므로 구간이 끝난 뒤에도 신원이 남는다. 색상(터미널 한정, `--plain` 으로 해제): 헤더 청록, 심각도 3 이상 빨강, 2 호박, 안내 회색. 파이프·JSON 출력에는 이스케이프가 없다. 메시지 언어는 `--ko`/`--en`(기본 LC_ALL → LC_MESSAGES → LANG). 한국어 메시지는 **UTF-8 전용**이다. 로캘 문자셋이 UTF-8 이 아니면(`ko_KR.eucKR`, 코드셋 없는 `ko_KR` 등) `--ko` 를 줘도 영문으로 내고 경고 1줄(`charset is not UTF-8: messages in English …`)을 낸다. 이때 하단의 `[l]` 안내도 빠진다. 영문 출력은 ASCII 만 쓰므로 EUC-KR 터미널에서도 깨지지 않는다.

### 2.3 `--detail`

```text
*************************** 1. LOG ***************************
state            : 1 writer         waitR/waitW : 1 / 1
owner tran       : 0 (system)
owner thread     : 4301
waiting writers  : thr 4290 (tran 13 DBA csql)
                   [1 of 1 waiting]
nenter / nwait   : 1060745 / 224577      reenter : 5904
total / max wait : 214479.0 ms / 502.005 ms
```

`--detail LOG` 처럼 이름이나 번호로 한 CS 만, 인자가 없으면 18개 전부를 세로 레코드로. 소유자가 클라이언트면 `owner client : DBA@dbhost01  program csql  login cubrid  ip 10.0.0.79  pid 2355439` 줄이 더 붙는다.

### 2.4 풀 집계 (스냅샷 하단)

스냅샷은 CS 표·트랜잭션 표 다음에 sync_Stats 풀(엔진이 통계 블록을 할당한 모든 동기화 객체) 중 CS 가 아닌 것(RWLOCK/RMUTEX/MUTEX)을 종류별·이름별로 묶어 덧붙인다. 풀의 CSECT 엔트리는 CS 표와 같은 통계 블록이라 다시 찍지 않는다. 서버가 `sync_Stats` 를 내보내지 않으면 풀 절 자리에 `[풀] 미수집 (sync_Stats 미노출)` 한 줄만 찍힌다(stderr 경고 없음, 기록·재생 동일).

```text
[풀] RWLOCK: 2엔트리 (이름별 그룹)
  name                                     n       nenter     max_ms
  CSS_RWLOCK_ACTIVE_CONN_ANCHOR            1         1139      0.000
  CSS_RWLOCK_FREE_CONN_ANCHOR              1         1704      0.000

[풀] RMUTEX: 513엔트리 (이름별 그룹)
  name                                     n       nenter     max_ms
  CONN_ENTRY                             408    181340682      2.000
  TDES_TOPOP                             105      5145644      1.001
```

| 컬럼 | 의미 |
|---|---|
| n | 같은 이름의 인스턴스 수 (CONN_ENTRY 408 = 접속 엔트리마다 하나) |
| nenter | 인스턴스들의 획득 시도 합 |
| max_ms | 인스턴스들 중 최대 획득 지연 |

RWLOCK/RMUTEX 는 엔진이 nwait 를 집계하지 않으므로 nwait 컬럼이 없다. 스냅샷에만 있고 라이브(`-i`)와 라이브 기록에는 없다.

### 2.5 `--trans` (무접속 tranlist)

```text
대상: pid=1229138 db=demodb lib=/opt/cubrid/lib/libcubrid.so.11.5
레이아웃: stride=160B count=18 stats_off=152 (런타임 검증 통과)
트랜잭션 (메모리 판독, SQL 무접속):
 idx     trid  user       program          login      host             client_ip           cpid
   1  1860887  DBA        csql             cubrid     dbhost01         10.0.0.79        2355406
   2  1860889  DBA        csql             cubrid     dbhost01         10.0.0.79        2355410
   3  1860888  DBA        csql             cubrid     dbhost01         10.0.0.79        2355414
   (… 18행)
```

| 컬럼 | 의미 |
|---|---|
| idx | 트랜잭션 인덱스 (CS 표의 `tran`, 소유자 블록의 `tran N` 과 같은 번호) |
| trid | 현재 트랜잭션 ID (`cubrid tranlist` 의 Transaction id; 검증을 통과하지 못하면 `-`) |
| user … cpid | CLIENTIDS: DB 사용자, 프로그램, OS 로그인, 호스트, 클라이언트 IP·PID |

클라이언트가 붙지 않은 슬롯과 해제된 슬롯은 표시하지 않는다. 표를 읽을 수 없으면(강등 `DEG_TRAN`) 사유를 출력하고 종료 코드 1.

`--trans` 만 주면(`--record`·`--dump` 없음) 스레드 엔트리를 탐색하지 않으므로 `레이아웃:` 줄에 `threads=` 가 없다. `-i --trans` 는 화면 대신 구간마다 같은 표를 스크롤로 낸다(표 형식은 라이브 기록 재생의 `--trans` 와 같다. 단 `--record` 없이는 샘플링하지 않으므로 배너가 다르고 samples=0):

```text
라이브: 1.0s 마다 트랜잭션 표; Ctrl-C로 종료

--- 11:26:51  (samples=0, dt=1.00s)
트랜잭션 (메모리 판독, SQL 무접속):
 idx     trid  user       program          login      host             client_ip           cpid
   1  1872484  DBA        csql             cubrid     dbhost01         10.0.0.79        2355406
   2  1872486  DBA        csql             cubrid     dbhost01         10.0.0.79        2355410
   (… 18행)

--- 11:26:52  (samples=0, dt=1.00s)
트랜잭션 (메모리 판독, SQL 무접속):
 idx     trid  user       program          login      host             client_ip           cpid
   1  1874884  DBA        csql             cubrid     dbhost01         10.0.0.79        2355406
   (… 18행)
```

구간마다 trid 가 바뀌는 것은 각 세션이 1초에 여러 트랜잭션을 커밋하기 때문이다. 터미널 라이브 화면의 `[세션]` 블록이 이 표의 요약(CS 를 쥐거나 기다린 세션만)이다.

### 2.6 `--json`

스냅샷 1문서, 최상위 키 순서: `tool, pid, db, ts, layout{stride,count,stats_off}, degraded, cs, thread_wait, log_disk, trans_available, identities_locked, trans`.
`cs` 원소: `ix, name, rwlock, waitR, waitW, tran, thr, owner, wq[{thr,tran}], nenter, nwait, nreenter, total_ms, max_ms`. `owner` 는 writer 가 있을 때만이며, 클라이언트면 `{tran,user,program,login,host,client_ip,cpid}`, 엔진 스레드면 `{system:true,comm}`. 구간 중 관측한 체인을 보일 때는 `wq_seen` 이 붙고, 통계 블록이 검증되지 않은 행에는 `nenter … max_ms` 가 없다. `trans` 원소는 `{idx,trid,user,program,login,host,client_ip,cpid}` 이고 trid 가 검증되지 않으면 `null`.
라이브: 최상위 `tool, pid, db, ts, layout, degraded, samples, dt, cs, thread_wait, log_disk` 를 윈도우당 1줄. 행별로 `occ_pct, wait_pct, enter_s, wait_s` 가 붙는다(`trans` 없음).
`--trans --json` 은 `cs` 없이 `trans_available, trans` 를 담는다.
`--replay <라이브 기록> --trans --json` 은 윈도우마다 `trans` 만 담은 1줄을 낸다.

### 2.7 `--record` / `--replay` / `--dump`

```sh
cub_ctv demodb -i --record incident.rec   # 현장: 보면서 기록 (Ctrl-C 로 끝내도 완성된 구간은 남음)
cub_ctv demodb -i                         # 또는 화면에서 [r]: <pid>_<db>_<시각>.ctv 자동 생성/중지
cub_ctv --replay incident.rec             # 어디서든 다시 보기 (기록 당시 간격)
cub_ctv --replay incident.rec --detail --narrow | less   # 보기 옵션을 바꿔 다시 보기
cub_ctv demodb --dump detail.json         # 디테일 덤프 (재생 불가)
```

| | 기록 파일 (`--record`) | 디테일 덤프 (`--dump`) |
|---|---|---|
| 용도 | 본 화면을 다시 보기 | 그 순간 알아낼 수 있는 정보 전부 보관 |
| 담는 것 | 화면을 그린 값(CS 행, 라이브 집계, 소유자 신원·스레드, 대기 체인, 트랜잭션 표, 풀 그룹) | 위 전부 + 오프셋·심볼·라이브러리, 트랜잭션 슬롯 전부, 사용 중 스레드 엔트리 전부, 풀 엔트리 전부 |
| 형식 | 줄 단위 텍스트 (`CTVREC 5` 로 시작) | JSON 1문서 (`"format":"cub_ctv detail dump"`) |
| 크기 (실측) | 스냅샷 약 4KB, 라이브 구간당 약 4KB (클라이언트 32개 기준, 신원·활동 줄 수에 비례) | 약 100KB |
| `--replay` | 가능. 터미널이면 키로 조작(아래) | 거부 (이유 표시) |

라이브 기록을 터미널에서 재생하면 화면 하단에 `재생 23/130  x1   [space] 일시정지  [<] [>] 구간  [+] [-] 속도  [l] 한/영  [q] 종료`(영문 `REPLAY …`)가 붙고 키를 받는다. 일시정지 중에는 `x1` 뒤에 `  일시정지`(`  PAUSED`)가 붙는다.

| 키 | 동작 |
|---|---|
| space | 일시정지 / 재개 |
| ← → (또는 `,` `.` / `<` `>` / `b` `n`) | 이전 / 다음 구간으로 이동 (이동하면 일시정지) |
| `+` `-` | 속도 2배 / 절반 (x1/16 ~ x64) |
| `l` | 한/영 전환 (현재 구간을 다른 언어로 다시 그림; 라이브 화면도 같음) |
| (창 크기 변경) | 현재 구간을 새 크기로 즉시 다시 그림 (라이브 화면도 같음) |
| `q` | 종료 (터미널 복원) |
| 그 밖의 키 (Esc·↑↓·Home 등 포함) | 무시 (구간을 넘기지 않음) |

기록 끝에 닿으면 마지막 구간에서 멈춘다. stdin 이 터미널이 아니면(`< /dev/null`, 파이프 입력) 키를 받을 수 없으므로 전체 화면 대신 스크롤 출력으로 재생한다. 기록·덤프 파일은 권한 0600 으로 만들고, 그 이름이 심볼릭 링크면 만들지 않는다. 음수 `--replay-speed` 는 `--replay-speed 값 오류` 로 거부된다. 파이프·`--json`·`--trans` 재생은 키 없이 기록 간격(`--replay-speed`)으로 흘러간다.

기록 파일 줄 형식 (공백 구분, 문자열은 `%XX` 이스케이프, 빈 문자열은 `~`):

| 줄 | 내용 |
|---|---|
| `H` | pid, 라이브 여부, hz, 표시 주기, 스레드 수, 강등 비트, stride·count·stats_off, DB, 라이브러리, 트랜잭션 강등 사유 |
| `F` | 프레임 시작: 시각, CS 수, 샘플 수, 구간 길이, 트랜잭션 가용·CLIENTIDS 잠금·trid 검증·슬롯 수, 풀 상태, 풀 절단 위치 |
| `C` | CS 한 행: 상태·대기자·tran·소유자 tid·대기 큐 머리·카운터 + 라이브 집계(점유·대기 샘플, 목격 소유자·대기 큐, 직전 nenter/nwait/total_ms, 목격 소유자의 클라이언트 pid(−1 모름)) |
| `K` | 신원 키프레임 여부: 1 = 이 프레임이 모든 신원을 담음(기록 첫 프레임과 60프레임마다), 0 = 직전 프레임 대비 바뀐 것만(`I` 갱신·`X` 제거). `K` 없는 프레임은 모든 신원을 담은 것으로 읽음 |
| `I` | 트랜잭션 신원: idx, trid, 클라이언트 pid(cpid), info, user, program, login, host, ip |
| `X` | 직전 프레임에 있던 신원 idx 가 사라짐(`K 0` 프레임) |
| `J` | 구간 중 목격됐으나 프레임 순간에는 슬롯을 떠난 클라이언트의 신원(`I` 와 같은 항목, 구간 시작 때 값). 키프레임과 무관하게 프레임마다 그 프레임의 전부 |
| `T` / `Q` / `S` | 소유자 tid → 스레드 칸 / 대기 큐 머리 → 칸 목록 / 칸 → tran, 그 tran 의 클라이언트 pid(−1 모름) |
| `G` | 풀 그룹: 종류, 개수, nenter 합, max_ms, 이름 |
| `W` | 구간 중 활동: cs, 종류(0 보유 / 1 대기), tran, 스레드 칸, 샘플 수, 목격 당시 클라이언트 pid(−1 모름) — 요약 화면의 세션 블록과 판정 근거 |
| `A` | 활동 표(4096칸, 그중 256칸은 보유 목격 전용)가 차서 보관하지 못한 목격 수 — 있을 때만, 세션 블록 머리에 표시 |
| `B` | 그 순간의 강등 비트 |
| `V` | 판정 지속성 이력: 개수, (몇 구간 전 1~9, 그때 최상위 CS 또는 −1) 쌍 — 중간에 시작한 기록도 라이브와 같은 "최근 N 구간 중 M" 을 보인다(없는 기록은 재생 중 쌓은 이력 사용) |
| `P` | 구간 중 처음 목격한 대기 큐: cs, 칸 수, 칸 목록 |
| `O` | 점유 스레드 OS 상태: 칸, tid, 샘플·R·D·기타 수, CPU 틱 전/후, 이름, 관측 ms |
| `D` | 로그 장치 diskstats: await ms, util %, 장치명 |
| `R` | 대기 사유 히스토그램: 실스레드 수, 코드 수, 코드별 개수 |
| `E` | 프레임 끝 (끝이 없는 마지막 프레임은 재생 때 경고 후 무시) |

검증: 프레임의 CS 수는 헤더와 같고 그 수만큼의 `C` 줄이 모두 있어야 하며, 줄마다 토큰 수가 형식과 정확히 맞아야 하고(프레임 밖 줄·프레임 안의 새 `F` 는 거부), 실수 값은 유한해야 하며(nan·inf 거부), `%XX` 는 16진수 2자리일 때만 복원하며(아니면 `%` 를 글자 그대로 둠), `Q` 의 칸 수는 토큰 수와 같아야 하고 64칸까지만 저장, `I` 는 idx 엄격 오름차순(중복 불가; 재생의 구간 이동은 앞쪽 키프레임부터 읽어 신원을 맞춤), `H` 는 토큰 13개·CS 수 1..64, `C` 는 인덱스 범위 안·중복 불가, `K` 는 0/1 만, `P` 는 칸 수 ≤ 64·토큰 수 일치, `W` 의 cs 는 범위 안이고 종류는 0/1·샘플 수는 0..프레임 샘플 수, `R` 은 코드 수 28, `V` 는 9쌍 이하에 거리 1~9·CS 범위 안이어야 하며, 라이브 프레임은 샘플 1 이상, 라이브 구간 길이는 0 초과 1e7초 이하·라이브 기록의 헤더 표시 주기는 0.1초 이상 3600초 이하여야 한다.

## 3. 빠른 확인: 이 서버에서 추적이 되는가 (`tests/t9_quick_trace.sh`)

```sh
tests/t9_quick_trace.sh demodb            # 15초 부하, 워커 4, 한글 화면
tests/t9_quick_trace.sh demodb 15 4 --en  # 영문 화면
```

스크립트가 하는 일: ① 스냅샷 1회로 서버 판독·레이아웃 검증 확인 → ② 짧은 경합 부하(병렬 INSERT + CREATE/DROP, `cub_ctv_demo_*`
임시 테이블만 사용, 끝나면 삭제) → ③ 부하 중 가짜 터미널에서 라이브 요약 화면 한 구간을 `--record` 와 함께 캡처 → ④ 판정문·대기 세션·
기록(활동 줄) 존재를 검사 → ⑤ 부하 없이 라이브 화면을 다시 띄워 `r` 두 번으로 녹화 시작·중지(파일·`[REC]` 표시)를 검사해 PASS/FAIL. 실제 실행 결과(demodb, 11.5):

```text
== target: target: pid=3912964 db=demodb lib=/opt/cubrid/lib/libcubrid.so.11.5
== layout: stride=160B count=18 stats_off=152 (runtime-probed)  threads=4314
== load: 4 workers, 15s (INSERT + DDL churn on cub_ctv_demo_* tables)
== live summary window (one frame, escapes stripped)
[녹화중] cub_ctv - demodb (pid 3912964)  02:42:23  구간 1.0s  (100Hz, samples=101)

[판정] 로그 플러시·WAL·읽기 (LOG) 병목  심각도 ***. 중간   지속: 1구간 (최근 1구간 중)
  항목        판단                                           근거
  어디서      로그 플러시·WAL·읽기 (LOG)                     손실 0.78 s/s; 2위 LOCATOR_CLASSNAME_TABLE 0.00 s/s
  누가        엔진 스레드 log-flush                          구간의 64% 보유 (점유 94%)
  얼마나      중간 (3/4)                                     대기 197건/s (진입 934건/s 의 21%), 진입당 획득 0.835 ms
  처리량      데몬 처리 상한 근접                            데몬 보유 64% -> 여유 36%, 진입 934건/s
  피해        클라이언트 세션 4개, 각 구간의 약 5% 대기      tran 6 (3), tran 8 (4), tran 7 (5), tran 5 (9)
  조치        세션 조치 불필요 -> 엔진 쪽을 볼 것            쥔 쪽이 엔진 데몬
  확인할 것   로그 디스크 쓰기 지연(await), log_buffer_size, 그룹 커밋
  디스크/CPU  디스크 쓰기 지연                               log-flush: D 94% R 6% cpu 4%
  화면 밖     SQL 문장; 서비스 영향(SLA)

[대기 발생 순위]
   name                    보호 대상                 occ%  wait%  wait/s  평균ms  쥔 쪽
 1 LOG                     로그 플러시·WAL·읽기        94     50     197   0.835  엔진 log-flush
 2 LOCATOR_CLASSNAME_TABLE 테이블 이름 찾기             2      0      58       -  reader 다수
 3 CT_OID_TABLE            클래스→카탈로그 OID 캐시     0      0       1       -  -
 4 DISK_CHECK              섹터 예약 (checkdb 배타)     0      0       0       -  -
 5 HEAP_CHNGUESS           클래스 캐시 버전 추측        0      0       0       -  -
 6 PARTITION_CACHE         파티션 캐시                  0      0       0       -  -
   나머지 12개: 활동 없음

[스레드 대기]  실스레드 126개, 대기 중 0개

[세션]  클라이언트 트랜잭션 8개, CS 활동 4개
 tran       pid 클라이언트     이 구간에 한 일
    5   3872689 csql           LOG 보유 (6샘플), LOG 대기 (9샘플)
    7   3872701 csql           LOG 보유 (6샘플), LOG 대기 (5샘플)
    8   3872704 csql           LOG 보유 (9샘플), LOG 대기 (4샘플)
    6   3872697 csql           LOG 보유 (9샘플), LOG 대기 (3샘플)
  나머지 4개 세션: 이 구간에 CS 활동 없음

녹화 중 /tmp/tmp.XXXXXXXXXX/quick.rec (--record)   [l] 한/영  [q] 종료

   1 LOG                        enter +67908    nwait +12252
   2 LOCATOR_CLASSNAME_TABLE    enter +357647   nwait +3317
   5 DISK_CHECK                 enter +56016    nwait +0
   7 HEAP_CHNGUESS              enter +96334    nwait +60
== [r] recording: 3912964_demodb_20261116-024236.ctv, 2 windows, stopped
t9 PASS: probe ok, verdict with waiting sessions, record 4 windows / 42 activity lines, [r] record/stop
```

읽는 법:

| 줄 | 뜻 |
|---|---|
| `target` / `layout` | 서버를 찾아 메모리 구조 검증을 통과했다(threads=4314 는 스레드 엔트리 배열 인식). 여기서 멈추면 추적 불가(사유가 함께 출력됨) |
| `[판정]` 표 | 1초 구간에서 심각도가 가장 높은 CS(같으면 손실이 큰 순)를 항목별로. 이 예: LOG 를 엔진 스레드 log-flush 가 구간의 64% 보유했고(`누가`), 플러시 관련 대기가 197건/s(진입 934건/s 의 21%), 진입당 획득 0.835ms, 초당 0.78초 손실이다(`얼마나`·`어디서`). 데몬 여유는 36%(`처리량`: 데몬 처리 상한 근접), 세션 4개가 각각 구간의 약 5%를 대기했다(`피해`). 쥔 쪽이 데몬이므로 세션 조치는 불필요하다(`조치`). 쥔 스레드의 D(디스크 대기) 94% 가 로그 디스크 쓰기 쪽을 가리킨다. `디스크/CPU` 행은 쥔 스레드의 D(디스크 대기)·R 비율과 CPU 로 디스크 지연·CPU 포화·짧은 보유 빈발을 가른다 |
| `지속` | 최근 10구간 중 같은 CS 가 1위였던 구간 수. 이 캡처는 기록 시작 후 1번째 구간이라 1/1이고, 라이브를 계속 보면 N/10 으로 누적된다. 구간 단위로 세므로 재생에서 같은 구간을 멈추거나 되돌려 다시 그려도 늘지 않는다 |
| `[대기 발생 순위]` | 활동이 있던 CS 만, 심각도→손실 순. `보호 대상`이 CS 이름의 뜻. LOCATOR 는 초당 58번 대기했지만 획득 지연 증가가 측정되지 않을 만큼 작다(평균ms `-`) — 진입 빈도가 높을 뿐 병목이 아니다 |
| `[세션]` | 구간 중 CS 를 쥐거나(보유) 기다린(대기) 트랜잭션과 그 횟수(100Hz 샘플 수). 밑줄 = 대기한 세션, 반전 = 보유만 한 세션 |
| `enter +… nwait +…` | 부하 스크립트의 전후 카운터 증분(엔진 누적값) — 부하가 실제로 그 CS 를 움직였다는 교차 확인 |
| `[r] recording` | 라이브 화면에서 `r` 로 녹화를 열고 닫았고(좌측 상단 `[REC]` 확인), 파일에 완성된 구간이 남았다 |
| `t9 PASS` | 판독·판정·대기 세션 식별·기록·녹화 키까지 모두 됨 |

유휴 서버에서 부하 없이 라이브를 띄우면 `[판정] 크리티컬 섹션 경합 없음` 과 `활동 없음` 만 나온다. 그것도 정상이다(추적은 되고 경합이 없는 것).

## 4. 해석 가이드

| 조건 | 판정 | 다음 행동 |
|---|---|---|
| wait% > 0 ∨ wait/s > 0 (`<-- WAIT`) | 경합 발생 | 아래로 분류 |
| occ% 높음 + enter/s 낮음 | 장기 점유 | `[system]` = 로그 플러시·체크포인트, 클라이언트면 `owner`/`thr=` 로 특정 |
| enter/s 높음 | 고회전 | 워크로드 측 대응 |
| nwait·max_ms 누적 큼 (스냅샷) | 과거 병목 | 재현 시 `-i`, 상시 수집은 `--json` |
| waitW > 0 지속 + writer 고정 + enter/s ≈ 0 | 행(hang) 의심 | `wq` 체인으로 주체 특정 (접속 불능 상태에서도 판독) |

| CS | 올리는 활동 |
|---|---|
| LOG | 로그 플러시, 더티 페이지 쓰기 전 WAL 보장, 체크포인트, 아카이브 (`[system]` 장기 점유는 정상). 일반 로그 기록·커밋 대기는 별도 뮤텍스/조건변수라 여기 안 잡힘 |
| LOG_ARCHIVE | 아카이브 로그 접근 (CDC·백업) |
| LOCATOR_CLASSNAME_TABLE | 클래스명→OID 조회 (접속 폭주·DDL churn) |
| TRAN_TABLE / ACCESS_STATUS | 트랜잭션 슬롯 할당·반납(접속·종료) / 로그인 |
| HEAP_CHNGUESS | 클래스 객체 전달(짧은 트랜잭션 반복), DDL |
| CT_OID_TABLE / PARTITION_CACHE | DDL / 파티션 DML |

| 스레드 대기 사유 (`[스레드 대기]`) | 뜻 | 다음에 볼 것 |
|---|---|---|
| 락 | 트랜잭션 락(행·테이블) 대기 | `cubrid lockdb`, 락 타임아웃·교착 로그 |
| 페이지 래치 | 페이지 버퍼 BCB 래치 대기 | 핫 페이지·인덱스 경합, 버퍼 크기 |
| 빈 버퍼 | victim 페이지가 날 때까지 대기 | `data_buffer_size`, 플러시 데몬 |
| 로그 writer | 로그 writer(HA 복제 전송) 대기 | 복제 상태 |
| 크리티컬 섹션 | 이 도구의 CS 표에 보이는 대기 | 위 판정 |
| 접속 큐 / DWB 큐 / 동시성 슬롯 | 접속 수락, 이중 쓰기 버퍼, 동시 실행 슬롯 대기 | 접속 폭주, DWB 설정, 동시성 제한 |
| 작업 큐 | 작업 큐 대기 | 워커 풀 포화 |
| 클래스 표현 | 클래스 표현(representation) 대기 | DDL 진행 여부 |
| sleep | `SLEEP()` 함수 등 의도적 대기 | 해당 질의 |

읽기 SELECT 는 페이지 버퍼 래치(BCB)를 타므로 CS 에 보이지 않는다.
