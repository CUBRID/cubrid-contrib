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

#include "extract_schema.hpp"
#include "migrate.h"

#define VERSION_INFO 		72
#define DATABASES_ENVNAME 	"CUBRID_DATABASES"
#define DATABASES_FILENAME 	"databases.txt"
#define MAX_LINE		4096
#define ENV_VAR_MAX		255

static const char *system_views[] = {
    "db_class",
    "db_direct_super_class",
    "db_vclass",
    "db_attribute",
    "db_attr_setdomain_elm",
    "db_method",
    "db_meth_arg",
    "db_meth_arg_setdomain_elm",
    "db_meth_file",
    "db_index",
    "db_index_key",
    "db_auth",
    "db_trig",
    "db_partition",
    "db_stored_procedure",
    "db_stored_procedure_args",
    "db_collation",
    "db_charset",
    "db_server",
    "db_synonym" 
};

static const char *view_query = "select class_name \
	 from _db_class \
	 where is_system_class = 0 and class_type = 1";

static const char *system_view_query[] = {
  /* alter catalog to add tables and views (_db_server, _db_synonym, db_server, db_synonym) */
  /* _db_server */
  "create table [_db_server] ( \
  	[link_name] character varying(255) not null, \
  	[host] character varying(255), \
  	[port] integer, \
  	[db_name] character varying(255), \
  	[user_name] character varying(255), \
  	[password] character varying(1073741823), \
  	[properties] character varying(2048), \
  	[owner] [db_user] not null, \
  	[comment] character varying(1024), \
  	constraint [pk__db_server_link_name_owner] primary key ([link_name], [owner]) \
  ) dont_reuse_oid",

  /* _db_synonym */
  "CREATE TABLE [_db_synonym] ( \
  	[unique_name] character varying(255) not null, \
  	[name] character varying(255) not null, \
  	[owner] [db_user] not null, \
  	[is_public] integer default 0 not null, \
  	[target_unique_name] character varying(255) not null, \
  	[target_name] character varying(255) not null, \
  	[target_owner] [db_user] not null, [comment] character varying(2048), \
  	constraint [pk__db_synonym_unique_name] primary key ([unique_name]), \
  	index [i__db_synonym_name_owner_is_public] ([name], [owner], [is_public]) \
  ) DONT_REUSE_OID",

  "REVOKE SELECT ON [db_class] FROM PUBLIC",
  /* db_class */
  "CREATE OR REPLACE VIEW [db_class] ( \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[class_type] varchar(6), \
  	[is_system_class] varchar(3), \
  	[tde_algorithm] varchar(32), \
  	[partitioned] varchar(3), \
  	[is_reuse_oid_class] varchar(3), \
  	[collation] varchar(32), \
  	[comment] varchar(2048) \
   ) as \
	SELECT [c].[class_name] AS [class_name], CAST ([c].[owner].[name] AS VARCHAR(255)) AS [owner_name], CASE [c].[class_type] WHEN 0 THEN 'CLASS' WHEN 1 THEN 'VCLASS' ELSE 'UNKNOW' END AS [class_type], CASE WHEN MOD([c].[is_system_class], 2) = 1 THEN 'YES' ELSE 'NO' END AS [is_system_class], CASE [c].[tde_algorithm] WHEN 0 THEN 'NONE' WHEN 1 THEN 'AES' WHEN 2 THEN 'ARIA' END AS [tde_algorithm], CASE WHEN [c].[sub_classes] IS NULL THEN 'NO' ELSE NVL ((SELECT 'YES' FROM [_db_partition] AS [p] WHERE [p].[class_of] = [c] AND [p].[pname] IS NULL), 'NO') END AS [partitioned], CASE WHEN MOD ([c].[is_system_class] / 8, 2) = 1 THEN 'YES' ELSE 'NO' END AS [is_reuse_oid_class], [coll].[coll_name] AS [collation], [c].[comment] AS [comment] FROM [_db_class] AS [c], [_db_collation] AS [coll] WHERE [c].[collation_id] = [coll].[coll_id] AND (CURRENT_USER = 'DBA' OR {[c].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[c]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_class] TO PUBLIC",

  "REVOKE SELECT ON [db_direct_super_class] FROM PUBLIC",
  /* db_direct_super_class */
  "CREATE OR REPLACE VIEW [db_direct_super_class] ( \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[super_class_name] varchar(255), \
  	[super_owner_name] varchar(255) \
   ) as \
SELECT [c].[class_name] AS [class_name], CAST ([c].[owner].[name] AS VARCHAR(255)) AS [owner_name], [s].[class_name] AS [super_class_name], CAST ([s].[owner].[name] AS VARCHAR(255)) AS [super_owner_name] FROM [_db_class] AS [c], TABLE ([c].[super_classes]) AS [t] ([s]) WHERE CURRENT_USER = 'DBA' OR {[c].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[c]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT')",
  "GRANT SELECT ON [db_direct_super_class] TO PUBLIC",

  "REVOKE SELECT ON [db_vclass] FROM PUBLIC",
  /* db_vclass */
  "CREATE OR REPLACE VIEW [db_vclass] ( \
  	[vclass_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[vclass_def] varchar(1073741823), \
  	[comment] varchar(2048) \
	) as \
SELECT [q].[class_of].[class_name] AS [vclass_name], CAST ([q].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], [q].[spec] AS [vclass_def], [c].[comment] AS [comment] FROM [_db_query_spec] AS [q], [_db_class] AS [c] WHERE [q].[class_of] = [c] AND (CURRENT_USER = 'DBA' OR {[q].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[q].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_vclass] TO PUBLIC",

  "REVOKE SELECT ON [db_attribute] FROM PUBLIC",
  /* db_attribute */
  "CREATE OR REPLACE VIEW [db_attribute] ( \
  	[attr_name] varchar(255), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[attr_type] varchar(8), \
  	[def_order] integer, \
  	[from_class_name] varchar(255), \
  	[from_owner_name] varchar(255), \
  	[from_attr_name] varchar(255), \
  	[data_type] varchar(9), \
  	[prec] integer, \
  	[scale] integer, \
  	[charset] varchar(32), \
  	[collation] varchar(32), \
  	[domain_class_name] varchar(255), \
  	[domain_owner_name] varchar(255), \
  	[default_value] varchar(255), \
  	[is_nullable] varchar(3), \
  	[comment] varchar(1024) \
   ) as \
SELECT [a].[attr_name] AS [attr_name], [c].[class_name] AS [class_name], CAST ([c].[owner].[name] AS VARCHAR(255)) AS [owner_name], CASE [a].[attr_type] WHEN 0 THEN 'INSTANCE' WHEN 1 THEN 'CLASS' ELSE 'SHARED' END AS [attr_type], [a].[def_order] AS [def_order], [a].[from_class_of].[class_name] AS [from_class_name], CAST ([a].[from_class_of].[owner].[name] AS VARCHAR(255)) AS [from_owner_name], [a].[from_attr_name] AS [from_attr_name], [t].[type_name] AS [data_type], [d].[prec] AS [prec], [d].[scale] AS [scale], IF ([a].[data_type] IN (4, 25, 26, 27, 35), (SELECT [ch].[charset_name] FROM [_db_charset] AS [ch] WHERE [d].[code_set] = [ch].[charset_id]), 'Not applicable') AS [charset], IF ([a].[data_type] IN (4, 25, 26, 27, 35), (SELECT [coll].[coll_name] FROM [_db_collation] AS [coll] WHERE [d].[collation_id] = [coll].[coll_id]), 'Not applicable') AS [collation], [d].[class_of].[class_name] AS [domain_class_name], CAST ([d].[class_of].[owner].[name] AS VARCHAR(255)) AS [domain_owner_name], [a].[default_value] AS [default_value], CASE WHEN [a].[is_nullable] = 1 THEN 'YES' ELSE 'NO' END AS [is_nullable], [a].[comment] AS [comment] FROM [_db_class] AS [c], [_db_attribute] AS [a], [_db_domain] AS [d], [_db_data_type] AS [t] WHERE [a].[class_of] = [c] AND [d].[object_of] = [a] AND [d].[data_type] = [t].[type_id] AND (CURRENT_USER = 'DBA' OR {[c].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[c]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_attribute] TO PUBLIC",

  "REVOKE SELECT ON [db_attr_setdomain_elm] FROM PUBLIC",
  /* db_attr_setdomain_elm */
  "CREATE OR REPLACE VIEW [db_attr_setdomain_elm] ( \
  	[attr_name] varchar(255), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[attr_type] varchar(8), \
  	[data_type] varchar(9), \
  	[prec] integer, \
  	[scale] integer, \
  	[code_set] integer, \
  	[domain_class_name] varchar(255), \
  	[domain_owner_name] varchar(255) \
    ) as \
SELECT [a].[attr_name] AS [attr_name], [c].[class_name] AS [class_name], CAST ([c].[owner].[name] AS VARCHAR(255)) AS [owner_name], CASE [a].[attr_type] WHEN 0 THEN 'INSTANCE' WHEN 1 THEN 'CLASS' ELSE 'SHARED' END AS [attr_type], [et].[type_name] AS [data_type], [e].[prec] AS [prec], [e].[scale] AS [scale], [e].[code_set] AS [code_set], [e].[class_of].[class_name] AS [domain_class_name], CAST ([e].[class_of].[owner].[name] AS VARCHAR(255)) AS [domain_owner_name] FROM [_db_class] AS [c], [_db_attribute] AS [a], [_db_domain] AS [d], TABLE ([d].[set_domains]) AS [t] ([e]), [_db_data_type] AS [et] WHERE [a].[class_of] = [c] AND [d].[object_of] = [a] AND [e].[data_type] = [et].[type_id] AND (CURRENT_USER = 'DBA' OR {[c].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[c]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_attr_setdomain_elm] TO PUBLIC",

  "REVOKE SELECT ON [db_method] FROM PUBLIC",
  /* db_method */
  "CREATE OR REPLACE VIEW [db_method] ( \
  	[meth_name] varchar(255), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[meth_type] varchar(8), \
  	[from_class_name] varchar(255), \
  	[from_owner_name] varchar(255), \
  	[from_meth_name] varchar(255), \
  	[func_name] varchar(255) \
   ) AS \
SELECT [m].[meth_name] AS [meth_name], [m].[class_of].[class_name] AS [class_name], CAST ([m].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], CASE [m].[meth_type] WHEN 0 THEN 'INSTANCE' ELSE 'CLASS' END AS [meth_type], [m].[from_class_of].[class_name] AS [from_class_name], CAST ([m].[from_class_of].[owner].[name] AS VARCHAR(255)) AS [from_owner_name], [m].[from_meth_name] AS [from_meth_name], [s].[func_name] AS [func_name] FROM [_db_method] AS [m], [_db_meth_sig] AS [s] WHERE [s].[meth_of] = [m] AND (CURRENT_USER = 'DBA' OR {[m].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[m].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_method] TO PUBLIC",

  "REVOKE SELECT ON [db_meth_arg] FROM PUBLIC",
  /* db_meth_arg */
  "CREATE OR REPLACE VIEW [db_meth_arg] ( \
  	[meth_name] varchar(255), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[meth_type] varchar(8), \
  	[index_of] integer, \
  	[data_type] varchar(9), \
  	[prec] integer, \
  	[scale] integer, \
  	[code_set] integer, \
  	[domain_class_name] varchar(255), \
  	[domain_owner_name] varchar(255) \
    ) AS \
SELECT [s].[meth_of].[meth_name] AS [meth_name], [s].[meth_of].[class_of].[class_name] AS [class_name], CAST ([s].[meth_of].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], CASE [s].[meth_of].[meth_type] WHEN 0 THEN 'INSTANCE' ELSE 'CLASS' END AS [meth_type], [a].[index_of] AS [index_of], [t].[type_name] AS [data_type], [d].[prec] AS [prec], [d].[scale] AS [scale], [d].[code_set] AS [code_set], [d].[class_of].[class_name] AS [domain_class_name], CAST ([d].[class_of].[owner].[name] AS VARCHAR(255)) AS [domain_owner_name] FROM [_db_meth_sig] AS [s], [_db_meth_arg] AS [a], [_db_domain] AS [d], [_db_data_type] AS [t] WHERE [a].[meth_sig_of] = [s] AND [d].[object_of] = [a] AND [d].[data_type] = [t].[type_id] AND (CURRENT_USER = 'DBA' OR {[s].[meth_of].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[s].[meth_of].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_meth_arg] TO PUBLIC",

  "REVOKE SELECT ON [db_meth_arg_setdomain_elm] FROM PUBLIC",
  /* db_meth_arg_setdomain_elm */
  "CREATE OR REPLACE VIEW [db_meth_arg_setdomain_elm] ( \
  	[meth_name] varchar(255), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[meth_type] varchar(8), \
  	[index_of] integer, \
  	[data_type] varchar(9), \
  	[prec] integer, \
  	[scale] integer, \
  	[code_set] integer, \
  	[domain_class_name] varchar(255), \
  	[domain_owner_name] varchar(255) \
   ) AS \
SELECT [s].[meth_of].[meth_name] AS [meth_name], [s].[meth_of].[class_of].[class_name] AS [class_name], CAST ([s].[meth_of].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], CASE [s].[meth_of].[meth_type] WHEN 0 THEN 'INSTANCE' ELSE 'CLASS' END AS [meth_type], [a].[index_of] AS [index_of], [et].[type_name] AS [data_type], [e].[prec] AS [prec], [e].[scale] AS [scale], [e].[code_set] AS [code_set], [e].[class_of].[class_name] AS [domain_class_name], CAST ([e].[class_of].[owner].[name] AS VARCHAR(255)) AS [domain_owner_name] FROM [_db_meth_sig] AS [s], [_db_meth_arg] AS [a], [_db_domain] AS [d], TABLE ([d].[set_domains]) AS [t] ([e]), [_db_data_type] AS [et] WHERE [a].[meth_sig_of] = [s] AND [d].[object_of] = [a] AND [e].[data_type] = [et].[type_id] AND (CURRENT_USER = 'DBA' OR {[s].[meth_of].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[s].[meth_of].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_meth_arg_setdomain_elm] TO PUBLIC",

  "REVOKE SELECT ON [db_meth_file] FROM PUBLIC",
  /* db_meth_file */
  "CREATE OR REPLACE VIEW [db_meth_file] ( \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[path_name] varchar(255), \
  	[from_class_name] varchar(255), \
  	[from_owner_name] varchar(255) \
   ) AS \
SELECT [f].[class_of].[class_name] AS [class_name], CAST ([f].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], [f].[path_name] AS [path_name], [f].[from_class_of].[class_name] AS [from_class_name], CAST ([f].[from_class_of].[owner].[name] AS VARCHAR(255)) AS [from_owner_name] FROM [_db_meth_file] AS [f] WHERE CURRENT_USER = 'DBA' OR {[f].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[f].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT')",
  "GRANT SELECT ON [db_meth_file] TO PUBLIC",

  "REVOKE SELECT ON [db_index] FROM PUBLIC",
  /* db_index */
  "CREATE OR REPLACE VIEW [db_index] ( \
  	[index_name] varchar(255), \
  	[is_unique] varchar(3), \
  	[is_reverse] varchar(3), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[key_count] integer, \
  	[is_primary_key] varchar(3), \
  	[is_foreign_key] varchar(3), \
  	[filter_expression] varchar(255), \
  	[have_function] varchar(3), \
  	[comment] varchar(1024), \
  	[status] varchar(255) \
   ) AS \
SELECT [i].[index_name] AS [index_name], CASE [i].[is_unique] WHEN 0 THEN 'NO' ELSE 'YES' END AS [is_unique], CASE [i].[is_reverse] WHEN 0 THEN 'NO' ELSE 'YES' END AS [is_reverse], [i].[class_of].[class_name] AS [class_name], CAST ([i].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], [i].[key_count] AS [key_count], CASE [i].[is_primary_key] WHEN 0 THEN 'NO' ELSE 'YES' END AS [is_primary_key], CASE [i].[is_foreign_key] WHEN 0 THEN 'NO' ELSE 'YES' END AS [is_foreign_key], [i].[filter_expression] AS [filter_expression], CASE [i].[have_function] WHEN 0 THEN 'NO' ELSE 'YES' END AS [have_function], [i].[comment] AS [comment], CASE [i].[status] WHEN 0 THEN 'NO_INDEX' WHEN 1 THEN 'NORMAL INDEX' WHEN 2 THEN 'INVISIBLE INDEX' WHEN 3 THEN 'INDEX IS IN ONLINE BUILDING' ELSE 'NULL' END AS [status] FROM [_db_index] AS [i] WHERE CURRENT_USER = 'DBA' OR {[i].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[i].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT')",
  "GRANT SELECT ON [db_index] TO PUBLIC",

  "REVOKE SELECT ON [db_index_key] FROM PUBLIC",
  /* db_index_key */
  "CREATE OR REPLACE VIEW [db_index_key] ( \
  	[index_name] varchar(255), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[key_attr_name] varchar(255), \
  	[key_order] integer, \
  	[asc_desc] varchar(4), \
  	[key_prefix_length] integer, \
  	[func] varchar(1023) \
   ) as \
SELECT [k].[index_of].[index_name] AS [index_name], [k].[index_of].[class_of].[class_name] AS [class_name], CAST ([k].[index_of].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], [k].[key_attr_name] AS [key_attr_name], [k].[key_order] AS [key_order], CASE [k].[asc_desc] WHEN 0 THEN 'ASC' WHEN 1 THEN 'DESC' ELSE 'UNKN' END AS [asc_desc], [k].[key_prefix_length] AS [key_prefix_length], [k].[func] AS [func] FROM [_db_index_key] AS [k] WHERE CURRENT_USER = 'DBA' OR {[k].[index_of].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[k].[index_of].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT')",
  "GRANT SELECT ON [db_index_key] TO PUBLIC",

  "REVOKE SELECT ON [db_auth] FROM PUBLIC",
  /* db_auth */
  "CREATE OR REPLACE VIEW [db_auth] ( \
  	[grantor_name] varchar(255), \
  	[grantee_name] varchar(255), \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[auth_type] varchar(7), \
  	[is_grantable] varchar(3) \
   ) as \
SELECT CAST ([a].[grantor].[name] AS VARCHAR(255)) AS [grantor_name], CAST ([a].[grantee].[name] AS VARCHAR(255)) AS [grantee_name], [a].[class_of].[class_name] AS [class_name], CAST ([a].[class_of].[owner].[name] AS VARCHAR(255)) AS [owner_name], [a].[auth_type] AS [auth_type], CASE [a].[is_grantable] WHEN 0 THEN 'NO' ELSE 'YES' END AS [is_grantable] FROM [_db_auth] AS [a] WHERE CURRENT_USER = 'DBA' OR {[a].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[a].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT')",
  "GRANT SELECT ON [db_auth] TO PUBLIC",

  "REVOKE SELECT ON [db_trig] FROM PUBLIC",
  /* db_trig */
  "CREATE OR REPLACE VIEW [db_trig] ( \
  	[trigger_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[target_class_name] varchar(255), \
  	[target_owner_name] varchar(255), \
  	[target_attr_name] varchar(255), \
  	[target_attr_type] varchar(255), \
  	[action_type] integer, \
  	[action_time] integer, \
  	[comment] varchar(1024) \
   ) as \
SELECT CAST ([t].[name] AS VARCHAR (255)) AS [trigger_name], CAST ([t].[owner].[name] AS VARCHAR(255)) AS [owner_name], [c].[class_name] AS [target_class_name], CAST ([c].[owner].[name] AS VARCHAR(255)) AS [target_owner_name], CAST ([t].[target_attribute] AS VARCHAR (255)) AS [target_attr_name], CASE [t].[target_class_attribute] WHEN 0 THEN 'INSTANCE' ELSE 'CLASS' END AS [target_attr_type], [t].[action_type] AS [action_type], [t].[action_time] AS [action_time], [t].[comment] AS [comment] FROM [db_trigger] AS [t] LEFT OUTER JOIN [_db_class] AS [c] ON [t].[target_class] = [c].[class_of] WHERE CURRENT_USER = 'DBA' OR {[t].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[c]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT')",
  "GRANT SELECT ON [db_trig] TO PUBLIC",

  "REVOKE SELECT ON [db_partition] FROM PUBLIC",
  /* db_partition */
  "CREATE OR REPLACE VIEW [db_partition] ( \
  	[class_name] varchar(255), \
  	[owner_name] varchar(255), \
  	[partition_name] varchar(255), \
  	[partition_class_name] varchar(255), \
  	[partition_type] varchar(32), \
  	[partition_expr] varchar(2048), \
  	[partition_values] sequence of, \
  	[comment] varchar(1024) \
   ) as \
SELECT [s].[class_name] AS [class_name], CAST ([s].[owner].[name] AS VARCHAR(255)) AS [owner_name], [p].[pname] AS [partition_name], CONCAT ([s].[class_name], '__p__', [p].[pname]) AS [partition_class_name], CASE [p].[ptype] WHEN 0 THEN 'HASH' WHEN 1 THEN 'RANGE' ELSE 'LIST' END AS [partition_type], TRIM (SUBSTRING ([pp].[pexpr] FROM 8 FOR (POSITION (' FROM ' IN [pp].[pexpr]) - 8))) AS [partition_expr], [p].[pvalues] AS [partition_values], [p].[comment] AS [comment] FROM [_db_partition] AS [p], [_db_class] AS [c], TABLE ([c].[super_classes]) AS [t] ([s]), [_db_class] AS [cc], TABLE ([cc].[partition]) AS [tt] ([pp]) WHERE [p].[class_of] = [c] AND [s] = [cc] AND (CURRENT_USER = 'DBA' OR {[p].[class_of].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[p].[class_of]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT'))",
  "GRANT SELECT ON [db_partition] TO PUBLIC",

  "REVOKE SELECT ON [db_stored_procedure] FROM PUBLIC",
  /* db_stored_procedure */
  "CREATE OR REPLACE VIEW [db_stored_procedure] ( \
  	[sp_name] varchar(255), \
  	[sp_type] varchar(16), \
  	[return_type] varchar(16), \
  	[arg_count] integer, \
  	[lang] varchar(16), \
  	[target] varchar(4096), \
  	[owner] varchar(256), \
  	[comment] varchar(1024) \
   ) as \
SELECT [sp].[sp_name] AS [sp_name], CASE [sp].[sp_type] WHEN 1 THEN 'PROCEDURE' ELSE 'FUNCTION' END AS [sp_type], CASE [sp].[return_type] WHEN 0 THEN 'void' WHEN 28 THEN 'CURSOR' ELSE (SELECT [t].[type_name] FROM [_db_data_type] AS [t] WHERE [sp].[return_type] = [t].[type_id]) END AS [return_type], [sp].[arg_count] AS [arg_count], CASE [sp].[lang] WHEN 1 THEN 'JAVA' ELSE '' END AS [lang], [sp].[target] AS [target], CAST ([sp].[owner].[name] AS VARCHAR(255)) AS [owner], [sp].[comment] AS [comment] FROM [_db_stored_procedure] AS [sp]",
  "GRANT SELECT ON [db_stored_procedure] TO PUBLIC",

  "REVOKE SELECT ON [db_stored_procedure_args] FROM PUBLIC",
  /* db_stored_procedure_args */
  "create or replace view [db_stored_procedure_args] ( \
  	[sp_name] varchar(255), \
  	[index_of] integer, \
  	[arg_name] varchar(255), \
  	[data_type] varchar(16), \
  	[mode] varchar(6), \
  	[comment] varchar(1024) \
   ) as \
SELECT [sp].[sp_name] AS [sp_name], [sp].[index_of] AS [index_of], [sp].[arg_name] AS [arg_name], CASE [sp].[data_type] WHEN 28 THEN 'CURSOR' ELSE (SELECT [t].[type_name] FROM [_db_data_type] AS [t] WHERE [sp].[data_type] = [t].[type_id]) END AS [data_type], CASE [sp].[mode] WHEN 1 THEN 'IN' WHEN 2 THEN 'OUT' ELSE 'INOUT' END AS [mode], [sp].[comment] AS [comment] FROM [_db_stored_procedure_args] AS [sp] ORDER BY [sp].[sp_name], [sp].[index_of]",
  "GRANT SELECT ON [db_stored_procedure_args] TO PUBLIC",

  "REVOKE SELECT ON [db_collation] FROM PUBLIC",
  /* db_collation */
  "CREATE OR REPLACE VIEW [db_collation] ( \
  	[coll_id] integer, \
  	[coll_name] varchar(32), \
  	[charset_name] varchar(32), \
  	[is_builtin] varchar(3), \
  	[has_expansions] varchar(3), \
  	[contractions] integer, \
  	[uca_strength] varchar(255) \
   ) as \
SELECT [coll].[coll_id] AS [coll_id], [coll].[coll_name] AS [coll_name], [ch].[charset_name] AS [charset_name], CASE [coll].[built_in] WHEN 0 THEN 'No' WHEN 1 THEN 'Yes' ELSE 'ERROR' END AS [is_builtin], CASE [coll].[expansions] WHEN 0 THEN 'No' WHEN 1 THEN 'Yes' ELSE 'ERROR' END AS [has_expansions], [coll].[contractions] AS [contractions], CASE [coll].[uca_strength] WHEN 0 THEN 'Not applicable' WHEN 1 THEN 'Primary' WHEN 2 THEN 'Secondary' WHEN 3 THEN 'Tertiary' WHEN 4 THEN 'Quaternary' WHEN 5 THEN 'Identity' ELSE 'Unknown' END AS [uca_strength] FROM [_db_collation] AS [coll] INNER JOIN [_db_charset] AS [ch] ON [coll].[charset_id] = [ch].[charset_id] ORDER BY [coll].[coll_id]",
  "GRANT SELECT ON [db_collation] TO PUBLIC",

  "REVOKE SELECT ON [db_charset] FROM PUBLIC",
  /* db_charset */
  "CREATE OR REPLACE VIEW [db_charset] ( \
  	[charset_id] integer, \
  	[charset_name] varchar(32), \
  	[default_collation] varchar(32), \
  	[char_size] integer \
   ) as \
SELECT [ch].[charset_id] AS [charset_id], [ch].[charset_name] AS [charset_name], [coll].[coll_name] AS [default_collation], [ch].[char_size] AS [char_size] FROM [_db_charset] AS [ch], [_db_collation] AS [coll] WHERE [ch].[default_collation] = [coll].[coll_id] ORDER BY [ch].[charset_id]",
  "GRANT SELECT ON [db_charset] TO PUBLIC",

  /* db_server */
  "CREATE OR REPLACE VIEW [db_server] ( \
  	[link_name] character varying(255), \
  	[host] character varying(255), \
  	[port] integer, \
  	[db_name] character varying(255), \
  	[user_name] character varying(255), \
  	[properties] character varying(2048), \
  	[owner] character varying(255), \
  	[comment] character varying(1024) \
  ) as \
SELECT [ds].[link_name] AS [link_name], [ds].[host] AS [host], [ds].[port] AS [port], [ds].[db_name] AS [db_name], [ds].[user_name] AS [user_name], [ds].[properties] AS [properties], CAST ([ds].[owner].[name] AS VARCHAR(255)) AS [owner], [ds].[comment] AS [comment] FROM [_db_server] AS [ds] WHERE CURRENT_USER = 'DBA' OR {[ds].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) OR {[ds]} SUBSETEQ (SELECT SUM (SET {[au].[class_of]}) FROM [_db_auth] AS [au] WHERE {[au].[grantee].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER) AND [au].[auth_type] = 'SELECT')",
  "GRANT SELECT ON [db_server] TO PUBLIC",

  /* db_synonym */
  "CREATE OR REPLACE VIEW [db_synonym] ( \
  	[synonym_name] character varying(255), \
  	[synonym_owner_name] character varying(255), \
  	[is_public_synonym] character varying(3), \
  	[target_name] character varying(255), \
  	[target_owner_name] character varying(255), \
  	[comment] character varying(2048) \
   ) AS \
SELECT [s].[name] AS [synonym_name], CAST ([s].[owner].[name] AS VARCHAR(255)) AS [synonym_owner_name], CASE [s].[is_public] WHEN 1 THEN 'YES' ELSE 'NO' END AS [is_public_synonym], [s].[target_name] AS [target_name], CAST ([s].[target_owner].[name] AS VARCHAR(255)) AS [target_owner_name], [s].[comment] AS [comment] FROM [_db_synonym] AS [s] WHERE CURRENT_USER = 'DBA' OR [s].[is_public] = 1 OR ([s].[is_public] = 0 AND {[s].[owner].[name]} SUBSETEQ (SELECT SET {CURRENT_USER} + COALESCE (SUM (SET {[t].[g].[name]}), SET {}) FROM [db_user] AS [u], TABLE ([u].[groups]) AS [t] ([g]) WHERE [u].[name] = CURRENT_USER))",
  "GRANT SELECT ON [db_synonym] TO PUBLIC",

  /* set system class for newly added tables and views */
  "UPDATE [_db_class] \
   SET is_system_class = 1 \
   WHERE \
  	class_name in ( \
      		'_db_server', \
      		'_db_synonym', \
      		'db_class', \
      		'db_direct_super_class', \
      		'db_vclass', \
      		'db_attribute', \
      		'db_attr_setdomain_elm', \
      		'db_method', \
      		'db_meth_arg', \
      		'db_meth_arg_setdomain_elm', \
      		'db_meth_file', \
      		'db_index', \
      		'db_index_key', \
      		'db_auth', \
      		'db_trig', \
      		'db_partition', \
      		'db_stored_procedure', \
      		'db_stored_procedure_args', \
      		'db_collation', \
      		'db_charset', \
      		'db_server', \
      		'db_synonym' \
    ) \
   AND is_system_class = 0"
};

