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
 * detail dump - everything read at one moment as one JSON document (not replayable)
 */

#include "ctv.h"
#include <sys/utsname.h>

/* ------------------------------------------------------- detail dump */

/* everything cub_ctv can learn about the target at one moment, as one JSON
 * document.  Not a replay file: --replay refuses it (see DETAIL_MAGIC). */


void
detail_dump_write (const char *path, const VIEW_HDR * h, uint64_t bias,
		   uint64_t sym_cs, uint64_t sym_pool, uint64_t sym_loggl,
		   uint64_t sym_thr)
{
  FILE *f = private_create (path, 0);
  struct utsname u;
  char exe[4096], link[64];
  ssize_t n;
  unsigned i;
  int first = 1, nthr_now, *tids = NULL;

  if (f == NULL)
    {
      die ("cannot create detail dump %s", "디테일 덤프 %s 생성 실패", path);
    }
  snprintf (link, sizeof (link), "/proc/%d/exe", h->pid);
  n = readlink (link, exe, sizeof (exe) - 1);
  exe[n > 0 ? n : 0] = '\0';
  if (uname (&u) != 0)
    {
      memset (&u, 0, sizeof (u));
    }

  /* the format key stays on the first line: --replay looks for it there */
  fprintf (f, "{\"format\":\"%s\",\"version\":2,\"replayable\":false,\n",
	   DETAIL_MAGIC);
  fprintf (f, "\"note\":\"everything cub_ctv read at this moment; "
	   "not a replay file - use --record for replay\",\n");
  fprintf (f, "\"ts\":%ld,\"host\":", view.ts);
  json_fstr (f, u.nodename);
  fprintf (f, ",\"kernel\":");
  json_fstr (f, u.release);
  fprintf (f, ",\n\"target\":{\"pid\":%d,\"db\":", h->pid);
  json_fstr (f, h->db);
  fprintf (f, ",\"exe\":");
  json_fstr (f, exe);
  fprintf (f, ",\"lib\":");
  json_fstr (f, h->lib);
  nthr_now = os_task_list (h->pid, &tids);
  free (tids);
  fprintf (f, ",\"load_bias\":\"0x%llx\",\"threads_now\":%d,"
	   "\"symbols\":{\"csectgl_Critical_sections\":\"0x%llx\","
	   "\"sync_Stats\":\"0x%llx\",\"log_Gl\":\"0x%llx\","
	   "\"cubthread::Manager\":\"0x%llx\"}},\n",
	   (unsigned long long) bias, nthr_now,
	   (unsigned long long) sym_cs, (unsigned long long) sym_pool,
	   (unsigned long long) sym_loggl, (unsigned long long) sym_thr);
  fprintf (f, "\"layout\":{\"stride\":%d,\"count\":%d,\"off_stats\":%d,"
	   "\"off_rwlock\":%d,\"off_tran\":%d,\"off_owner\":%d,"
	   "\"off_wq\":%d,\"off_pq\":%d},\n", h->lo.stride, h->lo.count,
	   h->lo.off_stats, h->lo.off_rwlock, h->lo.off_tran, h->lo.off_owner,
	   h->lo.off_wq, h->lo.off_pq);
  fprintf (f, "\"degraded\":%u,\"degraded_features\":[", degraded);
  for (i = 0; deg_names[i].bit != 0; i++)
    {
      if (degraded & deg_names[i].bit)
	{
	  fprintf (f, "%s\"%s (%s)\"", first ? "" : ",", deg_names[i].label,
		   deg_names[i].id);
	  first = 0;
	}
    }
  fprintf (f, "],\"tran_refused\":");
  json_fstr (f, h->tran_refused);
  fprintf (f, ",\n");
  json_cs_rows (f, &h->lo, view.rows, NULL, NULL, 0, NULL, 0.0, 1);
  fprintf (f, ",\n");
  tran_dump (f);
  fprintf (f, ",\n");
  thr_dump (f);
  fprintf (f, ",\n");
  if (sym_pool == 0 || pool_dump (f, bias + sym_pool) != 0)
    {
      fprintf (f, "\"pool\":null");
    }
  fprintf (f, "\n}\n");
  if (ferror (f) | (fclose (f) != 0))
    {
      die ("cannot write detail dump %s", "디테일 덤프 %s 쓰기 실패", path);
    }
}
