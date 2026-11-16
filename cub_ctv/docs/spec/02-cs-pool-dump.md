# 크리티컬 섹션·풀·덤프 명세 (Critical Sections, Pool and Dump Specification)

범위: `src/ctv_cs.c`, `src/ctv_pool.c`, `src/ctv_dump.c` 와 이들이 쓰는 `src/ctv.h` 정의. 이 문서만으로 세 모듈을 같은 동작으로 다시 구현할 수 있도록, 상수·오프셋·판정 규칙·출력 문자열을 코드 그대로 적는다. 엔진 배경은 `docs/critical-sections.md`, 모듈 배치는 `docs/architecture.md` 를 따르되, 둘이 코드와 다를 때는 코드가 기준이다.

표기:

- 오프셋은 모두 바이트, 리틀엔디언(x86-64). `rd_u64/rd_i32/rd_u32/rd_i64(buf, off)` 는 `memcpy` 로 정렬 무관하게 읽는 원시 판독이다.
- `S` = CS 원소 안에서 `SYNC_STATS *stats` 필드의 오프셋(`CS_LAYOUT.off_stats`).
- "대상 메모리" = 대상 cub_server 의 가상 주소 공간. 판독은 전부 `ctv_mem.c` 를 지난다(§1.3).

---

## 1. 공통 정의 (ctv.h)

### 1.1 상수

| 이름 | 값 | 용도 |
|---|---|---|
| `MAX_CS` | 64 | CS 배열 탐지 상한(원소 수). 프로브 버퍼 = `MAX_CS * 256` = 16384 B |
| `CS_MIN_SAMPLES` | 4 | stride/count 프로브가 합의해야 하는 최소 원소 수 |
| `NAME_MAX_LEN` | 64 | CS·동기화 객체 이름 버퍼(NUL 포함) |
| `STATS_SIZE` | 64 | `SYNC_STATS` 한 개의 크기 |
| `ST_OFF_TYPE` | 0 | `SYNC_STATS.type` (int32) |
| `ST_OFF_NAME` | 8 | `SYNC_STATS.name` (const char *, 8 B) |
| `ST_OFF_NENTER` | 16 | `nenter` (u32) |
| `ST_OFF_NWAIT` | 20 | `nwait` (u32) |
| `ST_OFF_NREENTER` | 24 | `nreenter` (u32) |
| `ST_OFF_TOTAL_TV` | 32 | `total_elapsed` (struct timeval, 16 B) |
| `ST_OFF_MAX_TV` | 48 | `max_elapsed` (struct timeval, 16 B) |
| `ST_TYPE_CSECT` | 1 | 크리티컬 섹션 |
| `ST_TYPE_MUTEX` | 4 | 유효 type 의 상한 |
| `ST_TYPE_OK(t)` | `1 <= t <= 4` | SYNC_STATS 유효 type (I-5) |
| `CS_OFF_NAME` | 0 | CS 원소의 `const char *name` |
| `CS_OFF_INDEX` | 8 | CS 원소의 `int cs_index` (I-1 닻) |
| `CS_PAIR_RETRIES` | 3 | I-4 **총 시도 수**(`tries = 0..2`). 첫 시도는 I-3 에 쓴 이미지 그대로, 재판독은 2·3번째 시도 앞의 최대 2회(§3.4 6단계). 01 §1.4 와 같은 뜻 |
| `CS_STATS_TO_RWLOCK` | 96 | `off_rwlock = S − 96` |
| `CS_STATS_TO_PQ` | 24 | `off_pq = S − 24` (waiting_promoters_queue) |
| `CS_STATS_TO_WQ` | 32 | `off_wq = S − 32` (waiting_writers_queue) |
| `CS_STATS_TO_OWNER` | 16 | `off_owner = S − 16` (thread_id_t owner) |
| (리터럴) | 8 | `off_tran = S − 8` (int tran_index) |
| `CS_WAIT_SANE_MAX` | 1000000 | waiting_readers / waiting_writers 상한 |
| `CHUNK_TAIL` | 16 | 풀 청크 꼬리 크기 |
| `CHUNK_BYTES(n)` | `n*64 + 16` | 엔트리 n 개 청크의 바이트 수 |
| `CHUNK_OFF_NEXT(n)` | `n*64` | 청크 꼬리의 next 포인터(u64) |
| `CHUNK_OFF_INUSE(n)` | `n*64 + 12` | 청크 꼬리의 in_use(int32) |
| `POOL_MAX_CHUNKS` | 1024 | 체인 홉 상한 |
| `POOL_MAX_ENTRIES` | 65536 | 순회 엔트리 상한 |
| `POOL_MAX_GROUPS` | 1024 | (type, name) 그룹 상한 |
| `POOL_GROUPS_CAPPED` | −2 | `pool_truncated` 센티널: 체인은 다 읽었으나 그룹이 가득 참 |
| `POOL_NO_MEMORY` | −3 | `pool_truncated` 센티널: 버퍼 할당 실패, 아무것도 읽지 않음 |
| `POOL_CHAIN_LOOP` | −4 | `pool_truncated` 센티널: 이미 읽은 청크로 되돌아옴 |
| `GROUP_HASH` (ctv_pool.c 내부) | 4096 | 그룹 해시 슬롯 수(2의 거듭제곱, `> 2*POOL_MAX_GROUPS`) |
| `FNV_SEED` | 14695981039346656037 | FNV-1a 64 초기값. 곱수 1099511628211 (`fnv()`, ctv_text.c) |
| `DETAIL_MAGIC` | `"cub_ctv detail dump"` | 디테일 덤프 형식 표시 |
| `VALID_PTR(p)` | `p > 0x1000 && p < 0x800000000000` | 사용자 공간 포인터 판정 |
| `REFUSE(id)` | `return (id)` | 프로브 함수의 거부 반환 |
| `M(en, ko)` | `use_ko ? ko : en` | 한/영 메시지 선택 |

### 1.2 구조체

**STATS_VIEW** — `SYNC_STATS` 한 개의 해독 결과.

| 필드 | 타입 | 출처 |
|---|---|---|
| `type` | int32_t | `+0` |
| `name_ptr` | uint64_t | `+8` |
| `nenter`, `nwait`, `nreenter` | uint32_t | `+16`, `+20`, `+24` |
| `total_ms` | double | `+32` timeval → ms |
| `max_ms` | double | `+48` timeval → ms |

**CS_LAYOUT** — 프로브가 확정한 CS 배열 기하. 모든 오프셋은 원소 시작 기준.

| 필드 | 뜻 | 결정 |
|---|---|---|
| `stride` | `sizeof(SYNC_CRITICAL_SECTION)` | I-1 |
| `count` | 정적 CS 개수 | I-2 |
| `off_stats` | `SYNC_STATS *stats` | I-3 |
| `off_tran` | `int tran_index` | `off_stats − 8` |
| `off_rwlock` | `int rwlock` (+4 waiting_readers, +8 waiting_writers) | `off_stats − 96` |
| `off_owner` | `thread_id_t owner` | `off_stats − 16` |
| `off_wq` | `THREAD_ENTRY *waiting_writers_queue` | `off_stats − 32` |
| `off_pq` | `THREAD_ENTRY *waiting_promoters_queue` | `off_stats − 24` |

**CS_ROW** — CS 하나의 한 시점 값.

| 필드 | 타입 | 출처 |
|---|---|---|
| `name[64]` | char | `name_of(*(u64)(elem+0))`, 읽지 못하면 `"?"` |
| `rwlock` | int32_t | `elem+off_rwlock` |
| `wr` | uint32_t | `elem+off_rwlock+4` (waiting_readers) |
| `ww` | uint32_t | `elem+off_rwlock+8` (waiting_writers) |
| `tran` | int32_t | `elem+off_tran` (원시값; writer 가 아닐 때도 남은 값 그대로) |
| `owner` | uint64_t | `elem+off_owner` (원시값; 0 = 없음) |
| `wq` | uint64_t | 첫 대기 큐 머리: promoter 큐가 비어 있지 않으면 그것, 아니면 writer 큐 (§3.5) |
| `wq2` | uint64_t | promoter 큐가 앞에 있을 때 그 뒤의 writer 큐 머리, 아니면 0 |
| `nenter`, `nwait`, `nreenter` | uint32_t | stats 블록 |
| `total_ms`, `max_ms` | double | stats 블록 |
| `has_stats` | int | stats 블록을 읽고 type 이 1..4 이면 1 |

**POOL_GROUP** — 풀의 (type, name) 묶음.

| 필드 | 타입 | 뜻 |
|---|---|---|
| `type` | int | 1..4 |
| `name[64]` | char | 엔트리 이름(`name_of`, 실패 시 `"?"`) |
| `cnt` | int | 묶인 엔트리 수 |
| `nenter` | uint64_t | nenter 합 |
| `max_ms` | double | max_ms 최대값 |

`nwait`·`nreenter`·`total_ms` 는 그룹에 합산하지 않는다(엔진이 nwait 를 CS 에 대해서만 세므로).

### 1.3 이 모듈들이 쓰는 판독 기반 (ctv_mem.c 계약)

| 함수 | 계약 |
|---|---|
| `mem_read(addr, buf, n)` | `/proc/<pid>/mem` 에 `pread` 1회. 정확히 n 바이트면 0, 아니면 −1 |
| `mem_read_shrink(addr, buf, max, min)` | `n = max` 부터 `n >= min && n > 0` 인 동안 `mem_read(addr, buf, n)` 시도, 실패하면 `n /= 2`. 성공한 n 반환, 모두 실패하면 0 |
| `mem_gather_ok(addrs, n, each, buf, ok)` | `buf` 를 0 으로 채운 뒤 `addrs[i]` 마다 `each` 바이트를 `buf[i*each]` 에 읽는다. 주소 0 은 건너뜀(0 유지). `process_vm_readv` 로 최대 `IOV_MAX` 범위씩. 배치 중간에서 멈추면 **멈춘 요소 하나만** `pread` 로 단독 판독(실패하면 그 요소 0 채움)하고, 그 뒤 요소들은 다음 반복에서 **다시 모아** `process_vm_readv` 로 읽는다(실패 범위 전체를 pread 하는 것이 아님; 정본 01 §3.5). `ENOSYS/EPERM` 이면 이후 전부 pread. 항상 0 반환. `ok` 가 NULL 이 아니면 읽힌 범위 표시 |
| `mem_cstr(addr, out, outn)` | `addr == 0` → −1. `pread(addr, out, outn)` 결과 `got < 2` 또는 `out[0..got)` 안에 NUL 없음 → −1. `out[0] != '\0'` 이고 `is_printable_text(out)`(ASCII 0x20..0x7E 또는 정형 UTF-8, 제어문자 없음) 이면 0. 짧은 판독도 NUL 이 들어 있으면 허용 |
| `name_of(ptr, out, n)` | 포인터→문자열 캐시(1024 슬롯 오픈 어드레싱, 키 해시 `((ptr>>3)*2654435761) & 1023`, 최대 512개 저장). 캐시 적중이면 `snprintf (out, n, "%s", vals[h])` 로 복사. `ptr == 0` 또는 `mem_cstr` 실패면 `snprintf (out, n, "?")` (캐시하지 않음). 항상 `out` 반환. 이 문서의 호출자는 모두 `n = 64` (01 §3.10) |

