/*-------------------------------------------------------------------------
 *
 * treeb.h
 *	  header file for postgres treeb access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/treeb.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TREEB_H
#define TREEB_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/tableam.h"
#include "access/xlogreader.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/shm_toc.h"
#include "utils/logtape.h"
#include "utils/pg_rusage.h"
#include "utils/tuplesort.h"
#include "optimizer/optimizer.h"

/* There's room for a 16-bit vacuum cycle ID in TreebPageOpaqueData */
typedef uint16 TreebCycleId;

/*
 *	TreebPageOpaqueData -- At the end of every page, we store a pointer
 *	to both siblings in the tree.  This is used to do forward/backward
 *	index scans.  The next-page link is also critical for recovery when
 *	a search has navigated to the wrong page due to concurrent page splits
 *	or deletions; see src/backend/access/treeb/README for more info.
 *
 *	In addition, we store the page's treeb level (counting upwards from
 *	zero at a leaf page) as well as some flag bits indicating the page type
 *	and status.  If the page is deleted, a TreebDeletedPageData struct is stored
 *	in the page's tuple area, while a standard TreebPageOpaqueData struct is
 *	stored in the page special area.
 *
 *	We also store a "vacuum cycle ID".  When a page is split while VACUUM is
 *	processing the index, a nonzero value associated with the VACUUM run is
 *	stored into both halves of the split page.  (If VACUUM is not running,
 *	both pages receive zero cycleids.)	This allows VACUUM to detect whether
 *	a page was split since it started, with a small probability of false match
 *	if the page was last split some exact multiple of MAX_TREEB_CYCLE_ID VACUUMs
 *	ago.  Also, during a split, the TREEBP_SPLIT_END flag is cleared in the left
 *	(original) page, and set in the right page, but only if the next page
 *	to its right has a different cycleid.
 *
 *	NOTE: the TREEBP_LEAF flag bit is redundant since level==0 could be tested
 *	instead.
 *
 *	NOTE: the treebpo_level field used to be a union type in order to allow
 *	deleted pages to store a 32-bit safexid in the same field.  We now store
 *	64-bit/full safexid values using TreebDeletedPageData instead.
 */

typedef struct TreebPageOpaqueData
{
	BlockNumber treebpo_prev;		/* left sibling, or P_NONE if leftmost */
	BlockNumber treebpo_next;		/* right sibling, or P_NONE if rightmost */
	uint32		treebpo_level;		/* tree level --- zero for leaf pages */
	uint16		treebpo_flags;		/* flag bits, see below */
	TreebCycleId	treebpo_cycleid;	/* vacuum cycle ID of latest split */
} TreebPageOpaqueData;

typedef TreebPageOpaqueData *TreebPageOpaque;

#define TREEBPageGetOpaque(page) ((TreebPageOpaque) PageGetSpecialPointer(page))

/* Bits defined in treebpo_flags */
#define TREEBP_LEAF		(1 << 0)	/* leaf page, i.e. not internal page */
#define TREEBP_ROOT		(1 << 1)	/* root page (has no parent) */
#define TREEBP_DELETED		(1 << 2)	/* page has been deleted from tree */
#define TREEBP_META		(1 << 3)	/* meta-page */
#define TREEBP_HALF_DEAD	(1 << 4)	/* empty, but still in tree */
#define TREEBP_SPLIT_END	(1 << 5)	/* rightmost page of split group */
#define TREEBP_HAS_GARBAGE (1 << 6)	/* page has LP_DEAD tuples (deprecated) */
#define TREEBP_INCOMPLETE_SPLIT (1 << 7)	/* right sibling's downlink is missing */
#define TREEBP_HAS_FULLXID	(1 << 8)	/* contains TreebDeletedPageData */

/*
 * The max allowed value of a cycle ID is a bit less than 64K.  This is
 * for convenience of pg_filedump and similar utilities: we want to use
 * the last 2 bytes of special space as an index type indicator, and
 * restricting cycle ID lets treeb use that space for vacuum cycle IDs
 * while still allowing index type to be identified.
 */
#define MAX_TREEB_CYCLE_ID		0xFF7F


/*
 * The Meta page is always the first page in the treeb index.
 * Its primary purpose is to point to the location of the treeb root page.
 * We also point to the "fast" root, which is the current effective root;
 * see README for discussion.
 */

typedef struct TreebMetaPageData
{
	uint32		treebm_magic;		/* should contain TREEB_MAGIC */
	uint32		treebm_version;	/* treeb version (always <= TREEB_VERSION) */
	BlockNumber treebm_root;		/* current root location */
	uint32		treebm_level;		/* tree level of the root page */
	BlockNumber treebm_fastroot;	/* current "fast" root location */
	uint32		treebm_fastlevel;	/* tree level of the "fast" root page */
	/* remaining fields only valid when treebm_version >= TREEB_NOVAC_VERSION */

	/* number of deleted, non-recyclable pages during last cleanup */
	uint32		treebm_last_cleanup_num_delpages;
	/* number of heap tuples during last cleanup (deprecated) */
	float8		treebm_last_cleanup_num_heap_tuples;

	bool		treebm_allequalimage;	/* are all columns "equalimage"? */
} TreebMetaPageData;

#define TreebPageGetMeta(p) \
	((TreebMetaPageData *) PageGetContents(p))

/*
 * The current Treeb version is 4.  That's what you'll get when you create
 * a new index.
 *
 * Treeb version 3 was used in PostgreSQL v11.  It is mostly the same as
 * version 4, but heap TIDs were not part of the keyspace.  Index tuples
 * with duplicate keys could be stored in any order.  We continue to
 * support reading and writing Treeb versions 2 and 3, so that they don't
 * need to be immediately re-indexed at pg_upgrade.  In order to get the
 * new heapkeyspace semantics, however, a REINDEX is needed.
 *
 * Deduplication is safe to use when the treebm_allequalimage field is set to
 * true.  It's safe to read the treebm_allequalimage field on version 3, but
 * only version 4 indexes make use of deduplication.  Even version 4
 * indexes created on PostgreSQL v12 will need a REINDEX to make use of
 * deduplication, though, since there is no other way to set
 * treebm_allequalimage to true (pg_upgrade hasn't been taught to set the
 * metapage field).
 *
 * Treeb version 2 is mostly the same as version 3.  There are two new
 * fields in the metapage that were introduced in version 3.  A version 2
 * metapage will be automatically upgraded to version 3 on the first
 * insert to it.  INCLUDE indexes cannot use version 2.
 */
#define TREEB_METAPAGE	0		/* first page is meta */
#define TREEB_MAGIC		0x053162	/* magic number in metapage */
#define TREEB_VERSION	4		/* current version number */
#define TREEB_MIN_VERSION	2	/* minimum supported version */
#define TREEB_NOVAC_VERSION	3	/* version with all meta fields set */

/*
 * Maximum size of a treeb index entry, including its tuple header.
 *
 * We actually need to be able to fit three items on every page,
 * so restrict any one item to 1/3 the per-page available space.
 *
 * There are rare cases where _treeb_truncate() will need to enlarge
 * a heap index tuple to make space for a tiebreaker heap TID
 * attribute, which we account for here.
 */
#define TreebMaxItemSize(page) \
	(MAXALIGN_DOWN((PageGetPageSize(page) - \
					MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
					MAXALIGN(sizeof(TreebPageOpaqueData))) / 3) - \
					MAXALIGN(sizeof(ItemPointerData)))
#define TreebMaxItemSizeNoHeapTid(page) \
	MAXALIGN_DOWN((PageGetPageSize(page) - \
				   MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
				   MAXALIGN(sizeof(TreebPageOpaqueData))) / 3)

/*
 * MaxTIDsPerTreebPage is an upper bound on the number of heap TIDs tuples
 * that may be stored on a treeb leaf page.  It is used to size the
 * per-page temporary buffers.
 *
 * Note: we don't bother considering per-tuple overheads here to keep
 * things simple (value is based on how many elements a single array of
 * heap TIDs must have to fill the space between the page header and
 * special area).  The value is slightly higher (i.e. more conservative)
 * than necessary as a result, which is considered acceptable.
 */
#define MaxTIDsPerTreebPage \
	(int) ((BLCKSZ - SizeOfPageHeaderData - sizeof(TreebPageOpaqueData)) / \
		   sizeof(ItemPointerData))

