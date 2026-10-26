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
# Old kernels (CentOS 6, 2.6.32).  /proc/<pid>/comm only exists from 2.6.33, so instances are
# found by the name in /proc/<pid>/stat - which can hold ')' and spaces, so it ends at the
# last ')'.  Before 2.6.39 /proc/<pid>/mem is readable by a tracer only and there is no
# process_vm_readv (3.2): a refusal there is the kernel's, and the reason says so rather than
# advising to run as the owner or root.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-oldkernel.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
# The status key misspelt in a copy of the source, so the parse misses RssFile as a 3.10 kernel would
sed 's/"RssFile:"/"RssFilX:"/' "$SRC" > "$TMP/src.c"; grep -q '"RssFilX:"' "$TMP/src.c" || { echo "→ FAIL (RssFile key not found)"; exit 1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
#include <sys/prctl.h>
int main(void){
    int bad=0, ok, n=0, same=0;
    DIR *d=opendir("/proc"); struct dirent *e; char a[64], c[64], p[300];
    while(d && (e=readdir(d))){
        if(e->d_name[0]<'0'||e->d_name[0]>'9') continue;
        snprintf(p,sizeof p,"/proc/%.20s/comm",e->d_name);
        if(read_file(p,c,sizeof c)<=0 || !proc_name(e->d_name,a,sizeof a)) continue;
        c[strcspn(c,"\n")]=0; n++; same+=!strcmp(a,c);
    }
    if(d) closedir(d);
    ok = n>0 && same==n;
    printf("  %s   name from stat equals comm for %d of %d processes\n",ok?"ok  ":"FAIL",same,n); bad|=!ok;
    pid_t k=fork();
    if(!k){ prctl(PR_SET_NAME,"cub) (x y"); pause(); _exit(0); }
    usleep(100000); char pk[16]; snprintf(pk,sizeof pk,"%d",(int)k);
    ok = proc_name(pk,a,sizeof a) && !strcmp(a,"cub) (x y");
    printf("  %s   a name holding ') (' and spaces: \"%s\"\n",ok?"ok  ":"FAIL",a); bad|=!ok;
    kill(k,SIGKILL); waitpid(k,NULL,0);
    static const struct { const char *r; int before; } kr[]={
        {"2.6.32-754.35.1.el6.x86_64",1},{"2.6.38",1},{"2.6.39",0},{"3.10.0-1160.el7.x86_64",0},{"4.18.0-553.el8",0},{"6.1",0}};
    int all=1; for(unsigned i=0;i<sizeof kr/sizeof kr[0];i++) if(release_before(kr[i].r,2,6,39)!=kr[i].before){ all=0; printf("        %s\n",kr[i].r); }
    printf("  %s   older than 2.6.39: el6 2.6.32 and 2.6.38 yes; 2.6.39, el7, el8, 6.1 no\n",all?"ok  ":"FAIL"); bad|=!all;
    /* RssFile is a 4.5 field: without it the file-backed share comes from statm, not a 0
       (the source copy above has the key misspelt; the real status is read here) */
    { double r,f,w; status_mem(getpid(),&r,&f,&w);
      char st[4096]; unsigned long rf=0,rs=0; char *q;
      if(read_file("/proc/self/status",st,sizeof st)>0){
          if((q=strstr(st,"RssFile:"))) sscanf(q+8,"%lu",&rf);
          if((q=strstr(st,"RssShmem:"))) sscanf(q+9,"%lu",&rs); }
      double want=(rf+rs)*1024.0;
      ok = f>0 && want>0 && f>=want*0.8 && f<=want*1.2;
      printf("  %s   no RssFile line: file-backed %.0fKB from statm (status RssFile+RssShmem %.0fKB)\n",ok?"ok  ":"FAIL",f/1024,want/1024); bad|=!ok; }
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
