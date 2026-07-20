#ifndef AVIFENC_AV1_TILE_INTRA_H
#define AVIFENC_AV1_TILE_INTRA_H

#include "encoder/av1_tile_internal.h"

AvifencStatus avifenc_av1_tile_trial_block_sized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    uint64_t *score);
AvifencStatus avifenc_av1_tile_write_block(AvifencAv1TileState *state,
                               uint32_t row,
                               uint32_t column);
AvifencStatus avifenc_av1_tile_write_block_sized(AvifencAv1TileState *state,
                                     uint32_t row,
                                     uint32_t column,
                                     uint32_t width_mi,
                                     uint32_t height_mi);

#endif
