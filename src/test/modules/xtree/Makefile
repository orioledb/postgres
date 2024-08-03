# xtree/Makefile

REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

MODULE_big = xtree
OBJS = $(WIN32RES) access/xtree.o module.o

EXTENSION = xtree
DATA = xtree--1.0.sql
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

# The xtree module does not supply its own regression tests.  Instead, it copies
# regression tests from elsewhere in the project and changes hash indexes
# contained within those tests to xtree instead.  The tests to use are parsed
# from parallel_schedule and isolation_schedule, as below.
CLONE_SCRIPT=../../../tools/clone_tests.pl
REGRESS_BY_REFERENCE=$(shell $(PERL) $(CLONE_SCRIPT) --schedule=../../regress/parallel_schedule)
ISOLATION_BY_REFERENCE=$(shell $(PERL) $(CLONE_SCRIPT) --schedule=../../isolation/isolation_schedule)

REGRESS_OPTS += --load-extension=xtree
REGRESS_OPTS += --temp-config $(srcdir)/xtree.conf
REGRESS = $(REGRESS_BY_REFERENCE)

ISOLATION_OPTS = --load-extension=xtree
ISOLATION_OPTS += --inputdir=isolation
ISOLATION_OPTS += --temp-config $(srcdir)/xtree.conf
ISOLATION = $(ISOLATION_BY_REFERENCE)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/xtree
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

$(OBJS): tests_by_reference

.PHONY: tests_by_reference
tests_by_reference:
	$(PERL) $(CLONE_SCRIPT) \
		--regress="$(REGRESS_BY_REFERENCE)" \
		--isolation="$(ISOLATION_BY_REFERENCE)" \
		--src_idx_am=btree \
		--dst_idx_am=xtree \
		--src_sql=../../regress/sql \
		--src_expected=../../regress/expected \
		--dst_sql=./sql \
		--dst_expected=./expected \
		--src_spec=../../isolation/specs \
		--src_iso_expected=../../isolation/expected \
		--dst_spec=./isolation/specs \
		--dst_iso_expected=./isolation/expected \
		--datadir_prefix="/../../regress"
