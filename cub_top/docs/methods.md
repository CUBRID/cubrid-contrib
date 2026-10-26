# 동적 힙 분해 — 방법 B (Dynamic Heap Breakdown)

`dynamic-heap`은 cub_server의 익명 힙에서 선할당 버퍼(`data_buffer`, `log_buffer`)와 스레드 스택을 제외한 나머지입니다. 정렬 버퍼·각종 캐시·락 테이블·세션·MVCC 스냅샷·질의 작업 메모리가 여기 섞여 있고, `/proc`만으로는 한 덩어리로만 보입니다.

CUBRID에는 내장 메모리 모니터(`cubrid memmon`)가 있지만 `enable_memory_monitoring=yes` + 재기동이 필요하고 할당마다 태깅 오버헤드가 붙습니다. cub_top은 **서버 변경 없이** 두 방법으로 이 덩어리를 분해합니다.

---

## 방법 B — 루트 전역 순회 (정확)

엔진의 전역 변수를 직접 읽어 서브시스템별 사용량을 얻습니다.

### 원리

```
1. /proc/<pid>/maps 에서 libcubrid.so.11.x 로드 베이스 확보
2. nm 으로 심볼의 링크 주소(st_value) 확보
3. 런타임 주소 = 로드 베이스 + st_value          (PIE 보정)
4. process_vm_readv 로 해당 주소에서 필드 상태 읽기    (정지 없음)
```

### 확보된 루트와 오프셋

CUBRID 11.5 빌드의 DWARF(`readelf --debug-dump=info`)에서 추출한 값입니다.

| 서브시스템 | 심볼 | 필드 | 오프셋 | 산출 |
|---|---|---|---|---|
| 페이지 버퍼 | `_ZL10pgbuf_Pool` | `num_buffers` | 0 | `num_buffers × DB_PAGESIZE` |
| **플랜 캐시(XASL)** | `xcache_Global` | `entry_count` | 440 | 엔트리 수 |
| | | `memory_usage_cache` | 484 | **엔진이 직접 세는 바이트** (● 측정) |
| | | `memory_usage_clone` | 488 | 엔진의 **추정치** — 클론당 `UNPACK_SCALE(3) × stream 크기` (◌ 추정, `plan clones (est)`) |
| 결과(list) 캐시 | `_ZL27qfile_List_cache_entry_pool` | `n_entries` | 8 | `× 4096`(고정 슬롯, `result cache (query)`) |
| | (파라미터) | `max_plan_cache_entries` | — | 해시 `(N+10)`개 × 버킷 약 `(N+10)` — ◌ 추정·하한, `result cache hash (est)` |
| | `_ZL16qfile_List_cache` | `n_pages` | 28 | **힙 아님**: 결과 리스트 파일의 temp 볼륨 페이지 수 — 개수만 표시, B 합계 제외(`result temp pages`) |
| 필터 술어 캐시 | `_ZL21fpcache_Entry_counter` | (INT32 자체) | 0 | `× 120` |
| 락 매니저(트랜잭션 슬롯) | `lk_Gl` | `config.num_trans` | 0 | `× 152` |
| 락 자원(객체) | `lk_Gl` | `m_obj_hash_table…m_freelist.alloc_cnt` | 84 | `× sizeof(lk_res)` 120 |
| 락 엔트리(객체) | `lk_Gl` | `obj_free_entry_list.alloc_cnt` | 480+12 | `× sizeof(lk_entry)` 128 |
| 세션 테이블 | `_ZL8sessions` | `…m_old_hash.m_freelist.alloc_cnt` | 12 (`OFF_LF_ALLOCCNT`) | `× 312` |
| 카탈로그 캐시 | `_ZL15catalog_Hashmap` | `m_old_hash.m_freelist.alloc_cnt` | 12 (`OFF_LF_ALLOCCNT2`, 세션과 별도 키) | `× 48` |
| 연결 엔트리 | `_ZL16css_Num_max_conn` | (INT32 자체) | 0 | `× 480` |
| | `_ZL19css_Num_active_conn` | (INT32 자체) | 0 | 활성 수 |

