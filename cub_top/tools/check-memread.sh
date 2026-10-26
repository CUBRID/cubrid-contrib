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
# Reasons speak to the account that started the server: never "run as root" or a capability.
# Reading the server's memory touches it (the kernel marks read pages accessed and swaps
# swapped ones back in), and a read can be denied.  cub_top runs as the server's owner,
# not root, and must stay non-invasive and light.
#  1. cap_probe tests the target, not itself: under Yama ptrace_scope>=1 a same-uid
#     non-child gets EPERM from process_vm_readv and EACCES from /proc/pid/mem while
#     smaps stays readable.  A preload shim reproduces that (self reads pass), and
#     the probe must turn reading off and report perm.limited with the scope.
#  2. Page headers: only a sample of pages the engine used recently (LRU zones 1-2) is
#     read, never cold ones, and a page not resident is skipped rather than swapped in.
#     A buffer above PG_LARGE BCBs is not scanned unless asked for.
#  3. With a server running: engine aggregates agree with the BCB scan, --hot is not an option,
#     no clear_refs is ever opened, the shim on the real target, and replay of the keys.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"; BIN="$ROOT/cub_top"
TMP=${TMPDIR:-/tmp}/cub_top-memread.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
bad=0
cat > "$TMP/yama.c" <<'C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static int other_mem(const char*p){ int pid; char t[8];
  return sscanf(p,"/proc/%d/%7s",&pid,t)==2 && !strcmp(t,"mem") && pid!=getpid(); }
long syscall(long n,...){ va_list ap; va_start(ap,n); long a[6]; for(int i=0;i<6;i++) a[i]=va_arg(ap,long); va_end(ap);
  static long(*real)(long,...); if(!real) real=(long(*)(long,...))dlsym(RTLD_NEXT,"syscall");
  if(n==SYS_process_vm_readv && (pid_t)a[0]!=getpid()){ errno=EPERM; return -1; }
  return real(n,a[0],a[1],a[2],a[3],a[4],a[5]); }
#define OPEN(fn) int fn(const char*p,int f,...){ static int(*real)(const char*,int,...); \
  if(!real) real=(int(*)(const char*,int,...))dlsym(RTLD_NEXT,#fn); \
  if(other_mem(p)){ errno=EACCES; return -1; } va_list ap; va_start(ap,f); int m=va_arg(ap,int); va_end(ap); return real(p,f,m); }
OPEN(open) OPEN(open64)
FILE *fopen(const char*p,const char*m){ static FILE*(*real)(const char*,const char*);
  if(!real) real=(FILE*(*)(const char*,const char*))dlsym(RTLD_NEXT,"fopen");
  const char*sc=getenv("YAMA_SCOPE");
  if(sc && !strcmp(p,"/proc/sys/kernel/yama/ptrace_scope")) return fmemopen((void*)sc,strlen(sc),"r");
  return real(p,m); }
FILE *fopen64(const char*p,const char*m){ return fopen(p,m); }
C
gcc -shared -fPIC -O1 -o "$TMP/yama.so" "$TMP/yama.c" -ldl 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (shim)"; exit 1; }

