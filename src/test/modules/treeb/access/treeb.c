/*-------------------------------------------------------------------------
 *
 * treeb.c
 *	  Implementation of Lehman and Yao's treeb management algorithm for
 *	  Postgres.
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/treeb/treeb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/treeb.h"
#include "access/relscan.h"
#include "access/xloginsert.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "storage/bulk_write.h"
#include "storage/condition_variable.h"
#include "storage/indexfsm.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/fmgrprotos.h"
#include "utils/index_selfuncs.h"
#include "utils/memutils.h"
#include "access/treeb_indexam.h"
#include "parser/parsetree.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/probes.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"


/*
 * TREEBPARALLEL_NOT_INITIALIZED indicates that the scan has not started.
 *
 * TREEBPARALLEL_NEED_PRIMSCAN indicates that some process must now seize the
 * scan to advance it via another call to _treeb_first.
 *
 * TREEBPARALLEL_ADVANCING indicates that some process is advancing the scan to
 * a new page; others must wait.
 *
 * TREEBPARALLEL_IDLE indicates that no backend is currently advancing the scan
 * to a new page; some process can start doing that.
 *
 * TREEBPARALLEL_DONE indicates that the scan is complete (including error exit).
 */
typedef enum
{
	TREEBPARALLEL_NOT_INITIALIZED,
	TREEBPARALLEL_NEED_PRIMSCAN,
	TREEBPARALLEL_ADVANCING,
	TREEBPARALLEL_IDLE,
	TREEBPARALLEL_DONE,
} TREEBPS_State;

/*
 * TreebParallelScanDescData contains treeb specific shared information required
 * for parallel scan.
 */
typedef struct TreebParallelScanDescData
{
	BlockNumber treebps_scanPage;	/* latest or next page to be scanned */
	TREEBPS_State	treebps_pageStatus;	/* indicates whether next page is
									 * available for scan. see above for
									 * possible states of parallel scan. */
	slock_t		treebps_mutex;		/* protects above variables, btps_arrElems */
	ConditionVariable treebps_cv;	/* used to synchronize parallel scan */

	/*
	 * btps_arrElems is used when scans need to schedule another primitive
	 * index scan.  Holds TreebArrayKeyInfo.cur_elem offsets for scan keys.
	 */
	int			btps_arrElems[FLEXIBLE_ARRAY_MEMBER];
}			TreebParallelScanDescData;

typedef struct TreebParallelScanDescData *TreebParallelScanDesc;


static void treebvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						 IndexBulkDeleteCallback callback, void *callback_state,
						 TreebCycleId cycleid);
static void treebvacuumpage(TreebVacState *vstate, BlockNumber scanblkno);
static TreebVacuumPosting treebvacuumposting(TreebVacState *vstate,
										  IndexTuple posting,
										  OffsetNumber updatedoffset,
										  int *nremaining);


/*
 * Treeb handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
PG_FUNCTION_INFO_V1(treeb_indexam_handler);
Datum
treeb_indexam_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = TreebMaxStrategyNumber;
	amroutine->amsupport = TreebNProcs;
	amroutine->amoptsprocnum = TREEBOPTIONS_PROC;
	amroutine->amcanorder = true;
	amroutine->amcanhash = false;
	amroutine->amcancrosscompare = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = true;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = true;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = false;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = true;
	amroutine->amcanbuildparallel = true;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_COND_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = treebbuild;
	amroutine->ambuildempty = treebbuildempty;
	amroutine->aminsert = treebinsert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = treebbulkdelete;
	amroutine->amvacuumcleanup = treebvacuumcleanup;
	amroutine->amcanreturn = treebcanreturn;
	amroutine->amcostestimate = treebcostestimate;
	amroutine->amgetrootheight = treebgetrootheight;
	amroutine->amoptions = treeboptions;
	amroutine->amproperty = treebproperty;
	amroutine->ambuildphasename = treebbuildphasename;
	amroutine->amvalidate = treebvalidate;
	amroutine->amadjustmembers = treebadjustmembers;
	amroutine->ambeginscan = treebbeginscan;
	amroutine->amrescan = treebrescan;
	amroutine->amgettuple = treebgettuple;
	amroutine->amgetbitmap = treebgetbitmap;
	amroutine->amendscan = treebendscan;
	amroutine->ammarkpos = treebmarkpos;
	amroutine->amrestrpos = treebrestrpos;
	amroutine->amestimateparallelscan = treebestimateparallelscan;
	amroutine->ambegintscluster = tuplesort_begin_cluster_treeb;
	amroutine->aminitparallelscan = treebinitparallelscan;
	amroutine->amparallelrescan = treebparallelrescan;

	PG_RETURN_POINTER(amroutine);
}

/*
 *	treebbuildempty() -- build an empty treeb index in the initialization fork
 */
void
treebbuildempty(Relation index)
{
	bool		allequalimage = _treeb_allequalimage(index, false);
	BulkWriteState *bulkstate;
	BulkWriteBuffer metabuf;

	bulkstate = smgr_bulk_start_rel(index, INIT_FORKNUM);

	/* Construct metapage. */
	metabuf = smgr_bulk_get_buf(bulkstate);
	_treeb_initmetapage((Page) metabuf, P_NONE, 0, allequalimage);
	smgr_bulk_write(bulkstate, TREEB_METAPAGE, metabuf, true);

	smgr_bulk_finish(bulkstate);
}

/*
 *	treebinsert() -- insert an index tuple into a treeb.
 *
 *		Descend the tree recursively, find the appropriate location for our
 *		new tuple, and put it there.
 */
bool
treebinsert(Relation rel, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel,
		 IndexUniqueCheck checkUnique,
		 bool indexUnchanged,
		 IndexInfo *indexInfo)
{
	bool		result;
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
	itup->t_tid = *ht_ctid;

	result = _treeb_doinsert(rel, itup, checkUnique, indexUnchanged, heapRel);

	pfree(itup);

	return result;
}

/*
 *	treebgettuple() -- Get the next tuple in the scan.
 */
bool
treebgettuple(IndexScanDesc scan, ScanDirection dir)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	bool		res;

	/* treeb indexes are never lossy */
	scan->xs_recheck = false;

	/* Each loop iteration performs another primitive index scan */
	do
	{
		/*
		 * If we've already initialized this scan, we can just advance it in
		 * the appropriate direction.  If we haven't done so yet, we call
		 * _treeb_first() to get the first item in the scan.
		 */
		if (!TreebScanPosIsValid(so->currPos))
			res = _treeb_first(scan, dir);
		else
		{
			/*
			 * Check to see if we should kill the previously-fetched tuple.
			 */
			if (scan->kill_prior_tuple)
			{
				/*
				 * Yes, remember it for later. (We'll deal with all such
				 * tuples at once right before leaving the index page.)  The
				 * test for numKilled overrun is not just paranoia: if the
				 * caller reverses direction in the indexscan then the same
				 * item might get entered multiple times. It's not worth
				 * trying to optimize that, so we don't detect it, but instead
				 * just forget any excess entries.
				 */
				if (so->killedItems == NULL)
					so->killedItems = (int *)
						palloc(MaxTIDsPerTreebPage * sizeof(int));
				if (so->numKilled < MaxTIDsPerTreebPage)
					so->killedItems[so->numKilled++] = so->currPos.itemIndex;
			}

			/*
			 * Now continue the scan.
			 */
			res = _treeb_next(scan, dir);
		}

		/* If we have a tuple, return it ... */
		if (res)
			break;
		/* ... otherwise see if we need another primitive index scan */
	} while (so->numArrayKeys && _treeb_start_prim_scan(scan, dir));

	return res;
}

/*
 * treebgetbitmap() -- gets all matching tuples, and adds them to a bitmap
 */
