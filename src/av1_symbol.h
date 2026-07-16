#ifndef AVIFDEC_AV1_SYMBOL_H
#define AVIFDEC_AV1_SYMBOL_H

#include "avifdec.h"

typedef struct {
    const AvifdecSpan *spans;
    size_t span_count;
    const unsigned char *contiguous_data;
    size_t start;
    size_t size;
    size_t bit_position;
    uint32_t range;
    uint32_t value;
    int64_t max_bits;
    AvifdecStatus status;
    uint8_t disable_cdf_update;
} Av1SymbolDecoder;

AvifdecStatus av1_symbol_init(Av1SymbolDecoder *decoder,
                              const AvifdecSpan *spans,
                              size_t span_count,
                              size_t start,
                              size_t size,
                              int disable_cdf_update);
uint32_t av1_symbol_read_literal(Av1SymbolDecoder *decoder, unsigned int bits);
uint32_t av1_symbol_read(Av1SymbolDecoder *decoder, uint16_t *cdf, size_t symbols);
AvifdecStatus av1_symbol_exit(Av1SymbolDecoder *decoder);

#endif