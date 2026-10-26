# 지표 정의 (Metrics)

화면에 나오는 숫자가 정확히 무엇인지 정의합니다.

## 프로세스 단위

### RSS (Resident Set Size) — 프로세스·인스턴스·티어·합계에 쓰는 값
프로세스가 물리 메모리에 올려둔 양(top 의 RES). 분석 대상 서버는 이번 프레임 `smaps` 의 Rss 합,
그 밖의 CUBRID 프로세스(다른 서버·PL·master·broker·CAS)는 `status:VmRSS`(인스턴스 목록) 또는
`stat` 의 24번째 필드(티어)입니다. 페이지 테이블을 걷지 않아 CAS 가 수천 개여도 프로세스당 수 µs 이고,
매 프레임 그때의 값입니다.

**주의**: 공유 페이지(libcubrid.so 등)를 프로세스마다 계상합니다. 그래서 RSS 합(티어 All, `total.cubrid_rss_bytes`)은
top·ps 로 더한 값과 같고, 실제 점유의 **상한**입니다.

> 예: 라이브러리 10MB를 CAS 5개가 공유 → RSS 합산은 50MB(중복), PSS 합산은 10MB.

### PSS (Proportional Set Size) — 분석 대상 서버에만
공유 페이지를 **공유 프로세스 수로 나눠 배분한** 상주량. 분석 대상 서버의 상세(`server.pss_kb`, 시계열 `server(PSS)`,
영역 합계 라벨)에만 씁니다. CAS·PL 같은 주변 프로세스는 드릴다운 대상이 아니라 PSS 를 내지 않습니다.

### VmSize / mapped
가상 주소 공간에 매핑된 양. 물리 메모리 사용과 무관합니다. 예약만 하고 안 쓰는 영역(glibc arena 3.3G)이 여기 잡힙니다.

## 영역 단위

### 할당(alloc / mapped)
해당 영역이 OS에 매핑해둔 크기. `smaps`의 영역 주소 범위(`end - start`).

### 상주(rss)
그 영역 중 실제 물리 메모리에 있는 양. `smaps`의 영역별 `Rss`.

### 상주율 = 상주 / 할당
낮으면 **예약만 하고 안 쓰는 영역**입니다. 대표 사례:

| 영역 | 할당 | 상주 | 상주율 | 해석 |
|---|---|---|---|---|
| `thread stacks` | 368.1M | 796K | 0% | 8MB × 스레드 예약, 실제 터치는 극소 |
| `glibc arena` | 3.3G | 0B | 0% | 아레나 예약(`---p`), 물리 미할당 |
| `data_buffer` | 580.8M | 572.8M | 99% | 워밍업 완료, 전량 상주 |

### 초과(over) = 할당(mapped) / 설정
`cubrid.conf` 설정값 대비 엔진이 실제로 **매핑한** 크기. **105% 초과 시 경고 표시**.

상주가 아니라 할당을 분자로 쓰는 이유는, 초과분(BCB·victim 배열 등)이 **같은 매핑 안에 함께 잡히기** 때문입니다. 상주 기준은 `상주율`이 따로 보여줍니다.

`data_buffer_size`는 통상 100%를 넘습니다(실측 112%). 초과분은 페이지 데이터 외에 **BCB·victim 후보 배열·해시 테이블** 같은 관리 구조가 같은 예약에 포함되기 때문이며, 버퍼 수와 페이지 크기에 따라 달라집니다.

### 최근 접근 비율(핫)은 내지 않는다
"상주 중 창 안에 실제로 접근된 비율"은 `clear_refs` 로 대상의 접근 비트를 지운 뒤 smaps `Referenced` 를 읽어야 잴 수 있다.
그 쓰기가 대상 페이지 테이블 전수 순회와 TLB 무효화를 일으키므로(상주 1GB 당 약 27ms, 메모리 집약 대역 처리량 −52%; 4.2 미만 커널은 THP 를 4K 로 쪼개고 순회 동안 `mmap_sem` 을 잡는다),
모니터가 대상에 부하를 만들지 않는다는 원칙에 따라 내지 않는다. 지우지 않은 `Referenced` 는 우리 읽기와 커널 회수 주기가
섞인 값이라 쓰지 않는다. 설치 계정 권한으로 접근 비트를 건드리지 않고 재는 방법(`page_idle`·DAMON)은 없다.
버퍼풀이 얼마나 쓰이는지는 엔진이 스스로 유지하는 값(LRU 존 분포·victim 후보·dirty 수, `pgbuf.engine.*`)과
회전율·재적재율로 본다.

