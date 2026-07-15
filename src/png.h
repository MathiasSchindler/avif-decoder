#ifndef AVIFDEC_PNG_H
#define AVIFDEC_PNG_H

#include "avifdec.h"
#include <stddef.h>
#include <stdint.h>

typedef int (*AvifdecPngWrite)(
    void *user_data, const void *data, size_t size);

typedef struct {
    uint32_t pixel_aspect_h_spacing;
    uint32_t pixel_aspect_v_spacing;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint8_t has_nclx;
} AvifdecPngMetadata;

AvifdecStatus avifdec_png_write(
    AvifdecPngWrite write_callback,
    void *user_data,
    const void *pixels,
    size_t stride,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    uint8_t bit_depth,
    const AvifdecPngMetadata *metadata);

#endif