int64
treebgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	int64		ntids = 0;
	ItemPointer heapTid;

	/* Each loop iteration performs another primitive index scan */
	do
	{
		/* Fetch the first page & tuple */
		if (_treeb_first(scan, ForwardScanDirection))
		{
			/* Save tuple ID, and continue scanning */
			heapTid = &scan->xs_heaptid;
			tbm_add_tuples(tbm, heapTid, 1, false);
			ntids++;

			for (;;)
			{
				/*
				 * Advance to next tuple within page.  This is the same as the
				 * easy case in _treeb_next().
				 */
				if (++so->currPos.itemIndex > so->currPos.lastItem)
				{
					/* let _treeb_next do the heavy lifting */
					if (!_treeb_next(scan, ForwardScanDirection))
						break;
				}

				/* Save tuple ID, and continue scanning */
				heapTid = &so->currPos.items[so->currPos.itemIndex].heapTid;
				tbm_add_tuples(tbm, heapTid, 1, false);
				ntids++;
			}
		}
		/* Now see if we need another primitive index scan */
	} while (so->numArrayKeys && _treeb_start_prim_scan(scan, ForwardScanDirection));

	return ntids;
}

/*
 *	treebbeginscan() -- start a scan on a treeb index
 */
IndexScanDesc
treebbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	TreebScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	/* get the scan */
	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (TreebScanOpaque) palloc(sizeof(TreebScanOpaqueData));
	TreebScanPosInvalidate(so->currPos);
	TreebScanPosInvalidate(so->markPos);
	if (scan->numberOfKeys > 0)
		so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
	else
		so->keyData = NULL;

	so->needPrimScan = false;
	so->scanBehind = false;
	so->arrayKeys = NULL;
	so->orderProcs = NULL;
	so->arrayContext = NULL;

	so->killedItems = NULL;		/* until needed */
	so->numKilled = 0;

	/*
	 * We don't know yet whether the scan will be index-only, so we do not
	 * allocate the tuple workspace arrays until treebrescan.  However, we set up
	 * scan->xs_itupdesc whether we'll need it or not, since that's so cheap.
	 */
	so->currTuples = so->markTuples = NULL;

	scan->xs_itupdesc = RelationGetDescr(rel);

	scan->opaque = so;

	return scan;
}

/*
 *	treebrescan() -- rescan an index relation
 */
void
treebrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (TreebScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_treeb_killitems(scan);
		TreebScanPosUnpinIfPinned(so->currPos);
		TreebScanPosInvalidate(so->currPos);
	}

	so->markItemIndex = -1;
	so->needPrimScan = false;
	so->scanBehind = false;
	TreebScanPosUnpinIfPinned(so->markPos);
	TreebScanPosInvalidate(so->markPos);

	/*
	 * Allocate tuple workspace arrays, if needed for an index-only scan and
	 * not already done in a previous rescan call.  To save on palloc
	 * overhead, both workspaces are allocated as one palloc block; only this
	 * function and treebendscan know that.
	 *
	 * NOTE: this data structure also makes it safe to return data from a
	 * "name" column, even though treeb name_ops uses an underlying storage
	 * datatype of cstring.  The risk there is that "name" is supposed to be
	 * padded to NAMEDATALEN, but the actual index tuple is probably shorter.
	 * However, since we only return data out of tuples sitting in the
	 * currTuples array, a fetch of NAMEDATALEN bytes can at worst pull some
	 * data out of the markTuples array --- running off the end of memory for
	 * a SIGSEGV is not possible.  Yeah, this is ugly as sin, but it beats
	 * adding special-case treatment for name_ops elsewhere.
	 */
	if (scan->xs_want_itup && so->currTuples == NULL)
	{
		so->currTuples = (char *) palloc(BLCKSZ * 2);
		so->markTuples = so->currTuples + BLCKSZ;
	}

	/*
	 * Reset the scan keys
	 */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	so->numberOfKeys = 0;		/* until _treeb_preprocess_keys sets it */
	so->numArrayKeys = 0;		/* ditto */
}

/*
 *	treebendscan() -- close down a scan
 */
void
treebendscan(IndexScanDesc scan)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (TreebScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_treeb_killitems(scan);
		TreebScanPosUnpinIfPinned(so->currPos);
	}

	so->markItemIndex = -1;
	TreebScanPosUnpinIfPinned(so->markPos);

	/* No need to invalidate positions, the RAM is about to be freed. */

	/* Release storage */
	if (so->keyData != NULL)
		pfree(so->keyData);
	/* so->arrayKeys and so->orderProcs are in arrayContext */
	if (so->arrayContext != NULL)
		MemoryContextDelete(so->arrayContext);
	if (so->killedItems != NULL)
		pfree(so->killedItems);
	if (so->currTuples != NULL)
		pfree(so->currTuples);
	/* so->markTuples should not be pfree'd, see treebrescan */
	pfree(so);
}

/*
 *	treebmarkpos() -- save current scan position
 */
void
treebmarkpos(IndexScanDesc scan)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;

	/* There may be an old mark with a pin (but no lock). */
	TreebScanPosUnpinIfPinned(so->markPos);

	/*
	 * Just record the current itemIndex.  If we later step to next page
	 * before releasing the marked position, _treeb_steppage makes a full copy of
	 * the currPos struct in markPos.  If (as often happens) the mark is moved
	 * before we leave the page, we don't have to do that work.
	 */
	if (TreebScanPosIsValid(so->currPos))
		so->markItemIndex = so->currPos.itemIndex;
	else
	{
		TreebScanPosInvalidate(so->markPos);
		so->markItemIndex = -1;
	}
}

/*
 *	treebrestrpos() -- restore scan to last saved position
 */
void
treebrestrpos(IndexScanDesc scan)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;

	if (so->markItemIndex >= 0)
	{
		/*
		 * The scan has never moved to a new page since the last mark.  Just
		 * restore the itemIndex.
		 *
		 * NB: In this case we can't count on anything in so->markPos to be
		 * accurate.
		 */
		so->currPos.itemIndex = so->markItemIndex;
	}
	else
	{
		/*
		 * The scan moved to a new page after last mark or restore, and we are
		 * now restoring to the marked page.  We aren't holding any read
		 * locks, but if we're still holding the pin for the current position,
		 * we must drop it.
		 */
		if (TreebScanPosIsValid(so->currPos))
		{
			/* Before leaving current page, deal with any killed items */
			if (so->numKilled > 0)
				_treeb_killitems(scan);
			TreebScanPosUnpinIfPinned(so->currPos);
		}

		if (TreebScanPosIsValid(so->markPos))
		{
			/* bump pin on mark buffer for assignment to current buffer */
			if (TreebScanPosIsPinned(so->markPos))
				IncrBufferRefCount(so->markPos.buf);
			memcpy(&so->currPos, &so->markPos,
				   offsetof(TreebScanPosData, items[1]) +
				   so->markPos.lastItem * sizeof(TreebScanPosItem));
			if (so->currTuples)
				memcpy(so->currTuples, so->markTuples,
					   so->markPos.nextTupleOffset);
			/* Reset the scan's array keys (see _treeb_steppage for why) */
			if (so->numArrayKeys)
			{
				_treeb_start_array_keys(scan, so->currPos.dir);
				so->needPrimScan = false;
			}
		}
		else
			TreebScanPosInvalidate(so->currPos);
	}
}

/*
 * treebestimateparallelscan -- estimate storage for TreebParallelScanDescData
 */
Size
treebestimateparallelscan(int nkeys, int norderbys)
{
	/* Pessimistically assume all input scankeys will be output with arrays */
	return offsetof(TreebParallelScanDescData, btps_arrElems) + sizeof(int) * nkeys;
}

/*
 * treebinitparallelscan -- initialize TreebParallelScanDesc for parallel treeb scan
 */
void
treebinitparallelscan(void *target)
{
	TreebParallelScanDesc treeb_target = (TreebParallelScanDesc) target;

	SpinLockInit(&treeb_target->treebps_mutex);
	treeb_target->treebps_scanPage = InvalidBlockNumber;
	treeb_target->treebps_pageStatus = TREEBPARALLEL_NOT_INITIALIZED;
	ConditionVariableInit(&treeb_target->treebps_cv);
}

/*
 *	treebparallelrescan() -- reset parallel scan
 */
