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
 * critical sections - the CS array layout probe (I-0..I-5), its rows and wait queues
 */

#include "ctv.h"

/* ------------------------------------------------------------- SYNC_STATS */

static double
tv_to_ms (const unsigned char *b, size_t off)
{
  return (double) rd_i64 (b, off) * 1000.0
    + (double) rd_i64 (b, off + 8) / 1000.0;
}

/* I-5: decode one SYNC_STATS image; -1 when the type does not validate */
int
stats_decode (const unsigned char *s, STATS_VIEW * v)
{
  v->type = rd_i32 (s, ST_OFF_TYPE);
  if (!ST_TYPE_OK (v->type))
    {
      return -1;
    }
  v->name_ptr = rd_u64 (s, ST_OFF_NAME);
  v->nenter = rd_u32 (s, ST_OFF_NENTER);
  v->nwait = rd_u32 (s, ST_OFF_NWAIT);
  v->nreenter = rd_u32 (s, ST_OFF_NREENTER);
  v->total_ms = tv_to_ms (s, ST_OFF_TOTAL_TV);
  v->max_ms = tv_to_ms (s, ST_OFF_MAX_TV);
  return 0;
}

static int
stats_read (uint64_t addr, STATS_VIEW * v)
{
  unsigned char s[STATS_SIZE];

  if (addr == 0 || mem_read (addr, s, sizeof (s)) != 0)
    {
      return -1;
    }
  return stats_decode (s, v);
}

/* ---------------------------------------------------------- CS geometry */

/* the array may sit near the end of a mapping: shrink until it reads */
static const char *
cs_probe_read (uint64_t cs_base, unsigned char *blob, size_t blobn,
	       size_t * got)
{
  memset (blob, 0, blobn);
  *got = mem_read_shrink (cs_base, blob, blobn, 2048);
  if (*got > 0)
    {
      return NULL;
    }
  REFUSE ("I-0 cs array unreadable");
}

/* I-1: stride = spacing that repeats cs_index 0,1,2,3 at +8 */
static const char *
cs_probe_stride (const unsigned char *blob, size_t got, CS_LAYOUT * lo)
{
  int cand, i;

  for (cand = 96; cand <= 256; cand += 8)
    {
      for (i = 0; i < CS_MIN_SAMPLES; i++)
	{
	  if ((size_t) (i * cand + CS_OFF_INDEX + 4) > got
	      || rd_i32 (blob, i * cand + CS_OFF_INDEX) != i)
	    {
	      break;
	    }
	}
      if (i == CS_MIN_SAMPLES)
	{
	  lo->stride = cand;
	  return NULL;
	}
    }
  REFUSE ("I-1 cs_index sequence");
}

/* CS names are engine string literals: plain ASCII, no spaces needed */
static int
ascii_name (const char *s)
{
  for (; *s != '\0'; s++)
    {
      if (*s < 0x21 || *s > 0x7e)
	{
	  return 0;
	}
    }
  return 1;
}

/* I-2: count = length of the run with cs_index == i and a printable name */
static const char *
cs_probe_count (uint64_t cs_base, const unsigned char *blob, size_t got,
		CS_LAYOUT * lo)
{
  char name[NAME_MAX_LEN];
  int i;

  lo->count = 0;
  for (i = 0; i < MAX_CS; i++)
    {
      unsigned char head[16];

      /* past the blob (array near a mapping end): read the slot directly */
      if ((size_t) ((i + 1) * lo->stride) <= got)
	{
	  memcpy (head, blob + (size_t) i * lo->stride, sizeof (head));
	}
      else if (mem_read (cs_base + (uint64_t) i * lo->stride, head,
			 sizeof (head)) != 0)
	{
	  break;
	}
      if (rd_i32 (head, CS_OFF_INDEX) != i
	  || mem_cstr (rd_u64 (head, CS_OFF_NAME), name, sizeof (name)) != 0
	  || !ascii_name (name))
	{
	  break;
	}
      lo->count = i + 1;
    }
  if (lo->count < CS_MIN_SAMPLES)
    {
      REFUSE ("I-2 cs count");
    }
  return NULL;
}

/* I-3: stats slot = unique 8-aligned offset whose target reads
 * type == CSECT and the SAME name pointer the CS carries, on every CS */
