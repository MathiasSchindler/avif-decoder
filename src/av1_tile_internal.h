#ifndef AVIFDEC_AV1_TILE_INTERNAL_H
#define AVIFDEC_AV1_TILE_INTERNAL_H

#include "av1_tile.h"

const Av1BlockCell *av1_block_cell(const Av1BlockState *state,
                                   uint32_t row,
                                   uint32_t column);

AvifdecStatus av1_tile_read_palette_info(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    uint8_t block_size);

AvifdecStatus av1_tile_read_palette_tokens(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields);

#endif
