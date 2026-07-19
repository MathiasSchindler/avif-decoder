#!/bin/sh
set -eu

binary=$1
tmp_dir=${TMPDIR:-/tmp}/avifenc-test-$$
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
mkdir -p "$tmp_dir"

output=$($binary --version)
test "$output" = 'avifenc 0.1.0'
output=$($binary --help)
case "$output" in
    *'usage: avifenc [--quantizer 0..255] WIDTH HEIGHT INPUT.yuv OUTPUT.avif'*) ;;
    *) echo 'encoder help is missing the command contract' >&2; exit 1 ;;
esac

printf '\020\040\060\100\200\200' > "$tmp_dir/tiny-2x2.yuv"

if output=$($binary 2 2 "$tmp_dir/tiny-2x2.yuv" \
        "$tmp_dir/tiny.avif" 2>&1); then
    echo 'WP1 encoder unexpectedly produced an AVIF file' >&2
    exit 1
else
    exit_code=$?
fi
test "$exit_code" -eq 3
case "$output" in
    *'unsupported feature: encoder implementation'*) ;;
    *) echo 'valid input did not reach the implementation boundary' >&2; exit 1 ;;
esac
test ! -e "$tmp_dir/tiny.avif"

if output=$($binary 3 2 "$tmp_dir/tiny-2x2.yuv" \
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

if output=$($binary --quantizer 256 2 2 "$tmp_dir/tiny-2x2.yuv" \
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