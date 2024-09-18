--
--  Create access method implementations tests
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

-- CREATE / DROP / ALTER IMPLEMENTATION basic checks.

SELECT count(*) FROM pg_amimpl;

CREATE IMPLEMENTATION btree_alias FOR ACCESS METHOD btree HANDLER bthandler;

-- Fail - names are globally unique.
CREATE IMPLEMENTATION btree_alias FOR ACCESS METHOD btree HANDLER bthandler;

-- Fail - wrong rettype: int4in is not an index_am_handler.
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD btree HANDLER int4in;

-- Fail - implementations may not target table AMs.
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD heap HANDLER bthandler;

-- Fail - AM must exist.
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD nosuch HANDLER bthandler;

-- Fail - opclass AM must exist and be an index AM.
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD btree
    HANDLER bthandler USING nosuch OPCLASSES;
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD btree
    HANDLER bthandler USING heap OPCLASSES;

SELECT i.implname, a.amname
FROM pg_amimpl i JOIN pg_am a ON a.oid = i.amoid
ORDER BY 1;

ALTER IMPLEMENTATION btree_alias RENAME TO btree_alias_renamed;

COMMENT ON IMPLEMENTATION btree_alias_renamed IS 'a btree alias';
SELECT pg_catalog.obj_description(oid, 'pg_amimpl')
FROM pg_amimpl WHERE implname = 'btree_alias_renamed';

DROP IMPLEMENTATION btree_alias_renamed;
SELECT count(*) FROM pg_amimpl;

-- bthandler_dummy wraps bthandler's IndexAmRoutine, appending custom
-- amcandummy/amrundummy: an extension shipping a private superset of
-- IndexAmRoutine in its handler.
CREATE FUNCTION bthandler_dummy(internal)
    RETURNS index_am_handler
    AS :'regresslib', 'bthandler_dummy'
    LANGUAGE C;

CREATE FUNCTION test_run_amrundummy(regclass)
    RETURNS bool
    AS :'regresslib', 'test_run_amrundummy'
    LANGUAGE C STRICT;

CREATE IMPLEMENTATION btree_dummy FOR ACCESS METHOD btree
    HANDLER bthandler_dummy;

CREATE TABLE amimpl_dummy_tbl (i int);
INSERT INTO amimpl_dummy_tbl SELECT g FROM generate_series(1, 100) g;

CREATE INDEX amimpl_dummy_idx ON amimpl_dummy_tbl USING btree (i)
    IMPLEMENTATION btree_dummy;

SELECT test_run_amrundummy('amimpl_dummy_idx');

SET enable_seqscan = off;
SELECT count(*) FROM amimpl_dummy_tbl WHERE i BETWEEN 10 AND 20;
RESET enable_seqscan;

DROP TABLE amimpl_dummy_tbl;
DROP IMPLEMENTATION btree_dummy;

-- Index has DEPENDENCY_NORMAL on its implementation: plain DROP fails,
-- CASCADE removes the index.
CREATE IMPLEMENTATION btree_drop_test FOR ACCESS METHOD btree
    HANDLER bthandler_dummy;

CREATE TABLE amimpl_drop_tbl (i int);
INSERT INTO amimpl_drop_tbl SELECT g FROM generate_series(1, 10) g;

CREATE INDEX amimpl_drop_idx ON amimpl_drop_tbl USING btree (i)
    IMPLEMENTATION btree_drop_test;

-- Fail - the index depends on the implementation.
DROP IMPLEMENTATION btree_drop_test;

SELECT c.relname,
       (SELECT implname FROM pg_amimpl WHERE oid = i.indimpl) AS implname
FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
WHERE c.relname = 'amimpl_drop_idx';

DROP IMPLEMENTATION btree_drop_test CASCADE;

-- The index is gone; the table is untouched.
SELECT count(*) FROM pg_class WHERE relname = 'amimpl_drop_idx';
SELECT count(*) FROM amimpl_drop_tbl;

DROP TABLE amimpl_drop_tbl;

-- RENAME preserves OID, so existing indexes keep working.
CREATE IMPLEMENTATION btree_alter_test FOR ACCESS METHOD btree
    HANDLER bthandler_dummy;

CREATE TABLE amimpl_alter_tbl (i int);
INSERT INTO amimpl_alter_tbl SELECT g FROM generate_series(1, 100) g;

