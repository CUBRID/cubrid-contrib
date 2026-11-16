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
 * frame - one screen's answers captured at one moment (VIEW_FRAME), and
 * the queries the renderers ask of it
 */

#include "ctv.h"

/* --------------------------------------------------------- the frame */

VIEW_FRAME view;

static void
view_add_tid (uint64_t tid)
{
  int k;

  if (tid == 0 || view.ntid >= VIEW_MAX_TIDS)
    {
      return;
    }
  for (k = 0; k < view.ntid; k++)
    {
      if (view.tid[k] == tid)
	{
	  return;
	}
    }
  view.tid[view.ntid] = tid;
  view.tid_slot[view.ntid] = thr_index_of_tid_mem (tid);
  view.ntid++;
}

static unsigned char slot_seen[THR_MAX];	/* slots already in this frame */

static void
view_add_slot_tran (int slot, int tran, int cpid)
{
  if (view.nslot >= VIEW_MAX_SLOTS || slot < 0 || slot >= THR_MAX
      || slot_seen[slot])
    {
      return;
    }
  slot_seen[slot] = 1;
  view.slot[view.nslot] = slot;
  view.slot_tran[view.nslot] = tran;
  view.slot_cpid[view.nslot] = cpid;
  view.nslot++;
}

/* OS record of a thread slot: copied from the window when it was sampled,
 * else just its tid and name */
static void
view_add_os (int slot, const OS_THR * src)
{
  OS_THR *o;
  int k;

  if (slot < 0 || view.nos >= OS_MAX)
    {
      return;
    }
  for (k = 0; k < view.nos; k++)
    {
      if (view.os[k].slot == slot)
	{
	  return;
	}
    }
  o = &view.os[view.nos];
  if (src != NULL)
    {
      *o = *src;
    }
  else
    {
      memset (o, 0, sizeof (*o));
      o->slot = slot;
      o->tid = thr_tid_of (slot);
      if (o->tid > 0)
	{
	  (void) os_task_comm (target_pid, o->tid, o->comm, sizeof (o->comm));
	}
    }
  if (o->tid > 0)
    {
      view.nos++;
    }
}

/* log device: probed once; diskstats deltas are taken per window */
static int dk_state = -1;	/* -1 unprobed, 0 none, 1 known */
static unsigned dk_major, dk_minor;
static char dk_name[32];

static int
disk_probe (void)
{
  if (dk_state < 0)
    {
      dk_state = os_log_device (target_pid, &dk_major, &dk_minor, dk_name,
				sizeof (dk_name)) ? 1 : 0;
    }
  return dk_state;
}

static uint64_t dk_end[3];	/* diskstats at the last window end */
static int dk_carry = 0;

void
view_disk_begin (LIVE_WIN * w)
{
  if (dk_carry)
    {
      /* the last window's end values: no second read of the same moment */
      memcpy (w->dk0, dk_end, sizeof (dk_end));
      w->dk_ok = 1;
      dk_carry = 0;
      return;
    }
  w->dk_ok = disk_probe () == 1
    && os_diskstats (dk_major, dk_minor, &w->dk0[0], &w->dk0[1], &w->dk0[2], NULL, 0) == 0;
}

static void
view_disk_end (const LIVE_WIN * w, double dt)
{
  uint64_t v[3];

  view.disk_avail = 0;
  view.disk_dev[0] = '\0';
  if (w == NULL || !w->dk_ok || dt <= 0
      || os_diskstats (dk_major, dk_minor, &v[0], &v[1], &v[2], NULL, 0) != 0)
    {
      return;
    }
  memcpy (dk_end, v, sizeof (v));
  dk_carry = 1;
  /* a counter that went back (device re-registered) gives no figure */
  if (v[0] < w->dk0[0] || v[1] < w->dk0[1] || v[2] < w->dk0[2])
    {
      return;
    }
  view.disk_avail = 1;
  view.disk_await_ms = v[0] > w->dk0[0]
    ? (double) (v[1] - w->dk0[1]) / (double) (v[0] - w->dk0[0]) : 0.0;
  view.disk_util_pct = (double) (v[2] - w->dk0[2]) / (dt * 10.0);
  snprintf (view.disk_dev, sizeof (view.disk_dev), "%s", dk_name);
}

/* the queue under head; the sampler's walk of the same head is reused when
 * it is as fresh as the sampler itself would accept */
