#ifndef AVIFDEC_AV1_PROFILE_H
#define AVIFDEC_AV1_PROFILE_H

#include "avifdec.h"

AvifdecStatus av1_profile_validate(uint8_t profile,
                                   uint8_t bit_depth,
                                   uint8_t monochrome,
                                   uint8_t subsampling_x,
                                   uint8_t subsampling_y);

AvifdecStatus av1_level_validate_dimensions(uint8_t level,
                                            uint32_t width,
                                            uint32_t height,
                                            int validate_picture_size);

#endif
