#include "encoder/av1_transform_write.h"
#include "av1_recon.h"
#include "base.h"

enum {
    TRANSFORM_BASE_LEVELS = 2,
    TRANSFORM_BASE_RANGE = 12,
    TRANSFORM_BR_CDF_SIZE = 4
};

static const uint8_t transform_scan_4x4[16] = {
    0U, 1U, 4U, 8U, 5U, 2U, 3U, 6U,
    9U, 12U, 13U, 10U, 7U, 11U, 14U, 15U
};

static int32_t transform_round_shift(int64_t value, unsigned int bits) {
    int64_t offset = (int64_t)1U << (bits - 1U);
    int64_t adjusted = value + offset;
    int64_t divisor = (int64_t)1U << bits;

    if (adjusted >= 0) return (int32_t)(adjusted / divisor);
    return (int32_t)(-((-adjusted + divisor - 1) / divisor));
}

static void transform_fdct4(const int32_t input[4], int32_t output[4]) {
    int32_t sum0 = input[0] + input[3];
    int32_t sum1 = input[1] + input[2];
    int32_t difference0 = input[1] - input[2];
    int32_t difference1 = input[0] - input[3];

    output[0] = transform_round_shift(
        (int64_t)5793 * sum0 + (int64_t)5793 * sum1, 13U);
    output[2] = transform_round_shift(
        (int64_t)5793 * sum0 - (int64_t)5793 * sum1, 13U);
    output[1] = transform_round_shift(
        (int64_t)3135 * difference0 + (int64_t)7568 * difference1, 13U);
    output[3] = transform_round_shift(
        (int64_t)3135 * difference1 - (int64_t)7568 * difference0, 13U);
}

AvifencStatus avifenc_av1_forward_dct_4x4(const int16_t *input,
                                           size_t stride,
                                           int32_t output[16]) {
    int32_t intermediate[16];
    int32_t values[4];
    int32_t transformed[4];
    size_t row;
    size_t column;

    if (input == 0 || output == 0 || stride < 4U) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    for (column = 0U; column < 4U; ++column) {
        for (row = 0U; row < 4U; ++row) {
            values[row] = (int32_t)input[row * stride + column] * 4;
        }
        transform_fdct4(values, transformed);
        for (row = 0U; row < 4U; ++row) {
            intermediate[row * 4U + column] = transformed[row];
        }
    }
    for (row = 0U; row < 4U; ++row) {
        transform_fdct4(intermediate + row * 4U, transformed);
        for (column = 0U; column < 4U; ++column) {
            output[column * 4U + row] = transformed[column];
        }
    }
    return AVIFENC_OK;
}

static int32_t transform_quantize_value(int32_t value, uint32_t step) {
    uint32_t magnitude = value < 0 ? (uint32_t)(-(int64_t)value)
                                   : (uint32_t)value;
    uint32_t quantized = (magnitude + (step >> 1U)) / step;

    return value < 0 ? -(int32_t)quantized : (int32_t)quantized;
}

AvifencStatus avifenc_av1_quantize_4x4(const int32_t input[16],
                                       uint8_t quantizer,
                                       AvifencAv1TransformBlock *block) {
    uint32_t dc_step;
    uint32_t ac_step;
    size_t index;

    if (input == 0 || block == 0 || quantizer == 0U) {
        return quantizer == 0U ? AVIFENC_UNSUPPORTED
                               : AVIFENC_INVALID_ARGUMENT;
    }
    dc_step = av1_recon_dc_quant(8U, quantizer);
    ac_step = av1_recon_ac_quant(8U, quantizer);
    if (dc_step == 0U || ac_step == 0U) return AVIFENC_LIMIT_EXCEEDED;
    block->eob = 0U;
    for (index = 0U; index < 16U; ++index) {
        block->quantized[index] = transform_quantize_value(
            input[index], index == 0U ? dc_step : ac_step);
    }
    for (index = 0U; index < 16U; ++index) {
        if (block->quantized[transform_scan_4x4[index]] != 0) {
            block->eob = (uint16_t)(index + 1U);
        }
    }
    return AVIFENC_OK;
}

