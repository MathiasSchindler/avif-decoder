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
    -Isrc -Isrc/shared tests/encoder_transform_reference.c \
    src/encoder/av1_transform_write.c src/encoder/av1_symbol_write.c \
    src/av1_recon.c src/av1_coeff.c src/av1_symbol.c \
    src/shared/av1_cdf.c src/base.c "$libaom" -lm -lpthread \
    -o "$work/transform-reference"
"$work/transform-reference"
echo "encoder transform reference: 1003 libaom vectors exact"