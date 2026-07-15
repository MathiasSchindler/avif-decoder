#include "av1_coeff.h"
#include "base.h"

#define COEFF_CDF_Q_CTXS AV1_COEFF_Q_CONTEXTS
#define TX_SIZES AV1_TX_SIZE_CONTEXTS
#define PLANE_TYPES AV1_PLANE_TYPES
#define TXB_SKIP_CONTEXTS AV1_TXB_SKIP_CONTEXTS
#define EOB_COEF_CONTEXTS AV1_EOB_COEF_CONTEXTS
#define DC_SIGN_CONTEXTS AV1_DC_SIGN_CONTEXTS
#define SIG_COEF_CONTEXTS_EOB AV1_SIG_COEF_CONTEXTS_EOB
#define SIG_COEF_CONTEXTS AV1_SIG_COEF_CONTEXTS
#define LEVEL_CONTEXTS AV1_LEVEL_CONTEXTS
#define BR_CDF_SIZE AV1_BR_CDF_SIZE
#include "av1_coeff_defaults.inc"
#undef COEFF_CDF_Q_CTXS
#undef TX_SIZES
#undef PLANE_TYPES
#undef TXB_SKIP_CONTEXTS
#undef EOB_COEF_CONTEXTS
#undef DC_SIGN_CONTEXTS
#undef SIG_COEF_CONTEXTS_EOB
#undef SIG_COEF_CONTEXTS
#undef LEVEL_CONTEXTS
#undef BR_CDF_SIZE

#include "av1_coeff_tables.inc"

enum {
    AV1_NUM_BASE_LEVELS = 2,
    AV1_COEFF_BASE_RANGE = 12,
    AV1_SIG_REF_DIFF_OFFSET_NUM = 5,
    AV1_MAX_SEG_EOB = 1024,
    AV1_MAX_GOLOMB_LENGTH = 20
};

typedef enum {
    AV1_TX_CLASS_2D = 0,
    AV1_TX_CLASS_HORIZ,
    AV1_TX_CLASS_VERT
} Av1TxClass;

typedef struct {
    const uint16_t *values;
    size_t count;
} Av1CoeffScan;

const Av1TxSizeInfo av1_tx_size_info[AV1_TX_SIZES_ALL] = {
    { 4U, 4U, 2U, 2U }, { 8U, 8U, 3U, 3U },
    { 16U, 16U, 4U, 4U }, { 32U, 32U, 5U, 5U },
    { 64U, 64U, 6U, 6U }, { 4U, 8U, 2U, 3U },
    { 8U, 4U, 3U, 2U }, { 8U, 16U, 3U, 4U },
    { 16U, 8U, 4U, 3U }, { 16U, 32U, 4U, 5U },
    { 32U, 16U, 5U, 4U }, { 32U, 64U, 5U, 6U },
    { 64U, 32U, 6U, 5U }, { 4U, 16U, 2U, 4U },
    { 16U, 4U, 4U, 2U }, { 8U, 32U, 3U, 5U },
    { 32U, 8U, 5U, 3U }, { 16U, 64U, 4U, 6U },
    { 64U, 16U, 6U, 4U }
};

static const uint8_t av1_tx_size_sqr[AV1_TX_SIZES_ALL] = {
    AV1_TX_4X4, AV1_TX_8X8, AV1_TX_16X16, AV1_TX_32X32, AV1_TX_64X64,
    AV1_TX_4X4, AV1_TX_4X4, AV1_TX_8X8, AV1_TX_8X8, AV1_TX_16X16,
    AV1_TX_16X16, AV1_TX_32X32, AV1_TX_32X32, AV1_TX_4X4, AV1_TX_4X4,
    AV1_TX_8X8, AV1_TX_8X8, AV1_TX_16X16, AV1_TX_16X16
};

static const uint8_t av1_tx_size_sqr_up[AV1_TX_SIZES_ALL] = {
    AV1_TX_4X4, AV1_TX_8X8, AV1_TX_16X16, AV1_TX_32X32, AV1_TX_64X64,
    AV1_TX_8X8, AV1_TX_8X8, AV1_TX_16X16, AV1_TX_16X16, AV1_TX_32X32,
    AV1_TX_32X32, AV1_TX_64X64, AV1_TX_64X64, AV1_TX_16X16,
    AV1_TX_16X16, AV1_TX_32X32, AV1_TX_32X32, AV1_TX_64X64,
    AV1_TX_64X64
};

