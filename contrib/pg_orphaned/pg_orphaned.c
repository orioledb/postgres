/*-------------------------------------------------------------------------
 *
 * pg_orphaned.c
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "utils/timestamp.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include <sys/stat.h>
#include <dirent.h>
#if PG_VERSION_NUM < 190000
#include "commands/dbcommands.h"
#else
#include "access/htup_details.h"
#include "utils/lsyscache.h"
#endif
#include "regex/regex.h"
#include "storage/fd.h"
#include "utils/hsearch.h"
#include "utils/tuplestore.h"

#if PG_VERSION_NUM < 110000
#include "catalog/pg_collation.h"
#include "catalog/catalog.h"
#include "utils/memutils.h"
#include "catalog/pg_tablespace.h"
#define pg_dir_create_mode S_IRWXU
#else
#include "catalog/pg_collation_d.h"
#include "catalog/pg_tablespace_d.h"
#include "common/file_perm.h"
#include "utils/rel.h"
#endif

#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/relmapper.h"
#include "catalog/indexing.h"
#include "utils/inval.h"

#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#include "access/genam.h"
#include "utils/snapmgr.h"
#else
#include "utils/tqual.h"
#include "access/htup_details.h"
#endif
#define NUMBER_SUFFIXES 2
#define MAX_SUFFIX_SIZE 5

#include "catalog/pg_control.h"
#include "common/controldata_utils.h"

PG_MODULE_MAGIC;
Datum pg_list_orphaned(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_list_orphaned);

Datum pg_list_orphaned_moved(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_list_orphaned_moved);

PG_FUNCTION_INFO_V1(pg_move_orphaned);
Datum pg_move_orphaned(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_remove_moved_orphaned);
Datum pg_remove_moved_orphaned(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_move_back_orphaned);
Datum pg_move_back_orphaned(PG_FUNCTION_ARGS);

static bool made_directory = false;
static bool found_existing_directory = false;
static char *orphaned_backup_dir= "orphaned_backup";
static Timestamp limitts;
static TimestampTz last_checkpoint_time;

static List   *list_orphaned_relations=NULL;
static void pg_list_orphaned_internal(FunctionCallInfo fcinfo);
static void search_orphaned(List **flist, Oid dboid, const char *dbname, const char *dir, Oid reltablespace);
static void pg_build_orphaned_list(Oid dbOid, bool restore);
static void verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found, bool display_hint);
static int pg_orphaned_mkdir_p(char *path, int omode);
static int pg_orphaned_check_dir(const char *dir);
static void requireSuperuser(void);
static Oid RelidByRelfilenodeDirty(Oid reltablespace, Oid relfilenode);
static void InitializeRelfilenodeMapDirty(void);
static bool is_directory_empty(const char *path);

/* Hash table for information about each relfilenode <-> oid pair */
static HTAB *RelfilenodeMapHashDirty = NULL;

/* built first time through in InitializeRelfilenodeMap */
static ScanKeyData relfilenode_skey_dirty[2];

typedef struct
{
	Oid                     reltablespace;
	Oid                     relfilenode;
} RelfilenodeMapKeyDirty;

typedef struct
{
	RelfilenodeMapKeyDirty key;          /* lookup key - must be first */
	Oid                     relid;                  /* pg_class.oid */
} RelfilenodeMapEntryDirty;

typedef struct OrphanedRelation {
	char	   *dbname;
	char	   *path;
	char	   *name;
	int size;
	TimestampTz mod_time;
	Oid relfilenode;
	Oid reloid;
} OrphanedRelation;

static void pgorph_add_suffix(List **flist, OrphanedRelation *orph);

/*
 * function to check the status of directory
 * this is mainly copy/paste from existing pg_check_dir
 * Returns:
 *              0 if nonexistent
 *              1 if exists and empty
 *              2 if exists and contains _only_ dot files
 *              3 if exists and contains a mount point
 *              4 if exists and not empty
 *              -1 if trouble accessing directory (errno reflects the error)
 */
