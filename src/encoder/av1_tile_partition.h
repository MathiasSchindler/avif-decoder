#ifndef AVIFENC_AV1_TILE_PARTITION_H
#define AVIFENC_AV1_TILE_PARTITION_H

#include "encoder/av1_tile_internal.h"

AvifencStatus avifenc_av1_tile_requirements(
    const AvifencAv1TileSource *source,
    AvifencAv1TileRequirements *requirements);
void avifenc_av1_tile_cdfs_init(AvifencAv1TileState *state);
AvifencStatus avifenc_av1_tile_write_partition(AvifencAv1TileState *state,
                                   uint32_t row,
                                   uint32_t column,
                                   uint32_t block_mi);

#endif
