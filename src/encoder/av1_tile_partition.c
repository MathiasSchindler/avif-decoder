#include "encoder/av1_tile_partition.h"
#include "encoder/av1_tile_intra.h"

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

typedef struct {
    uint8_t widths[64];
    uint8_t heights[64];
    uint8_t flags[64];
    uint8_t modes[64];
    uint8_t uv_modes[64];
    uint8_t palette_sizes_y[64];
    uint8_t palette_sizes_uv[64];
} AvifencAv1PartitionCheckpoint;

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

AvifencStatus avifenc_av1_tile_requirements(
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

void avifenc_av1_tile_cdfs_init(AvifencAv1TileState *state) {
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
        status = avifenc_av1_tile_trial_block_sized(                                \
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

AvifencStatus avifenc_av1_tile_write_partition(
    AvifencAv1TileState *state,
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
    if (block_mi == 1U) return avifenc_av1_tile_write_block(state, row, column);
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
        return avifenc_av1_tile_write_block_sized(
            state, row, column, block_mi, block_mi);
    }
    if (partition == 1U || partition == 2U) {
        status = avifenc_av1_symbol_writer_write(
            state->writer, cdf, symbols, partition);
        if (status != AVIFENC_OK) return status;
        if (partition == 1U) {
            status = avifenc_av1_tile_write_block_sized(
                state, row, column, block_mi, half);
            if (status == AVIFENC_OK) {
                status = avifenc_av1_tile_write_block_sized(
                    state, row + half, column, block_mi, half);
            }
        } else {
            status = avifenc_av1_tile_write_block_sized(
                state, row, column, half, block_mi);
            if (status == AVIFENC_OK) {
                status = avifenc_av1_tile_write_block_sized(
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
    status = avifenc_av1_tile_write_partition(state, row, column, half);
    if (status == AVIFENC_OK) {
        status = avifenc_av1_tile_write_partition(state, row, column + half, half);
    }
    if (status == AVIFENC_OK) {
        status = avifenc_av1_tile_write_partition(state, row + half, column, half);
    }
    if (status == AVIFENC_OK) {
        status = avifenc_av1_tile_write_partition(
            state, row + half, column + half, half);
    }
    return status;
}
