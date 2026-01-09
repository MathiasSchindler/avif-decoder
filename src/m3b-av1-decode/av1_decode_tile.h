#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// m3b.D scaffolding: tile syntax traversal entrypoint.
//
// This intentionally starts life as a probe that can run on real tile payloads without
// crashing. It will be expanded incrementally into real AV1 tile syntax decoding.

typedef enum {
    AV1_TILE_SYNTAX_PROBE_OK = 0,
    AV1_TILE_SYNTAX_PROBE_UNSUPPORTED = 1,
    AV1_TILE_SYNTAX_PROBE_ERROR = 2,
} Av1TileSyntaxProbeStatus;

typedef struct {
    // Tile bounds in MI units (4x4 luma blocks), half-open [start,end).
    uint32_t mi_col_start;
    uint32_t mi_col_end;
    uint32_t mi_row_start;
    uint32_t mi_row_end;

    // From the sequence header.
    uint32_t use_128x128_superblock;

    // From the sequence header color_config().
    uint32_t mono_chrome;
    uint32_t subsampling_x;
    uint32_t subsampling_y;

    // From the frame header derived state.
    uint32_t coded_lossless;

    // From the sequence header.
    uint32_t enable_filter_intra;

    // From the frame header.
    uint32_t allow_screen_content_tools;

    // From the frame header.
    uint32_t disable_cdf_update;

    // From the frame header quantizer_params(). Used to initialize coeff CDFs.
    uint32_t base_q_idx;

    // From read_tx_mode() in the frame header.
    // Values match the AV1 spec names:
    // 0 = ONLY_4X4, 1 = TX_MODE_LARGEST, 2 = TX_MODE_SELECT.
    uint32_t tx_mode;

    // reduced_tx_set (from the frame header).
    // Used by get_tx_set(txSz) when decoding intra_tx_type.
    uint32_t reduced_tx_set;

    // Probe-only behavior toggle:
    // 0 (default): keep the lightweight "stop early" probe behavior (expected UNSUPPORTED).
    // 1: attempt full tile traversal and call exit_symbol() at the true end-of-tile.
    uint32_t probe_try_exit_symbol;
} Av1TileDecodeParams;

