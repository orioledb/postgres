/* treeb/treeb--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION treeb" to load this file. \quit

CREATE FUNCTION treeb_indexam_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Access method
CREATE ACCESS METHOD treeb TYPE INDEX HANDLER treeb_indexam_handler;
COMMENT ON ACCESS METHOD treeb IS 'test copy of btree';


-- Operator families
CREATE OPERATOR FAMILY "array_ops" USING "treeb";

CREATE OPERATOR FAMILY "bit_ops" USING "treeb";

CREATE OPERATOR FAMILY "bool_ops" USING "treeb";

CREATE OPERATOR FAMILY "bpchar_ops" USING "treeb";

CREATE OPERATOR FAMILY "bytea_ops" USING "treeb";

CREATE OPERATOR FAMILY "char_ops" USING "treeb";

CREATE OPERATOR FAMILY "datetime_ops" USING "treeb";

ALTER OPERATOR FAMILY "datetime_ops" USING "treeb" ADD
	OPERATOR 5 <("date","timestamp"),
	OPERATOR 1 <=("date","timestamp"),
	OPERATOR 2 =("date","timestamp"),
	OPERATOR 3 >=("date","timestamp"),
	OPERATOR 4 >("date","timestamp"),
	OPERATOR 5 <("date","timestamptz"),
	OPERATOR 1 <=("date","timestamptz"),
	OPERATOR 2 =("date","timestamptz"),
	OPERATOR 3 >=("date","timestamptz"),
	OPERATOR 4 >("date","timestamptz"),
	OPERATOR 5 <("timestamp","date"),
	OPERATOR 1 <=("timestamp","date"),
	OPERATOR 2 =("timestamp","date"),
	OPERATOR 3 >=("timestamp","date"),
	OPERATOR 4 >("timestamp","date"),
	OPERATOR 5 <("timestamp","timestamptz"),
	OPERATOR 1 <=("timestamp","timestamptz"),
	OPERATOR 2 =("timestamp","timestamptz"),
	OPERATOR 3 >=("timestamp","timestamptz"),
	OPERATOR 4 >("timestamp","timestamptz"),
	OPERATOR 5 <("timestamptz","date"),
	OPERATOR 1 <=("timestamptz","date"),
	OPERATOR 2 =("timestamptz","date"),
	OPERATOR 3 >=("timestamptz","date"),
	OPERATOR 4 >("timestamptz","date"),
	OPERATOR 5 <("timestamptz","timestamp"),
	OPERATOR 1 <=("timestamptz","timestamp"),
	OPERATOR 2 =("timestamptz","timestamp"),
	OPERATOR 3 >=("timestamptz","timestamp"),
	OPERATOR 4 >("timestamptz","timestamp"),
	FUNCTION 1 ("date", "timestamp") "date_cmp_timestamp",
	FUNCTION 1 ("date", "timestamptz") "date_cmp_timestamptz",
	FUNCTION 1 ("timestamp", "date") "timestamp_cmp_date",
	FUNCTION 1 ("timestamp", "timestamptz") "timestamp_cmp_timestamptz",
	FUNCTION 1 ("timestamptz", "date") "timestamptz_cmp_date",
	FUNCTION 1 ("timestamptz", "timestamp") "timestamptz_cmp_timestamp",
	FUNCTION 3 ("date", "interval") "in_range"("date","date","interval","bool","bool"),
	FUNCTION 3 ("timestamp", "interval") "in_range"("timestamp","timestamp","interval","bool","bool"),
	FUNCTION 3 ("timestamptz", "interval") "in_range"("timestamptz","timestamptz","interval","bool","bool");


CREATE OPERATOR FAMILY "float_ops" USING "treeb";

ALTER OPERATOR FAMILY "float_ops" USING "treeb" ADD
	OPERATOR 5 <("float4","float8"),
	OPERATOR 1 <=("float4","float8"),
	OPERATOR 2 =("float4","float8"),
	OPERATOR 3 >=("float4","float8"),
	OPERATOR 4 >("float4","float8"),
	OPERATOR 5 <("float8","float4"),
	OPERATOR 1 <=("float8","float4"),
	OPERATOR 2 =("float8","float4"),
	OPERATOR 3 >=("float8","float4"),
	OPERATOR 4 >("float8","float4"),
	FUNCTION 1 ("float4", "float8") "btfloat48cmp",
	FUNCTION 1 ("float8", "float4") "btfloat84cmp",
	FUNCTION 3 ("float4", "float8") "in_range"("float4","float4","float8","bool","bool");


CREATE OPERATOR FAMILY "network_ops" USING "treeb";

CREATE OPERATOR FAMILY "integer_ops" USING "treeb";

ALTER OPERATOR FAMILY "integer_ops" USING "treeb" ADD
	OPERATOR 5 <("int2","int4"),
	OPERATOR 1 <=("int2","int4"),
	OPERATOR 2 =("int2","int4"),
	OPERATOR 3 >=("int2","int4"),
	OPERATOR 4 >("int2","int4"),
	OPERATOR 5 <("int2","int8"),
	OPERATOR 1 <=("int2","int8"),
	OPERATOR 2 =("int2","int8"),
	OPERATOR 3 >=("int2","int8"),
	OPERATOR 4 >("int2","int8"),
	OPERATOR 5 <("int4","int2"),
	OPERATOR 1 <=("int4","int2"),
	OPERATOR 2 =("int4","int2"),
	OPERATOR 3 >=("int4","int2"),
	OPERATOR 4 >("int4","int2"),
	OPERATOR 5 <("int4","int8"),
	OPERATOR 1 <=("int4","int8"),
	OPERATOR 2 =("int4","int8"),
	OPERATOR 3 >=("int4","int8"),
	OPERATOR 4 >("int4","int8"),
	OPERATOR 5 <("int8","int2"),
	OPERATOR 1 <=("int8","int2"),
	OPERATOR 2 =("int8","int2"),
	OPERATOR 3 >=("int8","int2"),
	OPERATOR 4 >("int8","int2"),
	OPERATOR 5 <("int8","int4"),
	OPERATOR 1 <=("int8","int4"),
	OPERATOR 2 =("int8","int4"),
	OPERATOR 3 >=("int8","int4"),
	OPERATOR 4 >("int8","int4"),
	FUNCTION 1 ("int2", "int4") "btint24cmp",
	FUNCTION 1 ("int2", "int8") "btint28cmp",
	FUNCTION 3 ("int2", "int8") "in_range"("int2","int2","int8","bool","bool"),
	FUNCTION 3 ("int2", "int4") "in_range"("int2","int2","int4","bool","bool"),
	FUNCTION 1 ("int4", "int8") "btint48cmp",
	FUNCTION 1 ("int4", "int2") "btint42cmp",
	FUNCTION 3 ("int4", "int8") "in_range"("int4","int4","int8","bool","bool"),
	FUNCTION 3 ("int4", "int2") "in_range"("int4","int4","int2","bool","bool"),
	FUNCTION 1 ("int8", "int4") "btint84cmp",
	FUNCTION 1 ("int8", "int2") "btint82cmp";


CREATE OPERATOR FAMILY "interval_ops" USING "treeb";

CREATE OPERATOR FAMILY "macaddr_ops" USING "treeb";

CREATE OPERATOR FAMILY "macaddr8_ops" USING "treeb";

CREATE OPERATOR FAMILY "numeric_ops" USING "treeb";

CREATE OPERATOR FAMILY "oid_ops" USING "treeb";

CREATE OPERATOR FAMILY "oidvector_ops" USING "treeb";

CREATE OPERATOR FAMILY "record_ops" USING "treeb";

CREATE OPERATOR FAMILY "record_image_ops" USING "treeb";

CREATE OPERATOR FAMILY "text_ops" USING "treeb";

ALTER OPERATOR FAMILY "text_ops" USING "treeb" ADD
	OPERATOR 5 <("name","text"),
	OPERATOR 1 <=("name","text"),
	OPERATOR 2 =("name","text"),
	OPERATOR 3 >=("name","text"),
	OPERATOR 4 >("name","text"),
	OPERATOR 5 <("text","name"),
	OPERATOR 1 <=("text","name"),
	OPERATOR 2 =("text","name"),
	OPERATOR 3 >=("text","name"),
	OPERATOR 4 >("text","name"),
	FUNCTION 1 ("name", "text") "btnametextcmp",
	FUNCTION 1 ("text", "name") "bttextnamecmp";


CREATE OPERATOR FAMILY "time_ops" USING "treeb";

ALTER OPERATOR FAMILY "time_ops" USING "treeb" ADD
	FUNCTION 3 ("time", "interval") "in_range"("time","time","interval","bool","bool");


CREATE OPERATOR FAMILY "timetz_ops" USING "treeb";

ALTER OPERATOR FAMILY "timetz_ops" USING "treeb" ADD
	FUNCTION 3 ("timetz", "interval") "in_range"("timetz","timetz","interval","bool","bool");


CREATE OPERATOR FAMILY "varbit_ops" USING "treeb";

CREATE OPERATOR FAMILY "text_pattern_ops" USING "treeb";

CREATE OPERATOR FAMILY "bpchar_pattern_ops" USING "treeb";

CREATE OPERATOR FAMILY "money_ops" USING "treeb";

CREATE OPERATOR FAMILY "tid_ops" USING "treeb";

CREATE OPERATOR FAMILY "xid8_ops" USING "treeb";

CREATE OPERATOR FAMILY "uuid_ops" USING "treeb";

CREATE OPERATOR FAMILY "pg_lsn_ops" USING "treeb";

CREATE OPERATOR FAMILY "enum_ops" USING "treeb";

CREATE OPERATOR FAMILY "tsvector_ops" USING "treeb";

CREATE OPERATOR FAMILY "tsquery_ops" USING "treeb";

CREATE OPERATOR FAMILY "range_ops" USING "treeb";

CREATE OPERATOR FAMILY "jsonb_ops" USING "treeb";

CREATE OPERATOR FAMILY "multirange_ops" USING "treeb";

-- Operator classes

CREATE OPERATOR CLASS "array_ops"
	DEFAULT FOR TYPE "anyarray" USING "treeb" FAMILY "array_ops" AS
	OPERATOR 5 <("anyarray","anyarray"),
	OPERATOR 1 <=("anyarray","anyarray"),
	OPERATOR 2 =("anyarray","anyarray"),
	OPERATOR 3 >=("anyarray","anyarray"),
	OPERATOR 4 >("anyarray","anyarray"),
	FUNCTION 1 "btarraycmp";



CREATE OPERATOR CLASS "bit_ops"
	DEFAULT FOR TYPE "bit" USING "treeb" FAMILY "bit_ops" AS
	OPERATOR 5 <("bit","bit"),
	OPERATOR 1 <=("bit","bit"),
	OPERATOR 2 =("bit","bit"),
	OPERATOR 3 >=("bit","bit"),
	OPERATOR 4 >("bit","bit"),
	FUNCTION 1 "bitcmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "bool_ops"
	DEFAULT FOR TYPE "bool" USING "treeb" FAMILY "bool_ops" AS
	OPERATOR 5 <("bool","bool"),
	OPERATOR 1 <=("bool","bool"),
	OPERATOR 2 =("bool","bool"),
	OPERATOR 3 >=("bool","bool"),
	OPERATOR 4 >("bool","bool"),
	FUNCTION 1 "btboolcmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "bpchar_ops"
	DEFAULT FOR TYPE "bpchar" USING "treeb" FAMILY "bpchar_ops" AS
	OPERATOR 5 <("bpchar","bpchar"),
	OPERATOR 1 <=("bpchar","bpchar"),
	OPERATOR 2 =("bpchar","bpchar"),
	OPERATOR 3 >=("bpchar","bpchar"),
	OPERATOR 4 >("bpchar","bpchar"),
	FUNCTION 1 "bpcharcmp",
	FUNCTION 2 "bpchar_sortsupport",
	FUNCTION 4 "btvarstrequalimage";



CREATE OPERATOR CLASS "bytea_ops"
	DEFAULT FOR TYPE "bytea" USING "treeb" FAMILY "bytea_ops" AS
	OPERATOR 5 <("bytea","bytea"),
	OPERATOR 1 <=("bytea","bytea"),
	OPERATOR 2 =("bytea","bytea"),
	OPERATOR 3 >=("bytea","bytea"),
	OPERATOR 4 >("bytea","bytea"),
	FUNCTION 1 "byteacmp",
	FUNCTION 2 "bytea_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "char_ops"
	DEFAULT FOR TYPE "char" USING "treeb" FAMILY "char_ops" AS
	OPERATOR 5 <("char","char"),
	OPERATOR 1 <=("char","char"),
	OPERATOR 2 =("char","char"),
	OPERATOR 3 >=("char","char"),
	OPERATOR 4 >("char","char"),
	FUNCTION 1 "btcharcmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "date_ops"
	DEFAULT FOR TYPE "date" USING "treeb" FAMILY "datetime_ops" AS
	OPERATOR 5 <("date","date"),
	OPERATOR 1 <=("date","date"),
	OPERATOR 2 =("date","date"),
	OPERATOR 3 >=("date","date"),
	OPERATOR 4 >("date","date"),
	FUNCTION 1 "date_cmp",
	FUNCTION 2 "date_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "float4_ops"
	DEFAULT FOR TYPE "float4" USING "treeb" FAMILY "float_ops" AS
	OPERATOR 5 <("float4","float4"),
	OPERATOR 1 <=("float4","float4"),
	OPERATOR 2 =("float4","float4"),
	OPERATOR 3 >=("float4","float4"),
	OPERATOR 4 >("float4","float4"),
	FUNCTION 1 "btfloat4cmp",
	FUNCTION 2 "btfloat4sortsupport";



CREATE OPERATOR CLASS "float8_ops"
	DEFAULT FOR TYPE "float8" USING "treeb" FAMILY "float_ops" AS
	OPERATOR 5 <("float8","float8"),
	OPERATOR 1 <=("float8","float8"),
	OPERATOR 2 =("float8","float8"),
	OPERATOR 3 >=("float8","float8"),
	OPERATOR 4 >("float8","float8"),
	FUNCTION 1 "btfloat8cmp",
	FUNCTION 2 "btfloat8sortsupport",
	FUNCTION 3 "in_range"("float8","float8","float8","bool","bool");



CREATE OPERATOR CLASS "inet_ops"
	DEFAULT FOR TYPE "inet" USING "treeb" FAMILY "network_ops" AS
	OPERATOR 5 <("inet","inet"),
	OPERATOR 1 <=("inet","inet"),
	OPERATOR 2 =("inet","inet"),
	OPERATOR 3 >=("inet","inet"),
	OPERATOR 4 >("inet","inet"),
	FUNCTION 1 "network_cmp",
	FUNCTION 2 "network_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "int2_ops"
	DEFAULT FOR TYPE "int2" USING "treeb" FAMILY "integer_ops" AS
	OPERATOR 5 <("int2","int2"),
	OPERATOR 1 <=("int2","int2"),
	OPERATOR 2 =("int2","int2"),
	OPERATOR 3 >=("int2","int2"),
	OPERATOR 4 >("int2","int2"),
	FUNCTION 1 "btint2cmp",
	FUNCTION 2 "btint2sortsupport",
	FUNCTION 4 "btequalimage",
	FUNCTION 3 "in_range"("int2","int2","int2","bool","bool");



CREATE OPERATOR CLASS "int4_ops"
	DEFAULT FOR TYPE "int4" USING "treeb" FAMILY "integer_ops" AS
	OPERATOR 5 <("int4","int4"),
	OPERATOR 1 <=("int4","int4"),
	OPERATOR 2 =("int4","int4"),
	OPERATOR 3 >=("int4","int4"),
	OPERATOR 4 >("int4","int4"),
	FUNCTION 1 "btint4cmp",
	FUNCTION 2 "btint4sortsupport",
	FUNCTION 4 "btequalimage",
	FUNCTION 3 "in_range"("int4","int4","int4","bool","bool");



CREATE OPERATOR CLASS "int8_ops"
	DEFAULT FOR TYPE "int8" USING "treeb" FAMILY "integer_ops" AS
	OPERATOR 5 <("int8","int8"),
	OPERATOR 1 <=("int8","int8"),
	OPERATOR 2 =("int8","int8"),
	OPERATOR 3 >=("int8","int8"),
	OPERATOR 4 >("int8","int8"),
	FUNCTION 1 "btint8cmp",
	FUNCTION 2 "btint8sortsupport",
	FUNCTION 4 "btequalimage",
	FUNCTION 3 "in_range"("int8","int8","int8","bool","bool");



CREATE OPERATOR CLASS "interval_ops"
	DEFAULT FOR TYPE "interval" USING "treeb" FAMILY "interval_ops" AS
	OPERATOR 5 <("interval","interval"),
	OPERATOR 1 <=("interval","interval"),
	OPERATOR 2 =("interval","interval"),
	OPERATOR 3 >=("interval","interval"),
	OPERATOR 4 >("interval","interval"),
	FUNCTION 1 "interval_cmp",
	FUNCTION 3 "in_range"("interval","interval","interval","bool","bool");



CREATE OPERATOR CLASS "macaddr_ops"
	DEFAULT FOR TYPE "macaddr" USING "treeb" FAMILY "macaddr_ops" AS
	OPERATOR 5 <("macaddr","macaddr"),
	OPERATOR 1 <=("macaddr","macaddr"),
	OPERATOR 2 =("macaddr","macaddr"),
	OPERATOR 3 >=("macaddr","macaddr"),
	OPERATOR 4 >("macaddr","macaddr"),
	FUNCTION 1 "macaddr_cmp",
	FUNCTION 2 "macaddr_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "macaddr8_ops"
	DEFAULT FOR TYPE "macaddr8" USING "treeb" FAMILY "macaddr8_ops" AS
	OPERATOR 5 <("macaddr8","macaddr8"),
	OPERATOR 1 <=("macaddr8","macaddr8"),
	OPERATOR 2 =("macaddr8","macaddr8"),
	OPERATOR 3 >=("macaddr8","macaddr8"),
	OPERATOR 4 >("macaddr8","macaddr8"),
	FUNCTION 1 "macaddr8_cmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "name_ops"
	DEFAULT FOR TYPE "name" USING "treeb" FAMILY "text_ops" AS
	OPERATOR 5 <("name","name"),
	OPERATOR 1 <=("name","name"),
	OPERATOR 2 =("name","name"),
	OPERATOR 3 >=("name","name"),
	OPERATOR 4 >("name","name"),
	FUNCTION 1 "btnamecmp",
	FUNCTION 2 "btnamesortsupport",
	FUNCTION 4 "btvarstrequalimage";



CREATE OPERATOR CLASS "numeric_ops"
	DEFAULT FOR TYPE "numeric" USING "treeb" FAMILY "numeric_ops" AS
	OPERATOR 5 <("numeric","numeric"),
	OPERATOR 1 <=("numeric","numeric"),
	OPERATOR 2 =("numeric","numeric"),
	OPERATOR 3 >=("numeric","numeric"),
	OPERATOR 4 >("numeric","numeric"),
	FUNCTION 1 "numeric_cmp",
	FUNCTION 2 "numeric_sortsupport",
	FUNCTION 3 "in_range"("numeric","numeric","numeric","bool","bool");



CREATE OPERATOR CLASS "oid_ops"
	DEFAULT FOR TYPE "oid" USING "treeb" FAMILY "oid_ops" AS
	OPERATOR 5 <("oid","oid"),
	OPERATOR 1 <=("oid","oid"),
	OPERATOR 2 =("oid","oid"),
	OPERATOR 3 >=("oid","oid"),
	OPERATOR 4 >("oid","oid"),
	FUNCTION 1 "btoidcmp",
	FUNCTION 2 "btoidsortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "oidvector_ops"
	DEFAULT FOR TYPE "oidvector" USING "treeb" FAMILY "oidvector_ops" AS
	OPERATOR 5 <("oidvector","oidvector"),
	OPERATOR 1 <=("oidvector","oidvector"),
	OPERATOR 2 =("oidvector","oidvector"),
	OPERATOR 3 >=("oidvector","oidvector"),
	OPERATOR 4 >("oidvector","oidvector"),
	FUNCTION 1 "btoidvectorcmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "record_ops"
	DEFAULT FOR TYPE "record" USING "treeb" FAMILY "record_ops" AS
	OPERATOR 5 <("record","record"),
	OPERATOR 1 <=("record","record"),
	OPERATOR 2 =("record","record"),
	OPERATOR 3 >=("record","record"),
	OPERATOR 4 >("record","record"),
	FUNCTION 1 "btrecordcmp";



CREATE OPERATOR CLASS "record_image_ops"
	FOR TYPE "record" USING "treeb" FAMILY "record_image_ops" AS
	OPERATOR 5 *<("record","record"),
	OPERATOR 1 *<=("record","record"),
	OPERATOR 2 *=("record","record"),
	OPERATOR 3 *>=("record","record"),
	OPERATOR 4 *>("record","record"),
	FUNCTION 1 "btrecordimagecmp";



CREATE OPERATOR CLASS "text_ops"
	DEFAULT FOR TYPE "text" USING "treeb" FAMILY "text_ops" AS
	OPERATOR 5 <("text","text"),
	OPERATOR 1 <=("text","text"),
	OPERATOR 2 =("text","text"),
	OPERATOR 3 >=("text","text"),
	OPERATOR 4 >("text","text"),
	FUNCTION 1 "bttextcmp",
	FUNCTION 2 "bttextsortsupport",
	FUNCTION 4 "btvarstrequalimage";



CREATE OPERATOR CLASS "time_ops"
	DEFAULT FOR TYPE "time" USING "treeb" FAMILY "time_ops" AS
	OPERATOR 5 <("time","time"),
	OPERATOR 1 <=("time","time"),
	OPERATOR 2 =("time","time"),
	OPERATOR 3 >=("time","time"),
	OPERATOR 4 >("time","time"),
	FUNCTION 1 "time_cmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "timestamptz_ops"
	DEFAULT FOR TYPE "timestamptz" USING "treeb" FAMILY "datetime_ops" AS
	OPERATOR 5 <("timestamptz","timestamptz"),
	OPERATOR 1 <=("timestamptz","timestamptz"),
	OPERATOR 2 =("timestamptz","timestamptz"),
	OPERATOR 3 >=("timestamptz","timestamptz"),
	OPERATOR 4 >("timestamptz","timestamptz"),
	FUNCTION 1 "timestamptz_cmp",
	FUNCTION 2 "timestamp_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "timetz_ops"
	DEFAULT FOR TYPE "timetz" USING "treeb" FAMILY "timetz_ops" AS
	OPERATOR 5 <("timetz","timetz"),
	OPERATOR 1 <=("timetz","timetz"),
	OPERATOR 2 =("timetz","timetz"),
	OPERATOR 3 >=("timetz","timetz"),
	OPERATOR 4 >("timetz","timetz"),
	FUNCTION 1 "timetz_cmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "varbit_ops"
	DEFAULT FOR TYPE "varbit" USING "treeb" FAMILY "varbit_ops" AS
	OPERATOR 5 <("varbit","varbit"),
	OPERATOR 1 <=("varbit","varbit"),
	OPERATOR 2 =("varbit","varbit"),
	OPERATOR 3 >=("varbit","varbit"),
	OPERATOR 4 >("varbit","varbit"),
	FUNCTION 1 "varbitcmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "timestamp_ops"
	DEFAULT FOR TYPE "timestamp" USING "treeb" FAMILY "datetime_ops" AS
	OPERATOR 5 <("timestamp","timestamp"),
	OPERATOR 1 <=("timestamp","timestamp"),
	OPERATOR 2 =("timestamp","timestamp"),
	OPERATOR 3 >=("timestamp","timestamp"),
	OPERATOR 4 >("timestamp","timestamp"),
	FUNCTION 1 "timestamp_cmp",
	FUNCTION 2 "timestamp_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "text_pattern_ops"
	FOR TYPE "text" USING "treeb" FAMILY "text_pattern_ops" AS
	OPERATOR 5 ~<~("text","text"),
	OPERATOR 1 ~<=~("text","text"),
	OPERATOR 2 =("text","text"),
	OPERATOR 3 ~>=~("text","text"),
	OPERATOR 4 ~>~("text","text"),
	FUNCTION 1 "bttext_pattern_cmp",
	FUNCTION 2 "bttext_pattern_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "bpchar_pattern_ops"
	FOR TYPE "bpchar" USING "treeb" FAMILY "bpchar_pattern_ops" AS
	OPERATOR 5 ~<~("bpchar","bpchar"),
	OPERATOR 1 ~<=~("bpchar","bpchar"),
	OPERATOR 2 =("bpchar","bpchar"),
	OPERATOR 3 ~>=~("bpchar","bpchar"),
	OPERATOR 4 ~>~("bpchar","bpchar"),
	FUNCTION 1 "btbpchar_pattern_cmp",
	FUNCTION 2 "btbpchar_pattern_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "money_ops"
	DEFAULT FOR TYPE "money" USING "treeb" FAMILY "money_ops" AS
	OPERATOR 5 <("money","money"),
	OPERATOR 1 <=("money","money"),
	OPERATOR 2 =("money","money"),
	OPERATOR 3 >=("money","money"),
	OPERATOR 4 >("money","money"),
	FUNCTION 1 "cash_cmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "tid_ops"
	DEFAULT FOR TYPE "tid" USING "treeb" FAMILY "tid_ops" AS
	OPERATOR 5 <("tid","tid"),
	OPERATOR 1 <=("tid","tid"),
	OPERATOR 2 =("tid","tid"),
	OPERATOR 3 >=("tid","tid"),
	OPERATOR 4 >("tid","tid"),
	FUNCTION 1 "bttidcmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "xid8_ops"
	DEFAULT FOR TYPE "xid8" USING "treeb" FAMILY "xid8_ops" AS
	OPERATOR 5 <("xid8","xid8"),
	OPERATOR 1 <=("xid8","xid8"),
	OPERATOR 2 =("xid8","xid8"),
	OPERATOR 3 >=("xid8","xid8"),
	OPERATOR 4 >("xid8","xid8"),
	FUNCTION 1 "xid8cmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "uuid_ops"
	DEFAULT FOR TYPE "uuid" USING "treeb" FAMILY "uuid_ops" AS
	OPERATOR 5 <("uuid","uuid"),
	OPERATOR 1 <=("uuid","uuid"),
	OPERATOR 2 =("uuid","uuid"),
	OPERATOR 3 >=("uuid","uuid"),
	OPERATOR 4 >("uuid","uuid"),
	FUNCTION 1 "uuid_cmp",
	FUNCTION 2 "uuid_sortsupport",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "pg_lsn_ops"
	DEFAULT FOR TYPE "pg_lsn" USING "treeb" FAMILY "pg_lsn_ops" AS
	OPERATOR 5 <("pg_lsn","pg_lsn"),
	OPERATOR 1 <=("pg_lsn","pg_lsn"),
	OPERATOR 2 =("pg_lsn","pg_lsn"),
	OPERATOR 3 >=("pg_lsn","pg_lsn"),
	OPERATOR 4 >("pg_lsn","pg_lsn"),
	FUNCTION 1 "pg_lsn_cmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "enum_ops"
	DEFAULT FOR TYPE "anyenum" USING "treeb" FAMILY "enum_ops" AS
	OPERATOR 5 <("anyenum","anyenum"),
	OPERATOR 1 <=("anyenum","anyenum"),
	OPERATOR 2 =("anyenum","anyenum"),
	OPERATOR 3 >=("anyenum","anyenum"),
	OPERATOR 4 >("anyenum","anyenum"),
	FUNCTION 1 "enum_cmp",
	FUNCTION 4 "btequalimage";



CREATE OPERATOR CLASS "tsvector_ops"
	DEFAULT FOR TYPE "tsvector" USING "treeb" FAMILY "tsvector_ops" AS
	OPERATOR 5 <("tsvector","tsvector"),
	OPERATOR 1 <=("tsvector","tsvector"),
	OPERATOR 2 =("tsvector","tsvector"),
	OPERATOR 3 >=("tsvector","tsvector"),
	OPERATOR 4 >("tsvector","tsvector"),
	FUNCTION 1 "tsvector_cmp";



CREATE OPERATOR CLASS "tsquery_ops"
	DEFAULT FOR TYPE "tsquery" USING "treeb" FAMILY "tsquery_ops" AS
	OPERATOR 5 <("tsquery","tsquery"),
	OPERATOR 1 <=("tsquery","tsquery"),
	OPERATOR 2 =("tsquery","tsquery"),
	OPERATOR 3 >=("tsquery","tsquery"),
	OPERATOR 4 >("tsquery","tsquery"),
	FUNCTION 1 "tsquery_cmp";



CREATE OPERATOR CLASS "range_ops"
	DEFAULT FOR TYPE "anyrange" USING "treeb" FAMILY "range_ops" AS
	OPERATOR 5 <("anyrange","anyrange"),
	OPERATOR 1 <=("anyrange","anyrange"),
	OPERATOR 2 =("anyrange","anyrange"),
	OPERATOR 3 >=("anyrange","anyrange"),
	OPERATOR 4 >("anyrange","anyrange"),
	FUNCTION 1 "range_cmp";



CREATE OPERATOR CLASS "multirange_ops"
	DEFAULT FOR TYPE "anymultirange" USING "treeb" FAMILY "multirange_ops" AS
	OPERATOR 5 <("anymultirange","anymultirange"),
	OPERATOR 1 <=("anymultirange","anymultirange"),
	OPERATOR 2 =("anymultirange","anymultirange"),
	OPERATOR 3 >=("anymultirange","anymultirange"),
	OPERATOR 4 >("anymultirange","anymultirange"),
	FUNCTION 1 "multirange_cmp";



CREATE OPERATOR CLASS "jsonb_ops"
	DEFAULT FOR TYPE "jsonb" USING "treeb" FAMILY "jsonb_ops" AS
	OPERATOR 5 <("jsonb","jsonb"),
	OPERATOR 1 <=("jsonb","jsonb"),
	OPERATOR 2 =("jsonb","jsonb"),
	OPERATOR 3 >=("jsonb","jsonb"),
	OPERATOR 4 >("jsonb","jsonb"),
	FUNCTION 1 "jsonb_cmp";
