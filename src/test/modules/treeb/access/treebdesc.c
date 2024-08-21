/*-------------------------------------------------------------------------
 *
 * treebdesc.c
 *	  rmgr descriptor routines for access/treeb/treebxlog.c
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/treebdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/treebxlog.h"
#include "access/rmgrdesc_utils.h"

static void delvacuum_desc(StringInfo buf, char *block_data,
						   uint16 ndeleted, uint16 nupdated);

void
treeb_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_TREEB_INSERT_LEAF:
		case XLOG_TREEB_INSERT_UPPER:
		case XLOG_TREEB_INSERT_META:
		case XLOG_TREEB_INSERT_POST:
			{
				xl_treeb_insert *xlrec = (xl_treeb_insert *) rec;

				appendStringInfo(buf, "off: %u", xlrec->offnum);
				break;
			}
		case XLOG_TREEB_SPLIT_L:
		case XLOG_TREEB_SPLIT_R:
			{
				xl_treeb_split *xlrec = (xl_treeb_split *) rec;

				appendStringInfo(buf, "level: %u, firstrightoff: %d, newitemoff: %d, postingoff: %d",
								 xlrec->level, xlrec->firstrightoff,
								 xlrec->newitemoff, xlrec->postingoff);
				break;
			}
		case XLOG_TREEB_DEDUP:
			{
				xl_treeb_dedup *xlrec = (xl_treeb_dedup *) rec;

				appendStringInfo(buf, "nintervals: %u", xlrec->nintervals);
				break;
			}
		case XLOG_TREEB_VACUUM:
			{
				xl_treeb_vacuum *xlrec = (xl_treeb_vacuum *) rec;

				appendStringInfo(buf, "ndeleted: %u, nupdated: %u",
								 xlrec->ndeleted, xlrec->nupdated);

				if (XLogRecHasBlockData(record, 0))
					delvacuum_desc(buf, XLogRecGetBlockData(record, 0, NULL),
								   xlrec->ndeleted, xlrec->nupdated);
				break;
			}
		case XLOG_TREEB_DELETE:
			{
				xl_treeb_delete *xlrec = (xl_treeb_delete *) rec;

				appendStringInfo(buf, "snapshotConflictHorizon: %u, ndeleted: %u, nupdated: %u, isCatalogRel: %c",
								 xlrec->snapshotConflictHorizon,
								 xlrec->ndeleted, xlrec->nupdated,
								 xlrec->isCatalogRel ? 'T' : 'F');

				if (XLogRecHasBlockData(record, 0))
					delvacuum_desc(buf, XLogRecGetBlockData(record, 0, NULL),
								   xlrec->ndeleted, xlrec->nupdated);
				break;
			}
		case XLOG_TREEB_MARK_PAGE_HALFDEAD:
			{
				xl_treeb_mark_page_halfdead *xlrec = (xl_treeb_mark_page_halfdead *) rec;

				appendStringInfo(buf, "topparent: %u, leaf: %u, left: %u, right: %u",
								 xlrec->topparent, xlrec->leafblk, xlrec->leftblk, xlrec->rightblk);
				break;
			}
		case XLOG_TREEB_UNLINK_PAGE_META:
		case XLOG_TREEB_UNLINK_PAGE:
			{
				xl_treeb_unlink_page *xlrec = (xl_treeb_unlink_page *) rec;

				appendStringInfo(buf, "left: %u, right: %u, level: %u, safexid: %u:%u, ",
								 xlrec->leftsib, xlrec->rightsib, xlrec->level,
								 EpochFromFullTransactionId(xlrec->safexid),
								 XidFromFullTransactionId(xlrec->safexid));
				appendStringInfo(buf, "leafleft: %u, leafright: %u, leaftopparent: %u",
								 xlrec->leafleftsib, xlrec->leafrightsib,
								 xlrec->leaftopparent);
				break;
			}
		case XLOG_TREEB_NEWROOT:
			{
				xl_treeb_newroot *xlrec = (xl_treeb_newroot *) rec;

				appendStringInfo(buf, "level: %u", xlrec->level);
				break;
			}
		case XLOG_TREEB_REUSE_PAGE:
			{
				xl_treeb_reuse_page *xlrec = (xl_treeb_reuse_page *) rec;

				appendStringInfo(buf, "rel: %u/%u/%u, snapshotConflictHorizon: %u:%u, isCatalogRel: %c",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber,
								 EpochFromFullTransactionId(xlrec->snapshotConflictHorizon),
								 XidFromFullTransactionId(xlrec->snapshotConflictHorizon),
								 xlrec->isCatalogRel ? 'T' : 'F');
				break;
			}
		case XLOG_TREEB_META_CLEANUP:
			{
				xl_treeb_metadata *xlrec;

				xlrec = (xl_treeb_metadata *) XLogRecGetBlockData(record, 0,
																  NULL);
				appendStringInfo(buf, "last_cleanup_num_delpages: %u",
								 xlrec->last_cleanup_num_delpages);
				break;
			}
	}
}

const char *
treeb_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_TREEB_INSERT_LEAF:
			id = "INSERT_LEAF";
			break;
		case XLOG_TREEB_INSERT_UPPER:
			id = "INSERT_UPPER";
			break;
		case XLOG_TREEB_INSERT_META:
			id = "INSERT_META";
			break;
		case XLOG_TREEB_SPLIT_L:
			id = "SPLIT_L";
			break;
		case XLOG_TREEB_SPLIT_R:
			id = "SPLIT_R";
			break;
		case XLOG_TREEB_INSERT_POST:
			id = "INSERT_POST";
			break;
		case XLOG_TREEB_DEDUP:
			id = "DEDUP";
			break;
		case XLOG_TREEB_VACUUM:
			id = "VACUUM";
			break;
		case XLOG_TREEB_DELETE:
			id = "DELETE";
			break;
		case XLOG_TREEB_MARK_PAGE_HALFDEAD:
			id = "MARK_PAGE_HALFDEAD";
			break;
		case XLOG_TREEB_UNLINK_PAGE:
			id = "UNLINK_PAGE";
			break;
		case XLOG_TREEB_UNLINK_PAGE_META:
			id = "UNLINK_PAGE_META";
			break;
		case XLOG_TREEB_NEWROOT:
			id = "NEWROOT";
			break;
		case XLOG_TREEB_REUSE_PAGE:
			id = "REUSE_PAGE";
			break;
		case XLOG_TREEB_META_CLEANUP:
			id = "META_CLEANUP";
			break;
	}

	return id;
}

static void
delvacuum_desc(StringInfo buf, char *block_data,
			   uint16 ndeleted, uint16 nupdated)
{
	OffsetNumber *deletedoffsets;
	OffsetNumber *updatedoffsets;
	xl_treeb_update *updates;

	/* Output deleted page offset number array */
	appendStringInfoString(buf, ", deleted:");
	deletedoffsets = (OffsetNumber *) block_data;
	array_desc(buf, deletedoffsets, sizeof(OffsetNumber), ndeleted,
			   &offset_elem_desc, NULL);

	/*
	 * Output updates as an array of "update objects", where each element
	 * contains a page offset number from updated array.  (This is not the
	 * most literal representation of the underlying physical data structure
	 * that we could use.  Readability seems more important here.)
	 */
	appendStringInfoString(buf, ", updated: [");
	updatedoffsets = (OffsetNumber *) (block_data + ndeleted *
									   sizeof(OffsetNumber));
	updates = (xl_treeb_update *) ((char *) updatedoffsets +
								   nupdated *
								   sizeof(OffsetNumber));
	for (int i = 0; i < nupdated; i++)
	{
		OffsetNumber off = updatedoffsets[i];

		Assert(OffsetNumberIsValid(off));
		Assert(updates->ndeletedtids > 0);

		/*
		 * "ptid" is the symbol name used when building each xl_treeb_update's
		 * array of offsets into a posting list tuple's ItemPointerData array.
		 * xl_treeb_update describes a subset of the existing TIDs to delete.
		 */
		appendStringInfo(buf, "{ off: %u, nptids: %u, ptids: [",
						 off, updates->ndeletedtids);
		for (int p = 0; p < updates->ndeletedtids; p++)
		{
			uint16	   *ptid;

			ptid = (uint16 *) ((char *) updates + SizeOfTreebUpdate) + p;
			appendStringInfo(buf, "%u", *ptid);

			if (p < updates->ndeletedtids - 1)
				appendStringInfoString(buf, ", ");
		}
		appendStringInfoString(buf, "] }");
		if (i < nupdated - 1)
			appendStringInfoString(buf, ", ");

		updates = (xl_treeb_update *)
			((char *) updates + SizeOfTreebUpdate +
			 updates->ndeletedtids * sizeof(uint16));
	}
	appendStringInfoChar(buf, ']');
}