| 파생 지표 | 식 | 뜻 |
|---|---|---|
| 상주율 | `rss / alloc` | 예약 대비 실사용 |
| 놀고있는 예약 | `alloc − rss` | 매핑했으나 물리 미사용 |

## 변화 지표

### ΔRSS
프레임 간(`grow` 열) 또는 관측창 전체(메모리 추이 박스) 상주 증감.

### minflt / majflt (per second)
출처 `/proc/<pid>/stat`의 필드 10·12 델타.

| 지표 | 뜻 |
|---|---|
| **minflt/s** | 마이너 폴트 — 물리 메모리 내 처리. **메모리 활동량** 지표 |
| **majflt/s** | 메이저 폴트 — 디스크 I/O 발생. 0이 아니면 **스왑/페이지 캐시 미스** 신호 |

### 시간당 환산 증가율 (`+X/h`)
관측창의 ΔRSS를 시간당으로 선형 외삽한 값. 창이 짧으면 크게 흔들립니다.

## OS 컨텍스트

| 항목 | 출처 | 뜻 |
|---|---|---|
| MemTotal | `/proc/meminfo` | 총 RAM |
| used | `MemTotal - MemAvailable` | 실사용 |
| avail | `MemAvailable` | 회수 가능분 포함 가용량 |
| Cached | `Cached` | 페이지 캐시 — **CUBRID 볼륨 파일 캐시가 여기** 잡힘 |
| swap | `SwapTotal/SwapFree` | `off`(비활성) / `idle`(있으나 미사용) / `used X/Y` |

> `Cached`가 큰 것은 정상입니다. CUBRID는 볼륨을 파일로 읽으므로 OS 페이지 캐시가 데이터 버퍼와 별도로 데이터를 들고 있습니다.

## I/O

메모리 도구에 I/O를 넣은 이유는 **버퍼가 제 역할을 하는지**가 두 값의 관계로만 판정되기 때문입니다. 버퍼가 크고 상주율이 높아도 디스크 읽기가 계속 많다면 그 버퍼는 워크로드에 효과가 없습니다.

### 프로세스 I/O (`/proc/<pid>/io`) — cub_server에 직접 귀속

| 지표 | 필드 | 뜻 |
|---|---|---|
| **disk read/write** | `read_bytes` / `write_bytes` | **블록 계층 실제 디스크 I/O**. 페이지 캐시에서 처리된 것은 포함되지 않음 |
| rchar / wchar | `rchar` / `wchar` | **시스템콜 수준** 바이트. 페이지 캐시 히트 포함 |
| iops | `syscr` / `syscw` | 읽기/쓰기 시스템콜 횟수 |

### cache absorb (페이지 캐시 흡수율)

```
absorb = 1 - (read_bytes / rchar)
```

시스템콜로 요청한 읽기 중 **디스크까지 내려가지 않은 비율**입니다.

- **높다(예: 80%)** — OS 페이지 캐시가 읽기를 대신 처리하고 있음
- **낮다(예: 0%)** — 요청한 만큼 그대로 디스크에서 읽고 있음

실측 사례: 전체 스캔 워크로드에서 `rchar 527M/s` vs `read_bytes 539M/s`로 **absorb 0%** 가 관측되었습니다. `data_buffer` 상주율은 100%였지만 재사용 없는 순차 스캔이라 버퍼와 페이지 캐시 모두 히트를 만들지 못한 상태입니다.

> `read_bytes`가 `rchar`보다 큰 경우가 있습니다(readahead 때문). 그래서 비율은 1.0으로 클램프합니다.