static Av1TxClass av1_coeff_tx_class(Av1TxType tx_type) {
    if (tx_type == AV1_TX_V_DCT || tx_type == AV1_TX_V_ADST ||
        tx_type == AV1_TX_V_FLIPADST) {
        return AV1_TX_CLASS_VERT;
    }
    if (tx_type == AV1_TX_H_DCT || tx_type == AV1_TX_H_ADST ||
        tx_type == AV1_TX_H_FLIPADST) {
        return AV1_TX_CLASS_HORIZ;
    }
    return AV1_TX_CLASS_2D;
}

#define AV1_SCAN(array) (Av1CoeffScan){ array, sizeof(array) / sizeof((array)[0]) }

static Av1CoeffScan av1_coeff_default_scan(Av1TxSize tx_size) {
    switch (tx_size) {
        case AV1_TX_4X4: return AV1_SCAN(av1_default_scan_4x4);
        case AV1_TX_4X8: return AV1_SCAN(av1_default_scan_4x8);
        case AV1_TX_8X4: return AV1_SCAN(av1_default_scan_8x4);
        case AV1_TX_8X8: return AV1_SCAN(av1_default_scan_8x8);
        case AV1_TX_8X16: return AV1_SCAN(av1_default_scan_8x16);
        case AV1_TX_16X8: return AV1_SCAN(av1_default_scan_16x8);
        case AV1_TX_16X16: return AV1_SCAN(av1_default_scan_16x16);
        case AV1_TX_16X32: return AV1_SCAN(av1_default_scan_16x32);
        case AV1_TX_32X16: return AV1_SCAN(av1_default_scan_32x16);
        case AV1_TX_4X16: return AV1_SCAN(av1_default_scan_4x16);
        case AV1_TX_16X4: return AV1_SCAN(av1_default_scan_16x4);
        case AV1_TX_8X32: return AV1_SCAN(av1_default_scan_8x32);
        case AV1_TX_32X8: return AV1_SCAN(av1_default_scan_32x8);
        default: return AV1_SCAN(av1_default_scan_32x32);
    }
}

static Av1CoeffScan av1_coeff_mrow_scan(Av1TxSize tx_size) {
    switch (tx_size) {
        case AV1_TX_4X4: return AV1_SCAN(av1_mrow_scan_4x4);
        case AV1_TX_4X8: return AV1_SCAN(av1_mrow_scan_4x8);
        case AV1_TX_8X4: return AV1_SCAN(av1_mrow_scan_8x4);
        case AV1_TX_8X8: return AV1_SCAN(av1_mrow_scan_8x8);
        case AV1_TX_8X16: return AV1_SCAN(av1_mrow_scan_8x16);
        case AV1_TX_16X8: return AV1_SCAN(av1_mrow_scan_16x8);
        case AV1_TX_16X16: return AV1_SCAN(av1_mrow_scan_16x16);
        case AV1_TX_4X16: return AV1_SCAN(av1_mrow_scan_4x16);
        default: return AV1_SCAN(av1_mrow_scan_16x4);
    }
}

static Av1CoeffScan av1_coeff_mcol_scan(Av1TxSize tx_size) {
    switch (tx_size) {
        case AV1_TX_4X4: return AV1_SCAN(av1_mcol_scan_4x4);
        case AV1_TX_4X8: return AV1_SCAN(av1_mcol_scan_4x8);
        case AV1_TX_8X4: return AV1_SCAN(av1_mcol_scan_8x4);
        case AV1_TX_8X8: return AV1_SCAN(av1_mcol_scan_8x8);
        case AV1_TX_8X16: return AV1_SCAN(av1_mcol_scan_8x16);
        case AV1_TX_16X8: return AV1_SCAN(av1_mcol_scan_16x8);
        case AV1_TX_16X16: return AV1_SCAN(av1_mcol_scan_16x16);
        case AV1_TX_4X16: return AV1_SCAN(av1_mcol_scan_4x16);
        default: return AV1_SCAN(av1_mcol_scan_16x4);
    }
}

