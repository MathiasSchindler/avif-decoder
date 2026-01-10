#include "av1_decode_tile.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "av1_symbol.h"

// Spec default CDF initializers extracted from the local av1bitstream.html.
#include "av1_default_cdfs_intra_tx_type.inc"
#include "av1_default_cdfs_coeff_base_luma.inc"
#include "av1_default_cdfs_coeff_base_chroma.inc"
#include "av1_default_cdfs_coeff_br_luma.inc"
#include "av1_default_cdfs_coeff_br_chroma.inc"
#include "av1_default_cdfs_dc_sign_luma.inc"
#include "av1_default_cdfs_dc_sign_chroma.inc"

// Additional chroma-only default coefficient CDF initializers (luma variants are currently embedded below).
#include "av1_default_cdfs_eob_chroma.inc"
#include "av1_default_cdfs_eob_extra_chroma.inc"
#include "av1_default_cdfs_coeff_base_eob_chroma.inc"

static uint32_t u32_ceil_div(uint32_t a, uint32_t b) {
   return (a + b - 1u) / b;
}

static bool mi_size_index_from_wlog2_hlog2(uint32_t wlog2, uint32_t hlog2, uint32_t *out_mi_size);

#define AV1_PARTITION_CONTEXTS 4u

#define AV1_SKIP_CONTEXTS 3u

#define AV1_INTRA_MODES 13u
#define AV1_Y_MODE_CONTEXTS 4u

#define AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED 13u
#define AV1_UV_INTRA_MODES_CFL_ALLOWED 14u

#define AV1_UV_MODE_CFL 13u

#define AV1_CFL_JOINT_SIGNS 8u
#define AV1_CFL_ALPHABET_SIZE 16u
#define AV1_CFL_ALPHA_CONTEXTS 6u

#define AV1_INTRA_FILTER_MODES 5u
#define AV1_BLOCK_SIZES 22u

#define AV1_PALETTE_BLOCK_SIZE_CONTEXTS 7u
#define AV1_PALETTE_Y_MODE_CONTEXTS 3u
#define AV1_PALETTE_UV_MODE_CONTEXTS 2u
#define AV1_PALETTE_SIZES 7u

#define AV1_TX_SIZE_CONTEXTS 3u
#define AV1_MAX_TX_DEPTH 2u
#define AV1_TX_SIZES_ALL 19u

// Spec-derived coefficient context helper table.
#include "av1_coeff_base_ctx_offset.inc"

// Coefficient CDF dimensions (subset we currently need for txb_skip).
#define AV1_COEFF_CDF_Q_CTXS 4u
#define AV1_COEFF_TX_SIZES 5u
#define AV1_COEFF_BR_TX_SIZES 4u
#define AV1_PLANE_TYPES 2u
#define AV1_MAX_PLANES 3u
#define AV1_TXB_SKIP_CONTEXTS 13u
#define AV1_EOB_COEF_CONTEXTS 9u
#define AV1_SIG_COEF_CONTEXTS 42u
#define AV1_SIG_COEF_CONTEXTS_2D 26u
#define AV1_SIG_COEF_CONTEXTS_EOB 4u

#define AV1_NUM_BASE_LEVELS 2u
#define AV1_COEFF_BASE_RANGE 12u
#define AV1_BR_CDF_SIZE 4u
#define AV1_LEVEL_CONTEXTS 21u
#define AV1_DC_SIGN_CONTEXTS 3u

#define AV1_INTRA_TX_TYPE_SET1_SYMBOLS 7u
#define AV1_INTRA_TX_TYPE_SET2_SYMBOLS 5u

// Minimal transform-set identifiers (spec names).
#define AV1_TX_SET_DCTONLY 0u
#define AV1_TX_SET_INTRA_1 1u
#define AV1_TX_SET_INTRA_2 2u

// Minimal transform-type identifiers (internal values; only used for tx_class and stats).
#define AV1_TX_TYPE_IDTX 0u
#define AV1_TX_TYPE_DCT_DCT 1u
#define AV1_TX_TYPE_V_DCT 2u
#define AV1_TX_TYPE_H_DCT 3u
#define AV1_TX_TYPE_ADST_ADST 4u
#define AV1_TX_TYPE_ADST_DCT 5u
#define AV1_TX_TYPE_DCT_ADST 6u

// TX class identifiers aligned with Sig_Ref_Diff_Offset ordering in the spec.
#define AV1_TX_CLASS_2D 0u
#define AV1_TX_CLASS_HORIZ 1u
#define AV1_TX_CLASS_VERT 2u

#define AV1_SIG_REF_DIFF_OFFSET_NUM 5u

// AV1 TxSize enum values (match common libaom ordering).
enum {
   AV1_TX_4X4 = 0,
   AV1_TX_8X8 = 1,
   AV1_TX_16X16 = 2,
   AV1_TX_32X32 = 3,
   AV1_TX_64X64 = 4,
   AV1_TX_4X8 = 5,
   AV1_TX_8X4 = 6,
   AV1_TX_8X16 = 7,
   AV1_TX_16X8 = 8,
   AV1_TX_16X32 = 9,
   AV1_TX_32X16 = 10,
   AV1_TX_32X64 = 11,
   AV1_TX_64X32 = 12,
   AV1_TX_4X16 = 13,
   AV1_TX_16X4 = 14,
   AV1_TX_8X32 = 15,
   AV1_TX_32X8 = 16,
   AV1_TX_16X64 = 17,
   AV1_TX_64X16 = 18,
};

// Adjusted_Tx_Size (spec table) for our TxSize enum.
// Used for coefficient context derivation.
static const uint8_t kAdjustedTxSize[AV1_TX_SIZES_ALL] = {
   AV1_TX_4X4,   // TX_4X4
   AV1_TX_8X8,   // TX_8X8
   AV1_TX_16X16, // TX_16X16
   AV1_TX_32X32, // TX_32X32
   AV1_TX_32X32, // TX_64X64
   AV1_TX_4X8,   // TX_4X8
   AV1_TX_8X4,   // TX_8X4
   AV1_TX_8X16,  // TX_8X16
   AV1_TX_16X8,  // TX_16X8
   AV1_TX_16X32, // TX_16X32
   AV1_TX_32X16, // TX_32X16
   AV1_TX_32X32, // TX_32X64
   AV1_TX_32X32, // TX_64X32
   AV1_TX_4X16,  // TX_4X16
   AV1_TX_16X4,  // TX_16X4
   AV1_TX_8X32,  // TX_8X32
   AV1_TX_32X8,  // TX_32X8
   AV1_TX_16X32, // TX_16X64
   AV1_TX_32X16, // TX_64X16
};

   // Tx_Size_Sqr and Tx_Size_Sqr_Up (spec tables) mapped into [0..AV1_COEFF_TX_SIZES-1]
   // representing {4x4,8x8,16x16,32x32,64x64}. Used for txSzCtx derivation in coeffs().
   static const uint8_t kTxSizeSqr[AV1_TX_SIZES_ALL] = {
      0, // TX_4X4
      1, // TX_8X8
      2, // TX_16X16
      3, // TX_32X32
      4, // TX_64X64
      0, // TX_4X8
      0, // TX_8X4
      1, // TX_8X16
      1, // TX_16X8
      2, // TX_16X32
      2, // TX_32X16
      3, // TX_32X64
      3, // TX_64X32
      0, // TX_4X16
      0, // TX_16X4
      1, // TX_8X32
      1, // TX_32X8
      2, // TX_16X64
      2, // TX_64X16
   };

   static const uint8_t kTxSizeSqrUp[AV1_TX_SIZES_ALL] = {
      0, // TX_4X4
      1, // TX_8X8
      2, // TX_16X16
      3, // TX_32X32
      4, // TX_64X64
      1, // TX_4X8
      1, // TX_8X4
      2, // TX_8X16
      2, // TX_16X8
      3, // TX_16X32
      3, // TX_32X16
      4, // TX_32X64
      4, // TX_64X32
      2, // TX_4X16
      2, // TX_16X4
      3, // TX_8X32
      3, // TX_32X8
      4, // TX_16X64
      4, // TX_64X16
   };

   // Tx_Width_Log2 and Tx_Height_Log2 (spec tables) for our TxSize enum.
   static const uint8_t kTxWidthLog2[AV1_TX_SIZES_ALL] = {
      2, // TX_4X4
      3, // TX_8X8
      4, // TX_16X16
      5, // TX_32X32
      6, // TX_64X64
      2, // TX_4X8
      3, // TX_8X4
      3, // TX_8X16
      4, // TX_16X8
      4, // TX_16X32
      5, // TX_32X16
      5, // TX_32X64
      6, // TX_64X32
      2, // TX_4X16
      4, // TX_16X4
      3, // TX_8X32
      5, // TX_32X8
      4, // TX_16X64
      6, // TX_64X16
   };

   static const uint8_t kTxHeightLog2[AV1_TX_SIZES_ALL] = {
      2, // TX_4X4
      3, // TX_8X8
      4, // TX_16X16
      5, // TX_32X32
      6, // TX_64X64
      3, // TX_4X8
      2, // TX_8X4
      4, // TX_8X16
      3, // TX_16X8
      5, // TX_16X32
      4, // TX_32X16
      6, // TX_32X64
      5, // TX_64X32
      4, // TX_4X16
      2, // TX_16X4
      5, // TX_8X32
      3, // TX_32X8
      6, // TX_16X64
      4, // TX_64X16
   };

#define AV1_DIRECTIONAL_MODES 8u
#define AV1_MAX_ANGLE_DELTA 3u
#define AV1_ANGLE_DELTA_SYMBOLS (2u * AV1_MAX_ANGLE_DELTA + 1u)

// Spec constants for read_delta_qindex/read_delta_lf.
#define AV1_DELTA_Q_SMALL 3u
#define AV1_DELTA_LF_SMALL 3u
#define AV1_DELTA_Q_ABS_SYMBOLS (AV1_DELTA_Q_SMALL + 1u)  // symbols 0..DELTA_Q_SMALL
#define AV1_DELTA_LF_ABS_SYMBOLS (AV1_DELTA_LF_SMALL + 1u) // symbols 0..DELTA_LF_SMALL
#define AV1_FRAME_LF_COUNT 4u
#define AV1_MAX_LOOP_FILTER 63

// Segmentation (tile-coded) constants.
#define AV1_MAX_SEGMENTS 8u
#define AV1_SEGMENT_ID_CONTEXTS 3u

typedef enum {
   AV1_PARTITION_NONE = 0,
   AV1_PARTITION_HORZ = 1,
   AV1_PARTITION_VERT = 2,
   AV1_PARTITION_SPLIT = 3,
   AV1_PARTITION_HORZ_A = 4,
   AV1_PARTITION_HORZ_B = 5,
   AV1_PARTITION_VERT_A = 6,
   AV1_PARTITION_VERT_B = 7,
   AV1_PARTITION_HORZ_4 = 8,
   AV1_PARTITION_VERT_4 = 9,
} Av1PartitionType;

typedef struct {
   uint8_t wlog2;
   uint8_t hlog2;

   // Per-MI skip_txfm flag for ctx derivation (0/1). Default 0.
   uint8_t skip;

   // Per-MI luma intra mode (symbol in [0..AV1_INTRA_MODES-1]). Default 0.
   uint8_t y_mode;

   // Per-MI palette sizes (0 if no palette). Default 0.
   uint8_t palette_y_size;
   uint8_t palette_uv_size;

   // Per-MI segment_id (0..7). Default 0.
   uint8_t segment_id;
} Av1MiSize;

typedef struct {
   // Mutable per-tile CDF copies (may be updated adaptively).
   uint16_t w8[AV1_PARTITION_CONTEXTS][5];
   uint16_t w16[AV1_PARTITION_CONTEXTS][11];
   uint16_t w32[AV1_PARTITION_CONTEXTS][11];
   uint16_t w64[AV1_PARTITION_CONTEXTS][11];
   uint16_t w128[AV1_PARTITION_CONTEXTS][9];
} Av1TilePartitionCdfs;

typedef struct {
   // Mutable per-tile CDF copies.
   uint16_t skip[AV1_SKIP_CONTEXTS][3];

   // Mutable per-tile CDF copies.
   uint16_t y_mode[AV1_Y_MODE_CONTEXTS][AV1_INTRA_MODES + 1u];

   // Mutable per-tile CDF copies.
   uint16_t uv_mode_cfl_not_allowed[AV1_INTRA_MODES][AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED + 1u];

   // Mutable per-tile CDF copies.
   uint16_t uv_mode_cfl_allowed[AV1_INTRA_MODES][AV1_UV_INTRA_MODES_CFL_ALLOWED + 1u];

   // Mutable per-tile CDF copies.
   uint16_t angle_delta[AV1_DIRECTIONAL_MODES][AV1_ANGLE_DELTA_SYMBOLS + 1u];

   // Mutable per-tile CDF copies.
   uint16_t cfl_sign[AV1_CFL_JOINT_SIGNS + 1u];

   // Mutable per-tile CDF copies.
   uint16_t cfl_alpha[AV1_CFL_ALPHA_CONTEXTS][AV1_CFL_ALPHABET_SIZE + 1u];

   // Mutable per-tile CDF copies.
   uint16_t filter_intra_mode[AV1_INTRA_FILTER_MODES + 1u];

   // Mutable per-tile CDF copies.
   uint16_t filter_intra[AV1_BLOCK_SIZES][3];

   // Mutable per-tile CDF copies.
   uint16_t palette_y_mode[AV1_PALETTE_BLOCK_SIZE_CONTEXTS][AV1_PALETTE_Y_MODE_CONTEXTS][3];
   uint16_t palette_uv_mode[AV1_PALETTE_UV_MODE_CONTEXTS][3];

   // Mutable per-tile CDF copies.
   uint16_t palette_y_size[AV1_PALETTE_BLOCK_SIZE_CONTEXTS][AV1_PALETTE_SIZES + 1u];
   uint16_t palette_uv_size[AV1_PALETTE_BLOCK_SIZE_CONTEXTS][AV1_PALETTE_SIZES + 1u];

   // Mutable per-tile CDF copies: segment_id.
   uint16_t segment_id[AV1_SEGMENT_ID_CONTEXTS][AV1_MAX_SEGMENTS + 1u];

   // Mutable per-tile CDF copies (TxMode == TX_MODE_SELECT): tx_depth.
   uint16_t tx8x8[AV1_TX_SIZE_CONTEXTS][3];
   uint16_t tx16x16[AV1_TX_SIZE_CONTEXTS][4];
   uint16_t tx32x32[AV1_TX_SIZE_CONTEXTS][4];
   uint16_t tx64x64[AV1_TX_SIZE_CONTEXTS][4];

   // Mutable per-tile CDF copies: intra_tx_type (transform_type syntax).
   // Spec:
   //   TileIntraTxTypeSet1Cdf[ Tx_Size_Sqr[ txSz ] ][ intraDir ]
   //   TileIntraTxTypeSet2Cdf[ Tx_Size_Sqr[ txSz ] ][ intraDir ]
   uint16_t intra_tx_type_set1[2][AV1_INTRA_MODES][8];
   uint16_t intra_tx_type_set2[3][AV1_INTRA_MODES][6];

   // Mutable per-tile CDF copies: delta_q_abs and delta_lf_abs.
   // Spec: TileDeltaQCdf, TileDeltaLFCdf, TileDeltaLFMultiCdf[i].
   uint16_t delta_q_abs[AV1_DELTA_Q_ABS_SYMBOLS + 1u];
   uint16_t delta_lf_abs[AV1_DELTA_LF_ABS_SYMBOLS + 1u];
   uint16_t delta_lf_multi[AV1_FRAME_LF_COUNT][AV1_DELTA_LF_ABS_SYMBOLS + 1u];

   // Spec-style runtime state (probe-only for now).
   uint32_t current_qindex;
   int32_t delta_lf_state[AV1_FRAME_LF_COUNT];
} Av1TileSkipCdfs;

typedef struct {
   // Mutable per-tile CDF copies (may be updated adaptively).
   // Only txb_skip is needed for the first coeff symbol (all_zero).
   uint16_t txb_skip[AV1_COEFF_TX_SIZES][AV1_TXB_SKIP_CONTEXTS][3];

   // Mutable per-tile CDF copies: eob_pt_* (ptype: 0=luma, 1=chroma).
   uint16_t eob_pt_16[AV1_PLANE_TYPES][2][6];
   uint16_t eob_pt_32[AV1_PLANE_TYPES][2][7];
   uint16_t eob_pt_64[AV1_PLANE_TYPES][2][8];
   uint16_t eob_pt_128[AV1_PLANE_TYPES][2][9];
   uint16_t eob_pt_256[AV1_PLANE_TYPES][2][10];
   uint16_t eob_pt_512[AV1_PLANE_TYPES][11];
   uint16_t eob_pt_1024[AV1_PLANE_TYPES][12];

   // Mutable per-tile CDF copies: eob_extra.
   // Spec: TileEobExtraCdf[ txSzCtx ][ ptype ][ eobPt - 3 ].
   uint16_t eob_extra[AV1_COEFF_TX_SIZES][AV1_PLANE_TYPES][AV1_EOB_COEF_CONTEXTS][3];

   // Mutable per-tile CDF copies: coeff_base_eob.
   // Spec: TileCoeffBaseEobCdf[ txSzCtx ][ ptype ][ ctx ].
   uint16_t coeff_base_eob[AV1_COEFF_TX_SIZES][AV1_PLANE_TYPES][AV1_SIG_COEF_CONTEXTS_EOB][4];

   // Mutable per-tile CDF copies: coeff_base.
   // Spec: TileCoeffBaseCdf[ txSzCtx ][ ptype ][ ctx ].
   uint16_t coeff_base[AV1_COEFF_TX_SIZES][AV1_PLANE_TYPES][AV1_SIG_COEF_CONTEXTS][5];

   // Mutable per-tile CDF copies: coeff_br.
   // Spec: TileCoeffBrCdf[ Min(txSzCtx, TX_32X32) ][ ptype ][ ctx ].
   uint16_t coeff_br[AV1_COEFF_BR_TX_SIZES][AV1_PLANE_TYPES][AV1_LEVEL_CONTEXTS][AV1_BR_CDF_SIZE + 1u];

   // Mutable per-tile CDF copies: dc_sign.
   // Spec: TileDcSignCdf[ ptype ][ ctx ].
   uint16_t dc_sign[AV1_PLANE_TYPES][AV1_DC_SIGN_CONTEXTS][3];
} Av1TileCoeffCdfs;

typedef struct {
   // Spec-style coefficient contexts, per plane.
   // Indexed by x4 (columns) or y4 (rows) in *that plane's* MI units.
   uint8_t *above_level[AV1_MAX_PLANES];
   uint8_t *left_level[AV1_MAX_PLANES];
   uint8_t *above_dc[AV1_MAX_PLANES];
   uint8_t *left_dc[AV1_MAX_PLANES];
   uint32_t cols[AV1_MAX_PLANES];
   uint32_t rows[AV1_MAX_PLANES];
} Av1TileCoeffCtx;

static void tile_coeff_ctx_free(Av1TileCoeffCtx *ctx);

static bool tile_coeff_ctx_init(Av1TileCoeffCtx *ctx,
                                uint32_t tile_mi_cols,
                                uint32_t tile_mi_rows,
                                uint32_t mono_chrome,
                                uint32_t subsampling_x,
                                uint32_t subsampling_y,
                                char *err,
                                size_t err_cap) {
   if (!ctx) {
      snprintf(err, err_cap, "invalid coeff ctx");
      return false;
   }
   memset(ctx, 0, sizeof(*ctx));

   // Plane 0 always present.
   ctx->cols[0] = tile_mi_cols;
   ctx->rows[0] = tile_mi_rows;
   // Planes 1/2 only present when chroma exists.
   if (!mono_chrome) {
      ctx->cols[1] = tile_mi_cols >> subsampling_x;
      ctx->rows[1] = tile_mi_rows >> subsampling_y;
      ctx->cols[2] = ctx->cols[1];
      ctx->rows[2] = ctx->rows[1];
   }

   for (uint32_t plane = 0; plane < AV1_MAX_PLANES; plane++) {
      const uint32_t cols = ctx->cols[plane];
      const uint32_t rows = ctx->rows[plane];
      if (cols == 0u && rows == 0u) {
         continue;
      }

      if (cols > 0u) {
         ctx->above_level[plane] = (uint8_t *)calloc((size_t)cols, sizeof(uint8_t));
         ctx->above_dc[plane] = (uint8_t *)calloc((size_t)cols, sizeof(uint8_t));
         if (!ctx->above_level[plane] || !ctx->above_dc[plane]) {
            snprintf(err, err_cap, "out of memory allocating coeff above ctx (plane=%u cols=%u)", plane, cols);
            tile_coeff_ctx_free(ctx);
            return false;
         }
      }

      if (rows > 0u) {
         ctx->left_level[plane] = (uint8_t *)calloc((size_t)rows, sizeof(uint8_t));
         ctx->left_dc[plane] = (uint8_t *)calloc((size_t)rows, sizeof(uint8_t));
         if (!ctx->left_level[plane] || !ctx->left_dc[plane]) {
            snprintf(err, err_cap, "out of memory allocating coeff left ctx (plane=%u rows=%u)", plane, rows);
            tile_coeff_ctx_free(ctx);
            return false;
         }
      }
   }
   return true;
}

static void tile_coeff_ctx_free(Av1TileCoeffCtx *ctx) {
   if (!ctx) return;
   for (uint32_t plane = 0; plane < AV1_MAX_PLANES; plane++) {
      free(ctx->above_level[plane]);
      free(ctx->left_level[plane]);
      free(ctx->above_dc[plane]);
      free(ctx->left_dc[plane]);
   }
   memset(ctx, 0, sizeof(*ctx));
}

static uint32_t dc_sign_ctx(const Av1TileCoeffCtx *ctx, uint32_t plane, uint32_t x4, uint32_t y4, uint32_t w4, uint32_t h4) {
   if (!ctx || plane >= AV1_MAX_PLANES) {
      return 0u;
   }

   const uint8_t *above_dc = ctx->above_dc[plane];
   const uint8_t *left_dc = ctx->left_dc[plane];
   const uint32_t cols = ctx->cols[plane];
   const uint32_t rows = ctx->rows[plane];
   if (!above_dc && !left_dc) {
      return 0u;
   }

   int32_t dcSign = 0;
   for (uint32_t k = 0u; k < w4; k++) {
      const uint32_t x = x4 + k;
      if (above_dc && x < cols) {
         const uint8_t sign = above_dc[x];
         if (sign == 1u) {
            dcSign--;
         } else if (sign == 2u) {
            dcSign++;
         }
      }
   }
   for (uint32_t k = 0u; k < h4; k++) {
      const uint32_t y = y4 + k;
      if (left_dc && y < rows) {
         const uint8_t sign = left_dc[y];
         if (sign == 1u) {
            dcSign--;
         } else if (sign == 2u) {
            dcSign++;
         }
      }
   }

   if (dcSign < 0) return 1u;
   if (dcSign > 0) return 2u;
   return 0u;
}

