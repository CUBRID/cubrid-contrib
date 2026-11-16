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
 * thread entries - the entry array, its probed fields' life, m_id/tid tables, wait chains
 */

#include "ctv.h"

/* the probed entry fields (I-15 .. I-20) and their re-probe schedule.  Only
 * the field functions change them; thr_publish copies them to thr_geom */
static FIELD f_tran, f_id, f_tid, f_resume, f_status, f_next;
static SCHED reprobe;
static void thr_publish (void);

/* ------------------------------------------------ thread entries (mem) */

/* cubthread::manager::m_all_entries = new entry[max]: base and stride probed
 * from the 'index' sequence (I-14); tran_index / m_id derived from 'index'
 * and validated (I-15 / I-16); next_wait_thrd checked at every hop */

int thr_keep_image = 0;	/* --dump: keep the probe image for thr_dump */
static unsigned char *thr_kept = NULL;
THR_GEOM thr_geom;

/* I-15 into its field */
static void
thr_probe_tran (const unsigned char *img)
{
  int rel = thr_find_tran (img);

  if (rel >= 0)
    {
      field_lock (&f_tran, rel, 0);
    }
}

/* m_id of every slot; filled from the probe image, then refreshed in place */
static uint64_t *thr_ids = NULL;

/* slot k holds a live thread (its m_id is known) */
int
thr_slot_live (int k)
{
  return thr_ids != NULL && thr_ids[k] != 0;
}

/* (m_id, slot) of the live entries sorted by m_id, rebuilt with thr_ids */
typedef struct
{
  uint64_t tid;
  int slot;
} THR_SORTED;
static THR_SORTED *thr_sorted = NULL;
static int thr_nsorted = 0;

static int
cmp_sorted (const void *a, const void *b)
{
  uint64_t x = ((const THR_SORTED *) a)->tid, y = ((const THR_SORTED *) b)->tid;

  return x < y ? -1 : x > y;
}

static void
thr_sorted_rebuild (void)
{
  int k;

  thr_nsorted = 0;
  if (thr_sorted == NULL)
    {
      thr_sorted = malloc ((size_t) thr_geom.count * sizeof (THR_SORTED));
      if (thr_sorted == NULL)
	{
	  return;
	}
    }
  for (k = 0; k < thr_geom.count; k++)
    {
      if (thr_ids[k] != 0)
	{
	  thr_sorted[thr_nsorted].tid = thr_ids[k];
	  thr_sorted[thr_nsorted++].slot = k;
	}
    }
  qsort (thr_sorted, (size_t) thr_nsorted, sizeof (THR_SORTED), cmp_sorted);
}
static double thr_ids_at = 0;	/* when the m_id table was last built */
/* OS tid of every slot (I-17), read once per m_id value */
static int *thr_tids = NULL;

/* the target's task list at the last refresh: m_id values change only when
 * threads come or go, so the table is rebuilt only when this list changes */
static int *thr_tasks = NULL;
static int thr_ntasks = 0;

static void
thr_tasks_remember (int *tasks, int ntask)
{
  free (thr_tasks);
  thr_tasks = tasks;
  thr_ntasks = ntask;
}

/* one field of every entry (base + k*stride + off) in one gather into
 * out[k * width], ok[k] = read (may be NULL); live_only skips entries
 * without a thread (they read 0) */
static int
thr_gather_field (int live_only, int off, size_t width, void *out, unsigned char *ok)
{
  static uint64_t *bases = NULL;
  static unsigned char *skip = NULL;
  int k;

  if (bases == NULL)
    {
      bases = malloc ((size_t) thr_geom.count * sizeof (uint64_t));
      skip = malloc ((size_t) thr_geom.count);
      if (bases == NULL || skip == NULL)
	{
	  free (bases);
	  free (skip);
	  bases = NULL;
	  skip = NULL;
	  return -1;
	}
      for (k = 0; k < thr_geom.count; k++)
	{
	  bases[k] = thr_geom.base + (uint64_t) k * thr_geom.stride;
	}
    }
  for (k = 0; live_only && k < thr_geom.count; k++)
    {
      skip[k] = thr_ids == NULL || thr_ids[k] == 0;
    }
  return mem_gather_field (bases, (size_t) thr_geom.count, (uint64_t) off, width,
			   live_only ? skip : NULL, out, ok);
}

