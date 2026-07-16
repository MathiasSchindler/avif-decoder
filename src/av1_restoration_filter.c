#include "av1_filter.h"
#include "base.h"

static int av1_restoration_clip(int low, int high, int value) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

typedef struct {
    const uint16_t *cdef;
    const uint16_t *deblocked;
    size_t cdef_stride;
    size_t deblocked_stride;
    uint32_t width;
    uint32_t height;
    int stripe_start_y;
    int stripe_end_y;
} Av1RestorationSource;

static int64_t av1_restoration_arshift64(int64_t value, unsigned int bits) {
    if (value >= 0) return value >> bits;
    return -(int64_t)(((uint64_t)(-value) + (((uint64_t)1U << bits) - 1U)) >>
                     bits);
}

static int64_t av1_restoration_round64(int64_t value, unsigned int bits) {
    if (bits == 0U) return value;
    return av1_restoration_arshift64(value + ((int64_t)1U << (bits - 1U)), bits);
}

static uint16_t av1_restoration_source_sample(const Av1RestorationSource *source,
                                               int x,
                                               int y) {
    const uint16_t *data;
    size_t stride;

    if (x < 0) x = 0;
    if ((uint32_t)x >= source->width) x = (int)source->width - 1;
    if (y < 0) y = 0;
    if ((uint32_t)y >= source->height) y = (int)source->height - 1;
    if (y < source->stripe_start_y) {
        if (y < source->stripe_start_y - 2) y = source->stripe_start_y - 2;
        if (y < 0) y = 0;
        data = source->deblocked;
        stride = source->deblocked_stride;
    } else if (y > source->stripe_end_y) {
        if (y > source->stripe_end_y + 2) y = source->stripe_end_y + 2;
        if ((uint32_t)y >= source->height) y = (int)source->height - 1;
        data = source->deblocked;
        stride = source->deblocked_stride;
    } else {
        data = source->cdef;
        stride = source->cdef_stride;
    }
    return data[(size_t)y * stride + (size_t)x];
}

static void av1_restoration_wiener_coefficients(const int8_t input[3],
                                                 int output[7]) {
    unsigned int index;

    output[3] = 128;
    for (index = 0U; index < 3U; ++index) {
        output[index] = input[index];
        output[6U - index] = input[index];
        output[3] -= 2 * input[index];
    }
}

static void av1_restoration_wiener_block(
    uint16_t *output,
    size_t output_stride,
    const Av1RestorationSource *source,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const Av1RestorationUnit *unit,
    uint8_t bit_depth) {
    int intermediate[10][4];
    int vertical[7];
    int horizontal[7];
    unsigned int round0 = bit_depth == 12U ? 5U : 3U;
    unsigned int round1 = bit_depth == 12U ? 9U : 11U;
    int offset = 1 << (bit_depth + 7U - round0 - 1U);
    int limit = (1 << (bit_depth + 1U + 7U - round0)) - 1;
    uint32_t row;
    uint32_t column;

    av1_restoration_wiener_coefficients(unit->wiener[0], vertical);
    av1_restoration_wiener_coefficients(unit->wiener[1], horizontal);
    for (row = 0U; row < height + 6U; ++row) {
        for (column = 0U; column < width; ++column) {
            int64_t sum = 0;
            unsigned int tap;
            int value;
            for (tap = 0U; tap < 7U; ++tap) {
                sum += horizontal[tap] * av1_restoration_source_sample(
                    source, (int)(x + column) + (int)tap - 3,
                    (int)(y + row) - 3);
            }
            value = (int)av1_restoration_round64(sum, round0);
            intermediate[row][column] = av1_restoration_clip(
                -offset, limit - offset, value);
        }
    }
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            int64_t sum = 0;
            int64_t value;
            unsigned int tap;
            for (tap = 0U; tap < 7U; ++tap) {
                sum += vertical[tap] * intermediate[row + tap][column];
            }
            value = av1_restoration_round64(sum, round1);
            if (value < 0) value = 0;
            if (value > ((1 << bit_depth) - 1)) {
                value = (1 << bit_depth) - 1;
            }
            output[(size_t)(y + row) * output_stride + x + column] =
                (uint16_t)value;
        }
    }
}

