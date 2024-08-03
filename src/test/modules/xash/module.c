/*-------------------------------------------------------------------------
 *
 * module.c
 *		EnterpriseDB xash plugin.
 *
 * Portions Copyright (c) 2022, EnterpriseDB Corporation. All Rights Reserved.
 *
 * IDENTIFICATION
 *	  xash/module.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"

PG_MODULE_MAGIC;

/*---- GUC variables ----*/


/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

/*---- Function definitions ----*/

/*
 * Module load callback
 */
void
_PG_init(void)
{
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
}
