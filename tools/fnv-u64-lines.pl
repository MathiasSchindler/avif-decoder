#!/usr/bin/env perl
use strict;
use warnings;
use Math::BigInt;

my $checksum = Math::BigInt->new('1469598103934665603');
my $prime = Math::BigInt->new('1099511628211');
my $mask = Math::BigInt->new('0xffffffffffffffff');

while (my $line = <STDIN>) {
    chomp $line;
    $line =~ s/^0x//;
    die "invalid 64-bit hexadecimal value: $line\n"
        unless $line =~ /\A[0-9a-fA-F]{1,16}\z/;
    my $value = Math::BigInt->from_hex("0x$line");
    for my $byte (0 .. 7) {
        my $part = $value->copy()->brsft($byte * 8)->band(0xff);
        $checksum->bxor($part)->bmul($prime)->band($mask);
    }
}

my $hex = $checksum->as_hex();
$hex =~ s/^0x//;
print '0x', ('0' x (16 - length($hex))), $hex, "\n";
