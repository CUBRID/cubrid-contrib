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
 * cub_ctv - CUBRID critical section viewer.
 *
 * Reads a live cub_server's critical sections, sync-stats pool, transaction
 * table and thread entries through /proc/<pid>/maps and read-only memory
 * reads (/proc/<pid>/mem pread; process_vm_readv for batches of small
 * ranges).  No lock in the target, no ptrace attach, no SQL connection.
 *
 * No CUBRID header is used: symbols come from the target's mapped
 * libcubrid.so and structure offsets are probed by runtime invariants
 * (I-0..I-20, ledger in PR.md).  Derived without a probe of their own: the
 * CS fields around stats (rwlock trio, owner and queues cross-checked; tran
 * unchecked), the SYNC_STATS counter offsets (fixed; type and name
 * checked), entry.tran_index and the pool chunk tail.  A probe that fails
 * REFUSEs with its id instead of guessing.
 *
 * Build:   ./build.sh
 * Usage:   cub_ctv --help
 */

#include "ctv.h"

/* ------------------------------------------------------------------- main */

static void
print_usage (void)
{
  fputs (M ("usage: cub_ctv [options] [<db-name> | <pid>]\n"
	    "\n"
	    "  (no arguments)       this help\n"
	    "  -l, --list           list running cub_server processes (pid, db)\n"
	    "  -i, --interactive    live mode: sample at --hz, print every --interval\n"
	    "                       (full screen on a tty, scrolling on a pipe);\n"
	    "                       [r] starts/stops a record <pid>_<db>_<time>.ctv,\n"
	    "                       [l] toggles the language (KO/EN), [q] quits\n"
	    "      --interval SEC   live display period (default 1, 0.1 .. 3600, at least\n"
	    "                       one sample: >= 1/--hz)\n"
	    "      --hz N           live sampling rate, 1..10000 (default 100)\n"
	    "      --detail[=NAME|IX]\n"
	    "                       one vertical record per CS (all, or one by\n"
	    "                       name/index; a word after the target works too:\n"
	    "                       cub_ctv demodb --detail LOG); with -i on a pipe,\n"
	    "                       rows that moved (a terminal shows the summary)\n"
	    "      --trans          transaction table only (no SQL connection);\n"
	    "                       with -i or a live replay, every window\n"
	    "      --wide           owner block always on the row\n"
	    "      --narrow         owner block always on indented lines\n"
	    "                       (default: split by terminal width)\n"
	    "      --plain          no ANSI colors (default: colors on a terminal);\n"
	    "                       the sessions panel keeps reverse/underline\n"
	    "      --json           JSON: one document, or one line per live window\n"
	    "      --record FILE    also write what is shown (snapshot, or every\n"
	    "                       live window) to FILE, compact text\n"
	    "      --replay FILE    show a --record file again, anywhere, no server;\n"
	    "                       view options (--detail, --trans, --json, ...)\n"
	    "                       may differ from the recording; on a terminal\n"
	    "                       keys: space pause, < > one window, + - speed,\n"
	    "                       l language, q quit\n"
	    "      --dump FILE      detail dump: everything cub_ctv can read at this\n"
	    "                       moment, as JSON (snapshot only; not replayable)\n"
	    "      --ko | --en      message language (default: LC_ALL, LC_MESSAGES,\n"
	    "                       LANG); Korean only on a UTF-8 locale\n"
	    "  -h, --help           show this help\n",
	    "사용법: cub_ctv [옵션] [<DB명> | <pid>]\n"
	    "\n"
	    "  (인자 없음)          이 도움말\n"
	    "  -l, --list           실행 중인 cub_server 목록 (pid, db)\n"
	    "  -i, --interactive    라이브 모드: --hz 로 샘플링, --interval 마다 출력\n"
	    "                       (터미널이면 전체 화면, 파이프면 스크롤);\n"
	    "                       [r] 로 <pid>_<db>_<시각>.ctv 녹화 시작/중지,\n"
	    "                       [l] 한/영 전환, [q] 종료\n"
	    "      --interval SEC   라이브 표시 주기 (기본 1, 0.1 .. 3600, 샘플 1회\n"
	    "                       이상: >= 1/--hz)\n"
	    "      --hz N           라이브 샘플링 주파수, 1..10000 (기본 100)\n"
	    "      --detail[=NAME|IX]\n"
	    "                       CS별 세로 레코드 (전체, 또는 이름/번호로 하나;\n"
	    "                       대상 뒤 단어도 됨: cub_ctv demodb --detail LOG);\n"
	    "                       파이프 -i 와 함께면 움직인 행만 (터미널은\n"
	    "                       요약 화면)\n"
	    "      --trans          트랜잭션 표만 (SQL 접속 없음); -i·라이브\n"
	    "                       재생에서는 구간마다\n"
	    "      --wide           소유자 정보를 항상 행 끝에\n"
	    "      --narrow         소유자 정보를 항상 들여쓴 줄로\n"
	    "                       (기본: 터미널 폭에 따라)\n"
	    "      --plain          ANSI 색상 없이 (기본: 터미널이면 색상); 세션 표의\n"
	    "                       반전·밑줄은 유지\n"
	    "      --json           JSON: 1문서, 라이브는 구간마다 1줄\n"
	    "      --record FILE    화면에 보인 내용(스냅샷, 또는 라이브 구간마다)을\n"
	    "                       FILE 에 함께 기록 (작은 텍스트)\n"
	    "      --replay FILE    --record 파일을 어디서나 다시 표시, 서버 불필요;\n"
	    "                       보기 옵션(--detail, --trans, --json 등)은\n"
	    "                       기록 때와 달라도 됨; 터미널 키: space 일시정지,\n"
	    "                       < > 구간 이동, + - 속도, l 한/영, q 종료\n"
	    "      --dump FILE      디테일 덤프: 이 순간 cub_ctv 가 읽을 수 있는 정보\n"
	    "                       전부를 JSON 으로 (스냅샷 전용, 재생 불가)\n"
	    "      --ko | --en      메시지 언어 (기본: LC_ALL, LC_MESSAGES, LANG);\n"
	    "                       한국어는 UTF-8 로캘에서만\n"
	    "  -h, --help           이 도움말\n"), stdout);
}

