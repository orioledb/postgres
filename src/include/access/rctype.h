/*-------------------------------------------------------------------------
 *
 * rctype.h
 *	  POSTGRES row compare type definitions.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/rctype.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RCTYPE_H
#define RCTYPE_H

/*
 * Operator types identify the semantics that operators have irrespective of
 * operator classes, families, or strategies.  Only semantics relevant to
 * planner decisions need be added here.
 *
 * ROWCOMPARE_NONE corresponds to index strategies for which there are no
 * generic row comparison semantics.
 *
 * ROWCOMPARE_INVALID corresponds to a strategy not implemented by the index.
 */
typedef enum RowCompareType
{
	ROWCOMPARE_NONE   = 0,
	ROWCOMPARE_LT     = 1,
	ROWCOMPARE_LE     = 2,
	ROWCOMPARE_EQ     = 3,
	ROWCOMPARE_GE     = 4,
	ROWCOMPARE_GT     = 5,
	ROWCOMPARE_NE     = 6,
	ROWCOMPARE_INVALID = 7
} RowCompareType;
#define RCTYPE_MIN ROWCOMPARE_NONE
#define RCTYPE_MAX RCTYPE_NE

#endif							/* RCTYPE_H */
