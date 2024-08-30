/*-------------------------------------------------------------------------
 *
 * xash.c
 *	  Implementation of an index AM by reference to Hash.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/test/modules/xash/access/xash.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/hash.h"
#include "commands/vacuum.h"
#include "utils/index_selfuncs.h"

/*
 * Xash handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
PG_FUNCTION_INFO_V1(xash_indexam_handler);
Datum
xash_indexam_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = HTMaxStrategyNumber;
	amroutine->amsupport = HASHNProcs;
	amroutine->amoptsprocnum = HASHOPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanhash = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = false;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false;
	amroutine->amtranslatestrategy = hashtranslatestrategy;
	amroutine->amtranslaterctype = hashtranslaterctype;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL;
	amroutine->amkeytype = INT4OID;

	amroutine->ambuild = hashbuild;
	amroutine->ambuildempty = hashbuildempty;
	amroutine->aminsert = hashinsert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = hashbulkdelete;
	amroutine->amvacuumcleanup = hashvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->ambegintscluster = NULL;
	amroutine->amcostestimate = hashcostestimate;
	amroutine->amgetrootheight = NULL;
	amroutine->amoptions = hashoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = hashvalidate;
	amroutine->amadjustmembers = hashadjustmembers;
	amroutine->ambeginscan = hashbeginscan;
	amroutine->amrescan = hashrescan;
	amroutine->amgettuple = hashgettuple;
	amroutine->amgetbitmap = hashgetbitmap;
	amroutine->amendscan = hashendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amgetrootheight = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}