typedef struct {
    uint32_t bools_requested;
    uint32_t bools_read;

    // Derived grid dimensions.
    uint32_t tile_mi_cols;
    uint32_t tile_mi_rows;
    uint32_t sb_mi_size;
    uint32_t sb_cols;
    uint32_t sb_rows;

    // First milestone symbol: root superblock partition decision.
    // If partition_decoded is false, the other fields are undefined.
    bool partition_decoded;
    // If true, the root partition decision was forced by boundary conditions
    // (no entropy-coded symbol was read for it).
    bool partition_forced;
    uint32_t partition_bsl;
    uint32_t partition_ctx;
    uint32_t partition_symbol;

    // Partition traversal progress (m3b.D.2).
    uint32_t partition_symbols_read;
    uint32_t partition_forced_splits;
    uint32_t leaf_blocks;

    // decode_block() progress (m3b.D.3): first block's `skip` symbol.
    bool block0_skip_decoded;
    uint32_t blocks_decoded;
    uint32_t block0_r_mi;
    uint32_t block0_c_mi;
    uint32_t block0_wlog2;
    uint32_t block0_hlog2;
    uint32_t block0_skip_ctx;
    uint32_t block0_skip;

    // decode_block() progress (m3b.D.4): first block's `y_mode` symbol.
    bool block0_y_mode_decoded;
    uint32_t block0_y_mode_ctx;
    uint32_t block0_y_mode;

    // decode_block() progress (m3b.D.5): first block's `uv_mode` symbol.
    bool block0_uv_mode_decoded;
    uint32_t block0_uv_mode;

    // decode_block() progress: optional directional mode angle deltas.
    bool block0_angle_delta_y_decoded;
    int32_t block0_angle_delta_y;
    bool block0_angle_delta_uv_decoded;
    int32_t block0_angle_delta_uv;

    // decode_block() progress: CFL alphas when uv_mode == UV_CFL_PRED.
    bool block0_cfl_alphas_decoded;
    uint32_t block0_cfl_alpha_signs;
    int32_t block0_cfl_alpha_u;
    int32_t block0_cfl_alpha_v;

    // decode_block() progress: filter intra when enabled + eligible.
    bool block0_use_filter_intra_decoded;
    uint32_t block0_use_filter_intra;
    bool block0_filter_intra_mode_decoded;
    uint32_t block0_filter_intra_mode;

    // decode_block() progress: palette mode info (screen-content tools).
    bool block0_has_palette_y_decoded;
    uint32_t block0_has_palette_y;
    bool block0_palette_size_y_decoded;
    uint32_t block0_palette_size_y;

    bool block0_has_palette_uv_decoded;
    uint32_t block0_has_palette_uv;
    bool block0_palette_size_uv_decoded;
    uint32_t block0_palette_size_uv;

    // decode_block() progress: tx_size selection depth (read_tx_size())
    // Only present when TxMode == TX_MODE_SELECT and !Lossless.
    bool block0_tx_depth_decoded;
    uint32_t block0_tx_depth;
    uint32_t block0_tx_mode;

    // decode_block() progress: derived TxSize (read_tx_size()).
    // Always derivable for our intra stub once MiSize is known.
    bool block0_tx_size_decoded;
    uint32_t block0_tx_size;

    // decode_block() progress: transform_type() result for the first transform block.
    // Only present when get_tx_set(txSz)>0 and qindex>0 (lossless forces DCT_DCT).
    bool block0_tx_type_decoded;
    uint32_t block0_tx_type;

    // decode_block() progress: first coeff-related symbol in coeffs() (all_zero / txb_skip).
    bool block0_txb_skip_decoded;
    uint32_t block0_txb_skip_ctx;
    uint32_t block0_txb_skip;

    // decode_block() progress: luma transform-block iteration within block0.
    // For now we only guarantee decoding the first two luma tx blocks.
    uint32_t block0_tx_blocks_decoded;
    bool block0_tx1_txb_skip_decoded;
    uint32_t block0_tx1_x4;
    uint32_t block0_tx1_y4;
    uint32_t block0_tx1_txb_skip_ctx;
    uint32_t block0_tx1_txb_skip;

    // decode_block() progress: second block's txb_skip (to exercise above/left coeff contexts).
    bool block1_txb_skip_decoded;
    uint32_t block1_r_mi;
    uint32_t block1_c_mi;
    uint32_t block1_txb_skip_ctx;
    uint32_t block1_txb_skip;

    // decode_block() progress: second block's eob_pt (to stop after the next coeff symbol).
    // Only present when block1 txb_skip==0.
    bool block1_eob_pt_decoded;
    uint32_t block1_eob_pt_ctx;
    uint32_t block1_eob_pt;

    // decode_block() progress: second block's derived eob (from eobPt).
    // Only present when block1 txb_skip==0.
    bool block1_eob_decoded;
    uint32_t block1_eob;

    // decode_block() progress: second block's coeff_base_eob (base level of last non-zero coeff).
    // Only present when block1 txb_skip==0.
    bool block1_coeff_base_eob_decoded;
    uint32_t block1_coeff_base_eob_ctx;
    uint32_t block1_coeff_base_eob_level;

    // decode_block() progress: second block's coeff_base symbol for c==0 (when eob>=2).
    bool block1_coeff_base_decoded;
    uint32_t block1_coeff_base_ctx;
    uint32_t block1_coeff_base_level;

    // decode_block() progress: next coeff symbol (eob_pt_* + 1 => eobPt).
    // Only present when txb_skip==0.
    bool block0_eob_pt_decoded;
    uint32_t block0_eob_pt;

    // decode_block() progress: derived eob (from eobPt via eob_extra + eob_extra_bit).
    // Only present when txb_skip==0.
    bool block0_eob_decoded;
    uint32_t block0_eob;

    // decode_block() progress: coeff_base_eob (base level of last non-zero coeff).
    // Only present when txb_skip==0.
    bool block0_coeff_base_eob_decoded;
    uint32_t block0_coeff_base_eob_ctx;
    uint32_t block0_coeff_base_eob_level;

    // decode_block() progress: coeff_base for c==0 when eob==2.
    bool block0_coeff_base_decoded;
    uint32_t block0_coeff_base_ctx;
    uint32_t block0_coeff_base_level;

    // decode_block() progress: first decoded coeff_br symbol (if any coefficient used it).
    bool block0_coeff_br_decoded;
    uint32_t block0_coeff_br_ctx;
    uint32_t block0_coeff_br_sym;

    // decode_block() progress: dc_sign for pos==0 when Quant[0]!=0.
    bool block0_dc_sign_decoded;
    uint32_t block0_dc_sign_ctx;
    uint32_t block0_dc_sign;
} Av1TileSyntaxProbeStats;

// Attempts to traverse (a prefix of) the entropy-coded tile syntax.
//
// For now:
// - Initializes the AV1 symbol decoder on the tile payload.
// - Optionally reads N bool symbols (diagnostic only).
// - Returns UNSUPPORTED to mark that full tile syntax decode is not implemented yet.
//
// When strict-gating later:
// - callers can treat UNSUPPORTED as failure for generated vectors.
Av1TileSyntaxProbeStatus av1_tile_syntax_probe(const uint8_t *tile_data,
                                               size_t tile_size_bytes,
                                               const Av1TileDecodeParams *params,
                                               uint32_t probe_bools,
                                               Av1TileSyntaxProbeStats *out_stats,
                                               char *err,
                                               size_t err_cap);
