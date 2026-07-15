#!/bin/sh
set -eu

decoder=${1:-build/$(uname -m)/avifdec}
work=build/sequence-test
rm -rf "$work"
mkdir -p "$work"

for tool in ffmpeg avifenc avifdec magick perl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "sequence test requires $tool" >&2
        exit 1
    }
done

magick -size 32x24 xc:red "$work/f0.png"
magick -size 32x24 xc:green "$work/f1.png"
magick -size 32x24 xc:blue "$work/f2.png"
avifenc --jobs 1 --codec aom --timescale 1000 --keyframe 2 \
    --duration 100 "$work/f0.png" \
    --duration 200 "$work/f1.png" \
    --duration 300 "$work/f2.png" "$work/sequence.avif" >/dev/null

output=$("$decoder" "$work/sequence.avif")
printf '%s\n' "$output" | grep -q '^sequence_frames=3$'
printf '%s\n' "$output" | grep -q '^sequence_timescale=1000$'
printf '%s\n' "$output" | grep -q '^sequence_duration=600$'
printf '%s\n' "$output" | grep -q '^sequence_repeat_forever=1$'

for index in 0 1 2; do
    output=$("$decoder" --raw-frame "$index" "$work/sequence.avif" \
        "$work/ours-$index.yuv")
    avifdec --index "$index" --codec aom "$work/sequence.avif" \
        "$work/reference-$index.y4m" >/dev/null
    ffmpeg -hide_banner -loglevel error \
        -i "$work/reference-$index.y4m" -frames:v 1 \
        -pix_fmt yuv444p -f rawvideo -y "$work/reference-$index.yuv"
    cmp "$work/ours-$index.yuv" "$work/reference-$index.yuv"
    "$decoder" --raw-frame "$index" "$work/sequence.avif" \
        "$work/repeat-$index.yuv" >/dev/null
    cmp "$work/ours-$index.yuv" "$work/repeat-$index.yuv"
    case $index in
        0)
            printf '%s\n' "$output" | grep -q '^frame_sync_index=0$'
            printf '%s\n' "$output" | grep -q '^frame_duration=100$'
            ;;
        1)
            printf '%s\n' "$output" | grep -q '^frame_sync_index=0$'
            printf '%s\n' "$output" | grep -q '^frame_dts=100$'
            printf '%s\n' "$output" | grep -q '^frame_duration=200$'
            ;;
        2)
            printf '%s\n' "$output" | grep -q '^frame_sync_index=2$'
            printf '%s\n' "$output" | grep -q '^frame_dts=300$'
            printf '%s\n' "$output" | grep -q '^frame_duration=300$'
            ;;
    esac
done

avifenc --jobs 1 --codec aom --timescale 10 --repetition-count 2 \
    "$work/f0.png" "$work/f1.png" "$work/repeat.avif" >/dev/null
output=$("$decoder" "$work/repeat.avif")
printf '%s\n' "$output" | grep -q '^sequence_repeat_forever=0$'
printf '%s\n' "$output" | grep -q '^sequence_repeat_count=2$'

for color in red green blue; do
    magick -size 16x12 "xc:$color" -alpha set \
        -channel A -evaluate set 50% +channel "$work/a-$color.png"
done
avifenc --jobs 1 --codec aom --timescale 30 --keyframe 1 \
    "$work/a-red.png" "$work/a-green.png" "$work/a-blue.png" \
    "$work/alpha.avif" >/dev/null
avifenc --jobs 1 --codec aom --timescale 30 --keyframe 1 --premultiply \
    "$work/a-red.png" "$work/a-green.png" "$work/a-blue.png" \
    "$work/premul.avif" >/dev/null

output=$("$decoder" "$work/alpha.avif")
printf '%s\n' "$output" | grep -q '^sequence_alpha=1$'
printf '%s\n' "$output" | grep -q '^sequence_alpha_premultiplied=0$'
output=$("$decoder" "$work/premul.avif")
printf '%s\n' "$output" | grep -q '^sequence_alpha=1$'
printf '%s\n' "$output" | grep -q '^sequence_alpha_premultiplied=1$'

for variant in alpha premul; do
    "$decoder" --png-frame 1 "$work/$variant.avif" \
        "$work/$variant-ours.png" >/dev/null
    avifdec --index 1 --codec aom "$work/$variant.avif" \
        "$work/$variant-reference.png" >/dev/null
    magick "$work/$variant-ours.png" -alpha extract \
        -depth 8 "gray:$work/$variant-ours-alpha.raw"
    magick "$work/$variant-reference.png" -alpha extract \
        -depth 8 "gray:$work/$variant-reference-alpha.raw"
    cmp "$work/$variant-ours-alpha.raw" \
        "$work/$variant-reference-alpha.raw"
done

perl -e '
    use strict;
    use warnings;
    local $/;
    my $data = <>;
    sub u32 { unpack("N", substr($data, $_[0], 4)) }
    sub put32 { substr($data, $_[0], 4) = pack("N", $_[1]) }
    sub box_at {
        my ($type) = @_;
        my $position = index($data, $type);
        die "$type not found\n" if $position < 4;
        return $position - 4;
    }
    sub parent_of {
        my ($type, $child) = @_;
        my $from = 0;
        my $best = -1;
        while (1) {
            my $position = index($data, $type, $from);
            last if $position < 0;
            my $box = $position - 4;
            my $size = u32($box);
            $best = $box if $box < $child && $child < $box + $size;
            $from = $position + 1;
        }
        die "parent $type not found\n" if $best < 0;
        return $best;
    }
    my $stsz = box_at("stsz");
    my $count = u32($stsz + 16);
    die "unexpected sample count\n" unless $count == 3;
    my @sizes = map { u32($stsz + 20 + 4 * $_) } 0 .. $count - 1;
    my $replacement = pack("N a4 N C3 C N n*", 26, "stz2", 0,
        0, 0, 0, 16, $count, @sizes);
    my $old_size = u32($stsz);
    my $delta = $old_size - length($replacement);
    my @parents;
    my $child = $stsz;
    for my $type ("stbl", "minf", "mdia", "trak", "moov") {
        my $parent = parent_of($type, $child);
        push @parents, $parent;
        $child = $parent;
    }
    substr($data, $stsz, $old_size) = $replacement;
    for my $parent (@parents) {
        put32($parent, u32($parent) - $delta);
    }
    my $stco = box_at("stco");
    put32($stco + 16, u32($stco + 16) - $delta);
    print $data;
' "$work/sequence.avif" > "$work/compact.avif"

for index in 0 1 2; do
    "$decoder" --raw-frame "$index" "$work/compact.avif" \
        "$work/compact-$index.yuv" >/dev/null
    cmp "$work/ours-$index.yuv" "$work/compact-$index.yuv"
done

perl -e '
    local $/;
    my $data = <>;
    my $position = index($data, "stz2");
    die "stz2 not found\n" if $position < 4;
    substr($data, $position + 8, 4) = pack("N", 4);
    print $data;
' "$work/compact.avif" > "$work/compact-invalid.avif"
if "$decoder" "$work/compact-invalid.avif" >/dev/null 2>&1; then
    echo "sequence test: malformed compact sample table was accepted" >&2
    exit 1
fi

echo "AVIF sequence test: timing, sync seek, alpha, compact tables, and exact frames"
