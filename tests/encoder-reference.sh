#!/bin/sh
set -eu

work=build/encoder-reference
aomenc_path=$(command -v aomenc)
aom_prefix=$(dirname "$(dirname "$aomenc_path")")
libaom=$aom_prefix/lib/libaom.a
compiler=${CC:-cc}

[ -f "$libaom" ] || {
    echo "encoder transform reference library not found: $libaom" >&2
    exit 1
}
mkdir -p "$work"
"$compiler" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
    -Isrc -Isrc/shared -Isrc/decoder -Isrc/codec \
    tests/encoder_transform_reference.c \
    src/encoder/av1_transform_write.c src/encoder/av1_symbol_write.c \
    src/encoder/av1_transform_forward.c \
    src/codec/av1_recon.c src/codec/av1_coeff.c src/codec/av1_symbol.c \
    src/codec/av1_cdf.c src/codec/av1_dsp.c src/base.c \
    "$libaom" -lm -lpthread \
    -o "$work/transform-reference"
"$work/transform-reference"
echo "encoder transform reference: 1003 libaom vectors exact"