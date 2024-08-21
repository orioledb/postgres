/*-------------------------------------------------------------------------
 *
 * treeb_indexam.c
 *	  EnterpriseDB treeb definitions.
 *
 * Copyright (c) 2024, EnterpriseDB Inc.
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * treeb/access/treeb_indexam.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/treeb_indexam.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "utils/syscache.h"

static Oid TREEB_AM_OID = InvalidOid;

const Oid
GetTreebOid(void)
{
	if (!OidIsValid(TREEB_AM_OID))
	{
		HeapTuple		tuple;
		Form_pg_am		accessMethodForm;

		tuple = SearchSysCache1(AMNAME, PointerGetDatum("treeb"));
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("access method \"%s\" does not exist",
							"treeb")));
		accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
		TREEB_AM_OID = accessMethodForm->oid;
		ReleaseSysCache(tuple);
	}

	return TREEB_AM_OID;
}
