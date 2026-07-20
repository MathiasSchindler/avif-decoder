#ifndef AVIFENC_AV1_TILE_PALETTE_H
#define AVIFENC_AV1_TILE_PALETTE_H

#include "encoder/av1_tile_internal.h"

unsigned int avifenc_av1_tile_palette_block_context(uint32_t width_mi,
                                        uint32_t height_mi);
int avifenc_av1_tile_palette_classify_luma(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    AvifencAv1LumaDecision *decision);
int avifenc_av1_tile_palette_classify_chroma(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    AvifencAv1ChromaDecision *decision);
uint64_t avifenc_av1_tile_palette_luma_rate_cost(
    const AvifencAv1TileState *state,
    uint32_t width_mi,
    uint32_t height_mi,
    const AvifencAv1LumaDecision *decision);
uint64_t avifenc_av1_tile_palette_chroma_rate_cost(
    const AvifencAv1TileState *state,
    uint32_t width_mi,
    uint32_t height_mi,
    const AvifencAv1ChromaDecision *decision);
AvifencStatus avifenc_av1_tile_write_palette_luma_colors(
    AvifencAv1TileState *state,
    const AvifencAv1LumaDecision *decision);
AvifencStatus avifenc_av1_tile_write_palette_chroma_colors(
    AvifencAv1TileState *state,
    const AvifencAv1ChromaDecision *decision);
AvifencStatus avifenc_av1_tile_write_palette_map(
    AvifencAv1TileState *state,
    const uint8_t *map,
    uint32_t width,
    uint32_t height,
    uint8_t palette_size,
    unsigned int plane);

#endif
