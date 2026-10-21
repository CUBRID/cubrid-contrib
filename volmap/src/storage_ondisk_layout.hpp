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
 * storage_ondisk_layout.hpp - on-disk layout structures shared by storage managers and inspection utilities
 *
 * These definitions were moved verbatim from disk_manager.c and file_manager.c so that read-only inspection
 * utilities (e.g. volmap) can interpret volume files with the exact compiled layout instead of hard-coded
 * offsets. No behavioral change is intended by this move.
 */

#ifndef _STORAGE_ONDISK_LAYOUT_HPP_
#define _STORAGE_ONDISK_LAYOUT_HPP_

#include "storage_common.h"
#include "log_lsa.hpp"

/************************************************************************/
/* File types, descriptors and sector-table entries (moved from file_manager.h) */
/************************************************************************/

typedef enum
{
  FILE_TRACKER,
  FILE_HEAP,
  FILE_HEAP_REUSE_SLOTS,
  FILE_MULTIPAGE_OBJECT_HEAP,
  FILE_BTREE,
  FILE_BTREE_OVERFLOW_KEY,
  FILE_EXTENDIBLE_HASH,
  FILE_EXTENDIBLE_HASH_DIRECTORY,
  FILE_CATALOG,
  FILE_DROPPED_FILES,
  FILE_VACUUM_DATA,
  FILE_QUERY_AREA,
  FILE_TEMP,
  FILE_UNKNOWN_TYPE,
  FILE_LAST = FILE_UNKNOWN_TYPE
} FILE_TYPE;

/* Heap file descriptor */
typedef struct file_heap_des FILE_HEAP_DES;
struct file_heap_des
{
  OID class_oid;
  HFID hfid;
};

/* Overflow heap file descriptor */
typedef struct file_ovf_heap_des FILE_OVF_HEAP_DES;
struct file_ovf_heap_des
{
  HFID hfid;
  OID class_oid;
};

/* Btree file descriptor */
typedef struct file_btree_des FILE_BTREE_DES;
struct file_btree_des
{
  OID class_oid;
  int attr_id;
};

/* Overflow key file descriptor */
typedef struct file_ovf_btree_des FILE_OVF_BTREE_DES;
struct file_ovf_btree_des
{
  BTID btid;
  OID class_oid;
};

/* Extensible Hash file descriptor */
typedef struct file_ehash_des FILE_EHASH_DES;
struct file_ehash_des
{
  OID class_oid;
  int attr_id;
};

/* Vacuum data file descriptor */
typedef struct file_vacuum_data_des FILE_VACUUM_DATA_DES;
struct file_vacuum_data_des
{
  VPID vpid_first;
};

/* note: if you change file descriptors size, make sure to change disk compatibility version too! */
#define FILE_DESCRIPTORS_SIZE 64
typedef union file_descriptors FILE_DESCRIPTORS;
union file_descriptors
{
  FILE_HEAP_DES heap;
  FILE_OVF_HEAP_DES heap_overflow;
  FILE_BTREE_DES btree;
  FILE_OVF_BTREE_DES btree_key_overflow;	/* TODO: rename FILE_OVF_BTREE_DES */
  FILE_EHASH_DES ehash;
  FILE_VACUUM_DATA_DES vacuum_data;
  char dummy_align[FILE_DESCRIPTORS_SIZE];
};

/* FILE_TABLESPACE: defines the space usage and extensions for files */
typedef struct file_tablespace FILE_TABLESPACE;
struct file_tablespace
{
  INT64 initial_size;
  float expand_ratio;
  int expand_min_size;
  int expand_max_size;
};

/* FILE_ALLOC_BITMAP -
 * Type used to store allocation bitmap for sectors.  */
typedef UINT64 FILE_ALLOC_BITMAP;
#define FILE_FULL_PAGE_BITMAP	    0xFFFFFFFFFFFFFFFF	/* Full allocation bitmap */
#define FILE_EMPTY_PAGE_BITMAP	    0x0000000000000000	/* Empty allocation bitmap */

#define FILE_ALLOC_BITMAP_NBITS ((int) (sizeof (FILE_ALLOC_BITMAP) * CHAR_BIT))

/* FILE_PARTIAL_SECTOR -
 * Structure used by partially allocated sectors table. Store sector VSID and its allocation bitmap. */
typedef struct file_partial_sector FILE_PARTIAL_SECTOR;
struct file_partial_sector
{
  VSID vsid;			/* Important - VSID must be first member of FILE_PARTIAL_SECTOR. Sometimes, the
				 * FILE_PARTIAL_SECTOR pointers in file table are reinterpreted as VSID. */
  FILE_ALLOC_BITMAP page_bitmap;
};
#define FILE_PARTIAL_SECTOR_INITIALIZER { VSID_INITIALIZER, 0 }

