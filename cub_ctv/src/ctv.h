/*
 * Copyright 2008 Search Solution Corporation
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
 * ctv.h - shared types, constants, globals and prototypes of cub_ctv.
 * Invariant ids (I-0..I-20) refer to the ledger in PR.md.
 */

#ifndef _CTV_H_
#define _CTV_H_

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SERVERS_MAX        1024	/* cub_server candidates listed */

#define MAX_CS             64	/* upper bound while detecting the CS array */

#define CS_MIN_SAMPLES     4	/* descriptors a sequence probe must agree on */

#define NAME_MAX_LEN       64	/* CS / primitive name buffer */

#define IDENT_LEN          64	/* one CLIENTIDS string buffer */

#define TAG_LEN            1024	/* owner / wait-chain text buffers */
#define TAG_TAIL_RESERVE   48	/* room kept for the closing "[n of W waiting]" */

#define PID_MAX_LIMIT      4194304	/* kernel pid_max upper bound (2^22) */

#define TRAN_SLOTS_MAX     65536	/* descriptor count sanity cap */

#define POOL_MAX_CHUNKS    1024	/* sync_Stats chain hops */

#define POOL_MAX_ENTRIES   65536	/* sync_Stats entries visited */

#define POOL_MAX_GROUPS    1024	/* distinct (type, name) groups shown */

#define LOG_GL_SCAN_MAX    65536	/* bytes of log_Gl scanned for all_tdes */

#define TDES_HEAD_SCAN     512	/* bytes of a TDES scanned for tran_index */

#define TDES_SCAN_MAX      32768	/* plausible TDES spacing upper bound */
#define TDES_SCAN_FALLBACK 8192	/* scan bound when the spacing is not one */

#define NWITNESS           3	/* descriptors cross-validating a geometry */

#define LAZY_LOCK_BUDGET   16	/* descriptors per lazy geometry retry (no trid) */
#define LAZY_LOCK_TRIES    4	/* new transactions tried per lazy retry */
#define LAZY_RETRY_S       10	/* a descriptor that failed is tried again after */

#define LIVE_TRANS_ROWS    24	/* session rows on the summary screen (fewer on a short terminal) */

#define STATS_SIZE         64

#define ST_OFF_TYPE        0

#define ST_OFF_NAME        8

#define ST_OFF_NENTER      16

#define ST_OFF_NWAIT       20

#define ST_OFF_NREENTER    24

#define ST_OFF_TOTAL_TV    32

#define ST_OFF_MAX_TV      48

#define ST_TYPE_CSECT      1

#define ST_TYPE_MUTEX      4

#define ST_TYPE_OK(t)      ((t) >= ST_TYPE_CSECT && (t) <= ST_TYPE_MUTEX)

#define CS_OFF_NAME        0	/* const char *name */

#define CS_OFF_INDEX       8	/* int cs_index (I-1 anchor) */

#define CS_PAIR_RETRIES    3	/* I-4 re-reads before refusing */
#define CS_STATS_TO_RWLOCK 96	/* cond48 + queues16 + owner8 + tran/pad8 + trio/pad16 */

#define CS_WAIT_SANE_MAX   1000000	/* waiting counters of a live server */

#define CHUNK_TAIL         16

#define CHUNK_BYTES(n)     ((size_t) (n) * STATS_SIZE + CHUNK_TAIL)

#define CHUNK_OFF_NEXT(n)  ((size_t) (n) * STATS_SIZE)

#define CHUNK_OFF_INUSE(n) ((size_t) (n) * STATS_SIZE + 12)

#define COW_STRIDE         8	/* old ABI: char* into heap, _Rep before it */

#define SSO_STRIDE         32	/* new ABI: {ptr, size, buf16|cap} */

#define COW_REP_LEN        24	/* _Rep{len, cap, refcount} precedes data */

#define SSO_LOCAL_OFF      16	/* local buffer offset inside the object */

#define SSO_LOCAL_MAX      15	/* longest string kept in the local buffer */

#define STR_LEN_MAX        255

#define STR_CAP_MAX        4096

#define STR_REF_MAX        65536

#define CLIENTIDS_NSTR     6	/* client_info .. client_ip_addr */

#define CLIENTIDS_BYTES(s) ((s) * CLIENTIDS_NSTR + 4)

#define CS_STATS_TO_OWNER  16	/* thread_id_t owner (writer) */

#define CS_STATS_TO_WQ     32	/* THREAD_ENTRY *waiting_writers_queue */
#define CS_STATS_TO_PQ     24	/* THREAD_ENTRY *waiting_promoters_queue */

#define THR_STRIDE_MIN     256	/* sizeof (entry) search range */

#define THR_STRIDE_MAX     65536

#define THR_INDEX_SCAN     2048	/* bytes of an entry scanned for 'index' */

#define THR_MAX            65536	/* entries cap */

