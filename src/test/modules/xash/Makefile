# xash/Makefile

REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

MODULE_big = xash
OBJS = $(WIN32RES) access/xash.o module.o

EXTENSION = xash
DATA = xash--1.0.sql
PGFILEDESC = "test copy of hash"

LDFLAGS_SL += $(filter -lm, $(LIBS))

# If the caller supplied a pklibdir, use that.  The build system will supply
# this, and perhaps also folks making installcheck from pgxs, and maybe others.
#
ifdef pkglibdir
REGRESS_OPTS = --dlpath=$(pkglibdir)
else
REGRESS_OPTS = --dlpath=$(top_builddir)/src/test/regress
endif

# The xash module does not supply its own regression tests.  Instead, it copies
# regression tests from elsewhere in the project and changes hash indexes
# contained within those tests to xash instead.  The tests to use are parsed
# from parallel_schedule and isolation_schedule, as below.
CLONE_SCRIPT=../../../tools/clone_tests.pl
REGRESS_BY_REFERENCE=$(shell $(PERL) $(CLONE_SCRIPT) --schedule=../../regress/parallel_schedule)
ISOLATION_BY_REFERENCE=$(shell $(PERL) $(CLONE_SCRIPT) --schedule=../../isolation/isolation_schedule)

REGRESS_OPTS += --load-extension=xash
REGRESS_OPTS += --temp-config $(srcdir)/xash.conf
REGRESS = $(REGRESS_BY_REFERENCE)

ISOLATION_OPTS = --load-extension=xash
ISOLATION_OPTS += --inputdir=isolation
ISOLATION_OPTS += --temp-config $(srcdir)/xash.conf
ISOLATION = $(ISOLATION_BY_REFERENCE)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/xash
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
		--src_idx_am=hash \
		--dst_idx_am=xash \
		--src_sql=../../regress/sql \
		--src_expected=../../regress/expected \
		--dst_sql=./sql \
		--dst_expected=./expected \
		--src_spec=../../isolation/specs \
		--src_iso_expected=../../isolation/expected \
		--dst_spec=./isolation/specs \
		--dst_iso_expected=./isolation/expected \
		--datadir_prefix="/../../regress"