/************************************************************************/
/* Slotted page header and slot (moved from slotted_page.h)             */
/************************************************************************/

typedef struct spage_header SPAGE_HEADER;
struct spage_header
{
  PGNSLOTS num_slots;		/* Number of allocated slots for the page */
  PGNSLOTS num_records;		/* Number of records on page */
  INT16 anchor_type;		/* Valid ANCHORED, ANCHORED_DONT_REUSE_SLOTS UNANCHORED_ANY_SEQUENCE,
				 * UNANCHORED_KEEP_SEQUENCE */
  unsigned short alignment;	/* Alignment for records: Valid values sizeof char, short, int, double */
  int total_free;		/* Total free space on page */
  int cont_free;		/* Contiguous free space on page */
  int offset_to_free_area;	/* Byte offset from the beginning of the page to the first free byte area on the page. */
  int reserved1;
  int flags;			/* Page flags: Always SPAGE_HEADER_FLAG_NONE, not currently used */
  unsigned int is_saving:1;	/* True if saving is need for recovery (undo) */
  unsigned int need_update_best_hint:1;	/* True if we should update best pages hint for this page */

  /* The followings are reserved for future use. */
  /* SPAGE_HEADER should be 8 bytes aligned. Packing of bit fields depends on compiler's behavior. It's better to use
   * 4-bytes type in order not to be affected by the compiler. */
  unsigned int reserved_bits:30;
};

/* 4-byte disk storage slot design */
typedef struct spage_slot SPAGE_SLOT;
struct spage_slot
{
  unsigned int offset_to_record:14;	/* Byte Offset from the beginning of the page to the beginning of the record */
  unsigned int record_length:14;	/* Length of record */
  unsigned int record_type:4;	/* Record type (REC_HOME, REC_NEWHOME, ...) described by slot. */
};

/************************************************************************/
/* Volume header (moved from disk_manager.c)                            */
/************************************************************************/


/* DON'T USE sizeof on this structure.. size if variable */
typedef struct disk_volume_header DISK_VOLUME_HEADER;
struct disk_volume_header
{				/* Volume header */
  /* DON'T MOVE THE MAGIC FIELD. IT IS USED BY FILEC */
  char magic[CUBRID_MAGIC_MAX_LENGTH];	/* Magic value for file/magic Unix utility */
  INT16 iopagesize;		/* This was only added for checking purposes. The actual value is stored on the log */
  INT16 volid;			/* Volume identifier */
  INT8 db_charset;		/* charset of database */
  INT8 dummy1;			/* Dummy fields for alignment */
  DB_VOLPURPOSE purpose;	/* Permanent or temporary volume purpose */
  DB_VOLTYPE type;		/* Permanent or temporary volume type */
  DKNPAGES sect_npgs;		/* Size of sector in pages */
  DKNSECTS nsect_total;		/* Total number of sectors */
  DKNSECTS nsect_max;		/* Maximum number of sectors */
  SECTID hint_allocsect;	/* Hint for next sector to be allocated */
  DKNPAGES stab_npages;		/* Size of sector allocation table in pages */
  PAGEID stab_first_page;	/* First page of sector allocation table */
  PAGEID sys_lastpage;		/* Last system page */
  INT32 dummy2;			/* Dummy fields for alignment */
  INT64 db_creation;		/* Database creation time. For safety reasons, this value is set on all volumes and the
				 * log. The value is generated by the log manager */
  INT64 vol_creation;		/* Volume creation time */
  LOG_LSA chkpt_lsa;		/* Lowest log sequence address to start the recovery process of this volume */
  HFID boot_hfid;		/* System Heap file for booting purposes and multi volumes */
  INT32 reserved0;		/* reserved area */
  INT32 reserved1;		/* reserved area */
  INT32 reserved2;		/* reserved area */
  INT32 reserved3;		/* reserved area */
  INT16 next_volid;		/* next volume identifier * */
  INT16 offset_to_vol_fullname;	/* Offset to vol_fullname */
  INT16 offset_to_next_vol_fullname;	/* Offset to next vol_fullname */
  INT16 offset_to_vol_remarks;	/* Offset to vol_remarks */

  char var_fields[1];		/* Variable length fields addresses by the offset Current ordering is: 1) vol_fullname,
				 * 2) next_vol_fullname 3) volume remarks The length is DB_PAGESIZE - offset of
				 * var_fields */

};

