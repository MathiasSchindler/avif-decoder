#ifndef AVIFENC_AV1_WRITE_H
#define AVIFENC_AV1_WRITE_H

#include "encoder/write.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    AvifencColor color;
    uint16_t quantizer;
    AvifencQuantization quantization;
} AvifencAv1Config;

typedef struct {
    uint16_t columns;
    uint16_t rows;
    uint16_t tile_width_sb;
    uint16_t tile_height_sb;
    uint8_t columns_log2;
    uint8_t rows_log2;
    uint8_t tile_size_bytes;
} AvifencAv1TileLayout;

typedef struct {
    const uint8_t *data;
    size_t size;
} AvifencAv1TilePayload;

AvifencStatus avifenc_av1_select_level(uint32_t width,
                                       uint32_t height,
                                       uint8_t *level);
AvifencStatus avifenc_av1_write(AvifencByteWriter *writer,
                                const AvifencAv1Config *config);
AvifencStatus avifenc_av1_write_with_tile(
    AvifencByteWriter *writer,
    const AvifencAv1Config *config,
    const uint8_t *tile_payload,
    size_t tile_payload_size);
AvifencStatus avifenc_av1_tile_layout(
    uint32_t width,
    uint32_t height,
    AvifencAv1TileLayout *layout);
AvifencStatus avifenc_av1_write_with_tiles(
    AvifencByteWriter *writer,
    const AvifencAv1Config *config,
    const AvifencAv1TileLayout *layout,
    const AvifencAv1TilePayload *tile_payloads,
    size_t tile_count);

#endif