CREATE INDEX amimpl_alter_idx ON amimpl_alter_tbl USING btree (i)
    IMPLEMENTATION btree_alter_test;

-- Capture the implementation OID stored in pg_index before the rename.
SELECT indimpl AS impl_oid_before
FROM pg_index WHERE indexrelid = 'amimpl_alter_idx'::regclass \gset

ALTER IMPLEMENTATION btree_alter_test RENAME TO btree_alter_renamed;

-- The OID in pg_index is unchanged; the new name is reachable through it.
SELECT indimpl = :impl_oid_before AS oid_unchanged,
       (SELECT implname FROM pg_amimpl WHERE oid = i.indimpl) AS implname
FROM pg_index i WHERE i.indexrelid = 'amimpl_alter_idx'::regclass;

-- The index is still bound to the dummy implementation and still works.
SELECT test_run_amrundummy('amimpl_alter_idx');
SET enable_seqscan = off;
SELECT count(*) FROM amimpl_alter_tbl WHERE i BETWEEN 30 AND 50;
RESET enable_seqscan;

DROP TABLE amimpl_alter_tbl;
DROP IMPLEMENTATION btree_alter_renamed;

--
-- gist_as_btree: gisthandler as a btree implementation
--

-- Default opclass-resolution AM (btree): gist's initGISTstate runs
-- against btree procs and errors cleanly on missing gist support.
CREATE IMPLEMENTATION gist_as_btree FOR ACCESS METHOD btree
    HANDLER gisthandler;

SELECT i.implname, a.amname AS am, p.proname AS handler
FROM pg_amimpl i
JOIN pg_am a ON a.oid = i.amoid
JOIN pg_proc p ON p.oid = i.implhandler
WHERE i.implname = 'gist_as_btree';

CREATE TABLE amimpl_gist_tbl (i int);
CREATE INDEX amimpl_gist_idx ON amimpl_gist_tbl USING btree (i)
    IMPLEMENTATION gist_as_btree;
DROP TABLE amimpl_gist_tbl;

DROP IMPLEMENTATION gist_as_btree;
SELECT count(*) FROM pg_amimpl;

-- USING gist OPCLASSES resolves column opclasses against gist.
-- Catalog AM (pg_class.relam) stays btree; runtime AM is gist.
CREATE FUNCTION test_print_index_am_handler(regclass)
    RETURNS text
    AS :'regresslib', 'test_print_index_am_handler'
    LANGUAGE C STRICT;

CREATE IMPLEMENTATION gist_as_btree FOR ACCESS METHOD btree
    HANDLER gisthandler
    USING gist OPCLASSES;

SELECT i.implname, a.amname AS am, oa.amname AS opcam, p.proname AS handler
FROM pg_amimpl i
JOIN pg_am a ON a.oid = i.amoid
JOIN pg_am oa ON oa.oid = i.implam
JOIN pg_proc p ON p.oid = i.implhandler
WHERE i.implname = 'gist_as_btree';

CREATE TABLE amimpl_gist_tbl (r int4range);
INSERT INTO amimpl_gist_tbl
SELECT int4range(g, g + 10) FROM generate_series(1, 100, 5) g;

CREATE INDEX amimpl_gist_idx ON amimpl_gist_tbl USING btree (r)
    IMPLEMENTATION gist_as_btree;

-- Dump resolved IndexAmRoutine. amimpl_gist_idx uses gist_as_btree,
-- so we see gist properties (amcanorderbyop=t). A stock btree index
-- below shows bthandler values (amcanorderbyop=f) for comparison.
SELECT regexp_split_to_table(test_print_index_am_handler('amimpl_gist_idx'),
                             E'\n') AS gist_as_btree;

CREATE TABLE btree_tbl (i int);
CREATE INDEX btree_idx ON btree_tbl (i);
SELECT regexp_split_to_table(test_print_index_am_handler('btree_idx'),
                             E'\n') AS stock_btree;
DROP TABLE btree_tbl;

-- Index scans through the gist-cloned routine.
SET enable_seqscan = off;
SELECT count(*) FROM amimpl_gist_tbl WHERE r && int4range(20, 40);
SELECT r FROM amimpl_gist_tbl WHERE r @> 42 ORDER BY r;

-- Insert
INSERT INTO amimpl_gist_tbl VALUES (int4range(200, 210)), (int4range(220, 230));
SELECT count(*) FROM amimpl_gist_tbl WHERE r && int4range(195, 225);

