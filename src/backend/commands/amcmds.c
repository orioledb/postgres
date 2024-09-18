/*-------------------------------------------------------------------------
 *
 * amcmds.c
 *	  Routines for SQL commands that manipulate access methods.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/amcmds.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amimpl.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static Oid	lookup_am_handler_func(List *handler_name, char amtype);
static Oid	lookup_amimpl_handler_func(List *handler_name);
static const char *get_am_type_string(char amtype);

/*
 * CreateAccessMethodImplementation
 *		Registers a new access method.
 */
ObjectAddress
CreateAccessMethodImplementation(CreateAmImplStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	ObjectAddress referenced;
	Oid 		amoid;
	Oid			implam;
	Oid			imploid;
	Oid			implhandler;
	bool		nulls[Natts_pg_amimpl];
	Datum		values[Natts_pg_amimpl];
	HeapTuple	tup;

	rel = table_open(AccessMethodImplementationId, RowExclusiveLock);

	/* Must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create access method implementation \"%s\"",
						stmt->implname),
				 errhint("Must be superuser to create an access method implementation.")));

	/* Check if name is used */
	imploid = GetSysCacheOid1(AMIMPLNAME, Anum_pg_amimpl_oid,
							CStringGetDatum(stmt->implname));
	if (OidIsValid(imploid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("access method implementation \"%s\" already exists",
						stmt->implname)));
	}

	/* Check if access method exists, and that it is an index AM */
	amoid = get_index_am_oid(stmt->amname, false);

	/*
	 * Resolve the AM whose opclasses are consulted at index build time.  When
	 * the user did not specify USING ... OPCLASSES, default to the AM the
	 * implementation is registered for.
	 */
	if (stmt->opcam_name != NULL)
		implam = get_index_am_oid(stmt->opcam_name, false);
	else
		implam = amoid;

	/*
	 * Get the handler function oid, verifying the AM type while at it.
	 */
	implhandler = lookup_amimpl_handler_func(stmt->handler_name);

	/*
	 * Insert tuple into pg_am.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	imploid = GetNewOidWithIndex(rel, AmImplImploidIndexId, Anum_pg_amimpl_oid);
	values[Anum_pg_amimpl_oid - 1] = ObjectIdGetDatum(imploid);
	values[Anum_pg_amimpl_implname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->implname));
	values[Anum_pg_amimpl_amoid - 1] = ObjectIdGetDatum(amoid);
	values[Anum_pg_amimpl_implam - 1] = ObjectIdGetDatum(implam);
	values[Anum_pg_amimpl_implhandler - 1] = ObjectIdGetDatum(implhandler);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	myself.classId = AccessMethodImplementationId;
	myself.objectId = imploid;
	myself.objectSubId = 0;

	/* Record dependency on handler function */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = implhandler;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Record dependency on the access method */
	referenced.classId = AccessMethodRelationId;
	referenced.objectId = amoid;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Record dependency on the opclass AM, if distinct from amoid */
	if (implam != amoid)
	{
		referenced.classId = AccessMethodRelationId;
		referenced.objectId = implam;
		referenced.objectSubId = 0;

		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	recordDependencyOnCurrentExtension(&myself, false);

	InvokeObjectPostCreateHook(AccessMethodImplementationId, imploid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * CreateAccessMethod
 *		Registers a new access method.
 */
ObjectAddress
CreateAccessMethod(CreateAmStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	ObjectAddress referenced;
	Oid			amoid;
	Oid			amhandler;
	bool		nulls[Natts_pg_am];
	Datum		values[Natts_pg_am];
	HeapTuple	tup;

	rel = table_open(AccessMethodRelationId, RowExclusiveLock);

	/* Must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create access method \"%s\"",
						stmt->amname),
				 errhint("Must be superuser to create an access method.")));

	/* Check if name is used */
	amoid = GetSysCacheOid1(AMNAME, Anum_pg_am_oid,
							CStringGetDatum(stmt->amname));
	if (OidIsValid(amoid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("access method \"%s\" already exists",
						stmt->amname)));
	}

	/*
	 * Get the handler function oid, verifying the AM type while at it.
	 */
	amhandler = lookup_am_handler_func(stmt->handler_name, stmt->amtype);

	/*
	 * Insert tuple into pg_am.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	amoid = GetNewOidWithIndex(rel, AmOidIndexId, Anum_pg_am_oid);
	values[Anum_pg_am_oid - 1] = ObjectIdGetDatum(amoid);
	values[Anum_pg_am_amname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->amname));
	values[Anum_pg_am_amhandler - 1] = ObjectIdGetDatum(amhandler);
	values[Anum_pg_am_amtype - 1] = CharGetDatum(stmt->amtype);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	myself.classId = AccessMethodRelationId;
	myself.objectId = amoid;
	myself.objectSubId = 0;

	/* Record dependency on handler function */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = amhandler;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	recordDependencyOnCurrentExtension(&myself, false);

	InvokeObjectPostCreateHook(AccessMethodRelationId, amoid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * get_amimpl_oid - given an access method implementation name, look up its OID.
 */
Oid
get_amimpl_oid(const char *implname, bool missing_ok)
{
	HeapTuple	tup;
	Oid			oid = InvalidOid;

	tup = SearchSysCache1(AMIMPLNAME, CStringGetDatum(implname));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_amimpl amimplform = (Form_pg_amimpl) GETSTRUCT(tup);

		oid = amimplform->oid;
		ReleaseSysCache(tup);
	}

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method implementation \"%s\" does not exist", implname)));
	return oid;
}

