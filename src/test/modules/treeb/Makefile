# treeb/Makefile

REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

MODULE_big = treeb
OBJS = $(WIN32RES) access/treeb.o module.o
OBJS = \
	$(WIN32RES) \
	access/treeb_indexam.o \
	access/treebdesc.o \
	access/treebutils.o \
	access/treebsort.o \
	access/treebsplitloc.o \
	access/treebvalidate.o \
	access/treebpage.o \
	access/treebcompare.o \
	access/treeb.o \
	access/treebinsert.o \
	access/treebdedup.o \
	access/treebsearch.o \
	access/treebxlog.o \
	module.o

EXTENSION = treeb
DATA = treeb--1.0.sql
PGFILEDESC = "test copy of btree"

LDFLAGS_SL += $(filter -lm, $(LIBS))

# If the caller supplied a pklibdir, use that.  The build system will supply
# this, and perhaps also folks making installcheck from pgxs, and maybe others.
#
ifdef pkglibdir
REGRESS_OPTS = --dlpath=$(pkglibdir)
else
REGRESS_OPTS = --dlpath=$(top_builddir)/src/test/regress
endif

# The treeb module does not supply its own regression tests.  Instead, it copies
# regression tests from elsewhere in the project and changes hash indexes
# contained within those tests to treeb instead.  The tests to use are parsed
# from parallel_schedule and isolation_schedule, as below.
CLONE_SCRIPT=../../../tools/clone_tests.pl
REGRESS_BY_REFERENCE=$(shell $(PERL) $(CLONE_SCRIPT) --schedule=../../regress/parallel_schedule)
ISOLATION_BY_REFERENCE=$(shell $(PERL) $(CLONE_SCRIPT) --schedule=../../isolation/isolation_schedule)

REGRESS_OPTS += --load-extension=treeb
REGRESS_OPTS += --temp-config $(srcdir)/treeb.conf
REGRESS = $(REGRESS_BY_REFERENCE)

ISOLATION_OPTS = --load-extension=treeb
ISOLATION_OPTS += --inputdir=isolation
ISOLATION_OPTS += --temp-config $(srcdir)/treeb.conf
ISOLATION = $(ISOLATION_BY_REFERENCE)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/treeb
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

$(OBJS): access_dir tests_by_reference

.PHONY: access_dir
access_dir:
	mkdir -p access

.PHONY: tests_by_reference
tests_by_reference:
	$(PERL) $(CLONE_SCRIPT) \
		--regress="$(REGRESS_BY_REFERENCE)" \
		--isolation="$(ISOLATION_BY_REFERENCE)" \
		--src_idx_am=btree \
		--dst_idx_am=treeb \
		--src_sql=../../regress/sql \
		--src_expected=../../regress/expected \
		--dst_sql=./sql \
		--dst_expected=./expected \
		--src_spec=../../isolation/specs \
		--src_iso_expected=../../isolation/expected \
		--dst_spec=./isolation/specs \
		--dst_iso_expected=./isolation/expected \
		--datadir_prefix="/../../regress"
