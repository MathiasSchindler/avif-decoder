#!/usr/bin/env perl
use strict;
use warnings;

my ($input_path, $output_path) = @ARGV;
die "usage: $0 docs/av1.html output.inc\n"
    unless defined $input_path && defined $output_path && @ARGV == 2;

open my $input, '<', $input_path or die "open $input_path: $!\n";
local $/;
my $html = <$input>;
close $input or die "close $input_path: $!\n";

my @tables = (
    [ 'Default_Txb_Skip_Cdf',       'av1_default_txb_skip_cdf' ],
    [ 'Default_Eob_Pt_16_Cdf',      'av1_default_eob_pt_16_cdf' ],
    [ 'Default_Eob_Pt_32_Cdf',      'av1_default_eob_pt_32_cdf' ],
    [ 'Default_Eob_Pt_64_Cdf',      'av1_default_eob_pt_64_cdf' ],
    [ 'Default_Eob_Pt_128_Cdf',     'av1_default_eob_pt_128_cdf' ],
    [ 'Default_Eob_Pt_256_Cdf',     'av1_default_eob_pt_256_cdf' ],
    [ 'Default_Eob_Pt_512_Cdf',     'av1_default_eob_pt_512_cdf' ],
    [ 'Default_Eob_Pt_1024_Cdf',    'av1_default_eob_pt_1024_cdf' ],
    [ 'Default_Eob_Extra_Cdf',      'av1_default_eob_extra_cdf' ],
    [ 'Default_Dc_Sign_Cdf',        'av1_default_dc_sign_cdf' ],
    [ 'Default_Coeff_Base_Eob_Cdf', 'av1_default_coeff_base_eob_cdf' ],
    [ 'Default_Coeff_Base_Cdf',     'av1_default_coeff_base_cdf' ],
    [ 'Default_Coeff_Br_Cdf',       'av1_default_coeff_br_cdf' ],
);
my @code_blocks = ($html =~ m{
    <div\ class="language-c\ highlighter-rouge">
    <div\ class="highlight"><pre\ class="highlight"><code>
    (.*?)
    </code></pre></div></div>
}xsg);

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by tools/generate-av1-coeff-cdfs.pl. */\n\n";

for my $table (@tables) {
    my ($spec_name, $c_name) = @$table;
    my @matches = grep { index($_, $spec_name) >= 0 } @code_blocks;
    die "expected one $spec_name table, found " . scalar(@matches) . "\n"
        unless @matches == 1;

    my $declaration = $matches[0];
    $declaration =~ s/<[^>]+>//g;
    $declaration =~ s/&lt;/</g;
    $declaration =~ s/&gt;/>/g;
    $declaration =~ s/&amp;/&/g;
    $declaration =~ s/\Q$spec_name\E/$c_name/;
    $declaration =~ s/^/static const uint16_t /;
    print {$output} $declaration, ";\n\n";
}

close $output or die "close $output_path: $!\n";