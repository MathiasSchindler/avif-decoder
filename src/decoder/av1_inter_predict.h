#ifndef AV1_INTER_PREDICT_H
#define AV1_INTER_PREDICT_H

#include <stddef.h>
#include <stdint.h>

#include "avifdec.h"

typedef enum Av1InterpFilter {
    AV1_INTERP_EIGHTTAP_REGULAR = 0,
    AV1_INTERP_EIGHTTAP_SMOOTH = 1,
    AV1_INTERP_EIGHTTAP_SHARP = 2,
    AV1_INTERP_BILINEAR = 3
} Av1InterpFilter;

typedef enum Av1InterIntraMode {
    AV1_INTERINTRA_DC = 0,
    AV1_INTERINTRA_VERTICAL = 1,
    AV1_INTERINTRA_HORIZONTAL = 2,
    AV1_INTERINTRA_SMOOTH = 3
} Av1InterIntraMode;

typedef struct Av1InterPredictParams {
    const uint16_t *src;
    size_t src_stride;
    uint16_t *dst;
    size_t dst_stride;
    uint32_t src_width;
    uint32_t src_height;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t block_x;
    uint32_t block_y;
    uint32_t block_width;
    uint32_t block_height;
    int32_t mv_col;
    int32_t mv_row;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t bit_depth;
    uint8_t filter_x;
    uint8_t filter_y;
} Av1InterPredictParams;

typedef struct Av1CompoundParams {
    uint16_t *dst;
    size_t dst_stride;
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t weight0;
    uint8_t weight1;
    const uint8_t *mask;
    size_t mask_stride;
} Av1CompoundParams;

AvifdecStatus av1_inter_predict_single(const Av1InterPredictParams *params);

AvifdecStatus av1_inter_predict_compound(const Av1InterPredictParams *params,
                                         uint16_t *compound,
                                         size_t compound_stride);

AvifdecStatus av1_inter_blend_average(const Av1CompoundParams *params,
                                      const uint16_t *pred0,
                                      size_t pred0_stride,
                                      const uint16_t *pred1,
                                      size_t pred1_stride);

AvifdecStatus av1_inter_blend_masked(const Av1CompoundParams *params,
                                     const uint16_t *pred0,
                                     size_t pred0_stride,
                                     const uint16_t *pred1,
                                     size_t pred1_stride);

AvifdecStatus av1_inter_build_diff_mask(uint8_t *mask,
                                        size_t mask_stride,
                                        const uint16_t *pred0,
                                        size_t pred0_stride,
                                        const uint16_t *pred1,
                                        size_t pred1_stride,
                                        uint32_t width,
                                        uint32_t height,
                                        uint8_t bit_depth,
                                        uint8_t inverse);

AvifdecStatus av1_inter_build_wedge_mask(uint8_t *mask,
                                         size_t mask_stride,
                                         uint32_t width,
                                         uint32_t height,
                                         uint8_t wedge_index,
                                         uint8_t wedge_sign);

AvifdecStatus av1_inter_build_interintra_mask(uint8_t *mask,
                                              size_t mask_stride,
                                              uint32_t width,
                                              uint32_t height,
                                              uint8_t mode);

void av1_inter_blend_interintra(uint16_t *dst,
                                size_t dst_stride,
                                const uint16_t *inter,
                                size_t inter_stride,
                                const uint16_t *intra,
                                size_t intra_stride,
                                const uint8_t *mask,
                                size_t mask_stride,
                                uint32_t width,
                                uint32_t height);

AvifdecStatus av1_inter_blend_obmc(uint16_t *dst,
                                   size_t dst_stride,
                                   const uint16_t *neighbor,
                                   size_t neighbor_stride,
                                   uint32_t width,
                                   uint32_t height,
                                   int from_left);

#endif