/* tids of every live slot in one gather read */
static void
thr_tids_fill_all (void)
{
  if (thr_tids != NULL && thr_geom.has_tid)
    {
      /* the tid sits inside struct pthread, reached through each m_id */
      (void) mem_gather_field (thr_ids, (size_t) thr_geom.count,
			       (uint64_t) thr_geom.off_tid, sizeof (int), NULL,
			       thr_tids, NULL);
    }
}

/* thr_geom's field validity flags and offsets from the fields, and the
 * DEG_THR_SUB bits from those: only this function writes them (the array
 * position base/stride/count comes from thr_probe) */
static void
thr_publish (void)
{
  thr_geom.has_tran = f_tran.state != FIELD_NONE;
  thr_geom.off_tran = thr_geom.has_tran ? f_tran.off : THR_TRAN_REL_MAX;
  thr_geom.has_id = f_id.state != FIELD_NONE;
  thr_geom.off_id = f_id.off;
  thr_geom.has_tid = f_tid.state != FIELD_NONE;
  thr_geom.off_tid = f_tid.off;
  thr_geom.has_resume = f_resume.state != FIELD_NONE;
  thr_geom.off_resume = f_resume.off;
  thr_geom.has_status = f_status.state != FIELD_NONE;
  thr_geom.off_status = f_status.off;
  thr_geom.has_next = f_next.state != FIELD_NONE;	/* candidates at least */
  thr_geom.off_next = f_next.state == FIELD_SET ? f_next.off : -1;
  if (thr_geom.base == 0)
    {
      return;			/* DEG_THREAD covers it */
    }
  deg_set (DEG_THR_SUB, 0);
  deg_set ((thr_geom.has_tran ? 0 : DEG_THR_TRAN)
	   | (thr_geom.has_id ? 0 : DEG_THR_ID) | (thr_geom.has_tid ? 0 : DEG_THR_TID)
	   | (thr_geom.has_resume ? 0 : DEG_THR_RESUME)
	   | (thr_geom.has_next ? 0 : DEG_THR_NEXT)
	   | (thr_geom.has_status ? 0 : DEG_THR_STATUS), 1);
}

/* the m_id table (one per slot), from a probe image or, img == NULL, from
 * one gather of the field; the sorted index follows.  0 on success */
static int
thr_ids_load (const unsigned char *img)
{
  int k;

  if (thr_ids == NULL
      && (thr_ids = calloc ((size_t) thr_geom.count, sizeof (uint64_t))) == NULL)
    {
      return -1;
    }
  if (img != NULL)
    {
      for (k = 0; k < thr_geom.count; k++)
	{
	  thr_ids[k] = rd_u64 (img, (size_t) k * thr_geom.stride + thr_geom.off_id);
	}
    }
  else
    {
      (void) thr_gather_field (0, thr_geom.off_id, 8, thr_ids, NULL);
    }
  thr_sorted_rebuild ();
  thr_ids_at = (double) time (NULL);
  return 0;
}

/* I-16: m_id = an 8-aligned field whose values are 0 or distinct user-space
 * addresses, about as many non-zero as the process has threads (the engine
 * clears m_id when a thread retires).  Several fields can look like that;
 * I-17 settles it: the one that reaches an OS tid through struct pthread.
 * When no tid offset validates, the strict rule (count within the thread
 * count minus the main thread and a small slack, unique) still accepts a
 * single candidate. */

/* I-17: the offset inside struct pthread (reached through the m_id at
 * id_off) at which the live entries name the target's OS threads, all
 * distinct, as many as the process has threads (minus slack).  Stage 1 finds
 * the unique offset on a sample of THR_TID_SAMPLE entries (1KB each); stage
 * 2 confirms it on every live entry (one gather).  Returns the matched
 * count, 0 when no offset validates. */

/* I-16 + I-17 together: among the m_id candidates, the one whose values lead
 * to OS tids; else the strict single candidate without tids */
