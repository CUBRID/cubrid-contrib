# 스레드 엔트리 명세 (Thread Entries Specification)

대상: `src/ctv_thr_probe.c`(순수 탐색), `src/ctv_thr.c`(필드 수명·표·대기 체인·집계·덤프), `src/ctv_field.c`(필드 상태기계, 이 모듈이 쓰는 부분), `src/ctv.h` 의 관련 선언. 불변식 번호 I-14~I-20 은 PR.md 원장과 같다. 이 문서만으로 같은 동작을 재구현할 수 있도록, 모든 상수·조건·부등호·순서를 코드 그대로 적는다.

---

## 1. 정의와 공통 규약

| 용어 | 정의 |
|---|---|
| 엔트리 배열 | `cubthread::Manager` 가 가진 `m_all_entries = new cubthread::entry[max]` 연속 배열. 시작 주소 `thr_geom.base`, 원소 크기 `thr_geom.stride`, 원소 수 `thr_geom.count` |
| 슬롯(slot) `k` | 배열 원소 번호 0..count−1. 엔진의 `index` 필드 값은 `k + 1`(엔진은 풀 엔트리를 1부터 번호, 0번은 메인 스레드라 배열 밖). 매크로 `THR_INDEX(slot) = slot + 1` |
| 엔트리 주소 | `base + k * stride` |
| 이미지(img) | 배열 전체(`count * stride` 바이트) 또는 그 접두부를 한 번에 읽은 로컬 버퍼. 슬롯 k 의 필드 off 는 `img[k*stride + off]` |
| 실스레드(live) 슬롯 | m_id 표 `thr_ids[k] != 0` 인 슬롯 (`thr_slot_live(k)`) |
| 절대 오프셋 | 엔트리 시작 기준 바이트 오프셋. `off_index`, `off_id`, `off_resume`, `off_status`, `off_next` 는 절대 오프셋 |
| 상대 오프셋 | `off_tran` 만 `index` 기준 상대값(28 또는 20). tran_index 절대 위치 = `off_index + off_tran` |
| `off_tid` | 엔트리가 아니라 **struct pthread**(m_id 값이 가리키는 곳) 안의 오프셋 |

판독 규약:

- 정수 판독 `rd_i32(buf, off)`, `rd_u64(buf, off)` 는 `memcpy` 로 호스트(리틀 엔디언) 순서 그대로 읽는다(정렬 요구 없음).
- `VALID_PTR(p)` ≡ `p > 0x1000 && p < 0x800000000000`.
- `mem_read(addr, buf, n)`: `/proc/<pid>/mem` pread 가 정확히 n 바이트면 0, 아니면 −1.
- `mem_read_shrink(addr, buf, max, min)`: n = max 부터 n ≥ min ∧ n > 0 인 동안 `mem_read(addr, buf, n)` 을 시도하고 실패하면 n 을 절반으로; 성공한 n 반환, 없으면 0.
- `mem_read_ptr(addr, &p)`: 8바이트 판독 성공 ∧ `VALID_PTR(p)` 이면 0.
- `mem_gather_ok(addrs, n, each, buf, ok)`: 각 `addrs[i]` 에서 `each` 바이트를 `buf[i*each]` 로. `buf` 전체와 `ok[]` 를 먼저 0으로 채움. 주소 0 은 읽지 않음(0, ok=0). `process_vm_readv` 로 최대 `IOV_MAX` 범위씩 묶어 읽고, 일부만 성공하면 처음 실패한 범위를 pread 단건으로 읽은 뒤 그 다음부터 재개. `process_vm_readv` 가 ENOSYS/EPERM 이면 이후 전부 pread. 읽힌 범위만 `ok[i]=1`. 항상 0 반환.
- `mem_gather_field(bases, n, off, width, skip, out, ok)`: `addrs[i] = (VALID_PTR(bases[i]) && (skip==NULL || !skip[i])) ? bases[i]+off : 0` 으로 `mem_gather_ok` 호출. 주소 배열 할당 실패 시 −1.
- `os_task_list(pid, &tasks)`: `/proc/<pid>/task` 의 '.' 로 시작하지 않는 항목을 `atoi` 해 **오름차순 정렬**한 배열과 개수. 디렉터리 열기 실패·할당 실패 −1, 0개면 `*out=NULL`·0.
- `os_task_known(tasks, n, tid)`: n>0 ∧ 이진 탐색으로 tid 존재.
- `deg_set(bits, on)`: 전역 `degraded` 에 bits OR(on) / AND NOT(off).
- `REFUSE(id)` ≡ `return (id)` — 거부 사유 문자열(불변식 ID)을 반환.

---

## 2. 상수

| 이름 | 값 | 위치 | 의미 |
|---|---|---|---|
| `CS_MIN_SAMPLES` | 4 | ctv.h | 수열 프로브가 일치해야 하는 최소 개수(엔트리 0..3, 최소 count, 최소 표본 등) |
| `TRAN_SLOTS_MAX` | 65536 | ctv.h | TDES 개수 미확보 시 tran_index 상한 |
| `THR_STRIDE_MIN` | 256 | ctv.h | stride 하한 |
| `THR_STRIDE_MAX` | 65536 | ctv.h | stride 상한 |
| `THR_INDEX_SCAN` | 2048 | ctv.h | `index` 후보를 찾는 엔트리 앞부분 바이트 수 |
| `THR_MAX` | 65536 | ctv.h | count 상한 |
| `THR_TRAN_REL_MAX` | 28 | ctv.h | tran_index 상대 오프셋의 최댓값; I-15 미확정 시 `off_tran` 기본값 |
| `THR_NEXT_SPAN` | 64 | ctv.h | resume_status 뒤 next_wait_thrd 후보 탐색 범위(바이트) |
| `THR_NEXT_CANDS` | 8 | ctv.h | next_wait_thrd 후보 최대 수 |
| `THR_ID_SLACK` | 8 | ctv.h | 엔트리 풀 밖 스레드 허용 수 |
| `THR_WAITERS_MAX` | 64 | ctv.h | 대기 체인 홉 상한(호출자가 max 로 넘김) |
| `THR_TID_SCAN` | 1024 | ctv.h | struct pthread 에서 OS tid 를 찾는 바이트 수 |
| `THR_RESUME_MAX` | 28 | ctv.h | resume_status 유효값 0..27 (배열 크기) |
| `THR_RESUME_CS_MIN` | 7 | ctv.h | CS 코드 범위 하한(CSECT_READER_SUSPENDED) |
| `THR_RESUME_CS_MAX` | 12 | ctv.h | CS 코드 범위 상한(CSECT_PROMOTER_RESUMED) |
| `THR_ID_CANDS` | 64 | ctv.h | m_id 후보 최대 수 |
| `THR_TID_SAMPLE` | 32 | ctv.h | I-17 1단계 표본 수 |
| `TS_WAIT` | 3 | ctv.h | `cubthread::entry::status` 의 suspend 상태 |
| (TS_RUN) | 2 | 코드 내 리터럴 | I-20 판정에 쓰는 실행 상태 값(매크로 없음) |
| `THR_NEXT_VOTES` | 3 | ctv_thr.c | I-19 확정에 필요한 서로 다른 링크 표 수 |
| `REPROBE_CHANGE_S` | 60 | ctv_thr.c | 스레드 구성 변동 시 느린 단계 재프로빙을 당기는 최소 간격(초) |
| `FIELD_NONE/WEAK/SET` | 0/1/2 | ctv.h | 필드 상태 |
| `FIELD_REJECTS` | 8 | ctv.h | 필드당 기각 오프셋 기억 수 |
| `SCHED_FIRST` | 2 | ctv.h | 첫 재시도 간격(초) |
| `SCHED_FAST_MAX` | 64 | ctv.h | 빠른 단계 간격 상한(초) |
| `SCHED_FAST_ROUNDS` | 6 | ctv.h | 빠른 단계 회차 수 |
| `SCHED_SLOW` | 300 | ctv.h | 느린 단계 간격(초) |
| `CS_STATS_TO_PQ` / `CS_STATS_TO_WQ` | 24 / 32 | ctv.h | CS 의 promoter/writer 대기 큐 필드 위치(stats − 24 / stats − 32), 체인 머리의 출처 |

강등 비트(ctv.h):

| 비트 | 값 | 불변식 | 의미 |
|---|---|---|---|
| `DEG_THREAD` | 0x4 | I-14 | 엔트리 배열 없음 |
| `DEG_THR_TRAN` | 0x8 | I-15 | tran_index 미검증 |
| `DEG_THR_ID` | 0x10 | I-16 | m_id 미검증 |
| `DEG_THR_TID` | 0x40 | I-17 | OS tid 오프셋 미검증 |
| `DEG_THR_RESUME` | 0x80 | I-18 | resume_status 미검증 |
| `DEG_THR_NEXT` | 0x100 | I-19 | next_wait_thrd 후보조차 없음 |
| `DEG_THR_STATUS` | 0x200 | I-20 | m_status 미검증 |
| `DEG_THR_SUB` | 위 6개(0x8,0x10,0x40,0x80,0x100,0x200)의 OR | — | thr_publish 가 매번 지우고 다시 세우는 집합 |

`thread_resume_suspend_status` 코드 해석(엔진 enum, 이 모듈이 의존하는 값): suspended 코드 = 3,5,7,…,25(홀수) 및 26; 7=CS reader suspended, 9=CS writer suspended, 11=CS promoter suspended, 8/10/12 = 각각 resumed. 짝수(26 제외)는 깨어난 뒤의 값.

---

## 3. 자료구조

### 3.1 `THR_GEOM thr_geom` (전역, ctv.h)

