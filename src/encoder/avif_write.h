#ifndef AVIFENC_AVIF_WRITE_H
#define AVIFENC_AVIF_WRITE_H

#include "encoder/write.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    AvifencColor color;
    uint8_t seq_level_idx_0;
} AvifencAvifConfig;

AvifencStatus avifenc_avif_write(AvifencByteWriter *writer,
                                 const AvifencAvifConfig *config,
                                 const void *av1_payload,
                                 size_t av1_payload_size);

#endif