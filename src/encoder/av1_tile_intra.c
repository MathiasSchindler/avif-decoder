#include "encoder/av1_tile_intra.h"
#include "encoder/av1_tile_palette.h"
#include "av1_predict.h"

static uint8_t tile_neg_deinterleave(uint8_t difference,
                                     uint8_t reference,
                                     uint8_t maximum) {
    if (reference == 0U) return difference;
    if (reference >= maximum - 1U) return (uint8_t)(maximum - difference - 1U);
    if (2U * reference < maximum) {
        if (difference <= 2U * reference) {
            return (difference & 1U) != 0U
                ? (uint8_t)(reference + ((difference + 1U) >> 1U))
                : (uint8_t)(reference - (difference >> 1U));
        }
        return difference;
    }
    if (difference <= 2U * (maximum - reference - 1U)) {
        return (difference & 1U) != 0U
            ? (uint8_t)(reference + ((difference + 1U) >> 1U))
            : (uint8_t)(reference - (difference >> 1U));
    }
    return (uint8_t)(maximum - difference - 1U);
}

static uint8_t tile_segment_difference(uint8_t segment,
                                       uint8_t prediction) {
    uint8_t difference;

    for (difference = 0U; difference < 3U; ++difference) {
        if (tile_neg_deinterleave(difference, prediction, 3U) == segment) {
            return difference;
        }
    }
    return 0U;
}

static uint8_t tile_select_segment(const AvifencAv1TileState *state,
                                   uint32_t row,
                                   uint32_t column,
                                   uint32_t width_mi,
                                   uint32_t height_mi) {
    uint32_t maximum_activity = 0U;
    unsigned int plane;

    for (plane = 0U; plane < 3U; ++plane) {
        const uint8_t *source = state->source->planes[plane];
        size_t stride = state->source->strides[plane];
        uint32_t shift = plane == 0U ? 0U : 1U;
        uint32_t start_x = (column << 2U) >> shift;
        uint32_t start_y = (row << 2U) >> shift;
        uint32_t width = (width_mi << 2U) >> shift;
        uint32_t height = (height_mi << 2U) >> shift;
        uint32_t plane_width = state->source->width >> shift;
        uint32_t plane_height = state->source->height >> shift;
        uint32_t activity = 0U;
        uint32_t edges = 0U;
        uint32_t y;
        uint32_t x;

        for (y = 0U; y < height && start_y + y < plane_height; ++y) {
            for (x = 0U; x < width && start_x + x < plane_width; ++x) {
                uint8_t sample = source[
                    (size_t)(start_y + y) * stride + start_x + x];

                if (x != 0U) {
                    uint8_t previous = source[
                        (size_t)(start_y + y) * stride + start_x + x - 1U];
                    activity += sample > previous ? sample - previous
                                                  : previous - sample;
                    ++edges;
                }
                if (y != 0U) {
                    uint8_t previous = source[
                        (size_t)(start_y + y - 1U) * stride + start_x + x];
                    activity += sample > previous ? sample - previous
                                                  : previous - sample;
                    ++edges;
                }
            }
        }
        if (edges != 0U && activity / edges > maximum_activity) {
            maximum_activity = activity / edges;
        }
    }
    if (maximum_activity < 4U) return 1U;
    if (maximum_activity > 24U) return 2U;
    return 0U;
}

static AvifencStatus tile_write_segment_id(AvifencAv1TileState *state,
                                           uint32_t row,
                                           uint32_t column,
                                           uint32_t width_mi,
                                           uint32_t height_mi,
                                           uint8_t segment) {
    int upper_left = -1;
    int upper = -1;
    int left = -1;
    uint8_t prediction;
    unsigned int context;
    AvifencStatus status;

    if (row != 0U && column != 0U) {
        upper_left = state->segment_ids[
            (size_t)(row - 1U) * state->mi_columns + column - 1U];
    }
    if (row != 0U) {
        upper = state->segment_ids[
            (size_t)(row - 1U) * state->mi_columns + column];
    }
    if (column != 0U) {
        left = state->segment_ids[(size_t)row * state->mi_columns + column - 1U];
    }
    if (upper < 0) prediction = left < 0 ? 0U : (uint8_t)left;
    else if (left < 0) prediction = (uint8_t)upper;
    else prediction = upper_left == upper ? (uint8_t)upper : (uint8_t)left;
    if (upper_left < 0) context = 0U;
    else if (upper_left == upper && upper_left == left) context = 2U;
    else if (upper_left == upper || upper_left == left || upper == left) {
        context = 1U;
    } else {
        context = 0U;
    }
    status = avifenc_av1_symbol_writer_write(
        state->writer, state->segment_id[context], 8U,
        tile_segment_difference(segment, prediction));
    if (status == AVIFENC_OK) {
        uint32_t y;
        uint32_t x;

        for (y = 0U; y < height_mi; ++y) {
            for (x = 0U; x < width_mi; ++x) {
                state->segment_ids[
                    (size_t)(row + y) * state->mi_columns + column + x] =
                    segment;
            }
        }
    }
    return status;
}

static void tile_set_effective_quantizer(AvifencAv1TileState *state,
                                         uint8_t segment) {
    int quantizer = state->source->quantizer;
    unsigned int plane;

    if (segment == 1U) quantizer += state->source->quantization.aq_strength;
    if (segment == 2U) quantizer -= state->source->quantization.aq_strength;
    if (quantizer < 1) quantizer = 1;
    if (quantizer > 255) quantizer = 255;
    state->quantizer = (uint16_t)quantizer;
    for (plane = 0U; plane < 3U; ++plane) {
        state->transform.dequant[plane].q_index = (uint8_t)quantizer;
    }
}

static AvifencStatus tile_predict_plane_mode(AvifencAv1TileState *state,
                                             unsigned int plane,
                                             uint32_t x,
                                             uint32_t y,
                                             uint32_t width,
                                             uint32_t height,
                                             uint8_t have_above,
                                             uint8_t have_left,
                                             uint8_t mode,
                                             int8_t angle_delta,
                                             int8_t filter_intra_mode) {
    static const uint16_t directional_angle[9] = {
        0U, 90U, 180U, 45U, 135U, 113U, 157U, 203U, 67U
    };
    Av1PreparedReferences prepared;
    uint32_t sub_x = plane == 0U ? 0U : 1U;
    uint32_t sub_y = plane == 0U ? 0U : 1U;
    uint32_t column = (x >> 2U) << sub_x;
    uint32_t row = (y >> 2U) << sub_y;
    uint32_t width_mi = (width >> 2U) << sub_x;
    uint32_t height_mi = (height >> 2U) << sub_y;
    uint8_t have_above_right = 0U;
    uint8_t have_below_left = 0U;
    AvifdecStatus status;

    if (have_above && column + width_mi < state->mi_columns) {
        have_above_right = state->block_widths[
            (size_t)(row - 1U) * state->mi_columns + column + width_mi] != 0U;
    }
    if (have_left && row + height_mi < state->mi_rows) {
        have_below_left = state->block_widths[
            (size_t)(row + height_mi) * state->mi_columns + column - 1U] != 0U;
    }
    status = av1_predict_prepare_references(
        state->reconstruction->planes[plane],
        state->reconstruction->strides[plane],
        state->reconstruction->widths[plane],
        state->reconstruction->heights[plane],
        state->reconstruction->widths[plane],
        state->reconstruction->heights[plane],
        x, y, width, height, 8U,
        have_above, have_left, have_above_right, have_below_left, &prepared);
    if (status != AVIFDEC_OK) return AVIFENC_LIMIT_EXCEEDED;
    if (filter_intra_mode >= 0) {
        status = av1_predict_filter_intra(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            (uint8_t)filter_intra_mode, &prepared.references);
    } else if (mode == 0U) {
        status = av1_predict_dc(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            &prepared.references);
    } else if (mode >= 1U && mode <= 8U) {
        status = av1_predict_directional(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            (uint16_t)(directional_angle[mode] + 3 * angle_delta),
            &prepared.references);
    } else if (mode >= 9U && mode <= 12U) {
        static const Av1PredictMode nondirectional_mode[4] = {
            AV1_PREDICT_SMOOTH,
            AV1_PREDICT_SMOOTH_VERTICAL,
            AV1_PREDICT_SMOOTH_HORIZONTAL,
            AV1_PREDICT_PAETH
        };

        status = av1_predict_nondirectional(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            nondirectional_mode[mode - 9U],
            &prepared.references);
    } else {
        return AVIFENC_INVALID_ARGUMENT;
    }
    return status == AVIFDEC_OK ? AVIFENC_OK : AVIFENC_LIMIT_EXCEEDED;
}

