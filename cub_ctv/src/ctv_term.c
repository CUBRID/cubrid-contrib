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
 * terminal - size, colors, raw mode, keys, signals, waiting (owns all tty state)
 */

#include "ctv.h"

/* set by SIGWINCH: the next size query re-reads the terminal */
static volatile sig_atomic_t winch_cols = 0, winch_rows = 0, winch_key = 0;

static void
on_winch (int sig)
{
  (void) sig;
  winch_cols = winch_rows = winch_key = 1;
}

/* terminal width for row splitting: on a terminal COLUMNS, else its size;
 * 0 (unlimited) when stdout is not a terminal - a pipe keeps one row per
 * line whatever COLUMNS says */
int
term_columns (void)
{
  static int cols = -1;

  if (cols < 0 || winch_cols)
    {
      const char *env = getenv ("COLUMNS");
      struct winsize ws;

      cols = 0;
      winch_cols = 0;
      if (!isatty (STDOUT_FILENO))
	{
	  ;			/* a pipe: one row per line */
	}
      else if (env != NULL && atoi (env) > 0)
	{
	  cols = atoi (env);
	}
      else if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
	{
	  cols = ws.ws_col;
	}
    }
  return cols;
}

/* terminal height, 0 when unknown (pipe) */
int
term_rows (void)
{
  static int rows = -1;

  if (rows < 0 || winch_rows)
    {
      struct winsize ws;

      rows = 0;
      winch_rows = 0;
      if (isatty (STDOUT_FILENO) && ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
	  && ws.ws_row > 0)
	{
	  rows = ws.ws_row;
	}
    }
  return rows;
}

int color_on = 0;

/* 256-color palette: cyan heads, green writer, red wait, amber owner,
 * purple wait queue, grey muted; "" when colors are off */
const char *
clr (int which)
{
  static const char *tab[] = {
    "\033[0m",			/* CLR_OFF */
    "\033[1;38;5;73m",		/* CLR_HEAD */
    "\033[38;5;150m",		/* CLR_WRITER */
    "\033[1;38;5;203m",		/* CLR_WAIT */
    "\033[38;5;222m",		/* CLR_OWNER */
    "\033[38;5;183m",		/* CLR_WQ */
    "\033[38;5;245m",		/* CLR_MUTED */
    "\033[38;5;150m",		/* CLR_BAR_LOW */
    "\033[38;5;221m",		/* CLR_BAR_MID */
    "\033[38;5;203m",		/* CLR_BAR_HIGH */
    "\033[7;38;5;222m",		/* CLR_PANEL_OWNER: reverse, amber */
    "\033[4;38;5;183m",		/* CLR_PANEL_WAITER: underline, purple */
  };

  return color_on ? tab[which] : "";
}

/* full-screen handling: alternate screen + hidden cursor, restored on exit */
static volatile sig_atomic_t screen_active = 0;

/* keyboard (replay keys, live [r]): raw stdin, restored with the screen */
static struct termios tty_saved;

static volatile sig_atomic_t tty_raw = 0;