/*
 * The leaf-page fillfactor defaults to 90% but is user-adjustable.
 * For pages above the leaf level, we use a fixed 70% fillfactor.
 * The fillfactor is applied during index build and when splitting
 * a rightmost page; when splitting non-rightmost pages we try to
 * divide the data equally.  When splitting a page that's entirely
 * filled with a single value (duplicates), the effective leaf-page
 * fillfactor is 96%, regardless of whether the page is a rightmost
 * page.
 */
#define TREEB_MIN_FILLFACTOR		10
#define TREEB_DEFAULT_FILLFACTOR	90
#define TREEB_NONLEAF_FILLFACTOR	70
#define TREEB_SINGLEVAL_FILLFACTOR	96

/*
 *	In general, the treeb code tries to localize its knowledge about
 *	page layout to a couple of routines.  However, we need a special
 *	value to indicate "no page number" in those places where we expect
 *	page numbers.  We can use zero for this because we never need to
 *	make a pointer to the metadata page.
 */

#define P_NONE			0

/*
 * Macros to test whether a page is leftmost or rightmost on its tree level,
 * as well as other state info kept in the opaque data.
 */
#define P_LEFTMOST(opaque)		((opaque)->treebpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)		((opaque)->treebpo_next == P_NONE)
#define P_ISLEAF(opaque)		(((opaque)->treebpo_flags & TREEBP_LEAF) != 0)
#define P_ISROOT(opaque)		(((opaque)->treebpo_flags & TREEBP_ROOT) != 0)
#define P_ISDELETED(opaque)		(((opaque)->treebpo_flags & TREEBP_DELETED) != 0)
#define P_ISMETA(opaque)		(((opaque)->treebpo_flags & TREEBP_META) != 0)
#define P_ISHALFDEAD(opaque)	(((opaque)->treebpo_flags & TREEBP_HALF_DEAD) != 0)
#define P_IGNORE(opaque)		(((opaque)->treebpo_flags & (TREEBP_DELETED|TREEBP_HALF_DEAD)) != 0)
#define P_HAS_GARBAGE(opaque)	(((opaque)->treebpo_flags & TREEBP_HAS_GARBAGE) != 0)
#define P_INCOMPLETE_SPLIT(opaque)	(((opaque)->treebpo_flags & TREEBP_INCOMPLETE_SPLIT) != 0)
#define P_HAS_FULLXID(opaque)	(((opaque)->treebpo_flags & TREEBP_HAS_FULLXID) != 0)

/*
 * TreebDeletedPageData is the page contents of a deleted page
 */
typedef struct TreebDeletedPageData
{
	FullTransactionId safexid;	/* See TreebPageIsRecyclable() */
} TreebDeletedPageData;

static inline void
TreebPageSetDeleted(Page page, FullTransactionId safexid)
{
	TreebPageOpaque opaque;
	PageHeader	header;
	TreebDeletedPageData *contents;

	opaque = TREEBPageGetOpaque(page);
	header = ((PageHeader) page);

	opaque->treebpo_flags &= ~TREEBP_HALF_DEAD;
	opaque->treebpo_flags |= TREEBP_DELETED | TREEBP_HAS_FULLXID;
	header->pd_lower = MAXALIGN(SizeOfPageHeaderData) +
		sizeof(TreebDeletedPageData);
	header->pd_upper = header->pd_special;

	/* Set safexid in deleted page */
	contents = ((TreebDeletedPageData *) PageGetContents(page));
	contents->safexid = safexid;
}

static inline FullTransactionId
TreebPageGetDeleteXid(Page page)
{
	TreebPageOpaque opaque;
	TreebDeletedPageData *contents;

	/* We only expect to be called with a deleted page */
	Assert(!PageIsNew(page));
	opaque = TREEBPageGetOpaque(page);
	Assert(P_ISDELETED(opaque));

	/* pg_upgrade'd deleted page -- must be safe to recycle now */
	if (!P_HAS_FULLXID(opaque))
		return FirstNormalFullTransactionId;

	/* Get safexid from deleted page */
	contents = ((TreebDeletedPageData *) PageGetContents(page));
	return contents->safexid;
}

/*
 * Is an existing page recyclable?
 *
 * This exists to centralize the policy on which deleted pages are now safe to
 * re-use.  However, _treeb_pendingfsm_finalize() duplicates some of the same
 * logic because it doesn't work directly with pages -- keep the two in sync.
 *
 * Note: PageIsNew() pages are always safe to recycle, but we can't deal with
 * them here (caller is responsible for that case themselves).  Caller might
 * well need special handling for new pages anyway.
 */
static inline bool
TreebPageIsRecyclable(Page page, Relation heaprel)
{
	TreebPageOpaque opaque;

	Assert(!PageIsNew(page));
	Assert(heaprel != NULL);

	/* Recycling okay iff page is deleted and safexid is old enough */
	opaque = TREEBPageGetOpaque(page);
	if (P_ISDELETED(opaque))
	{
		FullTransactionId safexid = TreebPageGetDeleteXid(page);

		/*
		 * The page was deleted, but when? If it was just deleted, a scan
		 * might have seen the downlink to it, and will read the page later.
		 * As long as that can happen, we must keep the deleted page around as
		 * a tombstone.
		 *
		 * For that check if the deletion XID could still be visible to
		 * anyone. If not, then no scan that's still in progress could have
		 * seen its downlink, and we can recycle it.
		 */
		return GlobalVisCheckRemovableFullXid(heaprel, safexid);
	}

	return false;
}

/*
 * TreebVacState and TreebPendingFSM are private treeb.c state used during VACUUM.
 * They are exported for use by page deletion related code in treebpage.c.
 */
typedef struct TreebPendingFSM
{
	BlockNumber target;			/* Page deleted by current VACUUM */
	FullTransactionId safexid;	/* Page's TreebDeletedPageData.safexid */
} TreebPendingFSM;

typedef struct TreebVacState
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	TreebCycleId	cycleid;
	MemoryContext pagedelcontext;

	/*
	 * _treeb_pendingfsm_finalize() state
	 */
	int			bufsize;		/* pendingpages space (in # elements) */
	int			maxbufsize;		/* max bufsize that respects work_mem */
	TreebPendingFSM *pendingpages; /* One entry per newly deleted page */
	int			npendingpages;	/* current # valid pendingpages */
} TreebVacState;

/*
 *	Lehman and Yao's algorithm requires a ``high key'' on every non-rightmost
 *	page.  The high key is not a tuple that is used to visit the heap.  It is
 *	a pivot tuple (see "Notes on B-Tree tuple format" below for definition).
 *	The high key on a page is required to be greater than or equal to any
 *	other key that appears on the page.  If we find ourselves trying to
 *	insert a key that is strictly > high key, we know we need to move right
 *	(this should only happen if the page was split since we examined the
 *	parent page).
 *
 *	Our insertion algorithm guarantees that we can use the initial least key
 *	on our right sibling as the high key.  Once a page is created, its high
 *	key changes only if the page is split.
 *
 *	On a non-rightmost page, the high key lives in item 1 and data items
 *	start in item 2.  Rightmost pages have no high key, so we store data
 *	items beginning in item 1.
 */

#define P_HIKEY				((OffsetNumber) 1)
#define P_FIRSTKEY			((OffsetNumber) 2)
#define P_FIRSTDATAKEY(opaque)	(P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY)