#define THR_TRAN_REL_MAX   28	/* tran_index: index+28 (3 ints, type, tid, client_id) or +20 (older, 1 int) */

#define THR_NEXT_SPAN      64	/* bytes after resume_status searched for next_wait_thrd (I-19) */
#define THR_NEXT_CANDS     8

#define THR_ID_SLACK       8	/* threads outside the entry pool tolerated */

#define THR_WAITERS_MAX    64	/* wait-chain hops followed (cycle guard) */
#define THR_TID_SCAN       1024	/* bytes of struct pthread scanned for the OS tid (I-17) */
#define THR_RESUME_MAX     28	/* thread_resume_suspend_status: values 0..27 */
#define THR_RESUME_CS_MIN  7	/* CSECT_READER_SUSPENDED: a queued CS waiter reads 7..11 */
#define THR_RESUME_CS_MAX  12	/* ..CSECT_PROMOTER_RESUMED: a waiter read a moment
					 * after the queue sample may already be resumed */
#define VERDICT_HISTORY    10	/* windows behind the persistence line */
#define CHAIN_REWALK_S   0.25	/* re-walk of a queue under the same head */
#define OS_SAMPLE_MIN_S  0.05	/* procfs state sampling cap (20 Hz per thread) */
#define OS_MAX             64	/* threads with OS state kept per frame */
#define OS_COMM_LEN        16
#define LAYOUT_AUTO        0	/* split owner block when the row overflows */
#define LAYOUT_WIDE        1	/* always one line */
#define LAYOUT_NARROW      2	/* always split */

#define DEG_TRID      0x1	/* I-8: trid column and released-slot filter */
#define DEG_TRAN_CAP  0x2	/* descriptor table larger than TRAN_SLOTS_MAX */
#define DEG_THREAD    0x4	/* I-14: thread entry table unavailable */

#define DEG_THR_TRAN  0x8	/* I-15: entry.tran_index not validated */

#define DEG_THR_ID    0x10	/* I-16: entry.m_id not validated */

#define DEG_TRAN      0x20	/* I-6..I-7: transaction table unavailable */
#define DEG_THR_TID   0x40	/* I-17: OS tid offset not validated */
#define DEG_THR_RESUME 0x80	/* I-18: entry.resume_status not validated */
#define DEG_THR_NEXT  0x100	/* I-19: entry.next_wait_thrd not validated */
#define DEG_THR_STATUS 0x200	/* I-20: entry.m_status not validated */

#define M(en, ko) (use_ko ? (ko) : (en))

#define REFUSE(id) return (id)

typedef struct
{
  int pid;
  char db[128];
} SERVER;

enum
{
  ELF_OK = 0,
  ELF_NOFILE = -1,		/* cannot open the library on disk */
  ELF_BAD = -2,			/* not a sane ELF64 image (truncated/corrupt) */
  ELF_NOSYM = -3		/* readable, but the symbol is absent */
};

enum
{
  LIB_OK = 0,
  LIB_NONE = -1,		/* libcubrid.so not mapped */
  LIB_DELETED = -2,		/* mapped only as "(deleted)": replaced on disk */
  LIB_NOMAPS = -3		/* /proc/<pid>/maps not readable (errno set) */
};

#define VALID_PTR(p) ((p) > 0x1000 && (p) < 0x800000000000ULL)

typedef struct
{
  int32_t type;
  uint64_t name_ptr;
  uint32_t nenter, nwait, nreenter;
  double total_ms, max_ms;
} STATS_VIEW;

typedef struct
{
  int stride;			/* sizeof (SYNC_CRITICAL_SECTION) */
  int count;			/* number of static critical sections */
  int off_stats;		/* offset of SYNC_STATS *stats */
  int off_tran;			/* offset of tran_index (stats - 8) */
  int off_rwlock;		/* offset of rwlock/waiting trio */
  int off_owner;		/* offset of the writer thread id (stats - 16) */
  int off_wq;			/* offset of waiting_writers_queue (stats - 32) */
  int off_pq;			/* offset of waiting_promoters_queue (stats - 24) */
} CS_LAYOUT;

typedef struct
{
  char name[NAME_MAX_LEN];
  int32_t rwlock;
  uint32_t wr, ww;
  int32_t tran;
  uint64_t owner;		/* writer thread id (pthread_t), 0 if none */
  uint64_t wq;			/* first waiter queue head (promoters, else
				 * writers), 0 if none */
  uint64_t wq2;			/* writers queue when promoters are ahead of
				 * it (live only; a record keeps the walk) */
  uint32_t nenter, nwait, nreenter;
  double total_ms, max_ms;
  int has_stats;
} CS_ROW;

/* per-CS figures of one window */
typedef struct
{
  int ix;
  double occ_pct, wait_pct, enter_s, wait_s, avg_ms, lost_s;
  int severity;			/* 0 idle .. 4 */
} CS_FIG;

