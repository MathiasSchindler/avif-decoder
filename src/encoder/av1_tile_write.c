#include "encoder/av1_tile_write.h"
#include "encoder/av1_transform_write.h"
#include "av1_intra.h"
#include "av1_predict.h"
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
    uint8_t *y_modes;
    uint8_t *trial_reconstruction;
    AvifencAv1TransformState transform;
    Av1IntraCdfs intra_cdfs;
    uint16_t partition8[4][5];
    uint16_t partition16[4][11];
    uint16_t partition32[4][11];
    uint16_t partition64[4][11];
    uint16_t skip[3][3];
} AvifencAv1TileState;

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
    if (source->quantizer == 0U) return AVIFENC_UNSUPPORTED;
    if (!tile_size_multiply(mi_columns, mi_rows, &cells) ||
        !tile_size_multiply(cells, 4U, &block_workspace) ||
        avifenc_av1_transform_context_size(
            mi_columns, mi_rows, &transform_workspace) != AVIFENC_OK ||
        !tile_size_add(
            block_workspace, transform_workspace, &workspace_required) ||
        !tile_size_add(workspace_required,
                       32U * 32U * sizeof(uint16_t),
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
    av1_intra_cdfs_init(&state->intra_cdfs);
}

static AvifencStatus tile_predict_plane_mode(AvifencAv1TileState *state,
                                             unsigned int plane,
                                             uint32_t x,
                                             uint32_t y,
                                             uint32_t width,
                                             uint32_t height,
                                             uint8_t have_above,
                                             uint8_t have_left,
                                             uint8_t mode) {
    Av1PreparedReferences prepared;
    AvifdecStatus status;

    status = av1_predict_prepare_references(
        state->reconstruction->planes[plane],
        state->reconstruction->strides[plane],
        state->reconstruction->widths[plane],
        state->reconstruction->heights[plane],
        state->reconstruction->widths[plane],
        state->reconstruction->heights[plane],
        x, y, width, height, 8U,
        have_above, have_left, 0U, 0U, &prepared);
    if (status != AVIFDEC_OK) return AVIFENC_LIMIT_EXCEEDED;
    if (mode == 0U) {
        status = av1_predict_dc(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            &prepared.references);
    } else if (mode == 1U || mode == 2U) {
        status = av1_predict_directional(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            mode == 1U ? 90U : 180U, &prepared.references);
    } else if (mode == 9U || mode == 12U) {
        status = av1_predict_nondirectional(
            state->reconstruction->planes[plane] +
                (size_t)y * state->reconstruction->strides[plane] + x,
            state->reconstruction->strides[plane], width, height, 8U,
            mode == 9U ? AV1_PREDICT_SMOOTH : AV1_PREDICT_PAETH,
            &prepared.references);
    } else {
        return AVIFENC_INVALID_ARGUMENT;
    }
    return status == AVIFDEC_OK ? AVIFENC_OK : AVIFENC_LIMIT_EXCEEDED;
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

static unsigned int tile_candidate_count(uint8_t speed) {
    static const unsigned int counts[3] = { 5U, 3U, 1U };

    return counts[speed];
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
            (uint8_t)(row != 0U), (uint8_t)(column != 0U), mode);

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
            (uint8_t)state->quantizer, 1, &block,
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
    unsigned int skip_context = 0U;
    AvifencStatus status = tile_select_luma_mode(
        state, row, column, y_mode_cdf, &y_mode);

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
    status = avifenc_av1_symbol_writer_write(
        state->writer, y_mode_cdf, 13U, y_mode);
    if (status != AVIFENC_OK) return status;
    state->block_flags[index] = 0U;
    state->y_modes[index] = y_mode;
    status = tile_predict_plane_mode(
        state, 0U, column << 2U, row << 2U,
        4U, 4U,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), y_mode);
    if (status != AVIFENC_OK) return status;
    if ((row & 1U) != 0U && (column & 1U) != 0U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer,
            state->intra_cdfs.uv_mode_cfl_allowed[y_mode], 14U, uv_mode);
        if (status != AVIFENC_OK) return status;
        status = tile_predict_plane_mode(
            state, 1U, (column >> 1U) << 2U, (row >> 1U) << 2U,
            4U, 4U,
            (uint8_t)(row > 1U), (uint8_t)(column > 1U), uv_mode);
        if (status != AVIFENC_OK) return status;
        status = tile_predict_plane_mode(
            state, 2U, (column >> 1U) << 2U, (row >> 1U) << 2U,
            4U, 4U,
            (uint8_t)(row > 1U), (uint8_t)(column > 1U), uv_mode);
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
        1, &transform_block);
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

