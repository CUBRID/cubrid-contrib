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
 * record - the CTVREC line format: writing frames, reading them back,
 * indexing for seeks
 */

#include "ctv.h"

/* ------------------------------------------------------ record (write) */

/* line format: one record per line, space separated; strings are
 * %XX-escaped, "~" stands for the empty string */
static FILE *rec_out = NULL;
static int rec_frames = 0;	/* frames written to rec_out */

static void
rec_str (FILE * f, const char *s)
{
  fputc (' ', f);
  if (*s == '\0')
    {
      fputc ('~', f);
      return;
    }
  for (; *s != '\0'; s++)
    {
      unsigned char c = (unsigned char) *s;

      if (c <= 0x20 || c >= 0x7f || c == '%' || c == '~')
	{
	  fprintf (f, "%%%02X", c);
	}
      else
	{
	  fputc (c, f);
	}
    }
}

/* a file holding client identities: 0600, never through a symlink;
 * excl = refuse an existing name (generated names) */
FILE *
private_create (const char *path, int excl)
{
  int fd = open (path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK
		 | (excl ? O_EXCL : 0), 0600);
  struct stat st;
  FILE *f;

  /* an existing name must be our own single-link regular file; it is
   * re-moded and only then truncated */
  if (fd < 0 || fstat (fd, &st) != 0 || !S_ISREG (st.st_mode)
      || st.st_nlink != 1 || st.st_uid != geteuid ()
      || fchmod (fd, 0600) != 0 || ftruncate (fd, 0) != 0
      || fcntl (fd, F_SETFL, 0) != 0)
    {
      if (fd >= 0)
	{
	  close (fd);
	}
      return NULL;
    }
  f = fdopen (fd, "w");
  if (f == NULL)
    {
      close (fd);
    }
  return f;
}

/* identities as last written, per tran index: a hash of the I line (0 =
 * not written) and the frame that last showed it */
static uint64_t *wr_hash = NULL;
static int *wr_seen = NULL, wr_cap = 0;


static uint64_t
ident_hash (const VIEW_IDENT * v)
{
  uint64_t h = fnv (FNV_SEED, &v->trid, sizeof (v->trid));
  const char *s[6] = { v->id.info, v->id.user, v->id.program, v->id.login,
    v->id.host, v->id.ip
  };
  int k;

  h = fnv (h, &v->id.cpid, sizeof (v->id.cpid));
  for (k = 0; k < 6; k++)
    {
      h = fnv (h, s[k], strlen (s[k]) + 1);
    }
  return h | 1;			/* never 0 (= not written) */
}

static int
wr_grow (int idx)
{
  if (idx < wr_cap)
    {
      return 0;
    }
  {
    int cap = idx + 64;
    uint64_t *h = realloc (wr_hash, (size_t) cap * sizeof (uint64_t));
    int *s;

    if (h == NULL)
      {
	return -1;
      }
    wr_hash = h;
    if ((s = realloc (wr_seen, (size_t) cap * sizeof (int))) == NULL)
      {
	return -1;
      }
    wr_seen = s;
    memset (wr_hash + wr_cap, 0, (size_t) (cap - wr_cap) * sizeof (uint64_t));
    memset (wr_seen + wr_cap, 0, (size_t) (cap - wr_cap) * sizeof (int));
    wr_cap = cap;
  }
  return 0;
}

/* I: an identity of the transaction table; J: a client seen in the window
 * that has left its slot (same tokens) */
static void
rec_ident_line (FILE * f, char tag, const VIEW_IDENT * v)
{
  fprintf (f, "%c %d %d %d", tag, v->idx, v->trid, v->id.cpid);
  rec_str (f, v->id.info);
  rec_str (f, v->id.user);
  rec_str (f, v->id.program);
  rec_str (f, v->id.login);
  rec_str (f, v->id.host);
  rec_str (f, v->id.ip);
  fputc ('\n', f);
}

/* the frame's identities: all of them on a keyframe ("K 1"), else only
 * the ones that changed ("I") or went away ("X") since the last frame */
static void
rec_idents (FILE * f)
{
  int i, key = (rec_frames - 1) % REC_KEYFRAME == 0;

  for (i = 0; !key && i < view.nident; i++)
    {
      key = wr_grow (view.ident[i].idx) != 0;	/* no memory: write all */
    }
  fprintf (f, "K %d\n", key);
  if (key && wr_cap > 0)
    {
      memset (wr_hash, 0, (size_t) wr_cap * sizeof (uint64_t));
    }
  for (i = 0; i < view.nident; i++)
    {
      const VIEW_IDENT *v = &view.ident[i];
      uint64_t h = ident_hash (v);

      if (key || wr_hash[v->idx] != h)
	{
	  rec_ident_line (f, 'I', v);
	}
      if (wr_grow (v->idx) == 0)
	{
	  wr_hash[v->idx] = h;
	  wr_seen[v->idx] = rec_frames;
	}
    }
  for (i = 0; !key && i < wr_cap; i++)
    {
      if (wr_hash[i] != 0 && wr_seen[i] != rec_frames)
	{
	  fprintf (f, "X %d\n", i);
	  wr_hash[i] = 0;
	}
    }
}