typedef struct
{
  uint64_t all_tdes;		/* address of the TDES pointer array */
  int off_ti;			/* offset of tran_index inside a TDES */
  int cid_off;			/* offset of the CLIENTIDS string run */
  int cid_stride;		/* COW_STRIDE / SSO_STRIDE, 0 = not locked */
  int max_scan;			/* per-TDES scan bound (tdes spacing) */
  int scan_measured;		/* max_scan is the measured spacing */
  int has_trid;			/* I-8 held: trid@off_ti+4 is trusted */
  int ntran;			/* descriptor count (fixed after boot) */
} TRAN_GEOM;

typedef struct
{
  char info[IDENT_LEN], user[IDENT_LEN], program[IDENT_LEN],
    login[IDENT_LEN], host[IDENT_LEN], ip[IDENT_LEN];
  int cpid;
} TRAN_IDENT;

typedef struct
{
  uint64_t base;		/* m_all_entries, 0 = unavailable */
  int stride;			/* sizeof (cubthread::entry) */
  int count;			/* entries (max threads) */
  int off_index;		/* int index */
  int off_id;			/* thread_id_t m_id (probed, I-16); 0 = unknown */
  int has_tran;			/* off_index + off_tran validated (I-15) */
  int off_tran;			/* tran_index relative to index: 28 or 20 */
  int has_id;			/* m_id offset found */
  int off_tid;			/* OS tid inside struct pthread (I-17) */
  int has_tid;
  int off_resume;		/* resume_status inside the entry (I-18) */
  int has_resume;
  int off_next;			/* next_wait_thrd (I-19); -1 = candidates only */
  int has_next;
  int off_status;		/* m_status (I-20): 3 = TS_WAIT while suspended */
  int has_status;
} THR_GEOM;

#define THR_INDEX(slot) ((slot) + 1)

typedef struct
{
  int type;
  char name[NAME_MAX_LEN];
  int cnt;
  uint64_t nenter;
  double max_ms;
} POOL_GROUP;

/* CS activity seen during a live window: a tran holding CS 'cs' as writer
 * or waiting for it, with the number of samples it was seen in */
#define ACT_MAX            4096
#define ACT_HELD_RESERVE   256	/* of ACT_MAX, kept for holder sightings */
#define ACT_HASH           16384	/* power of two > 2 * ACT_MAX */
#define ACT_HELD           0
#define ACT_WAIT           1
typedef struct
{
  int cs, kind, tran, thr, samples;
  int cpid;			/* client pid of tran when seen, -1 unknown */
} ACT;

/* OS state of one engine thread over a window: scheduler state samples
 * (running / disk sleep / other) and CPU ticks at first and last sighting */
typedef struct
{
  int slot, tid, samples, run, dsk, oth;
  long ticks0, ticks1;
  double t_first, t_last;	/* first / last procfs sample (mono s) */
  int span_ms;			/* t_last - t_first: what the tick delta covers */
  char comm[OS_COMM_LEN];
} OS_THR;

/* what one live window saw, shared by the sampler (LIVE_WIN) and the frame
 * (VIEW_FRAME): the frame takes it whole */
typedef struct
{
  int samples;
  int occ[MAX_CS], wai[MAX_CS];	/* samples seen held / with waiters */
  int seen_tran[MAX_CS];	/* writer tran seen during the window */
  int seen_cpid[MAX_CS];	/* its client pid when seen, -1 unknown */
  uint64_t seen_owner[MAX_CS];	/* writer thread id seen during the window */
  uint64_t seen_wq[MAX_CS];	/* the window's first waiter-queue head */
  int seen_n[MAX_CS];		/* its chain at sighting, -1 = none */
  int seen_slot[MAX_CS][THR_WAITERS_MAX], seen_tran_of[MAX_CS][THR_WAITERS_MAX];
  int seen_cpid_of[MAX_CS][THR_WAITERS_MAX];	/* client pid of each hop */
  ACT act[ACT_MAX];		/* who held / waited on which CS, how often */
  int nact;
  int act_dropped;		/* sightings not kept: table full */
} WIN_STATS;

/* sampler state of one live window; the renderers read VIEW_FRAME only */
typedef struct
{
  CS_ROW prev[MAX_CS], cur[MAX_CS];	/* rows at window start / end */
  WIN_STATS st;			/* what the window saw (copied to the frame) */
  uint64_t wq_last[MAX_CS];	/* queue head of the last sample */
  int ch_n[MAX_CS];		/* the queue under that head: slots and trans */
  int ch_slot[MAX_CS][THR_WAITERS_MAX], ch_tran[MAX_CS][THR_WAITERS_MAX];
  int ch_cpid[MAX_CS][THR_WAITERS_MAX];	/* client pid of each hop at the walk */
  double ch_walked[MAX_CS];	/* when it was last walked (mono s) */
  uint64_t own_last[MAX_CS];	/* owner tid of the last sample */
  int own_slot[MAX_CS];		/* its resolved entry slot */
  unsigned char *blob;		/* raw array image of the latest sample */
  unsigned short act_ix[ACT_HASH];	/* (cs, kind, tran) -> st.act index + 1 */
  OS_THR os[OS_MAX];		/* writer threads sampled in the window */
  int nos;
  uint64_t dk0[3];		/* diskstats at window start: wr_ios, wr_ms, io_ms */
  int dk_ok;
  int no_disk;			/* nothing shows or records disk figures */
  uint64_t cs_base;		/* the CS array (queue heads are re-checked there) */
} LIVE_WIN;