static void
thr_probe_id (int pid, const unsigned char *img)
{
  int *tasks = NULL, ntask = os_task_list (pid, &tasks);
  int cand[THR_ID_CANDS], strict, n;
  int k, best = -1, best_n = 0, best_tid = 0, ties = 0;

  if (ntask < 2)
    {
      free (tasks);
      return;
    }
  n = thr_id_candidates (ntask, img, cand, &strict);
  for (k = 0; k < n; k++)
    {
      int toff = 0, m = thr_probe_tid_at (tasks, ntask, img, cand[k], &toff);

      if (m > best_n)
	{
	  best = cand[k];
	  best_n = m;
	  best_tid = toff;
	  ties = 0;
	}
      else if (m > 0 && m == best_n)
	{
	  ties++;
	}
    }
  if (best >= 0 && ties == 0)
    {
      field_lock (&f_id, best, 0);
      field_lock (&f_tid, best_tid, 0);
    }
  else if (strict >= 0)
    {
      field_lock (&f_id, strict, 0);
    }
  thr_publish ();
  if (thr_geom.has_id)
    {
      (void) thr_ids_load (img);
      if (thr_geom.has_tid)
	{
	  if (thr_tids == NULL)
	    {
	      thr_tids = calloc ((size_t) thr_geom.count, sizeof (int));
	    }
	  thr_tids_fill_all ();
	}
    }
  thr_tasks_remember (tasks, ntask);
}

/* I-18: resume_status = the unique 4-aligned field past tran_index whose
 * value is 0..27 in every entry and, over the live entries, shows at least
 * three distinct codes, reaching 7, sparse; a suspended code breaks ties;
 * two codes only (idle) are taken provisionally from a lone candidate.
 * Confirmed at runtime: a queued CS waiter must read 7..12. */

#define REPROBE_CHANGE_S 60		/* while threads change: an image per minute at most */

static int
thr_probe_resume (const unsigned char *img)
{
  return field_probe (&f_resume, thr_probe_resume_pass, img);
}

int
thr_tid_of (int slot)
{
  return thr_tids != NULL && slot >= 0 && slot < thr_geom.count ? thr_tids[slot] : 0;
}

/* I-19: next_wait_thrd follows resume_status.  A candidate offset holds 0
 * or a pointer to another entry in every entry of the image; it is fixed
 * when exactly one candidate shows such a pointer in the image, or alone
 * holds one on THR_NEXT_VOTES queue hops while no other candidate ever did. */
static int next_cand[THR_NEXT_CANDS], next_ncand = 0;
#define THR_NEXT_VOTES 3	/* distinct links needed to fix the offset */
static int next_votes[THR_NEXT_CANDS];	/* distinct links held by this candidate alone */
static uint64_t next_voted[THR_NEXT_CANDS][THR_NEXT_VOTES];	/* those links (from<<32|to) */

/* I-20: m_status = the unique 4-aligned field between tran_index and
 * resume_status that reads 0..4 in every entry, 1..4 in every live entry
 * and 2 (TS_RUN) in at least one.  resume_status keeps the last reason
 * after a wakeup; m_status says whether the thread is suspended now. */

static int
thr_probe_status (const unsigned char *img)
{
  return field_probe (&f_status, thr_probe_status_pass, img);
}

static void
thr_probe_next (const unsigned char *img)
{
  int off, hit = -1, nhit = 0, old[THR_NEXT_CANDS], nold = next_ncand;

  /* votes gathered on queue hops survive a re-probe that finds the same
   * candidates */
  memcpy (old, next_cand, sizeof (old));
  next_ncand = 0;
  field_clear (&f_next);
  if (!thr_geom.has_resume)
    {
      return;
    }
  for (off = (thr_geom.off_resume + 4 + 7) & ~7;
       off <= thr_geom.off_resume + THR_NEXT_SPAN && off + 8 <= thr_geom.stride;
       off += 8)
    {
      int nz = thr_next_static (img, off);

      if (nz < 0 || next_ncand >= THR_NEXT_CANDS)
	{
	  continue;
	}
      next_cand[next_ncand++] = off;
      if (nz > 0)
	{
	  hit = off;
	  nhit++;
	}
    }
  if (nold != next_ncand || memcmp (old, next_cand, (size_t) nold * sizeof (int)) != 0)
    {
      memset (next_votes, 0, sizeof (next_votes));
    }
  /* one candidate showing a link settles it; several are kept for votes */
  if (next_ncand > 0)
    {
      field_lock (&f_next, nhit == 1 ? hit : -1, nhit != 1);
    }
}

/* one wait-chain hop: the span of an entry from 'index' to the last field
 * the walk needs (status, resume_status, next candidates), in one read;
 * the span is inside one entry, so stride-sized buffers always hold it */
static unsigned char *hop_buf = NULL, *hop_tbuf = NULL;