/* running servers */
static SERVER servers[SERVERS_MAX];
static int nserv = 0;

/* the target as a pid (all digits, in range), or 0 for a db name */
static long
pid_of_target (const char *target)
{
  char *end;
  long p = strtol (target, &end, 10);

  return *target != '\0' && *end == '\0' && p > 0 && p <= INT32_MAX ? p : 0;
}

/* index of the first server named by target (pid or db name), or -1 */
static int
server_match (const char *target, const SERVER * sv, int n)
{
  long want = pid_of_target (target);
  int i;

  for (i = 0; i < n; i++)
    {
      if ((want > 0 && sv[i].pid == want) || strcmp (sv[i].db, target) == 0)
	{
	  return i;
	}
    }
  return -1;
}

/* option NAME VALUE: 1 when argv[*i] is NAME (the value is consumed, or the
 * run ends with "needs a value"), 0 otherwise */
static int
opt_take (int argc, char **argv, int *i, const char *name, const char **val)
{
  if (strcmp (argv[*i], name) != 0)
    {
      return 0;
    }
  if (*i + 1 >= argc || argv[*i + 1][0] == '\0')
    {
      die ("%s needs a value", "%s 에 값이 필요합니다", name);
    }
  *val = argv[++*i];
  return 1;
}

/* a file name: not another option */
static int
opt_str (int argc, char **argv, int *i, const char *name, const char **dst)
{
  const char *v;

  if (!opt_take (argc, argv, i, name, &v))
    {
      return 0;
    }
  if (v[0] == '-')
    {
      die ("%s needs a value", "%s 에 값이 필요합니다", name);
    }
  *dst = v;
  return 1;
}

/* a number (range checks follow the loop) */
static int
opt_num (int argc, char **argv, int *i, const char *name, double *dst)
{
  const char *v;
  char *end;

  if (!opt_take (argc, argv, i, name, &v))
    {
      return 0;
    }
  *dst = strtod (v, &end);
  if (end == v || *end != '\0')
    {
      die ("%s needs a number", "%s 에 숫자가 필요합니다", name);
    }
  return 1;
}

