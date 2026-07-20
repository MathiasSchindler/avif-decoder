#ifndef AVIFDEC_AV1_TILE_GEOMETRY_H
#define AVIFDEC_AV1_TILE_GEOMETRY_H

#include "av1_tile.h"

extern const uint8_t av1_tile_block_width_mi[AV1_BLOCK_SIZES];
extern const uint8_t av1_tile_block_height_mi[AV1_BLOCK_SIZES];
extern const uint8_t av1_tile_max_tx_size[AV1_BLOCK_SIZES];
extern const uint8_t av1_tile_max_tx_depth[AV1_BLOCK_SIZES];
extern const uint8_t av1_tile_split_tx_size[19];
extern const uint8_t av1_tile_tx_width[19];
extern const uint8_t av1_tile_tx_height[19];

AvifdecStatus av1_tile_find_block_size(uint32_t width,
                                       uint32_t height,
                                       uint8_t *block_size);

#endif
