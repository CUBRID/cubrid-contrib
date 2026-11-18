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
 * summary screen - the full-screen view of a frame, fitted to the terminal
 */

#include "ctv.h"

/* ------------------------------------------------- full-screen summary */

/* screen width of this frame, 0 = unlimited (not a terminal) */
static int scr_cols = 0;

/* terminal columns of s[0..n); escape sequences take none */
static int
vis_cols (const char *s, size_t n)
{
  size_t i = 0;
  int cols = 0, len;

  while (i < n)
    {
      if (s[i] == '\033' && i + 1 < n && s[i + 1] == '[')
	{
	  for (i += 2; i < n && !(s[i] >= 0x40 && s[i] <= 0x7E); i++)
	    {
	      ;
	    }
	  i++;
	  continue;
	}
      cols += seq_cols (s + i, &len);
      i += (size_t) len;
    }
  return cols;
}

/* screen rows a line of 'cols' columns takes */
static int
line_rows (int cols)
{
  return scr_cols > 0 && cols > scr_cols ? (cols + scr_cols - 1) / scr_cols : 1;
}

/* text written from column 'at', broken at spaces to the screen width;
 * continuation lines start at column 'indent' (no final newline).
 * Returns the line breaks made; out NULL only counts them */
static int
wrap_out (FILE * out, const char *text, int at, int indent)
{
  const char *p = text;
  int col = at, len, breaks = 0;

  while (*p != '\0')
    {
      const char *w = p;
      int sp = 0, ww = 0;

      while (*w == ' ')
	{
	  sp++, w++;
	}
      for (len = 0; w[len] != '\0' && w[len] != ' ';)
	{
	  int l;

	  ww += seq_cols (w + len, &l);
	  len += l;
	}
      if (scr_cols > 0 && col + sp + ww > scr_cols && col > indent)
	{
	  if (out != NULL)
	    {
	      fprintf (out, "\n%*s", indent, "");
	    }
	  col = indent, sp = 0, breaks++;
	}
      if (out != NULL)
	{
	  fprintf (out, "%*s%.*s", sp, "", len, w);
	}
      col += sp + ww;
      p = w + len;
    }
  return breaks;
}

/* one "item | judgement | evidence" line of the verdict table; a screen
 * narrower than the columns keeps every word on more lines */
static void
verdict_row (FILE * out, const char *item, const char *judge, const char *evidence)
{
  char c1[64], c2[256];
  int ej = str_cols (judge), ee = str_cols (evidence), at = 61;

  dw_pad (c1, sizeof (c1), item, 11);
  fprintf (out, "  %s ", c1);
  if (evidence[0] == '\0')
    {
      wrap_out (out, judge, 14, 14);	/* a single long cell */
      fprintf (out, "\n");
      return;
    }
  if (ej <= 46 && (scr_cols == 0 || at + ee <= scr_cols))
    {
      dw_pad (c2, sizeof (c2), judge, 46);
      fprintf (out, "%s %s%s%s\n", c2, clr (CLR_MUTED), evidence, clr (CLR_OFF));
      return;
    }
  if (ej <= 46 && 14 + ej + 2 + ee <= scr_cols)
    {
      /* narrow: both on one line, the evidence right after the judgement */
      fprintf (out, "%s  %s%s%s\n", judge, clr (CLR_MUTED), evidence, clr (CLR_OFF));
      return;
    }
  /* the judgement on its line, the evidence under its column, or under
   * the judgement when the screen is narrower than that column */
  wrap_out (out, judge, 14, 14);
  at = scr_cols == 0 || at + ee <= scr_cols ? at : 14;
  fprintf (out, "\n%*s%s", at, "", clr (CLR_MUTED));
  wrap_out (out, evidence, at, at);
  fprintf (out, "%s\n", clr (CLR_OFF));
}