/* an integer */
static int
opt_int (int argc, char **argv, int *i, const char *name, int *dst)
{
  const char *v;
  char *end;
  long n;

  if (!opt_take (argc, argv, i, name, &v))
    {
      return 0;
    }
  n = strtol (v, &end, 10);
  if (end == v || *end != '\0' || n < INT_MIN || n > INT_MAX)
    {
      die ("%s needs an integer", "%s 에 정수가 필요합니다", name);
    }
  *dst = (int) n;
  return 1;
}

/* the directory part of path, resolved, plus its last component: two
 * spellings of one not-yet-created file compare equal */
static void
path_canon (const char *path, char *out, size_t n)
{
  char dir[PATH_MAX], real[PATH_MAX];
  const char *slash = strrchr (path, '/');
  const char *base = slash != NULL ? slash + 1 : path;

  snprintf (dir, sizeof (dir), "%.*s", slash == NULL ? 1
	    : slash == path ? 1 : (int) (slash - path), slash == NULL ? "." : path);
  if (realpath (dir, real) == NULL)
    {
      snprintf (real, sizeof (real), "%s", dir);
    }
  snprintf (out, n, "%s/%s", real, base);
}

/* a and b name one file (existing: same inode; else same resolved path) */
static int
same_file (const char *a, const char *b)
{
  struct stat sa, sb;
  char ca[PATH_MAX + 256], cb[PATH_MAX + 256];

  if (stat (a, &sa) == 0 && stat (b, &sb) == 0)
    {
      return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    }
  path_canon (a, ca, sizeof (ca));
  path_canon (b, cb, sizeof (cb));
  return strcmp (ca, cb) == 0;
}

