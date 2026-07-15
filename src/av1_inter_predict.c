#include "av1_inter_predict.h"

#define AV1_FILTER_BITS 7
#define AV1_SUBPEL_BITS 4
#define AV1_SUBPEL_SHIFTS (1 << AV1_SUBPEL_BITS)
#define AV1_SCALE_SUBPEL_BITS 10
#define AV1_SCALE_EXTRA_BITS (AV1_SCALE_SUBPEL_BITS - AV1_SUBPEL_BITS)
#define AV1_SCALE_SUBPEL_MASK ((1 << AV1_SCALE_SUBPEL_BITS) - 1)
#define AV1_REF_SCALE_SHIFT 14
#define AV1_ROUND0_BITS 3
#define AV1_COMPOUND_ROUND1_BITS 7
#define AV1_DIFF_FACTOR_LOG2 4
#define AV1_DIFF_MASK_BASE 38
#define AV1_WEDGE_MASTER_SIZE 64

typedef struct Av1WedgeCode {
    uint8_t direction;
    uint8_t x_offset;
    uint8_t y_offset;
} Av1WedgeCode;

enum {
    AV1_WEDGE_HORIZONTAL = 0,
    AV1_WEDGE_VERTICAL = 1,
    AV1_WEDGE_OBLIQUE27 = 2,
    AV1_WEDGE_OBLIQUE63 = 3,
    AV1_WEDGE_OBLIQUE117 = 4,
    AV1_WEDGE_OBLIQUE153 = 5
};

typedef struct Av1FilterKernel {
    const int16_t (*coeffs)[8];
    uint8_t taps;
} Av1FilterKernel;

static const int16_t av1_sub_pel_filters_8[16][8] = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },
    { 0, 2, -6, 126, 8, -2, 0, 0 },
    { 0, 2, -10, 122, 18, -4, 0, 0 },
    { 0, 2, -12, 116, 28, -8, 2, 0 },
    { 0, 2, -14, 110, 38, -10, 2, 0 },
    { 0, 2, -14, 102, 48, -12, 2, 0 },
    { 0, 2, -16, 94, 58, -12, 2, 0 },
    { 0, 2, -14, 84, 66, -12, 2, 0 },
    { 0, 2, -14, 76, 76, -14, 2, 0 },
    { 0, 2, -12, 66, 84, -14, 2, 0 },
    { 0, 2, -12, 58, 94, -16, 2, 0 },
    { 0, 2, -12, 48, 102, -14, 2, 0 },
    { 0, 2, -10, 38, 110, -14, 2, 0 },
    { 0, 2, -8, 28, 116, -12, 2, 0 },
    { 0, 0, -4, 18, 122, -10, 2, 0 },
    { 0, 0, -2, 8, 126, -6, 2, 0 }
};

static const int16_t av1_sub_pel_filters_8smooth[16][8] = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },
    { 0, 2, 28, 62, 34, 2, 0, 0 },
    { 0, 0, 26, 62, 36, 4, 0, 0 },
    { 0, 0, 22, 62, 40, 4, 0, 0 },
    { 0, 0, 20, 60, 42, 6, 0, 0 },
    { 0, 0, 18, 58, 44, 8, 0, 0 },
    { 0, 0, 16, 56, 46, 10, 0, 0 },
    { 0, -2, 16, 54, 48, 12, 0, 0 },
    { 0, -2, 14, 52, 52, 14, -2, 0 },
    { 0, 0, 12, 48, 54, 16, -2, 0 },
    { 0, 0, 10, 46, 56, 16, 0, 0 },
    { 0, 0, 8, 44, 58, 18, 0, 0 },
    { 0, 0, 6, 42, 60, 20, 0, 0 },
    { 0, 0, 4, 40, 62, 22, 0, 0 },
    { 0, 0, 4, 36, 62, 26, 0, 0 },
    { 0, 0, 2, 34, 62, 28, 2, 0 }
};

