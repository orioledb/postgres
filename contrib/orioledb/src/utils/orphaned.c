/*-------------------------------------------------------------------------
 *
 * orphaned.c
 *		Functions to list and manage orphaned OrioleDB data files.
 *
 * Copyright (c) 2026, Oriole DB Inc.
 *
 * IDENTIFICATION
 *	  contrib/orioledb/src/utils/orphaned.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "orioledb.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_control.h"
#include "catalog/pg_tablespace_d.h"
#include "commands/dbcommands.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include <dirent.h>
#include <sys/stat.h>

#define ORIOLEDB_ORPHANED_BACKUP_DIR	"orioledb_orphaned_backup"

typedef struct OrphanedRelation
{
	char	   *dbname;
	char	   *path;
	char	   *name;
	int64		size;
	TimestampTz mod_time;
	Oid			relfilenode;
	Oid			reloid;
} OrphanedRelation;

typedef struct
{
	Oid			reltablespace;
	Oid			relfilenode;
} RelfilenodeMapKeyDirty;

typedef struct
{
	RelfilenodeMapKeyDirty key;
	Oid			relid;
} RelfilenodeMapEntryDirty;

static bool made_directory = false;
static bool found_existing_directory = false;
static Timestamp limitts;
static TimestampTz last_checkpoint_time;
static List *list_orphaned_relations = NIL;
static HTAB *RelfilenodeMapHashDirty = NULL;
static ScanKeyData relfilenode_skey_dirty[2];

static void orioledb_list_orphaned_internal(FunctionCallInfo fcinfo);
static void orioledb_build_orphaned_list(Oid dbOid, bool restore);
static void orioledb_search_orphaned(List **flist, Oid dboid, const char *dbname,
									 const char *dir, Oid reltablespace);
static void orioledb_verify_dir_is_empty_or_create(char *dirname, bool *created,
												   bool *found, bool display_hint);
static int orioledb_mkdir_p(char *path, int omode);
static int orioledb_check_dir(const char *dir);
static void require_superuser(void);
static Oid RelidByRelfilenodeDirty(Oid reltablespace, Oid relfilenode);
static void InitializeRelfilenodeMapDirty(void);
static bool is_directory_empty(const char *path);
static bool orioledb_parse_relnode_from_filename(const char *name, Oid *relnode);

PG_FUNCTION_INFO_V1(orioledb_list_orphaned);
PG_FUNCTION_INFO_V1(orioledb_list_orphaned_moved);
PG_FUNCTION_INFO_V1(orioledb_move_orphaned);
PG_FUNCTION_INFO_V1(orioledb_remove_moved_orphaned);
PG_FUNCTION_INFO_V1(orioledb_move_back_orphaned);

static int
orioledb_check_dir(const char *dir)
{
	int			result = 1;
	DIR		   *chkdir;
	struct dirent *file;
	bool		dot_found = false;
	bool		mount_found = false;
	int			readdir_errno;

	chkdir = opendir(dir);
	if (chkdir == NULL)
		return (errno == ENOENT) ? 0 : -1;

	while (errno = 0, (file = readdir(chkdir)) != NULL)
	{
		if (strcmp(".", file->d_name) == 0 ||
			strcmp("..", file->d_name) == 0)
			continue;
#ifndef WIN32
		else if (file->d_name[0] == '.')
			dot_found = true;
		else if (strcmp("lost+found", file->d_name) == 0)
			mount_found = true;
#endif
		else
		{
			result = 4;
			break;
		}
	}

	if (errno)
		result = -1;

	readdir_errno = errno;
	if (closedir(chkdir))
		result = -1;
	else
		errno = readdir_errno;

	if (result == 1 && mount_found)
		result = 3;

	if (result == 1 && dot_found)
		result = 2;

	return result;
}

static int
orioledb_mkdir_p(char *path, int omode)
{
	struct stat sb;
	mode_t		numask,
				oumask;
	int			last,
				retval;
	char	   *p;

	retval = 0;
	p = path;

	oumask = umask(0);
	numask = oumask & ~(S_IWUSR | S_IXUSR);
	(void) umask(numask);

	if (p[0] == '/')
		++p;
	for (last = 0; !last; ++p)
	{
		if (p[0] == '\0')
			last = 1;
		else if (p[0] != '/')
			continue;
		*p = '\0';
		if (!last && p[1] == '\0')
			last = 1;

		if (last)
			(void) umask(oumask);

		if (stat(path, &sb) == 0)
		{
			if (!S_ISDIR(sb.st_mode))
			{
				if (last)
					errno = EEXIST;
				else
					errno = ENOTDIR;
				retval = -1;
				break;
			}
		}
		else if (mkdir(path, last ? omode : S_IRWXU | S_IRWXG | S_IRWXO) < 0)
		{
			retval = -1;
			break;
		}
		if (!last)
			*p = '/';
	}

	(void) umask(oumask);

	return retval;
}

static bool
orioledb_parse_relnode_from_filename(const char *name, Oid *relnode)
{
	uint32		file_relnode;
	uint32		file_chkp;
	uint32		file_segno;
	char		file_ext[5];

	if (sscanf(name, "%10u-%10u.%4s",
			   &file_relnode, &file_chkp, file_ext) == 3 &&
		(!strcmp(file_ext, "tmp") || !strcmp(file_ext, "map") ||
		 !strcmp(file_ext, "evt")))
	{
		*relnode = (Oid) file_relnode;
		return true;
	}

	if (sscanf(name, "%10u.%10u-%10u",
			   &file_relnode, &file_segno, &file_chkp) == 3)
	{
		*relnode = (Oid) file_relnode;
		return true;
	}

	if (sscanf(name, "%10u.%10u", &file_relnode, &file_segno) == 2)
	{
		*relnode = (Oid) file_relnode;
		return true;
	}

	if (sscanf(name, "%10u-%10u", &file_relnode, &file_chkp) == 2)
	{
		*relnode = (Oid) file_relnode;
		return true;
	}

	if (sscanf(name, "%10u", &file_relnode) == 1)
	{
		*relnode = (Oid) file_relnode;
		return true;
	}

	return false;
}

static void
orioledb_build_orphaned_list(Oid dbOid, bool restore)
{
	const char *dbName;
	DIR		   *dirdesc;
	struct dirent *direntry;
	char		dirpath[MAXPGPATH];
	char		dir[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY)];
	Oid			reltbsnode = InvalidOid;
	char	   *reltbsname;
	ControlFileData *ControlFile;
	bool		crc_ok;
	time_t		time_tmp;
	MemoryContext mctx;

	dbName = get_database_name(MyDatabaseId);

	ControlFile = get_controlfile(".", &crc_ok);
	if (!crc_ok)
		ereport(ERROR,
				(errmsg("pg_control CRC value is incorrect")));

	time_tmp = (time_t) ControlFile->checkPointCopy.time;
	last_checkpoint_time = time_t_to_timestamptz(time_tmp);

	mctx = MemoryContextSwitchTo(TopMemoryContext);

	list_free_deep(list_orphaned_relations);
	list_orphaned_relations = NIL;

	if (!restore)
		snprintf(dir, sizeof(dir), "%s/%u", ORIOLEDB_DATA_DIR, dbOid);
	else
		snprintf(dir, sizeof(dir), "%s/%u/" ORIOLEDB_DATA_DIR "/%u",
				 ORIOLEDB_ORPHANED_BACKUP_DIR, dbOid, dbOid);

	orioledb_search_orphaned(&list_orphaned_relations, dbOid, dbName, dir, 0);

	if (!restore)
		snprintf(dirpath, MAXPGPATH, "pg_tblspc");
	else
		snprintf(dirpath, MAXPGPATH, "%s/%u/pg_tblspc",
				 ORIOLEDB_ORPHANED_BACKUP_DIR, dbOid);

	if (restore && orioledb_check_dir(dirpath) != 4)
		return;

	dirdesc = AllocateDir(dirpath);

	while ((direntry = ReadDir(dirdesc, dirpath)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		if (!restore)
			snprintf(dir, sizeof(dir), "pg_tblspc/%s/%s/" ORIOLEDB_DATA_DIR "/%u",
					 direntry->d_name, TABLESPACE_VERSION_DIRECTORY, dbOid);
		else
			snprintf(dir, sizeof(dir), "%s/%u/pg_tblspc/%s/%s/" ORIOLEDB_DATA_DIR "/%u",
					 ORIOLEDB_ORPHANED_BACKUP_DIR, dbOid,
					 direntry->d_name, TABLESPACE_VERSION_DIRECTORY, dbOid);

		reltbsname = strdup(direntry->d_name);
		reltbsnode = (Oid) strtoul(reltbsname, &reltbsname, 10);

		orioledb_search_orphaned(&list_orphaned_relations, dbOid, dbName, dir,
								 reltbsnode);
	}
	FreeDir(dirdesc);
	MemoryContextSwitchTo(mctx);
}

Datum
orioledb_list_orphaned(PG_FUNCTION_ARGS)
{
	require_superuser();

	if (PG_ARGISNULL(0))
		limitts = GetCurrentTimestamp() - ((3600000 * 24) * (int64) 1000);
	else
		limitts = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval,
														TimestampGetDatum(GetCurrentTimestamp()),
														IntervalPGetDatum(PG_GETARG_INTERVAL_P(0))));

	orioledb_build_orphaned_list(MyDatabaseId, false);
	orioledb_list_orphaned_internal(fcinfo);
	return (Datum) 0;
}

Datum
orioledb_list_orphaned_moved(PG_FUNCTION_ARGS)
{
	require_superuser();

	orioledb_build_orphaned_list(MyDatabaseId, true);
	orioledb_list_orphaned_internal(fcinfo);
	return (Datum) 0;
}

static void
orioledb_list_orphaned_internal(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	ListCell   *cell;

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	foreach(cell, list_orphaned_relations)
	{
		OrphanedRelation *orph = (OrphanedRelation *) lfirst(cell);
		Datum		values[8];
		bool		nulls[8];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = CStringGetTextDatum(orph->dbname);
		values[1] = CStringGetTextDatum(orph->path);
		values[2] = CStringGetTextDatum(orph->name);
		values[3] = Int64GetDatum(orph->size);
		values[4] = TimestampTzGetDatum(orph->mod_time);
		values[5] = Int64GetDatum(orph->relfilenode);
		values[6] = Int64GetDatum(orph->reloid);
		values[7] = BoolGetDatum(orph->mod_time <= limitts);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
}

static void
orioledb_search_orphaned(List **flist, Oid dboid PG_USED_FOR_ASSERTS_ONLY,
						 const char *dbname, const char *dir, Oid reltablespace)
{
	Oid			oidrel;
	Oid			relfilenode;
	DIR		   *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(dir);
	if (!dirdesc)
		return;

	while ((de = ReadDir(dirdesc, dir)) != NULL)
	{
		char		path[MAXPGPATH * 2];
		struct stat attrib;
		OrphanedRelation *orph;
		TimestampTz segment_time;
		bool		is_base_file;

		if (de->d_name[0] == '.')
			continue;

		if (!orioledb_parse_relnode_from_filename(de->d_name, &relfilenode))
			continue;

		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &attrib) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", path)));

		if (!S_ISREG(attrib.st_mode))
			continue;

		oidrel = RelidByRelfilenodeDirty(reltablespace, relfilenode);
		segment_time = time_t_to_timestamptz(attrib.st_mtime);
		is_base_file = (strchr(de->d_name, '.') == NULL &&
						strchr(de->d_name, '-') == NULL);

		if (!OidIsValid(oidrel) &&
			!(is_base_file && attrib.st_size == 0 &&
			  segment_time > last_checkpoint_time))
		{
			orph = palloc(sizeof(*orph));
			orph->dbname = strdup(dbname);
			orph->path = strdup(dir);
			orph->name = strdup(de->d_name);
			orph->size = (int64) attrib.st_size;
			orph->mod_time = segment_time;
			orph->relfilenode = relfilenode;
			orph->reloid = oidrel;
			*flist = lappend(*flist, orph);
		}
	}

	FreeDir(dirdesc);
}

Datum
orioledb_move_orphaned(PG_FUNCTION_ARGS)
{
	Oid			dbOid;
	ListCell   *cell;
	char	   *dir_to_create;
	int			nb_moved;

	require_superuser();

	if (PG_ARGISNULL(0))
		limitts = GetCurrentTimestamp() - ((3600000 * 24) * (int64) 1000);
	else
		limitts = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval,
														TimestampGetDatum(GetCurrentTimestamp()),
														IntervalPGetDatum(PG_GETARG_INTERVAL_P(0))));

	dbOid = MyDatabaseId;
	orioledb_build_orphaned_list(dbOid, false);
	dir_to_create = psprintf("%s/%u", ORIOLEDB_ORPHANED_BACKUP_DIR, dbOid);

	orioledb_verify_dir_is_empty_or_create(dir_to_create, &made_directory,
										   &found_existing_directory, true);
	nb_moved = 0;

	foreach(cell, list_orphaned_relations)
	{
		char		orphaned_file[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
		char		orphaned_file_backup_dir[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
		char		orphaned_file_backup[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
		OrphanedRelation *orph = (OrphanedRelation *) lfirst(cell);

		snprintf(orphaned_file, sizeof(orphaned_file), "%s/%s",
				 orph->path, orph->name);
		snprintf(orphaned_file_backup_dir, sizeof(orphaned_file_backup_dir),
				 "%s/%s", dir_to_create, orph->path);

		if (orioledb_check_dir(orphaned_file_backup_dir) == 0)
			orioledb_verify_dir_is_empty_or_create(orphaned_file_backup_dir,
												   &made_directory,
												   &found_existing_directory,
												   false);

		snprintf(orphaned_file_backup, sizeof(orphaned_file_backup),
				 "%s/%s", orphaned_file_backup_dir, orph->name);

		if (orph->mod_time <= limitts)
		{
			if (rename(orphaned_file, orphaned_file_backup) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not rename \"%s\" to \"%s\": %m",
								orphaned_file, orphaned_file_backup)));
			nb_moved++;
		}
	}

	PG_RETURN_INT32(nb_moved);
}

Datum
orioledb_remove_moved_orphaned(PG_FUNCTION_ARGS)
{
	Oid			dbOid;
	char	   *dir_to_remove;

	require_superuser();

	dbOid = MyDatabaseId;
	dir_to_remove = psprintf("%s/%u", ORIOLEDB_ORPHANED_BACKUP_DIR, dbOid);

	if (!rmtree(dir_to_remove, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", dir_to_remove)));

	dir_to_remove = psprintf("%s", ORIOLEDB_ORPHANED_BACKUP_DIR);

	if (is_directory_empty(dir_to_remove) && !rmtree(dir_to_remove, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", dir_to_remove)));

	PG_RETURN_VOID();
}

Datum
orioledb_move_back_orphaned(PG_FUNCTION_ARGS)
{
	Oid			dbOid;
	ListCell   *cell;
	int			nb_moved;

	require_superuser();

	dbOid = MyDatabaseId;
	nb_moved = 0;

	if (orioledb_check_dir(ORIOLEDB_ORPHANED_BACKUP_DIR) != 4)
		PG_RETURN_INT32(nb_moved);

	orioledb_build_orphaned_list(dbOid, true);

	foreach(cell, list_orphaned_relations)
	{
		char		orphaned_file_backup[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
		char	   *orphaned_file_restore;
		char	   *orphaned_file_restore_dup;
		OrphanedRelation *orph = (OrphanedRelation *) lfirst(cell);

		snprintf(orphaned_file_backup, sizeof(orphaned_file_backup),
				 "%s/%s", orph->path, orph->name);

		orphaned_file_restore_dup = strdup(orphaned_file_backup);
		orphaned_file_restore = strchr(orphaned_file_restore_dup, '/');
		orphaned_file_restore_dup = orphaned_file_restore + 1;
		orphaned_file_restore = strchr(orphaned_file_restore_dup, '/');

		if (rename(orphaned_file_backup, orphaned_file_restore + 1) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rename \"%s\" to \"%s\": %m",
							orphaned_file_backup, orphaned_file_restore + 1)));
		nb_moved++;
	}

	PG_RETURN_INT32(nb_moved);
}

static void
orioledb_verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found,
									   bool display_hint)
{
	switch (orioledb_check_dir(dirname))
	{
		case 0:
			if (orioledb_mkdir_p(dirname, pg_dir_create_mode) == -1)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create directory \"%s\": %m",
								dirname)));
			if (created)
				*created = true;
			return;
		case 1:
			if (found)
				*found = true;
			return;
		case 2:
		case 3:
		case 4:
			if (!display_hint)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("directory \"%s\" exists but is not empty",
								dirname)));
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("directory \"%s\" exists but is not empty",
								dirname),
						 errhint("please check no files exist with orioledb_list_orphaned_moved(), move them back (if any) with orioledb_move_back_orphaned() and then clean \"%s\" up with orioledb_remove_moved_orphaned()",
								 dirname)));
			break;
		case -1:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access directory \"%s\": %m",
							dirname)));
			break;
	}
}

static void
require_superuser(void)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can execute orioledb orphaned functions")));
}

static Oid
RelidByRelfilenodeDirty(Oid reltablespace, Oid relfilenode)
{
	RelfilenodeMapKeyDirty key;
	RelfilenodeMapEntryDirty *entry;
	bool		found;
	SysScanDesc scandesc;
	Relation	relation;
	HeapTuple	ntp;
	ScanKeyData skey[2];
	Oid			relid;
	SnapshotData DirtySnapshot;

	InitDirtySnapshot(DirtySnapshot);

	if (RelfilenodeMapHashDirty == NULL)
		InitializeRelfilenodeMapDirty();

	if (reltablespace == MyDatabaseTableSpace)
		reltablespace = 0;

	MemSet(&key, 0, sizeof(key));
	key.reltablespace = reltablespace;
	key.relfilenode = relfilenode;

	entry = hash_search(RelfilenodeMapHashDirty, &key, HASH_FIND, &found);

	if (found)
		return entry->relid;

	relid = InvalidOid;

	if (reltablespace == GLOBALTABLESPACE_OID)
		relid = RelationMapFilenumberToOid(relfilenode, true);
	else
	{
		relation = table_open(RelationRelationId, AccessShareLock);
		memcpy(skey, relfilenode_skey_dirty, sizeof(skey));
		skey[0].sk_argument = ObjectIdGetDatum(reltablespace);
		skey[1].sk_argument = ObjectIdGetDatum(relfilenode);

		scandesc = systable_beginscan(relation,
									  ClassTblspcRelfilenodeIndexId,
									  true,
									  &DirtySnapshot,
									  2,
									  skey);

		found = false;

		while (HeapTupleIsValid(ntp = systable_getnext(scandesc)))
		{
			Form_pg_class classform = (Form_pg_class) GETSTRUCT(ntp);

			found = true;
			Assert(classform->reltablespace == reltablespace);
			Assert(classform->relfilenode == relfilenode);
			relid = classform->oid;
		}

		systable_endscan(scandesc);
		table_close(relation, AccessShareLock);

		if (!found)
			relid = RelationMapFilenumberToOid(relfilenode, false);
	}

	entry = hash_search(RelfilenodeMapHashDirty, &key, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "corrupted hashtable");
	entry->relid = relid;

	return relid;
}

static void
RelfilenodeMapInvalidateCallbackDirty(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	RelfilenodeMapEntryDirty *entry;

	Assert(RelfilenodeMapHashDirty != NULL);

	hash_seq_init(&status, RelfilenodeMapHashDirty);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (relid == InvalidOid ||
			entry->relid == InvalidOid ||
			entry->relid == relid)
		{
			if (hash_search(RelfilenodeMapHashDirty,
							&entry->key,
							HASH_REMOVE,
							NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

static void
InitializeRelfilenodeMapDirty(void)
{
	HASHCTL		ctl;
	int			i;

	if (CacheMemoryContext == NULL)
		CreateCacheMemoryContext();

	MemSet(&relfilenode_skey_dirty, 0, sizeof(relfilenode_skey_dirty));

	for (i = 0; i < 2; i++)
	{
		fmgr_info_cxt(F_OIDEQ,
					  &relfilenode_skey_dirty[i].sk_func,
					  CacheMemoryContext);
		relfilenode_skey_dirty[i].sk_strategy = BTEqualStrategyNumber;
		relfilenode_skey_dirty[i].sk_subtype = InvalidOid;
		relfilenode_skey_dirty[i].sk_collation = InvalidOid;
	}

	relfilenode_skey_dirty[0].sk_attno = Anum_pg_class_reltablespace;
	relfilenode_skey_dirty[1].sk_attno = Anum_pg_class_relfilenode;

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(RelfilenodeMapKeyDirty);
	ctl.entrysize = sizeof(RelfilenodeMapEntryDirty);
	ctl.hcxt = CacheMemoryContext;

	RelfilenodeMapHashDirty =
		hash_create("RelfilenodeMap cache", 64, &ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	CacheRegisterRelcacheCallback(RelfilenodeMapInvalidateCallbackDirty,
								  (Datum) 0);
}

static bool
is_directory_empty(const char *path)
{
	DIR		   *dir;
	struct dirent *de;
	bool		is_empty = true;

	dir = AllocateDir(path);
	if (dir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", path)));

	while ((de = ReadDir(dir, path)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		is_empty = false;
		break;
	}

	FreeDir(dir);

	return is_empty;
}
