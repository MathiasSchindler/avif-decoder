#!/bin/sh
set -eu

binary=$1
baseline=$2
actual=${TMPDIR:-/tmp}/avifenc-scorecard-$$.jsonl
trap 'rm -f "$actual"' EXIT HUP INT TERM

"$binary" --stable-json > "$actual"
if ! cmp "$baseline" "$actual"; then
    diff -u "$baseline" "$actual" >&2 || true
    echo 'encoder scorecard differs from the checked-in baseline' >&2
    exit 1
fi

cases=$(wc -l < "$actual" | tr -d ' ')
echo "encoder scorecard: $cases deterministic cases match baseline"
