#!/bin/sh
set -eu

binary=$1
decoder=$2
tmp_dir=${TMPDIR:-/tmp}/avifenc-test-$$
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
mkdir -p "$tmp_dir"

output=$($binary --version)
test "$output" = 'avifenc 0.2.0'
output=$($binary --help)
case "$output" in
    *'usage: avifenc [--quantizer 0..255] [--speed 0..2] [--workers 1..32]'*) ;;
    *) echo 'encoder help is missing the command contract' >&2; exit 1 ;;
esac
case "$output" in
    *'INPUT.png|jpg|jpeg OUTPUT.avif'*) ;;
    *) echo 'encoder help is missing the image input contract' >&2; exit 1 ;;
esac

write_fixture() {
    fixture=$1
    width=$2
    height=$3
    pattern=$4
    luma=$((width * height))
    chroma=$((width * height / 4))
    : > "$fixture"
    index=0
    while test "$index" -lt "$luma"; do
        case $pattern in
            flat) printf '\200' >> "$fixture" ;;
            ramp)
                case $((index % 4)) in
                    0) printf '\020' >> "$fixture" ;;
                    1) printf '\100' >> "$fixture" ;;
                    2) printf '\200' >> "$fixture" ;;
                    3) printf '\360' >> "$fixture" ;;
                esac
                ;;
            sharp)
                if test $((index % width)) -lt $((width / 2)); then
                    printf '\000' >> "$fixture"
                else
                    printf '\377' >> "$fixture"
                fi
                ;;
            chroma) printf '\200' >> "$fixture" ;;
        esac
        index=$((index + 1))
    done
    index=0
    while test "$index" -lt "$chroma"; do
        if test "$pattern" = chroma && test $((index % 2)) -eq 0; then
            printf '\020' >> "$fixture"
        else
            printf '\200' >> "$fixture"
        fi
        index=$((index + 1))
    done
    index=0
    while test "$index" -lt "$chroma"; do
        if test "$pattern" = chroma && test $((index % 2)) -eq 0; then
            printf '\360' >> "$fixture"
        else
            printf '\200' >> "$fixture"
        fi
        index=$((index + 1))
    done
}

for fixture in flat:2:2 ramp:10:6 sharp:10:6 chroma:10:6; do
    pattern=${fixture%%:*}
    dimensions=${fixture#*:}
    width=${dimensions%%:*}
    height=${dimensions#*:}
    yuv="$tmp_dir/$pattern-${width}x${height}.yuv"
    avif="$tmp_dir/$pattern-${width}x${height}.avif"
    write_fixture "$yuv" "$width" "$height" "$pattern"
    $binary "$width" "$height" "$yuv" "$avif"
    test -s "$avif"
    $decoder "$avif" >/dev/null
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -hide_banner -loglevel error -i "$avif" -f null -
    fi
done

wide_yuv="$tmp_dir/wide-4160x2.yuv"
wide_serial="$tmp_dir/wide-serial.avif"
wide_parallel="$tmp_dir/wide-parallel.avif"
write_fixture "$wide_yuv" 4160 2 ramp
$binary --speed 2 4160 2 "$wide_yuv" "$wide_serial"
$binary --speed 2 --workers 2 4160 2 "$wide_yuv" "$wide_parallel"
cmp "$wide_serial" "$wide_parallel"
output=$($decoder "$wide_parallel")
printf '%s\n' "$output" | grep -q '^width=4160$'
printf '%s\n' "$output" | grep -q '^height=2$'
printf '%s\n' "$output" | grep -q '^tile_columns=2$'
printf '%s\n' "$output" | grep -q '^tile_rows=1$'

for source in images/image-check/tribu.png images/chalk.jpg; do
    name=${source##*/}
    avif="$tmp_dir/$name.avif"
    $binary --quantizer 255 --speed 2 "$source" "$avif"
    test -s "$avif"
    $decoder "$avif" >/dev/null
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -hide_banner -loglevel error -i "$avif" -f null -
    fi
done

cp "$tmp_dir/flat-2x2.yuv" "$tmp_dir/trailing.yuv"
printf '\000' >> "$tmp_dir/trailing.yuv"
if $binary 2 2 "$tmp_dir/trailing.yuv" "$tmp_dir/trailing.avif" \
        >/dev/null 2>&1; then
    echo 'trailing YUV input was accepted' >&2
    exit 1
fi
test ! -e "$tmp_dir/trailing.avif"

printf '\020\040\060' > "$tmp_dir/truncated.yuv"
if $binary 2 2 "$tmp_dir/truncated.yuv" "$tmp_dir/truncated.avif" \
        >/dev/null 2>&1; then
    echo 'truncated YUV input was accepted' >&2
    exit 1
fi
test ! -e "$tmp_dir/truncated.avif"

if output=$($binary 3 2 "$tmp_dir/flat-2x2.yuv" \
        "$tmp_dir/odd.avif" 2>&1); then
    echo 'odd encoder dimensions were accepted' >&2
    exit 1
else
    exit_code=$?
fi
test "$exit_code" -eq 2
case "$output" in
    *'invalid argument: dimensions'*) ;;
    *) echo 'odd dimensions returned the wrong error' >&2; exit 1 ;;
esac

if output=$($binary --workers 0 2 2 "$tmp_dir/flat-2x2.yuv" \
        "$tmp_dir/workers.avif" 2>&1); then
    echo 'zero encoder workers were accepted' >&2
    exit 1
else
    exit_code=$?
fi
test "$exit_code" -eq 2
case "$output" in
    *'invalid worker count'*) ;;
    *) echo 'zero workers returned the wrong error' >&2; exit 1 ;;