static int
hop_span (int *lo)
{
  int hi = thr_geom.off_index + 4;

  *lo = thr_geom.off_index;
  if (thr_geom.has_status && thr_geom.off_status + 4 > hi)
    {
      hi = thr_geom.off_status + 4;
    }
  if (thr_geom.has_resume && thr_geom.off_resume + 4 > hi)
    {
      hi = thr_geom.off_resume + 4;
    }
  if (thr_geom.has_tran && thr_geom.off_index + thr_geom.off_tran + 4 > hi)
    {
      hi = thr_geom.off_index + thr_geom.off_tran + 4;
    }
  if (thr_geom.has_next)
    {
      int last = thr_geom.off_next >= 0 ? thr_geom.off_next
	: next_cand[next_ncand - 1];

      hi = last + 8 > hi ? last + 8 : hi;
    }
  if (hop_buf == NULL)
    {
      hop_buf = malloc ((size_t) thr_geom.stride);
    }
  if (hop_tbuf == NULL)
    {
      /* one span per next_wait_thrd candidate, gathered together */
      hop_tbuf = malloc ((size_t) thr_geom.stride * THR_NEXT_CANDS);
    }
  if (hop_buf == NULL || hop_tbuf == NULL)
    {
      return -1;		/* both are needed; each is retried next time */
    }
  return hi - *lo;
}

/* read hop k; 0 when its index field confirms the slot */
static int
hop_read (int k, unsigned char *buf, int lo, int len)
{
  return mem_read (thr_geom.base + (uint64_t) k * thr_geom.stride + (uint64_t) lo,
		   buf, (size_t) len) == 0
    && rd_i32 (buf, (size_t) (thr_geom.off_index - lo)) == k + 1 ? 0 : -1;
}

/* next_wait_thrd of hop k (buf), 0 when none or not yet knowable.  While
 * I-19 is unsettled, a candidate counts when it alone links to another
 * entry that is waiting as a CS writer; fixed after THR_NEXT_VOTES such
 * hops with no other candidate ever linking */
static uint64_t
hop_next (int k, const unsigned char *buf, int lo, int len)
{
  uint64_t v, pick = 0, addrs[THR_NEXT_CANDS];
  int c, nhit = 0, hit = -1, others = 0, tj[THR_NEXT_CANDS];
  unsigned char tok[THR_NEXT_CANDS];

  if (!thr_geom.has_next)
    {
      return 0;
    }
  if (thr_geom.off_next >= 0)
    {
      return rd_u64 (buf, (size_t) (thr_geom.off_next - lo));
    }
  /* the entry each candidate points at, all in one gather */
  for (c = 0; c < next_ncand; c++)
    {
      tj[c] = slot_of_ptr (rd_u64 (buf, (size_t) (next_cand[c] - lo)));
      addrs[c] = tj[c] >= 0 && tj[c] != k
	? thr_geom.base + (uint64_t) tj[c] * thr_geom.stride + (uint64_t) lo : 0;
    }
  (void) mem_gather_ok (addrs, (size_t) next_ncand, (size_t) len, hop_tbuf, tok);
  for (c = 0; c < next_ncand; c++)
    {
      const unsigned char *tb = hop_tbuf + (size_t) c * (size_t) len;

      /* read, its own index confirms the slot, and it waits as a writer */
      if (!tok[c] || rd_i32 (tb, (size_t) (thr_geom.off_index - lo)) != tj[c] + 1
	  || !buf_cs_writer_waiting (tb, -lo))
	{
	  continue;
	}
      v = rd_u64 (buf, (size_t) (next_cand[c] - lo));
      pick = v;
      hit = c;
      nhit++;
    }
  if (nhit != 1)
    {
      return 0;
    }
  {
    /* one vote per distinct link: the same queue walked again (sighting,
     * waiter cache, capture) adds nothing */
    uint64_t link = ((uint64_t) (unsigned) k << 32) | (unsigned) slot_of_ptr (pick);
    int u;

    for (u = 0; u < next_votes[hit] && next_voted[hit][u] != link; u++)
      ;
    if (u < next_votes[hit] || next_votes[hit] >= THR_NEXT_VOTES)
      {
	return pick;		/* seen link, or enough votes already */
      }
    next_voted[hit][next_votes[hit]] = link;
  }
  next_votes[hit]++;
  for (c = 0; c < next_ncand; c++)
    {
      others += c != hit ? next_votes[c] : 0;
    }
  if (next_votes[hit] >= THR_NEXT_VOTES && others == 0)
    {
      field_lock (&f_next, next_cand[hit], 0);
      thr_publish ();
    }
  return pick;
}

