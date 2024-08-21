/*-------------------------------------------------------------------------
 *
 * treebinsert.c
 *	  Item insertion in Lehman and Yao treebs for Postgres.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/treeb/treebinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/treeb.h"
#include "access/treebxlog.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "common/int.h"
#include "common/pg_prng.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "access/treeb_indexam.h"

/* Minimum tree height for application of fastpath optimization */
#define TREEB_FASTPATH_MIN_LEVEL	2


static TreebStack _treeb_search_insert(Relation rel, Relation heaprel,
								 TreebInsertState insertstate);
static TransactionId _treeb_check_unique(Relation rel, TreebInsertState insertstate,
									  Relation heapRel,
									  IndexUniqueCheck checkUnique, bool *is_unique,
									  uint32 *speculativeToken);
static OffsetNumber _treeb_findinsertloc(Relation rel,
									  TreebInsertState insertstate,
									  bool checkingunique,
									  bool indexUnchanged,
									  TreebStack stack,
									  Relation heapRel);
static void _treeb_stepright(Relation rel, Relation heaprel,
						  TreebInsertState insertstate, TreebStack stack);
static void _treeb_insertonpg(Relation rel, Relation heaprel, TreebScanInsert itup_key,
						   Buffer buf,
						   Buffer cbuf,
						   TreebStack stack,
						   IndexTuple itup,
						   Size itemsz,
						   OffsetNumber newitemoff,
						   int postingoff,
						   bool split_only_page);
static Buffer _treeb_split(Relation rel, Relation heaprel, TreebScanInsert itup_key,
						Buffer buf, Buffer cbuf, OffsetNumber newitemoff,
						Size newitemsz, IndexTuple newitem, IndexTuple orignewitem,
						IndexTuple nposting, uint16 postingoff);
static void _treeb_insert_parent(Relation rel, Relation heaprel, Buffer buf,
							  Buffer rbuf, TreebStack stack, bool isroot, bool isonly);
static Buffer _treeb_newlevel(Relation rel, Relation heaprel, Buffer lbuf, Buffer rbuf);
static inline bool _treeb_pgaddtup(Page page, Size itemsize, IndexTuple itup,
								OffsetNumber itup_off, bool newfirstdataitem);
static void _treeb_delete_or_dedup_one_page(Relation rel, Relation heapRel,
										 TreebInsertState insertstate,
										 bool simpleonly, bool checkingunique,
										 bool uniquedup, bool indexUnchanged);
static void _treeb_simpledel_pass(Relation rel, Buffer buffer, Relation heapRel,
							   OffsetNumber *deletable, int ndeletable,
							   IndexTuple newitem, OffsetNumber minoff,
							   OffsetNumber maxoff);
static BlockNumber *_treeb_deadblocks(Page page, OffsetNumber *deletable,
								   int ndeletable, IndexTuple newitem,
								   int *nblocks);
static inline int _treeb_blk_cmp(const void *arg1, const void *arg2);

/*
 *	_treeb_doinsert() -- Handle insertion of a single index tuple in the tree.
 *
 *		This routine is called by the public interface routine, treebinsert.
 *		By here, itup is filled in, including the TID.
 *
 *		If checkUnique is UNIQUE_CHECK_NO or UNIQUE_CHECK_PARTIAL, this
 *		will allow duplicates.  Otherwise (UNIQUE_CHECK_YES or
 *		UNIQUE_CHECK_EXISTING) it will throw error for a duplicate.
 *		For UNIQUE_CHECK_EXISTING we merely run the duplicate check, and
 *		don't actually insert.
 *
 *		indexUnchanged executor hint indicates if itup is from an
 *		UPDATE that didn't logically change the indexed value, but
 *		must nevertheless have a new entry to point to a successor
 *		version.
 *
 *		The result value is only significant for UNIQUE_CHECK_PARTIAL:
 *		it must be true if the entry is known unique, else false.
 *		(In the current implementation we'll also return true after a
 *		successful UNIQUE_CHECK_YES or UNIQUE_CHECK_EXISTING call, but
 *		that's just a coding artifact.)
 */
bool
_treeb_doinsert(Relation rel, IndexTuple itup,
			 IndexUniqueCheck checkUnique, bool indexUnchanged,
			 Relation heapRel)
{
	bool		is_unique = false;
	TreebInsertStateData insertstate;
	TreebScanInsert itup_key;
	TreebStack		stack;
	bool		checkingunique = (checkUnique != UNIQUE_CHECK_NO);

	/* we need an insertion scan key to do our search, so build one */
	itup_key = _treeb_mkscankey(rel, itup);

	if (checkingunique)
	{
		if (!itup_key->anynullkeys)
		{
			/* No (heapkeyspace) scantid until uniqueness established */
			itup_key->scantid = NULL;
		}
		else
		{
			/*
			 * Scan key for new tuple contains NULL key values.  Bypass
			 * checkingunique steps.  They are unnecessary because core code
			 * considers NULL unequal to every value, including NULL.
			 *
			 * This optimization avoids O(N^2) behavior within the
			 * _treeb_findinsertloc() heapkeyspace path when a unique index has a
			 * large number of "duplicates" with NULL key values.
			 */
			checkingunique = false;
			/* Tuple is unique in the sense that core code cares about */
			Assert(checkUnique != UNIQUE_CHECK_EXISTING);
			is_unique = true;
		}
	}

	/*
	 * Fill in the TreebInsertState working area, to track the current page and
	 * position within the page to insert on.
	 *
	 * Note that itemsz is passed down to lower level code that deals with
	 * inserting the item.  It must be MAXALIGN()'d.  This ensures that space
	 * accounting code consistently considers the alignment overhead that we
	 * expect PageAddItem() will add later.  (Actually, index_form_tuple() is
	 * already conservative about alignment, but we don't rely on that from
	 * this distance.  Besides, preserving the "true" tuple size in index
	 * tuple headers for the benefit of treebsplitloc.c might happen someday.
	 * Note that heapam does not MAXALIGN() each heap tuple's lp_len field.)
	 */
	insertstate.itup = itup;
	insertstate.itemsz = MAXALIGN(IndexTupleSize(itup));
	insertstate.itup_key = itup_key;
	insertstate.bounds_valid = false;
	insertstate.buf = InvalidBuffer;
	insertstate.postingoff = 0;

search:

	/*
	 * Find and lock the leaf page that the tuple should be added to by
	 * searching from the root page.  insertstate.buf will hold a buffer that
	 * is locked in exclusive mode afterwards.
	 */
	stack = _treeb_search_insert(rel, heapRel, &insertstate);

	/*
	 * checkingunique inserts are not allowed to go ahead when two tuples with
	 * equal key attribute values would be visible to new MVCC snapshots once
	 * the xact commits.  Check for conflicts in the locked page/buffer (if
	 * needed) here.
	 *
	 * It might be necessary to check a page to the right in _treeb_check_unique,
	 * though that should be very rare.  In practice the first page the value
	 * could be on (with scantid omitted) is almost always also the only page
	 * that a matching tuple might be found on.  This is due to the behavior
	 * of _treeb_findsplitloc with duplicate tuples -- a group of duplicates can
	 * only be allowed to cross a page boundary when there is no candidate
	 * leaf page split point that avoids it.  Also, _treeb_check_unique can use
	 * the leaf page high key to determine that there will be no duplicates on
	 * the right sibling without actually visiting it (it uses the high key in
	 * cases where the new item happens to belong at the far right of the leaf
	 * page).
	 *
	 * NOTE: obviously, _treeb_check_unique can only detect keys that are already
	 * in the index; so it cannot defend against concurrent insertions of the
	 * same key.  We protect against that by means of holding a write lock on
	 * the first page the value could be on, with omitted/-inf value for the
	 * implicit heap TID tiebreaker attribute.  Any other would-be inserter of
	 * the same key must acquire a write lock on the same page, so only one
	 * would-be inserter can be making the check at one time.  Furthermore,
	 * once we are past the check we hold write locks continuously until we
	 * have performed our insertion, so no later inserter can fail to see our
	 * insertion.  (This requires some care in _treeb_findinsertloc.)
	 *
	 * If we must wait for another xact, we release the lock while waiting,
	 * and then must perform a new search.
	 *
	 * For a partial uniqueness check, we don't wait for the other xact. Just
	 * let the tuple in and return false for possibly non-unique, or true for
	 * definitely unique.
	 */
	if (checkingunique)
	{
		TransactionId xwait;
		uint32		speculativeToken;

		xwait = _treeb_check_unique(rel, &insertstate, heapRel, checkUnique,
								 &is_unique, &speculativeToken);

		if (unlikely(TransactionIdIsValid(xwait)))
		{
			/* Have to wait for the other guy ... */
			_treeb_relbuf(rel, insertstate.buf);
			insertstate.buf = InvalidBuffer;

			/*
			 * If it's a speculative insertion, wait for it to finish (ie. to
			 * go ahead with the insertion, or kill the tuple).  Otherwise
			 * wait for the transaction to finish as usual.
			 */
			if (speculativeToken)
				SpeculativeInsertionWait(xwait, speculativeToken);
			else
				XactLockTableWait(xwait, rel, &itup->t_tid, XLTW_InsertIndex);

			/* start over... */
			if (stack)
				_treeb_freestack(stack);
			goto search;
		}

		/* Uniqueness is established -- restore heap tid as scantid */
		if (itup_key->heapkeyspace)
			itup_key->scantid = &itup->t_tid;
	}

	if (checkUnique != UNIQUE_CHECK_EXISTING)
	{
		OffsetNumber newitemoff;

		/*
		 * The only conflict predicate locking cares about for indexes is when
		 * an index tuple insert conflicts with an existing lock.  We don't
		 * know the actual page we're going to insert on for sure just yet in
		 * checkingunique and !heapkeyspace cases, but it's okay to use the
		 * first page the value could be on (with scantid omitted) instead.
		 */
		CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(insertstate.buf));

		/*
		 * Do the insertion.  Note that insertstate contains cached binary
		 * search bounds established within _treeb_check_unique when insertion is
		 * checkingunique.
		 */
		newitemoff = _treeb_findinsertloc(rel, &insertstate, checkingunique,
									   indexUnchanged, stack, heapRel);
		_treeb_insertonpg(rel, heapRel, itup_key, insertstate.buf, InvalidBuffer,
					   stack, itup, insertstate.itemsz, newitemoff,
					   insertstate.postingoff, false);
	}
	else
	{
		/* just release the buffer */
		_treeb_relbuf(rel, insertstate.buf);
	}

	/* be tidy */
	if (stack)
		_treeb_freestack(stack);
	pfree(itup_key);

	return is_unique;
}

/*
 *	_treeb_search_insert() -- _treeb_search() wrapper for inserts
 *
 * Search the tree for a particular scankey, or more precisely for the first
 * leaf page it could be on.  Try to make use of the fastpath optimization's
 * rightmost leaf page cache before actually searching the tree from the root
 * page, though.
 *
 * Return value is a stack of parent-page pointers (though see notes about
 * fastpath optimization and page splits below).  insertstate->buf is set to
 * the address of the leaf-page buffer, which is write-locked and pinned in
 * all cases (if necessary by creating a new empty root page for caller).
 *
 * The fastpath optimization avoids most of the work of searching the tree
 * repeatedly when a single backend inserts successive new tuples on the
 * rightmost leaf page of an index.  A backend cache of the rightmost leaf
 * page is maintained within _treeb_insertonpg(), and used here.  The cache is
 * invalidated here when an insert of a non-pivot tuple must take place on a
 * non-rightmost leaf page.
 *
 * The optimization helps with indexes on an auto-incremented field.  It also
 * helps with indexes on datetime columns, as well as indexes with lots of
 * NULL values.  (NULLs usually get inserted in the rightmost page for single
 * column indexes, since they usually get treated as coming after everything
 * else in the key space.  Individual NULL tuples will generally be placed on
 * the rightmost leaf page due to the influence of the heap TID column.)
 *
 * Note that we avoid applying the optimization when there is insufficient
 * space on the rightmost page to fit caller's new item.  This is necessary
 * because we'll need to return a real descent stack when a page split is
 * expected (actually, caller can cope with a leaf page split that uses a NULL
 * stack, but that's very slow and so must be avoided).  Note also that the
 * fastpath optimization acquires the lock on the page conditionally as a way
 * of reducing extra contention when there are concurrent insertions into the
 * rightmost page (we give up if we'd have to wait for the lock).  We assume
 * that it isn't useful to apply the optimization when there is contention,
 * since each per-backend cache won't stay valid for long.
 */