static void
view_add_chain (uint64_t head, uint64_t head2, const LIVE_WIN * w, int cs)
{
  VIEW_CHAIN *c;
  int k, tran[THR_WAITERS_MAX], cpid[THR_WAITERS_MAX];

  if (head == 0 || view.nchain >= VIEW_MAX_CHAINS)
    {
      return;
    }
  for (k = 0; k < view.nchain; k++)
    {
      if (view.chain[k].head == head)
	{
	  return;
	}
    }
  c = &view.chain[view.nchain++];
  c->head = head;
  if (w != NULL && w->wq_last[cs] == head
      && mono_s () - w->ch_walked[cs] < OS_SAMPLE_MIN_S)
    {
      c->n = w->ch_n[cs];
      memcpy (c->slot, w->ch_slot[cs], (size_t) c->n * sizeof (int));
      memcpy (tran, w->ch_tran[cs], (size_t) c->n * sizeof (int));
      memcpy (cpid, w->ch_cpid[cs], (size_t) c->n * sizeof (int));
    }
  else
    {
      /* read now, with the identities of this capture */
      c->n = thr_waiters_mem2 (head, head2, c->slot, tran, THR_WAITERS_MAX, 0);
      for (k = 0; k < c->n; k++)
	{
	  cpid[k] = -1;
	}
    }
  /* each hop with the tran read in the same read as its link */
  for (k = 0; k < c->n; k++)
    {
      view_add_slot_tran (c->slot[k], tran[k], cpid[k]);
    }
}

/* identities of the previous capture: who held each slot when the window
 * began, where a client that has left since is found */
static VIEW_IDENT *prev_ident = NULL;
static int prev_n = 0, prev_cap = 0;

void
view_add_gone (int idx, int32_t trid, const TRAN_IDENT * id)
{
  if (view.ngone == view.gone_cap)
    {
      int cap = view.gone_cap ? view.gone_cap * 2 : 16;
      VIEW_IDENT *p = realloc (view.gone, (size_t) cap * sizeof (*p));

      if (p == NULL)
	{
	  die ("out of memory", "메모리 부족", NULL);
	}
      view.gone = p;
      view.gone_cap = cap;
    }
  view.gone[view.ngone].idx = idx;
  view.gone[view.ngone].trid = trid;
  view.gone[view.ngone].id = *id;
  view.ngone++;
}

/* a client seen as (tran, cpid) that no longer holds the slot: keep its
 * identity from the window start, once */
static void
gone_note (int tran, int cpid)
{
  TRAN_IDENT id;
  int k;

  if (tran < 1 || cpid < 0 || (view_identity (tran, &id, NULL) == 0 && id.cpid == cpid))
    {
      return;
    }
  for (k = 0; k < view.ngone; k++)
    {
      if (view.gone[k].idx == tran && view.gone[k].id.cpid == cpid)
	{
	  return;
	}
    }
  for (k = 0; k < prev_n; k++)
    {
      if (prev_ident[k].idx == tran && prev_ident[k].id.cpid == cpid)
	{
	  view_add_gone (tran, prev_ident[k].trid, &prev_ident[k].id);
	  return;
	}
    }
}

/* every client the window saw that has left its slot; then this capture
 * becomes the next window's start */
static void
view_capture_gone (void)
{
  int i, k;

  view.ngone = 0;
  for (k = 0; k < view.st.nact; k++)
    {
      gone_note (view.st.act[k].tran, view.st.act[k].cpid);
    }
  for (i = 0; i < view.count; i++)
    {
      gone_note (view.st.seen_tran[i], view.st.seen_cpid[i]);
    }
  for (k = 0; k < view.nslot; k++)
    {
      gone_note (view.slot_tran[k], view.slot_cpid[k]);
    }
  if (prev_cap < view.nident)
    {
      VIEW_IDENT *p = realloc (prev_ident, (size_t) view.nident * sizeof (*p));

      if (p == NULL)
	{
	  prev_n = 0;
	  return;
	}
      prev_ident = p;
      prev_cap = view.nident;
    }
  if (view.nident > 0)
    {
      memcpy (prev_ident, view.ident, (size_t) view.nident * sizeof (*prev_ident));
    }
  prev_n = view.nident;
}

void
view_add_ident (int idx, int32_t trid, const TRAN_IDENT * id)
{
  if (view.nident == view.ident_cap)
    {
      int cap = view.ident_cap ? view.ident_cap * 2 : 64;
      VIEW_IDENT *p = realloc (view.ident, (size_t) cap * sizeof (*p));

      if (p == NULL)
	{
	  die ("out of memory", "메모리 부족", NULL);
	}
      view.ident = p;
      view.ident_cap = cap;
    }
  view.ident[view.nident].idx = idx;
  view.ident[view.nident].trid = trid;
  view.ident[view.nident].id = *id;
  view.nident++;
}

