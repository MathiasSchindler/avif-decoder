#!/bin/sh
set -eu

decoder=${1:-build/$(uname -m)/avifdec}
work=build/differential-test
rm -rf "$work"
mkdir -p "$work"

for tool in avifdec ffmpeg ffprobe; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "differential test requires $tool" >&2
        exit 1
    }
done
[ -x "$decoder" ] || {
    echo "differential test decoder not found: $decoder" >&2
    exit 1
}

find images tests/fixtures -type f -name '*.avif' -print | sort > "$work/files"
probe=$(sed -n '1p' "$work/files")
[ -n "$probe" ] || {
    echo "AVIF differential test: skipped (no checked-in AVIF files)"
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
    echo "differential test: libavif has no usable reference codec" >&2
    exit 1
}

files=0
accepted=0
rejected=0
comparisons=0
while IFS= read -r input; do
    files=$((files + 1))
    tag=$(printf '%03d' "$files")
    ours="$work/$tag-ours.yuv"
    if "$decoder" --raw "$input" "$ours" >"$work/$tag-ours.out" \
            2>"$work/$tag-ours.err"; then
        ours_status=ok
        accepted=$((accepted + 1))
    else
        ours_status=fail
        rejected=$((rejected + 1))
    fi

    for codec in $codecs; do
        reference_y4m="$work/$tag-$codec.y4m"
        reference_raw="$work/$tag-$codec.yuv"
        if avifdec --codec "$codec" "$input" "$reference_y4m" \
                >"$work/$tag-$codec.out" 2>"$work/$tag-$codec.err"; then
            reference_status=ok
        else
            reference_status=fail
        fi
        if [ "$ours_status" != "$reference_status" ]; then
            echo "$input: acceptance differs (ours=$ours_status, $codec=$reference_status)" >&2
            exit 1
        fi
        if [ "$ours_status" = fail ]; then
            continue
        fi

        pixel_format=$(ffprobe -v error -select_streams v:0 \
            -show_entries stream=pix_fmt -of default=nw=1:nk=1 \
            "$reference_y4m")
        ffmpeg -hide_banner -loglevel error -i "$reference_y4m" \
            -frames:v 1 -pix_fmt "$pixel_format" -f rawvideo \
            -y "$reference_raw"
        cmp "$ours" "$reference_raw" || {
            echo "$input: planar YUV differs from libavif $codec" >&2
            exit 1
        }
        comparisons=$((comparisons + 1))
    done
done < "$work/files"

echo "AVIF differential test: $files files, $accepted accepted, $rejected rejected, $comparisons exact reference decodes"