| 필드 | 형 | 의미 | 쓰는 곳 |
|---|---|---|---|
| `base` | uint64_t | 배열 시작, 0 = 없음 | `thr_probe_entries`(설정), `thr_probe`(0으로) |
| `stride` | int | 원소 크기 | `thr_probe_entries` |
| `count` | int | 원소 수 | `thr_probe` |
| `off_index` | int | `index` 절대 오프셋 | `thr_probe_entries` |
| `off_id` | int | m_id 절대 오프셋(I-16) | `thr_publish` (= `f_id.off`, 미확정이어도 그 값 복사; 초기 0) |
| `has_tran`, `off_tran` | int | I-15; `off_tran` 은 상대값, 미확정이면 `THR_TRAN_REL_MAX`(28) | `thr_publish` (그리고 `thr_probe` 첫 줄이 28 로 초기화) |
| `has_id` | int | I-16 | `thr_publish` |
| `has_tid`, `off_tid` | int | I-17, struct pthread 내 오프셋 | `thr_publish` |
| `has_resume`, `off_resume` | int | I-18 | `thr_publish` |
| `has_status`, `off_status` | int | I-20 | `thr_publish` |
| `has_next` | int | I-19 상태가 NONE 이 아님(후보만 있어도 1) | `thr_publish` |
| `off_next` | int | I-19 확정 오프셋, 후보만이면 −1 | `thr_publish` |

규칙: `has_*` 와 `off_*`(off_index·base·stride·count 제외)는 **`thr_publish` 만 쓴다**.

### 3.2 `FIELD` (ctv.h)

```c
typedef struct { int state, off; int hits, misses; int rejected[FIELD_REJECTS], nrejected; } FIELD;
```

| 필드 | 의미 |
|---|---|
| `state` | `FIELD_NONE`(0, 모름) / `FIELD_WEAK`(1, 잠정 규칙으로만 선택) / `FIELD_SET`(2, 확정) |
| `off` | 잠긴 오프셋 |
| `hits`, `misses` | 런타임 닻 집계 |
| `rejected[8]`, `nrejected` | 닻이 기각한 오프셋(오래된 것부터 밀려남) |

모듈 정적 필드 6개: `f_tran`(I-15), `f_id`(I-16), `f_tid`(I-17), `f_resume`(I-18), `f_status`(I-20), `f_next`(I-19). 모두 0 초기화(NONE).

### 3.3 `SCHED reprobe`

```c
typedef struct { time_t at, last; int gap, rounds; } SCHED;
```

재프로빙 일정 1개(모듈 정적). §14.2(SCHED 함수)·§14.3(사용처) 참조.

### 3.4 모듈 정적 상태 (ctv_thr.c)

| 이름 | 형 | 의미 |
|---|---|---|
| `thr_keep_image` | int (전역) | `--dump` 시 1: 시작 프로브 이미지를 `thr_kept` 로 보관 |
| `thr_kept` | unsigned char* | 보관 이미지 |
| `thr_ids` | uint64_t[count] | 슬롯별 m_id 값(pthread_t). 처음 필요할 때 calloc |
| `thr_sorted` | `THR_SORTED`[count] | (m_id, slot) 를 m_id 오름차순으로 |
| `thr_nsorted` | int | 그 개수 |
| `thr_ids_at` | double | m_id 표 마지막 구성 시각(초), 1초 게이트 공용 |
| `thr_tids` | int[count] | 슬롯별 OS tid(I-17) |
| `thr_tasks`, `thr_ntasks` | int*, int | 표를 만들 때의 `/proc/<pid>/task` 목록(정렬) |
| `next_cand[8]`, `next_ncand` | int | I-19 후보 오프셋(오름차순) |
| `next_votes[8]` | int | 후보별 단독 링크 표 수 |
| `next_voted[8][3]` | uint64_t | 후보별 이미 센 링크 `(from_slot<<32) \| to_slot` (OR) |
| `hop_buf` | unsigned char[stride] | 홉 판독 버퍼(지연 할당) |
| `hop_tbuf` | unsigned char[stride*8] | 후보 대상 엔트리 span gather 버퍼(지연 할당) |

```c
typedef struct { uint64_t tid; int slot; } THR_SORTED;   /* tid = m_id 값 */
```

---

## 4. I-14 엔트리 배열 탐색

### 4.1 진입: `void thr_probe(uint64_t manager_sym, int pid)`

`manager_sym` 은 `.symtab` 로컬 심볼 `_ZN9cubthreadL7ManagerE`(cubthread::Manager, 포인터 변수)의 런타임 주소(bias 적용), 없으면 0.

1. `thr_geom.off_tran = THR_TRAN_REL_MAX`.
2. `manager_sym == 0` 또는 `mem_read_ptr(manager_sym, &mobj) != 0` 또는 `thr_probe_entries(mobj) != NULL` 이면: `base = 0`, `deg_set(DEG_THREAD, 1)`, 반환(거부 아님, 강등).
3. `thr_geom.count = 0` 후 개수 세기(§4.5) → `thr_geom.count = n` 을 **먼저** 대입하고, 그 다음 `n < 4` 이면 이미지 해제, `base = 0`, `DEG_THREAD` 세움, 반환.
4. 필드 프로브(§14.1).

실패 경로별 `thr_geom` 잔존값 (`thr_publish` 는 어느 실패 경로에서도 불리지 않는다):

| 경로 | `base` | `stride` / `off_index` | `count` | `has_*` / `off_id`·`off_tid`·`off_resume`·`off_status`·`off_next` | `off_tran` | 강등 |
|---|---|---|---|---|---|---|
| 2단계(심볼 없음·포인터 판독 실패·`thr_probe_entries` 거부) | 0 | 0 / 0 (`thr_probe_entries` 는 성공 시에만 씀) | 0 (전역 0 초기화; 이 경로는 count 를 대입하지 않음) | 전부 0 (`off_next` 도 −1 이 아니라 0) | 28 | `DEG_THREAD` 만 |
| 3단계(`n < 4`) | 0 | **probe 가 찾은 값 유지** | **n (0..3) 유지** | 전부 0 | 28 | `DEG_THREAD` 만 |
| `--trans` 단독(`thr_skipped`, `thr_probe` 미호출) | 0 | 0 / 0 | 0 | 전부 0 | **0** (1단계도 실행 안 됨) | 없음(레이아웃 줄에 −1 을 넘기므로 `  threads=N` 조각이 생략됨) |

`thr_probe` 는 프로세스 수명 동안 한 번만 불리므로 "전역 0 초기화" 가 곧 이 값들이다. `thr_dump` 머리와 기록 헤더의 `thr_count`(cub_ctv.c: `h.thr_count = thr_geom.count`)는 위 표의 `count` 를 그대로 쓴다.

거부 사유 문자열의 행방: `thr_probe_entries` 가 돌려주는 `"I-14 manager object"`·`"I-14 entry array"`·`"I-14 out of memory"` 는 `thr_probe` 가 `!= NULL` 비교에만 쓰고 **버린다**. 어디에도 저장되지 않으며 레이아웃 줄·덤프·기록(`tran_refused` 같은 필드)에 나타나지 않는다. 바깥에 보이는 것은 `DEG_THREAD` 비트뿐이다.

### 4.2 `const char *thr_probe_entries(uint64_t manager_obj)`

반환: 성공 NULL(그리고 `base`·`stride`·`off_index` 설정), 실패 시 `"I-14 manager object"`, `"I-14 entry array"`, 또는 `"I-14 out of memory"`.

1. `mobj[4096]` 에 `mem_read_shrink(manager_obj, mobj, 4096, 256)` (4096→2048→1024→512→256). 0 이면 `"I-14 manager object"`. 읽은 길이 `mn` 뒤는 0으로 채움.
2. 창 버퍼 `win` 을 `winn = CS_MIN_SAMPLES * THR_STRIDE_MAX = 262144` 바이트로 할당(실패 → `"I-14 out of memory"`).
3. `off = 0, 8, …, 4088` 에 대해 후보 포인터 `p = rd_u64(mobj, off)`:
   1. `!VALID_PTR(p)` → 다음.
   2. `mem_read(p, win, 2048)` 실패 → 다음.
   3. `io = 0, 4, …, 2044` 중 `rd_i32(win, io) == 1` 인 io 를 모두 `cand[]`(최대 512) 에 모은다. 없으면 다음.
   4. 창 확장 루프(§4.3). 일치하면 `base = p`, `stride`, `off_index = io` 로 설정하고 NULL 반환.
4. 모든 off 실패 → `"I-14 entry array"`.

### 4.3 창 확장 (16KB → 약 66KB → 전체)

창 크기 수열 `step`:

| 회차 | step | 계산 |
|---|---|---|
| 1 | 16384 | 초기값 |
| 2 | 67592 | `16384*4 = 65536` 이 `THR_STRIDE_MAX + THR_INDEX_SCAN + 8 = 67592` 이하이므로 67592 |
| 3 | 262144 | `67592*4 > 67592` 이고 `67592*4 = 270368 ≥ winn` 이므로 `winn` |

일반식: `next = (step*4 > 67592) ? min(step*4, winn) : 67592` (`step*4 < winn` 일 때만 step*4).

각 회차:

1. `have = mem_read_shrink(p, win, step, CS_MIN_SAMPLES * THR_STRIDE_MIN = 1024)`. 0 → 확장 종료(이 p 실패).
2. `thr_match_stride(win, have, cand, ncand, &stride, &io) == 0` → 성공.
3. `have < step`(메모리가 여기서 끝남) 또는 `step >= winn`(전체 창 읽음) → 종료.
4. 확장 조건 `more` 계산. 후보 c 마다(하나라도 참이면 즉시 참):
   - `q2 = cand[c] + 256` 부터 8씩, `q2 + 4 <= have` ∧ `q2 <= cand[c] + 65536` 인 동안 `rd_i32(win, q2) == 2` 이면 참(index 2 가 창 안에서 보임 → 수열이 더 필요).
   - 또는 `cand[c] + THR_STRIDE_MAX + 4 > have`(가장 큰 stride 의 index 2 위치가 아직 창 밖).
5. `more == 0` → 종료. 아니면 다음 step.

결과적으로 1회차(16KB)는 항상 2회차로 확장 가능하고(`have ≤ 16384 < cand+65540`), 2회차(67592)에서는 `cand < 2048` 이라 두 번째 조건이 거짓이므로 8정렬 위치에 값 2 가 보일 때만 전체 창으로 확장한다.

### 4.4 `static int thr_match_stride(win, have, cand, ncand, *stride_out, *io_out)`

조건을 만족하는 (stride, io) 중 **stride 가 가장 작은 것**을 찾아 0, 없으면 −1.

