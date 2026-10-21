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
 * volmap.c - volume file mapping viewer utility
 *
 * Renders an ANSI terminal map of permanent volumes: which file (object) owns each sector, how densely
 * sectors are allocated/used, and how fragmented the free space is.
 *
 * Reads volume binary files directly using the compiled on-disk layout structures
 * (storage_ondisk_layout.hpp).  It takes no locks, opens no transaction and never
 * contacts the server process, so it works identically whether the database is
 * online or offline.  Buffer-pool state, when wanted, comes from a cub_top
 * --bcb-dump snapshot file (--bufmap), not from the server.
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <dirent.h>
#include <libgen.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "utility.h"
#include "volmap_options.h"   /* option codes, independent of the tree's utility.h */
#include "databases_file.h"
#include "environment_variable.h"
#include "message_catalog.h"
#include "oid.h"
#include "object_representation_constants.h"
#include "file_io.h"
#include "storage_ondisk_layout.hpp"
#if !defined (VOLMAP_STANDALONE)
#include "db.h"
#include "dbtype.h"
#endif

/* highest volid the engine issues: LOG_MAX_DBVOLID = VOLID_MAX - 1 = SHRT_MAX - 1.
   Temp volumes count down from here, so -V must cover the whole range. */
#define VOLMAP_MAX_VOLID  32766
/* the vinf entry naming the active log: LOG_DBLOG_ACTIVE_VOLID (log_volids.hpp) */
#define VOLMAP_LGAT_VOLID (-2)
/* highest major release whose layout this tool knows; a larger one reads as the
   newest layout, which is what VOLMAP_VLAY_114 already means */
#define VOLMAP_MAX_KNOWN_MAJOR 99
/* pages sampled to decide whether the page watermark is present (volmap_probe_watermark) */
#define VOLMAP_WM_SAMPLES     32
#define VOLMAP_WM_MIN_SAMPLES 4
#define VOLMAP_MAX_FILES  65536
#define VOLMAP_SECT_NPAGES DISK_SECTOR_NPAGES

/* ANSI helpers */
#define VM_RESET "\033[0m"
#define VM_BOLD  "\033[1m"
#define VM_DIM   "\033[2m"

typedef struct volmap_volume VOLMAP_VOLUME;
struct volmap_volume
{
  /* Allocated to the length actually needed.  As an inline char[PATH_MAX] this one
     field was 4096 of the struct's 4384 bytes - 93% - while real volume paths run
     about 50.  Making it a pointer is what lets the volume array grow on demand
     without the array itself becoming the expensive part. */
  char *path;
  int fd;
  INT16 volid;
  INT16 iopagesize;
  int user_size;		/* DB page (user area) size */
  DKNSECTS nsect_total;
  DKNPAGES stab_npages;
  PAGEID stab_first;
  PAGEID sys_lastpage;		/* end of the volume system area (header + sector table) */
  long tde_pages;		/* distinct pages seen with the TDE-encrypted flag (probe sample) */
  UINT64 *tde_bm;		/* sector -> which of its pages are already counted in tde_pages;
				   a page can be probed more than once (--full-sweep pass B
				   re-probes pass A's first page, and every refresh re-probes),
				   so the count needs the bitmap to stay a page count */
  PAGEID hwm_page;		/* highest allocated page = high-water mark (display marker) */
  DB_VOLPURPOSE purpose;
  unsigned char *stab;		/* 1 byte per sector: reserved bit */
  int *owner;			/* sector -> discovered file index, -1 unknown */
  int *alloc;			/* sector -> allocated page count (L2) */
  UINT64 *pagebm;		/* sector -> page allocation bitmap (LSB = first page) */
  /* write-side pointers used by DISCOVERY code (scan/refresh/batch); normally
   * aliases of the read-side arrays above.  During a threaded refresh the
   * batch worker points them at the sh_* shadow set, rebuilds there, then
   * swaps the read side - the UI thread never sees a half-rebuilt map. */
  unsigned char *w_stab;
  int *w_owner;
  int *w_alloc;
  UINT64 *w_pagebm;
  unsigned char *sh_stab;	/* spare buffer set for the next shadow rebuild */
  int *sh_owner;
  int *sh_alloc;
  UINT64 *sh_pagebm;
  unsigned char *respg;		/* DB page -> resident OS-subpage count (residency mode) */
  unsigned int *res_prefix;	/* prefix sums of respg[] (npages+1): O(1) per-cell residency */
  long res_total;		/* sum of respg[] (cached by volmap_read_residency) */
  bool res_untrusted;		/* mincore would report fake data for this file (volmap_mincore_trusted) */
  /* --bufmap: buffer-pool occupancy per DB page from the cub_top snapshot */
  unsigned char *bufpg;		/* 0 not in buffer pool, 1 resident clean, 2 resident dirty */
  unsigned int *buf_prefix;	/* prefix sums of (bufpg != 0)  (npages+1) */
  unsigned int *dirty_prefix;	/* prefix sums of (bufpg == 2)  (npages+1) */
  long buf_total;		/* pages of this volume in the buffer pool */
  long buf_dirty;		/* ... of which unflushed: DIRTY or FLUSHING (not yet on the volume) */
  long buf_flushing;		/* ... of which FLUSHING (write in progress at the snapshot) */
  long buf_freed;		/* ... buffered but not allocated on disk (deallocated pages linger in BCBs) */
  /* --deep results */
  INT64 deep_free_bytes;	/* sum of spage total_free over data pages */
  INT64 deep_data_pages;
  INT64 deep_recs;
  INT64 deep_fwd_slots;		/* REC_RELOCATION/REC_NEWHOME slots */
  INT64 deep_slots;
  INT64 deep_tde_skipped;	/* encrypted pages left out of the deep figures */
  INT64 deep_cache_returned;	/* bytes returned to the OS after the deep scan (FADV_DONTNEED) */
  INT64 deep_cache_kept;	/* bytes kept because they were already cached before the scan */
};

/* one sector a file's sector tables list, as the last walk saw it */
typedef struct volmap_sectref
{
  INT16 volid;
  INT32 sectid;
  UINT64 bitmap;		/* allocated pages (all ones for a full sector) */
} VOLMAP_SECTREF;

typedef struct volmap_file VOLMAP_FILE;
struct volmap_file
{
  VFID vfid;
  /* A refresh re-applies this list instead of re-reading the header page and its
     sector tables when the header page's LSA has not moved: every allocation,
     deallocation and sector change updates the header counters, so an unchanged LSA
     means unchanged tables. */
  UINT64 hdr_lsa;		/* page LSA of the header when the tables were last walked */
  VOLMAP_SECTREF *sects;
  int nsects;
  int sects_cap;
  FILE_TYPE ftype;
  OID class_oid;		/* heap only */
  int n_page_total;
  int n_page_user;
  int n_page_ftab;
  int n_page_free;
  int n_sector_total;
  INT64 alloc_pages;		/* from sector tables (L2) */
  int sectors_seen;
  char class_name[64];		/* lazily resolved from the class record; "" = not yet, "?" = failed */
  INT64 time_creation;		/* from FILE_HEADER — the only identity a temp file carries */
  PAGEID root_page;		/* b-tree root = sticky first page (BTID third component) */
  char index_name[64];		/* lazily resolved from class properties; "" = not yet, "?" = failed */
  signed char tracked;		/* 1 listed in the file tracker, 2 listed and marked deleted (dropped,
				   awaiting vacuum), 0 found by probing only */
  unsigned trk_seen;		/* the tracker walk (ctx->trk_gen) that last listed this file */
};

/* What runs beside the database, read from /proc (volmap_proc_scan).  Nothing here
   touches the server: process command lines and the socket table are public. */
typedef struct volmap_proc
{
  bool scanned;			/* /proc could be read at all */
  pid_t server_pid;		/* cub_server <db> on this host; 0 = none */
  int server_registered;	/* 1: its registration socket is in /proc/net/unix - boot and restart
				   recovery are over (css_init runs after boot_restart_server);
				   0: not there; -1: not observable - the socket got its pid-named
				   form in 11.1 (CBRD-23914), earlier releases use a random temp name */
  pid_t log_lock_pid;		/* who holds the fcntl write lock on the active log (every release
				   mounts it locked, before recovery); 0 nobody, -1 not checked */
  long server_elapsed;		/* seconds since cub_server started */
  char util[24];		/* cubrid/cub_admin utility running on this database ("" = none) */
  pid_t util_pid;
  long util_elapsed;		/* seconds since it started */
  time_t at;			/* when this was sampled */
} VOLMAP_PROC;

/* Disk activity of the device the volumes live on, from /proc/diskstats (readable by
   any account), and the server's own I/O from /proc/<pid>/io (its owner or root) */
typedef struct volmap_io
{
  int state;			/* 0 not sampled yet, 1 sampling, -1 device not found */
  int major_n, minor_n;
  char dev[32];
  struct timespec t;		/* last sample */
  unsigned long long io_ticks, in_queue, rd_sect, wr_sect;
  double util_pct;		/* % of the interval the device had I/O in flight */
  double queue;			/* average requests in flight */
  double mb_s;			/* device throughput over the interval */
  bool busy;			/* util_pct >= VOLMAP_IO_BUSY_PCT */
  int srv_ok;			/* /proc/<server>/io: 1 readable, 0 not, -1 not tried */
  unsigned long long srv_rd, srv_wr;
  double srv_mb_s;		/* the server's read+write throughput */
} VOLMAP_IO;
#define VOLMAP_IO_BUSY_PCT 80

/* one record of the cub_top --bcb-dump snapshot (32 bytes, little-endian; shared contract) */
typedef struct volmap_bufrec VOLMAP_BUFREC;
struct volmap_bufrec
{
  INT16 volid;
  INT16 zone;			/* 1/2/3 = LRU hot/warm/cold, 4 invalid, 8 void */
  INT32 pageid;
  UINT32 flags;			/* bit31 dirty, bit30 flushing */
  INT32 hdr_ok;			/* page header read + matched the BCB vpid */
  UINT64 page_lsa;		/* LSA in the in-memory page header (raw log_lsa) */
  UINT64 oldest_lsa;		/* BCB oldest_unflush_lsa (raw) */
};
#define VOLMAP_BUFREC_DIRTY 0x80000000u
#define VOLMAP_BUFREC_FLUSHING 0x40000000u	/* flush started: dirty bit already cleared, write not yet done */
/* not yet on the volume: the engine clears DIRTY when a flush starts and keeps FLUSHING
   until the write is done (pgbuf_bcb_mark_is_flushing / mark_was_flushed), so only a
   page with both bits off can be taken for the volume's content */
#define VOLMAP_BUFREC_UNFLUSHED (VOLMAP_BUFREC_DIRTY | VOLMAP_BUFREC_FLUSHING)
#define VOLMAP_LSA_NULL (~(UINT64) 0)

/* Why a volume listed in the vinf could not be shown.  DAMAGED and UNRECOGNIZED are
   findings; UNREADABLE and UNSUPPORTED only make the report incomplete. */
typedef enum
{
  VOLMAP_SKIP_UNREADABLE = 0,	/* cannot be opened or read (permission, missing, out of memory) */
  VOLMAP_SKIP_DAMAGED,		/* not a volume header, though the release is one volmap reads */
  VOLMAP_SKIP_UNRECOGNIZED,	/* CUBRID magic but the layout fails the self-check, release unknown:
				   a damaged header or a release before 10.1 - cannot tell which */
  VOLMAP_SKIP_UNSUPPORTED	/* the log names a release before 10.1: an older format, not damage */
} VOLMAP_SKIP_CAUSE;

/* A volume listed in the vinf that could not be shown, and why */
typedef struct volmap_skip
{
  char *path;
  int listed_volid;
  int cause;			/* VOLMAP_SKIP_CAUSE */
  char reason[200];
} VOLMAP_SKIP;

typedef struct volmap_ctx VOLMAP_CTX;
struct volmap_ctx
{
  VOLMAP_VOLUME *vols;		/* grown on demand; see volmap_open_volume () */
  int nvols_alloc;		/* entries allocated in vols[] */
  int nvols;
  VOLMAP_SKIP *skipped;		/* vinf volumes that could not be shown (volmap_skip_add) */
  int nskipped;
  char open_err[200];		/* why the last volmap_open_volume () failed */
  int open_err_cause;		/* ... and its VOLMAP_SKIP_CAUSE */
  /* Temp volumes (<db>_t<NNNNN>) are absent from the vinf and short-lived - they
     exist only while a query spills.  [r] rescans the same directory to add new
     ones and drop those that vanished, which needs the vinf path kept here. */
  char vinf_path[PATH_MAX];
  /* Where temp volumes live when the server spills elsewhere: temp_volume_path
     from cubrid.conf, or --temp-path.  Empty = database directory only. */
  char temp_path[PATH_MAX];
  /* header layout of these volumes: v11.4 inserted vol_creation, so the fields
     after it are 8 bytes earlier on an older volume (see VOLMAP_VLAYOUT) */
  int vlayout;
  /* bumped when a refresh is published: lets the UI drop answers it cached from the
     previous on-disk state (see volmap_ovf_owner_file).  Written under pub_mx while
     no frame is in progress, or by the UI thread itself, so frames read it safely. */
  unsigned refresh_gen;
  char db_release[32];
  bool release_unsupported;	/* the log names a release before 10.1 (warned at start and at exit) */
  int shutdown_state;		/* log header is_shutdown: 1 clean, 0 not clean, -1 unknown (no log) */
  long sweep_left;		/* unowned sectors the automatic second pass did not probe (over the cap) */
  /* The engine's own list of files: volume 0 names the boot heap, whose first record
     (BOOT_DB_PARM) names the file tracker.  Read first, so pass A probes only the
     sectors no listed file owns (volmap_discover_tracker). */
  bool no_tracker;		/* --no-tracker: probe every reserved sector's first page, as before */
  int tracker_state;		/* 0 not tried, 1 read, -1 not readable (tracker_why) */
  char tracker_why[120];
  VPID tracker_first;		/* first tracker page, for the re-walk on refresh */
  int files_tracked;		/* files registered from the tracker */
  long probe_sectors;		/* sectors whose first page pass A read after the tracker */
  VOLMAP_PROC proc;		/* server and utility processes of this database (volmap_proc_scan) */
  VOLMAP_IO io;			/* disk activity (volmap_io_sample) */
  bool hold;			/* -i: scan and auto refresh paused while a utility runs or the server recovers */
  bool hold_override;		/* the user pressed r during a hold: go on regardless, this run */
  char notes_json[2048];	/* --format=json: notes collected by volmap_findings */
  int notes_len;
  VOLMAP_FILE *files;
  int nfiles;
  /* (volid, fileid) -> index into files[] + 1, open addressing.  The tracker walk and
     every probe ask "is this file known"; a linear search made that quadratic in the
     file count on each refresh. */
  int *fidx;
  size_t fidx_cap;		/* power of two, 2 x VOLMAP_MAX_FILES */
  /* the tracker pages as last walked, with their LSAs: a refresh re-reads only the 32-byte
     page prefix of each and skips the walk when none has moved */
  VPID *trk_pages;
  UINT64 *trk_lsa;
  int trk_npages;
  unsigned trk_gen;		/* bumped by every complete tracker walk: files it did not list lose 'tracked' */
  int tracker_self;		/* files[] index of the tracker file itself (never in its own list), -1 none */
  long rf_cached;		/* last refresh: files re-applied from their sector list */
  long rf_walked;		/* ... files whose header moved and were re-walked */
  bool rf_tracker_walked;	/* ... tracker pages had moved: walked again */
  int width;
  int rows;
  bool plain;
  bool deep;
  bool full_sweep;
  FILE *outfp;
  /* -V selection, keyed by volid over the engine's whole range.  Sizing this by the
     volume count instead could not hold a temp volume: those are numbered down from
     LOG_MAX_DBVOLID (32766), so every one of them fell outside it and -V could
     neither select nor show them.  A bit per volid is 4KB. */
  unsigned char vol_filter[(VOLMAP_MAX_VOLID + 8) / 8];
  bool vol_filter_on;
  bool full;			/* -f: one cell per page, no row limit */
  bool interactive;		/* -i: full-screen mouse-driven browser */
  bool residency;		/* -m: glyph ramp shows OS page-cache residency (mincore) */
  /* --bufmap FILE: buffer-pool layer (cub_top --bcb-dump snapshot) */
  const char *bufmap_path;
  bool bufmap;			/* layer on (toggle: b) */
  bool bufmap_loaded;
  time_t bufmap_mtime;
  INT64 bm_ts;			/* snapshot time (epoch seconds) */
  long bm_nbuf, bm_nrec, bm_dirty;
  UINT64 bm_log_append, bm_log_nxio, bm_log_eof, bm_oldest;
  char bm_db[32];
  VOLMAP_BUFREC *bm_recs;	/* sorted by (volid, pageid) for the page-box lookup */
  const char *db_label;		/* database name (or vinf basename) for self-identifying output */
  int tick_sec;			/* -i auto-refresh period (--tick, default 2) */
  int warn_idle_pct;		/* --warn-idle=PCT: idle%% beyond this becomes a finding (0 = off) */
  bool check;			/* --check: integrity findings report */
  bool json;			/* --format json */
  /* progressive discovery (-i): the map appears immediately and file ownership
   * fills in as idle-time scan slices complete */
  bool scan_active;
  DKNSECTS *scan_pos;		/* per-volume next sector to probe */
  INT64 scan_probed;
  INT64 scan_total;		/* reserved sectors overall (progress denominator) */
  int scan_pref;		/* volume index to scan first (the one on screen) */
  char *scratch;		/* grow-only page/sector scratch shared by the render paths */
  int scratch_size;
  unsigned char *scratch_bmap;	/* byte-class scratch for the panel byte view */
  int scratch_bmap_size;
};

static int prv_user_offset (void);
static VOLMAP_VOLUME *volmap_find_vol (VOLMAP_CTX * ctx, INT16 volid);
static int volmap_open_volume (VOLMAP_CTX * ctx, const char *path);
static int volmap_read_stab (VOLMAP_VOLUME * vol);
static int volmap_read_residency (VOLMAP_VOLUME * vol);
static int volmap_discover_files (VOLMAP_CTX * ctx);
static int volmap_discover_files_from (VOLMAP_CTX * ctx, int first_pass);
static int volmap_resolve_volumes (VOLMAP_CTX * ctx, const char *db_name_or_vinf);
static int volmap_scan_temp_volumes (VOLMAP_CTX * ctx);
static bool volmap_try_file_header (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, PAGEID pageid, char *iopage);
static void volmap_discover_tracker (VOLMAP_CTX * ctx);
static int volmap_file_index (VOLMAP_CTX * ctx, INT16 volid, INT32 fileid);
static void volmap_fidx_put (VOLMAP_CTX * ctx, int k);
static int volmap_tracker_walk (VOLMAP_CTX * ctx, const VPID * first, char *why, size_t wn);
static void volmap_tde_sample (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol);
static void volmap_proc_scan (VOLMAP_CTX * ctx);
static void volmap_io_sample (VOLMAP_CTX * ctx);
static void volmap_io_begin (VOLMAP_CTX * ctx);
static void volmap_io_window (VOLMAP_CTX * ctx);
static int volmap_blockdev_of (const char *path, int *major_out, int *minor_out);
static void volmap_walk_extdata (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, const char *iopage, int offset,
				 bool is_partial, int file_idx);
static void volmap_deep_scan (VOLMAP_CTX * ctx);
static void volmap_render (VOLMAP_CTX * ctx);
static void volmap_usage (const char *argv0);

/* Is this volume selected?  True when -V was not given at all. */
static bool
volmap_vol_selected (const VOLMAP_CTX * ctx, int volid)
{
  if (!ctx->vol_filter_on)
    {
      return true;
    }
  if (volid < 0 || volid > VOLMAP_MAX_VOLID)
    {
      return false;
    }
  return (ctx->vol_filter[volid / 8] >> (volid % 8)) & 1;
}

/* offset of user page area inside an io page */
static int
prv_user_offset (void)
{
  return (int) offsetof (FILEIO_PAGE, page);
}

/* Which DISK_VOLUME_HEADER layout a volume uses.
 *
 * v11.4 inserted vol_creation (INT64) after db_creation - CBRD-25365, 44c022c31.
 * storage_ondisk_layout.hpp carries the 11.4+ copy, so on an older volume every
 * field from chkpt_lsa onward sits 8 bytes earlier than the struct says.  Reading
 * them through the struct yields garbage (a 10.2 demodb reports next_vol 29231,
 * which is really two characters of the volume name).
 *
 * No volume field records the release, but the layout does not need one: each of
 * the two shapes is self-evident in the volume itself, so a volume is read on its
 * own terms and the log is only a cross-check (volmap_vlayout_probe).
 *
 * The log can still be asked, and its header carries db_release as a printable
 * string.  That offset moved between releases too, so the string is located by
 * pattern rather than by a fixed offset.  Note the log states the release that
 * last wrote it, not the one that created the volume: an upgraded 10.2 database
 * has an 11.x log over 10.2-shaped volumes, which is a second reason the volume
 * decides and the log only confirms. */
typedef enum
{
  VOLMAP_VLAY_UNKNOWN = 0,	/* version not determined: do not print shifted fields */
  VOLMAP_VLAY_101,		/* 10.1: no page watermark either (see volmap_user_size) */
  VOLMAP_VLAY_PRE_114,		/* 10.2 .. 11.3: watermark present, no vol_creation */
  VOLMAP_VLAY_114		/* >= 11.4: vol_creation present (matches the struct) */
} VOLMAP_VLAYOUT;

/* The active log's path, as the vinf records it.  createdb --log-path puts the
   log elsewhere, and then guessing a sibling of the vinf finds nothing; the vinf
   lists the real path under LOG_DBLOG_ACTIVE_VOLID (-2), which is how the engine
   locates it too (get_active_log_vol_path in migrate.c).  Volume 0 is looked up
   the same way, to probe the layout from the volume itself.  Returns false when
   the vinf has no such entry. */
static bool
volmap_path_from_vinf (const char *vinf_path, int want, char *out, size_t outsz)
{
  char line[PATH_MAX + 64];
  FILE *fp;
  bool found = false;

  if (vinf_path == NULL || vinf_path[0] == '\0' || (fp = fopen (vinf_path, "r")) == NULL)
    {
      return false;
    }
  while (fgets (line, sizeof (line), fp) != NULL)
    {
      char path[PATH_MAX];
      int id;

      if (sscanf (line, " %d %4095s", &id, path) == 2 && id == want)
	{
	  snprintf (out, outsz, "%s", path);
	  found = true;
	  break;
	}
    }
  fclose (fp);
  return found;
}

/* Read the active log header and return its release string in rel[] ("" if not
   found), and in *shut its is_shutdown flag: 1 shut down cleanly, 0 not (the server
   is running, or stopped without a clean shutdown and recovery has not run yet),
   -1 unknown (no log, or the fields do not check out).
   From 10.1 to develop the fields after db_release are fixed: db_release[15],
   float db_compatibility (at +16), PGLENGTH db_iopagesize and db_logpagesize (+20,
   +22), then is_shutdown at +24 (an int in 10.1, a bool after).  The two page
   sizes confirm the position before the flag is believed. */
static bool
volmap_read_db_release (const char *vinf_path, char *rel, size_t relsz, int *shut)
{
  char path[PATH_MAX];
  unsigned char buf[512];
  size_t n, i;
  const char *dot;
  FILE *fp;

  rel[0] = '\0';
  *shut = -1;
  if (vinf_path == NULL || vinf_path[0] == '\0')
    {
      return false;
    }
  if (!volmap_path_from_vinf (vinf_path, VOLMAP_LGAT_VOLID, path, sizeof (path)))
    {
      /* No -2 entry: fall back to the sibling name.  Replace the suffix of the
         file name only - searching the whole path would match a directory called
         e.g. my_vinf_dir and build a path that does not exist. */
      dot = strrchr (vinf_path, '/');
      dot = strstr ((dot != NULL) ? dot + 1 : vinf_path, "_vinf");
      if (dot == NULL || strcmp (dot, "_vinf") != 0)
	{
	  return false;		/* only a trailing _vinf names a volume info file */
	}
      if (snprintf (path, sizeof (path), "%.*s_lgat", (int) (dot - vinf_path), vinf_path) >= (int) sizeof (path))
	{
	  return false;
	}
    }
  fp = fopen (path, "rb");
  if (fp == NULL)
    {
      return false;
    }
  n = fread (buf, 1, sizeof (buf), fp);
  fclose (fp);

  /* first "<digits>.<digits>" run in the header area is db_release */
  for (i = 0; i + 2 < n; i++)
    {
      size_t j = i;

      if (!isdigit (buf[i]) || (i > 0 && (isdigit (buf[i - 1]) || buf[i - 1] == '.')))
	{
	  continue;
	}
      while (j < n && (isdigit (buf[j]) || buf[j] == '.'))
	{
	  j++;
	}
      if (j - i >= 3 && memchr (buf + i, '.', j - i) != NULL)
	{
	  size_t len = j - i;

	  if (len >= relsz)
	    {
	      len = relsz - 1;
	    }
	  memcpy (rel, buf + i, len);
	  rel[len] = '\0';
	  if (i + 25 <= n)
	    {
	      INT16 iop, logp;

	      memcpy (&iop, buf + i + 20, sizeof (iop));
	      memcpy (&logp, buf + i + 22, sizeof (logp));
	      if ((iop == 4096 || iop == 8192 || iop == 16384) && (logp == 4096 || logp == 8192 || logp == 16384)
		  && buf[i + 24] <= 1)
		{
		  *shut = buf[i + 24];
		}
	    }
	  return true;
	}
    }
  return false;
}

/* Pick the header layout from a release string such as "11.4.4" or "10.2.17". */
static VOLMAP_VLAYOUT
volmap_vlayout_of (const char *rel)
{
  int maj = 0, min = 0;

  if (rel == NULL || sscanf (rel, "%d.%d", &maj, &min) != 2)
    {
      return VOLMAP_VLAY_UNKNOWN;
    }
  /* The string is found by scanning for a digits.digits run, so binary bytes can
     produce one by chance.  Only a release this tool knows about is accepted;
     anything else is no answer rather than a wrong one. */
  if (maj < 10 || maj > VOLMAP_MAX_KNOWN_MAJOR || min < 0 || min > 99)
    {
      return VOLMAP_VLAY_UNKNOWN;
    }
  if (maj > 11 || (maj == 11 && min >= 4))
    {
      return VOLMAP_VLAY_114;
    }
  if (maj == 10 && min <= 1)
    {
      return VOLMAP_VLAY_101;	/* 10.0 never gets this far - the header self-check rejects it */
    }
  return VOLMAP_VLAY_PRE_114;
}

/* A release before the 10.1 disk redesign (CBRD-20185): 10.0, 9.x, 8.x (2008 R4.x).
   Those volumes carry the same magic in an older layout.  Lower majors are not
   CUBRID releases, so a digits.digits run that binary bytes made by chance is not
   taken for one. */
static bool
volmap_release_before_101 (const char *rel)
{
  int maj = 0, min = 0;

  if (rel == NULL || sscanf (rel, "%d.%d", &maj, &min) != 2 || min < 0 || min > 99)
    {
      return false;
    }
  return (maj >= 8 && maj <= 9) || (maj == 10 && min == 0);
}

/* Said before the volumes are read and again when the run ends, so it is the last
   thing on the terminal after a long report or the interactive screen. */
static void
volmap_warn_unsupported (const VOLMAP_CTX * ctx)
{
  if (ctx->release_unsupported)
    {
      fprintf (stderr, "volmap: WARNING: database release %s is not supported - volmap reads 10.1 and later;"
	       " volumes of this release are listed as skipped, not as damaged\n", ctx->db_release);
    }
}

/* What the log says about how the database last stopped.  Not a judgement of whether
   it runs now: is_shutdown is 0 both while the server runs and after a crash. */
static const char *
volmap_shutdown_name (int st)
{
  return (st == 1) ? "clean" : (st == 0) ? "not_clean" : "unknown";
}

/* How the files were found: the tracker (and how many it listed) or probing alone */
static void
volmap_print_discovery (const VOLMAP_CTX * ctx, FILE * fp)
{
  int k, untracked = 0, deleted = 0;

  for (k = 0; k < ctx->nfiles; k++)
    {
      untracked += (ctx->files[k].tracked == 0);
      deleted += (ctx->files[k].tracked == 2);
    }
  if (ctx->tracker_state == 1)
    {
      fprintf (fp, "  files: %d listed in the file tracker", ctx->files_tracked);
      if (deleted > 0)
	{
	  fprintf (fp, " (%d marked deleted, sectors freed by vacuum later)", deleted);
	}
      fprintf (fp, "; %ld sector%s probed for the rest", ctx->probe_sectors, ctx->probe_sectors == 1 ? "" : "s");
      if (untracked > 0)
	{
	  fprintf (fp, ", %d file%s found that the tracker does not list", untracked, untracked == 1 ? "" : "s");
	}
      fprintf (fp, "\n");
    }
  else if (ctx->tracker_state == -1)
    {
      fprintf (fp, "  files: file tracker not read (%s) - found by probing the first page of %ld reserved sectors\n",
	       ctx->tracker_why, ctx->probe_sectors);
    }
  else if (ctx->no_tracker)
    {
      fprintf (fp, "  files: found by probing the first page of %ld reserved sectors (--no-tracker)\n", ctx->probe_sectors);
    }
}

static void
volmap_print_shutdown (const VOLMAP_CTX * ctx, FILE * fp)
{
  if (ctx->shutdown_state == 1)
    {
      fprintf (fp, "  log: release %s, shut down cleanly\n", ctx->db_release);
    }
  else if (ctx->shutdown_state == 0)
    {
      fprintf (fp, "  log: release %s, NOT shut down cleanly - the server is running, or it stopped without a clean"
	       " shutdown and restart recovery has not run: volumes may lack changes that are only in the log, and"
	       " may hold torn pages that restart replaces from the double write buffer\n", ctx->db_release);
    }
  else
    {
      fprintf (fp, "  log: %s - shutdown state unknown\n",
	       ctx->db_release[0] != '\0' ? "header fields not recognised" : "active log not found or unreadable");
    }
}

static const char *
volmap_vlayout_name (VOLMAP_VLAYOUT lay)
{
  switch (lay)
    {
    case VOLMAP_VLAY_101:
      return "10.1";
    case VOLMAP_VLAY_PRE_114:
      return "10.2-11.3";
    case VOLMAP_VLAY_114:
      return "11.4+";
    default:
      return "unknown";
    }
}

/* Does this volume carry the vol_creation field?  Tries both header shapes and
   keeps the one that is internally consistent.
 *
 * vol_fullname is the first of the variable-length fields, so its offset is 0
 * relative to var_fields, and the next two offsets run past it by the name's
 * length.  With the wrong shape those three INT16s land on the name's characters
 * or on the reserved zeros instead: a 10.2 demodb read as 11.4 gives
 * off_to_vol_fullname 28527 ('on'), and an 11.5 volume read as <=11.3 gives all
 * zeros.  Returns -1 when neither shape or both look right. */
static int
volmap_probe_vol_creation (const char *page0)
{
  const char *base = page0 + prv_user_offset ();
  int shift, found = -1;

  for (shift = 0; shift >= -8; shift -= 8)
    {
      INT16 nv, f_name, f_next, f_rem;

      memcpy (&nv, base + offsetof (DISK_VOLUME_HEADER, next_volid) + shift, sizeof (nv));
      memcpy (&f_name, base + offsetof (DISK_VOLUME_HEADER, offset_to_vol_fullname) + shift, sizeof (f_name));
      memcpy (&f_next, base + offsetof (DISK_VOLUME_HEADER, offset_to_next_vol_fullname) + shift, sizeof (f_next));
      memcpy (&f_rem, base + offsetof (DISK_VOLUME_HEADER, offset_to_vol_remarks) + shift, sizeof (f_rem));

      if (f_name != 0 || f_next <= 0 || f_rem < f_next || nv < -1 || nv > VOLMAP_MAX_VOLID)
	{
	  continue;
	}
      if (found != -1)
	{
	  return -1;		/* ambiguous: let the caller fall back to the log */
	}
      found = (shift == 0) ? 1 : 0;
    }
  return found;
}

/* Does this volume's pages end with the watermark?  FILEIO_PAGE_WATERMARK is a
   copy of prv.lsa (file_io.h), so a page that has one starts and ends with the
   same 8 bytes.  10.1 has no watermark and those last bytes are user data, which
   matches prv.lsa only by coincidence.  The decision is by majority, so a torn page
   (read during a server write, or left by a crash) cannot flip the layout: at least
   3/4 matching means present, at most one means absent, anything between is no
   answer.  Sampling walks the file rather than the header's page counts, which are
   themselves read through the layout being decided.  Returns -1 when too few
   written pages could be sampled or the vote is undecided. */
static int
volmap_probe_watermark (const char *path, int fd, int iopagesize)
{
  char *pg = (char *) malloc (iopagesize);
  int hit = 0, seen = 0;
  PAGEID p, npages;
  struct stat st;

  if (pg == NULL)
    {
      return -1;
    }
  if (fstat (fd, &st) != 0 || st.st_size < (off_t) iopagesize * 2)
    {
      free (pg);
      return -1;
    }
  npages = (PAGEID) (st.st_size / iopagesize);
  for (p = 1; p < npages && seen < VOLMAP_WM_SAMPLES; p++)
    {
      if (pread (fd, pg, iopagesize, (off_t) p * iopagesize) != iopagesize)
	{
	  break;
	}
      if (memcmp (pg, "\0\0\0\0\0\0\0\0", 8) == 0)
	{
	  continue;		/* never written: prv.lsa and the tail are both zero */
	}
      seen++;
      hit += (memcmp (pg, pg + iopagesize - 8, 8) == 0);
    }
  free (pg);
  if (seen < VOLMAP_WM_MIN_SAMPLES)
    {
      return -1;
    }
  if (hit >= seen * 3 / 4)
    {
      if (hit < seen)
	{
	  fprintf (stderr, "volmap: %s: %d of %d sampled pages do not end with their watermark"
		   " (torn or being written)\n", path, seen - hit, seen);
	}
      return 1;
    }
  if (hit <= 1)
    {
      return 0;
    }
  fprintf (stderr, "volmap: %s: watermark found on %d of %d sampled pages - layout undecided\n", path, hit, seen);
  return -1;
}

/* The layout of the volume at path, decided from the volume alone.  Returns
   VOLMAP_VLAY_UNKNOWN when the file cannot be read or neither shape fits. */
static VOLMAP_VLAYOUT
volmap_vlayout_probe (const char *path)
{
  char *page0;
  DISK_VOLUME_HEADER *vhdr;
  VOLMAP_VLAYOUT lay = VOLMAP_VLAY_UNKNOWN;
  int fd, vc, wm;

  fd = open (path, O_RDONLY);
  if (fd < 0)
    {
      return VOLMAP_VLAY_UNKNOWN;
    }
  page0 = (char *) malloc (64 * 1024);
  if (page0 == NULL || pread (fd, page0, 64 * 1024, 0) < 4096)
    {
      goto done;
    }
  vhdr = (DISK_VOLUME_HEADER *) (page0 + prv_user_offset ());
  if (strncmp (vhdr->magic, CUBRID_MAGIC_DATABASE_VOLUME, strlen (CUBRID_MAGIC_DATABASE_VOLUME)) != 0
      || vhdr->iopagesize < 1024 || vhdr->iopagesize > 64 * 1024)
    {
      goto done;
    }

  vc = volmap_probe_vol_creation (page0);
  if (vc == 1)
    {
      lay = VOLMAP_VLAY_114;	/* vol_creation implies 11.4+, which has the watermark */
      goto done;
    }
  if (vc == 0)
    {
      /* 10.1 .. 11.3; the watermark splits 10.1 off from the rest.  Too few pages
         to sample (a copy cut short, say) is no answer: leave it UNKNOWN so the next
         volume or the log decides, rather than assume the watermark is there. */
      wm = volmap_probe_watermark (path, fd, vhdr->iopagesize);
      lay = (wm == 0) ? VOLMAP_VLAY_101 : (wm == 1) ? VOLMAP_VLAY_PRE_114 : VOLMAP_VLAY_UNKNOWN;
    }

done:
  free (page0);
  close (fd);
  return lay;
}

/* Size of the user area inside an io page.
 *
 * The page watermark (FILEIO_PAGE_WATERMARK, 8B at the end of the page) arrived in
 * 10.2 - CBRD-22231, d81b071e8.  v10.1's storage_common.c has
 * RESERVED_SIZE_IN_PAGE = sizeof (FILEIO_PAGE_RESERVED) alone; 10.2 adds the
 * watermark to it.  Subtracting it on a 10.1 volume puts the end of the page - and
 * with it the slot directory, which is addressed backwards from there - 8 bytes off,
 * so slot views, --deep, del/dead counts and offline name resolution would all read
 * the wrong place.
 *
 * With the version undetermined the watermark layout is assumed, because every
 * release from 10.2 on has it; the caller warns in that case. */
static int
volmap_user_size (int vlayout, int iopagesize)
{
  int n = iopagesize - prv_user_offset ();

  if (vlayout != VOLMAP_VLAY_101)
    {
      n -= (int) sizeof (FILEIO_PAGE_WATERMARK);
    }
  return n;
}

static VOLMAP_VOLUME *
volmap_find_vol (VOLMAP_CTX * ctx, INT16 volid)
{
  int i;
  for (i = 0; i < ctx->nvols; i++)
    {
      if (ctx->vols[i].volid == volid)
	{
	  return &ctx->vols[i];
	}
    }
  return NULL;
}

/* ── fd budget: never keep more than 2 volume files open at once ──────────────
 * A long-lived monitor holding one fd per volume (up to 256) is an unintended
 * OS footprint; a 2-slot MRU cache keeps the working pair (current volume +
 * cross-volume chain hops) open and transparently reopens on miss (O_RDONLY,
 * offset-free pread everywhere, so reopening is free of state). */
#define VOLMAP_MAX_OPEN_FDS 2
static VOLMAP_VOLUME *volmap_fd_slot[VOLMAP_MAX_OPEN_FDS];
/* interactive threading pins every volume fd open: the MRU cache would let one
 * thread close an fd another thread is pread()ing (or reuse its number) */
static int volmap_fd_pin = 0;
/* file view ([f]): when >= 0, map cells owned by OTHER files render dimmed so
 * the selected file's footprint stands out (volume -> file -> sector browsing) */
static int volmap_focus_file = -1;
/* drill-down companion: when >= 0, cells owned by THIS file get the navy marker
 * background WITHOUT dimming the rest - the physical map stays intact while the
 * anchored file's scattered sectors light up (same-file-multi-sector cue) */
static int volmap_mark_file = -1;
/* object(=class) marking: every file of this class gets a role background -
 * heap navy 17, its indexes dark green 22, overflow appendages dark purple 53 */
static int volmap_mark_class_on = 0;
static OID volmap_mark_class = { -1, -1, -1 };
/* [p] logical-chain view: heap pages form a doubly-linked LOGICAL chain
 * (HEAP_CHAIN.next_vpid, offset +16 in slot 0 - same offset in the header's
 * HEAP_HDR_STATS).  The walk marks every JUMP source (next page not physically
 * adjacent) so intra-file logical fragmentation shows on the map in red. */
static UINT64 *volmap_chain_bm = NULL;	/* per-sector page bits of jump sources */
static long volmap_chain_bm_nsect = 0;
static INT16 volmap_chain_volid = -1;	/* volume the bitmap maps; < 0 = view off */
static char volmap_chain_sum[240] = "";
/* worker threads active: the scan slice must not poke stdin (keys belong to the UI thread) */
static volatile int volmap_mt_run = 0;

static int
volmap_vol_fd (VOLMAP_VOLUME * vol)
{
  int i;

  if (volmap_fd_pin)
    {
      if (vol->fd < 0)
	{
	  vol->fd = open (vol->path, O_RDONLY);	/* kept open for the process lifetime */
	}
      return vol->fd;
    }
  if (vol->fd >= 0)
    {
      for (i = 0; i < VOLMAP_MAX_OPEN_FDS && volmap_fd_slot[i] != vol; i++)
	{
	  ;
	}
      for (; i > 0 && i < VOLMAP_MAX_OPEN_FDS; i--)
	{
	  volmap_fd_slot[i] = volmap_fd_slot[i - 1];	/* move to MRU front */
	}
      volmap_fd_slot[0] = vol;
      return vol->fd;
    }
  if (volmap_fd_slot[VOLMAP_MAX_OPEN_FDS - 1] != NULL)
    {
      close (volmap_fd_slot[VOLMAP_MAX_OPEN_FDS - 1]->fd);	/* evict LRU */
      volmap_fd_slot[VOLMAP_MAX_OPEN_FDS - 1]->fd = -1;
    }
  for (i = VOLMAP_MAX_OPEN_FDS - 1; i > 0; i--)
    {
      volmap_fd_slot[i] = volmap_fd_slot[i - 1];
    }
  vol->fd = open (vol->path, O_RDONLY);
  volmap_fd_slot[0] = (vol->fd >= 0) ? vol : NULL;
  return vol->fd;
}

static void
volmap_vol_fd_close (VOLMAP_VOLUME * vol)
{
  int i;

  for (i = 0; i < VOLMAP_MAX_OPEN_FDS; i++)
    {
      if (volmap_fd_slot[i] == vol)
	{
	  volmap_fd_slot[i] = NULL;
	}
    }
  if (vol->fd >= 0)
    {
      close (vol->fd);
      vol->fd = -1;
    }
}

/* Release everything one volume slot owns: its fd and every heap array hanging
   off it.  Every release path goes through here, so the list lives in one place.
   The w_* pointers alias either the read side or the sh_* set and are never a
   separate allocation, so they must NOT be freed here. */
static void
volmap_vol_release (VOLMAP_VOLUME * vol)
{
  volmap_vol_fd_close (vol);
  free (vol->stab);
  free (vol->owner);
  free (vol->alloc);
  free (vol->pagebm);
  free (vol->sh_stab);
  free (vol->sh_owner);
  free (vol->sh_alloc);
  free (vol->sh_pagebm);
  free (vol->respg);
  free (vol->res_prefix);
  free (vol->bufpg);
  free (vol->buf_prefix);
  free (vol->dirty_prefix);
  free (vol->tde_bm);
  free (vol->path);
  memset (vol, 0, sizeof (*vol));
  vol->fd = -1;			/* memset left it 0, which is a valid fd */
}

static bool
volmap_read_iopage (VOLMAP_VOLUME * vol, PAGEID pageid, char *buf)
{
  if (pageid < 0)
    {
      return false;
    }
  return pread (volmap_vol_fd (vol), buf, vol->iopagesize, (off_t) pageid * vol->iopagesize) == vol->iopagesize;
}


/* (re)read the sector allocation table — a few KB per volume */
static int
volmap_read_stab (VOLMAP_VOLUME * vol)
{
  int prv = prv_user_offset ();
  char *iopage = (char *) malloc (vol->iopagesize);
  int i, b, k, sect = 0;

  if (iopage == NULL)
    {
      return ER_FAILED;
    }
  for (i = 0; i < vol->stab_npages && sect < vol->nsect_total; i++)
    {
      if (!volmap_read_iopage (vol, vol->stab_first + i, iopage))
	{
	  free (iopage);
	  return ER_FAILED;
	}
      for (b = prv; b < vol->iopagesize && sect < vol->nsect_total; b++)
	{
	  for (k = 0; k < CHAR_BIT && sect < vol->nsect_total; k++, sect++)
	    {
	      vol->w_stab[sect] = (((unsigned char) iopage[b]) >> k) & 1;
	    }
	}
    }
  free (iopage);
  return NO_ERROR;
}


/* ── --bufmap: buffer-pool snapshot from cub_top --bcb-dump ────────────────────
 * The snapshot is what cub_top read straight out of cub_server's BCB array (no
 * latch, no server request).  volmap only maps it onto the physical volume grid:
 *   background teal   = page is in the buffer pool (clean)
 *   background purple = page is in the buffer pool and DIRTY (not yet written back)
 * Together with -m (OS page cache) and the on-disk page LSA this closes the chain
 *   log append LSA >= buffered page LSA >= on-disk page LSA. */
static INT64
volmap_lsa_pageid (UINT64 raw)
{
  return (raw == VOLMAP_LSA_NULL) ? -1 : (INT64) (((INT64) (raw << 16)) >> 16);
}

static int
volmap_lsa_offset (UINT64 raw)
{
  return (raw == VOLMAP_LSA_NULL) ? -1 : (int) (INT16) (raw >> 48);
}

static void
volmap_lsa_str (UINT64 raw, char *out, size_t n)
{
  if (raw == VOLMAP_LSA_NULL)
    {
      snprintf (out, n, "-");
    }
  else
    {
      snprintf (out, n, "%lld|%d", (long long) volmap_lsa_pageid (raw), volmap_lsa_offset (raw));
    }
}

/* -1/0/1 like strcmp; NULL sorts first */
static int
volmap_lsa_cmp (UINT64 a, UINT64 b)
{
  INT64 pa = volmap_lsa_pageid (a), pb = volmap_lsa_pageid (b);
  int oa, ob;

  if (pa != pb)
    {
      return (pa < pb) ? -1 : 1;
    }
  oa = volmap_lsa_offset (a);
  ob = volmap_lsa_offset (b);
  return (oa < ob) ? -1 : (oa > ob);
}

static int
volmap_bufrec_cmp (const void *a, const void *b)
{
  const VOLMAP_BUFREC *x = (const VOLMAP_BUFREC *) a, *y = (const VOLMAP_BUFREC *) b;

  if (x->volid != y->volid)
    {
      return (x->volid < y->volid) ? -1 : 1;
    }
  return (x->pageid < y->pageid) ? -1 : (x->pageid > y->pageid);
}

static const VOLMAP_BUFREC *
volmap_bufmap_find (VOLMAP_CTX * ctx, INT16 volid, PAGEID pageid)
{
  long lo = 0, hi = ctx->bm_nrec - 1;

  if (!ctx->bufmap_loaded || ctx->bm_recs == NULL)
    {
      return NULL;
    }
  while (lo <= hi)
    {
      long mid = (lo + hi) / 2;
      const VOLMAP_BUFREC *r = &ctx->bm_recs[mid];

      if (r->volid < volid || (r->volid == volid && r->pageid < pageid))
	{
	  lo = mid + 1;
	}
      else if (r->volid > volid || r->pageid > pageid)
	{
	  hi = mid - 1;
	}
      else
	{
	  return r;
	}
    }
  return NULL;
}

/* read the snapshot file and rebuild the per-volume page arrays.  A snapshot for a
 * different database (name mismatch) is refused: the map would lie. */
static int
volmap_bufmap_load (VOLMAP_CTX * ctx)
{
  struct
  {
    char magic[8];
    UINT32 version, reclen;
    INT64 ts_sec, ts_nsec;
    INT32 nbuf, nrec, dirty, pagesize;
    UINT64 log_append, log_nxio, log_eof, oldest_dirty;
    char db[32];
  } h;
  FILE *fp;
  VOLMAP_BUFREC *recs;
  struct stat st;
  int vi;
  long k;

  if (ctx->bufmap_path == NULL)
    {
      return ER_FAILED;
    }
  fp = fopen (ctx->bufmap_path, "rb");
  if (fp == NULL)
    {
      return ER_FAILED;
    }
  if (fread (&h, sizeof (h), 1, fp) != 1 || memcmp (h.magic, "CBCBMAP1", 8) != 0 || h.version != 1
      || h.reclen != sizeof (VOLMAP_BUFREC) || h.nrec < 0 || h.nrec > (1 << 26))
    {
      fclose (fp);
      return ER_FAILED;
    }
  recs = (VOLMAP_BUFREC *) malloc (sizeof (VOLMAP_BUFREC) * (size_t) (h.nrec > 0 ? h.nrec : 1));
  if (recs == NULL || (h.nrec > 0 && fread (recs, sizeof (VOLMAP_BUFREC), (size_t) h.nrec, fp) != (size_t) h.nrec))
    {
      free (recs);
      fclose (fp);
      return ER_FAILED;
    }
  fclose (fp);
  if (h.db[0] != '\0' && ctx->db_label != NULL && strncmp (h.db, ctx->db_label, sizeof (h.db) - 1) != 0)
    {
      /* refuse silently-wrong data: the snapshot belongs to another database */
      free (recs);
      snprintf (ctx->bm_db, sizeof (ctx->bm_db), "%s", h.db);
      ctx->bufmap_loaded = false;
      return ER_FAILED;
    }
  qsort (recs, (size_t) h.nrec, sizeof (VOLMAP_BUFREC), volmap_bufrec_cmp);
  free (ctx->bm_recs);
  ctx->bm_recs = recs;
  ctx->bm_nrec = h.nrec;
  ctx->bm_nbuf = h.nbuf;
  ctx->bm_dirty = h.dirty;
  ctx->bm_ts = h.ts_sec;
  ctx->bm_log_append = h.log_append;
  ctx->bm_log_nxio = h.log_nxio;
  ctx->bm_log_eof = h.log_eof;
  ctx->bm_oldest = h.oldest_dirty;
  snprintf (ctx->bm_db, sizeof (ctx->bm_db), "%s", h.db);
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      long npages = (long) vol->nsect_total * VOLMAP_SECT_NPAGES, p;

      if (vol->bufpg == NULL)
	{
	  vol->bufpg = (unsigned char *) calloc ((size_t) npages, 1);
	  vol->buf_prefix = (unsigned int *) malloc (((size_t) npages + 1) * sizeof (unsigned int));
	  vol->dirty_prefix = (unsigned int *) malloc (((size_t) npages + 1) * sizeof (unsigned int));
	}
      else
	{
	  memset (vol->bufpg, 0, (size_t) npages);
	}
      if (vol->bufpg == NULL || vol->buf_prefix == NULL || vol->dirty_prefix == NULL)
	{
	  continue;
	}
      vol->buf_total = vol->buf_dirty = vol->buf_flushing = 0;
      (void) p;
    }
  for (k = 0; k < ctx->bm_nrec; k++)
    {
      const VOLMAP_BUFREC *r = &recs[k];
      VOLMAP_VOLUME *vol = volmap_find_vol (ctx, r->volid);
      long npages;

      if (vol == NULL || vol->bufpg == NULL || r->pageid < 0)
	{
	  continue;
	}
      npages = (long) vol->nsect_total * VOLMAP_SECT_NPAGES;
      if ((long) r->pageid >= npages)
	{
	  continue;		/* volume grew after our sector table was read */
	}
      vol->bufpg[r->pageid] = (r->flags & VOLMAP_BUFREC_UNFLUSHED) ? 2 : 1;
      vol->buf_flushing += (r->flags & VOLMAP_BUFREC_FLUSHING) != 0;
    }
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      long npages = (long) vol->nsect_total * VOLMAP_SECT_NPAGES, p;
      unsigned int acc = 0, dacc = 0;

      if (vol->bufpg == NULL || vol->buf_prefix == NULL || vol->dirty_prefix == NULL)
	{
	  continue;
	}
      vol->buf_prefix[0] = 0;
      vol->dirty_prefix[0] = 0;
      for (p = 0; p < npages; p++)
	{
	  acc += (vol->bufpg[p] != 0);
	  dacc += (vol->bufpg[p] == 2);
	  vol->buf_prefix[p + 1] = acc;
	  vol->dirty_prefix[p + 1] = dacc;
	}
      vol->buf_total = acc;
      vol->buf_dirty = dacc;
      /* buffered-but-unallocated: freed pages stay in the pool until victimized (normal),
       * temp pages of a finished query likewise; shown as 'freed N', a finding only when
       * nearly everything mismatches (stale or foreign snapshot) */
      vol->buf_freed = 0;
      if (vol->pagebm != NULL)
	{
	  for (p = 0; p < npages; p++)
	    {
	      if (vol->bufpg[p] != 0 && p > (long) vol->sys_lastpage
		  && !(vol->pagebm[p / VOLMAP_SECT_NPAGES] & ((UINT64) 1 << (p % VOLMAP_SECT_NPAGES))))
		{
		  vol->buf_freed++;
		}
	    }
	}
    }
  ctx->bufmap_loaded = true;
  if (stat (ctx->bufmap_path, &st) == 0)
    {
      ctx->bufmap_mtime = st.st_mtime;
    }
  return NO_ERROR;
}

/* reload when the file changed (cub_top live rewrites it every frame) — returns true if reloaded */
static bool
volmap_bufmap_reload (VOLMAP_CTX * ctx)
{
  struct stat st;

  if (ctx->bufmap_path == NULL)
    {
      return false;
    }
  if (stat (ctx->bufmap_path, &st) != 0)
    {
      return false;
    }
  if (ctx->bufmap_loaded && st.st_mtime == ctx->bufmap_mtime)
    {
      return false;
    }
  return volmap_bufmap_load (ctx) == NO_ERROR;
}

/* one-line summary of the snapshot for headers/footers */
static void
volmap_bufmap_summary (VOLMAP_CTX * ctx, char *out, size_t n)
{
  char ts[32] = "?", la[32], ln[32], lo[32];
  time_t t = (time_t) ctx->bm_ts;
  struct tm tmv;

  if (!ctx->bufmap_loaded)
    {
      if (ctx->bm_db[0] != '\0')
	{
	  snprintf (out, n, "bufmap: snapshot is for db '%s', not this database - ignored", ctx->bm_db);
	}
      else
	{
	  snprintf (out, n, "bufmap: %s not readable (run cub_top --bcb-dump FILE)", ctx->bufmap_path);
	}
      return;
    }
  if (t > 0 && localtime_r (&t, &tmv) != NULL)
    {
      strftime (ts, sizeof (ts), "%H:%M:%S", &tmv);
    }
  volmap_lsa_str (ctx->bm_log_append, la, sizeof (la));
  volmap_lsa_str (ctx->bm_log_nxio, ln, sizeof (ln));
  volmap_lsa_str (ctx->bm_oldest, lo, sizeof (lo));
  snprintf (out, n, "buffer pool @%s: %ld/%ld pages resident, %ld unflushed | log append %s flushed %s | oldest dirty LSA %s",
	    ts, ctx->bm_nrec, ctx->bm_nbuf, ctx->bm_dirty, la, ln, lo);
}

/* Can mincore(2) be believed for this volume?  For a file mapping the kernel reports
   the real page-cache state only to the file's owner or to a caller with write
   permission (mm/mincore.c can_do_mincore); anyone else is told every page is
   resident.  root passes as owner (CAP_FOWNER). */
static bool
volmap_mincore_trusted (VOLMAP_VOLUME * vol)
{
  struct stat st;

  if (geteuid () == 0)
    {
      return true;
    }
  if (fstat (volmap_vol_fd (vol), &st) == 0 && st.st_uid == geteuid ())
    {
      return true;
    }
  return faccessat (AT_FDCWD, vol->path, W_OK, AT_EACCESS) == 0;
}

/*
 * volmap_read_residency () - OS page-cache residency via mincore(2).
 * mmap+mincore only consults kernel metadata: no page is faulted in, nothing is read,
 * no lock is taken — safe on a live production volume.
 */
static int
volmap_read_residency (VOLMAP_VOLUME * vol)
{
  size_t len = (size_t) vol->nsect_total * VOLMAP_SECT_NPAGES * vol->iopagesize;
  size_t os_pages = (len + 4095) / 4096;
  int sub = vol->iopagesize / 4096;
  unsigned char *vec;
  void *base;
  long p, npages = (long) vol->nsect_total * VOLMAP_SECT_NPAGES;

  if (vol->res_untrusted || !volmap_mincore_trusted (vol))
    {
      vol->res_untrusted = true;	/* shown as unknown rather than as fully cached */
      return ER_FAILED;
    }
  if (vol->respg == NULL)
    {
      vol->respg = (unsigned char *) calloc (npages, 1);
      if (vol->respg == NULL)
	{
	  return ER_FAILED;
	}
    }
  base = mmap (NULL, len, PROT_READ, MAP_SHARED, volmap_vol_fd (vol), 0);
  if (base == MAP_FAILED)
    {
      return ER_FAILED;
    }
  vec = (unsigned char *) malloc (os_pages);
  if (vec == NULL || mincore (base, len, vec) != 0)
    {
      free (vec);
      munmap (base, len);
      return ER_FAILED;
    }
  if (vol->res_prefix == NULL)
    {
      vol->res_prefix = (unsigned int *) malloc (((size_t) npages + 1) * sizeof (unsigned int));
    }
  vol->res_total = 0;
  if (vol->res_prefix != NULL)
    {
      vol->res_prefix[0] = 0;
    }
  for (p = 0; p < npages; p++)
    {
      int i, cnt = 0;
      for (i = 0; i < sub; i++)
	{
	  cnt += vec[(size_t) p * sub + i] & 1;
	}
      vol->respg[p] = (unsigned char) cnt;
      vol->res_total += cnt;
      if (vol->res_prefix != NULL)
	{
	  vol->res_prefix[p + 1] = (unsigned int) (vol->res_prefix[p] + cnt);
	}
    }
  free (vec);
  munmap (base, len);
  return NO_ERROR;
}

/*
 * volmap_open_volume () - parse volume header and sector allocation table (L1) straight from the file
 */
static int
volmap_open_volume (VOLMAP_CTX * ctx, const char *path)
{
  VOLMAP_VOLUME *vol;
  DISK_VOLUME_HEADER *vhdr;
  char *iopage = NULL;
  int prv = prv_user_offset ();
  if (ctx->nvols > VOLMAP_MAX_VOLID)
    {
      /* Only reachable past the engine's own volid ceiling.  Dropping volumes in
         silence would understate every total on screen, so it is said once. */
      static bool said = false;

      if (!said)
	{
	  said = true;
	  fprintf (stderr, "volmap: more than %d volumes - the rest are not shown, so the totals are partial\n",
		   VOLMAP_MAX_VOLID + 1);
	}
      snprintf (ctx->open_err, sizeof (ctx->open_err), "more than %d volumes", VOLMAP_MAX_VOLID + 1);
      ctx->open_err_cause = VOLMAP_SKIP_UNREADABLE;
      return ER_FAILED;
    }
  if (ctx->nvols >= ctx->nvols_alloc)
    {
      /* Grow on demand: ~300 bytes per volume that exists.  Volumes are added only
         by the initial load (before the worker threads start) and by the [r] rescan
         (behind the list_frozen barrier), so no lane holds a VOLMAP_VOLUME * here. */
      int want = (ctx->nvols_alloc > 0) ? ctx->nvols_alloc * 2 : 16;
      VOLMAP_VOLUME *grown;

      if (want > VOLMAP_MAX_VOLID + 1)
	{
	  want = VOLMAP_MAX_VOLID + 1;
	}
      grown = (VOLMAP_VOLUME *) realloc (ctx->vols, (size_t) want * sizeof (*grown));
      if (grown == NULL)
	{
	  fprintf (stderr, "volmap: out of memory for %d volumes\n", want);
	  snprintf (ctx->open_err, sizeof (ctx->open_err), "out of memory");
	  ctx->open_err_cause = VOLMAP_SKIP_UNREADABLE;
	  return ER_FAILED;
	}
      memset (grown + ctx->nvols_alloc, 0, (size_t) (want - ctx->nvols_alloc) * sizeof (*grown));
      if (grown != ctx->vols)
	{
	  /* The LRU fd cache holds VOLMAP_VOLUME pointers across calls: rebase the
	     entries that point into this array, drop anything else. */
	  int k;

	  for (k = 0; k < VOLMAP_MAX_OPEN_FDS; k++)
	    {
	      if (volmap_fd_slot[k] >= ctx->vols && volmap_fd_slot[k] < ctx->vols + ctx->nvols_alloc)
		{
		  volmap_fd_slot[k] = grown + (volmap_fd_slot[k] - ctx->vols);
		}
	      else
		{
		  volmap_fd_slot[k] = NULL;
		}
	    }
	}
      ctx->vols = grown;
      ctx->nvols_alloc = want;
    }
  vol = &ctx->vols[ctx->nvols];
  memset (vol, 0, sizeof (*vol));
  vol->fd = -1;
  snprintf (ctx->open_err, sizeof (ctx->open_err), "out of memory");
  ctx->open_err_cause = VOLMAP_SKIP_UNREADABLE;
  vol->path = strdup (path);
  if (vol->path == NULL)
    {
      goto error;
    }

  if (volmap_vol_fd (vol) < 0)
    {
      /* diagnostics go to stderr: outfp may be a --format json document or -o file */
      fprintf (stderr, "volmap: cannot open %s: %s\n", path, strerror (errno));
      snprintf (ctx->open_err, sizeof (ctx->open_err), "cannot open: %s", strerror (errno));
      goto error;
    }

  /* header page: read a maximal io page first to learn the real page size from the header itself */
  iopage = (char *) malloc (64 * 1024);
  if (iopage == NULL)
    {
      goto error;
    }
  {
    ssize_t got = pread (volmap_vol_fd (vol), iopage, 64 * 1024, 0);

    if (got < 4096)
      {
	if (got < 0)
	  {
	    snprintf (ctx->open_err, sizeof (ctx->open_err), "cannot read the header: %s", strerror (errno));
	  }
	else
	  {
	    snprintf (ctx->open_err, sizeof (ctx->open_err), "shorter than one page (%ld bytes)", (long) got);
	    ctx->open_err_cause = VOLMAP_SKIP_DAMAGED;
	  }
	fprintf (stderr, "volmap: %s: %s\n", path, ctx->open_err);
	free (iopage);
	goto error;
      }
  }
  vhdr = (DISK_VOLUME_HEADER *) (iopage + prv);
  if (strncmp (vhdr->magic, CUBRID_MAGIC_DATABASE_VOLUME, strlen (CUBRID_MAGIC_DATABASE_VOLUME)) != 0)
    {
      /* Before 10.0 the page header is shorter, so even the magic is not where this
         build looks: an old release, not damage, when the log says so */
      if (ctx->release_unsupported)
	{
	  snprintf (ctx->open_err, sizeof (ctx->open_err),
		    "unsupported release %s (volmap reads 10.1+) - older volume layout", ctx->db_release);
	  ctx->open_err_cause = VOLMAP_SKIP_UNSUPPORTED;
	}
      else
	{
	  snprintf (ctx->open_err, sizeof (ctx->open_err), "not a CUBRID volume (bad magic)");
	  ctx->open_err_cause = VOLMAP_SKIP_DAMAGED;
	}
      fprintf (stderr, "volmap: %s: %s\n", path, ctx->open_err);
      free (iopage);
      goto error;
    }
  if (vhdr->sect_npgs != VOLMAP_SECT_NPAGES || vhdr->iopagesize < 1024 || vhdr->nsect_total <= 0)
    {
      /* The magic is the same before 10.1, only the layout differs, so a failed
         self-check alone cannot tell an old format from damage: the log decides. */
      if (ctx->release_unsupported)
	{
	  snprintf (ctx->open_err, sizeof (ctx->open_err),
		    "unsupported release %s (volmap reads 10.1+) - older volume layout", ctx->db_release);
	  ctx->open_err_cause = VOLMAP_SKIP_UNSUPPORTED;
	}
      else if (ctx->db_release[0] == '\0')
	{
	  snprintf (ctx->open_err, sizeof (ctx->open_err),
		    "header self-check failed (sect_npgs=%d iopagesize=%d) - unsupported format (before 10.1)"
		    " or damaged; the log gives no release to tell which", vhdr->sect_npgs, vhdr->iopagesize);
	  ctx->open_err_cause = VOLMAP_SKIP_UNRECOGNIZED;
	}
      else
	{
	  snprintf (ctx->open_err, sizeof (ctx->open_err), "header self-check failed (sect_npgs=%d iopagesize=%d)",
		    vhdr->sect_npgs, vhdr->iopagesize);
	  ctx->open_err_cause = VOLMAP_SKIP_DAMAGED;
	}
      fprintf (stderr, "volmap: %s: %s\n", path, ctx->open_err);
      free (iopage);
      goto error;
    }

  vol->volid = vhdr->volid;
  vol->iopagesize = vhdr->iopagesize;
  vol->user_size = volmap_user_size (ctx->vlayout, vol->iopagesize);
  vol->nsect_total = vhdr->nsect_total;
  vol->stab_npages = vhdr->stab_npages;
  vol->stab_first = vhdr->stab_first_page;
  vol->sys_lastpage = vhdr->sys_lastpage;
  vol->purpose = vhdr->purpose;
  free (iopage);

  /* L1: sector allocation table. bit order matches disk_stab_dump_unit (LSB first). */
  vol->stab = (unsigned char *) calloc (vol->nsect_total, 1);
  vol->owner = (int *) malloc (vol->nsect_total * sizeof (int));
  vol->alloc = (int *) calloc (vol->nsect_total, sizeof (int));
  vol->pagebm = (UINT64 *) calloc (vol->nsect_total, sizeof (UINT64));
  vol->tde_bm = (UINT64 *) calloc (vol->nsect_total, sizeof (UINT64));
  if (vol->stab == NULL || vol->owner == NULL || vol->alloc == NULL || vol->pagebm == NULL || vol->tde_bm == NULL)
    {
      goto error;
    }
  memset (vol->owner, -1, vol->nsect_total * sizeof (int));
  vol->w_stab = vol->stab;	/* write side aliases the read side until a threaded refresh */
  vol->w_owner = vol->owner;
  vol->w_alloc = vol->alloc;
  vol->w_pagebm = vol->pagebm;
  vol->sh_stab = NULL;
  vol->sh_owner = NULL;
  vol->sh_alloc = NULL;
  vol->sh_pagebm = NULL;
  vol->hwm_page = -1;

  if (volmap_read_stab (vol) != NO_ERROR)
    {
      snprintf (ctx->open_err, sizeof (ctx->open_err), "sector table unreadable");
      ctx->open_err_cause = VOLMAP_SKIP_DAMAGED;
      goto error;
    }

  ctx->nvols++;
  return NO_ERROR;

error:
  /* the slot will be reused by the next volume: nothing may stay owned by it */
  volmap_vol_release (vol);
  return ER_FAILED;
}

/* Skipped volumes that -V selects: -V narrows the findings and the exit code too */
static int
volmap_skipped_selected (VOLMAP_CTX * ctx)
{
  int i, n = 0;

  for (i = 0; i < ctx->nskipped; i++)
    {
      n += volmap_vol_selected (ctx, ctx->skipped[i].listed_volid) ? 1 : 0;
    }
  return n;
}

/* The report is incomplete without the volumes that could not be shown: list them
   after the map whether or not --check was given */
static void
volmap_print_skipped (VOLMAP_CTX * ctx, FILE * fp)
{
  int i, n = volmap_skipped_selected (ctx);

  if (n == 0)
    {
      return;
    }
  fprintf (fp, "%sSKIPPED VOLUMES%s  %d listed in the vinf, not shown\n", ctx->plain ? "" : VM_BOLD,
	   ctx->plain ? "" : VM_RESET, n);
  for (i = 0; i < ctx->nskipped; i++)
    {
      if (!volmap_vol_selected (ctx, ctx->skipped[i].listed_volid))
	{
	  continue;
	}
      fprintf (fp, "  [vol %d] %s: %s%s\n", ctx->skipped[i].listed_volid, ctx->skipped[i].path, ctx->skipped[i].reason,
	       ctx->skipped[i].cause == VOLMAP_SKIP_DAMAGED ? " (damaged header)" : "");
    }
  fprintf (fp, "\n");
}

/* Remember a vinf volume that volmap_open_volume () could not show, with its cause */
static void
volmap_skip_add (VOLMAP_CTX * ctx, const char *path, int listed_volid)
{
  VOLMAP_SKIP *grown = (VOLMAP_SKIP *) realloc (ctx->skipped, (size_t) (ctx->nskipped + 1) * sizeof (*grown));

  if (grown == NULL)
    {
      return;
    }
  ctx->skipped = grown;
  grown[ctx->nskipped].path = strdup (path);
  grown[ctx->nskipped].listed_volid = listed_volid;
  grown[ctx->nskipped].cause = ctx->open_err_cause;
  snprintf (grown[ctx->nskipped].reason, sizeof (grown[ctx->nskipped].reason), "%s", ctx->open_err);
  ctx->nskipped++;
}

/* buffers that follow cross-volume chains must fit the largest page size */
static int
volmap_max_iopagesize (VOLMAP_CTX * ctx)
{
  int vi, m = 0;

  for (vi = 0; vi < ctx->nvols; vi++)
    {
      m = MAX (m, ctx->vols[vi].iopagesize);
    }
  return (m > 0) ? m : (64 * 1024);
}

/*
 * volmap_walk_extdata () - follow one FILE_EXTENSIBLE_DATA chain (partial or full sector table) and
 *                          record sector ownership + allocated page counts. Chains may cross volumes.
 */
static void
volmap_walk_extdata (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, const char *iopage, int offset, bool is_partial,
		     int file_idx)
{
  int prv = prv_user_offset ();
  char *cur = (char *) malloc (volmap_max_iopagesize (ctx));	/* the extdata chain crosses volumes */
  VOLMAP_VOLUME *cur_vol = vol;
  int guard;

  if (cur == NULL)
    {
      return;
    }
  memcpy (cur, iopage, vol->iopagesize);

  for (guard = 0; guard < 1000000; guard++)
    {
      int expected_size = is_partial ? (int) sizeof (FILE_PARTIAL_SECTOR) : (int) sizeof (VSID);
      VPID next;
      int i;

      if (offset < 0 || prv + offset + (int) sizeof (FILE_EXTENSIBLE_DATA) > cur_vol->iopagesize)
	{
	  break;		/* corrupt offset would point outside the page buffer */
	}
      const FILE_EXTENSIBLE_DATA *extdata = (const FILE_EXTENSIBLE_DATA *) (cur + prv + offset);
      const char *items = (const char *) extdata + DB_ALIGN (sizeof (FILE_EXTENSIBLE_DATA), MAX_ALIGNMENT);
      if (extdata->size_of_item != expected_size || extdata->n_items < 0
	  || prv + offset + (int) DB_ALIGN (sizeof (FILE_EXTENSIBLE_DATA), MAX_ALIGNMENT)
	  + extdata->n_items * expected_size > cur_vol->iopagesize)
	{
	  break;		/* self-check failed; stop instead of guessing */
	}
      for (i = 0; i < extdata->n_items; i++)
	{
	  VSID vsid;
	  UINT64 bitmap = ~(UINT64) 0;	/* full sector: all 64 pages */
	  int npages = VOLMAP_SECT_NPAGES;
	  if (is_partial)
	    {
	      FILE_PARTIAL_SECTOR partsect;
	      memcpy (&partsect, items + i * expected_size, sizeof (partsect));
	      vsid = partsect.vsid;
	      bitmap = partsect.page_bitmap;
	      npages = __builtin_popcountll (bitmap);
	    }
	  else
	    {
	      memcpy (&vsid, items + i * expected_size, sizeof (vsid));
	    }

	  {
	    VOLMAP_VOLUME *sv = volmap_find_vol (ctx, vsid.volid);
	    if (sv != NULL && vsid.sectid >= 0 && vsid.sectid < sv->nsect_total)
	      {
		VOLMAP_FILE *f = &ctx->files[file_idx];

		sv->w_owner[vsid.sectid] = file_idx;
		sv->w_alloc[vsid.sectid] += npages;
		sv->w_pagebm[vsid.sectid] |= bitmap;
		f->sectors_seen++;
		f->alloc_pages += npages;
		/* remembered for the refreshes whose header LSA has not moved */
		if (f->nsects == f->sects_cap)
		  {
		    int want = (f->sects_cap > 0) ? f->sects_cap * 2 : 8;
		    VOLMAP_SECTREF *grown = (VOLMAP_SECTREF *) realloc (f->sects, (size_t) want * sizeof (*grown));

		    if (grown != NULL)
		      {
			f->sects = grown;
			f->sects_cap = want;
		      }
		  }
		if (f->nsects < f->sects_cap)
		  {
		    f->sects[f->nsects].volid = vsid.volid;
		    f->sects[f->nsects].sectid = vsid.sectid;
		    f->sects[f->nsects].bitmap = bitmap;
		    f->nsects++;
		  }
		else
		  {
		    f->hdr_lsa = 0;	/* list incomplete (no memory): never re-applied */
		  }
	      }
	  }
	}

      next = extdata->vpid_next;
      if (VPID_ISNULL (&next))
	{
	  break;
	}
      cur_vol = volmap_find_vol (ctx, next.volid);
      if (cur_vol == NULL || !volmap_read_iopage (cur_vol, next.pageid, cur))
	{
	  break;
	}
      offset = 0;
    }
  free (cur);
}

/* What a file entry takes from its header: identity, counts, the owning class.  Used
   when a file is first seen and when a header page turns out to hold another file of
   the same VFID (the entry is then re-made in place, the hash still finds it). */
static void
volmap_file_fill (VOLMAP_FILE * f, const FILE_HEADER * fhead)
{
  f->vfid = fhead->self;
  f->ftype = fhead->type;
  f->time_creation = fhead->time_creation;
  f->root_page = fhead->vpid_sticky_first.pageid;
  f->n_page_total = fhead->n_page_total;
  f->n_page_user = fhead->n_page_user;
  f->n_page_ftab = fhead->n_page_ftab;
  f->n_page_free = fhead->n_page_free;
  f->n_sector_total = fhead->n_sector_total;
  f->class_name[0] = '\0';
  f->index_name[0] = '\0';
  switch (fhead->type)
    {
    case FILE_HEAP:
    case FILE_HEAP_REUSE_SLOTS:
      f->class_oid = fhead->descriptor.heap.class_oid;
      break;
    case FILE_MULTIPAGE_OBJECT_HEAP:
      f->class_oid = fhead->descriptor.heap_overflow.class_oid;
      break;
    case FILE_BTREE:
      f->class_oid = fhead->descriptor.btree.class_oid;
      break;
    case FILE_BTREE_OVERFLOW_KEY:
      f->class_oid = fhead->descriptor.btree_key_overflow.class_oid;
      break;
    default:
      OID_SET_NULL (&f->class_oid);
      break;
    }
}

/* The LSA a header must carry for its sector list to be trusted on a later refresh.
   Pages of temporary volumes carry the constant PGBUF_TEMP_LSA {-2, -2} (page_buffer.h:
   pgbuf_init_temp_page_lsa; pgbuf_set_lsa leaves it alone), so their LSA never moves
   while their tables do - a temporary file's list is never re-applied.  0 = no cache. */
static UINT64
volmap_hdr_lsa_for_cache (const char *iopage)
{
  UINT64 raw;

  memcpy (&raw, iopage + offsetof (FILEIO_PAGE_RESERVED, lsa), sizeof (raw));
  if (volmap_lsa_pageid (raw) == -2 && volmap_lsa_offset (raw) == -2)
    {
      return 0;
    }
  return raw;
}

/*
 * volmap_try_file_header () - validate a candidate page as FILE_HEADER (self VFID must match its own
 *                             VPID) and, on success, harvest counts + sector ownership.
 */
static bool
volmap_try_file_header (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, PAGEID pageid, char *iopage)
{
  int prv = prv_user_offset ();
  const FILE_HEADER *fhead = (const FILE_HEADER *) (iopage + prv);
  VOLMAP_FILE *f;

  if (fhead->self.fileid != pageid || fhead->self.volid != vol->volid)
    {
      return false;
    }
  if (fhead->type < FILE_TRACKER || fhead->type >= FILE_LAST || fhead->n_page_total < 0
      || fhead->n_sector_total <= 0)
    {
      return false;
    }
  if (ctx->nfiles >= VOLMAP_MAX_FILES)
    {
      /* Said once: a file left out silently would make its sectors unowned and its
         pages idle in every figure that follows */
      static bool said = false;

      if (!said)
	{
	  said = true;
	  fprintf (stderr, "volmap: more than %d files - the rest are not shown; their sectors appear as unowned"
		   " and their pages as idle\n", VOLMAP_MAX_FILES);
	}
      return false;
    }

  f = &ctx->files[ctx->nfiles];
  memset (f, 0, sizeof (*f));
  volmap_file_fill (f, fhead);

  ctx->nfiles++;
  volmap_fidx_put (ctx, ctx->nfiles - 1);

  f->nsects = 0;
  f->hdr_lsa = volmap_hdr_lsa_for_cache (iopage);
  if (fhead->offset_to_partial_ftab >= 0)
    {
      volmap_walk_extdata (ctx, vol, iopage, fhead->offset_to_partial_ftab, true, ctx->nfiles - 1);
    }
  if (fhead->offset_to_full_ftab >= 0)
    {
      volmap_walk_extdata (ctx, vol, iopage, fhead->offset_to_full_ftab, false, ctx->nfiles - 1);
    }
  return true;
}

/* probe the first npages pages of one reserved sector; registers a file when a
 * page holds a valid, self-consistent FILE_HEADER */
static void
volmap_probe_sector (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, DKNSECTS s, char *iopage, int npages)
{
  int pg;

  for (pg = 0; pg < npages; pg++)
    {
      FILEIO_PAGE_RESERVED prv_area;
      PAGEID pageid = s * VOLMAP_SECT_NPAGES + pg;
      int k;
      bool known = false;

      if (pageid <= vol->sys_lastpage)
	{
	  continue;		/* volume metadata region cannot hold a FILE_HEADER */
	}
      if (pread (volmap_vol_fd (vol), &prv_area, sizeof (prv_area), (off_t) pageid * vol->iopagesize)
	  != (ssize_t) sizeof (prv_area))
	{
	  return;
	}
      if ((prv_area.pflag & 0x3) && vol->tde_bm != NULL
	  && !((vol->tde_bm[s] >> pg) & 1))
	{
	  /* TDE-encrypted (AES/ARIA): contents not interpretable.  Counted once per
	     page - this probe may be a repeat of an earlier one. */
	  vol->tde_bm[s] |= (UINT64) 1 << pg;
	  vol->tde_pages++;
	}
      if (prv_area.ptype != PAGE_FTAB || prv_area.pageid != pageid || prv_area.volid != vol->volid)
	{
	  continue;
	}
      known = volmap_file_index (ctx, vol->volid, pageid) >= 0;
      (void) k;
      if (!known && volmap_read_iopage (vol, pageid, iopage))
	{
	  (void) volmap_try_file_header (ctx, vol, pageid, iopage);
	}
    }
}

/* ---- the file tracker: the engine's own list of files -------------------------------
 *
 * Every file is registered in the tracker when it is created and unregistered when it
 * is destroyed (file_tracker_register/unregister).  Reading it gives the exact list of
 * files - wherever their headers happen to be - for a few pages: the tracker pages
 * (16 bytes per file) plus one header page per file.  Probing, which reads the first
 * page of every reserved sector, is then needed only for the sectors no listed file
 * owns: files being created or destroyed at that moment, and sectors a dropped file
 * still holds until vacuum frees them.
 *
 * The path is the engine's: volume 0's header names the boot heap (boot_hfid); the
 * first record of that heap is BOOT_DB_PARM, whose first field is trk_vfid - the same
 * position from 10.1 to 11.5; the tracker file's sticky first page starts the chain of
 * tracker pages (FILE_EXTENSIBLE_DATA, vpid_next).  Each step is checked against what
 * it must be (a heap header, a FILE_TRACKER header, 16-byte items), so a wrong guess
 * stops here instead of producing a wrong list; the probe then does all the work, as
 * before, and the report says the tracker was not read. */

/* FILE_TRACK_ITEM is private to file_manager.c; its 16-byte on-disk form is the
   same from 10.1 to 11.5 */
typedef struct volmap_track_item
{
  INT32 fileid;
  INT16 volid;
  INT16 type;
  unsigned char is_marked_deleted;	/* heap metadata: dropped, sectors freed by vacuum later */
  unsigned char pad[7];
} VOLMAP_TRACK_ITEM;

static size_t
volmap_fidx_hash (INT16 volid, INT32 fileid)
{
  unsigned long long h = ((unsigned long long) (unsigned short) volid << 32) ^ (unsigned int) fileid;

  h *= 0x9E3779B97F4A7C15ULL;
  return (size_t) (h >> 20);
}

/* record files[k] in the index (built on first use from the files already registered) */
static void
volmap_fidx_put (VOLMAP_CTX * ctx, int k)
{
  size_t i;

  if (ctx->fidx == NULL)
    {
      return;
    }
  i = volmap_fidx_hash (ctx->files[k].vfid.volid, ctx->files[k].vfid.fileid) & (ctx->fidx_cap - 1);
  while (ctx->fidx[i] != 0)
    {
      i = (i + 1) & (ctx->fidx_cap - 1);
    }
  ctx->fidx[i] = k + 1;
}

/* the index of the file with this VFID, or -1 */
static int
volmap_file_index (VOLMAP_CTX * ctx, INT16 volid, INT32 fileid)
{
  size_t i;

  if (ctx->fidx == NULL)
    {
      int k;

      ctx->fidx_cap = 2 * VOLMAP_MAX_FILES;	/* load factor <= 0.5: short probe runs */
      ctx->fidx = (int *) calloc (ctx->fidx_cap, sizeof (int));
      if (ctx->fidx == NULL)
	{
	  for (k = 0; k < ctx->nfiles; k++)	/* no memory for the index: the slow way */
	    {
	      if (ctx->files[k].vfid.volid == volid && ctx->files[k].vfid.fileid == fileid)
		{
		  return k;
		}
	    }
	  return -1;
	}
      for (k = 0; k < ctx->nfiles; k++)
	{
	  volmap_fidx_put (ctx, k);
	}
    }
  i = volmap_fidx_hash (volid, fileid) & (ctx->fidx_cap - 1);
  while (ctx->fidx[i] != 0)
    {
      const VOLMAP_FILE *f = &ctx->files[ctx->fidx[i] - 1];

      if (f->vfid.volid == volid && f->vfid.fileid == fileid)
	{
	  return ctx->fidx[i] - 1;
	}
      i = (i + 1) & (ctx->fidx_cap - 1);
    }
  return -1;
}

/* read page `pageid` of `vol` and check it is the FILE_HEADER of that very file and of
   `want_type` (FILE_LAST = any type); the page is left in iopage */
static bool
volmap_read_file_header (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, PAGEID pageid, int want_type, char *iopage)
{
  const FILE_HEADER *fhead;

  if (vol == NULL || pageid <= vol->sys_lastpage || (long) pageid >= (long) vol->nsect_total * VOLMAP_SECT_NPAGES
      || !volmap_read_iopage (vol, pageid, iopage))
    {
      return false;
    }
  fhead = (const FILE_HEADER *) (iopage + prv_user_offset ());
  if (fhead->self.fileid != pageid || fhead->self.volid != vol->volid)
    {
      return false;
    }
  if (fhead->type < FILE_TRACKER || fhead->type >= FILE_LAST || fhead->n_sector_total <= 0)
    {
      return false;
    }
  (void) ctx;
  return want_type == FILE_LAST || fhead->type == want_type;
}

/* Find the tracker: boot_hfid -> boot heap pages -> the record whose first 8 bytes name
   a FILE_TRACKER header.  Returns false with the reason in why. */
static bool
volmap_tracker_locate (VOLMAP_CTX * ctx, VPID * first, char *why, size_t wn)
{
  VOLMAP_VOLUME *vol0 = volmap_find_vol (ctx, 0);
  int prv = prv_user_offset ();
  char *page = NULL, *hpage = NULL;
  HFID hfid;
  int shifts[2], nshift, si, boot_idx = -1, vi;
  bool found = false;

  if (vol0 == NULL)
    {
      snprintf (why, wn, "volume 0 is not open");
      return false;
    }
  page = (char *) malloc (volmap_max_iopagesize (ctx));
  hpage = (char *) malloc (volmap_max_iopagesize (ctx));
  if (page == NULL || hpage == NULL || !volmap_read_iopage (vol0, 0, page))
    {
      snprintf (why, wn, "volume 0 header could not be read");
      free (page);
      free (hpage);
      return false;
    }
  /* boot_hfid sits after vol_creation, which v11.4 inserted: 8 bytes earlier on older
     volumes.  When the layout is known use it; otherwise try both, the header check
     below rejects the wrong one. */
  if (ctx->vlayout == VOLMAP_VLAY_114)
    {
      shifts[0] = 0;
      nshift = 1;
    }
  else if (ctx->vlayout == VOLMAP_VLAY_UNKNOWN)
    {
      shifts[0] = 0;
      shifts[1] = -8;
      nshift = 2;
    }
  else
    {
      shifts[0] = -8;
      nshift = 1;
    }
  for (si = 0; si < nshift && boot_idx < 0; si++)
    {
      memcpy (&hfid, page + prv + offsetof (DISK_VOLUME_HEADER, boot_hfid) + shifts[si], sizeof (hfid));
      if (hfid.vfid.volid != 0 || hfid.vfid.fileid <= 0 || hfid.hpgid <= 0)
	{
	  continue;
	}
      if (!volmap_read_file_header (ctx, vol0, hfid.vfid.fileid, FILE_HEAP, hpage))
	{
	  continue;
	}
      boot_idx = volmap_file_index (ctx, 0, hfid.vfid.fileid);
      if (boot_idx < 0 && volmap_try_file_header (ctx, vol0, hfid.vfid.fileid, hpage))
	{
	  boot_idx = ctx->nfiles - 1;	/* registered like any file: it is one */
	}
    }
  if (boot_idx < 0)
    {
      snprintf (why, wn, "boot heap not found from the volume 0 header");
      free (page);
      free (hpage);
      return false;
    }
  /* the boot heap's pages are known now (its sector tables were walked): scan their
     records for the one that names a FILE_TRACKER header */
  for (vi = 0; vi < ctx->nvols && !found; vi++)
    {
      VOLMAP_VOLUME *sv = &ctx->vols[vi];
      DKNSECTS s;

      for (s = 0; s < sv->nsect_total && !found; s++)
	{
	  int pg;

	  if (sv->w_owner[s] != boot_idx)
	    {
	      continue;
	    }
	  for (pg = 0; pg < VOLMAP_SECT_NPAGES && !found; pg++)
	    {
	      long lp = (long) s * VOLMAP_SECT_NPAGES + pg;
	      PAGEID pageid = (PAGEID) lp;

	      if (lp > INT32_MAX)
		{
		  break;
		}
	      const FILEIO_PAGE_RESERVED *pr;
	      const SPAGE_HEADER *sph;
	      int slotid;

	      if (!((sv->w_pagebm[s] >> pg) & 1) || (sv == vol0 && pageid == hfid.vfid.fileid)
		  || !volmap_read_iopage (sv, pageid, page))
		{
		  continue;
		}
	      pr = (const FILEIO_PAGE_RESERVED *) page;
	      sph = (const SPAGE_HEADER *) (page + prv);
	      if (pr->ptype != PAGE_HEAP || pr->pageid != pageid || pr->volid != sv->volid
		  || sph->num_slots <= 1 || sph->num_slots > sv->user_size / (int) sizeof (SPAGE_SLOT))
		{
		  continue;
		}
	      for (slotid = 1; slotid < sph->num_slots && !found; slotid++)
		{
		  const SPAGE_SLOT *slot = (const SPAGE_SLOT *) (page + prv + sv->user_size - sizeof (SPAGE_SLOT)) - slotid;
		  VFID cand;

		  if (slot->record_type != REC_HOME || slot->record_length < sizeof (VFID)
		      || slot->offset_to_record == 0 || slot->offset_to_record + sizeof (VFID) > (unsigned) sv->user_size)
		    {
		      continue;
		    }
		  memcpy (&cand, page + prv + slot->offset_to_record, sizeof (cand));
		  if (cand.volid != 0 || cand.fileid <= 0 || !volmap_read_file_header (ctx, vol0, cand.fileid, FILE_TRACKER, hpage))
		    {
		      continue;
		    }
		  *first = ((const FILE_HEADER *) (hpage + prv))->vpid_sticky_first;
		  found = !VPID_ISNULL (first);
		  if (found)
		    {
		      /* the tracker is a file too, the one file it does not list itself */
		      int ti = volmap_file_index (ctx, 0, cand.fileid);

		      if (ti < 0 && volmap_try_file_header (ctx, vol0, cand.fileid, hpage))
			{
			  ti = ctx->nfiles - 1;
			}
		      if (ti >= 0)
			{
			  ctx->files[ti].tracked = 1;
			  ctx->tracker_self = ti;
			}
		    }
		}
	    }
	}
    }
  if (!found)
    {
      snprintf (why, wn, "no record of the boot heap names a FILE_TRACKER header");
    }
  free (page);
  free (hpage);
  return found;
}

/* Walk the tracker pages and register every file they list.  Files already known are
   only marked.  Returns the number of files registered, -1 when the chain could not
   be read at all. */
static int
volmap_tracker_walk (VOLMAP_CTX * ctx, const VPID * first, char *why, size_t wn)
{
  int prv = prv_user_offset ();
  char *page = (char *) malloc (volmap_max_iopagesize (ctx));
  char *hpage = (char *) malloc (volmap_max_iopagesize (ctx));
  VPID cur = *first;
  int guard, registered = 0, pages = 0;
  bool trk_ok = true;
  VPID seen[4096];		/* the pages of this walk, for the cycle check (independent of trk_ok) */

  if (page == NULL || hpage == NULL)
    {
      free (page);
      free (hpage);
      snprintf (why, wn, "out of memory");
      return -1;
    }
  /* A walk already done: when no tracker page has moved (32-byte prefix each), the list
     is as it was and nothing needs looking at.  A page appended to the chain changes
     the previous last page's vpid_next, so that too moves an LSA. */
  ctx->rf_tracker_walked = false;
  if (ctx->trk_npages > 0)
    {
      int k;
      bool same = true;

      for (k = 0; k < ctx->trk_npages && same; k++)
	{
	  VOLMAP_VOLUME *vol = volmap_find_vol (ctx, ctx->trk_pages[k].volid);
	  FILEIO_PAGE_RESERVED pr;
	  UINT64 lsa;

	  same = vol != NULL
	    && pread (volmap_vol_fd (vol), &pr, sizeof (pr), (off_t) ctx->trk_pages[k].pageid * vol->iopagesize) == (ssize_t) sizeof (pr)
	    && pr.pageid == ctx->trk_pages[k].pageid && pr.volid == vol->volid;
	  if (same)
	    {
	      memcpy (&lsa, &pr.lsa, sizeof (lsa));
	      same = lsa == ctx->trk_lsa[k];
	    }
	}
      if (same)
	{
	  free (page);
	  free (hpage);
	  return 0;
	}
    }
  ctx->rf_tracker_walked = true;
  ctx->trk_npages = 0;
  ctx->trk_gen++;
  for (guard = 0; guard < 4096 && !VPID_ISNULL (&cur); guard++)
    {
      VOLMAP_VOLUME *vol = volmap_find_vol (ctx, cur.volid);
      const FILEIO_PAGE_RESERVED *pr;
      const FILE_EXTENSIBLE_DATA *extdata;
      const char *items;
      int i;

      if (vol == NULL || !volmap_read_iopage (vol, cur.pageid, page))
	{
	  break;
	}
      pr = (const FILEIO_PAGE_RESERVED *) page;
      extdata = (const FILE_EXTENSIBLE_DATA *) (page + prv);
      items = (const char *) extdata + DB_ALIGN (sizeof (FILE_EXTENSIBLE_DATA), MAX_ALIGNMENT);
      if (pr->ptype != PAGE_FTAB || pr->pageid != cur.pageid || pr->volid != vol->volid
	  || extdata->size_of_item != (int) sizeof (VOLMAP_TRACK_ITEM) || extdata->n_items < 0
	  || prv + (int) DB_ALIGN (sizeof (FILE_EXTENSIBLE_DATA), MAX_ALIGNMENT)
	  + extdata->n_items * (int) sizeof (VOLMAP_TRACK_ITEM) > vol->iopagesize)
	{
	  break;		/* not a tracker page: stop rather than guess */
	}
      /* a chain that comes back to a page already walked is damaged: stop here, as an
         incomplete walk (nothing is unlisted on its account) */
      {
	int k;

	for (k = 0; k < pages; k++)
	  {
	    if (VPID_EQ (&seen[k], &cur))
	      {
		break;
	      }
	  }
	if (k < pages)
	  {
	    break;
	  }
      }
      seen[pages] = cur;
      pages++;
      /* remember this page and its LSA for the next walk's check; one failed realloc
         gives up the check for this walk (every refresh then walks) */
      if (trk_ok)
	{
	  VPID *gp = (VPID *) realloc (ctx->trk_pages, (size_t) pages * sizeof (VPID));
	  UINT64 *gl = (UINT64 *) realloc (ctx->trk_lsa, (size_t) pages * sizeof (UINT64));

	  if (gp != NULL)
	    {
	      ctx->trk_pages = gp;
	    }
	  if (gl != NULL)
	    {
	      ctx->trk_lsa = gl;
	    }
	  if (gp != NULL && gl != NULL)
	    {
	      ctx->trk_pages[pages - 1] = cur;
	      memcpy (&ctx->trk_lsa[pages - 1], &pr->lsa, sizeof (UINT64));
	      ctx->trk_npages = pages;
	    }
	  else
	    {
	      trk_ok = false;
	      ctx->trk_npages = 0;
	    }
	}
      for (i = 0; i < extdata->n_items; i++)
	{
	  VOLMAP_TRACK_ITEM it;
	  VOLMAP_VOLUME *fv;
	  int idx;

	  memcpy (&it, items + i * sizeof (it), sizeof (it));
	  fv = volmap_find_vol (ctx, it.volid);
	  if (fv == NULL)
	    {
	      continue;		/* a volume that could not be opened: listed in SKIPPED VOLUMES */
	    }
	  idx = volmap_file_index (ctx, it.volid, it.fileid);
	  if (idx < 0)
	    {
	      /* The header is read and checked like a probed one; a file being created or
	         destroyed right now can fail that, and then the probe may still find it */
	      if (!volmap_read_file_header (ctx, fv, it.fileid, FILE_LAST, hpage)
		  || !volmap_try_file_header (ctx, fv, it.fileid, hpage))
		{
		  continue;
		}
	      idx = ctx->nfiles - 1;
	      registered++;
	    }
	  ctx->files[idx].tracked = (it.is_marked_deleted && (it.type == FILE_HEAP || it.type == FILE_HEAP_REUSE_SLOTS)) ? 2 : 1;
	  ctx->files[idx].trk_seen = ctx->trk_gen;
	}
      cur = extdata->vpid_next;
    }
  /* a walk that reached the end of the chain is the whole list: a file it did not
     name is no longer tracked (dropped and unregistered, its header still on disk) */
  if (pages > 0 && VPID_ISNULL (&cur))
    {
      int k;

      for (k = 0; k < ctx->nfiles; k++)
	{
	  if (ctx->files[k].trk_seen != ctx->trk_gen && k != ctx->tracker_self)
	    {
	      ctx->files[k].tracked = 0;
	    }
	}
    }
  free (page);
  free (hpage);
  if (pages == 0)
    {
      snprintf (why, wn, "first tracker page is not a tracker page");
      return -1;
    }
  return registered;
}

/* Read the tracker once per run; pass A then probes only the sectors still unowned */
static void
volmap_discover_tracker (VOLMAP_CTX * ctx)
{
  char why[120] = "";
  int n;

  if (ctx->no_tracker || ctx->tracker_state != 0)
    {
      return;
    }
  if (!volmap_tracker_locate (ctx, &ctx->tracker_first, why, sizeof (why)))
    {
      ctx->tracker_state = -1;
      snprintf (ctx->tracker_why, sizeof (ctx->tracker_why), "%s", why);
      return;
    }
  n = volmap_tracker_walk (ctx, &ctx->tracker_first, why, sizeof (why));
  if (n < 0)
    {
      ctx->tracker_state = -1;
      snprintf (ctx->tracker_why, sizeof (ctx->tracker_why), "%s", why);
      return;
    }
  ctx->tracker_state = 1;
  ctx->files_tracked = 0;
  for (n = 0; n < ctx->nfiles; n++)
    {
      ctx->files_tracked += (ctx->files[n].tracked != 0);
    }
}

/* With the tracker read, the probe no longer visits the sectors listed files own, and
   those are where the TDE flag is seen.  Keep that observation as a sample: the first
   page's 32-byte prefix of up to 64 owned sectors per volume, spread over the volume. */
#define VOLMAP_TDE_SAMPLE_SECTORS 64
static void
volmap_tde_sample (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol)
{
  DKNSECTS s, step = vol->nsect_total / VOLMAP_TDE_SAMPLE_SECTORS;

  if (ctx->tracker_state != 1 || vol->tde_bm == NULL)
    {
      return;
    }
  if (step < 1)
    {
      step = 1;
    }
  for (s = 0; s < vol->nsect_total; s += step)
    {
      FILEIO_PAGE_RESERVED prv_area;
      long lp = (long) s * VOLMAP_SECT_NPAGES;
      PAGEID pageid = (PAGEID) lp;

      if (lp > INT32_MAX || !vol->w_stab[s] || vol->w_owner[s] < 0 || pageid <= vol->sys_lastpage || ((vol->tde_bm[s] >> 0) & 1))
	{
	  continue;
	}
      if (pread (volmap_vol_fd (vol), &prv_area, sizeof (prv_area), (off_t) pageid * vol->iopagesize)
	  != (ssize_t) sizeof (prv_area))
	{
	  return;
	}
      if ((prv_area.pflag & 0x3) && prv_area.pageid == pageid && prv_area.volid == vol->volid)
	{
	  vol->tde_bm[s] |= (UINT64) 1;
	  vol->tde_pages++;
	}
    }
}

/* begin progressive discovery: allocate state; the actual probing happens in
 * volmap_discover_step() slices driven from the interactive idle loop */
static int
volmap_scan_begin (VOLMAP_CTX * ctx)
{
  int vi;
  DKNSECTS s;

  ctx->files = (VOLMAP_FILE *) calloc (VOLMAP_MAX_FILES, sizeof (VOLMAP_FILE));
  ctx->scan_pos = (DKNSECTS *) calloc (ctx->nvols, sizeof (DKNSECTS));
  if (ctx->files == NULL || ctx->scan_pos == NULL)
    {
      return ER_FAILED;
    }
  ctx->scan_total = 0;
  ctx->scan_probed = 0;
  ctx->scan_pref = 0;
  /* the listed files first (a few pages): the slices then skip the sectors they own,
     so on a database whose tracker is readable the scan is over almost at once */
  volmap_discover_tracker (ctx);
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      for (s = 0; s < ctx->vols[vi].nsect_total; s++)
	{
	  ctx->scan_total += ctx->vols[vi].stab[s];
	}
      volmap_tde_sample (ctx, &ctx->vols[vi]);
    }
  ctx->scan_active = true;
  return NO_ERROR;
}

/* scan order: the on-screen volume first, then the rest OUTSIDE-IN
 * (0, N-1, 1, N-2, ...) instead of ascending - the tail volumes hold the
 * newest data, so a user who jumps to the end finds it scanned right after
 * the volume they are looking at. Within a volume the order stays
 * sequential (the WILLNEED run-collapsing depends on it). */
static int
volmap_scan_order (VOLMAP_CTX * ctx, int oi)
{
  int lo = 0, hi = ctx->nvols - 1, from_lo = 1;

  if (oi == 0)
    {
      return ctx->scan_pref;
    }
  while (lo <= hi)
    {
      int v = from_lo ? lo++ : hi--;

      from_lo = !from_lo;
      if (v == ctx->scan_pref)
	{
	  continue;
	}
      if (--oi == 0)
	{
	  return v;
	}
    }
  return ctx->scan_pref;	/* oi out of range: harmless (volume is already complete) */
}

/* probe up to `budget` reserved sectors, the on-screen volume first.
 * A slice also ends after ~50ms or as soon as a key is pending, so the UI
 * stays responsive even when the disk is cold or loaded (a sector-count
 * budget alone can take seconds under those conditions).
 * Returns true when the whole scan is complete. */
static bool
volmap_discover_step (VOLMAP_CTX * ctx, long budget, bool user_idle)
{
  int oi;
  char *iopage;
  int maxio = 0;
  struct timespec t0, tn;

  clock_gettime (CLOCK_MONOTONIC, &t0);

  if (!ctx->scan_active)
    {
      return true;
    }
  for (oi = 0; oi < ctx->nvols; oi++)
    {
      maxio = MAX (maxio, ctx->vols[oi].iopagesize);
    }
  iopage = (char *) malloc (maxio);
  if (iopage == NULL)
    {
      ctx->scan_active = false;	/* give up cleanly: the 30ms repaint loop must not spin forever */
      return true;
    }
  for (oi = 0; oi < ctx->nvols && budget > 0; oi++)
    {
      int vi = volmap_scan_order (ctx, oi);
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      DKNSECTS *pos = &ctx->scan_pos[vi];
      DKNSECTS ahead;
      /* deep readahead is what makes a cold scan fast, but it also floods the
       * disk queue and starves the UI's own reads - so go deep only while the
       * user is idle, and stay shallow while keys are coming in */
      long b0 = user_idle ? budget : ((budget > 512) ? 512 : budget);

      if (*pos >= vol->nsect_total)
	{
	  continue;
	}
      /* queue readahead for this slice's header probes; contiguous reserved runs
       * collapse into one fadvise call instead of one per sector */
      for (ahead = *pos; ahead < vol->nsect_total && b0 > 0;)
	{
	  DKNSECTS run0 = ahead;

	  while (ahead < vol->nsect_total && b0 > 0 && vol->w_stab[ahead] && vol->w_owner[ahead] < 0)
	    {
	      ahead++;
	      b0--;
	    }
	  if (ahead > run0)
	    {
	      /* prefetch ONLY each sector's first page (the header probe target).
	       * One WILLNEED over the whole run span would drag the entire byte
	       * range into the page cache: measured 9.8GB disk read on a truly
	       * cold 90GB DB versus ~1.4GB for the headers actually probed. */
	      DKNSECTS pf;

	      for (pf = run0; pf < ahead; pf++)
		{
		  (void) posix_fadvise (volmap_vol_fd (vol), (off_t) pf * VOLMAP_SECT_NPAGES * vol->iopagesize,
					vol->iopagesize, POSIX_FADV_WILLNEED);
		}
	    }
	  else
	    {
	      ahead++;
	    }
	}
      while (*pos < vol->nsect_total && budget > 0)
	{
	  DKNSECTS s = (*pos)++;

	  if (!vol->w_stab[s])
	    {
	      continue;
	    }
	  if (vol->w_owner[s] >= 0)
	    {
	      ctx->scan_probed++;	/* a listed file's sector: counted as done, not read */
	      continue;
	    }
	  budget--;
	  ctx->scan_probed++;
	  ctx->probe_sectors++;
	  volmap_probe_sector (ctx, vol, s, iopage, 1);
	  if ((ctx->scan_probed & 0x7F) == 0)
	    {
	      fd_set rf;
	      struct timeval tv = { 0, 0 };

	      clock_gettime (CLOCK_MONOTONIC, &tn);
	      if ((tn.tv_sec - t0.tv_sec) * 1000 + (tn.tv_nsec - t0.tv_nsec) / 1000000 > (user_idle ? 300 : 50))
		{
		  budget = 0;	/* time slice used up: yield to the UI (and repaint progress) */
		  break;
		}
	      if (!volmap_mt_run)
		{
		  FD_ZERO (&rf);
		  FD_SET (0, &rf);
		  if (select (1, &rf, NULL, NULL, &tv) > 0)
		    {
		      budget = 0;	/* a key is waiting: yield immediately */
		      break;
		    }
		}
	    }
	}
    }
  free (iopage);
  for (oi = 0; oi < ctx->nvols; oi++)
    {
      if (ctx->scan_pos[oi] < ctx->vols[oi].nsect_total)
	{
	  return false;
	}
    }
  ctx->scan_active = false;
  (void) volmap_discover_files_from (ctx, 1);	/* pass B; pass A was done incrementally */
  return true;
}

/*
 * volmap_discover_files () - self-discovery without server/catalog.
 *
 * A file header page is usually the first page a file allocates from its freshly reserved sector, so
 * pass A probes only the first page of every reserved sector (prv is prefetched with posix_fadvise and
 * read in ascending offset order).  Not always: a heap or B-tree created while vacuum still holds a
 * dropped file's VFID takes a later page.  Pass B therefore probes every page of the sectors pass A
 * left without an owner - on its own when there are at most VOLMAP_AUTO_SWEEP_MAX of them (a few on a
 * healthy database), all of them with --full-sweep.
 */
#define VOLMAP_AUTO_SWEEP_MAX 256	/* sectors: at most 256MB read with 16KB pages */

static long
volmap_unowned_sectors (VOLMAP_CTX * ctx)
{
  long n = 0;
  int vi;
  DKNSECTS s;

  for (vi = 0; vi < ctx->nvols; vi++)
    {
      for (s = 0; s < ctx->vols[vi].nsect_total; s++)
	{
	  n += (ctx->vols[vi].w_stab[s] && ctx->vols[vi].w_owner[s] < 0) ? 1 : 0;
	}
    }
  return n;
}

static int
volmap_discover_files_from (VOLMAP_CTX * ctx, int first_pass)
{
  int vi, pass;
  char *iopage;
  int npasses = 2;

  if (ctx->files == NULL)
    {
      ctx->files = (VOLMAP_FILE *) calloc (VOLMAP_MAX_FILES, sizeof (VOLMAP_FILE));
      if (ctx->files == NULL)
	{
	  return ER_FAILED;
	}
    }

  if (first_pass == 0)
    {
      volmap_discover_tracker (ctx);	/* the listed files first: pass A then skips what they own */
    }
  for (pass = first_pass; pass < npasses; pass++)
    {
      if (pass == 1 && !ctx->full_sweep)
	{
	  long left = volmap_unowned_sectors (ctx);

	  ctx->sweep_left = (left > VOLMAP_AUTO_SWEEP_MAX) ? left : 0;
	  if (left == 0 || left > VOLMAP_AUTO_SWEEP_MAX)
	    {
	      break;		/* nothing to look for, or more than a quick look: --full-sweep */
	    }
	}
      for (vi = 0; vi < ctx->nvols; vi++)
	{
	  VOLMAP_VOLUME *vol = &ctx->vols[vi];
	  DKNSECTS s;
	  PAGEID pageid;

	  /* prefetch hint for the strided probe; a sector a listed file owns is not probed */
	  for (s = 0; s < vol->nsect_total; s++)
	    {
	      if (!vol->w_stab[s] || vol->w_owner[s] >= 0)
		{
		  continue;
		}
	      pageid = s * VOLMAP_SECT_NPAGES;
	      (void) posix_fadvise (volmap_vol_fd (vol), (off_t) pageid * vol->iopagesize,
				    (pass == 0 ? sizeof (FILEIO_PAGE_RESERVED) :
				     (off_t) VOLMAP_SECT_NPAGES * vol->iopagesize), POSIX_FADV_WILLNEED);
	    }

	  iopage = (char *) malloc (vol->iopagesize);
	  if (iopage == NULL)
	    {
	      return ER_FAILED;
	    }
	  for (s = 0; s < vol->nsect_total; s++)
	    {
	      if (!vol->w_stab[s] || vol->w_owner[s] >= 0)
		{
		  continue;
		}
	      if (pass == 0)
		{
		  ctx->probe_sectors++;
		}
	      volmap_probe_sector (ctx, vol, s, iopage, (pass == 0) ? 1 : VOLMAP_SECT_NPAGES);
	    }
	  free (iopage);
	  if (pass == 0)
	    {
	      volmap_tde_sample (ctx, vol);
	    }
	}
    }
  return NO_ERROR;
}

static int
volmap_discover_files (VOLMAP_CTX * ctx)
{
  return volmap_discover_files_from (ctx, 0);
}

/* DONTNEED the 4KB OS pages in [o0, o1) that were not cached when the scan began
   (prevec), in coalesced runs.  count: add to the returned/kept figures (the sector
   itself); the readahead window after it is returned without counting, or the same
   pages would be counted at every sector whose window covers them. */
static void
volmap_deep_return (VOLMAP_VOLUME * vol, const unsigned char *prevec, size_t o0, size_t o1, bool count)
{
  size_t o, r0 = (size_t) -1;

  if (prevec == NULL)
    {
      return;
    }
  for (o = o0; o <= o1; o++)
    {
      bool cold = (o < o1) && !(prevec[o] & 1);

      if (cold && r0 == (size_t) -1)
	{
	  r0 = o;
	}
      else if (!cold && r0 != (size_t) -1)
	{
	  (void) posix_fadvise (volmap_vol_fd (vol), (off_t) r0 * 4096, (off_t) (o - r0) * 4096, POSIX_FADV_DONTNEED);
	  if (count)
	    {
	      vol->deep_cache_returned += (INT64) (o - r0) * 4096;
	    }
	  r0 = (size_t) -1;
	}
      if (count && o < o1 && (prevec[o] & 1))
	{
	  vol->deep_cache_kept += 4096;
	}
    }
}

/*
 * volmap_deep_scan () - L4: sequential sector reads; record density from the persisted SPAGE_HEADER and
 *                       forwarding ratio from the slot array record types. No page buffer involved.
 */
static void
volmap_deep_scan (VOLMAP_CTX * ctx)
{
  int vi;
  int prv = prv_user_offset ();

  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      char *sectbuf;
      DKNSECTS s;

      if (!volmap_vol_selected (ctx, vol->volid))
	{
	  continue;
	}
      size_t sect_bytes = (size_t) VOLMAP_SECT_NPAGES * vol->iopagesize;
      size_t os_per_sect = sect_bytes / 4096;
      size_t maplen = (size_t) vol->nsect_total * sect_bytes;
      size_t pre_os_pages = (maplen + 4095) / 4096;
      unsigned char *prevec = NULL;	/* 1B per 4KB OS page: cached BEFORE the scan began */
      /* How far past a read the kernel may have pulled pages in: the device's
         read_ahead_kb, or 4MB when the device is not known (overlay) - the largest
         value seen.  Only that window after each sector is given back, never the
         whole volume: what the server loads elsewhere during a long scan stays. */
      size_t ra_os_pages = 4 * 1024 * 1024 / 4096;
      {
	int maj, min;
	char sys[256];
	FILE *f;
	long kb;

	if (volmap_blockdev_of (vol->path, &maj, &min) == 0)
	  {
	    snprintf (sys, sizeof (sys), "/sys/dev/block/%d:%d/queue/read_ahead_kb", maj, min);
	    f = fopen (sys, "r");
	    if (f == NULL)
	      {
		snprintf (sys, sizeof (sys), "/sys/dev/block/%d:%d/../queue/read_ahead_kb", maj, min);
		f = fopen (sys, "r");
	      }
	    if (f != NULL)
	      {
		if (fscanf (f, "%ld", &kb) == 1 && kb >= 0)
		  {
		    ra_os_pages = (size_t) kb * 1024 / 4096;
		  }
		fclose (f);
	      }
	  }
      }

      /* What the OS had cached when the scan began (the server's warm set, the discovery
         pass) is what stays.  The snapshot is not retaken right before each sector: the
         kernel's readahead from the previous read would already have pulled the next
         sectors in and they would look cached (measured on a 4MB readahead; FADV_RANDOM
         did not stop it on overlayfs).  mmap+mincore reads kernel metadata only. */
      {
	void *base = mmap (NULL, maplen, PROT_READ, MAP_SHARED, volmap_vol_fd (vol), 0);

	if (base != MAP_FAILED)
	  {
	    prevec = (unsigned char *) malloc (pre_os_pages);
	    if (prevec == NULL || mincore (base, maplen, prevec) != 0)
	      {
		free (prevec);
		prevec = NULL;	/* snapshot failed: keep everything (never evict blindly) */
	      }
	    munmap (base, maplen);
	  }
      }
      sectbuf = (char *) malloc (sect_bytes);
      if (sectbuf == NULL)
	{
	  free (prevec);
	  return;
	}
      (void) posix_fadvise (volmap_vol_fd (vol), 0, 0, POSIX_FADV_SEQUENTIAL);
      for (s = 0; s < vol->nsect_total; s++)
	{
	  int pg;

	  if (!vol->stab[s])
	    {
	      continue;
	    }
	  if (pread (volmap_vol_fd (vol), sectbuf, sect_bytes, (off_t) s * sect_bytes) != (ssize_t) sect_bytes)
	    {
	      continue;
	    }
	  for (pg = 0; pg < VOLMAP_SECT_NPAGES; pg++)
	    {
	      char *iopage = sectbuf + (size_t) pg * vol->iopagesize;
	      FILEIO_PAGE_RESERVED *p = (FILEIO_PAGE_RESERVED *) iopage;
	      PAGEID pageid = s * VOLMAP_SECT_NPAGES + pg;

	      /* A reserved sector may hold pages that are not allocated: after a drop or
	         a truncate the old header survives in place, and counting it would add a
	         deleted object's records and free space to the current figures.  The
	         drill-down checks the same bit before it reads a page. */
	      if (!((vol->pagebm[s] >> pg) & 1))
		{
		  continue;	/* sector reserved, page not allocated */
		}
	      if (p->pageid != pageid || p->volid != vol->volid)
		{
		  continue;	/* uninitialized or torn page; skip */
		}
	      /* The body of a TDE page is ciphertext.  Reading it as a slotted page would
	         let random bytes pass the range checks below and feed invented records and
	         densities into the figures - this tool does not decrypt, so the page is
	         left out and counted separately instead. */
	      if ((p->pflag & 0x3) != 0)
		{
		  vol->deep_tde_skipped++;
		  continue;
		}
	      if (p->ptype == PAGE_HEAP || p->ptype == PAGE_BTREE || p->ptype == PAGE_OVERFLOW)
		{
		  SPAGE_HEADER *sphdr = (SPAGE_HEADER *) (iopage + prv);
		  if (sphdr->num_slots >= 0 && sphdr->num_slots <= vol->user_size / (int) sizeof (SPAGE_SLOT) && sphdr->total_free >= 0
		      && sphdr->total_free <= vol->user_size)
		    {
		      int slotid;
		      vol->deep_data_pages++;
		      vol->deep_free_bytes += sphdr->total_free;
		      vol->deep_recs += sphdr->num_records;
		      for (slotid = 0; slotid < sphdr->num_slots; slotid++)
			{
			  SPAGE_SLOT *slot =
			    (SPAGE_SLOT *) (iopage + prv + vol->user_size - sizeof (SPAGE_SLOT)) - slotid;
			  vol->deep_slots++;
			  if (slot->record_type == REC_RELOCATION || slot->record_type == REC_NEWHOME)
			    {
			      vol->deep_fwd_slots++;
			    }
			}
		    }
		}
	    }
	  /* Give this sector back now, not after the whole volume, and with it the
	     readahead window behind it (the next sectors, whether the scan reads them or
	     skips them): the scan holds at most a sector (1MB at 16KB pages) plus that
	     window.  Nothing outside the window is touched, so pages the server loads
	     elsewhere while the scan runs are not evicted. */
	  {
	    size_t o1 = (size_t) (s + 1) * os_per_sect;
	    size_t o2 = (o1 + ra_os_pages < pre_os_pages) ? o1 + ra_os_pages : pre_os_pages;

	    volmap_deep_return (vol, prevec, (size_t) s * os_per_sect, o1, true);
	    volmap_deep_return (vol, prevec, o1, o2, false);
	  }
	}
      free (prevec);
      free (sectbuf);
    }
}

/* ── rendering ─────────────────────────────────────────────────────────── */

/* ASCII-art texture rendering — no color, no attribute, no unicode dependence.
 * Classic ASCII-art practice: legibility comes from CHARACTER SHAPE CLASSES, not color.
 * Each kind is a shape family; files of the same kind cycle through characters of that
 * family, so adjacent different files always render differently:
 * Default output applies the same idea with unicode glyph FAMILIES (solid / woven /
 * shaded / bars), --plain falls back to pure ASCII families:
 *   plain:  DATA "@08"  INDEX "/\\X"  CATALOG "+=*"  SYSTEM "$"  TEMP "~"
 *   glyph:  DATA \u2588\u259b\u259f  INDEX \u2593\u259a\u259e  CATALOG \u2592\u2591
 *           SYSTEM \u258c\u2590  TEMP \u2581\u2594
 *   '_' reserved-but-empty (hollow)   '.' unreserved (faint ground)   '?' unknown */
static const char *kind_family[5] = { "@08", "/\\X", "+=*", "$", "~" };
/* fill-ramp glyph sets (user-specified): every kind expresses page fill by shape.
 *   DATA    braille fill-up: U+28FF full  U+28F6 75%  U+28E4 50%  U+28C0 <=25%
 *   INDEX   U+25A9 full  U+25A8 75%  U+25A6 50%  U+25AA <=25%           (squares)
 *   CAT/SYS U+2593 full  U+2592 60%  U+2591 <=30%                        (shades)
 *   TEMP    U+25CF full  U+25D5 75%  U+25D3 50%  U+25D4 <=25%           (circles) */
/* braille 5-step ramp: 100%% \u28FF, 80%% \u28FE, 60%% \u28F6, 40%% \u28E4, 20%% \u28C0 */
static const char *kind_ramp[5] = {
  "\xe2\xa0\xbf", "\xe2\xa0\xbe", "\xe2\xa0\xb6", "\xe2\xa0\xb4", "\xe2\xa0\xa4"
};				/* braille fill ramp: 100/80/60/40/20% (shared by every kind) */
/* muted per-file palette (catalog/system/temp): adjacent files land on different hues */
static const int file_palette[6] = { 109, 143, 138, 73, 180, 103 };


/* interactive UI language: 0=English (default), 1=Korean — toggled with [l].
 * Batch output stays English (documentation and goldens are keyed to it). */
static int volmap_lang_ko = 0;
static int volmap_ascii_frame = 0;	/* [g]: +-| borders for clients that render
					 * box-drawing (EAW-ambiguous) double-width */
#define OVG(utf8, ascii) (volmap_ascii_frame ? (ascii) : (utf8))
/* heap/index tags: PREFIX badges - a single ASCII letter with its PADDING
 * colored, i.e. a 3-column chip " H " / " I ".  Pure ASCII ends two field
 * failures for good: circled letters (EAW=Ambiguous - glyph drawn 2 cells
 * wide while the bg stays on 1, half-colored chip) and small capitals
 * (Phonetic Extensions block missing from Windows default fonts - tofu).
 * The color already carries the kind; the letter is only a label.  Mode-
 * independent: [g] now affects borders only.
 *   easy revert - small caps: "\xca\x9c\xe1\xb4\x98" / "\xc9\xaax"  circled: "\xe2\x93\x97" / "\xe2\x93\x98" */
#define VOLMAP_TAG_H "\033[44;97;1m H \033[0m "
#define VOLMAP_TAG_I "\033[42;30;1m I \033[0m "
/* last-drawn overlay geometry, for mouse hit-testing inside the boxes */
#define VOLMAP_OV_W_MIN 32	/* sector grid width; 64 pages = 2 rows, changing it walks the arrows off the sector */
/* Page/slots/info box content width.  40 so the five legend entries
   (hdr rec free dir frag = 35 columns) fit on one row. */
#define VOLMAP_OV_PAGE_W 32	/* 2 sets (16x2); full box width = 32+4 = 36 */
/* Overlay content width grows with the screen.  With 64B cells a wider box
   also means fewer page-grid rows (256 cells / width), which helps on short
   screens.  Kept a multiple of 8 so existing alignment assumptions hold. */
/* Drill-down overlay width is fixed at 32.  Sector grid row count (64 pages),
   page grid row count (256 cells) and key/mouse step are all derived from it,
   so making it a runtime variable breaks all three at once.
   The file view is a separate box and keeps its own width (volmap_fv_w). */
#define VOLMAP_FV_W_MIN 32	/* file view minimum content width; independent of the drill-down set rule */
static int volmap_fv_w = VOLMAP_FV_W_MIN;	/* file view content width */
/* Cursor style: 0 = blue background, 1 = border highlight.
   The blue background shares the background channel with -m residency shading
   (grey 240/237) and can be misread as "this cell is cached"; the border style
   leaves the background alone.  Toggled live with [c]. */
static int volmap_cursor_border = 0;	/* default: blue background */
/* One cursor colour for both the map and the drill-down.  Orange (208) clashes
   with neither the kind colours (blue/green/red/purple) nor the residency greys
   (237/240), so it reads as the cursor over any background. */
#define VOLMAP_CURSOR_SGR "\033[48;5;208m\033[38;5;16m\033[1m"
/* Drill-down box content width: 64 when the overlay can sit outside the map
   (sector's 64 pages on one row), otherwise 40 (two rows of 32).  Decided once
   at the start of a frame and treated as constant within it, so every grid that
   derives its row count from this width sees the same value. */
/* Written only by volmap_ov_w_set(), once per frame; read via VOLMAP_OV_W.
   Exposing the global directly let any site assign to it, and a mid-frame change
   left grids that had already computed with the old width inconsistent. */
static int volmap_ov_w_cur = VOLMAP_OV_PAGE_W;

static void
volmap_ov_w_set (int w)
{
  volmap_ov_w_cur = (w > 0) ? w : VOLMAP_OV_PAGE_W;
}

#define VOLMAP_OV_W ((int) volmap_ov_w_cur)
#define VOLMAP_OV_WIDE_W 64	/* 4 sets (16x4); full box width = 64+4 = 68 */
#define VOLMAP_OV_MID_W 48	/* 3 sets (16x3); full box width = 48+4 = 52 */
#define VOLMAP_OV_NARROW_W 16	/* 1 set (16x1); full box width = 16+4 = 20, for 80-column screens */
#define VOLMAP_OV_RIGHT_MIN_COLS 300	/* only at this width is placement right of the map considered */
#define VOLMAP_OV_RIGHT_MIN_ROOM 25	/* minimum margin outside the map outline for right placement */

/* Overlay content width.
     80 <= screen < 300: box width (content+4) must not exceed 1/3 of the map
       outline.  Candidates 64 (sector on one row) then 40 (two rows); 40 is kept
       even if it exceeds 1/3, since the 2x32 sector grid cannot shrink further.
     screen >= 300: largest candidate that fits the margin right of the map,
       otherwise fall back to the 1/3 rule.
   frame_w is the full map outline width including borders. */
static int
volmap_ov_pick_w (int scr_cols, int frame_w)
{
  /* The drill-down uses fixed sizes: a 32xN or 64xN grid, plus 4 columns of
     border and padding.  It stays at that size as the screen grows; the spare
     width goes to the variable-width map, so the two never overlap.
     Candidates are tried widest first, leaving the map at least 24 columns. */
  /* width candidates are 36 and 68 only (grid 32/64) */
  static const int cand[] = { VOLMAP_OV_WIDE_W, VOLMAP_OV_PAGE_W };
  static const int ncand = (int) (sizeof (cand) / sizeof (cand[0]));
  int i;
  int room = (frame_w > 0) ? frame_w : scr_cols;

  /* Pick the largest fixed width that fits inside the map frame with margins.
     The default placement is a window floating inside the map, so the map - not
     the screen - is the reference. */
  for (i = 0; i < ncand; i++)
    {
      /* (1) The box plus both insets (3+3) must fit the map content width
	    (frame minus 4 border columns).
	 (2) The box must not exceed about 1/3 of the map outline - past that it
	    stops being a floating window and becomes a panel covering the map. */
      if (room - 4 >= (cand[i] + 4) + 6 && (cand[i] + 4) * 3 <= room)
	{
	  return cand[i];
	}
    }
  return VOLMAP_OV_PAGE_W;	/* very narrow screen: smallest size (36) */
}

/* Drill-down height and grid allocation - single decision point.
   This function alone decides the row count of all four boxes and the width and
   row count of both grids (sector, page).  Drawing, mouse hit-testing and arrow
   movement all derive from the result (panel.rows / panel.w), so they cannot
   disagree.

   Rules:
     - Sector grid covers one sector's 64 pages.  1..4 rows, width = ceil(64/rows)
       clamped to the box width.  With height to spare, 4 rows of 16 read best.
       If 64 cells do not fit, the last row gets '+' and the grid scrolls.
     - Page grid rows = ceil(sector rows * 1.5).
     - Slots and info keep a minimum row count; any shortfall is taken from the
       page grid first, then the sector grid. */
#define VOLMAP_OV_SECT_MAX_ROWS 4
#define VOLMAP_OV_SECT_MIN_ROWS 2	/* the sector grid never flattens to a single row */
#define VOLMAP_OV_SECT_ROW_W 32		/* cells per sector row; 64 pages = 32 cells x 2 rows */
#define VOLMAP_MAP_CELL_STEP 16		/* map cell size unit in pages; 16p = 256KB */
#define VOLMAP_OV_SECT_MIN_W 8	/* narrower than this and the sense of page position is lost */

/* Cells per grid row - the single rule shared by all three grids
   (sector, page, slots).  One set is 16 cells; as many sets are packed as the
   box content width allows (16/32/48/64...).  Both 64 (sector) and 256 (page)
   are multiples of 16, so rows always end evenly at any set count. */
#define VOLMAP_OV_SET_CELLS 16
static int
volmap_ov_grid_w (int box_w)
{
  int sets = box_w / VOLMAP_OV_SET_CELLS;

  if (sets < 1)
    {
      sets = 1;			/* below 16 columns use one set; drawing clips it */
    }
  return sets * VOLMAP_OV_SET_CELLS;
}

typedef struct volmap_ov_layout VOLMAP_OV_LAYOUT;
struct volmap_ov_layout
{
  int sect_rows, sect_w;	/* sector grid (one cell = one page) */
  int page_rows;		/* page box rows (grid + 2 legend rows) */
  int box_w;			/* box content width */
};
static VOLMAP_OV_LAYOUT volmap_ov_lo = { 2, 32, 6, VOLMAP_OV_PAGE_W };

/* State of one grid: width, rows, scroll position.
   Sector and page grids follow the same contract - a window of w x rows cells,
   showing from 'base', with 'more' set when cells remain past the window.
   Each grid owns its VOLMAP_GRID so drawing, mouse and arrow handling all read
   the same values.  Ownership: input handling and clamp write, drawing reads. */
typedef struct volmap_grid VOLMAP_GRID;
struct volmap_grid
{
  int w;			/* cells per row */
  int rows;			/* visible rows */
  int base;			/* first cell index in the window */
  int more;			/* 1 = more follows ('+' shown) */
};

/* Settle which part of 'total' cells the window covers: pull an out-of-range
   base back inside and set 'more'.  With sel >= 0, move the window so that cell
   is visible. */
static void
volmap_grid_clamp (VOLMAP_GRID * g, int total, int sel)
{
  int vis = g->w * g->rows;

  if (g->w <= 0 || g->rows <= 0)
    {
      return;
    }
  if (vis >= total)
    {
      g->base = 0;
      g->more = 0;
      return;
    }
  if (sel >= 0)
    {
      if (sel < g->base)
	{
	  g->base = (sel / g->w) * g->w;
	}
      else if (sel >= g->base + vis)
	{
	  g->base = ((sel - vis) / g->w + 1) * g->w;
	}
    }
  if (g->base > total - vis)
    {
      g->base = total - vis;
    }
  if (g->base < 0)
    {
      g->base = 0;
    }
  g->more = (g->base + vis < total);
}

#define VOLMAP_OV_BROWS 14	/* byte-distribution box rows: >=12 keeps the del/vac footers + legend */
/* Minimum rows per drill-down box including borders; 19 in total is the
   smallest drill-down.  On shorter screens no box is dropped - each scrolls. */
#define VOLMAP_OV_MIN_SECT  3
#define VOLMAP_OV_MIN_PAGE  6	/* legend included */
#define VOLMAP_OV_MIN_SLOTS 5
#define VOLMAP_OV_MIN_INFO  5
#define VOLMAP_OV_MIN_TOTAL (VOLMAP_OV_MIN_SECT + VOLMAP_OV_MIN_PAGE \
			     + VOLMAP_OV_MIN_SLOTS + VOLMAP_OV_MIN_INFO)
/* Drill-down overlay geometry, produced by drawing.
   Drawing fills this struct and returns it; input handling only reads it.
   That keeps hit-testing coordinates and scroll state single-owner. */
typedef struct volmap_ov_geom VOLMAP_OV_GEOM;
struct volmap_ov_geom
{
  int x0;			/* overlay left column */
  int y0, y1;			/* full vertical range including borders */
  int b1y;			/* sector grid first row */
  int b2y, b2n;			/* page box first row and row count */
  int b3y;			/* slots grid first row */
  int slot_base;		/* first slot number in the window */
  int nsl;			/* slots last drawn by the slots box (cursor upper bound) */
};
static VOLMAP_OV_GEOM volmap_ov_geom;	/* geometry of the last overlay drawn */
/* file view box geometry, left by drawing so the mouse can pick list entries */
typedef struct volmap_fv_geom VOLMAP_FV_GEOM;
struct volmap_fv_geom
{
  int x0, w;			/* box left column and full width */
  int list_y0, rows;		/* list first row and visible rows */
  int top;			/* list scroll offset (index of the first visible entry) */
};
static VOLMAP_FV_GEOM volmap_fv_geom;

/* Page box scroll state: owned by input handling, read by drawing. */
/* Page grid state (256 cells within a 16KB page).  base/more are reached
   through the aliases below; width and rows come from the box size at draw
   time, so they are filled in at the clamp call. */
static VOLMAP_GRID volmap_ov_page_grid = { VOLMAP_OV_PAGE_W, 1, 0, 0 };

#define volmap_ov_page_base (volmap_ov_page_grid.base)	/* first cell index shown by the page box */
#define volmap_ov_page_more (volmap_ov_page_grid.more)	/* 1 = more below ('+' shown) */
#define VOLMAP_PAGE_CELL_BYTES 64	/* 4096 / 64 = exactly 64 cells per 4KB */
#define VOLMAP_PAGE_GRID_CELLS 256	/* 16344B (user area) / 64B = cells per page */

/* the only function that fills the struct above. box_w = box content width, avail_rows = rows available to the drill-down */
static void
volmap_ov_layout_calc (VOLMAP_OV_LAYOUT * lo, int box_w, int avail_rows, int cell_sects)
{
  int sr;

  lo->box_w = box_w;
  /* Sector rows: 4 where height allows.  For each candidate the total height of
     all four boxes is computed and the largest that still fits is taken;
     sr == 1 is the fallback and is always accepted. */
  for (sr = VOLMAP_OV_SECT_MAX_ROWS; sr >= VOLMAP_OV_SECT_MIN_ROWS; sr--)
    {
      int pr = (sr * 3 + 1) / 2;	/* ceil(sr * 1.5) */
      int pcap = (VOLMAP_PAGE_GRID_CELLS + volmap_ov_grid_w (box_w) - 1)
	/ volmap_ov_grid_w (box_w);	/* stop once all 256 cells are visible */
      int need;

      if (pr > pcap)
	{
	  pr = pcap;		/* Apply the same cap while evaluating candidates - without it a
				   candidate looks larger than it is and 2 rows lose to 1 */
	}
      /* Height used: sector = grid rows + 2 border, page = grid rows + 2 legend
	 + 2 border.  Slots and info take their own minimum.  If the sum exceeds
	 the rows available, fall back to a smaller candidate. */
      need = (sr + 2) + (pr + 2 + 2) + VOLMAP_OV_MIN_SLOTS + VOLMAP_OV_MIN_INFO;
      if (need > avail_rows && sr > 2)
	{
	  continue;		/* try the next, smaller candidate */
	}
      if (need > avail_rows && sr == VOLMAP_OV_SECT_MIN_ROWS)
	{
	  /* Hold 2 rows to the end and let the page grid give way down to 1.
	     If that is still not enough each box scrolls internally; none is
	     dropped. */
	  int slack = need - avail_rows;

	  pr = (pr - slack < 1) ? 1 : pr - slack;
	}

      lo->sect_rows = sr;
      lo->page_rows = pr + 2;	/* +2 for the two legend rows */
      break;
    }
  /* Sector grid.
       - Grid width is either 32 or 64, nothing else.
       - Width 64 puts two sectors on a row (one sector = 32 cells);
         width 32 spreads one sector over two rows.
       - Only the sectors covered by the selected map cell are drawn, so the row
         count follows the cell's sector count.  Neighbouring sectors are never
         pulled in to fill spare rows. */
  {
    int sects = (cell_sects > 0) ? cell_sects : 1;
    int w, rows;

    /* One grid cell is one page, so width 64 is exactly one sector (64 pages)
       per row and width 32 spreads a sector over two rows. */
    if (box_w >= VOLMAP_SECT_NPAGES)
      {
	w = VOLMAP_SECT_NPAGES;	/* 64: one row is one sector */
	rows = sects;
      }
    else
      {
	w = VOLMAP_OV_SECT_ROW_W;	/* 32: one sector spans two rows */
	rows = sects * 2;
      }
    if (rows > VOLMAP_OV_SECT_MAX_ROWS)
      {
	rows = VOLMAP_OV_SECT_MAX_ROWS;	/* overflow scrolls with '+' */
      }
    if (rows > lo->sect_rows)
      {
	rows = lo->sect_rows;	/* as far as the screen height allows */
      }
    if (rows < 1)
      {
	rows = 1;
      }
    lo->sect_w = w;
    lo->sect_rows = rows;
  }
  /* Page grid row count.
     - Upper bound: stop once all 256 cells are visible, so spare height does not
       produce an empty box.
     - Lower bound: the 1.5x rule means "grow the page grid as the sector grid
       grows".  If rows remain, keep growing until all 256 cells fit rather than
       showing '+' while space is still free. */
  {
    int gw = volmap_ov_grid_w (box_w);
    int full = (VOLMAP_PAGE_GRID_CELLS + gw - 1) / gw + 2;
    int used = (lo->sect_rows + 2) + (lo->page_rows + 2) + VOLMAP_OV_MIN_SLOTS + VOLMAP_OV_MIN_INFO;
    int spare = avail_rows - used;

    if (lo->page_rows > full)
      {
	lo->page_rows = full;
      }
    else if (lo->page_rows < full && spare > 0)
      {
	int grow = full - lo->page_rows;

	lo->page_rows += (grow < spare) ? grow : spare;
      }
    if (lo->page_rows < 3)
      {
	lo->page_rows = 3;	/* 1 grid row + 2 legend rows */
      }
  }
}

/* Sector grid scroll state, shared by drawing (volmap_ov_sect_draw) and input
   handling.  The window follows the map cell's range; input handling keeps the
   selection inside that cell. */
static VOLMAP_GRID volmap_ov_sect_grid = { VOLMAP_OV_PAGE_W, 2, 0, 0 };

#define volmap_ov_sect_base (volmap_ov_sect_grid.base)	/* first cell of the sector grid (= page number) */
#define volmap_ov_sect_more (volmap_ov_sect_grid.more)	/* 1 = more follows ('+' shown) */

/* Settle the page box scroll state just before drawing (the only writer apart
   from input handling).  vis = cells visible in the grid.  Pulls an out-of-range
   base back inside and sets 'more'.  The window size is known only at draw time,
   so it is stored on the grid here and passed to the shared clamp. */
static void
volmap_ov_page_scroll_clamp (int vis)
{
  volmap_ov_page_grid.w = (vis > 0) ? vis : 1;
  volmap_ov_page_grid.rows = 1;
  volmap_grid_clamp (&volmap_ov_page_grid, VOLMAP_PAGE_GRID_CELLS, -1);
}

static const char *
L (const char *en, const char *ko)
{
  return volmap_lang_ko ? ko : en;
}

/* kind index: 0=DATA 1=INDEX 2=CATALOG 3=SYSTEM 4=TEMP/ETC */
/* highest allocated page of a volume (from the page bitmaps).  Cheap enough
 * to recompute per frame: one pass from the tail, usually a handful of steps. */
static void
volmap_vol_hwm_update (VOLMAP_VOLUME * vol)
{
  DKNSECTS s2;

  vol->hwm_page = -1;
  for (s2 = vol->nsect_total - 1; s2 >= 0; s2--)
    {
      UINT64 bm = vol->pagebm[s2];

      if (bm != 0)
	{
	  vol->hwm_page = (PAGEID) ((long) s2 * VOLMAP_SECT_NPAGES + (63 - __builtin_clzll (bm)));
	  break;
	}
    }
}

/* role inside one OBJECT (class): heap file / index file / overflow appendage */
static int
volmap_obj_role (FILE_TYPE t)
{
  switch (t)
    {
    case FILE_HEAP:
    case FILE_HEAP_REUSE_SLOTS:
      return 0;
    case FILE_BTREE:
      return 1;
    case FILE_MULTIPAGE_OBJECT_HEAP:
    case FILE_BTREE_OVERFLOW_KEY:
      return 2;
    default:
      return -1;
    }
}

static int
volmap_kind_idx (FILE_TYPE t)
{
  switch (t)
    {
    case FILE_HEAP:
    case FILE_HEAP_REUSE_SLOTS:
    case FILE_MULTIPAGE_OBJECT_HEAP:
      return 0;
    case FILE_BTREE:
    case FILE_BTREE_OVERFLOW_KEY:
      return 1;
    case FILE_CATALOG:
    case FILE_EXTENDIBLE_HASH:
    case FILE_EXTENDIBLE_HASH_DIRECTORY:
      return 2;
    case FILE_TRACKER:
    case FILE_DROPPED_FILES:
    case FILE_VACUUM_DATA:
      return 3;
    default:
      return 4;
    }
}

static void
volmap_human (INT64 nbytes, char *buf, size_t size)
{
  const char *units[] = { "B", "KB", "MB", "GB", "TB" };
  double v = (double) nbytes;
  int u = 0;
  while (v >= 1024.0 && u < 4)
    {
      v /= 1024.0;
      u++;
    }
  if (v == (double) (long long) v)
    {
      /* exact multiples (sector-snapped cells, whole-MB files) need no ".0" */
      snprintf (buf, size, "%lld%s", (long long) v, units[u]);
    }
  else
    {
      snprintf (buf, size, "%.1f%s", v, units[u]);
    }
}


/* ── interactive mode (-i): one volume per screen, mouse hit-testing ────── */

static const char *
volmap_ftype_name (FILE_TYPE t)
{
  switch (t)
    {
    case FILE_TRACKER: return "TRACKER";
    case FILE_HEAP: return "HEAP";
    case FILE_HEAP_REUSE_SLOTS: return "HEAP_REUSE";
    case FILE_MULTIPAGE_OBJECT_HEAP: return "MULTIPAGE_OBJ";
    case FILE_BTREE: return "BTREE";
    case FILE_BTREE_OVERFLOW_KEY: return "BTREE_OVF_KEY";
    case FILE_EXTENDIBLE_HASH: return "EHASH";
    case FILE_EXTENDIBLE_HASH_DIRECTORY: return "EHASH_DIR";
    case FILE_CATALOG: return "CATALOG";
    case FILE_DROPPED_FILES: return "DROPPED_FILES";
    case FILE_VACUUM_DATA: return "VACUUM_DATA";
    case FILE_QUERY_AREA: return "QUERY_AREA";
    case FILE_TEMP: return "TEMP";
    default: return "UNKNOWN";
    }
}



/*
 * volmap_resolve_class_name () - read the class record straight from the volume file and pull the
 *                                class name out of it (same layout knowledge as or_class_name():
 *                                the first variable attribute of a class record is its name).
 */
static const char *
volmap_resolve_class_name (VOLMAP_CTX * ctx, VOLMAP_FILE * f)
{
  int prv = prv_user_offset ();
  VOLMAP_VOLUME *vol;
  char *iopage = NULL;
  const char *user;
  const SPAGE_SLOT *slot;
  const char *rec;
  OID oid;
  int hops;

  if (f->class_name[0] != '\0')
    {
      return f->class_name;
    }
  strcpy (f->class_name, "?");
  oid = f->class_oid;

  for (hops = 0; hops < 2; hops++)	/* REC_RELOCATION is followed once */
    {
      unsigned int repid_and_flags, mvcc_flags, offset_size_flag;
      int hdr_size, offset_size;
      long var0_off;
      const char *table, *name;
      int len;

      if (OID_ISNULL (&oid))
	{
	  break;
	}
      vol = volmap_find_vol (ctx, oid.volid);
      if (vol == NULL)
	{
	  break;
	}
      free (iopage);
      iopage = (char *) malloc (vol->iopagesize);
      if (iopage == NULL || !volmap_read_iopage (vol, oid.pageid, iopage))
	{
	  break;
	}
      user = iopage + prv;
      if (oid.slotid < 0 || oid.slotid >= ((const SPAGE_HEADER *) user)->num_slots)
	{
	  break;
	}
      slot = (const SPAGE_SLOT *) (user + vol->user_size - sizeof (SPAGE_SLOT)) - oid.slotid;
      if (slot->offset_to_record == 0 || slot->offset_to_record >= (unsigned) vol->user_size)
	{
	  break;
	}
      rec = user + slot->offset_to_record;
      if (slot->record_type == REC_RELOCATION)
	{
	  memcpy (&oid, rec, sizeof (OID));	/* forward oid is first in the record */
	  continue;
	}
      if (slot->record_type == REC_BIGONE)
	{
	  /* record lives in an overflow chain; its first page holds OVERFLOW_FIRST_PART
	   * and the class name sits near the start, so one page is enough */
	  OID ovf_oid;
	  memcpy (&ovf_oid, rec, sizeof (OID));
	  vol = volmap_find_vol (ctx, ovf_oid.volid);
	  if (vol == NULL || !volmap_read_iopage (vol, ovf_oid.pageid, iopage))
	    {
	      break;
	    }
	  /* OVERFLOW_FIRST_PART layout (overflow_file.h): VPID next_vpid; int length; char data[];
	   * kept local because overflow_file.h drags in server-only headers */
	  rec = iopage + prv + sizeof (VPID) + sizeof (int);
	}
      else if (slot->record_type != REC_HOME && slot->record_type != REC_NEWHOME)
	{
	  break;
	}

      /* OR record: repid_and_flags(4) [chn 4] [insid 8] [delid 8] [prev_lsa 8] | var table | data.
       * OR integers are stored in NETWORK byte order (or_put_int uses htonl). */
      memcpy (&repid_and_flags, rec, 4);
      repid_and_flags = __builtin_bswap32 (repid_and_flags);
      mvcc_flags = (repid_and_flags >> OR_MVCC_FLAG_SHIFT_BITS) & OR_MVCC_FLAG_MASK;
      hdr_size = 8;		/* repid+flags, chn */
      if (mvcc_flags & OR_MVCC_FLAG_VALID_INSID)
	{
	  hdr_size += 8;
	}
      if (mvcc_flags & OR_MVCC_FLAG_VALID_DELID)
	{
	  hdr_size += 8;
	}
      if (mvcc_flags & OR_MVCC_FLAG_VALID_PREV_VERSION)
	{
	  hdr_size += 8;
	}
      offset_size_flag = repid_and_flags & OR_OFFSET_SIZE_FLAG;
      offset_size = (offset_size_flag == OR_OFFSET_SIZE_1BYTE) ? 1
	: (offset_size_flag == OR_OFFSET_SIZE_2BYTE) ? 2 : 4;

      table = rec + hdr_size;
      if (offset_size == 1)
	{
	  var0_off = *(const unsigned char *) table;
	}
      else if (offset_size == 2)
	{
	  unsigned short v16;
	  memcpy (&v16, table, 2);
	  var0_off = (short) __builtin_bswap16 (v16);
	}
      else
	{
	  unsigned int v32;
	  memcpy (&v32, table, 4);
	  var0_off = (int) __builtin_bswap32 (v32);
	}

      if (var0_off < 0 || var0_off > vol->user_size)
	{
	  break;
	}
      /* first variable attribute of a class record = class name (packed varchar) */
      name = rec + hdr_size + var0_off;
      {
	const char *ubase = iopage + prv;
	long noff = name - ubase;

	if (noff < 0 || noff + 5 >= vol->user_size)
	  {
	    break;		/* corrupt offsets would walk past the page buffer */
	  }
	len = *(const unsigned char *) name;
	name += (len != 0xFF) ? 1 : 1 + 4;
	snprintf (f->class_name, sizeof (f->class_name), "%.*s",
		  (int) (vol->user_size - (name - ubase)), name);
      }
      break;
    }
  free (iopage);
  return f->class_name;
}


/* fetch the full class record (following RELOCATION/NEWHOME and assembling BIGONE overflow chains) */
static int
volmap_fetch_class_record (VOLMAP_CTX * ctx, OID oid, char *buf, int bufsz)
{
  int prv = prv_user_offset ();
  char *iopage = NULL;
  int total = -1;
  int hops;

  for (hops = 0; hops < 3 && total < 0; hops++)
    {
      VOLMAP_VOLUME *vol = volmap_find_vol (ctx, oid.volid);
      const char *user;
      const SPAGE_SLOT *slot;

      if (vol == NULL || OID_ISNULL (&oid))
	{
	  break;
	}
      if (iopage == NULL)
	{
	  iopage = (char *) malloc (volmap_max_iopagesize (ctx));	/* the record chain crosses volumes */
	}
      if (iopage == NULL || !volmap_read_iopage (vol, oid.pageid, iopage))
	{
	  break;
	}
      user = iopage + prv;
      if (oid.slotid < 0 || oid.slotid >= ((const SPAGE_HEADER *) user)->num_slots)
	{
	  break;
	}
      slot = (const SPAGE_SLOT *) (user + vol->user_size - sizeof (SPAGE_SLOT)) - oid.slotid;
      if (slot->offset_to_record == 0 || slot->offset_to_record >= (unsigned) vol->user_size)
	{
	  break;
	}
      if (slot->record_type == REC_RELOCATION)
	{
	  memcpy (&oid, user + slot->offset_to_record, sizeof (OID));
	  continue;
	}
      if (slot->record_type == REC_HOME || slot->record_type == REC_NEWHOME)
	{
	  total = (int) slot->record_length;
	  if (total > bufsz)
	    {
	      total = bufsz;
	    }
	  if (total > vol->user_size - (int) slot->offset_to_record)
	    {
	      total = vol->user_size - (int) slot->offset_to_record;	/* corrupt length must not read past the page */
	    }
	  memcpy (buf, user + slot->offset_to_record, total);
	  break;
	}
      if (slot->record_type == REC_BIGONE)
	{
	  /* overflow chain: first part = VPID next + int length + data; rest = VPID next + data */
	  OID ovf;
	  int copied = 0, length = -1;
	  VPID next;

	  memcpy (&ovf, user + slot->offset_to_record, sizeof (OID));
	  next.volid = ovf.volid;
	  next.pageid = ovf.pageid;
	  while (next.pageid > 0 && copied < bufsz)
	    {
	      VOLMAP_VOLUME *ov = volmap_find_vol (ctx, next.volid);
	      const char *ou;
	      int off, chunk;

	      if (ov == NULL || !volmap_read_iopage (ov, next.pageid, iopage))
		{
		  break;
		}
	      ou = iopage + prv;
	      memcpy (&next, ou, sizeof (VPID));
	      if (length < 0)
		{
		  memcpy (&length, ou + sizeof (VPID), sizeof (int));
		  off = (int) sizeof (VPID) + (int) sizeof (int);
		}
	      else
		{
		  off = (int) sizeof (VPID);
		}
	      chunk = ov->user_size - off;
	      if (copied + chunk > bufsz)
		{
		  chunk = bufsz - copied;
		}
	      memcpy (buf + copied, ou + off, chunk);
	      copied += chunk;
	      if (length >= 0 && copied >= length)
		{
		  copied = length;
		  break;
		}
	    }
	  total = copied;
	  break;
	}
      break;
    }
  free (iopage);
  return total;
}

/*
 * volmap_resolve_index_name () - the class record's property list stores each index as
 * { name-string, ..., btid-string "volid|fileid|rootpage" }. Locate this file's btid string
 * (root = sticky first page) and take the nearest preceding identifier string as the name.
 */
static const char *
volmap_resolve_index_name (VOLMAP_CTX * ctx, VOLMAP_FILE * f)
{
  char needle[48];
  char *rec;
  int reclen, nlen, i;
  long best = -1;

  if (f->index_name[0] != '\0')
    {
      return f->index_name;
    }
  strcpy (f->index_name, "?");

  rec = (char *) malloc (64 * 1024);
  if (rec == NULL)
    {
      return f->index_name;
    }
  reclen = volmap_fetch_class_record (ctx, f->class_oid, rec, 64 * 1024);
  nlen = snprintf (needle, sizeof (needle), "%d|%d|%d", (int) f->vfid.volid, (int) f->vfid.fileid,
		   (int) f->root_page);

  for (i = 0; reclen > 0 && i + nlen < reclen; i++)
    {
      long pos;

      if (memcmp (rec + i, needle, nlen) != 0 || rec[i + nlen] != '\0')
	{
	  continue;
	}
      if (i < 1 || (unsigned char) rec[i - 1] != (unsigned char) nlen)
	{
	  continue;		/* must be a length-prefixed packed string */
	}
      /* scan backward for the nearest identifier-looking packed string = index name */
      for (pos = i - 2; pos >= 1 && pos > i - 512; pos--)
	{
	  int len = (unsigned char) rec[pos];
	  if (len >= 1 && len < 64 && pos + 1 + len < reclen && rec[pos + 1 + len] == '\0')
	    {
	      int k, ok = 1;
	      for (k = 0; k < len; k++)
		{
		  char c = rec[pos + 1 + k];
		  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
			|| c == '_' || c == '.' || c == '#' || c == '$'))
		    {
		      ok = 0;
		      break;
		    }
		}
	      if (ok)
		{
		  memcpy (f->index_name, rec + pos + 1, len);
		  f->index_name[len] = '\0';
		  best = pos;
		  break;
		}
	    }
	}
      if (best >= 0)
	{
	  break;
	}
    }
  free (rec);
  return f->index_name;
}

/* one-page peek: slot summary + FTAB detection for the breadcrumb (single pread) */
static void
volmap_peek_page (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long pageid, char *out, size_t outsz, bool * is_ftab)
{
  int prv = prv_user_offset ();
  char *iopage = (char *) malloc (vol->iopagesize);
  const FILEIO_PAGE_RESERVED *pr;
  const SPAGE_HEADER *sp;

  (void) ctx;
  out[0] = '\0';
  *is_ftab = false;
  if (iopage == NULL || !volmap_read_iopage (vol, (PAGEID) pageid, iopage))
    {
      free (iopage);
      return;
    }
  pr = (const FILEIO_PAGE_RESERVED *) iopage;
  if (pr->pageid != (PAGEID) pageid || pr->volid != vol->volid)
    {
      free (iopage);
      return;
    }
  if (pr->ptype == PAGE_FTAB)
    {
      *is_ftab = true;
    }
  else if (pr->ptype == PAGE_HEAP || pr->ptype == PAGE_BTREE || pr->ptype == PAGE_OVERFLOW)
    {
      sp = (const SPAGE_HEADER *) (iopage + prv);
      if (sp->num_slots >= 0 && sp->num_slots <= vol->user_size / (int) sizeof (SPAGE_SLOT))
	{
	  snprintf (out, outsz, " \xe2\x80\xba slots %d recs %d free %dB", sp->num_slots, sp->num_records,
		    sp->total_free);
	}
    }
  free (iopage);
}

static int volmap_ovf_owner_file (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, int ovf_idx);
static const char *volmap_json_escape (const char *in, char *out, int outsz);
static void volmap_describe_cell_base (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long per_pages, const char *cell_state,
					const int *cell_owner, const long *cell_p0, const long *cell_alloc_arr, int cell,
					char *out, size_t outsz);

/* describe + the --bufmap facts for the same page span (how many of these pages sit
 * in the buffer pool, how many are dirty) */
static void
volmap_describe_cell (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long per_pages, const char *cell_state,
		      const int *cell_owner, const long *cell_p0, const long *cell_alloc_arr, int cell,
		      char *out, size_t outsz)
{
  volmap_describe_cell_base (ctx, vol, per_pages, cell_state, cell_owner, cell_p0, cell_alloc_arr, cell, out, outsz);
  if (ctx->bufmap && ctx->bufmap_loaded && vol->buf_prefix != NULL)
    {
      long p0 = cell_p0[cell], npages = (long) vol->nsect_total * VOLMAP_SECT_NPAGES;
      long pe = p0 + per_pages;

      if (p0 >= 0 && p0 < npages)
	{
	  long nb, nd;
	  char tok[64];
	  char *ins = NULL;
	  int nsep = 0;

	  if (pe > npages)
	    {
	      pe = npages;
	    }
	  nb = (long) vol->buf_prefix[pe] - (long) vol->buf_prefix[p0];
	  nd = (long) vol->dirty_prefix[pe] - (long) vol->dirty_prefix[p0];
	  if (nd > 0)
	    {
	      snprintf (tok, sizeof (tok), "[buf %ld/%ld UNFLUSHED %ld] ", nb, pe - p0, nd);
	    }
	  else
	    {
	      snprintf (tok, sizeof (tok), "[buf %ld/%ld] ", nb, pe - p0);
	    }
	  /* the bottom bar is clipped at the terminal width: put the token right after the
	   * breadcrumb (3rd separator) so it is never the part that gets cut off */
	  for (ins = out; (ins = strstr (ins, " \xe2\x80\xba ")) != NULL; ins += 5)
	    {
	      if (++nsep == 3)
		{
		  ins += 5;	/* after "sep + space" */
		  break;
		}
	    }
	  if (ins != NULL)
	    {
	      size_t tl = strlen (tok), rest = strlen (ins);

	      if (strlen (out) + tl + 1 < outsz)
		{
		  memmove (ins + tl, ins, rest + 1);
		  memcpy (ins, tok, tl);
		}
	    }
	  else
	    {
	      size_t ln = strlen (out);

	      snprintf (out + ln, (outsz > ln) ? outsz - ln : 0, " %s", tok);
	    }
	}
    }
}

static void
volmap_describe_cell_base (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long per_pages, const char *cell_state,
			   const int *cell_owner, const long *cell_p0, const long *cell_alloc_arr, int cell,
			   char *out, size_t outsz)
{
  long p0 = cell_p0[cell];
  char crumb[96];

  snprintf (crumb, sizeof (crumb), "vol %d \xe2\x80\xba sect %ld..%ld \xe2\x80\xba page %ld..%ld",
	    (int) vol->volid, p0 / VOLMAP_SECT_NPAGES, (p0 + per_pages - 1) / VOLMAP_SECT_NPAGES,
	    p0, p0 + per_pages - 1);

  if (cell_state[cell] == 'F' && cell_owner[cell] >= 0)
    {
      VOLMAP_FILE *f = &ctx->files[cell_owner[cell]];
      int kind = volmap_kind_idx (f->ftype);
      char label[160];		/* tag + class name (64) + index name (64) + separators */

      if (f->ftype == FILE_TEMP || f->ftype == FILE_QUERY_AREA)
	{
	  /* temp/query files belong to a query, not a class — show creation time instead */
	  time_t t = (time_t) f->time_creation;
	  struct tm tmv;
	  char when[32] = "?";
	  if (t > 0 && localtime_r (&t, &tmv) != NULL)
	    {
	      strftime (when, sizeof (when), "%Y-%m-%d %H:%M:%S", &tmv);
	    }
	  snprintf (label, sizeof (label), "%s workspace created %s", volmap_ftype_name (f->ftype), when);
	}
      else if (OID_ISNULL (&f->class_oid))
	{
	  /* tracker/catalog/ehash/vacuum/dropped-files: internal files with no owning class */
	  /* An overflow file has no class_oid and would otherwise read as
	     'system file'.  Find its owning heap (reverse lookup of the heap
	     header's ovf_vfid) to say whose overflow it is. */
	  if (f->ftype == FILE_MULTIPAGE_OBJECT_HEAP)
	    {
	      int ow = volmap_ovf_owner_file (ctx, vol, cell_owner[cell]);

	      if (ow >= 0)
		{
		  const char *on = volmap_resolve_class_name (ctx, &ctx->files[ow]);

		  snprintf (label, sizeof (label), "%s ovf of %s%s",
			    volmap_ftype_name (f->ftype), VOLMAP_TAG_H, on);
		}
	      else
		{
		  snprintf (label, sizeof (label), "%s (owner unknown)", volmap_ftype_name (f->ftype));
		}
	    }
	  else
	  snprintf (label, sizeof (label), "%s (system file)", volmap_ftype_name (f->ftype));
	}
      else
	{
	  const char *name = volmap_resolve_class_name (ctx, f);
	  const char *tag = (kind == 0) ? VOLMAP_TAG_H : (kind == 1) ? VOLMAP_TAG_I : "";
	  if (name[0] == '?')
	    {
	      snprintf (label, sizeof (label), "%s%s class(%d|%d|%d)", tag, volmap_ftype_name (f->ftype),
			(int) f->class_oid.volid, (int) f->class_oid.pageid, (int) f->class_oid.slotid);
	    }
	  else if (kind == 1)
	    {
	      snprintf (label, sizeof (label), "%s%s %s", tag, name, volmap_resolve_index_name (ctx, f));
	    }
	  else
	    {
	      snprintf (label, sizeof (label), "%s%s", tag, name);
	    }
	}
      {
	char peek[64];
	bool is_ftab = false;
	volmap_peek_page (ctx, vol, p0, peek, sizeof (peek), &is_ftab);
	char cellinfo[64] = "";	/* " | cell alloc %ld/%ld" - two counts, never truncated */

	if (cell_alloc_arr[cell] < per_pages)
	  {
	    /* only when partially allocated: a full cell would print the constant
	     * "P/P" (the cell size already sits in the header line) */
	    snprintf (cellinfo, sizeof (cellinfo), " | cell alloc %ld/%ld", cell_alloc_arr[cell], per_pages);
	  }
	/* \x01 marks a line-break hint at a semantic boundary.  Row 1 is
	   "where / what" (location + object name), row 2 the detail (file id,
	   size, cell allocation).  Breaking purely on width would split tokens
	   mid-word, so the boundaries are given in the source. */
	snprintf (out, outsz, "%s \xe2\x80\xba %s\x01%s%sVFID %d|%d%s | file: %d pages (%d user, %d free)%s",
		  crumb, label, is_ftab ? "#file-table page " : "", "", (int) f->vfid.volid, (int) f->vfid.fileid,
		  peek, f->n_page_total, f->n_page_user, f->n_page_free, cellinfo);
      }
    }
  else if (cell_state[cell] == '?')
    {
      snprintf (out, outsz,
		"%s \xe2\x80\xba %s", crumb,
		L ("unknown owner - usually a transient snapshot skew on a LIVE db: press r, investigate only if it persists",
		   "\xec\x86\x8c\xec\x9c\xa0\xec\x9e\x90 \xeb\xaf\xb8\xec\x83\x81 \xe2\x80\x94 \xeb\x8c\x80\xea\xb0\x9c \xeb\x9d\xbc\xec\x9d\xb4\xeb\xb8\x8c DB\xec\x9d\x98 \xec\x9d\xbc\xec\x8b\x9c\xec\xa0\x81 \xec\x8a\xa4\xeb\x83\x85\xec\x83\xb7 \xec\x8b\x9c\xec\xb0\xa8: r \xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8, \xea\xb7\xb8 \xed\x9b\x84\xec\x97\x90\xeb\x8f\x84 \xeb\x82\xa8\xec\x9c\xbc\xeb\xa9\xb4 \xec\xa0\x90\xea\xb2\x80"));
    }
  else if (cell_state[cell] == '~')
    {
      snprintf (out, outsz, "%s \xe2\x80\xba %s (%d%%)", crumb,
		L ("not scanned yet (initial scan)", "\xec\x95\x84\xec\xa7\x81 \xec\x8a\xa4\xec\xba\x94 \xec\xa0\x84 (\xec\xb4\x88\xea\xb8\xb0 \xec\x8a\xa4\xec\xba\x94)"),
		(int) (ctx->scan_total > 0 ? 100 * ctx->scan_probed / ctx->scan_total : 0));
    }
  else if (cell_state[cell] == '#')
    {
      snprintf (out, outsz, "%s \xe2\x80\xba %s", crumb,
		L ("#volume metadata (header + sector allocation table)",
		   "#\xeb\xb3\xbc\xeb\xa5\xa8 \xeb\xa9\x94\xed\x83\x80\xeb\x8d\xb0\xec\x9d\xb4\xed\x84\xb0 (\xed\x97\xa4\xeb\x8d\x94 + \xec\x84\xb9\xed\x84\xb0 \xec\x98\x88\xec\x95\xbd \xec\x9e\xa5\xeb\xb6\x80)"));
    }
  else if (cell_state[cell] == '_')
    {
      snprintf (out, outsz, "%s \xe2\x80\xba %s", crumb,
		L ("reserved but empty (no pages allocated here)",
		   "\xec\x98\x88\xec\x95\xbd\xeb\xa7\x8c \xeb\x90\x98\xea\xb3\xa0 \xeb\xb9\x88 \xea\xb3\xb5\xea\xb0\x84 (\xed\x95\xa0\xeb\x8b\xb9\xeb\x90\x9c \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xec\x97\x86\xec\x9d\x8c)"));
    }
  else if (cell_state[cell] == '.')
    {
      snprintf (out, outsz, "%s \xe2\x80\xba %s", crumb, L ("unreserved space", "\xeb\xaf\xb8\xec\x98\x88\xec\x95\xbd \xea\xb3\xb5\xea\xb0\x84"));
    }
  else
    {
      snprintf (out, outsz, "%s \xe2\x80\xba %s", crumb,
		L ("unknown owner (press r; investigate only if it persists)",
		   "\xec\x86\x8c\xec\x9c\xa0\xec\x9e\x90 \xeb\xaf\xb8\xec\x83\x81 (r \xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8, \xea\xb7\xb8 \xed\x9b\x84\xec\x97\x90\xeb\x8f\x84 \xeb\x82\xa8\xec\x9c\xbc\xeb\xa9\xb4 \xec\xa0\x90\xea\xb2\x80)"));
    }
}

/*
 * volmap_refresh () - incremental live refresh, engineered to NEVER burden a production server:
 *   1. re-read the sector tables (a few KB per volume)
 *   2. re-walk the file tables of files we already know (their header/extdata pages only)
 *   3. probe ONLY reserved sectors that still have no owner (newly grown/created files)
 * No full-volume sweep is ever repeated; typical cost is well under a megabyte of reads.
 */
static int
volmap_refresh (VOLMAP_CTX * ctx)
{
  int vi, fi;
  char *iopage = NULL;
  int prv = prv_user_offset ();

  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];

      (void) volmap_read_stab (vol);
      memset (vol->w_owner, -1, vol->nsect_total * sizeof (int));
      memset (vol->w_alloc, 0, vol->nsect_total * sizeof (int));
      memset (vol->w_pagebm, 0, vol->nsect_total * sizeof (UINT64));
      /* tde_pages and tde_bm are deliberately NOT cleared here.  This refresh is a
         delta probe - it only visits sectors nobody owns - so zeroing the tally
         would throw away every page the initial full scan found and never look at
         it again.  The bitmap keeps the count correct instead: a page already
         counted is not counted twice, whichever probe reaches it. */
    }

  /* re-walk known files (counts + ownership); vanished files simply stop matching.
     First only the 32-byte page prefix of the header is read: an LSA that has not moved
     since the last walk means the sector tables have not either (every allocation,
     deallocation and sector change writes the header counters), so the remembered
     list is re-applied without reading the page. */
  ctx->rf_cached = 0;
  ctx->rf_walked = 0;
  for (fi = 0; fi < ctx->nfiles; fi++)
    {
      VOLMAP_FILE *f = &ctx->files[fi];
      VOLMAP_VOLUME *vol = volmap_find_vol (ctx, f->vfid.volid);
      const FILE_HEADER *fhead;
      FILEIO_PAGE_RESERVED pr;
      UINT64 lsa_now;

      if (vol == NULL)
	{
	  continue;
	}
      if (pread (volmap_vol_fd (vol), &pr, sizeof (pr), (off_t) f->vfid.fileid * vol->iopagesize) == (ssize_t) sizeof (pr)
	  && pr.pageid == f->vfid.fileid && pr.volid == f->vfid.volid && f->sects != NULL && f->hdr_lsa != 0)
	{
	  memcpy (&lsa_now, &pr.lsa, sizeof (lsa_now));
	  if (lsa_now == f->hdr_lsa)
	    {
	      int k;

	      f->alloc_pages = 0;
	      f->sectors_seen = 0;
	      for (k = 0; k < f->nsects; k++)
		{
		  VOLMAP_VOLUME *sv = volmap_find_vol (ctx, f->sects[k].volid);
		  int np = __builtin_popcountll (f->sects[k].bitmap);

		  if (sv == NULL || f->sects[k].sectid < 0 || f->sects[k].sectid >= sv->nsect_total)
		    {
		      continue;
		    }
		  sv->w_owner[f->sects[k].sectid] = fi;
		  sv->w_alloc[f->sects[k].sectid] += np;
		  sv->w_pagebm[f->sects[k].sectid] |= f->sects[k].bitmap;
		  f->sectors_seen++;
		  f->alloc_pages += np;
		}
	      ctx->rf_cached++;
	      continue;
	    }
	}
      if (iopage == NULL)
	{
	  iopage = (char *) malloc (64 * 1024);
	}
      if (iopage == NULL || !volmap_read_iopage (vol, f->vfid.fileid, iopage))
	{
	  continue;
	}
      fhead = (const FILE_HEADER *) (iopage + prv);
      if (fhead->self.fileid != f->vfid.fileid || fhead->self.volid != f->vfid.volid)
	{
	  f->sectors_seen = 0;	/* file was destroyed; its sectors go unowned */
	  f->nsects = 0;
	  f->hdr_lsa = 0;
	  continue;
	}
      if (fhead->type != f->ftype)
	{
	  /* the same VFID holds another file now: the entry is re-made in place (the hash
	     maps the VFID here, so a new entry could never be found) */
	  if (fhead->type < FILE_TRACKER || fhead->type >= FILE_LAST || fhead->n_sector_total <= 0 || fhead->n_page_total < 0)
	    {
	      f->sectors_seen = 0;
	      f->nsects = 0;
	      f->hdr_lsa = 0;
	      continue;
	    }
	  volmap_file_fill (f, fhead);
	  f->tracked = 0;
	  f->trk_seen = 0;
	}
      f->n_page_total = fhead->n_page_total;
      f->n_page_user = fhead->n_page_user;
      f->n_page_ftab = fhead->n_page_ftab;
      f->n_page_free = fhead->n_page_free;
      f->n_sector_total = fhead->n_sector_total;
      f->alloc_pages = 0;
      f->sectors_seen = 0;
      f->nsects = 0;
      f->hdr_lsa = volmap_hdr_lsa_for_cache (iopage);
      if (fhead->offset_to_partial_ftab >= 0)
	{
	  volmap_walk_extdata (ctx, vol, iopage, fhead->offset_to_partial_ftab, true, fi);
	}
      if (fhead->offset_to_full_ftab >= 0)
	{
	  volmap_walk_extdata (ctx, vol, iopage, fhead->offset_to_full_ftab, false, fi);
	}
      ctx->rf_walked++;
    }

  /* files created since: the tracker lists them, for the cost of its few pages */
  if (ctx->tracker_state == 1)
    {
      char why[120];
      int k;

      (void) volmap_tracker_walk (ctx, &ctx->tracker_first, why, sizeof (why));
      ctx->files_tracked = 0;
      for (k = 0; k < ctx->nfiles; k++)
	{
	  ctx->files_tracked += (ctx->files[k].tracked != 0);
	}
    }
  /* delta probe: only reserved sectors that nobody claimed (new files / new extents) */
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      DKNSECTS sct;

      for (sct = 0; sct < vol->nsect_total; sct++)
	{
	  if (!vol->w_stab[sct] || vol->w_owner[sct] >= 0 || iopage == NULL)
	    {
	      continue;
	    }
	  volmap_probe_sector (ctx, vol, sct, iopage, 1);
	}
    }
  free (iopage);
  return NO_ERROR;
}


/* grow-only shared scratch (single-threaded; replaces three static-local sets) */
static char *
volmap_scratch (VOLMAP_CTX * ctx, int need)
{
  if (ctx->scratch == NULL || ctx->scratch_size < need)
    {
      free (ctx->scratch);
      ctx->scratch_size = need;
      ctx->scratch = (char *) malloc (need);
    }
  return ctx->scratch;
}

static unsigned char *
volmap_scratch_bmap (VOLMAP_CTX * ctx, int need)
{
  if (ctx->scratch_bmap == NULL || ctx->scratch_bmap_size < need)
    {
      free (ctx->scratch_bmap);
      ctx->scratch_bmap_size = need;
      ctx->scratch_bmap = (unsigned char *) malloc (need);
    }
  return ctx->scratch_bmap;
}

/* ── right-hand drill-down preview panel (11 cols: " | " + 8 content cells) ────
 * Mirrors the web-style hierarchy preview: at the volume level it shows the
 * cursor cell's SECTOR as an 8x8 page grid; at the page/slot levels it shows
 * the page's full BYTE DISTRIBUTION (header / records / fragmented free /
 * contiguous free / slot directory), with the selected slot highlighted. */

#define VOLMAP_PANEL_W 8

typedef struct volmap_panel VOLMAP_PANEL;
struct volmap_panel
{
  int rows;			/* = maph */
  int w;			/* cells per row (8 = classic side panel, 32 = overlay) */
  char *color;			/* per cell: SGR color code index (see volmap_panel_put) */
  char *glyph;			/* per cell: 'b'=braille ramp lvl in aux, '#','_','.',' ','X' */
  char *aux;			/* ramp level / highlight flag */
  char *rbg;			/* -m residency shade per cell: 0 none, 1 dim(>=30%), 2 bright(>=80%) */
  long cached_key;		/* sector or page this panel was built for */
  int cached_hl;		/* highlighted slot / selected page the panel was built with */
  int cached_level;
  char mid[20];			/* label row inside the grid (e.g. selected page number) */
  int mid_row;			/* row the label occupies, -1 = none */
  int mid_style;		/* 0 = dim context label, 1 = selection label (blue bg) */
  long foot_val[2];		/* numeric footer values: nonzero draws an alert background */
  int hl_cell;			/* grid cell drawn with the selection background, -1 = none */
  int bytes_per_cell;		/* byte-distribution box: bytes one cell covers (for inspection) */
  char foot[2][24];		/* footer lines: deleted slots / unvacuumed dead versions */
  int nfoot;
  long sel_lo, sel_hi;		/* sector grid: page range the map cell covers (outside is grey and unselectable) */
  long base_pg;			/* absolute page number of the sector grid's first cell */
#define VOLMAP_PCOL_DIM 40	/* outside the cell range: grey, not selectable */
  int rec_col;			/* byte view: ANSI fg for record cells = the owning file's KIND color
				 * (map contract: data=34 blue, index=32 green) - 0 = unknown (37) */
};

/* Release a panel's cell buffers.  Paired with volmap_panel_ensure - every panel
   that gets one needs one of these before it goes out of scope. */
static void
volmap_panel_free (VOLMAP_PANEL * p)
{
  free (p->color);
  free (p->glyph);
  free (p->aux);
  free (p->rbg);
  p->color = p->glyph = p->aux = p->rbg = NULL;
  p->rows = p->w = 0;
}

static void
volmap_panel_ensure (VOLMAP_PANEL * p, int rows, int w)
{
  int n = w * rows;

  if (p->rows == rows && p->w == w)
    {
      return;
    }
  p->w = w;
  free (p->color);
  free (p->glyph);
  free (p->aux);
  free (p->rbg);
  p->color = (char *) malloc (n);
  p->glyph = (char *) malloc (n);
  p->aux = (char *) malloc (n);
  p->rbg = (char *) malloc (n);
  p->rows = rows;
  if (p->color != NULL)
    {
      memset (p->color, 0, n);
      memset (p->glyph, ' ', n);
      memset (p->aux, 0, n);
    }
  if (p->rbg != NULL)
    {
      memset (p->rbg, 0, n);
    }
  p->cached_key = -1;
  p->cached_hl = -1;
  p->cached_level = -1;
  p->mid_row = -1;
  p->hl_cell = -1;
  p->nfoot = 0;
}

/* blank the panel and drop its cache (target left the volume, or a refresh
 * invalidated the underlying pages) */
static void
volmap_panel_clear (VOLMAP_PANEL * p)
{
  if (p->color != NULL)
    {
      /* color and aux take part in the row-identity compare of the fold logic:
       * stale residue from a previous panel must not leak into it */
      memset (p->glyph, ' ', p->w * p->rows);
      memset (p->color, 0, p->w * p->rows);
      memset (p->aux, 0, p->w * p->rows);
      if (p->rbg != NULL)
	{
	  memset (p->rbg, 0, p->w * p->rows);
	}
    }
  p->cached_key = -1;
  p->cached_hl = -1;
  p->cached_level = -1;
  p->mid_row = -1;
  p->hl_cell = -1;
  p->nfoot = 0;
}

/* count deleted slots and unvacuumed dead versions in one slotted page.
 * dead version = REC_HOME/REC_NEWHOME record whose MVCC header carries a valid
 * delete MVCCID (OR_MVCC_FLAG_VALID_DELID, bit 0x02 of the 5-bit flag field at
 * bits 24..28 of the big-endian repid_and_flags word) and has not been vacuumed
 * yet.  Only PAGE_HEAP records carry that header. */
static void
volmap_page_deadstats (VOLMAP_VOLUME * vol, const char *iopage, long *del, long *dead)
{
  int prv = prv_user_offset ();
  const FILEIO_PAGE_RESERVED *pr = (const FILEIO_PAGE_RESERVED *) iopage;
  const SPAGE_HEADER *sp = (const SPAGE_HEADER *) (iopage + prv);
  int i, nslots;

  if (sp->num_slots < 0 || sp->num_slots > vol->user_size / (int) sizeof (SPAGE_SLOT))
    {
      return;
    }
  nslots = sp->num_slots;
  for (i = 0; i < nslots; i++)
    {
      const SPAGE_SLOT *sl = (const SPAGE_SLOT *) (iopage + prv + vol->user_size - sizeof (SPAGE_SLOT)) - i;

      if (sl->record_type == REC_MARKDELETED || sl->record_type == REC_DELETED_WILL_REUSE
	  || sl->offset_to_record == 0)
	{
	  (*del)++;
	}
      else if (pr->ptype == PAGE_HEAP && (sl->record_type == REC_HOME || sl->record_type == REC_NEWHOME)
	       && sl->record_length >= 8 && (int) sl->offset_to_record + 4 <= vol->user_size)
	{
	  unsigned int w;

	  memcpy (&w, iopage + prv + sl->offset_to_record, sizeof (w));
	  if ((__builtin_bswap32 (w) >> 24) & 0x02)
	    {
	      (*dead)++;
	    }
	}
    }
}

/* fill rows 0..7 with the 8x8 page grid of one sector; the whole sector is read
 * with a single 1MB pread into the shared scratch (returned for reuse).
 * sect_del/sect_dead accumulate --deep aggregates over the sector's heap pages. */
/* Fill one grid cell (= one page).  Extracted so single-sector and
   multi-sector fills share the same code and cannot drift apart.
   page = the page's raw image (NULL if unread). */
static void
volmap_panel_fill_one_page (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_PANEL * p,
			    long sect, long pg, int cell, const char *page, bool readable,
			    int tone, long *sect_del, long *sect_dead)
{
  int prv = prv_user_offset ();
  const FILEIO_PAGE_RESERVED *pr = (const FILEIO_PAGE_RESERVED *) page;
  const SPAGE_HEADER *sp = (const SPAGE_HEADER *) (page + prv);
  int i = (int) (pg % VOLMAP_SECT_NPAGES);

  if (ctx->residency && vol->respg != NULL && p->rbg != NULL)
    {
      int sub = vol->iopagesize / 4096;
      double rr = (sub > 0) ? (double) vol->respg[pg] / sub : 0.0;

      p->rbg[cell] = (char) ((rr >= 0.8) ? 2 : (rr >= 0.3) ? 1 : 0);
    }
  if (sect >= vol->nsect_total || !vol->stab[sect])
    {
      p->glyph[cell] = '.';
      return;
    }
  if (!((vol->pagebm[sect] >> i) & 1))
    {
      /* no owner: whether the page is allocated is unknown (that comes from the
         owner's file table), so it is '?' rather than passed off as empty */
      p->glyph[cell] = (vol->owner[sect] < 0 && !ctx->scan_active) ? '?' : '_';
      return;
    }
  if (!readable || page == NULL || pr->pageid != (PAGEID) pg || pr->volid != vol->volid)
    {
      p->glyph[cell] = '?';
      return;
    }
  if (pr->pflag & 0x3)
    {
      p->glyph[cell] = 'E';	/* TDE-encrypted page */
      return;
    }
  if (ctx->deep && pr->ptype == PAGE_HEAP && sect_del != NULL)
    {
      volmap_page_deadstats (vol, page, sect_del, sect_dead);
    }
  if (pr->ptype == PAGE_FTAB || pg <= (long) vol->sys_lastpage)
    {
      p->glyph[cell] = '#';
      return;
    }
  p->glyph[cell] = 'b';
  if (pr->ptype == PAGE_HEAP || pr->ptype == PAGE_BTREE || pr->ptype == PAGE_OVERFLOW)
    {
      double r = (sp->total_free >= 0 && sp->total_free <= vol->user_size)
	? 1.0 - (double) sp->total_free / vol->user_size : 1.0;
      p->aux[cell] = (char) ((r >= 0.9) ? 0 : (r >= 0.7) ? 1 : (r >= 0.5) ? 2 : (r >= 0.3) ? 3 : 4);
    }
  else
    {
      p->aux[cell] = 0;
    }
  /* same colours as the map: kind colour + tone (alternate tone is +30); temp uses a per-file palette (20..25) */
  if (vol->owner[sect] < 0)
    {
      p->color[cell] = 5;
    }
  else if (volmap_kind_idx (ctx->files[vol->owner[sect]].ftype) == 4)
    {
      p->color[cell] = (char) (20 + vol->owner[sect] % 6);
    }
  else
    {
      p->color[cell] = (char) (volmap_kind_idx (ctx->files[vol->owner[sect]].ftype) + (tone ? 30 : 0));
    }
}

/* fill grows panel rows starting at row0 with the byte distribution of one
 * slotted page (already read into iopage).  Returns false when the page does
 * not self-validate; del/dead receive the page's deleted-slot / dead-version
 * counts.  colors: 5=header 0=record 3=frag-free 1=contig-free 2=directory,
 * 4 = hl_slot's record extent and directory entry (selection background). */
static bool
volmap_panel_fill_pagebytes (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_PANEL * p, const char *iopage,
			     long pg, int hl_slot, int row0, int grows, long *del, long *dead, int *per_out)
{
  int prv = prv_user_offset ();
  unsigned char *bmap;
  const SPAGE_HEADER *sp = (const SPAGE_HEADER *) (iopage + prv);
  const FILEIO_PAGE_RESERVED *pr = (const FILEIO_PAGE_RESERVED *) iopage;
  int ncells = p->w * grows;
  int per;
  int i, nslots;

  if (grows <= 0)
    {
      return false;
    }
  /* -m residency shading for the page box.  A cell here is N bytes, so there is
     no per-cell residency; the whole row is shaded with this one page's rate so
     the reading stays available while drilled in. */
  /* -m residency at the fixed 64B cell size (4KB = 64 cells).
     respg[pg] gives only how many of the page's 4KB chunks are resident (0..4),
     not which ones.  The whole page is therefore painted in one tone rather than
     approximating per-chunk residency, which would be false detail. */
  /* Clear rbg when residency is off, otherwise the previous shading stays on the
     page box after toggling with m.  (The sector box refills from scratch each
     frame and is unaffected.) */
  if (p->rbg != NULL && (!ctx->residency || vol->respg == NULL || pg < 0))
    {
      memset (p->rbg + row0 * p->w, 0, (size_t) grows * p->w);
    }
  if (ctx->residency && vol->respg != NULL && p->rbg != NULL && pg >= 0)
    {
      int sub = vol->iopagesize / 4096;
      double rr = (sub > 0) ? (double) vol->respg[pg] / sub : 0.0;
      char shade = (char) ((rr >= 0.8) ? 2 : (rr >= 0.3) ? 1 : 0);
      int c;

      /* Shade only cells that hold content.  On a wide box the 256 cells end after a
	 few rows and the rest is margin; shading it too would read as "the empty
	 area is cached as well". */
      int ncell_real = (vol->user_size + VOLMAP_PAGE_CELL_BYTES - 1) / VOLMAP_PAGE_CELL_BYTES;
      int lim = row0 * p->w + ncell_real;

      if (lim > (row0 + grows) * p->w)
	{
	  lim = (row0 + grows) * p->w;
	}
      if (lim > p->w * p->rows)
	{
	  lim = p->w * p->rows;
	}
      for (c = row0 * p->w; c < lim; c++)
	{
	  p->rbg[c] = shade;
	}
    }
  {
    /* record color = the owning file's kind color (map/sector-grid contract) */
    long psec = pg / VOLMAP_SECT_NPAGES;
    int ow = (pg >= 0 && psec < vol->nsect_total) ? vol->owner[psec] : -1;
    static const int reccol[6] = { 34, 32, 31, 31, 37, 37 };

    p->rec_col = (ow >= 0) ? reccol[volmap_kind_idx (ctx->files[ow].ftype)] : 37;
  }
  /* Byte cell size is fixed at 64B to align with 4KB.
     The OS page cache unit is 4KB and respg[] is a 0..4 count per 4KB, so cells
     must sit on 4KB boundaries for per-chunk residency to be paintable.
     A multiple of 16 is not used: user_size (16344B) is not a multiple of 16, so
     rounding up would make the last cells point past the end of the page. */
  per = VOLMAP_PAGE_CELL_BYTES;
  bmap = volmap_scratch_bmap (ctx, vol->iopagesize);
  if (bmap == NULL || pr->pageid != (PAGEID) pg || (pr->pflag & 0x3) != 0
      || sp->num_slots < 0 || sp->num_slots > vol->user_size / (int) sizeof (SPAGE_SLOT))
    {
      return false;
    }
  nslots = sp->num_slots;
  memset (bmap, 3, vol->user_size);	/* default: fragmented free */
  memset (bmap, 5, sizeof (SPAGE_HEADER));	/* header */
  if (sp->offset_to_free_area >= 0 && sp->offset_to_free_area <= vol->user_size)
    {
      int dir_start = vol->user_size - nslots * (int) sizeof (SPAGE_SLOT);
      int end = (dir_start > sp->offset_to_free_area) ? dir_start : sp->offset_to_free_area;
      memset (bmap + sp->offset_to_free_area, 1, end - sp->offset_to_free_area);	/* contiguous free */
      if (dir_start >= 0 && dir_start <= vol->user_size)
	{
	  memset (bmap + dir_start, 2, vol->user_size - dir_start);	/* slot directory */
	}
    }
  for (i = 0; i < nslots; i++)
    {
      const SPAGE_SLOT *sl = (const SPAGE_SLOT *) (iopage + prv + vol->user_size - sizeof (SPAGE_SLOT)) - i;

      if (sl->offset_to_record > 0 && sl->offset_to_record < (unsigned) vol->user_size)
	{
	  int len = (int) sl->record_length;
	  int off = (int) sl->offset_to_record;

	  if (off + len > vol->user_size)
	    {
	      len = vol->user_size - off;
	    }
	  memset (bmap + off, (i == hl_slot) ? 4 : 0, len);	/* record (4 = highlighted) */
	  if (i == hl_slot)
	    {
	      int de = vol->user_size - (i + 1) * (int) sizeof (SPAGE_SLOT);
	      if (de >= 0)
		{
		  memset (bmap + de, 4, sizeof (SPAGE_SLOT));
		}
	    }
	}
    }
  if (p->w >= 24)
    {
      /* overlay: FLAT proportional render - every byte shown once, in order,
       * no folding and no "same" markers (the user asked to see it all) */
      int ci;

      /* Scrolling shows ncells cells from base.  base is not clamped here - a fill
	 function rewriting input state would give it three owners (fill, draw,
	 input).  volmap_ov_page_scroll_clamp settles it just before drawing. */
      for (ci = 0; ci < ncells; ci++)
	{
	  long off = (long) (ci + volmap_ov_page_base) * per;
	  int c = (row0 * p->w) + ci;

	  if (off >= vol->user_size)
	    {
	      p->glyph[c] = ' ';
	      continue;
	    }
	  {
	    /* majority class over this cell's byte span */
	    int cnt6[6] = { 0, 0, 0, 0, 0, 0 };
	    long e2 = off + per;
	    long k2;
	    int best = 0;

	    if (e2 > vol->user_size)
	      {
		e2 = vol->user_size;
	      }
	    for (k2 = off; k2 < e2; k2++)
	      {
		cnt6[bmap[k2] % 6]++;
	      }
	    for (k2 = 1; k2 < 6; k2++)
	      {
		if (cnt6[k2] > cnt6[best])
		  {
		    best = (int) k2;
		  }
	      }
	    p->glyph[c] = 'b';
	    p->aux[c] = 0;
	    p->color[c] = (char) (best + 10);
	  }
	}
      *per_out = per;
      return true;
    }
  /* ADAPTIVE layout: a uniform run spanning >= 2 rows at the base resolution
   * collapses into TWO class-colored label rows ("same" + its size); every
   * reclaimed row raises the resolution of the remaining, varied ranges. */
  {
    int c = row0 * p->w;
    int cend = (row0 + grows) * p->w;
    int ncol = 0, ncol2 = 0;
    int per_det = per;
    long col_start[16];
    long b;
    int it;

    /* iterate to a fixpoint: collapsing runs makes the rest finer, which can
     * stretch other uniform runs past the two-row mark - collapse those too */
    for (it = 0; it < 8; it++)
      {
	long thresh = (long) per_det * p->w * 4;	/* fold runs of >= 4 rows */
	long detailed = 0;
	int added = 0;

	for (b = 0; b < vol->user_size;)
	  {
	    long e = b;
	    bool have = false;
	    int k;

	    while (e < vol->user_size && bmap[e] == bmap[b])
	      {
		e++;
	      }
	    for (k = 0; k < ncol; k++)
	      {
		if (col_start[k] == b)
		  {
		    have = true;
		    break;
		  }
	      }
	    if (have)
	      {
		/* already collapsed */
	      }
	    else if (e - b >= thresh && ncol < 16 && (ncol + 1) * 5 <= grows - 1)
	      {
		col_start[ncol++] = b;
		added++;
	      }
	    else
	      {
		detailed += e - b;
	      }
	    b = e;
	  }
	if (ncol > 0 && detailed > 0)
	  {
	    per_det = (int) ((detailed + (long) (grows - 5 * ncol) * p->w - 1)
			     / ((long) (grows - 5 * ncol) * p->w));
	    /* Keep 4KB alignment: the main grid is fixed at 64B, so this detail column
	       uses the same grid.  Rounding to a multiple of 8 instead yields sizes
	       like 88B, and a cell then straddles a 4KB boundary, misaligning the
	       residency reading. */
	    per_det = ((per_det + VOLMAP_PAGE_CELL_BYTES - 1) / VOLMAP_PAGE_CELL_BYTES)
	      * VOLMAP_PAGE_CELL_BYTES;
	    if (per_det < VOLMAP_PAGE_CELL_BYTES)
	      {
		per_det = VOLMAP_PAGE_CELL_BYTES;
	      }
	  }
	if (added == 0)
	  {
	    break;
	  }
      }
    if (per_out != NULL)
      {
	*per_out = per_det;
      }
    /* pass 2: render runs in byte order */
    for (b = 0; b < vol->user_size && c < cend;)
      {
	long e = b;
	int cls = bmap[b];

	while (e < vol->user_size && bmap[e] == cls)
	  {
	    e++;
	  }
	{
	  bool have = false;
	  int km;

	  for (km = 0; km < ncol; km++)
	    {
	      if (col_start[km] == b)
		{
		  have = true;
		  break;
		}
	    }
	  if (have)
	  {
	    char hsz[24];
	    int k2;

	    ncol2++;
	    while (c % p->w != 0)
	      {
		p->glyph[c++] = ' ';	/* flush to a row boundary */
	      }
	    if (c + 2 * p->w > cend)
	      {
		break;
	      }
	    /* sandwich proportional to the repetition: shape rows / "xN" / shape
	     * rows - the mass of the pattern itself signals the scale
	     * (>= 100 rows: 3/1/3, otherwise 2/1/2) */
	    {
	      long total = (e - b + (long) per_det * p->w - 1)
		/ ((long) per_det * p->w);
	      int pat = (total >= 100) ? 3 : 2;
	      char cnt[24];
	      int pr2, nrows2 = 2 * pat + 1;

	      while (pat > 1 && c + nrows2 * p->w > cend)
		{
		  pat--;
		  nrows2 = 2 * pat + 1;
		}
	      snprintf (cnt, sizeof (cnt), "x%d", (int) total);
	      snprintf (hsz, sizeof (hsz), "%*s", p->w - 1, cnt);
	      for (pr2 = 0; pr2 < nrows2 && c + p->w <= cend; pr2++)
		{
		  for (k2 = 0; k2 < p->w; k2++)
		    {
		      if (pr2 == pat)
			{
			  p->glyph[c + k2] = (k2 < (int) strlen (hsz)) ? hsz[k2] : ' ';
			}
		      else
			{
			  p->glyph[c + k2] = 'b';
			  p->aux[c + k2] = 0;
			}
		      p->color[c + k2] = (char) (cls + 10);
		    }
		  c += p->w;
		}
	    }
	  }
	else
	  {
	    long b2;

	    for (b2 = b; b2 < e && c < cend; b2 += per_det)
	      {
		long b3 = (b2 + per_det < e) ? b2 + per_det : e;
		int cnt[6] = { 0, 0, 0, 0, 0, 0 }, k, best = 0;
		long k3;

		for (k3 = b2; k3 < b3; k3++)
		  {
		    cnt[bmap[k3]]++;
		  }
		for (k = 1; k < 6; k++)
		  {
		    if (cnt[k] > cnt[best])
		      {
			best = k;
		      }
		  }
		p->glyph[c] = 'b';
		p->color[c] = (char) (best + 10);
		p->aux[c] = 0;
		c++;
	      }
	  }
	}
	b = e;
      }
  }
  volmap_page_deadstats (vol, iopage, del, dead);
  return true;
}

/* footer rows in the same grammar as the legend: [glyph] word count.
 * '-' and 'D' are the slot-view characters for freed / deleted slots. */
static void
volmap_panel_foot (VOLMAP_PANEL * p, long del, long dead)
{
  if (del <= 999)
    {
      snprintf (p->foot[0], sizeof (p->foot[0]), "- del%3d", (int) del);
    }
  else
    {
      snprintf (p->foot[0], sizeof (p->foot[0]), "-del%4d", (int) del);
    }
  /* "vac" = vacuum-pending (unvacuumed dead versions): says what happens next
   * instead of the scary-sounding "dead" - the count is a to-do, not damage */
  if (dead <= 99999)
    {
      snprintf (p->foot[1], sizeof (p->foot[1]), "vac%5d", (int) dead);
    }
  else
    {
      snprintf (p->foot[1], sizeof (p->foot[1]), "vac>9999");
    }
  p->foot_val[0] = del;
  p->foot_val[1] = dead;
  p->nfoot = 2;
}

/* byte-class legend in the panel's trailing spare rows (bottom-aligned above
 * the footer).  Shown only when the same-shape compression left room; skipped
 * entirely on tight layouts. */
static void
volmap_panel_legend (VOLMAP_PANEL * p)
{
  static const struct
  {
    char cls;
    const char *txt;
  } items[5] =
  {
    {0, "rec"}, {1, "free"}, {3, "frag"}, {2, "dir"}, {5, "hdr"}
  };
  int w = p->w;
  int foot_top = p->rows - p->nfoot;
  int last = -1, r, i, k, n, start;

  for (r = foot_top - 1; r >= 0; r--)
    {
      bool blank = true;

      for (k = 0; k < w; k++)
	{
	  if (p->glyph[r * w + k] != ' ')
	    {
	      blank = false;
	      break;
	    }
	}
      if (!blank)
	{
	  last = r;
	  break;
	}
    }
  /* one blank row after the content; the legend may sit right above the
   * footer (both are caption-like rows and group naturally) */
  n = (foot_top - 1) - (last + 2) + 1;
  if (n > 5)
    {
      n = 5;
    }
  /* wide panels: two aligned columns (3 rows carry all 5 items); narrow: one per row */
  if (w >= 24)
    {
      if (p->rows < 10)
	{
	  return;
	}
      start = foot_top - 3;	/* the fill budget always leaves these rows free */
      for (i = 0; i < 5; i++)
	{
	  int c = (start + i / 2) * w + (i % 2) * (w / 2);
	  const char *t = items[i].txt;

	  p->glyph[c] = 'b';
	  p->aux[c] = 0;
	  p->color[c] = (char) (items[i].cls + 10);
	  for (k = 0; t[k] != '\0' && k < w / 2 - 3; k++)
	    {
	      p->glyph[c + 2 + k] = t[k];
	      p->color[c + 2 + k] = (char) (items[i].cls + 10);
	    }
	}
      return;
    }
  if (n < 3)
    {
      return;			/* too tight: no legend rather than a cryptic fragment */
    }
  start = foot_top - n;
  for (i = 0; i < n; i++)
    {
      int c = (start + i) * w;
      const char *t = items[i].txt;

      p->glyph[c] = 'b';
      p->aux[c] = 0;
      p->color[c] = (char) (items[i].cls + 10);
      for (k = 0; t[k] != '\0' && k < w - 2; k++)
	{
	  p->glyph[c + 2 + k] = t[k];
	  p->color[c + 2 + k] = (char) (items[i].cls + 10);
	}
    }
}

/* short page-type tag for the drill-down label ("what kind of page am I in") */
static const char *
volmap_ptype_tag (int ptype, signed char *color)
{
  *color = -1;			/* dim by default */
  switch (ptype)
    {
    case PAGE_HEAP:
      *color = 0;
      return "heap";
    case PAGE_BTREE:
      *color = 1;
      return "btree";
    case PAGE_OVERFLOW:
      *color = 4;		/* magenta: stands out - big-record continuation pages */
      return "overflow";
    case PAGE_CATALOG:
      *color = 2;
      return "catalog";
    case PAGE_FTAB:
      return "ftab";
    case PAGE_EHASH:
      return "ehash";
    case PAGE_QRESULT:
      return "qresult";
    case PAGE_VOLHEADER:
      return "volhdr";
    case PAGE_VOLBITMAP:
      return "volbmp";
    case PAGE_AREA:
      return "area";
    case PAGE_DROPPED_FILES:
      return "dropped";
    case PAGE_VACUUM_DATA:
      return "vacuum";
    default:
      return "?";
    }
}

/* write the type tag into one panel row (ASCII cells, kind-colored) */
static void
volmap_panel_tag_row (VOLMAP_PANEL * p, int row, int ptype, int cell_bytes)
{
  signed char col;
  const char *t = volmap_ptype_tag (ptype, &col);
  char sc[p->w + 2] = "";
  int k, tl = (int) strlen (t), sl;

  if (cell_bytes > 0)
    {
      if (cell_bytes < 1000)
	{
	  snprintf (sc, sizeof (sc), "%dB", cell_bytes);
	}
      else
	{
	  snprintf (sc, sizeof (sc), "%dK", cell_bytes / 1024);
	}
    }
  sl = (int) strlen (sc);
  for (k = 0; k < p->w; k++)
    {
      char g = ' ';
      signed char cc = col;

      if (k < tl)
	{
	  g = t[k];
	}
      else if (sl > 0 && tl + 1 + sl <= p->w && k >= p->w - sl)
	{
	  g = sc[k - (p->w - sl)];	/* byte-view scale: bytes per cell */
	  cc = -1;
	}
      p->glyph[row * p->w + k] = g;
      p->color[row * p->w + k] = cc;
      p->aux[row * p->w + k] = 0;
    }
}

/* levels 1 and 2: the byte distribution fills the panel, with the owning
 * sector labeled on the first row for hierarchy context */
static void
volmap_panel_pagebytes (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_PANEL * p, long pg, int hl_slot)
{
  char *iopage;
  long del = 0, dead = 0;

  if (p->cached_level == 1 && p->cached_key == pg && p->cached_hl == hl_slot)
    {
      return;
    }
  memset (p->glyph, ' ', p->w * p->rows);
  memset (p->color, 0, p->w * p->rows);
  memset (p->aux, 0, p->w * p->rows);
  if (p->rbg != NULL)
    {
      memset (p->rbg, 0, p->w * p->rows);
    }
  p->nfoot = 0;
  p->mid_row = -1;
  p->hl_cell = -1;
  iopage = volmap_scratch (ctx, vol->iopagesize);
  if (iopage == NULL || !volmap_read_iopage (vol, (PAGEID) pg, iopage))
    {
      p->cached_key = -1;
      p->cached_level = -1;
      return;
    }
  if (p->w >= 24)
    {
      /* overlay: the sector box above already names the sector, and the byte
       * classes carry their own legend - no label rows, all rows for data */
      p->mid_row = -1;
    }
  else
    {
      if (pg / VOLMAP_SECT_NPAGES <= 99999)
	{
	  snprintf (p->mid, sizeof (p->mid), "sec%5d", (int) (pg / VOLMAP_SECT_NPAGES));
	}
      else
	{
	  snprintf (p->mid, sizeof (p->mid), "s%7d", (int) (pg / VOLMAP_SECT_NPAGES));
	}
      p->mid_row = 0;
      p->mid_style = 0;
    }
  {
    int per_cell = 0;
    int row0 = (p->w >= 24) ? 0 : 2;

    if (volmap_panel_fill_pagebytes (ctx, vol, p, iopage, pg, hl_slot, row0,
				     p->rows - row0 - ((p->w >= 24) ? 3 : (p->rows >= 10) ? 6 : 0),
				     &del, &dead, &per_cell))
      {
	p->bytes_per_cell = per_cell;
	if (p->w < 24)
	  {
	    volmap_panel_tag_row (p, 1, ((const FILEIO_PAGE_RESERVED *) iopage)->ptype, per_cell);
	  }
      }
    else
      {
	p->mid_row = -1;
	p->cached_key = -1;
	p->cached_level = -1;
	return;
      }
  }
  if (p->w >= 24)
    {
      /* wide overlay box: the drawer paints a compact 2-row legend+counters
       * over the last two rows - just stash the numbers */
      p->foot_val[0] = del;
      p->foot_val[1] = dead;
      p->nfoot = 0;
    }
  else
    {
      if (p->rows >= 10)
	{
	  volmap_panel_foot (p, del, dead);
	}
      volmap_panel_legend (p);
    }
  p->cached_key = pg;
  p->cached_hl = hl_slot;
  p->cached_level = 1;
}

/* print one panel row: " | " separator + 8 cells */
static void
volmap_panel_print_row (VOLMAP_CTX * ctx, VOLMAP_PANEL * p, int row)
{
  int i;

  (void) ctx;
  if (row == p->mid_row)
    {
      if (p->mid_style == 1)
	{
	  printf (VOLMAP_CURSOR_SGR "%-*.*s\033[0m", p->w, p->w, p->mid);	/* one cursor colour */
	}
      else
	{
	  printf ("\033[38;5;245m%-*.*s\033[0m", p->w, p->w, p->mid);
	}
      return;
    }
  if (p->nfoot > 0 && row >= p->rows - p->nfoot)
    {
      int fi = row - (p->rows - p->nfoot);

      if (p->foot_val[fi] > 0)
	{
	  /* nonzero counts get an alert background: deleted = yellow, vacuum-pending = red */
	  printf ("%s%-*.*s\033[0m", (fi == 1) ? "\033[41;97m" : "\033[43;30m", p->w, p->w, p->foot[fi]);
	}
      else
	{
	  printf ("\033[38;5;%dm%-*.*s\033[0m", (fi == 1) ? 131 : 245, p->w, p->w, p->foot[fi]);
	}
      return;
    }
  for (i = 0; i < p->w; i++)
    {
      int c = row * p->w + i;
      char g = (p->glyph != NULL) ? p->glyph[c] : ' ';
      bool hl = (c == p->hl_cell);

      if (p->rbg != NULL)
	{
	  /* -m shade, same contract as the map (>=80% bright / >=30% dim).
	     Cells with no glyph are left unshaded: on a wide box the content ends
	     early and shading the margin reads as "the empty area is cached". */
	  fputs ((g == ' ' || g == '\0') ? "\033[49m"
		 : (p->rbg[c] == 2) ? "\033[48;5;240m"
		 : (p->rbg[c] == 1) ? "\033[48;5;237m" : "\033[49m", stdout);
	}
      if (hl)
	{
	  fputs (VOLMAP_CURSOR_SGR, stdout);	/* same colour as the map cursor */
	}
      if (p->color[c] == VOLMAP_PCOL_DIM && !hl && g != ' ' && g != '\0')
	{
	  /* Pages outside the map cell are dimmed to grey regardless of glyph kind.
	     Restricting this to allocated pages leaves volumes with many
	     unallocated '_' cells showing no dimming at all. */
	  static const char *dimramp[5] = { "\xe2\xa0\xbf", "\xe2\xa0\xbe", "\xe2\xa0\xb6",
	    "\xe2\xa0\xb4", "\xe2\xa0\xa4"
	  };
	  printf ("\033[38;5;240m%s", (g == 'b') ? dimramp[(int) p->aux[c] % 5] : "\xe2\xa0\x82");
	  continue;
	}
      switch (g)
	{
	case 'b':
	  if (p->color[c] >= 10 && p->color[c] <= 15)
	    {
	      /* byte classes with CLASS-SPECIFIC glyphs (shape + color double coding).
	       * 0 rec wears the OWNING FILE's kind color (same contract as the map and
	       * the sector grid: data=blue 34, index=green 32) so the byte view can
	       * never contradict the sector's color; the space-keeping classes are
	       * NEUTRAL grays (1 contig-free=242, 2 dir=245, 3 frag=131) so none of
	       * them collides with a kind color.  4 = cursor slot (blue bg =
	       * selection), 5 = header (magenta - distinct from every kind BASE color;
	       * the index ALT tone shares 35 but only appears as a map/grid ramp). */
	      static const char *bc[6] = { NULL, "\033[38;5;242m\xe2\xa0\xa4", "\033[38;5;245m\xe2\xa0\x9b",
		"\033[38;5;131m\xe2\xa0\xb6", "\033[44;37m\xe2\xa0\xbf\033[49m", "\033[35m\xe2\xa0\x89"
	      };
	      if (p->color[c] == 10)
		{
		  printf ("\033[%dm\xe2\xa0\xbf", (p->rec_col > 0) ? p->rec_col : 37);
		}
	      else
		{
		  printf ("%s", bc[p->color[c] - 10]);
		}
	    }
	  else
	    {
	      static const char *ramp[5] =
		{ "\xe2\xa0\xbf", "\xe2\xa0\xbe", "\xe2\xa0\xb6", "\xe2\xa0\xb4", "\xe2\xa0\xa4" };
	      static const int kcol[6] = { 34, 32, 31, 31, 35, 37 };
	      static const int kcol_alt[4] = { 36, 35, 91, 91 };	/* flipped boundary tone, as on the map */
	      if (p->color[c] >= 20 && p->color[c] <= 25)
		{
		  printf ("\033[38;5;%dm%s", file_palette[p->color[c] - 20], ramp[(int) p->aux[c] % 5]);
		}
	      else if (p->color[c] >= 30 && p->color[c] <= 33)
		{
		  printf ("\033[%dm%s", kcol_alt[p->color[c] - 30], ramp[(int) p->aux[c] % 5]);
		}
	      else
		{
		  printf ("\033[%dm%s", kcol[(int) p->color[c] % 6], ramp[(int) p->aux[c] % 5]);
		}
	    }
	  break;
	  /* On the cursor cell (hl) the foreground is not repainted.  A glyph setting its
	     own colour overwrites the cursor SGR already emitted, and '_'
	     (unallocated) uses the same orange (208) as the cursor background, so the
	     cursor vanishes entirely on sectors with many unallocated pages. */
	case '#':
	  printf ("%s#", hl ? "" : "\033[38;5;238m");
	  break;
	case '_':
	  printf ("%s\xe2\xa0\x82", hl ? "" : "\033[38;5;208m");
	  break;
	case '.':
	  if (p->color[c] >= 10 && p->color[c] <= 15)
	    {
	      goto label_text;	/* '.' inside a "same"-label size string */
	    }
	  printf ("%s\xe2\xa0\x82", hl ? "" : "\033[38;5;242m");
	  break;
	case 'E':
	  printf ("%sE", hl ? "" : "\033[31m");
	  break;
	case '?':
	  printf ("%s?", hl ? "" : "\033[38;5;243m");
	  break;
	default:
	label_text:
	  if (g > ' ' && g < 0x7f)
	    {
	      /* marker text in the color of what it stands for */
	      static const int clsfg[6] = { 32, 34, 36, 33, 37, 35 };
	      static const int kfg[6] = { 34, 32, 31, 31, 35, 37 };
	      int cc = (p->color[c] >= 10 && p->color[c] <= 15) ? clsfg[p->color[c] - 10]
		: (p->color[c] >= 0 && p->color[c] <= 5) ? kfg[(int) p->color[c]] : 0;

	      if (cc > 0)
		{
		  printf ("\033[%dm%c", cc, g);
		}
	      else
		{
		  printf ("\033[2;38;5;245m%c\033[0m", g);
		}
	    }
	  else
	    {
	      putchar (' ');
	    }
	  break;
	}
      if (hl)
	{
	  printf ("\033[49;22m");
	}
    }
  printf ("\033[0m");
}

/* ── unified cell model: ONE classify + ONE paint for the batch renderer and
 * the interactive level-0 map (they had drifted into four semantic copies) ── */

typedef struct volmap_cell VOLMAP_CELL;
struct volmap_cell
{
  long p0;
  long nres_pages, nalloc, npages_cell;
  int maj;			/* owner file index, -1 = none */
  int kind;			/* volmap_kind_idx (owner), -1 if none */
  int lvl;			/* 0..4 braille ramp step */
  char state;			/* 'F' '#' '~' '_' '?' '.' ' ' (same contract as cell_state[]) */
  signed char tone;		/* file-boundary tone used FOR this cell */
  signed char res_bg;		/* -2 residency off, -1 cold, 0 = 48;5;237, 1 = 48;5;240 */
  signed char hwm;		/* cell contains the volume's high-water mark page (sky-blue bg) */
  signed char ovf;		/* owner is an overflow appendage (desaturated glyph tone) */
  signed char chj;		/* cell holds a logical-chain JUMP source ([p] view, red bg) */
  signed char buf_bg;		/* --bufmap: 0 none, 1 buffered clean (teal bg), 2 buffered dirty (purple bg) */
  long nbuf_cell, ndirty_cell;	/* pages of this cell in the buffer pool / dirty */
};

/* classify one map cell. Pure: reads volume state only; no output, no arrays.
 * tone_before: flip the boundary tone before this cell is painted (interactive
 * convention); false = flip after painting (historical batch behavior). */
static void
volmap_cell_classify (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long p0, long per_pages, long total_pages,
		      int *prev_maj, int *tone, bool tone_before, VOLMAP_CELL * out)
{
  long p, p_end = p0 + per_pages;

  memset (out, 0, sizeof (*out));
  out->p0 = p0;
  out->maj = -1;
  out->kind = -1;
  out->res_bg = -2;
  out->state = ' ';
  if (ctx->residency && vol->res_prefix != NULL && p0 < total_pages)
    {
      long pe = (p_end > total_pages) ? total_pages : p_end;
      long rp = (long) vol->res_prefix[pe] - (long) vol->res_prefix[p0];
      int sub = vol->iopagesize / 4096;
      double rr = (pe > p0) ? (double) rp / ((double) (pe - p0) * sub) : 0.0;

      out->res_bg = (signed char) ((rr >= 0.8) ? 1 : (rr >= 0.3) ? 0 : -1);
    }
  if (vol->hwm_page >= 0 && (long) vol->hwm_page >= p0 && (long) vol->hwm_page < p0 + per_pages)
    {
      out->hwm = 1;
    }
  if (ctx->bufmap && ctx->bufmap_loaded && vol->buf_prefix != NULL && p0 < total_pages)
    {
      long pe = (p_end > total_pages) ? total_pages : p_end;

      out->nbuf_cell = (long) vol->buf_prefix[pe] - (long) vol->buf_prefix[p0];
      out->ndirty_cell = (long) vol->dirty_prefix[pe] - (long) vol->dirty_prefix[p0];
      out->buf_bg = (signed char) ((out->ndirty_cell > 0) ? 2 : (out->nbuf_cell > 0) ? 1 : 0);
    }
  if (p0 >= total_pages)
    {
      out->tone = (signed char) *tone;
      return;
    }
  if (p_end > total_pages)
    {
      p_end = total_pages;
    }
  for (p = p0; p < p_end;)
    {
      /* whole-sector spans use popcount on the page bitmap: 64x fewer
       * iterations than a per-page walk on every full frame */
      DKNSECTS sec = (DKNSECTS) (p / VOLMAP_SECT_NPAGES);
      int bit = (int) (p % VOLMAP_SECT_NPAGES);
      int span = VOLMAP_SECT_NPAGES - bit;

      if (span > (int) (p_end - p))
	{
	  span = (int) (p_end - p);
	}
      out->npages_cell += span;
      if (vol->stab[sec])
	{
	  UINT64 m = (span >= VOLMAP_SECT_NPAGES) ? ~(UINT64) 0 : ((((UINT64) 1 << span) - 1) << bit);

	  out->nres_pages += span;
	  out->nalloc += __builtin_popcountll (vol->pagebm[sec] & m);
	  if (volmap_chain_volid == vol->volid && volmap_chain_bm != NULL
	      && sec < volmap_chain_bm_nsect && (volmap_chain_bm[sec] & m))
	    {
	      out->chj = 1;
	    }
	  if (out->maj < 0 && vol->owner[sec] >= 0)
	    {
	      out->maj = vol->owner[sec];
	    }
	}
      p += span;
    }
  if (tone_before && out->maj >= 0)
    {
      if (*prev_maj >= 0 && out->maj != *prev_maj)
	{
	  *tone ^= 1;
	}
      *prev_maj = out->maj;
    }
  out->tone = (signed char) *tone;
  if (!tone_before && out->maj >= 0)
    {
      if (*prev_maj >= 0 && out->maj != *prev_maj)
	{
	  *tone ^= 1;
	}
      *prev_maj = out->maj;
    }
  if (out->nres_pages == 0)
    {
      out->state = '.';
    }
  else if (out->maj < 0 && p0 <= (long) vol->sys_lastpage)
    {
      out->state = '#';
    }
  else if (ctx->scan_active && out->maj < 0)
    {
      out->state = '~';		/* progressive scan has not attributed this sector yet */
    }
  else if (out->maj < 0)
    {
      /* reserved, but no file claims it: unknown, not empty.  Its allocation is not
         known either (it comes from the owner's file table), so this is decided
         before the fill test, or the cell would pass for reserved-but-empty */
      out->state = '?';
    }
  else if (out->nalloc * 50 < out->npages_cell)
    {
      out->state = '_';
    }
  else
    {
      double r = (out->npages_cell > 0) ? (double) out->nalloc / out->npages_cell : 0.0;

      out->state = 'F';
      out->kind = volmap_kind_idx (ctx->files[out->maj].ftype);
      out->ovf = (signed char) (volmap_obj_role (ctx->files[out->maj].ftype) == 2);
      out->lvl = (r >= 0.9) ? 0 : (r >= 0.7) ? 1 : (r >= 0.5) ? 2 : (r >= 0.3) ? 3 : 4;
    }
}

/* paint style bits (historical per-renderer byte formats, unified elsewhere) */
#define VOLMAP_PAINT_LEAD_RESET 0x1	/* prefix "\033[0m" (volmap_put_cell contract) */
#define VOLMAP_PAINT_BG_RESET   0x2	/* cold residency emits "\033[49m" (batch) instead of nothing */
#define VOLMAP_PAINT_DARK_QMARK 0x4	/* '?' in 238 (historical batch) instead of 243 */

/* render one classified cell into buf (NUL-terminated) */
static void
volmap_cell_paint (const VOLMAP_CELL * cell, VOLMAP_CTX * ctx, bool plain, int style, char *buf, size_t bufsz)
{
  const char *bg = "";
  int n = 0;

  if (plain)
    {
      char out = cell->state;

      if (cell->state == 'F')
	{
	  const char *fam = kind_family[cell->kind];

	  out = fam[cell->maj % (int) strlen (fam)];
	}
      else if (cell->state == ' ')
	{
	  out = ' ';
	}
      snprintf (buf, bufsz, "%c", out);
      return;
    }
  if (cell->res_bg == 1)
    {
      bg = "\033[48;5;240m";
    }
  else if (cell->res_bg == 0)
    {
      bg = "\033[48;5;237m";
    }
  else if (cell->hwm)
    {
      /* high-water mark cell: sky-blue background.  The residency grays above
       * win while the area is cached; when the cache lets go, the sky returns. */
      bg = "\033[48;5;24m";
    }
  if (cell->buf_bg == 2)
    {
      bg = "\033[48;5;53m";	/* --bufmap: in buffer pool and DIRTY (not yet written to the volume) */
    }
  else if (cell->buf_bg == 1)
    {
      bg = "\033[48;5;23m";	/* --bufmap: in buffer pool, clean (same content as the volume) */
    }
  if (cell->state == 'F' && cell->maj >= 0 && cell->maj < ctx->nfiles)
    {
      /* object/file marking: role backgrounds win over residency/hwm shading
       * while the operator is inspecting - heap navy, its indexes dark green,
       * overflow appendages dark purple */
      int marked = 0;

      if (volmap_mark_class_on && OID_EQ (&ctx->files[cell->maj].class_oid, &volmap_mark_class))
	{
	  marked = 1;
	}
      else if ((volmap_focus_file >= 0 && cell->maj == volmap_focus_file)
	       || (volmap_mark_file >= 0 && cell->maj == volmap_mark_file))
	{
	  marked = 1;
	}
      if (marked)
	{
	  int role = volmap_obj_role (ctx->files[cell->maj].ftype);

	  /* Object marking uses the background, and so does residency (-m), so with both
	     on the marking hides whether a page is cached.  While marking is active
	     residency is shown as background brightness instead - high/medium/low as
	     lighter tones of the same hue - so both readings survive. */
	  if (cell->res_bg == 1)
	    {
	      bg = (role == 2) ? "\033[48;5;136m" : (role == 1) ? "\033[48;5;28m" : "\033[48;5;26m";
	    }
	  else if (cell->res_bg == 0)
	    {
	      bg = (role == 2) ? "\033[48;5;130m" : (role == 1) ? "\033[48;5;25m" : "\033[48;5;19m";
	    }
	  else
	    {
	      bg = (role == 2) ? "\033[48;5;94m" : (role == 1) ? "\033[48;5;22m" : "\033[48;5;17m";
	    }
	}
    }
  if (cell->chj)
    {
      bg = "\033[48;5;52m";	/* [p]: logical-chain jump lives here - dark red */
    }
  else if (cell->res_bg == -1 && (style & VOLMAP_PAINT_BG_RESET))
    {
      bg = "\033[49m";
    }
  if (style & VOLMAP_PAINT_LEAD_RESET)
    {
      n = snprintf (buf, (size_t) bufsz, "\033[0m");
    }
  switch (cell->state)
    {
    case ' ':
      snprintf (buf + n, bufsz - (size_t) n, "%s ", bg);
      break;
    case '.':
      snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;242m\xe2\xa0\x82", bg);
      break;
    case '#':
      snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;238m#", bg);
      break;
    case '~':
      snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;238m\xe2\xa0\x92", bg);
      break;
    case '_':
      snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;208m\xe2\xa0\x82", bg);
      break;
    case '?':
      snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;%dm?", bg, (style & VOLMAP_PAINT_DARK_QMARK) ? 238 : 243);
      break;
    default:			/* 'F' */
      if (volmap_focus_file >= 0 && cell->maj != volmap_focus_file
	  && !(volmap_mark_class_on && cell->maj >= 0 && cell->maj < ctx->nfiles
	       && OID_EQ (&ctx->files[cell->maj].class_oid, &volmap_mark_class)))
	{
	  /* file view: everything outside the selected file's OBJECT recedes to gray */
	  snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;238m%s", bg, kind_ramp[cell->lvl]);
	}
      else if (cell->ovf)
	{
	  /* overflow appendage: light orange - one simple rule, "orange ramp =
	   * overflow", readable next to its heap/index even without marking
	   * (the reserved-empty dot is also orange but a different glyph shape) */
	  snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;215m%s", bg, kind_ramp[cell->lvl]);
	}
      else if (cell->kind == 2 || cell->kind == 3)
	{
	  snprintf (buf + n, bufsz - (size_t) n, "%s\033[%dm%s", bg, 31 + (cell->tone ? 60 : 0), kind_ramp[cell->lvl]);
	}
      else if (cell->kind == 4)
	{
	  snprintf (buf + n, bufsz - (size_t) n, "%s\033[38;5;%dm%s", bg, file_palette[cell->maj % 6], kind_ramp[cell->lvl]);
	}
      else
	{
	  int col = (cell->kind == 0) ? (cell->tone ? 36 : 34) : (cell->tone ? 35 : 32);

	  snprintf (buf + n, bufsz - (size_t) n, "%s\033[%dm%s", bg, col, kind_ramp[cell->lvl]);
	}
      break;
    }
  if ((cell->hwm || cell->buf_bg > 0) && cell->res_bg != 1 && cell->res_bg != 0 && (style & VOLMAP_PAINT_BG_RESET))
    {
      /* batch rows carry SGR state across cells: close the sky/buffer background so it
       * cannot bleed into the neighbours (interactive cells lead with \033[0m) */
      size_t ln = strlen (buf);

      snprintf (buf + ln, (size_t) bufsz - ln, "\033[49m");
    }
}

/* interactive mode dies by signal: put the terminal back before exiting, so a
 * Ctrl-C or kill never leaves the operator's console in raw/mouse-reporting mode */
struct termios volmap_term_saved;
volatile sig_atomic_t volmap_term_saved_ok = 0;

/* Window resize (SIGWINCH): the handler only sets a flag; select() wakes with
   EINTR.  Without it the size is read once per frame and select() blocks waiting
   for a key, so the screen does not follow a resize until a key is pressed. */
static volatile sig_atomic_t volmap_resized = 0;

static void
volmap_on_winch (int sig)
{
  (void) sig;
  volmap_resized = 1;
}

static void
volmap_on_signal (int sig)
{
  static const char restore[] = "\033]111\007\033[?1006l\033[?1000l\033[?7h\033[?25h\033[0m\r\n";

  (void) sig;
  if (volmap_term_saved_ok)
    {
      (void) write (1, restore, sizeof (restore) - 1);
      (void) tcsetattr (0, TCSANOW, &volmap_term_saved);
    }
  _exit (130);
}

/* the map's boundary tone at cell `cell_idx` — replays the classification walk so the
 * side panel and the L1 page window reuse the EXACT color the map shows at that spot
 * (same file = same color; the alternate tone appears only at real file boundaries) */
static int
volmap_map_tone_at (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, int cell_idx, long per_pages, long total_pages,
		    int *maj_out)
{
  int prev_maj = -1, tone = 0, c;
  VOLMAP_CELL cell;

  cell.tone = 0;
  cell.maj = -1;
  for (c = 0; c <= cell_idx; c++)
    {
      volmap_cell_classify (ctx, vol, (long) c * per_pages, per_pages, total_pages, &prev_maj, &tone, true, &cell);
    }
  if (maj_out != NULL)
    {
      *maj_out = cell.maj;
    }
  return cell.tone;
}

/* Decode the on-disk volume header (page 0) into one status line:
 * the raw header fields, readable without spacedb and without a server */
static void
volmap_describe_vhdr (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, char *out, size_t outsz)
{
  char *iopage = volmap_scratch (ctx, vol->iopagesize);
  const DISK_VOLUME_HEADER *vhdr;
  char ts[24] = "?";
  time_t t;

  if (iopage == NULL || !volmap_read_iopage (vol, 0, iopage))
    {
      snprintf (out, outsz, "volume header p0: read failed");
      return;
    }
  vhdr = (const DISK_VOLUME_HEADER *) (iopage + prv_user_offset ());
  t = (time_t) vhdr->db_creation;
  {
    /* db_creation comes off the disk: a torn page 0 can hold a value localtime
       cannot represent.  Show the raw value then, rather than crash or hide it. */
    struct tm tmv;

    if (localtime_r (&t, &tmv) != NULL)
      {
	strftime (ts, sizeof (ts), "%Y-%m-%d %H:%M", &tmv);
      }
    else
      {
	snprintf (ts, sizeof (ts), "?(%lld)", (long long) vhdr->db_creation);
      }
  }

  /* Everything up to db_creation is at the same offset in every release, so it is
     printed unconditionally.  chkpt_lsa and next_volid follow vol_creation, which
     only exists from 11.4 - on an older volume they are 8 bytes earlier, and with
     an undetermined version their position is not known at all. */
  {
    char tail[96];
    const char *base = (const char *) vhdr;
    int shift = (ctx->vlayout == VOLMAP_VLAY_114) ? 0 : -8;

    if (ctx->vlayout == VOLMAP_VLAY_UNKNOWN)
      {
	snprintf (tail, sizeof (tail), "chkpt_lsa ?  next_vol ?  (db version unknown)");
      }
    else
      {
	LOG_LSA lsa;
	INT16 nv;

	memcpy (&lsa, base + offsetof (DISK_VOLUME_HEADER, chkpt_lsa) + shift, sizeof (lsa));
	memcpy (&nv, base + offsetof (DISK_VOLUME_HEADER, next_volid) + shift, sizeof (nv));
	snprintf (tail, sizeof (tail), "chkpt_lsa %lld|%d  next_vol %d",
		  (long long) lsa.pageid, (int) lsa.offset, (int) nv);
      }
    snprintf (out, outsz,
	      "vhdr p0: iopg %d  %s  sect %dpg  nsect %d/%d(max)  stab p%d x%d  syslast %d  "
	      "%s  db_created %s",
	      (int) vhdr->iopagesize, (vhdr->purpose == DB_TEMPORARY_DATA_PURPOSE) ? "TEMP" : "PERM",
	      (int) vhdr->sect_npgs, (int) vhdr->nsect_total, (int) vhdr->nsect_max,
	      (int) vhdr->stab_first_page, (int) vhdr->stab_npages, (int) vhdr->sys_lastpage, tail, ts);
  }
}

/* one slot of one page as a status line: the record OID plus the slot's type/length */
static void
volmap_describe_slot (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long page, long slotid, char *out, size_t outsz)
{
  char *iopage = volmap_scratch (ctx, vol->iopagesize);
  int prv = prv_user_offset ();
  const SPAGE_HEADER *sp;
  int nslots = 0;

  if (iopage == NULL || !volmap_read_iopage (vol, (PAGEID) page, iopage))
    {
      snprintf (out, outsz, "vol %d \xe2\x80\xba page %ld \xe2\x80\xba slot %ld", (int) vol->volid, page, slotid);
      return;
    }
  sp = (const SPAGE_HEADER *) (iopage + prv);
  nslots = (sp->num_slots >= 0 && sp->num_slots <= vol->user_size / (int) sizeof (SPAGE_SLOT)) ? sp->num_slots : 0;
  if (slotid < nslots)
    {
      const SPAGE_SLOT *sl = (const SPAGE_SLOT *) (iopage + prv + vol->user_size - sizeof (SPAGE_SLOT)) - slotid;
      static const char *tn[8] = { "?", "assign", "home", "newhome", "relocation", "bigone", "markdeleted",
	"deleted_will_reuse"
      };
      int tt = (sl->record_type < 8) ? (int) sl->record_type : 0;

      if (sl->offset_to_record == 0)
	{
	  snprintf (out, outsz, "OID %d|%ld|%ld  freed slot  (H=home N=newhome R=relocation B=bigone D=deleted A=assign -=freed)",
		    (int) vol->volid, page, slotid);
	}
      else
	{
	  snprintf (out, outsz, "OID %d|%ld|%ld  %s  len %uB  off %uB  (H=home N=newhome R=relocation B=bigone D=deleted A=assign -=freed)",
		    (int) vol->volid, page, slotid, tn[tt], (unsigned) sl->record_length,
		    (unsigned) sl->offset_to_record);
	}
    }
  else
    {
      snprintf (out, outsz, "OID %d|%ld|%ld  (beyond num_slots %d)", (int) vol->volid, page, slotid, nslots);
    }
}

/* ── drill-down overlay: three stacked boxes drawn OVER the map (cubmem-style).
 * [sector] grid on top, [page] byte distribution below it, [slots] at the bottom -
 * the selection in the sector grid drives both lower boxes, so every zoom depth
 * is visible at once.  Focus (space) moves the arrow keys into the overlay:
 * focus 1 = pick a page in the grid, focus 2 = pick a slot (OID on the status
 * line).  Border glyphs are EAW-ambiguous box drawing, used for the frame only. */
/* print a UTF-8 string clipped+padded to exactly `cols` terminal columns.
 * Widths: ASCII=1, braille U+2800..U+28FF=1 (3 bytes!), other 3-byte (CJK)=2. */
static int volmap_disp_clip (const char *t, int cols, int *out_w);

static void
volmap_put_padded (const char *t, int cols)
{
  int w = 0;
  int nbytes = volmap_disp_clip (t, cols, &w);
  bool had_sgr = (t != NULL && memchr (t, '\033', (size_t) nbytes) != NULL);

  if (t != NULL && nbytes > 0)
    {
      fwrite (t, 1, (size_t) nbytes, stdout);
    }
  if (had_sgr)
    {
      fputs ("\033[0m", stdout);	/* a clipped badge must not bleed into the padding */
    }
  for (; w < cols; w++)
    {
      putchar (' ');
    }
}

/* Map a key typed on a Hangul (2-beolsik) layout back to its QWERTY key, so an
   active IME does not swallow commands (q arriving as the Hangul jamo still
   closes).  Shared by the main loop and the help pager.
   cp = Unicode code point.  Returns the key string, "" if not a mapped jamo. */
static const char *
volmap_hangul_to_keys (unsigned int cp)
{
  static const char *jamo_keys[51] = {	/* U+3131..U+3163 */
    "r", "R", "rt", "s", "sw", "sg", "e", "E", "f", "fr", "fa", "fq", "ft", "fx", "fv", "fg",
    "a", "q", "Q", "qt", "t", "T", "d", "w", "W", "c", "z", "x", "v", "g",
    "k", "o", "i", "O", "j", "p", "u", "P", "h", "hk", "ho", "hl", "y",
    "n", "nj", "np", "nl", "b", "m", "ml", "l"
  };
  static const char *cho_keys[19] = {
    "r", "R", "s", "e", "E", "f", "a", "q", "Q", "t", "T", "d", "w", "W", "c", "z", "x", "v", "g"
  };
  static const char *jong_keys[28] = {
    "", "r", "R", "rt", "s", "sw", "sg", "e", "f", "fr", "fa", "fq", "ft", "fx", "fv", "fg",
    "a", "q", "qt", "t", "T", "d", "w", "c", "z", "x", "v", "g"
  };
  static char keys[8];

  keys[0] = '\0';
  if (cp >= 0x3131 && cp <= 0x3163)
    {
      snprintf (keys, sizeof (keys), "%s", jamo_keys[cp - 0x3131]);
    }
  else if (cp >= 0xAC00 && cp <= 0xD7A3)
    {
      unsigned int sy = cp - 0xAC00;

      snprintf (keys, sizeof (keys), "%s%s%s", cho_keys[sy / 588],
		jamo_keys[30 + (sy % 588) / 28], jong_keys[sy % 28]);
    }
  return keys;
}

/* Valid anchor_type values for a slotted page header (slotted_page.h:
   ANCHORED .. UNANCHORED_KEEP_SEQUENCE).  Outside this range the bytes are not
   a SPAGE_HEADER. */
/* Find the heap that owns an overflow file.
   An overflow file has no class_oid of its own.  A heap's header page (slot 0)
   holds HEAP_HDR_STATS laid out as { OID class_oid; VFID ovf_vfid; ... }, so the
   6 bytes after class_oid (8B) identify the overflow file it uses.  Walk the
   heaps, read those links and build the reverse map.
   Returns the owning heap's file index, or -1. */
static int
volmap_ovf_owner_file (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, int ovf_idx)
{
  /* The describe line asks again on every frame for the same cell; remember the
     last answer so a cursor resting on an overflow page costs one walk, not one
     per frame.  Keyed on the refresh generation too: the heap header's link may
     not be on disk yet on a live database, so a miss - or a hit - holds only
     until the next refresh rereads the volumes. */
  static const VOLMAP_FILE *memo_files;
  static int memo_nfiles = -1, memo_ovf = -1, memo_owner = -1;
  static unsigned memo_gen;
  int i;

  (void) vol;
  if (ovf_idx < 0 || ovf_idx >= ctx->nfiles)
    {
      return -1;
    }
  if (memo_files == ctx->files && memo_nfiles == ctx->nfiles && memo_ovf == ovf_idx
      && memo_gen == ctx->refresh_gen)
    {
      return memo_owner;
    }
  memo_gen = ctx->refresh_gen;
  memo_files = ctx->files;
  memo_nfiles = ctx->nfiles;
  memo_ovf = ovf_idx;
  memo_owner = -1;
  for (i = 0; i < ctx->nfiles; i++)
    {
      VOLMAP_FILE *hf = &ctx->files[i];
      VOLMAP_VOLUME *hv;
      char *iop;
      long hpg;
      int guess;

      if (hf->ftype != FILE_HEAP && hf->ftype != FILE_HEAP_REUSE_SLOTS)
	{
	  continue;
	}
      /* the heap header lives in the heap's own volume, not the one on screen */
      hv = volmap_find_vol (ctx, hf->vfid.volid);
      if (hv == NULL)
	{
	  continue;
	}
      iop = volmap_scratch (ctx, hv->iopagesize);
      if (iop == NULL)
	{
	  return -1;
	}
      /* the heap header page is within a page or two after the file header */
      for (guess = 1; guess <= 2; guess++)
	{
	  const SPAGE_HEADER *sp;
	  const FILEIO_PAGE_RESERVED *pr;
	  const char *ud;
	  int us, off, ln;
	  unsigned int wslot;
	  int ovol, ofid;

	  hpg = (long) hf->vfid.fileid + guess;
	  if (hpg < 0 || hpg >= (long) hv->nsect_total * VOLMAP_SECT_NPAGES)
	    {
	      break;
	    }
	  if (!volmap_read_iopage (hv, (PAGEID) hpg, iop))
	    {
	      continue;
	    }
	  pr = (const FILEIO_PAGE_RESERVED *) iop;
	  if (pr->ptype != PAGE_HEAP || pr->pageid != (PAGEID) hpg || pr->volid != hf->vfid.volid)
	    {
	      continue;
	    }
	  ud = iop + prv_user_offset ();
	  sp = (const SPAGE_HEADER *) ud;
	  if (sp->num_slots < 1)
	    {
	      continue;
	    }
	  us = hv->user_size;
	  wslot = *(const unsigned int *) (ud + us - 4);	/* slot 0 */
	  off = (int) (wslot & 0x3FFF);
	  ln = (int) ((wslot >> 14) & 0x3FFF);
	  if (ln < 14 || off < 0 || off + ln > us)
	    {
	      continue;
	    }
	  ofid = *(const int *) (ud + off + 8);		/* the 8 bytes after class_oid are ovf_vfid */
	  ovol = *(const short *) (ud + off + 12);
	  if (ofid > 0 && ovol == (int) ctx->files[ovf_idx].vfid.volid
	      && ofid == (int) ctx->files[ovf_idx].vfid.fileid)
	    {
	      memo_owner = i;
	      return i;
	    }
	  break;
	}
    }
  return -1;
}

static int
spage_is_valid_anchor (int a)
{
  /* slotted_page.h: ANCHORED=1, ANCHORED_DONT_REUSE_SLOTS=2,
     UNANCHORED_ANY_SEQUENCE=3, UNANCHORED_KEEP_SEQUENCE=4.  0 is undefined.
     The range must include 4 - B-tree pages use UNANCHORED_KEEP_SEQUENCE, and
     rejecting it reports healthy index pages as "corrupt?". */
  return a >= 1 && a <= 4;
}

/* Display width - the single source of truth.
   Rules: SGR (\033...m) = 0 columns; a 3-byte sequence starting 0xE2/0xE1
   (symbols, braille) = 1 column; other 3-byte UTF-8 (Hangul etc.) = 2 columns;
   2-byte = 1; ASCII = 1.  Every width computation goes through this function.

   Walks t from the start up to at most 'cols' columns.
   Returns bytes consumed (truncating there fits within cols).
   *out_w = actual display width.  cols < 0 walks to the end (full measure). */
static int
volmap_disp_clip (const char *t, int cols, int *out_w)
{
  int i = 0, w = 0;

  if (t == NULL)
    {
      if (out_w != NULL)
	{
	  *out_w = 0;
	}
      return 0;
    }
  while (t[i] != '\0')
    {
      if (t[i] == '\033')
	{
	  while (t[i] != '\0' && t[i] != 'm')
	    {
	      i++;
	    }
	  if (t[i] == 'm')
	    {
	      i++;
	    }
	  continue;
	}
      {
	unsigned char c0 = (unsigned char) t[i];
	int nb = ((c0 & 0xF0) == 0xE0) ? 3 : ((c0 & 0xE0) == 0xC0) ? 2 : 1;
	int cw = (nb == 3 && c0 != 0xE2 && c0 != 0xE1) ? 2 : 1;

	if (cols >= 0 && w + cw > cols)
	  {
	    break;
	  }
	i += nb;
	w += cw;
      }
    }
  if (out_w != NULL)
    {
      *out_w = w;
    }
  return i;
}

/* when only the display width is needed */
static int
volmap_disp_w (const char *t)
{
  int w;

  (void) volmap_disp_clip (t, -1, &w);
  return w;
}

/* Join a left and a right chunk into one content row, dropping the right one
   if space runs short.  Borders are drawn by the caller's loop, so only the
   content is built here - callers should never count columns themselves. */
static const char *
volmap_ov_pair (char *buf, size_t bufsz, int inner, const char *lhs, const char *rhs)
{
  int lw = volmap_disp_w (lhs);
  int rw = volmap_disp_w (rhs);
  int pad = inner - lw - rw;
  int n;

  if (rhs == NULL || rhs[0] == '\0' || pad < 1)
    {
      snprintf (buf, bufsz, "%s", lhs != NULL ? lhs : "");
      return buf;
    }
  n = snprintf (buf, bufsz, "%s", lhs != NULL ? lhs : "");
  for (; pad > 0 && n < (int) bufsz - 2; pad--)
    {
      buf[n++] = ' ';
    }
  buf[n] = '\0';
  snprintf (buf + n, bufsz - (size_t) n, "%s", rhs);
  return buf;
}

static void
volmap_overlay_clear (int x0, int y, int w, int rows)
{
  int r;

  for (r = 0; r < rows; r++)
    {
      printf ("\033[0m\033[%d;%dH%*s", y + r, x0, w, "");
    }
}

static void
volmap_overlay_box2 (int x0, int y, int w, const char *title, int hot, const char *rtitle)
{
  int used = 1, tl;
  int rl = (rtitle != NULL && rtitle[0] != '\0') ? (int) strlen (rtitle) : 0;
  const char *B = "\033[38;5;240m";
  char tbuf[192];
  int tmax = w - 6;		/* border and decoration take 6 columns */

  /* Truncate a title longer than the box.  Printed as-is it runs past the border
     on a narrow box and covers the map.  Cut by display width, since box-drawing
     characters and Hangul may be mixed in. */
  if (tmax < 1)
    {
      tmax = 1;
    }
  snprintf (tbuf, sizeof (tbuf), "%s", title);
  tl = volmap_disp_clip (tbuf, tmax, NULL);
  tbuf[tl] = '\0';
  tl = volmap_disp_w (tbuf);
  printf ("\033[0m\033[%d;%dH%s%s%s%s %s %s%s", y, x0, B, OVG ("\xe2\x95\xad", "+"),
	  OVG ("\xe2\x94\xa4", "["), hot ? "\033[1;36m" : "\033[38;5;250m", tbuf, B, OVG ("\xe2\x94\x9c", "]"));
  used += 4 + tl;
  /* Right-hand label (e.g. 64B/cell).  Placing it on the border row saves a
     content row and keeps the layout steady when the value changes.  Omitted
     silently when there is no room. */
  /* 'used' is the columns already consumed, w-1 the column before the closing
     corner.  The inset must still fit when it ends exactly there
     (used + rl + 4 == w - 1), hence <= rather than <. */
  if (rl > 0 && used + rl + 4 <= w - 1)
    {
      int dash = w - 1 - used - (rl + 4);

      for (; dash > 0; dash--, used++)
	{
	  printf ("%s", OVG ("\xe2\x94\x80", "-"));
	}
      printf ("%s\033[38;5;245m %s %s%s", OVG ("\xe2\x94\xa4", "["), rtitle, B,
	      OVG ("\xe2\x94\x9c", "]"));
      used += rl + 4;
    }
  for (; used < w - 1; used++)
    {
      printf ("%s", OVG ("\xe2\x94\x80", "-"));
    }
  printf ("%s", OVG ("\xe2\x95\xae", "+"));
}

static void
volmap_overlay_box (int x0, int y, int w, const char *title, int hot)
{
  volmap_overlay_box2 (x0, y, w, title, hot, NULL);
}

static void
volmap_overlay_boxend (int x0, int y, int w)
{
  int i;

  printf ("\033[0m\033[%d;%dH\033[38;5;240m%s", y, x0, OVG ("\xe2\x95\xb0", "+"));
  for (i = 0; i < w - 2; i++)
    {
      printf ("%s", OVG ("\xe2\x94\x80", "-"));
    }
  printf ("%s\033[0m", OVG ("\xe2\x95\xaf", "+"));
  /* flush at a glyph-complete boundary: the kernel splits big writes at
   * arbitrary bytes and some ssh clients cannot reassemble UTF-8 across
   * packets (tmux does, which is why it never breaks there) */
  fflush (stdout);
}

/* Fill one grid with all the sectors the cell covers, laid end to end.
   Cell order is page order, so row boundaries line up with sector boundaries
   (at width 64 one row is one sector).  File boundaries show as a tone flip,
   the same vocabulary the map uses.  Returns the number of cells filled. */
static int
volmap_panel_fill_sector_span (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_PANEL * p,
			       long base_pg, long psel_pg, int tone0, long *sdel, long *sdead,
			       int *psel_cell_out)
{
  int slots = p->w * p->rows;
  int cell;
  int prev_owner = -2, tone = tone0;
  long cur_sect = -1;
  char *sectbuf = volmap_scratch (ctx, VOLMAP_SECT_NPAGES * vol->iopagesize);
  ssize_t got = 0;

  *psel_cell_out = -1;
  memset (p->glyph, ' ', slots);
  memset (p->color, 0, slots);
  memset (p->aux, 0, slots);
  if (p->rbg != NULL)
    {
      memset (p->rbg, 0, slots);
    }
  for (cell = 0; cell < slots; cell++)
    {
      long pg = base_pg + cell;
      long sect;
      int owner, i;
      const char *page = NULL;
      bool readable = false;

      if (pg < 0 || pg >= (long) vol->nsect_total * VOLMAP_SECT_NPAGES)
	{
	  break;
	}
      sect = pg / VOLMAP_SECT_NPAGES;
      i = (int) (pg % VOLMAP_SECT_NPAGES);
      if (sect != cur_sect)
	{
	  /* on a sector change read 1MB at once, cheaper than 64 scattered header reads */
	  got = 0;
	  if (sectbuf != NULL && sect < vol->nsect_total && vol->stab[sect])
	    {
	      got = pread (volmap_vol_fd (vol), sectbuf,
			   (size_t) VOLMAP_SECT_NPAGES * vol->iopagesize,
			   (off_t) sect * VOLMAP_SECT_NPAGES * vol->iopagesize);
	    }
	  cur_sect = sect;
	}
      owner = (sect < vol->nsect_total) ? vol->owner[sect] : -1;
      if (prev_owner != -2 && owner != prev_owner)
	{
	  tone ^= 1;		/* flip the tone at a file boundary to mark it by colour */
	}
      prev_owner = owner;
      if (sectbuf != NULL && (ssize_t) ((size_t) (i + 1) * vol->iopagesize) <= got)
	{
	  page = sectbuf + (size_t) i * vol->iopagesize;
	  readable = true;
	}
      if (pg == psel_pg)
	{
	  *psel_cell_out = cell;
	}
      volmap_panel_fill_one_page (ctx, vol, p, sect, pg, cell, page, readable, tone, sdel, sdead);
      if (p->sel_hi > p->sel_lo && (pg < p->sel_lo || pg > p->sel_hi))
	{
	  /* Pages outside the map cell: the whole sector is still shown, but dimmed to
	     grey and excluded from the cursor and clicks.  Nothing is cut, so which
	     part of the sector is selected stays visible in context. */
	  p->color[cell] = VOLMAP_PCOL_DIM;	/* grey, handled specially by print_row */
	}
    }
  return cell;
}

/* Horizontal overlay placement, shared by drill-down and file view.
     (1) Screen >= 300 columns and >= 25 columns of margin right of the map
         border (and the box actually fits): place it outside, to the right.
     (2) Otherwise: inside the map outline, opposite the cursor, inset OV_INSET
         columns from the border.
   The width itself comes from volmap_ov_pick_w() (never over 1/3 of the outline).
   The map stops growing at 240 columns, so (1) usually holds past 300. */
static int
volmap_overlay_place_x (int scr_cols, int map_right, int w, int left_side, int map_rows, int scr_rows)
{
  const int OV_INSET = 3;	/* gap in columns between the map border and the overlay */
  int map_lo = 2;		/* first map cell column (after the border and space) */
  int map_hi = (map_right > 0) ? map_right - 2 : scr_cols - 2;	/* last map cell column */
  int x0;

  /* Placement.
       Default: a window floating inside the map, inset OV_INSET from the border,
                on the side opposite the cursor - the map should visibly surround it.
       Exception: place it outside to the right only when it fits there without
                shrinking the map, and one of these holds:
                  (1) map rows <= half the screen height (little to draw, bottom free)
                  (2) screen width >= 300 (wide enough that floating is pointless)
     The map is never shrunk to make room. */
  if (map_right > 0 && (map_rows * 2 <= scr_rows || scr_cols >= VOLMAP_OV_RIGHT_MIN_COLS)
      && scr_cols - map_right >= w + 2)
    {
      return map_right + 2;	/* outside to the right: covers no part of the map */
    }
  x0 = left_side ? (map_lo + OV_INSET) : (map_hi - w - OV_INSET + 1);
  if (x0 < map_lo + 1)
    {
      x0 = map_lo + 1;		/* if the map is narrower than the overlay, give up the inset */
    }
  if (x0 + w > scr_cols)
    {
      x0 = scr_cols - w;
    }
  if (x0 < 1)
    {
      x0 = 1;
    }
  return x0;
}

/* Drill-down overlay.
   One function per box; the shared frame drawing lives in volmap_ov_row and the
   parsing in volmap_page_slotdir_probe.  Each box takes (x0, y, w), draws its
   own rows and returns the next y. */

/* one box content row: left border + content padded to w-4 + right border; callers never count columns */
static void
volmap_ov_row (int x0, int y, int w, const char *body)
{
  const char *B = "\033[38;5;240m";

  printf ("\033[0m\033[%d;%dH%s%s\033[0m ", y, x0, B, OVG ("\xe2\x94\x82", "|"));
  volmap_put_padded (body != NULL ? body : "", w - 4);
  printf ("\033[0m %s%s", B, OVG ("\xe2\x94\x82", "|"));
}

/* left and right borders when the panel row function prints the content itself (grid rows) */
static void
volmap_ov_row_open (int x0, int y)
{
  printf ("\033[0m\033[%d;%dH\033[38;5;240m%s\033[0m ", y, x0, OVG ("\xe2\x94\x82", "|"));
}

static void
volmap_ov_row_close (int pad)
{
  /* pad keeps the right border at the box width when the grid is narrower than
     the box content (e.g. a 32-cell sector grid in a 40-column box). */
  printf ("\033[0m%*s \033[38;5;240m%s", (pad > 0) ? pad : 0, "", OVG ("\xe2\x94\x82", "|"));
}

/* Slot directory probe: does this page have slots, how many, is it corrupt.
   Kept separate from drawing so the FTAB / overflow misclassification rules can
   be verified on their own. */
typedef struct volmap_slotdir VOLMAP_SLOTDIR;
struct volmap_slotdir
{
  int alloc;			/* is the page allocated */
  int slotted;			/* is this a kind that carries a slot directory (passed the checks) */
  int ptype;			/* page kind (FILEIO_PAGE_RESERVED.ptype); explains why there are no slots */
  int nsl;			/* slot count (0 means none or unreadable) */
  int corrupt_nsl;		/* non-zero: num_slots of a genuine corruption candidate */
  int ovf;			/* 1 = generic overflow chunk (overflow_file.c format) */
  int ovf_next_pageid, ovf_next_volid, ovf_len;
};

static void
volmap_page_slotdir_probe (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long pg, const char *iop, VOLMAP_SLOTDIR * out)
{
  const FILEIO_PAGE_RESERVED *pr0 = (const FILEIO_PAGE_RESERVED *) iop;

  memset (out, 0, sizeof (*out));
  out->ovf_next_pageid = out->ovf_next_volid = out->ovf_len = -1;
  out->alloc = (pg >= 0 && ((vol->pagebm[pg / VOLMAP_SECT_NPAGES] >> (pg % VOLMAP_SECT_NPAGES)) & 1));
  if (!out->alloc || iop == NULL)
    {
      return;			/* an unallocated page is uninitialised bytes and is not interpreted */
    }
  /* Only slotted page kinds (HEAP/BTREE/OVERFLOW) carry a slot directory.
     FTAB, VOLHEADER and CATALOG have their own formats, so reading them as a
     SPAGE_HEADER yields arbitrary numbers. */
  out->ptype = (int) pr0->ptype;
  out->slotted = (pr0->ptype == PAGE_HEAP || pr0->ptype == PAGE_BTREE || pr0->ptype == PAGE_OVERFLOW);
  if (pr0->ptype == PAGE_OVERFLOW)
    {
      /* PAGE_OVERFLOW covers two formats: the overflow_file.c family (large heap
	 rows, long keys) is a raw cast with no slots, while B-tree OID chains are
	 slotted pages.  ptype cannot tell them apart, so the owning file type
	 decides. */
      long psec0 = pg / VOLMAP_SECT_NPAGES;
      int ow0 = (psec0 < vol->nsect_total) ? vol->owner[psec0] : -1;

      if (ow0 >= 0 && ow0 < ctx->nfiles
	  && (ctx->files[ow0].ftype == FILE_MULTIPAGE_OBJECT_HEAP
	      || ctx->files[ow0].ftype == FILE_BTREE_OVERFLOW_KEY))
	{
	  const char *ud = iop + prv_user_offset ();

	  /* struct overflow_first_part { VPID next_vpid; int length; ... } - next_vpid
	     occupies the leading 8 bytes in both the first and the rest parts */
	  out->slotted = 0;
	  out->ovf = 1;
	  out->ovf_next_pageid = ((const int *) ud)[0];
	  out->ovf_next_volid = ((const short *) (ud + 4))[0];
	  out->ovf_len = ((const int *) ud)[2];
	  return;
	}
    }
  if (!out->slotted)
    {
      return;
    }
  {
    const SPAGE_HEADER *sp = (const SPAGE_HEADER *) (iop + prv_user_offset ());
    int physmax = vol->user_size / (int) sizeof (SPAGE_SLOT);
    /* report corrupt? only when certain: check every invariant a slotted page must satisfy */
    int ok = (sp->num_slots >= 0 && sp->num_slots <= physmax
	      && sp->num_records >= 0 && sp->num_records <= sp->num_slots
	      && sp->total_free >= 0 && sp->total_free <= vol->user_size
	      && sp->cont_free >= 0 && sp->cont_free <= sp->total_free
	      && sp->offset_to_free_area >= 0 && sp->offset_to_free_area <= vol->user_size
	      && spage_is_valid_anchor (sp->anchor_type));

    if (ok)
      {
	out->nsl = sp->num_slots;
      }
    else if (sp->num_slots >= 0 && sp->num_slots <= physmax)
      {
	out->corrupt_nsl = sp->num_slots;	/* plausible count but the rest is broken: corruption candidate */
	out->nsl = sp->num_slots;
      }
    else
      {
	out->slotted = 0;	/* the count itself is implausible: probably not a slotted page, so do not assert */
      }
  }
}

/* Box 1: sector grid. Returns the next y */
static int
volmap_ov_box_sector (VOLMAP_CTX * ctx, VOLMAP_PANEL * pa, int x0, int y, int w,
		      long sect, long sect_del, long sect_dead, int hot, long sect_lo, long sect_hi,
		      int iopagesize, VOLMAP_OV_GEOM * g)
{
  char t1[64], rng[56] = "";	/* " (%ld/%ld in cell)" with both counts at full width */
  int r;

  /* One map cell can cover several sectors (e.g. cell = 192 pages -> 3 sectors).
     This box shows one of them, so the title carries the cell's range and which
     one is current; [ and ] move between them.  For a cell of one sector or less
     the range is redundant and omitted. */
  if (sect_hi > sect_lo)
    {
      /* On a narrow box (one set = 20 columns) the range gets cut to a fragment like
	 "(1", so a short form is used instead, and dropped entirely if even that
	 does not fit - better than clipping the title. */
      if (w - 6 >= 28)
	{
	  snprintf (rng, sizeof (rng), " (%ld/%ld in cell)", sect - sect_lo + 1, sect_hi - sect_lo + 1);
	}
      else if (w - 6 >= 18)
	{
	  snprintf (rng, sizeof (rng), " %ld/%ld", sect - sect_lo + 1, sect_hi - sect_lo + 1);
	}
    }
  if (ctx->deep && (sect_del > 0 || sect_dead > 0))
    {
      snprintf (t1, sizeof (t1), "sector %ld%s  del %ld vac %ld", sect, rng, sect_del, sect_dead);
    }
  else
    {
      snprintf (t1, sizeof (t1), "sector %ld%s", sect, rng);
    }
  volmap_overlay_clear (x0, y, w, pa->rows + 2);
  {
    /* Cell size as a right-hand title inset, same contract as the page box's
       "64B/cell".  A sector grid cell is one page, hence 16KB. */
    char sc1[16];

    snprintf (sc1, sizeof (sc1), "%dKB/pg", iopagesize / 1024);
    volmap_overlay_box2 (x0, y++, w, t1, hot, sc1);
  }
  g->b1y = y;
  for (r = 0; r < pa->rows; r++, y++)	/* only the allocated rows (1..4), never more */
    {
      volmap_ov_row_open (x0, y);
      if (r == pa->rows - 1 && volmap_ov_sect_more && pa->glyph != NULL)
	{
	  int last = (r + 1) * pa->w - 1;	/* the last cell is reserved for the scroll marker */

	  if (last >= 0 && last < pa->w * pa->rows)
	    {
	      pa->glyph[last] = '+';
	      pa->color[last] = 0;
	    }
	}
      volmap_panel_print_row (ctx, pa, r);
      if (r == pa->rows - 1 && volmap_ov_sect_more)
	{
	  /* Only when the 64 cells do not all fit: '+' at the end of the last row, the
	     same contract as the page and slots boxes.  Clicking it, or walking off
	     the edge with the arrows, moves to the next screenful. */
	  /* When the grid fills the box there is no column left to append '+'.  Backing
	     the cursor up (\033[1D) lands after print_row's reset sequence, so the
	     last panel cell is replaced with '+' and drawn by print_row instead. */
	  volmap_ov_row_close (w - 4 - pa->w);
	}
      else
	{
	  volmap_ov_row_close (w - 4 - pa->w);
	}
    }
  volmap_overlay_boxend (x0, y++, w);
  return y;
}

/* Box 2: page byte distribution. Returns the next y */
static int
volmap_ov_box_page (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_PANEL * pb, int x0, int y, int w,
		    long pg, int hot, VOLMAP_OV_GEOM * g)
{
  char t1[96], sc[24];
  int r;

  /* On a narrow box a full title like "page 416064 (1/64)" overruns and pushes
     the right-hand scale (64B/cell) out.  Shorten the title by width - a shorter
     form beats a clipped one. */
  if (ctx->residency && vol->respg != NULL && pg >= 0 && w - 6 >= 34)
    {
      /* The "res N/M" suffix lengthens the title by 8 columns and would push out the
	 right-hand scale.  The scale is the reference for the reading and takes
	 priority, so the suffix is added only when both fit (content width 34+). */
      snprintf (t1, sizeof (t1), "page %ld (%d/64) res %d/%d", pg, (int) (pg % VOLMAP_SECT_NPAGES) + 1,
		(int) vol->respg[pg], vol->iopagesize / 4096);
    }
  else if (w - 6 >= 20)
    {
      snprintf (t1, sizeof (t1), "page %ld (%d/64)", pg, (int) (pg % VOLMAP_SECT_NPAGES) + 1);
    }
  else
    {
      /* One-set width (20 columns): a full page number leaves no room for the scale,
	 which would then disappear silently.  Show only the index within the sector
	 - the info box already carries the absolute number. */
      snprintf (t1, sizeof (t1), "pg .%d", (int) (pg % VOLMAP_SECT_NPAGES) + 1);
    }
  if (ctx->bufmap && ctx->bufmap_loaded && pg >= 0)
    {
      /* buffer pool state and LSA chain: in-memory page LSA (snapshot) vs on-disk header LSA (8B pread) */
      const VOLMAP_BUFREC *br = volmap_bufmap_find (ctx, vol->volid, (PAGEID) pg);
      size_t tl = strlen (t1);

      if (br == NULL)
	{
	  snprintf (t1 + tl, sizeof (t1) - tl, " buf:-");
	}
      else
	{
	  UINT64 disk = VOLMAP_LSA_NULL;
	  const char *rel = "";

	  if (br->hdr_ok && br->page_lsa != VOLMAP_LSA_NULL
	      && pread (volmap_vol_fd (vol), &disk, 8, (off_t) pg * vol->iopagesize) == 8)
	    {
	      int c = volmap_lsa_cmp (br->page_lsa, disk);

	      /* both bits off yet the volume is behind: the page waits in the double write
	         buffer for its volume write - only the LSA comparison shows it */
	      rel = (c > 0) ? ((br->flags & VOLMAP_BUFREC_UNFLUSHED) ? " unflushed" : " unflushed (in DWB?)")
		: (c < 0) ? " disk-newer!" : ((br->flags & VOLMAP_BUFREC_UNFLUSHED) ? " flushed" : "");
	    }
	  snprintf (t1 + tl, sizeof (t1) - tl, " buf:%s%s",
		    ((br->flags & VOLMAP_BUFREC_UNFLUSHED) == VOLMAP_BUFREC_UNFLUSHED) ? "DIRTY+FLUSHING"
		    : (br->flags & VOLMAP_BUFREC_DIRTY) ? "DIRTY" : (br->flags & VOLMAP_BUFREC_FLUSHING) ? "FLUSHING" : "clean", rel);
	}
    }
  /* The grid uses rows-2 (the last 2 rows are the legend).  Counting -3 loses a
     row and shows '+' even when all 256 cells are visible. */
  volmap_ov_page_scroll_clamp (((pb->rows > 2) ? pb->rows - 2 : 1) * pb->w);
  /* With the scroll marker attached "64B/cell +" is 10 columns and overflows a
     fixed 32-column box by one, so the unit suffix is dropped to "64B +",
     keeping both the number and the marker. */
  if (volmap_ov_page_more)
    {
      snprintf (sc, sizeof (sc), "%dB +", pb->bytes_per_cell);
    }
  else
    {
      /* The scale goes in as a right-hand title inset, and the fit must be computed
	 against the actual title length, not just the width: with a long page
	 number the full form otherwise misses by one column and is dropped. */
      int used = 1 + 4 + (int) strlen (t1);

      if (used + (int) strlen ("B/cell") + 6 + 4 <= w - 1)
	{
	  snprintf (sc, sizeof (sc), "%dB/cell", pb->bytes_per_cell);
	}
      else
	{
	  snprintf (sc, sizeof (sc), "%dB", pb->bytes_per_cell);	/* drop the unit suffix when there is no room */
	}
    }
  volmap_overlay_clear (x0, y, w, pb->rows + 2);
  volmap_overlay_box2 (x0, y++, w, t1, hot, sc);	/* the scale goes right of the title, saving a content row */
  g->b2y = y;
  g->b2n = pb->rows;
  for (r = 0; r < pb->rows; r++, y++)
    {
      if (r == pb->rows - 2)
	{
	  /* legend row 1: five glyphs on one line (31 columns fits the 40-column content) */
	  char leg[320];

	  snprintf (leg, sizeof (leg),
		    "\033[35m\xe2\xa0\x89 hdr \033[%dm\xe2\xa0\xbf rec \033[38;5;242m\xe2\xa0\xa4 free "
		    "\033[38;5;245m\xe2\xa0\x9b dir \033[38;5;131m\xe2\xa0\xb6 frag\033[0m",
		    (pb->rec_col > 0) ? pb->rec_col : 37);
	  volmap_ov_row (x0, y, w, leg);
	}
      else if (r == pb->rows - 1)
	{
	  /* legend row 2: slot statistics (del/vac) right-aligned, kept apart as they are not legend entries */
	  char st[96], both[220];

	  snprintf (st, sizeof (st), "%sdel%3d\033[0m \xc2\xb7 %svac%3d\033[0m",
		    pb->foot_val[0] > 0 ? "\033[43;30m" : "\033[38;5;245m", (int) pb->foot_val[0],
		    pb->foot_val[1] > 0 ? "\033[41;97m" : "\033[38;5;131m", (int) pb->foot_val[1]);
	  volmap_ov_row (x0, y, w, volmap_ov_pair (both, sizeof (both), w - 4, "", st));
	}
      else
	{
	  volmap_ov_row_open (x0, y);
	  volmap_panel_print_row (ctx, pb, r);
	  volmap_ov_row_close (w - 4 - pb->w);
	}
    }
  volmap_overlay_boxend (x0, y++, w);
  return y;
}

/* Box 3: slot directory. Returns the next y */
static int
volmap_ov_box_slots (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_PANEL * pa, int x0, int y, int w,
		     long pg, int ov_slot, int hot, int scr_rows, VOLMAP_OV_GEOM * g)
{
  static const char sc2[8] = { '?', 'A', 'H', 'N', 'R', 'B', 'D', 'd' };
  static const int scol2[8] = { 37, 32, 34, 36, 33, 35, 31, 31 };

  (void) pa;			/* the slots grid owns its geometry; kept for a uniform signature */
  char *iop = volmap_scratch (ctx, vol->iopagesize);
  VOLMAP_SLOTDIR sd;
  char t1[64];
  /* The slots grid takes its width from its own box.  Inheriting the sector
     panel's width made slots follow every sector width change, while mouse
     hit-testing used the box width - so drawing and clicks disagreed. */
  int rows3 = 3, base, r, i2, gw = volmap_ov_grid_w (w - 4);
  int leg2 = (w - 4 >= 62) ? 1 : 2;	/* legend rows (all nine kinds on one row when wide) */

  /* Slots grid rows: 3 by default, fewer on short screens to leave the info box
     its 5 rows (minimum 1).  A narrow box has a two-row legend, so one more row
     is given up there, otherwise the info box is pushed off screen. */
  {
    /* The legend row count follows the height available.  A narrow box can keep
       full labels on two rows, but on a short screen (80x25) that extra row
       pushes the info box out - there it falls back to one row of glyphs only. */
    int left = scr_rows - 4 - y - 2 - VOLMAP_OV_MIN_INFO - (leg2 - 1);

    if (rows3 > left)
      {
	rows3 = left;
      }
    if (rows3 < 1)
      {
	/* no grid row left means there is no height for a two-row legend, so fall back to one */
	if (leg2 > 1)
	  {
	    leg2 = 1;
	    left++;
	  }
	rows3 = (left >= 1) ? left : 1;
      }
  }
  if (iop != NULL && pg >= 0 && volmap_read_iopage (vol, (PAGEID) pg, iop))
    {
      volmap_page_slotdir_probe (ctx, vol, pg, iop, &sd);
    }
  else
    {
      memset (&sd, 0, sizeof (sd));
      sd.alloc = (pg >= 0 && ((vol->pagebm[pg / VOLMAP_SECT_NPAGES] >> (pg % VOLMAP_SECT_NPAGES)) & 1));
      iop = NULL;
    }
  /* the 3-row window follows the selection, so pages with hundreds of slots can be walked */
  base = (ov_slot / gw - 1) * gw;
  if (base < 0 || sd.nsl <= rows3 * gw)
    {
      base = 0;
    }
  if (base > 0)
    {
      snprintf (t1, sizeof (t1), "slots %d (from %d)", sd.nsl, base);
    }
  else if (sd.corrupt_nsl != 0)
    {
      snprintf (t1, sizeof (t1), "slots %d corrupt?", sd.corrupt_nsl);
    }
  else if (sd.ovf)
    {
      /* overflow chunk: show what the format holds (chain link, length) instead of slots */
      if (sd.ovf_next_pageid >= 0 && sd.ovf_next_volid >= 0)
	{
	  snprintf (t1, sizeof (t1), "overflow chunk  next %d|%d  len %d", sd.ovf_next_volid, sd.ovf_next_pageid, sd.ovf_len);
	}
      else
	{
	  snprintf (t1, sizeof (t1), "overflow chunk  next (end)  len %d", sd.ovf_len);
	}
    }
  else
    {
      if (sd.alloc && !sd.slotted)
	{
	  /* No slots is not a fault but a property of the page kind, so say which kind.
	     Temp volumes are mostly area (workspace) and qresult (sort output);
	     reporting both as a bare "n/a" gave no way to tell them apart. */
	  signed char pc;
	  const char *tag = volmap_ptype_tag (sd.ptype, &pc);

	  snprintf (t1, sizeof (t1), "slots n/a - %s page", tag);
	}
      else
	{
	  snprintf (t1, sizeof (t1), "slots %d%s", sd.nsl,
		    (!sd.alloc) ? " (unallocated)" : (sd.nsl == 0) ? " (empty)" : "");
	}
    }
  volmap_overlay_clear (x0, y, w, rows3 + 3);
  volmap_overlay_box (x0, y++, w, t1, hot);
  g->b3y = y;
  g->slot_base = base;
  g->nsl = sd.nsl;
  for (r = 0; r < rows3; r++, y++)
    {
      volmap_ov_row_open (x0, y);
      for (i2 = 0; i2 < gw; i2++)
	{
	  int sl = base + r * gw + i2;

	  if (r == rows3 - 1 && i2 == gw - 1 && sd.nsl > base + rows3 * gw)
	    {
	      printf ("\033[38;5;245m+");	/* more slots exist than the box shows */
	    }
	  else if (sl < sd.nsl && iop != NULL)
	    {
	      const SPAGE_SLOT *so = (const SPAGE_SLOT *) (iop + prv_user_offset () + vol->user_size - sizeof (SPAGE_SLOT)) - sl;
	      int tt = (so->record_type < 8) ? (int) so->record_type : 0;
	      int cur = (sl == ov_slot && hot);

	      if (cur)
		{
		  fputs (VOLMAP_CURSOR_SGR, stdout);
		}
	      if (so->offset_to_record == 0)
		{
		  printf ("\033[38;5;238m-");
		}
	      else
		{
		  printf ("\033[%dm%c", scol2[tt], sc2[tt]);
		}
	      if (cur)
		{
		  fputs ("\033[0m", stdout);
		}
	    }
	  else
	    {
	      printf ("\033[0m ");
	    }
	}
      volmap_ov_row_close (w - 4 - gw);
    }
  /* Slot legend, coloured like the grid (sc2/scol2), covering all nine kinds the
     grid can draw.  A narrow box cannot hold them on one row, so it splits over
     two - squeezing them onto one row would abbreviate labels past legibility. */
  {
    char leg[200];

    if (leg2 == 1 && w - 4 >= 62)
      {
	/* wide box: all nine kinds with labels on one row */
	snprintf (leg, sizeof (leg),
		  "\033[34mH\033[0m home \033[36mN\033[0m new \033[33mR\033[0m rel "
		  "\033[35mB\033[0m big \033[31mD\033[0m mk-del \033[31md\033[0m del-us "
		  "\033[32mA\033[0m asgn \033[38;5;238m-\033[0m free \033[37m?\033[0m unk");
	volmap_ov_row (x0, y++, w, leg);
      }
    else if (leg2 == 1)
      {
	/* Narrow and short (80x25): no height for two rows, and labels would be cut
	   mid-word, so only the nine glyphs are shown; the info box and help carry
	   the meanings. */
	snprintf (leg, sizeof (leg),
		  "\033[34mH\033[0m \033[36mN\033[0m \033[33mR\033[0m \033[35mB\033[0m "
		  "\033[31mD\033[0m \033[31md\033[0m \033[32mA\033[0m \033[38;5;238m-\033[0m "
		  "\033[37m?\033[0m\033[38;5;245m  slot kinds\033[0m");
	volmap_ov_row (x0, y++, w, leg);
      }
    else
      {
	/* narrow box (32 columns): 5 + 4 over two rows, keeping full labels */
	snprintf (leg, sizeof (leg),
		  "\033[34mH\033[0m home \033[36mN\033[0m new \033[33mR\033[0m rel "
		  "\033[35mB\033[0m big \033[32mA\033[0m asgn");
	volmap_ov_row (x0, y++, w, leg);
	snprintf (leg, sizeof (leg),
		  "\033[31mD\033[0m mk-del \033[31md\033[0m del-us "
		  "\033[38;5;238m-\033[0m freed \033[37m?\033[0m unk");
	volmap_ov_row (x0, y++, w, leg);
      }
  }
  volmap_overlay_boxend (x0, y++, w);
  return y;
}

/* Box 4: info - what the current selection is.  Returns the next y, or y
   unchanged when there is no room.  Shrinks to 4 rows (2 border + 2 content)
   rather than disappearing.  Overflow is marked with '+' on the last row, the
   same contract as the slots box. */
static int
volmap_ov_box_info (int x0, int y, int w, const char *info, int scr_rows)
{
  const char *p2 = info;
  int inf_rows, r2;

  if (info == NULL || info[0] == '\0' || y + 4 > scr_rows - 4)
    {
      return y;
    }
  inf_rows = scr_rows - 4 - y - 2;
  if (inf_rows > 4)
    {
      inf_rows = 4;
    }
  if (inf_rows < 1)
    {
      inf_rows = 1;
    }
  volmap_overlay_clear (x0, y, w, inf_rows + 2);
  volmap_overlay_box (x0, y++, w, "info", 0);
  for (r2 = 0; r2 < inf_rows; r2++, y++)
    {
      /* one row: up to an explicit newline, or as much as the width allows; width comes from disp_clip */
      char line[200];
      const char *nl = strchr (p2, '\n');
      size_t seg = (nl != NULL) ? (size_t) (nl - p2) : strlen (p2);
      int take;

      if (seg >= sizeof (line) - 4)
	{
	  seg = sizeof (line) - 4;
	}
      memcpy (line, p2, seg);
      line[seg] = '\0';
      take = volmap_disp_clip (line, w - 4, NULL);
      line[take] = '\0';
      p2 += take;
      if (nl != NULL && p2 == nl)
	{
	  p2++;			/* explicit break: OID line / detail line */
	}
      if (r2 == inf_rows - 1 && *p2 != '\0' && take < (int) sizeof (line) - 2)
	{
	  line[take++] = ' ';
	  line[take++] = '+';
	  line[take] = '\0';
	}
      volmap_ov_row (x0, y, w, line);
    }
  volmap_overlay_boxend (x0, y, w);
  return y + 1;
}

/* container: compute placement, draw the four boxes in order, return the geometry */
static void
volmap_overlay_draw (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_PANEL * pa, VOLMAP_PANEL * pb,
		     long sect, long pg, int ov_slot, int ov_focus, int scr_cols, long sect_del, long sect_dead,
		     const char *info, int scr_rows, int left_side, int map_right, long cell_p0, long cell_npg,
		     int map_rows)
{
  VOLMAP_OV_GEOM *g = &volmap_ov_geom;
  int w = pb->w + 4;		/* box width follows the page grid width */
  /* Sector range covered by the map cell.  When the scale is not a multiple of
     64 the cell straddles neighbouring sectors; deriving the range from the
     sectors of the first and last page covers that case naturally. */
  long sect_lo = (cell_p0 >= 0) ? cell_p0 / VOLMAP_SECT_NPAGES : sect;
  long sect_hi = (cell_p0 >= 0 && cell_npg > 0) ? (cell_p0 + cell_npg - 1) / VOLMAP_SECT_NPAGES : sect;
  int x0 = volmap_overlay_place_x (scr_cols, map_right, w, left_side, map_rows, scr_rows);
  /* start row: 7 on a tall screen (3 map rows visible), 4 on a short one, to fit all four boxes */
  int y = (scr_rows >= 7 + VOLMAP_OV_MIN_TOTAL + 4) ? 7 : 4;

  memset (g, 0, sizeof (*g));
  g->x0 = x0;
  g->y0 = y;
  y = volmap_ov_box_sector (ctx, pa, x0, y, w, sect, sect_del, sect_dead, ov_focus == 1, sect_lo, sect_hi,
			    vol->iopagesize, g);
  y = volmap_ov_box_page (ctx, vol, pb, x0, y, w, pg, ov_focus == 2, g);
  y = volmap_ov_box_slots (ctx, vol, pa, x0, y, w, pg, ov_slot, ov_focus == 3, scr_rows, g);
  g->y1 = y - 1;		/* stop here when there is no info box */
  y = volmap_ov_box_info (x0, y, w, info, scr_rows);
  if (y - 1 > g->y1)
    {
      g->y1 = y - 1;
    }
}

/* Help text: one scrolling document, not a set of pages.  When it is longer
   than the screen the arrows and PgUp/PgDn scroll it.  One array entry = one
   line. */
static const char *volmap_help_lines[] = {
  "\x1b[1m volmap \xed\x99\x94\xeb\xa9\xb4 \xed\x95\xb4\xec\x84\x9d \xea\xb0\x80\xec\x9d\xb4\xeb\x93\x9c\x1b[0m",
  "",
  "\x1b[1m [\xea\xb3\x84\xec\xb8\xb5]\x1b[0m  \xeb\xb3\xbc\xeb\xa5\xa8 \xe2\x94\x94 \x1b[1m\xec\x84\xb9\xed\x84\xb0\x1b[0m 64p=1MB \xe2\x94\x94 \x1b[1m\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\x1b[0m 16KB \xe2\x94\x94 \x1b[1m\xec\x8a\xac\xeb\xa1\xaf\x1b[0m \xe2\x94\x94 \xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c \xe2\x94\x94 \xec\xbb\xac\xeb\x9f\xbc",
  "   \xec\x84\xb9\xed\x84\xb0 = \xed\x8c\x8c\xec\x9d\xbc \xec\x86\x8c\xec\x9c\xa0\xea\xb6\x8c\xc2\xb7\xeb\x8b\xa8\xed\x8e\xb8\xed\x99\x94 \xeb\x8b\xa8\xec\x9c\x84    \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 = I/O\xc2\xb7\xeb\xb2\x84\xed\x8d\xbc \xeb\x8b\xa8\xec\x9c\x84",
  "   \x1b[2m\xe2\x80\xbb \xec\x8a\xac\xeb\xa1\xaf\xec\x9d\x80 heap\xc2\xb7" "btree\xc2\xb7overflow \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\xeb\xa7\x8c \xea\xb0\x80\xec\xa7\x84\xeb\x8b\xa4 (\xea\xb7\xb8 \xec\x99\xb8\xeb\x8a\x94 (n/a))\x1b[0m",
  "",
  "\x1b[1m [\xec\xa7\x80\xeb\x8f\x84]\x1b[0m  1\xec\xb9\xb8 = N\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80(2\xed\x96\x89 cell \xed\x91\x9c\xea\xb8\xb0), \xec\x83\x89 = \xea\xb7\xb8 \xea\xb5\xac\xea\xb0\x84\xec\x9d\x84 \xec\x86\x8c\xec\x9c\xa0\xed\x95\x9c \xed\x8c\x8c\xec\x9d\xbc\xec\x9d\x98 \xec\xa2\x85\xeb\xa5\x98",
  "   \xec\x83\x89   \x1b[34m\xe2\xa0\xbf\x1b[0m \xeb\x8d\xb0\xec\x9d\xb4\xed\x84\xb0 \xed\x9e\x99   \x1b[32m\xe2\xa0\xbf\x1b[0m \xec\x9d\xb8\xeb\x8d\xb1\xec\x8a\xa4   \x1b[31m\xe2\xa0\xbf\x1b[0m \xec\xb9\xb4\xed\x83\x88\xeb\xa1\x9c\xea\xb7\xb8/\xec\x8b\x9c\xec\x8a\xa4\xed\x85\x9c   temp = \xed\x8c\x8c\xec\x9d\xbc\xeb\xb3\x84 \xec\x83\x89",
  "   \xec\xb1\x84\xec\x9b\x80 \xe2\xa0\xbf\xe2\xa0\xbe\xe2\xa0\xb6\xe2\xa0\xb4\xe2\xa0\xa4 = \xec\x85\x80 \xec\x95\x88 \xed\x95\xa0\xeb\x8b\xb9\xeb\xa5\xa0 100/80/60/40/20%%",
  "   \xed\x86\xa4   \xea\xb0\x99\xec\x9d\x80 \xec\x83\x89 \xeb\x91\x90 \xed\x86\xa4 \xea\xb5\x90\xeb\x8c\x80 = \xed\x8c\x8c\xec\x9d\xbc \xea\xb2\xbd\xea\xb3\x84 \xe2\x80\x94 \x1b[1m\xec\x9e\xa6\xec\x9d\x80 \xeb\xb0\x98\xec\xa0\x84 = \xec\xa1\xb0\xea\xb0\x81\xed\x99\x94\x1b[0m",
  "   \xec\x83\x81\xed\x83\x9c \x1b[38;5;208m\xe2\xa0\x82\x1b[0m \xec\x98\x88\xec\x95\xbd-\xeb\xb9\x88  \x1b[38;5;242m\xe2\xa0\x82\x1b[0m \xeb\xaf\xb8\xec\x98\x88\xec\x95\xbd  \x1b[38;5;238m#\x1b[0m \xeb\xa9\x94\xed\x83\x80  \x1b[38;5;243m?\x1b[0m \xec\x86\x8c\xec\x9c\xa0\xec\x9e\x90 \xeb\xaf\xb8\xec\x83\x81  \x1b[31mE\x1b[0m TDE",
  "   \xeb\xb0\xb0\xea\xb2\xbd \xed\x9a\x8c\xec\x83\x89(-m)=OS \xec\xba\x90\xec\x8b\x9c \xec\x83\x81\xec\xa3\xbc   \x1b[48;5;24m \xed\x95\x98\xeb\x8a\x98 \x1b[49m=\xed\x95\x98\xec\x9d\xb4\xec\x9b\x8c\xed\x84\xb0\xeb\xa7\x88\xed\x81\xac",
  "        \x1b[48;5;23m \xec\xb2\xad\xeb\xa1\x9d \x1b[49m/\x1b[48;5;53m \xec\x9e\x90\xec\xa3\xbc \x1b[49m=(b) \xeb\xb2\x84\xed\x8d\xbc\xed\x92\x80 \xec\xa0\x81\xec\x9e\xac / \xea\xb7\xb8 \xec\xa4\x91 \xeb\xaf\xb8\xeb\xb0\x98\xec\x98\x81(dirty\xc2\xb7" "flushing)",
  "",
  "\x1b[1m [\xed\x82\xa4]\x1b[0m  space\xc2\xb7" "enter \xeb\x93\x9c\xeb\xa6\xb4\xeb\x8b\xa4\xec\x9a\xb4   bksp \xec\x9c\x84   tab \xec\x97\xb4\xea\xb8\xb0/\xeb\x8b\xab\xea\xb8\xb0   1/2/3 \xeb\xb0\x95\xec\x8a\xa4 \xec\xa7\x81\xed\x96\x89",
  "   < > \xeb\xb3\xbc\xeb\xa5\xa8   [ ] \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80   r \xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8   a \xec\x9e\x90\xeb\x8f\x99   m \xec\xba\x90\xec\x8b\x9c   l \xed\x95\x9c/\xec\x98\x81   q \xec\xa2\x85\xeb\xa3\x8c",
  "   f \xed\x8c\x8c\xec\x9d\xbc \xeb\xb7\xb0(\xea\xb3\xa0\xeb\xa5\xb8 \xed\x8c\x8c\xec\x9d\xbc\xeb\xa7\x8c \x1b[48;5;17m\xeb\x82\xa8\xec\x83\x89\x1b[49m)      p \xeb\x85\xbc\xeb\xa6\xac \xec\xb2\xb4\xec\x9d\xb8 \xeb\xb7\xb0(\xec\xa0\x90\xed\x94\x84 \x1b[48;5;52m\xeb\xb9\xa8\xea\xb0\x95\x1b[49m)",
  "   g ASCII \xed\x85\x8c\xeb\x91\x90\xeb\xa6\xac(\xeb\xb0\x95\xec\x8a\xa4 \xeb\xac\xb8\xec\x9e\x90 \xea\xb9\xa8\xec\xa7\x88 \xeb\x95\x8c)   \xeb\xa7\x88\xec\x9a\xb0\xec\x8a\xa4 \xed\x81\xb4\xeb\xa6\xad=\xec\x9d\xb4\xeb\x8f\x99, \xeb\x8d\x94\xeb\xb8\x94\xed\x81\xb4\xeb\xa6\xad=\xeb\x93\x9c\xeb\xa6\xb4\xeb\x8b\xa4\xec\x9a\xb4",
  "",
  "\x1b[1m [\xec\x9a\xa9\xec\x96\xb4]\x1b[0m  \xec\x84\xb9\xed\x84\xb0=64\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xeb\xac\xb6\xec\x9d\x8c(\xeb\x94\x94\xec\x8a\xa4\xed\x81\xac \xec\x84\xb9\xed\x84\xb0 \xec\x95\x84\xeb\x8b\x98)   heap=\xed\x85\x8c\xec\x9d\xb4\xeb\xb8\x94 \xed\x8c\x8c\xec\x9d\xbc(\xeb\xa9\x94\xeb\xaa\xa8\xeb\xa6\xac \xec\x95\x84\xeb\x8b\x98)",
  "   idle=\xec\x98\x88\xec\x95\xbd\xeb\xa7\x8c \xed\x95\x98\xea\xb3\xa0 \xec\x95\x88 \xec\x93\xb4 \xea\xb3\xb5\xea\xb0\x84   owner switches=\xed\x8c\x8c\xec\x9d\xbc \xea\xb2\xbd\xea\xb3\x84 \xeb\xb0\x94\xeb\x80\x90 \xed\x9a\x9f\xec\x88\x98(\xec\xa1\xb0\xea\xb0\x81\xed\x99\x94)",
  "   FTAB=\xed\x8c\x8c\xec\x9d\xbc\xec\x9d\x98 \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xeb\xaa\xa9\xeb\xa1\x9d\xed\x91\x9c   vhdr=\xeb\xb3\xbc\xeb\xa5\xa8 \xed\x91\x9c\xec\xa7\x80   dead/vac=vacuum \xeb\x8c\x80\xea\xb8\xb0",
  "",
  "",
  "\x1b[1m \xeb\x93\x9c\xeb\xa6\xb4\xeb\x8b\xa4\xec\x9a\xb4 \xec\x98\xa4\xeb\xb2\x84\xeb\xa0\x88\xec\x9d\xb4\x1b[0m",
  "",
  "   \xec\x9c\x84 \xeb\xb0\x95\xec\x8a\xa4\xec\x97\x90\xec\x84\x9c \xea\xb3\xa0\xeb\xa5\xb4\xeb\xa9\xb4 \xec\x95\x84\xeb\x9e\x98 \xeb\xb0\x95\xec\x8a\xa4\xeb\x93\xa4\xec\x9d\xb4 \xea\xb7\xb8 \xeb\x8c\x80\xec\x83\x81\xec\x9c\xbc\xeb\xa1\x9c \xec\xa6\x89\xec\x8b\x9c \xeb\xb0\x94\xeb\x80\x90\xeb\x8b\xa4",
  "",
  "\x1b[1m [1 sector]\x1b[0m  \xec\x84\xb9\xed\x84\xb0 \xed\x95\x98\xeb\x82\x98 = \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 64\xea\xb0\x9c,  \xed\x95\x9c \xec\xb9\xb8 = \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 1\xea\xb0\x9c",
  "   \x1b[38;5;238m#\x1b[0m FTAB \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80   \x1b[38;5;208m\xe2\xa0\x82\x1b[0m \xeb\xaf\xb8\xed\x95\xa0\xeb\x8b\xb9   \x1b[31mE\x1b[0m TDE   \x1b[1m\xec\xa3\xbc\xed\x99\xa9 \xeb\xb0\xb0\xea\xb2\xbd = \xec\x84\xa0\xed\x83\x9d\xed\x95\x9c \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\x1b[0m",
  "   \x1b[2m\xed\x9a\x8c\xec\x83\x89 \xec\xb9\xb8 = \xeb\xa7\xb5 \xec\x85\x80 \xeb\xb2\x94\xec\x9c\x84 \xeb\xb0\x96(\xec\x84\xa0\xed\x83\x9d \xeb\xb6\x88\xea\xb0\x80)   \xec\xa2\x8c\xec\x9a\xb0 \xeb\x81\x9d\xec\x9d\x84 \xeb\x84\x98\xec\x9c\xbc\xeb\xa9\xb4 \xec\x9d\xb4\xec\x9b\x83 \xec\x84\xb9\xed\x84\xb0\xeb\xa1\x9c \xec\x9d\xb4\xec\x96\xb4\xec\xa7\x84\xeb\x8b\xa4\x1b[0m",
  "",
  "\x1b[1m [2 page]\x1b[0m  \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 16KB \xec\xa0\x84\xec\xb2\xb4\xec\x9d\x98 \xeb\xb0\x94\xec\x9d\xb4\xed\x8a\xb8 \xec\xa7\x80\xeb\x8f\x84,  \xed\x95\x9c \xec\xb9\xb8 = N\xeb\xb0\x94\xec\x9d\xb4\xed\x8a\xb8(\xec\xa0\x9c\xeb\xaa\xa9 NB/cell)",
  "   \x1b[35m\xe2\xa0\x89\x1b[0m hdr \xed\x97\xa4\xeb\x8d\x94   \xe2\xa0\xbf rec \xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c(\xec\x83\x89=\xec\x86\x8c\xec\x9c\xa0 \xed\x8c\x8c\xec\x9d\xbc \xec\xa2\x85\xeb\xa5\x98)",
  "   \x1b[38;5;242m\xe2\xa0\xa4\x1b[0m free \xec\x97\xb0\xec\x86\x8d \xeb\xb9\x88\xec\xb9\xb8   \x1b[38;5;245m\xe2\xa0\x9b\x1b[0m dir \xec\x8a\xac\xeb\xa1\xaf \xeb\x94\x94\xeb\xa0\x89\xed\x84\xb0\xeb\xa6\xac   \x1b[38;5;131m\xe2\xa0\xb6\x1b[0m frag \xec\xa1\xb0\xea\xb0\x81\xeb\x82\x9c \xeb\xb9\x88\xec\xb9\xb8",
  "   \x1b[2m\xed\x91\xb8\xed\x84\xb0 del N = \xed\x95\xb4\xec\xa0\x9c\xeb\x90\x9c \xec\x8a\xac\xeb\xa1\xaf,  vac N = vacuum \xeb\x8c\x80\xea\xb8\xb0 (\xeb\xb9\xa8\xea\xb0\x95\xec\x9d\xb4\xec\x96\xb4\xeb\x8f\x84 \xea\xb3\xa0\xec\x9e\xa5 \xec\x95\x84\xeb\x8b\x98)\x1b[0m",
  "",
  "\x1b[1m [3 slots]\x1b[0m  \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xec\x95\x88 \xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c \xec\x9e\x90\xeb\xa6\xac \xe2\x80\x94 \x1b[2m\xec\x9e\x90\xec\x84\xb8\xed\x95\x9c \xeb\x9c\xbb\xec\x9d\x80 3/3\x1b[0m",
  "   \x1b[34mH\x1b[0m home \xeb\xb3\xb4\xed\x86\xb5   \x1b[36mN\x1b[0m new \xec\x98\xae\xea\xb2\xa8 \xec\x98\xa8 \xeb\xb3\xb8\xeb\xac\xb8   \x1b[33mR\x1b[0m rel \xec\x98\xae\xea\xb2\xa8 \xea\xb0\x84 \xed\x91\x9c\xec\xa7\x80   \x1b[35mB\x1b[0m big \xed\x81\xb0 \xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c",
  "   \x1b[31mD\x1b[0m mk-del \xec\x9e\xac\xec\x82\xac\xec\x9a\xa9 \xec\x95\x88 \xed\x95\xa8   \x1b[31md\x1b[0m del-us \xec\x9e\xac\xec\x82\xac\xec\x9a\xa9 \xec\x98\x88\xec\xa0\x95   \x1b[32mA\x1b[0m asgn \xec\x9e\x90\xeb\xa6\xac\xeb\xa7\x8c \xec\x9e\xa1\xec\x9d\x8c",
  "   \x1b[38;5;238m-\x1b[0m freed \xeb\xb9\x88 \xec\x8a\xac\xeb\xa1\xaf   \x1b[37m?\x1b[0m unknown \x1b[1m\xec\x86\x90\xec\x83\x81 \xed\x9b\x84\xeb\xb3\xb4 \xe2\x80\x94 \xea\xb0\x90\xec\xb6\x94\xec\xa7\x80 \xec\x95\x8a\xea\xb3\xa0 \xea\xb7\xb8\xeb\x8c\x80\xeb\xa1\x9c \xed\x91\x9c\xec\x8b\x9c\x1b[0m",
  "",
  "\x1b[1m [info]\x1b[0m  \xec\xa7\x80\xea\xb8\x88 \xec\x84\xa0\xed\x83\x9d\xed\x95\x9c \xea\xb2\x83\xec\x9d\x98 \xec\xa0\x95\xec\xb2\xb4 \xec\x9a\x94\xec\x95\xbd.  \xed\x95\x98\xeb\x8b\xa8 2\xec\xa4\x84\xec\x9d\x80 \xed\x95\xad\xec\x83\x81 \xec\xa0\x84\xec\xb2\xb4 \xec\x83\x81\xec\x84\xb8\xeb\xa5\xbc \xec\x9c\xa0\xec\xa7\x80",
  "",
  "",
  "\x1b[1m slot \xc2\xb7 record \xc2\xb7 row\x1b[0m",
  "",
  "   \x1b[1mrow\x1b[0m \x1b[2mSQL \xed\x95\x9c \xed\x96\x89\x1b[0m  \xe2\x94\x80\xe2\x94\x80\xe2\x96\xba  \x1b[1mrecord\x1b[0m \x1b[2m\xec\xa7\x81\xeb\xa0\xac\xed\x99\x94\xeb\x90\x9c \xeb\xb0\x94\xec\x9d\xb4\xed\x8a\xb8\x1b[0m  \xe2\x97\x84\xe2\x94\x80\xe2\x94\x80  \x1b[1mslot\x1b[0m \x1b[2m\xea\xb7\xb8 \xeb\xb0\x94\xec\x9d\xb4\xed\x8a\xb8\xec\x9d\x98 \xec\xa3\xbc\xec\x86\x8c\xed\x91\x9c\x1b[0m",
  "",
  "   \x1b[33m\xec\x8a\xac\xeb\xa1\xaf\xec\x9d\x80 \xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c\xea\xb0\x80 \xec\x95\x84\xeb\x8b\x88\xeb\x9d\xbc \xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c\xeb\xa5\xbc \xea\xb0\x80\xeb\xa6\xac\xed\x82\xa4\xeb\x8a\x94 \xed\x8f\xac\xec\x9d\xb8\xed\x84\xb0\xeb\x8b\xa4 (offset, length).\x1b[0m",
  "",
  "\x1b[1m [\xec\x99\x9c \xed\x95\x9c \xeb\x8b\xa8\xea\xb3\x84 \xea\xb1\xb4\xeb\x84\x88\xeb\x9b\xb0\xeb\x82\x98]\x1b[0m",
  "   OID = volid|pageid|\x1b[33mslotid\x1b[0m  \xe2\x86\x90 \xeb\xb0\x94\xec\x9d\xb4\xed\x8a\xb8 \xec\x9c\x84\xec\xb9\x98\xea\xb0\x80 \xec\x95\x84\xeb\x8b\x88\xeb\x9d\xbc \x1b[1m\xec\x8a\xac\xeb\xa1\xaf \xeb\xb2\x88\xed\x98\xb8\x1b[0m\xeb\xa1\x9c \xea\xb0\x80\xeb\xa6\xac\xed\x82\xa8\xeb\x8b\xa4",
  "   \xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c\xea\xb0\x80 \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xec\x95\x88\xec\x97\x90\xec\x84\x9c \xec\x9d\xb4\xec\x82\xac\xed\x95\xb4\xeb\x8f\x84 \xec\x8a\xac\xeb\xa1\xaf \xeb\xb2\x88\xed\x98\xb8\xeb\x8a\x94 \xea\xb7\xb8\xeb\x8c\x80\xeb\xa1\x9c \xe2\x86\x92 \xec\x9d\xb8\xeb\x8d\xb1\xec\x8a\xa4\xeb\xa5\xbc \xec\x95\x88 \xea\xb3\xa0\xec\xb3\x90\xeb\x8f\x84 \xeb\x90\x9c\xeb\x8b\xa4",
  "",
  "\x1b[1m [\xea\xb0\x9c\xec\x88\x98\xea\xb0\x80 \xec\x96\xb4\xea\xb8\x8b\xeb\x82\x98\xeb\xa9\xb4 \xea\xb7\xb8\xea\xb2\x8c \xec\x8b\xa0\xed\x98\xb8\xeb\x8b\xa4]\x1b[0m   \x1b[2m\xed\x95\x98\xeb\x8b\xa8\xeb\xb0\x94 slots N / recs N\x1b[0m",
  "   \x1b[1mslots = recs\x1b[0m   \xec\xa0\x95\xec\x83\x81 \xe2\x80\x94 \xea\xb2\xa9\xec\x9e\x90\xea\xb0\x80 \xec\xa0\x84\xeb\xb6\x80 \x1b[34mH\x1b[0m \xec\x9d\xb8 \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80",
  "   \x1b[1mslots > recs\x1b[0m   \xec\x82\xad\xec\xa0\x9c\xeb\x90\xa8 \xe2\x80\x94 \xec\x8a\xac\xeb\xa1\xaf\xeb\xa7\x8c \xeb\x82\xa8\xec\x95\x98\xeb\x8b\xa4 (\x1b[31mD\x1b[0m \x1b[31md\x1b[0m)",
  "   \x1b[1m\xed\x96\x89 1\xea\xb0\x9c = \xec\x8a\xac\xeb\xa1\xaf 2\xea\xb0\x9c\x1b[0m  \xec\x9e\xac\xeb\xb0\xb0\xec\xb9\x98 \xe2\x80\x94 \xec\x9b\x90 \xec\x9e\x90\xeb\xa6\xac \x1b[33mR\x1b[0m, \xeb\xb3\xb8\xeb\xac\xb8\xec\x9d\x80 \xeb\x8b\xa4\xeb\xa5\xb8 \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \x1b[36mN\x1b[0m",
  "   \x1b[1m\xeb\xa0\x88\xec\xbd\x94\xeb\x93\x9c\xea\xb0\x80 \xec\x97\xac\xeb\x9f\xac \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\x1b[0m  \xed\x81\xb0 \xed\x96\x89 \xe2\x80\x94 \x1b[35mB\x1b[0m \xea\xb0\x80 \xec\x98\xa4\xeb\xb2\x84\xed\x94\x8c\xeb\xa1\x9c \xec\xb2\xb4\xec\x9d\xb8\xec\x9d\x84 \xea\xb0\x80\xeb\xa6\xac\xed\x82\xa8\xeb\x8b\xa4",
  "",
  "\x1b[1m [\xec\x84\xb8 \xeb\xb0\x95\xec\x8a\xa4\xeb\x8a\x94 \xea\xb0\x81\xea\xb0\x81 \xed\x95\x9c \xea\xb3\x84\xec\xb8\xb5]\x1b[0m",
  "   \x1b[2mvol 0 \xe2\x80\xba sect 6455..6457 \xe2\x80\xba page 413168 \xe2\x80\xba H dba.hits\x1b[0m   \x1b[2m\xe2\x86\x90 \xed\x95\x98\xeb\x8b\xa8\xeb\xb0\x94\xea\xb0\x80 \xea\xb2\xbd\xeb\xa1\x9c\xeb\x8b\xa4\x1b[0m",
  "   sector \xeb\xb0\x95\xec\x8a\xa4 = \xec\x85\x80\xec\x9d\xb4 \xeb\x8d\xae\xeb\x8a\x94 \xec\x84\xb9\xed\x84\xb0    page \xeb\xb0\x95\xec\x8a\xa4 = \x1b[1m\xea\xb7\xb8\xec\xa4\x91 \xea\xb3\xa0\xeb\xa5\xb8 \xed\x95\x9c \xec\x9e\xa5\x1b[0m",
  "   slots \xeb\xb0\x95\xec\x8a\xa4 = \x1b[1m\xea\xb7\xb8 \xed\x95\x9c \xec\x9e\xa5 \xec\x95\x88\x1b[0m\xec\x9d\x98 \xec\x8a\xac\xeb\xa1\xaf\xeb\x93\xa4 \xe2\x80\x94 \xec\x84\xb9\xed\x84\xb0 \xec\xa0\x84\xec\xb2\xb4\xea\xb0\x80 \xec\x95\x84\xeb\x8b\x88\xeb\x8b\xa4",
  "   \xec\xa0\x9c\xeb\xaa\xa9 (49/64) = \xec\x84\xb9\xed\x84\xb0 64\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xec\xa4\x91 49\xeb\xb2\x88\xec\xa7\xb8  \x1b[2m(\xec\xb2\xab \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\xeb\xa9\xb4 1/64)\x1b[0m",
  "",
  "   \x1b[2m\xec\x98\x88) slots 17 recs 17 free 2636B \xe2\x86\x92 16KB \xec\x97\x90 17\xed\x96\x89, \xed\x96\x89\xeb\x8b\xb9 \xec\x95\xbd 800B.\x1b[0m",
  "   \x1b[2m    \xeb\x84\x93\xec\x9d\x80 \xed\x85\x8c\xec\x9d\xb4\xeb\xb8\x94\xec\x9d\xb4\xeb\x9d\xbc \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\xeb\x8b\xb9 \xed\x96\x89\xec\x9d\xb4 \xec\xa0\x81\xec\x9d\x80 \xea\xb2\x83\xec\x9d\xb4 \xec\xa0\x95\xec\x83\x81\xec\x9d\xb4\xeb\x8b\xa4.\x1b[0m",
};

#define VOLMAP_HELP_NLINES ((int) (sizeof (volmap_help_lines) / sizeof (volmap_help_lines[0])))

/* help shown while the file view (f) is open, kept separate from the general help */
static const char *volmap_help_fv_lines[] = {
  "\x1b[1m \xed\x8c\x8c\xec\x9d\xbc \xeb\xb7\xb0 (f)\x1b[0m   \x1b[2m\xec\x9d\xb4 \xeb\xb3\xbc\xeb\xa5\xa8\xec\x9d\x98 \xec\x84\xb9\xed\x84\xb0\xeb\xa5\xbc \xea\xb0\x80\xec\xa7\x84 \xed\x8c\x8c\xec\x9d\xbc \xeb\xaa\xa9\xeb\xa1\x9d\x1b[0m",
  "",
  "\x1b[1m [\xeb\xac\xb4\xec\x97\x87\xec\x9d\x84 \xeb\xb3\xb4\xec\x97\xac\xec\xa3\xbc\xeb\x82\x98]\x1b[0m",
  "   \xeb\xaa\xa9\xeb\xa1\x9d \xed\x95\xad\xeb\xaa\xa9 = \xec\xa2\x85\xeb\xa5\x98\xeb\xb0\xb0\xec\xa7\x80 + \xec\x9d\xb4\xeb\xa6\x84(+\xec\x9d\xb8\xeb\x8d\xb1\xec\x8a\xa4\xeb\xaa\x85),  \xec\x98\xa4\xeb\xa5\xb8\xec\xaa\xbd = \xea\xb7\xb8 \xed\x8c\x8c\xec\x9d\xbc\xec\x9d\xb4 \xec\x93\xb0\xeb\x8a\x94 \xec\x84\xb9\xed\x84\xb0 \xec\x88\x98",
  "   \xed\x95\x98\xeb\x8b\xa8 2\xec\xa4\x84 = \xec\x84\xa0\xed\x83\x9d\xed\x95\x9c \xed\x8c\x8c\xec\x9d\xbc\xec\x9d\x98 VFID\xc2\xb7\xec\xa2\x85\xeb\xa5\x98\xc2\xb7\xec\x84\xb9\xed\x84\xb0\xc2\xb7\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xea\xb5\xac\xec\x84\xb1",
  "",
  "\x1b[1m [\xed\x82\xa4]\x1b[0m",
  "   \x1b[1m\xed\x99\x94\xec\x82\xb4\xed\x91\x9c\x1b[0m  \xeb\xaa\xa9\xeb\xa1\x9d \xec\x9d\xb4\xeb\x8f\x99 \xe2\x80\x94 \xea\xb3\xa0\xeb\xa5\xb8 \xed\x8c\x8c\xec\x9d\xbc \xec\x85\x80\xeb\xa7\x8c \x1b[48;5;17m\xeb\x82\xa8\xec\x83\x89\x1b[49m, \xeb\x82\x98\xeb\xa8\xb8\xec\xa7\x80\xeb\x8a\x94 \xed\x9a\x8c\xec\x83\x89\xec\x9c\xbc\xeb\xa1\x9c \xec\xa3\xbd\xec\x9d\xb8\xeb\x8b\xa4",
  "   \x1b[1mspace\x1b[0m   \xea\xb7\xb8 \xed\x8c\x8c\xec\x9d\xbc\xec\x9d\x98 \xec\xb2\xab \xec\x84\xb9\xed\x84\xb0\xeb\xa1\x9c \xec\xa0\x90\xed\x94\x84 (\xed\x9d\xa9\xec\x96\xb4\xec\xa7\x84 \xed\x8c\x8c\xec\x9d\xbc \xec\x9c\x84\xec\xb9\x98\xeb\xa5\xbc \xeb\xb0\x94\xeb\xa1\x9c \xed\x99\x95\xec\x9d\xb8)",
  "   \x1b[1m\xed\x81\xb4\xeb\xa6\xad\x1b[0m    \xeb\xaa\xa9\xeb\xa1\x9d \xed\x95\xad\xeb\xaa\xa9 \xed\x81\xb4\xeb\xa6\xad = \xea\xb7\xb8 \xed\x8c\x8c\xec\x9d\xbc \xec\x84\xa0\xed\x83\x9d",
  "   \x1b[1mf\x1b[0m       \xed\x8c\x8c\xec\x9d\xbc \xeb\xb7\xb0 \xeb\x8b\xab\xea\xb8\xb0        \x1b[1mq\x1b[0m  \xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90 \xeb\x8b\xab\xea\xb8\xb0",
  "",
  "\x1b[1m [\xec\x9d\xbd\xeb\x8a\x94 \xec\x9a\x94\xeb\xa0\xb9]\x1b[0m",
  "   \xeb\x82\xa8\xec\x83\x89\xec\x9d\xb4 \x1b[1m\xec\x97\xac\xeb\x9f\xac \xea\xb5\xb0\xeb\x8d\xb0\xeb\xa1\x9c \xed\x9d\xa9\xec\x96\xb4\xec\xa0\xb8\x1b[0m \xec\x9e\x88\xec\x9c\xbc\xeb\xa9\xb4 \xea\xb7\xb8 \xed\x8c\x8c\xec\x9d\xbc\xec\x9d\xb4 \xec\xa1\xb0\xea\xb0\x81\xeb\x82\x98 \xec\x9e\x88\xeb\x8b\xa4\xeb\x8a\x94 \xeb\x9c\xbb\xec\x9d\xb4\xeb\x8b\xa4",
  "   \x1b[2m\xe2\x80\xbb \xed\x95\x9c \xed\x85\x8c\xec\x9d\xb4\xeb\xb8\x94\xec\x9d\x80 \xeb\xb3\xb4\xed\x86\xb5 \xed\x8c\x8c\xec\x9d\xbc \xec\x97\xac\xeb\x9f\xac \xea\xb0\x9c(\xed\x9e\x99 1 + \xec\x9d\xb8\xeb\x8d\xb1\xec\x8a\xa4 N) \xe2\x80\x94 file 1/2 \xec\xb2\x98\xeb\x9f\xbc \xed\x91\x9c\xec\x8b\x9c\xeb\x90\x9c\xeb\x8b\xa4\x1b[0m",
  "   \x1b[2m   \xec\x88\x9c\xec\x84\x9c\xeb\x8a\x94 \xed\x9e\x99 \xe2\x86\x92 \xec\x9d\xb8\xeb\x8d\xb1\xec\x8a\xa4 \xe2\x86\x92 \xec\x98\xa4\xeb\xb2\x84\xed\x94\x8c\xeb\xa1\x9c\xec\x9a\xb0\x1b[0m",
  "",
  "   \x1b[2m-m \xea\xb3\xbc \xed\x95\xa8\xea\xbb\x98 \xec\x93\xb0\xeb\xa9\xb4 \xeb\xa7\x88\xed\x82\xb9 \xeb\xb0\xb0\xea\xb2\xbd\xec\x9d\xb4 \xeb\xb0\x9d\xec\x9d\x84\xec\x88\x98\xeb\xa1\x9d \xec\xba\x90\xec\x8b\x9c \xec\x83\x81\xec\xa3\xbc\xec\x9c\xa8\xec\x9d\xb4 \xeb\x86\x92\xeb\x8b\xa4(\xeb\x91\x90 \xec\xa0\x95\xeb\xb3\xb4\xea\xb0\x80 \xeb\x8f\x99\xec\x8b\x9c\xec\x97\x90 \xeb\xb3\xb4\xec\x9d\xb8\xeb\x8b\xa4)\x1b[0m",
};

#define VOLMAP_HELP_FV_NLINES ((int) (sizeof (volmap_help_fv_lines) / sizeof (volmap_help_fv_lines[0])))

/* draw one help screen: screen-height lines starting at document line 'top' */
static void
volmap_help_draw (const char **doc, int nlines, int top, int rows, const char *title)
{
  int y, avail = rows - 1;	/* the last row is the status line */

  if (avail < 1)
    {
      avail = 1;
    }
  printf ("\033[2J\033[H");
  for (y = 0; y < avail; y++)
    {
      int n = top + y;

      if (n >= nlines)
	{
	  break;
	}
      printf ("%s\r\n", doc[n]);
    }
  /* status line: says how much is left, since a scrolling document otherwise hides its end */
  printf ("\033[%d;1H\x1b[7m %s ", rows, title);
  if (nlines > avail)
    {
      int last = top + avail;

      printf (" %d-%d/%d ", top + 1, (last > nlines) ? nlines : last, nlines);
      printf ("%s", (top + avail < nlines) ? "\xe2\x96\xbc \xeb\x8d\x94 \xec\x9e\x88\xec\x9d\x8c " : "\xeb\x81\x9d ");
    }
  printf (" \xe2\x86\x91\xe2\x86\x93 PgUp/PgDn \xec\x8a\xa4\xed\x81\xac\xeb\xa1\xa4 \xc2\xb7 q \xeb\x8b\xab\xea\xb8\xb0 \x1b[0m");
  fflush (stdout);
}

static void
volmap_help_screen (VOLMAP_CTX * ctx, bool file_view)
{
  struct termios tcur, tdrain;
  int top = 0;
  /* With the file view open, show help for that screen instead - the general
     help covers the map and drill-down, which is of little use in front of a
     file list. */
  const char **doc = file_view ? volmap_help_fv_lines : volmap_help_lines;
  int nlines = file_view ? VOLMAP_HELP_FV_NLINES : VOLMAP_HELP_NLINES;
  const char *title = file_view ? "volmap \xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90 \xc2\xb7 \xed\x8c\x8c\xec\x9d\xbc \xeb\xb7\xb0"
    : "volmap \xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90";

  (void) ctx;
  tcgetattr (0, &tcur);
  for (;;)
    {
      unsigned char ch;
      int adv = 0;		/* only q exits; other keys do not close the help */
      struct winsize ws;
      int rows = 24, step;

      if (ioctl (fileno (stdout), TIOCGWINSZ, &ws) == 0 && ws.ws_row >= 5)
	{
	  rows = ws.ws_row;
	}
      step = rows - 2;		/* PgUp/PgDn moves one screen, overlapping by one row */
      if (step < 1)
	{
	  step = 1;
	}
      volmap_help_draw (doc, nlines, top, rows, title);
      tcdrain (fileno (stdout));
      if (read (0, &ch, 1) != 1)
	{
	  break;
	}
      if (ch == 0x1b)
	{
	  /* ESC alone quits; ESC [ ... is an arrow or PgUp/PgDn.  A short timeout on
	     the following bytes tells them apart. */
	  unsigned char b1 = 0, b2 = 0;

	  tdrain = tcur;
	  tdrain.c_cc[VMIN] = 0;
	  tdrain.c_cc[VTIME] = 1;
	  tcsetattr (0, TCSANOW, &tdrain);
	  if (read (0, &b1, 1) != 1)
	    {
	      tcsetattr (0, TCSANOW, &tcur);
	      break;		/* ESC alone exits */
	    }
	  (void) read (0, &b2, 1);
	  if (b1 == (unsigned char) '[')
	    {
	      if (b2 == (unsigned char) 'D' || b2 == (unsigned char) 'A')
		{
		  adv = -1;	/* left/up: previous */
		}
	      else if (b2 == (unsigned char) 'C' || b2 == (unsigned char) 'B')
		{
		  adv = 1;	/* right/down: next */
		}
	      else if (b2 == (unsigned char) '5' || b2 == (unsigned char) '6')
		{
		  unsigned char tilde;

		  (void) read (0, &tilde, 1);	/* PgUp/PgDn = ESC [ 5~ / ESC [ 6~ */
		  adv = (b2 == (unsigned char) '5') ? -1 : 1;
		}
	    }
	  tcsetattr (0, TCSANOW, &tcur);
	}
      else
	{
	  /* Map Hangul-layout keys back to QWERTY: with an IME on, q arrives as a jamo
	     and dropping those bytes leaves the help unable to close.  Uses the same
	     2-beolsik reverse mapping as the main loop. */
	  if ((ch & 0xF0) == 0xE0)
	    {
	      unsigned char k2 = 0, k3 = 0;

	      tdrain = tcur;
	      tdrain.c_cc[VMIN] = 0;
	      tdrain.c_cc[VTIME] = 1;
	      tcsetattr (0, TCSANOW, &tdrain);
	      if (read (0, &k2, 1) == 1 && read (0, &k3, 1) == 1)
		{
		  unsigned int cp = ((unsigned int) (ch & 0x0F) << 12)
		    | ((unsigned int) (k2 & 0x3F) << 6) | (k3 & 0x3F);
		  const char *keys = volmap_hangul_to_keys (cp);

		  if (keys[0] != '\0')
		    {
		      ch = (unsigned char) keys[0];
		    }
		}
	      tcsetattr (0, TCSANOW, &tcur);
	    }
	  else if ((ch & 0x80) != 0)
	    {
	      unsigned char dr;

	      tdrain = tcur;
	      tdrain.c_cc[VMIN] = 0;
	      tdrain.c_cc[VTIME] = 1;
	      tcsetattr (0, TCSANOW, &tdrain);
	      while (read (0, &dr, 1) == 1 && (dr & 0xC0) == 0x80)
		{
		  ;
		}
	      tcsetattr (0, TCSANOW, &tcur);
	    }
	  if (ch == 'q' || ch == 'Q')
	    {
	      break;		/* closes the help only; another q leaves the browser */
	    }
	  if (ch == ' ' || ch == '\r' || ch == '\n')
	    {
	      adv = step;	/* space: one screen down */
	    }
	  else if (ch == 'b' || ch == 0x7f || ch == 0x08)
	    {
	      adv = -step;	/* b / backspace: one screen up */
	    }
	  else if (ch == 'g')
	    {
	      top = 0;		/* g: top */
	      continue;
	    }
	  else if (ch == 'G')
	    {
	      adv = nlines;	/* G: bottom (the clamp below settles it) */
	    }
	}
      top += adv;
      /* Scroll only as far as the last full screen - past that the view would be
         blank.  If the document is shorter than the screen, top stays 0. */
      {
	int vis = rows - 1;
	int maxtop = (nlines > vis) ? (nlines - vis) : 0;

	if (top > maxtop)
	  {
	    top = maxtop;
	  }
	if (top < 0)
	  {
	    top = 0;
	  }
      }
    }
}

#define VOLMAP_CELLSTR 40	/* max bytes of one rendered map cell (SGR + glyph, incl. optional bg reset) */

/* print one map cell from its stored render string.  Every string starts with
 * "\033[0m", so the cursor's reverse video is applied AFTER that reset (a
 * reverse code placed before the string would be cancelled by it).
 * Consecutive cells with the identical SGR prefix emit the glyph alone (SGR
 * state persists on the terminal), shrinking a full frame 3-4x. */
static char volmap_last_sgr[28];	/* longest prefix: reset + bg + fg = 26 bytes */

static void
volmap_put_cell_reset (void)
{
  volmap_last_sgr[0] = '\0';
}

static void
volmap_put_cell (const char *cr, bool at_cursor)
{
  const char *g = strrchr (cr, 'm');	/* glyph starts after the last SGR final byte */
  size_t plen;

  g = (g != NULL) ? g + 1 : cr;
  plen = (size_t) (g - cr);
  if (at_cursor)
    {
      /* Drop the cell's own SGR (cr+4) and use only the cursor colour - with reverse
	 video the cursor colour follows the background and "where am I" is lost. */
      fputs ("\033[0m" VOLMAP_CURSOR_SGR, stdout);
      fputs (g, stdout);
      fputs ("\033[0m", stdout);
      volmap_put_cell_reset ();
      return;
    }
  if (plen < sizeof (volmap_last_sgr) && volmap_last_sgr[plen] == '\0'
      && memcmp (cr, volmap_last_sgr, plen) == 0)
    {
      fputs (g, stdout);
      return;
    }
  fputs (cr, stdout);
  if (plen < sizeof (volmap_last_sgr))
    {
      memcpy (volmap_last_sgr, cr, plen);
      volmap_last_sgr[plen] = '\0';
    }
  else
    {
      volmap_put_cell_reset ();
    }
}

static void volmap_row_sync (int row_done);

/* start-of-row bookkeeping shared by the three interactive map loops: finish
 * the previous row with its panel column, break the line, and sync output at
 * a glyph-safe boundary */
static void
volmap_row_break (VOLMAP_CTX * ctx, VOLMAP_PANEL * panel, int cell, int mapw)
{
  if (cell % mapw != 0)
    {
      return;
    }
  if (cell > 0 && panel != NULL)
    {
      volmap_panel_print_row (ctx, panel, cell / mapw - 1);
    }
  printf ("\033[0m%s\033[2K\033[38;5;240m%s\033[0m ",
	  (cell > 0) ? (volmap_ascii_frame ? " \033[38;5;240m|\033[0m\r\n" : " \033[38;5;240m\xe2\x94\x82\033[0m\r\n") : "",
	  OVG ("\xe2\x94\x82", "|"));
  volmap_put_cell_reset ();
  if (cell > 0)
    {
      volmap_row_sync (cell / mapw - 1);
    }
}

/* flush at a row boundary every 4 rows, then drain the pty.  A frame larger
 * than the pty buffer is otherwise split by short writes at ARBITRARY byte
 * positions; a boundary inside a multi-byte glyph renders as U+FFFD on the
 * receiving terminal (timing-dependent).  Draining keeps every downstream
 * read aligned to our glyph-complete boundaries; on a pty this waits only
 * for the local reader (e.g. sshd) - no network round-trip. */
static void
volmap_row_sync (int row_done)
{
  /* flush EVERY row: a wide map row (~1KB) stays under the 4KB pty chunk, so
   * the kernel never splits a write inside a UTF-8 sequence - clients that
   * cannot reassemble across packets (some ssh tools) stop breaking glyphs */
  fflush (stdout);
  if ((row_done & 3) == 3)
    {
      tcdrain (fileno (stdout));
    }
}

#define VOLMAP_CHAIN_MAX 8192	/* pages walked per [p] request (bounded cost) */

/* walk the heap's LOGICAL page chain from its header page, marking every jump
 * source (next not physically adjacent) in the display volume's bitmap and
 * building the summary line.  Bounded to VOLMAP_CHAIN_MAX pages. */
static void
volmap_chain_walk (VOLMAP_CTX * ctx, VOLMAP_FILE * f, VOLMAP_VOLUME * disp)
{
  VPID cur;
  char *iop;
  int prv = prv_user_offset ();
  long walked = 0, seqn = 0, jumps = 0, maxj = 0, volx = 0;
  int maxio = disp->iopagesize;
  int vi9;
  const char *nm;

  for (vi9 = 0; vi9 < ctx->nvols; vi9++)
    {
      maxio = MAX (maxio, ctx->vols[vi9].iopagesize);
    }
  iop = (char *) malloc (maxio);
  if (iop == NULL)
    {
      return;
    }
  if (volmap_chain_bm_nsect < disp->nsect_total)
    {
      free (volmap_chain_bm);
      volmap_chain_bm_nsect = disp->nsect_total;
      volmap_chain_bm = (UINT64 *) malloc ((size_t) volmap_chain_bm_nsect * sizeof (UINT64));
    }
  if (volmap_chain_bm == NULL)
    {
      free (iop);
      return;
    }
  memset (volmap_chain_bm, 0, (size_t) volmap_chain_bm_nsect * sizeof (UINT64));
  volmap_chain_volid = disp->volid;
  /* the chain starts at the heap HEADER page = the file's sticky-first page
   * (vfid.fileid is the FILE-TABLE page - a FTAB page carries no HEAP_CHAIN) */
  cur.pageid = (f->root_page >= 0) ? f->root_page : f->vfid.fileid;
  cur.volid = f->vfid.volid;
  while (walked < VOLMAP_CHAIN_MAX)
    {
      VOLMAP_VOLUME *cv = volmap_find_vol (ctx, cur.volid);
      const SPAGE_HEADER *sp;
      const SPAGE_SLOT *sl;
      VPID nx;

      if (cv == NULL || cur.pageid < 0 || !volmap_read_iopage (cv, (PAGEID) cur.pageid, iop))
	{
	  break;
	}
      sp = (const SPAGE_HEADER *) (iop + prv);
      if (sp->num_slots < 1)
	{
	  break;
	}
      sl = (const SPAGE_SLOT *) (iop + prv + cv->user_size - sizeof (SPAGE_SLOT));
      if (sl->offset_to_record == 0
	  || (int) sl->offset_to_record + 16 + (int) sizeof (VPID) > cv->user_size)
	{
	  break;
	}
      /* HEAP_CHAIN and HEAP_HDR_STATS both keep next_vpid at +16 (after
       * class_oid + prev_vpid / ovf_vfid) - stored raw, native byte order */
      memcpy (&nx, iop + prv + sl->offset_to_record + 16, sizeof (VPID));
      walked++;
      if (nx.pageid < 0)
	{
	  break;		/* end of chain */
	}
      if (nx.volid == cur.volid && nx.pageid == cur.pageid + 1)
	{
	  seqn++;
	}
      else
	{
	  jumps++;
	  if (nx.volid != cur.volid)
	    {
	      volx++;
	    }
	  else
	    {
	      long d9 = labs ((long) nx.pageid - (long) cur.pageid);

	      if (d9 > maxj)
		{
		  maxj = d9;
		}
	    }
	  if (cur.volid == disp->volid && cur.pageid / VOLMAP_SECT_NPAGES < volmap_chain_bm_nsect)
	    {
	      volmap_chain_bm[cur.pageid / VOLMAP_SECT_NPAGES] |= (UINT64) 1 << (cur.pageid % VOLMAP_SECT_NPAGES);
	    }
	}
      cur = nx;
    }
  free (iop);
  nm = OID_ISNULL (&f->class_oid) ? volmap_ftype_name (f->ftype) : volmap_resolve_class_name (ctx, f);
  snprintf (volmap_chain_sum, sizeof (volmap_chain_sum),
	    "chain %s: %ld pages walked%s \xc2\xb7 sequential %.1f%% \xc2\xb7 jumps %ld (max %ldp, %ld vol-cross) \xc2\xb7 jump cells = red bg  [p]=off",
	    nm, walked, (walked >= VOLMAP_CHAIN_MAX) ? " (capped)" : "",
	    (walked > 1) ? 100.0 * seqn / (walked - (walked < VOLMAP_CHAIN_MAX ? 1 : 0)) : 100.0,
	    jumps, maxj, volx);
}

/* one-line identity of the file owning `sect`: badge + name(+ovf) + sectors + file k/n
 * inside its object - the FIRST line of the drill-down info box at every depth */
static void
volmap_file_line (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, long sect, char *buf, size_t bufsz)
{
  int ow0 = (sect >= 0 && sect < vol->nsect_total) ? vol->owner[sect] : -1;
  VOLMAP_FILE *f0;
  int k0, role0;
  const char *n0;

  buf[0] = '\0';
  if (ow0 < 0 || ow0 >= ctx->nfiles)
    {
      snprintf (buf, bufsz, "%s", (sect >= 0) ? "(no owning file)" : "");
      return;
    }
  f0 = &ctx->files[ow0];
  k0 = volmap_kind_idx (f0->ftype);
  role0 = volmap_obj_role (f0->ftype);
  if (OID_ISNULL (&f0->class_oid))
    {
      n0 = volmap_ftype_name (f0->ftype);
    }
  else if (k0 == 1 && role0 != 2)
    {
      n0 = volmap_resolve_index_name (ctx, f0);
    }
  else
    {
      n0 = volmap_resolve_class_name (ctx, f0);
    }
  /* what the tracker says of this file, when it was read: listed as dropped (its
     sectors are freed by vacuum later), or not listed at all (found by probing) */
  const char *trk = (ctx->tracker_state != 1 || f0->tracked == 1) ? ""
    : (f0->tracked == 2) ? " [dropped, awaiting vacuum]" : " [not in the file tracker]";

  if (!OID_ISNULL (&f0->class_oid))
    {
      int i9, tot9 = 0, ord9 = 1;

      for (i9 = 0; i9 < ctx->nfiles; i9++)
	{
	  VOLMAP_FILE *f9 = &ctx->files[i9];
	  int r9;

	  if (OID_ISNULL (&f9->class_oid) || !OID_EQ (&f9->class_oid, &f0->class_oid))
	    {
	      continue;
	    }
	  tot9++;
	  r9 = volmap_obj_role (f9->ftype);
	  if (f9 != f0
	      && (r9 < role0 || (r9 == role0 && f9->vfid.fileid < f0->vfid.fileid)
		  || (r9 == role0 && f9->vfid.fileid == f0->vfid.fileid && f9->vfid.volid < f0->vfid.volid)))
	    {
	      ord9++;
	    }
	}
      /* A bare 's' reads as seconds; the same value is written "sectors" elsewhere,
	 so spell the unit out here too (sectors_seen = sectors this file uses in
	 this volume). */
      snprintf (buf, bufsz, "%s%s%s (%d sect \xc2\xb7 file %d/%d)%s",
		(k0 == 0) ? VOLMAP_TAG_H : (k0 == 1) ? VOLMAP_TAG_I : "", n0,
		(role0 == 2) ? " ovf" : "", f0->sectors_seen, ord9, tot9, trk);
    }
  else
    {
      snprintf (buf, bufsz, "%s (%d sectors)%s", n0, f0->sectors_seen, trk);
    }
}

/* [f] file view: list of the files owning sectors in this volume - the map
 * dims everything else (volmap_focus_file), so one file's physical footprint
 * reads at a glance (volume -> file -> sector browsing). */
static void
volmap_file_view_draw (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, const int *fv_files, const long *fv_first,
		       const int *fv_nsect, int fv_n, int fv_sel, int scr_cols, int scr_rows, int inner_w,
		       int map_right, int left_side, int map_rows)
{
  (void) vol;			/* the list is already resolved; kept for a uniform signature */
  (void) fv_first;		/* first sector per file - used by the caller's cursor jump, not here */

  /* The file view is wider than the drill-down: a list entry is "name + index
     name", and at 36 columns most long index names are cut.  On a narrow screen
     it falls back to the usual width. */
  int w = inner_w + 4;		/* file view width, passed in by the caller rather than held globally */
  int x0;
  int y = 7;
  int budget;

  if (w + 10 > scr_cols)
    {
      w = VOLMAP_OV_W_MIN + 4;
    }
  x0 = volmap_overlay_place_x (scr_cols, map_right, w, left_side, map_rows, scr_rows);	/* same rule as the drill-down */
  budget = scr_rows - 4 - y - 3;
  int top = 0, r;
  char t1[48];

  if (x0 < 1)
    {
      x0 = 1;
    }
  if (budget < 3)
    {
      budget = 3;
    }
  if (budget > 18)
    {
      budget = 18;		/* drill-down-sized box: the list scrolls anyway */
    }
  if (budget > fv_n)
    {
      budget = fv_n;
    }
  if (fv_sel >= top + budget)
    {
      top = fv_sel - budget + 1;
    }
  snprintf (t1, sizeof (t1), "files %d (right = sectors)%s", fv_n, (top > 0) ? " ..." : "");
  volmap_overlay_clear (x0, y, w, budget + 4);	/* outline first, same contract as the file view */
  volmap_overlay_box (x0, y++, w, t1, 1);
  volmap_fv_geom.x0 = x0;
  volmap_fv_geom.w = w;
  volmap_fv_geom.list_y0 = y;
  volmap_fv_geom.rows = budget;
  volmap_fv_geom.top = top;
  for (r = 0; r < budget; r++, y++)
    {
      int li = top + r;
      VOLMAP_FILE *f = &ctx->files[fv_files[li]];
      int kind = volmap_kind_idx (f->ftype);
      const char *name;
      char row[128], cnt[16];
      int kc = (kind == 0) ? 34 : (kind == 1) ? 32 : (kind == 4) ? 33 : 31;

      int role = volmap_obj_role (f->ftype);
      char nbuf[96];

      if (OID_ISNULL (&f->class_oid))
	{
	  name = volmap_ftype_name (f->ftype);
	}
      else if (kind == 1 && role != 2)
	{
	  /* the IX badge already says "index": the INDEX NAME is the identity here */
	  name = volmap_resolve_index_name (ctx, f);
	}
      else
	{
	  name = volmap_resolve_class_name (ctx, f);
	}
      if (role == 2)
	{
	  snprintf (nbuf, sizeof (nbuf), "%s ovf", name);	/* overflow appendage of its object */
	  name = nbuf;
	}
      snprintf (cnt, sizeof (cnt), "%d", fv_nsect[li]);
      /* Derive the name column from the box width - a fixed 22 columns kept names
	 truncated even after widening the box.  What remains after the badge (4),
	 the sector count (6) and padding is the name's share. */
      {
	int namew = w - 8 - 7;	/* content width (w-8) less the 7 columns for the sector count */

	if (namew < 12)
	  {
	    namew = 12;
	  }
	if (namew > 60)
	  {
	    namew = 60;
	  }
	snprintf (row, sizeof (row), "%-*.*s %5s", namew, namew, name, cnt);
      }
      volmap_ov_row_open (x0, y);	/* left border, same primitive as the drill-down box */
      /* kind badge, exactly as everywhere else: heap = blue HP, index = green IX;
       * internal/temp files keep a kind-colored glyph */
      if (role == 2)
	{
	  printf ("\033[38;5;215m\xe2\xa0\xbf\033[0m   ");	/* orange = overflow, as on the map */
	}
      else if (kind == 0)
	{
	  fputs (VOLMAP_TAG_H, stdout);
	}
      else if (kind == 1)
	{
	  fputs (VOLMAP_TAG_I, stdout);
	}
      else
	{
	  printf ("\033[%dm\xe2\xa0\xbf\033[0m   ", kc);
	}
      if (li == fv_sel)
	{
	  fputs (VOLMAP_CURSOR_SGR, stdout);	/* same cursor colour as the map and drill-down */
	}
      /* A row is border(1) + space(1) + badge(4) + content + space(1) + border(1),
	 so the content width is w-8.  Using w-4 overflows by 4 and pushes the right
	 border outside the box. */
      volmap_put_padded (row, w - 8);
      volmap_ov_row_close (0);	/* right border */
      fflush (stdout);
    }
  volmap_overlay_boxend (x0, y++, w);
  printf ("\033[0m\033[%d;%dH%s", y, x0, "\033[38;5;245m");
  volmap_put_padded (" \xe2\x86\x95 file  space/enter jump  f close", w);
  printf ("\033[0m");
}

/* ==================  interactive threading (3-thread model)  ==================
 * UI thread   : input, rendering, terminal modes, signals - unchanged logic.
 * Worker A    : batch lane - progressive scan slices and the incremental
 *               refresh.  A refresh rebuilds the per-volume metadata in the
 *               sh_* shadow arrays (via the w_* write-side pointers) and
 *               commits by pointer swap, so the UI never renders a
 *               half-rebuilt map and a cold multi-second refresh does not
 *               freeze input.
 * Worker B    : low-latency lane - warms the overlay's 1MB sector read into
 *               the page cache (queue depth 1, stale requests replaced); the
 *               UI then fills the panel from cache without a cold stall.
 * Wakeups reach the UI's select() through a self-pipe.  All volume fds are
 * pre-opened and pinned (volmap_fd_pin), so in steady state no thread closes
 * an fd another thread is reading from.
 *
 * The one exception is the [r] temp-volume rescan, which does close fds and
 * move entries in ctx->vols[].  Pinning cannot cover that, so the rescan runs
 * behind a barrier instead: it raises list_frozen, then waits for a_busy and
 * b_busy to clear under each lane's own mutex, and skips the round rather than
 * proceed if either lane will not go idle.  Both lanes re-check list_frozen
 * under their mutex before taking work, so an observed "idle" cannot go stale.
 * See the [r] handler for the full sequence. */
static struct
{
  VOLMAP_CTX *ctx;
  pthread_t a_tid;
  pthread_t b_tid;
  bool started;
  volatile sig_atomic_t stop;
  int pipe_rd, pipe_wr;

  pthread_mutex_t a_mx;
  pthread_cond_t a_cv;
  bool a_refresh_req;
  bool a_refresh_done;		/* completion note for the status bar */
  bool a_busy;			/* the batch lane is walking the volume array; the list must not be touched meanwhile */
  int a_resid_vi;		/* >= 0: re-read residency for this volume index */
  unsigned list_gen;		/* bumped under a_mx whenever [r] changes the volume list */
  volatile unsigned ui_frames;	/* UI frame counter: shadow-reuse guard */
  /* Publish interlock.  ui_reading says the UI is dereferencing the per-volume arrays;
   * the commit may only swap the pointers while it is 0.  Both sides take pub_mx, so
   * the flag is not a bare cross-thread int and the check cannot race the swap. */
  pthread_mutex_t pub_mx;
  pthread_cond_t pub_cv;
  int ui_reading;

  pthread_mutex_t b_mx;
  pthread_cond_t b_cv;
  unsigned b_req_seq, b_done_seq;
  bool b_busy;			/* the prefetch lane holds a volume pointer/fd; the list must not be touched meanwhile */
  /* Rescan barrier.  The UI raises it before changing the volume list; both lanes
   * check it under their own mutex and refuse to start new work while it is set, so
   * "no lane is busy" cannot go stale between the check and the rescan. */
  volatile sig_atomic_t list_frozen;
  int b_vol;
  long b_sect;
  int b_done_vol;
  long b_done_sect;
  char *b_buf;
  size_t b_buf_sz;
} volmap_mt = {
  NULL, 0, 0, false, 0, -1, -1,
  PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false, -1, 0, 0,
  PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0,
  PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, false, 0, -1, -1, -1, -1, NULL, 0
};

static volatile long volmap_last_key_sec = 0;	/* CLOCK_MONOTONIC secs of the last keypress (scan idle detection) */
static unsigned char volmap_key_pending[8] = "";	/* keys decoded from one composed Hangul syllable, fed one per loop */

static void
volmap_mt_wake (void)
{
  if (volmap_mt.pipe_wr >= 0)
    {
      char c = 1;

      (void) !write (volmap_mt.pipe_wr, &c, 1);	/* O_NONBLOCK: a full pipe already wakes the UI */
    }
}

/* Rebuild the volume metadata in the shadow arrays, then swap the read side.
 * Runs on worker A only.  The scan is never published half way, so a frame can not
 * mix an old owner with a new reservation.
 *
 * skip_scan republishes a scan that is already sitting in the shadows: the caller
 * uses it after VOLMAP_PUB_PENDING so an abandoned swap costs one more swap attempt
 * rather than another full pass over every file table. */
typedef enum
{
  VOLMAP_PUB_OK = 0,		/* scanned (or not) and published */
  VOLMAP_PUB_PENDING,		/* scan is in the shadows, the UI was mid-frame: retry the swap */
  VOLMAP_PUB_FAILED		/* no shadow memory: nothing to retry, do not re-arm */
} VOLMAP_PUBRES;

static VOLMAP_PUBRES
volmap_mt_refresh (VOLMAP_CTX * ctx, bool skip_scan)
{
  int vi;

  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];

      if (vol->sh_stab == NULL)
	{
	  vol->sh_stab = (unsigned char *) calloc (vol->nsect_total, 1);
	  vol->sh_owner = (int *) malloc (vol->nsect_total * sizeof (int));
	  vol->sh_alloc = (int *) calloc (vol->nsect_total, sizeof (int));
	  vol->sh_pagebm = (UINT64 *) calloc (vol->nsect_total, sizeof (UINT64));
	  if (vol->sh_owner != NULL)
	    {
	      memset (vol->sh_owner, 0xff, vol->nsect_total * sizeof (int));	/* -1 = no owner */
	    }
	  /* A shadow made just now holds no scan, so there is nothing to republish:
	     scan whatever the caller asked. */
	  skip_scan = false;
	}
      if (vol->sh_stab == NULL || vol->sh_owner == NULL || vol->sh_alloc == NULL || vol->sh_pagebm == NULL)
	{
	  return VOLMAP_PUB_FAILED;	/* no shadow memory: the map keeps the old state */
	}
      vol->w_stab = vol->sh_stab;
      vol->w_owner = vol->sh_owner;
      vol->w_alloc = vol->sh_alloc;
      vol->w_pagebm = vol->sh_pagebm;
    }
  if (!skip_scan)
    {
      (void) volmap_refresh (ctx);	/* every write lands in the shadows */
    }
  /* Publish at a frame boundary.  The four pointers are swapped one at a time, and a
   * frame reads them together (reservation, owner, allocation, page bitmap), so a
   * commit landing mid-frame would draw a mix of the old and new scan.  Wait for the
   * UI to finish the frame it is in; the guard bounds the wait so a stalled UI only
   * delays the refresh by that much instead of blocking the worker for good. */
  /* Take the interlock and swap only while ui_reading is 0.  Holding pub_mx across
   * the swap is what makes it atomic with respect to a frame: the UI cannot enter a
   * frame in the middle of it.  If the UI does not yield within the bound, the swap
   * is abandoned rather than forced - the shadows keep the scan and the caller
   * retries, so the published set is always one whole scan. */
  pthread_mutex_lock (&volmap_mt.pub_mx);
  { int guard;

    for (guard = 0; volmap_mt.ui_reading && guard < 50 && !volmap_mt.stop; guard++)
      {
	struct timespec dl;

	clock_gettime (CLOCK_REALTIME, &dl);
	dl.tv_nsec += 2 * 1000000L;
	dl.tv_sec += dl.tv_nsec / 1000000000L;
	dl.tv_nsec %= 1000000000L;
	(void) pthread_cond_timedwait (&volmap_mt.pub_cv, &volmap_mt.pub_mx, &dl);
      }
    if (volmap_mt.ui_reading)
      {
	pthread_mutex_unlock (&volmap_mt.pub_mx);
	return VOLMAP_PUB_PENDING;	/* keep the scan in the shadows; retry the swap only */
      }
  }
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];

      /* commit: shadow becomes the read side; the old arrays become the next shadow.
       * Plain aligned pointer stores - the UI picks the new set up on its next deref. */
      vol->sh_stab = vol->stab;
      vol->sh_owner = vol->owner;
      vol->sh_alloc = vol->alloc;
      vol->sh_pagebm = vol->pagebm;
      vol->stab = vol->w_stab;
      vol->owner = vol->w_owner;
      vol->alloc = vol->w_alloc;
      vol->pagebm = vol->w_pagebm;
    }
  ctx->refresh_gen++;		/* with the swap: cached answers expire when the new map is shown */
  pthread_mutex_unlock (&volmap_mt.pub_mx);
  return VOLMAP_PUB_OK;
}

static void *
volmap_mt_a_main (void *arg)
{
  VOLMAP_CTX *ctx = volmap_mt.ctx;
  unsigned commit_frame = 0;
  bool committed = false;
  bool pending_publish = false;	/* a scan waiting in the shadows for a frame boundary */
  unsigned pending_gen = 0;	/* list_gen the pending scan was taken over */
  struct timespec last_prog = { 0, 0 };

  (void) arg;
  while (!volmap_mt.stop)
    {
      bool do_refresh, new_req, reuse;
      unsigned gen;
      int resid_vi;

      pthread_mutex_lock (&volmap_mt.a_mx);
      /* Wait unless there is new work.  A pending publish is not new work - its retry
       * waits out the tick, since the UI it waits for can stay mid-frame indefinitely
       * (the help screen holds ui_reading until it is closed).  Nor is anything while
       * the list is frozen: the UI wakes us when it lifts the barrier. */
      if (volmap_mt.list_frozen
	  || (!volmap_mt.a_refresh_req && volmap_mt.a_resid_vi < 0 && (!ctx->scan_active || ctx->hold)))
	{
	  struct timespec dl;

	  clock_gettime (CLOCK_REALTIME, &dl);
	  dl.tv_nsec += 200 * 1000000L;
	  dl.tv_sec += dl.tv_nsec / 1000000000L;
	  dl.tv_nsec %= 1000000000L;
	  (void) pthread_cond_timedwait (&volmap_mt.a_cv, &volmap_mt.a_mx, &dl);
	}
      new_req = volmap_mt.a_refresh_req;
      volmap_mt.a_refresh_req = false;
      resid_vi = volmap_mt.a_resid_vi;
      volmap_mt.a_resid_vi = -1;
      /* mark the window in which a UI-side list change would move the array underfoot */
      if (volmap_mt.list_frozen)
	{
	  /* Same barrier as lane B: hold off while the UI is about to change the list. */
	  volmap_mt.a_refresh_req = new_req;	/* keep a real request; the work is not lost */
	  new_req = false;
	  resid_vi = -1;
	}
      do_refresh = new_req || (pending_publish && !volmap_mt.list_frozen);
      /* The pending scan may be republished only as it is: no new request asked for
       * a fresh one, and the volume list is the one it was taken over.  [r] can add
       * or drop volumes while the retry waits, and a new volume has no scan. */
      reuse = pending_publish && !new_req && pending_gen == volmap_mt.list_gen;
      gen = volmap_mt.list_gen;
      volmap_mt.a_busy = (do_refresh || resid_vi >= 0);
      pthread_mutex_unlock (&volmap_mt.a_mx);
      if (volmap_mt.stop)
	{
	  break;
	}
      if (do_refresh)
	{
	  int guard;

	  /* the UI may still hold last-commit array pointers mid-frame: wait for
	   * one frame boundary before memsetting them as the next shadow */
	  for (guard = 0; committed && volmap_mt.ui_frames == commit_frame && guard < 30 && !volmap_mt.stop; guard++)
	    {
	      struct timespec ts = { 0, 10 * 1000000L };

	      nanosleep (&ts, NULL);
	    }
	  if (!reuse)
	    {
	      pending_gen = gen;	/* the list this scan is taken over */
	    }
	  switch (volmap_mt_refresh (ctx, reuse))
	    {
	    case VOLMAP_PUB_OK:
	      pending_publish = false;
	      committed = true;
	      commit_frame = volmap_mt.ui_frames;
	      pthread_mutex_lock (&volmap_mt.a_mx);
	      volmap_mt.a_refresh_done = true;
	      pthread_mutex_unlock (&volmap_mt.a_mx);
	      volmap_mt_wake ();
	      break;

	    case VOLMAP_PUB_PENDING:
	      /* The swap was abandoned to keep the frame consistent.  The scan is still
	       * in the shadows, so the retry republishes it instead of scanning again -
	       * a UI that stays mid-frame (the help screen holds ui_reading for as long
	       * as it is open) would otherwise re-read every file table each round. */
	      pending_publish = true;	/* retried after the tick; a_refresh_req stays for new requests */
	      break;

	    case VOLMAP_PUB_FAILED:
	      /* Out of shadow memory: retrying cannot help, so do not re-arm. */
	      pending_publish = false;
	      break;
	    }
	}
      if (resid_vi >= 0 && resid_vi < ctx->nvols)
	{
	  (void) volmap_read_residency (&ctx->vols[resid_vi]);
	  volmap_mt_wake ();
	}
      if (volmap_mt.a_busy)
	{
	  pthread_mutex_lock (&volmap_mt.a_mx);
	  volmap_mt.a_busy = false;	/* done with the volume array; the list may be changed now */
	  pthread_cond_broadcast (&volmap_mt.a_cv);
	  pthread_mutex_unlock (&volmap_mt.a_mx);
	}
      if (ctx->scan_active && !ctx->hold)
	{
	  struct timespec now;
	  bool idle, busy = ctx->io.busy;
	  bool done;
	  bool go;

	  /* Discovery walks the volume array too - and its last step, the --full-sweep
	     pass 2, runs after scan_active has dropped, when [r] no longer refuses.
	     Hold a_busy across the step, like any other work, so [r] waits for it. */
	  pthread_mutex_lock (&volmap_mt.a_mx);
	  go = !volmap_mt.list_frozen;
	  if (go)
	    {
	      volmap_mt.a_busy = true;
	    }
	  pthread_mutex_unlock (&volmap_mt.a_mx);
	  if (!go)
	    {
	      continue;
	    }
	  clock_gettime (CLOCK_MONOTONIC, &now);
	  idle = (now.tv_sec - volmap_last_key_sec) > 2;
	  /* a busy disk: shallow slices (512 sectors, 50ms) with a pause between them, so
	     the scan takes longer and the other load keeps the disk */
	  done = volmap_discover_step (ctx, busy ? 512 : 8192, idle && !busy);
	  pthread_mutex_lock (&volmap_mt.a_mx);
	  volmap_mt.a_busy = false;
	  pthread_cond_broadcast (&volmap_mt.a_cv);
	  pthread_mutex_unlock (&volmap_mt.a_mx);
	  if (busy && !done)
	    {
	      struct timespec ts = { 0, 200 * 1000000L };

	      nanosleep (&ts, NULL);
	    }
	  clock_gettime (CLOCK_MONOTONIC, &now);
	  if (done
	      || (now.tv_sec - last_prog.tv_sec) * 1000 + (now.tv_nsec - last_prog.tv_nsec) / 1000000 >= 150)
	    {
	      last_prog = now;
	      volmap_mt_wake ();	/* progress (or completion) repaint */
	    }
	}
    }
  return NULL;
}

static void *
volmap_mt_b_main (void *arg)
{
  VOLMAP_CTX *ctx = volmap_mt.ctx;

  (void) arg;
  pthread_mutex_lock (&volmap_mt.b_mx);
  while (!volmap_mt.stop)
    {
      unsigned seq;
      int v;
      long sect;
      size_t need;

      if (volmap_mt.b_req_seq == volmap_mt.b_done_seq)
	{
	  struct timespec dl;

	  clock_gettime (CLOCK_REALTIME, &dl);
	  dl.tv_nsec += 200 * 1000000L;
	  dl.tv_sec += dl.tv_nsec / 1000000000L;
	  dl.tv_nsec %= 1000000000L;
	  (void) pthread_cond_timedwait (&volmap_mt.b_cv, &volmap_mt.b_mx, &dl);
	  continue;
	}
      if (volmap_mt.list_frozen)
	{
	  /* A rescan is pending: do not pick up work that would pin a volume it is
	     about to drop.  Checked under b_mx together with b_busy, so the UI cannot
	     see "idle" and have this lane go busy right afterwards. */
	  struct timespec dl;

	  clock_gettime (CLOCK_REALTIME, &dl);
	  dl.tv_nsec += 5 * 1000000L;
	  dl.tv_sec += dl.tv_nsec / 1000000000L;
	  dl.tv_nsec %= 1000000000L;
	  (void) pthread_cond_timedwait (&volmap_mt.b_cv, &volmap_mt.b_mx, &dl);
	  continue;
	}
      seq = volmap_mt.b_req_seq;
      v = volmap_mt.b_vol;
      sect = volmap_mt.b_sect;
      /* The volume pointer and fd below are only valid while the list stays put, so
         mark the window the UI must wait out before it drops a vanished volume. */
      volmap_mt.b_busy = true;
      pthread_mutex_unlock (&volmap_mt.b_mx);
      if (v >= 0 && v < ctx->nvols && sect >= 0)
	{
	  VOLMAP_VOLUME *vol = &ctx->vols[v];

	  need = (size_t) VOLMAP_SECT_NPAGES * vol->iopagesize;
	  if (volmap_mt.b_buf_sz < need)
	    {
	      free (volmap_mt.b_buf);
	      volmap_mt.b_buf = (char *) malloc (need);
	      volmap_mt.b_buf_sz = (volmap_mt.b_buf != NULL) ? need : 0;
	    }
	  if (volmap_mt.b_buf != NULL)
	    {
	      /* the bytes are discarded: the point is the page-cache warm-up,
	       * so the UI's own fill right after is a cache hit */
	      (void) pread (volmap_vol_fd (vol), volmap_mt.b_buf, need, (off_t) sect * (off_t) need);
	    }
	}
      pthread_mutex_lock (&volmap_mt.b_mx);
      volmap_mt.b_busy = false;	/* done with the volume array; the list may be changed now */
      pthread_cond_broadcast (&volmap_mt.b_cv);
      volmap_mt.b_done_seq = seq;
      volmap_mt.b_done_vol = v;
      volmap_mt.b_done_sect = sect;
      pthread_cond_broadcast (&volmap_mt.b_cv);
      if (volmap_mt.b_req_seq == seq)
	{
	  volmap_mt_wake ();	/* the result is still the one on screen: repaint */
	}
    }
  pthread_mutex_unlock (&volmap_mt.b_mx);
  return NULL;
}

/* UI side: is this sector's 1MB warm?  If not, hand it to worker B and give a
 * cache-warm request up to 15ms to finish (avoids placeholder flicker on warm
 * data); a cold read completes later and wakes the UI through the pipe. */
static bool
volmap_mt_sector_warm (int vi, long sect)
{
  bool ok;
  struct timespec dl;

  if (!volmap_mt.started)
    {
      return true;		/* single-threaded: fill synchronously as before */
    }
  pthread_mutex_lock (&volmap_mt.b_mx);
  if (volmap_mt.b_done_seq == volmap_mt.b_req_seq && volmap_mt.b_done_vol == vi && volmap_mt.b_done_sect == sect)
    {
      pthread_mutex_unlock (&volmap_mt.b_mx);
      return true;
    }
  if (!(volmap_mt.b_req_seq != volmap_mt.b_done_seq && volmap_mt.b_vol == vi && volmap_mt.b_sect == sect))
    {
      volmap_mt.b_vol = vi;	/* queue depth 1: replace whatever was pending */
      volmap_mt.b_sect = sect;
      volmap_mt.b_req_seq++;
      pthread_cond_broadcast (&volmap_mt.b_cv);
    }
  clock_gettime (CLOCK_REALTIME, &dl);
  dl.tv_nsec += 15 * 1000000L;
  dl.tv_sec += dl.tv_nsec / 1000000000L;
  dl.tv_nsec %= 1000000000L;
  while (volmap_mt.b_done_seq != volmap_mt.b_req_seq)
    {
      if (pthread_cond_timedwait (&volmap_mt.b_cv, &volmap_mt.b_mx, &dl) != 0)
	{
	  break;
	}
    }
  ok = (volmap_mt.b_done_seq == volmap_mt.b_req_seq && volmap_mt.b_done_vol == vi && volmap_mt.b_done_sect == sect);
  pthread_mutex_unlock (&volmap_mt.b_mx);
  return ok;
}

static void
volmap_mt_start (VOLMAP_CTX * ctx)
{
  int pfd[2];
  int i;

  volmap_fd_pin = 1;		/* every thread preads on shared fds: no MRU eviction from now on */
  for (i = 0; i < ctx->nvols; i++)
    {
      (void) volmap_vol_fd (&ctx->vols[i]);
    }
  if (pipe (pfd) != 0)
    {
      return;			/* no pipe, no threads: interactive falls back to single-threaded */
    }
  (void) fcntl (pfd[0], F_SETFL, fcntl (pfd[0], F_GETFL, 0) | O_NONBLOCK);
  (void) fcntl (pfd[1], F_SETFL, fcntl (pfd[1], F_GETFL, 0) | O_NONBLOCK);
  volmap_mt.ctx = ctx;
  volmap_mt.pipe_rd = pfd[0];
  volmap_mt.pipe_wr = pfd[1];
  volmap_mt.stop = 0;
  if (pthread_create (&volmap_mt.a_tid, NULL, volmap_mt_a_main, NULL) != 0)
    {
      close (pfd[0]);
      close (pfd[1]);
      volmap_mt.pipe_rd = volmap_mt.pipe_wr = -1;
      return;
    }
  if (pthread_create (&volmap_mt.b_tid, NULL, volmap_mt_b_main, NULL) != 0)
    {
      volmap_mt.stop = 1;
      pthread_cond_broadcast (&volmap_mt.a_cv);
      pthread_join (volmap_mt.a_tid, NULL);
      close (pfd[0]);
      close (pfd[1]);
      volmap_mt.pipe_rd = volmap_mt.pipe_wr = -1;
      return;
    }
  volmap_mt.started = true;
  volmap_mt_run = 1;
}

static void
volmap_mt_stop (void)
{
  if (!volmap_mt.started)
    {
      return;
    }
  volmap_mt.stop = 1;
  pthread_cond_broadcast (&volmap_mt.a_cv);
  pthread_cond_broadcast (&volmap_mt.b_cv);
  pthread_join (volmap_mt.a_tid, NULL);
  pthread_join (volmap_mt.b_tid, NULL);
  close (volmap_mt.pipe_rd);
  close (volmap_mt.pipe_wr);
  volmap_mt.pipe_rd = volmap_mt.pipe_wr = -1;
  free (volmap_mt.b_buf);
  volmap_mt.b_buf = NULL;
  volmap_mt.b_buf_sz = 0;
  volmap_mt.started = false;
  volmap_mt_run = 0;
}

/* Bottom status bar: two inspection rows plus the key list.
   All width computation goes through volmap_disp_clip.
   status2 must be writable - the \x01 line-break hints are stripped in place. */
/* ---- what runs beside the database: /proc only, nothing asked of the server --------
 *
 * Two facts are public on Linux: every process's command line and the socket table.
 *   - cub_server <db> is the server.  It registers with cub_master only after
 *     boot_restart_server() - which includes restart recovery - has returned
 *     (network_sr.c: css_init after boot_restart_server), and that registration opens a
 *     datagram socket named cubrid_tcp_setup_server<pid>.  The file is unlinked, but
 *     the bound name stays in /proc/net/unix for as long as the server runs.  So:
 *     server process without that name = booting or recovering; with it = running.
 *   - cubrid/cub_admin <utility> ... <db> is a maintenance utility on this database
 *     (backupdb, checkdb, ...).  The engine keeps "backup in progress" only in memory
 *     (log_Gl.backup_in_progress), never in the log header, so the client process is
 *     the one sign of it outside the server.
 * Not visible this way, and so not claimed: work the server does on its own (vacuum,
 * checkpoint, flushing), a backup or utility run from another host, and other
 * accounts' processes on a /proc mounted with hidepid=2. */

static const char *const volmap_util_names[] = {
  "backupdb", "restoredb", "checkdb", "compactdb", "vacuumdb", "loaddb", "unloaddb", "addvoldb",
  "optimizedb", "copydb", "deletedb", "renamedb", "diagdb", NULL
};

/* /proc/<pid>/cmdline split on NUL; returns argc (0 = unreadable or gone) */
static int
volmap_cmdline (pid_t pid, char *buf, size_t bufsz, char **argv, int maxargv)
{
  char path[64];
  int fd, argc = 0;
  ssize_t n;
  size_t i;

  snprintf (path, sizeof (path), "/proc/%ld/cmdline", (long) pid);
  fd = open (path, O_RDONLY);
  if (fd < 0)
    {
      return 0;
    }
  n = read (fd, buf, bufsz - 1);
  close (fd);
  if (n <= 0)
    {
      return 0;
    }
  buf[n] = '\0';
  for (i = 0; i < (size_t) n && argc < maxargv; i++)
    {
      if (i == 0 || buf[i - 1] == '\0')
	{
	  argv[argc++] = buf + i;
	}
    }
  return argc;
}

/* seconds since the process started: /proc/<pid>/stat field 22 (clock ticks after boot)
   against /proc/uptime; -1 when unreadable */
static long
volmap_proc_elapsed (pid_t pid)
{
  char path[64], line[1024];
  FILE *f;
  double uptime = -1;
  long long start = -1;
  const char *p;
  int field;

  f = fopen ("/proc/uptime", "r");
  if (f != NULL)
    {
      if (fscanf (f, "%lf", &uptime) != 1)
	{
	  uptime = -1;
	}
      fclose (f);
    }
  snprintf (path, sizeof (path), "/proc/%ld/stat", (long) pid);
  f = fopen (path, "r");
  if (f == NULL || fgets (line, sizeof (line), f) == NULL)
    {
      if (f != NULL)
	{
	  fclose (f);
	}
      return -1;
    }
  fclose (f);
  /* the command name in field 2 may hold spaces: resume after its closing parenthesis */
  p = strrchr (line, ')');
  if (p == NULL)
    {
      return -1;
    }
  p++;
  for (field = 3; *p != '\0'; field++)
    {
      while (*p == ' ')
	{
	  p++;
	}
      if (field == 22)
	{
	  start = atoll (p);
	  break;
	}
      while (*p != '\0' && *p != ' ')
	{
	  p++;
	}
    }
  if (start < 0 || uptime < 0)
    {
      return -1;
    }
  return (long) (uptime - (double) start / (double) sysconf (_SC_CLK_TCK));
}

/* is the server's registration socket name bound (see the note above) */
static bool
volmap_server_registered (pid_t pid)
{
  char want[64], line[1024];
  FILE *f = fopen ("/proc/net/unix", "r");
  bool found = false;
  size_t wl;

  if (f == NULL)
    {
      return false;
    }
  snprintf (want, sizeof (want), "cubrid_tcp_setup_server%ld", (long) pid);
  wl = strlen (want);
  while (!found && fgets (line, sizeof (line), f) != NULL)
    {
      const char *hit = strstr (line, want);

      /* the name is the last field; the pid must end there (123 is not 1234) */
      found = hit != NULL && (hit[wl] == '\n' || hit[wl] == '\0' || hit[wl] == ' ');
    }
  fclose (f);
  return found;
}

static const char *
volmap_basename_of (const char *s)
{
  const char *b = strrchr (s, '/');

  return (b != NULL) ? b + 1 : s;
}

/* the server and any maintenance utility of this database, from /proc */
static void
volmap_proc_scan (VOLMAP_CTX * ctx)
{
  DIR *d = opendir ("/proc");
  struct dirent *de;
  const char *dbn = (ctx->db_label != NULL) ? ctx->db_label : "";
  VOLMAP_PROC np;

  memset (&np, 0, sizeof (np));
  np.at = time (NULL);
  if (d == NULL || dbn[0] == '\0')
    {
      if (d != NULL)
	{
	  closedir (d);
	}
      ctx->proc = np;
      return;
    }
  np.scanned = true;
  while ((de = readdir (d)) != NULL)
    {
      char buf[4096], *argv[64];
      int argc, i;
      const char *a0;
      pid_t pid;

      if (de->d_name[0] < '1' || de->d_name[0] > '9')
	{
	  continue;
	}
      pid = (pid_t) atol (de->d_name);
      argc = volmap_cmdline (pid, buf, sizeof (buf), argv, 64);
      if (argc < 2)
	{
	  continue;
	}
      a0 = volmap_basename_of (argv[0]);
      if (strcmp (a0, "cub_server") == 0)
	{
	  if (np.server_pid == 0 && strcmp (argv[1], dbn) == 0)
	    {
	      np.server_pid = pid;
	    }
	  continue;
	}
      if (strcmp (a0, "cub_admin") != 0 && strcmp (a0, "cubrid") != 0)
	{
	  continue;
	}
      for (i = 0; volmap_util_names[i] != NULL; i++)
	{
	  if (strcmp (argv[1], volmap_util_names[i]) == 0)
	    {
	      break;
	    }
	}
      if (volmap_util_names[i] == NULL)
	{
	  continue;
	}
      /* the database is named by name or by path (restoredb takes paths too) */
      for (int k = 2; k < argc; k++)
	{
	  if (strcmp (argv[k], dbn) == 0 || strcmp (volmap_basename_of (argv[k]), dbn) == 0)
	    {
	      /* cubrid launches cub_admin for the real work: either one will do, the
	         earlier-started one tells how long the utility has been running */
	      long el = volmap_proc_elapsed (pid);

	      if (np.util[0] == '\0' || el > np.util_elapsed)
		{
		  snprintf (np.util, sizeof (np.util), "%s", volmap_util_names[i]);
		  np.util_pid = pid;
		  np.util_elapsed = el;
		}
	      break;
	    }
	}
    }
  closedir (d);
  np.log_lock_pid = -1;
  if (np.server_pid > 0)
    {
      int maj = 0, min = 0;

      np.server_elapsed = volmap_proc_elapsed (np.server_pid);
      /* the pid-named socket exists from 11.1; before that the name is random, so the
         registration cannot be observed and must not be read as "not registered" */
      if (sscanf (ctx->db_release, "%d.%d", &maj, &min) == 2 && (maj > 11 || (maj == 11 && min >= 1)))
	{
	  np.server_registered = volmap_server_registered (np.server_pid) ? 1 : 0;
	}
      else
	{
	  np.server_registered = -1;
	}
      /* every release mounts the active log with an fcntl write lock (fileio_mount,
         lock = true) at log initialization, before recovery: F_GETLK takes no lock and
         names the holder.  It says a server has the database open, not which phase. */
      {
	char lg[PATH_MAX];

	if (volmap_path_from_vinf (ctx->vinf_path, VOLMAP_LGAT_VOLID, lg, sizeof (lg)))
	  {
	    int fd = open (lg, O_RDONLY);

	    if (fd >= 0)
	      {
		struct flock fl;

		memset (&fl, 0, sizeof (fl));
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		np.log_lock_pid = (fcntl (fd, F_GETLK, &fl) == 0) ? ((fl.l_type == F_UNLCK) ? 0 : fl.l_pid) : -1;
		close (fd);
	      }
	  }
      }
    }
  ctx->proc = np;
}

/* one line about the server, for the report head and the status bar */
static void
volmap_server_state_str (const VOLMAP_CTX * ctx, char *out, size_t n)
{
  const VOLMAP_PROC *p = &ctx->proc;

  if (!p->scanned)
    {
      snprintf (out, n, "unknown (/proc not readable)");
    }
  else if (p->server_pid > 0 && p->server_registered == 1)
    {
      snprintf (out, n, "running (cub_server pid %ld, up %lds)", (long) p->server_pid, p->server_elapsed);
    }
  else if (p->server_pid > 0 && p->server_registered == -1)
    {
      snprintf (out, n, "running, unverified (cub_server pid %ld, up %lds; %s; before 11.1 the registration with"
		" cub_master is not observable, so booting/recovering and running are not told apart)",
		(long) p->server_pid, p->server_elapsed,
		p->log_lock_pid == p->server_pid ? "it holds the active log lock"
		: p->log_lock_pid > 0 ? "the active log is locked by another pid" : "the active log is not locked");
    }
  else if (p->server_pid > 0)
    {
      snprintf (out, n, "starting or recovering (cub_server pid %ld, %lds, not yet registered with cub_master)",
		(long) p->server_pid, p->server_elapsed);
    }
  else if (ctx->shutdown_state == 0)
    {
      snprintf (out, n, "no cub_server for this database on this host - the log says it did not shut down cleanly:"
		" restart recovery has not run (or it runs elsewhere)");
    }
  else
    {
      snprintf (out, n, "no cub_server for this database on this host");
    }
}

static const char *
volmap_server_state_name (const VOLMAP_CTX * ctx)
{
  const VOLMAP_PROC *p = &ctx->proc;

  return !p->scanned ? "unknown" : (p->server_pid > 0 && p->server_registered == 1) ? "running"
    : (p->server_pid > 0 && p->server_registered == -1) ? "running_unverified"
    : (p->server_pid > 0) ? "starting_or_recovering" : "not_running";
}

/* ---- the disk under the volumes: /proc/diskstats, and the server's own I/O ----------
 *
 * The device is the one volume 0 lives on (resolved like volmap_media_rotational).
 * Between two samples: io_ticks says how much of the interval the device had requests
 * in flight (busy %), time_in_queue the average queue depth, sectors the throughput.
 * Any account can read it.  In a container these counters are the host's: still the
 * disk the volumes are on, which is what matters for contention.  The server's own
 * share is read from /proc/<pid>/io when the account may (the file owner or root). */

/* major:minor of the block device holding `path`; -1 when unresolved (pseudo filesystems) */
static int
volmap_blockdev_of (const char *path, int *major_out, int *minor_out)
{
  struct stat st;
  char sys[256];

  if (path == NULL || stat (path, &st) != 0)
    {
      return -1;
    }
  *major_out = (int) major (st.st_dev);
  *minor_out = (int) minor (st.st_dev);
  snprintf (sys, sizeof (sys), "/sys/dev/block/%d:%d", *major_out, *minor_out);
  if (access (sys, F_OK) == 0)
    {
      return 0;
    }
  /* an overlay or other pseudo device (major 0): the mount's source device instead */
  {
    FILE *mi = fopen ("/proc/self/mountinfo", "r");
    char line[4096], best[256] = "";
    size_t bestlen = 0;

    if (mi == NULL)
      {
	return -1;
      }
    while (fgets (line, sizeof (line), mi))
      {
	char mp[1024], src[256];
	int maj = -1, min = -1;
	char *sep = strstr (line, " - ");

	if (sep == NULL || sscanf (line, "%*d %*d %d:%d %*s %1023s", &maj, &min, mp) != 3)
	  {
	    continue;
	  }
	if (sscanf (sep, " - %*s %255s", src) != 1 || src[0] != '/')
	  {
	    continue;
	  }
	{
	  size_t l = strlen (mp);

	  if (l > bestlen && strncmp (path, mp, l) == 0 && (mp[l - 1] == '/' || path[l] == '/' || path[l] == '\0'))
	    {
	      bestlen = l;
	      snprintf (best, sizeof (best), "%s", src);
	    }
	}
      }
    fclose (mi);
    if (best[0] != '\0' && stat (best, &st) == 0 && S_ISBLK (st.st_mode))
      {
	*major_out = (int) major (st.st_rdev);
	*minor_out = (int) minor (st.st_rdev);
	return 0;
      }
  }
  return -1;
}

/* read the diskstats row of the device */
static bool
volmap_diskstats_row (int major_n, int minor_n, char *name, size_t nn, unsigned long long *rd_sect,
		      unsigned long long *wr_sect, unsigned long long *io_ticks, unsigned long long *in_queue)
{
  FILE *f = fopen ("/proc/diskstats", "r");
  char line[512];
  bool found = false;

  if (f == NULL)
    {
      return false;
    }
  while (!found && fgets (line, sizeof (line), f) != NULL)
    {
      int maj, min;
      unsigned long long v[11];
      char nm[32];

      if (sscanf (line, "%d %d %31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &maj, &min, nm,
		  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]) != 14)
	{
	  continue;
	}
      if (maj == major_n && min == minor_n)
	{
	  snprintf (name, nn, "%s", nm);
	  *rd_sect = v[2];
	  *wr_sect = v[6];
	  *in_queue = v[10];
	  *io_ticks = v[9];
	  found = true;
	}
    }
  fclose (f);
  return found;
}

static void
volmap_io_begin (VOLMAP_CTX * ctx)
{
  VOLMAP_IO *io = &ctx->io;

  memset (io, 0, sizeof (*io));
  io->srv_ok = -1;
  if (ctx->nvols == 0 || volmap_blockdev_of (ctx->vols[0].path, &io->major_n, &io->minor_n) != 0)
    {
      io->state = -1;
      return;
    }
  io->state = 1;
  volmap_io_sample (ctx);	/* the first sample only sets the baseline */
}

static void
volmap_io_sample (VOLMAP_CTX * ctx)
{
  VOLMAP_IO *io = &ctx->io;
  unsigned long long rd, wr, ticks, q;
  struct timespec now;
  double ms;

  if (io->state != 1)
    {
      return;
    }
  if (!volmap_diskstats_row (io->major_n, io->minor_n, io->dev, sizeof (io->dev), &rd, &wr, &ticks, &q))
    {
      io->state = -1;		/* no row for this device: unknown rather than another disk's figures */
      return;
    }
  clock_gettime (CLOCK_MONOTONIC, &now);
  if (io->t.tv_sec != 0)
    {
      ms = (now.tv_sec - io->t.tv_sec) * 1000.0 + (now.tv_nsec - io->t.tv_nsec) / 1e6;
      if (ms >= 50)
	{
	  io->util_pct = 100.0 * (double) (UINT32) (ticks - io->io_ticks) / ms;	/* 32-bit in the kernel */
	  if (io->util_pct > 100.0)
	    {
	      io->util_pct = 100.0;
	    }
	  io->queue = (double) (UINT32) (q - io->in_queue) / ms;
	  io->mb_s = ((double) (rd - io->rd_sect) + (double) (wr - io->wr_sect)) * 512.0 / (ms / 1000.0) / 1e6;
	  io->busy = io->util_pct >= VOLMAP_IO_BUSY_PCT;
	}
      else
	{
	  return;		/* too soon for a rate: keep the previous figures */
	}
    }
  io->t = now;
  io->io_ticks = ticks;
  io->in_queue = q;
  io->rd_sect = rd;
  io->wr_sect = wr;
  /* the server's share, when the account may read it */
  if (ctx->proc.server_pid > 0)
    {
      char path[64], line[128];
      FILE *f;
      unsigned long long srd = 0, swr = 0;
      int got = 0;

      snprintf (path, sizeof (path), "/proc/%ld/io", (long) ctx->proc.server_pid);
      f = fopen (path, "r");
      if (f != NULL)
	{
	  while (fgets (line, sizeof (line), f) != NULL)
	    {
	      got += sscanf (line, "read_bytes: %llu", &srd) == 1;
	      got += sscanf (line, "write_bytes: %llu", &swr) == 1;
	    }
	  fclose (f);
	}
      if (got == 2)
	{
	  if (io->srv_ok == 1 && ms >= 50)
	    {
	      io->srv_mb_s = ((double) (srd - io->srv_rd) + (double) (swr - io->srv_wr)) / (ms / 1000.0) / 1e6;
	    }
	  io->srv_rd = srd;
	  io->srv_wr = swr;
	  io->srv_ok = 1;
	}
      else
	{
	  io->srv_ok = 0;
	}
    }
}

/* A batch run can be over in a few milliseconds, too short for a rate: the final sample
   waits until the window since the baseline is at least 200ms (once per run) */
static void
volmap_io_window (VOLMAP_CTX * ctx)
{
  VOLMAP_IO *io = &ctx->io;
  struct timespec now;
  double ms;

  if (io->state != 1 || io->t.tv_sec == 0)
    {
      return;
    }
  if (io->util_pct > 0.0 || io->mb_s > 0.0)
    {
      volmap_io_sample (ctx);	/* already measured once: just extend */
      return;
    }
  clock_gettime (CLOCK_MONOTONIC, &now);
  ms = (now.tv_sec - io->t.tv_sec) * 1000.0 + (now.tv_nsec - io->t.tv_nsec) / 1e6;
  if (ms < 200)
    {
      struct timespec ts;

      ts.tv_sec = 0;
      ts.tv_nsec = (long) ((200 - ms) * 1e6);
      nanosleep (&ts, NULL);
    }
  volmap_io_sample (ctx);
}

/* the report head: server, utility, disk */
static void
volmap_print_environment (VOLMAP_CTX * ctx, FILE * fp)
{
  char st[256];

  volmap_server_state_str (ctx, st, sizeof (st));
  fprintf (fp, "  server: %s\n", st);
  if (ctx->proc.util[0] != '\0')
    {
      fprintf (fp, "  maintenance: cubrid %s is running on this database (pid %ld, %lds) - the map is the disk as that run"
	       " leaves it\n", ctx->proc.util, (long) ctx->proc.util_pid, ctx->proc.util_elapsed);
    }
  if (ctx->io.state == 1 && ctx->io.t.tv_sec != 0)
    {
      fprintf (fp, "  disk: %s busy %.0f%% during this run, %.1f MB/s%s", ctx->io.dev, ctx->io.util_pct, ctx->io.mb_s,
	       ctx->io.busy ? " (busy: another load is using it)" : "");
      if (ctx->io.srv_ok == 1)
	{
	  fprintf (fp, "; cub_server %.1f MB/s", ctx->io.srv_mb_s);
	}
      fprintf (fp, "\n");
    }
}

/* -i: should the scan and the auto refresh hold?  A maintenance utility is running on
   this database, or the server is still booting/recovering.  The reason, for the bar. */
static bool
volmap_hold_reason (const VOLMAP_CTX * ctx, char *out, size_t n)
{
  const VOLMAP_PROC *p = &ctx->proc;

  if (p->util[0] != '\0')
    {
      long m = p->util_elapsed / 60, s = p->util_elapsed % 60;

      if (p->util_elapsed >= 0)
	{
	  snprintf (out, n, "%s running (pid %ld, %ldm%02lds)", p->util, (long) p->util_pid, m, s);
	}
      else
	{
	  snprintf (out, n, "%s running (pid %ld)", p->util, (long) p->util_pid);
	}
      return true;
    }
  if (p->server_pid > 0 && p->server_registered == 0)
    {
      /* only when the absence is meaningful (11.1+); an older server is never held for this */
      snprintf (out, n, "cub_server starting or recovering (pid %ld, %lds)", (long) p->server_pid, p->server_elapsed);
      return true;
    }
  out[0] = '\0';
  return false;
}

static void
volmap_status_bar_draw (VOLMAP_CTX * ctx, char *status2, int cols, int rows, bool auto_refresh)
{
  int maxw = cols - 2;
  int li = 0;
  char *hint = strchr (status2, '\x01');

  /* separator row, fixed position */
  printf ("\033[0m\033[%d;1H\033[2K", rows - 3);

  /* row 1 break: at the hint if what precedes it fits, otherwise on width */
  if (hint != NULL)
    {
      int hw;
      char save = *hint;

      *hint = '\0';
      (void) volmap_disp_clip (status2, -1, &hw);
      *hint = save;
      if (hw <= maxw)
	{
	  li = (int) (hint - status2);
	  status2[li] = ' ';	/* the hint has done its job; make sure it leaks through no output path */
	  li++;
	  hint = NULL;
	}
    }
  if (hint != NULL || li == 0)
    {
      li = volmap_disp_clip (status2, maxw, NULL);
    }
  {
    char *leak;

    while ((leak = strchr (status2, '\x01')) != NULL)	/* blank out any remaining hints (there may be more than one) */
      {
	*leak = ' ';
      }
  }
  printf ("\033[%d;1H\033[2K %.*s\033[0m\r\n", rows - 2, li, status2);

  /* row 2: same rule, starting where row 1 ended */
  {
    const char *p2 = status2 + li;
    int li2 = volmap_disp_clip (p2, maxw, NULL);

    printf ("\033[2K %.*s\033[0m\r\n", li2, p2);
  }

  /* key list */
  {
    char l3[256];

    if (volmap_lang_ko)
      {
	/* on an 80-column screen [bksp] and [<>] are folded away; the keys still work */
	snprintf (l3, sizeof (l3),
		  cols < 100
		  ? " [sp/rtn/tab]%s [1-3]%s [f]%s [r]%s [a]%s%s [m]%s%s [l]English [h]%s [q]%s"
		  : " [sp/rtn/tab]%s [1-3]%s [f]%s [bksp]\xec\x9c\x84\xeb\xa1\x9c [<>]\xeb\xb3\xbc\xeb\xa5\xa8\xed\x8c\x8c\xec\x9d\xbc [r]%s [a]%s%s [m]%s%s [l]English [h]%s [q]%s",
		  "\xeb\x93\x9c\xeb\xa6\xb4\xeb\x8b\xa4\xec\x9a\xb4", "\xeb\xb0\x95\xec\x8a\xa4", "\xed\x8c\x8c\xec\x9d\xbc",
		  "\xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8", "\xec\x9e\x90\xeb\x8f\x99", auto_refresh ? ":ON" : "",
		  "\xec\x83\x81\xec\xa3\xbc", ctx->residency ? ":ON" : "",
		  "\xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90", "\xec\xa2\x85\xeb\xa3\x8c");
      }
    else
      {
	snprintf (l3, sizeof (l3),
		  cols < 100
		  ? " [sp/rtn/tab]drill-down [1-3]box [f]iles [r]efresh [a]uto%s [m]resid%s [l]\xed\x95\x9c\xea\xb8\x80 [h]elp [q]uit"
		  : " [sp/rtn/tab]drill-down [1-3]box [f]iles [bksp]up [<>]volfile [r]efresh [a]uto%s [m]resid%s [l]\xed\x95\x9c\xea\xb8\x80 [h]elp [q]uit",
		  auto_refresh ? ":ON" : "", ctx->residency ? ":ON" : "");
      }
    printf ("\033[2K\033[2m%.*s\033[0m", cols - 1, l3);
  }
}

/* Map frame border, shared by top and bottom.
   Top is "<corner>|left|---|right|<corner>", bottom the same shape, so one
   function draws both.  An inset that is NULL or "" is omitted.  The caller
   picks the left inset colour (top: db name, cyan bold; bottom: time, grey). */
static void
volmap_frame_border_draw (int mapw, bool bottom, const char *lins, const char *lcol, const char *rins)
{
  const char *B = "\033[38;5;240m";
  int lw = (lins != NULL && lins[0] != '\0') ? volmap_disp_w (lins) : 0;
  int rw = (rins != NULL && rins[0] != '\0') ? volmap_disp_w (rins) : 0;
  int dashes = mapw + 2 - (lw > 0 ? lw + 4 : 0) - (rw > 0 ? rw + 4 : 0);
  int bi;

  if (dashes < 1)
    {
      dashes = 1;		/* when narrow shorten only the dashes and keep the insets */
    }
  printf ("\033[2K%s%s", B, bottom ? OVG ("\xe2\x95\xb0", "+") : OVG ("\xe2\x95\xad", "+"));
  if (lw > 0)
    {
      printf ("%s%s %s %s%s", OVG ("\xe2\x94\xa4", "["), lcol != NULL ? lcol : "", lins, B, OVG ("\xe2\x94\x9c", "]"));
    }
  for (bi = 0; bi < dashes; bi++)
    {
      printf ("%s", OVG ("\xe2\x94\x80", "-"));
    }
  if (rw > 0)
    {
      printf ("%s\033[38;5;250m %s %s%s", OVG ("\xe2\x94\xa4", "["), rins, B, OVG ("\xe2\x94\x9c", "]"));
    }
  printf ("%s\033[0m", bottom ? OVG ("\xe2\x95\xaf", "+") : OVG ("\xe2\x95\xae", "+"));
  if (!bottom)
    {
      printf ("\r\n");
    }
}

/* Top bar: legend row, volume summary row, blank separator, map top border.
   Narrow screens (<100 columns) use the abbreviated form.  The statistics inset
   is built here and handed to the border function. */
static void
volmap_top_bar_draw (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, int vi, int cols, int mapw,
		     long per_pages, long res, INT64 alloc, long switches)
{
  char hsz[32], csz[32];
  bool narrow = (cols < 100);

  volmap_human ((INT64) vol->nsect_total * VOLMAP_SECT_NPAGES * vol->iopagesize, hsz, sizeof (hsz));
  volmap_human ((INT64) per_pages * vol->iopagesize, csz, sizeof (csz));

  /* row 1: legend */
  if (volmap_lang_ko)
    {
      printf ("\033[H\033[2K"
	      "\033[31m\xe2\xa0\xbf\033[0m \xec\xb9\xb4\xed\x83\x88\xeb\xa1\x9c\xea\xb7\xb8(\xec\x8b\x9c\xec\x8a\xa4\xed\x85\x9c)  "
	      "\033[34m\xe2\xa0\xbf\033[36m\xe2\xa0\xbf\033[0m \xeb\x8d\xb0\xec\x9d\xb4\xed\x84\xb0  "
	      "\033[32m\xe2\xa0\xbf\033[35m\xe2\xa0\xbf\033[0m \xec\x9d\xb8\xeb\x8d\xb1\xec\x8a\xa4  "
	      "\xe2\xa0\xbf\xe2\xa0\xbe\xe2\xa0\xb6\xe2\xa0\xb4\xe2\xa0\xa4=\xed\x95\xa0\xeb\x8b\xb9\xeb\xa5\xa0 100..20%% "
	      "\033[38;5;208m\xe2\xa0\x82\033[0m \xec\x98\x88\xec\x95\xbd-\xeb\xb9\x88 "
	      "\033[38;5;242m\xe2\xa0\x82\033[0m \xeb\xaf\xb8\xec\x98\x88\xec\x95\xbd "
	      "\033[38;5;238m#\033[0m \xeb\xa9\x94\xed\x83\x80%s\r\n",
	      narrow ? "" : " \033[2m(\xed\x86\xa4 \xeb\xb0\x98\xec\xa0\x84 = \xeb\x8b\xa4\xeb\xa5\xb8 \xed\x8c\x8c\xec\x9d\xbc)\033[0m");
    }
  else
    {
      /* catalog uses one glyph; data and index use two tones to show file
	 boundaries.  The fill ramp is 5 steps with percentages when wide, 3 steps
	 when narrow. */
      printf ("\033[H\033[2K"
	      "\033[31m\xe2\xa0\xbf\033[0m catalog(sys)  "
	      "\033[34m\xe2\xa0\xbf\033[36m\xe2\xa0\xbf\033[0m data  "
	      "\033[32m\xe2\xa0\xbf\033[35m\xe2\xa0\xbf\033[0m index  "
	      "%s"
	      "\033[38;5;208m\xe2\xa0\x82\033[0m resv-empty "
	      "\033[38;5;242m\xe2\xa0\x82\033[0m unreserved "
	      "\033[38;5;238m#\033[0m meta%s\r\n",
	      narrow ? "\xe2\xa0\xbf\xe2\xa0\xb6\xe2\xa0\xa4=alloc "
	      : "\xe2\xa0\xbf\xe2\xa0\xbe\xe2\xa0\xb6\xe2\xa0\xb4\xe2\xa0\xa4=alloc 100..20% ",
	      narrow ? "" : " \033[2m(tone change = other file)\033[0m");
    }

  /* row 2: volume summary */
  {
    char scanmsg[320] = "", zsfx[96], prog[24] = "", hr[160];

    if (ctx->scan_active && ctx->scan_total > 0)
      {
	snprintf (scanmsg, sizeof (scanmsg), "  \033[33mscanning %d%%\033[0;1m",
		  (int) (100 * ctx->scan_probed / ctx->scan_total));
      }
    /* what runs beside the database, and what this tool does about it */
    if (ctx->hold && volmap_hold_reason (ctx, hr, sizeof (hr)))
      {
	size_t sl = strlen (scanmsg);

	snprintf (scanmsg + sl, sizeof (scanmsg) - sl, "  \033[33m\xe2\x8f\xb8 %s - scan/refresh paused, r = continue\033[0;1m", hr);
      }
    else if (ctx->io.busy)
      {
	size_t sl = strlen (scanmsg);

	snprintf (scanmsg + sl, sizeof (scanmsg) - sl, "  \033[33mdisk %s busy %.0f%%%s\033[0;1m",
		  ctx->io.dev, ctx->io.util_pct, ctx->scan_active ? " - scanning slowly" : "");
      }
    {
      /* Also show how many sectors the cell covers.  It matches the sector count the
	 drill-down actually draws, so the two screens can be cross-checked by eye.
	 Cells are sized in units of 16 pages and so are not an integer multiple of a
	 sector (e.g. 160 pages = 2.5 sectors); the value is rounded up and marked
	 approximate. */
      long nsec = (per_pages + VOLMAP_SECT_NPAGES - 1) / VOLMAP_SECT_NPAGES;

      snprintf (zsfx, sizeof (zsfx), "(cell: %ld pages = %s, %s%ld sect)", per_pages, csz,
		(per_pages % VOLMAP_SECT_NPAGES) ? "~" : "", (nsec > 0) ? nsec : 1);
    }
    if (!narrow)
      {
	snprintf (prog, sizeof (prog), "[%3d/%d] ", vi + 1, ctx->nvols);
      }
    printf (narrow
	    ? "\033[2K\033[1m%svolid %d  %s  sect %ld/%d resv (x64=%lld pg)  %s%s\033[0m\r\n"
	    : "\033[2K\033[1m%svolid %d  %s  sectors %ld/%d reserved (x64 = %lld pages)  %s%s\033[0m\r\n",
	    prog, vol->volid, hsz, res, vol->nsect_total, (long long) res * VOLMAP_SECT_NPAGES, zsfx, scanmsg);
  }

  /* row 3: blank separator; row 4: map top border (left = db name, right = allocation statistics) */
  {
    char resinfo[128] = "", stats[256] = "";
    const char *dbn = (ctx->db_label != NULL) ? ctx->db_label : "?";
    int room;

    if (ctx->residency && vol->respg != NULL)
      {
	long npv = (long) vol->nsect_total * VOLMAP_SECT_NPAGES;
	int sub = vol->iopagesize / 4096;

	snprintf (resinfo, sizeof (resinfo), "  resident %.1f%%",
		  npv > 0 ? 100.0 * vol->res_total / ((double) npv * sub) : 0.0);
      }
    else if (ctx->residency && vol->res_untrusted)
      {
	snprintf (resinfo, sizeof (resinfo), "  resident unknown (needs file owner or write permission)");
      }
    if (ctx->bufmap && ctx->bufmap_loaded && vol->bufpg != NULL)
      {
	size_t rl = strlen (resinfo);

	char fl[40] = "", fr[40] = "";

	if (vol->buf_flushing > 0)
	  {
	    snprintf (fl, sizeof (fl), ", flushing %ld", vol->buf_flushing);
	  }
	if (vol->buf_freed > 0)
	  {
	    snprintf (fr, sizeof (fr), ", freed %ld", vol->buf_freed);
	  }
	snprintf (resinfo + rl, sizeof (resinfo) - rl, "  buf %ld pages (unflushed %ld%s%s)", vol->buf_total, vol->buf_dirty, fl, fr);
      }
    {
      INT64 idlep = res * (INT64) VOLMAP_SECT_NPAGES - alloc;
      char isz[32];

      volmap_human ((idlep > 0 ? idlep : 0) * vol->iopagesize, isz, sizeof (isz));
      if (ctx->scan_active)
	{
	  /* ownership is still being attributed: an idle figure now would be
	   * grossly overstated and misread as waste */
	  snprintf (stats, sizeof (stats),
		    narrow ? "pg %lld resv / %lld alloc  (idle ?)  o-switch %ld%s"
		    : "pages %lld reserved / %lld allocated  (idle ?)  owner switches %ld%s",
		    (long long) res * VOLMAP_SECT_NPAGES, (long long) alloc, switches, resinfo);
	}
      else
	{
	  snprintf (stats, sizeof (stats),
		    narrow ? "pg %lld resv / %lld alloc  (idle %lld pg = %s)  o-switch %ld%s"
		    : "pages %lld reserved / %lld allocated  (idle %lld pages = %s)  owner switches %ld%s",
		    (long long) res * VOLMAP_SECT_NPAGES, (long long) alloc,
		    (long long) (idlep > 0 ? idlep : 0), isz, switches, resinfo);
	}
    }
    printf ("\033[2K\r\n");	/* the old stats row stays BLANK (separation) */
    /* clip the statistics inset by display width so it does not collide with the db name inset */
    room = mapw - 10 - volmap_disp_w (dbn);
    if (room > 0 && volmap_disp_w (stats) > room)
      {
	int cut = volmap_disp_clip (stats, room, NULL);

	stats[cut] = '\0';
	while (cut > 0 && stats[cut - 1] == ' ')
	  {
	    stats[--cut] = '\0';	/* trim trailing space left by clipping */
	  }
      }
    volmap_frame_border_draw (mapw, false, dbn, "\033[1;36m", stats);
  }
}

/* Map grid driver.  Per-cell work (classify / paint / put_cell) is already
   factored out; this is the loop over them plus the parallel array updates.
   The arrays stay owned by the caller. */
static void
volmap_map_grid_draw (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, int mapw, int ncells, long per_pages,
		      long total_pages, int cur_x, int cur_y, int *cell_owner, char *cell_state,
		      long *cell_p0, long *cell_alloc_arr, char (*cell_render)[VOLMAP_CELLSTR])
{
  int prev_maj = -1, tone = 0;
  int c;

  volmap_vol_hwm_update (vol);
  for (c = 0; c < ncells; c++)
    {
      VOLMAP_CELL cell;
      bool at_cursor = (c == cur_y * mapw + cur_x);
      char *cr = cell_render[c];

      volmap_row_break (ctx, NULL, c, mapw);
      volmap_cell_classify (ctx, vol, (long) c * per_pages, per_pages, total_pages, &prev_maj, &tone, true, &cell);
      cell_owner[c] = cell.maj;
      cell_state[c] = cell.state;
      cell_p0[c] = cell.p0;
      cell_alloc_arr[c] = cell.nalloc;
      volmap_cell_paint (&cell, ctx, false, VOLMAP_PAINT_LEAD_RESET, cr, VOLMAP_CELLSTR);
      volmap_put_cell (cr, at_cursor);
    }
}

/* map bottom border: left = timestamp, right = volume file identity */
static void
volmap_frame_bottom_draw (VOLMAP_VOLUME * vol, int mapw)
{
  char vt[192], ts[32] = "";
  const char *vclass;
  size_t plen = strlen (vol->path);
  const char *base = strrchr (vol->path, '/');
  const char *tp = (base != NULL) ? strstr (base, "_t") : NULL;
  bool is_tt = false;
  time_t nowt = time (NULL);
  struct tm tmv;
  int tl, keep;

  /* volume class before the number: perm (permanent data), perm temp
   * (permanent file with temporary purpose), temp temp (runtime <db>_t<N>) */
  if (tp != NULL && tp[2] >= '0' && tp[2] <= '9')
    {
      size_t d = (size_t) (tp + 2 - vol->path);

      while (d < plen && vol->path[d] >= '0' && vol->path[d] <= '9')
	{
	  d++;
	}
      is_tt = (d == plen);	/* basename ends with _t<digits>: runtime temp file */
    }
  vclass = is_tt ? "temp temp" : (vol->purpose == DB_TEMPORARY_DATA_PURPOSE) ? "perm temp" : "perm";
  snprintf (vt, sizeof (vt), "%s vol %d: %s", vclass, (int) vol->volid, vol->path);

  /* timestamp inset: when the screen was taken, for reviewing captures and spotting a stalled auto-refresh */
  if (localtime_r (&nowt, &tmv) != NULL)
    {
      strftime (ts, sizeof (ts), "%m-%d %H:%M:%S", &tmv);
    }
  /* for a long path keep the tail: the file name matters more than the directory */
  keep = mapw - 6 - (ts[0] != '\0' ? (int) strlen (ts) + 4 : 0);
  tl = (int) strlen (vt);
  if (keep > 4 && tl > keep)
    {
      memmove (vt, vt + (tl - keep), (size_t) keep + 1);
      vt[0] = vt[1] = '.';
    }
  /* close the last grid row's right border, break the line, then draw the bottom border */
  printf ("\033[0m \033[38;5;240m%s\r\n", OVG ("\xe2\x94\x82", "|"));
  volmap_frame_border_draw (mapw, true, ts, "\033[38;5;245m", vt);
}

static void
volmap_interactive (VOLMAP_CTX * ctx)
{
  struct termios term_old, term_raw;
  int vi = 0;
  int vskip;
  int *cell_owner = NULL;	/* mapw*maph hit-test buffers */
  char *cell_state = NULL;	/* 'F'=file, '_'=empty-res, '.'=unres, '#'=meta */
  long *cell_p0 = NULL;
  long *cell_alloc_arr = NULL;
  long per_pages = 1;
  int mapw = 80, maph = 20;
  int cur_x = 0, cur_y = 0;	/* keyboard cursor */
  bool auto_refresh = false;
  /* Positional initializer: every member is listed, so adding one to the struct and
     not listing it here is caught by -Wmissing-field-initializers rather than
     silently shifting the values. */
  VOLMAP_PANEL panel = { 0, 0, NULL, NULL, NULL, NULL, -1, -1, -1, "", -1, 0, { 0, 0 }, -1, 0,
                         { "", "" }, 0, 0, 0, 0, 0 };
  VOLMAP_PANEL panelb = { 0, 0, NULL, NULL, NULL, NULL, -1, -1, -1, "", -1, 0, { 0, 0 }, -1, 0,
                          { "", "" }, 0, 0, 0, 0, 0 };
  char status2[512] = "new here? h = guide(\xed\x99\x94\xeb\xa9\xb4 \xed\x95\xb4\xec\x84\x9d \xea\xb0\x80\xec\x9d\xb4\xeb\x93\x9c)  |  l = \xed\x95\x9c\xea\xb8\x80  |  arrows move, space inspect, enter zoom";
  int prev_vi = -1, prev_cx = 0, prev_cy = 0;	/* last painted frame */
  int prev_cols = 0, prev_rows = 0;
  int alloc_ncells = 0;
  bool force_full = true;
  bool panel_on = false;	/* drill-down overlay: opens on space (or tab) only - a mouse click
				 * just moves the cursor and inspects, it never pops the overlay */
  bool file_view_on = false;	/* [f]: file list overlay - volume -> FILE -> sector browsing */
  int fv_sel = 0;		/* selected row in the file list */
  int *fv_files = NULL;		/* file indices owning sectors in this volume, first-sector order */
  long *fv_first = NULL;	/* first owned sector per listed file */
  int *fv_nsect = NULL;		/* owned sector count per listed file */
  int fv_n = 0, fv_cap = 0;
  int fv_last_cc = -1;		/* map cell the list selection was last synced to */
  long fv_sync_pg = -1;		/* opened FROM the drill-down: preselect this page's owning file */
  int ov_focus = 0;		/* 0=map, 1=sector grid, 2=page byte cells, 3=slots (keys 1/2/3 jump) */
  int ov_byte = 64;		/* selected byte cell in the page box (panelb cell index) */
  char ov_live[512] = "";	/* live inspection of whatever the arrows are on (bottom bar) */
  char ov_pin[512] = "";	/* page-level info pinned when committing into the page (bottom bar at F2/F3) */
  char ov_heap[512] = "";	/* live info from INSIDE the page (byte cells / slots) - shown in the info box */
  long ov_sect_off = 0;		/* sector offset inside the map cell (multi-sector cells: grid edges cross over) */
  int ov_slot = 0;		/* selected slot when ov_focus == 2 */
  long ov_anchor_pg = -1;	/* page the overlay described last frame: a change resets slot/byte picks */

  int l0_psel = -1;		/* stacked panel: selected page index within the cursor sector */
  DKNSECTS l0_psel_sect = (DKNSECTS) - 1;
  int l0_psel_cell = -1;	/* the map cell the selection is anchored to; a cell change re-picks the selection */
  long l0_psel_per = -1;	/* Cell size (pages) at that time.  A resize changes the cell size, so the same
				   cell index covers a different range and the anchor must be re-picked. */
  long l0_anchor_pg = -1;	/* the absolute page picked in that cell (a sector+offset pair drifts on recomputation) */
  char (*cell_render)[VOLMAP_CELLSTR] = NULL;	/* exact bytes last painted per map cell */

  if (!isatty (fileno (stdout)))
    {
      fprintf (stderr, "volmap: -i requires a terminal\n");
      return;
    }

  /* a tty stdout flushes every ~1KB, splitting each frame into dozens of write()s;
   * a boundary that lands inside a multi-byte glyph is rendered as U+FFFD by some
   * terminals (a vertical stripe of ghost glyphs, since rows have similar byte
   * lengths).  Full buffering stops those arbitrary flushes; volmap_row_sync()
   * then flushes ONLY at row boundaries (glyph-complete) and drains the pty so
   * downstream readers cannot re-split the frame mid-glyph either. */
  {
    static char frame_buf[1 << 20];	/* holds the largest frame (240x200 SGR cells) whole */

    fflush (stdout);
    setvbuf (stdout, frame_buf, _IOFBF, sizeof (frame_buf));
  }

  if (ctx->vol_filter_on)
    {
      /* -V: start on the first selected volume */
      for (vskip = 0; vskip < ctx->nvols; vskip++)
	{
	  VOLMAP_VOLUME *v0 = &ctx->vols[vskip];

	  if (volmap_vol_selected (ctx, v0->volid))
	    {
	      vi = vskip;
	      break;
	    }
	}
    }

  tcgetattr (0, &term_old);
  volmap_term_saved = term_old;
  volmap_term_saved_ok = 1;
  (void) signal (SIGINT, volmap_on_signal);
  (void) signal (SIGTERM, volmap_on_signal);
  (void) signal (SIGHUP, volmap_on_signal);
  (void) signal (SIGWINCH, volmap_on_winch);	/* redraw immediately on a resize */
  term_raw = term_old;
  term_raw.c_lflag &= ~(ICANON | ECHO);
  term_raw.c_cc[VMIN] = 1;
  term_raw.c_cc[VTIME] = 0;
  tcsetattr (0, TCSANOW, &term_raw);
  /* NO alternate screen: the last frame stays on the terminal after quitting,
   * so what was being inspected remains visible.  Hide cursor, autowrap OFF
   * (a wrapped bottom line scrolls the whole screen), SGR mouse reporting. */
  /* PRESERVE whatever the shell had on screen: push it into the scrollback
   * with plain newlines from the bottom row instead of erasing it - the
   * user's prior output stays scrollable above, and quitting still leaves
   * the last frame visible (no alternate screen, by design). */
  {
    struct winsize ws0;
    int r0 = 40;

    if (ioctl (1, TIOCGWINSZ, &ws0) == 0 && ws0.ws_row > 0)
      {
	r0 = ws0.ws_row;
      }
    printf ("\033[9999;1H");
    for (; r0 > 0; r0--)
      {
	putchar ('\n');
      }
  }
  /* a light terminal background washes the map colors out: switch the DEFAULT
   * background to near-black for the session (OSC 11) - the clear right after
   * (now over already-scrolled-away content) repaints the screen with it.
   * Restored on every exit path with OSC 111 (= reset to the user's
   * configured default).  Terminals without OSC 11 support ignore both. */
  printf ("\033]11;#101010\007");
  printf ("\033[2J\033[?25l\033[?7l\033[?1000h\033[?1006h");
  fflush (stdout);
  volmap_mt_start (ctx);	/* batch + low-latency worker lanes (scan/refresh off this thread) */
  {
    char hr[160];

    ctx->hold = volmap_hold_reason (ctx, hr, sizeof (hr));	/* sampled before the loop by the caller */
  }

  for (;;)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      struct winsize ws;
      long total_pages = (long) vol->nsect_total * VOLMAP_SECT_NPAGES;

      /* Drawing AND the key handling that follows read stab/owner/alloc/pagebm, so a
       * commit landing anywhere in between would mix a new reservation with an old
       * owner.  The flag is held across the whole iteration and dropped only around
       * select(), the one point where this thread holds no array pointer.  Taking
       * pub_mx to raise it means a frame starts either before the swap or after it,
       * never inside: the worker holds the same lock while it swaps. */
      pthread_mutex_lock (&volmap_mt.pub_mx);
      volmap_mt.ui_reading = 1;
      pthread_mutex_unlock (&volmap_mt.pub_mx);
      long res = 0;
      INT64 alloc = 0;
      long switches = 0;
      DKNSECTS sct;
      int ncells;
      bool full;

      if (ioctl (fileno (stdout), TIOCGWINSZ, &ws) != 0 || ws.ws_col < 20 || ws.ws_row < 10)
	{
	  ws.ws_col = 80;
	  ws.ws_row = 24;
	}
      mapw = (ws.ws_col > 240) ? 240 : ws.ws_col;
      /* The map uses the full screen width.  The drill-down goes outside to the
	 right only when spare margin exists; the map is never shrunk to make room.
	 Without margin it is drawn as a window floating inside the map. */
      mapw -= 4;		/* rounded frame around the map: "| " left + " |" right */
      mapw -= mapw % 8;		/* snap to 8-column steps: a safety margin against width drift */
      if (mapw < 8)
	{
	  mapw = 8;
	}
      maph = ws.ws_row - 9;	/* legend + header + alloc + top border + bottom border + 3 status + 1 spare */
      /* Map size follows the screen only.
	 The viewer's width and height are the same whatever the volume size;
	 the database size shows up solely in per_pages - how many pages one cell
	 represents, i.e. the resolution. */
      if (maph < 1)
	{
	  maph = 1;		/* an 8-row terminal must not divide by zero below */
	}
      ncells = mapw * maph;
      per_pages = (total_pages + ncells - 1) / ncells;
      /* Snap the cell size to a clean multiple - 16 pages (256KB) rather than values
	 like "173 pages = 2.7MB".
	 The floor is 16 pages, not one sector (64): with a 64-page floor a small
	 volume runs out of cells and the map stops short of filling the screen.
	 (Sector-boundary snapping is a drill-down rule, not a map rule.) */
      per_pages = ((per_pages + VOLMAP_MAP_CELL_STEP - 1) / VOLMAP_MAP_CELL_STEP) * VOLMAP_MAP_CELL_STEP;
      if (per_pages < 1)
	{
	  per_pages = 1;
	}
      /* a shrinking terminal must not leave any cursor beyond the new grid */
      cur_x = (cur_x >= mapw) ? mapw - 1 : cur_x;
      cur_y = (cur_y >= maph) ? maph - 1 : cur_y;
      {
	/* the grid is larger than the volume: the trailing cells are never drawn,
	 * so the cursor must not sit there (invisible selection, empty overlay) */
	long nvalid = (total_pages + per_pages - 1) / per_pages;

	if ((long) cur_y * mapw + cur_x >= nvalid && nvalid > 0)
	  {
	    cur_y = (int) ((nvalid - 1) / mapw);
	    cur_x = (int) ((nvalid - 1) % mapw);
	  }
      }
      if (volmap_chain_volid >= 0 && volmap_chain_volid != vol->volid)
	{
	  volmap_chain_volid = -1;	/* the bitmap maps another volume */
	  force_full = true;
	}
      if (!panel_on && !file_view_on && (volmap_mark_file >= 0 || volmap_mark_class_on))
	{
	  volmap_mark_file = -1;	/* drill-down closed: the marker goes with it */
	  volmap_mark_class_on = 0;
	  force_full = true;
	}
      if (file_view_on)
	{
	  /* files owning sectors in THIS volume, in first-sector (physical) order */
	  static int *seenrow = NULL;
	  static int seen_cap = 0;
	  DKNSECTS s9;

	  if (seen_cap < ctx->nfiles)
	    {
	      free (seenrow);
	      seen_cap = ctx->nfiles + 64;
	      seenrow = (int *) malloc ((size_t) seen_cap * sizeof (int));
	    }
	  if (fv_cap < ctx->nfiles)
	    {
	      free (fv_files);
	      free (fv_first);
	      free (fv_nsect);
	      fv_cap = ctx->nfiles + 64;
	      fv_files = (int *) malloc ((size_t) fv_cap * sizeof (int));
	      fv_first = (long *) malloc ((size_t) fv_cap * sizeof (long));
	      fv_nsect = (int *) malloc ((size_t) fv_cap * sizeof (int));
	    }
	  fv_n = 0;
	  if (seenrow != NULL && fv_files != NULL && fv_first != NULL && fv_nsect != NULL)
	    {
	      memset (seenrow, 0, (size_t) ctx->nfiles * sizeof (int));
	      for (s9 = 0; s9 < vol->nsect_total; s9++)
		{
		  int ow9 = vol->owner[s9];

		  if (ow9 < 0 || ow9 >= ctx->nfiles)
		    {
		      continue;
		    }
		  if (seenrow[ow9] == 0)
		    {
		      fv_files[fv_n] = ow9;
		      fv_first[fv_n] = s9;
		      fv_nsect[fv_n] = 1;
		      seenrow[ow9] = ++fv_n;
		    }
		  else
		    {
		      fv_nsect[seenrow[ow9] - 1]++;
		    }
		}
	    }
	  if (fv_n > 1 && fv_files != NULL)
	    {
	      /* group by OBJECT: leaders keep their physical (first-sector) order,
	       * class mates follow their leader as heap -> indexes -> overflows */
	      static int *t_files = NULL;
	      static long *t_first = NULL;
	      static int *t_nsect = NULL;
	      static char *t_used = NULL;
	      static int t_cap = 0;

	      if (t_cap < fv_n)
		{
		  free (t_files);
		  free (t_first);
		  free (t_nsect);
		  free (t_used);
		  t_cap = fv_n + 64;
		  t_files = (int *) malloc ((size_t) t_cap * sizeof (int));
		  t_first = (long *) malloc ((size_t) t_cap * sizeof (long));
		  t_nsect = (int *) malloc ((size_t) t_cap * sizeof (int));
		  t_used = (char *) malloc ((size_t) t_cap);
		}
	      if (t_files != NULL && t_first != NULL && t_nsect != NULL && t_used != NULL)
		{
		  int outn = 0, i1, j1, rpass;

		  memset (t_used, 0, (size_t) fv_n);
		  for (i1 = 0; i1 < fv_n; i1++)
		    {
		      OID *c1;

		      if (t_used[i1])
			{
			  continue;
			}
		      t_used[i1] = 1;
		      t_files[outn] = fv_files[i1];
		      t_first[outn] = fv_first[i1];
		      t_nsect[outn] = fv_nsect[i1];
		      outn++;
		      c1 = &ctx->files[fv_files[i1]].class_oid;
		      if (OID_ISNULL (c1))
			{
			  continue;
			}
		      for (rpass = 0; rpass <= 2; rpass++)
			{
			  for (j1 = 0; j1 < fv_n; j1++)
			    {
			      VOLMAP_FILE *fj;

			      if (t_used[j1])
				{
				  continue;
				}
			      fj = &ctx->files[fv_files[j1]];
			      if (OID_ISNULL (&fj->class_oid) || !OID_EQ (&fj->class_oid, c1)
				  || volmap_obj_role (fj->ftype) != rpass)
				{
				  continue;
				}
			      t_used[j1] = 1;
			      t_files[outn] = fv_files[j1];
			      t_first[outn] = fv_first[j1];
			      t_nsect[outn] = fv_nsect[j1];
			      outn++;
			    }
			}
		    }
		  memcpy (fv_files, t_files, (size_t) outn * sizeof (int));
		  memcpy (fv_first, t_first, (size_t) outn * sizeof (long));
		  memcpy (fv_nsect, t_nsect, (size_t) outn * sizeof (int));
		  for (i1 = 0; i1 < outn && seenrow != NULL; i1++)
		    {
		      seenrow[fv_files[i1]] = i1 + 1;	/* cursor sync follows the new rows */
		    }
		}
	    }
	  if (fv_sync_pg >= 0 && seenrow != NULL)
	    {
	      long sxp = fv_sync_pg / VOLMAP_SECT_NPAGES;

	      if (sxp < vol->nsect_total && vol->owner[sxp] >= 0 && vol->owner[sxp] < ctx->nfiles
		  && seenrow[vol->owner[sxp]] > 0)
		{
		  fv_sel = seenrow[vol->owner[sxp]] - 1;
		}
	      fv_sync_pg = -1;
	      fv_last_cc = cur_y * mapw + cur_x;	/* the cursor sync must not override this */
	    }
	  {
	    /* the list FOLLOWS the map: opening the view (and moving the map
	     * cursor) selects the file under the cursor, not just row 0 */
	    int cc9 = cur_y * mapw + cur_x;

	    if (cc9 != fv_last_cc && seenrow != NULL)
	      {
		long p09 = (long) cc9 * per_pages;

		if (p09 < total_pages)
		  {
		    long s90 = p09 / VOLMAP_SECT_NPAGES;
		    long s91 = (p09 + per_pages - 1) / VOLMAP_SECT_NPAGES;
		    long sx;

		    if (s91 >= vol->nsect_total)
		      {
			s91 = vol->nsect_total - 1;
		      }
		    for (sx = s90; sx <= s91; sx++)
		      {
			int ow9 = vol->owner[sx];

			if (ow9 >= 0 && ow9 < ctx->nfiles && seenrow[ow9] > 0)
			  {
			    fv_sel = seenrow[ow9] - 1;
			    break;
			  }
		      }
		  }
		fv_last_cc = cc9;
	      }
	  }
	  if (fv_sel >= fv_n)
	    {
	      fv_sel = (fv_n > 0) ? fv_n - 1 : 0;
	    }
	  volmap_focus_file = (fv_n > 0) ? fv_files[fv_sel] : -1;
	  if (volmap_focus_file >= 0 && !OID_ISNULL (&ctx->files[volmap_focus_file].class_oid))
	    {
	      volmap_mark_class_on = 1;	/* class mates keep their colors + role bg */
	      volmap_mark_class = ctx->files[volmap_focus_file].class_oid;
	    }
	  else
	    {
	      volmap_mark_class_on = 0;
	    }
	}
      else
	{
	  volmap_focus_file = -1;
	}
      prev_cx = (prev_cx >= mapw) ? mapw - 1 : prev_cx;
      prev_cy = (prev_cy >= maph) ? maph - 1 : prev_cy;

      {
	/* With the overlay outside the map there is no left/right switch at all, so
	   the switch detection must not trigger a full repaint (avoids needless
	   flicker on wide screens). */
	int ov_outside;
	int ov_left_now;
	int ov_w_new = volmap_ov_pick_w (ws.ws_col, mapw + 4);	/* the 1/3 rule and the right-placement rule */

	if (ov_w_new != VOLMAP_OV_W)
	  {
	    /* A width change changes the grid coordinate system: reset the scroll origin
	       and repaint fully.  The sector grid is deliberately left alone here -
	       volmap_grid_clamp re-places its window against the selection (ov_pg)
	       every frame, and forcing base to 0 here would leave the selection
	       outside the window and the cursor invisible. */
	    volmap_ov_w_set (ov_w_new);
	    volmap_ov_page_base = 0;
	    force_full = true;
	  }
	ov_outside = (volmap_overlay_place_x (ws.ws_col, mapw + 4, VOLMAP_OV_W + 4, 0, maph, ws.ws_row)
		      == mapw + 6);
	/* With the cursor in the right half of the map, put the box on the left so it
	   does not cover the cursor.  The test must be against the map, not the
	   screen width: the map stops growing at 240 columns, so on a wide screen a
	   cursor at the map's right edge would otherwise fail the test. */
	ov_left_now = ov_outside ? 2 : (cur_x >= mapw / 2);
	static int ov_left_prev = -1;

	full = force_full || vi != prev_vi || ws.ws_col != prev_cols || ws.ws_row != prev_rows
	  || (panel_on && ov_left_now != ov_left_prev);	/* side flip leaves a stale copy behind */
	ov_left_prev = panel_on ? ov_left_now : -1;
      }
      if (ctx->residency && vol->respg == NULL && !vol->res_untrusted)
	{
	  (void) volmap_read_residency (vol);	/* lazy: only when this volume comes on screen */
	}
      if (ncells != alloc_ncells)
	{
	  free (cell_owner);
	  free (cell_state);
	  free (cell_p0);
	  free (cell_alloc_arr);
	  free (cell_render);
	  cell_owner = (int *) malloc (ncells * sizeof (int));
	  cell_state = (char *) malloc (ncells);
	  cell_p0 = (long *) malloc (ncells * sizeof (long));
	  cell_alloc_arr = (long *) malloc (ncells * sizeof (long));
	  cell_render = (char (*)[VOLMAP_CELLSTR]) malloc ((size_t) ncells * VOLMAP_CELLSTR);
	  alloc_ncells = ncells;
	  full = true;
	}

      /* build the right-hand preview panel for the cursor target */
      {
	/* Overlay width per frame: up to about 45% of the screen, a multiple of 8,
	   floor 32 and ceiling 128 (beyond that a row is no longer readable at a
	   glance).  The map is guaranteed at least 24 columns. */
	/* File view width.  A list entry is "name + index name", which is mostly cut
	   at 32 columns.  The drill-down width is fixed to multiples of the 16-cell
	   set (20/36/52/68) because it holds a grid; the file view is a text list and
	   has no reason to follow that rule, so the two are separate values. */
	int cap = (ws.ws_col * 45) / 100;
	int room = ws.ws_col - 24 - 8;	/* map minimum width plus borders and margin */

	if (cap > room)
	  {
	    cap = room;
	  }
	cap &= ~7;
	if (cap < VOLMAP_FV_W_MIN)
	  {
	    cap = VOLMAP_FV_W_MIN;
	  }
	if (cap > 72)
	  {
	    cap = 72;		/* beyond that the names fit with room to spare */
	  }
	if (ws.ws_col < VOLMAP_OV_RIGHT_MIN_COLS || ws.ws_col - (mapw + 4) < VOLMAP_OV_RIGHT_MIN_ROOM)
	  {
	    /* When placed inside the map, the file view box (content+4) takes at most 1/2
	       of the map outline - not the 1/3 used by the sector/page/slot overlays.
	       It is a text list of long index names, so narrow truncation costs more
	       than the map area it hides, and it is only up while choosing a file. */
	    int half = (mapw + 4) / 2 - 4;

	    half &= ~7;
	    if (half < VOLMAP_FV_W_MIN)
	      {
		half = VOLMAP_FV_W_MIN;
	      }
	    if (cap > half)
	      {
		cap = half;
	      }
	  }
	if (cap != volmap_fv_w)
	  {
	    volmap_fv_w = cap;
	    force_full = true;
	  }
      }
      {
	/* Drill-down height and grid allocation - computed once here and fed to both
	   grids.  Deciding the sector grid (fixed rows) and the page grid (its own
	   arithmetic) independently makes the row counts disagree on a width change
	   and drops the info box on short screens. */
	int ov_y0 = (ws.ws_row >= 7 + VOLMAP_OV_MIN_TOTAL + 4) ? 7 : 4;	/* same rule as overlay_draw */
	int ov_avail = ws.ws_row - 4 /* status */  - ov_y0;

	{
	  /* sectors the cursor cell covers = the sector range of its first and last page, straddling included */
	  long cp0 = (long) (cur_y * mapw + cur_x) * per_pages;
	  long slo = (cp0 >= 0) ? cp0 / VOLMAP_SECT_NPAGES : 0;
	  long shi = (cp0 >= 0 && per_pages > 0) ? (cp0 + per_pages - 1) / VOLMAP_SECT_NPAGES : slo;

	  volmap_ov_layout_calc (&volmap_ov_lo, VOLMAP_OV_W, ov_avail, (int) (shi - slo + 1));
	}
	volmap_panel_ensure (&panel, volmap_ov_lo.sect_rows, volmap_ov_lo.sect_w);
	volmap_panel_ensure (&panelb, volmap_ov_lo.page_rows, volmap_ov_grid_w (VOLMAP_OV_W));
      }
      if (full || ctx->scan_active)
	{
	  /* while the progressive scan is attributing owners the cached panel
	   * goes stale between frames - rebuild it every frame until done */
	  volmap_panel_clear (&panel);
	}
      long ov_pg = -1;
      long ov_sdel = 0, ov_sdead = 0;

      if (panel_on && panel.color != NULL)
	{
	  int cc = cur_y * mapw + cur_x;
	  long sect = -1;
	  int hl2 = -1;

	  /* anchor: the page/sector under the cursor */
	  {
	      long p0 = (long) cc * per_pages;

	      if (p0 < total_pages)
		{
		  DKNSECTS psect = (DKNSECTS) (p0 / VOLMAP_SECT_NPAGES);

		  /* The anchor is keyed on the map cell (cc), not the sector.
		     Keying on the sector leaves a stale selection when moving between
		     cells that share a sector, and lets sect and l0_psel point at
		     different cells - the selected page then falls outside the grid and
		     the cursor disappears. */
		  /* Even at the same cell index, a changed cell size (per_pages, after a resize)
		     covers a different page range.  Keeping the old anchor lets
		     ov_sect_off exceed the new cell's sector count, putting the selection
		     outside the grid and losing the cursor. */
		  if (cc != l0_psel_cell || per_pages != l0_psel_per)
		    {
		      /* Pick the first allocated page inside the cell's range.  Using the sector's
			 first allocated page (psect*64 + ctz) instead yields a page outside
			 the cell whenever the cell straddles a sector boundary, leaving the
			 selection off-grid with no cell to draw the cursor on. */
		      long q, qend = p0 + per_pages;
		      long pick = -1;

		      if (qend > total_pages)
			{
			  qend = total_pages;
			}
		      for (q = p0; q < qend; q++)
			{
			  long qs = q / VOLMAP_SECT_NPAGES;

			  if (qs < vol->nsect_total && vol->stab[qs]
			      && ((vol->pagebm[qs] >> (q % VOLMAP_SECT_NPAGES)) & 1))
			    {
			      pick = q;
			      break;
			    }
			}
		      if (pick < 0)
			{
			  pick = p0;	/* with no allocated page, use the cell's first page */
			}
		      psect = (DKNSECTS) (pick / VOLMAP_SECT_NPAGES);
		      l0_psel = (int) (pick % VOLMAP_SECT_NPAGES);
		      l0_psel_sect = psect;
		      l0_psel_cell = cc;
		      l0_psel_per = per_pages;
		      l0_anchor_pg = pick;
		      ov_sect_off = 0;	/* new cell: reset the sector offset too */
		    }
		  /* While the cell does not change, leave the anchor alone.
		     Resetting it whenever it differs from psect (= p0/64) is true on
		     every frame once a cell starts mid-sector, which drags the anchor
		     back to the cell's leading sector.
		     Sector movement belongs to ov_sect_off, driven by the [ and ] keys. */
		  {
		    /* the cell may span several sectors: ov_sect_off walks them */
		    long span_sects = (per_pages + VOLMAP_SECT_NPAGES - 1) / VOLMAP_SECT_NPAGES;
		    long last_sect = (total_pages - 1) / VOLMAP_SECT_NPAGES;

		    if (ov_sect_off < 0)
		      {
			ov_sect_off = 0;
		      }
		    if (ov_sect_off > span_sects - 1)
		      {
			ov_sect_off = span_sects - 1;
		      }
		    /* While the cell does not change, the anchor sector (l0_psel_sect) is the
		       reference.  Recomputing psect (= p0/64) every frame drags the anchor
		       to the cell's leading sector when the cell straddles a boundary. */
		    /* Move ov_sect_off sectors from the anchor sector picked on entry
		       (l0_psel_sect).  Deriving sect from the anchor page instead applies
		       the offset twice and jumps far across sectors. */
		    sect = (long) ((cc == l0_psel_cell) ? l0_psel_sect : psect) + ov_sect_off;
		    if (sect > last_sect)
		      {
			sect = last_sect;
		      }
		  }
		  ov_pg = sect * VOLMAP_SECT_NPAGES + l0_psel;
		  l0_anchor_pg = ov_pg;	/* the current selection the clamp refers to */
		}
	    }
	  {
	    /* physical-first: the ladder stays sector-ordered, the FILE is the
	     * side-channel - mark every map cell of the anchored sector's owner
	     * so a file scattered over many sectors reads instantly */
	    int mk = -1;
	    int cls_on = 0;
	    OID cls = { -1, -1, -1 };

	    if (ov_focus >= 1 && sect >= 0 && sect < vol->nsect_total)
	      {
		mk = vol->owner[sect];
	      }
	    if (mk >= 0 && mk < ctx->nfiles && !OID_ISNULL (&ctx->files[mk].class_oid))
	      {
		cls_on = 1;	/* mark the whole OBJECT: heap + indexes + overflows */
		cls = ctx->files[mk].class_oid;
		mk = -1;
	      }
	    if (mk != volmap_mark_file || cls_on != volmap_mark_class_on
		|| (cls_on && !OID_EQ (&cls, &volmap_mark_class)))
	      {
		volmap_mark_file = mk;
		volmap_mark_class_on = cls_on;
		volmap_mark_class = cls;
		force_full = true;	/* recolor the map under the overlay */
	      }
	  }
	  if (ov_pg != ov_anchor_pg)
	    {
	      /* the overlay re-anchored to a DIFFERENT page (map cursor moved,
	       * volume switched, page picked): selections inside the old page
	       * must not survive into the new one */
	      ov_slot = 0;
	      ov_byte = 0;
	      ov_anchor_pg = ov_pg;
	    }
	  if (volmap_ov_geom.nsl > 0 && ov_slot >= volmap_ov_geom.nsl)
	    {
	      ov_slot = volmap_ov_geom.nsl - 1;	/* mouse/stale paths: never beyond the last real slot */
	    }
	  if (sect < 0)
	    {
	      volmap_panel_clear (&panel);
	      volmap_panel_clear (&panelb);
	    }
	  else
	    {
	      /* box 1: the sector grid, page selection highlighted */
	      long l0pp = (total_pages + (long) ncells - 1) / ncells;
	      int tone0, psel_cell2 = -1;

	      l0pp = ((l0pp + VOLMAP_SECT_NPAGES - 1) / VOLMAP_SECT_NPAGES) * VOLMAP_SECT_NPAGES;
	      tone0 = volmap_map_tone_at (ctx, vol, (int) (sect * VOLMAP_SECT_NPAGES / l0pp), l0pp,
					  total_pages, NULL);
	      /* Use the tone the map actually painted for this sector
		 (volmap_map_tone_at).  Flipping by position within the cell makes an
		 index sector come out in the alternate tone (35) instead of the map's
		 green (32); matching the map matters more than separating sectors.
		 Which sector it is comes from the "(n/N in cell)" title. */
	      if (volmap_mt_sector_warm (vi, sect))
		{
		  volmap_panel_clear (&panel);
		  /* Lay the sectors covered by the cell end to end in one grid.  The first grid
		     cell is the cell's first page; if the selected page falls outside the
		     window, the window moves. */
		  {
		    long cell_p0 = (long) (cur_y * mapw + cur_x) * per_pages;
		    long base;

		    if (cell_p0 < 0)
		      {
			cell_p0 = 0;
		      }
		    /* Window size comes from this frame's grid; scrolling uses the same
		       shared rule as the page grid (volmap_grid_clamp).
		       'total' is what the grid can scan, not the pages the cell covers:
		       the window (64x2 = 128 cells) may exceed the cell (e.g. 80 pages),
		       and passing the cell size would make clamp treat everything as
		       visible, reset base to 0 and push the selection out of view. */
		    volmap_ov_sect_grid.w = (panel.w > 0) ? panel.w : 1;
		    volmap_ov_sect_grid.rows = (panel.rows > 0) ? panel.rows : 1;
		    {
		      long vis = (long) volmap_ov_sect_grid.w * volmap_ov_sect_grid.rows;
		      long tot = (per_pages > vis) ? per_pages : vis;

		      volmap_grid_clamp (&volmap_ov_sect_grid, (int) tot,
					 (ov_pg >= 0) ? (int) (ov_pg - cell_p0) : -1);
		    }
		    base = cell_p0 + (long) volmap_ov_sect_base;
		    /* page range the map cell covers; outside is drawn grey and excluded from selection */
		    panel.sel_lo = cell_p0;
		    panel.sel_hi = cell_p0 + per_pages - 1;
		    panel.base_pg = base;
		    (void) volmap_panel_fill_sector_span (ctx, vol, &panel, base, ov_pg, tone0,
							  &ov_sdel, &ov_sdead, &psel_cell2);
		  }
		  panel.hl_cell = psel_cell2;
		  /* box 2: the selected page's byte distribution (slot highlight follows box 3) */
		  /* the page box stays STILL while slots are browsed: re-marking the
		   * selected slot's bytes re-folds the whole distribution (visible
		   * "movement"); the slot detail lives in the info box instead */
		  if (ov_pg >= 0 && ((vol->pagebm[ov_pg / VOLMAP_SECT_NPAGES] >> (ov_pg % VOLMAP_SECT_NPAGES)) & 1))
		    {
		      volmap_panel_pagebytes (ctx, vol, &panelb, ov_pg, hl2);
		    }
		  else
		    {
		      volmap_panel_clear (&panelb);	/* unallocated: no bytes to draw */
		    }
		}
	      /* else: the sector is cold and worker B is reading it - keep the
	       * previous panel this frame; B's completion wakeup repaints */
	      if (ov_focus == 2)
		{
		  /* byte cells occupy rows 0..rows-4 only - the last three rows are a
		   * spacer plus the 2-row legend, and a cursor allowed in there would
		   * sit on cells that are never drawn (invisible selection).  Same for
		   * the trailing cells past the page's user area: ceil rounding leaves
		   * them blank, so the cursor stops at the last cell that holds data. */
		  int maxc = panelb.w * (panelb.rows - 3) - 1;

		  if (panelb.bytes_per_cell > 0)
		    {
		      int ndata = (vol->user_size + panelb.bytes_per_cell - 1) / panelb.bytes_per_cell;

		      if (ndata > 0 && ndata - 1 < maxc)
			{
			  maxc = ndata - 1;
			}
		    }
		  if (maxc < 0)
		    {
		      maxc = 0;
		    }
		  if (ov_byte < 0)
		    {
		      ov_byte = 0;
		    }
		  if (ov_byte > maxc)
		    {
		      ov_byte = maxc;
		    }
		  panelb.hl_cell = ov_byte;
		}
	    }
	}


      printf ("\033[?2026h");	/* begin synchronized update: no partial-frame paints */

      if (!full)
	{
	  /* nothing but the cursor (and possibly the status text) moved: repaint the
	   * two affected map cells and the right-hand panel, skip everything else */
	  int oc = prev_cy * mapw + prev_cx;
	  int nc = cur_y * mapw + cur_x;

	  volmap_put_cell_reset ();
	  printf ("\033[%d;%dH", 5 + prev_cy, prev_cx + 3);
	  volmap_put_cell (cell_render[oc], false);
	  printf ("\033[%d;%dH", 5 + cur_y, cur_x + 3);
	  volmap_put_cell (cell_render[nc], true);
	  goto interactive_status;
	}

      for (sct = 0; sct < vol->nsect_total; sct++)
	{
	  res += vol->stab[sct];
	  alloc += vol->alloc[sct];
	  if (sct > 0 && vol->owner[sct] >= 0 && vol->owner[sct - 1] >= 0 && vol->owner[sct] != vol->owner[sct - 1])
	    {
	      switches++;
	    }
	}

      volmap_top_bar_draw (ctx, vol, vi, ws.ws_col, mapw, per_pages, res, alloc, switches);
      volmap_map_grid_draw (ctx, vol, mapw, ncells, per_pages, total_pages, cur_x, cur_y,
			    cell_owner, cell_state, cell_p0, cell_alloc_arr, cell_render);
      volmap_frame_bottom_draw (vol, mapw);



    interactive_status:
      /* overlay content + draw runs on BOTH paint paths (partial repaints jump
       * here): a mouse click inside the overlay must refresh the info box too */
      if (panel_on)
	{
	  char ovinfo[512] = "";
	  char ovshort[512] = "";

	  if (ov_pg >= 0 && ov_focus > 0)
	    {
	      /* per-depth inspection, TWO renditions:
	       *   ovinfo  = FULL detail (old zoom-screen level) -> bottom bar
	       *   ovshort = concise identity                    -> overlay info box */
	      if (ov_focus == 3)
		{
		  volmap_describe_slot (ctx, vol, ov_pg, ov_slot, ovinfo, sizeof (ovinfo));
		  /* the info box FIRST line already names the file: the slot line is the OID */
		  snprintf (ovshort, sizeof (ovshort), "OID %d|%ld|%d", (int) vol->volid, ov_pg, ov_slot);
		}
	      else if (ov_focus == 2)
		{
		  /* byte-cell inspection: which offsets, holding what */
		  static const char *bcls[6] = { "record", "contiguous free", "slot directory", "fragmented free",
		    "selected slot", "page header"
		  };
		  int cell2 = ov_byte;
		  long o0 = (long) cell2 * panelb.bytes_per_cell;
		  int cl2 = (panelb.color != NULL && panelb.glyph[ov_byte] == 'b'
			     && panelb.color[ov_byte] >= 10 && panelb.color[ov_byte] <= 15)
		    ? panelb.color[ov_byte] - 10 : -1;
		  const char *cn2 = (cl2 >= 0) ? bcls[cl2] : L ("(empty cell)", "(\xeb\xb9\x88 \xec\xb9\xb8)");

		  snprintf (ovinfo, sizeof (ovinfo), L ("page %ld  ~bytes %ld..%ld: %s  (cell = %dB)",
							"\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 %ld  ~\xeb\xb0\x94\xec\x9d\xb4\xed\x8a\xb8 %ld..%ld: %s  (\xec\x85\x80 = %dB)"),
			    ov_pg, o0, o0 + panelb.bytes_per_cell - 1, cn2, panelb.bytes_per_cell);
		  snprintf (ovshort, sizeof (ovshort), L ("%s  (~bytes %ld..%ld)",
							  "%s  (~\xeb\xb0\x94\xec\x9d\xb4\xed\x8a\xb8 %ld..%ld)"),
			    cn2, o0, o0 + panelb.bytes_per_cell - 1);
		}
	      else if (ov_pg == 0)
		{
		  volmap_describe_vhdr (ctx, vol, ovinfo, sizeof (ovinfo));
		  snprintf (ovshort, sizeof (ovshort), "%s", ovinfo);
		}
	      else
		{
		  /* page inspection: one synthetic 1-page cell reuses the rich describe.
		   * FULL (crumb included) goes to the bottom bar; the info box gets the
		   * object part after the breadcrumb (the boxes already show the where) */
		  long sect1 = ov_pg / VOLMAP_SECT_NPAGES;
		  char st1 = (vol->owner[sect1] >= 0) ? 'F' : (vol->stab[sect1] ? '_' : '.');
		  int ow1 = vol->owner[sect1];
		  long pp1 = ov_pg, al1 = 1;

		  volmap_describe_cell (ctx, vol, 1, &st1, &ow1, &pp1, &al1, 0, ovinfo, sizeof (ovinfo));
		  {
		    /* info box = SECTOR facts the bottom bar does not carry (the FULL
		     * describe there already names the object): page composition from
		     * the grid just built, file offset, residency and --deep counters */
		    int c9, usern = 0, ftabn = 0, tden = 0;
		    char off9[16], ln3[64] = "";

		    for (c9 = 0; c9 < VOLMAP_SECT_NPAGES && c9 < panel.w * panel.rows; c9++)
		      {
			switch (panel.glyph[c9])
			  {
			  case 'b':
			    usern++;
			    break;
			  case '#':
			    ftabn++;
			    break;
			  case 'E':
			    tden++;
			    break;
			  default:
			    break;
			  }
		      }
		    volmap_human ((INT64) sect1 * VOLMAP_SECT_NPAGES * vol->iopagesize, off9, sizeof (off9));
		    if (ctx->residency && vol->respg != NULL)
		      {
			int resn = 0;
			long pg9 = sect1 * VOLMAP_SECT_NPAGES;

			for (c9 = 0; c9 < VOLMAP_SECT_NPAGES; c9++)
			  {
			    resn += (vol->respg[pg9 + c9] > 0);
			  }
			snprintf (ln3, sizeof (ln3), "\nres %d/64", resn);
		      }
		    if (ctx->deep && (ov_sdel > 0 || ov_sdead > 0))
		      {
			size_t l9 = strlen (ln3);

			snprintf (ln3 + l9, sizeof (ln3) - l9, "%sdel %ld vac %ld",
				  (l9 > 0) ? "  " : "\n", ov_sdel, ov_sdead);
		      }
		    snprintf (ovshort, sizeof (ovshort),
			      L ("sect %ld @ %s\nalloc %d/64 (data %d ftab %d%s)%s",
				 "\xec\x84\xb9\xed\x84\xb0 %ld @ %s\n\xed\x95\xa0\xeb\x8b\xb9 %d/64 (data %d ftab %d%s)%s"),
			      sect1, off9, usern + ftabn, usern, ftabn,
			      (tden > 0) ? " E+" : "", ln3);
		  }
		}
	    }
	  if (ovinfo[0] != '\0')
	    {
	      snprintf (ov_live, sizeof (ov_live), "%s", ovinfo);	/* FULL detail for the bottom bar */
	    }
	  if (ov_focus >= 1 && ov_pg >= 0)
	    {
	      /* the file identity leads the info box at every depth (it moved here
	       * from above the sector box: at slot depth it was duplicated) */
	      char fl9[160], tmp9[512];

	      volmap_file_line (ctx, vol, ov_pg / VOLMAP_SECT_NPAGES, fl9, sizeof (fl9));
	      if (fl9[0] != '\0')
		{
		  snprintf (tmp9, sizeof (tmp9), "%s\n%s", fl9, ovshort);
		  snprintf (ovshort, sizeof (ovshort), "%s", tmp9);
		}
	    }
	  if (ov_focus >= 1)
	    {
	      /* copy even when empty: a stale identity from the PREVIOUS drill must
	       * never survive into a frame that describes something else */
	      snprintf (ov_heap, sizeof (ov_heap), "%s", ovshort);	/* concise identity for the info box */
	    }
	  else
	    {
	      ov_heap[0] = '\0';	/* map focus: the info box disappears until the next drill */
	    }
	  volmap_overlay_draw (ctx, vol, &panel, &panelb, (ov_pg >= 0) ? ov_pg / VOLMAP_SECT_NPAGES : -1,
			       ov_pg, ov_slot, ov_focus, ws.ws_col, ov_sdel, ov_sdead, ov_heap, ws.ws_row,
			       cur_x >= mapw / 2, mapw + 4,
			       (long) (cur_y * mapw + cur_x) * per_pages, per_pages, maph);
	}
      if (file_view_on)
	{
	  volmap_file_view_draw (ctx, vol, fv_files, fv_first, fv_nsect, fv_n, fv_sel, ws.ws_col, ws.ws_row, volmap_fv_w,
				 mapw + 4, cur_x >= mapw / 2, maph);
	}

      /* live inspection runs on BOTH paint paths (the partial-repaint path jumps
       * here directly): map level = cell describe; overlay depths already put
       * their FULL detail into ov_live before falling through */
      if (!panel_on || ov_focus == 0)
	{
	  int mcc = cur_y * mapw + cur_x;

	  ov_live[0] = '\0';
	  if (cell_state != NULL && mcc >= 0 && mcc < ncells && cell_state[mcc] != ' ')
	    {
	      volmap_describe_cell (ctx, vol, per_pages, cell_state, cell_owner,
				    cell_p0, cell_alloc_arr, mcc, ov_live, sizeof (ov_live));
	    }
	}
      if (file_view_on && fv_n > 0)
	{
	  VOLMAP_FILE *ff = &ctx->files[fv_files[fv_sel]];
	  int fk = volmap_kind_idx (ff->ftype);
	  const char *fn = OID_ISNULL (&ff->class_oid) ? volmap_ftype_name (ff->ftype)
	    : volmap_resolve_class_name (ctx, ff);

	  /* Same contract as the map status bar: \x01 is a line-break hint (after
	     the object name).  Row 1 says what it is, row 2 the detail.  Without
	     the hint the break falls on width and splits tokens mid-word. */
	  snprintf (ov_live, sizeof (ov_live),
		    "%s%s\x01 VFID %d|%d | %s | sectors %d (first %ld) | pages %d (%d user, %d free)",
		    (fk == 0) ? VOLMAP_TAG_H : (fk == 1) ? VOLMAP_TAG_I : "", fn,
		    (int) ff->vfid.volid, (int) ff->vfid.fileid, volmap_ftype_name (ff->ftype),
		    fv_nsect[fv_sel], fv_first[fv_sel], ff->n_page_total, ff->n_page_user, ff->n_page_free);
	}
      {
	/* Build the status bar string as a per-frame copy.  Appending the inspection
	   onto status2 directly accumulates it every frame while no key is pressed,
	   so the same sentence ends up on screen twice. */
	char bar[1024];

	if (ov_live[0] != '\0' && status2[0] != '\0' && !panel_on)
	  {
	    /* Combine into two rows only while the drill-down is closed.  With it open
	       ov_live already names the page exactly, and status2's map-cursor text
	       describes the same object by cell range - the prefixes (vol / sect /
	       file name / VFID) then duplicate. */
	    char live[512];
	    char *h;

	    snprintf (live, sizeof (live), "%s", ov_live);
	    for (h = strchr (live, '\x01'); h != NULL; h = strchr (h, '\x01'))
	      {
		*h = ' ';	/* one inspection line per row; two hints would misalign row 2 */
	      }
	    snprintf (bar, sizeof (bar), "%s\x01%s", live, status2);
	  }
	else if (ov_live[0] != '\0')
	  {
	    snprintf (bar, sizeof (bar), "%s", ov_live);
	  }
	else
	  {
	    snprintf (bar, sizeof (bar), "%s", status2);
	  }
	volmap_status_bar_draw (ctx, bar, ws.ws_col, ws.ws_row, auto_refresh);
      }

      printf ("\033[?25l\033[%d;%dH", ws.ws_row, ws.ws_col);	/* re-hide + park the cursor:
									 * some clients ignore hide and would
									 * show it hopping during the paint */
      printf ("\033[?2026l");	/* end synchronized update */
      fflush (stdout);
      tcdrain (fileno (stdout));
      prev_vi = vi;
      prev_cols = ws.ws_col;
      prev_rows = ws.ws_row;
      prev_cx = cur_x;
      prev_cy = cur_y;
      force_full = false;
      volmap_mt.ui_frames++;	/* frame boundary: worker A may reuse the last shadow set */

      /* input — the select also listens on the worker self-pipe: scan progress,
       * refresh completion and panel warm-ups arrive as wakeups, never as
       * work done on this thread */
    interactive_input:
      {
	unsigned char ch;
	ctx->scan_pref = vi;	/* the volume on screen fills in first */
	if (volmap_key_pending[0] != '\0')
	  {
	    /* remaining keys of a decomposed Hangul syllable */
	    ch = volmap_key_pending[0];
	    memmove (volmap_key_pending, volmap_key_pending + 1, sizeof (volmap_key_pending) - 1);
	    goto dispatch_key;
	  }
	{
	  fd_set rfds;
	  struct timeval tv;
	  struct timeval *ptv = NULL;
	  int rv, nfds = 1;

	  FD_ZERO (&rfds);
	  FD_SET (0, &rfds);
	  if (volmap_mt.started)
	    {
	      FD_SET (volmap_mt.pipe_rd, &rfds);
	      nfds = volmap_mt.pipe_rd + 1;
	    }
	  bool tick_is_refresh = false;

	  if (auto_refresh && !ctx->scan_active)
	    {
	      tv.tv_sec = ctx->tick_sec;
	      tv.tv_usec = 0;
	      ptv = &tv;
	      tick_is_refresh = true;
	    }
	  else if (!volmap_mt.started && ctx->scan_active)
	    {
	      /* single-threaded fallback (thread start failed): probe between keys */
	      tv.tv_sec = 0;
	      tv.tv_usec = 10000;
	      ptv = &tv;
	    }
	  /* /proc is re-read once a second while something holds the scan or the disk
	     is being watched, so a backup that ends or a server that finishes recovery
	     is noticed without a key */
	  if ((ctx->hold || (ctx->io.state == 1 && (ctx->scan_active || auto_refresh))) && (ptv == NULL || tv.tv_sec >= 1))
	    {
	      tv.tv_sec = 1;
	      tv.tv_usec = 0;
	      ptv = &tv;
	      tick_is_refresh = false;
	    }
	  /* The only point in the loop where this thread holds no array pointer: drop
	   * the interlock so a pending commit can publish, and take it again before the
	   * key handling below, which also dereferences owner[]/pagebm[]. */
	  pthread_mutex_lock (&volmap_mt.pub_mx);
	  volmap_mt.ui_reading = 0;
	  pthread_cond_broadcast (&volmap_mt.pub_cv);
	  pthread_mutex_unlock (&volmap_mt.pub_mx);

	  rv = select (nfds, &rfds, NULL, NULL, ptv);

	  pthread_mutex_lock (&volmap_mt.pub_mx);
	  volmap_mt.ui_reading = 1;
	  pthread_mutex_unlock (&volmap_mt.pub_mx);
	  if (rv < 0)
	    {
	      if (errno == EINTR)
		{
		  if (volmap_resized)
		    {
		      /* the size changed: redraw without waiting for further input */
		      volmap_resized = 0;
		      force_full = true;
		      continue;
		    }
		  goto interactive_input;
		}
	      break;
	    }
	  if (rv == 0)
	    {
	      /* a sampling wakeup, or the hold ending: no refresh is requested here */
	      {
		static time_t last_proc = 0;
		time_t nowt = time (NULL);

		if (nowt != last_proc)
		  {
		    char hr[160];
		    bool was_hold = ctx->hold, was_busy = ctx->io.busy, h;

		    last_proc = nowt;
		    volmap_proc_scan (ctx);
		    volmap_io_sample (ctx);
		    h = volmap_hold_reason (ctx, hr, sizeof (hr));
		    if (!h)
		      {
			ctx->hold_override = false;	/* the next hold holds again */
		      }
		    ctx->hold = h && !ctx->hold_override;
		    if (was_hold && !ctx->hold)
		      {
			snprintf (status2, sizeof (status2), "%s",
				  L ("the run beside the database ended - scan and refresh resumed",
				     "\xec\x98\x86\xec\x97\x90\xec\x84\x9c \xeb\x8f\x8c\xeb\x8d\x98 \xec\x9e\x91\xec\x97\x85\xec\x9d\xb4 \xeb\x81\x9d\xeb\x82\xa8 - \xec\x8a\xa4\xec\xba\x94\xc2\xb7\xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8 \xec\x9e\xac\xea\xb0\x9c"));
			if (volmap_mt.started)
			  {
			    pthread_mutex_lock (&volmap_mt.a_mx);
			    pthread_cond_broadcast (&volmap_mt.a_cv);
			    pthread_mutex_unlock (&volmap_mt.a_mx);
			  }
		      }
		    if (was_hold != ctx->hold || was_busy != ctx->io.busy)
		      {
			force_full = true;
		      }
		  }
	      }
	      if (!volmap_mt.started && ctx->scan_active && !ctx->hold)
		{
		  /* fallback path: scan on the UI thread as before */
		  static struct timespec last_paint = { 0, 0 };
		  struct timespec now;
		  bool idle, busy = ctx->io.busy;

		  clock_gettime (CLOCK_MONOTONIC, &now);
		  idle = (now.tv_sec - volmap_last_key_sec) > 2;
		  (void) volmap_discover_step (ctx, busy ? 512 : 8192, idle && !busy);
		  clock_gettime (CLOCK_MONOTONIC, &now);
		  if (ctx->scan_active
		      && (now.tv_sec - last_paint.tv_sec) * 1000 + (now.tv_nsec - last_paint.tv_nsec) / 1000000 < 150)
		    {
		      goto interactive_input;
		    }
		  last_paint = now;
		  force_full = true;
		  continue;
		}
	      if (!tick_is_refresh || ctx->hold)
		{
		  if (force_full)
		    {
		      continue;	/* the bar changed: repaint */
		    }
		  goto interactive_input;
		}
	      /* auto-refresh tick: the buffer-pool snapshot is a small file - reload it
	       * here on the UI lane (cub_top live rewrites it every frame) */
	      if (ctx->bufmap && volmap_bufmap_reload (ctx))
		{
		  force_full = true;
		}
	      /* auto-refresh tick: hand the incremental refresh to the batch lane */
	      if (volmap_mt.started)
		{
		  pthread_mutex_lock (&volmap_mt.a_mx);
		  volmap_mt.a_refresh_req = true;
		  if (ctx->residency)
		    {
		      volmap_mt.a_resid_vi = vi;
		    }
		  pthread_cond_broadcast (&volmap_mt.a_cv);
		  pthread_mutex_unlock (&volmap_mt.a_mx);
		  goto interactive_input;	/* repaint arrives with the completion wakeup */
		}
	      (void) volmap_refresh (ctx);
	      ctx->refresh_gen++;
	      force_full = true;
	      if (ctx->residency)
		{
		  (void) volmap_read_residency (vol);
		}
	      continue;
	    }
	  if (volmap_mt.started && FD_ISSET (volmap_mt.pipe_rd, &rfds))
	    {
	      char dr[64];

	      while (read (volmap_mt.pipe_rd, dr, sizeof (dr)) > 0)
		{
		  ;		/* drain every queued wakeup */
		}
	      pthread_mutex_lock (&volmap_mt.a_mx);
	      if (volmap_mt.a_refresh_done)
		{
		  volmap_mt.a_refresh_done = false;
		  snprintf (status2, sizeof (status2), "%s",
			    L ("refreshed (incremental)",
			       "\xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8 \xec\x99\x84\xeb\xa3\x8c (\xec\xa6\x9d\xeb\xb6\x84)"));
		}
	      pthread_mutex_unlock (&volmap_mt.a_mx);
	      if (!FD_ISSET (0, &rfds))
		{
		  /* worker wakeup only (no key): throttled full repaint */
		  static struct timespec last_paint2 = { 0, 0 };
		  struct timespec now2;

		  clock_gettime (CLOCK_MONOTONIC, &now2);
		  if (ctx->scan_active
		      && (now2.tv_sec - last_paint2.tv_sec) * 1000 + (now2.tv_nsec -
								      last_paint2.tv_nsec) / 1000000 < 150)
		    {
		      goto interactive_input;
		    }
		  last_paint2 = now2;
		  force_full = true;
		  continue;
		}
	    }
	}
	if (read (0, &ch, 1) <= 0)
	  {
	    break;
	  }
	if ((ch & 0xF0) == 0xE0)
	  {
	    /* Korean IME fallback: a 3-byte UTF-8 jamo means the user forgot to switch
	     * the IME - map the Dubeolsik key position back to the command it sits on.
	     * The continuation bytes are read with a 0.1s timeout (like the ESC parser)
	     * so a lone stray 0xEx byte can never stall the input loop. */
	    unsigned char k2 = 0, k3 = 0;
	    struct termios t3 = term_raw;
	    int got2;

	    t3.c_cc[VMIN] = 0;
	    t3.c_cc[VTIME] = 1;
	    tcsetattr (0, TCSANOW, &t3);
	    got2 = (read (0, &k2, 1) == 1 && read (0, &k3, 1) == 1);
	    tcsetattr (0, TCSANOW, &term_raw);
	    if (!got2)
	      {
		goto interactive_input;	/* truncated sequence: ignore */
	      }
	    {
	      /* full Dubeolsik reverse map.  IMEs often hold a vowel (e.g. \u3157 for
	       * 'h') in preedit and only COMMIT it composed with the next keystroke -
	       * as a syllable like \ud638 - so single-jamo cases alone miss half the
	       * real inputs.  Decode every compatibility jamo AND decompose composed
	       * syllables (U+AC00..) into their choseong/jungseong/jongseong key
	       * strings; the first key dispatches now, the rest queue up. */
	      unsigned int cp = ((unsigned int) (ch & 0x0F) << 12) | ((unsigned int) (k2 & 0x3F) << 6) | (k3 & 0x3F);
	      const char *keys = volmap_hangul_to_keys (cp);

	      if (keys[0] == '\0')
		{
		  goto interactive_input;	/* not Hangul: ignore */
		}
	      ch = (unsigned char) keys[0];
	      if (keys[1] != '\0')
		{
		  snprintf ((char *) volmap_key_pending, sizeof (volmap_key_pending), "%s", keys + 1);
		}
	    }
	  }
	{
	  struct timespec kts;

	  clock_gettime (CLOCK_MONOTONIC, &kts);
	  volmap_last_key_sec = kts.tv_sec;
	}
	if (ch == '\r' || ch == '\n')
	  {
	    ch = ' ';		/* enter = space everywhere: one drill-down ladder, two keys */
	  }
      dispatch_key:
	if (ch == 'q' || ch == 'Q')
	  {
	    break;
	  }
	else if (ch == '>' || ch == '.')
	  {
	    for (vskip = 0; vskip < ctx->nvols; vskip++)
	      {
		vi = (vi + 1) % ctx->nvols;
		if (volmap_vol_selected (ctx, ctx->vols[vi].volid))
		  {
		    break;	/* -V: cycle only through the selected volumes */
		  }
	      }
	    snprintf (status2, sizeof (status2), "%s", L ("click a cell to inspect it", "\xec\x85\x80\xec\x9d\x84 \xed\x81\xb4\xeb\xa6\xad\xed\x95\x98\xeb\xa9\xb4 \xec\x83\x81\xec\x84\xb8\xea\xb0\x80 \xed\x91\x9c\xec\x8b\x9c\xeb\x90\xa9\xeb\x8b\x88\xeb\x8b\xa4"));
	  }
	else if (ch == '<' || ch == ',')
	  {
	    for (vskip = 0; vskip < ctx->nvols; vskip++)
	      {
		vi = (vi + ctx->nvols - 1) % ctx->nvols;
		if (volmap_vol_selected (ctx, ctx->vols[vi].volid))
		  {
		    break;
		  }
	      }
	    snprintf (status2, sizeof (status2), "%s", L ("click a cell to inspect it", "\xec\x85\x80\xec\x9d\x84 \xed\x81\xb4\xeb\xa6\xad\xed\x95\x98\xeb\xa9\xb4 \xec\x83\x81\xec\x84\xb8\xea\xb0\x80 \xed\x91\x9c\xec\x8b\x9c\xeb\x90\xa9\xeb\x8b\x88\xeb\x8b\xa4"));
	  }
	else if (ch == 0x1b)
	  {
	    /* parse CSI (ESC [ ...) and SS3 (ESC O x - "application cursor keys",
	     * what PuTTY and several ssh clients send for the arrows) sequences;
	     * a plain ESC still quits.  200ms inter-byte timeout: on a laggy ssh
	     * link 100ms could split ESC from its sequence and fake a bare ESC. */
	    unsigned char seq[40];
	    int n = 0;
	    struct termios t2 = term_raw;
	    t2.c_cc[VMIN] = 0;
	    t2.c_cc[VTIME] = 2;
	    tcsetattr (0, TCSANOW, &t2);
	    while (n < (int) sizeof (seq) - 1)
	      {
		if (read (0, &seq[n], 1) <= 0)
		  {
		    break;
		  }
		n++;
		/* CSI final byte (0x40..0x7E) ends the sequence — do not swallow queued keys */
		if (n >= 2 && seq[0] == '[' && seq[n - 1] >= 0x40 && seq[n - 1] <= 0x7e)
		  {
		    break;
		  }
		if (n == 2 && seq[0] == 'O')
		  {
		    seq[0] = '[';	/* normalize SS3 arrows to the CSI form */
		    break;
		  }
		if (n == 1 && seq[0] != '[' && seq[0] != 'O')
		  {
		    break;	/* not an escape sequence we know */
		  }
	      }
	    tcsetattr (0, TCSANOW, &term_raw);
	    seq[n] = '\0';
	    if (n == 0)
	      {
		break;		/* bare ESC */
	      }
	    if (seq[0] == '[' && seq[1] == '<' && seq[n - 1] == 'M')
	      {
		int btn = 0, mx = 0, my = 0;
		if (sscanf ((char *) seq, "[<%d;%d;%dM", &btn, &mx, &my) == 3)
		  {
		    {
		      /* double-click on the same spot = space/enter: one step deeper.
		       * (the first click still does its normal select/inspect) */
		      static struct timespec dc_t = { 0, 0 };
		      static int dc_x = -1, dc_y = -1;
		      struct timespec nowc;
		      long dt_ms;

		      clock_gettime (CLOCK_MONOTONIC, &nowc);
		      dt_ms = (nowc.tv_sec - dc_t.tv_sec) * 1000 + (nowc.tv_nsec - dc_t.tv_nsec) / 1000000;
		      if (mx == dc_x && my == dc_y && dt_ms <= 400)
			{
			  dc_x = dc_y = -1;	/* consume: a triple click is not two drills */
			  ch = ' ';
			  goto dispatch_key;
			}
		      dc_t = nowc;
		      dc_x = mx;
		      dc_y = my;
		    }
		    int row = my - 5;	/* map starts at screen row 5 (legend, header, alloc, blank) */
		    int col = mx - 3;	/* the map frame shifts cells right by two */
		    /* On a wide screen the overlay (file view, drill-down) sits outside the map
		       frame, so hit-testing runs before the map bounds check; only map
		       cell hits consult the bounds. */
		      {
			int cell = row * mapw + col;

			if (file_view_on && fv_n > 0 && volmap_fv_geom.w > 0
			    && mx >= volmap_fv_geom.x0 && mx < volmap_fv_geom.x0 + volmap_fv_geom.w
			    && my >= volmap_fv_geom.list_y0 && my < volmap_fv_geom.list_y0 + volmap_fv_geom.rows)
			  {
			    /* Clicking a file list entry selects that file.  Row to entry index adds the
			       scroll offset (top).  Selection only - no jump, matching the
			       keyboard contract where space/enter jumps. */
			    int pick = volmap_fv_geom.top + (my - volmap_fv_geom.list_y0);

			    if (pick >= 0 && pick < fv_n)
			      {
				fv_sel = pick;
				fv_last_cc = cur_y * mapw + cur_x;	/* keep cursor sync from overwriting this selection
									   (-1 means "resync" instead) */
				force_full = true;
				status2[0] = '\0';
			      }
			  }
			else if (panel_on && volmap_ov_geom.x0 > 0
			    && mx >= volmap_ov_geom.x0 && mx < volmap_ov_geom.x0 + VOLMAP_OV_W + 4
			    && my >= volmap_ov_geom.y0 && my <= volmap_ov_geom.y1)
			  {
			    /* click INSIDE the overlay rectangle (borders included): select in
			     * the box under the pointer; a border/title/info hit is a no-op.
			     * Anything OUTSIDE the rectangle - including the map rows above and
			     * below the boxes in the same columns - falls through to the map. */
			    int ocol = mx - (volmap_ov_geom.x0 + 2);

			    if (ocol < 0 || ocol >= VOLMAP_OV_W)
			      {
				;	/* border column: swallow (the map here is covered) */
			      }
			    else if (my >= volmap_ov_geom.b1y && my < volmap_ov_geom.b1y + ((panel.rows > 0) ? panel.rows : 2))
			      {
				int gws = (panel.w > 0) ? panel.w : VOLMAP_OV_W;
				int prow = my - volmap_ov_geom.b1y;
				int pk = volmap_ov_sect_base + prow * gws + ocol;

				if (volmap_ov_sect_more && prow == panel.rows - 1 && ocol >= gws)
				  {
				    /* clicking '+' moves to the next window, wrapping at the end; same contract as the page box */
				    int vis = gws * panel.rows;

				    volmap_ov_sect_base += vis;
				    if (volmap_ov_sect_base >= VOLMAP_SECT_NPAGES)
				      {
					volmap_ov_sect_base = 0;
				      }
				    {
				      /* the window's first page, as an absolute page (see below) */
				      long cp = panel.sel_lo + volmap_ov_sect_base;

				      l0_psel_sect = (DKNSECTS) (cp / VOLMAP_SECT_NPAGES);
				      ov_sect_off = 0;
				      l0_psel = (int) (cp % VOLMAP_SECT_NPAGES);
				      l0_anchor_pg = cp;
				    }
				    ov_focus = 1;
				    ov_slot = 0;
				    force_full = true;
				    status2[0] = '\0';
				  }
				else if (pk < VOLMAP_SECT_NPAGES && ocol < gws
					 && (panel.sel_hi <= panel.sel_lo
					     || (panel.base_pg + prow * gws + ocol >= panel.sel_lo
						 && panel.base_pg + prow * gws + ocol <= panel.sel_hi)))
				  {
				    /* grey cells outside the map cell are not selectable by click either, matching the arrows */
				    /* The grid starts at the map cell's first page, so the click names
				       an absolute page; l0_psel is an offset within the anchor sector.
				       Re-anchor on the clicked page's sector - storing the grid index
				       as the offset lands elsewhere whenever the cell starts
				       mid-sector.  The cell is unchanged, so the anchor stays put. */
				    long cp = panel.base_pg + (long) prow * gws + ocol;

				    ov_focus = 1;
				    l0_psel_sect = (DKNSECTS) (cp / VOLMAP_SECT_NPAGES);
				    ov_sect_off = 0;
				    l0_psel = (int) (cp % VOLMAP_SECT_NPAGES);
				    l0_anchor_pg = cp;
				    ov_slot = 0;
				    status2[0] = '\0';
				  }
			      }
			    else if (my >= volmap_ov_geom.b2y && my < volmap_ov_geom.b2y + volmap_ov_geom.b2n)
			      {
				/* the last three rows are the spacer + legend: no cells there;
				 * an unallocated page has no byte cells at all */
				if (my - volmap_ov_geom.b2y < volmap_ov_geom.b2n - 3 && ov_pg >= 0
				    && ((vol->pagebm[ov_pg / VOLMAP_SECT_NPAGES] >> (ov_pg % VOLMAP_SECT_NPAGES)) & 1))
				  {
				    ov_focus = 2;
				    ov_byte = (my - volmap_ov_geom.b2y) * volmap_ov_grid_w (VOLMAP_OV_W) + ocol;
				    status2[0] = '\0';
				  }
				else if (my - volmap_ov_geom.b2y >= volmap_ov_geom.b2n - 2)
				  {
				    /* Clicking the legend row (the one carrying '+') pages forward, so
				       mouse-only use does not require scrolling by cursor.
				       Wraps to the start at the end. */
				    int grid = (volmap_ov_geom.b2n > 3) ? (volmap_ov_geom.b2n - 3) : 1;
				    int step = grid * volmap_ov_grid_w (VOLMAP_OV_W);
				    int total = (vol->user_size + VOLMAP_PAGE_CELL_BYTES - 1)
				      / VOLMAP_PAGE_CELL_BYTES;

				    if (volmap_ov_page_more)
				      {
					volmap_ov_page_base += step;
				      }
				    else
				      {
					volmap_ov_page_base = 0;
				      }
				    if (volmap_ov_page_base >= total)
				      {
					volmap_ov_page_base = 0;
				      }
				    ov_focus = 2;
				    force_full = true;
				    status2[0] = '\0';
				  }
			      }
			    else if (my >= volmap_ov_geom.b3y && my < volmap_ov_geom.b3y + 3 && ov_pg >= 0
				     && ((vol->pagebm[ov_pg / VOLMAP_SECT_NPAGES] >> (ov_pg % VOLMAP_SECT_NPAGES)) & 1))
			      {
				ov_focus = 3;
				ov_slot = volmap_ov_geom.slot_base
				  + (my - volmap_ov_geom.b3y) * volmap_ov_grid_w (VOLMAP_OV_W) + ocol;
				status2[0] = '\0';
			      }
			  }
			else if (row >= 0 && row < maph && col >= 0 && col < mapw
				 && (long) cell < (total_pages + per_pages - 1) / per_pages)
			  {
			    cur_x = col;
			    cur_y = row;
			    volmap_describe_cell (ctx, vol, per_pages, cell_state, cell_owner,
						  cell_p0, cell_alloc_arr, cell, status2, sizeof (status2));
			  }
			/* clicks on the undrawn area past the volume's last cell are ignored */
		      }
		  }
	      }
	    else if (seq[0] == '[' && seq[1] >= 'A' && seq[1] <= 'D' && panel_on && ov_focus == 1)
	      {
		/* overlay page selection: arrows walk the sector grid; the edges
		 * cross into the neighbouring sector of the same map cell */
		/* The sector grid uses its own width (panel.w, fixed 32); stepping by the
		   overlay width leaves the sector in one move once the width is 40+. */
		int gw1 = (panel.w > 0) ? panel.w : VOLMAP_OV_W;
		int d = (seq[1] == 'C') ? 1 : (seq[1] == 'D') ? -1 : (seq[1] == 'B') ? gw1 : -gw1;
		int np = l0_psel + d;

		if (panel.sel_hi > panel.sel_lo)
		  {
		    /* Stop at the edge of the range the map cell covers.  Cells drawn in grey are
		       context, not selectable; [ and ] move to neighbouring sectors. */
		    /* np is an offset within the sector (l0_psel + d), so the base must be the
		       sector start.  Adding it to the grid's first cell (base_pg) counts the
		       offset twice when a cell starts mid-sector and blocks even the row
		       below, which is still inside the range. */
		    long cur_sect = (l0_anchor_pg >= 0)
		      ? l0_anchor_pg / VOLMAP_SECT_NPAGES : panel.base_pg / VOLMAP_SECT_NPAGES;
		    long want = cur_sect * VOLMAP_SECT_NPAGES + np;

		    if (want < panel.sel_lo || want > panel.sel_hi)
		      {
			ov_slot = 0;
			status2[0] = '\0';
			goto interactive_input;	/* do not move */
		      }
		  }
		if (np < 0)
		  {
		    if (ov_sect_off > 0)
		      {
			ov_sect_off--;
			l0_psel = np + VOLMAP_SECT_NPAGES;
		      }
		  }
		else if (np >= VOLMAP_SECT_NPAGES)
		  {
		    ov_sect_off++;	/* clamped in the build block */
		    l0_psel = np - VOLMAP_SECT_NPAGES;
		  }
		else
		  {
		    l0_psel = np;
		  }
		ov_slot = 0;
		status2[0] = '\0';	/* the frame rebuild fills in the page inspection */
	      }
	    else if (seq[0] == '[' && seq[1] >= 'A' && seq[1] <= 'D' && panel_on && ov_focus == 2)
	      {
		int d = (seq[1] == 'C') ? 1 : (seq[1] == 'D') ? -1 : (seq[1] == 'B') ? VOLMAP_OV_W : -VOLMAP_OV_W;
		int grid2 = (volmap_ov_geom.b2n > 3) ? (volmap_ov_geom.b2n - 3) : 1;
		int vis = grid2 * VOLMAP_OV_W;
		int total2 = (vol->user_size + VOLMAP_PAGE_CELL_BYTES - 1) / VOLMAP_PAGE_CELL_BYTES;

		ov_byte += d;	/* clamped in the build block */
		if (ov_byte < 0)
		  {
		    ov_byte = 0;
		  }
		if (ov_byte >= total2)
		  {
		    ov_byte = total2 - 1;
		  }
		/* Move the window when the selection leaves it, so the cursor can walk the
		   whole 16KB - 4KB-aligned 64B cells do not fit on one screen. */
		if (ov_byte < volmap_ov_page_base)
		  {
		    volmap_ov_page_base = (ov_byte / VOLMAP_OV_W) * VOLMAP_OV_W;
		    force_full = true;
		  }
		else if (ov_byte >= volmap_ov_page_base + vis)
		  {
		    volmap_ov_page_base = ((ov_byte - vis) / VOLMAP_OV_W + 1) * VOLMAP_OV_W;
		    force_full = true;
		  }
		status2[0] = '\0';
	      }
	    else if (seq[0] == '[' && seq[1] >= 'A' && seq[1] <= 'D' && panel_on && ov_focus == 3)
	      {
		int d = (seq[1] == 'C') ? 1 : (seq[1] == 'D') ? -1 : (seq[1] == 'B') ? VOLMAP_OV_W : -VOLMAP_OV_W;

		ov_slot += d;
		if (ov_slot < 0)
		  {
		    ov_slot = 0;
		  }
		if (volmap_ov_geom.nsl > 0 && ov_slot >= volmap_ov_geom.nsl)
		  {
		    /* slot NSL-1 is the last REAL slot: past it there is nothing to
		     * select - the "OID" would be pure arithmetic (beyond num_slots) */
		    ov_slot = volmap_ov_geom.nsl - 1;
		  }
		/* upper clamp happens against the page's real slot count at draw
		 * time via the inspection (beyond num_slots reads as such) */
		status2[0] = '\0';
	      }
	    else if (seq[0] == '[' && seq[1] >= 'A' && seq[1] <= 'D')
	      {
		/* arrow keys: move the keyboard cursor */
		if (file_view_on && (seq[1] == 'A' || seq[1] == 'B'))
		  {
		    /* file view: up/down walk the file list, not the map cursor */
		    fv_sel += (seq[1] == 'B') ? 1 : -1;
		    if (fv_sel < 0)
		      {
			fv_sel = 0;
		      }
		    if (fv_sel >= fv_n && fv_n > 0)
		      {
			fv_sel = fv_n - 1;
		      }
		    force_full = true;	/* the dimmed map recolors around the new file */
		    status2[0] = '\0';
		  }
		else
		  {
		    long nvalid = (total_pages + per_pages - 1) / per_pages;	/* cells actually drawn */

		    if (seq[1] == 'A' && cur_y > 0)
		      {
			cur_y--;
		      }
		    else if (seq[1] == 'B' && cur_y < maph - 1 && (long) (cur_y + 1) * mapw + cur_x < nvalid)
		      {
			cur_y++;
		      }
		    else if (seq[1] == 'C' && cur_x < mapw - 1 && (long) cur_y * mapw + cur_x + 1 < nvalid)
		      {
			cur_x++;
		      }
		    else if (seq[1] == 'D' && cur_x > 0)
		      {
			cur_x--;
		      }
		    ov_sect_off = 0;	/* new cell: the selection restarts at its first sector */
		    status2[0] = '\0';	/* let the bottom bar track the cursor (live inspection) */
		  }
	      }
	  }
	else if ((ch == 0x7f || ch == 0x08) && panel_on && ov_focus == 3)
	  {
	    /* slots is the last rung: any exit key (space or bksp) goes back to the map -
	     * nothing to memorize at the bottom of the ladder */
	    ov_focus = 0;
	    force_full = true;
	    snprintf (status2, sizeof (status2), "%s", L ("back to map", "\xec\xa7\x80\xeb\x8f\x84 \xed\x8f\xac\xec\xbb\xa4\xec\x8a\xa4\xeb\xa1\x9c \xeb\xb3\xb5\xea\xb7\x80"));
	  }
	else if ((ch == 0x7f || ch == 0x08) && panel_on && ov_focus == 2)
	  {
	    ov_focus = 1;
	    force_full = true;
	    status2[0] = '\0';
	  }
	else if ((ch == 0x7f || ch == 0x08) && panel_on && ov_focus == 1)
	  {
	    ov_focus = 0;
	    force_full = true;
	    snprintf (status2, sizeof (status2), "%s", L ("back to map", "\xec\xa7\x80\xeb\x8f\x84 \xed\x8f\xac\xec\xbb\xa4\xec\x8a\xa4\xeb\xa1\x9c \xeb\xb3\xb5\xea\xb7\x80"));
	  }
	else if (ch == ' ' && panel_on && ov_focus >= 1 && ov_pg >= 0
		 && !((vol->pagebm[ov_pg / VOLMAP_SECT_NPAGES] >> (ov_pg % VOLMAP_SECT_NPAGES)) & 1))
	  {
	    /* an unallocated page has no bytes and no slots - nothing deeper to see */
	    snprintf (status2, sizeof (status2), "%s",
		      L ("empty page - nothing to drill into (pick an allocated page)",
			 "\xeb\xb9\x88 \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xe2\x80\x94 \xeb\x8d\x94 \xeb\x93\xa4\xec\x96\xb4\xea\xb0\x88 \xeb\x82\xb4\xec\x9a\xa9\xec\x9d\xb4 \xec\x97\x86\xec\x8a\xb5\xeb\x8b\x88\xeb\x8b\xa4 (\xed\x95\xa0\xeb\x8b\xb9\xeb\x90\x9c \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\xeb\xa5\xbc \xec\x84\xa0\xed\x83\x9d\xed\x95\x98\xec\x84\xb8\xec\x9a\x94)"));
	  }
	else if (ch == ' ' && !file_view_on && (!panel_on || ov_focus < 3))
	  {
	    /* space = one step deeper: map -> overlay(sector) -> page bytes -> slots.
	     * bksp walks back up; the arrows move the selection at every step. */
	    if (ov_live[0] != '\0' && (!panel_on || ov_focus <= 1))
	      {
		snprintf (ov_pin, sizeof (ov_pin), "%s", ov_live);	/* commit the page-level info */
	      }
	    if (!panel_on)
	      {
		panel_on = true;
		ov_focus = 1;
	      }
	    else if (ov_focus == 0)
	      {
		ov_focus = 1;
	      }
	    else
	      {
		ov_focus++;
	      }
	    if (ov_focus == 3)
	      {
		ov_slot = 0;
	      }
	    force_full = true;
	    if (ov_focus == 1)
	      {
		snprintf (status2, sizeof (status2), "%s",
			  L ("overlay: arrows pick a page, space = deeper (page > slots), bksp = back",
			     "\xec\x98\xa4\xeb\xb2\x84\xeb\xa0\x88\xec\x9d\xb4: \xed\x99\x94\xec\x82\xb4\xed\x91\x9c=\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xec\x84\xa0\xed\x83\x9d, space=\xeb\x8d\x94 \xea\xb9\x8a\xec\x9d\xb4(\xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80>\xec\x8a\xac\xeb\xa1\xaf), bksp=\xeb\x92\xa4\xeb\xa1\x9c"));
	      }
	    else
	      {
		status2[0] = '\0';	/* the frame rebuild fills in the inspection */
	      }
	  }
	else if (ch == ' ' && panel_on && ov_focus == 3)
	  {
	    ov_focus = 0;	/* the ladder wraps: deepest step exits back to the map */
	    force_full = true;
	    snprintf (status2, sizeof (status2), "%s", L ("back to map", "\xec\xa7\x80\xeb\x8f\x84 \xed\x8f\xac\xec\xbb\xa4\xec\x8a\xa4\xeb\xa1\x9c \xeb\xb3\xb5\xea\xb7\x80"));
	  }
	else if (ch == 'r' || ch == 'R')
	  {
	    if (ctx->hold)
	      {
		/* the user's call: the fact is on the bar, the reads are theirs to allow */
		ctx->hold_override = true;
		ctx->hold = false;
		snprintf (status2, sizeof (status2), "%s",
			  L ("continuing while it runs - reads compete with it",
			     "\xea\xb7\xb8 \xec\x9e\x91\xec\x97\x85 \xec\xa4\x91\xec\x97\x90\xeb\x8f\x84 \xea\xb3\x84\xec\x86\x8d - \xec\x9d\xbd\xea\xb8\xb0\xea\xb0\x80 \xea\xb2\xbd\xed\x95\xa9\xed\x95\xa9\xeb\x8b\x88\xeb\x8b\xa4"));
		if (volmap_mt.started)
		  {
		    pthread_mutex_lock (&volmap_mt.a_mx);
		    pthread_cond_broadcast (&volmap_mt.a_cv);
		    pthread_mutex_unlock (&volmap_mt.a_mx);
		  }
		force_full = true;
		continue;
	      }
	    if (ctx->scan_active)
	      {
		snprintf (status2, sizeof (status2), L ("initial scan in progress (%d%%)",
							"\xec\xb4\x88\xea\xb8\xb0 \xec\x8a\xa4\xec\xba\x94 \xec\xa7\x84\xed\x96\x89 \xec\xa4\x91 (%d%%)"),
			  (int) (ctx->scan_total > 0 ? 100 * ctx->scan_probed / ctx->scan_total : 0));
		continue;
	      }
	    if (ctx->bufmap)
	      {
		(void) volmap_bufmap_reload (ctx);
	      }
	    /* Temp volumes live only while a query spills, so [r] rescans the directory,
	       adding new ones and dropping those that vanished.  Both worker lanes hold
	       volume pointers and fds across their reads, so the list may only change
	       while neither is working AND neither can start:

	         1. raise list_frozen - each lane checks it under its own mutex before
	            taking new work, so "idle" cannot go stale after we observe it;
	         2. wait for a_busy and b_busy to clear, each under its own mutex;
	         3. rescan only if both really are clear - on timeout we skip this round
	            rather than pull the array out from under a lane;
	         4. drop the barrier and wake both lanes.  */
	    {
	      int guard;
	      bool a_idle, b_idle, list_changed = false;

	      volmap_mt.list_frozen = 1;

	      pthread_mutex_lock (&volmap_mt.a_mx);
	      for (guard = 0; volmap_mt.a_busy && guard < 200; guard++)
		{
		  struct timespec dl;

		  clock_gettime (CLOCK_REALTIME, &dl);
		  dl.tv_nsec += 10 * 1000000L;
		  dl.tv_sec += dl.tv_nsec / 1000000000L;
		  dl.tv_nsec %= 1000000000L;
		  (void) pthread_cond_timedwait (&volmap_mt.a_cv, &volmap_mt.a_mx, &dl);
		}
	      a_idle = !volmap_mt.a_busy;	/* read under a_mx, not after it */
	      pthread_mutex_unlock (&volmap_mt.a_mx);

	      pthread_mutex_lock (&volmap_mt.b_mx);
	      for (guard = 0; volmap_mt.b_busy && guard < 200; guard++)
		{
		  struct timespec dl;

		  clock_gettime (CLOCK_REALTIME, &dl);
		  dl.tv_nsec += 10 * 1000000L;
		  dl.tv_sec += dl.tv_nsec / 1000000000L;
		  dl.tv_nsec %= 1000000000L;
		  (void) pthread_cond_timedwait (&volmap_mt.b_cv, &volmap_mt.b_mx, &dl);
		}
	      b_idle = !volmap_mt.b_busy;	/* read under b_mx, not after it */

	      /* The barrier keeps both lanes out, so the rescan runs with the list to
	         itself.  b_mx is still held: lane B cannot even re-enter its loop body. */
	      if (a_idle && b_idle && volmap_scan_temp_volumes (ctx))
		{
		  list_changed = true;
		  /* the list changed; the cursor may point at a volume that is gone */
		  if (vi >= ctx->nvols)
		    {
		      vi = (ctx->nvols > 0) ? ctx->nvols - 1 : 0;
		    }
		  force_full = true;
		}
	      pthread_mutex_unlock (&volmap_mt.b_mx);

	      /* Before the barrier drops, so lane A sees the new generation the moment it
	         may run again: a scan pending over the old list must not be republished. */
	      if (list_changed)
		{
		  pthread_mutex_lock (&volmap_mt.a_mx);
		  volmap_mt.list_gen++;
		  pthread_mutex_unlock (&volmap_mt.a_mx);
		}
	      volmap_mt.list_frozen = 0;
	      pthread_mutex_lock (&volmap_mt.a_mx);
	      pthread_cond_broadcast (&volmap_mt.a_cv);
	      pthread_mutex_unlock (&volmap_mt.a_mx);
	      pthread_mutex_lock (&volmap_mt.b_mx);
	      pthread_cond_broadcast (&volmap_mt.b_cv);
	      pthread_mutex_unlock (&volmap_mt.b_mx);
	    }
	    if (volmap_mt.started)
	      {
		/* batch lane does the reads; the completion wakeup repaints */
		pthread_mutex_lock (&volmap_mt.a_mx);
		volmap_mt.a_refresh_req = true;
		pthread_cond_broadcast (&volmap_mt.a_cv);
		pthread_mutex_unlock (&volmap_mt.a_mx);
		force_full = true;
		snprintf (status2, sizeof (status2), "%s",
			  L ("refreshing in background...",
			     "\xeb\xb0\xb1\xea\xb7\xb8\xeb\x9d\xbc\xec\x9a\xb4\xeb\x93\x9c \xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8 \xec\xa4\x91..."));
	      }
	    else
	      {
		(void) volmap_refresh (ctx);
		ctx->refresh_gen++;
		force_full = true;
		snprintf (status2, sizeof (status2), "%s",
			  L ("refreshed (incremental)",
			     "\xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8 \xec\x99\x84\xeb\xa3\x8c (\xec\xa6\x9d\xeb\xb6\x84)"));
	      }
	  }
	else if (ch == 'a' || ch == 'A')
	  {
	    if (ctx->scan_active)
	      {
		snprintf (status2, sizeof (status2), "initial scan in progress (%d%%)",
			  (int) (ctx->scan_total > 0 ? 100 * ctx->scan_probed / ctx->scan_total : 0));
		continue;
	      }
	    auto_refresh = !auto_refresh;
	    snprintf (status2, sizeof (status2), L ("auto-refresh %s (%ds)", "\xec\x9e\x90\xeb\x8f\x99 \xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8 %s (%d\xec\xb4\x88)"),
		      auto_refresh ? "ON" : "OFF", ctx->tick_sec);
	  }
	else if (ch == 'h' || ch == 'H')
	  {
	    volmap_help_screen (ctx, file_view_on);
	    force_full = true;
	  }
	else if (ch == '\t')
	  {
	    if (file_view_on)
	      {
		file_view_on = false;	/* tab hands over to the drill-down */
		volmap_focus_file = -1;
	      }
	    panel_on = !panel_on;
	    force_full = true;
	    snprintf (status2, sizeof (status2), L ("drill-down overlay %s (space = step into it)", "\xeb\x93\x9c\xeb\xa6\xb4\xeb\x8b\xa4\xec\x9a\xb4 \xec\x98\xa4\xeb\xb2\x84\xeb\xa0\x88\xec\x9d\xb4 %s (space = \xeb\x82\xb4\xeb\xb6\x80 \xed\x8f\xac\xec\xbb\xa4\xec\x8a\xa4)"),
		      panel_on ? "ON" : "OFF");
	  }
	else if ((ch == '[' || ch == ']') && panel_on)
	  {
	    /* cycle the stacked panel's page selection through the cursor sector's
	     * allocated pages */
	    long p0 = (long) (cur_y * mapw + cur_x) * per_pages;

	    if (p0 < total_pages)
	      {
		DKNSECTS psect = (DKNSECTS) (p0 / VOLMAP_SECT_NPAGES);
		UINT64 bm = vol->pagebm[psect];
		int step = (ch == ']') ? 1 : VOLMAP_SECT_NPAGES - 1;
		int k;

		if (psect != l0_psel_sect)
		  {
		    l0_psel = (bm != 0) ? __builtin_ctzll (bm) : 0;
		    l0_psel_sect = psect;
		  }
		else if (bm != 0)
		  {
		    for (k = 0; k < VOLMAP_SECT_NPAGES; k++)
		      {
			l0_psel = (l0_psel + step) % VOLMAP_SECT_NPAGES;
			if ((bm >> l0_psel) & 1)
			  {
			    break;
			  }
		      }
		  }
		snprintf (status2, sizeof (status2), "panel page: %ld",
			  (long) psect * VOLMAP_SECT_NPAGES + l0_psel);
	      }
	  }
	else if (ch >= '1' && ch <= '3' && panel_on)
	  {
	    if (ch >= '2' && ov_pg >= 0
		&& !((vol->pagebm[ov_pg / VOLMAP_SECT_NPAGES] >> (ov_pg % VOLMAP_SECT_NPAGES)) & 1))
	      {
		/* an unallocated page has no bytes and no slots - parsing it would
		 * render garbage as a slot directory */
		snprintf (status2, sizeof (status2), "%s",
			  L ("empty page - nothing to drill into (pick an allocated page)",
			     "\xeb\xb9\x88 \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80 \xe2\x80\x94 \xeb\x8d\x94 \xeb\x93\xa4\xec\x96\xb4\xea\xb0\x88 \xeb\x82\xb4\xec\x9a\xa9\xec\x9d\xb4 \xec\x97\x86\xec\x8a\xb5\xeb\x8b\x88\xeb\x8b\xa4 (\xed\x95\xa0\xeb\x8b\xb9\xeb\x90\x9c \xed\x8e\x98\xec\x9d\xb4\xec\xa7\x80\xeb\xa5\xbc \xec\x84\xa0\xed\x83\x9d\xed\x95\x98\xec\x84\xb8\xec\x9a\x94)"));
		goto interactive_input;
	      }
	    if (ov_live[0] != '\0' && ov_focus <= 1 && ch - '0' >= 2)
	      {
		snprintf (ov_pin, sizeof (ov_pin), "%s", ov_live);
	      }
	    ov_focus = ch - '0';	/* 1=sector, 2=page bytes, 3=slots */
	    if (ov_focus == 3)
	      {
		ov_slot = 0;
	      }
	    force_full = true;
	    status2[0] = '\0';
	  }
	else if (ch == 'p' || ch == 'P')
	  {
	    if (volmap_chain_volid >= 0)
	      {
		volmap_chain_volid = -1;	/* toggle the chain view off */
		force_full = true;
		snprintf (status2, sizeof (status2), "%s", L ("chain view off",
							      "\xec\xb2\xb4\xec\x9d\xb8 \xeb\xb7\xb0 \xed\x95\xb4\xec\xa0\x9c"));
	      }
	    else
	      {
		/* target heap: the anchored/cursor file; indexes and overflows
		 * resolve to their object's heap through the class OID */
		int tf = -1;
		long tsect = -1;

		if (panel_on && ov_focus >= 1 && ov_pg >= 0)
		  {
		    tsect = ov_pg / VOLMAP_SECT_NPAGES;
		  }
		else
		  {
		    long p09 = (long) (cur_y * mapw + cur_x) * per_pages;

		    if (p09 < total_pages)
		      {
			long s90 = p09 / VOLMAP_SECT_NPAGES;
			long s91 = (p09 + per_pages - 1) / VOLMAP_SECT_NPAGES;

			if (s91 >= vol->nsect_total)
			  {
			    s91 = vol->nsect_total - 1;
			  }
			for (; s90 <= s91 && tsect < 0; s90++)
			  {
			    if (vol->owner[s90] >= 0)
			      {
				tsect = s90;
			      }
			  }
		      }
		  }
		if (tsect >= 0 && tsect < vol->nsect_total)
		  {
		    tf = vol->owner[tsect];
		  }
		if (tf >= 0 && tf < ctx->nfiles && volmap_obj_role (ctx->files[tf].ftype) != 0
		    && !OID_ISNULL (&ctx->files[tf].class_oid))
		  {
		    int i8;

		    for (i8 = 0; i8 < ctx->nfiles; i8++)
		      {
			if (volmap_obj_role (ctx->files[i8].ftype) == 0
			    && OID_EQ (&ctx->files[i8].class_oid, &ctx->files[tf].class_oid))
			  {
			    tf = i8;	/* index/overflow -> its object's heap */
			    break;
			  }
		      }
		  }
		if (tf >= 0 && tf < ctx->nfiles && volmap_obj_role (ctx->files[tf].ftype) == 0)
		  {
		    volmap_chain_walk (ctx, &ctx->files[tf], vol);
		    force_full = true;
		    snprintf (status2, sizeof (status2), "%s", volmap_chain_sum);
		  }
		else
		  {
		    snprintf (status2, sizeof (status2), "%s",
			      L ("chain view: put the cursor on a heap-owned cell first",
				 "\xec\xb2\xb4\xec\x9d\xb8 \xeb\xb7\xb0: \xed\x9e\x99 \xec\x86\x8c\xec\x9c\xa0 \xec\x85\x80\xec\x97\x90 \xec\xbb\xa4\xec\x84\x9c\xeb\xa5\xbc \xeb\x91\x90\xea\xb3\xa0 \xeb\x88\x84\xeb\xa5\xb4\xec\x84\xb8\xec\x9a\x94"));
		  }
	      }
	  }
	else if (ch == 'f' || ch == 'F')
	  {
	    file_view_on = !file_view_on;
	    if (file_view_on)
	      {
		/* one grammar everywhere: f = "show the file of what I selected".
		 * From the drill-down that is the anchored page (which may sit in a
		 * NEIGHBOUR sector after an F1 crossover - the cursor cell alone
		 * would pick the wrong file); from the map it is the cursor cell. */
		fv_sync_pg = (panel_on && ov_focus >= 1) ? ov_pg : -1;
		panel_on = false;	/* the file view replaces the drill-down overlay */
		ov_focus = 0;
		fv_sel = 0;
		fv_last_cc = -1;	/* first frame syncs the list to the cursor's file */
	      }
	    else
	      {
		volmap_focus_file = -1;
	      }
	    force_full = true;
	    status2[0] = '\0';
	  }
	else if (ch == ' ' && file_view_on)
	  {
	    /* jump the map cursor onto the selected file's first sector */
	    if (fv_n > 0 && per_pages > 0)
	      {
		long jc = fv_first[fv_sel] * VOLMAP_SECT_NPAGES / per_pages;

		cur_y = (int) (jc / mapw);
		cur_x = (int) (jc % mapw);
		force_full = true;
		status2[0] = '\0';
	      }
	  }
	else if (ch == 'g' || ch == 'G')
	  {
	    volmap_ascii_frame = !volmap_ascii_frame;
	    force_full = true;
	    snprintf (status2, sizeof (status2), "%s",
		      volmap_ascii_frame ? L ("ASCII borders (for terminals that draw box glyphs double-width)",
					      "ASCII \xed\x85\x8c\xeb\x91\x90\xeb\xa6\xac (\xeb\xb0\x95\xec\x8a\xa4 \xea\xb8\x80\xeb\xa6\xac\xed\x94\x84\xea\xb0\x80 2\xed\x8f\xad\xec\x9c\xbc\xeb\xa1\x9c \xea\xb9\xa8\xec\xa7\x80\xeb\x8a\x94 \xed\x84\xb0\xeb\xaf\xb8\xeb\x84\x90\xec\x9a\xa9)")
		      : L ("unicode borders", "\xec\x9c\xa0\xeb\x8b\x88\xec\xbd\x94\xeb\x93\x9c \xed\x85\x8c\xeb\x91\x90\xeb\xa6\xac"));
	  }
	else if (ch == 'l' || ch == 'L')
	  {
	    volmap_lang_ko = !volmap_lang_ko;
	    force_full = true;
	    snprintf (status2, sizeof (status2), "%s",
		      volmap_lang_ko ? "\xed\x95\x9c\xea\xb8\x80 \xed\x91\x9c\xec\x8b\x9c\xeb\xa1\x9c \xec\xa0\x84\xed\x99\x98 (l = English)" : "English UI (l = \xed\x95\x9c\xea\xb8\x80)");
	  }
	else if (ch == 'b' || ch == 'B')
	  {
	    if (ctx->bufmap_path == NULL)
	      {
		snprintf (status2, sizeof (status2), "%s",
			  L ("no buffer-pool snapshot: start with --bufmap FILE (written by cub_top --bcb-dump FILE)",
			     "\xeb\xb2\x84\xed\x8d\xbc\xed\x92\x80 \xec\x8a\xa4\xeb\x83\x85\xec\x83\xb7 \xec\x97\x86\xec\x9d\x8c: --bufmap FILE \xeb\xa1\x9c \xec\x8b\x9c\xec\x9e\x91 (cub_top --bcb-dump FILE \xea\xb0\x80 \xec\x83\x9d\xec\x84\xb1)"));
	      }
	    else
	      {
		ctx->bufmap = !ctx->bufmap;
		if (ctx->bufmap)
		  {
		    (void) volmap_bufmap_reload (ctx);
		  }
		force_full = true;
		{
		  char bsum[256];

		  volmap_bufmap_summary (ctx, bsum, sizeof (bsum));
		  snprintf (status2, sizeof (status2), "%s %s%s", L ("buffer-pool layer", "\xeb\xb2\x84\xed\x8d\xbc\xed\x92\x80 \xeb\xa0\x88\xec\x9d\xb4\xec\x96\xb4"),
			    ctx->bufmap ? "ON - " : "OFF", ctx->bufmap ? bsum : "");
		}
	      }
	  }
	else if (ch == 'c' || ch == 'C')
	  {
	    /* Switch the cursor style between border highlight and blue background at
	       runtime so it can be reverted on the spot.  The default also comes from
	       --cursor-bg. */
	    volmap_cursor_border = !volmap_cursor_border;
	    force_full = true;
	    snprintf (status2, sizeof (status2),
		      L ("cursor style: %s", "\xec\xbb\xa4\xec\x84\x9c \xed\x91\x9c\xec\x8b\x9c: %s"),
		      volmap_cursor_border
		      ? L ("border highlight (residency shade stays visible)",
			   "\xed\x85\x8c\xeb\x91\x90\xeb\xa6\xac \xea\xb0\x95\xec\xa1\xb0 (\xeb\xa0\x88\xec\xa7\x80\xeb\x8d\x98\xec\x8b\x9c \xec\x9d\x8c\xec\x98\x81\xec\x9d\xb4 \xea\xb7\xb8\xeb\x8c\x80\xeb\xa1\x9c \xeb\xb3\xb4\xec\x9d\xb8\xeb\x8b\xa4)")
		      : L ("blue background (classic)", "\xed\x8c\x8c\xeb\x9e\x80 \xeb\xb0\xb0\xea\xb2\xbd (\xec\xa2\x85\xeb\x9e\x98 \xeb\xb0\xa9\xec\x8b\x9d)"));
	  }
	else if (ch == 'm' || ch == 'M')
	  {
	    ctx->residency = !ctx->residency;
	    if (ctx->residency)
	      {
		(void) volmap_read_residency (vol);	/* only the volume on screen */
	      }
	    /* The drill-down panel reuses its cache for the same page, so toggling
	       residency leaves the page box shading stale (the sector box refills each
	       frame and updates at once).  Invalidate the cache so it takes effect. */
	    panel.cached_level = -1;
	    panel.cached_key = -1;
	    panelb.cached_level = -1;
	    panelb.cached_key = -1;
	    force_full = true;
	    snprintf (status2, sizeof (status2), L ("residency mode %s (background shade = memory-resident)",
						    "\xec\xba\x90\xec\x8b\x9c \xec\x83\x81\xec\xa3\xbc \xed\x91\x9c\xec\x8b\x9c %s (\xeb\xb0\xb0\xea\xb2\xbd \xec\x9d\x8c\xec\x98\x81 = \xeb\xa9\x94\xeb\xaa\xa8\xeb\xa6\xac\xec\x97\x90 \xec\x9e\x88\xec\x9d\x8c)"),
		      ctx->residency ? "ON" : "OFF");
	  }
      }
    }

  /* restore terminal */
  free (fv_files);
  free (fv_first);
  free (fv_nsect);
  volmap_focus_file = -1;
  volmap_mt_stop ();
  printf ("\033]111\007\033[?1006l\033[?1000l\033[?7h\033[?25h\033[9999;1H\r\n");
  fflush (stdout);
  tcsetattr (0, TCSANOW, &term_old);
  free (cell_owner);
  free (cell_state);
  free (cell_p0);
  free (cell_alloc_arr);
  free (cell_render);
  volmap_panel_free (&panel);
  volmap_panel_free (&panelb);	/* the page box: its buffers outlived the function */
}

static void
volmap_render (VOLMAP_CTX * ctx)
{
  FILE *fp = ctx->outfp;
  int vi, i;
  INT64 total_bytes = 0, total_reserved_pages = 0, total_alloc_pages = 0;
  char h1[32], h2[32];

  for (vi = 0; vi < ctx->nvols; vi++)
    {
      total_bytes += (INT64) ctx->vols[vi].nsect_total * VOLMAP_SECT_NPAGES * ctx->vols[vi].iopagesize;
    }
  for (i = 0; i < ctx->nfiles; i++)
    {
      total_reserved_pages += (INT64) ctx->files[i].n_sector_total * VOLMAP_SECT_NPAGES;
      total_alloc_pages += ctx->files[i].alloc_pages;
    }

  volmap_human (total_bytes, h1, sizeof (h1));
  {
    time_t now = time (NULL);
    char ts[32];

    strftime (ts, sizeof (ts), "%Y-%m-%d %H:%M:%S", localtime (&now));
    fprintf (fp, "\n%sCUBRID VOLUME MAP%s  db %s  %s  volumes %d, files %d, total %s\n", ctx->plain ? "" : VM_BOLD,
	     ctx->plain ? "" : VM_RESET, ctx->db_label != NULL ? ctx->db_label : "?", ts,
	     ctx->nvols, ctx->nfiles, h1);
  }
  volmap_print_shutdown (ctx, fp);
  volmap_io_window (ctx);	/* the disk over the run (at least a 200ms window) */
  volmap_print_environment (ctx, fp);
  volmap_print_discovery (ctx, fp);
  if (total_reserved_pages > 0)
    {
      {
	INT64 idlep = total_reserved_pages - total_alloc_pages;
	char isz[32];

	volmap_human ((idlep > 0 ? idlep : 0) * (ctx->nvols > 0 ? ctx->vols[0].iopagesize : 16384), isz, sizeof (isz));
	{
	  double ip = 100.0 * (1.0 - (double) total_alloc_pages / (double) total_reserved_pages);

	  fprintf (fp, "  pages %lld reserved / %lld allocated  (idle %s = %.2f%%)%s\n",
		   (long long) total_reserved_pages, (long long) total_alloc_pages, isz, ip,
		   (ip >= 50.0) ? "  [high idle is normal on small databases: files round up to 1MB sectors]" : "");
	}
      }
    }
  fprintf (fp, "\n");

  /* legend */
  if (ctx->plain)
    {
      fprintf (fp, "LEGEND  data @ 0 8   index / \\ X   catalog + = *   system $   temp ~\n"
	       "        (same kind cycles its family per FILE - a character change inside one family = file boundary)\n"
	       "        _ reserved-empty   . unreserved   # volume metadata   ? unknown owner\n\n");
    }
  else
    {
      fprintf (fp, "%sLEGEND%s  ALLOCATION ramp per kind (glyph = pages allocated in the cell, not record density):\n"
	       "          \033[34mdata (34\033[0m/\033[36m36\033[0m) / \033[32mindex (32\033[0m/\033[35m35\033[0m) / \033[31mcat+sys (31/91)\033[0m : braille \xe2\xa0\xbf 100%%  \xe2\xa0\xbe 80%%  \xe2\xa0\xb6 60%%  \xe2\xa0\xb4 40%%  \xe2\xa0\xa4 <=20%%\n"

	       "          temp    braille ramp, per-file color\n"
	       "        tone flips at every file change: \033[34mconstant\033[0m=contiguous file, \033[34mfl\033[36mip\033[34mpi\033[36mng\033[0m=interleaved files\n"
	       "        \033[38;5;208m\xe2\xa0\x82\033[0m resv (reserved-empty)   \033[38;5;242m\xe2\xa0\x82\033[0m unreserved   "
	       "\033[38;5;238m#\033[0m volume metadata   ? unknown owner\n"
	       "        owner switches = tone-flip count = fragmentation metric   "
	       "idle = reserved - allocated pages (space reserved but not yet written)\n\n", VM_BOLD, VM_RESET);
    }
  if (ctx->bufmap_path != NULL)
    {
      char bsum[256];

      volmap_bufmap_summary (ctx, bsum, sizeof (bsum));
      if (ctx->plain)
	{
	  fprintf (fp, "BUFMAP  %s\n        (per-volume 'buf N pages (unflushed M)' = pages of that volume in cub_server's buffer pool; unflushed = DIRTY or FLUSHING)\n\n", bsum);
	}
      else
	{
	  fprintf (fp, "%sBUFMAP%s  %s\n        background \033[48;5;23m teal \033[49m = cell has buffered pages   "
		   "\033[48;5;53m purple \033[49m = cell has unflushed buffered pages, DIRTY or FLUSHING (memory newer than volume)\n\n",
		   VM_BOLD, VM_RESET, bsum);
	}
    }

  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      int cells = ctx->width * ctx->rows;

      if (!volmap_vol_selected (ctx, vol->volid))
	{
	  continue;
	}
      long res = 0, owned = 0;
      INT64 alloc = 0;
      long switches = 0;
      DKNSECTS s;
      int c;

      for (s = 0; s < vol->nsect_total; s++)
	{
	  res += vol->stab[s];
	  owned += (vol->owner[s] >= 0);
	  alloc += vol->alloc[s];
	  if (s > 0 && vol->owner[s] >= 0 && vol->owner[s - 1] >= 0 && vol->owner[s] != vol->owner[s - 1])
	    {
	      switches++;
	    }
	}

      volmap_human ((INT64) vol->nsect_total * VOLMAP_SECT_NPAGES * vol->iopagesize, h1, sizeof (h1));
      {
	long total_pages = (long) vol->nsect_total * VOLMAP_SECT_NPAGES;
	long per_pages = ctx->full ? 1
	  : (((total_pages + cells - 1) / cells + VOLMAP_SECT_NPAGES - 1) / VOLMAP_SECT_NPAGES) * VOLMAP_SECT_NPAGES;
	char cellsz[32];
	if (per_pages < 1)
	  {
	    per_pages = 1;
	  }
	if (ctx->full)
	  {
	    cells = (int) total_pages;	/* -f: every page gets a cell, rows unbounded */
	  }
	volmap_human ((INT64) per_pages * vol->iopagesize, cellsz, sizeof (cellsz));
	/* how many owner changes survive at this cell zoom (map-visible switches) */
	long visible_switches = 0;
	{
	  int pm = -1, dpm = -1, dtn = 0;
	  long c2;

	  for (c2 = 0; c2 < cells; c2++)
	    {
	      VOLMAP_CELL cl;

	      volmap_cell_classify (ctx, vol, c2 * per_pages, per_pages, total_pages, &dpm, &dtn, false, &cl);
	      if (cl.state == ' ')
		{
		  break;
		}
	      if (cl.maj >= 0)
		{
		  if (pm >= 0 && cl.maj != pm)
		    {
		      visible_switches++;
		    }
		  pm = cl.maj;
		}
	    }
	}
	{
	  char resinfo[128] = "";
	  if (ctx->residency && vol->respg != NULL)
	    {
	      long rp = 0, q, npages_v = (long) vol->nsect_total * VOLMAP_SECT_NPAGES;
	      int sub = vol->iopagesize / 4096;
	      for (q = 0; q < npages_v; q++)
		{
		  rp += vol->respg[q];
		}
	      snprintf (resinfo, sizeof (resinfo), "  resident %.1f%%",
			npages_v > 0 ? 100.0 * rp / ((double) npages_v * sub) : 0.0);
	    }
	  else if (ctx->residency && vol->res_untrusted)
	    {
	      snprintf (resinfo, sizeof (resinfo), "  resident unknown (needs file owner or write permission)");
	    }
	  if (ctx->bufmap && ctx->bufmap_loaded && vol->bufpg != NULL)
	    {
	      size_t rl = strlen (resinfo);

	      char fl[40] = "", fr[40] = "";

	      if (vol->buf_flushing > 0)
		{
		  snprintf (fl, sizeof (fl), ", flushing %ld", vol->buf_flushing);
		}
	      if (vol->buf_freed > 0)
		{
		  snprintf (fr, sizeof (fr), ", freed %ld", vol->buf_freed);
		}
	      snprintf (resinfo + rl, sizeof (resinfo) - rl, "  buf %ld pages (unflushed %ld%s%s)", vol->buf_total, vol->buf_dirty, fl, fr);
	    }
	  fprintf (fp, "%s[%3d] %s  sectors %ld/%d reserved (x64 = %lld pages)  (cell = %ld pages = %s)%s%s\n",
		   ctx->plain ? "" : VM_BOLD, vol->volid, h1, res, vol->nsect_total,
		   (long long) res * VOLMAP_SECT_NPAGES, per_pages, cellsz, resinfo,
		   ctx->plain ? "" : VM_RESET);
	}
	{
	  INT64 idlep = res * (INT64) VOLMAP_SECT_NPAGES - alloc;
	  volmap_human ((idlep > 0 ? idlep : 0) * vol->iopagesize, h2, sizeof (h2));	/* live-volume skew clamp */
	}
	{
	  long unknown2 = 0;
	  DKNSECTS s2;
	  char mapinfo[40] = "";

	  for (s2 = 0; s2 < vol->nsect_total; s2++)
	    {
	      /* same definition as --check: the volume metadata area is not "unknown" */
	      unknown2 += (vol->stab[s2] && vol->owner[s2] < 0
			   && (long) s2 * VOLMAP_SECT_NPAGES > (long) vol->sys_lastpage);
	    }
	  if (switches > visible_switches)
	    {
	      snprintf (mapinfo, sizeof (mapinfo), " (map shows %ld)", visible_switches);
	    }
	  fprintf (fp, "%spages %lld reserved / %lld allocated (idle %s)  owner switches %ld%s  unknown %ld%s\n",
		   ctx->plain ? "" : VM_DIM, (long long) res * VOLMAP_SECT_NPAGES, (long long) alloc, h2, switches,
		   mapinfo, unknown2, ctx->plain ? "" : VM_RESET);
	}
	if (switches > 100 && visible_switches * 10 < switches)
	  {
	    fprintf (fp, "%s  fragmentation detail mostly hidden by cell zoom - use -f or --check for per-page view%s\n",
		     ctx->plain ? "" : VM_DIM, ctx->plain ? "" : VM_RESET);
	  }
	/* Also report when everything readable was encrypted: printing nothing would
	   read as "the sweep found no data pages" rather than "it could not look". */
	if (ctx->deep && vol->deep_data_pages == 0 && vol->deep_tde_skipped > 0)
	  {
	    fprintf (fp, "%sdeep: no readable data pages - %lld TDE pages excluded (not decrypted)%s\n",
		     ctx->plain ? "" : VM_DIM, (long long) vol->deep_tde_skipped,
		     ctx->plain ? "" : VM_RESET);
	  }
	if (ctx->deep && vol->deep_data_pages > 0)
	  {
	    volmap_human (vol->deep_free_bytes, h1, sizeof (h1));
	    volmap_human ((INT64) vol->deep_data_pages * vol->user_size, h2, sizeof (h2));
	    {
	      char h3[24], h4[24], tde[64] = "";

	      volmap_human (vol->deep_cache_returned, h3, sizeof (h3));
	      volmap_human (vol->deep_cache_kept, h4, sizeof (h4));
	      /* Say how many pages were left out, or the figures look like pages went
	         missing.  The ratios above are taken over the pages actually read, so
	         they stay correct - they just describe the readable part. */
	      if (vol->deep_tde_skipped > 0)
		{
		  snprintf (tde, sizeof (tde), ", %lld TDE pages excluded (not decrypted)",
			    (long long) vol->deep_tde_skipped);
		}
	      fprintf (fp, "%sdeep: data pages %lld, records %lld, in-page free %s of %s, forwarding %.2f%%%s"
		       "  (page cache returned %s, kept %s pre-warm)%s\n",
		       ctx->plain ? "" : VM_DIM, (long long) vol->deep_data_pages, (long long) vol->deep_recs, h1, h2,
		       vol->deep_slots > 0 ? 100.0 * vol->deep_fwd_slots / vol->deep_slots : 0.0, tde, h3, h4,
		       ctx->plain ? "" : VM_RESET);
	    }
	  }

	int prev_maj = -1;
	int tone = 0;		/* flips on every owner change: constant = contiguous file,
				 * rapid flips = interleaved files */

	volmap_vol_hwm_update (vol);
	for (c = 0; c < cells; c++)
	  {
	    long p0 = (long) c * per_pages;
	    VOLMAP_CELL cell;
	    char cbuf[VOLMAP_CELLSTR];

	    if (p0 >= total_pages)
	      {
		break;
	      }
	    if (c % ctx->width == 0)
	      {
		if (c > 0)
		  {
		    fprintf (fp, "%s", ctx->plain ? "" : VM_RESET);
		  }
		fprintf (fp, "%s", (c > 0) ? "\n" : "");
	      }
	    /* tone_before=true: the boundary tone flips ON the first cell of the new
	     * file (the interactive convention; the old batch behavior was off by one) */
	    volmap_cell_classify (ctx, vol, p0, per_pages, total_pages, &prev_maj, &tone, true, &cell);
	    volmap_cell_paint (&cell, ctx, ctx->plain, VOLMAP_PAINT_BG_RESET, cbuf, sizeof (cbuf));
	    fputs (cbuf, fp);
	  }
      }


      fprintf (fp, "%s\n\n", ctx->plain ? "" : VM_RESET);
    }

  /* per-kind summary */
  {
    const char *kinds[] = { "DATA", "INDEX", "CATALOG", "SYSTEM", "ETC(temp)" };
    size_t ki;
    fprintf (fp, "%sBY KIND%s\n", ctx->plain ? "" : VM_BOLD, ctx->plain ? "" : VM_RESET);
    for (ki = 0; ki < DIM (kinds); ki++)
      {
	int cnt = 0;
	INT64 res_pages = 0, alloc_pages = 0;
	for (i = 0; i < ctx->nfiles; i++)
	  {
	    if (volmap_kind_idx (ctx->files[i].ftype) == (int) ki)
	      {
		cnt++;
		res_pages += (INT64) ctx->files[i].n_sector_total * VOLMAP_SECT_NPAGES;
		alloc_pages += ctx->files[i].alloc_pages;
	      }
	  }
	if (cnt == 0)
	  {
	    continue;
	  }
	{
	  int psz = (ctx->nvols > 0) ? ctx->vols[0].iopagesize : 16384;

	  volmap_human (res_pages * psz, h1, sizeof (h1));
	  volmap_human (alloc_pages * psz, h2, sizeof (h2));
	}
	{
	  char h3[32];

	  volmap_human ((res_pages - alloc_pages > 0 ? res_pages - alloc_pages : 0)
			* (ctx->nvols > 0 ? ctx->vols[0].iopagesize : 16384), h3, sizeof (h3));
	  fprintf (fp, "  %-9s files %5d  reserved %9s  allocated %9s  idle %8s (%5.1f%%)\n", kinds[ki], cnt, h1, h2,
		   h3, res_pages > 0 ? 100.0 * (1.0 - (double) alloc_pages / res_pages) : 0.0);
	}
      }
    fprintf (fp, "\n");
  }

}



/* --check: integrity findings gathered from what pass 1 already knows */
/* Fragmentation analysis.
 * The map shows where files are interleaved; what an operator needs is what to
 * act on first.  Walks the already-resident vol->owner[] (sector -> file index)
 * once more, with no additional I/O, and derives:
 *   1. fragmentation % = owner switches / reserved sectors * 100, normalised so
 *      volumes of different sizes compare directly.
 *   2. extents = the number of contiguous sector runs a file occupies.
 *   3. the peer file it shares the most boundaries with.
 */
#define VOLMAP_FRAG_TOPN 5
#define VOLMAP_PEER_SLOTS 8
/* candidate peers a file adjoins, used to pick the one sharing the most boundaries */
typedef struct { int who; long cnt; } VOLMAP_PEERTAB[VOLMAP_PEER_SLOTS];
typedef struct volmap_frag VOLMAP_FRAG;
struct volmap_frag
{
  int file_idx;                 /* index into ctx->files[] */
  int volid;
  long sectors;                 /* sectors this file holds in this volume */
  long extents;                 /* number of contiguous runs */
  int  peer_idx;                /* the file it adjoins most often (-1 if none) */
  long peer_boundaries;         /* boundary count with that peer */
  double density;               /* extents per sector; lower means more contiguous */
};

/* Walk one volume's owner[] and count per-file extents and interleaving peers.
 * out[] must hold at least ctx->nfiles entries.  Returns the number filled. */
static int
volmap_frag_scan (VOLMAP_CTX * ctx, VOLMAP_VOLUME * vol, VOLMAP_FRAG * out, int outmax)
{
  DKNSECTS s;
  int n = 0, i;
  int *idx;                     /* file_idx -> position in out[], -1 if not yet present */
  /* Peer tally: each file keeps several candidate peers and the one sharing the
     most boundaries wins.  With a single slot the first peer seen sticks, so a
     system file brushed once is reported instead of the real pair
     (heap vs index). */
  VOLMAP_PEERTAB *peer;

  if (ctx->nfiles <= 0 || outmax <= 0)
    {
      return 0;
    }
  idx = (int *) malloc (sizeof (int) * (size_t) ctx->nfiles);
  peer = (VOLMAP_PEERTAB *) calloc ((size_t) ctx->nfiles, sizeof (*peer));
  if (idx == NULL || peer == NULL)
    {
      free (idx);
      free (peer);
      return 0;
    }
  for (i = 0; i < ctx->nfiles; i++)
    {
      idx[i] = -1;
    }
  for (s = 0; s < vol->nsect_total; s++)
    {
      int o = vol->owner[s];
      int prev = (s > 0) ? vol->owner[s - 1] : -1;

      if (o < 0 || o >= ctx->nfiles)
	{
	  continue;
	}
      if (idx[o] < 0)
	{
	  if (n >= outmax)
	    {
	      continue;         /* a file is dropped when there is no slot; harmless since only the top N are used */
	    }
	  idx[o] = n;
	  memset (&out[n], 0, sizeof (out[n]));
	  out[n].file_idx = o;
	  out[n].volid = vol->volid;
	  out[n].peer_idx = -1;
	  n++;
	}
      out[idx[o]].sectors++;
      if (prev != o)
	{
	  out[idx[o]].extents++;                 /* a new contiguous run started */
	  if (prev >= 0 && prev < ctx->nfiles)
	    {
	      /* Count the adjoining peer on both sides (once the slots are full further
	         peers are dropped - only the top peer is wanted) */
	      int pass;

	      for (pass = 0; pass < 2; pass++)
		{
		  int self = pass ? prev : o, other = pass ? o : prev, k2;

		  for (k2 = 0; k2 < VOLMAP_PEER_SLOTS; k2++)
		    {
		      if (peer[self][k2].cnt == 0 || peer[self][k2].who == other)
			{
			  peer[self][k2].who = other;
			  peer[self][k2].cnt++;
			  break;
			}
		    }
		}
	    }
	}
    }
  for (i = 0; i < n; i++)
    {
      int k2, best = -1;
      long bestc = 0;

      for (k2 = 0; k2 < VOLMAP_PEER_SLOTS; k2++)
	{
	  if (peer[out[i].file_idx][k2].cnt > bestc)
	    {
	      bestc = peer[out[i].file_idx][k2].cnt;
	      best = peer[out[i].file_idx][k2].who;
	    }
	}
      out[i].peer_idx = (bestc > 0) ? best : -1;
      out[i].peer_boundaries = bestc;
      out[i].density = (out[i].sectors > 0) ? (double) out[i].extents / (double) out[i].sectors : 0.0;
    }
  free (idx);
  free (peer);
  return n;
}

/* Maintenance priority: large and finely split comes first.
 * score = extents * log2(sectors) - more extents and larger size rank higher.
 * (Extents alone fills the top with small files; size alone promotes large
 * files that are already contiguous.) */
static int
volmap_frag_cmp (const void *a, const void *b)
{
  const VOLMAP_FRAG *x = (const VOLMAP_FRAG *) a, *y = (const VOLMAP_FRAG *) b;
  double sx, sy;

  if (x->extents <= 1 && y->extents <= 1)
    {
      return 0;
    }
  sx = (double) x->extents * (x->sectors > 1 ? log2 ((double) x->sectors) : 1.0);
  sy = (double) y->extents * (y->sectors > 1 ? log2 ((double) y->sectors) : 1.0);
  return (sy > sx) - (sy < sx);
}

/* file label, shared by findings and JSON: class name for a heap, class name plus index name for an index */
static const char *
volmap_frag_label (VOLMAP_CTX * ctx, int fidx, char *buf, size_t n)
{
  VOLMAP_FILE *f;

  if (fidx < 0 || fidx >= ctx->nfiles)
    {
      snprintf (buf, n, "(unknown)");
      return buf;
    }
  f = &ctx->files[fidx];
  if (!OID_ISNULL (&f->class_oid))
    {
      const char *cn = volmap_resolve_class_name (ctx, f);

      if (f->ftype == FILE_BTREE || f->ftype == FILE_BTREE_OVERFLOW_KEY)
	{
	  snprintf (buf, n, "%s %s", cn, volmap_resolve_index_name (ctx, f));
	}
      else
	{
	  snprintf (buf, n, "%s", cn);
	}
    }
  else
    {
      snprintf (buf, n, "%s (system)", volmap_ftype_name (f->ftype));
    }
  return buf;
}

/* Ideal sequential read count: what read-ahead (1MB = 1 sector) fetches in one
 * go.  Extents beyond that many break the read up by the difference. */
static long
volmap_frag_ideal_reads (long sectors)
{
  return (sectors > 0) ? sectors : 0;   /* one sector = 1MB = one read-ahead unit */
}

/* Is the block device holding the volume rotational -
   /sys/block/<dev>/queue/rotational.
   Returns 1 rotational (HDD), 0 non-rotational (SSD/NVMe), -1 unknown.
   Fragmentation means something different by orders of magnitude depending on
   the medium, so the reading is reported alongside it. */
static int
volmap_media_rotational (const char *path)
{
  struct stat st;
  char sys[256], name[64];
  FILE *f;
  int rot = -1, major_n, minor_n;

  if (path == NULL || stat (path, &st) != 0)
    {
      return -1;
    }
  major_n = (int) major (st.st_dev);
  minor_n = (int) minor (st.st_dev);
  /* /sys/dev/block/<maj>:<min> may point at a partition; step up to the parent disk if so */
  snprintf (sys, sizeof (sys), "/sys/dev/block/%d:%d/queue/rotational", major_n, minor_n);
  f = fopen (sys, "r");
  if (f == NULL)
    {
      snprintf (sys, sizeof (sys), "/sys/dev/block/%d:%d/../queue/rotational", major_n, minor_n);
      f = fopen (sys, "r");
    }
  if (f != NULL)
    {
      if (fscanf (f, "%d", &rot) != 1)
	{
	  rot = -1;
	}
      fclose (f);
    }
  if (rot < 0)
    {
      /* On a container overlay filesystem st_dev is not a real block device
         (major 0).  In that case find the mount covering this path in
         /proc/self/mountinfo and retry with its source device; if that fails,
         report -1 (unknown) rather than guessing. */
      FILE *mi = fopen ("/proc/self/mountinfo", "r");
      char line[4096], best[256] = "";
      size_t bestlen = 0;

      if (mi != NULL)
	{
	  while (fgets (line, sizeof (line), mi))
	    {
	      char mp[1024], src[256];
	      int maj = -1, min = -1;
	      char *sep = strstr (line, " - ");

	      if (sep == NULL || sscanf (line, "%*d %*d %d:%d %*s %1023s", &maj, &min, mp) != 3)
		{
		  continue;
		}
	      if (sscanf (sep, " - %*s %255s", src) != 1 || src[0] != '/')
		{
		  continue;
		}
	      /* does the longest (most specific) mount point contain this path */
	      {
		size_t l = strlen (mp);

		if (l > bestlen && strncmp (path, mp, l) == 0 && (mp[l - 1] == '/' || path[l] == '/' || path[l] == '\0'))
		  {
		    bestlen = l;
		    snprintf (best, sizeof (best), "%s", src);
		  }
	      }
	    }
	  fclose (mi);
	}
      if (best[0] != '\0' && stat (best, &st) == 0 && S_ISBLK (st.st_mode))
	{
	  snprintf (sys, sizeof (sys), "/sys/dev/block/%d:%d/queue/rotational",
		    (int) major (st.st_rdev), (int) minor (st.st_rdev));
	  f = fopen (sys, "r");
	  if (f == NULL)
	    {
	      snprintf (sys, sizeof (sys), "/sys/dev/block/%d:%d/../queue/rotational",
			(int) major (st.st_rdev), (int) minor (st.st_rdev));
	      f = fopen (sys, "r");
	    }
	  if (f != NULL)
	    {
	      if (fscanf (f, "%d", &rot) != 1)
		{
		  rot = -1;
		}
	      fclose (f);
	    }
	}
    }
  (void) name;
  return rot;
}

/* medium-specific qualifier, so the verdict is not passed off as a single number */
static const char *
volmap_media_note (int rot)
{
  return (rot == 1) ? "rotational media: extra seeks are costly"
    : (rot == 0) ? "non-rotational media: significant only for low queue-depth access"
    : "media type unknown";
}

static int
volmap_findings (VOLMAP_CTX * ctx, FILE * fp, bool as_json)
{
  int vi, fi, nfind = 0, nnote = 0;

/* The JSON and human forms share one argument list.  The human form sometimes
 * appends derived values (read restarts, media type), so JSON may carry fewer
 * specifiers and fprintf then ignores the extras - defined behaviour, and
 * intended here, so only -Wformat-extra-args is suppressed for this macro. */
#define VM_FINDING(json_fmt, human_fmt, ...) \
  do { \
    nfind++; \
    if (as_json) \
      { \
	_Pragma ("GCC diagnostic push") \
	_Pragma ("GCC diagnostic ignored \"-Wformat-extra-args\"") \
	fprintf (fp, "%s    " json_fmt, (nfind > 1) ? ",\n" : "", __VA_ARGS__); \
	_Pragma ("GCC diagnostic pop") \
      } \
    else \
      { \
	fprintf (fp, "  " human_fmt "\n", __VA_ARGS__); \
      } \
  } while (0)
/* A note is a property of the database worth saying next to the findings, not an
 * irregular structure: it does not count towards --check's exit status.  JSON collects
 * them for a notes[] array the caller prints after findings[]. */
#define VM_NOTE(json_fmt, human_fmt, ...) \
  do { \
    nnote++; \
    if (as_json) \
      { \
	int left_ = (int) sizeof (ctx->notes_json) - ctx->notes_len; \
	if (left_ > 0) \
	  { \
	    _Pragma ("GCC diagnostic push") \
	    _Pragma ("GCC diagnostic ignored \"-Wformat-extra-args\"") \
	    int w_ = snprintf (ctx->notes_json + ctx->notes_len, (size_t) left_, "%s    " json_fmt, (nnote > 1) ? ",\n" : "", __VA_ARGS__); \
	    _Pragma ("GCC diagnostic pop") \
	    ctx->notes_len += (w_ < 0) ? 0 : (w_ >= left_) ? left_ - 1 : w_; \
	  } \
      } \
    else \
      { \
	fprintf (fp, "  note: " human_fmt "\n", __VA_ARGS__); \
      } \
  } while (0)
  ctx->notes_len = 0;
  ctx->notes_json[0] = '\0';

  if (!as_json)
    {
      fprintf (fp, "%sFINDINGS%s\n", ctx->plain ? "" : VM_BOLD, ctx->plain ? "" : VM_RESET);
    }
  for (vi = 0; vi < ctx->nskipped; vi++)
    {
      char ep[2 * PATH_MAX], er[512];

      if (!volmap_vol_selected (ctx, ctx->skipped[vi].listed_volid))
	{
	  continue;
	}
      if (ctx->skipped[vi].cause == VOLMAP_SKIP_DAMAGED)
	{
	  VM_FINDING ("{\"finding\": \"volume_header_damaged\", \"listed_volid\": %d, \"path\": \"%s\", \"reason\": \"%s\"}",
		      "[vol %d] %s: %s - volume not shown, its header is not a valid volume header",
		      ctx->skipped[vi].listed_volid, volmap_json_escape (ctx->skipped[vi].path, ep, (int) sizeof (ep)),
		      volmap_json_escape (ctx->skipped[vi].reason, er, (int) sizeof (er)));
	}
      else if (ctx->skipped[vi].cause == VOLMAP_SKIP_UNRECOGNIZED)
	{
	  VM_FINDING ("{\"finding\": \"volume_header_unrecognized\", \"listed_volid\": %d, \"path\": \"%s\", \"reason\": \"%s\"}",
		      "[vol %d] %s: %s - volume not shown, a CUBRID volume header volmap cannot read",
		      ctx->skipped[vi].listed_volid, volmap_json_escape (ctx->skipped[vi].path, ep, (int) sizeof (ep)),
		      volmap_json_escape (ctx->skipped[vi].reason, er, (int) sizeof (er)));
	}
    }
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      DKNSECTS s;
      long unknown = 0, first_unknown = -1;
      long res = 0;
      INT64 alloc = 0;

      /* -V narrows the findings as it narrows the volumes: --check's exit status
         must speak for the volumes asked about, not the rest of the database */
      if (!volmap_vol_selected (ctx, vol->volid))
	{
	  continue;
	}

      for (s = 0; s < vol->nsect_total; s++)
	{
	  res += vol->stab[s];
	  alloc += vol->alloc[s];
	  if (vol->stab[s] && vol->owner[s] < 0 && (long) s * VOLMAP_SECT_NPAGES > (long) vol->sys_lastpage)
	    {
	      if (first_unknown < 0)
		{
		  first_unknown = s;
		}
	      unknown++;
	    }
	}
      if (unknown > 0)
	{
	  VM_FINDING ("{\"finding\": \"unknown_owner_sectors\", \"volid\": %d, \"count\": %ld, \"first_sectid\": %ld}",
		      "[vol %d] reserved sectors with no owning file: %ld (first sectid %ld) - tracker/page mismatch?",
		      vol->volid, unknown, first_unknown);
	}
      if (alloc > res * (INT64) VOLMAP_SECT_NPAGES)
	{
	  VM_FINDING ("{\"finding\": \"alloc_exceeds_reserved\", \"volid\": %d}",
		      "[vol %d] allocated pages exceed reserved pages - live-volume snapshot skew (rerun/refresh)",
		      vol->volid);
	}
      if (ctx->warn_idle_pct > 0 && res > 0)
	{
	  double ip = 100.0 * (1.0 - (double) alloc / ((double) res * VOLMAP_SECT_NPAGES));

	  if (ip >= (double) ctx->warn_idle_pct)
	    {
	      VM_FINDING ("{\"finding\": \"idle_over_threshold\", \"volid\": %d, \"idle_pct\": %.2f, \"threshold\": %d}",
			  "[vol %d] idle space %.2f%% exceeds --warn-idle=%d - reserved-but-unused space worth reclaiming",
			  vol->volid, ip, ctx->warn_idle_pct);
	    }
	}
      if (vol->tde_pages > 0)
	{
	  /* Encryption is how the database was made, not an irregularity: a note, so a
	     TDE database does not fail --check on that alone */
	  VM_NOTE ("{\"note\": \"tde_encrypted_pages\", \"volid\": %d, \"sampled\": %ld}",
		   "[vol %d] TDE-encrypted pages (%ld sampled) - contents not interpretable without the key; not a finding",
		   vol->volid, vol->tde_pages);
	}
      /* Fragmentation finding.
       * Judged on the ratio to reserved sectors rather than absolute switches,
       * so volume size does not drown it out.
       * Threshold 2%: healthy volumes measured 0.28-1.09%, a problem volume 5.76%.
       * Only physical quantities are reported - seek cost is not measured here
       * and barely applies to SSD/NVMe. */
      {
	long fswitch = 0, freserved = 0;
	DKNSECTS s3;

	for (s3 = 0; s3 < vol->nsect_total; s3++)
	  {
	    freserved += vol->stab[s3];
	    if (s3 > 0 && vol->owner[s3] >= 0 && vol->owner[s3 - 1] >= 0 && vol->owner[s3] != vol->owner[s3 - 1])
	      {
		fswitch++;
	      }
	  }
	if (freserved >= 1000 && fswitch * 100 > freserved * 2)
	  {
	    VOLMAP_FRAG *fr = (VOLMAP_FRAG *) malloc (sizeof (VOLMAP_FRAG) * (size_t) (ctx->nfiles > 0 ? ctx->nfiles : 1));
	    double pct = 100.0 * (double) fswitch / (double) freserved;

	    int rot = volmap_media_rotational (vol->path);

	    VM_FINDING ("{\"finding\": \"fragmentation\", \"volid\": %d, \"pct\": %.2f,"
			" \"owner_switches\": %ld, \"sectors_reserved\": %ld,"
			" \"media_rotational\": %d, \"media_note\": \"%s\"}",
			"[vol %d] fragmentation %.2f%% (%ld owner switches / %ld reserved sectors)"
			" - see compaction_priority for which objects are interleaved"
			" [rotational=%d: %s]",
			vol->volid, pct, fswitch, freserved, rot, volmap_media_note (rot));
	    if (fr != NULL)
	      {
		int nfr = volmap_frag_scan (ctx, vol, fr, ctx->nfiles), t;

		qsort (fr, (size_t) nfr, sizeof (VOLMAP_FRAG), volmap_frag_cmp);
		for (t = 0; t < nfr && t < 2; t++)
		  {
		    char lab[192], plab[192], elab[2 * 192], eplab[2 * 192];
		    const char *olab, *oplab;

		    if (fr[t].extents <= 1 || fr[t].peer_idx < 0)
		      {
			break;
		      }
		    volmap_frag_label (ctx, fr[t].file_idx, lab, sizeof (lab));
		    volmap_frag_label (ctx, fr[t].peer_idx, plab, sizeof (plab));
		    /* the same arguments feed the JSON and the text form: escape for JSON only */
		    olab = as_json ? volmap_json_escape (lab, elab, (int) sizeof (elab)) : lab;
		    oplab = as_json ? volmap_json_escape (plab, eplab, (int) sizeof (eplab)) : plab;
		    VM_FINDING ("{\"finding\": \"fragmented_object\", \"volid\": %d, \"object\": \"%s\","
				" \"sectors\": %ld, \"extents\": %ld, \"interleaved_with\": \"%s\", \"boundaries\": %ld}",
				"[vol %d] '%s' occupies %ld sectors in %ld extents,"
				" interleaved with '%s' at %ld boundaries"
				" (sequential read restarts %ld times vs %ld ideal) [%s]",
				vol->volid, olab, fr[t].sectors, fr[t].extents, oplab, fr[t].peer_boundaries,
				fr[t].extents, (long) 1, volmap_media_note (volmap_media_rotational (vol->path)));
		  }
		free (fr);
	      }
	  }
      }
      if (ctx->bufmap_loaded && vol->bufpg != NULL && vol->purpose != DB_TEMPORARY_DATA_PURPOSE)
	{
	  /* cross-check of the two tools: a page cub_top saw in the buffer pool must be an
	   * allocated page here.  A few can differ legitimately (deallocated between the two
	   * snapshots); a large count means the snapshot is stale or from another database.
	   * Temporary-purpose volumes are skipped: their pages are freed as soon as the query
	   * ends while the BCBs keep them until victimized (measured: 1141 of 2096 on cbench). */
	  long p, npv = (long) vol->nsect_total * VOLMAP_SECT_NPAGES, unalloc = 0, first = -1;

	  for (p = 0; p < npv; p++)
	    {
	      if (vol->bufpg[p] != 0 && (long) p > (long) vol->sys_lastpage
		  && !(vol->pagebm[p / VOLMAP_SECT_NPAGES] & ((UINT64) 1 << (p % VOLMAP_SECT_NPAGES))))
		{
		  if (first < 0)
		    {
		      first = p;
		    }
		  unalloc++;
		}
	    }
	  if (unalloc > 0 && vol->buf_total >= 64 && unalloc * 10 > vol->buf_total * 9)
	    {
	      /* deallocated pages legitimately linger in the pool (measured: 957 of 2386 on
	       * demodb after drops) - only a near-total mismatch is evidence of a stale or
	       * foreign snapshot.  Smaller counts show as 'freed N' in the volume header. */
	      VM_FINDING ("{\"finding\": \"bufmap_snapshot_mismatch\", \"volid\": %d, \"unallocated\": %ld, \"buffered\": %ld, \"first_pageid\": %ld}",
			  "[vol %d] %ld of %ld buffered pages are not allocated on disk (first page %ld) - stale or mismatched --bufmap snapshot",
			  vol->volid, unalloc, vol->buf_total, first);
	    }
	}
    }
  for (fi = 0; fi < ctx->nfiles; fi++)
    {
      if (ctx->files[fi].sectors_seen == 0 && ctx->files[fi].n_sector_total > 0
	  && volmap_vol_selected (ctx, ctx->files[fi].vfid.volid))
	{
	  VM_FINDING ("{\"finding\": \"stale_file_entry\", \"vfid\": \"%d|%d\"}",
		      "[file %d|%d] header exists but no sectors matched - dropped/reused during scan",
		      (int) ctx->files[fi].vfid.volid, (int) ctx->files[fi].vfid.fileid);
	}
    }
  if (nfind == 0 && !as_json)
    {
      fprintf (fp, "  no findings - ownership fully attributed, no skew\n");
    }
  if (!as_json)
    {
      fprintf (fp, "\n");
    }
#undef VM_FINDING
#undef VM_NOTE
  return nfind;
}


/* --format json: machine-readable dump of volumes, files and findings */
/* minimal JSON string escaping for paths/names (backslash, quote, control bytes) */
static const char *
volmap_json_escape (const char *in, char *out, int outsz)
{
  int oi = 0;
  const unsigned char *p = (const unsigned char *) in;

  for (; *p != '\0' && oi < outsz - 7; p++)
    {
      if (*p == '"' || *p == '\\')
	{
	  out[oi++] = '\\';
	  out[oi++] = *p;
	}
      else if (*p < 0x20)
	{
	  oi += snprintf (out + oi, outsz - oi, "\\u%04x", *p);
	}
      else
	{
	  out[oi++] = *p;
	}
    }
  out[oi] = '\0';
  return out;
}

static int
volmap_output_json (VOLMAP_CTX * ctx, const char *db_name)
{
  int nvol_printed = 0, nvol_selected = 0;
  FILE *fp = ctx->outfp;
  int vi, fi;

  {
    char edb[1024];

    time_t now = time (NULL);
    char ts[32];

    strftime (ts, sizeof (ts), "%Y-%m-%d %H:%M:%S", localtime (&now));
    char erel[64];

    char ewhy[2 * sizeof (ctx->tracker_why)];

    fprintf (fp, "{\n  \"db\": \"%s\", \"tool\": \"volmap\", \"timestamp\": \"%s\",\n"
	     "  \"release\": \"%s\", \"shutdown_state\": \"%s\",\n",
	     volmap_json_escape (db_name, edb, (int) sizeof (edb)), ts,
	     volmap_json_escape (ctx->db_release, erel, (int) sizeof (erel)), volmap_shutdown_name (ctx->shutdown_state));
    fprintf (fp, "  \"discovery\": {\"tracker\": \"%s\", \"tracker_files\": %d, \"probed_sectors\": %ld, \"reason\": \"%s\"},\n",
	     ctx->tracker_state == 1 ? "read" : ctx->no_tracker ? "off" : "unreadable",
	     ctx->tracker_state == 1 ? ctx->files_tracked : 0, ctx->probe_sectors,
	     volmap_json_escape (ctx->tracker_why, ewhy, (int) sizeof (ewhy)));
    /* beside the database: the server, a utility on it, the disk (volmap_proc_scan) */
    volmap_io_window (ctx);
    fprintf (fp, "  \"server\": {\"state\": \"%s\", \"pid\": %ld, \"registered\": %s, \"up_sec\": %ld, \"log_lock_pid\": %ld},\n",
	     volmap_server_state_name (ctx), (long) ctx->proc.server_pid,
	     ctx->proc.server_pid <= 0 || ctx->proc.server_registered == -1 ? "null"
	     : ctx->proc.server_registered == 1 ? "true" : "false",
	     ctx->proc.server_pid > 0 ? ctx->proc.server_elapsed : -1L, (long) ctx->proc.log_lock_pid);
    if (ctx->proc.util[0] != '\0')
      {
	fprintf (fp, "  \"maintenance\": {\"utility\": \"%s\", \"pid\": %ld, \"elapsed_sec\": %ld},\n", ctx->proc.util,
		 (long) ctx->proc.util_pid, ctx->proc.util_elapsed);
      }
    else
      {
	fprintf (fp, "  \"maintenance\": null,\n");
      }
    if (ctx->io.state == 1 && ctx->io.t.tv_sec != 0)
      {
	fprintf (fp, "  \"disk\": {\"device\": \"%s\", \"busy_pct\": %.1f, \"mb_s\": %.1f, \"server_mb_s\": ", ctx->io.dev,
		 ctx->io.util_pct, ctx->io.mb_s);
	if (ctx->io.srv_ok == 1)
	  {
	    fprintf (fp, "%.1f},\n", ctx->io.srv_mb_s);
	  }
	else
	  {
	    fprintf (fp, "null},\n");
	  }
      }
    else
      {
	fprintf (fp, "  \"disk\": null,\n");
      }
    fprintf (fp, "  \"volumes\": [\n");
  }
  /* -V selects the volumes to report; the batch renderer honours it, so JSON must too
     or an automation asking for one volume gets the whole database.  The separator is
     driven by what has actually been printed, not by the loop index - skipping a
     volume with an index-based comma would emit a trailing one and break the parse. */
  /* -V can filter volumes out, so the last one to be printed is not known from the
     index: count them first, then the comma can go where the files array puts it. */
  nvol_selected = 0;
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      nvol_selected += volmap_vol_selected (ctx, ctx->vols[vi].volid);
    }
  nvol_printed = 0;
  for (vi = 0; vi < ctx->nvols; vi++)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      DKNSECTS s;
      long res = 0, unknown = 0, switches = 0;

      if (!volmap_vol_selected (ctx, vol->volid))
	{
	  continue;
	}
      INT64 alloc = 0;

      for (s = 0; s < vol->nsect_total; s++)
	{
	  res += vol->stab[s];
	  alloc += vol->alloc[s];
	  unknown += (vol->stab[s] && vol->owner[s] < 0
		      && (long) s * VOLMAP_SECT_NPAGES > (long) vol->sys_lastpage);
	  if (s > 0 && vol->owner[s] >= 0 && vol->owner[s - 1] >= 0 && vol->owner[s] != vol->owner[s - 1])
	    {
	      switches++;
	    }
	}
      char epath[2 * PATH_MAX];

      fprintf (fp, "    {\"volid\": %d, \"path\": \"%s\", \"purpose\": \"%s\", \"iopagesize\": %d,"
	       " \"sectors_total\": %d,"
	       " \"sectors_reserved\": %ld, \"pages_allocated\": %lld, \"owner_switches\": %ld,"
	       " \"unknown_sectors\": %ld, \"tde_pages_probed\": %ld,"
	       " \"idle_pages\": %lld, \"idle_pct\": %.3f, \"fragmentation_pct\": %.3f,"
	       " \"media_rotational\": %d,"
	       " \"buffered_pages\": %ld, \"unflushed_pages\": %ld, \"flushing_pages\": %ld, \"buffered_freed_pages\": %ld}%s\n",
	       vol->volid, volmap_json_escape (vol->path, epath, (int) sizeof (epath)),
	       (vol->purpose == DB_TEMPORARY_DATA_PURPOSE) ? "temporary" : "permanent", vol->iopagesize,
	       vol->nsect_total, res, (long long) alloc, switches,
	       unknown, vol->tde_pages,
	       /* idle = pages reserved but not yet used; the primary signal for capacity trends */
	       (long long) (res * (INT64) VOLMAP_SECT_NPAGES - alloc > 0
			    ? res * (INT64) VOLMAP_SECT_NPAGES - alloc : 0),
	       (res > 0) ? 100.0 * (double) (res * (INT64) VOLMAP_SECT_NPAGES - alloc)
			   / (double) (res * (INT64) VOLMAP_SECT_NPAGES) : 0.0,
	       /* Fragmentation ratio - absolute switches drown in volume size, so normalise
	          by reserved sectors.  The medium (rotational or not) is reported with
	          it: the same figure means very different things on HDD and SSD. */
	       (res > 0) ? 100.0 * (double) switches / (double) res : 0.0,
	       volmap_media_rotational (vol->path),
	       ctx->bufmap_loaded ? vol->buf_total : -1L,
	       ctx->bufmap_loaded ? vol->buf_dirty : -1L, ctx->bufmap_loaded ? vol->buf_flushing : -1L,
	       ctx->bufmap_loaded ? vol->buf_freed : -1L,
	       (nvol_printed < nvol_selected - 1) ? "," : "");
      nvol_printed++;
    }
  fprintf (fp, "  ],\n");
  if (ctx->bufmap_loaded)
    {
      char la[32], ln[32], le[32], lo[32];

      volmap_lsa_str (ctx->bm_log_append, la, sizeof (la));
      volmap_lsa_str (ctx->bm_log_nxio, ln, sizeof (ln));
      volmap_lsa_str (ctx->bm_log_eof, le, sizeof (le));
      volmap_lsa_str (ctx->bm_oldest, lo, sizeof (lo));
      fprintf (fp, "  \"bufmap\": {\"snapshot_epoch\": %lld, \"num_buffers\": %ld, \"resident\": %ld, \"dirty\": %ld,"
	       " \"log_append_lsa\": \"%s\", \"log_flushed_lsa\": \"%s\", \"log_eof_lsa\": \"%s\", \"oldest_dirty_lsa\": \"%s\"},\n",
	       (long long) ctx->bm_ts, ctx->bm_nbuf, ctx->bm_nrec, ctx->bm_dirty, la, ln, le, lo);
    }
  fprintf (fp, "  \"files\": [\n");
  for (fi = 0; fi < ctx->nfiles; fi++)
    {
      VOLMAP_FILE *f = &ctx->files[fi];
      const char *name = "";
      const char *iname = "";
      /* Class and index names come off the disk records, and CUBRID allows a quoted
         identifier to contain " and \\ - unescaped they would terminate the JSON
         string and produce output no parser accepts. */
      /* Worst case is every byte escaped as \\uXXXX, so six bytes out per byte in. */
      char ename[sizeof (f->class_name) * 6 + 8];
      char einame[sizeof (f->index_name) * 6 + 8];

      if (!OID_ISNULL (&f->class_oid))
	{
	  name = volmap_resolve_class_name (ctx, f);
	  if (f->ftype == FILE_BTREE || f->ftype == FILE_BTREE_OVERFLOW_KEY)
	    {
	      iname = volmap_resolve_index_name (ctx, f);
	    }
	}
      name = volmap_json_escape (name, ename, (int) sizeof (ename));
      iname = volmap_json_escape (iname, einame, (int) sizeof (einame));
      fprintf (fp, "    {\"vfid\": \"%d|%d\", \"type\": \"%s\", \"class\": \"%s\", \"index\": \"%s\","
	       " \"pages_total\": %d, \"pages_user\": %d, \"pages_free\": %d, \"pages_ftab\": %d,"
	       " \"sectors\": %d, \"alloc_pages\": %lld, \"tracked\": %s}%s\n",
	       (int) f->vfid.volid, (int) f->vfid.fileid, volmap_ftype_name (f->ftype), name, iname,
	       f->n_page_total, f->n_page_user, f->n_page_free, f->n_page_ftab, f->n_sector_total,
	       (long long) f->alloc_pages,
	       /* true: listed; "deleted": listed as dropped, awaiting vacuum; false: found by
	          probing only; null: the tracker was not read, so nothing is known either way */
	       ctx->tracker_state != 1 ? "null" : f->tracked == 1 ? "true" : f->tracked == 2 ? "\"deleted\"" : "false",
	       (fi < ctx->nfiles - 1) ? "," : "");
    }
  fprintf (fp, "  ],\n");
  /* per-kind totals: what is growing, on one line */
  {
    static const char *kn[5] = { "data", "index", "catalog", "system", "temp" };
    INT64 kp[5] = { 0, 0, 0, 0, 0 };
    long kf[5] = { 0, 0, 0, 0, 0 };
    int k;

    for (fi = 0; fi < ctx->nfiles; fi++)
      {
	int ki = volmap_kind_idx (ctx->files[fi].ftype);

	if (ki < 0 || ki > 4)
	  {
	    continue;
	  }
	kp[ki] += ctx->files[fi].alloc_pages;
	kf[ki]++;
      }
    fprintf (fp, "  \"by_kind\": {");
    for (k = 0; k < 5; k++)
      {
	fprintf (fp, "%s\"%s\": {\"files\": %ld, \"pages\": %lld}", k ? ", " : "", kn[k], kf[k], (long long) kp[k]);
      }
    fprintf (fp, "},\n");
  }
  /* fragmentation top-N: which objects to maintain first */
  {
    VOLMAP_FRAG *fr = (VOLMAP_FRAG *) malloc (sizeof (VOLMAP_FRAG) * (size_t) (ctx->nfiles > 0 ? ctx->nfiles : 1));
    int printed = 0;

    fprintf (fp, "  \"compaction_priority\": [\n");
    if (fr != NULL)
      {
	for (vi = 0; vi < ctx->nvols; vi++)
	  {
	    VOLMAP_VOLUME *vol = &ctx->vols[vi];
	    int nfr = volmap_frag_scan (ctx, vol, fr, ctx->nfiles);
	    int t;

	    if (nfr <= 0)
	      {
		continue;
	      }
	    qsort (fr, (size_t) nfr, sizeof (VOLMAP_FRAG), volmap_frag_cmp);
	    for (t = 0; t < nfr && t < VOLMAP_FRAG_TOPN; t++)
	      {
		char lab[192], elab[2 * 192], plab[192], eplab[2 * 192];

		if (fr[t].extents <= 1)
		  {
		    break;      /* contiguous files need no maintenance */
		  }
		volmap_frag_label (ctx, fr[t].file_idx, lab, sizeof (lab));
		volmap_frag_label (ctx, fr[t].peer_idx, plab, sizeof (plab));
		fprintf (fp, "%s    {\"volid\": %d, \"object\": \"%s\", \"type\": \"%s\","
			 " \"sectors\": %ld, \"extents\": %ld, \"extents_per_1k_sectors\": %.1f,"
			 " \"ideal_sequential_reads\": %ld, \"actual_extent_starts\": %ld,"
			 " \"interleaved_with\": \"%s\", \"boundaries\": %ld}",
			 printed ? ",\n" : "", fr[t].volid,
			 volmap_json_escape (lab, elab, (int) sizeof (elab)),
			 volmap_ftype_name (ctx->files[fr[t].file_idx].ftype),
			 fr[t].sectors, fr[t].extents, fr[t].density * 1000.0,
			 volmap_frag_ideal_reads (fr[t].sectors), fr[t].extents,
			 (fr[t].peer_idx >= 0) ? volmap_json_escape (plab, eplab, (int) sizeof (eplab)) : "",
			 fr[t].peer_boundaries);
		printed++;
	      }
	  }
	free (fr);
      }
    fprintf (fp, "%s  ],\n", printed ? "\n" : "");
  }
  fprintf (fp, "  \"skipped_volumes\": [");
  for (int si = 0, sn = 0; si < ctx->nskipped; si++)
    {
      char ep[2 * PATH_MAX], er[512];

      if (!volmap_vol_selected (ctx, ctx->skipped[si].listed_volid))
	{
	  continue;
	}
      static const char *const cause_name[] = { "unreadable", "damaged", "unrecognized", "unsupported_release" };

      fprintf (fp, "%s\n    {\"listed_volid\": %d, \"path\": \"%s\", \"damaged\": %s, \"cause\": \"%s\", \"reason\": \"%s\"}",
	       sn++ ? "," : "", ctx->skipped[si].listed_volid,
	       volmap_json_escape (ctx->skipped[si].path, ep, (int) sizeof (ep)),
	       ctx->skipped[si].cause == VOLMAP_SKIP_DAMAGED ? "true" : "false", cause_name[ctx->skipped[si].cause],
	       volmap_json_escape (ctx->skipped[si].reason, er, (int) sizeof (er)));
    }
  fprintf (fp, "%s],\n", volmap_skipped_selected (ctx) ? "\n  " : "");
  fprintf (fp, "  \"findings\": [\n");
  int nfind = volmap_findings (ctx, fp, true);
  fprintf (fp, "%s  ],\n", nfind ? "\n" : "");
  /* notes: properties worth knowing (TDE), kept apart from findings and the exit status */
  fprintf (fp, "  \"notes\": [%s%s%s]\n}\n", ctx->notes_len ? "\n" : "", ctx->notes_json, ctx->notes_len ? "\n  " : "");
  return nfind;
}

/* Scan the directory for temp volumes ("<db>_t<NNNNN>").
   These never appear in the vinf - they are created while a query spills and
   dropped afterwards - so they are found by name pattern instead.

   Also called from [r]: temp volumes that vanished are removed from the list and
   new ones are added.  Permanent volumes (listed in the vinf) are left alone.
   Returns 1 if the list changed, 0 otherwise.

   Non-invasive: readdir + open/pread only, never writes to the database. */

/* Open every <db>_t<NNN> in one directory that is not held already.  "Held" is
   decided by volid, which is unique in a database, not by path: the same file is
   reached by several spellings (a trailing slash, a symlink, a relative vinf). */
static int
volmap_scan_temp_dir (VOLMAP_CTX * ctx, const char *dir, const char *prefix, size_t plen)
{
  DIR *dh;
  struct dirent *de;
  int changed = 0;

  dh = opendir (dir);
  if (dh == NULL)
    {
      return 0;
    }
  while ((de = readdir (dh)) != NULL)
    {
      char tpath[PATH_MAX];
      int have = 0, vi;

      if (strncmp (de->d_name, prefix, plen) != 0 || de->d_name[plen] < '0' || de->d_name[plen] > '9')
	{
	  continue;
	}
      /* The name carries the volid: fileio_make_volume_temp_name () writes
         <db>_t<volid>, unchanged from 10.1 to 11.5.  Reading it here means an
         unselected volume is never opened at all - it costs no fd, no header read,
         and does not grow the volume array (about 300 bytes a volume). */
      char *vend = NULL;
      long tvolid = strtol (de->d_name + plen, &vend, 10);
      int named = (vend != NULL && *vend == '\0');

      if (named && !volmap_vol_selected (ctx, (int) tvolid))
	{
	  continue;
	}
      if ((size_t) snprintf (tpath, sizeof (tpath), "%s/%s", dir, de->d_name) >= sizeof (tpath))
	{
	  continue;
	}
      for (vi = 0; vi < ctx->nvols; vi++)
	{
	  if (named ? ctx->vols[vi].volid == tvolid : strcmp (ctx->vols[vi].path, tpath) == 0)
	    {
	      have = 1;
	      break;
	    }
	}
      if (!have && volmap_open_volume (ctx, tpath) == NO_ERROR)
	{
	  changed = 1;
	}
    }
  closedir (dh);
  return changed;
}

/* Same directory, however each path is spelled (trailing slash, symlink, relative) */
static int
volmap_same_dir (const char *a, const char *b)
{
  struct stat sa, sb;

  return stat (a, &sa) == 0 && stat (b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int
volmap_scan_temp_volumes (VOLMAP_CTX * ctx)
{
  char dircopy[PATH_MAX], basecopy[PATH_MAX], prefix[PATH_MAX];
  char *dirp, *basep, *suffix;
  const char *live, *other;
  size_t plen;
  int vi, changed = 0;

  if (ctx->vinf_path[0] == '\0')
    {
      return 0;
    }
  snprintf (dircopy, sizeof (dircopy), "%s", ctx->vinf_path);
  snprintf (basecopy, sizeof (basecopy), "%s", ctx->vinf_path);
  dirp = dirname (dircopy);
  basep = basename (basecopy);
  suffix = strstr (basep, "_vinf");
  if (suffix != NULL)
    {
      *suffix = '\0';
    }
  if (snprintf (prefix, sizeof (prefix), "%s_t", basep) <= 0)
    {
      return 0;
    }
  plen = strlen (prefix);

  /* The engine creates and removes temp volumes only in temp_volume_path, or in the
     database directory when that is unset (boot_make_temp_volume_fullname).  After
     the setting changes, files left in the other directory are never cleaned up, so
     the engine's directory is the live one and wins a volid clash. */
  live = dirp;
  other = NULL;
  if (ctx->temp_path[0] != '\0' && !volmap_same_dir (ctx->temp_path, dirp))
    {
      live = ctx->temp_path;
      other = dirp;
    }

  /* Drop temp volumes that vanished - keeping one whose file is gone draws an
     empty shell - and leftovers outside the live directory once the live one has
     the same file.  Non-temp volumes (listed in the vinf) are left alone, told
     apart by name. */
  for (vi = 0; vi < ctx->nvols;)
    {
      VOLMAP_VOLUME *vol = &ctx->vols[vi];
      const char *b = strrchr (vol->path, '/');
      int drop = 0;

      b = (b != NULL) ? b + 1 : vol->path;
      if (strncmp (b, prefix, plen) == 0 && b[plen] >= '0' && b[plen] <= '9')
	{
	  drop = (access (vol->path, R_OK) != 0);
	  if (!drop && other != NULL)
	    {
	      char vdir[PATH_MAX], lpath[PATH_MAX];

	      snprintf (vdir, sizeof (vdir), "%s", vol->path);
	      if (!volmap_same_dir (dirname (vdir), live)
		  && (size_t) snprintf (lpath, sizeof (lpath), "%s/%s", live, b) < sizeof (lpath)
		  && access (lpath, R_OK) == 0)
		{
		  drop = 1;
		}
	    }
	}
      if (drop)
	{
	  volmap_vol_release (vol);
	  /* close the gap by shifting the array down (volume order is also screen order) */
	  if (vi + 1 < ctx->nvols)
	    {
	      memmove (&ctx->vols[vi], &ctx->vols[vi + 1],
		       (size_t) (ctx->nvols - vi - 1) * sizeof (ctx->vols[0]));
	    }
	  ctx->nvols--;
	  memset (&ctx->vols[ctx->nvols], 0, sizeof (ctx->vols[0]));
	  changed = 1;
	  continue;		/* re-examine the same position */
	}
      vi++;
    }

  /* add newly created temp volumes, skipping volumes already held: the live
     directory first, so its volume wins a volid clash, then the other one, whose
     files are shown only when the live directory has no volume of that volid. */
  changed |= volmap_scan_temp_dir (ctx, live, prefix, plen);
  if (other != NULL)
    {
      changed |= volmap_scan_temp_dir (ctx, other, prefix, plen);
    }
  return changed;
}

/* Read temp_volume_path out of cubrid.conf.
 *
 * The engine puts temp volumes in that directory when it is set, falling back to
 * the database directory when it is not (boot_sr.c, unchanged from 10.1 to 11.5).
 * Spill volumes on a separate disk are a common setup, so both directories are
 * scanned - otherwise those volumes are missing from the map, from BY KIND temp
 * and from --check.
 *
 * Section precedence follows the engine: [common] first, then [@<db>] overrides
 * it.  The file is $CUBRID_CONF_FILE if set, else $CUBRID/conf/cubrid.conf.
 * Returns false when there is no setting - then only the database directory is
 * scanned, as before. */
static bool
volmap_conf_temp_path (const char *db_name, char *out, size_t outsz)
{
  char path[PATH_MAX];
  char want[256];
  char line[PATH_MAX + 64];
  const char *env;
  bool in_common = false, in_db = false, from_db = false;
  bool found = false;
  FILE *fp;

  out[0] = '\0';
  env = getenv ("CUBRID_CONF_FILE");
  if (env != NULL && env[0] != '\0')
    {
      snprintf (path, sizeof (path), "%s", env);
    }
  else
    {
      const char *root = getenv ("CUBRID");

      if (root == NULL || root[0] == '\0')
	{
	  return false;
	}
      snprintf (path, sizeof (path), "%s/conf/cubrid.conf", root);
    }
  fp = fopen (path, "r");
  if (fp == NULL)
    {
      return false;
    }
  snprintf (want, sizeof (want), "[@%s]", (db_name != NULL) ? db_name : "");

  while (fgets (line, sizeof (line), fp) != NULL)
    {
      char *s = line, *e;

      while (*s == ' ' || *s == '\t')
	{
	  s++;
	}
      if (*s == '#' || *s == '\n' || *s == '\0')
	{
	  continue;
	}
      if (*s == '[')
	{
	  e = strchr (s, ']');
	  if (e != NULL)
	    {
	      e[1] = '\0';
	    }
	  /* The engine lowercases section names and keys as it parses (ini_str_lower
	     in ini_parser.c), so [COMMON] and TEMP_VOLUME_PATH are accepted there and
	     must be matched the same way here. */
	  in_common = (strncasecmp (s, "[common]", 8) == 0);
	  in_db = (db_name != NULL && db_name[0] != '\0' && strcasecmp (s, want) == 0);
	  continue;
	}
      if (!in_common && !in_db)
	{
	  continue;
	}
      if (strncasecmp (s, "temp_volume_path", 16) != 0)
	{
	  continue;
	}
      s += 16;
      while (*s == ' ' || *s == '\t')
	{
	  s++;
	}
      if (*s != '=')
	{
	  continue;		/* another parameter that merely starts the same way */
	}
      s++;
      while (*s == ' ' || *s == '\t')
	{
	  s++;
	}
      e = s + strlen (s);
      while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
	{
	  e--;
	}
      *e = '\0';
      if (*s == '\0')
	{
	  continue;
	}
      /* [@db] wins over [common]; once taken, [common] must not overwrite it */
      if (in_db || !from_db)
	{
	  snprintf (out, outsz, "%s", s);
	  found = true;
	  from_db = in_db;
	}
    }
  fclose (fp);
  return found;
}

/* resolve database name to its volumes via databases.txt + _vinf; accept a direct vinf path too */
static int
volmap_resolve_volumes (VOLMAP_CTX * ctx, const char *db_name_or_vinf)
{
  char vinf_path[PATH_MAX];
  FILE *fp;
  char line[PATH_MAX + 64];

  if (strstr (db_name_or_vinf, "_vinf") != NULL)
    {
      snprintf (vinf_path, sizeof (vinf_path), "%s", db_name_or_vinf);
    }
  else
    {
      DB_INFO *dir = NULL, *db;
      if (cfg_read_directory (&dir, false) != NO_ERROR || dir == NULL)
	{
	  fprintf (stderr, "volmap: cannot read databases.txt - set CUBRID_DATABASES (or CUBRID) to the directory holding it\n");
	  return ER_FAILED;
	}
      db = cfg_find_db_list (dir, db_name_or_vinf);
      if (db == NULL)
	{
	  fprintf (stderr, "volmap: unknown database '%s' - check databases.txt, or pass the *_vinf file path directly\n",
		   db_name_or_vinf);
	  cfg_free_directory (dir);
	  return ER_FAILED;
	}
      snprintf (vinf_path, sizeof (vinf_path), "%s/%s_vinf", db->pathname, db->name);
      cfg_free_directory (dir);
    }

  fp = fopen (vinf_path, "r");
  if (fp == NULL)
    {
      fprintf (stderr, "volmap: cannot open %s: %s\n", vinf_path, strerror (errno));
      return ER_FAILED;
    }

  /* Decide the layout BEFORE opening any volume: volmap_open_volume () needs it to
     size the user area (the page watermark arrived in 10.2). */
  snprintf (ctx->vinf_path, sizeof (ctx->vinf_path), "%s", vinf_path);

  /* Where to look for temp volumes, besides the database directory.  --temp-path
     wins; otherwise take temp_volume_path from cubrid.conf for this database. */
  if (ctx->temp_path[0] == '\0')
    {
      char nmcopy[PATH_MAX], *nm, *dot;

      snprintf (nmcopy, sizeof (nmcopy), "%s", vinf_path);
      nm = basename (nmcopy);
      dot = strstr (nm, "_vinf");
      if (dot != NULL)
	{
	  *dot = '\0';
	}
      (void) volmap_conf_temp_path (nm, ctx->temp_path, sizeof (ctx->temp_path));
    }
  /* The volume decides its own layout; the log is only a cross-check, so a missing,
     relocated or unreadable log costs nothing.  stderr, not outfp: these must not
     land in the middle of --format json. */
  {
    char vol0[PATH_MAX];
    VOLMAP_VLAYOUT from_log;

    /* Any permanent volume answers this, so take the first one that does rather
       than requiring volid 0 - a vinf may list only the volumes being looked at. */
    ctx->vlayout = VOLMAP_VLAY_UNKNOWN;
    rewind (fp);
    while (ctx->vlayout == VOLMAP_VLAY_UNKNOWN && fgets (line, sizeof (line), fp) != NULL)
      {
	int id;

	if (sscanf (line, " %d %4095s", &id, vol0) == 2 && id >= 0)
	  {
	    ctx->vlayout = (int) volmap_vlayout_probe (vol0);
	  }
      }
    rewind (fp);

    (void) volmap_read_db_release (ctx->vinf_path, ctx->db_release, sizeof (ctx->db_release), &ctx->shutdown_state);
    from_log = volmap_vlayout_of (ctx->db_release[0] != '\0' ? ctx->db_release : NULL);
    ctx->release_unsupported = volmap_release_before_101 (ctx->db_release);
    volmap_warn_unsupported (ctx);	/* before the run starts */

    if (ctx->vlayout == VOLMAP_VLAY_UNKNOWN)
      {
	ctx->vlayout = (int) from_log;
      }
    else if (ctx->release_unsupported)
      {
	;			/* no 10.1+ layout applies; the warning above says so */
      }
    else if (from_log != VOLMAP_VLAY_UNKNOWN && from_log != (VOLMAP_VLAYOUT) ctx->vlayout)
      {
	/* Report rather than hide it: an upgraded database legitimately has a newer
	   log over older volumes, and the volume is what is being read. */
	fprintf (stderr, "volmap: the volumes are %s but the log says %s (release %s) - using the volumes\n",
		 volmap_vlayout_name ((VOLMAP_VLAYOUT) ctx->vlayout), volmap_vlayout_name (from_log),
		 ctx->db_release);
      }

    if (ctx->vlayout == VOLMAP_VLAY_UNKNOWN && !ctx->release_unsupported)
      {
	/* The watermark layout is assumed (every release from 10.2 has it), but on a
	   10.1 volume that is wrong by 8 bytes - say so rather than be quietly off. */
	fprintf (stderr,
		 "volmap: cannot determine the volume layout - assuming 10.2+ page layout;"
		 " slot-level output would be 8 bytes off on a 10.1 volume\n");
      }
  }

  while (fgets (line, sizeof (line), fp) != NULL)
    {
      int id;
      char path[PATH_MAX];
      if (sscanf (line, " %d %4095s", &id, path) == 2 && id >= 0 && volmap_open_volume (ctx, path) != NO_ERROR)
	{
	  volmap_skip_add (ctx, path, id);
	}
    }
  fclose (fp);

  (void) volmap_scan_temp_volumes (ctx);

  /* With nothing open there is no map, but a listed volume that failed is still a
     result to report (a damaged header is a finding), so only an empty vinf fails. */
  return (ctx->nvols > 0 || ctx->nskipped > 0) ? NO_ERROR : ER_FAILED;
}

static void
volmap_usage (const char *argv0)
{
  fprintf (stderr, "usage: cubrid volmap [OPTIONS] database-name|vinf-path\n"
	   "  -o, --output-file=FILE   write output to FILE\n"
	   "  -w, --width=N            grid columns (default: terminal width, max 240)\n"
	   "      --wide               use 136 columns (140 snapped to 8) - batch only\n"
	   "  -r, --rows=N             grid rows per volume (default 20)\n"
	   "  -f, --full               one cell per page, unlimited rows\n"
	   "  -i, --interactive        full-screen browser: mouse to inspect, </> volume, q quit\n"
	   "  -m, --residency          glyph ramp shows OS page-cache residency (mincore)\n"
	   "  -B, --bufmap=FILE        overlay cub_server's buffer pool from a cub_top --bcb-dump snapshot\n"
	   "                           (teal bg = buffered, purple bg = buffered and not yet on the volume; interactive: b toggles, reloads on tick)\n"
	   "      --check              append an integrity findings report\n"
	   "      --format=json        machine-readable output (volumes, files, findings)\n"
	   "      --deep               full scan: record density + forwarding ratio\n"
	   "      --full-sweep         probe every page of every unowned sector (by default only when\n"
	   "                           there are at most 256 of them)\n"
	   "      --no-tracker         do not read the engine's file tracker; find files by probing the\n"
	   "                           first page of every reserved sector (the tracker is tried first by default)\n"
	   "      --plain              ASCII output without ANSI colors\n"
	   "      --tick=SEC           interactive auto-refresh period in seconds (default 2)\n"
	   "      --warn-idle=PCT      report volumes whose idle space >= PCT%% as a finding (exit 2)\n"
	   "      --temp-path=DIR      also scan DIR for temp volumes (default: temp_volume_path\n"
	   "                           from cubrid.conf, else the database directory)\n"
	   "  -V, --volume=N[,N...]    show only the given volume ids\n"
	   "  -h, --help               show this help\n");
  (void) argv0;
}

/*
 * volmap () - utility entry point (loaded by cub_admin via dlopen)
 */
int
volmap (UTIL_FUNCTION_ARG * arg)
{
  UTIL_ARG_MAP *arg_map = arg->arg_map;
  VOLMAP_CTX ctx;

  const char *db_name;
  const char *output_file;
  int error = NO_ERROR;
  int vi;

  if (utility_get_option_string_table_size (arg_map) != 1)
    {
      volmap_usage (arg->argv0);
      return EXIT_FAILURE;
    }
  db_name = utility_get_option_string_value (arg_map, OPTION_STRING_TABLE, 0);
  if (db_name == NULL)
    {
      volmap_usage (arg->argv0);
      return EXIT_FAILURE;
    }

  memset (&ctx, 0, sizeof (ctx));
  ctx.tracker_self = -1;	/* no tracker file known yet */
  ctx.width = utility_get_option_int_value (arg_map, VOLMAP_WIDTH_S);
  ctx.rows = utility_get_option_int_value (arg_map, VOLMAP_ROWS_S);
  ctx.deep = utility_get_option_bool_value (arg_map, VOLMAP_DEEP_S);
  ctx.full_sweep = utility_get_option_bool_value (arg_map, VOLMAP_FULL_SWEEP_S);
  ctx.no_tracker = utility_get_option_bool_value (arg_map, VOLMAP_NO_TRACKER_S);
  /* --no-overlay is still accepted and ignored: the live overlay was removed, and
     refusing the flag would break command lines that carry it. */
  (void) utility_get_option_bool_value (arg_map, VOLMAP_NO_OVERLAY_S);
  ctx.plain = utility_get_option_bool_value (arg_map, VOLMAP_PLAIN_S);
  ctx.full = utility_get_option_bool_value (arg_map, VOLMAP_FULL_S);
  ctx.interactive = utility_get_option_bool_value (arg_map, VOLMAP_INTERACTIVE_S);
  ctx.residency = utility_get_option_bool_value (arg_map, VOLMAP_RESIDENCY_S);
  ctx.bufmap_path = utility_get_option_string_value (arg_map, VOLMAP_BUFMAP_S, 0);
  ctx.bufmap = (ctx.bufmap_path != NULL);
  {
    const char *tp = utility_get_option_string_value (arg_map, VOLMAP_TEMP_PATH_S, 0);

    if (tp != NULL && tp[0] != '\0')
      {
	snprintf (ctx.temp_path, sizeof (ctx.temp_path), "%s", tp);	/* overrides cubrid.conf */
      }
  }
  ctx.check = utility_get_option_bool_value (arg_map, VOLMAP_CHECK_S);
  ctx.tick_sec = utility_get_option_int_value (arg_map, VOLMAP_TICK_S);
  if (ctx.tick_sec < 1 || ctx.tick_sec > 3600)
    {
      ctx.tick_sec = 2;
    }
  ctx.warn_idle_pct = utility_get_option_int_value (arg_map, VOLMAP_WARN_IDLE_S);
  if (ctx.warn_idle_pct < 0 || ctx.warn_idle_pct > 100)
    {
      ctx.warn_idle_pct = 0;
    }
  if (ctx.warn_idle_pct > 0)
    {
      ctx.check = true;		/* a threshold alert implies the findings report + exit-2 contract */
    }
  {
    /* The database name: the argument itself, or the vinf file name without its
       _vinf suffix.  The bufmap snapshot names the database, so "demodb_vinf"
       would never match it. */
    static char label[256];
    const char *base = strrchr (db_name, '/');
    size_t len;

    snprintf (label, sizeof (label), "%s", (base != NULL) ? base + 1 : db_name);
    len = strlen (label);
    if (len > 5 && strcmp (label + len - 5, "_vinf") == 0)
      {
	label[len - 5] = '\0';
      }
    ctx.db_label = label;
  }
  {
    const char *fmt = utility_get_option_string_value (arg_map, VOLMAP_FORMAT_S, 0);

    if (fmt != NULL && strcmp (fmt, "json") != 0)
      {
	/* falling back to text would hand a caller that asked for a machine-readable
	   document a human one, with a successful exit status */
	fprintf (stderr, "volmap: unknown --format '%s' (only 'json')\n", fmt);
	return EXIT_FAILURE;
      }
    ctx.json = (fmt != NULL);
  }
  {
    const char *vol_list = utility_get_option_string_value (arg_map, VOLMAP_VOLUME_S, 0);
    if (vol_list != NULL)
      {
	const char *p = vol_list;
	ctx.vol_filter_on = true;
	while (*p != '\0')
	  {
	    char *end = NULL;
	    long v = strtol (p, &end, 10);
	    if (end == p)
	      {
		break;
	      }
	    if (v >= 0 && v <= VOLMAP_MAX_VOLID)
	      {
		ctx.vol_filter[v / 8] |= (unsigned char) (1 << (v % 8));
	      }
	    else
	      {
		fprintf (stderr, "volmap: -V: volume id %ld is out of range (0..%d)\n", v, VOLMAP_MAX_VOLID);
		return EXIT_FAILURE;
	      }
	    p = (*end == ',') ? end + 1 : end;
	  }
      }
  }
  if (ctx.width <= 0)
    {
      /* auto width: follow the terminal, fall back to 80 when not a tty */
      struct winsize ws;
      ctx.width = 80;
      if (ioctl (fileno (stdout), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
	{
	  ctx.width = ws.ws_col;
	}
      if (utility_get_option_bool_value (arg_map, VOLMAP_WIDE_S))
	{
	  ctx.width = 140;
	}
    }
  if (ctx.width < 10)
    {
      ctx.width = 80;
    }
  if (ctx.width > 240)
    {
      ctx.width = 240;
    }
  ctx.width -= ctx.width % 8;	/* snap to 8-column steps */
  if (ctx.width < 8)
    {
      ctx.width = 8;
    }
  if (ctx.rows < 1 || ctx.rows > 64)
    {
      ctx.rows = 20;
    }

  output_file = utility_get_option_string_value (arg_map, VOLMAP_OUTPUT_FILE_S, 0);
  ctx.outfp = stdout;
  if (output_file != NULL)
    {
      ctx.outfp = fopen (output_file, "w");
      if (ctx.outfp == NULL)
	{
	  fprintf (stderr, "volmap: cannot open output file %s\n", output_file);
	  return EXIT_FAILURE;
	}
      ctx.plain = true;	/* files get plain output */
    }

  bool check_findings = false;

  /* Pass 1 — volume binaries only */
  error = volmap_resolve_volumes (&ctx, db_name);
  if (error != NO_ERROR)
    {
      volmap_warn_unsupported (&ctx);
      return EXIT_FAILURE;
    }
  if (ctx.interactive && ctx.nvols == 0)
    {
      fprintf (stderr, "volmap: no volume could be opened - nothing to browse\n");
      volmap_warn_unsupported (&ctx);
      return 3;
    }
  /* what runs beside the database and how busy its disk is: /proc only */
  volmap_proc_scan (&ctx);
  volmap_io_begin (&ctx);
  if (ctx.interactive)
    {
      /* progressive: draw immediately from the sector tables; file ownership
       * fills in through idle-time scan slices inside the interactive loop */
      error = volmap_scan_begin (&ctx);
    }
  else
    {
      error = volmap_discover_files (&ctx);
      if (error == NO_ERROR && ctx.sweep_left > 0)
	{
	  fprintf (stderr, "volmap: %ld reserved sectors still have no owner after the first pass - more than %d,"
		   " so their pages were not probed; --full-sweep probes them\n", ctx.sweep_left, VOLMAP_AUTO_SWEEP_MAX);
	}
    }
  if (error != NO_ERROR)
    {
      return EXIT_FAILURE;
    }
  if (ctx.residency && !ctx.interactive)
    {
      int vi2;

      for (vi2 = 0; vi2 < ctx.nvols; vi2++)
	{
	  (void) volmap_read_residency (&ctx.vols[vi2]);
	}
    }
  if (ctx.bufmap_path != NULL)
    {
      if (volmap_bufmap_load (&ctx) != NO_ERROR && !ctx.interactive && !ctx.json)
	{
	  char bsum[256];

	  volmap_bufmap_summary (&ctx, bsum, sizeof (bsum));
	  fprintf (stderr, "volmap: %s\n", bsum);
	}
    }
  /* Test hook (tools/check-refresh.sh): run the incremental refresh N times before the
     report, after an optional pause in which the database can be changed, and say what
     each refresh re-read.  Not an option: it is for checking that a refresh reaches
     the same state a fresh run does. */
  if (!ctx.interactive && getenv ("VOLMAP_REFRESH_LOOPS") != NULL)
    {
      int loops = atoi (getenv ("VOLMAP_REFRESH_LOOPS")), li;
      const char *rs = getenv ("VOLMAP_REFRESH_SLEEP");

      if (rs != NULL && atoi (rs) > 0)
	{
	  sleep ((unsigned) atoi (rs));
	}
      for (li = 0; li < loops; li++)
	{
	  (void) volmap_refresh (&ctx);
	  fprintf (stderr, "refresh %d: %ld files from cache, %ld re-walked, tracker %s\n", li + 1, ctx.rf_cached,
		   ctx.rf_walked, ctx.rf_tracker_walked ? "re-walked" : "unchanged, skipped");
	}
    }
  if (ctx.deep && !ctx.interactive)
    {
      /* The deep scan gives back only the page cache it pulled in, which it tells
         apart with mincore.  Where mincore reports fake data that would evict
         nothing and leave the server's cache displaced, so refuse instead. */
      for (vi = 0; vi < ctx.nvols; vi++)
	{
	  if (volmap_vol_selected (&ctx, ctx.vols[vi].volid) && !volmap_mincore_trusted (&ctx.vols[vi]))
	    {
	      fprintf (stderr, "volmap: --deep refused: %s is neither owned nor writable by this user, so mincore(2)"
		       " cannot tell which pages the server had cached and the scan could not give back the cache it"
		       " uses - run as the database owner\n", ctx.vols[vi].path);
	      return EXIT_FAILURE;
	    }
	}
      /* interactive mode reads pages on demand (panel footer); the full sweep
       * would stall startup for minutes on a large database */
      volmap_deep_scan (&ctx);
    }
  if (ctx.interactive)
    {
      volmap_interactive (&ctx);
    }
  else if (ctx.json)
    {
      if (volmap_output_json (&ctx, db_name) > 0 && ctx.check)
	{
	  check_findings = true;	/* --check / --warn-idle contract holds for JSON too */
	}
    }
  else
    {
      volmap_render (&ctx);
      volmap_print_skipped (&ctx, ctx.outfp);
      if (ctx.check)
	{
	  if (volmap_findings (&ctx, ctx.outfp, false) > 0)
	    {
	      check_findings = true;	/* --check contract: findings -> exit 2 */
	    }
	}

    }

  for (vi = 0; vi < ctx.nvols; vi++)
    {
      volmap_vol_release (&ctx.vols[vi]);
    }
  free (ctx.vols);
  free (ctx.files);
  free (ctx.scan_pos);
  free (ctx.scratch);
  free (ctx.scratch_bmap);
  free (ctx.bm_recs);		/* --bufmap snapshot records */
  for (vi = 0; vi < ctx.nskipped; vi++)
    {
      free (ctx.skipped[vi].path);
    }
  free (ctx.skipped);
  if (ctx.outfp != stdout)
    {
      fclose (ctx.outfp);
    }
  volmap_warn_unsupported (&ctx);	/* and once more as the run ends */
  if (error != NO_ERROR)
    {
      return EXIT_FAILURE;
    }
  /* 2: --check findings (a damaged header is one).  3: listed volumes were not shown,
     so the report is incomplete.  Findings come first: they are what to act on. */
  return check_findings ? 2 : (volmap_skipped_selected (&ctx) > 0) ? 3 : EXIT_SUCCESS;
}