/*
 * Notes on B-Tree tuple format, and key and non-key attributes:
 *
 * INCLUDE B-Tree indexes have non-key attributes.  These are extra
 * attributes that may be returned by index-only scans, but do not influence
 * the order of items in the index (formally, non-key attributes are not
 * considered to be part of the key space).  Non-key attributes are only
 * present in leaf index tuples whose item pointers actually point to heap
 * tuples (non-pivot tuples).  _treeb_check_natts() enforces the rules
 * described here.
 *
 * Non-pivot tuple format (plain/non-posting variant):
 *
 *  t_tid | t_info | key values | INCLUDE columns, if any
 *
 * t_tid points to the heap TID, which is a tiebreaker key column as of
 * TREEB_VERSION 4.
 *
 * Non-pivot tuples complement pivot tuples, which only have key columns.
 * The sole purpose of pivot tuples is to represent how the key space is
 * separated.  In general, any B-Tree index that has more than one level
 * (i.e. any index that does not just consist of a metapage and a single
 * leaf root page) must have some number of pivot tuples, since pivot
 * tuples are used for traversing the tree.  Suffix truncation can omit
 * trailing key columns when a new pivot is formed, which makes minus
 * infinity their logical value.  Since TREEB_VERSION 4 indexes treat heap
 * TID as a trailing key column that ensures that all index tuples are
 * physically unique, it is necessary to represent heap TID as a trailing
 * key column in pivot tuples, though very often this can be truncated
 * away, just like any other key column. (Actually, the heap TID is
 * omitted rather than truncated, since its representation is different to
 * the non-pivot representation.)
 *
 * Pivot tuple format:
 *
 *  t_tid | t_info | key values | [heap TID]
 *
 * We store the number of columns present inside pivot tuples by abusing
 * their t_tid offset field, since pivot tuples never need to store a real
 * offset (pivot tuples generally store a downlink in t_tid, though).  The
 * offset field only stores the number of columns/attributes when the
 * INDEX_ALT_TID_MASK bit is set, which doesn't count the trailing heap
 * TID column sometimes stored in pivot tuples -- that's represented by
 * the presence of TREEB_PIVOT_HEAP_TID_ATTR.  The INDEX_ALT_TID_MASK bit in
 * t_info is always set on TREEB_VERSION 4 pivot tuples, since
 * TreebTupleIsPivot() must work reliably on heapkeyspace versions.
 *
 * In version 2 or version 3 (!heapkeyspace) indexes, INDEX_ALT_TID_MASK
 * might not be set in pivot tuples.  TreebTupleIsPivot() won't work
 * reliably as a result.  The number of columns stored is implicitly the
 * same as the number of columns in the index, just like any non-pivot
 * tuple. (The number of columns stored should not vary, since suffix
 * truncation of key columns is unsafe within any !heapkeyspace index.)
 *
 * The 12 least significant bits from t_tid's offset number are used to
 * represent the number of key columns within a pivot tuple.  This leaves 4
 * status bits (TREEB_STATUS_OFFSET_MASK bits), which are shared by all tuples
 * that have the INDEX_ALT_TID_MASK bit set (set in t_info) to store basic
 * tuple metadata.  TreebTupleIsPivot() and TreebTupleIsPosting() use the
 * TREEB_STATUS_OFFSET_MASK bits.
 *
 * Sometimes non-pivot tuples also use a representation that repurposes
 * t_tid to store metadata rather than a TID.  PostgreSQL v13 introduced a
 * new non-pivot tuple format to support deduplication: posting list
 * tuples.  Deduplication merges together multiple equal non-pivot tuples
 * into a logically equivalent, space efficient representation.  A posting
 * list is an array of ItemPointerData elements.  Non-pivot tuples are
 * merged together to form posting list tuples lazily, at the point where
 * we'd otherwise have to split a leaf page.
 *
 * Posting tuple format (alternative non-pivot tuple representation):
 *
 *  t_tid | t_info | key values | posting list (TID array)
 *
 * Posting list tuples are recognized as such by having the
 * INDEX_ALT_TID_MASK status bit set in t_info and the TREEB_IS_POSTING status
 * bit set in t_tid's offset number.  These flags redefine the content of
 * the posting tuple's t_tid to store the location of the posting list
 * (instead of a block number), as well as the total number of heap TIDs
 * present in the tuple (instead of a real offset number).
 *
 * The 12 least significant bits from t_tid's offset number are used to
 * represent the number of heap TIDs present in the tuple, leaving 4 status
 * bits (the TREEB_STATUS_OFFSET_MASK bits).  Like any non-pivot tuple, the
 * number of columns stored is always implicitly the total number in the
 * index (in practice there can never be non-key columns stored, since
 * deduplication is not supported with INCLUDE indexes).
 */
#define INDEX_ALT_TID_MASK			INDEX_AM_RESERVED_BIT

/* Item pointer offset bit masks */
#define TREEB_OFFSET_MASK				0x0FFF
#define TREEB_STATUS_OFFSET_MASK		0xF000
/* TREEB_STATUS_OFFSET_MASK status bits */
#define TREEB_PIVOT_HEAP_TID_ATTR		0x1000
#define TREEB_IS_POSTING				0x2000

/*
 * Mask allocated for number of keys in index tuple must be able to fit
 * maximum possible number of index attributes
 */
StaticAssertDecl(TREEB_OFFSET_MASK >= INDEX_MAX_KEYS,
				 "TREEB_OFFSET_MASK can't fit INDEX_MAX_KEYS");

/*
 * Note: TreebTupleIsPivot() can have false negatives (but not false
 * positives) when used with !heapkeyspace indexes
 */
static inline bool
TreebTupleIsPivot(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* absence of TREEB_IS_POSTING in offset number indicates pivot tuple */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & TREEB_IS_POSTING) != 0)
		return false;

	return true;
}

static inline bool
TreebTupleIsPosting(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* presence of TREEB_IS_POSTING in offset number indicates posting tuple */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & TREEB_IS_POSTING) == 0)
		return false;

	return true;
}

static inline void
TreebTupleSetPosting(IndexTuple itup, uint16 nhtids, int postingoffset)
{
	Assert(nhtids > 1);
	Assert((nhtids & TREEB_STATUS_OFFSET_MASK) == 0);
	Assert((size_t) postingoffset == MAXALIGN(postingoffset));
	Assert(postingoffset < INDEX_SIZE_MASK);
	Assert(!TreebTupleIsPivot(itup));

	itup->t_info |= INDEX_ALT_TID_MASK;
	ItemPointerSetOffsetNumber(&itup->t_tid, (nhtids | TREEB_IS_POSTING));
	ItemPointerSetBlockNumber(&itup->t_tid, postingoffset);
}

static inline uint16
TreebTupleGetNPosting(IndexTuple posting)
{
	OffsetNumber existing;

	Assert(TreebTupleIsPosting(posting));

	existing = ItemPointerGetOffsetNumberNoCheck(&posting->t_tid);
	return (existing & TREEB_OFFSET_MASK);
}

static inline uint32
TreebTupleGetPostingOffset(IndexTuple posting)
{
	Assert(TreebTupleIsPosting(posting));

	return ItemPointerGetBlockNumberNoCheck(&posting->t_tid);
}

static inline ItemPointer
TreebTupleGetPosting(IndexTuple posting)
{
	return (ItemPointer) ((char *) posting +
						  TreebTupleGetPostingOffset(posting));
}

static inline ItemPointer
TreebTupleGetPostingN(IndexTuple posting, int n)
{
	return TreebTupleGetPosting(posting) + n;
}

/*
 * Get/set downlink block number in pivot tuple.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */
static inline BlockNumber
TreebTupleGetDownLink(IndexTuple pivot)
{
	return ItemPointerGetBlockNumberNoCheck(&pivot->t_tid);
}

static inline void
TreebTupleSetDownLink(IndexTuple pivot, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&pivot->t_tid, blkno);
}

/*
 * Get number of attributes within tuple.
 *
 * Note that this does not include an implicit tiebreaker heap TID
 * attribute, if any.  Note also that the number of key attributes must be
 * explicitly represented in all heapkeyspace pivot tuples.
 *
 * Note: This is defined as a macro rather than an inline function to
 * avoid including rel.h.
 */
#define TreebTupleGetNAtts(itup, rel)	\
	( \
		(TreebTupleIsPivot(itup)) ? \
		( \
			ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) & TREEB_OFFSET_MASK \
		) \
		: \
		IndexRelationGetNumberOfAttributes(rel) \
	)

/*
 * Set number of key attributes in tuple.
 *
 * The heap TID tiebreaker attribute bit may also be set here, indicating that
 * a heap TID value will be stored at the end of the tuple (i.e. using the
 * special pivot tuple representation).
 */
