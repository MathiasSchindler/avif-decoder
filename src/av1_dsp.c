#include "av1_dsp.h"

static int64_t av1_dsp_floor_div_pow2(int64_t value, unsigned int bits) {
    uint64_t magnitude;
    uint64_t mask;

    if (value >= 0) return value >> bits;
    magnitude = (uint64_t)(-value);
    mask = ((uint64_t)1U << bits) - 1U;
    return -(int64_t)((magnitude + mask) >> bits);
}

static int32_t av1_dsp_round2(int64_t value, unsigned int bits) {
    return (int32_t)av1_dsp_floor_div_pow2(
        value + ((int64_t)1U << (bits - 1U)), bits);
}

static int32_t av1_dsp_clip16(int64_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int32_t)value;
}

static void av1_dsp_inverse_dct4_1d_c(
    const int32_t *input,
    int32_t *output) {
    int32_t first = av1_dsp_round2(
        (int64_t)(input[0] + input[2]) * 2896, 12U);
    int32_t second = av1_dsp_round2(
        (int64_t)(input[0] - input[2]) * 2896, 12U);
    int32_t third = av1_dsp_round2(
        (int64_t)input[1] * 1567 - (int64_t)input[3] * 3784,
        12U);
    int32_t fourth = av1_dsp_round2(
        (int64_t)input[1] * 3784 + (int64_t)input[3] * 1567,
        12U);

    output[0] = av1_dsp_clip16((int64_t)first + fourth);
    output[1] = av1_dsp_clip16((int64_t)second + third);
    output[2] = av1_dsp_clip16((int64_t)second - third);
    output[3] = av1_dsp_clip16((int64_t)first - fourth);
}

void av1_dsp_inverse_dct4_c(const int32_t *input, int32_t *output) {
    int32_t intermediate[16];
    int32_t column_input[4];
    int32_t column_output[4];
    size_t row;
    size_t column;

    for (row = 0U; row < 4U; ++row) {
        av1_dsp_inverse_dct4_1d_c(
            input + row * 4U, intermediate + row * 4U);
    }
    for (column = 0U; column < 4U; ++column) {
        for (row = 0U; row < 4U; ++row) {
            column_input[row] = intermediate[row * 4U + column];
        }
        av1_dsp_inverse_dct4_1d_c(column_input, column_output);
        for (row = 0U; row < 4U; ++row) {
            output[row * 4U + column] =
                av1_dsp_round2(column_output[row], 4U);
        }
    }
}

static void av1_dsp_butterfly_c(
    int32_t *left,
    int32_t *right,
    int32_t cosine,
    int32_t sine,
    unsigned int flip) {
    int32_t x = av1_dsp_round2(
        (int64_t)*left * cosine - (int64_t)*right * sine, 12U);
    int32_t y = av1_dsp_round2(
        (int64_t)*left * sine + (int64_t)*right * cosine, 12U);

    *left = flip != 0U ? y : x;
    *right = flip != 0U ? x : y;
}

static void av1_dsp_hadamard_c(
    int32_t *left,
    int32_t *right,
    unsigned int flip) {
    int32_t first = *left;
    int32_t second = *right;

    *left = av1_dsp_clip16(
        flip != 0U ? (int64_t)second - first
                   : (int64_t)first + second);
    *right = av1_dsp_clip16(
        flip != 0U ? (int64_t)second + first
                   : (int64_t)first - second);
}

static void av1_dsp_inverse_dct8_1d_c(
    const int32_t *input,
    int32_t *output) {
    unsigned int index;

    output[0] = input[0];
    output[1] = input[4];
    output[2] = input[2];
    output[3] = input[6];
    output[4] = input[1];
    output[5] = input[5];
    output[6] = input[3];
    output[7] = input[7];
    av1_dsp_butterfly_c(&output[4], &output[7], 799, 4017, 0U);
    av1_dsp_butterfly_c(&output[5], &output[6], 3406, 2276, 0U);
    av1_dsp_butterfly_c(&output[0], &output[1], 2896, 2896, 1U);
    av1_dsp_butterfly_c(&output[2], &output[3], 1567, 3784, 0U);
    av1_dsp_hadamard_c(&output[4], &output[5], 0U);
    av1_dsp_hadamard_c(&output[6], &output[7], 1U);
    av1_dsp_hadamard_c(&output[0], &output[3], 0U);
    av1_dsp_hadamard_c(&output[1], &output[2], 0U);
    av1_dsp_butterfly_c(&output[6], &output[5], 2896, 2896, 1U);
    for (index = 0U; index < 4U; ++index) {
        av1_dsp_hadamard_c(&output[index], &output[7U - index], 0U);
    }
}