void
treebparallelrescan(IndexScanDesc scan)
{
	TreebParallelScanDesc treebscan;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;

	Assert(parallel_scan);

	treebscan = (TreebParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	/*
	 * In theory, we don't need to acquire the spinlock here, because there
	 * shouldn't be any other workers running at this point, but we do so for
	 * consistency.
	 */
	SpinLockAcquire(&treebscan->treebps_mutex);
	treebscan->treebps_scanPage = InvalidBlockNumber;
	treebscan->treebps_pageStatus = TREEBPARALLEL_NOT_INITIALIZED;
	SpinLockRelease(&treebscan->treebps_mutex);
}

/*
 * _treeb_parallel_seize() -- Begin the process of advancing the scan to a new
 *		page.  Other scans must wait until we call _treeb_parallel_release()
 *		or _treeb_parallel_done().
 *
 * The return value is true if we successfully seized the scan and false
 * if we did not.  The latter case occurs if no pages remain.
 *
 * If the return value is true, *pageno returns the next or current page
 * of the scan (depending on the scan direction).  An invalid block number
 * means the scan hasn't yet started, or that caller needs to start the next
 * primitive index scan (if it's the latter case we'll set so.needPrimScan).
 * The first time a participating process reaches the last page, it will return
 * true and set *pageno to P_NONE; after that, further attempts to seize the
 * scan will return false.
 *
 * Callers should ignore the value of pageno if the return value is false.
 *
 * Callers that are in a position to start a new primitive index scan must
 * pass first=true (all other callers pass first=false).  We just return false
 * for first=false callers that require another primitive index scan.
 */
bool
_treeb_parallel_seize(IndexScanDesc scan, BlockNumber *pageno, bool first)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	bool		exit_loop = false;
	bool		status = true;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	TreebParallelScanDesc treebscan;

	*pageno = P_NONE;

	if (first)
	{
		/*
		 * Initialize array related state when called from _treeb_first, assuming
		 * that this will either be the first primitive index scan for the
		 * scan, or a previous explicitly scheduled primitive scan.
		 *
		 * Note: so->needPrimScan is only set when a scheduled primitive index
		 * scan is set to be performed in caller's worker process.  It should
		 * not be set here by us for the first primitive scan, nor should we
		 * ever set it for a parallel scan that has no array keys.
		 */
		so->needPrimScan = false;
		so->scanBehind = false;
	}
	else
	{
		/*
		 * Don't attempt to seize the scan when backend requires another
		 * primitive index scan unless we're in a position to start it now
		 */
		if (so->needPrimScan)
			return false;
	}

	treebscan = (TreebParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	while (1)
	{
		SpinLockAcquire(&treebscan->treebps_mutex);

		if (treebscan->treebps_pageStatus == TREEBPARALLEL_DONE)
		{
			/* We're done with this parallel index scan */
			status = false;
		}
		else if (treebscan->treebps_pageStatus == TREEBPARALLEL_NEED_PRIMSCAN)
		{
			Assert(so->numArrayKeys);

			/*
			 * If we can start another primitive scan right away, do so.
			 * Otherwise just wait.
			 */
			if (first)
			{
				treebscan->treebps_pageStatus = TREEBPARALLEL_ADVANCING;
				for (int i = 0; i < so->numArrayKeys; i++)
				{
					TreebArrayKeyInfo *array = &so->arrayKeys[i];
					ScanKey		skey = &so->keyData[array->scan_key];

					array->cur_elem = treebscan->btps_arrElems[i];
					skey->sk_argument = array->elem_values[array->cur_elem];
				}
				so->needPrimScan = true;
				so->scanBehind = false;
				*pageno = InvalidBlockNumber;
				exit_loop = true;
			}
		}
		else if (treebscan->treebps_pageStatus != TREEBPARALLEL_ADVANCING)
		{
			/*
			 * We have successfully seized control of the scan for the purpose
			 * of advancing it to a new page!
			 */
			treebscan->treebps_pageStatus = TREEBPARALLEL_ADVANCING;
			*pageno = treebscan->treebps_scanPage;
			exit_loop = true;
		}
		SpinLockRelease(&treebscan->treebps_mutex);
		if (exit_loop || !status)
			break;
		ConditionVariableSleep(&treebscan->treebps_cv, WAIT_EVENT_TREEB_PAGE);
	}
	ConditionVariableCancelSleep();

	return status;
}

/*
 * _treeb_parallel_release() -- Complete the process of advancing the scan to a
 *		new page.  We now have the new value treebps_scanPage; some other backend
 *		can now begin advancing the scan.
 *
 * Callers whose scan uses array keys must save their scan_page argument so
 * that it can be passed to _treeb_parallel_primscan_schedule, should caller
 * determine that another primitive index scan is required.  If that happens,
 * scan_page won't be scanned by any backend (unless the next primitive index
 * scan lands on scan_page).
 */
void
_treeb_parallel_release(IndexScanDesc scan, BlockNumber scan_page)
{
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	TreebParallelScanDesc treebscan;

	treebscan = (TreebParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	SpinLockAcquire(&treebscan->treebps_mutex);
	treebscan->treebps_scanPage = scan_page;
	treebscan->treebps_pageStatus = TREEBPARALLEL_IDLE;
	SpinLockRelease(&treebscan->treebps_mutex);
	ConditionVariableSignal(&treebscan->treebps_cv);
}

/*
 * _treeb_parallel_done() -- Mark the parallel scan as complete.
 *
 * When there are no pages left to scan, this function should be called to
 * notify other workers.  Otherwise, they might wait forever for the scan to
 * advance to the next page.
 */
void
_treeb_parallel_done(IndexScanDesc scan)
{
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	TreebParallelScanDesc treebscan;
	bool		status_changed = false;

	/* Do nothing, for non-parallel scans */
	if (parallel_scan == NULL)
		return;

	treebscan = (TreebParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	/*
	 * Mark the parallel scan as done, unless some other process did so
	 * already
	 */
	SpinLockAcquire(&treebscan->treebps_mutex);
	if (treebscan->treebps_pageStatus != TREEBPARALLEL_DONE)
	{
		treebscan->treebps_pageStatus = TREEBPARALLEL_DONE;
		status_changed = true;
	}
	SpinLockRelease(&treebscan->treebps_mutex);

	/* wake up all the workers associated with this parallel scan */
	if (status_changed)
		ConditionVariableBroadcast(&treebscan->treebps_cv);
}

/*
 * _treeb_parallel_primscan_schedule() -- Schedule another primitive index scan.
 *
 * Caller passes the block number most recently passed to _treeb_parallel_release
 * by its backend.  Caller successfully schedules the next primitive index scan
 * if the shared parallel state hasn't been seized since caller's backend last
 * advanced the scan.
 */
void
_treeb_parallel_primscan_schedule(IndexScanDesc scan, BlockNumber prev_scan_page)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	TreebParallelScanDesc treebscan;

	Assert(so->numArrayKeys);

	treebscan = (TreebParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	SpinLockAcquire(&treebscan->treebps_mutex);
	if (treebscan->treebps_scanPage == prev_scan_page &&
		treebscan->treebps_pageStatus == TREEBPARALLEL_IDLE)
	{
		treebscan->treebps_scanPage = InvalidBlockNumber;
		treebscan->treebps_pageStatus = TREEBPARALLEL_NEED_PRIMSCAN;

		/* Serialize scan's current array keys */
		for (int i = 0; i < so->numArrayKeys; i++)
		{
			TreebArrayKeyInfo *array = &so->arrayKeys[i];

			treebscan->btps_arrElems[i] = array->cur_elem;
		}
	}
	SpinLockRelease(&treebscan->treebps_mutex);
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
treebbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	TreebCycleId	cycleid;

	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Establish the vacuum cycle ID to use for this scan */
	/* The ENSURE stuff ensures we clean up shared memory on failure */
	PG_ENSURE_ERROR_CLEANUP(_treeb_end_vacuum_callback, PointerGetDatum(rel));
	{
		cycleid = _treeb_start_vacuum(rel);

		treebvacuumscan(info, stats, callback, callback_state, cycleid);
	}
	PG_END_ENSURE_ERROR_CLEANUP(_treeb_end_vacuum_callback, PointerGetDatum(rel));
	_treeb_end_vacuum(rel);

	return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
treebvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	BlockNumber num_delpages;

	/* No-op in ANALYZE ONLY mode */
	if (info->analyze_only)
		return stats;

	/*
	 * If treebbulkdelete was called, we need not do anything (we just maintain
	 * the information used within _treeb_vacuum_needs_cleanup() by calling
	 * _treeb_set_cleanup_info() below).
	 *
	 * If treebbulkdelete was _not_ called, then we have a choice to make: we
	 * must decide whether or not a treebvacuumscan() call is needed now (i.e.
	 * whether the ongoing VACUUM operation can entirely avoid a physical scan
	 * of the index).  A call to _treeb_vacuum_needs_cleanup() decides it for us
	 * now.
	 */
	if (stats == NULL)
	{
		/* Check if VACUUM operation can entirely avoid treebvacuumscan() call */
		if (!_treeb_vacuum_needs_cleanup(info->index))
			return NULL;

		/*
		 * Since we aren't going to actually delete any leaf items, there's no
		 * need to go through all the vacuum-cycle-ID pushups here.
		 *
		 * Posting list tuples are a source of inaccuracy for cleanup-only
		 * scans.  treebvacuumscan() will assume that the number of index tuples
		 * from each page can be used as num_index_tuples, even though
		 * num_index_tuples is supposed to represent the number of TIDs in the
		 * index.  This naive approach can underestimate the number of tuples
		 * in the index significantly.
		 *
		 * We handle the problem by making num_index_tuples an estimate in
		 * cleanup-only case.
		 */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		treebvacuumscan(info, stats, NULL, NULL, 0);
		stats->estimated_count = true;
	}

	/*
	 * Maintain num_delpages value in metapage for _treeb_vacuum_needs_cleanup().
	 *
	 * num_delpages is the number of deleted pages now in the index that were
	 * not safe to place in the FSM to be recycled just yet.  num_delpages is
	 * greater than 0 only when _treeb_pagedel() actually deleted pages during
	 * our call to treebvacuumscan().  Even then, _treeb_pendingfsm_finalize() must
	 * have failed to place any newly deleted pages in the FSM just moments
	 * ago.  (Actually, there are edge cases where recycling of the current
	 * VACUUM's newly deleted pages does not even become safe by the time the
	 * next VACUUM comes around.  See treeb/README.)
	 */
	Assert(stats->pages_deleted >= stats->pages_free);
	num_delpages = stats->pages_deleted - stats->pages_free;
	_treeb_set_cleanup_info(info->index, num_delpages);

	/*
	 * It's quite possible for us to be fooled by concurrent page splits into
	 * double-counting some index tuples, so disbelieve any total that exceeds
	 * the underlying heap's count ... if we know that accurately.  Otherwise
	 * this might just make matters worse.
	 */
	if (!info->estimated_count)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	return stats;
}

/*
 * treebvacuumscan --- scan the index for VACUUMing purposes
 *
 * This combines the functions of looking for leaf tuples that are deletable
 * according to the vacuum callback, looking for empty pages that can be
 * deleted, and looking for old deleted pages that can be recycled.  Both
 * treebbulkdelete and treebvacuumcleanup invoke this (the latter only if no
 * treebbulkdelete call occurred and _treeb_vacuum_needs_cleanup returned true).
 *
 * The caller is responsible for initially allocating/zeroing a stats struct
 * and for obtaining a vacuum cycle ID if necessary.
 */
static void
treebvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state,
			 TreebCycleId cycleid)
{
	Relation	rel = info->index;
	TreebVacState	vstate;
	BlockNumber num_pages;
	BlockNumber scanblkno;
	bool		needLock;

	/*
	 * Reset fields that track information about the entire index now.  This
	 * avoids double-counting in the case where a single VACUUM command
	 * requires multiple scans of the index.
	 *
	 * Avoid resetting the tuples_removed and pages_newly_deleted fields here,
	 * since they track information about the VACUUM command, and so must last
	 * across each call to treebvacuumscan().
	 *
	 * (Note that pages_free is treated as state about the whole index, not
	 * the current VACUUM.  This is appropriate because RecordFreeIndexPage()
	 * calls are idempotent, and get repeated for the same deleted pages in
	 * some scenarios.  The point for us is to track the number of recyclable
	 * pages in the index at the end of the VACUUM command.)
	 */
	stats->num_pages = 0;
	stats->num_index_tuples = 0;
	stats->pages_deleted = 0;
	stats->pages_free = 0;

	/* Set up info to pass down to treebvacuumpage */
	vstate.info = info;
	vstate.stats = stats;
	vstate.callback = callback;
	vstate.callback_state = callback_state;
	vstate.cycleid = cycleid;

	/* Create a temporary memory context to run _treeb_pagedel in */
	vstate.pagedelcontext = AllocSetContextCreate(CurrentMemoryContext,
												  "_treeb_pagedel",
												  ALLOCSET_DEFAULT_SIZES);

	/* Initialize vstate fields used by _treeb_pendingfsm_finalize */
	vstate.bufsize = 0;
	vstate.maxbufsize = 0;
	vstate.pendingpages = NULL;
	vstate.npendingpages = 0;
	/* Consider applying _treeb_pendingfsm_finalize optimization */
	_treeb_pendingfsm_init(rel, &vstate, (callback == NULL));

	/*
	 * The outer loop iterates over all index pages except the metapage, in
	 * physical order (we hope the kernel will cooperate in providing
	 * read-ahead for speed).  It is critical that we visit all leaf pages,
	 * including ones added after we start the scan, else we might fail to
	 * delete some deletable tuples.  Hence, we must repeatedly check the
	 * relation length.  We must acquire the relation-extension lock while
	 * doing so to avoid a race condition: if someone else is extending the
	 * relation, there is a window where bufmgr/smgr have created a new
	 * all-zero page but it hasn't yet been write-locked by _treeb_getbuf(). If
	 * we manage to scan such a page here, we'll improperly assume it can be
	 * recycled.  Taking the lock synchronizes things enough to prevent a
	 * problem: either num_pages won't include the new page, or _treeb_getbuf
	 * already has write lock on the buffer and it will be fully initialized
	 * before we can examine it.  Also, we need not worry if a page is added
	 * immediately after we look; the page splitting code already has
	 * write-lock on the left page before it adds a right page, so we must
	 * already have processed any tuples due to be moved into such a page.
	 *
	 * XXX: Now that new pages are locked with RBM_ZERO_AND_LOCK, I don't
	 * think the use of the extension lock is still required.
	 *
	 * We can skip locking for new or temp relations, however, since no one
	 * else could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(rel);

	scanblkno = TREEB_METAPAGE + 1;
	for (;;)
	{
		/* Get the current relation length */
		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(rel);
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		if (info->report_progress)
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
										 num_pages);

		/* Quit if we've scanned the whole relation */
		if (scanblkno >= num_pages)
			break;
		/* Iterate over pages, then loop back to recheck length */
		for (; scanblkno < num_pages; scanblkno++)
		{
			treebvacuumpage(&vstate, scanblkno);
			if (info->report_progress)
				pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
											 scanblkno);
		}
	}

	/* Set statistics num_pages field to final size of index */
	stats->num_pages = num_pages;

	MemoryContextDelete(vstate.pagedelcontext);

	/*
	 * If there were any calls to _treeb_pagedel() during scan of the index then
	 * see if any of the resulting pages can be placed in the FSM now.  When
	 * it's not safe we'll have to leave it up to a future VACUUM operation.
	 *
	 * Finally, if we placed any pages in the FSM (either just now or during
	 * the scan), forcibly update the upper-level FSM pages to ensure that
	 * searchers can find them.
	 */
	_treeb_pendingfsm_finalize(rel, &vstate);
	if (stats->pages_free > 0)
		IndexFreeSpaceMapVacuum(rel);
}

