/*-------------------------------------------------------------------------
 *
 * pg_amimpl.h
 *	  definition of the "access method implementation" system catalog (pg_amimpl)
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_amimpl.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMIMPL_H
#define PG_AMIMPL_H

#include "catalog/genbki.h"
#include "catalog/pg_amimpl_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_amimpl definition.  cpp turns this into
 *		typedef struct FormData_pg_amimpl
 * ----------------
 */
CATALOG(pg_amimpl,8701,AccessMethodImplementationId)
{
	Oid			oid;			/* oid */

	/* access method implementation name */
	NameData	implname;

	/* OID of the access method (must be amtype = 'i') */
	Oid			amoid BKI_LOOKUP(pg_am);

	/*
	 * AM whose opclasses are consulted at index build time.  Equal to amoid
	 * for ordinary implementations; differs when the handler runs the code of
	 * a different AM (e.g. a btree implementation that uses gisthandler must
	 * resolve column opclasses against gist, not btree).
	 */
	Oid			implam BKI_LOOKUP(pg_am);

	/* handler function */
	regproc		implhandler BKI_LOOKUP(pg_proc);
} FormData_pg_amimpl;

/* ----------------
 *		Form_pg_amimpl corresponds to a pointer to a tuple with
 *		the format of pg_amimpl relation.
 * ----------------
 */
typedef FormData_pg_amimpl *Form_pg_amimpl;

DECLARE_UNIQUE_INDEX(pg_amimpl_implname_index, 8751, AmImplImplnameIndexId, pg_amimpl, btree(implname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_amimpl_imploid_index, 8752, AmImplImploidIndexId, pg_amimpl, btree(oid oid_ops));
DECLARE_INDEX(pg_amimpl_amoid_index, 8753, AmImplAmoidIndexId, pg_amimpl, btree(amoid oid_ops));

MAKE_SYSCACHE(AMIMPLNAME, pg_amimpl_implname_index, 4);
MAKE_SYSCACHE(AMIMPLOID, pg_amimpl_imploid_index, 4);

#endif							/* PG_AMIMPL_H */
