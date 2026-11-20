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
 * transactions - TDES and CLIENTIDS probes (I-6..I-11), identities and their cache
 */

#include "ctv.h"

/* --------------------------------------------- transaction identity (mem) */

/* log_Gl.trantable.all_tdes[i] -> TDES -> CLIENTIDS, all probed (I-6..I-11) */

TRAN_GEOM tran_geom;	/* all_tdes == 0: unavailable */

static uint64_t *tdes_ptrs = NULL;	/* all_tdes image (one read) */
static int ptrs_n = 0;			/* valid entries of tdes_ptrs */
static unsigned char *span_img = NULL;	/* gathered spans of the window */
static unsigned char *span_ok = NULL;	/* span read for this slot */
static unsigned char *span_free = NULL;	/* slot read as released (trid -1) */
static int prefetch_n = 0;		/* valid entries of span_img */

/* n pointers of all_tdes from index base: one read; near the end of a
 * mapping, one gather that zero-fills the unreadable ones */
static void
tdes_ptrs_read (int base, uint64_t * out, int n)
{
  uint64_t *addrs;
  int j;

  if (mem_read (tran_geom.all_tdes + 8ULL * base, out, (size_t) n * 8) == 0)
    {
      return;
    }
  if ((addrs = malloc ((size_t) n * sizeof (uint64_t))) == NULL)
    {
      memset (out, 0, (size_t) n * 8);
      return;
    }
  for (j = 0; j < n; j++)
    {
      addrs[j] = tran_geom.all_tdes + 8ULL * (uint64_t) (base + j);
    }
  (void) mem_gather_ok (addrs, (size_t) n, 8, out, NULL);
  free (addrs);
}

/* the descriptor pointer array in one read; 0 on success */
static int
tdes_ptrs_load (void)
{
  int n = tran_geom.ntran;

  ptrs_n = 0;
  if (tran_geom.all_tdes == 0 || n <= 0)
    {
      return -1;
    }
  if (tdes_ptrs == NULL && (tdes_ptrs = malloc ((size_t) n * 8)) == NULL)
    {
      return -1;
    }
  tdes_ptrs_read (0, tdes_ptrs, n);
  ptrs_n = n;
  return 0;
}

static int
tdes_at (int i, uint64_t * tdes)
{
  if (i < ptrs_n && VALID_PTR (tdes_ptrs[i]))
    {
      *tdes = tdes_ptrs[i];
      return 0;
    }
  return mem_read_ptr (tran_geom.all_tdes + 8ULL * i, tdes);
}

/* an image of target memory [base, base+len): reads inside it are served
 * from the image, the rest from the target (NULL = always the target) */
typedef struct
{
  const unsigned char *img;
  uint64_t base;
  size_t len;
} SPAN;

static int
tr_read (const SPAN * sp, uint64_t addr, void *buf, size_t n)
{
  if (sp != NULL && addr >= sp->base && addr + n <= sp->base + sp->len)
    {
      memcpy (buf, sp->img + (addr - sp->base), n);
      return 0;
    }
  return mem_read (addr, buf, n);
}

static int
tr_read_ptr (const SPAN * sp, uint64_t addr, uint64_t * p)
{
  return (tr_read (sp, addr, p, 8) == 0 && VALID_PTR (*p)) ? 0 : -1;
}

/* COW representations shared by many slots (the empty string, copied
 * names) are decoded once per window */
#define REP_CACHE_MAX 64
typedef struct
{
  uint64_t p;
  int rc;
  char s[IDENT_LEN];
} REP_CACHE;
static REP_CACHE rep_cache[REP_CACHE_MAX];
static int rep_cache_n = 0;

static void
rep_cache_reset (void)
{
  rep_cache_n = 0;
}

