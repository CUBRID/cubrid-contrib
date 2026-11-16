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
 * frame output - one frame in the chosen mode: summary screen, scrolling rows, transaction table or JSON
 */

#include "ctv.h"

const char *live_footer = NULL;

void
live_banner (int hz, double interval)
{
  if (hz <= 0)
    {
      /* --trans alone: one read per window, no sampling */
      printf (M ("live: transaction table every %.1fs; Ctrl-C to stop\n",
		 "라이브: %.1fs 마다 트랜잭션 표; Ctrl-C로 종료\n"), interval);
    }
  else
    {
      printf (M ("live: %dHz sampling, %.1fs display; Ctrl-C to stop\n",
		 "라이브: 샘플링 %dHz, 표시 %.1fs; Ctrl-C로 종료\n"), hz, interval);
    }
  fflush (stdout);
}

void
live_render (const CS_LAYOUT * lo, int screen, int json,
	     double dt, int hz, double interval, int pid, const char *db)
{
  char hhmmss[16];
  int i, shown = 0;

  clock_hms (view.ts, hhmmss, sizeof (hhmmss));
  if (json)
    {
      json_doc (pid, db, lo, 1, 1, 0);	/* one JSON document per window */
      return;
    }
  if (screen)
    {
      screen_summary (lo, dt, hz, interval, pid, db, hhmmss);
      return;
    }
  printf ("\n--- %s  (samples=%d, dt=%.2fs)\n", hhmmss, view.st.samples, dt);
  for (i = 0; i < lo->count; i++)
    {
      /* scroll mode stays terse: rows that moved only */
      if (!row_moved (i))
	{
	  continue;
	}
      if (!shown)
	{
	  printf ("%s%2s  %-24s %5s %-10s %6s %9s %9s %5s %5s  %s%s\n",
		  clr (CLR_HEAD), "ix", "name", "occ%", "occ-bar", "wait%",
		  "enter/s", "wait/s", "waitR", "waitW", "state",
		  clr (CLR_OFF));
	  shown = 1;
	}
      live_print_row (i, dt);
    }
  if (!shown)
    {
      printf (M ("    (idle - no critical section activity)\n",
		 "    (유휴 - 크리티컬 섹션 활동 없음)\n"));
    }
  if (layout_detail)
    {
      /* --detail with -i: vertical records of the rows that moved */
      for (i = 0; i < lo->count; i++)
	{
	  if (row_moved (i) && detail_match (i, view.rows[i].name))
	    {
	      print_detail_row (i, &view.rows[i]);
	    }
	}
    }
  fflush (stdout);
}

/* the transaction table of one window (--trans with -i or a replay) */
void
trans_window_out (int pid, const char *db, const CS_LAYOUT * lo, int json)
{
  if (json)
    {
      json_doc (pid, db, lo, 1, 0, 1);
    }
  else
    {
      char hhmmss[16];

      printf ("\n--- %s  (samples=%d, dt=%.2fs)\n",
	      clock_hms (view.ts, hhmmss, sizeof (hhmmss)), view.st.samples, view.dt);
      (void) print_trans ();
    }
  fflush (stdout);
}

/* replay wait for the current window: its length, but a window stretched
 * by a suspended live run waits one --interval */
double
replay_pace (const VIEW_HDR * h)
{
  return h->live && view.dt > h->interval ? h->interval : view.dt;
}

void
replay_frame_out (const VIEW_HDR * h, int screen, int json, int trans)
{
  if (trans)
    {
      trans_window_out (h->pid, h->db, &h->lo, json);
      return;
    }
  live_render (&h->lo, screen, json, view.dt, h->hz, h->interval, h->pid,
	       h->db);
}
