/*
 * t13: the verdict figures of every frame of a record (VERDICT, before any
 * text), one line per frame, for a byte comparison with a golden file.
 */
#include "ctv.h"

int
main (int argc, char **argv)
{
  VIEW_HDR h;
  CS_FIG fig[MAX_CS];
  VERDICT v;
  int f = 0;

  if (argc != 2)
    {
      return 2;
    }
  view_replay_open (argv[1], &h);
  while (view_replay_next () == 0)
    {
      f++;
      if (!h.live || view.st.samples < 1)
	{
	  continue;
	}
      cs_figures_ranked (h.lo.count, view.dt, fig);
      history_push (view.seq, fig[0].severity >= 2 ? fig[0].ix : -1);
      if (fig[0].severity < 2)
	{
	  printf ("%d idle\n", f);
	  continue;
	}
      verdict_compute (&fig[0], &fig[1], &v);
      printf ("%d top=%s sev=%d lost=%.3f second=%s persist=%d/%d holder=%d one_thr=%d"
	      " held=%.1f thr=%.1f waiters=%d waited=%.1f listed=%d more=%d bound=%d"
	      " dsk=%.0f run=%.0f cpu=%.0f readers=%d writers=%.1f os=%d\n", f, view.rows[v.top.ix].name,
	      v.top.severity, v.top.lost_s, v.has_second ? view.rows[v.second.ix].name : "-",
	      v.same, v.span, v.holder, v.one_thr, v.held_pct, v.thr_pct, v.nwaiters,
	      v.waited_pct, v.nlisted, v.more_waiters, v.bound, v.dsk_pct, v.run_pct,
	      v.cpu_pct, v.readers_most, v.writers_pct, v.os != NULL ? v.os->slot : -1);
    }
  return 0;
}
