#include "av1_profile.h"

typedef struct {
    uint32_t picture_size;
    uint32_t width;
    uint32_t height;
} Av1LevelLimits;

AvifdecStatus av1_profile_validate(uint8_t profile,
                                   uint8_t bit_depth,
                                   uint8_t monochrome,
                                   uint8_t subsampling_x,
                                   uint8_t subsampling_y) {
    if (profile == 0U) {
        if ((bit_depth != 8U && bit_depth != 10U) ||
            (!monochrome &&
             (subsampling_x != 1U || subsampling_y != 1U))) {
            return AVIFDEC_INVALID_DATA;
        }
        return AVIFDEC_OK;
    }
    if (profile == 1U) {
        if ((bit_depth != 8U && bit_depth != 10U) ||
            monochrome || subsampling_x != 0U ||
            subsampling_y != 0U) {
            return AVIFDEC_INVALID_DATA;
        }
        return AVIFDEC_OK;
    }
    if (profile == 2U) {
        if (bit_depth != 8U && bit_depth != 10U &&
            bit_depth != 12U) {
            return AVIFDEC_INVALID_DATA;
        }
        if (!monochrome) {
            if (bit_depth < 12U) {
                if (subsampling_x != 1U || subsampling_y != 0U) {
                    return AVIFDEC_INVALID_DATA;
                }
            } else if (!subsampling_x && subsampling_y) {
                return AVIFDEC_INVALID_DATA;
            }
        }
        return AVIFDEC_OK;
    }
    return AVIFDEC_INVALID_DATA;
}

static int av1_level_limits(uint8_t level, Av1LevelLimits *limits) {
    if (level == 0U) {
        limits->picture_size = 147456U;
        limits->width = 2048U;
        limits->height = 1152U;
    } else if (level == 1U) {
        limits->picture_size = 278784U;
        limits->width = 2816U;
        limits->height = 1584U;
    } else if (level == 4U) {
        limits->picture_size = 665856U;
        limits->width = 4352U;
        limits->height = 2448U;
    } else if (level == 5U) {
        limits->picture_size = 1065024U;
        limits->width = 5504U;
        limits->height = 3096U;
    } else if (level == 8U || level == 9U) {
        limits->picture_size = 2359296U;
        limits->width = 6144U;
        limits->height = 3456U;
    } else if (level >= 12U && level <= 15U) {
        limits->picture_size = 8912896U;
        limits->width = 8192U;
        limits->height = 4352U;
    } else if (level >= 16U && level <= 19U) {
        limits->picture_size = 35651584U;
        limits->width = 16384U;
        limits->height = 8704U;
    } else {
        return 0;
    }
    return 1;
}

AvifdecStatus av1_level_validate_dimensions(uint8_t level,
                                            uint32_t width,
                                            uint32_t height,
                                            int validate_picture_size) {
    Av1LevelLimits limits;

    if (width == 0U || height == 0U) {
        return AVIFDEC_INVALID_DATA;
    }
    if (level == 31U) return AVIFDEC_OK;
    if (!av1_level_limits(level, &limits)) {
        return AVIFDEC_INVALID_DATA;
    }
    if (width > limits.width || height > limits.height ||
        (validate_picture_size &&
         (uint64_t)width * height > limits.picture_size)) {
        return AVIFDEC_INVALID_DATA;
    }
    return AVIFDEC_OK;
}