static Av1CoeffScan av1_coeff_get_scan(Av1TxSize tx_size,
                                       Av1TxType tx_type) {
    if (tx_size == AV1_TX_16X64) return AV1_SCAN(av1_default_scan_16x32);
    if (tx_size == AV1_TX_64X16) return AV1_SCAN(av1_default_scan_32x16);
    if (av1_tx_size_sqr_up[tx_size] == AV1_TX_64X64) {
        return AV1_SCAN(av1_default_scan_32x32);
    }
    if (tx_type == AV1_TX_IDTX) return av1_coeff_default_scan(tx_size);
    if (av1_coeff_tx_class(tx_type) == AV1_TX_CLASS_VERT) {
        return av1_coeff_mrow_scan(tx_size);
    }
    if (av1_coeff_tx_class(tx_type) == AV1_TX_CLASS_HORIZ) {
        return av1_coeff_mcol_scan(tx_size);
    }
    return av1_coeff_default_scan(tx_size);
}

#undef AV1_SCAN

static unsigned int av1_coeff_txb_skip_context(
    const Av1CoeffPlaneContext *context,
    unsigned int plane,
    size_t x4,
    size_t y4,
    size_t w4,
    size_t h4,
    size_t block_width,
    size_t block_height,
    const Av1TxSizeInfo *tx) {
    size_t index;

    if (plane == 0U) {
        unsigned int top = 0U;
        unsigned int left = 0U;
        unsigned int maximum;
        unsigned int minimum;

        for (index = 0U; index < w4; ++index) {
            if (context->above_level_context[x4 + index] > top) {
                top = context->above_level_context[x4 + index];
            }
        }
        for (index = 0U; index < h4; ++index) {
            if (context->left_level_context[y4 + index] > left) {
                left = context->left_level_context[y4 + index];
            }
        }
        if (block_width == tx->width && block_height == tx->height) return 0U;
        if (top == 0U && left == 0U) return 1U;
        maximum = top > left ? top : left;
        minimum = top < left ? top : left;
        if (top == 0U || left == 0U) return 2U + (maximum > 3U);
        if (maximum <= 3U) return 4U;
        if (minimum <= 3U) return 5U;
        return 6U;
    } else {
        unsigned int above = 0U;
        unsigned int left = 0U;
        unsigned int result;

        for (index = 0U; index < w4; ++index) {
            above |= context->above_level_context[x4 + index];
            above |= context->above_dc_context[x4 + index];
        }
        for (index = 0U; index < h4; ++index) {
            left |= context->left_level_context[y4 + index];
            left |= context->left_dc_context[y4 + index];
        }
        result = 7U + (above != 0U) + (left != 0U);
        if (block_width * block_height > (size_t)tx->width * tx->height) {
            result += 3U;
        }
        return result;
    }
}

static unsigned int av1_coeff_dc_sign_context(
    const Av1CoeffPlaneContext *context,
    size_t x4,
    size_t y4,
    size_t w4,
    size_t h4) {
    int dc_sign = 0;
    size_t index;

    for (index = 0U; index < w4; ++index) {
        uint8_t sign = context->above_dc_context[x4 + index];
        if (sign == 1U) --dc_sign;
        else if (sign == 2U) ++dc_sign;
    }
    for (index = 0U; index < h4; ++index) {
        uint8_t sign = context->left_dc_context[y4 + index];
        if (sign == 1U) --dc_sign;
        else if (sign == 2U) ++dc_sign;
    }
    return dc_sign < 0 ? 1U : dc_sign > 0 ? 2U : 0U;
}