static int transform_size_add(size_t left, size_t right, size_t *result) {
    if (right > (size_t)-1 - left) return 0;
    *result = left + right;
    return 1;
}

AvifencStatus avifenc_av1_transform_context_size(uint32_t mi_columns,
                                                 uint32_t mi_rows,
                                                 size_t *required) {
    size_t chroma_columns;
    size_t chroma_rows;
    size_t luma;
    size_t chroma;
    size_t total;

    if (required == 0 || mi_columns == 0U || mi_rows == 0U) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    chroma_columns = (mi_columns + 1U) >> 1U;
    chroma_rows = (mi_rows + 1U) >> 1U;
    if (!transform_size_add(mi_columns, mi_rows, &luma) ||
        !transform_size_add(chroma_columns, chroma_rows, &chroma) ||
        !transform_size_add(luma, 2U * chroma, &total) ||
        total > (size_t)-1 / 2U) {
        return AVIFENC_OVERFLOW;
    }
    *required = 2U * total;
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_transform_state_init(
    AvifencAv1TransformState *state,
    uint8_t quantizer,
    uint32_t mi_columns,
    uint32_t mi_rows,
    void *workspace,
    size_t workspace_size) {
    static const uint16_t tx_type_set2[6] = {
        6554U, 13107U, 19661U, 26214U, 32768U, 0U
    };
    Av1CoeffPlaneContext plane[3];
    uint8_t *next = (uint8_t *)workspace;
    size_t required;
    unsigned int index;
    AvifdecStatus status;

    if (state == 0 || workspace == 0 || quantizer == 0U) {
        return quantizer == 0U ? AVIFENC_UNSUPPORTED
                               : AVIFENC_INVALID_ARGUMENT;
    }
    if (avifenc_av1_transform_context_size(
            mi_columns, mi_rows, &required) != AVIFENC_OK) {
        return AVIFENC_OVERFLOW;
    }
    if (workspace_size < required) return AVIFENC_OUT_OF_MEMORY;
    for (index = 0U; index < 3U; ++index) {
        size_t width4 = index == 0U ? mi_columns : (mi_columns + 1U) >> 1U;
        size_t height4 = index == 0U ? mi_rows : (mi_rows + 1U) >> 1U;

        plane[index].above_level_context = next;
        next += width4;
        plane[index].left_level_context = next;
        next += height4;
        plane[index].above_dc_context = next;
        next += width4;
        plane[index].left_dc_context = next;
        next += height4;
        plane[index].above_capacity = width4;
        plane[index].left_capacity = height4;
        plane[index].width4 = width4;
        plane[index].height4 = height4;
    }
    av1_coeff_cdfs_init(&state->cdfs, quantizer);
    avifdec_memory_copy(
        state->tx_type_set2, tx_type_set2, sizeof(state->tx_type_set2));
    status = av1_coeff_context_init(&state->contexts, plane);
    return status == AVIFDEC_OK ? AVIFENC_OK : AVIFENC_INVALID_ARGUMENT;
}

static unsigned int transform_txb_skip_context(
    const Av1CoeffPlaneContext *context,
    unsigned int plane,
    size_t x4,
    size_t y4) {
    unsigned int above;
    unsigned int left;

    if (plane == 0U) return 0U;
    above = context->above_level_context[x4] |
            context->above_dc_context[x4];
    left = context->left_level_context[y4] |
           context->left_dc_context[y4];
    return 7U + (above != 0U) + (left != 0U);
}

static unsigned int transform_dc_sign_context(
    const Av1CoeffPlaneContext *context,
    size_t x4,
    size_t y4) {
    int sign = 0;
    uint8_t above = context->above_dc_context[x4];
    uint8_t left = context->left_dc_context[y4];

    if (above == 1U) --sign;
    else if (above == 2U) ++sign;
    if (left == 1U) --sign;
    else if (left == 2U) ++sign;
    return sign < 0 ? 1U : sign > 0 ? 2U : 0U;
}

static unsigned int transform_base_context(const uint32_t levels[16],
                                           size_t position,
                                           size_t scan_index,
                                           int is_eob) {
    static const uint8_t offsets[4][4] = {
        { 0U, 1U, 6U, 6U },
        { 1U, 6U, 6U, 21U },
        { 6U, 6U, 21U, 21U },
        { 6U, 21U, 21U, 21U }
    };
    static const uint8_t references[5][2] = {
        { 0U, 1U }, { 1U, 0U }, { 1U, 1U }, { 0U, 2U }, { 2U, 0U }
    };
    size_t row = position >> 2U;
    size_t column = position & 3U;
    unsigned int magnitude = 0U;
    unsigned int context;
    size_t index;

    if (is_eob) {
        if (scan_index == 0U) return 38U;
        if (scan_index <= 2U) return 39U;
        if (scan_index <= 4U) return 40U;
        return 41U;
    }
    for (index = 0U; index < 5U; ++index) {
        size_t reference_row = row + references[index][0];
        size_t reference_column = column + references[index][1];

        if (reference_row < 4U && reference_column < 4U) {
            uint32_t level = levels[reference_row * 4U + reference_column];
            magnitude += level < 3U ? level : 3U;
        }
    }
    context = (magnitude + 1U) >> 1U;
    if (context > 4U) context = 4U;
    if (row == 0U && column == 0U) return 0U;
    return context + offsets[row][column];
}

static unsigned int transform_br_context(const uint32_t levels[16],
                                         size_t position) {
    static const uint8_t references[3][2] = {
        { 0U, 1U }, { 1U, 0U }, { 1U, 1U }
    };
    size_t row = position >> 2U;
    size_t column = position & 3U;
    unsigned int magnitude = 0U;
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        size_t reference_row = row + references[index][0];
        size_t reference_column = column + references[index][1];

        if (reference_row < 4U && reference_column < 4U) {
            uint32_t level = levels[reference_row * 4U + reference_column];
            magnitude += level < 15U ? level : 15U;
        }
    }
    magnitude = (magnitude + 1U) >> 1U;
    if (magnitude > 6U) magnitude = 6U;
    if (position == 0U) return magnitude;
    return magnitude + (row < 2U && column < 2U ? 7U : 14U);
}

