#!/bin/sh
set -eu

encoder=$1
decoder=$2
work=build/encoder-interoperability
rm -rf "$work"
mkdir -p "$work"

for tool in ffmpeg ffprobe avifdec aomdec; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "encoder interoperability test requires $tool" >&2
        exit 1
    }
done

write_fixture() {
    output=$1
    width=$2
    height=$3
    pattern=$4
    luma=$((width * height))
    chroma=$((luma / 4))
    : > "$output"

    if test "$pattern" = row; then
        total=$((luma + 2 * chroma))
        dd if=/dev/zero bs="$total" count=1 2>/dev/null |
            LC_ALL=C tr '\000' '\200' > "$output"
        return
    fi

    index=0
    while test "$index" -lt "$luma"; do
        column=$((index % width))
        row=$((index / width))
        case $pattern in
            flat) value=128 ;;
            edge)
                if test "$column" -lt $((width / 2)); then
                    value=16
                else
                    value=235
                fi
                ;;
            hedge)
                if test "$row" -lt $((height / 2)); then
                    value=16
                else
                    value=235
                fi
                ;;
            detail|wide) value=$(((column * 29 + row * 47 + index * 3) % 220 + 16)) ;;
        esac
        octal=$(printf '%03o' "$value")
        printf "\\$octal" >> "$output"
        index=$((index + 1))
    done

    plane=0
    while test "$plane" -lt 2; do
        index=0
        while test "$index" -lt "$chroma"; do
            case $pattern:$plane in
                detail:0|wide:0) value=$(((index * 37) % 225 + 16)) ;;
                detail:1|wide:1) value=$(((index * 53) % 225 + 16)) ;;
                *) value=128 ;;
            esac
            octal=$(printf '%03o' "$value")
            printf "\\$octal" >> "$output"
            index=$((index + 1))
        done
        plane=$((plane + 1))
    done
}

check_metadata() {
    input=$1
    width=$2
    height=$3
    metadata=$work/metadata
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name,profile,width,height,pix_fmt,color_range,color_space,color_transfer,color_primaries \
        -of default=noprint_wrappers=1 "$input" | sort > "$metadata"
    for expected in \
            codec_name=av1 profile=Main width=$width height=$height \
            pix_fmt=yuv420p color_range=tv color_space=bt709 \
            color_transfer=bt709 color_primaries=bt709; do
        grep -qx "$expected" "$metadata" || {
            echo "$input: missing ffprobe metadata: $expected" >&2
            exit 1
        }
    done
}

codecs=
probe=$work/probe.yuv
write_fixture "$probe" 2 2 flat
"$encoder" 2 2 "$probe" "$work/probe.avif"
for codec in aom dav1d libgav1; do
    if avifdec --codec "$codec" "$work/probe.avif" \
            "$work/probe-$codec.y4m" >/dev/null 2>&1; then
        codecs="$codecs $codec"
    fi
done
test -n "$codecs" || {
    echo 'encoder interoperability test requires a usable libavif decoder' >&2
    exit 1
}

