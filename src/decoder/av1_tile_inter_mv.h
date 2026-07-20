#ifndef AVIFDEC_AV1_TILE_INTER_MV_H
#define AVIFDEC_AV1_TILE_INTER_MV_H

#include "av1_tile.h"

AvifdecStatus av1_tile_inter_read_mode_and_mvs(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields);

AvifdecStatus av1_tile_inter_read_intrabc(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields);

uint8_t av1_tile_inter_component_mode(uint8_t mode, unsigned int list);

#endif