int
pg_orphaned_check_dir(const char *dir)
{
	int             result = 1;
	DIR             *chkdir;
	struct dirent   *file;
	bool		dot_found = false;
	bool		mount_found = false;
	int		readdir_errno;

	chkdir = opendir(dir);
	if (chkdir == NULL)
		return (errno == ENOENT) ? 0 : -1;

	while (errno = 0, (file = readdir(chkdir)) != NULL)
	{
		if (strcmp(".", file->d_name) == 0 ||
			strcmp("..", file->d_name) == 0)
		{
			/* skip this and parent directory */
			continue;
		}
#ifndef WIN32
		/* file starts with "." */
		else if (file->d_name[0] == '.')
		{
			dot_found = true;
		}
		/* lost+found directory */
		else if (strcmp("lost+found", file->d_name) == 0)
		{
			mount_found = true;
		}
#endif
		else
		{
			/* not empty */
			result = 4;
			break;
		}
	}

	/* some kind of I/O error? */
	if (errno)
		result = -1;

	/* Close chkdir and avoid overwriting the readdir errno on success */
	readdir_errno = errno;
	if (closedir(chkdir))
		/* error executing closedir */
		result = -1;
	else
		errno = readdir_errno;

	/* We report on mount point if we find a lost+found directory */
	if (result == 1 && mount_found)
		result = 3;

	/* We report on dot-files if we _only_ find dot files */
	if (result == 1 && dot_found)
		result = 2;

	return result;
}

/*
 * function to create directory
 * used to create the backup directory
 * mainly copy/paste from existing pg_mkdir_p
 *
 * This is equivalent to "mkdir -p" except we don't complain if the target
 * directory already exists.
 *
 * We assume the path is in canonical form, i.e., uses / as the separator.
 *
 * omode is the file permissions bits for the target directory.  Note that any
 * parent directories that have to be created get permissions according to the
 * prevailing umask, but with u+wx forced on to ensure we can create there.
 * (We declare omode as int, not mode_t, to minimize dependencies for port.h.)
 *
 * Returns 0 on success, -1 (with errno set) on failure.
 *
 * Note that on failure, the path arg has been modified to show the particular
 * directory level we had problems with.
 */
int
pg_orphaned_mkdir_p(char *path, int omode)
{
	struct stat sb;
	mode_t		numask,
				oumask;
	int			last,
				retval;
	char	   *p;

	retval = 0;
	p = path;

	/*
	 * POSIX 1003.2: For each dir operand that does not name an existing
	 * directory, effects equivalent to those caused by the following command
	 * shall occur:
	 *
	 * mkdir -p -m $(umask -S),u+wx $(dirname dir) && mkdir [-m mode] dir
	 *
	 * We change the user's umask and then restore it, instead of doing
	 * chmod's.  Note we assume umask() can't change errno.
	 */
	oumask = umask(0);
	numask = oumask & ~(S_IWUSR | S_IXUSR);
	(void) umask(numask);

	/* Skip leading '/'. */
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

		/* check for pre-existing directory */
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

	/* ensure we restored umask */
	(void) umask(oumask);

	return retval;
}

/*
 * function to build the list
 * of orphaned files
 * the boolean is used to search
 * in the standard directory or in the backup one
 * the logic to go through each directory/tablespace
 * is mainly inspired from the existing calculate_database_size()
 */
void
pg_build_orphaned_list(Oid dbOid, bool restore)
{

	const char *dbName = NULL;
	DIR                *dirdesc;
	struct dirent *direntry;
	char            dirpath[MAXPGPATH];
	char            dir[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY)];
	Oid                     reltbsnode = InvalidOid;
	char *reltbsname;
	ControlFileData *ControlFile;
	bool        crc_ok;
	time_t      time_tmp;
	MemoryContext   mctx;

	dbName=get_database_name(MyDatabaseId);

	/* get a copy of the control file */
#if PG_VERSION_NUM >= 120000
	ControlFile = get_controlfile(".", &crc_ok);
#else
	ControlFile = get_controlfile(".", NULL, &crc_ok);