static void av1_restoration_sgr_box(
    int32_t filtered[4][4],
    const Av1RestorationSource *source,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    unsigned int radius,
    unsigned int epsilon,
    unsigned int pass,
    uint8_t bit_depth) {
    int32_t coefficient_a[6][6];
    int32_t coefficient_b[6][6];
    unsigned int n = (2U * radius + 1U) * (2U * radius + 1U);
    unsigned int n2e = n * n * epsilon;
    unsigned int scale = ((1U << 20U) + n2e / 2U) / n2e;
    unsigned int reciprocal = ((1U << 12U) + n / 2U) / n;
    int row;
    int column;

    for (row = -1; row <= (int)height; ++row) {
        for (column = -1; column <= (int)width; ++column) {
            int64_t square_sum = 0;
            int64_t sum = 0;
            int dy;
            int dx;
            int64_t variance;
            int64_t z;
            int a2;
            int64_t b2;
            int64_t rounded_sum;
            for (dy = -(int)radius; dy <= (int)radius; ++dy) {
                for (dx = -(int)radius; dx <= (int)radius; ++dx) {
                    int64_t sample = av1_restoration_source_sample(
                        source, (int)x + column + dx, (int)y + row + dy);
                    square_sum += sample * sample;
                    sum += sample;
                }
            }
            square_sum = av1_restoration_round64(
                square_sum, 2U * (bit_depth - 8U));
            rounded_sum = av1_restoration_round64(sum, bit_depth - 8U);
            variance = square_sum * n - rounded_sum * rounded_sum;
            if (variance < 0) variance = 0;
            z = av1_restoration_round64(variance * scale, 20U);
            if (z >= 255) a2 = 256;
            else if (z == 0) a2 = 1;
            else a2 = (int)((z * 256 + z / 2) / (z + 1));
            b2 = (256 - a2) * sum * reciprocal;
            coefficient_a[row + 1][column + 1] = a2;
            coefficient_b[row + 1][column + 1] =
                (int32_t)av1_restoration_round64(b2, 12U);
        }
    }
    for (row = 0; row < (int)height; ++row) {
        unsigned int shift = pass == 0U && (row & 1) ? 4U : 5U;
        for (column = 0; column < (int)width; ++column) {
            int64_t a = 0;
            int64_t b = 0;
            int dy;
            int dx;
            for (dy = -1; dy <= 1; ++dy) {
                for (dx = -1; dx <= 1; ++dx) {
                    int weight;
                    if (pass == 0U) {
                        weight = ((row + dy) & 1) ? (dx == 0 ? 6 : 5) : 0;
                    } else {
                        weight = dx == 0 || dy == 0 ? 4 : 3;
                    }
                    a += weight * coefficient_a[row + dy + 1][column + dx + 1];
                    b += weight * coefficient_b[row + dy + 1][column + dx + 1];
                }
            }
            filtered[row][column] = (int32_t)av1_restoration_round64(
                a * source->cdef[(size_t)(y + (uint32_t)row) *
                                     source->cdef_stride + x + (uint32_t)column] + b,
                8U + shift - 4U);
        }
    }
}

