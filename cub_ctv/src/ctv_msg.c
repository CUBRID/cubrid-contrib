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
 * messages - Korean/English text, die/warn, degraded features (the only writer of their bits)
 */

#include "ctv.h"

/* ------------------------------------------------------------ messages */

int use_ko = 0;
int ko_ok = 1;			/* Korean needs a UTF-8 locale */

/* first set, non-empty variable of the list, or NULL */
static const char *
env_first (const char *a, const char *b, const char *c)
{
  const char *n[3] = { a, b, c }, *v;
  int k;

  for (k = 0; k < 3; k++)
    {
      if ((v = getenv (n[k])) != NULL && v[0] != '\0')
	{
	  return v;
	}
    }
  return NULL;
}

/* the messages are UTF-8: Korean only on a UTF-8 locale (by the variable,
 * not setlocale - a static binary may not load the host's locales).
 * Unset, C and POSIX name no charset and are taken as UTF-8; a locale
 * without a codeset (ko_KR) is its legacy one (EUC-KR) */
static int
charset_utf8 (const char *v)
{
  const char *dot;

  if (v == NULL || strcmp (v, "C") == 0 || strcmp (v, "POSIX") == 0)
    {
      return 1;
    }
  dot = strchr (v, '.');
  return dot != NULL && (strncasecmp (dot + 1, "utf8", 4) == 0
			 || strncasecmp (dot + 1, "utf-8", 5) == 0);
}

/* message language before the arguments are parsed: the locale, then
 * --ko / --en anywhere (the last wins); Korean asked on a locale that is
 * not UTF-8 is refused with one warning */
void
lang_init (int argc, char **argv)
{
  const char *lang = env_first ("LC_ALL", "LC_MESSAGES", "LANG");
  int i, want;

  ko_ok = charset_utf8 (env_first ("LC_ALL", "LC_CTYPE", "LANG"));
  want = lang != NULL && strncasecmp (lang, "ko", 2) == 0;
  for (i = 1; i < argc; i++)
    {
      want = strcmp (argv[i], "--ko") == 0 ? 1 : strcmp (argv[i], "--en") == 0 ? 0 : want;
    }
  use_ko = want && ko_ok;
  if (want && !ko_ok)
    {
      warn ("charset is not UTF-8: messages in English (Korean needs a UTF-8 locale)",
	    "문자셋이 UTF-8 이 아님: 영문 메시지 (한국어는 UTF-8 로캘 필요)");
    }
}

/* the language key of the footers, when Korean is available */
const char *
lang_key (void)
{
  return ko_ok ? M ("[l] KO/EN  ", "[l] 한/영  ") : "";
}

/* degraded bits: the layout line and the detail dump both name them here */
const DEG_NAME deg_names[] = {
  {DEG_TRID, "I-8", "trid not validated"},
  {DEG_TRAN_CAP, "cap", "tran table capped"},
  {DEG_TRAN, "I-6/I-7", "no transaction table"},
  {DEG_THREAD, "I-14", "no thread entries"},
  {DEG_THR_TRAN, "I-15", "no tran"},
  {DEG_THR_ID, "I-16", "no owner id"},
  {DEG_THR_TID, "I-17", "no os tid"},
  {DEG_THR_RESUME, "I-18", "no wait reason"},
  {DEG_THR_NEXT, "I-19", "no wait chain"},
  {DEG_THR_STATUS, "I-20", "no thread state"},
  {0, NULL, NULL}
};

void
die (const char *fmt_en, const char *fmt_ko, const char *arg)
{
  /* the argument comes from outside (a path, a db name, procfs text):
   * shown sanitized, like every other outside string */
  char safe[4096];

  snprintf (safe, sizeof (safe), "%s", arg ? arg : "");
  text_sanitize (safe);
  /* leave the alternate screen first, or atexit wipes the message */
  screen_restore ();
  fprintf (stderr, "cub_ctv: ");
  fprintf (stderr, M (fmt_en, fmt_ko), safe);
  fprintf (stderr, "\n");
  exit (1);
}

void
warn (const char *en, const char *ko)
{
  fprintf (stderr, "cub_ctv: %s\n", M (en, ko));
}

/* warn with arguments; string arguments from outside are passed sanitized */
void
warnf (const char *fmt_en, const char *fmt_ko, ...)
{
  va_list ap;

  va_start (ap, fmt_ko);
  fprintf (stderr, "cub_ctv: ");
  vfprintf (stderr, M (fmt_en, fmt_ko), ap);
  fprintf (stderr, "\n");
  va_end (ap);
}

/* ------------------------------------------------- refusal / degradation */

/* invariant that disabled the transaction table (DEG_TRAN) */
const char *tran_refused = NULL;

/* switch degraded features on or off (bits: DEG_*); the only writer */
void
deg_set (unsigned bits, int on)
{
  degraded = on ? degraded | bits : degraded & ~bits;
}

/* features switched off because their invariant did not hold */
unsigned degraded = 0;