---

## 2. 대상 메모리 레이아웃

### 2.1 SYNC_CRITICAL_SECTION 배열 (`csectgl_Critical_sections`)

배열 주소 = `load_bias + elf_lookup("csectgl_Critical_sections")` (main 이 계산해 `cs_base` 로 넘긴다). 원소 i 의 주소 = `cs_base + i * stride`.

코드가 고정으로 아는 것은 `+0 name`, `+8 cs_index` 두 개뿐이다. 나머지는 stats 슬롯 S 를 찾은 뒤 S 기준 상대 오프셋으로 유도한다.

| 필드 | 원소 내 오프셋 | 크기 | 비고 |
|---|---|---|---|
| `name` | 0 | 8 | 엔진 문자열 리터럴 포인터 |
| `cs_index` | 8 | 4 | 0, 1, 2, … |
| `rwlock` | S − 96 | 4 | >0 reader 수, <0 writer(재진입 깊이), 0 비어 있음 |
| `waiting_readers` | S − 92 | 4 | u32 |
| `waiting_writers` | S − 88 | 4 | u32 |
| (pad + `readers_ok` cond 48 B) | S − 84 … S − 33 | — | 판독하지 않음 |
| `waiting_writers_queue` | S − 32 | 8 | THREAD_ENTRY * |
| `waiting_promoters_queue` | S − 24 | 8 | THREAD_ENTRY * |
| `owner` | S − 16 | 8 | writer 스레드 id (pthread_t) |
| `tran_index` | S − 8 | 4 | writer 트랜잭션 (뒤 4 B pad) |
| `stats` | S | 8 | SYNC_STATS * |

`CS_STATS_TO_RWLOCK = 96` 의 구성(ctv.h 주석): cond 48 + 큐 2개 16 + owner 8 + tran/pad 8 + rwlock 3종/pad 16.

참고 예(11.5 x86-64, 엔진 구조체에서 계산한 값이며 코드 상수가 아님): `pthread_mutex_t lock` 이 16..56 이므로 rwlock 56, stats S = 152, stride = 160, count = 18. 프로브는 이 값을 가정하지 않고 매번 찾는다.

### 2.2 SYNC_STATS 블록 (64 B)

| 오프셋 | 크기 | 필드 | 해독 |
|---|---|---|---|
| 0 | 4 | `type` | int32. 0 = 미사용(NONE), 1 CSECT, 2 RWLOCK, 3 RMUTEX, 4 MUTEX |
| 4 | 4 | pad | — |
| 8 | 8 | `name` | 이름 포인터 |
| 16 | 4 | `nenter` | u32 |
| 20 | 4 | `nwait` | u32 |
| 24 | 4 | `nreenter` | u32 |
| 28 | 4 | pad | — |
| 32 | 16 | `total_elapsed` | `tv_sec`(i64 @32), `tv_usec`(i64 @40) |
| 48 | 16 | `max_elapsed` | `tv_sec`(i64 @48), `tv_usec`(i64 @56) |

timeval → ms: `ms = (double) tv_sec * 1000.0 + (double) tv_usec / 1000.0` (`tv_to_ms`, 두 값을 부호 있는 i64 로 읽는다).

type 이름표 (`pool_type_names`, 인덱스 = type): `{"?", "CSECT", "RWLOCK", "RMUTEX", "MUTEX"}`.

### 2.3 sync_Stats 풀 청크 체인

풀 주소 = `load_bias + elf_lookup("sync_Stats")`. 이 주소가 **첫 청크 자체**다(포인터를 한 번 따라가는 것이 아님). 심볼이 없으면 main 이 `sym_pool = 0` 으로 두고 풀 기능은 수집하지 않는다.

청크 하나(엔트리 수 N):

| 오프셋 | 크기 | 내용 |
|---|---|---|
| `k * 64` (k = 0..N−1) | 64 | SYNC_STATS 엔트리 k (§2.2) |
| `N * 64` | 8 | `next` — 다음 청크 주소, 0 = 끝 |
| `N * 64 + 8` | 4 | (판독하지 않음) |
| `N * 64 + 12` | 4 | `in_use` — 사용 중 엔트리 수(int32) |

총 `CHUNK_BYTES(N) = N*64 + 16` B. N 은 상수가 아니라 I-12 로 프로빙한다(후보 256, 128, 64, 512 순).

---

## 3. ctv_cs.c — 크리티컬 섹션 모듈

### 3.1 책임과 소유 상태

| 항목 | 내용 |
|---|---|
| 책임 | CS 배열 레이아웃 프로브(I-0~I-4), SYNC_STATS 해독(I-5), 배열 이미지 → `CS_ROW` 해독, 대기 큐 머리 선택, 상태 토큰 |
| 소유 전역 | 없음. 프로브 결과는 호출자가 준 `CS_LAYOUT` 에만 쓴다. 거부 사유는 반환값 |
| 쓰는 외부 | `mem_read`, `mem_read_shrink`, `mem_gather_ok`, `mem_cstr`, `name_of`, `rd_*` |

### 3.2 함수 목록

| 함수 | 공개 | 반환 |
|---|---|---|
| `static double tv_to_ms(const unsigned char *b, size_t off)` | — | ms |
| `int stats_decode(const unsigned char *s, STATS_VIEW *v)` | 공개 | 0 / −1 |
| `static int stats_read(uint64_t addr, STATS_VIEW *v)` | — | 0 / −1 |
| `static const char *cs_probe_read(uint64_t cs_base, unsigned char *blob, size_t blobn, size_t *got)` | — | NULL / 사유 |
| `static const char *cs_probe_stride(const unsigned char *blob, size_t got, CS_LAYOUT *lo)` | — | NULL / 사유 |
| `static int ascii_name(const char *s)` | — | 1 / 0 |
| `static const char *cs_probe_count(uint64_t cs_base, const unsigned char *blob, size_t got, CS_LAYOUT *lo)` | — | NULL / 사유 |
| `static const char *cs_probe_stats_off(const unsigned char *blob, CS_LAYOUT *lo)` | — | NULL / 사유 |
| `static const char *cs_probe_rwlock(const unsigned char *blob, CS_LAYOUT *lo)` | — | NULL / 사유 |
| `uint64_t cs_wait_queues(const unsigned char *blob, const CS_LAYOUT *lo, int i, uint64_t *head2, int *field)` | 공개 | 첫 큐 머리 |
| `const char *cs_layout_probe(uint64_t cs_base, CS_LAYOUT *lo)` | 공개 | NULL / 사유 |
| `void cs_decode_raw(const unsigned char *blob, const CS_LAYOUT *lo, int i, int32_t *rwlock, uint32_t *wr, uint32_t *ww, int32_t *tran)` | 공개 | — |
| `int cs_read_rows(uint64_t cs_base, const CS_LAYOUT *lo, CS_ROW *rows)` | 공개 | 0 / −1 |
| `int cs_rows_from_blob(const unsigned char *blob, const CS_LAYOUT *lo, CS_ROW *rows)` | 공개 | 항상 0 |
| `const char *state_str(int32_t rwlock, char *buf, size_t n)` | 공개 | `buf` |

프로브 계열 규약: `const char *` 를 반환하며 **NULL = 통과**, 그 밖은 정적 문자열 거부 사유(`REFUSE("…")`). 거부 사유 문자열은 불변식 id 로 시작한다.

### 3.3 SYNC_STATS 해독

**`stats_decode(s, v)`** (I-5)

1. `v->type = rd_i32(s, 0)`.
2. `!ST_TYPE_OK(type)` (1..4 밖) 이면 −1 반환. 이때 `v` 의 나머지 필드는 쓰지 않는다.
3. `name_ptr = rd_u64(s, 8)`, `nenter = rd_u32(s, 16)`, `nwait = rd_u32(s, 20)`, `nreenter = rd_u32(s, 24)`, `total_ms = tv_to_ms(s, 32)`, `max_ms = tv_to_ms(s, 48)`.
4. 0 반환.

**`stats_read(addr, v)`**: `addr == 0` 이거나 `mem_read(addr, buf, 64)` 실패면 −1, 아니면 `stats_decode` 결과.

### 3.4 레이아웃 프로브 `cs_layout_probe(cs_base, lo)`

스택 버퍼 `blob[MAX_CS * 256]`(16384 B), `got = 0`.

1. **I-0 판독** `cs_probe_read`: `blob` 을 0 으로 채우고 `got = mem_read_shrink(cs_base, blob, 16384, 2048)` (16384 → 8192 → 4096 → 2048 순 시도). `got == 0` 이면 거부 `"I-0 cs array unreadable"`.
2. **I-1 stride** `cs_probe_stride`: 후보 `cand = 96, 104, …, 256` (8 단위) 을 오름차순으로:
   - `i = 0..3` (CS_MIN_SAMPLES) 마다 `i*cand + 8 + 4 > got` 이거나 `rd_i32(blob, i*cand + 8) != i` 이면 이 후보 탈락.
   - 4개 모두 맞으면 `lo->stride = cand`, 통과(**처음 맞는 후보**).
   - 맞는 후보가 없으면 거부 `"I-1 cs_index sequence"`.
3. **I-2 count** `cs_probe_count`: `lo->count = 0`, `i = 0..MAX_CS−1`:
   - 원소 앞 16 B(`head`)를 얻는다: `(i+1)*stride <= got` 이면 `blob + i*stride` 에서 복사, 아니면 `mem_read(cs_base + i*stride, head, 16)`; 판독 실패면 루프 종료.
   - `rd_i32(head, 8) != i` 이거나, `mem_cstr(rd_u64(head, 0), name, 64) != 0` 이거나, `ascii_name(name) == 0` 이면 루프 종료.
   - 통과하면 `lo->count = i + 1`.
   - 루프 후 `count < 4` 이면 거부 `"I-2 cs count"`.
   - `ascii_name(s)`: 모든 바이트가 `0x21..0x7E` 이면 1(공백·제어·상위 비트 바이트 불가; `char` 가 부호 있으므로 0x80 이상은 음수로 걸린다). 빈 문자열은 1 이지만 `mem_cstr` 가 빈 문자열을 이미 거부한다.
4. **I-0 재판독**: `need = count * stride`. `need > got` 이면 `mem_read(cs_base, blob, need)`; 실패하면 거부 `"I-0 cs array read"`.
5. **I-3 stats 슬롯** `cs_probe_stats_off`: 후보 `cand = 16, 24, …, stride − 8` (8 단위):
   - 모든 CS `i < count` 에 대해 `stats_read(rd_u64(blob, i*stride + cand))` 가 성공하고 `type == 1 (CSECT)` 이고 `name_ptr == rd_u64(blob, i*stride + 0)` (CS 의 이름 **포인터 값** 과 동일) 이어야 한다. 하나라도 어긋나면 이 후보 탈락.
   - 모든 CS 를 통과한 후보 수 `found` 와 마지막 통과 후보 `best` 를 센다(모든 후보를 끝까지 검사).
   - `found == 0` → 거부 `"I-3 stats pointer"`; `found >= 2` → 거부 `"I-3 stats pointer not unique"`; `found == 1` → `lo->off_stats = best`.
   - stats 블록은 CS·후보마다 개별 `mem_read` 64 B 로 읽는다(묶음 판독 아님).
