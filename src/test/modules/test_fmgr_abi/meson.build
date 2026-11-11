# Copyright (c) 2026, PostgreSQL Global Development Group

test_fmgr_abi = shared_module('test_fmgr_abi',
  ['test_fmgr_abi.c'],
  kwargs: pg_test_mod_args,
)
test_install_libs += test_fmgr_abi

tests += {
  'name': 'test_fmgr_abi',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': ['t/001_test_fmgr_abi.pl'],
  },
}
