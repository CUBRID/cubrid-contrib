/*
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
 * volmap_standalone.cpp - single-binary launcher for the volmap utility
 *
 * Reuses volmap.c verbatim (compiled and linked together) but provides tiny local
 * implementations of the handful of framework symbols it needs, so the result links
 * against NOTHING from the CUBRID libraries.  Direct volume-file analysis works on
 * any 10.1+ volume, online or offline.
 *
 * Build (see tools/build_standalone.sh):
 *   g++ -std=gnu++17 -O2 -static volmap.c volmap_standalone.cpp -o volmap ...
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "utility.h"
#include "databases_file.h"

#include "volmap_options.h"   /* option codes, independent of the tree's utility.h */

/* ── minimal option store compatible with utility_get_option_* ─────────── */

#define VS_MAX_OPTS 64
static struct
{
  int key;
  const char *sval;
  INT64 ival;
  bool bval;
} vs_opts[VS_MAX_OPTS];
static int vs_nopts = 0;
static const char *vs_db_arg = NULL;
static bool vs_help = false;

static void
vs_set (int key, const char *sval, INT64 ival, bool bval)
{
  if (vs_nopts < VS_MAX_OPTS)
    {
      vs_opts[vs_nopts].key = key;
      vs_opts[vs_nopts].sval = sval;
      vs_opts[vs_nopts].ival = ival;
      vs_opts[vs_nopts].bval = bval;
      vs_nopts++;
    }
}

static int
vs_find (int key)
{
  for (int i = 0; i < vs_nopts; i++)
    {
      if (vs_opts[i].key == key)
	{
	  return i;
	}
    }
  return -1;
}

/* framework symbol replacements (normally in libcubridcs/sa) */
int
utility_get_option_int_value (UTIL_ARG_MAP * arg_map, int arg_ch)
{
  int i = vs_find (arg_ch);
  (void) arg_map;
  if (i >= 0)
    {
      return (int) vs_opts[i].ival;
    }
  if (arg_ch == VOLMAP_WIDTH_S)
    {
      return 0;			/* 0 = auto (terminal width) */
    }
  if (arg_ch == VOLMAP_ROWS_S)
    {
      return 20;
    }
  return 0;
}

bool
utility_get_option_bool_value (UTIL_ARG_MAP * arg_map, int arg_ch)
{
  int i = vs_find (arg_ch);
  (void) arg_map;
  return (i >= 0) ? vs_opts[i].bval : false;
}

char *
utility_get_option_string_value (UTIL_ARG_MAP * arg_map, int arg_ch, int index)
{
  (void) arg_map;
  if (arg_ch == OPTION_STRING_TABLE)
    {
      return (index == 0) ? (char *) vs_db_arg : NULL;
    }
  int i = vs_find (arg_ch);
  return (i >= 0) ? (char *) vs_opts[i].sval : NULL;
}

int
utility_get_option_string_table_size (UTIL_ARG_MAP * arg_map)
{
  (void) arg_map;
  return (vs_db_arg != NULL) ? 1 : 0;
}

/* ── databases.txt resolution without libcubrid ─────────────────────────── */

int
cfg_read_directory (DB_INFO ** info_p, bool write_flag)
{
  char path[4096];
  const char *dbs = getenv ("CUBRID_DATABASES");
  FILE *fp;
  char line[4096];
  DB_INFO *head = NULL, *tail = NULL;

  (void) write_flag;
  *info_p = NULL;
  if (dbs == NULL)
    {
      const char *cub = getenv ("CUBRID");
      if (cub == NULL)
	{
	  return -1;
	}
      snprintf (path, sizeof (path), "%s/databases/databases.txt", cub);
    }
  else
    {
      snprintf (path, sizeof (path), "%s/databases.txt", dbs);
    }
  fp = fopen (path, "r");
  if (fp == NULL)
    {
      return -1;
    }
  while (fgets (line, sizeof (line), fp) != NULL)
    {
      char name[1024], vol[2048];
      if (line[0] == '#' || sscanf (line, " %1023s %2047s", name, vol) != 2)
	{
	  continue;
	}
      DB_INFO *di = (DB_INFO *) calloc (1, sizeof (DB_INFO));
      di->name = strdup (name);
      di->pathname = strdup (vol);
      di->next = NULL;
      if (tail == NULL)
	{
	  head = tail = di;
	}
      else
	{
	  tail->next = di;
	  tail = di;
	}
    }
  fclose (fp);
  *info_p = head;
  return (head != NULL) ? 0 : -1;
}

DB_INFO *
cfg_find_db_list (DB_INFO * dir_info_p, const char *name)
{
  for (DB_INFO * di = dir_info_p; di != NULL; di = di->next)
    {
      if (strcmp (di->name, name) == 0)
	{
	  return di;
	}
    }
  return NULL;
}

