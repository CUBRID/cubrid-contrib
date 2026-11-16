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
 * verdict - the figures behind a summary screen: per-CS rates, losses and
 * severity, the holder (session or engine thread) of each CS, the
 * persistence history, the verdict itself (VERDICT), and the CS
 * descriptions and hints
 */

#include "ctv.h"


/* the live / replay screen: verdict, CS ranked by waits, sessions by what
 * they did in the window.  Everything comes from the captured frame. */

/* what each static critical section guards, in both languages */
static const struct
{
  const char *name, *en, *ko, *hint_en, *hint_ko;
} cs_info[] = {
  {"WFG", "wait-for graph (unused)", "대기 그래프 (미사용)", "-", "-"},
  {"LOG", "log flush/WAL/reads", "로그 플러시·WAL·읽기",
   "log disk write latency (await), log_buffer_size, group commit",
   "로그 디스크 쓰기 지연(await), log_buffer_size, 그룹 커밋"},
  {"LOCATOR_CLASSNAME_TABLE", "table name lookup", "테이블 이름 찾기",
   "connection rate, DDL frequency", "접속 빈도, DDL 빈도"},
  {"QPROC_QUERY_TABLE", "query table alloc (boot)", "질의 표 할당 (기동 시만)", "-", "-"},
  {"QPROC_LIST_CACHE", "query result cache", "질의 결과 캐시", "list cache hit rate", "리스트 캐시 적중률"},
  {"DISK_CHECK", "sector reserve/checkdb", "섹터 예약 (checkdb 배타)",
   "file/temp growth, checkdb running", "파일·임시파일 확장, checkdb 실행"},
  {"CNV_FMT_LEXER", "format lexer (unused)", "포맷 렉서 (미사용)", "-", "-"},
  {"HEAP_CHNGUESS", "class version guess", "클래스 캐시 버전 추측",
   "class fetches by many short transactions, DDL",
   "짧은 트랜잭션 다수의 클래스 객체 전달, DDL"},
  {"TRAN_TABLE", "transaction slots", "트랜잭션 슬롯 관리",
   "connection rate, pooling", "접속 빈도, 커넥션 풀"},
  {"CT_OID_TABLE", "class to catalog OID map", "클래스→카탈로그 OID 캐시", "DDL frequency", "DDL 빈도"},
  {"HA_SERVER_STATE", "HA state", "HA 상태", "HA role changes", "HA 역할 변경"},
  {"COMPACTDB_ONE_INSTANCE", "compactdb", "compactdb", "compactdb running", "compactdb 실행 중"},
  {"ACL", "allowed client IP list", "접속 허용 IP 목록", "access_ip_control reload", "access_ip_control 재적재"},
  {"PARTITION_CACHE", "partition cache", "파티션 캐시", "partition DDL/DML", "파티션 DDL/DML"},
  {"EVENT_LOG_FILE", "event log file", "이벤트 로그 파일",
   "lock timeout/deadlock/many-ioreads events", "락 타임아웃·교착·대량 I/O 이벤트 기록"},
  {"TRACE_LOG_FILE", "trace log file", "트레이스 로그 파일",
   "slow queries logged (sql_trace_slow_msecs)", "슬로 쿼리 기록 (sql_trace_slow_msecs)"},
  {"LOG_ARCHIVE", "archive log access", "아카이브 로그 접근",
   "archive readers (backup, CDC), log_max_archives", "아카이브 소비자(백업, CDC), log_max_archives"},
  {"ACCESS_STATUS", "login record", "로그인 기록", "connection rate", "접속 빈도"},
};

static int
cs_info_find (const char *name)
{
  unsigned i;

  for (i = 0; i < sizeof (cs_info) / sizeof (cs_info[0]); i++)
    {
      if (strcmp (cs_info[i].name, name) == 0)
	{
	  return (int) i;
	}
    }
  return -1;
}

const char *
cs_desc (const char *name)
{
  int i = cs_info_find (name);

  return i < 0 ? name : M (cs_info[i].en, cs_info[i].ko);
}

const char *
cs_hint (const char *name)
{
  int i = cs_info_find (name);

  return i < 0 ? M ("the holding session", "쥔 세션") : M (cs_info[i].hint_en,
							    cs_info[i].hint_ko);
}

/* the statistics were reset (;info csstat clears them) between two rows:
 * nenter falls on a reset and on a u32 wrap, total_elapsed (a time sum that
 * does not wrap) only on a reset; both falling also rules out a torn read
 * of total_elapsed alone */