int
view_record_open (const char *path, const VIEW_HDR * h, int excl)
{
  rec_out = private_create (path, excl);
  rec_frames = 0;
  if (rec_out == NULL)
    {
      return -1;
    }
  fprintf (rec_out, "%s\nH %d %d %d %.17g %d %u %d %d %d", REC_MAGIC, h->pid,
	   h->live, h->hz, h->interval, h->thr_count, h->degraded,
	   h->lo.stride, h->lo.count, h->lo.off_stats);
  rec_str (rec_out, h->db);
  rec_str (rec_out, h->lib);
  rec_str (rec_out, h->tran_refused);
  fputc ('\n', rec_out);
  fflush (rec_out);
  return 0;
}

/* 0 while the record is intact; -1 once a write failed */
int
view_record_ok (void)
{
  return rec_out == NULL || !ferror (rec_out) ? 0 : -1;	/* frames flush */
}

int
view_record_active (void)
{
  return rec_out != NULL;
}

int
view_record_close (void)
{
  int rc = 0;

  if (rec_out != NULL)
    {
      rc = ferror (rec_out) ? -1 : 0;
      rc = fclose (rec_out) != 0 ? -1 : rc;
      rec_out = NULL;
    }
  return rc;
}

/* frames written since the record was opened */
int
view_record_frames (void)
{
  return rec_frames;
}

/* one frame; flushed so an interrupted live recording keeps whole frames */
void
view_record_frame (void)
{
  FILE *f = rec_out;
  int i, k;

  if (f == NULL)
    {
      return;
    }
  rec_frames++;
  fprintf (f, "F %ld %d %d %.17g %d %d %d %d %d %d\n", view.ts, view.count,
	   view.st.samples, view.dt < REC_DT_MAX ? view.dt : REC_DT_MAX,
	   view.tran_avail, view.cid_locked,
	   view.has_trid, view.ntran, view.pool_state, view.pool_truncated);
  fprintf (f, "B %u\n", view.degraded);
  if (view.hist_known)
    {
      fprintf (f, "V %d", view.hist_n);
      for (i = 0; i < view.hist_n; i++)
	{
	  fprintf (f, " %d %d", view.hist_back[i], view.hist_top[i]);
	}
      fputc ('\n', f);
    }
  for (i = 0; i < view.count; i++)
    {
      const CS_ROW *r = &view.rows[i];

      fprintf (f, "C %d %d %u %u %d %llx %llx %d %u %u %u %.17g %.17g "
	       "%d %d %d %llx %llx %u %u", i, r->rwlock, r->wr, r->ww,
	       r->tran, (unsigned long long) r->owner,
	       (unsigned long long) r->wq, r->has_stats, r->nenter, r->nwait,
	       r->nreenter, r->total_ms, r->max_ms, view.st.occ[i], view.st.wai[i],
	       view.st.seen_tran[i], (unsigned long long) view.st.seen_owner[i],
	       (unsigned long long) view.st.seen_wq[i], view.prev[i].nenter,
	       view.prev[i].nwait);
      rec_str (f, r->name);
      fprintf (f, " %.17g %d", view.prev[i].total_ms, view.st.seen_cpid[i]);
      fputc ('\n', f);
    }
  rec_idents (f);
  for (i = 0; i < view.ngone; i++)
    {
      rec_ident_line (f, 'J', &view.gone[i]);
    }
  for (i = 0; i < view.ntid; i++)
    {
      fprintf (f, "T %llx %d\n", (unsigned long long) view.tid[i],
	       view.tid_slot[i]);
    }
  for (i = 0; i < view.nchain; i++)
    {
      fprintf (f, "Q %llx %d", (unsigned long long) view.chain[i].head,
	       view.chain[i].n);
      for (k = 0; k < view.chain[i].n; k++)
	{
	  fprintf (f, " %d", view.chain[i].slot[k]);
	}
      fputc ('\n', f);
    }
  for (i = 0; i < view.count; i++)
    {
      if (view.st.seen_n[i] < 0)
	{
	  continue;
	}
      fprintf (f, "P %d %d", i, view.st.seen_n[i]);
      for (k = 0; k < view.st.seen_n[i]; k++)
	{
	  fprintf (f, " %d", view.st.seen_slot[i][k]);
	}
      fputc ('\n', f);
    }
  for (i = 0; i < view.nslot; i++)
    {
      fprintf (f, "S %d %d %d\n", view.slot[i], view.slot_tran[i], view.slot_cpid[i]);
    }
  if (view.st.act_dropped > 0)
    {
      fprintf (f, "A %d\n", view.st.act_dropped);
    }
  for (i = 0; i < view.st.nact; i++)
    {
      fprintf (f, "W %d %d %d %d %d %d\n", view.st.act[i].cs, view.st.act[i].kind,
	       view.st.act[i].tran, view.st.act[i].thr, view.st.act[i].samples,
	       view.st.act[i].cpid);
    }
  for (i = 0; i < view.nos; i++)
    {
      const OS_THR *o = &view.os[i];

      fprintf (f, "O %d %d %d %d %d %d %d %ld %ld", o->slot, o->cs, o->tid,
	       o->samples, o->run, o->dsk, o->oth, o->ticks0, o->ticks1);
      rec_str (f, o->comm);
      fprintf (f, " %d\n", o->span_ms);
    }
  if (view.disk_avail)
    {
      fprintf (f, "D %.17g %.17g", view.disk_await_ms, view.disk_util_pct);
      rec_str (f, view.disk_dev);
      fputc ('\n', f);
    }
  if (view.wait_avail)
    {
      fprintf (f, "R %d %d", view.wait_live, THR_RESUME_MAX);
      for (i = 0; i < THR_RESUME_MAX; i++)
	{
	  fprintf (f, " %d", view.wait_cnt[i]);
	}
      fputc ('\n', f);
    }
  for (i = 0; i < view.npool && view.pool_state == 1; i++)
    {
      const POOL_GROUP *g = &view.pool[i];

      fprintf (f, "G %d %d %llu %.17g", g->type, g->cnt,
	       (unsigned long long) g->nenter, g->max_ms);
      rec_str (f, g->name);
      fputc ('\n', f);
    }
  fputs ("E\n", f);
  fflush (f);
}

