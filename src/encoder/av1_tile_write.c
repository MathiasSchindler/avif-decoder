#include "encoder/av1_tile_write.h"
#include "encoder/av1_transform_write.h"
#include "av1_intra.h"
#include "av1_predict.h"
#include "av1_tile.h"
#include "base.h"

typedef struct {
    AvifencAv1SymbolWriter *writer;
    const AvifencAv1TileSource *source;
    AvifencAv1TileReconstruction *reconstruction;
    uint16_t quantizer;
    uint32_t mi_rows;
    uint32_t mi_columns;
    uint8_t *block_widths;
    uint8_t *block_heights;
    uint8_t *block_flags;
    uint8_t *segment_ids;
    uint8_t *y_modes;
    uint8_t *uv_modes;
    uint8_t *palette_sizes_y;
    uint8_t *palette_sizes_uv;
    uint8_t *palette_map_y;
    uint8_t *palette_map_uv;
    uint8_t *trial_reconstruction;
    AvifencAv1TransformState transform;
    Av1IntraCdfs intra_cdfs;
    Av1PaletteCdfs palette_cdfs;
    uint16_t partition8[4][5];
    uint16_t partition16[4][11];
    uint16_t partition32[4][11];
    uint16_t partition64[4][11];
    uint16_t skip[3][3];
    uint16_t segment_id[3][9];
} AvifencAv1TileState;

typedef struct {
    uint8_t mode;
    int8_t angle_delta;
    int8_t filter_intra_mode;
    uint8_t palette_size;
    uint16_t palette_colors[8];
} AvifencAv1LumaDecision;

typedef struct {
    uint8_t mode;
    int8_t angle_delta;
    int8_t alpha_u;
    int8_t alpha_v;
    uint8_t palette_size;
    uint16_t palette_colors_u[8];
    uint16_t palette_colors_v[8];
} AvifencAv1ChromaDecision;

static const uint16_t tile_default_partition8[4][5] = {
    { 19132U, 25510U, 30392U, 32768U, 0U },
    { 13928U, 19855U, 28540U, 32768U, 0U },
    { 12522U, 23679U, 28629U, 32768U, 0U },
    { 9896U, 18783U, 25853U, 32768U, 0U }
};

static const uint16_t tile_default_partition16[4][11] = {
    { 15597U, 20929U, 24571U, 26706U, 27664U, 28821U, 29601U, 30571U, 31902U, 32768U, 0U },
    { 7925U, 11043U, 16785U, 22470U, 23971U, 25043U, 26651U, 28701U, 29834U, 32768U, 0U },
    { 5414U, 13269U, 15111U, 20488U, 22360U, 24500U, 25537U, 26336U, 32117U, 32768U, 0U },
    { 2662U, 6362U, 8614U, 20860U, 23053U, 24778U, 26436U, 27829U, 31171U, 32768U, 0U }
};

static const uint16_t tile_default_partition32[4][11] = {
    { 18462U, 20920U, 23124U, 27647U, 28227U, 29049U, 29519U, 30178U, 31544U, 32768U, 0U },
    { 7689U, 9060U, 12056U, 24992U, 25660U, 26182U, 26951U, 28041U, 29052U, 32768U, 0U },
    { 6015U, 9009U, 10062U, 24544U, 25409U, 26545U, 27071U, 27526U, 32047U, 32768U, 0U },
    { 1394U, 2208U, 2796U, 28614U, 29061U, 29466U, 29840U, 30185U, 31899U, 32768U, 0U }
};

static const uint16_t tile_default_partition64[4][11] = {
    { 20137U, 21547U, 23078U, 29566U, 29837U, 30261U, 30524U, 30892U, 31724U, 32768U, 0U },
    { 6732U, 7490U, 9497U, 27944U, 28250U, 28515U, 28969U, 29630U, 30104U, 32768U, 0U },
    { 5945U, 7663U, 8348U, 28683U, 29117U, 29749U, 30064U, 30298U, 32238U, 32768U, 0U },
    { 870U, 1212U, 1487U, 31198U, 31394U, 31574U, 31743U, 31881U, 32332U, 32768U, 0U }
};

static const uint16_t tile_default_skip[3][3] = {
    { 31671U, 32768U, 0U },
    { 16515U, 32768U, 0U },
    { 4576U, 32768U, 0U }
};

static const uint16_t tile_default_segment_id[3][9] = {
    { 5622U, 7893U, 16093U, 18233U, 27809U, 28373U, 32533U, 32768U, 0U },
    { 14274U, 18230U, 22557U, 24935U, 29980U, 30851U, 32344U, 32768U, 0U },
    { 27527U, 28487U, 28723U, 28890U, 32397U, 32647U, 32679U, 32768U, 0U }
};

static int tile_size_multiply(size_t left, size_t right, size_t *result) {
    if (left != 0U && right > (size_t)-1 / left) return 0;
    *result = left * right;
    return 1;
}

static int tile_size_add(size_t left, size_t right, size_t *result) {
    if (right > (size_t)-1 - left) return 0;
    *result = left + right;
    return 1;
}

