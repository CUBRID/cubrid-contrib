# CUBRID 크리티컬 섹션 이해하기 (Critical Sections Explained)

대상: CUBRID 서버 내부를 처음 보는 엔지니어. 이 문서는 cub_ctv 화면에 나오는 이름과 숫자가 엔진 안에서
무엇을 뜻하는지, 그리고 DBMS 가 왜 그런 장치를 두는지를 설명한다. 근거는 `src/thread/critical_section.{c,h}`
와 그 호출처(11.5)다.

## 1. 배경: DBMS 서버는 왜 "잠금"을 여러 층으로 두는가

cub_server 는 프로세스 하나에 스레드 수천 개(demodb 기준 스레드 엔트리 4,314개)가 돈다. 클라이언트 요청마다
워커 스레드가 붙고, 그 밖에 로그 플러시·체크포인트·vacuum 같은 데몬 스레드가 있다. 이 스레드들은 **같은 메모리**를
같이 만진다: 트랜잭션 테이블, 로그 버퍼, 페이지 버퍼, 클래스 이름 → OID 해시, 카탈로그 캐시 등. 두 스레드가 같은
구조를 동시에 고치면 깨지므로, 엔진은 "한 번에 한 스레드만(또는 읽는 스레드 여럿만)" 들어가게 하는 장치를 둔다.

그런데 장치가 하나가 아니다. **무엇을, 얼마나 오래 지키느냐**에 따라 층이 다르다.

| 층 | 지키는 것 | 보유 시간 | 타임아웃 | 교착 처리 | 보는 도구 |
|---|---|---|---|---|---|
| 트랜잭션 락 (lock manager) | 행·테이블 같은 **데이터 객체** | 트랜잭션이 끝날 때까지 (초~분) | 있음 (`lock_timeout`) | 교착 탐지 후 한쪽 중단 | `cubrid lockdb`, `tranlist` |
| 페이지 래치 (page buffer, BCB) | 버퍼에 올라온 **페이지 한 장** | 그 페이지를 읽고 쓰는 **한 연산 동안** (µs~ms) | 있음 (`page_latch_timeout_in_msecs`, 기본 300초, 숨은 파라미터) | 순서 규칙으로 회피 | 없음 (타임아웃만 서버 로그에) |
| **크리티컬 섹션** (이 문서) | 엔진 **전역 구조체** 18종 (로그 버퍼 끝, 트랜잭션 테이블, 클래스명 해시 …) | 그 구조체를 만지는 **짧은 구간** (µs) | **없음** (무한 대기) | 설계상 순서 고정으로 회피 | `SHOW CRITICAL SECTIONS`, `;info csstat`, cub_ctv |
| 뮤텍스·rwlock·lock-free | 그보다 작은 내부 구조 (연결 엔트리, 캐시 버킷 …) | 아주 짧게 | 없음 | — | `;info csstat` (일부), cub_ctv 스냅샷의 풀 집계 |

트랜잭션 락은 트랜잭션이 끝날 때까지, 페이지 래치는 한 페이지 연산 동안, 크리티컬 섹션은 전역 구조체 하나를 고치는
동안만 쥔다. 크리티컬 섹션은 보유 시간이 µs 단위지만 **여러 세션의 요청이 같은 전역 구조체를 거치므로**, 보유자가 한 번
오래 쥐면 그 뒤의 대기자 전원이 획득 지연을 겪는다. 그래서 보유 시간이 짧은데도 병목이 될 수 있다.

## 2. 크리티컬 섹션이란 무엇인가

### 2.1 구조체

엔진에는 `csectgl_Critical_sections[18]` 이라는 정적 배열이 있고, 원소 하나가 크리티컬 섹션 하나다.

```c
typedef struct sync_critical_section
{
  const char *name;                     /* "LOG" 같은 이름 */
  int cs_index;                         /* 0..17 */
  pthread_mutex_t lock;                 /* 아래 필드를 고칠 때 잡는 내부 뮤텍스 (아주 짧게) */
  int rwlock;                           /* >0 = reader 수, <0 = writer 보유 (재진입 깊이), 0 = 비어 있음 */
  unsigned int waiting_readers;         /* 지금 기다리는 reader 수 */
  unsigned int waiting_writers;         /* 지금 기다리는 writer 수 */
  pthread_cond_t readers_ok;            /* reader 를 한꺼번에 깨우는 조건 변수 */
  THREAD_ENTRY *waiting_writers_queue;  /* writer 대기 줄 (스레드 엔트리 연결 리스트, FIFO) */
  THREAD_ENTRY *waiting_promoters_queue;/* reader→writer 승격 대기 줄 */
  thread_id_t owner;                    /* writer 로 쥔 스레드 */
  int tran_index;                       /* writer 로 쥔 트랜잭션 번호 (0 = 엔진 데몬) */
  SYNC_STATS *stats;                    /* 누적 통계 */
} SYNC_CRITICAL_SECTION;
```