static const char *
cs_probe_stats_off (const unsigned char *blob, CS_LAYOUT * lo)
{
  int cand, i, found = 0, best = 0;

  for (cand = 16; cand <= lo->stride - 8; cand += 8)
    {
      for (i = 0; i < lo->count; i++)
	{
	  size_t off = (size_t) i * lo->stride;
	  STATS_VIEW v;

	  if (stats_read (rd_u64 (blob, off + cand), &v) != 0
	      || v.type != ST_TYPE_CSECT
	      || v.name_ptr != rd_u64 (blob, off + CS_OFF_NAME))
	    {
	      break;
	    }
	}
      if (i == lo->count)
	{
	  found++;
	  best = cand;
	}
    }
  if (found != 1)
    {
      REFUSE (found == 0 ? "I-3 stats pointer" : "I-3 stats pointer not unique");
    }
  lo->off_stats = best;
  return NULL;
}

/* I-4: derived from stats - rwlock trio -96, owner -16, writer queue -32,
 * promoter queue -24, tran -8 - and cross-checked: waiting counters sane,
 * queue pointers valid, and rwlock<0 => owner!=0 on every CS but at most
 * one (a torn read); demote leaves owner/tran set, so only that way.  tran
 * has no check of its own */
static const char *
cs_probe_rwlock (const unsigned char *blob, CS_LAYOUT * lo)
{
  int i, viol = 0;

  lo->off_tran = lo->off_stats - 8;
  lo->off_owner = lo->off_stats - CS_STATS_TO_OWNER;
  lo->off_wq = lo->off_stats - CS_STATS_TO_WQ;
  lo->off_pq = lo->off_stats - CS_STATS_TO_PQ;
  lo->off_rwlock = lo->off_stats - CS_STATS_TO_RWLOCK;
  if (lo->off_rwlock < 16)
    {
      REFUSE ("I-4 rwlock offset");
    }
  for (i = 0; i < lo->count; i++)
    {
      size_t off = (size_t) i * lo->stride;
      int held = rd_i32 (blob, off + lo->off_rwlock) < 0;
      int tid_set = rd_u64 (blob, off + lo->off_owner) != 0;
      uint64_t wq = rd_u64 (blob, off + lo->off_wq);
      uint64_t pq = rd_u64 (blob, off + lo->off_pq);

      if (rd_u32 (blob, off + lo->off_rwlock + 4) > CS_WAIT_SANE_MAX
	  || rd_u32 (blob, off + lo->off_rwlock + 8) > CS_WAIT_SANE_MAX)
	{
	  REFUSE ("I-4 waiting counters");
	}
      if ((wq != 0 && !VALID_PTR (wq)) || (pq != 0 && !VALID_PTR (pq)))
	{
	  REFUSE ("I-4 waiter queue pointer");
	}
      /* a writer always records its owner; demote and reader exits leave
       * owner / tran set, so only this direction holds */
      viol += held && !tid_set;
    }
  if (viol >= 2)
    {
      REFUSE ("I-4 writer without owner");
    }
  return NULL;
}

/* waiting writers in the order the engine serves them: the promoters
 * queue (readers promoting to writer, woken first), then the writers
 * queue; both count in waitW.  Returns the first head; *head2 = the writers
 * queue behind the promoters (or 0), *field = the CS offset the first head
 * was read from (either may be NULL). */
uint64_t
cs_wait_queues (const unsigned char *blob, const CS_LAYOUT * lo, int i,
		uint64_t * head2, int *field)
{
  size_t off = (size_t) i * lo->stride;
  uint64_t pq = rd_u64 (blob, off + lo->off_pq), wq = rd_u64 (blob, off + lo->off_wq);

  if (head2 != NULL)
    {
      *head2 = pq != 0 ? wq : 0;
    }
  if (field != NULL)
    {
      *field = pq != 0 ? lo->off_pq : lo->off_wq;
    }
  return pq != 0 ? pq : wq;
}