static void transform_store_context(Av1CoeffPlaneContext *context,
                                    size_t x4,
                                    size_t y4,
                                    uint8_t cul_level,
                                    uint8_t dc_category) {
    context->above_level_context[x4] = cul_level;
    context->left_level_context[y4] = cul_level;
    context->above_dc_context[x4] = dc_category;
    context->left_dc_context[y4] = dc_category;
}

static unsigned int transform_floor_log2(uint32_t value) {
    unsigned int result = 0U;

    while (value > 1U) {
        value >>= 1U;
        ++result;
    }
    return result;
}

static AvifencStatus transform_write_eob(
    AvifencAv1TransformState *state,
    AvifencAv1SymbolWriter *writer,
    unsigned int plane_type,
    uint16_t eob) {
    unsigned int point = eob <= 1U ? eob
                                   : transform_floor_log2(eob - 1U) + 2U;
    AvifencStatus status = avifenc_av1_symbol_writer_write(
        writer, state->cdfs.eob_pt_16[plane_type][0], 5U, point - 1U);

    if (status != AVIFENC_OK || point < 3U) return status;
    {
        unsigned int high_shift = point - 3U;
        uint32_t offset = eob - ((1U << (point - 2U)) + 1U);
        unsigned int index;

        status = avifenc_av1_symbol_writer_write(
            writer, state->cdfs.eob_extra[0][plane_type][high_shift],
            2U, (offset >> high_shift) & 1U);
        for (index = 1U; status == AVIFENC_OK && index < point - 2U;
             ++index) {
            unsigned int shift = point - 3U - index;
            status = avifenc_av1_symbol_writer_literal(
                writer, (offset >> shift) & 1U, 1U);
        }
    }
    return status;
}

