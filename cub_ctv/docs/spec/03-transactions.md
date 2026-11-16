# 트랜잭션 테이블 명세 (Transaction Table Specification)

대상 모듈: `src/ctv_tran.c` (+ `src/ctv.h` 의 `TRAN_GEOM`·`TRAN_IDENT`·관련 상수).
불변식 범위: I-6 ~ I-11.
이 문서만으로 같은 동작을 재구현할 수 있도록 상수값·오프셋·판정 순서를 코드 그대로 적는다. 소스와 다르면 소스가 정본이다.

---

## 1. 책임 (Responsibility)

| 항목 | 내용 |
|---|---|
| 정의 | 실행 중인 `cub_server` 메모리에서 `log_Gl.trantable.all_tdes` 를 찾아(TDES 포인터 배열), 각 트랜잭션 디스크립터(LOG_TDES)의 `tran_index`·`trid`·`CLIENTIDS`(클라이언트 신원 문자열 6개 + pid) 위치를 **심볼·헤더 없이 값 패턴으로 프로브**하고, 구간(window)마다 슬롯별 신원을 읽어 캐시한다. |
| 입력 | `log_Gl` 의 런타임 주소(호출자가 ELF 심볼 + 로드 bias 로 계산) |
| 출력 | 전역 `tran_geom`(기하), 슬롯별 `TRAN_IDENT`+trid 질의 API, JSON 덤프 |
| 접근 방식 | 읽기 전용. 단건은 `mem_read`(`/proc/<pid>/mem` pread), 묶음은 `mem_gather_field`/`mem_gather_ok`(`process_vm_readv`, 불가 시 pread). 대상 프로세스에 쓰기 없음. |
| 하지 않는 것 | 표시(행 구성, 텍스트 정렬)·기록/재생·강등 비트 이름 출력. 이들은 `ctv_frame.c`·`ctv_rows.c`·`ctv_json.c`·`cub_ctv.c` 가 담당. |

### 1.1 외부 의존 함수 계약 (이 모듈이 전제하는 동작)

| 함수 (정의 위치) | 계약 |
|---|---|
| `int mem_read(addr, buf, n)` (ctv_mem.c) | pread 가 정확히 n 바이트를 돌려주면 0, 아니면 -1. n=0 이면 0. |
| `int mem_read_ptr(addr, *p)` | 8바이트 읽기 성공 ∧ `VALID_PTR(*p)` 이면 0, 아니면 -1. |
| `size_t mem_read_shrink(addr, buf, max, min)` | n = max 부터 `n >= min && n > 0` 동안 n 을 절반으로 줄이며 `mem_read(addr, buf, n)` 시도. 첫 성공 n 반환, 전부 실패하면 0. **max < min 이면 시도 없이 0.** |
| `int mem_gather_ok(addrs, n, each, buf, ok)` | buf 전체(n×each)와 ok[n] 을 0 으로 채운 뒤, 주소 0 이 아닌 범위를 IOV_MAX 개씩 `process_vm_readv` 로 읽음. 실패 지점의 범위는 pread 단건으로 재시도하고 그 뒤부터 배치 재개. 읽힌 범위만 `ok[i]=1`, 못 읽은 범위는 0 으로 남음. 항상 0 반환. ENOSYS/EPERM 이면 이후 전부 pread. |
| `int mem_gather_field(bases, n, off, width, skip, out, ok)` | `addrs[i] = VALID_PTR(bases[i]) && (skip==NULL || !skip[i]) ? bases[i]+off : 0` 로 만든 뒤 `mem_gather_ok`. 내부 static 주소 버퍼 할당 실패 시에만 -1. |
| `int is_printable_text(s)` (ctv_text.c) | §6.1 |
| `size_t utf8_cut(s, ln)` | §6.2 |
| `int every(double *last, double now, double gap)` (ctv_sched.c) | `now - *last < gap` 이면 0. 아니면 `*last = now`, 1. |
| `void deg_set(bits, on)` (ctv_msg.c) | 전역 `degraded` 비트 설정/해제. |
| `void json_ident(f, id, with_info)` (ctv_json.c) | §10 |

### 1.2 공통 매크로

| 매크로 | 값 / 정의 |
|---|---|
| `VALID_PTR(p)` | `(p) > 0x1000 && (p) < 0x800000000000ULL` (x86-64 사용자 공간) |
| `REFUSE(id)` | `return (id)` — 거부 사유 문자열을 그대로 반환 |
| `rd_u64(buf, off)` / `rd_i64` / `rd_i32` | buf+off 에서 리틀엔디언 정수 (memcpy) |

---

## 2. 상수 (Constants, `ctv.h` / `ctv_tran.c`)

| 상수 | 값 | 용도 |
|---|---|---|
| `LOG_GL_SCAN_MAX` | 65536 | I-6: `log_Gl` 에서 all_tdes 포인터를 찾을 때 읽는 최대 바이트 |
| (I-6 축소 하한) | 4096 | `mem_read_shrink(log_gl, …, 65536, 4096)` 의 min |
| `TDES_HEAD_SCAN` | 512 | I-6: 디스크립터마다 `tran_index` 를 찾는 머리 바이트 수 |
| `CS_MIN_SAMPLES` | 4 | I-6: 수열 `tran_index == k` 를 확인하는 디스크립터 수(k=0..3) |
| `TRAN_SLOTS_MAX` | 65536 | 디스크립터 개수 상한(sanity cap) |
| (tran_count CHUNK) | 256 | 개수 세기 1회 gather 단위 |
| `TDES_SCAN_MAX` | 32768 | I-7: 간격이 이 이하일 때만 스캔 상한으로 채택 |
| `TDES_SCAN_FALLBACK` | 8192 | I-7: 간격이 무효일 때 스캔 상한 |
| (lock 축소 하한) | 512 | `mem_read_shrink(tdes, img, max_scan, 512)` 의 min |
| `NWITNESS` | 3 | I-10: 증인 디스크립터 수 |
| `LAZY_LOCK_BUDGET` | 16 | 지연 재시도 1회당 디스크립터 예산(trid 없는 경로) |
| `LAZY_LOCK_TRIES` | 4 | 지연 재시도 1회당 시도할 신규 트랜잭션 수(trid 경로) |
| `LAZY_RETRY_S` | 10 | 실패한 디스크립터(같은 trid)를 다시 시도하기까지의 초 |
| `IDENT_LEN` | 64 | 신원 문자열 1개 버퍼(최대 63바이트 + NUL) |
| `IDENT_CACHE_TTL` | 3 | 신원 캐시 히트 허용 횟수(§8.4) |
| `REP_CACHE_MAX` | 64 | COW `_Rep` 해독 캐시 엔트리 수 |
| `COW_STRIDE` | 8 | 구 ABI std::string 객체 크기(char* 하나) |
| `SSO_STRIDE` | 32 | 신 ABI std::string 객체 크기 |
| `COW_REP_LEN` | 24 | COW `_Rep{len, cap, refcount}` 헤더 길이(데이터 앞) |
| `SSO_LOCAL_OFF` | 16 | SSO 로컬 버퍼 오프셋 |
| `SSO_LOCAL_MAX` | 15 | SSO 로컬 버퍼에 들어가는 최대 길이 |
| `STR_LEN_MAX` | 255 | 문자열 길이 상한 |
| `STR_CAP_MAX` | 4096 | COW capacity 상한 |
| `STR_REF_MAX` | 65536 | COW refcount 상한(배타) |
| `CLIENTIDS_NSTR` | 6 | 문자열 개수 |
| `CLIENTIDS_BYTES(s)` | `s*6 + 4` | CLIENTIDS 구간 길이: COW 52, SSO 196 |
| `PID_MAX_LIMIT` | 4194304 (2²²) | pid 상한(배타) |
| `DEG_TRID` | 0x1 | I-8 실패 강등 비트 |
| `DEG_TRAN_CAP` | 0x2 | 디스크립터 수가 `TRAN_SLOTS_MAX` 에 도달(정확히 65536 개 연속 확인 포함, §5.2) |
| `DEG_TRAN` | 0x20 | I-6/I-7 거부(호출자가 설정) |
| `NULL_TRANID` | -1 (코드에는 리터럴 `-1`) | 해제 슬롯의 trid |