const char *
cs_layout_probe (uint64_t cs_base, CS_LAYOUT * lo)
{
  unsigned char blob[MAX_CS * 256];
  size_t got = 0, need;
  int tries;
  const char *why;

  if ((why = cs_probe_read (cs_base, blob, sizeof (blob), &got)) != NULL
      || (why = cs_probe_stride (blob, got, lo)) != NULL
      || (why = cs_probe_count (cs_base, blob, got, lo)) != NULL)
    {
      return why;
    }
  need = (size_t) lo->count * lo->stride;
  if (need > got && mem_read (cs_base, blob, need) != 0)
    {
      REFUSE ("I-0 cs array read");
    }
  if ((why = cs_probe_stats_off (blob, lo)) != NULL)
    {
      return why;
    }
  /* rwlock and owner are stored apart: a lockless read can tear, so retry;
   * the reason of the last try stands */
  for (tries = 0; tries < CS_PAIR_RETRIES; tries++)
    {
      if (tries > 0 && mem_read (cs_base, blob, need) != 0)
	{
	  REFUSE ("I-0 cs array read");	/* a torn read: look again */
	}
      if ((why = cs_probe_rwlock (blob, lo)) == NULL)
	{
	  return NULL;
	}
    }
  return why;
}

/* --------------------------------------------------------------- CS rows */

/* the live fields of CS i out of a raw array image */
void
cs_decode_raw (const unsigned char *blob, const CS_LAYOUT * lo, int i,
	       int32_t * rwlock, uint32_t * wr, uint32_t * ww, int32_t * tran)
{
  size_t off = (size_t) i * lo->stride;

  *rwlock = rd_i32 (blob, off + lo->off_rwlock);
  *wr = rd_u32 (blob, off + lo->off_rwlock + 4);
  *ww = rd_u32 (blob, off + lo->off_rwlock + 8);
  *tran = rd_i32 (blob, off + lo->off_tran);
}



int
cs_read_rows (uint64_t cs_base, const CS_LAYOUT * lo, CS_ROW * rows)
{
  unsigned char *blob = malloc ((size_t) lo->stride * lo->count);
  int rc;

  if (blob == NULL
      || mem_read (cs_base, blob, (size_t) lo->stride * lo->count) != 0)
    {
      free (blob);
      return -1;
    }
  rc = cs_rows_from_blob (blob, lo, rows);
  free (blob);
  return rc;
}

/* rows of an array image already read (the live sample of the window end) */
int
cs_rows_from_blob (const unsigned char *blob, const CS_LAYOUT * lo, CS_ROW * rows)
{
  uint64_t saddr[MAX_CS] = { 0 };
  unsigned char sblk[MAX_CS * STATS_SIZE];
  int i, count = lo->count < 0 ? 0 : lo->count < MAX_CS ? lo->count : MAX_CS;

  /* every stats block in one gather */
  for (i = 0; i < count; i++)
    {
      saddr[i] = rd_u64 (blob, (size_t) i * lo->stride + lo->off_stats);
    }
  (void) mem_gather_ok (saddr, (size_t) count, STATS_SIZE, sblk, NULL);
  memset (rows, 0, (size_t) count * sizeof (CS_ROW));
  for (i = 0; i < count; i++)
    {
      size_t off = (size_t) i * lo->stride;
      CS_ROW *r = &rows[i];
      STATS_VIEW v;

      (void) name_of (rd_u64 (blob, off + CS_OFF_NAME), r->name,
		      sizeof (r->name));
      cs_decode_raw (blob, lo, i, &r->rwlock, &r->wr, &r->ww, &r->tran);
      r->owner = rd_u64 (blob, off + lo->off_owner);
      r->wq = cs_wait_queues (blob, lo, i, &r->wq2, NULL);
      if (saddr[i] != 0 && stats_decode (sblk + (size_t) i * STATS_SIZE, &v) == 0)
	{
	  r->has_stats = 1;
	  r->nenter = v.nenter;
	  r->nwait = v.nwait;
	  r->nreenter = v.nreenter;
	  r->total_ms = v.total_ms;
	  r->max_ms = v.max_ms;
	}
    }
  return 0;
}

/* table tokens stay English in every language (double-width Hangul would
 * break the columns) */
const char *
state_str (int32_t rwlock, char *buf, size_t n)
{
  if (rwlock > 0)
    {
      snprintf (buf, n, "%d readers", rwlock);
    }
  else if (rwlock < 0)
    {
      snprintf (buf, n, "1 writer");
    }
  else
    {
      snprintf (buf, n, "none");
    }
  return buf;
}