static void
print_verdict (FILE * out, const CS_FIG * top, const CS_FIG * second)
{
  VERDICT v;
  const CS_ROW *c = &view.rows[top->ix];
  char hb[96], judge[160], ev[200], dots[8];
  int k;
  const char *sev = top->severity >= 4 ? M ("high", "높음")
    : top->severity == 3 ? M ("moderate", "중간") : M ("low", "낮음");

  verdict_compute (top, second, &v);
  for (k = 0; k < 4; k++)
    {
      dots[k] = k < top->severity ? '*' : '.';
    }
  dots[4] = '\0';
  snprintf (judge, sizeof (judge), M ("[verdict] %s (%s) bottleneck  severity %s %s",
				      "[판정] %s (%s) 병목  심각도 %s %s"),
	    cs_desc (c->name), c->name, dots, sev);
  snprintf (ev, sizeof (ev), M ("persistence: %d of the last %d windows",
				"지속: %d구간 (최근 %d구간 중)"), v.same, v.span);
  /* the persistence goes under the title when both do not fit */
  fprintf (out, "%s%s%s%s%s%s%s\n", clr (top->severity >= 3 ? CLR_WAIT : CLR_HEAD),
	   judge, clr (CLR_OFF), clr (CLR_MUTED),
	   scr_cols > 0 && str_cols (judge) + 3 + str_cols (ev) > scr_cols ? "\n  " : "   ",
	   ev, clr (CLR_OFF));
  {
    char h1[64], h2[160];

    dw_pad (h1, sizeof (h1), M ("item", "항목"), 11);
    dw_pad (h2, sizeof (h2), M ("judgement", "판단"), 46);
    fprintf (out, "%s  %s %s %s%s\n", clr (CLR_HEAD), h1, h2, M ("evidence", "근거"),
	    clr (CLR_OFF));
  }

  /* where */
  snprintf (judge, sizeof (judge), "%s (%s)", cs_desc (c->name), c->name);
  if (v.has_second)
    {
      snprintf (ev, sizeof (ev), M ("lost %.2f s/s; next %s %.2f s/s",
				    "손실 %.2f s/s; 2위 %s %.2f s/s"),
		top->lost_s, view.rows[v.second.ix].name, v.second.lost_s);
    }
  else
    {
      snprintf (ev, sizeof (ev), M ("lost %.2f s/s; no other CS waiting",
				    "손실 %.2f s/s; 다른 CS는 대기 없음"),
		top->lost_s);
    }
  verdict_row (out, M ("where", "어디서"), judge, ev);

  /* who */
  if (v.readers_most)
    {
      snprintf (judge, sizeof (judge), "%s",
		M ("readers (no single owner)", "reader 다수 (단일 소유자 없음)"));
    }
  else if (v.holder == 0)
    {
      const char *dn = daemon_name (top->ix);

      if (dn[0])
	{
	  snprintf (judge, sizeof (judge), M ("engine thread %s", "엔진 스레드 %s"), dn);
	}
      else
	{
	  snprintf (judge, sizeof (judge), "%s", strcmp (c->name, "LOG") == 0
		    ? M ("engine log flush daemon", "엔진 로그 플러시 데몬")
		    : M ("engine daemon (system tran)", "엔진 데몬 (system tran)"));
	}
    }
  else if (v.holder > 0)
    {
      snprintf (judge, sizeof (judge), M ("client session %s", "클라이언트 세션 %s"),
		tran_text (v.holder, v.holder_cpid, TRAN_TEXT_LABEL, hb, sizeof (hb)));
    }
  else
    {
      snprintf (judge, sizeof (judge), "%s", view.st.occ[top->ix] > 0
		? M ("readers (no single owner)", "reader 다수 (단일 소유자 없음)")
		: M ("not observed", "관측 안 됨"));
    }
  if (v.readers_most)
    {
      /* the writer seen is named as evidence only */
      const char *dn = daemon_name (top->ix);

      if (v.holder > 0)
	{
	  tran_text (v.holder, v.holder_cpid, TRAN_TEXT_LABEL, hb, sizeof (hb));
	}
      else
	{
	  snprintf (hb, sizeof (hb), "%s", dn[0] ? dn : M ("engine", "엔진"));
	}
      snprintf (ev, sizeof (ev), M ("writer %s held %.0f%% of the window (occupied %.0f%%)",
				    "writer %s 구간의 %.0f%% 보유 (점유 %.0f%%)"),
		hb, v.held_pct, top->occ_pct);
    }
  else if (v.one_thr && v.thr_pct < v.held_pct - 0.5)
    {
      snprintf (ev, sizeof (ev), M ("held %.0f%% of the window (engine threads together %.0f%%)",
				    "구간의 %.0f%% 보유 (엔진 스레드 합계 %.0f%%)"),
		v.thr_pct, v.held_pct);
    }
  else
    {
      snprintf (ev, sizeof (ev), M ("held %.0f%% of the window (occupied %.0f%%)",
				    "구간의 %.0f%% 보유 (점유 %.0f%%)"), v.held_pct,
		top->occ_pct);
    }
  verdict_row (out, M ("who", "누가"), judge, ev);

  /* how bad */
  snprintf (judge, sizeof (judge), "%s (%d/4)", sev, top->severity);
  if (top->avg_ms > 0)
    {
      snprintf (ev, sizeof (ev), M ("%.0f of %.0f entries/s waited (%.0f%%), acquire %.3f ms/entry",
				    "대기 %.0f건/s (진입 %.0f건/s 의 %.0f%%), 진입당 획득 %.3f ms"),
		top->wait_s, top->enter_s,
		top->enter_s > 0 ? 100.0 * top->wait_s / top->enter_s : 0.0,
		top->avg_ms);
    }
  else
    {
      snprintf (ev, sizeof (ev), M ("%.0f of %.0f entries/s waited; waiters seen %.0f%% of samples",
				    "대기 %.0f건/s (진입 %.0f건/s); 대기자 목격 %.0f%% 샘플"),
		top->wait_s, top->enter_s, top->wait_pct);
    }
  verdict_row (out, M ("how bad", "얼마나"), judge, ev);

  /* capacity, when a daemon holds it: one thread saturates, not several
   * sharing the work */
  if (v.holder == 0 && !v.readers_most)
    {
      snprintf (judge, sizeof (judge), "%s", v.thr_pct >= 60
		? M ("near the daemon's capacity", "데몬 처리 상한 근접")
		: M ("headroom remains", "여유 있음"));
      snprintf (ev, sizeof (ev), M ("daemon holds %.0f%% -> %.0f%% headroom at %.0f entries/s",
				    "데몬 보유 %.0f%% -> 여유 %.0f%%, 진입 %.0f건/s"),
		v.thr_pct, 100.0 - v.thr_pct, top->enter_s);
      verdict_row (out, M ("capacity", "처리량"), judge, ev);
    }

  /* who suffers: the share of the window each was seen waiting here */
  if (v.nwaiters > 0)
    {
      size_t o = 0;

      snprintf (judge, sizeof (judge), M ("%d client sessions, waited ~%.0f%% of the window each",
					  "클라이언트 세션 %d개, 각 구간의 약 %.0f%% 대기"),
		v.nwaiters, v.waited_pct);
      ev[0] = '\0';
      for (k = 0; k < v.nlisted; k++)
	{
	  buf_cat (ev, sizeof (ev), &o, "%stran %d (%d)", k ? ", " : "",
		   v.wtran[k], v.wsamples[k]);
	}
      if (v.more_waiters)
	{
	  buf_cat (ev, sizeof (ev), &o, ", ...");
	}
      verdict_row (out, M ("who suffers", "피해"), judge, ev);
    }
  else
    {
      verdict_row (out, M ("who suffers", "피해"),
		   M ("no client session identified", "식별된 클라이언트 세션 없음"),
		   M ("waiters were engine threads or not resolved",
		      "대기자가 엔진 스레드이거나 미식별"));
    }

  /* action */
  if (v.readers_most)
    {
      snprintf (judge, sizeof (judge), "%s", M ("check read volume and the writing session",
						"읽기 빈도와 쓰기 세션을 함께 볼 것"));
      snprintf (ev, sizeof (ev), M ("occupied %.0f%%, writer held %.0f%%; readers the rest",
				    "점유 %.0f%% 중 writer 보유 %.0f%%, 나머지는 reader"),
		top->occ_pct, v.held_pct);
    }
  else if (v.holder == 0)
    {
      snprintf (judge, sizeof (judge), "%s", M ("not a session problem -> look at the engine side",
						"세션 조치 불필요 -> 엔진 쪽을 볼 것"));
      snprintf (ev, sizeof (ev), "%s", M ("holder is an engine daemon", "쥔 쪽이 엔진 데몬"));
    }
  else if (v.holder > 0)
    {
      snprintf (judge, sizeof (judge), "%s", M ("inspect the holding session",
						"쥔 세션을 확인할 것"));
      snprintf (ev, sizeof (ev), "%s", M ("a client holds it longest", "클라이언트가 가장 오래 쥠"));
    }
  else
    {
      snprintf (judge, sizeof (judge), "%s", M ("reduce the entry rate", "진입 빈도를 줄일 것"));
      snprintf (ev, sizeof (ev), "%s", M ("no writer holds it; contention is by volume",
					  "writer 없음; 횟수에 의한 경합"));
    }
  verdict_row (out, M ("action", "조치"), judge, ev);
  verdict_row (out, M ("check", "확인할 것"), cs_hint (c->name), "");

  /* disk or CPU */
  if (v.bound == VB_NONE)
    {
      verdict_row (out, M ("not shown", "화면 밖"),
		   M ("disk vs daemon CPU (needs disk await); SQL text; service impact (SLA)",
		      "디스크 vs 데몬 CPU 구분(디스크 await 필요); SQL 문장; 서비스 영향(SLA)"), "");
      return;
    }
  snprintf (judge, sizeof (judge), "%s", v.bound == VB_DISK
	    ? M ("disk write latency", "디스크 쓰기 지연")
	    : v.bound == VB_CPU ? M ("holder thread CPU-bound", "쥔 스레드 CPU 포화")
	    : v.bound == VB_SHORT
	    ? M ("neither disk nor CPU saturated -> short holds, high rate",
		 "디스크·CPU 모두 여유 -> 짧은 보유가 잦음")
	    : M ("holder thread state unknown", "쥔 스레드 상태 미확보"));
  ev[0] = '\0';
  if (v.os != NULL)
    {
      snprintf (ev, sizeof (ev), M ("%s: D %.0f%% R %.0f%% cpu %.0f%%",
				    "%s: D %.0f%% R %.0f%% cpu %.0f%%"),
		v.os->comm[0] ? v.os->comm : "thr", v.dsk_pct, v.run_pct, v.cpu_pct);
    }
  if (view.disk_avail)
    {
      size_t o = strlen (ev);

      snprintf (ev + o, sizeof (ev) - o, M ("%s%s await %.1f ms util %.0f%%",
					     "%s%s await %.1f ms util %.0f%%"),
		o ? "; " : "", view.disk_dev, view.disk_await_ms, view.disk_util_pct);
    }
  verdict_row (out, M ("disk/cpu", "디스크/CPU"), judge, ev);
  verdict_row (out, M ("not shown", "화면 밖"),
	       M ("SQL text; service impact (SLA)", "SQL 문장; 서비스 영향(SLA)"), "");
}

