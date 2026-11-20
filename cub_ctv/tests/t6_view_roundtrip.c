/*
 * t6: record/replay round trip without a server.  The memory-backed queries
 * are replaced (ld --wrap) by synthetic answers that exercise the record
 * lines of a snapshot frame: identities with spaces, '%', '~' and empty
 * strings, owner threads, writer wait chains and pool groups.  "record" captures and renders;
 * "replay" loads the record and renders: the two outputs must be identical.
 */
#include "ctv.h"

int
__wrap_thr_index_of_tid_mem (uint64_t tid)
{
  return (int) (tid % 1000);
}

int
__wrap_thr_waiters_mem (uint64_t head, int *idx, int *tran, int max, uint64_t anchor)
{
  int n = (int) (head & 0xf), k;

  (void) anchor;
  for (k = 0; k < n && k < max; k++)
    {
      idx[k] = 100 + (int) (head % 50) + k;
      if (tran != NULL)
	{
	  tran[k] = idx[k] % 9;
	}
    }
  return k;
}

int
__wrap_thr_waiters_mem2 (uint64_t head, uint64_t head2, int *idx, int *tran,
			 int max, uint64_t anchor_at)
{
  int n = __wrap_thr_waiters_mem (head, idx, tran, max, anchor_at);

  if (head2 != 0 && n < max)
    {
      n += __wrap_thr_waiters_mem (head2, idx + n, tran != NULL ? tran + n : NULL,
				   max - n, 0);
    }
  return n;
}

int
__wrap_tran_identity_cached (int t, TRAN_IDENT * id, int32_t * trid)
{
  if (t < 1 || t > 6)
    {
      return -1;
    }
  memset (id, 0, sizeof (*id));
  snprintf (id->user, sizeof (id->user), t == 3 ? "DBA" : "user %d%%~", t);
  snprintf (id->program, sizeof (id->program), t == 4 ? "" : "prog-%d", t);
  snprintf (id->login, sizeof (id->login), "login%d", t);
  snprintf (id->host, sizeof (id->host), "host %d", t);
  snprintf (id->ip, sizeof (id->ip), "10.0.0.%d", t);
  id->cpid = 4000 + t;
  if (trid != NULL)
    {
      *trid = 1000 + t;
    }
  return 0;
}

int
main (int argc, char **argv)
{
  CS_LAYOUT lo;
  CS_ROW rows[4];
  VIEW_HDR h;
  int i;

  if (argc != 3)
    {
      return 2;
    }
  memset (&lo, 0, sizeof (lo));
  lo.stride = 160;
  lo.count = 4;
  lo.off_stats = 152;
  memset (rows, 0, sizeof (rows));
  for (i = 0; i < 4; i++)
    {
      snprintf (rows[i].name, sizeof (rows[i].name), "CS_%d", i);
      rows[i].rwlock = i == 0 ? 0 : -1;
      rows[i].tran = i == 0 ? -1 : i;
      rows[i].owner = i == 0 ? 0 : 4000 + i;
      rows[i].wq = i == 0 ? 0 : 0x7f0000000000ULL + 0x100 * i + 2 * i;
      rows[i].ww = 6;
      rows[i].has_stats = i != 2;
      rows[i].nenter = 1000u * i;
      rows[i].nwait = 10u * i;
      rows[i].total_ms = 1.0 / 3.0 * i;
      rows[i].max_ms = 0.1234567 * i;
    }
  layout_mode = LAYOUT_NARROW;
  if (strcmp (argv[1], "record") == 0)
    {
      tran_geom.all_tdes = 1;
      tran_geom.ntran = 8;
      tran_geom.cid_stride = COW_STRIDE;
      tran_geom.has_trid = 1;
      memset (&h, 0, sizeof (h));
      h.lo = lo;
      snprintf (h.db, sizeof (h.db), "t6 db");
      snprintf (h.lib, sizeof (h.lib), "/x/lib%%.so");
      if (view_record_open (argv[2], &h, 0) != 0)
	{
	  return 1;
	}
      view_capture (&lo, rows, NULL, 0.0);
      view.pool_state = 1;
      view.npool = 2;
      view.pool_truncated = -1;
      view.pool[0].type = 3;
      snprintf (view.pool[0].name, sizeof (view.pool[0].name), "CONN ENTRY");
      view.pool[0].cnt = 3;
      view.pool[0].nenter = 123456789012ULL;
      view.pool[0].max_ms = 1.5;
      view.pool[1].type = 2;
      snprintf (view.pool[1].name, sizeof (view.pool[1].name), "~lock%%");
      view.pool[1].cnt = 1;
      view_record_frame ();
    }
  else
    {
      view_replay_open (argv[2], &h);
      if (view_replay_next () != 0)
	{
	  return 1;
	}
    }
  print_snapshot (&lo, view.rows);
  print_detail (&lo, view.rows);
  print_trans ();
  print_view_pool ();
  json_cs_rows (stdout, &lo, view.rows, NULL, NULL, 0, NULL, 0.0, 0);
  json_trans ();
  printf ("\n");
  return 0;
}