/*
 * treebvacuumpage --- VACUUM one page
 *
 * This processes a single page for treebvacuumscan().  In some cases we must
 * backtrack to re-examine and VACUUM pages that were the scanblkno during
 * a previous call here.  This is how we handle page splits (that happened
 * after our cycleid was acquired) whose right half page happened to reuse
 * a block that we might have processed at some point before it was
 * recycled (i.e. before the page split).
 */
static void
treebvacuumpage(TreebVacState *vstate, BlockNumber scanblkno)
{
	IndexVacuumInfo *info = vstate->info;
	IndexBulkDeleteResult *stats = vstate->stats;
	IndexBulkDeleteCallback callback = vstate->callback;
	void	   *callback_state = vstate->callback_state;
	Relation	rel = info->index;
	Relation	heaprel = info->heaprel;
	bool		attempt_pagedel;
	BlockNumber blkno,
				backtrack_to;
	Buffer		buf;
	Page		page;
	TreebPageOpaque opaque;

	blkno = scanblkno;

backtrack:

	attempt_pagedel = false;
	backtrack_to = P_NONE;

	/* call vacuum_delay_point while not holding any buffer lock */
	vacuum_delay_point();

	/*
	 * We can't use _treeb_getbuf() here because it always applies
	 * _treeb_checkpage(), which will barf on an all-zero page. We want to
	 * recycle all-zero pages, not fail.  Also, we want to use a nondefault
	 * buffer access strategy.
	 */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
							 info->strategy);
	_treeb_lockbuf(rel, buf, TREEB_READ);
	page = BufferGetPage(buf);
	opaque = NULL;
	if (!PageIsNew(page))
	{
		_treeb_checkpage(rel, buf);
		opaque = TREEBPageGetOpaque(page);
	}

	Assert(blkno <= scanblkno);
	if (blkno != scanblkno)
	{
		/*
		 * We're backtracking.
		 *
		 * We followed a right link to a sibling leaf page (a page that
		 * happens to be from a block located before scanblkno).  The only
		 * case we want to do anything with is a live leaf page having the
		 * current vacuum cycle ID.
		 *
		 * The page had better be in a state that's consistent with what we
		 * expect.  Check for conditions that imply corruption in passing.  It
		 * can't be half-dead because only an interrupted VACUUM process can
		 * leave pages in that state, so we'd definitely have dealt with it
		 * back when the page was the scanblkno page (half-dead pages are
		 * always marked fully deleted by _treeb_pagedel(), barring corruption).
		 */
		if (!opaque || !P_ISLEAF(opaque) || P_ISHALFDEAD(opaque))
		{
			Assert(false);
			ereport(LOG,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("right sibling %u of scanblkno %u unexpectedly in an inconsistent state in index \"%s\"",
									 blkno, scanblkno, RelationGetRelationName(rel))));
			_treeb_relbuf(rel, buf);
			return;
		}

		/*
		 * We may have already processed the page in an earlier call, when the
		 * page was scanblkno.  This happens when the leaf page split occurred
		 * after the scan began, but before the right sibling page became the
		 * scanblkno.
		 *
		 * Page may also have been deleted by current treebvacuumpage() call,
		 * since _treeb_pagedel() sometimes deletes the right sibling page of
		 * scanblkno in passing (it does so after we decided where to
		 * backtrack to).  We don't need to process this page as a deleted
		 * page a second time now (in fact, it would be wrong to count it as a
		 * deleted page in the bulk delete statistics a second time).
		 */
		if (opaque->treebpo_cycleid != vstate->cycleid || P_ISDELETED(opaque))
		{
			/* Done with current scanblkno (and all lower split pages) */
			_treeb_relbuf(rel, buf);
			return;
		}
	}

	if (!opaque || TreebPageIsRecyclable(page, heaprel))
	{
		/* Okay to recycle this page (which could be leaf or internal) */
		RecordFreeIndexPage(rel, blkno);
		stats->pages_deleted++;
		stats->pages_free++;
	}
	else if (P_ISDELETED(opaque))
	{
		/*
		 * Already deleted page (which could be leaf or internal).  Can't
		 * recycle yet.
		 */
		stats->pages_deleted++;
	}
	else if (P_ISHALFDEAD(opaque))
	{
		/* Half-dead leaf page (from interrupted VACUUM) -- finish deleting */
		attempt_pagedel = true;

		/*
		 * _treeb_pagedel() will increment both pages_newly_deleted and
		 * pages_deleted stats in all cases (barring corruption)
		 */
	}
	else if (P_ISLEAF(opaque))
	{
		OffsetNumber deletable[MaxIndexTuplesPerPage];
		int			ndeletable;
		TreebVacuumPosting updatable[MaxIndexTuplesPerPage];
		int			nupdatable;
		OffsetNumber offnum,
					minoff,
					maxoff;
		int			nhtidsdead,
					nhtidslive;

		/*
		 * Trade in the initial read lock for a full cleanup lock on this
		 * page.  We must get such a lock on every leaf page over the course
		 * of the vacuum scan, whether or not it actually contains any
		 * deletable tuples --- see treeb/README.
		 */
		_treeb_upgradelockbufcleanup(rel, buf);

		/*
		 * Check whether we need to backtrack to earlier pages.  What we are
		 * concerned about is a page split that happened since we started the
		 * vacuum scan.  If the split moved tuples on the right half of the
		 * split (i.e. the tuples that sort high) to a block that we already
		 * passed over, then we might have missed the tuples.  We need to
		 * backtrack now.  (Must do this before possibly clearing treebpo_cycleid
		 * or deleting scanblkno page below!)
		 */
		if (vstate->cycleid != 0 &&
			opaque->treebpo_cycleid == vstate->cycleid &&
			!(opaque->treebpo_flags & TREEBP_SPLIT_END) &&
			!P_RIGHTMOST(opaque) &&
			opaque->treebpo_next < scanblkno)
			backtrack_to = opaque->treebpo_next;

		ndeletable = 0;
		nupdatable = 0;
		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		nhtidsdead = 0;
		nhtidslive = 0;
		if (callback)
		{
			/* treebbulkdelete callback tells us what to delete (or update) */
			for (offnum = minoff;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				IndexTuple	itup;

				itup = (IndexTuple) PageGetItem(page,
												PageGetItemId(page, offnum));

				Assert(!TreebTupleIsPivot(itup));
				if (!TreebTupleIsPosting(itup))
				{
					/* Regular tuple, standard table TID representation */
					if (callback(&itup->t_tid, callback_state))
					{
						deletable[ndeletable++] = offnum;
						nhtidsdead++;
					}
					else
						nhtidslive++;
				}
				else
				{
					TreebVacuumPosting vacposting;
					int			nremaining;

					/* Posting list tuple */
					vacposting = treebvacuumposting(vstate, itup, offnum,
													&nremaining);
					if (vacposting == NULL)
					{
						/*
						 * All table TIDs from the posting tuple remain, so no
						 * delete or update required
						 */
						Assert(nremaining == TreebTupleGetNPosting(itup));
					}
					else if (nremaining > 0)
					{

						/*
						 * Store metadata about posting list tuple in
						 * updatable array for entire page.  Existing tuple
						 * will be updated during the later call to
						 * _treeb_delitems_vacuum().
						 */
						Assert(nremaining < TreebTupleGetNPosting(itup));
						updatable[nupdatable++] = vacposting;
						nhtidsdead += TreebTupleGetNPosting(itup) - nremaining;
					}
					else
					{
						/*
						 * All table TIDs from the posting list must be
						 * deleted.  We'll delete the index tuple completely
						 * (no update required).
						 */
						Assert(nremaining == 0);
						deletable[ndeletable++] = offnum;
						nhtidsdead += TreebTupleGetNPosting(itup);
						pfree(vacposting);
					}

					nhtidslive += nremaining;
				}
			}
		}

		/*
		 * Apply any needed deletes or updates.  We issue just one
		 * _treeb_delitems_vacuum() call per page, so as to minimize WAL traffic.
		 */
		if (ndeletable > 0 || nupdatable > 0)
		{
			Assert(nhtidsdead >= ndeletable + nupdatable);
			_treeb_delitems_vacuum(rel, buf, deletable, ndeletable, updatable,
								nupdatable);

			stats->tuples_removed += nhtidsdead;
			/* must recompute maxoff */
			maxoff = PageGetMaxOffsetNumber(page);

			/* can't leak memory here */
			for (int i = 0; i < nupdatable; i++)
				pfree(updatable[i]);
		}
		else
		{
			/*
			 * If the leaf page has been split during this vacuum cycle, it
			 * seems worth expending a write to clear treebpo_cycleid even if we
			 * don't have any deletions to do.  (If we do, _treeb_delitems_vacuum
			 * takes care of this.)  This ensures we won't process the page
			 * again.
			 *
			 * We treat this like a hint-bit update because there's no need to
			 * WAL-log it.
			 */
			Assert(nhtidsdead == 0);
			if (vstate->cycleid != 0 &&
				opaque->treebpo_cycleid == vstate->cycleid)
			{
				opaque->treebpo_cycleid = 0;
				MarkBufferDirtyHint(buf, true);
			}
		}

		/*
		 * If the leaf page is now empty, try to delete it; else count the
		 * live tuples (live table TIDs in posting lists are counted as
		 * separate live tuples).  We don't delete when backtracking, though,
		 * since that would require teaching _treeb_pagedel() about backtracking
		 * (doesn't seem worth adding more complexity to deal with that).
		 *
		 * We don't count the number of live TIDs during cleanup-only calls to
		 * treebvacuumscan (i.e. when callback is not set).  We count the number
		 * of index tuples directly instead.  This avoids the expense of
		 * directly examining all of the tuples on each page.  VACUUM will
		 * treat num_index_tuples as an estimate in cleanup-only case, so it
		 * doesn't matter that this underestimates num_index_tuples
		 * significantly in some cases.
		 */
		if (minoff > maxoff)
			attempt_pagedel = (blkno == scanblkno);
		else if (callback)
			stats->num_index_tuples += nhtidslive;
		else
			stats->num_index_tuples += maxoff - minoff + 1;

		Assert(!attempt_pagedel || nhtidslive == 0);
	}

	if (attempt_pagedel)
	{
		MemoryContext oldcontext;

		/* Run pagedel in a temp context to avoid memory leakage */
		MemoryContextReset(vstate->pagedelcontext);
		oldcontext = MemoryContextSwitchTo(vstate->pagedelcontext);

		/*
		 * _treeb_pagedel maintains the bulk delete stats on our behalf;
		 * pages_newly_deleted and pages_deleted are likely to be incremented
		 * during call
		 */
		Assert(blkno == scanblkno);
		_treeb_pagedel(rel, buf, vstate);

		MemoryContextSwitchTo(oldcontext);
		/* pagedel released buffer, so we shouldn't */
	}
	else
		_treeb_relbuf(rel, buf);

	if (backtrack_to != P_NONE)
	{
		blkno = backtrack_to;
		goto backtrack;
	}
}

