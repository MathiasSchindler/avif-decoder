#include "av1_filter.h"

static int av1_superres_arshift(int value, unsigned int bits) {
    if (value >= 0) return value >> bits;
    return -(int)(((unsigned int)(-value) + ((1U << bits) - 1U)) >> bits);
}

static const int16_t av1_superres_filter[64][8] = {
    { 0, 0, 0, 128, 0, 0, 0, 0 }, { 0, 0, -1, 128, 2, -1, 0, 0 },
    { 0, 1, -3, 127, 4, -2, 1, 0 }, { 0, 1, -4, 127, 6, -3, 1, 0 },
    { 0, 2, -6, 126, 8, -3, 1, 0 }, { 0, 2, -7, 125, 11, -4, 1, 0 },
    { -1, 2, -8, 125, 13, -5, 2, 0 }, { -1, 3, -9, 124, 15, -6, 2, 0 },
    { -1, 3, -10, 123, 18, -6, 2, -1 }, { -1, 3, -11, 122, 20, -7, 3, -1 },
    { -1, 4, -12, 121, 22, -8, 3, -1 }, { -1, 4, -13, 120, 25, -9, 3, -1 },
    { -1, 4, -14, 118, 28, -9, 3, -1 }, { -1, 4, -15, 117, 30, -10, 4, -1 },
    { -1, 5, -16, 116, 32, -11, 4, -1 }, { -1, 5, -16, 114, 35, -12, 4, -1 },
    { -1, 5, -17, 112, 38, -12, 4, -1 }, { -1, 5, -18, 111, 40, -13, 5, -1 },
    { -1, 5, -18, 109, 43, -14, 5, -1 }, { -1, 6, -19, 107, 45, -14, 5, -1 },
    { -1, 6, -19, 105, 48, -15, 5, -1 }, { -1, 6, -19, 103, 51, -16, 5, -1 },
    { -1, 6, -20, 101, 53, -16, 6, -1 }, { -1, 6, -20, 99, 56, -17, 6, -1 },
    { -1, 6, -20, 97, 58, -17, 6, -1 }, { -1, 6, -20, 95, 61, -18, 6, -1 },
    { -2, 7, -20, 93, 64, -18, 6, -2 }, { -2, 7, -20, 91, 66, -19, 6, -1 },
    { -2, 7, -20, 88, 69, -19, 6, -1 }, { -2, 7, -20, 86, 71, -19, 6, -1 },
    { -2, 7, -20, 84, 74, -20, 7, -2 }, { -2, 7, -20, 81, 76, -20, 7, -1 },
    { -2, 7, -20, 79, 79, -20, 7, -2 }, { -1, 7, -20, 76, 81, -20, 7, -2 },
    { -2, 7, -20, 74, 84, -20, 7, -2 }, { -1, 6, -19, 71, 86, -20, 7, -2 },
    { -1, 6, -19, 69, 88, -20, 7, -2 }, { -1, 6, -19, 66, 91, -20, 7, -2 },
    { -2, 6, -18, 64, 93, -20, 7, -2 }, { -1, 6, -18, 61, 95, -20, 6, -1 },
    { -1, 6, -17, 58, 97, -20, 6, -1 }, { -1, 6, -17, 56, 99, -20, 6, -1 },
    { -1, 6, -16, 53, 101, -20, 6, -1 }, { -1, 5, -16, 51, 103, -19, 6, -1 },
    { -1, 5, -15, 48, 105, -19, 6, -1 }, { -1, 5, -14, 45, 107, -19, 6, -1 },
    { -1, 5, -14, 43, 109, -18, 5, -1 }, { -1, 5, -13, 40, 111, -18, 5, -1 },
    { -1, 4, -12, 38, 112, -17, 5, -1 }, { -1, 4, -12, 35, 114, -16, 5, -1 },
    { -1, 4, -11, 32, 116, -16, 5, -1 }, { -1, 4, -10, 30, 117, -15, 4, -1 },
    { -1, 3, -9, 28, 118, -14, 4, -1 }, { -1, 3, -9, 25, 120, -13, 4, -1 },
    { -1, 3, -8, 22, 121, -12, 4, -1 }, { -1, 3, -7, 20, 122, -11, 3, -1 },
    { -1, 2, -6, 18, 123, -10, 3, -1 }, { 0, 2, -6, 15, 124, -9, 3, -1 },
    { 0, 2, -5, 13, 125, -8, 2, -1 }, { 0, 1, -4, 11, 125, -7, 2, 0 },
    { 0, 1, -3, 8, 126, -6, 2, 0 }, { 0, 1, -3, 6, 127, -4, 1, 0 },
    { 0, 1, -2, 4, 127, -3, 1, 0 }, { 0, 0, -1, 2, 128, -1, 0, 0 }
};

