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