static void
parse_args (int argc, char **argv, OPTS * o)
{
  int i, help = 0, nword = 0;
  const char *word[2];

  memset (o, 0, sizeof (*o));
  o->hz = 100;
  o->interval = 1.0;
  o->replay_speed = 1.0;
  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "-h") == 0 || strcmp (argv[i], "--help") == 0)
	{
	  help = 1;
	}
      else if (strcmp (argv[i], "-i") == 0
	       || strcmp (argv[i], "--interactive") == 0
	       || strcmp (argv[i], "--live") == 0)	/* former name */
	{
	  o->live = 1;
	}
      else if (strcmp (argv[i], "-l") == 0 || strcmp (argv[i], "--list") == 0)
	{
	  o->list = 1;
	}
      else if (strcmp (argv[i], "--trans") == 0)
	{
	  o->trans = 1;
	}
      else if (strcmp (argv[i], "--json") == 0)
	{
	  o->json = 1;
	}
      else if (strcmp (argv[i], "--detail") == 0)
	{
	  layout_detail = 1;	/* the filter: --detail=NAME, or the 2nd bare word */
	}
      else if (strncmp (argv[i], "--detail=", 9) == 0 && argv[i][9] != '\0')
	{
	  layout_detail = 1;
	  o->detail_filter = argv[i] + 9;
	}
      else if (strcmp (argv[i], "--pool") == 0)
	{
	  ;			/* former option: the pool is always shown */
	}
      else if (strcmp (argv[i], "--plain") == 0)
	{
	  o->plain = 1;
	}
      else if (strcmp (argv[i], "--wide") == 0)
	{
	  layout_mode = LAYOUT_WIDE;
	}
      else if (strcmp (argv[i], "--narrow") == 0)
	{
	  layout_mode = LAYOUT_NARROW;
	}
      else if (opt_str (argc, argv, &i, "--dump", &o->dump)
	       || opt_str (argc, argv, &i, "--replay", &o->replay)
	       || opt_str (argc, argv, &i, "--record", &o->record)
	       || opt_num (argc, argv, &i, "--replay-speed", &o->replay_speed)
	       || opt_num (argc, argv, &i, "--interval", &o->interval)
	       || opt_int (argc, argv, &i, "--hz", &o->hz))
	{
	  ;
	}
      else if (strcmp (argv[i], "--ko") == 0)
	{
	  use_ko = ko_ok;
	}
      else if (strcmp (argv[i], "--en") == 0)
	{
	  use_ko = 0;
	}
      else if (argv[i][0] != '-' && nword < 2)
	{
	  word[nword++] = argv[i];	/* placed after the loop */
	}
      else if (argv[i][0] != '-')
	{
	  die ("unexpected extra argument: %s (target already given)",
	       "잉여 인자: %s (대상은 이미 지정됨)", argv[i]);
	}
      else
	{
	  die ("unknown option: %s (see --help)",
	       "알 수 없는 옵션: %s (--help 참고)", argv[i]);
	}
    }
  /* bare words by position: the target, then the --detail filter;
   * --replay has no target, so there the first word is the filter */
  {
    int w = 0;

    if (o->replay == NULL && w < nword)
      {
	o->target = word[w++];
      }
    if (w < nword && layout_detail && o->detail_filter == NULL)
      {
	o->detail_filter = word[w++];
      }
    if (w < nword)
      {
	die (o->replay != NULL ? "--replay takes no target: %s"
	     : "unexpected extra argument: %s (target already given)",
	     o->replay != NULL ? "--replay 에는 대상을 주지 않습니다: %s"
	     : "잉여 인자: %s (대상은 이미 지정됨)", word[w]);
      }
  }
  /* no arguments (or only --ko/--en) or -h: the help.  Options without a
   * target are a mistake (a forgotten or empty operand), not a request */
  if (o->target == NULL && !o->list && o->replay == NULL && !help)
    {
      int k, only_lang = 1;

      for (k = 1; k < argc; k++)
	{
	  only_lang &= strcmp (argv[k], "--ko") == 0 || strcmp (argv[k], "--en") == 0;
	}
      if (!only_lang)
	{
	  die ("no target: give a db name or a pid (-l lists them, -h for help)",
	       "대상 없음: DB 이름 또는 pid 를 주십시오 (-l 목록, -h 도움말)", NULL);
	}
      help = 1;
    }
  if (help)
    {
      print_usage ();
      exit (0);
    }
  if (o->replay == NULL	/* a replay takes both from the record */
      && (o->hz < 1 || o->hz > 10000 || !(o->interval >= INTERVAL_MIN)
	  || !(o->interval <= INTERVAL_MAX) || o->interval * o->hz < 1.0))
    {
      die ("bad --hz/--interval", "--hz/--interval 값 오류", NULL);
    }
  if (!(o->replay_speed >= 0.0) || !isfinite (o->replay_speed))
    {
      die ("bad --replay-speed (0 = no pacing, or a positive factor)",
	   "--replay-speed 값 오류 (0 = 대기 없음, 또는 양수 배율)", NULL);
    }
  if (o->live && o->dump != NULL)
    {
      die ("--dump writes one snapshot; it cannot be combined with -i/--interactive",
	   "--dump는 스냅샷 1회를 기록합니다; -i/--interactive 와 함께 쓸 수 없습니다", NULL);
    }
  if (o->dump != NULL && o->record != NULL && same_file (o->dump, o->record))
    {
      die ("--dump and --record name the same file: %s",
	   "--dump 와 --record 가 같은 파일입니다: %s", o->dump);
    }
  if (o->replay != NULL
      && (o->live || o->dump != NULL || o->record != NULL))
    {
      die ("--replay cannot be combined with -i/--interactive, --dump or --record",
	   "--replay는 -i/--interactive/--dump/--record와 함께 쓸 수 없습니다", NULL);
    }
}

/* target is a whole pid number or an exact db name */
static int
resolve_target (const char *target, const SERVER * sv, int n, int *pid,
		const char **db)
{
  int found = server_match (target, sv, n), dup = 0, i;
  long want = pid_of_target (target);

  /* a db name served by several servers: the first wins, a pid selects */
  for (i = found + 1; found >= 0 && want == 0 && i < n; i++)
    {
      dup += strcmp (sv[i].db, target) == 0;
    }
  if (found < 0)
    {
      /* a pid past the listed servers (a very busy host) */
      static SERVER one;

      if (want > 0 && server_by_pid ((int) want, &one) == 0)
	{
	  *pid = one.pid;
	  *db = one.db;
	  return 0;
	}
      return -1;
    }
  if (dup > 0)
    {
      char safe[256];

      snprintf (safe, sizeof (safe), "%s", target);
      text_sanitize (safe);
      warnf ("'%s': %d servers; using pid %d (give a pid to choose)",
	     "'%s' 서버가 %d개 있어 pid %d 를 사용합니다 (선택하려면 pid 지정)",
	     safe, dup + 1, sv[found].pid);
    }
  *pid = sv[found].pid;
  *db = sv[found].db;
  return 0;
}

