#!/bin/sh
set -eu

decoder=${1:-build/$(uname -m)/avifdec}
work=build/presentation-test
rm -rf "$work"
mkdir -p "$work"

for tool in ffmpeg ffprobe avifenc avifdec magick perl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "presentation test requires $tool" >&2
        exit 1
    }
done

render_rgb() {
    input=$1
    name=$2
    "$decoder" --rgb "$input" "$work/$name-ours.rgb" >/dev/null
    magick "$input" "$work/$name-reference.png"
    ffmpeg -hide_banner -loglevel error \
        -i "$work/$name-reference.png" -frames:v 1 \
        -pix_fmt rgb24 -f rawvideo -y "$work/$name-reference.rgb"
    cmp "$work/$name-reference.rgb" "$work/$name-ours.rgb"
    "$decoder" --png "$input" "$work/$name-ours.png" >/dev/null
    ffmpeg -hide_banner -loglevel error \
        -i "$work/$name-ours.png" -frames:v 1 \
        -pix_fmt rgb24 -f rawvideo -y "$work/$name-png.rgb"
    cmp "$work/$name-ours.rgb" "$work/$name-png.rgb"
}

render_rgba() {
    input=$1
    name=$2
    mode=$3
    "$decoder" "$mode" "$input" "$work/$name-ours.rgba" >/dev/null
    magick "$input" "$work/$name-reference.png"
    ffmpeg -hide_banner -loglevel error \
        -i "$work/$name-reference.png" -frames:v 1 \
        -pix_fmt rgba -f rawvideo -y "$work/$name-reference.rgba"
    cmp "$work/$name-reference.rgba" "$work/$name-ours.rgba"
    "$decoder" --rgba "$input" "$work/$name-straight.rgba" >/dev/null
    "$decoder" --png "$input" "$work/$name-ours.png" >/dev/null
    ffmpeg -hide_banner -loglevel error \
        -i "$work/$name-ours.png" -frames:v 1 \
        -pix_fmt rgba -f rawvideo -y "$work/$name-png.rgba"
    cmp "$work/$name-straight.rgba" "$work/$name-png.rgba"
}

ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=128x128:rate=1' -frames:v 1 \
    -pix_fmt rgb24 -y "$work/source.png"
avifenc --lossless --jobs 1 --codec aom --yuv 444 \
    --crop 1,2,120,110 --irot 1 --imir 1 --pasp 2,1 \
    "$work/source.png" "$work/transforms.avif" >/dev/null
render_rgb "$work/transforms.avif" transforms
[ "$(wc -c < "$work/transforms-ours.rgb")" -eq 39600 ]
output=$("$decoder" "$work/transforms.avif")
printf '%s\n' "$output" | grep -q '^presentation_width=110$'
printf '%s\n' "$output" | grep -q '^presentation_height=120$'
printf '%s\n' "$output" | grep -q '^transform_flags=15$'

ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=38x30:rate=1' -frames:v 1 \
    -pix_fmt yuv444p10le -strict -1 -f yuv4mpegpipe \
    -y "$work/source10.y4m"
avifenc --lossless --jobs 1 --codec aom \
    "$work/source10.y4m" "$work/rgb10.avif" >/dev/null
"$decoder" --rgb16 "$work/rgb10.avif" "$work/rgb10-ours.rgb16" >/dev/null
output=$("$decoder" --png "$work/rgb10.avif" "$work/rgb10-ours.png")
printf '%s\n' "$output" | grep -q '^packed_format=png-rgb16$'
ffmpeg -hide_banner -loglevel error \
    -i "$work/rgb10-ours.png" -frames:v 1 \
    -pix_fmt rgb48le -f rawvideo -y "$work/rgb10-png.rgb16"
cmp "$work/rgb10-ours.rgb16" "$work/rgb10-png.rgb16"

ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'color=red:size=64x64:rate=1' -frames:v 1 \
    -pix_fmt rgb24 -y "$work/t00.png"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'color=green:size=10x64:rate=1' -frames:v 1 \
    -pix_fmt rgb24 -y "$work/t01.png"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'color=blue:size=64x7:rate=1' -frames:v 1 \
    -pix_fmt rgb24 -y "$work/t10.png"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'color=white:size=10x7:rate=1' -frames:v 1 \
    -pix_fmt rgb24 -y "$work/t11.png"
avifenc --lossless --jobs 1 --codec aom --yuv 444 --grid 2x2 \
    "$work/t00.png" "$work/t01.png" "$work/t10.png" "$work/t11.png" \
    "$work/grid.avif" >/dev/null
render_rgb "$work/grid.avif" grid
[ "$(wc -c < "$work/grid-ours.rgb")" -eq 15762 ]
"$decoder" --workers 1 --rgb "$work/grid.avif" \
    "$work/grid-worker1.rgb" >"$work/grid-worker1.out"
"$decoder" --rgb "$work/grid.avif" \
    "$work/grid-worker4.rgb" --workers 4 >"$work/grid-worker4.out"
cmp "$work/grid-worker1.rgb" "$work/grid-worker4.rgb"
"$decoder" --png "$work/grid.avif" \
    "$work/grid-worker4.png" --workers 4 >/dev/null
