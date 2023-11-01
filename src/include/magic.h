/*-------------------------------------------------------------------------
 *
 * magic.h
 *	  Definitions for the Postgres function manager and function-call
 *	  interface.
 *
 * This file must be included by all Postgres modules that either define
 * or call fmgr-callable functions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/magic.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MAGIC_H
#define MAGIC_H

/*-------------------------------------------------------------------------
 *		Support for verifying backend compatibility of loaded modules
 *
 * We require dynamically-loaded modules to include the macro call
 *		PG_MODULE_MAGIC;
 * so that we can check for obvious incompatibility, such as being compiled
 * for a different major PostgreSQL version.
 *
 * To compile with versions of PostgreSQL that do not support this,
 * you may put an #ifdef/#endif test around it.  Note that in a multiple-
 * source-file module, the macro call should only appear once.
 *
 * The specific items included in the magic block are intended to be ones that
 * are custom-configurable and especially likely to break dynamically loaded
 * modules if they were compiled with other values.  Also, the length field
 * can be used to detect definition changes.
 *
 * Note: we compare magic blocks with memcmp(), so there had better not be
 * any alignment pad bytes in them.
 *
 * Note: when changing the contents of magic blocks, be sure to adjust the
 * incompatible_module_error() function in dfmgr.c.
 *-------------------------------------------------------------------------
 */

/* Definition of the magic block structure */
typedef struct
{
	int			len;			/* sizeof(this struct) */
	int			version;		/* PostgreSQL major version */
	int			funcmaxargs;	/* FUNC_MAX_ARGS */
	int			indexmaxkeys;	/* INDEX_MAX_KEYS */
	int			namedatalen;	/* NAMEDATALEN */
	int			float8byval;	/* FLOAT8PASSBYVAL */
	char		abi_extra[32];	/* see pg_config_manual.h */
} Pg_magic_struct;

/* The actual data block contents */
#define PG_MODULE_MAGIC_DATA \
{ \
	sizeof(Pg_magic_struct), \
	PG_VERSION_NUM / 100, \
	FUNC_MAX_ARGS, \
	INDEX_MAX_KEYS, \
	NAMEDATALEN, \
	FLOAT8PASSBYVAL, \
	FMGR_ABI_EXTRA, \
}

StaticAssertDecl(sizeof(FMGR_ABI_EXTRA) <= sizeof(((Pg_magic_struct *) 0)->abi_extra),
				 "FMGR_ABI_EXTRA too long");

/*
 * Declare the module magic function.  It needs to be a function as the dlsym
 * in the backend is only guaranteed to work on functions, not data
 */
typedef const Pg_magic_struct *(*PGModuleMagicFunction) (void);

#define PG_MAGIC_FUNCTION_NAME Pg_magic_func
#define PG_MAGIC_FUNCTION_NAME_STRING "Pg_magic_func"

#define PG_MODULE_MAGIC \
extern PGDLLEXPORT const Pg_magic_struct *PG_MAGIC_FUNCTION_NAME(void); \
const Pg_magic_struct * \
PG_MAGIC_FUNCTION_NAME(void) \
{ \
	static const Pg_magic_struct Pg_magic_data = PG_MODULE_MAGIC_DATA; \
	return &Pg_magic_data; \
} \
extern int no_such_variable

#endif							/* MAGIC_H */