/* thread wait reasons of the frame: one line (snapshot) or a block (screen).
 * Printed only when the resume_status field validated (I-18). */
void
print_thread_waits (FILE * out, int screen)
{
  static const int codes[] = { 17, 3, 21, 7, 9, 11, 19, 13, 15, 23, 25, 26, 5 };
  char line[400];
  size_t o = 0;
  int k, waiting = 0, csw = 0;
  const char *sep = M (", ", " · ");	/* English output stays ASCII */

  if (!view.wait_avail)
    {
      return;
    }
  for (k = 0; k < (int) (sizeof (codes) / sizeof (codes[0])); k++)
    {
      int v = codes[k], n = view.wait_cnt[v];

      if (n == 0)
	{
	  continue;
	}
      if (v >= THR_RESUME_CS_MIN && v <= THR_RESUME_CS_MAX)
	{
	  csw += n;
	  continue;
	}
      waiting += n;
      buf_cat (line, sizeof (line), &o, "%s%s %d", o ? sep : "",
	       resume_name (v), n);
    }
  if (csw > 0)
    {
      buf_cat (line, sizeof (line), &o, "%s%s %d", o ? sep : "",
	       resume_name (9), csw);
      waiting += csw;
    }
  if (screen)
    {
      fprintf (out, "\n%s", clr (CLR_HEAD));
      fprintf (out, M ("[thread waits]  %d live threads, %d waiting",
		 "[스레드 대기]  실스레드 %d개, 대기 중 %d개"), view.wait_live, waiting);
      fprintf (out, "%s\n", clr (CLR_OFF));
      if (waiting > 0)
	{
	  /* broken between items, never inside one */
	  const char *p = line, *e;
	  int col = 3;

	  fprintf (out, "   ");
	  for (; *p != '\0'; p = *e ? e + strlen (sep) : e)
	    {
	      int w;

	      e = strstr (p, sep);
	      e = e != NULL ? e : p + strlen (p);
	      w = vis_cols (p, (size_t) (e - p));
	      if (p != line && scr_cols > 0 && col + str_cols (sep) + w > scr_cols)
		{
		  fprintf (out, "\n   ");
		  col = 3;
		}
	      else if (p != line)
		{
		  fprintf (out, "%s", sep);
		  col += str_cols (sep);
		}
	      fprintf (out, "%.*s", (int) (e - p), p);
	      col += w;
	    }
	  fprintf (out, "\n");
	}
    }
  else
    {
      fprintf (out, "%s", clr (CLR_MUTED));
      fprintf (out, M ("thread waits: %d live, %d waiting%s%s",
		 "스레드 대기: 실스레드 %d개, 대기 중 %d개%s%s"), view.wait_live,
	      waiting, waiting ? " - " : "", waiting ? line : "");
      fprintf (out, "%s\n", clr (CLR_OFF));
    }
}