static int
stats_reset (const CS_ROW * c, const CS_ROW * p)
{
  return c->total_ms < p->total_ms && c->nenter < p->nenter;
}

/* growth of a u32 engine counter: the new value after a reset, else the
 * difference modulo 2^32 (a wrap counts) */
static uint32_t
counter_delta (uint32_t now, uint32_t before, int reset)
{
  return reset ? now : now - before;
}

void
cs_rates (const CS_ROW * c, const CS_ROW * p, int occ, int wai, int samples,
	  double dt, CS_RATES * r)
{
  int reset = stats_reset (c, p);

  r->de = counter_delta (c->nenter, p->nenter, reset);
  r->dw = counter_delta (c->nwait, p->nwait, reset);
  r->enter_s = dt > 0 ? r->de / dt : 0.0;
  r->wait_s = dt > 0 ? r->dw / dt : 0.0;
  r->occ_pct = samples > 0 ? 100.0 * occ / samples : 0.0;
  r->wait_pct = samples > 0 ? 100.0 * wai / samples : 0.0;
}

void
cs_figures (int i, double dt, CS_FIG * f)
{
  const CS_ROW *c = &view.rows[i], *p = &view.prev[i];
  CS_RATES rt;
  uint32_t de, dwt;
  double dms = stats_reset (c, p) ? c->total_ms : c->total_ms - p->total_ms;

  cs_rates (c, p, view.st.occ[i], view.st.wai[i], view.st.samples, dt, &rt);
  de = rt.de;
  dwt = rt.dw;
  f->ix = i;
  f->occ_pct = rt.occ_pct;
  f->wait_pct = rt.wait_pct;
  f->enter_s = rt.enter_s;
  f->wait_s = rt.wait_s;
  /* the engine adds every enter's acquire latency (mutex and queue) to
   * total_elapsed: lost = that time per second, avg = per enter */
  f->avg_ms = de > 0 && dms > 0 ? dms / de : 0.0;
  f->lost_s = dms > 0 ? dms / 1000.0 / dt : 0.0;
  if (f->lost_s >= 1.0 || f->wait_pct >= 75.0)
    {
      f->severity = 4;
    }
  else if (f->lost_s >= 0.3 || f->wait_pct >= 40.0)
    {
      f->severity = 3;
    }
  else if (dwt > 0 || view.st.wai[i] > 0 || f->lost_s >= 0.05)
    {
      f->severity = 2;
    }
  else if (de > 0 || view.st.occ[i] > 0)
    {
      f->severity = 1;
    }
  else
    {
      f->severity = 0;
    }
}

static int
cmp_fig (const void *a, const void *b)
{
  const CS_FIG *x = a, *y = b;

  /* a stall (waiters all window, no counter moving) outranks busy CS */
  if (x->severity != y->severity)
    {
      return y->severity - x->severity;
    }
  if (x->lost_s != y->lost_s)
    {
      return x->lost_s < y->lost_s ? 1 : -1;
    }
  if (x->wait_s != y->wait_s)
    {
      return x->wait_s < y->wait_s ? 1 : -1;
    }
  if (x->occ_pct != y->occ_pct)
    {
      return x->occ_pct < y->occ_pct ? 1 : -1;
    }
  return x->ix - y->ix;
}

/* per CS of the frame: the tran seen holding most (samples summed per tran;
 * engine threads share tran 0) and the engine thread that held most;
 * built once per frame */
static int hold_tran[MAX_CS], hold_cpid[MAX_CS], hold_thr0[MAX_CS], hold_thr0_samples[MAX_CS];
static long long hold_samples[MAX_CS];	/* sums of sighting counts */

/* held sightings ordered by (cs, tran, client pid, position in the window) */
static int
cmp_held (const void *a, const void *b)
{
  const ACT *x = &view.st.act[*(const int *) a], *y = &view.st.act[*(const int *) b];

  if (x->cs != y->cs)
    {
      return x->cs < y->cs ? -1 : 1;
    }
  if (x->tran != y->tran)
    {
      return x->tran < y->tran ? -1 : 1;
    }
  if (x->cpid != y->cpid)
    {
      return x->cpid < y->cpid ? -1 : 1;
    }
  return *(const int *) a - *(const int *) b;
}

