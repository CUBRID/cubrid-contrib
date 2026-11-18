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
 * text rows - CS table rows, owner blocks, vertical records and the transaction table of a frame
 */

#include "ctv.h"

static void
print_trans_row (const VIEW_IDENT * v)
{
  char tb[16];

  if (view.has_trid)
    {
      snprintf (tb, sizeof (tb), "%d", v->trid);
    }
  else
    {
      snprintf (tb, sizeof (tb), "-");
    }
  {
    char u[64], p[96], l[64], h[96], ip[64];

    /* identities may be UTF-8: pad and cut by columns, not bytes */
    dw_pad (u, sizeof (u), v->id.user, 10);
    dw_pad (p, sizeof (p), v->id.program, 16);
    dw_pad (l, sizeof (l), v->id.login, 10);
    dw_pad (h, sizeof (h), v->id.host, 16);
    dw_pad (ip, sizeof (ip), v->id.ip, 15);
    printf ("%4d %8s  %s %s %s %s %s %8d\n", v->idx, tb, u, p, l, h, ip,
	    v->id.cpid);
  }
}

int
print_trans (void)
{
  int i;

  printf ("%s", clr (CLR_HEAD));
  printf (M ("transactions (from memory, no SQL connection):",
	     "트랜잭션 (메모리 판독, SQL 무접속):"));
  printf ("%s\n", clr (CLR_OFF));
  if (!view.tran_avail)
    {
      printf (M ("    (transaction table unavailable: %s)\n",
		 "    (트랜잭션 테이블 사용 불가: %s)\n"),
	      tran_refused ? tran_refused : "-");
      return -1;
    }
  printf ("%s%4s %8s  %-10s %-16s %-10s %-16s %-15s %8s%s\n", clr (CLR_HEAD),
	  "idx", "trid", "user", "program", "login", "host", "client_ip",
	  "cpid", clr (CLR_OFF));
  for (i = 0; i < view.nident; i++)
    {
      print_trans_row (&view.ident[i]);
    }
  if (view.nident == 0 && !view.cid_locked)
    {
      /* the layout locks on the first client: none attached, or not
       * recognised - the two cannot be told apart from memory */
      printf (M ("    (no client identity: no client attached, or the CLIENTIDS layout not recognised)\n",
		 "    (클라이언트 신원 없음: 접속 클라이언트가 없거나 CLIENTIDS 레이아웃 미인식)\n"));
    }
  else if (view.nident == 0)
    {
      printf (M ("    (no client transaction)\n",
		 "    (클라이언트 트랜잭션 없음)\n"));
    }
  return 0;
}

int layout_mode = LAYOUT_AUTO;

int layout_detail = 0;

const char *detail_filter = NULL;	/* --detail NAME|IX, NULL = all */

/* one transaction seen with client pid cpid as text: SHORT " user program"
 * / " system" (after a "tran N"), LABEL "tran N (program pid P)" / "engine
 * (system tran 0)"; a client that has left the slot is marked */
char *
tran_text (int tran, int cpid, int style, char *buf, size_t n)
{
  TRAN_IDENT id;
  int r = tran >= 1 ? view_identity_seen (tran, cpid, &id, NULL) : -1;
  int have = r == 0 || r == 1;
  const char *gone = r == 1 || r == 2 ? M (", disconnected", ", 접속 종료") : "";

  if (style == TRAN_TEXT_SHORT)
    {
      if (r == 2)
	{
	  snprintf (buf, n, " pid %d%s", id.cpid, gone);
	}
      else if (have)
	{
	  snprintf (buf, n, " %s %s%s", id.user, id.program, gone);
	}
      else
	{
	  snprintf (buf, n, "%s", tran == 0 ? " system" : "");
	}
      return buf;
    }
  if (tran == 0)
    {
      snprintf (buf, n, M ("engine (system tran 0)", "엔진 (system tran 0)"));
    }
  else if (r == 2)
    {
      snprintf (buf, n, "tran %d (pid %d%s)", tran, id.cpid, gone);
    }
  else if (have)
    {
      snprintf (buf, n, "tran %d (%s pid %d%s)", tran, id.program, id.cpid, gone);
    }
  else if (tran > 0)
    {
      snprintf (buf, n, "tran %d", tran);
    }
  else
    {
      snprintf (buf, n, "-");
    }
  return buf;
}

/* the holder is the engine itself: the system tran, or an engine thread
 * holding without a tran (the sampler counts both as tran 0) */
int
owner_is_system (int tran, int thr)
{
  return tran == 0 || (tran < 0 && thr >= 0);
}