static uint32_t txb_skip_ctx(const Av1TileCoeffCtx *ctx,
                             uint32_t plane,
                             uint32_t x4,
                             uint32_t y4,
                             uint32_t w4,
                             uint32_t h4,
                             uint32_t bw_px,
                             uint32_t bh_px,
                             uint32_t tx_size) {
   // Spec ctx computation for all_zero / txb_skip (see local av1bitstream.html).
   if (!ctx || plane >= AV1_MAX_PLANES || tx_size >= AV1_TX_SIZES_ALL) return 0u;

   const uint32_t tx_w_px = 1u << kTxWidthLog2[tx_size];
   const uint32_t tx_h_px = 1u << kTxHeightLog2[tx_size];

   const uint8_t *above_level = ctx->above_level[plane];
   const uint8_t *left_level = ctx->left_level[plane];
   const uint8_t *above_dc = ctx->above_dc[plane];
   const uint8_t *left_dc = ctx->left_dc[plane];
   const uint32_t cols = ctx->cols[plane];
   const uint32_t rows = ctx->rows[plane];

   if (plane == 0u) {
      uint32_t top = 0u;
      uint32_t left = 0u;
      for (uint32_t k = 0u; k < w4; k++) {
         const uint32_t x = x4 + k;
         if (above_level && x < cols) {
            if (above_level[x] > top) top = above_level[x];
         }
      }
      for (uint32_t k = 0u; k < h4; k++) {
         const uint32_t y = y4 + k;
         if (left_level && y < rows) {
            if (left_level[y] > left) left = left_level[y];
         }
      }

      if (top > 255u) top = 255u;
      if (left > 255u) left = 255u;

      // If transform covers the whole residual plane block, ctx is forced to 0.
      if (bw_px == tx_w_px && bh_px == tx_h_px) {
         return 0u;
      }

      if (top == 0u && left == 0u) {
         return 1u;
      }
      if (top == 0u || left == 0u) {
         return 2u + ((top > 3u || left > 3u) ? 1u : 0u);
      }
      if ((top <= 3u) && (left <= 3u)) {
         return 4u;
      }
      if ((top <= 3u) || (left <= 3u)) {
         return 5u;
      }
      return 6u;
   }

   // plane > 0
   uint32_t above = 0u;
   uint32_t left = 0u;
   for (uint32_t i = 0u; i < w4; i++) {
      const uint32_t x = x4 + i;
      if (x < cols) {
         if (above_level) above |= above_level[x];
         if (above_dc) above |= above_dc[x];
      }
   }
   for (uint32_t i = 0u; i < h4; i++) {
      const uint32_t y = y4 + i;
      if (y < rows) {
         if (left_level) left |= left_level[y];
         if (left_dc) left |= left_dc[y];
      }
   }

   uint32_t ctxv = ((above != 0u) ? 1u : 0u) + ((left != 0u) ? 1u : 0u);
   ctxv += 7u;
   if (bw_px * bh_px > tx_w_px * tx_h_px) {
      ctxv += 3u;
   }
   return ctxv;
}

static void cdf_copy_u16(uint16_t *dst, const uint16_t *src, size_t n) {
   // Includes the trailing count slot.
   memcpy(dst, src, n * sizeof(uint16_t));
}

// Default txb_skip CDF table from the AV1 spec (Default_Txb_Skip_Cdf[qctx][txSzCtx][ctx][3]).
static const uint16_t kDefaultTxbSkipCdf[AV1_COEFF_CDF_Q_CTXS][AV1_COEFF_TX_SIZES][AV1_TXB_SKIP_CONTEXTS][3] =
#include "av1_default_cdfs_txb_skip.inc"
    ;

// Default EOB point CDF tables from the AV1 spec (see local av1bitstream.html).
// We embed only PLANE_TYPES==0 (luma).
static const uint16_t kDefaultEobPt16CdfLuma[4][2][6] = {
   {
      {840, 1039, 1980, 4895, 32768, 0},
      {370, 671, 1883, 4471, 32768, 0},
   },
   {
      {2125, 2551, 5165, 8946, 32768, 0},
      {513, 765, 1859, 6339, 32768, 0},
   },
   {
      {4016, 4897, 8881, 14968, 32768, 0},
      {716, 1105, 2646, 10056, 32768, 0},
   },
   {
      {6708, 8958, 14746, 22133, 32768, 0},
      {1222, 2074, 4783, 15410, 32768, 0},
   },
};

static const uint16_t kDefaultEobPt32CdfLuma[4][2][7] = {
   {
      {400, 520, 977, 2102, 6542, 32768, 0},
      {210, 405, 1315, 3326, 7537, 32768, 0},
   },
   {
      {989, 1249, 2019, 4151, 10785, 32768, 0},
      {313, 441, 1099, 2917, 8562, 32768, 0},
   },
   {
      {2515, 3003, 4452, 8162, 16041, 32768, 0},
      {574, 821, 1836, 5089, 13128, 32768, 0},
   },
   {
      {4617, 5709, 8446, 13584, 23135, 32768, 0},
      {1156, 1702, 3675, 9274, 20539, 32768, 0},
   },
};

static const uint16_t kDefaultEobPt64CdfLuma[4][2][8] = {
   {
      {329, 498, 1101, 1784, 3265, 7758, 32768, 0},
      {335, 730, 1459, 5494, 8755, 12997, 32768, 0},
   },
   {
      {1260, 1446, 2253, 3712, 6652, 13369, 32768, 0},
      {401, 605, 1029, 2563, 5845, 12626, 32768, 0},
   },
   {
      {2374, 2772, 4583, 7276, 12288, 19706, 32768, 0},
      {497, 810, 1315, 3000, 7004, 15641, 32768, 0},
   },
   {
      {6307, 7541, 12060, 16358, 22553, 27865, 32768, 0},
      {1289, 2320, 3971, 7926, 14153, 24291, 32768, 0},
   },
};

static const uint16_t kDefaultEobPt128CdfLuma[4][2][9] = {
   {
      {219, 482, 1140, 2091, 3680, 6028, 12586, 32768, 0},
      {371, 699, 1254, 4830, 9479, 12562, 17497, 32768, 0},
   },
   {
      {685, 933, 1488, 2714, 4766, 8562, 19254, 32768, 0},
      {217, 352, 618, 2303, 5261, 9969, 17472, 32768, 0},
   },
   {
      {1366, 1738, 2527, 5016, 9355, 15797, 24643, 32768, 0},
      {354, 558, 944, 2760, 7287, 14037, 21779, 32768, 0},
   },
   {
      {3472, 4885, 7489, 12481, 18517, 24536, 29635, 32768, 0},
      {886, 1731, 3271, 8469, 15569, 22126, 28383, 32768, 0},
   },
};

static const uint16_t kDefaultEobPt256CdfLuma[4][2][10] = {
   {
      {310, 584, 1887, 3589, 6168, 8611, 11352, 15652, 32768, 0},
      {998, 1850, 2998, 5604, 17341, 19888, 22899, 25583, 32768, 0},
   },
   {
      {1448, 2109, 4151, 6263, 9329, 13260, 17944, 23300, 32768, 0},
      {399, 1019, 1749, 3038, 10444, 15546, 22739, 27294, 32768, 0},
   },
   {
      {3089, 3920, 6038, 9460, 14266, 19881, 25766, 29176, 32768, 0},
      {1084, 2358, 3488, 5122, 11483, 18103, 26023, 29799, 32768, 0},
   },
   {
      {5348, 7113, 11820, 15924, 22106, 26777, 30334, 31757, 32768, 0},
      {2453, 4474, 6307, 8777, 16474, 22975, 29000, 31547, 32768, 0},
   },
};

static const uint16_t kDefaultEobPt512CdfLuma[4][11] = {
   {641, 983, 3707, 5430, 10234, 14958, 18788, 23412, 26061, 32768, 0},
   {1230, 2278, 5035, 7776, 11871, 15346, 19590, 24584, 28749, 32768, 0},
   {2624, 3936, 6480, 9686, 13979, 17726, 23267, 28410, 31078, 32768, 0},
   {5927, 7809, 10923, 14597, 19439, 24135, 28456, 31142, 32060, 32768, 0},
};

static const uint16_t kDefaultEobPt1024CdfLuma[4][12] = {
   {393, 421, 751, 1623, 3160, 6352, 13345, 18047, 22571, 25830, 32768, 0},
   {696, 948, 3145, 5702, 9706, 13217, 17851, 21856, 25692, 28034, 32768, 0},
   {2784, 3831, 7041, 10521, 14847, 18844, 23155, 26682, 29229, 31045, 32768, 0},
   {6698, 8334, 11961, 15762, 20186, 23862, 27434, 29326, 31082, 32050, 32768, 0},
};

