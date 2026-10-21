# 온디스크 포맷 지식 (On-Disk Format Notes)

볼륨 바이너리를 직접 해석하는 데 필요한 포맷 지식을 정리한다. 구조체 정의 자체는
`src/storage_ondisk_layout.hpp`가 원본이며(오프셋은 컴파일러가 계산), 이 문서는
그 배경 지식과 실측으로 확정한 사실을 기록한다. 파일 매니저 재설계(10.1, CBRD-20185)
이후 이 레이아웃은 불변이라 10.0~11.5 전 버전에 적용된다.

## 1. 페이지 공통 구조

```
┌─ FILEIO_PAGE_RESERVED (prv, 32B) ─┬─ 사용자 영역 (user_size) ─┬─ watermark ─┐
│ lsa(8) pageid@8(i32) volid@12(i16)│                           │    (8B)     │
│ ptype@14(u8) pflag@15(u8) ...     │                           │             │
└───────────────────────────────────┴───────────────────────────┴─────────────┘
user_size = iopagesize - sizeof(prv) - sizeof(watermark)   (16KB 페이지 → 16,344B)
```

- `prv.pageid == 실제 페이지 위치` 자기검증으로 미기록/재사용 페이지를 걸러낸다.
- `ptype`: PAGE_FTAB(1)·PAGE_HEAP(2)·PAGE_BTREE(10) 등 — `PAGE_TYPE` enum(storage_common.h).
- `pflag & 0x3`: TDE 암호화(AES=0x1, ARIA=0x2). 셋이면 내용 해석 불가 — 감지만 한다.

## 2. 볼륨 헤더와 섹터 할당 테이블

- `DISK_VOLUME_HEADER`는 magic 문자열("CUBRID/Volume")로 검증. 섹터=64페이지=1MB.
- 섹터 할당 테이블(stab): 1비트/섹터, LSB-first, `stab_first_page`부터 `stab_npages`.
  12GB 볼륨 기준 16KB 남짓 — 이것이 "읽기 최소"의 근거.
- 임시 볼륨(`<db>_t<N>`)은 `*_vinf`에 등재되지 않는다 — 디렉터리 패턴 스캔으로 자동 포함.

## 3. FILE_HEADER와 파일 자기발견 (실측 확정 오프셋, x86-64/prv=32)

| 필드 | 위치 (user 영역 기준) |
|---|---|
| self VFID | @8 — int32 fileid + int16 volid |
| n_page_total..n_sector_empty + ftype + flags | int32×11 @104 |
| volid_last / offset_to_partial / full / user_page_table | int16×4 @148 |
| 파일 디스크립터 (힙=class OID, 인덱스=BTID) | @40 |
| vpid_sticky_first (인덱스 루트 페이지) | — 인덱스명 해석에 사용 |

자기발견 절차: 예약 섹터의 **첫 페이지**만 프로브(prv.ptype==PAGE_FTAB) →
FILE_HEADER 파싱 → **self VFID == 자기 VPID** 일치 시 파일 확정. `file_create`의 할당
순서상 파일 헤더는 자기 첫 예약 섹터의 첫 페이지에 위치하므로 섹터당 1페이지 프로브로
대개 충분하다. 예외(vacuum 이 아직 dropped 로 보는 VFID 를 피해 헤더가 뒤 페이지에 놓인 파일)는
1차 뒤 소유자 미상 섹터의 전 페이지를 보는 2차 패스가 찾는다 — 256개 이하면 자동, `--full-sweep` 이면 전부.

## 4. 파일 섹터 테이블 (extdata 체인)

- `FILE_EXTENSIBLE_DATA` 헤더 16B(next VPID + int16×3). **체인은 볼륨 경계를 넘는다** —
  next VPID의 볼륨으로 따라가야 한다 (놓치면 소유자 미상 섹터가 생긴다).
- `FILE_PARTIAL_SECTOR` 16B = VSID(8) + UINT64 페이지 비트맵. full 테이블 항목은 VSID 8B.
- 이 비트맵이 "예약 대비 실할당"(reserve waste)과 페이지 단위 지도(L1)의 데이터 원천.

## 5. 슬롯 페이지 (SPAGE)

```
SPAGE_HEADER: num_slots(i16) num_records(i16) anchor(i16) alignment(u16)
              total_free(i32) cont_free(i32) offset_to_free_area(i32) ...
SPAGE_SLOT (4B, 페이지 끝에서 역방향):
              offset_to_record:14 | record_length:14 | record_type:4
```

