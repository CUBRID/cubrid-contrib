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
 * thread entry probes - read-only searches of the target and of an
 * entry-array image: where the array is (I-14, recorded in thr_geom), and
 * the candidate offsets of its fields (I-15 .. I-20); no FIELD state is
 * changed here - ctv_thr.c locks what they find
 */

#include "ctv.h"

static int cmp_u64 (const void *a, const void *b);

/* the smallest (stride, io) for which entries 0..3 of win read index
 * 1,2,3,4 at io: walk the 2s of the window (entry 1) instead of every
 * (stride, io) pair; 0 when found */
static int
thr_match_stride (const unsigned char *win, size_t have, const int *cand,
		  int ncand, int *stride_out, int *io_out)
{
  int best_stride = 0, best_io = 0, c, k;
  size_t q;

  for (q = THR_STRIDE_MIN; q + 4 <= have
       && q < (size_t) THR_STRIDE_MAX + THR_INDEX_SCAN; q += 4)
    {
      if (rd_i32 (win, q) != 2)
	{
	  continue;
	}
      for (c = 0; c < ncand; c++)
	{
	  int io = cand[c], stride = (int) q - io;

	  if (stride < THR_STRIDE_MIN || stride > THR_STRIDE_MAX
	      || stride % 8 != 0 || io + THR_TRAN_REL_MAX + 4 > stride
	      || (best_stride != 0 && stride >= best_stride)
	      || (size_t) (CS_MIN_SAMPLES - 1) * stride + THR_INDEX_SCAN > have)
	    {
	      continue;
	    }
	  for (k = 2; k < CS_MIN_SAMPLES; k++)
	    {
	      if (rd_i32 (win, (size_t) k * stride + io) != k + 1)
		{
		  break;
		}
	    }
	  if (k == CS_MIN_SAMPLES)
	    {
	      best_stride = stride;
	      best_io = io;
	    }
	}
    }
  if (best_stride == 0)
    {
      return -1;
    }
  *stride_out = best_stride;
  *io_out = best_io;
  return 0;
}

/* I-14: inside the manager object, the pointer p for which the ints at
 * p + k*stride + off read 1,2,3,4 for a single (stride, off): the engine
 * numbers pool entries from 1 (index 0 is the main thread, kept apart) */
