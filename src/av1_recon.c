#include "av1_recon.h"
#include "base.h"

static const int16_t av1_cos128_lookup[65] = {
    4096, 4095, 4091, 4085, 4076, 4065, 4052, 4036,
    4017, 3996, 3973, 3948, 3920, 3889, 3857, 3822,
    3784, 3745, 3703, 3659, 3612, 3564, 3513, 3461,
    3406, 3349, 3290, 3229, 3166, 3102, 3035, 2967,
    2896, 2824, 2751, 2675, 2598, 2520, 2440, 2359,
    2276, 2191, 2106, 2019, 1931, 1842, 1751, 1660,
    1567, 1474, 1380, 1285, 1189, 1092, 995, 897,
    799, 700, 601, 501, 401, 301, 201, 101, 0
};

static const uint8_t av1_transform_row_shift[AV1_TX_SIZES_ALL] = {
    0U, 1U, 2U, 2U, 2U, 0U, 0U, 1U, 1U, 1U,
    1U, 1U, 1U, 1U, 1U, 2U, 2U, 2U, 2U
};

#include "av1_quant_tables.inc"

static int av1_recon_clip_qindex(int q_index) {
    if (q_index < 0) return 0;
    if (q_index > 255) return 255;
    return q_index;
}

static unsigned int av1_recon_bit_depth_index(uint8_t bit_depth) {
    return (unsigned int)(bit_depth - 8U) >> 1;
}

uint16_t av1_recon_dc_quant(uint8_t bit_depth, int q_index) {
    if (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U) return 0U;
    return av1_dc_qlookup[av1_recon_bit_depth_index(bit_depth)]
                         [av1_recon_clip_qindex(q_index)];
}

uint16_t av1_recon_ac_quant(uint8_t bit_depth, int q_index) {
    if (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U) return 0U;
    return av1_ac_qlookup[av1_recon_bit_depth_index(bit_depth)]
                         [av1_recon_clip_qindex(q_index)];
}

AvifdecStatus av1_recon_qmatrix_decode(uint8_t level,
                                       uint8_t chroma,
                                       uint8_t *matrix,
                                       size_t matrix_capacity) {
    size_t matrix_index;
    size_t start;
    size_t end;
    size_t bit_position = 0U;
    size_t output_index;

    if (level >= 15U || chroma > 1U || matrix == 0 ||
        matrix_capacity < AV1_QM_TOTAL_SIZE) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    matrix_index = (size_t)level * 2U + chroma;
    start = av1_qm_compressed_offsets[matrix_index];
    end = av1_qm_compressed_offsets[matrix_index + 1U];
    if (start >= end || end > sizeof(av1_qm_compressed)) {
        return AVIFDEC_INVALID_DATA;
    }
    matrix[0] = av1_qm_compressed[start];
    for (output_index = 1U; output_index < AV1_QM_TOTAL_SIZE;
         ++output_index) {
        int node = AV1_QM_TREE_ROOT;

        while (node >= 0) {
            size_t byte_index = start + 1U + bit_position / 8U;
            unsigned int bit;

            if (byte_index >= end ||
                (size_t)node >= sizeof(av1_qm_tree) /
                                    sizeof(av1_qm_tree[0])) {
                return AVIFDEC_INVALID_DATA;
            }
            bit = (av1_qm_compressed[byte_index] >>
                   (7U - (unsigned int)(bit_position & 7U))) & 1U;
            ++bit_position;
            node = av1_qm_tree[node][bit];
        }
        matrix[output_index] =
            (uint8_t)(matrix[output_index - 1U] + (uint8_t)(-node - 1));
    }
    return AVIFDEC_OK;
}

static unsigned int av1_recon_dq_denom(Av1TxSize tx_size) {
    if (tx_size == AV1_TX_32X32 || tx_size == AV1_TX_16X32 ||
        tx_size == AV1_TX_32X16 || tx_size == AV1_TX_16X64 ||
        tx_size == AV1_TX_64X16) {
        return 2U;
    }
    if (tx_size == AV1_TX_64X64 || tx_size == AV1_TX_32X64 ||
        tx_size == AV1_TX_64X32) {
        return 4U;
    }
    return 1U;
}

static int av1_recon_dc_delta(const Av1DequantParams *params) {
    if (params->plane == 0U) return params->delta_q_y_dc;
    if (params->plane == 1U) return params->delta_q_u_dc;
    return params->delta_q_v_dc;
}