/*
 * treebvacuumposting --- determine TIDs still needed in posting list
 *
 * Returns metadata describing how to build replacement tuple without the TIDs
 * that VACUUM needs to delete.  Returned value is NULL in the common case
 * where no changes are needed to caller's posting list tuple (we avoid
 * allocating memory here as an optimization).
 *
 * The number of TIDs that should remain in the posting list tuple is set for
 * caller in *nremaining.
 */
static TreebVacuumPosting
treebvacuumposting(TreebVacState *vstate, IndexTuple posting,
				   OffsetNumber updatedoffset, int *nremaining)
{
	int			live = 0;
	int			nitem = TreebTupleGetNPosting(posting);
	ItemPointer items = TreebTupleGetPosting(posting);
	TreebVacuumPosting vacposting = NULL;

	for (int i = 0; i < nitem; i++)
	{
		if (!vstate->callback(items + i, vstate->callback_state))
		{
			/* Live table TID */
			live++;
		}
		else if (vacposting == NULL)
		{
			/*
			 * First dead table TID encountered.
			 *
			 * It's now clear that we need to delete one or more dead table
			 * TIDs, so start maintaining metadata describing how to update
			 * existing posting list tuple.
			 */
			vacposting = palloc(offsetof(TreebVacuumPostingData, deletetids) +
								nitem * sizeof(uint16));

			vacposting->itup = posting;
			vacposting->updatedoffset = updatedoffset;
			vacposting->ndeletedtids = 0;
			vacposting->deletetids[vacposting->ndeletedtids++] = i;
		}
		else
		{
			/* Second or subsequent dead table TID */
			vacposting->deletetids[vacposting->ndeletedtids++] = i;
		}
	}

	*nremaining = live;
	return vacposting;
}