# PG_LARGE lowered so the fake pool counts as large in the last case
sed 's/^#define PG_LARGE 262144 /#define PG_LARGE 500 /' "$SRC" > "$TMP/src.c"
grep -q '^#define PG_LARGE 500 ' "$TMP/src.c" || { echo "→ FAIL (PG_LARGE not found)"; exit 1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
/* Fake buffer pool in this process: 600 BCBs - 50 in zone 1, 100 in zone 2, 450 cold
   (zone 3) whose headers disagree with their BCB, so reading any of them shows up as a
   mismatch.  Ten zone-1 headers sit in a page never touched (not resident). */
#define NB 600
static unsigned char pool_[4096], *bcbs_, *iop_[NB], *cold_page;
static void put_bcb(int i,int pageid,uint32_t flags){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int16_t v=0; uint64_t ol=~(uint64_t)0;
    memcpy(b+g_bt->pg.vpid,&pageid,4); memcpy(b+g_bt->pg.vpid+4,&v,2);
    memcpy(b+g_bt->pg.flags,&flags,4); memcpy(b+g_bt->pg.olsa,&ol,8);
    uint64_t ip=(uint64_t)(uintptr_t)iop_[i]; memcpy(b+g_bt->pg.iopg,&ip,8);
}
static void put_hdr(int i,int pageid){
    unsigned char*h=iop_[i]+g_bt->pg.iobuf_iopg; int16_t v=0; uint64_t l=5;
    memcpy(h,&l,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
static int resident(void*a){ return pm_present(getpid(),(unsigned long)(uintptr_t)a)==1; }
int main(int c,char**v){
    if(c>1 && !strcmp(v[1],"probe")){            /* 1: probe a child target */
        pid_t ch=fork(); if(ch==0){ pause(); _exit(0); }
        usleep(100000);
        cap_probe(ch);
        printf("vm_readv=%d mem_pread=%d perm.limited=%d why=%s\n",CAP.vm_readv,CAP.mem_pread,g_perm_limited,g_perm_why);
        kill(ch,SIGKILL); waitpid(ch,NULL,0); return 0;
    }
    int bad=0; static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb);
    cold_page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    for(int i=0;i<NB;i++) iop_[i]=calloc(1,(size_t)g_bt->pg.iobuf_iopg+64);
    for(int i=0;i<10;i++) iop_[i]=cold_page+i*32-g_bt->pg.iobuf_iopg+64;   /* headers inside the untouched page */
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++){
        uint32_t z = i<50 ? 0x10000u : i<150 ? 0x20000u : 0x30000u;
        put_bcb(i,1000+i,z);
        if(i>=10) put_hdr(i, i<150 ? 1000+i : 9000+i);   /* cold headers disagree */
    }
    g_pg_detail=1;                                /* 2a: sample over a requested scan */
    pgbuf_scan(&P);
    int still_out=!resident(cold_page);
    printf("  sample: %d headers read (want 140 = zone 1-2 minus 10 not resident), %d skipped as not resident, cold mismatches %d (want 0), untouched page still not resident: %s\n",
           PG.nhdr,PG.nonres,PG.mismatch_raw,still_out?"yes":"NO");
    if(!PG.ok || PG.nhdr!=140 || PG.nonres!=10 || PG.mismatch_raw!=0 || !still_out) bad=1;
    g_pg_detail=0;                                /* 2b: large buffer, not asked for: one pass, then the pass stands */
    pgbuf_scan(&P); int h1=PG.nhdr;
    printf("  large buffer unasked: the first frame makes one pass (enabled=%d large=%d from_last=%d, %d headers)\n",PG.ok,PG.large,PG.from_last,PG.nhdr);
    if(!PG.ok || !PG.large || PG.from_last) bad=1;
    pgbuf_scan(&P);
    printf("  the next frame: no pass, no header read (from_last=%d, headers still %d)\n",PG.from_last,PG.nhdr);
    if(!PG.from_last || PG.nhdr!=h1) bad=1;
    return bad;
}
C
gcc -O0 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }

probe(){ # label want-regex reject-regex env...
    lab=$1; want=$2; rej=$3; shift 3
    out=$(env "$@" "$TMP/t" probe)
    if echo "$out" | grep -Eq "$want" && { [ -z "$rej" ] || ! echo "$out" | grep -Eq "$rej"; }; then echo "  ok   $lab: $out"
    else echo "  FAIL $lab: $out (want /$want/${rej:+, not /$rej/})"; bad=1; fi
}
echo "1. probe reads the target"
probe "no restriction"  'vm_readv=1 .*perm.limited=0'                       ''                X=1
probe "denied"          'vm_readv=0 mem_pread=0 perm.limited=1 why=.*EPERM' 'ptrace_scope|root|CAP_' LD_PRELOAD="$TMP/yama.so"
probe "scope 1"         'ptrace_scope=1.*ptrace_scope=0'                    'root|CAP_'       LD_PRELOAD="$TMP/yama.so" YAMA_SCOPE=1
probe "scope 3"         'perm.limited=1 .*ptrace_scope=3'                   'root|CAP_'       LD_PRELOAD="$TMP/yama.so" YAMA_SCOPE=3
echo "2. page headers: recent sample only, resident only; large buffer: one pass, then on its period"
"$TMP/t" || bad=1

