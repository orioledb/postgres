/*-------------------------------------------------------------------------
 *
 * treeb_indexam.h
 *	  EnterpriseDB treeb index access method declarations.
 *
 * Copyright (c) 2022, EnterpriseDB Inc.
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * treeb/access/treeb_indexam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TREEB_INDEXAM_H
#define TREEB_INDEXAM_H

#include <math.h>
#include "access/amapi.h"

/*
 * treebutils.c defines a structure and a pointer for shared memory
 * initially set to NULL.  We declare it here, because we need to test whether
 * the pointer is NULL from locations outside that file.
 */
typedef struct TreebVacInfo TreebVacInfo;
extern TreebVacInfo *treebvacinfo;

/* treebam interface */
extern Datum treeb_indexam_handler(PG_FUNCTION_ARGS);
extern Datum get_raw_page_treeb(PG_FUNCTION_ARGS);

extern const IndexAmRoutine *GetTreebamIndexAmRoutine(void);
extern const Oid GetTreebOid(void);

#endif							/* TREEB_INDEXAM_H */