/*
 *	treebcanreturn() -- Check whether treeb indexes support index-only scans.
 *
 * treebs always do, so this is trivial.
 */
bool
treebcanreturn(Relation index, int attno)
{
	return true;
}


void
treebcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
			   Cost *indexStartupCost, Cost *indexTotalCost,
			   Selectivity *indexSelectivity, double *indexCorrelation,
			   double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Oid			relid;
	AttrNumber	colnum;
	VariableStatData vardata = {0};
	double		numIndexTuples;
	Cost		descentCost;
	List	   *indexBoundQuals;
	int			indexcol;
	bool		eqQualHere;
	bool		found_saop;
	bool		found_is_null_op;
	double		num_sa_scans;
	ListCell   *lc;

	/*
	 * For a treeb scan, only leading '=' quals plus inequality quals for the
	 * immediately next attribute contribute to index selectivity (these are
	 * the "boundary quals" that determine the starting and stopping points of
	 * the index scan).  Additional quals can suppress visits to the heap, so
	 * it's OK to count them in indexSelectivity, but they should not count
	 * for estimating numIndexTuples.  So we must examine the given indexquals
	 * to find out which ones count as boundary quals.  We rely on the
	 * knowledge that they are given in index column order.
	 *
	 * For a RowCompareExpr, we consider only the first column, just as
	 * rowcomparesel() does.
	 *
	 * If there's a ScalarArrayOpExpr in the quals, we'll actually perform up
	 * to N index descents (not just one), but the ScalarArrayOpExpr's
	 * operator can be considered to act the same as it normally does.
	 */
	indexBoundQuals = NIL;
	indexcol = 0;
	eqQualHere = false;
	found_saop = false;
	found_is_null_op = false;
	num_sa_scans = 1;
	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		if (indexcol != iclause->indexcol)
		{
			/* Beginning of a new column's quals */
			if (!eqQualHere)
				break;			/* done if no '=' qual for indexcol */
			eqQualHere = false;
			indexcol++;
			if (indexcol != iclause->indexcol)
				break;			/* no quals at all for indexcol */
		}

		/* Examine each indexqual associated with this index clause */
		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Expr	   *clause = rinfo->clause;
			Oid			clause_op = InvalidOid;
			int			op_strategy;

			if (IsA(clause, OpExpr))
			{
				OpExpr	   *op = (OpExpr *) clause;

				clause_op = op->opno;
			}
			else if (IsA(clause, RowCompareExpr))
			{
				RowCompareExpr *rc = (RowCompareExpr *) clause;

				clause_op = linitial_oid(rc->opnos);
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
				Node	   *other_operand = (Node *) lsecond(saop->args);
				double		alength = estimate_array_length(root, other_operand);

				clause_op = saop->opno;
				found_saop = true;
				/* estimate SA descents by indexBoundQuals only */
				if (alength > 1)
					num_sa_scans *= alength;
			}
			else if (IsA(clause, NullTest))
			{
				NullTest   *nt = (NullTest *) clause;

				if (nt->nulltesttype == IS_NULL)
				{
					found_is_null_op = true;
					/* IS NULL is like = for selectivity purposes */
					eqQualHere = true;
				}
			}
			else
				elog(ERROR, "unsupported indexqual type: %d",
					 (int) nodeTag(clause));

			/* check for equality operator */
			if (OidIsValid(clause_op))
			{
				op_strategy = get_op_opfamily_strategy(clause_op,
													   index->opfamily[indexcol]);
				Assert(op_strategy != 0);	/* not a member of opfamily?? */
				if (op_strategy == TreebEqualStrategyNumber)
					eqQualHere = true;
			}

			indexBoundQuals = lappend(indexBoundQuals, rinfo);
		}
	}

	/*
	 * If index is unique and we found an '=' clause for each column, we can
	 * just assume numIndexTuples = 1 and skip the expensive
	 * clauselist_selectivity calculations.  However, a ScalarArrayOp or
	 * NullTest invalidates that theory, even though it sets eqQualHere.
	 */
	if (index->unique &&
		indexcol == index->nkeycolumns - 1 &&
		eqQualHere &&
		!found_saop &&
		!found_is_null_op)
		numIndexTuples = 1.0;
	else
	{
		List	   *selectivityQuals;
		Selectivity btreeSelectivity;

		/*
		 * If the index is partial, AND the index predicate with the
		 * index-bound quals to produce a more accurate idea of the number of
		 * rows covered by the bound conditions.
		 */
		selectivityQuals = add_predicate_to_index_quals(index, indexBoundQuals);

		btreeSelectivity = clauselist_selectivity(root, selectivityQuals,
												  index->rel->relid,
												  JOIN_INNER,
												  NULL);
		numIndexTuples = btreeSelectivity * index->rel->tuples;

		/*
		 * treeb automatically combines individual ScalarArrayOpExpr primitive
		 * index scans whenever the tuples covered by the next set of array
		 * keys are close to tuples covered by the current set.  That puts a
		 * natural ceiling on the worst case number of descents -- there
		 * cannot possibly be more than one descent per leaf page scanned.
		 *
		 * Clamp the number of descents to at most 1/3 the number of index
		 * pages.  This avoids implausibly high estimates with low selectivity
		 * paths, where scans usually require only one or two descents.  This
		 * is most likely to help when there are several SAOP clauses, where
		 * naively accepting the total number of distinct combinations of
		 * array elements as the number of descents would frequently lead to
		 * wild overestimates.
		 *
		 * We somewhat arbitrarily don't just make the cutoff the total number
		 * of leaf pages (we make it 1/3 the total number of pages instead) to
		 * give the treeb code credit for its ability to continue on the leaf
		 * level with low selectivity scans.
		 */
		num_sa_scans = Min(num_sa_scans, ceil(index->pages * 0.3333333));
		num_sa_scans = Max(num_sa_scans, 1);

		/*
		 * As in genericcostestimate(), we have to adjust for any
		 * ScalarArrayOpExpr quals included in indexBoundQuals, and then round
		 * to integer.
		 *
		 * It is tempting to make genericcostestimate behave as if SAOP
		 * clauses work in almost the same way as scalar operators during
		 * treeb scans, making the top-level scan look like a continuous scan
		 * (as opposed to num_sa_scans-many primitive index scans).  After
		 * all, treeb scans mostly work like that at runtime.  However, such a
		 * scheme would badly bias genericcostestimate's simplistic approach
		 * to calculating numIndexPages through prorating.
		 *
		 * Stick with the approach taken by non-native SAOP scans for now.
		 * genericcostestimate will use the Mackert-Lohman formula to
		 * compensate for repeat page fetches, even though that definitely
		 * won't happen during treeb scans (not for leaf pages, at least).
		 * We're usually very pessimistic about the number of primitive index
		 * scans that will be required, but it's not clear how to do better.
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * Now do generic index cost estimation.
	 */
	costs.numIndexTuples = numIndexTuples;
	costs.num_sa_scans = num_sa_scans;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * Add a CPU-cost component to represent the costs of initial treeb
	 * descent.  We don't charge any I/O cost for touching upper treeb levels,
	 * since they tend to stay in cache, but we still have to do about log2(N)
	 * comparisons to descend a treeb of N leaf tuples.  We charge one
	 * cpu_operator_cost per comparison.
	 *
	 * If there are ScalarArrayOpExprs, charge this once per estimated SA
	 * index descent.  The ones after the first one are not startup cost so
	 * far as the overall plan goes, so just add them to "total" cost.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples) / log(2.0)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Even though we're not charging I/O cost for touching upper treeb pages,
	 * it's still reasonable to charge some CPU cost per page descended
	 * through.  Moreover, if we had no such charge at all, bloated indexes
	 * would appear to have the same search cost as unbloated ones, at least
	 * in cases where only a single leaf page is expected to be visited.  This
	 * cost is somewhat arbitrarily set at 50x cpu_operator_cost per page
	 * touched.  The number of such pages is treeb tree height plus one (ie,
	 * we charge for the leaf page too).  As above, charge once per estimated
	 * SA index descent.
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	/*
	 * If we can get an estimate of the first column's ordering correlation C
	 * from pg_statistic, estimate the index correlation as C for a
	 * single-column index, or C * 0.75 for multiple columns. (The idea here
	 * is that multiple columns dilute the importance of the first column's
	 * ordering, but don't negate it entirely.  Before 8.0 we divided the
	 * correlation by the number of columns, but that seems too strong.)
	 */
	if (index->indexkeys[0] != 0)
	{
		/* Simple variable --- look to stats for the underlying table */
		RangeTblEntry *rte = planner_rt_fetch(index->rel->relid, root);

		Assert(rte->rtekind == RTE_RELATION);
		relid = rte->relid;
		Assert(relid != InvalidOid);
		colnum = index->indexkeys[0];

		if (get_relation_stats_hook &&
			(*get_relation_stats_hook) (root, rte, colnum, &vardata))
		{
			/*
			 * The hook took control of acquiring a stats tuple.  If it did
			 * supply a tuple, it'd better have supplied a freefunc.
			 */
			if (HeapTupleIsValid(vardata.statsTuple) &&
				!vardata.freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata.statsTuple = SearchSysCache3(STATRELATTINH,
												 ObjectIdGetDatum(relid),
												 Int16GetDatum(colnum),
												 BoolGetDatum(rte->inh));
			vardata.freefunc = ReleaseSysCache;
		}
	}
	else
	{
		/* Expression --- maybe there are stats for the index itself */
		relid = index->indexoid;
		colnum = 1;

		if (get_index_stats_hook &&
			(*get_index_stats_hook) (root, relid, colnum, &vardata))
		{
			/*
			 * The hook took control of acquiring a stats tuple.  If it did
			 * supply a tuple, it'd better have supplied a freefunc.
			 */
			if (HeapTupleIsValid(vardata.statsTuple) &&
				!vardata.freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata.statsTuple = SearchSysCache3(STATRELATTINH,
												 ObjectIdGetDatum(relid),
												 Int16GetDatum(colnum),
												 BoolGetDatum(false));
			vardata.freefunc = ReleaseSysCache;
		}
	}

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Oid			sortop;
		AttStatsSlot sslot;

		sortop = get_opfamily_member(index->opfamily[0],
									 index->opcintype[0],
									 index->opcintype[0],
									 TreebLessStrategyNumber);
		if (OidIsValid(sortop) &&
			get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_CORRELATION, sortop,
							 ATTSTATSSLOT_NUMBERS))
		{
			double		varCorrelation;

			Assert(sslot.nnumbers == 1);
			varCorrelation = sslot.numbers[0];

			if (index->reverse_sort[0])
				varCorrelation = -varCorrelation;

			if (index->nkeycolumns > 1)
				costs.indexCorrelation = varCorrelation * 0.75;
			else
				costs.indexCorrelation = varCorrelation;

			free_attstatsslot(&sslot);
		}
	}

	ReleaseVariableStats(vardata);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