static unsigned int av1_coeff_base_context(Av1TxSize tx_size,
                                           Av1TxType tx_type,
                                           const uint32_t levels[AV1_MAX_SEG_EOB],
                                           size_t pos,
                                           size_t scan_index,
                                           int is_eob) {
    Av1TxSize adjusted = (Av1TxSize)av1_adjusted_tx_size[tx_size];
    const Av1TxSizeInfo *tx = &av1_tx_size_info[adjusted];
    Av1TxClass tx_class = av1_coeff_tx_class(tx_type);
    size_t row;
    size_t column;
    unsigned int magnitude = 0U;
    unsigned int context;
    size_t index;

    if (is_eob) {
        size_t area = (size_t)tx->width * tx->height;
        if (scan_index == 0U) return AV1_SIG_COEF_CONTEXTS - 4U;
        if (scan_index <= area / 8U) return AV1_SIG_COEF_CONTEXTS - 3U;
        if (scan_index <= area / 4U) return AV1_SIG_COEF_CONTEXTS - 2U;
        return AV1_SIG_COEF_CONTEXTS - 1U;
    }
    row = pos >> tx->width_log2;
    column = pos - (row << tx->width_log2);
    for (index = 0U; index < AV1_SIG_REF_DIFF_OFFSET_NUM; ++index) {
        size_t ref_row = row + av1_sig_ref_diff_offset[tx_class][index][0];
        size_t ref_column = column + av1_sig_ref_diff_offset[tx_class][index][1];
        if (ref_row < tx->height && ref_column < tx->width) {
            uint32_t level = levels[(ref_row << tx->width_log2) + ref_column];
            magnitude += level < 3U ? level : 3U;
        }
    }
    context = (magnitude + 1U) >> 1;
    if (context > 4U) context = 4U;
    if (tx_class == AV1_TX_CLASS_2D) {
        size_t row_context = row < 4U ? row : 4U;
        size_t column_context = column < 4U ? column : 4U;
        if (row == 0U && column == 0U) return 0U;
        return context + av1_coeff_base_ctx_offset[tx_size]
                                                   [row_context]
                                                   [column_context];
    }
    index = tx_class == AV1_TX_CLASS_VERT ? row : column;
    if (index > 2U) index = 2U;
    return context + av1_coeff_base_pos_ctx_offset[index];
}

static unsigned int av1_coeff_br_context(Av1TxSize tx_size,
                                         Av1TxType tx_type,
                                         const uint32_t levels[AV1_MAX_SEG_EOB],
                                         size_t pos) {
    Av1TxSize adjusted = (Av1TxSize)av1_adjusted_tx_size[tx_size];
    const Av1TxSizeInfo *tx = &av1_tx_size_info[adjusted];
    Av1TxClass tx_class = av1_coeff_tx_class(tx_type);
    size_t row = pos >> tx->width_log2;
    size_t column = pos - (row << tx->width_log2);
    unsigned int magnitude = 0U;
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        size_t ref_row = row + av1_mag_ref_offset[tx_class][index][0];
        size_t ref_column = column + av1_mag_ref_offset[tx_class][index][1];
        if (ref_row < tx->height && ref_column < tx->width) {
            uint32_t level = levels[(ref_row << tx->width_log2) + ref_column];
            magnitude += level < 15U ? level : 15U;
        }
    }
    magnitude = (magnitude + 1U) >> 1;
    if (magnitude > 6U) magnitude = 6U;
    if (pos == 0U) return magnitude;
    if (tx_class == AV1_TX_CLASS_2D) {
        return magnitude + (row < 2U && column < 2U ? 7U : 14U);
    }
    if (tx_class == AV1_TX_CLASS_HORIZ) {
        return magnitude + (column == 0U ? 7U : 14U);
    }
    return magnitude + (row == 0U ? 7U : 14U);
}

static void av1_coeff_store_context(Av1CoeffPlaneContext *context,
                                    size_t x4,
                                    size_t y4,
                                    size_t w4,
                                    size_t h4,
                                    uint8_t cul_level,
                                    uint8_t dc_category) {
    size_t index;

    for (index = 0U; index < w4; ++index) {
        context->above_level_context[x4 + index] = cul_level;
        context->above_dc_context[x4 + index] = dc_category;
    }
    for (index = 0U; index < h4; ++index) {
        context->left_level_context[y4 + index] = cul_level;
        context->left_dc_context[y4 + index] = dc_category;
    }
}

