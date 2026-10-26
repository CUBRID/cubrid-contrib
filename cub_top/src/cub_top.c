/*
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * cub_top - CUBRID memory observer (single-file C99, no external libraries)
 *
 * Portability target: source-compatible down to CentOS 6 / kernel 2.6.32.
 *   - C99 (gnu99) only; no C11 features, anonymous unions or _Static_assert.
 *   - No libdw/libelf; <elf.h> is used for its declarations only.
 *   - gettimeofday rather than clock_gettime (no -lrt on old glibc).
 *   - process_vm_readv is called through syscall() rather than the glibc
 *     wrapper, so it compiles on glibc < 2.15 and degrades to pread when the
 *     kernel does not provide it.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <dirent.h>
#include <grp.h>
#include <elf.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <signal.h>
#include <time.h>

/* Old kernels and glibc: define the syscall numbers we need directly */
#ifndef __NR_process_vm_readv
# if defined(__x86_64__)
#  define __NR_process_vm_readv 310
# elif defined(__i386__)
#  define __NR_process_vm_readv 347
# elif defined(__aarch64__)
#  define __NR_process_vm_readv 270
# endif
#endif

#define MAXREG   4096
#define MAXPARAM  512
#define MAXSYM      32
#define MAXHIST   2048

/* ---------------------- Common utilities ---------------------- */
static double now_ms(void){
    struct timeval tv; gettimeofday(&tv,NULL);
    return tv.tv_sec*1000.0 + tv.tv_usec/1000.0;
}
static void human(double b,char *o,size_t n){
    static const char *u[]={"B","K","M","G","T","P"};
    int i=0; double v=b;
    while(v>=1024.0 && i<5){ v/=1024.0; i++; }
    if(i<2) snprintf(o,n,"%.0f%s",v,u[i]);
    else    snprintf(o,n,"%.1f%s",v,u[i]);
}
static const char *H(double b){                 /* Ring buffer, for output convenience */
    static char ring[8][32]; static int k=0;
    char *p=ring[k++ & 7]; human(b,p,32); return p;
}
/* Fixed M unit, for places like a total that must show it is measured on the same
   scale as the rows below - promoting to 1.1G stops it being added to 4.9M by eye. */
static const char *HM(double b){
    static char ring[4][24]; static int k=0;
    char *p=ring[k++ & 3];
    snprintf(p,24,"%.1fM",b/(1024.0*1024.0));
    return p;
}
/* Round up with no decimal, for narrow places where the digit count must not move
   (204.7M -> 205M).  Rounding down would understate the value. */
static const char *HC(double b){
    static char ring[4][24]; static int k=0;
    static const char *u[]={"B","K","M","G","T","P"};
    char *p=ring[k++ & 3];
    int i=0; double v=b;
    while(v>=1024.0 && i<5){ v/=1024.0; i++; }
    double c=(double)(long long)v; if(v>c) c+=1.0;
    snprintf(p,24,"%.0f%s",c,u[i]);
    return p;
}
static int read_file(const char *path,char *buf,size_t cap){
    int fd=open(path,O_RDONLY); if(fd<0) return -1;
    size_t got=0; ssize_t n;
    while(got+1<cap && (n=read(fd,buf+got,cap-1-got))>0) got+=n;
    close(fd); buf[got]=0; return (int)got;
}
static int file_exists(const char *p){ struct stat st; return stat(p,&st)==0; }
/* Delta of a cumulative unsigned counter.  One that went backwards - a server that
   exited mid-window, a device whose counters were reset - counts as no change rather
   than wrapping to ~1.8e19. */
#define UDELTA(a_,b_) ((a_)>=(b_) ? (double)((a_)-(b_)) : 0.0)

/* Value-with-unit string ("512.0M", "32768") to bytes */
static long long parse_size(const char *s){
    char *end; double v=strtod(s,&end);
    if(end==s) return -1;
    switch(*end){
        case 'K': case 'k': v*=1024.0; break;
        case 'M': case 'm': v*=1024.0*1024.0; break;
        case 'G': case 'g': v*=1024.0*1024.0*1024.0; break;
        default: break;
    }
    return (long long)v;
}

/* ---------------------- Portability layer ---------------------- */
static struct {
    int vm_readv;        /* process_vm_readv available */
    int mem_pread;       /* /proc/pid/mem pread available */
    int rollup;          /* smaps_rollup present */
    int proc_io;         /* /proc/pid/io present */
    char note[256];
} CAP;
static int g_ascii=0;   /* --ascii: English canvas labels, for terminals whose input method follows Hangul rendering */   /* Korean/English label, toggled by l or --ascii */
/* Replay shows times as the recorder's clock read: its UTC offset is taken from the recorded
   "ts" string against ts_epoch (rp_apply), so -d, --json and the screens do not depend on
   the viewer's TZ.  Live, or a frame without "ts", uses localtime. */
static long g_rep_tzoff=0; static int g_rep_tzok=0;
static void rep_localtime(time_t t,struct tm *o){
    if(g_rep_tzok){ time_t u=t+(time_t)g_rep_tzoff; gmtime_r(&u,o); } else localtime_r(&t,o);
}

static int g_mem_fd = -1;   /* For the /proc/pid/mem fallback */
static int g_mem_pid = -1;  /* The pid g_mem_fd belongs to: a switch or restart must not read the old process */
/* Bumped when the watched process changes, a reused pid included: descriptors opened on
   /proc/<pid> belong to the old process and are reopened */
static unsigned g_pid_gen=0, g_mem_gen=0;

static ssize_t vmread(int pid,unsigned long addr,void *buf,size_t len){
    size_t got=0;
    if(CAP.vm_readv){
#ifdef __NR_process_vm_readv
        while(got<len){
            struct iovec l,r;
            l.iov_base=(char*)buf+got; l.iov_len=len-got;
            r.iov_base=(void*)(addr+got); r.iov_len=len-got;
            long n=syscall(__NR_process_vm_readv,pid,&l,1UL,&r,1UL,0UL);
            if(n<=0){ if(got==0 && errno==ENOSYS) CAP.vm_readv=0; break; }
            got+=n;
        }
        if(got) return got;
#endif
    }
    if(CAP.mem_pread){                        /* Fallback: /proc/pid/mem */
        if(g_mem_fd>=0 && (g_mem_pid!=pid || g_mem_gen!=g_pid_gen)){ close(g_mem_fd); g_mem_fd=-1; }
        if(g_mem_fd<0){
            char p[64]; snprintf(p,sizeof p,"/proc/%d/mem",pid);
            g_mem_fd=open(p,O_RDONLY|O_CLOEXEC);
            if(g_mem_fd<0){ CAP.mem_pread=0; return 0; }
            g_mem_pid=pid; g_mem_gen=g_pid_gen;
        }
        got=0;
        while(got<len){
            ssize_t n=pread(g_mem_fd,(char*)buf+got,len-got,(off_t)(addr+got));
            if(n<=0) break;
            got+=n;
        }
    }
    return got;
}
static int g_rd_ok=1;   /* Whether the last batch read succeeded, so a failed 0 is not passed off as measured */
static int32_t rd_i32(int pid,unsigned long a){
    int32_t v=0;
    if(vmread(pid,a,&v,4)==4) return v;
    g_rd_ok=0; return 0;
}

/* The one-line capability summary shown with the fallback warning */
static void cap_note(void){
    snprintf(CAP.note,sizeof CAP.note,
        "vm_readv=%s rollup=%s proc_io=%s",
        CAP.vm_readv?"yes":!CAP.mem_pread?(g_ascii?"no(denied)":"no(거부)"):(g_ascii?"no(pread fallback)":"no(pread 폴백)"),
        CAP.rollup?"yes":(g_ascii?"no(smaps sum)":"no(smaps 합산)"),
        CAP.proc_io?"yes":"no");
}
static int  g_perm_limited=0;
static char g_perm_why[192]="";
/* First readable mapping of pid, the probe address; 0 when maps is unreadable */
static unsigned long first_readable_map(int pid){
    char p[64], ln[512]; unsigned long a=0,s,e; char perm[8];
    snprintf(p,sizeof p,"/proc/%d/maps",pid);
    FILE *fp=fopen(p,"r"); if(!fp) return 0;
    while(fgets(ln,sizeof ln,fp))
        if(sscanf(ln,"%lx-%lx %7s",&s,&e,perm)==3 && perm[0]=='r' && !strstr(ln,"[vvar]") && !strstr(ln,"[vsyscall]")){ a=s; break; }
    fclose(fp);
    return a;
}
/* Memory reads denied though maps/smaps are readable: Yama ptrace_scope>=1 checks only the attach mode */
/* A kernel release string ("2.6.32-754.el6.x86_64") is older than a.b.c */
static int release_before(const char *rel,int a,int b,int c){
    int x=0,y=0,z=0;
    if(sscanf(rel,"%d.%d.%d",&x,&y,&z)<2) return 0;
    if(x!=a) return x<a;
    if(y!=b) return y<b;
    return z<c;
}
static int kernel_before(int a,int b,int c){
    struct utsname u; return uname(&u)==0 && release_before(u.release,a,b,c);
}
static void perm_vm_denied(int err){
    int sc=-1; FILE *f=fopen("/proc/sys/kernel/yama/ptrace_scope","r");
    if(f){ if(fscanf(f,"%d",&sc)!=1) sc=-1; fclose(f); }
    g_perm_limited=1;
    if(sc>=3)      /* no attach at all until reboot */
        snprintf(g_perm_why,sizeof g_perm_why,
                 g_ascii?"memory read denied (%s): kernel.yama.ptrace_scope=%d - engine view off (no process may attach; fixed until reboot)"
                        :"메모리 읽기 거부(%s): kernel.yama.ptrace_scope=%d - 엔진 직독 꺼짐 (어떤 프로세스도 붙을 수 없음, 재부팅 전 변경 불가)",
                 err==EACCES?"EACCES":"EPERM",sc);
    else if(sc>=1)   /* 1: a process's own children only, even under the same account; 2: admins only */
        snprintf(g_perm_why,sizeof g_perm_why,
                 g_ascii?"memory read denied (%s): kernel.yama.ptrace_scope=%d - engine view off even for the server's account (works where ptrace_scope=0)"
                        :"메모리 읽기 거부(%s): kernel.yama.ptrace_scope=%d - 서버를 띄운 계정이어도 엔진 직독 꺼짐 (ptrace_scope=0 인 호스트에서 동작)",
                 err==EACCES?"EACCES":"EPERM",sc);
    else if(kernel_before(2,6,39))   /* no process_vm_readv (3.2) and /proc/pid/mem for the tracer only */
        snprintf(g_perm_why,sizeof g_perm_why,
                 g_ascii?"memory read denied (%s): kernel before 2.6.39 lets only a tracer read /proc/<pid>/mem (and has no process_vm_readv) - engine view off on this kernel"
                        :"메모리 읽기 거부(%s): 2.6.39 이전 커널은 /proc/<pid>/mem 을 tracer 만 읽게 함(process_vm_readv 도 없음) - 이 커널에서는 엔진 직독 불가",
                 err==EACCES?"EACCES":"EPERM");
    else
        snprintf(g_perm_why,sizeof g_perm_why,
                 g_ascii?"memory read denied (%s) - engine view off (run as the account that started the server)"
                        :"메모리 읽기 거부(%s) - 엔진 직독 꺼짐 (서버를 띄운 계정으로 실행)",
                 err==EACCES?"EACCES":"EPERM");
}
static void cap_probe(int pid){
    char p[64], buf[64];
    memset(&CAP,0,sizeof CAP);
    g_perm_limited=0; g_perm_why[0]=0;   /* Per target; the smaps path sets its own reason below */
    /* Probe the target itself: a self read always passes, while Yama or another owner
       refuses only reads of the target */
    unsigned long ta=first_readable_map(pid);
    int denied=0, derr=0;
    /* maps unreadable: memory reads need the stricter attach mode, so they fail too (smaps path reports it) */
    if(ta){
        CAP.mem_pread=1;
#ifdef __NR_process_vm_readv
        struct iovec l,r; l.iov_base=&buf[0]; l.iov_len=1; r.iov_base=(void*)ta; r.iov_len=1;
        long n=syscall(__NR_process_vm_readv,pid,&l,1UL,&r,1UL,0UL);
        if(n==1) CAP.vm_readv=1;
        else if(errno==EPERM||errno==EACCES){ denied=1; derr=errno; }
        else CAP.vm_readv=(errno!=ENOSYS);           /* other errors are address-specific */
#endif
        if(!CAP.vm_readv){
            /* /proc/pid/mem is checked on read too (2.6.32 allows only the tracer), so a byte is read */
            snprintf(p,sizeof p,"/proc/%d/mem",pid);
            int fd=open(p,O_RDONLY);
            if(fd>=0 && pread(fd,buf,1,(off_t)ta)==1) denied=0;
            else if(errno==EPERM||errno==EACCES){ CAP.mem_pread=0; if(!denied) derr=errno; denied=1; }
            if(fd>=0) close(fd);
        }
        if(denied){ CAP.vm_readv=0; CAP.mem_pread=0; perm_vm_denied(derr); }
    }
    snprintf(p,sizeof p,"/proc/%d/smaps_rollup",pid); CAP.rollup=file_exists(p);
    snprintf(p,sizeof p,"/proc/%d/io",pid);           CAP.proc_io=file_exists(p);
    /* Old-kernel simulation switch, to exercise the fallback paths without a real
       CentOS 6 (2.6.32) machine.
       CUB_TOP_COMPAT="no-vm_readv,no-rollup,no-proc_io" */
    { const char *cm=getenv("CUB_TOP_COMPAT");
      if(cm){
          if(strstr(cm,"no-vm_readv"))   CAP.vm_readv=0;
          if(strstr(cm,"no-rollup"))     CAP.rollup=0;
          if(strstr(cm,"no-proc_io"))    CAP.proc_io=0;
      } }
    cap_note();
}

/* Permission scope: for a target owned by another user the ptrace check blocks
 * smaps/maps/io (regions, PSS, I/O), while status and stat
 * stay readable (totals, CPU).  "Not permitted" is reported distinctly from
 * "measured zero" - a 0 would read as "uses no memory".  g_perm_limited/g_perm_why
 * are defined with cap_probe, which also sets them when only memory reads are denied. */

/* ---------------------- /proc parsers ---------------------- */
typedef struct {
    unsigned long start,end;
    char perm[8];
    char path[192];
    unsigned long rss_kb,pss_kb;
    char is_stack;    /* Marked in the first classify_regions loop when the pthread stack pattern matches */
} region_t;

typedef struct {
    int pid;
    char db[64];
    region_t reg[MAXREG];
    int nreg;
    int reg_dropped;   /* Regions past MAXREG: not classified, so the breakdown falls short of the total */
    unsigned long rss_kb,pss_kb,swap_kb,vsize_kb;
    /* Whole-process totals summed from this frame's smaps, every region included (even past
       MAXREG): the same figures smaps_rollup gives, without reading it again */
    int smaps_ok; unsigned long s_rss_kb,s_pss_kb,s_swap_kb,s_thp_kb,thp_kb;   /* thp: AnonHugePages, the share of the heap on huge pages */
    /* smaps is read on a period set by its own cost (see proc_read_mem); between reads the
       regions are kept and the totals come from status */
    double smaps_at_ms, smaps_ms, smaps_age_s; int smaps_pid; unsigned long maps_sig;
    unsigned long minflt,majflt,blkio_ticks;
    unsigned long utime,stime;     /* Cumulative CPU in clock ticks; utilization comes from the delta */
    int nthreads;
    /* /proc/pid/io */
    unsigned long rchar,wchar_,syscr,syscw,read_b,write_b;
} proc_t;


/* Instance (database) enumeration.  /proc order is arbitrary, so instances are
   sorted by db name to keep the target stable across runs.  cub_pl is attributed
   by the db name in its cmdline; cub_master belongs to no database.  The list grows
   with the servers found: none is left out. */
typedef struct {
    int pid; char db[64];
    int uid;              /* The account the server runs as: the same db name may run under several */
    unsigned long long start;   /* Start time (ticks since boot): with the pid, the process's identity */
    /* top/ps-level figures, for every instance (the analysed one in detail elsewhere) */
    double rss;           /* Resident (VmRSS) */
    double filemap;       /* File-backed resident (RssFile): the share resident in the page cache */
    double swap;          /* Swapped out (VmSwap) */
    double pl_rss; int pl_n;
} inst_t;
static inst_t *g_inst=NULL; static int g_ninst=0, g_inst_cap=0, g_cur=0;
static int g_inst_lost=0;          /* Servers left out only because the list could not grow (out of memory) */
static inst_t *inst_add(void){
    if(g_ninst==g_inst_cap){
        int nc=g_inst_cap?g_inst_cap*2:8;
        inst_t *t=realloc(g_inst,sizeof *t*(size_t)nc);
        if(!t){ g_inst_lost++; return NULL; }
        g_inst=t; g_inst_cap=nc;
    }
    inst_t *x=&g_inst[g_ninst++]; memset(x,0,sizeof *x); return x;
}
static char g_cur_db[64]="";       /* The db scan_tiers attributes PL to */

/* top/ps-level memory of a process: VmRSS, RssFile and VmSwap from /proc/PID/status, in
   bytes.  No page-table walk, so the cost does not grow with the process's size.
   RssFile exists from kernel 4.5; before that the file-backed resident share is the
   "shared" field of /proc/PID/statm (file pages, plus shmem on newer kernels - none in
   cub_server), present on every kernel, so the column is measured rather than a false 0. */
static void status_mem(int pid,double *rss,double *file,double *swap){
    char p[64],buf[4096]; snprintf(p,sizeof p,"/proc/%d/status",pid);
    double r=0,f=0,w=0; int have_file=0;
    if(read_file(p,buf,sizeof buf)>0){
        char*q; unsigned long v;
        if((q=strstr(buf,"VmRSS:"))&&sscanf(q+6,"%lu",&v)==1) r=v*1024.0;
        if((q=strstr(buf,"RssFile:"))&&sscanf(q+8,"%lu",&v)==1){ f=v*1024.0; have_file=1; }
        if((q=strstr(buf,"VmSwap:"))&&sscanf(q+7,"%lu",&v)==1) w=v*1024.0;
    }
    if(!have_file){
        unsigned long sz,res,sh; snprintf(p,sizeof p,"/proc/%d/statm",pid);
        if(read_file(p,buf,sizeof buf)>0 && sscanf(buf,"%lu %lu %lu",&sz,&res,&sh)==3)
            f=sh*(double)sysconf(_SC_PAGESIZE);
    }
    if(rss) *rss=r;
    if(file) *file=f;
    if(swap) *swap=w;
}
/* Take the second cmdline token (the db name) */
static void cmd_db(int pid,char*out,size_t n){
    char p[64],b[512]; snprintf(p,sizeof p,"/proc/%d/cmdline",pid);
    int r=read_file(p,b,sizeof b); out[0]=0;
    if(r>0){ int i=0; while(i<r&&b[i]) i++; if(++i<r) snprintf(out,n,"%s",b+i); }
}
typedef struct { double master,broker,cas,pl,server_rss; int npl,ncas;
    /* CPU per tier in cores, so "who is using CPU" reads on the same row as memory */
    double cpu_master,cpu_broker,cpu_cas,cpu_pl;
} tier_t;
static double tcpu_delta(int pid,double dt,double *rss);
static double stat_rss(int pid);
static void tcpu_sweep(void);
static tier_t *g_enum_T=NULL; static double g_enum_dt=0;   /* For the pass 2 tier aggregation */
/* One /proc walk, each process handled as it is met; nothing is kept but the instance list.
   Processes other than the analysed server are shown at the level of top and ps - RSS
   and CPU, read every frame - since they are context, not drilled into.
   A cub_pl joins its database's record: whichever of the server and the PL the walk meets
   first creates it (a PL first leaves pid -1 for the server to fill), and a record that
   ends with no server is dropped. */
/* The process name, from /proc/<pid>/stat's "(comm)" - /proc/<pid>/comm only exists from
   2.6.33.  The name may hold ')' or spaces, so it ends at the last ')'. */
static int proc_name(const char *pid,char *out,size_t on){
    char p[64], b[512]; snprintf(p,sizeof p,"/proc/%.20s/stat",pid);
    if(read_file(p,b,sizeof b)<=0) return 0;
    char *l=strchr(b,'('), *r=strrchr(b,')');
    if(!l || !r || r<l) return 0;
    if(r[1]==' ' && (r[2]=='Z' || r[2]=='X')) return 0;   /* a zombie or dead process is not running */
    size_t n=(size_t)(r-l-1); if(n>=on) n=on-1;
    memcpy(out,l+1,n); out[n]=0; return 1;
}
static int proc_uid(int pid){
    char p[32]; struct stat st; snprintf(p,sizeof p,"/proc/%d",pid);
    return stat(p,&st)==0 ? (int)st.st_uid : -1;
}
/* A process's start time, field 22 of /proc/<pid>/stat (ticks since boot); 0 if unreadable.
   A pid alone may be reused by another process while the watched one is gone. */
static unsigned long long proc_start(int pid){
    char p[64],b[1024]; snprintf(p,sizeof p,"/proc/%d/stat",pid);
    if(read_file(p,b,sizeof b)<=0) return 0;
    char *q=strrchr(b,')'); if(!q) return 0;
    q+=2;                                         /* field 3 */
    for(int f=3;f<22 && q;f++){ q=strchr(q,' '); if(q) q++; }
    return q ? strtoull(q,NULL,10) : 0;
}
/* One process of the /proc walk: a server, PL, master, broker or CAS is attributed; anything
   else is ignored.  The pass over all of /proc (every pid's comm) runs every INST_WALK_MS;
   between walks only the CUBRID pids found last time are read again (their comm, so one that
   ended is noticed), and a pid that ended brings the full walk forward to this frame.  A
   process started between walks (a new CAS, a restarted server) appears within one period. */
#define INST_WALK_MS 2000
static int *g_cand=NULL; static int g_ncand=0, g_cand_cap=0; static double g_walk_at=-1;
static int g_walk_force=0;      /* the watched server is down: every frame walks, so its restart is seen at once */
static void cand_add(int pid){
    if(g_ncand==g_cand_cap){ int nc=g_cand_cap?g_cand_cap*2:64; int *t=realloc(g_cand,sizeof(int)*(size_t)nc); if(!t) return; g_cand=t; g_cand_cap=nc; }
    g_cand[g_ncand++]=pid;
}
static int is_cubrid_proc(const char *b){
    return !strcmp(b,"cub_server") || !strncmp(b,"cub_pl",6) || !strcmp(b,"cub_master") || !strcmp(b,"cub_broker") || !strcmp(b,"cub_cas");
}
static void inst_consider(int pid,const char *b);
static int enum_instances(void){
    g_ninst=0; g_inst_lost=0;
    char b[256];
    int full = g_walk_at<0 || g_walk_force || now_ms()-g_walk_at>=INST_WALK_MS;
    g_walk_force=0;
    if(!full){
        /* The cached pids: their comm again (a reused pid with another name counts as gone) */
        static char (*nm)[32]=NULL; static int nmcap=0;
        if(g_ncand>nmcap){ free(nm); nm=malloc(32*(size_t)g_ncand); nmcap=nm?g_ncand:0; }
        for(int i=0;i<g_ncand && nm;i++){
            char ps[16]; snprintf(ps,sizeof ps,"%d",g_cand[i]);
            if(!proc_name(ps,b,sizeof b) || !is_cubrid_proc(b)){ full=1; break; }
            snprintf(nm[i],32,"%.31s",b);
        }
        if(!nm) full=1;
        if(!full) for(int i=0;i<g_ncand;i++) inst_consider(g_cand[i],nm[i]);
    }
    if(full){
        g_ncand=0;
        DIR *d=opendir("/proc"); if(!d) return 0;
        struct dirent *e;
        while((e=readdir(d))){
            if(e->d_name[0]<'0'||e->d_name[0]>'9') continue;
            if(!proc_name(e->d_name,b,sizeof b)) continue;
            if(!is_cubrid_proc(b)) continue;
            int pid=atoi(e->d_name);
            cand_add(pid); inst_consider(pid,b);
        }
        closedir(d);
        g_walk_at=now_ms();
    }
    if(g_enum_T) tcpu_sweep();
    { int m=0; for(int i=0;i<g_ninst;i++) if(g_inst[i].pid>=0) g_inst[m++]=g_inst[i]; g_ninst=m; }
    for(int i=0;i<g_ninst;i++) for(int j=i+1;j<g_ninst;j++)    /* Alphabetical by db */
        if(strcmp(g_inst[i].db,g_inst[j].db)>0){
            inst_t t=g_inst[i]; g_inst[i]=g_inst[j]; g_inst[j]=t; }
    return g_ninst;
}
static void inst_consider(int pid,const char *b){
    {
        if(!strcmp(b,"cub_server")){
            char db[64]; cmd_db(pid,db,sizeof db);
            if(!db[0]) snprintf(db,sizeof db,"-");
            inst_t *x=NULL;
            int uid=proc_uid(pid);
            for(int i=0;i<g_ninst;i++) if(g_inst[i].pid<0 && !strcmp(g_inst[i].db,db) && g_inst[i].uid==uid){ x=&g_inst[i]; break; }
            if(!x){ x=inst_add(); if(!x) return; snprintf(x->db,sizeof x->db,"%s",db); }
            x->pid=pid; x->uid=uid; x->start=proc_start(pid);
            status_mem(pid,&x->rss,&x->filemap,&x->swap);
            return;
        }
        if(!strncmp(b,"cub_pl",6)){
            char db[64]; cmd_db(pid,db,sizeof db);
            int cur = g_enum_T && (!g_cur_db[0] || !strcmp(db,g_cur_db));
            double pl=0, cpu = cur ? tcpu_delta(pid,g_enum_dt,&pl) : 0;
            if(!cur) pl=stat_rss(pid);
            inst_t *x=NULL; int uid=proc_uid(pid);   /* the PL of this account's server of that name */
            for(int i=0;i<g_ninst;i++) if(!strcmp(g_inst[i].db,db) && g_inst[i].uid==uid){ x=&g_inst[i]; break; }
            if(!x && (x=inst_add())!=NULL){ x->pid=-1; x->uid=uid; snprintf(x->db,sizeof x->db,"%s",db); }
            if(x){ x->pl_rss+=pl; x->pl_n++; }
            /* Only the current DB's PL is counted in the tiers */
            if(cur){ g_enum_T->pl+=pl; g_enum_T->npl++; g_enum_T->cpu_pl+=cpu; }
            return;
        }
        if(!g_enum_T) return;
        int is_master=!strcmp(b,"cub_master"), is_broker=!strcmp(b,"cub_broker");
        int is_cas=!strcmp(b,"cub_cas");
        if(!is_master&&!is_broker&&!is_cas) return;
        /* One /proc/PID/stat read gives both CPU ticks and RSS, as top reads it */
        double rss=0, cpu=tcpu_delta(pid,g_enum_dt,&rss);
        if(is_master){ g_enum_T->master+=rss; g_enum_T->cpu_master+=cpu; }
        else if(is_broker){ g_enum_T->broker+=rss; g_enum_T->cpu_broker+=cpu; }
        else { g_enum_T->cas+=rss; g_enum_T->ncas++; g_enum_T->cpu_cas+=cpu; }
    }
}
/* The server to follow after the watched one went away: the same db name under the same
   account (uid -1: any, when nothing was being watched).  -1 if none. */
static int restart_pick(const char *db,int uid){
    for(int i=0;i<g_ninst;i++)
        if(!strcmp(g_inst[i].db,db) && (uid<0 || g_inst[i].uid==uid)) return g_inst[i].pid;
    return -1;
}
/* The account of the server being watched.  Set where the target is chosen (start, < >,
   the one-shot walk, a followed restart), so a restart is looked for under the account
   chosen even when that server stops before a frame has seen it; -1 = any. */
static int g_track_uid=-1;
static unsigned long long g_track_start=0;   /* The watched server's start time; 0 = not known yet */
static void target_select(proc_t *P,int i){
    g_cur=i; P->pid=g_inst[i].pid; g_track_uid=g_inst[i].uid; g_track_start=g_inst[i].start;
    snprintf(P->db,sizeof P->db,"%s",g_inst[i].db);
    snprintf(g_cur_db,sizeof g_cur_db,"%s",g_inst[i].db);
}
/* A db name held by several accounts: this account's server first */
static int inst_find(const char*db){
    int first=-1;
    for(int i=0;i<g_ninst;i++) if(!strcmp(g_inst[i].db,db)){
        if(g_inst[i].uid==(int)geteuid()) return i;
        if(first<0) first=i; }
    return first;
}

/* Full smaps parse: regions plus Rss/Pss */
static void parse_smaps(proc_t *P){
    char f[64]; snprintf(f,sizeof f,"/proc/%d/smaps",P->pid);
    P->smaps_ok=0; P->s_rss_kb=P->s_pss_kb=P->s_swap_kb=P->s_thp_kb=0;
    P->nreg=0; P->reg_dropped=0;       /* before the open: a refused smaps must not leave the last target's regions */
    FILE *fp=fopen(f,"re"); if(!fp) return;
    char line[1024]; region_t *cur=NULL;
    while(fgets(line,sizeof line,fp)){
        /* First-character gate: a header starts with lower-case hex, an attribute line with
           an upper-case letter.  96% are attribute lines, so trying sscanf on each. */
        char c0=line[0];
        if((c0>='0'&&c0<='9')||(c0>='a'&&c0<='f')){
            unsigned long s,e; char perm[8];
            if(sscanf(line,"%lx-%lx %7s",&s,&e,perm)!=3) continue;
            if(P->nreg>=MAXREG){ P->reg_dropped++; cur=NULL; continue; }   /* counted, not hidden */
            cur=&P->reg[P->nreg++];
            memset(cur,0,sizeof *cur);
            cur->start=s; cur->end=e;
            snprintf(cur->perm,sizeof cur->perm,"%s",perm);
            /* The path starts at field 6 and may contain spaces; copied directly, no intermediate buffer */
            {   char *q=line; int fld=0; size_t n=0;
                while(*q && fld<5){ while(*q==' ')q++; while(*q && *q!=' ')q++; fld++; }
                while(*q==' ')q++;
                while(*q && *q!='\n' && n+1<sizeof cur->path) cur->path[n++]=*q++;
                cur->path[n]=0;
            }
        } else {
            /* Totals over every region, a dropped one included */
            unsigned long v;
            if(!strncmp(line,"Rss:",4)){ v=strtoul(line+4,NULL,10); P->s_rss_kb+=v; if(cur) cur->rss_kb=v; }
            else if(!strncmp(line,"Pss:",4)){ v=strtoul(line+4,NULL,10); P->s_pss_kb+=v; if(cur) cur->pss_kb=v; }
            else if(!strncmp(line,"Swap:",5))           P->s_swap_kb +=strtoul(line+5,NULL,10);
            else if(!strncmp(line,"AnonHugePages:",14)) P->s_thp_kb  +=strtoul(line+14,NULL,10);
        }
    }
    fclose(fp);
    P->smaps_ok = (P->nreg>0);
}

/* Use rollup where present, otherwise sum smaps (old-kernel fallback) */
static void parse_rollup(proc_t *P){
    P->rss_kb=P->pss_kb=P->swap_kb=0;
    /* This frame's smaps already gives the totals: smaps_rollup is not read again */
    if(P->smaps_ok && P->s_rss_kb){
        P->rss_kb=P->s_rss_kb; P->pss_kb=P->s_pss_kb; P->swap_kb=P->s_swap_kb; P->thp_kb=P->s_thp_kb;
        return;
    }
    if(CAP.rollup){
        char f[64]; snprintf(f,sizeof f,"/proc/%d/smaps_rollup",P->pid);
        FILE *fp=fopen(f,"r");
        if(fp){
            char line[256]; unsigned long v;
            while(fgets(line,sizeof line,fp)){
                if(!strncmp(line,"Rss:",4)&&sscanf(line+4,"%lu",&v)==1) P->rss_kb=v;
                else if(!strncmp(line,"Pss:",4)&&sscanf(line+4,"%lu",&v)==1) P->pss_kb=v;
                else if(!strncmp(line,"Swap:",5)&&sscanf(line+5,"%lu",&v)==1) P->swap_kb=v;
                else if(!strncmp(line,"AnonHugePages:",14)&&sscanf(line+14,"%lu",&v)==1) P->thp_kb=v;
            }
            fclose(fp);
            if(P->rss_kb) return;
        }
    }
    if(P->nreg==0){
        /* No region read at all means maps/smaps is inaccessible (owned by another user).
           VmRSS from status salvages the total; PSS is impossible without smaps. */
        char f2[64]; snprintf(f2,sizeof f2,"/proc/%d/status",P->pid);
        FILE*sf=fopen(f2,"r");
        if(sf){ char ln[256]; unsigned long v;
            while(fgets(ln,sizeof ln,sf)){
                if(!strncmp(ln,"VmRSS:",6)&&sscanf(ln+6,"%lu",&v)==1) P->rss_kb=v;
                else if(!strncmp(ln,"VmSwap:",7)&&sscanf(ln+7,"%lu",&v)==1) P->swap_kb=v;
            }
            fclose(sf); }
        if(!g_perm_limited){
            g_perm_limited=1;
            snprintf(g_perm_why,sizeof g_perm_why,
                     g_ascii?"target owned by another user: maps/smaps/io blocked - totals from status only (run as the account that started the server for the full view)"
                            :"대상이 다른 사용자 소유: maps/smaps/io 차단 - status 기반 총량만 (전체는 서버를 띄운 계정으로 실행)");
        }
        return;
    }
    for(int i=0;i<P->nreg;i++){                 /* Fallback sum */
        P->rss_kb+=P->reg[i].rss_kb;
        P->pss_kb+=P->reg[i].pss_kb;
    }
}

/* smaps (and smaps_rollup) make the kernel walk the target's page tables under its mmap
   read lock: about 6-9 ms per GB resident (measured), so a 100 GB server would be held for
   most of a second every frame.  They are read on a period that keeps them within
   SMAPS_BUDGET of wall time (at least the interval, at most SMAPS_MAX_MS), and earlier
   when the mapping set changes (a hash of maps, which reads no page tables; adjacent
   mappings merge, so a count of lines would miss a new one) - within SMAPS_BUDGET_CHG of
   wall time, since a server that maps and unmaps often must not bring the read back to
   every frame - or when the target changes.
   Between reads the regions are kept and the totals come from status (O(1)); PSS is the
   last read's.  The age of the regions is shown and recorded. */
#define SMAPS_BUDGET 0.01
#define SMAPS_BUDGET_CHG 0.10
#define SMAPS_MAX_MS 60000.0
static unsigned long maps_sig(int pid){
    char f[64]; snprintf(f,sizeof f,"/proc/%d/maps",pid);
    FILE *fp=fopen(f,"re"); if(!fp) return 0;
    unsigned long h=1469598103934665603UL; int c;
    while((c=getc(fp))!=EOF){ h^=(unsigned long)c; h*=1099511628211UL; }
    fclose(fp); return h?h:1;
}
static void status_totals(proc_t *P){
    char f[64],buf[4096]; snprintf(f,sizeof f,"/proc/%d/status",P->pid);
    if(read_file(f,buf,sizeof buf)<=0) return;
    char *q; unsigned long v;
    if((q=strstr(buf,"VmRSS:"))&&sscanf(q+6,"%lu",&v)==1) P->rss_kb=v;
    if((q=strstr(buf,"VmSwap:"))&&sscanf(q+7,"%lu",&v)==1) P->swap_kb=v;
}
static void parse_smaps(proc_t *P);
static void parse_rollup(proc_t *P);
static void proc_read_mem(proc_t *P,double interval_s,int force){
    double now=now_ms();
    int due = force || P->smaps_pid!=P->pid || P->nreg==0;
    if(!due){
        double period=P->smaps_ms/SMAPS_BUDGET;
        if(period<interval_s*1000.0) period=interval_s*1000.0;
        if(period>SMAPS_MAX_MS) period=SMAPS_MAX_MS;
        due = now-P->smaps_at_ms>=period;
    }
    if(!due && now-P->smaps_at_ms>=P->smaps_ms/SMAPS_BUDGET_CHG && now-P->smaps_at_ms>=interval_s*1000.0){
        unsigned long sg=maps_sig(P->pid); if(sg!=P->maps_sig) due=1; }
    if(due){
        parse_smaps(P); P->smaps_ms=now_ms()-now; P->smaps_at_ms=now_ms(); P->smaps_pid=P->pid;
        P->maps_sig=maps_sig(P->pid);
        parse_rollup(P);
    } else status_totals(P);
    P->smaps_age_s=(now_ms()-P->smaps_at_ms)/1000.0;
}
static void parse_status_stat(proc_t *P){
    char f[64],buf[8192];
    snprintf(f,sizeof f,"/proc/%d/status",P->pid);
    if(read_file(f,buf,sizeof buf)>0){
        char *p=strstr(buf,"VmSize:"); unsigned long v;
        if(p && sscanf(p+7,"%lu",&v)==1) P->vsize_kb=v;
    }
    snprintf(f,sizeof f,"/proc/%d/stat",P->pid);
    if(read_file(f,buf,sizeof buf)>0){
        char *q=strrchr(buf,')');            /* Safe even when comm contains spaces or parentheses */
        if(q && *(q+1)){
            q+=2;
            unsigned long fld[64]; int n=0;
            char *tok=strtok(q," ");
            while(tok && n<64){ fld[n++]=strtoul(tok,NULL,10); tok=strtok(NULL," "); }
            /* q starts at field 3, so index i is field(i+3) */
            if(n>10) P->minflt=fld[7];        /* field10 */
            if(n>12) P->majflt=fld[9];        /* field12 */
            if(n>11) P->utime=fld[11];        /* field 14: user CPU in ticks */
            if(n>12) P->stime=fld[12];        /* field 15: kernel CPU in ticks */
            if(n>39) P->blkio_ticks=fld[39];  /* field42 */
        }
    }
    snprintf(f,sizeof f,"/proc/%d/task",P->pid);
    DIR *d=opendir(f); P->nthreads=0;
    if(d){ struct dirent *e; while((e=readdir(d))) if(e->d_name[0]>='0'&&e->d_name[0]<='9') P->nthreads++;
           closedir(d); }
}

static void parse_procio(proc_t *P){
    if(!CAP.proc_io) return;
    char f[64],buf[1024];
    snprintf(f,sizeof f,"/proc/%d/io",P->pid);
    if(read_file(f,buf,sizeof buf)<=0) return;
    char *p;
    if((p=strstr(buf,"rchar:")))       P->rchar =strtoul(p+6,NULL,10);
    if((p=strstr(buf,"wchar:")))       P->wchar_=strtoul(p+6,NULL,10);
    if((p=strstr(buf,"syscr:")))       P->syscr =strtoul(p+6,NULL,10);
    if((p=strstr(buf,"syscw:")))       P->syscw =strtoul(p+6,NULL,10);
    if((p=strstr(buf,"read_bytes:")))  P->read_b =strtoul(p+11,NULL,10);
    if((p=strstr(buf,"write_bytes:"))) P->write_b=strtoul(p+12,NULL,10);
}

/* Count cub_server threads in D state (uninterruptible sleep = storage wait).
   Field 3 of task/<tid>/stat is the state character; comm can contain spaces or
   parentheses, so parse after the last ')'.  53 threads cost 1.1ms. */
static unsigned long *g_blk_baseline_slot=NULL;   /* The frame's closing scan becomes the next baseline */
static void count_d_threads(int pid,int *d_out,int *n_out,unsigned long *blk_out){
    *d_out=0; *n_out=0; *blk_out=0;
    if(pid<0) return;
    char dp[64]; snprintf(dp,sizeof dp,"/proc/%d/task",pid);
    DIR *d=opendir(dp); if(!d) return;
    struct dirent *e; char f[400],b[512];
    while((e=readdir(d))){
        if(e->d_name[0]<'0'||e->d_name[0]>'9') continue;
        snprintf(f,sizeof f,"%s/%s/stat",dp,e->d_name);
        if(read_file(f,b,sizeof b)<=0) continue;
        char *rp=strrchr(b,')'); if(!rp||!rp[1]) continue;
        char *s=rp+1; while(*s==' ') s++;
        if(!*s) continue;
        (*n_out)++;
        if(*s=='D') (*d_out)++;
        /* Sum delayacct_blkio_ticks (field 42) over threads: the process-level value
           covers the main thread only, which is always 0 because the workers do
           the I/O.  s points at field 3, so skip 39 more. */
        { char *p=s; int k=0;
          while(*p && k<39){ while(*p && *p!=' ') p++; while(*p==' ') p++; k++; }
          if(*p) *blk_out += strtoul(p,NULL,10); }
    }
    closedir(d);
}

/* meminfo */
#define MAXCORE 256          /* Beyond this, only the first MAXCORE go into the distribution */
typedef struct { unsigned long total,avail,cached,swtotal,swfree;
    /* Host CPU from a /proc/stat delta; cores is the online core count.
       busy_cores is how many cores' worth is in use (0..cores), the same unit as the
       process-side figure, so the two can be compared directly. */
    double cpu_us,cpu_sy,cpu_wa,busy_cores; int cores,pcores;
    /* Per-core usage, sorted descending.  A total alone cannot tell "12 cores' worth
       spread over 12" from "6 saturated", and the latter is the bottleneck.
       The same information as btop's per-core meters, compressed into one row. */
    double core_max; int ncore_pct, sat_cores, hi_cores;   /* busiest core %, cores measured, saturated (>=95%), high (50-95%) */
    double load1,load5,load15;
    /* The target's memory cgroup: the OOM boundary in a container or a systemd slice is
       its limit, not the host's RAM.  limit -1 = none; path as /proc/<pid>/cgroup gives it */
    double cg_limit, cg_usage; char cg_path[96];
    /* Memory pressure stall information (kernel 4.20+ or psi=1): -1 = not provided */
    double psi_some10, psi_full10;
} mem_t;
/* Per-field deltas of one /proc/stat cpu line, the way procps top takes them: a
   field that steps backwards (iowait does, proc(5)) contributes 0, and the total
   is the sum of the deltas, so the shares still add up. */
static double cpu_deltas(const unsigned long long *v,const unsigned long long *pv,int n,double *d){
    double t=0;
    for(int i=0;i<n;i++){ d[i]= v[i]>=pv[i] ? (double)(v[i]-pv[i]) : 0.0; t+=d[i]; }
    return t;
}
/* First /proc/stat line: user nice system idle iowait irq softirq steal ... */
static void parse_hostcpu(mem_t *M){
    static unsigned long long pv[10]; static int pv_ok=0;
    static char cbuf[MAXCORE*128];      /* All per-core lines, about 6KB at 64 cores */
    char buf[512];
    M->cores = (int)sysconf(_SC_NPROCESSORS_ONLN); if(M->cores<1) M->cores=1;
    /* Physical cores = sockets x cores per socket; differing from logical means SMT.
       cpuinfo has one block per core and is large, but socket ids are few, so they
       are counted in a bitmask. */
    if(M->pcores<=0){                       /* Does not change while booted; read once */
        static int cached=0;
        if(!cached){
            FILE *f=fopen("/proc/cpuinfo","r");
            if(f){ char ln[256]; unsigned long long sock=0; int percore=0;
                   while(fgets(ln,sizeof ln,f)){
                       int id;
                       if(sscanf(ln,"physical id : %d",&id)==1 && id>=0 && id<64) sock|=1ULL<<id;
                       else if(percore<=0) sscanf(ln,"cpu cores : %d",&percore);
                   }
                   fclose(f);
                   int ns=0; for(int i=0;i<64;i++) if(sock&(1ULL<<i)) ns++;
                   if(ns>0&&percore>0) cached=ns*percore;
            }
            if(cached<=0) cached=M->cores;   /* Unknown: same as logical */
        }
        M->pcores=cached;
    }
    if(read_file("/proc/stat",buf,sizeof buf)>0){
        unsigned long long v[10]={0};
        char *p=strchr(buf,' ');
        if(p){ int n=0; char *tok=strtok(p," \n");
               while(tok&&n<10){ v[n++]=strtoull(tok,NULL,10); tok=strtok(NULL," \n"); }
               double d[10];
               double dt = pv_ok ? cpu_deltas(v,pv,10,d) : 0;
               if(dt>0){
                   M->cpu_us=(d[0]+d[1])/dt*100.0;
                   M->cpu_sy=(d[2]+d[5]+d[6])/dt*100.0;
                   M->cpu_wa=d[4]/dt*100.0;
                   M->busy_cores=(1.0-(d[3]+d[4])/dt)*M->cores;
                   if(M->busy_cores<0) M->busy_cores=0;
               }
               memcpy(pv,v,sizeof pv); pv_ok=1; }
    }
    /* Per-core usage from the cpuN lines of /proc/stat.  The buffer read above holds
       only the first line, so read again with room here (about 6KB for 64 cores).
       Measured 0.27ms, 2% of the frame's work. */
    { static unsigned long long pc[MAXCORE][10];   /* the previous cpuN fields */
      static int primed=0;
      char *big=(char*)cbuf; size_t bn=sizeof cbuf;
      M->ncore_pct=0; M->sat_cores=0; M->hi_cores=0; M->core_max=0;
      if(read_file("/proc/stat",big,bn)>0){
          char *p=big; int idx=0;
          while((p=strstr(p,"cpu"))){
              p+=3;
              if(*p<'0'||*p>'9'){ continue; }          /* Skip the aggregate "cpu " line */
              int cid=(int)strtol(p,&p,10);
              if(cid<0||cid>=MAXCORE) continue;
              unsigned long long v[10]={0}; int n=0;
              while(n<10){
                  char *e; unsigned long long x=strtoull(p,&e,10);
                  if(e==p) break;
                  v[n++]=x; p=e;
              }
              if(n<5) continue;
              double d[10]={0};
              double dt = primed ? cpu_deltas(v,pc[cid],n,d) : 0;
              if(dt>0){
                  double b=(1.0-(d[3]+d[4])/dt)*100.0;
                  if(b<0) b=0;
                  if(b>100) b=100;
                  idx++; if(b>M->core_max) M->core_max=b;
                  if(b>=95.0) M->sat_cores++;
                  else if(b>=50.0) M->hi_cores++;
              }
              memcpy(pc[cid],v,sizeof pc[cid]);
          }
          M->ncore_pct=idx; primed=1;
      } }
    if(read_file("/proc/loadavg",buf,sizeof buf)>0)
        sscanf(buf,"%lf %lf %lf",&M->load1,&M->load5,&M->load15);
}
static void parse_meminfo(mem_t *M){
    char buf[8192]; memset(M,0,sizeof *M);
    if(read_file("/proc/meminfo",buf,sizeof buf)<=0) return;
    char *p;
    if((p=strstr(buf,"MemTotal:")))     M->total  =strtoul(p+9,NULL,10);
    if((p=strstr(buf,"MemAvailable:"))) M->avail  =strtoul(p+13,NULL,10);
    else if((p=strstr(buf,"MemFree:"))) M->avail  =strtoul(p+8,NULL,10); /* Old-kernel fallback */
    if((p=strstr(buf,"Cached:")))       M->cached =strtoul(p+7,NULL,10);
    if((p=strstr(buf,"SwapTotal:")))    M->swtotal=strtoul(p+10,NULL,10);
    if((p=strstr(buf,"SwapFree:")))     M->swfree =strtoul(p+9,NULL,10);
}

/* The memory cgroup of pid: v2 ("0::<path>", memory.max/memory.current) or v1
   ("N:memory:<path>", memory.limit_in_bytes/memory.usage_in_bytes).  In a container the
   path seen in /proc/<pid>/cgroup may not exist under the mount (the container's own group
   is the mount root), so the root is tried after the path.  "max" and the v1 no-limit
   value read as none. */
static double cg_num(const char *dir,const char *file){
    char p[320],b[64]; snprintf(p,sizeof p,"%s/%s",dir,file);
    if(read_file(p,b,sizeof b)<=0) return -2;             /* not there */
    if(!strncmp(b,"max",3)) return -1;
    double v=strtod(b,NULL); return v>=(double)(1ULL<<60) ? -1 : v;
}
static void parse_cgroup(mem_t *M,int pid){
    M->cg_limit=-1; M->cg_usage=-1; M->cg_path[0]=0;
    if(pid<0) return;
    char f[64],buf[4096]; snprintf(f,sizeof f,"/proc/%d/cgroup",pid);
    if(read_file(f,buf,sizeof buf)<=0) return;
    char path[256]=""; int v2=0;
    for(char *ln=buf;ln && *ln;){
        char *nl=strchr(ln,'\n'); if(nl) *nl=0;
        if(!strncmp(ln,"0::",3)){ if(!path[0]){ snprintf(path,sizeof path,"%.255s",ln+3); v2=1; } }
        else { char *c=strchr(ln,':'); if(c && !strncmp(c,":memory:",8)){ snprintf(path,sizeof path,"%.255s",c+8); v2=0; } }
        ln=nl?nl+1:NULL;
    }
    if(!path[0]) return;
    snprintf(M->cg_path,sizeof M->cg_path,"%.95s",path);
    const char *base = v2 ? "/sys/fs/cgroup" : "/sys/fs/cgroup/memory";
    const char *lf = v2 ? "memory.max" : "memory.limit_in_bytes", *uf = v2 ? "memory.current" : "memory.usage_in_bytes";
    char dir[600]; snprintf(dir,sizeof dir,"%s%s",base,strcmp(path,"/")?path:"");
    double lim=cg_num(dir,lf), use=cg_num(dir,uf);
    if(lim==-2){ lim=cg_num(base,lf); use=cg_num(base,uf); }   /* the mount root is this group */
    if(lim==-2) return;
    M->cg_limit=lim; M->cg_usage=use<0?-1:use;
}
static void parse_psi(mem_t *M){
    M->psi_some10=-1; M->psi_full10=-1;
    char b[512]; if(read_file("/proc/pressure/memory",b,sizeof b)<=0) return;
    char *p;
    if((p=strstr(b,"some avg10="))) M->psi_some10=strtod(p+11,NULL);
    if((p=strstr(b,"full avg10="))) M->psi_full10=strtod(p+11,NULL);
}

/* diskstats: whole devices present in /sys/block only */
/* diskstats fields (1-based): 1 rd_ios 2 rd_merges 3 rd_sect 4 rd_ticks
   5 wr_ios 6 wr_merges 7 wr_sect 8 wr_ticks 9 in_flight 10 io_ticks 11 time_in_queue.
   await = (rd_ticks+wr_ticks)/(rd_ios+wr_ios), the same definition as iostat's. */
typedef struct { char name[32]; unsigned long rsect,wsect,io_ms;
                 unsigned long rios,wios,rticks,wticks,inflight,tiq; } dstat_t;
#define MAXDISK 256
static int g_disk_dropped=0;   /* Whole devices past MAXDISK, left out of the busiest-device pick */
static int parse_diskstats(dstat_t *out,int max){
    FILE *fp=fopen("/proc/diskstats","r"); if(!fp) return 0;
    char line[512]; int n=0, dropped=0;
    while(fgets(line,sizeof line,fp)){
        char nm[32]; unsigned long f[16];
        int k=sscanf(line,"%*u %*u %31s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                     nm,&f[0],&f[1],&f[2],&f[3],&f[4],&f[5],&f[6],&f[7],&f[8],&f[9],&f[10]);
        if(k<11) continue;
        if(k<12) f[10]=0;
        char sp[96]; snprintf(sp,sizeof sp,"/sys/block/%.31s",nm);
        if(!file_exists(sp)) continue;         /* Partitions excluded */
        if(n>=max){ dropped++; continue; }
        snprintf(out[n].name,sizeof out[n].name,"%s",nm);
        out[n].rsect=f[2]; out[n].wsect=f[6]; out[n].io_ms=f[9];
        out[n].rios=f[0]; out[n].wios=f[4]; out[n].rticks=f[3]; out[n].wticks=f[7];
        out[n].inflight=f[8]; out[n].tiq=f[10];
        n++;
    }
    fclose(fp); g_disk_dropped=dropped; return n;
}

/* ────────────────────── paramdump ────────────────────── */
typedef struct { char name[64]; long long val; } param_t;
static param_t g_prm[MAXPARAM]; static int g_nprm=0;
static long long g_pagesize=0;

static long long prm_get(const char *n){
    for(int i=0;i<g_nprm;i++) if(!strcmp(g_prm[i].name,n)) return g_prm[i].val;
    return -1;
}
/* Server-down fallback: parse $CUBRID/conf/cubrid.conf directly (settings only).
   The engine reads [common] and then the database's own [@db]
   section, which wins; any other section ([@otherdb], [service]) does not apply.
   As in the engine, blanks inside the brackets are ignored, [common] matches in
   any case and [@db] matches the name exactly.  db may be NULL or "-" when the
   target is unknown: [common] only. */
static int params_from_conf(const char *cubrid,const char *db){
    char path[512]; snprintf(path,sizeof path,"%s/conf/cubrid.conf",cubrid?cubrid:"");
    FILE *fp=fopen(path,"r"); if(!fp) return 0;
    const char *want = (db && db[0] && strcmp(db,"-")) ? db : NULL;
    char line[512];
    int sect=0;                     /* 0 other, 1 [common], 2 [@db] */
    for(int pass=1;pass<=2;pass++){ /* [common] first, then [@db] over it */
        rewind(fp); sect=0;
        while(fgets(line,sizeof line,fp)){
            char *h=strchr(line,'#'); if(h) *h=0;
            char *p=line; while(*p==' '||*p=='\t') p++;
            if(*p=='['){
                char *e=p+strlen(p);
                while(e>p && isspace((unsigned char)e[-1])) e--;
                sect=0;
                if(e-p>=2 && e[-1]==']'){
                    char *a=p+1, *z=e-1;
                    while(a<z && isspace((unsigned char)*a)) a++;
                    while(z>a && isspace((unsigned char)z[-1])) z--;
                    if(*a=='@'){
                        char *n=a+1; while(n<z && isspace((unsigned char)*n)) n++;
                        if(want && (size_t)(z-n)==strlen(want) && !strncmp(n,want,(size_t)(z-n))) sect=2;
                    } else if(z-a==6 && !strncasecmp(a,"common",6)) sect=1;
                }
                continue;
            }
            if(sect!=pass||*p==0||*p=='\n') continue;
            char *eq=strchr(p,'='); if(!eq) continue;
            *eq=0;
            char nm[64]; int n=0;
            for(char *q=p; *q && n<63; q++)
                if(*q!=' '&&*q!='\t'&&*q!='\n'&&*q!='\r') nm[n++]=(char)tolower((unsigned char)*q);
            nm[n]=0;
            char *v=eq+1; while(*v==' '||*v=='\t') v++;
            long long val=parse_size(v);
            if(val<0||!nm[0]) continue;
            /* A repeated name overwrites, so terse has no duplicate keys and [@db] wins */
            { int hit=-1;
              for(int i=0;i<g_nprm;i++) if(!strcmp(g_prm[i].name,nm)){ hit=i; break; }
              if(hit>=0) g_prm[hit].val=val;
              else if(g_nprm<MAXPARAM){
                  snprintf(g_prm[g_nprm].name,sizeof g_prm[g_nprm].name,"%s",nm);
                  g_prm[g_nprm].val=val; g_nprm++;
              } }
        }
    }
    fclose(fp);
    long long dbs=prm_get("data_buffer_size"), dbp=prm_get("data_buffer_pages");
    if(dbs>0&&dbp>0) g_pagesize=dbs/dbp;
    if(g_pagesize<=0) g_pagesize=16384;
    return g_nprm;
}

/* The paramdump child (~0.3s) dominates wall clock, so its start and harvest
   straddle the sample window.  No shell: the db name comes from cmdline and
   CUBRID accepts quotes, ; , $ and backticks in it, so execvp passes arguments
   as an array rather than composing a command string. */
static pid_t g_pd_pid=-1;          /* The child to reap or time out */
static int g_pd_applied=0;         /* The last paramdump returned values (a booting server may give none) */
static char  g_pd_why[128]="";     /* Why the child could not start, kept for the screen and terse */

/* Room for one more process?  In a container the cgroup pids limit binds before
   ulimit -u, and at the limit the casualty is whatever else shares the
   container.  Stand down when the margin is thin; if the limit cannot be read,
   do not interfere. */
#define PD_PIDS_MARGIN 64
static int pids_headroom_ok(char *why,size_t wn){
    static const char *MAXP[]={"/sys/fs/cgroup/pids/pids.max","/sys/fs/cgroup/pids.max"};
    static const char *CURP[]={"/sys/fs/cgroup/pids/pids.current","/sys/fs/cgroup/pids.current"};
    char b[64];
    for(int i=0;i<2;i++){
        if(read_file(MAXP[i],b,sizeof b)<=0) continue;
        if(!strncmp(b,"max",3)) return 1;                  /* Unlimited */
        long mx=strtol(b,NULL,10); if(mx<=0) continue;
        if(read_file(CURP[i],b,sizeof b)<=0) continue;
        long cu=strtol(b,NULL,10); if(cu<0) continue;
        if(mx-cu < PD_PIDS_MARGIN){
            snprintf(why,wn,g_ascii?"pids headroom %ld (limit %ld) - paramdump skipped":"pids 여유 %ld (한도 %ld) — paramdump 생략",mx-cu,mx);
            return 0;
        }
        return 1;
    }
    return 1;                                              /* Unknown: proceed */
}

/* The server's own environment value (same owner only): 1 found, 0 not set, -1 unreadable.
   The two zeros differ: a variable the server never set has the engine's default. */
static int srv_env(int pid,const char *name,char *out,size_t on){
    char p[64]; snprintf(p,sizeof p,"/proc/%d/environ",pid);
    int fd=open(p,O_RDONLY|O_CLOEXEC); if(fd<0) return -1;
    static char e[65536]; ssize_t n=read(fd,e,sizeof e-1); close(fd);
    if(n<0) return -1;
    if(n==0) return 0;
    e[n]=0;
    size_t nl=strlen(name);
    for(char *q=e;q<e+n;q+=strlen(q)+1)
        if(!strncmp(q,name,nl) && q[nl]=='='){ snprintf(out,on,"%s",q+nl+1); return 1; }
    return 0;
}
/* The server's install root: <root>/lib/libcubrid.so.N from its mapping, else <root>/bin/cub_server
   from /proc/<pid>/exe.  1 found, 0 unreadable, -1 the mapped library was replaced on disk. */
static int srv_root(int pid,char *root,size_t rn){
    char mp[64], ln[1024]; root[0]=0;
    snprintf(mp,sizeof mp,"/proc/%d/maps",pid);
    FILE *mf=fopen(mp,"re");
    if(mf){ while(fgets(ln,sizeof ln,mf)){
                char *q=strstr(ln,"/libcubrid.so."); if(!q) continue;
                if(strstr(ln,"(deleted)")){ fclose(mf); return -1; }
                char *b=strchr(ln,'/'); *q=0;
                char *sl=strrchr(b,'/'); if(sl && sl>b) *sl=0;      /* drop "/lib" */
                snprintf(root,rn,"%s",b); break; }
            fclose(mf); }
    if(!root[0]){
        char ep[64], ex[512]; snprintf(ep,sizeof ep,"/proc/%d/exe",pid);
        ssize_t n=readlink(ep,ex,sizeof ex-1);
        size_t el=n>0?(size_t)n:0; if(n>0) ex[n]=0;
        if(n>0 && strstr(ex,"(deleted)")) return -1;
        if(el>15 && !strcmp(ex+el-15,"/bin/cub_server")){ ex[el-15]=0; snprintf(root,rn,"%s",ex); }
    }
    if(root[0]){ char b[600]; snprintf(b,sizeof b,"%s/bin/cubrid",root); if(access(b,X_OK)!=0) root[0]=0; }
    return root[0]?1:0;
}
/* cubrid.conf of the server's own install, read before paramdump is asked: the frame has
   configured values at once, and keeps them when paramdump never answers (skipped, refused,
   hung).  An answer replaces them (paramdump_apply).  Falls back to this shell's $CUBRID
   only when the server's install cannot be read, as the server-down path does. */
static void params_preload(int pid,const char *db){
    char cub[512]="";
    g_pagesize=0;
    if(pid>=0 && srv_env(pid,"CUBRID",cub,sizeof cub)<=0) (void)srv_root(pid,cub,sizeof cub);
    if(!cub[0]){ const char *own=getenv("CUBRID"); if(own && *own) snprintf(cub,sizeof cub,"%s",own); }
    if(cub[0]) params_from_conf(cub,db);
    if(g_pagesize<=0) g_pagesize=16384;
}
/* paramdump runs from the install the server itself mapped libcubrid.so from, so the
   client and the server are the same build: a client of another version waits on the
   handshake without an answer (11.4.5 against 11.5: no output in 20s).  Async: the pipe
   is drained each frame without blocking and the child reaped with WNOHANG. */
#define PD_WAIT_MS 10000           /* An attempt still running after this is killed */
enum { PD_NONE, PD_RUNNING, PD_OK, PD_REFUSED, PD_HUNG, PD_SKIPPED };
static int g_pd_state=PD_NONE;
static int g_pd_async=0;           /* Live: harvest without blocking; one-shot waits up to the deadline */
static double g_pd_deadline=0;
static char g_pd_buf[65536]; static size_t g_pd_used=0;
/* Killed children not yet reaped.  A child that gets SIGKILL is not gone at once: it must
   be scheduled to die, and one blocked in uninterruptible sleep (a disk or NFS wait) only
   dies when that wait ends.  Waiting for it with a blocking waitpid would freeze the live
   frame for as long, so after a short bounded wait the pid is kept here and reaped with
   WNOHANG on every frame and idle tick until it has gone. */
static pid_t g_pd_zombie[64]; static int g_pd_nz=0;
static void paramdump_apply(char *buf);
static void pd_reap_zombies(void){
    int m=0;
    for(int i=0;i<g_pd_nz;i++){ int st; pid_t r=waitpid(g_pd_zombie[i],&st,WNOHANG);
        if(r==0) g_pd_zombie[m++]=g_pd_zombie[i]; }
    g_pd_nz=m;
}
/* Done with the child: still alive (EOF with the group still up, say) it is killed, so
   nothing forgotten here can outlive the run; reaped later */
static void pd_forget_child(void){
    if(g_pd_pid<0) return;
    int st; pid_t r=waitpid(g_pd_pid,&st,WNOHANG);
    if(r==0){ kill(-g_pd_pid,SIGKILL);
              /* A killed child normally dies within a millisecond or two: wait up to 20ms so
                 no zombie is left at all, without letting a stuck one hold the frame */
              for(int i=0;i<20 && r==0;i++){ usleep(1000); r=waitpid(g_pd_pid,&st,WNOHANG); }
              if(r==0 && g_pd_nz<(int)(sizeof g_pd_zombie/sizeof g_pd_zombie[0])) g_pd_zombie[g_pd_nz++]=g_pd_pid; }
    g_pd_pid=-1;
}
/* A child right after fork(): the parent's children are not its to stop, nor its terminal
   to restore, so the handlers it inherited go back to the default first */
static void child_forget_parent(void);
/* Every signal with a handler is held from fork() until the child's pid is recorded and
   the child has reset the handlers: in the parent, a handler running in that window would
   not see the child and leave it behind; in the child, it would run as the parent's */
static const int g_handled_sigs[]={SIGINT,SIGTERM,SIGHUP,SIGQUIT,SIGTSTP,SIGCONT,SIGABRT,SIGSEGV,SIGBUS,SIGFPE,
                                   SIGXCPU,SIGUSR1,SIGUSR2,SIGALRM,SIGPIPE,SIGXFSZ};
static void kids_hold(sigset_t *old){
    sigset_t m; sigemptyset(&m);
    for(size_t i=0;i<sizeof g_handled_sigs/sizeof g_handled_sigs[0];i++) sigaddset(&m,g_handled_sigs[i]);
    sigprocmask(SIG_BLOCK,&m,old);
}
static FILE *paramdump_start(int spid,const char *db){
    char bin[512], envc[600], envd[600], envl[1100], root[512]="", dbs[512]="", cub[512]="";
    g_pd_why[0]=0; g_pd_applied=0; pd_reap_zombies();   /* this instance's values are unread until it answers */
    if(!pids_headroom_ok(g_pd_why,sizeof g_pd_why)){ g_pd_state=PD_SKIPPED; return NULL; }
    int r=srv_root(spid,root,sizeof root);
    if(r<=0){ snprintf(g_pd_why,sizeof g_pd_why,"%s",r<0
                  ?(g_ascii?"server library replaced on disk - paramdump skipped":"서버 라이브러리가 디스크에서 교체됨 — paramdump 생략")
                  :(g_ascii?"server install not readable (another account) - paramdump skipped":"서버 설치 위치를 읽을 수 없음(다른 계정) — paramdump 생략"));
              g_pd_state=PD_SKIPPED; return NULL; }
    { int ec=srv_env(spid,"CUBRID",cub,sizeof cub);
      int ed=srv_env(spid,"CUBRID_DATABASES",dbs,sizeof dbs);
      if(ec<0 || ed<0){
          snprintf(g_pd_why,sizeof g_pd_why,"%s",g_ascii?"server environment not readable - paramdump skipped":"서버 환경을 읽을 수 없음 — paramdump 생략");
          g_pd_state=PD_SKIPPED; return NULL; }
      if(ec==0) snprintf(cub,sizeof cub,"%s",root);
      /* Not set in the server: the engine then uses $CUBRID/databases, so the client is
         given that same directory rather than whatever this shell has */
      if(ed==0) snprintf(dbs,sizeof dbs,"%s/databases",cub); }
    /* Run as the server's account: root running the install's own binary would execute
       whatever that account can write, and leave root-owned logs in $CUBRID/log */
    uid_t suid=(uid_t)-1; gid_t sgid=(gid_t)-1;
    { char sp[64], ln[256]; snprintf(sp,sizeof sp,"/proc/%d/status",spid);
      FILE *sf=fopen(sp,"re");
      if(sf){ while(fgets(ln,sizeof ln,sf)){ unsigned a;
                  if(sscanf(ln,"Uid: %u",&a)==1) suid=(uid_t)a; else if(sscanf(ln,"Gid: %u",&a)==1) sgid=(gid_t)a; }
              fclose(sf); } }
    if(suid==(uid_t)-1 || sgid==(gid_t)-1){
        snprintf(g_pd_why,sizeof g_pd_why,"%s",g_ascii?"server account unknown - paramdump skipped":"서버 계정 미상 — paramdump 생략");
        g_pd_state=PD_SKIPPED; return NULL; }
    snprintf(bin,sizeof bin,"%s/bin/cubrid",root);
    snprintf(envc,sizeof envc,"CUBRID=%s",cub);
    snprintf(envd,sizeof envd,"CUBRID_DATABASES=%s",dbs);
    { const char *old=getenv("LD_LIBRARY_PATH");
      snprintf(envl,sizeof envl,"LD_LIBRARY_PATH=%s/lib%s%s",
               root, (old&&*old)?":":"", (old&&*old)?old:""); }
    int fd[2];
    if(pipe(fd)<0){ snprintf(g_pd_why,sizeof g_pd_why,g_ascii?"pipe failed: %s":"pipe 실패: %s",strerror(errno));
                    g_pd_state=PD_SKIPPED; return NULL; }
    sigset_t held; kids_hold(&held);
    pid_t pid=fork();
    if(pid<0){ /* EAGAIN means the host process limit; passing over it silently would leave no way
                  to tell why the parameters are empty. */
               int e=errno; sigprocmask(SIG_SETMASK,&held,NULL);
               snprintf(g_pd_why,sizeof g_pd_why,g_ascii?"fork failed: %s":"fork 실패: %s",strerror(e));
               close(fd[0]); close(fd[1]); g_pd_state=PD_SKIPPED; return NULL; }
    if(pid==0){                                  /* Child */
        child_forget_parent(); sigprocmask(SIG_SETMASK,&held,NULL);   /* exec keeps the mask */
        setpgid(0,0);                            /* Own pgid, so killing takes the grandchildren too */
        close(fd[0]);
        dup2(fd[1],STDOUT_FILENO); dup2(fd[1],STDERR_FILENO); close(fd[1]);   /* the error line is the reason */
        { int nul=open("/dev/null",O_RDONLY); if(nul>=0){ dup2(nul,STDIN_FILENO); close(nul); } }
        /* Nothing of ours reaches the child: /proc/<target>/mem, pagemap, recordings */
        { DIR *dd=opendir("/proc/self/fd"); struct dirent *de; int keep=dd?dirfd(dd):-1;
          if(dd){ while((de=readdir(dd))){ int f=atoi(de->d_name); if(f>2 && f!=keep) close(f); } closedir(dd); } }
        if(geteuid()==0 && suid!=0){
            if(setgroups(0,NULL)!=0 || setgid(sgid)!=0 || setuid(suid)!=0 || getuid()!=suid) _exit(126);
        }
        putenv(envc); putenv(envd); putenv(envl);
        /* -C: client-server only, never opening a stopped database stand-alone */
        char *av[5]; av[0]=bin; av[1]=(char*)"paramdump"; av[2]=(char*)"-C"; av[3]=(char*)db; av[4]=NULL;
        execvp(bin,av);
        _exit(127);                              /* exec failed; the parent sees empty output */
    }
    close(fd[1]);
    setpgid(pid,pid);                            /* Race guard: the parent sets it once too */
    { int fl=fcntl(fd[0],F_GETFL,0); if(fl>=0) fcntl(fd[0],F_SETFL,fl|O_NONBLOCK);
      fcntl(fd[0],F_SETFD,FD_CLOEXEC); }
    g_pd_pid=pid; g_pd_used=0; g_pd_state=PD_RUNNING; g_pd_deadline=now_ms()+PD_WAIT_MS;
    sigprocmask(SIG_SETMASK,&held,NULL);
    FILE *pf=fdopen(fd[0],"r");
    if(!pf){                                     /* nobody would read or reap it: stop it here */
        int e=errno; close(fd[0]); kill(-pid,SIGKILL); pd_forget_child(); pd_reap_zombies();
        g_pd_state=PD_SKIPPED;
        snprintf(g_pd_why,sizeof g_pd_why,g_ascii?"fdopen failed: %s - paramdump skipped":"fdopen 실패: %s — paramdump 생략",strerror(e));
    }
    return pf;
}
/* Stop an attempt now: the group is killed and reaped later without waiting */
static void paramdump_abort(FILE **pd){
    if(!*pd) return;
    if(g_pd_pid>0) kill(-g_pd_pid,SIGKILL);
    fclose(*pd); *pd=NULL; pd_forget_child(); pd_reap_zombies();
    if(g_pd_state==PD_RUNNING) g_pd_state=PD_NONE;
}
/* main's request, stopped by exit() on whichever return main takes */
static FILE **g_pd_atexit=NULL;
static void pd_atexit(void){ if(g_pd_atexit) paramdump_abort(g_pd_atexit); }
/* Drain the pipe.  wait_ms 0 never blocks (live frames); one-shot passes the deadline so the
   values reach its single frame.  Returns 1 once the attempt has ended (*pd closed). */
static int paramdump_poll(FILE **pd,int wait_ms){
    pd_reap_zombies();
    if(!*pd) return 1;
    int fd=fileno(*pd);
    double until=now_ms()+wait_ms;
    for(;;){
        ssize_t n=read(fd,g_pd_buf+g_pd_used,sizeof g_pd_buf-1-g_pd_used);
        if(n>0){ g_pd_used+=(size_t)n; if(g_pd_used+1<sizeof g_pd_buf) continue; }
        int eof = n==0 || g_pd_used+1>=sizeof g_pd_buf || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR);
        if(eof){
            g_pd_buf[g_pd_used]=0;
            fclose(*pd); *pd=NULL;
            int st=0, exited=0;
            if(g_pd_pid>0){ pid_t r=waitpid(g_pd_pid,&st,WNOHANG); if(r==g_pd_pid){ exited=1; g_pd_pid=-1; } else pd_forget_child(); }
            if(strchr(g_pd_buf,'=')){ g_pd_state=PD_OK; paramdump_apply(g_pd_buf); }
            else {
                /* Refused (server booting, max_clients, access) prints one line and exits */
                char line[160]=""; sscanf(g_pd_buf," %159[^\n]",line);
                g_pd_state = (exited && WIFEXITED(st) && WEXITSTATUS(st)==127) ? PD_SKIPPED : PD_REFUSED;
                snprintf(g_pd_why,sizeof g_pd_why,"paramdump: %s",line[0]?line:(g_ascii?"no output":"출력 없음"));
                g_pd_applied=0; if(g_pagesize<=0) g_pagesize=16384;
            }
            return 1;
        }
        double now=now_ms();
        if(now>=g_pd_deadline){
            if(g_pd_pid>0) kill(-g_pd_pid,SIGKILL);
            fclose(*pd); *pd=NULL; pd_forget_child();
            g_pd_state=PD_HUNG; g_pd_applied=0; if(g_pagesize<=0) g_pagesize=16384;
            snprintf(g_pd_why,sizeof g_pd_why,g_ascii?"paramdump gave no answer in %ds - cubrid.conf values kept (p reads again)"
                                                  :"paramdump %d초 무응답 — cubrid.conf 값 유지 (p 전환 시 다시 읽음)",PD_WAIT_MS/1000);
            return 1;
        }
        if(now>=until) return 0;
        struct pollfd pf={fd,POLLIN,0};
        double left=(until<g_pd_deadline?until:g_pd_deadline)-now;
        poll(&pf,1,(int)(left>1?left:1));
    }
}
/* Live loops that draw no frame (pause, help) still drain the request each tick, so the
   deadline is kept from one place and an answer is applied as soon as it comes */
static FILE **g_pd_idle=NULL;
static void pd_idle_tick(void){ pd_reap_zombies(); if(g_pd_idle && *g_pd_idle) paramdump_poll(g_pd_idle,0); }
static void paramdump_apply(char *buf){
    long long dbp=-1;
    g_pd_applied = strchr(buf,'=')!=NULL;
    if(g_pd_applied) g_nprm=0;       /* The applied values replace any read from cubrid.conf */
    for(char *line=buf,*nl; *line; line=nl){
        nl=strchr(line,'\n');
        if(nl) *nl++=0; else nl=line+strlen(line);
        /* 11.x prints "[S ] name=value", 10.x prints "name=value" - accept both.
           Requiring ']' discarded all 10.0 output, leaving zero parameters.  The
           prefix test only looks for ']' within the first 8 columns, so a ']'
           inside a value is not mistaken for one. */
        char *b=line;
        { char *br=strchr(line,']');
          if(br && br-line<8){ b=br+1; while(*b==' ')b++; } }
        char *eq=strchr(b,'='); if(!eq) continue;
        *eq=0;
        char nm[64]; snprintf(nm,sizeof nm,"%s",b);
        char *v=eq+1; while(*v==' ')v++;
        long long val=parse_size(v);
        if(val<0) continue;
        if(g_nprm<MAXPARAM){
            snprintf(g_prm[g_nprm].name,sizeof g_prm[g_nprm].name,"%s",nm);
            g_prm[g_nprm].val=val; g_nprm++;
        }
        if(!strcmp(nm,"data_buffer_pages")) dbp=val;
    }
    long long dbs=prm_get("data_buffer_size");
    if(dbs>0 && dbp>0) g_pagesize=dbs/dbp;
    if(g_pagesize<=0) g_pagesize=16384;
}

/* ---------------- Direct ELF .symtab parsing (no nm) ---------------- */
/* ======= Builtin DWARF engine (derived from tools/dwoff.c; keep both in sync) ======= */

/* dw_now_ms timed the standalone version; unused here */
/* The .so comes from whatever process is named cub_server, so its DWARF is not
   trusted any more than its symbol table (see sym_lookup).  Every read is bounded
   by the end of the area being parsed: DW_END is set by the caller, and a read that
   would cross it stops and raises DW_BAD, which the caller turns into a failure. */
static const unsigned char *DW_END; static int DW_BAD;
static uint64_t uleb(const unsigned char**p){ uint64_t r=0; int s=0; unsigned char b;
    do{ if(*p>=DW_END){ DW_BAD=1; return r; }
        b=*(*p)++; if(s<64) r|=(uint64_t)(b&0x7f)<<s; s+=7; }while(b&0x80); return r; }
static int64_t sleb(const unsigned char**p){ int64_t r=0; int s=0; unsigned char b;
    do{ if(*p>=DW_END){ DW_BAD=1; return r; }
        b=*(*p)++; if(s<64) r|=(int64_t)(b&0x7f)<<s; s+=7; }while(b&0x80);
    if(s<64&&(b&0x40)) r|=-((int64_t)1<<s);
    return r; }

/* DWARF tags and attributes */
#define T_struct   0x13
#define T_class    0x02
#define T_union    0x17
#define T_member   0x0d
#define T_var      0x34
#define T_typedef  0x16
#define T_const    0x26
#define T_volatile 0x35
#define T_base     0x24
#define A_name     0x03
#define A_bsize    0x0b
#define A_dml      0x38
#define A_type     0x49
#define A_loc      0x02
#define A_decl     0x3c
#define A_lnk      0x6e
#define A_mips_lnk 0x2007

static const unsigned char *DI,*AB,*DS; static size_t DI_SZ, AB_SZ, DS_SZ;

typedef struct { unsigned long start,first,end; uint64_t ab_off; int ver,addr_sz,off_sz; } cu_t;
static cu_t *CU; static int NCU;

typedef struct { uint64_t at,form; int64_t ic; } af_t;
typedef struct { uint64_t code,tag; int kids,n; af_t a[160]; } ab_t;
/* TAB is ab_t (about 3.9KB) x 32768 = 126MB; reserving that in BSS for a one-time
   extraction would inflate the static binary's core dumps and potential RSS.  The
   extraction path already mallocs, so borrow this from the heap and return it. */
#define NTAB_MAX 32768
static ab_t *TAB=NULL; static int NTAB;
static long G_desync=0;

static int load_abbrev(uint64_t off){
    NTAB=0;
    if(off>=AB_SZ) return -1;
    const unsigned char*a=AB+off;
    DW_END=AB+AB_SZ; DW_BAD=0;
    while(NTAB<NTAB_MAX){
        uint64_t code=uleb(&a); if(DW_BAD) return -1; if(!code) return 0;
        ab_t*t=&TAB[NTAB++]; t->code=code; t->tag=uleb(&a);
        if(DW_BAD||a>=DW_END) return -1;
        t->kids=*a++; t->n=0;
        for(;;){
            uint64_t at=uleb(&a), fm=uleb(&a); int64_t ic=0;
            if(fm==0x21) ic=sleb(&a);
            if(DW_BAD) return -1;
            if(!at&&!fm) break;
            if(t->n>=160) return -1;
            t->a[t->n].at=at; t->a[t->n].form=fm; t->a[t->n].ic=ic; t->n++;
        }
    }
    return -1;
}
static ab_t* ab_find(uint64_t code){
    for(int i=0;i<NTAB;i++) if(TAB[i].code==code) return &TAB[i];
    return NULL;
}
/* Consume one form; an unknown form fails at once rather than desyncing silently */
static int form_skip(const unsigned char**p,const unsigned char*lim,uint64_t form,int64_t ic,
                     const cu_t*cu,uint64_t*uv,const char**sv){
    const unsigned char*q=*p; uint64_t v=0;
    if(q>=lim&&form!=0x19&&form!=0x21) return -1;
    /* Room for n more bytes, checked before they are read */
    #define NEED(n_) do{ if((size_t)(lim-q)<(size_t)(n_)) return -1; }while(0)
    #define BLOCK(n_) do{ uint64_t bn_=(n_); if(DW_BAD||bn_>(uint64_t)(lim-q)) return -1; \
        if(bn_&&*q==0x23){ const unsigned char*r=q+1; v=uleb(&r); } q+=bn_; }while(0)
    switch(form){
    case 0x01: NEED(cu->addr_sz); v=cu->addr_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->addr_sz; break;
    case 0x03: NEED(2); v=*(uint16_t*)q; q+=2; NEED(v); q+=v; break;     /* block2: value = length */
    case 0x04: NEED(4); v=*(uint32_t*)q; q+=4; NEED(v); q+=v; break;     /* block4: value = length */
    case 0x05: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x06: NEED(4); v=*(uint32_t*)q; q+=4; break;
    case 0x07: NEED(8); v=*(uint64_t*)q; q+=8; break;
    case 0x08: { const unsigned char*z=memchr(q,0,(size_t)(lim-q));
                 if(!z) return -1;
                 if(sv) *sv=(const char*)q;
                 q=z+1; } break;
    case 0x09: { uint64_t n=uleb(&q); BLOCK(n); } break;
    case 0x0a: { NEED(1); uint64_t n=*q++; BLOCK(n); } break;
    case 0x0b: NEED(1); v=*q++; break;
    case 0x0c: NEED(1); v=*q++; break;
    case 0x0d: v=(uint64_t)sleb(&q); break;
    case 0x0e: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz;
               if(sv&&DS){ if(v>=DS_SZ||!memchr(DS+v,0,DS_SZ-(size_t)v)) return -1;
                           *sv=(const char*)(DS+v); }
               break;
    case 0x0f: v=uleb(&q); break;
    case 0x10: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break; /* Section-relative */
    case 0x11: NEED(1); v=cu->start+*q; q+=1; break;                                      /* CU-relative from here */
    case 0x12: NEED(2); v=cu->start+*(uint16_t*)q; q+=2; break;
    case 0x13: NEED(4); v=cu->start+*(uint32_t*)q; q+=4; break;
    case 0x14: NEED(8); v=cu->start+*(uint64_t*)q; q+=8; break;
    case 0x15: { uint64_t r=uleb(&q); v=cu->start+r; } break;
    case 0x16: { uint64_t f2=uleb(&q); if(DW_BAD||f2==0x16) return -1;   /* indirect to indirect: no end */
                 *p=q; return form_skip(p,lim,f2,0,cu,uv,sv); }
    case 0x17: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break;
    case 0x18: { uint64_t n=uleb(&q); BLOCK(n); } break;
    case 0x19: v=1; break;
    case 0x1a: v=uleb(&q); break;
    case 0x1b: v=uleb(&q); break;
    case 0x1c: NEED(4); q+=4; break;
    case 0x1d: NEED(cu->off_sz); q+=cu->off_sz; break;
    case 0x1e: NEED(16); q+=16; break;
    case 0x1f: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break;
    case 0x20: NEED(8); q+=8; break;
    case 0x21: v=(uint64_t)ic; break;
    case 0x22: v=uleb(&q); break;
    case 0x23: v=uleb(&q); break;
    case 0x24: NEED(8); q+=8; break;
    case 0x25: NEED(1); v=*q++; break;
    case 0x26: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x27: NEED(3); v=(uint64_t)q[0]|((uint64_t)q[1]<<8)|((uint64_t)q[2]<<16); q+=3; break;
    case 0x28: NEED(4); v=*(uint32_t*)q; q+=4; break;
    case 0x29: NEED(1); v=*q++; break;
    case 0x2a: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x2b: NEED(3); v=(uint64_t)q[0]|((uint64_t)q[1]<<8)|((uint64_t)q[2]<<16); q+=3; break;
    case 0x2c: NEED(4); v=*(uint32_t*)q; q+=4; break;
    default: return -1;
    }
    #undef NEED
    #undef BLOCK
    if(DW_BAD||q>lim) return -1;
    if(uv)*uv=v;
    *p=q;
    return 0;
}
/* The debug sections, and what about them this engine cannot read: compressed sections
   (-gz: SHF_COMPRESSED; old .zdebug_*), debug info moved to another file (.gnu_debuglink),
   dwz's shared file (.gnu_debugaltlink), DWARF5 string indices (.debug_str_offsets, strx).
   Sets DI/AB/DS.  0 = read, -1 = section table outside the file, -2 = names outside it. */
enum { DWF_COMPRESSED=1, DWF_ZDEBUG=2, DWF_DEBUGLINK=4, DWF_ALTLINK=8, DWF_STROFFS=16 };
static int G_split=0;                /* DWARF5 skeleton/split units: the DIEs are in .dwo files */
static int dw_sections(const unsigned char*m,size_t fsz,int*flags){
    #define DS_IN(off_,len_) ((uint64_t)(off_) <= (uint64_t)fsz && (uint64_t)(len_) <= (uint64_t)fsz - (uint64_t)(off_))
    const Elf64_Ehdr*eh=(const Elf64_Ehdr*)m; *flags=0;
    if(eh->e_shentsize!=sizeof(Elf64_Shdr) || eh->e_shnum==0 || eh->e_shstrndx>=eh->e_shnum ||
       !DS_IN(eh->e_shoff,(uint64_t)eh->e_shnum*sizeof(Elf64_Shdr))) return -1;
    const Elf64_Shdr*sh=(const Elf64_Shdr*)(m+eh->e_shoff);
    const Elf64_Shdr*ssh=&sh[eh->e_shstrndx];
    if(!DS_IN(ssh->sh_offset,ssh->sh_size)) return -2;
    const char*ss=(const char*)(m+ssh->sh_offset);
    for(int i=0;i<eh->e_shnum;i++){
        if(sh[i].sh_name>=ssh->sh_size || !memchr(ss+sh[i].sh_name,0,ssh->sh_size-sh[i].sh_name)) continue;
        const char*n=ss+sh[i].sh_name;
        if(!strncmp(n,".zdebug_",8)) *flags|=DWF_ZDEBUG;
        if(!strcmp(n,".gnu_debuglink")) *flags|=DWF_DEBUGLINK;
        if(!strcmp(n,".gnu_debugaltlink")) *flags|=DWF_ALTLINK;
        if(!strcmp(n,".debug_str_offsets")) *flags|=DWF_STROFFS;
        if(!DS_IN(sh[i].sh_offset,sh[i].sh_size)) continue;   /* a section outside the file is not used */
        int dbg = !strcmp(n,".debug_info")||!strcmp(n,".debug_abbrev")||!strcmp(n,".debug_str");
        if(dbg && (sh[i].sh_flags&SHF_COMPRESSED)){ *flags|=DWF_COMPRESSED; continue; }
        if(!strcmp(n,".debug_info")){ DI=m+sh[i].sh_offset; DI_SZ=sh[i].sh_size; }
        else if(!strcmp(n,".debug_abbrev")){ AB=m+sh[i].sh_offset; AB_SZ=sh[i].sh_size; }
        else if(!strcmp(n,".debug_str")){    DS=m+sh[i].sh_offset; DS_SZ=sh[i].sh_size; }
    }
    #undef DS_IN
    return 0;
}
static int build_index(void){
    int cap=4096; CU=malloc(sizeof(cu_t)*cap); NCU=0;
    if(!CU) return 0;
    const unsigned char*p=DI,*end=DI+DI_SZ;
    while(p+11<end){
        unsigned long cs=(unsigned long)(p-DI);
        uint32_t len=*(uint32_t*)p; p+=4; int osz=4;
        if(len==0xffffffff){ len=(uint32_t)*(uint64_t*)p; p+=8; osz=8; }
        if(len>(uint64_t)(end-p)) break;
        const unsigned char*ce=p+len;
        if((size_t)(ce-p) < (size_t)(2+2+osz)) break;    /* version, unit type/addr size, abbrev offset */
        uint16_t ver=*(uint16_t*)p; p+=2;
        uint64_t ao; int asz;
        if(ver>=5){ int ut=*p++; asz=*p++; ao=osz==8?*(uint64_t*)p:*(uint32_t*)p; p+=osz;
                    if(ut==4||ut==5){ p+=8; G_split++; }                   /* skeleton/split: dwo_id */
                    else if(ut==2||ut==6) p+=8+(size_t)osz;                 /* type unit: signature, type offset */
                    if(p>ce) break; }
        else { ao=osz==8?*(uint64_t*)p:*(uint32_t*)p; p+=osz; asz=*p++; }
        if(asz!=4 && asz!=8) break;                        /* form_skip reads addresses of this size */
        if(NCU==cap){ cu_t *t=realloc(CU,sizeof(cu_t)*(size_t)cap*2);
                      if(!t) break;           /* keep what is indexed so far */
                      CU=t; cap*=2; }
        cu_t*c=&CU[NCU++];
        c->start=cs; c->first=(unsigned long)(p-DI); c->end=(unsigned long)(ce-DI);
        c->ab_off=ao; c->ver=ver; c->addr_sz=asz; c->off_sz=osz;
        p=ce;
    }
    return NCU;
}
static cu_t* cu_of(unsigned long off){
    int lo=0,hi=NCU-1;
    while(lo<=hi){ int m=(lo+hi)/2;
        if(off<CU[m].start) hi=m-1; else if(off>=CU[m].end) lo=m+1; else return &CU[m]; }
    return NULL;
}

/* ---- One DIE ---- */
typedef struct {
    int tag; const char*name; long bsize; unsigned long type; int is_decl; int has_loc;
    int nmem; char mname[128][80]; long moff[128]; unsigned long mtype[128];
} die_t;

/* Walk the CUs to either collect variables matching a name or resolve a DIE at an offset */
typedef struct {
    const char*var;                 /* Variable name to find (mangled or source form) */
    unsigned long cand[32]; int ncand, cand_def[32];
    unsigned long want;             /* DIE offset to resolve */
    die_t out; int found;
    const char*type_name;           /* Find a struct by name */
    int pre;                        /* The one-pass index of every wanted name (prescan) */
} job_t;
/* One pass over .debug_info collects, for every name the table wants, the variable candidates
   and the first struct definition in each CU.  The per-key lookups below then read only the
   CUs that matter; a full pass per key (~55 of them over 100-200MB) took 26-46s. */
#define PRE_MAX 128
#define PRE_PER 256
typedef struct { const char*name; int is_type, next, last_cu;
                 int ncand; unsigned long cand[32]; int cand_def[32];
                 int ntd; unsigned long td[PRE_PER]; long tdsz[PRE_PER]; } pre_t;
static pre_t *PRE=NULL; static int NPRE=0, PRE_HEAD[256];
static unsigned pre_hash(const char*s){ unsigned h=2166136261u; while(*s){ h^=(unsigned char)*s++; h*=16777619u; } return h&255; }
static pre_t *pre_find(const char*nm,int is_type){
    if(!PRE||!nm) return NULL;
    for(int i=PRE_HEAD[pre_hash(nm)];i>=0;i=PRE[i].next)
        if(PRE[i].is_type==is_type && !strcmp(PRE[i].name,nm)) return &PRE[i];
    return NULL;
}
static void pre_add(const char*nm,int is_type){
    if(!nm||!PRE||NPRE>=PRE_MAX||pre_find(nm,is_type)) return;
    pre_t*e=&PRE[NPRE]; memset(e,0,sizeof *e); e->name=nm; e->is_type=is_type; e->last_cu=-1;
    unsigned h=pre_hash(nm); e->next=PRE_HEAD[h]; PRE_HEAD[h]=NPRE++;
}

static int scan_cu(cu_t*cu, job_t*J){
    if(load_abbrev(cu->ab_off)<0){ G_desync++; return -1; }
    const unsigned char*p=DI+cu->first, *lim=DI+cu->end;
    DW_END=lim; DW_BAD=0;
    int depth=0, in=-1;
    while(p<lim){
        unsigned long doff=(unsigned long)(p-DI);
        uint64_t code=uleb(&p);
        if(DW_BAD){ G_desync++; return -1; }
        if(!code){ if(depth>0)depth--; if(in>=0&&depth<=in) in=-1; continue; }
        ab_t*t=ab_find(code); if(!t){ G_desync++; return -1; }
        const char*nm=NULL,*lnk=NULL; uint64_t bsz=0,ty=(uint64_t)-1,dml=(uint64_t)-1;
        int is_decl=0,has_loc=0,have_dml=0;
        for(int i=0;i<t->n;i++){
            uint64_t v=0; const char*sv=NULL;
            if(form_skip(&p,lim,t->a[i].form,t->a[i].ic,cu,&v,&sv)<0){ G_desync++; return -1; }
            switch(t->a[i].at){
            case A_name: nm=sv; break;
            case A_lnk: case A_mips_lnk: lnk=sv; break;
            case A_bsize: bsz=v; break;
            case A_type: ty=v; break;
            case A_dml: dml=v; have_dml=1; break;
            case A_loc: has_loc=1; break;
            case A_decl: is_decl=(v!=0); break;
            default: break; }
        }
        if(J->pre){
            if(t->tag==T_var && ty!=(uint64_t)-1){
                pre_t*e=pre_find(nm,0); if(!e) e=pre_find(lnk,0);
                if(e && e->ncand<32){ e->cand[e->ncand]=(unsigned long)ty; e->cand_def[e->ncand]=(has_loc&&!is_decl); e->ncand++; }
            } else if((t->tag==T_struct||t->tag==T_class||t->tag==T_union) && nm && bsz>0){
                pre_t*e=pre_find(nm,1); int ci=(int)(cu-CU);
                if(e && e->last_cu!=ci && e->ntd<PRE_PER){ e->td[e->ntd]=doff; e->tdsz[e->ntd]=(long)bsz; e->ntd++; e->last_cu=ci; }
            }
            if(t->kids) depth++;
            continue;
        }
        if(J->var && t->tag==T_var && ty!=(uint64_t)-1 &&
           ((nm&&!strcmp(nm,J->var))||(lnk&&!strcmp(lnk,J->var)))){
            if(J->ncand<32){ J->cand[J->ncand]=(unsigned long)ty;
                             J->cand_def[J->ncand]=(has_loc&&!is_decl); J->ncand++; }
        }
        if(J->type_name && (t->tag==T_struct||t->tag==T_class||t->tag==T_union) &&
           nm && !strcmp(nm,J->type_name) && bsz>0 && !J->found){
            J->out.tag=t->tag; J->out.name=nm; J->out.bsize=(long)bsz; J->found=1;
            J->out.type=(unsigned long)-1; J->out.nmem=0; in=depth;
        }
        if(J->want && doff==J->want){
            J->out.tag=(int)t->tag; J->out.name=nm; J->out.bsize=(long)bsz;
            J->out.type=(unsigned long)ty; J->out.is_decl=is_decl; J->out.has_loc=has_loc;
            J->out.nmem=0; J->found=1; in=depth;
        }
        else if(in>=0 && t->tag==T_member && depth==in+1 && nm && have_dml && J->out.nmem<128){
            int k=J->out.nmem;
            snprintf(J->out.mname[k],80,"%s",nm);
            J->out.moff[k]=(long)dml; J->out.mtype[k]=(unsigned long)ty; J->out.nmem++;
        }
        if(t->kids) depth++;
    }
    if(p!=lim){ G_desync++; return -1; }      /* L1: must land exactly to be trusted */
    return 0;
}

/* Build the index for these names (vars: variable roots, types: struct names) */
static void prescan(const char*const*vars,int nv,const char*const*types,int nt){
    if(!PRE) PRE=(pre_t*)calloc(PRE_MAX,sizeof(pre_t));
    if(!PRE) return;
    NPRE=0; for(int i=0;i<256;i++) PRE_HEAD[i]=-1;
    for(int i=0;i<nv;i++) pre_add(vars[i],0);
    for(int i=0;i<nt;i++) pre_add(types[i],1);
    job_t J; memset(&J,0,sizeof J); J.pre=1;
    for(int c=0;c<NCU;c++) scan_cu(&CU[c],&J);
}
/* The DIE at an offset, read once: type chains and nested members revisit the same DIEs
   for every key, each visit a scan of a whole CU */
#define MEMO_N 4096
typedef struct { unsigned long off; int rc; die_t d; } memo_t;
static memo_t **MEMO=NULL;
static int die_at(unsigned long off,die_t*out){
    unsigned h=(unsigned)((off*2654435761u)>>4)&(MEMO_N-1);
    if(!MEMO) MEMO=(memo_t**)calloc(MEMO_N,sizeof *MEMO);
    for(unsigned k=0;MEMO && k<MEMO_N;k++){
        memo_t*m=MEMO[(h+k)&(MEMO_N-1)];
        if(!m) break;
        if(m->off==off){ if(m->rc==0) *out=m->d; return m->rc; }
    }
    cu_t*c=cu_of(off); int rc=-1;
    job_t*J=(job_t*)calloc(1,sizeof *J);
    if(c && J){ J->want=off; if(scan_cu(c,J)==0 && J->found){ *out=J->out; rc=0; } }
    if(MEMO){ memo_t*m=(memo_t*)malloc(sizeof *m);
        if(m){ m->off=off; m->rc=rc; if(rc==0) m->d=J->out;
            for(unsigned k=0;k<MEMO_N;k++){ unsigned i=(h+k)&(MEMO_N-1); if(!MEMO[i]){ MEMO[i]=m; m=NULL; break; } }
            free(m); } }
    free(J);
    return rc;
}
static void prescan_free(void){
    free(PRE); PRE=NULL; NPRE=0;
    if(MEMO){ for(int i=0;i<MEMO_N;i++) free(MEMO[i]); free(MEMO); MEMO=NULL; }
}
/* Resolve the type chain through typedef/const/volatile to a struct or base DIE */
static int resolve_type(unsigned long off, die_t*out){
    for(int g=0; g<16; g++){
        job_t J; memset(&J,0,sizeof J);
        if(die_at(off,&J.out)<0) return -1;
        if(J.out.tag==T_struct||J.out.tag==T_class||J.out.tag==T_union||J.out.tag==T_base){
            *out=J.out; return 0;
        }
        if(J.out.type==(unsigned long)-1) return -1;
        off=J.out.type;
    }
    return -1;
}
/* L2 value check */
static int sane_struct(const die_t*d){
    if(d->bsize<=0||d->bsize>(1<<20)) return 0;
    for(int i=0;i<d->nmem;i++)
        if(d->moff[i]<0||d->moff[i]>=d->bsize) return 0;
    return 1;
}
/* Symbol to type DIE; with several candidates, prefer the definition and discard those that fail */
static int resolve_symbol(const char*sym, die_t*out, unsigned long*type_off){
    job_t J; memset(&J,0,sizeof J); J.var=sym;
    pre_t*e=pre_find(sym,0);
    if(e){ J.ncand=e->ncand; memcpy(J.cand,e->cand,sizeof J.cand); memcpy(J.cand_def,e->cand_def,sizeof J.cand_def); }
    else for(int c=0;c<NCU;c++){ scan_cu(&CU[c],&J); }   /* Collect candidates from every CU */
    if(!J.ncand) return -1;
    /* Do not adopt when several DIEs share the name: a DIE picked by name alone is
       not guaranteed to be the symtab symbol (static variables repeat per CU).
       An ambiguous key is left empty; the runtime gate decides in any case. */
    if(J.ncand>1){
        /* Exception 1: an extern global declared in a header has many candidates but one
           definition (has a location, not a declaration) - use it if unique.
           Exception 2: if every candidate resolves to the same (type, sizeof) there
           is only one entity, so take the first.  Otherwise still refuse. */
        int ndef=0, di=-1;
        for(int i=0;i<J.ncand;i++) if(J.cand_def[i]){ ndef++; di=i; }
        if(ndef==1){ J.cand[0]=J.cand[di]; J.cand_def[0]=1; J.ncand=1; }
        else {
            die_t d0; int same=1;
            if(resolve_type(J.cand[0],&d0)<0) return -1;
            for(int i=1;i<J.ncand && same;i++){
                die_t di2;
                if(resolve_type(J.cand[i],&di2)<0 || di2.bsize!=d0.bsize ||
                   !d0.name || !di2.name || strcmp(d0.name,di2.name)) same=0;
            }
            if(!same) return -1;
            J.ncand=1;
        }
    }
    for(int pass=0;pass<2;pass++){                        /* Definitions first, then the rest */
        for(int i=0;i<J.ncand;i++){
            if(pass==0 && !J.cand_def[i]) continue;
            if(pass==1 &&  J.cand_def[i]) continue;
            die_t d;
            if(resolve_type(J.cand[i],&d)<0) continue;     /* Discard invalid candidates */
            if(d.tag!=T_base && !sane_struct(&d)) continue;
            *out=d; if(type_off)*type_off=J.cand[i]; return 0;
        }
    }
    return -1;
}
/* Search member names recursively, nested included, accumulating the offset */
static int find_member(const die_t*d,const char*want,long base,int depth,long*res,char*path,size_t plen){
    if(depth>4) return -1;
    for(int i=0;i<d->nmem;i++){
        if(!strcmp(d->mname[i],want)){ *res=base+d->moff[i];
            snprintf(path,plen,"%s",d->mname[i]); return 0; }
    }
    for(int i=0;i<d->nmem;i++){
        if(d->mtype[i]==(unsigned long)-1) continue;
        die_t sub;
        if(resolve_type(d->mtype[i],&sub)<0) continue;
        if(sub.tag==T_base) continue;
        if(!sane_struct(&sub)) continue;
        char p2[160];
        if(find_member(&sub,want,base+d->moff[i],depth+1,res,p2,sizeof p2)==0){
            snprintf(path,plen,"%.80s.%.160s",d->mname[i],p2); return 0;
        }
    }
    return -1;
}
/* sizeof a struct by name */
static int sizeof_type(const char*name,long*sz,const char**real){
    pre_t*e=pre_find(name,1);
    if(e){ for(int i=0;i<e->ntd;i++) if(e->tdsz[i]>0 && e->tdsz[i]<=(1<<20)){ *sz=e->tdsz[i]; if(real)*real=name; return 0; }
           if(e->ntd<PRE_PER) return -1; }
    job_t J; memset(&J,0,sizeof J); J.type_name=name;
    for(int c=0;c<NCU;c++){
        memset(&J.out,0,sizeof J.out); J.found=0; J.type_name=name;
        if(scan_cu(&CU[c],&J)<0) continue;
        if(J.found && J.out.bsize>0 && J.out.bsize<=(1<<20)){
            *sz=J.out.bsize; if(real)*real=name; return 0; }
    }
    return -1;
}

/* Member offset within a struct named directly (nested included), for types with no
   variable root - a BCB array element beyond a pointer.  Definitions only (bsize>0). */
static int member_of_type(const char*name,const char*member,long*off){
    pre_t*e=pre_find(name,1);
    if(e){ for(int i=0;i<e->ntd;i++){
               job_t K; memset(&K,0,sizeof K);
               if(die_at(e->td[i],&K.out)<0) continue;
               if(K.out.bsize>0 && K.out.nmem>0){
                   char path[320];
                   if(find_member(&K.out,member,0,0,off,path,sizeof path)==0) return 0; } }
           if(e->ntd<PRE_PER) return -1; }
    job_t J; memset(&J,0,sizeof J);
    for(int c=0;c<NCU;c++){
        memset(&J.out,0,sizeof J.out); J.found=0; J.type_name=name;
        if(scan_cu(&CU[c],&J)<0) continue;
        if(J.found && J.out.bsize>0 && J.out.nmem>0){
            char path[320];
            if(find_member(&J.out,member,0,0,off,path,sizeof path)==0) return 0;
        }
    }
    return -1;
}

/* ---- Extraction targets: the values actually read ----
   sym+member = member offset from a variable root; type alone = sizeof;
   type+member = member offset within that type. */
typedef struct { const char*key,*sym,*member,*type; long expect; } tgt_t;
static tgt_t TG[]={
  {"OFF_PGBUF_NBUF",    "pgbuf_Pool","num_buffers",          NULL,  0},
  {"OFF_XCACHE_ENTRIES","xcache_Global","entry_count",       NULL,440},
  {"OFF_XCACHE_USAGE",  "xcache_Global","memory_usage_cache",NULL,484},
  {"OFF_XCACHE_CLONE",  "xcache_Global","memory_usage_clone",NULL,488},
  {"OFF_QLIST_NPAGES",  "qfile_List_cache","n_pages",        NULL, 28},
  {"OFF_QPOOL_NENT",    "qfile_List_cache_entry_pool","n_entries",NULL,8},
  {"OFF_LK_NUMTRANS",   "lk_Gl","num_trans",                 NULL,  0},
  {"OFF_LF_ALLOCCNT",   "sessions","alloc_cnt",              NULL, 12},
  {"OFF_LF_ALLOCCNT2",  "catalog_Hashmap","alloc_cnt",       NULL, 12},
  {"SZ_SESSION_STATE",  NULL,NULL,"session_state",  312},
  {"SZ_CATALOG_ENTRY",  NULL,NULL,"catalog_entry",   48},
  {"SZ_FPCACHE_ENT",    NULL,NULL,"fpcache_ent",    120},
  {"SZ_LK_TRAN_LOCK",   NULL,NULL,"lk_tran_lock",   152},
  {"SZ_CSS_CONN_ENTRY", NULL,NULL,"css_conn_entry", 480},
  {"SZ_VACUUM_DATA",    NULL,NULL,"vacuum_data",    184},
  /* ---- Buffer pool BCB direct read (pgbuf section, volmap --bufmap); measured on 11.5 ---- */
  {"OFF_PGBUF_BCBTAB",  "pgbuf_Pool","BCB_table",            NULL,  8},
  {"SZ_PGBUF_BCB",      NULL,NULL,"pgbuf_bcb",               144},
  {"OFF_BCB_VPID",      NULL,"vpid","pgbuf_bcb",             44},
  {"OFF_BCB_FLAGS",     NULL,"flags","pgbuf_bcb",            64},
  {"OFF_BCB_OLDEST_LSA",NULL,"oldest_unflush_lsa","pgbuf_bcb",128},
  {"OFF_BCB_IOPAGE",    NULL,"iopage_buffer","pgbuf_bcb",    136},
  {"OFF_IOBUF_IOPAGE",  NULL,"iopage","pgbuf_iopage_buffer",   8},
  {"OFF_LOG_NXIO_LSA",  "log_Gl","nxio_lsa",                 NULL, 48},
  {"OFF_LOG_APPEND_LSA","log_Gl","append_lsa",               NULL,280},
  {"OFF_LOG_EOF_LSA",   "log_Gl","eof_lsa",                  NULL,488},
  /* Engine statistics (perfmon) for the capacity axis.  global_stats is a pointer to a
     UINT64 array whose indices pstat_Metadata[psid].start_offset fixes at runtime. */
  {"OFF_PSTAT_NVALS",   "pstat_Global","n_stat_values",       NULL,  0},
  {"OFF_PSTAT_GSTATS",  "pstat_Global","global_stats",        NULL,  8},
  {"OFF_PSTAT_INIT",    "pstat_Global","initialized",         NULL, 44},
  {"SZ_PSTAT_METADATA", NULL,NULL,"pstat_metadata",             56},
  {"OFF_PSTATMD_PSID",  NULL,"psid","pstat_metadata",            0},
  {"OFF_PSTATMD_START", NULL,"start_offset","pstat_metadata",   20},
  /* Engine-maintained buffer pool aggregates (zone counts, victim candidates, dirty count):
     kept by the engine for its own LRU and flush control, so read without a BCB scan */
  {"OFF_PGBUF_LRULIST", "pgbuf_Pool","buf_LRU_list",          NULL, 56},
  {"OFF_PGBUF_NLRU",    "pgbuf_Pool","num_LRU_list",          NULL, 40},
  {"OFF_PGBUF_QUOTA",   "pgbuf_Pool","quota",                 NULL, 328},
  {"OFF_QUOTA_NPRIV",   NULL,"num_private_LRU_list","pgbuf_page_quota", 0},
  {"OFF_PGBUF_MONITOR", "pgbuf_Pool","monitor",               NULL, 280},
  {"OFF_MON_DIRTIES",   NULL,"dirties_cnt","pgbuf_page_monitor",      0},
  {"SZ_PGBUF_LRU",      NULL,NULL,"pgbuf_lru_list",                 128},
  {"OFF_LRU_C1",        NULL,"count_lru1","pgbuf_lru_list",         80},
  {"OFF_LRU_C2",        NULL,"count_lru2","pgbuf_lru_list",         84},
  {"OFF_LRU_C3",        NULL,"count_lru3","pgbuf_lru_list",         88},
  {"OFF_LRU_VICT",      NULL,"count_vict_cand","pgbuf_lru_list",    92},
  /* Hash map kind and object lock pools: lockfree_hashmap::m_type is OLD(0)/NEW(1); only OLD
     keeps the lf freelist whose alloc_cnt is read */
  {"OFF_SESS_HTYPE",    "sessions","m_type",                  NULL, 400},
  {"OFF_CAT_HTYPE",     "catalog_Hashmap","m_type",           NULL, 400},
  {"OFF_LK_HTYPE",      "lk_Gl","m_type",                     NULL, 472},
  {"OFF_LK_RES_ALLOC",  "lk_Gl","alloc_cnt",                  NULL, 84},
  {"OFF_LK_ENTFL",      "lk_Gl","obj_free_entry_list",        NULL, 480},
  {"OFF_LFFL_ALLOC",    NULL,"alloc_cnt","lf_freelist",             12},
  {"SZ_LK_ENTRY",       NULL,NULL,"lk_entry",                       128},
  {"SZ_LK_RES",         NULL,NULL,"lk_res",                         120},
};
#define NTG ((int)(sizeof TG/sizeof TG[0]))
/* The standalone dwoff's GOT/OKF/NOTE are unused here; out[] replaces them */


/* ---- Builtin DWARF extraction API ----
   Returns the number of keys resolved (>0 on success); out[NTG] holds the values
   (-1 unresolved) and why the failure reason.  A full .debug_info scan takes
   seconds (8s / 125MB measured), so the caller should cache it. */
/* s cut to at most n bytes without splitting a UTF-8 character (ring of 4) */
static const char *u8n(const char *s,size_t n){
    static char ring[4][512]; static int k=0; char *o=ring[k++&3];
    size_t l=strlen(s); if(n>=sizeof ring[0]) n=sizeof ring[0]-1;
    if(l>n){ l=n; while(l>0 && ((unsigned char)s[l]&0xC0)==0x80) l--; }
    memcpy(o,s,l); o[l]=0; return o;
}
static int g_dw_flags=0;
static int dwoff_extract(const char*so_path,long out[],char*why,size_t wn){
    /* Reset globals in case of re-entry */
    DI=AB=DS=NULL; DI_SZ=AB_SZ=DS_SZ=0; NCU=0; NTAB=0; G_desync=0; g_dw_flags=0;
    int fd=open(so_path,O_RDONLY);
    if(fd<0){ snprintf(why,wn,"%s",g_ascii?"open failed":"open 실패"); return 0; }
    struct stat st;
    if(fstat(fd,&st)!=0 || st.st_size<(off_t)sizeof(Elf64_Ehdr)){ close(fd); snprintf(why,wn,"%s",g_ascii?"not ELF":"ELF 아님"); return 0; }
    unsigned char*m=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(m==MAP_FAILED){ snprintf(why,wn,"%s",g_ascii?"mmap failed":"mmap 실패"); return 0; }
    size_t fsz=(size_t)st.st_size;
    #define DW_IN_FILE(off_,len_) ((uint64_t)(off_) <= (uint64_t)fsz && \
                                   (uint64_t)(len_) <= (uint64_t)fsz - (uint64_t)(off_))
    int got=0;
    TAB=(ab_t*)calloc(NTAB_MAX,sizeof(ab_t));
    if(!TAB){ snprintf(why,wn,"%s",g_ascii?"table allocation failed":"TAB 할당 실패"); munmap(m,st.st_size); return 0; }
    Elf64_Ehdr*eh=(Elf64_Ehdr*)m;
    if(memcmp(eh->e_ident,ELFMAG,SELFMAG)||eh->e_ident[EI_CLASS]!=ELFCLASS64){
        snprintf(why,wn,"%s",g_ascii?"not ELF64":"ELF64 아님"); goto done; }
    G_split=0;
    { int fl=0, r=dw_sections(m,fsz,&fl);
      if(r==-1){ snprintf(why,wn,"%s",g_ascii?"section table outside the file":"섹션 표가 파일 밖"); goto done; }
      if(r==-2){ snprintf(why,wn,"%s",g_ascii?"section name table outside the file":"섹션 이름 표가 파일 밖"); goto done; }
      if(fl&DWF_COMPRESSED){ snprintf(why,wn,"%s",g_ascii?"compressed debug sections (-gz, SHF_COMPRESSED) - not supported":"압축된 디버그 섹션(-gz, SHF_COMPRESSED) — 미지원"); goto done; }
      if(!DI||!AB){
          snprintf(why,wn,"%s",(fl&DWF_ZDEBUG)?(g_ascii?"old-style compressed debug sections (.zdebug) - not supported":"구형 압축 디버그 섹션(.zdebug) — 미지원")
                              :(fl&DWF_DEBUGLINK)?(g_ascii?"debug info is in a separate file (.gnu_debuglink) - not read":"디버그 정보가 별도 파일(.gnu_debuglink) — 읽지 않음")
                              :(g_ascii?"no debug sections (stripped)":"디버그 섹션 없음(strip)"));
          goto done; }
      g_dw_flags=fl; }
    build_index();
    { const char *vs[PRE_MAX], *ts[PRE_MAX]; int nv=0, nt=0;
      for(int i=0;i<NTG;i++){ if(TG[i].sym && nv<PRE_MAX) vs[nv++]=TG[i].sym; if(TG[i].type && nt<PRE_MAX) ts[nt++]=TG[i].type; }
      prescan(vs,nv,ts,nt); }
    for(int i=0;i<NTG;i++){
        out[i]=-1;
        if(TG[i].type && TG[i].member){
            long off=-1; if(member_of_type(TG[i].type,TG[i].member,&off)==0){ out[i]=off; got++; }
        } else if(TG[i].type){
            long sz; if(sizeof_type(TG[i].type,&sz,NULL)==0){ out[i]=sz; got++; }
        } else {
            die_t d; unsigned long toff=0;
            if(resolve_symbol(TG[i].sym,&d,&toff)==0){
                long off=-1; char path[320]={0};
                if(find_member(&d,TG[i].member,0,0,&off,path,sizeof path)==0){ out[i]=off; got++; }
            }
        }
    }
    if(got==0) snprintf(why,wn,"%s",
        G_split?(g_ascii?"split DWARF (skeleton units, DIEs in .dwo) - not supported":"split DWARF(스켈레톤 유닛, DIE 는 .dwo) — 미지원")
        :(g_dw_flags&DWF_ALTLINK)?(g_ascii?"dwz-shared debug info (.gnu_debugaltlink) - not supported":"dwz 공유 디버그 정보(.gnu_debugaltlink) — 미지원")
        :(g_dw_flags&DWF_STROFFS)?(g_ascii?"DWARF5 string indices (strx, .debug_str_offsets) - not supported":"DWARF5 문자열 색인(strx, .debug_str_offsets) — 미지원")
        :(g_ascii?"no key resolved":"키 0개 해소"));
done:
    free(TAB); TAB=NULL; NTAB=0; prescan_free();
    if(CU){ free(CU); CU=NULL; NCU=0; }   /* The CU index has the same lifetime */
    #undef DW_IN_FILE
    munmap(m,st.st_size);
    return got;
}
/* TG[] indices; the order fills the table, so it is fixed by this enum */
enum { DW_PGBUF=0, DW_XC_ENT, DW_XC_USE, DW_XC_CLN, DW_QLIST, DW_QPOOL,
       DW_LK_NTRAN, DW_LF, DW_LF2,
       DW_SZ_SESS, DW_SZ_CAT, DW_SZ_FP, DW_SZ_LK, DW_SZ_CONN, DW_SZ_VAC,
       /* pgbuf/log, one to one with the tail of TG[] */
       DW_BCBTAB, DW_SZ_BCB, DW_BCB_VPID, DW_BCB_FLAGS, DW_BCB_OLSA, DW_BCB_IOPG,
       DW_IOBUF_IOPG, DW_LOG_NXIO, DW_LOG_APPEND, DW_LOG_EOF,
       DW_PS_NVALS, DW_PS_GSTATS, DW_PS_INIT, DW_PS_SZMD, DW_PS_MDPSID, DW_PS_MDSTART,
       DW_AG_LRULIST, DW_AG_NLRU, DW_AG_QUOTA, DW_AG_NPRIV, DW_AG_MONITOR, DW_AG_DIRTIES,
       DW_AG_SZLRU, DW_AG_C1, DW_AG_C2, DW_AG_C3, DW_AG_VICT,
       DW_HX_SESS, DW_HX_CAT, DW_HX_LK, DW_LK_RES, DW_LK_ENTFL, DW_LFFL, DW_SZ_LKE, DW_SZ_LKR, DW_N };

/* Extraction takes 1-3 seconds over 100-200MB of .debug_info (one pass for every key, then
   the CUs that matter).  Its result is kept in memory for the run only - no file is
   written anywhere (no /var/tmp cache: a full, read-only or quota-bound temp directory
   cannot break it, and nothing is left behind).  A library seen again in the same run
   (an instance switch back) is served from this memo by build-id.  To carry offsets to a
   host without DWARF, a table file (dwoff --emit-tbl) is placed in offsets/. */
/* The .so's GNU build-id as hex ("" without one).  size+mtime collides across cp -p, tar and
   SOURCE_DATE_EPOCH builds; the build-id names the build itself. */
static void elf_build_id(const char *so,char *hex,size_t hn){
    hex[0]=0;
    int fd=open(so,O_RDONLY|O_CLOEXEC); if(fd<0) return;
    Elf64_Ehdr eh; struct stat st;
    if(fstat(fd,&st)!=0 || pread(fd,&eh,sizeof eh,0)!=(ssize_t)sizeof eh || memcmp(eh.e_ident,ELFMAG,SELFMAG)
       || eh.e_ident[EI_CLASS]!=ELFCLASS64 || eh.e_shentsize!=sizeof(Elf64_Shdr) || eh.e_shnum==0 || eh.e_shnum>4096
       || eh.e_shoff>(uint64_t)st.st_size){ close(fd); return; }
    Elf64_Shdr *sh=malloc((size_t)eh.e_shnum*sizeof *sh);
    if(sh && pread(fd,sh,(size_t)eh.e_shnum*sizeof *sh,(off_t)eh.e_shoff)==(ssize_t)(eh.e_shnum*sizeof *sh)){
        for(int i=0;i<eh.e_shnum && !hex[0];i++){
            if(sh[i].sh_type!=SHT_NOTE || sh[i].sh_size<16 || sh[i].sh_size>4096) continue;
            unsigned char nb[4096];
            if(pread(fd,nb,(size_t)sh[i].sh_size,(off_t)sh[i].sh_offset)!=(ssize_t)sh[i].sh_size) continue;
            size_t o=0;
            while(o+12<=sh[i].sh_size){
                uint32_t nsz,dsz,typ; memcpy(&nsz,nb+o,4); memcpy(&dsz,nb+o+4,4); memcpy(&typ,nb+o+8,4);
                size_t no=o+12, doff=no+((nsz+3u)&~3u);
                if(doff>sh[i].sh_size || dsz>sh[i].sh_size-doff) break;
                if(typ==3 && nsz==4 && !memcmp(nb+no,"GNU",4) && dsz>0 && dsz*2<hn){   /* NT_GNU_BUILD_ID */
                    for(uint32_t k=0;k<dsz;k++) snprintf(hex+2*k,3,"%02x",nb[doff+k]);
                    break; }
                o=doff+((dsz+3u)&~3u);
            }
        }
    }
    free(sh); close(fd);
}
static int offs_text_load(const char*txt,long out[],int ssz[]);   /* the table parser, below */
/* The identity of a library's build: its build-id, else path, size and mtime */
static void dw_lib_key(const char*so,char*key,size_t kn){
    char bid[132]; elf_build_id(so,bid,sizeof bid);
    if(bid[0]){ snprintf(key,kn,"bid:%s",bid); return; }
    struct stat st; if(stat(so,&st)!=0){ key[0]=0; return; }
    snprintf(key,kn,"f:%s:%lld:%lld",so,(long long)st.st_size,(long long)st.st_mtime);
}
#define DW_MEMO_N 4
static struct { char key[160]; long v[DW_N]; } g_dw_memo[DW_MEMO_N]; static int g_dw_memo_next=0;
static int dw_memo_get(const char*key,long out[]){
    if(!key[0]) return 0;
    for(int i=0;i<DW_MEMO_N;i++) if(!strcmp(g_dw_memo[i].key,key)){ memcpy(out,g_dw_memo[i].v,sizeof(long)*DW_N); return 1; }
    return 0;
}
static void dw_memo_put(const char*key,const long v[]){
    if(!key[0]) return;
    int i=g_dw_memo_next; g_dw_memo_next=(g_dw_memo_next+1)%DW_MEMO_N;
    snprintf(g_dw_memo[i].key,sizeof g_dw_memo[i].key,"%s",key); memcpy(g_dw_memo[i].v,v,sizeof(long)*DW_N);
}
/* The child's answer on its pipe: "OK" and one "key value" line per key, or a reason */
static void dw_pipe_write(int fd,const long o[]){
    char buf[8192]; int n=snprintf(buf,sizeof buf,"OK\n");
    for(int i=0;i<NTG && n<(int)sizeof buf-80;i++) n+=snprintf(buf+n,sizeof buf-(size_t)n,"%s %ld\n",TG[i].key,o[i]);
    for(int w=0; w<n; ){ ssize_t r=write(fd,buf+w,(size_t)(n-w)); if(r<=0) break; w+=(int)r; }
}
/* Live, the extraction (a few seconds of CPU on a 100-200MB .so) runs in a niced child that
   hands the offsets back on a pipe; frames go on meanwhile and take them when it is done.
   One-shot modes wait, as they report this very frame. */
static pid_t g_dw_kid=0; static int g_dw_kfd=-1; static char g_dw_kso[PATH_MAX]; static char g_dw_kkey[160];
static void dw_kid_stop(void){
    if(g_dw_kid>0){ kill(g_dw_kid,SIGKILL); waitpid(g_dw_kid,NULL,0); g_dw_kid=0; }
    if(g_dw_kfd>=0){ close(g_dw_kfd); g_dw_kfd=-1; }   /* its pipe too, or each switch leaves one open */
}
/* The child has finished and waits to be collected (not collected here: dw_load_or_extract
   reads its reason and reaps it).  A frame that adopted another build's table meanwhile
   asks this to go back through the ladder, where this build's own offsets now win. */
static int dw_kid_done(void){
    if(g_dw_kid<=0) return 0;
    siginfo_t si; si.si_pid=0;
    return waitid(P_PID,(id_t)g_dw_kid,&si,WEXITED|WNOHANG|WNOWAIT)==0 && si.si_pid==g_dw_kid;
}
/* Asked each frame before the cached table is reused.  1 = the extraction of THIS library
   has finished: drop the cache so the ladder runs once more and its own offsets win.  A
   finished child of another library (started before an instance switch) is no reason to
   re-run the ladder - and would be every frame, since only dw_load_or_extract for that
   library reaps it: it is reaped here instead, its answer kept for a switch back. */
/* Collect a finished child: its answer goes to the memo under the library it was started
   for (whichever instance is on screen now).  Returns 1 with the offsets in out, else 0
   with the reason in why. */
static int dw_kid_harvest(long out[],char *why,size_t wn){
    char buf[8192]; size_t n=0; ssize_t r;
    while(n<sizeof buf-1 && (r=read(g_dw_kfd,buf+n,sizeof buf-1-n))>0) n+=(size_t)r;
    buf[n]=0;
    int st=0; waitpid(g_dw_kid,&st,0);
    close(g_dw_kfd); g_dw_kfd=-1; g_dw_kid=0;
    if(!strncmp(buf,"OK\n",3)){
        long v[DW_N];
        if(offs_text_load(buf+3,v,NULL)>0){ dw_memo_put(g_dw_kkey,v); if(out) memcpy(out,v,sizeof v); return 1; }
        snprintf(why,wn,"%s",g_ascii?"extraction gave no offsets":"추출 결과에 오프셋 없음"); return 0; }
    snprintf(why,wn,"%.150s",buf[0]?buf:(g_ascii?"extraction failed":"추출 실패"));   /* the child writes at most 160 */
    return 0;
}
static int dw_kid_finished_for(const char *so){
    if(!dw_kid_done()) return 0;
    if(!strcmp(g_dw_kso,so)) return 1;
    char w[160]; (void)dw_kid_harvest(NULL,w,sizeof w);   /* another library's: kept for a switch back */
    return 0;
}
/* 1 = offsets in out, 2 = failed (why), 0 = extracting in the background */
static int dw_load_or_extract(const char*so,long out[],char*why,size_t wn){
    char key[160]; dw_lib_key(so,key,sizeof key);
    if(!key[0]){ snprintf(why,wn,"%s",g_ascii?"stat failed":"stat 실패"); return 2; }
    /* A finished child is collected first: whichever library it ran for, its answer is kept */
    if(g_dw_kid>0 && dw_kid_done()){
        int mine=!strcmp(g_dw_kso,so); char kw[160]="";
        int ok=dw_kid_harvest(mine?out:NULL,kw,sizeof kw);
        if(mine){ if(ok) return 1; snprintf(why,wn,"%s",kw); return 2; }
    }
    if(dw_memo_get(key,out)) return 1;
    if(g_pd_async){
        if(g_dw_kid>0){                                   /* still running */
            if(strcmp(g_dw_kso,so)){ dw_kid_stop(); }   /* another instance's library now: start over */
            else { snprintf(why,wn,"%s",g_ascii?"extracting offsets from DWARF in the background":"DWARF 에서 오프셋 추출 중(백그라운드)"); return 0; }
        }
        int pf[2]; if(pipe2(pf,O_CLOEXEC)!=0){ snprintf(why,wn,"pipe: %s",strerror(errno)); return 2; }
        sigset_t held; kids_hold(&held);
        pid_t k=fork();
        if(k<0){ int e=errno; sigprocmask(SIG_SETMASK,&held,NULL);
                 close(pf[0]); close(pf[1]); snprintf(why,wn,"fork: %s",strerror(e)); return 2; }
        if(k==0){
            child_forget_parent(); sigprocmask(SIG_SETMASK,&held,NULL);
            close(pf[0]); if(setpriority(PRIO_PROCESS,0,10)){}
            long o[DW_N]; char w[160]="";
            /* the answer goes back on the pipe: no file, so no temp directory can fail it */
            if(dwoff_extract(so,o,w,sizeof w)>0){ dw_pipe_write(pf[1],o); _exit(0); }
            if(write(pf[1],w,strlen(w))<0){}
            _exit(2);
        }
        close(pf[1]); g_dw_kfd=pf[0]; g_dw_kid=k;
        snprintf(g_dw_kso,sizeof g_dw_kso,"%s",so); snprintf(g_dw_kkey,sizeof g_dw_kkey,"%s",key);
        sigprocmask(SIG_SETMASK,&held,NULL);
        { static int reg=0; if(!reg){ atexit(dw_kid_stop); reg=1; } }
        snprintf(why,wn,"%s",g_ascii?"extracting offsets from DWARF in the background":"DWARF 에서 오프셋 추출 중(백그라운드)");
        return 0;
    }
    fprintf(stderr,g_ascii?"cub_top: no offset table fits; extracting from the DWARF of %s (a few seconds, every run - a table in offsets/ avoids it)\n"
                          :"cub_top: 오프셋 표가 이 환경과 안 맞아 %s 의 DWARF 에서 직접 추출합니다 (실행마다 수 초 — offsets/ 에 표를 두면 생략)\n",so);
    if(dwoff_extract(so,out,why,wn)<=0) return 2;
    dw_memo_put(key,out);
    return 1;
}
/* Anchor probing - last resort on a stripped build with no table and no DWARF.
   Symbol addresses come from .symtab; the field offset is fixed by the one
   position where a paramdump-known value appears exactly once.  Non-unique
   matches are rejected.  Item sizes cannot be probed, so those become
   "count only". */
#define PROBE_WIN 64
static int probe_i32(int pid,unsigned long base,long lo,long hi,int *out_off,int32_t *out_v){
    /* If exactly one int32 in [lo,hi] lies in the window, that offset is fixed */
    int hits=0,off=-1; int32_t val=0;
    for(int o=0;o<PROBE_WIN;o+=4){
        int32_t v=rd_i32(pid,base+o);
        if(v>=lo && v<=hi){ hits++; off=o; val=v; }
    }
    if(hits!=1) return 0;
    *out_off=off; *out_v=val; return 1;
}
/* ======= End of the builtin DWARF engine ======= */

/* CUBRID version detection: the mapped .so name suffix, else a "1X.Y.Z.build"
   string in its rodata.  This only picks a candidate table - adoption still
   requires the runtime paramdump cross-check. */
static char g_cver[24]="";           /* "11.5" or "11.5.0.2374" */
static void detect_cubrid_ver(const char *so_path){
    g_cver[0]=0;
    const char *p=strstr(so_path,".so.");
    if(p && ((p[4]>='1'&&p[4]<='9'))){
        snprintf(g_cver,sizeof g_cver,"%s",p+4);          /* "11.5" */
    }
    /* Promote to the full build string (e.g. 11.5.0.2374) found in .rodata, which sits past
       18MB in debug builds; streamed in 256KB chunks.  No section headers: first 16MB. */
    FILE *fp=fopen(so_path,"rb");
    if(fp){
        static char buf[256*1024+32];
        long start=0, limit=16L*1024*1024, done=0; size_t carry=0;
        { Elf64_Ehdr eh;
          if(fread(&eh,sizeof eh,1,fp)==1 && !memcmp(eh.e_ident,ELFMAG,SELFMAG) && eh.e_ident[EI_CLASS]==ELFCLASS64
             && eh.e_shentsize==sizeof(Elf64_Shdr) && eh.e_shnum>0 && eh.e_shnum<4096 && eh.e_shstrndx<eh.e_shnum){
              static Elf64_Shdr shs[4096];
              if(fseek(fp,(long)eh.e_shoff,SEEK_SET)==0 && fread(shs,sizeof(Elf64_Shdr),eh.e_shnum,fp)==eh.e_shnum){
                  Elf64_Shdr *ns=&shs[eh.e_shstrndx];
                  char names[8192]; size_t nl=ns->sh_size<sizeof names-1?(size_t)ns->sh_size:sizeof names-1;
                  if(fseek(fp,(long)ns->sh_offset,SEEK_SET)==0 && fread(names,1,nl,fp)==nl){
                      names[nl]=0;
                      for(int i=0;i<eh.e_shnum;i++)
                          if(shs[i].sh_name<nl && !strcmp(names+shs[i].sh_name,".rodata")){
                              start=(long)shs[i].sh_offset; limit=(long)shs[i].sh_size; break; }
                  }
              }
          }
          if(fseek(fp,start,SEEK_SET)!=0){ start=0; limit=16L*1024*1024; rewind(fp); } }
        while(done<limit){
            size_t want_=sizeof buf-32-carry;
            if((long)want_>limit-done) want_=(size_t)(limit-done);
            size_t got=fread(buf+carry,1,want_,fp);
            if(got==0) break;
            size_t total=carry+got;
            for(size_t i=0;i+8<total;i++){
                if(buf[i]=='1' && (buf[i+1]=='0'||buf[i+1]=='1') && buf[i+2]=='.'
                   && buf[i+3]>='0'&&buf[i+3]<='9'){
                    size_t j=i,dots=0;
                    while(j<total && j<i+20 &&
                          ((buf[j]>='0'&&buf[j]<='9')||buf[j]=='.')){ if(buf[j]=='.')dots++; j++; }
                    if(dots>=3 && j<total && buf[j]==0){
                        snprintf(g_cver,sizeof g_cver,"%.*s",(int)(j-i),buf+i);
                        fclose(fp); return;
                    }
                    i=j;
                }
            }
            /* Carry 20 trailing bytes across the boundary */
            carry = total>20 ? 20 : total;
            memmove(buf,buf+total-carry,carry);
            done+=(long)got;
        }
        fclose(fp);
    }
}

/* Library load base = the first mapping with file offset 0, not the r-xp
   segment (whose file offset is non-zero).  Symbol st_value is relative to the
   file virtual address, so a wrong base shifts every address and disables
   method B.  Fall back to the file's lowest start. */
/* One /proc/<pid>/maps line: range, offset, inode and the path - the rest of the line, which
   may hold spaces; a trailing " (deleted)" is removed and reported.  0 if not a mapping line. */
static int maps_line(const char *line,unsigned long *s,unsigned long *e,unsigned long *off,
                     unsigned long *ino,char *path,size_t pn,int *deleted){
    char perm[8]; unsigned mj,mn; *deleted=0; path[0]=0;
    if(sscanf(line,"%lx-%lx %7s %lx %x:%x %lu",s,e,perm,off,&mj,&mn,ino)<7) return 0;
    const char *q=line; int fld=0;
    while(*q && fld<5){ while(*q==' ')q++; while(*q && *q!=' ')q++; fld++; }
    while(*q==' ') q++;
    size_t n=0; while(*q && *q!='\n' && n+1<pn) path[n++]=*q++;
    path[n]=0;
    if(n>10 && !strcmp(path+n-10," (deleted)")){ path[n-10]=0; *deleted=1; }
    return 1;
}
/* Why lib_base gave no path to read: the library was replaced and the running bytes
   (map_files) cannot be opened - a 3.10 kernel opens map_files for root only */
static char g_lib_why[200]="";
static unsigned long lib_base(int pid,const char *needle,char *path,size_t plen){
    char f[64]; snprintf(f,sizeof f,"/proc/%d/maps",pid);
    g_lib_why[0]=0;
    FILE *fp=fopen(f,"r"); if(!fp) return 0;
    char line[PATH_MAX+256]; unsigned long base=0, first=0, s0=0,e0=0, ino0=0; int del0=0;
    char pth0[PATH_MAX]={0};
    while(fgets(line,sizeof line,fp)){
        if(!strstr(line,needle)) continue;
        unsigned long s,e=0,off=0,ino=0; int del=0; char pth[PATH_MAX];
        if(!maps_line(line,&s,&e,&off,&ino,pth,sizeof pth,&del)) continue;
        if(!strstr(pth,needle)) continue;                 /* the name in the path, not elsewhere on the line */
        if(!first || s<first){ first=s; }                 /* Lowest start address (fallback) */
        if(off==0 && !base){ base=s; s0=s; e0=e; ino0=ino; del0=del; snprintf(pth0,sizeof pth0,"%s",pth); }
        if(!s0){ s0=s; e0=e; ino0=ino; del0=del; snprintf(pth0,sizeof pth0,"%s",pth); }   /* Path and map_files candidates */
    }
    fclose(fp);
    if(!base) base=first;
    if(base && path){
        /* Read symbols from the bytes the server is running: map_files/<range> opens that inode
           even after a rebuild.  Without it the disk file is used only if it still is that inode;
           a replaced library would give another build's symbols the gates cannot tell apart. */
        char mf[96]; snprintf(mf,sizeof mf,"/proc/%d/map_files/%lx-%lx",pid,s0,e0);
        int tfd=open(mf,O_RDONLY|O_CLOEXEC);
        struct stat ps;
        if(tfd>=0){ close(tfd); snprintf(path,plen,"%s",mf); }
        else if(del0 || stat(pth0,&ps)!=0 || (unsigned long)ps.st_ino!=ino0){
            path[0]=0;
            snprintf(g_lib_why,sizeof g_lib_why,"%s",g_ascii
                ?"libcubrid.so was replaced on disk and the running copy (map_files) cannot be opened - method B refused"
                :"libcubrid.so 가 디스크에서 교체됐고 실행 중인 사본(map_files)을 열 수 없음 — 방법 B 거부");
        }
        else snprintf(path,plen,"%s",pth0);
    }
    return base;
}
static int g_sym_symtab=-1;  /* -1 not looked, 0 none, 1 found: the last lookup found a .symtab (strip -s removes it, and the LOCAL roots with it) */
/* ELF symbol lookup.  The input file is not trusted: comm can be faked with
   prctl, so any process could map a crafted ELF.  Every pointer derived from
   header offset/count/size fields is checked against [m, m+size) before it is
   dereferenced. */
/* out[j] = st_value of names[j]; osz[j] (when given) = its st_size, the size of the object */
static int sym_lookup(const char *so,const char **names,unsigned long *out,int n,unsigned long *osz){
    int fd=open(so,O_RDONLY); if(fd<0) return -1;
    struct stat st; if(fstat(fd,&st)<0){ close(fd); return -1; }
    if(st.st_size<(off_t)sizeof(Elf64_Ehdr)){ close(fd); return -1; }
    size_t sz=(size_t)st.st_size;
    unsigned char *m=(unsigned char*)mmap(NULL,sz,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd); if(m==MAP_FAILED) return -1;
    int found=0;
    /* Does [off, off+len) lie entirely within the file (overflow checked) */
    #define IN_FILE(off_,len_) ((uint64_t)(off_) <= (uint64_t)sz && \
                                (uint64_t)(len_) <= (uint64_t)sz - (uint64_t)(off_))
    Elf64_Ehdr *eh=(Elf64_Ehdr*)m;
    if(memcmp(eh->e_ident,ELFMAG,SELFMAG) || eh->e_ident[EI_CLASS]!=ELFCLASS64){
        munmap(m,sz); return -1;   /* Guard against misreading ELFCLASS32; the structs assume Elf64 */
    }
    if(eh->e_shentsize!=sizeof(Elf64_Shdr) || eh->e_shnum==0 ||
       !IN_FILE(eh->e_shoff,(uint64_t)eh->e_shnum*sizeof(Elf64_Shdr))){
        munmap(m,sz); return -1;                       /* Section header table outside the file */
    }
    Elf64_Shdr *sh=(Elf64_Shdr*)(m+eh->e_shoff);
    /* An exact name wins; "name.suffix" (-flto's .lto_priv.N, a clone's .constprop.N) is taken
       only when it is the one such object - two of them are two objects and neither is used */
    int exact[MAXSYM]={0}, nsuf[MAXSYM]={0}; unsigned long sufv[MAXSYM]={0}, sufs[MAXSYM]={0};
    g_sym_symtab=0;
    for(int i=0;i<eh->e_shnum;i++) if(sh[i].sh_type==SHT_SYMTAB) g_sym_symtab=1;
    for(int i=0;i<eh->e_shnum && found<n;i++){
        if(sh[i].sh_type!=SHT_SYMTAB && sh[i].sh_type!=SHT_DYNSYM) continue;
        if(sh[i].sh_entsize!=sizeof(Elf64_Sym)) continue;          /* Guard against division by zero and misreading */
        if(!IN_FILE(sh[i].sh_offset,sh[i].sh_size)) continue;      /* Symbol table outside the file */
        if(sh[i].sh_link>=eh->e_shnum) continue;                   /* Invalid string section index */
        Elf64_Shdr *strsh=&sh[sh[i].sh_link];
        if(!IN_FILE(strsh->sh_offset,strsh->sh_size)) continue;    /* String table outside the file */
        Elf64_Sym *sy=(Elf64_Sym*)(m+sh[i].sh_offset);
        const char *str=(const char*)(m+strsh->sh_offset);
        const char *strend=str+strsh->sh_size;                     /* The section end, not the file end */
        long cnt=(long)(sh[i].sh_size/sizeof(Elf64_Sym));
        /* Four-byte prefilter: the first character alone is useless (65% of symbols start
           with '_'), while _ZL (local static) and _ZN (namespace) diverge at byte 4. */
        uint32_t want[MAXSYM];
        for(int j=0;j<n;j++) memcpy(&want[j],names[j],4);
        for(long k=0;k<cnt && found<n;k++){
            if(sy[k].st_name>=strsh->sh_size) continue;            /* Name offset outside the table */
            const char *nm=str+sy[k].st_name;
            if(!*nm || nm+4>strend) continue;
            /* Confirm the string is NUL-terminated, so strcmp cannot run past the section */
            if(!memchr(nm,0,(size_t)(strend-nm))) continue;
            uint32_t nm4; memcpy(&nm4,nm,4);
            if(sy[k].st_shndx==SHN_UNDEF || ELF64_ST_TYPE(sy[k].st_info)!=STT_OBJECT) continue;   /* a reference, a function */
            for(int j=0;j<n;j++){
                if(exact[j] || nm4!=want[j]) continue;
                size_t L=strlen(names[j]);
                if(strncmp(nm,names[j],L)) continue;
                if(!nm[L]){ out[j]=sy[k].st_value; if(osz) osz[j]=sy[k].st_size; exact[j]=1; found++; break; }
                if(nm[L]=='.' && (nsuf[j]==0 || sufv[j]!=sy[k].st_value)){ nsuf[j]++; sufv[j]=sy[k].st_value; sufs[j]=sy[k].st_size; break; }
            }
        }
    }
    #undef IN_FILE
    for(int j=0;j<n;j++) if(!exact[j] && nsuf[j]==1 && !out[j]){ out[j]=sufv[j]; if(osz) osz[j]=sufs[j]; found++; }
    munmap(m,sz); return found;
}

/* Method B: walk the root globals.  Offsets come from a table extracted from
 * DWARF.  Self-check: pgbuf_Pool.num_buffers must equal paramdump's
 * data_buffer_pages, else method B is disabled. */
enum { S_PGBUF, S_XCACHE, S_QLIST, S_QPOOL, S_FPCNT, S_LKGL, S_SESS, S_CAT,
       S_CONNMAX, S_CONNACT, S_VACUUM, S_LOGGL, S_PSTATG, S_PSTATMD, S_NSYM };
static const char *SYMN[S_NSYM]={
    "_ZL10pgbuf_Pool","xcache_Global","_ZL16qfile_List_cache",
    "_ZL27qfile_List_cache_entry_pool","_ZL21fpcache_Entry_counter",
    "lk_Gl","_ZL8sessions","_ZL15catalog_Hashmap",
    "_ZL16css_Num_max_conn","_ZL19css_Num_active_conn","_ZL11vacuum_Data",
    "log_Gl",   /* extern "C" globals: unmangled */
    "pstat_Global","pstat_Metadata"   /* Engine statistics (capacity axis); both are non-static globals */
};
static const char *SYMN10[S_NSYM]={
    /* 10.x (C build, unmangled).  Eight of these exist in the 10.0 symtab;
       xcache_Global and fpcache_Entry_counter arrived in 10.2, and catalog_Hashmap
       is a different structure (catalog_Hash_table) in 10.x.  A missing name blanks
       only its own item - the pgbuf, lk and conn anchors survive. */
    "pgbuf_Pool","xcache_Global","qfile_List_cache",
    "qfile_List_cache_entry_pool","fpcache_Entry_counter",
    "lk_Gl","sessions","catalog_Hashmap",
    "css_Num_max_conn","css_Num_active_conn","vacuum_Data",
    "log_Gl",
    "pstat_Global","pstat_Metadata"
};

/* DWARF-derived offsets and sizes (the 11.5 golden set; check-offsets.sh compares it with rules-11.5.h) */
#define OFF_XCACHE_ENTRIES 440
#define OFF_XCACHE_USAGE   484
#define OFF_XCACHE_CLONE   488
#define OFF_QLIST_NPAGES    28
#define OFF_QPOOL_NENT       8
#define OFF_LF_ALLOCCNT     12
#define SZ_LK_TRAN_LOCK    152
#define SZ_SESSION_STATE   312
#define SZ_CATALOG_ENTRY    48
#define SZ_FPCACHE_ENT     120
#define SZ_CSS_CONN_ENTRY  480
#define SZ_VACUUM_DATA     184

/* Version tables are candidates only.  Adoption requires the .so symtab lookup
   plus the paramdump cross-check in that environment, so every candidate is
   tried regardless of the detected version. */
typedef struct {
    const char *tag;                 /* Table name (its source), shown on screen and in terse */
    const char *ver;                 /* The build it was extracted from; "exact" only on equality */
    const char **syms;               /* S_NSYM symbol names */
    int off_pgbuf_nbuf;
    int off_xc_entries,off_xc_usage,off_xc_clone;
    int off_qlist_npages,off_qpool_nent,off_lf_alloccnt;
    int sz_lk_tran,sz_session,sz_catalog,sz_fpcache,sz_conn,sz_vacuum;   /* -1 means the size is unknown */
    /* Offset of num_trans within lk_Gl.  The structure differs by release, so a fixed
       +0 could read garbage that happens to be in range (10.2 is at +456).
       Keep this at the END of the struct: BTABS[] uses positional literals, so an
       insertion in the middle silently shifts every field. */
    int off_lk_ntran;
    /* ---- BCB direct read offsets; all -1 disables the pgbuf section.  Append at the struct end only ---- */
    struct { int bcbtab, sz_bcb, vpid, flags, olsa, iopg, iobuf_iopg, log_nxio, log_append, log_eof; } pg;
    /* perfmon offsets; -1 disables only the statistics direct read, leaving the rest */
    struct { int nvals, gstats, init, sz_md, md_psid, md_start; } ps;
    /* st_size of each root symbol (SYMN order) in that build; 0 = not recorded.  The gate
       compares them with the running .so: a struct that changed size is another layout. */
    int ssz[S_NSYM];
    /* Engine buffer pool aggregates; -1 disables only them.  Append at the struct end only */
    struct { int lrulist, nlru, quota, npriv, monitor, dirties, sz_lru, c1, c2, c3, vict; } ag;
    /* catalog alloc_cnt; hash map kind (lockfree_hashmap::m_type, -1 = no wrapper: the old lf
       hash); object lock pools.  Append at the struct end only */
    struct { int lf2, sess_ht, cat_ht, lk_ht, lk_res, lk_entfl, lffl, sz_lke, sz_lkr; } hx;
} btab_t;
static const btab_t BTABS[]={
    {"table-11.5","11.5.0.2374",SYMN,0,
     OFF_XCACHE_ENTRIES,OFF_XCACHE_USAGE,OFF_XCACHE_CLONE,
     OFF_QLIST_NPAGES,OFF_QPOOL_NENT,OFF_LF_ALLOCCNT,
     SZ_LK_TRAN_LOCK,SZ_SESSION_STATE,SZ_CATALOG_ENTRY,
     SZ_FPCACHE_ENT,SZ_CSS_CONN_ENTRY,SZ_VACUUM_DATA,
     0 /* off_lk_ntran: on 11.x num_trans is at the head of lk_Gl */,
     {8,144,44,64,128,136,8,48,280,488} /* pg, measured with dwoff on 11.5.0.2374 */,
     {0,8,44,56,0,20} /* ps, measured with dwoff on 11.5.0.2374 */,
     {648,608,48,16,4,640,416,408,4,4,184,1360,56,16968} /* ssz, readelf -s of 11.5.0.2374 */,
     {56,40,328,0,280,0,128,80,84,88,92} /* ag, measured with dwoff on 11.5.0.2374 */,
     {12,400,400,472,84,480,12,128,120} /* hx, measured with dwoff on 11.5.0.2374 */},
};
/* DWARF or offset-file values into the pg group.  If any is missing all become -1 - no partial trust. */
static void ps_from_dw(btab_t*bt,const long dw[]){
    long v[6]={dw[DW_PS_NVALS],dw[DW_PS_GSTATS],dw[DW_PS_INIT],dw[DW_PS_SZMD],dw[DW_PS_MDPSID],dw[DW_PS_MDSTART]};
    int ok=1; for(int i=0;i<6;i++) if(v[i]<0) ok=0;
    int *f=(int*)&bt->ps;
    for(int i=0;i<6;i++) f[i]= ok ? (int)v[i] : -1;
}
static void hx_from_dw(btab_t*bt,const long dw[]){
    int *f=(int*)&bt->hx; f[0]=(int)dw[DW_LF2];
    for(int i=DW_HX_SESS;i<=DW_SZ_LKR;i++) f[1+i-DW_HX_SESS]=(int)dw[i];   /* each -1 when absent */
}
static void ag_from_dw(btab_t*bt,const long dw[]){
    int ok=1; for(int i=DW_AG_LRULIST;i<=DW_AG_VICT;i++) if(dw[i]<0) ok=0;
    int *f=(int*)&bt->ag;
    for(int i=DW_AG_LRULIST;i<=DW_AG_VICT;i++) f[i-DW_AG_LRULIST]= ok ? (int)dw[i] : -1;
}
static void pg_from_dw(btab_t*bt,const long dw[]){
    long v[10]={dw[DW_BCBTAB],dw[DW_SZ_BCB],dw[DW_BCB_VPID],dw[DW_BCB_FLAGS],dw[DW_BCB_OLSA],
                dw[DW_BCB_IOPG],dw[DW_IOBUF_IOPG],dw[DW_LOG_NXIO],dw[DW_LOG_APPEND],dw[DW_LOG_EOF]};
    int ok=1; for(int i=0;i<10;i++) if(v[i]<0) ok=0;
    int *f=(int*)&bt->pg;
    for(int i=0;i<10;i++) f[i]= ok ? (int)v[i] : -1;
}
#define NBTABS ((int)(sizeof BTABS/sizeof BTABS[0]))
static const btab_t *g_bt=&BTABS[0];   /* Set by method_b to the candidate that passed validation */
static char g_offs_file[256]="";       /* --offsets <file>: the output of dwoff --emit-tbl */
/* Bumped when the mapped libcubrid.so changes (another instance or a restart on a
   different install): the version and every table chosen for it belong to that file. */
static unsigned g_lib_gen=0;
static char g_so_path[512]="";          /* The lib path symbols and DWARF were read from, for diagnostics and regression */

/* Offset file loader, the same key=val format as the dw cache (plus #meta comments).
   The metadata is informational: extracting on another machine is the point, so a
   (size,mtime) mismatch does not block - the runtime gates decide either way. */
#include "offsets_embedded.h"

/* Parse the table text.  Files and the built-in tables share this one parser, so an
   embedded table cannot be read differently from the same table on disk. */
/* SYMSZ_<name> <n>: st_size of a root symbol (name as in SYMN10) in the table's build */
static int offs_text_load(const char*txt,long out[],int ssz[]){
    for(int i=0;i<DW_N;i++) out[i]=-1;
    if(ssz) for(int i=0;i<S_NSYM;i++) ssz[i]=0;
    char k[64]; long v; int got=0; char line[256];
    const char *p=txt;
    while(*p){
        const char *e=strchr(p,'\n'); size_t n=e?(size_t)(e-p):strlen(p);
        if(n>=sizeof line) n=sizeof line-1;
        memcpy(line,p,n); line[n]=0;
        p = e ? e+1 : p+strlen(p);
        if(line[0]=='#') continue;
        if(sscanf(line,"%63s %ld",k,&v)!=2) continue;
        if(!strncmp(k,"SYMSZ_",6)){
            for(int i=0;i<S_NSYM;i++) if(ssz && !strcmp(k+6,SYMN10[i]) && v>0){ ssz[i]=(int)v; break; }
            continue; }
        for(int i=0;i<NTG;i++)
            if(!strcmp(k,TG[i].key)){ out[i]=v; got++; break; }
    }
    return got;
}
static int offs_file_load(const char*path,long out[],int ssz[]){
    FILE*f=fopen(path,"r"); if(!f) return 0;
    long sz; char *buf;
    fseek(f,0,SEEK_END); sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0 || sz>(1<<20)){ fclose(f); return 0; }
    buf=malloc((size_t)sz+1);
    if(!buf){ fclose(f); return 0; }
    size_t rd=fread(buf,1,(size_t)sz,f); buf[rd]=0;
    fclose(f);
    int got=offs_text_load(buf,out,ssz);
    free(buf);
    return got;
}

/* The table file's #meta version= value, used to pick a matching table from a
   directory.  Only the first few lines are read (dwoff writes the meta at the top). */
static int offs_tbl_version(const char*path,char*ver,size_t vn){
    FILE*f=fopen(path,"r"); if(!f) return 0;
    char line[256]; int ok=0;
    for(int i=0;i<10 && fgets(line,sizeof line,f);i++)
        if(sscanf(line,"#meta version=%31s",ver)==1 && (size_t)strlen(ver)<vn){ ok=1; break; }
    fclose(f); return ok;
}
/* Matching component count between two version strings: "11.4.5.1865" against
   "11.4.4.1832" gives 2.  Below major.minor there is no basis for sharing a table. */
static int ver_match_comp(const char*a,const char*b){
    int n=0;
    for(;;){
        const char*da=strchr(a,'.'), *db=strchr(b,'.');
        size_t la=da?(size_t)(da-a):strlen(a), lb=db?(size_t)(db-b):strlen(b);
        if(la==0||lb==0||la!=lb||strncmp(a,b,la)) return n;
        n++;
        if(!da||!db) return n;
        a=da+1; b=db+1;
    }
}

/* sym_lookup of the same .so with the same name set returns the same answer: the ladder
   tries a dozen tables against two name sets, so the ELF is read twice, not per table */
static int sym_lookup_memo(const char *so,const char **names,unsigned long *out,int n,unsigned long *osz){
    static char m_so[512]; static long long m_mt=-3; static const char **m_nm[2]; static int m_ret[2], m_n[2];
    static unsigned long m_out[2][S_NSYM], m_sz[2][S_NSYM];
    struct stat st; long long mt = stat(so,&st)==0 ? (long long)st.st_mtime : -2;
    if(n>S_NSYM) return sym_lookup(so,names,out,n,osz);
    if(strcmp(m_so,so) || m_mt!=mt){ snprintf(m_so,sizeof m_so,"%s",so); m_mt=mt; m_nm[0]=m_nm[1]=NULL; }
    for(int k=0;k<2;k++) if(m_nm[k]==names && m_n[k]==n){
        memcpy(out,m_out[k],sizeof(unsigned long)*(size_t)n); if(osz) memcpy(osz,m_sz[k],sizeof(unsigned long)*(size_t)n);
        return m_ret[k]; }
    int k = m_nm[0]==NULL ? 0 : 1;
    unsigned long so_[S_NSYM]={0};
    int r=sym_lookup(so,names,out,n,so_);
    if(osz) memcpy(osz,so_,sizeof(unsigned long)*(size_t)n);
    m_nm[k]=names; m_n[k]=n; m_ret[k]=r; memcpy(m_out[k],out,sizeof(unsigned long)*(size_t)n); memcpy(m_sz[k],so_,sizeof(unsigned long)*(size_t)n);
    return r;
}
/* Pick the embedded table whose version best matches, and parse it with the same
   parser files use.  Embedded tables make the binary self-contained: without them a
   copy of cub_top on another host falls back to DWARF extraction or probing on any
   version that has no built-in table. */
static int offs_embedded_load(const char*ver,long out[],char *vout,size_t vn,int ssz[]){
    int best=-1,bn=0;
    for(int i=0;i<EMB_TBL_N;i++){
        int n=ver_match_comp(ver,EMB_TBL[i].ver);
        if(n>bn){ bn=n; best=i; }
    }
    if(best<0||bn<2) return 0;        /* below major.minor there is no basis */
    if(vout) snprintf(vout,vn,"%s",EMB_TBL[best].ver);
    return offs_text_load(EMB_TBL[best].text,out,ssz);
}

/* Pick the *.tbl in a directory that best matches the detected version.
   Returns 1 on a choice (out filled), 0 if none; ties go to the longer match. */
static int offs_dir_pick(const char*dir,const char*cver,char*out,size_t on){
    DIR*d=opendir(dir); if(!d) return 0;
    struct dirent*e; int best=-1; char bestp[512]="";
    while((e=readdir(d))){
        size_t l=strlen(e->d_name);
        if(l<5 || strcmp(e->d_name+l-4,".tbl")) continue;
        char p[512]; snprintf(p,sizeof p,"%s/%s",dir,e->d_name);
        char v[32];
        if(!offs_tbl_version(p,v,sizeof v)) continue;
        int m=ver_match_comp(v,cver);
        if(m>=2 && m>best){ best=m; snprintf(bestp,sizeof bestp,"%s",p); }
    }
    closedir(d);
    if(best<0) return 0;
    snprintf(out,on,"%s",bestp); return 1;
}

typedef struct { char name[40]; double bytes; char note[72]; char grade;
    /* count-only: anchor probing on a stripped foreign build fixes the count alone.
       The byte figure (count x size) is not invented when the size is unknown, and it
       is left out of the total. */
    long count; char count_only;
} bitem_t;
/* The heap item's previous-frame value, found by name: B rebuilds its array every
   frame, so remembering an index would misalign everything the moment one drops. */
#define HPREVMAX 24
static struct { char name[40]; double bytes; } g_hprev[HPREVMAX];
static int g_hprevn=0;
static double hprev_get(const char *nm){
    for(int i=0;i<g_hprevn;i++) if(!strcmp(g_hprev[i].name,nm)) return g_hprev[i].bytes;
    return -1.0;                       /* No baseline */
}
static void hprev_set(const char *nm,double v){
    for(int i=0;i<g_hprevn;i++) if(!strcmp(g_hprev[i].name,nm)){ g_hprev[i].bytes=v; return; }
    if(g_hprevn<HPREVMAX){ snprintf(g_hprev[g_hprevn].name,40,"%s",nm);
                           g_hprev[g_hprevn].bytes=v; g_hprevn++; }
}
/* Delta string, placed left of the value; empty when unchanged, to keep the screen quiet */
static const char *hdelta(const char *nm,double cur){
    static char b[4][24]; static int k=0; char *o=b[k++&3];
    double p=hprev_get(nm);
    o[0]=0;
    if(p>=0){ double d=cur-p, a=d<0?-d:d;
              if(a>=1.0) snprintf(o,24,"%s%s",d>0?"+":"-",H(a)); }
    hprev_set(nm,cur);
    return o;
}
static struct {
    int ok; int partial; char why[320];   /* partial marks anchor-probing count-only mode */
    unsigned long base, sym[S_NSYM];
    unsigned long ssz[S_NSYM];        /* st_size of each root symbol in this .so */
    bitem_t it[16]; int n;
    double total;      /* Sum of valid items only */
    int n_bad;         /* Items failing validation, excluded from the sum */
} B;

/* Heap detail B items from the adopted table (B.base, B.sym, g_bt set by method_b) */
static void b_items(proc_t *P){

    bitem_t *it;
    #define ADD_COUNT(nm_,cnt_,...) do{ if(B.n<16){ it=&B.it[B.n++]; \
        snprintf(it->name,sizeof it->name,"%s",nm_); \
        it->bytes=0.0; it->grade='M'; it->count=(long)(cnt_); it->count_only=1; \
        snprintf(it->note,sizeof it->note,__VA_ARGS__); } }while(0)
    if(B.partial){
        /* Items with an anchor only.  An item that fails uniqueness cannot carry a note
           saying why (it does not exist at all), so the reason is in the help and docs. */
        long mxc=prm_get("max_clients"); if(mxc<0) mxc=0;
        if(B.sym[S_LKGL] && mxc>0){
            int off; int32_t nt;
            if(probe_i32(P->pid,B.base+B.sym[S_LKGL],mxc,mxc+16,&off,&nt))
                ADD_COUNT("lock table (tran)",nt,g_ascii?"num_trans=%d @+%d - size unknown":"num_trans=%d @+%d — 크기 미상",nt,off);
        }
        if(B.sym[S_CONNMAX] && mxc>0){
            int32_t mx=rd_i32(P->pid,B.base+B.sym[S_CONNMAX]);
            if(mx>=mxc && mx<=mxc*2+32)
                ADD_COUNT("conn entries (css)",mx,g_ascii?"max=%d - size unknown":"max=%d — 크기 미상",mx);
        }
        /* Count-only items add nothing to B.total, which method_b's memset left at 0 */
        return;
    }
    /* No single subsystem uses 64GB - the guard against reading a pointer as a count.
       Out-of-range values are downgraded to '?' and left out of totals. */
    #define SANE_MAX (64.0*1024.0*1024.0*1024.0)
    #define ADD(nm_,val_,gr_,...) do{ if(B.n<16){ it=&B.it[B.n++]; \
        snprintf(it->name,sizeof it->name,"%s",nm_); it->bytes=(val_); it->grade=(gr_); \
        it->count=0; it->count_only=0; \
        snprintf(it->note,sizeof it->note,__VA_ARGS__); \
        if(!(it->bytes>=0.0 && it->bytes<=SANE_MAX)){ \
            it->grade='U'; it->bytes=0.0; \
            snprintf(it->note,sizeof it->note,g_ascii?"out of plausible range - excluded" \
                                                     :"값 범위 벗어남 — 신뢰 불가, 합산 제외"); } } }while(0)

    /* Per item: on a read failure the grade drops to '?' so a 0 is not passed off as measured */
    /* An item at 0 was measured as 0, not missing - the screen says so */
    #define GRD (g_rd_ok?'M':'U')
    /* L4 count check: no negatives, and an upper bound from max_clients with 8x slack.
       Catches an offset that is aligned but wrong (entries far above the limit). */
    long mxcli=prm_get("max_clients"); if(mxcli<=0) mxcli=1000;
    #define CNT_OK(c) ((c)>=0)
    #define CNT_IN_RANGE(c) ((c)>=0 && (double)(c) <= (double)mxcli*8.0)
    /* An offset missing from the table is -1: that field is not read */
    if(B.sym[S_XCACHE] && g_bt->off_xc_entries>=0){
        unsigned long a=B.base+B.sym[S_XCACHE];
        g_rd_ok=1;
        int32_t ent=rd_i32(P->pid,a+g_bt->off_xc_entries);
        if(g_bt->off_xc_usage>=0 && g_bt->off_xc_clone>=0){
            int32_t uc =rd_i32(P->pid,a+g_bt->off_xc_usage);
            int32_t ucl=rd_i32(P->pid,a+g_bt->off_xc_clone);
            /* memory_usage_cache sums what the engine added; memory_usage_clone is its estimate,
               UNPACK_SCALE (3) x stream size per clone - so the two are graded apart */
            ADD("plan cache (xcache)",(double)uc,GRD,"entries=%d, engine-tracked",ent);
            ADD("plan clones (est)",(double)ucl,g_rd_ok?'E':'U',g_ascii?"engine estimate: 3 x stream size per clone":"엔진 추정치: 클론당 stream 크기 x3");
        } else if(CNT_OK(ent))   /* 11.5 and later track usage; older tables give the count only */
            ADD_COUNT("plan cache (xcache)",ent,g_ascii?"entries=%d - size not tracked":"entries=%d — 크기 미상",ent);
    }
    /* terse keys are separate from screen labels: a Hangul label makes slugify produce
       an empty string and keys like `heapB._bytes`.  The screen changes with L() but
       keys must stay fixed ASCII or monitoring integrations break. */
    if(B.sym[S_QLIST]||B.sym[S_QPOOL]){
        /* The manual's term is "query result cache" (list cache is the internal name).
           The feature is off entirely when max_query_cache_entries or query_cache_size_in_pages
           is 0 (the 11.5 default), so the screen distinguishes "off, hence 0" from "on but empty". */
        long qce=prm_get("max_query_cache_entries"), qcp=prm_get("query_cache_size_in_pages");
        int qok=(!B.sym[S_QLIST]||g_bt->off_qlist_npages>=0) && (!B.sym[S_QPOOL]||g_bt->off_qpool_nent>=0);
        g_rd_ok=1;
        if(qce==0 || qcp==0)
            ADD("result cache (query)",0.0,GRD,g_ascii?"disabled (%s=0)":"비활성 (%s=0)",
                qce==0?"max_query_cache_entries":"query_cache_size_in_pages");
        else if(qok){
            /* The cached results themselves are preserved temp list files (temp volume and data
               buffer, already outside A), so their pages are a count outside the heap total.
               The heap is the entry pool (4096B slots) and the (max_plan_cache_entries + 10)
               hash tables of as many buckets each, made at boot (qfile_initialize_list_cache). */
            int32_t np=B.sym[S_QLIST]?rd_i32(P->pid,B.base+B.sym[S_QLIST]+g_bt->off_qlist_npages):0;
            int32_t pe=B.sym[S_QPOOL]?rd_i32(P->pid,B.base+B.sym[S_QPOOL]+g_bt->off_qpool_nent):0;
            ADD("result cache (query)",CNT_OK(pe)?(double)pe*4096:-1.0,GRD,g_ascii?"entry pool %d x 4096":"엔트리 풀 %d x 4096",pe);
            long xe=prm_get("max_plan_cache_entries");
            if(xe>=0){ double nh=(double)xe+10;   /* per table: MHT_TABLE 112B, its fixed heap, nh bucket pointers */
                ADD("result cache hash (est)",nh*(112.0+64.0+nh*8.0)+nh*12.0,'E',
                    g_ascii?"%.0f tables x ~%.0f buckets (max_plan_cache_entries+10), lower bound":"해시 %.0f개 x 버킷 약 %.0f (max_plan_cache_entries+10), 하한",nh,nh); }
            else ADD("result cache hash (est)",0.0,'U',g_ascii?"max_plan_cache_entries not read - size unknown":"max_plan_cache_entries 미확인 — 크기 미상");
            if(CNT_OK(np))
                ADD_COUNT("result temp pages",np,g_ascii?"%d temp-volume pages (%.1fMB), not heap":"temp 볼륨 페이지 %d (%.1fMB) — 힙 아님",
                          np,(double)np*g_pagesize/1048576.0);
        }
    }
    if(B.sym[S_FPCNT]){
        g_rd_ok=1;
        int32_t c=rd_i32(P->pid,B.base+B.sym[S_FPCNT]);
        ADD("filter pred cache",CNT_OK(c)?(double)c*g_bt->sz_fpcache:-1.0,GRD,"counter=%d x%d",c,g_bt->sz_fpcache);
    }
    /* lockfree_hashmap kind: an alloc_cnt is the old lf freelist's, which stays 0 under the hidden
       new_lfhash=yes - then the count is unknown, not zero.  No wrapper (older builds): the old hash. */
    #define HASH_OLD(base_,off_) ((off_)<0 || rd_i32(P->pid,(base_)+(off_))==0)
    #define NEW_HASH_NOTE (g_ascii?"new lockfree hash (new_lfhash) - count not read, excluded":"새 lockfree 해시(new_lfhash) — 개수 미상, 합산 제외")
    if(B.sym[S_LKGL] && g_bt->off_lk_ntran>=0){
        g_rd_ok=1;
        int32_t nt=rd_i32(P->pid,B.base+B.sym[S_LKGL]+g_bt->off_lk_ntran);
        /* lk_Gl.tran_lock_table = per-transaction lock slots: num_trans x 152B
           preallocated from the connection limit.  Not lockdb's "Object Lock
           Table" (an LK_ENTRY/LK_RES pool), so the label names its subject. */
        ADD("lock table (tran)",CNT_IN_RANGE(nt)?(double)nt*g_bt->sz_lk_tran:-1.0,GRD,
            g_ascii?"num_trans=%d x%dB (not object locks)":"num_trans=%d x%dB (객체락 아님)",nt,g_bt->sz_lk_tran);
    }
    /* Object locks: the lock resources (LK_RES, the object hash's freelist) and the lock entries
       (LK_ENTRY freelist).  alloc_cnt is what the freelist has allocated, in use or free. */
    if(B.sym[S_LKGL] && g_bt->hx.lk_res>=0 && g_bt->hx.sz_lkr>0){
        unsigned long lk=B.base+B.sym[S_LKGL];
        g_rd_ok=1;
        if(HASH_OLD(lk,g_bt->hx.lk_ht)){
            int32_t c=rd_i32(P->pid,lk+g_bt->hx.lk_res);
            ADD("lock resources (obj)",CNT_OK(c)?(double)c*g_bt->hx.sz_lkr:-1.0,GRD,"alloc_cnt=%d x%dB",c,g_bt->hx.sz_lkr);
        } else ADD("lock resources (obj)",0.0,'U',"%s",NEW_HASH_NOTE);
    }
    if(B.sym[S_LKGL] && g_bt->hx.lk_entfl>=0 && g_bt->hx.lffl>=0 && g_bt->hx.sz_lke>0){
        g_rd_ok=1;
        int32_t c=rd_i32(P->pid,B.base+B.sym[S_LKGL]+g_bt->hx.lk_entfl+g_bt->hx.lffl);
        ADD("lock entries (obj)",CNT_OK(c)?(double)c*g_bt->hx.sz_lke:-1.0,GRD,"alloc_cnt=%d x%dB",c,g_bt->hx.sz_lke);
    }
    if(B.sym[S_SESS] && g_bt->off_lf_alloccnt>=0){
        g_rd_ok=1;
        if(HASH_OLD(B.base+B.sym[S_SESS],g_bt->hx.sess_ht)){
            int32_t c=rd_i32(P->pid,B.base+B.sym[S_SESS]+g_bt->off_lf_alloccnt);
            ADD("session table",CNT_IN_RANGE(c)?(double)c*g_bt->sz_session:-1.0,GRD,g_ascii?"alloc_cnt=%d x%dB, cap %ld":"alloc_cnt=%d x%dB, 한도 %ld",c,g_bt->sz_session,mxcli*8);
        } else ADD("session table",0.0,'U',"%s",NEW_HASH_NOTE);
    }
    /* catalog_Hashmap has its own alloc_cnt offset (OFF_LF_ALLOCCNT2), not the sessions' */
    if(B.sym[S_CAT] && g_bt->hx.lf2>=0){
        g_rd_ok=1;
        if(HASH_OLD(B.base+B.sym[S_CAT],g_bt->hx.cat_ht)){
            int32_t c=rd_i32(P->pid,B.base+B.sym[S_CAT]+g_bt->hx.lf2);
            ADD("catalog cache",CNT_OK(c)?(double)c*g_bt->sz_catalog:-1.0,GRD,"alloc_cnt=%d x%d",c,g_bt->sz_catalog);
        } else ADD("catalog cache",0.0,'U',"%s",NEW_HASH_NOTE);
    }
    #undef HASH_OLD
    #undef NEW_HASH_NOTE
    if(B.sym[S_CONNMAX]){
        g_rd_ok=1;
        int32_t mx=rd_i32(P->pid,B.base+B.sym[S_CONNMAX]);
        int32_t ac=B.sym[S_CONNACT]?rd_i32(P->pid,B.base+B.sym[S_CONNACT]):0;
        ADD("conn entries (css)",CNT_IN_RANGE(mx)?(double)mx*g_bt->sz_conn:-1.0,GRD,"max=%d act=%d x%d",mx,ac,g_bt->sz_conn);
    }
    #undef GRD
    #undef CNT_OK
    #undef CNT_IN_RANGE
    #undef ADD
    /* Sum valid items only; read failures and out-of-range values are excluded, leaving the count */
    B.total=0; B.n_bad=0;
    for(int i=0;i<B.n;i++){
        if(B.it[i].grade=='U'){ B.n_bad++; continue; }
        B.total+=B.it[i].bytes;
    }
}
static void method_b(proc_t *P){
    memset(&B,0,sizeof B);
    char so[512]={0};
    B.base=lib_base(P->pid,"libcubrid.so.",so,sizeof so);   /* Common to 10.x-11.x.  lib_base is a substring match, and neither
       "libcubrid_timezones.so." nor "libcubridsa" contains "libcubrid.so.",
       so there is no false match. */
    snprintf(g_so_path,sizeof g_so_path,"%s",so);        /* terse heapB.lib: the file actually read (the running inode via map_files) */
    if(!B.base){ snprintf(B.why,sizeof B.why,g_ascii?"libcubrid.so not mapped":"libcubrid.so 매핑 없음"); return; }
    if(!so[0]){ snprintf(B.why,sizeof B.why,"%s",g_lib_why); return; }
    /* Version and table choices belong to the library file, not to this run.  Switching
       to a server of another release kept the first one's version and tables, and a
       table that passed the gates by chance would be adopted with the wrong sizes. */
    { static dev_t ld; static ino_t li; static int lk=0; struct stat ls;
      if(stat(so,&ls)==0 && (!lk || ls.st_dev!=ld || ls.st_ino!=li)){
          ld=ls.st_dev; li=ls.st_ino; lk=1; g_lib_gen++; g_cver[0]=0; } }
    if(!g_cver[0]) detect_cubrid_ver(so);   /* From the very file the process mapped */
    if(!CAP.vm_readv && !CAP.mem_pread){
        if(g_perm_why[0]) snprintf(B.why,sizeof B.why,"%s",g_perm_why);
        else snprintf(B.why,sizeof B.why,g_ascii?"cannot read process memory":"프로세스 메모리 읽기 불가");
        return; }

    /* Cache across frames: symbol addresses and the adopted table are constant for
       the same .so and pid.  The L4 self-check is still re-read every frame. */
    {
        static char c_so[512]; static long long c_mt=-1; static int c_pid=-1;
        static const btab_t *c_bt=NULL;
        static unsigned long c_sym[S_NSYM];
        struct stat cst; long long mt = stat(so,&cst)==0 ? (long long)cst.st_mtime : -2;
        /* No short cut while this build's DWARF extraction has just finished: the cached
           table may be another build's, adopted while the child was still running. */
        if(c_bt && dw_kid_finished_for(so)) c_bt=NULL;
        if(c_bt && c_pid==P->pid && c_mt==mt && !strcmp(c_so,so)){
            long long want=prm_get("data_buffer_pages");
            memcpy(B.sym,c_sym,sizeof B.sym);
            int32_t nbuf=rd_i32(P->pid,B.base+B.sym[S_PGBUF]+c_bt->off_pgbuf_nbuf);
            /* Even on a cache hit the value is sanity-checked every frame, for the reason above */
            if((nbuf>0 && nbuf<=(1<<26)) && (want<=0 || nbuf==(int32_t)want)){
                g_bt=c_bt; B.ok=1;
                if(!B.partial)
                    snprintf(B.why,sizeof B.why,g_ascii?"self-check passed (num_buffers=%d, %s)"
                                                     :"자기검증 통과(num_buffers=%d, %s)",nbuf,g_bt->tag);
                goto adopted;
            }
            c_bt=NULL;   /* Validation failed: invalidate the cache and run the whole ladder again */
        }
        /* Filled when the ladder below adopts a candidate */
        #define BT_CACHE_FILL() do{ \
            snprintf(c_so,sizeof c_so,"%s",so); c_mt=mt; c_pid=P->pid; \
            c_bt=g_bt; memcpy(c_sym,B.sym,sizeof c_sym); }while(0)

    /* Candidate order.  num_buffers sits at offset 0 of pgbuf_Pool in every release, so the
       paramdump cross-check alone cannot tell builds apart; a candidate is therefore
       also matched on the st_size of the root symbols, and every candidate goes through
       the secondary anchors.
         1. a table of exactly this build (builtin, then the offsets file or embedded copy)
         2. this .so's own DWARF (or its cache)
         3. a table of another build, accepted only when its recorded symbol sizes all
            match and paramdump confirms num_buffers
       A table of another build without recorded sizes is never adopted. */
    long long want=prm_get("data_buffer_pages");
    long mxc=prm_get("max_clients");
    int32_t nbuf=0; int tried=0; char lastwhy[128]; snprintf(lastwhy,sizeof lastwhy,g_ascii?"no candidate":"후보 없음");
    /* One candidate through the gates; exact_ = the table is of this very build.  Macro-
       internal names use tb_/tm_ so they cannot collide with the caller's argument. */
    #define TRY_BTAB(bt_,exact_) do{ const btab_t*tb_=(bt_); int tm_=(exact_); tried++; \
        memset(B.sym,0,sizeof B.sym); memset(B.ssz,0,sizeof B.ssz); \
        if(sym_lookup_memo(so,tb_->syms,B.sym,S_NSYM,B.ssz)<=0){ \
            snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] symbol lookup failed":"[%s] 심볼 조회 실패",tb_->tag); break; } \
        if(!B.sym[S_PGBUF]){ \
            snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] no pgbuf_Pool symbol":"[%s] pgbuf_Pool 심볼 없음",tb_->tag); break; } \
        if(tb_->off_pgbuf_nbuf<0){ \
            snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] no num_buffers offset":"[%s] num_buffers 오프셋 없음",tb_->tag); break; } \
        { const int offs[]={tb_->off_pgbuf_nbuf,tb_->off_xc_entries,tb_->off_xc_usage, \
                            tb_->off_xc_clone,tb_->off_qlist_npages,tb_->off_qpool_nent, \
                            tb_->off_lf_alloccnt,tb_->off_lk_ntran}; \
          int bad=0; \
          for(unsigned i2=0;i2<sizeof offs/sizeof offs[0];i2++) \
              if(offs[i2]>=0 && (offs[i2]&3)) bad=1; \
          if(bad){ snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] offset misaligned":"[%s] 오프셋 정렬 위반",tb_->tag); break; } } \
        /* Root symbol sizes: a struct of another size is another layout */ \
        { int known_=0, bad_=-1; \
          for(int i2=0;i2<S_NSYM;i2++) if(tb_->ssz[i2]>0 && B.sym[i2]){ \
              known_++; if(B.ssz[i2]!=(unsigned long)tb_->ssz[i2]){ bad_=i2; break; } } \
          if(bad_>=0){ snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] symbol size mismatch %s %lu!=%d":"[%s] 심볼 크기 불일치 %s %lu≠%d",tb_->tag, \
                                SYMN10[bad_],B.ssz[bad_],tb_->ssz[bad_]); break; } \
          if(!tm_ && !known_){ snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] table of another build - no symbol sizes recorded":"[%s] 다른 빌드의 표 — 심볼 크기 기록 없음",tb_->tag); break; } } \
        nbuf=rd_i32(P->pid,B.base+B.sym[S_PGBUF]+tb_->off_pgbuf_nbuf); \
        if(want>0){ \
            if(nbuf!=(int32_t)want){ \
                snprintf(lastwhy,sizeof lastwhy,"[%s] num_buffers=%d != data_buffer_pages=%lld", \
                         tb_->tag,nbuf,want); \
                break; } \
        } else if(!tm_){ \
            snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] no paramdump - a table of another build is not adopted unverified":"[%s] paramdump 없음 — 다른 빌드의 표는 미검증 채택 불가",tb_->tag); \
            break; } \
        /* Without paramdump, sanity-check the value instead: num_buffers is \
           preallocated at boot, so it cannot be 0 or implausibly large on a live server. */ \
        if(nbuf<=0 || nbuf>(1<<26)){ \
            snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] num_buffers=%d implausible (no paramdump)":"[%s] num_buffers=%d 위생 위반(paramdump 부재)",tb_->tag,nbuf); \
            break; } \
        /* Secondary anchors, for every candidate: the conn max symbol and lk num_trans \
           against the range paramdump's max_clients implies */ \
        if(want>0 && mxc>0){ \
            if(B.sym[S_CONNMAX]){ int32_t mx_=rd_i32(P->pid,B.base+B.sym[S_CONNMAX]); \
                if(!(mx_>=mxc && mx_<=mxc*2+32)){ \
                    snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] secondary anchor mismatch (conn max)":"[%s] 이차 앵커 불일치(conn max)",tb_->tag); break; } } \
            if(tb_->off_lk_ntran>=0 && B.sym[S_LKGL]){ int32_t nt_=rd_i32(P->pid,B.base+B.sym[S_LKGL]+tb_->off_lk_ntran); \
                if(!(nt_>=mxc && nt_<=mxc+16)){ \
                    snprintf(lastwhy,sizeof lastwhy,g_ascii?"[%s] secondary anchor mismatch (lk num_trans)":"[%s] 이차 앵커 불일치(lk num_trans)",tb_->tag); break; } } \
        } \
        g_bt=tb_; B.ok=1; }while(0)
    /* A file or DWARF table: symbol names differ by release family (11.2+ mangled vs 10.x
       C symbols), so both sets are tried; the first set's rejection reason is kept. */
    #define TRY_BOTH(bt_,exact_) do{ char why1_[128]=""; \
        for(int si_=0; si_<2 && !B.ok; si_++){ \
            (bt_)->syms = si_ ? (const char**)SYMN10 : (const char**)SYMN; \
            TRY_BTAB((bt_),(exact_)); \
            if(si_==0 && !B.ok) snprintf(why1_,sizeof why1_,"%s",lastwhy); } \
        if(!B.ok && why1_[0] && (strstr(lastwhy,"심볼 조회")||strstr(lastwhy,"symbol lookup"))) snprintf(lastwhy,sizeof lastwhy,"%s",why1_); }while(0)

    /* 1a. Builtin table of this very build */
    for(int ti=0; ti<NBTABS && !B.ok; ti++)
        if(g_cver[0] && !strcmp(g_cver,BTABS[ti].ver)) TRY_BTAB(&BTABS[ti],1);

    /* The offsets table: --offsets FILE (taken as the user's choice for this build), the
       best *.tbl of an --offsets directory or of offsets/ beside the executable, or the
       embedded copy.  It is "exact" when its #meta version equals the detected one. */
    static btab_t fbt; int f_ready=0, f_exact=0;
    static char odir[512]="";        /* The offsets directory in use, for step 3 */
    {
        static char resolved[512]=""; static int rst=0;   /* 0 unparsed, 1 file, 2 none, 3 embedded */
        static int r_explicit=0; static unsigned rgen=0;
        if(rgen!=g_lib_gen){ rgen=g_lib_gen; rst=0; resolved[0]=0; odir[0]=0; }
        if(rst==0){
            char cand[512]=""; r_explicit=0;
            if(g_offs_file[0]) snprintf(cand,sizeof cand,"%s",g_offs_file);
            else{                       /* Auto-discovery: offsets beside the executable, or one level up */
                char exe[400]; ssize_t el=readlink("/proc/self/exe",exe,sizeof exe-1);
                if(el>0){ exe[el]=0; char*sl=strrchr(exe,'/'); if(sl)*sl=0;
                    char d1[512]; snprintf(d1,sizeof d1,"%s/offsets",exe);
                    char d2[512]; snprintf(d2,sizeof d2,"%s/../offsets",exe);
                    struct stat ds;
                    if(!stat(d1,&ds)&&S_ISDIR(ds.st_mode)) snprintf(cand,sizeof cand,"%s",d1);
                    else if(!stat(d2,&ds)&&S_ISDIR(ds.st_mode)) snprintf(cand,sizeof cand,"%s",d2);
                }
            }
            rst=2;
            if(cand[0]){
                struct stat ds;
                if(!stat(cand,&ds)&&S_ISDIR(ds.st_mode)){
                    snprintf(odir,sizeof odir,"%s",cand);
                    if(offs_dir_pick(cand,g_cver,resolved,sizeof resolved)) rst=1;
                    else if(g_offs_file[0])   /* No matching table in the given directory leaves a reason */
                        snprintf(lastwhy,sizeof lastwhy,g_ascii?"no table for %.20s in the --offsets directory"
                                 :"--offsets 디렉터리에 %.20s 와 맞는 표 없음",g_cver);
                } else { snprintf(resolved,sizeof resolved,"%s",cand); rst=1; r_explicit=(g_offs_file[0]!=0); }
            }
        }
        /* A file, when given, wins over the embedded copy: a newer patch release can be
           handed over as a table without rebuilding the binary. */
        static char embver[32]="";
        static long fv[DW_N]; static int fss[S_NSYM]; static int fst=0; static unsigned fgen=0;
        static char fver[32]="";
        if(fgen!=g_lib_gen){ fgen=g_lib_gen; fst=0; }
        if(fst==0){
            fst=2;
            if(rst==1 && offs_file_load(resolved,fv,fss)>0){
                fst=1; if(!offs_tbl_version(resolved,fver,sizeof fver)) fver[0]=0; }
            else if(rst==1) snprintf(lastwhy,sizeof lastwhy,"%s",g_ascii?"--offsets file not readable":"--offsets 파일을 읽을 수 없음");
            if(fst!=1 && g_cver[0] && offs_embedded_load(g_cver,fv,embver,sizeof embver,fss)>0){
                fst=1; rst=3; snprintf(fver,sizeof fver,"%s",embver); }
        }
        if(fst==1){
            fbt.tag = (rst==3) ? "offsets-embedded" : "offsets-file";
            fbt.ver=fver; fbt.syms=SYMN;
            fbt.off_pgbuf_nbuf =(int)fv[DW_PGBUF];
            fbt.off_xc_entries =(int)fv[DW_XC_ENT];  fbt.off_xc_usage=(int)fv[DW_XC_USE];
            fbt.off_xc_clone   =(int)fv[DW_XC_CLN];
            fbt.off_qlist_npages=(int)fv[DW_QLIST];  fbt.off_qpool_nent=(int)fv[DW_QPOOL];
            fbt.off_lf_alloccnt=(int)fv[DW_LF];
            fbt.sz_lk_tran =(int)fv[DW_SZ_LK];   fbt.sz_session=(int)fv[DW_SZ_SESS];
            fbt.sz_catalog =(int)fv[DW_SZ_CAT];  fbt.sz_fpcache=(int)fv[DW_SZ_FP];
            fbt.sz_conn    =(int)fv[DW_SZ_CONN]; fbt.sz_vacuum =(int)fv[DW_SZ_VAC];
            fbt.off_lk_ntran=(int)fv[DW_LK_NTRAN];
            pg_from_dw(&fbt,fv); ps_from_dw(&fbt,fv); ag_from_dw(&fbt,fv); hx_from_dw(&fbt,fv);
            memcpy(fbt.ssz,fss,sizeof fbt.ssz);
            f_ready=1;
            f_exact = r_explicit || (g_cver[0] && fver[0] && !strcmp(fver,g_cver));
        }
    }
    /* 1b. The offsets table of this very build */
    if(!B.ok && f_ready && f_exact) TRY_BOTH(&fbt,1);

    /* 2. This .so's own DWARF */
    if(!B.ok){
        static long dwv[DW_N]; static int dwst=0; static char dwwhy[160]; static unsigned dgen=0;
        if(dgen!=g_lib_gen){ dgen=g_lib_gen; dwst=0; dwwhy[0]=0; }
        static btab_t dwbt;
        if(dwst==0) dwst=dw_load_or_extract(so,dwv,dwwhy,sizeof dwwhy);
        if(dwst==1){
            dwbt.tag="dwarf-runtime"; dwbt.ver=g_cver; dwbt.syms=SYMN;
            dwbt.off_pgbuf_nbuf =(int)dwv[DW_PGBUF];
            dwbt.off_xc_entries =(int)dwv[DW_XC_ENT];
            dwbt.off_xc_usage   =(int)dwv[DW_XC_USE];
            dwbt.off_xc_clone   =(int)dwv[DW_XC_CLN];
            dwbt.off_qlist_npages=(int)dwv[DW_QLIST];
            dwbt.off_qpool_nent =(int)dwv[DW_QPOOL];
            dwbt.off_lf_alloccnt=(int)dwv[DW_LF];
            dwbt.sz_lk_tran =(int)dwv[DW_SZ_LK];   dwbt.sz_session=(int)dwv[DW_SZ_SESS];
            dwbt.sz_catalog =(int)dwv[DW_SZ_CAT];  dwbt.sz_fpcache=(int)dwv[DW_SZ_FP];
            dwbt.sz_conn    =(int)dwv[DW_SZ_CONN]; dwbt.sz_vacuum =(int)dwv[DW_SZ_VAC];
            dwbt.off_lk_ntran=(int)dwv[DW_LK_NTRAN];
            pg_from_dw(&dwbt,dwv); ps_from_dw(&dwbt,dwv); ag_from_dw(&dwbt,dwv); hx_from_dw(&dwbt,dwv);
            memset(dwbt.ssz,0,sizeof dwbt.ssz);   /* From this very .so: nothing to compare */
            TRY_BOTH(&dwbt,1);
        } else if(dwwhy[0])                  /* failed, or still extracting in the background */
            snprintf(lastwhy,sizeof lastwhy,"DWARF: %s",u8n(dwwhy,80));
    }

    /* 3. Every other table known - the builtin ones, every embedded copy and every *.tbl of
       the offsets directory - not just the one whose version string matches best: the
       nearest patch release is often one that records no sizes (and can never be adopted)
       while another does.  Tables recording no sizes are left out; the rest are tried with
       the most recorded sizes first, then the most keys, then the closest version, and the
       size gate keeps only a table whose recorded sizes all match this .so.  An embedded
       table of this very build that an offsets directory hid in 1b is tried here as exact. */
    if(!B.ok){
        #define OB_MAX 64
        static btab_t obt[OB_MAX]; static char over[OB_MAX][32]; static int obn=0; static unsigned ogen=0; static int obuilt=0;
        static int ord[OB_MAX+8], ordn=0, oexact[OB_MAX+8];       /* >=0: obt index, <0: -1-BTABS index */
        static int nosz=0;
        if(!obuilt || ogen!=g_lib_gen){
            ogen=g_lib_gen; obuilt=1; obn=0; ordn=0; nosz=0;
            int rk_sz[OB_MAX+8], rk_keys[OB_MAX+8], rk_ver[OB_MAX+8];
            /* builtin tables first in the dedup set: they are complete */
            for(int ti=0; ti<NBTABS && ordn<OB_MAX+8; ti++){
                int ns=0; for(int i=0;i<S_NSYM;i++) ns+=BTABS[ti].ssz[i]>0;
                int ex = g_cver[0] && !strcmp(g_cver,BTABS[ti].ver);
                if(ex) continue;                                  /* tried in 1a */
                if(!ns){ nosz++; continue; }
                ord[ordn]=-1-ti; oexact[ordn]=0; rk_sz[ordn]=ns; rk_keys[ordn]=DW_N; rk_ver[ordn]=ver_match_comp(BTABS[ti].ver,g_cver); ordn++;
            }
            /* embedded copies, then the directory's files */
            for(int src=0; src<2; src++){
                DIR *d = (src==1 && odir[0]) ? opendir(odir) : NULL;
                struct dirent *de=NULL; int ei=0;
                for(;;){
                    char v[32]=""; long fv[DW_N]; int fss[S_NSYM]; int got=0;
                    if(src==0){ if(ei>=EMB_TBL_N) break;
                                snprintf(v,sizeof v,"%s",EMB_TBL[ei].ver); got=offs_text_load(EMB_TBL[ei].text,fv,fss); ei++; }
                    else { if(!d || !(de=readdir(d))) break;
                           size_t l=strlen(de->d_name); if(l<5 || strcmp(de->d_name+l-4,".tbl")) continue;
                           char pth[600]; snprintf(pth,sizeof pth,"%s/%s",odir,de->d_name);
                           if(!offs_tbl_version(pth,v,sizeof v)) continue;
                           got=offs_file_load(pth,fv,fss); }
                    if(got<=0 || ver_match_comp(v,g_cver)<2) continue;   /* below major.minor: no basis */
                    int dup=0;
                    for(int k=0;k<ordn && !dup;k++){ const char*kv = ord[k]<0 ? BTABS[-1-ord[k]].ver : over[ord[k]]; dup=!strcmp(kv,v); }
                    if(dup) continue;
                    if(g_cver[0] && !strcmp(v,g_cver) && f_ready && f_exact) continue;   /* the exact one, tried in 1b */
                    int ns=0; for(int i=0;i<S_NSYM;i++) ns+=fss[i]>0;
                    if(!ns){ nosz++; continue; }
                    if(obn>=OB_MAX || ordn>=OB_MAX+8) break;
                    btab_t *b=&obt[obn]; memset(b,0,sizeof *b);
                    snprintf(over[obn],sizeof over[obn],"%s",v);
                    /* an embedded copy of this very build keeps its usual tag */
                    b->tag=(src==0 && g_cver[0] && !strcmp(v,g_cver)) ? "offsets-embedded" : "offsets-other"; b->ver=over[obn]; b->syms=SYMN;
                    b->off_pgbuf_nbuf =(int)fv[DW_PGBUF];
                    b->off_xc_entries =(int)fv[DW_XC_ENT];  b->off_xc_usage=(int)fv[DW_XC_USE];
                    b->off_xc_clone   =(int)fv[DW_XC_CLN];
                    b->off_qlist_npages=(int)fv[DW_QLIST];  b->off_qpool_nent=(int)fv[DW_QPOOL];
                    b->off_lf_alloccnt=(int)fv[DW_LF];
                    b->sz_lk_tran =(int)fv[DW_SZ_LK];   b->sz_session=(int)fv[DW_SZ_SESS];
                    b->sz_catalog =(int)fv[DW_SZ_CAT];  b->sz_fpcache=(int)fv[DW_SZ_FP];
                    b->sz_conn    =(int)fv[DW_SZ_CONN]; b->sz_vacuum =(int)fv[DW_SZ_VAC];
                    b->off_lk_ntran=(int)fv[DW_LK_NTRAN];
                    pg_from_dw(b,fv); ps_from_dw(b,fv); ag_from_dw(b,fv); hx_from_dw(b,fv);
                    memcpy(b->ssz,fss,sizeof b->ssz);
                    ord[ordn]=obn; oexact[ordn]= g_cver[0] && !strcmp(v,g_cver);
                    rk_sz[ordn]=ns; rk_keys[ordn]=got; rk_ver[ordn]=ver_match_comp(v,g_cver); ordn++; obn++;
                }
                if(d) closedir(d);
            }
            /* exact first, then most sizes, most keys, closest version (insertion sort, small n) */
            for(int a=1;a<ordn;a++)
                for(int b2=a; b2>0; b2--){
                    int x=b2-1, y=b2;
                    int better = oexact[y]!=oexact[x] ? oexact[y]>oexact[x]
                               : rk_sz[y]!=rk_sz[x] ? rk_sz[y]>rk_sz[x]
                               : rk_keys[y]!=rk_keys[x] ? rk_keys[y]>rk_keys[x]
                               : rk_ver[y]>rk_ver[x];
                    if(!better) break;
                    int t; t=ord[x]; ord[x]=ord[y]; ord[y]=t; t=oexact[x]; oexact[x]=oexact[y]; oexact[y]=t;
                    t=rk_sz[x]; rk_sz[x]=rk_sz[y]; rk_sz[y]=t; t=rk_keys[x]; rk_keys[x]=rk_keys[y]; rk_keys[y]=t;
                    t=rk_ver[x]; rk_ver[x]=rk_ver[y]; rk_ver[y]=t;
                }
        }
        for(int k=0;k<ordn && !B.ok;k++){
            if(ord[k]<0) TRY_BTAB(&BTABS[-1-ord[k]],oexact[k]);
            else TRY_BOTH(&obt[ord[k]],oexact[k]);
        }
        if(!B.ok && !ordn && nosz)
            snprintf(lastwhy,sizeof lastwhy,g_ascii?"%d table(s) of other builds record no symbol sizes - none adoptable"
                                                   :"다른 빌드의 표 %d개는 심볼 크기 기록이 없어 채택 불가",nosz);
        #undef OB_MAX
    }

    /* Anchor probing - last resort when both the table and DWARF are unavailable
       on a stripped build of another version. */
    if(!B.ok && want>0){
        /* Symbol names themselves differ by release family (11.2+ C++ mangling vs 10.x C
           symbols), so both sets are tried in turn. */
        const char **SYMSETS[2]; SYMSETS[0]=(const char**)SYMN; SYMSETS[1]=(const char**)SYMN10;
        for(unsigned si=0; si<2 && !B.ok; si++){
        memset(B.sym,0,sizeof B.sym);
        if(sym_lookup(so,SYMSETS[si],B.sym,S_NSYM,NULL)>0 && B.sym[S_PGBUF]){
            int off; int32_t v;
            if(probe_i32(P->pid,B.base+B.sym[S_PGBUF],want,want,&off,&v)){
                /* Fix pgbuf num_buffers uniquely within the window - both the proof that the symbol
                   and read paths work here and the gate itself.  Only items with an
                   anchor produce a count. */
                B.ok=1; B.partial=1;
                snprintf(B.why,sizeof B.why,
                         g_ascii?"anchor probe partial mode (num_buffers=%d @+%d, %s) - size unknown, count only"
                                :"앵커 탐사 부분 모드(num_buffers=%d @+%d, %s) — 크기 미상, 개수만",
                         v,off,si==0?(g_ascii?"11.x symbols":"11.x 심볼")
                                    :(g_ascii?"10.x symbols":"10.x 심볼"));
            }
        }
        }
    }
    if(!B.ok && g_sym_symtab==0){
        /* strip -s: pgbuf_Pool, sessions, catalog_Hashmap, css_Num_* are LOCAL, in .symtab only -
           not even the anchor probe has an address (--strip-debug keeps .symtab) */
        snprintf(B.why,sizeof B.why,"%s",g_ascii?"no .symtab (strip -s): the root symbols are local and only there - method B unavailable"
                                                :".symtab 없음(strip -s) — 루트 심볼이 LOCAL 이라 거기에만 있음: 방법 B 불가");
        return;
    }
    if(!B.ok){
        snprintf(B.why,sizeof B.why,
                 g_ascii?"%d tables+DWARF+probe rejected (detected %.12s) - %s"
                        :"표 %d개+DWARF+탐사 탈락(감지 %.12s) — %s",
                 tried,g_cver[0]?g_cver:"?",u8n(lastwhy,48));
        return;
    }
    if(!B.partial)   /* Do not overwrite the probing mode's own reason */
        snprintf(B.why,sizeof B.why,g_ascii?"self-check passed (num_buffers=%d, %s%s)"
                                           :"자기검증 통과(num_buffers=%d, %s%s)",
                 nbuf,g_bt->tag,(want>0)?"":(g_ascii?", no paramdump; conditional on version match"
                                                    :", paramdump 부재·버전일치 조건부"));
    if(!B.partial) BT_CACHE_FILL();   /* Probing mode is not a table, so it is excluded from the cache */
    }
adopted:;
    b_items(P);
}

static const region_t *g_reg_db=NULL,*g_reg_lg=NULL;   /* Filled by classify_regions */

/* ---- Buffer pool BCB direct read (pgbuf) ----
 * Method B applied to an array: pgbuf_Pool.BCB_table -> read the whole BCB array,
 * then gather each entry's page header (prv: lsa/pageid/volid).  Latch-free, so
 * the snapshot is graded as an estimate.
 *   L4 self-check: disable if bad VPIDs exceed 1%, or if header (pageid,volid)
 *   disagrees with the BCB vpid beyond a threshold (pages under replacement
 *   legitimately differ a little).
 *   Cost: 512MB buffer = 32,768 BCB x 144B plus a 16B gather - a few ms.
 *   Above PG_LARGE BCBs it runs only on request, then every frame.
 *   --bcb-dump FILE writes the snapshot (volmap --bufmap overlays it).
 */
typedef struct { int64_t pageid; int offset; } lsa_t;   /* struct log_lsa: pageid:48 | offset:16 (little-endian bitfield) */
static lsa_t lsa_dec(uint64_t raw){ lsa_t l; l.pageid=((int64_t)(raw<<16))>>16; l.offset=(int)(int16_t)(raw>>48); return l; }
static int   lsa_isnull(lsa_t a){ return a.pageid==-1 && a.offset==-1; }
static int   lsa_cmp(lsa_t a,lsa_t b){ if(a.pageid!=b.pageid) return a.pageid<b.pageid?-1:1;
                                       return a.offset<b.offset?-1:(a.offset>b.offset); }
#define PGBUF_F_DIRTY    0x80000000u
#define PGBUF_F_FLUSHING 0x40000000u
#define PGBUF_ZONE_MASK  0x000F0000u   /* LRU1=1<<16 LRU2=2<<16 LRU3=3<<16 INVALID=1<<18 VOID=2<<18 */
typedef struct {            /* Dump record, fixed 32B little-endian: the contract shared with volmap */
    int16_t volid; int16_t zone;  /* zone: 1/2/3 = LRU, 4 = invalid, 8 = void */
    int32_t pageid; uint32_t flags; int32_t hdr_ok;   /* hdr_ok: the page header was read and matched */
    uint64_t page_lsa;        /* Raw LSA from the in-memory page header; comparing it with the disk copy shows whether it was flushed */
    uint64_t oldest_lsa;      /* Raw BCB.oldest_unflush_lsa: when it became dirty */
} pgrec_t;
/* The dump contract (32B) stays as it is and the BCB index the recheck needs lives in
   a memory-only array - growing the record would break the file contract with volmap. */
static int *pg_bidx=NULL; static int pg_bidx_cap=0;
#define PG_MAXVOL 256
static struct {
    int ok; char why[384];
    int large;          /* Not scanned: a large buffer without a request */
    int nhdr, nonres, pm_unchecked;   /* Headers read (sample or all), skipped as not resident, read without a pagemap check */
    int nbuf, resident, dirty, flushing, z1,z2,z3, zvoid, zinv, mismatch, unread, badvpid;
    int unflushed;      /* DIRTY or FLUSHING: not yet written by the buffer (a page re-dirtied mid-flush has both) */
    int flush_only;     /* FLUSHING without DIRTY: the engine cleared oldest_unflush_lsa when the flush began */
    int mismatch_raw;   /* Mismatches before the recheck, including replacements between the two reads */
    int recheck;        /* Number rechecked */
    int nrec_all;               /* Records before a degraded frame dropped the unverified */
    int degraded;       /* Frames where remaining mismatches crossed the threshold and dropped to valid-only mode */
    int tempvol;      /* Resident pages of temporary volumes (volid at or above PG_MAXVOL); this is normal */
    int vol_res[PG_MAXVOL], vol_dirty[PG_MAXVOL];
    lsa_t oldest_dirty; int have_oldest;
    lsa_t log_nxio, log_append, log_eof; int have_log;
    uint64_t raw_oldest, raw_nxio, raw_append, raw_eof;
    double ms; long frame;
    long scan_seq;              /* one more per whole-array pass: cap_turnover works per pass */
    /* A large buffer: the pass runs on a period set by its own cost; between passes the last
       one stands (from_last) with its age, and a slot sample gives the admission rate */
    double scan_at_ms, scan_ms_full, scan_age_s; int scan_pid, from_last, ok_last;
    pgrec_t *rec; int nrec, reccap;
    struct timespec when;
} PG;
static char g_bcb_dump[256]="";

/* Gather n scattered addresses, each bytes wide.  process_vm_readv takes up to 1024
   iovs at once; on a partial success (remote fault) the rest are read individually.
   okflag[i]=1 on success. */
static void vm_gather(int pid,const uint64_t*addr,int n,size_t each,unsigned char*out,unsigned char*okflag){
    memset(okflag,0,(size_t)n);
#ifdef __NR_process_vm_readv
    if(CAP.vm_readv){
        static struct iovec l[1024],r[1024];
        for(int i=0;i<n;i+=1024){
            int m=(n-i<1024)?(n-i):1024;
            for(int k=0;k<m;k++){ l[k].iov_base=out+(size_t)(i+k)*each; l[k].iov_len=each;
                                  r[k].iov_base=(void*)(uintptr_t)addr[i+k]; r[k].iov_len=each; }
            long got=syscall(__NR_process_vm_readv,pid,l,(unsigned long)m,r,(unsigned long)m,0UL);
            int okn = got>0 ? (int)((size_t)got/each) : 0;
            if(okn>m) okn=m;
            for(int k=0;k<okn;k++) okflag[i+k]=1;
            for(int k=okn;k<m;k++)
                okflag[i+k] = vmread(pid,(unsigned long)addr[i+k],out+(size_t)(i+k)*each,each)==(ssize_t)each;
        }
        return;
    }
#endif
    for(int i=0;i<n;i++)
        okflag[i] = vmread(pid,(unsigned long)addr[i],out+(size_t)i*each,each)==(ssize_t)each;
}

static int cap_turnover_from_dump(void);   /* Must be read as the baseline before the dump is overwritten */
/* Output files a user names.  Never through a symlink, never into someone else's file: run as
   root, following one would let any account that can plant a name truncate or write a file. */
static int g_rec_overwrite=0;      /* --record-overwrite: replace an existing recording of ours */
static int g_stream=0;             /* --stream: the live loop without a terminal, frames as JSONL on stdout */
static FILE *out_open(const char *path,int overwrite,char *why,size_t wn){
    if(!strcmp(path,"-")) return stdout;          /* --stream: the frames go to the consumer */
    /* O_NONBLOCK: a FIFO planted at the path would block the open; it is cleared after fstat */
    int fd=open(path,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK|(overwrite?0:O_EXCL),0600);
    if(fd<0){
        if(errno==EEXIST) snprintf(why,wn,"%s",g_ascii?"already exists (--record-overwrite replaces it)":"이미 있음 (덮어쓰려면 --record-overwrite)");
        else if(errno==ELOOP) snprintf(why,wn,"%s",g_ascii?"is a symbolic link - not followed":"심볼릭 링크 — 따라가지 않음");
        else if(errno==ENXIO) snprintf(why,wn,"%s",g_ascii?"not a regular file of this account - not replaced":"이 계정의 일반 파일이 아님 — 덮어쓰지 않음");
        else snprintf(why,wn,"%s",strerror(errno));
        return NULL; }
    /* One link only: a hard link planted by another account to a file of ours (a volume,
       cubrid.conf) passes the owner check and would be truncated */
    struct stat st; int fl;
    if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || st.st_nlink!=1){
        snprintf(why,wn,"%s",g_ascii?"not a regular file of this account - not replaced":"이 계정의 일반 파일이 아님 — 덮어쓰지 않음");
        if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink>1)
            snprintf(why,wn,"%s",g_ascii?"has other hard links - not replaced":"다른 하드링크가 있음 — 덮어쓰지 않음");
        close(fd); return NULL; }
    if((fl=fcntl(fd,F_GETFL))<0 || fcntl(fd,F_SETFL,fl&~O_NONBLOCK)<0){ snprintf(why,wn,"%s",strerror(errno)); close(fd); return NULL; }
    if(ftruncate(fd,0)!=0){ snprintf(why,wn,"%s",strerror(errno)); close(fd); return NULL; }
    FILE *f=fdopen(fd,"w"); if(!f){ snprintf(why,wn,"%s",strerror(errno)); close(fd); }
    return f;
}
/* A fresh temporary beside path for write-then-rename: unique, created exclusively, no symlink.
   The name carries our pid, so one already there is a leftover of an earlier run that died
   between create and rename (no live process shares our pid): it is removed once and the
   create retried - only when it is a plain file of this account, never through a link. */
static FILE *tmp_create(const char *path,char *tmp,size_t tn,mode_t mode){
    snprintf(tmp,tn,"%s.%d.tmp",path,(int)getpid());
    int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,mode);
    if(fd<0 && errno==EEXIST){
        struct stat st;
        if(lstat(tmp,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() && unlink(tmp)==0)
            fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,mode);
        else errno=EEXIST;
    }
    if(fd<0) return NULL;
    FILE *f=fdopen(fd,"w"); if(!f){ close(fd); unlink(tmp); }
    return f;
}
/* Whether this frame replaced the --bcb-dump file, and why not: --bcb-dump alone must not
   report a snapshot it did not write */
static int g_bcb_saved=0; static char g_bcb_why[200]="";
/* Pages the double write buffer holds (0 = off, -1 = setting unknown).  A page copied into it
   counts as flushed - clean in the buffer - before the DWB block reaches the volume. */
static int dwb_pages(void){
    long long b=prm_get("double_write_buffer_size");
    if(b<0) return -1;
    if(b==0) return 0;
    long long ps=g_pagesize>0?g_pagesize:16384;
    return (int)(b/ps>0?b/ps:1);
}
static void pgbuf_dump(const char*db){
    if(!g_bcb_dump[0]||!PG.ok) return;
    /* 0644 under the umask: cub_volmap --bufmap may run as another account, and the file
       holds page ids, flags and LSAs only - no row data */
    char tmp[320]; FILE*f=tmp_create(g_bcb_dump,tmp,sizeof tmp,0644);
    if(!f){ snprintf(g_bcb_why,sizeof g_bcb_why,"%s %s",g_ascii?"cannot create the temporary file:":"임시 파일 생성 실패:",strerror(errno)); return; }
    struct { char magic[8]; uint32_t version, reclen; int64_t ts_sec, ts_nsec;
             int32_t nbuf, nrec, dirty, pagesize;
             uint64_t log_append, log_nxio, log_eof, oldest_dirty; char db[32]; } h;
    memset(&h,0,sizeof h); memcpy(h.magic,"CBCBMAP1",8); h.version=1; h.reclen=(uint32_t)sizeof(pgrec_t);
    h.ts_sec=PG.when.tv_sec; h.ts_nsec=PG.when.tv_nsec;
    h.nbuf=PG.nbuf; h.nrec=PG.nrec; h.dirty=PG.dirty; h.pagesize=(int32_t)g_pagesize;
    h.log_append=PG.have_log?PG.raw_append:~(uint64_t)0; h.log_nxio=PG.have_log?PG.raw_nxio:~(uint64_t)0;
    h.log_eof=PG.have_log?PG.raw_eof:~(uint64_t)0; h.oldest_dirty=PG.have_oldest?PG.raw_oldest:~(uint64_t)0;
    snprintf(h.db,sizeof h.db,"%.*s",(int)sizeof h.db-1,db?db:"");   /* fixed 32-byte field: truncation intended */
    int ok = fwrite(&h,sizeof h,1,f)==1 && (PG.nrec==0 || fwrite(PG.rec,sizeof(pgrec_t),(size_t)PG.nrec,f)==(size_t)PG.nrec);
    int we=errno;
    if(fclose(f)!=0){ if(ok) we=errno; ok=0; }
    if(!ok){ snprintf(g_bcb_why,sizeof g_bcb_why,"%s %s",g_ascii?"write failed:":"쓰기 실패:",strerror(we)); unlink(tmp); return; }
    if(rename(tmp,g_bcb_dump)!=0){
        snprintf(g_bcb_why,sizeof g_bcb_why,"%s %s",g_ascii?"rename failed:":"교체(rename) 실패:",strerror(errno)); unlink(tmp); return; }
    g_bcb_saved=1;
}

/* Zone counters, by the zone field kept in a record (flags>>16) */
static void pg_zone_add(int z,int d){
    if(z==1) PG.z1+=d; else if(z==2) PG.z2+=d; else if(z==3) PG.z3+=d;
    else if(z==4) PG.zinv+=d; else if(z==8) PG.zvoid+=d;
}
/* Residency, dirty and oldest-dirty totals from the records, after the recheck replaced some */
static void pg_recount(void){
    PG.resident=PG.dirty=PG.flushing=PG.tempvol=PG.unflushed=PG.flush_only=0; PG.have_oldest=0;
    memset(PG.vol_res,0,sizeof PG.vol_res); memset(PG.vol_dirty,0,sizeof PG.vol_dirty);
    for(int k=0;k<PG.nrec;k++){
        const pgrec_t*r=&PG.rec[k]; int vol_ok=(r->volid<PG_MAXVOL);
        if(!vol_ok) PG.tempvol++;
        PG.resident++; if(vol_ok) PG.vol_res[r->volid]++;
        if(r->flags&PGBUF_F_DIRTY){
            PG.dirty++; if(vol_ok) PG.vol_dirty[r->volid]++;
            lsa_t l=lsa_dec(r->oldest_lsa);
            if(!lsa_isnull(l) && (!PG.have_oldest || lsa_cmp(l,PG.oldest_dirty)<0)){ PG.oldest_dirty=l; PG.raw_oldest=r->oldest_lsa; PG.have_oldest=1; }
        }
        if(r->flags&PGBUF_F_FLUSHING) PG.flushing++;
        if(r->flags&(PGBUF_F_DIRTY|PGBUF_F_FLUSHING)) PG.unflushed++;
        if((r->flags&PGBUF_F_FLUSHING) && !(r->flags&PGBUF_F_DIRTY)) PG.flush_only++;
    }
}
/* Whether addr is resident in pid, from /proc/pid/pagemap (bit 63).  Reading pagemap does not
   touch the page; reading a swapped-out page would swap it in.  One 4KB block (512 pages) is
   cached since header addresses run in order.  Unreadable pagemap: -1 (not checked). */
/* One pagemap block (2MB of address space) is kept between calls, but only within a frame:
   residency changes between frames (swap-out, fault-in), so pm_frame() drops it. */
static unsigned g_pm_gen=0;
static void pm_frame(void){ g_pm_gen++; }
static int pm_present(int pid,unsigned long addr){
    static int fd=-1, fpid=-1; static uint64_t blk[512]; static long bno=-1; static unsigned gen=0, fgen=0;
    if(fpid!=pid || fgen!=g_pid_gen){ if(fd>=0) close(fd); char p[64]; snprintf(p,sizeof p,"/proc/%d/pagemap",pid);
                   fd=open(p,O_RDONLY|O_CLOEXEC); fpid=pid; fgen=g_pid_gen; bno=-1; }
    if(gen!=g_pm_gen){ gen=g_pm_gen; bno=-1; }
    if(fd<0) return -1;
    unsigned long pg=addr/4096; long b=(long)(pg/512);
    if(b!=bno){ if(pread(fd,blk,sizeof blk,(off_t)b*(off_t)sizeof blk)!=(ssize_t)sizeof blk) return -1; bno=b; }
    return (int)(blk[pg%512]>>63);
}
static void pg_log(proc_t*P){
    /* Log tail, the upper bound of WAL continuity: append >= nxio (the disk boundary) >= page LSA */
    PG.have_log=0;
    if(B.sym[S_LOGGL] && g_bt->pg.log_nxio>=0){
        unsigned long lg=B.base+B.sym[S_LOGGL]; uint64_t a=0,n=0,e=0;
        if(vmread(P->pid,lg+g_bt->pg.log_append,&a,8)==8 && vmread(P->pid,lg+g_bt->pg.log_nxio,&n,8)==8
           && vmread(P->pid,lg+g_bt->pg.log_eof,&e,8)==8){
            PG.raw_append=a; PG.raw_nxio=n; PG.raw_eof=e;
            PG.log_append=lsa_dec(a); PG.log_nxio=lsa_dec(n); PG.log_eof=lsa_dec(e);
            /* Check: pageid must be non-negative and append >= nxio */
            if(PG.log_append.pageid>=0 && PG.log_nxio.pageid>=0 && lsa_cmp(PG.log_append,PG.log_nxio)>=0) PG.have_log=1;
        }
    }
}
static int g_pg_detail=0;   /* Page-level detail on a large buffer: asked for (live d, --pgbuf-detail, --bcb-dump) */
#define PG_LARGE 262144     /* BCBs above which the whole-array pass runs on a period, not every frame */
#define PG_BUDGET 0.01      /* share of wall time the pass may take on a large buffer */
#define PG_LARGE_MIN_S 5.0  /* and never more often than this */
#define PG_SAMPLE_STRIDE 64 /* every 64th slot is read each frame between passes */
/* The slot sample: the VPID of every PG_SAMPLE_STRIDE-th BCB, read each frame on a large
   buffer.  A slot whose page changed since the last frame admitted a page, and the sample
   is unbiased for the whole array, so admissions per second = changed/sampled x nbuf / dt.
   The slot within each bucket of PG_SAMPLE_STRIDE is picked by a fixed hash, so a pattern
   in the engine's slot use with a period dividing the stride cannot bias the sample.
   A slot that changed twice in one frame counts once (under-count): at or above half the
   sample changed the rate is low-confidence, as adm_frac is for the pass. */
static struct { int n, cap, pid; uint64_t *key, *addr; unsigned char *buf, *ok; double at_ms, miss_pps, frac; } SM={0,0,-1,NULL,NULL,NULL,NULL,0,-1,-1};
static int g_miss_sample=0;   /* this frame's admissions came from the sample, not a pass */
static void pg_sample(proc_t*P,unsigned long pool,int nbuf){
    SM.miss_pps=-1; SM.frac=-1;
    uint64_t tab=0;
    if(vmread(P->pid,pool+g_bt->pg.bcbtab,&tab,8)!=8 || !tab || nbuf<=0) return;
    int n=nbuf/PG_SAMPLE_STRIDE; if(n<=0) return;
    if(n>SM.cap){ free(SM.key); free(SM.addr); free(SM.buf); free(SM.ok);
        SM.key=malloc(sizeof(uint64_t)*(size_t)n); SM.addr=malloc(sizeof(uint64_t)*(size_t)n); SM.buf=malloc(32*(size_t)n); SM.ok=malloc((size_t)n);
        SM.cap=(SM.key&&SM.addr&&SM.buf&&SM.ok)?n:0; SM.pid=-1; if(!SM.cap) return; }
    int o_v=g_bt->pg.vpid, o_f=g_bt->pg.flags, lo=o_v<o_f?o_v:o_f, hi=(o_v+6>o_f+4?o_v+6:o_f+4);
    size_t each=(size_t)(hi-lo); if(each>32) return;
    size_t szb=(size_t)g_bt->pg.sz_bcb;
    for(int i=0;i<n;i++){ unsigned j=(unsigned)i*2654435761u % PG_SAMPLE_STRIDE;
        SM.addr[i]=tab+((uint64_t)i*PG_SAMPLE_STRIDE+j)*szb+(uint64_t)lo; }
    vm_gather(P->pid,SM.addr,n,each,SM.buf,SM.ok);
    int same = SM.pid==P->pid && SM.n==n && SM.at_ms>0;
    long changed=0, valid=0;
    for(int i=0;i<n;i++){
        if(!SM.ok[i]){ if(same) SM.key[i]=0; continue; }
        const unsigned char *b=SM.buf+(size_t)i*each;
        int32_t pageid; int16_t volid; memcpy(&pageid,b+(o_v-lo),4); memcpy(&volid,b+(o_v-lo)+4,2);
        uint64_t k = (volid==-1||pageid==-1) ? 0 : (((uint64_t)(uint16_t)volid<<32)|(uint32_t)pageid);
        if(same){ valid++; if(k!=SM.key[i]) changed++; }
        SM.key[i]=k;
    }
    double now=now_ms();
    if(same && valid>0){ double dt=(now-SM.at_ms)/1000.0;
        SM.frac=(double)changed/(double)valid;
        SM.miss_pps = dt>0.05 ? SM.frac*(double)nbuf/dt : -1; }
    SM.at_ms=now; SM.pid=P->pid; SM.n=n;
}
#define PG_SAMPLE 256       /* Headers read to verify the offsets; a wrong offset mismatches all of them */
#define PG_MIN_VERIFY 32    /* Below this many hot/warm headers, cold ones top the sample up */
static void pgbuf_scan(proc_t*P){
    PG.frame++;
    pm_frame();                        /* this frame's residency, not a block cached by the last */
    PG.have_log=0;
    g_bcb_saved=0; g_bcb_why[0]=0;
    if(!B.ok){ PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"method B off — %s":"방법 B 비활성 — %s",B.why); return; }
    if(B.partial||g_bt->pg.sz_bcb<0){ PG.ok=0;
        snprintf(PG.why,sizeof PG.why,g_ascii?"table lacks BCB offsets (probe mode or old offsets/*.tbl — re-run tools/collect-offsets.sh)"
                                            :"표에 BCB 오프셋 없음 (탐사 모드 또는 구 offsets/*.tbl — tools/collect-offsets.sh 재수집)");
        return; }
    pg_log(P);                 /* O(1), kept on a large buffer too */
    int detail = g_pg_detail || g_bcb_dump[0];
    double t0=now_ms();
    unsigned long pool=B.base+B.sym[S_PGBUF];
    int32_t nbuf=rd_i32(P->pid,pool+g_bt->off_pgbuf_nbuf);
    /* A whole-array scan of a large buffer (2M BCBs = 300MB a pass) is load the monitor would
       put on the host: by default only the engine's own aggregates are shown */
    if(nbuf>PG_LARGE && !detail){
        /* A large buffer: the whole-array pass (2M BCBs = 300MB) runs on a period set by its
           own cost, the last pass stands with its age between, and the slot sample every
           frame gives the admission rate */
        PG.large=1; double now=now_ms();
        int due = PG.scan_pid!=P->pid;
        if(!due){ double period=PG.scan_ms_full/PG_BUDGET; if(period<PG_LARGE_MIN_S*1000.0) period=PG_LARGE_MIN_S*1000.0;
                  due = now-PG.scan_at_ms>=period; }
        if(!due){
            PG.nbuf=nbuf; PG.ok=PG.ok_last; PG.from_last=1; PG.scan_age_s=(now-PG.scan_at_ms)/1000.0;
            pg_sample(P,pool,nbuf);
            return;
        }
        PG.scan_at_ms=now; PG.scan_pid=P->pid; PG.scan_ms_full=50; PG.ok_last=0;   /* a failed pass is retried on the period, not every frame */
        PG.from_last=0; PG.scan_age_s=0;
    } else { PG.large=0; PG.from_last=0; PG.scan_age_s=0; }
    uint64_t tab=0;
    if(vmread(P->pid,pool+g_bt->pg.bcbtab,&tab,8)!=8 || !tab || nbuf<=0 || nbuf>(1<<26)){
        PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"BCB_table pointer unreadable":"BCB_table 포인터 읽기 실패"); return; }
    size_t szb=(size_t)g_bt->pg.sz_bcb, need=szb*(size_t)nbuf;
    static unsigned char*buf=NULL; static size_t cap=0;
    static uint64_t*addr=NULL; static unsigned char*hdr=NULL,*okf=NULL; static int acap=0;
    if(need>cap){ free(buf); buf=malloc(need); cap=buf?need:0; if(!buf){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; } }
    if(nbuf>pg_bidx_cap){ free(pg_bidx); pg_bidx=(int*)malloc(sizeof(int)*(size_t)nbuf);
                          pg_bidx_cap=pg_bidx?nbuf:0;
                          if(!pg_bidx){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; } }
    if(nbuf>acap){ free(addr); free(hdr); free(okf); free(PG.rec);
        addr=malloc(sizeof(uint64_t)*(size_t)nbuf); hdr=malloc(16*(size_t)nbuf); okf=malloc((size_t)nbuf);
        PG.rec=malloc(sizeof(pgrec_t)*(size_t)nbuf); acap=(addr&&hdr&&okf&&PG.rec)?nbuf:0; PG.reccap=acap;
        if(!acap){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; } }
    if(vmread(P->pid,(unsigned long)tab,buf,need)!=(ssize_t)need){
        PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"BCB array read failed (%d x %zuB)":"BCB 배열 읽기 실패 (%d x %zuB)",nbuf,szb); return; }
    /* pass 1: decode and aggregate */
    PG.nbuf=nbuf; PG.resident=PG.dirty=PG.flushing=PG.z1=PG.z2=PG.z3=PG.zvoid=PG.zinv=PG.badvpid=0;
    PG.unflushed=PG.flush_only=0;
    PG.tempvol=0;
    memset(PG.vol_res,0,sizeof PG.vol_res); memset(PG.vol_dirty,0,sizeof PG.vol_dirty);
    PG.have_oldest=0; PG.nrec=0;
    const int o_vpid=g_bt->pg.vpid, o_flags=g_bt->pg.flags, o_olsa=g_bt->pg.olsa, o_iopg=g_bt->pg.iopg;
    for(int i=0;i<nbuf;i++){
        const unsigned char*b=buf+(size_t)i*szb;
        int32_t pageid; int16_t volid; uint32_t flags; uint64_t olsa, iop;
        memcpy(&pageid,b+o_vpid,4); memcpy(&volid,b+o_vpid+4,2); memcpy(&flags,b+o_flags,4);
        memcpy(&olsa,b+o_olsa,8); memcpy(&iop,b+o_iopg,8);
        uint32_t zone=flags&PGBUF_ZONE_MASK;
        if(zone==0x10000) PG.z1++; else if(zone==0x20000) PG.z2++; else if(zone==0x30000) PG.z3++;
        else if(zone==0x40000) PG.zinv++; else if(zone==0x80000) PG.zvoid++;
        if(volid==-1 || pageid==-1) continue;                 /* Empty BCB (NULL VPID) */
        if(volid<0 || pageid<0){ PG.badvpid++; continue; }
        /* Temporary volumes have large volids (32766); permanent ones are 0..n.  Counting
           these as abnormal crosses the 1% threshold and switches the whole buffer pool
           view off.  They cannot
           go in the per-volume array, so they are counted only in the total. */
        int vol_ok = (volid<PG_MAXVOL);
        if(!vol_ok) PG.tempvol++;
        PG.resident++; if(vol_ok) PG.vol_res[volid]++;
        if(flags&PGBUF_F_DIRTY){
            PG.dirty++; if(vol_ok) PG.vol_dirty[volid]++;
            lsa_t l=lsa_dec(olsa);
            if(!lsa_isnull(l) && (!PG.have_oldest || lsa_cmp(l,PG.oldest_dirty)<0)){ PG.oldest_dirty=l; PG.raw_oldest=olsa; PG.have_oldest=1; }
        }
        if(flags&PGBUF_F_FLUSHING) PG.flushing++;
        if(flags&(PGBUF_F_DIRTY|PGBUF_F_FLUSHING)) PG.unflushed++;
        if((flags&PGBUF_F_FLUSHING) && !(flags&PGBUF_F_DIRTY)) PG.flush_only++;
        pgrec_t*r=&PG.rec[PG.nrec];
        r->volid=volid; r->zone=(int16_t)(zone>>16); r->pageid=pageid; r->flags=flags; r->hdr_ok=0;
        r->page_lsa=~(uint64_t)0; r->oldest_lsa=olsa;
        addr[PG.nrec]=iop+(uint64_t)g_bt->pg.iobuf_iopg;
        if(pg_bidx && PG.nrec<pg_bidx_cap) pg_bidx[PG.nrec]=i;   /* BCB index for the recheck */
        PG.nrec++;
    }
    if(PG.badvpid>nbuf/100){ PG.ok=0;
        snprintf(PG.why,sizeof PG.why,g_ascii?"abnormal VPIDs: %d — offset mismatch suspected":"비정상 VPID %d개 — 오프셋 불일치 의심",PG.badvpid); return; }
    /* pass 2: gather the page headers (prv 16B: lsa 8, pageid 4, volid 2, ptype/pflag 2) and compare.
       Reading a header touches that buffer page (the kernel marks it accessed, and swaps it in if
       it was out), so only a sample the engine itself used recently is read - zone 1, then zone 2,
       evenly spread - and only if resident.  All headers only for a --bcb-dump snapshot. */
    PG.mismatch=PG.unread=0; PG.mismatch_raw=0; PG.recheck=0; PG.degraded=0;
    PG.nhdr=PG.nonres=PG.pm_unchecked=0;
    int full = g_bcb_dump[0]!=0;
    static int *hsel=NULL; static uint64_t *haddr=NULL; static int hcap=0;
    if(PG.nrec>hcap){ free(hsel); free(haddr);
        hsel=(int*)malloc(sizeof(int)*(size_t)PG.nrec); haddr=(uint64_t*)malloc(sizeof(uint64_t)*(size_t)PG.nrec);
        hcap=(hsel&&haddr)?PG.nrec:0;
        if(!hcap){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; } }
    { int nc=0;
      if(full) for(int k=0;k<PG.nrec;k++) hsel[nc++]=k;
      else {
          for(int z=1;z<=2;z++) for(int k=0;k<PG.nrec;k++) if(PG.rec[k].zone==z) hsel[nc++]=k;
          if(nc>PG_SAMPLE){ for(int j=0;j<PG_SAMPLE;j++) hsel[j]=hsel[(int)((long)j*nc/PG_SAMPLE)]; nc=PG_SAMPLE; }
      }
      int m=0;
      for(int j=0;j<nc;j++){
          int pr=pm_present(P->pid,(unsigned long)addr[hsel[j]]);
          if(pr==0){ PG.nonres++; continue; }
          if(pr<0) PG.pm_unchecked++;
          hsel[m]=hsel[j]; haddr[m]=addr[hsel[j]]; m++; }
      /* An idle server can have few or no hot/warm pages, and the offsets would then go
         unchecked: resident cold pages, evenly spread, top the sample up to PG_MIN_VERIFY. */
      if(!full && m<PG_MIN_VERIFY){
          int ncold=0; for(int k=0;k<PG.nrec;k++) if(PG.rec[k].zone!=1 && PG.rec[k].zone!=2) ncold++;
          int want=PG_MIN_VERIFY-m, step=ncold>want?ncold/want:1, seen=0;
          for(int k=0;k<PG.nrec && m<PG_MIN_VERIFY;k++){
              if(PG.rec[k].zone==1 || PG.rec[k].zone==2) continue;
              if(seen++%step) continue;
              int pr=pm_present(P->pid,(unsigned long)addr[k]);
              if(pr==0){ PG.nonres++; continue; }
              if(pr<0) PG.pm_unchecked++;
              hsel[m]=k; haddr[m]=addr[k]; m++; }
      }
      PG.nhdr=m; }
    /* Nothing to check the offsets against - no page in the pool, or none resident: the
       values are not shown as verified ones */
    if(PG.nhdr==0){ PG.ok=0;
        if(PG.nrec==0) snprintf(PG.why,sizeof PG.why,g_ascii?"no page in the buffer pool - nothing to verify the offsets with"
                                                         :"버퍼풀에 적재된 페이지 없음 — 오프셋을 검증할 대상 없음");
        else snprintf(PG.why,sizeof PG.why,g_ascii?"no page header could be read to verify the offsets (%d not resident) - not verified"
                                                  :"오프셋을 검증할 페이지 헤더를 읽지 못함(비상주 %d) — 검증 안 됨",PG.nonres);
        return; }
    if(PG.nhdr>0){
        vm_gather(P->pid,haddr,PG.nhdr,16,hdr,okf);
        /* Collect the mismatched indices for the recheck.  addr[] is overwritten there, so
           the original iopage addresses are kept alongside. */
        static int *midx=NULL; static uint64_t *maddr=NULL; static int mcap=0;
        int nm=0;
        if(PG.nrec>mcap){ free(midx); free(maddr);
            midx=(int*)malloc(sizeof(int)*(size_t)PG.nrec);
            maddr=(uint64_t*)malloc(sizeof(uint64_t)*(size_t)PG.nrec);
            mcap=(midx&&maddr)?PG.nrec:0; }
        /* Without the index arrays no mismatch could be rechecked or counted, and 0
           mismatches would pass the gate - the frame is dropped instead */
        if(!mcap){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; }
        for(int j=0;j<PG.nhdr;j++){
            int k=hsel[j]; pgrec_t*r=&PG.rec[k];
            if(!okf[j]){ PG.unread++; continue; }
            uint64_t lsa; int32_t hp; int16_t hv;
            memcpy(&lsa,hdr+(size_t)j*16,8); memcpy(&hp,hdr+(size_t)j*16+8,4); memcpy(&hv,hdr+(size_t)j*16+12,2);
            if(hp!=r->pageid || hv!=r->volid){
                PG.mismatch_raw++;
                midx[nm]=k; maddr[nm]=haddr[j]; nm++;
                continue;
            }
            r->page_lsa=lsa; r->hdr_ok=1;
        }
        /* Recheck.  Pass 1 (BCB array) and pass 2 (page headers) are taken at different
           times, so pages replaced in between necessarily disagree; a flat 5%
           threshold could not tell that from a wrong offset and disabled 40% of
           frames under traffic.  The recheck re-pairs only the mismatches: a
           replacement resolves, a wrong offset stays 100% mismatched.  Cost is
           proportional to the mismatch count alone. */
        if(nm>0){
            /* rvp holds whole BCBs, whose size differs by version: its capacity is kept in bytes,
               or a switch to an instance with larger BCBs would overrun it */
            static unsigned char *rvp=NULL, *rhd=NULL, *rok=NULL; static int rcap=0; static size_t rvp_bytes=0;
            if(nm>rcap || szb*(size_t)nm>rvp_bytes){ free(rvp); free(rhd); free(rok);
                rvp=(unsigned char*)malloc(szb*(size_t)nm);    /* the whole BCB: a replaced page brings its own flags and LSA */
                rhd=(unsigned char*)malloc(16*(size_t)nm);
                rok=(unsigned char*)malloc((size_t)nm*2);
                rcap=(rvp&&rhd&&rok)?nm:0; rvp_bytes=rcap?szb*(size_t)nm:0; }
            if(rcap>=nm){
                /* Re-read those BCBs' vpids now */
                static uint64_t *va=NULL; static int vcap=0;
                if(nm>vcap){ free(va); va=(uint64_t*)malloc(sizeof(uint64_t)*(size_t)nm); vcap=va?nm:0; }
                if(vcap>=nm){
                    for(int j=0;j<nm;j++){
                        /* BCB element address = tab + i*szb, where i is the original BCB index, not the rec
                           index: rec skips empty BCBs, so it cannot be derived from the
                           iopage address - the BCB index is kept in rec (bidx). */
                        va[j]=(uint64_t)tab+(uint64_t)pg_bidx[midx[j]]*(uint64_t)szb;
                    }
                    vm_gather(P->pid,va,nm,szb,rvp,rok);
                    /* Re-read the same BCBs' iopage headers too (addresses unchanged) */
                    vm_gather(P->pid,maddr,nm,16,rhd,rok+nm);
                    int replaced=0;
                    for(int j=0;j<nm;j++){
                        PG.recheck++;
                        if(!rok[j]||!rok[nm+j]){ PG.mismatch++; continue; }
                        const unsigned char*nb=rvp+(size_t)j*szb;
                        int32_t npid; int16_t nvid; uint32_t nfl; uint64_t nol;
                        memcpy(&npid,nb+o_vpid,4); memcpy(&nvid,nb+o_vpid+4,2);
                        memcpy(&nfl,nb+o_flags,4); memcpy(&nol,nb+o_olsa,8);
                        uint64_t lsa; int32_t hp; int16_t hv;
                        memcpy(&lsa,rhd+(size_t)j*16,8); memcpy(&hp,rhd+(size_t)j*16+8,4); memcpy(&hv,rhd+(size_t)j*16+12,2);
                        if(hp==npid && hv==nvid && npid>=0 && nvid>=0){
                            /* It was a timing gap: the record becomes the new page as a whole -
                               identity, flags, zone and LSAs from the same re-read */
                            pgrec_t*r=&PG.rec[midx[j]];
                            int nz=(int)((nfl&PGBUF_ZONE_MASK)>>16);
                            pg_zone_add(r->zone,-1); pg_zone_add(nz,+1);
                            r->pageid=npid; r->volid=nvid; r->flags=nfl; r->zone=(int16_t)nz;
                            r->oldest_lsa=nol; r->page_lsa=lsa; r->hdr_ok=1;
                            replaced++;
                        } else PG.mismatch++;      /* Still wrong after the recheck: a genuine mismatch */
                    }
                    if(replaced) pg_recount();
                } else for(int j=0;j<nm;j++) PG.mismatch++;
            } else PG.mismatch=PG.mismatch_raw;
        }
        /* Judge on the mismatches remaining after the recheck; a wrong offset cannot pass a
           recheck, so safety is unchanged.  Above the threshold the snapshot is not
           discarded but degraded to a verified-records-only mode with the reason kept
           (dirty state and zone distribution are valid from vpid alone). */
        /* Unread entries count toward the gate too.  Without that, a wrong iopage_buffer
           offset failing all 32,768 reads still passed as healthy (a deliberate
           corruption test returned enabled=1).  Reading no header at all empties
           the LSA axis, so it is treated as an offset error. */
        /* Proportions of the headers actually read, at any sample size: a hot/warm sample
           on a large pool can be small, and a wrong offset fails every one of them. */
        if(PG.unread*2>PG.nhdr){ PG.ok=0;
            snprintf(PG.why,sizeof PG.why,
                     g_ascii?"page headers unreadable %d/%d — iopage offset mismatch suspected"
                            :"페이지 헤더 읽기 실패 %d/%d — iopage 오프셋 불일치 의심",PG.unread,PG.nhdr);
            return; }
        if(PG.mismatch*20>PG.nhdr){
            if(PG.mismatch*2>PG.nhdr || PG.unread+PG.mismatch>=PG.nhdr){ PG.ok=0;
                snprintf(PG.why,sizeof PG.why,
                         g_ascii?"page header vs BCB mismatch %d/%d after recheck — offset mismatch suspected"
                                :"재검 후에도 페이지 헤더·BCB 불일치 %d/%d — 오프셋 불일치 의심",PG.mismatch,PG.nhdr);
                return; }
          if(full){   /* Records are verified one by one only when every header was read */
            PG.degraded=1; PG.nrec_all=PG.nrec;
            /* Verified records only, as the mode says: the unverified are dropped and the
               record totals recounted.  Zone counts stay - they cover every BCB, empty ones
               included, and come from the BCB itself. */
            int m=0;
            for(int k=0;k<PG.nrec;k++) if(PG.rec[k].hdr_ok){
                if(m!=k){ PG.rec[m]=PG.rec[k]; if(pg_bidx && k<pg_bidx_cap) pg_bidx[m]=pg_bidx[k]; }
                m++; }
            PG.nrec=m;
            pg_recount();
          }
        }
    }
    PG.ok=1;
    if(PG.degraded)
        snprintf(PG.why,sizeof PG.why,
                 g_ascii?"%d slots; %d/%d unverified after recheck — counted from verified records only"
                        :"슬롯 %d개; 재검 후 %d/%d 미검증 — 검증된 레코드만 집계",
                 nbuf,PG.mismatch,PG.nrec_all);
    else if(PG.recheck>0)
        snprintf(PG.why,sizeof PG.why,
                 g_ascii?"%d page slots checked (%d rechecked, %d residual)"
                        :"페이지 슬롯 %d개 확인 (재검 %d건, 잔여 %d건)",
                 nbuf,PG.recheck,PG.mismatch);
    else
        snprintf(PG.why,sizeof PG.why,g_ascii?"%d page slots checked (%d headers%s)":"페이지 슬롯 %d개 확인 (헤더 %d개%s)",
                 nbuf,PG.nhdr,full?(g_ascii?", all":", 전수"):(g_ascii?" sampled":" 표본"));
    PG.ms=now_ms()-t0;
    PG.scan_seq++; PG.ok_last=PG.ok; if(PG.large) PG.scan_ms_full=PG.ms;
    { struct timeval tv; gettimeofday(&tv,NULL); PG.when.tv_sec=tv.tv_sec; PG.when.tv_nsec=tv.tv_usec*1000L; }
    /* Order matters: pgbuf_dump overwrites the --bcb-dump file, so a one-shot run must
       read the turnover baseline BEFORE that, or the baseline becomes this very frame
       and turnover is always 0. */
    (void)cap_turnover_from_dump();
    if(!PG.degraded) pgbuf_dump(P->db);   /* A degraded frame keeps the last verified snapshot on disk */
    else snprintf(g_bcb_why,sizeof g_bcb_why,g_ascii?"degraded frame (unverified headers) - previous snapshot kept"
                                                    :"degraded 프레임(미검증 헤더) — 이전 스냅샷 유지");
}
/* Engine-maintained buffer pool aggregates: per-LRU-list zone counts and victim candidates,
   and the dirty count the flush control keeps.  The engine updates them for its own LRU and
   flush decisions (not perfmon), so they are read whether or not anyone watches, in O(1) -
   a few KB of memory the engine itself touches on every fix - instead of a BCB scan. */
static struct {
    int ok; char why[320];
    int nlist, nshared, nbuf;      /* LRU lists read (shared + private), buffers */
    long z1, z2, z3, vict;         /* Sums over the lists */
    long long dirty;
    double ms;
} AG;
static void pgbuf_agg(proc_t*P){
    memset(&AG,0,sizeof AG);
    if(!B.ok){ snprintf(AG.why,sizeof AG.why,"%s",g_ascii?"method B off":"방법 B 비활성"); return; }
    if(B.partial||g_bt->ag.sz_lru<=0){ snprintf(AG.why,sizeof AG.why,"%s",
        g_ascii?"table lacks the aggregate offsets":"표에 집계 오프셋 없음"); return; }
    double t0=now_ms();
    unsigned long pool=B.base+B.sym[S_PGBUF];
    int32_t nbuf=rd_i32(P->pid,pool+g_bt->off_pgbuf_nbuf);
    int32_t nsh=rd_i32(P->pid,pool+g_bt->ag.nlru), npv=rd_i32(P->pid,pool+g_bt->ag.quota+g_bt->ag.npriv);
    uint64_t lists=0; int64_t dirty=-1;
    int okp = vmread(P->pid,pool+g_bt->ag.lrulist,&lists,8)==8 && lists;
    int okd = vmread(P->pid,pool+g_bt->ag.monitor+g_bt->ag.dirties,&dirty,8)==8;
    int n=nsh+npv;
    if(!okp || !okd || nbuf<=0 || nsh<=0 || npv<0 || n>nbuf || dirty<0 || dirty>nbuf){
        snprintf(AG.why,sizeof AG.why,g_ascii?"aggregates fail the sanity check (lists %d+%d, dirty %lld)"
                                              :"집계값 검증 실패 (리스트 %d+%d, dirty %lld)",nsh,npv,(long long)dirty);
        return; }
    size_t sz=(size_t)g_bt->ag.sz_lru, need=sz*(size_t)n;
    static unsigned char*buf=NULL; static size_t cap=0;
    if(need>cap){ free(buf); buf=malloc(need); cap=buf?need:0; if(!buf){ snprintf(AG.why,sizeof AG.why,"malloc"); return; } }
    if(vmread(P->pid,(unsigned long)lists,buf,need)!=(ssize_t)need){
        snprintf(AG.why,sizeof AG.why,"%s",g_ascii?"LRU list array unreadable":"LRU 리스트 배열 읽기 실패"); return; }
    for(int i=0;i<n;i++){
        const unsigned char*b=buf+(size_t)i*sz; int32_t c1,c2,c3,v;
        memcpy(&c1,b+g_bt->ag.c1,4); memcpy(&c2,b+g_bt->ag.c2,4); memcpy(&c3,b+g_bt->ag.c3,4); memcpy(&v,b+g_bt->ag.vict,4);
        if(c1<0||c2<0||c3<0||v<0){ snprintf(AG.why,sizeof AG.why,g_ascii?"negative count in list %d":"리스트 %d 의 음수 개수",i); return; }
        AG.z1+=c1; AG.z2+=c2; AG.z3+=c3; AG.vict+=v;
    }
    if(AG.z1+AG.z2+AG.z3>nbuf){
        snprintf(AG.why,sizeof AG.why,g_ascii?"zone sums exceed the buffer (%ld > %d)":"존 합이 버퍼를 넘음 (%ld > %d)",
                 AG.z1+AG.z2+AG.z3,nbuf); return; }
    AG.nlist=n; AG.nshared=nsh; AG.nbuf=nbuf; AG.dirty=dirty; AG.ok=1; AG.ms=now_ms()-t0;
}
/* Formatting helper */
static const char* lsa_str(lsa_t l){ static char ring[6][32]; static int k=0; char*o=ring[k++%6];
    if(lsa_isnull(l)) snprintf(o,32,"-"); else snprintf(o,32,"%lld|%d",(long long)l.pageid,l.offset); return o; }

/* ---------------- Classify cub_server regions into six kinds ---------------- */
typedef struct {
    char name[20]; double mapped,rss; long long cfg; char grade;
    double prev_rss;            /* Previous-frame baseline (live) for the grow column */
} srg_t;
static srg_t SR[8]; static int NSR=0;
static double g_dyn_rss=0,g_dyn_mapped=0;

/* A .so is identified by a ".so" suffix or a ".so." substring; the older
   strstr(".so") and "/lib" tests matched .sock files and locale-archive. */
static int path_is_so(const char *p){
    size_t n=strlen(p);
    if(n>=3 && !strcmp(p+n-3,".so")) return 1;
    return strstr(p,".so.")!=NULL;
}
static int path_is_heap(const char *p){ return !strcmp(p,"[heap]"); }

static void classify_regions(proc_t *P){
    double code_m=0,code_r=0,arena_m=0,stk_m=0,stk_r=0;
    double oth_m=0,oth_r=0;   /* Executable segments, message catalogues, /dev/shm and the like */
    region_t *db=NULL,*lg=NULL,*prev=NULL;
    long long cfg_db=prm_get("data_buffer_size"), cfg_lg=prm_get("log_buffer_size");
    /* Match large anonymous rw regions against the configured size (100-140%); [heap] is not a buffer candidate */
    for(int i=0;i<P->nreg;i++){
        region_t *r=&P->reg[i];
        if(r->path[0]||!strchr(r->perm,'w')) continue;
        double sz=(double)(r->end-r->start);
        if(!db && cfg_db>0 && sz>=cfg_db && sz<=cfg_db*1.4) { db=r; continue; }
        if(!lg && cfg_lg>0 && sz>=cfg_lg && sz<=cfg_lg*1.4) { lg=r; continue; }
    }
    for(int i=0;i<P->nreg;i++){
        region_t *r=&P->reg[i];
        double sz=(double)(r->end-r->start);
        int guard = prev && prev->path[0]==0 && !strchr(prev->perm,'w') && !strchr(prev->perm,'r');
        if(r->path[0] && path_is_so(r->path)){
            code_m+=sz; code_r+=r->rss_kb*1024.0; }
        else if(r->path[0]==0 && !strchr(r->perm,'w') && !strchr(r->perm,'r')) arena_m+=sz;
        else if(r->path[0]==0 && strchr(r->perm,'r') && strchr(r->perm,'w')
                && guard && (prev->end-prev->start)<(1UL<<20)
                && sz>=8384512.0 && sz<=8388608.0){
            r->is_stack=1;                                                 /* pthread stacks */
            stk_m+=sz; stk_r+=r->rss_kb*1024.0;
        }
        else if(!strncmp(r->path,"[stack",6)){ stk_m+=sz; stk_r+=r->rss_kb*1024.0; }
        else if(r->path[0] && !path_is_heap(r->path)){
            oth_m+=sz; oth_r+=r->rss_kb*1024.0; }
        prev=r;
    }
    /* Dynamic heap = unmatched anonymous rw + [heap] - pattern stacks.
       Excluding [heap] (the glibc main arena) leaves tens of MB unattributed.
       Stacks are excluded by the is_stack marking rather than by size, so a genuine
       8MB heap block without a guard is not dropped. */
    double dyn_m=0,dyn_r=0;
    for(int i=0;i<P->nreg;i++){
        region_t *r=&P->reg[i];
        int anon = (r->path[0]==0 || path_is_heap(r->path));
        if(!anon||!strchr(r->perm,'w')||!strchr(r->perm,'r')) continue;
        if(r==db||r==lg||r->is_stack) continue;
        dyn_m+=(double)(r->end-r->start); dyn_r+=r->rss_kb*1024.0;
    }
    g_reg_db=db; g_reg_lg=lg;   /* Exposed so the dynamic total excludes them */
    NSR=0;
    #define PUT(nm_,mp_,rs_,cfg_,gr_) do{ if(NSR<8){ srg_t*x=&SR[NSR++]; \
        snprintf(x->name,sizeof x->name,"%s",nm_); x->mapped=(mp_); x->rss=(rs_); \
        x->cfg=(cfg_); x->grade=(gr_);} }while(0)
    if(db) PUT("data_buffer",(double)(db->end-db->start),db->rss_kb*1024.0,cfg_db,'M');
    if(lg) PUT("log_buffer", (double)(lg->end-lg->start),lg->rss_kb*1024.0,cfg_lg,'M');
    /* When A/B explain more than half the dynamic heap the grade is "estimated", not
       "unattributed": leaving the largest row permanently at '?' reads as no
       decomposition at all. */
    PUT("dynamic-heap",dyn_m,dyn_r,-1,'U');
    PUT("thread stacks",stk_m,stk_r,-1,'R');
    PUT("code (.so)",code_m,code_r,-1,'M');
    if(oth_m>0) PUT("bin/shm etc",oth_m,oth_r,-1,'M');
    PUT("glibc arena",arena_m,0,-1,'R');
    #undef PUT
    g_dyn_rss=dyn_r; g_dyn_mapped=dyn_m;
}

/* Look up region values by name.  SR[] is filled conditionally (if(db)/if(lg)), so
   indexing SR[0]/SR[1]/SR[2] silently picks up another region's value when
   data_buffer was not detected - the tree, dashboard, plots and terse all go
   through this helper so they cannot disagree. */
static const srg_t *sr_find(const char *nm){
    for(int i=0;i<NSR;i++) if(!strcmp(SR[i].name,nm)) return &SR[i];
    return NULL;
}
static double sr_rss(const char *nm){ const srg_t*x=sr_find(nm); return x?x->rss:0.0; }
static double sr_mapped(const char *nm){ const srg_t*x=sr_find(nm); return x?x->mapped:0.0; }

/* Bumped whenever the target process changes (switch or restart).  Baselines kept
   across frames - engine counters, the turnover page set, per-item deltas - belong
   to one process; compared against another they yield nonsense rates. */
static unsigned g_inst_gen=0;
/* Collection segment: bumped when measurement stops being continuous - the target changes,
   or the user stops collection (Ctrl-Z, pause, help).  History, frame-to-frame baselines and
   trackers belong to one segment, and no frame spans two: data from both sides of a stop is
   not one data set. */
static unsigned g_seg_gen=0;
static volatile sig_atomic_t g_ustop_n=0;    /* User stops so far: SIGCONT, unpause, help closed */
static sig_atomic_t g_ustop_seen=0;
/* Must be called when switching instances.  g_reg_db/g_reg_lg point into P->reg[],
   so keeping the previous instance's pointers makes the dynamic total exclude the wrong
   region and double-count.  SR[]/NSR, B and A are cleared too, so the next frame
   recomputes against the new target. */
static void reset_instance_state(void){
    g_inst_gen++; g_seg_gen++; g_pid_gen++;
    g_hprevn=0;                        /* heap item deltas */
    g_perm_limited=0; g_perm_why[0]=0; /* the next target's access is judged afresh */
    g_reg_db=NULL; g_reg_lg=NULL;
    NSR=0; memset(SR,0,sizeof SR);
    g_dyn_rss=0; g_dyn_mapped=0;
    memset(&B,0,sizeof B);
    memset(&AG,0,sizeof AG);
    /* Buffer pool snapshot: the arrays are kept, what they describe is not.  A large
       buffer is read every few frames, so a stale ok would show the previous
       instance's residency, dirty and log values under the new name. */
    { pgrec_t *rec=PG.rec; int cap=PG.reccap;
      memset(&PG,0,sizeof PG); PG.rec=rec; PG.reccap=cap; }
    /* cubrid.conf parameters are per database too ([@db] sections differ), so without
       clearing them the previous instance's values make a verdict like "configured
       512M" entirely wrong. */
    g_nprm=0;
}

/* Re-grade dynamic-heap once the decomposition is in (called after method_b) */
static void grade_dynamic_heap(void){
    for(int i=0;i<NSR;i++){
        if(strcmp(SR[i].name,"dynamic-heap")) continue;
        if(SR[i].rss<=0) return;
        double explained = B.total;
        if(explained>=SR[i].rss*0.5) SR[i].grade='E';   /* Estimated */
    }
}

/* ---------------- Process tier totals ---------------- */
/* tier_t is defined earlier in the file, for the combined enum_instances pass */
/* Cumulative process CPU ticks; the delta comes from the previous value found by name and pid. */
/* Grows with the processes seen; an entry whose process was not met in the last walk is
   dropped (tcpu_sweep), so the table holds the live processes and no process is left out. */
typedef struct { int pid; unsigned long ticks; int seen; } tcpu_t;
static tcpu_t *g_tcpu=NULL; static int g_tcpun=0, g_tcpucap=0;
/* CPU ticks (fields 14+15) and RSS (field 24, pages) from one /proc/PID/stat read */
static unsigned long proc_ticks(int pid,double *rss){
    char p[64],b[512]; snprintf(p,sizeof p,"/proc/%d/stat",pid);
    if(rss) *rss=0;
    if(read_file(p,b,sizeof b)<=0) return 0;
    char *q=strrchr(b,')'); if(!q) return 0;
    q+=2; unsigned long f[24]; int n=0;
    char *tok=strtok(q," ");
    while(tok&&n<24){ f[n++]=strtoul(tok,NULL,10); tok=strtok(NULL," "); }
    if(rss && n>21){ static long pg=0; if(!pg){ pg=sysconf(_SC_PAGESIZE); if(pg<=0) pg=4096; }
                     *rss=(double)f[21]*(double)pg; }
    return (n>12)? f[11]+f[12] : 0;
}
static double stat_rss(int pid){ double r; (void)proc_ticks(pid,&r); return r; }
/* The table is kept in pid order and searched by bisection; a new pid is inserted in place */
static double tcpu_delta(int pid,double dt,double *rss){
    unsigned long cur=proc_ticks(pid,rss);
    int lo=0, hi=g_tcpun;
    while(lo<hi){ int m=(lo+hi)/2; if(g_tcpu[m].pid<pid) lo=m+1; else hi=m; }
    if(lo>=g_tcpun || g_tcpu[lo].pid!=pid){        /* First sight: the baseline, no delta yet */
        if(g_tcpun==g_tcpucap){
            int nc=g_tcpucap?g_tcpucap*2:64;
            tcpu_t *t=realloc(g_tcpu,sizeof *t*(size_t)nc);
            if(!t) return 0.0;
            g_tcpu=t; g_tcpucap=nc;
        }
        memmove(&g_tcpu[lo+1],&g_tcpu[lo],sizeof *g_tcpu*(size_t)(g_tcpun-lo));
        g_tcpu[lo].pid=pid; g_tcpu[lo].ticks=cur; g_tcpu[lo].seen=1; g_tcpun++;
        return 0.0; }
    unsigned long prev=g_tcpu[lo].ticks;
    g_tcpu[lo].ticks=cur; g_tcpu[lo].seen=1;
    long hz=sysconf(_SC_CLK_TCK); if(hz<=0) hz=100;
    if(cur<prev||dt<=0) return 0.0;
    return (double)(cur-prev)/hz/dt;      /* Core count */
}
/* After a walk: keep the processes met in it */
static void tcpu_sweep(void){
    int m=0; for(int i=0;i<g_tcpun;i++) if(g_tcpu[i].seen){ g_tcpu[i].seen=0; g_tcpu[m++]=g_tcpu[i]; }
    g_tcpun=m;
}
static void scan_tiers(tier_t *T,double dt){
    /* The work is done by enum_instances pass 2 (one /proc walk and one rollup per
       frame).  This wrapper skips a second walk when the same frame already counted. */
    if(g_enum_T==T) return;            /* The previous enum filled this T */
    memset(T,0,sizeof *T);
    g_enum_T=T; g_enum_dt=dt;
    enum_instances();
    g_enum_T=NULL;
}

/* ---- cubrid.conf parameters that consume memory ----
 * Shows the configured value regardless of whether usage is observable; tuning
 * advice and thresholds need that baseline.
 * obs: 1 if this tool also measures actual usage, 0 if it knows only the setting. */
/* unit: 'B' bytes (human), 'P' pages, 'N' a plain count */
typedef struct { const char *name, *desc, *desc_en; int obs; char unit; } prmrow_t;
static const prmrow_t PRMROWS[]={
 {"data_buffer_size",              "페이지 버퍼",                "page buffer",                1,'B'},
 {"log_buffer_size",               "로그 버퍼",                  "log buffer",                 1,'B'},
 {"double_write_buffer_size",      "DWB(이중 쓰기 버퍼)",        "DWB (double write buffer)",  0,'B'},
 {"sort_buffer_size",              "정렬 버퍼 — 세션·연산당",    "sort buffer — per session·op",0,'B'},
 {"temp_file_memory_size_in_pages","임시파일 메모리",            "temp-file memory",           0,'P'},
 {"max_plan_cache_entries",        "질의계획 캐시 항목 수",      "plan cache entries",         0,'N'},
 {"max_query_cache_entries",       "질의결과 캐시 항목 수",      "query cache entries",        0,'N'},
 {"max_filter_pred_cache_entries", "필터술어 캐시 항목 수",      "filter pred cache entries",  0,'N'},
 {"index_scan_oid_buffer_size",    "인덱스 스캔 OID 버퍼",       "index scan OID buffer",      0,'B'},
 {"index_scan_key_buffer_size",    "인덱스 스캔 키 버퍼",        "index scan key buffer",      0,'B'},
 {"thread_stacksize",              "스레드 스택 — HA 경로 전용", "thread stack — HA path only",0,'B'},
 {"max_clients",                   "최대 접속 수",               "max clients",                0,'N'},
};
#define NPRMROWS ((int)(sizeof PRMROWS/sizeof PRMROWS[0]))

/* ---------------- Render: drill-down tree ---------------- */
static const char *GR(char g){
    switch(g){ case 'M': return "●"; case 'R': return "◐";
               case 'E': return "◌"; default: return "?"; }
}

#define RCHAR_MIN (64.0*1024.0)   /* Floor below which the absorb denominator is meaningless (64KB/s) */
/* ---- Live dashboard: canvas / braille / TUI ----
 * Six-dot glyph gauges (foreground = the box's metric, background = a second share such as
 * dirty), four value columns alloc|rss|conf|chg, ~3 minute window, 24h time ruler.
 */
#define CV_W 120   /* Ceiling of 114 plus slack */
/* Plot mode is the tallest: nine panels (53 rows) plus the status line and the
   bottom ruler and key hints.  Overflow silently drops whole panels from the
   bottom, so leave margin (at 56 three new panels vanished). */
#define CV_H 80
typedef struct { int w,h; char ch[CV_H][CV_W][5]; char co[CV_H][CV_W][20]; } canvas_t;
static canvas_t CV;

static int g_argc=0; static char **g_argv=NULL;
static double g_interval=0.5;   /* The sample window, for display */
/* g_ascii is defined earlier in the file; method_b's reason strings reference it */
/* Hangul label to English; the tree and terse outside the canvas are unaffected */
static const char* L(const char*ko){
    static const char *M[][2]={
      {"OS 컨텍스트","os context"},
      /* Symmetric with the process box's "(PSS)": the two boxes' axes (apportioned shared
         vs full physical) differ in the title itself, so the gap between the total and
         PSS is less likely to be misread. */
      {"RSS · 서버","RSS · server"},
      {"성장 추이","growth"},{"RSS · 프로세스","RSS · processes"},
      {"I/O · cub_server + 최다사용 장치","i/o · cub_server + busiest device"},
      {"동적 메모리 상세 (B) — 자동 갱신","dynamic heap · method B — per frame"},
      {"호스트 CPU · 사용 코어 · 포화 코어 · load","host CPU · busy cores · saturated · load"},
      {"I/O 대기 · iowait · D스레드 · blkio","I/O wait · iowait · D threads · blkio"},
      {"티어별 CPU(코어) · master · pl · broker · CAS","per-tier CPU (cores) · master · pl · broker · CAS"},
      {"포화=95% 이상","saturated = >=95%"},
      {"판정문 근거","verdict evidence"},
      {"server 는 위 활동 패널","server: activity panel above"},
      {"디스크 읽기","disk read"},{"디스크 쓰기","disk write"},{"캐시 흡수율","cache absorb"},{"볼륨 페이지캐시","volume page cache"},
      {"유휴","idle"},{"B 합계(정확)","B total (exact)"},{"A 합계(청크)","A total (chunks)"},{"A 합계(청크·일부)","A total (chunks, partial)"},
      {"미설명 잔여","unexplained"},{"과대계상(free+비상주)","overcount(free+nonres)"},
      {"방법 B 비활성","method B disabled"},{"힙 순회 중…","walking heap…"},
{"장치","dev"},{"쓰기","write"},{"누적","total"},{"읽기","read"},
      {"전체 메모리 대비 점유","share of system RAM"},
      {"상주 구성 · server = data_buffer + log_buffer + dynamic-heap + 기타","resident composition"},
      {"할당(VmSize) vs 상주(VmRSS) · 간격 = 예약만 하고 안 쓰는 분량","reserved (VmSize) vs resident (VmRSS)"},
      {"활동 · CPU · 폴트/s","activity · cpu · faults/s"},
      {"디스크 I/O","disk i/o"},
      {"I/O 포화 · 장치 사용률 · 캐시 흡수율","i/o saturation · device util · cache absorb"},
      {"표본이 부족합니다 — 선을 그리려면 2개 이상 필요","not enough samples — need 2+ to draw a line"},
      {"측정 불가 구간은 0 으로 표기","unmeasurable spans drawn as 0"},
      {"계열별 정규화(단위 상이)","normalized per series"},
      {"메이저=디스크","major=disk"}};
    if(!g_ascii) return ko;
    for(unsigned i=0;i<sizeof M/sizeof M[0];i++) if(!strcmp(ko,M[i][0])) return M[i][1];
    return ko;
}
static const int GRAD[11]={46,82,118,154,190,226,220,214,208,202,196};
#define HOTBG 18
static int grad_of(double f){ int i=(int)(f*10.0); if(i<0)i=0; if(i>10)i=10; return GRAD[i]; }

/* Byte length of one UTF-8 character.  A sequence counts as multi-byte only when its
   continuation bytes are really there: labels cut with %.Ns in bytes can end in a
   split syllable, and the terminating NUL must not be stepped over. */
static int u8len(const char*s){
    unsigned char c=(unsigned char)*s;
    int n = c<0x80 ? 1 : (c>>5)==6 ? 2 : (c>>4)==14 ? 3 : (c>>3)==30 ? 4 : 1;
    for(int k=1;k<n;k++) if(((unsigned char)s[k]&0xC0)!=0x80) return 1;
    return n;
}
static int u8cp(const char*s,int len){
    unsigned char c=(unsigned char)s[0];
    if(len==1) return c;
    if(len==2) return ((c&0x1f)<<6)|((unsigned char)s[1]&0x3f);
    if(len==3) return ((c&0x0f)<<12)|(((unsigned char)s[1]&0x3f)<<6)|((unsigned char)s[2]&0x3f);
    return ((c&0x07)<<18)|(((unsigned char)s[1]&0x3f)<<12)|(((unsigned char)s[2]&0x3f)<<6)|((unsigned char)s[3]&0x3f);
}
/* Wide-character test (Hangul, CJK, fullwidth symbols): the core of canvas width arithmetic */
static int u8wide(int cp){
    return (cp>=0x1100&&cp<=0x115F)||(cp>=0x2E80&&cp<=0xA4CF)||
           (cp>=0xAC00&&cp<=0xD7A3)||(cp>=0xF900&&cp<=0xFAFF)||
           (cp>=0xFE30&&cp<=0xFE6F)||(cp>=0xFF00&&cp<=0xFF60)||
           (cp>=0xFFE0&&cp<=0xFFE6)||(cp>=0x20000&&cp<=0x3FFFD);
}
static int disp_w(const char*s){
    int w=0; while(*s){ int l=u8len(s); w+=u8wide(u8cp(s,l))?2:1; s+=l; } return w;
}
static void cv_init(int w,int h){
    if(w>CV_W)w=CV_W;
    if(h>CV_H)h=CV_H;
    CV.w=w; CV.h=h;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){ CV.ch[y][x][0]=' '; CV.ch[y][x][1]=0; CV.co[y][x][0]=0; }
}
static void cv_put(int x,int y,const char*s,const char*col){
    if(y<0||y>=CV.h) return;
    int cx=x;
    while(*s && cx<CV.w){
        int l=u8len(s); int wd=u8wide(u8cp(s,l))?2:1;
        if(cx+wd>CV.w) break;
        if(cx>=0){
            /* A wide character marks its second cell consumed, and flush skips it.  If a
               later half-width glyph overwrites only the first cell, the second
               stays marked, drops out of flush and shifts the row by one cell.
               So writing half-width revives a consumed cell as a space. */
            if(wd==1 && cx+1<CV.w && !CV.ch[y][cx+1][0]){
                CV.ch[y][cx+1][0]=' '; CV.ch[y][cx+1][1]=0; CV.co[y][cx+1][0]=0;
            }
            if(l==1 && (unsigned char)*s>=0x80){ CV.ch[y][cx][0]='?'; CV.ch[y][cx][1]=0; }   /* a broken sequence */
            else { int n=l<4?l:4; memcpy(CV.ch[y][cx],s,n); CV.ch[y][cx][n]=0; }
            if(col) snprintf(CV.co[y][cx],sizeof CV.co[y][cx],"%s",col); else CV.co[y][cx][0]=0;
            if(wd==2 && cx+1<CV.w){ CV.ch[y][cx+1][0]=0; CV.co[y][cx+1][0]=0; }  /* Cell consumed by a wide character */
        }
        s+=l; cx+=wd;
    }
}
/* Truncate to display width n with no padding, so a title cannot overrun the box corner */
static const char* cutf(const char*s,int n){
    static char ring[4][256]; static int k=0;
    char*o=ring[k++&3]; int acc=0; size_t p=0;
    while(*s && p+4<sizeof ring[0]){
        int l=u8len(s), wd=u8wide(u8cp(s,l))?2:1;
        if(acc+wd>n) break;
        for(int i=0;i<l&&s[i];i++) o[p++]=s[i];
        s+=l; acc+=wd;
    }
    o[p]=0; return o;
}
/* Build a left- or right-aligned string by display width */
static const char* padf(const char*s,int n,int right){
    static char ring[8][256]; static int k=0;
    char*o=ring[k++&7]; int w=disp_w(s);
    if(w>=n){ snprintf(o,256,"%s",s); return o; }
    int sp=n-w; if(sp>200) sp=200;
    if(right){ memset(o,' ',sp); snprintf(o+sp,256-sp,"%s",s); }
    else { int L=snprintf(o,256,"%s",s); if(L>200)L=200; memset(o+L,' ',sp); o[L+sp]=0; }
    return o;
}
static void cv_box2(int x,int y,int w,int h,const char*title,const char*rtitle){
    const char*bc="38;5;240";
    cv_put(x,y,"╭",bc); cv_put(x+w-1,y,"╮",bc);
    for(int i=1;i<w-1;i++){ cv_put(x+i,y,"─",bc); cv_put(x+i,y+h-1,"─",bc); }
    cv_put(x,y+h-1,"╰",bc); cv_put(x+w-1,y+h-1,"╯",bc);
    for(int yy=y+1;yy<y+h-1;yy++){ cv_put(x,yy,"│",bc); cv_put(x+w-1,yy,"│",bc); }
    int budget=w-7;                      /* The limit at which border + title + border stays inside the corner */
    if(rtitle&&*rtitle&&budget>0){       /* Right-hand title, set apart at the right of the border */
        const char*rt=cutf(rtitle,budget);
        int rw=disp_w(rt);
        if(rw>0){
            int rx=x+w-2-(rw+4);
            cv_put(rx,y,"┤ ",bc); cv_put(rx+2,y,rt,"38;5;250");
            cv_put(rx+2+rw,y," ├",bc);
            budget-=rw+5;                /* Deduct the budget so it cannot overlap the left title */
        }
    }
    if(title&&*title&&budget>0){
        const char*tt=cutf(title,budget);
        int tw=disp_w(tt);
        if(tw>0){
            cv_put(x+2,y,"┤ ",bc); cv_put(x+4,y,tt,"1;36");
            cv_put(x+4+tw,y," ├",bc);
        }
    }
}
static void cv_box(int x,int y,int w,int h,const char*title){ cv_box2(x,y,w,h,title,NULL); }
/* Six-dot gauge: foreground is the box's metric (green to red), background a second share (complementary blue) */
/* Overlay chg on the gauge.  The background already carries the second share, so an
   orange background would mix two signals in one channel; instead the growth
   since the previous frame is painted orange at the end of the filled bar, where
   it is visible in place and does not collide with the background. */
static void cv_meter3(int x,int y,int w,double frac,double share,double dfrac){
    if(y<0||y>=CV.h) return;
    if(frac<0)frac=0;
    if(frac>1)frac=1;
    int n=(int)(frac*w+0.5), hn=0;
    if(n==0 && frac>0 && w>0) n=1;   /* A non-zero value occupying no cells reads as "none" */
    if(share>0){
        if(share>1)share=1;
        hn=(int)(share*w+0.5);
        if(hn<1)hn=1;
    }
    /* Growth range [dn, n); at least one cell, so small changes stay visible */
    int dn=n;
    if(dfrac>0 && n>0){
        double df=dfrac>1?1:dfrac;
        int dw2=(int)(df*w+0.5); if(dw2<1) dw2=1; if(dw2>n) dw2=n;
        dn=n-dw2;
    }
    char col[24];
    for(int i=0;i<w;i++){
        if(x+i<0||x+i>=CV.w) continue;
        const char*bg=(i<hn)?";48;5;18":"";
        if(i<n){
            if(i>=dn) snprintf(col,sizeof col,"1;38;5;214%s",bg);   /* Growth in orange */
            else      snprintf(col,sizeof col,"38;5;%d%s",grad_of((double)i/(w>1?w-1:1)),bg);
            cv_put(x+i,y,"⠿",col);
        }
        else   { snprintf(col,sizeof col,"38;5;236%s",bg); cv_put(x+i,y,"⠤",col); }
    }
}
static void cv_meter(int x,int y,int w,double frac){
    cv_meter3(x,y,w,frac,0,0);
}
/* Per-core usage bars, sorted busiest first.  A total gauge cannot tell "12
   cores' worth spread over 12" from "6 saturated", and the latter is the
   bottleneck; sorted, the width of the dark run is the saturated core count.
   With more cores than columns a cell takes the maximum, not the average -
   averaging would hide the saturation this view exists to show. */
/* Braille 2x4 line chart, resampling the whole window to the x resolution */
static const int BRD[2][4]={{0x01,0x02,0x04,0x40},{0x08,0x10,0x20,0x80}};
/* Up to CV_NS series.  The caller supplies colours as palette indices (38;5;N) so the
   dashboard and plots draw the same metric in the same colour. */
#define CV_NS 9   /* Series limit including overlays.  Panels 3 and 4 gained OS cache, majflt and D
                     threads, and 7 truncated silently - leave margin, since overflow
                     drops the later series. */
static void cv_chart_n(int x,int y,int w,int h,const double*const*v,const int*colv,int ns,
                       int n,double vmax){
    static unsigned char cell[16][CV_W]; static unsigned char ccol[16][CV_W];
    if(h>16)h=16;
    if(w>CV_W)w=CV_W;
    if(w<2||h<1) return;                         /* Divides by px-1, so w must be at least 2 */
    if(ns>CV_NS) ns=CV_NS;
    for(int i=0;i<h;i++) for(int j=0;j<w;j++){ cell[i][j]=0; ccol[i][j]=0; }
    if(n<1||vmax<=0||ns<1) return;
    int px=w*2, py=h*4;
    for(int s=0;s<ns;s++){
        const double*vv=v[s]; if(!vv) continue;
        int lx=-1,ly=-1;
        for(int i=0;i<px;i++){
            /* Peak-preserving downsample: with n>px each pixel takes the maximum of its range.
               Even sampling would lose majflt and I/O spikes with probability
               (n/px-1)/(n/px). */
            double val;
            if(n>px){
                int a=(int)((double)i*n/px), b=(int)((double)(i+1)*n/px);
                if(b>n) b=n;
                if(b<=a) b=a+1;
                val=vv[a];
                for(int k=a+1;k<b;k++) if(vv[k]>val) val=vv[k];
            }else{
                int idx=(n>1)?(int)((double)i*(n-1)/(px-1)+0.5):0;
                val=vv[idx];
            }
            /* Written as "not >= 0" so NaN is caught too (every comparison with NaN is
               false): a NaN from a recording would otherwise become INT_MIN below and the
               line walk would run for ~2^31 steps. */
            if(!(val>=0)) val=0;
            double f=val/vmax; if(!(f>=0)) f=0; if(f>1) f=1;
            int yy=(int)((py-1)*(1.0-f)+0.5);
            if(lx>=0){                                   /* Bresenham line */
                int x0=lx,y0=ly,x1=i,y1=yy;
                        int dx=x1-x0, dy=y1>y0?y1-y0:y0-y1, sy=y1>y0?1:-1, err=dx-dy;
                while(1){
                    if(x0>=0&&x0<px&&y0>=0&&y0<py){ cell[y0/4][x0/2]|=BRD[x0%2][y0%4]; ccol[y0/4][x0/2]=(unsigned char)(s+1); }
                    if(x0==x1&&y0==y1) break;
                    int e2=2*err;
                    if(e2>-dy){ err-=dy; x0++; }
                    if(e2<dx){ err+=dx; y0+=sy; }
                }
            }
            lx=i; ly=yy;
        }
    }
    char buf[8],col[16];
    for(int cy=0;cy<h;cy++) for(int cx=0;cx<w;cx++){
        if(!cell[cy][cx]) continue;
        int cp=0x2800+cell[cy][cx];
        buf[0]=(char)(0xE0|(cp>>12)); buf[1]=(char)(0x80|((cp>>6)&0x3F)); buf[2]=(char)(0x80|(cp&0x3F)); buf[3]=0;
        int ci=ccol[cy][cx]-1; if(ci<0)ci=0; if(ci>=ns)ci=ns-1;
        snprintf(col,sizeof col,"38;5;%d",colv[ci]);
        cv_put(x+cx,y+cy,buf,col);
    }
}
/* Write the canvas out.  Each row is placed absolutely (ESC[y;1H) with no newline:
   descending by newline scrolls when the canvas is taller than the terminal, which
   pushes the top row off and back on, flickering.
   maxrows limits drawing (0 = all); returns the number of rows not drawn. */
static int cv_flush_rows(int use_color,int maxrows){
    char line[CV_W*40];
    int lim = (maxrows>0 && maxrows<CV.h) ? maxrows : CV.h;
    for(int y=0;y<lim;y++){
        int p=0; const char*cur=NULL;
        for(int x=0;x<CV.w;x++){
            if(!CV.ch[y][x][0]) continue;                 /* Cell consumed by a wide character */
            const char*c=CV.co[y][x][0]?CV.co[y][x]:NULL;
            if(use_color && c!=cur){
                if(cur) p+=snprintf(line+p,sizeof line-p,"\033[0m");
                if(c)   p+=snprintf(line+p,sizeof line-p,"\033[%sm",c);
                cur=c;
            }
            p+=snprintf(line+p,sizeof line-p,"%s",CV.ch[y][x]);
            if((size_t)p>sizeof line-64) break;
        }
        if(use_color&&cur) p+=snprintf(line+p,sizeof line-p,"\033[0m");
        printf("\033[%d;1H%s\033[K",y+1,line);
    }
    return CV.h-lim;
}

typedef struct {
    double rb,wb,ri,wi,absorb,rchar,blkpct,devutil,devr; char devnm[32];
    double minflt,majflt;          /* Faults per second; majflt is a direct signal of swapping and cache misses */
    double cpu_pct;                /* Server CPU percentage, one core = 100% */
    int ncpu;                      /* Host core count, for showing the ceiling */
    int absorb_ok;
    unsigned long blk_ticks;       /* Cumulative delayacct_blkio_ticks summed over all threads */
    int d_thr, n_thr;              /* cub_server threads in D state over total threads.  wa is divided by the core
                                      count, so a saturated disk shows as 1-2% on 64
                                      cores; the D count is not diluted and is direct
                                      evidence of waiting. */
} iod_t;

#define CAP_BUDGET_MS 500.0              /* Collection budget; over it the frame is dropped and the screen says so */
typedef struct {
    /* Demand */
    double read_bps, write_bps, riops, wiops;   /* Attributed to the process, from /proc/pid/io */
    double dev_rbps, dev_wbps;                  /* Device-wide, diskstats sectors x 512 */
    double miss_pps;                            /* Buffer misses: pages loaded per second (-1 not measured) */
    char miss_src[12];                          /* "scan" (page-set difference), "perfmon" (engine ioread counter), "-" */
    double hit_pct;                             /* Hit ratio percentage; -1 means it cannot be computed */
    char   hit_src[16];                         /* "perfmon" | "turnover" | "-" */
    char   dev[32];                             /* The DB's busiest disk this frame; "" when unknown */
    char   dev_why[160];                        /* Why no DB disk was found */
    double turnover_pct;                        /* Share of resident pages replaced within the window */
    double readmit_pct;                         /* Share evicted and read back: the signal that the working set exceeds the buffer */
    double adm_frac;                            /* This frame's admissions over nbuf; -1 unknown.  At 0.5 and above, pages
                                                   admitted and evicted inside the frame are not seen: the window
                                                   counts too few evictions and the set difference saturates */
    /* Ceiling and latency */
    double await_ms, r_await_ms, w_await_ms;    /* Wait per device request */
    double qdepth;                              /* Mean queue depth (time_in_queue over elapsed) */
    double dev_iops, dev_util;                  /* Device-wide IOPS and utilization */
    double ceil_iops, ceil_bps, ceil_util;      /* Ceiling observed at saturation, for the current profile */
    time_t ceil_when; int ceil_valid; double ceil_qd;
    /* Ceiling matrix.  The IOPS ceiling for 4KB random and 128KB sequential differ
       by orders of magnitude, so a single remembered number misjudges the moment
       the profile changes.  Ceilings are kept per cell, keyed by (mean request
       size x read share), and the verdict uses the cell in force now. */
    int    prof_bs, prof_rw;                    /* The cell this frame falls in */
    double req_kb, read_ratio;                  /* Mean request size in KB and read share (0-1) */
    double headroom_pct;                        /* Current over ceiling; -1 means no ceiling observed */
    /* Engine statistics, when available */
    int    ps_ok; char ps_why[96];
    double pb_fetch_ps, pb_ioread_ps, pb_iowrite_ps, log_iowrite_ps, commit_ps;
    double ms; int over_budget;                 /* Collection time and whether the budget was exceeded */
} cap_t;
static cap_t CAP2;
/* Clear the values only, keeping the reasons and remembered ceilings, so a budget-exceeded or server-down frame shows no half values */
static void cap_reset_values(void){
    CAP2.miss_pps=-1; CAP2.hit_pct=-1; CAP2.turnover_pct=-1; CAP2.readmit_pct=-1; CAP2.adm_frac=-1;
    snprintf(CAP2.miss_src,sizeof CAP2.miss_src,"-");
    CAP2.await_ms=CAP2.r_await_ms=CAP2.w_await_ms=-1; CAP2.qdepth=-1;
    CAP2.headroom_pct=-1; CAP2.ps_ok=0;
    CAP2.dev_rbps=CAP2.dev_wbps=0; CAP2.req_kb=0; CAP2.read_ratio=0.5;
    snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"-");
}

/* ---- History ring buffer ----
   HIST_MAX 800 x 0.5s default interval = a window of about 400 seconds.
   Every field the plot panels read is filled only here - computing per screen
   would let the same metric differ between views. */
#define HIST_MAX 800
typedef struct {
    double t, srv, dyn, rbps, wbps, riops, majflt;   /* Existing fields; order preserved */
    /* Plot extension, holding both srv_rss and srv_pss: the composition total
       (db+lg+dyn+etc) must be over RSS for the identity to hold, while "CUBRID total"
       sums across processes and must therefore be PSS. */
    double srv_rss, db, lg, etc, vsz, rss, dynmap;
    double minflt, devutil, absorb, osused, cub, ram;
    double dbdev_util;          /* The DB's own disk (CAP2.dev), -1 when it is not known: what panel D judges by;
                                   devutil above is the host's busiest device and stays with the host panel */
    double cpu;                 /* Server CPU percentage, one core = 100 */
    /* A metric on the dashboard must also be in the plots, or finding a trend means
       working out which screen had it. */
    double busy_cores;          /* Host cores in use (the OS box's used) */
    double sat_cores;           /* Saturated cores, 95% and above (the OS box's sat); double because it is graphed */
    double cpu_wa;              /* iowait percentage (the I/O box's iowait row) */
    double load1;               /* One-minute load (in the OS box title) */
    double d_thr;               /* D (disk wait) thread count, cited by the verdict */
    double blkpct;              /* blkio wait percentage, cited by the verdict */
    double wiops;               /* Write IOPS (reads are riops) */
    double c_master,c_pl,c_broker,c_cas;   /* CPU per tier, in cores */
    /* BCB direct read - the basis for the B (fill) and D (drain) panels and the
       verdict.  A frame with the snapshot inactive has pg_ok=0 and zero values. */
    /* RSS per tier - the delta and sparkline columns of the process box (what is growing).
       These are the only graphed view of the broker/CAS/PL trends. */
    double rss_srv, rss_master, rss_pl, rss_broker, rss_cas;
    /* Capacity axis, read by plots B and D and by the verdict */
    double cap_iops, cap_await, cap_hit, cap_miss, cap_head;
    double cg_pct, psi_full;     /* memory cgroup use over its limit (-1 none); PSI memory full avg10 (-1 not provided) */
    double vc_pct;               /* share of the volumes' bytes in the page cache (-1 not measured) */
    int    pg_ok;
    double pg_res_pct;          /* Resident over total, percent */
    double pg_dirty;            /* Dirty page count */
    double pg_cold_pct;         /* LRU cold zone share, percent of resident */
    double pg_span;             /* Flush lag: append.pageid - oldest_dirty.pageid, in log pages */
    double pg_lag;              /* Log fsync lag: append.pageid - nxio.pageid */
    int absorb_ok;
    /* Total of dynamic memory detail B (engine direct read).  It moves in steps:
       conn/lock/session are preallocated from max_clients and fixed, while only the
       plan/result/catalog caches follow actual use.  A (chunk estimate) is re-read
       only on r/a and takes hundreds of ms, so it is kept out of the time series -
       sparse samples would draw a trend that is not there. */
    double heapb;
    double cap_readmit;   /* Refill percentage: what separates a short buffer from a scan */
    double cap_admfrac;   /* Frame admissions over nbuf (-1 unknown); >= 0.5 makes the two above low-confidence */
    int    cap_mperf;     /* Admissions came from the engine's ioread counter (a large buffer not scanned) */
    char hh[12];
} hsam_t;
static hsam_t HS_live[HIST_MAX]; static hsam_t *HS=HS_live; static int HN=0, HCAP=HIST_MAX;
/* Minute averages for the long views (plot key w): every double of the sample averaged over
   the frames of one wall-clock minute - a negative value means "not measured" in every
   field and is left out, so a minute with no measurement stays -1 - the ints and the label
   from the minute's last frame; HM_MAX minutes = 24 hours.  A segment change closes the
   minute.  The same fields as HS, so the plots and verdicts read a long view unchanged. */
#define HM_MAX 1440
static hsam_t HMR[HM_MAX]; static int HMN=0;
static const size_t HS_DBL[]={offsetof(hsam_t,t),offsetof(hsam_t,srv),offsetof(hsam_t,dyn),offsetof(hsam_t,rbps),offsetof(hsam_t,wbps),offsetof(hsam_t,riops),offsetof(hsam_t,majflt),offsetof(hsam_t,srv_rss),offsetof(hsam_t,db),offsetof(hsam_t,lg),offsetof(hsam_t,etc),offsetof(hsam_t,vsz),offsetof(hsam_t,rss),offsetof(hsam_t,dynmap),offsetof(hsam_t,minflt),offsetof(hsam_t,devutil),offsetof(hsam_t,absorb),offsetof(hsam_t,osused),offsetof(hsam_t,cub),offsetof(hsam_t,ram),offsetof(hsam_t,dbdev_util),offsetof(hsam_t,cpu),offsetof(hsam_t,busy_cores),offsetof(hsam_t,sat_cores),offsetof(hsam_t,cpu_wa),offsetof(hsam_t,load1),offsetof(hsam_t,d_thr),offsetof(hsam_t,blkpct),offsetof(hsam_t,wiops),offsetof(hsam_t,c_master),offsetof(hsam_t,c_pl),offsetof(hsam_t,c_broker),offsetof(hsam_t,c_cas),offsetof(hsam_t,rss_srv),offsetof(hsam_t,rss_master),offsetof(hsam_t,rss_pl),offsetof(hsam_t,rss_broker),offsetof(hsam_t,rss_cas),offsetof(hsam_t,cap_iops),offsetof(hsam_t,cap_await),offsetof(hsam_t,cap_hit),offsetof(hsam_t,cap_miss),offsetof(hsam_t,cap_head),offsetof(hsam_t,cg_pct),offsetof(hsam_t,psi_full),offsetof(hsam_t,vc_pct),offsetof(hsam_t,pg_res_pct),offsetof(hsam_t,pg_dirty),offsetof(hsam_t,pg_cold_pct),offsetof(hsam_t,pg_span),offsetof(hsam_t,pg_lag),offsetof(hsam_t,heapb),offsetof(hsam_t,cap_readmit),offsetof(hsam_t,cap_admfrac)};
#define HS_NDBL (sizeof HS_DBL/sizeof HS_DBL[0])
static hsam_t HM_acc, HM_last; static int HM_n[HS_NDBL]; static int HM_cnt=0; static long HM_min=-1;
static void hm_close(void){
    if(HM_cnt<=0) return;
    for(size_t k=0;k<HS_NDBL;k++){ double *d=(double*)((char*)&HM_acc+HS_DBL[k]); *d = HM_n[k]>0 ? *d/HM_n[k] : -1; }
    HM_acc.pg_ok=HM_last.pg_ok; HM_acc.absorb_ok=HM_last.absorb_ok; HM_acc.cap_mperf=HM_last.cap_mperf;
    memcpy(HM_acc.hh,HM_last.hh,sizeof HM_acc.hh);
    if(HMN>=HM_MAX){ memmove(HMR,HMR+1,sizeof(hsam_t)*(HM_MAX-1)); HMN=HM_MAX-1; }
    HMR[HMN++]=HM_acc; HM_cnt=0; HM_min=-1;
}
static void hm_add(const hsam_t *s){
    long m=(long)(s->t/60.0);
    if(HM_cnt>0 && m!=HM_min) hm_close();
    if(HM_cnt==0){ memset(&HM_acc,0,sizeof HM_acc); memset(HM_n,0,sizeof HM_n); HM_min=m; }
    for(size_t k=0;k<HS_NDBL;k++){ double v=*(const double*)((const char*)s+HS_DBL[k]);
        if(v>=0){ *(double*)((char*)&HM_acc+HS_DBL[k])+=v; HM_n[k]++; } }
    HM_last=*s; HM_cnt++;
}
/* The window the plots draw: 0 the live ring, 1 the last hour of minutes, 2 all minutes */
static int g_plot_win=0;
static int hm_view(int win,hsam_t **p,int *n){
    if(!win || HMN<=0){ *p=NULL; *n=0; return 0; }
    int take = win==1 ? (HMN<60?HMN:60) : HMN;
    *p=HMR+(HMN-take); *n=take; return 1;
}

/* One place for the value sources.  Region values go through the sr_*() name lookups without exception; never by index. */
/* The volumes' page-cache residency, measured by vol_cache_scan (defined with the collection) */
static struct { double total, resident, at_ms, ms, age_s; int nfiles, pid; char db[64]; char why[160]; } VC={0,0,-1,0,0,0,-1,"",""};
static int g_hist_pid=-2; static unsigned g_hist_gen=0, g_hist_seg=0;   /* The process and segment the history belongs to */
static unsigned long long g_hist_start=0;
static void hist_push(double t,const proc_t*P,const mem_t*M,const tier_t*T,const iod_t*IO){
    /* History is one process's: after a switch, a restart or the server going down, the old
       samples would be compared with the new process (a "server -X GB" drop that is only a
       restart, a flush minimum and plots mixing two processes).  Live and in a replay alike. */
    if(P->pid!=g_hist_pid || g_inst_gen!=g_hist_gen || g_seg_gen!=g_hist_seg || g_track_start!=g_hist_start){
        HN=0; g_hist_pid=P->pid; g_hist_gen=g_inst_gen; g_hist_seg=g_seg_gen; g_hist_start=g_track_start; hm_close(); }
    if(HN>=HCAP){ memmove(HS,HS+1,sizeof(hsam_t)*(HCAP-1)); HN=HCAP-1; }
    hsam_t*s=&HS[HN++];
    memset(s,0,sizeof *s);
    s->t=t;
    s->srv     = P->pss_kb*1024.0;                  /* The analysed server's PSS: the growth trend */
    s->srv_rss = P->rss_kb*1024.0;                  /* Denominator of the composition total */
    s->rss     = P->rss_kb*1024.0;
    s->vsz     = P->vsize_kb*1024.0;
    s->db      = sr_rss("data_buffer");
    s->lg      = sr_rss("log_buffer");
    s->dyn     = sr_rss("dynamic-heap");
    s->dynmap  = sr_mapped("dynamic-heap");
    { double sum=s->db+s->lg+s->dyn;
      s->etc = (s->srv_rss>sum)?(s->srv_rss-sum):0.0; }
    s->heapb   = B.ok ? B.total : 0;   /* Dynamic memory B total, which moves in steps */
    s->minflt  = IO->minflt; s->majflt = IO->majflt;
    s->rbps=IO->rb; s->wbps=IO->wb; s->riops=IO->ri;
    s->devutil = IO->devutil;
    s->dbdev_util = CAP2.dev[0] ? CAP2.dev_util : -1.0;
    /* Idle stretches (below the denominator floor) are 0; drawing 100% would read as a
       perfect cache.  That makes 0 and "not measurable" the same pixel, so absorb_ok is
       kept and footnoted. */
    s->absorb_ok = IO->absorb_ok;
    s->absorb  = IO->absorb_ok ? IO->absorb*100.0 : 0.0;
    s->cpu     = IO->cpu_pct;
    s->ram     = (double)M->total*1024.0;
    s->cg_pct  = (M->cg_limit>0 && M->cg_usage>=0) ? 100.0*M->cg_usage/M->cg_limit : -1;
    s->psi_full= M->psi_full10;
    s->vc_pct  = VC.total>0 ? 100.0*VC.resident/VC.total : -1;
    s->osused  = (double)(M->total-M->avail)*1024.0;
    s->cub     = P->rss_kb*1024.0 + T->master + T->broker + T->cas + T->pl;   /* RSS, as top */
    /* CPU and I/O extension, holding the dashboard's values in the dashboard's units. */
    s->busy_cores = M->busy_cores;
    s->sat_cores  = (double)M->sat_cores;
    s->cpu_wa     = M->cpu_wa;
    s->load1      = M->load1;
    s->d_thr      = (double)IO->d_thr;
    s->blkpct     = IO->blkpct;
    s->wiops      = IO->wi;
    s->c_master   = T->cpu_master;
    s->c_pl       = T->cpu_pl;
    s->c_broker   = T->cpu_broker;
    s->c_cas      = T->cpu_cas;
    s->rss_srv=P->rss_kb*1024.0; s->rss_master=T->master; s->rss_pl=T->pl;
    s->rss_broker=T->broker; s->rss_cas=T->cas;
    s->cap_iops=CAP2.dev_iops; s->cap_await=CAP2.await_ms<0?0:CAP2.await_ms;
    s->cap_readmit=CAP2.readmit_pct;          /* -1 = not measured: it alone separates shortage from scan */
    s->cap_admfrac=CAP2.adm_frac;
    s->cap_hit=CAP2.hit_pct<0?0:CAP2.hit_pct; s->cap_miss=CAP2.miss_pps;   /* -1 = not measured */
    s->cap_mperf = !strcmp(CAP2.miss_src,"perfmon") ? 1 : !strcmp(CAP2.miss_src,"sample") ? 2 : 0;
    s->cap_head=CAP2.headroom_pct<0?0:CAP2.headroom_pct;
    s->pg_ok      = PG.ok || AG.ok;
    if(!PG.ok && AG.ok){                /* Not scanned: the engine's own aggregates; no oldest dirty */
        double lru=(double)(AG.z1+AG.z2+AG.z3);
        s->pg_res_pct  = AG.nbuf>0 ? 100.0*lru/AG.nbuf : 0;
        s->pg_dirty    = (double)AG.dirty;
        s->pg_cold_pct = lru>0 ? 100.0*AG.z3/lru : 0;
        s->pg_span     = -1;
        s->pg_lag      = PG.have_log ? (double)(PG.log_append.pageid-PG.log_nxio.pageid) : 0;
    }
    if(PG.ok){
        s->pg_res_pct  = PG.nbuf>0 ? 100.0*PG.resident/PG.nbuf : 0;
        s->pg_dirty    = PG.dirty;
        s->pg_cold_pct = PG.resident>0 ? 100.0*PG.z3/PG.resident : 0;
        s->pg_span     = (PG.have_log&&PG.have_oldest) ? (double)(PG.log_append.pageid-PG.oldest_dirty.pageid) : 0;
        s->pg_lag      = PG.have_log ? (double)(PG.log_append.pageid-PG.log_nxio.pageid) : 0;
    }
    time_t tt=(time_t)t; struct tm tmv; rep_localtime(tt,&tmv);
    strftime(s->hh,sizeof s->hh,"%H:%M:%S",&tmv);        /* 24-hour clock; the date is in the top box */
    hm_add(s);
}
/* Dump the last history sample to stderr, leaving the terse stdout contract clean.
   tools/check-hist.sh compares this against the terse keys to catch numbers that
   disagree between views. */
static void hist_dump_last(void){
    if(HN<1) return;
    const hsam_t*s=&HS[HN-1];
    fprintf(stderr,
        "hist.srv_rss=%.0f hist.db=%.0f hist.lg=%.0f hist.dyn=%.0f hist.etc=%.0f\n"
        "hist.dynmap=%.0f hist.vsz=%.0f hist.rss=%.0f hist.cub=%.0f hist.osused=%.0f\n"
        "hist.minflt=%.0f hist.majflt=%.0f hist.rbps=%.0f hist.wbps=%.0f\n"
        "hist.devutil=%.1f hist.dbdev_util=%.1f hist.absorb=%.1f hist.absorb_ok=%d\n"
        "hist.pg_ok=%d hist.pg_res_pct=%.2f hist.pg_dirty=%.0f hist.pg_cold_pct=%.2f hist.pg_span=%.0f hist.pg_lag=%.0f\n"
        "hist.rss_srv=%.0f hist.rss_master=%.0f hist.rss_pl=%.0f hist.rss_broker=%.0f hist.rss_cas=%.0f\n"
        "hist.cap_iops=%.0f hist.cap_await=%.2f hist.cap_hit=%.2f hist.cap_miss=%.0f hist.cap_head=%.1f\n",
        s->srv_rss,s->db,s->lg,s->dyn,s->etc,
        s->dynmap,s->vsz,s->rss,s->cub,s->osused,
        s->minflt,s->majflt,s->rbps,s->wbps,
        s->devutil,s->dbdev_util,s->absorb,s->absorb_ok,
        s->pg_ok,s->pg_res_pct,s->pg_dirty,s->pg_cold_pct,s->pg_span,s->pg_lag,
        s->rss_srv,s->rss_master,s->rss_pl,s->rss_broker,s->rss_cas,
        s->cap_iops,s->cap_await,s->cap_hit,s->cap_miss,s->cap_head);
}
static double hist_peak(int which,double floor_v){
    double mx=floor_v;
    for(int i=0;i<HN;i++){
        double v = which==0?HS[i].rbps : which==1?HS[i].wbps : HS[i].riops;
        if(v>mx) mx=v;
    }
    return mx;
}

/* ---------------- TUI control ---------------- */
#include <termios.h>
#include <locale.h>
#include <langinfo.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
static struct termios g_tio; static int g_raw=0;
static volatile sig_atomic_t g_stop=0;
static void on_sig(int s){ (void)s; g_stop=1; }

/* The terminal state is restored from signal handlers too, so the sequences are built once
   in tui_enter and written with write(2) and tcsetattr, both async-signal-safe */
static char g_seq_leave[256], g_seq_enter[256]; static int g_seq_leave_n, g_seq_enter_n;
static struct termios g_tio_raw;
/* The handlers write to a copy of the terminal fd taken in tui_enter: a recorded frame is
   rendered with fd 1 pointing at a memory file, and the sequences must not end up in it */
static int g_tty_fd=STDOUT_FILENO;
static void tui_seq_build(void){
    g_seq_leave_n=0; g_seq_enter_n=0;
    g_seq_leave_n+=snprintf(g_seq_leave+g_seq_leave_n,sizeof g_seq_leave-(size_t)g_seq_leave_n,"%s","\033[?25h\033[0m\033(B\017\033[?2004h\033[?2026l");
    g_seq_enter_n+=snprintf(g_seq_enter+g_seq_enter_n,sizeof g_seq_enter-(size_t)g_seq_enter_n,"%s","\033(B\017\033[2J\033[H\033[?25l\033[?2004l\033[?2026l");
}
static void tui_raw_restore(void){
    if(g_raw) tcsetattr(STDIN_FILENO,TCSANOW,&g_tio);
    if(write(g_tty_fd,g_seq_leave,(size_t)g_seq_leave_n)<0){}
}
static void tui_raw_reenter(void){
    if(g_raw) tcsetattr(STDIN_FILENO,TCSANOW,&g_tio_raw);
    if(write(g_tty_fd,g_seq_enter,(size_t)g_seq_enter_n)<0){}   /* clears: the next frame redraws everything */
}
/* Ctrl-Z: hand the terminal back as it was, stop; on fg the raw mode, colours and hidden
   cursor come back and the screen is cleared for the next frame */
/* Re-raise a signal with its default action from inside its own handler: it is blocked
   there, so it must be unblocked or it only pends (the handler would run again and again) */
static void raise_default(int s){
    sigset_t m; sigemptyset(&m); sigaddset(&m,s);
    signal(s,SIG_DFL); sigprocmask(SIG_UNBLOCK,&m,NULL); raise(s);
}
static void on_tstp(int s){
    int e=errno; tui_raw_restore();
    raise_default(SIGTSTP);                      /* stops here until fg */
    signal(SIGTSTP,on_tstp); (void)s; errno=e;
}
static void on_cont(int s){ int e=errno; (void)s; g_ustop_n++; tui_raw_reenter(); errno=e; }
/* Children this process started (paramdump's group, the DWARF extraction): stopped on the
   way out by a signal, since no exit path runs then.  Async-signal-safe: kill(2) only. */
static void kids_kill_now(void){
    if(g_pd_pid>0) kill(-g_pd_pid,SIGKILL);
    if(g_dw_kid>0) kill(g_dw_kid,SIGKILL);
}
static void child_forget_parent(void){
    g_pd_pid=-1; g_dw_kid=0; g_dw_kfd=-1; g_raw=0;
    if(g_tty_fd!=STDOUT_FILENO){ close(g_tty_fd); g_tty_fd=STDOUT_FILENO; }
    for(size_t i=0;i<sizeof g_handled_sigs/sizeof g_handled_sigs[0];i++) signal(g_handled_sigs[i],SIG_DFL);
}
/* abort() and crashes: the terminal first, then the default action (core, status) */
static void on_fatal(int s){ tui_raw_restore(); kids_kill_now(); raise_default(s); }
/* One-shot runs keep the default disposition of INT/TERM/HUP/QUIT (exit by that signal);
   only the children are stopped first, or a waiting paramdump would outlive the run */
static void on_term_kids(int s){ kids_kill_now(); raise_default(s); }
static void tui_leave(void){
    static int done=0;
    if(g_raw){ tcsetattr(STDIN_FILENO,TCSADRAIN,&g_tio); g_raw=0; }
    if(done) return;                 /* Called explicitly and via atexit, but runs once */
    done=1;
    /* Restore only the cursor and leave the screen, so the last frame stays on the tty.
       G0 is reset to ASCII again so the shell prompt does not inherit a CJK charset. */
    printf("\033[?25h\033[0m\033(B\017\033[?2004h\033[?2026l"); fflush(stdout);   /* Restore the input state */
}
static void tui_enter(void){
    if(isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&g_tio)==0){
        struct termios t=g_tio; t.c_lflag&=~(ICANON|ECHO); t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
        tcsetattr(STDIN_FILENO,TCSANOW,&t); g_raw=1;
    }
    /* No alternate screen (?1049h) - it would erase the last frame on exit.
       ESC(B + SI resets G0 to ASCII, clearing any CJK/line-drawing charset state
       a previous program left behind (restored on exit).

       On IME: no escape sequence lets a remote process turn off the client's
       input method - the IME belongs to the user's OS and SSH carries only its
       output.  The two sequences below are a best effort on terminals that
       honour them, and tui_key() folds Hangul jamo to ASCII keys for the rest.
         ESC[?2004l  disable bracketed paste, so composition is not read as a paste
         ESC[8;1"q   DECSCA default; some terminals reset input state on it
       tui_leave() restores both. */
    printf("\033(B\017\033[2J\033[H\033[?25l\033[?2004l\033[?2026l"); fflush(stdout);
    if(g_raw) tcgetattr(STDIN_FILENO,&g_tio_raw);
    { int d=fcntl(STDOUT_FILENO,F_DUPFD_CLOEXEC,3); if(d>=0) g_tty_fd=d; }
    tui_seq_build();
    atexit(tui_leave);
    signal(SIGINT,on_sig); signal(SIGTERM,on_sig);
    signal(SIGQUIT,on_sig); signal(SIGHUP,on_sig);       /* the normal way out, which restores */
    signal(SIGTSTP,on_tstp); signal(SIGCONT,on_cont);
    signal(SIGABRT,on_fatal); signal(SIGSEGV,on_fatal); signal(SIGBUS,on_fatal); signal(SIGFPE,on_fatal);
    /* Signals whose default action ends the process: the terminal comes back first */
    signal(SIGXCPU,on_fatal); signal(SIGUSR1,on_fatal); signal(SIGUSR2,on_fatal);
    signal(SIGALRM,on_fatal); signal(SIGPIPE,on_fatal);
}
/* Map Hangul typed on a 2-set layout back to the ASCII keys on the same physical
   keys, so a key pressed with the IME still in Hangul mode does what was intended.
   Jamo (U+3131-U+3163) give their key(s); a composed syllable (U+AC00-U+D7A3), which
   an IME emits for two or three keystrokes, gives each of them in order.  Shifted
   keys fold to lower case.  Returns "" for anything else. */
static const char *ko_to_keys(int cp){
    static const char *const jamo[51]={      /* U+3131..U+3163 */
        "r","r","rt","s","sw","sg","e","e","f","fr","fa","fq","ft","fx","fv","fg",
        "a","q","q","qt","t","t","d","w","w","c","z","x","v","g",
        "k","o","i","o","j","p","u","p","h","hk","ho","hl","y",
        "n","nj","np","nl","b","m","ml","l"};
    static const char *const cho[19]={
        "r","r","s","e","e","f","a","q","q","t","t","d","w","w","c","z","x","v","g"};
    static const char *const jong[28]={
        "","r","r","rt","s","sw","sg","e","f","fr","fa","fq","ft","fx","fv","fg",
        "a","q","qt","t","t","d","w","c","z","x","v","g"};
    static char k[8];
    k[0]=0;
    if(cp>=0x3131 && cp<=0x3163) snprintf(k,sizeof k,"%s",jamo[cp-0x3131]);
    else if(cp>=0xAC00 && cp<=0xD7A3){
        int sy=cp-0xAC00;
        snprintf(k,sizeof k,"%s%s%s",cho[sy/588],jamo[30+(sy%588)/28],jong[sy%28]);
    }
    return k;
}
/* Special key codes returned by tui_key, at 0x100 and above so they cannot collide with byte values */
#define TUI_KEY_UP    0x101
#define TUI_KEY_DOWN  0x102
#define TUI_KEY_RIGHT 0x103
#define TUI_KEY_LEFT  0x104
#define TUI_KEY_PGUP  0x105
#define TUI_KEY_PGDN  0x106
/* Bytes already read but not yet returned: the tail of an ESC prefix that was not a
   recognised sequence, or the further keys of a decomposed Hangul syllable. */
static unsigned char g_kq[16]; static int g_kqh, g_kqn;
/* Put n bytes back in front of the queue, so they are returned next and in order */
static void kq_unget(const unsigned char *b,int n){
    for(int i=n-1;i>=0 && g_kqn<(int)sizeof g_kq;i--){
        g_kqh=(g_kqh+(int)sizeof g_kq-1)%(int)sizeof g_kq; g_kq[g_kqh]=b[i]; g_kqn++;
    }
}
static int key_byte(unsigned char *c){
    if(g_kqn>0){ *c=g_kq[g_kqh]; g_kqh=(g_kqh+1)%(int)sizeof g_kq; g_kqn--; return 1; }
    return read(STDIN_FILENO,c,1)==1;
}
static int tui_key(void){
    unsigned char c;
    if(!key_byte(&c)) return -1;
    if(c==0x1b){
        /* Interpret "ESC [ X", "ESC O X" and "ESC [ N ~" one byte at a time; bytes
           that do not continue a known sequence are returned as the next keys. */
        unsigned char b[3]; int n=0;
        if(!key_byte(&b[n])) return 0x1b;
        n++;
        if(b[0]=='[' || b[0]=='O'){
            if(key_byte(&b[n])){
                n++;
                switch(b[1]){
                case 'A': return TUI_KEY_UP;
                case 'B': return TUI_KEY_DOWN;
                case 'C': return TUI_KEY_RIGHT;
                case 'D': return TUI_KEY_LEFT;
                }
                if(b[0]=='[' && (b[1]=='5'||b[1]=='6') && key_byte(&b[n])){
                    if(b[2]=='~') return b[1]=='5' ? TUI_KEY_PGUP : TUI_KEY_PGDN;
                    n++;
                }
            }
        }
        kq_unget(b,n);
        return 0x1b;
    }
    if(c<0x80) return c;
    /* Multi-byte: the remaining bytes must be consumed, or they are taken as the next key. */
    int need = (c>=0xF0)?3 : (c>=0xE0)?2 : (c>=0xC0)?1 : 0;
    unsigned char b[3]; int got=0;
    for(int i=0;i<need;i++){ if(!key_byte(&b[i])) break; got++; }
    if(got<need) return -1;                       /* Incomplete sequence: discarded */
    int cp=-1;
    if(need==1) cp=((c&0x1F)<<6)|(b[0]&0x3F);
    else if(need==2) cp=((c&0x0F)<<12)|((b[0]&0x3F)<<6)|(b[1]&0x3F);
    else if(need==3) cp=((c&0x07)<<18)|((b[0]&0x3F)<<12)|((b[1]&0x3F)<<6)|(b[2]&0x3F);
    const char *k=ko_to_keys(cp);
    if(!k[0]) return -1;                          /* Unmapped characters are ignored */
    kq_unget((const unsigned char*)k+1,(int)strlen(k+1));
    return (unsigned char)k[0];
}
static int term_w(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col>=60)
        return ws.ws_col<114?ws.ws_col:114;              /* Ceiling of 114 */
    return 114;
}
/* Real row count, no fallback; 0 means unknown.  Used to paginate the help. */
static int term_h(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_row>0) return ws.ws_row;
    return 0;
}
/* Real column count, no fallback; 0 means unknown.  term_w() falls back to 114 below
   60, and drawing at that width wraps and breaks the screen - wide views use this
   value to decide "too narrow" first. */
static int term_cols_raw(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col>0) return ws.ws_col;
    return 0;
}


/* ---- Capacity axis: scale up, scale down, tune ----
 * Looks at demand, ceiling and headroom against allocated resources rather than
 * at one query.  Nothing is written to the target and no connection or lock is
 * taken.
 *   Engine statistics are read only when already counting: perfmon does not
 *   increment counters unless n_watchers > 0, and creating a watcher would write
 *   to the target and add server hot-path cost, so it is not done.
 *   Fallback: the VPID set difference between two BCB snapshots gives buffer
 *   turnover with no extra I/O, since the same array is already read.
 *   Cost: 8B x 6 reads, a hash comparison, and /proc/diskstats fields already
 *   being read - well under 1ms per frame.
 */
static int g_replaying=0;             /* Set during --replay so the screen says so */
static char g_rp_capvd[320]="";           /* Recorded cap.verdict: recomputing it from rounded keys can change a digit */
static int g_rep_i=0,g_rep_n=0;       /* Replay progress, shown in the status line */
static time_t g_rep_ts=0;             /* The frame's recorded time, shown instead of now */
static double g_ts_frac=-1;           /* The frame's time in fractional seconds for ts_epoch_ms; <0 = ts_epoch */
static char g_rp_tbl[32]="";          /* Replay: the recording's heapB.table, which this host never adopted */
static char g_rp_tblver[32]="";       /* ... and its heapB.table_version */
static const char *tq(const char *s);   /* quoted dump value: escaped, valid UTF-8 */
/* v as printf("%.*f") prints it: a key derived from printed keys then replays exactly */
static double rnd_as_printed(double v,int dec){ char b[64]; snprintf(b,sizeof b,"%.*f",dec,v); return strtod(b,NULL); }
static const char *g_rec_path=NULL;   /* --record FILE  */
static char g_rec_err[160]="";        /* Why --record stopped: shown on screen, exit status 1 */
static const char *g_rep_path=NULL;   /* --replay FILE  */
static double g_rep_speed=1.0;        /* --replay-speed (0 = as fast as possible) */
/* Three block-size bands x three read/write bands.  The boundaries come from CUBRID's
   real I/O units: log and DWB around 4KB, data pages 16KB by default, scan read-ahead
   128KB and above. */
#define CAP_BS_N 3
#define CAP_RW_N 3
static const char *CAP_BS_NAME[CAP_BS_N]={"<8KB","8-64KB",">64KB"};
static const char *CAP_RW_NAME[CAP_RW_N]={"write-heavy","mixed","read-heavy"};
typedef struct { double iops,bps,util,qd,await_ms; time_t when; int valid; } cap_cell_t;
/* Ceilings are per device: the busiest device can change between frames, and a fast
   device's ceiling says nothing about a slow one.  g_cap_mtx is the frame's device. */
#define CAPDEV_N 16
typedef struct { char name[160]; cap_cell_t m[CAP_BS_N][CAP_RW_N]; } cap_dev_t;   /* name: dev_key() */
static cap_dev_t g_capdev[CAPDEV_N]; static int g_capdev_n=0;
static cap_cell_t g_cap_none[CAP_BS_N][CAP_RW_N];   /* No device (or table full): stays empty */
static cap_cell_t (*g_cap_mtx)[CAP_RW_N]=g_cap_none;
/* A device's stable identity for remembered ceilings: kernel names (sdX, nvme0n1, dm-N) move
   between boots and LVM activations.  dm uuid, wwid or serial, then "@<sectors>"; a partition
   is its disk's identity plus ":p<n>".  With no identity the kernel name stands, sized. */
static const char *dev_key(const char *nm){
    static char last[64]="", key[160]="";
    if(!nm||!nm[0]) return nm;
    if(!strcmp(last,nm)) return key;
    static char sp[PATH_MAX+64], rp[PATH_MAX]; char id[128]="", part[16]=""; long long sz=0;   /* realpath writes up to PATH_MAX */
    snprintf(sp,sizeof sp,"/sys/class/block/%s",nm);
    if(!realpath(sp,rp)) snprintf(rp,sizeof rp,"%.4000s",sp);
    { char b[64]; snprintf(sp,sizeof sp,"%s/size",rp); if(read_file(sp,b,sizeof b)>0) sz=atoll(b);
      snprintf(sp,sizeof sp,"%s/partition",rp); if(read_file(sp,b,sizeof b)>0){
          snprintf(part,sizeof part,":p%d",atoi(b)); char *sl=strrchr(rp,'/'); if(sl) *sl=0; } }
    static const char *ID[]={"dm/uuid","wwid","device/wwid","device/serial"};
    for(int i=0;i<4 && !id[0];i++){
        char b[128]; snprintf(sp,sizeof sp,"%s/%s",rp,ID[i]);
        if(read_file(sp,b,sizeof b)<=0) continue;
        int k=0, gap=0;
        for(char *q=b;*q && k<(int)sizeof id-1;q++){
            if(*q==' '||*q=='\t'||*q=='\n'){ gap=1; continue; }
            if(gap && k){ id[k++]='_'; } gap=0; id[k++]=*q; }
        id[k]=0; }
    snprintf(key,sizeof key,"%s%s@%lld",id[0]?id:nm,part,sz);
    snprintf(last,sizeof last,"%s",nm);
    return key;
}
static char g_cap_devid[160]="";   /* dev_key of the frame's device, as recorded */
static cap_dev_t *cap_dev_get(const char *nm){
    if(!nm||!nm[0]) return NULL;
    for(int i=0;i<g_capdev_n;i++) if(!strcmp(g_capdev[i].name,nm)) return &g_capdev[i];
    if(g_capdev_n>=CAPDEV_N) return NULL;
    cap_dev_t *d=&g_capdev[g_capdev_n++]; memset(d,0,sizeof *d);
    snprintf(d->name,sizeof d->name,"%s",nm); return d;
}
static int cap_bs_idx(double kb){ return (kb<8.0)?0:(kb<=64.0)?1:2; }
static int cap_rw_idx(double rr){ return (rr<0.3)?0:(rr<=0.7)?1:2; }

/* ---- Engine statistics direct read, used when already counting ----
   Adopted only after the psid self-check (metadata psid == requested psid) and a
   start_offset range check. */
enum { CPS_PB_FETCH=0, CPS_PB_IOREAD, CPS_PB_IOWRITE, CPS_LOG_IOWRITE, CPS_COMMIT, CPS_N };
/* The PSTAT_ID order from 11.5's perf_monitor.h.  A psid shifted by a version difference is caught by the self-check. */
static const int CPS_PSID[CPS_N]={ 8, 10, 11, 30, 44 };
static int cap_pstat_read(proc_t*P,uint64_t out[CPS_N]){
    if(!B.ok||B.partial) { snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"method B off":"방법 B 비활성"); return 0; }
    if(g_bt->ps.nvals<0||!B.sym[S_PSTATG]||!B.sym[S_PSTATMD]){
        snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"no perfmon offsets/symbols":"perfmon 오프셋·심볼 없음"); return 0; }
    unsigned long g=B.base+B.sym[S_PSTATG], md=B.base+B.sym[S_PSTATMD];
    int32_t nvals=0; unsigned char init=0; uint64_t gptr=0;
    if(vmread(P->pid,g+g_bt->ps.nvals,&nvals,4)!=4) return 0;
    vmread(P->pid,g+g_bt->ps.init,&init,1);
    if(vmread(P->pid,g+g_bt->ps.gstats,&gptr,8)!=8) return 0;
    if(!init||!gptr||nvals<=0||nvals>1000000){
        snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"perfmon not initialized":"perfmon 미초기화"); return 0; }
    for(int i=0;i<CPS_N;i++){
        unsigned long e=md+(unsigned long)CPS_PSID[i]*(unsigned long)g_bt->ps.sz_md;
        int32_t psid=-1, st=-1;
        if(vmread(P->pid,e+g_bt->ps.md_psid,&psid,4)!=4) return 0;
        if(vmread(P->pid,e+g_bt->ps.md_start,&st,4)!=4) return 0;
        if(psid!=CPS_PSID[i]||st<0||st>=nvals){    /* Version mismatch: give up rather than invent */
            snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"psid mismatch (other version)":"psid 불일치(타버전)"); return 0; }
        if(vmread(P->pid,(unsigned long)gptr+(unsigned long)st*8,&out[i],8)!=8) return 0;
    }
    return 1;
}

/* ---- Turnover by direct read: the exact VPID set difference between BCB snapshots ----
   PG.rec was already read this frame (no extra I/O).  Sets are sorted 48-bit keys
   (volid<<32 | pageid), so two pages never share an entry.  A refill is a page read
   back within the last nbuf evictions; a scan, never reread, ages out of that window. */
static inline uint64_t vp_key(int volid,int pageid){ return ((uint64_t)(uint16_t)volid<<32)|(uint32_t)pageid; }
static int vp_grow(void **a,int *cap,int need,size_t esz){
    if(need<=*cap) return 1;
    void *p=realloc(*a,esz*(size_t)need); if(!p) return 0;
    *a=p; *cap=need; return 1;
}
/* LSD radix sort on the 48 key bits, then drop duplicates (a latch-free read can see one page twice) */
static int vp_sort_unique(uint64_t *a,uint64_t *tmp,int n){
    static unsigned cnt[1<<16];
    uint64_t *src=a,*dst=tmp;
    for(int sh=0;sh<48;sh+=16){
        memset(cnt,0,sizeof cnt);
        for(int i=0;i<n;i++) cnt[(src[i]>>sh)&0xffff]++;
        unsigned sum=0; for(int d=0;d<(1<<16);d++){ unsigned c=cnt[d]; cnt[d]=sum; sum+=c; }
        for(int i=0;i<n;i++) dst[cnt[(src[i]>>sh)&0xffff]++]=src[i];
        uint64_t *t=src; src=dst; dst=t;
    }
    if(src!=a) memcpy(a,src,sizeof *a*(size_t)n);
    int m=0; for(int i=0;i<n;i++) if(m==0||a[m-1]!=a[i]) a[m++]=a[i];
    return m;
}
/* Keys of PG.rec, sorted and unique, into *out */
static int vp_from_pg(uint64_t **out,int *cap,uint64_t **tmp,int *tcap){
    int n=PG.nrec;
    if(!vp_grow((void**)out,cap,n,sizeof **out)||!vp_grow((void**)tmp,tcap,n,sizeof **tmp)) return -1;
    for(int i=0;i<n;i++) (*out)[i]=vp_key(PG.rec[i].volid,PG.rec[i].pageid);
    return vp_sort_unique(*out,*tmp,n);
}
typedef struct { uint64_t key, seq; } vp_ev_t;   /* An eviction: the page and its eviction number */
static uint64_t *vp_prev=NULL,*vp_cur=NULL,*vp_tmp=NULL,*vp_evk=NULL;
static int vp_prev_cap=0,vp_cur_cap=0,vp_tmp_cap=0,vp_evk_cap=0;
static vp_ev_t *vp_hist=NULL,*vp_hist2=NULL;      /* Evictions in the window, sorted by key */
static int vp_hist_n=0,vp_hist_cap=0,vp_hist2_cap=0;
static uint64_t vp_evseq=0;                       /* Evictions counted so far */
static int cap_prev_n=0;                          /* Size of the previous frame's set; 0 = no baseline */
static double cap_prev_t=0;
/* One-shot (-t, tree) has a single frame and so no memory baseline.  If a previous
   snapshot file from --bcb-dump exists it serves as the baseline (one file read, no
   extra contact with the server), used only when the two are 0.2-60s apart: shorter
   is noise, longer is meaningless. */
static int cap_turnover_from_dump(void){
    /* One-shot only: live keeps its baseline in memory (cap_turnover, which also has the
       eviction history), so re-reading and sorting the last dump every frame is waste */
    if(g_pd_async) return 0;
    if(!g_bcb_dump[0]||!PG.ok||PG.degraded||PG.nrec<=0) return 0;
    FILE*f=fopen(g_bcb_dump,"rb"); if(!f) return 0;
    struct { char magic[8]; uint32_t version,reclen; int64_t ts_sec,ts_nsec;
             int32_t nbuf,nrec,dirty,pagesize;
             uint64_t la,ln,le,od; char db[32]; } h;
    if(fread(&h,sizeof h,1,f)!=1||memcmp(h.magic,"CBCBMAP1",8)||h.reclen!=32||h.nrec<=0){ fclose(f); return 0; }
    /* Another database's snapshot (one-shot over several instances) is no baseline */
    h.db[sizeof h.db-1]=0;
    if(strncmp(h.db,g_cur_db,sizeof h.db-1)){ fclose(f); return 0; }
    /* With nanoseconds: runs 0.2-1s apart gave 0 (refused) or 1 (a wrong rate) in seconds */
    double dt=(double)(PG.when.tv_sec-h.ts_sec)+(double)(PG.when.tv_nsec-h.ts_nsec)/1e9;
    if(dt<0.2||dt>60.0){ fclose(f); return 0; }
    uint64_t *prev=NULL,*cur=NULL,*tmp=NULL; int pcap=0,ccap=0,tcap=0,np=0;
    if(!vp_grow((void**)&prev,&pcap,h.nrec,sizeof *prev)){ fclose(f); return 0; }
    for(int i=0;i<h.nrec;i++){
        struct { int16_t volid,zone; int32_t pageid; uint32_t flags; int32_t hdr_ok; uint64_t a,b; } r;
        if(fread(&r,sizeof r,1,f)!=1) break;
        if(r.volid<0||r.pageid<0) continue;
        prev[np++]=vp_key(r.volid,r.pageid);
    }
    fclose(f);
    int nc=-1;
    if(vp_grow((void**)&tmp,&tcap,np>PG.nrec?np:PG.nrec,sizeof *tmp)){
        np=vp_sort_unique(prev,tmp,np);
        nc=vp_from_pg(&cur,&ccap,&tmp,&tcap);
    }
    long admitted=0;
    for(int i=0,j=0;i<nc;i++){
        while(j<np && prev[j]<cur[i]) j++;
        if(j>=np || prev[j]!=cur[i]) admitted++;
    }
    free(prev); free(cur); free(tmp);
    if(nc<=0) return 0;
    CAP2.turnover_pct = 100.0*(double)admitted/(double)nc;
    CAP2.miss_pps     = (double)admitted/dt;
    CAP2.adm_frac     = PG.nbuf>0 ? (double)admitted/(double)PG.nbuf : -1;
    CAP2.readmit_pct  = -1;    /* A file baseline has no eviction history, so refill cannot be computed */
    return 1;
}
static void cap_turnover(double now_s){
    CAP2.turnover_pct=-1; CAP2.readmit_pct=-1; CAP2.miss_pps=-1; CAP2.adm_frac=-1;
    static long last_seq=-1; static double k_t=-1,k_r=-1,k_m=-1,k_f=-1;
    { static unsigned seen=0;
      if(seen!=g_seg_gen){ seen=g_seg_gen; cap_prev_n=0; vp_hist_n=0; vp_evseq=0; last_seq=-1; k_t=k_r=k_m=k_f=-1; } }
    g_miss_sample=0;
    if(PG.ok && PG.large && PG.from_last && PG.scan_seq==last_seq){
        /* Between passes: the last pass's turnover and re-admit stand; admissions come from
           this frame's slot sample when it has one */
        CAP2.turnover_pct=k_t; CAP2.readmit_pct=k_r; CAP2.miss_pps=k_m; CAP2.adm_frac=k_f;
        if(SM.miss_pps>=0){ CAP2.miss_pps=SM.miss_pps; CAP2.adm_frac=SM.frac; g_miss_sample=1; }
        return;
    }
    if(!PG.ok||PG.nrec<=0) return;
    if(PG.from_last) return;                       /* the last pass already went through here */
    /* A degraded frame holds only verified records; comparing it would count the dropped
       ones as evicted.  It is skipped and the baseline kept for the next full frame. */
    if(PG.degraded) return;
    int n=vp_from_pg(&vp_cur,&vp_cur_cap,&vp_tmp,&vp_tmp_cap);
    if(n<=0) return;
    if(cap_prev_n>0){
        uint64_t win=(uint64_t)(PG.nbuf>n?PG.nbuf:n);
        if(!vp_grow((void**)&vp_evk,&vp_evk_cap,cap_prev_n,sizeof *vp_evk)) return;
        /* One merge walk: admitted = cur - prev (looked up in the window), evicted = prev - cur */
        long admitted=0, readmit=0; int ne=0;
        for(int i=0,j=0,h=0;i<n||j<cap_prev_n;){
            if(j>=cap_prev_n || (i<n && vp_cur[i]<vp_prev[j])){
                uint64_t k=vp_cur[i++]; admitted++;
                while(h<vp_hist_n && vp_hist[h].key<k) h++;
                if(h<vp_hist_n && vp_hist[h].key==k && vp_hist[h].seq+win>vp_evseq) readmit++;
            } else if(i>=n || vp_prev[j]<vp_cur[i]) vp_evk[ne++]=vp_prev[j++];
            else { i++; j++; }
        }
        /* Window update: merge this frame's evictions, drop those older than the last win */
        uint64_t top=vp_evseq+(uint64_t)ne;
        if(!vp_grow((void**)&vp_hist2,&vp_hist2_cap,vp_hist_n+ne,sizeof *vp_hist2)) return;
        int m=0;
        for(int a=0,b=0;a<vp_hist_n||b<ne;){
            vp_ev_t e;
            if(b>=ne || (a<vp_hist_n && vp_hist[a].key<vp_evk[b])) e=vp_hist[a++];
            else { if(a<vp_hist_n && vp_hist[a].key==vp_evk[b]) a++;   /* evicted again: keep the newer */
                   e.key=vp_evk[b]; e.seq=vp_evseq+(uint64_t)(++b); }
            if(e.seq+win>top) vp_hist2[m++]=e;
        }
        { vp_ev_t *t=vp_hist; vp_hist=vp_hist2; vp_hist2=t;
          int c=vp_hist_cap; vp_hist_cap=vp_hist2_cap; vp_hist2_cap=c; }
        vp_hist_n=m; vp_evseq=top;
        double dt=now_s-cap_prev_t;
        if(dt>0.05){
            CAP2.turnover_pct = 100.0*(double)admitted/(double)n;
            CAP2.readmit_pct  = admitted>0 ? 100.0*(double)readmit/(double)admitted : 0;
            CAP2.miss_pps     = (double)admitted/dt;
            CAP2.adm_frac     = PG.nbuf>0 ? (double)admitted/(double)PG.nbuf : -1;
        }
    }
    last_seq=PG.scan_seq; k_t=CAP2.turnover_pct; k_r=CAP2.readmit_pct; k_m=CAP2.miss_pps; k_f=CAP2.adm_frac;
    { uint64_t *t=vp_prev; vp_prev=vp_cur; vp_cur=t;
      int c=vp_prev_cap; vp_prev_cap=vp_cur_cap; vp_cur_cap=c; }
    cap_prev_n=n; cap_prev_t=now_s;
}

/* ---- Remembered ceilings: the peak IOPS and bandwidth of frames with a queue (depth >= 4), kept for the run ---- */
static void cap_ceiling(void){
    CAP2.headroom_pct=-1;
    { const char *k=dev_key(CAP2.dev); snprintf(g_cap_devid,sizeof g_cap_devid,"%s",k?k:"");
      cap_dev_t *d=cap_dev_get(k); g_cap_mtx = d ? d->m : g_cap_none; }
    /* This frame's I/O profile: mean request size and read share */
    { double ios=CAP2.dev_iops, byt=CAP2.dev_rbps+CAP2.dev_wbps;
      CAP2.req_kb = (ios>0) ? byt/ios/1024.0 : 0;
      CAP2.read_ratio = (byt>0) ? CAP2.dev_rbps/byt : 0.5;
      CAP2.prof_bs = cap_bs_idx(CAP2.req_kb);
      CAP2.prof_rw = cap_rw_idx(CAP2.read_ratio); }
    /* The observed ceiling is updated only where a queue really built (depth >= 4): one I/O kept
       outstanding already shows util ~100% and depth ~1, and that serial stream measures the
       latency, not what the device can do in parallel.  Only the current profile cell is
       updated - a ceiling measured at 128KB sequential would make the 4KB random cell
       report false headroom. */
    double cur_iops=CAP2.dev_iops;
    int sat = CAP2.qdepth>=4.0;
    { cap_cell_t*c=&g_cap_mtx[CAP2.prof_bs][CAP2.prof_rw];
      if(sat && cur_iops>c->iops && g_cap_mtx!=g_cap_none){
          c->iops=cur_iops; c->bps=CAP2.dev_rbps+CAP2.dev_wbps; c->util=CAP2.dev_util;
          c->qd=CAP2.qdepth<0?0:CAP2.qdepth; c->await_ms=CAP2.await_ms<0?0:CAP2.await_ms;
          c->when=time(NULL); c->valid=1;
      }
      /* The ceiling used for the verdict is the current profile's cell; an empty cell means
         no ceiling observed.  Borrowing another cell compares different magnitudes. */
      if(c->valid){
          CAP2.ceil_iops=c->iops; CAP2.ceil_bps=c->bps; CAP2.ceil_util=c->util;
          CAP2.ceil_qd=c->qd; CAP2.ceil_when=c->when; CAP2.ceil_valid=1;
      } else CAP2.ceil_valid=0;
    }
    if(CAP2.ceil_valid && CAP2.ceil_iops>0) CAP2.headroom_pct = 100.0*cur_iops/CAP2.ceil_iops;
}

/* ---- Collection entry point, once at the end of sample_once ---- */
/* The disks under the DB's own files, as the install account (no root): the target's open
   volumes and logs (basename <db>, <db>_*), or - when its fds are another account's - the
   volume and log directories in $CUBRID_DATABASES/databases.txt.  A file's block device is
   found by st_dev in /sys/dev/block, else by the mount's /dev source, else by statfs f_fsid:
   overlay passes its upper filesystem's fsid through, and XFS puts its device number there
   (ext4/btrfs put a UUID hash - those stay unresolved).  A partition maps to its disk, dm/md
   through slaves to the physical disks.  Cached until the target's fd count changes. */
static void disk_leaves(const char *sys,char out[][32],int *n,int max,int depth){
    char sl[PATH_MAX+16], rp[PATH_MAX], b[16];
    if(depth>4 || !realpath(sys,rp)) return;
    snprintf(sl,sizeof sl,"%.4000s/partition",rp);
    if(read_file(sl,b,sizeof b)>0){ char *q=strrchr(rp,'/'); if(q) *q=0; }
    snprintf(sl,sizeof sl,"%.4000s/slaves",rp);
    DIR *d=opendir(sl); int any=0;
    if(d){ struct dirent *e;
           while((e=readdir(d))){ if(e->d_name[0]=='.') continue; any=1;
               char sub[PATH_MAX+300]; snprintf(sub,sizeof sub,"%.4000s/%.255s",sl,e->d_name);
               disk_leaves(sub,out,n,max,depth+1); }
           closedir(d); }
    if(any) return;
    const char *nm=strrchr(rp,'/'); nm=nm?nm+1:rp;
    for(int i=0;i<*n;i++) if(!strcmp(out[i],nm)) return;
    if(*n<max) snprintf(out[(*n)++],32,"%.31s",nm);
}
static int blk_sys(unsigned ma,unsigned mi,char *sys,size_t sn){
    snprintf(sys,sn,"/sys/dev/block/%u:%u",ma,mi); return access(sys,F_OK)==0;
}
/* 1 = st_dev, 2 = mount source, 3 = statfs fsid, 0 = none */
static int file_blkdev(const char *path,char *sys,size_t sn){
    struct stat st; if(stat(path,&st)!=0) return 0;
    if(blk_sys(major(st.st_dev),minor(st.st_dev),sys,sn)) return 1;
    FILE *mf=fopen("/proc/self/mountinfo","re"); char ln[4096]; int found=0;
    while(mf && !found && fgets(ln,sizeof ln,mf)){ unsigned ma,mi;
        if(sscanf(ln,"%*s %*s %u:%u",&ma,&mi)!=2 || ma!=major(st.st_dev) || mi!=minor(st.st_dev)) continue;
        char *sep=strstr(ln," - "), fs[64], src[512]; struct stat bs;
        if(sep && sscanf(sep+3,"%63s %511s",fs,src)==2 && !strncmp(src,"/dev/",5)
           && stat(src,&bs)==0 && S_ISBLK(bs.st_mode) && blk_sys(major(bs.st_rdev),minor(bs.st_rdev),sys,sn)) found=2;
        break; }
    if(mf) fclose(mf);
    if(found) return found;
    struct statfs sf; if(statfs(path,&sf)!=0) return 0;
    uint64_t v; int32_t f[2]; memcpy(f,&sf.f_fsid,sizeof f); v=(uint32_t)f[0]|((uint64_t)(uint32_t)f[1]<<32);
    unsigned ma=(v>>8)&0xfff, mi=(v&0xff)|((v>>12)&0xfff00);      /* huge_encode_dev */
    if(v>>32 || !ma || !blk_sys(ma,mi,sys,sn)) return 0;
    char zp[PATH_MAX+16], zb[32]; snprintf(zp,sizeof zp,"%.4000s/size",sys);   /* a hash rarely decodes to a device large enough */
    if(read_file(zp,zb,sizeof zb)<=0 || (double)sf.f_blocks*sf.f_bsize > strtod(zb,NULL)*512.0) return 0;
    return 3;
}
static int g_dk_via;                                /* best resolution used: 1 st_dev, 2 mount, 3 fsid */
static int db_disks(int pid,const char *db,char out[][32],int max,char *why,size_t wn){
    static int c_pid=-1, c_nfd=-2, c_n=0, c_via=0; static char c_db[64], c_out[8][32], c_why[160];
    char p[64], paths[16][PATH_MAX]; int np=0, files=0, nfd=-1; size_t dl=strlen(db);
    snprintf(p,sizeof p,"/proc/%d/fd",pid);
    DIR *d=opendir(p); struct dirent *e;
    if(d){ nfd=0; while((e=readdir(d))) if(e->d_name[0]!='.') nfd++; }
    if(pid==c_pid && nfd==c_nfd && !strcmp(db,c_db)){
        if(d) closedir(d);
        for(int i=0;i<c_n && i<max;i++) memcpy(out[i],c_out[i],32);
        snprintf(why,wn,"%s",c_why); g_dk_via=c_via; return c_n<max?c_n:max; }
    if(d){ rewinddir(d);
        while((e=readdir(d)) && np<16){
            if(e->d_name[0]=='.') continue;
            char fp[400], lk[PATH_MAX]; snprintf(fp,sizeof fp,"%s/%s",p,e->d_name);
            ssize_t k=readlink(fp,lk,sizeof lk-1); if(k<=0) continue; lk[k]=0;
            if(lk[0]!='/') continue;
            const char *bn=strrchr(lk,'/')+1;
            if(strncmp(bn,db,dl) || (bn[dl] && bn[dl]!='_')) continue;   /* <db>, <db>_lgat, <db>_x001 ... */
            struct stat st; if(stat(fp,&st)!=0 || !S_ISREG(st.st_mode)) continue;
            files++; snprintf(paths[np++],PATH_MAX,"%s",fp);           /* via the fd: right in any mount namespace */
        }
        closedir(d); }
    if(!files){                                     /* another account's server: its databases.txt entry */
        const char *dbs=getenv("CUBRID_DATABASES"); char tp[PATH_MAX], ln[4096];
        FILE *f=NULL; if(dbs){ snprintf(tp,sizeof tp,"%.4000s/databases.txt",dbs); f=fopen(tp,"re"); }
        while(f && fgets(ln,sizeof ln,f)){ char nm[256], vp[2048], hs[256], lp[2048];
            int nf=sscanf(ln,"%255s %2047s %255s %2047s",nm,vp,hs,lp);   /* name vol-path host [log-path] */
            if(ln[0]=='#' || nf<2 || strcmp(nm,db)) continue;
            snprintf(paths[np++],PATH_MAX,"%s",vp); files++;
            if(nf>=4 && strcmp(lp,vp) && np<16) snprintf(paths[np++],PATH_MAX,"%s",lp);   /* lp is set only with 4 fields */
            break; }
        if(f) fclose(f); }
    int n=0, via=0; char sys[PATH_MAX];
    for(int i=0;i<np;i++){ int v=file_blkdev(paths[i],sys,sizeof sys);
        if(v){ if(v>via) via=v; disk_leaves(sys,out,&n,max,0); } }
    if(n) why[0]=0;
    else if(!files) snprintf(why,wn,"%s",g_ascii?"DB files not found (no open file readable, no databases.txt entry)"
                                                 :"DB 파일 위치 확인 불가(열린 파일 읽기 불가·databases.txt 항목 없음)");
    else snprintf(why,wn,"%s",g_ascii?"DB files are on no identifiable block device (tmpfs, network, overlay over ext4/btrfs)"
                                      :"DB 파일의 블록 장치 식별 불가(tmpfs·네트워크·ext4/btrfs 위 overlay)");
    g_dk_via=via; c_via=via;
    c_pid=pid; c_nfd=nfd; snprintf(c_db,sizeof c_db,"%s",db); c_n=n<8?n:8;
    for(int i=0;i<c_n;i++) snprintf(c_out[i],32,"%s",out[i]);
    snprintf(c_why,sizeof c_why,"%s",why);
    return n;
}
/* Admissions when the page set was not read: a large buffer (above PG_LARGE) is scanned only
   on request, so there is no set difference.  The engine's ioread counter is the same
   quantity - pages read into the buffer - when a watcher keeps it running and two frames
   give a rate (live).  Re-admit stays unmeasured: that needs the page set. */
static void cap_miss_from_engine(void){
    if(CAP2.miss_pps>=0){ snprintf(CAP2.miss_src,sizeof CAP2.miss_src,g_miss_sample?"sample":"scan"); return; }
    snprintf(CAP2.miss_src,sizeof CAP2.miss_src,"-");
    if(CAP2.ps_ok && !strcmp(CAP2.hit_src,"perfmon")){      /* a per-second delta, not the cumulative figure */
        CAP2.miss_pps=CAP2.pb_ioread_ps; snprintf(CAP2.miss_src,sizeof CAP2.miss_src,"perfmon"); }
}
static void cap_collect(proc_t*P,iod_t*IO,const dstat_t*d0,int nd0,const dstat_t*d1,int nd1,double dt){
    CAP2.read_bps=IO->rb; CAP2.write_bps=IO->wb; CAP2.riops=IO->ri; CAP2.wiops=IO->wi;
    CAP2.dev[0]=0; CAP2.dev_why[0]=0; CAP2.dev_util=0; g_dk_via=0;
    CAP2.await_ms=CAP2.r_await_ms=CAP2.w_await_ms=-1; CAP2.qdepth=-1; CAP2.dev_iops=0;
    /* The device judged is the DB's own disk, the busiest of them - not the host's busiest:
       a backup saturating another disk says nothing about the DB's storage */
    { char dk[8][32]; int nk = P->pid>=0 ? db_disks(P->pid,P->db,dk,8,CAP2.dev_why,sizeof CAP2.dev_why) : 0;
      if(P->pid<0) snprintf(CAP2.dev_why,sizeof CAP2.dev_why,"%s",g_ascii?"server not running":"서버 미기동");
      double best=-1;
      for(int k=0;k<nk && dt>0;k++)
          for(int i=0;i<nd1;i++) if(!strcmp(d1[i].name,dk[k]))
              for(int j=0;j<nd0;j++) if(!strcmp(d0[j].name,dk[k])){
                  double u=UDELTA(d1[i].io_ms,d0[j].io_ms)/(dt*1000.0)*100.0; if(u>100) u=100;
                  if(u>best){ best=u; memcpy(CAP2.dev,dk[k],32); CAP2.dev_util=u; }
                  break; }
      if(nk>0 && !CAP2.dev[0]) snprintf(CAP2.dev_why,sizeof CAP2.dev_why,g_ascii?"DB disk %s not in diskstats":"DB 디스크 %s 가 diskstats 에 없음",dk[0]); }
    /* Latency, queue and device IOPS, from the diskstats delta of that disk */
    if(CAP2.dev[0] && dt>0){
        const dstat_t *a=NULL,*b2=NULL;
        for(int i=0;i<nd0;i++) if(!strcmp(d0[i].name,CAP2.dev)){ a=&d0[i]; break; }
        for(int i=0;i<nd1;i++) if(!strcmp(d1[i].name,CAP2.dev)){ b2=&d1[i]; break; }
        if(a&&b2){
            double dr=UDELTA(b2->rios,a->rios), dw=UDELTA(b2->wios,a->wios);
            double tr=UDELTA(b2->rticks,a->rticks), tw=UDELTA(b2->wticks,a->wticks);
            CAP2.dev_iops=(dr+dw)/dt;
            /* Device bytes for the profile decision; diskstats sectors are always 512B.
               Do not mix these with the process-attributed read_bps/write_bps: a
               process numerator over a device-wide denominator gives a nonsensical
               request size (0.0KB measured). */
            CAP2.dev_rbps=UDELTA(b2->rsect,a->rsect)*512.0/dt;
            CAP2.dev_wbps=UDELTA(b2->wsect,a->wsect)*512.0/dt;
            if(dr+dw>0) CAP2.await_ms=(tr+tw)/(dr+dw);
            if(dr>0)    CAP2.r_await_ms=tr/dr;
            if(dw>0)    CAP2.w_await_ms=tw/dw;
            if(b2->tiq>=a->tiq) CAP2.qdepth=(double)(b2->tiq-a->tiq)/(dt*1000.0);
        }
    }
    /* Hit ratio from the counters when they are running, otherwise from turnover */
    CAP2.hit_pct=-1; snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"-");
    CAP2.ps_ok=0; CAP2.ps_why[0]=0;
    { static uint64_t prev[CPS_N]; static int have=0; static unsigned seen=0;
      uint64_t v[CPS_N]={0};
      if(seen!=g_seg_gen){ seen=g_seg_gen; have=0; }
      if(cap_pstat_read(P,v)){
          /* One-shot has no delta baseline, but the fact that the counters are running and the
             cumulative hit ratio are still absolute values.  Per-second figures (_ps)
             are filled only in live mode. */
          /* A watcher that just attached has only a handful of fetch samples, making the
             cumulative hit ratio a false 0%.  It is reported only above 10,000 samples -
             an invented 0% is not published as a metric. */
          if(!have && v[CPS_PB_FETCH]>=10000){
              CAP2.ps_ok=1;
              CAP2.hit_pct=(1.0-(double)v[CPS_PB_IOREAD]/(double)v[CPS_PB_FETCH])*100.0;
              snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"perfmon-cum");
              snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                       g_ascii?"counters running (watcher present) - cumulative hit ratio; rates need live mode"
                              :"카운터 동작 중(watcher 있음) — 누적 히트율, 초당값은 라이브에서");
          } else if(!have && v[CPS_PB_FETCH]>0){
              snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                       g_ascii?"counters just started (%llu fetches) - too few to rate; using turnover"
                              :"카운터 방금 시작(fetch %llu건) — 표본 부족, 회전율로 대체",
                       (unsigned long long)v[CPS_PB_FETCH]);
          }
          if(have && dt>0){
              double f=(double)(v[CPS_PB_FETCH]-prev[CPS_PB_FETCH]);
              double r=(double)(v[CPS_PB_IOREAD]-prev[CPS_PB_IOREAD]);
              CAP2.pb_fetch_ps=f/dt; CAP2.pb_ioread_ps=r/dt;
              CAP2.pb_iowrite_ps=(double)(v[CPS_PB_IOWRITE]-prev[CPS_PB_IOWRITE])/dt;
              CAP2.log_iowrite_ps=(double)(v[CPS_LOG_IOWRITE]-prev[CPS_LOG_IOWRITE])/dt;
              CAP2.commit_ps=(double)(v[CPS_COMMIT]-prev[CPS_COMMIT])/dt;
              if(f>0){ CAP2.ps_ok=1; CAP2.hit_pct=(1.0-r/f)*100.0;
                       snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"perfmon");
                       snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"counters running (watcher present)":"카운터 동작 중(watcher 있음)"); }
              else snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                            g_ascii?"no fetches this window: no watcher (statdump/monitor off) or an idle database - using turnover"
                                   :"이번 구간 fetch 증가 없음: watcher 없음(statdump·모니터 미가동) 또는 유휴 DB — 회전율로 대체");
          }
          memcpy(prev,v,sizeof prev); have=1;
      } else if(!CAP2.ps_why[0]) snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"perfmon unreadable":"perfmon 읽기 불가");
    }
    /* Live (two or more frames) uses the in-memory baseline; one-shot uses the --bcb-dump file */
    { static int frames=0;
      double nowd=(double)PG.when.tv_sec+(double)PG.when.tv_nsec/1e9;
      double keep_t=CAP2.turnover_pct, keep_m=CAP2.miss_pps, keep_r=CAP2.readmit_pct, keep_f=CAP2.adm_frac;
      cap_turnover(nowd>0?nowd:(double)time(NULL));
      /* Keep the value pgbuf_scan already produced from the dump baseline (one-shot) */
      if(CAP2.turnover_pct<0 && keep_t>=0){ CAP2.turnover_pct=keep_t; CAP2.miss_pps=keep_m; CAP2.readmit_pct=keep_r; CAP2.adm_frac=keep_f; }
      frames++; }
    cap_miss_from_engine();
    if(CAP2.hit_pct<0 && CAP2.turnover_pct>=0 && PG.ok){
        /* Zero turnover means no new pages entered in the window: the working set has settled into the buffer */
        /* Turnover-based proxy for hit ratio: pages loaded per second are misses.  The
           fetch count is not observable, so the denominator is the buffer size and the
           figure reads as "what share of the buffer was newly filled in the window".
           It is not an absolute hit ratio, so its source is shown alongside. */
        CAP2.hit_pct = 100.0 - CAP2.turnover_pct;
        snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"turnover");
    }
    cap_ceiling();
}

/* ---------------- Render: time-series plots ---------------- */
/* History field selector, chosen by enum rather than index. */
enum { HK_OSUSED=0,HK_CUB,HK_SRV_RSS,HK_DB,HK_LG,HK_DYN,HK_ETC,
       HK_VSZ,HK_RSS,HK_DYNMAP,HK_MINFLT,HK_MAJFLT,
       HK_RBPS,HK_WBPS,HK_DEVUTIL,HK_ABSORB,HK_CPU,
       /* CPU and I/O series, so every dashboard metric is also plotted */
       HK_BUSYC,HK_SATC,HK_WA,HK_LOAD1,HK_DTHR,HK_BLKPCT,HK_WIOPS,
       HK_CMASTER,HK_CPL,HK_CBROKER,HK_CCAS,
       /* Merged into the four panels: buffer pool, server PSS, server core count */
       HK_SRVPSS,HK_CSERVER,HK_PGRES,HK_PGDIRTY,HK_PGCOLD,HK_PGSPAN,HK_PGLAG,
       /* Capacity axis */
       HK_CAPIOPS,HK_CAPAWAIT,HK_CAPHIT,HK_CAPMISS,HK_CAPHEAD,
       HK_HEAPB, HK_CAPREADMIT };   /* Dynamic memory B total and refill percentage */
static double hs_val(const hsam_t*s,int k){
    switch(k){
      case HK_OSUSED: return s->osused;  case HK_CUB:    return s->cub;
      case HK_SRV_RSS:return s->srv_rss; case HK_DB:     return s->db;
      case HK_LG:     return s->lg;      case HK_DYN:    return s->dyn;
      case HK_ETC:    return s->etc;     case HK_VSZ:    return s->vsz;
      case HK_RSS:    return s->rss;     case HK_DYNMAP: return s->dynmap;
      case HK_MINFLT: return s->minflt;
      case HK_MAJFLT: return s->majflt;  case HK_RBPS:   return s->rbps;
      case HK_WBPS:   return s->wbps;    case HK_DEVUTIL:return s->devutil;
      case HK_ABSORB: return s->absorb;  case HK_CPU: return s->cpu;
      case HK_BUSYC:  return s->busy_cores; case HK_SATC:  return s->sat_cores;
      case HK_WA:     return s->cpu_wa;     case HK_LOAD1: return s->load1;
      case HK_DTHR:   return s->d_thr;      case HK_BLKPCT:return s->blkpct;
      case HK_WIOPS:  return s->wiops;
      case HK_CMASTER:return s->c_master;   case HK_CPL:   return s->c_pl;
      case HK_CBROKER:return s->c_broker;   case HK_CCAS:  return s->c_cas;
      case HK_SRVPSS: return s->srv;        case HK_CSERVER:return s->cpu/100.0;
      case HK_PGRES:  return s->pg_res_pct; case HK_PGDIRTY:return s->pg_dirty;
      case HK_PGCOLD: return s->pg_cold_pct;case HK_PGSPAN: return s->pg_span;
      case HK_PGLAG:  return s->pg_lag;
      case HK_HEAPB:  return s->heapb;
      case HK_CAPREADMIT: return s->cap_readmit;
      case HK_CAPIOPS: return s->cap_iops;  case HK_CAPAWAIT:return s->cap_await;
      case HK_CAPHIT:  return s->cap_hit;   case HK_CAPMISS: return s->cap_miss;
      case HK_CAPHEAD: return s->cap_head;
      default: return 0.0;
    }
}
/* Value format: 0 bytes, 1 percent, 2 bytes per second, 3 events per second */
static const char* pf_val(double v,int fmt){
    static char b[8][32]; static int t=0; char*o=b[t=(t+1)&7];
    if(fmt==0)      snprintf(o,32,"%s",H(v));          /* Bytes, unit chosen automatically */
    else if(fmt==1) snprintf(o,32,"%.0f%%",v);          /* Percentage */
    else if(fmt==2) snprintf(o,32,"%s/s",H(v));         /* Bytes per second */
    else if(fmt==4) snprintf(o,32,v>=10?"%.0f":"%.1f",v); /* A count: cores, threads, load */
    else            snprintf(o,32,v>=10?"%.0f/s":"%.1f/s",v);  /* Events per second */
    return o;
}
typedef struct { int key,col,fmt; const char*lab; } pser_t;
#define PLOT_AXW 9      /* Width of the left-hand vertical ruler */

/* One panel = box + three vertical ticks + multi-series chart + one legend row.
   With norm!=0 each series is normalized to its own maximum: overlaying series with
   different units (%, per second) on one axis flattens the smaller to the floor. */
/* ov overlays one secondary series in a different unit (load and the like),
   normalized to its own maximum, with the real value left in the legend.
   verdict is one line under the legend (colour vcol); NULL draws no row. */
static void ticks_bottom(int x0,int w,int yb);
/* ticks draws one time-ruler row under the chart and above the legend, so each panel
   carries its own axis and a point is dated without looking down the screen. */
/* ---- Secondary bar subchart (zoom only) ----
   Like a price chart with volume beneath it: putting the quantity that causes the
   zoomed panel's metric below it lets cause and effect be read on one time axis.
   Drawn with block glyphs, visually distinct from the braille line above.  Each
   series is normalized to its own maximum (the units differ) and the legend
   carries the real values. */
/* hrows is the row count given to ONE series: 1 draws a single-row bar, more draws
   a taller one.  Zoom grows the main chart to 40 rows, and an effect metric pinned
   to one row would look flattened beside it. */
/* Four-dot braille bars.  Block characters gave one sample per cell; braille
   carries two horizontally, doubling time resolution at the same width, and
   shares a glyph family with the line chart above.  Vertically four steps per
   cell.  The BRD[col][row] bitmap is shared with the line chart, so the two
   cannot drift apart.  Bars fill from the bottom, lighting every dot below the
   height, so they read as area rather than line. */
static void cv_subbars(int x,int y,int w,const pser_t*se,int ns,int hrows){
    if(ns<1||w<4||hrows<1) return;
    int gx=x+1+PLOT_AXW, gw=w-2-PLOT_AXW;
    if(gw<2) return;
    if(hrows<1) hrows=1;
    for(int si=0; si<ns; si++){
        /* Percentage series (fmt==1) are drawn against 100: normalizing to their own maximum
           fills the bar even when the value sits at a steady 0.4%%, which reads as
           "full".  Other units (B/s, counts) have no absolute reference and use their
           own maximum. */
        double mx = (se[si].fmt==1) ? 100.0 : 0.0;
        if(mx<=0) for(int j=0;j<HN;j++){ double v=hs_val(&HS[j],se[si].key); if(v>mx) mx=v; }
        /* Tick widths are display widths.  Cutting bytes with %.*s splits a Hangul label
           mid-sequence and leaves a broken character; cutf respects both the character
           boundary and the width. */
        const char*lab=cutf(se[si].lab?se[si].lab:"",PLOT_AXW);
        int lrow=y+si*hrows+hrows-1;          /* Bottom row of the series block */
        cv_put(x+1,lrow,padf(lab,PLOT_AXW,1),"38;5;240");
        if(HN<1) continue;
        /* Where several samples share a cell, take the maximum - a spike is not averaged away
           (the same rule as the line chart's peak-preserving downsample). */
        /* Reserve the right-hand value slot in advance so a bar cannot cover the number. */
        double lastv = HN>0 ? hs_val(&HS[HN-1],se[si].key) : 0;
        char vtx[24]; snprintf(vtx,sizeof vtx,"%s",lastv<0?"-":pf_val(lastv,se[si].fmt));   /* -1 = not measured */
        int vw=(int)strlen(vtx)+1;
        int bw=gw-vw; if(bw<2) bw=gw;
        /* Braille cell buffer: hrows rows by bw columns for this series, two samples per cell horizontally. */
        static unsigned char bcell[8][CV_W];
        if(hrows>8) hrows=8;
        for(int r=0;r<hrows;r++) for(int c=0;c<bw && c<CV_W;c++) bcell[r][c]=0;
        int px=bw*2, py=hrows*4;                    /* Dot-level resolution */
        for(int i=0;i<px;i++){
            int a=(int)((double)i*HN/px), b=(int)((double)(i+1)*HN/px);
            if(b>HN) b=HN;
            if(b<=a) b=a+1;
            double val=0;
            for(int k=a;k<b && k<HN;k++){ double v=hs_val(&HS[k],se[si].key); if(v>val) val=v; }
            if(mx<=0||val<=0) continue;
            double fr=val/mx; if(fr>1) fr=1;
            int hpx=(int)(fr*py+0.5);               /* Height in dots from the bottom */
            if(hpx<1) hpx=1;                        /* At least one dot for a non-zero value, so it cannot vanish */
            if(hpx>py) hpx=py;
            for(int d=0;d<hpx;d++){
                int yy=py-1-d;                      /* Filled bottom up */
                int cx=i/2, cy=yy/4;
                if(cx<bw && cx<CV_W && cy<hrows) bcell[cy][cx]|=(unsigned char)BRD[i%2][yy%4];
            }
        }
        { char col[16]; snprintf(col,sizeof col,"38;5;%d",se[si].col);
          for(int r=0;r<hrows;r++) for(int c=0;c<bw && c<CV_W;c++){
              if(!bcell[r][c]) continue;
              char g[8]; unsigned cp=0x2800u|bcell[r][c];
              g[0]=(char)(0xE0|(cp>>12)); g[1]=(char)(0x80|((cp>>6)&0x3F)); g[2]=(char)(0x80|(cp&0x3F)); g[3]=0;
              cv_put(gx+c,y+si*hrows+r,g,col);
          } }
        /* Current value at the right: the bar gives the shape, the number is read here */
        int vx=x+w-2-(int)strlen(vtx);
        if(vx>gx+2){ char cc[16]; snprintf(cc,sizeof cc,"38;5;%d",se[si].col); cv_put(vx,lrow,vtx,cc); }
    }
}
/* bars/nb draws the cause-metric bars INSIDE the box, below the time axis and above
   the legend.  Outside the box they read as a separate element, detached from the
   graph. */
static void cv_panel2(int x,int y,int w,int ch,const char*title,const char*rtitle,
                      const pser_t*se,int ns,double fixed_ymax,int norm,const char*note,
                      const pser_t*ov,const char*verdict,const char*vcol,int ticks,
                      const pser_t*bars,int nb,int bpr){
    static double buf[CV_NS][HIST_MAX];
    const double*vp[CV_NS]={0}; int colv[CV_NS]={0};
    pser_t sa[CV_NS]; int nt;
    if(ns>CV_NS-1) ns=CV_NS-1;
    memcpy(sa,se,sizeof(pser_t)*(size_t)ns); nt=ns;
    if(ov){ sa[nt++]=*ov; }
    int extra = (verdict?1:0) + (ticks?1:0) + (nb>0?nb*(bpr>0?bpr:1):0);
    /* Footnotes come after the legend: when width runs short the footnote goes and the
       series values stay (the footnote is also in the help, the values are not). */
    cv_box2(x,y,w,ch+3+extra,title,rtitle);
    int gx=x+1+PLOT_AXW, gw=w-2-PLOT_AXW;
    if(gw<2) return;
    int ty_ = y+ch+1;              /* Ruler, then cause bars, then legend, then verdict */
    int by_ = ty_ + (ticks?1:0);
    int ly_ = by_ + (nb>0?nb*(bpr>0?bpr:1):0);
    if(ticks) ticks_bottom(gx,gw,ty_);
    if(nb>0) cv_subbars(x,by_,w,bars,nb,bpr>0?bpr:1);
    if(verdict) cv_put(x+2,ly_+1,cutf(verdict,w-4),vcol?vcol:"38;5;244");
    if(HN<2){
        cv_put(gx,y+1+ch/2,cutf(L("표본이 부족합니다 — 선을 그리려면 2개 이상 필요"),gw),"38;5;244");
        return;
    }
    double gmax=fixed_ymax;
    if(!norm && gmax<=0){
        for(int i=0;i<ns;i++) for(int j=0;j<HN;j++){
            double v=hs_val(&HS[j],se[i].key); if(v>gmax) gmax=v; }
        gmax*=1.15;
    }
    if(gmax<=0) gmax=1;
    for(int i=0;i<nt;i++){
        double m=0; int nrm = norm || (i>=ns);      /* Secondary series are always normalized */
        if(nrm){ for(int j=0;j<HN;j++){ double v=hs_val(&HS[j],sa[i].key); if(v>m)m=v; }
                 m*=1.15; if(m<=0)m=1; }
        for(int j=0;j<HN;j++){
            double v=hs_val(&HS[j],sa[i].key);
            buf[i][j] = nrm ? v/m*gmax : v;
        }
        vp[i]=buf[i]; colv[i]=sa[i].col;
    }
    cv_chart_n(gx,y+1,gw,ch,vp,colv,nt,HN,gmax);
    se=sa; ns=nt;   /* The legend below includes the secondary series */
    /* Vertical ruler: a normalized panel has no common axis, so drawing one would lie */
    if(!norm){
        /* The chart occupies y+1..y+ch; the mid label goes on its centre row, or the ruler lies. */
        cv_put(x+1,y+1,  padf(pf_val(gmax,se[0].fmt),PLOT_AXW,1),"38;5;240");
        /* Mid ticks sit on the cell the line actually crosses.  cv_chart_n plots with
           yy=(py-1)*(1-f)+0.5, so applying that at f=0.5 keeps label and line aligned. */
        if(ch>=3){ int mid=(int)((ch*4-1)*0.5+0.5)/4;
                   cv_put(x+1,y+1+mid,padf(pf_val(gmax/2,se[0].fmt),PLOT_AXW,1),"38;5;240"); }
        cv_put(x+1,y+ch, padf(pf_val(0,se[0].fmt),PLOT_AXW,1),"38;5;240");
    }else{
        cv_put(x+1,y+1+ch/2,padf(g_ascii?"norm":"정규화",PLOT_AXW,1),"38;5;240");
    }
    /* Legend: current value and (min-max) per series.  When it does not fit, shorten the
       names, then drop the ranges - a coloured line with no name cannot be read. */
    {
    /* The legend is a blank row below the vertical ticks, so there is no reason to reserve
       the tick width (PLOT_AXW); giving it the full inner width lets a five-series
       panel show its values. */
    int lgx=x+2, ly=ly_, lend=x+w-1;
    char item[CV_NS][96]; int need[CV_NS];
    for(int pass=0;pass<3;pass++){
        int tot=0;
        for(int i=0;i<ns;i++){
            double cur=hs_val(&HS[HN-1],se[i].key), mn=cur,mx=cur;
            for(int j=0;j<HN;j++){ double v=hs_val(&HS[j],se[i].key);
                if(v<mn)mn=v;
                if(v>mx)mx=v; }
            if(pass==0)      snprintf(item[i],96,"%s %s (%s~%s)",se[i].lab,
                                      pf_val(cur,se[i].fmt),pf_val(mn,se[i].fmt),pf_val(mx,se[i].fmt));
            else if(pass==1) snprintf(item[i],96,"%s %s",se[i].lab,pf_val(cur,se[i].fmt));
            else             snprintf(item[i],96,"%s",se[i].lab);
            need[i]=disp_w(item[i])+2;
            tot+=need[i]+1;
        }
        /* No trailing space is needed after the last entry; returning it keeps a legend that
           just fits from being abbreviated one step further. */
        if(lgx+tot-1 <= lend || pass==2) break;
    }
    /* Right-aligned, so the eye scans the same x in every panel.  When the legend does
       not fit, keep as many leading entries as do, hold that group to the right and
       mark the elision with a leading ellipsis - falling back to left alignment
       would leave one panel's legend out of line with the rest. */
    int lx=lgx;
    int nw=(note&&*note)?disp_w(note)+3:0;   /* Footnote slot plus one space */
    int room=lend-nw-lgx;                    /* Cells available to the legend */
    int nfit=0, tot=0;
    for(int i=0;i<ns;i++){
        int add=need[i]+(nfit?1:0);
        int mark=(i<ns-1)?2:0;               /* If more follows, room for an ellipsis and a space */
        if(tot+add+mark>room) break;
        tot+=add; nfit++;
    }
    if(nfit<ns && nfit>0) tot+=2;            /* "… " */
    { int start=lend-nw-tot; if(start>lgx) lx=start; }
    if(nfit<ns && nfit>0){ cv_put(lx,ly,"…","38;5;240"); lx+=2; }
    for(int i=0;i<nfit;i++){
        char c[16]; snprintf(c,sizeof c,"38;5;%d",se[i].col);
        cv_put(lx,ly,"●",c); cv_put(lx+1,ly,item[i],"38;5;250");
        lx+=need[i]+1;
    }
    if(nfit==0 && ns>0 && lx+1<lend) cv_put(lx,ly,"…","38;5;240");
    if(note && *note){
        int nw=disp_w(note);
        /* One space, so it does not touch the border */
        if(lx+nw+1 <= lend) cv_put(lend-nw-1,ly,note,"38;5;240");
    }
    }
}

/* ---- One-line verdict, per panel ----
   Follows the I/O box convention: cite only signals that actually hold, set
   thresholds relative to the window and the core count rather than absolutely, and
   state plain facts when uncertain (the warning mark is for actionable signals). */
static const char *VD_WARN="1;38;5;203", *VD_NOTE="38;5;215", *VD_OK="38;5;79", *VD_INFO="38;5;244";
static double vd_win(void){ return HN>1 ? HS[HN-1].t-HS[0].t : 0; }
static void vd_wtxt(char*o,size_t n){ double w=vd_win();
    if(w>=60) snprintf(o,n,"%dm%02ds",(int)(w/60),(int)w%60); else snprintf(o,n,"%.0fs",w); }
/* Evidence a sizing verdict rests on: the window and the sample count.  Under a minute the
   grade is provisional - a frame or two of one query is not a workload. */
static void vd_evid(char*o,size_t n){
    char w[24]; vd_wtxt(w,sizeof w);
    snprintf(o,n,g_ascii?"window %s/%d samples":"창 %s/%d표본",w,HN);
}
static const char *vd_prov(void){ return vd_win()<60 ? (g_ascii?"provisional, ":"잠정, ") : ""; }
/* The grade a re-admit share gives to enlarging the buffer (an estimate under LRU; the share
   itself is the measurement, the grade is the conclusion) */
static const char *vd_grade(double readmit){
    if(readmit>=30) return g_ascii?"enlarging helps a lot":"증설 효과 큼";
    if(readmit>=10) return g_ascii?"enlarging helps":"증설 효과 있음";
    return g_ascii?"enlarging helps little":"증설 효과 작음";
}
/* A: memory */
static void vd_mem(char*o,size_t n,const char**col){
    if(HN<4){ snprintf(o,n,"%s",g_ascii?"collecting samples...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*a=&HS[0],*b=&HS[HN-1];
    double d=b->srv-a->srv, pct=a->srv>0?d/a->srv:0, w=vd_win(), rate=w>0?d/w*3600:0;
    char wt[24]; vd_wtxt(wt,sizeof wt);
    char rt[32]; snprintf(rt,sizeof rt,"%s%s/h",rate>=0?"+":"-",H(rate<0?-rate:rate));
    int load_up = (b->load1 > a->load1*1.3 && b->load1 >= 1.0);
    double gap = b->vsz-b->rss;
    if(pct>0.05){
        if(load_up){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! server +%s (%s), load rising with it -> load-driven growth (buffers/work memory)"
                               :"⚠ 서버 +%s (%s) · load 도 함께 상승 → 부하 기인 증가(작업 메모리·캐시)",H(d),rt); }
        else { *col=VD_WARN;
            snprintf(o,n,g_ascii?"! server +%s (%s) while load is flat -> growth unrelated to load: check heap detail (B/A)"
                               :"⚠ 서버 +%s (%s) · load 평탄 → 부하와 무관한 증가 — 동적 메모리 상세(B/A)에서 어느 항목인지 확인",H(d),rt); }
    } else if(pct<-0.05){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"server -%s over %s - work memory / caches released":"· 서버 -%s (%s 동안) — 작업 메모리·캐시 반납",H(-d),wt);
    } else { *col=VD_OK;
        snprintf(o,n,g_ascii?"stable - server %s, change %s%s over %s%s":"· 안정 — 서버 %s, 변동 %s%s (%s)%s",
                 H(b->srv),d>=0?"+":"-",H(d<0?-d:d),wt,
                 (gap>2.5*b->rss && b->rss>0) ? (g_ascii?" / large reserved-only span (normal: stacks/arenas)":" / 예약만 한 분량 큼(정상: 스택·아레나)") : "");
    }
}
/* B: the fill side, volume to buffer */
static void vd_buf(char*o,size_t n,const char**col){
    if(HN<1){ snprintf(o,n,"%s",g_ascii?"collecting...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*b=&HS[HN-1];
    /* The buffer is judged by what it admits (misses), not by disk reads: CUBRID reads through the
       OS page cache, so a short buffer over a volume that fits in RAM misses with ~0 disk reads.
       Disk reads only say how much of the misses the page cache did not absorb. */
    double rb=b->rbps, ps=g_pagesize>0?(double)g_pagesize:16384.0;
    double ab = b->cap_miss>=0 ? b->cap_miss*ps : -1;          /* bytes/s admitted; -1 not measured */
    char disk[128], vc[40]="";
    if(b->vc_pct>=0) snprintf(vc,sizeof vc,g_ascii?", volumes %.0f%% in page cache":", 볼륨 페이지캐시 상주 %.0f%%",b->vc_pct);
    if(ab>0 && rb<ab*0.5) snprintf(disk,sizeof disk,g_ascii?"disk %s/s, the rest from the OS page cache%s":"디스크 %s/s, 나머지는 OS 페이지 캐시%s",H(rb),vc);
    else snprintf(disk,sizeof disk,g_ascii?"disk %s/s%s":"디스크 %s/s%s",H(rb),vc);
    if(!b->pg_ok){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"buffer-pool state unavailable - disk read %s/s, cache absorb %s":"· 버퍼풀 상태 읽기 비활성 — 디스크 읽기 %s/s, 캐시 흡수율 %s",
                 H(rb), b->absorb_ok?pf_val(b->absorb,1):(g_ascii?"idle":"유휴")); return; }
    if(b->pg_res_pct<90.0){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"buffer still filling (%.0f%% resident) - judgement deferred, %s"
                           :"· 버퍼 채우는 중 (상주 %.0f%%) — 판단 보류, %s",b->pg_res_pct,disk); return; }
    if(ab<0){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"admissions not measured (no page-scan baseline) - buffer not judged, %s"
                           :"· 버퍼 적재 미측정(페이지 스캔 기준 없음) — 버퍼 판단 보류, %s",disk); return; }
    if(ab>=8.0*1024*1024 && b->cap_readmit<0){ *col=VD_INFO;
        if(b->cap_mperf==1)   /* the rate is the engine's; the page set was not read */
            snprintf(o,n,g_ascii?"admitting %s/s by the engine counter (%s) - no page scan, so re-admit is not measured: d (--pgbuf-detail) tells shortage from scan"
                               :"· 적재 %s/s(엔진 카운터, %s) — 페이지 스캔이 없어 재적재율 미측정: d(--pgbuf-detail)로 켜면 버퍼 부족과 스캔 구분",H(ab),disk);
        else if(b->cap_mperf==2)   /* a large buffer between passes: the slot sample has no re-admit yet */
            snprintf(o,n,g_ascii?"admitting %s/s by the slot sample (%s) - re-admit comes with the next whole-array pass"
                               :"· 적재 %s/s(슬롯 표본, %s) — 재적재율은 다음 전수 스캔에서",H(ab),disk);
        else
            snprintf(o,n,g_ascii?"admitting %s/s (%s) - re-admit not measured, so shortage and scan are not told apart"
                               :"· 적재 %s/s(%s) — 재적재율 미측정이라 버퍼 부족과 스캔을 가르지 않음",H(ab),disk);
        return; }
    /* Re-admit = admitted pages evicted within the last nbuf evictions: under an exact LRU those
       are the misses a buffer twice the size would have served, so it estimates the share of
       misses doubling removes.  Only an estimate under that assumption: CUBRID inserts new pages
       at a midpoint, and pages admitted and evicted inside one frame are never seen, so it is no
       bound in either direction.  It does not tell a scan from a working set many times the
       buffer - both are low - so no "enlarging is useless" is concluded. */
    if(ab>=8.0*1024*1024 && b->cap_admfrac>=0.5){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! admitting %s/s (%s), %.0f%% of the buffer per frame - too fast to see between snapshots: re-admit %.0f%% and the admission rate are under-counted, low confidence (shorten --interval)"
                           :"⚠ 적재 %s/s(%s), 프레임당 버퍼의 %.0f%% — 스냅샷 사이를 다 보지 못하는 속도: 재적재 %.0f%%·적재량은 과소집계, 신뢰도 낮음(--interval 을 줄일 것)",
                 H(ab),disk,b->cap_admfrac*100,b->cap_readmit<0?0:b->cap_readmit); return; }
    char ev[48]; vd_evid(ev,sizeof ev);
    if(ab>=8.0*1024*1024 && b->cap_readmit>=30.0){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! buffer shortage - admitting %s/s (%s), re-admit %.0f%%: %s (%s%s, LRU assumed)"
                           :"⚠ 버퍼 부족 — 적재 %s/s(%s), 재적재 %.0f%%: %s(%s%s, LRU 가정)",H(ab),disk,b->cap_readmit,vd_grade(b->cap_readmit),vd_prov(),ev); return; }
    if(ab>=8.0*1024*1024 && b->cap_readmit>=10.0){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! working set larger than the buffer - admitting %s/s (%s), re-admit %.0f%%: %s (%s%s, LRU assumed)"
                           :"⚠ 작업집합이 버퍼보다 큼 — 적재 %s/s(%s), 재적재 %.0f%%: %s(%s%s, LRU 가정)",H(ab),disk,b->cap_readmit,vd_grade(b->cap_readmit),vd_prov(),ev); return; }
    if(ab>=8.0*1024*1024){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"admitting %s/s (%s), re-admit %.0f%% - reuse distance beyond the buffer (a scan, or a working set many times the buffer): %s (%s%s)"
                           :"· 적재 %s/s(%s) · 재적재 %.0f%% — 재사용 거리가 버퍼 밖(스캔이거나 작업집합이 버퍼의 수 배): %s(%s%s)",
                 H(ab),disk,b->cap_readmit,vd_grade(b->cap_readmit),vd_prov(),ev); return; }
    if(ab<1.0*1024*1024){ *col=VD_OK;
        snprintf(o,n,g_ascii?"served from buffer - admitting %.0f pages/s, %s, resident %.0f%%"
                           :"· 버퍼에서 처리 중 — 적재 초당 %.0f페이지, %s, 상주 %.0f%%",b->cap_miss,disk,b->pg_res_pct); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"admitting %s/s (%s) - hot+warm %.0f%% cold %.0f%% - working set turning over":"· 적재 %s/s(%s) · hot+warm %.0f%% cold %.0f%% — 작업집합 교체 중",
             H(ab),disk,100.0-b->pg_cold_pct,b->pg_cold_pct);
}
/* Host-wide verdict: separates a CUBRID problem from a machine-level one.
   Device utilization is host-wide (diskstats), so it belongs here. */
static void vd_host(char*o,size_t n,const char**col,int ncores){
    if(HN<1){ snprintf(o,n,"%s",g_ascii?"collecting...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*b=&HS[HN-1];
    if(ncores<=0) ncores=1;
    double cubpct = b->ram>0 ? b->cub/b->ram*100 : 0;
    if(b->sat_cores>=1){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! %.0f core(s) saturated of %d - host CPU is the constraint (CUBRID %.0f%% of RAM)"
                           :"⚠ 포화 코어 %.0f/%d개 — 호스트 CPU 가 제약 (CUBRID 는 RAM 의 %.0f%%)",
                 b->sat_cores,ncores,cubpct); return; }
    if(b->load1 > ncores*1.2){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! load %.1f > %d cores - run queue waiting (host-wide, not only CUBRID)"
                           :"⚠ load %.1f > 코어 %d — 실행 대기 큐 존재(호스트 전체, CUBRID 만은 아님)",
                 b->load1,ncores); return; }
    if(b->devutil>=80){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! device %.0f%% busy - host storage is the constraint (see panels 3/4 for this instance)"
                           :"⚠ 장치 %.0f%% — 호스트 스토리지가 제약 (이 인스턴스 몫은 3·4번 패널에서)",
                 b->devutil); return; }
    if(b->cg_pct>=90){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! memory cgroup at %.0f%% of its limit - the OOM boundary is this limit, not host RAM (CUBRID %.0f%% of RAM)"
                           :"⚠ 메모리 cgroup 한도의 %.0f%% 사용 — OOM 기준은 호스트 RAM 이 아니라 이 한도 (CUBRID 는 RAM 의 %.0f%%)",
                 b->cg_pct,cubpct); return; }
    if(b->psi_full>=5){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! memory pressure: PSI full %.1f%% over 10s - tasks stalled on reclaim (host-wide)"
                           :"⚠ 메모리 압박 — PSI full(10초) %.1f%%: 회수 대기로 멈춘 시간 (호스트 전체)",
                 b->psi_full); return; }
    if(b->ram>0 && b->osused/b->ram>0.92){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! host RAM %.0f%% used - little headroom left (CUBRID %.0f%%)"
                           :"⚠ 호스트 RAM %.0f%% 사용 — 여유가 적다 (CUBRID 는 %.0f%%)",
                 b->osused/b->ram*100,cubpct); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"host ok - %.1f of %d cores, RAM %.0f%%, device %.0f%%, CUBRID %.0f%% of RAM"
                       :"· 호스트 정상 — 사용 %.1f/%d 코어, RAM %.0f%%, 장치 %.0f%%, CUBRID 는 RAM 의 %.0f%%",
             b->busy_cores,ncores,b->ram>0?b->osused/b->ram*100:0,b->devutil,cubpct);
}
/* Re-admit percentage; -1 = not measured */
static const char* readmit_s(double v){ static char b[24]; if(v<0) return "-"; snprintf(b,sizeof b,"%.0f%%",v); return b; }
/* Flush span in log pages; -1 = not known (no page scan this frame) */
static const char* span_s(double v){ static char b[24]; if(v<0) return "?"; snprintf(b,sizeof b,"%.0f",v); return b; }
/* DB disk utilization as a percentage; the caller substitutes a word when it is unknown */
static const char* dev_pct_s(double v){ static char b[24]; snprintf(b,sizeof b,"%.0f%%",v); return b; }
/* D: the drain side, buffer back to volume */
static void vd_flush(char*o,size_t n,const char**col){
    if(HN<1){ snprintf(o,n,"%s",g_ascii?"collecting...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*b=&HS[HN-1];
    double dmin=b->pg_dirty; for(int i=0;i<HN;i++) if(HS[i].pg_ok && HS[i].pg_dirty<dmin) dmin=HS[i].pg_dirty;
    /* The device here is the DB's own disk (the capacity axis's), never the host's busiest:
       a backup saturating another disk is not this database's write bottleneck.  Unknown
       (no DB disk resolved) means no device judgement at all. */
    const char*dv = b->dbdev_util>=0 ? dev_pct_s(b->dbdev_util) : (g_ascii?"-":"미확인");
    int dev_busy = b->dbdev_util>=80 && b->wbps>=1.0*1024*1024;
    if(!b->pg_ok){
        if(dev_busy){ *col=VD_WARN;
            snprintf(o,n,g_ascii?"! DB disk %s busy with %s/s writes - write bottleneck":"⚠ DB 디스크 %s · 쓰기 %s/s — 쓰기 병목",dv,H(b->wbps)); }
        else { *col=VD_INFO; snprintf(o,n,g_ascii?"write %s/s, DB disk %s (buffer-pool state unavailable)":"· 쓰기 %s/s, DB 디스크 %s (버퍼풀 상태 읽기 비활성)",H(b->wbps),dv); }
        return; }
    if(b->pg_lag>0){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! fsync delay %.0f log pg (append ahead of flushed) - commit latency risk":"⚠ fsync 지연 %.0f 로그pg (append 가 flushed 앞) — 커밋 지연 위험",b->pg_lag); return; }
    if(dev_busy){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! write bottleneck - DB disk %s, write %s/s, dirty %.0f pages":"⚠ 쓰기 병목 — DB 디스크 %s, 쓰기 %s/s, dirty %.0f 페이지",dv,H(b->wbps),b->pg_dirty); return; }
    if(b->pg_dirty>=100 && b->pg_dirty>1.5*dmin){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! dirty pages accumulating %.0f (flush delay %s log pg) - grows until checkpoint/flush catches up"
                           :"⚠ dirty 누적 %.0f 페이지 (flush 지연 %s 로그pg) — 체크포인트·flush 가 따라잡기 전까지 증가",b->pg_dirty,span_s(b->pg_span)); return; }
    if(b->pg_dirty==0){ *col=VD_OK;
        snprintf(o,n,g_ascii?"all reflected to volumes - dirty 0, no flush delay, write %s/s":"· 볼륨 반영 완료 — dirty 0, flush 지연 없음, 쓰기 %s/s",H(b->wbps)); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"dirty %.0f pages - flush delay %s log pages - DB disk %s - write %s/s":"· dirty %.0f 페이지 · flush 지연 span %s 로그pg · DB 디스크 %s · 쓰기 %s/s",
             b->pg_dirty,span_s(b->pg_span),dv,H(b->wbps));
}


/* ---- Capacity verdict: is the answer more, less, or tuning? ----
   Same convention as the I/O verdict: only signals that hold, thresholds relative to
   the allocated resource, and the warning mark for actionable signals. */
static void vd_capacity(char*o,size_t n,const char**col){
    const cap_t*c=&CAP2;
    if(c->over_budget){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! capacity aggregation skipped this frame (%.0fms > %.0fms) - values withheld"
                            :"⚠ 이 프레임 용량 취합 생략 (%.0fms > %.0fms) — 값 보류",c->ms,(double)CAP_BUDGET_MS);
        return; }
    /* A serial stream (depth < 2) waiting long is latency-bound whatever the headroom says:
       more IOPS do not shorten each wait, faster storage or more parallel requests do */
    if(c->qdepth>=0 && c->qdepth<2.0 && c->await_ms>=20.0 && c->dev_iops>=10){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! await %.1fms at %.0f IOPS with queue %.1f (serial) - latency-bound: faster storage or more parallel requests, not more IOPS"
                            :"⚠ 대기 %.1fms·IOPS %.0f·큐 %.1f(직렬) — 지연 병목: IOPS 증설이 아니라 더 빠른 스토리지 또는 병렬 요청",
                 c->await_ms,c->dev_iops,c->qdepth); return; }
    /* Headroom against the ceiling is the strongest conclusion, but headroom alone
       does not establish a limit - the ceiling is an observation from some other
       moment.  A real limit comes with the device falling behind: await >= 20ms or
       queue depth >= 10.  With neither, report "near ceiling" only and do not
       recommend more capacity. */
    if(c->headroom_pct>=0){
        if(c->headroom_pct>=80){
            int pressed = (c->await_ms>=20.0) || (c->qdepth>=10.0);
            if(pressed){ *col=VD_WARN;
                snprintf(o,n,g_ascii?"! IOPS %.0f of ceiling %.0f (%.0f%%, %s/%s) with await %.1fms q%.1f - at capacity: add IOPS or cut reads"
                                    :"⚠ IOPS %.0f / 상한 %.0f (%.0f%%, %s/%s), 대기 %.1fms 큐 %.1f — 용량 한계: IOPS 증설 또는 읽기 감축",
                         c->dev_iops,c->ceil_iops,c->headroom_pct,
                         CAP_BS_NAME[c->prof_bs],CAP_RW_NAME[c->prof_rw],
                         c->await_ms>=0?c->await_ms:0,c->qdepth>=0?c->qdepth:0); return; }
            *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! IOPS %.0f near ceiling %.0f (%.0f%%, %s/%s) but await %.1fms q%.1f is not queueing - near ceiling, not proven limited"
                                :"⚠ IOPS %.0f 로 상한 %.0f 근접 (%.0f%%, %s/%s) — 대기 %.1fms·큐 %.1f 로 적체 없음: 한계 단정 불가(상한 근접)",
                     c->dev_iops,c->ceil_iops,c->headroom_pct,
                     CAP_BS_NAME[c->prof_bs],CAP_RW_NAME[c->prof_rw],
                     c->await_ms>=0?c->await_ms:0,c->qdepth>=0?c->qdepth:0); return; }
        if(c->headroom_pct<=20 && c->dev_util<50){ *col=VD_OK;
            snprintf(o,n,g_ascii?"spare: IOPS %.0f of ceiling %.0f (%.0f%%), await %.1fms - storage is not the limit"
                                :"· 여유: IOPS %.0f / 상한 %.0f (%.0f%%), 대기 %.1fms — 스토리지가 제약이 아님",
                     c->dev_iops,c->ceil_iops,c->headroom_pct,c->await_ms>=0?c->await_ms:0); return; }
    }
    /* Past the latency threshold the answer is the storage tier, whatever the IOPS headroom */
    if(c->await_ms>=20.0 && c->dev_iops>=10){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! await %.1fms at only %.0f IOPS (q%.1f) - latency-bound: faster storage, not more IOPS"
                            :"⚠ 대기 %.1fms인데 IOPS %.0f (큐 %.1f) — 지연 병목: IOPS 증설보다 빠른 스토리지",
                 c->await_ms,c->dev_iops,c->qdepth>=0?c->qdepth:0); return; }
    /* Buffer verdict: sustained misses call for more, no turnover and no use makes it a candidate to shrink */
    /* Resident share from the scan, else from the engine's zone counts (a large buffer not scanned) */
    if((PG.ok || AG.ok) && c->miss_pps>=0){
        double res = PG.ok ? (PG.nbuf>0?100.0*PG.resident/PG.nbuf:0)
                           : (AG.nbuf>0?100.0*(double)(AG.z1+AG.z2+AG.z3)/AG.nbuf:0);
        int mperf = !strcmp(c->miss_src,"perfmon");
        if(c->readmit_pct<0 && c->miss_pps>=200 && !strcmp(c->miss_src,"sample")){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! %.0f pages/s admitted (slot sample) - re-read share comes with the next whole-array pass"
                                :"⚠ 초당 %.0f페이지 적재(슬롯 표본) — 재적재 비율은 다음 전수 스캔에서",c->miss_pps); return; }
        if(c->readmit_pct<0 && c->miss_pps>=200 && mperf){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! %.0f pages/s admitted (engine counter) - a large buffer not scanned, so re-read share unknown: d (--pgbuf-detail) tells buffer shortage from scan"
                                :"⚠ 초당 %.0f페이지 적재(엔진 카운터) — 페이지 스캔을 끈 대형 버퍼라 재적재 비율 미상: d(--pgbuf-detail)로 켜면 버퍼 부족과 스캔 구분",
                     c->miss_pps); return; }
        if(c->readmit_pct<0 && c->miss_pps>=200){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! %.0f pages/s admitted (turnover %.0f%%) - re-read share unknown from a file baseline: run live (-b) to tell buffer shortage from scan"
                                :"⚠ 초당 %.0f페이지 적재(회전율 %.0f%%) — 파일 기준선이라 재적재 비율 미상: 라이브(-b)로 버퍼 부족과 스캔을 구분",
                     c->miss_pps,c->turnover_pct); return; }
        /* Re-admit estimates the share of misses a doubled buffer removes, assuming LRU (see vd_buf) */
        char hit[48]="";
        if(c->adm_frac>=0.5 && c->miss_pps>=200){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! %.0f pages/s admitted, %.0f%% of the buffer per frame - under-counted between snapshots, re-admit %.0f%% low confidence: shorten --interval before sizing"
                                :"⚠ 초당 %.0f페이지 적재, 프레임당 버퍼의 %.0f%% — 스냅샷 사이 과소집계, 재적재 %.0f%% 신뢰도 낮음: 증설 판단 전에 --interval 을 줄일 것",
                     c->miss_pps,c->adm_frac*100,c->readmit_pct<0?0:c->readmit_pct); return; }
        if(c->hit_pct>=0 && !strncmp(c->hit_src,"perfmon",7))
            snprintf(hit,sizeof hit,g_ascii?", engine hit %.1f%%":", 엔진 히트율 %.1f%%",c->hit_pct);
        char ev[48]; vd_evid(ev,sizeof ev);
        if(c->miss_pps>=200 && c->readmit_pct>=30){ *col=VD_WARN;
            snprintf(o,n,g_ascii?"! buffer too small: %.0f pages/s admitted, %.0f%% re-reads%s - %s (%s%s, LRU assumed): raise data_buffer_size"
                                :"⚠ 버퍼 부족: 초당 %.0f페이지 적재 중 %.0f%%가 재적재%s — %s(%s%s, LRU 가정): data_buffer_size 증설",
                     c->miss_pps,c->readmit_pct,hit,vd_grade(c->readmit_pct),vd_prov(),ev); return; }
        if(c->miss_pps>=200 && c->readmit_pct>=0){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! %.0f pages/s admitted, re-read %.0f%%%s - reuse distance beyond the buffer (a scan, or a working set many times the buffer): %s (%s%s)"
                                :"⚠ 초당 %.0f페이지 적재, 재적재 %.0f%%%s — 재사용 거리가 버퍼 밖(스캔이거나 작업집합이 버퍼의 수 배): %s(%s%s)",
                     c->miss_pps,c->readmit_pct,hit,vd_grade(c->readmit_pct),vd_prov(),ev); return; }
        if(res<60.0 && c->miss_pps<20){ *col=VD_OK;
            snprintf(o,n,g_ascii?"over-provisioned: only %.0f%% of data_buffer ever used, %.0f pages/s - can shrink"
                                :"· 과다 할당: data_buffer 중 %.0f%%만 사용, 초당 %.0f페이지 — 축소 가능",
                     res,c->miss_pps); return; }
    }
    /* No device verdict without the DB's own disk: another disk's load is not the DB's */
    if(!c->dev[0]){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"DB volume device unknown - %s; no storage verdict"
                            :"· DB 볼륨 장치 미확인 — %s; 스토리지 판정 생략",c->dev_why[0]?c->dev_why:"-"); return; }
    /* With no ceiling observed, say so rather than pass an estimate off as fact */
    if(c->headroom_pct<0){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"%s: IOPS %.0f, await %.1fms, dev %.0f%% - ceiling not yet observed (needs a moment with queue >= 4)"
                            :"· %s: IOPS %.0f, 대기 %.1fms, 장치 %.0f%% — 상한 미관측(큐 4 이상 순간이 필요)",
                 c->dev,c->dev_iops,c->await_ms>=0?c->await_ms:0,c->dev_util); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"%s: IOPS %.0f (%.0f%% of ceiling), await %.1fms, hit %.0f%% (%s)"
                        :"· %s: IOPS %.0f (상한의 %.0f%%), 대기 %.1fms, 히트 %.0f%% (%s)",
             c->dev,c->dev_iops,c->headroom_pct,c->await_ms>=0?c->await_ms:0,
             c->hit_pct>=0?c->hit_pct:0,c->hit_src);
}
/* In a replay the recorded sentence is shown as is, when it is in the language in force */
static void vd_capacity_shown(char*o,size_t n,const char**col){
    vd_capacity(o,n,col);
    if(g_replaying && g_rp_capvd[0]){
        int ascii=1; for(const unsigned char*q=(const unsigned char*)g_rp_capvd;*q;q++) if(*q>=0x80){ ascii=0; break; }
        if(ascii==(g_ascii!=0)) snprintf(o,n,"%s",g_rp_capvd);
    }
}

/* ---- Four-panel definition, shared by the plots and the dashboard ---- */
static int g_plot_zoom=0;   /* 0 is the four-panel view; 1-4 give that panel the full height */
static int g_sb_per=1;      /* Rows per cause-metric series when zoomed */
static const pser_t *pl_load(void){   /* the label follows the language toggle */
    static pser_t p={HK_LOAD1,227,4,NULL}; p.lab=g_ascii?"load(1m)":"load(1분)"; return &p; }
/* Host overlay on panels 3 and 4.  Its scope differs from the main series (this
   instance), so it is drawn as a secondary overlay; a solid line on the same axis
   confuses "my value" with "the environment".  The overlay is normalized to its
   own maximum and the legend carries the real values. */
static const pser_t PL_DEVUTIL={HK_DEVUTIL,240,1,"장치%(호스트)"};
static const pser_t PL_AWAIT  ={HK_CAPAWAIT,240,4,"await ms(호스트)"};
static const pser_t PL_DEVUTIL_EN={HK_DEVUTIL,240,1,"device%(host)"};
static const pser_t PL_AWAIT_EN  ={HK_CAPAWAIT,240,4,"await ms(host)"};
/* Panel A, memory on a byte axis.  server uses PSS, as the dashboard's process and
   trend boxes do (check-hist keeps testing the db+lg+dyn+etc == RSS identity on the
   RSS axis). */
static int pl_series_A(pser_t*s2,int with_vsz){
    int n=0;
    s2[n].key=HK_SRVPSS; s2[n].col=211; s2[n].fmt=0; s2[n].lab="server(PSS)"; n++;
    s2[n].key=HK_DB;     s2[n].col=79;  s2[n].fmt=0; s2[n].lab="data_buffer"; n++;
    s2[n].key=HK_LG;     s2[n].col=178; s2[n].fmt=0; s2[n].lab="log_buffer"; n++;
    s2[n].key=HK_DYN;    s2[n].col=203; s2[n].fmt=0; s2[n].lab="dynamic-heap"; n++;
    if(with_vsz){ s2[n].key=HK_VSZ; s2[n].col=117; s2[n].fmt=0; s2[n].lab=g_ascii?"reserved(VmSize)":"할당(VmSize)"; n++; }
    return n;
}
/* Panel 1 is host-wide, panels 2-4 belong to the selected instance.  Mixing the
   two scopes on one screen invites reading one axis against the other.  Panel 1
   stays on top even when zoomed, so a magnified instance metric is still seen
   against the host situation. */
/* Series for the zoomed panel's bottom bars - the quantities that cause its main
   metric.  Returns the series count (0 draws no subchart). */
/* The permanent bottom bar, used in four-panel mode too: metrics too small to leave
   the floor of a line chart get their own bars (the volume slot of a price chart).
   Bars normalize to their own maximum, so small values keep their shape. */
static int pl_subbars_always(int idx,pser_t*b){
    int n=0;
    if(idx==3){
        b[n].key=HK_ABSORB;b[n].col=228; b[n].fmt=1; b[n].lab=g_ascii?"absorb":"흡수%"; n++;
    }
    return n;
}
static int pl_subbars(int idx,pser_t*b){
    int n=0;
    switch(idx){
    case 2:   /* Why memory grew: engine structures (B) plus page demand pressure */
        b[n].key=HK_HEAPB;  b[n].col=42;  b[n].fmt=0; b[n].lab=g_ascii?"heapB":"heapB"; n++;
        b[n].key=HK_MINFLT; b[n].col=178; b[n].fmt=3; b[n].lab=g_ascii?"minflt":"마이너"; n++;
        break;
    case 3:   /* Why reads grew: loads (misses), refills (working set over buffer), OS cache absorption */
        b[n].key=HK_CAPMISS;b[n].col=178; b[n].fmt=3; b[n].lab=g_ascii?"admit":"적재"; n++;
        /* Refill%% is the share of pages evicted and read back.  High load with refill means
           the buffer is short (more helps); high load without refill is a scan (more
           does not).  It is the one criterion separating the two verdicts, so it is
           shown as a bar where the reasoning can be checked. */
        b[n].key=HK_CAPREADMIT;b[n].col=203; b[n].fmt=1; b[n].lab=g_ascii?"readmit":"재적재%"; n++;
        b[n].key=HK_ABSORB; b[n].col=228; b[n].fmt=1; b[n].lab=g_ascii?"absorb":"흡수%"; n++;
        b[n].key=HK_MAJFLT; b[n].col=140; b[n].fmt=3; b[n].lab=g_ascii?"majflt":"메이저"; n++;
        break;
    case 4:   /* Why writes fall behind: the actual write volume and the threads blocked on disk */
        b[n].key=HK_WBPS;   b[n].col=117; b[n].fmt=2; b[n].lab=g_ascii?"write":"쓰기"; n++;
        b[n].key=HK_DTHR;   b[n].col=203; b[n].fmt=4; b[n].lab=g_ascii?"D thr":"D스레드"; n++;
        b[n].key=HK_BLKPCT; b[n].col=215; b[n].fmt=1; b[n].lab=g_ascii?"blkio":"blkio%"; n++;
        break;
    default: break;   /* Panel 1 (host) is not a zoom target */
    }
    return n;
}
static void pl_panel(int idx,int y,int W,int ch,int ncores,
                     const char*hdr,const char*hdr_r){
    /* Cause-metric bars go inside the panel, below the time axis and above the legend.
       Zoom shows more of them; four-panel mode shows only those invisible as lines
       (absorption%). */
    pser_t bars[6]; int nb;
    if(g_plot_zoom==idx) nb=pl_subbars(idx,bars);
    else                 nb=pl_subbars_always(idx,bars);
    int bper = (g_plot_zoom==idx) ? g_sb_per : 1;   /* Zoomed: several rows per series */
    char t1[200],t2[160],vb[320]; const char*vc=VD_INFO;
    /* Sized by CV_NS, so adding series to a panel cannot overflow it */
    pser_t s2[CV_NS]; int n=0;
    switch(idx){
    case 1: {   /* Host-wide: what state this machine is in */
        s2[n].key=HK_BUSYC;  s2[n].col=117; s2[n].fmt=4; s2[n].lab=g_ascii?"busy cores":"사용 코어"; n++;
        s2[n].key=HK_SATC;   s2[n].col=203; s2[n].fmt=4; s2[n].lab=g_ascii?"saturated":"포화 코어"; n++;
        s2[n].key=HK_OSUSED; s2[n].col=140; s2[n].fmt=0; s2[n].lab=g_ascii?"host RAM used":"호스트 RAM 사용"; n++;
        s2[n].key=HK_CUB;    s2[n].col=211; s2[n].fmt=0; s2[n].lab=g_ascii?"CUBRID total":"CUBRID 총합"; n++;
        s2[n].key=HK_DEVUTIL;s2[n].col=215; s2[n].fmt=1; s2[n].lab=g_ascii?"device util":"장치 사용률"; n++;
        s2[n].key=HK_CAPIOPS;s2[n].col=178; s2[n].fmt=3; s2[n].lab=g_ascii?"device IOPS":"장치 IOPS"; n++;
        vd_host(vb,sizeof vb,&vc,ncores);
        /* The header box is gone and its contents (db, window, samples, instance, time) are
           absorbed into this panel's title, returning two contentless border rows to
           the chart. */
        if(hdr&&hdr[0]) snprintf(t1,sizeof t1,"%s",hdr);
        else snprintf(t1,sizeof t1,"%s",g_ascii?"1 host resources · CPU cores · RAM · device · load overlay (all processes)"
                                               :"1 호스트 리소스 · CPU 코어 · RAM · 장치 · load 겹침 (전체 프로세스)");
        if(hdr_r&&hdr_r[0]) snprintf(t2,sizeof t2,"%s",hdr_r);
        else snprintf(t2,sizeof t2,"%s",g_ascii?"host-wide":"호스트 전체");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,1,NULL,pl_load(),vb,vc,1,NULL,0,1);
        break; }
    case 2: {   /* This instance's memory */
        n=pl_series_A(s2,1);
        /* Draw the B total alongside: a dynamic heap that grows while B stays flat means the
           part B cannot see (region A) is growing, which calls for r to remeasure A.
           B moving in steps is normal. */
        s2[n].key=HK_HEAPB; s2[n].col=42; s2[n].fmt=0; s2[n].lab=g_ascii?"heapB total":"heapB 합계"; n++;
        vd_mem(vb,sizeof vb,&vc);
        double d=HN>1?HS[HN-1].srv-HS[0].srv:0, w=vd_win(), rate=w>0?d/w*3600:0;
        snprintf(t2,sizeof t2,"%s%s/h · RAM %.1f%%",rate>=0?"+":"-",H(rate<0?-rate:rate),
                 HN>0&&HS[HN-1].ram>0?HS[HN-1].cub/HS[HN-1].ram*100:0);
        snprintf(t1,sizeof t1,"%s",g_ascii?"2 instance memory · why and where does it grow · reserved(blue) vs resident · load overlay"
                                          :"2 인스턴스 메모리 · 왜, 어디서 느나 · 할당(청) vs 상주 · load 겹침");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,0,NULL,pl_load(),vb,vc,1,bars,nb,bper);
        break; }
    case 3: {   /* This instance: volume to buffer */
        s2[n].key=HK_RBPS;  s2[n].col=203; s2[n].fmt=2; s2[n].lab=g_ascii?"disk read":"디스크 읽기"; n++;
        s2[n].key=HK_PGRES; s2[n].col=79;  s2[n].fmt=1; s2[n].lab=g_ascii?"buffer resident%":"버퍼 상주%"; n++;
        s2[n].key=HK_PGCOLD;s2[n].col=215; s2[n].fmt=1; s2[n].lab=g_ascii?"cold zone%":"cold 존%"; n++;
        s2[n].key=HK_CAPMISS;s2[n].col=178; s2[n].fmt=3; s2[n].lab=g_ascii?"admitted pg/s":"적재 pg/s"; n++;
        /* majflt and OS cache absorption stay out of the main series: at eight series the
           right-aligned legend fills and the left entries disappear into an ellipsis
           (reproduced even at 150 columns).  Both appear in the bars below, so nothing
           is lost. */
        vd_buf(vb,sizeof vb,&vc);
        snprintf(t2,sizeof t2,"%s",g_ascii?"instance · overlay=host":"이 인스턴스 · 겹침=호스트");
        snprintf(t1,sizeof t1,"%s",g_ascii?"3 volume -> buffer · is the buffer working (per-series normalized)"
                                          :"3 올리는 쪽(볼륨→버퍼) · 버퍼가 일하고 있나 (계열별 정규화)");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,1,NULL,
                  g_ascii?&PL_DEVUTIL_EN:&PL_DEVUTIL,vb,vc,1,bars,nb,bper);
        break; }
    default: {  /* This instance: buffer to volume */
        s2[n].key=HK_PGDIRTY;s2[n].col=211; s2[n].fmt=4; s2[n].lab=g_ascii?"dirty pages":"dirty 페이지"; n++;
        s2[n].key=HK_PGSPAN; s2[n].col=215; s2[n].fmt=4; s2[n].lab=g_ascii?"flush delay(log pg)":"flush 지연(로그pg)"; n++;
        s2[n].key=HK_WBPS;   s2[n].col=117; s2[n].fmt=2; s2[n].lab=g_ascii?"disk write":"디스크 쓰기"; n++;
        /* D threads are those in uninterruptible wait for the disk.  Undiluted by the core
           count, it is the one instance metric that always registers a wait. */
        s2[n].key=HK_DTHR;   s2[n].col=203; s2[n].fmt=4; s2[n].lab=g_ascii?"D threads":"D스레드"; n++;
        vd_flush(vb,sizeof vb,&vc);
        snprintf(t2,sizeof t2,"%s",g_ascii?"instance · overlay=host":"이 인스턴스 · 겹침=호스트");
        snprintf(t1,sizeof t1,"%s",g_ascii?"4 buffer -> volume · is write-back/flush falling behind (per-series normalized)"
                                          :"4 내리는 쪽(버퍼→볼륨) · 반영·flush 가 밀리나 (계열별 정규화)");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,1,NULL,
                  g_ascii?&PL_AWAIT_EN:&PL_AWAIT,vb,vc,1,bars,nb,bper);
        break; }
    }
}


/* ---------------- Dashboard render ---------------- */

static void ticks_bottom(int x0,int w,int yb){
    if(HN<2||w<12) return;
    int n=(w+2)/11;
    if(n>4)n=4;
    if(n<2)n=2;
    for(int i=0;i<n;i++){
        double f=(double)i/(n-1);
        const char*lbl=HS[(int)(f*(HN-1)+0.5)].hh;
        int L=(int)strlen(lbl);
        int cx=x0+(int)(f*(w-1)+0.5), lx=cx-L/2;
        if(lx<x0+1) lx=x0+1;
        if(lx>x0+w-L-1) lx=x0+w-L-1;
        cv_put(lx-1,yb,"┤","38;5;240"); cv_put(lx,yb,lbl,"38;5;250");
        cv_put(lx+L,yb,"├","38;5;240");
    }
}

/* What a retained I/O verdict needs to be worded again: the values it was drawn
   from.  The sentence is built when drawn, so after [l] past entries follow the
   new language too. */
typedef struct { double wa, devutil, blkpct, majflt, io; int d_thr, n_thr; } vdval_t;
static void vd_format(int kind,const vdval_t *v,char *vb,size_t n){
    if(kind==1){
        /* Cite only signals that actually show this instance waiting.  majflt stays 0 even
           under a bottleneck because CUBRID reads with pread rather than
           faulting on mmap, so quoting it would contradict the conclusion. */
        char ev[64]; size_t ep=0; ev[0]=0;
        if(v->d_thr>0)     ep+=snprintf(ev+ep,sizeof ev-ep,"%s%s %d/%d",
                                         ep?" · ":"",g_ascii?"D thr":"D스레드",
                                         v->d_thr,v->n_thr);
        if(v->blkpct>=1.0) ep+=snprintf(ev+ep,sizeof ev-ep,"%sblkio %.0f%%",
                                         ep?" · ":"",v->blkpct);
        if(v->majflt>=1.0) ep+=snprintf(ev+ep,sizeof ev-ep,"%smajflt %.0f/s",
                                         ep?" · ":"",v->majflt);
        if(!ep && v->io>0) snprintf(ev,sizeof ev,"%s %s/s",
                                            g_ascii?"io":"이 서버",H(v->io));
        snprintf(vb,n,"%s wa %.1f%% + %s %.0f%% %s data_buffer %s%s%s%s",
                 g_ascii?"[!]":"⚠",v->wa,g_ascii?"dev":"장치",v->devutil,
                 g_ascii?"-> disk bottleneck.":"→ 디스크 병목.",
                 g_ascii?"shortage?":"부족 의심",
                 ev[0]?" (":"",ev,ev[0]?")":"");
    } else if(kind==2){
        snprintf(vb,n,"%s %s %.0f%% %s wa %.1f%% %s (%s %d/%d · blkio %.0f%%)",
                 g_ascii?"[!]":"⚠",g_ascii?"dev":"장치",v->devutil,
                 g_ascii?"busy but":"는 바쁜데",v->wa,
                 g_ascii?"low - CPU busy hides wa":"로 낮다 — CPU 가 바빠 wa 가 가려진 상태",
                 g_ascii?"D thr":"D스레드",v->d_thr,v->n_thr,v->blkpct);
    } else if(kind==4){
        snprintf(vb,n,"%s %s %.0f%% %s (%s %d/%d · blkio %.0f%% · io %s/s) — %s",
                 g_ascii?"[i]":"·",g_ascii?"dev":"장치",v->devutil,
                 g_ascii?"busy but this instance is idle":"는 바쁘지만 이 인스턴스는 조용",
                 g_ascii?"D thr":"D스레드",v->d_thr,v->n_thr,v->blkpct,
                 H(v->io),
                 g_ascii?"load is from another process":"다른 프로세스의 부하다");
    } else {
        snprintf(vb,n,"%s wa %.1f%% %s",
                 g_ascii?"[i]":"·",v->wa,
                 g_ascii?"but device idle - non-disk wait (net/NFS) or short window"
                        :"인데 장치는 한가 — 디스크가 아닌 대기(네트워크·NFS)이거나 측정 구간이 짧다");
    }
}

/* ---- Decisions the dashboard draws, as functions so the regressions test this code ---- */
/* I/O verdict: 0 none, 1 certain bottleneck, 2 masked, 3 non-disk wait, 4 someone else's load.
   wa is diluted by the core count (1-2% on 64 cores with the disk saturated), so it is
   crossed with device utilization and this instance's own signals.  The wa threshold is
   half of one core waiting outright (100/ncpu), floored at 0.5. */
static int io_verdict(double wa,int ncpu,int has_dev,double devutil,
                      double majflt,int d_thr,double blkpct,double iobps){
    double wa_hi=(ncpu>0?100.0/ncpu:100.0)*0.5;
    if(wa_hi<0.5) wa_hi=0.5;
    int hi_wa=(wa>=wa_hi), hi_dev=(has_dev&&devutil>=80);
    /* Evidence this instance is the one using the disk.  rb+wb counts too: under an
       overlay filesystem the DB files' device is not found in diskstats. */
    int mine=(majflt>=1.0)||(d_thr>0)||(blkpct>=1.0)||(iobps>=1.0*1024*1024);
    if(hi_wa&&hi_dev&&mine)  return 1;
    if(!hi_wa&&hi_dev&&mine) return 2;
    if(hi_dev&&!mine)        return 4;
    if(hi_wa&&!hi_dev)       return 3;
    return 0;
}
/* Verdict retention: the two most recent kinds stay with their time.  A repeat of the
   same kind updates the latest entry; a new kind pushes the others down and the pushed
   entry is no longer live; no verdict clears live but keeps the record. */
#define VDKEEP 2
typedef struct { int kind; vdval_t v; char hm[8]; int live; } vdkeep_t;
static void vd_keep_step(vdkeep_t *k,int *n,int verdict,const vdval_t *v,const char *hm){
    if(!verdict){ if(*n>0) k[0].live=0; return; }
    if(*n>0 && k[0].kind==verdict){
        k[0].v=*v; snprintf(k[0].hm,sizeof k[0].hm,"%s",hm); k[0].live=1; return; }
    if(*n>0) k[0].live=0;
    for(int i=VDKEEP-1;i>0;i--) k[i]=k[i-1];
    k[0].kind=verdict; k[0].v=*v; snprintf(k[0].hm,sizeof k[0].hm,"%s",hm); k[0].live=1;
    if(*n<VDKEEP) (*n)++;
}
/* CPU-row cells: labels drop as values grow, rather than overflow the cell - a truncated
   number cannot be read, a label can be inferred from the column order. */
static void cpu_cell(char *dst,size_t n,const char *lab,double val,int cap){
    snprintf(dst,n,"%s %.0f%%",lab,val);
    if(disp_w(dst)>cap) snprintf(dst,n,"%.0f%%",val);
    if(disp_w(dst)>cap) snprintf(dst,n,"%.0f",val);
}
/* Saturated-core cell, decided on display width (a 6-cell label may be 8 bytes) */
static void sat_cell(char *dst,size_t n,int sat,int cap){
    snprintf(dst,n,g_ascii?"sat %d":"포화 %d",sat);
    if(disp_w(dst)>cap) snprintf(dst,n,"sat %d",sat);
    if(disp_w(dst)>cap) snprintf(dst,n,"%d",sat);
}

static int tui_too_small(const char *what,const char *keys_en,const char *keys_ko);
static void render_dash(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,int paused,
                        double work_ms){
    if(tui_too_small(g_ascii?"dashboard":"대시보드","[p] plots  [q] quit","[p] 플롯  [q] 종료")) return;
    int W=term_w();
    double mt=M->total*1024.0, av=M->avail*1024.0, used=mt-av;
    double swt=M->swtotal*1024.0, swu=(M->swtotal-M->swfree)*1024.0;
    cv_init(W,CV_H);
    char t1[64],t2[160];
    /* During a replay the clock must show when the frame was RECORDED, not now. */
    time_t now=g_replaying&&g_rep_ts?g_rep_ts:time(NULL);
    struct tm tv; rep_localtime(now,&tv);
    int h12=tv.tm_hour%12; if(!h12)h12=12;
    snprintf(t1,sizeof t1,"%04d-%02d-%02d %02d:%02d:%02d %s",tv.tm_year+1900,tv.tm_mon+1,tv.tm_mday,
             h12,tv.tm_min,tv.tm_sec,tv.tm_hour<12?"AM":"PM");
    int y=0;
    /* Compact layout: if the rows a full-width layout needs exceed the terminal height,
       the process and trend boxes are set side by side, saving 6 rows.
       Below 100 columns halves do not work (sparklines, legends), so no compaction. */
    int compact=0;
    /* Instance rows in the OS box: every instance when the screen has room, otherwise as many
       as fit with the last row summarising the rest.  A limit of the screen, not of the
       collection - the dump lists every instance. */
    int irows = g_ninst>0 ? g_ninst : 0;
    { int trows=term_h();
      int base = 1 + 4 + 7 + (NSR>0?NSR+3:4) + 11 + 6 + 8 + 9 + 2;
      if(trows>=8){ int room = trows - base + (W>=100?6:0); if(room<2) room=2;
                    if(irows>room) irows=room; }
      int need = base + irows;
      if(trows>=8 && trows<need && W>=100) compact=1; }
    /* ---- Top time line, outside the boxes ----
       Left: this frame's collection work time.  Right: the current time.
       Both apply to the whole screen, so neither hangs off a particular box title. */
    {
        char lt[64];
        /* A replay must never be mistaken for live: the left slot says REPLAY and the
           right shows the RECORDED time, not now. */
        if(g_replaying) snprintf(lt,sizeof lt,"%s %d/%d",
                                 g_ascii?"REPLAY":"재생", g_rep_i+1, g_rep_n);
        else            snprintf(lt,sizeof lt,"%s %.0fms",g_ascii?"collect":"수집",work_ms);
        cv_put(1,y,lt,g_replaying?"1;38;5;214":"38;5;244");
        if(g_rec_err[0]){ char rw[200]; snprintf(rw,sizeof rw,"%s %s",g_ascii?"! --record stopped:":"⚠ --record 중단:",g_rec_err);
                          cv_put(3+disp_w(lt),y,cutf(rw,W-6-disp_w(lt)-disp_w(t1)),"1;38;5;203"); }
        cv_put(W-1-disp_w(t1),y,t1,"38;5;250");
        y+=1;
    }
    /* ---- OS context ---- */
    /* The instance list lives in the OS box: what is running on this host and how much
       each uses is an OS-level fact.  The boxes below analyse the selected one. */
    int oh = 4 + irows;   /* +1 for the CPU row */
    /* load is host-wide and includes I/O waiting, so it goes in the title rather than a
       column.  The right of the title is taken by column headings, so it follows the
       left title. */
    { char ot[160], cg[64]="", ps[40]="";
      if(M->cg_limit>0) snprintf(cg,sizeof cg,g_ascii?" · cgroup %s/%s":" · cgroup 한도 %s/%s",
                                 H(M->cg_usage>=0?M->cg_usage:0),H(M->cg_limit));
      if(M->psi_full10>=0) snprintf(ps,sizeof ps," · PSI %.1f%%",M->psi_full10);
      snprintf(ot,sizeof ot,"%s · load %.1f %.1f %.1f%s%s",L("OS 컨텍스트"),
               M->load1,M->load5,M->load15,cg,ps);
      cv_box(0,y,W,oh,ot); }
    /* Same form as the region box: column headings on the border and values in columns.
       Cached, avail and swap are gathered on one row instead of flowing as sentences. */
    /* Six columns (43 cells) need 63 with the title, so narrow terminals drop some.
       cache and avail go first: they are OS-only and read '-' on instance rows. */
    int os_wide = (W >= 78);   /* Six columns, 45 cells, plus the title */
    { int VX=W-1-(os_wide?45:29);                      /* Start x of the value columns */
      char hd[80];
      /* The six columns:
           used   OS = used,          instance = PSS + PL
           cache  OS = Cached,        instance = its share of mapped file pages
           avail  OS = MemAvailable,  instance = n/a (not a process concept)
           swap   OS = swap in use,   instance = swapped out
           total  OS = MemTotal,      instance = n/a
           pct    used/total (instance is against RAM, rounded up) */
      /* total first, then used and pct: how much of how much, and what share */
      if(os_wide) snprintf(hd,sizeof hd,"┤%7s|%7s|%4s|%7s|%7s|%6s├",
                           "total","used","pct","avail","cache","swap");
      else        snprintf(hd,sizeof hd,"┤%7s|%7s|%4s|%6s├","total","used","pct","swap");
      cv_put(VX-1,y,hd,"1;36");

      char v[160];
      /* CPU before memory - it is the first thing looked at for resource use.
         CPU uses the same columns: total = logical cores, used = cores in use,
         pct = utilization, with us/sy/wa in the remaining three.  The label reads
         "physical c / logical t" so hyperthreading is visible. */
      { char cl[64]; int nc=M->cores>0?M->cores:1, pc=M->pcores>0?M->pcores:nc;
        char cn[24];
        if(pc!=nc) snprintf(cn,sizeof cn,"%dc/%dt",pc,nc);   /* Hyperthreading */
        else       snprintf(cn,sizeof cn,"%dc",nc);
        snprintf(cl,sizeof cl,"CPU %s",cn);
        cv_put(2,y+1,padf(cl,22,0),"250");
        /* A total gauge; saturation shows in the counts beside it (sat/high/max core) */
        cv_meter(25,y+1,VX-26,M->busy_cores/nc);
        { char c1[16],c2[16],c4[16],c5[16];
          snprintf(c1,sizeof c1,"%d",nc);
          snprintf(c2,sizeof c2,"%.1f",M->busy_cores);
          /* The avail cell carries the saturated core count, which matters more than us/sy:
             "12 cores' worth" does not say whether that is 12 cores or 6 at 100%, and
             the latter is the bottleneck.  hi (50-95%) is counted with it as near
             saturation. */
          /* Drop the us/sy labels as values grow, rather than overflow the cell: a truncated
             number cannot be read, while a label can be inferred from the column order.
             The last cell is 6 wide, so "sy 100%" (7 characters) would push the width. */
          cpu_cell(c4,sizeof c4,"us",M->cpu_us,7);
          cpu_cell(c5,sizeof c5,"sy",M->cpu_sy,6);
          /* These cells pad by display width rather than %7s, so CJK does not push the
             columns; the CPU row is therefore laid out cell by cell.  The last
             three carry their own labels, unrelated to the column headings above,
             so they are dimmed to keep the pairing from being misread. */
          { char c3k[16]; int cap = os_wide?7:6;
            sat_cell(c3k,sizeof c3k,M->sat_cores,cap);
            char pctb[8]; snprintf(pctb,sizeof pctb,"%3.0f%%",nc>0?M->busy_cores/nc*100:0);
            int cx=VX;
            cv_put(cx,y+1,padf(c1,7,1),"250");   cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
            cv_put(cx,y+1,padf(c2,7,1),"250");   cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
            cv_put(cx,y+1,padf(pctb,4,1),"250"); cx+=4; cv_put(cx,y+1,"|","250"); cx+=1;
            if(os_wide){
                cv_put(cx,y+1,padf(c3k,7,1),"38;5;244"); cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
                cv_put(cx,y+1,padf(c4,7,1), "38;5;244"); cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
                cv_put(cx,y+1,padf(c5,6,1), "38;5;244");
            } else  /* Narrow: only one cell remains, so keep saturation - it stands in for the
                       distribution when the bar is too narrow to read. */
                cv_put(cx,y+1,padf(c3k,6,1),"38;5;244");
          } } }
      cv_put(2,y+2,padf("Memory",10,0),"250");
      cv_meter(25,y+2,VX-26,mt>0?used/mt:0);
      if(os_wide)
          snprintf(v,sizeof v,"%7s|%7s|%3.0f%%|%7s|%7s|%6s",
                   H(mt),H(used),mt>0?used/mt*100:0,H(av),
                   H(M->cached*1024.0),M->swtotal==0?"off":H(swu));
      else
          snprintf(v,sizeof v,"%7s|%7s|%3.0f%%|%6s",H(mt),H(used),
                   mt>0?used/mt*100:0,M->swtotal==0?"off":H(swu));
      cv_put(VX,y+2,v,"250"); (void)swt;
    }
    /* ---- Per-instance usage, on the same scale and columns as total memory ---- */
    if(g_ninst>0){
        int VX=W-1-(os_wide?45:29);
        /* With more instances than rows, the last row summarises the rest; the instance in
           view always keeps its row.  The rows show exactly `shown` distinct instances
           either way (slots 0..shown-2, then slot shown-1 or the one in view), so the
           summary's count is always g_ninst-shown. */
        int shown = irows<g_ninst ? irows-1 : g_ninst;
        for(int slot=0;slot<shown;slot++){
            int i = (slot==shown-1 && g_cur>=shown && shown<g_ninst) ? g_cur : slot;
            int yy=y+3+slot, cur=(i==g_cur);   /* One more row ahead, for CPU */
            double iv=g_inst[i].rss+g_inst[i].pl_rss;       /* Instance total (RSS, as top), PL included */
            char nm[80],v[80];
            snprintf(nm,sizeof nm," %s ",g_inst[i].db);     /* Background colour padding on both sides */
            cv_put(2,yy,padf(cutf(nm,10),10,0), cur?"1;38;5;231;48;5;24":"38;5;250");
            /* PL is a breakdown of that instance's total, set beside the name to make the membership clear */
            if(g_inst[i].pl_n>0){
                char pl[24]; snprintf(pl,sizeof pl,"(PL %s)",HC(g_inst[i].pl_rss));
                cv_put(13,yy,padf(pl,11,0),"38;5;244");
            }
            cv_meter(25,yy,VX-26,mt>0?iv/mt:0);
            /* Instances have no avail or total.  pct rounds up: writing 0.6% as 0% reads as
               unused and makes the column width jump. */
            double pc = mt>0?iv/mt*100:0;
            int pci = (int)pc; if(pc>(double)pci) pci++;
            /* A '-' for an inapplicable column is not placed against the pipe: beside a capacity
               figure, '-|' reads as a value.  A space shows the cell is empty. */
            if(os_wide)
                snprintf(v,sizeof v,"%6s |%7s|%3d%%|%6s |%7s|%6s",
                         "-",H(iv),pci,"-",H(g_inst[i].filemap),
                         g_inst[i].swap>0?H(g_inst[i].swap):"0");
            else
                snprintf(v,sizeof v,"%6s |%7s|%3d%%|%6s","-",H(iv),pci,
                         g_inst[i].swap>0?H(g_inst[i].swap):"0");
            cv_put(VX,yy,v,"38;5;250");
        }
        if(shown<g_ninst){
            char more[96];
            snprintf(more,sizeof more,g_ascii?" ... %d more instances (< > to switch, -d lists all)"
                                             :" … 외 인스턴스 %d개 (< > 로 전환, 전체는 -d)",g_ninst-shown);
            cv_put(2,y+3+shown,cutf(more,W-4),"38;5;244");
        }
    }
    /* Parameter attribution describes the selected instance's region composition, not the
       whole OS (85% on one DB against 92% on another), so it moved to the region box
       title. */
    y+=oh;
    /* ---- Processes; in compact layout this draws into the left half (bw) ---- */
    int cbx=0, cbw=W;
    if(compact){ cbx=0; cbw=W/2; }
    {
        const int bx=cbx, bw=cbw;
        const int nmw = (bw>=100) ? 22 : 14;        /* Name column width, narrowed in a half-width layout */
        /* RSS per process tier, as top shows it (shared pages count in each process).  The
           server row is RSS too, so the row total matches the CUBRID total exactly. */
        double srv=P->rss_kb*1024.0;
        char n_srv[96];
        snprintf(n_srv,sizeof n_srv,"%sserver",T->master>0?"  ├ ":"▾ ");
        int nr=2+(T->npl>0?1:0)+(T->master>0?1:0)+(T->ncas>0?1:0), ph=nr+2;
        if(compact && ph<7) ph=7;                  /* Match the height of the trend mini beside it (7 rows) */
        /* Trend columns: the window delta plus a sparkline when width allows, so
           broker/CAS/PL growth is visible too. */
        /* Sparkline width: the gauge duplicates the value column to its right (PSS size), so
           it is halved and the space given to the sparkline.  A 3-minute window at 0.5s
           gives 24 cells, about 7 seconds each; 12 at 80 columns. */
        int spark = (bw>=100) ? 24 : (bw>=80) ? 12 : (bw>=54) ? 8 : 0;
        char wt2[24]; vd_wtxt(wt2,sizeof wt2);
        { char rt[64];
          double ptot=T->master+srv+T->pl+T->broker+T->cas;
          /* cv_box2's right-hand title shifts as a whole including its trailing space, so the
             text always ends at W-4.  The value column below moves from W-9 to W-11 to
             line up.  The unit stays M - promoting to 1.1G would stop the rows below
             from being added by eye. */
          if(HN>=4) snprintf(rt,sizeof rt,"%s%s │ All %8s",g_ascii?"d":"Δ",wt2,HM(ptot));
          else      snprintf(rt,sizeof rt,"All %8s",HM(ptot));
          cv_box2(bx,y,bw,ph,L("RSS · 프로세스"),rt); }
        double mx=srv;
        if(T->broker>mx)mx=T->broker;
        if(T->cas>mx)mx=T->cas;
        if(T->pl>mx)mx=T->pl;
        if(T->master>mx)mx=T->master;
        if(mx<=0)mx=1;
        /* The real startup hierarchy: master starts server and broker, and PL is a child
           of server (cub_pl's PPID is that cub_server).
             master
               |- server@db
               |    \- pl server (JVM)
               \- broker + CAS */
        char n_cas[48];
        snprintf(n_cas,sizeof n_cas,"      └ CAS[%d]",T->ncas);
        const char*nm[6]; double vv[6],cc[6]; int k=0; int hk[6];
        /* In a half-width layout (14-cell names) use short names that do not truncate mid-word */
        char n_cas2[24]; snprintf(n_cas2,sizeof n_cas2,"    └ CAS[%d]",T->ncas);
        int nar = (nmw<22);
        if(T->master>0){ nm[k]=nar?"▾ master":(g_ascii?"▾ master (shared)":"▾ master (설치본)");
                         cc[k]=T->cpu_master; hk[k]=1; vv[k++]=T->master; }
        nm[k]=n_srv; cc[k]=IO->cpu_pct/100.0; hk[k]=0; vv[k++]=srv;   /* Percent (one core = 100) to cores */
        if(T->npl>0){ nm[k]=nar?"  │ └ pl(JVM)":"  │   └ pl server(JVM)"; cc[k]=T->cpu_pl; hk[k]=2; vv[k++]=T->pl; }
        nm[k]=T->master>0?"  └ broker":"▾ broker"; cc[k]=T->cpu_broker; hk[k]=3; vv[k++]=T->broker;
        if(T->ncas>0){ nm[k]=nar?n_cas2:n_cas; cc[k]=T->cpu_cas; hk[k]=4; vv[k++]=T->cas; }
        /* Tier history accessor: hk index to hsam_t field */
        #define TIER_HS(smp,idx) ((idx)==0?(smp)->rss_srv:(idx)==1?(smp)->rss_master:(idx)==2?(smp)->rss_pl:(idx)==3?(smp)->rss_broker:(smp)->rss_cas)
        /* Same rule as panel A's verdict: growth above 5% across the window while load stays flat */
        int load_flat = !(HN>=4 && HS[HN-1].load1 > HS[0].load1*1.3 && HS[HN-1].load1>=1.0);
        /* The total goes at the right of the box title rather than in a row; see cv_box2 below */
        /* The gauge stays on the memory (PSS) scale and CPU appears only as the number before
           it: 0.4 cores on a 64-core scale would not fill one cell. */
        for(int i=0;i<k;i++){
            cv_put(bx+2,y+1+i,padf(cutf(nm[i],nmw),nmw,0),"250");
            /* Core counts truncate to one decimal.  Colour follows magnitude, as in htop's
               %CPU column, so which tier is running is visible without a gauge.
               Zero renders as a dot so idle rows do not draw the eye - five tiers
               all reading 0.0 would be noise. */
            { char cb[16]; double c=(double)((long)(cc[i]*10))/10.0;
              const char *col;
              if(c<=0)                       { snprintf(cb,sizeof cb,"·"); col="38;5;240"; }
              else { snprintf(cb,sizeof cb,"%.1f",c);
                     if(c>=M->pcores)        col="38;5;203";  /* Above the physical core count: red */
                     else if(c>=M->pcores/8.0) col="38;5;215"; /* Noticeable load: orange */
                     else if(c>=1.0)         col="38;5;228";  /* At least one core: yellow */
                     else                    col="38;5;117";  /* Small: cyan */ }
              cv_put(bx+nmw+3,y+1+i,padf(cb,4,1),col); }
            /* Columns from the right: value 8, delta 8, sparkline, then the gauge */
            int xval=bx+bw-12, xdel=xval-9, xsp=xdel-(spark?spark+1:0);
            int mx0=bx+nmw+8, mw=xsp-mx0-1;
            if(mw>=4) cv_meter(mx0,y+1+i,mw,vv[i]/mx);   /* Narrow: drop the gauge, the value column stands in */
            if(HN>=4){
                double d=TIER_HS(&HS[HN-1],hk[i])-TIER_HS(&HS[0],hk[i]);
                double base=TIER_HS(&HS[0],hk[i]);
                double ad=d<0?-d:d;
                char db[16]; const char*dc="38;5;244"; int dcol=244;
                /* Delta threshold: a dot only below 0.1% or 1MB; (1% of a 950MB server is 9.5MB) */
                if(base>0 && ad < base*0.001 && ad < 1.0*1024*1024){ snprintf(db,sizeof db,"·"); }
                else { snprintf(db,sizeof db,"%s%s",d>=0?"+":"-",H(ad));
                       if(base>0 && d>base*0.05){ dc = load_flat ? "1;38;5;203" : "38;5;215"; dcol = load_flat?203:215; }
                       else if(d<0){ dc="38;5;117"; dcol=117; }
                       else { dc="38;5;250"; dcol=250; } }
                cv_put(xdel,y+1+i,padf(db,8,1),dc);
                if(spark){
                    /* min-max scale: against 0, a value that only moves near its maximum (resident
                       memory) sits at the top and cannot distinguish "steady" from
                       "10% swing".  Stretching to the window's min-max shows the
                       shape, while the delta beside it gives the magnitude.
                       Under 0.5% amplitude draws a flat grey line. */
                    static double sv[HIST_MAX]; double smin=1e300,smax=-1e300;
                    for(int j=0;j<HN;j++){ sv[j]=TIER_HS(&HS[j],hk[i]); if(sv[j]>smax) smax=sv[j]; if(sv[j]<smin) smin=sv[j]; }
                    double span=smax-smin;
                    int flat = (smax<=0) || (span < smax*0.005);
                    const double*vp1[1]={sv}; int c1[1]={ flat ? 240 : dcol };
                    /* A flat line draws at 0.65 height (the second dot row); 0.5 lands on the same row as
                       the gauge track glyph and looked like a continuation of the gauge. */
                    if(flat){ for(int j=0;j<HN;j++) sv[j]=0.65; cv_chart_n(xsp,y+1+i,spark,1,vp1,c1,1,HN,1.0); }
                    else {   /* Map [min,max] onto [0.1,0.9] so the extremes do not sit on the cell boundary */
                        for(int j=0;j<HN;j++) sv[j]=0.1+0.8*(sv[j]-smin)/span;
                        cv_chart_n(xsp,y+1+i,spark,1,vp1,c1,1,HN,1.0); }
                }
            }
            /* Align right with the title's All value (the title text ends at W-4) */
            cv_put(xval,y+1+i,padf(HM(vv[i]),8,1),"250");
        }
        #undef TIER_HS
        y+=ph;
    }
    if(compact){
        /* Right half: the trend mini (7 rows, no verdict).  The process box just advanced y by ph, so step back to draw */
        int ty=y-7;
        pser_t s2[6]; int n=pl_series_A(s2,0);
        char vb[320]; const char*vc=VD_INFO; vd_mem(vb,sizeof vb,&vc);
        char wtxt[24]; vd_wtxt(wtxt,sizeof wtxt);
        double d=HN>1?HS[HN-1].srv-HS[0].srv:0, w=vd_win(), rate=w>0?d/w*3600:0;
        if(g_ascii) snprintf(t2,sizeof t2,"trend %s · [p]",wtxt); else snprintf(t2,sizeof t2,"메모리 추이 %s · [p]",wtxt);
        char rt[32]; snprintf(rt,sizeof rt,"%s%s/h",rate>=0?"+":"-",H(rate<0?-rate:rate));
        cv_panel2(W/2,ty,W-W/2,3,t2,rt,s2,n,0,0,NULL,pl_load(),NULL,NULL,1,NULL,0,1);
        /* The verdict takes one full-width row; in a half it would lose its conclusion */
        cv_put(2,y,cutf(vb,W-4),vc); y+=1;
    }
    /* ---- cub_server regions ---- */
    /* Four value columns: alloc | rss | conf | growth, each preceded by a space so it does
       not touch the pipe. */
    /* Value width 7: just below 1024, H() produces "1023.9M", seven characters; six would
       truncate it or push the column.  grow stays at 6, being mostly small deltas. */
    int VAL_X=W-35, PCT_X=W-42, MET_X=19, MET_W=PCT_X-MET_X-1;
    if(NSR==0){
        cv_box(0,y,W,4,L("RSS · 서버"));
        cv_put(2,y+1,"? cub_server 미기동 — 실측 불가","38;5;203");
        y+=4;
    } else {
        /* The left title is the box's identity (axis, attribution).  The view label is a switch
           that changes what the value columns MEAN, so it sits just before the value
           headings - the reason next to what it changes. */
        snprintf(t2,sizeof t2,"%s",L("RSS · 서버"));
        int title_end = 4+disp_w(t2)+2;   /* The x where the border, title and closing border end */
        cv_box(0,y,W,NSR+3,t2);   /* +1 for the total row */
        /* The share of this instance's regions explained by cubrid.conf parameters.
           The rest (dynamic heap, stacks, code) is decomposed by heap [B]/[A]. */
        { double byparam=0,tsum=0;
        for(int i=0;i<NSR;i++){ tsum+=SR[i].rss; if(SR[i].cfg>0) byparam+=SR[i].rss; }
          if(tsum>0){
              char at[48];
              double pct=byparam/tsum*100;
              /* Below 70%, more than three tenths is unexplained by configuration - a signal to look
                 at the heap box, so it takes the same orange as over-counting. */
              const char *ac = (pct<70.0) ? "1;38;5;214" : "38;5;244";
              snprintf(at,sizeof at,"┤ %s %.0f%% ├",
                       g_ascii?"by-param":"설정으로 설명", pct);
              /* Not drawn if it would reach the value headings at the right; overlapping makes both unreadable */
              int ax=2+disp_w(t2)+4;
              if(ax+disp_w(at) <= PCT_X-2) cv_put(ax,y,at,ac);
          }
          /* If no configuration could be read and the reason is known, show it where the
             attribution rate goes - nothing at all reads as "this build does not expose
             parameters".  (byparam==0 means no region has a cfg, i.e. no config read.) */
          if(byparam<=0 && g_pd_why[0]){
              char at[160]; snprintf(at,sizeof at,"┤ %s ├",g_pd_why);
              int ax=2+disp_w(t2)+4;
              if(ax+disp_w(at) <= PCT_X-2) cv_put(ax,y,at,"38;5;214");
          } }
        /* Column order alloc | rss | conf | grow; the gauge is rss over alloc and grow the rss delta */
        snprintf(t2,sizeof t2,"┤%7s |%7s |%7s |%6s├","alloc","rss","conf","grow");
        /* A heading past the right corner breaks the box, so it is pulled left by the overhang.
           If it would still collide with the title (at 60 columns), it is omitted; the
           values remain. */
        { int hx=VAL_X-1, hw=disp_w(t2);
          if(hx+hw > W-1) hx = W-1-hw;
          if(hx >= title_end) cv_put(hx,y,t2,"1;36"); }
          double sm=0,ssr=0,sc=0,sd=0; int base_ok=1;   /* Material for the total row */
        for(int i=0;i<NSR;i++){
            srg_t*r=&SR[i]; int yy=y+1+i;
            cv_put(2,yy,GR(r->grade),
                   r->grade=='M'?"38;5;39":r->grade=='R'?"38;5;214":r->grade=='E'?"38;5;250":"38;5;203");
            cv_put(4,yy,padf(r->name,14,0),"250");
            double frac=0,dfrac=0; char tail[24]; int warn=0;
            /* Gauge: resident over allocated (low = reserved and not used).  The growth is painted
               orange at the end of the gauge; a decrease already shows as a shorter bar. */
            double dv=r->rss-r->prev_rss; if(dv<0) dv=0;
            frac=r->mapped>0?r->rss/r->mapped:0;
            dfrac=r->mapped>0?dv/r->mapped:0;
            snprintf(tail,sizeof tail,"%.0f%%",frac*100);
            if(r->mapped>0||r->rss>0) cv_meter3(MET_X,yy,MET_W,frac,0,dfrac);
            cv_put(PCT_X,yy,padf(tail,6,1),warn?"38;5;203":"250");
            cv_put(VAL_X,   yy,padf(r->mapped>0?H(r->mapped):"-",7,1),"244");
            cv_put(VAL_X+8, yy,"|","38;5;240");
            /* Zero prints as "0B", as in the tree; a different zero per view invites confusion */
            cv_put(VAL_X+9, yy,padf(r->rss>0?H(r->rss):"0B",7,1),"250");
            cv_put(VAL_X+17,yy,"|","38;5;240");
            /* conf: the configured size, directly comparable with alloc and rss */
            cv_put(VAL_X+18,yy,padf(r->cfg>0?H((double)r->cfg):"-",7,1),"250");
            cv_put(VAL_X+26,yy,"|","38;5;240");
            /* growth is the rss delta against the previous frame.  No change prints a dot: 0B would
               read as "wrote zero bytes" and not be distinguishable from no change.
               One-shot mode has no baseline and prints '-'. */
            { char cg[24]; const char *cc="38;5;244";
              double base = r->prev_rss;
              double cur  = r->rss;
              double d    = cur-base;
              double a=d<0?-d:d;
              /* With no baseline (previous frame) the delta is undefined - print '-'.
                 Adding d==0 to the condition would let the first frame's full-value d
                 through and report the whole RSS as "+512.3M growth". */
              if(base<=0)             snprintf(cg,sizeof cg,"-");
              else if(a<1.0)          snprintf(cg,sizeof cg,"·");   /* Genuinely unchanged only */
              else { snprintf(cg,sizeof cg,"%s%s",d>0?"+":"-",H(a)); cc="1;38;5;214"; }
              cv_put(VAL_X+27,yy,padf(cg,6,1),cc); }
            sm+=r->mapped; ssr+=r->rss; if(r->cfg>0) sc+=(double)r->cfg;
            /* Baseline test: a region where both rss and prev are 0 (an arena) has no change - it
               is not missing a baseline.  Treating it as such would leave the total's
               grow at '-' forever. */
            if(r->prev_rss<=0){ if(r->rss>0) base_ok=0; }
            else sd+=r->rss-r->prev_rss;
            r->prev_rss=r->rss;
        }
        /* ---- Total row ----
           The rss total equals process RSS by definition (all smaps mappings, held by
           a check-hist regression).  It always differs from the process box's PSS by
           the shared-page apportionment (measured: almost entirely .so) - stating
           that here keeps the two boxes from being read as inconsistent. */
        { int yy=y+1+NSR;
          char lab[96]; double pss=P->pss_kb*1024.0, df=ssr-pss;
          if(pss>0)
              snprintf(lab,sizeof lab,g_ascii?"Σ sum · PSS %s (shared %s%s)"
                                             :"Σ 합계 · PSS %s (공유 .so 배분 %s%s)",
                       H(pss), df>=0?"-":"+", H(df<0?-df:df));
          else snprintf(lab,sizeof lab,g_ascii?"Σ sum":"Σ 합계");
          if(P->thp_kb>0 && P->rss_kb>0){ size_t l=strlen(lab);
              snprintf(lab+l,sizeof lab-l,g_ascii?" · THP %.0f%%":" · THP %.0f%%",100.0*P->thp_kb/P->rss_kb); }
          cv_put(2,yy,cutf(lab,PCT_X-3),"38;5;244");
          cv_put(VAL_X,   yy,padf(sm>0?H(sm):"-",7,1),"244");
          cv_put(VAL_X+8, yy,"|","38;5;240");
          cv_put(VAL_X+9, yy,padf(ssr>0?H(ssr):"0B",7,1),"1;38;5;250");
          cv_put(VAL_X+17,yy,"|","38;5;240");
          cv_put(VAL_X+18,yy,padf(sc>0?H(sc):"-",7,1),"250");
          cv_put(VAL_X+26,yy,"|","38;5;240");
          { char cg[24]; const char *cc="38;5;244";
            double dv = sd;
            int ok    = base_ok;
            double a=dv<0?-dv:dv;
            if(!ok)        snprintf(cg,sizeof cg,"-");
            else if(a<1.0) snprintf(cg,sizeof cg,"·");
            else { snprintf(cg,sizeof cg,"%s%s",dv>0?"+":"-",H(a));
                   cc = "1;38;5;214"; }
            cv_put(VAL_X+27,yy,padf(cg,6,1),cc); }
        }
        y+=NSR+3;
    }
    /* ── heap B ── */
    int hh=11, Lw=W;
    cv_box(0,y,Lw,hh,L("동적 메모리 상세 (B) — 자동 갱신"));
    if(!B.ok){ cv_put(2,y+2,L("방법 B 비활성"),"38;5;203"); cv_put(4,y+3,B.why,"38;5;240"); }
    else {
        double bs=0; int nb=B.n>7?7:B.n;
        for(int i=0;i<nb;i++){
            int yy=y+1+i;
            if(B.it[i].grade!='U') bs+=B.it[i].bytes;   /* Invalid items are excluded from the sum */
            cv_put(2,yy,GR(B.it[i].grade),B.it[i].grade=='M'?"38;5;39":"38;5;250");
            cv_put(4,yy,padf(B.it[i].name,22,0),"250");
            /* The delta goes left of the value: colour alone does not say how much changed.
               No change prints nothing, keeping the screen quiet. */
            { const char *dl=hdelta(B.it[i].name,B.it[i].bytes);
              if(dl[0]) cv_put(Lw-19,yy,padf(dl,9,1),"1;38;5;214"); }
            if(B.it[i].count_only){
                char cb[16]; snprintf(cb,sizeof cb,g_ascii?"%ldx":"%ld개",B.it[i].count);
                cv_put(Lw-9,yy,padf(cb,7,1),"38;5;244");   /* Size unknown: dimmed */
            } else
                cv_put(Lw-9,yy,padf(H(B.it[i].bytes),7,1),"250");
        }
        for(int i=2;i<Lw-2;i++) cv_put(i,y+hh-3,"─","38;5;240");
        if(B.n_bad>0){ char bt[80]; snprintf(bt,sizeof bt,"B 합계(유효 %d, 제외 %d)",B.n-B.n_bad,B.n_bad);
                       cv_put(2,y+hh-2,bt,"1;97"); }
        else if(B.partial) cv_put(2,y+hh-2,g_ascii?"B partial (count-only)":"B 부분(개수만 — 크기 미상)","1;38;5;214");
        else cv_put(2,y+hh-2,L("B 합계(정확)"),"1;97");
        cv_put(Lw-9,y+hh-2,padf(H(bs),7,1),"38;5;42");
    }
    y+=hh;
    /* ---- Buffer pool, BCB direct read ---- */
    {
        char pt[96];
        snprintf(pt,sizeof pt,g_ascii?"buffer pool · page slots — buffer control blocks (◌, %.0fms)":"버퍼풀 · 페이지 슬롯 = 버퍼 제어블록(BCB) (◌, %.0fms)",PG.ok?PG.ms:0.0);
        if(PG.ok && PG.from_last) snprintf(pt,sizeof pt,g_ascii?"buffer pool · page slots (◌, pass %.0fms, %.0fs ago · slot sample each frame)":"버퍼풀 · 페이지 슬롯 (◌, 전수 %.0fms, %.0f초 전 · 매 프레임 슬롯 표본)",PG.ms,PG.scan_age_s);
        if(!PG.ok && AG.ok) snprintf(pt,sizeof pt,g_ascii?"buffer pool · engine aggregates (●, %.2fms)":"버퍼풀 · 엔진 집계 (●, %.2fms)",AG.ms);
        cv_box(0,y,W,6,pt);
        if(!PG.ok && AG.ok){
            /* Not scanned: the engine's own counts - zones, victim candidates, dirty - without per-page detail */
            char ln[200]; long lru=AG.z1+AG.z2+AG.z3;
            double rf = AG.nbuf>0 ? (double)lru/AG.nbuf : 0, df = AG.nbuf>0 ? (double)AG.dirty/AG.nbuf : 0;
            int mw = W>90 ? 30 : 16;
            cv_put(2,y+1,g_ascii?"in LRU":"LRU 상주","250");
            cv_meter3(12,y+1,mw,rf,df,0);
            snprintf(ln,sizeof ln,"%ld/%d %.0f%%   dirty %lld (%.1f%%)",lru,AG.nbuf,rf*100,AG.dirty,df*100);
            cv_put(12+mw+2,y+1,ln,"250");
            snprintf(ln,sizeof ln,g_ascii?"zones  hot %ld  warm %ld  cold %ld   victim candidates %ld":"존  hot %ld  warm %ld  cold %ld   victim 후보 %ld",
                     AG.z1,AG.z2,AG.z3,AG.vict);
            cv_put(2,y+2,ln,"250");
            if(PG.have_log)
                snprintf(ln,sizeof ln,g_ascii?"log append %s  flushed %s  fsync delay %lld pg":"로그 append %s  flushed %s  fsync 지연 %lld pg",
                         lsa_str(PG.log_append),lsa_str(PG.log_nxio),(long long)(PG.log_append.pageid-PG.log_nxio.pageid));
            else snprintf(ln,sizeof ln,g_ascii?"(log LSA unavailable)":"(로그 LSA 미확보)");
            cv_put(2,y+3,cutf(ln,W-4),"250");
            cv_put(2,y+4,cutf(PG.why,W-4),"38;5;244");
        }
        else if(!PG.ok){ cv_put(2,y+1,g_ascii?"off":"비활성","38;5;203"); cv_put(2,y+2,cutf(PG.why,W-4),"38;5;240"); }
        else {
            char ln[200];
            double rf = PG.nbuf>0 ? (double)PG.resident/PG.nbuf : 0, df = PG.nbuf>0 ? (double)PG.dirty/PG.nbuf : 0;
            int mw = W>90 ? 30 : 16;
            cv_put(2,y+1,g_ascii?"resident":"상주","250");
            cv_meter3(12,y+1,mw,rf,df,0);           /* Foreground is the resident share, blue background the dirty share */
            snprintf(ln,sizeof ln,"%d/%d %.0f%%   dirty %d (%.1f%%)   flushing %d",PG.resident,PG.nbuf,rf*100,PG.dirty,df*100,PG.flushing);
            cv_put(12+mw+2,y+1,ln,"250");
            { int p2=snprintf(ln,sizeof ln,g_ascii?"zones  hot %d  warm %d  cold %d  void %d  free %d":"존  hot %d  warm %d  cold %d  void %d  free %d",
                              PG.z1,PG.z2,PG.z3,PG.zvoid,PG.zinv);
              if(AG.ok && p2>0 && p2<(int)sizeof ln)
                  snprintf(ln+p2,sizeof ln-(size_t)p2,g_ascii?"   victim candidates %ld":"   victim 후보 %ld",AG.vict); }
            cv_put(2,y+2,ln,"250");
            if(PG.have_log){
                snprintf(ln,sizeof ln,g_ascii?"log append %s  flushed %s  fsync delay %lld pg   oldest dirty %s  flush delay %lld pg"
                                             :"로그 append %s  flushed %s  fsync 지연 %lld pg   최고령 dirty %s  flush 지연 %lld pg",
                         lsa_str(PG.log_append),lsa_str(PG.log_nxio),(long long)(PG.log_append.pageid-PG.log_nxio.pageid),
                         PG.have_oldest?lsa_str(PG.oldest_dirty):"-",
                         PG.have_oldest?(long long)(PG.log_append.pageid-PG.oldest_dirty.pageid):0LL);
                if(PG.flush_only>0){ size_t l=strlen(ln);
                    snprintf(ln+l,sizeof ln-l,g_ascii?" (excl. %d mid-flush)":" (flush 중 %d 제외)",PG.flush_only); }
            } else snprintf(ln,sizeof ln,g_ascii?"(log LSA unavailable)":"(로그 LSA 미확보)");
            cv_put(2,y+3,cutf(ln,W-4),"250");
            { int pos=0; ln[0]=0;
              for(int rank=0;rank<4;rank++){ int best=-1;
                  for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]>0 && (best<0||PG.vol_res[v]>PG.vol_res[best])) best=v;
                  if(best<0) break;
                  pos+=snprintf(ln+pos,sizeof ln-(size_t)pos,"%svol%d %d/%dd",rank?"  ":"",best,PG.vol_res[best],PG.vol_dirty[best]);
                  PG.vol_res[best]=-PG.vol_res[best]; if(pos>=(int)sizeof ln-1) break; }
              for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]<0) PG.vol_res[v]=-PG.vol_res[v];
              char l2[220]; snprintf(l2,sizeof l2,g_ascii?"per volume (resident/dirty): %s":"볼륨별(상주/dirty): %s",ln);
              cv_put(2,y+4,cutf(l2,W-4),"38;5;244"); }
        }
        y+=6;
    }
    /* ---- Memory trend: a reduced form of the time-series panel A ----
       (same render function, same verdict).  In compact layout it was already drawn
       beside the process box above. */
    if(!compact){
        pser_t s2[6]; int n=pl_series_A(s2,0);
        char vb[320]; const char*vc=VD_INFO; vd_mem(vb,sizeof vb,&vc);
        char wtxt[24]; vd_wtxt(wtxt,sizeof wtxt);
        double d=HN>1?HS[HN-1].srv-HS[0].srv:0, w=vd_win(), rate=w>0?d/w*3600:0, mj=0;
        for(int i=0;i<HN;i++) if(HS[i].majflt>mj) mj=HS[i].majflt;
        if(g_ascii) snprintf(t2,sizeof t2,"memory trend · load(OS) · window %s (%d samples) · [p] full timeseries",wtxt,HN);
        else        snprintf(t2,sizeof t2,"메모리 추이 · load(OS) · 관측창 %s (샘플 %d개) · [p] 시계열 전체",wtxt,HN);
        char rt[64];
        if(mj>0) snprintf(rt,sizeof rt,"%s%s/h · mf%.0f/s",rate>=0?"+":"-",H(rate<0?-rate:rate),mj);
        else     snprintf(rt,sizeof rt,"%s%s/h",rate>=0?"+":"-",H(rate<0?-rate:rate));
        cv_panel2(0,y,W,3,t2,rt,s2,n,0,0,NULL,pl_load(),vb,vc,1,NULL,0,1);
        y+=8;
    }
    /* ---- I/O; absolute gauge floors of 8MB/s and 200 IOPS stop idle flicker ---- */
    {
        int yy;
        /* Read, write, IOPS and absorption come from /proc/<pid> and are per instance,
           while device utilization and iowait come from diskstats and /proc/stat and
           are host-wide; the row labels say which (only the D-thread count beside
           iowait is this server's).  Row count varies, so count before drawing. */
        int has_dev = IO->devnm[0]?1:0;
        /* The verdict (io_verdict) crosses wa, device utilization and this instance's own I/O
           signals.  Verdicts persist (vd_keep_step): I/O anomalies are brief, so the two most
           recent are kept with their time. */
        int verdict = io_verdict(M->cpu_wa,IO->ncpu,has_dev,IO->devutil,IO->majflt,IO->d_thr,
                                 IO->blkpct,IO->rb+IO->wb);
        static vdkeep_t g_vd[VDKEEP];
        static int g_vdn=0;
        /* Verdicts include retained ones, so their row count is counted in advance for the box
           height; a new kind arriving this frame can add one. */
        int vdrows = g_vdn;
        if(verdict && (g_vdn==0 || g_vd[0].kind!=verdict)) vdrows++;
        if(vdrows>VDKEEP) vdrows=VDKEEP;
        int has_vc = VC.total>0 ? 1 : 0;
        int nrow = 5 + has_dev + has_vc + vdrows;
        cv_box(0,y,W,nrow+2,L("I/O · cub_server + 최다사용 장치"));
        double pr=hist_peak(0,8.0*1024*1024), pw=hist_peak(1,8.0*1024*1024), pi=hist_peak(2,200);
        struct { const char*g,*nm; double frac; char prim[24],sec[28]; int warn; } R[9];
        int n=0;
        /* CPU utilization itself is not repeated here - the OS and process boxes show it
           twice already.  Only iowait stays: it is an I/O metric and belongs beside the
           disk rows where it can be compared quickly. */
        /* iowait leads the disk rows.  Its gauge is scaled to 10%, not 100%: wa is
           divided by the core count, so on 64 cores a saturated device still
           measured 0.8% and would not fill one cell.  The D-thread count at the
           right is direct evidence, undiluted. */
        { R[n].g="●"; R[n].nm=g_ascii?"iowait (host)":"iowait(OS)";
          R[n].frac=M->cpu_wa/10.0; if(R[n].frac>1) R[n].frac=1;
          R[n].warn=(M->cpu_wa>=3.0)||(IO->d_thr>0);
          snprintf(R[n].prim,24,"%.1f%%",M->cpu_wa);
          if(IO->n_thr>0) snprintf(R[n].sec,28,"%s %d/%d",g_ascii?"D thr":"D스레드",
                                   IO->d_thr,IO->n_thr);
          else            snprintf(R[n].sec,28,"%s",g_ascii?"D thr -":"D스레드 -");
          n++; }
        R[n].g="●"; R[n].nm=L("디스크 읽기"); R[n].frac=IO->rb/pr; R[n].warn=0;
        snprintf(R[n].prim,24,"%s/s",H(IO->rb)); snprintf(R[n].sec,28,"%s %s",L("누적"),H((double)P->read_b)); n++;
        R[n].g="●"; R[n].nm=L("디스크 쓰기"); R[n].frac=IO->wb/pw; R[n].warn=0;
        snprintf(R[n].prim,24,"%s/s",H(IO->wb)); snprintf(R[n].sec,28,"%s %s",L("누적"),H((double)P->write_b)); n++;
        R[n].g="●"; R[n].nm="IOPS"; R[n].frac=IO->ri/pi; R[n].warn=0;
        snprintf(R[n].prim,24,"%.0f/s",IO->ri); snprintf(R[n].sec,28,"%s %.0f/s",L("쓰기"),IO->wi); n++;
        if(IO->absorb_ok){ R[n].g="◌"; R[n].nm=L("캐시 흡수율"); R[n].frac=IO->absorb;
            R[n].warn=(IO->absorb<0.05&&IO->rb>0);
            snprintf(R[n].prim,24,"%.0f%%",IO->absorb*100); }
        else { R[n].g="◌"; R[n].nm=L("캐시 흡수율"); R[n].frac=0; R[n].warn=0;
            snprintf(R[n].prim,24,"%s",L("유휴")); }
        snprintf(R[n].sec,28,"rchar %s/s",H(IO->rchar)); n++;
        if(has_vc){ double pct=100.0*VC.resident/VC.total;   /* how much of the volumes the OS holds: a miss that is not here is a disk read */
            R[n].g="◌"; R[n].nm=L("볼륨 페이지캐시"); R[n].frac=pct/100.0; R[n].warn=0;
            snprintf(R[n].prim,24,"%.0f%%",pct);
            snprintf(R[n].sec,28,"%s/%s %.0fs",H(VC.resident),H(VC.total),VC.age_s); n++; }
        if(IO->devnm[0]){ R[n].g="●"; R[n].frac=IO->devutil/100.0; R[n].warn=IO->devutil>=90;
            static char dn[80]; snprintf(dn,sizeof dn,"%s %s%s",L("장치"),IO->devnm,
                                         g_ascii?"(host)":"(OS)"); R[n].nm=dn;
            snprintf(R[n].prim,24,"%.0f%%",IO->devutil);
            snprintf(R[n].sec,28,"%s %s/s",L("읽기"),H(IO->devr)); n++; }
        /* Unlike the region box's four columns the I/O box has two values, so reusing those
           positions leaves 15 cells at the right - the values go to the right edge and
           the gauge takes the rest. */
        { int SEC_X=W-2-16, PRI_X=SEC_X-8;
          for(int i=0;i<n;i++){
            yy=y+1+i;
            cv_put(2,yy,R[i].g,R[i].g[0]=='?'?"38;5;203":"38;5;39");
            cv_put(4,yy,padf(R[i].nm,14,0),"250");
            cv_meter(MET_X,yy,PRI_X-MET_X-1,R[i].frac);
            cv_put(PRI_X,yy,padf(R[i].prim,7,1),R[i].warn?"38;5;203":"250");
            cv_put(SEC_X,yy,padf(R[i].sec,16,1),"38;5;244");
          } }
        /* The verdict states in one line what the rows above imply together, so that wa
           is not read on its own: it is diluted by core count and hidden when the CPU
           saturates. */
        { vdval_t vv={ M->cpu_wa, IO->devutil, IO->blkpct, IO->majflt, IO->rb+IO->wb, IO->d_thr, IO->n_thr };
          char hm[8]; strftime(hm,sizeof hm,"%H:%M",&tv);
          vd_keep_step(g_vd,&g_vdn,verdict,&vv,hm); }
        /* Draw retained verdicts newest first: those still holding in their own colour, past
           ones dimmed to grey. */
        for(int k=0;k<g_vdn;k++){
            char vb[256], line[288];
            vd_format(g_vd[k].kind,&g_vd[k].v,vb,sizeof vb);
            snprintf(line,sizeof line,"%s %s",g_vd[k].hm,vb);
            const char *c;
            if(!g_vd[k].live)      c="38;5;240";           /* Past */
            else if(g_vd[k].kind==1) c="1;38;5;203";
            else if(g_vd[k].kind==2) c="38;5;215";
            else                     c="38;5;244";
            cv_put(2,y+1+n,cutf(line,W-4),c);
            n++;
        }
        y+=n+2;
    }
    CV.h=y;                                   /* Output only up to the height actually used */
    /* Re-assert cursor hiding every frame: a paramdump child inheriting the terminal, or
       a resize or mode change, can restore it, and it then blinks for one frame at the
       top left right after the move to home. */
    /* Atomic frames (DECSET 2026, synchronized output).  Even with ?25l some
       terminals track the cursor while drawing, which shows as a cursor artifact
       on wide cells.  Output between 2026h and 2026l lands at once on terminals
       that support it, and unsupported ones ignore the sequence. */
    printf("\033[?2026h\033[?25l\033[H");
    /* Fill the screen: the canvas draws only above the last two rows (overflow would clip
       the lower boxes) and those two always carry the key hints.  No newline is used,
       so there is no scrolling or flicker. */
    int trows=term_h(); if(trows<=0) trows=CV.h+2;   /* unknown (not a tty); too small was refused above */
    int clipped=cv_flush_rows(1,trows-2);
    printf("\033[%d;1H\033[J",(CV.h<trows-2?CV.h:trows-2)+1);   /* Clear leftovers below the canvas */
    /* The status line is outside the canvas and unbounded, so it is cut to avoid wrapping
       on a narrow terminal, with the PAUSED marker's width budgeted in advance. */
    /* The separator is grey so the key groups stand out; printing one string would not
       allow that, hence the piecewise output.
       With a single instance < and > do nothing, so they are only hinted when several
       exist. */
    {
      const char *seg[8]; int nseg=0;
      if(g_ascii){
          if(g_ninst>1) seg[nseg++]="[<>]inst";
          seg[nseg++]="[p]plot";
          seg[nseg++]="[l]lang"; seg[nseg++]="[h]help"; seg[nseg++]="[q]quit";
      } else {
          if(g_ninst>1) seg[nseg++]="[<>]인스턴스";
          seg[nseg++]="[p]시계열";
          seg[nseg++]="[l]한/영"; seg[nseg++]="[h]도움말"; seg[nseg++]="[q]종료";
      }
      char clip[24]=""; int clipw=0;
      /* Keep the clipped-row count short, or the help and quit segments lose their budget */
      if(clipped>0){ snprintf(clip,sizeof clip,g_ascii?"v%d":"\xe2\x96\xbc%d",clipped); clipw=disp_w(clip)+1; }
      int budget = W-2-(paused?9:0)-clipw;   /* One leading space plus one of slack */
      if(budget<0) budget=0;
      /* Bottom two-row overlay: one blank row and one key row, placed absolutely and ended
         without a newline: a newline on the last row would scroll the screen every frame. */
      printf("\033[%d;1H\033[K\033[%d;1H %s",trows-1,trows, paused?"\033[7m PAUSED \033[0m ":"");
      /* When width runs short, drop middle segments rather than cutting from the end: help
         and quit must survive at any width (the way out for a first-time user). */
      int keep[8]; for(int i=0;i<nseg;i++) keep[i]=1;
      for(;;){
          int tot=0,first=1;
          for(int i=0;i<nseg;i++) if(keep[i]){ tot+=(first?0:3)+disp_w(seg[i]); first=0; }
          if(tot<=budget) break;
          int drop=-1;
          for(int i=nseg-3;i>=1;i--) if(keep[i]){ drop=i; break; }   /* Middle, later ones first */
          if(drop<0){ for(int i=0;i<nseg-2;i++) if(keep[i]){ drop=i; break; } }
          if(drop<0) break;
          keep[drop]=0;
      }
      int used=0, first=1;
      for(int i=0;i<nseg;i++){
          if(!keep[i]) continue;
          int sw=disp_w(seg[i]);
          if(!first){ if(used+3+sw>budget) break; printf("\033[38;5;240m · \033[0m"); used+=3; }
          else if(sw>budget) break;
          first=0;
          printf("\033[38;5;250m%s\033[0m",seg[i]); used+=sw;
      }
      if(clip[0]) printf(" \033[38;5;214m%s\033[0m",clip);   /* Clipped row count, reported rather than hidden */
      printf("\033[K"); }
    printf("\033[?2026l");           /* End the atomic frame; everything appears at once here */
    fflush(stdout);
}

static void render_tree(proc_t *P,mem_t *M,tier_t *T,
                        double io_rbps,double io_wbps,double io_riops,double io_wiops,
                        double absorb,double blkpct,const char *devnm,double devutil,
                        double devr,const char *ts,double rchar_bps,double work_ms,
                        double cpu_pct,int ncpu){
    double mt=M->total*1024.0, av=M->avail*1024.0, used=mt-av;
    /* Process totals are RSS, as top shows them, on the same basis as the dashboard's process
       box; only the analysed server's own breakdown below goes into smaps detail. */
    double srv=P->rss_kb*1024.0+T->master;
    double brk=T->broker+T->cas;
    double cub=P->rss_kb*1024.0+T->master+T->broker+T->cas+T->pl;

    char age[48]="";
    if(P->smaps_age_s>g_interval*1.5)
        snprintf(age,sizeof age,g_ascii?"  regions %.0fs old":"  영역 %.0f초 전 값",P->smaps_age_s);
    printf(g_ascii?"CUBRID memory drilldown (C)  db=%s  pid=%d  ⏱ %s%s\n"
                  :"CUBRID 메모리 드릴다운 (C)  db=%s  pid=%d  ⏱ %s%s\n",P->db,P->pid,ts,age);
    if(g_ninst>1){
        /* States that only the db above is examined in detail; the rest give totals only. */
        printf(g_ascii?"  %d instances:":"  인스턴스 %d개:",g_ninst);
        for(int i=0;i<g_ninst;i++)
            printf(" %s%s %s%s", i==g_cur?"▶":"", g_inst[i].db,
                   H(g_inst[i].rss+g_inst[i].pl_rss), i<g_ninst-1?" ·":"");
        printf(g_ascii?"   (detail = %s — other DBs: 'cub_top <db>')\n"
                      :"   (상세는 %s — 다른 DB 는 'cub_top <db명>')\n",P->db);
    }
    if(P->pid>=0 && NSR>0){
        /* This tool's premise is labelling by cubrid.conf parameter name; without stating the
           share actually attributable to parameters, the premise reads as an
           overpromise. */
        double byparam=0, tot=0;
        for(int i=0;i<NSR;i++){ tot+=SR[i].rss; if(SR[i].cfg>0) byparam+=SR[i].rss; }
        if(tot>0)
            printf(g_ascii?"explained by config: %.0f%% (%s) / unexplained %.0f%%"
                          " — the latter is split by heap detail below (B=engine, A=estimate)\n"
                          :"설정으로 설명: cubrid.conf 파라미터 %.0f%% (%s) / 설명 안 되는 영역 %.0f%%"
                          " — 후자는 아래 동적 메모리 상세(B=정확, A=크기 추정)로 분해\n",
                   byparam/tot*100, H(byparam), (tot-byparam)/tot*100);
    }
    /* Grade marks appear from the first line while the full legend is at the foot (for
       attaching to a ticket), so a summary comes first and '?' is not misread as an
       error by someone reading downward. */
    printf(g_ascii?"marks  ● measured  ◐ reserved  ◌ estimated  ? unattributed (split by heap detail)\n"
                  :"기호  ● 측정  ◐ 예약  ◌ 추정  ? 미귀속(아래 동적 메모리 상세가 분해)\n");
    /* Always yes in a normal environment and thus uninformative; reported only when a fallback applies */
    if(!(CAP.vm_readv && CAP.rollup && CAP.proc_io))
        printf(g_ascii?"⚠ fallback active: %s   (see -h)\n"
               :"⚠ 폴백 동작 중: %s   (의미는 -h 참조)\n",CAP.note);
    if(P->reg_dropped)
        printf(g_ascii?"⚠ %d memory regions beyond the %d tracked were not classified - the breakdown is short of the total\n"
               :"⚠ 메모리 영역 %d개가 한도 %d개를 넘어 분류되지 않음 — 구성 합계가 총량보다 작게 나옴\n",P->reg_dropped,MAXREG);
    if(g_disk_dropped)
        printf(g_ascii?"⚠ %d block devices beyond %d were not read\n":"⚠ 블록 장치 %d개가 한도 %d개를 넘어 읽지 않음\n",g_disk_dropped,MAXDISK);
    printf("OS RAM  %s\n",H(mt));
    printf("├─ used  %s (%.0f%%)\n",H(used),used/(mt>0?mt:1)*100);
    printf("├─ cached  %s\n",H(M->cached*1024.0));
    printf("├─ avail  %s\n",H(av));
    if(M->swtotal==0) printf("├─ swap  off\n");
    else printf("├─ swap  %s / %s\n",H((M->swtotal-M->swfree)*1024.0),H(M->swtotal*1024.0));
    printf("├─ CUBRID engine  %s (%.2f%% of RAM)\n",H(cub),cub/(mt>0?mt:1)*100);
    if(P->pid<0){
        printf(g_ascii?"│  ├─ ? cub_server  [not running] — no measurement, showing cubrid.conf only\n"
               :"│  ├─ ? cub_server  [미기동] — 실측 불가, cubrid.conf 설정값만 표시\n");
        static const char *cf[]={"data_buffer_size","log_buffer_size","sort_buffer_size",
                                 "temp_file_memory_size_in_pages","max_subquery_cache_size"};
        for(unsigned i=0;i<sizeof cf/sizeof cf[0];i++){
            long long v=prm_get(cf[i]);
            if(v>0) printf(g_ascii?"│  │  ├─ ◐ %s (config)  %s\n":"│  │  ├─ ◐ %s (설정)  %s\n",cf[i],H((double)v));
        }
        printf(g_ascii?"│  └─ ● broker+CAS (separate 3-tier layer)  %s\n"
               :"│  └─ ● broker+CAS (3-tier 별도 계층)  %s\n",H(brk));
        if(T->npl>0) printf("│     └─ ◐ PL server(JVM)  %s\n",H(T->pl));
        printf("└─ I/O\n");
        if(devnm[0]) printf(g_ascii?"   ├─ ● dev %s (host-wide)  util %.0f%%  r %s/s\n"
                                   :"   ├─ ● dev %s (시스템 전체)  util %.0f%%  r %s/s\n",devnm,devutil,H(devr));
        printf(g_ascii?"   └─ ? cub_server I/O  not running — cannot attribute\n"
               :"   └─ ? cub_server I/O  미기동 — 프로세스 귀속 불가\n");
        return;
    }
    /* Collection time is the tool's own business; reported only when slow */
    if(work_ms>=1000.0)
        printf(g_ascii?"│  ├─ ● server+master  %s  (VmSize %s, worker %d, collect %.1fs)\n"
                      :"│  ├─ ● server+master  %s  (VmSize %s, worker %d, 수집 %.1fs)\n",
               H(srv),H(P->vsize_kb*1024.0),P->nthreads,work_ms/1000.0);
    else
        printf("│  ├─ ● server+master  %s  (VmSize %s, worker %d)\n",
               H(srv),H(P->vsize_kb*1024.0),P->nthreads);
    for(int i=0;i<NSR;i++){
        srg_t *r=&SR[i];
        int last=(i==NSR-1) && T->npl==0;
        printf("│  │  %s %s %-14s %s",last?"└─":"├─",GR(r->grade),r->name,
               r->rss>0?H(r->rss):"0B");
        if(r->mapped>0) printf(" / alloc %s",H(r->mapped));
        if(!strcmp(r->name,"glibc arena"))
            printf(g_ascii?"  (virtual reservation — 0 physical)":"  (가상 예약 — 물리 메모리 0)");
        if(!strcmp(r->name,"thread stacks")){
            long long ts=prm_get("thread_stacksize");
            if(ts>0) printf(g_ascii?"  (thread_stacksize %s is HA-path only — workers use OS default 8MB)"
                                   :"  (thread_stacksize %s 는 HA 경로 전용 — 워커는 OS 기본 8MB)",
                            H((double)ts));
        }
        if(r->cfg>0){
            double ov=r->mapped/(double)r->cfg*100;
            /* Exceeding the configured size is normal - the BCB and victim arrays and other
               management structures share the mapping.  The normal range is given so it
               is not read as the setting being ignored. */
            printf(g_ascii?"  (config %s, alloc %.0f%%%s)":"  (설정 %s, 할당 %.0f%%%s)",
                   H((double)r->cfg),ov,
                   (ov<=120.0)?(g_ascii?" ok":" 정상"):(g_ascii?" ⚠over":" ⚠초과"));
        }
        printf("\n");
        if(!strcmp(r->name,"dynamic-heap")){
            if(B.ok){
                for(int k=0;k<B.n;k++)
                    printf("│  │  │  ├─ %s [B] %-22s %s   (%s%s)\n",
                           GR(B.it[k].grade),B.it[k].name,
                           B.it[k].count_only?"-":H(B.it[k].bytes),B.it[k].note,
                           (B.it[k].bytes==0.0 && B.it[k].grade!='U' && !B.it[k].count_only)
                               ? (g_ascii?" — unused":" — 미사용") : "");
            } else printf(g_ascii?"│  │  │  ├─ ? [B] disabled — %s\n"
                                 :"│  │  │  ├─ ? [B] 비활성 — %s\n",B.why);
        }
    }
    if(T->npl>0) printf(g_ascii?"│  │  └─ ◐ PL server(JVM) — separate process started by server  %s\n"
                               :"│  │  └─ ◐ PL server(JVM) — 서버가 기동하는 별도 프로세스  %s\n",H(T->pl));
    printf(g_ascii?"│  └─ ● broker+CAS (separate 3-tier layer)  %s\n"
               :"│  └─ ● broker+CAS (3-tier 별도 계층)  %s\n",H(brk));
    if(P->pid>=0){
        /* ---- Capacity: demand, ceiling and headroom against the allocated resource ---- */
        printf(g_ascii?"├─ ◌ capacity (demand vs ceiling, %.0fms)\n":"├─ ◌ 용량 (수요 대비 상한, %.0fms)\n",CAP2.ms);
        if(CAP2.over_budget){
            printf(g_ascii?"│  └─ ? skipped: %s\n":"│  └─ ? 취합 생략: %s\n",CAP2.ps_why);
        } else {
            printf(g_ascii?"│  ├─ ● demand  dev %.0f IOPS · this server %.0f IOPS · read %s/s write %s/s\n"
                          :"│  ├─ ● 수요  장치 %.0f IOPS · 이 서버 %.0f IOPS · 읽기 %s/s 쓰기 %s/s\n",
                   CAP2.dev_iops,CAP2.riops+CAP2.wiops,H(CAP2.read_bps),H(CAP2.write_bps));
            if(CAP2.await_ms>=0)
                printf(g_ascii?"│  ├─ ● latency  await %.2fms (r %.2f / w %.2f) · queue %.2f · util %.0f%%\n"
                              :"│  ├─ ● 지연  요청당 %.2fms (읽기 %.2f / 쓰기 %.2f) · 큐 %.2f · 사용률 %.0f%%\n",
                       CAP2.await_ms,CAP2.r_await_ms<0?0:CAP2.r_await_ms,CAP2.w_await_ms<0?0:CAP2.w_await_ms,
                       CAP2.qdepth<0?0:CAP2.qdepth,CAP2.dev_util);
            /* This frame's I/O profile, stating which ceiling cell the comparison uses */
            printf(g_ascii?"│  ├─ ● profile  avg request %.1fKB · read %.0f%% → cell [%s / %s]\n"
                          :"│  ├─ ● 프로파일  평균 요청 %.1fKB · 읽기 %.0f%% → 상한 칸 [%s / %s]\n",
                   CAP2.req_kb,CAP2.read_ratio*100.0,CAP_BS_NAME[CAP2.prof_bs],CAP_RW_NAME[CAP2.prof_rw]);
            if(CAP2.ceil_valid){
                struct tm ct; char cw[32]="?";
                if(localtime_r(&CAP2.ceil_when,&ct)) strftime(cw,sizeof cw,"%m-%d %H:%M",&ct);
                printf(g_ascii?"│  ├─ ◌ ceiling  %.0f IOPS observed at util %.0f%% (%s) in this cell → now using %.0f%%\n"
                              :"│  ├─ ◌ 상한  %.0f IOPS (같은 칸, 사용률 %.0f%% 시점 관측, %s) → 현재 %.0f%% 사용\n",
                       CAP2.ceil_iops,CAP2.ceil_util,cw,CAP2.headroom_pct<0?0:CAP2.headroom_pct);
            } else
                printf(g_ascii?"│  ├─ ? ceiling  not observed for this cell (needs a saturated moment at this profile in this run)\n"
                              :"│  ├─ ? 상한  이 칸은 미관측 (이 실행 중 같은 프로파일에서 포화 순간 필요)\n");
            /* Ceilings remembered in other cells are shown too, making clear that the ceiling varies by profile */
            { int shown=0;
              for(int b=0;b<CAP_BS_N;b++) for(int r=0;r<CAP_RW_N;r++){
                  const cap_cell_t*cc=&g_cap_mtx[b][r];
                  if(!cc->valid || (b==CAP2.prof_bs && r==CAP2.prof_rw)) continue;
                  if(!shown++) printf(g_ascii?"│  ├─ ◌ other cells\n":"│  ├─ ◌ 다른 칸 상한\n");
                  printf(g_ascii?"│  │    [%s / %s] %.0f IOPS · await %.2fms · q %.2f\n"
                                :"│  │    [%s / %s] %.0f IOPS · 대기 %.2fms · 큐 %.2f\n",
                         CAP_BS_NAME[b],CAP_RW_NAME[r],cc->iops,cc->await_ms,cc->qd);
              } }
            if(CAP2.hit_pct>=0)
                printf(g_ascii?"│  ├─ ◌ buffer  hit %.1f%% (source: %s) · admitted %.0f pages/s · re-read %s\n"
                              :"│  ├─ ◌ 버퍼  히트 %.1f%% (출처: %s) · 적재 초당 %.0f페이지 · 재적재 %s\n",
                       CAP2.hit_pct,CAP2.hit_src,CAP2.miss_pps<0?0:CAP2.miss_pps,readmit_s(CAP2.readmit_pct));
            if(CAP2.ps_ok)
                printf(g_ascii?"│  ├─ ● engine  fetch %.0f/s ioread %.0f/s iowrite %.0f/s log-write %.0f/s commit %.0f/s\n"
                              :"│  ├─ ● 엔진  fetch %.0f/s ioread %.0f/s iowrite %.0f/s 로그쓰기 %.0f/s 커밋 %.0f/s\n",
                       CAP2.pb_fetch_ps,CAP2.pb_ioread_ps,CAP2.pb_iowrite_ps,CAP2.log_iowrite_ps,CAP2.commit_ps);
            else
                printf(g_ascii?"│  ├─ ◌ engine counters idle — %s\n":"│  ├─ ◌ 엔진 카운터 정지 — %s\n",CAP2.ps_why);
            { char vb[320]; const char*vc=VD_INFO; vd_capacity_shown(vb,sizeof vb,&vc);
              printf(g_ascii?"│  └─ verdict  %s\n":"│  └─ 판정  %s\n",vb); }
        }
        /* Buffer pool: which pages are resident and how much has not reached disk (dirty).
           volmap --bufmap overlays the same snapshot on the volume map (--bcb-dump). */
        if(PG.ok){
            double rp = PG.nbuf>0 ? 100.0*PG.resident/PG.nbuf : 0, dp = PG.resident>0 ? 100.0*PG.dirty/PG.resident : 0;
            printf(g_ascii?"├─ ◌ page buffer (page slots/BCB read directly, %.0fms)  resident %d/%d (%.0f%%)  dirty %d (%.1f%% of resident)  flushing %d  → not yet written %d\n"
                          :"├─ ◌ 버퍼풀 (페이지 슬롯 %.0fms)  상주 %d/%d (%.0f%%)  dirty %d (상주의 %.1f%%)  flushing %d  → 아직 안 쓴 페이지 %d\n",
                   PG.ms,PG.resident,PG.nbuf,rp,PG.dirty,dp,PG.flushing,PG.unflushed);
            { int dw=dwb_pages();
              if(dw>0) printf(g_ascii?"│  ├─ DWB on: a page counts as written once copied into the DWB - up to %d \"clean\" pages may still await their volume write\n"
                                     :"│  ├─ DWB 사용: DWB 에 복사된 페이지는 flush 완료로 처리 — \"clean\" 중 최대 %d 페이지는 아직 볼륨 쓰기 대기일 수 있음\n",dw); }
            printf(g_ascii?"│  ├─ zones  hot %d · warm %d · cold %d · void %d · free %d   (victims are taken from cold; dirty ones are skipped)\n"
                          :"│  ├─ 존  hot %d · warm %d · cold %d · void %d · free %d   (victim 은 cold 에서, dirty 는 건너뜀)\n",
                   PG.z1,PG.z2,PG.z3,PG.zvoid,PG.zinv);
            { int shown=0;
              for(int rank=0;rank<6;rank++){ int best=-1;
                  for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]>0 && (best<0||PG.vol_res[v]>PG.vol_res[best])) best=v;
                  if(best<0) break;
                  printf(g_ascii?"│  ├─ vol %-3d resident %d (%s)  dirty %d\n":"│  ├─ vol %-3d 상주 %d (%s)  dirty %d\n",
                         best,PG.vol_res[best],H((double)PG.vol_res[best]*g_pagesize),PG.vol_dirty[best]);
                  PG.vol_res[best]=-PG.vol_res[best];   /* Marked as shown (negative), restored below */
                  shown++; }
              for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]<0) PG.vol_res[v]=-PG.vol_res[v];
              (void)shown; }
            if(PG.have_log){
                printf(g_ascii?"│  ├─ log  append %s  flushed(nxio) %s  → %lld log pages not yet on disk\n"
                              :"│  ├─ 로그  append %s  flushed(nxio) %s  → 아직 디스크에 안 내려간 로그 %lld 페이지\n",
                       lsa_str(PG.log_append),lsa_str(PG.log_nxio),(long long)(PG.log_append.pageid-PG.log_nxio.pageid));
                char ex[160]="";
                if(PG.flush_only>0) snprintf(ex,sizeof ex,g_ascii?"  (excludes %d page(s) mid-flush: the engine clears their LSA as the flush begins)"
                                                                 :"  (flush 중 %d 페이지 제외 — 엔진이 flush 시작 때 LSA 를 비움)",PG.flush_only);
                if(PG.have_oldest)
                    printf(g_ascii?"│  └─ oldest dirty page LSA %s  → flush delay %lld log pg behind append (recovery redo length)%s\n"
                                  :"│  └─ 가장 오래된 dirty 페이지 LSA %s  → append 보다 %lld 로그 페이지 뒤 = flush 지연(리커버리 redo 길이)%s\n",
                           lsa_str(PG.oldest_dirty),(long long)(PG.log_append.pageid-PG.oldest_dirty.pageid),ex);
                else if(PG.unflushed>0)
                    printf(g_ascii?"│  └─ no dirty page, %d being flushed%s\n":"│  └─ dirty 없음, flush 중 %d%s\n",PG.flushing,ex);
                else if(dwb_pages()>0)
                    printf(g_ascii?"│  └─ no dirty page — all written by the buffer (the last ones may still be in the DWB)\n"
                                  :"│  └─ dirty 없음 — 버퍼는 전부 내보냄 (마지막 몇 페이지는 아직 DWB 에 있을 수 있음)\n");
                else printf(g_ascii?"│  └─ no dirty page — buffer fully reflected to volumes\n":"│  └─ dirty 없음 — 버퍼 내용이 볼륨에 전부 반영됨\n");
            } else printf(g_ascii?"│  └─ (log LSA unavailable)\n":"│  └─ (로그 LSA 미확보)\n");
        } else if(AG.ok){
            long lru=AG.z1+AG.z2+AG.z3;
            printf(g_ascii?"├─ ● page buffer (engine aggregates, %.2fms)  in LRU %ld/%d (%.0f%%)  dirty %lld\n"
                          :"├─ ● 버퍼풀 (엔진 집계 %.2fms)  LRU 상주 %ld/%d (%.0f%%)  dirty %lld\n",
                   AG.ms,lru,AG.nbuf,AG.nbuf>0?100.0*lru/AG.nbuf:0,AG.dirty);
            printf(g_ascii?"│  ├─ zones  hot %ld · warm %ld · cold %ld   victim candidates %ld\n"
                          :"│  ├─ 존  hot %ld · warm %ld · cold %ld   victim 후보 %ld\n",AG.z1,AG.z2,AG.z3,AG.vict);
            if(PG.have_log)
                printf(g_ascii?"│  ├─ log  append %s  flushed(nxio) %s  → %lld log pages not yet on disk\n"
                              :"│  ├─ 로그  append %s  flushed(nxio) %s  → 아직 디스크에 안 내려간 로그 %lld 페이지\n",
                       lsa_str(PG.log_append),lsa_str(PG.log_nxio),(long long)(PG.log_append.pageid-PG.log_nxio.pageid));
            printf("│  └─ %s\n",PG.why);
        } else printf(g_ascii?"├─ ? page buffer (BCB)  off — %s\n":"├─ ? 버퍼풀 (BCB)  비활성 — %s\n",PG.why);
    }
    if(CAP.proc_io){
        printf("└─ CPU · I/O (cub_server + device)\n");
        /* CPU counts one core as 100%, so a multicore process can exceed it */
        printf(g_ascii?"   ├─ ● cpu  %.0f%%  (1 core = 100%%, host %d cores)\n"
               :"   ├─ ● cpu  %.0f%%  (코어 1개=100%%, 호스트 %d코어)\n",cpu_pct,ncpu);
        printf(g_ascii?"   ├─ ● disk read  %s/s  (total %s)\n":"   ├─ ● disk read  %s/s  (누적 %s)\n",H(io_rbps),H((double)P->read_b));
        printf(g_ascii?"   ├─ ● disk write  %s/s  (total %s)\n":"   ├─ ● disk write  %s/s  (누적 %s)\n",H(io_wbps),H((double)P->write_b));
        printf("   ├─ ● iops  read %.0f/s  write %.0f/s\n",io_riops,io_wiops);
        /* Below the rchar floor (64KB/s) the ratio is meaningless, and 100% must not be read
           as a perfect cache (absorb 100% at rchar 95B/s). */
        if(absorb>=0 && rchar_bps>=RCHAR_MIN) printf("   ├─ ◌ cache absorb  %.0f%%\n",absorb*100);
        else printf(g_ascii?"   ├─ ◌ cache absorb  idle (rchar %s/s — too little requested reading to judge)\n"
                          :"   ├─ ◌ cache absorb  idle (rchar %s/s — 판단하기엔 요청 읽기가 너무 적음)\n",H(rchar_bps));
        printf(g_ascii?"   ├─ ● blkio wait  %.2f%%  (sum of all threads — can exceed 100%%;"
                      " async waits not captured)\n"
                      :"   ├─ ● blkio wait  %.2f%%  (전 스레드 합 — 100%%를 넘을 수 있음;"
                      " 비동기 대기는 안 잡힘)\n", blkpct);
        if(devnm[0]) printf("   └─ ● dev %s  util %.0f%% %s  r %s/s\n",devnm,devutil,
                            (devutil>=90.0)?(g_ascii?"⚠ saturated":"⚠ 포화")
                                           :(g_ascii?"(ok <90%)":"(정상 <90%)"),H(devr));
        else         printf(g_ascii?"   └─ (no device stats)\n":"   └─ (장치 통계 없음)\n");
    } else printf(g_ascii?"└─ I/O  (no /proc/pid/io — kernel IO accounting off)\n"
                  :"└─ I/O  (/proc/pid/io 없음 — 커널 IO 회계 비활성)\n");
    if(P->pid>=0){
        int shown=0;
        for(int i=0;i<NPRMROWS;i++){
            long long v=prm_get(PRMROWS[i].name);
            if(v<0) continue;
            if(!shown){
                printf(g_ascii?"\nmemory parameters (cubrid.conf)   ● usage observed   ◐ config only"
                              " — usage folds into dynamic-heap\n"
                              :"\n메모리 파라미터 (cubrid.conf)   ● 사용량까지 관측   ◐ 설정만"
                              " — 사용량은 dynamic-heap 에 합산됨\n");
                shown=1;
            }
            char vs[32];
            switch(PRMROWS[i].unit){
            case 'B': snprintf(vs,sizeof vs,"%s",H((double)v)); break;
            case 'P': snprintf(vs,sizeof vs,"%lld p",v); break;
            default:  snprintf(vs,sizeof vs,"%lld",v); break;
            }
            printf("  %s %-32s %10s   %s\n",
                   PRMROWS[i].obs?"●":"◐", PRMROWS[i].name, vs,
                   g_ascii?PRMROWS[i].desc_en:PRMROWS[i].desc);
        }
    }
    /* A legend is kept so the output alone can be pasted into a ticket and still read (grade marks appear on every row) */
    printf(g_ascii?"\nmarks  ● measured (OS/engine reported)  ◐ reserved (mapped only)  ◌ estimated  ? unattributed\n"
                  :"\n등급  ● 측정(OS·엔진 직접 보고)  ◐ 예약(매핑만 확인)  ◌ 추정(패턴)  ? 미귀속\n");
}

/* Name to a key-safe slug (lower-case alphanumeric and '_').  Repeated keys
   (heapB.item, bytes) leave only the last in a dict parser, defeating the
   monitoring integration this exists for. */
static const char *slugify(const char *s){
    static char out[64];
    size_t n=0; int us=1;
    for(; *s && n+1<sizeof out; s++){
        unsigned char c=(unsigned char)*s;
        if((c>='a'&&c<='z')||(c>='0'&&c<='9')){ out[n++]=(char)c; us=0; }
        else if(c>='A'&&c<='Z'){ out[n++]=(char)(c-'A'+'a'); us=0; }
        else if(!us){ out[n++]='_'; us=1; }
    }
    while(n>0 && out[n-1]=='_') n--;
    out[n]=0;
    return out;
}

#define PLOT_MIN_COLS 60
#define TUI_MIN_ROWS  10
/* A terminal too narrow or too short to draw in: drawing anyway wraps or scrolls every frame,
   so say why and what to do.  Unknown sizes (not a tty) draw at the defaults. */
static int tui_too_small(const char *what,const char *keys_en,const char *keys_ko){
    int c=term_cols_raw(), r=term_h();
    if(!((c>0 && c<PLOT_MIN_COLS) || (r>0 && r<TUI_MIN_ROWS))) return 0;
    printf("\033[H\033[2J");
    printf(g_ascii?"%s needs at least %d x %d.\n":"%s 은(는) 최소 %d칸 x %d줄이 필요합니다.\n",what,PLOT_MIN_COLS,TUI_MIN_ROWS);
    printf(g_ascii?"  now %d x %d - enlarge the terminal.\n":"  현재 %d칸 x %d줄 — 터미널을 키우세요.\n",c,r);
    printf("  %s\n",g_ascii?keys_en:keys_ko);
    fflush(stdout);
    return 1;
}
static void render_plot_in(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,int paused);
/* A long view (key w) draws the minute ring in place of the live ring for this screen only:
   the plots and verdicts read HS/HN and nothing else */
static void render_plot(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,int paused){
    hsam_t *lp; int ln;
    if(hm_view(g_plot_win,&lp,&ln)){
        hsam_t *shs=HS; int shn=HN, scap=HCAP;
        HS=lp; HN=ln; HCAP=ln;
        render_plot_in(P,M,T,IO,paused);
        HS=shs; HN=shn; HCAP=scap;
    } else render_plot_in(P,M,T,IO,paused);
}
static void render_plot_in(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,int paused){
    (void)IO; (void)T;
    int raw=term_cols_raw();
    if(tui_too_small(g_ascii?"plot view":"플롯 화면","[p] dashboard  [q] quit","[p] 대시보드  [q] 종료")) return;
    int W=term_w(); if(W>CV_W)W=CV_W;
    if(raw>0 && W>raw) W=raw;
    char t1[160],t2[160];
    cv_init(W,CV_H);
    int y=0;
    /* Four panels (A memory, B fill, C CPU, D drain).  Each = title 1 + chart ch +
       legend 1 + verdict 1 + border 1 = ch+4 rows.  On a short screen ch shrinks (2-8)
       rather than dropping a panel.  Keys 1-4 zoom one panel to full height. */
    int rows=term_h(); if(rows<10) rows=24;
    if(rows>CV_H) rows=CV_H;
    int budget = rows - 1 - (P->pid<0?1:0);   /* No header box: one key row at the foot, with the rulers inside the panels */
    /* Zoom keeps panel 1 (host) on top and gives the rest to the zoomed panel: without
       the host situation, a magnified instance metric cannot be attributed.
       Panel 1 is not itself a zoom target (key 1 restores the four-panel view). */
    int host_ch = g_plot_zoom ? 4 : 0;   /* When zoomed, panel 1 (host) stays on top at a reduced height */
    /* Cause-metric bars go inside the panel box, below the time axis and above the legend.
       Here only their row count is needed, to yield that much chart height. */
    pser_t sb[6]; int nsb = g_plot_zoom ? pl_subbars(g_plot_zoom,sb) : 0;
    /* Zoom gives several rows per series: the cause metrics must grow with the main chart
       to keep the proportion.  About a third of the budget goes to them (2-5 rows each). */
    int sb_per = 1;
    if(nsb){
        /* The main chart can take 40 rows, but braille gives four steps per cell so 16 rows
           are already dense (cv_chart_n clips above 16).  The remaining height goes to
           the cause metrics; otherwise the main chart is left with empty space. */
        int main_cap=16;
        int avail = budget-10-4-main_cap;           /* Excluding the host summary, borders and the main chart's share */
        sb_per = (avail>0) ? avail/nsb : 2;
        /* Braille bars give four vertical steps per cell against a block's eight, so they need
           fewer rows - one less per series, returned to the main chart and margins. */
        sb_per -= 1;
        if(sb_per<1) sb_per=1;
        if(sb_per>5) sb_per=5;
    }
    int sb_rows = nsb ? nsb*sb_per : 0;
    g_sb_per = sb_per;
    /* Four-panel mode keeps a permanent bar (absorption%) below panel 3: as a line
       those values are too small and sit on the floor.  Each panel's chart shrinks to
       make room. */
    pser_t ab[4]; int nab = g_plot_zoom ? 0 : pl_subbars_always(3,ab);
    int ab_rows = nab ? nab : 0;
    int ch = g_plot_zoom ? budget-5-(host_ch+5)-sb_rows : (budget-ab_rows)/4-5;
    if(ch>(g_plot_zoom?16:8)) ch=(g_plot_zoom?16:8);   /* cv_chart_n saturates at 16 rows */
    if(ch<2) ch=2;

    /* The header always shows the window and the sample count, so a short line reads as
       "still collecting" rather than a bug. */
    double win = HN>1 ? HS[HN-1].t-HS[0].t : 0;
    char wtxt[24];
    if(win>=60) snprintf(wtxt,sizeof wtxt,"%dm%02ds",(int)(win/60),(int)win%60);
    else        snprintf(wtxt,sizeof wtxt,"%.0fs",win);
    /* These two strings become panel 1's title and right-hand badge; there is no separate
       header box, so its rows go to the chart. */
    if(g_plot_win && HS!=HS_live){
        if(g_ascii) snprintf(t1,sizeof t1,"1 host · db=%.32s · window %s (%d minute means) [w]",P->db[0]?P->db:"-",wtxt,HN);
        else        snprintf(t1,sizeof t1,"1 호스트 · db=%.32s · 관측창 %s (1분 평균 %d개) [w]",P->db[0]?P->db:"-",wtxt,HN); }
    else if(g_ascii) snprintf(t1,sizeof t1,"1 host · db=%.32s · window %s (%d samples, %.1fs) [w]",
                         P->db[0]?P->db:"-",wtxt,HN,g_interval);
    else        snprintf(t1,sizeof t1,"1 호스트 · db=%.32s · 관측창 %s (샘플 %d개, 간격 %.1f초) [w]",
                         P->db[0]?P->db:"-",wtxt,HN,g_interval);   /* The pid is on the dashboard */
    if(g_ninst>1) snprintf(t2,sizeof t2,"%d/%d %s · %s  %s",g_cur+1,g_ninst,
                           g_ascii?"inst":"인스턴스",g_ascii?"host-wide":"호스트 전체",
                           HN>0?HS[HN-1].hh:"--:--:--");
    else          snprintf(t2,sizeof t2,"%s  %s",g_ascii?"host-wide":"호스트 전체",
                           HN>0?HS[HN-1].hh:"--:--:--");
    /* A replay must not read as the present: REPLAY i/n leads the title and the badge
       carries the recorded date and time, as on the dashboard */
    if(g_replaying){
        char rt[200]; snprintf(rt,sizeof rt,"%s %d/%d · %s",g_ascii?"REPLAY":"재생",g_rep_i+1,g_rep_n,t1);
        snprintf(t1,sizeof t1,"%.*s",(int)sizeof t1-1,rt);
        char when[32]="--"; time_t rts=g_rep_ts;
        if(rts){ struct tm tmv; rep_localtime(rts,&tmv); strftime(when,sizeof when,"%Y-%m-%d %H:%M:%S",&tmv); }
        snprintf(t2,sizeof t2,"%s %s",g_ascii?"recorded":"기록",when);
    }
    if(g_rec_err[0]){   /* the recording stopped: said where the eye is, not only at exit */
        char rw[200]; snprintf(rw,sizeof rw,"%s %s",g_ascii?"! --record stopped:":"⚠ --record 중단:",g_rec_err);
        snprintf(t2,sizeof t2,"%.*s",(int)sizeof t2-1,rw);
    }

    if(P->pid<0){
        cv_put(2,y,g_ascii?"cub_server is not running — no server timeseries":
                           "cub_server 미기동 — 서버 시계열 없음","38;5;203");
        y+=1;
    }
    if(g_plot_zoom){
        pl_panel(1,y,W,host_ch,M->cores,t1,t2); y+=host_ch+5;
        pl_panel(g_plot_zoom,y,W,ch,M->cores,NULL,NULL); y+=ch+5+sb_rows;
    }
    else for(int k=1;k<=4;k++){
        pl_panel(k,y,W,ch,M->cores,k==1?t1:NULL,k==1?t2:NULL);
        y+=ch+5+(k==3?ab_rows:0);   /* The bar rows are inside the panel box */
    }

    if(g_ascii) snprintf(t1,sizeof t1," [p] dashboard  [1-4] %s  [space] %s  [l] ko/en  [h] help  [q] quit",
                         g_plot_zoom?"back to 4 panels":"zoom one panel",paused?"resume":"pause");
    else        snprintf(t1,sizeof t1," [p] 대시보드  [1-4] %s  [space] %s  [l] 한/영  [h] 도움말  [q] 종료",
                         g_plot_zoom?"4패널 복귀":"패널 확대",paused?"재개":"일시정지");
    cv_put(0,y,cutf(t1,W),paused?"38;5;178":"38;5;244"); y+=1;

    CV.h=y;
    { int trows=term_h(); if(trows<=0) trows=CV.h;
      printf("\033[?2026h\033[?25l\033[H");
      (void)cv_flush_rows(1,trows);          /* Absolute coordinates, no newline: no scroll flicker */
      if(CV.h<trows) printf("\033[%d;1H\033[J",CV.h+1);
      printf("\033[?2026l"); }
    fflush(stdout);
}

/* Reduce a database name to [A-Za-z0-9_] for the terse key=value stream.
   A db name only has to avoid whitespace, '/' and '\' (check_database_name_local
   in util_common.c), so '=' and '"' are legal in one and would otherwise split a
   value into further keys - json_from_terse turns those into real JSON keys.
   Returns out. */
static const char *terse_slug(const char *db,char *out,size_t n){
    size_t a=0,b=0; int us=0;
    if(!db) db="";
    for(;db[a]&&b+1<n;a++){ char c=db[a];
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')){out[b++]=c;us=0;}
        else if(!us&&b){out[b++]='_';us=1;} }
    while(b&&out[b-1]=='_')b--;
    out[b]=0;
    if(!b) snprintf(out,n,"%s","unknown");
    return out;
}

/* The key an instance is filed under.  Two servers can share a database name (two
   installs during an upgrade), and two names can slug alike; the second and later
   get the pid appended, or their keys would repeat and a JSON reader keep only the
   last.  instance.current uses the same rule, so it always names an existing key. */
static const char *inst_key(int i,char *out,size_t n){
    char a[64],b[64];
    terse_slug(g_inst[i].db,a,sizeof a);
    for(int j=0;j<i;j++){
        terse_slug(g_inst[j].db,b,sizeof b);
        if(!strcmp(a,b)){ snprintf(out,n,"%.40s_%d",a,g_inst[i].pid); return out; }
    }
    snprintf(out,n,"%s",a); return out;
}

/* ---------------- Render: terse ---------------- */
static void render_terse(proc_t *P,mem_t *M,tier_t *T,
                         double rb,double wb,double ri,double wi,double absorb,
                         double blkpct,const char *devnm,double devutil,double devr,
                         const char *ts,long tsec,double rchar_bps,
                         double minflt_ps,double majflt_ps,
                         double cpu_pct,int ncpu,int IO_dthr,int IO_nthr){
    double mt=M->total*1024.0, av=M->avail*1024.0;
    /* ts_epoch_ms: the sample interval is sub-second, so a recording needs more than ts_epoch */
    printf("ts=\"%s\" ts_epoch=%ld ts_epoch_ms=%.0f collect.segment=%u\n",tq(ts),tsec,(g_ts_frac>=0?g_ts_frac:(double)tsec)*1000.0,g_seg_gen);
    printf("caps.vm_readv=%d caps.mem_pread=%d caps.rollup=%d caps.proc_io=%d\n",
           CAP.vm_readv,CAP.mem_pread,CAP.rollup,CAP.proc_io);
    printf("perm.limited=%d",g_perm_limited);
    if(g_perm_why[0]) printf(" perm.reason=\"%s\"",tq(g_perm_why));
    printf("\n");
    if(P->reg_dropped || g_disk_dropped)
        printf("limits.regions_dropped=%d limits.regions_max=%d limits.disks_dropped=%d limits.disks_max=%d\n",
               P->reg_dropped,MAXREG,g_disk_dropped,MAXDISK);
    printf("os.ram_total_kb=%lu os.ram_avail_kb=%lu os.cached_kb=%lu "
           "os.swap_total_kb=%lu os.swap_free_kb=%lu\n",
           M->total,M->avail,M->cached,M->swtotal,M->swfree);
    /* The target's memory cgroup (limit -1 = none) and the host's memory PSI (-1 = not provided) */
    printf("os.cgroup_limit_bytes=%.0f os.cgroup_usage_bytes=%.0f os.cgroup_path=\"%s\" os.psi_mem_some10=%.2f os.psi_mem_full10=%.2f\n",
           M->cg_limit,M->cg_usage,tq(M->cg_path),M->psi_some10,M->psi_full10);
    /* Host CPU, the same values as the TUI's OS box.  us/sy/wa are percentages
       averaged over all cores (top's %Cpu(s) convention) and busy_cores converts
       that to cores.  server.cpu_pct uses top's "one core = 100" convention, so
       compare cpu_pct/100 against busy_cores. */
    printf("os.cpu_us_pct=%.1f os.cpu_sy_pct=%.1f os.cpu_wa_pct=%.1f"
           " os.cpu_busy_cores=%.2f os.cores_logical=%d os.cores_physical=%d\n",
           M->cpu_us,M->cpu_sy,M->cpu_wa,M->busy_cores,M->cores,M->pcores);
    printf("os.load1=%.2f os.load5=%.2f os.load15=%.2f\n",
           M->load1,M->load5,M->load15);
    /* Core saturation distribution: busy_cores alone cannot say whether the load is spread
       evenly or a few cores are at 100%.  sat is 95% and above, hi is 50-95%, max is the
       busiest core. */
    printf("os.cores_saturated=%d os.cores_high=%d os.core_max_pct=%.1f\n",
           M->sat_cores,M->hi_cores,M->core_max);
    /* The detected CUBRID version and the adopted offset table - the only mechanical way to
       check support status on a foreign deployment. */
    printf("cubrid.version=\"%s\" heapB.table=\"%s\" heapB.lib=\"%s\"\n",
           tq(g_cver[0]?g_cver:"unknown"),
           tq(!B.ok?"-":(g_replaying&&g_rp_tbl[0])?g_rp_tbl:(B.partial?"probe-runtime":g_bt->tag)),
           tq(g_so_path[0]?g_so_path:"-"));   /* A map_files path means the running inode was read */
    /* Whose build the adopted table was taken from: an offsets-other table is another release */
    if(g_replaying){ if(g_rp_tblver[0]) printf("heapB.table_version=\"%s\"\n",tq(g_rp_tblver)); }   /* as recorded */
    else if(B.ok && !B.partial) printf("heapB.table_version=\"%s\"\n",tq(g_bt->ver?g_bt->ver:"-"));
    printf("server.up=%d\n",P->pid<0?0:1);
    /* All instances.  The detailed keys (server, region) cover only the current one, so the
       other databases' totals are available nowhere else.  Keys are namespaced by db. */
    { char cs[64];
      printf("instance.count=%d instance.current=\"%s\"\n",g_ninst,
             tq(g_ninst>0?inst_key(g_cur,cs,sizeof cs):"-")); }
    for(int i=0;i<g_ninst;i++){
        char k[64];
        inst_key(i,k,sizeof k);
        printf("instance.%s.pid=%d instance.%s.uid=%d instance.%s.rss_bytes=%.0f instance.%s.file_bytes=%.0f instance.%s.swap_bytes=%.0f"
               " instance.%s.pl_rss_bytes=%.0f instance.%s.pl_count=%d instance.%s.tracked=%d\n",
               k,g_inst[i].pid,k,g_inst[i].uid,k,g_inst[i].rss,k,g_inst[i].filemap,k,g_inst[i].swap,
               k,g_inst[i].pl_rss,k,g_inst[i].pl_n,k,i==g_cur?1:0);
    }
    if(P->pid<0){
        for(int i=0;i<g_nprm;i++)
            printf("conf.%s=%lld\n",g_prm[i].name,g_prm[i].val);
        if(devnm[0]) printf("hostdev.name=\"%s\" hostdev.util_pct=%.1f\n",tq(devnm),devutil);
        printf("tier.server_rss_bytes=0 tier.master_rss_bytes=%.0f tier.broker_rss_bytes=%.0f"
               " tier.cas_rss_bytes=%.0f tier.cas_count=%d tier.pl_rss_bytes=%.0f tier.pl_count=%d\n",
               T->master,T->broker,T->cas,T->ncas,T->pl,T->npl);
        /* CPU per tier, in cores (the same unit as os.cpu_busy_cores). */
        printf("tier.master_cores=%.2f tier.broker_cores=%.2f"
               " tier.cas_cores=%.2f tier.pl_cores=%.2f tier.server_cores=0.00\n",
               T->cpu_master,T->cpu_broker,T->cpu_cas,T->cpu_pl);
        printf("total.cubrid_rss_bytes=%.0f\n",T->master+T->broker+T->cas+T->pl);
        return;
    }
    { char ds[64]; terse_slug(P->db,ds,sizeof ds);
    printf("server.db=\"%s\" server.pid=%d server.mapped_kb=%lu server.rss_kb=%lu "
           "server.pss_kb=%lu server.swap_kb=%lu server.workers=%d"
           " server.minflt_per_s=%.0f server.majflt_per_s=%.0f"
           " server.cpu_pct=%.1f server.host_cores=%d server.start_ticks=%llu"
           " server.smaps_age_s=%.1f server.smaps_ms=%.1f server.thp_kb=%lu\n",
           tq(ds),P->pid,P->vsize_kb,P->rss_kb,P->pss_kb,P->swap_kb,P->nthreads,
           minflt_ps,majflt_ps,cpu_pct,ncpu,g_track_start,P->smaps_age_s,P->smaps_ms,P->thp_kb); }
    for(int i=0;i<NSR;i++){
        srg_t *r=&SR[i];
        /* Strip spaces and parentheses from keys so a key=value parser does not break */
        char k[24]; { size_t a=0,b=0; int us=0;
            for(;r->name[a]&&b+1<sizeof k;a++){ char c=r->name[a];
                if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')){k[b++]=c;us=0;}
                else if(!us&&b){k[b++]='_';us=1;} }
            while(b&&k[b-1]=='_')b--;
            k[b]=0; }
        printf("region.%s.mapped_bytes=%.0f region.%s.rss_bytes=%.0f",
               k,r->mapped,k,r->rss);
        if(r->cfg>0) printf(" region.%s.cfg_bytes=%lld region.%s.over_pct=%.1f",
                            k,r->cfg,k,r->mapped/(double)r->cfg*100);
        printf("\n");
    }
    printf("heapB.enabled=%d heapB.reason=\"%s\" heapB.total_bytes=%.0f heapB.excluded=%d\n",
           B.ok,tq(B.why),B.total,B.n_bad);
    for(int i=0;i<B.n;i++)
    {   const char *sl=slugify(B.it[i].name);
        char kb[64];
        if(!sl[0]){ snprintf(kb,sizeof kb,"item%d",i); sl=kb; }   /* Safety net for non-ASCII labels */
        if(B.it[i].count_only)
             printf("heapB.%s_count=%ld\n",sl,B.it[i].count);
        else printf("heapB.%s_bytes=%.0f heapB.%s_grade=%c\n",sl,B.it[i].bytes,sl,B.it[i].grade); }
    /* Capacity axis: demand, ceiling, headroom.  Snapshot-based estimate. */
    printf("cap.aggregate_ms=%.1f cap.over_budget=%d cap.budget_ms=%.0f\n",CAP2.ms,CAP2.over_budget,(double)CAP_BUDGET_MS);
    printf("cap.perfmon_counters_running=%d cap.hit_source=\"%s\" cap.reason=\"%s\"\n",
           CAP2.ps_ok,tq(CAP2.hit_src),tq(CAP2.ps_why));
    if(!CAP2.over_budget){
        if(CAP2.hit_pct>=0)       printf("cap.buffer_hit_pct=%.2f\n",CAP2.hit_pct);
        if(CAP2.miss_pps>=0)      printf("cap.miss_pages_per_s=%.0f cap.miss_source=\"%s\"\n",CAP2.miss_pps,CAP2.miss_src);
        if(CAP2.turnover_pct>=0){ printf("cap.turnover_pct=%.2f\n",CAP2.turnover_pct);
                                  if(CAP2.readmit_pct>=0) printf("cap.readmit_pct=%.2f\n",CAP2.readmit_pct);
                                  if(CAP2.adm_frac>=0) printf("cap.admit_frac_of_buffer=%.3f\n",CAP2.adm_frac);
                                  else printf("cap.readmit_available=0\n"); }
        if(CAP2.await_ms>=0)      printf("cap.await_ms=%.2f cap.read_await_ms=%.2f cap.write_await_ms=%.2f\n",
                                         CAP2.await_ms,CAP2.r_await_ms<0?0:CAP2.r_await_ms,CAP2.w_await_ms<0?0:CAP2.w_await_ms);
        if(CAP2.qdepth>=0)        printf("cap.queue_depth=%.2f\n",CAP2.qdepth);
        printf("cap.device=\"%s\" cap.device_via=\"%s\" cap.device_reason=\"%s\"\n",tq(CAP2.dev),
               g_dk_via==1?"st_dev":g_dk_via==2?"mount":g_dk_via==3?"statfs_fsid":"",tq(CAP2.dev_why));
        printf("cap.dev_iops=%.0f cap.dev_util=%.1f cap.proc_iops=%.0f\n",CAP2.dev_iops,CAP2.dev_util,rnd_as_printed(CAP2.riops,0)+rnd_as_printed(CAP2.wiops,0));
        /* I/O profile: ceilings are compared only within this cell (block size x read share) */
        printf("cap.req_kb=%.1f cap.read_ratio=%.2f cap.profile=\"%s/%s\"\n",
               CAP2.req_kb,CAP2.read_ratio,CAP_BS_NAME[CAP2.prof_bs],CAP_RW_NAME[CAP2.prof_rw]);
        { int nc=0; for(int b=0;b<CAP_BS_N;b++) for(int r=0;r<CAP_RW_N;r++) if(g_cap_mtx[b][r].valid) nc++;
          printf("cap.ceiling_cells=%d\n",nc);
          if(g_cap_devid[0]) printf("cap.ceiling_device_id=\"%s\"\n",tq(g_cap_devid));
          for(int b=0;b<CAP_BS_N;b++) for(int r=0;r<CAP_RW_N;r++){
              const cap_cell_t*cc=&g_cap_mtx[b][r]; if(!cc->valid) continue;
              printf("cap.cell%d%d.profile=\"%s/%s\" cap.cell%d%d.iops=%.0f cap.cell%d%d.util_pct=%.1f"
                     " cap.cell%d%d.await_ms=%.2f cap.cell%d%d.queue_depth=%.2f cap.cell%d%d.epoch=%lld\n",
                     b,r,CAP_BS_NAME[b],CAP_RW_NAME[r],b,r,cc->iops,b,r,cc->util,
                     b,r,cc->await_ms,b,r,cc->qd,b,r,(long long)cc->when);
          } }
        if(CAP2.ceil_valid)       printf("cap.ceiling_iops=%.0f cap.ceiling_bps=%.0f cap.ceiling_util_pct=%.1f"
                                         " cap.ceiling_qdepth=%.2f cap.ceiling_epoch=%lld\n",
                                         CAP2.ceil_iops,CAP2.ceil_bps,CAP2.ceil_util,CAP2.ceil_qd,(long long)CAP2.ceil_when);
        if(CAP2.headroom_pct>=0)  printf("cap.used_of_ceiling_pct=%.1f\n",CAP2.headroom_pct);
        if(CAP2.ps_ok)            printf("cap.pb_fetch_per_s=%.0f cap.pb_ioread_per_s=%.0f cap.pb_iowrite_per_s=%.0f"
                                         " cap.log_iowrite_per_s=%.0f cap.commit_per_s=%.0f\n",
                                         CAP2.pb_fetch_ps,CAP2.pb_ioread_ps,CAP2.pb_iowrite_ps,
                                         CAP2.log_iowrite_ps,CAP2.commit_ps);
    }
    { char vb[320]; const char*vc=VD_INFO; vd_capacity_shown(vb,sizeof vb,&vc);
      printf("cap.verdict=\"%s\"\n",tq(vb)); }
    /* BCB direct read, graded as an estimate (a latch-free snapshot).  The keys share their source (the dump) with volmap --bufmap. */
    printf("pgbuf.enabled=%d pgbuf.reason=\"%s\" pgbuf.scan_age_s=%.1f pgbuf.sample_miss_per_s=%.0f\n",PG.ok,tq(PG.why),PG.scan_age_s,SM.miss_pps);
    if(PG.ok){
        printf("pgbuf.num_buffers=%d pgbuf.resident=%d pgbuf.dirty=%d pgbuf.flushing=%d"
               " pgbuf.zone1_hot=%d pgbuf.zone2_warm=%d pgbuf.zone3_cold=%d pgbuf.zone_void=%d pgbuf.zone_free=%d"
               " pgbuf.hdr_mismatch=%d pgbuf.hdr_unread=%d pgbuf.scan_ms=%.1f\n",
               PG.nbuf,PG.resident,PG.dirty,PG.flushing,PG.z1,PG.z2,PG.z3,PG.zvoid,PG.zinv,PG.mismatch,PG.unread,PG.ms);
        printf("pgbuf.hdr_mismatch_raw=%d pgbuf.rechecked=%d pgbuf.degraded=%d\n",
               PG.mismatch_raw,PG.recheck,PG.degraded);
        printf("pgbuf.hdr_read=%d pgbuf.hdr_nonresident=%d pgbuf.hdr_pagemap_unchecked=%d\n",
               PG.nhdr,PG.nonres,PG.pm_unchecked);
        for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]>0)
            printf("pgbuf.vol%d.resident=%d pgbuf.vol%d.dirty=%d\n",v,PG.vol_res[v],v,PG.vol_dirty[v]);
        if(PG.have_oldest) printf("pgbuf.oldest_dirty_pageid=%lld pgbuf.oldest_dirty_offset=%d\n",
                                  (long long)PG.oldest_dirty.pageid,PG.oldest_dirty.offset);
        /* Not yet written by the buffer, and what the oldest LSA cannot see: a flush clears
           DIRTY and the page's oldest_unflush_lsa before it writes.  With a DWB, a page counts
           as flushed once copied into it, so "clean" may still wait there for its volume write. */
        printf("pgbuf.unflushed=%d pgbuf.oldest_dirty_excludes_flushing=%d\n",PG.unflushed,PG.flush_only);
        { int dp=dwb_pages(); if(dp>=0) printf("pgbuf.dwb_enabled=%d pgbuf.dwb_pages_max=%d\n",dp>0,dp); }
    }
    {
        if(PG.have_log){
            printf("log.append_pageid=%lld log.append_offset=%d log.nxio_pageid=%lld log.nxio_offset=%d"
                   " log.eof_pageid=%lld log.eof_offset=%d log.flush_lag_pages=%lld",
                   (long long)PG.log_append.pageid,PG.log_append.offset,(long long)PG.log_nxio.pageid,PG.log_nxio.offset,
                   (long long)PG.log_eof.pageid,PG.log_eof.offset,(long long)(PG.log_append.pageid-PG.log_nxio.pageid));
            if(PG.ok && PG.have_oldest) printf(" log.dirty_span_pages=%lld",(long long)(PG.log_append.pageid-PG.oldest_dirty.pageid));
            printf("\n");
        }
    }
    printf("pgbuf.engine.enabled=%d pgbuf.engine.reason=\"%s\"\n",AG.ok,tq(AG.ok?(g_ascii?"engine aggregates":"엔진 집계"):AG.why));
    if(AG.ok)
        printf("pgbuf.engine.num_buffers=%d pgbuf.engine.lists=%d pgbuf.engine.lists_shared=%d pgbuf.engine.lru_pages=%ld"
               " pgbuf.engine.zone1=%ld pgbuf.engine.zone2=%ld pgbuf.engine.zone3=%ld pgbuf.engine.victim_candidates=%ld"
               " pgbuf.engine.dirty=%lld pgbuf.engine.read_ms=%.2f\n",
               AG.nbuf,AG.nlist,AG.nshared,AG.z1+AG.z2+AG.z3,AG.z1,AG.z2,AG.z3,AG.vict,AG.dirty,AG.ms);
    /* The volumes' page-cache residency (mincore), on its own period: bytes 0 = not measured */
    printf("volcache.bytes=%.0f volcache.resident_bytes=%.0f volcache.resident_pct=%.1f volcache.files=%d volcache.age_s=%.1f volcache.reason=\"%s\"\n",
           VC.total,VC.resident,VC.total>0?100.0*VC.resident/VC.total:-1.0,VC.nfiles,VC.age_s,tq(VC.why));
    printf("io.read_bps=%.0f io.write_bps=%.0f io.read_iops=%.0f io.write_iops=%.0f ",
           rb,wb,ri,wi);
    /* Below the rchar floor the ratio is meaningless, so the key is omitted entirely rather
       than a non-numeric "NA" placed in a numeric field. */
    if(absorb>=0 && rchar_bps>=RCHAR_MIN) printf("io.cache_absorb_pct=%.1f ",absorb*100);
    /* absorb_ok states WHY the ratio is absent, so a replay does not read a missing key as
       "measured 0"; rchar is its denominator. */
    printf("io.cache_absorb_ok=%d io.rchar_bps=%.0f ",
           (absorb>=0 && rchar_bps>=RCHAR_MIN)?1:0, rchar_bps);
    printf("io.blkio_wait_pct=%.2f io.total_read_bytes=%lu io.total_write_bytes=%lu\n",
           blkpct,P->read_b,P->write_b);
    /* D (disk wait) threads: wa is diluted by the core count, this is not.
       The same value that appears right of the iowait row on screen. */
    printf("server.d_threads=%d server.threads_total=%d\n",IO_dthr,IO_nthr);
    /* Where the param.* rows come from: the server's applied values, or cubrid.conf while
       paramdump has not answered (g_pd_why says why not). */
    printf("param.source=\"%s\"\n",g_pd_applied?"paramdump":g_nprm>0?"cubrid.conf":"none");
    if(g_pd_why[0]) printf("param.skipped_reason=\"%s\"\n",tq(g_pd_why));
    /* io.* comes from /proc/<pid>/io and belongs to the current instance, while the device
       figures come from diskstats and are host-wide - separate key prefixes keep the
       two scopes from being confused. */
    if(devnm[0]) printf("hostdev.name=\"%s\" hostdev.util_pct=%.1f hostdev.read_bps=%.0f\n",
                        tq(devnm),devutil,devr);
    for(int i=0;i<NPRMROWS;i++){
        long long v=prm_get(PRMROWS[i].name);
        if(v>=0) printf("param.%s=%lld param.%s.observed=%d\n",
                        PRMROWS[i].name,v,PRMROWS[i].name,PRMROWS[i].obs);
    }
    /* A total alone cannot say which tier grew, so RSS (as top) is emitted per tier */
    printf("tier.server_rss_bytes=%.0f tier.master_rss_bytes=%.0f tier.broker_rss_bytes=%.0f"
           " tier.cas_rss_bytes=%.0f tier.cas_count=%d tier.pl_rss_bytes=%.0f tier.pl_count=%d\n",
           P->rss_kb*1024.0,T->master,T->broker,T->cas,T->ncas,T->pl,T->npl);
    /* CPU per tier, in cores (the same unit as os.cpu_busy_cores).
       server is server.cpu_pct (one core = 100) divided by 100 to match. */
    printf("tier.server_cores=%.2f tier.master_cores=%.2f tier.broker_cores=%.2f"
           " tier.cas_cores=%.2f tier.pl_cores=%.2f\n",
           rnd_as_printed(cpu_pct,1)/100.0,T->cpu_master,T->cpu_broker,T->cpu_cas,T->cpu_pl);
    printf("total.cubrid_rss_bytes=%.0f total.cubrid_pct_ram=%.2f\n",
           P->rss_kb*1024.0+T->master+T->broker+T->cas+T->pl,
           (P->rss_kb*1024.0+T->master+T->broker+T->cas+T->pl)/(mt>0?mt:1)*100);
    (void)av;
}

/* A quoted value for -d/--json/--record: \" \\ and control characters escaped (JSON's
   escapes, so json_from_terse copies them through and rp_find undoes them), and any byte
   that is not part of a valid UTF-8 sequence - a Korean reason cut mid-character by a fixed
   buffer - replaced by '?', so the stream stays valid UTF-8.  Ring of 8 results. */
static const char *tq(const char *s){
    static char ring[8][4096]; static int k=0; char *o=ring[k++&7]; size_t n=0;
    const unsigned char *u=(const unsigned char*)(s?s:"");
    while(*u && n+8<sizeof ring[0]){
        unsigned c=*u; int len = c<0x80?1 : (c&0xE0)==0xC0?2 : (c&0xF0)==0xE0?3 : (c&0xF8)==0xF0?4 : 0;
        int ok = len>0 && !(len==2 && c<0xC2) && !(len==4 && c>0xF4);
        for(int i=1;ok && i<len;i++) if((u[i]&0xC0)!=0x80) ok=0;
        /* Overlong forms, UTF-16 surrogates (CESU-8) and code points past U+10FFFF */
        if(ok && ((c==0xE0 && u[1]<0xA0) || (c==0xED && u[1]>=0xA0) ||
                  (c==0xF0 && u[1]<0x90) || (c==0xF4 && u[1]>=0x90))) ok=0;
        if(!ok){ o[n++]='?'; u++; continue; }
        if(len>1){ memcpy(o+n,u,(size_t)len); n+=(size_t)len; u+=len; continue; }
        if(c=='"'||c=='\\'){ o[n++]='\\'; o[n++]=(char)c; }
        else if(c=='\n'){ o[n++]='\\'; o[n++]='n'; }
        else if(c=='\t'){ o[n++]='\\'; o[n++]='t'; }
        else if(c=='\r'){ o[n++]='\\'; o[n++]='r'; }
        else if(c<0x20) n+=(size_t)snprintf(o+n,7,"\\u%04x",c);
        else o[n++]=(char)c;
        u++;
    }
    o[n]=0; return o;
}
/* ---- JSON output (--json) ----
 * terse (key=value) is the single source: producing the same content twice would
 * drift, so terse output is mechanically converted, keeping keys, values and order
 * identical to -t.  Quoted values become strings, numerals become numbers, and any
 * other bare token (a device name) becomes a string. */
static void json_escape_out(FILE*o,const char*s,size_t n){
    for(size_t i=0;i<n;i++){ unsigned char c=(unsigned char)s[i];
        if(c=='"'||c=='\\') fprintf(o,"\\%c",c);
        else if(c<0x20) fprintf(o,"\\u%04x",c);
        else fputc(c,o); }
}
/* JSON's number grammar: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?  - strtod would also
   take 0123, 0x1f, +5, inf and nan, none of them a JSON number */
static int json_number(const char *v,size_t n){
    size_t i=0;
    if(i<n && v[i]=='-') i++;
    if(i>=n || !isdigit((unsigned char)v[i])) return 0;
    if(v[i]=='0') i++; else while(i<n && isdigit((unsigned char)v[i])) i++;
    if(i<n && v[i]=='.'){ i++; if(i>=n || !isdigit((unsigned char)v[i])) return 0;
                          while(i<n && isdigit((unsigned char)v[i])) i++; }
    if(i<n && (v[i]=='e'||v[i]=='E')){ i++; if(i<n && (v[i]=='+'||v[i]=='-')) i++;
                          if(i>=n || !isdigit((unsigned char)v[i])) return 0;
                          while(i<n && isdigit((unsigned char)v[i])) i++; }
    return i==n;
}
/* A quoted terse value carries tq()'s escapes, which are JSON's: kept as they are, a stray
   backslash or control byte escaped */
static void json_copy_escaped(FILE*o,const char*s,size_t n){
    for(size_t i=0;i<n;i++){ unsigned char c=(unsigned char)s[i];
        if(c=='\\'){ if(i+1<n && strchr("\"\\/bfnrtu",s[i+1])){ fputc('\\',o); fputc(s[++i],o); }
                      else fputs("\\\\",o); }
        else if(c=='"') fputs("\\\"",o);
        else if(c<0x20) fprintf(o,"\\u%04x",c);
        else fputc(c,o); }
}
static void render_terse(proc_t*,mem_t*,tier_t*,double,double,double,double,double,
                         double,const char*,double,double,const char*,long,double,
                         double,double,double,int,int,int);
static void json_from_terse(FILE*,FILE*);
static void json_line_from_terse(FILE*,FILE*);

/* ---- Record and replay ----
 * A recording is the terse/JSON stream itself, one object per line (JSONL): the
 * same keys -t and --json already emit, so nothing is serialized twice and a
 * recording stays readable with jq.
 *
 * Replay reconstructs the structs the renderers take and calls the same
 * render_dash()/render_plot(), so what is drawn is what the recorder saw.  No
 * /proc is read: rp_apply() below is the only place a frame is rebuilt.
 */
#define RP_MAXLINE (1<<20)

typedef struct { char *buf; size_t cap; FILE *f; int n; } rp_rec_t;
static rp_rec_t RPR={0,0,NULL,0};

/* The four hex digits of a JSON "\u" escape */
static int hex4(const char *h,unsigned *cp){
    unsigned v=0;
    for(int k=0;k<4;k++){ int c=(unsigned char)h[k]; if(!isxdigit(c)) return 0;
                          v=v*16+(unsigned)(isdigit(c)?c-'0':tolower(c)-'a'+10); }
    *cp=v; return 1;
}
/* One code point (not a surrogate, at most U+10FFFF) as UTF-8; the byte count */
static int u8enc(unsigned cp,char *e){
    if(cp<0x80){ e[0]=(char)cp; return 1; }
    if(cp<0x800){ e[0]=(char)(0xC0|cp>>6); e[1]=(char)(0x80|(cp&63)); return 2; }
    if(cp<0x10000){ e[0]=(char)(0xE0|cp>>12); e[1]=(char)(0x80|((cp>>6)&63)); e[2]=(char)(0x80|(cp&63)); return 3; }
    e[0]=(char)(0xF0|cp>>18); e[1]=(char)(0x80|((cp>>12)&63)); e[2]=(char)(0x80|((cp>>6)&63)); e[3]=(char)(0x80|(cp&63)); return 4;
}
/* Look up one key in a recorded frame (a JSON object on one line). */
static int rp_find(const char *txt,const char *key,char *out,size_t on){
    /* A recorded frame is one JSON object: "key": value.  The key is matched with its
       quotes so "os.load1" cannot match "os.load15", and the value is taken whether it
       is quoted or bare. */
    char pat[128];
    int pl=snprintf(pat,sizeof pat,"\"%s\":",key);
    if(pl<=0 || (size_t)pl>=sizeof pat) return 0;
    const char *p=strstr(txt,pat);
    if(!p) return 0;
    const char *v=p+pl;
    while(*v==' '||*v=='\t') v++;
    size_t vl;
    if(*v=='"'){                     /* a JSON string: its escapes undone */
        v++; size_t n=0;
        while(*v && *v!='"' && n+1<on){
            if(*v!='\\' || !v[1]){              /* a whole UTF-8 character, or stop */
                unsigned c=(unsigned char)*v; size_t l=c<0x80?1:(c&0xE0)==0xC0?2:(c&0xF0)==0xE0?3:(c&0xF8)==0xF0?4:1;
                if(n+l>=on) break;
                for(size_t i=0;i<l && *v;i++) out[n++]=*v++;
                continue; }
            if(n+3>=on) break;
            v++;
            switch(*v){
            case 'n': out[n++]='\n'; v++; break;
            case 't': out[n++]='\t'; v++; break;
            case 'r': out[n++]='\r'; v++; break;
            case 'b': out[n++]='\b'; v++; break;
            case 'f': out[n++]='\f'; v++; break;
            case 'u': { unsigned cp, lo;
                        if(!hex4(v+1,&cp)){ out[n++]='?'; v++; break; }
                        v+=5;
                        /* A surrogate pair is one character (4 bytes); a lone surrogate, or NUL,
                           has no UTF-8 form here and becomes '?' */
                        if(cp>=0xD800 && cp<=0xDBFF && v[0]=='\\' && v[1]=='u' && hex4(v+2,&lo) && lo>=0xDC00 && lo<=0xDFFF){
                            cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00); v+=6; }
                        else if((cp>=0xD800 && cp<=0xDFFF) || cp==0) cp='?';
                        char e[4]; int el=u8enc(cp,e);
                        if(n+(size_t)el>=on){ out[n]=0; return 1; }
                        memcpy(out+n,e,(size_t)el); n+=(size_t)el; break; }
            default:  out[n++]=*v++; break;      /* \" \\ \/ */
            }
        }
        out[n]=0; return 1;
    } else {
        const char *e=v;
        while(*e && *e!=',' && *e!='}' && *e!='\n' && *e!='\r') e++;
        while(e>v && (e[-1]==' '||e[-1]=='\t')) e--;
        vl=(size_t)(e-v);
    }
    if(vl>=on) vl=on-1;
    memcpy(out,v,vl); out[vl]=0; return 1;
}
static double rp_num(const char *txt,const char *key,double dflt);
static double rp_num(const char *txt,const char *key,double dflt){
    char b[128]; if(!rp_find(txt,key,b,sizeof b)) return dflt;
    char *end; double d=strtod(b,&end); return (end==b)?dflt:d;
}
static void rp_str(const char *txt,const char *key,char *out,size_t on){
    if(!rp_find(txt,key,out,on)) out[0]=0;
}
/* A frame's time in seconds: ts_epoch_ms when recorded, else the whole-second ts_epoch */
static double rp_time(const char *txt){
    double ms=rp_num(txt,"ts_epoch_ms",-1);
    return ms>=0 ? ms/1000.0 : rp_num(txt,"ts_epoch",0);
}

/* Rebuild one frame's structs from a recorded line.  This is the ONLY place a
   replayed frame comes from - no /proc is touched, so a replay on another host
   shows exactly what the recorder saw. */
/* Append one frame to the recording.  The frame is produced by render_terse() and
   converted by json_from_terse(), the same pair -t and --json use, so a recording
   cannot drift from those outputs. */
/* Recording stops at the first failure and says so: a capture that ends with status 0
   but a short or empty file would be trusted. */
static void rec_fail(const char *what,int e){
    if(!g_rec_err[0]) snprintf(g_rec_err,sizeof g_rec_err,"%s: %s",what,strerror(e?e:EIO));
    if(RPR.f){ if(RPR.f!=stdout) fclose(RPR.f); RPR.f=NULL; }
    g_rec_path=NULL;
}
/* The frame is rendered in memory (memfd, else a temporary file) - not on a /tmp that may be full */
static FILE *mem_file(void){
#ifdef __NR_memfd_create
    int fd=(int)syscall(__NR_memfd_create,"cub_top",1u /* MFD_CLOEXEC */);
    if(fd>=0){ FILE *f=fdopen(fd,"w+"); if(f) return f; close(fd); }
#endif
    return tmpfile();
}
static void rp_record(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,const char*ts,long tsec){
    if(!RPR.f){
        char why[160]; RPR.f=out_open(g_rec_path,g_rec_overwrite,why,sizeof why);
        if(!RPR.f){ fprintf(stderr,"cub_top: --record %s: %s\n",g_rec_path,why);
                    g_rec_path=NULL; return; }
    }
    FILE *tf=mem_file(); if(!tf){ rec_fail(g_ascii?"no buffer for the frame":"프레임 버퍼 없음",errno); return; }
    fflush(stdout);
    int save=dup(1); if(save<0){ int e=errno; fclose(tf); rec_fail("dup",e); return; }
    if(dup2(fileno(tf),1)<0){ int e=errno; close(save); fclose(tf); rec_fail("dup2",e); return; }
    render_terse(P,M,T,IO->rb,IO->wb,IO->ri,IO->wi,
                 IO->absorb_ok?IO->absorb:-1.0,IO->blkpct,IO->devnm,IO->devutil,IO->devr,
                 ts,tsec,IO->rchar,IO->minflt,IO->majflt,IO->cpu_pct,IO->ncpu,
                 IO->d_thr,IO->n_thr);
    int werr = fflush(stdout)!=0 || ferror(stdout); int e=errno;
    clearerr(stdout); dup2(save,1); close(save);
    if(werr){ fclose(tf); rec_fail(g_ascii?"rendering the frame":"프레임 렌더링",e); return; }
    /* One object per line: json_from_terse writes it multi-line, so newlines are folded. */
    char *b=NULL; size_t bn=0; FILE *lf=open_memstream(&b,&bn);
    if(!lf){ e=errno; fclose(tf); rec_fail("open_memstream",e); return; }
    json_from_terse(tf,lf); fclose(tf);
    if(fclose(lf)!=0 || !b || !bn){ e=errno; free(b); rec_fail(g_ascii?"converting the frame":"프레임 변환",e); return; }
    for(size_t i=0;i<bn;i++) if(b[i]=='\n') b[i]=' ';
    errno=0;
    int bad = fputs(b,RPR.f)<0 || fputc('\n',RPR.f)==EOF || fflush(RPR.f)!=0 || ferror(RPR.f);
    e=errno; free(b);
    if(bad){ rec_fail(g_ascii?"writing":"쓰기",e); return; }
    RPR.n++;
}

static void rp_apply(const char *t,proc_t*P,mem_t*M,tier_t*T,iod_t*IO){
    char b[512];
    memset(P,0,sizeof *P); memset(M,0,sizeof *M);
    memset(T,0,sizeof *T); memset(IO,0,sizeof *IO);

    M->total  =rp_num(t,"os.ram_total_kb",0);   M->avail =rp_num(t,"os.ram_avail_kb",0);
    M->cached =rp_num(t,"os.cached_kb",0);
    M->swtotal=rp_num(t,"os.swap_total_kb",0);  M->swfree=rp_num(t,"os.swap_free_kb",0);
    M->cores  =(int)rp_num(t,"os.cores_logical",0);
    M->pcores =(int)rp_num(t,"os.cores_physical",0);
    M->busy_cores=rp_num(t,"os.cpu_busy_cores",0);
    M->sat_cores =(int)rp_num(t,"os.cores_saturated",0);
    M->hi_cores  =(int)rp_num(t,"os.cores_high",0);
    M->cpu_us=rp_num(t,"os.cpu_us_pct",0); M->cpu_sy=rp_num(t,"os.cpu_sy_pct",0);
    M->cpu_wa=rp_num(t,"os.cpu_wa_pct",0);
    M->load1 =rp_num(t,"os.load1",0); M->load5=rp_num(t,"os.load5",0);
    M->load15=rp_num(t,"os.load15",0);
    M->cg_limit=rp_num(t,"os.cgroup_limit_bytes",-1); M->cg_usage=rp_num(t,"os.cgroup_usage_bytes",-1);
    rp_str(t,"os.cgroup_path",M->cg_path,sizeof M->cg_path);
    M->psi_some10=rp_num(t,"os.psi_mem_some10",-1); M->psi_full10=rp_num(t,"os.psi_mem_full10",-1);
    M->core_max=rp_num(t,"os.core_max_pct",0);

    P->pid     =(int)rp_num(t,"server.pid",-1);
    g_seg_gen  =(unsigned)rp_num(t,"collect.segment",0);              /* the history follows the recorded segments */
    g_track_start=(unsigned long long)rp_num(t,"server.start_ticks",0);
    P->smaps_age_s=rp_num(t,"server.smaps_age_s",0); P->smaps_ms=rp_num(t,"server.smaps_ms",0);
    P->thp_kb=(unsigned long)rp_num(t,"server.thp_kb",0);
    P->rss_kb  =(unsigned long)rp_num(t,"server.rss_kb",0);
    P->pss_kb  =(unsigned long)rp_num(t,"server.pss_kb",0);
    P->vsize_kb=(unsigned long)rp_num(t,"server.mapped_kb",0);
    P->swap_kb =(unsigned long)rp_num(t,"server.swap_kb",0);
    P->nthreads=(int)rp_num(t,"server.threads_total",0);
    P->read_b  =(unsigned long)rp_num(t,"io.total_read_bytes",0);
    P->write_b =(unsigned long)rp_num(t,"io.total_write_bytes",0);
    rp_str(t,"server.db",P->db,sizeof P->db);

    IO->rb=rp_num(t,"io.read_bps",0);  IO->wb=rp_num(t,"io.write_bps",0);
    IO->ri=rp_num(t,"io.read_iops",0); IO->wi=rp_num(t,"io.write_iops",0);
    IO->blkpct=rp_num(t,"io.blkio_wait_pct",0);
    IO->rchar =rp_num(t,"io.rchar_bps",0);
    VC.total=rp_num(t,"volcache.bytes",0); VC.resident=rp_num(t,"volcache.resident_bytes",0);
    VC.nfiles=(int)rp_num(t,"volcache.files",0); VC.age_s=rp_num(t,"volcache.age_s",0); rp_str(t,"volcache.reason",VC.why,sizeof VC.why);
    IO->absorb_ok=(int)rp_num(t,"io.cache_absorb_ok",0);
    IO->absorb=IO->absorb_ok?rp_num(t,"io.cache_absorb_pct",0)/100.0:-1.0;
    IO->devutil=rp_num(t,"hostdev.util_pct",0);
    IO->devr   =rp_num(t,"hostdev.read_bps",0);
    rp_str(t,"hostdev.name",IO->devnm,sizeof IO->devnm);
    IO->minflt=rp_num(t,"server.minflt_per_s",0);
    IO->majflt=rp_num(t,"server.majflt_per_s",0);
    IO->cpu_pct=rp_num(t,"server.cpu_pct",0);
    IO->ncpu  =(int)rp_num(t,"server.host_cores",0);
    IO->d_thr =(int)rp_num(t,"server.d_threads",0);
    IO->n_thr =(int)rp_num(t,"server.threads_total",0);

    T->server_rss=rp_num(t,"tier.server_rss_bytes",0);
    T->master=rp_num(t,"tier.master_rss_bytes",0);
    T->broker=rp_num(t,"tier.broker_rss_bytes",0);
    T->cas   =rp_num(t,"tier.cas_rss_bytes",0);
    T->pl    =rp_num(t,"tier.pl_rss_bytes",0);
    T->ncas  =(int)rp_num(t,"tier.cas_count",0);
    T->npl   =(int)rp_num(t,"tier.pl_count",0);
    T->cpu_master=rp_num(t,"tier.master_cores",0);
    T->cpu_broker=rp_num(t,"tier.broker_cores",0); T->cpu_cas=rp_num(t,"tier.cas_cores",0);
    T->cpu_pl    =rp_num(t,"tier.pl_cores",0);

    /* Regions: the JSON slugifies names ("thread stacks" -> thread_stacks), so the
       map back is explicit rather than guessed. */
    static const struct { const char *slug, *name; char grade; } RGN[]={
        {"data_buffer","data_buffer",'M'}, {"log_buffer","log_buffer",'M'},
        {"dynamic_heap","dynamic-heap",'U'},{"thread_stacks","thread stacks",'R'},
        {"code_so","code (.so)",'M'},      {"bin_shm_etc","bin/shm etc",'M'},
        {"glibc_arena","glibc arena",'R'},
    };
    /* SR[] is rebuilt every frame, but the grow column's baseline lives in it
       (render_dash writes prev_* after drawing).  Rebuilding blindly would zero that
       baseline and every delta would read "no change".  The old values are therefore
       carried over BY NAME - by index would misalign the moment a region appears or
       disappears between frames. */
    srg_t old[8]; int nold=NSR;
    memcpy(old,SR,sizeof old);
    NSR=0;
    for(unsigned i=0;i<sizeof RGN/sizeof RGN[0] && NSR<8;i++){
        char k[96]; snprintf(k,sizeof k,"region.%s.mapped_bytes",RGN[i].slug);
        if(!rp_find(t,k,b,sizeof b)) continue;
        srg_t*x=&SR[NSR++];
        snprintf(x->name,sizeof x->name,"%s",RGN[i].name);
        x->mapped=strtod(b,NULL);
        x->prev_rss=0;
        for(int j=0;j<nold;j++)
            if(!strcmp(old[j].name,x->name)){ x->prev_rss=old[j].prev_rss; break; }
        snprintf(k,sizeof k,"region.%s.rss_bytes",RGN[i].slug);   x->rss=rp_num(t,k,0);
        snprintf(k,sizeof k,"region.%s.cfg_bytes",RGN[i].slug);   x->cfg=(long long)rp_num(t,k,-1);
        x->grade=RGN[i].grade;
    }
    g_dyn_rss   =rp_num(t,"region.dynamic_heap.rss_bytes",0);
    g_dyn_mapped=rp_num(t,"region.dynamic_heap.mapped_bytes",0);

    /* Buffer pool and capacity: the plot panels and verdicts read these. */
    PG.ok=(int)rp_num(t,"pgbuf.enabled",0);
    rp_str(t,"pgbuf.reason",PG.why,sizeof PG.why);     /* shown on screen beside the state */
    PG.nbuf=(int)rp_num(t,"pgbuf.num_buffers",0);
    PG.resident=(int)rp_num(t,"pgbuf.resident",0);
    PG.dirty=(int)rp_num(t,"pgbuf.dirty",0);
    PG.z1=(int)rp_num(t,"pgbuf.zone1_hot",0); PG.z2=(int)rp_num(t,"pgbuf.zone2_warm",0);
    PG.z3=(int)rp_num(t,"pgbuf.zone3_cold",0);
    PG.flushing=(int)rp_num(t,"pgbuf.flushing",0);
    PG.unflushed=(int)rp_num(t,"pgbuf.unflushed",0);
    PG.flush_only=(int)rp_num(t,"pgbuf.oldest_dirty_excludes_flushing",0);
    PG.zvoid=(int)rp_num(t,"pgbuf.zone_void",0);
    PG.mismatch=(int)rp_num(t,"pgbuf.hdr_mismatch",0);
    PG.unread=(int)rp_num(t,"pgbuf.hdr_unread",0);
    PG.ms=rp_num(t,"pgbuf.scan_ms",0);
    PG.scan_age_s=rp_num(t,"pgbuf.scan_age_s",0); PG.from_last=PG.scan_age_s>0; SM.miss_pps=rp_num(t,"pgbuf.sample_miss_per_s",-1);
    PG.zinv=(int)rp_num(t,"pgbuf.zone_free",0);   /* emitted as zone_free */
    PG.mismatch_raw=(int)rp_num(t,"pgbuf.hdr_mismatch_raw",0);
    PG.recheck=(int)rp_num(t,"pgbuf.rechecked",0);
    PG.degraded=(int)rp_num(t,"pgbuf.degraded",0);
    PG.nhdr=(int)rp_num(t,"pgbuf.hdr_read",0);
    PG.nonres=(int)rp_num(t,"pgbuf.hdr_nonresident",0);
    PG.pm_unchecked=(int)rp_num(t,"pgbuf.hdr_pagemap_unchecked",0);
    /* The LSA axis.  Both are emitted only when present, so presence is the flag; a
       frame without them must not inherit the previous frame's values. */
    PG.have_oldest=rp_find(t,"pgbuf.oldest_dirty_pageid",b,sizeof b);
    if(PG.have_oldest){
        PG.oldest_dirty.pageid=(int64_t)strtoll(b,NULL,10);
        PG.oldest_dirty.offset=(int)rp_num(t,"pgbuf.oldest_dirty_offset",0);
    }
    PG.have_log=rp_find(t,"log.append_pageid",b,sizeof b);
    if(PG.have_log){
        PG.log_append.pageid=(int64_t)strtoll(b,NULL,10);
        PG.log_append.offset=(int)rp_num(t,"log.append_offset",0);
        PG.log_nxio.pageid=(int64_t)rp_num(t,"log.nxio_pageid",0);
        PG.log_nxio.offset=(int)rp_num(t,"log.nxio_offset",0);
        PG.log_eof.pageid=(int64_t)rp_num(t,"log.eof_pageid",0);
        PG.log_eof.offset=(int)rp_num(t,"log.eof_offset",0);
    }
    /* Engine aggregates */
    memset(&AG,0,sizeof AG);
    AG.ok=(int)rp_num(t,"pgbuf.engine.enabled",0);
    if(!AG.ok) rp_str(t,"pgbuf.engine.reason",AG.why,sizeof AG.why);
    else {
        AG.nbuf=(int)rp_num(t,"pgbuf.engine.num_buffers",0);
        AG.nlist=(int)rp_num(t,"pgbuf.engine.lists",0); AG.nshared=(int)rp_num(t,"pgbuf.engine.lists_shared",0);
        AG.z1=(long)rp_num(t,"pgbuf.engine.zone1",0); AG.z2=(long)rp_num(t,"pgbuf.engine.zone2",0);
        AG.z3=(long)rp_num(t,"pgbuf.engine.zone3",0); AG.vict=(long)rp_num(t,"pgbuf.engine.victim_candidates",0);
        AG.dirty=(long long)rp_num(t,"pgbuf.engine.dirty",0); AG.ms=rp_num(t,"pgbuf.engine.read_ms",0);
    }
    /* Per-volume residency: only volumes that had pages are emitted, so the array is
       cleared first and filled from whichever keys exist. */
    memset(PG.vol_res,0,sizeof PG.vol_res); memset(PG.vol_dirty,0,sizeof PG.vol_dirty);
    for(int v=0;v<PG_MAXVOL;v++){
        char k[48]; snprintf(k,sizeof k,"pgbuf.vol%d.resident",v);
        if(!rp_find(t,k,b,sizeof b)) continue;
        PG.vol_res[v]=(int)strtod(b,NULL);
        snprintf(k,sizeof k,"pgbuf.vol%d.dirty",v); PG.vol_dirty[v]=(int)rp_num(t,k,0);
    }
    B.ok=(int)rp_num(t,"heapB.enabled",0);
    rp_str(t,"heapB.reason",B.why,sizeof B.why);
    rp_str(t,"heapB.table",g_rp_tbl,sizeof g_rp_tbl);
    rp_str(t,"heapB.table_version",g_rp_tblver,sizeof g_rp_tblver);
    B.partial=!strcmp(g_rp_tbl,"probe-runtime");
    rp_str(t,"heapB.lib",g_so_path,sizeof g_so_path); if(!strcmp(g_so_path,"-")) g_so_path[0]=0;
    B.total=rp_num(t,"heapB.total_bytes",0);
    B.n_bad=(int)rp_num(t,"heapB.excluded",0);
    /* Heap detail B items.  The keys are slugified screen labels, so the map back is
       explicit; without it the box draws an empty table and "B total 0B". */
    { static const struct { const char *slug, *name; } BIT[]={
        {"plan_cache_xcache","plan cache (xcache)"},     /* the order method_b adds them */
        {"plan_clones_est","plan clones (est)"},
        {"result_cache_query","result cache (query)"},
        {"result_cache_hash_est","result cache hash (est)"},
        {"result_temp_pages","result temp pages"},
        {"filter_pred_cache","filter pred cache"},
        {"lock_table_tran","lock table (tran)"},
        {"lock_resources_obj","lock resources (obj)"},
        {"lock_entries_obj","lock entries (obj)"},
        {"session_table","session table"},
        {"catalog_cache","catalog cache"},
        {"conn_entries_css","conn entries (css)"},
      };
      B.n=0;
      for(unsigned i=0;i<sizeof BIT/sizeof BIT[0] && B.n<16;i++){
          char k[80]; snprintf(k,sizeof k,"heapB.%s_bytes",BIT[i].slug);
          int cnt_only=0;
          if(!rp_find(t,k,b,sizeof b)){
              snprintf(k,sizeof k,"heapB.%s_count",BIT[i].slug);
              if(!rp_find(t,k,b,sizeof b)) continue;
              cnt_only=1;
          }
          bitem_t*x=&B.it[B.n++];
          memset(x,0,sizeof *x);
          snprintf(x->name,sizeof x->name,"%s",BIT[i].name);
          if(cnt_only){ x->count=(long)strtod(b,NULL); x->count_only=1; x->grade='?'; }
          else        { x->bytes=strtod(b,NULL);
                        char g[8]; snprintf(k,sizeof k,"heapB.%s_grade",BIT[i].slug);
                        x->grade = rp_find(t,k,g,sizeof g) && g[0] ? g[0] : 'M'; }
      } }
    /* Capacity axis.  Without this the verdict is recomputed from zeros and states the
       OPPOSITE of the recording ("no headroom problem" for a frame that was at its
       ceiling), so every field the verdict reads is restored. */
    memset(&CAP2,0,sizeof CAP2);
    CAP2.read_bps =rp_num(t,"io.read_bps",0);   CAP2.write_bps=rp_num(t,"io.write_bps",0);
    CAP2.riops    =rp_num(t,"io.read_iops",0);  CAP2.wiops    =rp_num(t,"io.write_iops",0);
    CAP2.dev_iops =rp_num(t,"cap.dev_iops",0);
    /* The DB disk's own utilization.  hostdev.util_pct is the host's busiest device and
       must not stand in for it: a recording without cap.dev_util has no DB-disk figure. */
    CAP2.dev_util =rp_num(t,"cap.dev_util",-1);
    /* A key missing from the frame was not measured: -1 as in live, never 0 */
    CAP2.await_ms =rp_num(t,"cap.await_ms",-1);
    CAP2.r_await_ms=rp_num(t,"cap.read_await_ms",-1);
    CAP2.w_await_ms=rp_num(t,"cap.write_await_ms",-1);
    CAP2.qdepth   =rp_num(t,"cap.queue_depth",-1);
    CAP2.miss_pps =rp_num(t,"cap.miss_pages_per_s",-1);
    if(!rp_find(t,"cap.miss_source",CAP2.miss_src,sizeof CAP2.miss_src))   /* older recordings: scanned if measured */
        snprintf(CAP2.miss_src,sizeof CAP2.miss_src,"%s",CAP2.miss_pps>=0?"scan":"-");
    CAP2.hit_pct  =rp_num(t,"cap.buffer_hit_pct",-1);
    CAP2.turnover_pct=rp_num(t,"cap.turnover_pct",-1);
    CAP2.readmit_pct =rp_num(t,"cap.readmit_pct",-1);
    CAP2.adm_frac    =rp_num(t,"cap.admit_frac_of_buffer",-1);
    CAP2.ceil_iops=rp_num(t,"cap.ceiling_iops",0);
    CAP2.ceil_bps =rp_num(t,"cap.ceiling_bps",0);
    CAP2.ceil_util=rp_num(t,"cap.ceiling_util_pct",0);
    CAP2.ceil_qd  =rp_num(t,"cap.ceiling_qdepth",0);
    CAP2.ceil_when=(time_t)rp_num(t,"cap.ceiling_epoch",0);
    CAP2.ceil_valid=CAP2.ceil_iops>0;
    CAP2.headroom_pct=rp_num(t,"cap.used_of_ceiling_pct",-1);
    CAP2.req_kb   =rp_num(t,"cap.req_kb",0);
    CAP2.read_ratio=rp_num(t,"cap.read_ratio",0);
    /* The profile cell is derived from the restored figures, not guessed - otherwise the
       verdict names the wrong cell (">64KB" recorded, "<8KB" replayed). */
    CAP2.prof_bs=cap_bs_idx(CAP2.req_kb); CAP2.prof_rw=cap_rw_idx(CAP2.read_ratio);
    /* The recorded cell wins: read_ratio is printed to 0.01, so 0.299 (write-heavy) replays as 0.30 (mixed) */
    { char pf[64];
      if(rp_find(t,"cap.profile",pf,sizeof pf)){
          char *sl=strchr(pf,'/');
          if(sl){ *sl=0;
              for(int b=0;b<CAP_BS_N;b++) if(!strcmp(pf,CAP_BS_NAME[b])) CAP2.prof_bs=b;
              for(int r=0;r<CAP_RW_N;r++) if(!strcmp(sl+1,CAP_RW_NAME[r])) CAP2.prof_rw=r; } } }
    CAP2.ps_ok    =(int)rp_num(t,"cap.perfmon_counters_running",0);
    CAP2.pb_fetch_ps  =rp_num(t,"cap.pb_fetch_per_s",0);   CAP2.pb_ioread_ps=rp_num(t,"cap.pb_ioread_per_s",0);
    CAP2.pb_iowrite_ps=rp_num(t,"cap.pb_iowrite_per_s",0); CAP2.log_iowrite_ps=rp_num(t,"cap.log_iowrite_per_s",0);
    CAP2.commit_ps    =rp_num(t,"cap.commit_per_s",0);
    CAP2.ms       =rp_num(t,"cap.aggregate_ms",0);
    CAP2.over_budget=(int)rp_num(t,"cap.over_budget",0);
    rp_str(t,"cap.hit_source",CAP2.hit_src,sizeof CAP2.hit_src);
    rp_str(t,"cap.reason",CAP2.ps_why,sizeof CAP2.ps_why);
    rp_str(t,"cap.verdict",g_rp_capvd,sizeof g_rp_capvd);
    /* The remembered ceilings are per profile cell, so the whole matrix is restored -
       a verdict compares against the cell in force, not a single number. */
    /* The cells are the recorded device's, restored into that device's matrix */
    rp_str(t,"cap.device",CAP2.dev,sizeof CAP2.dev);
    rp_str(t,"cap.device_reason",CAP2.dev_why,sizeof CAP2.dev_why);
    { char v[16]=""; rp_str(t,"cap.device_via",v,sizeof v);
      g_dk_via = !strcmp(v,"st_dev")?1 : !strcmp(v,"mount")?2 : !strcmp(v,"statfs_fsid")?3 : 0; }
    /* A recording made before cap.dev_util was written names the DB disk without its
       utilization: that is "DB disk unknown" for every judgement, not 0% and not the host's */
    if(CAP2.dev[0] && CAP2.dev_util<0){ CAP2.dev[0]=0;
        snprintf(CAP2.dev_why,sizeof CAP2.dev_why,"%s",g_ascii?"recording has no cap.dev_util":"기록에 cap.dev_util 없음"); }
    rp_str(t,"cap.ceiling_device_id",g_cap_devid,sizeof g_cap_devid);
    { cap_dev_t *d=cap_dev_get(CAP2.dev); g_cap_mtx = d ? d->m : g_cap_none; }
    if(g_cap_mtx!=g_cap_none) memset(g_cap_mtx,0,sizeof(cap_cell_t)*CAP_BS_N*CAP_RW_N);
    for(int bs=0;bs<CAP_BS_N && g_cap_mtx!=g_cap_none;bs++) for(int rw=0;rw<CAP_RW_N;rw++){
        char k[64]; snprintf(k,sizeof k,"cap.cell%d%d.iops",bs,rw);
        if(!rp_find(t,k,b,sizeof b)) continue;
        cap_cell_t*cc=&g_cap_mtx[bs][rw];
        cc->iops=strtod(b,NULL); cc->valid=1;
        snprintf(k,sizeof k,"cap.cell%d%d.util_pct",bs,rw);    cc->util=rp_num(t,k,0);
        snprintf(k,sizeof k,"cap.cell%d%d.await_ms",bs,rw);    cc->await_ms=rp_num(t,k,0);
        snprintf(k,sizeof k,"cap.cell%d%d.queue_depth",bs,rw); cc->qd=rp_num(t,k,0);
        snprintf(k,sizeof k,"cap.cell%d%d.epoch",bs,rw);       cc->when=(time_t)rp_num(t,k,0);
    }
    /* Version and instance identity belong to the recording, not to this host. */
    rp_str(t,"cubrid.version",g_cver,sizeof g_cver);
    if(!strcmp(g_cver,"unknown")) g_cver[0]=0;
    g_ninst=0;
    { inst_t *x0=inst_add();
      if(x0){ snprintf(x0->db,sizeof x0->db,"%s",P->db); x0->pid=P->pid;
              x0->rss=rp_num(t,"tier.server_rss_bytes",0);
              x0->pl_rss=rp_num(t,"tier.pl_rss_bytes",0); } }
    g_cur=0;
    /* The recording host's capabilities, or the replay would warn of fallbacks it never had */
    CAP.vm_readv=(int)rp_num(t,"caps.vm_readv",0);  CAP.rollup=(int)rp_num(t,"caps.rollup",0);
    CAP.proc_io=(int)rp_num(t,"caps.proc_io",0);
    CAP.mem_pread=(int)rp_num(t,"caps.mem_pread",1);
    cap_note();
    P->reg_dropped=(int)rp_num(t,"limits.regions_dropped",0);
    /* Run state of the recording host: permissions */
    g_perm_limited=(int)rp_num(t,"perm.limited",0);
    rp_str(t,"perm.reason",g_perm_why,sizeof g_perm_why);
    rp_str(t,"param.skipped_reason",g_pd_why,sizeof g_pd_why);
    { char ts[40]; struct tm tm; double ep=rp_num(t,"ts_epoch",-1); g_rep_tzok=0;
      rp_str(t,"ts",ts,sizeof ts); memset(&tm,0,sizeof tm);
      if(ep>0 && sscanf(ts,"%d-%d-%d %d:%d:%d",&tm.tm_year,&tm.tm_mon,&tm.tm_mday,&tm.tm_hour,&tm.tm_min,&tm.tm_sec)==6){
          tm.tm_year-=1900; tm.tm_mon-=1;
          g_rep_tzoff=(long)(timegm(&tm)-(time_t)ep); g_rep_tzok=1; } }   /* why this frame has no paramdump values */
    g_disk_dropped=(int)rp_num(t,"limits.disks_dropped",0);
    /* Parameters: the verdicts compare usage against these, so a replay without them
       would judge against nothing.  Only the rows terse emits are looked for. */
    g_nprm=0;
    { char src[32]=""; rp_str(t,"param.source",src,sizeof src); g_pd_applied=!strcmp(src,"paramdump"); }
    for(int i=0;i<NPRMROWS && g_nprm<MAXPARAM;i++){
        char k[96]; snprintf(k,sizeof k,"param.%s",PRMROWS[i].name);
        if(!rp_find(t,k,b,sizeof b)) continue;
        snprintf(g_prm[g_nprm].name,sizeof g_prm[g_nprm].name,"%s",PRMROWS[i].name);
        g_prm[g_nprm].val=strtoll(b,NULL,10); g_nprm++;
    }
    /* A frame with the server down carries cubrid.conf as conf.<name> instead, every
       parameter read, in their order */
    for(const char *q=strstr(t,"\"conf."); q && g_nprm<MAXPARAM; q=strstr(q+1,"\"conf.")){
        const char *nm=q+6, *e=strchr(nm,'"'); char k[96];
        if(!e || (size_t)(e-nm)>=sizeof g_prm[0].name) continue;
        snprintf(k,sizeof k,"conf.%.*s",(int)(e-nm),nm);
        if(!rp_find(t,k,b,sizeof b)) continue;
        int hit=-1; for(int j=0;j<g_nprm;j++) if(!strncmp(g_prm[j].name,nm,(size_t)(e-nm)) && !g_prm[j].name[e-nm]){ hit=j; break; }
        if(hit<0){ hit=g_nprm++; snprintf(g_prm[hit].name,sizeof g_prm[hit].name,"%.*s",(int)(e-nm),nm); }
        g_prm[hit].val=strtoll(b,NULL,10);
    }
    /* Instances: rebuilt from every "instance.<key>.pid".  The key becomes the name, so
       re-emitting the frame produces the same keys. */
    g_ninst=0; g_cur=0;
    for(const char *q=t; (q=strstr(q,"\"instance."))!=NULL; q++){
        const char *ks=q+10, *ke=strstr(ks,".pid\":");
        const char *qe=strchr(ks,'"');
        if(!ke || !qe || ke>qe) continue;            /* not a .pid key */
        inst_t *x=inst_add(); if(!x) break;
        snprintf(x->db,sizeof x->db,"%.*s",(int)(ke-ks),ks);
        char k[128];
        snprintf(k,sizeof k,"instance.%s.pid",x->db);          x->pid=(int)rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.rss_bytes",x->db);    x->rss=rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.file_bytes",x->db);   x->filemap=rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.swap_bytes",x->db);   x->swap=rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.uid",x->db);          x->uid=(int)rp_num(t,k,-1);
        snprintf(k,sizeof k,"instance.%s.pl_rss_bytes",x->db); x->pl_rss=rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.pl_count",x->db);     x->pl_n=(int)rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.tracked",x->db);      if(rp_num(t,k,0)>0) g_cur=g_ninst-1;
    }
}

/* Replay loop.  Reads the recording, rebuilds each frame with rp_apply() and calls
   the same renderers the live loop calls, so nothing about the drawing differs.
   Keys are the live ones; [space] pauses, [,] and [.] step while paused. */
static int rp_run(const char *path,int view0,int terse,int jsonout){
    FILE *f=fopen(path,"re");
    if(!f){ fprintf(stderr,"cub_top: --replay %s: %s\n",path,strerror(errno)); return 1; }
    char *line=malloc(RP_MAXLINE);
    if(!line){ fclose(f); return 1; }
    /* The whole recording is held so [,] can step backwards. */
    size_t cap=256,n=0; char **fr=malloc(cap*sizeof *fr);
    if(!fr){ free(line); fclose(f); return 1; }
    /* A frame is taken only whole: '{' through '}' and its newline.  A line cut by SIGKILL,
       a full disk or power loss would otherwise replay its missing keys as 0 (plots drop
       to 0, verdicts change, a cut before ts gives 1970).  Cut lines are skipped and named. */
    long ln=0, nbad=0, badl[4]={0};
    while(fgets(line,RP_MAXLINE,f)){
        size_t l=strlen(line); int whole=l>0 && line[l-1]=='\n';
        ln++;
        if(!whole && !feof(f)){                     /* longer than RP_MAXLINE: drop all of it */
            int c; while((c=fgetc(f))!=EOF && c!='\n'); }
        while(l>0 && (line[l-1]=='\n'||line[l-1]=='\r'||line[l-1]==' '||line[l-1]=='\t')) line[--l]=0;
        if(l==0) continue;
        if(line[0]!='{' || line[l-1]!='}' || !whole){ if(nbad<4) badl[nbad]=ln; nbad++; continue; }
        if(n==cap){ cap*=2; char **t=realloc(fr,cap*sizeof *fr); if(!t) break; fr=t; }
        fr[n++]=strdup(line);
    }
    fclose(f); free(line);
    if(nbad){
        fprintf(stderr,g_ascii?"cub_top: --replay %s: skipped %ld incomplete line(s) (line":"cub_top: --replay %s: 불완전한 줄 %ld개 건너뜀 (줄",path,nbad);
        for(long k=0;k<nbad && k<4;k++) fprintf(stderr," %ld",badl[k]);
        fprintf(stderr,"%s)\n",nbad>4?" ...":"");
    }
    if(!n){ fprintf(stderr,"cub_top: --replay %s: no frames\n",path); free(fr); return 1; }
    /* Only recordings of this build are replayed.  A frame without the tier memory keys
       (an older build wrote PSS under *_pss_bytes) would replay master, broker, CAS and
       PL as 0 and shrink the totals: refused, with the line, instead of shown wrong. */
    for(size_t i=0;i<n;i++)
        if(!strstr(fr[i],"\"tier.master_rss_bytes\"")){
            int old=strstr(fr[i],"\"tier.master_pss_bytes\"")!=NULL;
            fprintf(stderr,g_ascii?"cub_top: --replay %s: frame %zu has no tier.*_rss_bytes%s - not a recording of this build;"
                                   " its tier memory would replay as 0, so it is not replayed\n"
                                  :"cub_top: --replay %s: 프레임 %zu 에 tier.*_rss_bytes 가 없음%s — 이 빌드가 만든 녹화가 아님."
                                   " 계층 메모리가 0으로 재생되므로 재생하지 않음\n",
                    path,i+1,old?(g_ascii?" (older build: *_pss_bytes)":" (이전 빌드의 *_pss_bytes)"):"");
            for(size_t k=0;k<n;k++) free(fr[k]);
            free(fr); return 1; }

    static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
    /* -t / --json on a replay re-emit each frame instead of drawing it, so a recording
       can be post-processed or compared without a terminal. */
    if(terse||jsonout){
        for(size_t j=0;j<n;j++){
            char ts[32]; time_t rt=(time_t)rp_num(fr[j],"ts_epoch",0); struct tm tmv;
            rp_apply(fr[j],&P,&M,&T,&IO);   /* sets the recorder's UTC offset */
            rep_localtime(rt,&tmv); strftime(ts,sizeof ts,"%Y-%m-%d %H:%M:%S",&tmv);
            g_replaying=1; g_ts_frac=rp_time(fr[j]);
            if(jsonout){
                FILE *tf=mem_file(); if(!tf) continue;
                fflush(stdout); int sv=dup(1);
                if(sv<0||dup2(fileno(tf),1)<0){ if(sv>=0) close(sv); fclose(tf); continue; }
                render_terse(&P,&M,&T,IO.rb,IO.wb,IO.ri,IO.wi,
                             IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,
                             ts,(long)rt,IO.rchar,IO.minflt,IO.majflt,IO.cpu_pct,IO.ncpu,
                             IO.d_thr,IO.n_thr);
                fflush(stdout); dup2(sv,1); close(sv);
                json_line_from_terse(tf,stdout); fclose(tf);
            } else {
                render_terse(&P,&M,&T,IO.rb,IO.wb,IO.ri,IO.wi,
                             IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,
                             ts,(long)rt,IO.rchar,IO.minflt,IO.majflt,IO.cpu_pct,IO.ncpu,
                             IO.d_thr,IO.n_thr);
            }
        }
        for(size_t j=0;j<n;j++) free(fr[j]);
        free(fr);
        return 0;
    }
    int view=view0, paused=0, rebuild=0; size_t i=0;
    size_t pushed=(size_t)-1;      /* the last frame added to the history */
    double t0=rp_time(fr[0]);
    tui_enter();
    while(!g_stop && i<n){
        if(rebuild){
            /* Stepping back: the plots must show the window as it stood at frame i, so
               the history is rebuilt from the frames before it (at most one window). */
            HN=0;
            for(size_t j = i>(size_t)HCAP ? i-(size_t)HCAP : 0; j<i; j++){
                rp_apply(fr[j],&P,&M,&T,&IO);
                hist_push(rp_time(fr[j]),&P,&M,&T,&IO);
            }
            rebuild=0; pushed=(size_t)-1;
        }
        rp_apply(fr[i],&P,&M,&T,&IO);
        /* The history the plots draw is rebuilt frame by frame, so a replay shows the
           window growing exactly as it did live.  Only a new frame is added: while
           paused the loop redraws the same frame every 80ms, and pushing it each time
           would scroll the plots with copies (live mode guards the same way). */
        if(pushed!=i){ hist_push(rp_time(fr[i]),&P,&M,&T,&IO); pushed=i; }
        g_replaying=1; g_rep_i=(int)i; g_rep_n=(int)n;
        g_rep_ts=(time_t)rp_num(fr[i],"ts_epoch",0);
        if(view) render_plot(&P,&M,&T,&IO,paused);
        else     render_dash(&P,&M,&T,&IO,paused,0);
        int k, step=0;
        while((k=tui_key())>=0){
            if(k=='q'){ g_stop=1; break; }
            else if(k==' ') paused=!paused;
            else if(k=='p'||k=='g') view=!view;
            else if(k=='l') g_ascii=!g_ascii;
            else if(k>='1'&&k<='4'){ if(view) g_plot_zoom=(k=='1'||g_plot_zoom==k-'0')?0:k-'0'; }
            else if(k=='w'){ if(view) g_plot_win=(g_plot_win+1)%3; }
            else if(k=='.') step=1;                       /* step forward while paused */
            else if(k==','){ if(i>0){ i--; rebuild=1; step=-1; } }   /* step back: history is rebuilt */
        }
        if(g_stop) break;
        if(paused && !step){ usleep(80000); continue; }
        if(step<0){ /* already moved */ }
        else {
            double dt=0;
            if(i+1<n && g_rep_speed>0){
                dt=(rp_time(fr[i+1])-rp_time(fr[i]))/g_rep_speed;
                if(dt<0) dt=0;
                if(dt>5) dt=5;   /* a long gap in the recording is not replayed in real time */
            }
            i++;
            if(!paused && dt>0) usleep((useconds_t)(dt*1e6));
        }
    }
    tui_leave();
    (void)t0;
    fprintf(stderr,"\nreplay: %zu frames from %s\n",n,path);
    for(size_t j=0;j<n;j++) free(fr[j]);
    free(fr);
    return 0;
}

static void json_from_terse(FILE*tf,FILE*o){
    fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
    if(sz<=0){ fputs("{}\n",o); return; }
    char *b=malloc((size_t)sz+1); if(!b){ fputs("{}\n",o); return; }
    size_t rd=fread(b,1,(size_t)sz,tf); b[rd]=0;
    fputs("{\n",o); int first=1;
    char *p=b;
    while(*p){
        while(*p==' '||*p=='\n'||*p=='\t'||*p=='\r') p++;
        if(!*p) break;
        char *k=p; while(*p && *p!='=' && *p!=' ' && *p!='\n') p++;
        if(*p!='='){ continue; }               /* Tokens without '=' are discarded (there should be none) */
        size_t kl=(size_t)(p-k); p++;
        char *v=p; size_t vl; int quoted=0;
        if(*p=='"'){ quoted=1; v=++p; while(*p && *p!='"'){ if(*p=='\\' && p[1]) p++; p++; }
                     vl=(size_t)(p-v); if(*p=='"') p++; }
        else       { while(*p && *p!=' ' && *p!='\n' && *p!='\r') p++;
                     vl=(size_t)(p-v); }
        if(!first) fputs(",\n",o);
        first=0;
        fputs("  \"",o); json_escape_out(o,k,kl); fputs("\": ",o);
        int numeric = !quoted && json_number(v,vl);   /* else a bare token becomes a string */
        if(numeric){ fwrite(v,1,vl,o); }
        else if(quoted){ fputc('"',o); json_copy_escaped(o,v,vl); fputc('"',o); }   /* tq() already escaped it */
        else { fputc('"',o); json_escape_out(o,v,vl); fputc('"',o); }
    }
    fputs("\n}\n",o); free(b);
}


/* One frame as a single JSONL line.  --replay prints a frame per recorded sample;
   emitting the pretty form repeatedly yields concatenated objects that are neither
   a JSON document nor JSONL, so the newlines are folded the way rp_record does. */
static void json_line_from_terse(FILE*tf,FILE*o){
    FILE *m=mem_file();
    if(!m){ json_from_terse(tf,o); return; }
    json_from_terse(tf,m);
    fflush(m); fseek(m,0,SEEK_END); long n=ftell(m); fseek(m,0,SEEK_SET);
    char *b=(n>0)?malloc((size_t)n+1):NULL;
    if(b){ size_t rd=fread(b,1,(size_t)n,m); b[rd]=0;
           for(size_t k=0;k<rd;k++) if(b[k]=='\n') b[k]=' ';
           fprintf(o,"%s\n",b); free(b); }
    fclose(m);
}static int g_json=0;

/* ---- One sample (shared by one-shot and live) ----
 * Returns collection work time in ms, excluding waits.  Window:
 *   baseline -> interval -> smaps/rollup/io -> classify/method B
 */
/* ---- Page-cache residency of the database's files (mincore) ----
   CUBRID reads through the OS page cache, so whether a buffer miss becomes a disk read
   depends on how much of the volumes the page cache holds.  mincore on a read-only mapping
   of each volume answers that without touching a page (no fault, no read): about 7 ms per
   GB measured, so it runs on a period of 100x its own duration (1% of wall time, at least
   VC_MIN_S), as smaps does.  The files are the server's open <db>, <db>_* files through its
   fd table (one per inode), else those in the databases.txt directories.  Opening a volume
   needs the install account: another account's server reads as not available. */
#define VC_BUDGET 0.01
#define VC_MIN_S 10.0
static int vc_files(int pid,const char *db,char out[][PATH_MAX],int max){
    int n=0; size_t dl=strlen(db); dev_t dv[32]; ino_t in[32]; if(max>32) max=32;
    char p[64]; snprintf(p,sizeof p,"/proc/%d/fd",pid);
    DIR *d=opendir(p); struct dirent *e;
    while(d && (e=readdir(d)) && n<max){
        if(e->d_name[0]=='.') continue;
        char fp[400], lk[PATH_MAX]; snprintf(fp,sizeof fp,"%s/%s",p,e->d_name);
        ssize_t k=readlink(fp,lk,sizeof lk-1); if(k<=0) continue; lk[k]=0;
        if(lk[0]!='/') continue;
        const char *bn=strrchr(lk,'/')+1;
        if(strncmp(bn,db,dl) || (bn[dl] && bn[dl]!='_')) continue;
        struct stat st; if(stat(fp,&st)!=0 || !S_ISREG(st.st_mode)) continue;
        int dup=0; for(int i=0;i<n;i++) if(dv[i]==st.st_dev && in[i]==st.st_ino){ dup=1; break; }
        if(dup) continue;
        dv[n]=st.st_dev; in[n]=st.st_ino; snprintf(out[n++],PATH_MAX,"%s",fp);
    }
    if(d) closedir(d);
    if(n) return n;
    const char *dbs=getenv("CUBRID_DATABASES"); char tp[PATH_MAX], ln[4096]; FILE *f=NULL;
    if(dbs){ snprintf(tp,sizeof tp,"%.4000s/databases.txt",dbs); f=fopen(tp,"re"); }
    static char dirs[2][2048]; int nd=0;
    while(f && fgets(ln,sizeof ln,f)){ char nm[256], vp[2048], hs[256], lp[2048];
        int nf=sscanf(ln,"%255s %2047s %255s %2047s",nm,vp,hs,lp);
        if(ln[0]=='#' || nf<2 || strcmp(nm,db)) continue;
        snprintf(dirs[nd++],2048,"%s",vp); if(nf>=4 && strcmp(lp,vp)) snprintf(dirs[nd++],2048,"%s",lp); break; }
    if(f) fclose(f);
    for(int i=0;i<nd;i++){ DIR *dd=opendir(dirs[i]); if(!dd) continue;
        while((e=readdir(dd)) && n<max){
            if(strncmp(e->d_name,db,dl) || (e->d_name[dl] && e->d_name[dl]!='_')) continue;
            char fp[PATH_MAX+300]; snprintf(fp,sizeof fp,"%s/%s",dirs[i],e->d_name);
            struct stat st; if(stat(fp,&st)!=0 || !S_ISREG(st.st_mode)) continue;
            snprintf(out[n++],PATH_MAX,"%.*s",PATH_MAX-1,fp); }
        closedir(dd); }
    return n;
}
static void vol_cache_scan(proc_t *P,double interval_s){
    double now=now_ms();
    if(P->pid<0){ VC.total=VC.resident=0; VC.nfiles=0; VC.age_s=0; VC.pid=-1;
                  snprintf(VC.why,sizeof VC.why,"%s",g_ascii?"server not running":"서버 미기동"); return; }
    int due = VC.pid!=P->pid || strcmp(VC.db,P->db)!=0 || VC.at_ms<0;
    if(!due){ double period=VC.ms/VC_BUDGET;
              if(period<VC_MIN_S*1000.0) period=VC_MIN_S*1000.0;
              if(period<interval_s*1000.0) period=interval_s*1000.0;
              due = now-VC.at_ms>=period; }
    if(!due){ VC.age_s=(now-VC.at_ms)/1000.0; return; }
    static char paths[32][PATH_MAX];
    int n=vc_files(P->pid,P->db,paths,32);
    double tot=0, res=0; int nf=0, denied=0;
    static unsigned char *vec=NULL; static size_t vcap=0;
    for(int i=0;i<n;i++){
        int fd=open(paths[i],O_RDONLY|O_CLOEXEC); if(fd<0){ if(errno==EACCES||errno==EPERM) denied++; continue; }
        struct stat st; if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0){ close(fd); continue; }
        size_t len=(size_t)st.st_size, chunk=(size_t)1<<30; long rp=0;
        for(size_t off=0; off<len; off+=chunk){
            size_t l=len-off<chunk?len-off:chunk, np=(l+4095)/4096;
            if(np>vcap){ free(vec); vec=malloc(np); vcap=vec?np:0; if(!vec) break; }
            void *m=mmap(NULL,l,PROT_READ,MAP_SHARED,fd,(off_t)off); if(m==MAP_FAILED) break;
            if(mincore(m,l,vec)==0) for(size_t k=0;k<np;k++) rp+=vec[k]&1;
            munmap(m,l);
        }
        close(fd);
        tot+=(double)len; res+=(double)rp*4096.0; nf++;
    }
    VC.total=tot; VC.resident=res>tot?tot:res; VC.nfiles=nf; VC.pid=P->pid; snprintf(VC.db,sizeof VC.db,"%s",P->db);
    VC.ms=now_ms()-now; VC.at_ms=now_ms(); VC.age_s=0;
    if(nf==0) snprintf(VC.why,sizeof VC.why,"%s",denied?(g_ascii?"volumes not readable (another account)":"볼륨 파일 읽기 불가(다른 계정)")
                                                  :n?(g_ascii?"volumes not readable":"볼륨 파일 읽기 불가")
                                                    :(g_ascii?"no volume file found (fd table, databases.txt)":"볼륨 파일 찾지 못함(fd 표·databases.txt)"));
    else VC.why[0]=0;
}
/* A user stop (Ctrl-Z, pause, help) ends the collection segment: frame-to-frame state is
   dropped and the next walk of the tiers is only their CPU baseline */
static void seg_break(void){
    g_ustop_seen=g_ustop_n; g_seg_gen++;
    g_hprevn=0;                                   /* heap item deltas */
    g_tcpun=0;
    tier_t tb; memset(&tb,0,sizeof tb);
    g_enum_T=&tb; g_enum_dt=0; enum_instances(); g_enum_T=NULL;
}
static double sample_once(proc_t*P,proc_t*P0,mem_t*M,tier_t*T,iod_t*IO,
                          double interval,FILE**pd){
    double w0=now_ms();
    /* Stopped by the user since the last frame (unpause, help, a Ctrl-Z between frames) */
    if(g_ustop_n!=g_ustop_seen) seg_break();
again:
    P0->pid=P->pid;
    if(P->pid>=0){
        parse_status_stat(P0); parse_procio(P0);
    }
    dstat_t d0[MAXDISK]; int nd0=parse_diskstats(d0,MAXDISK);
    /* Baseline for host CPU: /proc/stat accumulates since boot, so two points are needed
       for an interval.  Taking the window's start here lets the call at the end produce
       the interval value, so CPU is non-zero even in one-shot mode. */
    { mem_t base; memset(&base,0,sizeof base); parse_hostcpu(&base); }
    /* blkio baseline.  It is a sum over all threads, so P0's stat cannot give it, and
       the threads are not walked twice per frame: the previous frame's closing scan
       is this frame's baseline.  Only a changed pid or the first frame measures
       afresh, so one-shot (-t/-1) also produces a value. */
    unsigned long blk0=0;
    { static unsigned long last_blk=0; static int last_pid=-1; static unsigned last_seg=0;
      if(P->pid==last_pid && last_seg==g_seg_gen && last_blk>0) blk0=last_blk;
      else { int dd,nn; count_d_threads(P->pid,&dd,&nn,&blk0); }
      last_pid=P->pid; last_seg=g_seg_gen; g_blk_baseline_slot=&last_blk; }
    double t0=now_ms();
    usleep((useconds_t)(interval*1000000));
    pd_reap_zombies();                  /* every frame, whether or not a request is open */
    if(*pd) paramdump_poll(pd,g_pd_async?0:PD_WAIT_MS);
    if(P->pid>=0){ proc_read_mem(P,interval,0); parse_status_stat(P); parse_procio(P); }
    dstat_t d1[MAXDISK]; int nd1=parse_diskstats(d1,MAXDISK);
    /* The window's closing thread scan, at the same instant as the reads above */
    int dthr=0,nthr=0; unsigned long blk1=0; count_d_threads(P->pid,&dthr,&nthr,&blk1);
    double dt=(now_ms()-t0)/1000.0; if(dt<0.001) dt=0.001;
    /* Stopped inside the window (Ctrl-Z): its two ends are not one data set, so the window is
       measured again from here */
    if(g_ustop_n!=g_ustop_seen){
        seg_break();
        if(g_tty_fd!=STDOUT_FILENO){ printf("\033[H\033[2K%s",g_ascii?"resumed: measuring a new window...":"재개: 새 측정 구간 수집 중..."); fflush(stdout); }
        goto again;
    }
    parse_meminfo(M); parse_cgroup(M,P->pid); parse_psi(M);
    parse_hostcpu(M);                  /* Host CPU and load; a delta, so called every frame */
    /* Refresh the instance summary every frame (one rollup per process, under 1ms).
       The current instance was already measured precisely this frame, so that value
       overwrites it and the OS and process boxes use the same instant - measuring
       separately would differ by whatever changed between the two reads. */
    memset(T,0,sizeof *T);
    g_enum_T=T; g_enum_dt=dt;          /* This single enum serves as both the instance summary and the tier aggregation */
    if(P->pid<0) g_walk_force=1;       /* waiting for a restart: look at all of /proc this frame */
    enum_instances();
    /* Detect a server that died or restarted under a new pid.  Without this, /proc
       reads on the dead pid fail silently and the previous values are redrawn, so
       the screen appears frozen (measured: rss stuck at 912.6M for 20+ frames
       across a restart).  A new pid under the same db name is adopted. */
    {
        /* The check must run even at pid<0 (not running), or a restart is never noticed and the
           instance stays down forever once it has died. */
        /* A restart is followed to the same account's database of that name (g_track_uid,
           set where the target was chosen), never to another user's that happens to share it */
        int alive=0;
        if(P->pid>=0)
            for(int i=0;i<g_ninst;i++) if(g_inst[i].pid==P->pid){
                /* Same pid, another start time: the server went away and the pid was reused */
                if(g_track_start && g_inst[i].start && g_inst[i].start!=g_track_start) break;
                alive=1; g_track_uid=g_inst[i].uid; if(!g_track_start) g_track_start=g_inst[i].start; break; }
        if(!alive){
            /* Keep the db name: clearing it with memset loses the name while waiting for a restart,
               so the server can never be picked up again when it returns. */
            char keep[64]; snprintf(keep,sizeof keep,"%s",P->db[0]?P->db:g_cur_db);
            int newpid=-1;
            if(keep[0]) newpid=restart_pick(keep,g_track_uid);
            /* Started with no server and no db named: take the first, as at startup */
            if(newpid<0 && (!keep[0] || !strcmp(keep,"-")) && g_ninst>0){
                newpid=g_inst[0].pid; snprintf(keep,sizeof keep,"%s",g_inst[0].db); }
            /* Move to the new pid on a restart, or to not-running if it has not come up.
               Either way the previous instance's analysis must be discarded, or a dead
               server's region and heap values appear to belong to the new one.  Still
               down (no pid before or after): nothing to discard. */
            if(P->pid>=0 || newpid>=0){
                reset_instance_state();
                memset(P,0,sizeof *P);
                memset(P0,0,sizeof *P0);
                snprintf(P->db,sizeof P->db,"%s",keep);
                P->pid=newpid; if(newpid<0) g_track_start=0;
                if(newpid>=0){
                    for(int i=0;i<g_ninst;i++)
                        if(g_inst[i].pid==newpid){ g_cur=i; g_track_uid=g_inst[i].uid; g_track_start=g_inst[i].start; break; }
                    snprintf(g_cur_db,sizeof g_cur_db,"%s",keep);
                    cap_probe(newpid);             /* The new process: re-probe rather than inherit the old flags */
                    proc_read_mem(P,interval,1); parse_status_stat(P); parse_procio(P);
                    /* Heap A/B analyse a process address space and must be redone after a restart.
                       B runs every frame below, but A runs periodically, so without this a
                       dead server's measurement would linger - force an immediate remeasure. */
                }
                /* The delta baseline moves to the new process too, set to what was just read:
                   a zeroed baseline would count everything since the server started as this
                   frame's CPU, faults and I/O.  Rates start on the next frame. */
                if(newpid>=0){ *P0=*P; count_d_threads(newpid,&dthr,&nthr,&blk0); }
                blk1=blk0;
                P0->pid=P->pid;
                /* Configuration is per instance too: cubrid.conf now so this frame has it, then
                   the new server's applied values (harvested next frame) */
                params_preload(newpid,keep);
                paramdump_abort(pd);
                if(newpid>=0) *pd=paramdump_start(newpid,keep);
            }
        }
    }
    if(P->pid>=0){
        for(int i=0;i<g_ninst;i++)
            if(g_inst[i].pid==P->pid){
                g_inst[i].rss=P->rss_kb*1024.0;
                g_cur=i;                       /* Keep the selection when the list changes */
                break;
            }
    }
    scan_tiers(T,dt);               /* Already filled by enum; the wrapper skips the second walk */
    g_enum_T=NULL;
    if(P->pid>=0){
        classify_regions(P);
        method_b(P);
        pgbuf_agg(P);
        /* Only a --bcb-dump baseline read this frame may carry turnover into cap_collect */
        CAP2.turnover_pct=-1; CAP2.miss_pps=-1; CAP2.readmit_pct=-1;
        pgbuf_scan(P);
        grade_dynamic_heap();          /* Re-grade in light of the decomposition */
    } else { NSR=0; memset(&B,0,sizeof B); snprintf(B.why,sizeof B.why,g_ascii?"cub_server not running":"cub_server 미기동");
             memset(&AG,0,sizeof AG); snprintf(AG.why,sizeof AG.why,"%s",B.why);
             PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"cub_server not running":"cub_server 미기동"); }
    memset(IO,0,sizeof *IO);
    IO->rb=UDELTA(P->read_b,P0->read_b)/dt; IO->wb=UDELTA(P->write_b,P0->write_b)/dt;
    double drchar=UDELTA(P->rchar,P0->rchar);
    IO->rchar=drchar/dt;
    IO->ri=UDELTA(P->syscr,P0->syscr)/dt;
    IO->wi=UDELTA(P->syscw,P0->syscw)/dt;
    IO->absorb = drchar>0 ? 1.0-((IO->rb*dt)/drchar>1.0?1.0:(IO->rb*dt)/drchar) : -1.0;
    /* Below the rchar floor the ratio is meaningless, so it counts as idle */
    IO->absorb_ok = (IO->absorb>=0 && IO->rchar>=RCHAR_MIN);
    long hz=sysconf(_SC_CLK_TCK); if(hz<=0) hz=100;
    /* blkio wait, as the delta of the all-thread sum.  The main thread alone always
       reads 0 because the workers do the I/O.  Being a sum it can exceed one core
       (100%), the same convention as CPU usage, so it reads as "total time this
       server waited on disk".  The first frame has no baseline and is 0. */
    /* Thread scan at the end of the window; blkpct uses this, so it must come before that calculation. */
    IO->d_thr=dthr; IO->n_thr=nthr; IO->blk_ticks=blk1;
    if(g_blk_baseline_slot) *g_blk_baseline_slot=IO->blk_ticks;   /* Baseline for the next frame */
    IO->blkpct = (IO->blk_ticks>=blk0)
               ? ((double)(IO->blk_ticks-blk0)/hz)/dt*100.0 : 0.0;
    IO->minflt=UDELTA(P->minflt,P0->minflt)/dt;
    IO->majflt=UDELTA(P->majflt,P0->majflt)/dt;
    /* CPU: the utime+stime delta over the window length.  One core is 100%, so a multicore
       process can exceed 100% - the ceiling (cores x 100) is shown alongside. */
    { unsigned long c1=P->utime+P->stime, c0=P0->utime+P0->stime;
      IO->cpu_pct = (c1>=c0) ? ((double)(c1-c0)/hz)/dt*100.0 : 0.0;
      long nc=sysconf(_SC_NPROCESSORS_ONLN); IO->ncpu = (nc>0)?(int)nc:1; }
    for(int i=0;i<nd1;i++){
        for(int j=0;j<nd0;j++) if(!strcmp(d1[i].name,d0[j].name)){
            double u=UDELTA(d1[i].io_ms,d0[j].io_ms)/(dt*1000.0)*100.0;   /* a device reset rewinds its counters */
            if(u>100) u=100;
            if(IO->devnm[0]==0 || u>IO->devutil){
                IO->devutil=u;
                snprintf(IO->devnm,sizeof IO->devnm,"%.31s",d1[i].name);
                IO->devr=UDELTA(d1[i].rsect,d0[j].rsect)*512.0/dt;
            }
            break;
        }
    }
    /* A stop that landed after the window's reads but before here - during the tier walk
       or the scans - would still put the stopped time into the tier CPU deltas, which are
       taken in the walk: the frame is measured again from the resume */
    if(g_ustop_n!=g_ustop_seen){ seg_break(); goto again; }
    /* ---- Capacity axis collection ----
       Collection for interpretation must not affect operation.  It measures under
       1ms, but slow storage or a huge buffer could stretch it, so a 0.5s budget
       applies: over it, the frame's collection is dropped and the reason shown. */
    if(P->pid>=0){
        double c0=now_ms();
        cap_collect(P,IO,d0,nd0,d1,nd1,dt);
        CAP2.ms=now_ms()-c0;
        if(CAP2.ms>CAP_BUDGET_MS){
            CAP2.over_budget=1;
            snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                     g_ascii?"skipped: aggregation took %.0fms > %.0fms budget"
                            :"취합 생략: %.0fms 소요 > 예산 %.0fms",CAP2.ms,(double)CAP_BUDGET_MS);
            cap_reset_values();          /* Half values are not published as metrics */
        } else CAP2.over_budget=0;
    } else { memset(&CAP2,0,sizeof CAP2); cap_reset_values();
             snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"server down":"cub_server 미기동"); }
    /* On its own period and outside the capacity budget: a 100 GB database is 0.7 s of
       mincore every 70 s, which is this frame's work, not the aggregation's */
    vol_cache_scan(P,interval);
    return now_ms()-w0-interval*1000.0;
}

/* ---------------- Option specification: the single source for the banner and help ---------------- */
typedef struct { const char*key,*meta,*desc; } opt_t;
/* ---- Detect whether Hangul can be displayed ----
   A server process cannot know the user's font, so it uses what it can know.
   If the locale codeset is not UTF-8, Hangul bytes are certain to break -> English.
   On a UTF-8 interactive tty, print one Hangul character and ask for the cursor
   position (CPR): if it advanced by one cell the terminal treats Hangul as
   half-width and every column would shift -> English.  No reply within 200ms
   leaves Hangul in place.  --ascii / --ko always win. */
static int tty_hangul_wide(void){
    struct termios o,r;
    if(tcgetattr(STDIN_FILENO,&o)) return -1;
    r=o; r.c_lflag&=~(unsigned)(ICANON|ECHO); r.c_cc[VMIN]=0; r.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSANOW,&r)) return -1;
    /* At the start of a row, print one Hangul character and query the cursor: col 3 in the ESC[row;colR reply means width 2 */
    if(write(STDOUT_FILENO,"\r\xEA\xB0\x80\x1b[6n",9)<0){ tcsetattr(STDIN_FILENO,TCSANOW,&o); return -1; }
    char b[32]; int bn=0;
    struct pollfd pf={STDIN_FILENO,POLLIN,0};
    while(bn<(int)sizeof b-1 && poll(&pf,1,200)>0){
        ssize_t k=read(STDIN_FILENO,b+bn,1);
        if(k<=0) break;
        bn+=(int)k;
        if(b[bn-1]=='R') break;
    }
    b[bn]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&o);
    { ssize_t k=write(STDOUT_FILENO,"\r\x1b[K",4); (void)k; }   /* Erase the test character */
    int row=0,col=0;
    if(sscanf(b,"\x1b[%d;%dR",&row,&col)==2 && col>0) return col>=3?1:0;
    return -1;
}
static void auto_lang(int force_ko){
    if(g_ascii||force_ko) return;                 /* An explicit setting wins */
    setlocale(LC_CTYPE,"");
    const char *cs=nl_langinfo(CODESET);
    if(!cs || !strstr(cs,"UTF")){ g_ascii=1; return; }
    if(isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO))
        if(tty_hangul_wide()==0) g_ascii=1;
}

static const opt_t OPTS[]={
 {"[db명]",      "", "볼 데이터베이스 (생략 시 알파벳순 첫 번째, 라이브에서 < > 로 이동)"},
 {"-t, --tty",   "", "드릴다운 트리 (터미널용, 1회 출력)"},
 {"-d, --dump",  "", "key=value 덤프 (모니터링 연동·백데이터)"},
 {"--dump-json", "", "-d 와 같은 내용을 JSON 오브젝트로 (키·값·순서 동일)"},
 {"-b, -i, --live","", "btop형 대시보드 (라이브)"},
 {"--ascii",     "", "라벨을 영문으로 강제 (자동: 비 UTF-8 로케일·한글 폭 미지원 tty 는 영문)"},
 {"--ko",        "", "한글 라벨 강제 (자동 판정이 영문으로 잘못 내렸을 때)"},
 {"--offsets",   "F","오프셋 표 파일/디렉터리 (tools/collect-offsets.sh 산출물 — 디렉터리면 버전 자동 선택; 생략 시 실행파일 옆 offsets/ 자동 탐색)"},
 {"--pgbuf-detail","","버퍼 슬롯 26만 개(16K 페이지 4GB) 초과 버퍼풀도 페이지 단위로 스캔 (기본은 엔진 집계만 — 전수 스캔은 대상에 접촉이 크다)"},
 {"--bcb-dump",  "F","버퍼풀 BCB 스냅샷을 파일 F 로 (cub_volmap --bufmap F 가 지도에 얹음; 라이브는 매 프레임 갱신, 이때는 헤더를 전수 읽음)"},
 {"-p, --plot",  "", "시계열 플롯 (라이브, p 키로 대시보드와 상호 전환)"},
 {"--interval",  "S","샘플 창(초, 0.05~3600, 기본 0.5)"},
 {"--record", "F", "라이브(-b/-p) 프레임을 새 파일 F 로 기록 (JSONL — -d/--dump-json 과 같은 키, --replay 와 병용 불가). 이미 있거나 심볼릭 링크면 거부"},
 {"--stream", "", "화면 없이 --interval 주기로 프레임을 JSONL 로 표준 출력에 계속 내보냄 (수집기 연동; INT/TERM 또는 파이프 종료로 끝남)"},
 {"--record-overwrite","","--record 의 기존 파일을 덮어씀 (이 계정 소유의 일반 파일일 때만)"},
 {"--replay", "F", "기록 파일 F 를 재생 (서버·/proc 불필요, -b/-p 키 그대로)"},
 {"--replay-speed", "N", "재생 배속 (1=실측 간격, 0=최대속도, 기본 1)"},
 {"--dump-hist", "", "히스토리 1샘플을 stderr 로 덤프 (수치 대조·진단용)"},
 {"-h, --help, /h","","이 도움말"},
};
#define NOPTS ((int)(sizeof OPTS/sizeof OPTS[0]))
static int opt_known(const char*s){
    for(int i=0;i<NOPTS;i++){
        const char*k=OPTS[i].key; char buf[64]; snprintf(buf,sizeof buf,"%s",k);
        char*tok=strtok(buf,",");
        while(tok){
            while(*tok==' ')tok++;
            if(*tok=='-' && !strcmp(tok,s)) return 1;
            tok=strtok(NULL,",");
        }
    }
    return 0;
}
/* Join the recognized options with spaces, shared by terse and the banner */
static void opts_used_str(int argc,char**argv,char*out,size_t cap){
    out[0]=0;
    for(int i=1;i<argc;i++){
        if(!opt_known(argv[i])) continue;
        snprintf(out+strlen(out),cap-strlen(out),"%s%s",out[0]?" ":"",argv[i]);
        if(!strcmp(argv[i],"--interval") && i+1<argc)
            snprintf(out+strlen(out),cap-strlen(out)," %s",argv[i+1]);
    }
}
/* One line: which options ran and what else is available */
static void print_opts_line(int argc,char**argv){
    char used[256]={0};
    opts_used_str(argc,argv,used,sizeof used);
    /* Listing every option does not say what to do next (-h already has them), so this
       shows only the entry points a newcomer can run straight away. */
    printf(g_ascii?"opts %s   ·   live: cub_top -b · timeseries: -p"
                  " · machine output: -d · help: -h\n"
                  :"옵션 %s   ·   실시간 화면: cub_top -b · 시계열: -p"
                  " · 모니터링 연동: -d · 도움말: -h\n",
           used[0]?used:(g_ascii?"(default)":"(기본)"));
}

/* ────────────────── main ────────────────── */
/* Colour one help line so its structure reads first:
     "-- section --"  cyan bold
     "  key/option"   leading token bright, description plain
   Colour changes nothing, so piped output is the same text. */
static void help_line(const char *s){
    if(!s){ putchar('\n'); return; }
    if(strstr(s,"──")){ printf("\033[1;36m%s\033[0m\n",s); return; }
    /* When a line starts with a key or option, highlight just that part */
    if(s[0]==' '&&s[1]==' '&&(s[2]=='['||s[2]=='-'||(s[2]>='1'&&s[2]<='9'))){
        int i=2; while(s[i]&&s[i]!=' ') i++;
        while(s[i]==' '&&s[i+1]&&s[i+1]!=' ') i++;      /* A joined token such as "-o, --opt" */
        printf("  \033[1;37m%.*s\033[0m%s\n",i-2,s+2,s+i);
        return;
    }
    printf("%s\n",s);
}
/* Page several lines to the screen height (used by the live h screen).
   Returns 1 if the user leaves with q or ESC. */
/* Page breaks follow sections; cutting at a fixed line count leaves a section
   heading alone at the foot of a page.
     "\f" alone   an explicit break (not drawn)
     "-- title --" moves to the next page if fewer than 4 lines remain
     otherwise     break when the page is full
   The footer names the current section. */
static int help_pager(const char *const *ln,int n){
    int rows=term_h(); if(rows<10) rows=24;
    int body=rows-3;                       /* Body plus one blank, one footer and one of slack */
    static int ps[128]; int np=0;          /* Starting line of each page */
    { int used=0; ps[np++]=0;
      for(int i=0;i<n;i++){
          const char*s=ln[i];
          if(s && s[0]=='\f'){ if(used>0 && np<128){ ps[np++]=i+1; } used=0; continue; }
          int head = s && strstr(s,"──")!=NULL;
          /* A heading must start on the same page as its body: move on when few lines
             remain, and also check that at least some body fits after it.
             Otherwise a heading is left alone at the foot of one page and the page
             after it begins with no heading at all. */
          if(used>0 && (used>=body || (head && body-used<6))){ if(np<128) ps[np++]=i; used=0; }
          used++;
      } }
    int pg=0;
    for(;;){
        int a=ps[pg], b=pg+1<np?ps[pg+1]:n;
        printf("\033[H\033[2J");
        const char*title="";
        int drawn=0;
        for(int i=a;i<b;i++){
            if(ln[i] && ln[i][0]=='\f') continue;
            if(ln[i] && strstr(ln[i],"──") && !title[0]) title=ln[i];
            help_line(ln[i]); drawn++;
        }
        for(;drawn<body;drawn++) putchar('\n');   /* Fixed footer position */
        char tt[96]={0};
        if(title[0]){ const char*t=title;
                      while(*t==' ' || !strncmp(t,"─",3)) t+= (*t==' ')?1:3;   /* Strip the leading rule (3 UTF-8 bytes) */
                      snprintf(tt,sizeof tt,"%s",t);
                      char*e=strstr(tt,"──"); if(e) *e=0;
                      size_t L=strlen(tt); while(L&&tt[L-1]==' ') tt[--L]=0; }
        printf("\033[38;5;244m ── %d/%d%s%s ── \033[0m", pg+1, np, tt[0]?" · ":"", tt);
        printf("\033[38;5;250m  [space] 순환  [→/PgDn] 다음  [←/PgUp] 이전  [q] 닫기\033[0m\n");
        fflush(stdout);
        int k=-1;
        while(!g_stop && k<0){ k=tui_key(); if(k<0){ pd_idle_tick(); usleep(40000); } }
        if(g_stop) return 1;
        /* Same convention as volmap's help: only q closes (tui_key folds the Hangul jamo).
           space/enter rolls over (last page to first), right/down/PgDn advance,
           left/up/PgUp/b go back.  Other keys are ignored, so a stray key does not
           dismiss the help. */
        if(k=='q'||k=='Q') return 1;
        if(k==' '||k=='\r'||k=='\n'){ pg=(pg+1)%np; }
        else if(k=='n'||k=='j'||k==TUI_KEY_RIGHT||k==TUI_KEY_DOWN||k==TUI_KEY_PGDN){ if(pg+1<np) pg++; }
        else if(k=='b'||k=='p'||k=='k'||k==0x7f||k==0x08||k==TUI_KEY_LEFT||k==TUI_KEY_UP||k==TUI_KEY_PGUP){ if(pg>0) pg--; }
    }
}
/* Help line collector: the text hout() is given is printed for -h, or captured as an
   array for the live h pager, so each help text is written in one place only. */
#define HELPMAX 400   /* About 206 lines today and growing; an overflow leaves a warning below */
static char  g_hbuf[HELPMAX][1024];
static const char *g_hln[HELPMAX];
static int   g_hn=0, g_hcap=0;         /* hcap 1 means collection mode: nothing is printed */
/* Upper bound on one help block.  The last block arrives whole, so leave room -
   truncation here loses entire sections and can cut mid-UTF-8, leaving a broken
   character.  Overflow is reported rather than silently trimmed. */
#define HOUT_MAX 32768
static void hout(const char *fmt,...){
    char tmp[HOUT_MAX]; va_list ap; va_start(ap,fmt);   /* A large puts block arrives whole */
    int need=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(need>=(int)sizeof tmp)
        fprintf(stderr,"cub_top: 도움말 블록이 %d바이트로 잘렸다(HOUT_MAX=%d) — "
                       "HOUT_MAX 를 늘릴 것\n",need,HOUT_MAX);
    /* Several lines can arrive together; split them */
    char *p=tmp;
    for(;;){
        char *nl=strchr(p,'\n');
        if(nl) *nl=0;
        /* A line never exceeds 1023 bytes; longer ones are truncated, harmless in help text */
        if(g_hcap){ if(g_hn<HELPMAX){ snprintf(g_hbuf[g_hn],1024,"%.1023s",p);
                                      g_hln[g_hn]=g_hbuf[g_hn]; g_hn++; }
                    else {                    /* Report the overflow on screen.  g_hn never exceeds HELPMAX: g_hn++ once let it
                        reach HELPMAX+1, and the pager read g_hln[HELPMAX] out of
                        bounds and segfaulted. */
                        snprintf(g_hbuf[HELPMAX-1],1024,
                                 "%s","⚠ 도움말이 HELPMAX 에서 잘렸다 — 값을 늘릴 것");
                        g_hln[HELPMAX-1]=g_hbuf[HELPMAX-1]; g_hn=HELPMAX; } }
        else       { if(p[0]&&p[0]!='\f') printf("%s\n",p); else putchar('\n'); }   /* CLI: plain text; a page break becomes a blank line */
        if(!nl) break;
        p=nl+1;
        if(!*p) break;                  /* A trailing newline does not create a blank line */
    }
}
/* The CLI preamble (name, usage, options) is not needed in live h - it is already
   running.  Keeping it separate from the shared concepts section lets live go straight
   from mode keys to concepts. */
/* Name, usage and options: what -h prints, and the first page of the live h help */
static void usage_opts(void){
    hout("사용: cub_top <모드> [옵션] [db명]");
    for(int i=0;i<NOPTS;i++){
        /* Pad by display width: a Hangul key such as [db명] is wider than its byte count says */
        char kb[64]; snprintf(kb,sizeof kb,"%s%s%s",OPTS[i].key,OPTS[i].meta[0]?" ":"",OPTS[i].meta);
        int pad=18-disp_w(kb); if(pad<0) pad=0;
        hout("  %s%*s %s",kb,pad,"",OPTS[i].desc);
    }
    hout("");
}

/* ---- Per-mode key help ----
   Live h leads with the keys of the mode in view (dashboard or time series);
   pouring in both pushes the section actually wanted further down.
   Only the switch key (p) appears in both, so either can be reached. */
/* ---- Per-mode help ----
   Each mode's help is self-contained: no description of another mode's screen,
   while what applies to both (observation grades, environment variables,
   instances) appears in each.  The order follows the screen, top to bottom. */
static void help_keys_dash(void){
    hout(
"── 대시보드(-b) · 이 화면이 무엇을 보여주나 ──\n"
"\n"
"  cub_server 프로세스의 메모리를 /proc 과 프로세스 메모리만 읽어 분해한다. 쓰기·락 0,\n"
"  서버 질의는 서버마다 paramdump 1회뿐이다(재시작 시 다시, 무침습).\n"
"  핵심 질문은 하나다 — \"이 메모리가 왜 이만큼인가, 늘고 있다면 어디가 늘었나\".\n"
"\n"
"  화면은 위에서 아래로 여섯 상자다. 아래 설명도 그 순서다.\n"
"\n"
"    맨 윗줄        수집 시간(이 프레임에 든 실작업 ms) · 현재 시각\n"
"    ① OS 컨텍스트  호스트 CPU·메모리 · 가동 인스턴스 목록(▶ = 지금 보는 DB)\n"
"    ② RSS·프로세스 master / server / pl / broker / CAS 티어별 상주(top 기준)와 코어\n"
"    ③ RSS·서버     영역별 alloc·rss·conf·grow, 게이지 = 상주/할당\n"
"    ④ 동적 메모리  [B] 정확 · [A] 청크 추정 · ④' 버퍼풀 상태 · ⓐ 용량\n"
"    ⑤ 메모리 추이  서버 전체·동적 힙 증감 + load(OS) 겹침\n"
"    ⑥ I/O          iowait · 장치 · D스레드 → 맨 아래 한 줄 판정문\n"
"\n"
"  읽는 순서: ① 호스트가 괜찮은지 → ②③ 서버가 얼마나 쓰는지 →\n"
"  ③ 제목 옆 \"설정으로 설명 N%%\" 가 낮으면 ④ 로 내려가 무엇이 차지하는지 →\n"
"  느리다는 신고가 있으면 ⑥ 판정문부터.\n"
"\f\n"
"── ① OS 컨텍스트 — CPU 와 load ──\n"
"  CPU 32c/64t     물리 32 코어 / 논리 64 스레드. 다르면 하이퍼스레딩이다.\n"
"                  물리보다 많은 부하는 같은 코어를 나눠 쓰는 것이라 선형으로 안 빨라진다.\n"
"  total|used|pct  total=논리코어, used=사용 중 코어 수, pct=전체 사용률.\n"
"                  used 는 '몇 코어어치가 돌고 있나' 라 프로세스 박스의 코어 수와 바로 비교된다.\n"
"  막대(게이지)    코어를 바쁜 순으로 정렬해 왼쪽부터 그린 것 — 합계가 아니라 분포다.\n"
"                  색으로 구분한다 — 적색=포화(≥95%%) 주황=높음(≥50%%)\n"
"                  녹색=중간(≥10%%) 회색=유휴. 글리프는 다른 게이지와 같다.\n"
"                  used 12 이 12코어에 고르게 퍼진 것인지 6개가 100%%인지는 합계로는\n"
"                  구분되지 않는다 — 뒤엣것이 병목이고, 왼쪽 붉은 구간의 폭이 그것이다.\n"
"                  ※ 코어가 막대 폭보다 많으면 한 칸이 여러 코어를 대표하며 그 구간의\n"
"                    최댓값을 그린다(포화를 평균으로 감추지 않기 위함). 그래서 칸 수에\n"
"                    코어 수를 곱해 읽으면 안 된다 — 정확한 개수는 옆의 sat 숫자를 볼 것.\n"
"  sat N           포화 코어 수 — 사용률 95%% 이상인 코어가 몇 개인가.\n"
"                  포화 직전(50~95%%)은 막대의 주황 구간이 보여준다.\n"
"                  btop 의 코어별 미터에서 100%% 칸을 세는 것과 같은 값이다.\n"
"                  이 값이 물리코어 수에 근접하면 CPU 가 병목이다. 반대로 used 는 큰데\n"
"                  sat 이 0 이면 부하가 고르게 퍼진 것이라 아직 여유가 있다.\n"
"  us  user        사용자 코드. 질의 처리·정렬·조인 등 엔진이 실제 일한 몫\n"
"  sy  system      커널. I/O 발행, 락, 컨텍스트 스위치. us 대비 과도하면 경합을 의심\n"
"  load 1/5/15     최근 1·5·15분 평균 실행대기 태스크 수. CPU 사용률이 아니다 —\n"
"                  리눅스에서는 D 상태(디스크 대기)도 포함하므로, CPU 가 한가해도\n"
"                  I/O 가 막히면 올라간다. 논리코어 수(64)를 넘으면 대기가 쌓이는 중.\n"
"                  전체 호스트 값이라 인스턴스별로 나눌 수 없어 박스 제목에 둔다.\n"
"\f\n"
"── ② 프로세스 박스 — top 과 같은 RSS · 티어별 CPU ──\n"
"  RSS = 프로세스가 지금 물고 있는 물리 메모리(top 의 RES). master·server·pl·broker·CAS 는\n"
"  운영 관점의 점유 현황이라 top·ps 와 같은 값으로 보이고, 매 프레임 그때의 값이다.\n"
"  공유 페이지(libcubrid.so 등)는 프로세스마다 세므로 All 합계는 실제 점유의 상한이다.\n"
"  상세 분해(영역·힙·버퍼풀)는 분석 대상 서버 하나만 한다 — 아래 ③ 이하.\n"
"  프로세스 박스   각 티어(master/server/pl/broker/CAS) 앞의 숫자가 그 티어의 코어 수.\n"
"                  전부 더하면 OS 의 used 이하여야 정상이다(나머지는 CUBRID 밖 프로세스).\n"
"                  색으로 크기를 구분한다 — 회색 · =유휴, 청록 <1코어, 노랑 1코어 이상,\n"
"                  주황=눈에 띄는 부하, 적색=물리코어 수 초과. 0 은 숫자 대신 · 로 눌러\n"
"                  실제로 돌고 있는 티어만 눈에 들어오게 했다.\n"
"  ※ 다른 도구와 대조할 때\n"
"    · server 의 CPU 는 top 과 같은 '1코어=100%' 규약이라 100% 를 넘을 수 있다.\n"
"      OS 박스의 코어 수와 견주려면 100 으로 나눌 것(-d 출력은 tier.*_cores 로 이미 환산).\n"
"    · Memory 의 used 는 MemTotal-MemAvailable 로 top 과 같은 정의다.\n"
"      htop 은 Shmem 을 달리 처리해 수 GB 낮게 나오는데, 오차가 아니라 정의 차이다.\n"
"\f\n"
"── ③ RSS · 서버 — 각 항목이 알려주는 것 ──\n"
"  data_buffer     페이지 버퍼. 설정보다 큰 게 정상(BCB·victim 배열이 같은 매핑에 포함)\n"
"  log_buffer      로그 버퍼. 선할당이라 설정≈상주\n"
"  dynamic-heap    정렬·캐시·세션이 섞인 동적 힙 → 아래 [B]/[A] 로 분해\n"
"  thread stacks   워커 스택. 예약 크고 상주 극소가 정상\n"
"  glibc arena     할당자 예약. 상주 0이면 물리 메모리 미사용\n"
"  (설정 X, 할당 Y%)  설정 대비 매핑 비율. >105% 면 초과\n"
"  Σ 합계          4열의 합. rss 합 = 프로세스 RSS(항등)로 프로세스 박스의 server 와 같다.\n"
"                  라벨의 PSS 는 공유 .so 를 나눠 배분한 값이다(오류 아님).\n"
"\n"
"  열: alloc 할당 · rss 상주 · conf 설정값 · grow rss 증감(주황)\n"
"  게이지: 6점 글리프 = 상주/할당(녹→적), 끝의 주황 = 이번 프레임 증가분\n"
"  최근 접근 비율(핫)은 보여주지 않는다 — 재려면 대상 페이지 테이블을 전수 순회하는\n"
"  clear_refs 쓰기가 필요하고, 모니터가 그 부하를 만들면 안 되기 때문이다.\n"
"\f\n"
"── ③ 파라미터 귀속률 \"설정으로 설명 N%%\" (③ 박스 제목 옆) ──\n"
"  서버 메모리 중 cubrid.conf 설정으로 '왜 이만큼인지' 설명되는 비율.\n"
"    귀속   data_buffer(data_buffer_size), log_buffer(log_buffer_size)\n"
"    비귀속 dynamic-heap, thread stacks, code(.so) 등 — 설정으로 통제되지 않는 부분\n"
"  읽는 법:\n"
"    높다(90%대)   정상. 메모리 대부분이 설정대로다\n"
"    떨어진다      dynamic-heap 이 자라고 있다는 뜻 — 아래 heap 박스로 내려가\n"
"                  무엇이 늘었는지 확인할 것\n"
"  즉 이 값은 'heap 박스를 들여다볼 필요가 있는가' 를 알려주는 신호다.\n"
"\n"
"── 관측 등급 (값 왼쪽 1글자) ──\n"
"  ● 측정    OS/엔진이 직접 보고. 신뢰 가능\n"
"  ◐ 예약    매핑만 확인 — 실사용과 크게 다를 수 있음\n"
"  ◌ 추정    크기 패턴 추정 — 상대 비중만\n"
"  ? 미귀속  아직 분해되지 않음 — 해석하지 말 것\n"
"\f\n"
"── ④ 동적 메모리 상세(= glibc malloc 힙) — B=정확 · A=크기 추정 ──\n"
"  ※ 여기서 '힙'은 프로세스의 동적 할당 영역(malloc)이다. 테이블 데이터가 저장되는\n"
"     스토리지의 '힙 파일'(HFID)과는 다른 것이며, 그쪽은 cub_volmap 이 다룬다.\n"
"  [B] 항목        엔진이 직접 보고하는 값 → 정확. plan·결과·필터 캐시·락·세션·카탈로그·연결\n"
"  lock table (tran)  lk_Gl.tran_lock_table — 트랜잭션별 락 보유 슬롯이다.\n"
"                  num_trans x 152B 로, 접속 상한에 비례해 미리 잡힌다.\n"
"                  ※ cubrid lockdb 의 \"Object Lock Table\" 과는 다른 구조다.\n"
"                    저쪽은 잠긴 객체의 LK_ENTRY/LK_RES 풀(예: 1000개 242K)이고\n"
"                    이쪽은 트랜잭션 슬롯(예: 101개 15K)이라 값이 다른 것이 정상이다.\n"
"  [A] 항목        glibc 청크 분포 추정. 정렬·해시 작업버퍼 등 B가 못 잡는 것\n"
"  [A] overcount   A 합계가 상주 초과(free 청크·비상주 포함) — 정상, 상한으로 읽을 것\n"
"\n"
"── ④' 버퍼풀 상태 — 페이지 슬롯 = 버퍼 제어블록(BCB, Buffer Control Block) (◌) ──\n"
"  pgbuf_Pool.BCB_table 배열을 방법 B 와 같은 경로로 통째 읽어 어떤 페이지가 버퍼에\n"
"  올라와 있고(resident) 몇 장이 아직 볼륨에 안 내려갔는지(dirty) 센다. 래치 없는 스냅샷.\n"
"  페이지 헤더는 오프셋 검증용 표본 256개만(엔진이 최근 쓴 hot/warm 존, 상주 확인 후) 읽는다 —\n"
"  헤더를 읽으면 그 버퍼 페이지를 건드리게 되므로. 전수는 --bcb-dump 일 때만.\n"
"  버퍼 슬롯이 26만 개(16K 페이지 4GB)를 넘으면 배열 전수 스캔은 요청 시에만(d 키·--pgbuf-detail)\n"
"  하고, 평소에는 엔진이 스스로 유지하는 집계(존별 개수·victim 후보·dirty 수, ●)를 보인다.\n"
"  게이지: 전경=상주율, 배경(청색)=dirty 비율.  zones: hot/warm/cold = LRU 3구역,\n"
"  void = 적재/축출 전이 중, free = 빈 BCB.  victim 은 cold 에서 뽑고 dirty 는 건너뛴다.\n"
"  log: append = 로그 꼬리, flushed(nxio) = 디스크에 내려간 경계 → 차이 = 아직 fsync 안 된 로그.\n"
"  두 가지 '뒤처짐' 을 구분한다 — 둘 다 로그 꼬리(append) 기준이지만 대상이 다르다:\n"
"    fsync 지연 = append − flushed(nxio)     로그 자체가 디스크에 안 내려간 거리.\n"
"                 커밋 응답이 직접 느려지므로 더 급한 신호다.\n"
"    flush 지연 = append − oldest dirty LSA  데이터 페이지의 볼륨 반영이 뒤처진 거리\n"
"                 = 장애 시 redo 해야 할 길이. 정상에서도 0 보다 크며 크기보다 추세를 본다.\n"
"                 dirty 개수와 다르다 — 개수가 적어도 오래된 한 장이 남으면 이 값은 크다.  자기검증: VPID 이상치 1% · 헤더 읽기실패 50% · **재검 후** 잔여 불일치 5%(50% 초과면 비활성, 그 사이는 검증분만 집계).\n"
"  --bcb-dump F : 스냅샷을 파일로 → cub_volmap --bufmap F 로 볼륨 지도 위에 버퍼 상주/dirty 를 얹는다.\n"
"\f\n"
"── ⓐ 용량(capacity) — 증설·축소·개선을 무엇으로 판단하나 ──\n"
"  숲 관점의 축이다: 수요(지금 얼마 쓰나) · 상한(이 환경이 얼마까지 되나) · 헤드룸(남은 여유).\n"
"  수요   dev IOPS = DB 파일(볼륨·로그)이 놓인 디스크 전체, this server IOPS = 이 인스턴스 귀속(/proc/<pid>/io).\n"
"         장치는 호스트 최다사용 장치가 아니다. overlay 는 하위 XFS 의 fsid 로 찾고, 못 찾으면 장치 판정을 생략한다.\n"
"  지연   await = 요청당 대기(ms). IOPS 는 낮은데 await 가 크면 'IOPS 증설'이 아니라\n"
"         '더 빠른 스토리지'가 답이다. queue = 평균 큐 깊이.\n"
"  상한   큐가 쌓인 순간(평균 깊이 4 이상)의 IOPS 를 그 환경의 실효 상한으로 기억한다.\n"
"         **하나의 숫자로 기억하지 않는다.** 같은 장치라도 4KB 랜덤과 128KB 순차는 IOPS 상한이\n"
"         자릿수로 다르므로, I/O 프로파일 = 평균 요청 크기(<8KB / 8-64KB / >64KB) x 읽기 비중\n"
"         (write-heavy <30%% / mixed / read-heavy >70%%) 9칸으로 나눠 칸마다 따로 기억하고,\n"
"         지금 프로파일과 **같은 칸**의 상한하고만 비교한다. 다른 칸 값을 빌려 쓰면\n"
"         (128KB 로 잰 상한을 4KB 워크로드에 적용) '여유 있다' 는 거짓 판정이 나온다.\n"
"         상한은 이 실행 동안만 기억한다(파일로 남기지 않는다).\n"
"         그 칸에서 큐가 쌓인 적이 없으면 '이 칸은 미관측'으로 표기한다(추정을 사실로 위장하지 않는다).\n"
"  버퍼   히트율의 출처가 두 가지다.\n"
"         perfmon   엔진 카운터가 **이미 돌고 있을 때**(statdump·csql ;histo 등 watcher 존재)\n"
"                   그 값을 읽는다. 우리가 watcher 를 만들지는 않는다(무침습 절대 조건).\n"
"         turnover  카운터가 정지면 BCB 스냅샷 두 장의 VPID 집합 차이로 회전율을 낸다.\n"
"                   적재 pg/s = 미스, 재적재%% = 빠졌다 다시 들어온 비율(작업집합>버퍼 신호).\n"
"  판정   ⚠ 용량 한계 = 헤드룸 80%%↑ **그리고** 장치가 실제로 밀릴 때(await 20ms↑ 또는 큐 10↑).\n"
"         헤드룸만 높고 적체가 없으면 '상한 근접 — 한계 단정 불가'로만 말하고 증설을 권고하지 않는다.\n"
"         상한은 과거 다른 순간의 관측치라 워크로드가 가벼워지기만 해도 80%%를 넘길 수 있기 때문이다.\n"
"         · ⚠ 지연 병목(await 20ms↑ & IOPS 낮음)\n"
"         ⚠ 버퍼 부족(적재 많고 재적재 30%%↑) · ⚠ 재사용 거리가 버퍼 밖(재적재 30%%↓ — 스캔이거나 작업집합이 버퍼의 수 배)\n"
"         재적재 r%% 는 '버퍼를 2배로 늘리면 줄어드는 읽기' 의 추정치다(LRU 가정 — 하한이 아니다).\n"
"         프레임당 적재가 버퍼의 절반을 넘으면 스냅샷 사이를 다 못 봐 과소집계 — '신뢰도 낮음'으로 적는다.\n"
"         · 과다 할당(버퍼 60%% 미사용 & 적재 거의 없음 — 축소 가능) · · 여유\n"
"  비용   취합 예산 0.5초. 넘으면 그 프레임 값을 버리고 '취합 생략'을 화면에 남긴다.\n"
"         실측 0.2~0.9ms(예산의 0.2%% 미만). 대상 프로세스 쓰기 0, 접속·락 0.\n"
"\n"
"── ⑤ 메모리 추이 박스의 load(OS) 겹침 ──\n"
"  분홍=서버 전체, 적색=동적 힙, 노랑=load(OS, 1분). load 는 단위가 달라\n"
"  자체 최대 기준으로 모양만 겹치고, 실제 값·범위는 우측 범례에 있다.\n"
"  읽는 법: 메모리가 늘 때 load 도 같이 오르면 부하 기인 —\n"
"           load 는 평탄한데 메모리만 계속 오르면 누수를 의심할 것.\n"
"\f\n"
"── ⑥ I/O 박스 — iowait · D스레드 · 항목 ──\n"
"  wa  iowait      ※ 'CPU 가 I/O 를 기다리며 바빴다' 가 아니다.\n"
"                  CPU 가 놀고 있었고(idle) 동시에 그 CPU 에 디스크 대기 태스크가\n"
"                  있었던 시간의 비율 — 즉 idle 의 하위 분류다. 함정 둘:\n"
"                    · 코어 수로 나뉜다. 64코어에서 한 프로세스가 디스크를 완전히\n"
"                      포화시켜도 wa 는 1~2% 로밖에 안 보인다\n"
"                    · CPU 가 바쁘면 idle 이 없어 wa 도 0 이 된다 → I/O 병목이 가려진다\n"
"                  그래서 wa 단독으로 판단하지 말고 아래 조합으로 읽을 것:\n"
"                    wa 낮음 + dev util 높음  CPU 가 바빠 wa 가 가려진 상태. 병목 가능성 있음\n"
"                    wa 높음 + dev util 높음  디스크 병목이 확실 — data_buffer 부족을 의심\n"
"                    wa 높음 + dev util 낮음  디스크가 아닌 대기이거나 측정 구간이 짧다\n"
"                    majflt 증가             버퍼에 없어 디스크에서 읽은 것 — 증설의 직접 근거\n"
"                  dev util 과 majflt 는 코어 수에 희석되지 않는 물리량이라 더 믿을 만하다.\n"
"                  ※ I/O 박스가 이 조합을 대신 판정해 한 줄로 알려준다(아래 참조).\n"
"  D스레드 n/m     iowait 행 오른쪽. cub_server 스레드 m 개 중 n 개가 D 상태\n"
"                  (디스크 응답을 기다리는 중단 불가 대기)라는 뜻이다.\n"
"                  wa 와 달리 코어 수로 희석되지 않아, 대기가 있으면 반드시 잡힌다.\n"
"                  0 이 아니면 그 순간 실제로 디스크에 막혀 있다는 직접 증거다.\n"
"  cache absorb    요청 읽기 중 디스크로 안 내려간 비율. 0% 면 캐시가 무효\n"
"  dev util        장치 포화도 — I/O 가 많은지의 1차 판정. 100%면 포화\n"
"  blkio wait      이 서버가 디스크를 기다린 시간의 비율(delayacct_blkio_ticks).\n"
"                  전 스레드 합이라 한 코어(100%%)를 넘을 수 있다 — CPU 사용률과\n"
"                  같은 규약이다. cub_server 는 워커가 I/O 를 하므로 반드시 스레드를\n"
"                  모두 더해야 한다(메인 스레드만 보면 늘 0 이다).\n"
"                  pidstat -d 의 iodelay 와 같은 출처이며, 그쪽은 기본이 메인 스레드\n"
"                  기준이라(-t 로 스레드별) 값이 다르게 보일 수 있다.\n"
"                  비동기 대기는 안 잡히므로 0 을 '문제 없음'으로 단정하지 말 것.\n"
"\f\n"
"── ⑥ I/O 판정문 (I/O 박스 맨 아래) ──\n"
"  wa·장치 사용률·인스턴스 신호 세 가지를 조합해 결론을 한 줄로 낸다. 조건이\n"
"  풀려도 지워지지 않고 시각과 함께 최근 2건까지 남는다(지난 것은 회색) —\n"
"  I/O 이상은 순간적이라 지워지면 '봤는데 없어졌다' 가 되기 때문이다.\n"
"\n"
"  판정에 쓰는 세 조건\n"
"    (1) wa 높음    iowait >= 100/코어수 x 0.5 (%%). 코어 수에 맞춰 자동 조정된다 —\n"
"                   64코어면 0.78%%, 32코어면 1.6%%, 4코어면 12.5%% (하한 0.5%%).\n"
"                   고정 3%% 같은 기준은 코어가 많으면 도달하지 않아 병목을 놓친다.\n"
"    (2) 장치 바쁨  최다사용 장치의 사용률 >= 80%%. 장치를 식별했을 때만 성립한다.\n"
"    (3) 내 신호    이 인스턴스가 실제로 I/O 중이라는 증거. 넷 중 하나면 성립:\n"
"                   majflt >= 1/s · D스레드 > 0 · blkio >= 1%% · 이 서버 io >= 1MB/s\n"
"                   ※ 컨테이너(overlay)에서는 DB 파일의 백킹 장치를 diskstats 에서\n"
"                     못 찾아 (2)가 인스턴스 부하와 이어지지 않는다. 그래서 (3)에\n"
"                     이 서버의 실제 읽기/쓰기량을 반드시 포함한다.\n"
"\n"
"  나올 수 있는 경우 — 세 조건의 8조합이 판정 5종으로 모인다\n"
"   (1)(2)(3)  판정\n"
"    O  O  O   ⚠ 디스크 병목 — 셋 다 성립. 가장 확실한 신호다.\n"
"              data_buffer 를 늘려 디스크 읽기를 줄이는 것이 1차 대응이다.\n"
"    X  O  O   ⚠ wa 가 가려짐 — 장치는 바쁜데 wa 가 낮다. CPU 가 바빠 idle 이\n"
"              없으면 wa 도 0 이 된다(wa 는 idle 의 하위 분류). 병목일 수 있다.\n"
"    O  X  O   · 비디스크 대기 — wa 는 있는데 장치는 한가. 네트워크·NFS 대기이거나\n"
"    O  X  X     측정 구간이 짧아 장치 통계가 아직 안 잡힌 경우다.\n"
"    X  O  X   · 다른 프로세스의 부하 — 장치는 바쁘지만 이 인스턴스는 조용하다.\n"
"    O  O  X     CUBRID 문제가 아니므로 같은 호스트의 다른 프로세스를 볼 것.\n"
"    X  X  O   (표시 없음) — 이 서버가 I/O 를 하지만 wa 도 장치도 한가하다.\n"
"    X  X  X   (표시 없음) — 유휴. 정상 상태다.\n"
"\n"
"  읽는 순서\n"
"    1) ⚠(적색·주황)이면 그 줄만 읽어도 결론이 난다.\n"
"    2) ·(회색)이면 이 인스턴스의 문제가 아니거나 판단하기 이른 상태다.\n"
"    3) 아무것도 없으면 I/O 는 정상이다 — 느리다면 CPU(OS 박스)나 잠금을 볼 것.\n"
"\f\n"
"── 대시보드 · 키 ──\n"
"\n"
"    키            동작\n"
"    ───────────   ─────────────────────────────────────────────────────────\n"
"    d             ④' 대형 버퍼풀의 페이지 단위 스캔 켬/끔 (기본 끔 — 엔진 집계만)\n"
"    <  >          인스턴스(DB) 이동 — 여러 cub_server 가 떠 있을 때\n"
"    p             시계열 화면으로 (그동안 쌓인 히스토리가 바로 그려진다)\n"
"    space         일시정지 / 재개\n"
"    l             한글 / 영문 라벨 전환\n"
"    h             이 도움말        q   종료\n"
"\n"
"  ※ 한글 입력 상태로 눌러도 된다 — ㅂ=q  ㅗ=h  ㅔ=p  ㅣ=l (자판 위치로 되돌림)\n"
"\n"
"── 파라미터 읽기 (설정 축이 살아 있으려면) ──\n"
"  서버마다 시작할 때 한 번, 서버가 매핑한 설치본의 'bin/cubrid paramdump -C <db>' 를\n"
"  서버와 같은 CUBRID_DATABASES 로 실행해 실제로 적용 중인 파라미터를 읽는다\n"
"  (셸 없이 execvp, 라이브는 비동기라 화면이 멈추지 않음, 10초 무응답이면 종료).\n"
"  못 읽으면 사유만 적고 스스로 다시 시도하지 않는다 — p 로 화면을 전환할 때 다시 읽는다.\n"
"  읽지 못하면 $CUBRID/conf/cubrid.conf 를 직접 파싱하고, 그것도 없으면 아래가 모두 빠진다:\n"
"    · data_buffer/log_buffer 의 '설정 대비 %' 판정 — 설정값이 없어 초과 여부를 알 수 없다\n"
"    · 파라미터 귀속률 — 설정으로 설명되는 몫을 셀 수 없어 표시되지 않는다\n"
"    · DB 페이지 크기 — data_buffer_size/pages 로 역산한다(실패 시 16KB 로 가정)\n"
"    · 방법 B 자기검증 — num_buffers 와 data_buffer_pages 대조가 불가해 힙 B 가 꺼진다\n"
"    · 하단 '메모리 파라미터' 표 전체\n"
"  즉 RSS/PSS 같은 실측은 그대로 나오지만, '왜 이만큼인가' 를 설명하는 축이 사라진다.\n"
"\n"
"── 여러 데이터베이스(인스턴스) ──\n"
"  cub_top <db명>     그 DB 를 본다.  생략하면 가동 중인 것 중 알파벳순 첫 번째\n"
"  라이브에서 < > 로 이동. 상세 추적은 화면에 보이는 DB 하나뿐이고,\n"
"  나머지는 총량만 집계한다(상세 직독을 전부에 돌리면 그만큼 대상들에 접촉이 늘기 때문).\n"
"  -d 출력은 instance.<db>.* 로 모든 인스턴스를 내보내며 tracked=1 이 현재 추적 대상이다.\n"
"\n"
"  자세한 한계는 docs/limitations.md, 이식성은 docs/portability.md 참조.");
}
static void help_keys_plot(void){
    hout(
"── 시계열(-p) · 이 화면이 무엇을 보여주나 ──\n"
"\n"
"  대시보드가 \"지금 값\"이라면 이 화면은 \"그 값이 어떻게 움직였나\"다.\n"
"  한 프레임의 숫자로는 알 수 없는 세 가지를 여기서 얻는다:\n"
"\n"
"    1) 추세   지금 값이 원래 그런지, 방금 올라간 것인지\n"
"    2) 상관   메모리가 늘 때 load 도 같이 올랐나(부하 기인) 혼자 올랐나(누수)\n"
"    3) 순서   장치가 먼저 포화됐나, 버퍼 적재가 먼저 늘었나 — 원인과 결과의 방향\n"
"\n"
"  ★ 패널의 범위가 다르다 — 1번은 호스트 전체, 2~4번은 지금 보는 인스턴스다.\n"
"    머신 차원의 문제인지(1번) 이 DB 의 문제인지(2~4번)를 한 화면에서 가른다.\n"
"\n"
"    맨 윗줄   db · 관측창(샘플 수·간격) · 인스턴스 위치 · 현재 시각\n"
"    1 호스트 리소스   CPU 코어·포화·RAM·장치·load          [호스트 전체]  + 판정문\n"
"    2 인스턴스 메모리 왜·어디서 느나 (할당 vs 상주, load)  [이 인스턴스] + 판정문\n"
"    3 올리는 쪽       볼륨→버퍼, 버퍼가 일하나             [이 인스턴스] + 판정문\n"
"    4 내리는 쪽       버퍼→볼륨, 쓰기가 밀리나             [이 인스턴스] + 판정문\n"
"    맨 아랫줄 시간축 눈금 · 키 안내\n"
"\n"
"  공통 규약\n"
"    · 관측창은 자동 확장된다 — 시작 직후 초 단위, 오래 두면 분 단위(제목에 표기).\n"
"    · 범례는 \"이름 현재값 (최소~최대)\" 다. 모양은 그래프, 숫자는 범례에서 읽는다.\n"
"    · 3·4 패널의 회색 겹침선은 호스트 값이다(장치%%, await) — 주 계열은 이 인스턴스,\n"
"      겹침은 환경. 둘을 대조해 \"장치는 포화인데 내 인스턴스는 조용\"(남의 부하)을 읽는다.\n"
"    · 1·3·4 패널은 계열마다 단위가 달라 계열별로 정규화한다(높이끼리 비교 금지).\n"
"      2 패널은 공통 바이트 축이라 높이 비교가 유효하다.\n"
"    · 각 패널 아래 한 줄 판정문:  ⚠ = 조치 신호   · = 사실 기록\n"
"    · 표본 2개 미만이면 \"표본 수집 중…\" 으로 판정을 보류한다(추측하지 않는다).\n"
"\f\n"
"── 1 호스트 리소스 [호스트 전체] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    사용 코어 (하늘)      코어         호스트에서 실제 돌고 있는 코어 수(전체 프로세스)\n"
"    포화 코어 (적색)      개           사용률 95%% 이상 코어 수. 물리코어에 근접하면 CPU 가 제약\n"
"    호스트 RAM 사용 (자주)바이트       MemTotal-MemAvailable. 92%% 넘으면 여유가 적다\n"
"    CUBRID 총합 (분홍)    바이트       CUBRID 프로세스 전체 RSS 합(top 기준) — RAM 중 우리 몫의 상한\n"
"    장치 사용률 (주황)    %%            최다사용 장치 포화도(호스트 전역). 80%%↑ 면 스토리지가 제약\n"
"    장치 IOPS (노랑)      회/s         호스트 장치 전체 IOPS. 포화 시점의 값이 그 환경의 실효 상한\n"
"    load (노랑 겹침)      태스크       실행 대기 포함. 코어 수를 넘으면 큐가 쌓인다\n"
"\n"
"  읽는 법 — \"머신이 문제냐, 이 DB 가 문제냐\" 를 먼저 가른다.\n"
"    포화 코어 있음        호스트 CPU 가 제약(판정문: ⚠ 포화 코어 N/N — 호스트 CPU 가 제약)\n"
"    load > 코어 수        실행 대기 큐 — 호스트 전체 값이라 CUBRID 만의 문제로 단정 불가\n"
"    장치 80%↑             호스트 스토리지가 제약 → 이 인스턴스 몫은 3·4번 패널에서 확인\n"
"    호스트 RAM 92%↑       여유 부족. CUBRID 총합과 견주어 우리 몫인지 남의 몫인지 본다\n"
"    CUBRID 총합이 낮은데   같은 호스트의 다른 프로세스가 원인 — CUBRID 를 건드릴 일이 아니다\n"
"    RAM·CPU 가 높다\n"
"\f\n"
"── 2 인스턴스 메모리 [이 인스턴스] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    server(PSS) (분홍)    바이트       이 인스턴스 상주(PSS). 우상향이면 무엇이든 늘고 있다\n"
"    data_buffer (녹색)    바이트       페이지 버퍼 몫. 설정대로면 평탄하다\n"
"    log_buffer (노랑)     바이트       로그 버퍼 몫. 선할당이라 거의 평탄\n"
"    dynamic-heap (적색)   바이트       malloc 힙(정렬·캐시·세션). server 와 같이 오르면 원인이 여기\n"
"    할당(VmSize) (하늘)   바이트       예약. 상주와의 간격 = 잡아뒀지만 안 쓰는 몫\n"
"    load (노랑 겹침)      태스크       호스트 load — 증가가 부하 기인인지 가르는 기준선\n""    heapB 합계 (연녹)     바이트       동적 메모리 중 엔진이 직접 보고하는 몫(정확). 계단형이 정상 —\n"
"                                       conn/lock/session 은 max_clients 비례 선할당이라 고정이고,\n"
"                                       plan·결과·카탈로그 캐시만 실사용에 따라 오른다\n"
"                                       (실측: 유휴 154KB → 질의 6세션 412KB)\n"
"\n"
"  ※ A(크기 추정)는 이 그래프에 없다 — r/a 로만 재고 수백 ms 가 걸려 표본이 띄엄띄엄하다.\n"
"    점 몇 개를 선으로 이으면 없는 추세가 생기므로 넣지 않는다. A 가 필요하면 p 로 대시보드에서 본다.\n"
"    dynamic-heap 은 오르는데 heapB 가 평탄하면 = B 가 못 잡는 몫(A 영역)이 늘고 있다는 신호다.\n"
"\n"
"  읽는 법 — 이 패널의 목적은 \"증가가 부하 때문인가\" 를 가르는 것이다.\n"
"    메모리↑ + load↑      부하 기인(접속·질의 증가로 작업 메모리·캐시가 커진 것)\n"
"                         판정문: ⚠ 서버 +N (구간) · load 도 함께 상승 → 부하 기인 증가\n"
"    메모리↑ + load 평탄   부하와 무관한 증가 = 누수 의심\n"
"                         판정문: ⚠ … load 평탄 → 동적 메모리 상세(B/A)에서 어느 항목인지 확인\n"
"                         → p 로 대시보드에 가서 ④ [B]/[A] 로 항목을 특정한다\n"
"    dynamic-heap 만↑      정렬·캐시·세션 쪽. data_buffer 는 설정이라 이렇게 안 움직인다\n"
"    할당만↑ 상주 평탄     예약만 늘었다 — 물리 메모리는 아직 안 쓴다(문제 아님)\n"
"    메모리↓               작업 메모리·캐시 반환(정상 회수)\n"
"\n"
"  제목 오른쪽: 시간당 증가율(+N/h)과 RAM 대비 점유율. 창이 짧으면 순간 변동이 과대 반영된다.\n"
"\f\n"
"── 3 올리는 쪽 (볼륨 → 버퍼) [이 인스턴스] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    디스크 읽기 (적색)    B/s          볼륨에서 실제로 읽어 올린 양. 버퍼가 일하면 낮다\n"
"    버퍼 상주%% (녹색)     %%            data_buffer 중 페이지가 올라와 있는 비율\n"
"    cold 존%% (주황)       %%            LRU cold 비중. 높으면 재사용 없이 지나가는 페이지가 많다\n"
"    적재 pg/s (노랑)      pg/s         버퍼에 새로 올린 페이지 수 = 미스율의 직접 지표\n""    장치%%(호스트) (회색)  %%            겹침선. 호스트 전역 장치 포화도(이 인스턴스 값 아님)\n"
"\n"
"  패널 아래 막대 — 선으로는 값이 작아 바닥에 붙어 안 보이던 지표를 따로 깐다\n"
"  (주식 차트의 거래량 자리. 막대는 자기 최대 기준, 백분율은 100 기준):\n"
"    흡수%%  버퍼가 미스해도 OS 페이지 캐시가 받아준 비율 — 높으면 디스크로 안 내려간다\n"
"           (실측 호스트 캐시 64.7GB vs data_buffer 512MB)\n"
"    OS 캐시 흡수%% (연노랑) %%           버퍼가 미스해도 OS 페이지 캐시가 받아준 비율.\n"
"\n"
"  읽는 법 — 버퍼 증설이 효과가 있을 상황인지 가른다.\n"
"    재적재 r%%            버퍼를 2배로 늘리면 디스크 읽기가 약 r%% 준다(추정치, LRU 가정)\n"
"                          CUBRID 는 중간점 삽입이라 실제와 어긋날 수 있고, 프레임당 적재가 버퍼의\n"
"                          절반을 넘으면 과소집계라 '신뢰도 낮음'으로 표시한다(--interval 을 줄일 것)\n"
"                          근거는 줌(3) 보조막대의 '재적재' 막대에서 눈으로 확인한다\n"
"    적재↑ + 재적재 30%%↑   작업집합 > 버퍼 → 증설이 듣는다 (판정문: ⚠ 버퍼 부족)\n"
"    적재↑ + 재적재 낮음   재사용 거리가 버퍼 밖 — 스캔이거나 작업집합이 버퍼의 수 배.\n"
"                          둘은 이 지표로 가를 수 없고, 2배 증설 효과가 약 r%% 에 그친다는 것만 말한다\n"
"    상주 100% + 읽기 0    버퍼에서 다 서비스되는 정상 (판정문: · 버퍼에서 서비스 중)\n"
"    상주 60% 미만 + 적재 0 과다 할당 — 축소 가능 (판정문: · 과다 할당)\n"
"    상주이 오르는 중       버퍼 채우는 중 — 히트율 판단 보류(기동 직후)\n"
"\n"
"  ※ 버퍼풀 상태(BCB) 읽기가 꺼진 환경에서는 상주%·cold%·적재가 없고, 디스크 읽기와\n"
"    캐시 흡수율만으로 판정한다(판정문에 \"버퍼풀 상태 읽기 비활성\" 이 붙는다).\n"
"\f\n"
"── 4 내리는 쪽 (버퍼 → 볼륨) [이 인스턴스] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    dirty 페이지 (분홍)   pg           아직 볼륨에 안 내려간 버퍼 페이지 수\n"
"    flush 지연 (주황)     로그pg       가장 오래된 dirty 가 로그 꼬리에서 뒤처진 거리\n"
"                                       = 장애 시 redo 해야 할 길이. dirty 페이지(개수)와 다르다 —\n"
"                                       개수가 적어도 오래된 한 장이 안 내려가면 이 값은 크다\n"
"                                       ※ 정상에서도 0 보다 크다. 크기보다 계속 늘어나는지를 볼 것\n"
"    디스크 쓰기 (하늘)    B/s          실제로 볼륨에 내려간 양\n"
"    D스레드 (적색)        개           디스크 응답을 기다리는 스레드 수. 코어 수에 희석되지 않아\n"
"                                       0 이 아니면 그 순간 실제로 막혀 있다는 직접 증거\n"
"    await ms(호스트) (회색) ms           겹침선. 호스트 장치의 요청당 평균 대기(이 인스턴스 값 아님)\n"
"\n"
"  읽는 법 — 쓰기 경로가 따라오지 못하는지, 커밋이 느려질 위험이 있는지.\n"
"    쓰기↑ 인데 1번 장치도↑   쓰기 병목 (판정문: ⚠ 장치 N% · 쓰기 N/s — 쓰기 병목)\n"
"    flush 지연↑ 계속        체크포인트가 밀린다 → 장애 복구 시간이 길어진다\n"
"                            판정문: ⚠ 로그 fsync 지연 N 로그 페이지 — 커밋 지연 위험\n"
"    dirty↑ 인데 쓰기 평탄   내려보내지 못하고 쌓이는 중 — 1번의 장치·이 패널 await 를 함께\n"
"    await↑(겹침) 인데       스토리지 자체가 느리다. 증설이 아니라 교체가 답이다\n"
"    내 쓰기는 평탄\n"
"    D스레드 > 0 지속        이 인스턴스가 실제로 디스크에 막혀 있다 — wa 가 0 이어도 확실한 증거\n""\n"
"  flush 지연 과 fsync 지연 — 둘 다 \"로그 꼬리(append)에서 얼마나 뒤처졌나\" 이지만 대상이 다르다.\n"
"\n"
"    이름          계산                      무엇이 뒤처진 것인가          어디에 나오나\n"
"    ───────────   ───────────────────────   ──────────────────────────   ──────────────────\n"
"    flush 지연    append − oldest_dirty     데이터 페이지의 볼륨 반영     이 패널의 주황 계열\n"
"    fsync 지연    append − nxio(flushed)    로그 자체의 디스크 기록      이 패널 판정문·트리\n"
"\n"
"    · flush 지연만 크다   데이터 반영이 밀렸다 → 체크포인트 주기·flush 처리량을 본다.\n"
"                          장애 시 redo 가 길어질 뿐, 커밋 응답은 정상일 수 있다.\n"
"    · fsync 지연이 있다   로그가 디스크에 안 내려갔다 → 커밋 응답이 직접 느려진다(더 급한 신호).\n"
"                          판정문: ⚠ 로그 fsync 지연 N 로그 페이지 — 커밋 지연 위험\n"
"    · 둘 다 크다          쓰기 경로 전체가 밀린 상태 — 1번의 장치·await 겹침선을 함께 볼 것.\n"
"\n"
"  1번(호스트)과 세로로 함께 읽는 것이 이 화면의 값이다 — 예: 1번 장치 포화가 먼저 오르고\n"
"  뒤이어 3번 적재가 오르며 2번 메모리가 평탄하면, 원인은 쓰기·읽기 경로이고 메모리는 결과가 아니다.\n"
"  1번 장치는 바쁜데 3·4번이 조용하면 다른 프로세스의 부하다.\n"
"\f\n"
"── 시계열 · 키 ──\n"
"\n"
"    키            동작\n"
"    ───────────   ─────────────────────────────────────────────────────────\n"
"    2 3 4         그 패널을 확대\n"
"                    화면 = [1 호스트 요약] + [확대한 패널] + [원인 지표 막대]\n"
"                    확대는 세로를 키우는 것만이 아니다 — 아래에 그 지표의 *원인이 되는\n"
"                    양*을 같은 시간축 막대로 깔아 원인·결과를 한 번에 읽게 한다\n"
"                    (주식 차트의 가격+거래량 구조. 막대는 모양, 숫자는 우측 값)\n"
"                      2 확대 → heapB · 마이너 폴트\n"
"                      3 확대 → 적재 pg/s · OS 캐시 흡수%% · 메이저 폴트\n"
"                      4 확대 → 디스크 쓰기 · D스레드 · blkio%%\n"
"    1             기본 화면(4패널)으로 복귀. 1번은 확대 대상이 아니다\n"
"                    (줌에서도 1번은 항상 위에 남으므로 따로 크게 볼 이유가 없다)\n"
"                    확대한 키를 다시 눌러도 복귀한다\n"
"    w             관측창 전환: 라이브(표본 그대로) → 1시간(1분 평균) → 24시간(1분 평균). 판정문의 창 길이도 따라간다\n"
"    <  >          인스턴스(DB) 이동 — 2~4번이 그 DB 로 바뀌고 히스토리도 새로 쌓는다\n"
"    p             대시보드 화면으로 (지금 값의 분해를 보러)\n"
"    space         일시정지 / 재개 — 멈춰 두고 구간을 비교할 때\n"
"    l             한글 / 영문 라벨 전환\n"
"    h             이 도움말        q   종료\n"
"\n"
"  ※ 한글 입력 상태로 눌러도 된다 — ㅂ=q  ㅗ=h  ㅔ=p  ㅣ=l (자판 위치로 되돌림)\n"
"  ※ 히스토리는 대시보드를 보는 동안에도 계속 쌓인다 — p 로 오면 그동안의 창이 바로 그려진다.\n"
"  ※ 대시보드 ⑤ \"메모리 추이\" 상자는 이 2번 패널의 축소판이다.\n"
"\f\n"
"── 파라미터 읽기 (설정 축이 살아 있으려면) ──\n"
"  서버마다 시작할 때 한 번, 서버가 매핑한 설치본의 'bin/cubrid paramdump -C <db>' 를\n"
"  서버와 같은 CUBRID_DATABASES 로 실행해 실제로 적용 중인 파라미터를 읽는다\n"
"  (셸 없이 execvp, 라이브는 비동기라 화면이 멈추지 않음, 10초 무응답이면 종료).\n"
"  못 읽으면 사유만 적고 스스로 다시 시도하지 않는다 — p 로 화면을 전환할 때 다시 읽는다.\n"
"  읽지 못하면 $CUBRID/conf/cubrid.conf 를 직접 파싱하고, 그것도 없으면 아래가 모두 빠진다:\n"
"    · data_buffer/log_buffer 의 '설정 대비 %' 판정 — 설정값이 없어 초과 여부를 알 수 없다\n"
"    · 파라미터 귀속률 — 설정으로 설명되는 몫을 셀 수 없어 표시되지 않는다\n"
"    · DB 페이지 크기 — data_buffer_size/pages 로 역산한다(실패 시 16KB 로 가정)\n"
"    · 방법 B 자기검증 — num_buffers 와 data_buffer_pages 대조가 불가해 힙 B 가 꺼진다\n"
"    · 하단 '메모리 파라미터' 표 전체\n"
"  즉 RSS/PSS 같은 실측은 그대로 나오지만, '왜 이만큼인가' 를 설명하는 축이 사라진다.\n"
"\n"
"── 여러 데이터베이스(인스턴스) ──\n"
"  cub_top <db명>     그 DB 를 본다.  생략하면 가동 중인 것 중 알파벳순 첫 번째\n"
"  라이브에서 < > 로 이동. 상세 추적은 화면에 보이는 DB 하나뿐이고,\n"
"  나머지는 총량만 집계한다(상세 직독을 전부에 돌리면 그만큼 대상들에 접촉이 늘기 때문).\n"
"  -d 출력은 instance.<db>.* 로 모든 인스턴스를 내보내며 tracked=1 이 현재 추적 대상이다.\n"
"\n"
"  자세한 한계는 docs/limitations.md, 이식성은 docs/portability.md 참조.");
}
/* Help entry point.
   mode 0 = batch/CLI (-h): name, usage, options plus the dashboard description.
   mode 1 = live dashboard, 2 = live time series: that mode's help only. */
static void usage_modal(int mode){
    if(mode==2) help_keys_plot();
    else        help_keys_dash();
}

/* A numeric option value: the whole string must be a number within [lo,hi]. */
static int opt_num(const char *opt,const char *s,double lo,double hi,double *out){
    char *end; double v=strtod(s,&end);
    if(end==s || *end || !(v>=lo && v<=hi)){
        fprintf(stderr,"cub_top: %s 의 값 '%s' 이(가) 올바르지 않습니다 (범위 %g ~ %g)\n",opt,s,lo,hi);
        return 0;
    }
    *out=v; return 1;
}

int main(int argc,char **argv){
    g_argc=argc; g_argv=argv;
    int terse=0,tree=0,live=0,view0=0,dump_hist=0,force_ko=0,interval_set=0,speed_set=0; double interval=0.5;
    int bcb_only=0;                   /* --bcb-dump with no display mode: write the snapshot only */
    const char *want_db=NULL;         /* The positional argument is the db to view; without it, the first alphabetically */
    /* Options that take a value.  The tests below are written "&&i+1<argc", which would
       let an option without its value match nothing (--replay with no path would sample
       live), so a missing value is rejected here first. */
    static const char *const NEEDVAL[]={"--record","--replay","--replay-speed","--offsets",
        "--interval","--bcb-dump",NULL};
    for(int i=1;i<argc;i++){
        for(int n=0;NEEDVAL[n];n++)
            if(!strcmp(argv[i],NEEDVAL[n])&&i+1>=argc){
                fprintf(stderr,"cub_top: %s 에 값이 필요합니다\n",argv[i]);
                return 2;
            }
    }
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-t")||!strcmp(argv[i],"--tty")) tree=1;
        else if(!strcmp(argv[i],"-d")||!strcmp(argv[i],"--dump")) terse=1;
        else if(!strcmp(argv[i],"--dump-json")){ terse=1; g_json=1; }
        else if(!strcmp(argv[i],"-b")||!strcmp(argv[i],"-i")||!strcmp(argv[i],"--live")) live=1;
        else if(!strcmp(argv[i],"-p")||!strcmp(argv[i],"--plot")){ live=1; view0=1; }
        else if(!strcmp(argv[i],"--record")&&i+1<argc) g_rec_path=argv[++i];
        else if(!strcmp(argv[i],"--record-overwrite")) g_rec_overwrite=1;
        else if(!strcmp(argv[i],"--stream")){ live=1; g_stream=1; }
        else if(!strcmp(argv[i],"--replay")&&i+1<argc) g_rep_path=argv[++i];
        else if(!strcmp(argv[i],"--replay-speed")&&i+1<argc){ if(!opt_num(argv[i],argv[i+1],0,1e6,&g_rep_speed)) return 2; i++; speed_set=1; }
        else if(!strcmp(argv[i],"--dump-hist")) dump_hist=1;
        else if(!strcmp(argv[i],"--pgbuf-detail")) g_pg_detail=1;
        else if(!strcmp(argv[i],"--ascii")) g_ascii=1;
        else if(!strcmp(argv[i],"--ko")) force_ko=1;
        else if(!strcmp(argv[i],"--offsets") && i+1<argc)
            snprintf(g_offs_file,sizeof g_offs_file,"%s",argv[++i]);
        else if(!strcmp(argv[i],"--interval") && i+1<argc){ if(!opt_num(argv[i],argv[i+1],0.05,3600,&interval)) return 2; i++; interval_set=1; }
        else if(!strcmp(argv[i],"--bcb-dump") && i+1<argc) snprintf(g_bcb_dump,sizeof g_bcb_dump,"%s",argv[++i]);
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")||!strcmp(argv[i],"/h")
                ||!strcmp(argv[i],"-?")||!strcmp(argv[i],"/?")){
            /* Like any CLI: the usage and options, printed in place.  The screen
               explanations stay in the live h help. */
            usage_opts();
            return 0; }
        else if(argv[i][0]!='-'&&argv[i][0]!='/'){
            /* One database per run */
            if(want_db){ fprintf(stderr,"cub_top: 데이터베이스는 하나만 지정합니다 ('%s' 와 '%s')\n",want_db,argv[i]);
                         return 2; }
            want_db=argv[i]; }
        else {
            /* Unknown options are rejected so a typo (--jsn) cannot pass for a valid run. */
            fprintf(stderr,"cub_top: 알 수 없는 옵션 '%s' (-h 로 목록 확인)\n",argv[i]);
            return 2;
        }
    }
    /* --record captures live frames only: elsewhere it would leave an empty file, and
       with --replay on the same path it would truncate the recording being read. */
    if(g_rec_path && g_rep_path){
        fprintf(stderr,"cub_top: --record 와 --replay 는 함께 쓸 수 없습니다 (녹화 파일 보호).\n");
        return 2;
    }
    if(g_rec_path && !live){
        fprintf(stderr,"cub_top: --record 는 라이브 모드(-b 또는 -p)에서만 기록합니다.\n");
        return 2;
    }
    if(g_stream && g_rec_path){
        fprintf(stderr,g_ascii?"cub_top: --stream writes the frames to stdout; --record is not combined with it\n":"cub_top: --stream 은 프레임을 표준 출력에 내보내므로 --record 와 함께 쓰지 않습니다\n");
        return 2; }
    /* One output mode per run: the tree, the dump and the live screens all write to stdout. */
    if(tree + terse + live > 1){
        fprintf(stderr,"cub_top: 출력 모드는 하나만 지정합니다 (-t · -d/--dump-json · -b/-p)\n");
        return 2;
    }
    if(g_rep_path && tree){
        fprintf(stderr,"cub_top: --replay 는 트리(-t)를 그리지 않습니다 — -b/-p 또는 -d/--dump-json\n");
        return 2;
    }
    /* A replay shows the recording as it was: options that act on a live server or on this
       run's sampling have nothing to act on, and accepting them silently would make the
       options line claim what did not happen */
    if(g_rep_path){
        char no[200]=""; size_t nl=0;
        #define NOREP(c_,s_) do{ if(c_) nl+=(size_t)snprintf(no+nl,nl<sizeof no?sizeof no-nl:0,"%s%s",nl?", ":"",s_); }while(0)
        NOREP(want_db,"<db>"); NOREP(interval_set,"--interval"); NOREP(g_bcb_dump[0],"--bcb-dump");
        NOREP(g_offs_file[0],"--offsets");
        NOREP(g_pg_detail,"--pgbuf-detail"); NOREP(dump_hist,"--dump-hist"); NOREP(g_rec_overwrite,"--record-overwrite"); NOREP(g_stream,"--stream");
        #undef NOREP
        if(no[0]){
            fprintf(stderr,g_ascii?"cub_top: --replay shows the recording as recorded - not applicable: %s\n"
                                  :"cub_top: --replay 는 기록된 그대로 보여 줍니다 — 적용되지 않는 옵션: %s\n",no);
            return 2; }
    }
    /* An option qualifying another that is absent has nothing to act on */
    if(g_rec_overwrite && !g_rec_path){
        fprintf(stderr,g_ascii?"cub_top: --record-overwrite applies only with --record\n":"cub_top: --record-overwrite 는 --record 와 함께만 씁니다\n");
        return 2; }
    if(speed_set && !g_rep_path){
        fprintf(stderr,g_ascii?"cub_top: --replay-speed applies only with --replay\n":"cub_top: --replay-speed 는 --replay 와 함께만 씁니다\n");
        return 2; }
    /* No mode: the help, as a CLI does.  --bcb-dump alone is an action of its own (the
       snapshot cub_volmap --bufmap reads), so it runs without one. */
    if(!tree && !terse && !live && !g_rep_path){
        if(g_bcb_dump[0]) bcb_only=1;
        else {
            usage_opts();
            if(argc>1){ fprintf(stderr,"cub_top: 출력 모드를 지정하세요 — -t, -d, --dump-json, -b, -p\n"); return 2; }
            return 0;
        }
    }
    auto_lang(force_ko);   /* Detect whether Hangul can be displayed; explicit options win. */
    /* Replay draws only from the recording, so it must run before enum_instances() and
       paramdump touch /proc or the server - a replay works on a host with no CUBRID. */
    if(g_rep_path) return rp_run(g_rep_path,view0,terse,g_json);
    static proc_t P,P0;   /* About 1MB twice including region_t[4096]: static rather than stack */
    enum_instances();
    if(want_db){
        int k=inst_find(want_db);
        if(k<0){
            fprintf(stderr,"cub_top: '%s' 인스턴스가 없습니다.\n",want_db);
            if(g_ninst>0){ fprintf(stderr,"  가동 중:");
                for(int i=0;i<g_ninst;i++) fprintf(stderr,"%s %s",i?",":"",g_inst[i].db);
                fprintf(stderr,"\n"); }
            else fprintf(stderr,"  가동 중인 cub_server 가 없습니다.\n");
            return 2;
        }
        g_cur=k;
    } else g_cur=0;                     /* First alphabetically */
    if(g_ninst>0) target_select(&P,g_cur);
    else { P.pid=-1; snprintf(P.db,sizeof P.db,"-"); g_cur_db[0]=0; }
    /* The recording is opened (and truncated) only after every check that can reject the
       run, so a mistyped command never empties an existing recording; still before the
       first frame, so a path that cannot be written fails the run instead of a capture
       that reports success with nothing written. */
    if(g_rec_path){
        char why[160]; FILE *rf=out_open(g_rec_path,g_rec_overwrite,why,sizeof why);
        if(!rf){ fprintf(stderr,"cub_top: --record %s: %s\n",g_rec_path,why); return 2; }
        RPR.f=rf;
    }
    const char *cub=getenv("CUBRID");
    /* static: the exit handler below reaches it after main has returned */
    static FILE *pd=NULL;
    /* Before the first paramdump: the child is in its own process group, out of reach of
       the terminal's ^C, so a signal here must stop it first.  Every mode, live too - its
       first frame (interval plus the forced method-A walk) runs before tui_enter installs
       the live handlers, which then replace these. */
    signal(SIGINT,on_term_kids); signal(SIGTERM,on_term_kids);
    signal(SIGHUP,on_term_kids); signal(SIGQUIT,on_term_kids);
    signal(SIGABRT,on_term_kids); signal(SIGSEGV,on_term_kids);   /* a crash too */
    signal(SIGBUS,on_term_kids); signal(SIGFPE,on_term_kids);
    signal(SIGXCPU,on_term_kids); signal(SIGUSR1,on_term_kids); signal(SIGUSR2,on_term_kids);
    signal(SIGALRM,on_term_kids); signal(SIGPIPE,on_term_kids);
    /* A file size limit (ulimit -f): write() returns EFBIG instead, so --record and
       --bcb-dump report the failure and the exit status says so */
    signal(SIGXFSZ,SIG_IGN);
    /* Every way out of main - each return, the error paths included - passes exit(), so
       one handler stops a request still waiting instead of an abort before each return */
    g_pd_atexit=&pd; atexit(pd_atexit);
    if(P.pid<0){
        /* Server not running: fill in what is observable rather than stopping */
        memset(&CAP,0,sizeof CAP);
        if(cub && *cub) params_from_conf(cub,P.db);
        if(g_pagesize<=0) g_pagesize=16384;
        snprintf(CAP.note,sizeof CAP.note,g_ascii
                 ?"cub_server not running - only OS, broker and device are observed"
                 :"cub_server 미기동 — OS·브로커·장치만 관측");
    } else {
        cap_probe(P.pid);
        params_preload(P.pid,P.db);       /* cubrid.conf first, as after a restart: a refused or
                                             skipped paramdump otherwise leaves no parameter at all */
        pd=paramdump_start(P.pid,P.db);   /* The child (about 0.03s) overlaps the sample window */
    }

    g_interval=interval;

    static iod_t IO;
    static mem_t M; static tier_t T;
    g_pd_async=live;
    double work_ms=sample_once(&P,&P0,&M,&T,&IO,interval,&pd);

    if(live){
        /* ---- Live dashboard ---- */
        int paused=0,help=0,view=view0;
        g_pd_idle=&pd;
        if(g_stream){
            /* No terminal: the frames go to stdout as the recording format, one per interval,
               until INT/TERM/HUP or the consumer closing the pipe (EPIPE ends the run quietly) */
            g_rec_path="-"; signal(SIGINT,on_sig); signal(SIGTERM,on_sig); signal(SIGHUP,on_sig); signal(SIGPIPE,on_sig);
        } else tui_enter();
        while(!g_stop){
            if(help){
                /* The help does not fit one screen, so it is collected as lines and handed to the pager,
                   with the current mode's key section first (2 time series, 1 dashboard). */
                g_hn=0; g_hcap=1; usage_modal(view?2:1); g_hcap=0;
                help_pager(g_hln,g_hn);
                help=0; g_ustop_n++;            /* nothing was measured meanwhile: a new segment */
                continue;
            }
            /* History accumulates every frame regardless of view, so pressing p draws the
               window collected so far.  Not while paused, though: the pause loop
               returns here every 150ms, so without this guard the series would
               keep scrolling - faster than when running. */
            double fr_t=now_ms()/1000.0;    /* One instant for the history and the recording */
            if(!paused) hist_push(fr_t, &P, &M, &T, &IO);
            if(g_rec_path && !paused){
                char rts[32]; time_t rt=(time_t)fr_t; struct tm tmv;
                localtime_r(&rt,&tmv); strftime(rts,sizeof rts,"%Y-%m-%d %H:%M:%S",&tmv);
                g_ts_frac=fr_t; rp_record(&P,&M,&T,&IO,rts,(long)rt); g_ts_frac=-1;
            }
            if(g_stream){ if(g_rec_err[0]) break; work_ms=sample_once(&P,&P0,&M,&T,&IO,interval,&pd); continue; }
            if(view) render_plot(&P,&M,&T,&IO,paused);
            else     render_dash(&P,&M,&T,&IO,paused,work_ms);
            /* Key handling */
            int k;
            while((k=tui_key())>=0){
                if(k=='q') { g_stop=1; break; }
                else if(k>='1'&&k<='4'){
                    /* Plot zoom: panel 1 (host) is not a zoom target, since it stays on top regardless.
                       So 1 restores the four-panel view and 2-4 zoom that panel (the
                       same key again restores). */
                    if(view) g_plot_zoom = (k=='1' || g_plot_zoom==k-'0') ? 0 : k-'0';
                }
                else if(k==' '){ paused=!paused; if(!paused) g_ustop_n++; }   /* resuming starts a new segment */
                else if(k=='w'){ if(view) g_plot_win=(g_plot_win+1)%3; }      /* live / last hour / 24 h of minute means */
                else if(k=='d') g_pg_detail=!g_pg_detail;   /* Page-level scan of a large buffer, every frame, on request */
                else if(k=='l') g_ascii=!g_ascii;   /* Switch labels between Korean and English, for a tty without Hangul support */
                else if(k=='p'||k=='g'){ view=!view;   /* Dashboard and time-series plots */
                    /* Never retried on its own: a view switch is the user's way to read it again */
                    if(!pd && !g_pd_applied && P.pid>=0) pd=paramdump_start(P.pid,P.db); }
                else if((k=='<'||k==','||k=='>'||k=='.') && g_ninst>1){
                    /* Switch instance.  The previous target's classification would make the dynamic total
                       exclude the wrong region and double-count, so the globals are
                       cleared and tracking starts over. */
                    target_select(&P,(k=='<'||k==',') ? (g_cur+g_ninst-1)%g_ninst : (g_cur+1)%g_ninst);
                    reset_instance_state();
                    cap_probe(P.pid);                         /* Capabilities belong to the target, not the last one */
                    memset(&P0,0,sizeof P0); P0.pid=P.pid;   /* A fresh delta baseline too */
                    HN=0;                                     /* History is per database; mixing them falsifies it */
                    params_preload(P.pid,P.db);               /* Configuration follows the new database too */
                    paramdump_abort(&pd);
                    pd=paramdump_start(P.pid,P.db);
                }
                else if(k=='h') help=1;
            }
            if(g_stop||help) continue;
            /* No frame is sampled while paused; a paramdump still waiting is drained all the same */
            if(paused){ pd_idle_tick(); usleep(150000); continue; }
            work_ms=sample_once(&P,&P0,&M,&T,&IO,interval,&pd);
        }
        if(!g_stream) tui_leave();
        paramdump_abort(&pd);          /* a request still waiting must not outlive the run */
        if(g_mem_fd>=0) close(g_mem_fd);
        if(RPR.f){ FILE *rf=RPR.f; int e=0; RPR.f=NULL;
                   if(fflush(rf)!=0 || ferror(rf)) e=errno?errno:EIO;
                   if(rf!=stdout && fclose(rf)!=0 && !e) e=errno?errno:EIO;
                   if(e) rec_fail(g_ascii?"closing":"닫기",e); }
        if(g_stream && g_rec_err[0] && strstr(g_rec_err,strerror(EPIPE))) return 0;   /* the consumer went away */
        if(g_rec_err[0]){
            fprintf(stderr,g_ascii?"cub_top: --record stopped after %d frame(s) - %s\n"
                                  :"cub_top: --record 가 %d 프레임 뒤 중단됨 — %s\n",RPR.n,g_rec_err);
            return 1; }
        return 0;
    }

    /* Time */
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv);
    char ts[32]; strftime(ts,sizeof ts,"%Y-%m-%d %H:%M:%S",&tmv);
    /* For cross-checking: build one history sample by the same path the screen uses and
       write it to stderr.  tools/check-hist.sh compares it with the terse keys, so
       plots, dashboard and terse are checked against one another. */
    if(dump_hist){ hist_push((double)now_ms()/1000.0,&P,&M,&T,&IO); hist_dump_last(); }
    /* --json captures the terse output to a temporary file and converts it, so the
       content is never produced twice.  stdout is redirected at fd level, which does
       not rely on the stdio implementation. */
    FILE *jf=NULL; int jsave=-1;
    if(terse && g_json){
        /* A consumer expecting JSON must not get key=value or nothing with exit 0. */
        fflush(stdout);
        jf=mem_file();
        if(jf) jsave=dup(1);
        if(!jf || jsave<0 || dup2(fileno(jf),1)<0){
            fprintf(stderr,"cub_top: --dump-json 임시 출력 준비 실패: %s\n",strerror(errno));
            return 1;
        }
    }
    if(bcb_only){
        /* Success only when this run replaced the file; anything else leaves no new snapshot */
        if(PG.ok && g_bcb_saved){ printf("bcb snapshot: %s (%d pages resident)\n",g_bcb_dump,PG.nrec); return 0; }
        fprintf(stderr,"cub_top: --bcb-dump %s 를 쓰지 못했습니다 — %s\n",g_bcb_dump,
                !PG.ok ? (PG.why[0]?PG.why:"버퍼풀 상태 읽기 불가") : (g_bcb_why[0]?g_bcb_why:"저장되지 않음"));
        return 1;
    }
    if(terse){ char ou[256]; opts_used_str(argc,argv,ou,sizeof ou);
               printf("opts.used=\"%s\"\n",tq(ou)); }
    else    { print_opts_line(argc,argv); putchar('\n'); }   /* Blank line between the options and the body */
    if(terse){
        render_terse(&P,&M,&T,IO.rb,IO.wb,IO.ri,IO.wi,
                     IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,ts,(long)now,
                     IO.rchar,IO.minflt,IO.majflt,IO.cpu_pct,IO.ncpu,
                     IO.d_thr,IO.n_thr);
        /* This block must stay inside the terse branch: outside it, the else below binds to
           if(jf) and plain -d prints the tree as well. */
        if(jf){ fflush(stdout);
                if(dup2(jsave,1)<0){ fprintf(stderr,"cub_top: --dump-json 출력 복원 실패: %s\n",strerror(errno)); return 1; }
                close(jsave);
                json_from_terse(jf,stdout); fclose(jf);
                if(fflush(stdout)!=0 || ferror(stdout)){ fprintf(stderr,"cub_top: --dump-json 출력 쓰기 실패: %s\n",strerror(errno)); return 1; } }
    }
    else {
        /* The tree prints every instance in turn; one per invocation would mean running the
           command repeatedly to see the other databases.  A named DB prints only that
           one.  Each instance is collected afresh at its own instant. */
        int only = want_db ? g_cur : -1;
        for(int ii=0; ii<(g_ninst>0?g_ninst:1); ii++){
            if(only>=0 && ii!=only) continue;
            if(g_ninst>0 && ii!=g_cur){        /* Switch target, the same procedure as live < and > */
                target_select(&P,ii);
                reset_instance_state();
                cap_probe(P.pid);
                memset(&P0,0,sizeof P0); P0.pid=P.pid;
                params_preload(P.pid,P.db);
                paramdump_abort(&pd);
                pd=paramdump_start(P.pid,P.db);
                work_ms=sample_once(&P,&P0,&M,&T,&IO,interval,&pd);
            }
            if(ii>0 || only<0) printf("[@%s]\n",P.db[0]?P.db:"-");
            render_tree(&P,&M,&T,IO.rb,IO.wb,IO.ri,IO.wi,
                        IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,ts,
                        IO.rchar,work_ms,IO.cpu_pct,IO.ncpu);
            if(g_ninst>1 && ii<g_ninst-1){
                putchar('\n');
                for(int c=0;c<70;c++) putchar('-');
                putchar('\n');
            }
        }
    }
    paramdump_abort(&pd);   /* One a restart started during the run */
    if(g_mem_fd>=0) close(g_mem_fd);
    return 0;
}
