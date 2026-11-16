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
 * live loop - sampling, closing windows, keys and [r] recording
 */

#include "ctv.h"

void
die_target_gone (void)
{
  die ("target read failed (server gone?)", "대상 판독 실패(서버 종료?)",
       NULL);
}

/* [r] on the live screen: start a record "<pid>_<db>_<YYYYmmdd-HHMMSS>.ctv"
 * in the current directory, or stop the one running */
static int
live_record_toggle (const VIEW_HDR * h, char *name, size_t n)
{
  /* 0 on success; -1 when the file could not be created or its last
   * write / close failed (name cleared either way) */
  if (view_record_active ())
    {
      int empty = view_record_frames () == 0;
      int rc = view_record_close ();

      if (empty)
	{
	  (void) unlink (name);	/* stopped before a window: nothing to keep */
	}
      name[0] = '\0';
      return rc;
    }
  {
    time_t t = time (NULL);
    struct tm tmv;
    char stamp[32], safe[128];
    int k;
    VIEW_HDR hh;

    localtime_r (&t, &tmv);
    strftime (stamp, sizeof (stamp), "%Y%m%d-%H%M%S", &tmv);
    /* the db name comes from the target's argv: one plain path component */
    {
      size_t j;

      snprintf (safe, sizeof (safe), "%s", h->db);
      for (j = 0; safe[j] != '\0'; j++)
	{
	  unsigned char ch = (unsigned char) safe[j];

	  if (!(isalnum (ch) || ch == '_' || ch == '-' || (ch == '.' && j > 0)))
	    {
	      safe[j] = '_';
	    }
	}
    }
    snprintf (name, n, "%d_%s_%s.ctv", h->pid, safe, stamp);
    /* a name taken (same second) gets a suffix -2 .. -9 */
    hh = *h;
    hh.degraded = degraded;	/* the bits as of now, not of the start */
    for (k = 2; view_record_open (name, &hh, 1) != 0; k++)
      {
	if (errno != EEXIST || k > 9)
	  {
	    name[0] = '\0';
	    return -1;
	  }
	snprintf (name, n, "%d_%s_%s-%d.ctv", h->pid, safe, stamp, k);
      }
  }
  return 0;
}

/* live footer: record state, keys; rec_fail adds a one-time failure note */
static void
live_footer_text (char *footer, size_t n, const char *record,
		  const char *recname, int rec_fail)
{
  if (record != NULL)
    {
      char f[PATH_MAX];

      /* a --record file is the user's: [r] does not touch it */
      snprintf (f, sizeof (f), "%s", record);
      text_sanitize (f);
      snprintf (footer, n, M ("REC %s (--record)   [l] KO/EN  [q] quit",
			      "녹화 중 %s (--record)   [l] 한/영  [q] 종료"), f);
    }
  else if (view_record_active ())
    {
      snprintf (footer, n,
		M ("REC %s   [r] stop recording  [l] KO/EN  [q] quit",
		   "녹화 중 %s   [r] 녹화 중지  [l] 한/영  [q] 종료"), recname);
    }
  else
    {
      snprintf (footer, n, "%s%s",
		M ("[r] record to <pid>_<db>_<time>.ctv  [l] KO/EN  [q] quit",
		   "[r] 녹화 (<pid>_<db>_<시각>.ctv)  [l] 한/영  [q] 종료"),
		rec_fail ? M ("   (record file could not be written)",
			      "   (기록 파일 쓰기 실패)") : "");
    }
}

/* one live run: options, terminal mode, the sampler window, [r] state */
typedef struct
{
  const VIEW_HDR *h;
  const CS_LAYOUT *lo;
  uint64_t cs_base;
  int json, trans, screen, keys, sampled, have_frame, rec_fail, said_r;
  const char *record;		/* --record file, or NULL */
  char recname[256];		/* the [r] record, "" when none */
  char footer[320];
  LIVE_WIN *w;
  double period, interval, t_prev;
} LIVE_RUN;

