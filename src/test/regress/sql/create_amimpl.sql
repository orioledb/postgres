--
-- Create access method implementation tests
--

-- Make gist2 over gisthandler. In fact, it would be a synonym to gist.
CREATE IMPLEMENTATION test_btree_1 FOR ACCESS METHOD btree HANDLER testbtreehandler1;
CREATE IMPLEMENTATION test_btree_2 FOR ACCESS METHOD btree HANDLER testbtreehandler2;

CREATE ACCESS METHOD btree2 TYPE INDEX HANDLER testbtreehandler;

CREATE IMPLEMENTATION test_btree_1 FOR ACCESS METHOD btree2 HANDLER testbtreehandler1;
CREATE IMPLEMENTATION test_btree_2 FOR ACCESS METHOD btree2 HANDLER testbtreehandler2;

-- Implementation for non-index AM
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD heap HANDLER testbtreehandler1;

-- Verify return type checks for handlers
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD btree HANDLER int4in;
CREATE IMPLEMENTATION bogus FOR ACCESS METHOD btree HANDLER heap_tableam_handler;


SELECT implname, implhandler, amoid FROM pg_amimpl ORDER BY 1, 2;

-- Try to drop access method: fail because of dependent objects
DROP ACCESS METHOD btree2;



-- First create tables employing the new AM using USING

-- plain CREATE TABLE
CREATE TABLE tableam_tbl_heap2(f1 int) USING heap2;
INSERT INTO tableam_tbl_heap2 VALUES(1);
SELECT f1 FROM tableam_tbl_heap2 ORDER BY f1;

-- CREATE TABLE AS
CREATE TABLE tableam_tblas_heap2 USING heap2 AS SELECT * FROM tableam_tbl_heap2;
SELECT f1 FROM tableam_tbl_heap2 ORDER BY f1;

-- SELECT INTO doesn't support USING
SELECT INTO tableam_tblselectinto_heap2 USING heap2 FROM tableam_tbl_heap2;

-- CREATE VIEW doesn't support USING
CREATE VIEW tableam_view_heap2 USING heap2 AS SELECT * FROM tableam_tbl_heap2;

-- CREATE SEQUENCE doesn't support USING
CREATE SEQUENCE tableam_seq_heap2 USING heap2;

-- CREATE MATERIALIZED VIEW does support USING
CREATE MATERIALIZED VIEW tableam_tblmv_heap2 USING heap2 AS SELECT * FROM tableam_tbl_heap2;
SELECT f1 FROM tableam_tblmv_heap2 ORDER BY f1;

-- CREATE TABLE ..  PARTITION BY doesn't not support USING
CREATE TABLE tableam_parted_heap2 (a text, b int) PARTITION BY list (a) USING heap2;

CREATE TABLE tableam_parted_heap2 (a text, b int) PARTITION BY list (a);
-- new partitions will inherit from the current default, rather the partition root
SET default_table_access_method = 'heap';
CREATE TABLE tableam_parted_a_heap2 PARTITION OF tableam_parted_heap2 FOR VALUES IN ('a');
SET default_table_access_method = 'heap2';
CREATE TABLE tableam_parted_b_heap2 PARTITION OF tableam_parted_heap2 FOR VALUES IN ('b');
RESET default_table_access_method;
-- but the method can be explicitly specified
CREATE TABLE tableam_parted_c_heap2 PARTITION OF tableam_parted_heap2 FOR VALUES IN ('c') USING heap;
CREATE TABLE tableam_parted_d_heap2 PARTITION OF tableam_parted_heap2 FOR VALUES IN ('d') USING heap2;

-- List all objects in AM
SELECT
    pc.relkind,
    pa.amname,
    CASE WHEN relkind = 't' THEN
        (SELECT 'toast for ' || relname::regclass FROM pg_class pcm WHERE pcm.reltoastrelid = pc.oid)
    ELSE
        relname::regclass::text
    END COLLATE "C" AS relname
FROM pg_class AS pc,
    pg_am AS pa
WHERE pa.oid = pc.relam
   AND pa.amname = 'heap2'
ORDER BY 3, 1, 2;

-- Show dependencies onto AM - there shouldn't be any for toast
SELECT pg_describe_object(classid,objid,objsubid) AS obj
FROM pg_depend, pg_am
WHERE pg_depend.refclassid = 'pg_am'::regclass
    AND pg_am.oid = pg_depend.refobjid
    AND pg_am.amname = 'heap2'
ORDER BY classid, objid, objsubid;

-- ALTER TABLE SET ACCESS METHOD
CREATE TABLE heaptable USING heap AS
  SELECT a, repeat(a::text, 100) FROM generate_series(1,9) AS a;
SELECT amname FROM pg_class c, pg_am am
  WHERE c.relam = am.oid AND c.oid = 'heaptable'::regclass;