/* ------------------------------------------------------- replay (read) */

static FILE *rec_in = NULL;
static const char *rec_path = "";
static long rec_line = 0;
static int rec_count = 0;	/* CS rows per frame, from the header */
static long *rec_off = NULL;	/* file offset / line number of every complete frame */
static long *rec_lno = NULL;
static unsigned char *rec_kf = NULL;	/* frame carries every identity */

/* identities carried from frame to frame (a "K 0" frame only changes
 * them), and the frame's removals */
static VIEW_IDENT *car = NULL;
static int ncar = 0, car_cap = 0;
static int *rm = NULL, nrm = 0, rm_cap = 0;

/* end of a frame: a delta frame becomes carried identities + its I lines
 * - its X lines; the result is carried to the next frame */
static void
idents_settle (int keyframe)
{
  int i = 0, j = 0, nupd = view.nident;
  VIEW_IDENT *upd;

  if (!keyframe)
    {
      upd = malloc ((size_t) (nupd > 0 ? nupd : 1) * sizeof (VIEW_IDENT));
      if (upd == NULL)
	{
	  die ("out of memory", "메모리 부족", NULL);
	}
      memcpy (upd, view.ident, (size_t) nupd * sizeof (VIEW_IDENT));
      qsort (rm, (size_t) nrm, sizeof (int), cmp_int);
      view.nident = 0;
      while (i < ncar || j < nupd)
	{
	  const VIEW_IDENT *v;

	  if (j >= nupd || (i < ncar && car[i].idx < upd[j].idx))
	    {
	      v = &car[i++];
	      if (nrm > 0 && bsearch (&v->idx, rm, (size_t) nrm, sizeof (int), cmp_int))
		{
		  continue;
		}
	    }
	  else
	    {
	      i += i < ncar && car[i].idx == upd[j].idx;
	      v = &upd[j++];
	    }
	  view_add_ident (v->idx, v->trid, &v->id);
	}
      free (upd);
    }
  if (view.nident > car_cap)
    {
      VIEW_IDENT *p = realloc (car, (size_t) view.nident * sizeof (VIEW_IDENT));

      if (p == NULL)
	{
	  die ("out of memory", "메모리 부족", NULL);
	}
      car = p;
      car_cap = view.nident;
    }
  memcpy (car, view.ident, (size_t) view.nident * sizeof (VIEW_IDENT));
  ncar = view.nident;
}
static int rec_nframes = 0;
/* an interrupted recording ends in a partial frame: said once per record */
static void
rec_tail_warn (void)
{
  static int said = 0;

  if (!said)
    {
      said = 1;
      warn ("last frame of the record is incomplete (recording interrupted) "
	    "- ignored", "기록의 마지막 프레임이 불완전합니다(기록 중단) - 무시");
    }
}

static long rec_seq = 0;	/* ordinal of the next frame read */
static int rec_live = 0;	/* header: live record (windows) or snapshot */

static void
rec_bad (void)
{
  char where[4200];

  snprintf (where, sizeof (where), "%s:%ld", rec_path, rec_line);
  die ("malformed record file at %s", "기록 파일 형식 오류: %s", where);
}

/* a real number token: nan or inf is a damaged record (JSON has neither) */
static double
rec_real (const char *tok)
{
  double v = strtod (tok, NULL);

  if (!isfinite (v))
    {
      rec_bad ();
    }
  return v;
}