/* old ABI: the object is a char* into the heap, _Rep{len,cap,ref} before */
static int
read_cow_string (const SPAN * sp, uint64_t addr, char *out, size_t outn)
{
  uint64_t p;
  unsigned char rep[20];
  int64_t len, cap;
  int32_t ref;
  size_t ln;
  int i, rc = -1;

  if (tr_read_ptr (sp, addr, &p) != 0 || p < COW_REP_LEN)
    {
      return -1;
    }
  for (i = 0; i < rep_cache_n; i++)
    {
      if (rep_cache[i].p == p)
	{
	  snprintf (out, outn, "%s", rep_cache[i].s);
	  return rep_cache[i].rc;
	}
    }
  out[0] = '\0';
  if (mem_read (p - COW_REP_LEN, rep, sizeof (rep)) == 0)
    {
      len = rd_i64 (rep, 0);
      cap = rd_i64 (rep, 8);
      memcpy (&ref, rep + 16, 4);
      if (len >= 0 && len <= STR_LEN_MAX && cap >= len && cap <= STR_CAP_MAX
	  && ref >= -1 && ref < STR_REF_MAX)
	{
	  ln = (size_t) len < outn - 1 ? (size_t) len : outn - 1;
	  if (ln == 0 || mem_read (p, out, ln) == 0)
	    {
	      if ((size_t) len > ln)
		{
		  ln = utf8_cut (out, ln);	/* cut at a character boundary */
		}
	      out[ln] = '\0';
	      rc = 0;
	    }
	}
    }
  if (rc != 0)
    {
      out[0] = '\0';		/* a failed body read may leave no NUL */
    }
  if (rep_cache_n < REP_CACHE_MAX && outn >= IDENT_LEN)
    {
      rep_cache[rep_cache_n].p = p;
      rep_cache[rep_cache_n].rc = rc;
      snprintf (rep_cache[rep_cache_n].s, IDENT_LEN, "%s", out);
      rep_cache_n++;
    }
  return rc;
}

/* new ABI: {ptr, size, buf16|cap}; short strings live in the local buffer */
static int
read_sso_string (const SPAN * sp, uint64_t addr, char *out, size_t outn)
{
  unsigned char raw[SSO_STRIDE];
  uint64_t p, size;
  size_t ln;

  if (tr_read (sp, addr, raw, sizeof (raw)) != 0)
    {
      return -1;
    }
  p = rd_u64 (raw, 0);
  size = rd_u64 (raw, 8);
  if (size > STR_LEN_MAX || !VALID_PTR (p))
    {
      return -1;
    }
  ln = (size_t) size < outn - 1 ? (size_t) size : outn - 1;
  if (p == addr + SSO_LOCAL_OFF)
    {
      if (size > SSO_LOCAL_MAX)
	{
	  return -1;
	}
      memcpy (out, raw + SSO_LOCAL_OFF, ln);
    }
  else if (mem_read (p, out, ln) != 0)
    {
      return -1;
    }
  if ((size_t) size > ln)
    {
      ln = utf8_cut (out, ln);	/* cut at a character boundary */
    }
  out[ln] = '\0';
  return 0;
}

/* one std::string of either ABI, printable text; 0 on success */
static int
read_std_string (const SPAN * sp, uint64_t addr, int stride, char *out,
		 size_t outn)
{
  int rc;

  out[0] = '\0';
  rc = (stride == COW_STRIDE) ? read_cow_string (sp, addr, out, outn)
    : read_sso_string (sp, addr, out, outn);
  return (rc == 0 && is_printable_text (out)) ? 0 : -1;
}

/* a descriptor carries client content */
static int
ident_has_content (const TRAN_IDENT * id)
{
  return id->info[0] || id->user[0] || id->program[0] || id->login[0]
    || id->host[0] || id->ip[0];
}

/* I-9: 6 strings + pid at (off, stride) in a TDES.  strict (relaxed==0)
 * demands content; relaxed treats empty / zero COW pointers as empty. */
static int
read_clientids_at (const SPAN * sp, uint64_t tdes, int off, int stride,
		   TRAN_IDENT * id,
		   int relaxed)
{
  char *slot[CLIENTIDS_NSTR] =
    { id->info, id->user, id->program, id->login, id->host, id->ip };
  int k;
  int32_t cpid;

  for (k = 0; k < CLIENTIDS_NSTR; k++)
    {
      uint64_t sa = tdes + off + (uint64_t) k * stride;

      if (read_std_string (sp, sa, stride, slot[k], IDENT_LEN) != 0)
	{
	  uint64_t p;

	  if (relaxed && stride == COW_STRIDE
	      && tr_read (sp, sa, &p, 8) == 0 && p == 0)
	    {
	      slot[k][0] = '\0';
	      continue;
	    }
	  return -1;
	}
    }
  if ((!relaxed && !ident_has_content (id))
      || tr_read (sp, tdes + off + (uint64_t) CLIENTIDS_NSTR * stride,
		  &cpid, 4) != 0
      || cpid < 0 || cpid >= PID_MAX_LIMIT)
    {
      return -1;
    }
  id->cpid = cpid;
  return 0;
}

