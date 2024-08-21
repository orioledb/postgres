/*-------------------------------------------------------------------------
 *
 * module.c
 *		EnterpriseDB treeb plugin.
 *
 * Portions Copyright (c) 2022, EnterpriseDB Corporation. All Rights Reserved.
 *
 * IDENTIFICATION
 *	  treeb/module.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/treeb.h"
#include "access/treeb_indexam.h"
#include "access/treebxlog.h"
#include "access/xlog_internal.h"
#include "catalog/objectaccess.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

PG_MODULE_MAGIC;

/*---- GUC variables ----*/

/*---- Private function declarations ----*/
static void RegisterTreebHooks(void);
static void treeb_shmem_request(void);
static void treeb_shmem_startup(void);

/*
 * Saved hook entries (if stacked)
 */
static object_access_hook_type next_object_access_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static RmgrData treeb_rmgr = {
	.rm_name = "RM_TREEB",
	.rm_redo = treeb_redo,
	.rm_desc = treeb_desc,
	.rm_identify = treeb_identify,
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_mask = treeb_mask,
	.rm_decode = NULL
};

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

/*---- Function definitions ----*/

/*
 * treeb_object_access
 */
static void
treeb_object_access(
			ObjectAccessType access,
			Oid classId,
			Oid objectId,
			int subId,
			void *arg)
{
	if (next_object_access_hook)
		(*next_object_access_hook) (access, classId, objectId, subId, arg);

	switch (access)
	{
		case OAT_POST_CREATE:
			break;
		case OAT_DROP:
			break;
		case OAT_POST_ALTER:
			break;
		case OAT_NAMESPACE_SEARCH:
			break;
		case OAT_FUNCTION_EXECUTE:
			break;
		case OAT_TRUNCATE:
			break;
		/* Intentionally no default */
	}
}

/*
 * treebshmem_request hook: request additional shared resources.  We'll
 * allocate or attach to the shared resources in treeb_shmem_startup().
 */
static void
treeb_shmem_request(void)
{
	Size	size = 0;

	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	size += TreebShmemSize();

	RequestAddinShmemSpace(size);
}

/*
 * treeb_shmem_shutdown hook.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
treeb_shmem_shutdown(int code, Datum arg)
{
	/* Don't try to shutdown during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (treebvacinfo == NULL)
		return;

	/*
	 * TODO: whatever shutdown logic treeb requires.
	 *
	 * Note that not all AMs have any reason to want a shutdown hook.
	 * This function can be removed, along with its registration in
	 * treeb_shmem_startup, if there is nothing to be done.
	 */
}

/*
 * treebshmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 * Also create and load the query-texts file, which is expected to exist
 * (even if empty) while the module is enabled.
 */
static void
treeb_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	TreebShmemInit();
	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook for treeb to use, if needed.  Note that this can be removed
	 * in AMs that do not require a shutdown hook.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(treeb_shmem_shutdown, (Datum) 0);
}

/*
 * Module load callback
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		Assert(false);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("must be loaded as a shared preload library")));
	}

	/*
	 * Replace RM_EXPERIMENTAL_ID with a unique ID before release.
	 *
	 * See https://wiki.postgresql.org/wiki/CustomWALResourceManagers
	 */
	RegisterCustomRmgr(
		RM_EXPERIMENTAL_ID,
		&treeb_rmgr
	);

	/*
	 * Set up Treeb specific hooks, if any.
	 */
	RegisterTreebHooks();

	/* Object access hook */
	next_object_access_hook = object_access_hook;
	object_access_hook = treeb_object_access;

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = treeb_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = treeb_shmem_startup;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
}

/*
 * Register all hooks for this index AM
 */
void
RegisterTreebHooks(void)
{
}