- total_free/num_records가 페이지 선두에 영속되므로 레코드 밀도는 헤더만 읽어 얻는다.
- record_type: HOME(2) NEWHOME(3) RELOCATION(4) BIGONE(5) MARKDELETED(6) DELETED_WILL_REUSE(7).
- 삭제 슬롯 판정: type 6/7 또는 offset_to_record==0.

## 6. OR 레코드와 이름 해석

- **OR 정수는 네트워크 바이트오더**(`or_put_int`=htonl) — 파싱 시 bswap 필수.
- 클래스 레코드의 **첫 가변속성이 클래스명**(`or_class_name`과 동일 지식).
  가변 오프셋 테이블은 repid 상위 비트의 offset-size 플래그(1/2/4B)를 따른다.
- `REC_RELOCATION`의 포워딩 목적지는 `REC_NEWHOME`; 대형 레코드는 `REC_BIGONE` →
  오버플로 first-part(next_vpid+length 뒤 data)에서 이어붙여 파싱한다.
- 인덱스명: 클래스 레코드 property list에 BTID가 `"vol|file|rootpage"` 문자열로 저장된다.
  파일의 VFID+vpid_sticky_first로 needle을 만들어 찾고, 직전의 길이-접두 식별자 문자열이
  인덱스명이다 (≤512B 역방향 탐색).

## 7. MVCC 레코드 헤더 (dead version 판정)

레코드 선두 int32 `repid_and_flags`(빅엔디안)의 상위 5비트(비트 24–28)가 MVCC 플래그다:

| 플래그 | 값 | 의미 |
|---|---|---|
| OR_MVCC_FLAG_VALID_INSID | 0x01 | insert MVCCID 보유 |
| OR_MVCC_FLAG_VALID_DELID | 0x02 | **delete MVCCID 보유 = 미바큠 dead version** |
| OR_MVCC_FLAG_VALID_PREV_VERSION | 0x04 | 이전 버전 LSA 보유 |

vacuum이 수행되면 DELID가 제거(레코드 삭제 또는 헤더 정리)되므로, DELID가 남아 있는
HOME/NEWHOME 레코드 수 = "미바큠 dead version 수"다. PAGE_HEAP에만 적용한다
(BTREE 레코드는 레이아웃이 다르다). BIGONE의 MVCC 헤더는 오버플로 페이지에 있어 제외.

## 8. 버전별 볼륨 포맷 변천 (10.0 → 11.5)

volmap이 파싱하는 볼륨 포맷의 성립·변화 이력. 근거는 CUBRID 소스 git 태그 간
구조체 직접 diff다 (`DISK_VOLUME_HEADER` / `FILE_HEADER` / `FILEIO_PAGE_RESERVED`).

| 버전 | 볼륨 포맷 변화 | volmap 영향 |
|---|---|---|
| ~10.0 (9.x 포함) | **구포맷** — 페이지 할당 비트테이블(`page_alloctb`), 볼륨 purpose 4종(DATA/INDEX/GENERIC/TEMP), `used_data/index_npages` 카운터, 파일은 allocset 체인(extdata 없음) | **미지원.** 자기검증이 거부 — 10.0 은 magic 은 같고 그 자리 값이 달라(`sect_npgs=128` 실측) 탈락, 9.x 는 페이지 헤더가 짧아 magic 부터 어긋난다. 로그 릴리스가 10.1 미만이면 손상이 아닌 미지원으로 보고하고 경고 |
| **10.1** (2016-11) | **현행 포맷 성립** — 디스크/파일 매니저 재설계(CBRD-20185, cc368329d). 섹터(64페이지=1MB) 예약 비트테이블 `stab`, purpose 2종(PERMANENT/TEMPORARY), `FILE_HEADER`+extdata 파일 구조. 페이지 prv 끝 8B는 미사용 reserved | 지도·요약·자기발견·--check 전부 동작하는 최소 버전 |
| **10.2** | **페이지 워터마크 도입** — 페이지 끝에 `FILEIO_PAGE_WATERMARK` 8B 추가(CBRD-22231, d81b071e8). 사용자 영역이 그만큼 줄어든다 | 실측 검증 버전 |
| **11.0** | **TDE 도입** — prv의 reserved 1B가 `pflag`(암호화 비트)로, reserved 8B가 `tde_nonce`로 전환 (**prv 32B 크기 불변**). 워터마크는 이미 10.2 에 있다 → **사용자 영역 8B 감소**(`iopagesize − prv − 8`) | `E` 표기·TDE 소견의 근거. 사용자 영역 크기 기준 변화(아래 캐비엇) |
| 11.2 / 11.3 | 변화 없음 | 11.3 실측 검증 |
| **11.4** | 볼륨 헤더에 `vol_creation`(INT64) 추가 — CBRD-25365(44c022c31), `db_creation` 뒤 삽입 | **삽입점 뒤 필드에 영향** — `chkpt_lsa`·`next_volid` 는 ≤11.3 볼륨에서 8B 앞에 있다. 버전을 판별해 보정한다(아래). 10.2 / 11.0 / 11.2 / 11.3 / 11.4 / 11.5 실측 검증 |
| 11.5 | 변화 없음 | 11.5 실측 검증 | <!-- (iopagesize/volid/purpose/sect_npgs/nsect_total/stab_*/sys_lastpage)는 전부 삽입점 앞 |

