/*-------------------------------------------------------------------------
 *
 * treebsearch.c
 *	  Search code for postgres treebs.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/treeb/treebsearch.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/treeb.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


static void _treeb_drop_lock_and_maybe_pin(IndexScanDesc scan, TreebScanPos sp);
static OffsetNumber _treeb_binsrch(Relation rel, TreebScanInsert key, Buffer buf);
static int	_treeb_binsrch_posting(TreebScanInsert key, Page page,
								OffsetNumber offnum);
static bool _treeb_readpage(IndexScanDesc scan, ScanDirection dir,
						 OffsetNumber offnum, bool firstPage);
static void _treeb_saveitem(TreebScanOpaque so, int itemIndex,
						 OffsetNumber offnum, IndexTuple itup);
static int	_treeb_setuppostingitems(TreebScanOpaque so, int itemIndex,
								  OffsetNumber offnum, ItemPointer heapTid,
								  IndexTuple itup);
static inline void _treeb_savepostingitem(TreebScanOpaque so, int itemIndex,
									   OffsetNumber offnum,
									   ItemPointer heapTid, int tupleOffset);
static bool _treeb_steppage(IndexScanDesc scan, ScanDirection dir);
static bool _treeb_readnextpage(IndexScanDesc scan, BlockNumber blkno, ScanDirection dir);
static bool _treeb_parallel_readpage(IndexScanDesc scan, BlockNumber blkno,
								  ScanDirection dir);
static Buffer _treeb_walk_left(Relation rel, Buffer buf);
static bool _treeb_endpoint(IndexScanDesc scan, ScanDirection dir);
static inline void _treeb_initialize_more_data(TreebScanOpaque so, ScanDirection dir);


/*
 *	_treeb_drop_lock_and_maybe_pin()
 *
 * Unlock the buffer; and if it is safe to release the pin, do that, too.
 * This will prevent vacuum from stalling in a blocked state trying to read a
 * page when a cursor is sitting on it.
 *
 * See treeb/README section on making concurrent TID recycling safe.
 */
static void
_treeb_drop_lock_and_maybe_pin(IndexScanDesc scan, TreebScanPos sp)
{
	_treeb_unlockbuf(scan->indexRelation, sp->buf);

	if (IsMVCCSnapshot(scan->xs_snapshot) &&
		RelationNeedsWAL(scan->indexRelation) &&
		!scan->xs_want_itup)
	{
		ReleaseBuffer(sp->buf);
		sp->buf = InvalidBuffer;
	}
}

/*
 *	_treeb_search() -- Search the tree for a particular scankey,
 *		or more precisely for the first leaf page it could be on.
 *
 * The passed scankey is an insertion-type scankey (see treeb/README),
 * but it can omit the rightmost column(s) of the index.
 *
 * Return value is a stack of parent-page pointers (i.e. there is no entry for
 * the leaf level/page).  *bufP is set to the address of the leaf-page buffer,
 * which is locked and pinned.  No locks are held on the parent pages,
 * however!
 *
 * The returned buffer is locked according to access parameter.  Additionally,
 * access = TREEB_WRITE will allow an empty root page to be created and returned.
 * When access = TREEB_READ, an empty index will result in *bufP being set to
 * InvalidBuffer.  Also, in TREEB_WRITE mode, any incomplete splits encountered
 * during the search will be finished.
 *
 * heaprel must be provided by callers that pass access = TREEB_WRITE, since we
 * might need to allocate a new root page for caller -- see _treeb_allocbuf.
 */
TreebStack
_treeb_search(Relation rel, Relation heaprel, TreebScanInsert key, Buffer *bufP,
		   int access)
{
	TreebStack		stack_in = NULL;
	int			page_access = TREEB_READ;

	/* heaprel must be set whenever _treeb_allocbuf is reachable */
	Assert(access == TREEB_READ || access == TREEB_WRITE);
	Assert(access == TREEB_READ || heaprel != NULL);

	/* Get the root page to start with */
	*bufP = _treeb_getroot(rel, heaprel, access);

	/* If index is empty and access = TREEB_READ, no root page is created. */
	if (!BufferIsValid(*bufP))
		return (TreebStack) NULL;

	/* Loop iterates once per level descended in the tree */
	for (;;)
	{
		Page		page;
		TreebPageOpaque opaque;
		OffsetNumber offnum;
		ItemId		itemid;
		IndexTuple	itup;
		BlockNumber child;
		TreebStack		new_stack;

		/*
		 * Race -- the page we just grabbed may have split since we read its
		 * downlink in its parent page (or the metapage).  If it has, we may
		 * need to move right to its new sibling.  Do that.
		 *
		 * In write-mode, allow _treeb_moveright to finish any incomplete splits
		 * along the way.  Strictly speaking, we'd only need to finish an
		 * incomplete split on the leaf page we're about to insert to, not on
		 * any of the upper levels (internal pages with incomplete splits are
		 * also taken care of in _treeb_getstackbuf).  But this is a good
		 * opportunity to finish splits of internal pages too.
		 */
		*bufP = _treeb_moveright(rel, heaprel, key, *bufP, (access == TREEB_WRITE),
							  stack_in, page_access);

		/* if this is a leaf page, we're done */
		page = BufferGetPage(*bufP);
		opaque = TREEBPageGetOpaque(page);
		if (P_ISLEAF(opaque))
			break;

		/*
		 * Find the appropriate pivot tuple on this page.  Its downlink points
		 * to the child page that we're about to descend to.
		 */
		offnum = _treeb_binsrch(rel, key, *bufP);
		itemid = PageGetItemId(page, offnum);
		itup = (IndexTuple) PageGetItem(page, itemid);
		Assert(TreebTupleIsPivot(itup) || !key->heapkeyspace);
		child = TreebTupleGetDownLink(itup);

		/*
		 * We need to save the location of the pivot tuple we chose in a new
		 * stack entry for this page/level.  If caller ends up splitting a
		 * page one level down, it usually ends up inserting a new pivot
		 * tuple/downlink immediately after the location recorded here.
		 */
		new_stack = (TreebStack) palloc(sizeof(TreebStackData));
		new_stack->treebs_blkno = BufferGetBlockNumber(*bufP);
		new_stack->treebs_offset = offnum;
		new_stack->treebs_parent = stack_in;

		/*
		 * Page level 1 is lowest non-leaf page level prior to leaves.  So, if
		 * we're on the level 1 and asked to lock leaf page in write mode,
		 * then lock next page in write mode, because it must be a leaf.
		 */
		if (opaque->treebpo_level == 1 && access == TREEB_WRITE)
			page_access = TREEB_WRITE;

		/* drop the read lock on the page, then acquire one on its child */
		*bufP = _treeb_relandgetbuf(rel, *bufP, child, page_access);

		/* okay, all set to move down a level */
		stack_in = new_stack;
	}

	/*
	 * If we're asked to lock leaf in write mode, but didn't manage to, then
	 * relock.  This should only happen when the root page is a leaf page (and
	 * the only page in the index other than the metapage).
	 */
	if (access == TREEB_WRITE && page_access == TREEB_READ)
	{
		/* trade in our read lock for a write lock */
		_treeb_unlockbuf(rel, *bufP);
		_treeb_lockbuf(rel, *bufP, TREEB_WRITE);

		/*
		 * Race -- the leaf page may have split after we dropped the read lock
		 * but before we acquired a write lock.  If it has, we may need to
		 * move right to its new sibling.  Do that.
		 */
		*bufP = _treeb_moveright(rel, heaprel, key, *bufP, true, stack_in, TREEB_WRITE);
	}

	return stack_in;
}

/*
 *	_treeb_moveright() -- move right in the treeb if necessary.
 *
 * When we follow a pointer to reach a page, it is possible that
 * the page has changed in the meanwhile.  If this happens, we're
 * guaranteed that the page has "split right" -- that is, that any
 * data that appeared on the page originally is either on the page
 * or strictly to the right of it.
 *
 * This routine decides whether or not we need to move right in the
 * tree by examining the high key entry on the page.  If that entry is
 * strictly less than the scankey, or <= the scankey in the
 * key.nextkey=true case, then we followed the wrong link and we need
 * to move right.
 *
 * The passed insertion-type scankey can omit the rightmost column(s) of the
 * index. (see treeb/README)
 *
 * When key.nextkey is false (the usual case), we are looking for the first
 * item >= key.  When key.nextkey is true, we are looking for the first item
 * strictly greater than key.
 *
 * If forupdate is true, we will attempt to finish any incomplete splits
 * that we encounter.  This is required when locking a target page for an
 * insertion, because we don't allow inserting on a page before the split is
 * completed.  'heaprel' and 'stack' are only used if forupdate is true.
 *
 * On entry, we have the buffer pinned and a lock of the type specified by
 * 'access'.  If we move right, we release the buffer and lock and acquire
 * the same on the right sibling.  Return value is the buffer we stop at.
 */
Buffer
_treeb_moveright(Relation rel,
			  Relation heaprel,
			  TreebScanInsert key,
			  Buffer buf,
			  bool forupdate,
			  TreebStack stack,
			  int access)
{
	Page		page;
	TreebPageOpaque opaque;
	int32		cmpval;

	Assert(!forupdate || heaprel != NULL);

	/*
	 * When nextkey = false (normal case): if the scan key that brought us to
	 * this page is > the high key stored on the page, then the page has split
	 * and we need to move right.  (pg_upgrade'd !heapkeyspace indexes could
	 * have some duplicates to the right as well as the left, but that's
	 * something that's only ever dealt with on the leaf level, after
	 * _treeb_search has found an initial leaf page.)
	 *
	 * When nextkey = true: move right if the scan key is >= page's high key.
	 * (Note that key.scantid cannot be set in this case.)
	 *
	 * The page could even have split more than once, so scan as far as
	 * needed.
	 *
	 * We also have to move right if we followed a link that brought us to a
	 * dead page.
	 */
	cmpval = key->nextkey ? 0 : 1;

	for (;;)
	{
		page = BufferGetPage(buf);
		opaque = TREEBPageGetOpaque(page);

		if (P_RIGHTMOST(opaque))
			break;

		/*
		 * Finish any incomplete splits we encounter along the way.
		 */
		if (forupdate && P_INCOMPLETE_SPLIT(opaque))
		{
			BlockNumber blkno = BufferGetBlockNumber(buf);

			/* upgrade our lock if necessary */
			if (access == TREEB_READ)
			{
				_treeb_unlockbuf(rel, buf);
				_treeb_lockbuf(rel, buf, TREEB_WRITE);
			}

			if (P_INCOMPLETE_SPLIT(opaque))
				_treeb_finish_split(rel, heaprel, buf, stack);
			else
				_treeb_relbuf(rel, buf);

			/* re-acquire the lock in the right mode, and re-check */
			buf = _treeb_getbuf(rel, blkno, access);
			continue;
		}

		if (P_IGNORE(opaque) || _treeb_compare(rel, key, page, P_HIKEY) >= cmpval)
		{
			/* step right one page */
			buf = _treeb_relandgetbuf(rel, buf, opaque->treebpo_next, access);
			continue;
		}
		else
			break;
	}

	if (P_IGNORE(opaque))
		elog(ERROR, "fell off the end of index \"%s\"",
			 RelationGetRelationName(rel));

	return buf;
}