6. **I-4 rwlock 묶음** (재시도 루프): `tries = 0 .. CS_PAIR_RETRIES−1` (총 3회):
   - `tries > 0` 이면 `mem_read(cs_base, blob, need)` 로 배열을 다시 읽는다; 실패하면 즉시 거부 `"I-0 cs array read"`.
   - `cs_probe_rwlock(blob, lo)` 가 NULL 이면 프로브 성공(NULL 반환).
   - 3회 모두 거부면 **마지막 시도의 사유**를 반환.

`cs_probe_rwlock(blob, lo)` (I-4):

1. 유도: `off_tran = S−8`, `off_owner = S−16`, `off_wq = S−32`, `off_pq = S−24`, `off_rwlock = S−96`. (이 대입은 거부 여부와 무관하게 `lo` 에 남는다.)
2. `off_rwlock < 16` 이면 거부 `"I-4 rwlock offset"`.
3. `viol = 0`. 각 CS `i < count` (원소 오프셋 `off = i*stride`):
   - `rd_u32(off+off_rwlock+4) > 1000000` 또는 `rd_u32(off+off_rwlock+8) > 1000000` → 즉시 거부 `"I-4 waiting counters"`.
   - `wq = rd_u64(off+off_wq)`, `pq = rd_u64(off+off_pq)`; `(wq != 0 && !VALID_PTR(wq)) || (pq != 0 && !VALID_PTR(pq))` → 즉시 거부 `"I-4 waiter queue pointer"`.
   - `held = rd_i32(off+off_rwlock) < 0`, `tid_set = rd_u64(off+off_owner) != 0`; `held && !tid_set` 이면 `viol++`.
4. `viol >= 2` → 거부 `"I-4 writer without owner"`. 즉 writer 인데 owner 가 0 인 CS 는 **1개까지 허용**(무락 판독의 찢김). 역방향(`owner != 0 ⇒ rwlock < 0`)은 검사하지 않는다: demote·reader 퇴장이 owner/tran 을 남기기 때문.

거부 시 호출자(main) 동작: `die("cannot validate SYNC_CRITICAL_SECTION layout (%s) - refusing to guess", "SYNC_CRITICAL_SECTION 레이아웃 검증 실패 (%s) - 추측하지 않고 중단", why)` 로 종료(stderr, `cub_ctv: ` 접두, exit 1). 성공 시 main 은 레이아웃 줄을 찍는다(ctv_cs.c 밖, 정본 06 §2.12 `print_layout_line`).

레이아웃 줄 바이트 규칙(코드 그대로):

| 항목 | 내용 |
|---|---|
| 출력 조건 | `!o.json` 일 때만. 시점은 CS 프로브 → `tran_subsystem_init` → (스레드 프로브) **뒤**. 바로 앞 줄은 프로브 전에 찍은 대상 줄 `target: pid=%d db=%s lib=%s\n` / `대상: pid=%d db=%s lib=%s\n` |
| 스트림·색 | `printf` → **stdout**. 색(`clr`) 없음, 앞에 빈 줄 없음 |
| 1 | `printf (M ("layout: stride=%dB count=%d stats_off=%d (runtime-probed)", "레이아웃: stride=%dB count=%d stats_off=%d (런타임 검증 통과)"), stride, count, off_stats)` |
| 2 | `threads >= 0 && !(degraded & DEG_THREAD)` 이면 `printf ("  threads=%d", threads)` (공백 2). `threads` = 스레드 프로브를 건너뛰었으면(`--trans` 단독) −1, 아니면 `thr_geom.count`; 재생은 `h.thr_count` |
| 3 | `deg_names[]` 순서(§5.5 표)로 켜진 비트마다: `printf (" [%s (%s)", label, id)` → 그 비트가 `DEG_TRAN` 이고 전역 `tran_refused != NULL` 이면 `printf (": %s", tran_refused)` → `DEG_TRAN_CAP` 이면 `printf (": %d", TRAN_SLOTS_MAX)` (= `": 65536"`) → `printf ("]")`. 즉 항목 형식은 `␠[<label> (<id>)]` 또는 `␠[<label> (<id>): <부가>]`, 항목 사이 구분자는 각 항목 앞의 공백 1개뿐. label·id 는 언어와 무관하게 영어 |
| 4 | `printf ("\n")` |

예(fixture `demodb-snapshot.rec` 재생, 강등 없음): `layout: stride=160B count=18 stats_off=152 (runtime-probed)  threads=4314\n`. 강등이 있으면 같은 줄 끝에 예컨대 ` [trid not validated (I-8)]`, 트랜잭션 표 거부 시 ` [no transaction table (I-6/I-7): log_Gl symbol]` 가 붙는다.

### 3.5 대기 큐 머리 `cs_wait_queues(blob, lo, i, head2, field)`

엔진이 깨우는 순서(승격 대기자 → writer 줄 맨 앞)대로 대기 writer 를 한 체인으로 보기 위한 선택 함수. 두 큐에 선 스레드 모두 `waiting_writers`(waitW)에 포함된다.

1. `off = i*stride`, `pq = rd_u64(off+off_pq)`, `wq = rd_u64(off+off_wq)`.
2. `head2 != NULL` 이면 `*head2 = (pq != 0) ? wq : 0`.
3. `field != NULL` 이면 `*field = (pq != 0) ? off_pq : off_wq` (첫 머리를 읽은 CS 내 오프셋; 샘플러가 I-18/I-20 런타임 닻 재확인에 사용).
4. 반환 `(pq != 0) ? pq : wq`.

| pq | wq | 반환(첫 머리) | head2 | field |
|---|---|---|---|---|
| 0 | 0 | 0 | 0 | off_wq |
| 0 | W | W | 0 | off_wq |
| P | 0 | P | 0 | off_pq |
| P | W | P | W | off_pq |

### 3.6 행 해독

**`cs_decode_raw(blob, lo, i, &rwlock, &wr, &ww, &tran)`**: `off = i*stride`; `rwlock = rd_i32(off+off_rwlock)`, `wr = rd_u32(off+off_rwlock+4)`, `ww = rd_u32(off+off_rwlock+8)`, `tran = rd_i32(off+off_tran)`. 검증·부작용 없음.

**`cs_read_rows(cs_base, lo, rows)`**:

1. `blob = malloc(stride * count)`.
2. `blob == NULL` 또는 `mem_read(cs_base, blob, stride*count) != 0` → `free(blob)`, −1 반환(호출자는 "대상 사라짐"으로 종료).
3. `cs_rows_from_blob(blob, lo, rows)` 결과를 반환하고 `blob` 해제.

**`cs_rows_from_blob(blob, lo, rows)`** (라이브 루프가 마지막 샘플 이미지로 직접 호출하기도 함):

1. `count` = `lo->count` 를 `[0, MAX_CS]` 로 자른 값, `saddr` 는 0 으로 초기화. `i < count` 마다 `saddr[i] = rd_u64(blob, i*stride + off_stats)`.
2. `mem_gather_ok(saddr, count, 64, sblk, NULL)` 로 stats 블록을 한 번에 모은다(반환값 무시; 읽지 못한 블록·주소 0 은 0 채움).
3. `rows[0..count)` 를 0 으로 초기화.
4. 각 i:
   - `name_of(rd_u64(off+0), r->name, 64)` (실패 시 `"?"`).
   - `cs_decode_raw` 로 `rwlock, wr, ww, tran`.
   - `owner = rd_u64(off+off_owner)`.
   - `wq = cs_wait_queues(blob, lo, i, &wq2, NULL)`.
   - `saddr[i] != 0` 이고 `stats_decode(sblk + i*64) == 0` 이면 `has_stats = 1` 과 `nenter, nwait, nreenter, total_ms, max_ms` 복사. 아니면 `has_stats = 0` 이고 통계 필드는 0. (행 해독에서는 type 이 CSECT 인지, 이름 포인터가 같은지는 다시 보지 않는다 — I-5 는 type 1..4 만.)
5. 0 반환.

`rows` 는 `count` 개 이상 공간이 있어야 한다(호출자는 `MAX_CS` 배열 사용).

개수 상한: `cs_rows_from_blob` 은 첫머리에서 `lo->count` 를 `[0, MAX_CS]` 로 자르고 gather(`n = count`)·`memset`·행 루프 모두 이 `count` 를 쓴다. 지역 버퍼 `saddr[MAX_CS]`·`sblk[MAX_CS * 64]` 를 넘지 않는다. 실제 호출자는 둘뿐이며(`cs_read_rows` ← main·라이브 시작, ctv_live.c 의 구간 마감) 모두 `cs_layout_probe` 가 만든 `CS_LAYOUT`(I-2 로 4..64)을 넘기므로 절단은 방어용이다. 재생 경로는 이 함수를 부르지 않는다(행은 기록 파일의 `C` 줄에서 온다, 05 §6.5).

### 3.7 상태 토큰 `state_str(rwlock, buf, n)`

모든 언어에서 영어 그대로(한글 2칸 폭이 표 열을 깨므로).

| rwlock | 문자열 |
|---|---|
| > 0 | `"%d readers"` (1 이어도 `"1 readers"`) |
| < 0 | `"1 writer"` (재진입 깊이와 무관) |
| 0 | `"none"` |

`snprintf(buf, n, …)` 후 `buf` 반환.

### 3.8 불변식 요약 (CS)

| id | 규칙 | 임계·재시도 | 실패 시 |
|---|---|---|---|
| I-0 | 배열이 읽힌다: 16384 B 부터 반으로 줄여 2048 B 까지; count 확정 후 `count*stride` 가 이미 읽은 양보다 크면 그 크기로 재판독; I-4 재시도마다 재판독 | — | `"I-0 cs array unreadable"` / `"I-0 cs array read"` → main 종료 |
| I-1 | `cand ∈ {96..256 step 8}` 중 원소 0..3 의 `cs_index@+8 == i` 인 첫 값 = stride | 표본 4 | `"I-1 cs_index sequence"` |
| I-2 | count = `cs_index == i ∧ name 이 mem_cstr 통과 ∧ 전 바이트 0x21..0x7E` 인 연속 구간 길이 | ≥ 4, ≤ 64 | `"I-2 cs count"` |
| I-3 | stats 슬롯 = 모든 CS 에서 `stats->type == 1 ∧ stats->name == cs->name` 인 **유일한** 8 정렬 오프셋 (16..stride−8) | 유일성 | `"I-3 stats pointer"` / `"I-3 stats pointer not unique"` |
| I-4 | 유도(−96/−16/−32/−24/−8) + `off_rwlock ≥ 16` + 대기수 ≤ 1,000,000 + 큐 포인터 0 또는 VALID_PTR + `rwlock<0 ∧ owner==0` 인 CS ≤ 1 | 3회(재판독 포함), 마지막 사유 | `"I-4 rwlock offset"` / `"I-4 waiting counters"` / `"I-4 waiter queue pointer"` / `"I-4 writer without owner"` |
| I-5 | 행별 `SYNC_STATS.type ∈ 1..4` | — | 그 행 `has_stats = 0` (표에서 통계 칸이 `-`), 종료하지 않음 |

