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

my @matrix_matches = grep { /^Quantizer_Matrix\s*\[/ } @plain_blocks;
die "expected one Quantizer_Matrix table, found " .
    scalar(@matrix_matches) . "\n" unless @matrix_matches == 1;
my $matrix = $matrix_matches[0];
$matrix =~ s/^[^{]*//s;
$matrix =~ s{/\*.*?\*/}{}gs;
my @values = ($matrix =~ /\b(\d+)\b/g);
die "expected 100320 quantizer values, found " . scalar(@values) . "\n"
    unless @values == 15 * 2 * 3344;

my @frequency = (0) x 256;
for my $matrix_index (0 .. 29) {
    my $base = $matrix_index * 3344;
    for my $index (1 .. 3343) {
        ++$frequency[($values[$base + $index] -
                      $values[$base + $index - 1]) & 255];
    }
}
my @nodes;
my @queue;
for my $symbol (0 .. 255) {
    next unless $frequency[$symbol];
    push @nodes, {
        frequency => $frequency[$symbol],
        symbol => $symbol,
        left => -1,
        right => -1,
        order => $symbol,
    };
    push @queue, $#nodes;
}
while (@queue > 1) {
    @queue = sort {
        $nodes[$a]{frequency} <=> $nodes[$b]{frequency} ||
        $nodes[$a]{order} <=> $nodes[$b]{order}
    } @queue;
    my $left = shift @queue;
    my $right = shift @queue;
    push @nodes, {
        frequency => $nodes[$left]{frequency} + $nodes[$right]{frequency},
        symbol => -1,
        left => $left,
        right => $right,
        order => 256 + scalar(@nodes),
    };
    push @queue, $#nodes;
}
my $root = $queue[0];
my @codes;
my $assign_codes;
$assign_codes = sub {
    my ($node_index, $bits) = @_;
    my $node = $nodes[$node_index];
    if ($node->{symbol} >= 0) {
        $codes[$node->{symbol}] = [ @$bits ];
        return;
    }
    $assign_codes->($node->{left}, [ @$bits, 0 ]);
    $assign_codes->($node->{right}, [ @$bits, 1 ]);
};
$assign_codes->($root, []);

my @internal = grep { $nodes[$_]{symbol} < 0 } 0 .. $#nodes;
my %internal_index;
for my $index (0 .. $#internal) {
    $internal_index{$internal[$index]} = $index;
}
my @tree;
for my $node_index (@internal) {
    my @children;
    for my $child ($nodes[$node_index]{left}, $nodes[$node_index]{right}) {
        push @children, $nodes[$child]{symbol} >= 0
            ? -1 - $nodes[$child]{symbol}
            : $internal_index{$child};
    }
    push @tree, \@children;
}

my @compressed;
my @offsets;
for my $matrix_index (0 .. 29) {
    my $base = $matrix_index * 3344;
    push @offsets, scalar(@compressed);
    push @compressed, $values[$base];
    my $byte = 0;
    my $bit_count = 0;
    for my $index (1 .. 3343) {
        my $delta = ($values[$base + $index] -
                     $values[$base + $index - 1]) & 255;
        for my $bit (@{$codes[$delta]}) {
            $byte = ($byte << 1) | $bit;
            ++$bit_count;
            if ($bit_count == 8) {
                push @compressed, $byte;
                $byte = 0;
                $bit_count = 0;
            }
        }
    }
    push @compressed, $byte << (8 - $bit_count) if $bit_count;
}
push @offsets, scalar(@compressed);

sub print_array {
    my ($file, $values, $per_line, $suffix) = @_;
    for my $index (0 .. $#$values) {
        print {$file} "    " if $index % $per_line == 0;
        print {$file} $values->[$index], $suffix;
        print {$file} $index == $#$values || $index % $per_line == $per_line - 1
            ? "\n" : " ";
    }
}

print {$output} "static const int16_t av1_qm_tree[][2] = {\n";
for my $children (@tree) {
    print {$output} "    { $children->[0], $children->[1] },\n";
}
print {$output} "};\n\n";
print {$output} "enum { AV1_QM_TREE_ROOT = $internal_index{$root} };\n\n";
print {$output} "static const uint16_t av1_qm_compressed_offsets[31] = {\n";
print_array($output, \@offsets, 8, "U,");
print {$output} "};\n\n";
print {$output} "static const uint8_t av1_qm_compressed[" .
    scalar(@compressed) . "] = {\n";
print_array($output, \@compressed, 16, "U,");
print {$output} "};\n\n";
close $output or die "close $output_path: $!\n";