/* I-6: all_tdes = a pointer inside log_Gl to an array of pointers t_k with
 * *(int *)(t_k + off_ti) == k for k = 0..3 */
static const char *
tran_probe_all_tdes (uint64_t log_gl)
{
  unsigned char *blob;
  size_t scan = LOG_GL_SCAN_MAX;
  int off;

  blob = malloc (scan);
  if (blob == NULL)
    {
      return "I-6 out of memory";
    }
  scan = mem_read_shrink (log_gl, blob, scan, 4096);
  if (scan == 0)
    {
      free (blob);
      REFUSE ("I-6 log_Gl unreadable");
    }
  for (off = 0; off + 8 <= (int) scan; off += 8)
    {
      uint64_t p = rd_u64 (blob, off);
      unsigned char heads[CS_MIN_SAMPLES][TDES_HEAD_SCAN];
      uint64_t tds[CS_MIN_SAMPLES];
      int k, ti;

      if (!VALID_PTR (p) || mem_read (p, tds, sizeof (tds)) != 0)
	{
	  continue;
	}
      for (k = 0; k < CS_MIN_SAMPLES; k++)
	{
	  if (!VALID_PTR (tds[k])
	      || mem_read (tds[k], heads[k], sizeof (heads[k])) != 0)
	    {
	      break;
	    }
	}
      if (k < CS_MIN_SAMPLES)
	{
	  continue;
	}
      for (ti = 0; ti + 4 <= TDES_HEAD_SCAN; ti += 4)
	{
	  for (k = 0; k < CS_MIN_SAMPLES; k++)
	    {
	      if (rd_i32 (heads[k], ti) != k)
		{
		  break;
		}
	    }
	  if (k == CS_MIN_SAMPLES)
	    {
	      tran_geom.all_tdes = p;
	      tran_geom.off_ti = ti;
	      free (blob);
	      return NULL;
	    }
	}
    }
  free (blob);
  REFUSE ("I-6 all_tdes");
}

/* number of descriptors: tran_index self-validates each slot */
static int
tran_count (void)
{
  enum { CHUNK = 256 };
  uint64_t ptrs[CHUNK];
  int32_t ti[CHUNK];
  int base, j;

  for (base = 0; base < TRAN_SLOTS_MAX; base += CHUNK)
    {
      int cnt = TRAN_SLOTS_MAX - base < CHUNK ? TRAN_SLOTS_MAX - base : CHUNK;

      tdes_ptrs_read (base, ptrs, cnt);
      if (mem_gather_field (ptrs, (size_t) cnt, (uint64_t) tran_geom.off_ti, 4,
			    NULL, ti, NULL) != 0)
	{
	  return base;		/* not read: count only what was confirmed */
	}
      for (j = 0; j < cnt; j++)
	{
	  if (!VALID_PTR (ptrs[j]) || ti[j] != base + j)
	    {
	      return base + j;
	    }
	}
    }
  deg_set (DEG_TRAN_CAP, 1);
  return TRAN_SLOTS_MAX;
}

/* I-7: per-TDES scan bound = spacing of the first two descriptors */
static const char *
tran_probe_spacing (void)
{
  uint64_t t0, t1;

  if (tdes_at (0, &t0) != 0 || tdes_at (1, &t1) != 0)
    {
      REFUSE ("I-7 descriptor pointers");
    }
  {
    uint64_t gap = t1 > t0 ? t1 - t0 : t0 - t1;

    tran_geom.max_scan = gap <= TDES_SCAN_MAX ? (int) gap : 0;
  }
  tran_geom.scan_measured = tran_geom.max_scan > 0;
  if (tran_geom.max_scan <= 0)
    {
      /* descriptors not laid out back to back (separate allocations): the
       * scan is bounded instead; CLIENTIDS still has to validate (I-9) */
      tran_geom.max_scan = TDES_SCAN_FALLBACK;
    }
  return NULL;
}