읽기/쓰기 락(reader-writer lock)이다. **reader 는 여럿이 동시에**, **writer 는 혼자만** 들어간다. `rwlock` 하나가
상태를 담는다: 3이면 reader 3명, −1이면 writer 1명(−2면 같은 writer 가 두 번 재진입), 0이면 아무도 없음.

### 2.2 18종과 각각이 지키는 것

| ix | 이름 | 지키는 것 (쉽게) | 어떤 작업이 들어오나 | 주 호출처 |
|---|---|---|---|---|
| 0 | WFG | 대기 그래프(wait-for graph) | 11.x 에서는 진입 없음 — 사용 코드가 `ENABLE_UNUSED_FUNCTION`/`#if 0` 안에 있음. 교착 탐지는 락 매니저가 직접 한다 | wait_for_graph.c |
| 1 | **LOG** | 로그 파일 쓰기와 로그 상태(헤더·아카이브·플러시 범위) | **로그 플러시**, 더티 페이지를 쓰기 전 **WAL 보장 플러시**, 체크포인트 양끝, 아카이브·백업·CDC, 복구, **로그 페이지 읽기**(롤백·세이브포인트·커밋 시 postpone 처리·CDC·HA 로그 전송·flashback 은 reader 로 진입. 읽을 페이지가 아직 prior list 에 있으면 writer 로 잡고 prior list 를 로그 버퍼로 내림 — MVCC 이전 버전을 로그에서 읽을 때도 같음). (일반 로그 기록은 prior list 뮤텍스, 커밋 대기는 그룹 커밋 조건변수 — 둘 다 CS 밖) | `LOG_CS_ENTER` 매크로 — log_append.cpp, log_manager.c, log_page_buffer.c 등 |
| 2 | **LOCATOR_CLASSNAME_TABLE** | 테이블 이름 → 클래스 OID 해시 | 테이블 이름을 쓰는 거의 모든 질의의 준비 단계, CREATE/DROP | locator_sr.c |
| 3 | QPROC_QUERY_TABLE | 트랜잭션별 질의 엔트리 배열 할당 | 서버 기동·종료, 트랜잭션 테이블 확장 때만 (런타임에는 사실상 진입 없음. 질의 시작·종료는 진입하지 않음) | query_manager.c |
| 4 | QPROC_LIST_CACHE | 질의 결과(list file) 캐시 | 결과 캐시 조회·등록 | list_file.c |
| 5 | DISK_CHECK | 섹터 예약·해제 vs checkdb 배타 | 파일·임시 파일 생성·확장·삭제의 섹터 예약·해제와 볼륨 추가(reader), checkdb 의 디스크 검사 `disk_check`/`disk_check_volume`(writer) | disk_manager.c (복구·undo 함수에만 "대기 0초 시도" 진입이 있음) |
| 6 | CNV_FMT_LEXER | 값/문자열 포맷 변환 렉서 | 진입 없음 (참조하는 코드 없음) | — |
| 7 | **HEAP_CHNGUESS** | 클라이언트마다 "이 클래스 객체를 어느 버전(CHN)으로 캐시하고 있을 것"이라는 추측표. 서버가 객체를 보낼 때 이미 최신을 가진 클라이언트에는 다시 보내지 않으려고 쓴다 | 클래스 객체 전달(트랜잭션마다 클래스를 잠그고 가져올 때), 클래스 갱신(DDL), 트랜잭션 슬롯 해제 | heap_file.c, locator_sr.c, log_tran_table.c |
| 8 | **TRAN_TABLE** | 트랜잭션 테이블 (슬롯 할당·해제) | 접속·종료(슬롯 할당·반납, writer), 체크포인트(writer), 2PC(writer), tranlist 등 표 조회(reader). 새 트랜잭션 번호 발급은 원자적 CAS, 일반 커밋은 진입하지 않음 | `TR_TABLE_CS_ENTER` 매크로 — log_tran_table.c, server_support.c 등 |
| 9 | CT_OID_TABLE | 클래스 OID → 카탈로그(`_db_class`) 레코드 OID 캐시 | 카탈로그 행 갱신 중 조회(reader), 캐시 미스 시 삽입·DROP 시 삭제(writer) | catalog_class.c, locator_sr.c |
| 10 | HA_SERVER_STATE | HA 상태 전환 | 역할 변경 | server_support.c |
| 11 | COMPACTDB_ONE_INSTANCE | compactdb 단일 실행 보장 | compactdb | compactdb_sr.c |
| 12 | ACL | 접속 허용 IP 목록 (access_ip_control) | 목록 재적재·접속 시 검사 | server_support.c |
| 13 | PARTITION_CACHE | 파티션 정보 캐시 | 파티션 테이블 DDL/DML | partition.c |
| 14 | EVENT_LOG_FILE | 이벤트 로그 파일 쓰기 | 락 타임아웃·교착·대량 I/O 읽기·볼륨 확장·디스크·로그 플러시 대기 이벤트 기록 | lock_manager.c, network_interface_sr.cpp, disk_manager.c, trace_event_log.c |
| 15 | TRACE_LOG_FILE | 트레이스 로그 파일 쓰기 | 느린 질의 기록 (`sql_trace_slow_msecs`) | network_interface_sr.cpp, trace_event_log.c |
| 16 | **LOG_ARCHIVE** | 아카이브 로그 접근 | 백업, CDC/복제의 아카이브 읽기, 아카이브 생성 | `LOG_ARCHIVE_CS_ENTER` 매크로 — log_manager.c, log_page_buffer.c |
| 17 | ACCESS_STATUS | 사용자 접속 기록 | 로그인 | connection_sr.c |