void av1_dsp_inverse_dct8_c(const int32_t *input, int32_t *output) {
    int32_t intermediate[64];
    int32_t column_input[8];
    int32_t column_output[8];
    size_t row;
    size_t column;

    for (row = 0U; row < 8U; ++row) {
        av1_dsp_inverse_dct8_1d_c(
            input + row * 8U, intermediate + row * 8U);
        for (column = 0U; column < 8U; ++column) {
            intermediate[row * 8U + column] = av1_dsp_round2(
                intermediate[row * 8U + column], 1U);
        }
    }
    for (column = 0U; column < 8U; ++column) {
        for (row = 0U; row < 8U; ++row) {
            column_input[row] = intermediate[row * 8U + column];
        }
        av1_dsp_inverse_dct8_1d_c(column_input, column_output);
        for (row = 0U; row < 8U; ++row) {
            output[row * 8U + column] =
                av1_dsp_round2(column_output[row], 4U);
        }
    }
}

static void av1_dsp_inverse_dct16_1d_c(
    const int32_t *input,
    int32_t *output) {
    static const uint8_t permutation[16] = {
        0U, 8U, 4U, 12U, 2U, 10U, 6U, 14U,
        1U, 9U, 5U, 13U, 3U, 11U, 7U, 15U
    };
    unsigned int index;

    for (index = 0U; index < 16U; ++index) {
        output[index] = input[permutation[index]];
    }
    av1_dsp_butterfly_c(&output[8], &output[15], 401, 4076, 0U);
    av1_dsp_butterfly_c(&output[9], &output[14], 3166, 2598, 0U);
    av1_dsp_butterfly_c(&output[10], &output[13], 1931, 3612, 0U);
    av1_dsp_butterfly_c(&output[11], &output[12], 3920, 1189, 0U);
    av1_dsp_butterfly_c(&output[4], &output[7], 799, 4017, 0U);
    av1_dsp_butterfly_c(&output[5], &output[6], 3406, 2276, 0U);
    av1_dsp_hadamard_c(&output[8], &output[9], 0U);
    av1_dsp_hadamard_c(&output[10], &output[11], 1U);
    av1_dsp_hadamard_c(&output[12], &output[13], 0U);
    av1_dsp_hadamard_c(&output[14], &output[15], 1U);
    av1_dsp_butterfly_c(&output[0], &output[1], 2896, 2896, 1U);
    av1_dsp_butterfly_c(&output[2], &output[3], 1567, 3784, 0U);
    av1_dsp_hadamard_c(&output[4], &output[5], 0U);
    av1_dsp_hadamard_c(&output[6], &output[7], 1U);
    av1_dsp_butterfly_c(&output[14], &output[9], 1567, 3784, 1U);
    av1_dsp_butterfly_c(&output[13], &output[10], -3784, 1567, 1U);
    av1_dsp_hadamard_c(&output[0], &output[3], 0U);
    av1_dsp_hadamard_c(&output[1], &output[2], 0U);
    av1_dsp_butterfly_c(&output[6], &output[5], 2896, 2896, 1U);
    av1_dsp_hadamard_c(&output[8], &output[11], 0U);
    av1_dsp_hadamard_c(&output[9], &output[10], 0U);
    av1_dsp_hadamard_c(&output[12], &output[15], 1U);
    av1_dsp_hadamard_c(&output[13], &output[14], 1U);
    for (index = 0U; index < 4U; ++index) {
        av1_dsp_hadamard_c(&output[index], &output[7U - index], 0U);
    }
    av1_dsp_butterfly_c(&output[13], &output[10], 2896, 2896, 1U);
    av1_dsp_butterfly_c(&output[12], &output[11], 2896, 2896, 1U);
    for (index = 0U; index < 8U; ++index) {
        av1_dsp_hadamard_c(&output[index], &output[15U - index], 0U);
    }
}

