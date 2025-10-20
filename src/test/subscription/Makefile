#-------------------------------------------------------------------------
#
# Makefile for src/test/subscription
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/subscription/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/subscription
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

EXTRA_INSTALL = contrib/hstore

export with_icu

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

# Run OrioleDB-specific subscription tests
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
		PG_TEST_INITDB_EXTRA_OPTS="--locale=C" \
		PROVE_TESTS="$$ORIOLE_TESTS"

clean distclean:
	rm -rf tmp_check