static const int16_t av1_sub_pel_filters_8sharp[16][8] = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },
    { -2, 2, -6, 126, 8, -2, 2, 0 },
    { -2, 6, -12, 124, 16, -6, 4, -2 },
    { -2, 8, -18, 120, 26, -10, 6, -2 },
    { -4, 10, -22, 116, 38, -14, 6, -2 },
    { -4, 10, -22, 108, 48, -18, 8, -2 },
    { -4, 10, -24, 100, 60, -20, 8, -2 },
    { -4, 10, -24, 90, 70, -22, 10, -2 },
    { -4, 12, -24, 80, 80, -24, 12, -4 },
    { -2, 10, -22, 70, 90, -24, 10, -4 },
    { -2, 8, -20, 60, 100, -24, 10, -4 },
    { -2, 8, -18, 48, 108, -22, 10, -4 },
    { -2, 6, -14, 38, 116, -22, 10, -4 },
    { -2, 6, -10, 26, 120, -18, 8, -2 },
    { -2, 4, -6, 16, 124, -12, 6, -2 },
    { 0, 2, -2, 8, 126, -6, 2, -2 }
};

static const int16_t av1_bilinear_filters[16][8] = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },
    { 0, 0, 0, 120, 8, 0, 0, 0 },
    { 0, 0, 0, 112, 16, 0, 0, 0 },
    { 0, 0, 0, 104, 24, 0, 0, 0 },
    { 0, 0, 0, 96, 32, 0, 0, 0 },
    { 0, 0, 0, 88, 40, 0, 0, 0 },
    { 0, 0, 0, 80, 48, 0, 0, 0 },
    { 0, 0, 0, 72, 56, 0, 0, 0 },
    { 0, 0, 0, 64, 64, 0, 0, 0 },
    { 0, 0, 0, 56, 72, 0, 0, 0 },
    { 0, 0, 0, 48, 80, 0, 0, 0 },
    { 0, 0, 0, 40, 88, 0, 0, 0 },
    { 0, 0, 0, 32, 96, 0, 0, 0 },
    { 0, 0, 0, 24, 104, 0, 0, 0 },
    { 0, 0, 0, 16, 112, 0, 0, 0 },
    { 0, 0, 0, 8, 120, 0, 0, 0 }
};

static const int16_t av1_sub_pel_filters_4[16][8] = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },
    { 0, 0, -4, 126, 8, -2, 0, 0 },
    { 0, 0, -8, 122, 18, -4, 0, 0 },
    { 0, 0, -10, 116, 28, -6, 0, 0 },
    { 0, 0, -12, 110, 38, -8, 0, 0 },
    { 0, 0, -12, 102, 48, -10, 0, 0 },
    { 0, 0, -14, 94, 58, -10, 0, 0 },
    { 0, 0, -12, 84, 66, -10, 0, 0 },
    { 0, 0, -12, 76, 76, -12, 0, 0 },
    { 0, 0, -10, 66, 84, -12, 0, 0 },
    { 0, 0, -10, 58, 94, -14, 0, 0 },
    { 0, 0, -10, 48, 102, -12, 0, 0 },
    { 0, 0, -8, 38, 110, -12, 0, 0 },
    { 0, 0, -6, 28, 116, -10, 0, 0 },
    { 0, 0, -4, 18, 122, -8, 0, 0 },
    { 0, 0, -2, 8, 126, -4, 0, 0 }
};

static const int16_t av1_sub_pel_filters_4smooth[16][8] = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },
    { 0, 0, 30, 62, 34, 2, 0, 0 },
    { 0, 0, 26, 62, 36, 4, 0, 0 },
    { 0, 0, 22, 62, 40, 4, 0, 0 },
    { 0, 0, 20, 60, 42, 6, 0, 0 },
    { 0, 0, 18, 58, 44, 8, 0, 0 },
    { 0, 0, 16, 56, 46, 10, 0, 0 },
    { 0, 0, 14, 54, 48, 12, 0, 0 },
    { 0, 0, 12, 52, 52, 12, 0, 0 },
    { 0, 0, 12, 48, 54, 14, 0, 0 },
    { 0, 0, 10, 46, 56, 16, 0, 0 },
    { 0, 0, 8, 44, 58, 18, 0, 0 },
    { 0, 0, 6, 42, 60, 20, 0, 0 },
    { 0, 0, 4, 40, 62, 22, 0, 0 },
    { 0, 0, 4, 36, 62, 26, 0, 0 },
    { 0, 0, 2, 34, 62, 30, 0, 0 }
};

static const uint8_t av1_wedge_master_oblique_odd[64] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  6,  18,
    37, 53, 60, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

static const uint8_t av1_wedge_master_oblique_even[64] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  4,  11, 27,
    46, 58, 62, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

static const uint8_t av1_wedge_master_vertical[64] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  7,  21,
    43, 57, 62, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