/*
 * Look up and call sortsupport function to setup SortSupport comparator;
 * or if no such function exists or it declines to set up the appropriate
 * state, prepare a suitable shim.
 */
static void
FinishSortSupportFunction(Oid opfamily, Oid opcintype, SortSupport ssup)
{
	Oid			sortSupportFunction;

	/* Look for a sort support function */
	sortSupportFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
											TREEBSORTSUPPORT_PROC);
	if (OidIsValid(sortSupportFunction))
	{
		/*
		 * The sort support function can provide a comparator, but it can also
		 * choose not to so (e.g. based on the selected collation).
		 */
		OidFunctionCall1(sortSupportFunction, PointerGetDatum(ssup));
	}

	if (ssup->comparator == NULL)
	{
		Oid			sortFunction;

		sortFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
										 TREEBORDER_PROC);

		if (!OidIsValid(sortFunction))
			elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
				 TREEBORDER_PROC, opcintype, opcintype, opfamily);

		/* We'll use a shim to call the old-style treeb comparator */
		PrepareSortSupportComparisonShim(sortFunction, ssup);
	}
}

/*
 * Fill in SortSupport given an index relation, attribute, and strategy.
 *
 * Caller must previously have zeroed the SortSupportData structure and then
 * filled in ssup_cxt, ssup_attno, ssup_collation, and ssup_nulls_first.  This
 * will fill in ssup_reverse (based on the supplied strategy), as well as the
 * comparator function pointer.
 */