### DWARF에서 얻은 sizeof

| 구조체 | 바이트 | 구조체 | 바이트 |
|---|---|---|---|
| `pgbuf_buffer_pool` | 648 | `session_state` | 312 |
| `xasl_cache_ent` | 264 | `catalog_entry` | 48 |
| `qfile_list_cache_entry` | 296 | `lk_entry` | 128 |
| `qfile_list_cache` | 48 | `lk_res` | 120 |
| `fpcache_ent` | 120 | `lk_tran_lock` | 152 |
| `css_conn_entry` | 480 | `mvcc_snapshot` | 80 |
| `vacuum_data` | 184 | `sort_param` | 600 |

### 정확도가 가장 높은 지점

`xcache_Global.memory_usage_cache` 는 **엔진이 스스로 카운팅하는 바이트 값**입니다. 그 값을 그대로 읽으므로 측정(●)입니다.
`memory_usage_clone` 은 엔진이 실제 할당을 세는 값이 아니라 클론당 `sizeof(xasl_unpack_info)+sizeof(XASL_NODE)+UNPACK_SCALE×stream` 으로
**가정한 크기**의 합이라 별도 항목 `plan clones (est)` 로 추정(◌) 등급을 붙입니다.

결과 캐시의 `n_pages` 는 캐시된 결과 리스트 파일(`list_id.page_cnt`)의 합입니다. 이 파일은 preserved temp file 이라
temp 볼륨과 데이터 버퍼에 있고(힙 합계에서 제외하는 영역) `file_temp_retire_preserved` 로 지워지므로, 힙 합계에
넣지 않고 페이지 수만 따로 보입니다. 결과 캐시의 실제 힙은 엔트리 풀(`× 4096`)과, 기동 시
`qfile_initialize_list_cache` 가 만드는 `max_plan_cache_entries + 10` 개의 해시 테이블(각 같은 수의 버킷)입니다.
해시 크기는 테이블 헤더·fixed heap·버킷 포인터 배열로 추정하며, 엔트리가 들어오면서 늘어나는 HENTRY 청크는 넣지 않아
하한입니다. `max_query_cache_entries` 또는 `query_cache_size_in_pages` 가 0 이면 결과 캐시가 꺼져 해시도 만들어지지 않습니다.

`vacuum_Data` 는 184B 구조체일 뿐 vacuum 이 쓰는 메모리를 나타내지 않으므로 항목으로 내지 않습니다.

이 두 필드는 11.5 표에만 있습니다. 10.2~11.4 표에는 `entry_count`만 있으므로 플랜 캐시는 **엔트리 수만** 표시하고(크기 `-`, `heapB.plan_cache_xcache_count`) 합계에서 뺍니다. 표에 없는 오프셋은 읽지 않고 그 항목을 생략합니다.

### 락프리 해시맵 주의

`sessions`, `catalog_Hashmap`, `fpcache_Hashmap`, `lk_Gl.m_obj_hash_table`은 `cubthread::lockfree_hashmap<K,V>`이고, 숨은 파라미터
`new_lfhash`(기본 `no`)에 따라 **구현 두 개 중 하나**를 씁니다. 어느 쪽인지는 래퍼의 `m_type`(OLD=0, NEW=1)에 있습니다.

| `m_type` | 읽는 값 | 표시 |
|---|---|---|
| OLD(0) | 임베드된 `lf_freelist.alloc_cnt` | 개수 × 구조체 크기 (● 측정) |
| NEW(1) | — (구 freelist 의 `alloc_cnt` 는 0 으로 남음) | `?` "새 lockfree 해시 — 개수 미상, 합산 제외". 0 B 측정으로 보이지 않게 한다 |
| 래퍼 없음(10.x) | 그 자체가 구 lf 해시 | OLD 와 같음 |