static const Av1WedgeCode av1_wedge_codebook_hgtw[16] = {
    { AV1_WEDGE_OBLIQUE27, 4, 4 },  { AV1_WEDGE_OBLIQUE63, 4, 4 },
    { AV1_WEDGE_OBLIQUE117, 4, 4 }, { AV1_WEDGE_OBLIQUE153, 4, 4 },
    { AV1_WEDGE_HORIZONTAL, 4, 2 }, { AV1_WEDGE_HORIZONTAL, 4, 4 },
    { AV1_WEDGE_HORIZONTAL, 4, 6 }, { AV1_WEDGE_VERTICAL, 4, 4 },
    { AV1_WEDGE_OBLIQUE27, 4, 2 },  { AV1_WEDGE_OBLIQUE27, 4, 6 },
    { AV1_WEDGE_OBLIQUE153, 4, 2 }, { AV1_WEDGE_OBLIQUE153, 4, 6 },
    { AV1_WEDGE_OBLIQUE63, 2, 4 },  { AV1_WEDGE_OBLIQUE63, 6, 4 },
    { AV1_WEDGE_OBLIQUE117, 2, 4 }, { AV1_WEDGE_OBLIQUE117, 6, 4 }
};

static const Av1WedgeCode av1_wedge_codebook_hltw[16] = {
    { AV1_WEDGE_OBLIQUE27, 4, 4 },  { AV1_WEDGE_OBLIQUE63, 4, 4 },
    { AV1_WEDGE_OBLIQUE117, 4, 4 }, { AV1_WEDGE_OBLIQUE153, 4, 4 },
    { AV1_WEDGE_VERTICAL, 2, 4 },   { AV1_WEDGE_VERTICAL, 4, 4 },
    { AV1_WEDGE_VERTICAL, 6, 4 },   { AV1_WEDGE_HORIZONTAL, 4, 4 },
    { AV1_WEDGE_OBLIQUE27, 4, 2 },  { AV1_WEDGE_OBLIQUE27, 4, 6 },
    { AV1_WEDGE_OBLIQUE153, 4, 2 }, { AV1_WEDGE_OBLIQUE153, 4, 6 },
    { AV1_WEDGE_OBLIQUE63, 2, 4 },  { AV1_WEDGE_OBLIQUE63, 6, 4 },
    { AV1_WEDGE_OBLIQUE117, 2, 4 }, { AV1_WEDGE_OBLIQUE117, 6, 4 }
};

static const Av1WedgeCode av1_wedge_codebook_heqw[16] = {
    { AV1_WEDGE_OBLIQUE27, 4, 4 },  { AV1_WEDGE_OBLIQUE63, 4, 4 },
    { AV1_WEDGE_OBLIQUE117, 4, 4 }, { AV1_WEDGE_OBLIQUE153, 4, 4 },
    { AV1_WEDGE_HORIZONTAL, 4, 2 }, { AV1_WEDGE_HORIZONTAL, 4, 6 },
    { AV1_WEDGE_VERTICAL, 2, 4 },   { AV1_WEDGE_VERTICAL, 6, 4 },
    { AV1_WEDGE_OBLIQUE27, 4, 2 },  { AV1_WEDGE_OBLIQUE27, 4, 6 },
    { AV1_WEDGE_OBLIQUE153, 4, 2 }, { AV1_WEDGE_OBLIQUE153, 4, 6 },
    { AV1_WEDGE_OBLIQUE63, 2, 4 },  { AV1_WEDGE_OBLIQUE63, 6, 4 },
    { AV1_WEDGE_OBLIQUE117, 2, 4 }, { AV1_WEDGE_OBLIQUE117, 6, 4 }
};

static const uint8_t av1_interintra_weights[128] = {
    60, 58, 56, 54, 52, 50, 48, 47, 45, 44, 42, 41, 39, 38, 37, 35,
    34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 22, 21, 20,
    19, 19, 18, 18, 17, 16, 16, 15, 15, 14, 14, 13, 13, 12, 12, 12,
    11, 11, 10, 10, 10, 9,  9,  9,  8,  8,  8,  8,  7,  7,  7,  7,
    6,  6,  6,  6,  6,  5,  5,  5,  5,  5,  4,  4,  4,  4,  4,  4,
    4,  4,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1
};