void av1_dsp_inverse_dct16_c(const int32_t *input, int32_t *output) {
    int32_t intermediate[256];
    int32_t column_input[16];
    int32_t column_output[16];
    size_t row;
    size_t column;

    for (row = 0U; row < 16U; ++row) {
        av1_dsp_inverse_dct16_1d_c(
            input + row * 16U, intermediate + row * 16U);
        for (column = 0U; column < 16U; ++column) {
            intermediate[row * 16U + column] = av1_dsp_round2(
                intermediate[row * 16U + column], 2U);
        }
    }
    for (column = 0U; column < 16U; ++column) {
        for (row = 0U; row < 16U; ++row) {
            column_input[row] = intermediate[row * 16U + column];
        }
        av1_dsp_inverse_dct16_1d_c(column_input, column_output);
        for (row = 0U; row < 16U; ++row) {
            output[row * 16U + column] =
                av1_dsp_round2(column_output[row], 4U);
        }
    }
}

#if defined(AVIFDEC_AARCH64_NEON)
typedef int32_t Av1DspI32x4 __attribute__((ext_vector_type(4)));
typedef int64_t Av1DspI64x4 __attribute__((ext_vector_type(4)));

static Av1DspI32x4 av1_dsp_clip16x4(Av1DspI32x4 value) {
    const Av1DspI32x4 minimum = { -32768, -32768, -32768, -32768 };
    const Av1DspI32x4 maximum = { 32767, 32767, 32767, 32767 };

    value = value < minimum ? minimum : value;
    return value > maximum ? maximum : value;
}

static void av1_dsp_inverse_dct4_1d_neon(
    Av1DspI32x4 input0,
    Av1DspI32x4 input1,
    Av1DspI32x4 input2,
    Av1DspI32x4 input3,
    Av1DspI32x4 *output0,
    Av1DspI32x4 *output1,
    Av1DspI32x4 *output2,
    Av1DspI32x4 *output3) {
    Av1DspI64x4 wide0 = __builtin_convertvector(input0, Av1DspI64x4);
    Av1DspI64x4 wide1 = __builtin_convertvector(input1, Av1DspI64x4);
    Av1DspI64x4 wide2 = __builtin_convertvector(input2, Av1DspI64x4);
    Av1DspI64x4 wide3 = __builtin_convertvector(input3, Av1DspI64x4);
    Av1DspI32x4 first = __builtin_convertvector(
        ((wide0 + wide2) * 2896 + 2048) >> 12, Av1DspI32x4);
    Av1DspI32x4 second = __builtin_convertvector(
        ((wide0 - wide2) * 2896 + 2048) >> 12, Av1DspI32x4);
    Av1DspI32x4 third = __builtin_convertvector(
        (wide1 * 1567 - wide3 * 3784 + 2048) >> 12,
        Av1DspI32x4);
    Av1DspI32x4 fourth = __builtin_convertvector(
        (wide1 * 3784 + wide3 * 1567 + 2048) >> 12,
        Av1DspI32x4);

    *output0 = av1_dsp_clip16x4(first + fourth);
    *output1 = av1_dsp_clip16x4(second + third);
    *output2 = av1_dsp_clip16x4(second - third);
    *output3 = av1_dsp_clip16x4(first - fourth);
}

