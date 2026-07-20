#include "av1_warp.h"

#define AV1_WARP_FILTER_BITS 7
#define AV1_WARP_PIXEL_PREC_BITS 6
#define AV1_WARP_PIXEL_PREC_SHIFTS (1 << AV1_WARP_PIXEL_PREC_BITS)
#define AV1_WARP_DIFF_PREC_BITS \
    (AV1_WARP_MODEL_PREC_BITS - AV1_WARP_PIXEL_PREC_BITS)
#define AV1_WARP_PARAM_REDUCE_BITS 6
#define AV1_WARP_DIV_LUT_BITS 8
#define AV1_WARP_DIV_LUT_PREC_BITS 14
#define AV1_WARP_TRANS_CLAMP (1 << 23)
#define AV1_WARP_AFFINE_CLAMP (1 << 13)
#define AV1_WARP_LS_MV_MAX 256
#define AV1_WARP_LS_MAT_MIN (-(1 << 22))
#define AV1_WARP_LS_MAT_MAX ((1 << 22) - 1)

#include "av1_warp_tables.inc"

static int64_t av1_warp_floor_shift(int64_t value, unsigned int bits) {
    uint64_t magnitude;
    uint64_t mask;

    if (bits == 0U) return value;
    if (value >= 0) return value >> bits;
    magnitude = 0U - (uint64_t)value;
    mask = ((uint64_t)1 << bits) - 1U;
    return -(int64_t)((magnitude + mask) >> bits);
}

static int64_t av1_warp_round(int64_t value, unsigned int bits) {
    if (bits == 0U) return value;
    return av1_warp_floor_shift(
        value + ((int64_t)1 << (bits - 1U)), bits);
}

static int64_t av1_warp_round_signed(int64_t value, unsigned int bits) {
    uint64_t magnitude;
    uint64_t rounded;

    if (bits == 0U) return value;
    if (value >= 0) {
        return (value + ((int64_t)1 << (bits - 1U))) >> bits;
    }
    magnitude = 0U - (uint64_t)value;
    rounded = (magnitude + ((uint64_t)1 << (bits - 1U))) >> bits;
    return -(int64_t)rounded;
}

static int32_t av1_warp_clip_i32(int64_t value,
                                  int32_t minimum,
                                  int32_t maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return (int32_t)value;
}

static uint16_t av1_warp_clip_pixel(int64_t value, uint8_t bit_depth) {
    int64_t maximum = ((int64_t)1 << bit_depth) - 1;

    if (value < 0) return 0;
    if (value > maximum) return (uint16_t)maximum;
    return (uint16_t)value;
}

static uint32_t av1_warp_clamp_coordinate(int64_t value, uint32_t limit) {
    if (value < 0) return 0U;
    if ((uint64_t)value >= limit) return limit - 1U;
    return (uint32_t)value;
}

static uint64_t av1_warp_magnitude_i64(int64_t value) {
    return value < 0 ? 0U - (uint64_t)value : (uint64_t)value;
}

static unsigned int av1_warp_floor_log2(uint64_t value) {
    unsigned int result = 0U;

    while (value > 1U) {
        value >>= 1U;
        ++result;
    }
    return result;
}

Av1WarpStatus av1_warp_resolve_divisor(int64_t divisor,
                                        uint8_t *shift,
                                        int32_t *factor) {
    uint64_t magnitude;
    uint64_t remainder;
    uint64_t index;
    unsigned int msb;

    if (divisor == 0 || shift == 0 || factor == 0) {
        return AV1_WARP_INVALID_ARGUMENT;
    }
    magnitude = av1_warp_magnitude_i64(divisor);
    msb = av1_warp_floor_log2(magnitude);
    remainder = magnitude - ((uint64_t)1 << msb);
    if (msb > AV1_WARP_DIV_LUT_BITS) {
        index = (uint64_t)av1_warp_round(
            (int64_t)remainder, msb - AV1_WARP_DIV_LUT_BITS);
    } else {
        index = remainder << (AV1_WARP_DIV_LUT_BITS - msb);
    }
    *shift = (uint8_t)(msb + AV1_WARP_DIV_LUT_PREC_BITS);
    *factor = divisor < 0 ? -(int32_t)av1_warp_div_lut[index]
                          : (int32_t)av1_warp_div_lut[index];
    return AV1_WARP_OK;
}

static int av1_warp_model_components_valid(const Av1WarpModel *model) {
    return model != 0 && model->matrix[2] > 0;
}