static const uint16_t kDefaultEobExtraCdfLuma[4][5][9][3] = {
   {
      {
         {16961, 32768, 0},
         {17223, 32768, 0},
         {7621, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {20401, 32768, 0},
         {17025, 32768, 0},
         {12845, 32768, 0},
         {12873, 32768, 0},
         {14094, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {23905, 32768, 0},
         {17194, 32768, 0},
         {16170, 32768, 0},
         {17695, 32768, 0},
         {13826, 32768, 0},
         {15810, 32768, 0},
         {12036, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {27399, 32768, 0},
         {16327, 32768, 0},
         {18071, 32768, 0},
         {19584, 32768, 0},
         {20721, 32768, 0},
         {18432, 32768, 0},
         {19560, 32768, 0},
         {10150, 32768, 0},
         {8805, 32768, 0},
      },
      {
         {23406, 32768, 0},
         {21845, 32768, 0},
         {18432, 32768, 0},
         {16384, 32768, 0},
         {17096, 32768, 0},
         {12561, 32768, 0},
         {17320, 32768, 0},
         {22395, 32768, 0},
         {21370, 32768, 0},
      },
   },
   {
      {
         {17471, 32768, 0},
         {20223, 32768, 0},
         {11357, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {20430, 32768, 0},
         {20662, 32768, 0},
         {15367, 32768, 0},
         {16970, 32768, 0},
         {14657, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {22409, 32768, 0},
         {21012, 32768, 0},
         {15650, 32768, 0},
         {17395, 32768, 0},
         {15469, 32768, 0},
         {20205, 32768, 0},
         {19511, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {25991, 32768, 0},
         {20314, 32768, 0},
         {17731, 32768, 0},
         {19678, 32768, 0},
         {18649, 32768, 0},
         {17307, 32768, 0},
         {21798, 32768, 0},
         {17549, 32768, 0},
         {15630, 32768, 0},
      },
      {
         {26605, 32768, 0},
         {11304, 32768, 0},
         {16726, 32768, 0},
         {16560, 32768, 0},
         {20866, 32768, 0},
         {23524, 32768, 0},
         {19878, 32768, 0},
         {13469, 32768, 0},
         {23084, 32768, 0},
      },
   },
   {
      {
         {18983, 32768, 0},
         {20512, 32768, 0},
         {14885, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {19139, 32768, 0},
         {21487, 32768, 0},
         {18959, 32768, 0},
         {20910, 32768, 0},
         {19089, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {19833, 32768, 0},
         {21502, 32768, 0},
         {17485, 32768, 0},
         {20267, 32768, 0},
         {18353, 32768, 0},
         {23329, 32768, 0},
         {21478, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {23312, 32768, 0},
         {21607, 32768, 0},
         {16526, 32768, 0},
         {18957, 32768, 0},
         {18034, 32768, 0},
         {18934, 32768, 0},
         {24247, 32768, 0},
         {16921, 32768, 0},
         {17080, 32768, 0},
      },
      {
         {26998, 32768, 0},
         {16737, 32768, 0},
         {17838, 32768, 0},
         {18922, 32768, 0},
         {19515, 32768, 0},
         {18636, 32768, 0},
         {17333, 32768, 0},
         {15776, 32768, 0},
         {22658, 32768, 0},
      },
   },
   {
      {
         {20177, 32768, 0},
         {20789, 32768, 0},
         {20262, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {20238, 32768, 0},
         {21057, 32768, 0},
         {19159, 32768, 0},
         {22337, 32768, 0},
         {20159, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {19941, 32768, 0},
         {20527, 32768, 0},
         {21470, 32768, 0},
         {22487, 32768, 0},
         {19558, 32768, 0},
         {22354, 32768, 0},
         {20331, 32768, 0},
         {16384, 32768, 0},
         {16384, 32768, 0},
      },
      {
         {21442, 32768, 0},
         {22358, 32768, 0},
         {18503, 32768, 0},
         {20291, 32768, 0},
         {19945, 32768, 0},
         {21294, 32768, 0},
         {21178, 32768, 0},
         {19400, 32768, 0},
         {10556, 32768, 0},
      },
      {
         {26064, 32768, 0},
         {22098, 32768, 0},
         {19613, 32768, 0},
         {20525, 32768, 0},
         {17595, 32768, 0},
         {16618, 32768, 0},
         {20497, 32768, 0},
         {18989, 32768, 0},
         {15513, 32768, 0},
      },
   },
};

// Default coeff_base_eob CDF tables from the AV1 spec (see local av1bitstream.html).
// We embed only PLANE_TYPES==0 (luma).
static const uint16_t kDefaultCoeffBaseEobCdfLuma[4][5][4][4] = {
   {
      {
         {17837, 29055, 32768, 0},
         {29600, 31446, 32768, 0},
         {30844, 31878, 32768, 0},
         {24926, 28948, 32768, 0},
      },
      {
         {5717, 26477, 32768, 0},
         {30491, 31703, 32768, 0},
         {31550, 32158, 32768, 0},
         {29648, 31491, 32768, 0},
      },
      {
         {1786, 12612, 32768, 0},
         {30663, 31625, 32768, 0},
         {32339, 32468, 32768, 0},
         {31148, 31833, 32768, 0},
      },
      {
         {1787, 2532, 32768, 0},
         {30832, 31662, 32768, 0},
         {31824, 32682, 32768, 0},
         {32133, 32569, 32768, 0},
      },
      {
         {1725, 3449, 32768, 0},
         {31102, 31935, 32768, 0},
         {32457, 32613, 32768, 0},
         {32412, 32649, 32768, 0},
      },
   },
   {
      {
         {17560, 29888, 32768, 0},
         {29671, 31549, 32768, 0},
         {31007, 32056, 32768, 0},
         {27286, 30006, 32768, 0},
      },
      {
         {15239, 29932, 32768, 0},
         {31315, 32095, 32768, 0},
         {32130, 32434, 32768, 0},
         {30864, 31996, 32768, 0},
      },
      {
         {2644, 25198, 32768, 0},
         {32038, 32451, 32768, 0},
         {32639, 32695, 32768, 0},
         {32166, 32518, 32768, 0},
      },
      {
         {1044, 2257, 32768, 0},
         {30755, 31923, 32768, 0},
         {32208, 32693, 32768, 0},
         {32244, 32615, 32768, 0},
      },
      {
         {478, 1834, 32768, 0},
         {31005, 31987, 32768, 0},
         {32317, 32724, 32768, 0},
         {30865, 32648, 32768, 0},
      },
   },
   {
      {
         {20092, 30774, 32768, 0},
         {30695, 32020, 32768, 0},
         {31131, 32103, 32768, 0},
         {28666, 30870, 32768, 0},
      },
      {
         {18049, 30489, 32768, 0},
         {31706, 32286, 32768, 0},
         {32163, 32473, 32768, 0},
         {31550, 32184, 32768, 0},
      },
      {
         {12854, 29093, 32768, 0},
         {32272, 32558, 32768, 0},
         {32667, 32729, 32768, 0},
         {32306, 32585, 32768, 0},
      },
      {
         {2809, 19301, 32768, 0},
         {32205, 32622, 32768, 0},
         {32338, 32730, 32768, 0},
         {31786, 32616, 32768, 0},
      },
      {
         {935, 3382, 32768, 0},
         {30789, 31909, 32768, 0},
         {32466, 32756, 32768, 0},
         {30860, 32513, 32768, 0},
      },
   },
   {
      {
         {22497, 31198, 32768, 0},
         {31715, 32495, 32768, 0},
         {31606, 32337, 32768, 0},
         {30388, 31990, 32768, 0},
      },
      {
         {21457, 31043, 32768, 0},
         {31951, 32483, 32768, 0},
         {32153, 32562, 32768, 0},
         {31473, 32215, 32768, 0},
      },
      {
         {19980, 30591, 32768, 0},
         {32219, 32597, 32768, 0},
         {32581, 32706, 32768, 0},
         {31803, 32287, 32768, 0},
      },
      {
         {24647, 30463, 32768, 0},
         {32412, 32695, 32768, 0},
         {32468, 32720, 32768, 0},
         {31269, 32523, 32768, 0},
      },
      {
         {12358, 24977, 32768, 0},
         {31331, 32385, 32768, 0},
         {32634, 32756, 32768, 0},
         {30411, 32548, 32768, 0},
      },
   },
};

static uint32_t coeff_base_eob_ctx_from_c(uint32_t tx_size, uint32_t c) {
   // Spec: get_coeff_base_ctx(txSz,...,c,isEob=1) returns SIG_COEF_CONTEXTS-4..-1,
   // and coeff_base_eob uses:
   //   ctx = get_coeff_base_ctx(...) - SIG_COEF_CONTEXTS + SIG_COEF_CONTEXTS_EOB
   // which maps to 0..3 based on 'c' and adjusted transform dimensions.
   if (tx_size >= AV1_TX_SIZES_ALL) {
      return 0u;
   }
   const uint32_t adj = kAdjustedTxSize[tx_size];
   const uint32_t bwl = kTxWidthLog2[adj];
   const uint32_t height = 1u << kTxHeightLog2[adj];
   const uint32_t coeffs = height << bwl;

   if (c == 0u) {
      return 0u;
   }
   if (c <= (coeffs / 8u)) {
      return 1u;
   }
   if (c <= (coeffs / 4u)) {
      return 2u;
   }
   return 3u;
}

// Default partition CDF tables from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultPartitionW8Cdf[AV1_PARTITION_CONTEXTS][5] = {
    {19132, 25510, 30392, 32768, 0},
    {13928, 19855, 28540, 32768, 0},
    {12522, 23679, 28629, 32768, 0},
    {9896, 18783, 25853, 32768, 0},
};

static const uint16_t kDefaultPartitionW16Cdf[AV1_PARTITION_CONTEXTS][11] = {
    {15597, 20929, 24571, 26706, 27664, 28821, 29601, 30571, 31902, 32768, 0},
    {7925, 11043, 16785, 22470, 23971, 25043, 26651, 28701, 29834, 32768, 0},
    {5414, 13269, 15111, 20488, 22360, 24500, 25537, 26336, 32117, 32768, 0},
    {2662, 6362, 8614, 20860, 23053, 24778, 26436, 27829, 31171, 32768, 0},
};

static const uint16_t kDefaultPartitionW32Cdf[AV1_PARTITION_CONTEXTS][11] = {
    {18462, 20920, 23124, 27647, 28227, 29049, 29519, 30178, 31544, 32768, 0},
    {7689, 9060, 12056, 24992, 25660, 26182, 26951, 28041, 29052, 32768, 0},
    {6015, 9009, 10062, 24544, 25409, 26545, 27071, 27526, 32047, 32768, 0},
    {1394, 2208, 2796, 28614, 29061, 29466, 29840, 30185, 31899, 32768, 0},
};

static const uint16_t kDefaultPartitionW64Cdf[AV1_PARTITION_CONTEXTS][11] = {
    {20137, 21547, 23078, 29566, 29837, 30261, 30524, 30892, 31724, 32768, 0},
    {6732, 7490, 9497, 27944, 28250, 28515, 28969, 29630, 30104, 32768, 0},
    {5945, 7663, 8348, 28683, 29117, 29749, 30064, 30298, 32238, 32768, 0},
    {870, 1212, 1487, 31198, 31394, 31574, 31743, 31881, 32332, 32768, 0},
};

static const uint16_t kDefaultPartitionW128Cdf[AV1_PARTITION_CONTEXTS][9] = {
    {27899, 28219, 28529, 32484, 32539, 32619, 32639, 32768, 0},
    {6607, 6990, 8268, 32060, 32219, 32338, 32371, 32768, 0},
    {5429, 6676, 7122, 32027, 32227, 32531, 32582, 32768, 0},
    {711, 966, 1172, 32448, 32538, 32617, 32664, 32768, 0},
};

static const uint16_t kDefaultSkipCdf[AV1_SKIP_CONTEXTS][3] = {
   {31671, 32768, 0},
   {16515, 32768, 0},
   {4576, 32768, 0},
};

static const uint16_t kDefaultDeltaQCdf[AV1_DELTA_Q_ABS_SYMBOLS + 1u] = {28160, 32120, 32677, 32768, 0};
static const uint16_t kDefaultDeltaLFCdf[AV1_DELTA_LF_ABS_SYMBOLS + 1u] = {28160, 32120, 32677, 32768, 0};

static int32_t clip3_i32(int32_t lo, int32_t hi, int32_t v) {
   if (v < lo) return lo;
   if (v > hi) return hi;
   return v;
}

static uint32_t coeff_cdf_q_ctx_from_base_q_idx(uint32_t base_q_idx) {
   // init_coeff_cdfs() (spec): idx derived from base_q_idx thresholds.
   if (base_q_idx <= 20u) return 0u;
   if (base_q_idx <= 60u) return 1u;
   if (base_q_idx <= 120u) return 2u;
   return 3u;
}

static uint32_t qindex_for_segment(const Av1TileDecodeParams *params, uint32_t segment_id) {
   int32_t q = (int32_t)(params ? params->base_q_idx : 0u);
   if (params && params->segmentation_enabled && segment_id < 8u && params->seg_feature_enabled_alt_q[segment_id]) {
      q += params->seg_feature_data_alt_q[segment_id];
   }
   if (q < 0) {
      q = 0;
   } else if (q > 255) {
      q = 255;
   }
   return (uint32_t)q;
}

static bool lossless_for_segment(const Av1TileDecodeParams *params, uint32_t segment_id) {
   const uint32_t qindex = qindex_for_segment(params, segment_id);
   if (qindex != 0u) {
      return false;
   }
   if (!params) {
      return true;
   }
   return params->delta_q_y_dc == 0 && params->delta_q_u_dc == 0 && params->delta_q_u_ac == 0 &&
          params->delta_q_v_dc == 0 && params->delta_q_v_ac == 0;
}

static uint32_t tx_sz_ctx_from_tx_size(uint32_t tx_size) {
   if (tx_size >= AV1_TX_SIZES_ALL) {
      return 0u;
   }
   const uint32_t a = (uint32_t)kTxSizeSqr[tx_size];
   const uint32_t b = (uint32_t)kTxSizeSqrUp[tx_size];
   uint32_t ctx = (a + b + 1u) >> 1;
   if (ctx >= AV1_COEFF_TX_SIZES) {
      ctx = AV1_COEFF_TX_SIZES - 1u;
   }
   return ctx;
}

static uint32_t get_tx_set_intra(uint32_t tx_size, uint32_t reduced_tx_set) {
   // Spec get_tx_set(txSz) for is_inter==0.
   if (tx_size >= AV1_TX_SIZES_ALL) {
      return AV1_TX_SET_DCTONLY;
   }
   const uint32_t txSzSqr = (uint32_t)kTxSizeSqr[tx_size];
   const uint32_t txSzSqrUp = (uint32_t)kTxSizeSqrUp[tx_size];

   // txSzSqrUp > TX_32X32 => TX_SET_DCTONLY.
   if (txSzSqrUp > 3u) {
      return AV1_TX_SET_DCTONLY;
   }

   // Intra: txSzSqrUp == TX_32X32 => TX_SET_DCTONLY.
   if (txSzSqrUp == 3u) {
      return AV1_TX_SET_DCTONLY;
   }

   if (reduced_tx_set) {
      return AV1_TX_SET_INTRA_2;
   }
   if (txSzSqr == 2u /* TX_16X16 */) {
      return AV1_TX_SET_INTRA_2;
   }
   return AV1_TX_SET_INTRA_1;
}

static uint32_t get_tx_class_from_tx_type(uint32_t tx_type) {
   // Spec get_tx_class(txType).
   if (tx_type == AV1_TX_TYPE_V_DCT /* || V_ADST || V_FLIPADST (not present yet) */) {
      return AV1_TX_CLASS_VERT;
   }
   if (tx_type == AV1_TX_TYPE_H_DCT /* || H_ADST || H_FLIPADST (not present yet) */) {
      return AV1_TX_CLASS_HORIZ;
   }
   return AV1_TX_CLASS_2D;
}

static void build_scan(uint32_t tx_class, uint32_t width, uint32_t height, uint16_t *out_scan) {
   // Builds a scan[] array mapping coefficient index c -> raster pos.
   // - 2D: diagonal zigzag (Default_Scan_*).
   // - HORIZ: row-major (Mrow_Scan_*).
   // - VERT: column-major (Mcol_Scan_*).
   uint32_t k = 0u;
   if (tx_class == AV1_TX_CLASS_VERT) {
      for (uint32_t col = 0; col < width; col++) {
         for (uint32_t row = 0; row < height; row++) {
            out_scan[k++] = (uint16_t)(row * width + col);
         }
      }
      return;
   }
   if (tx_class == AV1_TX_CLASS_HORIZ) {
      for (uint32_t row = 0; row < height; row++) {
         for (uint32_t col = 0; col < width; col++) {
            out_scan[k++] = (uint16_t)(row * width + col);
         }
      }
      return;
   }

   const uint32_t max_sum = (width + height) ? (width + height - 2u) : 0u;
   for (uint32_t sum = 0u; sum <= max_sum; sum++) {
      if ((sum & 1u) == 0u) {
         // Even diagonals: down-left (increasing row).
         uint32_t row = (sum < (height - 1u)) ? sum : (height - 1u);
         uint32_t row_min = (sum > (width - 1u)) ? (sum - (width - 1u)) : 0u;
         for (; row + 1u > row_min; row--) {
            const uint32_t col = sum - row;
            out_scan[k++] = (uint16_t)(row * width + col);
            if (row == row_min) {
               break;
            }
         }
      } else {
         // Odd diagonals: up-right (increasing col).
         uint32_t col = (sum < (width - 1u)) ? sum : (width - 1u);
         uint32_t col_min = (sum > (height - 1u)) ? (sum - (height - 1u)) : 0u;
         for (; col + 1u > col_min; col--) {
            const uint32_t row = sum - col;
            out_scan[k++] = (uint16_t)(row * width + col);
            if (col == col_min) {
               break;
            }
         }
      }
   }
}

static uint32_t coeff_base_ctx_isEob0(uint32_t tx_size,
                                     uint32_t tx_type,
                                     uint32_t bwl,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t pos,
                                     const int32_t *quant) {
   static const int8_t kSigRefDiffOffset[3][AV1_SIG_REF_DIFF_OFFSET_NUM][2] = {
      // TX_CLASS_2D
      {{0, 1}, {1, 0}, {1, 1}, {0, 2}, {2, 0}},
      // TX_CLASS_HORIZ
      {{0, 1}, {1, 0}, {0, 2}, {0, 3}, {0, 4}},
      // TX_CLASS_VERT
      {{0, 1}, {1, 0}, {2, 0}, {3, 0}, {4, 0}},
   };

   const uint32_t tx_class = get_tx_class_from_tx_type(tx_type);
   const uint32_t row = pos >> bwl;
   const uint32_t col = pos - (row << bwl);

   uint32_t mag = 0u;
   for (uint32_t i = 0u; i < AV1_SIG_REF_DIFF_OFFSET_NUM; i++) {
      const int32_t rr = (int32_t)row + (int32_t)kSigRefDiffOffset[tx_class][i][0];
      const int32_t cc = (int32_t)col + (int32_t)kSigRefDiffOffset[tx_class][i][1];
      if (rr >= 0 && cc >= 0 && (uint32_t)rr < height && (uint32_t)cc < width) {
         const uint32_t idx = ((uint32_t)rr << bwl) + (uint32_t)cc;
         int32_t v = quant[idx];
         if (v < 0) v = -v;
         uint32_t a = (uint32_t)v;
         if (a > 3u) a = 3u;
         mag += a;
      }
   }

   uint32_t ctx = (mag + 1u) >> 1;
   if (ctx > 4u) ctx = 4u;

   if (tx_class == AV1_TX_CLASS_2D) {
      if (row == 0u && col == 0u) {
         return 0u;
      }
      const uint32_t rr = (row < 4u) ? row : 4u;
      const uint32_t cc = (col < 4u) ? col : 4u;
      return ctx + (uint32_t)kCoeffBaseCtxOffset[tx_size][rr][cc];
   }

   const uint32_t idx = (tx_class == AV1_TX_CLASS_VERT) ? row : col;
   const uint32_t cap = (idx < 2u) ? idx : 2u;
   static const uint32_t kCoeffBasePosCtxOffset[3] = {
      AV1_SIG_COEF_CONTEXTS_2D,
      AV1_SIG_COEF_CONTEXTS_2D + 5u,
      AV1_SIG_COEF_CONTEXTS_2D + 10u,
   };
   return ctx + kCoeffBasePosCtxOffset[cap];
}

static uint32_t coeff_br_ctx(uint32_t tx_size, uint32_t tx_type, uint32_t pos, const int32_t *quant) {
   // Spec ctx computation for coeff_br (see local av1bitstream.html).
   static const int8_t kMagRefOffsetWithTxClass[3][3][2] = {
      {{0, 1}, {1, 0}, {1, 1}},
      {{0, 1}, {1, 0}, {0, 2}},
      {{0, 1}, {1, 0}, {2, 0}},
   };

   if (tx_size >= AV1_TX_SIZES_ALL) {
      return 0u;
   }

   const uint32_t adj = kAdjustedTxSize[tx_size];
   const uint32_t bwl = kTxWidthLog2[adj];
   const uint32_t txw = 1u << bwl;
   const uint32_t txh = 1u << kTxHeightLog2[adj];

   const uint32_t row = pos >> bwl;
   const uint32_t col = pos - (row << bwl);

   const uint32_t tx_class = get_tx_class_from_tx_type(tx_type);
   uint32_t mag = 0u;
   for (uint32_t idx = 0u; idx < 3u; idx++) {
      const int32_t refRow = (int32_t)row + (int32_t)kMagRefOffsetWithTxClass[tx_class][idx][0];
      const int32_t refCol = (int32_t)col + (int32_t)kMagRefOffsetWithTxClass[tx_class][idx][1];
      if (refRow >= 0 && refCol >= 0 && (uint32_t)refRow < txh && (uint32_t)refCol < txw) {
         int32_t v = quant[(uint32_t)refRow * txw + (uint32_t)refCol];
         if (v < 0) v = -v;
         const uint32_t q = (uint32_t)v;
         const uint32_t cap = AV1_COEFF_BASE_RANGE + AV1_NUM_BASE_LEVELS + 1u;
         mag += (q > cap) ? cap : q;
      }
   }

   mag = (mag + 1u) >> 1;
   if (mag > 6u) mag = 6u;

   uint32_t ctx = 0u;
   if (pos == 0u) {
      ctx = mag;
   } else if (tx_class == AV1_TX_CLASS_2D) {
      if (row < 2u && col < 2u) {
         ctx = mag + 7u;
      } else {
         ctx = mag + 14u;
      }
   } else if (tx_class == AV1_TX_CLASS_HORIZ) {
      ctx = (col == 0u) ? (mag + 7u) : (mag + 14u);
   } else {
      ctx = (row == 0u) ? (mag + 7u) : (mag + 14u);
   }

   if (ctx >= AV1_LEVEL_CONTEXTS) {
      ctx = AV1_LEVEL_CONTEXTS - 1u;
   }
   return ctx;
}

// Default y_mode CDF table from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultYModeCdf[AV1_Y_MODE_CONTEXTS][AV1_INTRA_MODES + 1u] = {
   {22801, 23489, 24293, 24756, 25601, 26123, 26606, 27418, 27945, 29228, 29685, 30349, 32768, 0},
   {18673, 19845, 22631, 23318, 23950, 24649, 25527, 27364, 28152, 29701, 29984, 30852, 32768, 0},
   {19770, 20979, 23396, 23939, 24241, 24654, 25136, 27073, 27830, 29360, 29730, 30659, 32768, 0},
   {20155, 21301, 22838, 23178, 23261, 23533, 23703, 24804, 25352, 26575, 27016, 28049, 32768, 0},
};

// Default uv_mode CDF table (CFL not allowed) from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultUvModeCflNotAllowedCdf[AV1_INTRA_MODES][AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED + 1u] = {
   {22631, 24152, 25378, 25661, 25986, 26520, 27055, 27923, 28244, 30059, 30941, 31961, 32768, 0},
   {9513, 26881, 26973, 27046, 27118, 27664, 27739, 27824, 28359, 29505, 29800, 31796, 32768, 0},
   {9845, 9915, 28663, 28704, 28757, 28780, 29198, 29822, 29854, 30764, 31777, 32029, 32768, 0},
   {13639, 13897, 14171, 25331, 25606, 25727, 25953, 27148, 28577, 30612, 31355, 32493, 32768, 0},
   {9764, 9835, 9930, 9954, 25386, 27053, 27958, 28148, 28243, 31101, 31744, 32363, 32768, 0},
   {11825, 13589, 13677, 13720, 15048, 29213, 29301, 29458, 29711, 31161, 31441, 32550, 32768, 0},
   {14175, 14399, 16608, 16821, 17718, 17775, 28551, 30200, 30245, 31837, 32342, 32667, 32768, 0},
   {12885, 13038, 14978, 15590, 15673, 15748, 16176, 29128, 29267, 30643, 31961, 32461, 32768, 0},
   {12026, 13661, 13874, 15305, 15490, 15726, 15995, 16273, 28443, 30388, 30767, 32416, 32768, 0},
   {19052, 19840, 20579, 20916, 21150, 21467, 21885, 22719, 23174, 28861, 30379, 32175, 32768, 0},
   {18627, 19649, 20974, 21219, 21492, 21816, 22199, 23119, 23527, 27053, 31397, 32148, 32768, 0},
   {17026, 19004, 19997, 20339, 20586, 21103, 21349, 21907, 22482, 25896, 26541, 31819, 32768, 0},
   {12124, 13759, 14959, 14992, 15007, 15051, 15078, 15166, 15255, 15753, 16039, 16606, 32768, 0},
};

// Default uv_mode CDF table (CFL allowed) from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultUvModeCflAllowedCdf[AV1_INTRA_MODES][AV1_UV_INTRA_MODES_CFL_ALLOWED + 1u] = {
   {10407, 11208, 12900, 13181, 13823, 14175, 14899, 15656, 15986, 20086, 20995, 22455, 24212, 32768, 0},
   {4532, 19780, 20057, 20215, 20428, 21071, 21199, 21451, 22099, 24228, 24693, 27032, 29472, 32768, 0},
   {5273, 5379, 20177, 20270, 20385, 20439, 20949, 21695, 21774, 23138, 24256, 24703, 26679, 32768, 0},
   {6740, 7167, 7662, 14152, 14536, 14785, 15034, 16741, 18371, 21520, 22206, 23389, 24182, 32768, 0},
   {4987, 5368, 5928, 6068, 19114, 20315, 21857, 22253, 22411, 24911, 25380, 26027, 26376, 32768, 0},
   {5370, 6889, 7247, 7393, 9498, 21114, 21402, 21753, 21981, 24780, 25386, 26517, 27176, 32768, 0},
   {4816, 4961, 7204, 7326, 8765, 8930, 20169, 20682, 20803, 23188, 23763, 24455, 24940, 32768, 0},
   {6608, 6740, 8529, 9049, 9257, 9356, 9735, 18827, 19059, 22336, 23204, 23964, 24793, 32768, 0},
   {5998, 7419, 7781, 8933, 9255, 9549, 9753, 10417, 18898, 22494, 23139, 24764, 25989, 32768, 0},
   {10660, 11298, 12550, 12957, 13322, 13624, 14040, 15004, 15534, 20714, 21789, 23443, 24861, 32768, 0},
   {10522, 11530, 12552, 12963, 13378, 13779, 14245, 15235, 15902, 20102, 22696, 23774, 25838, 32768, 0},
   {10099, 10691, 12639, 13049, 13386, 13665, 14125, 15163, 15636, 19676, 20474, 23519, 25208, 32768, 0},
   {3144, 5087, 7382, 7504, 7593, 7690, 7801, 8064, 8232, 9248, 9875, 10521, 29048, 32768, 0},
};

static const uint16_t kDefaultAngleDeltaCdf[AV1_DIRECTIONAL_MODES][AV1_ANGLE_DELTA_SYMBOLS + 1u] = {
   {2180, 5032, 7567, 22776, 26989, 30217, 32768, 0},
   {2301, 5608, 8801, 23487, 26974, 30330, 32768, 0},
   {3780, 11018, 13699, 19354, 23083, 31286, 32768, 0},
   {4581, 11226, 15147, 17138, 21834, 28397, 32768, 0},
   {1737, 10927, 14509, 19588, 22745, 28823, 32768, 0},
   {2664, 10176, 12485, 17650, 21600, 30495, 32768, 0},
   {2240, 11096, 15453, 20341, 22561, 28917, 32768, 0},
   {3605, 10428, 12459, 17676, 21244, 30655, 32768, 0},
};

// Default CFL CDF tables from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultCflSignCdf[AV1_CFL_JOINT_SIGNS + 1u] = {
   1418,
   2123,
   13340,
   18405,
   26972,
   28343,
   32294,
   32768,
   0,
};

static const uint16_t kDefaultCflAlphaCdf[AV1_CFL_ALPHA_CONTEXTS][AV1_CFL_ALPHABET_SIZE + 1u] = {
   {7637, 20719, 31401, 32481, 32657, 32688, 32692, 32696, 32700, 32704, 32708, 32712, 32716, 32720, 32724, 32768, 0},
   {14365, 23603, 28135, 31168, 32167, 32395, 32487, 32573, 32620, 32647, 32668, 32672, 32676, 32680, 32684, 32768, 0},
   {11532, 22380, 28445, 31360, 32349, 32523, 32584, 32649, 32673, 32677, 32681, 32685, 32689, 32693, 32697, 32768, 0},
   {26990, 31402, 32282, 32571, 32692, 32696, 32700, 32704, 32708, 32712, 32716, 32720, 32724, 32728, 32732, 32768, 0},
   {17248, 26058, 28904, 30608, 31305, 31877, 32126, 32321, 32394, 32464, 32516, 32560, 32576, 32593, 32622, 32768, 0},
   {14738, 21678, 25779, 27901, 29024, 30302, 30980, 31843, 32144, 32413, 32520, 32594, 32622, 32656, 32660, 32768, 0},
};

// Default filter-intra CDF tables from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultFilterIntraModeCdf[AV1_INTRA_FILTER_MODES + 1u] = {8949, 12776, 17211, 29558, 32768, 0};

static const uint16_t kDefaultFilterIntraCdf[AV1_BLOCK_SIZES][3] = {
   {4621, 32768, 0},
   {6743, 32768, 0},
   {5893, 32768, 0},
   {7866, 32768, 0},
   {12551, 32768, 0},
   {9394, 32768, 0},
   {12408, 32768, 0},
   {14301, 32768, 0},
   {12756, 32768, 0},
   {22343, 32768, 0},
   {16384, 32768, 0},
   {16384, 32768, 0},
   {16384, 32768, 0},
   {16384, 32768, 0},
   {16384, 32768, 0},
   {16384, 32768, 0},
   {12770, 32768, 0},
   {10368, 32768, 0},
   {20229, 32768, 0},
   {18101, 32768, 0},
   {16384, 32768, 0},
   {16384, 32768, 0},
};

// Default palette CDF tables from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultPaletteYModeCdf[AV1_PALETTE_BLOCK_SIZE_CONTEXTS][AV1_PALETTE_Y_MODE_CONTEXTS][3] = {
   {{31676, 32768, 0}, {3419, 32768, 0}, {1261, 32768, 0}},
   {{31912, 32768, 0}, {2859, 32768, 0}, {980, 32768, 0}},
   {{31823, 32768, 0}, {3400, 32768, 0}, {781, 32768, 0}},
   {{32030, 32768, 0}, {3561, 32768, 0}, {904, 32768, 0}},
   {{32309, 32768, 0}, {7337, 32768, 0}, {1462, 32768, 0}},
   {{32265, 32768, 0}, {4015, 32768, 0}, {1521, 32768, 0}},
   {{32450, 32768, 0}, {7946, 32768, 0}, {129, 32768, 0}},
};

static const uint16_t kDefaultPaletteUVModeCdf[AV1_PALETTE_UV_MODE_CONTEXTS][3] = {
   {32461, 32768, 0},
   {21488, 32768, 0},
};

static const uint16_t kDefaultPaletteYSizeCdf[AV1_PALETTE_BLOCK_SIZE_CONTEXTS][AV1_PALETTE_SIZES + 1u] = {
   {7952, 13000, 18149, 21478, 25527, 29241, 32768, 0},
   {7139, 11421, 16195, 19544, 23666, 28073, 32768, 0},
   {7788, 12741, 17325, 20500, 24315, 28530, 32768, 0},
   {8271, 14064, 18246, 21564, 25071, 28533, 32768, 0},
   {12725, 19180, 21863, 24839, 27535, 30120, 32768, 0},
   {9711, 14888, 16923, 21052, 25661, 27875, 32768, 0},
   {14940, 20797, 21678, 24186, 27033, 28999, 32768, 0},
};

static const uint16_t kDefaultPaletteUVSizeCdf[AV1_PALETTE_BLOCK_SIZE_CONTEXTS][AV1_PALETTE_SIZES + 1u] = {
   {8713, 19979, 27128, 29609, 31331, 32272, 32768, 0},
   {5839, 15573, 23581, 26947, 29848, 31700, 32768, 0},
   {4426, 11260, 17999, 21483, 25863, 29430, 32768, 0},
   {3228, 9464, 14993, 18089, 22523, 27420, 32768, 0},
   {3768, 8886, 13091, 17852, 22495, 27207, 32768, 0},
   {2464, 8451, 12861, 21632, 25525, 28555, 32768, 0},
   {1269, 5435, 10433, 18963, 21700, 25865, 32768, 0},
};

// Default segment_id CDF tables from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultSegmentIdCdf[AV1_SEGMENT_ID_CONTEXTS][AV1_MAX_SEGMENTS + 1u] = {
   {5622, 7893, 16093, 18233, 27809, 28373, 32533, 32768, 0},
   {14274, 18230, 22557, 24935, 29980, 30851, 32344, 32768, 0},
   {27527, 28487, 28723, 28890, 32397, 32647, 32679, 32768, 0},
};

// Default tx_depth CDF tables from the AV1 spec (see local av1bitstream.html).
static const uint16_t kDefaultTx8x8Cdf[AV1_TX_SIZE_CONTEXTS][3] = {
   {19968, 32768, 0},
   {19968, 32768, 0},
   {24320, 32768, 0},
};

static const uint16_t kDefaultTx16x16Cdf[AV1_TX_SIZE_CONTEXTS][4] = {
   {12272, 30172, 32768, 0},
   {12272, 30172, 32768, 0},
   {18677, 30848, 32768, 0},
};

static const uint16_t kDefaultTx32x32Cdf[AV1_TX_SIZE_CONTEXTS][4] = {
   {12986, 15180, 32768, 0},
   {12986, 15180, 32768, 0},
   {24302, 25602, 32768, 0},
};

static const uint16_t kDefaultTx64x64Cdf[AV1_TX_SIZE_CONTEXTS][4] = {
   {5782, 11475, 32768, 0},
   {5782, 11475, 32768, 0},
   {16803, 22759, 32768, 0},
};

static bool max_tx_size_rect_from_mi_size(uint32_t mi_size, uint32_t *out_tx_size) {
   // From AV1 spec Max_Tx_Size_Rect[BLOCK_SIZES] table.
   static const uint8_t kMaxTxSizeRect[AV1_BLOCK_SIZES] = {
       AV1_TX_4X4,  AV1_TX_4X8,   AV1_TX_8X4,   AV1_TX_8X8,   AV1_TX_8X16,  AV1_TX_16X8,
       AV1_TX_16X16, AV1_TX_16X32, AV1_TX_32X16, AV1_TX_32X32, AV1_TX_32X64, AV1_TX_64X32,
       AV1_TX_64X64, AV1_TX_64X64, AV1_TX_64X64, AV1_TX_64X64, AV1_TX_4X16,  AV1_TX_16X4,
       AV1_TX_8X32,  AV1_TX_32X8,  AV1_TX_16X64, AV1_TX_64X16,
   };
   if (!out_tx_size || mi_size >= AV1_BLOCK_SIZES) {
      return false;
   }
   *out_tx_size = (uint32_t)kMaxTxSizeRect[mi_size];
   return true;
}

static bool get_plane_residual_mi_size(uint32_t luma_wlog2,
                                      uint32_t luma_hlog2,
                                      uint32_t plane,
                                      uint32_t subsampling_x,
                                      uint32_t subsampling_y,
                                      uint32_t *out_mi_size) {
   if (!out_mi_size) {
      return false;
   }
   if (plane == 0u) {
      return mi_size_index_from_wlog2_hlog2(luma_wlog2, luma_hlog2, out_mi_size);
   }
   const uint32_t wlog2 = (luma_wlog2 > subsampling_x) ? (luma_wlog2 - subsampling_x) : 0u;
   const uint32_t hlog2 = (luma_hlog2 > subsampling_y) ? (luma_hlog2 - subsampling_y) : 0u;
   return mi_size_index_from_wlog2_hlog2(wlog2, hlog2, out_mi_size);
}

static bool get_tx_size_for_plane(uint32_t plane,
                                 uint32_t tx_size,
                                 uint32_t luma_wlog2,
                                 uint32_t luma_hlog2,
                                 uint32_t subsampling_x,
                                 uint32_t subsampling_y,
                                 uint32_t *out_tx_size) {
   // Spec get_tx_size(plane, txSz):
   // - plane==0 => txSz
   // - plane>0  => uvTx = Max_Tx_Size_Rect[get_plane_residual_size(MiSize,plane)] with 64x* capped.
   if (!out_tx_size || tx_size >= AV1_TX_SIZES_ALL) {
      return false;
   }
   if (plane == 0u) {
      *out_tx_size = tx_size;
      return true;
   }

   uint32_t residual_mi_size = 0u;
   if (!get_plane_residual_mi_size(luma_wlog2, luma_hlog2, plane, subsampling_x, subsampling_y, &residual_mi_size)) {
      return false;
   }

   uint32_t uvTx = AV1_TX_4X4;
   if (!max_tx_size_rect_from_mi_size(residual_mi_size, &uvTx) || uvTx >= AV1_TX_SIZES_ALL) {
      return false;
   }

   const uint32_t w_px = 1u << kTxWidthLog2[uvTx];
   const uint32_t h_px = 1u << kTxHeightLog2[uvTx];
   if (w_px == 64u || h_px == 64u) {
      if (w_px == 16u) {
         *out_tx_size = AV1_TX_16X32;
         return true;
      }
      if (h_px == 16u) {
         *out_tx_size = AV1_TX_32X16;
         return true;
      }
      *out_tx_size = AV1_TX_32X32;
      return true;
   }

   *out_tx_size = uvTx;
   return true;
}

static bool is_tx_type_in_set_intra(uint32_t tx_set, uint32_t tx_type) {
   // Minimal spec behavior for intra transform-set membership.
   // - DCTONLY: only DCT_DCT
   // - INTRA_1: all of our supported intra tx types
   // - INTRA_2: does not include V_DCT / H_DCT
   if (tx_set == AV1_TX_SET_DCTONLY) {
      return tx_type == AV1_TX_TYPE_DCT_DCT;
   }
   if (tx_set == AV1_TX_SET_INTRA_2) {
      return (tx_type != AV1_TX_TYPE_V_DCT) && (tx_type != AV1_TX_TYPE_H_DCT);
   }
   return true;
}

static void tile_coeff_cdfs_init(Av1TileCoeffCdfs *out, uint32_t base_q_idx) {
   if (!out) return;
   const uint32_t qctx = coeff_cdf_q_ctx_from_base_q_idx(base_q_idx);
   for (uint32_t tx = 0; tx < AV1_COEFF_TX_SIZES; tx++) {
      for (uint32_t ctx = 0; ctx < AV1_TXB_SKIP_CONTEXTS; ctx++) {
         cdf_copy_u16(out->txb_skip[tx][ctx], kDefaultTxbSkipCdf[qctx][tx][ctx], 3u);
      }
   }

   for (uint32_t ptype = 0; ptype < AV1_PLANE_TYPES; ptype++) {
      const uint16_t (*eob16)[2][6] = (ptype == 0u) ? kDefaultEobPt16CdfLuma : kDefaultEobPt16CdfChroma;
      const uint16_t (*eob32)[2][7] = (ptype == 0u) ? kDefaultEobPt32CdfLuma : kDefaultEobPt32CdfChroma;
      const uint16_t (*eob64)[2][8] = (ptype == 0u) ? kDefaultEobPt64CdfLuma : kDefaultEobPt64CdfChroma;
      const uint16_t (*eob128)[2][9] = (ptype == 0u) ? kDefaultEobPt128CdfLuma : kDefaultEobPt128CdfChroma;
      const uint16_t (*eob256)[2][10] = (ptype == 0u) ? kDefaultEobPt256CdfLuma : kDefaultEobPt256CdfChroma;
      const uint16_t (*eob512)[11] = (ptype == 0u) ? kDefaultEobPt512CdfLuma : kDefaultEobPt512CdfChroma;
      const uint16_t (*eob1024)[12] = (ptype == 0u) ? kDefaultEobPt1024CdfLuma : kDefaultEobPt1024CdfChroma;

      const uint16_t (*eob_extra)[5][9][3] = (ptype == 0u) ? kDefaultEobExtraCdfLuma : kDefaultEobExtraCdfChroma;
      const uint16_t (*coeff_base_eob)[5][4][4] = (ptype == 0u) ? kDefaultCoeffBaseEobCdfLuma : kDefaultCoeffBaseEobCdfChroma;

      const uint16_t (*coeff_base)[5][42][5] = (ptype == 0u) ? kDefaultCoeffBaseCdfLuma : kDefaultCoeffBaseCdfChroma;
      const uint16_t (*coeff_br)[5][21][5] = (ptype == 0u) ? kDefaultCoeffBrCdfLuma : kDefaultCoeffBrCdfChroma;
      const uint16_t (*dc_sign)[3][3] = (ptype == 0u) ? kDefaultDcSignCdfLuma : kDefaultDcSignCdfChroma;

      for (uint32_t ctx = 0; ctx < 2u; ctx++) {
         cdf_copy_u16(out->eob_pt_16[ptype][ctx], eob16[qctx][ctx], 6u);
         cdf_copy_u16(out->eob_pt_32[ptype][ctx], eob32[qctx][ctx], 7u);
         cdf_copy_u16(out->eob_pt_64[ptype][ctx], eob64[qctx][ctx], 8u);
         cdf_copy_u16(out->eob_pt_128[ptype][ctx], eob128[qctx][ctx], 9u);
         cdf_copy_u16(out->eob_pt_256[ptype][ctx], eob256[qctx][ctx], 10u);
      }
      cdf_copy_u16(out->eob_pt_512[ptype], eob512[qctx], 11u);
      cdf_copy_u16(out->eob_pt_1024[ptype], eob1024[qctx], 12u);

      for (uint32_t tx = 0; tx < AV1_COEFF_TX_SIZES; tx++) {
         for (uint32_t ctx = 0; ctx < AV1_EOB_COEF_CONTEXTS; ctx++) {
            cdf_copy_u16(out->eob_extra[tx][ptype][ctx], eob_extra[qctx][tx][ctx], 3u);
         }
      }

      for (uint32_t tx = 0; tx < AV1_COEFF_TX_SIZES; tx++) {
         for (uint32_t ctx = 0; ctx < AV1_SIG_COEF_CONTEXTS_EOB; ctx++) {
            cdf_copy_u16(out->coeff_base_eob[tx][ptype][ctx], coeff_base_eob[qctx][tx][ctx], 4u);
         }
      }

      for (uint32_t tx = 0; tx < AV1_COEFF_TX_SIZES; tx++) {
         for (uint32_t ctx = 0; ctx < AV1_SIG_COEF_CONTEXTS; ctx++) {
            cdf_copy_u16(out->coeff_base[tx][ptype][ctx], coeff_base[qctx][tx][ctx], 5u);
         }
      }

      for (uint32_t tx = 0; tx < AV1_COEFF_BR_TX_SIZES; tx++) {
         for (uint32_t ctx = 0; ctx < AV1_LEVEL_CONTEXTS; ctx++) {
            cdf_copy_u16(out->coeff_br[tx][ptype][ctx], coeff_br[qctx][tx][ctx], AV1_BR_CDF_SIZE + 1u);
         }
      }

      for (uint32_t ctx = 0; ctx < AV1_DC_SIGN_CONTEXTS; ctx++) {
         cdf_copy_u16(out->dc_sign[ptype][ctx], dc_sign[qctx][ctx], 3u);
      }
   }
}

static bool split_tx_size(uint32_t tx_size, uint32_t *out_tx_size) {
   // From AV1 spec Split_Tx_Size[TX_SIZES_ALL] table.
   static const uint8_t kSplitTxSize[AV1_TX_SIZES_ALL] = {
       AV1_TX_4X4,   // TX_4X4
       AV1_TX_4X4,   // TX_8X8
       AV1_TX_8X8,   // TX_16X16
       AV1_TX_16X16, // TX_32X32
       AV1_TX_32X32, // TX_64X64
       AV1_TX_4X4,   // TX_4X8
       AV1_TX_4X4,   // TX_8X4
       AV1_TX_8X8,   // TX_8X16
       AV1_TX_8X8,   // TX_16X8
       AV1_TX_16X16, // TX_16X32
       AV1_TX_16X16, // TX_32X16
       AV1_TX_32X32, // TX_32X64
       AV1_TX_32X32, // TX_64X32
       AV1_TX_4X8,   // TX_4X16
       AV1_TX_8X4,   // TX_16X4
       AV1_TX_8X16,  // TX_8X32
       AV1_TX_16X8,  // TX_32X8
       AV1_TX_16X32, // TX_16X64
       AV1_TX_32X16, // TX_64X16
   };
   if (!out_tx_size || tx_size >= AV1_TX_SIZES_ALL) {
      return false;
   }
   *out_tx_size = (uint32_t)kSplitTxSize[tx_size];
   return true;
}

static void tile_partition_cdfs_init(Av1TilePartitionCdfs *t) {
   cdf_copy_u16(&t->w8[0][0], &kDefaultPartitionW8Cdf[0][0], AV1_PARTITION_CONTEXTS * 5u);
   cdf_copy_u16(&t->w16[0][0], &kDefaultPartitionW16Cdf[0][0], AV1_PARTITION_CONTEXTS * 11u);
   cdf_copy_u16(&t->w32[0][0], &kDefaultPartitionW32Cdf[0][0], AV1_PARTITION_CONTEXTS * 11u);
   cdf_copy_u16(&t->w64[0][0], &kDefaultPartitionW64Cdf[0][0], AV1_PARTITION_CONTEXTS * 11u);
   cdf_copy_u16(&t->w128[0][0], &kDefaultPartitionW128Cdf[0][0], AV1_PARTITION_CONTEXTS * 9u);
}

static void tile_skip_cdfs_init(Av1TileSkipCdfs *t) {
   cdf_copy_u16(&t->skip[0][0], &kDefaultSkipCdf[0][0], AV1_SKIP_CONTEXTS * 3u);
   cdf_copy_u16(&t->y_mode[0][0], &kDefaultYModeCdf[0][0], AV1_Y_MODE_CONTEXTS * (AV1_INTRA_MODES + 1u));
   cdf_copy_u16(&t->uv_mode_cfl_not_allowed[0][0],
                &kDefaultUvModeCflNotAllowedCdf[0][0],
                AV1_INTRA_MODES * (AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED + 1u));
   cdf_copy_u16(&t->uv_mode_cfl_allowed[0][0],
                &kDefaultUvModeCflAllowedCdf[0][0],
                AV1_INTRA_MODES * (AV1_UV_INTRA_MODES_CFL_ALLOWED + 1u));
   cdf_copy_u16(&t->angle_delta[0][0],
                &kDefaultAngleDeltaCdf[0][0],
                AV1_DIRECTIONAL_MODES * (AV1_ANGLE_DELTA_SYMBOLS + 1u));

   cdf_copy_u16(&t->cfl_sign[0], &kDefaultCflSignCdf[0], AV1_CFL_JOINT_SIGNS + 1u);
   cdf_copy_u16(&t->cfl_alpha[0][0], &kDefaultCflAlphaCdf[0][0], AV1_CFL_ALPHA_CONTEXTS * (AV1_CFL_ALPHABET_SIZE + 1u));

   cdf_copy_u16(&t->filter_intra_mode[0], &kDefaultFilterIntraModeCdf[0], AV1_INTRA_FILTER_MODES + 1u);
   cdf_copy_u16(&t->filter_intra[0][0], &kDefaultFilterIntraCdf[0][0], AV1_BLOCK_SIZES * 3u);

   cdf_copy_u16(&t->palette_y_mode[0][0][0], &kDefaultPaletteYModeCdf[0][0][0], AV1_PALETTE_BLOCK_SIZE_CONTEXTS * AV1_PALETTE_Y_MODE_CONTEXTS * 3u);
   cdf_copy_u16(&t->palette_uv_mode[0][0], &kDefaultPaletteUVModeCdf[0][0], AV1_PALETTE_UV_MODE_CONTEXTS * 3u);
   cdf_copy_u16(&t->palette_y_size[0][0], &kDefaultPaletteYSizeCdf[0][0], AV1_PALETTE_BLOCK_SIZE_CONTEXTS * (AV1_PALETTE_SIZES + 1u));
   cdf_copy_u16(&t->palette_uv_size[0][0], &kDefaultPaletteUVSizeCdf[0][0], AV1_PALETTE_BLOCK_SIZE_CONTEXTS * (AV1_PALETTE_SIZES + 1u));

   cdf_copy_u16(&t->segment_id[0][0], &kDefaultSegmentIdCdf[0][0], AV1_SEGMENT_ID_CONTEXTS * (AV1_MAX_SEGMENTS + 1u));

   cdf_copy_u16(&t->tx8x8[0][0], &kDefaultTx8x8Cdf[0][0], AV1_TX_SIZE_CONTEXTS * 3u);
   cdf_copy_u16(&t->tx16x16[0][0], &kDefaultTx16x16Cdf[0][0], AV1_TX_SIZE_CONTEXTS * 4u);
   cdf_copy_u16(&t->tx32x32[0][0], &kDefaultTx32x32Cdf[0][0], AV1_TX_SIZE_CONTEXTS * 4u);
   cdf_copy_u16(&t->tx64x64[0][0], &kDefaultTx64x64Cdf[0][0], AV1_TX_SIZE_CONTEXTS * 4u);

   cdf_copy_u16(&t->intra_tx_type_set1[0][0][0], &kDefaultIntraTxTypeSet1Cdf[0][0][0], 2u * AV1_INTRA_MODES * 8u);
   cdf_copy_u16(&t->intra_tx_type_set2[0][0][0], &kDefaultIntraTxTypeSet2Cdf[0][0][0], 3u * AV1_INTRA_MODES * 6u);

   cdf_copy_u16(&t->delta_q_abs[0], &kDefaultDeltaQCdf[0], AV1_DELTA_Q_ABS_SYMBOLS + 1u);
   cdf_copy_u16(&t->delta_lf_abs[0], &kDefaultDeltaLFCdf[0], AV1_DELTA_LF_ABS_SYMBOLS + 1u);
   cdf_copy_u16(&t->delta_lf_multi[0][0], &kDefaultDeltaLFCdf[0], AV1_FRAME_LF_COUNT * (AV1_DELTA_LF_ABS_SYMBOLS + 1u));

   t->current_qindex = 0;
   memset(&t->delta_lf_state[0], 0, sizeof(t->delta_lf_state));
}

static uint32_t max_tx_depth_from_mi_size(uint32_t mi_size) {
   // From AV1 spec Max_Tx_Depth[BLOCK_SIZES] table.
   static const uint8_t kMaxTxDepth[AV1_BLOCK_SIZES] = {
       0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4, 2, 2, 3, 3, 4, 4,
   };
   if (mi_size >= AV1_BLOCK_SIZES) {
      return 0;
   }
   return (uint32_t)kMaxTxDepth[mi_size];
}

static bool mi_size_index_from_wlog2_hlog2(uint32_t wlog2, uint32_t hlog2, uint32_t *out_mi_size) {
   if (!out_mi_size) {
      return false;
   }
   // Map from (w,h) in MI units to AV1 MiSize enum index (matches spec table for subSize).
   // MI unit is 4x4 luma samples.
   const uint32_t w_mi = 1u << wlog2;
   const uint32_t h_mi = 1u << hlog2;
   const uint32_t w_px = w_mi * 4u;
   const uint32_t h_px = h_mi * 4u;

   // Common sizes from the spec's subSize table (0..21).
   if (w_px == 4u && h_px == 4u) {
      *out_mi_size = 0u;
      return true;
   }
   if (w_px == 4u && h_px == 8u) {
      *out_mi_size = 1u;
      return true;
   }
   if (w_px == 8u && h_px == 4u) {
      *out_mi_size = 2u;
      return true;
   }
   if (w_px == 8u && h_px == 8u) {
      *out_mi_size = 3u;
      return true;
   }
   if (w_px == 8u && h_px == 16u) {
      *out_mi_size = 4u;
      return true;
   }
   if (w_px == 16u && h_px == 8u) {
      *out_mi_size = 5u;
      return true;
   }
   if (w_px == 16u && h_px == 16u) {
      *out_mi_size = 6u;
      return true;
   }
   if (w_px == 16u && h_px == 32u) {
      *out_mi_size = 7u;
      return true;
   }
   if (w_px == 32u && h_px == 16u) {
      *out_mi_size = 8u;
      return true;
   }
   if (w_px == 32u && h_px == 32u) {
      *out_mi_size = 9u;
      return true;
   }
   if (w_px == 32u && h_px == 64u) {
      *out_mi_size = 10u;
      return true;
   }
   if (w_px == 64u && h_px == 32u) {
      *out_mi_size = 11u;
      return true;
   }
   if (w_px == 64u && h_px == 64u) {
      *out_mi_size = 12u;
      return true;
   }
   if (w_px == 64u && h_px == 128u) {
      *out_mi_size = 13u;
      return true;
   }
   if (w_px == 128u && h_px == 64u) {
      *out_mi_size = 14u;
      return true;
   }
   if (w_px == 128u && h_px == 128u) {
      *out_mi_size = 15u;
      return true;
   }
   if (w_px == 4u && h_px == 16u) {
      *out_mi_size = 16u;
      return true;
   }
   if (w_px == 16u && h_px == 4u) {
      *out_mi_size = 17u;
      return true;
   }
   if (w_px == 8u && h_px == 32u) {
      *out_mi_size = 18u;
      return true;
   }
   if (w_px == 32u && h_px == 8u) {
      *out_mi_size = 19u;
      return true;
   }
   if (w_px == 16u && h_px == 64u) {
      *out_mi_size = 20u;
      return true;
   }
   if (w_px == 64u && h_px == 16u) {
      *out_mi_size = 21u;
      return true;
   }

   return false;
}

static bool decode_cfl_alphas(Av1SymbolDecoder *sd,
                              Av1TileSkipCdfs *mode_cdfs,
                              uint32_t *out_cfl_alpha_signs,
                              int32_t *out_alpha_u,
                              int32_t *out_alpha_v,
                              char *err,
                              size_t err_cap) {
   if (!sd || !mode_cdfs || !out_cfl_alpha_signs || !out_alpha_u || !out_alpha_v) {
      snprintf(err, err_cap, "invalid args");
      return false;
   }

   uint32_t cfl_alpha_signs = 0;
   if (!av1_symbol_read_symbol(sd, mode_cdfs->cfl_sign, AV1_CFL_JOINT_SIGNS, &cfl_alpha_signs, err, err_cap)) {
      return false;
   }

   // Spec: signU = (cfl_alpha_signs + 1) / 3; signV = (cfl_alpha_signs + 1) % 3
   const uint32_t signU = (cfl_alpha_signs + 1u) / 3u;
   const uint32_t signV = (cfl_alpha_signs + 1u) % 3u;

   int32_t alpha_u = 0;
   int32_t alpha_v = 0;

   // CFL_SIGN_ZERO=0, CFL_SIGN_POS=1, CFL_SIGN_NEG=2.
   if (signU != 0u) {
      // Spec table (also note: ctx == cfl_alpha_signs - 2 when signU != 0).
      if (cfl_alpha_signs < 2u) {
         snprintf(err, err_cap, "invalid cfl_alpha_signs=%u for U", cfl_alpha_signs);
         return false;
      }
      const uint32_t ctx_u = cfl_alpha_signs - 2u;
      if (ctx_u >= AV1_CFL_ALPHA_CONTEXTS) {
         snprintf(err, err_cap, "invalid cfl_alpha_u ctx=%u", ctx_u);
         return false;
      }
      uint32_t cfl_alpha_u = 0;
      if (!av1_symbol_read_symbol(sd, mode_cdfs->cfl_alpha[ctx_u], AV1_CFL_ALPHABET_SIZE, &cfl_alpha_u, err, err_cap)) {
         return false;
      }
      alpha_u = (int32_t)(1u + cfl_alpha_u);
      if (signU == 2u) {
         alpha_u = -alpha_u;
      }
   }

   if (signV != 0u) {
      // Spec table for V ctx.
      uint32_t ctx_v = 0;
      switch (cfl_alpha_signs) {
         case 0u:
            ctx_v = 0u;
            break;
         case 1u:
            ctx_v = 3u;
            break;
         case 3u:
            ctx_v = 1u;
            break;
         case 4u:
            ctx_v = 4u;
            break;
         case 6u:
            ctx_v = 2u;
            break;
         case 7u:
            ctx_v = 5u;
            break;
         default:
            snprintf(err, err_cap, "invalid cfl_alpha_signs=%u for V", cfl_alpha_signs);
            return false;
      }
      if (ctx_v >= AV1_CFL_ALPHA_CONTEXTS) {
         snprintf(err, err_cap, "invalid cfl_alpha_v ctx=%u", ctx_v);
         return false;
      }
      uint32_t cfl_alpha_v = 0;
      if (!av1_symbol_read_symbol(sd, mode_cdfs->cfl_alpha[ctx_v], AV1_CFL_ALPHABET_SIZE, &cfl_alpha_v, err, err_cap)) {
         return false;
      }
      alpha_v = (int32_t)(1u + cfl_alpha_v);
      if (signV == 2u) {
         alpha_v = -alpha_v;
      }
   }

   *out_cfl_alpha_signs = cfl_alpha_signs;
   *out_alpha_u = alpha_u;
   *out_alpha_v = alpha_v;
   return true;
}

static uint32_t bsl_to_num4x4(uint32_t bsl) {
   return 1u << bsl;
}

static uint32_t mi_index(uint32_t row, uint32_t col, uint32_t stride) {
   return row * stride + col;
}

static uint32_t partition_ctx_from_mi_grid(const Av1MiSize *mi_grid,
                                           uint32_t mi_rows,
                                           uint32_t mi_cols,
                                           uint32_t r,
                                           uint32_t c,
                                           uint32_t bsl) {
   // Spec:
   // above = AvailU && ( Mi_Width_Log2[ MiSizes[r-1][c] ] < bsl )
   // left  = AvailL && ( Mi_Height_Log2[ MiSizes[r][c-1] ] < bsl )
   const bool avail_u = (r > 0);
   const bool avail_l = (c > 0);

   bool above = false;
   bool left = false;

   if (avail_u && r - 1 < mi_rows && c < mi_cols) {
      const Av1MiSize ms = mi_grid[mi_index(r - 1, c, mi_cols)];
      above = (ms.wlog2 < bsl);
   }
   if (avail_l && r < mi_rows && c - 1 < mi_cols) {
      const Av1MiSize ms = mi_grid[mi_index(r, c - 1, mi_cols)];
      left = (ms.hlog2 < bsl);
   }

   return (uint32_t)(left ? 2u : 0u) + (uint32_t)(above ? 1u : 0u);
}

static bool select_partition_cdf(Av1TilePartitionCdfs *t,
                                 uint32_t bsl,
                                 uint32_t ctx,
                                 uint16_t **out_cdf,
                                 size_t *out_n) {
   if (!t || !out_cdf || !out_n) {
      return false;
   }
   if (ctx >= AV1_PARTITION_CONTEXTS) {
      return false;
   }

   // n is the number of symbols (cdf length is n+1, but av1_symbol_read_symbol expects n).
   switch (bsl) {
      case 1u:
         *out_cdf = t->w8[ctx];
         *out_n = 4;
         return true;
      case 2u:
         *out_cdf = t->w16[ctx];
         *out_n = 10;
         return true;
      case 3u:
         *out_cdf = t->w32[ctx];
         *out_n = 10;
         return true;
      case 4u:
         *out_cdf = t->w64[ctx];
         *out_n = 10;
         return true;
      case 5u:
         *out_cdf = t->w128[ctx];
         // For 128x128, *_4 partitions are not present.
         *out_n = 8;
         return true;
      default:
         return false;
   }
}

static uint32_t cdf_mass(const uint16_t *cdf, uint32_t idx) {
   // Returns probability mass for symbol idx.
   if (idx == 0) {
      return (uint32_t)cdf[0];
   }
   return (uint32_t)cdf[idx] - (uint32_t)cdf[idx - 1];
}

static bool derive_split_or_horz_cdf(const uint16_t *partition_cdf,
                                     bool is_128x128,
                                     uint16_t out_cdf[3]) {
   if (!partition_cdf) {
      return false;
   }

   uint32_t psum = 0;
   psum += cdf_mass(partition_cdf, AV1_PARTITION_VERT);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_SPLIT);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_HORZ_A);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_VERT_A);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_VERT_B);
   if (!is_128x128) {
      psum += cdf_mass(partition_cdf, AV1_PARTITION_VERT_4);
   }

   if (psum > (1u << 15)) {
      psum = 1u << 15;
   }

   out_cdf[0] = (uint16_t)((1u << 15) - psum);
   out_cdf[1] = (uint16_t)(1u << 15);
   out_cdf[2] = 0;
   return true;
}