1. `q = 256, 260, …` (4씩), `q + 4 <= have` ∧ `q < THR_STRIDE_MAX + THR_INDEX_SCAN (67584)` 인 동안:
   1. `rd_i32(win, q) != 2` → 다음 q.
   2. 후보 c 마다 `io = cand[c]`, `stride = q − io`. 다음 중 하나면 건너뜀:
      - `stride < 256` ∨ `stride > 65536` ∨ `stride % 8 != 0`
      - `io + THR_TRAN_REL_MAX + 4 > stride` (index+28 의 tran_index 가 엔트리 안에 들지 않음)
      - 이미 best 가 있고 `stride >= best_stride`
      - `3 * stride + 2048 > have` (엔트리 3 의 앞 2KB 까지 창에 없음)
   3. `k = 2, 3` 에 대해 `rd_i32(win, k*stride + io) == k + 1` 이어야 함(엔트리 0=1 은 후보 정의, 엔트리 1=2 는 q 에서 확인). 모두 맞으면 best 갱신.
2. 같은 stride 는 먼저 발견된 (q 가 작은) 쌍이 남는다.

### 4.5 개수 세기 (thr_probe 내부, 1MB 단위 이미지)

변수: `have = 0`, `step = 1MB`, `img = NULL`, `n = 0`.

반복:

1. `img = realloc(img, have + step)` 실패 → 종료.
2. `mem_read(base + have, img + have, step)` 실패 시:
   1. `step > stride` 이면 `step = max(step/2, stride)` 로 줄여 1단계부터 재시도.
   2. `step == stride` 이면(한 엔트리까지 줄었는데 실패) 마지막 부분 엔트리 처리: `rem = (n+1)*stride − have`; `0 < rem < stride` ∧ `n*stride + off_index + 4 <= have`(그 엔트리의 index 는 이미 이미지에 있음) ∧ `mem_read(base+have, img+have, rem) == 0` 이면 `have += rem` 하고 `rd_i32(img, n*stride + off_index) == n+1` 이면 `n++`. 그리고 종료.
3. 성공 시 `have += step`; `n < THR_MAX` ∧ `(n+1)*stride <= have` ∧ `rd_i32(img, n*stride + off_index) == n+1` 인 동안 `n++`.
4. `n >= THR_MAX` 또는 `(n+1)*stride <= have`(수열이 이미지 안에서 끊김) → 종료.
5. `step = 1MB` 로 되돌리고 반복.

`count = n`. `n < CS_MIN_SAMPLES(4)` 이면 강등(§4.1-3). 이 이미지(`count*stride` 이상 크기)가 시작 프로브 이미지이며 I-15~I-20 과 첫 m_id 표에 재판독 없이 쓰인다.

`realloc` 실패(1단계): 이전 `img` 는 그대로 유효하며(realloc 실패는 원 블록을 보존), 루프만 끝낸다. `count = n`(그때까지 확정된 값)이고, `n >= 4` 면 그 `img` 를 시작 이미지로 그대로 쓴다. 이미지가 `count*stride` 보다 짧아지는 일은 없다: `n` 은 `(n+1)*stride <= have` 일 때만(3단계), 또는 부분 엔트리 처리에서 `have += rem` 으로 `have == (n+1)*stride` 가 된 뒤에만(2.2단계) 증가하므로 언제나 `have >= count*stride` 다. 할당 크기(`have + step`)와 `have` 사이의 바이트는 초기화되지 않았지만 모든 검사는 `count` 범위만 읽는다. 첫 회차에서 realloc 이 실패하면 `img == NULL`, `n = 0` → 강등(`free(NULL)`).

### 4.6 슬롯 보조 함수 (ctv_thr_probe.c)

| 함수 | 동작 | 반환 |
|---|---|---|
| `static int thr_entry(int k, uint64_t *addr)` | `0 <= k < count` 확인, `*addr = base + k*stride`, 대상 메모리에서 `*addr + off_index` 4바이트를 읽어 `== k+1` | 0 / −1 |
| `int slot_of_ptr(uint64_t p)` | 산술만: `base != 0` ∧ `p >= base` ∧ `(p−base) % stride == 0` ∧ `(p−base)/stride < count` | 슬롯 / −1 |
| `int thr_index_of_ptr(uint64_t ptr)` | `slot_of_ptr` 후 `thr_entry` 로 실메모리 index 확인 | 슬롯 / −1 (이름과 달리 엔진 index 가 아니라 슬롯) |
| `unsigned char *thr_image(void)` | `count*stride` 바이트를 malloc 하고 `mem_read(base, …)` 1회 | 이미지 / NULL(할당·판독 실패) |

---

## 5. I-15 tran_index

### 5.1 `static int thr_check_tran(const unsigned char *img, int rel)`

상한 `lim = tran_geom.ntran > 0 ? tran_geom.ntran : TRAN_SLOTS_MAX`.

모듈 간 호출 순서(cub_ctv.c `main`): `cs_layout_probe` → `tran_subsystem_init`(03 §9.1: `tran_probe` + 전수 잠금) → `target_pid = pid` → (`thr_skipped` 가 아니면) `thr_keep_image = (o.dump != NULL)`; `thr_probe(…)`. 따라서 `thr_check_tran` 이 처음 돌 때 `tran_geom.ntran` 은 이미 확정되어 있다. 트랜잭션 표가 거부됐거나(`ntran == 0`, 03 §9.2) `log_Gl` 심볼이 없으면 `lim = 65536`. `--dump` 는 `--trans` 와 함께여도 `thr_skipped = TRANS_TABLE_ONLY(...) && o.dump == NULL` 이 거짓이라 `thr_probe` 를 실행한다. 재프로빙(`thr_cache_refresh`)의 `thr_probe_tran` 도 같은 `ntran` 을 쓴다(트랜잭션 쪽은 다시 프로브하지 않음).

1. 모든 슬롯 k(0..count−1)에서 `v = rd_i32(img, k*stride + off_index + rel)`.
2. `v < −1` ∨ `v >= lim` 이면 즉시 0.
3. `neg` = v == −1 인 슬롯 수.
4. `neg * 2 > count`(−1 이 엄격 과반) 이면 1, 아니면 0.

### 5.2 `int thr_find_tran(const unsigned char *img)`

1. `rel ∈ {28, 20}` 순서로, `off_index + rel + 4 <= stride` ∧ `thr_check_tran(img, rel)` 인 것을 센다.
2. 정확히 1개면 그 rel, 아니면 −1.

### 5.3 필드 반영: `static void thr_probe_tran(img)`

`rel = thr_find_tran(img)`; `rel >= 0` 이면 `field_lock(&f_tran, rel, 0)`(SET). 실패 시 상태 변화 없음. f_tran 은 런타임 닻이 없어 한 번 SET 이면 내려가지 않는다.

---

## 6. I-16 m_id 후보

### 6.1 `int thr_id_candidates(int nthr, const unsigned char *img, int *cand, int *strict)`

`nthr` = 프로세스의 OS 스레드 수(`os_task_list` 개수). `cand[]` 는 `THR_ID_CANDS(64)` 칸.

1. `*strict = −1`. 값 버퍼 할당 실패 또는 `nthr < 2` → 0 반환.
2. `off = 0, 8, …` (`off + 8 <= stride` ∧ 후보 수 `n < 64` 인 동안):
   1. 모든 슬롯의 `v = rd_u64(img, k*stride + off)`. 0 은 건너뜀. 비0 이 `!VALID_PTR(v)` 이면 이 off 탈락. 비0 값을 모음(`nz` 개).
   2. `nz < 4` ∨ `nz > nthr + 4*THR_ID_SLACK (nthr+32)` → 탈락.
   3. 정렬 후 인접 중복이 있으면 탈락(전부 서로 달라야 함).
   4. 통과 → `cand[n++] = off`.
   5. 엄격 규칙: `nthr − 1 − 8 <= nz <= nthr − 1` 이면 `*strict` = (처음이면 off, 두 번째부터 −2).
3. 후보 수 n 반환. `*strict`: −1 = 엄격 후보 없음, −2 = 둘 이상(사용 불가), ≥0 = 유일 엄격 후보.

엄격 규칙의 적용 범위(코드 구조):

```c
for (off = 0; off + 8 <= stride && n < THR_ID_CANDS; off += 8) {
  … 탈락 검사(2.1~2.3) …
  if (ok) {                       /* 중복 없음까지 통과한 off 만 */
    cand[n++] = off;
    if (nz <= nthr - 1 && nz >= nthr - 1 - THR_ID_SLACK)
      *strict = nstrict++ == 0 ? off : -2;
  }
}
```

| 항목 | 규칙 |
|---|---|
| 평가 대상 | 2.1~2.3 을 모두 통과해 `cand[]` 에 들어간 off **만**. 탈락한 off 는 nz 가 범위 안이어도 셈하지 않음 |
| 64 개 도달 | `n == 64` 가 되면 `for` 조건에서 루프가 끝나므로 그 뒤 off 는 후보로도 엄격 규칙으로도 평가되지 않는다(65번째 이후의 엄격 후보 때문에 −2 가 되는 일 없음) |
| 할당 | 값 버퍼 `vals` 는 `count × 8` 바이트 malloc, 함수 끝에서 free |

### 6.2 `static void thr_probe_id(int pid, const unsigned char *img)` (I-16 + I-17 결합)

1. `ntask = os_task_list(pid, &tasks)`; `ntask < 2` 이면 tasks 해제 후 반환(발행·표 갱신 없음).
2. `n = thr_id_candidates(ntask, img, cand, &strict)`.
3. 초기값 `best = −1`, `best_n = 0`, `best_tid = 0`, `ties = 0`. 각 후보 `cand[k]`(k 오름차순 = off 오름차순)에 `toff = 0` 으로 `m = thr_probe_tid_at(tasks, ntask, img, cand[k], &toff)`(if / else-if):
   - `m > best_n` → `best = cand[k]`, `best_n = m`, `best_tid = toff`, `ties = 0`.
   - `m > 0 ∧ m == best_n` → `ties++`.
   - `best_n` 이 0 에서 시작하므로 `m == 0` 인 후보는 갱신도 동률도 아니다. 더 큰 m 이 나오면 `ties` 는 0 으로 리셋된다.
