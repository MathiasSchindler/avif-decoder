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
    [ 'Default_Txb_Skip_Cdf',       'av1_default_txb_skip_cdf',       3, 260 ],
    [ 'Default_Eob_Pt_16_Cdf',      'av1_default_eob_pt_16_cdf',      6, 16 ],
    [ 'Default_Eob_Pt_32_Cdf',      'av1_default_eob_pt_32_cdf',      7, 16 ],
    [ 'Default_Eob_Pt_64_Cdf',      'av1_default_eob_pt_64_cdf',      8, 16 ],
    [ 'Default_Eob_Pt_128_Cdf',     'av1_default_eob_pt_128_cdf',     9, 16 ],
    [ 'Default_Eob_Pt_256_Cdf',     'av1_default_eob_pt_256_cdf',    10, 16 ],
    [ 'Default_Eob_Pt_512_Cdf',     'av1_default_eob_pt_512_cdf',    11, 8 ],
    [ 'Default_Eob_Pt_1024_Cdf',    'av1_default_eob_pt_1024_cdf',   12, 8 ],
    [ 'Default_Eob_Extra_Cdf',      'av1_default_eob_extra_cdf',      3, 360 ],
    [ 'Default_Dc_Sign_Cdf',        'av1_default_dc_sign_cdf',        3, 24 ],
    [ 'Default_Coeff_Base_Eob_Cdf', 'av1_default_coeff_base_eob_cdf', 4, 160 ],
    [ 'Default_Coeff_Base_Cdf',     'av1_default_coeff_base_cdf',     5, 1680 ],
    [ 'Default_Coeff_Br_Cdf',       'av1_default_coeff_br_cdf',       5, 840 ],
);
my @code_blocks = ($html =~ m{
    <div\ class="language-c\ highlighter-rouge">
    <div\ class="highlight"><pre\ class="highlight"><code>
    (.*?)
    </code></pre></div></div>
}xsg);

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by tools/generate-av1-coeff-cdfs.pl. */\n";
print {$output} "/* CDF rows omit the invariant 32768 terminal and zero adaptation count. */\n\n";

for my $table (@tables) {
    my ($spec_name, $c_name, $stride, $expected_rows) = @$table;
    my @matches = grep { index($_, $spec_name) >= 0 } @code_blocks;
    die "expected one $spec_name table, found " . scalar(@matches) . "\n"
        unless @matches == 1;

    my $declaration = $matches[0];
    $declaration =~ s/<[^>]+>//g;
    $declaration =~ s/&lt;/</g;
    $declaration =~ s/&gt;/>/g;
    $declaration =~ s/&amp;/&/g;
    my ($initializer) = $declaration =~ /=\s*(\{.*\})\s*$/s;
    die "could not isolate $spec_name initializer\n"
        unless defined $initializer;
    my @rows = ($initializer =~ /\{\s*([^{}]+?)\s*\}/g);
    die "$spec_name has " . scalar(@rows) . " rows, expected $expected_rows\n"
        unless @rows == $expected_rows;

    my @packed;
    for my $row (0 .. $#rows) {
        my @values = split /,/, $rows[$row];
        s/^\s+|\s+$//g for @values;
        die "$spec_name row $row has " . scalar(@values) .
            " values, expected $stride\n" unless @values == $stride;
        for my $value (@values) {
            die "$spec_name row $row has unsupported value '$value'\n"
                unless $value =~ /^\d+(?:\s*\*\s*\d+)*$/;
            my @factors = split /\s*\*\s*/, $value;
            $value = 1;
            $value *= $_ for @factors;
        }
        die "$spec_name row $row has a non-invariant CDF suffix\n"
            unless $values[-2] == 32768 && $values[-1] == 0;
        splice @values, -2;
        push @packed, @values;
    }
    print {$output} "static const uint16_t ${c_name}_packed[" .
                    scalar(@packed) . "] = {\n";
    while (@packed) {
        my @line = splice @packed, 0, 12;
        print {$output} "    ", join(', ', map { "${_}U" } @line), ",\n";
    }
    print {$output} "};\n\n";
}

close $output or die "close $output_path: $!\n";