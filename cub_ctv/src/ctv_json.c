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
 * JSON output - one JSON document per frame or window
 */

#include "ctv.h"

void
json_fstr (FILE * f, const char *s)
{
  fputc ('"', f);
  while (*s != '\0')
    {
      unsigned char c = (unsigned char) *s;
      int len = utf8_valid_len (s);

      if (c == '"' || c == '\\')
	{
	  fputc ('\\', f);
	  fputc (c, f);
	  s++;
	  continue;
	}
      if (c < 0x20)
	{
	  fprintf (f, "\\u%04x", c);
	  s++;
	  continue;
	}
      if (len == 0)
	{
	  fputs ("\\ufffd", f);	/* not UTF-8 (e.g. an EUC-KR path) */
	  s++;
	  continue;
	}
      fwrite (s, 1, (size_t) len, f);
      s += len;
    }
  fputc ('"', f);
}

/* the client identity fields of a JSON object (",\"user\":..."), with
 * client_info first when with_info */
void
json_ident (FILE * f, const TRAN_IDENT * id, int with_info)
{
  if (with_info)
    {
      fprintf (f, ",\"info\":");
      json_fstr (f, id->info);
    }
  fprintf (f, ",\"user\":");
  json_fstr (f, id->user);
  fprintf (f, ",\"program\":");
  json_fstr (f, id->program);
  fprintf (f, ",\"login\":");
  json_fstr (f, id->login);
  fprintf (f, ",\"host\":");
  json_fstr (f, id->host);
  fprintf (f, ",\"client_ip\":");
  json_fstr (f, id->ip);
  fprintf (f, ",\"cpid\":%d", id->cpid);
}

/* CS rows as JSON; raw adds the fields the detail dump keeps (owner thread
 * id, wait-queue head, stats validity) */
void
json_cs_rows (FILE * f, const CS_LAYOUT * lo, const CS_ROW * rows,
	      const int *occ, const int *wai, int samples, const CS_ROW * prev,
	      double dt, int raw)
{
  int i;

  fprintf (f, "\"cs\":[");
  for (i = 0; i < lo->count; i++)
    {
      const CS_ROW *r = &rows[i];
      int widx[THR_WAITERS_MAX], nw, k;
      TRAN_IDENT id;

      fprintf (f, "%s{\"ix\":%d,\"name\":", i ? "," : "", i);
      json_fstr (f, r->name);
      fprintf (f, ",\"rwlock\":%d,\"waitR\":%u,\"waitW\":%u,\"tran\":%d",
	       r->rwlock, r->wr, r->ww, raw ? r->tran : row_tran (r));
      {
	int slot = raw ? thr_index_of_tid_mem (r->owner)
	  : view_slot_of_tid (row_owner (r));

	fprintf (f, ",\"thr\":%d", slot >= 0 ? THR_INDEX (slot) : -1);
	if (!raw && owner_is_system (row_tran (r), slot))
	  {
	    const OS_THR *os = slot >= 0 ? view_os_of (slot) : NULL;

	    fprintf (f, ",\"owner\":{\"system\":true,\"comm\":");
	    json_fstr (f, os != NULL ? os->comm : "");
	    fprintf (f, "}");
	  }
      }
      if (raw)
	{
	  fprintf (f, ",\"owner_tid\":\"0x%llx\",\"wq_head\":\"0x%llx\","
		   "\"has_stats\":%d", (unsigned long long) r->owner,
		   (unsigned long long) r->wq, r->has_stats);
	}
      if (row_tran (r) >= 1 && view_identity (row_tran (r), &id, NULL) == 0)
	{
	  fprintf (f, ",\"owner\":{\"tran\":%d", row_tran (r));
	  json_ident (f, &id, 0);
	  fprintf (f, "}");
	}
      {
	/* the chain as the rows show it: an emptied queue gives the one seen
	 * in the window ("wq_seen"); the raw dump keeps the current queue */
	int wt[THR_WAITERS_MAX];
	int seen_cs = !raw && r->wq == 0 && view.st.seen_wq[i] != 0 ? i : -1;

	nw = chain_of (r->wq, seen_cs, widx, wt);
	fprintf (f, ",\"wq\":[");
	for (k = 0; k < nw; k++)
	  {
	    fprintf (f, "%s{\"thr\":%d,\"tran\":%d}", k ? "," : "",
		     THR_INDEX (widx[k]), wt[k]);
	  }
	fprintf (f, "]%s", seen_cs >= 0 && nw > 0 ? ",\"wq_seen\":true" : "");
      }
      if (r->has_stats)
	{
	  fprintf (f, ",\"nenter\":%u,\"nwait\":%u,\"nreenter\":%u,"
		   "\"total_ms\":%.3f,\"max_ms\":%.3f", r->nenter, r->nwait,
		   r->nreenter, r->total_ms, r->max_ms);
	}
      if (samples > 0)
	{
	  CS_RATES rt;

	  cs_rates (r, &prev[i], occ[i], wai[i], samples, dt, &rt);
	  fprintf (f, ",\"occ_pct\":%.2f,\"wait_pct\":%.2f,\"enter_s\":%.2f,"
		   "\"wait_s\":%.2f", rt.occ_pct, rt.wait_pct, rt.enter_s,
		   rt.wait_s);
	}
      fprintf (f, "}");
    }
  fprintf (f, "]");
  json_os (f);
}