**유휴 구간에서는 표시하지 않습니다.** `rchar`가 64KB/s 미만이면 분모가 사실상 0이라
비율 자체가 무의미합니다. 실측 예로 `rchar 95B/s`에서 식은 `absorb 100%`를 내지만
이는 "캐시가 완벽히 흡수 중"이 아니라 **"읽기가 없음"** 입니다. 이 구간은 `idle`로 표기하고
게이지를 비웁니다(오독 방지).

### 게이지 절대 하한 — 유휴 시 점멸·과대표시 방지

rate 게이지를 세션 피크만으로 정규화하면 유휴 상태에서 미세값(12 iops)이 피크가 되어
**게이지가 만충으로 표시되고, 값이 12↔0으로 흔들릴 때마다 100%↔0%로 점멸**합니다(실측 재현).
그래서 분모를 `max(세션 피크, 절대 하한)`으로 둡니다.

| 행 | 절대 하한 | 유휴 시 표시 |
|---|---|---|
| disk read / write | 8MB/s | 12 iops·95B/s 수준이면 거의 빈 게이지 |
| iops | 200/s | 44칸 중 3칸 (정직한 비율) |

부하가 하한을 넘어서면 하한은 무력해지고 종전대로 피크 상대 게이지로 동작합니다
(실측: read 540MB/s → 만충, iops 32,000/s → 41/44).

### blkio wait

```
blk_pct = (delayacct_blkio_ticks 델타 / CLK_TCK) / 경과시간 × 100
```

프로세스가 블록 I/O 완료를 **기다린 시간 비율**. 출처는 `/proc/<pid>/stat` 42번 필드입니다.

**주의**: 커널 delayacct가 동기 대기만 계상하므로, readahead·비동기 경로가 많은 워크로드에서는 장치가 포화(util 100%)여도 이 값이 0%에 가깝게 나옵니다. 낮은 값을 "I/O 문제 없음"으로 읽지 마십시오. 등급을 `◌`로 둔 이유입니다.

### device util

```
util = (diskstats io_ms 델타) / 경과시간(ms) × 100
```

`iostat %util`과 같은 정의입니다. 활동 중인 장치 중 **가장 바쁜 하나**를 보여줍니다.

**"I/O가 많은지 적은지"의 1차 판정 지표**입니다. 100%에 가까우면 장치가 포화되어 더 이상 처리량을 못 냅니다(90% 이상 경고 표시).

**한계**: 컨테이너/오버레이 파일시스템에서는 DB 볼륨 파일의 `st_dev`가 실제 블록 장치로 매핑되지 않습니다. 따라서 이 값은 **CUBRID 전용이 아닌 시스템 전체 맥락**입니다. `/sys/block`에 존재하는 전체 장치(물리 디스크 + device-mapper)를 대상으로 하며 파티션은 제외합니다.

### 게이지 스케일

| 행 | 게이지 기준 |
|---|---|
| disk read / write / iops | **max(세션 피크, 절대 하한)** 대비. 하한 = 8MB/s, 200 iops |
| cache absorb | 0~100% 절대. 단 `rchar < 64KB/s` 면 `idle` (아래 참조) |
| device util | 0~100% 절대 |

## 버퍼풀 · 로그 (`pgbuf.*` / `log.*`) — 버퍼 제어블록(BCB) 상태 읽기

모두 ◌(래치 없는 스냅샷). LSA 표기는 `pageid|offset`(로그 페이지 번호 | 페이지 내 오프셋).

