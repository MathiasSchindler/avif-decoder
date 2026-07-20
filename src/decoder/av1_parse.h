#ifndef AVIFDEC_AV1_PARSE_H
#define AVIFDEC_AV1_PARSE_H

#include "av1_bitstream.h"

typedef struct {
    uint8_t profile;
    uint8_t still_picture;
    uint8_t reduced_still_picture_header;
    uint8_t level;
    uint8_t tier;
    uint8_t frame_width_bits;
    uint8_t frame_height_bits;
    uint8_t frame_id_numbers_present;
    uint8_t delta_frame_id_length;
    uint8_t additional_frame_id_length;
    uint8_t use_128x128_superblock;
    uint8_t enable_filter_intra;
    uint8_t enable_intra_edge_filter;
    uint8_t enable_interintra_compound;
    uint8_t enable_masked_compound;
    uint8_t enable_warped_motion;
    uint8_t enable_dual_filter;
    uint8_t enable_order_hint;
    uint8_t enable_jnt_comp;
    uint8_t enable_ref_frame_mvs;
    uint8_t order_hint_bits;
    uint8_t seq_force_screen_content_tools;
    uint8_t seq_force_integer_mv;
    uint8_t enable_superres;
    uint8_t enable_cdef;
    uint8_t enable_restoration;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
    uint8_t separate_uv_delta_q;
    uint8_t color_range;
    uint8_t film_grain_params_present;
    uint8_t timing_info_present;
    uint8_t decoder_model_info_present;
    uint8_t equal_picture_interval;
    uint8_t buffer_delay_length;
    uint8_t buffer_removal_time_length;
    uint8_t frame_presentation_time_length;
    uint8_t selected_operating_point;
    uint8_t operating_points_count;
    uint16_t operating_point_idc[32];
    uint8_t operating_point_level[32];
    uint8_t operating_point_tier[32];
    uint8_t decoder_model_present[32];
    uint8_t low_delay_mode[32];
    uint8_t initial_display_delay_present[32];
    uint8_t initial_display_delay_minus_1[32];
    uint32_t decoder_buffer_delay[32];
    uint32_t encoder_buffer_delay[32];
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint32_t num_units_in_display_tick;
    uint32_t time_scale;
    uint32_t num_ticks_per_picture_minus_1;
    uint32_t num_units_in_decoding_tick;
    uint32_t max_width;
    uint32_t max_height;
} Av1Sequence;

AvifdecStatus av1_parse_sequence_header(
    Av1Bits *bits,
    uint8_t selected_operating_point,
    Av1Sequence *sequence);
int av1_layer_in_operating_point(const Av1Sequence *sequence,
                                 uint8_t temporal_id,
                                 uint8_t spatial_id);

#endif