/* one rendered moment (a snapshot, or one live window): every answer the
 * renderers need, captured at once.  Live runs and replays render from it. */
#define VIEW_MAX_TIDS      (2 * MAX_CS)
#define VIEW_MAX_CHAINS    (2 * MAX_CS)
#define VIEW_MAX_SLOTS     (VIEW_MAX_CHAINS * THR_WAITERS_MAX)

typedef struct
{
  int idx;
  int32_t trid;
  TRAN_IDENT id;
} VIEW_IDENT;

typedef struct
{
  uint64_t head;
  int n;
  int slot[THR_WAITERS_MAX];
} VIEW_CHAIN;

typedef struct
{
  long ts;			/* wall clock of the moment */
  long seq;			/* frame ordinal (capture count, or record index) */
  int count;			/* CS rows */
  CS_ROW rows[MAX_CS];
  /* live window (st.samples == 0 for a snapshot) */
  double dt;
  WIN_STATS st;
  CS_ROW prev[MAX_CS];		/* rows at window start: rows with the
				 * window-start nenter / nwait / total_ms */
  /* transactions */
  int tran_avail, cid_locked, has_trid, ntran;
  VIEW_IDENT *ident;
  int nident, ident_cap;
  VIEW_IDENT *gone;		/* clients seen in the window that left their slot,
				 * as last identified (idx = tran, id.cpid = pid) */
  int ngone, gone_cap;
  /* threads */
  uint64_t tid[VIEW_MAX_TIDS];
  int tid_slot[VIEW_MAX_TIDS], ntid;
  VIEW_CHAIN chain[VIEW_MAX_CHAINS];
  int nchain;
  int slot[VIEW_MAX_SLOTS], slot_tran[VIEW_MAX_SLOTS], slot_cpid[VIEW_MAX_SLOTS], nslot;
  /* OS side: thread names / states, log device, wait reasons */
  OS_THR os[OS_MAX];
  int nos;
  int disk_avail;
  double disk_await_ms, disk_util_pct;
  char disk_dev[32];
  int wait_avail, wait_live, wait_cnt[THR_RESUME_MAX];
  /* pool: 1 collected, 0 not collected, -1 did not validate */
  int pool_state, npool, pool_truncated;
  POOL_GROUP pool[POOL_MAX_GROUPS];
  unsigned degraded;		/* degraded bits at this moment */
  /* the verdict's top CS of the windows before this one (back 1..9,
   * top -1 = idle), so a replay starts with the history live had */
  int hist_known, hist_n;
  int hist_back[VERDICT_HISTORY], hist_top[VERDICT_HISTORY];
} VIEW_FRAME;

/* replay file header: what main() otherwise learns from /proc and ELF */
typedef struct
{
  int pid, live, hz, thr_count;
  double interval;
  char db[128], lib[4096], tran_refused[64];
  unsigned degraded;
  CS_LAYOUT lo;
} VIEW_HDR;

#define REC_MAGIC          "CTVREC 5"
#define INTERVAL_MIN       0.1	/* shortest --interval (s): one capture each */
#define INTERVAL_MAX       3600.0	/* longest --interval (s) */
#define REC_SAMPLES_MAX    36010000	/* samples of a window: --hz max x
					 * (--interval max + 1 s) */
#define REC_KEYFRAME       60	/* every Nth frame carries every identity */
#define REC_DT_MAX         1e7	/* longest window a record may claim (s); a
				 * suspended live run makes windows longer
				 * than --interval */
#define REC_TOKENS_MAX     (4 + THR_WAITERS_MAX)	/* longest line: Q head n slots... */
#define POOL_GROUPS_CAPPED (-2)	/* pool_truncated: chain read whole, groups full */
#define POOL_NO_MEMORY     (-3)	/* pool_truncated: no buffer here, nothing read */
#define POOL_CHAIN_LOOP    (-4)	/* pool_truncated: a chunk came round again */
#define DETAIL_MAGIC       "cub_ctv detail dump"

typedef struct
{
  const char *target, *dump, *record, *replay, *detail_filter;
  int live, trans, json, hz, plain;
  double interval, replay_speed;
  int list;
  int pid;			/* --pid N: that cub_server only (0: none) */
} OPTS;

