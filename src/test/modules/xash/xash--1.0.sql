/* xash/xash--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION xash" to load this file. \quit

CREATE FUNCTION xash_indexam_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Access method
CREATE ACCESS METHOD xash TYPE INDEX HANDLER xash_indexam_handler;
COMMENT ON ACCESS METHOD xash IS 'test copy of hash';


-- Operator families
CREATE OPERATOR FAMILY "array_ops" USING "xash";

CREATE OPERATOR FAMILY "bpchar_ops" USING "xash";

CREATE OPERATOR FAMILY "char_ops" USING "xash";

CREATE OPERATOR FAMILY "date_ops" USING "xash";

CREATE OPERATOR FAMILY "float_ops" USING "xash";

ALTER OPERATOR FAMILY "float_ops" USING "xash" ADD
	OPERATOR 1 =("float4","float8"),
	OPERATOR 1 =("float8","float4");


CREATE OPERATOR FAMILY "network_ops" USING "xash";

CREATE OPERATOR FAMILY "integer_ops" USING "xash";

ALTER OPERATOR FAMILY "integer_ops" USING "xash" ADD
	OPERATOR 1 =("int2","int4"),
	OPERATOR 1 =("int2","int8"),
	OPERATOR 1 =("int4","int2"),
	OPERATOR 1 =("int4","int8"),
	OPERATOR 1 =("int8","int2"),
	OPERATOR 1 =("int8","int4");


CREATE OPERATOR FAMILY "interval_ops" USING "xash";

CREATE OPERATOR FAMILY "macaddr_ops" USING "xash";

CREATE OPERATOR FAMILY "macaddr8_ops" USING "xash";

CREATE OPERATOR FAMILY "numeric_ops" USING "xash";

CREATE OPERATOR FAMILY "oid_ops" USING "xash";

CREATE OPERATOR FAMILY "oidvector_ops" USING "xash";

CREATE OPERATOR FAMILY "record_ops" USING "xash";

CREATE OPERATOR FAMILY "text_ops" USING "xash";

ALTER OPERATOR FAMILY "text_ops" USING "xash" ADD
	OPERATOR 1 =("name","text"),
	OPERATOR 1 =("text","name");


CREATE OPERATOR FAMILY "time_ops" USING "xash";

CREATE OPERATOR FAMILY "timestamptz_ops" USING "xash";

CREATE OPERATOR FAMILY "timetz_ops" USING "xash";

CREATE OPERATOR FAMILY "timestamp_ops" USING "xash";

CREATE OPERATOR FAMILY "bool_ops" USING "xash";

CREATE OPERATOR FAMILY "bytea_ops" USING "xash";

CREATE OPERATOR FAMILY "xid_ops" USING "xash";

CREATE OPERATOR FAMILY "xid8_ops" USING "xash";

CREATE OPERATOR FAMILY "cid_ops" USING "xash";

CREATE OPERATOR FAMILY "tid_ops" USING "xash";

CREATE OPERATOR FAMILY "text_pattern_ops" USING "xash";

CREATE OPERATOR FAMILY "bpchar_pattern_ops" USING "xash";

CREATE OPERATOR FAMILY "aclitem_ops" USING "xash";

CREATE OPERATOR FAMILY "uuid_ops" USING "xash";

CREATE OPERATOR FAMILY "pg_lsn_ops" USING "xash";

CREATE OPERATOR FAMILY "enum_ops" USING "xash";

CREATE OPERATOR FAMILY "range_ops" USING "xash";

CREATE OPERATOR FAMILY "jsonb_ops" USING "xash";

CREATE OPERATOR FAMILY "multirange_ops" USING "xash";

-- Operator classes

CREATE OPERATOR CLASS "array_ops"
	DEFAULT FOR TYPE "anyarray" USING "xash" FAMILY "array_ops" AS
	OPERATOR 1 =("anyarray","anyarray"),
	FUNCTION 1 "hash_array",
	FUNCTION 2 "hash_array_extended";



CREATE OPERATOR CLASS "bpchar_ops"
	DEFAULT FOR TYPE "bpchar" USING "xash" FAMILY "bpchar_ops" AS
	OPERATOR 1 =("bpchar","bpchar"),
	FUNCTION 1 "hashbpchar",
	FUNCTION 2 "hashbpcharextended";



CREATE OPERATOR CLASS "char_ops"
	DEFAULT FOR TYPE "char" USING "xash" FAMILY "char_ops" AS
	OPERATOR 1 =("char","char"),
	FUNCTION 1 "hashchar",
	FUNCTION 2 "hashcharextended";



CREATE OPERATOR CLASS "date_ops"
	DEFAULT FOR TYPE "date" USING "xash" FAMILY "date_ops" AS
	OPERATOR 1 =("date","date"),
	FUNCTION 1 "hashint4",
	FUNCTION 2 "hashint4extended";



CREATE OPERATOR CLASS "float4_ops"
	DEFAULT FOR TYPE "float4" USING "xash" FAMILY "float_ops" AS
	OPERATOR 1 =("float4","float4"),
	FUNCTION 1 "hashfloat4",
	FUNCTION 2 "hashfloat4extended";



CREATE OPERATOR CLASS "float8_ops"
	DEFAULT FOR TYPE "float8" USING "xash" FAMILY "float_ops" AS
	OPERATOR 1 =("float8","float8"),
	FUNCTION 1 "hashfloat8",
	FUNCTION 2 "hashfloat8extended";



CREATE OPERATOR CLASS "inet_ops"
	DEFAULT FOR TYPE "inet" USING "xash" FAMILY "network_ops" AS
	OPERATOR 1 =("inet","inet"),
	FUNCTION 1 "hashinet",
	FUNCTION 2 "hashinetextended";



CREATE OPERATOR CLASS "int2_ops"
	DEFAULT FOR TYPE "int2" USING "xash" FAMILY "integer_ops" AS
	OPERATOR 1 =("int2","int2"),
	FUNCTION 1 "hashint2",
	FUNCTION 2 "hashint2extended";



CREATE OPERATOR CLASS "int4_ops"
	DEFAULT FOR TYPE "int4" USING "xash" FAMILY "integer_ops" AS
	OPERATOR 1 =("int4","int4"),
	FUNCTION 1 "hashint4",
	FUNCTION 2 "hashint4extended";



CREATE OPERATOR CLASS "int8_ops"
	DEFAULT FOR TYPE "int8" USING "xash" FAMILY "integer_ops" AS
	OPERATOR 1 =("int8","int8"),
	FUNCTION 1 "hashint8",
	FUNCTION 2 "hashint8extended";



CREATE OPERATOR CLASS "interval_ops"
	DEFAULT FOR TYPE "interval" USING "xash" FAMILY "interval_ops" AS
	OPERATOR 1 =("interval","interval"),
	FUNCTION 1 "interval_hash",
	FUNCTION 2 "interval_hash_extended";



CREATE OPERATOR CLASS "macaddr_ops"
	DEFAULT FOR TYPE "macaddr" USING "xash" FAMILY "macaddr_ops" AS
	OPERATOR 1 =("macaddr","macaddr"),
	FUNCTION 1 "hashmacaddr",
	FUNCTION 2 "hashmacaddrextended";



CREATE OPERATOR CLASS "macaddr8_ops"
	DEFAULT FOR TYPE "macaddr8" USING "xash" FAMILY "macaddr8_ops" AS
	OPERATOR 1 =("macaddr8","macaddr8"),
	FUNCTION 1 "hashmacaddr8",
	FUNCTION 2 "hashmacaddr8extended";



CREATE OPERATOR CLASS "name_ops"
	DEFAULT FOR TYPE "name" USING "xash" FAMILY "text_ops" AS
	OPERATOR 1 =("name","name"),
	FUNCTION 1 "hashname",
	FUNCTION 2 "hashnameextended";



CREATE OPERATOR CLASS "numeric_ops"
	DEFAULT FOR TYPE "numeric" USING "xash" FAMILY "numeric_ops" AS
	OPERATOR 1 =("numeric","numeric"),
	FUNCTION 1 "hash_numeric",
	FUNCTION 2 "hash_numeric_extended";



CREATE OPERATOR CLASS "oid_ops"
	DEFAULT FOR TYPE "oid" USING "xash" FAMILY "oid_ops" AS
	OPERATOR 1 =("oid","oid"),
	FUNCTION 1 "hashoid",
	FUNCTION 2 "hashoidextended";



CREATE OPERATOR CLASS "oidvector_ops"
	DEFAULT FOR TYPE "oidvector" USING "xash" FAMILY "oidvector_ops" AS
	OPERATOR 1 =("oidvector","oidvector"),
	FUNCTION 1 "hashoidvector",
	FUNCTION 2 "hashoidvectorextended";



CREATE OPERATOR CLASS "record_ops"
	DEFAULT FOR TYPE "record" USING "xash" FAMILY "record_ops" AS
	OPERATOR 1 =("record","record"),
	FUNCTION 1 "hash_record",
	FUNCTION 2 "hash_record_extended";



CREATE OPERATOR CLASS "text_ops"
	DEFAULT FOR TYPE "text" USING "xash" FAMILY "text_ops" AS
	OPERATOR 1 =("text","text"),
	FUNCTION 1 "hashtext",
	FUNCTION 2 "hashtextextended";



CREATE OPERATOR CLASS "time_ops"
	DEFAULT FOR TYPE "time" USING "xash" FAMILY "time_ops" AS
	OPERATOR 1 =("time","time"),
	FUNCTION 1 "time_hash",
	FUNCTION 2 "time_hash_extended";



CREATE OPERATOR CLASS "timestamptz_ops"
	DEFAULT FOR TYPE "timestamptz" USING "xash" FAMILY "timestamptz_ops" AS
	OPERATOR 1 =("timestamptz","timestamptz"),
	FUNCTION 1 "timestamp_hash",
	FUNCTION 2 "timestamp_hash_extended";



CREATE OPERATOR CLASS "timetz_ops"
	DEFAULT FOR TYPE "timetz" USING "xash" FAMILY "timetz_ops" AS
	OPERATOR 1 =("timetz","timetz"),
	FUNCTION 1 "timetz_hash",
	FUNCTION 2 "timetz_hash_extended";



CREATE OPERATOR CLASS "timestamp_ops"
	DEFAULT FOR TYPE "timestamp" USING "xash" FAMILY "timestamp_ops" AS
	OPERATOR 1 =("timestamp","timestamp"),
	FUNCTION 1 "timestamp_hash",
	FUNCTION 2 "timestamp_hash_extended";



CREATE OPERATOR CLASS "bool_ops"
	DEFAULT FOR TYPE "bool" USING "xash" FAMILY "bool_ops" AS
	OPERATOR 1 =("bool","bool"),
	FUNCTION 1 "hashchar",
	FUNCTION 2 "hashcharextended";



CREATE OPERATOR CLASS "bytea_ops"
	DEFAULT FOR TYPE "bytea" USING "xash" FAMILY "bytea_ops" AS
	OPERATOR 1 =("bytea","bytea"),
	FUNCTION 1 "hashvarlena",
	FUNCTION 2 "hashvarlenaextended";



CREATE OPERATOR CLASS "xid_ops"
	DEFAULT FOR TYPE "xid" USING "xash" FAMILY "xid_ops" AS
	OPERATOR 1 =("xid","xid"),
	FUNCTION 1 "hashint4",
	FUNCTION 2 "hashint4extended";



CREATE OPERATOR CLASS "xid8_ops"
	DEFAULT FOR TYPE "xid8" USING "xash" FAMILY "xid8_ops" AS
	OPERATOR 1 =("xid8","xid8"),
	FUNCTION 1 "hashint8",
	FUNCTION 2 "hashint8extended";



CREATE OPERATOR CLASS "cid_ops"
	DEFAULT FOR TYPE "cid" USING "xash" FAMILY "cid_ops" AS
	OPERATOR 1 =("cid","cid"),
	FUNCTION 1 "hashint4",
	FUNCTION 2 "hashint4extended";



CREATE OPERATOR CLASS "tid_ops"
	DEFAULT FOR TYPE "tid" USING "xash" FAMILY "tid_ops" AS
	OPERATOR 1 =("tid","tid"),
	FUNCTION 1 "hashtid",
	FUNCTION 2 "hashtidextended";



CREATE OPERATOR CLASS "text_pattern_ops"
	FOR TYPE "text" USING "xash" FAMILY "text_pattern_ops" AS
	OPERATOR 1 =("text","text"),
	FUNCTION 1 "hashtext",
	FUNCTION 2 "hashtextextended";



CREATE OPERATOR CLASS "bpchar_pattern_ops"
	FOR TYPE "bpchar" USING "xash" FAMILY "bpchar_pattern_ops" AS
	OPERATOR 1 =("bpchar","bpchar"),
	FUNCTION 1 "hashbpchar",
	FUNCTION 2 "hashbpcharextended";



CREATE OPERATOR CLASS "aclitem_ops"
	DEFAULT FOR TYPE "aclitem" USING "xash" FAMILY "aclitem_ops" AS
	OPERATOR 1 =("aclitem","aclitem"),
	FUNCTION 1 "hash_aclitem",
	FUNCTION 2 "hash_aclitem_extended";



CREATE OPERATOR CLASS "uuid_ops"
	DEFAULT FOR TYPE "uuid" USING "xash" FAMILY "uuid_ops" AS
	OPERATOR 1 =("uuid","uuid"),
	FUNCTION 1 "uuid_hash",
	FUNCTION 2 "uuid_hash_extended";



CREATE OPERATOR CLASS "pg_lsn_ops"
	DEFAULT FOR TYPE "pg_lsn" USING "xash" FAMILY "pg_lsn_ops" AS
	OPERATOR 1 =("pg_lsn","pg_lsn"),
	FUNCTION 1 "pg_lsn_hash",
	FUNCTION 2 "pg_lsn_hash_extended";



CREATE OPERATOR CLASS "enum_ops"
	DEFAULT FOR TYPE "anyenum" USING "xash" FAMILY "enum_ops" AS
	OPERATOR 1 =("anyenum","anyenum"),
	FUNCTION 1 "hashenum",
	FUNCTION 2 "hashenumextended";



CREATE OPERATOR CLASS "range_ops"
	DEFAULT FOR TYPE "anyrange" USING "xash" FAMILY "range_ops" AS
	OPERATOR 1 =("anyrange","anyrange"),
	FUNCTION 1 "hash_range",
	FUNCTION 2 "hash_range_extended";



CREATE OPERATOR CLASS "multirange_ops"
	DEFAULT FOR TYPE "anymultirange" USING "xash" FAMILY "multirange_ops" AS
	OPERATOR 1 =("anymultirange","anymultirange"),
	FUNCTION 1 "hash_multirange",
	FUNCTION 2 "hash_multirange_extended";



CREATE OPERATOR CLASS "jsonb_ops"
	DEFAULT FOR TYPE "jsonb" USING "xash" FAMILY "jsonb_ops" AS
	OPERATOR 1 =("jsonb","jsonb"),
	FUNCTION 1 "jsonb_hash",
	FUNCTION 2 "jsonb_hash_extended";