/*
 * get_amimpl_amoid - given an access method implementation name, look up its access method OID.
 */
Oid
get_amimpl_amoid(const char *implname, bool missing_ok)
{
	HeapTuple	tup;
	Oid			oid = InvalidOid;

	tup = SearchSysCache1(AMIMPLNAME, CStringGetDatum(implname));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_amimpl amimplform = (Form_pg_amimpl) GETSTRUCT(tup);

		oid = amimplform->amoid;
		ReleaseSysCache(tup);
	}

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method for implementation \"%s\" does not exist", implname)));
	return oid;
}

/*
 * get_am_type_oid
 *		Worker for various get_am_*_oid variants
 *
 * If missing_ok is false, throw an error if access method not found.  If
 * true, just return InvalidOid.
 *
 * If amtype is not '\0', an error is raised if the AM found is not of the
 * given type.
 */
static Oid
get_am_type_oid(const char *amname, char amtype, bool missing_ok)
{
	HeapTuple	tup;
	Oid			oid = InvalidOid;

	tup = SearchSysCache1(AMNAME, CStringGetDatum(amname));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_am	amform = (Form_pg_am) GETSTRUCT(tup);

		if (amtype != '\0' &&
			amform->amtype != amtype)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("access method \"%s\" is not of type %s",
							NameStr(amform->amname),
							get_am_type_string(amtype))));

		oid = amform->oid;
		ReleaseSysCache(tup);
	}

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist", amname)));
	return oid;
}

/*
 * get_index_am_oid - given an access method name, look up its OID
 *		and verify it corresponds to an index AM.
 */
Oid
get_index_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, AMTYPE_INDEX, missing_ok);
}

/*
 * get_table_am_oid - given an access method name, look up its OID
 *		and verify it corresponds to a table AM.
 */
Oid
get_table_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, AMTYPE_TABLE, missing_ok);
}

/*
 * get_am_oid - given an access method name, look up its OID.
 *		The type is not checked.
 */
Oid
get_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, '\0', missing_ok);
}

/*
 * get_am_name - given an access method OID, look up its name.
 */
char *
get_amimpl_implname(Oid imploid)
{
	HeapTuple	tup;
	char	   *result = NULL;

	tup = SearchSysCache1(AMIMPLOID, ObjectIdGetDatum(imploid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_amimpl	amimplform = (Form_pg_amimpl) GETSTRUCT(tup);

		result = pstrdup(NameStr(amimplform->implname));
		ReleaseSysCache(tup);
	}
	return result;
}

/*
 * get_am_name - given an access method OID, look up its name.
 */
char *
get_am_name(Oid amOid)
{
	HeapTuple	tup;
	char	   *result = NULL;

	tup = SearchSysCache1(AMOID, ObjectIdGetDatum(amOid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_am	amform = (Form_pg_am) GETSTRUCT(tup);

		result = pstrdup(NameStr(amform->amname));
		ReleaseSysCache(tup);
	}
	return result;
}

/*
 * Convert single-character access method type into string for error reporting.
 */
static const char *
get_am_type_string(char amtype)
{
	switch (amtype)
	{
		case AMTYPE_INDEX:
			return "INDEX";
		case AMTYPE_TABLE:
			return "TABLE";
		default:
			/* shouldn't happen */
			elog(ERROR, "invalid access method type '%c'", amtype);
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * Convert an implementation handler function name to an Oid.  Implementations
 * apply only to index AMs, so the handler must return index_am_handler.
 *
 * Returns a valid function Oid or throws an error.
 */
static Oid
lookup_amimpl_handler_func(List *handler_name)
{
	Oid			handlerOid;
	Oid			funcargtypes[1] = {INTERNALOID};

	if (handler_name == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("handler function is not specified")));

	/* handlers have one argument of type internal */
	handlerOid = LookupFuncName(handler_name, 1, funcargtypes, false);

	if (get_func_rettype(handlerOid) != INDEX_AM_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						get_func_name(handlerOid),
						"index_am_handler")));

	return handlerOid;
}

/*
 * Convert a handler function name to an Oid.  If the return type of the
 * function doesn't match the given AM type, an error is raised.
 *
 * This function either return valid function Oid or throw an error.
 */
static Oid
lookup_am_handler_func(List *handler_name, char amtype)
{
	Oid			handlerOid;
	Oid			funcargtypes[1] = {INTERNALOID};
	Oid			expectedType = InvalidOid;

	if (handler_name == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("handler function is not specified")));

	/* handlers have one argument of type internal */
	handlerOid = LookupFuncName(handler_name, 1, funcargtypes, false);

	/* check that handler has the correct return type */
	switch (amtype)
	{
		case AMTYPE_INDEX:
			expectedType = INDEX_AM_HANDLEROID;
			break;
		case AMTYPE_TABLE:
			expectedType = TABLE_AM_HANDLEROID;
			break;
		default:
			elog(ERROR, "unrecognized access method type \"%c\"", amtype);
	}

	if (get_func_rettype(handlerOid) != expectedType)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						get_func_name(handlerOid),
						format_type_extended(expectedType, -1, 0))));

	return handlerOid;
}