# 2c. Residency is read afresh every frame: header pages that come in or go out between two
#     frames are read or skipped by what the pagemap says now.  All io pages share one 2MB
#     pagemap block, so a block kept from the last frame would be reused.
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/f.c"
cat >> "$TMP/f.c" <<'C'
#define NB 40
static unsigned char pool_[4096], *bcbs_, *iop_[NB];
int main(void){
    int bad=0; static proc_t P; P.pid=getpid(); CAP.vm_readv=1; g_ascii=1;
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb);
    unsigned char *raw=mmap(NULL,4u<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *blk=(unsigned char*)(((uintptr_t)raw+(2u<<20)-1)&~(uintptr_t)((2u<<20)-1));   /* one 2MB block */
    for(int i=0;i<NB;i++) iop_[i]=blk+(size_t)i*8192;
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++){ unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int pid=500+i; int16_t v=0; uint32_t z=0x10000u; uint64_t ol=~0ull;
        memcpy(b+g_bt->pg.vpid,&pid,4); memcpy(b+g_bt->pg.vpid+4,&v,2); memcpy(b+g_bt->pg.flags,&z,4); memcpy(b+g_bt->pg.olsa,&ol,8);
        uint64_t ip=(uint64_t)(uintptr_t)iop_[i]; memcpy(b+g_bt->pg.iopg,&ip,8); }
    pgbuf_scan(&P); int r1=PG.nhdr, n1=PG.nonres;                       /* frame 1: none resident */
    for(int i=0;i<NB;i++){ unsigned char*h=iop_[i]+g_bt->pg.iobuf_iopg; int pid=500+i; int16_t v=0; uint64_t l=5;
        memcpy(h,&l,8); memcpy(h+8,&pid,4); memcpy(h+12,&v,2); }            /* they come in */
    pgbuf_scan(&P); int r2=PG.nhdr, n2=PG.nonres, ok2=PG.ok;              /* frame 2 */
    madvise(blk,(size_t)NB*8192,MADV_DONTNEED);                           /* they go out */
    pgbuf_scan(&P); int r3=PG.nhdr, n3=PG.nonres;                       /* frame 3 */
    int ok = r1==0 && n1==NB && r2==NB && n2==0 && ok2 && r3==0 && n3==NB;
    printf("  %s   residency per frame: read %d/%d/%d, not resident %d/%d/%d (want 0/%d/0, %d/0/%d)\n",
           ok?"ok  ":"FAIL",r1,r2,r3,n1,n2,n3,NB,NB,NB);
    return !ok;
}
C
gcc -O0 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/f" "$TMP/f.c" -lm -lpthread 2>"$TMP/ccf.err" || { cat "$TMP/ccf.err"; echo "→ FAIL (compile failed)"; exit 1; }
"$TMP/f" || bad=1

echo "3. live target"
if [ -x "$BIN" ] && "$BIN" -d 2>/dev/null | grep -q '^heapB.enabled=1'; then
    if ldd "$BIN" >/dev/null 2>&1; then     # a static binary ignores the preload shim
    out=$(LD_PRELOAD="$TMP/yama.so" YAMA_SCOPE=1 timeout 10 "$BIN" -d 2>/dev/null)
    if echo "$out" | grep -q '^perm.limited=1' && echo "$out" | grep -q 'heapB.enabled=0 heapB.reason="[^"]*ptrace_scope=1'; then
        echo "  ok   denied read: perm.limited=1, method B reason names ptrace_scope"
    else echo "  FAIL denied read: $(echo "$out" | grep -E '^perm|^heapB.enabled')"; bad=1; fi
    else echo "  SKIP denied read on the live target (static binary)"; fi
    out=$(timeout 10 "$BIN" -d 2>/dev/null)
    python3 - <<PY || bad=1
