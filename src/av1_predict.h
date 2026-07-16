#ifndef AVIFDEC_AV1_PREDICT_H
#define AVIFDEC_AV1_PREDICT_H

#include "avifdec.h"

typedef enum {
    AV1_PREDICT_VERTICAL = 0,
    AV1_PREDICT_HORIZONTAL,
    AV1_PREDICT_PAETH,
    AV1_PREDICT_SMOOTH,
    AV1_PREDICT_SMOOTH_VERTICAL,
    AV1_PREDICT_SMOOTH_HORIZONTAL
} Av1PredictMode;

typedef struct {
    const uint16_t *above;
    const uint16_t *left;
    uint16_t top_left;
    uint8_t have_above;
    uint8_t have_left;
} Av1IntraReferences;

typedef struct {
    Av1IntraReferences references;
    uint16_t above_storage[258];
    uint16_t left_storage[258];
} Av1PreparedReferences;

AvifdecStatus av1_predict_prepare_references(const uint16_t *plane,
                                              size_t plane_stride,
                                              uint32_t plane_width,
                                              uint32_t plane_height,
                                              uint32_t reference_width,
                                              uint32_t reference_height,
                                              uint32_t x,
                                              uint32_t y,
                                              uint32_t width,
                                              uint32_t height,
                                              uint8_t bit_depth,
                                              uint8_t have_above,
                                              uint8_t have_left,
                                              uint8_t have_above_right,
                                              uint8_t have_below_left,
                                              Av1PreparedReferences *prepared);

AvifdecStatus av1_predict_dc(uint16_t *destination,
                             size_t stride,
                             uint32_t width,
                             uint32_t height,
                             uint8_t bit_depth,
                             const Av1IntraReferences *references);
AvifdecStatus av1_predict_nondirectional(uint16_t *destination,
                                         size_t stride,
                                         uint32_t width,
                                         uint32_t height,
                                         uint8_t bit_depth,
                                         Av1PredictMode mode,
                                         const Av1IntraReferences *references);
AvifdecStatus av1_predict_directional(uint16_t *destination,
                                      size_t stride,
                                      uint32_t width,
                                      uint32_t height,
                                      uint8_t bit_depth,
                                      uint16_t angle,
                                      const Av1IntraReferences *references);
AvifdecStatus av1_predict_directional_edges(uint16_t *destination,
                                            size_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            uint8_t bit_depth,
                                            uint16_t angle,
                                            uint8_t filter_type,
                                            Av1PreparedReferences *prepared);
unsigned int av1_predict_edge_filter_strength(uint32_t width,
                                               uint32_t height,
                                               uint8_t filter_type,
                                               int angle_delta);
int av1_predict_edge_upsample_selected(uint32_t width,
                                       uint32_t height,
                                       uint8_t filter_type,
                                       int angle_delta);
AvifdecStatus av1_predict_filter_intra(uint16_t *destination,
                                       size_t stride,
                                       uint32_t width,
                                       uint32_t height,
                                       uint8_t bit_depth,
                                       uint8_t filter_mode,
                                       const Av1IntraReferences *references);
AvifdecStatus av1_predict_palette(uint16_t *destination,
                                  size_t stride,
                                  uint32_t width,
                                  uint32_t height,
                                  uint8_t bit_depth,
                                  const uint16_t *palette,
                                  uint8_t palette_size,
                                  const uint8_t *color_map,
                                  size_t map_stride);
AvifdecStatus av1_predict_cfl(uint16_t *destination,
                              size_t destination_stride,
                              const uint16_t *luma,
                              size_t luma_stride,
                              uint32_t luma_width,
                              uint32_t luma_height,
                              uint32_t luma_x,
                              uint32_t luma_y,
                              uint32_t width,
                              uint32_t height,
                              uint8_t subsampling_x,
                              uint8_t subsampling_y,
                              int8_t alpha,
                              uint8_t bit_depth);
AvifdecStatus av1_predict_checksum(const uint16_t *samples,
                                   size_t stride,
                                   uint32_t width,
                                   uint32_t height,
                                   uint8_t plane,
                                   uint64_t *checksum);

#endif