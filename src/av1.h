#ifndef AVIFDEC_AV1_H
#define AVIFDEC_AV1_H

#include "avifdec.h"
#include "av1_film_grain.h"

#define AV1_NUM_REF_FRAMES 8U
#define AV1_REFS_PER_FRAME 7U

typedef struct {
    uint32_t frame_id;
    uint32_t order_hint;
    uint32_t upscaled_width;
    uint32_t frame_height;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t mi_rows;
    uint32_t mi_cols;
    uint32_t ref_order_hint[AV1_REFS_PER_FRAME];
    uint8_t frame_type;
    uint8_t valid;
    uint8_t showable_frame;
    uint8_t pixels_valid;
    uint8_t segmentation_enabled;
    uint8_t feature_enabled[8][8];
    int16_t feature_data[8][8];
    int32_t gm_params[7][6];
    uint8_t film_grain_valid;
    Av1FilmGrainParams film_grain;
} Av1ReferenceSlot;

int32_t av1_relative_distance(uint8_t enable_order_hint,
                              uint8_t order_hint_bits,
                              uint32_t first,
                              uint32_t second);
AvifdecStatus av1_mark_reference_frames(Av1ReferenceSlot slots[8],
                                        uint8_t id_length,
                                        uint8_t delta_frame_id_length,
                                        uint32_t current_frame_id);
AvifdecStatus av1_set_frame_refs(const Av1ReferenceSlot slots[8],
                                 uint8_t order_hint_bits,
                                 uint32_t order_hint,
                                 uint8_t last_frame_idx,
                                 uint8_t gold_frame_idx,
                                 uint8_t ref_frame_idx[7]);
AvifdecStatus av1_reference_show_existing(
    const Av1ReferenceSlot slots[8],
    uint8_t map_index,
    int frame_id_present,
    uint32_t display_frame_id,
    Av1ReferenceSlot *shown);
AvifdecStatus avifdec_av1_workspace_requirement(
    const AvifdecImageInfo *info,
    size_t *required);

AvifdecStatus avifdec_av1_query(const AvifdecSpan *spans,
                                size_t span_count,
                                const AvifdecLimits *limits,
                                AvifdecImageInfo *info,
                                AvifdecError *error);
AvifdecStatus avifdec_av1_trace(const AvifdecSpan *spans,
                                size_t span_count,
                                const AvifdecLimits *limits,
                                AvifdecImageInfo *info,
                                void *workspace,
                                size_t workspace_size,
                                AvifdecEntropyTrace *trace,
                                AvifdecError *error);
AvifdecStatus avifdec_av1_decode(const AvifdecSpan *spans,
                                 size_t span_count,
                                 const AvifdecLimits *limits,
                                 AvifdecImageInfo *info,
                                 void *workspace,
                                 size_t workspace_size,
                                 AvifdecImage *image,
                                 AvifdecEntropyTrace *trace,
                                 AvifdecError *error);

#endif