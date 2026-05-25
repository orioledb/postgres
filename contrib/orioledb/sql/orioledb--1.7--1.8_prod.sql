/* contrib/orioledb/sql/orioledb--1.7--1.8_prod.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION orioledb UPDATE TO '1.8'" to load this file. \quit


CREATE FUNCTION orioledb_list_orphaned(
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
AS 'MODULE_PATHNAME', 'orioledb_list_orphaned'
LANGUAGE C VOLATILE;

CREATE FUNCTION orioledb_list_orphaned_moved(
	OUT dbname text,
	OUT path text,
	OUT name text,
	OUT size bigint,
	OUT mod_time timestamptz,
	OUT relfilenode bigint,
	OUT reloid bigint)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'orioledb_list_orphaned_moved'
LANGUAGE C VOLATILE;

CREATE FUNCTION orioledb_move_orphaned(older_than interval default null)
RETURNS int
AS 'MODULE_PATHNAME', 'orioledb_move_orphaned'
LANGUAGE C VOLATILE;

CREATE FUNCTION orioledb_remove_moved_orphaned()
RETURNS void
AS 'MODULE_PATHNAME', 'orioledb_remove_moved_orphaned'
LANGUAGE C VOLATILE;

CREATE FUNCTION orioledb_move_back_orphaned()
RETURNS int
AS 'MODULE_PATHNAME', 'orioledb_move_back_orphaned'
LANGUAGE C VOLATILE;

REVOKE EXECUTE ON FUNCTION orioledb_list_orphaned(interval) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION orioledb_list_orphaned_moved() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION orioledb_move_orphaned(interval) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION orioledb_remove_moved_orphaned() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION orioledb_move_back_orphaned() FROM PUBLIC;