static inline void
TreebTupleSetNAtts(IndexTuple itup, uint16 nkeyatts, bool heaptid)
{
	Assert(nkeyatts <= INDEX_MAX_KEYS);
	Assert((nkeyatts & TREEB_STATUS_OFFSET_MASK) == 0);
	Assert(!heaptid || nkeyatts > 0);
	Assert(!TreebTupleIsPivot(itup) || nkeyatts == 0);

	itup->t_info |= INDEX_ALT_TID_MASK;

	if (heaptid)
		nkeyatts |= TREEB_PIVOT_HEAP_TID_ATTR;

	/* TREEB_IS_POSTING bit is deliberately unset here */
	ItemPointerSetOffsetNumber(&itup->t_tid, nkeyatts);
	Assert(TreebTupleIsPivot(itup));
}

/*
 * Get/set leaf page's "top parent" link from its high key.  Used during page
 * deletion.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */
static inline BlockNumber
TreebTupleGetTopParent(IndexTuple leafhikey)
{
	return ItemPointerGetBlockNumberNoCheck(&leafhikey->t_tid);
}

static inline void
TreebTupleSetTopParent(IndexTuple leafhikey, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&leafhikey->t_tid, blkno);
	TreebTupleSetNAtts(leafhikey, 0, false);
}

/*
 * Get tiebreaker heap TID attribute, if any.
 *
 * This returns the first/lowest heap TID in the case of a posting list tuple.
 */
static inline ItemPointer
TreebTupleGetHeapTID(IndexTuple itup)
{
	if (TreebTupleIsPivot(itup))
	{
		/* Pivot tuple heap TID representation? */
		if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) &
			 TREEB_PIVOT_HEAP_TID_ATTR) != 0)
			return (ItemPointer) ((char *) itup + IndexTupleSize(itup) -
								  sizeof(ItemPointerData));

		/* Heap TID attribute was truncated */
		return NULL;
	}
	else if (TreebTupleIsPosting(itup))
		return TreebTupleGetPosting(itup);

	return &itup->t_tid;
}

/*
 * Get maximum heap TID attribute, which could be the only TID in the case of
 * a non-pivot tuple that does not have a posting list.
 *
 * Works with non-pivot tuples only.
 */
static inline ItemPointer
TreebTupleGetMaxHeapTID(IndexTuple itup)
{
	Assert(!TreebTupleIsPivot(itup));

	if (TreebTupleIsPosting(itup))
	{
		uint16		nposting = TreebTupleGetNPosting(itup);

		return TreebTupleGetPostingN(itup, nposting - 1);
	}

	return &itup->t_tid;
}

#define TreebLessStrategyNumber 1
#define TreebLessEqualStrategyNumber 2
#define TreebEqualStrategyNumber 3
#define TreebGreaterEqualStrategyNumber 4
#define TreebGreaterStrategyNumber 5
#define TreebMaxStrategyNumber 5

static inline StrategyNumber
TreebCommuteStrategyNumber(StrategyNumber strat)
{
	switch (strat)
	{
		case TreebLessStrategyNumber:
			 return TreebGreaterStrategyNumber;
		case TreebLessEqualStrategyNumber:
			 return TreebGreaterEqualStrategyNumber;
		case TreebEqualStrategyNumber:
			 return TreebEqualStrategyNumber;
		case TreebGreaterEqualStrategyNumber:
			 return TreebLessEqualStrategyNumber;
		case TreebGreaterStrategyNumber:
			 return TreebLessStrategyNumber;
	}
	elog(ERROR, "Unexpected treeb strategy number: %d", strat);
}

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure (TREEBORDER_PROC) for determining
 *	whether, for two keys a and b, a < b, a = b, or a > b.  This routine
 *	must return < 0, 0, > 0, respectively, in these three cases.
 *
 *	To facilitate accelerated sorting, an operator class may choose to
 *	offer a second procedure (TREEBSORTSUPPORT_PROC).  For full details, see
 *	src/include/utils/sortsupport.h.
 *
 *	To support window frames defined by "RANGE offset PRECEDING/FOLLOWING",
 *	an operator class may choose to offer a third amproc procedure
 *	(TREEBINRANGE_PROC), independently of whether it offers sortsupport.
 *	For full details, see doc/src/sgml/treeb.sgml.
 *
 *	To facilitate B-Tree deduplication, an operator class may choose to
 *	offer a forth amproc procedure (TreebEQUALIMAGE_PROC).  For full details,
 *	see doc/src/sgml/treeb.sgml.
 */

#define TREEBORDER_PROC		1
#define TREEBSORTSUPPORT_PROC	2
#define TREEBINRANGE_PROC		3
#define TreebEQUALIMAGE_PROC	4
#define TREEBOPTIONS_PROC		5
#define TreebNProcs			5

/*
 *	We need to be able to tell the difference between read and write
 *	requests for pages, in order to do locking correctly.
 */

#define TREEB_READ			BUFFER_LOCK_SHARE
#define TREEB_WRITE		BUFFER_LOCK_EXCLUSIVE

/*
 * TreebStackData -- As we descend a tree, we push the location of pivot
 * tuples whose downlink we are about to follow onto a private stack.  If
 * we split a leaf, we use this stack to walk back up the tree and insert
 * data into its parent page at the correct location.  We also have to
 * recursively insert into the grandparent page if and when the parent page
 * splits.  Our private stack can become stale due to concurrent page
 * splits and page deletions, but it should never give us an irredeemably
 * bad picture.
 */
typedef struct TreebStackData
{
	BlockNumber treebs_blkno;
	OffsetNumber treebs_offset;
	struct TreebStackData *treebs_parent;
} TreebStackData;

typedef TreebStackData *TreebStack;

/*
 * TreebScanInsertData is the treeb-private state needed to find an initial
 * position for an indexscan, or to insert new tuples -- an "insertion
 * scankey" (not to be confused with a search scankey).  It's used to descend
 * a B-Tree using _treeb_search.
 *
 * heapkeyspace indicates if we expect all keys in the index to be physically
 * unique because heap TID is used as a tiebreaker attribute, and if index may
 * have truncated key attributes in pivot tuples.  This is actually a property
 * of the index relation itself (not an indexscan).  heapkeyspace indexes are
 * indexes whose version is >= version 4.  It's convenient to keep this close
 * by, rather than accessing the metapage repeatedly.
 *
 * allequalimage is set to indicate that deduplication is safe for the index.
 * This is also a property of the index relation rather than an indexscan.
 *
 * anynullkeys indicates if any of the keys had NULL value when scankey was
 * built from index tuple (note that already-truncated tuple key attributes
 * set NULL as a placeholder key value, which also affects value of
 * anynullkeys).  This is a convenience for unique index non-pivot tuple
 * insertion, which usually temporarily unsets scantid, but shouldn't iff
 * anynullkeys is true.  Value generally matches non-pivot tuple's HasNulls
 * bit, but may not when inserting into an INCLUDE index (tuple header value
 * is affected by the NULL-ness of both key and non-key attributes).
 *
 * See comments in _treeb_first for an explanation of the nextkey and backward
 * fields.
 *
 * scantid is the heap TID that is used as a final tiebreaker attribute.  It
 * is set to NULL when index scan doesn't need to find a position for a
 * specific physical tuple.  Must be set when inserting new tuples into
 * heapkeyspace indexes, since every tuple in the tree unambiguously belongs
 * in one exact position (it's never set with !heapkeyspace indexes, though).
 * Despite the representational difference, treeb search code considers
 * scantid to be just another insertion scankey attribute.
 *
 * scankeys is an array of scan key entries for attributes that are compared
 * before scantid (user-visible attributes).  keysz is the size of the array.
 * During insertion, there must be a scan key for every attribute, but when
 * starting a regular index scan some can be omitted.  The array is used as a
 * flexible array member, though it's sized in a way that makes it possible to
 * use stack allocations.  See treeb/README for full details.
 */
typedef struct TreebScanInsertData
{
	bool		heapkeyspace;
	bool		allequalimage;
	bool		anynullkeys;
	bool		nextkey;
	bool		backward;		/* backward index scan? */
	ItemPointer scantid;		/* tiebreaker for scankeys */
	int			keysz;			/* Size of scankeys array */
	ScanKeyData scankeys[INDEX_MAX_KEYS];	/* Must appear last */
} TreebScanInsertData;

typedef TreebScanInsertData *TreebScanInsert;