const char *
thr_probe_entries (uint64_t manager_obj)
{
  unsigned char mobj[4096], *win;
  size_t winn = (size_t) CS_MIN_SAMPLES * THR_STRIDE_MAX, mn, step, q2;
  int off;

  /* the object sits on the heap; shrink until the read fits */
  mn = mem_read_shrink (manager_obj, mobj, sizeof (mobj), 256);
  if (mn == 0)
    {
      REFUSE ("I-14 manager object");
    }
  memset (mobj + mn, 0, sizeof (mobj) - mn);
  win = malloc (winn);
  if (win == NULL)
    {
      return "I-14 out of memory";
    }
  for (off = 0; off + 8 <= (int) sizeof (mobj); off += 8)
    {
      uint64_t p = rd_u64 (mobj, off);
      size_t have;
      int stride, io, cand[THR_INDEX_SCAN / 4], ncand = 0;

      if (!VALID_PTR (p))
	{
	  continue;
	}
      /* slot 0 must hold index 1 within its first 2KB: those offsets are
       * the only candidates for 'index' */
      if (mem_read (p, win, THR_INDEX_SCAN) != 0)
	{
	  continue;
	}
      for (io = 0; io + 4 <= THR_INDEX_SCAN; io += 4)
	{
	  if (rd_i32 (win, io) == 1)
	    {
	      cand[ncand++] = io;
	    }
	}
      if (ncand == 0)
	{
	  continue;
	}
      /* grow the window 16KB -> 66KB (every index-2 position of the widest
       * stride) -> full only while nothing matched: the real array (small
       * strides) is settled from the first read */
      for (step = 16384;;
	   step = step * 4 > (size_t) THR_STRIDE_MAX + THR_INDEX_SCAN + 8
	   ? (step * 4 < winn ? step * 4 : winn)
	   : (size_t) THR_STRIDE_MAX + THR_INDEX_SCAN + 8)
	{
	  have = mem_read_shrink (p, win, step,
				  (size_t) CS_MIN_SAMPLES * THR_STRIDE_MIN);
	  if (have == 0)
	    {
	      break;
	    }
	  if (thr_match_stride (win, have, cand, ncand, &stride, &io) == 0)
	    {
	      thr_geom.base = p;
	      thr_geom.stride = stride;
	      thr_geom.off_index = io;
	      free (win);
	      return NULL;
	    }
	  if (have < step || step >= winn)
	    {
	      break;		/* the memory ends here, or the full window was read */
	    }
	  /* grow only while some candidate's index 2 (one stride after its
	   * index 1) was seen, so the sequence needs more entries, or may
	   * still lie beyond what was read */
	  {
	    int c, more = 0;

	    for (c = 0; c < ncand && !more; c++)
	      {
		for (q2 = (size_t) cand[c] + THR_STRIDE_MIN;
		     !more && q2 + 4 <= have
		     && q2 <= (size_t) cand[c] + THR_STRIDE_MAX; q2 += 8)
		  {
		    more = rd_i32 (win, q2) == 2;
		  }
		more |= (size_t) cand[c] + THR_STRIDE_MAX + 4 > have;
	      }
	    if (!more)
	      {
		break;
	      }
	  }
	}

    }
  free (win);
  REFUSE ("I-14 entry array");
}

/* array slot k (engine index k + 1), validated by its own index field */
static int
thr_entry (int k, uint64_t * addr)
{
  int32_t idx;

  if (k < 0 || k >= thr_geom.count)
    {
      return -1;
    }
  *addr = thr_geom.base + (uint64_t) k * thr_geom.stride;
  return (mem_read (*addr + thr_geom.off_index, &idx, 4) == 0
	  && idx == k + 1) ? 0 : -1;
}

/* slot of an in-array pointer by arithmetic only, or -1 */
int
slot_of_ptr (uint64_t p)
{
  uint64_t rel = p - thr_geom.base;

  if (thr_geom.base == 0 || p < thr_geom.base
      || rel % (uint64_t) thr_geom.stride != 0
      || rel / (uint64_t) thr_geom.stride >= (uint64_t) thr_geom.count)
    {
      return -1;
    }
  return (int) (rel / (uint64_t) thr_geom.stride);
}

/* index of the entry an in-array pointer refers to, or -1 */
int
thr_index_of_ptr (uint64_t ptr)
{
  uint64_t addr;
  int k = slot_of_ptr (ptr);

  return k >= 0 && thr_entry (k, &addr) == 0 ? k : -1;
}

/* one image of the whole entry array (count * stride bytes), or NULL */
unsigned char *
thr_image (void)
{
  size_t n = (size_t) thr_geom.count * thr_geom.stride;
  unsigned char *img = malloc (n);

  if (img != NULL && mem_read (thr_geom.base, img, n) != 0)
    {
      free (img);
      return NULL;
    }
  return img;
}

/* I-15: tran_index derivation holds when every entry reads -1 or a value
 * below the descriptor count, and idle entries (-1) are the majority */
static int
thr_check_tran (const unsigned char *img, int rel)
{
  int k, neg = 0;

  for (k = 0; k < thr_geom.count; k++)
    {
      int32_t v = rd_i32 (img, (size_t) k * thr_geom.stride
			  + thr_geom.off_index + rel);

      /* below the descriptor count; without the table, below its limit */
      if (v < -1 || v >= (tran_geom.ntran > 0 ? tran_geom.ntran : TRAN_SLOTS_MAX))
	{
	  return 0;
	}
      neg += v == -1;
    }
  return neg * 2 > thr_geom.count;
}