static AvifencStatus tile_predict_chroma(
    AvifencAv1TileState *state,
    unsigned int plane,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint8_t have_above,
    uint8_t have_left,
    const AvifencAv1ChromaDecision *decision) {
    AvifencStatus status;
    AvifdecStatus decoder_status;

    if (decision->palette_size != 0U) {
        const uint16_t *colors = plane == 1U
            ? decision->palette_colors_u : decision->palette_colors_v;

        decoder_status = av1_predict_palette(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            colors, decision->palette_size, state->palette_map_uv, width);
        return decoder_status == AVIFDEC_OK ? AVIFENC_OK
                                            : AVIFENC_LIMIT_EXCEEDED;
    }
    if (decision->mode != 13U) {
        return tile_predict_plane_mode(
            state, plane, x, y, width, height, have_above, have_left,
            decision->mode, decision->angle_delta, -1);
    }
    status = tile_predict_plane_mode(
        state, plane, x, y, width, height, have_above, have_left,
        0U, 0, -1);
    if (status != AVIFENC_OK) return status;
    decoder_status = av1_predict_cfl(
        state->reconstruction->planes[plane] +
            (size_t)y * state->reconstruction->strides[plane] + x,
        state->reconstruction->strides[plane],
        state->reconstruction->planes[0],
        state->reconstruction->strides[0],
        state->reconstruction->widths[0],
        state->reconstruction->heights[0],
        x << 1U, y << 1U, width, height, 1U, 1U,
        plane == 1U ? decision->alpha_u : decision->alpha_v, 8U);
    return decoder_status == AVIFDEC_OK ? AVIFENC_OK
                                        : AVIFENC_LIMIT_EXCEEDED;
}

static Av1TxType tile_chroma_tx_type(uint8_t mode) {
    static const uint8_t mode_to_tx_type[AV1_UV_INTRA_MODES_CFL_ALLOWED] = {
        AV1_TX_DCT_DCT, AV1_TX_ADST_DCT, AV1_TX_DCT_ADST,
        AV1_TX_DCT_DCT, AV1_TX_ADST_ADST, AV1_TX_ADST_DCT,
        AV1_TX_DCT_ADST, AV1_TX_DCT_ADST, AV1_TX_ADST_DCT,
        AV1_TX_ADST_ADST, AV1_TX_ADST_DCT, AV1_TX_DCT_ADST,
        AV1_TX_ADST_ADST, AV1_TX_DCT_DCT
    };

    return mode < AV1_UV_INTRA_MODES_CFL_ALLOWED
        ? (Av1TxType)mode_to_tx_type[mode] : AV1_TX_DCT_DCT;
}

static uint32_t tile_cfl_joint_sign(int8_t alpha_u, int8_t alpha_v) {
    uint32_t sign_u = alpha_u == 0 ? 0U : alpha_u < 0 ? 1U : 2U;
    uint32_t sign_v = alpha_v == 0 ? 0U : alpha_v < 0 ? 1U : 2U;
    uint32_t symbol;

    for (symbol = 0U; symbol < AV1_CFL_JOINT_SIGNS; ++symbol) {
        if ((symbol + 1U) / 3U == sign_u &&
            (symbol + 1U) % 3U == sign_v) {
            return symbol;
        }
    }
    return AV1_CFL_JOINT_SIGNS;
}

static uint64_t tile_cfl_rate_cost(const AvifencAv1TileState *state,
                                   int8_t alpha_u,
                                   int8_t alpha_v) {
    uint32_t joint = tile_cfl_joint_sign(alpha_u, alpha_v);
    uint32_t sign_u;
    uint32_t sign_v;
    uint64_t cost;

    if (joint >= AV1_CFL_JOINT_SIGNS) return UINT64_MAX;
    sign_u = (joint + 1U) / 3U;
    sign_v = (joint + 1U) % 3U;
    cost = tile_symbol_cost(state->intra_cdfs.cfl_sign, joint);
    if (sign_u != 0U) {
        uint32_t magnitude = alpha_u < 0
            ? (uint32_t)(-(int32_t)alpha_u) : (uint32_t)alpha_u;
        cost += tile_symbol_cost(
            state->intra_cdfs.cfl_alpha[joint - 2U], magnitude - 1U);
    }
    if (sign_v != 0U) {
        uint32_t magnitude = alpha_v < 0
            ? (uint32_t)(-(int32_t)alpha_v) : (uint32_t)alpha_v;
        unsigned int context = (sign_v - 1U) * 3U + sign_u;
        cost += tile_symbol_cost(
            state->intra_cdfs.cfl_alpha[context], magnitude - 1U);
    }
    return cost;
}

static AvifencStatus tile_write_cfl(AvifencAv1TileState *state,
                                    int8_t alpha_u,
                                    int8_t alpha_v) {
    uint32_t joint = tile_cfl_joint_sign(alpha_u, alpha_v);
    uint32_t sign_u;
    uint32_t sign_v;
    AvifencStatus status;

    if (joint >= AV1_CFL_JOINT_SIGNS) return AVIFENC_INVALID_ARGUMENT;
    sign_u = (joint + 1U) / 3U;
    sign_v = (joint + 1U) % 3U;
    status = avifenc_av1_symbol_writer_write(
        state->writer, state->intra_cdfs.cfl_sign,
        AV1_CFL_JOINT_SIGNS, joint);
    if (status != AVIFENC_OK) return status;
    if (sign_u != 0U) {
        uint32_t magnitude = alpha_u < 0
            ? (uint32_t)(-(int32_t)alpha_u) : (uint32_t)alpha_u;

        status = avifenc_av1_symbol_writer_write(
            state->writer, state->intra_cdfs.cfl_alpha[joint - 2U],
            AV1_CFL_ALPHABET_SIZE, magnitude - 1U);
        if (status != AVIFENC_OK) return status;
    }
    if (sign_v != 0U) {
        uint32_t magnitude = alpha_v < 0
            ? (uint32_t)(-(int32_t)alpha_v) : (uint32_t)alpha_v;
        unsigned int context = (sign_v - 1U) * 3U + sign_u;

        status = avifenc_av1_symbol_writer_write(
            state->writer, state->intra_cdfs.cfl_alpha[context],
            AV1_CFL_ALPHABET_SIZE, magnitude - 1U);
    }
    return status;
}