굵은 것이 운영에서 실제로 경합이 보이는 다섯 개다. 이름이 비슷해도(…_TABLE, …_CACHE) 서로 **독립된** 락이다. 하나가
막힌다고 다른 것이 같이 막히지는 않는다(§4.3).

## 3. 동작: 들어가고, 기다리고, 나오기

### 3.1 writer 로 들어가기 (`csect_enter`)

```text
nenter++  (통계: 진입 시도)
내부 뮤텍스 잡기
반복:
  비어 있거나(rwlock == 0 이고 owner 없음) → 통과
  내가 이미 writer 면 (rwlock < 0 이고 owner == 나) → nreenter++, 통과   ← 재진입 허용
  아니면:
     waiting_writers++, nwait++   (통계: 대기 발생)
     writer 대기 줄 맨 뒤에 내 스레드 엔트리를 붙이고 잠든다 (FIFO)
     깨어나면 다시 반복
rwlock-- (→ -1), owner = 나, tran_index = 내 트랜잭션
획득에 걸린 시간을 total_elapsed 에 더하고 max_elapsed 갱신
내부 뮤텍스 놓기
```

핵심 세 가지:
- **같은 스레드는 다시 들어갈 수 있다**(재진입). `rwlock` 이 −2, −3 으로 깊어지고 나올 때마다 하나씩 올라온다.
- **writer 대기는 줄(FIFO)** 이다. 스레드 엔트리의 `next_wait_thrd` 포인터로 이어진다. cub_ctv 의 `wq` 체인이 바로 이
  리스트다.
- 기다릴 때 **타임아웃이 없다**(`INF_WAIT`). 엔진의 정적 18종 호출은 전부 무한 대기다. 유일한 예외는 `DISK_CHECK` 의
  복구·undo 함수(`disk_rv_reserve_sectors`/`disk_rv_unreserve_sectors`)로, "일단 시도(대기 0초)" 후 실패하면 페이지를
  unfix 하고 무한 대기로 다시 진입한다(포기하지 않음). 그래서 **크리티컬 섹션 타임아웃이라는 오류는 서버 로그에 존재하지 않는다**.
  로그에 보이는 "LATCH ON PAGE … TIMEDOUT" 은 페이지 래치(다른 층)다.

### 3.2 reader 로 들어가기 (`csect_enter_as_reader`)

```text
nenter++
내가 writer 로 쥐고 있으면 → rwlock-- (writer 가 reader 로도 재진입), nreenter++, 통과
아니면 반복:
  writer 가 있거나 (rwlock < 0)
  또는 writer 대기자가 있고 강등 상태가 아니면 (waiting_writers > 0 && owner 비어 있음)   ← writer 우선
     waiting_readers++, nwait++, readers_ok 조건변수에서 잠든다
rwlock++ (reader 하나 추가)
```