/* I-15: entry.tran_index sits at index+28 (engines with the per-thread
 * pgbuf counters) or index+20 (older); exactly one must pass */
int
thr_find_tran (const unsigned char *img)
{
  static const int rel[] = { 28, 20 };
  int i, hit = -1, n = 0;

  for (i = 0; i < 2; i++)
    {
      if (thr_geom.off_index + rel[i] + 4 <= thr_geom.stride
	  && thr_check_tran (img, rel[i]))
	{
	  hit = rel[i];
	  n++;
	}
    }
  return n == 1 ? hit : -1;
}

int
thr_id_candidates (int nthr, const unsigned char *img, int *cand, int *strict)
{
  uint64_t *vals = malloc ((size_t) thr_geom.count * sizeof (uint64_t));
  int off, n = 0, nstrict = 0;

  *strict = -1;
  if (vals == NULL || nthr < 2)
    {
      free (vals);
      return 0;
    }
  for (off = 0; off + 8 <= thr_geom.stride && n < THR_ID_CANDS; off += 8)
    {
      int k, nz = 0, ok = 1;

      for (k = 0; k < thr_geom.count; k++)
	{
	  uint64_t v = rd_u64 (img, (size_t) k * thr_geom.stride + off);

	  if (v == 0)
	    {
	      continue;
	    }
	  if (!VALID_PTR (v))
	    {
	      ok = 0;
	      break;
	    }
	  vals[nz++] = v;
	}
      if (!ok || nz < CS_MIN_SAMPLES || nz > nthr + 4 * THR_ID_SLACK)
	{
	  continue;
	}
      qsort (vals, (size_t) nz, sizeof (uint64_t), cmp_u64);
      for (k = 1; k < nz && ok; k++)
	{
	  ok = vals[k] != vals[k - 1];
	}
      if (ok)
	{
	  cand[n++] = off;
	  if (nz <= nthr - 1 && nz >= nthr - 1 - THR_ID_SLACK)
	    {
	      *strict = nstrict++ == 0 ? off : -2;
	    }
	}
    }
  free (vals);
  return n;
}