static void av1_restoration_sgr_block(
    uint16_t *output,
    size_t output_stride,
    const Av1RestorationSource *source,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const Av1RestorationUnit *unit,
    uint8_t bit_depth) {
    static const uint8_t parameters[16][4] = {
        { 2U, 12U, 1U, 4U }, { 2U, 15U, 1U, 6U },
        { 2U, 18U, 1U, 8U }, { 2U, 21U, 1U, 9U },
        { 2U, 24U, 1U, 10U }, { 2U, 29U, 1U, 11U },
        { 2U, 36U, 1U, 12U }, { 2U, 45U, 1U, 13U },
        { 2U, 56U, 1U, 14U }, { 2U, 68U, 1U, 15U },
        { 0U, 0U, 1U, 5U }, { 0U, 0U, 1U, 8U },
        { 0U, 0U, 1U, 11U }, { 0U, 0U, 1U, 14U },
        { 2U, 30U, 0U, 0U }, { 2U, 75U, 0U, 0U }
    };
    int32_t filtered[2][4][4];
    int weight0 = unit->sgr_xqd[0];
    int weight1 = unit->sgr_xqd[1];
    int weight2 = 128 - weight0 - weight1;
    unsigned int pass;
    uint32_t row;
    uint32_t column;

    for (pass = 0U; pass < 2U; ++pass) {
        unsigned int radius = parameters[unit->sgr_set][pass * 2U];
        if (radius != 0U) {
            av1_restoration_sgr_box(
                filtered[pass], source, x, y, width, height, radius,
                parameters[unit->sgr_set][pass * 2U + 1U], pass, bit_depth);
        }
    }
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            int64_t original = source->cdef[(size_t)(y + row) *
                                                source->cdef_stride + x + column];
            int64_t scaled_original = original << 4U;
            int64_t value = weight1 * scaled_original;
            int64_t result;
            if (parameters[unit->sgr_set][0] != 0U) {
                value += weight0 * filtered[0][row][column];
            } else {
                value += weight0 * scaled_original;
            }
            if (parameters[unit->sgr_set][2] != 0U) {
                value += weight2 * filtered[1][row][column];
            } else {
                value += weight2 * scaled_original;
            }
            result = av1_restoration_round64(value, 11U);
            if (result < 0) result = 0;
            if (result > ((1 << bit_depth) - 1)) {
                result = (1 << bit_depth) - 1;
            }
            output[(size_t)(y + row) * output_stride + x + column] =
                (uint16_t)result;
        }
    }
}

typedef struct {
    Av1FramePlanes *output;
    const Av1FramePlanes *upscaled_cdef;
    const Av1FramePlanes *upscaled_deblocked;
    const Av1RestorationState *restoration;
    size_t row_units;
    uint8_t bit_depth;
    uint8_t plane_count;
} Av1RestorationParallelContext;