/* client kind from the registered program name: "csql", "cas<n>" with its
 * broker ("broker1:cas2"), else the name itself */
static const char *
client_kind (const char *program, char *buf, size_t n)
{
  const char *p = strstr (program, "_cub_cas_");

  if (p != NULL && p > program && isdigit ((unsigned char) p[9]))
    {
      snprintf (buf, n, "%.*s:cas%s", (int) (p - program), program, p + 9);
    }
  else
    {
      snprintf (buf, n, "%s", program[0] ? program : "-");
    }
  return buf;
}

/* sessions: every tran seen holding or waiting, longest wait first */
typedef struct
{
  int tran, cpid, waits, holds;
  int first, n;			/* its sightings: act_by_tran[first ..+n) */
} SESS;

/* sightings of client trans ordered by (tran, client pid, position in the
 * window): a slot that changed hands gives one session per client */
static int act_by_tran[ACT_MAX];

static int
cmp_act_tran (const void *a, const void *b)
{
  int x = *(const int *) a, y = *(const int *) b;

  if (view.st.act[x].tran != view.st.act[y].tran)
    {
      return view.st.act[x].tran < view.st.act[y].tran ? -1 : 1;
    }
  if (view.st.act[x].cpid != view.st.act[y].cpid)
    {
      return view.st.act[x].cpid < view.st.act[y].cpid ? -1 : 1;
    }
  return x - y;
}

