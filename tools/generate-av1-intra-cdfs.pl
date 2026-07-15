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
    [ 'Default_Intra_Frame_Y_Mode_Cdf',       'av1_default_intra_frame_y_mode_cdf' ],
    [ 'Default_Uv_Mode_Cfl_Not_Allowed_Cdf',  'av1_default_uv_mode_cfl_not_allowed_cdf' ],
    [ 'Default_Uv_Mode_Cfl_Allowed_Cdf',      'av1_default_uv_mode_cfl_allowed_cdf' ],
    [ 'Default_Angle_Delta_Cdf',              'av1_default_intra_angle_delta_cdf' ],
    [ 'Default_Filter_Intra_Mode_Cdf',        'av1_default_filter_intra_mode_cdf' ],
    [ 'Default_Filter_Intra_Cdf',             'av1_default_filter_intra_cdf' ],
    [ 'Default_Intra_Tx_Type_Set1_Cdf',       'av1_default_intra_tx_type_set1_cdf' ],
    [ 'Default_Intra_Tx_Type_Set2_Cdf',       'av1_default_intra_tx_type_set2_cdf' ],
    [ 'Default_Cfl_Sign_Cdf',                 'av1_default_cfl_sign_cdf' ],
    [ 'Default_Cfl_Alpha_Cdf',                'av1_default_cfl_alpha_cdf' ],
);
my @code_blocks = ($html =~ m{
    <div\ class="language-c\ highlighter-rouge">
    <div\ class="highlight"><pre\ class="highlight"><code>
    (.*?)
    </code></pre></div></div>
}xsg);

open my $output, '>', $output_path or die "open $output_path: $!\n";
print {$output} "/* Generated from docs/av1.html by tools/generate-av1-intra-cdfs.pl. */\n\n";

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