#endif
	if (!crc_ok)
		ereport(ERROR,(errmsg("pg_control CRC value is incorrect")));

	/* get last checkpoint time */
	time_tmp = (time_t) ControlFile->checkPointCopy.time;
	last_checkpoint_time = time_t_to_timestamptz(time_tmp);

	mctx = MemoryContextSwitchTo(TopMemoryContext);

	list_free_deep(list_orphaned_relations);
	list_orphaned_relations=NIL;

	/* default tablespace */
	if (!restore)
		snprintf(dir, sizeof(dir), "base/%u", dbOid);
	else
		snprintf(dir, sizeof(dir), "%s/%u/base/%u", orphaned_backup_dir, dbOid, dbOid);
	search_orphaned(&list_orphaned_relations, dbOid, dbName, dir, 0);

	/* Scan the non-default tablespaces */
	if (!restore)
		snprintf(dirpath, MAXPGPATH, "pg_tblspc");
	else
		snprintf(dirpath, MAXPGPATH, "%s/%u/pg_tblspc", orphaned_backup_dir, dbOid);

	/*
	 * In case no tablespaces in the dedicated backup dir
	 */
	if (restore && pg_orphaned_check_dir(dirpath) != 4)
		return;

	dirdesc = AllocateDir(dirpath);

	while ((direntry = ReadDir(dirdesc, dirpath)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		if (!restore)
			snprintf(dir, sizeof(dir), "pg_tblspc/%s/%s/%u",
				direntry->d_name, TABLESPACE_VERSION_DIRECTORY, dbOid);
		else
			snprintf(dir, sizeof(dir), "%s/%u/pg_tblspc/%s/%s/%u",
				orphaned_backup_dir, dbOid, direntry->d_name, TABLESPACE_VERSION_DIRECTORY, dbOid);

		reltbsname = strdup(direntry->d_name);
		reltbsnode = (Oid) strtoul(reltbsname, &reltbsname, 10);

		search_orphaned(&list_orphaned_relations, dbOid, dbName, dir, reltbsnode);
	}
	FreeDir(dirdesc);
	MemoryContextSwitchTo(mctx);
}

Datum
pg_list_orphaned(PG_FUNCTION_ARGS)
{
	requireSuperuser();

    if (PG_ARGISNULL(0))
		limitts = GetCurrentTimestamp() - ((3600000 * 24) * (int64) 1000); // 1 Day
	else
		limitts = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval, TimestampGetDatum(GetCurrentTimestamp()), IntervalPGetDatum(PG_GETARG_INTERVAL_P(0))));

	pg_build_orphaned_list(MyDatabaseId, false);
	pg_list_orphaned_internal(fcinfo);
	return (Datum) 0;
}

Datum
pg_list_orphaned_moved(PG_FUNCTION_ARGS)
{
	requireSuperuser();

	pg_build_orphaned_list(MyDatabaseId, true);
	pg_list_orphaned_internal(fcinfo);
	return (Datum) 0;
}