/* ---- globals ---- */
extern int layout_mode;
extern int layout_detail;
extern const char *detail_filter;

/* per-window rates of one CS (u32 wrap-safe counter deltas) */
typedef struct
{
  double enter_s, wait_s, occ_pct, wait_pct;
  uint32_t de, dw;		/* raw deltas */
} CS_RATES;
extern int color_on;		/* ANSI colors: tty only, off with --plain */
extern const char *live_footer;	/* screen footer; NULL = "[q] quit" */
extern TRAN_GEOM tran_geom;
extern THR_GEOM thr_geom;
extern int thr_keep_image;
extern const char *tran_refused;
extern int use_ko;
extern int ko_ok;
void lang_init (int argc, char **argv);
const char *lang_key (void);
extern unsigned degraded;

typedef struct
{
  unsigned bit;
  const char *id, *label;
} DEG_NAME;
extern const DEG_NAME deg_names[];	/* terminated by bit 0 */
#define DEG_THR_SUB (DEG_THR_TRAN | DEG_THR_ID | DEG_THR_TID | DEG_THR_RESUME | DEG_THR_NEXT | DEG_THR_STATUS)
extern VIEW_FRAME view;

#define FNV_SEED 14695981039346656037ULL
extern int target_pid;
#define TRAN_TEXT_SHORT 0
#define TRAN_TEXT_LABEL 1

/* color roles (clr) */
enum
{
  CLR_OFF, CLR_HEAD, CLR_WRITER, CLR_WAIT, CLR_OWNER, CLR_WQ, CLR_MUTED,
  CLR_BAR_LOW, CLR_BAR_MID, CLR_BAR_HIGH, CLR_PANEL_OWNER, CLR_PANEL_WAITER
};
/* --trans without a record (or dump): the transaction table alone, no
 * sampling and no thread entries; main decides it once (thr_skipped) */
#define TRANS_TABLE_ONLY(trans, record) ((trans) && (record) == NULL)

/* key codes from key_read: sentinels outside the byte range, so no key
 * byte can be read as one */
#define KEY_IGNORED 256	/* a key with no action: redraw, do not advance */
#define KEY_EOF 257	/* stdin closed or failed: no key will come */
#define KEY_RESIZE 258	/* the terminal was resized: redraw */
#define KEY_BACK 259	/* one window back: left arrow, <, ',', b */
#define KEY_FWD 260	/* one window on: right arrow, >, '.', n */

/* ---------------------------------------------- field state (ctv_field.c) */
#define FIELD_NONE 0		/* not known */
#define FIELD_WEAK 1		/* provisional rule only; re-probed */
#define FIELD_SET 2		/* settled */
#define FIELD_REJECTS 8
typedef struct
{
  int state, off;
  int hits, misses;		/* runtime anchor sightings */
  int rejected[FIELD_REJECTS], nrejected;
} FIELD;

/* ------------------------------------------------ retry timing (ctv_sched.c) */
#define SCHED_FIRST 2		/* s: first gap */
#define SCHED_FAST_MAX 64	/* s: the doubling stops here */
#define SCHED_FAST_ROUNDS 6	/* gaps 2,4,..,64,64 s: about three minutes */
#define SCHED_SLOW 300		/* s: then one try per 5 minutes */
typedef struct
{
  time_t at, last;
  int gap, rounds;
} SCHED;

/* --------------------------------------------- the verdict (ctv_verdict.c) */
#define VERDICT_WAITERS_LISTED 4
/* what holds the top CS back: from the holder thread's states and the log
 * device */
#define VB_NONE 0		/* neither thread states nor the device known */
#define VB_DISK 1
#define VB_CPU 2
#define VB_SHORT 3		/* neither saturated: short holds, high rate */
#define VB_UNKNOWN 4		/* the device known, the thread not */
typedef struct
{
  CS_FIG top, second;
  int has_second;		/* a second CS also waiting */
  int same, span;		/* persistence: windows with this top / known */
  int holder;			/* -2 none (readers or not seen), 0 engine, >0 tran */
  int holder_cpid;		/* client pid seen with it, -1 unknown */
  int one_thr;			/* an engine holder named by one thread */
  double held_pct, thr_pct;	/* the holder's share; that one thread's own */
  int readers_most;		/* a holder, but readers occupy most of it */
  int nwaiters;			/* client sessions seen waiting on it */
  double waited_pct;		/* the share of the window each waited (mean) */
  int wtran[VERDICT_WAITERS_LISTED], wsamples[VERDICT_WAITERS_LISTED];
  int nlisted, more_waiters;
  const OS_THR *os;		/* the holder thread most seen, or NULL */
  double dsk_pct, run_pct, cpu_pct;
  int bound;			/* VB_* */
} VERDICT;