/* I-8: trid@off_ti+4 is trusted only if the system tran reads 0 AND the
 * next descriptors read non-zero (-1 or a real trid); else degrade */
static void
tran_probe_trid (void)
{
  int32_t v;
  uint64_t tk;
  int k, ok;

  ok = tdes_at (0, &tk) == 0
    && mem_read (tk + tran_geom.off_ti + 4, &v, 4) == 0 && v == 0;
  for (k = 1; ok && k <= 3 && k < tran_geom.ntran; k++)
    {
      ok = tdes_at (k, &tk) == 0
	&& mem_read (tk + tran_geom.off_ti + 4, &v, 4) == 0 && v != 0;
    }
  tran_geom.has_trid = ok;
  if (!ok)
    {
      deg_set (DEG_TRID, 1);
    }
}

/* one-time geometry probe; NULL when the transaction table is available,
 * else the invariant that refused */
const char *
tran_probe (uint64_t log_gl)
{
  const char *why;

  if ((why = tran_probe_all_tdes (log_gl)) != NULL
      || (why = tran_probe_spacing ()) != NULL)
    {
      tran_geom.all_tdes = 0;
      return why;
    }
  tran_geom.ntran = tran_count ();
  tran_probe_trid ();
  return NULL;
}

/* up to NWITNESS other valid descriptors, one per third of the table
 * (falling forward to the next valid slot), never the same slot twice */
static int
collect_witnesses (int i, int ntran, uint64_t * wit)
{
  int k, nwit = 0, wstep = ntran > 3 ? ntran / 3 : 1;

  for (k = 0; k < NWITNESS; k++)
    {
      int oi, tries;

      for (oi = (i + 1 + k * wstep) % ntran, tries = 0;
	   tries < ntran; oi = (oi + 1) % ntran, tries++)
	{
	  uint64_t other;
	  int dup = 0, w;

	  if (oi == i || tdes_at (oi, &other) != 0)
	    {
	      continue;
	    }
	  for (w = 0; w < nwit; w++)
	    {
	      dup |= wit[w] == other;
	    }
	  if (dup)
	    {
	      continue;
	    }
	  wit[nwit++] = other;
	  break;
	}
    }
  return nwit;
}

/* witness vote at a candidate geometry: 1 = parses with real content,
 * 0 = parses but all-empty (a freed slot - no information), -1 = fails */
static int
witness_vote (uint64_t tdes, int off, int stride)
{
  TRAN_IDENT w;

  if (read_clientids_at (NULL, tdes, off, stride, &w, 1) != 0)
    {
      return -1;
    }
  return ident_has_content (&w) ? 1 : 0;
}

/* I-10: accept unless >= 2 witnesses fail AND they outnumber content
 * agreements; empty witnesses carry no vote */
static int
witnesses_accept (int i, int ntran, int off, int stride, uint64_t * wit,
		  int *nwit)
{
  int wi, fails = 0, content = 0;

  if (*nwit < 0)
    {
      *nwit = collect_witnesses (i, ntran, wit);
    }
  for (wi = 0; wi < *nwit; wi++)
    {
      int v = witness_vote (wit[wi], off, stride);

      fails += v < 0;
      content += v > 0;
    }
  return !(fails >= 2 && fails > content);
}

/* in-image prefilter: every string object at off must look like a string
 * (COW: user pointer; SSO: user pointer and size <= STR_LEN_MAX) */
static int
clientids_plausible (const unsigned char *img, int off, int stride)
{
  int k;

  for (k = 0; k < CLIENTIDS_NSTR; k++)
    {
      size_t o = (size_t) off + (size_t) k * stride;

      if (!VALID_PTR (rd_u64 (img, o))
	  || (stride == SSO_STRIDE && rd_u64 (img, o + 8) > STR_LEN_MAX))
	{
	  return 0;
	}
    }
  return 1;
}

