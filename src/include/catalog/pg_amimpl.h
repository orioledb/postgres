/*-------------------------------------------------------------------------
 *
 * pg_amimpl.h
 *	  definition of the "access method" system catalog (pg_am)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#include "catalog/pg_amimpl_d.h"

/* ----------------
 *		pg_amimpl definition.  cpp turns this into
 *		typedef struct FormData_pg_amimpl
 * ----------------
 */
CATALOG(pg_amimpl,8701,AccessMethodImplementationId)
{
	Oid			oid;				/* implementation oid */

	/* access method implementation name e.g. tableam name for particular index implementation */
	NameData	implname;				/* implementation name */
	Oid		amoid;					/* index/AM oid */

	/* handler function */
	regproc		implhandler BKI_LOOKUP(pg_proc);

} FormData_pg_amimpl;

/* ----------------
 *		Form_pg_am corresponds to a pointer to a tuple with
 *		the format of pg_am relation.
 * ----------------
 */
typedef FormData_pg_amimpl *Form_pg_amimpl;

DECLARE_UNIQUE_INDEX(pg_amimpl_implname_index, 8751, AmImplImplnameIndexId, on pg_amimpl using btree(implname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_amimpl_imploid_index, 8752, AmImplImploidIndexId, on pg_amimpl using btree(oid oid_ops));
DECLARE_INDEX(pg_amimpl_amoid_index, 8753, AmImplAmoidIndexId, on pg_amimpl using btree(amoid oid_ops));

#endif							/* PG_AMIMPL_H */