static bool derive_split_or_vert_cdf(const uint16_t *partition_cdf,
                                     bool is_128x128,
                                     uint16_t out_cdf[3]) {
   if (!partition_cdf) {
      return false;
   }

   uint32_t psum = 0;
   psum += cdf_mass(partition_cdf, AV1_PARTITION_HORZ);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_SPLIT);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_HORZ_A);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_HORZ_B);
   psum += cdf_mass(partition_cdf, AV1_PARTITION_VERT_A);
   if (!is_128x128) {
      psum += cdf_mass(partition_cdf, AV1_PARTITION_HORZ_4);
   }

   if (psum > (1u << 15)) {
      psum = 1u << 15;
   }

   out_cdf[0] = (uint16_t)((1u << 15) - psum);
   out_cdf[1] = (uint16_t)(1u << 15);
   out_cdf[2] = 0;
   return true;
}

static void mi_fill_block(Av1MiSize *mi_grid,
                          uint32_t mi_rows,
                          uint32_t mi_cols,
                          uint32_t r,
                          uint32_t c,
                          uint32_t wlog2,
                          uint32_t hlog2,
                          Av1TileSyntaxProbeStats *st) {
   const uint32_t w = 1u << wlog2;
   const uint32_t h = 1u << hlog2;

   for (uint32_t rr = 0; rr < h; rr++) {
      for (uint32_t cc = 0; cc < w; cc++) {
         const uint32_t y = r + rr;
         const uint32_t x = c + cc;
         if (y < mi_rows && x < mi_cols) {
            mi_grid[mi_index(y, x, mi_cols)].wlog2 = (uint8_t)wlog2;
            mi_grid[mi_index(y, x, mi_cols)].hlog2 = (uint8_t)hlog2;
         }
      }
   }
   if (st) {
      st->leaf_blocks++;
   }
}