static AvifencStatus tile_requirements(
    const AvifencAv1TileSource *source,
    AvifencAv1TileRequirements *requirements) {
    size_t cells;
    size_t block_workspace;
    size_t transform_workspace;
    size_t workspace_required;
    uint32_t mi_columns;
    uint32_t mi_rows;

    if (source == 0 || requirements == 0 || source->width == 0U ||
        source->height == 0U || (source->width & 1U) != 0U ||
        (source->height & 1U) != 0U ||
        source->width > AVIFENC_MAX_DIMENSION ||
        source->height > AVIFENC_MAX_DIMENSION ||
        source->quantizer > 255U || source->speed > AVIFENC_MAX_SPEED ||
        source->planes[0] == 0 ||
        source->planes[1] == 0 || source->planes[2] == 0 ||
        source->strides[0] < source->width ||
        source->strides[1] < (source->width >> 1U) ||
        source->strides[2] < (source->width >> 1U)) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    mi_columns = 2U * ((source->width + 7U) >> 3U);
    mi_rows = 2U * ((source->height + 7U) >> 3U);
    if (!tile_size_multiply(mi_columns, mi_rows, &cells) ||
        !tile_size_multiply(cells, 8U, &block_workspace) ||
        avifenc_av1_transform_context_size(
            mi_columns, mi_rows, &transform_workspace) != AVIFENC_OK ||
        !tile_size_add(
            block_workspace, transform_workspace, &workspace_required) ||
        !tile_size_add(workspace_required,
                       32U * 32U * sizeof(uint16_t),
                       &workspace_required) ||
        !tile_size_add(workspace_required, 32U * 32U + 16U * 16U,
                       &workspace_required)) {
        return AVIFENC_OVERFLOW;
    }
    requirements->workspace_required = workspace_required;
    requirements->reconstruction_widths[0] = mi_columns << 2U;
    requirements->reconstruction_heights[0] = mi_rows << 2U;
    requirements->reconstruction_widths[1] =
        requirements->reconstruction_widths[2] =
        ((mi_columns + 1U) >> 1U) << 2U;
    requirements->reconstruction_heights[1] =
        requirements->reconstruction_heights[2] =
        ((mi_rows + 1U) >> 1U) << 2U;
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_tile_query(
    const AvifencAv1TileSource *source,
    AvifencAv1TileRequirements *requirements) {
    return tile_requirements(source, requirements);
}

static void tile_cdfs_init(AvifencAv1TileState *state) {
    avifdec_memory_copy(state->partition8, tile_default_partition8,
                        sizeof(state->partition8));
    avifdec_memory_copy(state->partition16, tile_default_partition16,
                        sizeof(state->partition16));
    avifdec_memory_copy(state->partition32, tile_default_partition32,
                        sizeof(state->partition32));
    avifdec_memory_copy(state->partition64, tile_default_partition64,
                        sizeof(state->partition64));
    avifdec_memory_copy(state->skip, tile_default_skip, sizeof(state->skip));
    avifdec_memory_copy(state->segment_id, tile_default_segment_id,
                        sizeof(state->segment_id));
    av1_intra_cdfs_init(&state->intra_cdfs);
    av1_palette_cdfs_init(&state->palette_cdfs);
}

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

static uint64_t tile_symbol_cost(const uint16_t *cdf, size_t symbol) {
    uint32_t low = symbol == 0U ? 0U : cdf[symbol - 1U];
    uint32_t probability = cdf[symbol] - low;
    uint64_t cost = 0U;

    if (probability == 0U) probability = 1U;
    while (probability < 32768U) {
        probability <<= 1U;
        cost += 256U;
    }
    return cost;
}

static uint64_t tile_candidate_score(uint64_t distortion,
                                     uint64_t rate_cost,
                                     uint16_t quantizer) {
    uint64_t lambda = 1U +
        ((uint64_t)quantizer * quantizer >> 8U);

    return distortion * 256U + rate_cost * lambda;
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

static AvifencStatus tile_write_block(AvifencAv1TileState *state,
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

static unsigned int tile_ceil_log2(uint32_t value) {
    unsigned int bits = 0U;

    if (value <= 1U) return 0U;
    --value;
    while (value != 0U) {
        value >>= 1U;
        ++bits;
    }
    return bits;
}

static unsigned int tile_log2_mi(uint32_t value) {
    unsigned int result = 0U;

    while (value > 1U) {
        value >>= 1U;
        ++result;
    }
    return result;
}

static unsigned int tile_palette_block_context(uint32_t width_mi,
                                                uint32_t height_mi) {
    return tile_log2_mi(width_mi) + tile_log2_mi(height_mi) - 2U;
}

static int tile_palette_classify_luma(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    AvifencAv1LumaDecision *decision) {
    uint32_t width = width_mi << 2U;
    uint32_t height = height_mi << 2U;
    uint32_t y;
    uint32_t x;
    uint8_t count = 0U;

    if (width_mi < 2U || height_mi < 2U || width_mi > 16U ||
        height_mi > 16U || ((uint64_t)column + width_mi) * 4U >
            state->source->width || ((uint64_t)row + height_mi) * 4U >
            state->source->height ||
        (row != 0U && state->palette_sizes_y[
            (size_t)(row - 1U) * state->mi_columns + column] != 0U) ||
        (column != 0U && state->palette_sizes_y[
            (size_t)row * state->mi_columns + column - 1U] != 0U)) {
        return 0;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source = state->source->planes[0] +
            ((size_t)(row << 2U) + y) * state->source->strides[0] +
            (column << 2U);

        for (x = 0U; x < width; ++x) {
            uint8_t sample = source[x];
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors[index] == sample) break;
            }
            if (index == count) {
                if (count == 8U) return 0;
                decision->palette_colors[count++] = sample;
            }
        }
    }
    if (count < 2U) return 0;
    for (x = 1U; x < count; ++x) {
        uint16_t value = decision->palette_colors[x];
        uint32_t position = x;

        while (position != 0U &&
               decision->palette_colors[position - 1U] > value) {
            decision->palette_colors[position] =
                decision->palette_colors[position - 1U];
            --position;
        }
        decision->palette_colors[position] = value;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source = state->source->planes[0] +
            ((size_t)(row << 2U) + y) * state->source->strides[0] +
            (column << 2U);

        for (x = 0U; x < width; ++x) {
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors[index] == source[x]) break;
            }
            state->palette_map_y[(size_t)y * width + x] = index;
        }
    }
    decision->mode = 0U;
    decision->angle_delta = 0;
    decision->filter_intra_mode = -1;
    decision->palette_size = count;
    return 1;
}

