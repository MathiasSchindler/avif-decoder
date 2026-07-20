#include "av1_parse.h"
#include "av1_profile.h"
#include "base.h"

static AvifdecStatus av1_parse_color_config(Av1Bits *bits,
                                             Av1Sequence *sequence) {
    uint8_t high_bitdepth = (uint8_t)av1_bits_read(bits, 1U);
    uint8_t color_description_present;

    if (sequence->profile == 2U && high_bitdepth) {
        sequence->bit_depth = av1_bits_read(bits, 1U) ? 12U : 10U;
    } else if (sequence->profile <= 2U) {
        sequence->bit_depth = high_bitdepth ? 10U : 8U;
    } else {
        return AVIFDEC_INVALID_DATA;
    }
    sequence->monochrome =
        sequence->profile == 1U ? 0U : (uint8_t)av1_bits_read(bits, 1U);
    color_description_present = (uint8_t)av1_bits_read(bits, 1U);
    if (color_description_present) {
        sequence->color_primaries = (uint16_t)av1_bits_read(bits, 8U);
        sequence->transfer_characteristics =
            (uint16_t)av1_bits_read(bits, 8U);
        sequence->matrix_coefficients = (uint16_t)av1_bits_read(bits, 8U);
    } else {
        sequence->color_primaries = 2U;
        sequence->transfer_characteristics = 2U;
        sequence->matrix_coefficients = 2U;
    }
    if (sequence->monochrome) {
        sequence->color_range = (uint8_t)av1_bits_read(bits, 1U);
        sequence->subsampling_x = 1U;
        sequence->subsampling_y = 1U;
        sequence->chroma_sample_position = 0U;
        sequence->separate_uv_delta_q = 0U;
        return bits->status;
    }
    if (sequence->color_primaries == 1U &&
        sequence->transfer_characteristics == 13U &&
        sequence->matrix_coefficients == 0U) {
        sequence->color_range = 1U;
        sequence->subsampling_x = 0U;
        sequence->subsampling_y = 0U;
    } else {
        sequence->color_range = (uint8_t)av1_bits_read(bits, 1U);
        if (sequence->profile == 0U) {
            sequence->subsampling_x = 1U;
            sequence->subsampling_y = 1U;
        } else if (sequence->profile == 1U) {
            sequence->subsampling_x = 0U;
            sequence->subsampling_y = 0U;
        } else if (sequence->bit_depth == 12U) {
            sequence->subsampling_x = (uint8_t)av1_bits_read(bits, 1U);
            sequence->subsampling_y =
                sequence->subsampling_x
                    ? (uint8_t)av1_bits_read(bits, 1U)
                    : 0U;
        } else {
            sequence->subsampling_x = 1U;
            sequence->subsampling_y = 0U;
        }
        sequence->chroma_sample_position =
            sequence->subsampling_x && sequence->subsampling_y
                ? (uint8_t)av1_bits_read(bits, 2U)
                : 0U;
    }
    sequence->separate_uv_delta_q = (uint8_t)av1_bits_read(bits, 1U);
    return bits->status;
}

