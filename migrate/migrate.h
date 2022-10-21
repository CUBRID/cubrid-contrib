/*
 * Copyright 2022 CUBRID Corporation
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

#ifndef _CUB_MIGRATE_
#define _CUB_MIGRATE_

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <termios.h>
#include <string.h>
#include <pwd.h>
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <dbi.h>

#define VERSION "1.0"

#define BUF_LEN 2048
#define DB_CLIENT_TYPE_ADMIN_UTILITY 7

#define PRINT_LOG(fmt, ...) print_log (fmt "\tin %s () from %s:%d", __VA_ARGS__, __func__, __FILE__, __LINE__)

#define DB_SIZEOF(val)          (sizeof(val))

#define free_and_init(ptr) \
        do { \
          free ((ptr)); \
          (ptr) = NULL; \
        } while (0)

#define SM_CLASSFLAG_SYSTEM 1

typedef enum au_fetchmode
{
  AU_FETCH_READ,
  AU_FETCH_SCAN,		/* scan that does not allow write */
  AU_FETCH_EXCLUSIVE_SCAN,	/* scan that does allow neither write nor other exclusive scan, i.e, scan for load
				 * index. */
  AU_FETCH_WRITE,
  AU_FETCH_UPDATE
} AU_FETCHMODE;

typedef enum
{
  SM_META_ROOT,			/* the object is the root class */
  SM_META_CLASS			/* the object is a normal class */
} SM_METATYPE;

typedef struct ws_object_header WS_OBJECT_HEADER;

struct ws_object_header
{
  int chn;
};

typedef struct hfid HFID;	/* FILE HEAP IDENTIFIER */
struct hfid
{
  VFID vfid;			/* Volume and file identifier */
  int hpgid;			/* First page identifier (the header page) */
};

typedef struct sm_class_header SM_CLASS_HEADER;
struct sm_class_header
{
  WS_OBJECT_HEADER ch_obj_header;	/* always have the object header (chn) */

  SM_METATYPE ch_type;		/* doesn't need to be a full word */
  const char *ch_name;

  OID ch_rep_dir;		/* representation directory record OID */
  HFID ch_heap;
};

typedef enum
{
  SM_CLASS_CT,			/* default CSQL class */
  SM_VCLASS_CT,			/* component db virtual class */
  SM_ADT_CT			/* Abstract data type-pseudo class */
} SM_CLASS_TYPE;

typedef struct sm_class SM_CLASS;
struct sm_class
{
  SM_CLASS_HEADER header;

  void *users;		/* immediate sub classes */
  SM_CLASS_TYPE class_type;	/* what kind of class variant is this? */
  int repid;			/* current representation id */

  void *representations;	/* list of old representations */

  void *inheritance;	/* immediate super classes */
  int object_size;		/* memory size in bytes */
  int att_count;		/* number of instance attributes */
  void *attributes;	/* list of instance attribute definitions */
  void *shared;		/* list of shared attribute definitions */
  int shared_count;		/* number of shared attributes */
  int class_attribute_count;	/* number of class attributes */
  void *class_attributes;	/* list of class attribute definitions */

  void *method_files;	/* list of method files */
  const char *loader_commands;	/* command string to the dynamic loader */

  void *methods;		/* list of method definitions */
  int method_count;		/* number of instance methods */
  int class_method_count;	/* number of class methods */
  void *class_methods;	/* list of class method definitions */

  void *resolutions;	/* list of instance and class resolutions */

  int fixed_count;		/* number of fixed size attributes */
  int variable_count;		/* number of variable size attributes */
  int fixed_size;		/* byte size of fixed attributes */

  int att_ids;			/* attribute id counter */
  int method_ids;		/* method id counter */
  int unused;			/* formerly repid counter, delete */

  void *query_spec;	/* virtual class query_spec information */
  void *new_;		/* temporary structure */
  void *stats;		/* server statistics, loaded on demand */

  MOP owner;			/* authorization object */
  int collation_id;		/* class collation */
  void *auth_cache;		/* compiled cache */

  void *ordered_attributes;	/* see classobj_fixup_loaded_class () */
  void *properties;		/* property list */
  struct parser_context *virtual_query_cache;
  struct tr_schema_cache *triggers;	/* Trigger cache */
  void *constraints;	/* Constraint cache */
  const char *comment;		/* table comment */
  void *fk_ref;	/* fk ref cache */
  void *partition;	/* partition information */

  unsigned int flags;
  unsigned int virtual_cache_local_schema_id;
  unsigned int virtual_cache_global_schema_id;
  unsigned int virtual_cache_snapshot_version;

  int tde_algorithm;

  unsigned methods_loaded:1;	/* set when dynamic linking was performed */
  unsigned post_load_cleanup:1;	/* set if post load cleanup has occurred */

  unsigned triggers_validated:1;	/* set when trigger cache is validated */
  unsigned has_active_triggers:1;	/* set if trigger processing is required */
  unsigned dont_decache_constraints_or_flush:1;	/* prevent decaching class constraint and flushing. */
  unsigned recache_constraints:1;	/* class constraints need recache. */
  unsigned load_index_from_heap:1;	/* load index from its heap if there are records. If false, create a new empty regardless of the heap when allocating an index. e.g. TRUNCATE */
};

typedef struct list_mops LIST_MOPS;
struct list_mops
{
  int num;
  MOP mops[1];
};