**writer 우선**이 중요하다. 줄 서 있는 writer 가 있으면 새 reader 는 이미 다른 reader 가 들어가 있어도 기다리며, writer 가
먼저 지나가도록 양보한다. 예외는 강등 상태(§3.4, owner 가 남아 있음)로, 이때는 writer 대기와 무관하게 바로 들어간다. 그래서 writer
가 자주 들어오는 CS(LOG)에서는 reader(예: 로그 읽기)도 같이 느려진다.

### 3.3 나오기 (`csect_exit`)

```text
writer 였으면 rwlock++ ; 0 이 되면 owner·tran_index 비움 (재진입 중이면 아직 안 비움)
reader 였으면 rwlock--
깨우기 (하나만 고른다):
  rwlock == 0 이고 writer 대기자가 있고 owner 가 남아 있으면(강등 중) → 승격자 1명을 깨운다   (promoter 줄)
  rwlock == 0 이고 writer 대기자가 있고 owner 가 비어 있으면         → writer 줄 맨 앞 1명을 깨운다 (writer 줄)
  그 밖의 경우                                                         → 기다리던 reader 를 전부 깨운다 (readers_ok broadcast)
```

reader 가 아직 남아 있으면(rwlock > 0) writer 가 기다려도 writer 를 깨우지 않고 reader broadcast 로 간다. 깨어난 reader 는
§3.2 의 조건을 다시 보고, writer 대기자가 있으면 다시 잠든다.

### 3.4 강등과 승격 (`demote` / `promote`)

writer 가 reader 로 내려가거나(demote), reader 가 writer 로 올라가는(promote) 경로가 있다. 쓰는 곳은 로그 플러시
(`logpb_flush_all_append_pages`) 한 군데이고, **HA 모드에서만** 쓴다. 플러시 스레드가 LOG 를 writer 로 잡고 디스크에 쓸
페이지 범위를 확정한 뒤, HA 로그 전송 스레드(copylogdb 에 로그를 보내는 LOGWR 스레드)를 깨우고 reader 로 내려가 디스크에
쓴다. 그 사이 전송 스레드들은 reader 로 로그를 읽는다. 플러시 스레드는 그들이 읽기를 마치기를 기다린 뒤 다시 writer 로
올라간다. 비HA 서버에서는 디스크 쓰기 내내 writer 로 쥔다.

강등 중에도 owner·tran_index 는 지워지지 않는다. 그래서 상태가 "reader N명"인데 owner 가 남아 있는 모습이 정상적으로 나온다.

어느 모드든 cub_ctv 화면에서 LOG 의 소유자가 `system`(트랜잭션 0, 엔진 데몬)으로 길게 보이는 것이 정상이다 — 그것이
로그 플러시다.

### 3.5 통계 (`SYNC_STATS`) — 숫자의 정확한 뜻

| 필드 | 올라가는 순간 | 주의 |
|---|---|---|
| nenter | 진입을 **시도**할 때마다 (writer·reader 모두, demote·promote 포함) | 성공 여부와 무관. 내부 뮤텍스 밖에서 올리므로 동시 진입 시 엔진 스스로 증가분 일부를 잃는다 |
| nwait | 들어가지 못해 **잠들 때**마다 | 무한 대기 경로에서만 센다. "대기 0초 시도" 진입은 안 센다 |
| nreenter | 이미 쥔 스레드가 다시 들어올 때 | LOG 가 큰 이유: LOG 를 writer 로 쥔 채 로그 페이지를 읽는 경로(reader 진입이 재진입으로 셈)와, 체크포인트·아카이브·백업이 플러시 함수를 중첩 호출하는 경로 |
| total_elapsed | 진입 시도부터 **획득까지** 걸린 시간의 합 | **보유 시간이 아니다.** 기다린 시간이다 |
| max_elapsed | 그중 최대 | 누적 최대라 지난 사건이 남아 있음 (`;info csstat` 실행 시 리셋) |
| (구조체) owner / tran_index | writer 가 쥘 때 기록, 강등 중에도 남음 | **reader 가 누구인지는 어디에도 없다** |