static int tile_palette_classify_chroma(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    AvifencAv1ChromaDecision *decision) {
    uint32_t width = width_mi << 1U;
    uint32_t height = height_mi << 1U;
    uint32_t y;
    uint32_t x;
    uint8_t count = 0U;

    if (width_mi < 2U || height_mi < 2U || width_mi > 8U ||
        height_mi > 8U || ((uint64_t)column + width_mi) * 4U >
            state->source->width || ((uint64_t)row + height_mi) * 4U >
            state->source->height ||
        (row != 0U && state->palette_sizes_uv[
            (size_t)(row - 1U) * state->mi_columns + column] != 0U) ||
        (column != 0U && state->palette_sizes_uv[
            (size_t)row * state->mi_columns + column - 1U] != 0U)) {
        return 0;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source_u = state->source->planes[1] +
            ((size_t)(row << 1U) + y) * state->source->strides[1] +
            (column << 1U);
        const uint8_t *source_v = state->source->planes[2] +
            ((size_t)(row << 1U) + y) * state->source->strides[2] +
            (column << 1U);

        for (x = 0U; x < width; ++x) {
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors_u[index] == source_u[x] &&
                    decision->palette_colors_v[index] == source_v[x]) {
                    break;
                }
            }
            if (index == count) {
                if (count == 8U) return 0;
                decision->palette_colors_u[count] = source_u[x];
                decision->palette_colors_v[count] = source_v[x];
                ++count;
            }
        }
    }
    if (count < 2U) return 0;
    for (x = 1U; x < count; ++x) {
        uint16_t value_u = decision->palette_colors_u[x];
        uint16_t value_v = decision->palette_colors_v[x];
        uint32_t position = x;

        while (position != 0U &&
               decision->palette_colors_u[position - 1U] > value_u) {
            decision->palette_colors_u[position] =
                decision->palette_colors_u[position - 1U];
            decision->palette_colors_v[position] =
                decision->palette_colors_v[position - 1U];
            --position;
        }
        decision->palette_colors_u[position] = value_u;
        decision->palette_colors_v[position] = value_v;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source_u = state->source->planes[1] +
            ((size_t)(row << 1U) + y) * state->source->strides[1] +
            (column << 1U);
        const uint8_t *source_v = state->source->planes[2] +
            ((size_t)(row << 1U) + y) * state->source->strides[2] +
            (column << 1U);

        for (x = 0U; x < width; ++x) {
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors_u[index] == source_u[x] &&
                    decision->palette_colors_v[index] == source_v[x]) {
                    break;
                }
            }
            state->palette_map_uv[(size_t)y * width + x] = index;
        }
    }
    decision->mode = 0U;
    decision->angle_delta = 0;
    decision->alpha_u = 0;
    decision->alpha_v = 0;
    decision->palette_size = count;
    return 1;
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

static unsigned int tile_palette_color_context(const uint8_t *map,
                                                uint32_t stride,
                                                uint32_t row,
                                                uint32_t column,
                                                uint8_t colors,
                                                uint8_t order[8]) {
    static const int8_t contexts[9] = {
        -1, -1, 0, -1, -1, 4, 3, 2, 1
    };
    uint8_t scores[8] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    unsigned int index;
    unsigned int hash;

    for (index = 0U; index < colors; ++index) order[index] = (uint8_t)index;
    if (column != 0U) scores[map[(size_t)row * stride + column - 1U]] += 2U;
    if (row != 0U && column != 0U) {
        ++scores[map[(size_t)(row - 1U) * stride + column - 1U]];
    }
    if (row != 0U) scores[map[(size_t)(row - 1U) * stride + column]] += 2U;
    for (index = 0U; index < 3U; ++index) {
        unsigned int best = index;
        unsigned int candidate;

        for (candidate = index + 1U; candidate < colors; ++candidate) {
            if (scores[candidate] > scores[best]) best = candidate;
        }
        if (best != index) {
            uint8_t best_score = scores[best];
            uint8_t best_color = order[best];

            for (candidate = best; candidate > index; --candidate) {
                scores[candidate] = scores[candidate - 1U];
                order[candidate] = order[candidate - 1U];
            }
            scores[index] = best_score;
            order[index] = best_color;
        }
    }
    hash = scores[0] + 2U * scores[1] + 2U * scores[2];
    return (unsigned int)contexts[hash];
}