4. 결정:
   - `best >= 0 ∧ ties == 0` → `field_lock(&f_id, best, 0)`, `field_lock(&f_tid, best_tid, 0)`.
   - 아니고 `strict >= 0` → `field_lock(&f_id, strict, 0)` (f_tid 는 그대로).
   - 둘 다 아니면 필드 변화 없음.
5. `thr_publish()`.
6. `has_id` 이면 `thr_ids_load(img)`; 이어서 `has_tid` 이면 `thr_tids` 를 (없을 때) calloc 하고 `thr_tids_fill_all()`.
7. `thr_tasks_remember(tasks, ntask)` (기존 목록 해제, 소유권 이전).

---

## 7. I-17 OS tid (struct pthread 안)

### 7.1 `int thr_probe_tid_at(const int *tasks, int ntask, const unsigned char *img, int id_off, int *tid_off)`

반환: 확인된 엔트리 수 n(>0) 과 `*tid_off`, 실패 0.

준비:

1. `idx[]` = 이미지에서 `rd_u64(img, k*stride + id_off) != 0` 인 슬롯 목록, 개수 `live`.
2. `need = max(min(live, ntask − 1) − THR_ID_SLACK, CS_MIN_SAMPLES)`.
3. `live < need` 이거나 할당 실패면 0.

1단계(표본, 1KB 창):

1. `step = max(live / 32, 1)`(정수 나눗셈). `k = 0, step, 2*step, …` (`k < live` ∧ `ns < 32`) 마다 `mid = rd_u64(img, idx[k]*stride + id_off)` — **k 는 `idx[]` 의 순번**(k 번째 실스레드 슬롯)이지 원시 슬롯 번호가 아니다. `mem_read(mid, win + ns*1024, 1024)` 성공 시 `ns++`; 실패해도 k 는 step 만큼 전진한다(대체 표본 없음).
2. `ns >= 4` 일 때만, `off = 0, 4, …, 1020`:
   1. 표본마다 `v = rd_i32(win, s*1024 + off)`. `v <= 0` ∨ `!os_task_known(tasks, ntask, v)` 면 건너뜀(셈 밖).
   2. 이미 본 값과 같으면 이 off 탈락(중복). 아니면 기록, `m++`.
   3. 탈락 없고 `m >= 4` ∧ `m >= ns − 8` 이면 `found++`, `best = off`.
3. `found != 1` 이면 실패(0).

2단계(전 실스레드, gather 1회):

1. `addrs[k] = m_id(idx[k])`, `mem_gather_field(addrs, live, best, 4, NULL, vals, okv)`(실패 → 0).
2. 각 k: `!okv[k]` ∨ `v <= 0` ∨ 알려지지 않은 tid → 건너뜀. 이미 나온 값과 같으면 즉시 실패(두 엔트리가 한 스레드를 가리킴). 아니면 `n++`.
3. `n < need` → 0. 아니면 `*tid_off = best`, n 반환.

### 7.2 tid 표

- `static void thr_tids_fill_all(void)`: `thr_tids != NULL ∧ has_tid` 이면 `mem_gather_field(thr_ids, count, off_tid, 4, NULL, thr_tids, NULL)` — m_id 값 자체를 base 로 쓴다(0·비유효 m_id 슬롯은 0). 실패 엔트리도 0.
- `int thr_tid_of(int slot)`: `thr_tids != NULL ∧ 0 <= slot < count` 이면 `thr_tids[slot]`, 아니면 0.

---

## 8. I-18 resume_status

### 8.1 `int resume_suspended(int v)`

`(3 <= v <= 25 ∧ v 홀수) ∨ v == 26` 이면 1.

### 8.2 `int thr_probe_resume_pass(const FIELD *f, const unsigned char *img, int ignore, int *provisional)`

탐색 범위: `off = off_index + off_tran + 4` 부터 4씩, `off + 4 <= stride`. (`off_tran` 은 발행값: I-15 미확정이면 28.)

오프셋마다:

1. `field_skips(f, off, ignore)` 이면 건너뜀.
2. 모든 슬롯 `v = rd_i32(img, k*stride + off)`: `0 <= v < 28` 이 아니면 이 off 탈락(즉시). 범위 안이고 `thr_slot_live(k)` 이며 처음 보는 값이면 비트마스크에 기록하고 `distinct++`, `hi = max(hi, v)`, `odd |= resume_suspended(v)`.
3. 약한 규칙: `distinct >= 2` ∧ `hi >= 7` ∧ `distinct <= hi/2 + 1`(희소; 0..n 을 채우는 카운터 배제) → `weak++`, `best_weak = off`.
4. 강한 규칙: 약한 규칙 ∧ `distinct >= 3` → `found++`, `best = off`; 그리고 `odd` 면 `found_odd++`, `best_odd = off`.

결정:

| 조건 | 결과 |
|---|---|
| `found > 1 ∧ found_odd == 1` | `best_odd`, `*provisional = 0` |
| `found == 1` | `best`, `*provisional = 0` |
| 그 외, `weak == 1` | `best_weak`, `*provisional = 1` |
| 그 외 | −1 (`*provisional` 미변경; 이전 WEAK 잠금은 유지됨) |

주의: `found > 1` 이고 홀수 결선이 실패하면 `weak >= found > 1` 이므로 −1.

오프셋 1개 안의 판정 순서(코드 구조):

```c
if (field_skips (f, off, ignore)) continue;
for (k = 0; k < count && ok; k++) {           /* 첫 범위 밖 값에서 멈춤 */
  v = rd_i32 (img, k*stride + off);
  ok = v >= 0 && v < 28;
  if (ok && thr_slot_live (k) && !(seen & (1u << v)))
    { seen |= 1u << v; distinct++; hi = max (hi, v); odd |= resume_suspended (v); }
}
if (ok && distinct >= 2 && hi >= 7 && distinct <= hi / 2 + 1) {   /* 약 */
  weak++; best_weak = off;
  if (distinct >= 3) {                                              /* 강 */
    found++; best = off;
    if (odd) { found_odd++; best_odd = off; }
  }
}
```

| 질문 | 답 |
|---|---|
| 약한 규칙을 못 넘은 off 가 강한 규칙을 넘을 수 있나 | 없음. 강한 규칙은 약한 규칙 블록 **안**에서만 검사 |
| `distinct`·`hi`·`odd` 의 대상 | 실스레드(`thr_slot_live`) 슬롯의 **서로 다른** 값만. 비실스레드 슬롯은 범위 검사(`0..27`)에만 참여 |
| `odd` | 실스레드의 서로 다른 값 중 하나라도 `resume_suspended` 이면 1 |
| 초기값 | `found = best = found_odd = best_odd = weak = best_weak = 0`; 오프셋마다 `ok = 1, hi = 0, distinct = 0, odd = 0, seen = 0` |
| 같은 규칙에 여러 off | `best*` 는 마지막(가장 큰) off 로 덮이지만, 결과로 쓰이는 것은 개수가 정확히 1 일 때뿐 |

결정 절차(코드 순서):

1. `found > 1 ∧ found_odd == 1` 이면 `found = 1`, `best = best_odd`.
2. `found != 1` 이면: `weak != 1` → −1, 아니면 `*provisional = 1`, `best_weak` 반환.
3. 아니면 `*provisional = 0`, `best` 반환.

따라서 `found > 1` 이면 `found_odd` 가 0 이든 2 이상이든 1 단계를 넘지 못하고, 2 단계에서 `weak >= found >= 2` 라 항상 −1 이다(약한 규칙으로 되돌아가지 않음).

### 8.3 필드 반영

`static int thr_probe_resume(img)` = `field_probe(&f_resume, thr_probe_resume_pass, img)`. 잠정이면 WEAK, 강한 규칙이면 SET.

---

## 9. I-20 m_status

### 9.1 `int thr_probe_status_pass(const FIELD *f, const unsigned char *img, int ignore, int *provisional)`

1. `*provisional = 0`. `!has_resume` → −1(resume_status 뒤에 프로브).
2. `off = off_index + off_tran + 4` 부터 4씩, `off + 4 <= off_resume`:
   1. 모든 슬롯 `v`: `0 <= v <= 4` 이고, 실스레드 슬롯이면 `v >= 1` 이어야 함(아니면 탈락). `run` = 실스레드 중 `v == 2`(TS_RUN) 수.
   2. 통과 ∧ `run > 0` ∧ `!field_skips(f, off, ignore)` → `found++`, `best = off`.
3. `found == 1` 이면 best, 아니면 −1. (잠정 규칙 없음 → 항상 SET 또는 실패.)

`static int thr_probe_status(img)` = `field_probe(&f_status, thr_probe_status_pass, img)`.

런타임 해석: `st == TS_WAIT(3)` 이면 지금 suspend 중.

---

## 10. CS writer 대기 판정과 I-19 next_wait_thrd

### 10.1 `static int cs_writer_waiting(int status, int code)`

| 발행 상태 | 결과 |
|---|---|
| `has_status` | `status == 3 ∧ (!has_resume ∨ code == 9 ∨ code == 11)` |
| `!has_status` | `!has_resume ∨ (7 <= code <= 12)` |

(둘 다 없으면 항상 참.)

### 10.2 `int buf_cs_writer_waiting(const unsigned char *buf, long base)`

엔트리 사본의 시작이 `buf + base` 에 있다고 보고(이미지면 `k*stride`, 홉 span 이면 `−lo`), `status = has_status ? rd_i32(buf, base+off_status) : −1`, `code = has_resume ? rd_i32(buf, base+off_resume) : −1` 로 `cs_writer_waiting` 호출.

### 10.3 `int thr_next_static(const unsigned char *img, int off)`

1. 모든 슬롯 k 에서 `v = rd_u64(img, k*stride + off)`. 0 은 건너뜀.
2. `j = slot_of_ptr(v)`; `j < 0` ∨ `j == k` ∨ 이미지상 `rd_i32(img, j*stride + off_index) != j+1` 이면 −1(후보 아님).
3. `nz += buf_cs_writer_waiting(img, k*stride) && buf_cs_writer_waiting(img, j*stride)` (양 끝이 모두 CS writer/promoter 대기일 때만 셈; `tran_next_wait` 같은 락 대기 링크 배제).
4. nz 반환(≥0 = 후보, >0 = 유효 링크를 보임).

### 10.4 `static void thr_probe_next(const unsigned char *img)`