static const char *catalog_query[] = {
  /* alter catalog to add column */
  "alter table _db_class add column unique_name varchar (255) after [class_of]",
  "delete from _db_attribute where class_of.class_name = '_db_class' and rownum % 2 = 1",

  "alter table db_serial add column unique_name varchar first",
  "delete from _db_attribute where class_of.class_name = 'db_serial' and rownum % 2 = 1 and attr_name <> 'unique_name'",

  "alter table db_trigger add column unique_name varchar after owner",
  "delete from _db_attribute where class_of.class_name = 'db_trigger' and rownum % 2 = 1 and attr_name <> 'unique_name'",

  /* alter catalog to modify _db_index_key */
  "alter table _db_index_key modify column func varchar(1023)",
  "delete from _db_attribute where class_of.class_name = '_db_index_key' and rownum % 2 = 1"
};

static const char *rename_query = "select \
     case \
       when class_type = 0 then \
      'rename table [' || class_name || '] to [' || lower (owner.name) || '.' || class_name || '] ' \
     else \
      'rename view [' || class_name || '] to [' || lower (owner.name) || '.' || class_name || '] ' \
     end as q \
   from \
     _db_class \
   where \
     is_system_class % 8 = 0";

/* update class_name and unique_name except for system classes. */
static const char *update_db_class_not_for_system_classes =
  "update _db_class set class_name = substring_index (class_name, '.', -1), unique_name = class_name where is_system_class % 8 = 0";

