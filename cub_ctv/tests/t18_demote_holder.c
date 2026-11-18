/*
 * t18: a CS demoted to a reader by its owner is still held by that thread.
 * The HA log flush takes LOG as a writer, demotes to a reader (owner and
 * tran_index stay set) while it writes and waits, then promotes back.  The
 * sampler must count the demoted span as the owner's hold, so the verdict's
 * daemon share matches LOG's occupancy; plain readers (no owner) are no
 * holder.  The sampler is included for live_accumulate.
 */
#include "../src/ctv_sampler.c"

static void
put_cs (unsigned char *blob, const CS_LAYOUT * lo, int i, int32_t rwlock,
	uint64_t owner, int32_t tran)
{
  unsigned char *p = blob + (size_t) i * lo->stride;

  memset (p + lo->off_rwlock, 0, 12);
  memcpy (p + lo->off_rwlock, &rwlock, 4);
  memcpy (p + lo->off_owner, &owner, 8);
  memcpy (p + lo->off_tran, &tran, 4);
}

int
main (void)
{
  CS_LAYOUT lo;
  CS_ROW rows[2];
  LIVE_WIN *w;
  CS_FIG f;
  VERDICT v;
  int i, k, held[2] = { 0, 0 }, bad = 0;
  const uint64_t daemon = 0x7f0000001000ULL;

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
  for (i = 0; i < 2; i++)
    {
      snprintf (rows[i].name, sizeof (rows[i].name), i == 0 ? "LOG" : "CS_1");
      rows[i].tran = -1;
      rows[i].has_stats = 1;
    }
  w = live_win_new (lo.count, lo.stride);
  w->no_disk = 1;
  live_win_reset (w, lo.count);
  memset (w->blob, 0, (size_t) lo.stride * lo.count);
  /* 100 samples: LOG written 3, demoted 92 (alone or with another reader),
   * free 5; CS_1 read by two plain readers throughout */
  for (k = 0; k < 100; k++)
    {
      put_cs (w->blob, &lo, 0, k < 3 ? -1 : k < 95 ? 1 + k % 2 : 0,
	      k < 95 ? daemon : 0, -1);
      put_cs (w->blob, &lo, 1, 2, 0, -1);
      live_accumulate (w, &lo);	/* counts the sample */
    }
  for (i = 0; i < w->st.nact; i++)
    {
      if (w->st.act[i].kind == ACT_HELD)
	{
	  held[w->st.act[i].cs] += w->st.act[i].samples;
	}
    }
  if (held[0] != 95 || held[1] != 0)
    {
      printf ("t18 FAIL: held samples LOG %d (want 95: writer 3 + demoted 92), plain readers %d (want 0)\n",
	      held[0], held[1]);
      bad = 1;
    }
  view_capture (&lo, rows, w, 1.0);
  cs_figures (0, 1.0, &f);
  verdict_compute (&f, NULL, &v);
  if (v.holder != 0 || v.thr_pct < 94.5 || v.thr_pct > 95.5)
    {
      printf ("t18 FAIL: verdict holder %d, daemon share %.1f%% (want engine, 95%%; occupied %.1f%%)\n",
	      v.holder, v.thr_pct, f.occ_pct);
      bad = 1;
    }
  /* a demoted row names its owner; a plain reader row names none */
  rows[0].rwlock = 1;
  rows[0].owner = daemon;
  rows[0].tran = 0;
  rows[1].rwlock = 2;
  rows[1].owner = 0;
  if (row_owner (&rows[0]) != daemon || row_tran (&rows[0]) != 0
      || row_owner (&rows[1]) != 0 || row_tran (&rows[1]) != -1)
    {
      printf ("t18 FAIL: owner of a demoted row / of plain readers\n");
      bad = 1;
    }
  live_win_free (w);
  if (!bad)
    {
      printf ("t18 PASS: demoted owner held %d of 100 samples (verdict daemon %.0f%%), plain readers held none\n",
	      held[0], v.thr_pct);
    }
  return bad;
}
