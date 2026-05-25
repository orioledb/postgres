CREATE FUNCTION pg_list_orphaned(
	older_than interval default null,
	OUT dbname text,
	OUT path text,
	OUT name text,
	OUT size bigint,
	OUT mod_time timestamptz,
	OUT relfilenode bigint,
	OUT reloid bigint,
	OUT older bool)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME','pg_list_orphaned'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_list_orphaned_moved(
	OUT dbname text,
	OUT path text,
	OUT name text,
	OUT size bigint,
	OUT mod_time timestamptz,
	OUT relfilenode bigint,
	OUT reloid bigint)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME','pg_list_orphaned_moved'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_move_orphaned(older_than interval default null)
    RETURNS int
    LANGUAGE c
AS 'MODULE_PATHNAME', 'pg_move_orphaned';

CREATE FUNCTION pg_remove_moved_orphaned()
    RETURNS void
    LANGUAGE c
AS 'MODULE_PATHNAME', 'pg_remove_moved_orphaned';

CREATE FUNCTION pg_move_back_orphaned()
    RETURNS int
    LANGUAGE c
AS 'MODULE_PATHNAME', 'pg_move_back_orphaned';

revoke execute on function pg_list_orphaned(older_than interval) from public;
revoke execute on function pg_list_orphaned_moved() from public;
revoke execute on function pg_move_orphaned(older_than interval) from public;
revoke execute on function pg_remove_moved_orphaned() from public;
revoke execute on function pg_move_back_orphaned() from public;