static int av1_recon_ac_delta(const Av1DequantParams *params) {
    if (params->plane == 0U) return 0;
    if (params->plane == 1U) return params->delta_q_u_ac;
    return params->delta_q_v_ac;
}

static int32_t av1_recon_clip_dequant(int64_t value, uint8_t bit_depth) {
    int32_t limit = (int32_t)1 << (7U + bit_depth);
    if (value < -(int64_t)limit) return -limit;
    if (value > (int64_t)limit - 1) return limit - 1;
    return (int32_t)value;
}

AvifdecStatus av1_recon_dequantize(const int32_t *quantized,
                                   size_t quantized_count,
                                   Av1TxSize tx_size,
                                   Av1TxType tx_type,
                                   const Av1DequantParams *params,
                                   int32_t *dequantized,
                                   size_t dequantized_capacity) {
    const Av1TxSizeInfo *tx;
    size_t width;
    size_t height;
    size_t count;
    size_t index;
    unsigned int denominator;

    if (quantized == 0 || params == 0 || dequantized == 0 ||
        tx_size >= AV1_TX_SIZES_ALL || tx_type >= AV1_TX_TYPES ||
        params->plane >= 3U ||
        (params->bit_depth != 8U && params->bit_depth != 10U &&
         params->bit_depth != 12U) || params->qm_level > 15U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    tx = &av1_tx_size_info[tx_size];
    width = tx->width < 32U ? tx->width : 32U;
    height = tx->height < 32U ? tx->height : 32U;
    count = width * height;
    if (quantized_count < count || dequantized_capacity < count) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    denominator = av1_recon_dq_denom(tx_size);
    for (index = 0U; index < count; ++index) {
        int q_index = params->q_index;
        uint32_t q;
        uint32_t q2;
        int64_t product;
        uint64_t magnitude;
        int64_t value;

        if (index == 0U) {
            q = av1_recon_dc_quant(params->bit_depth,
                                   q_index + av1_recon_dc_delta(params));
        } else {
            q = av1_recon_ac_quant(params->bit_depth,
                                   q_index + av1_recon_ac_delta(params));
        }
        q2 = q;
        if (params->using_qmatrix != 0U && tx_type < AV1_TX_IDTX &&
            params->qm_level < 15U) {
            size_t qm_index = (size_t)av1_qm_offset[tx_size] + index;
            if (qm_index >= AV1_QM_TOTAL_SIZE || params->qmatrix == 0) {
                return AVIFDEC_INVALID_DATA;
            }
            q2 = (q * params->qmatrix[qm_index] + 16U) >> 5;
        }
        product = (int64_t)quantized[index] * q2;
        magnitude = product < 0 ? (uint64_t)(-product) : (uint64_t)product;
        magnitude = (magnitude & 0xffffffU) / denominator;
        value = product < 0 ? -(int64_t)magnitude : (int64_t)magnitude;
        dequantized[index] = av1_recon_clip_dequant(value, params->bit_depth);
    }
    return AVIFDEC_OK;
}

static int64_t av1_recon_floor_div_pow2(int64_t value, unsigned int bits) {
    uint64_t magnitude;
    uint64_t mask;

    if (bits == 0U) return value;
    if (value >= 0) return value >> bits;
    magnitude = (uint64_t)(-value);
    mask = ((uint64_t)1U << bits) - 1U;
    return -(int64_t)((magnitude + mask) >> bits);
}

static int32_t av1_recon_round2(int64_t value, unsigned int bits) {
    if (bits == 0U) return (int32_t)value;
    return (int32_t)av1_recon_floor_div_pow2(
        value + ((int64_t)1U << (bits - 1U)), bits);
}

static int32_t av1_recon_clip_signed(int64_t value, unsigned int bits) {
    int64_t limit = (int64_t)1U << (bits - 1U);
    if (value < -limit) return (int32_t)-limit;
    if (value > limit - 1) return (int32_t)(limit - 1);
    return (int32_t)value;
}

static unsigned int av1_recon_brev(unsigned int bits, unsigned int value) {
    unsigned int result = 0U;
    unsigned int index;
    for (index = 0U; index < bits; ++index) {
        result |= ((value >> index) & 1U) << (bits - index - 1U);
    }
    return result;
}

static int32_t av1_recon_cos128(int angle) {
    unsigned int wrapped = (unsigned int)angle & 255U;
    if (wrapped <= 64U) return av1_cos128_lookup[wrapped];
    if (wrapped <= 128U) return -av1_cos128_lookup[128U - wrapped];
    if (wrapped <= 192U) return -av1_cos128_lookup[wrapped - 128U];
    return av1_cos128_lookup[256U - wrapped];
}

static void av1_recon_butterfly(int32_t *values,
                                unsigned int first,
                                unsigned int second,
                                int angle,
                                unsigned int flip) {
    int32_t left = values[first];
    int32_t right = values[second];
    int32_t cosine = av1_recon_cos128(angle);
    int32_t sine = av1_recon_cos128(angle - 64);
    int32_t x = av1_recon_round2((int64_t)left * cosine -
                                 (int64_t)right * sine, 12U);
    int32_t y = av1_recon_round2((int64_t)left * sine +
                                 (int64_t)right * cosine, 12U);
    values[first] = flip != 0U ? y : x;
    values[second] = flip != 0U ? x : y;
}

static void av1_recon_hadamard(int32_t *values,
                               unsigned int first,
                               unsigned int second,
                               unsigned int flip,
                               unsigned int clamp_range) {
    int32_t left = values[first];
    int32_t right = values[second];
    if (flip == 0U) {
        values[first] = av1_recon_clip_signed((int64_t)left + right, clamp_range);
        values[second] = av1_recon_clip_signed((int64_t)left - right, clamp_range);
    } else {
        values[first] = av1_recon_clip_signed((int64_t)right - left, clamp_range);
        values[second] = av1_recon_clip_signed((int64_t)right + left, clamp_range);
    }
}

static void av1_recon_inverse_dct(int32_t *values,
                                  unsigned int length_log2,
                                  unsigned int clamp_range) {
    int32_t copy[64];
    unsigned int length = 1U << length_log2;
    unsigned int index;
    unsigned int outer;
    unsigned int inner;

    for (index = 0U; index < length; ++index) copy[index] = values[index];
    for (index = 0U; index < length; ++index) {
        values[index] = copy[av1_recon_brev(length_log2, index)];
    }
    if (length_log2 == 6U) for (index = 0U; index < 16U; ++index)
        av1_recon_butterfly(values, 32U + index, 63U - index,
                            63 - 4 * (int)av1_recon_brev(4U, index), 0U);
    if (length_log2 >= 5U) for (index = 0U; index < 8U; ++index)
        av1_recon_butterfly(values, 16U + index, 31U - index,
                            6 + (int)(av1_recon_brev(3U, 7U - index) << 3), 0U);
    if (length_log2 == 6U) for (index = 0U; index < 16U; ++index)
        av1_recon_hadamard(values, 32U + index * 2U, 33U + index * 2U,
                           index & 1U, clamp_range);
    if (length_log2 >= 4U) for (index = 0U; index < 4U; ++index)
        av1_recon_butterfly(values, 8U + index, 15U - index,
                            12 + (int)(av1_recon_brev(2U, 3U - index) << 4), 0U);
    if (length_log2 >= 5U) for (index = 0U; index < 8U; ++index)
        av1_recon_hadamard(values, 16U + index * 2U, 17U + index * 2U,
                           index & 1U, clamp_range);
    if (length_log2 == 6U) for (outer = 0U; outer < 4U; ++outer)
        for (inner = 0U; inner < 2U; ++inner)
            av1_recon_butterfly(values, 62U - outer * 4U - inner,
                                33U + outer * 4U + inner,
                                60 - 16 * (int)av1_recon_brev(2U, outer) +
                                64 * (int)inner, 1U);
    if (length_log2 >= 3U) for (index = 0U; index < 2U; ++index)
        av1_recon_butterfly(values, 4U + index, 7U - index,
                            56 - 32 * (int)index, 0U);
    if (length_log2 >= 4U) for (index = 0U; index < 4U; ++index)
        av1_recon_hadamard(values, 8U + index * 2U, 9U + index * 2U,
                           index & 1U, clamp_range);
    if (length_log2 >= 5U) for (outer = 0U; outer < 2U; ++outer)
        for (inner = 0U; inner < 2U; ++inner)
            av1_recon_butterfly(values, 30U - outer * 4U - inner,
                                17U + outer * 4U + inner,
                                24 + 64 * (int)inner + 32 * (1 - (int)outer), 1U);
    if (length_log2 == 6U) for (outer = 0U; outer < 8U; ++outer)
        for (inner = 0U; inner < 2U; ++inner)
            av1_recon_hadamard(values, 32U + outer * 4U + inner,
                               35U + outer * 4U - inner,
                               outer & 1U, clamp_range);
    for (index = 0U; index < 2U; ++index)
        av1_recon_butterfly(values, 2U * index, 2U * index + 1U,
                            32 + 16 * (int)index, 1U - index);
    if (length_log2 >= 3U) for (index = 0U; index < 2U; ++index)
        av1_recon_hadamard(values, 4U + index * 2U, 5U + index * 2U,
                           index, clamp_range);
    if (length_log2 >= 4U) for (index = 0U; index < 2U; ++index)
        av1_recon_butterfly(values, 14U - index, 9U + index,
                            48 + 64 * (int)index, 1U);
    if (length_log2 >= 5U) for (outer = 0U; outer < 4U; ++outer)
        for (inner = 0U; inner < 2U; ++inner)
            av1_recon_hadamard(values, 16U + outer * 4U + inner,
                               19U + outer * 4U - inner,
                               outer & 1U, clamp_range);
    if (length_log2 == 6U) for (outer = 0U; outer < 2U; ++outer)
        for (inner = 0U; inner < 4U; ++inner)
            av1_recon_butterfly(values, 61U - outer * 8U - inner,
                                34U + outer * 8U + inner,
                                56 - 32 * (int)outer + 64 * (int)(inner >> 1), 1U);
    for (index = 0U; index < 2U; ++index)
        av1_recon_hadamard(values, index, 3U - index, 0U, clamp_range);
    if (length_log2 >= 3U) av1_recon_butterfly(values, 6U, 5U, 32, 1U);
    if (length_log2 >= 4U) for (outer = 0U; outer < 2U; ++outer)
        for (inner = 0U; inner < 2U; ++inner)
            av1_recon_hadamard(values, 8U + outer * 4U + inner,
                               11U + outer * 4U - inner, outer, clamp_range);
    if (length_log2 >= 5U) for (index = 0U; index < 4U; ++index)
        av1_recon_butterfly(values, 29U - index, 18U + index,
                            48 + 64 * (int)(index >> 1), 1U);
    if (length_log2 == 6U) for (outer = 0U; outer < 4U; ++outer)
        for (inner = 0U; inner < 4U; ++inner)
            av1_recon_hadamard(values, 32U + outer * 8U + inner,
                               39U + outer * 8U - inner,
                               outer & 1U, clamp_range);
    if (length_log2 >= 3U) for (index = 0U; index < 4U; ++index)
        av1_recon_hadamard(values, index, 7U - index, 0U, clamp_range);
    if (length_log2 >= 4U) for (index = 0U; index < 2U; ++index)
        av1_recon_butterfly(values, 13U - index, 10U + index, 32, 1U);
    if (length_log2 >= 5U) for (outer = 0U; outer < 2U; ++outer)
        for (inner = 0U; inner < 4U; ++inner)
            av1_recon_hadamard(values, 16U + outer * 8U + inner,
                               23U + outer * 8U - inner, outer, clamp_range);
    if (length_log2 == 6U) for (index = 0U; index < 8U; ++index)
        av1_recon_butterfly(values, 59U - index, 36U + index,
                            index < 4U ? 48 : 112, 1U);
    if (length_log2 >= 4U) for (index = 0U; index < 8U; ++index)
        av1_recon_hadamard(values, index, 15U - index, 0U, clamp_range);
    if (length_log2 >= 5U) for (index = 0U; index < 4U; ++index)
        av1_recon_butterfly(values, 27U - index, 20U + index, 32, 1U);
    if (length_log2 == 6U) for (index = 0U; index < 8U; ++index) {
        av1_recon_hadamard(values, 32U + index, 47U - index, 0U, clamp_range);
        av1_recon_hadamard(values, 48U + index, 63U - index, 1U, clamp_range);
    }
    if (length_log2 >= 5U) for (index = 0U; index < 16U; ++index)
        av1_recon_hadamard(values, index, 31U - index, 0U, clamp_range);
    if (length_log2 == 6U) for (index = 0U; index < 8U; ++index)
        av1_recon_butterfly(values, 55U - index, 40U + index, 32, 1U);
    if (length_log2 == 6U) for (index = 0U; index < 32U; ++index)
        av1_recon_hadamard(values, index, 63U - index, 0U, clamp_range);
}

static void av1_recon_adst_permute_input(int32_t *values,
                                         unsigned int length_log2) {
    int32_t copy[16];
    unsigned int length = 1U << length_log2;
    unsigned int index;
    for (index = 0U; index < length; ++index) copy[index] = values[index];
    for (index = 0U; index < length; ++index) {
        unsigned int source = (index & 1U) != 0U ? index - 1U
                                                : length - index - 1U;
        values[index] = copy[source];
    }
}

static void av1_recon_adst_permute_output(int32_t *values,
                                          unsigned int length_log2) {
    int32_t copy[16];
    unsigned int length = 1U << length_log2;
    unsigned int index;
    for (index = 0U; index < length; ++index) copy[index] = values[index];
    for (index = 0U; index < length; ++index) {
        unsigned int a = (index >> 3) & 1U;
        unsigned int b = ((index >> 2) & 1U) ^ a;
        unsigned int c = ((index >> 1) & 1U) ^ ((index >> 2) & 1U);
        unsigned int d = (index & 1U) ^ ((index >> 1) & 1U);
        unsigned int source = ((d << 3) | (c << 2) | (b << 1) | a) >>
                              (4U - length_log2);
        values[index] = (index & 1U) != 0U ? -copy[source] : copy[source];
    }
}

static void av1_recon_inverse_adst4(int32_t *values) {
    int64_t s0 = 1321LL * values[0] + 3803LL * values[2] + 2482LL * values[3];
    int64_t s1 = 2482LL * values[0] - 1321LL * values[2] - 3803LL * values[3];
    int64_t s3 = 3344LL * values[1];
    int64_t s2 = 3344LL * (values[0] - values[2] + values[3]);
    int64_t x0 = s0 + s3;
    int64_t x1 = s1 + s3;
    int64_t x2 = s2;
    int64_t x3 = s0 + s1 - s3;
    values[0] = av1_recon_round2(x0, 12U);
    values[1] = av1_recon_round2(x1, 12U);
    values[2] = av1_recon_round2(x2, 12U);
    values[3] = av1_recon_round2(x3, 12U);
}

static void av1_recon_inverse_adst(int32_t *values,
                                   unsigned int length_log2,
                                   unsigned int clamp_range) {
    unsigned int index;
    unsigned int outer;
    if (length_log2 == 2U) {
        av1_recon_inverse_adst4(values);
        return;
    }
    av1_recon_adst_permute_input(values, length_log2);
    if (length_log2 == 3U) {
        for (index = 0U; index < 4U; ++index)
            av1_recon_butterfly(values, 2U * index, 2U * index + 1U,
                                60 - 16 * (int)index, 1U);
        for (index = 0U; index < 4U; ++index)
            av1_recon_hadamard(values, index, 4U + index, 0U, clamp_range);
        for (index = 0U; index < 2U; ++index)
            av1_recon_butterfly(values, 4U + 3U * index, 5U + index,
                                48 - 32 * (int)index, 1U);
        for (outer = 0U; outer < 2U; ++outer)
            for (index = 0U; index < 2U; ++index)
                av1_recon_hadamard(values, 4U * outer + index,
                                   2U + 4U * outer + index, 0U, clamp_range);
        for (index = 0U; index < 2U; ++index)
            av1_recon_butterfly(values, 2U + 4U * index, 3U + 4U * index,
                                32, 1U);
    } else {
        for (index = 0U; index < 8U; ++index)
            av1_recon_butterfly(values, 2U * index, 2U * index + 1U,
                                62 - 8 * (int)index, 1U);
        for (index = 0U; index < 8U; ++index)
            av1_recon_hadamard(values, index, 8U + index, 0U, clamp_range);
        for (index = 0U; index < 2U; ++index) {
            av1_recon_butterfly(values, 8U + 2U * index, 9U + 2U * index,
                                56 - 32 * (int)index, 1U);
            av1_recon_butterfly(values, 13U + 2U * index, 12U + 2U * index,
                                8 + 32 * (int)index, 1U);
        }
        for (outer = 0U; outer < 2U; ++outer)
            for (index = 0U; index < 4U; ++index)
                av1_recon_hadamard(values, 8U * outer + index,
                                   4U + 8U * outer + index, 0U, clamp_range);
        for (outer = 0U; outer < 2U; ++outer)
            for (index = 0U; index < 2U; ++index)
                av1_recon_butterfly(values, 4U + 8U * outer + 3U * index,
                                    5U + 8U * outer + index,
                                    48 - 32 * (int)index, 1U);
        for (outer = 0U; outer < 4U; ++outer)
            for (index = 0U; index < 2U; ++index)
                av1_recon_hadamard(values, 4U * outer + index,
                                   2U + 4U * outer + index, 0U, clamp_range);
        for (index = 0U; index < 4U; ++index)
            av1_recon_butterfly(values, 2U + 4U * index, 3U + 4U * index,
                                32, 1U);
    }
    av1_recon_adst_permute_output(values, length_log2);
}

static void av1_recon_inverse_identity(int32_t *values,
                                       unsigned int length_log2) {
    unsigned int length = 1U << length_log2;
    unsigned int index;
    for (index = 0U; index < length; ++index) {
        if (length_log2 == 2U) {
            values[index] = av1_recon_round2((int64_t)values[index] * 5793, 12U);
        } else if (length_log2 == 3U) {
            values[index] *= 2;
        } else if (length_log2 == 4U) {
            values[index] = av1_recon_round2((int64_t)values[index] * 11586, 12U);
        } else {
            values[index] *= 4;
        }
    }
}

static void av1_recon_inverse_wht(int32_t *values, unsigned int shift) {
    int32_t a = (int32_t)av1_recon_floor_div_pow2(values[0], shift);
    int32_t c = (int32_t)av1_recon_floor_div_pow2(values[1], shift);
    int32_t d = (int32_t)av1_recon_floor_div_pow2(values[2], shift);
    int32_t b = (int32_t)av1_recon_floor_div_pow2(values[3], shift);
    int32_t e;
    a += c;
    d -= b;
    e = (int32_t)av1_recon_floor_div_pow2((int64_t)a - d, 1U);
    b = e - b;
    c = e - c;
    a -= b;
    d += c;
    values[0] = a;
    values[1] = b;
    values[2] = c;
    values[3] = d;
}

AvifdecStatus av1_recon_inverse_1d(int32_t *values,
                                   unsigned int length_log2,
                                   Av1Inverse1dType type,
                                   unsigned int clamp_range,
                                   unsigned int wht_shift) {
    if (values == 0 || length_log2 < 2U || length_log2 > 6U ||
        clamp_range < 2U || clamp_range > 31U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (type == AV1_INVERSE_DCT) {
        av1_recon_inverse_dct(values, length_log2, clamp_range);
    } else if (type == AV1_INVERSE_ADST) {
        if (length_log2 > 4U) return AVIFDEC_INVALID_ARGUMENT;
        av1_recon_inverse_adst(values, length_log2, clamp_range);
    } else if (type == AV1_INVERSE_IDENTITY) {
        if (length_log2 > 5U) return AVIFDEC_INVALID_ARGUMENT;
        av1_recon_inverse_identity(values, length_log2);
    } else if (type == AV1_INVERSE_WHT) {
        if (length_log2 != 2U || wht_shift > 2U) return AVIFDEC_INVALID_ARGUMENT;
        av1_recon_inverse_wht(values, wht_shift);
    } else {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    return AVIFDEC_OK;
}

static Av1Inverse1dType av1_recon_row_type(Av1TxType tx_type) {
    if (tx_type == AV1_TX_DCT_DCT || tx_type == AV1_TX_ADST_DCT ||
        tx_type == AV1_TX_FLIPADST_DCT || tx_type == AV1_TX_H_DCT) {
        return AV1_INVERSE_DCT;
    }
    if (tx_type == AV1_TX_DCT_ADST || tx_type == AV1_TX_ADST_ADST ||
        tx_type == AV1_TX_DCT_FLIPADST || tx_type == AV1_TX_FLIPADST_FLIPADST ||
        tx_type == AV1_TX_ADST_FLIPADST || tx_type == AV1_TX_FLIPADST_ADST ||
        tx_type == AV1_TX_H_ADST || tx_type == AV1_TX_H_FLIPADST) {
        return AV1_INVERSE_ADST;
    }
    return AV1_INVERSE_IDENTITY;
}

static Av1Inverse1dType av1_recon_column_type(Av1TxType tx_type) {
    if (tx_type == AV1_TX_DCT_DCT || tx_type == AV1_TX_DCT_ADST ||
        tx_type == AV1_TX_DCT_FLIPADST || tx_type == AV1_TX_V_DCT) {
        return AV1_INVERSE_DCT;
    }
    if (tx_type == AV1_TX_ADST_DCT || tx_type == AV1_TX_ADST_ADST ||
        tx_type == AV1_TX_FLIPADST_DCT || tx_type == AV1_TX_FLIPADST_FLIPADST ||
        tx_type == AV1_TX_ADST_FLIPADST || tx_type == AV1_TX_FLIPADST_ADST ||
        tx_type == AV1_TX_V_ADST || tx_type == AV1_TX_V_FLIPADST) {
        return AV1_INVERSE_ADST;
    }
    return AV1_INVERSE_IDENTITY;
}

AvifdecStatus av1_recon_inverse_transform(const int32_t *dequantized,
                                          size_t dequantized_count,
                                          Av1TxSize tx_size,
                                          Av1TxType tx_type,
                                          uint8_t bit_depth,
                                          uint8_t lossless,
                                          int32_t *residual,
                                          size_t residual_capacity) {
    const Av1TxSizeInfo *tx;
    size_t width;
    size_t height;
    size_t dequant_width;
    size_t dequant_height;
    size_t row;
    size_t column;
    int32_t values[64];
    unsigned int row_clamp;
    unsigned int column_clamp;
    unsigned int row_shift;

    if (dequantized == 0 || residual == 0 || tx_size >= AV1_TX_SIZES_ALL ||
        tx_type >= AV1_TX_TYPES ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    tx = &av1_tx_size_info[tx_size];
    width = tx->width;
    height = tx->height;
    dequant_width = width < 32U ? width : 32U;
    dequant_height = height < 32U ? height : 32U;
    if (dequantized_count < dequant_width * dequant_height ||
        residual_capacity < width * height ||
        (lossless != 0U && tx_size != AV1_TX_4X4)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    row_clamp = bit_depth + 8U;
    column_clamp = bit_depth + 6U < 16U ? 16U : bit_depth + 6U;
    row_shift = lossless != 0U ? 0U : av1_transform_row_shift[tx_size];
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            values[column] = row < dequant_height && column < dequant_width
                             ? dequantized[row * dequant_width + column] : 0;
            if ((tx->width_log2 + 1U == tx->height_log2 ||
                 tx->height_log2 + 1U == tx->width_log2)) {
                values[column] = av1_recon_round2(
                    (int64_t)values[column] * 2896, 12U);
            }
        }
        if (lossless != 0U) {
            av1_recon_inverse_wht(values, 2U);
        } else if (av1_recon_inverse_1d(values, tx->width_log2,
                                        av1_recon_row_type(tx_type),
                                        row_clamp, 0U) != AVIFDEC_OK) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        for (column = 0U; column < width; ++column) {
            residual[row * width + column] = av1_recon_clip_signed(
                av1_recon_round2(values[column], row_shift), column_clamp);
        }
    }
    for (column = 0U; column < width; ++column) {
        for (row = 0U; row < height; ++row) {
            values[row] = residual[row * width + column];
        }
        if (lossless != 0U) {
            av1_recon_inverse_wht(values, 0U);
        } else if (av1_recon_inverse_1d(values, tx->height_log2,
                                        av1_recon_column_type(tx_type),
                                        column_clamp, 0U) != AVIFDEC_OK) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        for (row = 0U; row < height; ++row) {
            residual[row * width + column] = av1_recon_round2(
                values[row], lossless != 0U ? 0U : 4U);
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_recon_add_residual(uint16_t *destination,
                                     size_t stride,
                                     size_t width,
                                     size_t height,
                                     const int32_t *residual,
                                     size_t residual_count,
                                     uint8_t bit_depth,
                                     uint8_t flip_lr,
                                     uint8_t flip_ud) {
    size_t row;
    size_t column;
    int32_t maximum;
    if (destination == 0 || residual == 0 || stride < width ||
        residual_count < width * height ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    maximum = ((int32_t)1 << bit_depth) - 1;
    for (row = 0U; row < height; ++row) {
        size_t output_row = flip_ud != 0U ? height - row - 1U : row;
        for (column = 0U; column < width; ++column) {
            size_t output_column = flip_lr != 0U ? width - column - 1U : column;
            int32_t value = destination[output_row * stride + output_column] +
                            residual[row * width + column];
            if (value < 0) value = 0;
            if (value > maximum) value = maximum;
            destination[output_row * stride + output_column] = (uint16_t)value;
        }
    }
    return AVIFDEC_OK;
}