### `vol_creation` 삽입과 버전 판별

지도·요약·`--check` 가 쓰는 필드는 **전부 삽입점(`db_creation`) 앞**이라 영향이 없다.
영향을 받는 것은 **대화형 볼륨 헤더 상세줄**(`vhdr p0:`)이 출력하는 `chkpt_lsa` 와
`next_volid` 두 개다 — `storage_ondisk_layout.hpp` 가 11.4+ 사본이므로, ≤11.3 볼륨에서
구조체 그대로 읽으면 8B 밀린 값이 나온다.

볼륨 헤더에는 버전 필드가 없다. 그러나 **버전을 알 필요는 없고 레이아웃만 알면 되며,
레이아웃은 볼륨 파일 자체에서 판별된다.** 따라서 판별은 볼륨만으로 끝내고, 로그는
교차검증에만 쓴다 — 로그가 없거나 분리되었거나 읽을 수 없어도 동작에 지장이 없다.

**축 ① `vol_creation` 유무** — 두 레이아웃을 모두 적용해 보고 자체 정합한 쪽을 택한다.
`vol_fullname` 은 가변 길이 영역의 첫 항목이라 `offset_to_vol_fullname` 이 항상 0 이고,
뒤의 두 오프셋은 이름 길이만큼 뒤에 온다. 틀린 레이아웃으로 읽으면 이 세 INT16 이
이름의 글자나 예약 0 위에 떨어진다 — 10.2 볼륨을 11.4 로 읽으면 28527(`'on'`),
11.5 볼륨을 ≤11.3 으로 읽으면 전부 0.

**축 ② 페이지 워터마크 유무** — `FILEIO_PAGE_WATERMARK` 는 `prv.lsa` 의 복제본이므로
(`file_io.h`), 워터마크가 있는 페이지는 **선두 8B 와 말미 8B 가 같다**. 10.1 은
워터마크가 없어 그 자리가 사용자 데이터이고, 우연히 일치할 수 있으므로 기록된 페이지를
최대 32개 표본하여 **다수결**로 판정한다.

| 일치 수 (표본 n) | 판정 | 근거 |
|---|---|---|
| ≥ n × 3/4 | 워터마크 있음 (10.2+) | 일치하지 않는 페이지가 있으면 그 수를 stderr 에 알린다 |
| ≤ 1 | 워터마크 없음 (10.1) | 10.1 은 `prv.lsa` 가 대부분 NULL_LSA(0xff…)이고 말미가 0 이라 우연 일치가 드물다 |
| 그 사이 | 판정 보류 | 다음 볼륨 → 로그 순으로 넘긴다 |

전원 일치를 요구하지 않는 이유: 온라인 DB 에서 서버의 페이지 쓰기와 동시에 읽거나, 장애 직후
복사한 볼륨이면 찢어진 페이지(선두는 새 LSA, 말미는 옛 워터마크)가 있을 수 있고, 그 한 장이
10.2+ 볼륨 전체를 10.1 로 오판시킨다. 실측(전 파일): 10.1 일치 0/4,095, 10.2·11.3·11.4 일치
100%(각 8,191/8,191).

표본은 헤더의 페이지 수가 아니라 파일 크기로 걷는다 — 그 페이지 수 자체가 판별 대상
레이아웃으로 읽히는 값이기 때문이다.

