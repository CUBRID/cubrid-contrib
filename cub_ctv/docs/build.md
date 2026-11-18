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
| `tests/t5_replay.sh` | 기록 픽스처(`*.rec`)를 보기 6종으로 재생 == 골든(바이트), JSON 유효성, 한국어는 UTF-8 로캘에서만(그 밖은 ASCII 영문 + 경고) | 불필요 |
| `tests/t6_view_roundtrip.sh` | 대기 체인·특수문자 이름·풀을 주입한 프레임 기록→재생 동일 (`ld --wrap`) | 불필요 |
| `tests/t11_ident_delta.sh` | 신원 변화분 기록(`K`/`X`) 재생 = 전체 기록 재생, 구간 이동 신원 = 순차 | 불필요 |
| `tests/t12_field_sched.sh` | FIELD·SCHED 규칙 단위 시험 | 불필요 |
| `tests/t13_verdict.sh` | 판정 수치 골든 비교 | 불필요 |
| `tests/t14_slot_reuse.sh` | 구간 중 슬롯 재사용(`J` 신원·`W`/`S`/`C` cpid 주입): 소유자 블록·대기 홉·세션 패널이 슬롯의 현재 클라이언트가 아닌 목격된 클라이언트를 `disconnected` 로 표시 | 불필요 |
| `tests/t15_screen_fit.py` | 요약 화면을 80×24·80×40·100×30·120×30·200×50·60×20 가상 터미널에서 한/영으로 재생: 줄바꿈까지 센 화면 줄 수가 높이 − 1 이하(스크롤 없음), 80칸 이상이면 폭을 넘는 줄 없음, 넓은 터미널은 `보호 대상` 열 유지 | 불필요 |
| `tests/t16_act_samples.sh` | 샘플러 활동 표: tran 미상(−1) 대기자 둘·엔진 스레드 둘·클라이언트 둘이 각각 따로 세어져 어느 활동도 구간 샘플 수를 넘지 않고, 그 라이브 기록이 재생됨 | 불필요 |
| `tests/t17_target_pick.sh` | 대상 선택: DB 이름 우선(숫자 이름이 다른 서버 pid 와 같아도), 같은 이름 서버 여럿은 거부하고 후보별 `--pid` 안내, DB 이름이 없는 숫자는 pid, `--pid` 는 pid 로만 (대기만 하는 `cub_server` 대체 프로세스 사용, gcc 필요) | 불필요 |
| `tests/t18_demote_holder.sh` | 샘플러에 writer 3·강등 92·비움 5 샘플을 넣어 강등 구간이 owner 의 보유로 세어지고 판정의 데몬 보유가 점유와 같은지, 일반 reader 는 보유자가 아닌지 | 불필요 |
| `tests/t7_record_equiv.sh` | 실서버에서 라이브 화면 = 기록 재생 화면, 디테일 덤프 JSON·재생 거부 | 필요 |
| `tests/t8_replay_keys.py` | pty 에서 재생 키(일시정지·구간·속도·종료), 요약 화면 세션 강조, 색상·`--plain` | 불필요 |
| `tests/t10_os_probe.sh` | OS tid·스레드 이름·대기 사유를 `/proc` 과 대조 (I-17/I-18) | 필요 |
| `tests/t9_quick_trace.sh` | 빠른 확인: 부하 중 라이브 요약 1구간 + 기록, 판정·대기 세션 검사, 라이브 `r` 녹화 시작·중지 (usage.md §3) | 필요 |
| `tests/check_msg_pairs.py` | 한/영 메시지 쌍의 printf 지시자 동일성 | 불필요 |
| `tests/t2_show_compare.py` | `SHOW CRITICAL SECTIONS` 브래킷 비교 + 자기영향 0 (설치 경로 자동, `CUB=`·`CUB_CTV_TOOL=` 로 지정 가능) | 필요 |
| `tests/t4_trans_check.sh` | 해제 슬롯 미표시, 라이브 trid, `cubrid tranlist` 대조 | 필요 |
| `tests/demo_contention.sh` | CS 인덱스(0–17)별 표적 경합 / 스큐 부하, 전후 증분 판정 | 필요 |

픽스처 갱신: 경합 부하(`tests/demo_contention.sh <db> 45 6 1,2,9`) 중에 `cub_ctv <db> --record tests/fixtures/demodb-snapshot.rec`, `cub_ctv <db> -i --interval 0.5 --record tests/fixtures/demodb-live.rec`(약 5초 후 Ctrl-C) 로 기록한 뒤 `tests/t5_replay.sh --update` 와 `tests/t13_verdict.sh --update` 로 골든을 다시 만든다. 기록에는 클라이언트 사용자·호스트·IP 가 들어 있으므로 공개 가능한 서버에서 기록한다.