/*
 *	_treeb_binsrch() -- Do a binary search for a key on a particular page.
 *
 * On an internal (non-leaf) page, _treeb_binsrch() returns the OffsetNumber
 * of the last key < given scankey, or last key <= given scankey if nextkey
 * is true.  (Since _treeb_compare treats the first data key of such a page as
 * minus infinity, there will be at least one key < scankey, so the result
 * always points at one of the keys on the page.)
 *
 * On a leaf page, _treeb_binsrch() returns the final result of the initial
 * positioning process that started with _treeb_first's call to _treeb_search.
 * We're returning a non-pivot tuple offset, so things are a little different.
 * It is possible that we'll return an offset that's either past the last
 * non-pivot slot, or (in the case of a backward scan) before the first slot.
 *
 * This procedure is not responsible for walking right, it just examines
 * the given page.  _treeb_binsrch() has no lock or refcount side effects
 * on the buffer.
 */
static OffsetNumber
_treeb_binsrch(Relation rel,
			TreebScanInsert key,
			Buffer buf)
{
	Page		page;
	TreebPageOpaque opaque;
	OffsetNumber low,
				high;
	int32		result,
				cmpval;

	page = BufferGetPage(buf);
	opaque = TREEBPageGetOpaque(page);

	/* Requesting nextkey semantics while using scantid seems nonsensical */
	Assert(!key->nextkey || key->scantid == NULL);
	/* scantid-set callers must use _treeb_binsrch_insert() on leaf pages */
	Assert(!P_ISLEAF(opaque) || key->scantid == NULL);

	low = P_FIRSTDATAKEY(opaque);
	high = PageGetMaxOffsetNumber(page);

	/*
	 * If there are no keys on the page, return the first available slot. Note
	 * this covers two cases: the page is really empty (no keys), or it
	 * contains only a high key.  The latter case is possible after vacuuming.
	 * This can never happen on an internal page, however, since they are
	 * never empty (an internal page must have at least one child).
	 */
	if (unlikely(high < low))
		return low;

	/*
	 * Binary search to find the first key on the page >= scan key, or first
	 * key > scankey when nextkey is true.
	 *
	 * For nextkey=false (cmpval=1), the loop invariant is: all slots before
	 * 'low' are < scan key, all slots at or after 'high' are >= scan key.
	 *
	 * For nextkey=true (cmpval=0), the loop invariant is: all slots before
	 * 'low' are <= scan key, all slots at or after 'high' are > scan key.
	 *
	 * We can fall out when high == low.
	 */
	high++;						/* establish the loop invariant for high */

	cmpval = key->nextkey ? 0 : 1;	/* select comparison value */

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		/* We have low <= mid < high, so mid points at a real slot */

		result = _treeb_compare(rel, key, page, mid);

		if (result >= cmpval)
			low = mid + 1;
		else
			high = mid;
	}

	/*
	 * At this point we have high == low.
	 *
	 * On a leaf page we always return the first non-pivot tuple >= scan key
	 * (resp. > scan key) for forward scan callers.  For backward scans, it's
	 * always the _last_ non-pivot tuple < scan key (resp. <= scan key).
	 */
	if (P_ISLEAF(opaque))
	{
		/*
		 * In the backward scan case we're supposed to locate the last
		 * matching tuple on the leaf level -- not the first matching tuple
		 * (the last tuple will be the first one returned by the scan).
		 *
		 * At this point we've located the first non-pivot tuple immediately
		 * after the last matching tuple (which might just be maxoff + 1).
		 * Compensate by stepping back.
		 */
		if (key->backward)
			return OffsetNumberPrev(low);

		return low;
	}

	/*
	 * On a non-leaf page, return the last key < scan key (resp. <= scan key).
	 * There must be one if _treeb_compare() is playing by the rules.
	 *
	 * _treeb_compare() will seldom see any exactly-matching pivot tuples, since
	 * a truncated -inf heap TID is usually enough to prevent it altogether.
	 * Even omitted scan key entries are treated as > truncated attributes.
	 *
	 * However, during backward scans _treeb_compare() interprets omitted scan
	 * key attributes as == corresponding truncated -inf attributes instead.
	 * This works just like < would work here.  Under this scheme, < strategy
	 * backward scans will always directly descend to the correct leaf page.
	 * In particular, they will never incur an "extra" leaf page access with a
	 * scan key that happens to contain the same prefix of values as some
	 * pivot tuple's untruncated prefix.  VACUUM relies on this guarantee when
	 * it uses a leaf page high key to "re-find" a page undergoing deletion.
	 */
	Assert(low > P_FIRSTDATAKEY(opaque));

	return OffsetNumberPrev(low);
}

/*
 *
 *	_treeb_binsrch_insert() -- Cacheable, incremental leaf page binary search.
 *
 * Like _treeb_binsrch(), but with support for caching the binary search
 * bounds.  Only used during insertion, and only on the leaf page that it
 * looks like caller will insert tuple on.  Exclusive-locked and pinned
 * leaf page is contained within insertstate.
 *
 * Caches the bounds fields in insertstate so that a subsequent call can
 * reuse the low and strict high bounds of original binary search.  Callers
 * that use these fields directly must be prepared for the case where low
 * and/or stricthigh are not on the same page (one or both exceed maxoff
 * for the page).  The case where there are no items on the page (high <
 * low) makes bounds invalid.
 *
 * Caller is responsible for invalidating bounds when it modifies the page
 * before calling here a second time, and for dealing with posting list
 * tuple matches (callers can use insertstate's postingoff field to
 * determine which existing heap TID will need to be replaced by a posting
 * list split).
 */
OffsetNumber
_treeb_binsrch_insert(Relation rel, TreebInsertState insertstate)
{
	TreebScanInsert key = insertstate->itup_key;
	Page		page;
	TreebPageOpaque opaque;
	OffsetNumber low,
				high,
				stricthigh;
	int32		result,
				cmpval;

	page = BufferGetPage(insertstate->buf);
	opaque = TREEBPageGetOpaque(page);

	Assert(P_ISLEAF(opaque));
	Assert(!key->nextkey);
	Assert(insertstate->postingoff == 0);

	if (!insertstate->bounds_valid)
	{
		/* Start new binary search */
		low = P_FIRSTDATAKEY(opaque);
		high = PageGetMaxOffsetNumber(page);
	}
	else
	{
		/* Restore result of previous binary search against same page */
		low = insertstate->low;
		high = insertstate->stricthigh;
	}

	/* If there are no keys on the page, return the first available slot */
	if (unlikely(high < low))
	{
		/* Caller can't reuse bounds */
		insertstate->low = InvalidOffsetNumber;
		insertstate->stricthigh = InvalidOffsetNumber;
		insertstate->bounds_valid = false;
		return low;
	}

	/*
	 * Binary search to find the first key on the page >= scan key. (nextkey
	 * is always false when inserting).
	 *
	 * The loop invariant is: all slots before 'low' are < scan key, all slots
	 * at or after 'high' are >= scan key.  'stricthigh' is > scan key, and is
	 * maintained to save additional search effort for caller.
	 *
	 * We can fall out when high == low.
	 */
	if (!insertstate->bounds_valid)
		high++;					/* establish the loop invariant for high */
	stricthigh = high;			/* high initially strictly higher */

	cmpval = 1;					/* !nextkey comparison value */

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		/* We have low <= mid < high, so mid points at a real slot */

		result = _treeb_compare(rel, key, page, mid);

		if (result >= cmpval)
			low = mid + 1;
		else
		{
			high = mid;
			if (result != 0)
				stricthigh = high;
		}

		/*
		 * If tuple at offset located by binary search is a posting list whose
		 * TID range overlaps with caller's scantid, perform posting list
		 * binary search to set postingoff for caller.  Caller must split the
		 * posting list when postingoff is set.  This should happen
		 * infrequently.
		 */
		if (unlikely(result == 0 && key->scantid != NULL))
		{
			/*
			 * postingoff should never be set more than once per leaf page
			 * binary search.  That would mean that there are duplicate table
			 * TIDs in the index, which is never okay.  Check for that here.
			 */
			if (insertstate->postingoff != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg_internal("table tid from new index tuple (%u,%u) cannot find insert offset between offsets %u and %u of block %u in index \"%s\"",
										 ItemPointerGetBlockNumber(key->scantid),
										 ItemPointerGetOffsetNumber(key->scantid),
										 low, stricthigh,
										 BufferGetBlockNumber(insertstate->buf),
										 RelationGetRelationName(rel))));

			insertstate->postingoff = _treeb_binsrch_posting(key, page, mid);
		}
	}

	/*
	 * On a leaf page, a binary search always returns the first key >= scan
	 * key (at least in !nextkey case), which could be the last slot + 1. This
	 * is also the lower bound of cached search.
	 *
	 * stricthigh may also be the last slot + 1, which prevents caller from
	 * using bounds directly, but is still useful to us if we're called a
	 * second time with cached bounds (cached low will be < stricthigh when
	 * that happens).
	 */
	insertstate->low = low;
	insertstate->stricthigh = stricthigh;
	insertstate->bounds_valid = true;

	return low;
}

/*----------
 *	_treeb_binsrch_posting() -- posting list binary search.
 *
 * Helper routine for _treeb_binsrch_insert().
 *
 * Returns offset into posting list where caller's scantid belongs.
 *----------
 */
static int
_treeb_binsrch_posting(TreebScanInsert key, Page page, OffsetNumber offnum)
{
	IndexTuple	itup;
	ItemId		itemid;
	int			low,
				high,
				mid,
				res;

	/*
	 * If this isn't a posting tuple, then the index must be corrupt (if it is
	 * an ordinary non-pivot tuple then there must be an existing tuple with a
	 * heap TID that equals inserter's new heap TID/scantid).  Defensively
	 * check that tuple is a posting list tuple whose posting list range
	 * includes caller's scantid.
	 *
	 * (This is also needed because contrib/amcheck's rootdescend option needs
	 * to be able to relocate a non-pivot tuple using _treeb_binsrch_insert().)
	 */
	itemid = PageGetItemId(page, offnum);
	itup = (IndexTuple) PageGetItem(page, itemid);
	if (!TreebTupleIsPosting(itup))
		return 0;

	Assert(key->heapkeyspace && key->allequalimage);

	/*
	 * In the event that posting list tuple has LP_DEAD bit set, indicate this
	 * to _treeb_binsrch_insert() caller by returning -1, a sentinel value.  A
	 * second call to _treeb_binsrch_insert() can take place when its caller has
	 * removed the dead item.
	 */
	if (ItemIdIsDead(itemid))
		return -1;

	/* "high" is past end of posting list for loop invariant */
	low = 0;
	high = TreebTupleGetNPosting(itup);
	Assert(high >= 2);

	while (high > low)
	{
		mid = low + ((high - low) / 2);
		res = ItemPointerCompare(key->scantid,
								 TreebTupleGetPostingN(itup, mid));

		if (res > 0)
			low = mid + 1;
		else if (res < 0)
			high = mid;
		else
			return mid;
	}

	/* Exact match not found */
	return low;
}