static void
rec_unescape (const char *in, char *out, size_t n)
{
  size_t o = 0;

  if (strcmp (in, "~") == 0)
    {
      out[0] = '\0';
      return;
    }
  for (; *in != '\0' && o + 1 < n; in++)
    {
      unsigned int c;

      if (*in == '%' && isxdigit ((unsigned char) in[1])
	  && isxdigit ((unsigned char) in[2]) && sscanf (in + 1, "%2x", &c) == 1)
	{
	  out[o++] = (char) c;
	  in += 2;
	}
      else
	{
	  out[o++] = *in;
	}
    }
  out[o] = '\0';
  text_sanitize (out);	/* a record is data from anywhere */
}

/* split a line into space-separated tokens; returns the count */
static int
rec_tokens (char *line, char **tok, int max)
{
  int n = 0;
  char *save = NULL, *t;

  line[strcspn (line, "\n")] = '\0';
  for (t = strtok_r (line, " ", &save); t != NULL && n < max;
       t = strtok_r (NULL, " ", &save))
    {
      tok[n++] = t;
    }
  return n;
}

void
view_replay_open (const char *path, VIEW_HDR * h)
{
  char line[16384], *tok[16];

  rec_path = path;
  rec_in = fopen (path, "r");
  if (rec_in == NULL)
    {
      die ("cannot open record %s", "기록 파일 %s 열기 실패", path);
    }
  rec_line = 1;
  if (fgets (line, sizeof (line), rec_in) == NULL)
    {
      rec_bad ();
    }
  line[strcspn (line, "\r\n")] = '\0';
  if (strcmp (line, REC_MAGIC) != 0)
    {
      if (strncmp (line, "CTVREC ", 7) == 0)
	{
	  die ("%s is a cub_ctv record of another format version (this build "
	       "reads " REC_MAGIC ")", "%s 는 다른 형식 버전의 cub_ctv 기록입니다 "
	       "(이 빌드는 " REC_MAGIC " 만 읽음)", path);
	}
      if (line[0] == '{' && strstr (line, DETAIL_MAGIC) != NULL)
	{
	  die ("%s is a detail dump (--dump): it holds everything cub_ctv read "
	       "but is not a replay file - record with --record",
	       "%s 는 디테일 덤프(--dump)입니다: 판독한 정보를 모두 담지만 "
	       "재생용이 아닙니다 - 재생하려면 --record 로 기록하십시오", path);
	}
      die ("%s is not a cub_ctv record (--record)",
	   "%s 는 cub_ctv 기록 파일(--record)이 아닙니다", path);
    }
  rec_line = 2;
  memset (h, 0, sizeof (*h));
  if (fgets (line, sizeof (line), rec_in) == NULL
      || rec_tokens (line, tok, 16) != 13 || strcmp (tok[0], "H") != 0)
    {
      rec_bad ();
    }
  h->pid = atoi (tok[1]);
  h->live = atoi (tok[2]);
  h->hz = atoi (tok[3]);
  h->interval = rec_real (tok[4]);
  h->thr_count = atoi (tok[5]);
  h->degraded = (unsigned) strtoul (tok[6], NULL, 10);
  h->lo.stride = atoi (tok[7]);
  h->lo.count = atoi (tok[8]);
  h->lo.off_stats = atoi (tok[9]);
  rec_unescape (tok[10], h->db, sizeof (h->db));
  rec_unescape (tok[11], h->lib, sizeof (h->lib));
  rec_unescape (tok[12], h->tran_refused, sizeof (h->tran_refused));
  if (h->lo.count < 1 || h->lo.count > MAX_CS
      || (h->live && !(h->interval >= INTERVAL_MIN && h->interval <= INTERVAL_MAX)))
    {
      rec_bad ();
    }
  rec_count = h->lo.count;
  rec_live = h->live;
}

static unsigned long long
hex (const char *s)
{
  return strtoull (s, NULL, 16);
}

/* one frame being read: rows seen, identity keyframe flag, frame state */
typedef struct
{
  unsigned char seen[MAX_CS];
  int nrows, keyframe, in_frame, done;
} REC_RD;

/* the role of a line in the frame structure (what the seek index needs) */
#define REC_FRAME_START 1
#define REC_FRAME_KEY 2
#define REC_FRAME_END 3

/* one line kind: its tag, token count bounds, frame role and reader */
typedef struct
{
  const char *tag;
  int min_tok, max_tok, mark;
  void (*read) (char **tok, int n, REC_RD * rd);
} REC_LINE;

/* "K 0" or "K 1" exactly; anything else is damage */
static int
rec_key_value (char **tok)
{
  if (strcmp (tok[1], "0") != 0 && strcmp (tok[1], "1") != 0)
    {
      rec_bad ();
    }
  return tok[1][0] == '1';
}