1. 기존 후보 `old[] = next_cand[]`, `nold = next_ncand` 저장; `next_ncand = 0`; `field_clear(&f_next)`.
2. `!has_resume` → 반환(후보 0, NONE).
3. `off = (off_resume + 4 + 7) & ~7`(resume 뒤 첫 8정렬) 부터 8씩, `off <= off_resume + 64` ∧ `off + 8 <= stride`:
   - `nz = thr_next_static(img, off)`; `nz < 0` ∨ 후보가 이미 8개면 건너뜀.
   - `next_cand[next_ncand++] = off`; `nz > 0` 이면 `hit = off`, `nhit++`.
4. 후보 목록이 이전과 다르면(개수 또는 내용) `next_votes[]` 전부 0(같으면 표가 유지됨; `next_voted` 는 지우지 않으나 votes 가 0 이면 무의미).
5. `next_ncand > 0` 이면 `field_lock(&f_next, nhit == 1 ? hit : −1, nhit != 1)`:
   - 유일 정적 링크 → SET, off = hit.
   - 그 외 → WEAK, off = −1 ("후보만" 상태).
   - 후보 0 → NONE 유지(강등 `DEG_THR_NEXT`).

### 10.5 홉 투표 (`hop_next` 안, §16.3)

WEAK(후보만) 동안 대기 큐 홉마다: 후보 c 가 가리키는 대상 엔트리가 (읽힘 ∧ 대상 index 확인 ∧ 대상이 CS writer 대기) 인 후보가 **정확히 1개**면 그 링크 `(k<<32) | slot(pick)` 를 그 후보에 1표로 기록. 같은 링크는 다시 세지 않고, 후보당 최대 3표. `next_votes[hit] >= 3` ∧ 다른 모든 후보의 표 합 == 0 이면 `field_lock(&f_next, next_cand[hit], 0)`(SET) 후 `thr_publish()`.

중복 링크 판정 범위: **그 후보(hit)의 유효 칸 `next_voted[hit][0 .. next_votes[hit])` 만** 비교한다. 다른 후보의 링크, 그리고 `next_votes` 가 0 으로 리셋된 뒤 남아 있는 묵은 `next_voted` 칸은 비교 대상이 아니며, 새 표가 그 칸(`next_voted[hit][next_votes[hit]]`)을 덮어쓴다. 따라서 §10.4 4단계에서 `next_voted` 를 지우지 않아도 동작에 영향이 없다.

```c
link = ((uint64_t)(unsigned) k << 32) | (unsigned) slot_of_ptr (pick);
for (u = 0; u < next_votes[hit] && next_voted[hit][u] != link; u++) ;
if (u < next_votes[hit] || next_votes[hit] >= THR_NEXT_VOTES) return pick;
next_voted[hit][next_votes[hit]] = link;
next_votes[hit]++;
others = Σ_{c < next_ncand, c != hit} next_votes[c];
if (next_votes[hit] >= THR_NEXT_VOTES && others == 0)
  { field_lock (&f_next, next_cand[hit], 0); thr_publish (); }
return pick;
```

결과: 어느 후보든 1표라도 받은 다른 후보가 있으면(`others > 0`), 표가 3에 이른 후보도 확정되지 않으며 표는 3 에서 더 늘지 않는다. 이 막힘은 재프로빙이 **다른** 후보 목록을 찾아 `next_votes` 를 리셋할 때까지 지속된다(같은 목록이면 유지).

---

## 11. FIELD 상태기계 (ctv_field.c)

### 11.1 함수

| 함수 | 동작 |
|---|---|
| `void field_lock(FIELD *f, int off, int weak)` | `state == NONE` 이거나 `off` 가 바뀌면 `hits = misses = 0`. `f->off = off`, `state = weak ? WEAK : SET` |
| `void field_clear(FIELD *f)` | `state = NONE` (off·집계·기각 목록 유지) |
| `void field_reject(FIELD *f)` | `state != NONE` 이면 현재 off 를 `rejected[]` 끝에 추가(가득(8)이면 가장 오래된 것을 memmove 로 밀어냄). 그리고 `state = NONE` |
| `int field_skips(const FIELD *f, int off, int ignore)` | `!ignore` 이고 off 가 `rejected[]` 에 있으면 1 |
| `int field_sight(FIELD *f, int ok)` | ok 면 `hits++`, 아니면 `misses++`; `hits + misses >= 64` 이면 둘 다 정수 절반. 반환 `misses >= 4 ∧ misses > hits` (1 = 내려야 함) |
| `int field_probe(FIELD *f, pass, img)` | `off = pass(f, img, 0, &weak)`. `off < 0 ∧ state == NONE ∧ nrejected > 0` 이면 `off = pass(f, img, 1, &weak)`(기각 무시 재기회) 하고 성공 시 `nrejected = 0`. `off >= 0` 이면 `field_lock(f, off, weak)`. 반환 `off >= 0` |

상태 전이:

```text
NONE --field_lock(weak=0)/field_probe 강한 규칙--> SET
NONE --field_lock(weak=1)/field_probe 잠정 규칙--> WEAK
WEAK --field_probe 강한 규칙 / hop 투표 확정------> SET
WEAK --field_probe 실패-------------------------> WEAK (유지)
SET/WEAK --field_clear---------------------------> NONE (기각 기록 없음)
SET/WEAK --field_reject(fields_drop)-------------> NONE (+ off 를 기각 목록에)
```

### 11.2 필드별 사용

| 필드 | 잠금 경로 | 가능한 상태 | field_sight | field_reject | field_clear | field_skips |
|---|---|---|---|---|---|---|
| `f_tran` (I-15) | `thr_probe_tran` → `field_lock(rel,0)` | NONE/SET | 없음 | 없음 | 없음 | 없음 |
| `f_id` (I-16) | `thr_probe_id` → `field_lock(best 또는 strict, 0)` | NONE/SET | 없음 | 없음 | 없음 | 없음 |
| `f_tid` (I-17) | `thr_probe_id` → `field_lock(best_tid,0)` | NONE/SET | 없음 | 없음 | 없음 | 없음 |
| `f_resume` (I-18) | `field_probe(thr_probe_resume_pass)` | NONE/WEAK/SET | `resume_strike`(닻, `thr_wait_counts` 범위 밖 값) | `fields_drop(&f_resume)` | 없음 | pass 내부 |
| `f_status` (I-20) | `field_probe(thr_probe_status_pass)` | NONE/SET | `thr_resume_anchor` | `fields_drop(&f_status)` | `resume_dependents_clear` | pass 내부 |
| `f_next` (I-19) | `thr_probe_next` → `field_lock(hit,0)` 또는 `(−1,1)`; `hop_next` → `field_lock(cand,0)` | NONE/WEAK(off −1)/SET | 없음(투표 별도) | 없음 | `thr_probe_next` 시작, `resume_dependents_clear` | 없음 |

### 11.3 의존 연쇄

- `static void resume_dependents_clear(void)`: `field_clear(&f_status)`, `field_clear(&f_next)` (resume_status 기준으로 찾은 필드; 기각 아님).
- `static void fields_drop(FIELD *f)`: `field_reject(f)`; `f == &f_resume` 이면 `resume_dependents_clear()`; `sched_restart(&reprobe)`(빠른 회차로, 즉시 만기); `thr_publish()`.
- `thr_cache_refresh` 에서 resume 이 새로 잡히거나 오프셋이 바뀌면 `resume_dependents_clear()`(§14.3 11단계).

---

## 12. 발행: `static void thr_publish(void)`

`thr_geom` 의 `has_*`/`off_*` 와 스레드 하위 강등 비트의 **유일한 쓰기 함수**.

1. `has_tran = f_tran.state != NONE`; `off_tran = has_tran ? f_tran.off : 28`.
2. `has_id = f_id.state != NONE`; `off_id = f_id.off`.
3. `has_tid = f_tid.state != NONE`; `off_tid = f_tid.off`.
4. `has_resume = f_resume.state != NONE`(WEAK 포함); `off_resume = f_resume.off`.
5. `has_status = f_status.state != NONE`; `off_status = f_status.off`.
6. `has_next = f_next.state != NONE`(후보만이어도 1); `off_next = (f_next.state == SET) ? f_next.off : −1`.
7. `base == 0` 이면 반환(DEG_THREAD 가 대신함).
8. `deg_set(DEG_THR_SUB, 0)` 후, 없는 것만 `deg_set(…, 1)`: `!has_tran→DEG_THR_TRAN`, `!has_id→DEG_THR_ID`, `!has_tid→DEG_THR_TID`, `!has_resume→DEG_THR_RESUME`, `!has_next→DEG_THR_NEXT`, `!has_status→DEG_THR_STATUS`.

---

## 13. 런타임 닻

### 13.1 `static void resume_strike(int ok)`

`field_sight(&f_resume, ok)` 가 1 이면 `fields_drop(&f_resume)`.

### 13.2 `static void thr_resume_anchor(int v, int st)`

`v` = 머리 엔트리의 resume_status, `st` = 같은 판독의 m_status(없으면 −1).

1. `!has_resume` → 반환.
2. `resume_strike(7 <= v <= 12)`.
3. 그 뒤의 발행값으로 `has_status ∧ (st == TS_WAIT ∨ resume_suspended(v))` 이면 `field_sight(&f_status, st == TS_WAIT)`; 1 이면 `fields_drop(&f_status)`. (resume 이 막 내려가면 연쇄로 has_status=0 이 되어 이 단계는 생략된다. 이미 풀려나 resumed 코드인 머리 ∧ st≠3 은 셈하지 않음.)

### 13.3 닻이 셈하는 목격

`thr_waiters_mem` 의 첫 홉(n == 0)에서만, 다음이 모두 참일 때 `thr_resume_anchor` 호출:

- 호출자가 `anchor_at != 0` 을 줌(머리를 읽어 온 CS 필드 주소: `cs_base + i*cs_stride + field`, field = promoter 큐가 비0이면 `off_pq` 아니면 `off_wq`). 샘플러가 **새 머리**를 걸을 때만 넘기고, 화면 프레임 구성은 0.
- `has_resume`.
- 머리 엔트리를 `hop_read` 로 읽은 **뒤** `mem_read(anchor_at, &still, 8) == 0 ∧ still == first`(그 CS 필드가 아직 같은 머리를 가리킴). 그 사이 머리가 서비스되어 다른 곳에 줄 섰을 수 있으므로 재확인한다.
- 닻 판정은 체인 걷기가 그 머리를 기각하더라도 수행(잘못된 오프셋이면 모든 머리가 기각되므로).