static TreebStack
_treeb_search_insert(Relation rel, Relation heaprel, TreebInsertState insertstate)
{
	Assert(insertstate->buf == InvalidBuffer);
	Assert(!insertstate->bounds_valid);
	Assert(insertstate->postingoff == 0);

	if (RelationGetTargetBlock(rel) != InvalidBlockNumber)
	{
		/* Simulate a _treeb_getbuf() call with conditional locking */
		insertstate->buf = ReadBuffer(rel, RelationGetTargetBlock(rel));
		if (_treeb_conditionallockbuf(rel, insertstate->buf))
		{
			Page		page;
			TreebPageOpaque opaque;

			_treeb_checkpage(rel, insertstate->buf);
			page = BufferGetPage(insertstate->buf);
			opaque = TREEBPageGetOpaque(page);

			/*
			 * Check if the page is still the rightmost leaf page and has
			 * enough free space to accommodate the new tuple.  Also check
			 * that the insertion scan key is strictly greater than the first
			 * non-pivot tuple on the page.  (Note that we expect itup_key's
			 * scantid to be unset when our caller is a checkingunique
			 * inserter.)
			 */
			if (P_RIGHTMOST(opaque) &&
				P_ISLEAF(opaque) &&
				!P_IGNORE(opaque) &&
				PageGetFreeSpace(page) > insertstate->itemsz &&
				PageGetMaxOffsetNumber(page) >= P_HIKEY &&
				_treeb_compare(rel, insertstate->itup_key, page, P_HIKEY) > 0)
			{
				/*
				 * Caller can use the fastpath optimization because cached
				 * block is still rightmost leaf page, which can fit caller's
				 * new tuple without splitting.  Keep block in local cache for
				 * next insert, and have caller use NULL stack.
				 *
				 * Note that _treeb_insert_parent() has an assertion that catches
				 * leaf page splits that somehow follow from a fastpath insert
				 * (it should only be passed a NULL stack when it must deal
				 * with a concurrent root page split, and never because a NULL
				 * stack was returned here).
				 */
				return NULL;
			}

			/* Page unsuitable for caller, drop lock and pin */
			_treeb_relbuf(rel, insertstate->buf);
		}
		else
		{
			/* Lock unavailable, drop pin */
			ReleaseBuffer(insertstate->buf);
		}

		/* Forget block, since cache doesn't appear to be useful */
		RelationSetTargetBlock(rel, InvalidBlockNumber);
	}

	/* Cannot use optimization -- descend tree, return proper descent stack */
	return _treeb_search(rel, heaprel, insertstate->itup_key, &insertstate->buf,
					  TREEB_WRITE);
}

/*
 *	_treeb_check_unique() -- Check for violation of unique index constraint
 *
 * Returns InvalidTransactionId if there is no conflict, else an xact ID
 * we must wait for to see if it commits a conflicting tuple.   If an actual
 * conflict is detected, no return --- just ereport().  If an xact ID is
 * returned, and the conflicting tuple still has a speculative insertion in
 * progress, *speculativeToken is set to non-zero, and the caller can wait for
 * the verdict on the insertion using SpeculativeInsertionWait().
 *
 * However, if checkUnique == UNIQUE_CHECK_PARTIAL, we always return
 * InvalidTransactionId because we don't want to wait.  In this case we
 * set *is_unique to false if there is a potential conflict, and the
 * core code must redo the uniqueness check later.
 *
 * As a side-effect, sets state in insertstate that can later be used by
 * _treeb_findinsertloc() to reuse most of the binary search work we do
 * here.
 *
 * This code treats NULLs as equal, unlike the default semantics for unique
 * indexes.  So do not call here when there are NULL values in scan key and
 * the index uses the default NULLS DISTINCT mode.
 */
static TransactionId
_treeb_check_unique(Relation rel, TreebInsertState insertstate, Relation heapRel,
				 IndexUniqueCheck checkUnique, bool *is_unique,
				 uint32 *speculativeToken)
{
	IndexTuple	itup = insertstate->itup;
	IndexTuple	curitup = NULL;
	ItemId		curitemid = NULL;
	TreebScanInsert itup_key = insertstate->itup_key;
	SnapshotData SnapshotDirty;
	OffsetNumber offset;
	OffsetNumber maxoff;
	Page		page;
	TreebPageOpaque opaque;
	Buffer		nbuf = InvalidBuffer;
	bool		found = false;
	bool		inposting = false;
	bool		prevalldead = true;
	int			curposti = 0;

	/* Assume unique until we find a duplicate */
	*is_unique = true;

	InitDirtySnapshot(SnapshotDirty);

	page = BufferGetPage(insertstate->buf);
	opaque = TREEBPageGetOpaque(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * Find the first tuple with the same key.
	 *
	 * This also saves the binary search bounds in insertstate.  We use them
	 * in the fastpath below, but also in the _treeb_findinsertloc() call later.
	 */
	Assert(!insertstate->bounds_valid);
	offset = _treeb_binsrch_insert(rel, insertstate);

	/*
	 * Scan over all equal tuples, looking for live conflicts.
	 */
	Assert(!insertstate->bounds_valid || insertstate->low == offset);
	Assert(!itup_key->anynullkeys);
	Assert(itup_key->scantid == NULL);
	for (;;)
	{
		/*
		 * Each iteration of the loop processes one heap TID, not one index
		 * tuple.  Current offset number for page isn't usually advanced on
		 * iterations that process heap TIDs from posting list tuples.
		 *
		 * "inposting" state is set when _inside_ a posting list --- not when
		 * we're at the start (or end) of a posting list.  We advance curposti
		 * at the end of the iteration when inside a posting list tuple.  In
		 * general, every loop iteration either advances the page offset or
		 * advances curposti --- an iteration that handles the rightmost/max
		 * heap TID in a posting list finally advances the page offset (and
		 * unsets "inposting").
		 *
		 * Make sure the offset points to an actual index tuple before trying
		 * to examine it...
		 */
		if (offset <= maxoff)
		{
			/*
			 * Fastpath: In most cases, we can use cached search bounds to
			 * limit our consideration to items that are definitely
			 * duplicates.  This fastpath doesn't apply when the original page
			 * is empty, or when initial offset is past the end of the
			 * original page, which may indicate that we need to examine a
			 * second or subsequent page.
			 *
			 * Note that this optimization allows us to avoid calling
			 * _treeb_compare() directly when there are no duplicates, as long as
			 * the offset where the key will go is not at the end of the page.
			 */
			if (nbuf == InvalidBuffer && offset == insertstate->stricthigh)
			{
				Assert(insertstate->bounds_valid);
				Assert(insertstate->low >= P_FIRSTDATAKEY(opaque));
				Assert(insertstate->low <= insertstate->stricthigh);
				Assert(_treeb_compare(rel, itup_key, page, offset) < 0);
				break;
			}

			/*
			 * We can skip items that are already marked killed.
			 *
			 * In the presence of heavy update activity an index may contain
			 * many killed items with the same key; running _treeb_compare() on
			 * each killed item gets expensive.  Just advance over killed
			 * items as quickly as we can.  We only apply _treeb_compare() when
			 * we get to a non-killed item.  We could reuse the bounds to
			 * avoid _treeb_compare() calls for known equal tuples, but it
			 * doesn't seem worth it.
			 */
			if (!inposting)
				curitemid = PageGetItemId(page, offset);
			if (inposting || !ItemIdIsDead(curitemid))
			{
				ItemPointerData htid;
				bool		all_dead = false;

				if (!inposting)
				{
					/* Plain tuple, or first TID in posting list tuple */
					if (_treeb_compare(rel, itup_key, page, offset) != 0)
						break;	/* we're past all the equal tuples */

					/* Advanced curitup */
					curitup = (IndexTuple) PageGetItem(page, curitemid);
					Assert(!TreebTupleIsPivot(curitup));
				}

				/* okay, we gotta fetch the heap tuple using htid ... */
				if (!TreebTupleIsPosting(curitup))
				{
					/* ... htid is from simple non-pivot tuple */
					Assert(!inposting);
					htid = curitup->t_tid;
				}
				else if (!inposting)
				{
					/* ... htid is first TID in new posting list */
					inposting = true;
					prevalldead = true;
					curposti = 0;
					htid = *TreebTupleGetPostingN(curitup, 0);
				}
				else
				{
					/* ... htid is second or subsequent TID in posting list */
					Assert(curposti > 0);
					htid = *TreebTupleGetPostingN(curitup, curposti);
				}

				/*
				 * If we are doing a recheck, we expect to find the tuple we
				 * are rechecking.  It's not a duplicate, but we have to keep
				 * scanning.
				 */
				if (checkUnique == UNIQUE_CHECK_EXISTING &&
					ItemPointerCompare(&htid, &itup->t_tid) == 0)
				{
					found = true;
				}

				/*
				 * Check if there's any table tuples for this index entry
				 * satisfying SnapshotDirty. This is necessary because for AMs
				 * with optimizations like heap's HOT, we have just a single
				 * index entry for the entire chain.
				 */
				else if (table_index_fetch_tuple_check(heapRel, &htid,
													   &SnapshotDirty,
													   &all_dead))
				{
					TransactionId xwait;

					/*
					 * It is a duplicate. If we are only doing a partial
					 * check, then don't bother checking if the tuple is being
					 * updated in another transaction. Just return the fact
					 * that it is a potential conflict and leave the full
					 * check till later. Don't invalidate binary search
					 * bounds.
					 */
					if (checkUnique == UNIQUE_CHECK_PARTIAL)
					{
						if (nbuf != InvalidBuffer)
							_treeb_relbuf(rel, nbuf);
						*is_unique = false;
						return InvalidTransactionId;
					}

					/*
					 * If this tuple is being updated by other transaction
					 * then we have to wait for its commit/abort.
					 */
					xwait = (TransactionIdIsValid(SnapshotDirty.xmin)) ?
						SnapshotDirty.xmin : SnapshotDirty.xmax;

					if (TransactionIdIsValid(xwait))
					{
						if (nbuf != InvalidBuffer)
							_treeb_relbuf(rel, nbuf);
						/* Tell _treeb_doinsert to wait... */
						*speculativeToken = SnapshotDirty.speculativeToken;
						/* Caller releases lock on buf immediately */
						insertstate->bounds_valid = false;
						return xwait;
					}

					/*
					 * Otherwise we have a definite conflict.  But before
					 * complaining, look to see if the tuple we want to insert
					 * is itself now committed dead --- if so, don't complain.
					 * This is a waste of time in normal scenarios but we must
					 * do it to support CREATE INDEX CONCURRENTLY.
					 *
					 * We must follow HOT-chains here because during
					 * concurrent index build, we insert the root TID though
					 * the actual tuple may be somewhere in the HOT-chain.
					 * While following the chain we might not stop at the
					 * exact tuple which triggered the insert, but that's OK
					 * because if we find a live tuple anywhere in this chain,
					 * we have a unique key conflict.  The other live tuple is
					 * not part of this chain because it had a different index
					 * entry.
					 */
					htid = itup->t_tid;
					if (table_index_fetch_tuple_check(heapRel, &htid,
													  SnapshotSelf, NULL))
					{
						/* Normal case --- it's still live */
					}
					else
					{
						/*
						 * It's been deleted, so no error, and no need to
						 * continue searching
						 */
						break;
					}

					/*
					 * Check for a conflict-in as we would if we were going to
					 * write to this page.  We aren't actually going to write,
					 * but we want a chance to report SSI conflicts that would
					 * otherwise be masked by this unique constraint
					 * violation.
					 */
					CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(insertstate->buf));

					/*
					 * This is a definite conflict.  Break the tuple down into
					 * datums and report the error.  But first, make sure we
					 * release the buffer locks we're holding ---
					 * BuildIndexValueDescription could make catalog accesses,
					 * which in the worst case might touch this same index and
					 * cause deadlocks.
					 */
					if (nbuf != InvalidBuffer)
						_treeb_relbuf(rel, nbuf);
					_treeb_relbuf(rel, insertstate->buf);
					insertstate->buf = InvalidBuffer;
					insertstate->bounds_valid = false;

					{
						Datum		values[INDEX_MAX_KEYS];
						bool		isnull[INDEX_MAX_KEYS];
						char	   *key_desc;

						index_deform_tuple(itup, RelationGetDescr(rel),
										   values, isnull);

						key_desc = BuildIndexValueDescription(rel, values,
															  isnull);

						ereport(ERROR,
								(errcode(ERRCODE_UNIQUE_VIOLATION),
								 errmsg("duplicate key value violates unique constraint \"%s\"",
										RelationGetRelationName(rel)),
								 key_desc ? errdetail("Key %s already exists.",
													  key_desc) : 0,
								 errtableconstraint(heapRel,
													RelationGetRelationName(rel))));
					}
				}
				else if (all_dead && (!inposting ||
									  (prevalldead &&
									   curposti == TreebTupleGetNPosting(curitup) - 1)))
				{
					/*
					 * The conflicting tuple (or all HOT chains pointed to by
					 * all posting list TIDs) is dead to everyone, so mark the
					 * index entry killed.
					 */
					ItemIdMarkDead(curitemid);
					opaque->treebpo_flags |= TREEBP_HAS_GARBAGE;

					/*
					 * Mark buffer with a dirty hint, since state is not
					 * crucial. Be sure to mark the proper buffer dirty.
					 */
					if (nbuf != InvalidBuffer)
						MarkBufferDirtyHint(nbuf, true);
					else
						MarkBufferDirtyHint(insertstate->buf, true);
				}

				/*
				 * Remember if posting list tuple has even a single HOT chain
				 * whose members are not all dead
				 */
				if (!all_dead && inposting)
					prevalldead = false;
			}
		}

		if (inposting && curposti < TreebTupleGetNPosting(curitup) - 1)
		{
			/* Advance to next TID in same posting list */
			curposti++;
			continue;
		}
		else if (offset < maxoff)
		{
			/* Advance to next tuple */
			curposti = 0;
			inposting = false;
			offset = OffsetNumberNext(offset);
		}
		else
		{
			int			highkeycmp;

			/* If scankey == hikey we gotta check the next page too */
			if (P_RIGHTMOST(opaque))
				break;
			highkeycmp = _treeb_compare(rel, itup_key, page, P_HIKEY);
			Assert(highkeycmp <= 0);
			if (highkeycmp != 0)
				break;
			/* Advance to next non-dead page --- there must be one */
			for (;;)
			{
				BlockNumber nblkno = opaque->treebpo_next;

				nbuf = _treeb_relandgetbuf(rel, nbuf, nblkno, TREEB_READ);
				page = BufferGetPage(nbuf);
				opaque = TREEBPageGetOpaque(page);
				if (!P_IGNORE(opaque))
					break;
				if (P_RIGHTMOST(opaque))
					elog(ERROR, "fell off the end of index \"%s\"",
						 RelationGetRelationName(rel));
			}
			/* Will also advance to next tuple */
			curposti = 0;
			inposting = false;
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
			/* Don't invalidate binary search bounds */
		}
	}

	/*
	 * If we are doing a recheck then we should have found the tuple we are
	 * checking.  Otherwise there's something very wrong --- probably, the
	 * index is on a non-immutable expression.
	 */
	if (checkUnique == UNIQUE_CHECK_EXISTING && !found)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to re-find tuple within index \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("This may be because of a non-immutable index expression."),
				 errtableconstraint(heapRel,
									RelationGetRelationName(rel))));

	if (nbuf != InvalidBuffer)
		_treeb_relbuf(rel, nbuf);

	return InvalidTransactionId;
}


