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
 * live sampler - the per-sample counts of a window (LIVE_WIN): occupancy, waits, activity, queue walks, OS states
 */

#include "ctv.h"

/* a live window: fixed per-CS arrays, the raw array image sized by stride */
LIVE_WIN *
live_win_new (int count, int stride)
{
  LIVE_WIN *w = calloc (1, sizeof (LIVE_WIN));

  if (w == NULL || (w->blob = malloc ((size_t) stride * count)) == NULL)
    {
      die ("out of memory", "메모리 부족", NULL);
    }
  return w;
}

void
live_win_free (LIVE_WIN * w)
{
  free (w->blob);
  free (w);
}

void
live_win_reset (LIVE_WIN * w, int count)
{
  int i;

  win_stats_clear (&w->st);
  for (i = 0; i < count; i++)
    {
      w->wq_last[i] = 0;
      w->ch_n[i] = 0;
      w->own_last[i] = 0;
      w->own_slot[i] = -1;
    }
  memset (w->act_ix, 0, sizeof (w->act_ix));
  w->nos = w->nrd = 0;
  if (!w->no_disk)
    {
      view_disk_begin (w);
    }
}

/* one scheduler-state sample of the thread in slot k while it holds CS cs:
 * procfs is read once per thread (20 Hz at most) and the state counted on
 * the (thread, CS) entry, so a CS's figures never mix in what its holder
 * did under another CS */
static void
os_sample (LIVE_WIN * w, int k, int cs)
{
  OS_THR *o = NULL;
  OS_RD *rd = NULL;
  int j, tid = thr_tid_of (k);

  if (tid <= 0)
    {
      return;
    }
  for (j = 0; j < w->nos; j++)
    {
      if (w->os[j].slot == k && w->os[j].cs == cs)
	{
	  o = &w->os[j];
	  break;
	}
    }
  if (o == NULL)
    {
      if (w->nos >= OS_MAX)
	{
	  return;
	}
      o = &w->os[w->nos++];
      memset (o, 0, sizeof (*o));
      o->slot = k;
      o->cs = cs;
      o->tid = tid;
      (void) os_task_comm (target_pid, tid, o->comm, sizeof (o->comm));
    }
  for (j = 0; j < w->nrd; j++)
    {
      if (w->rd[j].slot == k)
	{
	  rd = &w->rd[j];
	  break;
	}
    }
  if (rd == NULL)
    {
      if (w->nrd >= OS_RD_MAX)
	{
	  return;
	}
      rd = &w->rd[w->nrd++];
      memset (rd, 0, sizeof (*rd));
      rd->slot = k;
      rd->seq = -1;
    }
  /* decided once a sample for the thread: every CS it holds in this sample
   * shares the reading, or none of them counts */
  if (rd->seq != w->st.samples)
    {
      double now = mono_s ();

      rd->seq = w->st.samples;
      if (rd->t > 0 && now - rd->t < OS_SAMPLE_MIN_S)
	{
	  rd->ok = 0;
	}
      else
	{
	  rd->t = now;		/* a failing read is throttled too */
	  rd->ok = os_task_state (target_pid, tid, &rd->st, &rd->ticks) == 0;
	}
    }
  if (!rd->ok)
    {
      return;
    }
  if (o->samples == 0)
    {
      o->ticks0 = rd->ticks;
      o->t_first = rd->t;
    }
  o->t_last = rd->t;
  o->ticks1 = rd->ticks;
  o->span_ms = (int) ((rd->t - o->t_first) * 1000.0 + 0.5);
  o->samples++;
  if (rd->st == 'R')
    {
      o->run++;
    }
  else if (rd->st == 'D')
    {
      o->dsk++;
    }
  else
    {
      o->oth++;
    }
}

/* client pid of a tran's slot, read at most every OS_SAMPLE_MIN_S: a slot
 * that changes hands in the window keeps each client's sightings apart */
static int
client_pid (int tran)
{
  static int *pid = NULL;
  static double *at = NULL;
  static int n = 0;
  double t;

  if (tran < 1 || tran_geom.ntran <= 0)
    {
      return -1;
    }
  if (n != tran_geom.ntran)
    {
      free (pid);
      free (at);
      n = tran_geom.ntran;
      pid = malloc ((size_t) n * sizeof (int));
      at = calloc ((size_t) n, sizeof (double));
      if (pid == NULL || at == NULL)
	{
	  free (pid);
	  free (at);
	  pid = NULL;
	  at = NULL;
	  n = 0;
	  return -1;
	}
    }
  if (tran >= n)
    {
      return -1;
    }
  t = mono_s ();
  if (at[tran] == 0.0 || t - at[tran] >= OS_SAMPLE_MIN_S)
    {
      pid[tran] = tran_client_pid (tran);
      at[tran] = t;
    }
  return pid[tran];
}