static unsigned int tile_candidate_count(uint8_t speed) {
    static const unsigned int counts[3] = { 5U, 3U, 1U };

    return counts[speed];
}

static uint8_t tile_block_size(uint32_t width_mi, uint32_t height_mi) {
    static const uint8_t widths[10] = {
        1U, 1U, 2U, 2U, 2U, 4U, 4U, 4U, 8U, 8U
    };
    static const uint8_t heights[10] = {
        1U, 2U, 1U, 2U, 4U, 2U, 4U, 8U, 4U, 8U
    };
    uint8_t index;

    for (index = 0U; index < 10U; ++index) {
        if (widths[index] == width_mi && heights[index] == height_mi) {
            return index;
        }
    }
    return 0xffU;
}

static uint16_t *tile_y_mode_cdf(AvifencAv1TileState *state,
                                 uint32_t row,
                                 uint32_t column) {
    static const uint8_t mode_context[AV1_INTRA_MODES] = {
        0U, 1U, 2U, 3U, 4U, 4U, 4U, 4U, 3U, 0U, 1U, 2U, 0U
    };
    unsigned int above_context = 0U;
    unsigned int left_context = 0U;

    if (row != 0U) {
        above_context = mode_context[
            state->y_modes[(size_t)(row - 1U) * state->mi_columns + column]];
    }
    if (column != 0U) {
        left_context = mode_context[
            state->y_modes[(size_t)row * state->mi_columns + column - 1U]];
    }
    return state->intra_cdfs.y_mode[above_context][left_context];
}

static AvifencStatus tile_select_luma_mode(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    const uint16_t *mode_cdf,
    uint8_t *selected_mode) {
    static const uint8_t modes[5] = { 0U, 1U, 2U, 9U, 12U };
    uint64_t best_score = UINT64_MAX;
    unsigned int candidate;

    for (candidate = 0U;
         candidate < tile_candidate_count(state->source->speed);
         ++candidate) {
        AvifencAv1TransformBlock block;
        uint8_t mode = modes[candidate];
        uint64_t distortion;
        uint64_t rate_cost;
        uint64_t score;
        AvifencStatus status = tile_predict_plane_mode(
            state, 0U, column << 2U, row << 2U,
            4U, 4U,
            (uint8_t)(row != 0U), (uint8_t)(column != 0U), mode, 0, -1);

        if (state->source->statistics != 0) {
            ++state->source->statistics->prediction_trial_count;
        }
        if (status != AVIFENC_OK) return status;
        avifdec_memory_copy(
            state->transform.tx_type_set2,
            state->intra_cdfs.tx_type_set2[0][mode],
            sizeof(state->transform.tx_type_set2));
        status = avifenc_av1_transform_trial_4x4(
            &state->transform, 0U, column, row,
            state->source->planes[0], state->source->strides[0],
            state->source->width, state->source->height,
            state->reconstruction->planes[0],
            state->reconstruction->strides[0],
            (uint8_t)state->quantizer, state->quantizer != 0U, &block,
            &distortion, &rate_cost);
        if (state->source->statistics != 0) {
            ++state->source->statistics->transform_trial_count;
        }
        if (status != AVIFENC_OK) return status;
        rate_cost += tile_symbol_cost(mode_cdf, mode);
        if ((row & 1U) != 0U && (column & 1U) != 0U) {
            rate_cost += tile_symbol_cost(
                state->intra_cdfs.uv_mode_cfl_allowed[mode], 0U);
        }
        score = tile_candidate_score(
            distortion, rate_cost, state->quantizer);
        if (score < best_score) {
            best_score = score;
            *selected_mode = mode;
        }
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_tile_write_block(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column) {
    AvifencAv1TransformBlock transform_block;
    size_t index = (size_t)row * state->mi_columns + column;
    uint16_t *y_mode_cdf = tile_y_mode_cdf(state, row, column);
    uint8_t y_mode = 0U;
    uint8_t uv_mode = 0U;
    uint8_t segment = 0U;
    unsigned int skip_context = 0U;
    AvifencStatus status;

    if (state->source->quantization.adaptive_quantization != 0U) {
        segment = tile_select_segment(state, row, column, 1U, 1U);
        tile_set_effective_quantizer(state, segment);
    }
    status = tile_select_luma_mode(state, row, column, y_mode_cdf, &y_mode);

    if (status != AVIFENC_OK) return status;
    if (state->source->statistics != 0) {
        ++state->source->statistics->block_count;
    }
    state->block_widths[index] = 1U;
    state->block_heights[index] = 1U;
    if (row != 0U) {
        skip_context += state->block_flags[index - state->mi_columns] & 1U;
    }
    if (column != 0U) skip_context += state->block_flags[index - 1U] & 1U;
    status = avifenc_av1_symbol_writer_write(
        state->writer, state->skip[skip_context], 2U, 0U);
    if (status != AVIFENC_OK) return status;
    if (state->source->quantization.adaptive_quantization != 0U) {
        status = tile_write_segment_id(
            state, row, column, 1U, 1U, segment);
        if (status != AVIFENC_OK) return status;
    }
    status = avifenc_av1_symbol_writer_write(
        state->writer, y_mode_cdf, 13U, y_mode);
    if (status != AVIFENC_OK) return status;
    state->block_flags[index] = 0U;
    state->y_modes[index] = y_mode;
    if (state->source->statistics != 0) {
        state->source->statistics->luma_mode_mask |= (uint64_t)1U << y_mode;
    }
    status = tile_predict_plane_mode(
        state, 0U, column << 2U, row << 2U,
        4U, 4U,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), y_mode, 0, -1);
    if (status != AVIFENC_OK) return status;
    if ((row & 1U) != 0U && (column & 1U) != 0U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer,
            state->intra_cdfs.uv_mode_cfl_allowed[y_mode], 14U, uv_mode);
        if (status != AVIFENC_OK) return status;
        if (state->source->statistics != 0) {
            state->source->statistics->chroma_mode_mask |=
                (uint64_t)1U << uv_mode;
        }
        status = tile_predict_plane_mode(
            state, 1U, (column >> 1U) << 2U, (row >> 1U) << 2U,
            4U, 4U,
            (uint8_t)(row > 1U), (uint8_t)(column > 1U), uv_mode, 0, -1);
        if (status != AVIFENC_OK) return status;
        status = tile_predict_plane_mode(
            state, 2U, (column >> 1U) << 2U, (row >> 1U) << 2U,
            4U, 4U,
            (uint8_t)(row > 1U), (uint8_t)(column > 1U), uv_mode, 0, -1);
        if (status != AVIFENC_OK) return status;
    }
    if (y_mode == 0U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer, state->intra_cdfs.filter_intra[0], 2U, 0U);
        if (status != AVIFENC_OK) return status;
    }
    avifdec_memory_copy(
        state->transform.tx_type_set2,
        state->intra_cdfs.tx_type_set2[0][y_mode],
        sizeof(state->transform.tx_type_set2));
    status = avifenc_av1_transform_encode_4x4(
        &state->transform, state->writer, 0U, column, row,
        state->source->planes[0], state->source->strides[0],
        state->source->width, state->source->height,
        state->reconstruction->planes[0],
        state->reconstruction->strides[0], (uint8_t)state->quantizer,
        state->quantizer != 0U, &transform_block);
    if (status != AVIFENC_OK) return status;
    if (state->source->statistics != 0) {
        ++state->source->statistics->transform_count;
    }
    if ((row & 1U) != 0U && (column & 1U) != 0U) {
        unsigned int plane;

        for (plane = 1U; plane < 3U; ++plane) {
            status = avifenc_av1_transform_encode_4x4(
                &state->transform, state->writer, plane,
                column >> 1U, row >> 1U,
                state->source->planes[plane], state->source->strides[plane],
                state->source->width >> 1U, state->source->height >> 1U,
                state->reconstruction->planes[plane],
                state->reconstruction->strides[plane],
                (uint8_t)state->quantizer, 0, &transform_block);
            if (status != AVIFENC_OK) return status;
            if (state->source->statistics != 0) {
                ++state->source->statistics->transform_count;
            }
        }
    }
    if (state->source->quantization.adaptive_quantization != 0U) {
        tile_set_effective_quantizer(state, 0U);
    }
    return status;
}