static void mi_set_skip_block(Av1MiSize *mi_grid,
                              uint32_t mi_rows,
                              uint32_t mi_cols,
                              uint32_t r,
                              uint32_t c,
                              uint32_t wlog2,
                              uint32_t hlog2,
                              uint32_t skip) {
   const uint32_t w = 1u << wlog2;
   const uint32_t h = 1u << hlog2;
   for (uint32_t rr = 0; rr < h; rr++) {
      for (uint32_t cc = 0; cc < w; cc++) {
         const uint32_t y = r + rr;
         const uint32_t x = c + cc;
         if (y < mi_rows && x < mi_cols) {
            mi_grid[mi_index(y, x, mi_cols)].skip = (uint8_t)(skip ? 1u : 0u);
         }
      }
   }
}

static void mi_set_y_mode_block(Av1MiSize *mi_grid,
                                uint32_t mi_rows,
                                uint32_t mi_cols,
                                uint32_t r,
                                uint32_t c,
                                uint32_t wlog2,
                                uint32_t hlog2,
                                uint32_t y_mode) {
   const uint32_t w = 1u << wlog2;
   const uint32_t h = 1u << hlog2;
   for (uint32_t rr = 0; rr < h; rr++) {
      for (uint32_t cc = 0; cc < w; cc++) {
         const uint32_t y = r + rr;
         const uint32_t x = c + cc;
         if (y < mi_rows && x < mi_cols) {
            mi_grid[mi_index(y, x, mi_cols)].y_mode = (uint8_t)y_mode;
         }
      }
   }
}

static void mi_set_palette_sizes_block(Av1MiSize *mi_grid,
                                       uint32_t mi_rows,
                                       uint32_t mi_cols,
                                       uint32_t r,
                                       uint32_t c,
                                       uint32_t wlog2,
                                       uint32_t hlog2,
                                       uint32_t palette_y_size,
                                       uint32_t palette_uv_size) {
   const uint32_t w = 1u << wlog2;
   const uint32_t h = 1u << hlog2;
   for (uint32_t rr = 0; rr < h; rr++) {
      for (uint32_t cc = 0; cc < w; cc++) {
         const uint32_t y = r + rr;
         const uint32_t x = c + cc;
         if (y < mi_rows && x < mi_cols) {
            mi_grid[mi_index(y, x, mi_cols)].palette_y_size = (uint8_t)palette_y_size;
            mi_grid[mi_index(y, x, mi_cols)].palette_uv_size = (uint8_t)palette_uv_size;
         }
      }
   }
}

static void mi_set_segment_id_block(Av1MiSize *mi_grid,
                                   uint32_t mi_rows,
                                   uint32_t mi_cols,
                                   uint32_t r,
                                   uint32_t c,
                                   uint32_t wlog2,
                                   uint32_t hlog2,
                                   uint32_t segment_id) {
   const uint32_t w = 1u << wlog2;
   const uint32_t h = 1u << hlog2;
   for (uint32_t rr = 0; rr < h; rr++) {
      for (uint32_t cc = 0; cc < w; cc++) {
         const uint32_t y = r + rr;
         const uint32_t x = c + cc;
         if (y < mi_rows && x < mi_cols) {
            mi_grid[mi_index(y, x, mi_cols)].segment_id = (uint8_t)segment_id;
         }
      }
   }
}

static uint32_t segment_id_ctx_from_mi_grid(const Av1MiSize *mi_grid, uint32_t mi_rows, uint32_t mi_cols, uint32_t r, uint32_t c) {
   if (!mi_grid || r >= mi_rows || c >= mi_cols) {
      return 0u;
   }

   const bool availU = (r > 0u);
   const bool availL = (c > 0u);
   const bool availUL = availU && availL;

   const int32_t prevUL = availUL ? (int32_t)mi_grid[mi_index(r - 1u, c - 1u, mi_cols)].segment_id : -1;
   const int32_t prevU = availU ? (int32_t)mi_grid[mi_index(r - 1u, c, mi_cols)].segment_id : -1;
   const int32_t prevL = availL ? (int32_t)mi_grid[mi_index(r, c - 1u, mi_cols)].segment_id : -1;

   uint32_t ctx = 0u;
   if (prevUL < 0) {
      ctx = 0u;
   } else if ((prevUL == prevU) && (prevUL == prevL)) {
      ctx = 2u;
   } else if ((prevUL == prevU) || (prevUL == prevL) || (prevU == prevL)) {
      ctx = 1u;
   } else {
      ctx = 0u;
   }

   if (ctx >= AV1_SEGMENT_ID_CONTEXTS) {
      ctx = AV1_SEGMENT_ID_CONTEXTS - 1u;
   }
   return ctx;
}

static uint32_t segment_id_pred_from_mi_grid(const Av1MiSize *mi_grid, uint32_t mi_rows, uint32_t mi_cols, uint32_t r, uint32_t c) {
   if (!mi_grid || r >= mi_rows || c >= mi_cols) {
      return 0u;
   }

   const bool availU = (r > 0u);
   const bool availL = (c > 0u);
   const bool availUL = availU && availL;

   const int32_t prevUL = availUL ? (int32_t)mi_grid[mi_index(r - 1u, c - 1u, mi_cols)].segment_id : -1;
   const int32_t prevU = availU ? (int32_t)mi_grid[mi_index(r - 1u, c, mi_cols)].segment_id : -1;
   const int32_t prevL = availL ? (int32_t)mi_grid[mi_index(r, c - 1u, mi_cols)].segment_id : -1;

   uint32_t pred = 0u;
   if (prevU == -1) {
      pred = (prevL == -1) ? 0u : (uint32_t)prevL;
   } else if (prevL == -1) {
      pred = (uint32_t)prevU;
   } else {
      pred = (prevUL == prevU) ? (uint32_t)prevU : (uint32_t)prevL;
   }
   if (pred >= AV1_MAX_SEGMENTS) {
      pred = 0u;
   }
   return pred;
}

static uint32_t neg_deinterleave(uint32_t diff, uint32_t ref, uint32_t max) {
   if (max == 0u) {
      return 0u;
   }
   if (ref == 0u) {
      return diff;
   }
   if (ref >= (max - 1u)) {
      return max - diff - 1u;
   }
   if (2u * ref < max) {
      if (diff <= 2u * ref) {
         if (diff & 1u) {
            return ref + ((diff + 1u) >> 1u);
         }
         return ref - (diff >> 1u);
      }
      return diff;
   }

   // 2*ref >= max
   const uint32_t span = max - ref - 1u;
   if (diff <= 2u * span) {
      if (diff & 1u) {
         return ref + ((diff + 1u) >> 1u);
      }
      return ref - (diff >> 1u);
   }
   return max - (diff + 1u);
}

static bool tile_read_intra_segment_id(Av1SymbolDecoder *sd,
                                      const Av1TileDecodeParams *params,
                                      Av1TileSkipCdfs *mode_cdfs,
                                      Av1MiSize *mi_grid,
                                      uint32_t mi_rows,
                                      uint32_t mi_cols,
                                      uint32_t r,
                                      uint32_t c,
                                      uint32_t wlog2,
                                      uint32_t hlog2,
                                      uint32_t skip,
                                      char *err,
                                      size_t err_cap) {
   if (!sd || !params || !mode_cdfs || !mi_grid) {
      snprintf(err, err_cap, "invalid args");
      return false;
   }

   uint32_t segment_id = 0u;
   if (params->segmentation_enabled) {
      const uint32_t pred = segment_id_pred_from_mi_grid(mi_grid, mi_rows, mi_cols, r, c);
      if (skip) {
         segment_id = pred;
      } else {
         const uint32_t ctx = segment_id_ctx_from_mi_grid(mi_grid, mi_rows, mi_cols, r, c);
         uint32_t max = params->last_active_seg_id + 1u;
         if (max == 0u) {
            max = 1u;
         }
         if (max > AV1_MAX_SEGMENTS) {
            max = AV1_MAX_SEGMENTS;
         }
         uint32_t diff = 0u;
         if (max > 1u) {
            if (!av1_symbol_read_symbol(sd, mode_cdfs->segment_id[ctx], max, &diff, err, err_cap)) {
               return false;
            }
         }
         segment_id = neg_deinterleave(diff, pred, max);
      }
   }

   if (segment_id >= AV1_MAX_SEGMENTS) {
      snprintf(err, err_cap, "invalid segment_id=%u", segment_id);
      return false;
   }

   mi_set_segment_id_block(mi_grid, mi_rows, mi_cols, r, c, wlog2, hlog2, segment_id);
   return true;
}

static uint32_t palette_y_ctx_from_mi_grid(const Av1MiSize *mi_grid,
                                          uint32_t mi_rows,
                                          uint32_t mi_cols,
                                          uint32_t r,
                                          uint32_t c) {
   // Spec: ctx = (AvailU && PaletteSizes[0][MiRow-1][MiCol] > 0) + (AvailL && PaletteSizes[0][MiRow][MiCol-1] > 0)
   uint32_t ctx = 0;
   if (r > 0 && c < mi_cols) {
      ctx += (mi_grid[mi_index(r - 1, c, mi_cols)].palette_y_size > 0) ? 1u : 0u;
   }
   if (c > 0 && r < mi_rows) {
      ctx += (mi_grid[mi_index(r, c - 1, mi_cols)].palette_y_size > 0) ? 1u : 0u;
   }
   if (ctx >= AV1_PALETTE_Y_MODE_CONTEXTS) {
      ctx = AV1_PALETTE_Y_MODE_CONTEXTS - 1u;
   }
   return ctx;
}

static uint32_t size_group_from_wlog2_hlog2(uint32_t wlog2, uint32_t hlog2) {
   // Equivalent to spec Size_Group[BLOCK_SIZES] for the block sizes we can produce.
   // wlog2/hlog2 are in MI units (4x4 luma blocks).
   if ((wlog2 == 0u && hlog2 == 0u) || (wlog2 == 0u && hlog2 == 1u) || (wlog2 == 1u && hlog2 == 0u) ||
       (wlog2 == 0u && hlog2 == 2u) || (wlog2 == 2u && hlog2 == 0u)) {
      return 0u;
   }
   if ((wlog2 == 1u && hlog2 == 1u) || (wlog2 == 1u && hlog2 == 2u) || (wlog2 == 2u && hlog2 == 1u) ||
       (wlog2 == 1u && hlog2 == 3u) || (wlog2 == 3u && hlog2 == 1u)) {
      return 1u;
   }
   if ((wlog2 == 2u && hlog2 == 2u) || (wlog2 == 2u && hlog2 == 3u) || (wlog2 == 3u && hlog2 == 2u) ||
       (wlog2 == 2u && hlog2 == 4u) || (wlog2 == 4u && hlog2 == 2u)) {
      return 2u;
   }
   return 3u;
}

static bool intra_directional_index(uint32_t intra_mode, uint32_t *out_dir_idx) {
   // Assumes AV1's canonical intra mode numbering.
   // Directional modes: V, H, D45, D135, D113, D157, D203, D67.
   switch (intra_mode) {
      case 1u:
         *out_dir_idx = 0u;
         return true;
      case 2u:
         *out_dir_idx = 1u;
         return true;
      case 3u:
         *out_dir_idx = 2u;
         return true;
      case 4u:
         *out_dir_idx = 3u;
         return true;
      case 5u:
         *out_dir_idx = 4u;
         return true;
      case 6u:
         *out_dir_idx = 5u;
         return true;
      case 7u:
         *out_dir_idx = 6u;
         return true;
      case 8u:
         *out_dir_idx = 7u;
         return true;
      default:
         return false;
   }
}

static uint32_t skip_ctx_from_mi_grid(const Av1MiSize *mi_grid,
                                      uint32_t mi_rows,
                                      uint32_t mi_cols,
                                      uint32_t r,
                                      uint32_t c) {
   // Approximation aligned with spec intent: ctx derived from above/left skip flags.
   // For the first block in a tile, ctx will be 0.
   uint32_t above = 0;
   uint32_t left = 0;
   if (r > 0 && c < mi_cols) {
      above = mi_grid[mi_index(r - 1, c, mi_cols)].skip ? 1u : 0u;
   }
   if (c > 0 && r < mi_rows) {
      left = mi_grid[mi_index(r, c - 1, mi_cols)].skip ? 1u : 0u;
   }
   uint32_t ctx = above + left;
   if (ctx >= AV1_SKIP_CONTEXTS) {
      ctx = AV1_SKIP_CONTEXTS - 1u;
   }
   return ctx;
}

static bool decode_coeffs_luma_one_tx_block(Av1SymbolDecoder *sd,
                                           Av1TileCoeffCdfs *coeff_cdfs,
                                           Av1TileCoeffCtx *coeff_ctx,
                                           uint32_t plane,
                                           uint32_t block_index,
                                           uint32_t block_r,
                                           uint32_t block_c,
                                           uint32_t tx_index,
                                           uint32_t x4,
                                           uint32_t y4,
                                           uint32_t bw_px,
                                           uint32_t bh_px,
                                           uint32_t tx_size,
                                           uint32_t tx_type,
                                           bool probe_try_exit_symbol,
                                           Av1TileSyntaxProbeStats *st,
                                           bool *out_stop_now,
                                           char *err,
                                           size_t err_cap) {
   if (out_stop_now) {
      *out_stop_now = false;
   }

   const uint32_t ptype = (plane == 0u) ? 0u : 1u;

   // coeffs() (spec): first symbol is all_zero (aka txb_skip).
   const uint32_t txSzCtx = tx_sz_ctx_from_tx_size(tx_size);
   const uint32_t w4 = 1u << (kTxWidthLog2[tx_size] - 2u);
   const uint32_t h4 = 1u << (kTxHeightLog2[tx_size] - 2u);
   const uint32_t ctx = txb_skip_ctx(coeff_ctx, plane, x4, y4, w4, h4, bw_px, bh_px, tx_size);
   uint32_t all_zero = 0u;
   uint16_t *cdf = coeff_cdfs->txb_skip[txSzCtx][ctx];
   if (!av1_symbol_read_symbol(sd, cdf, 2u, &all_zero, err, err_cap)) {
      return false;
   }
   if (st && plane == 0u && block_index == 0u && tx_index == 0u && !st->block0_txb_skip_decoded) {
      st->block0_txb_skip_decoded = true;
      st->block0_txb_skip_ctx = ctx;
      st->block0_txb_skip = all_zero;
   }
   if (st && plane == 1u && block_index == 0u && tx_index == 0u && !st->block0_u_txb_skip_decoded) {
      st->block0_u_txb_skip_decoded = true;
      st->block0_u_txb_skip_ctx = ctx;
      st->block0_u_txb_skip = all_zero;
   }
   if (st && plane == 2u && block_index == 0u && tx_index == 0u && !st->block0_v_txb_skip_decoded) {
      st->block0_v_txb_skip_decoded = true;
      st->block0_v_txb_skip_ctx = ctx;
      st->block0_v_txb_skip = all_zero;
   }
   if (st && plane == 0u && block_index == 0u && tx_index == 1u && !st->block0_tx1_txb_skip_decoded) {
      st->block0_tx1_txb_skip_decoded = true;
      st->block0_tx1_x4 = x4;
      st->block0_tx1_y4 = y4;
      st->block0_tx1_txb_skip_ctx = ctx;
      st->block0_tx1_txb_skip = all_zero;
   }
   if (st && plane == 0u && block_index == 1u && tx_index == 0u && !st->block1_txb_skip_decoded) {
      st->block1_txb_skip_decoded = true;
      st->block1_r_mi = block_r;
      st->block1_c_mi = block_c;
      st->block1_txb_skip_ctx = ctx;
      st->block1_txb_skip = all_zero;
   }

   if (all_zero == 0u) {
      // eob_pt_* (spec): depends on transform size via eobMultisize.
      if (tx_size >= AV1_TX_SIZES_ALL) {
         snprintf(err, err_cap, "invalid tx_size=%u for eob_pt", tx_size);
         return false;
      }
      const uint32_t tx_wlog2 = kTxWidthLog2[tx_size];
      const uint32_t tx_hlog2 = kTxHeightLog2[tx_size];
      const uint32_t wcap = (tx_wlog2 > 5u) ? 5u : tx_wlog2;
      const uint32_t hcap = (tx_hlog2 > 5u) ? 5u : tx_hlog2;
      const uint32_t eobMultisize = wcap + hcap - 4u;

      const uint32_t eob_ctx = (get_tx_class_from_tx_type(tx_type) == AV1_TX_CLASS_2D) ? 0u : 1u;
      uint16_t *eob_cdf = NULL;
      uint32_t eob_nsyms = 0u;

      if (eobMultisize == 0u) {
         eob_cdf = coeff_cdfs->eob_pt_16[ptype][eob_ctx];
         eob_nsyms = 5u;
      } else if (eobMultisize == 1u) {
         eob_cdf = coeff_cdfs->eob_pt_32[ptype][eob_ctx];
         eob_nsyms = 6u;
      } else if (eobMultisize == 2u) {
         eob_cdf = coeff_cdfs->eob_pt_64[ptype][eob_ctx];
         eob_nsyms = 7u;
      } else if (eobMultisize == 3u) {
         eob_cdf = coeff_cdfs->eob_pt_128[ptype][eob_ctx];
         eob_nsyms = 8u;
      } else if (eobMultisize == 4u) {
         eob_cdf = coeff_cdfs->eob_pt_256[ptype][eob_ctx];
         eob_nsyms = 9u;
      } else if (eobMultisize == 5u) {
         eob_cdf = coeff_cdfs->eob_pt_512[ptype];
         eob_nsyms = 10u;
      } else {
         eob_cdf = coeff_cdfs->eob_pt_1024[ptype];
         eob_nsyms = 11u;
      }

      uint32_t eob_pt_sym = 0u;
      if (!av1_symbol_read_symbol(sd, eob_cdf, eob_nsyms, &eob_pt_sym, err, err_cap)) {
         return false;
      }

      const uint32_t eobPt = eob_pt_sym + 1u;

      if (st && plane == 0u && block_index == 0u && tx_index == 0u && !st->block0_eob_pt_decoded) {
         st->block0_eob_pt_decoded = true;
         st->block0_eob_pt = eobPt;
      }
      if (st && plane == 0u && block_index == 1u && tx_index == 0u && !st->block1_eob_pt_decoded) {
         st->block1_eob_pt_decoded = true;
         st->block1_eob_pt_ctx = eob_ctx;
         st->block1_eob_pt = eobPt;
      }

      // eob calculation (spec): consume eob_extra (S) and eob_extra_bit (L(1)).
      uint32_t eob = (eobPt < 2u) ? eobPt : ((1u << (eobPt - 2u)) + 1u);
      const int32_t eobShift0 = (eobPt >= 3u) ? (int32_t)(eobPt - 3u) : -1;
      if (eobShift0 >= 0) {
         const uint32_t ctxIdx = eobPt - 3u;
         if (ctxIdx >= AV1_EOB_COEF_CONTEXTS) {
            snprintf(err, err_cap, "invalid eobPt=%u for eob_extra", eobPt);
            return false;
         }

         uint32_t eob_extra = 0u;
         uint16_t *eob_extra_cdf = coeff_cdfs->eob_extra[txSzCtx][ptype][ctxIdx];
         if (!av1_symbol_read_symbol(sd, eob_extra_cdf, 2u, &eob_extra, err, err_cap)) {
            return false;
         }
         if (eob_extra) {
            eob += (1u << (uint32_t)eobShift0);
         }

         const uint32_t eobPtMinus2 = (eobPt > 2u) ? (eobPt - 2u) : 0u;
         for (uint32_t i = 1u; i < eobPtMinus2; i++) {
            const uint32_t shift = (eobPtMinus2 - 1u) - i;
            uint32_t bit = 0u;
            if (!av1_symbol_read_bool(sd, &bit, err, err_cap)) {
               return false;
            }
            if (bit) {
               eob += (1u << shift);
            }
         }
      }

      // Sanity check eob against segEob (spec coeffs()).
      uint32_t segEob = 0u;
      if (tx_size == AV1_TX_16X64 || tx_size == AV1_TX_64X16) {
         segEob = 512u;
      } else {
         const uint32_t txw = 1u << tx_wlog2;
         const uint32_t txh = 1u << tx_hlog2;
         const uint32_t wh = txw * txh;
         segEob = (wh < 1024u) ? wh : 1024u;
      }
      if (eob == 0u || eob > segEob) {
         snprintf(err, err_cap, "invalid eob=%u (segEob=%u, eobPt=%u)", eob, segEob, eobPt);
         return false;
      }

      if (st && plane == 0u && block_index == 0u && tx_index == 0u && !st->block0_eob_decoded) {
         st->block0_eob_decoded = true;
         st->block0_eob = eob;
      }
      if (st && plane == 0u && block_index == 1u && tx_index == 0u && !st->block1_eob_decoded) {
         st->block1_eob_decoded = true;
         st->block1_eob = eob;
      }

      // coeff_base_eob (spec): base level of the last non-zero coefficient.
      uint32_t coeff_base_eob_level = 0u;
      {
         const uint32_t c_eob = (eob > 0u) ? (eob - 1u) : 0u;
         const uint32_t cb_ctx = coeff_base_eob_ctx_from_c(tx_size, c_eob);
         if (cb_ctx >= AV1_SIG_COEF_CONTEXTS_EOB) {
            snprintf(err, err_cap, "invalid coeff_base_eob ctx=%u", cb_ctx);
            return false;
         }
         uint32_t cb_sym = 0u;
         uint16_t *cb_cdf = coeff_cdfs->coeff_base_eob[txSzCtx][ptype][cb_ctx];
         if (!av1_symbol_read_symbol(sd, cb_cdf, 3u, &cb_sym, err, err_cap)) {
            return false;
         }
         const uint32_t level = cb_sym + 1u;
         if (level < 1u || level > 3u) {
            snprintf(err, err_cap, "invalid coeff_base_eob level=%u", level);
            return false;
         }
         coeff_base_eob_level = level;
         if (st && plane == 0u && block_index == 0u && tx_index == 0u && !st->block0_coeff_base_eob_decoded) {
            st->block0_coeff_base_eob_decoded = true;
            st->block0_coeff_base_eob_ctx = cb_ctx;
            st->block0_coeff_base_eob_level = level;
         }
         if (st && plane == 0u && block_index == 1u && tx_index == 0u && !st->block1_coeff_base_eob_decoded) {
            st->block1_coeff_base_eob_decoded = true;
            st->block1_coeff_base_eob_ctx = cb_ctx;
            st->block1_coeff_base_eob_level = level;
         }

      }

      // For chroma planes, stop after the same stable prefix (txb_skip,eob_pt,eob,coeff_base_eob)
      // to keep the probe lightweight. The caller may still invoke coeffs() for both U and V.
      if (plane != 0u && !probe_try_exit_symbol) {
         if (out_stop_now) {
            *out_stop_now = true;
         }
         return true;
      }

      // For block1, try to reach the next milestone (coeff_base for c==0) when possible.
      // If eob==1, there are no coeff_base symbols (only the eob coefficient exists), so
      // stop after coeff_base_eob.
      if (!probe_try_exit_symbol && st && plane == 0u && block_index == 1u && tx_index == 0u && eob <= 1u) {
         if (out_stop_now) {
            *out_stop_now = true;
         }
         return true;
      }

      // coeff_base + coeff_br + sign bits (spec coeffs()):
      if (tx_size >= AV1_TX_SIZES_ALL) {
         snprintf(err, err_cap, "invalid tx_size=%u for coeffs", tx_size);
         return false;
      }

      const uint32_t adj = kAdjustedTxSize[tx_size];
      const uint32_t bwl = kTxWidthLog2[adj];
      const uint32_t width = 1u << bwl;
      const uint32_t height = 1u << kTxHeightLog2[adj];
      const uint32_t coeffs = width * height;
      const uint32_t segEobAdj = (coeffs < 1024u) ? coeffs : 1024u;

      if (eob <= segEobAdj) {
         uint16_t scan[1024];
         int32_t quant[1024] = {0};

         const uint32_t tx_class = get_tx_class_from_tx_type(tx_type);
         build_scan(tx_class, width, height, scan);

         // Seed + extend the eob coefficient.
         {
            const uint32_t c_eob = eob - 1u;
            const uint32_t pos_eob = scan[c_eob];
            if (pos_eob >= segEobAdj) {
               snprintf(err, err_cap, "invalid scan pos_eob=%u (segEob=%u)", pos_eob, segEobAdj);
               return false;
            }

            uint32_t level = coeff_base_eob_level;
            if (level > AV1_NUM_BASE_LEVELS) {
               for (uint32_t idx = 0u; idx < (AV1_COEFF_BASE_RANGE / (AV1_BR_CDF_SIZE - 1u)); idx++) {
                  const uint32_t br_ctx = coeff_br_ctx(tx_size, tx_type, pos_eob, quant);
                  uint32_t br_sym = 0u;
                  uint32_t br_tx = txSzCtx;
                  if (br_tx >= AV1_COEFF_BR_TX_SIZES) br_tx = AV1_COEFF_BR_TX_SIZES - 1u;
                  uint16_t *br_cdf = coeff_cdfs->coeff_br[br_tx][ptype][br_ctx];
                  if (!av1_symbol_read_symbol(sd, br_cdf, AV1_BR_CDF_SIZE, &br_sym, err, err_cap)) {
                     return false;
                  }
                  if (br_sym >= AV1_BR_CDF_SIZE) {
                     snprintf(err, err_cap, "invalid coeff_br sym=%u", br_sym);
                     return false;
                  }
                  level += br_sym;
                  if (st && plane == 0u && block_index == 0u && tx_index == 0u && !st->block0_coeff_br_decoded) {
                     st->block0_coeff_br_decoded = true;
                     st->block0_coeff_br_ctx = br_ctx;
                     st->block0_coeff_br_sym = br_sym;
                  }
                  if (br_sym < (AV1_BR_CDF_SIZE - 1u)) {
                     break;
                  }
               }
            }
            quant[pos_eob] = (int32_t)level;
         }

         // Decode coeff_base (c < eob-1), then optional coeff_br, then store Quant[pos].
         for (int32_t cc = (int32_t)eob - 2; cc >= 0; cc--) {
            const uint32_t coef_c = (uint32_t)cc;
            const uint32_t pos = scan[coef_c];
            if (pos >= segEobAdj) {
               snprintf(err, err_cap, "invalid scan pos=%u (segEob=%u)", pos, segEobAdj);
               return false;
            }
            const uint32_t cb_ctx = coeff_base_ctx_isEob0(tx_size, tx_type, bwl, width, height, pos, quant);
            if (cb_ctx >= AV1_SIG_COEF_CONTEXTS) {
               snprintf(err, err_cap, "invalid coeff_base ctx=%u", cb_ctx);
               return false;
            }
            uint32_t cb_sym = 0u;
            uint16_t *cb_cdf = coeff_cdfs->coeff_base[txSzCtx][ptype][cb_ctx];
            if (!av1_symbol_read_symbol(sd, cb_cdf, 4u, &cb_sym, err, err_cap)) {
               return false;
            }
            if (cb_sym > 3u) {
               snprintf(err, err_cap, "invalid coeff_base sym=%u", cb_sym);
               return false;
            }

            uint32_t level = cb_sym;
            if (level > AV1_NUM_BASE_LEVELS) {
               for (uint32_t idx = 0u; idx < (AV1_COEFF_BASE_RANGE / (AV1_BR_CDF_SIZE - 1u)); idx++) {
                  const uint32_t br_ctx = coeff_br_ctx(tx_size, tx_type, pos, quant);
                  uint32_t br_sym = 0u;
                  uint32_t br_tx = txSzCtx;
                  if (br_tx >= AV1_COEFF_BR_TX_SIZES) br_tx = AV1_COEFF_BR_TX_SIZES - 1u;
                  uint16_t *br_cdf = coeff_cdfs->coeff_br[br_tx][ptype][br_ctx];
                  if (!av1_symbol_read_symbol(sd, br_cdf, AV1_BR_CDF_SIZE, &br_sym, err, err_cap)) {
                     return false;
                  }
                  if (br_sym >= AV1_BR_CDF_SIZE) {
                     snprintf(err, err_cap, "invalid coeff_br sym=%u", br_sym);
                     return false;
                  }
                  level += br_sym;
                  if (st && plane == 0u && block_index == 0u && tx_index == 0u && !st->block0_coeff_br_decoded) {
                     st->block0_coeff_br_decoded = true;
                     st->block0_coeff_br_ctx = br_ctx;
                     st->block0_coeff_br_sym = br_sym;
                  }
                  if (br_sym < (AV1_BR_CDF_SIZE - 1u)) {
                     break;
                  }
               }
            }
            quant[pos] = (int32_t)level;

            // Keep reporting the c==0 coeff_base symbol as the stable milestone.
            if (plane == 0u && coef_c == 0u && st && block_index == 0u && tx_index == 0u && !st->block0_coeff_base_decoded) {
               st->block0_coeff_base_decoded = true;
               st->block0_coeff_base_ctx = cb_ctx;
               st->block0_coeff_base_level = cb_sym;
            }

            // For block1, stop once we've reached the c==0 coeff_base milestone.
            if (!probe_try_exit_symbol && plane == 0u && coef_c == 0u && st && block_index == 1u && tx_index == 0u && !st->block1_coeff_base_decoded) {
               st->block1_coeff_base_decoded = true;
               st->block1_coeff_base_ctx = cb_ctx;
               st->block1_coeff_base_level = cb_sym;
               if (out_stop_now) {
                  *out_stop_now = true;
               }
               return true;
            }
         }

         // Sign coding (dc_sign / sign_bit) + optional Exp-Golomb extension.
         uint32_t culLevel = 0u;
         uint32_t dcCategory = 0u;

         for (uint32_t coef_idx = 0u; coef_idx < eob; coef_idx++) {
            const uint32_t pos = scan[coef_idx];
            uint32_t sign = 0u;
            if (quant[pos] != 0) {
               if (coef_idx == 0u) {
                  const uint32_t dc_ctx = dc_sign_ctx(coeff_ctx, plane, x4, y4, w4, h4);
                  uint32_t dc_sym = 0u;
                  uint16_t *dc_cdf = coeff_cdfs->dc_sign[ptype][dc_ctx];
                  if (!av1_symbol_read_symbol(sd, dc_cdf, 2u, &dc_sym, err, err_cap)) {
                     return false;
                  }
                  sign = dc_sym;
                  if (st && plane == 0u && block_index == 0u && tx_index == 0u && !st->block0_dc_sign_decoded) {
                     st->block0_dc_sign_decoded = true;
                     st->block0_dc_sign_ctx = dc_ctx;
                     st->block0_dc_sign = dc_sym;
                  }
               } else {
                  uint32_t bit = 0u;
                  if (!av1_symbol_read_bool(sd, &bit, err, err_cap)) {
                     return false;
                  }
                  sign = bit;
               }
            }

            if ((uint32_t)(quant[pos] < 0 ? -quant[pos] : quant[pos]) > (AV1_NUM_BASE_LEVELS + AV1_COEFF_BASE_RANGE)) {
               uint32_t length = 0u;
               uint32_t golomb_length_bit = 0u;
               do {
                  length++;
                  if (!av1_symbol_read_bool(sd, &golomb_length_bit, err, err_cap)) {
                     return false;
                  }
               } while (!golomb_length_bit);

               uint32_t x = 1u;
               for (int32_t i = (int32_t)length - 2; i >= 0; i--) {
                  uint32_t golomb_data_bit = 0u;
                  if (!av1_symbol_read_bool(sd, &golomb_data_bit, err, err_cap)) {
                     return false;
                  }
                  x = (x << 1) | golomb_data_bit;
               }

               uint32_t q = x + AV1_COEFF_BASE_RANGE + AV1_NUM_BASE_LEVELS;
               q &= 0xFFFFFu;
               quant[pos] = (int32_t)q;
            }

            if (pos == 0u && quant[pos] > 0) {
               dcCategory = sign ? 1u : 2u;
            }

            uint32_t mag = (uint32_t)(quant[pos] < 0 ? -quant[pos] : quant[pos]);
            culLevel += mag;
            if (culLevel > 63u) {
               culLevel = 63u;
            }

            if (sign) {
               quant[pos] = -quant[pos];
            }
         }

         // Update spec-style contexts for future blocks / tx blocks.
         if (coeff_ctx) {
            for (uint32_t i = 0u; i < w4; i++) {
               const uint32_t x = x4 + i;
               if (x < coeff_ctx->cols[plane]) {
                  if (coeff_ctx->above_level[plane]) coeff_ctx->above_level[plane][x] = (uint8_t)culLevel;
                  if (coeff_ctx->above_dc[plane]) coeff_ctx->above_dc[plane][x] = (uint8_t)dcCategory;
               }
            }
            for (uint32_t i = 0u; i < h4; i++) {
               const uint32_t y = y4 + i;
               if (y < coeff_ctx->rows[plane]) {
                  if (coeff_ctx->left_level[plane]) coeff_ctx->left_level[plane][y] = (uint8_t)culLevel;
                  if (coeff_ctx->left_dc[plane]) coeff_ctx->left_dc[plane][y] = (uint8_t)dcCategory;
               }
            }
         }
      }
   }

   return true;
}

