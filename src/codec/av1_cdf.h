#ifndef AVIF_AV1_CDF_H
#define AVIF_AV1_CDF_H

#include <stddef.h>
#include <stdint.h>

void av1_cdf_update(uint16_t *cdf, size_t symbols, size_t symbol);

#endif