/* the writer queue to show and the tran each hop waits for: the current
 * queue, or (seen_cs >= 0: empty now) its chain as first seen */
int
chain_of (uint64_t wq, int seen_cs, int *widx, int *tran, int *cpid)
{
  int nw = seen_cs >= 0 ? view_seen_chain (seen_cs, widx, THR_WAITERS_MAX)
    : view_waiters (wq, widx, THR_WAITERS_MAX), i;

  for (i = 0; i < nw; i++)
    {
      tran[i] = view_tran_of (widx[i]);
      if (cpid != NULL)
	{
	  cpid[i] = view_tran_cpid_of (widx[i]);
	}
    }
  return nw;
}

/* the count closing a chain of nw hops: all of waitW now, or "seen" for a
 * chain remembered from the window (seen) or a queue that outgrew waitW */
static const char *
chain_count (int nw, uint32_t waitw, int seen, char *buf, size_t n)
{
  if (!seen && waitw >= (uint32_t) nw)
    {
      snprintf (buf, n, "[%d of %u waiting]", nw, waitw);
    }
  else
    {
      snprintf (buf, n, "[%d seen]", nw);
    }
  return buf;
}

/* owner + writer wait queue of one CS, in two shapes:
 *   one : "  [user@host program pid=N thr=T trid=X]  {wq: thr=A(tran 1), ..}"
 *   l1  : "owner  user@host program pid=N login=.. ip=.. thr=T trid=X"
 *   l2  : "wq     thr=A(tran 1 user prog) -> thr=B(..)  [n of waitW waiting]"
 * every string is "" when nothing is known */
static void
owner_block (int tran_index, int cpid, uint64_t owner_tid, uint64_t wq, int seen_cs,
	     uint32_t waitw, char *one, size_t onen, char *l1, size_t l1n,
	     char *l2, size_t l2n)
{
  TRAN_IDENT id;
  int32_t trid = 0;
  int thr = view_slot_of_tid (owner_tid), have_id = 0, r;
  int widx[THR_WAITERS_MAX], wtran[THR_WAITERS_MAX], wcpid[THR_WAITERS_MAX], nw, i;
  size_t o = 0, a = 0, b = 0;
  char cnt[48];

  one[0] = l1[0] = l2[0] = '\0';
  r = tran_index >= 1 ? view_identity_seen (tran_index, cpid, &id, &trid) : -1;
  if (r == 0 || r == 1)
    {
      have_id = 1;
    }
  if (owner_is_system (tran_index, thr))
    {
      /* system tran, or an engine thread holding it without a tran (the
       * sampler counts both as tran 0) */
      const OS_THR *os = thr >= 0 ? view_os_of (thr) : NULL;
      const char *nm = os != NULL && os->comm[0] ? os->comm : "";

      buf_cat (one, onen, &o, "  [system%s%s", nm[0] ? " " : "", nm);
      buf_cat (l1, l1n, &a, "owner  system%s%s", nm[0] ? " " : "", nm);
    }
  else if (have_id)
    {
      buf_cat (one, onen, &o, "  [%s@%s %s pid=%d", id.user,
		     id.host, id.program, id.cpid);
      buf_cat (l1, l1n, &a, "owner  %s@%s %s pid=%d login=%s ip=%s",
		     id.user, id.host, id.program, id.cpid, id.login, id.ip);
    }
  else if (r == 2)
    {
      buf_cat (one, onen, &o, "  [tran %d pid=%d", tran_index, id.cpid);
      buf_cat (l1, l1n, &a, "owner  tran %d pid=%d", tran_index, id.cpid);
    }
  else if (tran_index >= 1)
    {
      buf_cat (one, onen, &o, "  [tran %d", tran_index);
      buf_cat (l1, l1n, &a, "owner  tran %d", tran_index);
    }
  if (o > 0)
    {
      if (thr >= 0)
	{
	  buf_cat (one, onen, &o, " thr=%d", THR_INDEX (thr));
	  buf_cat (l1, l1n, &a, "  thr=%d", THR_INDEX (thr));
	}
      if (r == 0 && view.has_trid)
	{
	  buf_cat (one, onen, &o, " trid=%d", trid);
	  buf_cat (l1, l1n, &a, "  trid=%d", trid);
	}
      if (r == 1 || r == 2)
	{
	  buf_cat (one, onen, &o, "%s", M (" disconnected", " 접속 종료"));
	  buf_cat (l1, l1n, &a, "  %s", M ("disconnected", "접속 종료"));
	}
      buf_cat (one, onen, &o, "]");
    }
  nw = chain_of (wq, seen_cs, widx, wtran, wcpid);
  for (i = 0; i < nw; i++)
    {
      int t = wtran[i];
      char ts[160], h1[64], h2[256];

      snprintf (h1, sizeof (h1), "%s thr=%d", i ? "," : "  {wq:",
		THR_INDEX (widx[i]));
      snprintf (h2, sizeof (h2), "%sthr=%d", i ? " -> " : "wq     ",
		THR_INDEX (widx[i]));
      if (t >= 0)
	{
	  size_t n1 = strlen (h1), n2 = strlen (h2);

	  snprintf (h1 + n1, sizeof (h1) - n1, "(tran %d)", t);
	  snprintf (h2 + n2, sizeof (h2) - n2, "(tran %d%s)", t,
		    tran_text (t, wcpid[i], TRAN_TEXT_SHORT, ts, sizeof (ts)));
	}
      /* a hop is added whole or not at all; the count still closes the line */
      if (o + strlen (h1) + TAG_TAIL_RESERVE >= onen
	  || b + strlen (h2) + TAG_TAIL_RESERVE >= l2n)
	{
	  buf_cat (one, onen, &o, ",..");
	  buf_cat (l2, l2n, &b, " -> ..");
	  break;
	}
      buf_cat (one, onen, &o, "%s", h1);
      buf_cat (l2, l2n, &b, "%s", h2);
    }
  if (nw > 0)
    {
      chain_count (nw, waitw, seen_cs >= 0, cnt, sizeof (cnt));
      buf_cat (one, onen, &o, " %s}", cnt);
      buf_cat (l2, l2n, &b, "  %s", cnt);
    }
}