typedef struct Av1TileSbProbeState {
   uint32_t sb_origin_r;
   uint32_t sb_origin_c;
   uint32_t sb_mi_size; // 16 or 32

   // Models ReadDeltas in the AV1 spec; initialized per superblock.
   uint32_t read_deltas;

   // Tracks whether we already consumed cdef_idx for each 64x64 region inside the superblock.
   // For 64x64 SB: bit0 used. For 128x128 SB: bits [0..3] used for the 2x2 64x64 regions.
   uint8_t cdef_seen_mask;
} Av1TileSbProbeState;

static bool tile_read_delta_qindex(Av1SymbolDecoder *sd,
                                  const Av1TileDecodeParams *params,
                                  Av1TileSkipCdfs *mode_cdfs,
                                  bool mi_is_sb,
                                  uint32_t skip,
                                  char *err,
                                  size_t err_cap) {
   if (!sd || !params || !mode_cdfs) {
      snprintf(err, err_cap, "invalid args");
      return false;
   }

   // Spec: if (MiSize == sbSize && skip) return.
   if (mi_is_sb && skip) {
      return true;
   }

   uint32_t delta_q_abs_sym = 0;
   if (!av1_symbol_read_symbol(sd, mode_cdfs->delta_q_abs, AV1_DELTA_Q_ABS_SYMBOLS, &delta_q_abs_sym, err, err_cap)) {
      return false;
   }

   uint32_t delta_q_abs = delta_q_abs_sym;
   if (delta_q_abs_sym == AV1_DELTA_Q_SMALL) {
      uint32_t delta_q_rem_bits = 0;
      if (!av1_symbol_read_literal(sd, 3u, &delta_q_rem_bits, err, err_cap)) {
         return false;
      }
      delta_q_rem_bits++;

      uint32_t delta_q_abs_bits = 0;
      if (!av1_symbol_read_literal(sd, (unsigned)delta_q_rem_bits, &delta_q_abs_bits, err, err_cap)) {
         return false;
      }
      delta_q_abs = delta_q_abs_bits + (1u << delta_q_rem_bits) + 1u;
   }

   if (delta_q_abs) {
      uint32_t sign = 0;
      if (!av1_symbol_read_literal(sd, 1u, &sign, err, err_cap)) {
         return false;
      }
      const int32_t reduced = sign ? -(int32_t)delta_q_abs : (int32_t)delta_q_abs;
      const int32_t scale = (params->delta_q_res < 31u) ? (1 << (int32_t)params->delta_q_res) : 1;
      const int32_t delta = reduced * scale;

      int32_t next = (int32_t)mode_cdfs->current_qindex + delta;
      if (next < 1) next = 1;
      if (next > 255) next = 255;
      mode_cdfs->current_qindex = (uint32_t)next;
   }

   return true;
}

static bool tile_read_delta_lf(Av1SymbolDecoder *sd,
                              const Av1TileDecodeParams *params,
                              Av1TileSkipCdfs *mode_cdfs,
                              bool mi_is_sb,
                              uint32_t skip,
                              char *err,
                              size_t err_cap) {
   if (!sd || !params || !mode_cdfs) {
      snprintf(err, err_cap, "invalid args");
      return false;
   }

   // Spec: if (MiSize == sbSize && skip) return.
   if (mi_is_sb && skip) {
      return true;
   }

   if (!params->delta_lf_present) {
      return true;
   }

   uint32_t frame_lf_count = 1u;
   if (params->delta_lf_multi) {
      const uint32_t num_planes = params->mono_chrome ? 1u : 3u;
      frame_lf_count = (num_planes > 1u) ? AV1_FRAME_LF_COUNT : (AV1_FRAME_LF_COUNT - 2u);
   }
   if (frame_lf_count > AV1_FRAME_LF_COUNT) {
      frame_lf_count = AV1_FRAME_LF_COUNT;
   }

   for (uint32_t i = 0; i < frame_lf_count; i++) {
      uint16_t *cdf = params->delta_lf_multi ? mode_cdfs->delta_lf_multi[i] : mode_cdfs->delta_lf_abs;

      uint32_t delta_lf_abs_sym = 0;
      if (!av1_symbol_read_symbol(sd, cdf, AV1_DELTA_LF_ABS_SYMBOLS, &delta_lf_abs_sym, err, err_cap)) {
         return false;
      }

      uint32_t delta_lf_abs = delta_lf_abs_sym;
      if (delta_lf_abs_sym == AV1_DELTA_LF_SMALL) {
         uint32_t delta_lf_rem_bits = 0;
         if (!av1_symbol_read_literal(sd, 3u, &delta_lf_rem_bits, err, err_cap)) {
            return false;
         }
         const uint32_t nbits = delta_lf_rem_bits + 1u;

         uint32_t delta_lf_abs_bits = 0;
         if (!av1_symbol_read_literal(sd, (unsigned)nbits, &delta_lf_abs_bits, err, err_cap)) {
            return false;
         }
         delta_lf_abs = delta_lf_abs_bits + (1u << nbits) + 1u;
      }

      if (delta_lf_abs) {
         uint32_t sign = 0;
         if (!av1_symbol_read_literal(sd, 1u, &sign, err, err_cap)) {
            return false;
         }
         const int32_t reduced = sign ? -(int32_t)delta_lf_abs : (int32_t)delta_lf_abs;
         const int32_t scale = (params->delta_lf_res < 31u) ? (1 << (int32_t)params->delta_lf_res) : 1;
         const int32_t delta = reduced * scale;
         mode_cdfs->delta_lf_state[i] = clip3_i32(-AV1_MAX_LOOP_FILTER, AV1_MAX_LOOP_FILTER, mode_cdfs->delta_lf_state[i] + delta);
      }
   }

   return true;
}