static const uint8_t av1_obmc_mask_1[1] = { 64 };
static const uint8_t av1_obmc_mask_2[2] = { 45, 64 };
static const uint8_t av1_obmc_mask_4[4] = { 39, 50, 59, 64 };
static const uint8_t av1_obmc_mask_8[8] = {
    36, 42, 48, 53, 57, 61, 64, 64
};
static const uint8_t av1_obmc_mask_16[16] = {
    34, 37, 40, 43, 46, 49, 52, 54,
    56, 58, 60, 61, 64, 64, 64, 64
};
static const uint8_t av1_obmc_mask_32[32] = {
    33, 35, 36, 38, 40, 41, 43, 44,
    45, 47, 48, 50, 51, 52, 53, 55,
    56, 57, 58, 59, 60, 60, 61, 62,
    64, 64, 64, 64, 64, 64, 64, 64
};

static int32_t av1_round_power_of_two(int64_t value, uint8_t bits) {
    if (bits == 0) {
        return (int32_t)value;
    }
    return (int32_t)((value + ((int64_t)1 << (bits - 1))) >> bits);
}

static uint16_t av1_clip_pixel(int32_t value, uint8_t bit_depth) {
    const int32_t maximum = ((int32_t)1 << bit_depth) - 1;
    if (value < 0) {
        return 0;
    }
    if (value > maximum) {
        return (uint16_t)maximum;
    }
    return (uint16_t)value;
}

static uint32_t av1_clamp_coordinate(int32_t value, uint32_t limit) {
    if (value < 0) {
        return 0;
    }
    if ((uint32_t)value >= limit) {
        return limit - 1;
    }
    return (uint32_t)value;
}

static Av1FilterKernel av1_filter_kernel(uint8_t filter, uint32_t dimension) {
    Av1FilterKernel kernel = { av1_sub_pel_filters_8, 8 };
    if (filter == AV1_INTERP_EIGHTTAP_SMOOTH) {
        kernel.coeffs = dimension <= 4 ? av1_sub_pel_filters_4smooth
                                       : av1_sub_pel_filters_8smooth;
    } else if (filter == AV1_INTERP_EIGHTTAP_SHARP) {
        kernel.coeffs = dimension <= 4 ? av1_sub_pel_filters_4
                                       : av1_sub_pel_filters_8sharp;
    } else if (filter == AV1_INTERP_BILINEAR) {
        kernel.coeffs = av1_bilinear_filters;
    } else if (dimension <= 4) {
        kernel.coeffs = av1_sub_pel_filters_4;
    }
    return kernel;
}

static int32_t av1_scale_factor(uint32_t reference, uint32_t current) {
    return (int32_t)((((uint64_t)reference << AV1_REF_SCALE_SHIFT) +
                      current / 2) /
                     current);
}

static int32_t av1_scaled_position(int32_t value_q4, int32_t scale) {
    const int64_t offset =
        (int64_t)(scale - (1 << AV1_REF_SCALE_SHIFT)) *
        (1 << (AV1_SUBPEL_BITS - 1));
    return av1_round_power_of_two(
        (int64_t)value_q4 * scale + offset,
        AV1_REF_SCALE_SHIFT - AV1_SCALE_EXTRA_BITS);
}

static int av1_valid_prediction_params(const Av1InterPredictParams *params) {
    if (params == NULL || params->src == NULL || params->dst == NULL ||
        params->src_width == 0 || params->src_height == 0 ||
        params->frame_width == 0 || params->frame_height == 0 ||
        params->block_width == 0 || params->block_height == 0 ||
        params->bit_depth < 8 || params->bit_depth > 12 ||
        params->filter_x > AV1_INTERP_BILINEAR ||
        params->filter_y > AV1_INTERP_BILINEAR ||
        params->subsampling_x > 1 || params->subsampling_y > 1 ||
        params->src_stride < params->src_width ||
        params->dst_stride < params->block_width) {
        return 0;
    }
    return 2 * params->frame_width >= params->src_width &&
           2 * params->frame_height >= params->src_height &&
           params->frame_width <= 16 * params->src_width &&
           params->frame_height <= 16 * params->src_height;
}