static Av1TxSize tile_tx_size(uint32_t width, uint32_t height) {
    if (width == height) {
        return width == 4U ? AV1_TX_4X4
             : width == 8U ? AV1_TX_8X8
             : width == 16U ? AV1_TX_16X16
                            : AV1_TX_32X32;
    }
    if (width == 4U) return AV1_TX_4X8;
    if (height == 4U) return AV1_TX_8X4;
    if (width == 8U) return AV1_TX_8X16;
    if (height == 8U) return AV1_TX_16X8;
    if (width == 16U) return AV1_TX_16X32;
    return AV1_TX_32X16;
}

static int tile_tx_writes_type(Av1TxSize tx_size) {
    return av1_tx_size_info[tx_size].width < 32U &&
           av1_tx_size_info[tx_size].height < 32U;
}

static unsigned int tile_tx_type_context(Av1TxSize tx_size) {
    uint32_t minimum = av1_tx_size_info[tx_size].width <
            av1_tx_size_info[tx_size].height
        ? av1_tx_size_info[tx_size].width
        : av1_tx_size_info[tx_size].height;

    return minimum == 4U ? 0U : minimum == 8U ? 1U : 2U;
}

static uint8_t tile_luma_tx_mode(const AvifencAv1LumaDecision *decision) {
    static const uint8_t filter_mode_to_intra[5] = { 0U, 1U, 2U, 6U, 0U };

    return decision->filter_intra_mode >= 0
        ? filter_mode_to_intra[(unsigned int)decision->filter_intra_mode]
        : decision->mode;
}

static AvifencStatus tile_predict_luma(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width,
    uint32_t height,
    const AvifencAv1LumaDecision *decision) {
    if (decision->palette_size != 0U) {
        AvifdecStatus status = av1_predict_palette(
            state->reconstruction->planes[0] +
                (size_t)(row << 2U) * state->reconstruction->strides[0] +
                (column << 2U),
            state->reconstruction->strides[0], width, height, 8U,
            decision->palette_colors, decision->palette_size,
            state->palette_map_y, width);

        return status == AVIFDEC_OK ? AVIFENC_OK : AVIFENC_LIMIT_EXCEEDED;
    }
    return tile_predict_plane_mode(
        state, 0U, column << 2U, row << 2U, width, height,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), decision->mode,
        decision->angle_delta, decision->filter_intra_mode);
}

static unsigned int tile_luma_candidates(
    uint8_t speed,
    uint8_t allow_filter_intra,
    uint8_t allow_angle_delta,
    AvifencAv1LumaDecision candidates[66]) {
    unsigned int count = 0U;
    uint8_t mode;

    for (mode = 0U; mode < AV1_INTRA_MODES; ++mode) {
        candidates[count].mode = mode;
        candidates[count].angle_delta = 0;
        candidates[count].filter_intra_mode = -1;
        candidates[count].palette_size = 0U;
        ++count;
    }
    if (allow_angle_delta && speed < AVIFENC_MAX_SPEED) {
        int8_t minimum_delta = speed == 0U ? -AV1_MAX_ANGLE_DELTA : -1;
        int8_t maximum_delta = speed == 0U ? AV1_MAX_ANGLE_DELTA : 1;

        for (mode = 1U; mode <= AV1_DIRECTIONAL_MODES; ++mode) {
            int8_t delta;

            for (delta = minimum_delta; delta <= maximum_delta; ++delta) {
                if (delta == 0) continue;
                candidates[count].mode = mode;
                candidates[count].angle_delta = delta;
                candidates[count].filter_intra_mode = -1;
                candidates[count].palette_size = 0U;
                ++count;
            }
        }
    }
    if (allow_filter_intra) {
        int8_t filter_mode;

        for (filter_mode = 0; filter_mode < 5; ++filter_mode) {
            candidates[count].mode = 0U;
            candidates[count].angle_delta = 0;
            candidates[count].filter_intra_mode = filter_mode;
            candidates[count].palette_size = 0U;
            ++count;
        }
    }
    return count;
}

