#include "encoder/av1_transform_forward.h"
#include "base.h"

static int32_t transform_round_shift(int64_t value, unsigned int bits) {
    int64_t offset = (int64_t)1U << (bits - 1U);
    int64_t adjusted = value + offset;
    int64_t divisor = (int64_t)1U << bits;

    if (adjusted >= 0) return (int32_t)(adjusted / divisor);
    return (int32_t)(-((-adjusted + divisor - 1) / divisor));
}

static int32_t transform_floor_divide_2(int32_t value) {
    return value >= 0 ? value / 2 : -(int32_t)((-(int64_t)value + 1) / 2);
}

static void transform_forward_wht4(const int32_t input[4],
                                   int32_t output[4]) {
    int32_t sum = input[0] + input[1];
    int32_t difference = input[3] - input[2];
    int32_t half = transform_floor_divide_2(sum - difference);

    output[0] = sum - half + input[2];
    output[1] = half - input[2];
    output[2] = difference + half - input[1];
    output[3] = half - input[1];
}

void avifenc_av1_forward_wht_4x4(const int16_t input[16],
                                 int32_t output[16]) {
    int32_t intermediate[16];
    int32_t values[4];
    int32_t transformed[4];
    size_t row;
    size_t column;

    for (column = 0U; column < 4U; ++column) {
        for (row = 0U; row < 4U; ++row) {
            values[row] = input[row * 4U + column];
        }
        transform_forward_wht4(values, transformed);
        for (row = 0U; row < 4U; ++row) {
            intermediate[row * 4U + column] = transformed[row];
        }
    }
    for (row = 0U; row < 4U; ++row) {
        transform_forward_wht4(intermediate + row * 4U, transformed);
        for (column = 0U; column < 4U; ++column) {
            output[row * 4U + column] = transformed[column];
        }
    }
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

static AvifencStatus transform_size(Av1TxSize tx_size,
                                    size_t *width,
                                    size_t *height,
                                    unsigned int *column_shift) {
    if (width == 0 || height == 0 || column_shift == 0) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    switch (tx_size) {
        case AV1_TX_4X4:
            *width = *height = 4U;
            *column_shift = 0U;
            return AVIFENC_OK;
        case AV1_TX_8X8:
            *width = *height = 8U;
            *column_shift = 1U;
            return AVIFENC_OK;
        case AV1_TX_16X16:
            *width = *height = 16U;
            *column_shift = 2U;
            return AVIFENC_OK;
        case AV1_TX_32X32:
            *width = *height = 32U;
            *column_shift = 4U;
            return AVIFENC_OK;
        case AV1_TX_4X8:
            *width = 4U;
            *height = 8U;
            *column_shift = 1U;
            return AVIFENC_OK;
        case AV1_TX_8X4:
            *width = 8U;
            *height = 4U;
            *column_shift = 1U;
            return AVIFENC_OK;
        case AV1_TX_8X16:
            *width = 8U;
            *height = 16U;
            *column_shift = 2U;
            return AVIFENC_OK;
        case AV1_TX_16X8:
            *width = 16U;
            *height = 8U;
            *column_shift = 2U;
            return AVIFENC_OK;
        case AV1_TX_16X32:
            *width = 16U;
            *height = 32U;
            *column_shift = 4U;
            return AVIFENC_OK;
        case AV1_TX_32X16:
            *width = 32U;
            *height = 16U;
            *column_shift = 4U;
            return AVIFENC_OK;
        default:
            return AVIFENC_UNSUPPORTED;
    }
}

static void transform_fdct(const int32_t *input,
                           int32_t *output,
                           size_t size) {
    size_t frequency;

    for (frequency = 0U; frequency < size; ++frequency) {
        int64_t sum = 0;
        size_t sample;

        for (sample = 0U; sample < size; ++sample) {
            int32_t basis = frequency == 0U
                ? 2896
                : av1_recon_cos128((int)(64U * (2U * sample + 1U) *
                                             frequency / size));
            sum += (int64_t)input[sample] * basis;
        }
        output[frequency] = transform_round_shift(sum, 12U);
    }
}

int avifenc_av1_transform_size_supported(Av1TxSize tx_size) {
    size_t width;
    size_t height;
    unsigned int column_shift;

    return transform_size(
        tx_size, &width, &height, &column_shift) == AVIFENC_OK;
}

AvifencStatus avifenc_av1_forward_dct(const int16_t *input,
                                      size_t stride,
                                      Av1TxSize tx_size,
                                      int32_t *output,
                                      size_t output_capacity) {
    int32_t intermediate[1024];
    int32_t values[32];
    int32_t transformed[32];
    size_t width;
    size_t height;
    unsigned int column_shift;
    size_t row;
    size_t column;
    AvifencStatus status = transform_size(
        tx_size, &width, &height, &column_shift);

    if (status != AVIFENC_OK) return status;
    if (input == 0 || output == 0 || stride < width ||
        output_capacity < width * height) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    if (tx_size == AV1_TX_4X4) {
        return avifenc_av1_forward_dct_4x4(input, stride, output);
    }
    for (column = 0U; column < width; ++column) {
        for (row = 0U; row < height; ++row) {
            values[row] = (int32_t)input[row * stride + column] * 4;
        }
        transform_fdct(values, transformed, height);
        for (row = 0U; row < height; ++row) {
            intermediate[row * width + column] = column_shift == 0U
                ? transformed[row]
                : transform_round_shift(transformed[row], column_shift);
        }
    }
    for (row = 0U; row < height; ++row) {
        transform_fdct(intermediate + row * width, transformed, width);
        for (column = 0U; column < width; ++column) {
            int32_t value = transformed[column];

            if (width != height) {
                value = transform_round_shift((int64_t)value * 5793, 12U);
            }
            output[column * height + row] = value;
        }
    }
    return AVIFENC_OK;
}