static void
holders_build (void)
{
  static long built = -1;
  static int idx[ACT_MAX];
  int first[MAX_CS], bs[MAX_CS];
  int k, i, m = 0;

  if (built == view.seq)
    {
      return;
    }
  built = view.seq;
  for (i = 0; i < MAX_CS; i++)
    {
      hold_tran[i] = -2;
      hold_cpid[i] = -1;
      hold_samples[i] = 0;
      hold_thr0[i] = -1;
      hold_thr0_samples[i] = 0;
      first[i] = ACT_MAX;
      bs[i] = 0;
    }
  for (k = 0; k < view.st.nact; k++)
    {
      const ACT *a = &view.st.act[k];

      if (a->kind != ACT_HELD || a->cs < 0 || a->cs >= MAX_CS)
	{
	  continue;
	}
      idx[m++] = k;
      /* the engine thread that held most (first seen wins a tie) */
      if (a->tran == 0 && a->thr >= 0 && a->samples > bs[a->cs])
	{
	  bs[a->cs] = a->samples;
	  hold_thr0[a->cs] = a->thr;
	  hold_thr0_samples[a->cs] = a->samples;
	}
    }
  /* samples summed per (cs, tran, client pid); the most wins, a tie goes
   * to the one seen first in the window */
  qsort (idx, (size_t) m, sizeof (int), cmp_held);
  for (i = 0; i < m;)
    {
      const ACT *a = &view.st.act[idx[i]];
      long long sum = 0;
      int j = i;

      for (; j < m && view.st.act[idx[j]].cs == a->cs
	   && view.st.act[idx[j]].tran == a->tran
	   && view.st.act[idx[j]].cpid == a->cpid; j++)
	{
	  sum += view.st.act[idx[j]].samples;
	}
      if (sum > hold_samples[a->cs]
	  || (sum == hold_samples[a->cs] && idx[i] < first[a->cs]))
	{
	  hold_samples[a->cs] = sum;
	  hold_tran[a->cs] = a->tran;
	  hold_cpid[a->cs] = a->cpid;
	  first[a->cs] = idx[i];
	}
      i = j;
    }
}

/* name of the engine thread seen holding CS i most, "" if unknown */
const char *
daemon_name (int i)
{
  const OS_THR *os;

  holders_build ();
  os = hold_thr0[i] >= 0 ? view_os_of (hold_thr0[i]) : NULL;
  return os != NULL ? os->comm : "";
}

/* the engine thread that held CS i most (tran 0), or -1; *samples = its
 * own sightings */
int
holder_thread_of (int i, int *samples)
{
  holders_build ();
  *samples = hold_thr0_samples[i];
  return hold_thr0[i];
}

/* the figures of every CS, most severe first */
void
cs_figures_ranked (int count, double dt, CS_FIG * fig)
{
  int i;

  for (i = 0; i < count; i++)
    {
      cs_figures (i, dt, &fig[i]);
    }
  qsort (fig, (size_t) count, sizeof (fig[0]), cmp_fig);
}

/* the tran seen holding CS i most often in the window, -2 if none; *cpid
 * its client pid when seen */
int
holder_of (int i, int *samples, int *cpid)
{
  holders_build ();
  *samples = hold_samples[i] > INT_MAX ? INT_MAX : (int) hold_samples[i];
  *cpid = hold_cpid[i];
  return hold_tran[i];
}

/* windows' top CS, for the persistence line, by frame number: a
 * re-render (replay pause/step) is not a new entry, and stepping back
 * finds the earlier windows it already showed.  hist_seq holds seq + 1
 * (0 = empty). */
#define HIST_CAP 4096
static int hist_top[HIST_CAP];
static long hist_seq[HIST_CAP];
static int hist_kept = 0;

void
history_push (long seq, int top_ix)
{
  if (seq < 0)
    {
      return;			/* before the first frame: nothing to keep */
    }
  hist_seq[seq % HIST_CAP] = seq + 1;
  hist_top[seq % HIST_CAP] = top_ix;
  hist_kept = 1;
}

/* top CS of window s, or -2 when not known */
static int
history_at (long s)
{
  return s >= 0 && hist_seq[s % HIST_CAP] == s + 1 ? hist_top[s % HIST_CAP] : -2;
}

/* the entries before frame seq, as distances back (1..9); -1 when no
 * summary screen keeps a history (pipe, --json, --trans) */
int
history_export (long seq, int *back, int *top)
{
  int b, n = 0;

  if (!hist_kept)
    {
      return -1;
    }
  for (b = 1; b < VERDICT_HISTORY; b++)
    {
      int t = history_at (seq - b);

      if (t != -2)
	{
	  back[n] = b;
	  top[n++] = t;
	}
    }
  return n;
}