/*
 *	_treeb_findinsertloc() -- Finds an insert location for a tuple
 *
 *		On entry, insertstate buffer contains the page the new tuple belongs
 *		on.  It is exclusive-locked and pinned by the caller.
 *
 *		If 'checkingunique' is true, the buffer on entry is the first page
 *		that contains duplicates of the new key.  If there are duplicates on
 *		multiple pages, the correct insertion position might be some page to
 *		the right, rather than the first page.  In that case, this function
 *		moves right to the correct target page.
 *
 *		(In a !heapkeyspace index, there can be multiple pages with the same
 *		high key, where the new tuple could legitimately be placed on.  In
 *		that case, the caller passes the first page containing duplicates,
 *		just like when checkingunique=true.  If that page doesn't have enough
 *		room for the new tuple, this function moves right, trying to find a
 *		legal page that does.)
 *
 *		If 'indexUnchanged' is true, this is for an UPDATE that didn't
 *		logically change the indexed value, but must nevertheless have a new
 *		entry to point to a successor version.  This hint from the executor
 *		will influence our behavior when the page might have to be split and
 *		we must consider our options.  Bottom-up index deletion can avoid
 *		pathological version-driven page splits, but we only want to go to the
 *		trouble of trying it when we already have moderate confidence that
 *		it's appropriate.  The hint should not significantly affect our
 *		behavior over time unless practically all inserts on to the leaf page
 *		get the hint.
 *
 *		On exit, insertstate buffer contains the chosen insertion page, and
 *		the offset within that page is returned.  If _treeb_findinsertloc needed
 *		to move right, the lock and pin on the original page are released, and
 *		the new buffer is exclusively locked and pinned instead.
 *
 *		If insertstate contains cached binary search bounds, we will take
 *		advantage of them.  This avoids repeating comparisons that we made in
 *		_treeb_check_unique() already.
 */
static OffsetNumber
_treeb_findinsertloc(Relation rel,
				  TreebInsertState insertstate,
				  bool checkingunique,
				  bool indexUnchanged,
				  TreebStack stack,
				  Relation heapRel)
{
	TreebScanInsert itup_key = insertstate->itup_key;
	Page		page = BufferGetPage(insertstate->buf);
	TreebPageOpaque opaque;
	OffsetNumber newitemoff;

	opaque = TREEBPageGetOpaque(page);

	/* Check 1/3 of a page restriction */
	if (unlikely(insertstate->itemsz > TreebMaxItemSize(page)))
		_treeb_check_third_page(rel, heapRel, itup_key->heapkeyspace, page,
							 insertstate->itup);

	Assert(P_ISLEAF(opaque) && !P_INCOMPLETE_SPLIT(opaque));
	Assert(!insertstate->bounds_valid || checkingunique);
	Assert(!itup_key->heapkeyspace || itup_key->scantid != NULL);
	Assert(itup_key->heapkeyspace || itup_key->scantid == NULL);
	Assert(!itup_key->allequalimage || itup_key->heapkeyspace);

	if (itup_key->heapkeyspace)
	{
		/* Keep track of whether checkingunique duplicate seen */
		bool		uniquedup = indexUnchanged;

		/*
		 * If we're inserting into a unique index, we may have to walk right
		 * through leaf pages to find the one leaf page that we must insert on
		 * to.
		 *
		 * This is needed for checkingunique callers because a scantid was not
		 * used when we called _treeb_search().  scantid can only be set after
		 * _treeb_check_unique() has checked for duplicates.  The buffer
		 * initially stored in insertstate->buf has the page where the first
		 * duplicate key might be found, which isn't always the page that new
		 * tuple belongs on.  The heap TID attribute for new tuple (scantid)
		 * could force us to insert on a sibling page, though that should be
		 * very rare in practice.
		 */
		if (checkingunique)
		{
			if (insertstate->low < insertstate->stricthigh)
			{
				/* Encountered a duplicate in _treeb_check_unique() */
				Assert(insertstate->bounds_valid);
				uniquedup = true;
			}

			for (;;)
			{
				/*
				 * Does the new tuple belong on this page?
				 *
				 * The earlier _treeb_check_unique() call may well have
				 * established a strict upper bound on the offset for the new
				 * item.  If it's not the last item of the page (i.e. if there
				 * is at least one tuple on the page that goes after the tuple
				 * we're inserting) then we know that the tuple belongs on
				 * this page.  We can skip the high key check.
				 */
				if (insertstate->bounds_valid &&
					insertstate->low <= insertstate->stricthigh &&
					insertstate->stricthigh <= PageGetMaxOffsetNumber(page))
					break;

				/* Test '<=', not '!=', since scantid is set now */
				if (P_RIGHTMOST(opaque) ||
					_treeb_compare(rel, itup_key, page, P_HIKEY) <= 0)
					break;

				_treeb_stepright(rel, heapRel, insertstate, stack);
				/* Update local state after stepping right */
				page = BufferGetPage(insertstate->buf);
				opaque = TREEBPageGetOpaque(page);
				/* Assume duplicates (if checkingunique) */
				uniquedup = true;
			}
		}

		/*
		 * If the target page cannot fit newitem, try to avoid splitting the
		 * page on insert by performing deletion or deduplication now
		 */
		if (PageGetFreeSpace(page) < insertstate->itemsz)
			_treeb_delete_or_dedup_one_page(rel, heapRel, insertstate, false,
										 checkingunique, uniquedup,
										 indexUnchanged);
	}
	else
	{
		/*----------
		 * This is a !heapkeyspace (version 2 or 3) index.  The current page
		 * is the first page that we could insert the new tuple to, but there
		 * may be other pages to the right that we could opt to use instead.
		 *
		 * If the new key is equal to one or more existing keys, we can
		 * legitimately place it anywhere in the series of equal keys.  In
		 * fact, if the new key is equal to the page's "high key" we can place
		 * it on the next page.  If it is equal to the high key, and there's
		 * not room to insert the new tuple on the current page without
		 * splitting, then we move right hoping to find more free space and
		 * avoid a split.
		 *
		 * Keep scanning right until we
		 *		(a) find a page with enough free space,
		 *		(b) reach the last page where the tuple can legally go, or
		 *		(c) get tired of searching.
		 * (c) is not flippant; it is important because if there are many
		 * pages' worth of equal keys, it's better to split one of the early
		 * pages than to scan all the way to the end of the run of equal keys
		 * on every insert.  We implement "get tired" as a random choice,
		 * since stopping after scanning a fixed number of pages wouldn't work
		 * well (we'd never reach the right-hand side of previously split
		 * pages).  The probability of moving right is set at 0.99, which may
		 * seem too high to change the behavior much, but it does an excellent
		 * job of preventing O(N^2) behavior with many equal keys.
		 *----------
		 */
		while (PageGetFreeSpace(page) < insertstate->itemsz)
		{
			/*
			 * Before considering moving right, see if we can obtain enough
			 * space by erasing LP_DEAD items
			 */
			if (P_HAS_GARBAGE(opaque))
			{
				/* Perform simple deletion */
				_treeb_delete_or_dedup_one_page(rel, heapRel, insertstate, true,
											 false, false, false);

				if (PageGetFreeSpace(page) >= insertstate->itemsz)
					break;		/* OK, now we have enough space */
			}

			/*
			 * Nope, so check conditions (b) and (c) enumerated above
			 *
			 * The earlier _treeb_check_unique() call may well have established a
			 * strict upper bound on the offset for the new item.  If it's not
			 * the last item of the page (i.e. if there is at least one tuple
			 * on the page that's greater than the tuple we're inserting to)
			 * then we know that the tuple belongs on this page.  We can skip
			 * the high key check.
			 */
			if (insertstate->bounds_valid &&
				insertstate->low <= insertstate->stricthigh &&
				insertstate->stricthigh <= PageGetMaxOffsetNumber(page))
				break;

			if (P_RIGHTMOST(opaque) ||
				_treeb_compare(rel, itup_key, page, P_HIKEY) != 0 ||
				pg_prng_uint32(&pg_global_prng_state) <= (PG_UINT32_MAX / 100))
				break;

			_treeb_stepright(rel, heapRel, insertstate, stack);
			/* Update local state after stepping right */
			page = BufferGetPage(insertstate->buf);
			opaque = TREEBPageGetOpaque(page);
		}
	}

	/*
	 * We should now be on the correct page.  Find the offset within the page
	 * for the new tuple. (Possibly reusing earlier search bounds.)
	 */
	Assert(P_RIGHTMOST(opaque) ||
		   _treeb_compare(rel, itup_key, page, P_HIKEY) <= 0);

	newitemoff = _treeb_binsrch_insert(rel, insertstate);

	if (insertstate->postingoff == -1)
	{
		/*
		 * There is an overlapping posting list tuple with its LP_DEAD bit
		 * set.  We don't want to unnecessarily unset its LP_DEAD bit while
		 * performing a posting list split, so perform simple index tuple
		 * deletion early.
		 */
		_treeb_delete_or_dedup_one_page(rel, heapRel, insertstate, true,
									 false, false, false);

		/*
		 * Do new binary search.  New insert location cannot overlap with any
		 * posting list now.
		 */
		Assert(!insertstate->bounds_valid);
		insertstate->postingoff = 0;
		newitemoff = _treeb_binsrch_insert(rel, insertstate);
		Assert(insertstate->postingoff == 0);
	}

	return newitemoff;
}

/*
 * Step right to next non-dead page, during insertion.
 *
 * This is a bit more complicated than moving right in a search.  We must
 * write-lock the target page before releasing write lock on current page;
 * else someone else's _treeb_check_unique scan could fail to see our insertion.
 * Write locks on intermediate dead pages won't do because we don't know when
 * they will get de-linked from the tree.
 *
 * This is more aggressive than it needs to be for non-unique !heapkeyspace
 * indexes.
 */
static void
_treeb_stepright(Relation rel, Relation heaprel, TreebInsertState insertstate,
			  TreebStack stack)
{
	Page		page;
	TreebPageOpaque opaque;
	Buffer		rbuf;
	BlockNumber rblkno;

	Assert(heaprel != NULL);
	page = BufferGetPage(insertstate->buf);
	opaque = TREEBPageGetOpaque(page);

	rbuf = InvalidBuffer;
	rblkno = opaque->treebpo_next;
	for (;;)
	{
		rbuf = _treeb_relandgetbuf(rel, rbuf, rblkno, TREEB_WRITE);
		page = BufferGetPage(rbuf);
		opaque = TREEBPageGetOpaque(page);

		/*
		 * If this page was incompletely split, finish the split now.  We do
		 * this while holding a lock on the left sibling, which is not good
		 * because finishing the split could be a fairly lengthy operation.
		 * But this should happen very seldom.
		 */
		if (P_INCOMPLETE_SPLIT(opaque))
		{
			_treeb_finish_split(rel, heaprel, rbuf, stack);
			rbuf = InvalidBuffer;
			continue;
		}

		if (!P_IGNORE(opaque))
			break;
		if (P_RIGHTMOST(opaque))
			elog(ERROR, "fell off the end of index \"%s\"",
				 RelationGetRelationName(rel));

		rblkno = opaque->treebpo_next;
	}
	/* rbuf locked; unlock buf, update state for caller */
	_treeb_relbuf(rel, insertstate->buf);
	insertstate->buf = rbuf;
	insertstate->bounds_valid = false;
}

/*----------
 *	_treeb_insertonpg() -- Insert a tuple on a particular page in the index.
 *
 *		This recursive procedure does the following things:
 *
 *			+  if postingoff != 0, splits existing posting list tuple
 *			   (since it overlaps with new 'itup' tuple).
 *			+  if necessary, splits the target page, using 'itup_key' for
 *			   suffix truncation on leaf pages (caller passes NULL for
 *			   non-leaf pages).
 *			+  inserts the new tuple (might be split from posting list).
 *			+  if the page was split, pops the parent stack, and finds the
 *			   right place to insert the new child pointer (by walking
 *			   right using information stored in the parent stack).
 *			+  invokes itself with the appropriate tuple for the right
 *			   child page on the parent.
 *			+  updates the metapage if a true root or fast root is split.
 *
 *		On entry, we must have the correct buffer in which to do the
 *		insertion, and the buffer must be pinned and write-locked.  On return,
 *		we will have dropped both the pin and the lock on the buffer.
 *
 *		This routine only performs retail tuple insertions.  'itup' should
 *		always be either a non-highkey leaf item, or a downlink (new high
 *		key items are created indirectly, when a page is split).  When
 *		inserting to a non-leaf page, 'cbuf' is the left-sibling of the page
 *		we're inserting the downlink for.  This function will clear the
 *		INCOMPLETE_SPLIT flag on it, and release the buffer.
 *----------
 */