static void av1_dsp_inverse_dct4_neon(
    const int32_t *input,
    int32_t *output) {
    Av1DspI32x4 input0 = { input[0], input[4], input[8], input[12] };
    Av1DspI32x4 input1 = { input[1], input[5], input[9], input[13] };
    Av1DspI32x4 input2 = { input[2], input[6], input[10], input[14] };
    Av1DspI32x4 input3 = { input[3], input[7], input[11], input[15] };
    Av1DspI32x4 row0;
    Av1DspI32x4 row1;
    Av1DspI32x4 row2;
    Av1DspI32x4 row3;
    Av1DspI32x4 result0;
    Av1DspI32x4 result1;
    Av1DspI32x4 result2;
    Av1DspI32x4 result3;

    av1_dsp_inverse_dct4_1d_neon(
        input0, input1, input2, input3,
        &row0, &row1, &row2, &row3);
    input0 = (Av1DspI32x4){ row0[0], row1[0], row2[0], row3[0] };
    input1 = (Av1DspI32x4){ row0[1], row1[1], row2[1], row3[1] };
    input2 = (Av1DspI32x4){ row0[2], row1[2], row2[2], row3[2] };
    input3 = (Av1DspI32x4){ row0[3], row1[3], row2[3], row3[3] };
    av1_dsp_inverse_dct4_1d_neon(
        input0, input1, input2, input3,
        &result0, &result1, &result2, &result3);
    result0 = (result0 + 8) >> 4;
    result1 = (result1 + 8) >> 4;
    result2 = (result2 + 8) >> 4;
    result3 = (result3 + 8) >> 4;
    output[0] = result0[0];
    output[1] = result0[1];
    output[2] = result0[2];
    output[3] = result0[3];
    output[4] = result1[0];
    output[5] = result1[1];
    output[6] = result1[2];
    output[7] = result1[3];
    output[8] = result2[0];
    output[9] = result2[1];
    output[10] = result2[2];
    output[11] = result2[3];
    output[12] = result3[0];
    output[13] = result3[1];
    output[14] = result3[2];
    output[15] = result3[3];
}

static void av1_dsp_butterfly4(
    Av1DspI32x4 *left,
    Av1DspI32x4 *right,
    int32_t cosine,
    int32_t sine,
    unsigned int flip) {
    Av1DspI64x4 wide_left =
        __builtin_convertvector(*left, Av1DspI64x4);
    Av1DspI64x4 wide_right =
        __builtin_convertvector(*right, Av1DspI64x4);
    Av1DspI32x4 x = __builtin_convertvector(
        (wide_left * cosine - wide_right * sine + 2048) >> 12,
        Av1DspI32x4);
    Av1DspI32x4 y = __builtin_convertvector(
        (wide_left * sine + wide_right * cosine + 2048) >> 12,
        Av1DspI32x4);

    *left = flip != 0U ? y : x;
    *right = flip != 0U ? x : y;
}

static void av1_dsp_hadamard4(
    Av1DspI32x4 *left,
    Av1DspI32x4 *right,
    unsigned int flip) {
    Av1DspI32x4 first = *left;
    Av1DspI32x4 second = *right;

    *left = av1_dsp_clip16x4(
        flip != 0U ? second - first : first + second);
    *right = av1_dsp_clip16x4(
        flip != 0U ? second + first : first - second);
}

static void av1_dsp_inverse_dct8_1d_neon(Av1DspI32x4 values[8]) {
    Av1DspI32x4 copy[8];
    unsigned int index;

    for (index = 0U; index < 8U; ++index) copy[index] = values[index];
    values[0] = copy[0];
    values[1] = copy[4];
    values[2] = copy[2];
    values[3] = copy[6];
    values[4] = copy[1];
    values[5] = copy[5];
    values[6] = copy[3];
    values[7] = copy[7];
    av1_dsp_butterfly4(&values[4], &values[7], 799, 4017, 0U);
    av1_dsp_butterfly4(&values[5], &values[6], 3406, 2276, 0U);
    av1_dsp_butterfly4(&values[0], &values[1], 2896, 2896, 1U);
    av1_dsp_butterfly4(&values[2], &values[3], 1567, 3784, 0U);
    av1_dsp_hadamard4(&values[4], &values[5], 0U);
    av1_dsp_hadamard4(&values[6], &values[7], 1U);
    av1_dsp_hadamard4(&values[0], &values[3], 0U);
    av1_dsp_hadamard4(&values[1], &values[2], 0U);
    av1_dsp_butterfly4(&values[6], &values[5], 2896, 2896, 1U);
    for (index = 0U; index < 4U; ++index) {
        av1_dsp_hadamard4(&values[index], &values[7U - index], 0U);
    }
}