/*----------
 *	_treeb_compare() -- Compare insertion-type scankey to tuple on a page.
 *
 *	page/offnum: location of treeb item to be compared to.
 *
 *		This routine returns:
 *			<0 if scankey < tuple at offnum;
 *			 0 if scankey == tuple at offnum;
 *			>0 if scankey > tuple at offnum.
 *
 * NULLs in the keys are treated as sortable values.  Therefore
 * "equality" does not necessarily mean that the item should be returned
 * to the caller as a matching key.  Similarly, an insertion scankey
 * with its scantid set is treated as equal to a posting tuple whose TID
 * range overlaps with their scantid.  There generally won't be a
 * matching TID in the posting tuple, which caller must handle
 * themselves (e.g., by splitting the posting list tuple).
 *
 * CRUCIAL NOTE: on a non-leaf page, the first data key is assumed to be
 * "minus infinity": this routine will always claim it is less than the
 * scankey.  The actual key value stored is explicitly truncated to 0
 * attributes (explicitly minus infinity) with version 3+ indexes, but
 * that isn't relied upon.  This allows us to implement the Lehman and
 * Yao convention that the first down-link pointer is before the first
 * key.  See backend/access/treeb/README for details.
 *----------
 */
int32
_treeb_compare(Relation rel,
			TreebScanInsert key,
			Page page,
			OffsetNumber offnum)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	TreebPageOpaque opaque = TREEBPageGetOpaque(page);
	IndexTuple	itup;
	ItemPointer heapTid;
	ScanKey		scankey;
	int			ncmpkey;
	int			ntupatts;
	int32		result;

	Assert(_treeb_check_natts(rel, key->heapkeyspace, page, offnum));
	Assert(key->keysz <= IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(key->heapkeyspace || key->scantid == NULL);

	/*
	 * Force result ">" if target item is first data item on an internal page
	 * --- see NOTE above.
	 */
	if (!P_ISLEAF(opaque) && offnum == P_FIRSTDATAKEY(opaque))
		return 1;

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	ntupatts = TreebTupleGetNAtts(itup, rel);

	/*
	 * The scan key is set up with the attribute number associated with each
	 * term in the key.  It is important that, if the index is multi-key, the
	 * scan contain the first k key attributes, and that they be in order.  If
	 * you think about how multi-key ordering works, you'll understand why
	 * this is.
	 *
	 * We don't test for violation of this condition here, however.  The
	 * initial setup for the index scan had better have gotten it right (see
	 * _treeb_first).
	 */

	ncmpkey = Min(ntupatts, key->keysz);
	Assert(key->heapkeyspace || ncmpkey == key->keysz);
	Assert(!TreebTupleIsPosting(itup) || key->allequalimage);
	scankey = key->scankeys;
	for (int i = 1; i <= ncmpkey; i++)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(itup, scankey->sk_attno, itupdesc, &isNull);

		if (scankey->sk_flags & SK_ISNULL)	/* key is NULL */
		{
			if (isNull)
				result = 0;		/* NULL "=" NULL */
			else if (scankey->sk_flags & SK_TREEB_NULLS_FIRST)
				result = -1;	/* NULL "<" NOT_NULL */
			else
				result = 1;		/* NULL ">" NOT_NULL */
		}
		else if (isNull)		/* key is NOT_NULL and item is NULL */
		{
			if (scankey->sk_flags & SK_TREEB_NULLS_FIRST)
				result = 1;		/* NOT_NULL ">" NULL */
			else
				result = -1;	/* NOT_NULL "<" NULL */
		}
		else
		{
			/*
			 * The sk_func needs to be passed the index value as left arg and
			 * the sk_argument as right arg (they might be of different
			 * types).  Since it is convenient for callers to think of
			 * _treeb_compare as comparing the scankey to the index item, we have
			 * to flip the sign of the comparison result.  (Unless it's a DESC
			 * column, in which case we *don't* flip the sign.)
			 */
			result = DatumGetInt32(FunctionCall2Coll(&scankey->sk_func,
													 scankey->sk_collation,
													 datum,
													 scankey->sk_argument));

			if (!(scankey->sk_flags & SK_TREEB_DESC))
				INVERT_COMPARE_RESULT(result);
		}

		/* if the keys are unequal, return the difference */
		if (result != 0)
			return result;

		scankey++;
	}

	/*
	 * All non-truncated attributes (other than heap TID) were found to be
	 * equal.  Treat truncated attributes as minus infinity when scankey has a
	 * key attribute value that would otherwise be compared directly.
	 *
	 * Note: it doesn't matter if ntupatts includes non-key attributes;
	 * scankey won't, so explicitly excluding non-key attributes isn't
	 * necessary.
	 */
	if (key->keysz > ntupatts)
		return 1;

	/*
	 * Use the heap TID attribute and scantid to try to break the tie.  The
	 * rules are the same as any other key attribute -- only the
	 * representation differs.
	 */
	heapTid = TreebTupleGetHeapTID(itup);
	if (key->scantid == NULL)
	{
		/*
		 * Forward scans have a scankey that is considered greater than a
		 * truncated pivot tuple if and when the scankey has equal values for
		 * attributes up to and including the least significant untruncated
		 * attribute in tuple.  Even attributes that were omitted from the
		 * scan key are considered greater than -inf truncated attributes.
		 * (See _treeb_binsrch for an explanation of our backward scan behavior.)
		 *
		 * For example, if an index has the minimum two attributes (single
		 * user key attribute, plus heap TID attribute), and a page's high key
		 * is ('foo', -inf), and scankey is ('foo', <omitted>), the search
		 * will not descend to the page to the left.  The search will descend
		 * right instead.  The truncated attribute in pivot tuple means that
		 * all non-pivot tuples on the page to the left are strictly < 'foo',
		 * so it isn't necessary to descend left.  In other words, search
		 * doesn't have to descend left because it isn't interested in a match
		 * that has a heap TID value of -inf.
		 *
		 * Note: the heap TID part of the test ensures that scankey is being
		 * compared to a pivot tuple with one or more truncated -inf key
		 * attributes.  The heap TID attribute is the last key attribute in
		 * every index, of course, but other than that it isn't special.
		 */
		if (!key->backward && key->keysz == ntupatts && heapTid == NULL &&
			key->heapkeyspace)
			return 1;

		/* All provided scankey arguments found to be equal */
		return 0;
	}

	/*
	 * Treat truncated heap TID as minus infinity, since scankey has a key
	 * attribute value (scantid) that would otherwise be compared directly
	 */
	Assert(key->keysz == IndexRelationGetNumberOfKeyAttributes(rel));
	if (heapTid == NULL)
		return 1;

	/*
	 * Scankey must be treated as equal to a posting list tuple if its scantid
	 * value falls within the range of the posting list.  In all other cases
	 * there can only be a single heap TID value, which is compared directly
	 * with scantid.
	 */
	Assert(ntupatts >= IndexRelationGetNumberOfKeyAttributes(rel));
	result = ItemPointerCompare(key->scantid, heapTid);
	if (result <= 0 || !TreebTupleIsPosting(itup))
		return result;
	else
	{
		result = ItemPointerCompare(key->scantid,
									TreebTupleGetMaxHeapTID(itup));
		if (result > 0)
			return 1;
	}

	return 0;
}

/*
 *	_treeb_first() -- Find the first item in a scan.
 *
 *		We need to be clever about the direction of scan, the search
 *		conditions, and the tree ordering.  We find the first item (or,
 *		if backwards scan, the last item) in the tree that satisfies the
 *		qualifications in the scan key.  On success exit, the page containing
 *		the current index tuple is pinned but not locked, and data about
 *		the matching tuple(s) on the page has been loaded into so->currPos.
 *		scan->xs_heaptid is set to the heap TID of the current tuple, and if
 *		requested, scan->xs_itup points to a copy of the index tuple.
 *
 * If there are no matching items in the index, we return false, with no
 * pins or locks held.
 *
 * Note that scan->keyData[], and the so->keyData[] scankey built from it,
 * are both search-type scankeys (see treeb/README for more about this).
 * Within this routine, we build a temporary insertion-type scankey to use
 * in locating the scan start position.
 */