/* what was found relative to resume_status: m_status below it,
 * next_wait_thrd after it - found again when it moves or goes */
static void
resume_dependents_clear (void)
{
  field_clear (&f_status);
  field_clear (&f_next);
}

/* the runtime anchor rejected f: it and its dependents are probed again at
 * the next refresh */
static void
fields_drop (FIELD * f)
{
  field_reject (f);
  if (f == &f_resume)
    {
      resume_dependents_clear ();
    }
  sched_restart (&reprobe);
  thr_publish ();
}

/* I-18 at runtime: one sighting for every check of the field.  Reads are
 * not atomic with the sample that motivated them, so single misses are
 * tolerated (field_sight) */
static void
resume_strike (int ok)
{
  if (field_sight (&f_resume, ok))
    {
      fields_drop (&f_resume);
    }
}

/* a thread seen at the head of a CS wait queue must read a CS code; v and
 * st are its resume_status and m_status from one read of the entry */
static void
thr_resume_anchor (int v, int st)
{
  if (!thr_geom.has_resume)
    {
      return;
    }
  resume_strike (v >= THR_RESUME_CS_MIN && v <= THR_RESUME_CS_MAX);
  /* a suspended code with a non-TS_WAIT status is a miss; a running head
   * (resumed code) proves nothing */
  if (thr_geom.has_status && (st == TS_WAIT || resume_suspended (v))
      && field_sight (&f_status, st == TS_WAIT))
    {
      fields_drop (&f_status);
    }
}

/* resume_status histogram over the live threads (one gather read);
 * 0 on success, -1 when the field is unavailable */
int
thr_wait_counts (int *cnt, int *live, int reader_cap)
{
  static int32_t *vals = NULL, *st = NULL;
  static unsigned char *okv = NULL, *oks = NULL;
  int k;

  memset (cnt, 0, THR_RESUME_MAX * sizeof (int));
  *live = 0;
  if (!thr_geom.has_resume || thr_ids == NULL
      || (vals == NULL
	  && (vals = malloc ((size_t) thr_geom.count * sizeof (int32_t))) == NULL)
      || (st == NULL
	  && (st = malloc ((size_t) thr_geom.count * sizeof (int32_t))) == NULL)
      || (okv == NULL && (okv = malloc ((size_t) thr_geom.count)) == NULL)
      || (oks == NULL && (oks = malloc ((size_t) thr_geom.count)) == NULL)
      || thr_gather_field (1, thr_geom.off_resume, 4, vals, okv) != 0
      || (thr_geom.has_status
	  && thr_gather_field (1, thr_geom.off_status, 4, st, oks) != 0))
    {
      return -1;
    }
  for (k = 0; k < thr_geom.count; k++)
    {
      int v = vals[k];

      /* an entry whose read failed is neither live nor a vote on I-18 */
      if (thr_ids[k] == 0 || !okv[k] || (thr_geom.has_status && !oks[k]))
	{
	  continue;
	}
      if (v < 0 || v >= THR_RESUME_MAX)
	{
	  resume_strike (0);	/* a torn or failed read of one entry */
	  continue;
	}
      (*live)++;
      /* resume_status keeps the last reason: a thread counts only while it
       * is suspended (I-20).  CS readers wait without TS_WAIT and keep code 7
       * after waking: they are counted from the CS reader queues below. */
      if (v != 7 && (!thr_geom.has_status ? resume_suspended (v)
		     : st[k] == TS_WAIT && resume_suspended (v)))
	{
	  cnt[v]++;
	}
    }
  cnt[7] = reader_cap;		/* sum of waitR over the CS array */
  return thr_geom.has_resume ? 0 : -1;
}