int
thr_probe_tid_at (const int *tasks, int ntask, const unsigned char *img,
		  int id_off, int *tid_off)
{
  int k, off, live = 0, ns = 0, step, found = 0, best = 0, n = 0, need;
  unsigned char *win = NULL, *okv = NULL;
  int *idx = NULL, *seen = NULL;
  uint64_t *addrs = NULL;
  int32_t *vals = NULL;

  idx = malloc ((size_t) thr_geom.count * sizeof (int));
  for (k = 0; idx != NULL && k < thr_geom.count; k++)
    {
      if (rd_u64 (img, (size_t) k * thr_geom.stride + (size_t) id_off) != 0)
	{
	  idx[live++] = k;
	}
    }
  need = (live < ntask - 1 ? live : ntask - 1) - THR_ID_SLACK;
  need = need > CS_MIN_SAMPLES ? need : CS_MIN_SAMPLES;
  if (idx != NULL && live >= need)
    {
      win = malloc ((size_t) THR_TID_SAMPLE * THR_TID_SCAN);
      seen = malloc ((size_t) live * sizeof (int));
    }
  if (win == NULL || seen == NULL)
    {
      free (idx);
      free (win);
      free (seen);
      return 0;
    }
  /* stage 1: a spread sample of live entries */
  step = live / THR_TID_SAMPLE > 0 ? live / THR_TID_SAMPLE : 1;
  for (k = 0; k < live && ns < THR_TID_SAMPLE; k += step)
    {
      uint64_t mid = rd_u64 (img, (size_t) idx[k] * thr_geom.stride + (size_t) id_off);

      if (mem_read (mid, win + (size_t) ns * THR_TID_SCAN, THR_TID_SCAN) == 0)
	{
	  ns++;
	}
    }
  for (off = 0; ns >= CS_MIN_SAMPLES && off + 4 <= THR_TID_SCAN; off += 4)
    {
      int m = 0, j, ok = 1;

      for (k = 0; k < ns && ok; k++)
	{
	  int v = rd_i32 (win, (size_t) k * THR_TID_SCAN + (size_t) off);

	  if (v <= 0 || !os_task_known (tasks, ntask, v))
	    {
	      continue;
	    }
	  for (j = 0; j < m && ok; j++)
	    {
	      ok = seen[j] != v;
	    }
	  seen[m++] = v;
	}
      if (ok && m >= CS_MIN_SAMPLES && m >= ns - THR_ID_SLACK)
	{
	  found++;
	  best = off;
	}
    }
  /* stage 2: every live entry at that offset, in one gather */
  if (found == 1)
    {
      addrs = malloc ((size_t) live * sizeof (uint64_t));
      vals = malloc ((size_t) live * sizeof (int32_t));
      okv = malloc ((size_t) live);
      if (addrs == NULL || vals == NULL || okv == NULL)
	{
	  found = 0;
	}
      for (k = 0; found == 1 && k < live; k++)
	{
	  addrs[k] = rd_u64 (img, (size_t) idx[k] * thr_geom.stride + (size_t) id_off);
	}
      /* the field behind each m_id (struct pthread) */
      if (found == 1 && mem_gather_field (addrs, (size_t) live, (uint64_t) best, 4,
					  NULL, vals, okv) != 0)
	{
	  found = 0;
	}
    }
  for (k = 0; found == 1 && k < live; k++)
    {
      int32_t v = vals[k];
      int j;

      if (!okv[k] || v <= 0 || !os_task_known (tasks, ntask, v))
	{
	  continue;
	}
      for (j = 0; j < n; j++)
	{
	  if (seen[j] == v)
	    {
	      found = 0;	/* two entries naming one thread: not this field */
	      break;
	    }
	}
      seen[n++] = v;
    }
  free (idx);
  free (win);
  free (seen);
  free (addrs);
  free (vals);
  free (okv);
  if (found != 1 || n < need)
    {
      return 0;
    }
  *tid_off = best;
  return n;
}

/* a suspended code of thread_resume_suspend_status (3, 5, .., 25, 26) */
int
resume_suspended (int v)
{
  return (v >= 3 && v <= 25 && (v & 1)) || v == 26;
}

/* the resume_status offset, or -1; *provisional when only the weak rule
 * (an idle server's two codes) picked it */
int
thr_probe_resume_pass (const FIELD * f, const unsigned char *img, int ignore,
		       int *provisional)
{
  int off, found = 0, best = 0, found_odd = 0, best_odd = 0;
  int weak = 0, best_weak = 0;

  for (off = thr_geom.off_index + thr_geom.off_tran + 4;
       off + 4 <= thr_geom.stride; off += 4)
    {
      int k, ok = 1, hi = 0, distinct = 0, odd = 0;
      unsigned seen = 0;

      if (field_skips (f, off, ignore))
	{
	  continue;		/* dropped by the runtime anchor before */
	}

      for (k = 0; k < thr_geom.count && ok; k++)
	{
	  int32_t v = rd_i32 (img, (size_t) k * thr_geom.stride + (size_t) off);

	  ok = v >= 0 && v < THR_RESUME_MAX;
	  if (ok && thr_slot_live (k) && !(seen & (1u << v)))
	    {
	      seen |= 1u << v;
	      distinct++;
	      hi = v > hi ? v : hi;
	      odd |= resume_suspended (v);
	    }
	}
      /* resume codes of several kinds: at least three distinct values,
       * reaching the CS range, sparse (a counter fills 0..n; codes skip).
       * A suspended (odd) code breaks a tie between such offsets. */
      if (ok && distinct >= 2 && hi >= THR_RESUME_CS_MIN
	  && distinct <= hi / 2 + 1)
	{
	  weak++;
	  best_weak = off;
	  if (distinct >= 3)
	    {
	      found++;
	      best = off;
	      if (odd)
		{
		  found_odd++;
		  best_odd = off;
		}
	    }
	}
    }
  if (found > 1 && found_odd == 1)
    {
      found = 1;
      best = best_odd;
    }
  if (found != 1)
    {
      /* an idle server shows two codes only; a lone offset in the CS
       * range is taken provisionally and re-probed on the schedule, the
       * runtime anchor (CS queue heads) still drops it on mismatch */
      if (weak != 1)
	{
	  return -1;		/* a previous (weak) lock stays as it is */
	}
      *provisional = 1;
      return best_weak;
    }
  *provisional = 0;
  return best;
}

