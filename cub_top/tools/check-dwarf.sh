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
# Symbols and DWARF on builds the tables were not made from.
#  - an -flto build renames a static to name.lto_priv.N: taken when it is the only such
#    object; two of them, a function or an undefined reference never are; an exact name wins
#  - strip -s takes .symtab, and the LOCAL roots with it: said, not reported as a lookup miss
#  - debug info it cannot read (compressed -gz / .zdebug, .gnu_debuglink, split DWARF, dwz)
#    is named as such instead of "no key resolved" or "stripped"
#  - the offsets come back on the child's pipe and are kept in memory, keyed by the GNU
#    build-id (size+mtime collide across cp -p and tar); no file is written anywhere
#  - live, extraction runs in a background child; the frame does not wait for it
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d /tmp/cub_top-dwarf.XXXXXX); trap 'rm -rf "$TMP"' EXIT
command -v objcopy >/dev/null || { echo "→ SKIP (no objcopy)"; exit 0; }
cd "$TMP" || exit 1
# A library with the shapes the extractor looks for (a struct named session_state)
cat > lib.c <<'C'
struct session_state { int a; long b; char c[296]; };
struct session_state the_session;
int the_count;
C
gcc -g -O0 -shared -fPIC -o lib.so lib.c || { echo "→ FAIL (build)"; exit 1; }
gcc -g -O0 -shared -fPIC -Wl,--build-id=none -o nobid.so lib.c
cp lib.so gabi.so; objcopy --compress-debug-sections=zlib-gabi gabi.so
cp lib.so gnu.so;  objcopy --compress-debug-sections=zlib-gnu gnu.so 2>/dev/null
objcopy --only-keep-debug lib.so lib.dbg; cp lib.so link.so; strip -g link.so; objcopy --add-gnu-debuglink=lib.dbg link.so
gcc -g -gsplit-dwarf -O0 -shared -fPIC -o split.so lib.c 2>/dev/null
if command -v dwz >/dev/null; then   # dwz -m moves shared DIEs into a common file named by .gnu_debugaltlink
    cp lib.so dwz1.so; gcc -g -O0 -shared -fPIC -o dwz2.so lib.c -Wl,--build-id=sha1 -DX=1
    dwz -m common.debug dwz1.so dwz2.so >/dev/null 2>&1 || rm -f dwz1.so
    readelf -S dwz1.so 2>/dev/null | grep -q gnu_debugaltlink || rm -f dwz1.so