fixtures=0
comparisons=0
for specification in flat:2:2:1:2 edge:32:32:128:1 hedge:32:32:128:1 detail:64:48:224:0 wide:4160:2:128:2 row:2242:4098:255:2; do
    pattern=${specification%%:*}
    remainder=${specification#*:}
    width=${remainder%%:*}
    remainder=${remainder#*:}
    height=${remainder%%:*}
    remainder=${remainder#*:}
    quantizer=${remainder%%:*}
    speed=${remainder#*:}
    prefix=$work/$pattern-${width}x${height}-q$quantizer-s$speed
    source=$prefix-input.yuv
    encoded=$prefix.avif
    repeated=$prefix-repeat.avif
    ours=$prefix-ours.yuv

    write_fixture "$source" "$width" "$height" "$pattern"
    "$encoder" --quantizer "$quantizer" --speed "$speed" \
        "$width" "$height" "$source" "$encoded"
    "$encoder" --quantizer "$quantizer" --speed "$speed" \
        "$width" "$height" "$source" "$repeated"
    cmp "$encoded" "$repeated" || {
        echo "$encoded: encoder output is not deterministic" >&2
        exit 1
    }
    if test "$pattern" = wide || test "$pattern" = row; then
        "$encoder" --quantizer "$quantizer" --speed "$speed" --workers 2 \
            "$width" "$height" "$source" "$prefix-workers.avif"
        cmp "$encoded" "$prefix-workers.avif" || {
            echo "$encoded: worker count changed encoder output" >&2
            exit 1
        }
    fi
    check_metadata "$encoded" "$width" "$height"
    "$decoder" --raw "$encoded" "$ours" >/dev/null

    ffmpeg -hide_banner -loglevel error -i "$encoded" -frames:v 1 \
        -pix_fmt yuv420p -f rawvideo -y "$prefix-ffmpeg.yuv"
    cmp "$ours" "$prefix-ffmpeg.yuv" || {
        echo "$encoded: planar YUV differs from ffmpeg" >&2
        exit 1
    }
    comparisons=$((comparisons + 1))

    ffmpeg -hide_banner -loglevel error -i "$encoded" -map 0:v:0 \
        -c copy -f obu -y "$prefix.obu"
    aomdec --limit=1 --rawvideo --i420 -o "$prefix-libaom.yuv" \
        "$prefix.obu" >/dev/null 2>&1
    cmp "$ours" "$prefix-libaom.yuv" || {
        echo "$encoded: planar YUV differs from libaom" >&2
        exit 1
    }
    comparisons=$((comparisons + 1))

    for codec in $codecs; do
        avifdec --codec "$codec" "$encoded" "$prefix-$codec.y4m" >/dev/null
        ffmpeg -hide_banner -loglevel error -i "$prefix-$codec.y4m" \
            -frames:v 1 -pix_fmt yuv420p -f rawvideo -y \
            "$prefix-$codec.yuv"
        cmp "$ours" "$prefix-$codec.yuv" || {
            echo "$encoded: planar YUV differs from libavif $codec" >&2
            exit 1
        }
        comparisons=$((comparisons + 1))
    done
    fixtures=$((fixtures + 1))
done

for feature in lossless goal5; do
    prefix=$work/$feature-32x32
    source=$prefix-input.yuv
    encoded=$prefix.avif
    ours=$prefix-ours.yuv

    write_fixture "$source" 32 32 detail
    if test "$feature" = lossless; then
        "$encoder" --quantizer 0 32 32 "$source" "$encoded"
    else
        "$encoder" --quantizer 96 --y-dc-delta -7 --u-dc-delta 3 \
            --u-ac-delta -5 --v-dc-delta 9 --v-ac-delta 4 \
            --qmatrix 7 --aq activity --aq-strength 12 \
            32 32 "$source" "$encoded"
    fi
    "$decoder" --raw "$encoded" "$ours" >/dev/null
    if test "$feature" = lossless; then
        cmp "$source" "$ours" || {
            echo "$encoded: lossless output differs from source" >&2
            exit 1
        }
    fi
    ffmpeg -hide_banner -loglevel error -i "$encoded" -frames:v 1 \
        -pix_fmt yuv420p -f rawvideo -y "$prefix-ffmpeg.yuv"
    cmp "$ours" "$prefix-ffmpeg.yuv"
    ffmpeg -hide_banner -loglevel error -i "$encoded" -map 0:v:0 \
        -c copy -f obu -y "$prefix.obu"
    aomdec --limit=1 --rawvideo --i420 -o "$prefix-libaom.yuv" \
        "$prefix.obu" >/dev/null 2>&1
    cmp "$ours" "$prefix-libaom.yuv"
    comparisons=$((comparisons + 2))
    for codec in $codecs; do
        avifdec --codec "$codec" "$encoded" "$prefix-$codec.y4m" >/dev/null
        ffmpeg -hide_banner -loglevel error -i "$prefix-$codec.y4m" \
            -frames:v 1 -pix_fmt yuv420p -f rawvideo -y \
            "$prefix-$codec.yuv"
        cmp "$ours" "$prefix-$codec.yuv"
        comparisons=$((comparisons + 1))
    done
    fixtures=$((fixtures + 1))
done

echo "encoder interoperability: $fixtures fixtures, $comparisons exact external decodes, codecs:$codecs"
