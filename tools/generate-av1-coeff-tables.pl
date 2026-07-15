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

my @scan_shapes = qw(
    4x4 4x8 8x4 8x8 8x16 16x8 16x16 16x32 32x16 32x32
    4x16 16x4 8x32 32x8
);
my @directional_shapes = qw(
    4x4 4x8 8x4 8x8 8x16 16x8 16x16 4x16 16x4
);
my @tables;

push @tables, map {
    [ "Default_Scan_$_", "av1_default_scan_$_", 'uint16_t' ]
} @scan_shapes;
for my $prefix (qw(Mrow Mcol)) {
    my $local_prefix = lc $prefix;
    push @tables, map {
        [ "${prefix}_Scan_$_", "av1_${local_prefix}_scan_$_", 'uint16_t' ]
    } @directional_shapes;
}
push @tables,
    [ 'Coeff_Base_Ctx_Offset', 'av1_coeff_base_ctx_offset', 'uint8_t' ],
    [ 'Coeff_Base_Pos_Ctx_Offset', 'av1_coeff_base_pos_ctx_offset', 'uint8_t' ],
    [ 'Mag_Ref_Offset_With_Tx_Class', 'av1_mag_ref_offset', 'uint8_t' ],
    [ 'Sig_Ref_Diff_Offset', 'av1_sig_ref_diff_offset', 'uint8_t' ],
    [ 'Adjusted_Tx_Size', 'av1_adjusted_tx_size', 'uint8_t' ];

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

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by tools/generate-av1-coeff-tables.pl. */\n\n";

for my $table (@tables) {
    my ($spec_name, $local_name, $type) = @$table;
    my @matches = grep { /^\Q$spec_name\E\s*\[/ } @plain_blocks;
    die "expected one $spec_name table, found " . scalar(@matches) . "\n"
        unless @matches == 1;

    my $declaration = $matches[0];
    $declaration =~ s/\Q$spec_name\E/$local_name/;
    $declaration =~ s/TX_SIZES_ALL/AV1_TX_SIZES_ALL/g;
    $declaration =~ s/SIG_REF_DIFF_OFFSET_NUM/5/g;
    $declaration =~ s/SIG_COEF_CONTEXTS_2D/26/g;
    $declaration =~ s/\bTX_([0-9]+X[0-9]+)\b/AV1_TX_$1/g;
    $declaration =~ s/^/static const $type /;
    print {$output} $declaration, ";\n\n";
}

close $output or die "close $output_path: $!\n";