/* count one sighting of tran (client pid cpid) holding / waiting for CS cs */
static void
act_add (LIVE_WIN * w, int cs, int kind, int tran, int thr, int cpid)
{
  /* engine threads (tran 0) and unknown trans (-1) are kept apart by
   * thread: the verdict names the one that held most, and no activity is
   * seen twice in one sample */
  int key_thr = tran <= 0 ? thr : -1;
  unsigned h = ((unsigned) cs * 31u + (unsigned) kind * 7u
		+ (unsigned) tran * 2654435761u + (unsigned) (key_thr + 1) * 40503u
		+ (unsigned) (cpid + 1) * 97u) & (ACT_HASH - 1);

  /* open addressing over act[]: slot = index + 1, 0 = empty */
  for (; w->act_ix[h] != 0; h = (h + 1) & (ACT_HASH - 1))
    {
      ACT *a = &w->st.act[w->act_ix[h] - 1];

      if (a->cs == cs && a->kind == kind && a->tran == tran && a->cpid == cpid
	  && (tran > 0 || a->thr == key_thr))
	{
	  a->samples++;
	  if (a->thr < 0)
	    {
	      a->thr = thr;
	    }
	  return;
	}
    }
  /* waiters cannot fill the reserve: a holder seen late is still kept */
  if (w->st.nact < (kind == ACT_HELD ? ACT_MAX : ACT_MAX - ACT_HELD_RESERVE))
    {
      ACT *a = &w->st.act[w->st.nact++];

      a->cs = cs;
      a->kind = kind;
      a->tran = tran;
      a->thr = thr;
      a->samples = 1;
      a->cpid = cpid;
      w->act_ix[h] = (unsigned short) w->st.nact;
    }
  else
    {
      w->st.act_dropped++;
    }
}

/* one sample: occupancy / waiter presence and the writer's tran index */

void
live_accumulate (LIVE_WIN * w, const CS_LAYOUT * lo)
{
  int i;

  for (i = 0; i < lo->count; i++)
    {
      int32_t rwlock, tran;
      uint32_t wr, ww;

      size_t off = (size_t) i * lo->stride;
      uint64_t owner;
      int held;

      cs_decode_raw (w->blob, lo, i, &rwlock, &wr, &ww, &tran);
      owner = rd_u64 (w->blob, off + lo->off_owner);
      held = cs_held (rwlock, owner);	/* a writer, or its owner demoted */
      if (rwlock != 0)
	{
	  w->st.occ[i]++;
	}
      /* a holder without a transaction (tran -1) is an engine thread:
       * counted like the system tran */
      if (held && tran < 0)
	{
	  tran = 0;
	}
      /* prefer a client tran (>=1) over the system tran (0) */
      if (held)
	{
	  int k, cpid = client_pid (tran);

	  if (tran >= 1 || (tran == 0 && w->st.seen_tran[i] < 0))
	    {
	      w->st.seen_tran[i] = tran;
	      w->st.seen_cpid[i] = cpid;
	      w->st.seen_owner[i] = owner;
	    }

	  /* the owner's slot is looked up once per distinct owner tid; a torn
	   * sample may still read owner 0 */
	  if (owner == 0)
	    {
	      k = -1;
	    }
	  else
	    {
	      if (owner != w->own_last[i])
		{
		  w->own_last[i] = owner;
		  w->own_slot[i] = thr_index_of_tid_mem (owner);
		}
	      k = w->own_slot[i];
	    }
	  act_add (w, i, ACT_HELD, tran, k, cpid);
	  if (k >= 0)
	    {
	      os_sample (w, k, i);
	    }
	}
      if (wr != 0 || ww != 0)
	{
	  uint64_t head2;
	  int field;
	  uint64_t head = cs_wait_queues (w->blob, lo, i, &head2, &field);

	  w->st.wai[i]++;
	  /* the queue (slots and the trans they wait for, read now - a served
	   * worker takes other work): a new head at most 20 times a second per
	   * CS, the same head again every CHAIN_REWALK_S (a stall's tail grows) */
	  if (head != 0)
	    {
	      int q, fresh = head != w->wq_last[i];
	      double t = mono_s ();

	      if (every (&w->ch_walked[i], t, fresh ? OS_SAMPLE_MIN_S : CHAIN_REWALK_S))
		{
		  /* a new head is checked against the CS field it came from */
		  uint64_t at = w->cs_base + (uint64_t) i * (uint64_t) lo->stride
		    + (uint64_t) field;

		  w->ch_n[i] = thr_waiters_mem2 (head, head2,
						 w->ch_slot[i], w->ch_tran[i],
						 THR_WAITERS_MAX, fresh ? at : 0);
		  for (q = 0; q < w->ch_n[i]; q++)
		    {
		      w->ch_cpid[i][q] = client_pid (w->ch_tran[i][q]);
		    }
		  /* a head not walked (throttled, or no hop accepted) is
		   * retried at the next chance */
		  w->wq_last[i] = w->ch_n[i] > 0 ? head : 0;
		  if ((w->st.seen_wq[i] == 0 || w->st.seen_wq[i] == head) && w->ch_n[i] > 0)
		    {
		      /* the window's first queue, kept for display */
		      w->st.seen_wq[i] = head;
		      w->st.seen_n[i] = w->ch_n[i];
		      memcpy (w->st.seen_slot[i], w->ch_slot[i],
			      (size_t) w->ch_n[i] * sizeof (int));
		      memcpy (w->st.seen_tran_of[i], w->ch_tran[i],
			      (size_t) w->ch_n[i] * sizeof (int));
		      memcpy (w->st.seen_cpid_of[i], w->ch_cpid[i],
			      (size_t) w->ch_n[i] * sizeof (int));
		    }
		}
	      /* only the chain of the current head is charged */
	      for (q = 0; head == w->wq_last[i] && q < w->ch_n[i]; q++)
		{
		  act_add (w, i, ACT_WAIT, w->ch_tran[i][q], w->ch_slot[i][q],
			   w->ch_cpid[i][q]);
		}
	    }
	  else
	    {
	      w->wq_last[i] = 0;	/* a later head is resolved afresh */
	    }
	}
      else
	{
	  w->wq_last[i] = 0;
	}
    }
  w->st.samples++;
}