static uint64_t tile_palette_map_rate_cost(
    const AvifencAv1TileState *state,
    const uint8_t *map,
    uint32_t width,
    uint32_t height,
    uint8_t palette_size,
    unsigned int plane) {
    unsigned int first_width = tile_ceil_log2(palette_size);
    uint32_t minimum = (1U << first_width) - palette_size;
    uint64_t cost = (first_width -
        (map[0] < minimum ? 1U : 0U)) * 256U;
    uint32_t diagonal;

    for (diagonal = 1U; diagonal < width + height - 1U; ++diagonal) {
        uint32_t column = diagonal < width ? diagonal : width - 1U;
        uint32_t minimum = diagonal >= height
            ? diagonal - height + 1U : 0U;

        for (;;) {
            uint32_t token_row = diagonal - column;
            uint8_t order[8];
            unsigned int context = tile_palette_color_context(
                map, width, token_row, column, palette_size, order);
            unsigned int symbol;

            for (symbol = 0U; symbol < palette_size; ++symbol) {
                if (order[symbol] == map[(size_t)token_row * width + column]) {
                    break;
                }
            }
            cost += tile_symbol_cost(
                state->palette_cdfs.color[plane][palette_size - 2U][context],
                symbol);
            if (column == minimum) break;
            --column;
        }
    }
    return cost;
}

static uint64_t tile_palette_luma_rate_cost(
    const AvifencAv1TileState *state,
    uint32_t width_mi,
    uint32_t height_mi,
    const AvifencAv1LumaDecision *decision) {
    unsigned int block_context = tile_palette_block_context(
        width_mi, height_mi);
    unsigned int bits = 8U;
    uint64_t cost = tile_symbol_cost(
        state->palette_cdfs.y_mode[block_context][0], 1U);
    uint8_t index;

    cost += tile_symbol_cost(
        state->palette_cdfs.y_size[block_context],
        decision->palette_size - 2U);
    cost += (8U + 2U) * 256U;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors[index];
        uint32_t range;
        unsigned int range_bits;

        cost += bits * 256U;
        range = 255U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    return cost + tile_palette_map_rate_cost(
        state, state->palette_map_y, width_mi << 2U, height_mi << 2U,
        decision->palette_size, 0U);
}

static uint64_t tile_palette_chroma_rate_cost(
    const AvifencAv1TileState *state,
    uint32_t width_mi,
    uint32_t height_mi,
    const AvifencAv1ChromaDecision *decision) {
    unsigned int block_context = tile_palette_block_context(
        width_mi, height_mi);
    unsigned int bits = 8U;
    uint64_t cost = tile_symbol_cost(
        state->palette_cdfs.uv_mode[0], 1U);
    uint8_t index;

    cost += tile_symbol_cost(
        state->palette_cdfs.uv_size[block_context],
        decision->palette_size - 2U);
    cost += (8U + 2U + 1U + 8U * decision->palette_size) * 256U;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors_u[index];
        uint32_t range;
        unsigned int range_bits;

        cost += bits * 256U;
        range = 256U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    return cost + tile_palette_map_rate_cost(
        state, state->palette_map_uv, width_mi << 1U, height_mi << 1U,
        decision->palette_size, 1U);
}

static AvifencStatus tile_write_ns(AvifencAv1SymbolWriter *writer,
                                   uint32_t value,
                                   uint32_t symbols) {
    unsigned int width;
    uint32_t minimum;

    if (symbols <= 1U) return value == 0U ? AVIFENC_OK
                                          : AVIFENC_INVALID_ARGUMENT;
    if (value >= symbols) return AVIFENC_INVALID_ARGUMENT;
    width = tile_ceil_log2(symbols);
    minimum = (1U << width) - symbols;
    if (value < minimum) {
        return avifenc_av1_symbol_writer_literal(writer, value, width - 1U);
    }
    value += minimum;
    if (avifenc_av1_symbol_writer_literal(
            writer, value >> 1U, width - 1U) != AVIFENC_OK) {
        return writer->status;
    }
    return avifenc_av1_symbol_writer_literal(writer, value & 1U, 1U);
}

static AvifencStatus tile_write_palette_luma_colors(
    AvifencAv1TileState *state,
    const AvifencAv1LumaDecision *decision) {
    unsigned int bits = 8U;
    uint8_t index;
    AvifencStatus status = avifenc_av1_symbol_writer_literal(
        state->writer, decision->palette_colors[0], 8U);

    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_symbol_writer_literal(state->writer, 3U, 2U);
    if (status != AVIFENC_OK) return status;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors[index];
        uint32_t delta = value - decision->palette_colors[index - 1U] - 1U;
        uint32_t range;
        unsigned int range_bits;

        status = avifenc_av1_symbol_writer_literal(
            state->writer, delta, bits);
        if (status != AVIFENC_OK) return status;
        range = 255U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    return AVIFENC_OK;
}

static AvifencStatus tile_write_palette_chroma_colors(
    AvifencAv1TileState *state,
    const AvifencAv1ChromaDecision *decision) {
    unsigned int bits = 8U;
    uint8_t index;
    AvifencStatus status = avifenc_av1_symbol_writer_literal(
        state->writer, decision->palette_colors_u[0], 8U);

    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_symbol_writer_literal(state->writer, 3U, 2U);
    if (status != AVIFENC_OK) return status;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors_u[index];
        uint32_t delta = value - decision->palette_colors_u[index - 1U];
        uint32_t range;
        unsigned int range_bits;

        status = avifenc_av1_symbol_writer_literal(
            state->writer, delta, bits);
        if (status != AVIFENC_OK) return status;
        range = 256U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    status = avifenc_av1_symbol_writer_literal(state->writer, 0U, 1U);
    for (index = 0U; status == AVIFENC_OK && index < decision->palette_size;
         ++index) {
        status = avifenc_av1_symbol_writer_literal(
            state->writer, decision->palette_colors_v[index], 8U);
    }
    return status;
}

