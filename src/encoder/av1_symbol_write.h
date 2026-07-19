#ifndef AVIFENC_AV1_SYMBOL_WRITE_H
#define AVIFENC_AV1_SYMBOL_WRITE_H

#include "encoder/avifenc.h"

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t position;
    uint64_t low;
    uint16_t range;
    int16_t count;
    AvifencStatus status;
    uint8_t disable_cdf_update;
    uint8_t sizing_only;
    uint8_t finalized;
} AvifencAv1SymbolWriter;

void avifenc_av1_symbol_writer_init(AvifencAv1SymbolWriter *writer,
                                    void *data,
                                    size_t capacity,
                                    int disable_cdf_update);
void avifenc_av1_symbol_writer_init_sizing(
    AvifencAv1SymbolWriter *writer,
    int disable_cdf_update);
size_t avifenc_av1_symbol_writer_size(
    const AvifencAv1SymbolWriter *writer);
AvifencStatus avifenc_av1_symbol_writer_write(
    AvifencAv1SymbolWriter *writer,
    uint16_t *cdf,
    size_t symbols,
    size_t symbol);
AvifencStatus avifenc_av1_symbol_writer_literal(
    AvifencAv1SymbolWriter *writer,
    uint32_t value,
    unsigned int bit_count);
AvifencStatus avifenc_av1_symbol_writer_finish(
    AvifencAv1SymbolWriter *writer);

#endif