static AvifencStatus tile_select_luma_mode_sized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    const uint16_t *mode_cdf,
    AvifencAv1LumaDecision *selected,
    uint64_t *selected_score) {
    AvifencAv1LumaDecision candidates[66];
    uint32_t width = width_mi << 2U;
    uint32_t height = height_mi << 2U;
    Av1TxSize tx_size = tile_tx_size(width, height);
    uint8_t block_size = tile_block_size(width_mi, height_mi);
    unsigned int palette_context = 0U;
    unsigned int palette_block_context = 0U;
    unsigned int candidate_count;
    uint64_t best_score = UINT64_MAX;
    unsigned int candidate;

    if (block_size == 0xffU) return AVIFENC_UNSUPPORTED;
    if (block_size >= 3U) {
        palette_block_context = avifenc_av1_tile_palette_block_context(
            width_mi, height_mi);
        if (row != 0U && state->palette_sizes_y[
                (size_t)(row - 1U) * state->mi_columns + column] != 0U) {
            ++palette_context;
        }
        if (column != 0U && state->palette_sizes_y[
                (size_t)row * state->mi_columns + column - 1U] != 0U) {
            ++palette_context;
        }
    }
    candidate_count = tile_luma_candidates(
        state->source->speed,
        (uint8_t)(width_mi <= 8U && height_mi <= 8U),
        (uint8_t)(block_size >= 3U), candidates);
    for (candidate = 0U; candidate < candidate_count; ++candidate) {
        AvifencAv1TransformBlock block;
        const AvifencAv1LumaDecision *decision = &candidates[candidate];
        uint8_t mode = decision->mode;
        uint64_t distortion;
        uint64_t rate_cost;
        uint64_t score;
        AvifencStatus status = tile_predict_luma(
            state, row, column, width, height, decision);

        if (state->source->statistics != 0) {
            ++state->source->statistics->prediction_trial_count;
        }
        if (status != AVIFENC_OK) return status;
        if (tile_tx_writes_type(tx_size)) {
            avifdec_memory_copy(
                state->transform.tx_type_set2,
                state->intra_cdfs.tx_type_set2[
                    tile_tx_type_context(tx_size)][
                        tile_luma_tx_mode(decision)],
                sizeof(state->transform.tx_type_set2));
        }
        status = avifenc_av1_transform_trial(
            &state->transform, 0U, column, row, width, height, tx_size,
            state->source->planes[0], state->source->strides[0],
            state->source->width, state->source->height,
            state->reconstruction->planes[0],
            state->reconstruction->strides[0],
            (uint8_t)state->quantizer, AV1_TX_DCT_DCT,
            tile_tx_writes_type(tx_size),
            &block, &distortion, &rate_cost);
        if (state->source->statistics != 0) {
            ++state->source->statistics->transform_trial_count;
        }
        if (status != AVIFENC_OK) return status;
        rate_cost += tile_symbol_cost(mode_cdf, mode);
        if (block_size >= 3U && mode >= 1U && mode <= 8U) {
            rate_cost += tile_symbol_cost(
                state->intra_cdfs.angle_delta[mode - 1U],
                (size_t)(decision->angle_delta + AV1_MAX_ANGLE_DELTA));
        }
        if (mode == 0U) {
            if (block_size >= 3U) {
                rate_cost += tile_symbol_cost(
                    state->palette_cdfs.y_mode[palette_block_context]
                                              [palette_context], 0U);
            }
            rate_cost += tile_symbol_cost(
                state->intra_cdfs.filter_intra[block_size],
                decision->filter_intra_mode >= 0);
            if (decision->filter_intra_mode >= 0) {
                rate_cost += tile_symbol_cost(
                    state->intra_cdfs.filter_intra_mode,
                    (size_t)decision->filter_intra_mode);
            }
        }
        rate_cost += tile_symbol_cost(
            state->intra_cdfs.uv_mode_cfl_allowed[mode], 0U);
        score = tile_candidate_score(
            distortion, rate_cost, state->quantizer);
        if (score < best_score) {
            best_score = score;
            *selected = *decision;
        }
    }
    if (block_size >= 3U) {
        AvifencAv1LumaDecision decision;

        avifdec_memory_fill(&decision, 0U, sizeof(decision));
        decision.filter_intra_mode = -1;
        if (avifenc_av1_tile_palette_classify_luma(
                state, row, column, width_mi, height_mi, &decision)) {
            AvifencAv1TransformBlock block;
            uint64_t distortion;
            uint64_t rate_cost;
            uint64_t score;
            AvifencStatus status = tile_predict_luma(
                state, row, column, width, height, &decision);

            if (state->source->statistics != 0) {
                ++state->source->statistics->prediction_trial_count;
            }
            if (status != AVIFENC_OK) return status;
            if (tile_tx_writes_type(tx_size)) {
                avifdec_memory_copy(
                    state->transform.tx_type_set2,
                    state->intra_cdfs.tx_type_set2[
                        tile_tx_type_context(tx_size)][0],
                    sizeof(state->transform.tx_type_set2));
            }
            status = avifenc_av1_transform_trial(
                &state->transform, 0U, column, row, width, height, tx_size,
                state->source->planes[0], state->source->strides[0],
                state->source->width, state->source->height,
                state->reconstruction->planes[0],
                state->reconstruction->strides[0],
                (uint8_t)state->quantizer, AV1_TX_DCT_DCT,
                tile_tx_writes_type(tx_size),
                &block, &distortion, &rate_cost);
            if (state->source->statistics != 0) {
                ++state->source->statistics->transform_trial_count;
            }
            if (status != AVIFENC_OK) return status;
            rate_cost += tile_symbol_cost(mode_cdf, 0U);
            rate_cost += avifenc_av1_tile_palette_luma_rate_cost(
                state, width_mi, height_mi, &decision);
            rate_cost += tile_symbol_cost(
                state->intra_cdfs.uv_mode_cfl_allowed[0], 0U);
            score = tile_candidate_score(
                distortion, rate_cost, state->quantizer);
            if (score < best_score) {
                best_score = score;
                *selected = decision;
            }
        }
    }
    if (selected_score != 0) *selected_score = best_score;
    return AVIFENC_OK;
}

static uint64_t tile_chroma_prediction_distortion(
    const AvifencAv1TileState *state,
    unsigned int plane,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    const uint8_t *source = state->source->planes[plane];
    size_t source_stride = state->source->strides[plane];
    uint32_t source_width = state->source->width >> 1U;
    uint32_t source_height = state->source->height >> 1U;
    const uint16_t *prediction = state->reconstruction->planes[plane];
    size_t prediction_stride = state->reconstruction->strides[plane];
    uint64_t distortion = 0U;
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < height && y + row < source_height; ++row) {
        for (column = 0U; column < width && x + column < source_width;
             ++column) {
            int32_t difference =
                source[(size_t)(y + row) * source_stride + x + column] -
                prediction[(size_t)(y + row) * prediction_stride + x + column];

            distortion += (uint64_t)(difference * difference);
        }
    }
    return distortion;
}

static AvifencStatus tile_select_cfl_alpha(
    AvifencAv1TileState *state,
    unsigned int plane,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint8_t have_above,
    uint8_t have_left,
    int8_t *best_alpha,
    uint64_t *best_distortion,
    int8_t *best_nonzero_alpha,
    uint64_t *best_nonzero_distortion) {
    unsigned int step = 1U << state->source->speed;
    int alpha;

    *best_distortion = UINT64_MAX;
    *best_nonzero_distortion = UINT64_MAX;
    for (alpha = -16; alpha <= 16; alpha += (int)step) {
        AvifencAv1ChromaDecision decision = {
            13U, 0, 0, 0, 0U, { 0 }, { 0 }
        };
        uint64_t distortion;
        AvifencStatus status;

        if (plane == 1U) decision.alpha_u = (int8_t)alpha;
        else decision.alpha_v = (int8_t)alpha;
        status = tile_predict_chroma(
            state, plane, x, y, width, height, have_above, have_left,
            &decision);
        if (state->source->statistics != 0) {
            ++state->source->statistics->prediction_trial_count;
        }
        if (status != AVIFENC_OK) return status;
        distortion = tile_chroma_prediction_distortion(
            state, plane, x, y, width, height);
        if (distortion < *best_distortion) {
            *best_distortion = distortion;
            *best_alpha = (int8_t)alpha;
        }
        if (alpha != 0 && distortion < *best_nonzero_distortion) {
            *best_nonzero_distortion = distortion;
            *best_nonzero_alpha = (int8_t)alpha;
        }
    }
    return AVIFENC_OK;
}

static AvifencStatus tile_chroma_transform_trial(
    AvifencAv1TileState *state,
    unsigned int plane,
    uint32_t row,
    uint32_t column,
    uint32_t width,
    uint32_t height,
    Av1TxSize tx_size,
    Av1TxType tx_type,
    uint64_t *distortion,
    uint64_t *rate_cost) {
    AvifencAv1TransformBlock block;
    AvifencStatus status = avifenc_av1_transform_trial(
        &state->transform, plane, column >> 1U, row >> 1U,
        width, height, tx_size,
        state->source->planes[plane], state->source->strides[plane],
        state->source->width >> 1U, state->source->height >> 1U,
        state->reconstruction->planes[plane],
        state->reconstruction->strides[plane],
        (uint8_t)state->quantizer, tx_type, 0,
        &block, distortion, rate_cost);

    if (state->source->statistics != 0) {
        ++state->source->statistics->transform_trial_count;
    }
    return status;
}

