#!/bin/sh
set -eu

decoder=${1:-build/$(uname -m)/avifdec}
obu_trace=${2:-build/$(uname -m)/obu-trace}
work=build/reference-block
source_dir=$work/aom-src
build_dir=$work/aom-build
patch=tools/libaom-reference-v3.13.1.patch
vendor_source=vendor/libaom
corpus_dir=images/avif
expected_commit=d772e334cc724105040382a977ebb10dfd393293

for tool in git cmake ffmpeg avifenc aomenc sed; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "reference block test requires $tool" >&2
        exit 1
    }
done
[ -x "$decoder" ] || {
    echo "reference block test decoder not found: $decoder" >&2
    exit 1
}
[ -x "$obu_trace" ] || {
    echo "reference block test OBU tracer not found: $obu_trace" >&2
    exit 1
}
[ -f "$patch" ] || {
    echo "reference block test patch not found: $patch" >&2
    exit 1
}
[ -d "$vendor_source/.git" ] || {
    echo "vendored reference libaom not found: $vendor_source" >&2
    exit 1
}
mkdir -p "$work"
if [ ! -d "$source_dir/.git" ]; then
    git clone --no-hardlinks "$(pwd)/$vendor_source" "$source_dir"
fi
vendor_commit=$(git -C "$vendor_source" rev-parse HEAD)
[ "$vendor_commit" = "$expected_commit" ] || {
    echo "vendored reference libaom commit mismatch: $vendor_commit" >&2
    exit 1
}
actual_commit=$(git -C "$source_dir" rev-parse HEAD)
[ "$actual_commit" = "$expected_commit" ] || {
    rm -rf "$source_dir" "$build_dir"
    git clone --no-hardlinks "$(pwd)/$vendor_source" "$source_dir"
}
if git -C "$source_dir" apply --recount --check "$(pwd)/$patch" 2>/dev/null; then
    git -C "$source_dir" apply --recount "$(pwd)/$patch"
elif ! git -C "$source_dir" apply --recount --reverse --check \
        "$(pwd)/$patch" 2>/dev/null; then
    rm -rf "$source_dir" "$build_dir"
    git clone --no-hardlinks "$(pwd)/$vendor_source" "$source_dir"
    git -C "$source_dir" apply --recount --check "$(pwd)/$patch"
    git -C "$source_dir" apply --recount "$(pwd)/$patch"
fi

cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DAOM_TARGET_CPU=generic \
    -DCONFIG_AV1_ENCODER=0 \
    -DCONFIG_AV1_DECODER=1 \
    -DCONFIG_INSPECTION=1 \
    -DENABLE_TESTS=0 \
    -DENABLE_DOCS=0 \
    -DENABLE_EXAMPLES=1 \
    -DENABLE_TOOLS=0
cmake --build "$build_dir" --target aomdec -j "${REFERENCE_JOBS:-2}"
oracle_decoder=$build_dir/aomdec

field_from_file() {
    field=$1
    file=$2
    sed -n "s/^$field=//p" "$file"
}

field_from_text() {
    field=$1
    text=$2
    printf '%s\n' "$text" | sed -n "s/^$field=//p"
}

compared=0
directional=0
palette=0
cfl=0
filter_intra=0
nonzero_transforms=0

compare_avif() {
    input=$1
    name=$2
    obu=$work/$name.obu
    oracle=$work/$name.oracle

    ffmpeg -hide_banner -loglevel error -i "$input" -c copy -f obu -y "$obu"
    "$oracle_decoder" --threads=1 -o /dev/null "$obu" 2>"$oracle"
    local_output=$($decoder "$input")
    for checkpoint in mode predictor quantized dequantized \
        deblocked cdef superres restoration; do
        expected=$(field_from_file "avifdec_${checkpoint}_checksum" "$oracle")
        actual=$(field_from_text "${checkpoint}_checksum" "$local_output")
        [ -n "$expected" ] && [ "$actual" = "$expected" ] || {
            echo "$name $checkpoint mismatch: local=$actual oracle=$expected" >&2
            exit 1
        }
    done

    directional=$((directional + $(field_from_file avifdec_directional_blocks "$oracle")))
    palette=$((palette + $(field_from_file avifdec_palette_blocks "$oracle")))
    cfl=$((cfl + $(field_from_file avifdec_cfl_blocks "$oracle")))
    filter_intra=$((filter_intra + $(field_from_file avifdec_filter_intra_blocks "$oracle")))
    nonzero_transforms=$((nonzero_transforms + $(field_from_file avifdec_nonzero_transforms "$oracle")))
    compared=$((compared + 1))
}