bool
_treeb_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	Buffer		buf;
	TreebStack		stack;
	OffsetNumber offnum;
	StrategyNumber strat;
	TreebScanInsertData inskey;
	ScanKey		startKeys[INDEX_MAX_KEYS];
	ScanKeyData notnullkeys[INDEX_MAX_KEYS];
	int			keysz = 0;
	int			i;
	bool		status;
	StrategyNumber strat_total;
	TreebScanPosItem *currItem;
	BlockNumber blkno;

	Assert(!TreebScanPosIsValid(so->currPos));

	pgstat_count_index_scan(rel);

	/*
	 * Examine the scan keys and eliminate any redundant keys; also mark the
	 * keys that must be matched to continue the scan.
	 */
	_treeb_preprocess_keys(scan);

	/*
	 * Quit now if _treeb_preprocess_keys() discovered that the scan keys can
	 * never be satisfied (eg, x == 1 AND x > 2).
	 */
	if (!so->qual_ok)
	{
		_treeb_parallel_done(scan);
		return false;
	}

	/*
	 * For parallel scans, get the starting page from shared state. If the
	 * scan has not started, proceed to find out first leaf page in the usual
	 * way while keeping other participating processes waiting.  If the scan
	 * has already begun, use the page number from the shared structure.
	 *
	 * When a parallel scan has another primitive index scan scheduled, a
	 * parallel worker will seize the scan for that purpose now.  This is
	 * similar to the case where the top-level scan hasn't started.
	 */
	if (scan->parallel_scan != NULL)
	{
		status = _treeb_parallel_seize(scan, &blkno, true);

		/*
		 * Initialize arrays (when _treeb_parallel_seize didn't already set up
		 * the next primitive index scan)
		 */
		if (so->numArrayKeys && !so->needPrimScan)
			_treeb_start_array_keys(scan, dir);

		if (!status)
			return false;
		else if (blkno == P_NONE)
		{
			_treeb_parallel_done(scan);
			return false;
		}
		else if (blkno != InvalidBlockNumber)
		{
			if (!_treeb_parallel_readpage(scan, blkno, dir))
				return false;
			goto readcomplete;
		}
	}
	else if (so->numArrayKeys && !so->needPrimScan)
	{
		/*
		 * First _treeb_first call (for current treebrescan) without parallelism.
		 *
		 * Initialize arrays, and the corresponding scan keys that were just
		 * output by _treeb_preprocess_keys.
		 */
		_treeb_start_array_keys(scan, dir);
	}

	/*----------
	 * Examine the scan keys to discover where we need to start the scan.
	 *
	 * We want to identify the keys that can be used as starting boundaries;
	 * these are =, >, or >= keys for a forward scan or =, <, <= keys for
	 * a backwards scan.  We can use keys for multiple attributes so long as
	 * the prior attributes had only =, >= (resp. =, <=) keys.  Once we accept
	 * a > or < boundary or find an attribute with no boundary (which can be
	 * thought of as the same as "> -infinity"), we can't use keys for any
	 * attributes to its right, because it would break our simplistic notion
	 * of what initial positioning strategy to use.
	 *
	 * When the scan keys include cross-type operators, _treeb_preprocess_keys
	 * may not be able to eliminate redundant keys; in such cases we will
	 * arbitrarily pick a usable one for each attribute.  This is correct
	 * but possibly not optimal behavior.  (For example, with keys like
	 * "x >= 4 AND x >= 5" we would elect to scan starting at x=4 when
	 * x=5 would be more efficient.)  Since the situation only arises given
	 * a poorly-worded query plus an incomplete opfamily, live with it.
	 *
	 * When both equality and inequality keys appear for a single attribute
	 * (again, only possible when cross-type operators appear), we *must*
	 * select one of the equality keys for the starting point, because
	 * _treeb_checkkeys() will stop the scan as soon as an equality qual fails.
	 * For example, if we have keys like "x >= 4 AND x = 10" and we elect to
	 * start at x=4, we will fail and stop before reaching x=10.  If multiple
	 * equality quals survive preprocessing, however, it doesn't matter which
	 * one we use --- by definition, they are either redundant or
	 * contradictory.
	 *
	 * Any regular (not SK_SEARCHNULL) key implies a NOT NULL qualifier.
	 * If the index stores nulls at the end of the index we'll be starting
	 * from, and we have no boundary key for the column (which means the key
	 * we deduced NOT NULL from is an inequality key that constrains the other
	 * end of the index), then we cons up an explicit SK_SEARCHNOTNULL key to
	 * use as a boundary key.  If we didn't do this, we might find ourselves
	 * traversing a lot of null entries at the start of the scan.
	 *
	 * In this loop, row-comparison keys are treated the same as keys on their
	 * first (leftmost) columns.  We'll add on lower-order columns of the row
	 * comparison below, if possible.
	 *
	 * The selected scan keys (at most one per index column) are remembered by
	 * storing their addresses into the local startKeys[] array.
	 *
	 * _treeb_checkkeys/_treeb_advance_array_keys decide whether and when to start
	 * the next primitive index scan (for scans with array keys) based in part
	 * on an understanding of how it'll enable us to reposition the scan.
	 * They're directly aware of how we'll sometimes cons up an explicit
	 * SK_SEARCHNOTNULL key.  They'll even end primitive scans by applying a
	 * symmetric "deduce NOT NULL" rule of their own.  This allows top-level
	 * scans to skip large groups of NULLs through repeated deductions about
	 * key strictness (for a required inequality key) and whether NULLs in the
	 * key's index column are stored last or first (relative to non-NULLs).
	 * If you update anything here, _treeb_checkkeys/_treeb_advance_array_keys might
	 * need to be kept in sync.
	 *----------
	 */
	strat_total = TreebEqualStrategyNumber;
	if (so->numberOfKeys > 0)
	{
		AttrNumber	curattr;
		ScanKey		chosen;
		ScanKey		impliesNN;
		ScanKey		cur;

		/*
		 * chosen is the so-far-chosen key for the current attribute, if any.
		 * We don't cast the decision in stone until we reach keys for the
		 * next attribute.
		 */
		curattr = 1;
		chosen = NULL;
		/* Also remember any scankey that implies a NOT NULL constraint */
		impliesNN = NULL;

		/*
		 * Loop iterates from 0 to numberOfKeys inclusive; we use the last
		 * pass to handle after-last-key processing.  Actual exit from the
		 * loop is at one of the "break" statements below.
		 */
		for (cur = so->keyData, i = 0;; cur++, i++)
		{
			if (i >= so->numberOfKeys || cur->sk_attno != curattr)
			{
				/*
				 * Done looking at keys for curattr.  If we didn't find a
				 * usable boundary key, see if we can deduce a NOT NULL key.
				 */
				if (chosen == NULL && impliesNN != NULL &&
					((impliesNN->sk_flags & SK_TREEB_NULLS_FIRST) ?
					 ScanDirectionIsForward(dir) :
					 ScanDirectionIsBackward(dir)))
				{
					/* Yes, so build the key in notnullkeys[keysz] */
					chosen = &notnullkeys[keysz];
					ScanKeyEntryInitialize(chosen,
										   (SK_SEARCHNOTNULL | SK_ISNULL |
											(impliesNN->sk_flags &
											 (SK_TREEB_DESC | SK_TREEB_NULLS_FIRST))),
										   curattr,
										   ((impliesNN->sk_flags & SK_TREEB_NULLS_FIRST) ?
											TreebGreaterStrategyNumber :
											TreebLessStrategyNumber),
										   InvalidOid,
										   InvalidOid,
										   InvalidOid,
										   (Datum) 0);
				}

				/*
				 * If we still didn't find a usable boundary key, quit; else
				 * save the boundary key pointer in startKeys.
				 */
				if (chosen == NULL)
					break;
				startKeys[keysz++] = chosen;

				/*
				 * Adjust strat_total, and quit if we have stored a > or <
				 * key.
				 */
				strat = chosen->sk_strategy;
				if (strat != TreebEqualStrategyNumber)
				{
					strat_total = strat;
					if (strat == TreebGreaterStrategyNumber ||
						strat == TreebLessStrategyNumber)
						break;
				}

				/*
				 * Done if that was the last attribute, or if next key is not
				 * in sequence (implying no boundary key is available for the
				 * next attribute).
				 */
				if (i >= so->numberOfKeys ||
					cur->sk_attno != curattr + 1)
					break;

				/*
				 * Reset for next attr.
				 */
				curattr = cur->sk_attno;
				chosen = NULL;
				impliesNN = NULL;
			}

			/*
			 * Can we use this key as a starting boundary for this attr?
			 *
			 * If not, does it imply a NOT NULL constraint?  (Because
			 * SK_SEARCHNULL keys are always assigned TreebEqualStrategyNumber,
			 * *any* inequality key works for that; we need not test.)
			 */
			switch (cur->sk_strategy)
			{
				case TreebLessStrategyNumber:
				case TreebLessEqualStrategyNumber:
					if (chosen == NULL)
					{
						if (ScanDirectionIsBackward(dir))
							chosen = cur;
						else
							impliesNN = cur;
					}
					break;
				case TreebEqualStrategyNumber:
					/* override any non-equality choice */
					chosen = cur;
					break;
				case TreebGreaterEqualStrategyNumber:
				case TreebGreaterStrategyNumber:
					if (chosen == NULL)
					{
						if (ScanDirectionIsForward(dir))
							chosen = cur;
						else
							impliesNN = cur;
					}
					break;
			}
		}
	}

	/*
	 * If we found no usable boundary keys, we have to start from one end of
	 * the tree.  Walk down that edge to the first or last key, and scan from
	 * there.
	 */
	if (keysz == 0)
	{
		bool		match;

		match = _treeb_endpoint(scan, dir);

		if (!match)
		{
			/* No match, so mark (parallel) scan finished */
			_treeb_parallel_done(scan);
		}

		return match;
	}

	/*
	 * We want to start the scan somewhere within the index.  Set up an
	 * insertion scankey we can use to search for the boundary point we
	 * identified above.  The insertion scankey is built using the keys
	 * identified by startKeys[].  (Remaining insertion scankey fields are
	 * initialized after initial-positioning strategy is finalized.)
	 */
	Assert(keysz <= INDEX_MAX_KEYS);
	for (i = 0; i < keysz; i++)
	{
		ScanKey		cur = startKeys[i];

		Assert(cur->sk_attno == i + 1);

		if (cur->sk_flags & SK_ROW_HEADER)
		{
			/*
			 * Row comparison header: look to the first row member instead.
			 *
			 * The member scankeys are already in insertion format (ie, they
			 * have sk_func = 3-way-comparison function), but we have to watch
			 * out for nulls, which _treeb_preprocess_keys didn't check. A null
			 * in the first row member makes the condition unmatchable, just
			 * like qual_ok = false.
			 */
			ScanKey		subkey = (ScanKey) DatumGetPointer(cur->sk_argument);

			Assert(subkey->sk_flags & SK_ROW_MEMBER);
			if (subkey->sk_flags & SK_ISNULL)
			{
				_treeb_parallel_done(scan);
				return false;
			}
			memcpy(inskey.scankeys + i, subkey, sizeof(ScanKeyData));

			/*
			 * If the row comparison is the last positioning key we accepted,
			 * try to add additional keys from the lower-order row members.
			 * (If we accepted independent conditions on additional index
			 * columns, we use those instead --- doesn't seem worth trying to
			 * determine which is more restrictive.)  Note that this is OK
			 * even if the row comparison is of ">" or "<" type, because the
			 * condition applied to all but the last row member is effectively
			 * ">=" or "<=", and so the extra keys don't break the positioning
			 * scheme.  But, by the same token, if we aren't able to use all
			 * the row members, then the part of the row comparison that we
			 * did use has to be treated as just a ">=" or "<=" condition, and
			 * so we'd better adjust strat_total accordingly.
			 */
			if (i == keysz - 1)
			{
				bool		used_all_subkeys = false;

				Assert(!(subkey->sk_flags & SK_ROW_END));
				for (;;)
				{
					subkey++;
					Assert(subkey->sk_flags & SK_ROW_MEMBER);
					if (subkey->sk_attno != keysz + 1)
						break;	/* out-of-sequence, can't use it */
					if (subkey->sk_strategy != cur->sk_strategy)
						break;	/* wrong direction, can't use it */
					if (subkey->sk_flags & SK_ISNULL)
						break;	/* can't use null keys */
					Assert(keysz < INDEX_MAX_KEYS);
					memcpy(inskey.scankeys + keysz, subkey,
						   sizeof(ScanKeyData));
					keysz++;
					if (subkey->sk_flags & SK_ROW_END)
					{
						used_all_subkeys = true;
						break;
					}
				}
				if (!used_all_subkeys)
				{
					switch (strat_total)
					{
						case TreebLessStrategyNumber:
							strat_total = TreebLessEqualStrategyNumber;
							break;
						case TreebGreaterStrategyNumber:
							strat_total = TreebGreaterEqualStrategyNumber;
							break;
					}
				}
				break;			/* done with outer loop */
			}
		}
		else
		{
			/*
			 * Ordinary comparison key.  Transform the search-style scan key
			 * to an insertion scan key by replacing the sk_func with the
			 * appropriate treeb comparison function.
			 *
			 * If scankey operator is not a cross-type comparison, we can use
			 * the cached comparison function; otherwise gotta look it up in
			 * the catalogs.  (That can't lead to infinite recursion, since no
			 * indexscan initiated by syscache lookup will use cross-data-type
			 * operators.)
			 *
			 * We support the convention that sk_subtype == InvalidOid means
			 * the opclass input type; this is a hack to simplify life for
			 * ScanKeyInit().
			 */
			if (cur->sk_subtype == rel->rd_opcintype[i] ||
				cur->sk_subtype == InvalidOid)
			{
				FmgrInfo   *procinfo;

				procinfo = index_getprocinfo(rel, cur->sk_attno, TREEBORDER_PROC);
				ScanKeyEntryInitializeWithInfo(inskey.scankeys + i,
											   cur->sk_flags,
											   cur->sk_attno,
											   InvalidStrategy,
											   cur->sk_subtype,
											   cur->sk_collation,
											   procinfo,
											   cur->sk_argument);
			}
			else
			{
				RegProcedure cmp_proc;

				cmp_proc = get_opfamily_proc(rel->rd_opfamily[i],
											 rel->rd_opcintype[i],
											 cur->sk_subtype,
											 TREEBORDER_PROC);
				if (!RegProcedureIsValid(cmp_proc))
					elog(ERROR, "missing support function %d(%u,%u) for attribute %d of index \"%s\"",
						 TREEBORDER_PROC, rel->rd_opcintype[i], cur->sk_subtype,
						 cur->sk_attno, RelationGetRelationName(rel));
				ScanKeyEntryInitialize(inskey.scankeys + i,
									   cur->sk_flags,
									   cur->sk_attno,
									   InvalidStrategy,
									   cur->sk_subtype,
									   cur->sk_collation,
									   cmp_proc,
									   cur->sk_argument);
			}
		}
	}

	/*----------
	 * Examine the selected initial-positioning strategy to determine exactly
	 * where we need to start the scan, and set flag variables to control the
	 * initial descent by _treeb_search (and our _treeb_binsrch call for the leaf
	 * page _treeb_search returns).
	 *----------
	 */
	_treeb_metaversion(rel, &inskey.heapkeyspace, &inskey.allequalimage);
	inskey.anynullkeys = false; /* unused */
	inskey.scantid = NULL;
	inskey.keysz = keysz;
	switch (strat_total)
	{
		case TreebLessStrategyNumber:

			inskey.nextkey = false;
			inskey.backward = true;
			break;

		case TreebLessEqualStrategyNumber:

			inskey.nextkey = true;
			inskey.backward = true;
			break;

		case TreebEqualStrategyNumber:

			/*
			 * If a backward scan was specified, need to start with last equal
			 * item not first one.
			 */
			if (ScanDirectionIsBackward(dir))
			{
				/*
				 * This is the same as the <= strategy
				 */
				inskey.nextkey = true;
				inskey.backward = true;
			}
			else
			{
				/*
				 * This is the same as the >= strategy
				 */
				inskey.nextkey = false;
				inskey.backward = false;
			}
			break;

		case TreebGreaterEqualStrategyNumber:

			/*
			 * Find first item >= scankey
			 */
			inskey.nextkey = false;
			inskey.backward = false;
			break;

		case TreebGreaterStrategyNumber:

			/*
			 * Find first item > scankey
			 */
			inskey.nextkey = true;
			inskey.backward = false;
			break;

		default:
			/* can't get here, but keep compiler quiet */
			elog(ERROR, "unrecognized strat_total: %d", (int) strat_total);
			return false;
	}

	/*
	 * Use the manufactured insertion scan key to descend the tree and
	 * position ourselves on the target leaf page.
	 */
	Assert(ScanDirectionIsBackward(dir) == inskey.backward);
	stack = _treeb_search(rel, NULL, &inskey, &buf, TREEB_READ);

	/* don't need to keep the stack around... */
	_treeb_freestack(stack);

	if (!BufferIsValid(buf))
	{
		/*
		 * We only get here if the index is completely empty. Lock relation
		 * because nothing finer to lock exists.  Without a buffer lock, it's
		 * possible for another transaction to insert data between
		 * _treeb_search() and PredicateLockRelation().  We have to try again
		 * after taking the relation-level predicate lock, to close a narrow
		 * window where we wouldn't scan concurrently inserted tuples, but the
		 * writer wouldn't see our predicate lock.
		 */
		if (IsolationIsSerializable())
		{
			PredicateLockRelation(rel, scan->xs_snapshot);
			stack = _treeb_search(rel, NULL, &inskey, &buf, TREEB_READ);
			_treeb_freestack(stack);
		}

		if (!BufferIsValid(buf))
		{
			/*
			 * Mark parallel scan as done, so that all the workers can finish
			 * their scan.
			 */
			_treeb_parallel_done(scan);
			TreebScanPosInvalidate(so->currPos);
			return false;
		}
	}

	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);

	_treeb_initialize_more_data(so, dir);

	/* position to the precise item on the page */
	offnum = _treeb_binsrch(rel, &inskey, buf);
	Assert(!TreebScanPosIsValid(so->currPos));
	so->currPos.buf = buf;

	/*
	 * Now load data from the first page of the scan.
	 *
	 * If inskey.nextkey = false and inskey.backward = false, offnum is
	 * positioned at the first non-pivot tuple >= inskey.scankeys.
	 *
	 * If inskey.nextkey = false and inskey.backward = true, offnum is
	 * positioned at the last non-pivot tuple < inskey.scankeys.
	 *
	 * If inskey.nextkey = true and inskey.backward = false, offnum is
	 * positioned at the first non-pivot tuple > inskey.scankeys.
	 *
	 * If inskey.nextkey = true and inskey.backward = true, offnum is
	 * positioned at the last non-pivot tuple <= inskey.scankeys.
	 *
	 * It's possible that _treeb_binsrch returned an offnum that is out of bounds
	 * for the page.  For example, when inskey is both < the leaf page's high
	 * key and > all of its non-pivot tuples, offnum will be "maxoff + 1".
	 */
	if (!_treeb_readpage(scan, dir, offnum, true))
	{
		/*
		 * There's no actually-matching data on this page.  Try to advance to
		 * the next page.  Return false if there's no matching data at all.
		 */
		_treeb_unlockbuf(scan->indexRelation, so->currPos.buf);
		if (!_treeb_steppage(scan, dir))
			return false;
	}
	else
	{
		/* We have at least one item to return as scan's first item */
		_treeb_drop_lock_and_maybe_pin(scan, &so->currPos);
	}