/*
 * TreebInsertStateData is a working area used during insertion.
 *
 * This is filled in after descending the tree to the first leaf page the new
 * tuple might belong on.  Tracks the current position while performing
 * uniqueness check, before we have determined which exact page to insert
 * to.
 *
 * (This should be private to treebinsert.c, but it's also used by
 * _treeb_binsrch_insert)
 */
typedef struct TreebInsertStateData
{
	IndexTuple	itup;			/* Item we're inserting */
	Size		itemsz;			/* Size of itup -- should be MAXALIGN()'d */
	TreebScanInsert itup_key;		/* Insertion scankey */

	/* Buffer containing leaf page we're likely to insert itup on */
	Buffer		buf;

	/*
	 * Cache of bounds within the current buffer.  Only used for insertions
	 * where _treeb_check_unique is called.  See _treeb_binsrch_insert and
	 * _treeb_findinsertloc for details.
	 */
	bool		bounds_valid;
	OffsetNumber low;
	OffsetNumber stricthigh;

	/*
	 * if _treeb_binsrch_insert found the location inside existing posting list,
	 * save the position inside the list.  -1 sentinel value indicates overlap
	 * with an existing posting list tuple that has its LP_DEAD bit set.
	 */
	int			postingoff;
} TreebInsertStateData;

typedef TreebInsertStateData *TreebInsertState;

/*
 * State used to representing an individual pending tuple during
 * deduplication.
 */
typedef struct TreebDedupInterval
{
	OffsetNumber baseoff;
	uint16		nitems;
} TreebDedupInterval;

/*
 * TreebDedupStateData is a working area used during deduplication.
 *
 * The status info fields track the state of a whole-page deduplication pass.
 * State about the current pending posting list is also tracked.
 *
 * A pending posting list is comprised of a contiguous group of equal items
 * from the page, starting from page offset number 'baseoff'.  This is the
 * offset number of the "base" tuple for new posting list.  'nitems' is the
 * current total number of existing items from the page that will be merged to
 * make a new posting list tuple, including the base tuple item.  (Existing
 * items may themselves be posting list tuples, or regular non-pivot tuples.)
 *
 * The total size of the existing tuples to be freed when pending posting list
 * is processed gets tracked by 'phystupsize'.  This information allows
 * deduplication to calculate the space saving for each new posting list
 * tuple, and for the entire pass over the page as a whole.
 */
typedef struct TreebDedupStateData
{
	/* Deduplication status info for entire pass over page */
	bool		deduplicate;	/* Still deduplicating page? */
	int			nmaxitems;		/* Number of max-sized tuples so far */
	Size		maxpostingsize; /* Limit on size of final tuple */

	/* Metadata about base tuple of current pending posting list */
	IndexTuple	base;			/* Use to form new posting list */
	OffsetNumber baseoff;		/* page offset of base */
	Size		basetupsize;	/* base size without original posting list */

	/* Other metadata about pending posting list */
	ItemPointer htids;			/* Heap TIDs in pending posting list */
	int			nhtids;			/* Number of heap TIDs in htids array */
	int			nitems;			/* Number of existing tuples/line pointers */
	Size		phystupsize;	/* Includes line pointer overhead */

	/*
	 * Array of tuples to go on new version of the page.  Contains one entry
	 * for each group of consecutive items.  Note that existing tuples that
	 * will not become posting list tuples do not appear in the array (they
	 * are implicitly unchanged by deduplication pass).
	 */
	int			nintervals;		/* current number of intervals in array */
	TreebDedupInterval intervals[MaxIndexTuplesPerPage];
} TreebDedupStateData;

typedef TreebDedupStateData *TreebDedupState;

/*
 * TreebVacuumPostingData is state that represents how to VACUUM (or delete) a
 * posting list tuple when some (though not all) of its TIDs are to be
 * deleted.
 *
 * Convention is that itup field is the original posting list tuple on input,
 * and palloc()'d final tuple used to overwrite existing tuple on output.
 */
typedef struct TreebVacuumPostingData
{
	/* Tuple that will be/was updated */
	IndexTuple	itup;
	OffsetNumber updatedoffset;

	/* State needed to describe final itup in WAL */
	uint16		ndeletedtids;
	uint16		deletetids[FLEXIBLE_ARRAY_MEMBER];
} TreebVacuumPostingData;

typedef TreebVacuumPostingData *TreebVacuumPosting;

/*
 * TreebScanOpaqueData is the treeb-private state needed for an indexscan.
 * This consists of preprocessed scan keys (see _treeb_preprocess_keys() for
 * details of the preprocessing), information about the current location
 * of the scan, and information about the marked location, if any.  (We use
 * TreebScanPosData to represent the data needed for each of current and marked
 * locations.)	In addition we can remember some known-killed index entries
 * that must be marked before we can move off the current page.
 *
 * Index scans work a page at a time: we pin and read-lock the page, identify
 * all the matching items on the page and save them in TreebScanPosData, then
 * release the read-lock while returning the items to the caller for
 * processing.  This approach minimizes lock/unlock traffic.  Note that we
 * keep the pin on the index page until the caller is done with all the items
 * (this is needed for VACUUM synchronization, see treeb/README).  When we
 * are ready to step to the next page, if the caller has told us any of the
 * items were killed, we re-lock the page to mark them killed, then unlock.
 * Finally we drop the pin and step to the next page in the appropriate
 * direction.
 *
 * If we are doing an index-only scan, we save the entire IndexTuple for each
 * matched item, otherwise only its heap TID and offset.  The IndexTuples go
 * into a separate workspace array; each TreebScanPosItem stores its tuple's
 * offset within that array.  Posting list tuples store a "base" tuple once,
 * allowing the same key to be returned for each TID in the posting list
 * tuple.
 */

typedef struct TreebScanPosItem	/* what we remember about each match */
{
	ItemPointerData heapTid;	/* TID of referenced heap item */
	OffsetNumber indexOffset;	/* index item's location within page */
	LocationIndex tupleOffset;	/* IndexTuple's offset in workspace, if any */
} TreebScanPosItem;

typedef struct TreebScanPosData
{
	Buffer		buf;			/* if valid, the buffer is pinned */

	XLogRecPtr	lsn;			/* pos in the WAL stream when page was read */
	BlockNumber currPage;		/* page referenced by items array */
	BlockNumber nextPage;		/* page's right link when we scanned it */

	/*
	 * moreLeft and moreRight track whether we think there may be matching
	 * index entries to the left and right of the current page, respectively.
	 * We can clear the appropriate one of these flags when _treeb_checkkeys()
	 * sets TREEBReadPageState.continuescan = false.
	 */
	bool		moreLeft;
	bool		moreRight;

	/*
	 * Direction of the scan at the time that _treeb_readpage was called.
	 *
	 * Used by treebrestrpos to "restore" the scan's array keys by resetting each
	 * array to its first element's value (first in this scan direction). This
	 * avoids the need to directly track the array keys in treebmarkpos.
	 */
	ScanDirection dir;

	/*
	 * If we are doing an index-only scan, nextTupleOffset is the first free
	 * location in the associated tuple storage workspace.
	 */
	int			nextTupleOffset;

	/*
	 * The items array is always ordered in index order (ie, increasing
	 * indexoffset).  When scanning backwards it is convenient to fill the
	 * array back-to-front, so we start at the last slot and fill downwards.
	 * Hence we need both a first-valid-entry and a last-valid-entry counter.
	 * itemIndex is a cursor showing which entry was last returned to caller.
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */
	int			itemIndex;		/* current index in items[] */

	TreebScanPosItem items[MaxTIDsPerTreebPage];	/* MUST BE LAST */
} TreebScanPosData;

typedef TreebScanPosData *TreebScanPos;

#define TreebScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)
#define TreebScanPosUnpin(scanpos) \
	do { \
		ReleaseBuffer((scanpos).buf); \
		(scanpos).buf = InvalidBuffer; \
	} while (0)
#define TreebScanPosUnpinIfPinned(scanpos) \
	do { \
		if (TreebScanPosIsPinned(scanpos)) \
			TreebScanPosUnpin(scanpos); \
	} while (0)

#define TreebScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)
#define TreebScanPosInvalidate(scanpos) \
	do { \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).lsn = InvalidXLogRecPtr; \
		(scanpos).nextTupleOffset = 0; \
	} while (0)