void
screen_restore (void)
{
  if (tty_raw)
    {
      tty_raw = 0;
      tcsetattr (STDIN_FILENO, TCSANOW, &tty_saved);
    }
  if (screen_active)
    {
      screen_active = 0;
      (void) !write (STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
    }
}

/* stdin to single keys without echo (the current mode saved); 0 on success */
static int
tty_raw_on (void)
{
  struct termios t;

  if (tcgetattr (STDIN_FILENO, &tty_saved) != 0)
    {
      return -1;
    }
  t = tty_saved;
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  if (tcsetattr (STDIN_FILENO, TCSANOW, &t) != 0)
    {
      return -1;
    }
  tty_raw = 1;
  return 0;
}

int
keys_enter (void)
{
  return isatty (STDIN_FILENO) && tty_raw_on () == 0 ? 0 : -1;
}

/* a live run ends at a window boundary: the record and the screen then
 * hold the same windows */
static volatile sig_atomic_t live_stop = 0;

/* a stop signal (INT/TERM/HUP) arrived: the live run ends at the next
 * window boundary */
int
term_stop_requested (void)
{
  return live_stop != 0;
}

/* one key within 'wait' seconds (< 0: block): a byte, or one of the
 * KEY_* codes above; 0 on timeout or a stop */
int
key_read (double wait)
{
  fd_set fds;
  struct timeval tv;
  struct timespec ts, *tsp = NULL;
  sigset_t block, orig;
  unsigned char c[8];
  ssize_t n;
  int rc;

  FD_ZERO (&fds);
  FD_SET (STDIN_FILENO, &fds);
  if (wait >= 0)
    {
      ts.tv_sec = (time_t) wait;
      ts.tv_nsec = (long) ((wait - (double) ts.tv_sec) * 1e9);
      tsp = &ts;
    }
  /* SIGWINCH and the stop signals are blocked from the checks until
   * pselect waits, so a resize or a stop in between ends the wait */
  sigemptyset (&block);
  sigaddset (&block, SIGWINCH);
  sigaddset (&block, SIGINT);
  sigaddset (&block, SIGTERM);
  sigaddset (&block, SIGHUP);
  sigprocmask (SIG_BLOCK, &block, &orig);
  if (winch_key || live_stop)
    {
      int stop = live_stop;

      winch_key = 0;
      sigprocmask (SIG_SETMASK, &orig, NULL);
      return stop ? 0 : KEY_RESIZE;	/* resized while drawing: redraw before waiting */
    }
  rc = pselect (STDIN_FILENO + 1, &fds, NULL, NULL, tsp, &orig);
  sigprocmask (SIG_SETMASK, &orig, NULL);
  if (rc < 0 && errno == EINTR && winch_key)
    {
      winch_key = 0;
      return KEY_RESIZE;	/* the wait was cut by a resize: redraw */
    }
  if (rc < 0)
    {
      /* a stop ends the wait; any other interruption is no timeout (a
       * playing replay must not move on early) */
      return live_stop ? 0 : KEY_IGNORED;
    }
  if (rc == 0)
    {
      return 0;			/* timeout: the caller's next step */
    }
  /* one byte per call so a burst of keys is not swallowed; an arrow is
   * ESC [ C/D, read in two more non-blocking bytes */
  n = read (STDIN_FILENO, c, 1);
  if (n < 0 && (errno == EINTR || errno == EAGAIN))
    {
      return KEY_IGNORED;	/* no byte after all: not a timeout */
    }
  if (n <= 0)
    {
      return KEY_EOF;
    }
  if (c[0] == 0x1b)
    {
      /* arrow = ESC [ C/D; the rest may arrive a moment later */
      for (n = 1; n < 3; n++)
	{
	  FD_ZERO (&fds);
	  FD_SET (STDIN_FILENO, &fds);
	  tv.tv_sec = 0;
	  tv.tv_usec = 50000;
	  if (select (STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0
	      || read (STDIN_FILENO, c + n, 1) != 1)
	    {
	      return KEY_IGNORED;	/* a lone Esc */
	    }
	}
      return c[1] == '[' ? (c[2] == 'C' ? KEY_FWD : c[2] == 'D' ? KEY_BACK
			    : KEY_IGNORED) : KEY_IGNORED;
    }
  switch (c[0])
    {
    case ',':
    case '<':
    case 'b':
      return KEY_BACK;
    case '.':
    case '>':
    case 'n':
      return KEY_FWD;
    case '=':
      return '+';
    case '_':
      return '-';
    default:
      /* a control byte (NUL, Ctrl-A ..) is no command */
      return c[0] < 0x20 || c[0] == 0x7f ? KEY_IGNORED : c[0];
    }
}

static void
on_signal (int sig)
{
  screen_restore ();
  _exit (128 + sig);
}

static void
on_stop (int sig)
{
  (void) sig;
  live_stop = 1;
}

void
live_stop_on_signals (void)
{
  signal (SIGINT, on_stop);
  signal (SIGTERM, on_stop);
  signal (SIGHUP, on_stop);
}

static int screen_wanted = 0, keys_wanted = 0;	/* restored after fg */

/* Ctrl-Z: give the terminal back, then stop for real */
static void
on_tstp (int sig)
{
  (void) sig;
  screen_wanted = screen_active;
  keys_wanted = tty_raw;
  screen_restore ();
  signal (SIGTSTP, SIG_DFL);
  raise (SIGTSTP);
}

/* fg: take the terminal again; the next frame redraws it */
static void
on_cont (int sig)
{
  (void) sig;
  signal (SIGTSTP, on_tstp);
  if (keys_wanted && !tty_raw)
    {
      (void) tty_raw_on ();
    }
  if (screen_wanted && !screen_active)
    {
      (void) !write (STDOUT_FILENO, "\033[?1049h\033[?25l", 14);
      screen_active = 1;
    }
  keys_wanted = screen_wanted = 0;	/* a later bare SIGCONT changes nothing */
  winch_key = winch_cols = winch_rows = 1;	/* redraw as after a resize */
}

/* the terminal (raw keys, alternate screen) is restored on exit, on the
 * usual signals, and across Ctrl-Z / fg */
void
restore_on_exit (void)
{
  static int done = 0;

  if (!done)
    {
      done = 1;
      atexit (screen_restore);
      signal (SIGINT, on_signal);
      signal (SIGTERM, on_signal);
      signal (SIGHUP, on_signal);
      signal (SIGPIPE, on_signal);
      signal (SIGTSTP, on_tstp);
      signal (SIGCONT, on_cont);
    }
}

void
screen_enter (void)
{
  (void) !write (STDOUT_FILENO, "\033[?1049h\033[?25l", 14);
  screen_active = 1;
  restore_on_exit ();
  signal (SIGWINCH, on_winch);
}

double
mono_s (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

void
sleep_until (double t)
{
  double rest = t - mono_s ();
  sigset_t block, orig;

  /* the stop signals are blocked from the live_stop check until pselect
   * waits, so a stop in between ends the wait at once */
  sigemptyset (&block);
  sigaddset (&block, SIGINT);
  sigaddset (&block, SIGTERM);
  sigaddset (&block, SIGHUP);
  sigprocmask (SIG_BLOCK, &block, &orig);
  if (rest > 0 && !live_stop)
    {
      struct timespec ts;

      ts.tv_sec = (time_t) rest;
      ts.tv_nsec = (long) ((rest - ts.tv_sec) * 1e9);
      (void) pselect (0, NULL, NULL, NULL, &ts, &orig);
    }
  sigprocmask (SIG_SETMASK, &orig, NULL);
}

/* "HH:MM:SS" of a frame time */
const char *
clock_hms (long ts, char *buf, size_t n)
{
  time_t wall = (time_t) ts;
  struct tm tmv;

  localtime_r (&wall, &tmv);
  strftime (buf, n, "%H:%M:%S", &tmv);
  return buf;
}