static const char *serial_query = "select \
       'call change_serial_owner (''' || name || ''', ''' || substring_index (class_name, '.', 1) || ''') \
          on class db_serial' as q \
  from db_serial \
  where class_name is not null";

static const char *update_serial[] = {
  "update db_serial set name = substring_index (name, '.', -1)",
  "update db_serial set unique_name = lower (owner.name) || '.' || name"
};

static const char *update_trigger = "update db_trigger set unique_name = lower (owner.name) || '.' || name";

static const char *index_query[] = {
  "create unique index u_db_serial_name_owner ON db_serial (name, owner)",
  "alter table db_serial drop constraint pk_db_serial_name",
  "alter table db_serial add constraint pk_db_serial_unique_name primary key (unique_name)",
  "drop index i__db_class_class_name on _db_class",
  "create index i__db_class_unique_name on _db_class (unique_name)",
  "create index i__db_class_class_name_owner on _db_class (class_name, owner)",
};

/* only system classes update unique_name. */
static const char *update_db_class_for_system_classes =
  "update _db_class set unique_name = class_name where is_system_class % 8 != 0";

static char db_path[PATH_MAX];

FILE *out = NULL;
int verbose = 0;

static void
print_errmsg (const char *err_msg)
{
  fprintf (stderr, "ERROR: %s\n", err_msg);

  return;
}