static AvifencStatus tile_write_palette_map(
    AvifencAv1TileState *state,
    const uint8_t *map,
    uint32_t width,
    uint32_t height,
    uint8_t palette_size,
    unsigned int plane) {
    uint32_t diagonal;
    AvifencStatus status = tile_write_ns(
        state->writer, map[0], palette_size);

    if (status != AVIFENC_OK) return status;
    for (diagonal = 1U; diagonal < width + height - 1U; ++diagonal) {
        uint32_t column = diagonal < width ? diagonal : width - 1U;
        uint32_t minimum = diagonal >= height
            ? diagonal - height + 1U : 0U;

        for (;;) {
            uint32_t token_row = diagonal - column;
            uint8_t order[8];
            unsigned int context = tile_palette_color_context(
                map, width, token_row, column, palette_size, order);
            unsigned int symbol;

            for (symbol = 0U; symbol < palette_size; ++symbol) {
                if (order[symbol] == map[(size_t)token_row * width + column]) {
                    break;
                }
            }
            status = avifenc_av1_symbol_writer_write(
                state->writer,
                state->palette_cdfs.color[plane][palette_size - 2U][context],
                palette_size, symbol);
            if (status != AVIFENC_OK) return status;
            if (column == minimum) break;
            --column;
        }
    }
    return AVIFENC_OK;
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
        palette_block_context = tile_palette_block_context(
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
        if (tile_palette_classify_luma(
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
            rate_cost += tile_palette_luma_rate_cost(
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
        if (tile_palette_classify_chroma(
                state, row, column, width_mi, height_mi, &decision)) {
            uint64_t distortion = 0U;
            uint64_t rate_cost = tile_symbol_cost(mode_cdf, 0U) +
                tile_palette_chroma_rate_cost(
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

typedef struct {
    uint8_t widths[64];
    uint8_t heights[64];
    uint8_t flags[64];
    uint8_t modes[64];
    uint8_t uv_modes[64];
    uint8_t palette_sizes_y[64];
    uint8_t palette_sizes_uv[64];
} AvifencAv1PartitionCheckpoint;

static void tile_partition_checkpoint(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t block_mi,
    AvifencAv1PartitionCheckpoint *checkpoint) {
    uint32_t pixels = block_mi << 2U;
    uint32_t y;
    uint32_t x;

    for (y = 0U; y < pixels; ++y) {
        avifdec_memory_copy(
            state->trial_reconstruction +
                (size_t)y * pixels * sizeof(uint16_t),
            state->reconstruction->planes[0] +
                ((size_t)(row << 2U) + y) *
                    state->reconstruction->strides[0] +
                (column << 2U),
            (size_t)pixels * sizeof(uint16_t));
    }
    for (y = 0U; y < block_mi; ++y) {
        for (x = 0U; x < block_mi; ++x) {
            size_t source_index = (size_t)(row + y) * state->mi_columns +
                column + x;
            size_t checkpoint_index = (size_t)y * block_mi + x;

            checkpoint->widths[checkpoint_index] =
                state->block_widths[source_index];
            checkpoint->heights[checkpoint_index] =
                state->block_heights[source_index];
            checkpoint->flags[checkpoint_index] =
                state->block_flags[source_index];
            checkpoint->modes[checkpoint_index] =
                state->y_modes[source_index];
            checkpoint->uv_modes[checkpoint_index] =
                state->uv_modes[source_index];
            checkpoint->palette_sizes_y[checkpoint_index] =
                state->palette_sizes_y[source_index];
            checkpoint->palette_sizes_uv[checkpoint_index] =
                state->palette_sizes_uv[source_index];
        }
    }
}

static void tile_partition_restore(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t block_mi,
    const AvifencAv1PartitionCheckpoint *checkpoint) {
    uint32_t pixels = block_mi << 2U;
    uint32_t y;
    uint32_t x;

    for (y = 0U; y < pixels; ++y) {
        avifdec_memory_copy(
            state->reconstruction->planes[0] +
                ((size_t)(row << 2U) + y) *
                    state->reconstruction->strides[0] +
                (column << 2U),
            state->trial_reconstruction +
                (size_t)y * pixels * sizeof(uint16_t),
            (size_t)pixels * sizeof(uint16_t));
    }
    for (y = 0U; y < block_mi; ++y) {
        for (x = 0U; x < block_mi; ++x) {
            size_t destination_index =
                (size_t)(row + y) * state->mi_columns + column + x;
            size_t checkpoint_index = (size_t)y * block_mi + x;

            state->block_widths[destination_index] =
                checkpoint->widths[checkpoint_index];
            state->block_heights[destination_index] =
                checkpoint->heights[checkpoint_index];
            state->block_flags[destination_index] =
                checkpoint->flags[checkpoint_index];
            state->y_modes[destination_index] =
                checkpoint->modes[checkpoint_index];
            state->uv_modes[destination_index] =
                checkpoint->uv_modes[checkpoint_index];
            state->palette_sizes_y[destination_index] =
                checkpoint->palette_sizes_y[checkpoint_index];
            state->palette_sizes_uv[destination_index] =
                checkpoint->palette_sizes_uv[checkpoint_index];
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

static AvifencStatus tile_trial_block_sized(
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

static AvifencStatus tile_trial_partition(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t block_mi,
    const uint16_t *cdf,
    unsigned int partition,
    uint64_t *score) {
    AvifencAv1PartitionCheckpoint checkpoint;
    uint32_t half = block_mi >> 1U;
    uint64_t block_score;
    AvifencStatus status = AVIFENC_OK;

    tile_partition_checkpoint(
        state, row, column, block_mi, &checkpoint);
    *score = tile_candidate_score(
        0U, tile_symbol_cost(cdf, partition), state->quantizer);
#define TILE_ADD_TRIAL(r, c, w, h)                                      \
    do {                                                                \
        status = tile_trial_block_sized(                                \
            state, (r), (c), (w), (h), &block_score);                   \
        if (status == AVIFENC_OK) {                                     \
            if (*score > UINT64_MAX - block_score) {                    \
                status = AVIFENC_OVERFLOW;                              \
            } else {                                                    \
                *score += block_score;                                  \
            }                                                           \
        }                                                               \
    } while (0)
    if (partition == 0U) {
        TILE_ADD_TRIAL(row, column, block_mi, block_mi);
    } else if (partition == 1U) {
        TILE_ADD_TRIAL(row, column, block_mi, half);
        if (status == AVIFENC_OK) {
            TILE_ADD_TRIAL(row + half, column, block_mi, half);
        }
    } else if (partition == 2U) {
        TILE_ADD_TRIAL(row, column, half, block_mi);
        if (status == AVIFENC_OK) {
            TILE_ADD_TRIAL(row, column + half, half, block_mi);
        }
    } else {
        TILE_ADD_TRIAL(row, column, half, half);
        if (status == AVIFENC_OK) {
            TILE_ADD_TRIAL(row, column + half, half, half);
        }
        if (status == AVIFENC_OK) {
            TILE_ADD_TRIAL(row + half, column, half, half);
        }
        if (status == AVIFENC_OK) {
            TILE_ADD_TRIAL(row + half, column + half, half, half);
        }
    }
#undef TILE_ADD_TRIAL
    tile_partition_restore(
        state, row, column, block_mi, &checkpoint);
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
        unsigned int palette_block_context = tile_palette_block_context(
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
                status = tile_write_palette_luma_colors(state, &y_decision);
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
                status = tile_write_palette_chroma_colors(
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
        status = tile_write_palette_map(
            state, state->palette_map_y, width, height,
            y_decision.palette_size, 0U);
        if (status != AVIFENC_OK) return status;
    }
    if (uv_decision.palette_size != 0U) {
        status = tile_write_palette_map(
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

static AvifencStatus tile_write_block_sized(AvifencAv1TileState *state,
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

static int tile_partition_use_none(const AvifencAv1TileState *state,
                                   uint32_t row,
                                   uint32_t column,
                                   uint32_t block_mi) {
    static const uint32_t variance_limit = 96U;
    static const uint32_t edge_limit = 24U;
    uint32_t pixels = block_mi << 2U;
    uint64_t sum = 0U;
    uint64_t sum_squared = 0U;
    uint32_t maximum_edge = 0U;
    uint32_t y;
    uint32_t x;
    uint64_t count = (uint64_t)pixels * pixels;
    uint64_t variance;
    uint8_t colors[8];
    uint8_t color_count = 0U;

    if (((uint64_t)column + block_mi) * 4U > state->source->width ||
        ((uint64_t)row + block_mi) * 4U > state->source->height) {
        return 0;
    }

    for (y = 0U; y < pixels; ++y) {
        const uint8_t *source_row = state->source->planes[0] +
            ((size_t)row << 2U) * state->source->strides[0] +
            (column << 2U) + (size_t)y * state->source->strides[0];

        for (x = 0U; x < pixels; ++x) {
            uint32_t sample = source_row[x];
            uint8_t color_index;

            sum += sample;
            sum_squared += (uint64_t)sample * sample;
            if (color_count <= 8U) {
                for (color_index = 0U; color_index < color_count;
                     ++color_index) {
                    if (colors[color_index] == sample) break;
                }
                if (color_index == color_count && color_count < 8U) {
                    colors[color_count++] = (uint8_t)sample;
                } else if (color_index == color_count) {
                    color_count = 9U;
                }
            }
            if (x != 0U) {
                uint32_t edge = sample > source_row[x - 1U]
                    ? sample - source_row[x - 1U]
                    : source_row[x - 1U] - sample;
                if (edge > maximum_edge) maximum_edge = edge;
            }
            if (y != 0U) {
                const uint8_t *above_row =
                    source_row - state->source->strides[0];
                uint32_t above = above_row[x];
                uint32_t edge = sample > above ? sample - above
                                               : above - sample;
                if (edge > maximum_edge) maximum_edge = edge;
            }
        }
    }
    if (color_count >= 2U && color_count <= 8U) return 1;
    variance = (sum_squared * count - sum * sum) / (count * count);
        return variance <= variance_limit + state->quantizer / 4U &&
            maximum_edge <= edge_limit;
}

static unsigned int tile_partition_rectangle(
    const AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t block_mi) {
    uint32_t pixels = block_mi << 2U;
    uint64_t horizontal_activity = 0U;
    uint64_t vertical_activity = 0U;
    uint32_t horizontal_transitions = 0U;
    uint32_t vertical_transitions = 0U;
    uint32_t y;
    uint32_t x;

    if (state->source->speed == AVIFENC_MAX_SPEED || block_mi < 4U ||
        block_mi > 8U ||
        ((uint64_t)column + block_mi) * 4U > state->source->width ||
        ((uint64_t)row + block_mi) * 4U > state->source->height) {
        return 0U;
    }
    for (y = 0U; y < pixels; ++y) {
        const uint8_t *source_row = state->source->planes[0] +
            ((size_t)row << 2U) * state->source->strides[0] +
            (column << 2U) + (size_t)y * state->source->strides[0];

        for (x = 0U; x < pixels; ++x) {
            if (x != 0U) {
                uint32_t edge = source_row[x] > source_row[x - 1U]
                    ? source_row[x] - source_row[x - 1U]
                    : source_row[x - 1U] - source_row[x];

                vertical_activity += edge;
                if (edge > 16U) ++vertical_transitions;
            }
            if (y != 0U) {
                const uint8_t *above_row =
                    source_row - state->source->strides[0];
                uint32_t edge = source_row[x] > above_row[x]
                    ? source_row[x] - above_row[x]
                    : above_row[x] - source_row[x];

                horizontal_activity += edge;
                if (edge > 16U) ++horizontal_transitions;
            }
        }
    }
    if (horizontal_transitions >= (3U * pixels) / 4U &&
        horizontal_transitions <= (5U * pixels) / 4U &&
        horizontal_activity > 2U * vertical_activity + pixels * pixels) {
        return 1U;
    }
    if (vertical_transitions >= (3U * pixels) / 4U &&
        vertical_transitions <= (5U * pixels) / 4U &&
        vertical_activity > 2U * horizontal_activity + pixels * pixels) {
        return 2U;
    }
    return 0U;
}

static AvifencStatus tile_select_partition(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t block_mi,
    const uint16_t *cdf,
    unsigned int *selected) {
    unsigned int rectangle = tile_partition_rectangle(
        state, row, column, block_mi);
    int use_none = tile_partition_use_none(
        state, row, column, block_mi);
    uint64_t best_score;
    uint64_t score;
    AvifencStatus status;

    if (state->source->speed == AVIFENC_MAX_SPEED) {
        *selected = use_none ? 0U : 3U;
        return AVIFENC_OK;
    }
    if (!use_none && rectangle == 0U) {
        *selected = 3U;
        return AVIFENC_OK;
    }
    status = tile_trial_partition(
        state, row, column, block_mi, cdf, 3U, &best_score);
    if (status != AVIFENC_OK) return status;
    *selected = 3U;
    if (use_none) {
        status = tile_trial_partition(
            state, row, column, block_mi, cdf, 0U, &score);
        if (status != AVIFENC_OK) return status;
        if (score < best_score) {
            best_score = score;
            *selected = 0U;
        }
    }
    if (rectangle != 0U) {
        status = tile_trial_partition(
            state, row, column, block_mi, cdf, rectangle, &score);
        if (status != AVIFENC_OK) return status;
        if (score < best_score) *selected = rectangle;
    }
    return AVIFENC_OK;
}

static uint32_t tile_partition_probability(const uint16_t *cdf,
                                           size_t symbol) {
    return cdf[symbol] - (symbol == 0U ? 0U : cdf[symbol - 1U]);
}

static AvifencStatus tile_write_partition(AvifencAv1TileState *state,
                                          uint32_t row,
                                          uint32_t column,
                                          uint32_t block_mi) {
    uint32_t half;
    int has_rows;
    int has_columns;
    unsigned int context = 0U;
    uint16_t *cdf;
    size_t symbols;
    unsigned int partition = 3U;
    AvifencStatus status;

    if (row >= state->mi_rows || column >= state->mi_columns) return AVIFENC_OK;
    if (state->source->statistics != 0) {
        ++state->source->statistics->partition_node_count;
    }
    if (block_mi == 1U) return tile_write_block(state, row, column);
    half = block_mi >> 1U;
    has_rows = row + half < state->mi_rows;
    has_columns = column + half < state->mi_columns;
    if (row != 0U &&
        state->block_widths[(size_t)(row - 1U) * state->mi_columns + column] <
            block_mi) {
        context |= 1U;
    }
    if (column != 0U &&
        state->block_heights[(size_t)row * state->mi_columns + column - 1U] <
            block_mi) {
        context |= 2U;
    }
    if (block_mi == 2U) {
        cdf = state->partition8[context];
        symbols = 4U;
    } else if (block_mi == 4U) {
        cdf = state->partition16[context];
        symbols = 10U;
    } else if (block_mi == 8U) {
        cdf = state->partition32[context];
        symbols = 10U;
    } else if (block_mi == 16U) {
        cdf = state->partition64[context];
        symbols = 10U;
    } else {
        return AVIFENC_UNSUPPORTED;
    }
    if (state->quantizer != 0U && has_rows && has_columns && block_mi <= 8U &&
        ((uint64_t)column + block_mi) * 4U <= state->source->width &&
        ((uint64_t)row + block_mi) * 4U <= state->source->height) {
        status = tile_select_partition(
            state, row, column, block_mi, cdf, &partition);
        if (status != AVIFENC_OK) return status;
    }
    if (partition == 0U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer, cdf, symbols, 0U);
        if (status != AVIFENC_OK) return status;
        return tile_write_block_sized(
            state, row, column, block_mi, block_mi);
    }
    if (partition == 1U || partition == 2U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer, cdf, symbols, partition);
        if (status != AVIFENC_OK) return status;
        if (partition == 1U) {
            status = tile_write_block_sized(
                state, row, column, block_mi, half);
            if (status == AVIFENC_OK) {
                status = tile_write_block_sized(
                    state, row + half, column, block_mi, half);
            }
        } else {
            status = tile_write_block_sized(
                state, row, column, half, block_mi);
            if (status == AVIFENC_OK) {
                status = tile_write_block_sized(
                    state, row, column + half, half, block_mi);
            }
        }
        return status;
    }
    if (has_rows || has_columns) {
        if (has_rows && has_columns) {
            status = avifenc_av1_symbol_writer_write(
                state->writer, cdf, symbols, 3U);
        } else {
            uint16_t binary_cdf[3];
            uint32_t split_probability;

            if (block_mi == 2U) return AVIFENC_LIMIT_EXCEEDED;
            if (has_columns) {
                split_probability =
                    tile_partition_probability(cdf, 2U) +
                    tile_partition_probability(cdf, 3U) +
                    tile_partition_probability(cdf, 4U) +
                    tile_partition_probability(cdf, 6U) +
                    tile_partition_probability(cdf, 7U) +
                    tile_partition_probability(cdf, 9U);
            } else {
                split_probability =
                    tile_partition_probability(cdf, 1U) +
                    tile_partition_probability(cdf, 3U) +
                    tile_partition_probability(cdf, 4U) +
                    tile_partition_probability(cdf, 5U) +
                    tile_partition_probability(cdf, 6U) +
                    tile_partition_probability(cdf, 8U);
            }
            binary_cdf[0] = (uint16_t)(32768U - split_probability);
            binary_cdf[1] = 32768U;
            binary_cdf[2] = 0U;
            status = avifenc_av1_symbol_writer_write(
                state->writer, binary_cdf, 2U, 1U);
        }
        if (status != AVIFENC_OK) return status;
    }
    status = tile_write_partition(state, row, column, half);
    if (status == AVIFENC_OK) {
        status = tile_write_partition(state, row, column + half, half);
    }
    if (status == AVIFENC_OK) {
        status = tile_write_partition(state, row + half, column, half);
    }
    if (status == AVIFENC_OK) {
        status = tile_write_partition(
            state, row + half, column + half, half);
    }
    return status;
}

AvifencStatus avifenc_av1_tile_write(
    AvifencAv1SymbolWriter *writer,
    const AvifencAv1TileSource *source,
    AvifencAv1TileReconstruction *reconstruction,
    void *workspace,
    size_t workspace_size) {
    AvifencAv1TileRequirements requirements;
    AvifencAv1TileState state;
    size_t cells;
    uint32_t row;
    uint32_t column;
    unsigned int plane;
    AvifencStatus status = tile_requirements(source, &requirements);

    if (status != AVIFENC_OK) return status;
    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (writer->disable_cdf_update == 0U || reconstruction == 0 ||
        workspace == 0 || workspace_size < requirements.workspace_required) {
        return workspace_size < requirements.workspace_required
            ? AVIFENC_OUT_OF_MEMORY : AVIFENC_INVALID_ARGUMENT;
    }
            if (source->statistics != 0) ++source->statistics->tile_count;
    for (plane = 0U; plane < 3U; ++plane) {
        if (reconstruction->planes[plane] == 0 ||
            reconstruction->strides[plane] <
                requirements.reconstruction_widths[plane] ||
            reconstruction->widths[plane] <
                requirements.reconstruction_widths[plane] ||
            reconstruction->heights[plane] <
                requirements.reconstruction_heights[plane]) {
            return AVIFENC_INVALID_ARGUMENT;
        }
    }
    state.writer = writer;
    state.source = source;
    state.reconstruction = reconstruction;
    state.quantizer = source->quantizer;
    state.mi_columns = 2U * ((source->width + 7U) >> 3U);
    state.mi_rows = 2U * ((source->height + 7U) >> 3U);
    cells = (size_t)state.mi_columns * state.mi_rows;
    state.block_widths = (uint8_t *)workspace;
    state.block_heights = state.block_widths + cells;
    state.block_flags = state.block_heights + cells;
    state.segment_ids = state.block_flags + cells;
    state.y_modes = state.segment_ids + cells;
    state.uv_modes = state.y_modes + cells;
    state.palette_sizes_y = state.uv_modes + cells;
    state.palette_sizes_uv = state.palette_sizes_y + cells;
    state.palette_map_y = state.palette_sizes_uv + cells;
    state.palette_map_uv = state.palette_map_y + 32U * 32U;
    state.trial_reconstruction = (uint8_t *)workspace +
        requirements.workspace_required - 32U * 32U * sizeof(uint16_t);
    avifdec_memory_fill(workspace, 0U, requirements.workspace_required);
    tile_cdfs_init(&state);
    status = avifenc_av1_transform_state_init(
        &state.transform, (uint8_t)source->quantizer,
        state.mi_columns, state.mi_rows,
        state.palette_map_uv + 16U * 16U,
        workspace_size - 8U * cells - 32U * 32U * sizeof(uint16_t) -
            32U * 32U - 16U * 16U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_transform_state_set_quantization(
        &state.transform, &source->quantization,
        (uint8_t)source->quantizer,
        state.transform.matrix_workspace,
        3U * AV1_QM_TOTAL_SIZE);
    if (status != AVIFENC_OK) return status;
    for (row = 0U; row < state.mi_rows; row += 16U) {
        for (column = 0U; column < state.mi_columns; column += 16U) {
            status = tile_write_partition(&state, row, column, 16U);
            if (status != AVIFENC_OK) return status;
        }
    }
    return avifenc_av1_symbol_writer_finish(writer);
}