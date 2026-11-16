# 빌드 (Build)

```sh
./build.sh          # src/ 에서 gcc -O2 -Wall -Wextra -Wshadow -I. -static <build.sh 의 소스 목록> -o ../cub_ctv
```

| 항목 | 내용 |
|---|---|
| 요구 | gcc, glibc-static (정적 빌드). 외부 라이브러리 없음. 테스트: bash, python3. 서버 시험: CUBRID 설치본·csql |
| 폴백 | glibc-static 이 없으면 동적 링크로 빌드하고 `full-static unavailable - building dynamic (libc only)` 출력 |
| 동적 빌드 | `./build.sh --dynamic` — glibc 를 동적 링크(재배포 시 LGPL 의무 회피, NOTICE 참고) |
| 산출물 | `./cub_ctv` 약 1.2MB, `env -i ./cub_ctv` 실행 가능 |
| 기타 인자 | `./build.sh --list-sources` — 소스 경로 목록(테스트용). 그 밖의 인자는 usage 출력 후 종료 코드 2 |
| 커밋 | 바이너리는 `.gitignore` 로 제외 |

## 테스트

| 스크립트 | 내용 | 서버 |
|---|---|---|
| `tests/t5_replay.sh` | 기록 픽스처(`*.rec`)를 보기 6종으로 재생 == 골든(바이트), JSON 유효성 | 불필요 |
| `tests/t6_view_roundtrip.sh` | 대기 체인·특수문자 이름·풀을 주입한 프레임 기록→재생 동일 (`ld --wrap`) | 불필요 |
| `tests/t11_ident_delta.sh` | 신원 변화분 기록(`K`/`X`) 재생 = 전체 기록 재생, 구간 이동 신원 = 순차 | 불필요 |
| `tests/t12_field_sched.sh` | FIELD·SCHED 규칙 단위 시험 | 불필요 |
| `tests/t13_verdict.sh` | 판정 수치 골든 비교 | 불필요 |
| `tests/t14_slot_reuse.sh` | 구간 중 슬롯 재사용(`J` 신원·`W`/`S`/`C` cpid 주입): 소유자 블록·대기 홉·세션 패널이 슬롯의 현재 클라이언트가 아닌 목격된 클라이언트를 `disconnected` 로 표시 | 불필요 |
| `tests/t7_record_equiv.sh` | 실서버에서 라이브 화면 = 기록 재생 화면, 디테일 덤프 JSON·재생 거부 | 필요 |
| `tests/t8_replay_keys.py` | pty 에서 재생 키(일시정지·구간·속도·종료), 요약 화면 세션 강조, 색상·`--plain` | 불필요 |
| `tests/t10_os_probe.sh` | OS tid·스레드 이름·대기 사유를 `/proc` 과 대조 (I-17/I-18) | 필요 |
| `tests/t9_quick_trace.sh` | 빠른 확인: 부하 중 라이브 요약 1구간 + 기록, 판정·대기 세션 검사, 라이브 `r` 녹화 시작·중지 (usage.md §3) | 필요 |
| `tests/check_msg_pairs.py` | 한/영 메시지 쌍의 printf 지시자 동일성 | 불필요 |
| `tests/t2_show_compare.py` | `SHOW CRITICAL SECTIONS` 브래킷 비교 + 자기영향 0 (설치 경로 자동, `CUB=`·`CUB_CTV_TOOL=` 로 지정 가능) | 필요 |
| `tests/t4_trans_check.sh` | 해제 슬롯 미표시, 라이브 trid, `cubrid tranlist` 대조 | 필요 |
| `tests/demo_contention.sh` | CS 인덱스(0–17)별 표적 경합 / 스큐 부하, 전후 증분 판정 | 필요 |

픽스처 갱신: 경합 부하(`tests/demo_contention.sh <db> 45 6 1,2,9`) 중에 `cub_ctv <db> --record tests/fixtures/demodb-snapshot.rec`, `cub_ctv <db> -i --interval 0.5 --record tests/fixtures/demodb-live.rec`(약 5초 후 Ctrl-C) 로 기록한 뒤 `tests/t5_replay.sh --update` 와 `tests/t13_verdict.sh --update` 로 골든을 다시 만든다. 기록에는 클라이언트 사용자·호스트·IP 가 들어 있으므로 공개 가능한 서버에서 기록한다.