/* terminal mode, banner, the rows the first window starts from */
static void
run_start (LIVE_RUN * r)
{
  if (r->screen)
    {
      r->keys = keys_enter () == 0;
      if (r->keys)
	{
	  if (layout_detail)
	    {
	      warn ("the terminal shows the summary screen; --detail applies to "
		    "the scrolling (pipe) live output", "터미널은 요약 화면을 "
		    "보입니다; --detail 은 스크롤(파이프) 라이브 출력에 적용");
	    }
	  screen_enter ();
	}
      else
	{
	  r->screen = 0;	/* stdin not a terminal: scrolling output */
	  live_banner (r->sampled ? r->h->hz : 0, r->interval);
	}
    }
  else if (!r->json)
    {
      live_banner (r->sampled ? r->h->hz : 0, r->interval);
    }
  if (r->sampled && cs_read_rows (r->cs_base, r->lo, r->w->prev) != 0)
    {
      die_target_gone ();	/* the table alone needs no CS rows */
    }
  r->t_prev = mono_s ();
  /* the transaction table alone (no record) samples nothing: one read per
   * window; on a terminal it still takes the keys (no full screen) */
  if (!r->sampled && !r->json && isatty (STDOUT_FILENO) && keys_enter () == 0)
    {
      restore_on_exit ();
      r->keys = 1;
    }
  live_stop_on_signals ();
}

/* one sample of the CS array into the window */
static void
run_sample (LIVE_RUN * r)
{
  if (mem_read (r->cs_base, r->w->blob, (size_t) r->lo->stride * r->lo->count) != 0)
    {
      die_target_gone ();
    }
  live_accumulate (r->w, r->lo);
  thr_cache_refresh ();		/* owner lookups follow thread changes: once a second */
}

/* the window ends now: capture, record, show; 1 when the run must stop
 * (--trans and no table) */
static int
run_close_window (LIVE_RUN * r, double now)
{
  if (!r->sampled)
    {
      /* the table alone: procfs says whether the server is still there */
      char proc[32];
      struct stat st;

      snprintf (proc, sizeof (proc), "/proc/%d", r->h->pid);
      if (stat (proc, &st) != 0)
	{
	  die_target_gone ();
	}
      view_capture_trans (now - r->t_prev);
    }
  else
    {
      (void) cs_rows_from_blob (r->w->blob, r->lo, r->w->cur);	/* this sample */
      view_capture (r->lo, r->w->cur, r->w, now - r->t_prev);
    }
  view_record_frame ();
  if (view_record_ok () != 0)
    {
      if (r->record != NULL)
	{
	  die ("cannot write record %s", "기록 파일 %s 쓰기 실패", r->record);
	}
      (void) view_record_close ();	/* [r] record: stop, say once */
      r->recname[0] = '\0';
      r->rec_fail = 1;
    }
  if (r->keys)
    {
      live_footer_text (r->footer, sizeof (r->footer), r->record, r->recname,
			r->rec_fail);
      live_footer = r->footer;
      r->rec_fail = 0;
    }
  r->have_frame = 1;
  if (r->trans)
    {
      trans_window_out (r->h->pid, r->h->db, r->lo, r->json);
      if (!view.tran_avail)
	{
	  return 1;		/* no table to show: like a --trans snapshot */
	}
    }
  else
    {
      live_render (r->lo, r->screen, r->json, now - r->t_prev, r->h->hz,
		   r->interval, r->h->pid, r->h->db);
    }
  memcpy (r->w->prev, r->w->cur, (size_t) r->lo->count * sizeof (CS_ROW));
  live_win_reset (r->w, r->lo->count);
  r->t_prev = now;
  return 0;
}

/* the keys of one tick, waiting at most until tick_end (typing never raises
 * the sampling rate); 1 on [q] */