카운터는 부호 없는 32비트라 수십억 번이면 돌아간다(cub_ctv 는 구간 차이를 wrap 안전하게 계산한다).

csql 의 `;info csstat` 은 통계를 출력한 직후 **누적 통계를 0으로 리셋**한다(CS·RWLOCK·RMUTEX 전부). 그 뒤로는
`SHOW CRITICAL SECTIONS` 와 cub_ctv 의 누적값도 0부터 다시 쌓인다. 카운터(`unsigned int`)는 오래 가동한 서버에서 32bit 를 넘어 0부터 다시 세기도 하는데, 이때도 값이 줄어든다.
cub_ctv 는 두 번 읽은 사이에 진입 수와 누적 획득 시간(total_elapsed, 넘치지 않음)이 함께 줄었으면 리셋으로 보고
새 값을 그 구간의 증가분으로 쓰며, 진입 수만 줄었으면 32bit 넘침으로 계산한다. 또한 `;info csstat` 출력은 헤더가 `Total Wait|Total Reenter` 순인데
값은 nreenter, nwait 순으로 찍혀 두 열이 뒤바뀌어 있다(엔진 출력 버그).

## 4. 예시로 보는 흐름

### 4.1 커밋 한 건이 지나는 길 (autocommit INSERT)

```text
INSERT INTO t VALUES (...)
  ① 테이블 이름 't' → 클래스 OID        LOCATOR_CLASSNAME_TABLE   reader, µs
  ② 클래스 객체 잠금·가져오기           HEAP_CHNGUESS             writer, µs
     (클라이언트가 가진 버전 추측)
  ③ 페이지에 행 쓰기                    (페이지 래치 — CS 아님)
  ④ 로그 레코드 기록 (undo/redo)        (prior list 뮤텍스 — CS 아님)
COMMIT
  ⑤ 커밋 레코드 기록                    (prior list 뮤텍스 — CS 아님)
  ⑥ 로그 플러시를 기다림                 (그룹 커밋 조건변수 — CS 아님)
  ⑦ 로그 플러시 데몬이 디스크에 씀        LOG                       writer (HA 면 쓰기 구간만 reader 로 강등), ms
그 사이, 더티 페이지를 디스크에 쓰는 스레드(주로 페이지 플러시 데몬. victim 페이지 쓰기는 페이지 래치 없이 수행)
  ⑧ 그 페이지 LSA 까지 로그를 먼저 내림   LOG                       writer (WAL 보장)
```

11.x 에서 **로그 기록과 커밋 대기 자체는 LOG CS 를 지나지 않는다**(단 postpone 이 있는 커밋과 롤백은 자기 로그를 읽느라
LOG 를 지난다, §2.2). 위 흐름에서 LOG CS 를 쥐는 것은 ⑦ 플러시 데몬과 ⑧ WAL 보장 플러시다. 그래서 cub_ctv 가 보여 준
`[판정]` 의 LOG 수치(예: "엔진 스레드 log-flush 가 구간의 64% 보유, 대기 197건/s (진입 934건/s 의 21%), 진입당 획득 0.835 ms", docs/usage.md §3)는 **플러시
관련 진입**의 집계이고, 커밋들은 그 데몬을 (보이지 않게) 기다리느라 함께 느려진다.

클라이언트 세션이 LOG writer 로 보이면 대개 다음 둘 중 하나다.
- 롤백·세이브포인트·postpone 처리 등에서 자기 로그를 읽으려고 prior list 를 로그 버퍼로 내리는 경우
- 페이지를 고정한 채 명시적 페이지 flush(`pgbuf_flush_with_wal`)를 하면서 WAL 을 보장하는 경우

빈 버퍼가 필요한 워커는 victim 페이지를 직접 쓰지 않는다. 페이지 플러시 데몬이 있는 서버 모드에서는 victim 을 넘겨받을
대기열에 들어가 LOG 가 아니라 '빈 버퍼(21)'(§4.6)에서 기다린다. 어느 경우든 LOG 를 오래 쥐는 쪽은 플러시 데몬이므로
**세션을 죽여도 풀리지 않고**, 디스크 쓰기 지연과 `log_buffer_size`·그룹 커밋을 봐야 한다.

### 4.2 접속 한 건

```text
connect → 트랜잭션 슬롯 할당     TRAN_TABLE      writer
        → 로그인 기록            ACCESS_STATUS   writer
        → 세션 종료 시 슬롯 반납  TRAN_TABLE      writer
```