| 키 | 정의 | 읽는 법 |
|---|---|---|
| `pgbuf.num_buffers` | BCB 수 = `data_buffer_size / page size` | paramdump 와 일치해야 한다(L4 게이트) |
| `pgbuf.resident` | NULL 이 아닌 VPID 를 가진 BCB 수 = 버퍼에 올라온 페이지 수 | `resident/num_buffers` 가 100% 미만이면 버퍼가 아직 안 찼다(워밍 중 또는 DB 가 버퍼보다 작다) |
| `pgbuf.dirty` | dirty 플래그가 선 BCB 수 | 커밋과 무관하게 남는다(커밋은 로그만 내린다). 체크포인트·flush 스레드가 줄인다. flush 가 시작되면 엔진이 dirty 를 내리므로 flush 중인 페이지는 여기 없다 |
| `pgbuf.flushing` | flush 진행 중인 BCB 수 | 순간값. 아직 볼륨에 안 쓰였다 |
| `pgbuf.unflushed` | dirty 또는 flushing 인 BCB 수 = 버퍼가 아직 내보내지 않은 페이지 | flush 도중 다시 dirty 가 된 페이지는 두 비트가 함께 서므로 합이 아니라 한 번만 센다 |
| `pgbuf.oldest_dirty_excludes_flushing` | flushing 이면서 dirty 가 아닌 BCB 수 | 엔진이 flush 시작 때 `oldest_unflush_lsa` 를 비우므로 가장 오래된 dirty LSA 가 이 페이지들을 보지 못한다 — 보통 가장 오래된 축이다 |
| `pgbuf.dwb_enabled / dwb_pages_max` | DWB 사용 여부와 DWB 가 담는 페이지 수(`double_write_buffer_size ÷ 페이지 크기`) | DWB 가 켜져 있으면 페이지는 DWB 에 복사된 순간 flush 완료로 처리된다. 그래서 dirty·flushing 이 아닌(clean) 페이지 중 최대 이만큼은 아직 DWB 에서 볼륨 쓰기를 기다릴 수 있다. 어느 페이지인지는 BCB 로 알 수 없다 |
| `pgbuf.zone1_hot / zone2_warm / zone3_cold` | LRU 3구역 | victim 은 cold 에서만 뽑고 dirty 는 건너뛴다 → cold 가 대부분 dirty 면 victim 기아 신호 |
| `pgbuf.zone_void` | 적재/축출 전이 중 | 순간값 |
| `pgbuf.zone_free` | 빈 BCB(invalid 존) | `resident + zone_free = num_buffers` |
| `pgbuf.vol<N>.resident / .dirty` | 볼륨별 분해 | `cub_volmap --bufmap` 볼륨 헤더의 `buf N pages (dirty M)` 과 같은 값 |
| `pgbuf.hdr_read` | 읽은 페이지 헤더 수 | 평소 표본 최대 256개(LRU 존 1·2 에서 고르게, 32개 미만이면 상주 cold 페이지로 32개까지 채움). `--bcb-dump` 일 때만 전수. 0 이면 오프셋을 검증하지 못한 것이라 `pgbuf.enabled=0` |
| `pgbuf.hdr_nonresident` | 상주가 아니어서(pagemap) 읽지 않은 헤더 수 | 읽으면 스왑인을 일으키므로 건너뛴다 |
| `pgbuf.hdr_pagemap_unchecked` | pagemap 을 못 읽어 상주 확인 없이 읽은 수 | 보통 0 |
| `pgbuf.hdr_mismatch / hdr_unread` | 읽은 헤더 중 대조 불일치 / 읽기 실패 수 | 소량(교체 중)은 정상, 표본의 50% 초과면 비활성 |
| `pgbuf.oldest_dirty_pageid/offset` | 가장 오래된 dirty 페이지의 `oldest_unflush_lsa` | 리커버리 redo 의 시작점 후보. flush 중인 페이지는 LSA 가 비워져 빠진다(`oldest_dirty_excludes_flushing`) |
| `pgbuf.engine.*` | 엔진이 유지하는 집계: `zone1/2/3`(LRU 리스트별 `count_lru1/2/3` 합), `victim_candidates`(`count_vict_cand` 합), `dirty`(`monitor.dirties_cnt`), `lru_pages`, `lists`(공유+private) | 퍼프몬과 무관하게 늘 읽힌다(O(1), 수십 KB). 버퍼 슬롯 26만 개 초과 버퍼풀은 BCB 스캔 대신 이것을 보인다. 존1 크기는 `pgbuf_lru_hot_ratio` 로 상한이 정해져 뜨거움의 지표는 아니다. victim 후보가 cold 대비 적으면 cold 가 dirty·fix 로 막혀 있다는 뜻 |
| `log.append_*` | 로그 꼬리(`hdr.append_lsa`) | 새 로그 레코드가 붙는 위치 |
| `log.nxio_*` | 아직 디스크에 안 쓴 첫 LSA(`append.nxio_lsa`) | WAL 경계: 이보다 앞선 로그는 영속 |
| `log.eof_*` | `hdr.eof_lsa` | |
| `log.flush_lag_pages` | `append.pageid − nxio.pageid` | 0 = 로그가 전부 디스크에 있음(커밋 직후 정상). 크면 그룹커밋/로그 I/O 지연 |
| `log.dirty_span_pages` | `append.pageid − oldest_dirty.pageid` | **flush 지연** — 장애 시 redo 해야 할 로그 길이의 하한. 체크포인트 주기 튜닝의 직접 지표 |