static AvifencStatus tile_select_chroma_mode_sized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    uint8_t y_mode,
    AvifencAv1ChromaDecision *selected,
    uint64_t *selected_score) {
    AvifencAv1LumaDecision mode_candidates[66];
    uint32_t width = width_mi << 1U;
    uint32_t height = height_mi << 1U;
    uint32_t x = column << 1U;
    uint32_t y = row << 1U;
    uint8_t block_size = tile_block_size(width_mi, height_mi);
    Av1TxSize tx_size = tile_tx_size(width, height);
    uint16_t *mode_cdf = state->intra_cdfs.uv_mode_cfl_allowed[y_mode];
    unsigned int candidate_count;
    uint64_t best_score = UINT64_MAX;
    unsigned int candidate;

    if (block_size == 0xffU) return AVIFENC_UNSUPPORTED;
    candidate_count = tile_luma_candidates(
        state->source->speed, 0U, (uint8_t)(block_size >= 3U),
        mode_candidates);
    for (candidate = 0U; candidate < candidate_count; ++candidate) {
        AvifencAv1ChromaDecision decision = {
            mode_candidates[candidate].mode,
            mode_candidates[candidate].angle_delta, 0, 0, 0U, { 0 }, { 0 }
        };
        uint64_t distortion = 0U;
        uint64_t rate_cost = tile_symbol_cost(mode_cdf, decision.mode);
        uint64_t score;
        unsigned int plane;

        if (block_size >= 3U && decision.mode >= 1U && decision.mode <= 8U) {
            rate_cost += tile_symbol_cost(
                state->intra_cdfs.angle_delta[decision.mode - 1U],
                (size_t)(decision.angle_delta + AV1_MAX_ANGLE_DELTA));
        }
        if (block_size >= 3U && decision.mode == 0U) {
            rate_cost += tile_symbol_cost(
                state->palette_cdfs.uv_mode[0], 0U);
        }
        for (plane = 1U; plane < 3U; ++plane) {
            uint64_t plane_distortion;
            uint64_t plane_rate;
            AvifencStatus status = tile_predict_chroma(
                state, plane, x, y, width, height,
                (uint8_t)(row != 0U), (uint8_t)(column != 0U), &decision);

            if (state->source->statistics != 0) {
                ++state->source->statistics->prediction_trial_count;
            }
            if (status != AVIFENC_OK) return status;
            status = tile_chroma_transform_trial(
                state, plane, row, column, width, height, tx_size,
                tile_chroma_tx_type(decision.mode),
                &plane_distortion, &plane_rate);
            if (status != AVIFENC_OK) return status;
            distortion += plane_distortion;
            rate_cost += plane_rate;
        }
        score = tile_candidate_score(
            distortion <= UINT64_MAX / 2U ? distortion * 2U : UINT64_MAX,
            rate_cost, state->quantizer);
        if (score < best_score) {
            best_score = score;
            *selected = decision;
        }
    }
    if (block_size >= 3U) {
        AvifencAv1ChromaDecision decision;

        avifdec_memory_fill(&decision, 0U, sizeof(decision));
        if (avifenc_av1_tile_palette_classify_chroma(
                state, row, column, width_mi, height_mi, &decision)) {
            uint64_t distortion = 0U;
            uint64_t rate_cost = tile_symbol_cost(mode_cdf, 0U) +
                avifenc_av1_tile_palette_chroma_rate_cost(
                    state, width_mi, height_mi, &decision);
            uint64_t score;
            unsigned int plane;

            for (plane = 1U; plane < 3U; ++plane) {
                uint64_t plane_distortion;
                uint64_t plane_rate;
                AvifencStatus status = tile_predict_chroma(
                    state, plane, x, y, width, height,
                    (uint8_t)(row != 0U), (uint8_t)(column != 0U),
                    &decision);

                if (state->source->statistics != 0) {
                    ++state->source->statistics->prediction_trial_count;
                }
                if (status != AVIFENC_OK) return status;
                status = tile_chroma_transform_trial(
                    state, plane, row, column, width, height, tx_size,
                    AV1_TX_DCT_DCT, &plane_distortion, &plane_rate);
                if (status != AVIFENC_OK) return status;
                distortion += plane_distortion;
                rate_cost += plane_rate;
            }
            score = tile_candidate_score(
                distortion <= UINT64_MAX / 2U ? distortion * 2U : UINT64_MAX,
                rate_cost, state->quantizer);
            if (score < best_score) {
                best_score = score;
                *selected = decision;
            }
        }
    }
    {
        AvifencAv1ChromaDecision decision = {
            13U, 0, 0, 0, 0U, { 0 }, { 0 }
        };
        int8_t nonzero_u = 0;
        int8_t nonzero_v = 0;
        uint64_t raw_u;
        uint64_t raw_v;
        uint64_t nonzero_distortion_u;
        uint64_t nonzero_distortion_v;
        uint64_t distortion = 0U;
        uint64_t rate_cost;
        uint64_t score;
        unsigned int plane;
        AvifencStatus status = tile_select_cfl_alpha(
            state, 1U, x, y, width, height,
            (uint8_t)(row != 0U), (uint8_t)(column != 0U),
            &decision.alpha_u, &raw_u, &nonzero_u, &nonzero_distortion_u);

        if (status != AVIFENC_OK) return status;
        status = tile_select_cfl_alpha(
            state, 2U, x, y, width, height,
            (uint8_t)(row != 0U), (uint8_t)(column != 0U),
            &decision.alpha_v, &raw_v, &nonzero_v, &nonzero_distortion_v);
        if (status != AVIFENC_OK) return status;
        if (decision.alpha_u == 0 && decision.alpha_v == 0) {
            if (nonzero_distortion_u - raw_u <= nonzero_distortion_v - raw_v) {
                decision.alpha_u = nonzero_u;
            } else {
                decision.alpha_v = nonzero_v;
            }
        }
        rate_cost = tile_symbol_cost(mode_cdf, 13U) +
            tile_cfl_rate_cost(state, decision.alpha_u, decision.alpha_v);
        for (plane = 1U; plane < 3U; ++plane) {
            uint64_t plane_distortion;
            uint64_t plane_rate;

            status = tile_predict_chroma(
                state, plane, x, y, width, height,
                (uint8_t)(row != 0U), (uint8_t)(column != 0U), &decision);
            if (state->source->statistics != 0) {
                ++state->source->statistics->prediction_trial_count;
            }
            if (status != AVIFENC_OK) return status;
            status = tile_chroma_transform_trial(
                state, plane, row, column, width, height, tx_size,
                tile_chroma_tx_type(decision.mode),
                &plane_distortion, &plane_rate);
            if (status != AVIFENC_OK) return status;
            distortion += plane_distortion;
            rate_cost += plane_rate;
        }
        score = tile_candidate_score(
            distortion <= UINT64_MAX / 2U ? distortion * 2U : UINT64_MAX,
            rate_cost, state->quantizer);
        if (score < best_score) {
            best_score = score;
            *selected = decision;
        }
    }
    if (selected_score != 0) *selected_score = best_score;
    return AVIFENC_OK;
}

static void tile_store_block_state(AvifencAv1TileState *state,
                                   uint32_t row,
                                   uint32_t column,
                                   uint32_t width,
                                   uint32_t height,
                                   uint8_t y_mode) {
    uint32_t y;
    uint32_t x;

    for (y = 0U; y < height; ++y) {
        for (x = 0U; x < width; ++x) {
            size_t index = (size_t)(row + y) * state->mi_columns +
                column + x;

            state->block_widths[index] = (uint8_t)width;
            state->block_heights[index] = (uint8_t)height;
            state->block_flags[index] = 0U;
            state->y_modes[index] = y_mode;
        }
    }
}

static void tile_store_uv_mode(AvifencAv1TileState *state,
                               uint32_t row,
                               uint32_t column,
                               uint32_t width,
                               uint32_t height,
                               uint8_t uv_mode) {
    uint32_t y;
    uint32_t x;

    for (y = 0U; y < height; ++y) {
        for (x = 0U; x < width; ++x) {
            state->uv_modes[(size_t)(row + y) * state->mi_columns +
                            column + x] = uv_mode;
        }
    }
}

