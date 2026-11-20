#!/bin/bash
# demo_contention.sh - cub_ctv 동작 확인용 CS 경합 유발 데모.
#
# CS 인덱스(0-17)를 지정하면 해당 크리티컬 섹션을 겨냥한 워크로드를
# 병렬로 돌리면서 cub_ctv -i 로 캡처하고, 전/후 스냅샷의 nenter/nwait
# 증분으로 "그 CS 가 실제로 움직였는가/경합했는가"를 판정한다.
# 모든 부하는 cub_ctv_demo_* 전용 일반 테이블에만 쓰며 종료 시 DROP 한다.
#
# usage: demo_contention.sh [dbname|pid] [duration_s] [workers] [cs-index[,idx...]|skew]
#        cs-index 생략 또는 'skew' = 스큐 부하(기본):
#          - 키 분포 70/20/10: 핫 10키 / 웜 90키 / 콜드 900키 (UPDATE 60%
#            + SELECT 25% + INSERT 15% 혼합, 커밋마다 autocommit)
#          - 세션 스큐: 워커 1 = 핫 세션(타 워커의 3배 문장 수) → owner 태그와
#            --trans 패널에서 "누가 몰리는지"가 그대로 드러남
#          - 종료 후 생성 키 분포(top-5 점유율)와 워커별 문장 수 리포트 출력
#   예:  demo_contention.sh cbench 20 6          # 스큐 부하
#        demo_contention.sh cbench 20 6 7        # HEAP_CHNGUESS 표적
#        demo_contention.sh cbench 20 6 2,9      # LOCATOR + CT_OID (DDL churn)
#
# 항목별 유발 메커니즘 (SQL 로 유발 가능한 것만 구현):
#   0  WFG                  교차 UPDATE 데드락 (엔진이 WFG 를 안 쓰면 nenter 0 그대로 보고)
#   1  LOG                  병렬 autocommit INSERT (커밋마다 WAL)
#   2  LOCATOR_CLASSNAME    CREATE/DROP TABLE 반복 (클래스명 테이블 writer)
#   3  QPROC_QUERY_TABLE    병렬 다중 질의 실행 (질의 엔트리 등록/해제)
#   4  QPROC_LIST_CACHE     동일 GROUP BY 질의 반복 (list cache 조회)
#   5  DISK_CHECK           대량 INSERT 성장 (볼륨 확장 검사)
#   7  HEAP_CHNGUESS        공유 테이블 병렬 INSERT (best-guess 캐시)
#   8  TRAN_TABLE           접속/종료 폭주 (트랜잭션 슬롯 할당)
#   9  CT_OID_TABLE         DDL churn (카탈로그 갱신)
#   13 PARTITION_CACHE      파티션 테이블 생성/조회/드롭
#   14 EVENT_LOG_FILE       락 타임아웃 이벤트 (row 락 보유 + 타임아웃 UPDATE)
#   16 LOG_ARCHIVE          지속 대량 DML (아카이브 회전 시에만 — best effort)
#   17 ACCESS_STATUS        접속 폭주 (로그인 기록)
# 외부 유발 불가(미지원): 6 CNV_FMT_LEXER, 10 HA_SERVER_STATE(비HA),
#   11 COMPACTDB(전용 유틸·데이터 변경), 12 ACL(비활성 파라미터), 15 TRACE_LOG
set -u
DB=${1:-cbench}
DUR=${2:-20}
W=${3:-6}
IDXS=${4:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
CUB_CTV=$HERE/cub_ctv
OUT=${DEMO_OUT:-$(mktemp -d)}
mkdir -p "$OUT"
# csql 은 현재 디렉터리에 csql.err 를 남긴다: 출력 디렉터리에서 실행
OUT=$(cd "$OUT" && pwd); cd "$OUT"
UNSUPPORTED="6 10 11 12 15"

# ---- 대상 서버의 설치 루트/DB 경로를 프로세스에서 역추적 (환경 무가정)
# DB 는 이름 또는 pid; 같은 이름의 서버가 여럿이면 pid 로 지정
ENV=$(CUB_CTV_TOOL="$CUB_CTV" python3 "$HERE/tests/server_env.py" "$DB") \
  || { echo "demo: server for '$DB' not found"; exit 1; }
eval "$ENV"
export CUBRID=$INSTALL CUBRID_DATABASES=$DBDIR LD_LIBRARY_PATH=$INSTALL/lib
CSQL=("$INSTALL/bin/csql" -u dba -C "$DB")	# an array: install paths may hold spaces

run_sql () { timeout "$DUR" "${CSQL[@]}" -i "$1" >/dev/null 2>&1; }
# csql 접속 churn 루프 (한 접속 = 로그인+트랜잭션 슬롯 왕복)
churn_loop () {
  local end=$((SECONDS + DUR))
  while [ $SECONDS -lt $end ]; do
    "${CSQL[@]}" -c "SELECT 1;" >/dev/null 2>&1
  done
}

# ---- 인덱스별 워크로드 생성기: gen_<idx> 는 배경 작업을 띄우고 PID들을 WPIDS에 추가
WPIDS=()
CLEANUP_SQL=()

start_insert_workers () {           # 1,5,16: 워커별 테이블 병렬 INSERT
  local i j
  for i in $(seq 1 "$W"); do
    { echo "DROP TABLE IF EXISTS cub_ctv_demo_w$i;"
      echo "CREATE TABLE cub_ctv_demo_w$i (a INT, b VARCHAR(64));"
      for j in $(seq 1 20000); do
        echo "INSERT INTO cub_ctv_demo_w$i VALUES ($j, 'demo-row-$j');"
      done
      echo "DROP TABLE cub_ctv_demo_w$i;"
    } > "$OUT/ins$i.sql"
    run_sql "$OUT/ins$i.sql" & WPIDS+=($!)
    CLEANUP_SQL+=("DROP TABLE IF EXISTS cub_ctv_demo_w$i;")
  done
}
gen_1 ()  { start_insert_workers; }
gen_5 ()  { start_insert_workers; }
gen_16 () { start_insert_workers; }

gen_7 () {                          # HEAP_CHNGUESS: 공유 테이블 병렬 INSERT
  local i j
  "${CSQL[@]}" -c "DROP TABLE IF EXISTS cub_ctv_demo_shared; CREATE TABLE cub_ctv_demo_shared (a INT, b VARCHAR(64));" >/dev/null 2>&1
  CLEANUP_SQL+=("DROP TABLE IF EXISTS cub_ctv_demo_shared;")
  for i in $(seq 1 "$W"); do
    { for j in $(seq 1 20000); do
        echo "INSERT INTO cub_ctv_demo_shared VALUES ($j, 'shared-$i-$j');"
      done
    } > "$OUT/sh$i.sql"
    run_sql "$OUT/sh$i.sql" & WPIDS+=($!)
  done
}

gen_2 () {                          # LOCATOR: CREATE/DROP churn
  local i j
  for i in $(seq 1 "$W"); do
    { for j in $(seq 1 2000); do
        echo "CREATE TABLE cub_ctv_demo_ddl_${i} (a INT);"
        echo "DROP TABLE cub_ctv_demo_ddl_${i};"
      done
    } > "$OUT/ddl$i.sql"
    run_sql "$OUT/ddl$i.sql" & WPIDS+=($!)
    CLEANUP_SQL+=("DROP TABLE IF EXISTS cub_ctv_demo_ddl_${i};")
  done
}
gen_9 () { gen_2; }                 # CT_OID: 같은 DDL churn 이 카탈로그를 갱신

gen_3 () {                          # QPROC_QUERY_TABLE: 병렬 다중 질의
  local i j
  for i in $(seq 1 "$W"); do
    { for j in $(seq 1 4000); do
        echo "SELECT COUNT(*) FROM db_class WHERE class_name <> 'x$j';"
      done
    } > "$OUT/q$i.sql"
    run_sql "$OUT/q$i.sql" & WPIDS+=($!)
  done
}
gen_4 () {                          # LIST_CACHE: 동일 GROUP BY 반복
  local i j
  for i in $(seq 1 "$W"); do
    { for j in $(seq 1 4000); do
        echo "SELECT is_system_class, COUNT(*) FROM db_class GROUP BY is_system_class ORDER BY 1;"
      done
    } > "$OUT/lc$i.sql"
    run_sql "$OUT/lc$i.sql" & WPIDS+=($!)
  done
}

gen_8 () {                          # TRAN_TABLE: 접속 churn
  local i
  for i in $(seq 1 "$W"); do
    churn_loop & WPIDS+=($!)
  done
}
gen_17 () { gen_8; }                # ACCESS_STATUS: 접속=로그인 기록

gen_13 () {                         # PARTITION_CACHE
  local i j
  "${CSQL[@]}" -c "DROP TABLE IF EXISTS cub_ctv_demo_part;
    CREATE TABLE cub_ctv_demo_part (a INT PRIMARY KEY, b VARCHAR(32))
    PARTITION BY HASH(a) PARTITIONS 8;" >/dev/null 2>&1
  CLEANUP_SQL+=("DROP TABLE IF EXISTS cub_ctv_demo_part;")
  for i in $(seq 1 "$W"); do
    { for j in $(seq 1 8000); do
        echo "INSERT INTO cub_ctv_demo_part VALUES ($((i*100000+j)), 'p$j');"
        [ $((j % 50)) -eq 0 ] && echo "SELECT COUNT(*) FROM cub_ctv_demo_part WHERE a = $((i*100000+j));"
      done
    } > "$OUT/pt$i.sql"
    run_sql "$OUT/pt$i.sql" & WPIDS+=($!)
  done
}

gen_14 () {                         # EVENT_LOG: 락 타임아웃 이벤트
  "${CSQL[@]}" -c "DROP TABLE IF EXISTS cub_ctv_demo_lock;
    CREATE TABLE cub_ctv_demo_lock (a INT PRIMARY KEY, b INT);
    INSERT INTO cub_ctv_demo_lock VALUES (1, 0);" >/dev/null 2>&1
  CLEANUP_SQL+=("DROP TABLE IF EXISTS cub_ctv_demo_lock;")
  # 홀더: 트랜잭션 열고 row 락 보유한 채 대기
  { echo ";autocommit off"
    echo "UPDATE cub_ctv_demo_lock SET b = b + 1 WHERE a = 1;"
    echo "SELECT SLEEP($DUR);"
    echo "COMMIT;"
  } > "$OUT/holder.sql"
  run_sql "$OUT/holder.sql" & WPIDS+=($!)
  sleep 1
  # 경쟁자: 짧은 lock_timeout 으로 타임아웃 이벤트 반복 유발
  local i j
  for i in $(seq 1 "$W"); do
    { echo "SET SYSTEM PARAMETERS 'lock_timeout=500';"
      for j in $(seq 1 200); do
        echo "UPDATE cub_ctv_demo_lock SET b = b + 1 WHERE a = 1;"
      done
    } > "$OUT/wt$i.sql"
    run_sql "$OUT/wt$i.sql" & WPIDS+=($!)
  done
}

prepare_skew () {                   # 스큐 부하 준비: SQL 생성 + 프리로드 (라이브 시작 전)
  # 데이터: 1,000행 프리로드. 키 선택 70%→1..10(핫), 20%→11..100(웜),
  # 10%→101..1000(콜드). 문장 혼합 UPDATE 60 / SELECT 25 / INSERT 15.
  # 세션 스큐: 워커 1이 3배 문장 수(핫 세션). 문장 수는 DUR 에 비례.
  {
    echo "DROP TABLE IF EXISTS cub_ctv_demo_skew;"
    echo "CREATE TABLE cub_ctv_demo_skew (a INT PRIMARY KEY, b INT, c VARCHAR(64));"
    echo "INSERT INTO cub_ctv_demo_skew SELECT ROWNUM, 0, 'preload' FROM db_class x, db_class y LIMIT 1000;"
  } > "$OUT/skew_load.sql"
  "${CSQL[@]}" -i "$OUT/skew_load.sql" >/dev/null 2>&1
  CLEANUP_SQL+=("DROP TABLE IF EXISTS cub_ctv_demo_skew;")

  local i stmts=$((DUR * 500))
  : > "$OUT/skew_keys.txt"
  : > "$OUT/skew_workers.txt"
  for i in $(seq 1 "$W"); do
    local n=$stmts
    [ "$i" -eq 1 ] && n=$((stmts * 3))    # 핫 세션
    # awk 로 고속 생성 (bash RANDOM 루프는 대량 생성 시 수 초 소요)
    awk -v n="$n" -v i="$i" -v seed=$((42 + i)) -v keys="$OUT/skew_keys.txt" '
      BEGIN {
        srand(seed);
        for (j = 1; j <= n; j++) {
          r = rand() * 100;
          if (r < 70)      key = int(rand() * 10) + 1;
          else if (r < 90) key = int(rand() * 90) + 11;
          else             key = int(rand() * 900) + 101;
          print key >> keys;
          r = rand() * 100;
          if (r < 60)
            printf "UPDATE cub_ctv_demo_skew SET b = b + 1, c = %sw%d-%d%s WHERE a = %d;\n", q, i, j, q, key;
          else if (r < 85)
            printf "SELECT b FROM cub_ctv_demo_skew WHERE a = %d;\n", key;
          else
            printf "INSERT INTO cub_ctv_demo_skew VALUES (%d, 0, %sins-w%d%s);\n", 100000 * i + j, q, i, q;
        }
      }' q="'" > "$OUT/skew_w$i.sql"
    echo "$i $n" >> "$OUT/skew_workers.txt"
  done
}

gen_skew () {                       # 스큐 워커 기동 + 중간 시점 --trans 스냅샷
  local i
  for i in $(seq 1 "$W"); do
    run_sql "$OUT/skew_w$i.sql" & WPIDS+=($!)
  done
  { sleep $((DUR / 2)); "$CUB_CTV" "$PID" --trans > "$OUT/trans_mid.txt" 2>&1; } &
  WPIDS+=($!)
}

report_skew () {
  [ -s "$OUT/skew_keys.txt" ] || return 0
  echo
  echo "== skew 프로필 (생성된 접근 키 분포):"
  local total
  total=$(wc -l < "$OUT/skew_keys.txt")
  sort -n "$OUT/skew_keys.txt" | uniq -c | sort -rn | head -5 |
  while read -r cnt key; do
    printf "  key %-5s %6d회 (%.1f%%)\n" "$key" "$cnt" \
      "$(echo "$cnt $total" | awk '{printf "%.1f", 100*$1/$2}')"
  done
  awk '$1<=10 {h+=1} END {printf "  핫 키(1..10) 점유율: %.1f%% (총 %d 문장)\n", 100*h/NR, NR}' \
    "$OUT/skew_keys.txt"
  echo "  워커별 문장 수(세션 스큐):"
  awk '{printf "    worker %s: %s%s\n", $1, $2, ($1==1 ? "  <-- 핫 세션" : "")}' \
    "$OUT/skew_workers.txt"
  if [ -s "$OUT/trans_mid.txt" ]; then
    echo
    echo "== 부하 중간 시점 트랜잭션 스냅샷 (cub_ctv --trans, 무접속):"
    sed -n '3,$p' "$OUT/trans_mid.txt" | sed 's/^/  /'
  fi
}

gen_0 () {                          # WFG: 교차 UPDATE 데드락 시나리오
  "${CSQL[@]}" -c "DROP TABLE IF EXISTS cub_ctv_demo_dl;
    CREATE TABLE cub_ctv_demo_dl (a INT PRIMARY KEY, b INT);
    INSERT INTO cub_ctv_demo_dl VALUES (1,0),(2,0);" >/dev/null 2>&1
  CLEANUP_SQL+=("DROP TABLE IF EXISTS cub_ctv_demo_dl;")
  local i
  for i in $(seq 1 "$W"); do
    { echo ";autocommit off"
      if [ $((i % 2)) -eq 0 ]; then o1=1; o2=2; else o1=2; o2=1; fi
      local j
      for j in $(seq 1 200); do
        echo "UPDATE cub_ctv_demo_dl SET b = b + 1 WHERE a = $o1;"
        echo "UPDATE cub_ctv_demo_dl SET b = b + 1 WHERE a = $o2;"
        echo "COMMIT;"
      done
    } > "$OUT/dl$i.sql"
    run_sql "$OUT/dl$i.sql" & WPIDS+=($!)
  done
}

# ---- 실행 -----------------------------------------------------------------
echo "== cub_ctv contention demo: db=$DB pid=$PID dur=${DUR}s workers=$W idx=${IDXS:-default}"
echo "== install=$INSTALL out=$OUT"

for x in ${IDXS//,/ }; do
  case " $UNSUPPORTED " in *" $x "*)
    echo "== idx $x: 미지원 (SQL 로 외부 유발 불가 — 스크립트 헤더 참조)";;
  esac
done

SKEW_MODE=0
if [ -z "$IDXS" ] || [ "$IDXS" = "skew" ]; then
  SKEW_MODE=1
  prepare_skew                       # SQL 생성·프리로드는 캡처 밖에서
fi

"$CUB_CTV" "$PID" --json > "$OUT/before.json"
timeout $((DUR + 3)) "$CUB_CTV" "$PID" -i --interval 2 --hz 200 \
  > "$OUT/live.txt" 2>&1 &
LIVE_PID=$!
sleep 1

if [ "$SKEW_MODE" -eq 1 ]; then
  gen_skew
else
  for x in ${IDXS//,/ }; do
    if declare -f "gen_$x" >/dev/null; then
      "gen_$x"
    fi
  done
fi
[ ${#WPIDS[@]} -gt 0 ] && wait "${WPIDS[@]}" 2>/dev/null
wait "$LIVE_PID" 2>/dev/null

for sql in "${CLEANUP_SQL[@]:-}"; do
  [ -n "$sql" ] && "${CSQL[@]}" -c "$sql" >/dev/null 2>&1
done
"$CUB_CTV" "$PID" --json > "$OUT/after.json"

report_skew

echo
echo "== live capture (활동 윈도우만):"
grep -v "^    (idle\|^    (유휴" "$OUT/live.txt"

echo
echo "== nenter/nwait 증분 (이번 실행 중 발생분):"
# 전후 스냅샷(--json)의 CS 별 nenter/nwait 차 (u32 순환 고려)
python3 - "$OUT/before.json" "$OUT/after.json" <<'PY' | tee "$OUT/verdict.txt"
import json, sys
b = {c["ix"]: c for c in json.load(open(sys.argv[1]))["cs"]}
for c in json.load(open(sys.argv[2]))["cs"]:
    p = b.get(c["ix"])
    if p is None or "nenter" not in c or "nenter" not in p:
        continue
    de = (c["nenter"] - p["nenter"]) & 0xffffffff
    dw = (c["nwait"] - p["nwait"]) & 0xffffffff
    if de > 0 or dw > 0:
        print("  %2d %-26s enter +%-8d nwait +%d" % (c["ix"], c["name"], de, dw))
PY
if grep -q "nwait +[1-9]" "$OUT/verdict.txt"; then
  echo "== 판정: 경합(대기) 발생 확인"
else
  echo "== 판정: 활동은 있으나 이번 구간 대기 미발생 (enter 증분/occ% 로 확인)"
fi
echo "== raw: $OUT"