static AvifdecStatus av1_inter_convolve(const Av1InterPredictParams *params,
                                        uint16_t *compound,
                                        size_t compound_stride,
                                        int is_compound) {
    Av1FilterKernel filter_x;
    Av1FilterKernel filter_y;
    int32_t scale_x;
    int32_t scale_y;
    int32_t step_x;
    int32_t step_y;
    int32_t position_x;
    int32_t position_y;
    uint8_t round0;
    uint8_t round1;
    uint8_t bits;
    uint32_t y;
    if (!av1_valid_prediction_params(params) ||
        (is_compound &&
         (compound == NULL || compound_stride < params->block_width))) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    filter_x = av1_filter_kernel(params->filter_x, params->block_width);
    filter_y = av1_filter_kernel(params->filter_y, params->block_height);
    scale_x = av1_scale_factor(params->src_width, params->frame_width);
    scale_y = av1_scale_factor(params->src_height, params->frame_height);
    step_x = av1_round_power_of_two(
        scale_x, AV1_REF_SCALE_SHIFT - AV1_SCALE_SUBPEL_BITS);
    step_y = av1_round_power_of_two(
        scale_y, AV1_REF_SCALE_SHIFT - AV1_SCALE_SUBPEL_BITS);
    position_x = av1_scaled_position(
                     ((int32_t)params->block_x << AV1_SUBPEL_BITS) +
                         params->mv_col *
                             (1 << (1 - params->subsampling_x)),
                     scale_x) +
                 (1 << (AV1_SCALE_EXTRA_BITS - 1));
    position_y = av1_scaled_position(
                     ((int32_t)params->block_y << AV1_SUBPEL_BITS) +
                         params->mv_row *
                             (1 << (1 - params->subsampling_y)),
                     scale_y) +
                 (1 << (AV1_SCALE_EXTRA_BITS - 1));
    round0 = AV1_ROUND0_BITS;
    if (params->bit_depth + AV1_FILTER_BITS - round0 + 2 > 16) {
        round0 = (uint8_t)(params->bit_depth + AV1_FILTER_BITS + 2 - 16);
    }
    round1 = is_compound ? AV1_COMPOUND_ROUND1_BITS
                         : (uint8_t)(2 * AV1_FILTER_BITS - round0);
    bits = (uint8_t)(2 * AV1_FILTER_BITS - round0 - round1);
    for (y = 0; y < params->block_height; ++y) {
        uint32_t x;
        const int32_t y_qn = position_y + (int32_t)y * step_y;
        const int32_t source_y = y_qn >> AV1_SCALE_SUBPEL_BITS;
        const uint8_t phase_y =
            (uint8_t)((y_qn & AV1_SCALE_SUBPEL_MASK) >>
                      AV1_SCALE_EXTRA_BITS);
        const int16_t *coeff_y = filter_y.coeffs[phase_y];
        for (x = 0; x < params->block_width; ++x) {
            int32_t intermediate[8];
            const int32_t x_qn = position_x + (int32_t)x * step_x;
            const int32_t source_x = x_qn >> AV1_SCALE_SUBPEL_BITS;
            const uint8_t phase_x =
                (uint8_t)((x_qn & AV1_SCALE_SUBPEL_MASK) >>
                          AV1_SCALE_EXTRA_BITS);
            const int16_t *coeff_x = filter_x.coeffs[phase_x];
            uint8_t ky;
            int64_t vertical_sum;
            for (ky = 0; ky < filter_y.taps; ++ky) {
                const uint32_t sample_y = av1_clamp_coordinate(
                    source_y + ky - (filter_y.taps / 2 - 1),
                    params->src_height);
                int64_t horizontal_sum =
                    (int64_t)1
                    << (params->bit_depth + AV1_FILTER_BITS - 1);
                uint8_t kx;
                for (kx = 0; kx < filter_x.taps; ++kx) {
                    const uint32_t sample_x = av1_clamp_coordinate(
                        source_x + kx - (filter_x.taps / 2 - 1),
                        params->src_width);
                    horizontal_sum +=
                        (int32_t)coeff_x[kx] *
                        params->src[(size_t)sample_y * params->src_stride +
                                    sample_x];
                }
                intermediate[ky] =
                    av1_round_power_of_two(horizontal_sum, round0);
            }
            vertical_sum =
                (int64_t)1
                << (params->bit_depth + 2 * AV1_FILTER_BITS - round0);
            for (ky = 0; ky < filter_y.taps; ++ky) {
                vertical_sum += (int64_t)coeff_y[ky] * intermediate[ky];
            }
            if (is_compound) {
                compound[(size_t)y * compound_stride + x] =
                    (uint16_t)av1_round_power_of_two(vertical_sum, round1);
            } else {
                int32_t value =
                    av1_round_power_of_two(vertical_sum, round1);
                value -=
                    (1 << (params->bit_depth + 2 * AV1_FILTER_BITS - round0 -
                           round1)) +
                    (1 << (params->bit_depth + 2 * AV1_FILTER_BITS - round0 -
                           round1 - 1));
                params->dst[(size_t)y * params->dst_stride + x] =
                    av1_clip_pixel(av1_round_power_of_two(value, bits),
                                   params->bit_depth);
            }
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_inter_predict_single(const Av1InterPredictParams *params) {
    return av1_inter_convolve(params, NULL, 0, 0);
}

AvifdecStatus av1_inter_predict_compound(const Av1InterPredictParams *params,
                                         uint16_t *compound,
                                         size_t compound_stride) {
    return av1_inter_convolve(params, compound, compound_stride, 1);
}

static AvifdecStatus av1_inter_blend_compound(
    const Av1CompoundParams *params,
    const uint16_t *pred0,
    size_t pred0_stride,
    const uint16_t *pred1,
    size_t pred1_stride,
    int masked) {
    uint8_t round0 = AV1_ROUND0_BITS;
    uint8_t bits;
    uint8_t offset_bits;
    uint32_t y;
    if (params == NULL || params->dst == NULL || pred0 == NULL ||
        pred1 == NULL || params->width == 0 || params->height == 0 ||
        params->bit_depth < 8 || params->bit_depth > 12 ||
        params->dst_stride < params->width ||
        pred0_stride < params->width || pred1_stride < params->width ||
        (masked &&
         (params->mask == NULL || params->mask_stride < params->width)) ||
        (!masked && params->weight0 + params->weight1 != 16)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (params->bit_depth + AV1_FILTER_BITS - round0 + 2 > 16) {
        round0 = (uint8_t)(params->bit_depth + AV1_FILTER_BITS + 2 - 16);
    }
    bits =
        (uint8_t)(2 * AV1_FILTER_BITS - round0 - AV1_COMPOUND_ROUND1_BITS);
    offset_bits =
        (uint8_t)(params->bit_depth + 2 * AV1_FILTER_BITS - round0);
    for (y = 0; y < params->height; ++y) {
        uint32_t x;
        for (x = 0; x < params->width; ++x) {
            int32_t value;
            const uint16_t first = pred0[(size_t)y * pred0_stride + x];
            const uint16_t second = pred1[(size_t)y * pred1_stride + x];
            if (masked) {
                const uint8_t mask =
                    params->mask[(size_t)y * params->mask_stride + x];
                value =
                    ((int32_t)mask * first + (64 - mask) * second) >> 6;
            } else {
                value =
                    ((int32_t)first * params->weight0 +
                     (int32_t)second * params->weight1) >>
                    4;
            }
            value -=
                (1 << (offset_bits - AV1_COMPOUND_ROUND1_BITS)) +
                (1 << (offset_bits - AV1_COMPOUND_ROUND1_BITS - 1));
            params->dst[(size_t)y * params->dst_stride + x] =
                av1_clip_pixel(av1_round_power_of_two(value, bits),
                               params->bit_depth);
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_inter_blend_average(const Av1CompoundParams *params,
                                      const uint16_t *pred0,
                                      size_t pred0_stride,
                                      const uint16_t *pred1,
                                      size_t pred1_stride) {
    return av1_inter_blend_compound(
        params, pred0, pred0_stride, pred1, pred1_stride, 0);
}

AvifdecStatus av1_inter_blend_masked(const Av1CompoundParams *params,
                                     const uint16_t *pred0,
                                     size_t pred0_stride,
                                     const uint16_t *pred1,
                                     size_t pred1_stride) {
    return av1_inter_blend_compound(
        params, pred0, pred0_stride, pred1, pred1_stride, 1);
}

AvifdecStatus av1_inter_build_diff_mask(uint8_t *mask,
                                        size_t mask_stride,
                                        const uint16_t *pred0,
                                        size_t pred0_stride,
                                        const uint16_t *pred1,
                                        size_t pred1_stride,
                                        uint32_t width,
                                        uint32_t height,
                                        uint8_t bit_depth,
                                        uint8_t inverse) {
    uint8_t round0 = AV1_ROUND0_BITS;
    uint8_t round;
    uint32_t y;
    if (mask == NULL || pred0 == NULL || pred1 == NULL || width == 0 ||
        height == 0 || bit_depth < 8 || bit_depth > 12 ||
        mask_stride < width || pred0_stride < width || pred1_stride < width ||
        inverse > 1) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (bit_depth + AV1_FILTER_BITS - round0 + 2 > 16) {
        round0 = (uint8_t)(bit_depth + AV1_FILTER_BITS + 2 - 16);
    }
    round = (uint8_t)(2 * AV1_FILTER_BITS - round0 -
                      AV1_COMPOUND_ROUND1_BITS + bit_depth - 8);
    for (y = 0; y < height; ++y) {
        uint32_t x;
        for (x = 0; x < width; ++x) {
            int32_t difference =
                (int32_t)pred0[(size_t)y * pred0_stride + x] -
                pred1[(size_t)y * pred1_stride + x];
            int32_t value;
            if (difference < 0) {
                difference = -difference;
            }
            difference = av1_round_power_of_two(difference, round);
            value = AV1_DIFF_MASK_BASE +
                    (difference >> AV1_DIFF_FACTOR_LOG2);
            if (value > 64) {
                value = 64;
            }
            mask[(size_t)y * mask_stride + x] =
                (uint8_t)(inverse ? 64 - value : value);
        }
    }
    return AVIFDEC_OK;
}

static uint8_t av1_wedge_oblique63(uint32_t row, uint32_t column) {
    const uint8_t *prototype = (row & 1) != 0
                                   ? av1_wedge_master_oblique_odd
                                   : av1_wedge_master_oblique_even;
    const int32_t shift = 16 - (int32_t)((row + 1) / 2);
    int32_t source = (int32_t)column - shift;
    if (source < 0) {
        source = 0;
    } else if (source >= AV1_WEDGE_MASTER_SIZE) {
        source = AV1_WEDGE_MASTER_SIZE - 1;
    }
    return prototype[source];
}

static uint8_t av1_wedge_master_value(uint8_t direction,
                                      uint8_t negative,
                                      uint32_t row,
                                      uint32_t column) {
    uint8_t value;
    if (direction == AV1_WEDGE_VERTICAL) {
        value = av1_wedge_master_vertical[column];
    } else if (direction == AV1_WEDGE_HORIZONTAL) {
        value = av1_wedge_master_vertical[row];
    } else if (direction == AV1_WEDGE_OBLIQUE27) {
        value = av1_wedge_oblique63(column, row);
    } else if (direction == AV1_WEDGE_OBLIQUE117) {
        value = (uint8_t)(64 - av1_wedge_oblique63(
                                  row, AV1_WEDGE_MASTER_SIZE - 1 - column));
    } else if (direction == AV1_WEDGE_OBLIQUE153) {
        value = (uint8_t)(64 - av1_wedge_oblique63(
                                  column, AV1_WEDGE_MASTER_SIZE - 1 - row));
    } else {
        value = av1_wedge_oblique63(row, column);
    }
    return negative ? (uint8_t)(64 - value) : value;
}

static const Av1WedgeCode *av1_wedge_codebook(uint32_t width,
                                              uint32_t height) {
    if (height > width) {
        return av1_wedge_codebook_hgtw;
    }
    if (height < width) {
        return av1_wedge_codebook_hltw;
    }
    return av1_wedge_codebook_heqw;
}

static int av1_wedge_signflip(uint32_t width,
                              uint32_t height,
                              uint8_t wedge_index) {
    if (wedge_index == 10 || wedge_index == 14) {
        return 0;
    }
    if (wedge_index == 4 && width != height) {
        return 0;
    }
    if (width == 8 && height == 32 && wedge_index == 8) {
        return 0;
    }
    if (width == 32 && height == 8 && wedge_index == 12) {
        return 0;
    }
    return 1;
}

AvifdecStatus av1_inter_build_wedge_mask(uint8_t *mask,
                                         size_t mask_stride,
                                         uint32_t width,
                                         uint32_t height,
                                         uint8_t wedge_index,
                                         uint8_t wedge_sign) {
    const Av1WedgeCode *codebook;
    const Av1WedgeCode *code;
    uint32_t offset_x;
    uint32_t offset_y;
    uint8_t negative;
    uint32_t y;
    if (mask == NULL || mask_stride < width || wedge_index >= 16 ||
        wedge_sign > 1 ||
        !((width == 8 && (height == 8 || height == 16 || height == 32)) ||
          (width == 16 && (height == 8 || height == 16 || height == 32)) ||
          (width == 32 && (height == 8 || height == 16 || height == 32)))) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    codebook = av1_wedge_codebook(width, height);
    code = &codebook[wedge_index];
    offset_x = (uint32_t)code->x_offset * width / 8;
    offset_y = (uint32_t)code->y_offset * height / 8;
    negative =
        (uint8_t)(wedge_sign ^
                  av1_wedge_signflip(width, height, wedge_index));
    for (y = 0; y < height; ++y) {
        uint32_t x;
        for (x = 0; x < width; ++x) {
            mask[(size_t)y * mask_stride + x] = av1_wedge_master_value(
                code->direction, negative,
                AV1_WEDGE_MASTER_SIZE / 2 - offset_y + y,
                AV1_WEDGE_MASTER_SIZE / 2 - offset_x + x);
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_inter_build_interintra_mask(uint8_t *mask,
                                              size_t mask_stride,
                                              uint32_t width,
                                              uint32_t height,
                                              uint8_t mode) {
    uint32_t scale;
    uint32_t y;
    if (mask == NULL || mask_stride < width || width < 4 || height < 4 ||
        width > 128 || height > 128 || mode > AV1_INTERINTRA_SMOOTH) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    scale = 128 / (width > height ? width : height);
    for (y = 0; y < height; ++y) {
        uint32_t x;
        for (x = 0; x < width; ++x) {
            uint8_t value = 32;
            if (mode == AV1_INTERINTRA_VERTICAL) {
                value = av1_interintra_weights[y * scale];
            } else if (mode == AV1_INTERINTRA_HORIZONTAL) {
                value = av1_interintra_weights[x * scale];
            } else if (mode == AV1_INTERINTRA_SMOOTH) {
                value = av1_interintra_weights[(x < y ? x : y) * scale];
            }
            mask[(size_t)y * mask_stride + x] = value;
        }
    }
    return AVIFDEC_OK;
}

void av1_inter_blend_interintra(uint16_t *dst,
                                size_t dst_stride,
                                const uint16_t *inter,
                                size_t inter_stride,
                                const uint16_t *intra,
                                size_t intra_stride,
                                const uint8_t *mask,
                                size_t mask_stride,
                                uint32_t width,
                                uint32_t height) {
    uint32_t y;
    for (y = 0; y < height; ++y) {
        uint32_t x;
        for (x = 0; x < width; ++x) {
            const uint8_t weight = mask[(size_t)y * mask_stride + x];
            dst[(size_t)y * dst_stride + x] =
                (uint16_t)av1_round_power_of_two(
                    (int32_t)weight *
                            intra[(size_t)y * intra_stride + x] +
                        (64 - weight) *
                            inter[(size_t)y * inter_stride + x],
                    6);
        }
    }
}

AvifdecStatus av1_inter_blend_obmc(uint16_t *dst,
                                   size_t dst_stride,
                                   const uint16_t *neighbor,
                                   size_t neighbor_stride,
                                   uint32_t width,
                                   uint32_t height,
                                   int from_left) {
    const uint8_t *mask;
    uint32_t length;
    uint32_t y;

    if (dst == NULL || neighbor == NULL || width == 0U || height == 0U ||
        dst_stride < width || neighbor_stride < width ||
        (from_left != 0 && from_left != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    length = from_left ? width : height;
    if (length == 1U) mask = av1_obmc_mask_1;
    else if (length == 2U) mask = av1_obmc_mask_2;
    else if (length == 4U) mask = av1_obmc_mask_4;
    else if (length == 8U) mask = av1_obmc_mask_8;
    else if (length == 16U) mask = av1_obmc_mask_16;
    else if (length == 32U) mask = av1_obmc_mask_32;
    else return AVIFDEC_INVALID_ARGUMENT;

    for (y = 0U; y < height; ++y) {
        uint32_t x;
        for (x = 0U; x < width; ++x) {
            const uint32_t weight = mask[from_left ? x : y];
            dst[(size_t)y * dst_stride + x] = (uint16_t)(
                (weight * dst[(size_t)y * dst_stride + x] +
                 (64U - weight) *
                     neighbor[(size_t)y * neighbor_stride + x] +
                 32U) >>
                6);
        }
    }
    return AVIFDEC_OK;
}