---

## 4. ctv_pool.c — sync_Stats 풀 모듈

### 4.1 책임과 소유 상태

| 항목 | 내용 |
|---|---|
| 책임 | 풀 청크 엔트리 수 프로브(I-12), 체인 순회, (type, name) 묶음, 그룹 출력, 덤프용 전 엔트리 JSON |
| 소유 전역 | `static unsigned char *probe_head` — 마지막으로 통과한 프로브의 첫 청크 이미지(`CHUNK_BYTES(N)` 바이트 이상, 할당은 `CHUNK_BYTES(512)`); `static uint64_t probe_head_addr` — 그 주소. `pool_walk` 내 `static uint64_t visited[POOL_MAX_CHUNKS]` — 순회 중 방문 청크 주소 |
| 쓰는 외부 전역 | `pool_dump` 만 `view.pool_state`, `view.npool`, `view.pool`, `view.pool_truncated` 를 쓴다 |

### 4.2 함수 목록

| 함수 | 공개 | 반환 |
|---|---|---|
| `static int pool_probe_entries(uint64_t pool_addr)` | — | N (256/128/64/512) / −1 |
| `static int pool_walk(uint64_t pool_addr, int nent, POOL_GROUP *g, int maxg, int *truncated, FILE *dump)` | — | 그룹 수 |
| `void pool_print_groups(const POOL_GROUP *g, int ng, int truncated)` | 공개 | — |
| `int pool_collect(uint64_t pool_addr, POOL_GROUP *g, int maxg, int *ng, int *truncated)` | 공개 | 0 / −1 |
| `int pool_dump(FILE *f, uint64_t pool_addr)` | 공개 | 0 / −1 |

### 4.3 I-12 청크 엔트리 수 프로브 `pool_probe_entries(pool_addr)`

1. `raw = malloc(CHUNK_BYTES(512))` (32784 B). 실패하면 −1.
2. 후보 `n` 을 **256, 128, 64, 512** 순서로:
   1. `mem_read(pool_addr, raw, CHUNK_BYTES(n))` 실패 → 이 후보 탈락.
   2. `nxt = rd_u64(raw, n*64)`, `in_use = rd_i32(raw, n*64 + 12)`.
   3. `k = 0..n−1` 마다 `t = rd_i32(raw, k*64)`; `t < 0 || t > 4` 이면 탈락(즉시 중단). `t != 0` 이면 `used++`.
   4. 조건: `in_use >= 0 && in_use <= n && |used − in_use| <= 2` (동시 갱신 여유 ±2).
   5. `nxt == 0` 이면 통과. 아니면 `mem_read(nxt, head, 4)` 가 성공하고 `0 <= rd_i32(head, 0) <= 4` 여야 통과.
   6. 통과하면: 이전 `probe_head` 해제, `probe_head = raw`, `probe_head_addr = pool_addr`, `n` 반환(첫 통과 후보).
3. 모두 탈락하면 `raw` 해제, −1.

변수 범위(코드 그대로):

| 변수 | 범위 | 초기화 |
|---|---|---|
| `raw` | 함수 전체, 후보 사이에 **같은 버퍼 재사용**(후보마다 `CHUNK_BYTES(n)` 바이트만 덮어씀) | 함수 시작 시 1회 `malloc` |
| `ok` | 후보마다 | 0 |
| `nxt`, `in_use`, `k`, `used`, `types_ok` | 후보마다(판독 성공 블록 안에서 선언) | `used = 0`, `types_ok = 1` |

- 2.3 의 루프는 `for (k = 0; k < n && types_ok; k++) { t = rd_i32(…); types_ok = (0 <= t && t <= 4); used += (t != 0); }` 이다. 범위 밖 `t` 를 만난 반복에서도 `used` 가 한 번 더해지지만 `types_ok = 0` 이므로 결과에 영향이 없다.
- 2.4 조건은 코드상 `used − in_use <= 2 && in_use − used <= 2` (int 연산).
- `probe_head` 교체는 **통과한 후보에서만** 일어난다. 탈락 후보는 `probe_head`·`probe_head_addr` 를 건드리지 않는다. 모든 후보가 탈락하면 이번 호출의 `raw` 는 해제되고, 이전 호출에서 통과해 남은 `probe_head`(다른 시점의 이미지)는 그대로 남는다(뒤이은 `pool_walk` 는 프로브 성공 후에만 불리므로 이 낡은 이미지가 쓰이지는 않는다).
- 통과한 `raw` 는 `probe_head` 로 넘어가므로 해제하지 않는다. 버퍼 크기는 항상 `CHUNK_BYTES(512)` 라서 어떤 `nent` 로도 `memcpy (raw, probe_head, CHUNK_BYTES(nent))` 가 안전하다.

### 4.4 체인 순회 `pool_walk(pool_addr, nent, g, maxg, truncated, dump)`

상태: `chunk = pool_addr`, `ng = 0`(그룹 수), `hop = 0`(청크 순번), `total = 0`(유효 엔트리 수), 해시 `gh = calloc(4096, sizeof(unsigned short))`(값 = 그룹 인덱스 + 1, 0 = 빈 슬롯), `raw = malloc(CHUNK_BYTES(nent))`.

1. `*truncated = −1`. `raw` 또는 `gh` 할당 실패면 둘 다 해제, `*truncated = POOL_NO_MEMORY (−3)`, 0 반환.
2. `chunk != 0` 인 동안:
   1. `hop >= 1024 || total >= 65536` → `*truncated = hop`, 중단. (`total` 검사는 청크 시작 시점에만 하므로 마지막 청크에서 65536 을 넘을 수 있다.)
   2. `visited[0..hop)` 에 `chunk` 가 있으면 `*truncated = POOL_CHAIN_LOOP (−4)`, 중단.
   3. `visited[hop] = chunk`.
   4. `hop == 0 && probe_head != NULL && probe_head_addr == chunk` 이면 `probe_head` 의 `CHUNK_BYTES(nent)` 바이트를 `raw` 로 복사(첫 청크는 다시 읽지 않음). 아니면 `mem_read(chunk, raw, CHUNK_BYTES(nent))`; 실패면 `*truncated = hop`, 중단.
   5. 엔트리 `i = 0..nent−1`:
      - `stats_decode(raw + i*64)` 실패(type ∉ 1..4)면 건너뜀(type 0 미사용 슬롯 포함).
      - `name_of(v.name_ptr, name, 64)` (실패 시 `"?"`).
      - `total++`.
      - `dump != NULL` 이면 엔트리 JSON 한 줄 출력(§4.7).
      - 그룹 찾기: `h = (unsigned) fnv(fnv(FNV_SEED, &v.type, 4), name, strlen(name)) & 4095`; `gh[h] != 0` 인 동안 `g[gh[h]−1].type == v.type && strcmp(name) == 0` 이면 그 그룹, 아니면 `h = (h+1) & 4095` 선형 탐사. 찾으면 `k = gh[h]−1`, 빈 슬롯에서 멈추면 `k = ng`(새 그룹), `slot = h`.
      - 새 그룹(`k == ng`)인데 `ng >= maxg` 이면: `*truncated == −1` 일 때만 `*truncated = POOL_GROUPS_CAPPED (−2)`; 이 엔트리는 묶지 않고 다음으로. 아니면 `g[ng]` 0 초기화, `type`, `name` 복사, `gh[slot] = ++ng`.
      - `g[k].cnt++`, `g[k].nenter += v.nenter`, `v.max_ms > g[k].max_ms` 이면 갱신.
   6. `chunk = rd_u64(raw, nent*64)`, `hop++`.
3. `raw`, `gh` 해제, `ng` 반환.

`*truncated` 최종값 의미:

| 값 | 뜻 |
|---|---|
| −1 | 체인 끝(next 0)까지 정상 |
| ≥ 0 | 그 청크 순번에서 끊김(판독 실패, 1024 홉 상한, 65536 엔트리 상한) |
| −2 | 체인은 끝까지 읽었으나 서로 다른 이름이 1024 를 넘어 나머지는 묶지 않음 (이후 다른 중단이 생기면 그 값으로 덮임) |
| −3 | cub_ctv 메모리 부족, 아무것도 읽지 않음 |
| −4 | 이미 읽은 청크로 되돌아옴(찢긴 판독·재사용 메모리), 그 지점에서 중단 |

그룹 순서 = 처음 본 순서. 그룹은 CSECT 를 포함해 모든 type 을 묶는다(출력에서 CSECT 를 뺀다).

### 4.5 수집 `pool_collect(pool_addr, g, maxg, ng, truncated)`

1. `nent = pool_probe_entries(pool_addr)`; −1 이면 −1 반환(`*ng`, `*truncated` 는 건드리지 않음).
2. `*ng = pool_walk(pool_addr, nent, g, maxg, truncated, NULL)`, 0 반환.

호출자 `view_capture_pool`(ctv_frame.c)은 미리 `view.npool = 0`, `view.pool_truncated = −1` 로 두고 결과로 `view.pool_state = (0 → 1, −1 → −1)` 을 정한다. 스냅샷에서 `sym_pool != 0` 이고 (`--record` 이거나, `--trans`·`--json`·`--detail` 이 아닌 경우) 에만 호출된다(덤프 실행에서는 `pool_dump` 가 대신 채운다).

모드별 풀 수집·표시(옵션 정의는 06 §2.2, 경로는 06 §2.11, 기록 형식은 05 §6):

| 모드 | 수집 | 화면 표시 | 기록(CTVREC) |
|---|---|---|---|
| 스냅샷(옵션 없음) | 1회 `view_capture_pool` | `print_view_pool` (`render_snapshot` 마지막) | — |
| 스냅샷 `--record` | 1회(`--trans`/`--json`/`--detail` 이어도 수집) | 위 조건대로 | `F` 의 `pool_state`, `pool_state == 1` 이면 `G` 줄(05 §6.5) |
| 스냅샷 `--json` / `--detail` / `--trans` (기록 없음) | 수집 안 함(`pool_state = 0` 그대로) | 없음(이 경로의 `render_snapshot` 은 `print_view_pool` 을 부르지 않음) | — |
| 스냅샷 `--dump` | `pool_dump` 의 걷기 1회(§4.7) | 일반 스냅샷 경로면 `print_view_pool` | `--record` 를 함께 주면 덤프가 채운 그룹이 기록됨 |
| 라이브 `-i` (화면·파이프·`--json` 모두) | **수집 안 함**(시작 시에도, 구간마다도 아님). `view_capture` 가 매 구간 `pool_state = 0` | 없음(라이브 렌더러는 `print_view_pool` 을 부르지 않음) | 라이브 기록의 `F` 는 `pool_state 0`, `G` 없음 |
| 재생 | 대상 메모리를 읽지 않음 | 스냅샷 기록 재생만 `render_snapshot` → `print_view_pool`(기록된 `G` 사용). 라이브 기록 재생은 표시 안 함 | — |

`--hz`(샘플링 주파수, 기본 100, 1..10000)와 `--interval`(구간 길이 초, 기본 1.0, 0.1..3600)은 라이브 전용이며 풀과 무관하다(06 §2.2, §2.6). `--plain` 은 색만 끈다(`color_on = 0`).