그 밖의 resume 목격: `thr_wait_counts` 에서 읽힌 실스레드 값이 0..27 밖이면 `resume_strike(0)`.

"새 머리" 의 정확한 정의 (ctv_sampler.c, CS 배열 i 마다, 표본 1회당):

| 단계 | 내용 |
|---|---|
| 진입 조건 | 이번 표본 이미지에서 `wr != 0 ∨ ww != 0` |
| 머리 | `head = cs_wait_queues(blob, lo, i, &head2, &field)`: `pq = rd_u64(off_pq)`, `wq = rd_u64(off_wq)`; `head = pq ? pq : wq`, `head2 = pq ? wq : 0`, `field = pq ? off_pq : off_wq` |
| `fresh` | `head != 0 ∧ head != w->wq_last[i]` |
| 걷기 게이트 | `every(&w->ch_walked[i], mono_s(), fresh ? OS_SAMPLE_MIN_S(0.05) : CHAIN_REWALK_S(0.25))` 가 참일 때만 걷는다 |
| 닻 인자 | `thr_waiters_mem2(head, head2, …, THR_WAITERS_MAX, fresh ? at : 0)`, `at = cs_base + i*lo->stride + field` |
| `wq_last` 갱신 | 걸은 뒤 `wq_last[i] = ch_n[i] > 0 ? head : 0`. `head == 0` 이거나 `wr == ww == 0` 인 표본에서는 `wq_last[i] = 0` |

귀결: 체인 걷기가 홉을 하나도 받지 못하면(`ch_n == 0`, 예: 머리가 `buf_cs_writer_waiting` 에서 기각됨) `wq_last` 가 0 이 되므로 같은 머리가 다음 표본에서도 다시 `fresh` 이고, 0.05초마다 닻이 다시 셈한다. 게이트(`every`)에 막힌 표본은 걷지 않으므로 `wq_last` 도 바뀌지 않는다. 화면 프레임 구성(ctv_frame.c `chain_of`)은 항상 `anchor_at = 0` 이다.

---

## 14. 초기 프로브 순서와 재프로빙 일정

### 14.1 `thr_probe` 의 필드 단계(개수 세기 성공 후)

1. `thr_probe_tran(img)`; `thr_publish()`.
2. `thr_probe_id(pid, img)` (내부 발행·m_id 표·tid 표·task 목록 기억).
3. `has_id` 이면: `thr_probe_resume(img)`; `thr_publish()`; `thr_probe_status(img)`; `thr_publish()`. (resume/status 는 실스레드 판별에 m_id 표가 필요.)
4. `thr_probe_next(img)`; `thr_publish()`.
5. `thr_keep_image` 면 기존 `thr_kept` 해제 후 `thr_kept = img`, 아니면 해제.
6. `sched_start(&reprobe, (time_t) mono_s())`; `thr_publish()`.

### 14.2 SCHED (ctv_sched.c)

| 함수 | 동작 |
|---|---|
| `sched_start(s, now)` | `rounds = 0`, `gap = 2`, `last = now`, `at = now + 2` |
| `sched_due(s, now)` | `now >= at` |
| `sched_ran(s, now)` | `rounds++`; `gap = rounds > 6 ? 300 : (gap == 0 ? 2 : gap < 64 ? gap*2 : 64)`; `at = now + gap`; `last = now` |
| `sched_restart(s)` | `rounds = gap = 0`, `at = 0`(즉시 만기) |
| `sched_slow(s)` | `rounds > 6` |
| `sched_sooner(s, min_gap)` | `at > last + min_gap` 이면 `at = last + min_gap` |
| `every(&last, now, gap)` | `now − last < gap` 이면 0, 아니면 `last = now` 하고 1 |

`sched_ran` 의 코드: `s->gap = ++s->rounds > 6 ? 300 : s->gap == 0 ? 2 : s->gap < 64 ? s->gap * 2 : 64;` — `rounds` 를 **먼저** 올린 뒤 비교한다.

간격 수열(시도 = `sched_ran` 호출, 표의 간격 = 그 시도 뒤 다음 만기까지):

| 기점 | 첫 만기 | 1회 뒤 | 2 | 3 | 4 | 5 | 6 | 7회 이후 |
|---|---|---|---|---|---|---|---|---|
| `sched_start`(rounds 0, gap 2) | +2초 | 4 | 8 | 16 | 32 | 64 | 64 | 300 |
| `sched_restart`(rounds 0, gap 0) | 즉시(`at = 0`) | 2 | 4 | 8 | 16 | 32 | 64 | 300 |

- 시작 기점: 대기 간격 2, 4, 8, 16, 32, 64, 64 다음 300 (64 가 두 번: 5회째 `32*2 = 64`, 6회째 `gap < 64` 거짓 → 64).
- 재시작 기점: 즉시, 이어 2, 4, 8, 16, 32, 64 다음 300 (64 는 **한 번**: 0 → 2 로 한 단계를 쓰므로).
- 어느 쪽이든 7번째 `sched_ran` 에서 `rounds = 7 > 6` 이 되어 간격 300, 그 시점부터 `sched_slow` 참.

### 14.3 `void thr_cache_refresh(void)` (샘플러·프레임 구성에서 호출)

1. `now = (time_t) mono_s()` (단조 초). `base == 0` 또는 `!every(&thr_ids_at, now, 1.0)` → 반환(초당 1회; `thr_ids_load` 도 `thr_ids_at` 을 갱신).
2. `ntask = os_task_list(target_pid, &tasks)`; `ntask < 0` → 반환.
3. `waits_settled = (f_resume.state == SET) ∧ (f_status.state != NONE)`.
4. `settled = waits_settled ∧ ((f_tran.state ∧ f_id.state ∧ f_tid.state 모두 비NONE) ∨ sched_slow(&reprobe))` — 구조 필드(I-15~I-17)는 빠른 회차 동안만 재시도.
5. task 목록 비교(`ntask == thr_ntasks ∧ thr_tasks != NULL ∧ memcmp 동일`):
   - 같음: tasks 해제. `settled ∨ !sched_due` 면 반환(판독 0).
   - 다름: `thr_tasks_remember(tasks, ntask)`; `sched_slow ∧ !waits_settled` 면 `sched_sooner(&reprobe, 60)`.
6. `settled ∨ !sched_due(&reprobe, now)` 이면: `has_id` 일 때 `thr_tables_reload()`(gather 2회) 후 반환.
7. 재프로빙: `sched_ran(&reprobe, now)`; `img = thr_image()`(실패 시 반환).
8. `f_tran == NONE` → `thr_probe_tran(img)`.
9. `f_id == NONE ∨ f_tid == NONE` → `thr_probe_id(target_pid, img)`(task 목록을 다시 읽어 기억); 아니면 `thr_ids_load(img)` + `thr_tids_fill_all()`.
10. `thr_publish()`.
11. `has_id` 이면:
    1. `was = (f_resume != NONE) ? f_resume.off : −1`.
    2. `f_resume != SET` → `thr_probe_resume(img)`.
    3. `f_resume != NONE ∧ f_resume.off != was` → `resume_dependents_clear()`(새로 찾음 또는 이동).
    4. `thr_publish()`.
    5. `f_status == NONE` → `thr_probe_status(img)`; `thr_publish()`.
    6. `f_next != SET` → `thr_probe_next(img)`; `thr_publish()`.
12. 이미지 해제.

요점: 같은 스레드 구성이면 확정 후 대상 판독 0. 구성이 바뀌면 m_id·tid 표만 gather 2회로 재구성. 미확정(또는 닻 강등 후)일 때만 일정에 따라 배열 이미지 1회.

---

## 15. m_id 표·정렬 색인·tid 표

### 15.1 `static int thr_gather_field(int live_only, int off, size_t width, void *out, unsigned char *ok)`

정적 `bases[k] = base + k*stride`(최초 1회 계산), 정적 `skip[]`. `live_only` 면 `skip[k] = (thr_ids == NULL ∨ thr_ids[k] == 0)`. `mem_gather_field(bases, count, off, width, live_only ? skip : NULL, out, ok)` 반환(할당 실패 −1).

### 15.2 `static int thr_ids_load(const unsigned char *img)`

1. `thr_ids` 없으면 calloc(count)(실패 −1).
2. `img != NULL` → `thr_ids[k] = rd_u64(img, k*stride + off_id)`; `img == NULL` → `thr_gather_field(0, off_id, 8, thr_ids, NULL)`(못 읽은 칸 0).
3. `thr_sorted_rebuild()`; `thr_ids_at = mono_s()`; 0 반환.

### 15.3 `static void thr_sorted_rebuild(void)`

`thr_nsorted = 0` 으로 시작. `thr_sorted` 를 (없으면) count 칸 malloc(실패 시 `thr_nsorted = 0` 인 채 반환, 다음 호출에서 재시도). k 오름차순으로 `thr_ids[k] != 0` 인 (m_id, k) 를 채우고 `qsort(thr_sorted, thr_nsorted, sizeof(THR_SORTED), cmp_sorted)`. `cmp_sorted` 는 m_id 만 비교(`x < y ? -1 : x > y`)하며 slot 은 비교하지 않는다.

중복 m_id(동률) 규칙: **없음.** 중복 제거도, 동률 시 slot 순 정렬도 하지 않는다. I-16 프로브는 후보 오프셋의 비0 값이 모두 다를 것을 요구하지만(§6.1), 이후 재구성(`thr_ids_load`, 특히 gather 판독 `img == NULL`)은 중복을 검사하지 않으므로 찢긴 판독·pthread_t 재사용 시 같은 m_id 가 두 슬롯에 있을 수 있다. 그때 같은 키들의 상대 순서는 libc `qsort` 의 구현에, `thr_index_of_tid_mem` 이 그중 어느 원소를 돌려주는지는 libc `bsearch` 의 구현에 달려 있다. 바이트 동일 재구현이 필요하면 원본과 같은 libc(정적 링크된 것)의 `qsort`/`bsearch` 를 같은 인자(원소 크기 16, 위 비교 함수)로 호출해야 한다. 중복이 없으면 결과는 유일하다.