/* F: frame start: time, CS count, samples, window, table flags, pool */
static void
rd_F (char **tok, int n, REC_RD * rd)
{
  int k;

  (void) n;

  if (rd->in_frame)
    {
      rec_bad ();       /* a frame started inside another: damaged */
    }
  /* frame numbers start past the history depth, so a recorded V line
   * (up to 9 windows back) never points before frame 0 */
  view.seq = VERDICT_HISTORY + rec_seq++;
  view.ts = atol (tok[1]);
  view.count = atoi (tok[2]);
  view.st.samples = atoi (tok[3]);
  view.dt = rec_real (tok[4]);
  view.tran_avail = atoi (tok[5]);
  view.cid_locked = atoi (tok[6]);
  view.has_trid = atoi (tok[7]);
  view.ntran = atoi (tok[8]);
  view.pool_state = atoi (tok[9]);
  view.pool_truncated = atoi (tok[10]);
  /* a live window must carry samples and a positive, finite length
   * (the figures divide by both, the replay sleeps for it) */
  if (view.count != rec_count
      || view.st.samples < 0 || view.st.samples > REC_SAMPLES_MAX
      || (rec_live && (view.st.samples < 1 || !(view.dt > 0.0)
		       || !(view.dt <= REC_DT_MAX))))
    {
      rec_bad ();
    }
  view.nident = view.ntid = view.nchain = view.nslot = view.npool = 0;
  view.st.nact = view.nos = view.disk_avail = view.wait_avail = 0;
  view.st.act_dropped = 0;
  rd->keyframe = 1;             /* a frame without "K" lists every identity */
  nrm = 0;
  view.hist_known = view.hist_n = 0;
  memset (view.rows, 0, sizeof (view.rows));
  memset (view.st.occ, 0, sizeof (view.st.occ));
  memset (view.st.wai, 0, sizeof (view.st.wai));
  memset (view.st.seen_tran, 0, sizeof (view.st.seen_tran));
  memset (view.st.seen_owner, 0, sizeof (view.st.seen_owner));
  memset (view.st.seen_wq, 0, sizeof (view.st.seen_wq));
  for (k = 0; k < MAX_CS; k++)
    {
      view.st.seen_n[k] = -1;      /* no P line: no queue seen */
      view.st.seen_cpid[k] = -1;
    }
  view.ngone = 0;
  memset (rd->seen, 0, sizeof (rd->seen));
  rd->nrows = 0;
  rd->in_frame = 1;
}

/* C: one CS row and its window counts */
static void
rd_C (char **tok, int n, REC_RD * rd)
{
  uint32_t pn, pw;
  int i = atoi (tok[1]);
  CS_ROW *r;

  (void) n;

  if (i < 0 || i >= view.count || rd->seen[i])
    {
      rec_bad ();
    }
  r = &view.rows[i];
  r->rwlock = atoi (tok[2]);
  r->wr = (uint32_t) strtoul (tok[3], NULL, 10);
  r->ww = (uint32_t) strtoul (tok[4], NULL, 10);
  r->tran = atoi (tok[5]);
  r->owner = hex (tok[6]);
  r->wq = hex (tok[7]);
  r->has_stats = atoi (tok[8]);
  r->nenter = (uint32_t) strtoul (tok[9], NULL, 10);
  r->nwait = (uint32_t) strtoul (tok[10], NULL, 10);
  r->nreenter = (uint32_t) strtoul (tok[11], NULL, 10);
  r->total_ms = rec_real (tok[12]);
  r->max_ms = rec_real (tok[13]);
  view.st.occ[i] = atoi (tok[14]);
  view.st.wai[i] = atoi (tok[15]);
  view.st.seen_tran[i] = atoi (tok[16]);
  view.st.seen_owner[i] = hex (tok[17]);
  view.st.seen_wq[i] = hex (tok[18]);
  pn = (uint32_t) strtoul (tok[19], NULL, 10);
  pw = (uint32_t) strtoul (tok[20], NULL, 10);
  rec_unescape (tok[21], r->name, sizeof (r->name));
  view.prev[i] = *r;    /* as view_capture builds it */
  view.prev[i].nenter = pn;
  view.prev[i].nwait = pw;
  view.prev[i].total_ms = rec_real (tok[22]);
  view.st.seen_cpid[i] = atoi (tok[23]);
  rd->seen[i] = 1;
  rd->nrows++;
}

/* I: one identity */
static void
rd_I (char **tok, int n, REC_RD * rd)
{
  TRAN_IDENT id;

  (void) n;
  (void) rd;

  if (view.nident > 0 && atoi (tok[1]) <= view.ident[view.nident - 1].idx)
    {
      rec_bad ();
    }
  memset (&id, 0, sizeof (id));
  id.cpid = atoi (tok[3]);
  rec_unescape (tok[4], id.info, sizeof (id.info));
  rec_unescape (tok[5], id.user, sizeof (id.user));
  rec_unescape (tok[6], id.program, sizeof (id.program));
  rec_unescape (tok[7], id.login, sizeof (id.login));
  rec_unescape (tok[8], id.host, sizeof (id.host));
  rec_unescape (tok[9], id.ip, sizeof (id.ip));
  view_add_ident (atoi (tok[1]), atoi (tok[2]), &id);
}