/* mapped library path, load bias and the symbols we need */
static void
locate_lib (int pid, char *lib, size_t libn, uint64_t * bias,
	    uint64_t * sym_cs, uint64_t * sym_pool, uint64_t * sym_loggl,
	    uint64_t * sym_thr)
{
  uint64_t lib_base = 0, load0 = 0, dummy;
  char range[64], file[4352];
  const char *src;
  int r;

  /* the file the target mapped: through its own root (another mount
   * namespace / container), or map_files when it was replaced on disk */
  switch (find_lib_base (pid, &lib_base, lib, libn, range, sizeof (range)))
    {
    case LIB_OK:
      snprintf (file, sizeof (file), "/proc/%d/root%s", pid, lib);
      break;
    case LIB_DELETED:
      snprintf (file, sizeof (file), "/proc/%d/map_files/%s", pid, range);
      break;
    case LIB_NOMAPS:
      die ("cannot read /proc/<pid>/maps: %s (run as the server's OS user or root)",
	   "/proc/<pid>/maps 판독 실패: %s (서버의 OS 사용자 또는 root 로 실행)",
	   strerror (errno));
      break;
    default:
      die ("libcubrid.so not mapped in target",
	   "대상에 libcubrid.so가 매핑되어 있지 않습니다", NULL);
    }
  src = file;
  r = elf_lookup (src, "csectgl_Critical_sections", sym_cs, &load0);
  if (r == ELF_NOFILE && access (file, F_OK) != 0 && strstr (file, "/root/") != NULL)
    {
      src = lib;		/* /proc/<pid>/root not usable here: the path itself */
      r = elf_lookup (src, "csectgl_Critical_sections", sym_cs, &load0);
    }
  switch (r)
    {
    case ELF_OK:
      break;
    case ELF_NOFILE:
      if (strstr (file, "/map_files/") != NULL)
	{
	  die ("mapped libcubrid.so was deleted/replaced on disk and its "
	       "mapping is not readable (map_files needs root) - restart the "
	       "server or run as root",
	       "매핑된 libcubrid.so가 디스크에서 교체/삭제되었고 매핑도 읽을 수 "
	       "없습니다(map_files 는 root 필요) - 서버 재시작 또는 root 로 실행",
	       NULL);
	}
      die ("cannot open %s (permission? replaced?)",
	   "%s 열기 실패 (권한? 교체됨?)", lib);
    case ELF_BAD:
      die ("%s is not a sane ELF64 image (truncated/corrupt) - refusing",
	   "%s가 정상 ELF64가 아님(절단/손상) - 중단", lib);
    default:
      die ("csectgl_Critical_sections not found in %s - unsupported build",
	   "%s에서 csectgl_Critical_sections 심볼 없음 - 미지원 빌드", lib);
    }
  *bias = lib_base - load0;
  /* optional symbols: their views degrade gracefully when absent */
  if (elf_lookup (src, "sync_Stats", sym_pool, &dummy) != ELF_OK)
    {
      *sym_pool = 0;
    }
  if (elf_lookup (src, "log_Gl", sym_loggl, &dummy) != ELF_OK)
    {
      *sym_loggl = 0;
    }
  /* cubthread::Manager is a file-static: .symtab only (absent if stripped) */
  if (elf_lookup (src, "_ZN9cubthreadL7ManagerE", sym_thr, &dummy) != ELF_OK)
    {
      *sym_thr = 0;
    }
}

/* transaction table: when it does not validate, identity views degrade
 * (DEG_TRAN) and the reason is kept for the layout line */
static void
tran_subsystem_init (uint64_t bias, uint64_t sym_loggl)
{
  const char *why = sym_loggl == 0 ? "log_Gl symbol" : tran_probe (bias + sym_loggl);

  if (why == NULL)
    {
      (void) tran_lock_clientids (tran_geom.ntran, 0);
    }
  if (tran_geom.all_tdes == 0)
    {
      deg_set (DEG_TRAN, 1);
      tran_refused = why != NULL ? why : "transaction table probe";
    }
}

static int thr_skipped = 0;	/* --trans alone (no record, no dump): the
				 * transaction table only, thread entries not
				 * probed */