---

## 3. 소유 상태 (Owned State)

### 3.1 `TRAN_GEOM tran_geom` (전역, `ctv.h` 선언, 0 초기화)

| 필드 | 타입 | 의미 | 설정 시점 |
|---|---|---|---|
| `all_tdes` | uint64_t | TDES 포인터 배열 주소. **0 = 트랜잭션 표 사용 불가** | I-6 성공 시. I-6/I-7 거부 시 0 으로 되돌림 |
| `off_ti` | int | TDES 안 `tran_index`(int32) 오프셋. trid 는 `off_ti + 4` | I-6 |
| `cid_off` | int | TDES 안 CLIENTIDS 구간 시작 오프셋 | CLIENTIDS 잠금(§7) |
| `cid_stride` | int | `COW_STRIDE`(8) / `SSO_STRIDE`(32). **0 = 아직 잠기지 않음** | CLIENTIDS 잠금 |
| `max_scan` | int | 디스크립터 1개에서 CLIENTIDS 를 찾는 스캔 바이트 | I-7 |
| `scan_measured` | int | 1 = `max_scan` 이 실측 간격, 0 = fallback | I-7 |
| `has_trid` | int | 1 = I-8 성립, `off_ti+4` 의 trid 를 신뢰 | I-8 |
| `ntran` | int | 디스크립터 개수(부팅 후 고정으로 간주) | `tran_count()` |

잠금 후에는 `cid_off`/`cid_stride` 가 프로세스 수명 동안 바뀌지 않는다(재프로브 없음).

### 3.2 `TRAN_IDENT`

```c
typedef struct {
  char info[64], user[64], program[64], login[64], host[64], ip[64];
  int cpid;
} TRAN_IDENT;
```

필드 순서 = CLIENTIDS 문자열 순서 k=0..5: `client_info`, `db_user`, `program_name`, `login_name`, `host_name`, `client_ip_addr`, 그리고 pid.

### 3.3 파일 static

| 이름 | 크기 | 수명 / 갱신 |
|---|---|---|
| `tdes_ptrs` (uint64_t*) | `ntran × 8` | `tdes_ptrs_load` 가 최초 할당(그때의 ntran). `ident_cache_setup` 재구성 시 free→NULL. 구간마다 덮어씀 |
| `ptrs_n` | int | `tdes_ptrs` 유효 엔트리 수. load 시작 시 0, 성공 시 ntran |
| `span_img` | `ntran × span_len` | 구간별 CLIENTIDS 구간 이미지. prefetch 에서 할당 |
| `span_ok` | `ntran` | 슬롯 구간이 이번 gather 에서 읽혔는가 |
| `span_free` | `ntran` | 슬롯이 이번 구간에 해제(trid -1)로 읽혔는가 |
| `prefetch_n` | int | `span_img` 유효 슬롯 수. 0 = 프리페치 없음(전부 개별 판독) |
| `rep_cache[64]` | `{uint64_t p; int rc; char s[64];}` × 64 | COW 해독 캐시(§8.5) |
| `rep_cache_n` | int | 사용 엔트리 수 |
| `ident_cache` (IDENT_CACHE*) | `ntran` 엔트리 | §8.4 |
| `ident_cache_n` | int | 엔트리 수(0 = 미구성) |
| `span_lo`, `span_len` | int | 캐시 키 구간: `cid_off`, `CLIENTIDS_BYTES(cid_stride)` |
| `span_trid` | `ntran × 4` | 이번 구간 슬롯별 trid |
| `span_trid_ok` | `ntran` | trid 판독 성공 여부 |
| `last_try` (double) | — | 마지막 CLIENTIDS 잠금 시도 시각(초). `tran_lock_clientids` 와 `tran_geom_refresh` 가 공유 |
| `tran_lock_clientids` 내부 static | `cursor`(int), `tcursor`(int), `tried`(int32×ntran, calloc), `tried_at`(time_t×ntran, calloc), `trid`(int32×ntran, malloc) | 최초 사용 시 할당, 해제하지 않음 |

`IDENT_CACHE` 엔트리:

| 필드 | 의미 |
|---|---|
| `uint64_t tdes` | 마지막 해독 때의 디스크립터 주소. 0 = 빈 엔트리 |
| `unsigned char *key` | 키 바이트 `4 + span_len`(COW 56, SSO 200) — 최초 미스 때 malloc |
| `int age` | 마지막 해독 이후 히트 횟수 |
| `int rc` | 마지막 해독 결과(0 / -1). **실패도 캐시됨** |
| `int32_t trid` | 마지막 해독 때의 trid |
| `TRAN_IDENT id` | 마지막 해독 결과 신원 |

---

## 4. 대상 메모리 레이아웃 (Target Layouts)

```
log_Gl (전역 LOG_GLOBAL, 주소 = bias + 심볼값)
 └─ [log_Gl + X] (8정렬, X < 읽힌 바이트 ≤ 65536) : uint64 all_tdes    ← I-6 이 X 를 찾음
      all_tdes ─► uint64 tdes_ptr[0 .. ntran-1]     (TDES 포인터 배열)
                     │
                     ▼
                  LOG_TDES (디스크립터 k)
                   +off_ti     : int32 tran_index (== k)          ← I-6 (off_ti < 512, 4정렬)
                   +off_ti+4   : int32 trid (0 시스템 / -1 해제 / >0 실 트랜잭션)  ← I-8
                   +cid_off    : std::string × 6 (stride 8 또는 32)  ← I-9/I-10 (8정렬)
                   +cid_off+6×stride : int32 client pid
```

### 4.1 CLIENTIDS 구간

| 항목 | COW (구 ABI) | SSO (신 ABI) |
|---|---|---|
| 문자열 k 객체 주소 | `tdes + cid_off + 8k` | `tdes + cid_off + 32k` |
| pid 주소 | `tdes + cid_off + 48` | `tdes + cid_off + 192` |
| 구간 길이 `CLIENTIDS_BYTES` | 52 | 196 |

### 4.2 구 ABI (COW) std::string

```
객체(8B):  [ char *p ]  ──►  힙:  p-24: int64 len | p-16: int64 cap | p-8: int32 refcount | (pad 4)
                                   p   : char data[len] ...
```

| 필드 | 위치 | 허용 범위 |
|---|---|---|
| `p` | 객체 +0 | `VALID_PTR(p)` ∧ `p ≥ 24` |
| `len` | `p - 24` (int64) | 0 ≤ len ≤ 255 |
| `cap` | `p - 16` (int64) | len ≤ cap ≤ 4096 |
| `ref` | `p - 8` (int32) | -1 ≤ ref < 65536 |
| 데이터 | `p` | len 바이트(최대 63 복사) |

`_Rep` 판독은 `p - 24` 에서 20바이트 1회.

### 4.3 신 ABI (SSO) std::string

```
객체(32B): +0 char *p | +8 uint64 size | +16 char local[16]  (또는 +16 capacity)
```

| 조건 | 데이터 위치 |
|---|---|
| `p == 객체주소 + 16` | 로컬: `size ≤ 15` 이어야 함, 객체 이미지 +16 에서 복사 |
| 그 외 | 힙: `p` 에서 `min(size, 63)` 바이트 판독 |

공통 조건: `size ≤ 255` ∧ `VALID_PTR(p)`.

---

## 5. 불변식 (Invariants I-6 ~ I-11)