static int
lock_on_descriptor (int i, int ntran, uint64_t tdes)
{
  static const int strides[] = { COW_STRIDE, SSO_STRIDE };
  uint64_t wit[NWITNESS];
  SPAN sp;
  unsigned char *img = malloc ((size_t) tran_geom.max_scan);
  int nwit = -1, off;
  unsigned int s;
  size_t got;
  TRAN_IDENT id;

  /* one read of the descriptor (shorter near the end of its mapping);
   * candidates are filtered from the image */
  got = img != NULL ? mem_read_shrink (tdes, img, (size_t) tran_geom.max_scan, 512) : 0;
  if (got == 0)
    {
      free (img);
      return -1;
    }
  sp.img = img;
  sp.base = tdes;
  sp.len = got;
  for (s = 0; s < 2; s++)
    {
      for (off = 0; off + CLIENTIDS_BYTES (strides[s]) <= (int) got;
	   off += 8)
	{
	  if (!clientids_plausible (img, off, strides[s])
	      || read_clientids_at (&sp, tdes, off, strides[s], &id, 0) != 0)
	    {
	      continue;
	    }
	  if (!witnesses_accept (i, ntran, off, strides[s], wit, &nwit))
	    {
	      continue;
	    }
	  tran_geom.cid_off = off;
	  tran_geom.cid_stride = strides[s];
	  free (img);
	  return 0;
	}
    }
  free (img);
  return -1;
}

/* the trid of every loaded descriptor in one gather (ok[t] = read; may be
 * NULL); 0 on success.  Needs tdes_ptrs and the trid field. */
static int
trids_gather (int32_t * trid, unsigned char *ok)
{
  if (!tran_geom.has_trid || ptrs_n <= 0)
    {
      return -1;
    }
  return mem_gather_field (tdes_ptrs, (size_t) ptrs_n,
			   (uint64_t) tran_geom.off_ti + 4, 4, NULL, trid, ok);
}

/* lock the CLIENTIDS geometry from a descriptor with a client attached.
 * budget <= 0: scan all; else at most that many from a rotating cursor */
static double last_try = 0;	/* last CLIENTIDS lock attempt */

int
tran_lock_clientids (int ntran, int budget)
{
  static int cursor = 0;
  int step;

  if (tran_geom.cid_stride != 0)
    {
      return 0;
    }
  if (ntran <= 0)
    {
      return -1;
    }
  if (budget <= 0 || budget > ntran)
    {
      budget = ntran;
    }
  last_try = mono_s ();		/* a full or lazy scan both count */
  rep_cache_reset ();		/* string decodes of an earlier window are stale */
  (void) tdes_ptrs_load ();
  if (tran_geom.has_trid && ptrs_n == ntran)
    {
      /* only descriptors holding a transaction can carry a client (trid of
       * every descriptor in one gather); the lazy retry tries those that
       * started one since its last look, and a failed one again after
       * LAZY_RETRY_S (its strings may still have been filling) */
      static int32_t *tried = NULL, *trid = NULL;
      static time_t *tried_at = NULL;
      static int tcursor = 0;
      int tries = 0, lazy = budget < ntran;

      if (tried == NULL)
	{
	  tried = calloc ((size_t) ntran, sizeof (int32_t));
	}
      if (tried_at == NULL)
	{
	  tried_at = calloc ((size_t) ntran, sizeof (time_t));
	}
      if (trid == NULL)
	{
	  trid = malloc ((size_t) ntran * sizeof (int32_t));
	}
      if (tried == NULL || tried_at == NULL || trid == NULL
	  || trids_gather (trid, NULL) != 0)
	{
	  return -1;
	}
      /* from a rotating cursor, so descriptors that keep failing at low
       * indices do not take every try */
      for (step = 0; step < ntran - 1 && (!lazy || tries < LAZY_LOCK_TRIES); step++)
	{
	  int i = 1 + (tcursor + step) % (ntran - 1);

	  if (trid[i] <= 0
	      || (trid[i] == tried[i] && (time_t) last_try - tried_at[i] < LAZY_RETRY_S))
	    {
	      continue;
	    }
	  tried[i] = trid[i];
	  tried_at[i] = (time_t) last_try;
	  tries++;
	  if (lock_on_descriptor (i, ntran, tdes_ptrs[i]) == 0)
	    {
	      return 0;
	    }
	}
      if (ntran > 1)
	{
	  tcursor = (tcursor + step) % (ntran - 1);
	}
      return -1;
    }
  for (step = 0; step < budget; step++)
    {
      int i = (cursor + step) % ntran;
      uint64_t tdes;

      if (tdes_at (i, &tdes) == 0 && lock_on_descriptor (i, ntran, tdes) == 0)
	{
	  return 0;
	}
    }
  cursor = (cursor + budget) % ntran;
  return -1;
}

