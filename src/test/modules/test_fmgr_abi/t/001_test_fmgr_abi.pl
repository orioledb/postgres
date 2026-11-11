# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->command_checks_all(
	[ 'psql', '-d', $node->connstr('postgres'), '-c', "LOAD 'test_fmgr_abi'" ],
	1,
	[qr/^$/],
	[qr/ERROR:  incompatible library .*: ABI mismatch.*Server has ABI "OrioleDB PostgreSQL", library has "PostgreSQL"/s],
	'PostgreSQL module is rejected cleanly');

done_testing();