| ID | 규칙 | 실패 시 |
|---|---|---|
| I-6 | `log_Gl` 의 8정렬 오프셋 X 에 있는 포인터 P 에 대해, P 가 가리키는 배열의 첫 4개 포인터 t₀..t₃ 이 모두 유효하고, 4정렬 오프셋 `ti` (0 ≤ ti ≤ 508) 에서 `*(int32*)(t_k + ti) == k` (k=0..3). 첫 (X, ti) 를 채택 | `tran_probe` 가 사유 반환, `all_tdes=0` → 호출자가 `DEG_TRAN` |
| I-7 | `gap = |tdes_ptr[1] - tdes_ptr[0]|` 이 `0 < gap ≤ 32768` 이면 `max_scan = gap`, 아니면 `max_scan = 8192` | 포인터 0/1 판독 실패만 거부("I-7 descriptor pointers"). 범위 밖은 거부 아님(fallback) |
| I-8 | `trid(slot 0) == 0` ∧ k=1..min(3, ntran-1) 모두 `trid(slot k) != 0` (판독 성공 포함) | `has_trid=0`, `DEG_TRID` 설정 — trid 열 `-`, 해제 슬롯 필터 OFF |
| I-9 | CLIENTIDS 후보: 문자열 6개가 해당 ABI 로 해독되고 인쇄가능(§6.1), pid ∈ [0, 2²²). strict 는 6개 중 하나 이상 비어있지 않을 것 추가 | 후보 기각 |
| I-10 | 후보 기하를 증인 ≤3 개에 relaxed 로 적용, `fails ≥ 2 ∧ fails > content` 일 때만 기각 | 후보 기각 |
| I-11 | `has_trid` 일 때 trid == -1 인 슬롯은 클라이언트 아님 | 신원 없음(-1), 행 숨김 |

### 5.1 I-6 — `tran_probe_all_tdes(log_gl)` 

반환: `const char*` 거부 사유, NULL = 성공.

1. `blob = malloc(65536)`. 실패 → `"I-6 out of memory"`.
2. `scan = mem_read_shrink(log_gl, blob, 65536, 4096)` (65536→32768→16384→8192→4096). 0 → free, `"I-6 log_Gl unreadable"`.
3. `off = 0, 8, 16, …` (`off + 8 ≤ scan`) 마다:
   1. `p = rd_u64(blob, off)`. `!VALID_PTR(p)` 이면 다음.
   2. `mem_read(p, tds, 32)` (포인터 4개). 실패 → 다음.
   3. k=0..3: `VALID_PTR(tds[k])` ∧ `mem_read(tds[k], heads[k], 512) == 0`. 하나라도 실패 → 다음 off.
   4. `ti = 0, 4, …, 508`: k=0..3 모두 `rd_i32(heads[k], ti) == k` 이면 `all_tdes = p`, `off_ti = ti`, free, NULL 반환.
4. 끝까지 없으면 free, `"I-6 all_tdes"`.

### 5.2 디스크립터 개수 — `tran_count()`

반환: int 개수. I-6 성공 후 호출.

1. `base = 0, 256, 512, …` (< 65536) 마다 `cnt = min(256, 65536 - base)`:
   1. `tdes_ptrs_read(base, ptrs, cnt)` (§8.1).
   2. `mem_gather_field(ptrs, cnt, off_ti, 4, NULL, ti, NULL)`. -1 → `return base`.
   3. j=0..cnt-1: `!VALID_PTR(ptrs[j]) || ti[j] != base + j` 이면 `return base + j`.
2. 끝까지 연속이면 `deg_set(DEG_TRAN_CAP, 1)`, `return 65536`.

즉 개수 = `tran_index == 인덱스` 가 처음 깨지는 위치(포인터 무효·판독 실패로 0 이 된 값 포함).

버퍼·실패 동작 (Buffers and failures):

| 항목 | 내용 |
|---|---|
| `ptrs[256]`(uint64_t), `ti[256]`(int32_t) | `tran_count` 의 **스택 지역 배열**(`enum { CHUNK = 256 }`). 이 함수 자체에는 malloc 이 없다 |
| `tdes_ptrs_read` 내부 주소 배열 malloc 실패 | `ptrs` 가 전부 0 → 첫 j 에서 `!VALID_PTR` → `return base` |
| `mem_gather_field` 내부 static 주소 버퍼 할당 실패 | `ti` 전부 0 으로 채우고 -1 → `return base` (청크 전체를 미확인으로 취급) |
| 종료 조건 | 위 `return` 들뿐. 0..3 도 그대로 반환된다(아래) |

`DEG_TRAN_CAP` 정확한 조건: 65536 개 슬롯 **전부**(인덱스 0..65535)가 연속으로 확인된 경우에만 설정된다. 즉 정확히 65536 개여도 설정되며("도달", ≥ 65536), 65537번째 이후는 읽지 않는다.

작은 개수(I-6 이후 `ntran < 4`): I-6 은 0..3 을 확인했지만 `tran_count` 는 다시 라이브로 읽으므로 경합·판독 실패·할당 실패로 0~3 을 돌려줄 수 있다. 이 경우에도 **거부하지 않는다**: `tran_probe` 는 NULL 을 반환하고 `all_tdes` 는 0 이 아닌 값으로 남으며 `ntran` 은 그 작은 값(0 포함)이다. 결과:

| ntran | 동작 |
|---|---|
| 0 | `tran_probe_trid` 는 슬롯 0 만 검사(`k < ntran` 거짓). `tran_lock_clientids(0, …)` 는 -1(초기화·지연 재시도 모두). 신원 API 는 `tran_index ≥ ntran` 으로 항상 -1. 덤프는 `available:true`, `"ntran":0`, 슬롯 0 줄 |
| 1..3 | I-8 은 `k < ntran` 까지만. 잠금은 정상 경로(슬롯 수가 적을 뿐) |

`DEG_TRAN` 은 서지 않는다(`all_tdes != 0`). 호출자의 대체 문자열 `"transaction table probe"`(§9)는 `tran_probe` 가 NULL 을 반환하면 `all_tdes` 가 항상 0 이 아니므로(`VALID_PTR` 통과 값) **도달 불가한 방어 코드**다.

### 5.3 I-7 — `tran_probe_spacing()`

1. `tdes_at(0, &t0)`, `tdes_at(1, &t1)` (이 시점 `ptrs_n == 0` 이므로 `mem_read_ptr(all_tdes + 8i)` 직접). 실패 → `"I-7 descriptor pointers"`.
2. `gap = |t1 - t0|`; `max_scan = gap ≤ 32768 ? gap : 0`.
3. `scan_measured = (max_scan > 0)`.
4. `max_scan ≤ 0` 이면 `max_scan = 8192`.
5. NULL 반환.

주의(동작 그대로): `max_scan < 512` 이면 `lock_on_descriptor` 의 `mem_read_shrink(tdes, img, max_scan, 512)` 가 시도 없이 0 을 돌려주므로 CLIENTIDS 잠금은 항상 실패한다.

### 5.4 I-8 — `tran_probe_trid()`

1. `ok = tdes_at(0) 성공 ∧ mem_read(t0 + off_ti + 4, 4) 성공 ∧ v == 0`.
2. k = 1..3 이고 `k < ntran` 인 동안(ok 가 유지되는 동안): `ok = tdes_at(k) 성공 ∧ 판독 성공 ∧ v != 0`.
3. `has_trid = ok`. `!ok` 이면 `deg_set(DEG_TRID, 1)`.

(근거: 시스템 트랜잭션 슬롯 0 의 trid 는 0, 이후 슬롯은 -1(해제) 또는 실 trid.)

### 5.5 I-9 — `read_clientids_at(sp, tdes, off, stride, id, relaxed)`

반환 0 = 성공(id 채움), -1 = 실패. id 의 문자열 버퍼는 실패 시에도 일부 덮어써질 수 있음.

1. k = 0..5: `sa = tdes + off + k × stride`.
   1. `read_std_string(sp, sa, stride, slot[k], 64)` (§6.3) 가 0 이면 다음 k.
   2. 실패 시, `relaxed ∧ stride == COW_STRIDE ∧ tr_read(sp, sa, &p, 8) == 0 ∧ p == 0` 이면 `slot[k] = ""` 로 두고 다음 k (relaxed 의 COW 널 포인터 = 빈 문자열).
   3. 그 외 → -1.
2. 다음 중 하나라도 해당하면 -1 (단락 평가 순서대로):
   1. `!relaxed ∧ !ident_has_content(id)` (strict: 6개 모두 빈 문자열이면 기각).
   2. `tr_read(sp, tdes + off + 6 × stride, &cpid, 4) != 0`.
   3. `cpid < 0 ∨ cpid ≥ 4194304`.
