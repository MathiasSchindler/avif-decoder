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

sub table_values {
    my ($name, $expected) = @_;
    my @matches = grep { /(?:^|\n)\Q$name\E\s*\[/ } @plain_blocks;
    die "expected one $name table, found " . scalar(@matches) . "\n"
        unless @matches == 1;
    my ($initializer) = $matches[0] =~ /
        (?:^|\n)\Q$name\E\s*\[[^=]*=\s*(.*?)\n\}
    /xs;
    die "could not isolate $name initializer\n" unless defined $initializer;
    my @values = $initializer =~ /(?<![A-Za-z_])-?\d+/g;
    die "$name has " . scalar(@values) . " values, expected $expected\n"
        unless @values == $expected;
    return @values;
}

sub print_size_table {
    my ($output, $spec_name, $local_name) = @_;
    my @values = table_values($spec_name, 7 * 8);
    print {$output} "static const uint16_t ${local_name}[7][8] = {\n";
    for my $row (0 .. 6) {
        my @slice = @values[$row * 8 .. $row * 8 + 7];
        print {$output} "    { ", join(', ', map { "${_}U" } @slice), " }",
                        ($row == 6 ? "\n" : ",\n");
    }
    print {$output} "};\n\n";
}

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by tools/generate-av1-palette-tables.pl. */\n\n";
print_size_table($output, 'Default_Palette_Y_Size_Cdf',
                 'av1_default_palette_y_size');
print_size_table($output, 'Default_Palette_Uv_Size_Cdf',
                 'av1_default_palette_uv_size');

print {$output} "static const uint16_t av1_default_palette_color[2][7][5][9] = {\n";
for my $plane (qw(Y Uv)) {
    print {$output} "    {\n";
    for my $size (2 .. 8) {
        my $name = "Default_Palette_Size_${size}_${plane}_Color_Cdf";
        my @values = table_values($name, 5 * ($size + 1));
        print {$output} "        {\n";
        for my $context (0 .. 4) {
            my @row = @values[$context * ($size + 1) ..
                              $context * ($size + 1) + $size];
            push @row, (0) x (9 - scalar(@row));
            print {$output} "            { ",
                join(', ', map { "${_}U" } @row), " }",
                ($context == 4 ? "\n" : ",\n");
        }
        print {$output} "        }", ($size == 8 ? "\n" : ",\n");
    }
    print {$output} "    }", ($plane eq 'Uv' ? "\n" : ",\n");
}
print {$output} "};\n";
close $output or die "close $output_path: $!\n";