### 15.4 `static void thr_tables_reload(void)`

`thr_ids_load(NULL) == 0` 이면 `thr_tids_fill_all()`. (m_id 8B × count gather 1회 + tid 4B × 실스레드 gather 1회.)

### 15.5 조회

| 함수 | 동작 | 반환 |
|---|---|---|
| `int thr_index_of_tid_mem(uint64_t tid)` | `thr_sorted == NULL ∨ tid == 0 ∨ thr_nsorted == 0` → −1; 아니면 m_id 로 bsearch | 슬롯 / −1. 인자 tid 는 pthread_t(m_id) 값(CS owner 필드 등) |
| `int thr_slot_live(int k)` | `thr_ids != NULL ∧ thr_ids[k] != 0` | 0/1 |
| `int thr_tid_of(int slot)` | §7.2 | OS tid / 0 |

슬롯→tran_index 조회 함수(`thr_tran_of`)는 이 모듈에 없다. 대기 홉의 tran_index 는 `thr_waiters_mem` 의 `tran[]` 출력으로 같은 판독에서 얻고, 슬롯→tran 질의는 프레임 모듈(`view_tran_of`)이 그 값으로 답한다.

---

## 16. 대기 체인

### 16.1 `static int hop_span(int *lo)`

한 홉에서 필요한 엔트리 구간 [lo, hi).

1. `lo = off_index`; `hi = off_index + 4`.
2. `has_status` → `hi = max(hi, off_status + 4)`.
3. `has_resume` → `hi = max(hi, off_resume + 4)`.
4. `has_tran` → `hi = max(hi, off_index + off_tran + 4)`.
5. `has_next` → `last = off_next >= 0 ? off_next : next_cand[next_ncand − 1]`; `hi = max(hi, last + 8)`.
   - 불변식(코드가 따로 검사하지 않음): `f_next.state == WEAK ⇒ next_ncand > 0`. 근거: f_next 를 NONE 아닌 상태로 만드는 경로는 (a) `thr_probe_next` — 시작에서 `next_ncand = 0`·`field_clear` 후 `next_ncand > 0` 일 때만 `field_lock`, (b) `hop_next` — `nhit == 1` 이려면 `next_ncand ≥ 1`, 둘뿐이다. `next_ncand` 를 바꾸는 곳은 `thr_probe_next` 하나이고, 그 함수는 시작에서 f_next 를 NONE 으로 만든다. `thr_cache_refresh` 는 `f_next == SET` 이면 `thr_probe_next` 를 부르지 않으며, SET 이면 `off_next >= 0` 이라 `next_cand` 를 보지 않는다. 따라서 `has_next ∧ off_next < 0` 일 때 `next_ncand − 1 >= 0` 이 항상 성립한다.
6. `hop_buf`(stride), `hop_tbuf`(stride × 8) 지연 할당; 하나라도 없으면 −1(다음 호출에 재시도).
7. `hi − lo` 반환.

### 16.2 `static int hop_read(int k, unsigned char *buf, int lo, int len)`

`mem_read(base + k*stride + lo, buf, len) == 0 ∧ rd_i32(buf, off_index − lo) == k + 1` 이면 0, 아니면 −1.

### 16.3 `static uint64_t hop_next(int k, const unsigned char *buf, int lo, int len)`

반환: 다음 대기자 엔트리 포인터, 0 = 끝/알 수 없음.

1. `!has_next` → 0.
2. `off_next >= 0`(SET) → `rd_u64(buf, off_next − lo)` 를 그대로 반환(대상 대기 여부는 다음 홉에서 확인).
3. WEAK(후보만):
   1. 후보 c 마다 `tj[c] = slot_of_ptr(rd_u64(buf, next_cand[c] − lo))`, `addrs[c] = (tj[c] >= 0 ∧ tj[c] != k) ? base + tj[c]*stride + lo : 0`.
   2. `mem_gather_ok(addrs, next_ncand, len, hop_tbuf, tok)` — 후보 대상들의 span 을 한 번에.
   3. 후보 c 가 `tok[c] ∧ rd_i32(tb, off_index − lo) == tj[c] + 1 ∧ buf_cs_writer_waiting(tb, −lo)` 이면 `pick = 그 포인터`, `hit = c`, `nhit++`.
   4. `nhit != 1` → 0.
   5. 투표(§10.5): `link = (k << 32) | slot_of_ptr(pick)`. 이미 기록된 링크거나 `next_votes[hit] >= 3` 이면 pick 반환. 아니면 기록·`next_votes[hit]++`; 다른 후보 표 합 `others`; `votes >= 3 ∧ others == 0` → `field_lock(&f_next, next_cand[hit], 0)` + `thr_publish()`. pick 반환.

### 16.4 `int thr_waiters_mem(uint64_t head, int *idx, int *tran, int max, uint64_t anchor_at)`

반환: 저장한 홉 수 n(≤ max). `idx[]` 는 **슬롯** 번호, `tran[]`(NULL 허용) 은 각 홉의 tran_index(I-15 없으면 −1).

1. `first = head`; `len = hop_span(&lo)`; `buf = hop_buf`.
2. `head != 0 ∧ n < max ∧ len > 0` 인 동안:
   1. `k = slot_of_ptr(head)`; `k < 0` ∨ `hop_read(k, buf, lo, len) != 0` → 중단.
   2. 닻(§13.3 조건, n == 0 일 때만): `thr_resume_anchor(rd_i32(buf, off_resume − lo), has_status ? rd_i32(buf, off_status − lo) : −1)`; 이어 `len = hop_span(&lo)`(닻이 필드를 내렸을 수 있음), `len <= 0` 이면 중단.
   3. `!buf_cs_writer_waiting(buf, −lo)` → 중단(풀려나 다른 것에 줄 선 스레드 등).
   4. `k` 가 `idx[0..n)` 에 있으면 n 반환(순환 = 찢긴 판독).
   5. `tran[n] = has_tran ? rd_i32(buf, off_index + off_tran − lo) : −1`.
   6. `idx[n++] = k`; `head = hop_next(k, buf, lo, len)`.
3. n 반환.

### 16.5 `int thr_waiters_mem2(uint64_t head, uint64_t head2, int *idx, int *tran, int max, uint64_t anchor_at)`

엔진이 깨우는 순서(promoter 큐 → writer 큐)의 한 체인.

1. `n = thr_waiters_mem(head, idx, tran, max, anchor_at)`.
2. `head2 != 0 ∧ n < max` 이면 `m = thr_waiters_mem(head2, idx+n, tran ? tran+n : NULL, max−n, 0)`(두 번째 큐는 닻 없음).
3. 두 번째 부분의 k 번째 슬롯이 첫 부분에 있으면 `m = k` 로 잘라냄(양 큐에 같은 스레드 = 찢긴 판독).
   - k 는 0 부터 **오름차순**으로 보고 처음 걸린 k 에서 `break` → 가장 작은 k. 비교 범위는 첫 부분 `idx[0..n)` 뿐이다(두 번째 부분 안의 중복은 `thr_waiters_mem` 자신의 순환 검사가 이미 막음).
   - 잘라내기는 걷기가 **끝난 뒤** 출력 개수만 줄인다. 잘린 꼬리를 걷는 동안 생긴 부작용 — `hop_next` 의 투표(§10.5)·그로 인한 `f_next` 확정과 `thr_publish` — 은 되돌리지 않고 유효하다. `idx[n+m..]`·`tran[n+m..]` 칸에 쓰인 값도 남아 있지만 반환 개수 밖이라 읽히지 않는다.
   - 두 번째 큐는 `anchor_at = 0` 이므로 닻 부작용은 없다.
4. `n + m` 반환.

호출 규약: `head` 는 promoter 큐가 비0이면 그것, 아니면 writer 큐; `head2` 는 promoter 큐가 비0일 때의 writer 큐(아니면 0) — `cs_wait_queues` 가 만든다. `max` 는 `THR_WAITERS_MAX`(64).

---

## 17. 대기 사유 집계: `int thr_wait_counts(int *cnt, int *live, int reader_cap)`

`cnt[]` 는 `THR_RESUME_MAX`(28) 칸. `reader_cap` = 호출자가 넘기는 CS 배열 전체 waitR(`wr`) 합.

1. `cnt[0..27] = 0`, `*live = 0`.
2. 다음이면 −1: `!has_resume`, `thr_ids == NULL`, 정적 버퍼(vals, st, okv, oks; count 크기) 할당 실패, `thr_gather_field(1, off_resume, 4, vals, okv) != 0`, `has_status ∧ thr_gather_field(1, off_status, 4, st, oks) != 0`. (live_only=1: m_id 0 슬롯은 읽지 않음.)
3. 슬롯 k 마다:
   1. `thr_ids[k] == 0` ∨ `!okv[k]` ∨ `(has_status ∧ !oks[k])` → 건너뜀(실스레드로도, 표로도 세지 않음).
   2. `v < 0 ∨ v >= 28` → `resume_strike(0)`, 건너뜀.
   3. `(*live)++`.
   4. `v != 7` 이고, `has_status` 면 `st[k] == 3 ∧ resume_suspended(v)`, 아니면 `resume_suspended(v)` 일 때 `cnt[v]++`.
4. `cnt[7] = reader_cap` (CS reader 대기는 TS_WAIT 없이 기다리고 깨어난 뒤에도 7 을 남기므로 스레드 값으로 세지 않고 CS 큐 합으로 대체).
5. `has_resume ? 0 : −1` 반환(집계 도중 strike 로 resume 이 내려갔으면 −1).

`has_status`·`has_resume` 은 루프 안에서 매번 발행값을 읽는다(중간 강등 반영).

집계 도중 strike 로 f_resume 이 내려간 뒤의 동작(코드 그대로):

