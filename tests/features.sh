#!/bin/sh
set -eu

binary=$1
fixture_dir=${2:-tests/fixtures}
tmp_dir=${TMPDIR:-/tmp}/avifdec-features-$$
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
mkdir -p "$tmp_dir"

output=$($binary "$fixture_dir/delta-q-lf.avif")
printf '%s\n' "$output" | grep -q '^width=128$'
printf '%s\n' "$output" | grep -q '^height=128$'
printf '%s\n' "$output" | grep -q '^segmentation_enabled=0$'
printf '%s\n' "$output" | grep -q '^delta_q_present=1$'
printf '%s\n' "$output" | grep -q '^delta_lf_present=1$'
printf '%s\n' "$output" | grep -q '^entropy_tiles=1$'
printf '%s\n' "$output" | grep -q '^entropy_partition_nodes=240$'
printf '%s\n' "$output" | grep -q '^entropy_blocks=181$'
printf '%s\n' "$output" | grep -q '^entropy_transforms=543$'
printf '%s\n' "$output" | grep -q '^entropy_nonzero_transforms=418$'
printf '%s\n' "$output" | grep -q '^entropy_coefficients=6628$'
printf '%s\n' "$output" | grep -q '^entropy_checksum=0x31f086cbb1d06911$'

output=$($binary "$fixture_dir/segmentation.avif")
printf '%s\n' "$output" | grep -q '^width=64$'
printf '%s\n' "$output" | grep -q '^height=64$'
printf '%s\n' "$output" | grep -q '^segmentation_enabled=1$'
printf '%s\n' "$output" | grep -q '^delta_q_present=0$'
printf '%s\n' "$output" | grep -q '^delta_lf_present=0$'
printf '%s\n' "$output" | grep -q '^entropy_tiles=1$'
printf '%s\n' "$output" | grep -q '^entropy_partition_nodes=93$'
printf '%s\n' "$output" | grep -q '^entropy_blocks=84$'
printf '%s\n' "$output" | grep -q '^entropy_transforms=270$'
printf '%s\n' "$output" | grep -q '^entropy_nonzero_transforms=180$'
printf '%s\n' "$output" | grep -q '^entropy_coefficients=1793$'
printf '%s\n' "$output" | grep -q '^entropy_checksum=0xef1e43e67f7938d3$'

# Flip the low bit of the final byte using only coreutils so the
# self-contained test suite stays free of third-party dependencies.
cp "$fixture_dir/segmentation.avif" "$tmp_dir/corrupt-segmentation.avif"
corrupt_size=$(wc -c < "$tmp_dir/corrupt-segmentation.avif")
corrupt_last=$(od -An -tu1 -j "$((corrupt_size - 1))" -N1 \
  "$tmp_dir/corrupt-segmentation.avif" | tr -d ' ')
printf "$(printf '\\%03o' "$((corrupt_last ^ 1))")" | \
  dd of="$tmp_dir/corrupt-segmentation.avif" bs=1 seek="$((corrupt_size - 1))" \
    conv=notrunc 2>/dev/null
if output=$($binary "$tmp_dir/corrupt-segmentation.avif" 2>&1); then
    echo 'feature fixture test: corrupt segmentation tile was accepted' >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q '^avifdec: invalid data '

echo 'AV1 feature fixture test: segmentation and delta state ok'