static void tile_store_palette_sizes(AvifencAv1TileState *state,
                                     uint32_t row,
                                     uint32_t column,
                                     uint32_t width,
                                     uint32_t height,
                                     uint8_t palette_size_y,
                                     uint8_t palette_size_uv) {
    uint32_t y;
    uint32_t x;

    for (y = 0U; y < height; ++y) {
        for (x = 0U; x < width; ++x) {
            size_t index = (size_t)(row + y) * state->mi_columns +
                column + x;

            state->palette_sizes_y[index] = palette_size_y;
            state->palette_sizes_uv[index] = palette_size_uv;
        }
    }
}

static AvifencStatus tile_trial_block_sized_quantized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    uint64_t *score) {
    AvifencAv1TransformBlock block;
    uint32_t width = width_mi << 2U;
    uint32_t height = height_mi << 2U;
    Av1TxSize tx_size = tile_tx_size(width, height);
    uint16_t *mode_cdf = tile_y_mode_cdf(state, row, column);
    AvifencAv1LumaDecision decision;
    uint64_t distortion;
    uint64_t rate_cost;
    AvifencStatus status = tile_select_luma_mode_sized(
        state, row, column, width_mi, height_mi, mode_cdf, &decision, score);

    if (status != AVIFENC_OK) return status;
    tile_store_block_state(
        state, row, column, width_mi, height_mi, decision.mode);
    tile_store_palette_sizes(
        state, row, column, width_mi, height_mi,
        decision.palette_size, 0U);
    status = tile_predict_luma(
        state, row, column, width, height, &decision);
    if (state->source->statistics != 0) {
        ++state->source->statistics->prediction_trial_count;
    }
    if (status != AVIFENC_OK) return status;
    if (tile_tx_writes_type(tx_size)) {
        avifdec_memory_copy(
            state->transform.tx_type_set2,
            state->intra_cdfs.tx_type_set2[
                tile_tx_type_context(tx_size)][tile_luma_tx_mode(&decision)],
            sizeof(state->transform.tx_type_set2));
    }
    status = avifenc_av1_transform_trial(
        &state->transform, 0U, column, row, width, height, tx_size,
        state->source->planes[0], state->source->strides[0],
        state->source->width, state->source->height,
        state->reconstruction->planes[0],
        state->reconstruction->strides[0],
        (uint8_t)state->quantizer, AV1_TX_DCT_DCT,
        tile_tx_writes_type(tx_size),
        &block, &distortion, &rate_cost);
    if (state->source->statistics != 0) {
        ++state->source->statistics->transform_trial_count;
    }
    return status;
}

AvifencStatus avifenc_av1_tile_trial_block_sized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    uint64_t *score) {
    AvifencStatus status;

    if (state->source->quantization.adaptive_quantization != 0U) {
        tile_set_effective_quantizer(
            state, tile_select_segment(
                state, row, column, width_mi, height_mi));
    }
    status = tile_trial_block_sized_quantized(
        state, row, column, width_mi, height_mi, score);
    if (state->source->quantization.adaptive_quantization != 0U) {
        tile_set_effective_quantizer(state, 0U);
    }
    return status;
}

