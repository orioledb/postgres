/*
 * test_fmgr_abi.c
 *
 * Test module that presents the ABI marker used by PostgreSQL proper.
 */
#include "postgres.h"

#include "fmgr.h"

extern PGDLLEXPORT const Pg_magic_struct *Pg_magic_func(void);

const Pg_magic_struct *
Pg_magic_func(void)
{
	static const Pg_magic_struct magic =
	{
		.len = sizeof(Pg_magic_struct),
		.abi_fields =
		{
			PG_VERSION_NUM / 100,
			FUNC_MAX_ARGS,
			INDEX_MAX_KEYS,
			NAMEDATALEN,
			FLOAT8PASSBYVAL,
			"PostgreSQL",
		},
	};

	return &magic;
}
