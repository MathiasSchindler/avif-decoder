#ifndef AVIFENC_AV1_TILE_INTERNAL_H
#define AVIFENC_AV1_TILE_INTERNAL_H

#include "encoder/av1_tile_write.h"
#include "encoder/av1_transform_write.h"
#include "av1_intra.h"
#include "av1_tile.h"
#include "base.h"

typedef struct {
    AvifencAv1SymbolWriter *writer;
    const AvifencAv1TileSource *source;
    AvifencAv1TileReconstruction *reconstruction;
    uint16_t quantizer;
    uint32_t mi_rows;
    uint32_t mi_columns;
    uint8_t *block_widths;
    uint8_t *block_heights;
    uint8_t *block_flags;
    uint8_t *segment_ids;
    uint8_t *y_modes;
    uint8_t *uv_modes;
    uint8_t *palette_sizes_y;
    uint8_t *palette_sizes_uv;
    uint8_t *palette_map_y;
    uint8_t *palette_map_uv;
    uint8_t *trial_reconstruction;
    AvifencAv1TransformState transform;
    Av1IntraCdfs intra_cdfs;
    Av1PaletteCdfs palette_cdfs;
    uint16_t partition8[4][5];
    uint16_t partition16[4][11];
    uint16_t partition32[4][11];
    uint16_t partition64[4][11];
    uint16_t skip[3][3];
    uint16_t segment_id[3][9];
} AvifencAv1TileState;

typedef struct {
    uint8_t mode;
    int8_t angle_delta;
    int8_t filter_intra_mode;
    uint8_t palette_size;
    uint16_t palette_colors[8];
} AvifencAv1LumaDecision;

typedef struct {
    uint8_t mode;
    int8_t angle_delta;
    int8_t alpha_u;
    int8_t alpha_v;
    uint8_t palette_size;
    uint16_t palette_colors_u[8];
    uint16_t palette_colors_v[8];
} AvifencAv1ChromaDecision;

static inline uint64_t tile_symbol_cost(const uint16_t *cdf, size_t symbol) {
    uint32_t low = symbol == 0U ? 0U : cdf[symbol - 1U];
    uint32_t probability = cdf[symbol] - low;
    uint64_t cost = 0U;

    if (probability == 0U) probability = 1U;
    while (probability < 32768U) {
        probability <<= 1U;
        cost += 256U;
    }
    return cost;
}

static inline uint64_t tile_candidate_score(uint64_t distortion,
                                            uint64_t rate_cost,
                                            uint16_t quantizer) {
    uint64_t lambda = 1U +
        ((uint64_t)quantizer * quantizer >> 8U);

    return distortion * 256U + rate_cost * lambda;
}

#endif
