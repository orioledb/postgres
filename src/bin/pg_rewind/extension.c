/*-------------------------------------------------------------------------
 *
 * extension.c
 *	  Functions for processing shared libraries loaded by pg_rewind.
 *
 * Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#ifndef WIN32
#include <dlfcn.h>

/*
 * On macOS, <dlfcn.h> insists on including <stdbool.h>.  If we're not
 * using stdbool, undef bool to undo the damage.
 */
#ifndef PG_USE_STDBOOL
#ifdef bool
#undef bool
#endif
#endif
#endif							/* !WIN32 */

#include <sys/stat.h>

#include "access/xlog_internal.h"
#include "pg_rewind.h"

/* signature for pg_rewind extension library rewind function */
typedef void (*PG_rewind_t) (const char *datadir_target, char *datadir_source,
							 char *connstr_source, XLogRecPtr startpoint,
							 int tliIndex, XLogRecPtr endpoint,
							 const char *restoreCommand, const char *argv0,
							 bool debug);

static bool
file_exists(const char *argv0, const char *name)
{
	struct stat st;

	Assert(name != NULL);

	if (stat(name, &st) == 0)
		return !S_ISDIR(st.st_mode);
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
	{
		const char *progname;

		progname = get_progname(argv0);
		pg_log_error("could not access file \"%s\": %m", name);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	return false;
}

static char *
expand_dynamic_library_name(const char *argv0, const char *name)
{
	char	   *full;
	char		my_exec_path[MAXPGPATH];
	char		pkglib_path[MAXPGPATH];

	Assert(name);

	if (find_my_exec(argv0, my_exec_path) < 0)
		pg_fatal("%s: could not locate my own executable path", argv0);
	get_pkglib_path(my_exec_path, pkglib_path);
	full = palloc(strlen(pkglib_path) + 1 + strlen(name) + 1);
	sprintf(full, "%s/%s", pkglib_path, name);
	if (file_exists(argv0, full))
		return full;
	pfree(full);

	full = palloc(strlen(pkglib_path) + 1 + strlen(name) + 1 +
				  strlen(DLSUFFIX) + 1);
	sprintf(full, "%s/%s%s", pkglib_path, name, DLSUFFIX);
	if (file_exists(argv0, full))
		return full;
	pfree(full);

	return pstrdup(name);
}

void
process_extensions(SimpleStringList *extensions, const char *datadir_target,
				   char *datadir_source, char *connstr_source,
				   XLogRecPtr startpoint, int tliIndex, XLogRecPtr endpoint,
				   const char *restoreCommand, const char *argv0,
				   bool debug)
{
	SimpleStringListCell *cell;

	if (extensions->head == NULL)
		return;					/* nothing to do */

	for (cell = extensions->head; cell; cell = cell->next)
	{
		char	   *filename = cell->val;
		char	   *fullname;
		void	   *lib_handle;
		PG_rewind_t PG_rewind;
		char	   *load_error;

		fullname = expand_dynamic_library_name(argv0, filename);

		lib_handle = dlopen(fullname, RTLD_NOW | RTLD_GLOBAL);
		if (lib_handle == NULL)
		{
			load_error = dlerror();
			pg_fatal("could not load library \"%s\": %s", fullname, load_error);
		}

		PG_rewind = dlsym(lib_handle, "_PG_rewind");

		if (PG_rewind == NULL)
			pg_fatal("could not find function \"_PG_rewind\" in \"%s\"",
					 fullname);
		pfree(fullname);

		if (showprogress)
			pg_log_info("performing rewind for '%s' extension", filename);
		PG_rewind(datadir_target, datadir_source, connstr_source, startpoint,
				  tliIndex, endpoint, restoreCommand, argv0, debug);

		pg_log_debug("loaded library \"%s\"", filename);
	}
}
