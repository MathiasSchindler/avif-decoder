#include "av1_filter.h"
#include "base.h"

static int av1_cdef_abs(int value) {
    return value < 0 ? -value : value;
}

static int av1_cdef_clip(int low, int high, int value) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int av1_cdef_arshift(int value, unsigned int bits) {
    if (value >= 0) return value >> bits;
    return -(int)(((unsigned int)(-value) + ((1U << bits) - 1U)) >> bits);
}

static unsigned int av1_cdef_floor_log2(unsigned int value) {
    unsigned int result = 0U;
    while (value > 1U) {
        value >>= 1;
        ++result;
    }
    return result;
}

static const Av1BlockCell *av1_cdef_cell(const Av1BlockState *blocks,
                                          uint32_t row,
                                          uint32_t column) {
    size_t index = (size_t)row * blocks->mi_columns + column;
    if (row >= blocks->mi_rows || column >= blocks->mi_columns ||
        index >= blocks->cell_capacity || blocks->cells[index].width == 0U) {
        return 0;
    }
    return &blocks->cells[index];
}

AvifdecStatus av1_cdef_find_direction(const uint16_t *source,
                                      size_t stride,
                                      uint8_t bit_depth,
                                      uint8_t *direction,
                                      uint32_t *variance) {
    static const unsigned int divisors[9] = {
        0U, 840U, 420U, 280U, 210U, 168U, 140U, 120U, 105U
    };
    int partial[8][15];
    int64_t cost[8];
    int64_t best_cost = 0;
    unsigned int best_direction = 0U;
    unsigned int row;
    unsigned int column;
    unsigned int index;

    if (source == 0 || stride < 8U || direction == 0 || variance == 0 ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (index = 0U; index < 8U; ++index) {
        unsigned int line;
        cost[index] = 0;
        for (line = 0U; line < 15U; ++line) partial[index][line] = 0;
    }
    for (row = 0U; row < 8U; ++row) {
        for (column = 0U; column < 8U; ++column) {
            int value = (int)(source[row * stride + column] >>
                              (bit_depth - 8U)) - 128;
            partial[0][row + column] += value;
            partial[1][row + column / 2U] += value;
            partial[2][row] += value;
            partial[3][3U + row - column / 2U] += value;
            partial[4][7U + row - column] += value;
            partial[5][3U - row / 2U + column] += value;
            partial[6][column] += value;
            partial[7][row / 2U + column] += value;
        }
    }
    for (index = 0U; index < 8U; ++index) {
        cost[2] += (int64_t)partial[2][index] * partial[2][index];
        cost[6] += (int64_t)partial[6][index] * partial[6][index];
    }
    cost[2] *= divisors[8];
    cost[6] *= divisors[8];
    for (index = 0U; index < 7U; ++index) {
        cost[0] += ((int64_t)partial[0][index] * partial[0][index] +
                    (int64_t)partial[0][14U - index] *
                        partial[0][14U - index]) * divisors[index + 1U];
        cost[4] += ((int64_t)partial[4][index] * partial[4][index] +
                    (int64_t)partial[4][14U - index] *
                        partial[4][14U - index]) * divisors[index + 1U];
    }
    cost[0] += (int64_t)partial[0][7] * partial[0][7] * divisors[8];
    cost[4] += (int64_t)partial[4][7] * partial[4][7] * divisors[8];
    for (index = 1U; index < 8U; index += 2U) {
        unsigned int line;
        for (line = 0U; line < 5U; ++line) {
            cost[index] += (int64_t)partial[index][3U + line] *
                           partial[index][3U + line];
        }
        cost[index] *= divisors[8];
        for (line = 0U; line < 3U; ++line) {
            cost[index] +=
                ((int64_t)partial[index][line] * partial[index][line] +
                 (int64_t)partial[index][10U - line] *
                     partial[index][10U - line]) * divisors[2U * line + 2U];
        }
    }
    for (index = 0U; index < 8U; ++index) {
        if (cost[index] > best_cost) {
            best_cost = cost[index];
            best_direction = index;
        }
    }
    *direction = (uint8_t)best_direction;
    *variance = (uint32_t)((best_cost - cost[(best_direction + 4U) & 7U]) >> 10);
    return AVIFDEC_OK;
}

static int av1_cdef_constrain(int difference,
                              unsigned int threshold,
                              unsigned int damping) {
    unsigned int magnitude;
    unsigned int damping_adjustment;
    int constrained;

    if (threshold == 0U) return 0;
    magnitude = (unsigned int)av1_cdef_abs(difference);
    damping_adjustment = av1_cdef_floor_log2(threshold);
    damping_adjustment = damping > damping_adjustment
                         ? damping - damping_adjustment : 0U;
    constrained = (int)threshold - (int)(magnitude >> damping_adjustment);
    constrained = av1_cdef_clip(0, (int)magnitude, constrained);
    return difference < 0 ? -constrained : constrained;
}

static int av1_cdef_available(const Av1CdefParams *params,
                              int x,
                              int y,
                              unsigned int sub_x,
                              unsigned int sub_y) {
    uint32_t column;
    uint32_t row;
    if (x < 0 || y < 0) return 0;
    column = ((uint32_t)x << sub_x) >> 2;
    row = ((uint32_t)y << sub_y) >> 2;
    return column < params->mi_columns && row < params->mi_rows;
}

static AvifdecStatus av1_cdef_filter_block(
    Av1FramePlanes *output,
    const Av1FramePlanes *input,
    const Av1CdefParams *params,
    unsigned int plane,
    uint32_t mi_row,
    uint32_t mi_column,
    unsigned int primary_strength,
    unsigned int secondary_strength,
    unsigned int damping,
    unsigned int direction) {
    static const int directions[8][2][2] = {
        { { -1, 1 }, { -2, 2 } }, { { 0, 1 }, { -1, 2 } },
        { { 0, 1 }, { 0, 2 } }, { { 0, 1 }, { 1, 2 } },
        { { 1, 1 }, { 2, 2 } }, { { 1, 0 }, { 2, 1 } },
        { { 1, 0 }, { 2, 0 } }, { { 1, 0 }, { 2, -1 } }
    };
    static const unsigned int primary_taps[2][2] = {
        { 4U, 2U }, { 3U, 3U }
    };
    static const unsigned int secondary_taps[2] = { 2U, 1U };
    unsigned int sub_x = plane == 0U ? 0U : params->subsampling_x;
    unsigned int sub_y = plane == 0U ? 0U : params->subsampling_y;
    uint32_t x0 = (mi_column * 4U) >> sub_x;
    uint32_t y0 = (mi_row * 4U) >> sub_y;
    unsigned int width = 8U >> sub_x;
    unsigned int height = 8U >> sub_y;
    unsigned int coeff_shift = params->bit_depth - 8U;
    unsigned int tap_set = (primary_strength >> coeff_shift) & 1U;
    int clipping_required = primary_strength != 0U && secondary_strength != 0U;
    unsigned int row;
    unsigned int column;

    if (direction >= 8U) return AVIFDEC_INVALID_ARGUMENT;
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint32_t destination_x = x0 + column;
            uint32_t destination_y = y0 + row;
            int center;
            int minimum;
            int maximum;
            int sum = 0;
            unsigned int distance;

            if (destination_x >= output->width[plane] ||
                destination_y >= output->height[plane]) continue;
            center = input->data[plane][
                (size_t)destination_y * input->stride[plane] + destination_x];
            minimum = center;
            maximum = center;
            for (distance = 0U; distance < 2U; ++distance) {
                int sign;
                for (sign = -1; sign <= 1; sign += 2) {
                    int primary_y = (int)destination_y +
                        sign * directions[direction][distance][0];
                    int primary_x = (int)destination_x +
                        sign * directions[direction][distance][1];
                    int direction_offset;

                    if (av1_cdef_available(params, primary_x, primary_y,
                                           sub_x, sub_y)) {
                        int sample = input->data[plane][
                            (size_t)primary_y * input->stride[plane] +
                            (size_t)primary_x];
                        sum += (int)primary_taps[tap_set][distance] *
                            av1_cdef_constrain(sample - center,
                                               primary_strength, damping);
                        if (clipping_required) {
                            if (sample < minimum) minimum = sample;
                            if (sample > maximum) maximum = sample;
                        }
                    }
                    for (direction_offset = -2; direction_offset <= 2;
                         direction_offset += 4) {
                        unsigned int secondary_direction =
                            (direction + (unsigned int)direction_offset) & 7U;
                        int secondary_y = (int)destination_y + sign *
                            directions[secondary_direction][distance][0];
                        int secondary_x = (int)destination_x + sign *
                            directions[secondary_direction][distance][1];
                        if (av1_cdef_available(params, secondary_x, secondary_y,
                                               sub_x, sub_y)) {
                            int sample = input->data[plane][
                                (size_t)secondary_y * input->stride[plane] +
                                (size_t)secondary_x];
                            sum += (int)secondary_taps[distance] *
                                av1_cdef_constrain(sample - center,
                                                   secondary_strength, damping);
                            if (clipping_required) {
                                if (sample < minimum) minimum = sample;
                                if (sample > maximum) maximum = sample;
                            }
                        }
                    }
                }
            }
            center += av1_cdef_arshift(8 + sum - (sum < 0), 4U);
            if (clipping_required) {
                center = av1_cdef_clip(minimum, maximum, center);
            }
            output->data[plane][
                (size_t)destination_y * output->stride[plane] + destination_x] =
                (uint16_t)center;
        }
    }
    return AVIFDEC_OK;
}