static int32_t av1_warp_reduce_shear(int32_t value) {
    return (int32_t)av1_warp_round_signed(
               value, AV1_WARP_PARAM_REDUCE_BITS) *
           (1 << AV1_WARP_PARAM_REDUCE_BITS);
}

Av1WarpStatus av1_warp_derive_shear(const Av1WarpModel *model,
                                    Av1WarpShear *shear) {
    const int32_t *matrix;
    uint8_t div_shift;
    int32_t div_factor;
    int64_t product;
    int64_t value;

    if (shear == 0 || !av1_warp_model_components_valid(model)) {
        return model == 0 || shear == 0 ? AV1_WARP_INVALID_ARGUMENT
                                       : AV1_WARP_INVALID_MODEL;
    }
    matrix = model->matrix;
    if (av1_warp_resolve_divisor(
            matrix[2], &div_shift, &div_factor) != AV1_WARP_OK) {
        return AV1_WARP_INVALID_MODEL;
    }
    product = (int64_t)matrix[3] * matrix[4];
    if (av1_warp_magnitude_i64(product) >
        (uint64_t)INT64_MAX / (uint32_t)div_factor) {
        return AV1_WARP_INVALID_MODEL;
    }
    shear->alpha = av1_warp_clip_i32(
        (int64_t)matrix[2] - (1 << AV1_WARP_MODEL_PREC_BITS),
        -32768, 32767);
    shear->beta = av1_warp_clip_i32(matrix[3], -32768, 32767);
    value = (int64_t)matrix[4] *
            (1 << AV1_WARP_MODEL_PREC_BITS) * div_factor;
    shear->gamma = av1_warp_clip_i32(
        av1_warp_round_signed(value, div_shift), -32768, 32767);
    value = product * div_factor;
    shear->delta = av1_warp_clip_i32(
        (int64_t)matrix[5] -
            av1_warp_round_signed(value, div_shift) -
            (1 << AV1_WARP_MODEL_PREC_BITS),
        -32768, 32767);
    shear->alpha = av1_warp_reduce_shear(shear->alpha);
    shear->beta = av1_warp_reduce_shear(shear->beta);
    shear->gamma = av1_warp_reduce_shear(shear->gamma);
    shear->delta = av1_warp_reduce_shear(shear->delta);
    if (4 * (shear->alpha < 0 ? -shear->alpha : shear->alpha) +
            7 * (shear->beta < 0 ? -shear->beta : shear->beta) >=
        (1 << AV1_WARP_MODEL_PREC_BITS)) {
        return AV1_WARP_INVALID_MODEL;
    }
    if (4 * (shear->gamma < 0 ? -shear->gamma : shear->gamma) +
            4 * (shear->delta < 0 ? -shear->delta : shear->delta) >=
        (1 << AV1_WARP_MODEL_PREC_BITS)) {
        return AV1_WARP_INVALID_MODEL;
    }
    return AV1_WARP_OK;
}

int av1_warp_model_is_valid(const Av1WarpModel *model) {
    Av1WarpShear shear;

    return av1_warp_derive_shear(model, &shear) == AV1_WARP_OK;
}

static int64_t av1_warp_ls_product(int64_t left, int64_t right) {
    return av1_warp_floor_shift(left * right, 2U) + left + right;
}

static int av1_warp_ls_value_valid(int64_t value) {
    return value >= AV1_WARP_LS_MAT_MIN &&
           value <= AV1_WARP_LS_MAT_MAX;
}

static int32_t av1_warp_project_component(int64_t numerator,
                                          int64_t factor,
                                          unsigned int shift,
                                          int diagonal) {
    int64_t value = av1_warp_round_signed(numerator * factor, shift);

    if (diagonal) {
        return av1_warp_clip_i32(
            value,
            (1 << AV1_WARP_MODEL_PREC_BITS) -
                AV1_WARP_AFFINE_CLAMP + 1,
            (1 << AV1_WARP_MODEL_PREC_BITS) +
                AV1_WARP_AFFINE_CLAMP - 1);
    }
    return av1_warp_clip_i32(value, -AV1_WARP_AFFINE_CLAMP + 1,
                             AV1_WARP_AFFINE_CLAMP - 1);
}

