#ifndef AVIFENC_AV1_WRITE_H
#define AVIFENC_AV1_WRITE_H

#include "encoder/write.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    AvifencColor color;
    uint16_t quantizer;
} AvifencAv1Config;

AvifencStatus avifenc_av1_select_level(uint32_t width,
                                       uint32_t height,
                                       uint8_t *level);
AvifencStatus avifenc_av1_write(AvifencByteWriter *writer,
                                const AvifencAv1Config *config);

#endif