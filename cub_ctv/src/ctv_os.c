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
 * OS view - procfs: tasks, thread names and states, clock ticks, the log device
 */

#include "ctv.h"
#include <sys/sysmacros.h>

/* ------------------------------------------------ OS view of the target */

int target_pid = 0;

/* /proc/<pid>/task: everything here is procfs text, no target memory read */

int
cmp_int (const void *a, const void *b)
{
  int x = *(const int *) a, y = *(const int *) b;

  return x < y ? -1 : x > y;
}

/* sorted OS thread ids of the target; returns the count, -1 if unreadable */
int
os_task_list (int pid, int **out)
{
  char path[64];
  DIR *d;
  struct dirent *e;
  int n = 0, cap = 0, *v = NULL;

  snprintf (path, sizeof (path), "/proc/%d/task", pid);
  d = opendir (path);
  if (d == NULL)
    {
      return -1;
    }
  while ((e = readdir (d)) != NULL)
    {
      if (e->d_name[0] == '.')
	{
	  continue;
	}
      if (n == cap)
	{
	  int *m = realloc (v, (size_t) (cap = cap ? cap * 2 : 256) * sizeof (int));

	  if (m == NULL)
	    {
	      free (v);
	      closedir (d);
	      return -1;
	    }
	  v = m;
	}
      v[n++] = atoi (e->d_name);
    }
  closedir (d);
  if (n == 0)
    {
      free (v);
      *out = NULL;
      return 0;
    }
  qsort (v, (size_t) n, sizeof (int), cmp_int);
  *out = v;
  return n;
}

int
os_task_known (const int *tasks, int n, int tid)
{
  return n > 0 && bsearch (&tid, tasks, (size_t) n, sizeof (int), cmp_int) != NULL;
}

/* a small procfs file in one read, NUL-terminated; its length, or -1 */
int
read_small (const char *path, char *buf, size_t n)
{
  int fd = open (path, O_RDONLY);
  ssize_t got;

  if (fd < 0)
    {
      return -1;
    }
  got = read (fd, buf, n - 1);
  close (fd);
  if (got <= 0)
    {
      return -1;
    }
  buf[got] = '\0';
  return (int) got;
}

/* thread name the engine gave the OS thread ("pgbuf-page-flus", ...) */
#define COMM_CACHE 256
#define COMM_TTL   5

int
os_task_comm (int pid, int tid, char *out, size_t n)
{
  /* names change rarely: a tid's name is reused for COMM_TTL seconds */
  static struct
  {
    int tid;
    time_t at;
    char comm[OS_COMM_LEN];
  } cache[COMM_CACHE];
  char path[80], buf[64];
  size_t len;
  time_t now = (time_t) mono_s ();
  unsigned h = (unsigned) tid % COMM_CACHE;

  if (cache[h].tid == tid && now - cache[h].at < COMM_TTL)
    {
      snprintf (out, n, "%s", cache[h].comm);
      return 0;
    }
  snprintf (path, sizeof (path), "/proc/%d/task/%d/comm", pid, tid);
  if (read_small (path, buf, sizeof (buf)) < 0)
    {
      out[0] = '\0';
      return -1;
    }
  len = strcspn (buf, "\n");
  if (len >= n)
    {
      len = n - 1;
    }
  memcpy (out, buf, len);
  out[len] = '\0';
  text_sanitize (out);		/* a thread may name itself anything */
  cache[h].tid = tid;
  cache[h].at = now;
  snprintf (cache[h].comm, sizeof (cache[h].comm), "%s", out);
  return 0;
}

/* scheduler state letter (R running, D disk sleep, S sleeping, ...) and
 * user+system CPU ticks of one thread */
int
os_task_state (int pid, int tid, char *state, long *ticks)
{
  char path[80], buf[512], *p;
  long ut, st;

  snprintf (path, sizeof (path), "/proc/%d/task/%d/stat", pid, tid);
  if (read_small (path, buf, sizeof (buf)) < 0)
    {
      return -1;
    }
  /* fields follow the ")" that closes the command name */
  p = strrchr (buf, ')');
  if (p == NULL || sscanf (p + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld",
			   state, &ut, &st) != 3)
    {
      return -1;
    }
  *ticks = ut + st;
  return 0;
}

long
os_clock_ticks (void)
{
  long hz = sysconf (_SC_CLK_TCK);

  return hz > 0 ? hz : 100;
}

/* block device behind the active log (an fd named *_lgat); 0 = not a block
 * device (overlay / network filesystem) */
int
os_log_device (int pid, unsigned *major_out, unsigned *minor_out, char *name,
	       size_t n)
{
  char path[64], link[4096];
  DIR *d;
  struct dirent *e;
  int found = 0;

  snprintf (path, sizeof (path), "/proc/%d/fd", pid);
  d = opendir (path);
  if (d == NULL)
    {
      return 0;
    }
  while (!found && (e = readdir (d)) != NULL)
    {
      char fdp[384];
      ssize_t got;
      size_t len;
      struct stat st;

      if (e->d_name[0] == '.')
	{
	  continue;
	}
      snprintf (fdp, sizeof (fdp), "%s/%s", path, e->d_name);
      got = readlink (fdp, link, sizeof (link) - 1);
      if (got <= 0)
	{
	  continue;
	}
      link[got] = '\0';
      len = strlen (link);
      if (len < 5 || strcmp (link + len - 5, "_lgat") != 0)
	{
	  continue;
	}
      if (stat (fdp, &st) == 0 && major (st.st_dev) != 0)
	{
	  *major_out = major (st.st_dev);
	  *minor_out = minor (st.st_dev);
	  found = 1;
	}
      break;
    }
  closedir (d);
  if (!found)
    {
      return 0;
    }
  /* device name: the diskstats line of that major:minor */
  {
    uint64_t a, b, c;

    name[0] = '\0';
    (void) os_diskstats (*major_out, *minor_out, &a, &b, &c, name, n);
  }
  return name[0] != '\0';
}

/* writes completed, ms spent writing, ms the device was busy */
int
os_diskstats (unsigned major_n, unsigned minor_n, uint64_t * wr_ios,
	      uint64_t * wr_ms, uint64_t * io_ms, char *name, size_t namen)
{
  FILE *f = fopen ("/proc/diskstats", "r");
  char line[256];
  int rc = -1;

  while (f != NULL && fgets (line, sizeof (line), f) != NULL)
    {
      unsigned mj, mn;
      char dev[64];
      unsigned long long v[11];

      if (sscanf (line, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		  &mj, &mn, dev, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
		  &v[7], &v[8], &v[9], &v[10]) == 14 && mj == major_n
	  && mn == minor_n)
	{
	  *wr_ios = v[4];
	  *wr_ms = v[7];
	  *io_ms = v[9];
	  if (name != NULL)
	    {
	      snprintf (name, namen, "%s", dev);
	    }
	  rc = 0;
	  break;
	}
    }
  if (f != NULL)
    {
      fclose (f);
    }
  return rc;
}
