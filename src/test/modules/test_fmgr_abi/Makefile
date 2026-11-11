# src/test/modules/test_fmgr_abi/Makefile

MODULE_big = test_fmgr_abi
OBJS = $(WIN32RES) test_fmgr_abi.o
PGFILEDESC = "test_fmgr_abi - test loadable module ABI checks"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_fmgr_abi
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