접속을 초당 수백 번 맺고 끊는 애플리케이션(커넥션 풀 없음)은 여기서 직렬화된다. 질의가 느린 게 아니라 "접속이 느린"
증상으로 나타난다.

### 4.3 DDL 반복과 연쇄 지연 (hold-and-wait)

```text
CREATE TABLE → 이름 등록(예약)  LOCATOR_CLASSNAME_TABLE  writer   (xlocator_reserve_class_name)
             → 클래스 갱신      HEAP_CHNGUESS            writer
             → 로그 기록        (prior list 뮤텍스 — CS 아님)
             → 카탈로그 갱신    CT_OID_TABLE             reader/writer (클래스 flush·커밋 때)
```

CS 들은 독립적이지만, 한 스레드가 **A 를 쥔 채 B 를 기다리면** A 를 기다리는 다른 스레드들이 함께 묶인다. 예: DDL 스레드가
CT_OID_TABLE 을 쥔 채 카탈로그 페이지를 고정하려다 디스크 읽기나 빈 버퍼(21)를 기다리는 경우다. 그동안 카탈로그를 고치려는
세션들이 CT_OID_TABLE 에서 줄을 서는데, 원인은 디스크 I/O 나 버퍼 부족이다.
이런 경우 cub_ctv 순위표에는 CT_OID_TABLE 의 손실(획득 시간)이 크게 나타나고, 쥔 스레드의 대기 사유(§4.6)가 원인을
가리킨다. 쥔 스레드가 다른 CS 에서 기다리는 경우에는 **두 CS 가 동시에 손실이 큰** 모양으로 나타나고, 원인은 보통 가장
오래 쥐는 아래쪽 CS 다.

### 4.4 보이지 않는 것

읽기 전용 SELECT 는 CS 를 거의 지나지 않는다(이름 조회 reader 정도. MVCC 이전 버전을 로그에서 읽을 때만 LOG 를 지난다). 행 락 경합, 페이지 래치 경합, 버퍼 부족으로 느린
경우는 CS 통계가 조용하다. 그래서 cub_ctv 가 "경합 없음"이라고 하면 "CS 층은 원인이 아니다"로 읽으면 된다.

### 4.5 페이지 래치 타임아웃과 크리티컬 섹션 — 자주 묻는 두 가지

서버 로그에 `LATCH ON PAGE(vol|page) TIMEDOUT`(오류 -836)이 찍히면 두 가지를 묻게 된다.

**(1) 그 트랜잭션은 실패하는가?** — 트랜잭션의 `lock_timeout` 에 따라 다르다. 기본값(-1, 무한)이면 트랜잭션 전체가 롤백된다.

| 트랜잭션의 `lock_timeout` | 엔진 동작 (`pgbuf_timed_sleep`) | 결과 |
|---|---|---|
| -1 (무한, 기본값) | `ER_PAGE_LATCH_TIMEDOUT` 에 이어 `ER_LK_UNILATERALLY_ABORTED` 를 설정한다. 래치 타임아웃을 버퍼 교착의 희생자로 본다 (디버그 빌드는 이 지점에서 `assert(0)`) | 이 오류가 클라이언트로 반환될 때 서버가 트랜잭션을 일방 롤백한다(`need_to_abort_tran` → `tran_server_unilaterally_abort_tran`) |
| 유한값 (> 0) | `ER_PAGE_LATCH_TIMEDOUT` 에 이어 `ER_LK_PAGE_TIMEOUT` 을 설정한다 | 문장만 실패한다. 트랜잭션은 열린 채 남아 애플리케이션이 재시도하거나 롤백한다 |

- 일부 경로(`pgbuf_fix_with_retry`)는 정해진 횟수까지 다시 시도하고, 그래도 안 되면 `ER_PAGE_LATCH_ABORTED`로 연산을 포기한다.
- 실무상 기본값 300초를 기다린 뒤에야 나는 오류이므로, 그 전에 대개 클라이언트 쪽 타임아웃이나 사용자가 먼저 끊는다.

**(2) 래치 타임아웃과 크리티컬 섹션은 관계가 있는가?** — 직접 관계는 없고, 간접 관계는 있다.