3. `id->cpid = cpid`, 0 반환.

`ident_has_content(id)`: 6개 문자열 중 첫 바이트가 NUL 이 아닌 것이 하나라도 있으면 1.

| 모드 | 사용처 | 빈 문자열 6개 | COW 널 포인터 |
|---|---|---|---|
| strict (relaxed=0) | 잠금 후보 판정, `identity_read` | 기각 | 기각(`VALID_PTR` 실패) |
| relaxed (relaxed=1) | 증인 투표, `tran_dump` | 허용 | 빈 문자열로 허용 |

SSO 는 relaxed 에서도 널 포인터 예외가 없다(SSO 빈 문자열은 로컬 버퍼를 가리키므로 유효 포인터).

### 5.6 I-10 — 증인 투표

#### `collect_witnesses(i, ntran, wit)` → 증인 수(0..3)

1. `wstep = ntran > 3 ? ntran / 3 : 1`.
2. k = 0..2:
   1. 시작 `oi = (i + 1 + k × wstep) % ntran`, 최대 `ntran` 회 `oi = (oi + 1) % ntran` 로 전진.
   2. `oi == i` 이거나 `tdes_at(oi, &other)` 실패면 건너뜀.
   3. `other` 가 이미 고른 증인 포인터와 같으면(주소 비교) 건너뜀.
   4. 아니면 `wit[nwit++] = other`, 이 k 종료.
3. `nwit` 반환. 슬롯 0(시스템)도 증인이 될 수 있다.

루프 경계 (코드 그대로):

```c
for (oi = (i + 1 + k * wstep) % ntran, tries = 0;
     tries < ntran; oi = (oi + 1) % ntran, tries++)
  { if (oi == i || tdes_at (oi, &other) != 0) continue;
    if (other 가 wit[0..nwit) 중 하나와 같음) continue;
    wit[nwit++] = other; break; }
```

| 항목 | 규칙 |
|---|---|
| 시작 `oi` | 첫 전진 **전에** 검사한다 |
| 검사 횟수 | k 마다 정확히 최대 `ntran` 개 위치(시작 포함). 건너뛴 위치(자기 자신 `oi == i`, `tdes_at` 실패, 중복 포인터)도 모두 `tries` 에 산입 |
| 결과 | 한 k 에서 `ntran` 개를 다 봐도 못 찾으면 그 k 는 증인 없이 끝나고 다음 k 로 |
| 중복 판정 | 슬롯 번호가 아니라 **디스크립터 주소** 비교(`wit[w] == other`). 앞 k 가 고른 증인과 같은 주소면 그 다음 슬롯으로 전진 |

#### `witness_vote(tdes, off, stride)` → 1 / 0 / -1

| 결과 | 조건 |
|---|---|
| -1 (fail) | `read_clientids_at(NULL, tdes, off, stride, &w, relaxed=1) != 0` (대상에서 직접 판독) |
| 1 (content) | relaxed 성공 ∧ `ident_has_content(&w)` |
| 0 (무표) | relaxed 성공 ∧ 6개 모두 빈 문자열(해제 슬롯: 정보 없음) |

#### `witnesses_accept(i, ntran, off, stride, wit, *nwit)` → 1 수용 / 0 기각

1. `*nwit < 0` 이면 `*nwit = collect_witnesses(i, ntran, wit)` (디스크립터당 1회 수집, 후보 간 재사용).
2. 증인마다 `fails += (v < 0)`, `content += (v > 0)`.
3. `return !(fails >= 2 && fails > content)`.

증인 3개 기준 판정표:

| fails | content | 결과 |
|---|---|---|
| 0, 1 | 무관 | 수용 |
| 2 | 0 또는 1 | 기각 |
| 3 | 0 | 기각 |

증인이 0~1 개면 항상 수용(fails ≤ 1).

### 5.7 I-11 — 해제 슬롯

`has_trid` 일 때 trid == -1 인 슬롯:
- `identity_read`: -1 반환(신원 없음).
- `tran_identity_prefetch`: `span_free[t] = 1` → CLIENTIDS gather 에서 제외.
- `tran_identity_cached`: `span_free` 가 서 있으면 즉시 -1.
- `tran_dump`: `state = "released"`.

`has_trid == 0` 이면 이 필터는 꺼진다(모든 슬롯을 CLIENTIDS 내용만으로 판정).

---

## 6. 문자열 해독 (String Decoding)

### 6.1 인쇄가능성 — `is_printable_text(s)`

문자열 전체가 아래 "인쇄가능 시퀀스" 의 연속이면 1. 빈 문자열은 1.

`utf8_seq_len(c)` (리드 바이트 → 길이):

| 리드 바이트 c | 길이 |
|---|---|
| `c < 0x80` | 1 |
| `(c & 0xE0) == 0xC0 ∧ c ≥ 0xC2` | 2 |
| `(c & 0xF0) == 0xE0` | 3 |
| `(c & 0xF8) == 0xF0 ∧ c ≤ 0xF4` | 4 |
| 그 외(연속 바이트, C0/C1, F5~FF) | 0 |

`utf8_valid_len(s)`: 길이 n 의 나머지 n-1 바이트가 모두 `(b & 0xC0) == 0x80` 이면 n, 아니면 0. (3바이트 overlong·서로게이트는 따로 검사하지 않음.)

인쇄가능 시퀀스 길이:
- ASCII: `0x20 ≤ b ≤ 0x7E` 이면 1, 그 외 제어 문자·DEL 은 0.
- 비ASCII: `utf8_valid_len` 이 2 이상이고 C1 제어(`0xC2 0x80..0x9F`)가 아니면 그 길이, 아니면 0.

### 6.2 UTF-8 경계 절단 — `utf8_cut(s, ln)`

버퍼 크기(63) 때문에 원문보다 짧게 잘린 경우에만 호출(`len > 복사길이`).

1. `p = ln`. `p > 0 ∧ s[p-1]` 이 연속 바이트 `∧ ln - p < 3` 동안 `p--` (최대 3바이트 후퇴).
2. `p == 0` 이면 `ln` 반환.
3. `c = s[p-1]`, `need = utf8_seq_len(c)`.
4. `need > 1 ∧ (p - 1) + need > ln` 이면 `p - 1` 반환(잘린 시퀀스 제거), 아니면 `ln`.

### 6.3 `read_std_string(sp, addr, stride, out, outn)`

1. `out[0] = '\0'`.
2. `stride == COW_STRIDE` 이면 `read_cow_string`, 아니면 `read_sso_string`.
3. 결과 0 ∧ `is_printable_text(out)` 이면 0, 아니면 -1.

### 6.4 `read_cow_string(sp, addr, out, outn)`

1. `tr_read_ptr(sp, addr, &p)` (8바이트, `VALID_PTR`) 실패 또는 `p < 24` → -1.
2. `rep_cache[0..n)` 을 앞에서부터 찾아 `p` 가 **처음** 일치하는 엔트리가 있으면 `snprintf(out, outn, "%s", 엔트리.s)` 후 `엔트리.rc` 반환(대상 판독 없음). 복사 상한은 `outn`(NUL 포함)이며 이 복사에서는 `utf8_cut` 을 다시 적용하지 않는다. 엔트리 `s` 에는 아래 4~5 단계를 거친 최종 `out`(이미 `utf8_cut` 으로 잘린 문자열, 실패면 `""`)이 `snprintf(s, 64, "%s", out)` 으로 저장되어 있다. 불변식: 현재 모든 호출자가 `outn == IDENT_LEN(64)` 으로 부르므로(§5.5 `read_std_string(…, slot[k], 64)`) 히트 시 복사는 저장된 문자열 전체이다. `outn < 64` 인 호출은 없으며, 그런 호출은 캐시에 추가하지 않는다(6단계)만 히트는 할 수 있다.
3. `out[0] = '\0'`, `rc = -1`.
4. `mem_read(p - 24, rep, 20)` 성공 시: `len = i64@0`, `cap = i64@8`, `ref = i32@16`. §4.2 범위를 모두 만족하면:
   1. `ln = min(len, outn - 1)`.
   2. `ln == 0` 이거나 `mem_read(p, out, ln) == 0` 이면: `len > ln` 이면 `ln = utf8_cut(out, ln)`; `out[ln] = '\0'`; `rc = 0`.