/* the history exactly as a frame carries it */
void
history_import (long seq, int n, const int *back, const int *top)
{
  int b;

  for (b = 1; b < VERDICT_HISTORY && seq - b >= 0; b++)
    {
      hist_seq[(seq - b) % HIST_CAP] = 0;
    }
  for (b = 0; b < n; b++)
    {
      history_push (seq - back[b], top[b]);
    }
}

/* windows among the 10 up to this one (by frame number, idle ones too)
 * whose top CS was top_ix; *span = windows known in that range */
int
history_count (long seq, int top_ix, int *span)
{
  int b, same = 0;

  *span = 0;
  for (b = 0; b < VERDICT_HISTORY; b++)
    {
      int t = history_at (seq - b);

      if (t != -2)
	{
	  (*span)++;
	  same += t == top_ix;
	}
    }
  return same;
}

/* the verdict on the top CS of the frame: every figure and judgement the
 * screen shows, computed from the frame alone */
void
verdict_compute (const CS_FIG * top, const CS_FIG * second, VERDICT * v)
{
  const CS_ROW *c = &view.rows[top->ix];
  int k, hs, thr_samples, bs = 0;
  double waited = 0.0;

  memset (v, 0, sizeof (*v));
  v->top = *top;
  v->has_second = second != NULL && second->severity >= 2;
  if (v->has_second)
    {
      v->second = *second;
    }
  v->same = history_count (view.seq, top->ix, &v->span);
  v->holder = holder_of (top->ix, &hs, &v->holder_cpid);
  v->held_pct = view.st.samples > 0 ? 100.0 * hs / view.st.samples : 0.0;
  /* an engine holder is named by one thread: its own share, the engine
   * threads together as evidence */
  v->one_thr = v->holder == 0 && holder_thread_of (top->ix, &thr_samples) >= 0;
  v->thr_pct = v->one_thr && view.st.samples > 0
    ? 100.0 * thr_samples / view.st.samples : v->held_pct;
  for (k = 0; k < view.st.nact; k++)
    {
      const ACT *a = &view.st.act[k];

      if (a->cs != top->ix || a->kind != ACT_WAIT || a->tran < 1)
	{
	  continue;
	}
      v->nwaiters++;
      waited += a->samples;	/* samples each session was seen waiting */
      if (v->nlisted < VERDICT_WAITERS_LISTED)
	{
	  v->wtran[v->nlisted] = a->tran;
	  v->wsamples[v->nlisted++] = a->samples;
	}
      else
	{
	  v->more_waiters = 1;
	}
    }
  if (v->nwaiters > 0 && view.st.samples > 0)
    {
      v->waited_pct = 100.0 * waited / v->nwaiters / view.st.samples;
    }
  /* disk or CPU: the holder thread's scheduler states and the log device */
  for (k = 0; k < view.st.nact; k++)
    {
      const ACT *a = &view.st.act[k];
      const OS_THR *o = a->cs == top->ix && a->kind == ACT_HELD && a->thr >= 0
	? view_os_of (a->thr) : NULL;

      if (o != NULL && o->samples > bs)
	{
	  v->os = o;
	  bs = o->samples;
	}
    }
  if (v->os != NULL && v->os->samples <= 0)
    {
      v->os = NULL;
    }
  if (v->os != NULL)
    {
      /* the tick delta covers first..last sighting, not the whole window */
      double seen_s = v->os->span_ms / 1000.0;

      v->dsk_pct = 100.0 * v->os->dsk / v->os->samples;
      v->run_pct = 100.0 * v->os->run / v->os->samples;
      v->cpu_pct = seen_s >= 0.05
	? 100.0 * (double) (v->os->ticks1 - v->os->ticks0) / os_clock_ticks () / seen_s : 0.0;
    }
  if (v->os == NULL && !view.disk_avail)
    {
      v->bound = VB_NONE;
    }
  /* the log device's await speaks for the log sections only; any other
   * section needs its holder seen in D state */
  else if ((v->os != NULL && v->dsk_pct >= 50.0)
	   || (strncmp (c->name, "LOG", 3) == 0 && view.disk_avail
	       && view.disk_await_ms >= 5.0))
    {
      v->bound = VB_DISK;
    }
  else if (v->os != NULL && (v->run_pct >= 80.0 || v->cpu_pct >= 80.0))
    {
      v->bound = VB_CPU;
    }
  else
    {
      v->bound = v->os != NULL ? VB_SHORT : VB_UNKNOWN;
    }
}