/* wait-reason histogram and log-device figures of the frame */
void
json_os (FILE * f)
{
  int k, first = 1;

  fprintf (f, ",\"thread_wait\":");
  if (!view.wait_avail)
    {
      fprintf (f, "null");
    }
  else
    {
      fprintf (f, "{\"live\":%d,\"counts\":{", view.wait_live);
      for (k = 0; k < THR_RESUME_MAX; k++)
	{
	  if (view.wait_cnt[k] > 0)
	    {
	      fprintf (f, "%s\"%d\":%d", first ? "" : ",", k, view.wait_cnt[k]);
	      first = 0;
	    }
	}
      fprintf (f, "}}");
    }
  fprintf (f, ",\"log_disk\":");
  if (!view.disk_avail)
    {
      fprintf (f, "null");
    }
  else
    {
      fprintf (f, "{\"dev\":");
      json_fstr (f, view.disk_dev);
      fprintf (f, ",\"await_ms\":%.3f,\"util_pct\":%.1f}", view.disk_await_ms,
	       view.disk_util_pct);
    }
}

void
json_trans (void)
{
  int i, first = 1;

  printf ("\"trans_available\":%s,\"identities_locked\":%s,\"trans\":[",
	  view.tran_avail ? "true" : "false", view.cid_locked ? "true" : "false");
  for (i = 0; i < view.nident; i++)
    {
      const VIEW_IDENT *v = &view.ident[i];

      printf ("%s{\"idx\":%d,\"trid\":", first ? "" : ",", v->idx);
      if (view.has_trid)
	{
	  printf ("%d", v->trid);
	}
      else
	{
	  printf ("null");	/* -1 would read as a released slot */
	}
      json_ident (stdout, &v->id, 0);
      printf ("}");
      first = 0;
    }
  printf ("]");
}

static void
json_header (int pid, const char *db, const CS_LAYOUT * lo)
{
  printf ("{\"tool\":\"cub_ctv\",\"pid\":%d,\"db\":", pid);
  json_fstr (stdout, db);
  printf (",\"ts\":%ld,\"layout\":{\"stride\":%d,\"count\":%d,"
	  "\"stats_off\":%d},\"degraded\":%u,", view.ts,
	  lo->stride, lo->count, lo->off_stats, view.degraded);
}

/* one JSON document of the frame: header, the window's samples/dt (live),
 * the CS rows and/or the transaction table */
void
json_doc (int pid, const char *db, const CS_LAYOUT * lo, int window, int rows,
	  int trans)
{
  json_header (pid, db, lo);
  if (window)
    {
      printf ("\"samples\":%d,\"dt\":%.3f,", view.st.samples, view.dt);
    }
  if (rows)
    {
      json_cs_rows (stdout, lo, view.rows, window ? view.st.occ : NULL,
		    window ? view.st.wai : NULL, window ? view.st.samples : 0,
		    window ? view.prev : NULL, window ? view.dt : 0.0, 0);
    }
  if (rows && trans)
    {
      printf (",");
    }
  if (trans)
    {
      json_trans ();
    }
  printf ("}\n");
  fflush (stdout);
}