5. `rc != 0` 이면 `out[0] = '\0'`.
6. `rep_cache_n < 64 ∧ outn ≥ 64` 이면 `{p, rc, out}` 을 캐시에 추가(실패 결과도 캐시).
7. `rc` 반환.

`_Rep` 와 데이터는 SPAN 이 아니라 항상 대상 메모리에서 읽는다(힙에 있으므로). SPAN 은 객체(포인터 8바이트) 판독에만 쓰인다.

### 6.5 `read_sso_string(sp, addr, out, outn)`

1. `tr_read(sp, addr, raw, 32)` 실패 → -1.
2. `p = u64@0`, `size = u64@8`. `size > 255 ∨ !VALID_PTR(p)` → -1.
3. `ln = min(size, outn - 1)`.
4. `p == addr + 16` (로컬): `size > 15` → -1; `memcpy(out, raw + 16, ln)`.
   그 외(힙): `mem_read(p, out, ln)` 실패 → -1.
5. `size > ln` 이면 `ln = utf8_cut(out, ln)`. `out[ln] = '\0'`. 0 반환.

SSO 는 rep_cache 를 쓰지 않는다.

### 6.6 SPAN 과 `tr_read`

```c
typedef struct { const unsigned char *img; uint64_t base; size_t len; } SPAN;
```

`tr_read(sp, addr, buf, n)`: `sp != NULL ∧ addr ≥ base ∧ addr + n ≤ base + len` 이면 `img + (addr - base)` 에서 memcpy(0), 아니면 `mem_read`. `tr_read_ptr`: `tr_read` 8바이트 성공 ∧ `VALID_PTR` 이면 0.
SPAN 은 이미 읽어 둔 대상 메모리 이미지를 재사용해 판독 횟수를 줄이는 장치이며, 범위 밖 판독은 투명하게 대상 메모리로 간다.

### 6.7 정화(sanitizing)

이 모듈은 문자열을 고쳐 쓰지 않는다. 인쇄가능하지 않은 문자열(제어 문자, C1, 깨진 UTF-8)은 §6.3 에서 **판독 실패로 거부**되므로 `TRAN_IDENT` 에는 인쇄가능 텍스트만 들어간다. JSON 출력 시 `json_fstr`(ctv_json.c)가 바이트 단위로 다음을 적용한다(검사 순서 그대로):

| 순서 | 바이트 c (현재 위치) | 출력 |
|---|---|---|
| 1 | `"` 또는 `\` | `\` + c (2바이트), 1바이트 전진 |
| 2 | `c < 0x20` | `fprintf("\\u%04x", c)` — **소문자 16진, 4자리 0 채움**(예: `\u001f`, `\u000a`), 1바이트 전진 |
| 3 | `utf8_valid_len(s) == 0`(무효 리드·깨진 연속) | ASCII 6글자 이스케이프 `\ufffd` 를 그대로 출력(U+FFFD 의 UTF-8 바이트 `EF BF BD` 가 **아님**), 1바이트만 전진 |
| 4 | 그 외 | 유효 시퀀스 `len` 바이트를 원문 그대로 출력 |

앞뒤에 `"` 를 붙인다. DEL(0x7F)·C1(`C2 80..9F`)은 유효 시퀀스이므로 원문 그대로 나간다. 이 모듈의 `TRAN_IDENT` 문자열은 이미 인쇄가능 텍스트라 1~3 번 중 1 번만 실제로 발생할 수 있다.

---

## 7. CLIENTIDS 기하 잠금 (Geometry Lock)

### 7.1 `clientids_plausible(img, off, stride)` — 이미지 사전필터

k=0..5, `o = off + k × stride`: `VALID_PTR(u64@o)` 이고, SSO 면 추가로 `u64@(o+8) ≤ 255`. 하나라도 어긋나면 0. 포인터·길이 범위만 보며 대상 판독 없음.

### 7.2 `lock_on_descriptor(i, ntran, tdes)` → 0 잠금 / -1

1. `img = malloc(max_scan)`; `got = mem_read_shrink(tdes, img, max_scan, 512)`. 할당 실패 또는 `got == 0` → -1.
2. `sp = {img, tdes, got}`, `nwit = -1`.
3. stride 순서 `{COW_STRIDE, SSO_STRIDE}` (COW 먼저):
   - `off = 0, 8, 16, …` (`off + CLIENTIDS_BYTES(stride) ≤ got`):
     1. `clientids_plausible(img, off, stride)` 아니면 다음.
     2. `read_clientids_at(&sp, tdes, off, stride, &id, strict)` 실패면 다음.
     3. `witnesses_accept(i, ntran, off, stride, wit, &nwit)` 가 0 이면 다음.
     4. `cid_off = off`, `cid_stride = stride`, free, 0 반환.
4. free, -1.

### 7.3 `tran_lock_clientids(ntran, budget)` → 0 잠금됨 / -1 (공개)

`budget ≤ 0`: 전수 스캔. 양수: 지연 재시도 예산.

1. `cid_stride != 0` → 0 (이미 잠김).
2. `ntran ≤ 0` → -1.
3. `budget ≤ 0 ∨ budget > ntran` 이면 `budget = ntran`.
4. `last_try = time(NULL)` (전수·지연 모두 시도로 계산).
5. `rep_cache_reset()`.
6. `tdes_ptrs_load()` (결과 무시).
7. **trid 경로** — `has_trid ∧ ptrs_n == ntran` 인 경우:
   1. `tried`(calloc), `tried_at`(calloc), `trid`(malloc) 각 ntran 개를 최초 1회 할당. 하나라도 NULL 이거나 `trids_gather(trid, NULL) != 0` → -1 (비-trid 경로로 내려가지 않음).
   2. `lazy = (budget < ntran)`, `tries = 0`.
   3. `step = 0 .. ntran-2` 동안, 단 `lazy` 면 `tries < 4` 인 동안:
      1. `i = 1 + (tcursor + step) % (ntran - 1)` (슬롯 0 제외 순환).
      2. 건너뜀: `trid[i] ≤ 0` (미사용 0·해제 -1) 또는 `trid[i] == tried[i] ∧ last_try - tried_at[i] < 10`.
      3. `tried[i] = trid[i]`, `tried_at[i] = last_try`, `tries++`.
      4. `lock_on_descriptor(i, ntran, tdes_ptrs[i]) == 0` → 0 반환(커서 갱신 없음).
   4. `ntran > 1` 이면 `tcursor = (tcursor + step) % (ntran - 1)` (검사한 슬롯 수만큼 전진). -1 반환.
8. **비-trid 경로**:
   1. `step = 0 .. budget-1`: `i = (cursor + step) % ntran` (슬롯 0 포함). `tdes_at(i)` 성공 ∧ `lock_on_descriptor == 0` → 0.
   2. `cursor = (cursor + budget) % ntran`, -1.

루프 경계 (코드 그대로, `tcursor`·`cursor` 계산에 영향):

```c
/* trid 경로 */
for (step = 0; step < ntran - 1 && (!lazy || tries < LAZY_LOCK_TRIES); step++)
  { i = 1 + (tcursor + step) % (ntran - 1);
    if (skip 조건) continue;
    tried[i] = trid[i]; tried_at[i] = (time_t) last_try; tries++;
    if (lock_on_descriptor (i, ntran, tdes_ptrs[i]) == 0) return 0; }
if (ntran > 1) tcursor = (tcursor + step) % (ntran - 1);
return -1;

/* 비-trid 경로 */
for (step = 0; step < budget; step++)
  { i = (cursor + step) % ntran;
    if (tdes_at (i, &tdes) == 0 && lock_on_descriptor (i, ntran, tdes) == 0) return 0; }
cursor = (cursor + budget) % ntran;
return -1;
```