/* once per display cycle: lock the geometry if a client has since appeared */
void
tran_geom_refresh (void)
{
  double now = mono_s ();

  /* no client yet: once a second; with trid, only new transactions are
   * scanned, so a waiting run costs two small reads */
  if (tran_geom.all_tdes != 0 && tran_geom.cid_stride == 0
      && every (&last_try, now, 1.0))
    {
      (void) tran_lock_clientids (tran_geom.ntran, LAZY_LOCK_BUDGET);
    }
}

/* identity of one tran index (pure query); 0 and id filled when a client
 * is attached.  I-11: a released slot (trid == NULL_TRANID) is no client. */
static int
identity_read (const SPAN * sp, int tran_index, TRAN_IDENT * id,
	       int32_t * trid, const int32_t * known_trid)
{
  uint64_t tdes;
  int32_t t = 0;

  if (tran_geom.all_tdes == 0 || tran_geom.cid_stride == 0
      || tran_index < 0 || tran_index >= tran_geom.ntran
      || tdes_at (tran_index, &tdes) != 0)
    {
      return -1;
    }
  if (tran_geom.has_trid)
    {
      if (known_trid != NULL)
	{
	  t = *known_trid;	/* already gathered with the others */
	}
      else if (tr_read (sp, tdes + tran_geom.off_ti + 4, &t, 4) != 0)
	{
	  return -1;
	}
      if (t == -1)
	{
	  return -1;
	}
    }
  if (trid != NULL)
    {
      *trid = t;
    }
  if (read_clientids_at (sp, tdes, tran_geom.cid_off, tran_geom.cid_stride,
			 id, 0) != 0)
    {
      return -1;
    }
  return ident_has_content (id) ? 0 : -1;
}

static int
tran_identity_mem (int tran_index, TRAN_IDENT * id, int32_t * trid)
{
  return identity_read (NULL, tran_index, id, trid, NULL);
}

/* per-slot identity cache: per window the trid from its own gather and the
 * CLIENTIDS run in one gather, strings re-decoded only when trid or the CLIENTIDS bytes changed or the
 * entry aged out.  A new client in a slot starts a new transaction (a new
 * trid); heap strings that change in place keep at most TTL windows. */
#define IDENT_CACHE_TTL 3

typedef struct
{
  uint64_t tdes;		/* 0 = empty entry */
  unsigned char *key;		/* span image of the last decode */
  int age;
  int rc;
  int32_t trid;
  TRAN_IDENT id;
} IDENT_CACHE;

static IDENT_CACHE *ident_cache = NULL;
static int ident_cache_n = 0;
static int span_lo = 0, span_len = 0;	/* the CLIENTIDS run of a TDES */
static int32_t *span_trid = NULL;	/* trid of each slot, this window */
static unsigned char *span_trid_ok = NULL;

static int
ident_cache_setup (void)
{
  int t;

  if (ident_cache_n == tran_geom.ntran && span_len > 0)
    {
      return 0;
    }
  for (t = 0; t < ident_cache_n; t++)
    {
      free (ident_cache[t].key);
    }
  free (ident_cache);
  ident_cache = calloc ((size_t) tran_geom.ntran, sizeof (IDENT_CACHE));
  ident_cache_n = ident_cache == NULL ? 0 : tran_geom.ntran;
  free (tdes_ptrs);
  free (span_img);
  free (span_ok);
  free (span_free);
  free (span_trid);
  free (span_trid_ok);
  tdes_ptrs = NULL;
  span_img = NULL;
  span_ok = NULL;
  span_free = NULL;
  span_trid = NULL;
  span_trid_ok = NULL;
  ptrs_n = 0;
  prefetch_n = 0;
  /* the trid comes from its own gather: the span is CLIENTIDS alone */
  span_lo = tran_geom.cid_off;
  span_len = CLIENTIDS_BYTES (tran_geom.cid_stride);
  return ident_cache == NULL ? -1 : 0;
}

