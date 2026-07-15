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

my @matches = grep { /^\s*Gaussian_Sequence\s*\[/ } @plain_blocks;
die "expected one Gaussian_Sequence table, found " . scalar(@matches) . "\n"
    unless @matches == 1;

my $block = $matches[0];
my ($body) = $block =~ /\{(.*)\}/s;
die "could not locate table body\n" unless defined $body;

my @values = ($body =~ /(-?\d+)/g);
die "expected 2048 entries, found " . scalar(@values) . "\n"
    unless @values == 2048;

for my $value (@values) {
    die "value $value out of int16 range\n"
        unless $value >= -2048 && $value <= 2047;
}

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by "
    . "tools/generate-av1-film-grain-table.pl. */\n\n";
print {$output} "static const int16_t av1_film_grain_gaussian[ 2048 ] = {\n";
for (my $i = 0; $i < @values; $i += 12) {
    my $end = $i + 12;
    $end = @values if $end > @values;
    my @row = @values[$i .. $end - 1];
    print {$output} "    ", join(', ', @row), ",\n";
}
print {$output} "};\n";
close $output or die "close $output_path: $!\n";
