#include "av1_cdf.h"

#define AV1_CDF_PROB_TOP 32768U

static unsigned int av1_cdf_floor_log2(uint32_t value) {
    unsigned int result = 0U;

    while (value > 1U) {
        value >>= 1U;
        ++result;
    }
    return result;
}

void av1_cdf_update(uint16_t *cdf, size_t symbols, size_t symbol) {
    unsigned int rate = 3U + (cdf[symbols] > 15U) +
                        (cdf[symbols] > 31U);
    unsigned int symbol_bits = av1_cdf_floor_log2((uint32_t)symbols);
    uint32_t target = 0U;
    size_t index;

    rate += symbol_bits < 2U ? symbol_bits : 2U;
    for (index = 0U; index + 1U < symbols; ++index) {
        if (index == symbol) target = AV1_CDF_PROB_TOP;
        if (target < cdf[index]) {
            cdf[index] = (uint16_t)(
                cdf[index] - ((cdf[index] - target) >> rate));
        } else {
            cdf[index] = (uint16_t)(
                cdf[index] + ((target - cdf[index]) >> rate));
        }
    }
    if (cdf[symbols] < 32U) ++cdf[symbols];
}