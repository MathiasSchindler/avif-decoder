#!/bin/sh
set -eu

decoder=${1:-build/$(uname -m)/avifdec}
work=build/reference-test
rm -rf "$work"
mkdir -p "$work"

for tool in ffmpeg avifenc avifdec aomenc perl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "reference test requires $tool" >&2
        exit 1
    }
done

check_case() {
    name=$1
    pixel_format=$2
    mode=$3
    source="$work/$name.y4m"
    encoded="$work/$name.avif"
    reference="$work/$name-reference.y4m"
    reference_raw="$work/$name-reference.yuv"
    actual_raw="$work/$name-actual.yuv"
    case $pixel_format in
        yuv444*) cicp=0/2/0 ;;
        *) cicp=2/2/6 ;;
    esac

    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i 'testsrc2=size=32x32:rate=1' -frames:v 1 \
        -pix_fmt "$pixel_format" -strict -1 -f yuv4mpegpipe -y "$source"
    if [ "$mode" = lossless ]; then
        avifenc --lossless --jobs 1 --speed 6 --codec aom --cicp "$cicp" \
            "$source" "$encoded" >/dev/null
    elif [ "$mode" = unfiltered ]; then
        avifenc --qcolor 70 --jobs 1 --speed 6 --codec aom --cicp "$cicp" \
            -a loopfilter-control=0 -a enable-cdef=0 \
            -a enable-restoration=0 "$source" "$encoded" >/dev/null
    else
        avifenc --qcolor 70 --jobs 1 --speed 6 --codec aom --cicp "$cicp" \
            "$source" "$encoded" >/dev/null
    fi
    avifdec --codec aom "$encoded" "$reference" >/dev/null
    ffmpeg -hide_banner -loglevel error -i "$reference" -frames:v 1 \
        -pix_fmt "$pixel_format" -f rawvideo -y "$reference_raw"
    expected=$(perl tools/y4m-reconstruction-checksum.pl "$reference")
    checkpoint=reconstruction
    if [ "$mode" = filtered ]; then checkpoint=restoration; fi
    actual=$($decoder --raw "$encoded" "$actual_raw" |
        sed -n "s/^${checkpoint}_checksum=//p")
    [ "$actual" = "$expected" ] || {
        echo "$name: expected $expected, got $actual" >&2
        exit 1
    }
    cmp "$reference_raw" "$actual_raw" || {
        echo "$name: final planar YUV differs from libaom" >&2
        exit 1
    }
    if [ "$mode" = filtered ]; then
        threaded_raw="$work/$name-threaded.yuv"
        threaded_output=$(
            "$decoder" --workers 4 --raw "$encoded" "$threaded_raw"
        )
        threaded=$(printf '%s\n' "$threaded_output" |
            sed -n "s/^${checkpoint}_checksum=//p")
        [ "$threaded" = "$actual" ] || {
            echo "$name: threaded checksum differs" >&2
            exit 1
        }
        cmp "$actual_raw" "$threaded_raw" || {
            echo "$name: threaded planar YUV differs" >&2
            exit 1
        }
        if [ "$(uname -s)" = Linux ] &&
            [ "$(uname -m)" = x86_64 ]; then
            workers=$(printf '%s\n' "$threaded_output" |
                sed -n 's/^decode_workers=//p')
            [ "$workers" = 4 ] || {
                echo "$name: expected 4 decode workers, got $workers" >&2
                exit 1
            }
        fi
    fi
    echo "$name: $actual"
}

check_case lossless-444-8 yuv444p lossless
check_case lossless-444-10 yuv444p10le lossless
check_case lossless-444-12 yuv444p12le lossless
check_case lossy-444-8 yuv444p unfiltered
check_case lossy-420-8 yuv420p unfiltered
check_case lossy-420-10 yuv420p10le unfiltered
check_case lossy-420-12 yuv420p12le unfiltered
check_case filtered-mono-8 gray filtered
check_case filtered-422-8 yuv422p filtered
check_case filtered-444-8 yuv444p filtered
check_case filtered-420-8 yuv420p filtered
check_case filtered-420-10 yuv420p10le filtered
check_case filtered-420-12 yuv420p12le filtered

tile_source="$work/tiled.y4m"
tile_avif="$work/tiled.avif"
tile_serial="$work/tiled-serial.yuv"
tile_threaded="$work/tiled-threaded.yuv"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=256x256:rate=1' -frames:v 1 -pix_fmt yuv420p \
    -f yuv4mpegpipe -y "$tile_source"
avifenc --qcolor 70 --jobs 1 --speed 6 --codec aom --cicp 2/2/6 \
    -a tile-columns=1 -a tile-rows=1 \
    "$tile_source" "$tile_avif" >/dev/null
"$decoder" --workers 1 --raw "$tile_avif" "$tile_serial" \
    >"$work/tiled-serial.out"
"$decoder" --workers 4 --raw "$tile_avif" "$tile_threaded" \
    >"$work/tiled-threaded.out"
cmp "$tile_serial" "$tile_threaded" || {
    echo "tiled: threaded planar YUV differs" >&2
    exit 1
}
sed '/^workspace_required=/d; /^decode_workers=/d' \
    "$work/tiled-serial.out" >"$work/tiled-serial-normalized.out"
sed '/^workspace_required=/d; /^decode_workers=/d' \
    "$work/tiled-threaded.out" >"$work/tiled-threaded-normalized.out"
cmp "$work/tiled-serial-normalized.out" \
    "$work/tiled-threaded-normalized.out" || {
    echo "tiled: threaded trace differs" >&2
    exit 1
}
grep -q '^tile_columns=2$' "$work/tiled-threaded.out"
grep -q '^tile_rows=2$' "$work/tiled-threaded.out"
echo "tiled: exact serial/threaded planar YUV and trace"

part6_source="$work/part6-active.y4m"
part6_ivf="$work/part6-active.ivf"
part6_avif="$work/part6-active.avif"
part6_reference="$work/part6-active-reference.y4m"
part6_reference_raw="$work/part6-active-reference.yuv"
part6_actual_raw="$work/part6-active-actual.yuv"
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=128x96:rate=1' -frames:v 1 -pix_fmt yuv420p \
    -f yuv4mpegpipe -y "$part6_source"
aomenc --ivf --codec=av1 --cpu-used=0 --end-usage=q --cq-level=45 \
    --limit=1 --superres-mode=1 --superres-denominator=12 \
    --superres-kf-denominator=12 -o "$part6_ivf" "$part6_source" >/dev/null
ffmpeg -hide_banner -loglevel error -i "$part6_ivf" -frames:v 1 \
    -c:v copy -f avif -y "$part6_avif"
avifdec --codec aom "$part6_avif" "$part6_reference" >/dev/null
ffmpeg -hide_banner -loglevel error -i "$part6_reference" -frames:v 1 \
    -pix_fmt yuv420p -f rawvideo -y "$part6_reference_raw"
$decoder --raw "$part6_avif" "$part6_actual_raw" >/dev/null
cmp "$part6_reference_raw" "$part6_actual_raw" || {
    echo "part6-active: final planar YUV differs from libaom" >&2
    exit 1
}
echo "part6-active: exact final planar YUV"

echo "trusted reconstruction and final planar YUV test: ok"