readcomplete:
	/* OK, itemIndex says what to return */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;
	if (scan->xs_want_itup)
		scan->xs_itup = (IndexTuple) (so->currTuples + currItem->tupleOffset);

	return true;
}

/*
 *	_treeb_next() -- Get the next item in a scan.
 *
 *		On entry, so->currPos describes the current page, which may be pinned
 *		but is not locked, and so->currPos.itemIndex identifies which item was
 *		previously returned.
 *
 *		On successful exit, scan->xs_heaptid is set to the TID of the next
 *		heap tuple, and if requested, scan->xs_itup points to a copy of the
 *		index tuple.  so->currPos is updated as needed.
 *
 *		On failure exit (no more tuples), we release pin and set
 *		so->currPos.buf to InvalidBuffer.
 */
bool
_treeb_next(IndexScanDesc scan, ScanDirection dir)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	TreebScanPosItem *currItem;

	/*
	 * Advance to next tuple on current page; or if there's no more, try to
	 * step to the next page with data.
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (++so->currPos.itemIndex > so->currPos.lastItem)
		{
			if (!_treeb_steppage(scan, dir))
				return false;
		}
	}
	else
	{
		if (--so->currPos.itemIndex < so->currPos.firstItem)
		{
			if (!_treeb_steppage(scan, dir))
				return false;
		}
	}

	/* OK, itemIndex says what to return */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;
	if (scan->xs_want_itup)
		scan->xs_itup = (IndexTuple) (so->currTuples + currItem->tupleOffset);

	return true;
}

/*
 *	_treeb_readpage() -- Load data from current index page into so->currPos
 *
 * Caller must have pinned and read-locked so->currPos.buf; the buffer's state
 * is not changed here.  Also, currPos.moreLeft and moreRight must be valid;
 * they are updated as appropriate.  All other fields of so->currPos are
 * initialized from scratch here.
 *
 * We scan the current page starting at offnum and moving in the indicated
 * direction.  All items matching the scan keys are loaded into currPos.items.
 * moreLeft or moreRight (as appropriate) is cleared if _treeb_checkkeys reports
 * that there can be no more matching tuples in the current scan direction
 * (could just be for the current primitive index scan when scan has arrays).
 *
 * _treeb_first caller passes us an offnum returned by _treeb_binsrch, which might
 * be an out of bounds offnum such as "maxoff + 1" in certain corner cases.
 * _treeb_checkkeys will stop the scan as soon as an equality qual fails (when
 * its scan key was marked required), so _treeb_first _must_ pass us an offnum
 * exactly at the beginning of where equal tuples are to be found.  When we're
 * passed an offnum past the end of the page, we might still manage to stop
 * the scan on this page by calling _treeb_checkkeys against the high key.
 *
 * In the case of a parallel scan, caller must have called _treeb_parallel_seize
 * prior to calling this function; this function will invoke
 * _treeb_parallel_release before returning.
 *
 * Returns true if any matching items found on the page, false if none.
 */
