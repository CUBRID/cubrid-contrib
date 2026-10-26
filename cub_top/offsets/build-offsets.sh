#!/bin/sh
#
# Copyright 2016 CUBRID Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
# build-offsets.sh - generate offsets/*.tbl for a CUBRID version from source.
#
# Usage: offsets/build-offsets.sh [옵션] <ref> [<ref> ...]
#   ref = git tag/branch/commit   예: v11.5.0.2374  v11.4.6.1963  develop
#
# Options
#   -r URL    소스 저장소 (기본 https://github.com/CUBRID/cubrid.git)
#   -o DIR    출력 디렉터리 (기본 이 스크립트가 있는 곳)
#   -w DIR    작업 디렉터리 (기본 임시 디렉터리, 끝나면 지움)
#   -j DIR    JAVA_HOME (CUBRID build.sh 의 -j 와 같다; 생략 시 build.sh 기본값)
#   -k        작업 디렉터리를 지우지 않는다 (빌드 재사용·디버깅)
#             재사용하면 시작할 때 origin 의 브랜치·태그를 모두 fetch 하고, 실패하면 중단한다.
#             브랜치 이름은 origin/<브랜치> 를, 태그는 태그를 detached 로 체크아웃하고
#             빌드한 커밋을 출력·표(#meta source=)에 남긴다
#   -f        이미 있는 .tbl 을 덮어쓴다 (기본: 덮어쓰지 않고 건너뜀)
#
# 왜 빌드가 필요한가
#   .tbl 에 담는 값은 구조체 필드의 바이트 오프셋과 sizeof 다. 이것은 컴파일러가
#   패딩·정렬·빌드 플래그에 따라 정하는 값이라 소스 텍스트만으로는 확정할 수 없다.
#   그래서 해당 ref 를 실제로 빌드해 libcubrid.so 를 만들고, 그 DWARF 에서 뽑는다.
#
#   추출한 표는 후보일 뿐이다. 채택은 대상 환경에서 cub_top 의 3중 게이트
#   (심볼 존재·오프셋 정렬·paramdump 교차검증)를 통과해야 한다.
#
#   선택 순서: cub_top 은 감지한 버전과 일치하는 **내장 표**를 먼저 쓴다. 따라서
#   내장 표가 있는 버전에서는 --offsets 로 준 표가 쓰이지 않는다. 이 스크립트가
#   필요한 경우는 내장 표가 없는 버전이거나, 같은 메이저라도 구조체가 바뀐 패치
#   버전이다(실제로 11.5.0.2374 와 11.5.0.2548 은 SZ_CSS_CONN_ENTRY 가 480 vs 488 로 다르다).
#
# 요구사항
#   git, cmake(3.x), gcc/g++, make, ant, JDK — CUBRID 빌드 요구와 같다.
#   빌드는 반드시 **디버그 정보가 있는 모드**여야 한다(아래 build.sh debug).
set -u

REPO="https://github.com/CUBRID/cubrid.git"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT="$HERE"
WORK=""
KEEP=0
FORCE=0
JAVA_HOME_OPT=""

while [ $# -gt 0 ]; do
    case "$1" in
        -r) REPO=$2; shift 2 ;;
        -o) OUT=$2;  shift 2 ;;
        -w) WORK=$2; shift 2 ;;
        -j) JAVA_HOME_OPT=$2; shift 2 ;;
        -k) KEEP=1;  shift ;;
        -f) FORCE=1; shift ;;
        -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; break ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *)  break ;;
    esac