부등식 `log.append ≥ log.nxio ≥ (dirty 페이지의 메모리 LSA) ≥ (그 페이지의 디스크 LSA)` 가 항상 성립한다.
cub_volmap 페이지 박스의 `buf:DIRTY unflushed / flushed` 는 마지막 두 항의 비교다.

## 용량 축 (`cap.*`) — 증설·축소·개선 판단

목적은 질의 하나(나뭇가지)가 아니라 **할당 리소스 대비 수요·상한·헤드룸**(숲)이다.
전부 읽기 전용이며 대상 프로세스에 쓰기가 없다. 취합 예산 0.5초를 넘으면 그 프레임 값을 버리고 사유를 표기한다.

### 수요 (demand)

| 키 | 정의 | 읽는 법 |
|---|---|---|
| `cap.device` | 용량 판정 장치 = DB 파일(볼륨·로그)이 놓인 물리 디스크 중 이 프레임에 가장 바쁜 것. 비면 장치 판정 없음 | 호스트 최다사용 장치가 아니다 — 백업이 다른 디스크를 채워도 DB 디스크의 판정에 섞이지 않는다 |
| `cap.device_via` | 찾은 경로: `st_dev`(일반 마운트) · `mount`(마운트 원본) · `statfs_fsid`(overlay 등 익명 장치, 하위 XFS 의 fsid) | |
| `cap.device_reason` | 장치를 못 찾은 사유(tmpfs·네트워크·ext4/btrfs 위 overlay, DB 파일 위치 미확인) | |
| `cap.dev_iops` | `cap.device` 의 초당 I/O 완료 수 = `(rd_ios+wr_ios) 델타 ÷ 경과` | 디스크 단위 값이다(같은 디스크의 다른 프로세스 포함) |
| `cap.dev_util` | `cap.device` 의 사용률 % = `io_ticks 델타 ÷ 경과` | DB 디스크의 값. 4번 패널(flush)의 쓰기 병목 판정이 이것을 쓴다 — `hostdev.util_pct`(호스트에서 가장 바쁜 장치)가 아니다. 없으면(구 기록) DB 디스크 미확인으로 재생 |
| `cap.proc_iops` | 이 인스턴스의 `syscr+syscw` 초당 = 엔진이 발행한 요청 수 | 장치 IOPS 보다 크면 정상(페이지 캐시가 흡수한 분) |
| `cap.read_bps` / `write_bps` | `/proc/<pid>/io` 의 `read_bytes`/`write_bytes` 초당 | 블록 계층 실제 디스크 바이트 |

### 지연 (latency) — iostat 의 await 와 같은 정의

| 키 | 정의 | 판단 |
|---|---|---|
| `cap.await_ms` | `(rd_ticks+wr_ticks) ÷ (rd_ios+wr_ios)` | IOPS 는 낮은데 이 값이 크면 **IOPS 증설이 아니라 더 빠른 스토리지**가 답이다 |
| `cap.read_await_ms` / `write_await_ms` | 읽기·쓰기 분리 | 쓰기만 크면 로그·flush 경로, 읽기만 크면 스캔 경로 |
| `cap.queue_depth` | `time_in_queue 델타 ÷ 경과(ms)` = 평균 큐 깊이 | 1 이상이면 요청이 줄서기 시작한 상태 |

### 상한 (ceiling) 과 헤드룸

