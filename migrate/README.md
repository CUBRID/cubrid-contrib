# migrate
The "migrate" is a utility tool for upgrading DB disk image from 11.0.x or 11.1.x to 11.2.

# Supported version
- database disk image: 11.0.x
- engine version: 11.2.x

# Prerequisites
You need to upgrade to 11.0.9 or later if you have a disk image of 11.0 version and you should upgrade your DBMS engine to 11.2.2 or later if you have CUBRID 11.2 engine.

# Quick Start
You can get a upgrade tool by following the instructions.
```sh
$ git clone https://github.com/CUBRID/cubrid-contrib.git
$ cd cubrid-contrib/migrate
$ make migrate
$ ./migrate your-database-name
```

# Files
You can get a upgrade disk image which is 11.2 compatible. In addition, you can get the files as like view-file and synonym-file. A view-file is a kind of sql command script file to create view used at old version. A synonym-file is also a kind of sql command script to create synonym for convinience. The synonym only includes tables grated at old version.

# Usage
```sh
usage: migrate [options] db-name

valid options
  -v:           verbose detailed
  -o FILE:      redirect output to FILE
```

# Example
```sh
$ ./migrate mdb
Phase 1: Initializing
/home/cubrid/CUBRID/databases/mdb/mdb_lgat reading

Phase 2: Checking log volume
11.0.10.0339

Phase 3: Backup the log volume
        backup volume, /home/cubrid/CUBRID/databases/mdb/mdb_lgat.bak created

Phase 4: Extracting Views

Phase 5: Creating synonyms

Phase 6: Executing the mirgate queries

Phase 7: Updating version info for log volume
migration done

$ ls mdb_*
mdb_schema  mdb_synonym mdb_view
```