void
PrepareSortSupportFromTreebRel(Relation indexRel, int16 strategy,
							   SortSupport ssup)
{
	Oid			opfamily = indexRel->rd_opfamily[ssup->ssup_attno - 1];
	Oid			opcintype = indexRel->rd_opcintype[ssup->ssup_attno - 1];

	Assert(ssup->comparator == NULL);

	if (indexRel->rd_rel->relam != GetTreebOid())
		elog(ERROR, "unexpected non-treeb AM: %u", indexRel->rd_rel->relam);
	if (strategy != TreebGreaterStrategyNumber &&
		strategy != TreebLessStrategyNumber)
		elog(ERROR, "unexpected sort support strategy: %d", strategy);
	ssup->ssup_reverse = (strategy == TreebGreaterStrategyNumber);

	FinishSortSupportFunction(opfamily, opcintype, ssup);
}

static int
comparetup_index_treeb_tiebreak(const SortTuple *a, const SortTuple *b,
								Tuplesortstate *state)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexTreebArg *arg = (TuplesortIndexTreebArg *) base->arg;
	SortSupport sortKey = base->sortKeys;
	IndexTuple	tuple1;
	IndexTuple	tuple2;
	int			keysz;
	TupleDesc	tupDes;
	bool		equal_hasnull = false;
	int			nkey;
	int32		compare;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;

	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;
	keysz = base->nKeys;
	tupDes = RelationGetDescr(arg->index.indexRel);

	if (sortKey->abbrev_converter)
	{
		datum1 = index_getattr(tuple1, 1, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, 1, tupDes, &isnull2);

		compare = ApplySortAbbrevFullComparator(datum1, isnull1,
												datum2, isnull2,
												sortKey);
		if (compare != 0)
			return compare;
	}

	/* they are equal, so we only need to examine one null flag */
	if (a->isnull1)
		equal_hasnull = true;

	sortKey++;
	for (nkey = 2; nkey <= keysz; nkey++, sortKey++)
	{
		datum1 = index_getattr(tuple1, nkey, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, nkey, tupDes, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;		/* done when we find unequal attributes */

		/* they are equal, so we only need to examine one null flag */
		if (isnull1)
			equal_hasnull = true;
	}

	/*
	 * If treeb has asked us to enforce uniqueness, complain if two equal
	 * tuples are detected (unless there was at least one NULL field and NULLS
	 * NOT DISTINCT was not set).
	 *
	 * It is sufficient to make the test here, because if two tuples are equal
	 * they *must* get compared at some stage of the sort --- otherwise the
	 * sort algorithm wouldn't have checked whether one must appear before the
	 * other.
	 */
	if (arg->enforceUnique && !(!arg->uniqueNullsNotDistinct && equal_hasnull))
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		char	   *key_desc;

		/*
		 * Some rather brain-dead implementations of qsort (such as the one in
		 * QNX 4) will sometimes call the comparison routine to compare a
		 * value to itself, but we always use our own implementation, which
		 * does not.
		 */
		Assert(tuple1 != tuple2);

		index_deform_tuple(tuple1, tupDes, values, isnull);

		key_desc = BuildIndexValueDescription(arg->index.indexRel, values, isnull);

		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("could not create unique index \"%s\"",
						RelationGetRelationName(arg->index.indexRel)),
				 key_desc ? errdetail("Key %s is duplicated.", key_desc) :
				 errdetail("Duplicate keys exist."),
				 errtableconstraint(arg->index.heapRel,
									RelationGetRelationName(arg->index.indexRel))));
	}

	/*
	 * If key values are equal, we sort on ItemPointer.  This is required for
	 * treeb indexes, since heap TID is treated as an implicit last key
	 * attribute in order to ensure that all keys in the index are physically
	 * unique.
	 */
	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	/* ItemPointer values should never be equal */
	Assert(false);

	return 0;
}

static int
comparetup_index_treeb(const SortTuple *a, const SortTuple *b,
					   Tuplesortstate *state)
{
	/*
	 * This is similar to comparetup_heap(), but expects index tuples.  There
	 * is also special handling for enforcing uniqueness, and special
	 * treatment for equal keys at the end.
	 */
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	SortSupport sortKey = base->sortKeys;
	int32		compare;

	/* Compare the leading sort key */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* Compare additional sort keys */
	return comparetup_index_treeb_tiebreak(a, b, state);
}

/*
 * Routines specialized for IndexTuple case
 *
 * The treeb and hash cases require separate comparison functions, but the
 * IndexTuple representation is the same so the copy/write/read support
 * functions can be shared.
 */

static void
removeabbrev_treeb(Tuplesortstate *state, SortTuple *stups, int count)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexArg *arg = (TuplesortIndexArg *) base->arg;
	int			i;

	for (i = 0; i < count; i++)
	{
		IndexTuple	tuple;

		tuple = stups[i].tuple;
		stups[i].datum1 = index_getattr(tuple,
										1,
										RelationGetDescr(arg->indexRel),
										&stups[i].isnull1);
	}
}

static void
readtup_index(Tuplesortstate *state, SortTuple *stup,
			  LogicalTape *tape, unsigned int len)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexArg *arg = (TuplesortIndexArg *) base->arg;
	unsigned int tuplen = len - sizeof(unsigned int);
	IndexTuple	tuple = (IndexTuple) tuplesort_readtup_alloc(state, tuplen);

	LogicalTapeReadExact(tape, tuple, tuplen);
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* need trailing length word? */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value */
	stup->datum1 = index_getattr(tuple,
								 1,
								 RelationGetDescr(arg->indexRel),
								 &stup->isnull1);
}

static void
writetup_index(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	IndexTuple	tuple = (IndexTuple) stup->tuple;
	unsigned int tuplen;

	tuplen = IndexTupleSize(tuple) + sizeof(tuplen);
	LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
	LogicalTapeWrite(tape, tuple, IndexTupleSize(tuple));
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* need trailing length word? */
		LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
}

Tuplesortstate *
tuplesort_begin_index_treeb(Relation heapRel,
							Relation indexRel,
							bool enforceUnique,
							bool uniqueNullsNotDistinct,
							int workMem,
							SortCoordinate coordinate,
							int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TreebScanInsert indexScanKey;
	TuplesortIndexTreebArg *arg;
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(base->maincontext);
	arg = (TuplesortIndexTreebArg *) palloc(sizeof(TuplesortIndexTreebArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: unique = %c, workMem = %d, randomAccess = %c",
			 enforceUnique ? 't' : 'f',
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	base->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(INDEX_SORT,
								enforceUnique,
								base->nKeys,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	base->removeabbrev = removeabbrev_treeb;
	base->comparetup = comparetup_index_treeb;
	base->comparetup_tiebreak = comparetup_index_treeb_tiebreak;
	base->writetup = writetup_index;
	base->readtup = readtup_index;
	base->haveDatum1 = true;
	base->arg = arg;

	arg->index.heapRel = heapRel;
	arg->index.indexRel = indexRel;
	arg->enforceUnique = enforceUnique;
	arg->uniqueNullsNotDistinct = uniqueNullsNotDistinct;

	indexScanKey = _treeb_mkscankey(indexRel, NULL);

	/* Prepare SortSupport data for each column */
	base->sortKeys = (SortSupport) palloc0(base->nKeys *
										   sizeof(SortSupportData));

	for (i = 0; i < base->nKeys; i++)
	{
		SortSupport sortKey = base->sortKeys + i;
		ScanKey		scanKey = indexScanKey->scankeys + i;
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_TREEB_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0 && base->haveDatum1);

		Assert(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_TREEB_DESC) != 0 ?
			TreebGreaterStrategyNumber : TreebLessStrategyNumber;

		PrepareSortSupportFromTreebRel(indexRel, strategy, sortKey);
	}

	pfree(indexScanKey);

	MemoryContextSwitchTo(oldcontext);

	return state;
}