static int
cmp_sess (const void *a, const void *b)
{
  const SESS *x = a, *y = b;

  if (x->waits != y->waits)
    {
      return x->waits < y->waits ? 1 : -1;
    }
  if (x->holds != y->holds)
    {
      return x->holds < y->holds ? 1 : -1;
    }
  return x->tran < y->tran ? -1 : x->tran > y->tran;
}

static void
print_sessions (int panel_rows)
{
  static SESS sess[ACT_MAX];
  static int strans[ACT_MAX];
  int ns = 0, nst = 0, m = 0, k, j, shown = 0;
  char pad[128], cpad[64];

  /* one sort, then each session is a run of the same tran */
  for (k = 0; k < view.st.nact; k++)
    {
      if (view.st.act[k].tran >= 1)
	{
	  act_by_tran[m++] = k;
	}
    }
  qsort (act_by_tran, (size_t) m, sizeof (int), cmp_act_tran);
  for (k = 0; k < m;)
    {
      SESS *sp = &sess[ns++];

      sp->tran = view.st.act[act_by_tran[k]].tran;
      sp->cpid = view.st.act[act_by_tran[k]].cpid;
      sp->waits = sp->holds = 0;
      sp->first = k;
      for (; k < m && view.st.act[act_by_tran[k]].tran == sp->tran
	   && view.st.act[act_by_tran[k]].cpid == sp->cpid; k++)
	{
	  const ACT *a = &view.st.act[act_by_tran[k]];

	  if (a->kind == ACT_WAIT)
	    {
	      sp->waits += a->samples;
	    }
	  else
	    {
	      sp->holds += a->samples;
	    }
	}
      sp->n = k - sp->first;
      {
	TRAN_IDENT cur;

	/* the slot's current client counts as active (ascending tran order) */
	if (view_identity_seen (sp->tran, sp->cpid, &cur, NULL) == 0)
	  {
	    strans[nst++] = sp->tran;
	  }
      }
    }
  qsort (sess, (size_t) ns, sizeof (sess[0]), cmp_sess);

  {
    char cnt[96], drop[sizeof (pad)];

    snprintf (cnt, sizeof (cnt), M ("[sessions]  %d client transactions, %d active on a CS",
				    "[세션]  클라이언트 트랜잭션 %d개, CS 활동 %d개"),
	      view.nident, ns);
    drop[0] = '\0';
    if (view.st.act_dropped > 0)
      {
	snprintf (drop, sizeof (drop), M ("(+%d sightings not kept: activity table full)",
					  "(목격 %d건 미보관: 활동 표 가득 참)"),
		  view.st.act_dropped);
      }
    /* the count line is one screen row: the note is cut to the width */
    if (drop[0] != '\0' && scr_cols > 0 && str_cols (cnt) + 2 + str_cols (drop) > scr_cols)
      {
	int room = scr_cols - str_cols (cnt) - 2;

	dw_pad (pad, sizeof (pad), drop, room > 0 ? room : 0);
	snprintf (drop, sizeof (drop), "%s", pad);
      }
    printf ("%s%s%s%s%s\n", clr (CLR_HEAD), cnt, drop[0] ? "  " : "", drop, clr (CLR_OFF));
  }
  if (ns == 0 || panel_rows < 0)
    {
      return;			/* nothing to list, or no room: the count line */
    }
  printf ("%s", clr (CLR_HEAD));
  dw_pad (cpad, sizeof (cpad), M ("client", "클라이언트"), 14);
  printf ("%5s %9s %s %s", "tran", "pid", cpad, M ("what it did in the window", "이 구간에 한 일"));
  printf ("%s\n", clr (CLR_OFF));
  for (j = 0; j < ns && shown < panel_rows; j++, shown++)
    {
      const SESS *sp = &sess[j];
      TRAN_IDENT id;
      char line[512], pidb[16], kind[IDENT_LEN + 8], kpad[64];
      size_t o = 0;
      int r = view_identity_seen (sp->tran, sp->cpid, &id, NULL);
      int have = r >= 0;

      client_kind (r == 0 || r == 1 ? id.program : "", kind, sizeof (kind));

      line[0] = '\0';
      if (r == 1 || r == 2)
	{
	  /* the client that did this has left the slot since */
	  buf_cat (line, sizeof (line), &o, "%s", M ("(disconnected) ", "(접속 종료) "));
	}
      for (k = sp->first; k < sp->first + sp->n && o + 2 < sizeof (line); k++)
	{
	  const ACT *a = &view.st.act[act_by_tran[k]];

	  buf_cat (line, sizeof (line), &o, "%s", k > sp->first ? ", " : "");
	  buf_cat (line, sizeof (line), &o,
		   a->kind == ACT_WAIT
		   ? M ("waits %s (%d samples)", "%s 대기 (%d샘플)")
		   : M ("holds %s (%d samples)", "%s 보유 (%d샘플)"),
		   view.rows[a->cs].name, a->samples);
	  if (o + 1 >= sizeof (line))
	    {
	      break;
	    }
	}
      snprintf (pidb, sizeof (pidb), "%d", have ? id.cpid : 0);
      /* one screen row per session (the panel counts rows): the activity
       * text is cut to the columns left after tran, pid and client */
      {
	int cols = scr_cols, room = cols - 31 - 1;

	if (cols > 31 + 8 && str_cols (line) > room)
	  {
	    char cut[sizeof (line)];

	    dw_pad (cut, sizeof (cut), line, room);	/* cut at a column */
	    snprintf (line, sizeof (line), "%s", cut);
	  }
      }
      /* reverse = holds, underline = waits: attributes, kept under --plain */
      dw_pad (kpad, sizeof (kpad), kind, 14);	/* by columns: names may be UTF-8 */
      printf ("%s%5d %9s %s %s\033[0m\n",
	      sp->waits > 0 ? (color_on ? clr (CLR_PANEL_WAITER) : "\033[4m")
	      : (color_on ? clr (CLR_PANEL_OWNER) : "\033[7m"), sp->tran,
	      have ? pidb : "-", kpad, line);
    }
  if (shown < ns)
    {
      printf ("%s", clr (CLR_MUTED));
      printf (M ("  ... %d more active sessions", "  ... 활동 세션 %d개 더"), ns - shown);
      printf ("%s\n", clr (CLR_OFF));
    }
  {
    /* identified sessions with no CS activity in the window */
    int idle = 0;

    for (k = 0; k < view.nident; k++)
      {
	idle += view.ident[k].idx >= 1
	  && bsearch (&view.ident[k].idx, strans, (size_t) nst, sizeof (int),
		      cmp_int) == NULL;
      }
    if (idle > 0)
      {
	printf ("%s", clr (CLR_MUTED));
	printf (M ("  %d other sessions: no CS activity in this window",
		   "  나머지 %d개 세션: 이 구간에 CS 활동 없음"), idle);
	printf ("%s\n", clr (CLR_OFF));
      }
  }
}