static bool
_treeb_readpage(IndexScanDesc scan, ScanDirection dir, OffsetNumber offnum,
			 bool firstPage)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	Page		page;
	TreebPageOpaque opaque;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	TREEBReadPageState pstate;
	bool		arrayKeys;
	int			itemIndex,
				indnatts;

	/*
	 * We must have the buffer pinned and locked, but the usual macro can't be
	 * used here; this function is what makes it good for currPos.
	 */
	Assert(BufferIsValid(so->currPos.buf));

	page = BufferGetPage(so->currPos.buf);
	opaque = TREEBPageGetOpaque(page);

	/* allow next page be processed by parallel worker */
	if (scan->parallel_scan)
	{
		if (ScanDirectionIsForward(dir))
			pstate.prev_scan_page = opaque->treebpo_next;
		else
			pstate.prev_scan_page = BufferGetBlockNumber(so->currPos.buf);

		_treeb_parallel_release(scan, pstate.prev_scan_page);
	}

	indnatts = IndexRelationGetNumberOfAttributes(scan->indexRelation);
	arrayKeys = so->numArrayKeys != 0;
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	/* initialize page-level state that we'll pass to _treeb_checkkeys */
	pstate.dir = dir;
	pstate.minoff = minoff;
	pstate.maxoff = maxoff;
	pstate.finaltup = NULL;
	pstate.page = page;
	pstate.offnum = InvalidOffsetNumber;
	pstate.skip = InvalidOffsetNumber;
	pstate.continuescan = true; /* default assumption */
	pstate.prechecked = false;
	pstate.firstmatch = false;
	pstate.rechecks = 0;
	pstate.targetdistance = 0;

	/*
	 * We note the buffer's block number so that we can release the pin later.
	 * This allows us to re-read the buffer if it is needed again for hinting.
	 */
	so->currPos.currPage = BufferGetBlockNumber(so->currPos.buf);

	/*
	 * We save the LSN of the page as we read it, so that we know whether it
	 * safe to apply LP_DEAD hints to the page later.  This allows us to drop
	 * the pin for MVCC scans, which allows vacuum to avoid blocking.
	 */
	so->currPos.lsn = BufferGetLSNAtomic(so->currPos.buf);

	/*
	 * we must save the page's right-link while scanning it; this tells us
	 * where to step right to after we're done with these items.  There is no
	 * corresponding need for the left-link, since splits always go right.
	 */
	so->currPos.nextPage = opaque->treebpo_next;

	/* initialize tuple workspace to empty */
	so->currPos.nextTupleOffset = 0;

	/*
	 * Now that the current page has been made consistent, the macro should be
	 * good.
	 */
	Assert(TreebScanPosIsPinned(so->currPos));

	/*
	 * Prechecking the value of the continuescan flag for the last item on the
	 * page (for backwards scan it will be the first item on a page).  If we
	 * observe it to be true, then it should be true for all other items. This
	 * allows us to do significant optimizations in the _treeb_checkkeys()
	 * function for all the items on the page.
	 *
	 * With the forward scan, we do this check for the last item on the page
	 * instead of the high key.  It's relatively likely that the most
	 * significant column in the high key will be different from the
	 * corresponding value from the last item on the page.  So checking with
	 * the last item on the page would give a more precise answer.
	 *
	 * We skip this for the first page read by each (primitive) scan, to avoid
	 * slowing down point queries.  They typically don't stand to gain much
	 * when the optimization can be applied, and are more likely to notice the
	 * overhead of the precheck.
	 *
	 * The optimization is unsafe and must be avoided whenever _treeb_checkkeys
	 * just set a low-order required array's key to the best available match
	 * for a truncated -inf attribute value from the prior page's high key
	 * (array element 0 is always the best available match in this scenario).
	 * It's quite likely that matches for array element 0 begin on this page,
	 * but the start of matches won't necessarily align with page boundaries.
	 * When the start of matches is somewhere in the middle of this page, it
	 * would be wrong to treat page's final non-pivot tuple as representative.
	 * Doing so might lead us to treat some of the page's earlier tuples as
	 * being part of a group of tuples thought to satisfy the required keys.
	 *
	 * Note: Conversely, in the case where the scan's arrays just advanced
	 * using the prior page's HIKEY _without_ advancement setting scanBehind,
	 * the start of matches must be aligned with page boundaries, which makes
	 * it safe to attempt the optimization here now.  It's also safe when the
	 * prior page's HIKEY simply didn't need to advance any required array. In
	 * both cases we can safely assume that the _first_ tuple from this page
	 * must be >= the current set of array keys/equality constraints. And so
	 * if the final tuple is == those same keys (and also satisfies any
	 * required < or <= strategy scan keys) during the precheck, we can safely
	 * assume that this must also be true of all earlier tuples from the page.
	 */
	if (!firstPage && !so->scanBehind && minoff < maxoff)
	{
		ItemId		iid;
		IndexTuple	itup;

		iid = PageGetItemId(page, ScanDirectionIsForward(dir) ? maxoff : minoff);
		itup = (IndexTuple) PageGetItem(page, iid);

		/* Call with arrayKeys=false to avoid undesirable side-effects */
		_treeb_checkkeys(scan, &pstate, false, itup, indnatts);
		pstate.prechecked = pstate.continuescan;
		pstate.continuescan = true; /* reset */
	}

	if (ScanDirectionIsForward(dir))
	{
		/* SK_SEARCHARRAY forward scans must provide high key up front */
		if (arrayKeys && !P_RIGHTMOST(opaque))
		{
			ItemId		iid = PageGetItemId(page, P_HIKEY);

			pstate.finaltup = (IndexTuple) PageGetItem(page, iid);
		}

		/* load items[] in ascending order */
		itemIndex = 0;

		offnum = Max(offnum, minoff);

		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	itup;
			bool		passes_quals;

			/*
			 * If the scan specifies not to return killed tuples, then we
			 * treat a killed tuple as not passing the qual
			 */
			if (scan->ignore_killed_tuples && ItemIdIsDead(iid))
			{
				offnum = OffsetNumberNext(offnum);
				continue;
			}

			itup = (IndexTuple) PageGetItem(page, iid);
			Assert(!TreebTupleIsPivot(itup));

			pstate.offnum = offnum;
			passes_quals = _treeb_checkkeys(scan, &pstate, arrayKeys,
										 itup, indnatts);

			/*
			 * Check if we need to skip ahead to a later tuple (only possible
			 * when the scan uses array keys)
			 */
			if (arrayKeys && OffsetNumberIsValid(pstate.skip))
			{
				Assert(!passes_quals && pstate.continuescan);
				Assert(offnum < pstate.skip);

				offnum = pstate.skip;
				pstate.skip = InvalidOffsetNumber;
				continue;
			}

			if (passes_quals)
			{
				/* tuple passes all scan key conditions */
				pstate.firstmatch = true;
				if (!TreebTupleIsPosting(itup))
				{
					/* Remember it */
					_treeb_saveitem(so, itemIndex, offnum, itup);
					itemIndex++;
				}
				else
				{
					int			tupleOffset;

					/*
					 * Set up state to return posting list, and remember first
					 * TID
					 */
					tupleOffset =
						_treeb_setuppostingitems(so, itemIndex, offnum,
											  TreebTupleGetPostingN(itup, 0),
											  itup);
					itemIndex++;
					/* Remember additional TIDs */
					for (int i = 1; i < TreebTupleGetNPosting(itup); i++)
					{
						_treeb_savepostingitem(so, itemIndex, offnum,
											TreebTupleGetPostingN(itup, i),
											tupleOffset);
						itemIndex++;
					}
				}
			}
			/* When !continuescan, there can't be any more matches, so stop */
			if (!pstate.continuescan)
				break;

			offnum = OffsetNumberNext(offnum);
		}

		/*
		 * We don't need to visit page to the right when the high key
		 * indicates that no more matches will be found there.
		 *
		 * Checking the high key like this works out more often than you might
		 * think.  Leaf page splits pick a split point between the two most
		 * dissimilar tuples (this is weighed against the need to evenly share
		 * free space).  Leaf pages with high key attribute values that can
		 * only appear on non-pivot tuples on the right sibling page are
		 * common.
		 */
		if (pstate.continuescan && !P_RIGHTMOST(opaque))
		{
			ItemId		iid = PageGetItemId(page, P_HIKEY);
			IndexTuple	itup = (IndexTuple) PageGetItem(page, iid);
			int			truncatt;

			truncatt = TreebTupleGetNAtts(itup, scan->indexRelation);
			pstate.prechecked = false;	/* precheck didn't cover HIKEY */
			_treeb_checkkeys(scan, &pstate, arrayKeys, itup, truncatt);
		}

		if (!pstate.continuescan)
			so->currPos.moreRight = false;

		Assert(itemIndex <= MaxTIDsPerTreebPage);
		so->currPos.firstItem = 0;
		so->currPos.lastItem = itemIndex - 1;
		so->currPos.itemIndex = 0;
	}
	else
	{
		/* SK_SEARCHARRAY backward scans must provide final tuple up front */
		if (arrayKeys && minoff <= maxoff && !P_LEFTMOST(opaque))
		{
			ItemId		iid = PageGetItemId(page, minoff);

			pstate.finaltup = (IndexTuple) PageGetItem(page, iid);
		}

		/* load items[] in descending order */
		itemIndex = MaxTIDsPerTreebPage;

		offnum = Min(offnum, maxoff);

		while (offnum >= minoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	itup;
			bool		tuple_alive;
			bool		passes_quals;

			/*
			 * If the scan specifies not to return killed tuples, then we
			 * treat a killed tuple as not passing the qual.  Most of the
			 * time, it's a win to not bother examining the tuple's index
			 * keys, but just skip to the next tuple (previous, actually,
			 * since we're scanning backwards).  However, if this is the first
			 * tuple on the page, we do check the index keys, to prevent
			 * uselessly advancing to the page to the left.  This is similar
			 * to the high key optimization used by forward scans.
			 */
			if (scan->ignore_killed_tuples && ItemIdIsDead(iid))
			{
				Assert(offnum >= P_FIRSTDATAKEY(opaque));
				if (offnum > P_FIRSTDATAKEY(opaque))
				{
					offnum = OffsetNumberPrev(offnum);
					continue;
				}

				tuple_alive = false;
			}
			else
				tuple_alive = true;

			itup = (IndexTuple) PageGetItem(page, iid);
			Assert(!TreebTupleIsPivot(itup));

			pstate.offnum = offnum;
			passes_quals = _treeb_checkkeys(scan, &pstate, arrayKeys,
										 itup, indnatts);

			/*
			 * Check if we need to skip ahead to a later tuple (only possible
			 * when the scan uses array keys)
			 */
			if (arrayKeys && OffsetNumberIsValid(pstate.skip))
			{
				Assert(!passes_quals && pstate.continuescan);
				Assert(offnum > pstate.skip);

				offnum = pstate.skip;
				pstate.skip = InvalidOffsetNumber;
				continue;
			}

			if (passes_quals && tuple_alive)
			{
				/* tuple passes all scan key conditions */
				pstate.firstmatch = true;
				if (!TreebTupleIsPosting(itup))
				{
					/* Remember it */
					itemIndex--;
					_treeb_saveitem(so, itemIndex, offnum, itup);
				}
				else
				{
					int			tupleOffset;

					/*
					 * Set up state to return posting list, and remember first
					 * TID.
					 *
					 * Note that we deliberately save/return items from
					 * posting lists in ascending heap TID order for backwards
					 * scans.  This allows _treeb_killitems() to make a
					 * consistent assumption about the order of items
					 * associated with the same posting list tuple.
					 */
					itemIndex--;
					tupleOffset =
						_treeb_setuppostingitems(so, itemIndex, offnum,
											  TreebTupleGetPostingN(itup, 0),
											  itup);
					/* Remember additional TIDs */
					for (int i = 1; i < TreebTupleGetNPosting(itup); i++)
					{
						itemIndex--;
						_treeb_savepostingitem(so, itemIndex, offnum,
											TreebTupleGetPostingN(itup, i),
											tupleOffset);
					}
				}
			}
			if (!pstate.continuescan)
			{
				/* there can't be any more matches, so stop */
				so->currPos.moreLeft = false;
				break;
			}

			offnum = OffsetNumberPrev(offnum);
		}

		Assert(itemIndex >= 0);
		so->currPos.firstItem = itemIndex;
		so->currPos.lastItem = MaxTIDsPerTreebPage - 1;
		so->currPos.itemIndex = MaxTIDsPerTreebPage - 1;
	}

	return (so->currPos.firstItem <= so->currPos.lastItem);
}

/* Save an index item into so->currPos.items[itemIndex] */
static void
_treeb_saveitem(TreebScanOpaque so, int itemIndex,
			 OffsetNumber offnum, IndexTuple itup)
{
	TreebScanPosItem *currItem = &so->currPos.items[itemIndex];

	Assert(!TreebTupleIsPivot(itup) && !TreebTupleIsPosting(itup));

	currItem->heapTid = itup->t_tid;
	currItem->indexOffset = offnum;
	if (so->currTuples)
	{
		Size		itupsz = IndexTupleSize(itup);

		currItem->tupleOffset = so->currPos.nextTupleOffset;
		memcpy(so->currTuples + so->currPos.nextTupleOffset, itup, itupsz);
		so->currPos.nextTupleOffset += MAXALIGN(itupsz);
	}
}