Av1WarpStatus av1_warp_project_samples(Av1WarpModel *model,
                                       const Av1WarpSample *samples,
                                       size_t sample_count,
                                       int32_t center_x,
                                       int32_t center_y,
                                       int32_t mv_x,
                                       int32_t mv_y) {
    int64_t matrix_a00 = 0;
    int64_t matrix_a01 = 0;
    int64_t matrix_a11 = 0;
    int64_t vector_bx0 = 0;
    int64_t vector_bx1 = 0;
    int64_t vector_by0 = 0;
    int64_t vector_by1 = 0;
    int64_t source_center_x;
    int64_t source_center_y;
    int64_t destination_center_x;
    int64_t destination_center_y;
    int64_t determinant;
    int64_t divisor_factor;
    unsigned int divisor_shift;
    uint8_t resolved_shift;
    int32_t resolved_factor;
    Av1WarpModel projected;
    Av1WarpShear shear;
    size_t index;

    if (model == 0 || samples == 0 || sample_count == 0U ||
        sample_count > AV1_WARP_MAX_SAMPLES) {
        return AV1_WARP_INVALID_ARGUMENT;
    }
    source_center_x =
        (int64_t)center_x * (1 << AV1_WARP_SAMPLE_PREC_BITS);
    source_center_y =
        (int64_t)center_y * (1 << AV1_WARP_SAMPLE_PREC_BITS);
    destination_center_x = source_center_x + mv_x;
    destination_center_y = source_center_y + mv_y;
    for (index = 0U; index < sample_count; ++index) {
        int64_t source_x = samples[index].source_x - source_center_x;
        int64_t source_y = samples[index].source_y - source_center_y;
        int64_t destination_x =
            samples[index].destination_x - destination_center_x;
        int64_t destination_y =
            samples[index].destination_y - destination_center_y;
        int64_t difference_x = source_x - destination_x;
        int64_t difference_y = source_y - destination_y;

        if (difference_x <= -AV1_WARP_LS_MV_MAX ||
            difference_x >= AV1_WARP_LS_MV_MAX ||
            difference_y <= -AV1_WARP_LS_MV_MAX ||
            difference_y >= AV1_WARP_LS_MV_MAX) {
            continue;
        }
        matrix_a00 += av1_warp_ls_product(source_x, source_x) + 8;
        matrix_a01 += av1_warp_ls_product(source_x, source_y) + 4;
        matrix_a11 += av1_warp_ls_product(source_y, source_y) + 8;
        vector_bx0 +=
            av1_warp_ls_product(source_x, destination_x) + 8;
        vector_bx1 +=
            av1_warp_ls_product(source_y, destination_x) + 4;
        vector_by0 +=
            av1_warp_ls_product(source_x, destination_y) + 4;
        vector_by1 +=
            av1_warp_ls_product(source_y, destination_y) + 8;
        if (!av1_warp_ls_value_valid(matrix_a00) ||
            !av1_warp_ls_value_valid(matrix_a01) ||
            !av1_warp_ls_value_valid(matrix_a11) ||
            !av1_warp_ls_value_valid(vector_bx0) ||
            !av1_warp_ls_value_valid(vector_bx1) ||
            !av1_warp_ls_value_valid(vector_by0) ||
            !av1_warp_ls_value_valid(vector_by1)) {
            return AV1_WARP_INVALID_ARGUMENT;
        }
    }
    determinant =
        matrix_a00 * matrix_a11 - matrix_a01 * matrix_a01;
    if (determinant == 0) return AV1_WARP_SINGULAR;
    if (av1_warp_resolve_divisor(
            determinant, &resolved_shift, &resolved_factor) != AV1_WARP_OK) {
        return AV1_WARP_SINGULAR;
    }
    divisor_shift = resolved_shift;
    divisor_factor = resolved_factor;
    if (divisor_shift < AV1_WARP_MODEL_PREC_BITS) {
        divisor_factor *=
            (int64_t)1 << (AV1_WARP_MODEL_PREC_BITS - divisor_shift);
        divisor_shift = 0U;
    } else {
        divisor_shift -= AV1_WARP_MODEL_PREC_BITS;
    }
    projected.matrix[2] = av1_warp_project_component(
        matrix_a11 * vector_bx0 - matrix_a01 * vector_bx1,
        divisor_factor, divisor_shift, 1);
    projected.matrix[3] = av1_warp_project_component(
        -matrix_a01 * vector_bx0 + matrix_a00 * vector_bx1,
        divisor_factor, divisor_shift, 0);
    projected.matrix[4] = av1_warp_project_component(
        matrix_a11 * vector_by0 - matrix_a01 * vector_by1,
        divisor_factor, divisor_shift, 0);
    projected.matrix[5] = av1_warp_project_component(
        -matrix_a01 * vector_by0 + matrix_a00 * vector_by1,
        divisor_factor, divisor_shift, 1);
    projected.matrix[0] = av1_warp_clip_i32(
        (int64_t)mv_x *
                (1 << (AV1_WARP_MODEL_PREC_BITS -
                       AV1_WARP_SAMPLE_PREC_BITS)) -
            ((int64_t)center_x *
                 (projected.matrix[2] -
                  (1 << AV1_WARP_MODEL_PREC_BITS)) +
             (int64_t)center_y * projected.matrix[3]),
        -AV1_WARP_TRANS_CLAMP, AV1_WARP_TRANS_CLAMP - 1);
    projected.matrix[1] = av1_warp_clip_i32(
        (int64_t)mv_y *
                (1 << (AV1_WARP_MODEL_PREC_BITS -
                       AV1_WARP_SAMPLE_PREC_BITS)) -
            ((int64_t)center_x * projected.matrix[4] +
             (int64_t)center_y *
                 (projected.matrix[5] -
                  (1 << AV1_WARP_MODEL_PREC_BITS))),
        -AV1_WARP_TRANS_CLAMP, AV1_WARP_TRANS_CLAMP - 1);
    if (av1_warp_derive_shear(&projected, &shear) != AV1_WARP_OK) {
        return AV1_WARP_INVALID_MODEL;
    }
    *model = projected;
    return AV1_WARP_OK;
}