`m_type` 오프셋은 표의 `OFF_SESS_HTYPE`·`OFF_CAT_HTYPE`·`OFF_LK_HTYPE` 이다. 락 엔트리 풀(`obj_free_entry_list`)은 해시맵이 아닌
`LF_FREELIST` 라 종류와 무관하게 읽는다.

### 전역 루트에서 도달 불가한 것

정렬 버퍼(`sort_param.internal_memory`), 해시조인/그룹바이 해시테이블, heap scan cache는 **질의 실행 컨텍스트와 스레드 스택에 매달려** 있어 전역 변수에서 포인터로 도달할 수 없습니다. 방법 B로는 원리적으로 잡히지 않습니다.

---

## 버퍼풀 버퍼 제어블록(BCB) 상태 읽기 (pgbuf) —

방법 B 의 원리(전역 → 포인터 → 구조체 상태 읽기)를 **배열**에 적용해, 버퍼풀의 페이지 단위 상태를 읽는다.
목적은 `cub_volmap` 과의 연속성이다: 볼륨 지도(디스크) 위에 "지금 버퍼에 있는 페이지 / 아직 안 내려간 페이지"를 얹는다.

### 읽는 것

| 단계 | 대상 | 방법 |
|---|---|---|
| 1 | `pgbuf_Pool.num_buffers`, `pgbuf_Pool.BCB_table`(포인터) | 방법 B 와 동일(심볼 + 오프셋 표) |
| 2 | BCB 배열 통째 (`num_buffers × sizeof(pgbuf_bcb)`) | `process_vm_readv` 1회(512MB 버퍼 = 32,768 × 144B ≈ 4.7MB) |
| 3 | BCB 마다 `vpid`, `flags`(bit31 dirty·bit30 flushing·bit16-19 존), `oldest_unflush_lsa`, `iopage_buffer` 포인터 | 배열 안에서 디코드 |
| 4 | 각 iopage 의 페이지 헤더 16B (`prv.lsa`, `pageid`, `volid`) | `process_vm_readv` gather (iov 1024개 × 32회) |
| 5 | `log_Gl.append.nxio_lsa`(디스크 반영 경계), `log_Gl.hdr.append_lsa`(꼬리), `hdr.eof_lsa` | 8B × 3 |

### 오프셋 (dwoff 추출 — 버전마다 다르다)

| 키 | 11.5.0 | 11.4.5 | 10.2.17 | 의미 |
|---|---|---|---|---|
| `OFF_PGBUF_BCBTAB` | 8 | 8 | 8 | `pgbuf_Pool.BCB_table` |
| `SZ_PGBUF_BCB` | **144** | **128** | 128 | `sizeof(pgbuf_bcb)` — 배열 stride |
| `OFF_BCB_VPID` | 44 | 44 | 44 | `vpid` (pageid 4B + volid 2B) |
| `OFF_BCB_FLAGS` | **64** | **60** | 60 | `flags` |
| `OFF_BCB_OLDEST_LSA` | 128 | 112 | 112 | `oldest_unflush_lsa` |
| `OFF_BCB_IOPAGE` | 136 | 120 | 120 | `iopage_buffer` 포인터 |
| `OFF_IOBUF_IOPAGE` | 8 | 8 | 8 | `pgbuf_iopage_buffer.iopage` |
| `OFF_LOG_NXIO_LSA` | 48 | 48 | 48 | `log_Gl.append.nxio_lsa` |
| `OFF_LOG_APPEND_LSA` | 280 | 280 | 264 | `log_Gl.hdr.append_lsa` |
| `OFF_LOG_EOF_LSA` | 488 | 488 | 472 | `log_Gl.hdr.eof_lsa` |

`pgbuf_bcb` 는 변수 루트가 없는 타입(포인터 너머)이라 dwoff 에 **type+member** 형(`{"OFF_BCB_FLAGS", NULL, "flags", "pgbuf_bcb"}`)을 추가했다.
`log_Gl` 은 헤더 선언이 CU 마다 반복되는 extern 전역이라 후보 DIE 가 수십 개다 — "정의가 정확히 1개" 또는 "전 후보가 같은 (타입명, sizeof) 로 해소"일 때만 채택하도록 다후보 규칙을 완화했다(동명 정적 변수는 여전히 거부).

