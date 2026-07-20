#ifndef AVIFDEC_AV1_COPY_H
#define AVIFDEC_AV1_COPY_H

#include "av1_tile.h"

typedef struct {
    uint16_t *dst_data[3];
    size_t dst_stride[3];
    const uint16_t *src_data[3];
    size_t src_stride[3];
    uint32_t width[3];
    size_t row_offset[4];
    unsigned int plane_count;
} Av1PlaneCopyPlan;

typedef struct {
    void *dst;
    const void *src;
    size_t element_size;
} Av1FlatCopyPlan;

typedef struct {
    AvifdecImage *image;
    const Av1FramePlanes *source;
    unsigned int plane;
    uint32_t output_plane_width;
    uint32_t output_plane_height;
    uint32_t source_plane_width;
    uint32_t source_plane_height;
    uint64_t divisor;
} Av1ImageScaleContext;

typedef struct {
    Av1ImageScaleContext planes[3];
    size_t row_offset[4];
    unsigned int plane_count;
} Av1ImageScalePlan;

AvifdecStatus av1_plane_copy_range(size_t begin,
                                   size_t end,
                                   size_t worker_index,
                                   void *arg);
AvifdecStatus av1_flat_copy_range(size_t begin,
                                  size_t end,
                                  size_t worker_index,
                                  void *arg);
AvifdecStatus av1_copy_image_scale_range(size_t begin,
                                         size_t end,
                                         size_t worker_index,
                                         void *arg);

#endif