static void
print_log (const char *log_fmt, ...)
{
  char log_msg[BUF_LEN];
  time_t t;
  struct tm *tm;
  va_list ap;

  t = time (NULL);
  tm = localtime (&t);

  snprintf (log_msg, BUF_LEN, "%d-%02d-%02d %02d:%02d:%02d\t",
	    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

  va_start (ap, log_fmt);

  vsnprintf (log_msg + strlen (log_msg), BUF_LEN - strlen (log_msg), log_fmt, ap);

  va_end (ap);

  snprintf (log_msg + strlen (log_msg), BUF_LEN - strlen (log_msg), "\n");

  fprintf (stderr, "ERROR : %s\n", log_msg);

  return;
}

static int
char_isspace (int c)
{
  return ((c) == ' ' || (c) == '\t' || (c) == '\r' || (c) == '\n' || (c) == '\f' || (c) == '\v');
}

static char *
next_char (char *str_p)
{
  char *p;

  p = str_p;
  while (char_isspace ((int) *p) && *p != '\0')
    {
      p++;
    }

  return (p);
}

static char *
get_token (char *str_p, char **token_p)
{
  char *p, *end, *token = NULL;
  int length;

  p = str_p;
  while (char_isspace ((int) *p) && *p != '\0')
    {
      p++;
    }
  end = p;
  while (!char_isspace ((int) *end) && *end != '\0')
    {
      end++;
    }

  length = (int) (end - p);
  if (length > 0)
    {
      token = (char *) malloc (length + 1);
      if (token != NULL)
	{
	  strncpy (token, p, length);
	  token[length] = '\0';
	}
    }

  *token_p = token;
  return (end);
}

static int
get_directory_path (char *buffer)
{
  const char *env_name;

  env_name = getenv (DATABASES_ENVNAME);
  if (env_name == NULL || strlen (env_name) == 0)
    {
      fprintf (out, "migrate: env variable %s is not set\n", DATABASES_ENVNAME);
      return -1;
    }
  else
    {
      if (env_name[strlen (env_name) - 1] == '/')
	{
	  sprintf (buffer, "%s%s", env_name, DATABASES_FILENAME);
	}
      else
	{
	  sprintf (buffer, "%s/%s", env_name, DATABASES_FILENAME);
	}
    }

  return 0;
}

static int
get_db_path (char *dbname, char **pathname)
{
  FILE *file_p = NULL;
  char line[MAX_LINE];
  char filename[PATH_MAX];
  char *vol_path = NULL;
  char *name = NULL;
  char *host = NULL;
  char *str = NULL;

  if (get_directory_path (filename) < 0)
    {
      return -1;
    }

  file_p = fopen (filename, "r");

  while (fgets (line, MAX_LINE - 1, file_p) != NULL)
    {
      str = next_char (line);
      if (*str != '\0' && *str != '#')
	{
	  str = get_token (str, &name);
	  str = get_token (str, &vol_path);
	  str = get_token (str, &host);

	  if (vol_path == NULL)
	    {
	      fclose (file_p);
	      fprintf (out, "migrate: %s is invalid for %s\n", filename, dbname);
	      return -1;
	    }

	  if (strcmp (host, "localhost") == 0 && strcmp (name, dbname) == 0)
	    {
	      str = get_token (str, pathname);
	      if (*pathname == NULL || *pathname == (char *) '\0')
		{
		  *pathname = vol_path;
		}
	      else
		{
		  free (vol_path);
		}

	      free (name);
	      free (host);

	      fclose (file_p);

	      return 0;
	    }

	  free (vol_path);
	  free (name);
	  free (host);
	}
    }

  fclose (file_p);

  fprintf (out, "migrate: can not find database, %s\n", dbname);

  return -1;
}

static int
migrate_extract_views (int view_cnt, char **class_name, const char *schema_file)
{
  int i, j, error, is_partition = 0;
  DB_OBJECT *class_ = NULL;
  MOP *sub_partitions = NULL;
  extract_context unload_context;
  LIST_MOPS *class_table;
  DB_OBJECT **req_class_table;

  *cub_required_class_only = true;
  *cub_do_schema = true;
  *cub_include_references = false;
  *cub_input_filename = (char *) schema_file;

  class_table = cub_locator_get_all_mops (*cub_sm_Root_class_mop, DB_FETCH_READ, NULL);
  *cub_class_table = class_table;

  req_class_table = (DB_OBJECT **) malloc (DB_SIZEOF (void *) * class_table->num);
  *cub_req_class_table = req_class_table;

  for (i = 0; i < class_table->num; ++i)
    {
      req_class_table[i] = NULL;
    }

  i = 0;
  while (i < view_cnt)
    {
      class_ = cub_locator_find_class (class_name[i]);
      if (class_ != NULL)
	{
	  req_class_table[i] = class_;
	  i++;
	}
    }

  for (i = 0; req_class_table[i]; i++)
    {
      cub_au_fetch_class (req_class_table[i], NULL, 0, 1);
    }

  unload_context.do_auth = 1;
  unload_context.storage_order = FOLLOW_STORAGE_ORDER;
  unload_context.exec_name = "migrate";

  if ((error = cub_extract_classes_to_file (unload_context, schema_file)) != 0)
    {
      return error;
    }

  return NO_ERROR;
}

static int
migrate_get_db_path (char *dbname)
{
  char *path;
  int error;

  error = get_db_path (dbname, &path);

  if (error < 0)
    {
      return -1;
    }

  sprintf (db_path, "%s/%s_lgat", path, dbname);

  return NO_ERROR;
}

static int
migrate_check_log_volume (char *dbname)
{
  char *version = cub_db_get_database_version ();
  char *path;
  int fd;
  float log_ver;

  if (version)
    {
      if ((strncmp (version, "11.0", 4) != 0 && strncmp (version, "11.1", 4) != 0))	// build number여서 11.X.X-XXXX 로 표시됨 
	{
	  fprintf (out, "CUBRID version %s, should be 11.0.x or 11.1.x\n", version);
	  return -1;
	}

      fprintf (out, "%s\n", version);
    }
  else
    {
      fprintf (out, "migrate: can not get version info.\n");
      return -1;
    }

  fd = open (db_path, O_RDONLY);

  if (fd < 0)
    {
      fprintf (out, "migrate: can not open the log file\n");
      return -1;
    }

  if (lseek (fd, VERSION_INFO, SEEK_SET) < 0)
    {
      fprintf (out, "migrate: can not seek the version info.\n");
      close (fd);
      return -1;
    }

  if (read (fd, &log_ver, 4) < 0)
    {
      fprintf (out, "migrate: can not read the version info.\n");
      close (fd);
      return -1;
    }

  if (log_ver < 11.0f || log_ver >= 11.2f)
    {
      fprintf (out, "migrate: the database volume %2.1f is not a migratable version\n", log_ver);
      close (fd);
      return -1;
    }

  close (fd);

  return 0;
}

static int
migrate_backup_log_volume (char *dbname)
{
  char backup_path[PATH_MAX];
  int fd;

  int backup;
  off_t copied = 0;
  struct stat fileinfo = { 0 };

  fd = open (db_path, O_RDWR);

  /* open log volume file and seek version info */
  if (fd < 0)
    {
      fprintf (out, "migrate: can not open the log file for upgrade\n");
      return -1;
    }

  /* creating backup log file */
  sprintf (backup_path, "%s.bak", db_path);
  backup = open (backup_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (backup < 0)
    {
      fprintf (out, "migrate: can not create backup log file, %s\n", backup_path);
      close (fd);
      return -1;
    }

  /* copying to backup log file */
  fstat (fd, &fileinfo);
  ssize_t bytes = sendfile (backup, fd, &copied, fileinfo.st_size);

  if (bytes != copied)
    {
      fprintf (out, "migrate: cant not copy the backup log file\n");
      close (fd);
      close (backup);
      return -1;
    }

  fprintf (out, "\tbackup volume, %s created\n", backup_path);

  close (fd);
  close (backup);

  return 0;
}

static void
migrate_update_log_volume (char *dbname)
{
  float version = 11.2;
  int fd;

  fd = open (db_path, O_RDWR);

  /* open log volume file and seek version info */
  if (fd < 0)
    {
      fprintf (out, "migrate: can not open the log file for upgrade\n");
      return;
    }

  if (lseek (fd, VERSION_INFO, SEEK_SET) < 0)
    {
      fprintf (out, "migrate: can not seek the version info. for upgrade\n");
      close (fd);
      return;
    }

  /* updating volumne info. */
  if (write (fd, &version, 4) < 0)
    {
      fprintf (out, "migrate: can not write the version info. for upgrade\n");
      close (fd);
      return;
    }

  fsync (fd);

  close (fd);

  fprintf (out, "migration done\n");
}

static int
cub_db_execute_query (const char *str, DB_QUERY_RESULT ** result)
{
  DB_SESSION *session = NULL;
  STATEMENT_ID stmt_id;
  int error = NO_ERROR;

  session = cub_db_open_buffer (str);
  if (session == NULL)
    {
      error = cub_er_errid ();
      goto exit;
    }

  stmt_id = cub_db_compile_statement (session);
  if (stmt_id < 0)
    {
      error = cub_er_errid ();
      goto exit;
    }

  error = cub_db_execute_statement_local (session, stmt_id, result);

  if (error >= 0)
    {
      return error;
    }

exit:
  if (session != NULL)
    {
      cub_db_close_session (session);
    }

  return error;
}

static int
migrate_initialize (char *dbname)
{
  char libcubridsa_path[BUF_LEN];
  int error = 0;

  CUBRID_ENV = getenv ("CUBRID");
  if (CUBRID_ENV == NULL)
    {
      print_errmsg ("$CUBRID environment variable is not defined.");
      error = -1;
    }

  if (getenv ("CUBRID_DATABASES") == NULL)
    {
      print_errmsg ("$CUBRID_DATABASES environment variable is not defined.");
      error = -1;
    }

  snprintf (libcubridsa_path, BUF_LEN, "%s/lib/libcubridsa.so", CUBRID_ENV);

  if (migrate_get_db_path (dbname) < 0)
    {
      return -1;
    }

  fprintf (out, "%s reading\n", db_path);

  /* dynamic loading (libcubridsa.so) */
  dl_handle = dlopen (libcubridsa_path, RTLD_LAZY);
  if (dl_handle == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_get_database_version = (DB_GET_DATABASE_VERSION) dlsym (dl_handle, "db_get_database_version");
  if (cub_db_get_database_version == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_au_disable_passwords = (AU_DISABLE_PASSWORDS) dlsym (dl_handle, "au_disable_passwords");
  if (cub_au_disable_passwords == NULL)
    {
      cub_au_disable_passwords = (AU_DISABLE_PASSWORDS) dlsym (dl_handle, "_Z20au_disable_passwordsv");
      if (cub_au_disable_passwords == NULL)
	{
	  PRINT_LOG ("%s", dlerror ());
	  error = -1;
	}
    }

  cub_db_restart_ex = (DB_RESTART_EX) dlsym (dl_handle, "db_restart_ex");
  if (cub_db_restart_ex == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_er_errid = (ER_ERRID) dlsym (dl_handle, "er_errid");
  if (cub_er_errid == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_open_buffer = (DB_OPEN_BUFFER) dlsym (dl_handle, "db_open_buffer");
  if (cub_db_open_buffer == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_compile_statement = (DB_COMPILE_STATEMENT) dlsym (dl_handle, "db_compile_statement");
  if (cub_db_compile_statement == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_execute_statement_local = (DB_EXECUTE_STATEMENT_LOCAL) dlsym (dl_handle, "db_execute_statement_local");
  if (cub_db_execute_statement_local == NULL)
    {
      cub_db_execute_statement_local =
	(DB_EXECUTE_STATEMENT_LOCAL) dlsym (dl_handle,
					    "_Z26db_execute_statement_localP10db_sessioniPP15db_query_result");
      if (cub_db_execute_statement_local == NULL)
	{
	  PRINT_LOG ("%s", dlerror ());
	  error = -1;
	}
    }

  cub_db_query_get_tuple_value = (DB_QUERY_GET_TUPLE_VALUE) dlsym (dl_handle, "db_query_get_tuple_value");
  if (cub_db_query_get_tuple_value == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_query_first_tuple = (DB_QUERY_TUPLE) dlsym (dl_handle, "db_query_first_tuple");
  if (cub_db_query_first_tuple == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_query_next_tuple = (DB_QUERY_TUPLE) dlsym (dl_handle, "db_query_next_tuple");
  if (cub_db_query_next_tuple == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_query_tuple_count = (DB_QUERY_TUPLE) dlsym (dl_handle, "db_query_tuple_count");
  if (cub_db_query_tuple_count == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_close_session = (DB_CLOSE_SESSION) dlsym (dl_handle, "db_close_session");
  if (cub_db_close_session == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_query_end = (DB_QUERY_END) dlsym (dl_handle, "db_query_end");
  if (cub_db_query_end == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_commit_transaction = (DB_COMMIT_TRANSACTION) dlsym (dl_handle, "db_commit_transaction");
  if (cub_db_commit_transaction == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_abort_transaction = (DB_ABORT_TRANSACTION) dlsym (dl_handle, "db_abort_transaction");
  if (cub_db_abort_transaction == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_shutdown = (DB_SHUTDOWN) dlsym (dl_handle, "db_shutdown");
  if (cub_db_shutdown == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_db_error_string = (DB_ERROR_STRING) dlsym (dl_handle, "db_error_string");
  if (cub_db_error_string == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_Au_disable = (int *) dlsym (dl_handle, "Au_disable");
  if (cub_Au_disable == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_req_class_table = (DB_OBJECT ***) dlsym (dl_handle, "req_class_table");
  if (cub_req_class_table == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_required_class_only = (bool *) dlsym (dl_handle, "required_class_only");
  if (cub_required_class_only == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_include_references = (bool *) dlsym (dl_handle, "include_references");
  if (cub_include_references == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_input_filename = (char **) dlsym (dl_handle, "input_filename");
  if (cub_input_filename == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_do_schema = (bool *) dlsym (dl_handle, "do_schema");
  if (cub_do_schema == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_class_table = (LIST_MOPS **) dlsym (dl_handle, "class_table");
  if (cub_class_table == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_sm_Root_class_mop = (MOP *) dlsym (dl_handle, "sm_Root_class_mop");
  if (cub_sm_Root_class_mop == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_locator_get_all_mops =
    (LOCATOR_GET_ALL_MOPS) dlsym (dl_handle,
				  "_Z20locator_get_all_mopsP9db_object13DB_FETCH_MODEP21LC_FETCH_VERSION_TYPE");
  if (cub_locator_get_all_mops == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_au_fetch_class_force =
    (AU_FETCH_FORCE) dlsym (dl_handle,
				  "_Z20au_fetch_class_forceP9db_objectPP8sm_class12au_fetchmode");
  if (cub_au_fetch_class_force == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_locator_free_list_mops =
    (LOCATOR_FREE_LIST) dlsym (dl_handle,
				  "_Z22locator_free_list_mopsP9list_mops");
  if (cub_locator_free_list_mops == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_locator_find_class = (LOCATOR_FIND_CLASS) dlsym (dl_handle, "_Z18locator_find_classPKc");
  if (cub_locator_find_class == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_au_fetch_class =
    (AU_FETCH_CLASS) dlsym (dl_handle, "_Z14au_fetch_classP9db_objectPP8sm_class12au_fetchmode7DB_AUTH");
  if (cub_au_fetch_class == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_extract_classes_to_file =
    (EXTRACT_CLASSES_TO_FILE) dlsym (dl_handle, "_Z23extract_classes_to_fileR15extract_contextPKc");
  if (cub_locator_find_class == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  cub_sm_mark_system_classes =
    (SM_MARK_SYSTEM_CLASSES)dlsym (dl_handle, "_Z22sm_mark_system_classesv");
  if (cub_sm_mark_system_classes == NULL)
    {
      PRINT_LOG ("%s", dlerror ());
      error = -1;
    }

  return error;
}

static int
migrate_execute_query (const char *query)
{
  DB_QUERY_RESULT *result;
  int error;

  error = cub_db_execute_query (query, &result);

  if (verbose)
    {
      fprintf (out, "\tmigrate: execute query: %s\n", query);
    }

  if (error < 0)
    {
      fprintf (out, "migrate: execute query failed \"%s\"\n", query);
      return -1;
    }
  cub_db_query_end (result);

  return NO_ERROR;
}

static int
migrate_get_view_list (char ***view_list)
{
  DB_QUERY_RESULT *result;
  int cnt = 0, error;
  char **list;

  error = cub_db_execute_query (view_query, &result);
  if (error < 0)
    {
      fprintf (out, "view query: execute query failed\n \"%s\"\n", view_query);
      return -1;
    }

  cnt = cub_db_query_tuple_count (result);

  if (cnt == 0)
    {
      *view_list = NULL;
      return 0;
    }

  list = (char **) malloc (cnt * sizeof (char *));
  *view_list = list;

  cnt = 0;
  if ((error = cub_db_query_first_tuple (result)) == DB_CURSOR_SUCCESS)
    {
      do
	{
	  DB_VALUE value;

	  /* from query */
	  error = cub_db_query_get_tuple_value (result, 0, &value);
	  if (error < 0)
	    {
	      fprintf (out, "view_list: can not get a tuple for \"%s\"\n", view_query);
	      return -1;
	    }

	  list[cnt++] = strdup (value.data.ch.medium.buf);

	  error = cub_db_query_next_tuple (result);
	}
      while (error != DB_CURSOR_END && error != DB_CURSOR_ERROR);
    }

end:
  cub_db_query_end (result);

  if (error < 0)
    {
      fprintf (out, "generated: can not get a next tuple for \"%s\"\n", view_query);
      return -1;
    }

  return cnt;
}

static int
migrate_generated (const char *generated, int col_num)
{
  DB_QUERY_RESULT *gen_result;
  DB_QUERY_RESULT *result;
  const char *query;
  int error;

  error = cub_db_execute_query (generated, &gen_result);
  if (error < 0)
    {
      fprintf (out, "generated: execute query failed\n \"%s\"\n", generated);
      return -1;
    }

  if ((error = cub_db_query_first_tuple (gen_result)) == DB_CURSOR_SUCCESS)
    {
      do
	{
	  int i;
	  DB_VALUE value;

	  for (i = 0; i < col_num; i++)
	    {
	      /* from query */
	      error = cub_db_query_get_tuple_value (gen_result, i, &value);
	      if (error < 0)
		{
		  fprintf (out, "generated: can not get a tuple for \"%s\"\n", generated);
		  return -1;
		}

	      query = value.data.ch.medium.buf;
	      if (query == NULL)
		{
		  goto end;
		}

	      /* from generated result */
	      error = migrate_execute_query (query);
	      if (error < 0)
		{
		  return error;
		}
	    }
	  error = cub_db_query_next_tuple (gen_result);
	}
      while (error != DB_CURSOR_END && error != DB_CURSOR_ERROR);
    }

end:
  cub_db_query_end (gen_result);

  if (error < 0)
    {
      fprintf (out, "generated: can not get a next tuple for \"%s\"\n", generated);
      return -1;
    }

  return 0;
}

int is_system_view(const char *name)
{
  int i;

  for (i = 0; i < sizeof(system_views) / sizeof(const char *); i++)
    {
      if (strstr((char *)name, system_views[i]))
	{
	  return 1;
	}
    }

  return 0;
}

void migrate_mark_system_classes_for_system_views(void)
{
  LIST_MOPS *lmops;
  SM_CLASS *class_;
  int i;

  lmops = cub_locator_get_all_mops (*cub_sm_Root_class_mop, DB_FETCH_QUERY_WRITE, NULL);
  for (i = 0; i < lmops->num; i++)
    {
      if (lmops->mops[i] == *cub_sm_Root_class_mop)
	{
	  continue;
	}
      if (cub_au_fetch_class_force (lmops->mops[i], &class_, AU_FETCH_UPDATE) == NO_ERROR 
		&& is_system_view(class_->header.ch_name))
        {
          class_->flags |= SM_CLASSFLAG_SYSTEM;
        }
    }

  cub_locator_free_list_mops (lmops);
}

static int
migrate_queries ()
{
  DB_QUERY_RESULT *result;
  DB_VALUE value;
  int i, error;

  /* generated query for rename */
  error = migrate_generated (rename_query, 1);
  if (error < 0)
    {
      fprintf (out, "migrate: execute query failed \"%s\"\n", rename_query);
      return -1;
    }

  /* system view query */
  for (i = 0; i < sizeof (system_view_query) / sizeof (const char *); i++)
    {
      error = migrate_execute_query (system_view_query[i]);
      if (error < 0)
	{
	  return -1;
	}
    }

  migrate_mark_system_classes_for_system_views();

  error = cub_db_commit_transaction ();

  /* catalog query */
  for (i = 0; i < sizeof (catalog_query) / sizeof (const char *); i++)
    {
      error = migrate_execute_query (catalog_query[i]);
      if (error < 0)
	{
	  return -1;
	}
    }

  error = migrate_execute_query (update_db_class_not_for_system_classes);
  if (error < 0)
    {
      return -1;
    }

  error = migrate_generated (serial_query, 1);
  if (error < 0)
    {
      fprintf (out, "migrate: execute query failed \"%s\"\n", serial_query);
      return -1;
    }

  for (i = 0; i < sizeof (update_serial) / sizeof (const char *); i++)
    {
      error = migrate_execute_query (update_serial[i]);
      if (error < 0)
	{
	  return -1;
	}
    }

  error = migrate_execute_query (update_trigger);
  if (error < 0)
    {
      return -1;
    }

  /* index query */
  for (i = 0; i < sizeof (index_query) / sizeof (const char *); i++)
    {
      error = migrate_execute_query (index_query[i]);
      if (error < 0)
	{
	  return -1;
	}
    }

  error = migrate_execute_query (update_db_class_for_system_classes);
  if (error < 0)
    {
      return -1;
    }

  return 0;
}

static void
print_usage (void)
{
  printf ("usage: migrate [options] db-name\n\n");
  printf ("valid options\n");
  printf ("  -v:\t\tverbose detailed\n");
  printf ("  -o FILE:\tredirect output to FILE\n");
}

static char *
parse_args (int argc, char *argv[])
{
  int i;
  char *dbname = NULL;
  char *outfile = { 0 };

  out = stdout;

  for (i = 1; i < argc; i++)
    {
      if (argv[i][0] == '-')
	{
	  switch (argv[i][1])
	    {
	    case 'v':
	      verbose = 1;
	      break;
	    case 'o':
	      if (argc > i + 1)
		{
		  outfile = argv[++i];
		  if (*outfile == '-')
		    {
		      printf ("error: missing output file\n\n");
		      goto error_exit;
		    }
		  else
		    {
		      out = fopen (outfile, "w");
		      if (out == NULL)
			{
			  printf ("error: can not open out file\n\n");
			  goto error_exit;
			}
		    }
		}
	      else
		{
		  printf ("error: missing out file\n\n");
		  goto error_exit;
		}
	      break;
	    default:
	      printf ("error: no such option\n\n");
	      goto error_exit;
	    }
	}
      else
	{
	  if (dbname == NULL)
	    {
	      dbname = argv[i];
	    }
	  else
	    {
	      printf ("error: not allowed multi DB\n\n");
	      goto error_exit;
	    }
	}
    }

  if (dbname == NULL)
    {
      printf ("error: missing db-name\n\n");
      goto error_exit;
    }

  return dbname;

error_exit:
  print_usage ();
  return NULL;
}

int
main (int argc, char *argv[])
{
  int status, error;
  char *dbname;
  char **view_list;
  int view_cnt;

  if (argc < 2)
    {
      print_usage ();
      return -1;
    }
  else
    {
      dbname = parse_args (argc, argv);
      if (dbname == NULL)
	{
	  return -1;
	}
    }

  printf ("\n");
  printf ("Phase 1: Initializing\n");

  error = migrate_initialize (dbname);
  if (error < 0)
    {
      fprintf (out, "migrate: error encountered while initializing\n");
      return -1;
    }

  printf ("\n");
  printf ("Phase 2: Checking log volume\n");

  status = migrate_check_log_volume (dbname);

  if (status < 0)
    {
      printf ("migrate: encountered error while checking the log volume\n");
      return -1;
    }

  printf ("\n");
  printf ("Phase 3: Backup the log volume\n");

  /* backup first before upating catalog */
  if (migrate_backup_log_volume (dbname) < 0)
    {
      printf ("migrate: encountered error while backup the log volume\n");
      return -1;
    }

  cub_au_disable_passwords ();

  error = cub_db_restart_ex ("migrate", dbname, "DBA", NULL, NULL, DB_CLIENT_TYPE_ADMIN_UTILITY);
  if (error)
    {
      PRINT_LOG ("migrate: db_restart_ex () call failed [err_code: %d, err_msg: %s]", error, cub_db_error_string (1));
      return -1;
    }

  *cub_Au_disable = 1;

  printf ("\n");
  printf ("Phase 4: Extracting Views\n");

  /* get view list from database by query related to view */
  view_cnt = migrate_get_view_list (&view_list);
  if (view_cnt < 0)
    {
      printf ("migrate: encountered error while get view list\n");
      return -1;
    }

  if (view_list)
    {
      int i;
      FILE *f_query, *f_view;
      char sql[4096];
      char view_list_file[256];
      char view_query_file[256];

      /* make view list file as like unload schema */
      sprintf (view_list_file, "%s_schema", dbname);
      if (migrate_extract_views (view_cnt, view_list, view_list_file) < 0)
	{
	  printf ("migrate: encountered error while extracting the views\n");
	  return -1;
	}

      /* make view query file for drop view queries */
      sprintf (view_query_file, "%s.view", dbname);
      f_query = fopen (view_query_file, "w");
      if (f_query == NULL)
	{
	  printf ("migrate: encountered error while opening view query file\n");
	  return -1;
	}

      /* readey to make queries to drop views */
      for (i = 0; i < view_cnt; i++)
	{
	  fprintf (f_query, "DROP VIEW [%s];\n", view_list[i]);
	}
      fprintf (f_query, "\n");

      /* concatenate view list file and view query file */
      f_view = fopen (view_list_file, "r");
      if (f_view == NULL)
	{
	  printf ("migrate: encountered error while opening view list file\n");
	  return -1;
	}

      /* copy and concat from view schema */
      while (fgets (sql, 4096, f_view))
	{
	  fprintf (f_query, "%s", sql);
	}

      fclose (f_query);
      fclose (f_view);

      free (view_list);
    }

  printf ("\n");
  printf ("Phase 5: Executing the mirgate queries\n");

  error = migrate_queries ();
  if (error < 0)
    {
      printf ("migrate: error encountered while executing quries\n");
      if (cub_db_abort_transaction () < 0)
	{
	  printf ("migrate: error encountered while aborting\n");
	}
      cub_db_shutdown ();
      goto end;
    }

  error = cub_db_commit_transaction ();
  if (error < 0)
    {
      printf ("migrate: error encountered while committing\n");
      cub_db_shutdown ();
      goto end;
    }

  cub_db_shutdown ();

end:
  if (error < 0)
    {
      return -1;
    }

  printf ("\n");
  printf ("Phase 6: Updating version info for log volume\n");

  /* finalizing: update volume info. to 11.2 */
  migrate_update_log_volume (dbname);

  return 0;
}