| | 내용 |
|---|---|
| 직접 | 페이지 래치(BCB)와 크리티컬 섹션은 서로 다른 락 체계다(§1 표). 크리티컬 섹션은 타임아웃이 없고, 페이지 래치 타임아웃을 세는 쪽은 페이지 버퍼다. 한쪽의 대기가 다른 쪽의 카운터에 잡히지 않는다 |
| 간접 | 페이지 래치가 300초 넘게 안 풀리려면 **래치를 쥔 스레드가 어딘가에서 멈춰 있어야** 한다. 그 "어딘가"가 크리티컬 섹션(예: 페이지를 고정한 채 명시적 flush 를 하는 스레드가 WAL 보장 플러시로 LOG 에서 멈춤)이거나 디스크 I/O일 수 있다. 반대 방향(CS 대기가 길어져 페이지 래치 타임아웃을 일으키는 것)도 같은 경로다 |

그래서 래치 타임아웃이 찍힌 **시각**의 크리티컬 섹션 상태를 보면 원인이 CS 쪽인지 아닌지를 가릴 수 있다. cub_ctv 로
그 시간대를 기록해 두었다면 `--replay`로 그 구간을 열어 "LOG 를 누가 쥐고 있었나, 대기 세션에 그 스레드가 있나"를
확인한다. CS 가 조용했다면 원인은 디스크 I/O 나 페이지 래치 자체(다른 층)이고, 그것만으로도 조사 범위가 반으로 준다.

### 4.6 CS 밖의 대기를 보는 법 — 스레드 대기 사유

워커 스레드는 기다릴 때 자기 엔트리의 `resume_status` 에 사유를 적는다(`thread_entry.hpp` 의 `thread_resume_suspend_status`: 페이지 래치 3, 작업 큐 5, CS reader/writer/promoter 7/9/11, 접속 큐 13, 락 17, 로그 writer 19, 빈 버퍼 21, DWB 23, sleep 25, 동시성 슬롯 26. 대부분 SUSPENDED 값 +1 이 깨어난 뒤의 RESUMED 값이다. 예외로 동시성 슬롯은 26/27 이고, sleep 25 는 짝이 없다. CS reader 대기 7 은 `pthread_cond_wait` 로 직접 깨어나 RESUMED 로 바뀌지 않고 7 로 남으므로, cub_ctv 는 CS reader 대기를 이 값이 아니라 대기열 집계로 센다). cub_ctv 는 이 필드를 프로빙해(I-18) 구간마다 실스레드 전체의 분포를 `[스레드 대기]` 로 보여 준다. CS 표가 조용한데 "락 37, 페이지 래치 5" 라면 원인은 CS 밖(락 매니저·페이지 버퍼)이고, 그쪽 도구(`cubrid lockdb`, 버퍼 지표)로 넘어가면 된다. 11.4~현재 소스에서 값이 같다.

## 5. 언제 문제이고 언제 아닌가

| 신호 | 뜻 | 판단 |
|---|---|---|
| 진입 많음, 대기 거의 없음 (LOCATOR 초당 수만 번, 진입당 획득 0.001ms 수준) | 고회전 | 정상. 설계대로 짧게 쥐고 있음 |
| 대기율·진입당 획득 시간 큼, 쥔 쪽 = 엔진 데몬 (LOG) | 플러시·체크포인트가 상한 | 디스크·로그 설정 쪽 |
| 대기 큼, 쥔 쪽 = 특정 클라이언트 세션 | 그 세션이 오래 쥠 (DDL, 대량 작업) | 세션 쪽 |
| 대기자 있음 + 진입 0 + writer 고정 | 쥔 스레드가 다른 곳에서 멈춤 (디스크, 페이지 래치, 교착) | 행(hang) 의심 — 쥔 스레드가 무엇을 기다리는지 추적 |
| 서버 로그 "LATCH ON PAGE … TIMEDOUT" | 페이지 래치 (CS 아님) | 그 시각의 CS 상태를 재생해 "CS 가 관여했는가"만 확인 |

## 6. 보는 방법 비교