import re,sys
t="""$out"""
kv=dict(re.findall(r'([a-z0-9_.]+)=("[^"]*"|\S+)',t))
g=lambda k: int(kv.get(k,'-1'))
if kv.get('pgbuf.enabled')!='1' or kv.get('pgbuf.engine.enabled')!='1':
    print("  SKIP aggregates vs scan (enabled %s/%s)"%(kv.get('pgbuf.enabled'),kv.get('pgbuf.engine.enabled'))); sys.exit(0)
pairs=[('pgbuf.zone1_hot','pgbuf.engine.zone1'),('pgbuf.zone2_warm','pgbuf.engine.zone2'),('pgbuf.zone3_cold','pgbuf.engine.zone3'),
       ('pgbuf.resident','pgbuf.engine.lru_pages'),('pgbuf.dirty','pgbuf.engine.dirty')]
nb=max(g('pgbuf.num_buffers'),1)
diff=[(a,g(a),g(b)) for a,b in pairs if abs(g(a)-g(b))>nb//100]
print("  %s   engine aggregates agree with the BCB scan (within 1%% of %d slots): %s"%("ok" if not diff else "FAIL",nb,diff or [(a,g(a)) for a,_ in pairs]))
sys.exit(1 if diff else 0)
PY
    if "$BIN" -d --hot >/dev/null 2>"$TMP/hot.err"; then echo "  FAIL --hot accepted"; bad=1
    else echo "  ok   --hot is not an option: $(head -1 "$TMP/hot.err" | cut -c1-80)"; fi
    n=$(printf '%s' "$out" | grep -c 'hot_pct\|^hot\.')
    if [ "$n" = 0 ]; then echo "  ok   no hot keys in the dump"; else echo "  FAIL $n hot lines in the dump"; bad=1; fi
    if command -v strace >/dev/null; then
        timeout 20 strace -f -qq -e trace=openat -o "$TMP/st" "$BIN" -d >/dev/null 2>&1
        if grep -q 'clear_refs' "$TMP/st"; then echo "  FAIL clear_refs opened"; bad=1
        else echo "  ok   no clear_refs opened ($(grep -c . "$TMP/st") openat calls traced)"; fi
    else echo "  SKIP clear_refs trace (no strace)"; fi
    "$BIN" -b --interval 0.3 --record "$TMP/h.jsonl" </dev/null >/dev/null 2>&1 & p=$!
    sleep 2; kill $p; wait $p 2>/dev/null
    "$BIN" --replay "$TMP/h.jsonl" --dump-json >"$TMP/h.out" 2>/dev/null
    python3 - "$TMP/h.jsonl" "$TMP/h.out" <<'PY' || bad=1
import json,sys
rec=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
rep=[json.loads(l) for l in open(sys.argv[2]) if l.strip()]
hk=lambda k: k.startswith("pgbuf.") or k.startswith("caps.") or k.startswith("perm.")
diff=[(i,k,a.get(k),b.get(k)) for i,(a,b) in enumerate(zip(rec,rep)) for k in sorted(set(filter(hk,a))|set(filter(hk,b))) if a.get(k)!=b.get(k)]
ok=len(rec)==len(rep) and len(rec)>0 and not diff
print("  %s   replay keeps pgbuf/engine/caps/perm keys in every frame (%d frames)%s" % ("ok" if ok else "FAIL",len(rec),"" if ok else " %s"%diff[:4]))
sys.exit(0 if ok else 1)
PY
else
    echo "  SKIP (no server with method B)"
fi
if [ $bad = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