unsigned int av1_coeff_q_context(uint8_t base_q_index) {
    if (base_q_index <= 20U) return 0U;
    if (base_q_index <= 60U) return 1U;
    if (base_q_index <= 120U) return 2U;
    return 3U;
}

void av1_coeff_cdfs_init(Av1CoeffCdfs *cdfs, uint8_t base_q_index) {
    unsigned int context;

    if (cdfs == 0) return;
    context = av1_coeff_q_context(base_q_index);
    avifdec_memory_copy(cdfs->txb_skip, av1_default_txb_skip_cdf[context],
                        sizeof(cdfs->txb_skip));
    avifdec_memory_copy(cdfs->eob_pt_16, av1_default_eob_pt_16_cdf[context],
                        sizeof(cdfs->eob_pt_16));
    avifdec_memory_copy(cdfs->eob_pt_32, av1_default_eob_pt_32_cdf[context],
                        sizeof(cdfs->eob_pt_32));
    avifdec_memory_copy(cdfs->eob_pt_64, av1_default_eob_pt_64_cdf[context],
                        sizeof(cdfs->eob_pt_64));
    avifdec_memory_copy(cdfs->eob_pt_128, av1_default_eob_pt_128_cdf[context],
                        sizeof(cdfs->eob_pt_128));
    avifdec_memory_copy(cdfs->eob_pt_256, av1_default_eob_pt_256_cdf[context],
                        sizeof(cdfs->eob_pt_256));
    avifdec_memory_copy(cdfs->eob_pt_512, av1_default_eob_pt_512_cdf[context],
                        sizeof(cdfs->eob_pt_512));
    avifdec_memory_copy(cdfs->eob_pt_1024, av1_default_eob_pt_1024_cdf[context],
                        sizeof(cdfs->eob_pt_1024));
    avifdec_memory_copy(cdfs->eob_extra, av1_default_eob_extra_cdf[context],
                        sizeof(cdfs->eob_extra));
    avifdec_memory_copy(cdfs->dc_sign, av1_default_dc_sign_cdf[context],
                        sizeof(cdfs->dc_sign));
    avifdec_memory_copy(cdfs->coeff_base_eob,
                        av1_default_coeff_base_eob_cdf[context],
                        sizeof(cdfs->coeff_base_eob));
    avifdec_memory_copy(cdfs->coeff_base, av1_default_coeff_base_cdf[context],
                        sizeof(cdfs->coeff_base));
    avifdec_memory_copy(cdfs->coeff_br, av1_default_coeff_br_cdf[context],
                        sizeof(cdfs->coeff_br));
}

uint64_t av1_coeff_cdfs_checksum(const Av1CoeffCdfs *cdfs) {
    const uint8_t *bytes = (const uint8_t *)cdfs;
    uint64_t checksum = (uint64_t)1469598103934665603ULL;
    size_t index;

    if (cdfs == 0) return 0U;
    for (index = 0U; index < sizeof(*cdfs); ++index) {
        checksum ^= bytes[index];
        checksum *= (uint64_t)1099511628211ULL;
    }
    return checksum;
}