/* threads: the entry count, or -1 when the entries were not probed */
static void
print_layout_line (const CS_LAYOUT * lo, int threads)
{
  int d;

  printf (M ("layout: stride=%dB count=%d stats_off=%d (runtime-probed)",
	     "레이아웃: stride=%dB count=%d stats_off=%d (런타임 검증 통과)"),
	  lo->stride, lo->count, lo->off_stats);
  if (threads >= 0 && !(degraded & DEG_THREAD))
    {
      printf ("  threads=%d", threads);
    }
  /* every degraded feature, named from the one table the dump also uses */
  for (d = 0; deg_names[d].bit != 0; d++)
    {
      if (!(degraded & deg_names[d].bit))
	{
	  continue;
	}
      printf (" [%s (%s)", deg_names[d].label, deg_names[d].id);
      if (deg_names[d].bit == DEG_TRAN && tran_refused != NULL)
	{
	  printf (": %s", tran_refused);
	}
      if (deg_names[d].bit == DEG_TRAN_CAP)
	{
	  printf (": %d", TRAN_SLOTS_MAX);
	}
      printf ("]");
    }
  printf ("\n");
}

/* everything after the frame is captured: the same path for a live run and
 * a replay */
static int
render_snapshot (const OPTS * o, const CS_LAYOUT * lo, int pid,
		 const char *db)
{
  if (o->trans)
    {
      /* --trans asks for the table alone: no table is a failure (exit 1) */
      if (o->json)
	{
	  json_doc (pid, db, lo, 0, 0, 1);
	  return view.tran_avail ? 0 : 1;
	}
      return print_trans () == 0 ? 0 : 1;
    }
  if (o->json)
    {
      json_doc (pid, db, lo, 0, 1, 1);
      return 0;
    }
  if (layout_detail)
    {
      print_detail (lo, view.rows);
      return 0;
    }
  print_snapshot (lo, view.rows);
  print_thread_waits (stdout, 0);
  printf ("\n");
  (void) print_trans ();
  print_view_pool ();
  return 0;
}

static int
replay_main (const OPTS * o)
{
  VIEW_HDR h;

  view_replay_open (o->replay, &h);
  deg_set (~0u, 0);		/* a replay shows the recorded bits */
  deg_set (h.degraded, 1);
  tran_refused = h.tran_refused[0] != '\0' ? h.tran_refused : NULL;
  if (!o->json)
    {
      printf (M ("target: pid=%d db=%s lib=%s\n",
		 "대상: pid=%d db=%s lib=%s\n"), h.pid, h.db, h.lib);
      /* from the header: the probe geometry of this process stays unset */
      print_layout_line (&h.lo, h.thr_count);
    }
  if (h.live)
    {
      return live_replay (&h, o->replay_speed, o->json, o->trans);
    }
  if (view_replay_next () != 0)
    {
      die ("record %s holds no frame", "기록 %s 에 프레임이 없습니다",
	   o->replay);
    }
  return render_snapshot (o, &h.lo, h.pid, h.db);
}

