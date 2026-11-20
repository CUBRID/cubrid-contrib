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
 * target memory - the only reader of the server's memory (pread, gather, strings, names)
 */

#include "ctv.h"


/* ------------------------------------------------------------- mem access */

/* every target read goes through mem_pread on /proc/<pid>/mem, or through
 * mem_gather_ok (process_vm_readv: the same permission and the same page
 * walk, one syscall for many small ranges) */
static int mem_fd = -1;
static int mem_pid = 0;
static int gather_ok = 1;	/* process_vm_readv available */

int
mem_open (int pid)
{
  char p[64];

  snprintf (p, sizeof (p), "/proc/%d/mem", pid);
  mem_fd = open (p, O_RDONLY);
  mem_pid = pid;
  return mem_fd < 0 ? -1 : 0;
}

static ssize_t
mem_pread (uint64_t addr, void *buf, size_t n)
{
  return pread (mem_fd, buf, n, (off_t) addr);
}

int
mem_read (uint64_t addr, void *buf, size_t n)
{
  return mem_pread (addr, buf, n) == (ssize_t) n ? 0 : -1;
}

/* one range alone: read whole, or zero-filled and not marked */
static void
pread_one (uint64_t addr, char *dst, size_t each, unsigned char *ok)
{
  if (mem_pread (addr, dst, each) != (ssize_t) each)
    {
      memset (dst, 0, each);
    }
  else if (ok != NULL)
    {
      *ok = 1;
    }
}

/* read 'each' bytes at every address into buf[i*each]; address 0 and a
 * range that cannot be read come back zero-filled, and ok[i] (when ok is
 * not NULL) says which ranges were read.  Up to IOV_MAX ranges per
 * process_vm_readv; it stops at the first unreadable range, which is read
 * alone with pread, and the batch resumes after it.  Without
 * process_vm_readv every range is a pread. */
int
mem_gather_ok (const uint64_t * addrs, size_t n, size_t each, void *buf,
	       unsigned char *ok)
{
  size_t i = 0;

  memset (buf, 0, n * each);
  if (ok != NULL)
    {
      memset (ok, 0, n);
    }
  while (i < n)
    {
      struct iovec lv[IOV_MAX], rv[IOV_MAX];
      size_t idx[IOV_MAX], cnt = 0, scan, k;

      for (scan = i; scan < n && cnt < IOV_MAX; scan++)
	{
	  if (addrs[scan] == 0)
	    {
	      continue;
	    }
	  idx[cnt] = scan;
	  lv[cnt].iov_base = (char *) buf + scan * each;
	  lv[cnt].iov_len = each;
	  rv[cnt].iov_base = (void *) (uintptr_t) addrs[scan];
	  rv[cnt].iov_len = each;
	  cnt++;
	}
      if (cnt == 0)
	{
	  break;
	}
      if (gather_ok)
	{
	  ssize_t got = process_vm_readv (mem_pid, lv, cnt, rv, cnt, 0);

	  if (got < 0 && (errno == ENOSYS || errno == EPERM))
	    {
	      gather_ok = 0;	/* not available here: pread each range */
	    }
	  else
	    {
	      size_t done = got > 0 ? (size_t) got / each : 0, d;

	      for (d = 0; ok != NULL && d < done && d < cnt; d++)
		{
		  ok[idx[d]] = 1;
		}
	      if (done >= cnt)
		{
		  i = scan;
		  continue;
		}
	      k = idx[done];
	      pread_one (addrs[k], (char *) buf + k * each, each,
			 ok != NULL ? &ok[k] : NULL);
	      i = k + 1;
	      continue;
	    }
	}
      for (k = 0; k < cnt; k++)
	{
	  pread_one (addrs[idx[k]], (char *) buf + idx[k] * each, each,
		     ok != NULL ? &ok[idx[k]] : NULL);
	}
      i = scan;
    }
  return 0;
}

/* one field of every object in one gather: width bytes at bases[i] + off
 * into out[i * width].  A base that is not a plausible pointer, or one
 * marked in skip (may be NULL), is not read (zero, ok[i] = 0). */
