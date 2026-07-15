#ifndef AVIFDEC_AV1_METADATA_H
#define AVIFDEC_AV1_METADATA_H

#include "av1_bitstream.h"

typedef struct {
    uint32_t max_width;
    uint32_t max_height;
    uint32_t num_units_in_display_tick;
    uint32_t time_scale;
    uint32_t num_ticks_per_picture_minus_1;
    uint8_t timing_info_present;
    uint8_t equal_picture_interval;
} Av1MetadataConfig;

void av1_metadata_reset(AvifdecImageInfo *info);

AvifdecStatus av1_metadata_parse(Av1Bits *bits,
                                 const Av1MetadataConfig *config,
                                 uint8_t extension_flag,
                                 AvifdecImageInfo *info);

#endif