/* the m_status offset, or -1 */
int
thr_probe_status_pass (const FIELD * f, const unsigned char *img, int ignore,
		       int *provisional)
{
  int off, found = 0, best = 0, hi;

  *provisional = 0;
  if (!thr_geom.has_resume)
    {
      return -1;		/* bounded by resume_status: probed after it */
    }
  hi = thr_geom.off_resume;
  for (off = thr_geom.off_index + thr_geom.off_tran + 4; off + 4 <= hi; off += 4)
    {
      int k, ok = 1, run = 0;

      for (k = 0; k < thr_geom.count && ok; k++)
	{
	  int32_t v = rd_i32 (img, (size_t) k * thr_geom.stride + (size_t) off);
	  int live = thr_slot_live (k);

	  ok = v >= 0 && v <= 4 && (!live || v >= 1);
	  run += live && v == 2;
	}
      if (ok && run > 0 && !field_skips (f, off, ignore))
	{
	  found++;
	  best = off;
	}
    }
  return found == 1 ? best : -1;
}

/* a thread queued as a CS writer / promoter right now: suspended (I-20)
 * with a writer or promoter code; without I-20, a CS code (I-18) */
static int
cs_writer_waiting (int status, int code)
{
  if (thr_geom.has_status)
    {
      return status == TS_WAIT && (!thr_geom.has_resume || code == 9 || code == 11);
    }
  return !thr_geom.has_resume || (code >= THR_RESUME_CS_MIN && code <= THR_RESUME_CS_MAX);
}

/* waiting as a CS writer, from an entry copy whose start is at buf + base
 * (an image: k * stride; a hop span: -lo) */
int
buf_cs_writer_waiting (const unsigned char *buf, long base)
{
  return cs_writer_waiting (thr_geom.has_status
			    ? rd_i32 (buf, (size_t) (base + thr_geom.off_status)) : -1,
			    thr_geom.has_resume
			    ? rd_i32 (buf, (size_t) (base + thr_geom.off_resume)) : -1);
}

int
thr_next_static (const unsigned char *img, int off)
{
  int k, nz = 0;

  for (k = 0; k < thr_geom.count; k++)
    {
      size_t e = (size_t) k * thr_geom.stride;
      uint64_t v = rd_u64 (img, e + (size_t) off);
      int j;

      if (v == 0)
	{
	  continue;
	}
      /* a link to another entry, checked against the image itself */
      j = slot_of_ptr (v);
      if (j < 0 || j == k || rd_i32 (img, (size_t) j * thr_geom.stride + thr_geom.off_index) != j + 1)
	{
	  return -1;
	}
      /* other entry links (tran_next_wait: lock waiters) sit nearby: count
       * only a link between two threads waiting on a critical section */
      nz += buf_cs_writer_waiting (img, (long) k * thr_geom.stride)
	&& buf_cs_writer_waiting (img, (long) j * thr_geom.stride);
    }
  return nz;
}

static int
cmp_u64 (const void *a, const void *b)
{
  uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;

  return x < y ? -1 : x > y;
}