| 항목 | 동작 |
|---|---|
| 루프 | **계속 돈다**(break 없음). 남은 슬롯도 이미 gather 한 `vals`(옛 `off_resume` 에서 읽은 값)로 처리 |
| `has_status` | `fields_drop(&f_resume)` → `resume_dependents_clear()` 가 f_status 를 NONE 으로 → `thr_publish()` 로 `has_status = 0`. 이후 슬롯은 `oks` 검사를 하지 않고 `cnt[v]++` 조건도 `resume_suspended(v)` 단독 분기로 바뀐다 |
| 추가 범위 밖 값 | 매번 `resume_strike(0)` → `field_sight(&f_resume, 0)` 이 NONE 상태 필드의 `misses` 를 계속 올린다. 1 을 반환하면 다시 `fields_drop`: `field_reject` 는 state 가 NONE 이라 기각 목록에 아무것도 추가하지 않고, `resume_dependents_clear`·`sched_restart`(rounds = gap = at = 0, 멱등)·`thr_publish` 가 반복된다 |
| 남는 영향 | 추가 drop 은 첫 drop 과 같은 상태를 다시 만들 뿐이다. NONE 동안 쌓인 `hits`/`misses` 는 다음 `field_lock` 이 (state == NONE 이므로) 0 으로 리셋하므로 이후 판정에 영향이 없다 |
| `cnt`, `*live` | 끝까지 채워지고, 마지막에 `cnt[7] = reader_cap` 도 실행된다. 반환은 −1 |

---

## 18. 덤프: `void thr_dump(FILE *f)`

1. 이미지: `thr_kept` 가 있으면 그것, 없고 `base != 0` 이면 `thr_image()`, 아니면 NULL.
2. 머리 객체(한 줄, 키 순서 고정):

```text
"threads":{"available":<true|false>,"base":"0x<hex>","stride":<d>,"count":<d>,"off_index":<d>,"off_id":<d>,"has_tran":<d>,"has_id":<d>,"has_tid":<d>,"off_tid":<d>,"has_resume":<d>,"off_resume":<d>,"has_next":<d>,"off_next":<d>,"off_tran":<d>,"has_status":<d>,"off_status":<d>,"entries":[\n
```

`available` 은 이미지 유무.
3. 슬롯 k 마다(이미지 있을 때):
   - `tid = has_id ? rd_u64(img, k*stride + off_id) : 0`
   - `tran = has_tran ? rd_i32(img, k*stride + off_index + off_tran) : −1`
   - `next = off_next >= 0 ? rd_u64(img, k*stride + off_next) : 0`; `nk = next ? thr_index_of_ptr(next) : −1`(실메모리로 index 확인)
   - `tid == 0 ∧ tran == −1 ∧ next == 0` 이면 생략.
   - 출력: `{"slot":k,"index":<img의 index>,"tid":"0x<hex>","tran":<d>,"next_wait":"0x<hex>","next_wait_index":<nk>=0 ? nk+1 : -1>` (첫 항목 앞 구분자 없음, 이후 `,`).
   - `has_tid ∧ tid != 0 ∧ thr_ids != NULL ∧ thr_ids[k] == tid` 이면 `,"os_tid":<thr_tid_of(k)>,"comm":<json 문자열>` (comm 은 `os_task_comm(target_pid, thr_tid_of(k), buf, OS_COMM_LEN=16)` 의 결과 버퍼를 `json_fstr` 로; 반환값은 검사하지 않음).
   - `has_resume` 이면 `,"resume_status":<img 값>`.
   - `}\n`.
4. `]}` 출력. 이미지가 `thr_kept` 가 아니면 해제.

바이트 단위 형식 (printf 그대로):

```c
fprintf (f, "\"threads\":{\"available\":%s,\"base\":\"0x%llx\","
         "\"stride\":%d,\"count\":%d,\"off_index\":%d,\"off_id\":%d,"
         "\"has_tran\":%d,\"has_id\":%d,\"has_tid\":%d,\"off_tid\":%d,"
         "\"has_resume\":%d,\"off_resume\":%d,\"has_next\":%d,"
         "\"off_next\":%d,\"off_tran\":%d,\"has_status\":%d,"
         "\"off_status\":%d,\"entries\":[\n", …);
/* 항목 */
fprintf (f, "%s{\"slot\":%d,\"index\":%d,\"tid\":\"0x%llx\","
         "\"tran\":%d,\"next_wait\":\"0x%llx\",\"next_wait_index\":%d",
         first ? "" : ",", k, rd_i32 (img, e), tid, tran, next,
         nk >= 0 ? THR_INDEX (nk) : -1);
if (has_tid && tid != 0 && thr_ids != NULL && thr_ids[k] == tid)
  { os_task_comm (target_pid, thr_tid_of (k), comm, 16);
    fprintf (f, ",\"os_tid\":%d,\"comm\":", thr_tid_of (k)); json_fstr (f, comm); }
if (has_resume) fprintf (f, ",\"resume_status\":%d", rd_i32 (img, k*stride + off_resume));
fprintf (f, "}\n");
first = 0;
/* 끝 */
fprintf (f, "]}");
```

| 항목 | 규칙 |
|---|---|
| 머리 | 한 줄, 공백 없음, `"entries":[` 뒤 `\n` |
| 구분자 | `first` 플래그: 실제로 **출력된** 첫 항목 앞은 빈 문자열, 이후 항목 줄 머리에 `,`. 생략된 슬롯은 `first` 를 바꾸지 않는다 |
| 항목 끝 | 모든 항목은 `}\n` |
| 끝 | `]}` — **뒤에 줄바꿈 없음**(호출자 `detail_dump_write` 가 이어서 `",\n"` 출력) |
| 16진 | `base`·`tid`·`next_wait` 모두 `0x%llx`: 소문자, 0 채움 없음, 0 이면 `0x0` |
| `e` | `k*stride + off_index`; `index` 는 이미지의 그 위치 int32 |
| `os_tid` | `thr_tid_of(k)`(`thr_tids` 없음·범위 밖이면 0) |
| `comm` | `os_task_comm` 실패(예: tid 0, 스레드 종료)면 버퍼가 `""` 로 채워져 `"comm":""`. 내용은 `json_fstr` 규칙(03 §6.7 표: `"`·`\` 백슬래시, `< 0x20` → `\u%04x` 소문자 4자리, 무효 UTF-8 → 바이트마다 6글자 `\ufffd`) |
| 이미지 없음 | `available:false` 이고 루프 조건(`img != NULL`)이 거짓 → `…"entries":[\n]}` |
| `thr_kept` 와 발행값 | `thr_kept` 는 시작 프로브 이미지, `has_*`/`off_*` 와 `thr_ids` 는 덤프 시점 값이다(`os_tid` 는 `thr_ids[k] == tid` 일 때만) |

---

## 19. 공개 함수 요약 (ctv.h)

| 시그니처 | 파일 | 반환·부작용 |
|---|---|---|
| `const char *thr_probe_entries(uint64_t manager_obj)` | probe | NULL/거부 문자열; base·stride·off_index |
| `int slot_of_ptr(uint64_t p)` | probe | 슬롯/−1, 부작용 없음 |
| `int thr_index_of_ptr(uint64_t ptr)` | probe | 슬롯/−1, 4B 판독 |
| `unsigned char *thr_image(void)` | probe | malloc 이미지/NULL |
| `int thr_find_tran(const unsigned char *img)` | probe | 28/20/−1 |
| `int thr_id_candidates(int nthr, const unsigned char *img, int *cand, int *strict)` | probe | 후보 수, `*strict` |
| `int thr_probe_tid_at(const int *tasks, int ntask, const unsigned char *img, int id_off, int *tid_off)` | probe | 확인 수/0 |
| `int resume_suspended(int v)` | probe | 0/1 |
| `int thr_probe_resume_pass(const FIELD *f, const unsigned char *img, int ignore, int *provisional)` | probe | 오프셋/−1 |
| `int thr_probe_status_pass(const FIELD *f, const unsigned char *img, int ignore, int *provisional)` | probe | 오프셋/−1 |
| `int buf_cs_writer_waiting(const unsigned char *buf, long base)` | probe | 0/1 |
| `int thr_next_static(const unsigned char *img, int off)` | probe | −1/유효 링크 수 |
| `int thr_slot_live(int k)` | thr | 0/1 |
| `int thr_tid_of(int slot)` | thr | tid/0 |
| `int thr_wait_counts(int *cnt, int *live, int reader_cap)` | thr | 0/−1; resume strike 가능 |
| `void thr_probe(uint64_t manager_sym, int pid)` | thr | 전 필드 초기 프로브, DEG 비트, SCHED 시작 |
| `void thr_cache_refresh(void)` | thr | 초당 1회 표 갱신·재프로빙 |
| `int thr_index_of_tid_mem(uint64_t tid)` | thr | 슬롯/−1 |
| `int thr_waiters_mem(uint64_t head, int *idx, int *tran, int max, uint64_t anchor_at)` | thr | 홉 수; 닻·투표 부작용 |
| `int thr_waiters_mem2(uint64_t head, uint64_t head2, int *idx, int *tran, int max, uint64_t anchor_at)` | thr | 홉 수 |
| `void thr_dump(FILE *f)` | thr | JSON 조각 출력 |
| 전역 `THR_GEOM thr_geom`, `int thr_keep_image` | thr | — |

---

## 20. 재구현 시 주의점

| 항목 | 내용 |
|---|---|
| 오프셋 기준 | `off_tran` 만 index 상대. resume/status 탐색 시작은 `off_index + off_tran + 4`(I-15 미확정이면 +28 가정) |
| 실스레드 판별 | resume/status 프로브는 m_id 표에 의존하므로 `has_id` 일 때만 수행. 표가 없으면 `thr_slot_live` 는 항상 0 |
| WEAK 의 의미 | f_resume WEAK = 잠정 규칙, `has_resume` 은 1. f_next WEAK = 후보만(off −1), `has_next` 1, `off_next` −1 |
| 표 재사용 | 투표(`next_votes`)는 재프로빙이 같은 후보 목록을 찾으면 유지 |
| 판독 원자성 | 홉·머리 판독은 샘플과 원자적이지 않음: 닻은 머리 재확인 후에만 셈, 단발 불일치는 `field_sight` 감쇠·임계(4회 ∧ 다수)로 흡수 |
| 이미지 크기 | 시작 이미지는 개수 세기 과정에서 count*stride 보다 클 수 있으나 모든 검사는 count 범위만 본다 |