static void
_treeb_insertonpg(Relation rel,
			   Relation heaprel,
			   TreebScanInsert itup_key,
			   Buffer buf,
			   Buffer cbuf,
			   TreebStack stack,
			   IndexTuple itup,
			   Size itemsz,
			   OffsetNumber newitemoff,
			   int postingoff,
			   bool split_only_page)
{
	Page		page;
	TreebPageOpaque opaque;
	bool		isleaf,
				isroot,
				isrightmost,
				isonly;
	IndexTuple	oposting = NULL;
	IndexTuple	origitup = NULL;
	IndexTuple	nposting = NULL;

	page = BufferGetPage(buf);
	opaque = TREEBPageGetOpaque(page);
	isleaf = P_ISLEAF(opaque);
	isroot = P_ISROOT(opaque);
	isrightmost = P_RIGHTMOST(opaque);
	isonly = P_LEFTMOST(opaque) && P_RIGHTMOST(opaque);

	/* child buffer must be given iff inserting on an internal page */
	Assert(isleaf == !BufferIsValid(cbuf));
	/* tuple must have appropriate number of attributes */
	Assert(!isleaf ||
		   TreebTupleGetNAtts(itup, rel) ==
		   IndexRelationGetNumberOfAttributes(rel));
	Assert(isleaf ||
		   TreebTupleGetNAtts(itup, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(!TreebTupleIsPosting(itup));
	Assert(MAXALIGN(IndexTupleSize(itup)) == itemsz);
	/* Caller must always finish incomplete split for us */
	Assert(!P_INCOMPLETE_SPLIT(opaque));

	/*
	 * Every internal page should have exactly one negative infinity item at
	 * all times.  Only _treeb_split() and _treeb_newlevel() should add items that
	 * become negative infinity items through truncation, since they're the
	 * only routines that allocate new internal pages.
	 */
	Assert(isleaf || newitemoff > P_FIRSTDATAKEY(opaque));

	/*
	 * Do we need to split an existing posting list item?
	 */
	if (postingoff != 0)
	{
		ItemId		itemid = PageGetItemId(page, newitemoff);

		/*
		 * The new tuple is a duplicate with a heap TID that falls inside the
		 * range of an existing posting list tuple on a leaf page.  Prepare to
		 * split an existing posting list.  Overwriting the posting list with
		 * its post-split version is treated as an extra step in either the
		 * insert or page split critical section.
		 */
		Assert(isleaf && itup_key->heapkeyspace && itup_key->allequalimage);
		oposting = (IndexTuple) PageGetItem(page, itemid);

		/*
		 * postingoff value comes from earlier call to _treeb_binsrch_posting().
		 * Its binary search might think that a plain tuple must be a posting
		 * list tuple that needs to be split.  This can happen with corruption
		 * involving an existing plain tuple that is a duplicate of the new
		 * item, up to and including its table TID.  Check for that here in
		 * passing.
		 *
		 * Also verify that our caller has made sure that the existing posting
		 * list tuple does not have its LP_DEAD bit set.
		 */
		if (!TreebTupleIsPosting(oposting) || ItemIdIsDead(itemid))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("table tid from new index tuple (%u,%u) overlaps with invalid duplicate tuple at offset %u of block %u in index \"%s\"",
									 ItemPointerGetBlockNumber(&itup->t_tid),
									 ItemPointerGetOffsetNumber(&itup->t_tid),
									 newitemoff, BufferGetBlockNumber(buf),
									 RelationGetRelationName(rel))));

		/* use a mutable copy of itup as our itup from here on */
		origitup = itup;
		itup = CopyIndexTuple(origitup);
		nposting = _treeb_swap_posting(itup, oposting, postingoff);
		/* itup now contains rightmost/max TID from oposting */

		/* Alter offset so that newitem goes after posting list */
		newitemoff = OffsetNumberNext(newitemoff);
	}

	/*
	 * Do we need to split the page to fit the item on it?
	 *
	 * Note: PageGetFreeSpace() subtracts sizeof(ItemIdData) from its result,
	 * so this comparison is correct even though we appear to be accounting
	 * only for the item and not for its line pointer.
	 */
	if (PageGetFreeSpace(page) < itemsz)
	{
		Buffer		rbuf;

		Assert(!split_only_page);

		/* split the buffer into left and right halves */
		rbuf = _treeb_split(rel, heaprel, itup_key, buf, cbuf, newitemoff, itemsz,
						 itup, origitup, nposting, postingoff);
		PredicateLockPageSplit(rel,
							   BufferGetBlockNumber(buf),
							   BufferGetBlockNumber(rbuf));

		/*----------
		 * By here,
		 *
		 *		+  our target page has been split;
		 *		+  the original tuple has been inserted;
		 *		+  we have write locks on both the old (left half)
		 *		   and new (right half) buffers, after the split; and
		 *		+  we know the key we want to insert into the parent
		 *		   (it's the "high key" on the left child page).
		 *
		 * We're ready to do the parent insertion.  We need to hold onto the
		 * locks for the child pages until we locate the parent, but we can
		 * at least release the lock on the right child before doing the
		 * actual insertion.  The lock on the left child will be released
		 * last of all by parent insertion, where it is the 'cbuf' of parent
		 * page.
		 *----------
		 */
		_treeb_insert_parent(rel, heaprel, buf, rbuf, stack, isroot, isonly);
	}
	else
	{
		Buffer		metabuf = InvalidBuffer;
		Page		metapg = NULL;
		TreebMetaPageData *metad = NULL;
		BlockNumber blockcache;

		/*
		 * If we are doing this insert because we split a page that was the
		 * only one on its tree level, but was not the root, it may have been
		 * the "fast root".  We need to ensure that the fast root link points
		 * at or above the current page.  We can safely acquire a lock on the
		 * metapage here --- see comments for _treeb_newlevel().
		 */
		if (unlikely(split_only_page))
		{
			Assert(!isleaf);
			Assert(BufferIsValid(cbuf));

			metabuf = _treeb_getbuf(rel, TREEB_METAPAGE, TREEB_WRITE);
			metapg = BufferGetPage(metabuf);
			metad = TreebPageGetMeta(metapg);

			if (metad->treebm_fastlevel >= opaque->treebpo_level)
			{
				/* no update wanted */
				_treeb_relbuf(rel, metabuf);
				metabuf = InvalidBuffer;
			}
		}

		/* Do the update.  No ereport(ERROR) until changes are logged */
		START_CRIT_SECTION();

		if (postingoff != 0)
			memcpy(oposting, nposting, MAXALIGN(IndexTupleSize(nposting)));

		if (PageAddItem(page, (Item) itup, itemsz, newitemoff, false,
						false) == InvalidOffsetNumber)
			elog(PANIC, "failed to add new item to block %u in index \"%s\"",
				 BufferGetBlockNumber(buf), RelationGetRelationName(rel));

		MarkBufferDirty(buf);

		if (BufferIsValid(metabuf))
		{
			/* upgrade meta-page if needed */
			if (metad->treebm_version < TREEB_NOVAC_VERSION)
				_treeb_upgrademetapage(metapg);
			metad->treebm_fastroot = BufferGetBlockNumber(buf);
			metad->treebm_fastlevel = opaque->treebpo_level;
			MarkBufferDirty(metabuf);
		}

		/*
		 * Clear INCOMPLETE_SPLIT flag on child if inserting the new item
		 * finishes a split
		 */
		if (!isleaf)
		{
			Page		cpage = BufferGetPage(cbuf);
			TreebPageOpaque cpageop = TREEBPageGetOpaque(cpage);

			Assert(P_INCOMPLETE_SPLIT(cpageop));
			cpageop->treebpo_flags &= ~TREEBP_INCOMPLETE_SPLIT;
			MarkBufferDirty(cbuf);
		}

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_treeb_insert xlrec;
			xl_treeb_metadata xlmeta;
			uint8		xlinfo;
			XLogRecPtr	recptr;
			uint16		upostingoff;

			xlrec.offnum = newitemoff;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfTreebInsert);

			if (isleaf && postingoff == 0)
			{
				/* Simple leaf insert */
				xlinfo = XLOG_TREEB_INSERT_LEAF;
			}
			else if (postingoff != 0)
			{
				/*
				 * Leaf insert with posting list split.  Must include
				 * postingoff field before newitem/orignewitem.
				 */
				Assert(isleaf);
				xlinfo = XLOG_TREEB_INSERT_POST;
			}
			else
			{
				/* Internal page insert, which finishes a split on cbuf */
				xlinfo = XLOG_TREEB_INSERT_UPPER;
				XLogRegisterBuffer(1, cbuf, REGBUF_STANDARD);

				if (BufferIsValid(metabuf))
				{
					/* Actually, it's an internal page insert + meta update */
					xlinfo = XLOG_TREEB_INSERT_META;

					Assert(metad->treebm_version >= TREEB_NOVAC_VERSION);
					xlmeta.version = metad->treebm_version;
					xlmeta.root = metad->treebm_root;
					xlmeta.level = metad->treebm_level;
					xlmeta.fastroot = metad->treebm_fastroot;
					xlmeta.fastlevel = metad->treebm_fastlevel;
					xlmeta.last_cleanup_num_delpages = metad->treebm_last_cleanup_num_delpages;
					xlmeta.allequalimage = metad->treebm_allequalimage;

					XLogRegisterBuffer(2, metabuf,
									   REGBUF_WILL_INIT | REGBUF_STANDARD);
					XLogRegisterBufData(2, (char *) &xlmeta,
										sizeof(xl_treeb_metadata));
				}
			}

			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			if (postingoff == 0)
			{
				/* Just log itup from caller */
				XLogRegisterBufData(0, (char *) itup, IndexTupleSize(itup));
			}
			else
			{
				/*
				 * Insert with posting list split (XLOG_TREEB_INSERT_POST
				 * record) case.
				 *
				 * Log postingoff.  Also log origitup, not itup.  REDO routine
				 * must reconstruct final itup (as well as nposting) using
				 * _treeb_swap_posting().
				 */
				upostingoff = postingoff;

				XLogRegisterBufData(0, (char *) &upostingoff, sizeof(uint16));
				XLogRegisterBufData(0, (char *) origitup,
									IndexTupleSize(origitup));
			}

			recptr = XLogInsert(RM_TREEB_ID, xlinfo);

			if (BufferIsValid(metabuf))
				PageSetLSN(metapg, recptr);
			if (!isleaf)
				PageSetLSN(BufferGetPage(cbuf), recptr);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/* Release subsidiary buffers */
		if (BufferIsValid(metabuf))
			_treeb_relbuf(rel, metabuf);
		if (!isleaf)
			_treeb_relbuf(rel, cbuf);

		/*
		 * Cache the block number if this is the rightmost leaf page.  Cache
		 * may be used by a future inserter within _treeb_search_insert().
		 */
		blockcache = InvalidBlockNumber;
		if (isrightmost && isleaf && !isroot)
			blockcache = BufferGetBlockNumber(buf);

		/* Release buffer for insertion target block */
		_treeb_relbuf(rel, buf);

		/*
		 * If we decided to cache the insertion target block before releasing
		 * its buffer lock, then cache it now.  Check the height of the tree
		 * first, though.  We don't go for the optimization with small
		 * indexes.  Defer final check to this point to ensure that we don't
		 * call _treeb_getrootheight while holding a buffer lock.
		 */
		if (BlockNumberIsValid(blockcache) &&
			_treeb_getrootheight(rel) >= TREEB_FASTPATH_MIN_LEVEL)
			RelationSetTargetBlock(rel, blockcache);
	}

	/* be tidy */
	if (postingoff != 0)
	{
		/* itup is actually a modified copy of caller's original */
		pfree(nposting);
		pfree(itup);
	}
}

/*
 *	_treeb_split() -- split a page in the treeb.
 *
 *		On entry, buf is the page to split, and is pinned and write-locked.
 *		newitemoff etc. tell us about the new item that must be inserted
 *		along with the data from the original page.
 *
 *		itup_key is used for suffix truncation on leaf pages (internal
 *		page callers pass NULL).  When splitting a non-leaf page, 'cbuf'
 *		is the left-sibling of the page we're inserting the downlink for.
 *		This function will clear the INCOMPLETE_SPLIT flag on it, and
 *		release the buffer.
 *
 *		orignewitem, nposting, and postingoff are needed when an insert of
 *		orignewitem results in both a posting list split and a page split.
 *		These extra posting list split details are used here in the same
 *		way as they are used in the more common case where a posting list
 *		split does not coincide with a page split.  We need to deal with
 *		posting list splits directly in order to ensure that everything
 *		that follows from the insert of orignewitem is handled as a single
 *		atomic operation (though caller's insert of a new pivot/downlink
 *		into parent page will still be a separate operation).  See
 *		treeb/README for details on the design of posting list splits.
 *
 *		Returns the new right sibling of buf, pinned and write-locked.
 *		The pin and lock on buf are maintained.
 */