/*
 * Setup state to save TIDs/items from a single posting list tuple.
 *
 * Saves an index item into so->currPos.items[itemIndex] for TID that is
 * returned to scan first.  Second or subsequent TIDs for posting list should
 * be saved by calling _treeb_savepostingitem().
 *
 * Returns an offset into tuple storage space that main tuple is stored at if
 * needed.
 */
static int
_treeb_setuppostingitems(TreebScanOpaque so, int itemIndex, OffsetNumber offnum,
					  ItemPointer heapTid, IndexTuple itup)
{
	TreebScanPosItem *currItem = &so->currPos.items[itemIndex];

	Assert(TreebTupleIsPosting(itup));

	currItem->heapTid = *heapTid;
	currItem->indexOffset = offnum;
	if (so->currTuples)
	{
		/* Save base IndexTuple (truncate posting list) */
		IndexTuple	base;
		Size		itupsz = TreebTupleGetPostingOffset(itup);

		itupsz = MAXALIGN(itupsz);
		currItem->tupleOffset = so->currPos.nextTupleOffset;
		base = (IndexTuple) (so->currTuples + so->currPos.nextTupleOffset);
		memcpy(base, itup, itupsz);
		/* Defensively reduce work area index tuple header size */
		base->t_info &= ~INDEX_SIZE_MASK;
		base->t_info |= itupsz;
		so->currPos.nextTupleOffset += itupsz;

		return currItem->tupleOffset;
	}

	return 0;
}

/*
 * Save an index item into so->currPos.items[itemIndex] for current posting
 * tuple.
 *
 * Assumes that _treeb_setuppostingitems() has already been called for current
 * posting list tuple.  Caller passes its return value as tupleOffset.
 */
static inline void
_treeb_savepostingitem(TreebScanOpaque so, int itemIndex, OffsetNumber offnum,
					ItemPointer heapTid, int tupleOffset)
{
	TreebScanPosItem *currItem = &so->currPos.items[itemIndex];

	currItem->heapTid = *heapTid;
	currItem->indexOffset = offnum;

	/*
	 * Have index-only scans return the same base IndexTuple for every TID
	 * that originates from the same posting list
	 */
	if (so->currTuples)
		currItem->tupleOffset = tupleOffset;
}

/*
 *	_treeb_steppage() -- Step to next page containing valid data for scan
 *
 * On entry, if so->currPos.buf is valid the buffer is pinned but not locked;
 * if pinned, we'll drop the pin before moving to next page.  The buffer is
 * not locked on entry.
 *
 * For success on a scan using a non-MVCC snapshot we hold a pin, but not a
 * read lock, on that page.  If we do not hold the pin, we set so->currPos.buf
 * to InvalidBuffer.  We return true to indicate success.
 */
static bool
_treeb_steppage(IndexScanDesc scan, ScanDirection dir)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	BlockNumber blkno = InvalidBlockNumber;
	bool		status;

	Assert(TreebScanPosIsValid(so->currPos));

	/* Before leaving current page, deal with any killed items */
	if (so->numKilled > 0)
		_treeb_killitems(scan);

	/*
	 * Before we modify currPos, make a copy of the page data if there was a
	 * mark position that needs it.
	 */
	if (so->markItemIndex >= 0)
	{
		/* bump pin on current buffer for assignment to mark buffer */
		if (TreebScanPosIsPinned(so->currPos))
			IncrBufferRefCount(so->currPos.buf);
		memcpy(&so->markPos, &so->currPos,
			   offsetof(TreebScanPosData, items[1]) +
			   so->currPos.lastItem * sizeof(TreebScanPosItem));
		if (so->markTuples)
			memcpy(so->markTuples, so->currTuples,
				   so->currPos.nextTupleOffset);
		so->markPos.itemIndex = so->markItemIndex;
		so->markItemIndex = -1;

		/*
		 * If we're just about to start the next primitive index scan
		 * (possible with a scan that has arrays keys, and needs to skip to
		 * continue in the current scan direction), moreLeft/moreRight only
		 * indicate the end of the current primitive index scan.  They must
		 * never be taken to indicate that the top-level index scan has ended
		 * (that would be wrong).
		 *
		 * We could handle this case by treating the current array keys as
		 * markPos state.  But depending on the current array state like this
		 * would add complexity.  Instead, we just unset markPos's copy of
		 * moreRight or moreLeft (whichever might be affected), while making
		 * btrestpos reset the scan's arrays to their initial scan positions.
		 * In effect, btrestpos leaves advancing the arrays up to the first
		 * _treeb_readpage call (that takes place after it has restored markPos).
		 */
		Assert(so->markPos.dir == dir);
		if (so->needPrimScan)
		{
			if (ScanDirectionIsForward(dir))
				so->markPos.moreRight = true;
			else
				so->markPos.moreLeft = true;
		}
	}

	if (ScanDirectionIsForward(dir))
	{
		/* Walk right to the next page with data */
		if (scan->parallel_scan != NULL)
		{
			/*
			 * Seize the scan to get the next block number; if the scan has
			 * ended already, bail out.
			 */
			status = _treeb_parallel_seize(scan, &blkno, false);
			if (!status)
			{
				/* release the previous buffer, if pinned */
				TreebScanPosUnpinIfPinned(so->currPos);
				TreebScanPosInvalidate(so->currPos);
				return false;
			}
		}
		else
		{
			/* Not parallel, so use the previously-saved nextPage link. */
			blkno = so->currPos.nextPage;
		}

		/* Remember we left a page with data */
		so->currPos.moreLeft = true;

		/* release the previous buffer, if pinned */
		TreebScanPosUnpinIfPinned(so->currPos);
	}
	else
	{
		/* Remember we left a page with data */
		so->currPos.moreRight = true;

		if (scan->parallel_scan != NULL)
		{
			/*
			 * Seize the scan to get the current block number; if the scan has
			 * ended already, bail out.
			 */
			status = _treeb_parallel_seize(scan, &blkno, false);
			TreebScanPosUnpinIfPinned(so->currPos);
			if (!status)
			{
				TreebScanPosInvalidate(so->currPos);
				return false;
			}
		}
		else
		{
			/* Not parallel, so just use our own notion of the current page */
			blkno = so->currPos.currPage;
		}
	}

	if (!_treeb_readnextpage(scan, blkno, dir))
		return false;

	/* We have at least one item to return as scan's next item */
	_treeb_drop_lock_and_maybe_pin(scan, &so->currPos);

	return true;
}

/*
 *	_treeb_readnextpage() -- Read next page containing valid data for scan
 *
 * On success exit, so->currPos is updated to contain data from the next
 * interesting page, and we return true.  Caller must release the lock (and
 * maybe the pin) on the buffer on success exit.
 *
 * If there are no more matching records in the given direction, we drop all
 * locks and pins, set so->currPos.buf to InvalidBuffer, and return false.
 */
static bool
_treeb_readnextpage(IndexScanDesc scan, BlockNumber blkno, ScanDirection dir)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	Relation	rel;
	Page		page;
	TreebPageOpaque opaque;
	bool		status;

	rel = scan->indexRelation;

	if (ScanDirectionIsForward(dir))
	{
		for (;;)
		{
			/*
			 * if we're at end of scan, give up and mark parallel scan as
			 * done, so that all the workers can finish their scan
			 */
			if (blkno == P_NONE || !so->currPos.moreRight)
			{
				_treeb_parallel_done(scan);
				TreebScanPosInvalidate(so->currPos);
				return false;
			}
			/* check for interrupts while we're not holding any buffer lock */
			CHECK_FOR_INTERRUPTS();
			/* step right one page */
			so->currPos.buf = _treeb_getbuf(rel, blkno, TREEB_READ);
			page = BufferGetPage(so->currPos.buf);
			opaque = TREEBPageGetOpaque(page);
			/* check for deleted page */
			if (!P_IGNORE(opaque))
			{
				PredicateLockPage(rel, blkno, scan->xs_snapshot);
				/* see if there are any matches on this page */
				/* note that this will clear moreRight if we can stop */
				if (_treeb_readpage(scan, dir, P_FIRSTDATAKEY(opaque), false))
					break;
			}
			else if (scan->parallel_scan != NULL)
			{
				/* allow next page be processed by parallel worker */
				_treeb_parallel_release(scan, opaque->treebpo_next);
			}

			/* nope, keep going */
			if (scan->parallel_scan != NULL)
			{
				_treeb_relbuf(rel, so->currPos.buf);
				status = _treeb_parallel_seize(scan, &blkno, false);
				if (!status)
				{
					TreebScanPosInvalidate(so->currPos);
					return false;
				}
			}
			else
			{
				blkno = opaque->treebpo_next;
				_treeb_relbuf(rel, so->currPos.buf);
			}
		}
	}
	else
	{
		/*
		 * Should only happen in parallel cases, when some other backend
		 * advanced the scan.
		 */
		if (so->currPos.currPage != blkno)
		{
			TreebScanPosUnpinIfPinned(so->currPos);
			so->currPos.currPage = blkno;
		}

		/*
		 * Walk left to the next page with data.  This is much more complex
		 * than the walk-right case because of the possibility that the page
		 * to our left splits while we are in flight to it, plus the
		 * possibility that the page we were on gets deleted after we leave
		 * it.  See treeb/README for details.
		 *
		 * It might be possible to rearrange this code to have less overhead
		 * in pinning and locking, but that would require capturing the left
		 * sibling block number when the page is initially read, and then
		 * optimistically starting there (rather than pinning the page twice).
		 * It is not clear that this would be worth the complexity.
		 */
		if (TreebScanPosIsPinned(so->currPos))
			_treeb_lockbuf(rel, so->currPos.buf, TREEB_READ);
		else
			so->currPos.buf = _treeb_getbuf(rel, so->currPos.currPage, TREEB_READ);

		for (;;)
		{
			/* Done if we know there are no matching keys to the left */
			if (!so->currPos.moreLeft)
			{
				_treeb_relbuf(rel, so->currPos.buf);
				_treeb_parallel_done(scan);
				TreebScanPosInvalidate(so->currPos);
				return false;
			}

			/* Step to next physical page */
			so->currPos.buf = _treeb_walk_left(rel, so->currPos.buf);

			/* if we're physically at end of index, return failure */
			if (so->currPos.buf == InvalidBuffer)
			{
				_treeb_parallel_done(scan);
				TreebScanPosInvalidate(so->currPos);
				return false;
			}

			/*
			 * Okay, we managed to move left to a non-deleted page. Done if
			 * it's not half-dead and contains matching tuples. Else loop back
			 * and do it all again.
			 */
			page = BufferGetPage(so->currPos.buf);
			opaque = TREEBPageGetOpaque(page);
			if (!P_IGNORE(opaque))
			{
				PredicateLockPage(rel, BufferGetBlockNumber(so->currPos.buf), scan->xs_snapshot);
				/* see if there are any matches on this page */
				/* note that this will clear moreLeft if we can stop */
				if (_treeb_readpage(scan, dir, PageGetMaxOffsetNumber(page), false))
					break;
			}
			else if (scan->parallel_scan != NULL)
			{
				/* allow next page be processed by parallel worker */
				_treeb_parallel_release(scan, BufferGetBlockNumber(so->currPos.buf));
			}

			/*
			 * For parallel scans, get the last page scanned as it is quite
			 * possible that by the time we try to seize the scan, some other
			 * worker has already advanced the scan to a different page.  We
			 * must continue based on the latest page scanned by any worker.
			 */
			if (scan->parallel_scan != NULL)
			{
				_treeb_relbuf(rel, so->currPos.buf);
				status = _treeb_parallel_seize(scan, &blkno, false);
				if (!status)
				{
					TreebScanPosInvalidate(so->currPos);
					return false;
				}
				so->currPos.buf = _treeb_getbuf(rel, blkno, TREEB_READ);
			}
		}
	}

	return true;
}