AvifdecStatus av1_coeff_context_init(Av1CoeffContextState *state,
                                     const Av1CoeffPlaneContext plane[3]) {
    unsigned int index;

    if (state == 0 || plane == 0) return AVIFDEC_INVALID_ARGUMENT;
    for (index = 0U; index < 3U; ++index) {
        if ((plane[index].width4 != 0U || plane[index].height4 != 0U) &&
            (plane[index].above_level_context == 0 ||
             plane[index].left_level_context == 0 ||
             plane[index].above_dc_context == 0 ||
             plane[index].left_dc_context == 0 ||
             plane[index].above_capacity < plane[index].width4 ||
             plane[index].left_capacity < plane[index].height4)) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
    }
    for (index = 0U; index < 3U; ++index) {
        state->plane[index] = plane[index];
        if (plane[index].width4 != 0U) {
            avifdec_memory_fill(state->plane[index].above_level_context, 0U,
                                plane[index].width4);
            avifdec_memory_fill(state->plane[index].above_dc_context, 0U,
                                plane[index].width4);
        }
        if (plane[index].height4 != 0U) {
            avifdec_memory_fill(state->plane[index].left_level_context, 0U,
                                plane[index].height4);
            avifdec_memory_fill(state->plane[index].left_dc_context, 0U,
                                plane[index].height4);
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_coeff_parse_block_select(
    Av1SymbolDecoder *decoder,
    Av1CoeffCdfs *cdfs,
    Av1CoeffContextState *contexts,
    unsigned int plane,
    Av1TxSize tx_size,
    Av1TxType tx_type,
    size_t plane_block_width,
    size_t plane_block_height,
    size_t x4,
    size_t y4,
    Av1CoeffSelectTxType select_tx_type,
    void *select_user_data,
    int32_t *coefficients,
    size_t coefficient_capacity,
    Av1CoeffBlockResult *result) {
    uint32_t levels[AV1_MAX_SEG_EOB];
    const Av1TxSizeInfo *tx;
    Av1CoeffPlaneContext *context;
    Av1CoeffScan scan;
    size_t segment_eob;
    size_t w4;
    size_t h4;
    unsigned int tx_size_context;
    unsigned int plane_type;
    unsigned int skip_context;
    unsigned int all_zero;
    uint32_t eob_point;
    uint32_t eob;
    uint32_t cul_level = 0U;
    uint8_t dc_category = 0U;
    size_t coefficient;

    if (decoder == 0 || cdfs == 0 || contexts == 0 || result == 0 ||
        plane >= 3U || tx_size < AV1_TX_4X4 || tx_size >= AV1_TX_SIZES_ALL ||
        tx_type < AV1_TX_DCT_DCT || tx_type >= AV1_TX_TYPES) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    tx = &av1_tx_size_info[tx_size];
    context = &contexts->plane[plane];
    w4 = tx->width >> 2;
    h4 = tx->height >> 2;
    if (context->above_level_context == 0 || context->left_level_context == 0 ||
        context->above_dc_context == 0 || context->left_dc_context == 0 ||
        context->above_capacity < context->width4 ||
        context->left_capacity < context->height4 ||
        x4 > context->width4 || w4 > context->width4 - x4 ||
        y4 > context->height4 || h4 > context->height4 - y4 ||
        plane_block_width < tx->width || plane_block_height < tx->height ||
        plane_block_width > 128U || plane_block_height > 128U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    segment_eob = tx_size == AV1_TX_16X64 || tx_size == AV1_TX_64X16
                  ? 512U : (size_t)tx->width * tx->height;
    if (segment_eob > AV1_MAX_SEG_EOB) segment_eob = AV1_MAX_SEG_EOB;
    if (coefficients != 0 && coefficient_capacity < segment_eob) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (coefficients != 0) {
        avifdec_memory_fill(coefficients, 0U,
                            segment_eob * sizeof(*coefficients));
    }
    result->eob = 0U;
    result->cul_level = 0U;
    result->dc_category = 0U;
    avifdec_memory_fill(levels, 0U, sizeof(levels));
    tx_size_context = (av1_tx_size_sqr[tx_size] +
                       av1_tx_size_sqr_up[tx_size] + 1U) >> 1;
    plane_type = plane > 0U;
    skip_context = av1_coeff_txb_skip_context(context, plane, x4, y4, w4, h4,
                                               plane_block_width,
                                               plane_block_height, tx);
    all_zero = av1_symbol_read(decoder,
                               cdfs->txb_skip[tx_size_context][skip_context], 2U);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    if (all_zero != 0U) {
        av1_coeff_store_context(context, x4, y4, w4, h4, 0U, 0U);
        return AVIFDEC_OK;
    }
    if (select_tx_type != 0) {
        AvifdecStatus status = select_tx_type(select_user_data, decoder,
                                              tx_size, &tx_type);
        if (status != AVIFDEC_OK) return status;
        if (tx_type < AV1_TX_DCT_DCT || tx_type >= AV1_TX_TYPES) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    scan = av1_coeff_get_scan(tx_size, tx_type);
    if (segment_eob == 0U || scan.count < segment_eob) {
        return AVIFDEC_INVALID_ARGUMENT;
    }

    {
        unsigned int eob_multi_size =
            (tx->width_log2 < 5U ? tx->width_log2 : 5U) +
            (tx->height_log2 < 5U ? tx->height_log2 : 5U) - 4U;
        unsigned int eob_context =
            av1_coeff_tx_class(tx_type) == AV1_TX_CLASS_2D ? 0U : 1U;

        switch (eob_multi_size) {
            case 0U:
                eob_point = av1_symbol_read(decoder,
                    cdfs->eob_pt_16[plane_type][eob_context], 5U) + 1U;
                break;
            case 1U:
                eob_point = av1_symbol_read(decoder,
                    cdfs->eob_pt_32[plane_type][eob_context], 6U) + 1U;
                break;
            case 2U:
                eob_point = av1_symbol_read(decoder,
                    cdfs->eob_pt_64[plane_type][eob_context], 7U) + 1U;
                break;
            case 3U:
                eob_point = av1_symbol_read(decoder,
                    cdfs->eob_pt_128[plane_type][eob_context], 8U) + 1U;
                break;
            case 4U:
                eob_point = av1_symbol_read(decoder,
                    cdfs->eob_pt_256[plane_type][eob_context], 9U) + 1U;
                break;
            case 5U:
                eob_point = av1_symbol_read(decoder,
                    cdfs->eob_pt_512[plane_type], 10U) + 1U;
                break;
            default:
                eob_point = av1_symbol_read(decoder,
                    cdfs->eob_pt_1024[plane_type], 11U) + 1U;
                break;
        }
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    eob = eob_point < 2U ? eob_point : (1U << (eob_point - 2U)) + 1U;
    if (eob_point >= 3U) {
        unsigned int high_shift = eob_point - 3U;
        unsigned int extra = av1_symbol_read(
            decoder, cdfs->eob_extra[tx_size_context][plane_type][high_shift], 2U);
        unsigned int index;

        if (extra != 0U) eob += 1U << high_shift;
        for (index = 1U; index < eob_point - 2U; ++index) {
            unsigned int shift = eob_point - 3U - index;
            if (av1_symbol_read_literal(decoder, 1U) != 0U) eob += 1U << shift;
        }
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    if (eob == 0U || eob > segment_eob) {
        decoder->status = AVIFDEC_INVALID_DATA;
        return decoder->status;
    }

    for (coefficient = eob; coefficient-- > 0U;) {
        size_t pos = scan.values[coefficient];
        unsigned int base_context = av1_coeff_base_context(
            tx_size, tx_type, levels, pos, coefficient, coefficient == eob - 1U);
        uint32_t level;

        if (pos >= segment_eob) {
            decoder->status = AVIFDEC_INVALID_DATA;
            return decoder->status;
        }
        if (coefficient == eob - 1U) {
            unsigned int eob_context = base_context - AV1_SIG_COEF_CONTEXTS +
                                       AV1_SIG_COEF_CONTEXTS_EOB;
            level = av1_symbol_read(
                decoder,
                cdfs->coeff_base_eob[tx_size_context][plane_type][eob_context],
                3U) + 1U;
        } else {
            level = av1_symbol_read(
                decoder,
                cdfs->coeff_base[tx_size_context][plane_type][base_context], 4U);
        }
        if (decoder->status != AVIFDEC_OK) return decoder->status;
        if (level > AV1_NUM_BASE_LEVELS) {
            unsigned int range_context = av1_coeff_br_context(
                tx_size, tx_type, levels, pos);
            unsigned int range_tx_context =
                tx_size_context < AV1_TX_32X32 ? tx_size_context : AV1_TX_32X32;
            unsigned int index;

            for (index = 0U;
                 index < AV1_COEFF_BASE_RANGE / (AV1_BR_CDF_SIZE - 1U);
                 ++index) {
                uint32_t increment = av1_symbol_read(
                    decoder,
                    cdfs->coeff_br[range_tx_context][plane_type][range_context],
                    AV1_BR_CDF_SIZE);
                level += increment;
                if (decoder->status != AVIFDEC_OK) return decoder->status;
                if (increment < AV1_BR_CDF_SIZE - 1U) break;
            }
        }
        levels[pos] = level;
    }

    for (coefficient = 0U; coefficient < eob; ++coefficient) {
        size_t pos = scan.values[coefficient];
        uint32_t level = levels[pos];
        unsigned int sign = 0U;

        if (level != 0U) {
            if (coefficient == 0U) {
                unsigned int dc_context = av1_coeff_dc_sign_context(
                    context, x4, y4, w4, h4);
                sign = av1_symbol_read(decoder,
                    cdfs->dc_sign[plane_type][dc_context], 2U);
            } else {
                sign = av1_symbol_read_literal(decoder, 1U);
            }
        }
        if (decoder->status != AVIFDEC_OK) return decoder->status;
        if (level > AV1_NUM_BASE_LEVELS + AV1_COEFF_BASE_RANGE) {
            unsigned int length;
            uint32_t value = 1U;
            unsigned int index;

            for (length = 1U; length <= AV1_MAX_GOLOMB_LENGTH; ++length) {
                uint32_t end = av1_symbol_read_literal(decoder, 1U);
                if (decoder->status != AVIFDEC_OK) return decoder->status;
                if (end != 0U) break;
            }
            if (length > AV1_MAX_GOLOMB_LENGTH) {
                decoder->status = AVIFDEC_INVALID_DATA;
                return decoder->status;
            }
            for (index = 1U; index < length; ++index) {
                value = (value << 1) | av1_symbol_read_literal(decoder, 1U);
                if (decoder->status != AVIFDEC_OK) return decoder->status;
            }
            level = value + AV1_COEFF_BASE_RANGE + AV1_NUM_BASE_LEVELS;
        }
        levels[pos] = level | (sign != 0U ? 0x80000000U : 0U);
        if (pos == 0U && level != 0U) dc_category = sign != 0U ? 1U : 2U;
        level &= 0xfffffU;
        cul_level += level;
    }
    if (cul_level > 63U) cul_level = 63U;
    result->eob = (uint16_t)eob;
    result->cul_level = (uint8_t)cul_level;
    result->dc_category = dc_category;
    if (coefficients != 0) {
        for (coefficient = 0U; coefficient < segment_eob; ++coefficient) {
            uint32_t magnitude = levels[coefficient] & 0x7fffffffU;
            coefficients[coefficient] = (levels[coefficient] & 0x80000000U) != 0U
                                        ? -(int32_t)magnitude
                                        : (int32_t)magnitude;
        }
    }
    av1_coeff_store_context(context, x4, y4, w4, h4,
                            result->cul_level, result->dc_category);
    return AVIFDEC_OK;
}

AvifdecStatus av1_coeff_parse_block(Av1SymbolDecoder *decoder,
                                    Av1CoeffCdfs *cdfs,
                                    Av1CoeffContextState *contexts,
                                    unsigned int plane,
                                    Av1TxSize tx_size,
                                    Av1TxType tx_type,
                                    size_t plane_block_width,
                                    size_t plane_block_height,
                                    size_t x4,
                                    size_t y4,
                                    Av1CoeffBlockResult *result) {
    return av1_coeff_parse_block_select(
        decoder, cdfs, contexts, plane, tx_size, tx_type,
        plane_block_width, plane_block_height, x4, y4, 0, 0, 0, 0U, result);
}

    AvifdecStatus av1_coeff_parse_block_values(Av1SymbolDecoder *decoder,
                           Av1CoeffCdfs *cdfs,
                           Av1CoeffContextState *contexts,
                           unsigned int plane,
                           Av1TxSize tx_size,
                           Av1TxType tx_type,
                           size_t plane_block_width,
                           size_t plane_block_height,
                           size_t x4,
                           size_t y4,
                           int32_t *coefficients,
                           size_t coefficient_capacity,
                           Av1CoeffBlockResult *result) {
        if (coefficients == 0) return AVIFDEC_INVALID_ARGUMENT;
        return av1_coeff_parse_block_select(
        decoder, cdfs, contexts, plane, tx_size, tx_type,
        plane_block_width, plane_block_height, x4, y4, 0, 0,
        coefficients, coefficient_capacity, result);
    }