### 4.6 그룹 출력 `pool_print_groups(g, ng, truncated)`

CSECT 그룹은 CS 표와 같은 통계 블록이므로 출력하지 않는다. `t = 2 (RWLOCK), 3 (RMUTEX), 4 (MUTEX)` 순으로:

1. `tt` = type t 그룹들의 `cnt` 합.
2. type t 그룹을 저장 순서대로 출력. 첫 그룹 앞에서 한 번 머리를 찍는다:
   - `printf("\n%s", clr(CLR_HEAD))`
   - `printf(M("[pool] %s: %d entries (grouped by name)", "[풀] %s: %d엔트리 (이름별 그룹)"), pool_type_names[t], tt)`
   - `printf("%s\n%s  %-36s %5s %12s %10s%s\n", clr(CLR_OFF), clr(CLR_HEAD), "name", "n", "nenter", "max_ms", clr(CLR_OFF))`
3. 행: `printf("  %-36s %5d %12lu %10.3f\n", name, cnt, (unsigned long) nenter, max_ms)`.
4. 그룹이 없는 type 은 머리도 찍지 않는다.

`clr()` 값(정본 06 §5.2): `color_on == 0` 이면 모든 역할이 `""`. 켜져 있으면 `clr(CLR_HEAD)` = `"\033[1;38;5;73m"` (ESC `[1;38;5;73m`, 굵은 청록), `clr(CLR_OFF)` = `"\033[0m"`. `color_on = !--plain && !--json && isatty(STDOUT_FILENO)` (06 §2.10). 따라서 색이 꺼진 머리 블록의 바이트는 `"\n[pool] RWLOCK: 2 entries (grouped by name)\n  name                                     n       nenter     max_ms\n"` 이다(`name` 뒤 공백 32개 + 공백 1 + `    n` …; fixture `demodb-snapshot.rec` 재생 출력과 일치).

열 정렬 규칙:

| 항목 | 규칙 |
|---|---|
| 이름 열 `%-36s` | **바이트 기준** 왼쪽 정렬 폭 36. `dw_pad`·열 폭 계산을 쓰지 않으며 자르지도 않는다. 36바이트를 넘는 이름(버퍼상 최대 63바이트)은 그대로 다 찍혀 뒤 열이 오른쪽으로 밀리고, 다중 바이트 UTF-8 이름은 바이트 수만큼 채움이 줄어 화면 열이 어긋날 수 있다(이름은 `mem_cstr` 를 통과한 인쇄 가능 텍스트이므로 정화는 불필요). 00-overview 의 "폭은 코드 포인트 단위" 원칙은 이 표에는 적용되지 않는다 |
| `n` `%5d` | `cnt`, 5자리 오른쪽 정렬, 넘치면 그대로 늘어남 |
| `nenter` `%12lu` | `uint64_t` 합을 `unsigned long`(LP64 에서 64비트, 손실 없음)으로 캐스트. 12자리를 넘으면 자르지 않고 그대로 늘어나 뒤 열이 밀린다(절단 규칙 없음) |
| `max_ms` `%10.3f` | 소수 3자리, 폭 10 |

이어서 `truncated` 에 따라 한 줄(해당 없으면 없음). 출력은 머리·행과 같은 `printf` → **stdout**, 색(`clr`) 없음, `warn` 이 아니다:

| 조건 | 영어 | 한국어 |
|---|---|---|
| `truncated >= 0` | `  (pool chain truncated at chunk %d - counts incomplete)\n` | `  (풀 체인이 청크 %d에서 끊김 - 집계 불완전)\n` |
| `== POOL_NO_MEMORY` | `  (pool not grouped: out of memory in cub_ctv)\n` | `  (풀 집계 생략: cub_ctv 메모리 부족)\n` |
| `== POOL_CHAIN_LOOP` | `  (pool chain came back to a chunk already read - stopped there)\n` | `  (풀 체인이 이미 읽은 청크로 돌아옴 - 그 지점에서 멈춤)\n` |
| `== POOL_GROUPS_CAPPED` | `  (more than %d distinct names - the rest not grouped)\n` (%d 인자는 **상수 `POOL_MAX_GROUPS`** = 1024; `pool_walk` 의 `maxg` 가 아님. 현재 모든 호출자가 `maxg = POOL_MAX_GROUPS` 로 부르므로 값은 같다) | `  (서로 다른 이름이 %d개를 넘음 - 나머지는 묶지 않음)\n` |

관련 문자열(호출자 `print_view_pool`, ctv_rows.c — 이 모듈 밖이지만 풀 절의 세 상태를 이룬다):

| `view.pool_state` | 동작 |
|---|---|
| 1 | `pool_print_groups(view.pool, view.npool, view.pool_truncated)` |
| < 0 | `warn("stats pool did not validate - skipping pool view", "통계 풀 검증 실패 - 풀 뷰 생략")` → **stderr** 에 `cub_ctv: <문구>\n` (색 없음, stdout 에는 아무것도 쓰지 않음) |
| 0 | `printf(M("\n[pool] not collected (sync_Stats not exported)\n", "\n[풀] 미수집 (sync_Stats 미노출)\n"))` |

### 4.7 덤프 `pool_dump(f, pool_addr)`

1. `nent = pool_probe_entries(pool_addr)`; −1 이면 `view.pool_state = −1`, −1 반환. **아무것도 쓰지 않는다**(호출자가 `"pool":null` 을 쓴다).
2. `fprintf(f, "\"pool_chunk_entries\":%d,\"pool\":[\n", nent)`.
3. `view.npool = pool_walk(pool_addr, nent, view.pool, POOL_MAX_GROUPS, &view.pool_truncated, f)` — 한 번의 순회로 전 엔트리를 덤프하고 그룹을 프레임에 채운다(덤프 뒤 화면 출력이 이 그룹을 쓴다).
4. `view.pool_state = 1`.
5. `t = view.pool_truncated`; `fprintf(f, "],\"pool_truncated_at_chunk\":%d,\"pool_walk\":\"%s\"", t >= 0 ? t : −1, word)`.

| t | `pool_walk` 값 |
|---|---|
| ≥ 0 | `"chain_cut"` |
| −2 | `"groups_capped"` |
| −3 | `"no_memory"` |
| −4 | `"chain_loop"` |
| −1 | `"complete"` |

6. 0 반환.

엔트리 한 줄(유효 엔트리마다, `total > 1` 이면 앞에 `,`):

```text
{"chunk":<hop>,"entry":<i>,"type":"<CSECT|RWLOCK|RMUTEX|MUTEX>","name":<json 문자열>,"nenter":<u>,"nwait":<u>,"nreenter":<u>,"total_ms":<%.3f>,"max_ms":<%.3f>}\n
```

| 키 | 타입 | 출처 |
|---|---|---|
| `chunk` | int | 청크 순번 `hop` (0 = sync_Stats 자신) |
| `entry` | int | 청크 내 엔트리 인덱스 0..N−1 |
| `type` | string | `pool_type_names[type]` |
| `name` | string | `name_of`, 실패 시 `"?"`; `json_fstr` 로 이스케이프 |
| `nenter`, `nwait`, `nreenter` | uint | SYNC_STATS 원값 |
| `total_ms`, `max_ms` | number(소수 3자리) | timeval → ms |

덤프에는 CSECT 엔트리도 나온다. `nwait` 는 덤프에만 있고 그룹에는 없다.

### 4.8 불변식 요약 (풀)

| id | 규칙 | 실패 시 |
|---|---|---|
| I-12 | 청크 N ∈ {256,128,64,512}(이 순서) 중 첫 통과: 전 엔트리 type ∈ 0..4 ∧ 0 ≤ in_use ≤ N ∧ \|used − in_use\| ≤ 2 ∧ next ∈ {0, 4 B 판독 가능하고 첫 int 가 0..4} | 수집: `pool_state = −1`, 경고 후 풀 뷰 생략. 덤프: `"pool":null` |
| (순회) | 홉 ≤ 1024, 엔트리 ≤ 65536, 그룹 ≤ 1024, 방문 청크 재방문 금지, 청크 판독 성공 | `pool_truncated` 센티널·청크 번호, 출력에 한 줄 표시 |

---

## 5. ctv_dump.c — 디테일 덤프 모듈

### 5.1 책임과 소유 상태

| 항목 | 내용 |
|---|---|
| 책임 | `--dump FILE`: 한 시점에 읽을 수 있는 모든 것(대상 정보, 심볼, 레이아웃, 강등 상태, CS 원시 행, 전 트랜잭션 슬롯, 전 스레드 엔트리, 전 풀 엔트리)을 JSON 문서 하나로 쓴다. 재생 파일이 아니다 |
| 소유 상태 | 없음 |
| 함수 | `void detail_dump_write(const char *path, const VIEW_HDR *h, uint64_t bias, uint64_t sym_cs, uint64_t sym_pool, uint64_t sym_loggl, uint64_t sym_thr)` |

### 5.2 호출 맥락

main 의 스냅샷 경로에서: 레이아웃 프로브 성공 → 트랜잭션 초기화 → 스레드 프로브(`thr_keep_image = 1`: 엔트리 이미지를 덤프용으로 보존) → `cs_read_rows` → `view_capture(&lo, rows, NULL, 0.0)` (이때 `view.ts = time(NULL)`, `view.pool_state = 0`) → `detail_dump_write(...)`. 덤프 뒤에는 평소대로 스냅샷을 화면에 출력하며, 풀 그룹은 `pool_dump` 가 채운 것을 쓴다. `sym_*` 는 로드 바이어스를 더하기 **전** 의 ELF 심볼 값이다(없으면 0; `sym_cs` 는 항상 있음).

`VIEW_HDR` (정의 ctv.h, 정본 05 §3.6). 덤프가 읽는 필드:

| 필드 | 타입 | 값(main 이 `memset` 0 후 설정) | 정화 |
|---|---|---|---|
| `pid` | `int` | 대상 pid | — |
| `db` | `char[128]` | `snprintf (h.db, 128, "%s", db)`; `db` 는 `SERVER.db` (cmdline 첫 인자, `server_by_pid` 에서 `text_sanitize` 됨) | 정화됨 |
| `lib` | `char[4096]` | `snprintf (h.lib, 4096, "%s", lib)`; `lib` 는 `find_lib_base` 가 돌려준 maps 경로(§5.5 `target.lib`) | **정화 안 됨**(원문). 화면의 대상 줄만 별도 복사본을 정화 |
| `tran_refused` | `char[64]` | 전역 `tran_refused` 의 63바이트 절단 복사, 없으면 `""` (01 §1.6 "tran_refused 수명") | 정적 리터럴 |
| `lo` | `CS_LAYOUT` | 프로브 결과 전체(8개 필드 모두 유효) | — |

나머지 필드(`live`, `hz`, `thr_count`, `interval`, `degraded`)는 덤프가 쓰지 않는다. 특히 `degraded` 키는 `h->degraded` 가 아니라 덤프 시점의 전역 `degraded` 다.

화면·스트림 순서(`--dump FILE`, 대상 줄·레이아웃 줄은 `--json` 이 아닐 때만):

