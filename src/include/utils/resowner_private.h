/*-------------------------------------------------------------------------
 *
 * resowner_private.h
 *	  POSTGRES resource owner private definitions.
 *
 * See utils/resowner/README for more info.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/resowner_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESOWNER_PRIVATE_H
#define RESOWNER_PRIVATE_H

#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/lock.h"
#include "utils/catcache.h"
#include "utils/plancache.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"


extern void ResourceOwnerRememberCatCacheRef(ResourceOwner owner,
											 HeapTuple tuple);
extern void ResourceOwnerRememberCatCacheListRef(ResourceOwner owner,
												 CatCList *list);

#endif							/* RESOWNER_PRIVATE_H */