/* the transaction table of this moment into the frame */
static void
view_capture_idents (void)
{
  int t;

  view.tran_avail = tran_geom.all_tdes != 0 && !(degraded & DEG_TRAN);
  view.cid_locked = tran_geom.cid_stride != 0;
  view.has_trid = tran_geom.has_trid;
  view.ntran = view.tran_avail ? tran_geom.ntran : 0;
  view.nident = 0;
  tran_identity_prefetch ();
  for (t = 0; t < view.ntran; t++)
    {
      TRAN_IDENT id;
      int32_t trid = 0;

      if (tran_identity_cached (t, &id, &trid) == 0)
	{
	  view_add_ident (t, trid, &id);
	}
    }
}

/* --trans alone (no record): only what the table shows - no CS rows,
 * thread entries, queues or OS figures are read */
void
view_capture_trans (double dt)
{
  tran_geom_refresh ();
  view.ts = (long) time (NULL);
  view.seq++;
  view.st.samples = 0;
  view.dt = dt;
  view_capture_idents ();
  view.ngone = 0;
  view.degraded = degraded;
}

/* an empty window: nothing seen, no queue, no tran */
void
win_stats_clear (WIN_STATS * st)
{
  int i;

  memset (st, 0, offsetof (WIN_STATS, act));
  for (i = 0; i < MAX_CS; i++)
    {
      st->seen_tran[i] = -1;
      st->seen_cpid[i] = -1;
      st->seen_n[i] = -1;
    }
  st->nact = st->act_dropped = 0;
}

/* read every answer one screen needs from the server, at one moment */
void
view_capture (const CS_LAYOUT * lo, const CS_ROW * rows, const LIVE_WIN * w,
	      double dt)
{
  int i, k;

  tran_geom_refresh ();
  thr_cache_refresh ();
  view.ts = (long) time (NULL);
  view.seq++;
  view.hist_n = history_export (view.seq, view.hist_back, view.hist_top);
  view.hist_known = view.hist_n >= 0;
  view.hist_n = view.hist_known ? view.hist_n : 0;
  view.count = lo->count;
  memcpy (view.rows, rows, (size_t) lo->count * sizeof (CS_ROW));
  view.dt = dt;
  if (w != NULL)
    {
      view.st = w->st;		/* what the window saw, whole */
    }
  else
    {
      win_stats_clear (&view.st);	/* a snapshot: no window */
    }
  for (i = 0; i < lo->count; i++)
    {
      /* only these three differ from the row (the record keeps them) */
      view.prev[i] = rows[i];
      view.prev[i].nenter = w != NULL ? w->prev[i].nenter : 0;
      view.prev[i].nwait = w != NULL ? w->prev[i].nwait : 0;
      view.prev[i].total_ms = w != NULL ? w->prev[i].total_ms : 0.0;
    }

  view_capture_idents ();

  for (i = 0; i < view.nslot; i++)
    {
      slot_seen[view.slot[i]] = 0;	/* only the marks of the last frame */
    }
  view.ntid = view.nchain = view.nslot = 0;
  for (i = 0; i < lo->count; i++)
    {
      view_add_tid (rows[i].rwlock < 0 ? rows[i].owner : 0);	/* stale otherwise */
      view_add_tid (view.st.seen_owner[i]);
      view_add_chain (rows[i].wq, rows[i].wq2, w, i);
    }
  /* then the window's earlier sightings: the tran a slot waited for when
   * seen, unless it waits in a current queue (added above) */
  for (i = 0; i < lo->count; i++)
    {
      for (k = 0; k < view.st.seen_n[i]; k++)
	{
	  view_add_slot_tran (view.st.seen_slot[i][k], view.st.seen_tran_of[i][k],
			      view.st.seen_cpid_of[i][k]);
	}
    }
  view_capture_gone ();
  /* OS side: sampled writer threads first, then names of every owner seen */
  view.nos = 0;
  for (i = 0; w != NULL && i < w->nos; i++)
    {
      view_add_os (w->os[i].slot, &w->os[i]);
    }
  for (i = 0; i < view.ntid; i++)
    {
      view_add_os (view.tid_slot[i], NULL);
    }
  {
    int readers = 0;

    for (i = 0; i < lo->count; i++)
      {
	readers += (int) rows[i].wr;
      }
    view.wait_avail = thr_wait_counts (view.wait_cnt, &view.wait_live,
				       readers) == 0;
  }
  view_disk_end (w, dt);
  view.pool_state = 0;
  view.degraded = degraded;	/* after every probe of this capture */
}

void
view_capture_pool (uint64_t pool_addr)
{
  view.npool = 0;
  view.pool_truncated = -1;
  view.pool_state = pool_collect (pool_addr, view.pool, POOL_MAX_GROUPS,
				  &view.npool, &view.pool_truncated) == 0
    ? 1 : -1;
  view.degraded = degraded;
}