/* thread entry module internals (ctv_thr.c, ctv_thr_probe.c) */
#define THR_ID_CANDS 64
#define THR_TID_SAMPLE 32
#define TS_WAIT 3		/* cubthread::entry::status while suspended */

/* ---- prototypes, by module ---- */

/* ctv_cs.c: critical sections - the CS array layout probe (I-0..I-5), its rows and wait queues */
int stats_decode (const unsigned char *s, STATS_VIEW * v);
uint64_t cs_wait_queues (const unsigned char *blob, const CS_LAYOUT * lo, int i, uint64_t * head2, int *field);
const char *cs_layout_probe (uint64_t cs_base, CS_LAYOUT * lo);
void cs_decode_raw (const unsigned char *blob, const CS_LAYOUT * lo, int i, int32_t * rwlock, uint32_t * wr, uint32_t * ww, int32_t * tran);
int cs_read_rows (uint64_t cs_base, const CS_LAYOUT * lo, CS_ROW * rows);
int cs_rows_from_blob (const unsigned char *blob, const CS_LAYOUT * lo, CS_ROW * rows);
const char *state_str (int32_t rwlock, char *buf, size_t n);
int cs_held (int32_t rwlock, uint64_t owner);

/* ctv_dump.c: detail dump - everything read at one moment as one JSON document (not replayable) */
void detail_dump_write (const char *path, const VIEW_HDR * h, uint64_t bias, uint64_t sym_cs, uint64_t sym_pool, uint64_t sym_loggl, uint64_t sym_thr);

/* ctv_elf.c: ELF - the mapped libcubrid.so: its base in the target and symbol lookups */
int elf_lookup (const char *lib, const char *want, uint64_t * value, uint64_t * load0_vaddr);
int find_lib_base (int pid, uint64_t * base, char *path, size_t pathn, char *range, size_t rangen);

/* ctv_text.c: text - UTF-8 lengths, terminal columns, cutting and padding by column, sanitizing outside strings, hashing */
int utf8_valid_len (const char *s);
int seq_cols (const char *s, int *len);
size_t utf8_cut (const char *s, size_t ln);
uint64_t fnv (uint64_t h, const void *p, size_t n);
int str_cols (const char *s);
void text_sanitize (char *s);
int utf8_cols (const char *s, int len);
int is_printable_text (const char *s);
size_t dw_pad (char *out, size_t n, const char *s, int width);
void buf_cat (char *buf, size_t n, size_t * pos, const char *fmt, ...)
  __attribute__ ((format (printf, 4, 5)));

/* ctv_field.c: field - the state of one probed field (an invariant's offset): not known, provisional (weak rule) or settled; the runtime anchor's tally; the offsets the anchor rejected.  Every change of a field's state goes through these functions. */
void field_lock (FIELD * f, int off, int weak);
void field_clear (FIELD * f);
void field_reject (FIELD * f);
int field_skips (const FIELD * f, int off, int ignore);
int field_sight (FIELD * f, int ok);
int field_probe (FIELD * f, int (*pass) (const FIELD *, const unsigned char *, int, int *), const unsigned char *img);

/* ctv_sched.c: sched - when to retry something costly: fast rounds with a doubling gap, then a slow fixed gap; a restart returns to the fast rounds, and a change can bring the next try forward (rate-limited).  Also a plain minimum interval gate. */
void sched_start (SCHED * s, time_t now);
int sched_due (const SCHED * s, time_t now);
void sched_ran (SCHED * s, time_t now);
void sched_restart (SCHED * s);
int sched_slow (const SCHED * s);
void sched_sooner (SCHED * s, int min_gap);
int every (double *last, double now, double gap);

/* ctv_mem.c: target memory - the only reader of the server's memory (pread, gather, strings, names) */
int mem_open (int pid);
int mem_read (uint64_t addr, void *buf, size_t n);
int mem_gather_ok (const uint64_t * addrs, size_t n, size_t each, void *buf, unsigned char *ok);
int mem_gather_field (const uint64_t * bases, size_t n, uint64_t off, size_t width, const unsigned char *skip, void *out, unsigned char *ok);
size_t mem_read_shrink (uint64_t addr, void *buf, size_t max, size_t min);
int mem_read_ptr (uint64_t addr, uint64_t * p);
int mem_cstr (uint64_t addr, char *out, size_t outn);
const char *name_of (uint64_t ptr, char *out, size_t n);
uint64_t rd_u64 (const unsigned char *b, size_t off);
int32_t rd_i32 (const unsigned char *b, size_t off);
uint32_t rd_u32 (const unsigned char *b, size_t off);
int64_t rd_i64 (const unsigned char *b, size_t off);

/* ctv_msg.c: messages - Korean/English text, die/warn, degraded features (the only writer of their bits) */
void die (const char *fmt_en, const char *fmt_ko, const char *arg)
  __attribute__ ((noreturn));