static Av1WarpStatus av1_warp_validate_plane(
    const Av1WarpPlaneParams *params,
    uint16_t *output,
    size_t output_stride,
    Av1WarpShear *shear) {
    if (params == 0 || output == 0 || shear == 0 ||
        params->source == 0 || params->model == 0 ||
        params->source_width == 0U || params->source_height == 0U ||
        params->block_width == 0U || params->block_height == 0U ||
        params->source_stride < params->source_width ||
        output_stride < params->block_width ||
        params->subsampling_x > 1U || params->subsampling_y > 1U ||
        (params->bit_depth != 8U && params->bit_depth != 10U &&
         params->bit_depth != 12U) ||
        params->source_width > 0x7fffffffU ||
        params->source_height > 0x7fffffffU ||
        params->block_width > 0x7ffffffbU ||
        params->block_height > 0x7ffffffbU ||
        params->block_x > 0x7ffffffbU - params->block_width ||
        params->block_y > 0x7ffffffbU - params->block_height) {
        return AV1_WARP_INVALID_ARGUMENT;
    }
    return av1_warp_derive_shear(params->model, shear);
}

static Av1WarpStatus av1_warp_predict(const Av1WarpPlaneParams *params,
                                      uint16_t *output,
                                      size_t output_stride,
                                      int compound) {
    Av1WarpShear shear;
    Av1WarpStatus status =
        av1_warp_validate_plane(params, output, output_stride, &shear);
    unsigned int round0;
    unsigned int round1;
    uint32_t block_row;

    if (status != AV1_WARP_OK) return status;
    round0 = params->bit_depth == 12U ? 5U : 3U;
    round1 = compound ? 7U : (params->bit_depth == 12U ? 9U : 11U);
    for (block_row = 0U; block_row < params->block_height;
         block_row += 8U) {
        uint32_t block_column;
        for (block_column = 0U; block_column < params->block_width;
             block_column += 8U) {
            int32_t intermediate[15][8];
            int64_t source_x =
                ((int64_t)params->block_x + block_column + 4)
                << params->subsampling_x;
            int64_t source_y =
                ((int64_t)params->block_y + block_row + 4)
                << params->subsampling_y;
            const int32_t *matrix = params->model->matrix;
            int64_t destination_x =
                (int64_t)matrix[2] * source_x +
                (int64_t)matrix[3] * source_y + matrix[0];
            int64_t destination_y =
                (int64_t)matrix[4] * source_x +
                (int64_t)matrix[5] * source_y + matrix[1];
            int64_t x4 = av1_warp_floor_shift(
                destination_x, params->subsampling_x);
            int64_t y4 = av1_warp_floor_shift(
                destination_y, params->subsampling_y);
            int64_t integer_x =
                av1_warp_floor_shift(x4, AV1_WARP_MODEL_PREC_BITS);
            int64_t integer_y =
                av1_warp_floor_shift(y4, AV1_WARP_MODEL_PREC_BITS);
            int32_t fraction_x =
                (int32_t)((uint64_t)x4 &
                          ((1U << AV1_WARP_MODEL_PREC_BITS) - 1U));
            int32_t fraction_y =
                (int32_t)((uint64_t)y4 &
                          ((1U << AV1_WARP_MODEL_PREC_BITS) - 1U));
            int row;

            fraction_x -= 4 * shear.alpha + 4 * shear.beta;
            fraction_y -= 4 * shear.gamma + 4 * shear.delta;
            fraction_x =
                (int32_t)av1_warp_floor_shift(
                    fraction_x, AV1_WARP_PARAM_REDUCE_BITS) *
                (1 << AV1_WARP_PARAM_REDUCE_BITS);
            fraction_y =
                (int32_t)av1_warp_floor_shift(
                    fraction_y, AV1_WARP_PARAM_REDUCE_BITS) *
                (1 << AV1_WARP_PARAM_REDUCE_BITS);
            for (row = -7; row < 8; ++row) {
                uint32_t sample_y = av1_warp_clamp_coordinate(
                    integer_y + row, params->source_height);
                int column;
                for (column = -4; column < 4; ++column) {
                    int32_t phase = (int32_t)av1_warp_round(
                        fraction_x + shear.alpha * (column + 4) +
                            shear.beta * (row + 4),
                        AV1_WARP_DIFF_PREC_BITS);
                    int64_t sum =
                        (int64_t)1
                        << (params->bit_depth + AV1_WARP_FILTER_BITS - 1U);
                    unsigned int tap;

                    phase += AV1_WARP_PIXEL_PREC_SHIFTS;
                    for (tap = 0U; tap < 8U; ++tap) {
                        uint32_t sample_x = av1_warp_clamp_coordinate(
                            integer_x + column - 3 + tap,
                            params->source_width);
                        sum +=
                            (int64_t)av1_warped_filters[phase][tap] *
                            params->source[
                                (size_t)sample_y * params->source_stride +
                                sample_x];
                    }
                    intermediate[row + 7][column + 4] =
                        (int32_t)av1_warp_round(sum, round0);
                }
            }
            for (row = -4;
                 row < 4 &&
                 (int64_t)block_row + row + 4 <
                     params->block_height;
                 ++row) {
                int column;
                for (column = -4;
                     column < 4 &&
                     (int64_t)block_column + column + 4 <
                         params->block_width;
                     ++column) {
                    int32_t phase = (int32_t)av1_warp_round(
                        fraction_y + shear.gamma * (column + 4) +
                            shear.delta * (row + 4),
                        AV1_WARP_DIFF_PREC_BITS);
                    int64_t sum =
                        (int64_t)1
                        << (params->bit_depth +
                            2U * AV1_WARP_FILTER_BITS - round0);
                    unsigned int tap;
                    size_t output_index;

                    phase += AV1_WARP_PIXEL_PREC_SHIFTS;
                    for (tap = 0U; tap < 8U; ++tap) {
                        sum +=
                            (int64_t)av1_warped_filters[phase][tap] *
                            intermediate[row + (int)tap + 4][column + 4];
                    }
                    sum = av1_warp_round(sum, round1);
                    output_index =
                        (size_t)(block_row + (uint32_t)(row + 4)) *
                            output_stride +
                        block_column + (uint32_t)(column + 4);
                    if (compound) {
                        output[output_index] = (uint16_t)sum;
                    } else {
                        sum -=
                            ((int64_t)1
                             << (params->bit_depth +
                                 2U * AV1_WARP_FILTER_BITS - round0 -
                                 round1)) +
                            ((int64_t)1
                             << (params->bit_depth +
                                 2U * AV1_WARP_FILTER_BITS - round0 -
                                 round1 - 1U));
                        output[output_index] =
                            av1_warp_clip_pixel(sum, params->bit_depth);
                    }
                }
            }
        }
    }
    return AV1_WARP_OK;
}

Av1WarpStatus av1_warp_predict_single(const Av1WarpPlaneParams *params,
                                      uint16_t *destination,
                                      size_t destination_stride) {
    return av1_warp_predict(
        params, destination, destination_stride, 0);
}

Av1WarpStatus av1_warp_predict_compound(const Av1WarpPlaneParams *params,
                                        uint16_t *compound,
                                        size_t compound_stride) {
    return av1_warp_predict(params, compound, compound_stride, 1);
}