| 순서 | 스트림 | 내용 |
|---|---|---|
| 1 | stdout | 대상 줄 `target: pid=%d db=%s lib=%s\n` (CS 프로브 전) |
| 2 | stdout | 레이아웃 줄(§3.4) |
| 3 | 파일 | 덤프 문서 전체(§5.3~). **완료를 알리는 메시지는 없다**("wrote …" 등 출력 없음). 실패만 `die` |
| 4 | 파일 | `--record` 를 함께 주었으면 이어서 그 기록에 프레임 1개 기록·닫기(풀 그룹은 덤프가 채운 것) |
| 5 | stdout/stderr | `render_snapshot` (06 §2.11): 옵션에 따라 `--trans`/`--json`/`--detail`/일반 스냅샷. 일반 스냅샷이면 마지막에 `print_view_pool` 이 `view.pool_state` 를 보고: 1 → 그룹 표(stdout), −1 (`pool_dump` 의 I-12 실패) → stderr 에 `cub_ctv: stats pool did not validate - skipping pool view\n` / `cub_ctv: 통계 풀 검증 실패 - 풀 뷰 생략\n` **1회**(덤프 쪽에서는 경고를 찍지 않으므로 중복 없음), 0 (`sym_pool == 0`) → stdout 에 `\n[pool] not collected (sync_Stats not exported)\n` |

### 5.3 알고리즘

1. `f = private_create(path, 0)` — 함수는 ctv_rec.c 에 있고 정본은 05 §9.1. 두 번째 인자 `excl = 0` 은 "이미 있는 이름도 허용"(`O_EXCL` 을 붙이지 않음)이라는 뜻이다(`excl = 1` 은 라이브 `[r]` 의 자동 생성 이름용). 동작: `open (path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600)` → `fstat` 성공 ∧ `S_ISREG` ∧ `st_nlink == 1` ∧ `st_uid == geteuid ()` ∧ `fchmod (fd, 0600) == 0` ∧ `ftruncate (fd, 0) == 0` ∧ `fcntl (fd, F_SETFL, 0) == 0` 이 모두 성립해야 하고 하나라도 실패하면 fd 를 닫고 NULL → `fdopen (fd, "w")` (실패 시 fd 닫고 NULL). 00-overview 의 "정규 파일·링크 1개·소유자 확인" 과 이 문서 이전 판의 "기존이면 잘라 씀" 은 같은 규칙을 가리킨다(검사를 모두 통과한 **뒤에야** 잘라 씀). 실패 사유별 메시지는 따로 없다: 심볼릭 링크(`ELOOP`), 다른 소유자, 하드 링크 2개 이상, FIFO·디렉터리, 권한 없음 등 모든 실패가 같은 `die("cannot create detail dump %s", "디테일 덤프 %s 생성 실패", path)` 하나로 끝난다(errno 문구 없음, stderr, exit 1). 이때까지 stdout 에는 대상 줄·레이아웃 줄이 이미 찍혀 있다.
2. `snprintf (link, 64, "/proc/%d/exe", h->pid)`, `n = readlink (link, exe, sizeof (exe) − 1)` (`exe[4096]` → 최대 4095바이트), `exe[n > 0 ? n : 0] = '\0'`. 실패(−1)나 0 이면 빈 문자열. 4095바이트를 넘는 경로는 조용히 잘린다. 링크 대상에 커널이 붙이는 ` (deleted)` 접미사(실행 파일이 교체·삭제됨)는 **그대로 둔다**. `text_sanitize` 하지 않으며 JSON 이스케이프(`json_fstr`)만 거친다.
3. `uname(&u)`; 실패하면 `u` 를 0 으로(빈 문자열).
4. 아래 순서로 출력(줄바꿈 위치 포함; §5.4).
5. 스레드 수: `os_task_list(h->pid, &tids)` 의 반환(정렬된 task 수, procfs 판독 실패 시 −1), 목록은 즉시 해제.
6. 풀: `sym_pool == 0` 이거나 `pool_dump(f, bias + sym_pool) != 0` 이면 `"pool":null`.
7. `"\n}\n"` 로 닫고, `ferror(f) | (fclose(f) != 0)` 이면 `die("cannot write detail dump %s", "디테일 덤프 %s 쓰기 실패", path)`. (`|` 는 비단락 OR: `fclose` 는 항상 실행된다.) 쓰기 도중의 개별 `fprintf` 실패는 검사하지 않고 이 마지막 판정 하나로 모은다. 실패 시 부분 파일은 지우지 않는다.

### 5.4 문서 형태

출력 순서와 줄 구성(`…` 은 하위 모듈이 쓰는 부분):

```text
{"format":"cub_ctv detail dump","version":2,"replayable":false,
"note":"everything cub_ctv read at this moment; not a replay file - use --record for replay",
"ts":<long>,"host":"<nodename>","kernel":"<release>",
"target":{"pid":<int>,"db":"…","exe":"…","lib":"…","load_bias":"0x…","threads_now":<int>,"symbols":{"csectgl_Critical_sections":"0x…","sync_Stats":"0x…","log_Gl":"0x…","cubthread::Manager":"0x…"}},
"layout":{"stride":<int>,"count":<int>,"off_stats":<int>,"off_rwlock":<int>,"off_tran":<int>,"off_owner":<int>,"off_wq":<int>,"off_pq":<int>},
"degraded":<uint>,"degraded_features":[…],"tran_refused":"…",
"cs":[…],"thread_wait":…,"log_disk":…,
"transactions":{…},
"threads":{…},
"pool_chunk_entries":<int>,"pool":[
…엔트리 줄…
],"pool_truncated_at_chunk":<int>,"pool_walk":"…"
}
```

줄 구성의 정확한 규칙(`fprintf` 단위, 누가 무엇을 쓰는지):

| # | 쓰는 함수 | 바이트 |
|---|---|---|
| 1 | `detail_dump_write` | `{"format":"cub_ctv detail dump","version":2,"replayable":false,\n` |
| 2 | 〃 | `"note":"everything cub_ctv read at this moment; not a replay file - use --record for replay",\n` |
| 3 | 〃 | `"ts":%ld,"host":` + `json_fstr(nodename)` + `,"kernel":` + `json_fstr(release)` + `,\n` |
| 4 | 〃 | `"target":{"pid":%d,"db":` + `json_fstr(h->db)` + `,"exe":` + `json_fstr(exe)` + `,"lib":` + `json_fstr(h->lib)` + `,"load_bias":"0x%llx","threads_now":%d,"symbols":{"csectgl_Critical_sections":"0x%llx","sync_Stats":"0x%llx","log_Gl":"0x%llx","cubthread::Manager":"0x%llx"}},\n` |
| 5 | 〃 | `"layout":{"stride":%d,"count":%d,"off_stats":%d,"off_rwlock":%d,"off_tran":%d,"off_owner":%d,"off_wq":%d,"off_pq":%d},\n` |
| 6 | 〃 | `"degraded":%u,"degraded_features":[` + 항목들 + `],"tran_refused":` + `json_fstr(h->tran_refused)` + `,\n` |
| 7 | `json_cs_rows` (+ 끝에서 부르는 `json_os`) | `"cs":[` + 원소들 + `]` + `,"thread_wait":…` + `,"log_disk":…` (**한 줄, 줄바꿈 없음**). 이어서 `detail_dump_write` 가 `,\n` |
| 8 | `tran_dump` (03 §10) | `"transactions":{…,"slots":[\n` + 슬롯 줄들 + `]}`. 이어서 `detail_dump_write` 가 `,\n` |
| 9 | `thr_dump` (04 §18) | `"threads":{…,"entries":[\n` + 엔트리 줄들 + `]}`. 이어서 `detail_dump_write` 가 `,\n` |
| 10a | `pool_dump` (성공) | `"pool_chunk_entries":%d,"pool":[\n` + 엔트리 줄들 + `],"pool_truncated_at_chunk":%d,"pool_walk":"%s"` |
| 10b | `detail_dump_write` (풀 없음·I-12 실패) | `"pool":null` |
| 11 | `detail_dump_write` | `\n}\n` |

`"threads"` 뒤의 `,\n` 은 `detail_dump_write` 가 풀 분기 **전에** 무조건 쓴다(`pool_dump` 는 앞 구분자를 쓰지 않는다). 따라서 풀이 null 일 때 문서 꼬리는 정확히 `…]},\n"pool":null\n}\n` 이고(`"threads":{…]}` + `,\n` + `"pool":null` + `\n}\n`), `"pool":null` 앞에는 줄바꿈 1개가 있다. 풀이 있을 때 꼬리는 `…}\n],"pool_truncated_at_chunk":-1,"pool_walk":"complete"\n}\n` (마지막 엔트리 줄의 `\n` 뒤에 `]`). 엔트리가 하나도 없으면 `"pool":[\n],"pool_truncated_at_chunk":…`.

`"format"` 키는 반드시 첫 줄에 있어야 한다: `--replay` 는 첫 줄이 `{` 로 시작하고 `DETAIL_MAGIC` 을 포함하면 `die("%s is a detail dump (--dump): it holds everything cub_ctv read but is not a replay file - record with --record", "%s 는 디테일 덤프(--dump)입니다: 판독한 정보를 모두 담지만 재생용이 아닙니다 - 재생하려면 --record 로 기록하십시오")` 로 거부한다.

### 5.5 최상위·target·layout 키

| 키 | 타입 | 출처 |
|---|---|---|
| `format` | string | `DETAIL_MAGIC` = `"cub_ctv detail dump"` |
| `version` | int | 리터럴 2 |
| `replayable` | bool | 리터럴 false |
| `note` | string | 리터럴(위) |
| `ts` | int | `view.ts` (캡처 시각, epoch 초) |
| `host` | string | `uname().nodename` |
| `kernel` | string | `uname().release` |
| `target.pid` | int | `h->pid` |
| `target.db` | string | `h->db` |
| `target.exe` | string | `/proc/<pid>/exe` 링크 대상(§5.3 2단계: 최대 4095바이트, ` (deleted)` 유지, 정화 없음, 실패 시 `""`) |
| `target.lib` | string | `h->lib` = `find_lib_base` 가 돌려준 **maps 의 원래 경로**(01 §9.4). `LIB_OK` 면 가장 낮은 정상 매핑의 경로, `LIB_DELETED` 면 끝의 ` (deleted)` 를 **떼어 낸** 경로. 실제로 판독한 `/proc/<pid>/root<path>`·`/proc/<pid>/map_files/<range>` 가 아니며 ` (deleted)` 를 다시 붙이지도 않는다. 정화 없음(원문), `json_fstr` 이스케이프만 |
| `target.load_bias` | string `"0x%llx"` | `bias` |
| `target.threads_now` | int | `/proc/<pid>/task` 항목 수, 실패 −1 |
| `target.symbols.csectgl_Critical_sections` | string hex | `sym_cs` (바이어스 미포함) |
| `target.symbols.sync_Stats` | string hex | `sym_pool` (없으면 `"0x0"`) |
| `target.symbols.log_Gl` | string hex | `sym_loggl` |
| `target.symbols.cubthread::Manager` | string hex | `sym_thr` (`_ZN9cubthreadL7ManagerE`) |
| `layout.stride` … `layout.off_pq` | int | `h->lo` 의 8개 필드(§1.2), 키 순서 stride, count, off_stats, off_rwlock, off_tran, off_owner, off_wq, off_pq |
| `degraded` | uint | 전역 `degraded` 비트(덤프 시점 값) |
| `degraded_features` | string[] | `deg_names[]` 순서로 켜진 비트마다 `fprintf (f, "%s\"%s (%s)\"", first ? "" : ",", label, id)` — 구분자는 공백 없는 `,`, 문자열은 `json_fstr` 가 아닌 직접 `"…"` (label·id 는 이스케이프가 필요 없는 정적 ASCII). 켜진 비트가 없으면 `[]`. 레이아웃 줄과 달리 `DEG_TRAN` 사유나 `: 65536` 은 붙이지 않는다 |
| `tran_refused` | string | `h->tran_refused` (`VIEW_HDR` 의 `char[64]` 복사본, 트랜잭션 프로브 거부 사유, 없으면 `""`). 01 §1.6 의 전역 `tran_refused` 와 같은 값이다: main 이 `thr_probe` 와 레이아웃 줄 출력 뒤 `VIEW_HDR h` 를 구성할 때 전역을 이 필드로 복사하고(06 §2.10)(63바이트 절단, 사유 문자열은 모두 더 짧음) 그 뒤 전역은 바뀌지 않는다. 재생 경로에서는 덤프가 실행되지 않는다 |