| 키 | 정의 |
|---|---|
| `cap.ceiling_iops` | **큐가 쌓인 순간에 관측된** IOPS 최댓값. 갱신 조건 = 평균 큐 깊이 4 이상. I/O 하나만 계속 걸어 둬도 사용률 ≈100%·큐 ≈1 이 되므로, 사용률만으로는 직렬 스트림의 처리량(지연의 역수)을 장치 상한으로 잘못 기억한다 |
| `cap.ceiling_device_id` | 상한이 묶인 장치의 고유 ID — `dm/uuid` → `wwid` → `device/wwid` → `device/serial` 순, 끝에 `@<섹터 수>`, 파티션은 `:p<n>`. 커널 이름(sdX·nvme0n1·dm-N)은 재부팅·LVM 활성화 순서에 따라 다른 디스크로 옮겨 가므로 키로 쓰지 않는다(ID 가 없을 때만 이름+크기) |
| `cap.ceiling_util_pct` / `ceiling_qdepth` / `ceiling_epoch` | 그 순간의 근거(사용률·큐)와 시각 |
| `cap.used_of_ceiling_pct` | 현재 IOPS ÷ 상한. 80% 이상이면 용량 한계, 20% 이하이고 사용률 50% 미만이면 여유 |

상한은 **그 장치, 그 워크로드 패턴에서의 실효 상한**이다(16KB 랜덤 읽기 위주 등). 장치마다, 그리고 요청 크기 3구간 × 읽기 비율 3구간의 칸마다 따로 기억하고, 판정은 DB 디스크(`cap.device`)의 같은 칸 상한과만 비교한다.
상한은 이 실행 동안만 기억하고 파일로 남기지 않는다(도구가 디스크에 상시 쓰는 경로를 두지 않기 위해서). 큐가 4 이상 쌓인 적이 없으면 `상한 미관측`으로 표기한다.

실측(cbench 콜드 스캔, sda): 사용률 100% 시점 **5,449 IOPS**, 이후 프레임 헤드룸 81~100%.

### 버퍼 (히트율) — 이중 경로