로그는 **교차검증용**이다. 로그 경로는 **vinf 의 `-2` 항목**(`LOG_DBLOG_ACTIVE_VOLID`)에서
읽는다 — `createdb --log-path` 로 분리한 구성에서는 vinf 옆에 `_lgat` 가 없기 때문이다.
엔진도 같은 방식으로 찾는다(`migrate.c` 의 `get_active_log_vol_path`). `db_release` 자체의
오프셋도 릴리스마다 달라서(10.2~11.3 은 56, 11.4 는 64) 고정 오프셋이 아니라 **패턴으로
찾는다**. 볼륨과 로그의 판정이 다르면 **볼륨을 따르고 그 사실을 stderr 에 알린다** —
업그레이드한 데이터베이스는 구 볼륨 위에 신 로그가 놓이는 것이 정상이고, 읽는 대상은
볼륨이기 때문이다. 같은 이유로 `db_release` 는 **볼륨이 만들어진 버전이 아니라 로그를
마지막으로 쓴 버전**이다.

> 아카이브 로그(`_lgar*`)에는 `db_release` 가 없다(`log_arv_header` 에 필드 자체가 없음).
> 버전 문자열을 가진 것은 액티브 로그와 백업 헤더뿐이다.

오프셋이 밀린 이유는 같다: **로그 헤더도 같은 자리에 `vol_creation` 이 삽입**되어
(`db_creation` 뒤) `db_release` 가 8B 뒤로 밀렸다. 즉 11.4 는 볼륨 헤더와 로그 헤더가
함께 바뀌었다.

### 구간 판정 근거

구조체 정의를 전 버전 대조했다.

| 구조체 | 10.2 | 11.0 | 11.1 | 11.2 | 11.3 | 11.4 |
|---|---|---|---|---|---|---|
| `DISK_VOLUME_HEADER` | 동일 | 동일 | 동일 | 동일 | 기준 | **+`vol_creation` 1줄** |
| `log_header` | 동일 | 동일 | 동일 | 동일 | 기준 | **+`vol_creation` 1줄** |

10.2~11.3 은 **텍스트 단위로 완전히 같고**, 11.4 와의 차이는 삽입된 그 한 줄뿐이다
(`CUBRID_MAGIC_MAX_LENGTH` 도 전 버전 25 로 동일).

그리고 **11.0·11.2 는 실제 DB 로 실행 실측**해 이 대조가 맞음을 확인했다(아래 표).
`db_release` 위치도 전 버전에서 예측과 일치했다 — ≤11.3 은 56, ≥11.4 는 64,
**측정 7종 전부 일치**(10.2.18·11.0.16·11.2.9·11.3.5·11.4.4·11.4.5·11.5.0).

**11.1 만 실행 실측이 없다** — 양쪽 장비 어디에도 11.1 DB 가 없다. 다만 11.1 은
11.0·11.2 와 구조체가 동일하고 그 둘이 모두 실측되었으므로, 구간 내부에 남은
미검증 지점은 없다.

**10.1 도 실측했다** — 공식 배포본(10.1.8.7823, ftp.cubrid.org)으로 실제
데이터베이스를 생성해 확인했다.

| 확인 항목 | 결과 |
|---|---|
| 워터마크 부재 | 표본 39페이지 **전부 불일치**(선두 8B ≠ 말미 8B) — 10.2+ 는 전부 일치 |
| 레이아웃 판정 | 로그 유무와 무관하게 `10.1` |
| 슬롯 디코딩 | 클래스명 **42종 해석**(`user_size` 16352 적용) |
| 반대 경우(대조군) | `user_size` 를 16344 로 강제하면 클래스명 **42 → 24** 로 18종이 조용히 유실 |

마지막 행이 이 축을 두는 이유다 — 틀려도 오류 없이 **덜 보일 뿐**이라 실측 없이는
드러나지 않는다.

### 10.0 은 대상 밖

10.0 은 10.1 이전 구포맷이라 **자기검증 단계에서 거부**된다(섹션 §8 첫 행).
공식 10.0.0.1376 배포본으로 만든 DB 에서 `sect_npgs=128` 로 걸러졌다 — 버전 판별
이전에 막히므로 이 보정의 대상이 아니다. 로그 릴리스가 10.1 미만이면 그 실패는
`unsupported release` 로 보고된다(손상 소견 아님).

