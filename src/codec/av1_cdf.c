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
    unsigned int symbol_bits;
    uint32_t target = 0U;
    size_t index;

    if (symbols == 4U && symbol < 4U) {
        rate += 2U;
        cdf[0] = symbol == 0U
            ? (uint16_t)(cdf[0] + ((AV1_CDF_PROB_TOP - cdf[0]) >> rate))
            : (uint16_t)(cdf[0] - (cdf[0] >> rate));
        cdf[1] = symbol <= 1U
            ? (uint16_t)(cdf[1] + ((AV1_CDF_PROB_TOP - cdf[1]) >> rate))
            : (uint16_t)(cdf[1] - (cdf[1] >> rate));
        cdf[2] = symbol <= 2U
            ? (uint16_t)(cdf[2] + ((AV1_CDF_PROB_TOP - cdf[2]) >> rate))
            : (uint16_t)(cdf[2] - (cdf[2] >> rate));
        if (cdf[4] < 32U) ++cdf[4];
        return;
    }
    symbol_bits = av1_cdf_floor_log2((uint32_t)symbols);
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