static Buffer
_treeb_split(Relation rel, Relation heaprel, TreebScanInsert itup_key, Buffer buf,
		  Buffer cbuf, OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
		  IndexTuple orignewitem, IndexTuple nposting, uint16 postingoff)
{
	Buffer		rbuf;
	Page		origpage;
	Page		leftpage,
				rightpage;
	BlockNumber origpagenumber,
				rightpagenumber;
	TreebPageOpaque ropaque,
				lopaque,
				oopaque;
	Buffer		sbuf = InvalidBuffer;
	Page		spage = NULL;
	TreebPageOpaque sopaque = NULL;
	Size		itemsz;
	ItemId		itemid;
	IndexTuple	firstright,
				lefthighkey;
	OffsetNumber firstrightoff;
	OffsetNumber afterleftoff,
				afterrightoff,
				minusinfoff;
	OffsetNumber origpagepostingoff;
	OffsetNumber maxoff;
	OffsetNumber i;
	bool		newitemonleft,
				isleaf,
				isrightmost;

	/*
	 * origpage is the original page to be split.  leftpage is a temporary
	 * buffer that receives the left-sibling data, which will be copied back
	 * into origpage on success.  rightpage is the new page that will receive
	 * the right-sibling data.
	 *
	 * leftpage is allocated after choosing a split point.  rightpage's new
	 * buffer isn't acquired until after leftpage is initialized and has new
	 * high key, the last point where splitting the page may fail (barring
	 * corruption).  Failing before acquiring new buffer won't have lasting
	 * consequences, since origpage won't have been modified and leftpage is
	 * only workspace.
	 */
	origpage = BufferGetPage(buf);
	oopaque = TREEBPageGetOpaque(origpage);
	isleaf = P_ISLEAF(oopaque);
	isrightmost = P_RIGHTMOST(oopaque);
	maxoff = PageGetMaxOffsetNumber(origpage);
	origpagenumber = BufferGetBlockNumber(buf);

	/*
	 * Choose a point to split origpage at.
	 *
	 * A split point can be thought of as a point _between_ two existing data
	 * items on origpage (the lastleft and firstright tuples), provided you
	 * pretend that the new item that didn't fit is already on origpage.
	 *
	 * Since origpage does not actually contain newitem, the representation of
	 * split points needs to work with two boundary cases: splits where
	 * newitem is lastleft, and splits where newitem is firstright.
	 * newitemonleft resolves the ambiguity that would otherwise exist when
	 * newitemoff == firstrightoff.  In all other cases it's clear which side
	 * of the split every tuple goes on from context.  newitemonleft is
	 * usually (but not always) redundant information.
	 *
	 * firstrightoff is supposed to be an origpage offset number, but it's
	 * possible that its value will be maxoff+1, which is "past the end" of
	 * origpage.  This happens in the rare case where newitem goes after all
	 * existing items (i.e. newitemoff is maxoff+1) and we end up splitting
	 * origpage at the point that leaves newitem alone on new right page.  Any
	 * "!newitemonleft && newitemoff == firstrightoff" split point makes
	 * newitem the firstright tuple, though, so this case isn't a special
	 * case.
	 */
	firstrightoff = _treeb_findsplitloc(rel, origpage, newitemoff, newitemsz,
									 newitem, &newitemonleft);

	/* Allocate temp buffer for leftpage */
	leftpage = PageGetTempPage(origpage);
	_treeb_pageinit(leftpage, BufferGetPageSize(buf));
	lopaque = TREEBPageGetOpaque(leftpage);

	/*
	 * leftpage won't be the root when we're done.  Also, clear the SPLIT_END
	 * and HAS_GARBAGE flags.
	 */
	lopaque->treebpo_flags = oopaque->treebpo_flags;
	lopaque->treebpo_flags &= ~(TREEBP_ROOT | TREEBP_SPLIT_END | TREEBP_HAS_GARBAGE);
	/* set flag in leftpage indicating that rightpage has no downlink yet */
	lopaque->treebpo_flags |= TREEBP_INCOMPLETE_SPLIT;
	lopaque->treebpo_prev = oopaque->treebpo_prev;
	/* handle treebpo_next after rightpage buffer acquired */
	lopaque->treebpo_level = oopaque->treebpo_level;
	/* handle treebpo_cycleid after rightpage buffer acquired */

	/*
	 * Copy the original page's LSN into leftpage, which will become the
	 * updated version of the page.  We need this because XLogInsert will
	 * examine the LSN and possibly dump it in a page image.
	 */
	PageSetLSN(leftpage, PageGetLSN(origpage));

	/*
	 * Determine page offset number of existing overlapped-with-orignewitem
	 * posting list when it is necessary to perform a posting list split in
	 * passing.  Note that newitem was already changed by caller (newitem no
	 * longer has the orignewitem TID).
	 *
	 * This page offset number (origpagepostingoff) will be used to pretend
	 * that the posting split has already taken place, even though the
	 * required modifications to origpage won't occur until we reach the
	 * critical section.  The lastleft and firstright tuples of our page split
	 * point should, in effect, come from an imaginary version of origpage
	 * that has the nposting tuple instead of the original posting list tuple.
	 *
	 * Note: _treeb_findsplitloc() should have compensated for coinciding posting
	 * list splits in just the same way, at least in theory.  It doesn't
	 * bother with that, though.  In practice it won't affect its choice of
	 * split point.
	 */
	origpagepostingoff = InvalidOffsetNumber;
	if (postingoff != 0)
	{
		Assert(isleaf);
		Assert(ItemPointerCompare(&orignewitem->t_tid,
								  &newitem->t_tid) < 0);
		Assert(TreebTupleIsPosting(nposting));
		origpagepostingoff = OffsetNumberPrev(newitemoff);
	}

	/*
	 * The high key for the new left page is a possibly-truncated copy of
	 * firstright on the leaf level (it's "firstright itself" on internal
	 * pages; see !isleaf comments below).  This may seem to be contrary to
	 * Lehman & Yao's approach of using a copy of lastleft as the new high key
	 * when splitting on the leaf level.  It isn't, though.
	 *
	 * Suffix truncation will leave the left page's high key fully equal to
	 * lastleft when lastleft and firstright are equal prior to heap TID (that
	 * is, the tiebreaker TID value comes from lastleft).  It isn't actually
	 * necessary for a new leaf high key to be a copy of lastleft for the L&Y
	 * "subtree" invariant to hold.  It's sufficient to make sure that the new
	 * leaf high key is strictly less than firstright, and greater than or
	 * equal to (not necessarily equal to) lastleft.  In other words, when
	 * suffix truncation isn't possible during a leaf page split, we take
	 * L&Y's exact approach to generating a new high key for the left page.
	 * (Actually, that is slightly inaccurate.  We don't just use a copy of
	 * lastleft.  A tuple with all the keys from firstright but the max heap
	 * TID from lastleft is used, to avoid introducing a special case.)
	 */
	if (!newitemonleft && newitemoff == firstrightoff)
	{
		/* incoming tuple becomes firstright */
		itemsz = newitemsz;
		firstright = newitem;
	}
	else
	{
		/* existing item at firstrightoff becomes firstright */
		itemid = PageGetItemId(origpage, firstrightoff);
		itemsz = ItemIdGetLength(itemid);
		firstright = (IndexTuple) PageGetItem(origpage, itemid);
		if (firstrightoff == origpagepostingoff)
			firstright = nposting;
	}

	if (isleaf)
	{
		IndexTuple	lastleft;

		/* Attempt suffix truncation for leaf page splits */
		if (newitemonleft && newitemoff == firstrightoff)
		{
			/* incoming tuple becomes lastleft */
			lastleft = newitem;
		}
		else
		{
			OffsetNumber lastleftoff;

			/* existing item before firstrightoff becomes lastleft */
			lastleftoff = OffsetNumberPrev(firstrightoff);
			Assert(lastleftoff >= P_FIRSTDATAKEY(oopaque));
			itemid = PageGetItemId(origpage, lastleftoff);
			lastleft = (IndexTuple) PageGetItem(origpage, itemid);
			if (lastleftoff == origpagepostingoff)
				lastleft = nposting;
		}

		lefthighkey = _treeb_truncate(rel, lastleft, firstright, itup_key);
		itemsz = IndexTupleSize(lefthighkey);
	}
	else
	{
		/*
		 * Don't perform suffix truncation on a copy of firstright to make
		 * left page high key for internal page splits.  Must use firstright
		 * as new high key directly.
		 *
		 * Each distinct separator key value originates as a leaf level high
		 * key; all other separator keys/pivot tuples are copied from one
		 * level down.  A separator key in a grandparent page must be
		 * identical to high key in rightmost parent page of the subtree to
		 * its left, which must itself be identical to high key in rightmost
		 * child page of that same subtree (this even applies to separator
		 * from grandparent's high key).  There must always be an unbroken
		 * "seam" of identical separator keys that guide index scans at every
		 * level, starting from the grandparent.  That's why suffix truncation
		 * is unsafe here.
		 *
		 * Internal page splits will truncate firstright into a "negative
		 * infinity" data item when it gets inserted on the new right page
		 * below, though.  This happens during the call to _treeb_pgaddtup() for
		 * the new first data item for right page.  Do not confuse this
		 * mechanism with suffix truncation.  It is just a convenient way of
		 * implementing page splits that split the internal page "inside"
		 * firstright.  The lefthighkey separator key cannot appear a second
		 * time in the right page (only firstright's downlink goes in right
		 * page).
		 */
		lefthighkey = firstright;
	}

	/*
	 * Add new high key to leftpage
	 */
	afterleftoff = P_HIKEY;

	Assert(TreebTupleGetNAtts(lefthighkey, rel) > 0);
	Assert(TreebTupleGetNAtts(lefthighkey, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(itemsz == MAXALIGN(IndexTupleSize(lefthighkey)));
	if (PageAddItem(leftpage, (Item) lefthighkey, itemsz, afterleftoff, false,
					false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add high key to the left sibling"
			 " while splitting block %u of index \"%s\"",
			 origpagenumber, RelationGetRelationName(rel));
	afterleftoff = OffsetNumberNext(afterleftoff);

	/*
	 * Acquire a new right page to split into, now that left page has a new
	 * high key.  From here on, it's not okay to throw an error without
	 * zeroing rightpage first.  This coding rule ensures that we won't
	 * confuse future VACUUM operations, which might otherwise try to re-find
	 * a downlink to a leftover junk page as the page undergoes deletion.
	 *
	 * It would be reasonable to start the critical section just after the new
	 * rightpage buffer is acquired instead; that would allow us to avoid
	 * leftover junk pages without bothering to zero rightpage.  We do it this
	 * way because it avoids an unnecessary PANIC when either origpage or its
	 * existing sibling page are corrupt.
	 */
	rbuf = _treeb_allocbuf(rel, heaprel);
	rightpage = BufferGetPage(rbuf);
	rightpagenumber = BufferGetBlockNumber(rbuf);
	/* rightpage was initialized by _treeb_getbuf */
	ropaque = TREEBPageGetOpaque(rightpage);

	/*
	 * Finish off remaining leftpage special area fields.  They cannot be set
	 * before both origpage (leftpage) and rightpage buffers are acquired and
	 * locked.
	 *
	 * treebpo_cycleid is only used with leaf pages, though we set it here in all
	 * cases just to be consistent.
	 */
	lopaque->treebpo_next = rightpagenumber;
	lopaque->treebpo_cycleid = _treeb_vacuum_cycleid(rel);

	/*
	 * rightpage won't be the root when we're done.  Also, clear the SPLIT_END
	 * and HAS_GARBAGE flags.
	 */
	ropaque->treebpo_flags = oopaque->treebpo_flags;
	ropaque->treebpo_flags &= ~(TREEBP_ROOT | TREEBP_SPLIT_END | TREEBP_HAS_GARBAGE);
	ropaque->treebpo_prev = origpagenumber;
	ropaque->treebpo_next = oopaque->treebpo_next;
	ropaque->treebpo_level = oopaque->treebpo_level;
	ropaque->treebpo_cycleid = lopaque->treebpo_cycleid;

	/*
	 * Add new high key to rightpage where necessary.
	 *
	 * If the page we're splitting is not the rightmost page at its level in
	 * the tree, then the first entry on the page is the high key from
	 * origpage.
	 */
	afterrightoff = P_HIKEY;

	if (!isrightmost)
	{
		IndexTuple	righthighkey;

		itemid = PageGetItemId(origpage, P_HIKEY);
		itemsz = ItemIdGetLength(itemid);
		righthighkey = (IndexTuple) PageGetItem(origpage, itemid);
		Assert(TreebTupleGetNAtts(righthighkey, rel) > 0);
		Assert(TreebTupleGetNAtts(righthighkey, rel) <=
			   IndexRelationGetNumberOfKeyAttributes(rel));
		if (PageAddItem(rightpage, (Item) righthighkey, itemsz, afterrightoff,
						false, false) == InvalidOffsetNumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add high key to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		afterrightoff = OffsetNumberNext(afterrightoff);
	}

	/*
	 * Internal page splits truncate first data item on right page -- it
	 * becomes "minus infinity" item for the page.  Set this up here.
	 */
	minusinfoff = InvalidOffsetNumber;
	if (!isleaf)
		minusinfoff = afterrightoff;

	/*
	 * Now transfer all the data items (non-pivot tuples in isleaf case, or
	 * additional pivot tuples in !isleaf case) to the appropriate page.
	 *
	 * Note: we *must* insert at least the right page's items in item-number
	 * order, for the benefit of _treeb_restore_page().
	 */
	for (i = P_FIRSTDATAKEY(oopaque); i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	dataitem;

		itemid = PageGetItemId(origpage, i);
		itemsz = ItemIdGetLength(itemid);
		dataitem = (IndexTuple) PageGetItem(origpage, itemid);

		/* replace original item with nposting due to posting split? */
		if (i == origpagepostingoff)
		{
			Assert(TreebTupleIsPosting(dataitem));
			Assert(itemsz == MAXALIGN(IndexTupleSize(nposting)));
			dataitem = nposting;
		}

		/* does new item belong before this one? */
		else if (i == newitemoff)
		{
			if (newitemonleft)
			{
				Assert(newitemoff <= firstrightoff);
				if (!_treeb_pgaddtup(leftpage, newitemsz, newitem, afterleftoff,
								  false))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the left sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				afterleftoff = OffsetNumberNext(afterleftoff);
			}
			else
			{
				Assert(newitemoff >= firstrightoff);
				if (!_treeb_pgaddtup(rightpage, newitemsz, newitem, afterrightoff,
								  afterrightoff == minusinfoff))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the right sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				afterrightoff = OffsetNumberNext(afterrightoff);
			}
		}

		/* decide which page to put it on */
		if (i < firstrightoff)
		{
			if (!_treeb_pgaddtup(leftpage, itemsz, dataitem, afterleftoff, false))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the left sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			afterleftoff = OffsetNumberNext(afterleftoff);
		}
		else
		{
			if (!_treeb_pgaddtup(rightpage, itemsz, dataitem, afterrightoff,
							  afterrightoff == minusinfoff))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the right sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			afterrightoff = OffsetNumberNext(afterrightoff);
		}
	}

	/* Handle case where newitem goes at the end of rightpage */
	if (i <= newitemoff)
	{
		/*
		 * Can't have newitemonleft here; that would imply we were told to put
		 * *everything* on the left page, which cannot fit (if it could, we'd
		 * not be splitting the page).
		 */
		Assert(!newitemonleft && newitemoff == maxoff + 1);
		if (!_treeb_pgaddtup(rightpage, newitemsz, newitem, afterrightoff,
						  afterrightoff == minusinfoff))
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add new item to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		afterrightoff = OffsetNumberNext(afterrightoff);
	}

	/*
	 * We have to grab the original right sibling (if any) and update its prev
	 * link.  We are guaranteed that this is deadlock-free, since we couple
	 * the locks in the standard order: left to right.
	 */
	if (!isrightmost)
	{
		sbuf = _treeb_getbuf(rel, oopaque->treebpo_next, TREEB_WRITE);
		spage = BufferGetPage(sbuf);
		sopaque = TREEBPageGetOpaque(spage);
		if (sopaque->treebpo_prev != origpagenumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("right sibling's left-link doesn't match: "
									 "block %u links to %u instead of expected %u in index \"%s\"",
									 oopaque->treebpo_next, sopaque->treebpo_prev, origpagenumber,
									 RelationGetRelationName(rel))));
		}

		/*
		 * Check to see if we can set the SPLIT_END flag in the right-hand
		 * split page; this can save some I/O for vacuum since it need not
		 * proceed to the right sibling.  We can set the flag if the right
		 * sibling has a different cycleid: that means it could not be part of
		 * a group of pages that were all split off from the same ancestor
		 * page.  If you're confused, imagine that page A splits to A B and
		 * then again, yielding A C B, while vacuum is in progress.  Tuples
		 * originally in A could now be in either B or C, hence vacuum must
		 * examine both pages.  But if D, our right sibling, has a different
		 * cycleid then it could not contain any tuples that were in A when
		 * the vacuum started.
		 */
		if (sopaque->treebpo_cycleid != ropaque->treebpo_cycleid)
			ropaque->treebpo_flags |= TREEBP_SPLIT_END;
	}

	/*
	 * Right sibling is locked, new siblings are prepared, but original page
	 * is not updated yet.
	 *
	 * NO EREPORT(ERROR) till right sibling is updated.  We can get away with
	 * not starting the critical section till here because we haven't been
	 * scribbling on the original page yet; see comments above.
	 */
	START_CRIT_SECTION();

	/*
	 * By here, the original data page has been split into two new halves, and
	 * these are correct.  The algorithm requires that the left page never
	 * move during a split, so we copy the new left page back on top of the
	 * original.  We need to do this before writing the WAL record, so that
	 * XLogInsert can WAL log an image of the page if necessary.
	 */
	PageRestoreTempPage(leftpage, origpage);
	/* leftpage, lopaque must not be used below here */

	MarkBufferDirty(buf);
	MarkBufferDirty(rbuf);

	if (!isrightmost)
	{
		sopaque->treebpo_prev = rightpagenumber;
		MarkBufferDirty(sbuf);
	}

	/*
	 * Clear INCOMPLETE_SPLIT flag on child if inserting the new item finishes
	 * a split
	 */
	if (!isleaf)
	{
		Page		cpage = BufferGetPage(cbuf);
		TreebPageOpaque cpageop = TREEBPageGetOpaque(cpage);

		cpageop->treebpo_flags &= ~TREEBP_INCOMPLETE_SPLIT;
		MarkBufferDirty(cbuf);
	}

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_treeb_split xlrec;
		uint8		xlinfo;
		XLogRecPtr	recptr;

		xlrec.level = ropaque->treebpo_level;
		/* See comments below on newitem, orignewitem, and posting lists */
		xlrec.firstrightoff = firstrightoff;
		xlrec.newitemoff = newitemoff;
		xlrec.postingoff = 0;
		if (postingoff != 0 && origpagepostingoff < firstrightoff)
			xlrec.postingoff = postingoff;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfTreebSplit);

		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBuffer(1, rbuf, REGBUF_WILL_INIT);
		/* Log original right sibling, since we've changed its prev-pointer */
		if (!isrightmost)
			XLogRegisterBuffer(2, sbuf, REGBUF_STANDARD);
		if (!isleaf)
			XLogRegisterBuffer(3, cbuf, REGBUF_STANDARD);

		/*
		 * Log the new item, if it was inserted on the left page. (If it was
		 * put on the right page, we don't need to explicitly WAL log it
		 * because it's included with all the other items on the right page.)
		 * Show the new item as belonging to the left page buffer, so that it
		 * is not stored if XLogInsert decides it needs a full-page image of
		 * the left page.  We always store newitemoff in the record, though.
		 *
		 * The details are sometimes slightly different for page splits that
		 * coincide with a posting list split.  If both the replacement
		 * posting list and newitem go on the right page, then we don't need
		 * to log anything extra, just like the simple !newitemonleft
		 * no-posting-split case (postingoff is set to zero in the WAL record,
		 * so recovery doesn't need to process a posting list split at all).
		 * Otherwise, we set postingoff and log orignewitem instead of
		 * newitem, despite having actually inserted newitem.  REDO routine
		 * must reconstruct nposting and newitem using _treeb_swap_posting().
		 *
		 * Note: It's possible that our page split point is the point that
		 * makes the posting list lastleft and newitem firstright.  This is
		 * the only case where we log orignewitem/newitem despite newitem
		 * going on the right page.  If XLogInsert decides that it can omit
		 * orignewitem due to logging a full-page image of the left page,
		 * everything still works out, since recovery only needs to log
		 * orignewitem for items on the left page (just like the regular
		 * newitem-logged case).
		 */
		if (newitemonleft && xlrec.postingoff == 0)
			XLogRegisterBufData(0, (char *) newitem, newitemsz);
		else if (xlrec.postingoff != 0)
		{
			Assert(isleaf);
			Assert(newitemonleft || firstrightoff == newitemoff);
			Assert(newitemsz == IndexTupleSize(orignewitem));
			XLogRegisterBufData(0, (char *) orignewitem, newitemsz);
		}

		/* Log the left page's new high key */
		if (!isleaf)
		{
			/* lefthighkey isn't local copy, get current pointer */
			itemid = PageGetItemId(origpage, P_HIKEY);
			lefthighkey = (IndexTuple) PageGetItem(origpage, itemid);
		}
		XLogRegisterBufData(0, (char *) lefthighkey,
							MAXALIGN(IndexTupleSize(lefthighkey)));

		/*
		 * Log the contents of the right page in the format understood by
		 * _treeb_restore_page().  The whole right page will be recreated.
		 *
		 * Direct access to page is not good but faster - we should implement
		 * some new func in page API.  Note we only store the tuples
		 * themselves, knowing that they were inserted in item-number order
		 * and so the line pointers can be reconstructed.  See comments for
		 * _treeb_restore_page().
		 */
		XLogRegisterBufData(1,
							(char *) rightpage + ((PageHeader) rightpage)->pd_upper,
							((PageHeader) rightpage)->pd_special - ((PageHeader) rightpage)->pd_upper);

		xlinfo = newitemonleft ? XLOG_TREEB_SPLIT_L : XLOG_TREEB_SPLIT_R;
		recptr = XLogInsert(RM_TREEB_ID, xlinfo);

		PageSetLSN(origpage, recptr);
		PageSetLSN(rightpage, recptr);
		if (!isrightmost)
			PageSetLSN(spage, recptr);
		if (!isleaf)
			PageSetLSN(BufferGetPage(cbuf), recptr);
	}

	END_CRIT_SECTION();

	/* release the old right sibling */
	if (!isrightmost)
		_treeb_relbuf(rel, sbuf);

	/* release the child */
	if (!isleaf)
		_treeb_relbuf(rel, cbuf);

	/* be tidy */
	if (isleaf)
		pfree(lefthighkey);

	/* split's done */
	return rbuf;
}