void
thr_probe (uint64_t manager_sym, int pid)
{
  uint64_t mobj;

  thr_geom.off_tran = THR_TRAN_REL_MAX;
  if (manager_sym == 0 || mem_read_ptr (manager_sym, &mobj) != 0
      || thr_probe_entries (mobj) != NULL)
    {
      thr_geom.base = 0;
      deg_set (DEG_THREAD, 1);	/* degraded, not refused */
      return;
    }
  /* count: grow the image in 1MB steps until the index sequence breaks or
   * the memory behind the array ends */
  thr_geom.count = 0;
  {
    size_t have = 0, step = (size_t) 1 << 20;
    unsigned char *img = NULL;
    int n = 0;

    for (;;)
      {
	unsigned char *more = realloc (img, have + step);

	if (more == NULL)
	  {
	    break;
	  }
	img = more;
	if (mem_read (thr_geom.base + have, img + have, step) != 0)
	  {
	    /* the mapping ends inside this chunk: retry smaller, down to
	     * one entry */
	    if (step > (size_t) thr_geom.stride)
	      {
		step = step / 2 < (size_t) thr_geom.stride
		  ? (size_t) thr_geom.stride : step / 2;
		continue;
	      }
	    /* readable region may end with the array: finish the entry whose
	     * index is already visible */
	    {
	      size_t rem = (size_t) (n + 1) * thr_geom.stride - have;

	      if (rem > 0 && rem < (size_t) thr_geom.stride
		  && (size_t) n * thr_geom.stride + thr_geom.off_index + 4 <= have
		  && mem_read (thr_geom.base + have, img + have, rem) == 0)
		{
		  have += rem;
		  if (rd_i32 (img, (size_t) n * thr_geom.stride
			      + thr_geom.off_index) == n + 1)
		    {
		      n++;
		    }
		}
	    }
	    break;
	  }
	have += step;
	while (n < THR_MAX && (size_t) (n + 1) * thr_geom.stride <= have
	       && rd_i32 (img, (size_t) n * thr_geom.stride + thr_geom.off_index)
	       == n + 1)
	  {
	    n++;
	  }
	if (n >= THR_MAX || (size_t) (n + 1) * thr_geom.stride <= have)
	  {
	    break;		/* sequence ended inside the image */
	  }
	step = (size_t) 1 << 20;
      }
    thr_geom.count = n;
    if (n < CS_MIN_SAMPLES)
      {
	free (img);
	thr_geom.base = 0;
	deg_set (DEG_THREAD, 1);
	return;
      }
    thr_probe_tran (img);
    thr_publish ();
    thr_probe_id (pid, img);	/* the probe image also serves the first screen */
    if (thr_geom.has_id)
      {
	(void) thr_probe_resume (img);
	thr_publish ();
	(void) thr_probe_status (img);
	thr_publish ();
      }
    thr_probe_next (img);
    thr_publish ();
    if (thr_keep_image)
      {
	free (thr_kept);
	thr_kept = img;		/* the dump writes from this image */
      }
    else
      {
	free (img);
      }
  }
  /* the probe image was just read: the first re-probe waits the back-off */
  sched_start (&reprobe, time (NULL));
  thr_publish ();
}

/* m_id and tid tables from two gathers (no array image) */
static void
thr_tables_reload (void)
{
  if (thr_ids_load (NULL) == 0)
    {
      thr_tids_fill_all ();
    }
}

/* once a second: m_id / tid tables rebuilt by two gathers when the task
 * list (procfs) changed; an unsettled probe re-reads the array image on
 * its schedule, and a change only brings a slow-phase try forward */