if [ -d "$corpus_dir" ]; then
    for input in "$corpus_dir"/*.avif; do
        [ -f "$input" ] || continue
        name=$(basename "$input" .avif)
        compare_avif "$input" "$name"
    done
else
    echo "reference block external corpus: skipped ($corpus_dir not found)"
fi

filter_source=$work/filter-intra.y4m
filter_avif=$work/filter-intra.avif
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc=size=32x32:rate=1' -frames:v 1 -pix_fmt yuv444p \
    -f yuv4mpegpipe -y "$filter_source"
avifenc --qcolor 70 --jobs 1 --speed 0 --codec aom --cicp 0/2/0 \
    -a loopfilter-control=0 -a enable-cdef=0 -a enable-restoration=0 \
    -a enable-filter-intra=1 -a enable-palette=0 \
    "$filter_source" "$filter_avif" >/dev/null
compare_avif "$filter_avif" filter-intra

part6_source=$work/part6-active.y4m
part6_ivf=$work/part6-active.ivf
part6_avif=$work/part6-active.avif
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=128x96:rate=1' -frames:v 1 -pix_fmt yuv420p \
    -f yuv4mpegpipe -y "$part6_source"
aomenc --ivf --codec=av1 --cpu-used=0 --end-usage=q --cq-level=45 \
    --limit=1 --superres-mode=1 --superres-denominator=12 \
    --superres-kf-denominator=12 -o "$part6_ivf" "$part6_source" >/dev/null
ffmpeg -hide_banner -loglevel error -i "$part6_ivf" -frames:v 1 \
    -c:v copy -f avif -y "$part6_avif"
compare_avif "$part6_avif" part6-active
part6_oracle=$work/part6-active.oracle
previous=$(field_from_file avifdec_deblocked_checksum "$part6_oracle")
for checkpoint in cdef superres restoration; do
    current=$(field_from_file "avifdec_${checkpoint}_checksum" "$part6_oracle")
    [ "$current" != "$previous" ] || {
        echo "part6-active does not exercise $checkpoint" >&2
        exit 1
    }
    previous=$current
done

multitile_source=$work/multitile.y4m
multitile_ivf=$work/multitile.ivf
multitile_avif=$work/multitile.avif
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=256x128:rate=1' -frames:v 1 -pix_fmt yuv420p \
    -f yuv4mpegpipe -y "$multitile_source"
aomenc --ivf --codec=av1 --cpu-used=0 --end-usage=q --cq-level=45 \
    --limit=1 --tile-columns=1 -o "$multitile_ivf" "$multitile_source" \
    >/dev/null
ffmpeg -hide_banner -loglevel error -i "$multitile_ivf" -frames:v 1 \
    -c:v copy -f avif -y "$multitile_avif"
tile_columns=$($decoder "$multitile_avif" | sed -n 's/^tile_columns=//p')
[ "$tile_columns" = 2 ] || {
    echo "multitile fixture has $tile_columns tile columns, expected 2" >&2
    exit 1
}
multitile_obu=$work/multitile.obu
multitile_oracle=$work/multitile.oracle
ffmpeg -hide_banner -loglevel error -i "$multitile_avif" \
    -c copy -f obu -y "$multitile_obu"
"$oracle_decoder" --threads=1 -o /dev/null "$multitile_obu" \
    2>"$multitile_oracle"
multitile_output=$($decoder "$multitile_avif")
for checkpoint in deblocked cdef superres restoration; do
    expected=$(field_from_file "avifdec_${checkpoint}_checksum" \
        "$multitile_oracle")
    actual=$(field_from_text "${checkpoint}_checksum" "$multitile_output")
    [ -n "$expected" ] && [ "$actual" = "$expected" ] || {
        echo "multitile $checkpoint mismatch: local=$actual oracle=$expected" >&2
        exit 1
    }
done

part7_source=$work/part7-motion.y4m
part7_obu=$work/part7-motion.obu
part7_oracle=$work/part7-motion.oracle
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "nullsrc=s=64x32:r=1,geq=lum='64+mod(X,16)*8+mod(Y,8)*4':cb=128:cr=128,crop=32:32:x='2*n':y=0" \
    -frames:v 4 -pix_fmt yuv420p -f yuv4mpegpipe -y "$part7_source"
aomenc --obu --codec=av1 --cpu-used=6 --end-usage=q --cq-level=63 \
    --limit=4 --lag-in-frames=0 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=0 --max-reference-frames=3 --enable-order-hint=1 \
    --enable-ref-frame-mvs=0 --enable-global-motion=0 \
    --enable-warped-motion=0 --enable-obmc=0 --enable-interintra-comp=0 \
    --enable-masked-comp=0 --enable-dual-filter=0 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part7_obu" "$part7_source" >/dev/null
"$oracle_decoder" --threads=1 -o /dev/null "$part7_obu" 2>"$part7_oracle"
part7_output=$($obu_trace "$part7_obu")
if "$obu_trace" --max-frames 3 "$part7_obu" \
    >"$work/part7-limit.out" 2>"$work/part7-limit.err"; then
    echo "part7-motion exceeded the frame limit without failing" >&2
    exit 1
fi
grep -q 'limit exceeded' "$work/part7-limit.err" || {
    echo "part7-motion frame limit did not report limit exceeded" >&2
    exit 1
}
for checkpoint in inter_mode mv_stack mv; do
    expected=$(field_from_file "avifdec_${checkpoint}_tile_checksum" \
        "$part7_oracle" | perl tools/fnv-u64-lines.pl)
    actual=$(field_from_text "${checkpoint}_checksum" "$part7_output")
    [ "$actual" = "$expected" ] || {
        echo "part7-motion $checkpoint mismatch: local=$actual oracle=$expected" >&2
        exit 1
    }
done
expected=$(field_from_file avifdec_reference_state_checksum \
    "$part7_oracle" | tail -n 1)
actual=$(field_from_text reference_state_checksum "$part7_output")
[ "$actual" = "$expected" ] || {
    echo "part7-motion reference state mismatch: local=$actual oracle=$expected" >&2
    exit 1
}
expected=$(field_from_file avifdec_reference_frames "$part7_oracle" | tail -n 1)
actual=$(field_from_text frames "$part7_output")
[ "$actual" = "$expected" ] || {
    echo "part7-motion frame count mismatch: local=$actual oracle=$expected" >&2
    exit 1
}
part7_frames=$actual
expected=$(field_from_file avifdec_inter_blocks "$part7_oracle" |
    awk '{ total += $1 } END { print total + 0 }')
actual=$(field_from_text inter_blocks "$part7_output")
[ "$actual" = "$expected" ] && [ "$actual" -gt 0 ] || {
    echo "part7-motion inter block count mismatch: local=$actual oracle=$expected" >&2
    exit 1
}
part7_inter_blocks=$actual
mode_mask=0
for value in $(field_from_file avifdec_inter_mode_mask "$part7_oracle"); do
    mode_mask=$((mode_mask | value))
done
[ $((mode_mask & (1 << 13))) -ne 0 ] &&
[ $((mode_mask & (1 << 14))) -ne 0 ] &&
[ $((mode_mask & (1 << 16))) -ne 0 ] || {
    echo "part7-motion does not cover nearest, near, and new MV modes" >&2
    exit 1
}

compare_inter_obu() {
    obu=$1
    name=$2
    expected_frames=$3
    minimum_compound=$4
    minimum_interintra=$5
    oracle=$work/$name.oracle

    "$oracle_decoder" --threads=1 -o /dev/null "$obu" 2>"$oracle"
    output=$($obu_trace "$obu")
    expected=$(field_from_file avifdec_restoration_plane_checksum "$oracle" |
        perl tools/fnv-u64-lines.pl)
    actual=$(field_from_text restoration_checksum "$output")
    [ "$actual" = "$expected" ] || {
        echo "$name final pixels mismatch: local=$actual oracle=$expected" >&2
        exit 1
    }

    compare_part9_obu() {
        obu=$1
        name=$2
        counter=$3
        minimum=$4
        oracle=$work/$name.oracle

        "$oracle_decoder" --threads=1 -o /dev/null "$obu" 2>"$oracle"
        output=$($obu_trace "$obu")
        expected=$(field_from_file avifdec_restoration_plane_checksum "$oracle" |
            perl tools/fnv-u64-lines.pl)
        actual=$(field_from_text restoration_checksum "$output")
        [ "$actual" = "$expected" ] || {
            echo "$name final pixels mismatch: local=$actual oracle=$expected" >&2
            exit 1
        }
        expected=$(field_from_file avifdec_reference_frames "$oracle" | tail -n 1)
        actual=$(field_from_text frames "$output")
        [ "$actual" = "$expected" ] || {
            echo "$name frame count mismatch: local=$actual oracle=$expected" >&2
            exit 1
        }
        coverage=$(field_from_file "avifdec_${counter}_blocks" "$oracle" |
            awk '{ total += $1 } END { print total + 0 }')
        [ "$coverage" -ge "$minimum" ] || {
            echo "$name $counter block count is $coverage" >&2
            exit 1
        }
    }
    actual=$(field_from_text frames "$output")
    [ "$actual" = "$expected_frames" ] || {
        echo "$name frame count mismatch: $actual" >&2
        exit 1
    }
    actual=$(field_from_text inter_blocks "$output")
    [ "$actual" -gt 0 ] || {
        echo "$name does not exercise inter prediction" >&2
        exit 1
    }
    actual=$(field_from_text compound_blocks "$output")
    [ "$actual" -ge "$minimum_compound" ] || {
        echo "$name compound block count is $actual" >&2
        exit 1
    }
    interintra=$(field_from_file avifdec_interintra_blocks "$oracle" |
        awk '{ total += $1 } END { print total + 0 }')
    [ "$interintra" -ge "$minimum_interintra" ] || {
        echo "$name inter-intra block count is $interintra" >&2
        exit 1
    }
    motion_mask=0
    for value in $(field_from_file avifdec_motion_mode_mask "$oracle"); do
        motion_mask=$((motion_mask | value))
    done
    [ "$motion_mask" -eq 1 ] || {
        echo "$name unexpectedly uses advanced motion modes: $motion_mask" >&2
        exit 1
    }
}

part8_source=$work/part8-translational.y4m
part8_obu=$work/part8-translational.obu
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "nullsrc=s=96x64:r=1,geq=lum='64+mod(X,24)*5+mod(Y,16)*4+40*sin((X+Y)/7)':cb='96+mod(X,32)':cr='160-mod(Y,32)',crop=64:64:x='4*n':y=0" \
    -frames:v 8 -pix_fmt yuv420p -f yuv4mpegpipe -y "$part8_source"
aomenc --obu --codec=av1 --cpu-used=4 --end-usage=q --cq-level=35 \
    --limit=8 --lag-in-frames=0 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=0 --max-reference-frames=7 --enable-order-hint=1 \
    --enable-ref-frame-mvs=0 --enable-global-motion=0 \
    --enable-warped-motion=0 --enable-obmc=0 --enable-interintra-comp=0 \
    --enable-masked-comp=0 --enable-dual-filter=1 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part8_obu" "$part8_source" >/dev/null
compare_inter_obu "$part8_obu" part8-translational 8 0 0

part8_compound_source=$work/part8-compound.y4m
part8_compound_full=$work/part8-compound-full.obu
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "nullsrc=s=96x64:r=4,geq=lum='64+mod(X,24)*5+mod(Y,16)*4+40*sin((X+Y)/7)':cb='96+mod(X,32)':cr='160-mod(Y,32)',crop=64:64:x='2*n':y=0" \
    -frames:v 16 -pix_fmt yuv420p -f yuv4mpegpipe \
    -y "$part8_compound_source"
aomenc --obu --codec=av1 --cpu-used=0 --end-usage=q --cq-level=35 \
    --limit=16 --lag-in-frames=0 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=0 --max-reference-frames=7 --enable-order-hint=1 \
    --enable-ref-frame-mvs=0 --enable-global-motion=0 \
    --enable-warped-motion=0 --enable-obmc=0 --enable-interintra-comp=1 \
    --enable-masked-comp=1 --enable-dual-filter=1 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part8_compound_full" "$part8_compound_source" >/dev/null 2>&1
compare_inter_obu "$part8_compound_full" part8-compound 16 1 0

part8_interintra_source=$work/part8-interintra.y4m
part8_interintra_full=$work/part8-interintra-full.obu
part8_interintra_obu=$work/part8-interintra.obu
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=64x64:rate=4' -frames:v 8 -pix_fmt yuv420p \
    -f yuv4mpegpipe -y "$part8_interintra_source"
aomenc --obu --codec=av1 --cpu-used=0 --end-usage=q --cq-level=55 \
    --limit=8 --lag-in-frames=0 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=0 --max-reference-frames=7 --enable-order-hint=1 \
    --enable-ref-frame-mvs=0 --enable-global-motion=0 \
    --enable-warped-motion=0 --enable-obmc=0 --enable-interintra-comp=1 \
    --enable-masked-comp=1 --enable-dual-filter=1 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part8_interintra_full" "$part8_interintra_source" >/dev/null 2>&1
ffmpeg -hide_banner -loglevel error -i "$part8_interintra_full" \
    -frames:v 2 -c copy -f obu -y "$part8_interintra_obu"
compare_inter_obu "$part8_interintra_obu" part8-interintra 2 0 1

part9_obmc_source=$work/part9-obmc.y4m
part9_obmc_full=$work/part9-obmc-full.obu
part9_obmc_obu=$work/part9-obmc.obu
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "nullsrc=s=128x96:r=4,geq=lum='64+mod(X,24)*5+mod(Y,16)*4+40*sin((X+Y)/7)':cb='96+mod(X,32)':cr='160-mod(Y,32)',crop=96:64:x='2*n':y='n'" \
    -frames:v 12 -pix_fmt yuv420p -f yuv4mpegpipe -y "$part9_obmc_source"
aomenc --obu --codec=av1 --cpu-used=0 --end-usage=q --cq-level=35 \
    --limit=12 --lag-in-frames=0 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=0 --max-reference-frames=7 --enable-order-hint=1 \
    --enable-ref-frame-mvs=0 --enable-global-motion=0 \
    --enable-warped-motion=0 --enable-obmc=1 --enable-interintra-comp=0 \
    --enable-masked-comp=0 --enable-dual-filter=1 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part9_obmc_full" "$part9_obmc_source" >/dev/null 2>&1
ffmpeg -hide_banner -loglevel error -i "$part9_obmc_full" \
    -frames:v 10 -c copy -f obu -y "$part9_obmc_obu"
compare_part9_obu "$part9_obmc_obu" part9-obmc obmc 1

part9_warp_obu=$work/part9-local-warp.obu
aomenc --obu --codec=av1 --cpu-used=4 --end-usage=q --cq-level=35 \
    --limit=4 --lag-in-frames=0 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=0 --max-reference-frames=7 --enable-order-hint=1 \
    --enable-ref-frame-mvs=0 --enable-global-motion=0 \
    --enable-warped-motion=1 --enable-obmc=0 --enable-interintra-comp=0 \
    --enable-masked-comp=0 --enable-dual-filter=1 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part9_warp_obu" "$part8_source" >/dev/null 2>&1
compare_part9_obu "$part9_warp_obu" part9-local-warp local_warp 2

part9_intrabc_source=$work/part9-intrabc.y4m
part9_intrabc_obu=$work/part9-intrabc.obu
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "nullsrc=s=384x64:r=1,geq=lum='mod(37*mod(X,64)+73*mod(Y,64)+mod(mod(X,64)*mod(Y,64),251),256)':cb='mod(19*mod(X,64)+41*mod(Y,64),256)':cr='mod(61*mod(X,64)+23*mod(Y,64),256)'" \
    -frames:v 1 -pix_fmt yuv444p -f yuv4mpegpipe -y "$part9_intrabc_source"
aomenc --obu --codec=av1 --cpu-used=0 --lossless=1 --limit=1 \
    --lag-in-frames=0 --tune-content=screen --enable-intrabc=1 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part9_intrabc_obu" "$part9_intrabc_source" >/dev/null 2>&1
compare_part9_obu "$part9_intrabc_obu" part9-intrabc intrabc 1

part9_skip_source=$work/part9-skip.y4m
part9_skip_obu=$work/part9-skip.obu
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=64x64:rate=8' -frames:v 24 -pix_fmt yuv420p \
    -f yuv4mpegpipe -y "$part9_skip_source"
aomenc --obu --codec=av1 --cpu-used=0 --end-usage=q --cq-level=60 \
    --limit=24 --lag-in-frames=12 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=1 --max-reference-frames=7 --enable-order-hint=1 \
    --enable-ref-frame-mvs=0 --enable-global-motion=0 \
    --enable-warped-motion=0 --enable-obmc=0 --enable-interintra-comp=1 \
    --enable-masked-comp=1 --enable-dual-filter=1 --enable-cdef=0 \
    --enable-restoration=0 --loopfilter-control=0 \
    -o "$part9_skip_obu" "$part9_skip_source" >/dev/null 2>&1
compare_part9_obu "$part9_skip_obu" part9-skip skip_mode 1

part9_switch_full=$work/part9-switch-full.obu
part9_switch_obu=$work/part9-switch.obu
aomenc --obu --codec=av1 --cpu-used=0 --end-usage=q --cq-level=60 \
    --limit=24 --lag-in-frames=12 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=1 --max-reference-frames=7 --sframe-dist=16 \
    --sframe-mode=1 --enable-order-hint=1 --enable-ref-frame-mvs=0 \
    --enable-global-motion=0 --enable-warped-motion=0 --enable-obmc=0 \
    --enable-interintra-comp=1 --enable-masked-comp=1 \
    --enable-dual-filter=1 --enable-cdef=0 --enable-restoration=0 \
    --loopfilter-control=0 \
    -o "$part9_switch_full" "$part9_skip_source" >/dev/null 2>&1
perl - "$part9_switch_full" "$part9_switch_obu" <<'PERL'
use strict;
use warnings;

my ($input_path, $output_path) = @ARGV;
open my $input, '<:raw', $input_path or die "$input_path: $!\n";
local $/;
my $data = <$input>;
close $input or die "$input_path: $!\n";

open my $output, '>:raw', $output_path or die "$output_path: $!\n";
my ($offset, $frames) = (0, 0);
while ($offset < length $data && $frames < 16) {
    my $start = $offset;
    my $header = ord substr($data, $offset++, 1);
    my $type = ($header >> 3) & 15;
    $offset++ if ($header >> 2) & 1;
    die "OBU without a size field\n" unless ($header >> 1) & 1;
    my ($size, $shift) = (0, 0);
    while (1) {
        die "truncated OBU size\n" if $offset >= length $data;
        my $byte = ord substr($data, $offset++, 1);
        $size |= ($byte & 127) << $shift;
        last if $byte < 128;
        $shift += 7;
        die "oversized OBU size\n" if $shift > 56;
    }
    die "truncated OBU payload\n" if $size > length($data) - $offset;
    $offset += $size;
    print {$output} substr($data, $start, $offset - $start);
    ++$frames if $type == 6;
}
close $output or die "$output_path: $!\n";
die "switch stream contains only $frames coded frames\n" if $frames != 16;
PERL
compare_part9_obu "$part9_switch_obu" part9-switch switch_frame 0
switch_frames=$(field_from_file avifdec_frame_type "$work/part9-switch.oracle" |
    awk '$1 == 3 { total += 1 } END { print total + 0 }')
[ "$switch_frames" -gt 0 ] || {
    echo "part9-switch does not contain a switch frame" >&2
    exit 1
}

part9_global_source=$work/part9-global.y4m
part9_global_obu=$work/part9-global.obu
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "nullsrc=s=128x128:r=1,geq=lum='48+mod(X,48)*2+mod(Y,40)*2+25*sin((X+Y)/9)':cb='96+mod(X,48)':cr='160-mod(Y,48)',zoompan=z='1+0.04*on':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=1:s=64x64:fps=4" \
    -frames:v 8 -pix_fmt yuv420p -f yuv4mpegpipe -y "$part9_global_source"
aomenc --obu --codec=av1 --cpu-used=0 --end-usage=q --cq-level=50 \
    --limit=8 --lag-in-frames=0 --kf-min-dist=999 --kf-max-dist=999 \
    --auto-alt-ref=0 --max-reference-frames=7 --min-partition-size=16 \
    --enable-order-hint=1 --enable-ref-frame-mvs=0 \
    --enable-global-motion=1 --enable-warped-motion=0 --enable-obmc=0 \
    --enable-interintra-comp=0 --enable-masked-comp=0 \
    --enable-dual-filter=1 --enable-cdef=0 --enable-restoration=0 \
    --loopfilter-control=0 \
    -o "$part9_global_obu" "$part9_global_source" >/dev/null 2>&1
compare_part9_obu "$part9_global_obu" part9-global global_warp 1

grain_source=$work/part10-grain-8.y4m
grain_obu=$work/part10-grain-8.obu
grain_avif=$work/part10-grain-8.avif
grain_local=$work/part10-grain-8.local.yuv
grain_oracle=$work/part10-grain-8.oracle.yuv
grain_unfiltered=$work/part10-grain-8.unfiltered.yuv
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=65x35:rate=1' -frames:v 1 -pix_fmt yuv420p \
    -f yuv4mpegpipe -y "$grain_source"
aomenc --obu --codec=av1 --cpu-used=4 --end-usage=q --cq-level=40 \
    --limit=1 --film-grain-test=1 -o "$grain_obu" "$grain_source" \
    >/dev/null 2>&1
ffmpeg -hide_banner -loglevel error -i "$grain_obu" -frames:v 1 \
    -c:v copy -f avif -y "$grain_avif"
"$decoder" --raw "$grain_avif" "$grain_local" >/dev/null
"$oracle_decoder" --threads=1 --rawvideo --i420 \
    -o "$grain_oracle" "$grain_obu" >/dev/null 2>&1
"$oracle_decoder" --threads=1 --rawvideo --i420 --skip-film-grain \
    -o "$grain_unfiltered" "$grain_obu" >/dev/null 2>&1
cmp "$grain_local" "$grain_oracle"
! cmp -s "$grain_oracle" "$grain_unfiltered" || {
    echo "part10-grain-8 does not exercise film grain" >&2
    exit 1
}
grain_present=$("$decoder" "$grain_avif" |
    sed -n 's/^film_grain_params_present=//p')
[ "$grain_present" = 1 ] || {
    echo "part10-grain-8 does not signal film grain" >&2
    exit 1
}
grain_applied=$("$decoder" "$grain_avif" |
    sed -n 's/^film_grain_applied=//p')
[ "$grain_applied" = 1 ] || {
    echo "part10-grain-8 does not apply film grain" >&2
    exit 1
}

grain10_source=$work/part10-grain-10.y4m
grain10_obu=$work/part10-grain-10.obu
grain10_avif=$work/part10-grain-10.avif
grain10_local=$work/part10-grain-10.local.yuv
grain10_oracle=$work/part10-grain-10.oracle.yuv
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc=size=33x37:rate=1' -frames:v 1 \
    -pix_fmt yuv444p10le -strict -1 -f yuv4mpegpipe \
    -y "$grain10_source"
aomenc --obu --codec=av1 --cpu-used=4 --end-usage=q --cq-level=40 \
    --limit=1 --profile=1 --bit-depth=10 --input-bit-depth=10 \
    --film-grain-test=2 -o "$grain10_obu" "$grain10_source" \
    >/dev/null 2>&1
ffmpeg -hide_banner -loglevel error -i "$grain10_obu" -frames:v 1 \
    -c:v copy -f avif -y "$grain10_avif"
"$decoder" --raw "$grain10_avif" "$grain10_local" >/dev/null
"$oracle_decoder" --threads=1 --rawvideo --output-bit-depth=10 \
    -o "$grain10_oracle" "$grain10_obu" >/dev/null 2>&1
cmp "$grain10_local" "$grain10_oracle"

grain12_source=$work/part10-grain-12.y4m
grain12_obu=$work/part10-grain-12.obu
grain12_avif=$work/part10-grain-12.avif
grain12_local=$work/part10-grain-12.local.yuv
grain12_oracle=$work/part10-grain-12.oracle.yuv
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=35x39:rate=1' -frames:v 1 \
    -pix_fmt yuv420p12le -strict -1 -f yuv4mpegpipe \
    -y "$grain12_source"
aomenc --obu --codec=av1 --cpu-used=4 --end-usage=q --cq-level=40 \
    --limit=1 --profile=2 --bit-depth=12 --input-bit-depth=12 \
    --film-grain-test=3 -o "$grain12_obu" "$grain12_source" \
    >/dev/null 2>&1
ffmpeg -hide_banner -loglevel error -i "$grain12_obu" -frames:v 1 \
    -c:v copy -f avif -y "$grain12_avif"
"$decoder" --raw "$grain12_avif" "$grain12_local" >/dev/null
"$oracle_decoder" --threads=1 --rawvideo --output-bit-depth=12 \
    -o "$grain12_oracle" "$grain12_obu" >/dev/null 2>&1
cmp "$grain12_local" "$grain12_oracle"

grain422_source=$work/part10-grain-422.y4m
grain422_obu=$work/part10-grain-422.obu
grain422_avif=$work/part10-grain-422.avif
grain422_local=$work/part10-grain-422.local.yuv
grain422_oracle=$work/part10-grain-422.oracle.yuv
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i 'testsrc=size=37x35:rate=1' -frames:v 1 \
    -pix_fmt yuv422p10le -strict -1 -f yuv4mpegpipe \
    -y "$grain422_source"
aomenc --obu --codec=av1 --cpu-used=4 --end-usage=q --cq-level=40 \
    --limit=1 --profile=2 --bit-depth=10 --input-bit-depth=10 \
    --film-grain-test=4 -o "$grain422_obu" "$grain422_source" \
    >/dev/null 2>&1
ffmpeg -hide_banner -loglevel error -i "$grain422_obu" -frames:v 1 \
    -c:v copy -f avif -y "$grain422_avif"
"$decoder" --raw "$grain422_avif" "$grain422_local" >/dev/null
"$oracle_decoder" --threads=1 --rawvideo --output-bit-depth=10 \
    -o "$grain422_oracle" "$grain422_obu" >/dev/null 2>&1
cmp "$grain422_local" "$grain422_oracle"

[ "$compared" -gt 0 ]
[ "$directional" -gt 0 ] || { echo "no directional blocks covered" >&2; exit 1; }
[ "$palette" -gt 0 ] || { echo "no palette blocks covered" >&2; exit 1; }
[ "$cfl" -gt 0 ] || { echo "no CfL blocks covered" >&2; exit 1; }
[ "$filter_intra" -gt 0 ] || { echo "no filter-intra blocks covered" >&2; exit 1; }
[ "$nonzero_transforms" -gt 0 ] || { echo "no nonzero transforms covered" >&2; exit 1; }

printf 'trusted block trace test: %d files, directional=%d palette=%d CfL=%d filter-intra=%d nonzero-transforms=%d, active post-filters, multi-tile filters\n' \
    "$compared" "$directional" "$palette" "$cfl" "$filter_intra" "$nonzero_transforms"
printf 'trusted Part 7 trace test: frames=%s inter-blocks=%s mode-mask=0x%x\n' \
    "$part7_frames" "$part7_inter_blocks" "$mode_mask"
printf 'trusted Part 8 pixel test: translational frames=8, compound frames=16, inter-intra frames=2\n'
printf 'trusted Part 9 pixel test: OBMC, local/global warp, intrabc, skip mode, and switch frames\n'
printf 'trusted Part 10 film-grain test: 8/10/12-bit 4:2:0, 4:2:2, and 4:4:4 exact output\n'
