#include "av1_frame_header.h"

#include "av1_profile.h"
#include "base.h"

static unsigned int av1_tile_log2(uint32_t block_size, uint32_t target) {
    unsigned int result = 0U;

    while (((uint64_t)block_size << result) < target) ++result;
    return result;
}

static AvifdecStatus av1_parse_tile_info(Av1Bits *bits,
                                         const Av1Sequence *sequence,
                                         Av1Frame *frame) {
    uint32_t sb_cols = sequence->use_128x128_superblock
                       ? (frame->mi_cols + 31U) >> 5 : (frame->mi_cols + 15U) >> 4;
    uint32_t sb_rows = sequence->use_128x128_superblock
                       ? (frame->mi_rows + 31U) >> 5 : (frame->mi_rows + 15U) >> 4;
    uint32_t max_tile_width_sb = 4096U >> (sequence->use_128x128_superblock ? 7U : 6U);
    uint32_t max_tile_area_sb = 4096U * 2304U >> (sequence->use_128x128_superblock ? 14U : 12U);
    unsigned int min_log2_tile_cols = av1_tile_log2(max_tile_width_sb, sb_cols);
    unsigned int max_log2_tile_cols = av1_tile_log2(1U, sb_cols < 64U ? sb_cols : 64U);
    unsigned int max_log2_tile_rows = av1_tile_log2(1U, sb_rows < 64U ? sb_rows : 64U);
    unsigned int min_log2_tiles = av1_tile_log2(max_tile_area_sb, sb_rows * sb_cols);
    unsigned int tile_cols_log2;
    unsigned int tile_rows_log2;

    if (min_log2_tiles < min_log2_tile_cols) min_log2_tiles = min_log2_tile_cols;
    if (av1_bits_read(bits, 1U)) {
        uint32_t tile_width_sb;
        uint32_t tile_height_sb;
        uint32_t start;
        uint16_t index;

        tile_cols_log2 = min_log2_tile_cols;
        while (tile_cols_log2 < max_log2_tile_cols && av1_bits_read(bits, 1U)) ++tile_cols_log2;
        tile_width_sb = (sb_cols + (1U << tile_cols_log2) - 1U) >> tile_cols_log2;
        frame->tile_columns = (uint16_t)((sb_cols + tile_width_sb - 1U) / tile_width_sb);
        index = 0U;
        for (start = 0U; start < sb_cols; start += tile_width_sb) {
            frame->mi_col_starts[index++] = start << (sequence->use_128x128_superblock ? 5U : 4U);
        }
        frame->mi_col_starts[index] = frame->mi_cols;
        tile_rows_log2 = min_log2_tiles > tile_cols_log2 ? min_log2_tiles - tile_cols_log2 : 0U;
        while (tile_rows_log2 < max_log2_tile_rows && av1_bits_read(bits, 1U)) ++tile_rows_log2;
        tile_height_sb = (sb_rows + (1U << tile_rows_log2) - 1U) >> tile_rows_log2;
        frame->tile_rows = (uint16_t)((sb_rows + tile_height_sb - 1U) / tile_height_sb);
        index = 0U;
        for (start = 0U; start < sb_rows; start += tile_height_sb) {
            frame->mi_row_starts[index++] = start << (sequence->use_128x128_superblock ? 5U : 4U);
        }
        frame->mi_row_starts[index] = frame->mi_rows;
    } else {
        uint32_t start = 0U;
        uint32_t widest = 0U;
        uint32_t max_height;

        frame->tile_columns = 0U;
        while (start < sb_cols) {
            uint32_t maximum = sb_cols - start;
            uint32_t size;

            if (frame->tile_columns >= 64U) {
                return AVIFDEC_INVALID_DATA;
            }
            if (maximum > max_tile_width_sb) maximum = max_tile_width_sb;
            frame->mi_col_starts[frame->tile_columns] =
                start << (sequence->use_128x128_superblock ? 5U : 4U);
            size = av1_bits_ns(bits, maximum) + 1U;
            if (size > widest) widest = size;
            start += size;
            ++frame->tile_columns;
        }
        frame->mi_col_starts[frame->tile_columns] = frame->mi_cols;
        tile_cols_log2 = av1_tile_log2(1U, frame->tile_columns);
        if (min_log2_tiles > 0U) max_tile_area_sb = sb_rows * sb_cols >> (min_log2_tiles + 1U);
        else max_tile_area_sb = sb_rows * sb_cols;
        max_height = max_tile_area_sb / widest;
        if (max_height == 0U) max_height = 1U;
        start = 0U;
        frame->tile_rows = 0U;
        while (start < sb_rows) {
            uint32_t maximum = sb_rows - start;
            uint32_t size;

            if (frame->tile_rows >= 64U) {
                return AVIFDEC_INVALID_DATA;
            }
            if (maximum > max_height) maximum = max_height;
            frame->mi_row_starts[frame->tile_rows] =
                start << (sequence->use_128x128_superblock ? 5U : 4U);
            size = av1_bits_ns(bits, maximum) + 1U;
            start += size;
            ++frame->tile_rows;
        }
        frame->mi_row_starts[frame->tile_rows] = frame->mi_rows;
        tile_rows_log2 = av1_tile_log2(1U, frame->tile_rows);
    }
    frame->tile_columns_log2 = (uint8_t)tile_cols_log2;
    frame->tile_rows_log2 = (uint8_t)tile_rows_log2;
    if (tile_cols_log2 > 0U || tile_rows_log2 > 0U) {
        frame->context_update_tile_id =
            (uint16_t)av1_bits_read(bits, tile_cols_log2 + tile_rows_log2);
        frame->tile_size_bytes = (uint8_t)(av1_bits_read(bits, 2U) + 1U);
        if (frame->context_update_tile_id >= frame->tile_columns * frame->tile_rows) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    return bits->status;
}

static int32_t av1_read_delta_q(Av1Bits *bits) {
    return av1_bits_read(bits, 1U) ? av1_bits_signed(bits, 7U) : 0;
}

static AvifdecStatus av1_parse_quantization(Av1Bits *bits,
                                            const Av1Sequence *sequence,
                                            Av1Frame *frame,
                                            int32_t deltas[5]) {
    uint8_t different_uv = 0U;

    frame->base_q_index = (uint8_t)av1_bits_read(bits, 8U);
    deltas[0] = av1_read_delta_q(bits);
    if (!sequence->monochrome) {
        if (sequence->separate_uv_delta_q) different_uv = (uint8_t)av1_bits_read(bits, 1U);
        deltas[1] = av1_read_delta_q(bits);
        deltas[2] = av1_read_delta_q(bits);
        if (different_uv) {
            deltas[3] = av1_read_delta_q(bits);
            deltas[4] = av1_read_delta_q(bits);
        } else {
            deltas[3] = deltas[1];
            deltas[4] = deltas[2];
        }
    }
    frame->delta_q_y_dc = (int8_t)deltas[0];
    frame->delta_q_u_dc = (int8_t)deltas[1];
    frame->delta_q_u_ac = (int8_t)deltas[2];
    frame->delta_q_v_dc = (int8_t)deltas[3];
    frame->delta_q_v_ac = (int8_t)deltas[4];
    frame->using_qmatrix = (uint8_t)av1_bits_read(bits, 1U);
    frame->qm_y = 15U;
    frame->qm_u = 15U;
    frame->qm_v = 15U;
    if (frame->using_qmatrix) {
        frame->qm_y = (uint8_t)av1_bits_read(bits, 4U);
        frame->qm_u = (uint8_t)av1_bits_read(bits, 4U);
        frame->qm_v = sequence->separate_uv_delta_q
                      ? (uint8_t)av1_bits_read(bits, 4U) : frame->qm_u;
    }
    return bits->status;
}

static AvifdecStatus av1_parse_segmentation(Av1Bits *bits,
                                            Av1Frame *frame,
                                            int32_t alt_q[8]) {
    static const uint8_t feature_bits[8] = { 8U, 6U, 6U, 6U, 6U, 3U, 0U, 0U };
    static const uint8_t feature_signed[8] = { 1U, 1U, 1U, 1U, 1U, 0U, 0U, 0U };
    static const int16_t feature_max[8] = { 255, 63, 63, 63, 63, 7, 0, 0 };
    uint8_t update_data = 1U;
    uint8_t segment;

    frame->segmentation_enabled = (uint8_t)av1_bits_read(bits, 1U);
    if (!frame->segmentation_enabled) {
        avifdec_memory_fill(frame->feature_enabled, 0U,
                            sizeof(frame->feature_enabled));
        avifdec_memory_fill(frame->feature_data, 0U,
                            sizeof(frame->feature_data));
        return bits->status;
    }
    if (frame->primary_ref_frame != 7U) {
        frame->segmentation_update_map =
            (uint8_t)av1_bits_read(bits, 1U);
        if (frame->segmentation_update_map) {
            frame->segmentation_temporal_update =
                (uint8_t)av1_bits_read(bits, 1U);
        }
        update_data = (uint8_t)av1_bits_read(bits, 1U);
    } else {
        frame->segmentation_update_map = 1U;
    }
    if (update_data) {
        avifdec_memory_fill(frame->feature_enabled, 0U,
                            sizeof(frame->feature_enabled));
        avifdec_memory_fill(frame->feature_data, 0U,
                            sizeof(frame->feature_data));
        for (segment = 0U; segment < 8U; ++segment) {
            uint8_t feature;

            for (feature = 0U; feature < 8U; ++feature) {
                if (av1_bits_read(bits, 1U)) {
                    int32_t value = feature_signed[feature]
                                    ? av1_bits_signed(bits,
                                          feature_bits[feature] + 1U)
                                    : (int32_t)av1_bits_read(
                                          bits, feature_bits[feature]);

                    if (value < -feature_max[feature]) {
                        value = -feature_max[feature];
                    }
                    if (value > feature_max[feature]) {
                        value = feature_max[feature];
                    }
                    frame->feature_enabled[segment][feature] = 1U;
                    frame->feature_data[segment][feature] = (int16_t)value;
                }
            }
        }
    }
    frame->last_active_segment = 0U;
    frame->seg_id_pre_skip = 0U;
    for (segment = 0U; segment < 8U; ++segment) {
        uint8_t feature;

        if (frame->feature_enabled[segment][0]) {
            alt_q[segment] = frame->feature_data[segment][0];
        }
        for (feature = 0U; feature < 8U; ++feature) {
            if (!frame->feature_enabled[segment][feature]) continue;
            frame->last_active_segment = segment;
            if (feature >= 5U) frame->seg_id_pre_skip = 1U;
        }
    }
    return bits->status;
}

static AvifdecStatus av1_parse_loop_filter(Av1Bits *bits,
                                           const Av1Sequence *sequence,
                                           Av1Frame *frame) {
    unsigned int index;

    if (frame->coded_lossless || frame->allow_intrabc) return AVIFDEC_OK;
    frame->loop_filter_ref_deltas[0] = 1;
    frame->loop_filter_ref_deltas[1] = 0;
    frame->loop_filter_ref_deltas[2] = 0;
    frame->loop_filter_ref_deltas[3] = 0;
    frame->loop_filter_ref_deltas[4] = -1;
    frame->loop_filter_ref_deltas[5] = 0;
    frame->loop_filter_ref_deltas[6] = -1;
    frame->loop_filter_ref_deltas[7] = -1;
    frame->loop_filter_level[0] = (uint8_t)av1_bits_read(bits, 6U);
    frame->loop_filter_level[1] = (uint8_t)av1_bits_read(bits, 6U);
    if (!sequence->monochrome &&
        (frame->loop_filter_level[0] != 0U ||
         frame->loop_filter_level[1] != 0U)) {
        frame->loop_filter_level[2] = (uint8_t)av1_bits_read(bits, 6U);
        frame->loop_filter_level[3] = (uint8_t)av1_bits_read(bits, 6U);
    }
    frame->loop_filter_sharpness = (uint8_t)av1_bits_read(bits, 3U);
    frame->loop_filter_delta_enabled = (uint8_t)av1_bits_read(bits, 1U);
    if (frame->loop_filter_delta_enabled && av1_bits_read(bits, 1U)) {
        for (index = 0U; index < 8U; ++index) {
            if (av1_bits_read(bits, 1U)) {
                frame->loop_filter_ref_deltas[index] =
                    (int8_t)av1_bits_signed(bits, 7U);
            }
        }
        for (index = 0U; index < 2U; ++index) {
            if (av1_bits_read(bits, 1U)) {
                frame->loop_filter_mode_deltas[index] =
                    (int8_t)av1_bits_signed(bits, 7U);
            }
        }
    }
    return bits->status;
}

static AvifdecStatus av1_parse_cdef(Av1Bits *bits,
                                    const Av1Sequence *sequence,
                                    Av1Frame *frame) {
    uint32_t index;

    frame->cdef_damping = 3U;
    if (frame->coded_lossless || frame->allow_intrabc || !sequence->enable_cdef) {
        return AVIFDEC_OK;
    }
    frame->cdef_damping = (uint8_t)(av1_bits_read(bits, 2U) + 3U);
    frame->cdef_bits = (uint8_t)av1_bits_read(bits, 2U);
    for (index = 0U; index < (1U << frame->cdef_bits); ++index) {
        frame->cdef_y_pri_strength[index] =
            (uint8_t)av1_bits_read(bits, 4U);
        frame->cdef_y_sec_strength[index] =
            (uint8_t)av1_bits_read(bits, 2U);
        if (frame->cdef_y_sec_strength[index] == 3U) {
            ++frame->cdef_y_sec_strength[index];
        }
        if (!sequence->monochrome) {
            frame->cdef_uv_pri_strength[index] =
                (uint8_t)av1_bits_read(bits, 4U);
            frame->cdef_uv_sec_strength[index] =
                (uint8_t)av1_bits_read(bits, 2U);
            if (frame->cdef_uv_sec_strength[index] == 3U) {
                ++frame->cdef_uv_sec_strength[index];
            }
        }
    }
    return bits->status;
}

static AvifdecStatus av1_skip_restoration(Av1Bits *bits,
                                          const Av1Sequence *sequence,
                                          Av1Frame *frame,
                                          int all_lossless,
                                          int allow_intrabc) {
    static const uint8_t restoration_type[4] = { 0U, 3U, 1U, 2U };
    uint8_t uses_restoration = 0U;
    uint8_t chroma_restoration = 0U;
    unsigned int planes = sequence->monochrome ? 1U : 3U;
    unsigned int plane;
    uint32_t unit_shift = 0U;

    if (all_lossless || allow_intrabc || !sequence->enable_restoration) return AVIFDEC_OK;
    for (plane = 0U; plane < planes; ++plane) {
        uint8_t type = (uint8_t)av1_bits_read(bits, 2U);
        frame->restoration_type[plane] = restoration_type[type];
        if (type != 0U) uses_restoration = 1U;
        if (plane > 0U && type != 0U) chroma_restoration = 1U;
    }
    if (uses_restoration) {
        if (sequence->use_128x128_superblock) {
            unit_shift = av1_bits_read(bits, 1U) + 1U;
        } else {
            unit_shift = av1_bits_read(bits, 1U);
            if (unit_shift != 0U) unit_shift += av1_bits_read(bits, 1U);
        }
        frame->loop_restoration_size[0] =
            (uint16_t)(256U >> (2U - unit_shift));
        if (sequence->subsampling_x && sequence->subsampling_y && chroma_restoration) {
            unit_shift = av1_bits_read(bits, 1U);
        } else {
            unit_shift = 0U;
        }
        for (plane = 1U; plane < planes; ++plane) {
            frame->loop_restoration_size[plane] =
                (uint16_t)(frame->loop_restoration_size[0] >> unit_shift);
        }
    }
    return bits->status;
}

static void av1_parse_superres(Av1Bits *bits,
                               const Av1Sequence *sequence,
                               Av1Frame *frame) {
    frame->upscaled_width = frame->frame_width;
    frame->superres_denom = 8U;
    if (sequence->enable_superres && av1_bits_read(bits, 1U)) {
        uint32_t denominator = av1_bits_read(bits, 3U) + 9U;
        frame->superres_denom = (uint8_t)denominator;
        frame->frame_width = (frame->upscaled_width * 8U + denominator / 2U) / denominator;
    }
    frame->mi_cols = 2U * ((frame->frame_width + 7U) >> 3);
    frame->mi_rows = 2U * ((frame->frame_height + 7U) >> 3);
}

static void av1_parse_frame_size(Av1Bits *bits,
                                 const Av1Sequence *sequence,
                                 Av1Frame *frame,
                                 int frame_size_override) {
    if (frame_size_override) {
        frame->frame_width = av1_bits_read(bits, sequence->frame_width_bits) + 1U;
        frame->frame_height = av1_bits_read(bits, sequence->frame_height_bits) + 1U;
    } else {
        frame->frame_width = sequence->max_width;
        frame->frame_height = sequence->max_height;
    }
    av1_parse_superres(bits, sequence, frame);
}

static void av1_parse_render_size(Av1Bits *bits, Av1Frame *frame) {
    if (av1_bits_read(bits, 1U)) {
        frame->render_width = av1_bits_read(bits, 16U) + 1U;
        frame->render_height = av1_bits_read(bits, 16U) + 1U;
    } else {
        frame->render_width = frame->upscaled_width;
        frame->render_height = frame->frame_height;
    }
}

static AvifdecStatus av1_parse_frame_size_with_refs(
    Av1Bits *bits,
    const Av1Sequence *sequence,
    const Av1ReferenceState *references,
    Av1Frame *frame,
    int frame_size_override,
    int error_resilient_mode) {
    unsigned int index;

    if (frame_size_override && !error_resilient_mode) {
        for (index = 0U; index < AV1_REFS_PER_FRAME; ++index) {
            if (av1_bits_read(bits, 1U)) {
                const Av1ReferenceSlot *slot =
                    &references->slots[frame->ref_frame_idx[index]];

                if (!slot->valid) return AVIFDEC_INVALID_DATA;
                frame->frame_width = slot->upscaled_width;
                frame->frame_height = slot->frame_height;
                frame->render_width = slot->render_width;
                frame->render_height = slot->render_height;
                av1_parse_superres(bits, sequence, frame);
                return bits->status;
            }
        }
    }
    av1_parse_frame_size(bits, sequence, frame, frame_size_override);
    av1_parse_render_size(bits, frame);
    return bits->status;
}

static void av1_parse_skip_mode(Av1Bits *bits,
                                const Av1Sequence *sequence,
                                const Av1ReferenceState *references,
                                Av1Frame *frame) {
    int forward = -1;
    int backward = -1;
    int second_forward = -1;
    uint32_t forward_hint = 0U;
    uint32_t backward_hint = 0U;
    uint32_t second_forward_hint = 0U;
    unsigned int index;

    if (frame->frame_type == 0U || frame->frame_type == 2U ||
        !frame->reference_select || !sequence->enable_order_hint) {
        return;
    }
    for (index = 0U; index < AV1_REFS_PER_FRAME; ++index) {
        uint32_t hint = references->slots[frame->ref_frame_idx[index]].order_hint;
        int32_t distance = av1_relative_distance(
            1U, sequence->order_hint_bits, hint, frame->order_hint);

        if (distance < 0 &&
            (forward < 0 || av1_relative_distance(
                1U, sequence->order_hint_bits, hint, forward_hint) > 0)) {
            forward = (int)index;
            forward_hint = hint;
        } else if (distance > 0 &&
                   (backward < 0 || av1_relative_distance(
                       1U, sequence->order_hint_bits, hint,
                       backward_hint) < 0)) {
            backward = (int)index;
            backward_hint = hint;
        }
    }
    if (forward < 0) return;
    if (backward < 0) {
        for (index = 0U; index < AV1_REFS_PER_FRAME; ++index) {
            uint32_t hint =
                references->slots[frame->ref_frame_idx[index]].order_hint;

            if (av1_relative_distance(1U, sequence->order_hint_bits,
                                      hint, forward_hint) < 0 &&
                (second_forward < 0 || av1_relative_distance(
                    1U, sequence->order_hint_bits, hint,
                    second_forward_hint) > 0)) {
                second_forward = (int)index;
                second_forward_hint = hint;
            }
        }
        if (second_forward < 0) return;
        backward = second_forward;
    }
    frame->skip_mode_frame[0] =
        (uint8_t)(1 + (forward < backward ? forward : backward));
    frame->skip_mode_frame[1] =
        (uint8_t)(1 + (forward > backward ? forward : backward));
    frame->skip_mode_present = (uint8_t)av1_bits_read(bits, 1U);
}

static uint32_t av1_inverse_recenter(uint32_t reference, uint32_t value) {
    if (value > 2U * reference) return value;
    if (value & 1U) return reference - ((value + 1U) >> 1);
    return reference + (value >> 1);
}

static uint32_t av1_decode_subexp(Av1Bits *bits, uint32_t symbols) {
    unsigned int iteration = 0U;
    uint32_t consumed = 0U;

    for (;;) {
        unsigned int bit_count = iteration == 0U ? 3U : iteration + 2U;
        uint32_t group = 1U << bit_count;

        if (symbols <= consumed + 3U * group) {
            return consumed + av1_bits_ns(bits, symbols - consumed);
        }
        if (!av1_bits_read(bits, 1U)) {
            return consumed + av1_bits_read(bits, bit_count);
        }
        consumed += group;
        ++iteration;
    }
}

static int32_t av1_decode_signed_subexp_with_ref(Av1Bits *bits,
                                                  int32_t low,
                                                  int32_t high,
                                                  int32_t reference) {
    uint32_t symbols = (uint32_t)(high - low);
    uint32_t shifted_reference = (uint32_t)(reference - low);
    uint32_t value = av1_decode_subexp(bits, symbols);
    uint32_t decoded;

    if (shifted_reference * 2U <= symbols) {
        decoded = av1_inverse_recenter(shifted_reference, value);
    } else {
        decoded = symbols - 1U - av1_inverse_recenter(
            symbols - 1U - shifted_reference, value);
    }
    return (int32_t)decoded + low;
}

static void av1_read_global_param(Av1Bits *bits,
                                  Av1Frame *frame,
                                  int32_t previous[7][6],
                                  unsigned int type,
                                  unsigned int reference,
                                  unsigned int parameter) {
    unsigned int absolute_bits = 12U;
    unsigned int precision_bits = 15U;
    unsigned int precision_difference;
    int32_t identity = parameter % 3U == 2U ? 1 << 16 : 0;
    int32_t subtraction;
    int32_t decoded;
    int32_t maximum;

    if (parameter < 2U) {
        if (type == 1U) {
            absolute_bits = 9U - !frame->allow_high_precision_mv;
            precision_bits = 3U - !frame->allow_high_precision_mv;
        } else {
            precision_bits = 6U;
        }
    }
    precision_difference = 16U - precision_bits;
    subtraction = parameter % 3U == 2U ? 1 << precision_bits : 0;
    maximum = 1 << absolute_bits;
    decoded = av1_decode_signed_subexp_with_ref(
        bits, -maximum, maximum + 1,
        (previous[reference][parameter] >> precision_difference) -
            subtraction);
    frame->gm_params[reference][parameter] =
        decoded * (1 << precision_difference) + identity;
}

static void av1_parse_global_motion(Av1Bits *bits, Av1Frame *frame) {
    int32_t previous[7][6];
    unsigned int reference;
    unsigned int parameter;

    if (frame->primary_ref_frame == 7U) {
        avifdec_memory_fill(previous, 0U, sizeof(previous));
        for (reference = 0U; reference < AV1_REFS_PER_FRAME; ++reference) {
            previous[reference][2] = 1 << 16;
            previous[reference][5] = 1 << 16;
        }
    } else {
        avifdec_memory_copy(previous, frame->gm_params, sizeof(previous));
    }
    for (reference = 0U; reference < AV1_REFS_PER_FRAME; ++reference) {
        unsigned int type = 0U;

        for (parameter = 0U; parameter < 6U; ++parameter) {
            frame->gm_params[reference][parameter] =
                parameter % 3U == 2U ? 1 << 16 : 0;
        }
        if (frame->frame_type == 0U || frame->frame_type == 2U) continue;
        if (av1_bits_read(bits, 1U)) {
            if (av1_bits_read(bits, 1U)) type = 2U;
            else type = av1_bits_read(bits, 1U) ? 1U : 3U;
        }
        frame->gm_type[reference] = (uint8_t)type;
        if (type >= 2U) {
            av1_read_global_param(bits, frame, previous, type, reference, 2U);
            av1_read_global_param(bits, frame, previous, type, reference, 3U);
            if (type == 3U) {
                av1_read_global_param(bits, frame, previous, type, reference, 4U);
                av1_read_global_param(bits, frame, previous, type, reference, 5U);
            } else {
                frame->gm_params[reference][4] =
                    -frame->gm_params[reference][3];
                frame->gm_params[reference][5] =
                    frame->gm_params[reference][2];
            }
        }
        if (type >= 1U) {
            av1_read_global_param(bits, frame, previous, type, reference, 0U);
            av1_read_global_param(bits, frame, previous, type, reference, 1U);
        }
    }
}

AvifdecStatus av1_parse_frame_header(Av1Bits *bits,
                                            const Av1Sequence *sequence,
                                            uint8_t temporal_id,
                                            uint8_t spatial_id,
                                            Av1ReferenceState *references,
                                            Av1Frame *frame) {
    int32_t quant_deltas[5] = { 0, 0, 0, 0, 0 };
    int32_t alt_q[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t allow_screen_content_tools;
    uint8_t delta_q_present = 0U;
    unsigned int segment;

    avifdec_memory_fill(frame, 0U, sizeof(*frame));
    frame->primary_ref_frame = 7U;
    if (sequence->reduced_still_picture_header) {
        frame->frame_type = 0U;
        frame->show_frame = 1U;
        frame->error_resilient_mode = 1U;
        frame->refresh_frame_flags = 0xffU;
        frame->force_integer_mv = 1U;
        frame->disable_cdf_update = (uint8_t)av1_bits_read(bits, 1U);
        allow_screen_content_tools = (uint8_t)av1_bits_read(bits, 1U);
        if (allow_screen_content_tools) (void)av1_bits_read(bits, 1U);
        av1_parse_frame_size(bits, sequence, frame, 0);
        av1_parse_render_size(bits, frame);
    } else {
        uint8_t frame_is_intra;
        uint8_t frame_size_override;
        uint8_t id_length = sequence->additional_frame_id_length +
                            sequence->delta_frame_id_length;
        unsigned int index;

        frame->show_existing_frame = (uint8_t)av1_bits_read(bits, 1U);
        if (frame->show_existing_frame) {
            Av1ReferenceSlot shown;

            frame->frame_to_show_map_idx =
                (uint8_t)av1_bits_read(bits, 3U);
            if (sequence->decoder_model_info_present &&
                !sequence->equal_picture_interval) {
                frame->frame_presentation_time_present = 1U;
                frame->frame_presentation_time = av1_bits_read(
                    bits, sequence->frame_presentation_time_length);
            }
            if (sequence->frame_id_numbers_present) {
                uint32_t display_frame_id = av1_bits_read(bits, id_length);

                if (av1_reference_show_existing(
                        references->slots, frame->frame_to_show_map_idx, 1,
                        display_frame_id, &shown) != AVIFDEC_OK) {
                    return AVIFDEC_INVALID_DATA;
                }
            } else if (av1_reference_show_existing(
                           references->slots, frame->frame_to_show_map_idx, 0,
                           0U, &shown) != AVIFDEC_OK) {
                return AVIFDEC_INVALID_DATA;
            }
            frame->frame_type = shown.frame_type;
            frame->show_frame = 1U;
            frame->current_frame_id = shown.frame_id;
            frame->order_hint = shown.order_hint;
            frame->upscaled_width = shown.upscaled_width;
            frame->frame_width = shown.upscaled_width;
            frame->frame_height = shown.frame_height;
            frame->render_width = shown.render_width;
            frame->render_height = shown.render_height;
            frame->showable_frame = shown.showable_frame;
            frame->segmentation_enabled = shown.segmentation_enabled;
            avifdec_memory_copy(frame->feature_enabled,
                                shown.feature_enabled,
                                sizeof(frame->feature_enabled));
            avifdec_memory_copy(frame->feature_data, shown.feature_data,
                                sizeof(frame->feature_data));
            avifdec_memory_copy(frame->gm_params, shown.gm_params,
                                sizeof(frame->gm_params));
            if (sequence->film_grain_params_present) {
                frame->film_grain = shown.film_grain;
            }
            frame->refresh_frame_flags = frame->frame_type == 0U
                                         ? 0xffU : 0U;
            if (av1_level_validate_dimensions(
                    sequence->level, frame->upscaled_width,
                    frame->frame_height, 1) != AVIFDEC_OK) {
                return AVIFDEC_INVALID_DATA;
            }
            return bits->status;
        }
        frame->frame_type = (uint8_t)av1_bits_read(bits, 2U);
        frame_is_intra = frame->frame_type == 0U || frame->frame_type == 2U;
        frame->show_frame = (uint8_t)av1_bits_read(bits, 1U);
        if (frame->show_frame && sequence->decoder_model_info_present &&
            !sequence->equal_picture_interval) {
            frame->frame_presentation_time_present = 1U;
            frame->frame_presentation_time = av1_bits_read(
                bits, sequence->frame_presentation_time_length);
        }
        frame->showable_frame = frame->show_frame
                                ? frame->frame_type != 0U
                                : (uint8_t)av1_bits_read(bits, 1U);
        frame->error_resilient_mode =
            frame->frame_type == 3U ||
            (frame->frame_type == 0U && frame->show_frame)
            ? 1U : (uint8_t)av1_bits_read(bits, 1U);
        if (frame->frame_type == 0U && frame->show_frame) {
            for (index = 0U; index < AV1_NUM_REF_FRAMES; ++index) {
                references->slots[index].valid = 0U;
                references->slots[index].order_hint = 0U;
            }
        }
        frame->disable_cdf_update = (uint8_t)av1_bits_read(bits, 1U);
        allow_screen_content_tools = sequence->seq_force_screen_content_tools == 2U
                                     ? (uint8_t)av1_bits_read(bits, 1U)
                                     : sequence->seq_force_screen_content_tools;
        if (allow_screen_content_tools) {
            frame->force_integer_mv = sequence->seq_force_integer_mv == 2U
                ? (uint8_t)av1_bits_read(bits, 1U)
                : sequence->seq_force_integer_mv;
        }
        if (frame_is_intra) frame->force_integer_mv = 1U;
        if (sequence->frame_id_numbers_present) {
            frame->current_frame_id = av1_bits_read(bits, id_length);
            if (references->have_previous_frame_id &&
                frame->current_frame_id == references->previous_frame_id) {
                return AVIFDEC_INVALID_DATA;
            }
            if (av1_mark_reference_frames(
                    references->slots, id_length,
                    sequence->delta_frame_id_length,
                    frame->current_frame_id) != AVIFDEC_OK) {
                return AVIFDEC_INVALID_DATA;
            }
        }
        frame_size_override = frame->frame_type == 3U
                              ? 1U : (uint8_t)av1_bits_read(bits, 1U);
        frame->order_hint = av1_bits_read(bits, sequence->order_hint_bits);
        if (!frame_is_intra && !frame->error_resilient_mode) {
            frame->primary_ref_frame = (uint8_t)av1_bits_read(bits, 3U);
        }
        if (sequence->decoder_model_info_present) {
            uint8_t buffer_removal_time_present = (uint8_t)av1_bits_read(bits, 1U);

            if (buffer_removal_time_present) {
                for (index = 0U; index < sequence->operating_points_count; ++index) {
                    uint16_t idc = sequence->operating_point_idc[index];
                    int in_layer = idc == 0U || (((idc >> temporal_id) & 1U) != 0U &&
                                                 ((idc >> (spatial_id + 8U)) & 1U) != 0U);

                    if (sequence->decoder_model_present[index] && in_layer) {
                        uint32_t removal_time = av1_bits_read(
                            bits,
                            sequence->buffer_removal_time_length);

                        if (index ==
                            sequence->selected_operating_point) {
                            frame->buffer_removal_time_present = 1U;
                            frame->buffer_removal_time = removal_time;
                        }
                    }
                }
            }
        }
        frame->refresh_frame_flags =
            frame->frame_type == 3U ||
            (frame->frame_type == 0U && frame->show_frame)
            ? 0xffU : (uint8_t)av1_bits_read(bits, 8U);
        if ((!frame_is_intra || frame->refresh_frame_flags != 0xffU) &&
            frame->error_resilient_mode && sequence->enable_order_hint) {
            for (index = 0U; index < 8U; ++index) {
                uint32_t hint = av1_bits_read(bits, sequence->order_hint_bits);

                if (hint != references->slots[index].order_hint) {
                    references->slots[index].valid = 0U;
                }
            }
        }
        if (frame_is_intra) {
            av1_parse_frame_size(bits, sequence, frame, frame_size_override);
            av1_parse_render_size(bits, frame);
        } else {
            uint8_t short_signaling = sequence->enable_order_hint
                ? (uint8_t)av1_bits_read(bits, 1U) : 0U;

            if (short_signaling) {
                uint8_t last_frame_idx = (uint8_t)av1_bits_read(bits, 3U);
                uint8_t gold_frame_idx = (uint8_t)av1_bits_read(bits, 3U);
                AvifdecStatus status = av1_set_frame_refs(
                    references->slots, sequence->order_hint_bits,
                    frame->order_hint, last_frame_idx, gold_frame_idx,
                    frame->ref_frame_idx);

                if (status != AVIFDEC_OK) return status;
            }
            for (index = 0U; index < AV1_REFS_PER_FRAME; ++index) {
                uint8_t ref_index;

                if (!short_signaling) {
                    frame->ref_frame_idx[index] =
                        (uint8_t)av1_bits_read(bits, 3U);
                }
                ref_index = frame->ref_frame_idx[index];
                if (!references->slots[ref_index].valid) {
                    return AVIFDEC_INVALID_DATA;
                }
                if (sequence->frame_id_numbers_present) {
                    uint32_t delta = av1_bits_read(
                        bits, sequence->delta_frame_id_length) + 1U;
                    uint32_t mask = (1U << id_length) - 1U;
                    uint32_t expected =
                        (frame->current_frame_id - delta) & mask;

                    if (references->slots[ref_index].frame_id != expected) {
                        return AVIFDEC_INVALID_DATA;
                    }
                }
            }
            if (av1_parse_frame_size_with_refs(
                    bits, sequence, references, frame, frame_size_override,
                    frame->error_resilient_mode) != AVIFDEC_OK) {
                return bits->status != AVIFDEC_OK ? bits->status
                                                   : AVIFDEC_INVALID_DATA;
            }
            if (!frame->force_integer_mv) {
                frame->allow_high_precision_mv =
                    (uint8_t)av1_bits_read(bits, 1U);
            }
            frame->interpolation_filter = av1_bits_read(bits, 1U)
                                          ? 4U
                                          : (uint8_t)av1_bits_read(bits, 2U);
            frame->is_motion_mode_switchable =
                (uint8_t)av1_bits_read(bits, 1U);
            if (!frame->error_resilient_mode &&
                sequence->enable_ref_frame_mvs) {
                frame->use_ref_frame_mvs =
                    (uint8_t)av1_bits_read(bits, 1U);
            }
            for (index = 0U; index < AV1_REFS_PER_FRAME; ++index) {
                uint8_t ref_frame = (uint8_t)(index + 1U);
                uint32_t hint = references->slots[
                    frame->ref_frame_idx[index]].order_hint;

                frame->ref_frame_sign_bias[ref_frame] =
                    sequence->enable_order_hint &&
                    av1_relative_distance(1U, sequence->order_hint_bits,
                                          hint, frame->order_hint) > 0;
            }
        }
        if (frame->primary_ref_frame != 7U) {
            const Av1ReferenceSlot *primary = &references->slots[
                frame->ref_frame_idx[frame->primary_ref_frame]];

            if (!primary->valid) return AVIFDEC_INVALID_DATA;
            avifdec_memory_copy(frame->feature_enabled,
                                primary->feature_enabled,
                                sizeof(frame->feature_enabled));
            avifdec_memory_copy(frame->feature_data, primary->feature_data,
                                sizeof(frame->feature_data));
            avifdec_memory_copy(frame->gm_params, primary->gm_params,
                                sizeof(frame->gm_params));
        }
    }
    if (av1_level_validate_dimensions(
            sequence->level, frame->upscaled_width,
            frame->frame_height, 1) != AVIFDEC_OK) {
        return AVIFDEC_INVALID_DATA;
    }
    if ((frame->frame_type == 0U || frame->frame_type == 2U) &&
        allow_screen_content_tools &&
        frame->upscaled_width == frame->frame_width) {
        frame->allow_intrabc = (uint8_t)av1_bits_read(bits, 1U);
    }
    frame->allow_screen_content_tools = allow_screen_content_tools;
    if (sequence->reduced_still_picture_header || frame->disable_cdf_update) {
        frame->disable_frame_end_update_cdf = 1U;
    } else {
        frame->disable_frame_end_update_cdf = (uint8_t)av1_bits_read(bits, 1U);
    }
    if (av1_parse_tile_info(bits, sequence, frame) != AVIFDEC_OK ||
        av1_parse_quantization(bits, sequence, frame, quant_deltas) != AVIFDEC_OK ||
        av1_parse_segmentation(bits, frame, alt_q) != AVIFDEC_OK) {
        return bits->status;
    }
    if (frame->base_q_index > 0U) delta_q_present = (uint8_t)av1_bits_read(bits, 1U);
    frame->delta_q_present = delta_q_present;
    if (frame->delta_q_present) {
        frame->delta_q_res = (uint8_t)av1_bits_read(bits, 2U);
        if (!frame->allow_intrabc) {
            frame->delta_lf_present = (uint8_t)av1_bits_read(bits, 1U);
        }
        if (frame->delta_lf_present) {
            frame->delta_lf_res = (uint8_t)av1_bits_read(bits, 2U);
            frame->delta_lf_multi = (uint8_t)av1_bits_read(bits, 1U);
        }
    }
    frame->coded_lossless = 1U;
    for (segment = 0U; segment < 8U; ++segment) {
        int32_t qindex = (int32_t)frame->base_q_index + alt_q[segment];
        if (qindex < 0) qindex = 0;
        if (qindex > 255) qindex = 255;
        frame->lossless[segment] =
            qindex == 0 && quant_deltas[0] == 0 && quant_deltas[1] == 0 &&
            quant_deltas[2] == 0 && quant_deltas[3] == 0 && quant_deltas[4] == 0;
        if (!frame->lossless[segment]) {
            frame->coded_lossless = 0U;
        }
    }
    if (av1_parse_loop_filter(bits, sequence, frame) != AVIFDEC_OK ||
        av1_parse_cdef(bits, sequence, frame) != AVIFDEC_OK ||
        av1_skip_restoration(bits, sequence, frame,
                             frame->coded_lossless && frame->frame_width == frame->upscaled_width,
                             frame->allow_intrabc) != AVIFDEC_OK) {
        return bits->status;
    }
    if (frame->coded_lossless) frame->tx_mode = 0U;
    else frame->tx_mode = av1_bits_read(bits, 1U) ? 2U : 1U;
    if (frame->frame_type != 0U && frame->frame_type != 2U) {
        frame->reference_select = (uint8_t)av1_bits_read(bits, 1U);
    }
    av1_parse_skip_mode(bits, sequence, references, frame);
    if (frame->frame_type != 0U && frame->frame_type != 2U &&
        !frame->error_resilient_mode && sequence->enable_warped_motion) {
        frame->allow_warped_motion = (uint8_t)av1_bits_read(bits, 1U);
    }
    frame->reduced_tx_set = (uint8_t)av1_bits_read(bits, 1U);
    av1_parse_global_motion(bits, frame);
    {
        Av1FilmGrainParseConfig grain_config;
        const Av1FilmGrainParams *reference_params[AV1_NUM_REF_FRAMES];
        unsigned int slot;

        for (slot = 0U; slot < AV1_NUM_REF_FRAMES; ++slot) {
            reference_params[slot] =
                references->slots[slot].valid &&
                        references->slots[slot].film_grain_valid
                    ? &references->slots[slot].film_grain
                    : 0;
        }
        avifdec_memory_fill(&grain_config, 0U, sizeof(grain_config));
        grain_config.film_grain_params_present =
            sequence->film_grain_params_present;
        grain_config.show_frame = frame->show_frame;
        grain_config.showable_frame = frame->showable_frame;
        grain_config.frame_type = frame->frame_type;
        grain_config.mono_chrome = sequence->monochrome;
        grain_config.subsampling_x = sequence->subsampling_x;
        grain_config.subsampling_y = sequence->subsampling_y;
        grain_config.ref_frame_idx = frame->ref_frame_idx;
        grain_config.reference_params = reference_params;
        {
            AvifdecStatus grain_status = av1_film_grain_parse(
                bits, &grain_config, &frame->film_grain);

            if (grain_status != AVIFDEC_OK) return grain_status;
        }
    }
    return bits->status;
}