| 항목 | 규칙 |
|---|---|
| `tries < 4` 검사 위치 | `for` **조건**(break 아님). 4번째 시도가 실패하면 `step++` 가 먼저 실행되고 조건에서 멈춘다 → 종료 시 `step` = 4번째로 시도한 슬롯의 step + 1 |
| 결과 | `tcursor` 는 마지막으로 시도한 슬롯의 **다음** 위치부터 다음 회차를 시작 |
| 비-lazy 종료 | `step == ntran - 1` → `tcursor = (tcursor + ntran - 1) % (ntran - 1) = tcursor`(변화 없음) |
| 건너뛴 슬롯 | `tries` 를 늘리지 않지만 `step` 은 늘린다 |
| 비-trid 경로 | 조기 종료가 성공 반환뿐이므로 실패 시 항상 `budget` 개를 보고 `cursor += budget`. `tdes_at` 실패 슬롯도 1개로 센다 |
| 성공 시 | 두 경로 모두 커서를 갱신하지 않는다(성공 후에는 `cid_stride != 0` 이라 다시 오지 않음) |

trid 경로 요점:
- 트랜잭션을 가진(trid > 0) 디스크립터만 클라이언트를 가질 수 있으므로 그것만 시도한다.
- 같은 trid 로 실패한 슬롯은 10초 안에 재시도하지 않는다(문자열이 채워지는 중이었을 수 있으므로 10초 뒤 재시도). trid 가 바뀌면(새 트랜잭션) 즉시 대상이 된다.
- 전수 스캔(초기화)에서도 `tried`/`tried_at` 이 기록된다.
- `ntran ≤ 16` 이면 `budget = 16 > ntran` → `budget = ntran` → `lazy = 0` 이 되어, 지연 재시도도 tried 필터를 통과한 슬롯 전부를 시도한다.

### 7.4 `tran_geom_refresh()` (공개) — 표시 주기마다 호출

조건 `all_tdes != 0 ∧ cid_stride == 0 ∧ every(&last_try, time(NULL), 1.0)` 이 모두 참이면 `tran_lock_clientids(ntran, LAZY_LOCK_BUDGET=16)`. 즉 잠기기 전까지 **초당 최대 1회** 재시도(`last_try` 는 초기화의 전수 스캔 시각부터 이어짐). trid 경로에서는 새 트랜잭션이 없으면 비용이 포인터 배열 판독 1회 + trid gather 1회다.

---

## 8. 신원 판독 (Identity Reading)

### 8.1 포인터 배열

#### `tdes_ptrs_read(base, out, n)` (void)
1. `mem_read(all_tdes + 8×base, out, 8n)` 성공 → 끝.
2. 실패(매핑 끝 등): 주소 배열 `all_tdes + 8(base+j)` 를 malloc 해 `mem_gather_ok(addrs, n, 8, out, NULL)` — 읽힌 것만 채우고 나머지 0. 주소 배열 malloc 실패 시 `out` 전부 0.

#### `tdes_ptrs_load()` → 0 / -1
1. `ptrs_n = 0`. `all_tdes == 0 ∨ ntran ≤ 0` → -1.
2. `tdes_ptrs == NULL` 이면 `malloc(ntran × 8)`, 실패 → -1.
3. `tdes_ptrs_read(0, tdes_ptrs, ntran)`, `ptrs_n = ntran`, 0.

#### `tdes_at(i, *tdes)` → 0 / -1
`i < ptrs_n ∧ VALID_PTR(tdes_ptrs[i])` 이면 이미지 값. 아니면 `mem_read_ptr(all_tdes + 8i, tdes)` (라이브 판독).

### 8.2 trid gather — `trids_gather(trid, ok)` → 0 / -1
`!has_trid ∨ ptrs_n ≤ 0` → -1. 아니면 `mem_gather_field(tdes_ptrs, ptrs_n, off_ti + 4, 4, NULL, trid, ok)`.

### 8.3 `identity_read(sp, tran_index, id, trid, known_trid)` → 0 / -1 (내부, 순수 질의)

1. `all_tdes == 0 ∨ cid_stride == 0 ∨ tran_index ∉ [0, ntran) ∨ tdes_at 실패` → -1.
2. `t = 0`. `has_trid` 이면:
   1. `known_trid != NULL` → `t = *known_trid`; 아니면 `tr_read(sp, tdes + off_ti + 4, &t, 4)` 실패 → -1.
   2. `t == -1` → -1 (I-11).
3. `trid != NULL` 이면 `*trid = t` (CLIENTIDS 실패여도 이미 기록됨).
4. `read_clientids_at(sp, tdes, cid_off, cid_stride, id, strict)` 실패 → -1.
5. `ident_has_content(id) ? 0 : -1`.

`tran_identity_mem(i, id, trid)` = `identity_read(NULL, i, id, trid, NULL)` (캐시 없는 직접 판독, 캐시 키 할당 실패 시 fallback).

### 8.4 신원 캐시

#### `ident_cache_setup()` → 0 / -1
1. `ident_cache_n == ntran ∧ span_len > 0` → 0 (구성 완료).
2. 기존 엔트리의 `key` 와 `ident_cache` free, `ident_cache = calloc(ntran)`, `ident_cache_n = (NULL ? 0 : ntran)`.
3. `tdes_ptrs`, `span_img`, `span_ok`, `span_free`, `span_trid`, `span_trid_ok` 전부 free·NULL, `ptrs_n = 0`, `prefetch_n = 0`.
4. `span_lo = cid_off`, `span_len = CLIENTIDS_BYTES(cid_stride)` (trid 는 별도 gather 이므로 구간에 포함하지 않음).
5. `ident_cache == NULL ? -1 : 0`.

CLIENTIDS 잠금 후 첫 호출에서 한 번 구성되며, `ntran` 이 고정이므로 이후 재구성되지 않는다.

`tdes_ptrs` 상태 전이 (`tdes_at` 이 보는 값):

| 시점 | `tdes_ptrs` / `ptrs_n` | `tdes_at(i)` |
|---|---|---|
| `tran_probe` 중(I-7, I-8) | NULL / 0 | 항상 라이브 `mem_read_ptr` |
| `tran_lock_clientids` 의 `tdes_ptrs_load` 후 | 그때 읽은 이미지 / `ntran` | 이미지 값(무효 포인터 칸만 라이브) |
| `ident_cache_setup` 재구성 직후 | **free 후 NULL** / 0 (같은 함수 안에서 함께 설정) | 라이브. 해제된 포인터를 보는 순간은 없다 |
| 같은 `tran_identity_prefetch` 의 `tdes_ptrs_load` 후 | 새 malloc + 이번 구간 이미지 / `ntran` | 이번 구간 이미지 |
| 그 load 의 malloc 실패 | NULL / 0 | 라이브 |

`--dump` 경로(cub_ctv.c `main`): `tran_subsystem_init`(잠금 → 이미지 적재) → `thr_probe` → `view_capture` → `view_capture_idents` → `tran_identity_prefetch`(잠긴 경우 `ident_cache_setup` 재구성 + 재적재) → `detail_dump_write` → `tran_dump`. 따라서 `tran_dump` 의 `tdes_at` 은, 잠겼으면 직전 프리페치 구간의 이미지를, 잠기지 않았으면(`prefetch` 가 `cid_stride == 0` 으로 바로 반환) 초기화 잠금 시도 때의 이미지를 쓴다. 이미지 칸이 `VALID_PTR` 이 아니면 그 슬롯만 라이브로 읽는다.

#### `tran_identity_prefetch()` (공개, void) — 구간당 1회

1. `prefetch_n = 0`, `rep_cache_reset()`.
2. `all_tdes == 0 ∨ cid_stride == 0 ∨ ident_cache_setup() != 0` → 반환.
3. `n = ntran`. NULL 인 버퍼만 할당: `span_img (n × span_len)`, `span_ok (n)`, `span_free (n)`, `span_trid (4n)`, `span_trid_ok (n)`.
4. 하나라도 NULL 이거나 `tdes_ptrs_load() != 0` → 반환. (판독 1: 포인터 배열 1회)
5. `span_free`, `span_trid_ok` 를 0 으로.
6. `has_trid` 이면: `ptrs_n != n ∨ trids_gather(span_trid, span_trid_ok) != 0` → 반환(`prefetch_n = 0`). (판독 2: trid gather 1회) 그 뒤 `span_free[t] = span_trid_ok[t] ∧ span_trid[t] == -1`.
7. `mem_gather_field(tdes_ptrs, n, span_lo, span_len, span_free, span_img, span_ok) == 0` 이면 `prefetch_n = n`. (판독 3: 해제 슬롯을 뺀 CLIENTIDS 구간 gather 1회)

