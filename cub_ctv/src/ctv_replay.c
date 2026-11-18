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
 * replay loop - a recorded run, scrolling or interactive
 */

#include "ctv.h"

static void
replay_interactive (const VIEW_HDR * h, int n, double speed)
{
  int i = 0, paused = 0, key, loaded = -1;
  char footer[200];

  if (n == 0)
    {
      die ("record holds no complete frame", "기록에 완성된 프레임이 없습니다",
	   NULL);
    }
  if (speed <= 0)
    {
      speed = 1;
    }
  for (;;)
    {
      /* the loaded frame is redrawn as is; the next one is read on from
       * here; any other is reached from its keyframe */
      if (i != loaded
	  && (i == loaded + 1 ? view_replay_next () : view_replay_goto (i)) != 0)
	{
	  die ("cannot read frame", "프레임 판독 실패", NULL);
	}
      loaded = i;
      snprintf (footer, sizeof (footer),
		M ("REPLAY %d/%d  x%g%s   [space] pause  [<] [>] window  "
		   "[+] [-] speed  %s[q] quit",
		   "재생 %d/%d  x%g%s   [space] 일시정지  [<] [>] 구간  "
		   "[+] [-] 속도  %s[q] 종료"), i + 1, n, speed,
		paused ? M ("  PAUSED", "  일시정지") : "", lang_key ());
      live_footer = footer;
      replay_frame_out (h, 1, 0, 0);
      key = key_read (paused ? -1.0 : replay_pace (h) / speed);
      switch (key)
	{
	case 'q':
	case KEY_EOF:		/* stdin is gone: nothing can resume a pause */
	  live_footer = NULL;	/* footer lives on this stack */
	  return;
	case ' ':
	  paused = !paused;
	  break;
	case KEY_BACK:
	  i = i > 0 ? i - 1 : 0;
	  paused = 1;
	  break;
	case KEY_FWD:
	  i = i < n - 1 ? i + 1 : i;
	  paused = 1;
	  break;
	case '+':
	  speed = speed < 64 ? speed * 2 : speed;
	  break;
	case '-':
	  speed = speed > 1.0 / 16 ? speed / 2 : speed;
	  break;
	case 'l':
	  use_ko = ko_ok && !use_ko;	/* the loop redraws this frame */
	  break;
	case KEY_RESIZE:	/* resized: redraw this frame */
	case KEY_IGNORED:
	  break;
	case 0:		/* no key: the next window */
	  if (!paused)
	    {
	      if (i < n - 1)
		{
		  i++;
		}
	      else
		{
		  paused = 1;	/* end of the record */
		}
	    }
	  break;
	default:
	  break;
	}
    }
}

/* windows of a live record through the live renderer.  On a terminal the
 * replay takes keys: space pause, left/right one window, +/- speed, q quit.
 * On a pipe (or --json/--trans) it streams at the recorded pace (speed 2 =
 * twice as fast, 0 = no waiting). */
int
live_replay (const VIEW_HDR * h, double speed, int json, int trans)
{
  int screen = !json && !trans && isatty (STDOUT_FILENO);

  if (screen && !view_replay_seekable ())
    {
      /* stepping needs to seek: a pipe or FIFO plays through instead */
      warn ("the record is not seekable (pipe): scrolling replay without keys",
	    "기록을 되감을 수 없습니다(파이프): 키 없이 스크롤 재생");
      screen = 0;
    }
  if (screen)
    {
      int n = view_replay_index ();	/* warns before the screen switch */

      if (keys_enter () == 0)
	{
	  if (layout_detail)
	    {
	      warn ("the terminal shows the summary screen; --detail applies to "
		    "the scrolling (pipe) replay", "터미널은 요약 화면을 "
		    "보입니다; --detail 은 스크롤(파이프) 재생에 적용");
	    }
	  screen_enter ();
	  replay_interactive (h, n, speed);
	  return 0;
	}
      screen = 0;		/* stdin not a terminal: scrolling output */
    }
  if (!json)
    {
      live_banner (h->hz, h->interval);
    }
  /* each frame stays for its own length before the next one is shown */
  {
    double pace = -1.0;

    while (view_replay_next () == 0)
      {
	if (speed > 0 && pace >= 0)
	  {
	    sleep_until (mono_s () + pace / speed);
	  }
	replay_frame_out (h, screen, json, trans);
	if (trans && !view.tran_avail)
	  {
	    return 1;		/* as the live run that made it */
	  }
	pace = replay_pace (h);
      }
    if (pace < 0)
      {
	die ("record holds no complete frame", "기록에 완성된 프레임이 없습니다",
	     NULL);		/* as the interactive replay */
      }
  }
  return 0;
}