typedef enum
{
  LC_FETCH_CURRENT_VERSION = 0x01,	/* fetch current version */
  LC_FETCH_MVCC_VERSION = 0x02,	/* fetch MVCC - visible version */
  LC_FETCH_DIRTY_VERSION = 0x03,	/* fetch dirty version - S-locked */
  LC_FETCH_CURRENT_VERSION_NO_CHECK = 0x04,	/* fetch current version and not check server side */
} LC_FETCH_VERSION_TYPE;

/* global variable */
const char *PRO_NAME = NULL;
const char *CUBRID_ENV = NULL;

void *dl_handle = NULL;

/* CUBRID function pointer */
typedef void (*LOCATOR_FREE_LIST) (LIST_MOPS * mops);
typedef void (*SM_MARK_SYSTEM_CLASSES) (void);
typedef void (*AU_DISABLE_PASSWORDS) (void);
typedef int (*DB_RESTART_EX) (const char *, const char *, const char *, const char *, const char *, int);
typedef int (*ER_ERRID) (void);
typedef DB_SESSION *(*DB_OPEN_BUFFER) (const char *);
typedef int (*DB_COMPILE_STATEMENT) (DB_SESSION *);
typedef int (*DB_EXECUTE_STATEMENT_LOCAL) (DB_SESSION *, int, DB_QUERY_RESULT **);
typedef void (*DB_CLOSE_SESSION) (DB_SESSION *);
typedef int (*DB_QUERY_END) (DB_QUERY_RESULT *);
typedef int (*DB_COMMIT_TRANSACTION) (void);
typedef int (*DB_ABORT_TRANSACTION) (void);
typedef int (*DB_QUERY_GET_TUPLE_VALUE) (DB_QUERY_RESULT * result, int tuple_index, DB_VALUE * value);
typedef int (*DB_QUERY_TUPLE) (DB_QUERY_RESULT * result);
typedef int (*DB_QUERY_GET_TUPLE_OID) (DB_QUERY_RESULT * result, DB_VALUE * value);
typedef int (*DB_MAKE_STRING) (DB_VALUE * value, DB_CONST_C_CHAR str);
typedef DB_OBJECT *(*DB_GET_OBJECT) (DB_VALUE * value);
typedef int (*DB_PUT) (DB_OBJECT * obj, const char *name, DB_VALUE * value);
typedef char *(*DB_GET_DATABASE_VERSION) (void);
typedef int (*DB_SHUTDOWN) (void);
typedef const char *(*DB_ERROR_STRING) (int);
typedef MOP (*LOCATOR_FIND_CLASS) (const char *classname);
typedef int (*EXTRACT_CLASSES_TO_FILE) (extract_context & ctxt, const char *output_filename);
typedef LIST_MOPS *(*LOCATOR_GET_ALL_MOPS) (MOP class_mop, DB_FETCH_MODE class_purpose,
					    LC_FETCH_VERSION_TYPE * force_fetch_version_type);
typedef int (*AU_FETCH_CLASS) (MOP op, void **class_ptr, int fetchmode, int type);
typedef int (*AU_FETCH_FORCE) (MOP op, SM_CLASS ** class_, AU_FETCHMODE fetchmode);

SM_MARK_SYSTEM_CLASSES cub_sm_mark_system_classes;
AU_FETCH_FORCE cub_au_fetch_class_force;
LOCATOR_FREE_LIST cub_locator_free_list_mops;
AU_DISABLE_PASSWORDS cub_au_disable_passwords;
DB_RESTART_EX cub_db_restart_ex;
ER_ERRID cub_er_errid;
DB_OPEN_BUFFER cub_db_open_buffer;
DB_COMPILE_STATEMENT cub_db_compile_statement;
DB_EXECUTE_STATEMENT_LOCAL cub_db_execute_statement_local;
DB_CLOSE_SESSION cub_db_close_session;
DB_QUERY_END cub_db_query_end;
DB_COMMIT_TRANSACTION cub_db_commit_transaction;
DB_ABORT_TRANSACTION cub_db_abort_transaction;
DB_QUERY_GET_TUPLE_VALUE cub_db_query_get_tuple_value;
DB_QUERY_TUPLE cub_db_query_next_tuple;
DB_QUERY_TUPLE cub_db_query_first_tuple;
DB_QUERY_TUPLE cub_db_query_tuple_count;
DB_QUERY_GET_TUPLE_OID cub_db_query_get_tuple_oid;
DB_MAKE_STRING cub_db_make_string;
DB_GET_OBJECT cub_db_get_object;
DB_PUT cub_db_put;
DB_GET_DATABASE_VERSION cub_db_get_database_version;
DB_SHUTDOWN cub_db_shutdown;
DB_ERROR_STRING cub_db_error_string;
LOCATOR_FIND_CLASS cub_locator_find_class;
EXTRACT_CLASSES_TO_FILE cub_extract_classes_to_file;
LOCATOR_GET_ALL_MOPS cub_locator_get_all_mops;
AU_FETCH_CLASS cub_au_fetch_class; 
/* CUBRID global variable */
int *cub_Au_disable;

DB_OBJECT ***cub_req_class_table;
bool *cub_required_class_only;
bool *cub_include_references;
bool *cub_do_schema;
LIST_MOPS **cub_class_table;
MOP *cub_sm_Root_class_mop;
char **cub_input_filename;
extract_context cub_unload_context;

#endif