static bool decode_block_stub(Av1SymbolDecoder *sd,
                              const Av1TileDecodeParams *params,
                              struct Av1TileSbProbeState *sb,
                              Av1TileSkipCdfs *mode_cdfs,
                              Av1TileCoeffCdfs *coeff_cdfs,
                              Av1TileCoeffCtx *coeff_ctx,
                              Av1MiSize *mi_grid,
                              uint32_t mi_rows,
                              uint32_t mi_cols,
                              uint32_t r,
                              uint32_t c,
                              uint32_t wlog2,
                              uint32_t hlog2,
                              Av1TileSyntaxProbeStats *st,
                              bool *out_stop,
                              char *err,
                              size_t err_cap) {
   if (!sd || !params || !mode_cdfs || !coeff_cdfs || !mi_grid || !coeff_ctx) {
      snprintf(err, err_cap, "invalid args");
      return false;
   }

   const uint32_t block_index = st ? st->blocks_decoded : 0u;

   // intra_frame_mode_info(): optionally read intra_segment_id() before skip.
   // This is currently try-EOT only to keep default probe output stable.
   if (params->probe_try_exit_symbol && params->segmentation_enabled && params->seg_id_pre_skip) {
      if (!tile_read_intra_segment_id(sd, params, mode_cdfs, mi_grid, mi_rows, mi_cols, r, c, wlog2, hlog2, 0u /*skip*/, err, err_cap)) {
         return false;
      }
   }

   const uint32_t ctx = skip_ctx_from_mi_grid(mi_grid, mi_rows, mi_cols, r, c);
   uint16_t *cdf = mode_cdfs->skip[ctx];
   uint32_t skip = 0;
   if (!av1_symbol_read_symbol(sd, cdf, 2, &skip, err, err_cap)) {
      return false;
   }

   mi_set_skip_block(mi_grid, mi_rows, mi_cols, r, c, wlog2, hlog2, skip);

   if (st && !st->block0_skip_decoded) {
      st->block0_skip_decoded = true;
      st->block0_r_mi = r;
      st->block0_c_mi = c;
      st->block0_wlog2 = wlog2;
      st->block0_hlog2 = hlog2;
      st->block0_skip_ctx = ctx;
      st->block0_skip = skip;
   }

   // try-EOT mode: consume a few additional per-superblock tile syntax elements which appear
   // before intra prediction modes (see intra_frame_mode_info in av1bitstream.html).
   // This is intentionally gated so default probe behavior remains stable for regression tests.
   if (params->probe_try_exit_symbol) {
      // intra_frame_mode_info(): intra_segment_id() (SegIdPreSkip==0) appears after skip.
      if (params->segmentation_enabled && !params->seg_id_pre_skip) {
         if (!tile_read_intra_segment_id(sd, params, mode_cdfs, mi_grid, mi_rows, mi_cols, r, c, wlog2, hlog2, skip, err, err_cap)) {
            return false;
         }
      }

      // We do not implement intrabc tile syntax yet.
      if (params->allow_intrabc) {
         if (out_stop) {
            *out_stop = true;
         }
         snprintf(err, err_cap, "unsupported: allow_intrabc=1 (use_intrabc not implemented)");
         return true;
      }

      // For try-EOT, derive per-block segment_id/qindex/Lossless.
      // Note: Frame-level coded_lossless is insufficient when segmentation is enabled.
   }

   uint32_t segment_id = 0u;
   if (params->probe_try_exit_symbol && params->segmentation_enabled && r < mi_rows && c < mi_cols) {
      segment_id = (uint32_t)mi_grid[mi_index(r, c, mi_cols)].segment_id;
      if (segment_id >= 8u) {
         segment_id = 0u;
      }
   }
   const uint32_t block_qindex = params->probe_try_exit_symbol ? qindex_for_segment(params, segment_id) : params->base_q_idx;
   const bool block_lossless = params->probe_try_exit_symbol ? lossless_for_segment(params, segment_id) : (params->coded_lossless != 0u);

   if (params->probe_try_exit_symbol) {

      // read_cdef(): cdef_idx is a literal (L(cdef_bits)) consumed once per 64x64 region.
      // We track just the minimal per-superblock state to avoid double-consuming.
      if (sb && !skip && !block_lossless && params->enable_cdef && params->cdef_bits > 0) {
         // 64x64 cdef regions are aligned to 16 MI units (64/4).
         const uint32_t cdef_mask = ~15u;
         const uint32_t rr = r & cdef_mask;
         const uint32_t cc = c & cdef_mask;

         uint32_t r_idx = 0;
         uint32_t c_idx = 0;
         if (sb->sb_mi_size == 32u) {
            const uint32_t dr = (rr >= sb->sb_origin_r) ? (rr - sb->sb_origin_r) : 0u;
            const uint32_t dc = (cc >= sb->sb_origin_c) ? (cc - sb->sb_origin_c) : 0u;
            r_idx = (dr >> 4);
            c_idx = (dc >> 4);
            if (r_idx > 1u) r_idx = 1u;
            if (c_idx > 1u) c_idx = 1u;
         }
         const uint32_t region = (r_idx << 1) | c_idx;
         const uint8_t bit = (uint8_t)(1u << region);
         if ((sb->cdef_seen_mask & bit) == 0) {
            uint32_t tmp = 0;
            if (!av1_symbol_read_literal(sd, (unsigned)params->cdef_bits, &tmp, err, err_cap)) {
               return false;
            }
            sb->cdef_seen_mask |= bit;
         }
      }

      // read_delta_qindex()/read_delta_lf(): entropy-coded deltas in intra_frame_mode_info.
      // Spec: ReadDeltas is initialized per superblock from delta_q_present, and is reset after
      // the first block in the superblock.
      if (sb && sb->read_deltas && params->delta_q_present) {
         const uint32_t bw4 = 1u << wlog2;
         const uint32_t bh4 = 1u << hlog2;
         const bool mi_is_sb = (bw4 == sb->sb_mi_size) && (bh4 == sb->sb_mi_size);

         if (!tile_read_delta_qindex(sd, params, mode_cdfs, mi_is_sb, skip, err, err_cap)) {
            return false;
         }
         if (!tile_read_delta_lf(sd, params, mode_cdfs, mi_is_sb, skip, err, err_cap)) {
            return false;
         }
      }
      // Spec resets ReadDeltas after the first block in the superblock.
      if (sb) {
         sb->read_deltas = 0;
      }
   }

   uint32_t y_mode_ctx = size_group_from_wlog2_hlog2(wlog2, hlog2);
   if (y_mode_ctx >= AV1_Y_MODE_CONTEXTS) {
      y_mode_ctx = AV1_Y_MODE_CONTEXTS - 1u;
   }
   uint16_t *y_cdf = mode_cdfs->y_mode[y_mode_ctx];
   uint32_t y_mode = 0;
   if (!av1_symbol_read_symbol(sd, y_cdf, AV1_INTRA_MODES, &y_mode, err, err_cap)) {
      return false;
   }
   mi_set_y_mode_block(mi_grid, mi_rows, mi_cols, r, c, wlog2, hlog2, y_mode);
   if (st && !st->block0_y_mode_decoded) {
      st->block0_y_mode_decoded = true;
      st->block0_y_mode_ctx = y_mode_ctx;
      st->block0_y_mode = y_mode;
   }

   // Spec: when skip==1, there are no residual coefficients for the block.
   // Our probe still decodes mode info for milestones, but must not consume coeffs().
   if (skip) {
      goto block_done;
   }

   // Optional intra angle deltas for luma (only for directional intra modes).
   {
      uint32_t dir_idx = 0u;
      if (intra_directional_index(y_mode, &dir_idx)) {
         uint16_t *angle_cdf = mode_cdfs->angle_delta[dir_idx];
         uint32_t angle_sym = 0;
         if (!av1_symbol_read_symbol(sd, angle_cdf, AV1_ANGLE_DELTA_SYMBOLS, &angle_sym, err, err_cap)) {
            return false;
         }
         if (st && !st->block0_angle_delta_y_decoded) {
            st->block0_angle_delta_y_decoded = true;
            st->block0_angle_delta_y = (int32_t)angle_sym - (int32_t)AV1_MAX_ANGLE_DELTA;
         }
      }
   }

   uint32_t uv_mode = 0;
   bool cfl_allowed = false;
   if (!params->mono_chrome) {
      // uv_mode cdf selection per spec (see av1bitstream.html): depends on Lossless and block size.
      if (y_mode >= AV1_INTRA_MODES) {
         snprintf(err, err_cap, "invalid y_mode=%u", y_mode);
         return false;
      }

      const uint32_t luma_w_px = (1u << wlog2) * 4u;
      const uint32_t luma_h_px = (1u << hlog2) * 4u;

      if (block_lossless) {
         uint32_t chroma_w = (params->subsampling_x ? (luma_w_px >> params->subsampling_x) : luma_w_px);
         uint32_t chroma_h = (params->subsampling_y ? (luma_h_px >> params->subsampling_y) : luma_h_px);
         if (chroma_w < 4u) chroma_w = 4u;
         if (chroma_h < 4u) chroma_h = 4u;
         cfl_allowed = (chroma_w == 4u) && (chroma_h == 4u);
      } else {
         const uint32_t max_wh = (luma_w_px > luma_h_px) ? luma_w_px : luma_h_px;
         cfl_allowed = (max_wh <= 32u);
      }

      uint16_t *uv_cdf = cfl_allowed ? mode_cdfs->uv_mode_cfl_allowed[y_mode] : mode_cdfs->uv_mode_cfl_not_allowed[y_mode];
      const uint32_t uv_n = cfl_allowed ? AV1_UV_INTRA_MODES_CFL_ALLOWED : AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED;
      if (!av1_symbol_read_symbol(sd, uv_cdf, uv_n, &uv_mode, err, err_cap)) {
         return false;
      }
      if (st && !st->block0_uv_mode_decoded) {
         st->block0_uv_mode_decoded = true;
         st->block0_uv_mode = uv_mode;
      }

      if (cfl_allowed && uv_mode == AV1_UV_MODE_CFL) {
         uint32_t cfl_alpha_signs = 0;
         int32_t alpha_u = 0;
         int32_t alpha_v = 0;
         if (!decode_cfl_alphas(sd, mode_cdfs, &cfl_alpha_signs, &alpha_u, &alpha_v, err, err_cap)) {
            return false;
         }
         if (st && !st->block0_cfl_alphas_decoded) {
            st->block0_cfl_alphas_decoded = true;
            st->block0_cfl_alpha_signs = cfl_alpha_signs;
            st->block0_cfl_alpha_u = alpha_u;
            st->block0_cfl_alpha_v = alpha_v;
         }
      }
   }

   {
      uint32_t dir_idx = 0u;
      if (!params->mono_chrome && intra_directional_index(uv_mode, &dir_idx)) {
         uint16_t *angle_cdf = mode_cdfs->angle_delta[dir_idx];
         uint32_t angle_sym = 0;
         if (!av1_symbol_read_symbol(sd, angle_cdf, AV1_ANGLE_DELTA_SYMBOLS, &angle_sym, err, err_cap)) {
            return false;
         }
         if (st && !st->block0_angle_delta_uv_decoded) {
            st->block0_angle_delta_uv_decoded = true;
            st->block0_angle_delta_uv = (int32_t)angle_sym - (int32_t)AV1_MAX_ANGLE_DELTA;
         }
      }
   }

   // palette_mode_info() (spec): only possible with screen content tools and certain block sizes.
   // We only consume the initial entropy-coded presence flag(s) and palette size, then stop early
   // if palette is actually used (palette color payload not implemented yet).
   uint32_t palette_size_y = 0;
   uint32_t palette_size_uv = 0;
   {
      const uint32_t luma_w_px = (1u << wlog2) * 4u;
      const uint32_t luma_h_px = (1u << hlog2) * 4u;
      const bool mi_ge_8x8 = (wlog2 >= 1u) && (hlog2 >= 1u);
      const bool le_64 = (luma_w_px <= 64u) && (luma_h_px <= 64u);

      if (params->allow_screen_content_tools && mi_ge_8x8 && le_64) {
         uint32_t bsize_ctx = wlog2 + hlog2;
         if (bsize_ctx < 2u) {
            bsize_ctx = 0u;
         } else {
            bsize_ctx -= 2u;
         }
         if (bsize_ctx >= AV1_PALETTE_BLOCK_SIZE_CONTEXTS) {
            bsize_ctx = AV1_PALETTE_BLOCK_SIZE_CONTEXTS - 1u;
         }

         if (y_mode == 0u /* DC_PRED */) {
            uint32_t has_palette_y = 0;
            const uint32_t ctx = palette_y_ctx_from_mi_grid(mi_grid, mi_rows, mi_cols, r, c);
            if (!av1_symbol_read_symbol(sd, mode_cdfs->palette_y_mode[bsize_ctx][ctx], 2, &has_palette_y, err, err_cap)) {
               return false;
            }
            if (st && !st->block0_has_palette_y_decoded) {
               st->block0_has_palette_y_decoded = true;
               st->block0_has_palette_y = has_palette_y;
            }

            if (has_palette_y) {
               uint32_t palette_size_y_minus_2 = 0;
               if (!av1_symbol_read_symbol(sd, mode_cdfs->palette_y_size[bsize_ctx], AV1_PALETTE_SIZES, &palette_size_y_minus_2, err, err_cap)) {
                  return false;
               }
               palette_size_y = palette_size_y_minus_2 + 2u;
               mi_set_palette_sizes_block(mi_grid, mi_rows, mi_cols, r, c, wlog2, hlog2, palette_size_y, 0u);
               if (st && !st->block0_palette_size_y_decoded) {
                  st->block0_palette_size_y_decoded = true;
                  st->block0_palette_size_y = palette_size_y;
               }
               if (st) {
                  st->blocks_decoded++;
               }
               if (out_stop) {
                  *out_stop = true;
               }
               return true;
            }
         }

         if (!params->mono_chrome && uv_mode == 0u /* DC_PRED */) {
            uint32_t has_palette_uv = 0;
            const uint32_t ctx = (palette_size_y > 0u) ? 1u : 0u;
            if (!av1_symbol_read_symbol(sd, mode_cdfs->palette_uv_mode[ctx], 2, &has_palette_uv, err, err_cap)) {
               return false;
            }
            if (st && !st->block0_has_palette_uv_decoded) {
               st->block0_has_palette_uv_decoded = true;
               st->block0_has_palette_uv = has_palette_uv;
            }

            if (has_palette_uv) {
               uint32_t palette_size_uv_minus_2 = 0;
               if (!av1_symbol_read_symbol(sd, mode_cdfs->palette_uv_size[bsize_ctx], AV1_PALETTE_SIZES, &palette_size_uv_minus_2, err, err_cap)) {
                  return false;
               }
               palette_size_uv = palette_size_uv_minus_2 + 2u;
               mi_set_palette_sizes_block(mi_grid, mi_rows, mi_cols, r, c, wlog2, hlog2, 0u, palette_size_uv);
               if (st && !st->block0_palette_size_uv_decoded) {
                  st->block0_palette_size_uv_decoded = true;
                  st->block0_palette_size_uv = palette_size_uv;
               }
               if (st) {
                  st->blocks_decoded++;
               }
               if (out_stop) {
                  *out_stop = true;
               }
               return true;
            }
         }
      }
   }

   // filter_intra_mode_info() (spec): may appear for intra blocks based on seq flag and block characteristics.
   // Only possible when PaletteSizeY==0.
   uint32_t use_filter_intra = 0u;
   uint32_t filter_intra_mode = 0u;
   bool use_filter_intra_decoded = false;
   bool filter_intra_mode_decoded = false;
   {
      const uint32_t luma_w_px = (1u << wlog2) * 4u;
      const uint32_t luma_h_px = (1u << hlog2) * 4u;
      const uint32_t max_wh = (luma_w_px > luma_h_px) ? luma_w_px : luma_h_px;

      if (params->enable_filter_intra && y_mode == 0u /* DC_PRED */ && max_wh <= 32u && palette_size_y == 0u) {
         uint32_t mi_size = 0;
         if (!mi_size_index_from_wlog2_hlog2(wlog2, hlog2, &mi_size) || mi_size >= AV1_BLOCK_SIZES) {
            snprintf(err, err_cap, "unsupported MiSize mapping for %ux%u px", luma_w_px, luma_h_px);
            return false;
         }

         use_filter_intra = 0;
         if (!av1_symbol_read_symbol(sd, mode_cdfs->filter_intra[mi_size], 2, &use_filter_intra, err, err_cap)) {
            return false;
         }
         use_filter_intra_decoded = true;
         if (st && !st->block0_use_filter_intra_decoded) {
            st->block0_use_filter_intra_decoded = true;
            st->block0_use_filter_intra = use_filter_intra;
         }

         if (use_filter_intra) {
            filter_intra_mode = 0;
            if (!av1_symbol_read_symbol(sd, mode_cdfs->filter_intra_mode, AV1_INTRA_FILTER_MODES, &filter_intra_mode, err, err_cap)) {
               return false;
            }
            filter_intra_mode_decoded = true;
            if (st && !st->block0_filter_intra_mode_decoded) {
               st->block0_filter_intra_mode_decoded = true;
               st->block0_filter_intra_mode = filter_intra_mode;
            }
         }
      }
   }

   // read_tx_size(allowSelect) (spec): for intra blocks, allowSelect is always true
   // because allowSelect = (!skip || !is_inter) and is_inter==0.
   // We only consume tx_depth when TxMode == TX_MODE_SELECT and !Lossless, but TxSize is always derivable.
   uint32_t mi_size = 0;
   if (!mi_size_index_from_wlog2_hlog2(wlog2, hlog2, &mi_size)) {
      snprintf(err, err_cap, "unsupported MiSize for tx_size (%ux%u px)", (1u << wlog2) * 4u, (1u << hlog2) * 4u);
      return false;
   }

   if (st) {
      st->block0_tx_mode = params->tx_mode;
   }

   uint32_t tx_size = AV1_TX_4X4;
   if (!block_lossless) {
      if (!max_tx_size_rect_from_mi_size(mi_size, &tx_size)) {
         snprintf(err, err_cap, "unsupported MiSize for Max_Tx_Size_Rect (%u)", mi_size);
         return false;
      }
   }

   if (!block_lossless && params->tx_mode == 2u /* TX_MODE_SELECT */ && mi_size > 0u /* MiSize > BLOCK_4X4 */) {
      const uint32_t ctx = 0u; // Correct for the first block; full ctx derivation comes later.
      const uint32_t max_tx_depth = max_tx_depth_from_mi_size(mi_size);
      uint16_t *cdf = NULL;
      uint32_t nsyms = 0;
      if (max_tx_depth == 4u) {
         cdf = mode_cdfs->tx64x64[ctx];
         nsyms = 3u;
      } else if (max_tx_depth == 3u) {
         cdf = mode_cdfs->tx32x32[ctx];
         nsyms = 3u;
      } else if (max_tx_depth == 2u) {
         cdf = mode_cdfs->tx16x16[ctx];
         nsyms = 3u;
      } else {
         cdf = mode_cdfs->tx8x8[ctx];
         nsyms = 2u;
      }

      uint32_t tx_depth = 0;
      if (!av1_symbol_read_symbol(sd, cdf, nsyms, &tx_depth, err, err_cap)) {
         return false;
      }
      if (st && !st->block0_tx_depth_decoded) {
         st->block0_tx_depth_decoded = true;
         st->block0_tx_depth = tx_depth;
      }

      // Apply Split_Tx_Size tx_depth times.
      const uint32_t depth_cap = (tx_depth > AV1_MAX_TX_DEPTH) ? AV1_MAX_TX_DEPTH : tx_depth;
      for (uint32_t i = 0; i < depth_cap; i++) {
         uint32_t next_tx = 0;
         if (!split_tx_size(tx_size, &next_tx)) {
            snprintf(err, err_cap, "unsupported tx_size split (tx_size=%u)", tx_size);
            return false;
         }
         tx_size = next_tx;
      }
   }

   if (st && !st->block0_tx_size_decoded) {
      st->block0_tx_size_decoded = true;
      st->block0_tx_size = tx_size;
   }

   // transform_type() (spec): for our intra stub, decode intra_tx_type when the
   // tx set allows it and qindex>0. Otherwise TxType is forced to DCT_DCT.
   uint32_t tx_type = AV1_TX_TYPE_DCT_DCT;
   {
      const uint32_t set = get_tx_set_intra(tx_size, params->reduced_tx_set);
      const uint32_t txSzSqrUp = (tx_size < AV1_TX_SIZES_ALL) ? (uint32_t)kTxSizeSqrUp[tx_size] : 0u;

      if (!block_lossless && txSzSqrUp <= 3u && set != AV1_TX_SET_DCTONLY && block_qindex > 0u) {
         uint32_t intraDir = y_mode;
         if (use_filter_intra_decoded && use_filter_intra) {
            // Filter_Intra_Mode_To_Intra_Dir = { DC_PRED, V_PRED, H_PRED, D157_PRED, DC_PRED }.
            static const uint8_t kFilterIntraModeToIntraDir[5] = {0u, 1u, 2u, 6u, 0u};
            if (!filter_intra_mode_decoded || filter_intra_mode >= 5u) {
               snprintf(err, err_cap, "invalid filter_intra_mode=%u", filter_intra_mode);
               return false;
            }
            intraDir = (uint32_t)kFilterIntraModeToIntraDir[filter_intra_mode];
         }
         if (intraDir >= AV1_INTRA_MODES) {
            snprintf(err, err_cap, "invalid intraDir=%u", intraDir);
            return false;
         }

         const uint32_t txSzSqr = (tx_size < AV1_TX_SIZES_ALL) ? (uint32_t)kTxSizeSqr[tx_size] : 0u;
         uint32_t intra_tx_type = 0u;

         if (set == AV1_TX_SET_INTRA_1) {
            if (txSzSqr >= 2u) {
               snprintf(err, err_cap, "invalid Tx_Size_Sqr=%u for TX_SET_INTRA_1", txSzSqr);
               return false;
            }
            uint16_t *cdf = mode_cdfs->intra_tx_type_set1[txSzSqr][intraDir];
            if (!av1_symbol_read_symbol(sd, cdf, AV1_INTRA_TX_TYPE_SET1_SYMBOLS, &intra_tx_type, err, err_cap)) {
               return false;
            }
            static const uint8_t kInv[AV1_INTRA_TX_TYPE_SET1_SYMBOLS] = {
               AV1_TX_TYPE_IDTX,
               AV1_TX_TYPE_DCT_DCT,
               AV1_TX_TYPE_V_DCT,
               AV1_TX_TYPE_H_DCT,
               AV1_TX_TYPE_ADST_ADST,
               AV1_TX_TYPE_ADST_DCT,
               AV1_TX_TYPE_DCT_ADST,
            };
            if (intra_tx_type >= AV1_INTRA_TX_TYPE_SET1_SYMBOLS) {
               snprintf(err, err_cap, "invalid intra_tx_type=%u", intra_tx_type);
               return false;
            }
            tx_type = (uint32_t)kInv[intra_tx_type];
         } else if (set == AV1_TX_SET_INTRA_2) {
            if (txSzSqr >= 3u) {
               snprintf(err, err_cap, "invalid Tx_Size_Sqr=%u for TX_SET_INTRA_2", txSzSqr);
               return false;
            }
            uint16_t *cdf = mode_cdfs->intra_tx_type_set2[txSzSqr][intraDir];
            if (!av1_symbol_read_symbol(sd, cdf, AV1_INTRA_TX_TYPE_SET2_SYMBOLS, &intra_tx_type, err, err_cap)) {
               return false;
            }
            static const uint8_t kInv[AV1_INTRA_TX_TYPE_SET2_SYMBOLS] = {
               AV1_TX_TYPE_IDTX,
               AV1_TX_TYPE_DCT_DCT,
               AV1_TX_TYPE_ADST_ADST,
               AV1_TX_TYPE_ADST_DCT,
               AV1_TX_TYPE_DCT_ADST,
            };
            if (intra_tx_type >= AV1_INTRA_TX_TYPE_SET2_SYMBOLS) {
               snprintf(err, err_cap, "invalid intra_tx_type=%u", intra_tx_type);
               return false;
            }
            tx_type = (uint32_t)kInv[intra_tx_type];
         }

         if (st && !st->block0_tx_type_decoded) {
            st->block0_tx_type_decoded = true;
            st->block0_tx_type = tx_type;
         }
      }
   }

   // coeffs() (spec): iterate luma transform blocks in raster order.
   {
      if (tx_size >= AV1_TX_SIZES_ALL) {
         snprintf(err, err_cap, "invalid tx_size=%u for coeffs", tx_size);
         return false;
      }

      const uint32_t bw_px = (1u << wlog2) * 4u;
      const uint32_t bh_px = (1u << hlog2) * 4u;
      const uint32_t bw4 = (bw_px >> 2u);
      const uint32_t bh4 = (bh_px >> 2u);
      const uint32_t tx_w4 = 1u << (kTxWidthLog2[tx_size] - 2u);
      const uint32_t tx_h4 = 1u << (kTxHeightLog2[tx_size] - 2u);
      if (tx_w4 == 0u || tx_h4 == 0u) {
         snprintf(err, err_cap, "invalid tx dims");
         return false;
      }
      if ((bw4 % tx_w4) != 0u || (bh4 % tx_h4) != 0u) {
         snprintf(err, err_cap, "unsupported tx tiling (block=%ux%u 4x4, tx=%ux%u 4x4)", bw4, bh4, tx_w4, tx_h4);
         return false;
      }
      const uint32_t tx_cols = bw4 / tx_w4;
      const uint32_t tx_rows = bh4 / tx_h4;
      const uint32_t total_tx = tx_cols * tx_rows;

      uint32_t max_tx = 1u;
      if (st && block_index == 0u) {
         max_tx = (total_tx >= 2u) ? 2u : total_tx;
      }
      if (params->probe_try_exit_symbol) {
         max_tx = total_tx;
      }

      for (uint32_t tx_index = 0u; tx_index < max_tx; tx_index++) {
         const uint32_t tx = (tx_cols == 0u) ? 0u : (tx_index % tx_cols);
         const uint32_t ty = (tx_cols == 0u) ? 0u : (tx_index / tx_cols);
         const uint32_t x4 = c + tx * tx_w4;
         const uint32_t y4 = r + ty * tx_h4;

         bool stop_now = false;
         if (!decode_coeffs_luma_one_tx_block(sd,
                                              coeff_cdfs,
                                              coeff_ctx,
                                              0u,
                                              block_index,
                                              r,
                                              c,
                                              tx_index,
                                              x4,
                                              y4,
                                              bw_px,
                                              bh_px,
                                              tx_size,
                                              tx_type,
                                              params->probe_try_exit_symbol,
                                              st,
                                              &stop_now,
                                              err,
                                              err_cap)) {
            return false;
         }

         if (st && block_index == 0u) {
            st->block0_tx_blocks_decoded = tx_index + 1u;
         }

         if (stop_now) {
            goto block_done;
         }
      }
   }

   // coeffs() (spec): for chroma planes, use get_tx_size(plane,txSz) and compute_tx_type().
   // We only decode a lightweight, stable prefix for one tx block per chroma plane.
   if (!params->mono_chrome) {
      static const uint8_t kModeToTxfmUv[AV1_UV_INTRA_MODES_CFL_ALLOWED] = {
          AV1_TX_TYPE_DCT_DCT,   // DC_PRED
          AV1_TX_TYPE_ADST_DCT,  // V_PRED
          AV1_TX_TYPE_DCT_ADST,  // H_PRED
          AV1_TX_TYPE_DCT_DCT,   // D45_PRED
          AV1_TX_TYPE_ADST_ADST, // D135_PRED
          AV1_TX_TYPE_ADST_DCT,  // D113_PRED
          AV1_TX_TYPE_DCT_ADST,  // D157_PRED
          AV1_TX_TYPE_DCT_ADST,  // D203_PRED
          AV1_TX_TYPE_ADST_DCT,  // D67_PRED
          AV1_TX_TYPE_ADST_ADST, // SMOOTH_PRED
          AV1_TX_TYPE_ADST_DCT,  // SMOOTH_V_PRED
          AV1_TX_TYPE_DCT_ADST,  // SMOOTH_H_PRED
          AV1_TX_TYPE_ADST_ADST, // PAETH_PRED
          AV1_TX_TYPE_DCT_DCT,   // UV_CFL_PRED
      };

      for (uint32_t plane = 1u; plane <= 2u; plane++) {
         const uint32_t subx = params->subsampling_x;
         const uint32_t suby = params->subsampling_y;
         const uint32_t pwlog2 = (wlog2 > subx) ? (wlog2 - subx) : 0u;
         const uint32_t phlog2 = (hlog2 > suby) ? (hlog2 - suby) : 0u;
         const uint32_t bw_px = (1u << pwlog2) * 4u;
         const uint32_t bh_px = (1u << phlog2) * 4u;
         const uint32_t bw4 = 1u << pwlog2;
         const uint32_t bh4 = 1u << phlog2;

         uint32_t plane_tx_size = AV1_TX_4X4;
         if (!get_tx_size_for_plane(plane, tx_size, wlog2, hlog2, subx, suby, &plane_tx_size)) {
            snprintf(err, err_cap, "failed get_tx_size_for_plane(plane=%u,tx_size=%u)", plane, tx_size);
            return false;
         }
         if (plane_tx_size >= AV1_TX_SIZES_ALL) {
            snprintf(err, err_cap, "invalid plane_tx_size=%u", plane_tx_size);
            return false;
         }

         uint32_t plane_tx_type = AV1_TX_TYPE_DCT_DCT;
         if (!block_lossless) {
            if (uv_mode < AV1_UV_INTRA_MODES_CFL_ALLOWED) {
               plane_tx_type = (uint32_t)kModeToTxfmUv[uv_mode];
            }
            const uint32_t set = get_tx_set_intra(plane_tx_size, params->reduced_tx_set);
            if (!is_tx_type_in_set_intra(set, plane_tx_type)) {
               plane_tx_type = AV1_TX_TYPE_DCT_DCT;
            }
         }

         const uint32_t tx_w4 = 1u << (kTxWidthLog2[plane_tx_size] - 2u);
         const uint32_t tx_h4 = 1u << (kTxHeightLog2[plane_tx_size] - 2u);
         if (tx_w4 == 0u || tx_h4 == 0u) {
            snprintf(err, err_cap, "invalid chroma tx dims");
            return false;
         }
         if ((bw4 % tx_w4) != 0u || (bh4 % tx_h4) != 0u) {
            snprintf(err,
                     err_cap,
                     "unsupported chroma tx tiling (plane=%u block=%ux%u 4x4, tx=%ux%u 4x4)",
                     plane,
                     bw4,
                     bh4,
                     tx_w4,
                     tx_h4);
            return false;
         }

         const uint32_t tx_cols = bw4 / tx_w4;
         const uint32_t tx_rows = bh4 / tx_h4;
         if (tx_cols == 0u || tx_rows == 0u) {
            continue;
         }

         const uint32_t base_x4 = c >> subx;
         const uint32_t base_y4 = r >> suby;
         uint32_t max_plane_tx = 1u;
         if (params->probe_try_exit_symbol) {
            max_plane_tx = tx_cols * tx_rows;
         }

         for (uint32_t tx_index = 0u; tx_index < max_plane_tx; tx_index++) {
            const uint32_t tx = (tx_cols == 0u) ? 0u : (tx_index % tx_cols);
            const uint32_t ty = (tx_cols == 0u) ? 0u : (tx_index / tx_cols);
            const uint32_t x4 = base_x4 + tx * tx_w4;
            const uint32_t y4 = base_y4 + ty * tx_h4;

            bool stop_now = false;
            if (!decode_coeffs_luma_one_tx_block(sd,
                                                 coeff_cdfs,
                                                 coeff_ctx,
                                                 plane,
                                                 block_index,
                                                 r,
                                                 c,
                                                 tx_index,
                                                 x4,
                                                 y4,
                                                 bw_px,
                                                 bh_px,
                                                 plane_tx_size,
                                                 plane_tx_type,
                                                 params->probe_try_exit_symbol,
                                                 st,
                                                 &stop_now,
                                                 err,
                                                 err_cap)) {
               return false;
            }

            if (stop_now) {
               break;
            }

            if (!params->probe_try_exit_symbol) {
               // Default probe mode: only decode the first transform block per chroma plane.
               break;
            }
         }
      }
   }

block_done:

   if (st) {
      st->blocks_decoded++;
   }
   if (out_stop) {
      if (params->probe_try_exit_symbol) {
         // Experimental mode: keep traversing leaf blocks.
         *out_stop = false;
      } else {
         // If caller isn't collecting stats, keep the legacy behavior: stop after one block.
         // Otherwise stop after decoding two blocks.
         if (!st) {
            *out_stop = true;
         } else {
            *out_stop = (st->blocks_decoded >= 2u);
         }
      }
   }
   return true;
}