/*
 *	_treeb_parallel_readpage() -- Read current page containing valid data for scan
 *
 * On success, release lock and maybe pin on buffer.  We return true to
 * indicate success.
 */
static bool
_treeb_parallel_readpage(IndexScanDesc scan, BlockNumber blkno, ScanDirection dir)
{
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;

	Assert(!so->needPrimScan);

	_treeb_initialize_more_data(so, dir);

	if (!_treeb_readnextpage(scan, blkno, dir))
		return false;

	/* We have at least one item to return as scan's next item */
	_treeb_drop_lock_and_maybe_pin(scan, &so->currPos);

	return true;
}

/*
 * _treeb_walk_left() -- step left one page, if possible
 *
 * The given buffer must be pinned and read-locked.  This will be dropped
 * before stepping left.  On return, we have pin and read lock on the
 * returned page, instead.
 *
 * Returns InvalidBuffer if there is no page to the left (no lock is held
 * in that case).
 *
 * It is possible for the returned leaf page to be half-dead; caller must
 * check that condition and step left again when required.
 */
static Buffer
_treeb_walk_left(Relation rel, Buffer buf)
{
	Page		page;
	TreebPageOpaque opaque;

	page = BufferGetPage(buf);
	opaque = TREEBPageGetOpaque(page);

	for (;;)
	{
		BlockNumber obknum;
		BlockNumber lblkno;
		BlockNumber blkno;
		int			tries;

		/* if we're at end of tree, release buf and return failure */
		if (P_LEFTMOST(opaque))
		{
			_treeb_relbuf(rel, buf);
			break;
		}
		/* remember original page we are stepping left from */
		obknum = BufferGetBlockNumber(buf);
		/* step left */
		blkno = lblkno = opaque->treebpo_prev;
		_treeb_relbuf(rel, buf);
		/* check for interrupts while we're not holding any buffer lock */
		CHECK_FOR_INTERRUPTS();
		buf = _treeb_getbuf(rel, blkno, TREEB_READ);
		page = BufferGetPage(buf);
		opaque = TREEBPageGetOpaque(page);

		/*
		 * If this isn't the page we want, walk right till we find what we
		 * want --- but go no more than four hops (an arbitrary limit). If we
		 * don't find the correct page by then, the most likely bet is that
		 * the original page got deleted and isn't in the sibling chain at all
		 * anymore, not that its left sibling got split more than four times.
		 *
		 * Note that it is correct to test P_ISDELETED not P_IGNORE here,
		 * because half-dead pages are still in the sibling chain.
		 */
		tries = 0;
		for (;;)
		{
			if (!P_ISDELETED(opaque) && opaque->treebpo_next == obknum)
			{
				/* Found desired page, return it */
				return buf;
			}
			if (P_RIGHTMOST(opaque) || ++tries > 4)
				break;
			blkno = opaque->treebpo_next;
			buf = _treeb_relandgetbuf(rel, buf, blkno, TREEB_READ);
			page = BufferGetPage(buf);
			opaque = TREEBPageGetOpaque(page);
		}

		/* Return to the original page to see what's up */
		buf = _treeb_relandgetbuf(rel, buf, obknum, TREEB_READ);
		page = BufferGetPage(buf);
		opaque = TREEBPageGetOpaque(page);
		if (P_ISDELETED(opaque))
		{
			/*
			 * It was deleted.  Move right to first nondeleted page (there
			 * must be one); that is the page that has acquired the deleted
			 * one's keyspace, so stepping left from it will take us where we
			 * want to be.
			 */
			for (;;)
			{
				if (P_RIGHTMOST(opaque))
					elog(ERROR, "fell off the end of index \"%s\"",
						 RelationGetRelationName(rel));
				blkno = opaque->treebpo_next;
				buf = _treeb_relandgetbuf(rel, buf, blkno, TREEB_READ);
				page = BufferGetPage(buf);
				opaque = TREEBPageGetOpaque(page);
				if (!P_ISDELETED(opaque))
					break;
			}

			/*
			 * Now return to top of loop, resetting obknum to point to this
			 * nondeleted page, and try again.
			 */
		}
		else
		{
			/*
			 * It wasn't deleted; the explanation had better be that the page
			 * to the left got split or deleted. Without this check, we'd go
			 * into an infinite loop if there's anything wrong.
			 */
			if (opaque->treebpo_prev == lblkno)
				elog(ERROR, "could not find left sibling of block %u in index \"%s\"",
					 obknum, RelationGetRelationName(rel));
			/* Okay to try again with new lblkno value */
		}
	}

	return InvalidBuffer;
}

/*
 * _treeb_get_endpoint() -- Find the first or last page on a given tree level
 *
 * If the index is empty, we will return InvalidBuffer; any other failure
 * condition causes ereport().  We will not return a dead page.
 *
 * The returned buffer is pinned and read-locked.
 */
Buffer
_treeb_get_endpoint(Relation rel, uint32 level, bool rightmost)
{
	Buffer		buf;
	Page		page;
	TreebPageOpaque opaque;
	OffsetNumber offnum;
	BlockNumber blkno;
	IndexTuple	itup;

	/*
	 * If we are looking for a leaf page, okay to descend from fast root;
	 * otherwise better descend from true root.  (There is no point in being
	 * smarter about intermediate levels.)
	 */
	if (level == 0)
		buf = _treeb_getroot(rel, NULL, TREEB_READ);
	else
		buf = _treeb_gettrueroot(rel);

	if (!BufferIsValid(buf))
		return InvalidBuffer;

	page = BufferGetPage(buf);
	opaque = TREEBPageGetOpaque(page);

	for (;;)
	{
		/*
		 * If we landed on a deleted page, step right to find a live page
		 * (there must be one).  Also, if we want the rightmost page, step
		 * right if needed to get to it (this could happen if the page split
		 * since we obtained a pointer to it).
		 */
		while (P_IGNORE(opaque) ||
			   (rightmost && !P_RIGHTMOST(opaque)))
		{
			blkno = opaque->treebpo_next;
			if (blkno == P_NONE)
				elog(ERROR, "fell off the end of index \"%s\"",
					 RelationGetRelationName(rel));
			buf = _treeb_relandgetbuf(rel, buf, blkno, TREEB_READ);
			page = BufferGetPage(buf);
			opaque = TREEBPageGetOpaque(page);
		}

		/* Done? */
		if (opaque->treebpo_level == level)
			break;
		if (opaque->treebpo_level < level)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("treeb level %u not found in index \"%s\"",
									 level, RelationGetRelationName(rel))));

		/* Descend to leftmost or rightmost child page */
		if (rightmost)
			offnum = PageGetMaxOffsetNumber(page);
		else
			offnum = P_FIRSTDATAKEY(opaque);

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
		blkno = TreebTupleGetDownLink(itup);

		buf = _treeb_relandgetbuf(rel, buf, blkno, TREEB_READ);
		page = BufferGetPage(buf);
		opaque = TREEBPageGetOpaque(page);
	}

	return buf;
}

/*
 *	_treeb_endpoint() -- Find the first or last page in the index, and scan
 * from there to the first key satisfying all the quals.
 *
 * This is used by _treeb_first() to set up a scan when we've determined
 * that the scan must start at the beginning or end of the index (for
 * a forward or backward scan respectively).  Exit conditions are the
 * same as for _treeb_first().
 */
static bool
_treeb_endpoint(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	TreebScanOpaque so = (TreebScanOpaque) scan->opaque;
	Buffer		buf;
	Page		page;
	TreebPageOpaque opaque;
	OffsetNumber start;
	TreebScanPosItem *currItem;

	/*
	 * Scan down to the leftmost or rightmost leaf page.  This is a simplified
	 * version of _treeb_search().  We don't maintain a stack since we know we
	 * won't need it.
	 */
	buf = _treeb_get_endpoint(rel, 0, ScanDirectionIsBackward(dir));

	if (!BufferIsValid(buf))
	{
		/*
		 * Empty index. Lock the whole relation, as nothing finer to lock
		 * exists.
		 */
		PredicateLockRelation(rel, scan->xs_snapshot);
		TreebScanPosInvalidate(so->currPos);
		return false;
	}

	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);
	page = BufferGetPage(buf);
	opaque = TREEBPageGetOpaque(page);
	Assert(P_ISLEAF(opaque));

	if (ScanDirectionIsForward(dir))
	{
		/* There could be dead pages to the left, so not this: */
		/* Assert(P_LEFTMOST(opaque)); */

		start = P_FIRSTDATAKEY(opaque);
	}
	else if (ScanDirectionIsBackward(dir))
	{
		Assert(P_RIGHTMOST(opaque));

		start = PageGetMaxOffsetNumber(page);
	}
	else
	{
		elog(ERROR, "invalid scan direction: %d", (int) dir);
		start = 0;				/* keep compiler quiet */
	}

	/* remember which buffer we have pinned */
	so->currPos.buf = buf;

	_treeb_initialize_more_data(so, dir);

	/*
	 * Now load data from the first page of the scan.
	 */
	if (!_treeb_readpage(scan, dir, start, true))
	{
		/*
		 * There's no actually-matching data on this page.  Try to advance to
		 * the next page.  Return false if there's no matching data at all.
		 */
		_treeb_unlockbuf(scan->indexRelation, so->currPos.buf);
		if (!_treeb_steppage(scan, dir))
			return false;
	}
	else
	{
		/* We have at least one item to return as scan's first item */
		_treeb_drop_lock_and_maybe_pin(scan, &so->currPos);
	}

	/* OK, itemIndex says what to return */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;
	if (scan->xs_want_itup)
		scan->xs_itup = (IndexTuple) (so->currTuples + currItem->tupleOffset);

	return true;
}

/*
 * _treeb_initialize_more_data() -- initialize moreLeft, moreRight and scan dir
 * from currPos
 */
static inline void
_treeb_initialize_more_data(TreebScanOpaque so, ScanDirection dir)
{
	so->currPos.dir = dir;
	if (so->needPrimScan)
	{
		Assert(so->numArrayKeys);

		so->currPos.moreLeft = true;
		so->currPos.moreRight = true;
		so->needPrimScan = false;
	}
	else if (ScanDirectionIsForward(dir))
	{
		so->currPos.moreLeft = false;
		so->currPos.moreRight = true;
	}
	else
	{
		so->currPos.moreLeft = true;
		so->currPos.moreRight = false;
	}
	so->numKilled = 0;			/* just paranoia */
	so->markItemIndex = -1;		/* ditto */
}