static AvifencStatus tile_write_block_sized_quantized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    uint8_t segment) {
    AvifencAv1TransformBlock transform_block;
    size_t index = (size_t)row * state->mi_columns + column;
    uint16_t *y_mode_cdf = tile_y_mode_cdf(state, row, column);
    uint32_t width = width_mi << 2U;
    uint32_t height = height_mi << 2U;
    uint32_t chroma_width = width >> 1U;
    uint32_t chroma_height = height >> 1U;
    Av1TxSize luma_tx = tile_tx_size(width, height);
    Av1TxSize chroma_tx = tile_tx_size(chroma_width, chroma_height);
    AvifencAv1LumaDecision y_decision;
    AvifencAv1ChromaDecision uv_decision;
    uint8_t y_mode;
    uint8_t block_size = tile_block_size(width_mi, height_mi);
    unsigned int palette_context = 0U;
    unsigned int skip_context = 0U;
    AvifencStatus status = tile_select_luma_mode_sized(
        state, row, column, width_mi, height_mi, y_mode_cdf, &y_decision, 0);

    if (status != AVIFENC_OK) return status;
    y_mode = y_decision.mode;
    status = tile_predict_luma(
        state, row, column, width, height, &y_decision);
    if (state->source->statistics != 0) {
        ++state->source->statistics->prediction_trial_count;
    }
    if (status != AVIFENC_OK) return status;
    if (tile_tx_writes_type(luma_tx)) {
        avifdec_memory_copy(
            state->transform.tx_type_set2,
            state->intra_cdfs.tx_type_set2[
                tile_tx_type_context(luma_tx)][tile_luma_tx_mode(&y_decision)],
            sizeof(state->transform.tx_type_set2));
    }
    {
        uint64_t distortion;
        uint64_t rate_cost;

        status = avifenc_av1_transform_trial(
            &state->transform, 0U, column, row, width, height, luma_tx,
            state->source->planes[0], state->source->strides[0],
            state->source->width, state->source->height,
            state->reconstruction->planes[0],
            state->reconstruction->strides[0],
            (uint8_t)state->quantizer, AV1_TX_DCT_DCT,
            tile_tx_writes_type(luma_tx),
            &transform_block, &distortion, &rate_cost);
        if (state->source->statistics != 0) {
            ++state->source->statistics->transform_trial_count;
        }
        if (status != AVIFENC_OK) return status;
    }
    status = tile_select_chroma_mode_sized(
        state, row, column, width_mi, height_mi, y_mode, &uv_decision, 0);
    if (status != AVIFENC_OK) return status;
    if (state->source->statistics != 0) {
        ++state->source->statistics->block_count;
        state->source->statistics->luma_mode_mask |=
            (uint64_t)1U << y_mode;
        state->source->statistics->chroma_mode_mask |=
            (uint64_t)1U << uv_decision.mode;
        if (y_mode >= 1U && y_mode <= 8U) {
            state->source->statistics->angle_delta_mask |=
                (uint64_t)1U <<
                (unsigned int)(y_decision.angle_delta + AV1_MAX_ANGLE_DELTA);
        }
        if (uv_decision.mode >= 1U && uv_decision.mode <= 8U) {
            state->source->statistics->angle_delta_mask |=
                (uint64_t)1U <<
                (unsigned int)(uv_decision.angle_delta + AV1_MAX_ANGLE_DELTA);
        }
        if (uv_decision.mode == 13U) {
            ++state->source->statistics->cfl_block_count;
        }
        if (y_decision.filter_intra_mode >= 0) {
            ++state->source->statistics->filter_intra_block_count;
        }
        if (y_decision.palette_size != 0U) {
            ++state->source->statistics->palette_block_count;
        }
        if (uv_decision.palette_size != 0U) {
            ++state->source->statistics->palette_block_count;
        }
    }
    if (row != 0U) {
        skip_context += state->block_flags[index - state->mi_columns] & 1U;
    }
    if (column != 0U) skip_context += state->block_flags[index - 1U] & 1U;
    status = avifenc_av1_symbol_writer_write(
        state->writer, state->skip[skip_context], 2U, 0U);
    if (status != AVIFENC_OK) return status;
    if (state->source->quantization.adaptive_quantization != 0U) {
        status = tile_write_segment_id(
            state, row, column, width_mi, height_mi, segment);
        if (status != AVIFENC_OK) return status;
    }
    status = avifenc_av1_symbol_writer_write(
        state->writer, y_mode_cdf, 13U, y_mode);
    if (status != AVIFENC_OK) return status;
    if (block_size >= 3U && y_mode >= 1U && y_mode <= 8U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer, state->intra_cdfs.angle_delta[y_mode - 1U],
            2U * AV1_MAX_ANGLE_DELTA + 1U,
            (size_t)(y_decision.angle_delta + AV1_MAX_ANGLE_DELTA));
        if (status != AVIFENC_OK) return status;
    }
    status = avifenc_av1_symbol_writer_write(
        state->writer, state->intra_cdfs.uv_mode_cfl_allowed[y_mode],
        AV1_UV_INTRA_MODES_CFL_ALLOWED, uv_decision.mode);
    if (status != AVIFENC_OK) return status;
    if (uv_decision.mode == 13U) {
        status = tile_write_cfl(
            state, uv_decision.alpha_u, uv_decision.alpha_v);
        if (status != AVIFENC_OK) return status;
    } else if (block_size >= 3U && uv_decision.mode >= 1U &&
               uv_decision.mode <= 8U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer,
            state->intra_cdfs.angle_delta[uv_decision.mode - 1U],
            2U * AV1_MAX_ANGLE_DELTA + 1U,
            (size_t)(uv_decision.angle_delta + AV1_MAX_ANGLE_DELTA));
        if (status != AVIFENC_OK) return status;
    }
    if (block_size >= 3U) {
        unsigned int palette_block_context = avifenc_av1_tile_palette_block_context(
            width_mi, height_mi);

        if (row != 0U && state->palette_sizes_y[
                (size_t)(row - 1U) * state->mi_columns + column] != 0U) {
            ++palette_context;
        }
        if (column != 0U && state->palette_sizes_y[
                (size_t)row * state->mi_columns + column - 1U] != 0U) {
            ++palette_context;
        }
        if (y_mode == 0U) {
            status = avifenc_av1_symbol_writer_write(
                state->writer,
                state->palette_cdfs.y_mode[palette_block_context]
                                          [palette_context],
                2U, y_decision.palette_size != 0U);
            if (status != AVIFENC_OK) return status;
            if (y_decision.palette_size != 0U) {
                status = avifenc_av1_symbol_writer_write(
                    state->writer,
                    state->palette_cdfs.y_size[palette_block_context], 7U,
                    y_decision.palette_size - 2U);
                if (status != AVIFENC_OK) return status;
                status = avifenc_av1_tile_write_palette_luma_colors(state, &y_decision);
                if (status != AVIFENC_OK) return status;
            }
        }
        if (uv_decision.mode == 0U) {
            status = avifenc_av1_symbol_writer_write(
                state->writer,
                state->palette_cdfs.uv_mode[y_decision.palette_size != 0U],
                2U, uv_decision.palette_size != 0U);
            if (status != AVIFENC_OK) return status;
            if (uv_decision.palette_size != 0U) {
                status = avifenc_av1_symbol_writer_write(
                    state->writer,
                    state->palette_cdfs.uv_size[palette_block_context], 7U,
                    uv_decision.palette_size - 2U);
                if (status != AVIFENC_OK) return status;
                status = avifenc_av1_tile_write_palette_chroma_colors(
                    state, &uv_decision);
                if (status != AVIFENC_OK) return status;
            }
        }
    }
    if (y_mode == 0U && y_decision.palette_size == 0U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer, state->intra_cdfs.filter_intra[block_size], 2U,
            y_decision.filter_intra_mode >= 0);
        if (status != AVIFENC_OK) return status;
        if (y_decision.filter_intra_mode >= 0) {
            status = avifenc_av1_symbol_writer_write(
                state->writer, state->intra_cdfs.filter_intra_mode, 5U,
                (size_t)y_decision.filter_intra_mode);
            if (status != AVIFENC_OK) return status;
        }
    }
    if (y_decision.palette_size != 0U) {
        status = avifenc_av1_tile_write_palette_map(
            state, state->palette_map_y, width, height,
            y_decision.palette_size, 0U);
        if (status != AVIFENC_OK) return status;
    }
    if (uv_decision.palette_size != 0U) {
        status = avifenc_av1_tile_write_palette_map(
            state, state->palette_map_uv, chroma_width, chroma_height,
            uv_decision.palette_size, 1U);
        if (status != AVIFENC_OK) return status;
    }
    tile_store_block_state(
        state, row, column, width_mi, height_mi, y_mode);
    tile_store_uv_mode(
        state, row, column, width_mi, height_mi, uv_decision.mode);
    tile_store_palette_sizes(
        state, row, column, width_mi, height_mi,
        y_decision.palette_size, uv_decision.palette_size);
    status = tile_predict_luma(
        state, row, column, width, height, &y_decision);
    if (status != AVIFENC_OK) return status;
    if (tile_tx_writes_type(luma_tx)) {
        avifdec_memory_copy(
            state->transform.tx_type_set2,
            state->intra_cdfs.tx_type_set2[
                tile_tx_type_context(luma_tx)][tile_luma_tx_mode(&y_decision)],
            sizeof(state->transform.tx_type_set2));
    }
    status = avifenc_av1_transform_encode(
        &state->transform, state->writer, 0U, column, row,
        width, height, luma_tx,
        state->source->planes[0], state->source->strides[0],
        state->source->width, state->source->height,
        state->reconstruction->planes[0],
        state->reconstruction->strides[0], (uint8_t)state->quantizer,
        AV1_TX_DCT_DCT,
        tile_tx_writes_type(luma_tx), &transform_block);
    if (status != AVIFENC_OK) return status;
    if (state->source->statistics != 0) {
        ++state->source->statistics->transform_count;
    }
    status = tile_predict_chroma(
        state, 1U, column << 1U, row << 1U,
        chroma_width, chroma_height,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), &uv_decision);
    if (status != AVIFENC_OK) return status;
    status = tile_predict_chroma(
        state, 2U, column << 1U, row << 1U,
        chroma_width, chroma_height,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), &uv_decision);
    if (status != AVIFENC_OK) return status;
    {
        unsigned int plane;

        for (plane = 1U; plane < 3U; ++plane) {
            status = avifenc_av1_transform_encode(
                &state->transform, state->writer, plane,
                column >> 1U, row >> 1U,
                chroma_width, chroma_height, chroma_tx,
                state->source->planes[plane], state->source->strides[plane],
                state->source->width >> 1U, state->source->height >> 1U,
                state->reconstruction->planes[plane],
                state->reconstruction->strides[plane],
                (uint8_t)state->quantizer,
                tile_chroma_tx_type(uv_decision.mode), 0,
                &transform_block);
            if (status != AVIFENC_OK) return status;
            if (state->source->statistics != 0) {
                ++state->source->statistics->transform_count;
            }
        }
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_tile_write_block_sized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi) {
    uint8_t segment = 0U;
    AvifencStatus status;

    if (state->source->quantization.adaptive_quantization != 0U) {
        segment = tile_select_segment(
            state, row, column, width_mi, height_mi);
        tile_set_effective_quantizer(state, segment);
    }
    status = tile_write_block_sized_quantized(
        state, row, column, width_mi, height_mi, segment);
    if (state->source->quantization.adaptive_quantization != 0U) {
        tile_set_effective_quantizer(state, 0U);
    }
    return status;
}