/* printed width of a string: ANSI escape sequences do not count */
static size_t
visible_len (const char *s)
{
  size_t n = 0;

  /* terminal columns: ANSI sequences 0, 3/4-byte UTF-8 (Hangul) 2 */
  while (*s != '\0')
    {
      int len;

      if (*s == '\033')
	{
	  while (*s != '\0' && *s != 'm')
	    {
	      s++;
	    }
	  if (*s == '\0')
	    {
	      break;
	    }
	  s++;
	  continue;
	}
      n += (size_t) seq_cols (s, &len);
      while (len-- > 0 && *s != '\0')
	{
	  s++;
	}
    }
  return n;
}

/* one row and its owner block: on one line when it fits the terminal,
 * otherwise on indented continuation lines (a very long wait chain ends in
 * "..": --detail lists every hop) */
static void
print_row_tagged (const char *row, const char *one, const char *l1,
		  const char *l2)
{
  int cols = term_columns ();
  int split = layout_mode == LAYOUT_NARROW
    || (layout_mode == LAYOUT_AUTO && cols > 0
	&& visible_len (row) + visible_len (one) > (size_t) cols);

  if (one[0] == '\0' || !split)
    {
      const char *wq = color_on ? strstr (one, "  {wq:") : NULL;

      if (wq != NULL)
	{
	  printf ("%s%s%.*s%s%s%s\n", row, clr (CLR_OWNER), (int) (wq - one),
		  one, clr (CLR_WQ), wq, clr (CLR_OFF));
	}
      else
	{
	  printf ("%s%s%s%s\n", row, one[0] ? clr (CLR_OWNER) : "", one,
		  one[0] ? clr (CLR_OFF) : "");
	}
      return;
    }
  printf ("%s\n", row);
  if (l1[0] != '\0')
    {
      printf ("      | %s%s%s\n", clr (CLR_OWNER), l1, clr (CLR_OFF));
    }
  if (l2[0] != '\0')
    {
      printf ("      | %s%s%s\n", clr (CLR_WQ), l2, clr (CLR_OFF));
    }
}

/* owner fields count only while someone holds the CS (cs_held) */
int32_t
row_tran (const CS_ROW * r)
{
  return cs_held (r->rwlock, r->owner) ? r->tran : -1;
}

uint64_t
row_owner (const CS_ROW * r)
{
  return cs_held (r->rwlock, r->owner) ? r->owner : 0;
}

/* every field of one CS as a vertical record: SHOW's 12 columns, re-enter,
 * owner identity and the whole writer wait chain */