int
mem_gather_field (const uint64_t * bases, size_t n, uint64_t off, size_t width,
		  const unsigned char *skip, void *out, unsigned char *ok)
{
  static uint64_t *addrs = NULL;
  static size_t addrs_n = 0;
  size_t i;

  if (addrs_n < n)
    {
      free (addrs);
      addrs_n = (addrs = malloc (n * sizeof (uint64_t))) != NULL ? n : 0;
      if (addrs == NULL)
	{
	  memset (out, 0, n * width);
	  if (ok != NULL)
	    {
	      memset (ok, 0, n);
	    }
	  return -1;
	}
    }
  for (i = 0; i < n; i++)
    {
      addrs[i] = VALID_PTR (bases[i]) && (skip == NULL || !skip[i])
	? bases[i] + off : 0;
    }
  return mem_gather_ok (addrs, n, width, out, ok);
}

/* read up to max bytes, halving until a read of at least min bytes fits
 * (an object near the end of its mapping); bytes read, 0 when none */
size_t
mem_read_shrink (uint64_t addr, void *buf, size_t max, size_t min)
{
  size_t n;

  for (n = max; n >= min && n > 0; n /= 2)
    {
      if (mem_read (addr, buf, n) == 0)
	{
	  return n;
	}
    }
  return 0;
}

/* read a user-space pointer and validate its range; 0 on success */
int
mem_read_ptr (uint64_t addr, uint64_t * p)
{
  return (mem_read (addr, p, 8) == 0 && VALID_PTR (*p)) ? 0 : -1;
}

/* read a non-empty NUL-terminated printable string; 0 on success.
 * a short read is accepted when the NUL fits (string near a mapping end). */
int
mem_cstr (uint64_t addr, char *out, size_t outn)
{
  ssize_t got;

  if (addr == 0)
    {
      return -1;
    }
  got = mem_pread (addr, out, outn);
  /* a terminated string within the buffer: a longer printable run is a
   * region, not a name */
  if (got < 2 || memchr (out, '\0', (size_t) got) == NULL)
    {
      return -1;
    }
  return (out[0] != '\0' && is_printable_text (out)) ? 0 : -1;
}

/* the string at a static name pointer (CS / sync primitive names): read
 * once per pointer, "?" when unreadable (not cached) */
#define NAME_CACHE_MAX 1024	/* power of two; filled to half at most */
const char *
name_of (uint64_t ptr, char *out, size_t n)
{
  static uint64_t keys[NAME_CACHE_MAX];
  static char vals[NAME_CACHE_MAX][NAME_MAX_LEN];
  static int nkeys = 0;
  unsigned h = (unsigned) ((ptr >> 3) * 2654435761u) & (NAME_CACHE_MAX - 1);

  for (; keys[h] != 0; h = (h + 1) & (NAME_CACHE_MAX - 1))
    {
      if (keys[h] == ptr)
	{
	  snprintf (out, n, "%s", vals[h]);
	  return out;
	}
    }
  if (ptr == 0 || mem_cstr (ptr, out, n) != 0)
    {
      snprintf (out, n, "?");
      return out;
    }
  if (nkeys < NAME_CACHE_MAX / 2)
    {
      keys[h] = ptr;
      snprintf (vals[h], NAME_MAX_LEN, "%s", out);
      nkeys++;
    }
  return out;
}

uint64_t
rd_u64 (const unsigned char *b, size_t off)
{
  uint64_t v;

  memcpy (&v, b + off, 8);
  return v;
}

int32_t
rd_i32 (const unsigned char *b, size_t off)
{
  int32_t v;

  memcpy (&v, b + off, 4);
  return v;
}

uint32_t
rd_u32 (const unsigned char *b, size_t off)
{
  uint32_t v;

  memcpy (&v, b + off, 4);
  return v;
}

int64_t
rd_i64 (const unsigned char *b, size_t off)
{
  int64_t v;

  memcpy (&v, b + off, 8);
  return v;
}