fi
cp lib.so stripped.so; strip -s stripped.so
echo 'int other_build=7;' > other.c; gcc -shared -fPIC -o other.so other.c; strip -s other.so   # another build-id
# Symbols: an -flto-style suffix, an ambiguous pair, a function, an exact name beside a suffix
cat > syms.c <<'C'
static int pool_obj __asm__("_ZL10pgbuf_Pool.lto_priv.0") __attribute__((used)) = 1;   /* LOCAL, as -flto makes it */
int amb1 __asm__("_ZL15catalog_Hashmap.lto_priv.0") = 1;
int amb2 __asm__("_ZL15catalog_Hashmap.lto_priv.1") = 2;
void fn(void) __asm__("_ZL8sessions"); void fn(void){}
int xg __asm__("xcache_Global") = 3;
int xg2 __asm__("xcache_Global.lto_priv.3") = 4;
C
gcc -shared -fPIC -o syms.so syms.c || { echo "→ FAIL (build syms)"; exit 1; }
# A real -flto build with several partitions: the static becomes _ZL10pgbuf_Pool.lto_priv.0
printf 'struct pool { int num_buffers; long x; };\nstatic pool pgbuf_Pool;\nint get_a(){ return pgbuf_Pool.num_buffers++; }\n' > a.cpp
printf 'static int other_static;\nint get_b(){ return other_static++; }\n' > b.cpp
command -v g++ >/dev/null && g++ -O2 -flto -flto-partition=max -fPIC -shared -o lto.so a.cpp b.cpp 2>/dev/null
cp syms.so syms_s.so; strip -s syms_s.so
cp "$SRC" src.c
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > t.c
cat >> t.c <<'C'
static int bad=0;
static void why_is(const char *so,const char *want){
    long o[DW_N]; char w[160]=""; int got=dwoff_extract(so,o,w,sizeof w);
    int ok = strstr(w,want)!=NULL;
    printf("  %s   %-10s -> %d key(s), \"%s\"\n",ok?"ok  ":"FAIL",so,got,w); bad|=!ok;
}
int main(void){
    g_ascii=1; int ok;
    /* symbols */
    const char *nm[3]={"_ZL10pgbuf_Pool","_ZL15catalog_Hashmap","_ZL8sessions"};
    unsigned long v[3]={0}; int n=sym_lookup("syms.so",nm,v,3,NULL);
    ok = n==1 && v[0] && !v[1] && !v[2];
    printf("  %s   lookup: .lto_priv.0 taken (%s), two suffixed not (%s), a function not (%s)\n",ok?"ok  ":"FAIL",
           v[0]?"yes":"no",v[1]?"taken":"refused",v[2]?"taken":"refused"); bad|=!ok;
    if(access("lto.so",F_OK)==0){
        FILE *q=popen("nm lto.so | awk '/pgbuf_Pool/{print $3}'","r"); char ln[128]=""; if(q){ if(!fgets(ln,sizeof ln,q)) ln[0]=0; pclose(q); }
        ln[strcspn(ln,"\n")]=0;
        const char *nl[1]={"_ZL10pgbuf_Pool"}; unsigned long vl[1]={0}; sym_lookup("lto.so",nl,vl,1,NULL);
        ok = vl[0]!=0;
        printf("  %s   real -flto build: %s found as _ZL10pgbuf_Pool\n",ok?"ok  ":"FAIL",ln); bad|=!ok;
    } else printf("  SKIP   real -flto build (g++ -flto unavailable)\n");
    const char *nx[1]={"xcache_Global"}; unsigned long vx[1]={0}, sz[1]={0};
    sym_lookup("syms.so",nx,vx,1,sz);
    { FILE *q=popen("nm syms.so | awk '$3==\"xcache_Global\"{print $1}'","r"); char hb[64]=""; if(q){ if(!fgets(hb,sizeof hb,q)) hb[0]=0; pclose(q); }
      unsigned long want=strtoul(hb,NULL,16);
      ok = vx[0]==want && want!=0 && g_sym_symtab==1;
      printf("  %s   exact name beside a suffixed one: the exact one (0x%lx, nm 0x%lx), .symtab seen\n",ok?"ok  ":"FAIL",vx[0],want); bad|=!ok; }
    unsigned long vs[1]={0}; const char *ns[1]={"_ZL10pgbuf_Pool"}; sym_lookup("syms_s.so",ns,vs,1,NULL);
    ok = vs[0]==0 && g_sym_symtab==0;
    printf("  %s   strip -s: local root not found, no .symtab reported (%d)\n",ok?"ok  ":"FAIL",g_sym_symtab); bad|=!ok;
    /* DWARF */
    { long o[DW_N]; char w[160]=""; int got=dwoff_extract("lib.so",o,w,sizeof w);
      ok = got>0; printf("  %s   plain DWARF: %d key(s)\n",ok?"ok  ":"FAIL",got); bad|=!ok; }
    why_is("gabi.so","SHF_COMPRESSED");
    if(access("gnu.so",F_OK)==0) why_is("gnu.so",".zdebug");
    why_is("link.so",".gnu_debuglink");
    if(access("dwz1.so",F_OK)==0){ long o[DW_N]; char w[160]=""; int got=dwoff_extract("dwz1.so",o,w,sizeof w);
        ok = got>0 || strstr(w,"gnu_debugaltlink");
        printf("  %s   dwz1.so    -> %d key(s)%s%s\n",ok?"ok  ":"FAIL",got,got?"":", ",got?"":w); bad|=!ok; }
    else printf("  SKIP   dwz (not available or nothing shared)\n");
    if(access("split.so",F_OK)==0) why_is("split.so","split DWARF");
    why_is("stripped.so","stripped");
    /* memo key: the build-id, else path/size/mtime */
    char bid[132], k1[160], k2[160]; elf_build_id("lib.so",bid,sizeof bid);
    dw_lib_key("lib.so",k1,sizeof k1); dw_lib_key("nobid.so",k2,sizeof k2);
    FILE *r=popen("readelf -n lib.so | sed -n 's/.*Build ID: //p'","r"); char rb[132]=""; if(r){ if(!fgets(rb,sizeof rb,r)) rb[0]=0; pclose(r); }
    rb[strcspn(rb,"\n")]=0;
    ok = bid[0] && !strcmp(bid,rb) && strstr(k1,bid) && !strncmp(k2,"f:",2);
    printf("  %s   memo key: build-id %s (readelf %s); without one: %.24s...\n",ok?"ok  ":"FAIL",bid,rb,k2);
    bad|=!ok;
    /* no file: count the cub_top.dw-* entries in /var/tmp before and after a live extraction */
    int before=0; { DIR*dd=opendir("/var/tmp"); struct dirent*e; while(dd&&(e=readdir(dd))) before+=!strncmp(e->d_name,"cub_top.dw-",11); if(dd) closedir(dd); }
    /* live: background extraction */
    g_pd_async=1; long o[DW_N]; char w[160]=""; double t0=now_ms();
    int st=dw_load_or_extract("lib.so",o,w,sizeof w); double first=now_ms()-t0;
    int polls=0; while(st==0 && polls<200){ usleep(50000); polls++; st=dw_load_or_extract("lib.so",o,w,sizeof w); }
    { long d[DW_N]; char dw2[160]; int dg=dwoff_extract("lib.so",d,dw2,sizeof dw2), same=dg>0;
      for(int i=0;i<NTG && same;i++) same = d[i]==o[i];
      int after=0; DIR*dd=opendir("/var/tmp"); struct dirent*e; while(dd&&(e=readdir(dd))) after+=!strncmp(e->d_name,"cub_top.dw-",11); if(dd) closedir(dd);
      ok = first<200 && st==1 && same && after==before;
      printf("  %s   live: first call returns at once (%.0fms), result after %d poll(s) through the pipe, equal to a direct extraction (%s), no file written (%d -> %d)\n",
             ok?"ok  ":"FAIL",first,polls,same?"yes":"no",before,after); bad|=!ok; }
    /* a stripped copy has the same build-id: the DWARF of its unstripped build serves it */
    st=dw_load_or_extract("stripped.so",o,w,sizeof w);
    ok = st==1; printf("  %s   stripped copy of the same build: served from memory by build-id\n",ok?"ok  ":"FAIL"); bad|=!ok;
    st=dw_load_or_extract("other.so",o,w,sizeof w); polls=0;
    while(st==0 && polls<200){ usleep(50000); polls++; st=dw_load_or_extract("other.so",o,w,sizeof w); }
    ok = st==2 && strstr(w,"stripped");
    printf("  %s   live failure comes back with its reason: \"%s\"\n",ok?"ok  ":"FAIL",w); bad|=!ok;
    /* A frame that adopted another build's table while the child ran asks dw_kid_done()
       each frame: 0 while it runs, 1 once it has exited - without collecting it, so the
       next dw_load_or_extract still reads its result and reaps it */
    memset(g_dw_memo,0,sizeof g_dw_memo);   /* forget it: extract again */
    st=dw_load_or_extract("lib.so",o,w,sizeof w); int d0=dw_kid_done(); pid_t kid=g_dw_kid;
    polls=0; while(!dw_kid_done() && polls<200){ usleep(50000); polls++; }
    int d1=dw_kid_done(), still=g_dw_kid==kid && kill(kid,0)==0;
    st=dw_load_or_extract("lib.so",o,w,sizeof w);
    int wp=waitpid(kid,NULL,WNOHANG); ok = st==1 && d0==0 && d1==1 && still && g_dw_kid==0 && wp<0; if(!ok) printf("        st=%d still=%d g_dw_kid=%d wp=%d w=%s\n",st,still,(int)g_dw_kid,wp,w);
    printf("  %s   finished child noticed (%d then %d after %d poll(s)) without being reaped early; then adopted and reaped\n",
           ok?"ok  ":"FAIL",d0,d1,polls); bad|=!ok;
    /* A child of another library that finished after an instance switch: asking for the
       current library must not report it (which would re-run the ladder every frame) and
       must reap it, so it is neither asked about again nor left a zombie */
    { memset(g_dw_memo,0,sizeof g_dw_memo);   /* forget it: extract again */
      st=dw_load_or_extract("lib.so",o,w,sizeof w); pid_t kid=g_dw_kid;
      int polls=0; while(!dw_kid_done() && polls<200){ usleep(50000); polls++; }
      int r1=dw_kid_finished_for("nobid.so"), r2=dw_kid_finished_for("nobid.so");
      int wp=waitpid(kid,NULL,WNOHANG);
      ok = st==0 && kid>0 && r1==0 && r2==0 && g_dw_kid==0 && g_dw_kfd<0 && wp<0;
      printf("  %s   another library's finished child: not reported (%d, %d), reaped (kid %d, waitpid %d)\n",
             ok?"ok  ":"FAIL",r1,r2,(int)g_dw_kid,wp); bad|=!ok;
      /* its answer was kept: switching back to that library adopts at once, no new child */
      st=dw_load_or_extract("lib.so",o,w,sizeof w);
      ok = st==1 && g_dw_kid==0;
      printf("  %s   switching back to that library: served from memory at once (st %d, no child)\n",ok?"ok  ":"FAIL",st); bad|=!ok;
      /* the same library's: reported, so the ladder runs once more */
      memset(g_dw_memo,0,sizeof g_dw_memo);   /* forget it: extract again */
      st=dw_load_or_extract("lib.so",o,w,sizeof w);
      polls=0; while(!dw_kid_done() && polls<200){ usleep(50000); polls++; }
      int r3=dw_kid_finished_for("lib.so");
      st=dw_load_or_extract("lib.so",o,w,sizeof w);
      ok = r3==1 && st==1 && g_dw_kid==0;
      printf("  %s   this library's finished child: reported (%d), then adopted and reaped\n",ok?"ok  ":"FAIL",r3); bad|=!ok; }
    /* Switching to another instance's library while a child still extracts: the child is
       stopped and its pipe closed, every time - no descriptor left per switch.  The child
       is held with SIGSTOP so it is still running when the switch comes. */
    { int nfd0=0, nfd1=0; DIR *dd; struct dirent *de;
      memset(g_dw_memo,0,sizeof g_dw_memo);   /* forget both: extract again */
      if((dd=opendir("/proc/self/fd"))){ while((de=readdir(dd))) nfd0++; closedir(dd); }
      for(int sw=0;sw<6;sw++){
          const char *so = (sw&1) ? "nobid.so" : "lib.so";
          dw_load_or_extract(so,o,w,sizeof w);
          if(g_dw_kid>0) kill(g_dw_kid,SIGSTOP);
      }
      dw_kid_stop();
      if((dd=opendir("/proc/self/fd"))){ while((de=readdir(dd))) nfd1++; closedir(dd); }
      ok = nfd1==nfd0 && g_dw_kfd<0 && g_dw_kid==0;
      printf("  %s   6 library switches during extraction: open fds %d -> %d, pipe %d (want equal, -1)\n",ok?"ok  ":"FAIL",nfd0,nfd1,g_dw_kfd); bad|=!ok; }
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o t t.c -lm -lpthread 2>cc.err || { cat cc.err; echo "→ FAIL (compile failed)"; exit 1; }
if ./t; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