static AvifdecStatus av1_restoration_validate(
    Av1FramePlanes *output,
    const Av1FramePlanes *upscaled_cdef,
    const Av1FramePlanes *upscaled_deblocked,
    const Av1RestorationState *restoration,
    uint8_t bit_depth,
    unsigned int *plane_count) {
    unsigned int planes;
    unsigned int plane;

    if (output == 0 || upscaled_cdef == 0 || upscaled_deblocked == 0 ||
        restoration == 0 || plane_count == 0 ||
        restoration->monochrome > 1U ||
        restoration->subsampling_x > 1U ||
        restoration->subsampling_y > 1U ||
        restoration->upscaled_width == 0U ||
        restoration->frame_height == 0U ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    planes = restoration->monochrome ? 1U : 3U;
    *plane_count = planes;
    for (plane = 0U; plane < planes; ++plane) {
        unsigned int sub_x =
            plane == 0U ? 0U : restoration->subsampling_x;
        unsigned int sub_y =
            plane == 0U ? 0U : restoration->subsampling_y;
        uint32_t width = (restoration->upscaled_width +
                          ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        uint32_t height = (restoration->frame_height +
                           ((uint32_t)1U << sub_y) - 1U) >> sub_y;

        if (output->data[plane] == 0 ||
            upscaled_cdef->data[plane] == 0 ||
            upscaled_deblocked->data[plane] == 0 ||
            output->stride[plane] < width ||
            upscaled_cdef->stride[plane] < width ||
            upscaled_deblocked->stride[plane] < width) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        output->width[plane] = width;
        output->height[plane] = height;
        if (restoration->frame_type[plane] == 0U) continue;
        {
            size_t unit_count;
            size_t unit_end;
            size_t unit_index;

            if (restoration->frame_type[plane] > 3U ||
                restoration->units == 0 ||
                restoration->unit_size[plane] < 32U ||
                restoration->unit_size[plane] > 256U ||
                restoration->unit_rows[plane] == 0U ||
                restoration->unit_columns[plane] == 0U) {
                return AVIFDEC_INVALID_DATA;
            }
            if (!avifdec_size_multiply(
                    restoration->unit_rows[plane],
                    restoration->unit_columns[plane],
                    &unit_count) ||
                !avifdec_size_add(
                    restoration->plane_offset[plane],
                    unit_count, &unit_end)) {
                return AVIFDEC_OVERFLOW;
            }
            if (unit_end > restoration->unit_capacity) {
                return AVIFDEC_LIMIT_EXCEEDED;
            }
            for (unit_index = restoration->plane_offset[plane];
                 unit_index < unit_end;
                 ++unit_index) {
                const Av1RestorationUnit *unit =
                    &restoration->units[unit_index];

                if (!unit->parsed || unit->type == 0U) continue;
                if (unit->type == 1U ||
                    (unit->type == 2U && unit->sgr_set < 16U)) {
                    continue;
                }
                return AVIFDEC_INVALID_DATA;
            }
        }
    }
    return AVIFDEC_OK;
}

static void av1_restoration_copy_row(
    const Av1RestorationParallelContext *parallel,
    unsigned int plane,
    uint32_t luma_row) {
    unsigned int sub_y = plane == 0U
        ? 0U : parallel->restoration->subsampling_y;
    uint32_t y = luma_row >> sub_y;
    uint32_t row_count = 4U >> sub_y;
    uint32_t row;

    if (y + row_count > parallel->output->height[plane]) {
        row_count = parallel->output->height[plane] - y;
    }
    for (row = 0U; row < row_count; ++row) {
        avifdec_memory_copy(
            parallel->output->data[plane] +
                (size_t)(y + row) *
                    parallel->output->stride[plane],
            parallel->upscaled_cdef->data[plane] +
                (size_t)(y + row) *
                    parallel->upscaled_cdef->stride[plane],
            parallel->output->width[plane] *
                sizeof(uint16_t));
    }
}

static void av1_restoration_filter_row(
    const Av1RestorationParallelContext *parallel,
    unsigned int plane,
    uint32_t luma_row) {
    const Av1RestorationState *restoration =
        parallel->restoration;
    unsigned int sub_x =
        plane == 0U ? 0U : restoration->subsampling_x;
    unsigned int sub_y =
        plane == 0U ? 0U : restoration->subsampling_y;
    uint32_t width = parallel->output->width[plane];
    uint32_t height = parallel->output->height[plane];
    uint32_t stripe = (luma_row + 8U) / 64U;
    Av1RestorationSource source;
    uint32_t luma_column;

    if (restoration->frame_type[plane] == 0U) return;
    source.cdef = parallel->upscaled_cdef->data[plane];
    source.deblocked =
        parallel->upscaled_deblocked->data[plane];
    source.cdef_stride =
        parallel->upscaled_cdef->stride[plane];
    source.deblocked_stride =
        parallel->upscaled_deblocked->stride[plane];
    source.width = width;
    source.height = height;
    source.stripe_start_y = (int)av1_restoration_arshift64(
        -8 + (int64_t)stripe * 64, sub_y);
    source.stripe_end_y = source.stripe_start_y +
                          (int)(64U >> sub_y) - 1;
    for (luma_column = 0U;
         luma_column < restoration->upscaled_width;
         luma_column += 4U) {
        uint32_t x = luma_column >> sub_x;
        uint32_t y = luma_row >> sub_y;
        uint32_t block_width = 4U >> sub_x;
        uint32_t block_height = 4U >> sub_y;
        uint32_t unit_row =
            (((luma_row + 8U) >> sub_y) /
             restoration->unit_size[plane]);
        uint32_t unit_column =
            (luma_column >> sub_x) /
            restoration->unit_size[plane];
        size_t unit_index;
        const Av1RestorationUnit *unit;

        if (x + block_width > width) {
            block_width = width - x;
        }
        if (y + block_height > height) {
            block_height = height - y;
        }
        if (unit_row >= restoration->unit_rows[plane]) {
            unit_row = restoration->unit_rows[plane] - 1U;
        }
        if (unit_column >=
            restoration->unit_columns[plane]) {
            unit_column =
                restoration->unit_columns[plane] - 1U;
        }
        unit_index = restoration->plane_offset[plane] +
            (size_t)unit_row *
                restoration->unit_columns[plane] +
            unit_column;
        unit = &restoration->units[unit_index];
        if (!unit->parsed || unit->type == 0U) continue;
        if (unit->type == 1U) {
            av1_restoration_wiener_block(
                parallel->output->data[plane],
                parallel->output->stride[plane], &source,
                x, y, block_width, block_height, unit,
                parallel->bit_depth);
        } else {
            av1_restoration_sgr_block(
                parallel->output->data[plane],
                parallel->output->stride[plane], &source,
                x, y, block_width, block_height, unit,
                parallel->bit_depth);
        }
    }
}

static AvifdecStatus av1_restoration_filter_ranges(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg) {
    Av1RestorationParallelContext *parallel =
        (Av1RestorationParallelContext *)arg;
    size_t work_count;

    (void)worker_index;
    if (parallel == 0 || begin > end ||
        !avifdec_size_multiply(
            parallel->row_units, parallel->plane_count,
            &work_count) ||
        end > work_count) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    while (begin < end) {
        unsigned int plane =
            (unsigned int)(begin / parallel->row_units);
        size_t plane_end =
            (size_t)(plane + 1U) * parallel->row_units;

        if (plane_end > end) plane_end = end;
        while (begin < plane_end) {
            uint32_t luma_row = (uint32_t)(
                (begin % parallel->row_units) * 4U);

            av1_restoration_copy_row(
                parallel, plane, luma_row);
            av1_restoration_filter_row(
                parallel, plane, luma_row);
            ++begin;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_loop_restoration_frame_ex(
    Av1FramePlanes *output,
    const Av1FramePlanes *upscaled_cdef,
    const Av1FramePlanes *upscaled_deblocked,
    const Av1RestorationState *restoration,
    uint8_t bit_depth,
    const AvifdecExecutor *executor) {
    Av1RestorationParallelContext parallel;
    unsigned int plane_count;
    size_t work_count;
    AvifdecStatus status;

    if (executor != 0 &&
        (executor->worker_count == 0U ||
         executor->worker_count > AVIFDEC_EXECUTOR_MAX_WORKERS ||
         executor->parallel_for == 0)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = av1_restoration_validate(
        output, upscaled_cdef, upscaled_deblocked,
        restoration, bit_depth, &plane_count);
    if (status != AVIFDEC_OK) return status;
    parallel.output = output;
    parallel.upscaled_cdef = upscaled_cdef;
    parallel.upscaled_deblocked = upscaled_deblocked;
    parallel.restoration = restoration;
    parallel.row_units =
        ((size_t)restoration->frame_height + 3U) / 4U;
    parallel.bit_depth = bit_depth;
    parallel.plane_count = (uint8_t)plane_count;
    if (!avifdec_size_multiply(
            parallel.row_units, plane_count, &work_count)) {
        return AVIFDEC_OVERFLOW;
    }
    if (executor != 0 && executor->worker_count > 1U &&
        work_count > 1U) {
        return executor->parallel_for(
            executor->user_data, work_count, 1U,
            av1_restoration_filter_ranges, &parallel);
    }
    return av1_restoration_filter_ranges(
        0U, work_count, 0U, &parallel);
}

AvifdecStatus av1_loop_restoration_frame(
    Av1FramePlanes *output,
    const Av1FramePlanes *upscaled_cdef,
    const Av1FramePlanes *upscaled_deblocked,
    const Av1RestorationState *restoration,
    uint8_t bit_depth) {
    return av1_loop_restoration_frame_ex(
        output, upscaled_cdef, upscaled_deblocked,
        restoration, bit_depth, 0);
}
