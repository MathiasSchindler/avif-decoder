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
    </code></pre></div>\s*</div>
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
        (?:^|\n)\Q$name\E\s*\[[^=]*=\s*(\{.*\})\s*$
    /xs;
    die "could not isolate $name initializer\n" unless defined $initializer;
    my @values = $initializer =~ /-?\s*\d+/g;
    s/\s+//g for @values;
    die "$name has " . scalar(@values) . " values, expected $expected\n"
        unless @values == $expected;
    return @values;
}

my @filters = table_values('Warped_Filters', 193 * 8);
my @divisors = table_values('Div_Lut', 257);

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by ",
                "tools/generate-av1-warp-tables.pl. */\n\n";
print {$output} "static const int16_t av1_warped_filters[193][8] = {\n";
for my $row (0 .. 192) {
    my @values = @filters[$row * 8 .. $row * 8 + 7];
    print {$output} "    { ", join(', ', @values), " }",
                    ($row == 192 ? "\n" : ",\n");
}
print {$output} "};\n\n";
print {$output} "static const uint16_t av1_warp_div_lut[257] = {\n";
for my $row (0 .. 16) {
    my $first = $row * 16;
    my $last = $first + 15;
    $last = 256 if $last > 256;
    my @values = map { "${_}U" } @divisors[$first .. $last];
    print {$output} "    ", join(', ', @values),
                    ($last == 256 ? "\n" : ",\n");
}
print {$output} "};\n";
close $output or die "close $output_path: $!\n";