/* We need one of these for each equality-type SK_SEARCHARRAY scan key */
typedef struct TreebArrayKeyInfo
{
	int			scan_key;		/* index of associated key in keyData */
	int			cur_elem;		/* index of current element in elem_values */
	int			num_elems;		/* number of elems in current array value */
	Datum	   *elem_values;	/* array of num_elems Datums */
} TreebArrayKeyInfo;

typedef struct TreebScanOpaqueData
{
	/* these fields are set by _treeb_preprocess_keys(): */
	bool		qual_ok;		/* false if qual can never be satisfied */
	int			numberOfKeys;	/* number of preprocessed scan keys */
	ScanKey		keyData;		/* array of preprocessed scan keys */

	/* workspace for SK_SEARCHARRAY support */
	int			numArrayKeys;	/* number of equality-type array keys */
	bool		needPrimScan;	/* New prim scan to continue in current dir? */
	bool		scanBehind;		/* Last array advancement matched -inf attr? */
	TreebArrayKeyInfo *arrayKeys;	/* info about each equality-type array key */
	FmgrInfo   *orderProcs;		/* ORDER procs for required equality keys */
	MemoryContext arrayContext; /* scan-lifespan context for array data */

	/* info about killed items if any (killedItems is NULL if never used) */
	int		   *killedItems;	/* currPos.items indexes of killed items */
	int			numKilled;		/* number of currently stored items */

	/*
	 * If we are doing an index-only scan, these are the tuple storage
	 * workspaces for the currPos and markPos respectively.  Each is of size
	 * BLCKSZ, so it can hold as much as a full page's worth of tuples.
	 */
	char	   *currTuples;		/* tuple storage for currPos */
	char	   *markTuples;		/* tuple storage for markPos */

	/*
	 * If the marked position is on the same page as current position, we
	 * don't use markPos, but just keep the marked itemIndex in markItemIndex
	 * (all the rest of currPos is valid for the mark position). Hence, to
	 * determine if there is a mark, first look at markItemIndex, then at
	 * markPos.
	 */
	int			markItemIndex;	/* itemIndex, or -1 if not valid */

	/* keep these last in struct for efficiency */
	TreebScanPosData currPos;		/* current position data */
	TreebScanPosData markPos;		/* marked position, if any */
} TreebScanOpaqueData;

typedef TreebScanOpaqueData *TreebScanOpaque;

/*
 * _treeb_readpage state used across _treeb_checkkeys calls for a page
 */
typedef struct TREEBReadPageState
{
	/* Input parameters, set by _treeb_readpage for _treeb_checkkeys */
	ScanDirection dir;			/* current scan direction */
	OffsetNumber minoff;		/* Lowest non-pivot tuple's offset */
	OffsetNumber maxoff;		/* Highest non-pivot tuple's offset */
	IndexTuple	finaltup;		/* Needed by scans with array keys */
	BlockNumber prev_scan_page; /* previous _treeb_parallel_release block */
	Page		page;			/* Page being read */

	/* Per-tuple input parameters, set by _treeb_readpage for _treeb_checkkeys */
	OffsetNumber offnum;		/* current tuple's page offset number */

	/* Output parameter, set by _treeb_checkkeys for _treeb_readpage */
	OffsetNumber skip;			/* Array keys "look ahead" skip offnum */
	bool		continuescan;	/* Terminate ongoing (primitive) index scan? */

	/*
	 * Input and output parameters, set and unset by both _treeb_readpage and
	 * _treeb_checkkeys to manage precheck optimizations
	 */
	bool		prechecked;		/* precheck set continuescan to 'true'? */
	bool		firstmatch;		/* at least one match so far?  */

	/*
	 * Private _treeb_checkkeys state used to manage "look ahead" optimization
	 * (only used during scans with array keys)
	 */
	int16		rechecks;
	int16		targetdistance;

} TREEBReadPageState;

/*
 * We use some private sk_flags bits in preprocessed scan keys.  We're allowed
 * to use bits 16-31 (see skey.h).  The uppermost bits are copied from the
 * index's indoption[] array entry for the index attribute.
 */
#define SK_TREEB_REQFWD	0x00010000	/* required to continue forward scan */
#define SK_TREEB_REQBKWD	0x00020000	/* required to continue backward scan */
#define SK_TREEB_INDOPTION_SHIFT  24	/* must clear the above bits */
#define SK_TREEB_DESC			(INDOPTION_DESC << SK_TREEB_INDOPTION_SHIFT)
#define SK_TREEB_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_TREEB_INDOPTION_SHIFT)

typedef struct TreebOptions
{
	int32		varlena_header_;	/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
	float8		vacuum_cleanup_index_scale_factor;	/* deprecated */
	bool		deduplicate_items;	/* Try to deduplicate items? */
} TreebOptions;

#define TreebGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GetTreebOid()), \
	 (relation)->rd_options ? \
	 ((TreebOptions *) (relation)->rd_options)->fillfactor : \
	 TREEB_DEFAULT_FILLFACTOR)
#define TreebGetTargetPageFreeSpace(relation) \
	(BLCKSZ * (100 - TreebGetFillFactor(relation)) / 100)
#define TreebGetDeduplicateItems(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GetTreebOid()), \
	((relation)->rd_options ? \
	 ((TreebOptions *) (relation)->rd_options)->deduplicate_items : true))

/*
 * Constant definition for progress reporting.  Phase numbers must match
 * treebbuildphasename.
 */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 (see progress.h) */
#define PROGRESS_TREEB_PHASE_INDEXBUILD_TABLESCAN		2
#define PROGRESS_TREEB_PHASE_PERFORMSORT_1				3
#define PROGRESS_TREEB_PHASE_PERFORMSORT_2				4
#define PROGRESS_TREEB_PHASE_LEAF_LOAD					5

/*
 * external entry points for treeb, in treeb.c
 */
extern void treebbuildempty(Relation index);
extern bool treebinsert(Relation rel, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 bool indexUnchanged,
					 struct IndexInfo *indexInfo);
extern IndexScanDesc treebbeginscan(Relation rel, int nkeys, int norderbys);
extern Size treebestimateparallelscan(int nkeys, int norderbys);
extern void treebinitparallelscan(void *target);
extern bool treebgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 treebgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void treebrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);
extern void treebparallelrescan(IndexScanDesc scan);
extern void treebendscan(IndexScanDesc scan);
extern void treebmarkpos(IndexScanDesc scan);
extern void treebrestrpos(IndexScanDesc scan);
extern IndexBulkDeleteResult *treebbulkdelete(IndexVacuumInfo *info,
										   IndexBulkDeleteResult *stats,
										   IndexBulkDeleteCallback callback,
										   void *callback_state);
extern IndexBulkDeleteResult *treebvacuumcleanup(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats);
extern bool treebcanreturn(Relation index, int attno);

/*
 * prototypes for internal functions in treeb.c
 */
extern bool _treeb_parallel_seize(IndexScanDesc scan, BlockNumber *pageno,
							   bool first);
extern void _treeb_parallel_release(IndexScanDesc scan, BlockNumber scan_page);
extern void _treeb_parallel_done(IndexScanDesc scan);
extern void _treeb_parallel_primscan_schedule(IndexScanDesc scan,
										   BlockNumber prev_scan_page);

/*
 * prototypes for functions in treebdedup.c
 */
extern void _treeb_dedup_pass(Relation rel, Buffer buf, IndexTuple newitem,
						   Size newitemsz, bool bottomupdedup);
extern bool _treeb_bottomupdel_pass(Relation rel, Buffer buf, Relation heapRel,
								 Size newitemsz);
extern void _treeb_dedup_start_pending(TreebDedupState state, IndexTuple base,
									OffsetNumber baseoff);
extern bool _treeb_dedup_save_htid(TreebDedupState state, IndexTuple itup);
extern Size _treeb_dedup_finish_pending(Page newpage, TreebDedupState state);
extern IndexTuple _treeb_form_posting(IndexTuple base, ItemPointer htids,
								   int nhtids);
extern void _treeb_update_posting(TreebVacuumPosting vacposting);
extern IndexTuple _treeb_swap_posting(IndexTuple newitem, IndexTuple oposting,
								   int postingoff);

/*
 * prototypes for functions in treebinsert.c
 */
