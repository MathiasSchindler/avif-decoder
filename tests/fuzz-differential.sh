#!/bin/sh
set -eu

decoder=${1:-build/$(uname -m)/avifdec}
corpus=${2:-build/fuzz/corpus}
work=build/fuzz/differential
rm -rf "$work"
mkdir -p "$work"

for tool in avifdec ffmpeg ffprobe; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "fuzz differential replay requires $tool" >&2
        exit 1
    }
done
[ -x "$decoder" ] || {
    echo "fuzz differential decoder not found: $decoder" >&2
    exit 1
}

probe=
for input in "$corpus"/*.avif; do
    if [ -f "$input" ] && "$decoder" --raw "$input" "$work/probe.yuv" \
            >/dev/null 2>&1; then
        probe=$input
        break
    fi
done
[ -n "$probe" ] || {
    echo "fuzz differential replay: skipped (no accepted still-image seeds)"
    exit 0
}

codecs=
for codec in aom dav1d libgav1; do
    if avifdec --codec "$codec" "$probe" "$work/probe-$codec.y4m" \
            >/dev/null 2>&1; then
        codecs="$codecs $codec"
    fi
done
[ -n "$codecs" ] || {
    echo "fuzz differential replay: no usable libavif codec" >&2
    exit 1
}

files=0
accepted=0
comparisons=0
for input in "$corpus"/*.avif; do
    [ -f "$input" ] || continue
    files=$((files + 1))
    tag=$(printf '%04d' "$files")
    ours="$work/$tag-ours.yuv"
    if ! "$decoder" --raw "$input" "$ours" \
            >"$work/$tag-ours.out" 2>"$work/$tag-ours.err"; then
        continue
    fi
    accepted=$((accepted + 1))
    for codec in $codecs; do
        reference_y4m="$work/$tag-$codec.y4m"
        reference_raw="$work/$tag-$codec.yuv"
        if ! avifdec --codec "$codec" "$input" "$reference_y4m" \
                >"$work/$tag-$codec.out" 2>"$work/$tag-$codec.err"; then
            echo "$input: ours accepted but libavif $codec rejected" >&2
            exit 1
        fi
        pixel_format=$(ffprobe -v error -select_streams v:0 \
            -show_entries stream=pix_fmt -of default=nw=1:nk=1 \
            "$reference_y4m" | sed 's/^yuva/yuv/')
        ffmpeg -hide_banner -loglevel error -i "$reference_y4m" \
            -frames:v 1 -pix_fmt "$pixel_format" -f rawvideo \
            -y "$reference_raw"
        cmp "$ours" "$reference_raw" || {
            echo "$input: planar YUV differs from libavif $codec" >&2
            exit 1
        }
        comparisons=$((comparisons + 1))
    done
done

echo "fuzz differential replay: $files seeds, $accepted accepted, $comparisons exact reference decodes"
