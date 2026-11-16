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
 * text - UTF-8 lengths, terminal columns, cutting and padding by column,
 * sanitizing outside strings, hashing
 */

#include "ctv.h"

/* length of the UTF-8 sequence led by c: 1 for ASCII, 2..4 for a valid
 * lead byte, 0 for a continuation or an invalid lead */
static int
utf8_seq_len (unsigned char c)
{
  return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 && c >= 0xC2 ? 2
    : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 4 : 0;
}

/* length of the complete, valid UTF-8 sequence at s (1 for ASCII), or 0
 * for a stray continuation, an invalid lead or a sequence cut short */
int
utf8_valid_len (const char *s)
{
  const unsigned char *p = (const unsigned char *) s;
  int n = utf8_seq_len (*p), j;

  for (j = 1; j < n; j++)
    {
      if ((p[j] & 0xC0) != 0x80)
	{
	  return 0;
	}
    }
  return n;
}

/* length of the printable sequence at s: an ASCII graphic/space byte, or
 * a valid UTF-8 sequence that is not a C1 control (U+0080..U+009F);
 * 0 for a control byte, a C1 control or broken UTF-8 */
static int
printable_seq_len (const char *s)
{
  const unsigned char *p = (const unsigned char *) s;
  int n;

  if (*p < 0x80)
    {
      return *p >= 0x20 && *p != 0x7f ? 1 : 0;
    }
  n = utf8_valid_len (s);
  return n < 2 || (p[0] == 0xC2 && p[1] < 0xA0) ? 0 : n;
}

/* bytes (*len, at least 1) and terminal columns of the sequence at s; a
 * broken sequence is one byte, one column */
int
seq_cols (const char *s, int *len)
{
  int n = utf8_valid_len (s);

  *len = n == 0 ? 1 : n;
  return n == 0 ? 1 : utf8_cols (s, n);
}

/* length of s[0..ln) without a UTF-8 sequence cut at its end */
size_t
utf8_cut (const char *s, size_t ln)
{
  size_t p = ln, need;
  unsigned char c;

  while (p > 0 && ((unsigned char) s[p - 1] & 0xC0) == 0x80 && ln - p < 3)
    {
      p--;
    }
  if (p == 0)
    {
      return ln;
    }
  c = (unsigned char) s[p - 1];
  need = (size_t) utf8_seq_len (c);
  return need > 1 && (p - 1) + need > ln ? p - 1 : ln;
}

/* FNV-1a (64-bit) of n bytes, continued from h (start with FNV_SEED) */
uint64_t
fnv (uint64_t h, const void *p, size_t n)
{
  const unsigned char *b = p;
  size_t i;

  for (i = 0; i < n; i++)
    {
      h = (h ^ b[i]) * 1099511628211ULL;
    }
  return h;
}

/* terminal columns of a whole string */
int
str_cols (const char *s)
{
  int cols = 0, len;

  for (; *s != '\0'; s += len)
    {
      cols += seq_cols (s, &len);
    }
  return cols;
}

/* outside text (records, procfs names, argv) before it can reach the
 * terminal: control bytes (C0, DEL), C1 code points (U+0080..U+009F) and
 * broken UTF-8 become '?' */
void
text_sanitize (char *s)
{
  unsigned char *p = (unsigned char *) s;

  while (*p != '\0')
    {
      int len = printable_seq_len ((const char *) p), j;

      if (len > 0)
	{
	  p += len;
	  continue;
	}
      /* not printable: '?' over the whole (valid) sequence, else one byte */
      for (j = utf8_valid_len ((const char *) p); j > 1 && p[1] != '\0'; j--)
	{
	  *p++ = '?';
	}
      *p++ = '?';
    }
}

/* terminal columns of the UTF-8 sequence at s (len bytes): 2 for East Asian
 * wide characters and emoji, else 1 */
int
utf8_cols (const char *s, int len)
{
  const unsigned char *p = (const unsigned char *) s;
  unsigned cp;

  if (len < 3)
    {
      return 1;
    }
  cp = len == 3 ? ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu)
    : ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6)
    | (p[3] & 0x3Fu);
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E)
    || (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF)
    || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF)
    || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF)
    || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60)
    || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F)
    || (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD)
    ? 2 : 1;
}

/* printable text: ASCII 0x20..0x7E or well-formed UTF-8 (no controls) */
int
is_printable_text (const char *s)
{
  const unsigned char *p = (const unsigned char *) s;

  while (*p != '\0')
    {
      int n = printable_seq_len ((const char *) p);

      if (n == 0)
	{
	  return 0;
	}
      p += n;
    }
  return 1;
}

/* copy s into out padded (or cut) to 'width' terminal columns, each
 * sequence counted by seq_cols */
size_t
dw_pad (char *out, size_t n, const char *s, int width)
{
  size_t o = 0;
  int cols = 0;

  while (*s != '\0' && o + 4 < n)
    {
      int len, w = seq_cols (s, &len);

      if (cols + w > width)
	{
	  break;
	}
      memcpy (out + o, s, (size_t) len);
      o += (size_t) len;
      s += len;
      cols += w;
    }
  while (cols < width && o + 1 < n)
    {
      out[o++] = ' ';
      cols++;
    }
  out[o] = '\0';
  return o;
}

/* append to buf at *pos; on overflow *pos stops at the terminator */
void
buf_cat (char *buf, size_t n, size_t * pos, const char *fmt, ...)
{
  va_list ap;
  int w;

  if (*pos + 1 >= n)
    {
      return;
    }
  va_start (ap, fmt);
  w = vsnprintf (buf + *pos, n - *pos, fmt, ap);
  va_end (ap);
  if (w < 0)
    {
      return;
    }
  if (*pos + (size_t) w < n)
    {
      *pos += (size_t) w;
      return;
    }
  /* cut: never inside a UTF-8 sequence */
  *pos = utf8_cut (buf, n - 1);
  buf[*pos] = '\0';
}