cmp "$work/grid-ours.png" "$work/grid-worker4.png"
grep -q '^decode_workers=1$' "$work/grid-worker1.out"
if { [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ]; } ||
   { [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; }; then
    grep -q '^decode_workers=4$' "$work/grid-worker4.out"
else
    grep -q '^decode_workers=1$' "$work/grid-worker4.out"
fi
output=$("$decoder" "$work/grid.avif")
printf '%s\n' "$output" | grep -q '^primary_item_type=grid$'
printf '%s\n' "$output" | grep -q '^grid_rows=2$'
printf '%s\n' "$output" | grep -q '^grid_columns=2$'

perl -e '
    local $/;
    my $data = <>;
    my $offset = index($data, "\x00\x00\x01\x01\x00\x4a\x00\x47");
    die "grid payload not found\n" if $offset < 0;
    substr($data, $offset + 4, 2) = pack("n", 129);
    print $data;
' "$work/grid.avif" > "$work/grid-invalid.avif"
if "$decoder" "$work/grid-invalid.avif" >/dev/null 2>&1; then
    echo "presentation test: invalid grid coverage was accepted" >&2
    exit 1
fi

magick -size 64x64 -depth 8 xc:red -alpha set \
    -channel A -evaluate set 50% +channel "$work/alpha.png"
avifenc --lossless --jobs 1 --codec aom --yuv 444 \
    "$work/alpha.png" "$work/alpha-straight.avif" >/dev/null
render_rgba "$work/alpha-straight.avif" alpha-straight --rgba
avifenc --lossless --jobs 1 --codec aom --yuv 444 --premultiply \
    "$work/alpha.png" "$work/alpha-premultiplied.avif" >/dev/null
render_rgba "$work/alpha-premultiplied.avif" alpha-premultiplied \
    --rgba-premul
output=$("$decoder" "$work/alpha-premultiplied.avif")
printf '%s\n' "$output" | grep -q '^alpha_present=1$'
printf '%s\n' "$output" | grep -q '^alpha_premultiplied=1$'

avifenc --qcolor 80 --jobs 1 --codec aom --yuv 444 --progressive \
    "$work/source.png" "$work/progressive.avif" >/dev/null
perl -e '
    use strict;
    use warnings;
    local $/;
    my $data = <>;
    sub read32 { unpack("N", substr($data, $_[0], 4)) }
    sub write32 { substr($data, $_[0], 4) = pack("N", $_[1]) }
    my $meta = index($data, "meta") - 4;
    my $iprp = index($data, "iprp") - 4;
    my $ipco = index($data, "ipco") - 4;
    my $ipma = index($data, "ipma") - 4;
    my $iloc = index($data, "iloc") - 4;
    my $ispe = index($data, "ispe") - 4;
    die "unexpected progressive layout\n"
        if $meta < 0 || $iprp < 0 || $ipco < 0 ||
           $ipma < 0 || $iloc < 0 || $ispe < 0 ||
           read32($ispe + 12) != 128 || read32($ispe + 16) != 128;
    write32($meta, read32($meta) + 11);
    write32($iprp, read32($iprp) + 11);
    write32($ipco, read32($ipco) + 10);
    my $sizes = ord(substr($data, $iloc + 12, 1));
    my $offset_size = $sizes >> 4;
    die "unexpected iloc offset size\n" if $offset_size != 4;
    my $item = $iloc + 16;
    my $extent_count = unpack("n", substr($data, $item + 4, 2));
    my $extent = $item + 6;
    for (my $i = 0; $i < $extent_count; ++$i) {
        write32($extent, read32($extent) + 11);
        $extent += 8;
    }
    my $lsel = pack("N a4 n", 10, "lsel", 0);
    my $old_ipma_size = read32($ipma);
    my $association_count = ord(substr($data, $ipma + 18, 1));
    die "unexpected ipma association count\n"
        if $association_count >= 127;
    substr($data, $ipma + 18, 1) =
        chr($association_count + 1);
    substr($data, $ipma + $old_ipma_size, 0) = chr(0x80 | 6);
    write32($ipma, $old_ipma_size + 1);
    substr($data, $ipma, 0) = $lsel;
    write32($ispe + 12, 64);
    write32($ispe + 16, 64);
    print $data;
' "$work/progressive.avif" > "$work/progressive-layer0.avif"
"$decoder" --raw "$work/progressive-layer0.avif" \
    "$work/progressive-layer0-ours.yuv" >/dev/null
avifdec --codec aom "$work/progressive-layer0.avif" \
    "$work/progressive-layer0-reference.y4m" >/dev/null
ffmpeg -hide_banner -loglevel error \
    -i "$work/progressive-layer0-reference.y4m" -frames:v 1 \
    -pix_fmt yuv444p -f rawvideo -y \
    "$work/progressive-layer0-reference.yuv"
cmp "$work/progressive-layer0-reference.yuv" \
    "$work/progressive-layer0-ours.yuv"
output=$("$decoder" "$work/progressive-layer0.avif")
printf '%s\n' "$output" | grep -q '^width=64$'
printf '%s\n' "$output" | grep -q '^height=64$'
printf '%s\n' "$output" | grep -q '^render_width=128$'
printf '%s\n' "$output" | grep -q '^render_height=128$'
printf '%s\n' "$output" | grep -q '^layer_count=2$'
printf '%s\n' "$output" | grep -q '^selected_layer=0$'

echo "AVIF presentation test: transforms, alpha, grid, layers, PNG8, and PNG16 exact"