int
main (int argc, char **argv)
{
  OPTS o;
  int i, pid = -1;
  const char *db = "?";
  uint64_t bias = 0, sym_cs = 0, sym_pool = 0, sym_loggl = 0, sym_thr = 0;
  char lib[4096], slib[4096];
  CS_LAYOUT lo;
  CS_ROW rows[MAX_CS];
  VIEW_HDR h;

  lang_init (argc, argv);	/* before any argument error is reported */
  /* large read buffers: keep THP off so allocations never wait for
   * kernel compaction */
  (void) prctl (PR_SET_THP_DISABLE, 1, 0, 0, 0);
  parse_args (argc, argv, &o);
  color_on = !o.plain && !o.json && isatty (STDOUT_FILENO);
  detail_filter = o.detail_filter;

  if (o.replay != NULL)
    {
      return replay_main (&o);
    }
  nserv = list_servers (servers, SERVERS_MAX);
  if (nserv == SERVERS_MAX)
    {
      warn ("more cub_server processes than can be listed: give a pid if yours is missing",
	    "cub_server 프로세스가 목록 한도를 넘음: 없으면 pid 로 지정하십시오");
    }
  if (o.list || o.target == NULL)
    {
      if (nserv == 0)
	{
	  die ("no cub_server process found",
	       "cub_server 프로세스를 찾지 못했습니다", NULL);
	}
      printf (M ("candidate cub_server processes:\n",
		 "cub_server 후보 프로세스:\n"));
      for (i = 0; i < nserv; i++)
	{
	  printf ("  %8d  %s\n", servers[i].pid, servers[i].db);
	}
      return 0;
    }
  if (resolve_target (o.target, servers, nserv, &pid, &db) != 0)
    {
      if (o.detail_filter != NULL
	  && server_match (o.detail_filter, servers, nserv) >= 0)
	{
	  /* the words read as target then filter; say so, do not swap */
	  warn ("bare words are the target then the --detail filter: "
		"use --detail=NAME, or put the target first",
		"단어는 대상, 그다음 --detail 필터 순입니다: "
		"--detail=NAME 을 쓰거나 대상을 먼저 주십시오");
	}
      die ("no cub_server process found (%s)",
	   "cub_server 프로세스를 찾지 못했습니다 (%s)", o.target);
    }
  locate_lib (pid, lib, sizeof (lib), &bias, &sym_cs, &sym_pool, &sym_loggl,
	      &sym_thr);
  if (mem_open (pid) != 0)
    {
      die ("cannot open /proc/<pid>/mem: %s",
	   "/proc/<pid>/mem 열기 실패: %s", strerror (errno));
    }

  if (!o.json)
    {
      snprintf (slib, sizeof (slib), "%s", lib);
      text_sanitize (slib);
      printf (M ("target: pid=%d db=%s lib=%s\n",
		 "대상: pid=%d db=%s lib=%s\n"), pid, db, slib);
    }
  {
    const char *why = cs_layout_probe (bias + sym_cs, &lo);

    if (why != NULL)
      {
	die ("cannot validate SYNC_CRITICAL_SECTION layout (%s) - refusing to guess",
	     "SYNC_CRITICAL_SECTION 레이아웃 검증 실패 (%s) - 추측하지 않고 중단",
	     why);
      }
  }
  tran_subsystem_init (bias, sym_loggl);
  target_pid = pid;
  /* the transaction table alone needs no thread entries */
  thr_skipped = TRANS_TABLE_ONLY (o.trans, o.record) && o.dump == NULL;
  if (!thr_skipped)
    {
      thr_keep_image = o.dump != NULL;
      thr_probe (sym_thr != 0 ? bias + sym_thr : 0, pid);
    }
  if (!o.json)
    {
      print_layout_line (&lo, thr_skipped ? -1 : thr_geom.count);
    }

  memset (&h, 0, sizeof (h));
  h.pid = pid;
  h.live = o.live;
  h.hz = o.hz;
  h.interval = o.interval;
  h.thr_count = thr_geom.count;
  h.degraded = degraded;
  h.lo = lo;
  snprintf (h.db, sizeof (h.db), "%s", db);
  snprintf (h.lib, sizeof (h.lib), "%s", lib);
  snprintf (h.tran_refused, sizeof (h.tran_refused), "%s",
	    tran_refused != NULL ? tran_refused : "");
  if (o.record != NULL && view_record_open (o.record, &h, 0) != 0)
    {
      die ("cannot create record %s", "기록 파일 %s 생성 실패", o.record);
    }

  if (o.live)
    {
      return live_mode (bias + sym_cs, &h, o.json, o.trans, o.record, thr_skipped);
    }
  if (thr_skipped)
    {
      view_capture_trans (0.0);	/* the table alone, as in live mode */
      return render_snapshot (&o, &lo, pid, db);
    }
  if (cs_read_rows (bias + sym_cs, &lo, rows) != 0)
    {
      die_target_gone ();
    }
  view_capture (&lo, rows, NULL, 0.0);
  if (o.dump != NULL)
    {
      /* its pool walk also fills the frame's groups */
      detail_dump_write (o.dump, &h, bias, sym_cs, sym_pool, sym_loggl,
			 sym_thr);
    }
  else if (sym_pool != 0
	   && (o.record != NULL || (!o.trans && !o.json && !layout_detail)))
    {
      view_capture_pool (bias + sym_pool);	/* only where shown or recorded */
    }
  view_record_frame ();
  if (o.record != NULL && view_record_close () != 0)
    {
      die ("cannot write record %s", "기록 파일 %s 쓰기 실패", o.record);
    }
  return render_snapshot (&o, &lo, pid, db);
}
