#ifndef AVIFDEC_AV1_FILTER_H
#define AVIFDEC_AV1_FILTER_H

#include "av1_tile.h"

typedef struct {
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t mi_rows;
    uint32_t mi_columns;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t level[4];
    uint8_t sharpness;
    uint8_t delta_enabled;
    uint8_t delta_lf_multi;
    int8_t ref_deltas[8];
    int8_t mode_deltas[2];
    const uint8_t *segment_feature_enabled;
    const int16_t *segment_feature_data;
    const uint8_t *segment_lossless;
    const uint8_t *tx_sizes[3];
    size_t tx_size_capacity;
} Av1LoopFilterParams;

AvifdecStatus av1_loop_filter_sample(uint16_t *q0,
                                     ptrdiff_t step,
                                     unsigned int plane,
                                     unsigned int filter_size,
                                     unsigned int limit,
                                     unsigned int blimit,
                                     unsigned int thresh,
                                     uint8_t bit_depth);
AvifdecStatus av1_loop_filter_frame(Av1FramePlanes *planes,
                                    const Av1BlockState *blocks,
                                    const Av1LoopFilterParams *params);
AvifdecStatus av1_loop_filter_frame_ex(
    Av1FramePlanes *planes,
    const Av1BlockState *blocks,
    const Av1LoopFilterParams *params,
    const AvifdecExecutor *executor);

typedef struct {
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t mi_rows;
    uint32_t mi_columns;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t damping;
    uint8_t bits;
    const uint8_t *y_pri_strength;
    const uint8_t *y_sec_strength;
    const uint8_t *uv_pri_strength;
    const uint8_t *uv_sec_strength;
    const uint8_t *indices;
    size_t index_capacity;
} Av1CdefParams;

AvifdecStatus av1_cdef_find_direction(const uint16_t *source,
                                      size_t stride,
                                      uint8_t bit_depth,
                                      uint8_t *direction,
                                      uint32_t *variance);
AvifdecStatus av1_cdef_frame(Av1FramePlanes *output,
                             const Av1FramePlanes *input,
                             const Av1BlockState *blocks,
                             const Av1CdefParams *params);
AvifdecStatus av1_cdef_frame_ex(
    Av1FramePlanes *output,
    const Av1FramePlanes *input,
    const Av1BlockState *blocks,
    const Av1CdefParams *params,
    const AvifdecExecutor *executor);

AvifdecStatus av1_superres_upscale_plane(uint16_t *output,
                                         size_t output_stride,
                                         uint32_t output_width,
                                         const uint16_t *input,
                                         size_t input_stride,
                                         uint32_t input_width,
                                         uint32_t padded_input_width,
                                         uint32_t height,
                                         uint8_t bit_depth);
AvifdecStatus av1_superres_upscale_plane_ex(
    uint16_t *output,
    size_t output_stride,
    uint32_t output_width,
    const uint16_t *input,
    size_t input_stride,
    uint32_t input_width,
    uint32_t padded_input_width,
    uint32_t height,
    uint8_t bit_depth,
    const AvifdecExecutor *executor);

AvifdecStatus av1_loop_restoration_frame(
    Av1FramePlanes *output,
    const Av1FramePlanes *upscaled_cdef,
    const Av1FramePlanes *upscaled_deblocked,
    const Av1RestorationState *restoration,
    uint8_t bit_depth);
AvifdecStatus av1_loop_restoration_frame_ex(
    Av1FramePlanes *output,
    const Av1FramePlanes *upscaled_cdef,
    const Av1FramePlanes *upscaled_deblocked,
    const Av1RestorationState *restoration,
    uint8_t bit_depth,
    const AvifdecExecutor *executor);

#endif