static AvifencStatus transform_write_golomb(AvifencAv1SymbolWriter *writer,
                                            uint32_t value) {
    unsigned int length = transform_floor_log2(value) + 1U;
    unsigned int index;
    AvifencStatus status = AVIFENC_OK;

    for (index = 1U; status == AVIFENC_OK && index < length; ++index) {
        status = avifenc_av1_symbol_writer_literal(writer, 0U, 1U);
    }
    if (status == AVIFENC_OK) {
        status = avifenc_av1_symbol_writer_literal(writer, 1U, 1U);
    }
    if (status == AVIFENC_OK && length > 1U) {
        status = avifenc_av1_symbol_writer_literal(
            writer, value & (((uint32_t)1U << (length - 1U)) - 1U),
            length - 1U);
    }
    return status;
}

static AvifencStatus transform_write_coefficients(
    AvifencAv1TransformState *state,
    AvifencAv1SymbolWriter *writer,
    unsigned int plane,
    size_t x4,
    size_t y4,
    int write_tx_type,
    const AvifencAv1TransformBlock *block) {
    Av1CoeffPlaneContext *context = &state->contexts.plane[plane];
    uint32_t levels[16] = { 0U };
    unsigned int plane_type = plane != 0U;
    unsigned int skip_context;
    uint32_t cul_level = 0U;
    uint8_t dc_category = 0U;
    size_t scan_index;
    AvifencStatus status;

    if (x4 >= context->width4 || y4 >= context->height4) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    skip_context = transform_txb_skip_context(context, plane, x4, y4);
    status = avifenc_av1_symbol_writer_write(
        writer, state->cdfs.txb_skip[0][skip_context], 2U,
        block->eob == 0U);
    if (status != AVIFENC_OK) return status;
    if (block->eob == 0U) {
        transform_store_context(context, x4, y4, 0U, 0U);
        return AVIFENC_OK;
    }
    if (write_tx_type) {
        status = avifenc_av1_symbol_writer_write(
            writer, state->tx_type_set2, 5U, 1U);
        if (status != AVIFENC_OK) return status;
    }
    status = transform_write_eob(state, writer, plane_type, block->eob);
    if (status != AVIFENC_OK) return status;
    for (scan_index = block->eob; scan_index-- > 0U;) {
        size_t position = transform_scan_4x4[scan_index];
        uint32_t level = block->quantized[position] < 0
            ? (uint32_t)(-(int64_t)block->quantized[position])
            : (uint32_t)block->quantized[position];
        unsigned int base_context = transform_base_context(
            levels, position, scan_index, scan_index == block->eob - 1U);

        if (scan_index == block->eob - 1U) {
            status = avifenc_av1_symbol_writer_write(
                writer,
                state->cdfs.coeff_base_eob[0][plane_type]
                                                  [base_context - 38U],
                3U, (level < 3U ? level : 3U) - 1U);
        } else {
            status = avifenc_av1_symbol_writer_write(
                writer, state->cdfs.coeff_base[0][plane_type][base_context],
                4U, level < 3U ? level : 3U);
        }
        if (status != AVIFENC_OK) return status;
        if (level > TRANSFORM_BASE_LEVELS) {
            unsigned int range_context = transform_br_context(levels, position);
            uint32_t remaining = level - 3U;
            unsigned int index;

            for (index = 0U; index < TRANSFORM_BASE_RANGE /
                                      (TRANSFORM_BR_CDF_SIZE - 1U);
                 ++index) {
                uint32_t increment = remaining < 3U ? remaining : 3U;

                status = avifenc_av1_symbol_writer_write(
                    writer, state->cdfs.coeff_br[0][plane_type][range_context],
                    TRANSFORM_BR_CDF_SIZE, increment);
                if (status != AVIFENC_OK) return status;
                remaining -= increment;
                if (increment < 3U) break;
            }
        }
        levels[position] = level;
    }
    for (scan_index = 0U; scan_index < block->eob; ++scan_index) {
        size_t position = transform_scan_4x4[scan_index];
        int32_t value = block->quantized[position];
        uint32_t level = value < 0 ? (uint32_t)(-(int64_t)value)
                                   : (uint32_t)value;

        if (level != 0U) {
            if (scan_index == 0U) {
                unsigned int dc_context = transform_dc_sign_context(
                    context, x4, y4);
                status = avifenc_av1_symbol_writer_write(
                    writer, state->cdfs.dc_sign[plane_type][dc_context],
                    2U, value < 0);
                dc_category = value < 0 ? 1U : 2U;
            } else {
                status = avifenc_av1_symbol_writer_literal(
                    writer, value < 0, 1U);
            }
            if (status != AVIFENC_OK) return status;
            if (level > TRANSFORM_BASE_LEVELS + TRANSFORM_BASE_RANGE) {
                status = transform_write_golomb(
                    writer, level - TRANSFORM_BASE_LEVELS -
                                      TRANSFORM_BASE_RANGE);
                if (status != AVIFENC_OK) return status;
            }
        }
        cul_level += level;
    }
    if (cul_level > 63U) cul_level = 63U;
    transform_store_context(
        context, x4, y4, (uint8_t)cul_level, dc_category);
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_transform_encode_4x4(
    AvifencAv1TransformState *state,
    AvifencAv1SymbolWriter *writer,
    unsigned int plane,
    size_t x4,
    size_t y4,
    const uint8_t *source,
    size_t source_stride,
    uint32_t source_width,
    uint32_t source_height,
    uint16_t *reconstruction,
    size_t reconstruction_stride,
    uint8_t quantizer,
    int write_tx_type,
    AvifencAv1TransformBlock *block) {
    int16_t input[16];
    int32_t transformed[16];
    int32_t dequantized[16];
    int32_t residual[16];
    Av1DequantParams params = { 0 };
    size_t pixel_x = x4 << 2U;
    size_t pixel_y = y4 << 2U;
    size_t row;
    size_t column;
    AvifdecStatus decoder_status;
    AvifencStatus status;

    if (state == 0 || writer == 0 || source == 0 || reconstruction == 0 ||
        block == 0 || plane >= 3U || source_stride < source_width ||
        reconstruction_stride < pixel_x + 4U || write_tx_type < 0 ||
        write_tx_type > 1) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    for (row = 0U; row < 4U; ++row) {
        for (column = 0U; column < 4U; ++column) {
            size_t x = pixel_x + column;
            size_t y = pixel_y + row;
            uint16_t predicted = reconstruction[
                y * reconstruction_stride + x];
            uint16_t sample = x < source_width && y < source_height
                ? source[y * source_stride + x] : predicted;

            input[row * 4U + column] = (int16_t)(sample - predicted);
        }
    }
    status = avifenc_av1_forward_dct_4x4(input, 4U, transformed);
    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_quantize_4x4(transformed, quantizer, block);
    if (status != AVIFENC_OK) return status;
    status = transform_write_coefficients(
        state, writer, plane, x4, y4, write_tx_type, block);
    if (status != AVIFENC_OK) return status;
    params.bit_depth = 8U;
    params.q_index = quantizer;
    params.plane = (uint8_t)plane;
    params.qm_level = 15U;
    decoder_status = av1_recon_dequantize(
        block->quantized, 16U, AV1_TX_4X4, AV1_TX_DCT_DCT,
        &params, dequantized, 16U);
    if (decoder_status == AVIFDEC_OK) {
        decoder_status = av1_recon_inverse_transform(
            dequantized, 16U, AV1_TX_4X4, AV1_TX_DCT_DCT,
            8U, 0U, residual, 16U);
    }
    if (decoder_status == AVIFDEC_OK) {
        decoder_status = av1_recon_add_residual(
            reconstruction + pixel_y * reconstruction_stride + pixel_x,
            reconstruction_stride, 4U, 4U, residual, 16U, 8U, 0U, 0U);
    }
    return decoder_status == AVIFDEC_OK ? AVIFENC_OK
                                        : AVIFENC_LIMIT_EXCEEDED;
}