AvifdecStatus av1_parse_sequence_header(
    Av1Bits *bits,
    uint8_t selected_operating_point,
    Av1Sequence *sequence) {
    uint8_t decoder_model_info_present = 0U;
    uint8_t initial_display_delay_present = 0U;
    uint8_t buffer_delay_length = 0U;
    uint8_t operating_points_minus_one;
    uint8_t index;

    avifdec_memory_fill(sequence, 0U, sizeof(*sequence));
    sequence->selected_operating_point = selected_operating_point;
    sequence->profile = (uint8_t)av1_bits_read(bits, 3U);
    sequence->still_picture = (uint8_t)av1_bits_read(bits, 1U);
    sequence->reduced_still_picture_header =
        (uint8_t)av1_bits_read(bits, 1U);
    if (sequence->profile > 2U ||
        (sequence->reduced_still_picture_header &&
         !sequence->still_picture)) {
        return AVIFDEC_INVALID_DATA;
    }
    if (sequence->reduced_still_picture_header) {
        if (selected_operating_point != 0U) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        sequence->level = (uint8_t)av1_bits_read(bits, 5U);
        sequence->operating_point_level[0] = sequence->level;
        operating_points_minus_one = 0U;
    } else {
        sequence->timing_info_present = (uint8_t)av1_bits_read(bits, 1U);
        if (sequence->timing_info_present) {
            sequence->num_units_in_display_tick = av1_bits_read(bits, 32U);
            sequence->time_scale = av1_bits_read(bits, 32U);
            if (sequence->num_units_in_display_tick == 0U ||
                sequence->time_scale == 0U) {
                return AVIFDEC_INVALID_DATA;
            }
            sequence->equal_picture_interval =
                (uint8_t)av1_bits_read(bits, 1U);
            if (sequence->equal_picture_interval) {
                sequence->num_ticks_per_picture_minus_1 =
                    av1_bits_uvlc(bits);
            }
            decoder_model_info_present =
                (uint8_t)av1_bits_read(bits, 1U);
            if (decoder_model_info_present) {
                sequence->decoder_model_info_present = 1U;
                buffer_delay_length =
                    (uint8_t)(av1_bits_read(bits, 5U) + 1U);
                sequence->buffer_delay_length = buffer_delay_length;
                sequence->num_units_in_decoding_tick =
                    av1_bits_read(bits, 32U);
                if (sequence->num_units_in_decoding_tick == 0U) {
                    return AVIFDEC_INVALID_DATA;
                }
                sequence->buffer_removal_time_length =
                    (uint8_t)(av1_bits_read(bits, 5U) + 1U);
                sequence->frame_presentation_time_length =
                    (uint8_t)(av1_bits_read(bits, 5U) + 1U);
            }
        }
        initial_display_delay_present =
            (uint8_t)av1_bits_read(bits, 1U);
        operating_points_minus_one = (uint8_t)av1_bits_read(bits, 5U);
        sequence->operating_points_count =
            (uint8_t)(operating_points_minus_one + 1U);
        for (index = 0U; index <= operating_points_minus_one; ++index) {
            uint16_t operating_point_idc =
                (uint16_t)av1_bits_read(bits, 12U);
            uint8_t level = (uint8_t)av1_bits_read(bits, 5U);
            uint8_t tier =
                level > 7U ? (uint8_t)av1_bits_read(bits, 1U) : 0U;
            uint8_t model_for_op = 0U;

            (void)operating_point_idc;
            sequence->operating_point_idc[index] = operating_point_idc;
            sequence->operating_point_level[index] = level;
            sequence->operating_point_tier[index] = tier;
            if (decoder_model_info_present) {
                model_for_op = (uint8_t)av1_bits_read(bits, 1U);
                sequence->decoder_model_present[index] = model_for_op;
                if (model_for_op) {
                    sequence->decoder_buffer_delay[index] =
                        av1_bits_read(bits, buffer_delay_length);
                    sequence->encoder_buffer_delay[index] =
                        av1_bits_read(bits, buffer_delay_length);
                    sequence->low_delay_mode[index] =
                        (uint8_t)av1_bits_read(bits, 1U);
                }
            }
            if (initial_display_delay_present &&
                av1_bits_read(bits, 1U)) {
                sequence->initial_display_delay_present[index] = 1U;
                sequence->initial_display_delay_minus_1[index] =
                    (uint8_t)av1_bits_read(bits, 4U);
            }
        }
        if (selected_operating_point > operating_points_minus_one) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        sequence->level =
            sequence->operating_point_level[selected_operating_point];
        sequence->tier =
            sequence->operating_point_tier[selected_operating_point];
    }
    sequence->frame_width_bits =
        (uint8_t)(av1_bits_read(bits, 4U) + 1U);
    sequence->frame_height_bits =
        (uint8_t)(av1_bits_read(bits, 4U) + 1U);
    sequence->max_width =
        av1_bits_read(bits, sequence->frame_width_bits) + 1U;
    sequence->max_height =
        av1_bits_read(bits, sequence->frame_height_bits) + 1U;
    if (!sequence->reduced_still_picture_header) {
        sequence->frame_id_numbers_present =
            (uint8_t)av1_bits_read(bits, 1U);
        if (sequence->frame_id_numbers_present) {
            sequence->delta_frame_id_length =
                (uint8_t)(av1_bits_read(bits, 4U) + 2U);
            sequence->additional_frame_id_length =
                (uint8_t)(av1_bits_read(bits, 3U) + 1U);
        }
    }
    sequence->use_128x128_superblock =
        (uint8_t)av1_bits_read(bits, 1U);
    sequence->enable_filter_intra = (uint8_t)av1_bits_read(bits, 1U);
    sequence->enable_intra_edge_filter =
        (uint8_t)av1_bits_read(bits, 1U);
    if (!sequence->reduced_still_picture_header) {
        uint8_t screen_content_tools;

        sequence->enable_interintra_compound =
            (uint8_t)av1_bits_read(bits, 1U);
        sequence->enable_masked_compound =
            (uint8_t)av1_bits_read(bits, 1U);
        sequence->enable_warped_motion =
            (uint8_t)av1_bits_read(bits, 1U);
        sequence->enable_dual_filter =
            (uint8_t)av1_bits_read(bits, 1U);
        sequence->enable_order_hint =
            (uint8_t)av1_bits_read(bits, 1U);
        if (sequence->enable_order_hint) {
            sequence->enable_jnt_comp =
                (uint8_t)av1_bits_read(bits, 1U);
            sequence->enable_ref_frame_mvs =
                (uint8_t)av1_bits_read(bits, 1U);
        }
        screen_content_tools =
            av1_bits_read(bits, 1U)
                ? 2U
                : (uint8_t)av1_bits_read(bits, 1U);
        sequence->seq_force_screen_content_tools = screen_content_tools;
        if (screen_content_tools > 0U) {
            sequence->seq_force_integer_mv =
                av1_bits_read(bits, 1U)
                    ? 2U
                    : (uint8_t)av1_bits_read(bits, 1U);
        } else {
            sequence->seq_force_integer_mv = 2U;
        }
        if (sequence->enable_order_hint) {
            sequence->order_hint_bits =
                (uint8_t)(av1_bits_read(bits, 3U) + 1U);
        }
    } else {
        sequence->seq_force_screen_content_tools = 2U;
        sequence->seq_force_integer_mv = 2U;
        sequence->operating_points_count = 1U;
    }
    sequence->enable_superres = (uint8_t)av1_bits_read(bits, 1U);
    sequence->enable_cdef = (uint8_t)av1_bits_read(bits, 1U);
    sequence->enable_restoration = (uint8_t)av1_bits_read(bits, 1U);
    if (av1_parse_color_config(bits, sequence) != AVIFDEC_OK) {
        return bits->status;
    }
    if (av1_profile_validate(
            sequence->profile, sequence->bit_depth,
            sequence->monochrome, sequence->subsampling_x,
            sequence->subsampling_y) != AVIFDEC_OK ||
        av1_level_validate_dimensions(
            sequence->level, sequence->max_width,
            sequence->max_height, 0) != AVIFDEC_OK) {
        return AVIFDEC_INVALID_DATA;
    }
    sequence->film_grain_params_present =
        (uint8_t)av1_bits_read(bits, 1U);
    if (bits->status != AVIFDEC_OK) return bits->status;
    return av1_trailing_bits(bits);
}

int av1_layer_in_operating_point(const Av1Sequence *sequence,
                                 uint8_t temporal_id,
                                 uint8_t spatial_id) {
    uint16_t idc = sequence->operating_point_idc[
        sequence->selected_operating_point];

    return idc == 0U || (((idc >> temporal_id) & 1U) != 0U &&
                         ((idc >> (spatial_id + 8U)) & 1U) != 0U);
}