static AvifencStatus tile_select_luma_mode_sized(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    const uint16_t *mode_cdf,
    uint8_t *selected_mode,
    uint64_t *selected_score) {
    static const uint8_t modes[5] = { 0U, 1U, 2U, 9U, 12U };
    uint32_t width = width_mi << 2U;
    uint32_t height = height_mi << 2U;
    Av1TxSize tx_size = tile_tx_size(width, height);
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
            state, 0U, column << 2U, row << 2U, width, height,
            (uint8_t)(row != 0U), (uint8_t)(column != 0U), mode);

        if (state->source->statistics != 0) {
            ++state->source->statistics->prediction_trial_count;
        }
        if (status != AVIFENC_OK) return status;
        if (tile_tx_writes_type(tx_size)) {
            avifdec_memory_copy(
                state->transform.tx_type_set2,
                state->intra_cdfs.tx_type_set2[
                    tile_tx_type_context(tx_size)][mode],
                sizeof(state->transform.tx_type_set2));
        }
        status = avifenc_av1_transform_trial(
            &state->transform, 0U, column, row, width, height, tx_size,
            state->source->planes[0], state->source->strides[0],
            state->source->width, state->source->height,
            state->reconstruction->planes[0],
            state->reconstruction->strides[0],
            (uint8_t)state->quantizer, tile_tx_writes_type(tx_size),
            &block, &distortion, &rate_cost);
        if (state->source->statistics != 0) {
            ++state->source->statistics->transform_trial_count;
        }
        if (status != AVIFENC_OK) return status;
        rate_cost += tile_symbol_cost(mode_cdf, mode);
        if (mode >= 1U && mode <= 8U) {
            rate_cost += tile_symbol_cost(
                state->intra_cdfs.angle_delta[mode - 1U],
                AV1_MAX_ANGLE_DELTA);
        }
        rate_cost += tile_symbol_cost(
            state->intra_cdfs.uv_mode_cfl_allowed[mode], 0U);
        score = tile_candidate_score(
            distortion, rate_cost, state->quantizer);
        if (score < best_score) {
            best_score = score;
            *selected_mode = mode;
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

typedef struct {
    uint8_t widths[64];
    uint8_t heights[64];
    uint8_t flags[64];
    uint8_t modes[64];
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
        }
    }
}