| | `SHOW CRITICAL SECTIONS` | `;info csstat` (csql) | cub_ctv |
|---|---|---|---|
| 접속 | DBA 로 SQL 접속 필요 (접속 자체가 TRAN_TABLE·ACCESS_STATUS 를 지남) | csql 접속 필요 | 없음 (/proc 읽기) |
| 범위 | 정적 18종 | 18종 + 풀의 rwlock/rmutex (mutex 없음) | 18종 + 풀 |
| 시간축 | 1회 스냅샷 | 1회 텍스트 | 100Hz 샘플링, 구간 집계 |
| 소유자 | 스레드·트랜잭션 **번호** | 없음 | 번호 + 클라이언트 신원 + 대기 체인 |
| 자기 영향 | 조회가 카운터를 올림 | 출력 직후 **누적 통계를 0으로 리셋**(CS·RWLOCK·RMUTEX 전부). SHOW·cub_ctv 의 누적값도 함께 0부터 다시 쌓임 | 0 |
| 해석 | 없음 (숫자) | 없음 | 판정문 (어디서·누가·얼마나·조치) |
| 행 걸린 서버 | 불가 | 불가 | 가능 |

## 7. 정리

| 질문 | 답 |
|---|---|
| 크리티컬 섹션이 뭔가 | 엔진 전역 구조체 18종을 지키는 reader-writer 락. 보유는 µs, 하지만 모든 커밋·접속이 거치므로 병목이 될 수 있다 |
| 트랜잭션 락·페이지 래치와 다른가 | 다른 층이다. 지키는 것(객체/페이지/전역 구조체)과 보유 시간이 다르고, CS 만 타임아웃이 없다 |
| 재진입되나 | writer 는 같은 스레드가 재진입 가능(`rwlock` 깊이), `nreenter` 로 센다 |
| 누가 먼저 들어가나 | rwlock 이 0 이 되면 강등 중(owner 남음)이면 승격자 1명, 아니면 writer 줄 맨 앞 1명. 그 밖에는 reader 전체 broadcast. reader 는 writer 가 기다리면 양보한다(강등 중 제외) |
| 통계의 시간은 무엇인가 | 획득까지 기다린 시간(보유 시간 아님). nwait 는 잠든 횟수 |
| 서로 연관되나 | 독립. 단 한 스레드가 하나를 쥔 채 다른 것을 기다리면 지연이 번진다 |
| 왜 LOG 소유자가 늘 system 인가 | 로그 플러시 데몬(트랜잭션 0)이 디스크 쓰기 동안 쥐기 때문(HA 모드면 쓰기 구간만 reader 로 강등, owner 는 남음). 정상이며, 길면 디스크·설정을 본다 |
| LOG 대기 수가 커밋 수인가 | 아니다. 로그 기록은 prior list 뮤텍스, 커밋 대기는 그룹 커밋 조건변수로 CS 밖이다. LOG CS 의 진입·대기는 플러시·WAL 보장·체크포인트·아카이브, 그리고 롤백·postpone·세이브포인트·CDC·HA 전송의 로그 페이지 읽기(reader, 필요하면 prior list 내리기 writer)다 |
| 타임아웃 오류가 왜 없나 | 정적 CS 진입은 전부 무한 대기. 로그의 래치 타임아웃은 페이지 래치다 |
| 래치 타임아웃이 나면 트랜잭션은 | 기본 `lock_timeout`(-1, 무한)이면 버퍼 교착 희생자로 보고 `ER_LK_UNILATERALLY_ABORTED` → 서버가 트랜잭션을 일방 롤백. `lock_timeout` 이 유한값이면 `ER_LK_PAGE_TIMEOUT` 으로 문장만 실패 |
| 래치 타임아웃과 CS 의 관계 | 직접 없음(다른 층). 간접 있음 — 래치를 쥔 스레드가 CS 에서 멈췄을 수 있으니 그 시각의 CS 상태를 본다 |

### 용어

| 용어 | 뜻 |
|---|---|
| writer / reader | 구조체를 고치는 쪽 / 읽기만 하는 쪽. writer 는 혼자, reader 는 여럿 |
| rwlock | 상태 정수. 양수 = reader 수, 음수 = writer 보유(재진입 깊이), 0 = 비어 있음 |
| 대기 큐 (wq) | writer 가 줄을 서는 스레드 엔트리 연결 리스트. FIFO |
| 재진입 | 이미 쥔 스레드가 같은 CS 에 다시 들어가는 것 |
| 강등 / 승격 | writer→reader / reader→writer 전환. HA 모드의 로그 플러시가 사용 |
| system (tran 0) | 클라이언트가 아닌 엔진 데몬 스레드의 트랜잭션 번호 |
| 획득 지연 | 진입을 시도한 순간부터 들어가기까지 기다린 시간. 통계의 `elapsed` |
| 점유율 (occ%) | cub_ctv 가 100Hz 로 본 샘플 중 CS 가 잡혀 있던 비율 |