/* T: owner thread id -> slot */
static void
rd_T (char **tok, int n, REC_RD * rd)
{
  (void) n;
  (void) rd;

  if (view.ntid < VIEW_MAX_TIDS)        /* beyond capacity: skipped */
    {
      view.tid[view.ntid] = hex (tok[1]);
      view.tid_slot[view.ntid++] = atoi (tok[2]);
    }
}

/* Q: a queue head and its slots */
static void
rd_Q (char **tok, int n, REC_RD * rd)
{
  int cn = atoi (tok[2]), q;

  (void) rd;

  if (cn < 0 || n != 3 + cn)
    {
      rec_bad ();
    }
  if (cn <= THR_WAITERS_MAX && view.nchain < VIEW_MAX_CHAINS)
    {
      VIEW_CHAIN *c = &view.chain[view.nchain++];

      c->head = hex (tok[1]);
      c->n = cn;
      for (q = 0; q < cn; q++)
	{
	  c->slot[q] = atoi (tok[3 + q]);
	}
    }
}

/* P: the window's first queue of a CS */
static void
rd_P (char **tok, int n, REC_RD * rd)
{
  int cs = atoi (tok[1]), cn = atoi (tok[2]), k2;

  (void) rd;

  if (cs < 0 || cs >= view.count || cn < 0 || cn > THR_WAITERS_MAX
      || n != 3 + cn)
    {
      rec_bad ();
    }
  view.st.seen_n[cs] = cn;
  for (k2 = 0; k2 < cn; k2++)
    {
      view.st.seen_slot[cs][k2] = atoi (tok[3 + k2]);
    }
}

/* J: a client seen in the window that has left its slot */
static void
rd_J (char **tok, int n, REC_RD * rd)
{
  TRAN_IDENT id;

  (void) n;
  (void) rd;

  memset (&id, 0, sizeof (id));
  id.cpid = atoi (tok[3]);
  rec_unescape (tok[4], id.info, sizeof (id.info));
  rec_unescape (tok[5], id.user, sizeof (id.user));
  rec_unescape (tok[6], id.program, sizeof (id.program));
  rec_unescape (tok[7], id.login, sizeof (id.login));
  rec_unescape (tok[8], id.host, sizeof (id.host));
  rec_unescape (tok[9], id.ip, sizeof (id.ip));
  view_add_gone (atoi (tok[1]), atoi (tok[2]), &id);
}

/* W: one activity sighting */
static void
rd_W (char **tok, int n, REC_RD * rd)
{
  ACT a;

  (void) n;
  (void) rd;

  a.cs = atoi (tok[1]);
  a.kind = atoi (tok[2]);
  a.tran = atoi (tok[3]);
  a.thr = atoi (tok[4]);
  a.samples = atoi (tok[5]);
  a.cpid = atoi (tok[6]);
  if (a.samples < 0 || a.samples > view.st.samples)
    {
      rec_bad ();       /* a sighting count beyond the window's samples */
    }
  if (a.cs < 0 || a.cs >= view.count || (a.kind != ACT_HELD && a.kind != ACT_WAIT))
    {
      rec_bad ();
    }
  if (view.st.nact < ACT_MAX)
    {
      view.st.act[view.st.nact++] = a;
    }
}

/* O: one thread's OS state under one CS (cs -1: its name only) */
static void
rd_O (char **tok, int n, REC_RD * rd)
{
  OS_THR tmp, *o = &tmp;

  (void) n;
  (void) rd;

  memset (o, 0, sizeof (*o));
  o->span_ms = atoi (tok[11]);

  o->slot = atoi (tok[1]);
  o->cs = atoi (tok[2]);
  o->tid = atoi (tok[3]);
  o->samples = atoi (tok[4]);
  o->run = atoi (tok[5]);
  o->dsk = atoi (tok[6]);
  o->oth = atoi (tok[7]);
  o->ticks0 = atol (tok[8]);
  o->ticks1 = atol (tok[9]);
  rec_unescape (tok[10], o->comm, sizeof (o->comm));
  if (o->cs < -1 || o->cs >= view.count)
    {
      rec_bad ();
    }
  if (view.nos < OS_MAX)
    {
      view.os[view.nos++] = tmp;
    }
}

/* V: verdict history */
static void
rd_V (char **tok, int n, REC_RD * rd)
{
  int k2;

  (void) rd;

  if (n != 2 + 2 * atoi (tok[1]) || atoi (tok[1]) >= VERDICT_HISTORY)
    {
      rec_bad ();
    }
  view.hist_known = 1;
  view.hist_n = atoi (tok[1]);
  for (k2 = 0; k2 < view.hist_n; k2++)
    {
      view.hist_back[k2] = atoi (tok[2 + 2 * k2]);
      view.hist_top[k2] = atoi (tok[3 + 2 * k2]);
      if (view.hist_back[k2] < 1 || view.hist_back[k2] >= VERDICT_HISTORY
	  || view.hist_top[k2] < -1 || view.hist_top[k2] >= view.count)
	{
	  rec_bad ();
	}
    }
}