void
thr_cache_refresh (void)
{
  time_t now = time (NULL);
  int *tasks = NULL, ntask, settled, waits_settled;
  unsigned char *img;

  /* once a second */
  if (thr_geom.base == 0 || !every (&thr_ids_at, (double) now, 1.0))
    {
      return;
    }
  ntask = os_task_list (target_pid, &tasks);
  if (ntask < 0)
    {
      return;
    }
  /* I-19 settles on queue hops (votes), not by re-reading the image.  The
   * wait fields (I-18, I-20) are retried until they settle; an entry field
   * that failed at start (I-15..I-17) only in the fast rounds - a build
   * that lacks it would otherwise re-read the array for the whole run */
  waits_settled = f_resume.state == FIELD_SET && f_status.state != FIELD_NONE;
  settled = waits_settled
    && ((f_tran.state && f_id.state && f_tid.state) || sched_slow (&reprobe));
  if (ntask == thr_ntasks && thr_tasks != NULL
      && memcmp (tasks, thr_tasks, (size_t) ntask * sizeof (int)) == 0)
    {
      free (tasks);
      /* same threads: only an unsettled probe is retried, on the schedule */
      if (settled || !sched_due (&reprobe, now))
	{
	  return;
	}
    }
  else
    {
      thr_tasks_remember (tasks, ntask);
      if (sched_slow (&reprobe) && !waits_settled)
	{
	  sched_sooner (&reprobe, REPROBE_CHANGE_S);	/* an elastic pool changes often */
	}
    }
  if (settled || !sched_due (&reprobe, now))
    {
      /* nothing to probe now: m_id and tid tables from two gathers */
      if (thr_geom.has_id)
	{
	  thr_tables_reload ();
	}
      return;
    }
  sched_ran (&reprobe, now);
  img = thr_image ();
  if (img == NULL)
    {
      return;
    }
  if (f_tran.state == FIELD_NONE)
    {
      thr_probe_tran (img);
    }
  if (f_id.state == FIELD_NONE || f_tid.state == FIELD_NONE)
    {
      thr_probe_id (target_pid, img);
    }
  else
    {
      (void) thr_ids_load (img);
      thr_tids_fill_all ();
    }
  thr_publish ();
  if (thr_geom.has_id)		/* the fields below need the m_id table */
    {
      int was = f_resume.state != FIELD_NONE ? f_resume.off : -1;

      if (f_resume.state != FIELD_SET)
	{
	  (void) thr_probe_resume (img);
	}
      if (f_resume.state != FIELD_NONE && f_resume.off != was)
	{
	  resume_dependents_clear ();	/* found or moved */
	}
      thr_publish ();
      if (f_status.state == FIELD_NONE)
	{
	  (void) thr_probe_status (img);
	  thr_publish ();
	}
      if (f_next.state != FIELD_SET)
	{
	  thr_probe_next (img);
	  thr_publish ();
	}
    }
  free (img);
}

/* slot owning a pthread_t, or -1 (binary search of the sorted index) */
int
thr_index_of_tid_mem (uint64_t tid)
{
  THR_SORTED key, *hit;

  if (thr_sorted == NULL || tid == 0 || thr_nsorted == 0)
    {
      return -1;
    }
  key.tid = tid;
  hit = bsearch (&key, thr_sorted, (size_t) thr_nsorted, sizeof (THR_SORTED),
		 cmp_sorted);
  return hit != NULL ? hit->slot : -1;
}

/* follow waiting_writers_queue from its head; every hop must land on an
 * in-array entry.  Returns the number of indices stored (<= max), with
 * each hop's tran_index in tran (may be NULL).  anchor_at (the CS field
 * the head came from, or 0): the head checks I-18 / I-20 even when the
 * walk rejects it (a wrong field would reject them all) - only while the
 * field still names it after its entry was read, so a head served in
 * between (now waiting on something else) proves nothing. */
int
thr_waiters_mem (uint64_t head, int *idx, int *tran, int max, uint64_t anchor_at)
{
  uint64_t first = head;
  int n = 0, lo, len = hop_span (&lo);
  unsigned char *buf = hop_buf;
  uint64_t still;

  while (head != 0 && n < max && len > 0)
    {
      int k = slot_of_ptr (head), j;

      /* one read per hop; a served waiter may be queued elsewhere now */
      if (k < 0 || hop_read (k, buf, lo, len) != 0)
	{
	  break;
	}
      if (anchor_at != 0 && n == 0 && thr_geom.has_resume
	  && mem_read (anchor_at, &still, 8) == 0 && still == first)
	{
	  thr_resume_anchor (rd_i32 (buf, (size_t) (thr_geom.off_resume - lo)),
			     thr_geom.has_status
			     ? rd_i32 (buf, (size_t) (thr_geom.off_status - lo)) : -1);
	  len = hop_span (&lo);	/* the anchor may drop fields */
	  if (len <= 0)
	    {
	      break;
	    }
	}
      if (!buf_cs_writer_waiting (buf, -lo))
	{
	  break;
	}
      for (j = 0; j < n; j++)
	{
	  if (idx[j] == k)
	    {
	      return n;		/* cycle: a torn read, stop here */
	    }
	}
      if (tran != NULL)
	{
	  tran[n] = thr_geom.has_tran
	    ? rd_i32 (buf, (size_t) (thr_geom.off_index + thr_geom.off_tran - lo)) : -1;
	}
      idx[n++] = k;
      head = hop_next (k, buf, lo, len);
    }
  return n;
}

