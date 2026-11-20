/*
 * t20: outside strings come out as valid UTF-8.  The second byte's range
 * depends on the lead (RFC 3629): overlong forms (E0 80..9F, F0 80..8F),
 * surrogates (ED A0..BF) and code points past U+10FFFF (F4 90..) are broken
 * sequences, while the edges just inside stay whole.  Checks
 * utf8_valid_len, text_sanitize and json_fstr (written to argv[1] as one
 * JSON array for the caller to parse).
 */
#include "ctv.h"

static const struct
{
  const char *s;
  int valid;
} cases[] = {
  {"\xE0\xA0\x80", 3}, {"\xE0\x9F\xBF", 0}, {"\xE0\x80\x80", 0},
  {"\xED\x9F\xBF", 3}, {"\xED\xA0\x80", 0}, {"\xED\xBF\xBF", 0},
  {"\xEE\x80\x80", 3}, {"\xE1\x80\x80", 3},
  {"\xF0\x90\x80\x80", 4}, {"\xF0\x8F\xBF\xBF", 0}, {"\xF0\x80\x80\x80", 0},
  {"\xF4\x8F\xBF\xBF", 4}, {"\xF4\x90\x80\x80", 0}, {"\xF3\xBF\xBF\xBF", 4},
  {"\xC2\xA0", 2}, {"\xC1\xBF", 0}, {"\xE0", 0}, {"\xF4\x8F", 0},
};

int
main (int argc, char **argv)
{
  size_t i, n = sizeof (cases) / sizeof (cases[0]);
  int bad = 0;
  FILE *f;

  if (argc != 2 || (f = fopen (argv[1], "w")) == NULL)
    {
      return 2;
    }
  fputc ('[', f);
  for (i = 0; i < n; i++)
    {
      char buf[64];
      int got = utf8_valid_len (cases[i].s);

      if (got != cases[i].valid)
	{
	  printf ("t20 FAIL: utf8_valid_len case %zu = %d, want %d\n", i, got, cases[i].valid);
	  bad = 1;
	}
      /* as a client program name: sanitized, and in JSON */
      snprintf (buf, sizeof (buf), "prog%sx", cases[i].s);
      json_fstr (f, buf);
      fputc (',', f);
      text_sanitize (buf);
      json_fstr (f, buf);
      fputc (i + 1 < n ? ',' : ']', f);
      if (cases[i].valid == 0 && strchr (buf, '?') == NULL)
	{
	  printf ("t20 FAIL: text_sanitize kept broken case %zu\n", i);
	  bad = 1;
	}
    }
  fputc ('\n', f);
  return fclose (f) != 0 || bad;
}