static void av1_dsp_inverse_dct8_neon(
    const int32_t *input,
    int32_t *output) {
    int32_t intermediate[64];
    Av1DspI32x4 values[8];
    unsigned int group;
    unsigned int index;
    unsigned int lane;

    for (group = 0U; group < 2U; ++group) {
        for (index = 0U; index < 8U; ++index) {
            values[index] = (Av1DspI32x4){
                input[(group * 4U + 0U) * 8U + index],
                input[(group * 4U + 1U) * 8U + index],
                input[(group * 4U + 2U) * 8U + index],
                input[(group * 4U + 3U) * 8U + index]
            };
        }
        av1_dsp_inverse_dct8_1d_neon(values);
        for (index = 0U; index < 8U; ++index) {
            values[index] = (values[index] + 1) >> 1;
            for (lane = 0U; lane < 4U; ++lane) {
                intermediate[(group * 4U + lane) * 8U + index] =
                    values[index][lane];
            }
        }
    }
    for (group = 0U; group < 2U; ++group) {
        for (index = 0U; index < 8U; ++index) {
            values[index] = (Av1DspI32x4){
                intermediate[index * 8U + group * 4U + 0U],
                intermediate[index * 8U + group * 4U + 1U],
                intermediate[index * 8U + group * 4U + 2U],
                intermediate[index * 8U + group * 4U + 3U]
            };
        }
        av1_dsp_inverse_dct8_1d_neon(values);
        for (index = 0U; index < 8U; ++index) {
            values[index] = (values[index] + 8) >> 4;
            for (lane = 0U; lane < 4U; ++lane) {
                output[index * 8U + group * 4U + lane] =
                    values[index][lane];
            }
        }
    }
}

static void av1_dsp_inverse_dct16_1d_neon(Av1DspI32x4 values[16]) {
    static const uint8_t permutation[16] = {
        0U, 8U, 4U, 12U, 2U, 10U, 6U, 14U,
        1U, 9U, 5U, 13U, 3U, 11U, 7U, 15U
    };
    Av1DspI32x4 copy[16];
    unsigned int index;

    for (index = 0U; index < 16U; ++index) copy[index] = values[index];
    for (index = 0U; index < 16U; ++index) {
        values[index] = copy[permutation[index]];
    }
    av1_dsp_butterfly4(&values[8], &values[15], 401, 4076, 0U);
    av1_dsp_butterfly4(&values[9], &values[14], 3166, 2598, 0U);
    av1_dsp_butterfly4(&values[10], &values[13], 1931, 3612, 0U);
    av1_dsp_butterfly4(&values[11], &values[12], 3920, 1189, 0U);
    av1_dsp_butterfly4(&values[4], &values[7], 799, 4017, 0U);
    av1_dsp_butterfly4(&values[5], &values[6], 3406, 2276, 0U);
    av1_dsp_hadamard4(&values[8], &values[9], 0U);
    av1_dsp_hadamard4(&values[10], &values[11], 1U);
    av1_dsp_hadamard4(&values[12], &values[13], 0U);
    av1_dsp_hadamard4(&values[14], &values[15], 1U);
    av1_dsp_butterfly4(&values[0], &values[1], 2896, 2896, 1U);
    av1_dsp_butterfly4(&values[2], &values[3], 1567, 3784, 0U);
    av1_dsp_hadamard4(&values[4], &values[5], 0U);
    av1_dsp_hadamard4(&values[6], &values[7], 1U);
    av1_dsp_butterfly4(&values[14], &values[9], 1567, 3784, 1U);
    av1_dsp_butterfly4(&values[13], &values[10], -3784, 1567, 1U);
    av1_dsp_hadamard4(&values[0], &values[3], 0U);
    av1_dsp_hadamard4(&values[1], &values[2], 0U);
    av1_dsp_butterfly4(&values[6], &values[5], 2896, 2896, 1U);
    av1_dsp_hadamard4(&values[8], &values[11], 0U);
    av1_dsp_hadamard4(&values[9], &values[10], 0U);
    av1_dsp_hadamard4(&values[12], &values[15], 1U);
    av1_dsp_hadamard4(&values[13], &values[14], 1U);
    for (index = 0U; index < 4U; ++index) {
        av1_dsp_hadamard4(&values[index], &values[7U - index], 0U);
    }
    av1_dsp_butterfly4(&values[13], &values[10], 2896, 2896, 1U);
    av1_dsp_butterfly4(&values[12], &values[11], 2896, 2896, 1U);
    for (index = 0U; index < 8U; ++index) {
        av1_dsp_hadamard4(&values[index], &values[15U - index], 0U);
    }
}