void
cfg_free_directory (DB_INFO * databases)
{
  while (databases != NULL)
    {
      DB_INFO *next = databases->next;
      free (databases->name);
      free (databases->pathname);
      free (databases);
      databases = next;
    }
}

/* ── entry point ────────────────────────────────────────────────────────── */

extern int volmap (UTIL_FUNCTION_ARG * arg);

int
main (int argc, char **argv)
{
  static struct option longopts[] = {
    {VOLMAP_OUTPUT_FILE_L, 1, 0, VOLMAP_OUTPUT_FILE_S},
    {VOLMAP_WIDTH_L, 1, 0, VOLMAP_WIDTH_S},
    {VOLMAP_ROWS_L, 1, 0, VOLMAP_ROWS_S},
    {VOLMAP_DEEP_L, 0, 0, VOLMAP_DEEP_S},
    {VOLMAP_FULL_SWEEP_L, 0, 0, VOLMAP_FULL_SWEEP_S},
    {VOLMAP_NO_OVERLAY_L, 0, 0, VOLMAP_NO_OVERLAY_S},
    {VOLMAP_PLAIN_L, 0, 0, VOLMAP_PLAIN_S},
    {VOLMAP_VOLUME_L, 1, 0, VOLMAP_VOLUME_S},
    {VOLMAP_WIDE_L, 0, 0, VOLMAP_WIDE_S},
    {VOLMAP_FULL_L, 0, 0, VOLMAP_FULL_S},
    {VOLMAP_INTERACTIVE_L, 0, 0, VOLMAP_INTERACTIVE_S},
    {VOLMAP_RESIDENCY_L, 0, 0, VOLMAP_RESIDENCY_S},
    {VOLMAP_BUFMAP_L, 1, 0, VOLMAP_BUFMAP_S},
    {VOLMAP_CHECK_L, 0, 0, VOLMAP_CHECK_S},
    {VOLMAP_FORMAT_L, 1, 0, VOLMAP_FORMAT_S},
    {VOLMAP_TICK_L, 1, 0, VOLMAP_TICK_S},
    {VOLMAP_WARN_IDLE_L, 1, 0, VOLMAP_WARN_IDLE_S},
    {VOLMAP_TEMP_PATH_L, 1, 0, VOLMAP_TEMP_PATH_S},
    {VOLMAP_NO_TRACKER_L, 0, 0, VOLMAP_NO_TRACKER_S},
    {VOLMAP_HELP_L, 0, 0, VOLMAP_HELP_S},
    {0, 0, 0, 0}
  };
  UTIL_FUNCTION_ARG arg;
  int opt;

  while ((opt = getopt_long (argc, argv, "o:w:r:V:B:fimh", longopts, NULL)) != -1)
    {
      switch (opt)
	{
	case VOLMAP_OUTPUT_FILE_S:
	case VOLMAP_VOLUME_S:
	case VOLMAP_FORMAT_S:
	case VOLMAP_BUFMAP_S:
	case VOLMAP_TEMP_PATH_S:
	  vs_set (opt, optarg, 0, false);
	  break;
	case VOLMAP_WIDTH_S:
	case VOLMAP_ROWS_S:
	case VOLMAP_TICK_S:
	case VOLMAP_WARN_IDLE_S:
	  vs_set (opt, NULL, atoll (optarg), false);
	  break;
	case '?':
	  /* getopt_long has already said which option it disliked (unrecognized, or
	     a missing required argument).  Stopping here matters most for --check
	     and --warn-idle: running on and exiting 0 would tell a caller that
	     watches only the exit code that the database passed, when in fact the
	     requested check never ran. */
	  fprintf (stderr, "volmap: try --help for the list of options\n");
	  return EXIT_FAILURE;
	case VOLMAP_HELP_S:
	  vs_help = true;
	  break;
	default:
	  vs_set (opt, NULL, 0, true);
	  break;
	}
    }
  if (vs_help)
    {
      memset (&arg, 0, sizeof (arg));
      arg.command_name = "volmap";
      arg.argv0 = argv[0];
      (void) volmap (&arg);	/* no database argument: prints usage */
      return EXIT_SUCCESS;
    }
  if (optind < argc)
    {
      vs_db_arg = argv[optind];
    }
  if (optind + 1 < argc)
    {
      /* one database per run; a second one would otherwise be dropped in silence */
      fprintf (stderr, "volmap: unexpected argument '%s' - one database per run\n", argv[optind + 1]);
      return EXIT_FAILURE;
    }

  memset (&arg, 0, sizeof (arg));
  arg.command_name = "volmap";
  arg.argv0 = argv[0];
  return volmap (&arg);
}
