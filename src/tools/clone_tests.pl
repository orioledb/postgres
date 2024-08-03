# Make modified copies of regression tests from elsewhere in the source tree,
# for use in testing a loadable module.  This copying mechanism saves us the
# bloat of committing the same regression tests in the modules as elsewhere.
#
# Copyright (c) 2000-2024, PostgreSQL Global Development Group

=pod

=head1 NAME

clone_tests.pl - script for copying regression and isolation test files from
elsewhere in the postgresql source tree to a module, modifying the test to
exercise one or more table or index AMs provided by the module.

=head1 SYNOPSIS

  copy_regression_test.pl \
    --src_idx_am=btree \
    --dst_idx_am=xtree \
	--schedule=../../regress/parallel_schedule
	--regress=../../regress/sql
	--datadir_prefix=/../../regress

  copy_regression_test.pl --schedule=../../regress/parallel_schedule

=head1 OPTIONS

=over

=item --src_tbl_am

=item --dst_tbl_am

Optional argument pairs.  Multiple pairs may be specified.

The name of a table AM used (or potentially used) in the original test for
which we'd like to use a different table AM in the modified test.  For each
src_tbl_am specified, there should be a dst_tbl_am also given, with the order
of the arguments determining which source maps to which destination.

=item --src_idx_am

=item --dst_idx_am

Like src_tbl_am and dst_tbl_am, but for index AMs.

=item --schedule

A schedule file listing all regression or isolation tests.  If given, a space
separated list of all tests, in order, will be printed on STDOUT.

=item --regress

=item --isolation

A space separated list of regression or isolation tests to be processed, such
as the list returned by this script when handed a regression tests schedule
file via the C<--schedule> option.

=item --src_sql

=item --src_expected

The directory path to read the regression tests' .sql and .out files.

=item --src_spec

=item --src_iso_expected

The directory path to read the isolation tests' .sql and .out files.

	"dst_sql=s"          => \$dst_sql,
	"dst_iso_expected=s" => \$dst_iso_expected,

=item --dst_sql

=item --dst_expected

The directory path to write the regression tests' modified .sql and .out files.

=item --dst_spec

=item --dst_iso_expected

The directory path to write the isolation tests' modified .spec and .out files.

=item --datadir_prefix

A prefix to be prepended to paths of data files within the tests being
translated.  For instace, to convert tests from C<src/test/regress/sql> that
contain a snippet like:

=over 4

\set filename :abs_srcdir '/data/hash.data'

=back

into a line in the resulting translated test like:

=over 4

\set filename :abs_srcdir '/../../regress/data/hash.data'

=back

provide the argument C<--datadir_prefix="/../../regress">.

=back

=cut

use strict;
# use warnings FATAL => 'all';
use IO::File;
use Getopt::Long;
use FindBin;
use lib "$FindBin::RealBin/../perl/";

sub push_path
{
	my ($aryref, $option_name, $path) = @_;
	die "$option_name: No such file: $path" unless -f $path;
	push (@$aryref, $path);
}

sub require_equal_length_arrays
{
	my ($ary1, $ary2, $name) = @_;
	die "different number of src_$name and dst_$name arguments given"
		unless(scalar(@$ary1) == scalar(@$ary2));
}



my (@src_tbl_am,
	@src_idx_am,
	@src_regress,
	@src_expected,
	@src_spec,
	@src_output_iso,
	@dst_tbl_am,
	@dst_idx_am,
	@dst_regress,
	@dst_expected,
	@dst_spec,
	@dst_output_iso,
	@schedule,
	$regress,
	$src_sql,
	$src_expected,
	$dst_sql,
	$dst_expected,
	$isolation,
	$src_spec,
	$src_iso_expected,
	$dst_spec,
	$dst_iso_expected,
	$datadir_prefix);
GetOptions(
	"src_tbl_am=s"       => \@src_tbl_am,
	"src_idx_am=s"       => \@src_idx_am,
	"dst_tbl_am=s"       => \@dst_tbl_am,
	"dst_idx_am=s"       => \@dst_idx_am,
	"schedule=s"         => \@schedule,
	"regress=s"          => \$regress,
	"src_sql=s"          => \$src_sql,
	"src_expected=s"     => \$src_expected,
	"dst_sql=s"          => \$dst_sql,
	"dst_expected=s"     => \$dst_expected,
	"isolation=s"        => \$isolation,
	"src_spec=s"         => \$src_spec,
	"src_iso_expected=s" => \$src_iso_expected,
	"dst_spec=s"         => \$dst_spec,
	"dst_iso_expected=s" => \$dst_iso_expected,
	"datadir_prefix=s"   => \$datadir_prefix,
	);