static void av1_dsp_inverse_dct16_neon(
    const int32_t *input,
    int32_t *output) {
    int32_t intermediate[256];
    Av1DspI32x4 values[16];
    unsigned int group;
    unsigned int index;
    unsigned int lane;

    for (group = 0U; group < 4U; ++group) {
        for (index = 0U; index < 16U; ++index) {
            values[index] = (Av1DspI32x4){
                input[(group * 4U + 0U) * 16U + index],
                input[(group * 4U + 1U) * 16U + index],
                input[(group * 4U + 2U) * 16U + index],
                input[(group * 4U + 3U) * 16U + index]
            };
        }
        av1_dsp_inverse_dct16_1d_neon(values);
        for (index = 0U; index < 16U; ++index) {
            values[index] = (values[index] + 2) >> 2;
            for (lane = 0U; lane < 4U; ++lane) {
                intermediate[(group * 4U + lane) * 16U + index] =
                    values[index][lane];
            }
        }
    }
    for (group = 0U; group < 4U; ++group) {
        for (index = 0U; index < 16U; ++index) {
            values[index] = (Av1DspI32x4){
                intermediate[index * 16U + group * 4U + 0U],
                intermediate[index * 16U + group * 4U + 1U],
                intermediate[index * 16U + group * 4U + 2U],
                intermediate[index * 16U + group * 4U + 3U]
            };
        }
        av1_dsp_inverse_dct16_1d_neon(values);
        for (index = 0U; index < 16U; ++index) {
            values[index] = (values[index] + 8) >> 4;
            for (lane = 0U; lane < 4U; ++lane) {
                output[index * 16U + group * 4U + lane] =
                    values[index][lane];
            }
        }
    }
}
#endif

void av1_dsp_inverse_dct4(const int32_t *input, int32_t *output) {
#if defined(AVIFDEC_AARCH64_NEON)
    av1_dsp_inverse_dct4_neon(input, output);
#else
    av1_dsp_inverse_dct4_c(input, output);
#endif
}

void av1_dsp_inverse_dct8(const int32_t *input, int32_t *output) {
#if defined(AVIFDEC_AARCH64_NEON)
    av1_dsp_inverse_dct8_neon(input, output);
#else
    av1_dsp_inverse_dct8_c(input, output);
#endif
}

void av1_dsp_inverse_dct16(const int32_t *input, int32_t *output) {
#if defined(AVIFDEC_AARCH64_NEON)
    av1_dsp_inverse_dct16_neon(input, output);
#else
    av1_dsp_inverse_dct16_c(input, output);
#endif
}

#if defined(AVIFDEC_AARCH64_NEON)
void av1_dsp_add_residual_8bpc_neon(uint16_t *destination,
                                    size_t stride,
                                    size_t width,
                                    size_t height,
                                    const int32_t *residual);
#endif

void av1_dsp_add_residual_c(uint16_t *destination,
                            size_t stride,
                            size_t width,
                            size_t height,
                            const int32_t *residual,
                            uint8_t bit_depth,
                            uint8_t flip_lr,
                            uint8_t flip_ud) {
    size_t row;
    size_t column;
    int32_t maximum = ((int32_t)1 << bit_depth) - 1;

    for (row = 0U; row < height; ++row) {
        size_t output_row = flip_ud != 0U ? height - row - 1U : row;
        for (column = 0U; column < width; ++column) {
            size_t output_column =
                flip_lr != 0U ? width - column - 1U : column;
            int32_t value =
                destination[output_row * stride + output_column] +
                residual[row * width + column];
            if (value < 0) value = 0;
            if (value > maximum) value = maximum;
            destination[output_row * stride + output_column] =
                (uint16_t)value;
        }
    }
}

void av1_dsp_add_residual(uint16_t *destination,
                          size_t stride,
                          size_t width,
                          size_t height,
                          const int32_t *residual,
                          uint8_t bit_depth,
                          uint8_t flip_lr,
                          uint8_t flip_ud) {
#if defined(AVIFDEC_AARCH64_NEON)
    if (bit_depth == 8U && flip_lr == 0U && flip_ud == 0U &&
        width >= 8U && (width & 7U) == 0U) {
        av1_dsp_add_residual_8bpc_neon(
            destination, stride, width, height, residual);
        return;
    }
#endif
    av1_dsp_add_residual_c(
        destination, stride, width, height, residual,
        bit_depth, flip_lr, flip_ud);
}