/*
 * Rows are independent: row `row` of the output only reads input row
 * `row` (via input + row*input_stride) and writes output row `row` (via
 * output + row*output_stride). Every per-row quantity below (step_x,
 * initial_subpel_x, maximum) is derived solely from the immutable
 * function arguments, computed once up front, and then only read
 * (never written) while rows are processed - so disjoint row ranges can
 * run on separate workers with no shared mutable state and no risk of
 * aliasing between output rows (output_stride >= output_width is
 * validated below, and each worker only ever writes its own assigned
 * rows).
 */
typedef struct {
    uint16_t *output;
    size_t output_stride;
    uint32_t output_width;
    const uint16_t *input;
    size_t input_stride;
    uint32_t padded_input_width;
    int64_t step_x;
    int64_t initial_subpel_x;
    unsigned int maximum;
} Av1SuperresParallelContext;

static AvifdecStatus av1_superres_upscale_rows(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg) {
    const Av1SuperresParallelContext *context =
        (const Av1SuperresParallelContext *)arg;
    size_t row;

    (void)worker_index;
    if (context == 0 || begin > end) return AVIFDEC_INVALID_ARGUMENT;
    for (row = begin; row < end; ++row) {
        uint32_t column;

        for (column = 0U; column < context->output_width; ++column) {
            int64_t source_x = context->initial_subpel_x +
                               (int64_t)column * context->step_x;
            int64_t source_pixel =
                av1_superres_arshift((int)(source_x - (1LL << 14)), 14U);
            unsigned int subpel =
                (unsigned int)((source_x & 0x3fff) >> 8);
            int64_t sum = 0;
            unsigned int tap;

            for (tap = 0U; tap < 8U; ++tap) {
                int64_t sample_x = source_pixel + (int64_t)tap - 3;
                if (sample_x < 0) sample_x = 0;
                if ((uint64_t)sample_x >= context->padded_input_width) {
                    sample_x = context->padded_input_width - 1U;
                }
                sum += context->input[row * context->input_stride +
                                      (size_t)sample_x] *
                       av1_superres_filter[subpel][tap];
            }
            sum = av1_superres_arshift((int)(sum + 64), 7U);
            if (sum < 0) sum = 0;
            if ((uint64_t)sum > context->maximum) sum = context->maximum;
            context->output[row * context->output_stride + column] =
                (uint16_t)sum;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_superres_upscale_plane_ex(
    uint16_t *output,
    size_t output_stride,
    uint32_t output_width,
    const uint16_t *input,
    size_t input_stride,
    uint32_t input_width,
    uint32_t padded_input_width,
    uint32_t height,
    uint8_t bit_depth,
    const AvifdecExecutor *executor) {
    Av1SuperresParallelContext context;
    int64_t step_x;
    int64_t error;
    int64_t initial_subpel_x;

    if (executor != 0 &&
        (executor->worker_count == 0U ||
         executor->worker_count > AVIFDEC_EXECUTOR_MAX_WORKERS ||
         executor->parallel_for == 0)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (output == 0 || input == 0 || input_width == 0U ||
        output_width <= input_width || padded_input_width < input_width ||
        output_stride < output_width || input_stride < padded_input_width ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    step_x = (((int64_t)input_width << 14) + output_width / 2U) /
             output_width;
    error = (int64_t)output_width * step_x -
            ((int64_t)input_width << 14);
    initial_subpel_x =
        (-((int64_t)(output_width - input_width) << 13) +
         output_width / 2U) / output_width + 128 - error / 2;
    initial_subpel_x = (int64_t)((uint64_t)initial_subpel_x & 0x3fffU);
    context.output = output;
    context.output_stride = output_stride;
    context.output_width = output_width;
    context.input = input;
    context.input_stride = input_stride;
    context.padded_input_width = padded_input_width;
    context.step_x = step_x;
    context.initial_subpel_x = initial_subpel_x;
    context.maximum = (1U << bit_depth) - 1U;
    if (executor != 0 && executor->worker_count > 1U && height > 1U) {
        return executor->parallel_for(
            executor->user_data, height, 1U,
            av1_superres_upscale_rows, &context);
    }
    return av1_superres_upscale_rows(0U, height, 0U, &context);
}

AvifdecStatus av1_superres_upscale_plane(uint16_t *output,
                                         size_t output_stride,
                                         uint32_t output_width,
                                         const uint16_t *input,
                                         size_t input_stride,
                                         uint32_t input_width,
                                         uint32_t padded_input_width,
                                         uint32_t height,
                                         uint8_t bit_depth) {
    return av1_superres_upscale_plane_ex(
        output, output_stride, output_width, input, input_stride,
        input_width, padded_input_width, height, bit_depth, 0);
}
