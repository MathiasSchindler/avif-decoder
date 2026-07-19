#!/bin/sh
set -eu

corpus=${1:-build/fuzz/encoder-corpus}
rm -rf "$corpus"
mkdir -p "$corpus"

printf '\000\000\000\000\000\001\002' > "$corpus/minimum.seed"
printf '\017\017\376\002\017\000\007gradient' > "$corpus/maximum.seed"

for input in tests/encoder-fuzz-regressions/*.seed; do
    if [ -f "$input" ]; then
        cp "$input" "$corpus/regression-$(basename "$input")"
    fi
done

count=$(find "$corpus" -type f -name '*.seed' | wc -l | tr -d ' ')
echo "encoder fuzz seed corpus: $count files in $corpus"