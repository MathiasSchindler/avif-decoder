#ifndef AVIFDEC_AV1_TILE_INTER_MODE_H
#define AVIFDEC_AV1_TILE_INTER_MODE_H

#include "av1_tile.h"

AvifdecStatus av1_tile_inter_read_interp_filters(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields);

AvifdecStatus av1_tile_inter_read_compound_syntax(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    uint8_t block_size);

#endif
