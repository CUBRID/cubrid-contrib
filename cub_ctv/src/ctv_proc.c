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
 * servers - running cub_server processes from procfs
 */

#include "ctv.h"

/* ------------------------------------------------------- target finding */

/* pid as a server entry (db = first argument); 0 when it is a cub_server */
int
server_by_pid (int pid, SERVER * out)
{
  char path[300], buf[4096];
  int got, i;

  snprintf (path, sizeof (path), "/proc/%d/comm", pid);
  if (pid <= 0 || read_small (path, buf, sizeof (buf)) < 0)
    {
      return -1;
    }
  buf[strcspn (buf, "\n")] = '\0';
  snprintf (path, sizeof (path), "/proc/%d/cmdline", pid);
  if (strcmp (buf, "cub_server") != 0
      || (got = read_small (path, buf, sizeof (buf))) < 0)
    {
      return -1;
    }
  out->pid = pid;
  snprintf (out->db, sizeof (out->db), "?");
  for (i = 0; i + 1 < got; i++)
    {
      if (buf[i] == '\0')
	{
	  snprintf (out->db, sizeof (out->db), "%s", buf + i + 1);
	  text_sanitize (out->db);
	  break;
	}
    }
  return 0;
}

int
list_servers (SERVER * out, int max)
{
  DIR *d = opendir ("/proc");
  struct dirent *e;
  int n = 0;

  if (d == NULL)
    {
      return 0;
    }
  while ((e = readdir (d)) != NULL && n < max)
    {
      n += server_by_pid (atoi (e->d_name), &out[n]) == 0;
    }
  closedir (d);
  return n;
}
