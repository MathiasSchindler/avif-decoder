#!/usr/bin/env perl
use strict;
use warnings;
use Math::BigInt;

@ARGV == 1 or die "usage: $0 FILE.y4m\n";
open my $file, '<:raw', $ARGV[0] or die "$ARGV[0]: $!\n";
local $/;
my $data = <$file>;
close $file or die "$ARGV[0]: $!\n";

$data =~ /\AYUV4MPEG2 ([^\n]*)\n/ or die "invalid Y4M header\n";
my $header = $1;
$header =~ /(?:^| )W([0-9]+)(?: |$)/ or die "missing Y4M width\n";
my $width = 0 + $1;
$header =~ /(?:^| )H([0-9]+)(?: |$)/ or die "missing Y4M height\n";
my $height = 0 + $1;
$header =~ /(?:^| )C([^ ]+)(?: |$)/ or die "missing Y4M chroma format\n";
my $chroma = $1;
my $depth = $chroma =~ /p(10|12|16)/ ? 0 + $1 : 8;
my $bytes_per_sample = $depth > 8 ? 2 : 1;
my @dimensions;
if ($chroma =~ /^444/) {
    @dimensions = ([$width, $height], [$width, $height], [$width, $height]);
} elsif ($chroma =~ /^422/) {
    @dimensions = ([$width, $height], [($width + 1) >> 1, $height],
                   [($width + 1) >> 1, $height]);
} elsif ($chroma =~ /^420/) {
    @dimensions = ([$width, $height], [($width + 1) >> 1, ($height + 1) >> 1],
                   [($width + 1) >> 1, ($height + 1) >> 1]);
} elsif ($chroma =~ /^mono/) {
    @dimensions = ([$width, $height]);
} else {
    die "unsupported Y4M chroma format '$chroma'\n";
}

my $frame = index($data, "FRAME\n");
$frame >= 0 or die "missing Y4M frame\n";
my $offset = $frame + 6;
my $prime = Math::BigInt->new('1099511628211');
my $mask = Math::BigInt->new('0xffffffffffffffff');
my $checksum = Math::BigInt->new('1469598103934665603');

sub hash_byte {
    my ($value, $byte) = @_;
    $value->bxor($byte);
    $value->bmul($prime);
    $value->band($mask);
}

for my $plane (0 .. $#dimensions) {
    my ($plane_width, $plane_height) = @{$dimensions[$plane]};
    my $plane_checksum = Math::BigInt->new('1469598103934665603');
    hash_byte($plane_checksum, $plane);
    for (1 .. $plane_width * $plane_height) {
        $offset + $bytes_per_sample <= length($data)
            or die "truncated Y4M frame\n";
        my $sample = $bytes_per_sample == 1
            ? ord(substr($data, $offset, 1))
            : unpack('v', substr($data, $offset, 2));
        $offset += $bytes_per_sample;
        hash_byte($plane_checksum, $sample & 255);
        hash_byte($plane_checksum, $sample >> 8);
    }
    for my $byte (0 .. 7) {
        my $part = $plane_checksum->copy()->brsft($byte * 8)->band(255)->numify();
        hash_byte($checksum, $part);
    }
}

my $hex = substr($checksum->as_hex(), 2);
print '0x', ('0' x (16 - length($hex))), $hex, "\n";