-- Switching to heap2 adds new dependency entry to the AM.
ALTER TABLE heaptable SET ACCESS METHOD heap2;
SELECT pg_describe_object(classid, objid, objsubid) as obj,
       pg_describe_object(refclassid, refobjid, refobjsubid) as objref,
       deptype
  FROM pg_depend
  WHERE classid = 'pg_class'::regclass AND
        objid = 'heaptable'::regclass
  ORDER BY 1, 2;
-- Switching to heap should not have a dependency entry to the AM.
ALTER TABLE heaptable SET ACCESS METHOD heap;
SELECT pg_describe_object(classid, objid, objsubid) as obj,
       pg_describe_object(refclassid, refobjid, refobjsubid) as objref,
       deptype
  FROM pg_depend
  WHERE classid = 'pg_class'::regclass AND
        objid = 'heaptable'::regclass
  ORDER BY 1, 2;
ALTER TABLE heaptable SET ACCESS METHOD heap2;
SELECT amname FROM pg_class c, pg_am am
  WHERE c.relam = am.oid AND c.oid = 'heaptable'::regclass;
SELECT COUNT(a), COUNT(1) FILTER(WHERE a=1) FROM heaptable;
-- ALTER MATERIALIZED VIEW SET ACCESS METHOD
CREATE MATERIALIZED VIEW heapmv USING heap AS SELECT * FROM heaptable;
SELECT amname FROM pg_class c, pg_am am
  WHERE c.relam = am.oid AND c.oid = 'heapmv'::regclass;
ALTER MATERIALIZED VIEW heapmv SET ACCESS METHOD heap2;
SELECT amname FROM pg_class c, pg_am am
  WHERE c.relam = am.oid AND c.oid = 'heapmv'::regclass;
SELECT COUNT(a), COUNT(1) FILTER(WHERE a=1) FROM heapmv;
-- No support for multiple subcommands
ALTER TABLE heaptable SET ACCESS METHOD heap, SET ACCESS METHOD heap2;
ALTER MATERIALIZED VIEW heapmv SET ACCESS METHOD heap, SET ACCESS METHOD heap2;
DROP MATERIALIZED VIEW heapmv;
DROP TABLE heaptable;
-- No support for partitioned tables.
CREATE TABLE am_partitioned(x INT, y INT)
  PARTITION BY hash (x);
ALTER TABLE am_partitioned SET ACCESS METHOD heap2;
DROP TABLE am_partitioned;

-- Second, create objects in the new AM by changing the default AM
BEGIN;
SET LOCAL default_table_access_method = 'heap2';

-- following tests should all respect the default AM
CREATE TABLE tableam_tbl_heapx(f1 int);
CREATE TABLE tableam_tblas_heapx AS SELECT * FROM tableam_tbl_heapx;
SELECT INTO tableam_tblselectinto_heapx FROM tableam_tbl_heapx;
CREATE MATERIALIZED VIEW tableam_tblmv_heapx USING heap2 AS SELECT * FROM tableam_tbl_heapx;
CREATE TABLE tableam_parted_heapx (a text, b int) PARTITION BY list (a);
CREATE TABLE tableam_parted_1_heapx PARTITION OF tableam_parted_heapx FOR VALUES IN ('a', 'b');

-- but an explicitly set AM overrides it
CREATE TABLE tableam_parted_2_heapx PARTITION OF tableam_parted_heapx FOR VALUES IN ('c', 'd') USING heap;

-- sequences, views and foreign servers shouldn't have an AM
CREATE VIEW tableam_view_heapx AS SELECT * FROM tableam_tbl_heapx;
CREATE SEQUENCE tableam_seq_heapx;
CREATE FOREIGN DATA WRAPPER fdw_heap2 VALIDATOR postgresql_fdw_validator;
CREATE SERVER fs_heap2 FOREIGN DATA WRAPPER fdw_heap2 ;
CREATE FOREIGN table tableam_fdw_heapx () SERVER fs_heap2;

-- Verify that new AM was used for tables, matviews, but not for sequences, views and fdws
SELECT
    pc.relkind,
    pa.amname,
    CASE WHEN relkind = 't' THEN
        (SELECT 'toast for ' || relname::regclass FROM pg_class pcm WHERE pcm.reltoastrelid = pc.oid)
    ELSE
        relname::regclass::text
    END COLLATE "C" AS relname
FROM pg_class AS pc
    LEFT JOIN pg_am AS pa ON (pa.oid = pc.relam)
WHERE pc.relname LIKE 'tableam_%_heapx'
ORDER BY 3, 1, 2;

-- don't want to keep those tables, nor the default
ROLLBACK;

-- Third, check that we can neither create a table using a nonexistent
-- AM, nor using an index AM
CREATE TABLE i_am_a_failure() USING "";
CREATE TABLE i_am_a_failure() USING i_do_not_exist_am;
CREATE TABLE i_am_a_failure() USING "I do not exist AM";
CREATE TABLE i_am_a_failure() USING "btree";

-- Drop table access method, which fails as objects depends on it
DROP ACCESS METHOD heap2;

-- we intentionally leave the objects created above alive, to verify pg_dump support