/* one window: the descriptor pointer array in one read, the trid array in
 * one gather, then the CLIENTIDS runs of the slots in use in one gather.
 * Slots not covered fall back to per-slot reads. */
void
tran_identity_prefetch (void)
{
  int t, n;

  prefetch_n = 0;
  rep_cache_reset ();
  if (tran_geom.all_tdes == 0 || tran_geom.cid_stride == 0
      || ident_cache_setup () != 0)
    {
      return;
    }
  n = tran_geom.ntran;
  if (span_img == NULL)
    {
      span_img = malloc ((size_t) n * (size_t) span_len);
    }
  if (span_ok == NULL)
    {
      span_ok = malloc ((size_t) n);
    }
  if (span_free == NULL)
    {
      span_free = malloc ((size_t) n);
    }
  if (span_trid == NULL)
    {
      span_trid = malloc ((size_t) n * 4);
    }
  if (span_trid_ok == NULL)
    {
      span_trid_ok = malloc ((size_t) n);
    }
  if (span_img == NULL || span_ok == NULL || span_free == NULL
      || span_trid == NULL || span_trid_ok == NULL || tdes_ptrs_load () != 0)
    {
      return;
    }
  memset (span_free, 0, (size_t) n);
  memset (span_trid_ok, 0, (size_t) n);
  if (tran_geom.has_trid)
    {
      if (ptrs_n != n || trids_gather (span_trid, span_trid_ok) != 0)
	{
	  return;
	}
      for (t = 0; t < n; t++)
	{
	  span_free[t] = span_trid_ok[t] && span_trid[t] == -1;
	}
    }
  if (mem_gather_field (tdes_ptrs, (size_t) n, (uint64_t) span_lo, (size_t) span_len,
			span_free, span_img, span_ok) == 0)
    {
      prefetch_n = n;
    }
}

/* the client pid in a slot's CLIENTIDS now (-1: not known): which client a
 * sighting belongs to when the slot changes hands */
int
tran_client_pid (int tran_index)
{
  uint64_t tdes;
  int32_t pid;

  if (tran_geom.cid_stride == 0 || tran_index < 1 || tran_index >= tran_geom.ntran)
    {
      return -1;
    }
  if (tdes_ptrs != NULL && tran_index < ptrs_n && VALID_PTR (tdes_ptrs[tran_index]))
    {
      tdes = tdes_ptrs[tran_index];
    }
  else if (tdes_at (tran_index, &tdes) != 0)
    {
      return -1;
    }
  if (mem_read (tdes + (uint64_t) tran_geom.cid_off
		+ (uint64_t) CLIENTIDS_NSTR * (uint64_t) tran_geom.cid_stride, &pid, 4) != 0
      || pid < 0 || pid >= PID_MAX_LIMIT)
    {
      return -1;
    }
  return pid;
}