-- Update (drives aminsert for the new index entry of the moved row)
UPDATE amimpl_gist_tbl SET r = int4range(500, 510) WHERE r = int4range(46, 56);
SELECT r FROM amimpl_gist_tbl WHERE r @> 505;
RESET enable_seqscan;

DROP TABLE amimpl_gist_tbl;
DROP IMPLEMENTATION gist_as_btree;
DROP FUNCTION test_print_index_am_handler(regclass);

--
-- Round-trip: pg_get_indexdef emits IMPLEMENTATION; REINDEX preserves it.
--
CREATE IMPLEMENTATION btree_def_test FOR ACCESS METHOD btree
    HANDLER bthandler_dummy;
CREATE TABLE amimpl_def_tbl (i int);
INSERT INTO amimpl_def_tbl SELECT g FROM generate_series(1, 50) g;
CREATE INDEX amimpl_def_idx ON amimpl_def_tbl (i)
    IMPLEMENTATION btree_def_test;
SELECT pg_get_indexdef('amimpl_def_idx'::regclass);
REINDEX INDEX amimpl_def_idx;
SELECT test_run_amrundummy('amimpl_def_idx');
DROP TABLE amimpl_def_tbl;
DROP IMPLEMENTATION btree_def_test;

-- CREATE INDEX rejects an impl registered for a different AM.
CREATE IMPLEMENTATION btree_only_test FOR ACCESS METHOD btree
    HANDLER bthandler_dummy;
CREATE TABLE amimpl_cross_tbl (p point);
CREATE INDEX ON amimpl_cross_tbl USING gist (p) IMPLEMENTATION btree_only_test;
DROP TABLE amimpl_cross_tbl;
DROP IMPLEMENTATION btree_only_test;

-- UNIQUE enforcement runs through the impl's aminsert.
CREATE IMPLEMENTATION btree_uniq_test FOR ACCESS METHOD btree
    HANDLER bthandler_dummy;
CREATE TABLE amimpl_uniq_tbl (i int);
CREATE UNIQUE INDEX amimpl_uniq_idx ON amimpl_uniq_tbl (i)
    IMPLEMENTATION btree_uniq_test;
INSERT INTO amimpl_uniq_tbl VALUES (1), (2);
INSERT INTO amimpl_uniq_tbl VALUES (1);
DROP TABLE amimpl_uniq_tbl;
DROP IMPLEMENTATION btree_uniq_test;

-- Partitioned index: only the parent currently carries indimpl;
-- generateClonedIndexStmt does not propagate idxImpl to children.
CREATE IMPLEMENTATION btree_part_test FOR ACCESS METHOD btree
    HANDLER bthandler_dummy;
CREATE TABLE amimpl_part_tbl (i int) PARTITION BY RANGE (i);
CREATE TABLE amimpl_part_p1 PARTITION OF amimpl_part_tbl
    FOR VALUES FROM (0) TO (100);
CREATE TABLE amimpl_part_p2 PARTITION OF amimpl_part_tbl
    FOR VALUES FROM (100) TO (200);
CREATE INDEX amimpl_part_idx ON amimpl_part_tbl (i)
    IMPLEMENTATION btree_part_test;
SELECT c.relname,
       (SELECT implname FROM pg_amimpl WHERE oid = i.indimpl) AS impl
FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
WHERE c.relname LIKE 'amimpl_part%'
ORDER BY c.relname;
DROP TABLE amimpl_part_tbl;
DROP IMPLEMENTATION btree_part_test;

-- Impl depends on handler proc: DROP FUNCTION fails, CASCADE removes impl.
CREATE FUNCTION bthandler_dummy2(internal)
    RETURNS index_am_handler
    AS :'regresslib', 'bthandler_dummy'
    LANGUAGE C;
CREATE IMPLEMENTATION btree_handler_dep FOR ACCESS METHOD btree
    HANDLER bthandler_dummy2;
DROP FUNCTION bthandler_dummy2(internal);
DROP FUNCTION bthandler_dummy2(internal) CASCADE;
SELECT count(*) FROM pg_amimpl WHERE implname = 'btree_handler_dep';

DROP FUNCTION test_run_amrundummy(regclass);
DROP FUNCTION bthandler_dummy(internal);
