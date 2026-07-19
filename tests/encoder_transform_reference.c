#include "encoder/av1_transform_write.h"

void av1_fwd_txfm2d_4x4_c(const int16_t *input,
                           int32_t *output,
                           int stride,
                           int tx_type,
                           int bit_depth);

static int compare_block(const int16_t input[16]) {
    int32_t local[16];
    int32_t reference[16];
    size_t index;

    if (avifenc_av1_forward_dct_4x4(input, 4U, local) != AVIFENC_OK) return 1;
    av1_fwd_txfm2d_4x4_c(input, reference, 4, 0, 8);
    for (index = 0U; index < 16U; ++index) {
        if (local[index] != reference[index]) return 1;
    }
    return 0;
}

int main(void) {
    int16_t input[16];
    uint32_t random_state = 0x6d2b79f5U;
    unsigned int iteration;
    size_t index;

    for (index = 0U; index < 16U; ++index) input[index] = 1;
    if (compare_block(input) != 0) return 1;
    for (index = 0U; index < 16U; ++index) input[index] = 0;
    input[0] = 1;
    if (compare_block(input) != 0) return 2;
    for (index = 0U; index < 16U; ++index) {
        input[index] = (index & 1U) != 0U ? 255 : -255;
    }
    if (compare_block(input) != 0) return 3;
    for (iteration = 0U; iteration < 1000U; ++iteration) {
        for (index = 0U; index < 16U; ++index) {
            random_state = random_state * 1664525U + 1013904223U;
            input[index] = (int16_t)((int32_t)(random_state >> 24U) - 128);
        }
        if (compare_block(input) != 0) return 4;
    }
    return 0;
}