int
tran_identity_cached (int tran_index, TRAN_IDENT * id, int32_t * trid)
{
  IDENT_CACHE *c;
  unsigned char buf[CLIENTIDS_BYTES (SSO_STRIDE)], *img = buf;
  unsigned char key[4 + CLIENTIDS_BYTES (SSO_STRIDE)];
  size_t keyn;
  uint64_t tdes;
  int32_t trid_v = 0;

  if (tran_geom.all_tdes == 0 || tran_geom.cid_stride == 0
      || tran_index < 0 || tran_index >= tran_geom.ntran
      || ident_cache_setup () != 0)
    {
      return -1;
    }
  if (tran_index < prefetch_n && span_free[tran_index])
    {
      return -1;		/* released this window, as identity_read */
    }
  /* a span the gather could not read falls back to its own reads */
  if (tran_index < prefetch_n && VALID_PTR (tdes_ptrs[tran_index])
      && span_ok[tran_index])
    {
      tdes = tdes_ptrs[tran_index];
      img = span_img + (size_t) tran_index * (size_t) span_len;
    }
  else if (tdes_at (tran_index, &tdes) != 0
	   || mem_read (tdes + span_lo, buf, (size_t) span_len) != 0)
    {
      return -1;
    }
  if (tran_geom.has_trid)
    {
      if (tran_index < prefetch_n && span_trid_ok[tran_index])
	{
	  trid_v = span_trid[tran_index];
	}
      else if (mem_read (tdes + (uint64_t) tran_geom.off_ti + 4, &trid_v, 4) != 0)
	{
	  return -1;
	}
    }
  c = &ident_cache[tran_index];
  /* the key: trid and the CLIENTIDS bytes */
  memcpy (key, &trid_v, 4);
  memcpy (key + 4, img, (size_t) span_len);
  keyn = 4 + (size_t) span_len;
  if (c->tdes == tdes && c->key != NULL && c->age < IDENT_CACHE_TTL
      && memcmp (c->key, key, keyn) == 0)
    {
      c->age++;
    }
  else
    {
      if (c->key == NULL && (c->key = malloc (keyn)) == NULL)
	{
	  return tran_identity_mem (tran_index, id, trid);
	}
      memcpy (c->key, key, keyn);
      c->tdes = tdes;
      c->age = 0;
      {
	SPAN sp = { img, tdes + (uint64_t) span_lo, (size_t) span_len };

	c->rc = identity_read (&sp, tran_index, &c->id, &c->trid,
			       tran_geom.has_trid ? &trid_v : NULL);
      }
    }
  if (c->rc == 0)
    {
      *id = c->id;
      if (trid != NULL)
	{
	  *trid = c->trid;
	}
    }
  return c->rc;
}

/* every descriptor slot as JSON (detail dump): raw tran_index/trid and the
 * client strings, including released and empty slots */
void
tran_dump (FILE * f)
{
  int t;

  rep_cache_reset ();
  fprintf (f, "\"transactions\":{\"available\":%s,\"all_tdes\":\"0x%llx\","
	   "\"ntran\":%d,\"off_tran_index\":%d,\"cid_off\":%d,"
	   "\"cid_abi\":\"%s\",\"tdes_scan_bytes\":%d,"
	   "\"tdes_scan_is_spacing\":%s,\"has_trid\":%d,"
	   "\"slots\":[\n", tran_geom.all_tdes != 0 ? "true" : "false",
	   (unsigned long long) tran_geom.all_tdes, tran_geom.ntran,
	   tran_geom.off_ti, tran_geom.cid_off,
	   tran_geom.cid_stride == COW_STRIDE ? "cow"
	   : tran_geom.cid_stride == SSO_STRIDE ? "sso" : "not-locked",
	   tran_geom.max_scan, tran_geom.scan_measured ? "true" : "false",
	   tran_geom.has_trid);
  for (t = 0; tran_geom.all_tdes != 0 && t < tran_geom.ntran; t++)
    {
      uint64_t tdes;
      int32_t ti = -2, trid = 0;
      TRAN_IDENT id;
      int have_id;
      const char *state;

      memset (&id, 0, sizeof (id));
      if (tdes_at (t, &tdes) != 0)
	{
	  fprintf (f, "%s{\"idx\":%d,\"state\":\"unreadable\"}\n",
		   t ? "," : "", t);
	  continue;
	}
      (void) mem_read (tdes + tran_geom.off_ti, &ti, 4);
      (void) mem_read (tdes + tran_geom.off_ti + 4, &trid, 4);
      have_id = tran_geom.cid_stride != 0
	&& read_clientids_at (NULL, tdes, tran_geom.cid_off, tran_geom.cid_stride,
			      &id, 1) == 0;
      state = t == 0 ? "system"
	: tran_geom.has_trid && trid == -1 ? "released"
	: have_id && ident_has_content (&id) ? "client" : "empty";
      fprintf (f, "%s{\"idx\":%d,\"tdes\":\"0x%llx\",\"tran_index\":%d,"
	       "\"trid\":%d,\"state\":\"%s\"", t ? "," : "", t,
	       (unsigned long long) tdes, ti, trid, state);
      if (have_id)
	{
	  json_ident (f, &id, 1);
	}
      fprintf (f, "}\n");
    }
  fprintf (f, "]}");
}