void
print_detail_row (int i, const CS_ROW * r)
{
  TRAN_IDENT id;
  int32_t trid = 0;
  char sb[32], ts[160];
  int thr = view_slot_of_tid (row_owner (r));
  int32_t tran = row_tran (r);
  int widx[THR_WAITERS_MAX], wtran[THR_WAITERS_MAX], wcpid[THR_WAITERS_MAX], nw, k, seen_cs;
  char cnt[48];

  printf ("%s*************************** %d. %s ***************************%s\n",
	  clr (CLR_HEAD), i, r->name, clr (CLR_OFF));
  printf ("state            : %-16s waitR/waitW : %u / %u\n",
	  state_str (r->rwlock, sb, sizeof (sb)), r->wr, r->ww);
  if (tran >= 1 && view_identity (tran, &id, &trid) == 0)
    {
      printf ("owner tran       : %d", tran);
      if (view.has_trid)
	{
	  printf (" (trid %d)", trid);
	}
      printf ("\n");
      printf ("owner client     : %s@%s  program %s  login %s  ip %s  pid %d\n",
	      id.user, id.host, id.program, id.login, id.ip, id.cpid);
    }
  else if (owner_is_system (tran, thr))
    {
      const OS_THR *os = thr >= 0 ? view_os_of (thr) : NULL;

      printf ("owner tran       : 0 (system%s%s)\n",
	      os != NULL && os->comm[0] ? " " : "", os != NULL ? os->comm : "");
    }
  else
    {
      if (tran > 0)
	{
	  printf ("owner tran       : %d\n", tran);
	}
      else
	{
	  printf ("owner tran       : -\n");
	}
    }
  if (thr >= 0)
    {
      printf ("owner thread     : %d\n", THR_INDEX (thr));
    }
  seen_cs = r->wq == 0 && view.st.seen_wq[i] != 0 ? i : -1;	/* as the row */
  nw = chain_of (r->wq, seen_cs, widx, wtran, wcpid);
  if (nw > 0)
    {
      for (k = 0; k < nw; k++)
	{
	  int t = wtran[k];

	  printf ("%s thr %d", k == 0 ? "waiting writers  :" : "                  ",
		  THR_INDEX (widx[k]));
	  if (t >= 0)
	    {
	      printf (" (tran %d%s)", t, tran_text (t, wcpid[k], TRAN_TEXT_SHORT, ts, sizeof (ts)));
	    }
	  printf ("\n");
	}
      printf ("                   %s\n",
	      chain_count (nw, r->ww, seen_cs >= 0, cnt, sizeof (cnt)));
    }
  else if (r->ww > 0)
    {
      printf ("waiting writers  : %u (chain not readable)\n", r->ww);
    }
  if (r->has_stats)
    {
      printf ("nenter / nwait   : %u / %u      reenter : %u\n", r->nenter,
	      r->nwait, r->nreenter);
      printf ("total / max wait : %.1f ms / %.3f ms\n", r->total_ms,
	      r->max_ms);
    }
  else
    {
      printf ("stats            : - (block did not validate)\n");
    }
}

/* CS i (name) passes the --detail filter */
int
detail_match (int i, const char *name)
{
  char ix[16];

  if (detail_filter == NULL)
    {
      return 1;
    }
  snprintf (ix, sizeof (ix), "%d", i);
  return strcmp (detail_filter, ix) == 0 || strcasecmp (detail_filter, name) == 0;
}

/* snapshot detail: all CS, or those matching the --detail filter */
void
print_detail (const CS_LAYOUT * lo, const CS_ROW * rows)
{
  int i, shown = 0;

  for (i = 0; i < lo->count; i++)
    {
      if (!detail_match (i, rows[i].name))
	{
	  continue;
	}
      print_detail_row (i, &rows[i]);
      shown++;
    }
  if (shown == 0)
    {
      char f[256];

      snprintf (f, sizeof (f), "%s", detail_filter ? detail_filter : "");
      text_sanitize (f);
      printf (M ("(no critical section matches %s)\n",
		 "(일치하는 크리티컬 섹션 없음: %s)\n"), f);
    }
}

