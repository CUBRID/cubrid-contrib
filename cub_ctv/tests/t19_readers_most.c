/*
 * t19: a holder is named only when it holds most of the occupancy.  Readers
 * leave no owner, so on a CS read 94% of the window a writer seen once must
 * not become "the session that holds it longest".  CS 0 is read most of the
 * time with client tran 3 writing in one sample; CS 1 is written by client
 * tran 5 in most of its occupancy; CS 2 is written by four clients in turn
 * and never read - writers sharing a CS are not readers, the one seen first
 * among the longest is its holder; CS 3 is written by tran 21 in exactly
 * half of its occupancy and read in the other half - not under half, so
 * its writer is named.  Writes a live record for the screen
 * check (t19_readers_most.sh).  The sampler is included for live_accumulate.
 */
#include "../src/ctv_sampler.c"

static void
put_cs (unsigned char *blob, const CS_LAYOUT * lo, int i, int32_t rwlock,
	uint32_t ww, uint64_t owner, int32_t tran)
{
  unsigned char *p = blob + (size_t) i * lo->stride;

  memset (p + lo->off_rwlock, 0, 12);
  memcpy (p + lo->off_rwlock, &rwlock, 4);
  memcpy (p + lo->off_rwlock + 8, &ww, 4);
  memcpy (p + lo->off_owner, &owner, 8);
  memcpy (p + lo->off_tran, &tran, 4);
}

int
main (int argc, char **argv)
{
  CS_LAYOUT lo;
  CS_ROW rows[4];
  VIEW_HDR h;
  LIVE_WIN *w;
  CS_FIG f;
  VERDICT v0, v1, v2, v3;
  int i, k, bad = 0;

  if (argc != 2)
    {
      return 2;
    }
  memset (&lo, 0, sizeof (lo));
  lo.stride = 160;
  lo.count = 4;
  lo.off_stats = 152;
  lo.off_rwlock = lo.off_stats - CS_STATS_TO_RWLOCK;
  lo.off_owner = lo.off_stats - CS_STATS_TO_OWNER;
  lo.off_tran = lo.off_stats - 8;
  lo.off_wq = lo.off_stats - CS_STATS_TO_WQ;
  lo.off_pq = lo.off_stats - CS_STATS_TO_PQ;
  memset (rows, 0, sizeof (rows));
  snprintf (rows[0].name, sizeof (rows[0].name), "LOCATOR_CLASSNAME_TABLE");
  snprintf (rows[1].name, sizeof (rows[1].name), "QPROC_QUERY_CACHE");
  snprintf (rows[2].name, sizeof (rows[2].name), "CT_OID_TABLE");
  snprintf (rows[3].name, sizeof (rows[3].name), "HA_SERVER_STATE");
  for (i = 0; i < 4; i++)
    {
      rows[i].rwlock = 0;
      rows[i].tran = -1;
      rows[i].has_stats = 1;
    }
  memset (&h, 0, sizeof (h));
  h.lo = lo;
  h.live = 1;
  h.hz = 100;
  h.interval = 1.0;
  snprintf (h.db, sizeof (h.db), "t19");
  if (view_record_open (argv[1], &h, 0) != 0)
    {
      return 1;
    }
  w = live_win_new (lo.count, lo.stride);
  w->no_disk = 1;
  live_win_reset (w, lo.count);
  memset (w->blob, 0, (size_t) lo.stride * lo.count);
  /* 100 samples.  CS 0: written by tran 3 once, read 93 more, a writer
   * waiting in 30.  CS 1: written by tran 5 in 60, read in 20, a writer
   * waiting in 10.  CS 2: written by trans 11..14, 20 samples each.
   * CS 3: written by tran 21 in 40, read in 40 */
  for (k = 0; k < 100; k++)
    {
      put_cs (w->blob, &lo, 0, k == 0 ? -1 : k < 94 ? 2 : 0, k < 30 ? 1 : 0,
	      k == 0 ? 0x7f0000003000ULL : 0, k == 0 ? 3 : -1);
      put_cs (w->blob, &lo, 1, k < 60 ? -1 : k < 80 ? 1 : 0, k < 10 ? 1 : 0,
	      k < 60 ? 0x7f0000005000ULL : 0, k < 60 ? 5 : -1);
      put_cs (w->blob, &lo, 2, k < 80 ? -1 : 0, 0,
	      k < 80 ? 0x7f0000011000ULL + (uint64_t) (k / 20) * 0x1000 : 0,
	      k < 80 ? 11 + k / 20 : -1);
      put_cs (w->blob, &lo, 3, k < 40 ? -1 : k < 80 ? 3 : 0, 0,
	      k < 40 ? 0x7f0000021000ULL : 0, k < 40 ? 21 : -1);
      live_accumulate (w, &lo);	/* counts the sample */
    }
  view_capture (&lo, rows, w, 1.0);
  cs_figures (0, 1.0, &f);
  verdict_compute (&f, NULL, &v0);
  cs_figures (1, 1.0, &f);
  verdict_compute (&f, NULL, &v1);
  cs_figures (2, 1.0, &f);
  verdict_compute (&f, NULL, &v2);
  cs_figures (3, 1.0, &f);
  verdict_compute (&f, NULL, &v3);
  if (v0.holder != 3 || !v0.readers_most)
    {
      printf ("t19 FAIL: CS read 94%% with tran 3 writing 1%%: holder %d, readers_most %d (want 3, 1)\n",
	      v0.holder, v0.readers_most);
      bad = 1;
    }
  if (v1.holder != 5 || v1.readers_most)
    {
      printf ("t19 FAIL: CS written by tran 5 in 60 of 80: holder %d, readers_most %d (want 5, 0)\n",
	      v1.holder, v1.readers_most);
      bad = 1;
    }
  if (v2.holder != 11 || v2.readers_most || v2.writers_pct < 79.5 || v2.writers_pct > 80.5)
    {
      printf ("t19 FAIL: CS written by four clients 20%% each, never read: holder %d, readers_most %d, writers %.0f%% (want 11, 0, 80)\n",
	      v2.holder, v2.readers_most, v2.writers_pct);
      bad = 1;
    }
  if (v3.holder != 21 || v3.readers_most || v3.writers_pct < 39.5 || v3.writers_pct > 40.5)
    {
      printf ("t19 FAIL: CS written in 40 and read in 40 of 80: holder %d, readers_most %d, writers %.0f%% (want 21, 0, 40)\n",
	      v3.holder, v3.readers_most, v3.writers_pct);
      bad = 1;
    }
  view_record_frame ();
  live_win_free (w);
  return view_record_close () != 0 || bad;
}