/*
 * _treeb_insert_parent() -- Insert downlink into parent, completing split.
 *
 * On entry, buf and rbuf are the left and right split pages, which we
 * still hold write locks on.  Both locks will be released here.  We
 * release the rbuf lock once we have a write lock on the page that we
 * intend to insert a downlink to rbuf on (i.e. buf's current parent page).
 * The lock on buf is released at the same point as the lock on the parent
 * page, since buf's INCOMPLETE_SPLIT flag must be cleared by the same
 * atomic operation that completes the split by inserting a new downlink.
 *
 * stack - stack showing how we got here.  Will be NULL when splitting true
 *			root, or during concurrent root split, where we can be inefficient
 * isroot - we split the true root
 * isonly - we split a page alone on its level (might have been fast root)
 */
static void
_treeb_insert_parent(Relation rel,
				  Relation heaprel,
				  Buffer buf,
				  Buffer rbuf,
				  TreebStack stack,
				  bool isroot,
				  bool isonly)
{
	Assert(heaprel != NULL);

	/*
	 * Here we have to do something Lehman and Yao don't talk about: deal with
	 * a root split and construction of a new root.  If our stack is empty
	 * then we have just split a node on what had been the root level when we
	 * descended the tree.  If it was still the root then we perform a
	 * new-root construction.  If it *wasn't* the root anymore, search to find
	 * the next higher level that someone constructed meanwhile, and find the
	 * right place to insert as for the normal case.
	 *
	 * If we have to search for the parent level, we do so by re-descending
	 * from the root.  This is not super-efficient, but it's rare enough not
	 * to matter.
	 */
	if (isroot)
	{
		Buffer		rootbuf;

		Assert(stack == NULL);
		Assert(isonly);
		/* create a new root node one level up and update the metapage */
		rootbuf = _treeb_newlevel(rel, heaprel, buf, rbuf);
		/* release the split buffers */
		_treeb_relbuf(rel, rootbuf);
		_treeb_relbuf(rel, rbuf);
		_treeb_relbuf(rel, buf);
	}
	else
	{
		BlockNumber bknum = BufferGetBlockNumber(buf);
		BlockNumber rbknum = BufferGetBlockNumber(rbuf);
		Page		page = BufferGetPage(buf);
		IndexTuple	new_item;
		TreebStackData fakestack;
		IndexTuple	ritem;
		Buffer		pbuf;

		if (stack == NULL)
		{
			TreebPageOpaque opaque;

			elog(DEBUG2, "concurrent ROOT page split");
			opaque = TREEBPageGetOpaque(page);

			/*
			 * We should never reach here when a leaf page split takes place
			 * despite the insert of newitem being able to apply the fastpath
			 * optimization.  Make sure of that with an assertion.
			 *
			 * This is more of a performance issue than a correctness issue.
			 * The fastpath won't have a descent stack.  Using a phony stack
			 * here works, but never rely on that.  The fastpath should be
			 * rejected within _treeb_search_insert() when the rightmost leaf
			 * page will split, since it's faster to go through _treeb_search()
			 * and get a stack in the usual way.
			 */
			Assert(!(P_ISLEAF(opaque) &&
					 BlockNumberIsValid(RelationGetTargetBlock(rel))));

			/* Find the leftmost page at the next level up */
			pbuf = _treeb_get_endpoint(rel, opaque->treebpo_level + 1, false);
			/* Set up a phony stack entry pointing there */
			stack = &fakestack;
			stack->treebs_blkno = BufferGetBlockNumber(pbuf);
			stack->treebs_offset = InvalidOffsetNumber;
			stack->treebs_parent = NULL;
			_treeb_relbuf(rel, pbuf);
		}

		/* get high key from left, a strict lower bound for new right page */
		ritem = (IndexTuple) PageGetItem(page,
										 PageGetItemId(page, P_HIKEY));

		/* form an index tuple that points at the new right page */
		new_item = CopyIndexTuple(ritem);
		TreebTupleSetDownLink(new_item, rbknum);

		/*
		 * Re-find and write lock the parent of buf.
		 *
		 * It's possible that the location of buf's downlink has changed since
		 * our initial _treeb_search() descent.  _treeb_getstackbuf() will detect
		 * and recover from this, updating the stack, which ensures that the
		 * new downlink will be inserted at the correct offset. Even buf's
		 * parent may have changed.
		 */
		pbuf = _treeb_getstackbuf(rel, heaprel, stack, bknum);

		/*
		 * Unlock the right child.  The left child will be unlocked in
		 * _treeb_insertonpg().
		 *
		 * Unlocking the right child must be delayed until here to ensure that
		 * no concurrent VACUUM operation can become confused.  Page deletion
		 * cannot be allowed to fail to re-find a downlink for the rbuf page.
		 * (Actually, this is just a vestige of how things used to work.  The
		 * page deletion code is expected to check for the INCOMPLETE_SPLIT
		 * flag on the left child.  It won't attempt deletion of the right
		 * child until the split is complete.  Despite all this, we opt to
		 * conservatively delay unlocking the right child until here.)
		 */
		_treeb_relbuf(rel, rbuf);

		if (pbuf == InvalidBuffer)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("failed to re-find parent key in index \"%s\" for split pages %u/%u",
									 RelationGetRelationName(rel), bknum, rbknum)));

		/* Recursively insert into the parent */
		_treeb_insertonpg(rel, heaprel, NULL, pbuf, buf, stack->treebs_parent,
					   new_item, MAXALIGN(IndexTupleSize(new_item)),
					   stack->treebs_offset + 1, 0, isonly);

		/* be tidy */
		pfree(new_item);
	}
}