| 키 | 정의 |
|---|---|
| `cap.perfmon_counters_running` | 엔진 카운터가 돌고 있나(1/0) |
| `cap.hit_source` | `perfmon`(초당 델타) / `perfmon-cum`(누적) / `turnover`(대체) / `-`(불가) |
| `cap.buffer_hit_pct` | perfmon: `1 − ioreads/fetches`. turnover: `100 − 회전율` (성격이 다르므로 출처를 반드시 함께 읽는다) |
| `cap.turnover_pct` | 관측창에 **새로 들어온** 페이지 ÷ 상주 = 버퍼 교체율 |
| `cap.miss_pages_per_s` | 초당 적재 페이지 = 미스 |
| `os.cgroup_limit_bytes` / `os.cgroup_usage_bytes` / `os.cgroup_path` | 대상 서버의 메모리 cgroup 한도·사용량·경로(v2 `memory.max`/`current`, v1 `limit_in_bytes`/`usage_in_bytes`). 한도 -1 = 없음. 컨테이너·systemd 슬라이스에서 OOM 기준은 호스트 RAM 이 아니라 이 한도다 |
| `os.psi_mem_some10` / `os.psi_mem_full10` | `/proc/pressure/memory` 의 10초 평균(%). -1 = 커널이 제공하지 않음(4.20 미만 또는 psi=0) |
| `server.thp_kb` | 대상 서버 메모리 중 투명 거대 페이지(AnonHugePages)에 올라간 양. smaps 를 읽을 때 갱신 |
| `volcache.bytes` / `volcache.resident_bytes` / `volcache.resident_pct` / `volcache.files` / `volcache.age_s` / `volcache.reason` | DB 볼륨·로그 파일(서버 fd 표의 `<db>`, `<db>_*`, 없으면 databases.txt 디렉터리)이 OS 페이지캐시에 올라 있는 비율. 읽기 전용 mmap 위에서 `mincore` 만 호출(페이지 폴트·I/O 없음, GB당 약 7ms)하고 소요 시간의 100배 주기(최소 10초)로 갱신하며 `age_s` 가 나이다. bytes 0 = 미측정(`reason`) |
| `server.smaps_age_s` / `server.smaps_ms` | 영역별 상주·PSS 를 마지막으로 읽은 뒤 지난 초 / 그 읽기에 걸린 ms. smaps 는 소요 시간의 100배 주기로만 읽는다(limitations 침습도 표) |
| `cap.miss_source` | `scan`(BCB 전수 통과의 페이지 집합 차) / `sample`(대형 버퍼의 통과 사이: 슬롯 1/64 표본에서 바뀐 비율 × 슬롯 수) / `perfmon`(표본도 없을 때 엔진 ioread 카운터 초당 델타, watcher 가 있고 창 안에 fetch 가 있을 때만) |
| `pgbuf.scan_age_s` / `pgbuf.sample_miss_per_s` | 대형 버퍼: 마지막 전수 통과 뒤 지난 초(0 = 이 프레임이 통과) / 이 프레임 슬롯 표본의 초당 적재 추정(-1 없음) |
| `cap.readmit_pct` | 적재분 중 **최근 nbuf건의 내보내기 안에서 빠졌다 다시 들어온** 비율((volid, pageid) 정확 집합으로 비교). 정확한 LRU 에서 이 페이지들은 버퍼가 2배였다면 상주했을 것이므로 **2배 증설로 줄어드는 읽기의 추정치(LRU 가정)** 다 — 하한이 아니다(중간점 삽입·프레임 안 적재·앨리어싱은 views.md 3번 패널 절). 30% 이상 = 작업집합 > 버퍼(증설 근거). 낮은 값은 재사용 거리가 버퍼 밖이라는 뜻이며, 스캔인지 버퍼의 수 배인 작업집합인지는 가르지 않는다 |
| `cap.admit_frac_of_buffer` | 이 프레임의 적재 페이지 수 ÷ nbuf. **0.5 이상이면** 프레임 안에서 적재되고 내보내진 페이지를 스냅샷이 못 봐 재적재율이 부정확하고 적재율은 집합 차라 `nbuf/간격` 에서 포화된다 — 판정문이 '신뢰도 낮음' 으로 바뀐다 |
| `cap.readmit_available=0` | 파일 기준선(1회 실행)에서는 재적재 이력이 없어 못 낸다 → 라이브(`-b`)로 확인 |

**중요(엔진 사실)**: `perfmon_add_stat()` 은 `pstat_Global.n_watchers > 0` 이 아니면 카운터를 **아예 증가시키지 않는다**
(`perf_monitor.h`: `perfmon_is_perf_tracking()`). 즉 statdump·`csql ;histo on` 같은 watcher 가 없으면 모든 카운터가 0 이다.
cub_top 은 **watcher 를 만들지 않는다** — 만들려면 대상 프로세스 메모리 쓰기 + 서버 핫패스 비용이 생기고, 이는 무침습 조건 위반이다.
그래서 카운터가 이미 돌면 읽고, 아니면 회전율로 내려간다. 실증: watcher 없을 때 전 카운터 0, `csql ;histo on` 직후 fetches 26·ioreads 26 관측.

### 엔진 카운터 (있을 때만)

`cap.pb_fetch_per_s` / `pb_ioread_per_s` / `pb_iowrite_per_s` / `log_iowrite_per_s` / `commit_per_s` —
각각 `PSTAT_PB_NUM_FETCHES` / `PB_NUM_IOREADS` / `PB_NUM_IOWRITES` / `LOG_NUM_IOWRITES` / `TRAN_NUM_COMMITS` 의 초당 델타.
`pstat_Metadata[psid].start_offset` 로 인덱스를 찾고, 메타데이터의 psid 가 요청 psid 와 다르면(타버전) 지어내지 않고 포기한다.

### 취합 비용

| 키 | 정의 |
|---|---|
| `cap.aggregate_ms` | 이 프레임의 용량 취합 소요. 실측 0.2~0.9ms |
| `cap.budget_ms` / `cap.over_budget` | 예산(500ms)과 초과 여부. 초과 프레임은 값을 내보내지 않고 판정문에 `취합 생략`을 남긴다 |