extern bool _treeb_doinsert(Relation rel, IndexTuple itup,
						 IndexUniqueCheck checkUnique, bool indexUnchanged,
						 Relation heapRel);
extern void _treeb_finish_split(Relation rel, Relation heaprel, Buffer lbuf,
							 TreebStack stack);
extern Buffer _treeb_getstackbuf(Relation rel, Relation heaprel, TreebStack stack,
							  BlockNumber child);

/*
 * prototypes for functions in treebsplitloc.c
 */
extern OffsetNumber _treeb_findsplitloc(Relation rel, Page origpage,
									 OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
									 bool *newitemonleft);

/*
 * prototypes for functions in treebpage.c
 */
extern int	treebgetrootheight(Relation rel);
extern void _treeb_initmetapage(Page page, BlockNumber rootbknum, uint32 level,
							 bool allequalimage);
extern bool _treeb_vacuum_needs_cleanup(Relation rel);
extern void _treeb_set_cleanup_info(Relation rel, BlockNumber num_delpages);
extern void _treeb_upgrademetapage(Page page);
extern Buffer _treeb_getroot(Relation rel, Relation heaprel, int access);
extern Buffer _treeb_gettrueroot(Relation rel);
extern void _treeb_metaversion(Relation rel, bool *heapkeyspace,
							bool *allequalimage);
extern void _treeb_checkpage(Relation rel, Buffer buf);
extern Buffer _treeb_getbuf(Relation rel, BlockNumber blkno, int access);
extern Buffer _treeb_allocbuf(Relation rel, Relation heaprel);
extern Buffer _treeb_relandgetbuf(Relation rel, Buffer obuf,
							   BlockNumber blkno, int access);
extern void _treeb_relbuf(Relation rel, Buffer buf);
extern void _treeb_lockbuf(Relation rel, Buffer buf, int access);
extern void _treeb_unlockbuf(Relation rel, Buffer buf);
extern bool _treeb_conditionallockbuf(Relation rel, Buffer buf);
extern void _treeb_upgradelockbufcleanup(Relation rel, Buffer buf);
extern void _treeb_pageinit(Page page, Size size);
extern void _treeb_delitems_vacuum(Relation rel, Buffer buf,
								OffsetNumber *deletable, int ndeletable,
								TreebVacuumPosting *updatable, int nupdatable);
extern void _treeb_delitems_delete_check(Relation rel, Buffer buf,
									  Relation heapRel,
									  TM_IndexDeleteOp *delstate);
extern void _treeb_pagedel(Relation rel, Buffer leafbuf, TreebVacState *vstate);
extern void _treeb_pendingfsm_init(Relation rel, TreebVacState *vstate,
								bool cleanuponly);
extern void _treeb_pendingfsm_finalize(Relation rel, TreebVacState *vstate);

/*
 * prototypes for functions in treebsearch.c
 */
extern TreebStack _treeb_search(Relation rel, Relation heaprel, TreebScanInsert key,
						  Buffer *bufP, int access);
extern Buffer _treeb_moveright(Relation rel, Relation heaprel, TreebScanInsert key,
							Buffer buf, bool forupdate, TreebStack stack,
							int access);
extern OffsetNumber _treeb_binsrch_insert(Relation rel, TreebInsertState insertstate);
extern int32 _treeb_compare(Relation rel, TreebScanInsert key, Page page, OffsetNumber offnum);
extern bool _treeb_first(IndexScanDesc scan, ScanDirection dir);
extern bool _treeb_next(IndexScanDesc scan, ScanDirection dir);
extern Buffer _treeb_get_endpoint(Relation rel, uint32 level, bool rightmost);

/*
 * prototypes for functions in treebutils.c
 */
extern TreebScanInsert _treeb_mkscankey(Relation rel, IndexTuple itup);
extern void _treeb_freestack(TreebStack stack);
extern bool _treeb_start_prim_scan(IndexScanDesc scan, ScanDirection dir);
extern void _treeb_start_array_keys(IndexScanDesc scan, ScanDirection dir);
extern void _treeb_preprocess_keys(IndexScanDesc scan);
extern bool _treeb_checkkeys(IndexScanDesc scan, TREEBReadPageState *pstate, bool arrayKeys,
						  IndexTuple tuple, int tupnatts);
extern void _treeb_killitems(IndexScanDesc scan);
extern TreebCycleId _treeb_vacuum_cycleid(Relation rel);
extern TreebCycleId _treeb_start_vacuum(Relation rel);
extern void _treeb_end_vacuum(Relation rel);
extern void _treeb_end_vacuum_callback(int code, Datum arg);
extern Size TreebShmemSize(void);
extern void TreebShmemInit(void);
extern bytea *treeboptions(Datum reloptions, bool validate);
extern bool treebproperty(Oid index_oid, int attno,
					   IndexAMProperty prop, const char *propname,
					   bool *res, bool *isnull);
extern char *treebbuildphasename(int64 phasenum);
extern IndexTuple _treeb_truncate(Relation rel, IndexTuple lastleft,
							   IndexTuple firstright, TreebScanInsert itup_key);
extern int	_treeb_keep_natts_fast(Relation rel, IndexTuple lastleft,
								IndexTuple firstright);
extern bool _treeb_check_natts(Relation rel, bool heapkeyspace, Page page,
							OffsetNumber offnum);
extern void _treeb_check_third_page(Relation rel, Relation heap,
								 bool needheaptidspace, Page page, IndexTuple newtup);
extern bool _treeb_allequalimage(Relation rel, bool debugmessage);

/*
 * prototypes for functions in treebvalidate.c
 */
extern bool treebvalidate(Oid opclassoid);
extern void treebadjustmembers(Oid opfamilyoid,
							Oid opclassoid,
							List *operators,
							List *functions);

/*
 * prototypes for functions in treebsort.c
 */
extern IndexBuildResult *treebbuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);
extern void _treeb_parallel_build_main(dsm_segment *seg, shm_toc *toc);
extern Tuplesortstate *tuplesort_begin_cluster_treeb(TupleDesc tupDesc,
													 Relation indexRel,
													 int workMem,
													 SortCoordinate coordinate,
													 int sortopt);


#define TreebVacuumLock BtreeVacuumLock


#define PROGRESS_VACUUM_TREEB_BLKS_SCANNED PROGRESS_VACUUM_HEAP_BLKS_SCANNED


#define PROGRESS_VACUUM_TREEB_BLKS_VACUUMED PROGRESS_VACUUM_HEAP_BLKS_VACUUMED


#define PROGRESS_VACUUM_PHASE_SCAN_TREEB PROGRESS_VACUUM_PHASE_SCAN_HEAP


#define PROGRESS_VACUUM_PHASE_VACUUM_TREEB PROGRESS_VACUUM_PHASE_VACUUM_HEAP


#define PROGRESS_VACUUM_TOTAL_TREEB_BLKS PROGRESS_VACUUM_TOTAL_HEAP_BLKS


#define RELOPT_KIND_TREEB RELOPT_KIND_BTREE


#define WAIT_EVENT_TREEB_PAGE WAIT_EVENT_BTREE_PAGE




#define USEMEM(state,amt)	((state)->availMem -= (amt))



#define LACKMEM(state)		((state)->availMem < 0 && !(state)->slabAllocatorUsed)



#define WORKER(state)		((state)->shared && (state)->worker != -1)



#define FREEMEM(state,amt)	((state)->availMem += (amt))



/* When using this macro, beware of double evaluation of len */
#define LogicalTapeReadExact(tape, ptr, len) \
	do { \
		if (LogicalTapeRead(tape, ptr, len) != (size_t) (len)) \
			elog(ERROR, "unexpected end of data"); \
	} while(0)



#define DEFAULT_PAGE_CPU_MULTIPLIER 50.0



#define SLAB_SLOT_SIZE 1024



typedef union SlabSlot
{
	union SlabSlot *nextfree;
	char		buffer[SLAB_SLOT_SIZE];
} SlabSlot;



typedef enum
{
	TSS_INITIAL,				/* Loading tuples; still within memory limit */
	TSS_BOUNDED,				/* Loading tuples into bounded-size heap */
	TSS_BUILDRUNS,				/* Loading tuples; writing to tape */
	TSS_SORTEDINMEM,			/* Sort completed entirely in memory */
	TSS_SORTEDONTAPE,			/* Sort completed, final run is on tape */
	TSS_FINALMERGE,				/* Performing final merge on-the-fly */
} TupSortStatus;



