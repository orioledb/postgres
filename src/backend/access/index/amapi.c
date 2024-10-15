/*-------------------------------------------------------------------------
 *
 * amapi.c
 *	  Support routines for API for Postgres index access methods.
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/amapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

IndexAMRoutineHookType IndexAMRoutineHook = NULL;

IndexAmRoutine *
GetIndexAmRoutineWithTableAM(Oid tamoid, Oid amhandler)
{
	Datum		datum;
	IndexAmRoutine *routine;

	if (IndexAMRoutineHook != NULL)
	{
		routine = IndexAMRoutineHook(tamoid, amhandler);
		if (routine)
			return routine;
	}

	datum = OidFunctionCall0(amhandler);
	routine = (IndexAmRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, IndexAmRoutine))
		elog(ERROR, "index access method handler function %u did not return an IndexAmRoutine struct",
			 amhandler);

	return routine;
}

/*
 * GetIndexAmRoutine - call the specified access method handler routine to get
 * its IndexAmRoutine struct, which will be palloc'd in the caller's context.
 *
 * Note that if the amhandler function is built-in, this will not involve
 * any catalog access.  It's therefore safe to use this while bootstrapping
 * indexes for the system catalogs.  relcache.c relies on that.
 */
IndexAmRoutine *
GetIndexAmRoutine(Oid amhandler)
{
	return GetIndexAmRoutineExtended(InvalidOid, amhandler);
}

IndexAmRoutine *
GetIndexAmRoutineExtended(Oid indoid, Oid amhandler)
{
	HeapTuple	ht_idx;
	HeapTuple	ht_tblrel;
	Form_pg_index idxrec;
	Form_pg_class tblrelrec;
	Oid			indrelid;
	Oid			tamoid;

	if (!OidIsValid((indoid)) || indoid < FirstNormalObjectId)
		return GetIndexAmRoutineWithTableAM(HEAP_TABLE_AM_OID, amhandler);

	ht_idx = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indoid));
	if (!HeapTupleIsValid(ht_idx))
		elog(ERROR, "cache lookup failed for index %u", indoid);
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);
	Assert(indoid == idxrec->indexrelid);
	indrelid = idxrec->indrelid;

	ht_tblrel = SearchSysCache1(RELOID, ObjectIdGetDatum(indrelid));
	if (!HeapTupleIsValid(ht_tblrel))
		elog(ERROR, "cache lookup failed for relation %u", indrelid);
	tblrelrec = (Form_pg_class) GETSTRUCT(ht_tblrel);
	tamoid = tblrelrec->relam;

	ReleaseSysCache(ht_tblrel);
	ReleaseSysCache(ht_idx);

	return GetIndexAmRoutineWithTableAM(tamoid, amhandler);
}

/*
 * GetIndexAmRoutineByAmId - look up the handler of the index access method
 * with the given OID, and get its IndexAmRoutine struct.
 *
 * If the given OID isn't a valid index access method, returns NULL if
 * noerror is true, else throws error.
 */
IndexAmRoutine *
GetIndexAmRoutineByAmId(Oid indoid, Oid amoid, bool noerror)
{
	HeapTuple	tuple;
	Form_pg_am	amform;
	regproc		amhandler;

	/* Get handler function OID for the access method */
	tuple = SearchSysCache1(AMOID, ObjectIdGetDatum(amoid));
	if (!HeapTupleIsValid(tuple))
	{
		if (noerror)
			return NULL;
		elog(ERROR, "cache lookup failed for access method %u",
			 amoid);
	}
	amform = (Form_pg_am) GETSTRUCT(tuple);

	/* Check if it's an index access method as opposed to some other AM */
	if (amform->amtype != AMTYPE_INDEX)
	{
		if (noerror)
		{
			ReleaseSysCache(tuple);
			return NULL;
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access method \"%s\" is not of type %s",
						NameStr(amform->amname), "INDEX")));
	}

	amhandler = amform->amhandler;

	/* Complain if handler OID is invalid */
	if (!RegProcedureIsValid(amhandler))
	{
		if (noerror)
		{
			ReleaseSysCache(tuple);
			return NULL;
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index access method \"%s\" does not have a handler",
						NameStr(amform->amname))));
	}

	ReleaseSysCache(tuple);

	/* And finally, call the handler function to get the API struct. */
	return GetIndexAmRoutineExtended(indoid, amhandler);
}


/*
 * Ask appropriate access method to validate the specified opclass.
 */
Datum
amvalidate(PG_FUNCTION_ARGS)
{
	Oid			opclassoid = PG_GETARG_OID(0);
	bool		result;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			amoid;
	IndexAmRoutine *amroutine;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	amoid = classform->opcmethod;

	ReleaseSysCache(classtup);

	amroutine = GetIndexAmRoutineByAmId(InvalidOid, amoid, false);

	if (amroutine->amvalidate == NULL)
		elog(ERROR, "function amvalidate is not defined for index access method %u",
			 amoid);

	result = amroutine->amvalidate(opclassoid);

	pfree(amroutine);

	PG_RETURN_BOOL(result);
}
