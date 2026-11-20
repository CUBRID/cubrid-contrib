/*
 * t22: the disk/CPU figures of a CS are those of its holder while it held
 * that CS.  A thread's procfs state is read once per sample at most and
 * counted on the (thread, CS) entry, so what a thread did under another CS
 * is never mixed in, and the verdict takes the entry of the holder it
 * names - or none.
 *   1 thread 10 (tran 5) holds LOG in 20 samples in disk sleep, then CS 0 in
 *     79 running; thread 11 (tran 6) holds LOG in 75 in disk sleep and CS 0
 *     once (both in that sample): four entries, one read a sample per
 *     thread, the verdict on CS 0 = thread 10 under CS 0 (R), not its LOG
 *     entry (D) nor thread 11
 *   2 20 Hz a thread: 20 samples 10 ms apart read 4 times
 *   3 a new window forgets the readings of the last one
 *   4 a failing read is throttled like any other and counts nowhere
 *   5 a thread holding two sections: both entries get the same samples,
 *     also when the clock passes the 50 ms mark between them
 *   6 readers hold most of the CS: no holder is named, no thread state
 *   7 the holder's thread is unknown: no other thread's state instead
 *   8 more threads than readings kept (OS_RD_MAX): names only past it
 * Target reads are replaced (ld --wrap): thread ids, procfs, the clock.
 */
#include "../src/ctv_sampler.c"

#define OWNER_A 0x7f000000a000ULL	/* thread slot 10 */
#define OWNER_B 0x7f000000b000ULL	/* thread slot 11 */
#define OWNER_C 0x7f000000c000ULL	/* no thread slot known */
#define OWNER_N 0x7f0001000000ULL	/* + n * 0x1000: thread slot 100 + n */

static double g_now = 100.0, g_tick = 0.0;	/* g_tick: time a clock read takes */
static int g_reads[2], g_reads_other;	/* procfs state reads of slot 10 / 11 / the rest */
static char g_state[2] = { 'R', 'D' };
static int g_fail;

double
__wrap_mono_s (void)
{
  double t = g_now;

  g_now += g_tick;
  return t;
}

int
__wrap_thr_index_of_tid_mem (uint64_t tid)
{
  return tid == OWNER_A ? 10 : tid == OWNER_B ? 11
    : tid >= OWNER_N ? 100 + (int) ((tid - OWNER_N) / 0x1000) : -1;
}

int
__wrap_thr_tid_of (int slot)
{
  return slot == 10 || slot == 11 || slot >= 100 ? 1000 + slot : 0;
}

int
__wrap_os_task_comm (int pid, int tid, char *out, size_t n)
{
  (void) pid;
  snprintf (out, n, "worker-%d", tid - 1000);
  return 0;
}

int
__wrap_os_task_state (int pid, int tid, char *state, long *ticks)
{
  (void) pid;
  if (tid == 1010 || tid == 1011)
    {
      g_reads[tid - 1010]++;
      *state = g_state[tid - 1010];
    }
  else
    {
      g_reads_other++;
      *state = 'S';
    }
  *ticks = 500;
  return g_fail ? -1 : 0;
}

static CS_LAYOUT lo;

static void
put_cs (LIVE_WIN * w, int i, int32_t rwlock, uint32_t ww, uint64_t owner, int32_t tran)
{
  unsigned char *p = w->blob + (size_t) i * lo.stride;

  memset (p + lo.off_rwlock, 0, 12);
  memcpy (p + lo.off_rwlock, &rwlock, 4);
  memcpy (p + lo.off_rwlock + 8, &ww, 4);
  memcpy (p + lo.off_owner, &owner, 8);
  memcpy (p + lo.off_tran, &tran, 4);
}

static void
put_free (LIVE_WIN * w, int i)
{
  put_cs (w, i, 0, 0, 0, -1);
}