typedef struct disk_recv_link_perm_volume DISK_RECV_LINK_PERM_VOLUME;

/************************************************************************/
/* File header (moved from file_manager.c)                              */
/************************************************************************/


/* FILE_HEADER -
 * This structure keeps meta-data on files in the file header page. The rest of the file header page, based on the type
 * of files, is used by other tables.
 */
typedef struct file_header FILE_HEADER;
struct file_header
{
  INT64 time_creation;		/* Time of file creation. */

  VFID self;			/* Self VFID */
  FILE_TABLESPACE tablespace;	/* The table space definition */
  FILE_DESCRIPTORS descriptor;	/* File descriptor. Depends on file type. */

  /* Page counts. */
  int n_page_total;		/* File total page count. */
  int n_page_user;		/* User page count. */
  int n_page_ftab;		/* Page count used for file tables. */
  int n_page_free;		/* Free page count. Pages that were reserved on disk and can be allocated by user (or
				 * table) in the future. */
  int n_page_mark_delete;	/* used by numerable files to track marked deleted pages */

  /* Sector counts. */
  int n_sector_total;		/* File total sector count. */
  int n_sector_partial;		/* Partially allocated sectors count. */
  int n_sector_full;		/* Fully allocated sectors count. */
  int n_sector_empty;		/* Completely empty sectors count. Empty sectors are also considered partially
				 * allocated (so this is less or equal to n_sector_partial. */

  FILE_TYPE type;		/* File type. */

  INT32 file_flags;		/* File flags. */

  VOLID volid_last_expand;	/* Last volume used for expansion. */

  INT16 offset_to_partial_ftab;	/* Offset to partial sectors table. */
  INT16 offset_to_full_ftab;	/* Offset to full sectors table. */
  INT16 offset_to_user_page_ftab;	/* Offset to user pages table. */

  VPID vpid_sticky_first;	/* VPID of first page (if it is sticky). This page should never be deallocated. */

  /* Temporary files. */
  /* Temporary files are handled differently than permanent files for simplicity. Temporary file pages are never
   * deallocated, they are deallocated when the file is destroyed or reclaimed when file is reset (but kept in cache).
   * Therefore, we can just keep a cursor that tracks the location of last allocated page. This cursor has two
   * components: the last page of partially allocated sectors and the offset to last sector in this page.
   * When the sector becomes full, the cursor is incremented. When all page becomes full, the cursor is moved to next
   * page. */
  VPID vpid_last_temp_alloc;	/* VPID of partial table page last used to allocate a page. */
  int offset_to_last_temp_alloc;	/* Sector offset in partial table last used to allocate a page. */

  /* Numerable files */
  /* Numerable files have an additional property compared to regular files. The order of user page allocation is
   * tracked and the user can get nth page according to this order. To optimize allocations, we keep the VPID of last
   * page of user page table. Newly allocated page is appended here. */
  VPID vpid_last_user_page_ftab;

  /* cache last file_numerable_find_nth page and index of its entry. used as an optimization for external sort files.
   * the extensible hash case is not interesting for this optimization (because they are usually small files and because
   * the access pattern is less predictable.
   * the usual pattern external sort files is find nth, find nth+1, find nth+2 and so on. so we can cache the page and
   * index of its first entry from last search to predict where the next file_numerable_find_nth will land.
   *
   * how it works:
   * cache last search location for file_numerable_find_nth by saving user page table page VPID and index of first entry
   * in page. next search will probably land in the same page (and it will just need to get to the right offset).
   * if a page is deallocated, the cached location is reset (this is just a safe-guard since external sort does not
   * deallocate pages currently.
   * if a page is allocated, since it is appended at the end, will not affect the cached search location. current thread
   * is actually the only one accessing the file so it can change it safely without promoting to write latch. to avoid
   * safe-guards, we won't set the page dirty (it is hot anyway and likely to remain in memory).
   */
  VPID vpid_find_nth_last;
  int first_index_find_nth_last;

  /* reserved area for future extension */
  INT32 reserved0;
  INT32 reserved1;
  INT32 reserved2;
  INT32 reserved3;
};

/************************************************************************/
/* Extensible data header (moved from file_manager.c)                   */
/************************************************************************/

/* FILE_EXTENSIBLE_DATA -
 * This structure is actually the beginning of a extensible data component. It is usually accessed as a pointer in page.
 */
typedef struct file_extensible_data FILE_EXTENSIBLE_DATA;
struct file_extensible_data
{
  VPID vpid_next;
  INT16 max_size;
  INT16 size_of_item;
  INT16 n_items;
};

#endif /* _STORAGE_ONDISK_LAYOUT_HPP_ */
