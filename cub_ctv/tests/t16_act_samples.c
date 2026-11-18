/*
 * t16: one activity is counted at most once per sample, so its samples never
 * exceed the window's and a live record replays.  Waiters whose tran is not
 * known (-1, e.g. a server where I-15 is degraded) used to be merged into
 * one activity: two of them waiting together counted twice per sample.
 * The sampler is included for its activity table (act_add).
 */
#include "../src/ctv_sampler.c"

int
main (int argc, char **argv)
{
  CS_LAYOUT lo;
  CS_ROW rows[2];
  VIEW_HDR h;
  LIVE_WIN *w;
  int i, k, bad = 0;

  if (argc != 3)
    {
      return 2;
    }
  memset (&lo, 0, sizeof (lo));
  lo.stride = 160;
  lo.count = 2;
  lo.off_stats = 152;
  memset (rows, 0, sizeof (rows));
  for (i = 0; i < 2; i++)
    {
      snprintf (rows[i].name, sizeof (rows[i].name), "CS_%d", i);
      rows[i].rwlock = -1;
      rows[i].tran = -1;
      rows[i].has_stats = 1;
    }
  if (strcmp (argv[1], "record") == 0)
    {
      memset (&h, 0, sizeof (h));
      h.lo = lo;
      snprintf (h.db, sizeof (h.db), "t16");
      if (view_record_open (argv[2], &h, 0) != 0)
	{
	  return 1;
	}
      w = live_win_new (lo.count, lo.stride);
      w->no_disk = 1;
      live_win_reset (w, lo.count);
      /* 100 samples: tran -1 waiters on threads 10 and 11 in every one,
       * engine threads 20 and 21 (tran 0) and clients tran 5 / 6 in half */
      for (k = 0; k < 100; k++)
	{
	  act_add (w, 0, ACT_WAIT, -1, 10, -1);
	  act_add (w, 0, ACT_WAIT, -1, 11, -1);
	  act_add (w, 0, ACT_HELD, -1, 12, -1);
	  if (k % 2 == 0)
	    {
	      act_add (w, 1, ACT_WAIT, 0, 20, -1);
	      act_add (w, 1, ACT_WAIT, 0, 21, -1);
	      act_add (w, 1, ACT_WAIT, 5, 30, 4005);
	      act_add (w, 1, ACT_WAIT, 6, 31, 4006);
	    }
	  w->st.samples++;
	}
      for (i = 0; i < w->st.nact; i++)
	{
	  if (w->st.act[i].samples > w->st.samples)
	    {
	      printf ("t16 FAIL: activity cs %d tran %d thr %d: %d samples of %d\n",
		      w->st.act[i].cs, w->st.act[i].tran, w->st.act[i].thr,
		      w->st.act[i].samples, w->st.samples);
	      bad = 1;
	    }
	}
      if (w->st.nact != 7)
	{
	  printf ("t16 FAIL: %d activities, expected 7 (one per thread or client)\n", w->st.nact);
	  bad = 1;
	}
      view_capture (&lo, rows, w, 1.0);
      view_record_frame ();
      live_win_free (w);
      return view_record_close () != 0 || bad;
    }
  view_replay_open (argv[2], &h);
  return view_replay_next () != 0;
}