done
[ $# -ge 1 ] || { echo "usage: $0 [-r URL] [-o DIR] [-w DIR] [-j JAVA_HOME] [-k] [-f] <ref> ..." >&2; exit 2; }

# 정리는 이번 실행이 만든 것만: 이미 있던 tools/dwoff 와 -w 로 받은 디렉터리는 지우지 않는다.
DWOFF_TMP=""; WORK_TMP=""
cleanup(){ [ -n "$DWOFF_TMP" ] && rm -rf "$DWOFF_TMP"; [ -n "$WORK_TMP" ] && rm -rf "$WORK_TMP"; }
trap cleanup EXIT
trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM   # 시그널로 끝나도 EXIT 정리가 돈다

# dwoff: 추출기. 없으면 여기서 빌드한다(단일 파일, 외부 의존 0).
DWOFF="$HERE/../tools/dwoff"
if [ ! -x "$DWOFF" ]; then
    DWOFF_TMP=$(mktemp -d /tmp/dwoff.XXXXXX) || exit 1
    DWOFF="$DWOFF_TMP/dwoff"
    ${CC:-cc} -O2 -std=gnu99 -o "$DWOFF" "$HERE/../tools/dwoff.c" \
        || { echo "dwoff build failed" >&2; exit 1; }
fi

if [ -z "$WORK" ]; then
    WORK=$(mktemp -d /tmp/cubofs.XXXXXX) || exit 1
    [ $KEEP -eq 0 ] && WORK_TMP=$WORK
fi
mkdir -p "$WORK" "$OUT" || exit 1

SRC="$WORK/cubrid"
ok=0; fail=0; skip=0

# 한 번만 받고 ref 마다 체크아웃한다 — ref 마다 clone 하면 1GB 넘게 중복된다.
if [ ! -d "$SRC/.git" ]; then
    echo "clone   $REPO"
    git clone --quiet "$REPO" "$SRC" || { echo "clone failed" >&2; exit 1; }
else
    # A reused work tree (-w/-k): bring every branch and tag up to the remote.  A failed fetch
    # stops the run - building from what is left would report a table of old sources as OK.
    echo "fetch   $(cd "$SRC" && git remote get-url origin 2>/dev/null)"
    ( cd "$SRC" && git fetch --quiet --force --tags origin '+refs/heads/*:refs/remotes/origin/*' ) \
        || { echo "fetch failed - stopping rather than building old sources" >&2; exit 1; }
fi

for REF in "$@"; do
    echo "----- $REF -----"

    # 기존 표는 기본적으로 건드리지 않는다. 이미 검증된 표를 빌드 환경 차이로
    # 조용히 갈아치우면, 그 표로 통과하던 환경이 설명 없이 깨질 수 있다.
    if [ $FORCE -eq 0 ]; then
        # ref 가 태그면 버전 문자열을 예측할 수 있어 빌드 전에 거를 수 있다(수십 분 절약).
        guess=$(printf '%s' "$REF" | sed 's/^v//')
        if [ -n "$guess" ] && [ -f "$OUT/cubrid-$guess.tbl" ]; then
            echo "  skip (이미 있음: cubrid-$guess.tbl — 덮어쓰려면 -f)"
            skip=$((skip+1)); continue
        fi
    fi

    # A tag as such, a branch as the remote's tip (a local branch of that name is never
    # fast-forwarded and would build old sources), else a commit.  Checked out detached.
    TARGET=$( cd "$SRC" || exit 1
        if   git rev-parse -q --verify "refs/tags/$REF^{commit}" >/dev/null; then echo "refs/tags/$REF"
        elif git rev-parse -q --verify "refs/remotes/origin/$REF^{commit}" >/dev/null; then echo "refs/remotes/origin/$REF"
        elif git rev-parse -q --verify "$REF^{commit}" >/dev/null; then echo "$REF"
        fi )
    [ -n "$TARGET" ] || { echo "  FAIL $REF 는 태그·origin 브랜치·커밋 어느 것도 아님"; fail=$((fail+1)); continue; }
    ( cd "$SRC" && git -c advice.detachedHead=false checkout --quiet --detach "$TARGET" ) \
        || { echo "  FAIL checkout $TARGET"; fail=$((fail+1)); continue; }
    SHA=$(cd "$SRC" && git rev-parse HEAD)
    echo "  source $TARGET = $SHA"

    # 디버그 정보가 있어야 DWARF 를 읽을 수 있다. build.sh 의 debug 모드를 쓴다.
    # debug 모드여야 .debug_info 가 들어간다. 병렬도는 build.sh 가 아니라 생성기가 정하므로
    # 여기서 지정하지 않는다(build.sh 의 -j 는 JAVA_HOME 이다).
    echo "  build  (debug) ... 수십 분 걸릴 수 있다"
    BLOG="$WORK/build-$(printf '%s' "$REF" | tr '/' '_').log"
    if ! ( cd "$SRC" && ./build.sh -m debug ${JAVA_HOME_OPT:+-j "$JAVA_HOME_OPT"} build >"$BLOG" 2>&1 ); then
        echo "  FAIL build  (로그: $BLOG)"
        tail -5 "$BLOG" | sed 's/^/        /'
        fail=$((fail+1)); continue
    fi

    LIB=$(find "$SRC" -name 'libcubrid.so*' -type f 2>/dev/null \
          | grep -v 'libcubridsa\|libcubridcs\|_timezones' | head -1)
    [ -n "$LIB" ] || { echo "  FAIL libcubrid.so 를 찾지 못함"; fail=$((fail+1)); continue; }

    if ! readelf -S "$LIB" 2>/dev/null | grep -q '\.debug_info'; then
        echo "  FAIL $LIB 에 .debug_info 없음 (release 빌드?)"; fail=$((fail+1)); continue
    fi

    # 버전은 lib 의 릴리스 문자열에서 읽는다 — cub_top 의 감지와 같은 출처라
    # 파일 이름과 런타임 선택이 어긋나지 않는다.
    VER=$(strings -a "$LIB" 2>/dev/null \
          | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
    [ -n "$VER" ] || VER=$(printf '%s' "$REF" | sed 's/^v//')
    [ -n "$VER" ] || { echo "  FAIL 버전 문자열을 찾지 못함"; fail=$((fail+1)); continue; }

    F="$OUT/cubrid-$VER.tbl"
    # 빌드 후에 다시 본다 — ref 이름과 실제 릴리스 문자열이 다를 수 있다(브랜치 등).
    if [ $FORCE -eq 0 ] && [ -f "$F" ]; then
        echo "  skip (이미 있음: $(basename "$F") — 덮어쓰려면 -f)"
        skip=$((skip+1)); continue
    fi

    TMP="$F.tmp.$$"
    if "$DWOFF" "$LIB" --emit-tbl >"$TMP" 2>/dev/null && [ -s "$TMP" ]; then
        # 버전 메타를 머리에 넣는다 — cub_top 이 디렉터리에서 표를 고를 때 이 줄을 본다.
        { head -1 "$TMP"; echo "#meta version=$VER"; echo "#meta source=$REF@$SHA"; tail -n +2 "$TMP"; } >"$F"
        rm -f "$TMP"
        N=$(grep -c '^[A-Z]' "$F" 2>/dev/null || echo 0)
        echo "  OK     $(basename "$F")  (항목 $N)"
        ok=$((ok+1))
    else
        rm -f "$TMP"
        echo "  FAIL   추출 실패 ($VER) — DWARF 는 있으나 대상 심볼/타입을 못 찾음"
        fail=$((fail+1))
    fi
done

echo
echo "완료: 생성 $ok · 건너뜀 $skip · 실패 $fail  →  $OUT"
[ $KEEP -eq 1 ] && echo "작업 디렉터리 유지: $WORK"
[ $fail -eq 0 ]