#### `tran_identity_cached(tran_index, id, trid)` → 0 / -1 (공개)

1. `all_tdes == 0 ∨ cid_stride == 0 ∨ tran_index ∉ [0, ntran) ∨ ident_cache_setup() != 0` → -1.
2. `tran_index < prefetch_n ∧ span_free[tran_index]` → -1 (이번 구간 해제).
3. 구간 이미지 결정:
   - `tran_index < prefetch_n ∧ VALID_PTR(tdes_ptrs[i]) ∧ span_ok[i]` → `tdes = tdes_ptrs[i]`, `img = span_img + i × span_len`.
   - 아니면 `tdes_at(i)` ∧ `mem_read(tdes + span_lo, buf, span_len)` (스택 버퍼). 실패 → -1.
4. `trid_v = 0`. `has_trid` 이면 `i < prefetch_n ∧ span_trid_ok[i]` → `span_trid[i]`, 아니면 `mem_read(tdes + off_ti + 4, 4)` 실패 → -1.
5. 키 = `trid_v`(4바이트) ‖ `img[0..span_len)`, `keyn = 4 + span_len`.
6. 엔트리 `c = &ident_cache[i]`:
   - **히트**: `c->tdes == tdes ∧ c->key != NULL ∧ c->age < 3 ∧ memcmp(c->key, key, keyn) == 0` → `c->age++`.
   - **미스**: `c->key == NULL` 이면 malloc(keyn), 실패 → `return tran_identity_mem(i, id, trid)`. 키 복사, `c->tdes = tdes`, `c->age = 0`, `SPAN sp = {img, tdes + span_lo, span_len}` 로 `c->rc = identity_read(&sp, i, &c->id, &c->trid, has_trid ? &trid_v : NULL)`.
7. `c->rc == 0` 이면 `*id = c->id`, `trid != NULL` 이면 `*trid = c->trid`.
8. `c->rc` 반환.

캐시 규칙 정리:

| 항목 | 내용 |
|---|---|
| 키 | (trid, CLIENTIDS 구간 원시 바이트 = 문자열 객체 6개 + pid) + 엔트리의 `tdes` 주소 일치 |
| 해독 생략 조건 | 키 동일 ∧ `age < 3` |
| 강제 재해독 | 해독 뒤 연속 3회 히트(age 0→1→2→3)한 다음 호출 — 즉 키가 안 바뀌어도 4번째 호출마다 재해독 |
| 근거 | 슬롯에 새 클라이언트가 붙으면 새 트랜잭션(새 trid) → 키 변경. COW 힙 문자열이 제자리에서 바뀌는 경우(객체 바이트 불변)는 최대 TTL 구간만 낡은 값 유지 |
| 실패 캐시 | `rc = -1` 도 같은 규칙으로 재사용 |
| SPAN | 문자열 객체·pid 는 이미지에서, COW `_Rep`/데이터·SSO 힙 데이터는 대상에서 판독 |

### 8.5 COW 표현 캐시 (`rep_cache`)

| 항목 | 내용 |
|---|---|
| 목적 | 여러 슬롯이 공유하는 COW 표현(빈 문자열 `_S_empty_rep`, 복사된 이름)을 구간당 1회만 해독 |
| 키 | 데이터 포인터 `p` |
| 값 | 해독 결과 rc 와 문자열(최대 63바이트) |
| 용량 | 64. 가득 차면 새 엔트리를 추가하지 않음(교체 없음) |
| 초기화 | `tran_identity_prefetch`, `tran_lock_clientids`, `tran_dump` 시작 시 `rep_cache_n = 0` |
| 주의 | 캐시 값은 인쇄가능성 검사 전 문자열이며, 히트 때도 `read_std_string` 이 다시 `is_printable_text` 를 적용 |

---

## 9. 공개 함수 요약 (Public API)

| 함수 | 반환 | 부작용 |
|---|---|---|
| `const char *tran_probe(uint64_t log_gl)` | NULL = 표 사용 가능, 아니면 거부 사유 문자열 | I-6 → I-7 실패 시 `all_tdes = 0` 후 사유 반환. 성공 시 `ntran = tran_count()`, `tran_probe_trid()` 수행(`DEG_TRAN_CAP`·`DEG_TRID` 설정 가능). 단계 순서·거부 시 잔존값은 §9.2 |
| `int tran_lock_clientids(int ntran, int budget)` | 0 잠김 / -1 | §7.3 |
| `void tran_geom_refresh(void)` | — | §7.4 |
| `void tran_identity_prefetch(void)` | — | §8.4 |
| `int tran_identity_cached(int, TRAN_IDENT*, int32_t*)` | 0 = 클라이언트 신원 있음 / -1 | §8.4 |
| `void tran_dump(FILE *f)` | — | §10 |

거부 사유 문자열 전체:

| 문자열 | 발생 |
|---|---|
| `"I-6 out of memory"` | log_Gl 스캔 버퍼 malloc 실패 |
| `"I-6 log_Gl unreadable"` | log_Gl 4096 바이트도 못 읽음 |
| `"I-6 all_tdes"` | 수열 0..3 을 만족하는 포인터 없음 |
| `"I-7 descriptor pointers"` | 포인터 0 또는 1 이 무효/판독 불가 |
| `"log_Gl symbol"` | (호출자) 심볼 없음 — `tran_probe` 미호출 |
| `"transaction table probe"` | (호출자) `all_tdes == 0` 인데 사유가 NULL 인 경우의 대체 문자열 |

### 9.1 호출 순서 (호출자 기준)

1. 초기화(`cub_ctv.c: tran_subsystem_init`): `why = tran_probe(bias + sym_loggl)`; NULL 이면 `tran_lock_clientids(ntran, 0)` (전수 스캔). `all_tdes == 0` 이면 `deg_set(DEG_TRAN, 1)`, `tran_refused = why`. CLIENTIDS 잠금 실패는 강등이 아니다(이후 지연 재시도).
2. 표시 주기마다(`ctv_frame.c`): `tran_geom_refresh()` → `tran_identity_prefetch()` → `t = 0..ntran-1` 에 대해 `tran_identity_cached(t, …)` 가 0 인 슬롯만 프레임에 추가. 표 사용 가능 판정은 `all_tdes != 0 ∧ !(degraded & DEG_TRAN)`.
3. 상세 덤프: `tran_dump(f)`.

`tran_probe` 는 프로세스 수명 동안 `tran_subsystem_init` 에서 **한 번만** 불린다(`sym_loggl == 0` 이면 한 번도 불리지 않음). `tran_subsystem_init` 은 `main` 에서 `cs_layout_probe` 성공 직후, `thr_probe` **이전**에 호출된다(04 §5.1 의 `tran_geom.ntran` 의존).

### 9.2 `tran_probe` 내부 순서와 거부 시 잔존값

```c
if ((why = tran_probe_all_tdes (log_gl)) != NULL      /* I-6 */
    || (why = tran_probe_spacing ()) != NULL)         /* I-7 */
  { tran_geom.all_tdes = 0; return why; }
tran_geom.ntran = tran_count ();                       /* I-7 뒤 */
tran_probe_trid ();                                    /* I-8 */
return NULL;
```

- I-7 은 `tran_count` **이전**에 돈다. 그 시점 `ntran == 0`, `ptrs_n == 0` 이므로 `tdes_at(0/1)` 은 라이브 판독이다.
- 거부 시 되돌리는 것은 `all_tdes = 0` **하나뿐**이다. 다른 필드는 리셋하지 않는다(전역 0 초기화 상태 + 그때까지 쓴 값).