esac

if output=$($binary --quantizer 256 2 2 "$tmp_dir/flat-2x2.yuv" \
        "$tmp_dir/quantizer.avif" 2>&1); then
    echo 'invalid encoder quantizer was accepted' >&2
    exit 1
else
    exit_code=$?
fi
test "$exit_code" -eq 2
case "$output" in
    *'invalid argument: quantizer'*) ;;
    *) echo 'invalid quantizer returned the wrong error' >&2; exit 1 ;;
esac

$binary --quantizer 0 2 2 "$tmp_dir/flat-2x2.yuv" \
    "$tmp_dir/lossless.avif"
$decoder "$tmp_dir/lossless.avif" >/dev/null

$binary --quantizer 96 --y-dc-delta -7 --u-dc-delta 3 \
    --u-ac-delta -5 --v-dc-delta 9 --v-ac-delta 4 \
    --qmatrix 7 --aq activity --aq-strength 12 \
    10 6 "$tmp_dir/ramp-10x6.yuv" "$tmp_dir/goal5.avif"
$decoder "$tmp_dir/goal5.avif" >/dev/null

if $binary --target-quality 9000 --target-size 500 \
        2 2 "$tmp_dir/flat-2x2.yuv" "$tmp_dir/conflicting.avif" \
        >/dev/null 2>&1; then
    echo 'conflicting rate targets were accepted' >&2
    exit 1
fi

if output=$($binary --speed 3 2 2 "$tmp_dir/flat-2x2.yuv" \
        "$tmp_dir/speed.avif" 2>&1); then
    echo 'invalid encoder speed was accepted' >&2
    exit 1
else
    exit_code=$?
fi
test "$exit_code" -eq 2
case "$output" in
    *'invalid argument: options'*) ;;
    *) echo 'invalid speed returned the wrong error' >&2; exit 1 ;;
esac

if $binary >/dev/null 2>&1; then
    echo 'missing encoder arguments were accepted' >&2
    exit 1
fi

test -z "$(nm -u "$binary")"
case $(uname -s) in
    Darwin)
        if test -n "$(otool -L "$binary" | sed '1d')"; then
            echo 'freestanding encoder has a dynamic library dependency' >&2
            exit 1
        fi
        ;;
    *)
        if readelf -l "$binary" | grep -q INTERP; then
            echo 'freestanding encoder has a dynamic interpreter' >&2
            exit 1
        fi
        ;;
esac

echo 'encoder CLI tests: ok'