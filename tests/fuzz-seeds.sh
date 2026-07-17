#!/bin/sh
set -eu

corpus=${1:-build/fuzz/corpus}
generated=build/fuzz/generated-seeds
rm -rf "$corpus" "$generated"
mkdir -p "$corpus" "$generated"

copy_seeds() {
    prefix=$1
    shift
    index=0
    for input in "$@"; do
        if [ ! -f "$input" ]; then
            continue
        fi
        index=$((index + 1))
        cp "$input" "$corpus/$prefix-$(printf '%03d' "$index").avif"
    done
}

set --
for input in tests/fixtures/*.avif images/image-check/*.avif images/avif/*.avif; do
    if [ -f "$input" ]; then
        set -- "$@" "$input"
    fi
done
copy_seeds checked-in "$@"

set --
for input in tests/fuzz-regressions/*.avif; do
    if [ -f "$input" ]; then
        set -- "$@" "$input"
    fi
done
copy_seeds regression "$@"

set --
for input in build/*-test/*.avif build/reference-block/*.avif; do
    if [ -f "$input" ]; then
        set -- "$@" "$input"
    fi
done
copy_seeds test-output "$@"

if command -v ffmpeg >/dev/null 2>&1 &&
        command -v avifenc >/dev/null 2>&1; then
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i 'testsrc2=size=32x24:rate=1' -frames:v 1 \
        -pix_fmt rgb24 -y "$generated/source.png"
    avifenc --lossless --jobs 1 --codec aom --yuv 444 \
        "$generated/source.png" "$generated/still-444.avif" >/dev/null
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i 'testsrc2=size=34x26:rate=1' -frames:v 1 \
        -pix_fmt yuv420p10le -strict -1 -f yuv4mpegpipe \
        -y "$generated/source-10.y4m"
    avifenc --qcolor 60 --jobs 1 --codec aom \
        "$generated/source-10.y4m" "$generated/still-420-10.avif" \
        >/dev/null
    avifenc --qcolor 70 --jobs 1 --codec aom --yuv 444 --progressive \
        "$generated/source.png" "$generated/progressive.avif" >/dev/null

    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i 'color=red:size=64x64:rate=1' -frames:v 1 \
        -pix_fmt rgb24 -y "$generated/red.png"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i 'color=green:size=10x64:rate=1' -frames:v 1 \
        -pix_fmt rgb24 -y "$generated/green.png"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i 'color=blue:size=64x7:rate=1' -frames:v 1 \
        -pix_fmt rgb24 -y "$generated/blue.png"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i 'color=white:size=10x7:rate=1' -frames:v 1 \
        -pix_fmt rgb24 -y "$generated/white.png"
    avifenc --lossless --jobs 1 --codec aom --yuv 444 --grid 2x2 \
        "$generated/red.png" "$generated/green.png" \
        "$generated/blue.png" "$generated/white.png" \
        "$generated/grid.avif" >/dev/null
    avifenc --jobs 1 --codec aom --timescale 30 --keyframe 2 \
        "$generated/source.png" "$generated/source.png" \
        "$generated/source.png" "$generated/sequence.avif" >/dev/null
    copy_seeds generated "$generated"/*.avif
fi

perl -e '
    use strict;
    use warnings;
    my ($input, $output, $mode) = @ARGV;
    open my $in, "<:raw", $input or die "$input: $!\n";
    local $/;
    my $data = <$in>;
    close $in;
    if ($mode eq "truncate") {
        substr($data, int(length($data) / 2)) = "";
    } elsif ($mode eq "box-size") {
        substr($data, 0, 4) = pack("N", 0xffffffff);
    } else {
        my $position = index($data, $mode);
        exit 2 if $position < 0;
        if ($mode eq "iref") {
            substr($data, $position - 4, 4) = pack("N", 8);
        } elsif ($mode eq "iloc") {
            substr($data, $position + 8, 2) = "\xff\xff";
        } else {
            substr($data, $position + 8, 4) = pack("N", 0xffffffff);
        }
    }
    open my $out, ">:raw", $output or die "$output: $!\n";
    print {$out} $data;
    close $out;
' "$corpus/checked-in-001.avif" "$corpus/malformed-truncated.avif" truncate
perl -e '
    use strict;
    use warnings;
    local $/;
    my ($input, $output) = @ARGV;
    open my $in, "<:raw", $input or die "$input: $!\n";
    my $data = <$in>;
    close $in;
    substr($data, 0, 4) = pack("N", 0xffffffff);
    open my $out, ">:raw", $output or die "$output: $!\n";
    print {$out} $data;
' "$corpus/checked-in-001.avif" "$corpus/malformed-box-size.avif"

mutation_source=
for candidate in "$corpus"/generated-*.avif "$corpus"/test-output-*.avif; do
    if [ -f "$candidate" ]; then
        mutation_source=$candidate
        break
    fi
done
if [ -n "$mutation_source" ]; then
    for box in iref iloc stsz stsc stss; do
        output="$corpus/malformed-$box.avif"
        if ! perl -e '
            use strict;
            use warnings;
            local $/;
            my ($input, $output, $type) = @ARGV;
            open my $in, "<:raw", $input or die "$input: $!\n";
            my $data = <$in>;
            close $in;
            my $position = index($data, $type);
            exit 2 if $position < 0;
            if ($type eq "iref") {
                substr($data, $position - 4, 4) = pack("N", 8);
            } elsif ($type eq "iloc") {
                substr($data, $position + 8, 2) = "\xff\xff";
            } else {
                substr($data, $position + 8, 4) = pack("N", 0xffffffff);
            }
            open my $out, ">:raw", $output or die "$output: $!\n";
            print {$out} $data;
        ' "$mutation_source" "$output" "$box"; then
            rm -f "$output"
        fi
    done
fi

count=$(find "$corpus" -type f -name '*.avif' | wc -l | tr -d ' ')
echo "fuzz seed corpus: $count files in $corpus"