void
print_snapshot (const CS_LAYOUT * lo, const CS_ROW * rows)
{
  char sb[32];
  int i;

  printf ("%six  name                      state        waitR waitW  tran"
	  "     nenter     nwait  reenter   total_ms    max_ms%s\n",
	  clr (CLR_HEAD), clr (CLR_OFF));
  for (i = 0; i < lo->count; i++)
    {
      const CS_ROW *r = &rows[i];
      char tr[16], st[80], row[256];
      char one[TAG_LEN], l1[TAG_LEN], l2[TAG_LEN];

      if (row_tran (r) == -1)
	{
	  snprintf (tr, sizeof (tr), "-");
	}
      else
	{
	  snprintf (tr, sizeof (tr), "%d", row_tran (r));
	}
      /* stats block that did not validate */
      if (r->has_stats)
	{
	  snprintf (st, sizeof (st), "%10u %9u %8u %10.1f %9.3f",
		    r->nenter, r->nwait, r->nreenter, r->total_ms,
		    r->max_ms);
	}
      else
	{
	  snprintf (st, sizeof (st), "%10s %9s %8s %10s %9s",
		    "-", "-", "-", "-", "-");
	}
      snprintf (row, sizeof (row), "%2d  %-24s %s%-12s%s %5u %5u %5s %s",
		i, r->name, r->rwlock < 0 ? clr (CLR_WRITER) : "",
		state_str (r->rwlock, sb, sizeof (sb)),
		r->rwlock < 0 ? clr (CLR_OFF) : "", r->wr, r->ww, tr, st);
      owner_block (row_tran (r), -1, row_owner (r), r->wq, -1, r->ww, one, sizeof (one),
		   l1, sizeof (l1), l2, sizeof (l2));
      print_row_tagged (row, one, l1, l2);
    }
}

void
live_print_row (int i, double dt)
{
  const CS_ROW *c = &view.rows[i], *p = &view.prev[i];
  CS_RATES rt;
  double de, dw, occ_pct;
  int fill, b;
  int held = cs_held (c->rwlock, c->owner);
  int tg = held ? c->tran : view.st.seen_tran[i];
  int tg_cpid = held ? -1 : view.st.seen_cpid[i];
  uint64_t owner = held ? c->owner : view.st.seen_owner[i];
  uint64_t wq = c->wq;
  int seen_cs = c->wq == 0 && view.st.seen_wq[i] != 0 ? i : -1;
  char bar[12], sb[32], row[320];
  char one[TAG_LEN], l1[TAG_LEN], l2[TAG_LEN];

  cs_rates (c, p, view.st.occ[i], view.st.wai[i], view.st.samples, dt, &rt);
  de = rt.enter_s;
  dw = rt.wait_s;
  occ_pct = rt.occ_pct;
  fill = (int) (occ_pct / 10.0 + 0.5);
  for (b = 0; b < 10; b++)
    {
      bar[b] = b < fill ? '#' : '.';
    }
  bar[10] = '\0';
  {
    int waiting = view.st.wai[i] > 0 || dw > 0.0;
    int idle = !waiting && view.st.occ[i] == 0 && de == 0.0 && c->rwlock == 0;
    const char *barc = occ_pct >= 80 ? clr (CLR_BAR_HIGH)
      : occ_pct >= 50 ? clr (CLR_BAR_MID) : clr (CLR_BAR_LOW);

    snprintf (row, sizeof (row),
	      "%s%2d  %-24s %5.1f %s%s%s %6.1f %9.1f %9.1f %5u %5u  %s%s%s%s%s",
	      idle ? clr (CLR_MUTED) : "", i, c->name, occ_pct, barc, bar,
	      idle ? clr (CLR_MUTED) : clr (CLR_OFF),
	      rt.wait_pct, de, dw, c->wr, c->ww,
	      c->rwlock < 0 ? clr (CLR_WRITER) : "",
	      state_str (c->rwlock, sb, sizeof (sb)),
	      waiting ? clr (CLR_WAIT) : "", waiting ? "  <-- WAIT" : "",
	      (idle || waiting || c->rwlock < 0) ? clr (CLR_OFF) : "");
  }
  owner_block (tg, tg_cpid, owner, wq, seen_cs, c->ww, one, sizeof (one), l1, sizeof (l1),
	       l2, sizeof (l2));
  print_row_tagged (row, one, l1, l2);
}

/* CS i had activity in the frame's window */
int
row_moved (int i)
{
  const CS_ROW *c = &view.rows[i], *p = &view.prev[i];

  return c->nenter != p->nenter || c->nwait != p->nwait || view.st.occ[i] != 0
    || view.st.wai[i] != 0;
}

/* pool groups of the frame (read from the server, or from a record) */
void
print_view_pool (void)
{
  if (view.pool_state == 1)
    {
      pool_print_groups (view.pool, view.npool, view.pool_truncated);
    }
  else if (view.pool_state < 0)
    {
      warn ("stats pool did not validate - skipping pool view",
	    "통계 풀 검증 실패 - 풀 뷰 생략");
    }
  else
    {
      printf (M ("\n[pool] not collected (sync_Stats not exported)\n",
		 "\n[풀] 미수집 (sync_Stats 미노출)\n"));
    }
}
