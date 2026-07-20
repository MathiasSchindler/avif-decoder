#ifndef AVIFDEC_AV1_TILE_MODE_H
#define AVIFDEC_AV1_TILE_MODE_H

#include "av1_tile.h"

AvifdecStatus av1_tile_read_skip(Av1SymbolDecoder *decoder,
                                 Av1TileCdfs *cdfs,
                                 const Av1BlockState *state,
                                 const Av1BlockAvailability *availability,
                                 uint32_t row,
                                 uint32_t column,
                                 int forced_skip,
                                 uint8_t *skip);
AvifdecStatus av1_tile_read_segment_id(Av1SymbolDecoder *decoder,
                                       Av1TileCdfs *cdfs,
                                       const Av1BlockState *state,
                                       const Av1BlockAvailability *availability,
                                       uint32_t row,
                                       uint32_t column,
                                       uint8_t last_active_segment,
                                       int skip,
                                       uint8_t *segment_id);
AvifdecStatus av1_tile_read_delta(Av1SymbolDecoder *decoder,
                                  uint16_t cdf[5],
                                  int32_t *delta);
AvifdecStatus av1_tile_read_y_mode(Av1SymbolDecoder *decoder,
                                   Av1TileCdfs *cdfs,
                                   uint32_t width,
                                   uint32_t height,
                                   uint8_t *y_mode);
AvifdecStatus av1_tile_read_intra_frame_y_mode(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1BlockState *state,
    const Av1BlockAvailability *availability,
    uint32_t row,
    uint32_t column,
    uint8_t *y_mode);
AvifdecStatus av1_tile_read_angle_delta(Av1SymbolDecoder *decoder,
                                        Av1TileCdfs *cdfs,
                                        uint8_t mode,
                                        int8_t *angle_delta);
AvifdecStatus av1_tile_read_tx_depth(Av1SymbolDecoder *decoder,
                                     Av1TileCdfs *cdfs,
                                     unsigned int maximum_tx_log2,
                                     unsigned int maximum_depth,
                                     unsigned int context,
                                     uint8_t *tx_depth);
AvifdecStatus av1_tile_decode_partitions(const Av1TilePartitionConfig *config,
                                         const Av1TileCdfs *frame_cdfs,
                                         Av1TileCdfs *tile_cdfs,
                                         Av1PartitionTrace *trace);
AvifdecStatus av1_tile_decode_modes(
    const Av1TilePartitionConfig *partition_config,
    const Av1TileModeConfig *mode_config,
    const Av1TileCdfs *frame_cdfs,
    Av1TileCdfs *tile_cdfs,
    Av1PartitionTrace *partition_trace);

#endif