/* K: identity keyframe flag */
static void
rd_K (char **tok, int n, REC_RD * rd)
{
  (void) n;

  rd->keyframe = rec_key_value (tok);
}

/* X: an identity gone */
static void
rd_X (char **tok, int n, REC_RD * rd)
{
  (void) n;
  (void) rd;

  if (nrm == rm_cap)
    {
      int *p2 = realloc (rm, (size_t) (rm_cap ? rm_cap * 2 : 64) * sizeof (int));

      if (p2 == NULL)
	{
	  die ("out of memory", "메모리 부족", NULL);
	}
      rm = p2;
      rm_cap = rm_cap ? rm_cap * 2 : 64;
    }
  rm[nrm++] = atoi (tok[1]);
}

/* A: sightings not kept */
static void
rd_A (char **tok, int n, REC_RD * rd)
{
  (void) n;
  (void) rd;

  view.st.act_dropped = atoi (tok[1]);
}

/* B: degraded bits */
static void
rd_B (char **tok, int n, REC_RD * rd)
{
  (void) n;
  (void) rd;

  view.degraded = (unsigned) strtoul (tok[1], NULL, 10);
}

/* D: log device figures */
static void
rd_D (char **tok, int n, REC_RD * rd)
{
  (void) n;
  (void) rd;

  view.disk_avail = 1;
  view.disk_await_ms = rec_real (tok[1]);
  view.disk_util_pct = rec_real (tok[2]);
  rec_unescape (tok[3], view.disk_dev, sizeof (view.disk_dev));
}

/* R: thread wait reasons */
static void
rd_R (char **tok, int n, REC_RD * rd)
{
  int k;

  (void) n;
  (void) rd;

  if (atoi (tok[2]) != THR_RESUME_MAX)
    {
      rec_bad ();
    }
  view.wait_avail = 1;
  view.wait_live = atoi (tok[1]);
  for (k = 0; k < THR_RESUME_MAX; k++)
    {
      view.wait_cnt[k] = atoi (tok[3 + k]);
    }
}

/* S: slot -> tran */
static void
rd_S (char **tok, int n, REC_RD * rd)
{
  (void) n;
  (void) rd;

  if (view.nslot < VIEW_MAX_SLOTS)
    {
      view.slot[view.nslot] = atoi (tok[1]);
      view.slot_tran[view.nslot] = atoi (tok[2]);
      view.slot_cpid[view.nslot++] = atoi (tok[3]);
    }
}

/* G: one pool group */
static void
rd_G (char **tok, int n, REC_RD * rd)
{
  POOL_GROUP tmp, *g = &tmp;

  (void) n;
  (void) rd;

  g->type = atoi (tok[1]);
  g->cnt = atoi (tok[2]);
  g->nenter = strtoull (tok[3], NULL, 10);
  g->max_ms = rec_real (tok[4]);
  rec_unescape (tok[5], g->name, sizeof (g->name));
  if (view.npool < POOL_MAX_GROUPS)
    {
      view.pool[view.npool++] = tmp;
    }
}

/* E: frame end */
static void
rd_E (char **tok, int n, REC_RD * rd)
{
  (void) tok;
  (void) n;

  /* every CS row must have arrived */
  if (rd->nrows != view.count)
    {
      rec_bad ();
    }
  idents_settle (rd->keyframe);
  rd->done = 1;
}

static const REC_LINE rec_lines[] = {
  {"F", 11, 11, REC_FRAME_START, rd_F},
  {"C", 24, 24, 0, rd_C},
  {"I", 10, 10, 0, rd_I},
  {"J", 10, 10, 0, rd_J},
  {"T", 3, 3, 0, rd_T},
  {"Q", 3, REC_TOKENS_MAX, 0, rd_Q},
  {"P", 3, REC_TOKENS_MAX, 0, rd_P},
  {"W", 7, 7, 0, rd_W},
  {"O", 12, 12, 0, rd_O},
  {"V", 2, 2 + 2 * (VERDICT_HISTORY - 1), 0, rd_V},
  {"K", 2, 2, REC_FRAME_KEY, rd_K},
  {"X", 2, 2, 0, rd_X},
  {"A", 2, 2, 0, rd_A},
  {"B", 2, 2, 0, rd_B},
  {"D", 4, 4, 0, rd_D},
  {"R", 3 + THR_RESUME_MAX, 3 + THR_RESUME_MAX, 0, rd_R},
  {"S", 4, 4, 0, rd_S},
  {"G", 6, 6, 0, rd_G},
  {"E", 1, 1, REC_FRAME_END, rd_E},
};