void
screen_summary (const CS_LAYOUT * lo, double dt, int hz,
		double interval, int pid, const char *db, const char *hhmmss)
{
  CS_FIG fig[MAX_CS];
  int i, n = 0, idle = 0, rows = term_rows (), top = 0, tiny = 0;
  char d1[128];
  /* everything above the sessions panel is written to memory first, so
   * the panel gets exactly the rows that are left */
  char *top_buf = NULL;
  size_t top_len = 0;
  FILE *mem = open_memstream (&top_buf, &top_len);
  FILE *out = mem != NULL ? mem : stdout;
  static char whos[MAX_CS][96];
  int guards = 1, frows;
  const char *footer;

  /* laid out for the terminal of each frame (a resize, a replay elsewhere) */
  scr_cols = term_columns ();
  fprintf (out, "\033[H\033[2J");
  if (view_record_active ())
    {
      /* top-left recording mark */
      fprintf (out, "%s%s%s ", clr (CLR_WAIT), M ("[REC]", "[녹화중]"), clr (CLR_OFF));
    }
  fprintf (out, "%s", clr (CLR_HEAD));
  fprintf (out, M ("cub_ctv - %s (pid %d)  %s  window %.1fs  (%dHz, samples=%d)",
	     "cub_ctv - %s (pid %d)  %s  구간 %.1fs  (%dHz, samples=%d)"),
	  db, pid, hhmmss, view.dt > 0 ? view.dt : interval, hz, view.st.samples);	/* the real window: rates use it */
  fprintf (out, "%s\n\n", clr (CLR_OFF));

  cs_figures_ranked (lo->count, dt, fig);

  /* every window counts for persistence; an idle one has no top CS */
  if (view.hist_known)
    {
      history_import (view.seq, view.hist_n, view.hist_back, view.hist_top);
    }
  history_push (view.seq, fig[0].severity >= 2 ? fig[0].ix : -1);
  if (fig[0].severity >= 2)
    {
      print_verdict (out, &fig[0], lo->count > 1 ? &fig[1] : NULL);
    }
  else
    {
      fprintf (out, "%s", clr (CLR_HEAD));
      fprintf (out, M ("[verdict] no critical section contention",
		 "[판정] 크리티컬 섹션 경합 없음"));
      fprintf (out, "%s\n", clr (CLR_OFF));
      if (view.wait_avail)
	{
	  int lk = view.wait_cnt[17], pg = view.wait_cnt[3], fb = view.wait_cnt[21];

	  if (lk + pg + fb > 0)
	    {
	      char t[200];

	      snprintf (t, sizeof (t), M ("outside the CS layer: lock waits %d (cubrid lockdb), page latch %d, free buffer %d",
					  "CS 밖에서 대기: 락 %d (cubrid lockdb), 페이지 래치 %d, 빈 버퍼 %d"), lk, pg, fb);
	      fprintf (out, "  %s", clr (CLR_OWNER));
	      wrap_out (out, t, 2, 4);
	      fprintf (out, "%s\n", clr (CLR_OFF));
	    }
	}
    }
  fprintf (out, "\n%s", clr (CLR_HEAD));
  fprintf (out, M ("[critical sections by waiting]", "[대기 발생 순위]"));
  fprintf (out, "%s\n", clr (CLR_OFF));
  /* the holders first: the "guards" column stays only when every row then
   * fits the screen, and a holder still too long is cut at its edge */
  for (i = 0; i < lo->count; i++)
    {
      const CS_FIG *f = &fig[i];
      int hs, holder, hcpid;
      char *who = whos[i];

      if (f->severity == 0)
	{
	  continue;
	}
      holder = holder_of (f->ix, &hs, &hcpid);
      if (holder != -2 && held_minor (f->ix, hs))
	{
	  holder = -2;		/* readers most of the time (see the verdict) */
	}
      if (holder == 0 && daemon_name (f->ix)[0])
	{
	  snprintf (who, sizeof (whos[i]), M ("engine %s", "엔진 %s"), daemon_name (f->ix));
	}
      else if (holder != -2)
	{
	  tran_text (holder, hcpid, TRAN_TEXT_LABEL, who, sizeof (whos[i]));
	}
      else
	{
	  /* occupied without a writer: readers (no owner is recorded) */
	  snprintf (who, sizeof (whos[i]), "%s", view.st.occ[f->ix] > 0
		    ? M ("readers", "reader 다수") : "-");
	}
      /* number, name, guards (25), figures, two spaces: 82 columns */
      guards = guards && (scr_cols == 0 || 82 + str_cols (who) <= scr_cols);
    }
  fprintf (out, "%s", clr (CLR_HEAD));
  dw_pad (d1, sizeof (d1), M ("guards", "보호 대상"), 24);
  {
    const char *avg = M ("avg ms", "평균ms");
    int gap = 7 - str_cols (avg);

    fprintf (out, "   %-23s %s%s%5s %6s %7s %*s%s  %s", "name", guards ? d1 : "",
	     guards ? " " : "", "occ%", "wait%", "wait/s", gap > 0 ? gap : 0, "", avg,
	     M ("holder", "쥔 쪽"));
  }
  fprintf (out, "%s\n", clr (CLR_OFF));
  for (i = 0; i < lo->count; i++)
    {
      const CS_FIG *f = &fig[i];
      const CS_ROW *c = &view.rows[f->ix];
      char avg[16], cutb[96];
      const char *who = whos[i];
      int room = scr_cols - (guards ? 82 : 57);

      if (f->severity == 0)
	{
	  idle++;
	  continue;
	}
      dw_pad (d1, sizeof (d1), cs_desc (c->name), 24);
      if (f->avg_ms > 0)
	{
	  snprintf (avg, sizeof (avg), "%7.3f", f->avg_ms);
	}
      else
	{
	  snprintf (avg, sizeof (avg), "%7s", "-");
	}
      if (scr_cols > 0 && str_cols (who) > room)
	{
	  dw_pad (cutb, sizeof (cutb), who, room > 0 ? room : 0);
	  who = cutb;
	}
      fprintf (out, "%s%2d %-23s %s%s%5.0f %6.0f %7.0f %s  %s%s\n",
	       f->severity >= 3 ? clr (CLR_WAIT) : f->severity == 2
	       ? clr (CLR_OWNER) : "", ++n, c->name, guards ? d1 : "", guards ? " " : "",
	       f->occ_pct, f->wait_pct, f->wait_s, avg, who, clr (CLR_OFF));
    }
  if (idle > 0)
    {
      fprintf (out, "%s", clr (CLR_MUTED));
      fprintf (out, M ("   %d others: idle", "   나머지 %d개: 활동 없음"), idle);
      fprintf (out, "%s\n", clr (CLR_OFF));
    }
  print_thread_waits (out, 1);
  fprintf (out, "\n");
  /* the footer: a blank line and its text, which may wrap */
  footer = live_footer != NULL ? live_footer : M ("[q] quit", "[q] 종료");
  frows = 2 + wrap_out (NULL, footer, 0, 0);
  if (mem != NULL)
    {
      size_t k, s;

      /* rows - 1 screen rows fit without scrolling; the footer takes its
       * rows and the sessions at least 1: a taller top is cut, marked.
       * Rows are counted on the screen: a line wider than it wraps */
      int keep = rows > 0 ? rows - 1 - frows - (view.tran_avail ? 1 : 0) - 1 : -1;
      size_t cut;

      if (rows > 0 && keep < 1)
	{
	  keep = 1;		/* a tiny terminal: the title and the footer */
	  tiny = 1;
	}

      fclose (mem);		/* sets top_buf / top_len */
      cut = top_len;
      for (k = s = 0; k < top_len; k++)
	{
	  if (top_buf[k] == '\n')
	    {
	      int r = line_rows (vis_cols (top_buf + s, k - s));

	      if (keep > 0 && top + r > keep && s > 0)
		{
		  cut = s;	/* this line no longer fits: cut before it */
		  break;
		}
	      top += r;
	      s = k + 1;
	    }
	}
      fwrite (top_buf, 1, cut, stdout);
      if (cut < top_len && !tiny)
	{
	  printf ("%s%s%s\n", clr (CLR_MUTED),
		  M ("  ... (terminal too short: enlarge it for the rest)",
		     "  ... (터미널이 작음: 늘리면 나머지 표시)"), clr (CLR_OFF));
	  top++;
	}
      free (top_buf);
    }
  if (view.tran_avail && !tiny)
    {
      /* below the panel: its two header lines, "more" and "idle" lines,
       * a blank line and the footer, and no newline into the last row;
       * with no room for one row the block is its count line alone */
      /* the top block not measured (no memory stream): the count line */
      int panel = mem == NULL && rows > 0 ? -1
	: rows > 0 ? rows - 1 - top - frows - 4 : LIVE_TRANS_ROWS;

      panel = panel > LIVE_TRANS_ROWS ? LIVE_TRANS_ROWS : panel < 1 ? -1 : panel;
      print_sessions (panel);
    }
  printf ("%s\n", clr (CLR_MUTED));
  wrap_out (stdout, footer, 0, 0);
  printf ("%s\n", clr (CLR_OFF));
  fflush (stdout);
}