static AvifencStatus tile_trial_block_sized(
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
    uint8_t mode;
    uint64_t distortion;
    uint64_t rate_cost;
    AvifencStatus status = tile_select_luma_mode_sized(
        state, row, column, width_mi, height_mi, mode_cdf, &mode, score);

    if (status != AVIFENC_OK) return status;
    tile_store_block_state(
        state, row, column, width_mi, height_mi, mode);
    status = tile_predict_plane_mode(
        state, 0U, column << 2U, row << 2U, width, height,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), mode);
    if (state->source->statistics != 0) {
        ++state->source->statistics->prediction_trial_count;
    }
    if (status != AVIFENC_OK) return status;
    if (tile_tx_writes_type(tx_size)) {
        avifdec_memory_copy(
            state->transform.tx_type_set2,
            state->intra_cdfs.tx_type_set2[
                tile_tx_type_context(tx_size)][mode],
            sizeof(state->transform.tx_type_set2));
    }
    status = avifenc_av1_transform_trial(
        &state->transform, 0U, column, row, width, height, tx_size,
        state->source->planes[0], state->source->strides[0],
        state->source->width, state->source->height,
        state->reconstruction->planes[0],
        state->reconstruction->strides[0],
        (uint8_t)state->quantizer, tile_tx_writes_type(tx_size),
        &block, &distortion, &rate_cost);
    if (state->source->statistics != 0) {
        ++state->source->statistics->transform_trial_count;
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

static AvifencStatus tile_write_block_sized(AvifencAv1TileState *state,
                                            uint32_t row,
                                            uint32_t column,
                                            uint32_t width_mi,
                                            uint32_t height_mi) {
    AvifencAv1TransformBlock transform_block;
    size_t index = (size_t)row * state->mi_columns + column;
    uint16_t *y_mode_cdf = tile_y_mode_cdf(state, row, column);
    uint32_t width = width_mi << 2U;
    uint32_t height = height_mi << 2U;
    uint32_t chroma_width = width >> 1U;
    uint32_t chroma_height = height >> 1U;
    Av1TxSize luma_tx = tile_tx_size(width, height);
    Av1TxSize chroma_tx = tile_tx_size(chroma_width, chroma_height);
    uint8_t y_mode = 0U;
    uint8_t uv_mode = 0U;
    unsigned int skip_context = 0U;
    AvifencStatus status = tile_select_luma_mode_sized(
        state, row, column, width_mi, height_mi, y_mode_cdf, &y_mode, 0);

    if (status != AVIFENC_OK) return status;
    if (state->source->statistics != 0) {
        ++state->source->statistics->block_count;
    }
    if (row != 0U) {
        skip_context += state->block_flags[index - state->mi_columns] & 1U;
    }
    if (column != 0U) skip_context += state->block_flags[index - 1U] & 1U;
    status = avifenc_av1_symbol_writer_write(
        state->writer, state->skip[skip_context], 2U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_symbol_writer_write(
        state->writer, y_mode_cdf, 13U, y_mode);
    if (status != AVIFENC_OK) return status;
    if (y_mode >= 1U && y_mode <= 8U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer, state->intra_cdfs.angle_delta[y_mode - 1U],
            2U * AV1_MAX_ANGLE_DELTA + 1U, AV1_MAX_ANGLE_DELTA);
        if (status != AVIFENC_OK) return status;
    }
    status = avifenc_av1_symbol_writer_write(
        state->writer, state->intra_cdfs.uv_mode_cfl_allowed[y_mode],
        AV1_UV_INTRA_MODES_CFL_ALLOWED, uv_mode);
    if (status != AVIFENC_OK) return status;
    tile_store_block_state(
        state, row, column, width_mi, height_mi, y_mode);
    status = tile_predict_plane_mode(
        state, 0U, column << 2U, row << 2U, width, height,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), y_mode);
    if (status != AVIFENC_OK) return status;
    status = tile_predict_plane_mode(
        state, 1U, (column >> 1U) << 2U, (row >> 1U) << 2U,
        chroma_width, chroma_height,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), uv_mode);
    if (status != AVIFENC_OK) return status;
    status = tile_predict_plane_mode(
        state, 2U, (column >> 1U) << 2U, (row >> 1U) << 2U,
        chroma_width, chroma_height,
        (uint8_t)(row != 0U), (uint8_t)(column != 0U), uv_mode);
    if (status != AVIFENC_OK) return status;
    if (tile_tx_writes_type(luma_tx)) {
        avifdec_memory_copy(
            state->transform.tx_type_set2,
            state->intra_cdfs.tx_type_set2[
                tile_tx_type_context(luma_tx)][y_mode],
            sizeof(state->transform.tx_type_set2));
    }
    status = avifenc_av1_transform_encode(
        &state->transform, state->writer, 0U, column, row,
        width, height, luma_tx,
        state->source->planes[0], state->source->strides[0],
        state->source->width, state->source->height,
        state->reconstruction->planes[0],
        state->reconstruction->strides[0], (uint8_t)state->quantizer,
        tile_tx_writes_type(luma_tx), &transform_block);
    if (status != AVIFENC_OK) return status;
    if (state->source->statistics != 0) {
        ++state->source->statistics->transform_count;
    }
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
                (uint8_t)state->quantizer, 0, &transform_block);
            if (status != AVIFENC_OK) return status;
            if (state->source->statistics != 0) {
                ++state->source->statistics->transform_count;
            }
        }
    }
    return AVIFENC_OK;
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

            sum += sample;
            sum_squared += (uint64_t)sample * sample;
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
    if (has_rows && has_columns && block_mi <= 8U &&
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
    state.y_modes = state.block_flags + cells;
    state.trial_reconstruction = (uint8_t *)workspace +
        requirements.workspace_required - 32U * 32U * sizeof(uint16_t);
    avifdec_memory_fill(workspace, 0U, requirements.workspace_required);
    tile_cdfs_init(&state);
    status = avifenc_av1_transform_state_init(
        &state.transform, (uint8_t)source->quantizer,
        state.mi_columns, state.mi_rows, state.y_modes + cells,
        workspace_size - 4U * cells - 32U * 32U * sizeof(uint16_t));
    if (status != AVIFENC_OK) return status;
    for (row = 0U; row < state.mi_rows; row += 16U) {
        for (column = 0U; column < state.mi_columns; column += 16U) {
            status = tile_write_partition(&state, row, column, 16U);
            if (status != AVIFENC_OK) return status;
        }
    }
    return avifenc_av1_symbol_writer_finish(writer);
}