my @regress = grep /\w+/, split(/\s+/, $regress);
foreach my $regress (@regress)
{
	push (@src_regress, "$src_sql/$regress.sql");
	push (@dst_regress, "$dst_sql/$regress.sql");
	push (@src_expected, "$src_expected/$regress.out");
	push (@dst_expected, "$dst_expected/$regress.out");
}
my @isolation = grep /\w+/, split(/\s+/, $isolation);
foreach my $isolation (@isolation)
{
	push (@src_spec, "$src_spec/$isolation.spec");
	push (@dst_spec, "$dst_spec/$isolation.spec");
	push (@src_output_iso, "$src_iso_expected/$isolation.out");
	push (@dst_output_iso, "$dst_iso_expected/$isolation.out");
}
require_equal_length_arrays(\@src_tbl_am, \@dst_tbl_am, "tbl_am");
require_equal_length_arrays(\@src_idx_am, \@dst_idx_am, "idx_am");

# First, define some regular expressions that make copying and modifying random
# code more robust.
#

# Compiled regular expression for matching a single character (or escaped char)
# within a quote except one that ends the quote
our $quotecharre;
$quotecharre = qr{
					(?:
						''				# double single-quote, does not close
						|
						(?> [^'])		# Non-closing singlequote, without backtracking
						|
						(?>\\')			# backslash escaped single-quote, without backtracking
					)
				}msx;

# Compiled regular expression for matching a SQL quoted string
our $quotere;
$quotere = qr{
					(?:
						'				# Begin quote
						$quotecharre*	# Any number of characters within the quote
						'				# End quote
					)
				}msx;



# Compiled regular expression for matching a code snippet that is balanced
# relative to (), [], {} pairings, while not being fooled by unbalanced
# snippets if they are within a single-quoted string.
our $balancedre;
$balancedre = qr{(
					(?:
						# A quote, which might include unbalanced
						# parens/brackets/braces
						(?> $quotere )
						|
						# non-special characters, no backtracking
						(?> [^(){}\[\]]+ )
						|
						\(					# begin balanced parens
							(?-1)			#   ... recurse
						\)					# end balanced parens
						|
						\[					# begin balanced brackets
							(?-1)			#   ... recurse
						\]					# end balanced brackets
						|
						\{					# begin balanced braces
							(?-1)			#   ... recurse
						\}					# end balanced braces
					)*
				)}xms;

our $unqualifiednamere = qr/\b\w+\b/;
our $qualifiednamere = qr/\b\w+(?:\.\w+\b)?/;

sub convert_indexes_to_use_am
{
	my ($code, $srcam, $dstam) = @_;

	# If the srcam is btree, then we need to modify CREATE INDEX statments that
	# say "USING btree" but also ones that omit a USING clause entirely.  For
	# all other srcam, we look for an explicit "USING srcam".
	my $using_srcam_re;
	if (lc($srcam) eq 'btree')
	{
		$using_srcam_re = qr{
								# Optionally, an explicit USING clause
								(?: USING \s+ $srcam )?
							}msix;
	}
	else
	{
		$using_srcam_re = qr{
								# Mandatory explicit USING clause
								(?: USING \s+ $srcam )
								# No alternative
							}msix;
	}

	$code =~ s{ (								# Begin capture of \$1
				CREATE
				\s+
				(?: UNIQUE \s+ )?
				INDEX
				\s+
				(?: CONCURRENTLY \s+ )?
				(?:
					(?!CONCURRENTLY)			# Don't match "concurrently" as a name
					$unqualifiednamere			# Index name is optional
					\s+
				)?
				ON
				\s+
				$qualifiednamere
				)								# End capture of \$1
				\s*
				$using_srcam_re
				(								# Begin capture of \$2
				\s*
				\(
				)								# End capture of \$2
			  }
			  {$1 USING $dstam $2}msigx;

	return $code;
}

sub convert_tables_to_use_am
{
	my ($code, $srcam, $dstam) = @_;

	# If the srcam is heap, then we need to modify CREATE TABLE statments that
	# say "USING heap" but also ones that omit a USING clause entirely.  For
	# all other srcam, we look for an explicit "USING srcam".
	my $using_srcam_re;
	if (lc($srcam) eq 'heap')
	{
		$using_srcam_re = qr{
								# Optionally, an explicit USING clause
								(?: USING \s+ $srcam )?
							}msix;
	}
	else
	{
		$using_srcam_re = qr{
								# Mandatory explicit USING clause
								(?: USING \s+ $srcam )
								# No alternative
							}msix;
	}

	# We don't need to capture the entire CREATE TABLE statement.  We just need
	# the prefix up to where the optional USING clause belongs, so that we can
	# add or replace with our own USING $dstam clause.
	#
	# We're not worried about whether the CREATE TABLE statement that we
	# capture is syntactically correct, so long as we don't break valid syntax
	# with our replacement.  If the test we modify has an intentionally invalid
	# syntax, we still want to insert our "USING $dstam" clause, so long as we
	# can reasonably determine where to do so.
	$code =~ s{ (								# Begin capture of \$1
				CREATE
				\s+
				(?: (?: TEMPORARY | TEMP ) \s+ )?
				(?: UNLOGGED  \s+ )?
				TABLE
				\s+
				(?: IF \s+ NOT \s+ EXISTS \s+ )?
				(?! IF )		# Don't match "if" as the qualified name
				$qualifiednamere \s*
				(?: OF \s+ $qualifiednamere \s* )?
				(?: PARTITION \s+ OF \s+ $qualifiednamere \s* )?
				\( $balancedre \) \s*
				(?: FOR \s+ VALUES \s+ (?:IN|FROM|TO|WITH) \s* \( $balancedre \) \s* )?
				(?: INHERITS \s* \( $balancedre \) \s* )?
				(?: PARTITION \s+ BY \s+ (?:RANGE|LIST|HASH) \s+ \( $balancedre \) \s* )?
				)								# End capture of \$1
				$using_srcam_re
			  }
			  {$1 USING $dstam }msigx;

	return $code;
}

sub convert_expected_output
{
	my ($code, $srcam, $dstam) = @_;
	$srcam = lc($srcam);
	$dstam = lc($dstam);

	# Convert anticipated error which includes the index AM in the error message
	$code =~ s{ERROR:  access method "$srcam" does not support included columns}
			  {ERROR:  access method "$dstam" does not support included columns}g;

	# Convert \d table output that embeds the index AM name in the output
	$code =~ s{("$unqualifiednamere")(?: UNIQUE,)? $srcam(?= \($balancedre\))}{$1 $dstam}g;

	return $code;
}

sub convert_code
{
	my ($input, $output) = @_;

	my $code = slurp($input);
	for (my $i = 0; $i < scalar(@src_tbl_am); $i++)
	{
		$code = convert_tables_to_use_am($code, $src_tbl_am[$i], $dst_tbl_am[$i]);
		$code = convert_expected_output($code, $src_tbl_am[$i], $dst_tbl_am[$i]);
	}
	for (my $i = 0; $i < scalar(@src_idx_am); $i++)
	{
		$code = convert_indexes_to_use_am($code, $src_idx_am[$i], $dst_idx_am[$i]);
		$code = convert_expected_output($code, $src_idx_am[$i], $dst_idx_am[$i]);
	}
	if (defined $datadir_prefix)
	{
		$code =~ s{(?=/data/\w+\.data)}{$datadir_prefix}g;
	}
	spew($output, $code);
}

sub slurp
{
	my ($path) = @_;
	my $fh = IO::File->new($path) or die "Cannot read $path: $!";
	my $results;
	{
		undef(local $/);
		$results = <$fh>;
	}
	$fh->close();
	return $results;
}

sub spew
{
	my ($path, $contents) = @_;
	my $fh = IO::File->new(">$path") or die "Cannot write $path: $!";
	$fh->print($contents);
	$fh->close();
}

# Handle user supplied arguments for translating tests, and/or the arguments for
# parsing a regression or isolation schedule file.  At present, the build system
# never invokes this script to do both together, but if that ever changes, we assume
# the order we do them in doesn't matter.
#

# First, translate the tests, if requested...
convert_code($src_regress[$_], $dst_regress[$_]) for (0..$#src_regress);
convert_code($src_expected[$_], $dst_expected[$_]) for (0..$#src_expected);
convert_code($src_spec[$_], $dst_spec[$_]) for (0..$#src_spec);
convert_code($src_output_iso[$_], $dst_output_iso[$_]) for (0..$#src_output_iso);

# Then, parse the schedule files, if requested...
my $schedule = "";
foreach my $path (@schedule)
{
	my $code = slurp($path);
	while ($code =~ m/^test:(.*)$/mg)
	{
		$schedule .= $1;
	}
}

# The build system reads the schedule from our stdout...
print($schedule, "\n");
