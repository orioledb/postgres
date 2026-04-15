#-------------------------------------------------------------------------
#
# Makefile for src/test/recovery
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/recovery/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL=contrib/pg_prewarm \
	contrib/pg_stat_statements \
	contrib/test_decoding \
	src/test/modules/injection_points

subdir = src/test/recovery
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export enable_injection_points

# required for 017_shm.pl and 027_stream_regress.pl
REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

# Run OrioleDB-specific recovery tests
# Tests to run are listed in oriole_tests.txt
installcheck-oriole:
	@if [ ! -f oriole_tests.txt ]; then \
		echo "Error: oriole_tests.txt not found"; \
		echo "Create oriole_tests.txt with a list of test files to run"; \
		exit 1; \
	fi
	@if [ ! -f orioledb.conf ]; then \
		echo "Error: orioledb.conf not found"; \
		exit 1; \
	fi
	@ORIOLE_TESTS=$$(grep -v '^#' oriole_tests.txt | grep -v '^$$' | tr '\n' ' '); \
	if [ -z "$$ORIOLE_TESTS" ]; then \
		echo "No tests found in oriole_tests.txt"; \
		exit 1; \
	fi; \
	$(MAKE) installcheck \
		TEMP_CONFIG="$(CURDIR)/orioledb.conf" \
		PG_TEST_INITDB_EXTRA_OPTS="--locale=C  --encoding=UTF8" \
		EXTRA_REGRESS_OPTS="--load-extension=orioledb" \
		EXTRA_SHARED_PRELOAD_LIBS="orioledb" \
		PROVE_TESTS="$$ORIOLE_TESTS"

clean distclean:
	rm -rf tmp_check