/* the spec of a tokenized line, or NULL (unknown tag, wrong token count) */
static const REC_LINE *
rec_line_of (char **tok, int n)
{
  size_t k;

  for (k = 0; k < sizeof (rec_lines) / sizeof (rec_lines[0]); k++)
    {
      if (strcmp (tok[0], rec_lines[k].tag) == 0)
	{
	  return n >= rec_lines[k].min_tok && n <= rec_lines[k].max_tok
	    ? &rec_lines[k] : NULL;
	}
    }
  return NULL;
}

/* load the next frame into 'view': 0 = loaded, 1 = no more frames */
int
view_replay_next (void)
{
  char line[16384], *tok[REC_TOKENS_MAX];
  REC_RD rd;
  int n;

  memset (&rd, 0, sizeof (rd));
  rd.keyframe = 1;
  while (fgets (line, sizeof (line), rec_in) != NULL)
    {
      const REC_LINE *spec;

      rec_line++;
      /* a line cut by an interrupted recording ends the record */
      if (strchr (line, '\n') == NULL && feof (rec_in))
	{
	  break;
	}
      n = rec_tokens (line, tok, REC_TOKENS_MAX);
      if (n == 0)
	{
	  continue;
	}
      spec = rec_line_of (tok, n);
      if (spec == NULL || (spec->mark != REC_FRAME_START && !rd.in_frame))
	{
	  rec_bad ();
	}
      spec->read (tok, n, &rd);
      if (rd.done)
	{
	  return 0;
	}
    }
  if (rd.in_frame)
    {
      rec_tail_warn ();
    }
  return 1;
}

/* the record can be read back and forth (a file, not a pipe) */
int
view_replay_seekable (void)
{
  return rec_in != NULL && ftell (rec_in) >= 0 && fseek (rec_in, 0, SEEK_CUR) == 0;
}

/* index every complete frame (F .. E) for random access; returns the count
 * and leaves the stream and its line number where they were */
int
view_replay_index (void)
{
  char line[16384], *tok[REC_TOKENS_MAX];
  long pos = ftell (rec_in), at, lno = rec_line, line0 = rec_line, f_off = -1, f_lno = 0;
  int cap = 0, f_kf = 1, nt;
  const REC_LINE *spec;

  rec_nframes = 0;
  for (;;)
    {
      at = ftell (rec_in);
      if (fgets (line, sizeof (line), rec_in) == NULL)
	{
	  break;
	}
      lno++;
      if (strchr (line, '\n') == NULL && feof (rec_in))
	{
	  break;
	}
      /* the same tokens and line table the frame reader uses */
      nt = rec_tokens (line, tok, REC_TOKENS_MAX);
      spec = nt > 0 ? rec_line_of (tok, nt) : NULL;
      if (spec != NULL && spec->mark == REC_FRAME_START)
	{
	  if (f_off >= 0)
	    {
	      rec_line = lno;
	      rec_bad ();	/* as the reader: a frame inside another */
	    }
	  f_off = at;
	  f_lno = lno;
	  f_kf = 1;
	}
      else if (spec != NULL && spec->mark == REC_FRAME_KEY)
	{
	  rec_line = lno;
	  f_kf = rec_key_value (tok);
	}
      else if (spec != NULL && spec->mark == REC_FRAME_END && f_off >= 0)
	{
	  if (rec_nframes == cap)
	    {
	      cap = cap ? cap * 2 : 256;
	      rec_off = realloc (rec_off, (size_t) cap * sizeof (long));
	      rec_lno = realloc (rec_lno, (size_t) cap * sizeof (long));
	      rec_kf = realloc (rec_kf, (size_t) cap);
	      if (rec_off == NULL || rec_lno == NULL || rec_kf == NULL)
		{
		  die ("out of memory", "메모리 부족", NULL);
		}
	    }
	  rec_off[rec_nframes] = f_off;
	  rec_lno[rec_nframes] = f_lno;
	  rec_kf[rec_nframes] = (unsigned char) f_kf;
	  rec_nframes++;
	  f_off = -1;
	}
    }
  if (f_off >= 0)
    {
      rec_tail_warn ();
    }
  fseek (rec_in, pos, SEEK_SET);
  rec_line = line0;
  return rec_nframes;
}

/* load indexed frame i (0-based) into 'view': read from the keyframe at
 * or before it, so the identities it only changes are complete */
int
view_replay_goto (int i)
{
  int j = i;

  if (i < 0 || i >= rec_nframes)
    {
      return -1;
    }
  while (j > 0 && !rec_kf[j])
    {
      j--;
    }
  if (fseek (rec_in, rec_off[j], SEEK_SET) != 0)
    {
      return -1;
    }
  rec_line = rec_lno[j] - 1;
  rec_seq = j;
  ncar = 0;
  for (; j <= i; j++)
    {
      if (view_replay_next () != 0)
	{
	  return -1;
	}
    }
  return 0;
}
