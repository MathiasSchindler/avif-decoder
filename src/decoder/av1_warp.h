#ifndef AVIFDEC_AV1_WARP_H
#define AVIFDEC_AV1_WARP_H

#include <stddef.h>
#include <stdint.h>

#define AV1_WARP_MODEL_PREC_BITS 16
#define AV1_WARP_SAMPLE_PREC_BITS 3
#define AV1_WARP_MAX_SAMPLES 8

typedef enum Av1WarpStatus {
    AV1_WARP_OK = 0,
    AV1_WARP_INVALID_ARGUMENT = 1,
    AV1_WARP_INVALID_MODEL = 2,
    AV1_WARP_SINGULAR = 3
} Av1WarpStatus;

/*
 * matrix is the Q16 affine transform:
 *   x' = matrix[2] * x + matrix[3] * y + matrix[0]
 *   y' = matrix[4] * x + matrix[5] * y + matrix[1]
 */
typedef struct Av1WarpModel {
    int32_t matrix[6];
} Av1WarpModel;

typedef struct Av1WarpShear {
    int32_t alpha;
    int32_t beta;
    int32_t gamma;
    int32_t delta;
} Av1WarpShear;

/* Sample coordinates use one eighth luma-sample units. */
typedef struct Av1WarpSample {
    int32_t source_x;
    int32_t source_y;
    int32_t destination_x;
    int32_t destination_y;
} Av1WarpSample;

typedef struct Av1WarpPlaneParams {
    const uint16_t *source;
    size_t source_stride;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t block_x;
    uint32_t block_y;
    uint32_t block_width;
    uint32_t block_height;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t bit_depth;
    const Av1WarpModel *model;
} Av1WarpPlaneParams;

Av1WarpStatus av1_warp_resolve_divisor(int64_t divisor,
                                        uint8_t *shift,
                                        int32_t *factor);

Av1WarpStatus av1_warp_derive_shear(const Av1WarpModel *model,
                                    Av1WarpShear *shear);

int av1_warp_model_is_valid(const Av1WarpModel *model);

/*
 * center_x and center_y are luma-sample coordinates. mv_x and mv_y use
 * one eighth luma-sample units.
 */
Av1WarpStatus av1_warp_project_samples(Av1WarpModel *model,
                                       const Av1WarpSample *samples,
                                       size_t sample_count,
                                       int32_t center_x,
                                       int32_t center_y,
                                       int32_t mv_x,
                                       int32_t mv_y);

Av1WarpStatus av1_warp_predict_single(const Av1WarpPlaneParams *params,
                                      uint16_t *destination,
                                      size_t destination_stride);

/*
 * Compound output retains the AV1 convolution offset and InterPostRound
 * fractional bits for use by the compound blending process.
 */
Av1WarpStatus av1_warp_predict_compound(const Av1WarpPlaneParams *params,
                                        uint16_t *compound,
                                        size_t compound_stride);

#endif
