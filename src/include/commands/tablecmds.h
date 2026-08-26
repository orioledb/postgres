/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/tablecmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "access/htup.h"
#include "catalog/dependency.h"
#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"
#include "storage/lock.h"
#include "utils/relcache.h"

struct AlterTableUtilityContext;	/* avoid including tcop/utility.h here */


/* --------------------------------------------------------------------
 * ALTER TABLE phase machinery.
 *
 * The pending-work queue for an ALTER TABLE is a List of AlteredTableInfo
 * structs, one for each table modified by the operation.  This struct is
 * exposed here so that table access methods (via the
 * relation_alter_table_finish callback in TableAmRoutine) can inspect the
 * cumulative effect of a whole ALTER TABLE command.  The fields use only
 * types from public headers; List cells for constraints/newvals are opaque
 * pointers (NewConstraint/NewColumnValue stay private to tablecmds.c).
 * --------------------------------------------------------------------
 */
typedef enum AlterTablePass
{
	AT_PASS_UNSET = -1,			/* UNSET will cause ERROR */
	AT_PASS_DROP,				/* DROP (all flavors) */
	AT_PASS_ALTER_TYPE,			/* ALTER COLUMN TYPE */
	AT_PASS_ADD_COL,			/* ADD COLUMN */
	AT_PASS_SET_EXPRESSION,		/* ALTER SET EXPRESSION */
	AT_PASS_OLD_INDEX,			/* re-add existing indexes */
	AT_PASS_OLD_CONSTR,			/* re-add existing constraints */
	/* We could support a RENAME COLUMN pass here, but not currently used */
	AT_PASS_ADD_CONSTR,			/* ADD constraints (initial examination) */
	AT_PASS_COL_ATTRS,			/* set column attributes, eg NOT NULL */
	AT_PASS_ADD_INDEXCONSTR,	/* ADD index-based constraints */
	AT_PASS_ADD_INDEX,			/* ADD indexes */
	AT_PASS_ADD_OTHERCONSTR,	/* ADD other constraints, defaults */
	AT_PASS_MISC,				/* other stuff */
} AlterTablePass;

#define AT_NUM_PASSES			(AT_PASS_MISC + 1)

typedef struct AlteredTableInfo
{
	/* Information saved before any work commences: */
	Oid			relid;			/* Relation to work on */
	char		relkind;		/* Its relkind */
	TupleDesc	oldDesc;		/* Pre-modification tuple descriptor */

	/*
	 * Transiently set during Phase 2, normally set to NULL.
	 *
	 * ATRewriteCatalogs sets this when it starts, and closes when ATExecCmd
	 * returns control.  This can be exploited by ATExecCmd subroutines to
	 * close/reopen across transaction boundaries.
	 */
	Relation	rel;

	/* Information saved by Phase 1 for Phase 2: */
	List	   *subcmds[AT_NUM_PASSES]; /* Lists of AlterTableCmd */
	/* Information saved by Phases 1/2 for Phase 3: */
	List	   *constraints;	/* List of NewConstraint */
	List	   *newvals;		/* List of NewColumnValue */
	List	   *afterStmts;		/* List of utility command parsetrees */
	bool		verify_new_notnull; /* T if we should recheck NOT NULL */
	int			rewrite;		/* Reason for forced rewrite, if any */
	bool		chgAccessMethod;	/* T if SET ACCESS METHOD is used */
	Oid			newAccessMethod;	/* new access method; 0 means no change,
									 * if above is true */
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
	bool		chgPersistence; /* T if SET LOGGED/UNLOGGED is used */
	char		newrelpersistence;	/* if above is true */
	Expr	   *partition_constraint;	/* for attach partition validation */
	/* true, if validating default due to some other attach/detach */
	bool		validate_default;
	/* Objects to rebuild after completing ALTER TYPE operations */
	List	   *changedConstraintOids;	/* OIDs of constraints to rebuild */
	List	   *changedConstraintDefs;	/* string definitions of same */
	List	   *changedIndexOids;	/* OIDs of indexes to rebuild */
	List	   *changedIndexDefs;	/* string definitions of same */
	char	   *replicaIdentityIndex;	/* index to reset as REPLICA IDENTITY */
	char	   *clusterOnIndex; /* index to use for CLUSTER */
	List	   *changedStatisticsOids;	/* OIDs of statistics to rebuild */
	List	   *changedStatisticsDefs;	/* string definitions of same */
} AlteredTableInfo;