void
pg_list_orphaned_internal(FunctionCallInfo fcinfo)
{

	ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	TupleDesc           tupdesc;
	MemoryContext   per_query_ctx;
	MemoryContext   oldcontext;
	ListCell   *cell;

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * Build a tuple descriptor for our result type
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

#if (PG_VERSION_NUM < 130000)
	for (cell = list_head(list_orphaned_relations); cell != NULL; cell = lnext(cell))
#else
	for (cell = list_head(list_orphaned_relations); cell != NULL; cell = lnext(list_orphaned_relations, cell))
#endif
	{
		OrphanedRelation  *orph = (OrphanedRelation *)lfirst(cell);

		Datum           values[8];
		bool            nulls[8];
		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = CStringGetTextDatum(orph->dbname);
		values[1] = CStringGetTextDatum(orph->path);
		values[2] = CStringGetTextDatum(orph->name);
		values[3] = Int64GetDatum(orph->size);
		values[4] = TimestampTzGetDatum(orph->mod_time);
		values[5] = Int64GetDatum(orph->relfilenode);
		values[6] = Int64GetDatum(orph->reloid);

        if (orph->mod_time <= limitts)
            values[7] = BoolGetDatum(true);
        else
            values[7] = BoolGetDatum(false);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
}

/*
 * function that look for orphaned files
 * in a given directory
 * the logic to go through the list of files
 * is mainly inspired by the existing pg_ls_dir_files()
 */
void
search_orphaned(List **flist, Oid dboid, const char* dbname, const char* dir, Oid reltablespace)
{
	Oid                     oidrel = InvalidOid;
	Oid                     relfilenode = InvalidOid;
	char *relfilename;
	DIR                *dirdesc;
	struct dirent *de;
	OrphanedRelation *orph;
	TimestampTz segment_time;

	dirdesc = AllocateDir(dir);
	if (!dirdesc)
		return ;

	while ((de = ReadDir(dirdesc, dir)) != NULL)
	{
		char            path[MAXPGPATH * 2];
		struct stat attrib;

		/* Skip hidden files */
		if (de->d_name[0] == '.')
			continue;

		/* Get the file info */
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &attrib) < 0)
			ereport(ERROR,
				(errcode_for_file_access(),
				errmsg("could not stat directory \"%s\": %m", dir)));

		/* Ignore anything but regular files */
		if (!S_ISREG(attrib.st_mode))
			continue;

		/* Ignore non digit files */
		if (strstr(de->d_name, "_") == NULL && isdigit((unsigned char) *(de->d_name))) {
			orph = palloc(sizeof(*orph));
			relfilename = strdup(de->d_name);
			relfilenode = (Oid) strtoul(relfilename, &relfilename, 10);
			/* If RelidByRelfilenodeDirty does not return a valid oid
			 * then we consider this file as orphaned
			 */
			oidrel = RelidByRelfilenodeDirty(reltablespace, relfilenode);
			/*
			 * Filter and don't report as orphaned
			 * if first segment, size is zero and created after the last checkpoint
			 * due to https://github.com/postgres/postgres/blob/REL_12_8/src/backend/storage/smgr/md.c#L225
			 */
			segment_time = time_t_to_timestamptz(attrib.st_mtime);
			if (!OidIsValid(oidrel) && !(attrib.st_size == 0 &&
				strstr(de->d_name, ".") == NULL && segment_time > last_checkpoint_time))
			{
				orph->dbname = strdup(dbname);
				orph->path = strdup(dir);
				orph->name = strdup(de->d_name);
				orph->size = (int64) attrib.st_size;
				orph->mod_time = segment_time;
				orph->relfilenode = relfilenode;
				orph->reloid = oidrel;
				*flist = lappend(*flist, orph);
				/* search for _init and _fsm */
				if(strstr(de->d_name, ".") == NULL)
					pgorph_add_suffix(flist, orph);
			}
		/* 
		 * unless is this a temp table?
		 * temp table format on disk is: t%d_%u
		 * so that we check it starts with a t
		 * and then check the format with a regex
		 */
		} else if (de->d_name[0] == 't') {
			int i;
			pg_wchar   *wstr;
			int        wlen;
			pg_wchar   *regwstr;
			int        regwlen;
			int     r;
			char *regex = "^t[0-9]*_[0-9]";
			int  regcomp_result;
			char            errMsg[100];
			regex_t* preg = palloc(sizeof(regex_t));
			char	   *t;
			char       *tokptr = NULL;
			char *temprel;

			regwstr = palloc((strlen(regex) + 1) * sizeof(pg_wchar));
			regwlen = pg_mb2wchar_with_len(regex, regwstr, strlen(regex));

			regcomp_result = pg_regcomp(preg,
							regwstr,
							regwlen,
							REG_ADVANCED | REG_NOSUB,
							DEFAULT_COLLATION_OID);

			pfree(regwstr);

			if (regcomp_result == REG_OKAY) {
				wstr = palloc((strlen(de->d_name) + 1) * sizeof(pg_wchar));
				wlen = pg_mb2wchar_with_len(de->d_name, wstr, strlen(de->d_name));
				r = pg_regexec(preg, wstr, wlen, 0, NULL, 0, NULL, 0);
				if (r != REG_NOMATCH) {
					temprel = pstrdup(de->d_name);
					for (i = 0, t = strtok_r(temprel, "_", &tokptr); t != NULL; i++, t = strtok_r(NULL, "_", &tokptr))
					{
						if (i == 1) {
							orph = palloc(sizeof(*orph));
							relfilename = strdup(t);
							relfilenode = (Oid) strtoul(relfilename, &relfilename, 10);
							/* If RelidByRelfilenodeDirty does not return a valid oid
							 * then we consider this file as orphaned
							 */
							oidrel = RelidByRelfilenodeDirty(reltablespace, relfilenode);
							if (!OidIsValid(oidrel)) {
								orph->dbname = strdup(dbname);
								orph->path = strdup(dir);
								orph->name = strdup(de->d_name);
								orph->size = (int64) attrib.st_size;
								orph->mod_time = time_t_to_timestamptz(attrib.st_mtime);
								orph->relfilenode = relfilenode;
								orph->reloid = oidrel;
								*flist = lappend(*flist, orph);
								/* _fsm case has already been handled for temp */
								/* _init would have been too but _init on temp is not possible */
							}
						}	
					}
				}
				pfree(wstr);
			} else {
				/* regex didn't compile */
				pg_regerror(regcomp_result, preg, errMsg, sizeof(errMsg));
						ereport(ERROR,
							(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
							errmsg("invalid regular expression: %s", errMsg)));
			}
			pg_regfree(preg);
		}
	}
	FreeDir(dirdesc);
}

