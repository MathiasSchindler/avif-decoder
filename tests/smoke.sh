#!/bin/sh
set -eu

binary=$1
tmp_dir=${TMPDIR:-/tmp}/avifdec-smoke-$$
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
mkdir -p "$tmp_dir"

printf '\000\000\000\030ftypavif\000\000\000\000avifmiaf' > "$tmp_dir/minimal.avif"
printf '\000\000\000\050meta\000\000\000\000' >> "$tmp_dir/minimal.avif"
printf '\000\000\000\014hdlrDATA' >> "$tmp_dir/minimal.avif"
printf '\000\000\000\020iprp\000\000\000\010ipco' >> "$tmp_dir/minimal.avif"
printf '\000\000\000\000mdat' >> "$tmp_dir/minimal.avif"
printf '<!doctype html>' > "$tmp_dir/not-avif"

printf '\000\000\000\030ftypmif1\000\000\000\000avifmiaf' > "$tmp_dir/idat.avif"
printf '\000\000\000\010mdat' >> "$tmp_dir/idat.avif"
printf '\000\000\000\024meta\000\000\000\000\000\000\000\010idat' >> "$tmp_dir/idat.avif"

output=$($binary --boxes "$tmp_dir/minimal.avif")
printf '%s\n' "$output" | grep -q '^ftyp offset=0 size=24$'
printf '%s\n' "$output" | grep -q '^meta offset=24 size=40$'
printf '%s\n' "$output" | grep -q '^  hdlr offset=36 size=12$'
printf '%s\n' "$output" | grep -q '^    ipco offset=56 size=8$'
printf '%s\n' "$output" | grep -q '^mdat offset=64 size=8$'
printf '%s\n' "$output" | grep -q '^major_brand=avif$'
printf '%s\n' "$output" | grep -q '^compatible_brand=miaf$'
printf '%s\n' "$output" | grep -q '^boxes=6$'

output=$($binary --boxes "$tmp_dir/minimal.avif" --workers 0)
printf '%s\n' "$output" | grep -q '^boxes=6$'
output=$($binary --workers 1 --boxes "$tmp_dir/minimal.avif")
printf '%s\n' "$output" | grep -q '^boxes=6$'
if $binary --boxes "$tmp_dir/minimal.avif" \
        --workers 1 --workers 1 >/dev/null 2>&1; then
    echo 'duplicate worker option was accepted' >&2
    exit 1
fi
if $binary --boxes "$tmp_dir/minimal.avif" \
        --workers >/dev/null 2>&1; then
    echo 'worker option without a count was accepted' >&2
    exit 1
fi

output=$($binary --boxes "$tmp_dir/idat.avif")
printf '%s\n' "$output" | grep -q '^mdat offset=24 size=8$'
printf '%s\n' "$output" | grep -q '^meta offset=32 size=20$'
printf '%s\n' "$output" | grep -q '^  idat offset=44 size=8$'
printf '%s\n' "$output" | grep -q '^major_brand=mif1$'
printf '%s\n' "$output" | grep -q '^compatible_brand=avif$'
printf '%s\n' "$output" | grep -q '^boxes=4$'

if $binary "$tmp_dir/minimal.avif" > /dev/null 2>&1; then
    echo 'incomplete AVIF item metadata was accepted by query mode' >&2
    exit 1
fi

if $binary "$tmp_dir/not-avif" > /dev/null 2>&1; then
    echo 'invalid input was accepted' >&2
    exit 1
fi

printf '\000\000\000\030ftypavif\000\000\000\000avifmiaf\000\000\000\001uuid' > "$tmp_dir/truncated-large-size.avif"
printf '\000\000\000\030ftypavif\000\000\000\000avifmiaf\000\000\000\004free' > "$tmp_dir/short-box.avif"
printf '\000\000\000\030ftypavif\000\000\000\000avifmiaf\000\000\000\020meta' > "$tmp_dir/truncated-meta.avif"
for malformed in "$tmp_dir/truncated-large-size.avif" "$tmp_dir/short-box.avif" "$tmp_dir/truncated-meta.avif"; do
    if $binary "$malformed" > /dev/null 2>&1; then
        echo "malformed input was accepted: $malformed" >&2
        exit 1
    fi
done

test -z "$(nm -u "$binary")"
case $(uname -s) in
    Darwin)
        if test -n "$(otool -L "$binary" | sed '1d')"; then
            echo 'freestanding binary has a dynamic library dependency' >&2
            exit 1
        fi
        ;;
    *)
        if readelf -l "$binary" | grep -q INTERP; then
            echo 'freestanding binary has a dynamic interpreter' >&2
            exit 1
        fi
        ;;
esac

echo 'freestanding smoke test: ok'