| 판별 결과 | 처리 |
|---|---|
| ≥ 11.4 | 구조체 그대로 |
| ≤ 11.3 | 삽입점 뒤 필드를 **8B 앞에서** 읽는다 |
| 판별 실패 | 해당 필드를 **출력하지 않는다** (`chkpt_lsa ?  next_vol ?`) — 틀린 값을 조용히 보이지 않는다 |

실측(동일 struct·동일 memcpy 경로):

| 볼륨 | 보정 전 | 보정 후 | 근거 |
|---|---|---|---|
| 10.2.17 | `chkpt_lsa 128\|0  next_vol 29231` | `chkpt_lsa 140149\|13888  next_vol 1` | 실행 실측 |
| **11.0.16** | `chkpt_lsa 128\|0  next_vol 29231` | `chkpt_lsa 4018\|13264  next_vol -1` | **실행 실측** |
| 11.1 | — | — | 11.0·11.2 와 구조체 동일 (DB 없음) |
| **11.2.9** | `chkpt_lsa 128\|0  next_vol 29231` | `chkpt_lsa 4140\|11280  next_vol -1` | **실행 실측** |
| 11.3.3 / 11.3.5 | `chkpt_lsa 128\|0  next_vol 29231` | `790\|8800` / `4139\|7328`  `next_vol -1` | 실행 실측 |
| 11.4.4 / 11.4.5 | `chkpt_lsa 62029\|6904  next_vol 1` | **동일**(변화 없음) | 실행 실측 |
| 11.5.0 | `chkpt_lsa 119727\|13840  next_vol -1` | **동일**(변화 없음) | 실행 실측 |

≤11.3 볼륨이 보정 전 **전부 `128|0` · `next_vol 29231`** 로 같은 값을 낸 것이,
읽던 자리가 데이터가 아니라 고정된 엉뚱한 바이트였음을 보여준다.

`next_vol 29231` 은 볼륨 이름 문자열 두 글자를 숫자로 읽은 값이다.

`FILE_HEADER` 는 10.1 → 11.5 구조체 diff 가 동일하다 — 파일 자기발견·이름 해석·temp 생성
시각이 그 구간에서 같은 코드로 동작하는 근거.

> **develop 은 다르다.** CBRD-27308(1ea077d85)이 `FILE_HEADER` 를, CBRD-26176(e84a7f6dc)이
> `SPAGE_HEADER` 를 바꿨다. 사본(`storage_ondisk_layout.hpp`)을 쓰는 부분은 이 변경이
> **컴파일로 드러나지 않으므로**, develop 볼륨을 다룰 때는 이 표를 먼저 확인해야 한다.

**10.1 볼륨의 사용자 영역 — 워터마크 유무로 판별한다**: 페이지 워터마크
(`FILEIO_PAGE_WATERMARK` 8B)는 **10.2** 에서 도입됐다(CBRD-22231, d81b071e8).
10.1 의 `storage_common.c` 는 `RESERVED_SIZE_IN_PAGE = sizeof (FILEIO_PAGE_RESERVED)`
뿐이고, 10.2 부터 여기에 워터마크가 더해진다.

사용자 영역 끝은 **슬롯 디렉터리의 기준점**이다(디렉터리는 끝에서 역방향으로 자란다).
그래서 10.1 볼륨에서 8B 를 더 빼면 슬롯 뷰·`--deep`·del/dead 집계·오프라인 이름 해석이
전부 엉뚱한 자리를 읽는다. 그래서 축 ②(§ 위)로 **워터마크 유무를 직접 확인**해 결정한다.

| 판별 | `user_size` (iopagesize 16384 기준) |
|---|---|
| 10.1 | `iopagesize − prv` = **16352** (워터마크 없음) |
| 10.2 ~ 11.3 | `iopagesize − prv − 8` = 16344 |
| ≥ 11.4 | 〃 16344 |
| 판별 실패 | 16344 **+ 경고**(stderr) — 10.2 이후가 전부 이 레이아웃이라 그쪽을 가정하되, 10.1 이면 8B 어긋남을 알린다 |

지도·요약·자기발견·`--check` 는 슬롯 배열을 쓰지 않아 어느 쪽이든 무관하다.

## 9. 유틸리티 등록 계약 (CUBRID 트리)

`ua_Utility_Map`(util_admin.c)은 **enum 값 = 배열 인덱스** 계약이다 — 중간 삽입은
`util_get_utility_index`의 인덱스 참조를 어긋나게 해 segfault를 유발한다.
유틸리티 추가는 반드시 배열 끝 + enum의 대응 위치에.