LSA 는 `struct log_lsa { int64 pageid:48; int64 offset:16; }` 8바이트다(리틀엔디안 비트필드: 하위 48비트 pageid, 상위 16비트 offset). NULL = 전 비트 1.

### 자기검증 (틀린 오프셋으로 그림을 그리지 않기 위한 장치)

| 검사 | 임계 | 위반 시 |
|---|---|---|
| L4 pgbuf 게이트 (기존) | `num_buffers == paramdump data_buffer_pages` | 방법 B 전체 비활성 → pgbuf 도 비활성 |
| VPID 위생 | volid ∉ [-1, 255] 또는 pageid < -1 인 BCB 가 1% 초과 | pgbuf 비활성 + 사유 |
| **페이지 헤더 대조** | 실제로 읽은 헤더 수 대비 비율 — 표본 크기와 무관. 읽기 실패 50% 초과, 또는 재검 후에도 (pageid, volid) ≠ BCB.vpid 가 50% 초과, 또는 검증된 헤더 0개 | pgbuf 비활성 + 사유. 5~50% 는 전수(`--bcb-dump`)일 때 검증된 레코드만 집계(degraded). 소량 불일치는 교체 중인 페이지(정상) |
| **검증 표본 없음** | 읽을 수 있는 헤더가 0개 — 버퍼풀이 비었거나 표본 페이지가 모두 비상주 | pgbuf 비활성 + 오프셋 오류와 구분되는 사유("검증 안 됨"). hot/warm 표본이 32개 미만이면 상주 cold 페이지를 고르게 더해 32개까지 채운다 |
| 로그 위생 | append ≥ nxio, pageid ≥ 0 | 로그 항목만 생략 |

실측(11.5, 32,768 BCB): 위생 위반 0, 헤더 불일치 0, 읽기 실패 0, 소요 8~17ms.

### 비용
- 512MB 버퍼: 프레임당 약 4.7MB 읽기 + 헤더 표본 256개 gather. 헤더 전수(`--bcb-dump`)는 32 syscall 이 더 든다.
- 262,144 BCB(4GB) 초과 시 배열 전수 통과는 자기 비용의 100배 주기(최소 5초)로 하고 사이에는 슬롯 1/64 표본으로 적재량만 잰다. 요청(`d`·`--pgbuf-detail`·`--bcb-dump`)하면 매 프레임 한다.
  사이 프레임에 직전 스냅샷을 다시 보이는 방식(희석)은 쓰지 않는다 — 화면의 값은 모두 그 프레임에 읽은 값이어야 해서다.
  평소에는 엔진 집계(`pgbuf.engine.*`)를 보인다.
- 래치를 잡지 않는 스냅샷이다. 틱 사이의 변동은 놓치며, 등급은 ◌(추정)로 표기한다.

### `--bcb-dump FILE` 스냅샷 파일 (cub_volmap 과의 계약)

`FILE.tmp` 에 쓰고 `rename` 으로 원자 교체한다. 라이브는 매 프레임, 1회 실행은 1회.
파일 권한은 umask 를 적용한 0644 다(보통 `-rw-r--r--`). `cub_volmap --bufmap` 을 다른 계정으로 돌려도 읽을 수 있게 하기 위해서이며,
내용은 페이지 번호·상태 플래그·LSA·DB 이름뿐이고 행 데이터는 없다. umask 가 더 엄격하면(예: 077) 그대로 따르고, umask 가 0 이어도
그룹·다른 계정에 쓰기 권한은 주지 않는다. `--record` 기록(0600)은 이 계정 전용이다.