static bool decode_partition_rec(Av1SymbolDecoder *sd,
                                 Av1TilePartitionCdfs *cdfs,
                                 const Av1TileDecodeParams *params,
                                 Av1TileSbProbeState *sb,
                                 Av1TileSkipCdfs *skip_cdfs,
                                 Av1TileCoeffCdfs *coeff_cdfs,
                                 Av1TileCoeffCtx *coeff_ctx,
                                 Av1MiSize *mi_grid,
                                 uint32_t mi_rows,
                                 uint32_t mi_cols,
                                 uint32_t r,
                                 uint32_t c,
                                 uint32_t bsl,
                                 uint32_t sb_bsl_for_stats,
                                 Av1TileSyntaxProbeStats *st,
                                 bool *out_stop,
                                 char *err,
                                 size_t err_cap) {
   if (!sd || !cdfs || !params || !mi_grid || !coeff_cdfs || !coeff_ctx) {
      snprintf(err, err_cap, "invalid args");
      return false;
   }

   if (out_stop && *out_stop) {
      return true;
   }

   // Out-of-tile sub-blocks are ignored.
   if (r >= mi_rows || c >= mi_cols) {
      return true;
   }

   // Base case: bSize < BLOCK_8X8 => partition = NONE.
   if (bsl == 0) {
      mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, 0, 0, st);
      return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, 0, 0, st, out_stop, err, err_cap);
   }

   const uint32_t num4x4 = bsl_to_num4x4(bsl);
   const uint32_t half = num4x4 >> 1;
   const uint32_t quarter = half >> 1;

   const bool has_rows = (r + half) < mi_rows;
   const bool has_cols = (c + half) < mi_cols;

   Av1PartitionType partition = AV1_PARTITION_SPLIT;

   if (!has_rows && !has_cols) {
      // Forced by spec: partition = SPLIT.
      if (st) {
         st->partition_forced_splits++;
      }
      partition = AV1_PARTITION_SPLIT;

      if (st && !st->partition_decoded && r == 0 && c == 0 && bsl == sb_bsl_for_stats) {
         st->partition_decoded = true;
         st->partition_forced = true;
         st->partition_bsl = bsl;
         st->partition_ctx = 0;
         st->partition_symbol = (uint32_t)partition;
      }
   } else if (has_rows && has_cols) {
      const uint32_t ctx = partition_ctx_from_mi_grid(mi_grid, mi_rows, mi_cols, r, c, bsl);
      uint16_t *cdf = NULL;
      size_t n = 0;
      if (!select_partition_cdf(cdfs, bsl, ctx, &cdf, &n)) {
         snprintf(err, err_cap, "unsupported partition cdf for bsl=%u ctx=%u", bsl, ctx);
         return false;
      }

      uint32_t sym = 0;
      if (!av1_symbol_read_symbol(sd, cdf, n, &sym, err, err_cap)) {
         return false;
      }
      if (st) {
         st->partition_symbols_read++;
      }
      partition = (Av1PartitionType)sym;

      if (st && !st->partition_decoded && r == 0 && c == 0 && bsl == sb_bsl_for_stats) {
         st->partition_decoded = true;
         st->partition_forced = false;
         st->partition_bsl = bsl;
         st->partition_ctx = ctx;
         st->partition_symbol = (uint32_t)partition;
      }
   } else if (has_cols) {
      // split_or_horz
      if (bsl == 1) {
         snprintf(err, err_cap, "unsupported: split_or_horz with bsl=1");
         return false;
      }
      const uint32_t ctx = partition_ctx_from_mi_grid(mi_grid, mi_rows, mi_cols, r, c, bsl);
      uint16_t *partition_cdf = NULL;
      size_t n = 0;
      if (!select_partition_cdf(cdfs, bsl, ctx, &partition_cdf, &n)) {
         snprintf(err, err_cap, "unsupported partition cdf for split_or_horz (bsl=%u ctx=%u)", bsl, ctx);
         return false;
      }
      (void)n;
      uint16_t bool_cdf[3];
      if (!derive_split_or_horz_cdf(partition_cdf, bsl == 5u, bool_cdf)) {
         snprintf(err, err_cap, "failed to derive split_or_horz cdf");
         return false;
      }
      uint32_t split = 0;
      if (!av1_symbol_read_symbol(sd, bool_cdf, 2, &split, err, err_cap)) {
         return false;
      }
      if (st) {
         st->partition_symbols_read++;
      }
      partition = split ? AV1_PARTITION_SPLIT : AV1_PARTITION_HORZ;

      if (st && !st->partition_decoded && r == 0 && c == 0 && bsl == sb_bsl_for_stats) {
         st->partition_decoded = true;
         st->partition_forced = false;
         st->partition_bsl = bsl;
         st->partition_ctx = ctx;
         st->partition_symbol = (uint32_t)partition;
      }
   } else {
      // has_rows only => split_or_vert
      if (bsl == 1) {
         snprintf(err, err_cap, "unsupported: split_or_vert with bsl=1");
         return false;
      }
      const uint32_t ctx = partition_ctx_from_mi_grid(mi_grid, mi_rows, mi_cols, r, c, bsl);
      uint16_t *partition_cdf = NULL;
      size_t n = 0;
      if (!select_partition_cdf(cdfs, bsl, ctx, &partition_cdf, &n)) {
         snprintf(err, err_cap, "unsupported partition cdf for split_or_vert (bsl=%u ctx=%u)", bsl, ctx);
         return false;
      }
      (void)n;
      uint16_t bool_cdf[3];
      if (!derive_split_or_vert_cdf(partition_cdf, bsl == 5u, bool_cdf)) {
         snprintf(err, err_cap, "failed to derive split_or_vert cdf");
         return false;
      }
      uint32_t split = 0;
      if (!av1_symbol_read_symbol(sd, bool_cdf, 2, &split, err, err_cap)) {
         return false;
      }
      if (st) {
         st->partition_symbols_read++;
      }
      partition = split ? AV1_PARTITION_SPLIT : AV1_PARTITION_VERT;

      if (st && !st->partition_decoded && r == 0 && c == 0 && bsl == sb_bsl_for_stats) {
         st->partition_decoded = true;
         st->partition_forced = false;
         st->partition_bsl = bsl;
         st->partition_ctx = ctx;
         st->partition_symbol = (uint32_t)partition;
      }
   }

   // For now, we traverse the tree and run a tiny decode_block() stub on the first
   // leaf block (decoding skip + y_mode) to keep bit alignment.
   switch (partition) {
      case AV1_PARTITION_NONE:
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl, bsl, st);
         return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl, bsl, st, out_stop, err, err_cap);

      case AV1_PARTITION_HORZ:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the top half.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st);
            return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st, out_stop, err, err_cap);
         }
         // Two leaf blocks: top half then bottom half.
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r + half, c, bsl, bsl - 1u, st);
         return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r + half, c, bsl, bsl - 1u, st, out_stop, err, err_cap);

      case AV1_PARTITION_VERT:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the left half.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st);
            return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st, out_stop, err, err_cap);
         }
         // Two leaf blocks: left half then right half.
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl, st);
         return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl, st, out_stop, err, err_cap);

      case AV1_PARTITION_SPLIT:
         // Recurse into 4 sub-blocks.
         if (!decode_partition_rec(sd,
                                   cdfs,
                                   params,
                                   sb,
                                   skip_cdfs,
                                   coeff_cdfs,
                                   coeff_ctx,
                                   mi_grid,
                                   mi_rows,
                                   mi_cols,
                                   r,
                                   c,
                                   bsl - 1u,
                                   sb_bsl_for_stats,
                                   st,
                                   out_stop,
                                   err,
                                   err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         if (!decode_partition_rec(sd,
                                   cdfs,
                                   params,
                                   sb,
                                   skip_cdfs,
                                   coeff_cdfs,
                                   coeff_ctx,
                                   mi_grid,
                                   mi_rows,
                                   mi_cols,
                                   r,
                                   c + half,
                                   bsl - 1u,
                                   sb_bsl_for_stats,
                                   st,
                                   out_stop,
                                   err,
                                   err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         if (!decode_partition_rec(sd,
                                   cdfs,
                                   params,
                                   sb,
                                   skip_cdfs,
                                   coeff_cdfs,
                                   coeff_ctx,
                                   mi_grid,
                                   mi_rows,
                                   mi_cols,
                                   r + half,
                                   c,
                                   bsl - 1u,
                                   sb_bsl_for_stats,
                                   st,
                                   out_stop,
                                   err,
                                   err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         if (!decode_partition_rec(sd,
                                   cdfs,
                                   params,
                                   sb,
                                   skip_cdfs,
                                   coeff_cdfs,
                                   coeff_ctx,
                                   mi_grid,
                                   mi_rows,
                                   mi_cols,
                                   r + half,
                                   c + half,
                                   bsl - 1u,
                                   sb_bsl_for_stats,
                                   st,
                                   out_stop,
                                   err,
                                   err_cap)) {
            return false;
         }
         return true;

      // Rectangular / 3-way partitions: for now, treat them as leaf placements to keep
      // traversal progress without needing full decode_block() parsing.
      // This is sufficient for ctx derivation consistency in later traversal steps.
      case AV1_PARTITION_HORZ_A:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the top-left quarter.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st);
            return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap);
         }
         // Leaf blocks: top-left quarter, top-right quarter, then bottom half.
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r + half, c, bsl, bsl - 1u, st);
         return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r + half, c, bsl, bsl - 1u, st, out_stop, err, err_cap);

      case AV1_PARTITION_HORZ_B:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the top half.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st);
            return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st, out_stop, err, err_cap);
         }
         // Leaf blocks: top half, then bottom-left quarter, then bottom-right quarter.
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r + half, c, bsl - 1u, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r + half, c, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r + half, c + half, bsl - 1u, bsl - 1u, st);
         return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r + half, c + half, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap);

      case AV1_PARTITION_VERT_A:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the top-left quarter.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st);
            return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap);
         }
         // Leaf blocks: top-left quarter, bottom-left quarter, then right half.
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r + half, c, bsl - 1u, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r + half, c, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl, st);
         return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl, st, out_stop, err, err_cap);

      case AV1_PARTITION_VERT_B:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the left half.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st);
            return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st, out_stop, err, err_cap);
         }
         // Leaf blocks: left half, then top-right quarter, then bottom-right quarter.
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c, bsl - 1u, bsl, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl - 1u, st);
         if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, c + half, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap)) {
            return false;
         }
         if (out_stop && *out_stop) {
            return true;
         }
         mi_fill_block(mi_grid, mi_rows, mi_cols, r + half, c + half, bsl - 1u, bsl - 1u, st);
         return decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r + half, c + half, bsl - 1u, bsl - 1u, st, out_stop, err, err_cap);

      case AV1_PARTITION_HORZ_4:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the first stripe.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r + 0u * quarter, c, bsl, bsl - 2u, st);
            return decode_block_stub(sd,
                                    params,
                                    sb,
                                    skip_cdfs,
                                    coeff_cdfs,
                                    coeff_ctx,
                                     mi_grid,
                                     mi_rows,
                                     mi_cols,
                                     r + 0u * quarter,
                                     c,
                                     bsl,
                                     bsl - 2u,
                                     st,
                                     out_stop,
                                     err,
                                     err_cap);
         }
         if (bsl < 2u) {
            snprintf(err, err_cap, "invalid HORZ_4 bsl=%u", bsl);
            return false;
         }
         // Four leaf stripes in raster order.
         for (uint32_t i = 0; i < 4u; i++) {
            const uint32_t rr = r + i * quarter;
            mi_fill_block(mi_grid, mi_rows, mi_cols, rr, c, bsl, bsl - 2u, st);
            if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, rr, c, bsl, bsl - 2u, st, out_stop, err, err_cap)) {
               return false;
            }
            if (out_stop && *out_stop) {
               return true;
            }
         }
         return true;

      case AV1_PARTITION_VERT_4:
         if (!params->probe_try_exit_symbol) {
            // First decode_block would be the first stripe.
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, c + 0u * quarter, bsl - 2u, bsl, st);
            return decode_block_stub(sd,
                                    params,
                                    sb,
                                    skip_cdfs,
                                    coeff_cdfs,
                                    coeff_ctx,
                                     mi_grid,
                                     mi_rows,
                                     mi_cols,
                                     r,
                                     c + 0u * quarter,
                                     bsl - 2u,
                                     bsl,
                                     st,
                                     out_stop,
                                     err,
                                     err_cap);
         }
         if (bsl < 2u) {
            snprintf(err, err_cap, "invalid VERT_4 bsl=%u", bsl);
            return false;
         }
         // Four leaf stripes in raster order.
         for (uint32_t i = 0; i < 4u; i++) {
            const uint32_t cc = c + i * quarter;
            mi_fill_block(mi_grid, mi_rows, mi_cols, r, cc, bsl - 2u, bsl, st);
            if (!decode_block_stub(sd, params, sb, skip_cdfs, coeff_cdfs, coeff_ctx, mi_grid, mi_rows, mi_cols, r, cc, bsl - 2u, bsl, st, out_stop, err, err_cap)) {
               return false;
            }
            if (out_stop && *out_stop) {
               return true;
            }
         }
         return true;

      default:
         snprintf(err, err_cap, "unsupported partition=%u", (uint32_t)partition);
         return false;
   }
}

Av1TileSyntaxProbeStatus av1_tile_syntax_probe(const uint8_t *tile_data,
                                    size_t tile_size_bytes,
                                    const Av1TileDecodeParams *params,
                                    uint32_t probe_bools,
                                    Av1TileSyntaxProbeStats *out_stats,
                                    char *err,
                                    size_t err_cap) {
   if (out_stats) {
      memset(out_stats, 0, sizeof(*out_stats));
      out_stats->bools_requested = probe_bools;
   }

   if (!tile_data || tile_size_bytes == 0) {
      snprintf(err, err_cap, "invalid tile payload");
      return AV1_TILE_SYNTAX_PROBE_ERROR;
   }
   if (!params) {
      snprintf(err, err_cap, "missing tile decode params");
      return AV1_TILE_SYNTAX_PROBE_ERROR;
   }
   if (params->mi_col_end < params->mi_col_start || params->mi_row_end < params->mi_row_start) {
      snprintf(err, err_cap, "invalid tile MI bounds");
      return AV1_TILE_SYNTAX_PROBE_ERROR;
   }

   const uint32_t tile_mi_cols = params->mi_col_end - params->mi_col_start;
   const uint32_t tile_mi_rows = params->mi_row_end - params->mi_row_start;
   const uint32_t sb_mi_size = params->use_128x128_superblock ? 32u : 16u;
   const uint32_t sb_cols = tile_mi_cols ? u32_ceil_div(tile_mi_cols, sb_mi_size) : 0;
   const uint32_t sb_rows = tile_mi_rows ? u32_ceil_div(tile_mi_rows, sb_mi_size) : 0;

   if (out_stats) {
      out_stats->tile_mi_cols = tile_mi_cols;
      out_stats->tile_mi_rows = tile_mi_rows;
      out_stats->sb_mi_size = sb_mi_size;
      out_stats->sb_cols = sb_cols;
      out_stats->sb_rows = sb_rows;
   }

   Av1SymbolDecoder sd;
   if (!av1_symbol_init(&sd, tile_data, tile_size_bytes, params->disable_cdf_update != 0, err, err_cap)) {
      return AV1_TILE_SYNTAX_PROBE_ERROR;
   }

   for (uint32_t i = 0; i < probe_bools; i++) {
      uint32_t b;
      if (!av1_symbol_read_bool(&sd, &b, err, err_cap)) {
         if (out_stats) {
            out_stats->bools_read = i;
         }
         return AV1_TILE_SYNTAX_PROBE_ERROR;
      }
      if (out_stats) {
         out_stats->bools_read = i + 1;
      }
   }

   // m3b.D: Walk the partition tree and run a tiny decode_block() stub on the first
   // leaf block (decoding mode_info + tx_size + a small prefix of coeffs()) to keep bit alignment.
   uint32_t sb_bsl = 0;
   if (sb_mi_size == 16u) {
      sb_bsl = 4u;
   } else if (sb_mi_size == 32u) {
      sb_bsl = 5u;
   } else {
      snprintf(err, err_cap, "unsupported sb_mi_size=%u", sb_mi_size);
      return AV1_TILE_SYNTAX_PROBE_ERROR;
   }

   const size_t mi_grid_count = (size_t)tile_mi_cols * (size_t)tile_mi_rows;
   Av1MiSize *mi_grid = NULL;
   if (mi_grid_count > 0) {
      mi_grid = (Av1MiSize *)calloc(mi_grid_count, sizeof(Av1MiSize));
      if (!mi_grid) {
         snprintf(err, err_cap, "out of memory allocating MI grid (%zu entries)", mi_grid_count);
         return AV1_TILE_SYNTAX_PROBE_ERROR;
      }
   }

   Av1TilePartitionCdfs cdfs;
   tile_partition_cdfs_init(&cdfs);

   Av1TileSkipCdfs skip_cdfs;
   tile_skip_cdfs_init(&skip_cdfs);
   skip_cdfs.current_qindex = params->base_q_idx;
   memset(&skip_cdfs.delta_lf_state[0], 0, sizeof(skip_cdfs.delta_lf_state));

   Av1TileCoeffCdfs coeff_cdfs;
   tile_coeff_cdfs_init(&coeff_cdfs, params->base_q_idx);

   Av1TileCoeffCtx coeff_ctx;
   if (!tile_coeff_ctx_init(&coeff_ctx,
                            tile_mi_cols,
                            tile_mi_rows,
                            params->mono_chrome,
                            params->subsampling_x,
                            params->subsampling_y,
                            err,
                            err_cap)) {
      free(mi_grid);
      return AV1_TILE_SYNTAX_PROBE_ERROR;
   }

   bool stop = false;
   if (mi_grid_count > 0) {
      if (!params->probe_try_exit_symbol) {
         // Default probe mode: only traverse starting at the first superblock.
         if (!decode_partition_rec(&sd,
                                   &cdfs,
                                   params,
                                   NULL,
                                   &skip_cdfs,
                                   &coeff_cdfs,
                                   &coeff_ctx,
                                   mi_grid,
                                   tile_mi_rows,
                                   tile_mi_cols,
                                   0,
                                   0,
                                   sb_bsl,
                                   sb_bsl,
                                   out_stats,
                                   &stop,
                                   err,
                                   err_cap)) {
               tile_coeff_ctx_free(&coeff_ctx);
               free(mi_grid);
            return AV1_TILE_SYNTAX_PROBE_ERROR;
         }
      } else {
         // try-EOT mode: traverse all superblocks in raster order.
         for (uint32_t sb_r = 0; sb_r < sb_rows; sb_r++) {
            for (uint32_t sb_c = 0; sb_c < sb_cols; sb_c++) {
               const uint32_t r0 = sb_r * sb_mi_size;
               const uint32_t c0 = sb_c * sb_mi_size;
               
               Av1TileSbProbeState sb;
               memset(&sb, 0, sizeof(sb));
               sb.sb_origin_r = r0;
               sb.sb_origin_c = c0;
               sb.sb_mi_size = sb_mi_size;
               sb.read_deltas = params->delta_q_present;
               
               if (!decode_partition_rec(&sd,
                                         &cdfs,
                                         params,
                                         &sb,
                                         &skip_cdfs,
                                         &coeff_cdfs,
                                         &coeff_ctx,
                                         mi_grid,
                                         tile_mi_rows,
                                         tile_mi_cols,
                                         r0,
                                         c0,
                                         sb_bsl,
                                         sb_bsl,
                                         out_stats,
                                         &stop,
                                         err,
                                         err_cap)) {
                  tile_coeff_ctx_free(&coeff_ctx);
                  free(mi_grid);
                  return AV1_TILE_SYNTAX_PROBE_ERROR;
               }
               if (stop) {
                  break;
               }
            }
            if (stop) {
               break;
            }
         }
      }
   }

         tile_coeff_ctx_free(&coeff_ctx);
   free(mi_grid);

   if (params->probe_try_exit_symbol) {
      // In try-EOT mode, attempt to validate that we reached the end of the coded tile by
      // running exit_symbol(). This will typically fail until full tile syntax decode exists,
      // but is still useful as a correctness check.
      if (stop) {
         // stop==true is only used for early-exit cases like palette (unsupported) at the moment.
         if (err && err_cap > 0 && err[0] == 0) {
            snprintf(err, err_cap, "unsupported: probe stopped before end-of-tile");
         }
         return AV1_TILE_SYNTAX_PROBE_UNSUPPORTED;
      }
      const uint64_t pre_bitpos = sd.br.bitpos;
      const int32_t pre_smb = sd.symbol_max_bits;
      const uint64_t total_bits = (uint64_t)sd.br.size * 8u;

      if (!av1_symbol_exit(&sd, err, err_cap)) {
         if (err && err_cap > 0) {
            char tmp[256];
            tmp[0] = 0;
            if (err[0]) {
               // Preserve the original error message (best-effort).
               snprintf(tmp, sizeof(tmp), "%s", err);
            }
            snprintf(err,
                     err_cap,
                     "exit_symbol failed: %s (bitpos=%llu/%llu smb=%d)",
                     tmp[0] ? tmp : "(unknown)",
                     (unsigned long long)pre_bitpos,
                     (unsigned long long)total_bits,
                     (int)pre_smb);
         }
         return AV1_TILE_SYNTAX_PROBE_ERROR;
      }
      if (err && err_cap > 0) {
         err[0] = 0;
      }
      return AV1_TILE_SYNTAX_PROBE_OK;
   }

   if (out_stats && ((out_stats->block0_has_palette_y_decoded && out_stats->block0_has_palette_y) ||
                     (out_stats->block0_has_palette_uv_decoded && out_stats->block0_has_palette_uv))) {
      snprintf(err,
               err_cap,
               "unsupported: palette used (Y=%s%u size=%s%u, UV=%s%u size=%s%u); stopped=%s",
               out_stats->block0_has_palette_y_decoded ? "" : "n/a ",
               out_stats->block0_has_palette_y_decoded ? out_stats->block0_has_palette_y : 0u,
               out_stats->block0_palette_size_y_decoded ? "" : "n/a ",
               out_stats->block0_palette_size_y_decoded ? out_stats->block0_palette_size_y : 0u,
               out_stats->block0_has_palette_uv_decoded ? "" : "n/a ",
               out_stats->block0_has_palette_uv_decoded ? out_stats->block0_has_palette_uv : 0u,
               out_stats->block0_palette_size_uv_decoded ? "" : "n/a ",
               out_stats->block0_palette_size_uv_decoded ? out_stats->block0_palette_size_uv : 0u,
               stop ? "yes" : "no");
   } else {
      snprintf(err,
               err_cap,
               "unsupported: decode_block() stub decoded skip+y_mode+uv_mode (+ optional angle_delta/filter_intra/palette prelude) + tx_depth when applicable + coeffs() prefix (txb_skip,eob_pt,eob,coeff_base_eob[,coeff_base]); stopped=%s",
               stop ? "yes" : "no");
   }
   return AV1_TILE_SYNTAX_PROBE_UNSUPPORTED;
}