struct Tuplesortstate
{
	TuplesortPublic base;
	TupSortStatus status;		/* enumerated value as shown above */
	bool		bounded;		/* did caller specify a maximum number of
								 * tuples to return? */
	bool		boundUsed;		/* true if we made use of a bounded heap */
	int			bound;			/* if bounded, the maximum number of tuples */
	int64		tupleMem;		/* memory consumed by individual tuples.
								 * storing this separately from what we track
								 * in availMem allows us to subtract the
								 * memory consumed by all tuples when dumping
								 * tuples to tape */
	int64		availMem;		/* remaining memory available, in bytes */
	int64		allowedMem;		/* total memory allowed, in bytes */
	int			maxTapes;		/* max number of input tapes to merge in each
								 * pass */
	int64		maxSpace;		/* maximum amount of space occupied among sort
								 * of groups, either in-memory or on-disk */
	bool		isMaxSpaceDisk; /* true when maxSpace is value for on-disk
								 * space, false when it's value for in-memory
								 * space */
	TupSortStatus maxSpaceStatus;	/* sort status when maxSpace was reached */
	LogicalTapeSet *tapeset;	/* logtape.c object for tapes in a temp file */

	/*
	 * This array holds the tuples now in sort memory.  If we are in state
	 * INITIAL, the tuples are in no particular order; if we are in state
	 * SORTEDINMEM, the tuples are in final sorted order; in states BUILDRUNS
	 * and FINALMERGE, the tuples are organized in "heap" order per Algorithm
	 * H.  In state SORTEDONTAPE, the array is not used.
	 */
	SortTuple  *memtuples;		/* array of SortTuple structs */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */
	bool		growmemtuples;	/* memtuples' growth still underway? */

	/*
	 * Memory for tuples is sometimes allocated using a simple slab allocator,
	 * rather than with palloc().  Currently, we switch to slab allocation
	 * when we start merging.  Merging only needs to keep a small, fixed
	 * number of tuples in memory at any time, so we can avoid the
	 * palloc/pfree overhead by recycling a fixed number of fixed-size slots
	 * to hold the tuples.
	 *
	 * For the slab, we use one large allocation, divided into SLAB_SLOT_SIZE
	 * slots.  The allocation is sized to have one slot per tape, plus one
	 * additional slot.  We need that many slots to hold all the tuples kept
	 * in the heap during merge, plus the one we have last returned from the
	 * sort, with tuplesort_gettuple.
	 *
	 * Initially, all the slots are kept in a linked list of free slots.  When
	 * a tuple is read from a tape, it is put to the next available slot, if
	 * it fits.  If the tuple is larger than SLAB_SLOT_SIZE, it is palloc'd
	 * instead.
	 *
	 * When we're done processing a tuple, we return the slot back to the free
	 * list, or pfree() if it was palloc'd.  We know that a tuple was
	 * allocated from the slab, if its pointer value is between
	 * slabMemoryBegin and -End.
	 *
	 * When the slab allocator is used, the USEMEM/LACKMEM mechanism of
	 * tracking memory usage is not used.
	 */
	bool		slabAllocatorUsed;

	char	   *slabMemoryBegin;	/* beginning of slab memory arena */
	char	   *slabMemoryEnd;	/* end of slab memory arena */
	SlabSlot   *slabFreeHead;	/* head of free list */

	/* Memory used for input and output tape buffers. */
	size_t		tape_buffer_mem;

	/*
	 * When we return a tuple to the caller in tuplesort_gettuple_XXX, that
	 * came from a tape (that is, in TSS_SORTEDONTAPE or TSS_FINALMERGE
	 * modes), we remember the tuple in 'lastReturnedTuple', so that we can
	 * recycle the memory on next gettuple call.
	 */
	void	   *lastReturnedTuple;

	/*
	 * While building initial runs, this is the current output run number.
	 * Afterwards, it is the number of initial runs we made.
	 */
	int			currentRun;

	/*
	 * Logical tapes, for merging.
	 *
	 * The initial runs are written in the output tapes.  In each merge pass,
	 * the output tapes of the previous pass become the input tapes, and new
	 * output tapes are created as needed.  When nInputTapes equals
	 * nInputRuns, there is only one merge pass left.
	 */
	LogicalTape **inputTapes;
	int			nInputTapes;
	int			nInputRuns;

	LogicalTape **outputTapes;
	int			nOutputTapes;
	int			nOutputRuns;

	LogicalTape *destTape;		/* current output tape */

	/*
	 * These variables are used after completion of sorting to keep track of
	 * the next tuple to return.  (In the tape case, the tape's current read
	 * position is also critical state.)
	 */
	LogicalTape *result_tape;	/* actual tape of finished output */
	int			current;		/* array index (only used if SORTEDINMEM) */
	bool		eof_reached;	/* reached EOF (needed for cursors) */

	/* markpos_xxx holds marked position for mark and restore */
	int64		markpos_block;	/* tape block# (only used if SORTEDONTAPE) */
	int			markpos_offset; /* saved "current", or offset in tape block */
	bool		markpos_eof;	/* saved "eof_reached" */

	/*
	 * These variables are used during parallel sorting.
	 *
	 * worker is our worker identifier.  Follows the general convention that
	 * -1 value relates to a leader tuplesort, and values >= 0 worker
	 * tuplesorts. (-1 can also be a serial tuplesort.)
	 *
	 * shared is mutable shared memory state, which is used to coordinate
	 * parallel sorts.
	 *
	 * nParticipants is the number of worker Tuplesortstates known by the
	 * leader to have actually been launched, which implies that they must
	 * finish a run that the leader needs to merge.  Typically includes a
	 * worker state held by the leader process itself.  Set in the leader
	 * Tuplesortstate only.
	 */
	int			worker;
	Sharedsort *shared;
	int			nParticipants;

	/*
	 * Additional state for managing "abbreviated key" sortsupport routines
	 * (which currently may be used by all cases except the hash index case).
	 * Tracks the intervals at which the optimization's effectiveness is
	 * tested.
	 */
	int64		abbrevNext;		/* Tuple # at which to next check
								 * applicability */

	/*
	 * Resource snapshot for time of sort start.
	 */
#ifdef TRACE_SORT
	PGRUsage	ru_start;
#endif
};



struct Sharedsort
{
	/* mutex protects all fields prior to tapes */
	slock_t		mutex;

	/*
	 * currentWorker generates ordinal identifier numbers for parallel sort
	 * workers.  These start from 0, and are always gapless.
	 *
	 * Workers increment workersFinished to indicate having finished.  If this
	 * is equal to state.nParticipants within the leader, leader is ready to
	 * merge worker runs.
	 */
	int			currentWorker;
	int			workersFinished;

	/* Temporary file space */
	SharedFileSet fileset;

	/* Size of tapes flexible array */
	int			nTapes;

	/*
	 * Tapes array used by workers to report back information needed by the
	 * leader to concatenate all worker tapes into one for merging
	 */
	TapeShare	tapes[FLEXIBLE_ARRAY_MEMBER];
};



typedef struct
{
	Relation	heapRel;		/* table the index is being built on */
	Relation	indexRel;		/* index being built */
} TuplesortIndexArg;



typedef struct
{
	TuplesortIndexArg index;

	bool		enforceUnique;	/* complain if we find duplicate tuples */
	bool		uniqueNullsNotDistinct; /* unique constraint null treatment */
} TuplesortIndexTreebArg;



extern void treebcostestimate(struct PlannerInfo *root,
						   struct IndexPath *path,
						   double loop_count,
						   Cost *indexStartupCost,
						   Cost *indexTotalCost,
						   Selectivity *indexSelectivity,
						   double *indexCorrelation,
						   double *indexPages);



extern void PrepareSortSupportFromTreebRel(Relation indexRel, int16 strategy,
										   SortSupport ssup);



extern Tuplesortstate *tuplesort_begin_index_treeb(Relation heapRel,
												   Relation indexRel,
												   bool enforceUnique,
												   bool uniqueNullsNotDistinct,
												   int workMem, SortCoordinate coordinate,
												   int sortopt);

#endif							/* TREEB_H */