| 거부 지점 | `all_tdes` | `off_ti` | `max_scan` / `scan_measured` | `ntran` | `has_trid` | `DEG_TRAN_CAP` / `DEG_TRID` |
|---|---|---|---|---|---|---|
| `"log_Gl symbol"`(호출 안 함) | 0 | 0 | 0 / 0 | 0 | 0 | 미설정 |
| I-6 거부(3종) | 0 | 0 (I-6 은 성공 시에만 씀) | 0 / 0 | 0 | 0 | 미설정 |
| I-7 거부 | 0 (I-6 이 쓴 값을 지움) | **I-6 이 찾은 ti 유지** | 0 / 0 | 0 | 0 | 미설정 |

따라서 I-7 거부 후 덤프 머리는 `"available":false,"all_tdes":"0x0","ntran":0,"off_tran_index":<ti>,"cid_off":0,"cid_abi":"not-locked","tdes_scan_bytes":0,"tdes_scan_is_spacing":false,"has_trid":0` 이고, I-6 거부면 `off_tran_index` 도 0 이다. 트랜잭션 관련 강등 비트는 `DEG_TRAN` 만 선다(`DEG_TRAN_CAP`·`DEG_TRID` 없음).

---

## 10. `tran_dump(FILE *f)` JSON

1. `rep_cache_reset()`.
2. 머리 객체(한 줄):

```
"transactions":{"available":<bool>,"all_tdes":"0x<hex>","ntran":<int>,
 "off_tran_index":<int>,"cid_off":<int>,"cid_abi":"<cow|sso|not-locked>",
 "tdes_scan_bytes":<int>,"tdes_scan_is_spacing":<bool>,"has_trid":<0|1>,
 "slots":[\n
```

| 키 | 값 |
|---|---|
| `available` | `all_tdes != 0` |
| `all_tdes` | `"0x%llx"` |
| `ntran` | `tran_geom.ntran` |
| `off_tran_index` | `off_ti` |
| `cid_off` | `cid_off` (미잠금이면 0) |
| `cid_abi` | stride 8 → `"cow"`, 32 → `"sso"`, 그 외 `"not-locked"` |
| `tdes_scan_bytes` | `max_scan` |
| `tdes_scan_is_spacing` | `scan_measured` (true/false) |
| `has_trid` | 정수 0/1 |

3. `all_tdes != 0` 일 때 `t = 0..ntran-1` 각 슬롯을 한 줄로(첫 슬롯 외에는 앞에 `,`):
   - `tdes_at(t)` 실패: `{"idx":t,"state":"unreadable"}`.
   - 성공: `ti = -2`, `trid = 0` 기본값으로 `off_ti`, `off_ti + 4` 를 각각 4바이트 판독(실패 무시). `have_id = cid_stride != 0 ∧ read_clientids_at(NULL, tdes, cid_off, cid_stride, &id, relaxed) == 0`.
   - `state` 우선순위: `t == 0` → `"system"`; `has_trid ∧ trid == -1` → `"released"`; `have_id ∧ ident_has_content` → `"client"`; 그 외 `"empty"`.
   - 출력: `{"idx":t,"tdes":"0x<hex>","tran_index":ti,"trid":trid,"state":"<state>"` + (`have_id` 이면 `json_ident(f, &id, 1)`) + `}`.
4. 마지막에 `]}`.

바이트 단위 형식 (printf 그대로):

```c
fprintf (f, "\"transactions\":{\"available\":%s,\"all_tdes\":\"0x%llx\","
         "\"ntran\":%d,\"off_tran_index\":%d,\"cid_off\":%d,"
         "\"cid_abi\":\"%s\",\"tdes_scan_bytes\":%d,"
         "\"tdes_scan_is_spacing\":%s,\"has_trid\":%d,"
         "\"slots\":[\n", …);
/* 슬롯 t (판독 실패) */
fprintf (f, "%s{\"idx\":%d,\"state\":\"unreadable\"}\n", t ? "," : "", t);
/* 슬롯 t (판독 성공) */
fprintf (f, "%s{\"idx\":%d,\"tdes\":\"0x%llx\",\"tran_index\":%d,"
         "\"trid\":%d,\"state\":\"%s\"", t ? "," : "", t, tdes, ti, trid, state);
if (have_id) json_ident (f, &id, 1);
fprintf (f, "}\n");
/* 끝 */
fprintf (f, "]}");
```

| 항목 | 규칙 |
|---|---|
| 머리 | 한 줄, 공백 없음, `"slots":[` 뒤 `\n` |
| 구분자 | 슬롯 줄 **앞**에 `,`(t ≠ 0 이면 무조건 — 슬롯 0 은 항상 출력되므로 빈 첫 항목 문제 없음). 쉼표는 줄 머리에 붙는다: `…}\n,{…}\n` |
| 슬롯 줄 끝 | 모든 슬롯 줄은 `}\n` 으로 끝난다 |
| 끝 | `]}` — **뒤에 줄바꿈 없음**(호출자 `detail_dump_write` 가 이어서 `",\n"` 출력) |
| 16진 | `all_tdes`·`tdes` 모두 `0x%llx`: 소문자, 0 채움 없음, 0 이면 `0x0` |
| 정수 | `ntran`·`off_tran_index`·`cid_off`·`tdes_scan_bytes`·`has_trid`·`idx`·`tran_index`·`trid` 는 `%d`. `ti` 판독 실패면 기본값 -2, `trid` 실패면 0 |
| 불리언 | `available`, `tdes_scan_is_spacing` 은 `true`/`false` 리터럴, `has_trid` 는 정수 |
| `available:false` | 슬롯 루프 조건(`all_tdes != 0`)이 거짓이라 머리 직후 `]}` → `…"slots":[\n]}` |

`json_ident(f, id, with_info=1)` 가 붙이는 키(순서대로): `"info"`, `"user"`, `"program"`, `"login"`, `"host"`, `"client_ip"` (모두 `json_fstr` 문자열), `"cpid"` (정수).

덤프는 해제·빈 슬롯을 포함한 모든 슬롯의 원시값을 보여 주며(relaxed 파싱), 신원 캐시·프리페치를 쓰지 않는다(`tdes_at` 은 직전에 적재된 `tdes_ptrs` 를 쓸 수 있음 — 어느 이미지인지는 §8.4 "`tdes_ptrs` 상태 전이"). 시작 시 `rep_cache_reset()` 하므로 COW 해독은 덤프 안에서 새로 캐시된다.

`json_ident` 바이트 형식: `with_info` 면 `,"info":` + `json_fstr(info)`, 이어 `,"user":`·`,"program":`·`,"login":`·`,"host":`·`,"client_ip":` 각각 + `json_fstr(…)`, 마지막 `fprintf(",\"cpid\":%d", cpid)`. 공백 없음. relaxed 판독 실패(`have_id == 0`)면 이 키들이 전혀 붙지 않는다.

---

## 11. 실패·강등 동작 정리

| 상황 | 동작 |
|---|---|
| I-6/I-7 거부 | `all_tdes = 0`, 호출자가 `DEG_TRAN` + `tran_refused`. 이후 모든 신원 API 가 -1, 덤프는 `available:false`, 슬롯 없음 |
| I-8 실패 | `has_trid = 0`, `DEG_TRID`. 잠금은 비-trid 경로(커서 + 예산 16), 해제 필터·trid gather 없음, trid 는 0 으로 보고 |
| 디스크립터 65536 개 이상 | `ntran = 65536`, `DEG_TRAN_CAP` |
| CLIENTIDS 미잠금 | `cid_stride = 0`. 신원 API -1, 덤프 `cid_abi:"not-locked"`. `tran_geom_refresh` 가 초당 1회 재시도 |
| 프리페치 실패(할당·trid gather) | `prefetch_n = 0` — `tran_identity_cached` 가 슬롯별 개별 판독으로 동작 |
| 개별 gather 범위 실패 | 해당 슬롯 `span_ok = 0` → 그 슬롯만 개별 판독 |
| 캐시 키 할당 실패 | 캐시 없이 `tran_identity_mem` |
| 메모리 할당 실패 일반 | 해당 기능 -1/미수행, 프로세스 중단 없음 |
