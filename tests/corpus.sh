#!/bin/sh
set -eu

binary=$1
corpus_dir=${2:-images/avif}

if [ ! -d "$corpus_dir" ]; then
    echo "AVIF corpus test: skipped ($corpus_dir not found)"
    exit 0
fi

count=0
traced=0
for file in "$corpus_dir"/*.avif; do
    if [ ! -f "$file" ]; then
        continue
    fi
    output=$($binary "$file")
    printf '%s\n' "$output" | grep -q '^major_brand='
    printf '%s\n' "$output" | grep -q '^compatible_brand=avif$'
    printf '%s\n' "$output" | grep -q '^boxes=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^width=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^height=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^bit_depth=\(8\|10\|12\)$'
    printf '%s\n' "$output" | grep -q '^payload_size=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^obus=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^tile_columns=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^tile_rows=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^entropy_tiles=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^entropy_partition_nodes=[0-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^entropy_blocks=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^entropy_transforms=[1-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^entropy_nonzero_transforms=[0-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^entropy_coefficients=[0-9][0-9]*$'
    printf '%s\n' "$output" | grep -q '^entropy_checksum=0x[0-9a-f][0-9a-f]*$'
    traced=$((traced + 1))
    count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
    echo "AVIF corpus test: skipped (no .avif files in $corpus_dir)"
    exit 0
fi

echo "AVIF corpus test: $count files ok ($traced strict traces)"