static const OS_THR *
os_of (const LIVE_WIN * w, int slot, int cs)
{
  int j;

  for (j = 0; j < w->nos; j++)
    {
      if (w->os[j].slot == slot && w->os[j].cs == cs)
	{
	  return &w->os[j];
	}
    }
  return NULL;
}

#define CHECK(c, ...) do { if (!(c)) { printf ("t22 FAIL: " __VA_ARGS__); printf ("\n"); bad = 1; } } while (0)
#define SAMPLES(o) ((o) != NULL ? (o)->samples : -1)

int
main (int argc, char **argv)
{
  CS_ROW rows[2];
  VIEW_HDR h;
  LIVE_WIN *w;
  CS_FIG f;
  VERDICT v;
  const OS_THR *a, *b, *c, *d;
  int i, k, bad = 0, r0, r1;

  if (argc != 2)
    {
      return 2;
    }
  memset (&lo, 0, sizeof (lo));
  lo.stride = 160;
  lo.count = 2;
  lo.off_stats = 152;
  lo.off_rwlock = lo.off_stats - CS_STATS_TO_RWLOCK;
  lo.off_owner = lo.off_stats - CS_STATS_TO_OWNER;
  lo.off_tran = lo.off_stats - 8;
  lo.off_wq = lo.off_stats - CS_STATS_TO_WQ;
  lo.off_pq = lo.off_stats - CS_STATS_TO_PQ;
  memset (rows, 0, sizeof (rows));
  snprintf (rows[0].name, sizeof (rows[0].name), "HEAP_CHNGUESS");
  snprintf (rows[1].name, sizeof (rows[1].name), "LOG");
  for (i = 0; i < 2; i++)
    {
      rows[i].tran = -1;
      rows[i].has_stats = 1;
    }
  memset (&h, 0, sizeof (h));
  h.lo = lo;
  h.live = 1;
  h.hz = 100;
  h.interval = 1.0;
  snprintf (h.db, sizeof (h.db), "t22");
  if (view_record_open (argv[1], &h, 0) != 0)
    {
      return 1;
    }
  w = live_win_new (lo.count, lo.stride);
  w->no_disk = 1;

  /* 1: 100 samples 60 ms apart (every one may read procfs) */
  live_win_reset (w, lo.count);
  memset (w->blob, 0, (size_t) lo.stride * lo.count);
  for (k = 0; k < 100; k++)
    {
      g_now += 0.06;
      g_state[0] = k < 20 ? 'D' : 'R';
      if (k < 20)
	{
	  put_free (w, 0);
	  put_cs (w, 1, -1, 0, OWNER_A, 5);	/* thread 10 writes the log */
	}
      else
	{
	  /* thread 10 holds CS 0; in sample 50 thread 11 holds both */
	  put_cs (w, 0, -1, k < 60, k == 50 ? OWNER_B : OWNER_A, k == 50 ? 6 : 5);
	  if (k < 95)
	    {
	      put_cs (w, 1, -1, 0, OWNER_B, 6);
	    }
	  else
	    {
	      put_free (w, 1);
	    }
	}
      live_accumulate (w, &lo);
    }
  a = os_of (w, 10, 1);
  b = os_of (w, 10, 0);
  c = os_of (w, 11, 1);
  d = os_of (w, 11, 0);
  CHECK (w->nos == 4 && a != NULL && b != NULL && c != NULL && d != NULL,
	 "entries: %d, want (10,LOG) (10,CS0) (11,LOG) (11,CS0)", w->nos);
  if (a != NULL && b != NULL && c != NULL && d != NULL)
    {
      CHECK (a->samples == 20 && a->dsk == 20, "thread 10 under LOG: %d samples, %d D (want 20, 20)", a->samples, a->dsk);
      CHECK (b->samples == 79 && b->run == 79, "thread 10 under CS 0: %d samples, %d R (want 79, 79)", b->samples, b->run);
      CHECK (c->samples == 75 && c->dsk == 75, "thread 11 under LOG: %d samples, %d D (want 75, 75)", c->samples, c->dsk);
      CHECK (d->samples == 1 && d->dsk == 1, "thread 11 under CS 0: %d samples (want 1)", d->samples);
    }
  CHECK (g_reads[0] == 99 && g_reads[1] == 75,
	 "procfs reads %d / %d (want 99 / 75: one per thread per sample, also holding two sections)",
	 g_reads[0], g_reads[1]);
  view_capture (&lo, rows, w, 6.0);
  CHECK (view.nos == 4, "frame keeps %d OS entries (want 4: no name-only copy of a sampled thread)", view.nos);
  cs_figures (0, 6.0, &f);
  verdict_compute (&f, NULL, &v);
  CHECK (v.holder == 5 && !v.readers_most && v.os != NULL && v.os->slot == 10 && v.os->cs == 0,
	 "verdict on CS 0: holder tran %d, disk/cpu from slot %d under CS %d (want tran 5, slot 10, CS 0)",
	 v.holder, v.os != NULL ? v.os->slot : -1, v.os != NULL ? v.os->cs : -1);
  CHECK (v.run_pct > 99.0 && v.dsk_pct < 1.0 && v.bound == VB_CPU,
	 "verdict on CS 0: D %.0f%% R %.0f%% bound %d (want R 100%%, CPU-bound: its LOG samples stay out)",
	 v.dsk_pct, v.run_pct, v.bound);
  view_record_frame ();

  /* 2: 20 samples 10 ms apart */
  live_win_reset (w, lo.count);
  r0 = g_reads[0];
  put_free (w, 1);
  for (k = 0; k < 20; k++)
    {
      g_now += 0.01;
      put_cs (w, 0, -1, 0, OWNER_A, 5);
      live_accumulate (w, &lo);
    }
  CHECK (g_reads[0] - r0 == 4 && SAMPLES (os_of (w, 10, 0)) == 4,
	 "throttle: %d reads, %d samples in 0.2 s (want 4, 4)", g_reads[0] - r0, SAMPLES (os_of (w, 10, 0)));

  /* 3: the last window saw thread 10 last in its sample 19; so does this one */
  live_win_reset (w, lo.count);
  r0 = g_reads[0];
  for (k = 0; k < 20; k++)
    {
      g_now += 0.06;
      if (k == 19)
	{
	  put_cs (w, 0, -1, 0, OWNER_A, 5);
	}
      else
	{
	  put_free (w, 0);
	}
      live_accumulate (w, &lo);
    }
  CHECK (g_reads[0] - r0 == 1 && SAMPLES (os_of (w, 10, 0)) == 1,
	 "new window: %d reads, %d samples (want 1, 1: a reading of the window before is not reused)",
	 g_reads[0] - r0, SAMPLES (os_of (w, 10, 0)));

  /* 4: reads that fail */
  live_win_reset (w, lo.count);
  r0 = g_reads[0];
  g_fail = 1;
  g_now += 1.0;
  for (k = 0; k < 20; k++)
    {
      g_now += 0.01;
      put_cs (w, 0, -1, 0, OWNER_A, 5);
      live_accumulate (w, &lo);
    }
  g_fail = 0;
  CHECK (g_reads[0] - r0 == 4 && SAMPLES (os_of (w, 10, 0)) == 0,
	 "failing reads: %d attempts, %d samples in 0.2 s (want 4, 0)", g_reads[0] - r0, SAMPLES (os_of (w, 10, 0)));

  /* 5: one thread holds both sections; the clock moves between the two */
  live_win_reset (w, lo.count);
  r1 = g_reads[1];
  g_now += 1.0;
  /* 20 ms a sample, and 20 ms pass in each clock read: seen from the
   * second section the 50 ms mark is passed when it is not from the first */
  g_tick = 0.02;
  for (k = 0; k < 100; k++)
    {
      g_now += 0.02;
      put_cs (w, 0, -1, 0, OWNER_B, 6);
      put_cs (w, 1, -1, 0, OWNER_B, 6);
      live_accumulate (w, &lo);
    }
  g_tick = 0.0;
  CHECK (SAMPLES (os_of (w, 11, 0)) == g_reads[1] - r1 && SAMPLES (os_of (w, 11, 1)) == g_reads[1] - r1
	 && g_reads[1] - r1 == 50,
	 "two sections a sample: %d / %d samples from %d reads (want 50 / 50 from 50)",
	 SAMPLES (os_of (w, 11, 0)), SAMPLES (os_of (w, 11, 1)), g_reads[1] - r1);

  /* 6: CS 0 read in 86 samples, written by thread 10 in 4 */
  live_win_reset (w, lo.count);
  g_now += 1.0;
  g_state[0] = 'R';
  put_free (w, 1);
  for (k = 0; k < 100; k++)
    {
      g_now += 0.06;
      if (k < 90 && k % 20 == 10)
	{
	  put_cs (w, 0, -1, 1, OWNER_A, 5);
	}
      else if (k < 90)
	{
	  put_cs (w, 0, 2, 1, 0, -1);
	}
      else
	{
	  put_free (w, 0);
	}
      live_accumulate (w, &lo);
    }
  CHECK (SAMPLES (os_of (w, 10, 0)) == 4, "readers: thread 10 sampled %d times (want 4)", SAMPLES (os_of (w, 10, 0)));
  view_capture (&lo, rows, w, 6.0);
  cs_figures (0, 6.0, &f);
  verdict_compute (&f, NULL, &v);
  CHECK (v.holder == 5 && v.readers_most && v.os == NULL && v.bound == VB_NONE,
	 "readers most: holder %d readers_most %d os %s bound %d (want 5, 1, none, %d)",
	 v.holder, v.readers_most, v.os != NULL ? v.os->comm : "none", v.bound, VB_NONE);

  /* 7: the holder's thread slot is not known; thread 11 held it a little */
  live_win_reset (w, lo.count);
  g_now += 1.0;
  for (k = 0; k < 100; k++)
    {
      g_now += 0.06;
      if (k < 60)
	{
	  put_cs (w, 0, -1, 1, OWNER_C, 7);
	}
      else if (k < 70)
	{
	  put_cs (w, 0, -1, 0, OWNER_B, 6);
	}
      else
	{
	  put_free (w, 0);
	}
      live_accumulate (w, &lo);
    }
  view_capture (&lo, rows, w, 6.0);
  cs_figures (0, 6.0, &f);
  verdict_compute (&f, NULL, &v);
  CHECK (v.holder == 7 && !v.readers_most && v.os == NULL && SAMPLES (os_of (w, 11, 0)) == 10,
	 "holder without a thread: holder %d os %s (want 7, none - not thread 11's %d samples)",
	 v.holder, v.os != NULL ? v.os->comm : "none", SAMPLES (os_of (w, 11, 0)));

  /* 8: 70 threads hold CS 0 in turn */
  live_win_reset (w, lo.count);
  g_now += 1.0;
  r0 = g_reads_other;
  for (k = 0; k < 70; k++)
    {
      g_now += 0.06;
      put_cs (w, 0, -1, 0, OWNER_N + (uint64_t) k * 0x1000, 0);
      live_accumulate (w, &lo);
    }
  CHECK (w->nrd == OS_RD_MAX && w->nos == 70 && g_reads_other - r0 == OS_RD_MAX
	 && SAMPLES (os_of (w, 100 + OS_RD_MAX - 1, 0)) == 1 && SAMPLES (os_of (w, 100 + OS_RD_MAX, 0)) == 0,
	 "70 threads: %d readings kept, %d entries, %d reads (want %d, 70, %d; names only past them)",
	 w->nrd, w->nos, g_reads_other - r0, OS_RD_MAX, OS_RD_MAX);

  live_win_free (w);
  return view_record_close () != 0 || bad;
}