/* the promoters queue (head) and then the writers queue behind it (head2,
 * 0 if none) as one chain, in serving order */
int
thr_waiters_mem2 (uint64_t head, uint64_t head2, int *idx, int *tran, int max,
		  uint64_t anchor_at)
{
  int n = thr_waiters_mem (head, idx, tran, max, anchor_at);

  if (head2 != 0 && n < max)
    {
      int m = thr_waiters_mem (head2, idx + n, tran != NULL ? tran + n : NULL,
			       max - n, 0), k, j;

      /* a thread in both queues is a torn read: the second part stops there */
      for (k = 0; k < m; k++)
	{
	  for (j = 0; j < n && idx[j] != idx[n + k]; j++)
	    {
	      ;
	    }
	  if (j < n)
	    {
	      m = k;
	      break;
	    }
	}
      n += m;
    }
  return n;
}

/* thread entry table as JSON (detail dump): geometry, then every entry that
 * has a thread id, a transaction or a waiter link */
void
thr_dump (FILE * f)
{
  unsigned char *img = thr_kept != NULL ? thr_kept
    : thr_geom.base != 0 ? thr_image () : NULL;
  int k, first = 1;

  fprintf (f, "\"threads\":{\"available\":%s,\"base\":\"0x%llx\","
	   "\"stride\":%d,\"count\":%d,\"off_index\":%d,\"off_id\":%d,"
	   "\"has_tran\":%d,\"has_id\":%d,\"has_tid\":%d,\"off_tid\":%d,"
	   "\"has_resume\":%d,\"off_resume\":%d,\"has_next\":%d,"
	   "\"off_next\":%d,\"off_tran\":%d,\"has_status\":%d,"
	   "\"off_status\":%d,\"entries\":[\n",
	   img != NULL ? "true" : "false",
	   (unsigned long long) thr_geom.base, thr_geom.stride,
	   thr_geom.count, thr_geom.off_index, thr_geom.off_id,
	   thr_geom.has_tran, thr_geom.has_id, thr_geom.has_tid,
	   thr_geom.off_tid, thr_geom.has_resume, thr_geom.off_resume,
	   thr_geom.has_next, thr_geom.off_next, thr_geom.off_tran,
	   thr_geom.has_status, thr_geom.off_status);
  for (k = 0; img != NULL && k < thr_geom.count; k++)
    {
      size_t e = (size_t) k * thr_geom.stride + thr_geom.off_index;
      uint64_t tid = thr_geom.has_id
	? rd_u64 (img, (size_t) k * thr_geom.stride + thr_geom.off_id) : 0;
      int32_t tran = thr_geom.has_tran
	? rd_i32 (img, e + thr_geom.off_tran) : -1;
      uint64_t next = thr_geom.off_next >= 0
	? rd_u64 (img, (size_t) k * thr_geom.stride + thr_geom.off_next) : 0;
      int nk = next != 0 ? thr_index_of_ptr (next) : -1;

      if (tid == 0 && tran == -1 && next == 0)
	{
	  continue;
	}
      fprintf (f, "%s{\"slot\":%d,\"index\":%d,\"tid\":\"0x%llx\","
	       "\"tran\":%d,\"next_wait\":\"0x%llx\",\"next_wait_index\":%d",
	       first ? "" : ",", k, rd_i32 (img, e), (unsigned long long) tid,
	       tran, (unsigned long long) next, nk >= 0 ? THR_INDEX (nk) : -1);
      /* the OS tid belongs to the current m_id of the slot: only when the
       * image shows the same thread */
      if (thr_geom.has_tid && tid != 0 && thr_ids != NULL && thr_ids[k] == tid)
	{
	  char comm[OS_COMM_LEN];

	  (void) os_task_comm (target_pid, thr_tid_of (k), comm, sizeof (comm));
	  fprintf (f, ",\"os_tid\":%d,\"comm\":", thr_tid_of (k));
	  json_fstr (f, comm);
	}
      if (thr_geom.has_resume)
	{
	  fprintf (f, ",\"resume_status\":%d",
		   rd_i32 (img, (size_t) k * thr_geom.stride + (size_t) thr_geom.off_resume));
	}
      fprintf (f, "}\n");
      first = 0;
    }
  fprintf (f, "]}");
  if (img != thr_kept)
    {
      free (img);
    }
}