/*
 * function to move orphaned files
 * to the backup directory
 * the exact same directory tree is kept
 */
Datum
pg_move_orphaned(PG_FUNCTION_ARGS)
{
	Oid                     dbOid;
	ListCell   *cell;
	char *dir_to_create;
	int nb_moved;

	requireSuperuser();

    if (PG_ARGISNULL(0))
		limitts = GetCurrentTimestamp() - ((3600000 * 24) * (int64) 1000); // 1 Day
    else
		limitts = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval, TimestampGetDatum(GetCurrentTimestamp()), IntervalPGetDatum(PG_GETARG_INTERVAL_P(0))));

	dbOid = MyDatabaseId;
	pg_build_orphaned_list(dbOid, false);
	dir_to_create = psprintf("%s/%d", orphaned_backup_dir, dbOid);

	verify_dir_is_empty_or_create(dir_to_create, &made_directory, &found_existing_directory, true);
	nb_moved = 0;

	/* going through the list of orphaned files */
#if (PG_VERSION_NUM < 130000)
	for (cell = list_head(list_orphaned_relations); cell != NULL; cell = lnext(cell))
#else
	for (cell = list_head(list_orphaned_relations); cell != NULL; cell = lnext(list_orphaned_relations, cell))
#endif
	{
		char  orphaned_file[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
		char  orphaned_file_backup_dir[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
		char  orphaned_file_backup[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};

		OrphanedRelation  *orph = (OrphanedRelation *)lfirst(cell);

        snprintf(orphaned_file, sizeof(orphaned_file), "%s/%s", orph->path, orph->name);
        snprintf(orphaned_file_backup_dir, sizeof(orphaned_file_backup_dir), "%s/%s", dir_to_create, orph->path);

		/*
		 * Create the directory if it does not exist
		 */
		if (pg_orphaned_check_dir(orphaned_file_backup_dir) == 0)
			verify_dir_is_empty_or_create(orphaned_file_backup_dir, &made_directory, &found_existing_directory, false);

        snprintf(orphaned_file_backup, sizeof(orphaned_file_backup), "%s/%s", orphaned_file_backup_dir, orph->name);

        /* If old enough, move the orphaned file and check the success */
        if (orph->mod_time <= limitts)
        {
            if (rename(orphaned_file, orphaned_file_backup) != 0)
                ereport(ERROR,
                        (errcode_for_file_access(),
                        errmsg("could not rename \"%s\" to \"%s\": %m",
                        orphaned_file, orphaned_file_backup)));
            else
                nb_moved++;
        }
	}
	PG_RETURN_INT32(nb_moved);
}

/*
 * function to remove the orphaned files
 * we basically remove the whole backup directory
 * for this database
 */
Datum
pg_remove_moved_orphaned(PG_FUNCTION_ARGS)
{

	Oid                     dbOid;
	char *dir_to_remove;

	requireSuperuser();

	dbOid = MyDatabaseId;

	dir_to_remove = psprintf("%s/%d", orphaned_backup_dir, dbOid);

	if (!rmtree(dir_to_remove, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", dir_to_remove)));

	/* Now remove the top directory if empty */
	dir_to_remove = psprintf("%s", orphaned_backup_dir);

	if (is_directory_empty(dir_to_remove) && !rmtree(dir_to_remove, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", dir_to_remove)));

	PG_RETURN_VOID();
}

/*
 * function to move back the backed up
 * orphaned files to their original location
 * we ensure that the files to restore are still
 * orphaned ones
 */
Datum
pg_move_back_orphaned(PG_FUNCTION_ARGS)
{

	Oid                     dbOid;
	ListCell   *cell;
	int nb_moved;

	requireSuperuser();

	dbOid = MyDatabaseId;
	nb_moved = 0;

	/*
	 * Exists but empty
	 */
	if (pg_orphaned_check_dir(orphaned_backup_dir) != 4)
		PG_RETURN_INT32(nb_moved);

	/* building the list of orphaned files
	 * from the backup location: so the second arg is set to true
	 */
	pg_build_orphaned_list(dbOid, true);

	/* going through the list of orphaned files */
#if (PG_VERSION_NUM < 130000)
	for (cell = list_head(list_orphaned_relations); cell != NULL; cell = lnext(cell))
#else
	for (cell = list_head(list_orphaned_relations); cell != NULL; cell = lnext(list_orphaned_relations, cell))
#endif
	{
		char  orphaned_file_backup[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
		char  *orphaned_file_restore;
		char  *orphaned_file_restore_dup;

		OrphanedRelation  *orph = (OrphanedRelation *)lfirst(cell);

		snprintf(orphaned_file_backup, sizeof(orphaned_file_backup), "%s/%s", orph->path, orph->name);

		/* remove first 2 directories used to locate the backup */	
		orphaned_file_restore_dup = strdup(orphaned_file_backup);
		orphaned_file_restore = strchr(orphaned_file_restore_dup, '/');

		orphaned_file_restore_dup = orphaned_file_restore + 1;
		orphaned_file_restore = strchr(orphaned_file_restore_dup, '/');

		/* move the orphaned files back to their original location */
		if (rename(orphaned_file_backup, orphaned_file_restore + 1) != 0)
			ereport(ERROR,
				(errcode_for_file_access(),
					errmsg("could not rename \"%s\" to \"%s\": %m",
						orphaned_file_backup, orphaned_file_restore+1)));
		else
			nb_moved++;
	}
	PG_RETURN_INT32(nb_moved);
}

/*
 * Verify that the given directory exists and is empty. If it does not
 * exist, it is created. If it exists but is not empty, an error will
 * be given and the process ended.
 * mainly a copy/paste from the verify_dir_is_empty_or_create() in pg_basebackup.c
 */
static void
verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found, bool display_hint)
{
	switch (pg_orphaned_check_dir(dirname))
	{
		case 0:

			/*
			 * Does not exist, so create
			 */
			if (pg_orphaned_mkdir_p(dirname, pg_dir_create_mode) == -1)
			{
				ereport(ERROR,
					(errcode_for_file_access(),
					errmsg("could not create directory \"%s\": %m", dirname)));
				exit(1);
			}
			if (created)
				*created = true;
			return;
		case 1:

			/*
			 * Exists, empty
			 */
			if (found)
				*found = true;
			return;
		case 2:
		case 3:
		case 4:

			/*
			 * Exists, not empty
			 */
			if (!display_hint)
				ereport(ERROR,
					(errcode_for_file_access(),
					errmsg("directory \"%s\" exists but is not empty", dirname)));
			else
				ereport(ERROR,
					(errcode_for_file_access(),
					errmsg("directory \"%s\" exists but is not empty", dirname),
					errhint(" please check no files exist with pg_list_orphaned_moved(), move them back (if any) with pg_move_back_orphaned() and then clean \"%s\" up with pg_remove_moved_orphaned()", dirname)));
			exit(1);
		case -1:

			/*
			 * Access problem
			 */
			ereport(ERROR,
				(errcode_for_file_access(),
				errmsg("could not access directory \"%s\": %m", dirname)));
			exit(1);
	}
}

/*
 * We don't want non superuser to execute the functions
 */
static void
requireSuperuser(void)
{
	if (!superuser())
		ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			errmsg("only superuser can execute pg_orphaned functions")));
}

/*
 * If exists add _init, _fsm if exists
 * to the list of orphaned files
 */
void
pgorph_add_suffix(List **flist, OrphanedRelation *orph)
{
	struct stat st;
	char  orphaned_init_fsm[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY) + 10 + 6] = {0};
	char  orphaned_name[10 + 6] = {0};
	char add_suffix[NUMBER_SUFFIXES][MAX_SUFFIX_SIZE] = { "init", "fsm" };
	int i;

	for (i = 0; i < NUMBER_SUFFIXES; i++)
	{
		snprintf(orphaned_init_fsm, sizeof(orphaned_init_fsm), "%s/%s_%s", orph->path, orph->name, add_suffix[i]);
		/* Does the corresponding file exist? */
		if (lstat(orphaned_init_fsm, &st) < 0)
		{
			if (errno != ENOENT)
				elog(ERROR, "could not stat file \"%s\": %m", orphaned_init_fsm);
		}
		else
		/* file exists let's add it to the orphaned list */
		{
			OrphanedRelation *orph_suffix;
			orph_suffix = palloc(sizeof(*orph_suffix));

			/* most of the attributes are the same so just copy */
			memcpy(orph_suffix, orph, sizeof(OrphanedRelation));

			/* update specific attributes */
			snprintf(orphaned_name, sizeof(orphaned_name), "%s_%s", orph_suffix->name, add_suffix[i]);
			orph_suffix->name = strdup(orphaned_name);
			orph_suffix->size = (int64) st.st_size;
			orph_suffix->mod_time = time_t_to_timestamptz(st.st_mtime);

			*flist = lappend(*flist, orph_suffix);
		}
	}
}

/*
 * Map a relation's (tablespace, filenode) to a relation's oid and cache the
 * result.
 *
 * This is the same as the existing RelidByRelfilenode in relfilenodemap.c but
 * it is done by using a DirtySnapshot as we want to see relation being created.
 *
 * Returns InvalidOid if no relation matching the criteria could be found.
 */
Oid
RelidByRelfilenodeDirty(Oid reltablespace, Oid relfilenode)
{
	RelfilenodeMapKeyDirty key;
	RelfilenodeMapEntryDirty *entry;
	bool            found;
	SysScanDesc scandesc;
	Relation        relation;
	HeapTuple       ntp;
	ScanKeyData skey[2];
	Oid                     relid;
	SnapshotData DirtySnapshot;

	InitDirtySnapshot(DirtySnapshot);
	if (RelfilenodeMapHashDirty == NULL)
		InitializeRelfilenodeMapDirty();

	/* pg_class will show 0 when the value is actually MyDatabaseTableSpace */
	if (reltablespace == MyDatabaseTableSpace)
		reltablespace = 0;

	MemSet(&key, 0, sizeof(key));
	key.reltablespace = reltablespace;
	key.relfilenode = relfilenode;

	/*
	 * Check cache and return entry if one is found. Even if no target
	 * relation can be found later on we store the negative match and return a
	 * InvalidOid from cache. That's not really necessary for performance
	 * since querying invalid values isn't supposed to be a frequent thing,
	 * but it's basically free.
	 */
	entry = hash_search(RelfilenodeMapHashDirty, (void *) &key, HASH_FIND, &found);

	if (found)
		return entry->relid;

	/* ok, no previous cache entry, do it the hard way */

	/* initialize empty/negative cache entry before doing the actual lookups */
	relid = InvalidOid;

	if (reltablespace == GLOBALTABLESPACE_OID)
	{
		/*
		 * Ok, shared table, check relmapper.
		 */
#if PG_VERSION_NUM >= 160000
		relid = RelationMapFilenumberToOid(relfilenode, true);
#else
		relid = RelationMapFilenodeToOid(relfilenode, true);
#endif
	}
	else
	{
		/*
		 * Not a shared table, could either be a plain relation or a
		 * non-shared, nailed one, like e.g. pg_class.
		 */
		/* check for plain relations by looking in pg_class */
#if PG_VERSION_NUM >= 120000
		relation = table_open(RelationRelationId, AccessShareLock);
#else
		relation = heap_open(RelationRelationId, AccessShareLock);
#endif
		/* copy scankey to local copy, it will be modified during the scan */
		memcpy(skey, relfilenode_skey_dirty, sizeof(skey));

		/* set scan arguments */
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
#if PG_VERSION_NUM >= 120000
			Form_pg_class classform = (Form_pg_class) GETSTRUCT(ntp);
			found = true;

			Assert(classform->reltablespace == reltablespace);
			Assert(classform->relfilenode == relfilenode);
			relid = classform->oid;
#else
			found = true;
			relid = HeapTupleGetOid(ntp);
#endif
		}

		systable_endscan(scandesc);
#if PG_VERSION_NUM >= 120000
		table_close(relation, AccessShareLock);
#else
		heap_close(relation, AccessShareLock);
#endif
		/* check for tables that are mapped but not shared */
		if (!found)
#if PG_VERSION_NUM >= 160000
			relid = RelationMapFilenumberToOid(relfilenode, false);
#else
			relid = RelationMapFilenodeToOid(relfilenode, false);
#endif
	}

	/*
	 * Only enter entry into cache now, our opening of pg_class could have
	 * caused cache invalidations to be executed which would have deleted a
	 * new entry if we had entered it above.
	 */
	entry = hash_search(RelfilenodeMapHashDirty, (void *) &key, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "corrupted hashtable");
	entry->relid = relid;

	return relid;
}

/*
 *  Flush mapping entries when pg_class is updated in a relevant fashion.
 *  Same as RelfilenodeMapInvalidateCallback in relfilenodemap.c
 */
static void
RelfilenodeMapInvalidateCallbackDirty(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	RelfilenodeMapEntryDirty *entry;

	/* callback only gets registered after creating the hash */
	Assert(RelfilenodeMapHashDirty != NULL);

	hash_seq_init(&status, RelfilenodeMapHashDirty);
	while ((entry = (RelfilenodeMapEntryDirty *) hash_seq_search(&status)) != NULL)
	{
		/*
		 * If relid is InvalidOid, signalling a complete reset, we must remove
		 * all entries, otherwise just remove the specific relation's entry.
		 * Always remove negative cache entries.
		 */
		if (relid == InvalidOid ||      /* complete reset */
			entry->relid == InvalidOid ||   /* negative cache entry */
			entry->relid == relid)  /* individual flushed relation */
		{
			if (hash_search(RelfilenodeMapHashDirty,
							(void *) &entry->key,
							HASH_REMOVE,
							NULL) == NULL)
						elog(ERROR, "hash table corrupted");
		}
	}
}

/*
 * Initialize cache, either on first use or after a reset.
 * Same as InitializeRelfilenodeMap in relfilenodemap.c
 */
static void
InitializeRelfilenodeMapDirty(void)
{
	HASHCTL         ctl;
	int                     i;

	/* Make sure we've initialized CacheMemoryContext. */
	if (CacheMemoryContext == NULL)
		CreateCacheMemoryContext();

	/* build skey */
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

	/* Initialize the hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(RelfilenodeMapKeyDirty);
	ctl.entrysize = sizeof(RelfilenodeMapEntryDirty);
	ctl.hcxt = CacheMemoryContext;

	/*
	 * Only create the RelfilenodeMapHashDirty now, so we don't end up partially
	 * initialized when fmgr_info_cxt() above ERRORs out with an out of memory
	 * error.
	 * Note that the hash table is not created in shared memory but in
	 * private memory.
	 */
	RelfilenodeMapHashDirty =
		hash_create("RelfilenodeMap cache", 64, &ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Watch for invalidation events. */
	CacheRegisterRelcacheCallback(RelfilenodeMapInvalidateCallbackDirty,
									(Datum) 0);
}

static bool
is_directory_empty(const char *path)
{
    DIR        *dir;
    struct dirent *de;
    bool        is_empty = true;

    dir = AllocateDir(path);
    if (dir == NULL)
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open directory \"%s\": %m", path)));

    while ((de = ReadDir(dir, path)) != NULL)
    {
        /* Skip "." and ".." */
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0)
            continue;

        /* Found a real entry - directory is not empty */
        is_empty = false;
        break;
    }

    FreeDir(dir);

    return is_empty;
}
