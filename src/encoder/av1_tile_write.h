#ifndef AVIFENC_AV1_TILE_WRITE_H
#define AVIFENC_AV1_TILE_WRITE_H

#include "encoder/av1_symbol_write.h"

typedef struct {
    const uint8_t *planes[3];
    size_t strides[3];
    uint32_t width;
    uint32_t height;
    uint16_t quantizer;
} AvifencAv1TileSource;

typedef struct {
    uint16_t *planes[3];
    size_t strides[3];
    uint32_t widths[3];
    uint32_t heights[3];
} AvifencAv1TileReconstruction;

typedef struct {
    size_t workspace_required;
    uint32_t reconstruction_widths[3];
    uint32_t reconstruction_heights[3];
} AvifencAv1TileRequirements;

AvifencStatus avifenc_av1_tile_query(
    const AvifencAv1TileSource *source,
    AvifencAv1TileRequirements *requirements);
AvifencStatus avifenc_av1_tile_write(
    AvifencAv1SymbolWriter *writer,
    const AvifencAv1TileSource *source,
    AvifencAv1TileReconstruction *reconstruction,
    void *workspace,
    size_t workspace_size);

#endif