/*
 * _treeb_finish_split() -- Finish an incomplete split
 *
 * A crash or other failure can leave a split incomplete.  The insertion
 * routines won't allow to insert on a page that is incompletely split.
 * Before inserting on such a page, call _treeb_finish_split().
 *
 * On entry, 'lbuf' must be locked in write-mode.  On exit, it is unlocked
 * and unpinned.
 *
 * Caller must provide a valid heaprel, since finishing a page split requires
 * allocating a new page if and when the parent page splits in turn.
 */
void
_treeb_finish_split(Relation rel, Relation heaprel, Buffer lbuf, TreebStack stack)
{
	Page		lpage = BufferGetPage(lbuf);
	TreebPageOpaque lpageop = TREEBPageGetOpaque(lpage);
	Buffer		rbuf;
	Page		rpage;
	TreebPageOpaque rpageop;
	bool		wasroot;
	bool		wasonly;

	Assert(P_INCOMPLETE_SPLIT(lpageop));
	Assert(heaprel != NULL);

	/* Lock right sibling, the one missing the downlink */
	rbuf = _treeb_getbuf(rel, lpageop->treebpo_next, TREEB_WRITE);
	rpage = BufferGetPage(rbuf);
	rpageop = TREEBPageGetOpaque(rpage);

	/* Could this be a root split? */
	if (!stack)
	{
		Buffer		metabuf;
		Page		metapg;
		TreebMetaPageData *metad;

		/* acquire lock on the metapage */
		metabuf = _treeb_getbuf(rel, TREEB_METAPAGE, TREEB_WRITE);
		metapg = BufferGetPage(metabuf);
		metad = TreebPageGetMeta(metapg);

		wasroot = (metad->treebm_root == BufferGetBlockNumber(lbuf));

		_treeb_relbuf(rel, metabuf);
	}
	else
		wasroot = false;

	/* Was this the only page on the level before split? */
	wasonly = (P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop));

	elog(DEBUG1, "finishing incomplete split of %u/%u",
		 BufferGetBlockNumber(lbuf), BufferGetBlockNumber(rbuf));

	_treeb_insert_parent(rel, heaprel, lbuf, rbuf, stack, wasroot, wasonly);
}

/*
 *	_treeb_getstackbuf() -- Walk back up the tree one step, and find the pivot
 *						 tuple whose downlink points to child page.
 *
 *		Caller passes child's block number, which is used to identify
 *		associated pivot tuple in parent page using a linear search that
 *		matches on pivot's downlink/block number.  The expected location of
 *		the pivot tuple is taken from the stack one level above the child
 *		page.  This is used as a starting point.  Insertions into the
 *		parent level could cause the pivot tuple to move right; deletions
 *		could cause it to move left, but not left of the page we previously
 *		found it on.
 *
 *		Caller can use its stack to relocate the pivot tuple/downlink for
 *		any same-level page to the right of the page found by its initial
 *		descent.  This is necessary because of the possibility that caller
 *		moved right to recover from a concurrent page split.  It's also
 *		convenient for certain callers to be able to step right when there
 *		wasn't a concurrent page split, while still using their original
 *		stack.  For example, the checkingunique _treeb_doinsert() case may
 *		have to step right when there are many physical duplicates, and its
 *		scantid forces an insertion to the right of the "first page the
 *		value could be on".  (This is also relied on by all of our callers
 *		when dealing with !heapkeyspace indexes.)
 *
 *		Returns write-locked parent page buffer, or InvalidBuffer if pivot
 *		tuple not found (should not happen).  Adjusts treebs_blkno &
 *		treebs_offset if changed.  Page split caller should insert its new
 *		pivot tuple for its new right sibling page on parent page, at the
 *		offset number treebs_offset + 1.
 */