```
헤더 112B (LE):  magic "CBCBMAP1" | u32 version=1 | u32 reclen=32 | i64 ts_sec | i64 ts_nsec
                 | i32 nbuf | i32 nrec | i32 dirty | i32 pagesize
                 | u64 log_append | u64 log_nxio | u64 log_eof | u64 oldest_dirty  (raw LSA, NULL=~0)
                 | char db[32]
레코드 32B × nrec: i16 volid | i16 zone(1/2/3 LRU, 4 invalid, 8 void) | i32 pageid | u32 flags
                 | i32 hdr_ok | u64 page_lsa(메모리 페이지 헤더) | u64 oldest_lsa(BCB)
```

`flags` 는 BCB 플래그 원값이다. **볼륨과 같은 내용이라고 볼 수 있는 것은 bit31(dirty)과 bit30(flushing)이 모두 꺼진 페이지뿐이다.**
flush 가 시작되면 엔진은 dirty 를 내리고 flushing 을 세우며 `oldest_lsa` 를 비운다(쓰기가 끝나면 flushing 을 내린다). 도중에 다시
dirty 가 되면 두 비트가 함께 선다. DWB 가 켜져 있으면(`pgbuf.dwb_enabled=1`) 페이지는 DWB 에 복사된 순간 flushing 이 내려가므로,
두 비트가 모두 꺼진 페이지 중 최대 `pgbuf.dwb_pages_max` 개는 아직 DWB 에서 볼륨 쓰기를 기다릴 수 있다 — 이 파일만으로는 어느
페이지인지 알 수 없고, 디스크 LSA 가 메모리 페이지 LSA 보다 오래되었다면 그런 페이지다.

`cub_volmap --bufmap FILE` 은 db 이름이 다르면 거부하고, 레코드를 볼륨별 페이지 배열로 펴서 배경색 층(청록=상주, 자주=dirty)과
페이지 박스 제목(`buf:DIRTY unflushed` = 메모리 LSA > 디스크 LSA)에 쓴다.

---

## 방법 A(청크 히스토그램)는 없앴다

glibc 아레나의 청크를 걸어 크기 분포를 추정하던 방법 A 는 제거했다. 수백 ms 의 대상 메모리 순회 부하, free·비상주 청크를 포함하는 구조적 과대계상, 그리고 값이 운영 조치로 이어지지 않는다는 점 때문이다. 동적 힙은 방법 B(엔진 전역 직독)로만 분해하고, B 가 상주의 절반 이상을 설명하면 `dynamic-heap` 등급을 E 로 올린다.

## 재생 시의 복원 (`rp_apply`)

재생은 측정이 아니라 **복원**이다. 한 프레임의 JSON 한 줄에서 `proc_t`·`mem_t`·`tier_t`·`iod_t`
와 영역 배열·버퍼풀·용량 축·힙 항목을 되살려, 라이브와 같은 렌더러에 넘긴다.

| 복원 대상 | 키 | 비고 |
|---|---|---|
| OS·프로세스·티어 | `os.*` `server.*` `tier.*` | 1:1 대응 |
| 영역 | `region.<슬러그>.*` | 슬러그→표기명 역매핑(`thread_stacks` → `thread stacks`) |
| 버퍼풀 | `pgbuf.*` | 존·볼륨별 상주는 있는 키만 채움 |
| 용량 축 | `cap.*` | 상한은 **칸별 매트릭스** 전체 |
| 동적 메모리 B | `heapB.<슬러그>_bytes` | 슬러그→항목명 역매핑 |

슬러그 역매핑이 필요한 이유는 `-d` 출력의 키가 화면 표기명을 `slugify()` 한 것이기 때문이다
(화면은 한/영이 바뀌지만 키는 고정 ASCII여야 모니터링이 깨지지 않는다 — `render_terse()` 주석 참조).
역매핑 표에 없는 항목은 **복원하지 않는다**. 이름을 추측해 만들어내지 않는다.

측정에 쓰이는 값 중 **재계산이 가능한 것은 저장값을 쓰지 않고 다시 계산한다.** 프로파일 칸이
그 예로, `req_kb`/`read_ratio` 에서 `cap_bs_idx()`/`cap_rw_idx()` 로 구한다.