void warn (const char *en, const char *ko);
void warnf (const char *fmt_en, const char *fmt_ko, ...);
void deg_set (unsigned bits, int on);

/* ctv_os.c: OS view - procfs: tasks, thread names and states, clock ticks, the log device */
int cmp_int (const void *a, const void *b);
int os_task_list (int pid, int **out);
int os_task_known (const int *tasks, int n, int tid);
int read_small (const char *path, char *buf, size_t n);
int os_task_comm (int pid, int tid, char *out, size_t n);
int os_task_state (int pid, int tid, char *state, long *ticks);
long os_clock_ticks (void);
int os_log_device (int pid, unsigned *major_out, unsigned *minor_out, char *name, size_t n);
int os_diskstats (unsigned major_n, unsigned minor_n, uint64_t * wr_ios, uint64_t * wr_ms, uint64_t * io_ms, char *name, size_t namen);

/* ctv_pool.c: sync stats pool - the chain of per-primitive stats, grouped by type and name */
void pool_print_groups (const POOL_GROUP * g, int ng, int truncated);
int pool_collect (uint64_t pool_addr, POOL_GROUP * g, int maxg, int *ng, int *truncated);
int pool_dump (FILE * f, uint64_t pool_addr);

/* ctv_proc.c: servers - running cub_server processes from procfs */
int server_by_pid (int pid, SERVER * out);
int list_servers (SERVER * out, int max);

/* ctv_screen.c: summary screen - the full-screen view of a frame, fitted to the terminal */
void print_thread_waits (FILE * out, int screen);
void screen_summary (const CS_LAYOUT * lo, double dt, int hz, double interval, int pid, const char *db, const char *hhmmss);

/* ctv_verdict.c: verdict - the figures behind a summary screen: per-CS rates, losses and severity, the holder (session or engine thread) of each CS, the persistence history, the verdict itself (VERDICT), and the CS descriptions and hints */
const char *cs_desc (const char *name);
const char *cs_hint (const char *name);
void cs_rates (const CS_ROW * c, const CS_ROW * p, int occ, int wai, int samples, double dt, CS_RATES * r);
void cs_figures (int i, double dt, CS_FIG * f);
const char *daemon_name (int i);
int holder_thread_of (int i, int *samples);
void cs_figures_ranked (int count, double dt, CS_FIG * fig);
int holder_of (int i, int *samples, int *cpid);
int held_minor (int i, int samples);
void history_push (long seq, int top_ix);
int history_export (long seq, int *back, int *top);
void history_import (long seq, int n, const int *back, const int *top);
int history_count (long seq, int top_ix, int *span);
void verdict_compute (const CS_FIG * top, const CS_FIG * second, VERDICT * v);

/* ctv_thr_probe.c: thread entry probes - read-only searches of the target and of an entry-array image: where the array is (I-14, recorded in thr_geom), and the candidate offsets of its fields (I-15 .. I-20); no FIELD state is changed here - ctv_thr.c locks what they find */
const char *thr_probe_entries (uint64_t manager_obj);
int slot_of_ptr (uint64_t p);
int thr_index_of_ptr (uint64_t ptr);
unsigned char *thr_image (void);
int thr_find_tran (const unsigned char *img);
int thr_id_candidates (int nthr, const unsigned char *img, int *cand, int *strict);
int thr_probe_tid_at (const int *tasks, int ntask, const unsigned char *img, int id_off, int *tid_off);
int resume_suspended (int v);
int thr_probe_resume_pass (const FIELD * f, const unsigned char *img, int ignore, int *provisional);
int thr_probe_status_pass (const FIELD * f, const unsigned char *img, int ignore, int *provisional);
int buf_cs_writer_waiting (const unsigned char *buf, long base);
int thr_next_static (const unsigned char *img, int off);

/* ctv_thr.c: thread entries - the entry array, its probed fields' life, m_id/tid tables, wait chains */
int thr_slot_live (int k);
int thr_tid_of (int slot);
int thr_wait_counts (int *cnt, int *live, int reader_cap);
void thr_probe (uint64_t manager_sym, int pid);
void thr_cache_refresh (void);
int thr_index_of_tid_mem (uint64_t tid);
int thr_waiters_mem (uint64_t head, int *idx, int *tran, int max, uint64_t anchor_at);
int thr_waiters_mem2 (uint64_t head, uint64_t head2, int *idx, int *tran, int max, uint64_t anchor_at);
void thr_dump (FILE * f);

/* ctv_tran.c: transactions - TDES and CLIENTIDS probes (I-6..I-11), identities and their cache */
const char *tran_probe (uint64_t log_gl);
int tran_lock_clientids (int ntran, int budget);
void tran_geom_refresh (void);
void tran_identity_prefetch (void);
int tran_client_pid (int tran_index);
int tran_identity_cached (int tran_index, TRAN_IDENT * id, int32_t * trid);
void tran_dump (FILE * f);