static int av1_cdef_block_skipped(const Av1BlockState *blocks,
                                  uint32_t row,
                                  uint32_t column) {
    unsigned int y;
    unsigned int x;
    for (y = 0U; y < 2U; ++y) {
        for (x = 0U; x < 2U; ++x) {
            const Av1BlockCell *cell;
            if (row + y >= blocks->mi_rows || column + x >= blocks->mi_columns) {
                continue;
            }
            cell = av1_cdef_cell(blocks, row + y, column + x);
            if (cell != 0 && !cell->skip) return 0;
        }
    }
    return 1;
}

AvifdecStatus av1_cdef_frame(Av1FramePlanes *output,
                             const Av1FramePlanes *input,
                             const Av1BlockState *blocks,
                             const Av1CdefParams *params) {
    static const uint8_t uv_direction[2][2][8] = {
        { { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U },
          { 1U, 2U, 2U, 2U, 3U, 4U, 6U, 0U } },
        { { 7U, 0U, 2U, 4U, 5U, 6U, 6U, 6U },
          { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U } }
    };
    unsigned int plane_count;
    unsigned int plane;
    uint32_t row;
    uint32_t column;

    if (output == 0 || input == 0 || blocks == 0 || params == 0 ||
        params->indices == 0 || params->y_pri_strength == 0 ||
        params->y_sec_strength == 0 || params->bits > 3U ||
        params->monochrome > 1U || params->subsampling_x > 1U ||
        params->subsampling_y > 1U || blocks->mi_rows != params->mi_rows ||
        blocks->mi_columns != params->mi_columns ||
        (params->bit_depth != 8U && params->bit_depth != 10U &&
         params->bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    plane_count = params->monochrome ? 1U : 3U;
    if (!params->monochrome &&
        (params->uv_pri_strength == 0 || params->uv_sec_strength == 0)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (plane = 0U; plane < plane_count; ++plane) {
        uint32_t copy_row;
        if (input->data[plane] == 0 || output->data[plane] == 0 ||
            input->width[plane] != output->width[plane] ||
            input->height[plane] != output->height[plane]) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        for (copy_row = 0U; copy_row < input->height[plane]; ++copy_row) {
            avifdec_memory_copy(
                output->data[plane] + (size_t)copy_row * output->stride[plane],
                input->data[plane] + (size_t)copy_row * input->stride[plane],
                input->width[plane] * sizeof(uint16_t));
        }
    }
    for (row = 0U; row < params->mi_rows; row += 2U) {
        for (column = 0U; column < params->mi_columns; column += 2U) {
            size_t index = (size_t)(row & ~15U) * params->mi_columns +
                           (column & ~15U);
            uint8_t strength_index;
            uint8_t direction;
            uint32_t variance;
            unsigned int coeff_shift = params->bit_depth - 8U;
            unsigned int primary;
            unsigned int signaled_primary;
            unsigned int secondary;
            unsigned int variance_strength;
            AvifdecStatus status;

            if (index >= params->index_capacity) return AVIFDEC_LIMIT_EXCEEDED;
            strength_index = params->indices[index];
                if (strength_index == 0xffU || av1_cdef_block_skipped(
                    blocks, row, column)) continue;
            if (strength_index >= (1U << params->bits)) {
                return AVIFDEC_INVALID_DATA;
            }
            status = av1_cdef_find_direction(
                input->data[0] + (size_t)(row * 4U) * input->stride[0] +
                    column * 4U,
                input->stride[0], params->bit_depth, &direction, &variance);
            if (status != AVIFDEC_OK) return status;
            primary = (unsigned int)params->y_pri_strength[strength_index] <<
                      coeff_shift;
            signaled_primary = primary;
            secondary = (unsigned int)params->y_sec_strength[strength_index] <<
                        coeff_shift;
            variance_strength = variance >> 6;
            variance_strength = variance_strength != 0U
                ? av1_cdef_floor_log2(variance_strength) : 0U;
            if (variance_strength > 12U) variance_strength = 12U;
            primary = variance != 0U
                ? (primary * (4U + variance_strength) + 8U) >> 4 : 0U;
            status = av1_cdef_filter_block(
                output, input, params, 0U, row, column, primary, secondary,
                params->damping + coeff_shift,
                signaled_primary == 0U ? 0U : direction);
            if (status != AVIFDEC_OK || params->monochrome) {
                if (status != AVIFDEC_OK) return status;
                continue;
            }
            primary = (unsigned int)params->uv_pri_strength[strength_index] <<
                      coeff_shift;
            secondary = (unsigned int)params->uv_sec_strength[strength_index] <<
                        coeff_shift;
            direction = primary == 0U ? 0U :
                uv_direction[params->subsampling_x][params->subsampling_y]
                            [direction];
            for (plane = 1U; plane < 3U; ++plane) {
                status = av1_cdef_filter_block(
                    output, input, params, plane, row, column,
                    primary, secondary,
                    params->damping + coeff_shift - 1U, direction);
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    return AVIFDEC_OK;
}