static int
run_keys (LIVE_RUN * r, double tick_end)
{
  int polled;

  for (polled = 0;; polled = 1)
    {
      double rest = tick_end - mono_s ();
      int key;

      /* an overrun tick (high --hz) still polls once, without waiting */
      if ((rest <= 0 && polled) || (key = key_read (rest > 0 ? rest : 0)) == 0)
	{
	  return 0;
	}
      if (key == KEY_EOF)
	{
	  r->keys = 0;		/* stdin is gone: keep sampling, stop reading */
	  return 0;
	}
      if (key == 'r' && r->record == NULL && r->sampled)
	{
	  r->rec_fail = live_record_toggle (r->h, r->recname, sizeof (r->recname)) != 0;
	  if (r->have_frame && r->screen)
	    {
	      /* the [REC] mark and footer change now, not next window */
	      live_footer_text (r->footer, sizeof (r->footer), r->record,
				r->recname, r->rec_fail);
	      replay_frame_out (r->h, r->screen, r->json, r->trans);
	    }
	}
      else if (key == 'r' && !r->sampled && !r->said_r)
	{
	  r->said_r = 1;
	  warn ("[r] records the sampled live view: run -i without --trans",
		"[r] 은 샘플링 라이브 화면을 기록합니다: --trans 없이 -i 로 실행");
	}
      else if (key == 'l')
	{
	  /* language toggle: the full screen redraws the last frame;
	   * scrolling output takes it from the next window */
	  use_ko = !use_ko;
	  live_footer_text (r->footer, sizeof (r->footer), r->record, r->recname, 0);
	  if (r->have_frame && r->screen)
	    {
	      replay_frame_out (r->h, r->screen, r->json, r->trans);
	    }
	}
      else if (key == KEY_RESIZE && r->have_frame && r->screen)
	{
	  replay_frame_out (r->h, r->screen, r->json, r->trans);
	}
      else if (key == 'q')
	{
	  return 1;		/* same as Ctrl-C: completed windows stay recorded */
	}
    }
}

/* close an [r] record ([r] then quit before a window: nothing to keep) */
static void
run_finish (LIVE_RUN * r)
{
  if (view_record_active ())
    {
      int empty = r->record == NULL && view_record_frames () == 0;

      if (view_record_close () != 0 && r->record != NULL)
	{
	  die ("cannot write record %s", "기록 파일 %s 쓰기 실패", r->record);
	}
      if (empty)
	{
	  (void) unlink (r->recname);
	}
    }
  live_footer = NULL;		/* the footer lives in the run */
  live_win_free (r->w);
}

int
live_mode (uint64_t cs_base, const VIEW_HDR * h, int json, int trans,
	   const char *record, int table_only)
{
  LIVE_RUN r;
  int rc = 0;

  memset (&r, 0, sizeof (r));
  r.h = h;
  r.lo = &h->lo;
  r.cs_base = cs_base;
  r.json = json;
  r.trans = trans;
  r.record = record;
  r.sampled = !table_only;	/* the table alone needs no sampling */
  r.screen = !json && !trans && isatty (STDOUT_FILENO);
  r.period = 1.0 / h->hz;
  r.interval = h->interval;
  r.w = live_win_new (r.lo->count, r.lo->stride);
  r.w->cs_base = cs_base;	/* the sampler checks queue heads against it */
  r.w->no_disk = !r.sampled;
  live_win_reset (r.w, r.lo->count);
  run_start (&r);
  if (trans)
    {
      /* a table that cannot be read fails now, not after one --interval */
      view_capture_trans (0.0);
      if (!view.tran_avail)
	{
	  trans_window_out (h->pid, h->db, r.lo, json);
	  rc = 1;
	}
    }
  while (rc == 0 && !term_stop_requested ())
    {
      double tick_end = r.sampled ? mono_s () + r.period : r.t_prev + r.interval, now;

      if (r.sampled)
	{
	  run_sample (&r);
	}
      now = mono_s ();
      if (now - r.t_prev >= r.interval && run_close_window (&r, now) != 0)
	{
	  rc = 1;
	  break;
	}
      if (r.keys)
	{
	  if (run_keys (&r, tick_end))
	    {
	      break;
	    }
	}
      else
	{
	  sleep_until (tick_end);
	}
    }
  run_finish (&r);
  return rc;
}