/* ctv_frame.c: frame - one screen's answers captured at one moment (VIEW_FRAME), and the queries the renderers ask of it */
void view_disk_begin (LIVE_WIN * w);
void view_add_gone (int idx, int32_t trid, const TRAN_IDENT * id);
void view_add_ident (int idx, int32_t trid, const TRAN_IDENT * id);
void view_capture_trans (double dt);
void win_stats_clear (WIN_STATS * st);
void view_capture (const CS_LAYOUT * lo, const CS_ROW * rows, const LIVE_WIN * w, double dt);
void view_capture_pool (uint64_t pool_addr);
const OS_THR *view_os_of (int slot);
const char *resume_name (int v);
int view_identity (int tran_index, TRAN_IDENT * id, int32_t * trid);
int view_identity_seen (int tran, int cpid, TRAN_IDENT * id, int32_t * trid);
int view_slot_of_tid (uint64_t tid);
int view_tran_cpid_of (int slot);
int view_tran_of (int slot);
int view_waiters (uint64_t head, int *idx, int max);
int view_seen_chain (int cs, int *idx, int max);

/* ctv_rec.c: record - the CTVREC line format: writing frames, reading them back, indexing for seeks */
FILE *private_create (const char *path, int excl);
int view_record_open (const char *path, const VIEW_HDR * h, int excl);
int view_record_ok (void);
int view_record_active (void);
int view_record_close (void);
int view_record_frames (void);
void view_record_frame (void);
void view_replay_open (const char *path, VIEW_HDR * h);
int view_replay_next (void);
int view_replay_seekable (void);
int view_replay_index (void);
int view_replay_goto (int i);

/* ctv_term.c: terminal - size, colors, raw mode, keys, signals, waiting (owns all tty state) */
int term_columns (void);
int term_rows (void);
const char *clr (int which);
void screen_restore (void);
int keys_enter (void);
int term_stop_requested (void);
int key_read (double wait);
void live_stop_on_signals (void);
void restore_on_exit (void);
void screen_enter (void);
double mono_s (void);
void sleep_until (double t);
const char *clock_hms (long ts, char *buf, size_t n);

/* ctv_rows.c: text rows - CS table rows, owner blocks, vertical records and the transaction table of a frame */
int print_trans (void);
char *tran_text (int tran, int cpid, int style, char *buf, size_t n);
int owner_is_system (int tran, int thr);
int chain_of (uint64_t wq, int seen_cs, int *widx, int *tran, int *cpid);
int32_t row_tran (const CS_ROW * r);
uint64_t row_owner (const CS_ROW * r);
void print_detail_row (int i, const CS_ROW * r);
int detail_match (int i, const char *name);
void print_detail (const CS_LAYOUT * lo, const CS_ROW * rows);
void print_snapshot (const CS_LAYOUT * lo, const CS_ROW * rows);
void live_print_row (int i, double dt);
int row_moved (int i);
void print_view_pool (void);

/* ctv_json.c: JSON output - one JSON document per frame or window */
void json_fstr (FILE * f, const char *s);
void json_ident (FILE * f, const TRAN_IDENT * id, int with_info);
void json_cs_rows (FILE * f, const CS_LAYOUT * lo, const CS_ROW * rows, const int *occ, const int *wai, int samples, const CS_ROW * prev, double dt, int raw);
void json_os (FILE * f);
void json_trans (void);
void json_doc (int pid, const char *db, const CS_LAYOUT * lo, int window, int rows, int trans);

/* ctv_sampler.c: live sampler - the per-sample counts of a window (LIVE_WIN): occupancy, waits, activity, queue walks, OS states */
LIVE_WIN *live_win_new (int count, int stride);
void live_win_free (LIVE_WIN * w);
void live_win_reset (LIVE_WIN * w, int count);
void live_accumulate (LIVE_WIN * w, const CS_LAYOUT * lo);

/* ctv_present.c: frame output - one frame in the chosen mode: summary screen, scrolling rows, transaction table or JSON */
void live_banner (int hz, double interval);
void live_render (const CS_LAYOUT * lo, int screen, int json, double dt, int hz, double interval, int pid, const char *db);
void trans_window_out (int pid, const char *db, const CS_LAYOUT * lo, int json);
double replay_pace (const VIEW_HDR * h);
void replay_frame_out (const VIEW_HDR * h, int screen, int json, int trans);

/* ctv_live.c: live loop - sampling, closing windows, keys and [r] recording */
void die_target_gone (void)
  __attribute__ ((noreturn));
int live_mode (uint64_t cs_base, const VIEW_HDR * h, int json, int trans, const char *record, int table_only);

/* ctv_replay.c: replay loop - a recorded run, scrolling or interactive */
int live_replay (const VIEW_HDR * h, double speed, int json, int trans);

#endif /* _CTV_H_ */
