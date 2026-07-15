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

my @code_blocks = ($html =~ m{
    <div\ class="language-c\ highlighter-rouge">
    <div\ class="highlight"><pre\ class="highlight"><code>
    (.*?)
    </code></pre></div></div>
}xsg);
my @plain_blocks = map {
    my $block = $_;
    $block =~ s/<[^>]+>//g;
    $block =~ s/&lt;/</g;
    $block =~ s/&gt;/>/g;
    $block =~ s/&amp;/&/g;
    $block;
} @code_blocks;
my @tables = (
    [ 'Dc_Qlookup', 'av1_dc_qlookup', 'uint16_t' ],
    [ 'Ac_Qlookup', 'av1_ac_qlookup', 'uint16_t' ],
    [ 'Qm_Offset', 'av1_qm_offset', 'uint16_t' ],
    [ 'Quantizer_Matrix', 'av1_quantizer_matrix', 'uint8_t' ],
);

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by tools/generate-av1-quant-tables.pl. */\n\n";
for my $table (@tables) {
    my ($spec_name, $local_name, $type) = @$table;
    my @matches = grep { /^\Q$spec_name\E\s*\[/ } @plain_blocks;
    die "expected one $spec_name table, found " . scalar(@matches) . "\n"
        unless @matches == 1;
    my $declaration = $matches[0];
    $declaration =~ s/\Q$spec_name\E/$local_name/;
    $declaration =~ s/TX_SIZES_ALL/AV1_TX_SIZES_ALL/g;
    $declaration =~ s/QM_TOTAL_SIZE/3344/g;
    $declaration =~ s/^/static const $type /;
    print {$output} $declaration, ";\n\n";
}
close $output or die "close $output_path: $!\n";