`deg_names[]` (ctv_msg.c) — 배열 순서 = 출력 순서:

| 비트 | id | label | 출력 예 |
|---|---|---|---|
| 0x1 `DEG_TRID` | I-8 | trid not validated | `"trid not validated (I-8)"` |
| 0x2 `DEG_TRAN_CAP` | cap | tran table capped | `"tran table capped (cap)"` |
| 0x20 `DEG_TRAN` | I-6/I-7 | no transaction table | |
| 0x4 `DEG_THREAD` | I-14 | no thread entries | |
| 0x8 `DEG_THR_TRAN` | I-15 | no tran | |
| 0x10 `DEG_THR_ID` | I-16 | no owner id | |
| 0x40 `DEG_THR_TID` | I-17 | no os tid | |
| 0x80 `DEG_THR_RESUME` | I-18 | no wait reason | |
| 0x100 `DEG_THR_NEXT` | I-19 | no wait chain | |
| 0x200 `DEG_THR_STATUS` | I-20 | no thread state | |

문자열 값은(`degraded_features` 항목·`format`·`note`·hex 문자열 같은 고정 리터럴을 제외하고) 모두 `json_fstr` (ctv_json.c, 정본 06 §9.1) 로 쓴다. 알고리즘(코드 그대로):

1. `fputc('"')`.
2. `*s != '\0'` 인 동안, `c = (unsigned char) *s`, `len = utf8_valid_len (s)` (01 §2.3):
   1. `c == '"'` 또는 `c == '\\'` → `\` 와 `c` 두 바이트, `s++`.
   2. 아니면 `c < 0x20` → `fprintf ("\\u%04x", c)` (예: 0x0A → `\u000a`, 소문자 16진 4자리), `s++`. `\n`·`\t` 같은 짧은 이스케이프는 쓰지 않는다.
   3. 아니면 `len == 0` (무효 선두, 고립 연속 바이트, 잘린 시퀀스) → **ASCII 6바이트 `\ufffd`** (백슬래시 + `ufffd`; 3바이트 U+FFFD 원문이 아님), `s++` — 잘못된 **바이트마다 1개**(시퀀스당 1개가 아님; 다음 바이트는 다시 판정).
   4. 아니면 `len` 바이트를 그대로 `fwrite`, `s += len`.
3. `fputc('"')`.

| 입력 | 출력 |
|---|---|
| `/` | 그대로(이스케이프 없음) |
| DEL 0x7F | 그대로 1바이트(`len == 1`) |
| C1 U+0080..U+009F (0xC2 0x80..0x9F) | 그대로 2바이트(유효 UTF-8 이므로) |
| U+2028/U+2029 등 | 그대로 |
| 0xFF, 0xC0 0x80 같은 깨진 바이트 | 바이트마다 `\ufffd` |

`json_fstr` 은 `text_sanitize` 를 먼저 하지 않는다(값을 바꾸지 않고 이스케이프만). 덤프 값 중 이미 정화된 것은 `db`(server_by_pid)·스레드 `comm`(os_task_comm) 뿐이고, CS·풀 이름은 `mem_cstr` 가, 신원 문자열은 03 §6.3 판독 규칙이 인쇄 가능 텍스트만 통과시키므로, 위 표의 제어 문자·깨진 바이트 경로는 `host`·`kernel`·`exe`·`lib` 에서만 실제로 생길 수 있다. 인자가 NULL 이면 역참조로 죽는다(덤프의 호출은 모두 배열 또는 비NULL).

### 5.6 `cs` 배열 (raw 모드)

`json_cs_rows(f, &h->lo, view.rows, NULL, NULL, 0, NULL, 0.0, 1)` — `raw = 1`, 표본 없음(정본 06 §9.3).

배치: `"cs":[` 를 쓴 뒤 `i = 0..count−1` 원소를 `fprintf ("%s{\"ix\":%d,…", i ? "," : "", i)` 로 잇는다. 원소 구분자는 `,` 하나이며 **줄바꿈·공백이 전혀 없다**(cs 배열 전체와 뒤의 `thread_wait`·`log_disk` 가 한 줄). 원소는 `}` 로 닫고, 배열은 `]` 로 닫는다. 아래 표의 행 순서가 **원소 안 키의 출력 순서**이며 조건부 키는 조건이 맞을 때만 그 자리에 들어간다. raw 덤프에서 원소 키 순서는 정확히:

`ix, name, rwlock, waitR, waitW, tran, thr, owner_tid, wq_head, has_stats, [owner], wq, [nenter, nwait, nreenter, total_ms, max_ms]`

(raw 에서는 `owner` 시스템 형, `wq_seen`, `occ_pct`·`wait_pct`·`enter_s`·`wait_s` 가 나오지 않는다.) 원소별 서식: `{"ix":%d,"name":<json_fstr>,"rwlock":%d,"waitR":%u,"waitW":%u,"tran":%d,"thr":%d,"owner_tid":"0x%llx","wq_head":"0x%llx","has_stats":%d` … `}`.

원소마다(`ix` 순):

| 키 | 타입 | 출처 / 조건 |
|---|---|---|
| `ix` | int | CS 인덱스 |
| `name` | string | `CS_ROW.name` |
| `rwlock` | int | `CS_ROW.rwlock` |
| `waitR` | uint | `wr` |
| `waitW` | uint | `ww` |
| `tran` | int | raw: `CS_ROW.tran` 원시값(writer 가 아니어도 남은 값) |
| `thr` | int | raw: `thr_index_of_tid_mem(owner)` 슬롯 → `slot + 1`, 못 찾으면 −1 (owner 원시값 기준) |
| `owner_tid` | string `"0x%llx"` | raw 전용: `CS_ROW.owner` |
| `wq_head` | string `"0x%llx"` | raw 전용: `CS_ROW.wq` (첫 큐 머리, §3.5) |
| `has_stats` | int 0/1 | raw 전용 |
| `owner` | object | `row_tran(r) = (rwlock < 0 ? tran : −1)` 가 ≥ 1 이고 `view_identity(row_tran, &id, NULL) == 0` (프레임에 그 tran 의 신원이 있음, 05 §5)일 때만: `,"owner":{"tran":%d` + `json_ident(f, &id, 0)` + `}` (raw 에서는 `system` 형 owner 객체는 나오지 않음) |
| `wq` | object[] | `chain_of(wq, −1, …)` = 프레임에 캡처된 큐 머리 `wq` 의 체인(promoter→writer 연속): 원소 `{"thr":<slot+1>,"tran":<int>}`. raw 는 창에서 본 체인(`wq_seen`)을 쓰지 않는다. **키는 항상 나온다**: 체인이 비면 `,"wq":[]` |
| `nenter`, `nwait`, `nreenter`, `total_ms`, `max_ms` | uint / %.3f | `has_stats` 일 때만 |

`owner` 객체 세부(`json_ident(f, &id, with_info = 0)`, 정본 03 §10 끝·06 §9.3):

| 키(순서) | 타입 | 규칙 |
|---|---|---|
| `tran` | number `%d` | `row_tran(r)` |
| `user`, `program`, `login`, `host`, `client_ip` | string | `TRAN_IDENT` 의 `user`, `program`, `login`, `host`, `ip` 를 `json_fstr`. **빈 값이어도 키를 빼지 않고 `""` 로 쓴다** |
| `cpid` | **number** `%d` | `TRAN_IDENT.cpid` (문자열 아님) |

`info` 키는 이 객체에 없다(`with_info = 0`; 트랜잭션 덤프의 슬롯에만 있음). 예(fixture 의 `--json`, 같은 함수의 비 raw 경로): `"owner":{"tran":22,"user":"DBA","program":"csql","login":"cubrid","host":"dbhost01","client_ip":"10.0.0.79","cpid":3214275}`.

`wq` 체인 순회 규칙(정본: 05 §4.1 `view_add_chain`·§5 `view_waiters`, 04 §16.4–16.5):

| 단계 | 규칙 |
|---|---|
| 캡처 | `view_capture` 가 CS `i` 순서로 `view_add_chain(rows[i].wq, rows[i].wq2, NULL, i)`. 같은 머리 주소의 체인이 이미 있으면 다시 걷지 않는다(먼저 나온 CS 의 걷기를 공유). 체인 수 상한 128 (`VIEW_MAX_CHAINS`), 넘치면 그 머리는 체인 없음 |
| 걷기 | 스냅샷은 `w == NULL` 이므로 항상 `thr_waiters_mem2(head, head2, slot, tran, 64, 0)`: promoter 큐를 걷고, 자리가 남으면 writer 큐를 이어 걷는다. 홉 상한 64 (`THR_WAITERS_MAX`), 모든 홉은 엔트리 배열 안이어야 하며 이미 방문한 슬롯을 다시 만나면 거기서 멈춘다(순환 = 찢긴 판독). 두 번째 큐에서 첫 부분의 슬롯을 만나면 거기서 자른다(중복 제거) |
| 조회 | `chain_of(r->wq, −1, widx, wt, NULL)`(홉 cpid 는 쓰지 않음) → `view_waiters(wq)` 가 머리 주소가 같은 체인의 슬롯을 최대 64개 복사. 머리 0 또는 일치 체인 없음 → 0개 → `"wq":[]` |
| 원소 | `{"thr":%d,"tran":%d}` = `THR_INDEX(widx[k])` (= 슬롯+1), `view_tran_of(widx[k])` (걷기에서 그 홉과 같은 판독으로 읽은 tran, 05 §5). 구분자 `,`, 공백·줄바꿈 없음 |

`cs` 배열 뒤에 `json_os` (ctv_json.c) 가 같은 객체 수준에 두 키를 붙인다(앞에 `,`, 줄바꿈 없음):

| 키 | 값 |
|---|---|
| `thread_wait` | `view.wait_avail` 이 0 이면 `null`, 아니면 `{"live":%d,"counts":{` + 항목들 + `}}`. 항목은 `k = 0..27` **오름차순**으로 `wait_cnt[k] > 0` 인 것만 `"%d":%d` (키는 10진 문자열, 구분자 `,`, 공백 없음). 모두 0 이면 `"counts":{}` |
| `log_disk` | `view.disk_avail` 이 0 이면 `null`, 아니면 `{"dev":` + `json_fstr(view.disk_dev)` + `,"await_ms":%.3f,"util_pct":%.1f}` |

`thread_wait` 값의 출처(정본 04 §17 `thr_wait_counts`, 05 §4.1 12단계):

| 항목 | 의미 |
|---|---|
| `wait_avail` | `view_capture` 가 **스냅샷에서도** `thr_wait_counts(view.wait_cnt, &view.wait_live, readers) == 0` 으로 정한다(실시간 gather 판독). 스레드 엔트리가 있고 `resume_status` 필드(I-18)가 검증되어 있으며 gather 할당·판독이 성공하면 1. `DEG_THREAD`·`DEG_THR_RESUME` 이거나 집계 중 resume 이 강등되면 0 → `null`. 즉 덤프에서 `thread_wait` 는 정상 대상이면 **대개 객체**다 |
| `live` | 스레드 엔트리 중 `thr_ids[k] != 0` (살아 있는 m_id) 이고 `resume_status` 판독 성공(`m_status` 검증 시 그 판독도 성공)이며 값이 0..27 인 엔트리 수 |
| `counts["k"]` (k ≠ 7) | 위 `live` 엔트리 중 값이 `k` 이고 현재 중단 상태인 것의 수(`m_status` 검증 시 `TS_WAIT`(3) 이고 중단 코드, 미검증 시 중단 코드) |
| `counts["7"]` | 스레드 값으로 세지 않고 `readers = Σ rows[i].wr` (CS 배열 전체 waitR 합)로 덮어쓴 값. 0 이면 키가 없다 |

`log_disk`: 디스크 수치는 구간 시작·끝 diskstats 차이로만 만들어지므로(05 §4.5 `view_disk_end` 가 `w == NULL` 이면 즉시 `disk_avail = 0`) **스냅샷·덤프에서는 항상 `null`** 이다.

### 5.7 `transactions` 객체 (`tran_dump`, ctv_tran.c)

| 키 | 타입 | 출처 |
|---|---|---|
| `available` | bool | `tran_geom.all_tdes != 0` |
| `all_tdes` | hex string | TDES 포인터 배열 주소 |
| `ntran` | int | 슬롯 수 |
| `off_tran_index` | int | TDES 내 tran_index 오프셋 |
| `cid_off` | int | CLIENTIDS 문자열 구간 오프셋 |
| `cid_abi` | string | `"cow"` / `"sso"` / `"not-locked"` |
| `tdes_scan_bytes` | int | `max_scan` |
| `tdes_scan_is_spacing` | bool | `scan_measured` |
| `has_trid` | int | I-8 통과 |
| `slots` | object[] | 슬롯 0..ntran−1, 한 줄에 하나. 판독 실패: `{"idx","state":"unreadable"}`. 그 밖: `{"idx","tdes":hex,"tran_index","trid","state"}` + 신원이 읽히면 `info,user,program,login,host,client_ip,cpid`. `state` = idx 0 → `"system"`, has_trid 이고 trid == −1 → `"released"`, 신원 내용 있음 → `"client"`, 그 밖 `"empty"` |

### 5.8 `threads` 객체 (`thr_dump`, ctv_thr.c)

| 키 | 타입 | 출처 |
|---|---|---|
| `available` | bool | 엔트리 이미지가 있음(보존 이미지 또는 새 판독) |
| `base`, `stride`, `count`, `off_index`, `off_id`, `has_tran`, `has_id`, `has_tid`, `off_tid`, `has_resume`, `off_resume`, `has_next`, `off_next`, `off_tran`, `has_status`, `off_status` | hex string(base) / int | `thr_geom` |
| `entries` | object[] | tid·tran·next 가 모두 0/−1/0 인 슬롯은 생략. 원소 `{"slot","index","tid":hex,"tran","next_wait":hex,"next_wait_index"}` + (OS tid 확인 시) `"os_tid","comm"` + (has_resume) `"resume_status"` |

(트랜잭션·스레드 객체는 각 모듈 명세가 정본이며 여기서는 덤프 문서의 구성 요소로만 적는다: `tran_dump` = 03 §10, `thr_dump` = 04 §18. 두 절 모두 키 순서·서식 문자열을 코드 그대로 담고 있다.)

줄 구성(두 함수 공통 형태, 코드 확인):

| 부분 | `transactions` (`tran_dump`) | `threads` (`thr_dump`) |
|---|---|---|
| 머리 | 한 줄: `"transactions":{"available":%s,"all_tdes":"0x%llx","ntran":%d,"off_tran_index":%d,"cid_off":%d,"cid_abi":"%s","tdes_scan_bytes":%d,"tdes_scan_is_spacing":%s,"has_trid":%d,"slots":[` + `\n` | 한 줄: `"threads":{"available":%s,"base":"0x%llx","stride":%d,"count":%d,"off_index":%d,"off_id":%d,"has_tran":%d,"has_id":%d,"has_tid":%d,"off_tid":%d,"has_resume":%d,"off_resume":%d,"has_next":%d,"off_next":%d,"off_tran":%d,"has_status":%d,"off_status":%d,"entries":[` + `\n` |
| 원소 줄 | 슬롯 t 마다 `(t ? "," : "")` + `{…}` + `\n` — 구분자 `,` 는 **다음 줄의 맨 앞**에 붙는다(`t` 기준이므로 슬롯 0 이 unreadable 이어도 같은 규칙) | 출력하는 엔트리마다 `(first ? "" : ",")` + `{…}` + `\n` (생략된 엔트리는 구분자 판단에 들어가지 않음) |
| 꼬리 | `]}` (줄바꿈 없음; 뒤의 `,\n` 은 `detail_dump_write`) | `]}` (같음) |
| `available:false` | `all_tdes == 0`: 머리의 **모든 키를 그대로** 쓰고(값은 0/`"0x0"`/`"not-locked"` 등 현재 `tran_geom` 값), 슬롯 줄 없이 `"slots":[\n]}` | 이미지 없음: 머리의 모든 키를 현재 `thr_geom` 값으로 쓰고 `"entries":[\n]}` |

예: 트랜잭션 표가 없을 때 이 두 부분은 `"transactions":{"available":false,…,"slots":[\n]},\n"threads":{…,"entries":[\n` … 형태가 된다.

### 5.9 `pool` 부분

§4.7. I-12 는 통과했으나 `pool_walk` 의 버퍼 할당이 실패하면 null 이 아니라 `"pool_chunk_entries":N,"pool":[\n],"pool_truncated_at_chunk":-1,"pool_walk":"no_memory"` 이고 `view.pool_state = 1`(화면은 `(pool not grouped: out of memory in cub_ctv)` 줄). 풀이 없거나 I-12 실패면 `"pool":null` 만 쓰고 `pool_chunk_entries`·`pool_truncated_at_chunk`·`pool_walk` 키는 없다. 이 경우 `view.pool_state` 는 심볼 없음이면 0(화면: `[pool] not collected …`), 검증 실패면 −1(화면: 경고).

---

## 6. 모듈 간 흐름 요약

| 단계 | 호출 | 결과 |
|---|---|---|
| 시작 | `cs_layout_probe(bias+sym_cs, &lo)` | `CS_LAYOUT` 또는 die |
| 스냅샷 | `cs_read_rows` → `view_capture` | `view.rows` |
| 라이브 시작 | `cs_read_rows(cs_base, lo, w->prev)` | 창 시작 행 |
| 라이브 표본(`--hz`, 기본 100 Hz) | `mem_read(cs_base, blob, stride*count)` → `cs_decode_raw` (점유·대기 집계), 대기가 있으면 `cs_wait_queues(…, &head2, &field)` | `LIVE_WIN` |
| 창 마감 | `cs_rows_from_blob(w->blob, lo, w->cur)` → `view_capture` | 창 끝 행 |
| 풀(스냅샷) | `view_capture_pool` → `pool_collect` | `view.pool*` |
| 덤프 | `detail_dump_write` → `pool_dump` | 파일 + `view.pool*` |
| 표시 | `state_str`, `print_view_pool` → `pool_print_groups` | 텍스트 |

표본 주기: `OPTS.hz` 초기값 100 (06 §2.2), 허용 1..10000, `interval * hz >= 1` 이어야 함(06 §2.6). 라이브 루프의 틱 간격은 `1.0 / h->hz` 초(ctv_live.c `r.period`, 06 §12). 100 Hz 는 기본값일 뿐 고정이 아니다.

---

## 7. 실패 동작 총괄

| 상황 | 모듈 | 동작 |
|---|---|---|
| CS 프로브 거부 | ctv_cs.c → main | 사유 포함 die, 종료 |
| 행 판독 실패(`cs_read_rows` −1) | main / live | `die_target_gone()` (ctv_live.c) = `die ("target read failed (server gone?)", "대상 판독 실패(서버 종료?)", NULL)` → stderr `cub_ctv: target read failed (server gone?)\n` / `cub_ctv: 대상 판독 실패(서버 종료?)\n`, exit 1 (06 §2.11) |
| stats 블록 판독·type 실패 | ctv_cs.c | 그 행 `has_stats = 0`, 계속 |
| 이름 판독 실패 | ctv_cs.c / ctv_pool.c | `"?"` |
| I-12 실패 | ctv_pool.c | 수집 −1(경고, 풀 뷰 생략) / 덤프 `"pool":null` |
| 체인 중단 | ctv_pool.c | 그때까지 묶은 그룹 유지, 중단 사유 한 줄 |
| 덤프 파일 생성·쓰기 실패 | ctv_dump.c | die |

---

## 8. 재구현 시 지켜야 할 세부

| 항목 | 규칙 |
|---|---|
| 후보 순서 | stride 는 오름차순 첫 일치, stats 슬롯은 전수 후 유일성, 청크 N 은 256→128→64→512 첫 통과 |
| I-3 비교 대상 | 이름 문자열이 아니라 **포인터 값** 동일성 |
| I-4 사유 | 재시도 3회 중 마지막 시도의 사유를 반환; 재판독 실패는 즉시 `"I-0 cs array read"` |
| I-4 즉시 거부 | 대기수·큐 포인터 위반은 CS 하나만 어긋나도 그 시도 거부(1회 허용은 writer-owner 규칙에만) |
| 풀 첫 청크 | 프로브가 읽은 이미지를 순회 첫 홉에 재사용(주소가 같을 때) |
| 풀 해시 | FNV-1a 64: type 4 B 를 먼저, 이어 이름 바이트(NUL 제외); 하위 12비트, 선형 탐사 |
| 덤프 엔트리 구분자 | 첫 유효 엔트리를 뺀 나머지 앞에 `,`; 각 엔트리 끝에 `\n` |
| `pool_truncated_at_chunk` | 센티널(음수)은 −1 로 쓰고 사유는 `pool_walk` 단어로 |
| 덤프 숫자 형식 | ms 값은 `%.3f`, 주소는 `"0x%llx"` 문자열 |

---