extern ObjectAddress DefineRelation(CreateStmt *stmt, char relkind, Oid ownerId,
									ObjectAddress *typaddress, const char *queryString);

/*
 * LookupAlteredTableInfo - return the AlteredTableInfo for `relid` from the
 * ALTER TABLE phase-2 work queue currently being executed, or NULL if no
 * ALTER TABLE phase 2 is in progress (or `relid` is not one of its targets).
 * Intended for in-process table AMs whose object_access_hook callbacks fire
 * during phase 2 and need PG's own cumulative state (e.g. tab->rewrite).
 */
extern AlteredTableInfo *LookupAlteredTableInfo(Oid relid);

extern TupleDesc BuildDescForRelation(const List *columns);

extern void RemoveRelations(DropStmt *drop);

extern Oid	AlterTableLookupRelation(AlterTableStmt *stmt, LOCKMODE lockmode);

extern void AlterTable(AlterTableStmt *stmt, LOCKMODE lockmode,
					   struct AlterTableUtilityContext *context);

extern LOCKMODE AlterTableGetLockLevel(List *cmds);

extern void ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing, LOCKMODE lockmode);

extern void AlterTableInternal(Oid relid, List *cmds, bool recurse);

extern Oid	AlterTableMoveAll(AlterTableMoveAllStmt *stmt);

extern ObjectAddress AlterTableNamespace(AlterObjectSchemaStmt *stmt,
										 Oid *oldschema);

extern void AlterTableNamespaceInternal(Relation rel, Oid oldNspOid,
										Oid nspOid, ObjectAddresses *objsMoved);

extern void AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
										   Oid oldNspOid, Oid newNspOid,
										   bool hasDependEntry,
										   ObjectAddresses *objsMoved);

extern void CheckTableNotInUse(Relation rel, const char *stmt);

extern void ExecuteTruncate(TruncateStmt *stmt);
extern void ExecuteTruncateGuts(List *explicit_rels,
								List *relids,
								List *relids_logged,
								DropBehavior behavior,
								bool restart_seqs,
								bool run_as_table_owner);

extern void SetRelationHasSubclass(Oid relationId, bool relhassubclass);

extern bool CheckRelationTableSpaceMove(Relation rel, Oid newTableSpaceId);
extern void SetRelationTableSpace(Relation rel, Oid newTableSpaceId,
								  RelFileNumber newRelFilenumber);

extern ObjectAddress renameatt(RenameStmt *stmt);

extern ObjectAddress RenameConstraint(RenameStmt *stmt);

extern ObjectAddress RenameRelation(RenameStmt *stmt);

extern void RenameRelationInternal(Oid myrelid,
								   const char *newrelname, bool is_internal,
								   bool is_index);

extern void ResetRelRewrite(Oid myrelid);

extern void find_composite_type_dependencies(Oid typeOid,
											 Relation origRelation,
											 const char *origTypeName);

extern void check_of_type(HeapTuple typetuple);

extern void register_on_commit_action(Oid relid, OnCommitAction action);
extern void remove_on_commit_action(Oid relid);

extern void PreCommit_on_commit_actions(void);
extern void AtEOXact_on_commit_actions(bool isCommit);
extern void AtEOSubXact_on_commit_actions(bool isCommit,
										  SubTransactionId mySubid,
										  SubTransactionId parentSubid);

extern void RangeVarCallbackMaintainsTable(const RangeVar *relation,
										   Oid relId, Oid oldRelId,
										   void *arg);

extern void RangeVarCallbackOwnsRelation(const RangeVar *relation,
										 Oid relId, Oid oldRelId, void *arg);
extern bool PartConstraintImpliedByRelConstraint(Relation scanrel,
												 List *partConstraint);

#endif							/* TABLECMDS_H */