/* ------------------------------------------- answers for the renderers */

const OS_THR *
view_os_of (int slot)
{
  int k;

  for (k = 0; k < view.nos; k++)
    {
      if (view.os[k].slot == slot)
	{
	  return &view.os[k];
	}
    }
  return NULL;
}

/* what a thread_resume_suspend_status code means ("" for idle / resumed) */
const char *
resume_name (int v)
{
  switch (v)
    {
    case 3:
      return M ("page latch", "페이지 래치");
    case 5:
      return M ("job queue", "작업 큐");
    case 7:
    case 9:
    case 11:
      return M ("critical section", "크리티컬 섹션");
    case 13:
      return M ("connection queue", "접속 큐");
    case 15:
      return M ("class repr", "클래스 표현");
    case 17:
      return M ("lock", "락");
    case 19:
      return M ("log writer", "로그 writer");
    case 21:
      return M ("free buffer", "빈 버퍼");
    case 23:
      return M ("DWB queue", "DWB 큐");
    case 25:
      return M ("sleep", "sleep");
    case 26:
      return M ("concurrency slot", "동시성 슬롯");
    default:
      return "";
    }
}

static int
cmp_ident (const void *a, const void *b)
{
  int x = ((const VIEW_IDENT *) a)->idx, y = ((const VIEW_IDENT *) b)->idx;

  return x < y ? -1 : x > y;	/* no subtraction: replayed values are any int */
}

int
view_identity (int tran_index, TRAN_IDENT * id, int32_t * trid)
{
  VIEW_IDENT key, *hit;

  if (view.nident == 0)
    {
      return -1;
    }
  key.idx = tran_index;
  hit = bsearch (&key, view.ident, (size_t) view.nident, sizeof (VIEW_IDENT),
		 cmp_ident);
  if (hit == NULL)
    {
      return -1;
    }
  *id = hit->id;
  if (trid != NULL)
    {
      *trid = hit->trid;
    }
  return 0;
}

/* the identity to show for a client seen as (tran, cpid): 0 the slot's
 * current client (or cpid not known), 1 a client that has left the slot
 * (identity from the window start), 2 left and not identified (only the
 * pid in id->cpid), -1 none */
int
view_identity_seen (int tran, int cpid, TRAN_IDENT * id, int32_t * trid)
{
  int k;

  if (view_identity (tran, id, trid) == 0 && (cpid < 0 || id->cpid == cpid))
    {
      return 0;
    }
  if (tran < 1 || cpid < 0)
    {
      return -1;
    }
  for (k = 0; k < view.ngone; k++)
    {
      if (view.gone[k].idx == tran && view.gone[k].id.cpid == cpid)
	{
	  *id = view.gone[k].id;
	  if (trid != NULL)
	    {
	      *trid = view.gone[k].trid;
	    }
	  return 1;
	}
    }
  memset (id, 0, sizeof (*id));
  id->cpid = cpid;
  return 2;
}

int
view_slot_of_tid (uint64_t tid)
{
  int k;

  for (k = 0; tid != 0 && k < view.ntid; k++)
    {
      if (view.tid[k] == tid)
	{
	  return view.tid_slot[k];
	}
    }
  return -1;
}

/* client pid seen with the tran of a slot, -1 unknown */
int
view_tran_cpid_of (int slot)
{
  int k;

  for (k = 0; k < view.nslot; k++)
    {
      if (view.slot[k] == slot)
	{
	  return view.slot_cpid[k];
	}
    }
  return -1;
}

int
view_tran_of (int slot)
{
  int k;

  for (k = 0; k < view.nslot; k++)
    {
      if (view.slot[k] == slot)
	{
	  return view.slot_tran[k];
	}
    }
  return -1;
}

int
view_waiters (uint64_t head, int *idx, int max)
{
  int k, n;

  for (k = 0; head != 0 && k < view.nchain; k++)
    {
      if (view.chain[k].head == head)
	{
	  n = view.chain[k].n < max ? view.chain[k].n : max;
	  memcpy (idx, view.chain[k].slot, (size_t) n * sizeof (int));
	  return n;
	}
    }
  return 0;
}

/* the chain of CS cs as walked when its queue head was first seen in the
 * window */
int
view_seen_chain (int cs, int *idx, int max)
{
  int n;

  if (cs < 0 || cs >= view.count || view.st.seen_n[cs] < 0)
    {
      return 0;
    }
  n = view.st.seen_n[cs] < max ? view.st.seen_n[cs] : max;
  memcpy (idx, view.st.seen_slot[cs], (size_t) n * sizeof (int));
  return n;
}