Buffer
_treeb_getstackbuf(Relation rel, Relation heaprel, TreebStack stack, BlockNumber child)
{
	BlockNumber blkno;
	OffsetNumber start;

	blkno = stack->treebs_blkno;
	start = stack->treebs_offset;

	for (;;)
	{
		Buffer		buf;
		Page		page;
		TreebPageOpaque opaque;

		buf = _treeb_getbuf(rel, blkno, TREEB_WRITE);
		page = BufferGetPage(buf);
		opaque = TREEBPageGetOpaque(page);

		Assert(heaprel != NULL);
		if (P_INCOMPLETE_SPLIT(opaque))
		{
			_treeb_finish_split(rel, heaprel, buf, stack->treebs_parent);
			continue;
		}

		if (!P_IGNORE(opaque))
		{
			OffsetNumber offnum,
						minoff,
						maxoff;
			ItemId		itemid;
			IndexTuple	item;

			minoff = P_FIRSTDATAKEY(opaque);
			maxoff = PageGetMaxOffsetNumber(page);

			/*
			 * start = InvalidOffsetNumber means "search the whole page". We
			 * need this test anyway due to possibility that page has a high
			 * key now when it didn't before.
			 */
			if (start < minoff)
				start = minoff;

			/*
			 * Need this check too, to guard against possibility that page
			 * split since we visited it originally.
			 */
			if (start > maxoff)
				start = OffsetNumberNext(maxoff);

			/*
			 * These loops will check every item on the page --- but in an
			 * order that's attuned to the probability of where it actually
			 * is.  Scan to the right first, then to the left.
			 */
			for (offnum = start;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (TreebTupleGetDownLink(item) == child)
				{
					/* Return accurate pointer to where link is now */
					stack->treebs_blkno = blkno;
					stack->treebs_offset = offnum;
					return buf;
				}
			}

			for (offnum = OffsetNumberPrev(start);
				 offnum >= minoff;
				 offnum = OffsetNumberPrev(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (TreebTupleGetDownLink(item) == child)
				{
					/* Return accurate pointer to where link is now */
					stack->treebs_blkno = blkno;
					stack->treebs_offset = offnum;
					return buf;
				}
			}
		}

		/*
		 * The item we're looking for moved right at least one page.
		 *
		 * Lehman and Yao couple/chain locks when moving right here, which we
		 * can avoid.  See treeb/README.
		 */
		if (P_RIGHTMOST(opaque))
		{
			_treeb_relbuf(rel, buf);
			return InvalidBuffer;
		}
		blkno = opaque->treebpo_next;
		start = InvalidOffsetNumber;
		_treeb_relbuf(rel, buf);
	}
}

/*
 *	_treeb_newlevel() -- Create a new level above root page.
 *
 *		We've just split the old root page and need to create a new one.
 *		In order to do this, we add a new root page to the file, then lock
 *		the metadata page and update it.  This is guaranteed to be deadlock-
 *		free, because all readers release their locks on the metadata page
 *		before trying to lock the root, and all writers lock the root before
 *		trying to lock the metadata page.  We have a write lock on the old
 *		root page, so we have not introduced any cycles into the waits-for
 *		graph.
 *
 *		On entry, lbuf (the old root) and rbuf (its new peer) are write-
 *		locked. On exit, a new root page exists with entries for the
 *		two new children, metapage is updated and unlocked/unpinned.
 *		The new root buffer is returned to caller which has to unlock/unpin
 *		lbuf, rbuf & rootbuf.
 */
static Buffer
_treeb_newlevel(Relation rel, Relation heaprel, Buffer lbuf, Buffer rbuf)
{
	Buffer		rootbuf;
	Page		lpage,
				rootpage;
	BlockNumber lbkno,
				rbkno;
	BlockNumber rootblknum;
	TreebPageOpaque rootopaque;
	TreebPageOpaque lopaque;
	ItemId		itemid;
	IndexTuple	item;
	IndexTuple	left_item;
	Size		left_item_sz;
	IndexTuple	right_item;
	Size		right_item_sz;
	Buffer		metabuf;
	Page		metapg;
	TreebMetaPageData *metad;

	lbkno = BufferGetBlockNumber(lbuf);
	rbkno = BufferGetBlockNumber(rbuf);
	lpage = BufferGetPage(lbuf);
	lopaque = TREEBPageGetOpaque(lpage);

	/* get a new root page */
	rootbuf = _treeb_allocbuf(rel, heaprel);
	rootpage = BufferGetPage(rootbuf);
	rootblknum = BufferGetBlockNumber(rootbuf);

	/* acquire lock on the metapage */
	metabuf = _treeb_getbuf(rel, TREEB_METAPAGE, TREEB_WRITE);
	metapg = BufferGetPage(metabuf);
	metad = TreebPageGetMeta(metapg);

	/*
	 * Create downlink item for left page (old root).  The key value used is
	 * "minus infinity", a sentinel value that's reliably less than any real
	 * key value that could appear in the left page.
	 */
	left_item_sz = sizeof(IndexTupleData);
	left_item = (IndexTuple) palloc(left_item_sz);
	left_item->t_info = left_item_sz;
	TreebTupleSetDownLink(left_item, lbkno);
	TreebTupleSetNAtts(left_item, 0, false);

	/*
	 * Create downlink item for right page.  The key for it is obtained from
	 * the "high key" position in the left page.
	 */
	itemid = PageGetItemId(lpage, P_HIKEY);
	right_item_sz = ItemIdGetLength(itemid);
	item = (IndexTuple) PageGetItem(lpage, itemid);
	right_item = CopyIndexTuple(item);
	TreebTupleSetDownLink(right_item, rbkno);

	/* NO EREPORT(ERROR) from here till newroot op is logged */
	START_CRIT_SECTION();

	/* upgrade metapage if needed */
	if (metad->treebm_version < TREEB_NOVAC_VERSION)
		_treeb_upgrademetapage(metapg);

	/* set treeb special data */
	rootopaque = TREEBPageGetOpaque(rootpage);
	rootopaque->treebpo_prev = rootopaque->treebpo_next = P_NONE;
	rootopaque->treebpo_flags = TREEBP_ROOT;
	rootopaque->treebpo_level =
		(TREEBPageGetOpaque(lpage))->treebpo_level + 1;
	rootopaque->treebpo_cycleid = 0;

	/* update metapage data */
	metad->treebm_root = rootblknum;
	metad->treebm_level = rootopaque->treebpo_level;
	metad->treebm_fastroot = rootblknum;
	metad->treebm_fastlevel = rootopaque->treebpo_level;

	/*
	 * Insert the left page pointer into the new root page.  The root page is
	 * the rightmost page on its level so there is no "high key" in it; the
	 * two items will go into positions P_HIKEY and P_FIRSTKEY.
	 *
	 * Note: we *must* insert the two items in item-number order, for the
	 * benefit of _treeb_restore_page().
	 */
	Assert(TreebTupleGetNAtts(left_item, rel) == 0);
	if (PageAddItem(rootpage, (Item) left_item, left_item_sz, P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(PANIC, "failed to add leftkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/*
	 * insert the right page pointer into the new root page.
	 */
	Assert(TreebTupleGetNAtts(right_item, rel) > 0);
	Assert(TreebTupleGetNAtts(right_item, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	if (PageAddItem(rootpage, (Item) right_item, right_item_sz, P_FIRSTKEY,
					false, false) == InvalidOffsetNumber)
		elog(PANIC, "failed to add rightkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/* Clear the incomplete-split flag in the left child */
	Assert(P_INCOMPLETE_SPLIT(lopaque));
	lopaque->treebpo_flags &= ~TREEBP_INCOMPLETE_SPLIT;
	MarkBufferDirty(lbuf);

	MarkBufferDirty(rootbuf);
	MarkBufferDirty(metabuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_treeb_newroot xlrec;
		XLogRecPtr	recptr;
		xl_treeb_metadata md;

		xlrec.rootblk = rootblknum;
		xlrec.level = metad->treebm_level;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfTreebNewroot);

		XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, lbuf, REGBUF_STANDARD);
		XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		Assert(metad->treebm_version >= TREEB_NOVAC_VERSION);
		md.version = metad->treebm_version;
		md.root = rootblknum;
		md.level = metad->treebm_level;
		md.fastroot = rootblknum;
		md.fastlevel = metad->treebm_level;
		md.last_cleanup_num_delpages = metad->treebm_last_cleanup_num_delpages;
		md.allequalimage = metad->treebm_allequalimage;

		XLogRegisterBufData(2, (char *) &md, sizeof(xl_treeb_metadata));

		/*
		 * Direct access to page is not good but faster - we should implement
		 * some new func in page API.
		 */
		XLogRegisterBufData(0,
							(char *) rootpage + ((PageHeader) rootpage)->pd_upper,
							((PageHeader) rootpage)->pd_special -
							((PageHeader) rootpage)->pd_upper);

		recptr = XLogInsert(RM_TREEB_ID, XLOG_TREEB_NEWROOT);

		PageSetLSN(lpage, recptr);
		PageSetLSN(rootpage, recptr);
		PageSetLSN(metapg, recptr);
	}

	END_CRIT_SECTION();

	/* done with metapage */
	_treeb_relbuf(rel, metabuf);

	pfree(left_item);
	pfree(right_item);

	return rootbuf;
}

/*
 *	_treeb_pgaddtup() -- add a data item to a particular page during split.
 *
 *		The difference between this routine and a bare PageAddItem call is
 *		that this code can deal with the first data item on an internal treeb
 *		page in passing.  This data item (which is called "firstright" within
 *		_treeb_split()) has a key that must be treated as minus infinity after
 *		the split.  Therefore, we truncate away all attributes when caller
 *		specifies it's the first data item on page (downlink is not changed,
 *		though).  This extra step is only needed for the right page of an
 *		internal page split.  There is no need to do this for the first data
 *		item on the existing/left page, since that will already have been
 *		truncated during an earlier page split.
 *
 *		See _treeb_split() for a high level explanation of why we truncate here.
 *		Note that this routine has nothing to do with suffix truncation,
 *		despite using some of the same infrastructure.
 */
static inline bool
_treeb_pgaddtup(Page page,
			 Size itemsize,
			 IndexTuple itup,
			 OffsetNumber itup_off,
			 bool newfirstdataitem)
{
	IndexTupleData trunctuple;

	if (newfirstdataitem)
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		TreebTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (unlikely(PageAddItem(page, (Item) itup, itemsize, itup_off, false,
							 false) == InvalidOffsetNumber))
		return false;

	return true;
}

/*
 * _treeb_delete_or_dedup_one_page - Try to avoid a leaf page split.
 *
 * There are three operations performed here: simple index deletion, bottom-up
 * index deletion, and deduplication.  If all three operations fail to free
 * enough space for the incoming item then caller will go on to split the
 * page.  We always consider simple deletion first.  If that doesn't work out
 * we consider alternatives.  Callers that only want us to consider simple
 * deletion (without any fallback) ask for that using the 'simpleonly'
 * argument.
 *
 * We usually pick only one alternative "complex" operation when simple
 * deletion alone won't prevent a page split.  The 'checkingunique',
 * 'uniquedup', and 'indexUnchanged' arguments are used for that.
 *
 * Note: We used to only delete LP_DEAD items when the TREEBP_HAS_GARBAGE page
 * level flag was found set.  The flag was useful back when there wasn't
 * necessarily one single page for a duplicate tuple to go on (before heap TID
 * became a part of the key space in version 4 indexes).  But we don't
 * actually look at the flag anymore (it's not a gating condition for our
 * caller).  That would cause us to miss tuples that are safe to delete,
 * without getting any benefit in return.  We know that the alternative is to
 * split the page; scanning the line pointer array in passing won't have
 * noticeable overhead.  (We still maintain the TREEBP_HAS_GARBAGE flag despite
 * all this because !heapkeyspace indexes must still do a "getting tired"
 * linear search, and so are likely to get some benefit from using it as a
 * gating condition.)
 */
static void
_treeb_delete_or_dedup_one_page(Relation rel, Relation heapRel,
							 TreebInsertState insertstate,
							 bool simpleonly, bool checkingunique,
							 bool uniquedup, bool indexUnchanged)
{
	OffsetNumber deletable[MaxIndexTuplesPerPage];
	int			ndeletable = 0;
	OffsetNumber offnum,
				minoff,
				maxoff;
	Buffer		buffer = insertstate->buf;
	TreebScanInsert itup_key = insertstate->itup_key;
	Page		page = BufferGetPage(buffer);
	TreebPageOpaque opaque = TREEBPageGetOpaque(page);

	Assert(P_ISLEAF(opaque));
	Assert(simpleonly || itup_key->heapkeyspace);
	Assert(!simpleonly || (!checkingunique && !uniquedup && !indexUnchanged));

	/*
	 * Scan over all items to see which ones need to be deleted according to
	 * LP_DEAD flags.  We'll usually manage to delete a few extra items that
	 * are not marked LP_DEAD in passing.  Often the extra items that actually
	 * end up getting deleted are items that would have had their LP_DEAD bit
	 * set before long anyway (if we opted not to include them as extras).
	 */
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsDead(itemId))
			deletable[ndeletable++] = offnum;
	}

	if (ndeletable > 0)
	{
		_treeb_simpledel_pass(rel, buffer, heapRel, deletable, ndeletable,
						   insertstate->itup, minoff, maxoff);
		insertstate->bounds_valid = false;

		/* Return when a page split has already been avoided */
		if (PageGetFreeSpace(page) >= insertstate->itemsz)
			return;

		/* Might as well assume duplicates (if checkingunique) */
		uniquedup = true;
	}

	/*
	 * We're done with simple deletion.  Return early with callers that only
	 * call here so that simple deletion can be considered.  This includes
	 * callers that explicitly ask for this and checkingunique callers that
	 * probably don't have any version churn duplicates on the page.
	 *
	 * Note: The page's TREEBP_HAS_GARBAGE hint flag may still be set when we
	 * return at this point (or when we go on the try either or both of our
	 * other strategies and they also fail).  We do not bother expending a
	 * separate write to clear it, however.  Caller will definitely clear it
	 * when it goes on to split the page (note also that the deduplication
	 * process will clear the flag in passing, just to keep things tidy).
	 */
	if (simpleonly || (checkingunique && !uniquedup))
	{
		Assert(!indexUnchanged);
		return;
	}

	/* Assume bounds about to be invalidated (this is almost certain now) */
	insertstate->bounds_valid = false;

	/*
	 * Perform bottom-up index deletion pass when executor hint indicated that
	 * incoming item is logically unchanged, or for a unique index that is
	 * known to have physical duplicates for some other reason.  (There is a
	 * large overlap between these two cases for a unique index.  It's worth
	 * having both triggering conditions in order to apply the optimization in
	 * the event of successive related INSERT and DELETE statements.)
	 *
	 * We'll go on to do a deduplication pass when a bottom-up pass fails to
	 * delete an acceptable amount of free space (a significant fraction of
	 * the page, or space for the new item, whichever is greater).
	 *
	 * Note: Bottom-up index deletion uses the same equality/equivalence
	 * routines as deduplication internally.  However, it does not merge
	 * together index tuples, so the same correctness considerations do not
	 * apply.  We deliberately omit an index-is-allequalimage test here.
	 */
	if ((indexUnchanged || uniquedup) &&
		_treeb_bottomupdel_pass(rel, buffer, heapRel, insertstate->itemsz))
		return;

	/* Perform deduplication pass (when enabled and index-is-allequalimage) */
	if (TreebGetDeduplicateItems(rel) && itup_key->allequalimage)
		_treeb_dedup_pass(rel, buffer, insertstate->itup, insertstate->itemsz,
					   (indexUnchanged || uniquedup));
}

/*
 * _treeb_simpledel_pass - Simple index tuple deletion pass.
 *
 * We delete all LP_DEAD-set index tuples on a leaf page.  The offset numbers
 * of all such tuples are determined by caller (caller passes these to us as
 * its 'deletable' argument).
 *
 * We might also delete extra index tuples that turn out to be safe to delete
 * in passing (though they must be cheap to check in passing to begin with).
 * There is no certainty that any extra tuples will be deleted, though.  The
 * high level goal of the approach we take is to get the most out of each call
 * here (without noticeably increasing the per-call overhead compared to what
 * we need to do just to be able to delete the page's LP_DEAD-marked index
 * tuples).
 *
 * The number of extra index tuples that turn out to be deletable might
 * greatly exceed the number of LP_DEAD-marked index tuples due to various
 * locality related effects.  For example, it's possible that the total number
 * of table blocks (pointed to by all TIDs on the leaf page) is naturally
 * quite low, in which case we might end up checking if it's possible to
 * delete _most_ index tuples on the page (without the tableam needing to
 * access additional table blocks).  The tableam will sometimes stumble upon
 * _many_ extra deletable index tuples in indexes where this pattern is
 * common.
 *
 * See treeb/README for further details on simple index tuple deletion.
 */
static void
_treeb_simpledel_pass(Relation rel, Buffer buffer, Relation heapRel,
				   OffsetNumber *deletable, int ndeletable, IndexTuple newitem,
				   OffsetNumber minoff, OffsetNumber maxoff)
{
	Page		page = BufferGetPage(buffer);
	BlockNumber *deadblocks;
	int			ndeadblocks;
	TM_IndexDeleteOp delstate;
	OffsetNumber offnum;

	/* Get array of table blocks pointed to by LP_DEAD-set tuples */
	deadblocks = _treeb_deadblocks(page, deletable, ndeletable, newitem,
								&ndeadblocks);

	/* Initialize tableam state that describes index deletion operation */
	delstate.irel = rel;
	delstate.iblknum = BufferGetBlockNumber(buffer);
	delstate.bottomup = false;
	delstate.bottomupfreespace = 0;
	delstate.ndeltids = 0;
	delstate.deltids = palloc(MaxTIDsPerTreebPage * sizeof(TM_IndexDelete));
	delstate.status = palloc(MaxTIDsPerTreebPage * sizeof(TM_IndexStatus));

	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);
		TM_IndexDelete *odeltid = &delstate.deltids[delstate.ndeltids];
		TM_IndexStatus *ostatus = &delstate.status[delstate.ndeltids];
		BlockNumber tidblock;
		void	   *match;

		if (!TreebTupleIsPosting(itup))
		{
			tidblock = ItemPointerGetBlockNumber(&itup->t_tid);
			match = bsearch(&tidblock, deadblocks, ndeadblocks,
							sizeof(BlockNumber), _treeb_blk_cmp);

			if (!match)
			{
				Assert(!ItemIdIsDead(itemid));
				continue;
			}

			/*
			 * TID's table block is among those pointed to by the TIDs from
			 * LP_DEAD-bit set tuples on page -- add TID to deltids
			 */
			odeltid->tid = itup->t_tid;
			odeltid->id = delstate.ndeltids;
			ostatus->idxoffnum = offnum;
			ostatus->knowndeletable = ItemIdIsDead(itemid);
			ostatus->promising = false; /* unused */
			ostatus->freespace = 0; /* unused */

			delstate.ndeltids++;
		}
		else
		{
			int			nitem = TreebTupleGetNPosting(itup);

			for (int p = 0; p < nitem; p++)
			{
				ItemPointer tid = TreebTupleGetPostingN(itup, p);

				tidblock = ItemPointerGetBlockNumber(tid);
				match = bsearch(&tidblock, deadblocks, ndeadblocks,
								sizeof(BlockNumber), _treeb_blk_cmp);

				if (!match)
				{
					Assert(!ItemIdIsDead(itemid));
					continue;
				}

				/*
				 * TID's table block is among those pointed to by the TIDs
				 * from LP_DEAD-bit set tuples on page -- add TID to deltids
				 */
				odeltid->tid = *tid;
				odeltid->id = delstate.ndeltids;
				ostatus->idxoffnum = offnum;
				ostatus->knowndeletable = ItemIdIsDead(itemid);
				ostatus->promising = false; /* unused */
				ostatus->freespace = 0; /* unused */

				odeltid++;
				ostatus++;
				delstate.ndeltids++;
			}
		}
	}

	pfree(deadblocks);

	Assert(delstate.ndeltids >= ndeletable);

	/* Physically delete LP_DEAD tuples (plus any delete-safe extra TIDs) */
	_treeb_delitems_delete_check(rel, buffer, heapRel, &delstate);

	pfree(delstate.deltids);
	pfree(delstate.status);
}

/*
 * _treeb_deadblocks() -- Get LP_DEAD related table blocks.
 *
 * Builds sorted and unique-ified array of table block numbers from index
 * tuple TIDs whose line pointers are marked LP_DEAD.  Also adds the table
 * block from incoming newitem just in case it isn't among the LP_DEAD-related
 * table blocks.
 *
 * Always counting the newitem's table block as an LP_DEAD related block makes
 * sense because the cost is consistently low; it is practically certain that
 * the table block will not incur a buffer miss in tableam.  On the other hand
 * the benefit is often quite high.  There is a decent chance that there will
 * be some deletable items from this block, since in general most garbage
 * tuples became garbage in the recent past (in many cases this won't be the
 * first logical row that core code added to/modified in table block
 * recently).
 *
 * Returns final array, and sets *nblocks to its final size for caller.
 */
static BlockNumber *
_treeb_deadblocks(Page page, OffsetNumber *deletable, int ndeletable,
			   IndexTuple newitem, int *nblocks)
{
	int			spacentids,
				ntids;
	BlockNumber *tidblocks;

	/*
	 * Accumulate each TID's block in array whose initial size has space for
	 * one table block per LP_DEAD-set tuple (plus space for the newitem table
	 * block).  Array will only need to grow when there are LP_DEAD-marked
	 * posting list tuples (which is not that common).
	 */
	spacentids = ndeletable + 1;
	ntids = 0;
	tidblocks = (BlockNumber *) palloc(sizeof(BlockNumber) * spacentids);

	/*
	 * First add the table block for the incoming newitem.  This is the one
	 * case where simple deletion can visit a table block that doesn't have
	 * any known deletable items.
	 */
	Assert(!TreebTupleIsPosting(newitem) && !TreebTupleIsPivot(newitem));
	tidblocks[ntids++] = ItemPointerGetBlockNumber(&newitem->t_tid);

	for (int i = 0; i < ndeletable; i++)
	{
		ItemId		itemid = PageGetItemId(page, deletable[i]);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

		Assert(ItemIdIsDead(itemid));

		if (!TreebTupleIsPosting(itup))
		{
			if (ntids + 1 > spacentids)
			{
				spacentids *= 2;
				tidblocks = (BlockNumber *)
					repalloc(tidblocks, sizeof(BlockNumber) * spacentids);
			}

			tidblocks[ntids++] = ItemPointerGetBlockNumber(&itup->t_tid);
		}
		else
		{
			int			nposting = TreebTupleGetNPosting(itup);

			if (ntids + nposting > spacentids)
			{
				spacentids = Max(spacentids * 2, ntids + nposting);
				tidblocks = (BlockNumber *)
					repalloc(tidblocks, sizeof(BlockNumber) * spacentids);
			}

			for (int j = 0; j < nposting; j++)
			{
				ItemPointer tid = TreebTupleGetPostingN(itup, j);

				tidblocks[ntids++] = ItemPointerGetBlockNumber(tid);
			}
		}
	}

	qsort(tidblocks, ntids, sizeof(BlockNumber), _treeb_blk_cmp);
	*nblocks = qunique(tidblocks, ntids, sizeof(BlockNumber), _treeb_blk_cmp);

	return tidblocks;
}

/*
 * _treeb_blk_cmp() -- qsort comparison function for _treeb_simpledel_pass
 */
static inline int
_treeb_blk_cmp(const void *arg1, const void *arg2)
{
	BlockNumber b1 = *((BlockNumber *) arg1);
	BlockNumber b2 = *((BlockNumber *) arg2);

	return pg_cmp_u32(b1, b2);
}
