#include "av1_tile.h"
#include "av1_tile_internal.h"
#include "av1.h"
#include "av1_inter_predict.h"
#include "av1_predict.h"
#include "base.h"

AvifdecStatus av1_tile_read_skip(Av1SymbolDecoder *decoder,
                                 Av1TileCdfs *cdfs,
                       const Av1BlockState *state,
                                 const Av1BlockAvailability *availability,
                                 uint32_t row,
                                 uint32_t column,
                                 int forced_skip,
                                 uint8_t *skip) {
    unsigned int context = 0U;

    if (decoder == 0 || cdfs == 0 || state == 0 || availability == 0 || skip == 0 ||
        (forced_skip != 0 && forced_skip != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (forced_skip) {
        *skip = 1U;
        return AVIFDEC_OK;
    }
    if (availability->above) {
        const Av1BlockCell *above = av1_block_cell(state, row - 1U, column);
        if (above == 0) return AVIFDEC_LIMIT_EXCEEDED;
        if (above->skip > 1U) return AVIFDEC_INVALID_DATA;
        context += above->skip;
    }
    if (availability->left) {
        const Av1BlockCell *left = av1_block_cell(state, row, column - 1U);
        if (left == 0) return AVIFDEC_LIMIT_EXCEEDED;
        if (left->skip > 1U) return AVIFDEC_INVALID_DATA;
        context += left->skip;
    }
    *skip = (uint8_t)av1_symbol_read(decoder, cdfs->skip[context], 2U);
    return decoder->status;
}

static AvifdecStatus av1_tile_read_skip_mode(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1BlockState *state,
    const Av1BlockAvailability *availability,
    uint32_t row,
    uint32_t column,
    int skip_mode_present,
    uint8_t *skip_mode) {
    unsigned int context = 0U;

    if (decoder == 0 || cdfs == 0 || state == 0 || availability == 0 ||
        skip_mode == 0 ||
        (skip_mode_present != 0 && skip_mode_present != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!skip_mode_present) {
        *skip_mode = 0U;
        return AVIFDEC_OK;
    }
    if (availability->above) {
        const Av1BlockCell *above = av1_block_cell(state, row - 1U, column);
        if (above == 0) return AVIFDEC_LIMIT_EXCEEDED;
        context += above->skip_mode;
    }
    if (availability->left) {
        const Av1BlockCell *left = av1_block_cell(state, row, column - 1U);
        if (left == 0) return AVIFDEC_LIMIT_EXCEEDED;
        context += left->skip_mode;
    }
    if (context >= 3U) return AVIFDEC_INVALID_DATA;
    *skip_mode = (uint8_t)av1_symbol_read(
        decoder, cdfs->inter.skip_mode[context], 2U);
    return decoder->status;
}

static uint8_t av1_neg_deinterleave(uint8_t difference,
                                    uint8_t reference,
                                    uint8_t maximum) {
    if (reference == 0U) return difference;
    if (reference >= maximum - 1U) return (uint8_t)(maximum - difference - 1U);
    if (2U * reference < maximum) {
        if (difference <= 2U * reference) {
            return (difference & 1U) != 0U
                   ? (uint8_t)(reference + ((difference + 1U) >> 1))
                   : (uint8_t)(reference - (difference >> 1));
        }
        return difference;
    }
    if (difference <= 2U * (maximum - reference - 1U)) {
        return (difference & 1U) != 0U
               ? (uint8_t)(reference + ((difference + 1U) >> 1))
               : (uint8_t)(reference - (difference >> 1));
    }
    return (uint8_t)(maximum - (difference + 1U));
}

AvifdecStatus av1_tile_read_segment_id(Av1SymbolDecoder *decoder,
                                       Av1TileCdfs *cdfs,
                                       const Av1BlockState *state,
                                       const Av1BlockAvailability *availability,
                                       uint32_t row,
                                       uint32_t column,
                                       uint8_t last_active_segment,
                                       int skip,
                                       uint8_t *segment_id) {
    int prev_upper_left = -1;
    int prev_upper = -1;
    int prev_left = -1;
    uint8_t prediction;
    unsigned int context;
    uint8_t difference;

    if (decoder == 0 || cdfs == 0 || state == 0 || availability == 0 ||
        segment_id == 0 || last_active_segment >= 8U || (skip != 0 && skip != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (availability->above && availability->left) {
        const Av1BlockCell *cell = av1_block_cell(state, row - 1U, column - 1U);
        if (cell == 0) return AVIFDEC_LIMIT_EXCEEDED;
        prev_upper_left = cell->segment_id;
    }
    if (availability->above) {
        const Av1BlockCell *cell = av1_block_cell(state, row - 1U, column);
        if (cell == 0) return AVIFDEC_LIMIT_EXCEEDED;
        prev_upper = cell->segment_id;
    }
    if (availability->left) {
        const Av1BlockCell *cell = av1_block_cell(state, row, column - 1U);
        if (cell == 0) return AVIFDEC_LIMIT_EXCEEDED;
        prev_left = cell->segment_id;
    }
    if (prev_upper < 0) prediction = prev_left < 0 ? 0U : (uint8_t)prev_left;
    else if (prev_left < 0) prediction = (uint8_t)prev_upper;
    else prediction = prev_upper_left == prev_upper
                      ? (uint8_t)prev_upper : (uint8_t)prev_left;
    if (skip) {
        *segment_id = prediction;
        return AVIFDEC_OK;
    }
    if (last_active_segment == 0U) {
        *segment_id = 0U;
        return AVIFDEC_OK;
    }
    if (prev_upper_left < 0) context = 0U;
    else if (prev_upper_left == prev_upper && prev_upper_left == prev_left) context = 2U;
    else if (prev_upper_left == prev_upper || prev_upper_left == prev_left ||
             prev_upper == prev_left) context = 1U;
    else context = 0U;
    difference = (uint8_t)av1_symbol_read(
        decoder, cdfs->segment_id[context], 8U);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    *segment_id = av1_neg_deinterleave(difference, prediction,
                                       (uint8_t)(last_active_segment + 1U));
    return *segment_id <= last_active_segment ? AVIFDEC_OK : AVIFDEC_INVALID_DATA;
}

AvifdecStatus av1_tile_read_delta(Av1SymbolDecoder *decoder,
                                  uint16_t cdf[5],
                                  int32_t *delta) {
    uint32_t absolute;

    if (decoder == 0 || cdf == 0 || delta == 0) return AVIFDEC_INVALID_ARGUMENT;
    absolute = av1_symbol_read(decoder, cdf, 4U);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    if (absolute == 3U) {
        unsigned int bits = av1_symbol_read_literal(decoder, 3U) + 1U;
        uint32_t remainder = av1_symbol_read_literal(decoder, bits);

        if (decoder->status != AVIFDEC_OK) return decoder->status;
        absolute = (1U << bits) + remainder + 1U;
    }
    if (absolute != 0U && av1_symbol_read_literal(decoder, 1U)) {
        *delta = -(int32_t)absolute;
    } else {
        *delta = (int32_t)absolute;
    }
    return decoder->status;
}

AvifdecStatus av1_tile_read_y_mode(Av1SymbolDecoder *decoder,
                                   Av1TileCdfs *cdfs,
                                   uint32_t width,
                                   uint32_t height,
                                   uint8_t *y_mode) {
    uint32_t minimum;
    unsigned int group;

    if (decoder == 0 || cdfs == 0 || y_mode == 0 || width == 0U || height == 0U ||
        width > 32U || height > 32U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    minimum = width < height ? width : height;
    if (minimum <= 1U) group = 0U;
    else if (minimum <= 2U) group = 1U;
    else if (minimum <= 4U) group = 2U;
    else group = 3U;
    *y_mode = (uint8_t)av1_symbol_read(decoder, cdfs->y_mode[group], 13U);
    return decoder->status;
}

AvifdecStatus av1_tile_read_intra_frame_y_mode(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1BlockState *state,
    const Av1BlockAvailability *availability,
    uint32_t row,
    uint32_t column,
    uint8_t *y_mode) {
    static const uint8_t mode_context[AV1_INTRA_MODES] = {
        0U, 1U, 2U, 3U, 4U, 4U, 4U, 4U, 3U, 0U, 1U, 2U, 0U
    };
    unsigned int above_context = 0U;
    unsigned int left_context = 0U;

    if (decoder == 0 || cdfs == 0 || state == 0 || availability == 0 ||
        y_mode == 0 || !av1_block_state_is_inside(state, row, column)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (availability->above) {
        const Av1BlockCell *above = av1_block_cell(state, row - 1U, column);

        if (above == 0) return AVIFDEC_LIMIT_EXCEEDED;
        if (above->y_mode >= AV1_INTRA_MODES) return AVIFDEC_INVALID_DATA;
        above_context = mode_context[above->y_mode];
    }
    if (availability->left) {
        const Av1BlockCell *left = av1_block_cell(state, row, column - 1U);

        if (left == 0) return AVIFDEC_LIMIT_EXCEEDED;
        if (left->y_mode >= AV1_INTRA_MODES) return AVIFDEC_INVALID_DATA;
        left_context = mode_context[left->y_mode];
    }
    *y_mode = (uint8_t)av1_symbol_read(
        decoder, cdfs->intra.y_mode[above_context][left_context],
        AV1_INTRA_MODES);
    return decoder->status;
}

AvifdecStatus av1_tile_read_angle_delta(Av1SymbolDecoder *decoder,
                                        Av1TileCdfs *cdfs,
                                        uint8_t mode,
                                        int8_t *angle_delta) {
    uint32_t symbol;

    if (decoder == 0 || cdfs == 0 || angle_delta == 0 || mode < 1U || mode > 8U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    symbol = av1_symbol_read(decoder, cdfs->angle_delta[mode - 1U], 7U);
    *angle_delta = (int8_t)((int32_t)symbol - 3);
    return decoder->status;
}

static AvifdecStatus av1_tile_read_intra_angle_delta(Av1SymbolDecoder *decoder,
                                                      Av1TileCdfs *cdfs,
                                                      uint8_t mode,
                                                      int8_t *angle_delta) {
    uint32_t symbol;

    if (decoder == 0 || cdfs == 0 || angle_delta == 0 || mode < 1U || mode > 8U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    symbol = av1_symbol_read(decoder, cdfs->intra.angle_delta[mode - 1U], 7U);
    *angle_delta = (int8_t)((int32_t)symbol - 3);
    return decoder->status;
}

AvifdecStatus av1_tile_read_tx_depth(Av1SymbolDecoder *decoder,
                                     Av1TileCdfs *cdfs,
                                     unsigned int maximum_tx_log2,
                                     unsigned int maximum_depth,
                                     unsigned int context,
                                     uint8_t *tx_depth) {
    uint16_t *cdf;
    size_t symbols;

    if (decoder == 0 || cdfs == 0 || tx_depth == 0 || maximum_tx_log2 == 0U ||
        maximum_tx_log2 > 4U || maximum_depth > 4U || context >= 3U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (maximum_depth == 0U) {
        *tx_depth = 0U;
        return AVIFDEC_OK;
    }
    if (maximum_tx_log2 == 1U) cdf = cdfs->tx8[context];
    else if (maximum_tx_log2 == 2U) cdf = cdfs->tx16[context];
    else if (maximum_tx_log2 == 3U) cdf = cdfs->tx32[context];
    else cdf = cdfs->tx64[context];
    symbols = maximum_depth == 1U ? 2U : 3U;
    *tx_depth = (uint8_t)av1_symbol_read(decoder, cdf, symbols);
    return decoder->status;
}

typedef struct {
    Av1PartitionEntropy entropy;
    Av1PartitionBlock decode_block;
    Av1TileDecodeBlock decode_mode_block;
    Av1TileCdfs *cdfs;
    void *user_data;
} Av1TilePartitionContext;

static AvifdecStatus av1_tile_read_partition(void *user_data,
                                              uint32_t row,
                                              uint32_t column,
                                              uint32_t block_mi,
                                              unsigned int context,
                                              int has_rows,
                                              int has_columns,
                                              Av1Partition *partition) {
    Av1TilePartitionContext *tile = (Av1TilePartitionContext *)user_data;

    return av1_partition_read_entropy(&tile->entropy, row, column, block_mi,
                                      context, has_rows, has_columns, partition);
}

static AvifdecStatus av1_tile_decode_block(void *user_data,
                                            uint32_t row,
                                            uint32_t column,
                                            uint32_t width,
                                            uint32_t height) {
    Av1TilePartitionContext *tile = (Av1TilePartitionContext *)user_data;

    if (tile->decode_mode_block != 0) {
        return tile->decode_mode_block(tile->user_data, tile->entropy.decoder,
                                       tile->cdfs, row, column, width, height);
    }
    if (tile->decode_block == 0) return AVIFDEC_OK;
    return tile->decode_block(tile->user_data, row, column, width, height);
}

static void av1_tile_partition_hash(Av1PartitionTrace *trace, uint64_t value) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        trace->checksum ^= (value >> (index * 8U)) & 0xffU;
        trace->checksum *= (uint64_t)1099511628211ULL;
    }
}

AvifdecStatus av1_tile_decode_partitions(const Av1TilePartitionConfig *config,
                                         const Av1TileCdfs *frame_cdfs,
                                         Av1TileCdfs *tile_cdfs,
                                         Av1PartitionTrace *trace) {
    Av1SymbolDecoder decoder;
    Av1TilePartitionContext context;
    Av1PartitionGrid grid;
    uint32_t row;
    uint32_t column;
    AvifdecStatus status;

    if (config == 0 || frame_cdfs == 0 || tile_cdfs == 0 || trace == 0 ||
        config->spans == 0 || config->span_count == 0U || config->size == 0U ||
        config->block_widths == 0 || config->block_heights == 0 ||
        (config->superblock_mi != 16U && config->superblock_mi != 32U) ||
        config->tile_row_start >= config->tile_row_end ||
        config->tile_row_end > config->mi_rows ||
        config->tile_column_start >= config->tile_column_end ||
        config->tile_column_end > config->mi_columns ||
        config->max_partition_nodes == 0U || config->disable_cdf_update > 1U ||
        (config->decode_block != 0 && config->decode_mode_block != 0)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    avifdec_memory_copy(tile_cdfs, frame_cdfs, sizeof(*tile_cdfs));
    status = av1_symbol_init(&decoder, config->spans, config->span_count,
                             config->start, config->size,
                             config->disable_cdf_update);
    if (status != AVIFDEC_OK) return status;
    context.entropy.decoder = &decoder;
    context.entropy.cdfs = &tile_cdfs->partition;
    context.decode_block = config->decode_block;
    context.decode_mode_block = config->decode_mode_block;
    context.cdfs = tile_cdfs;
    context.user_data = config->user_data;
    avifdec_memory_fill(&grid, 0U, sizeof(grid));
    grid.mi_rows = config->mi_rows;
    grid.mi_columns = config->mi_columns;
    grid.tile_row_start = config->tile_row_start;
    grid.tile_row_end = config->tile_row_end;
    grid.tile_column_start = config->tile_column_start;
    grid.tile_column_end = config->tile_column_end;
    grid.block_widths = config->block_widths;
    grid.block_heights = config->block_heights;
    grid.grid_capacity = config->grid_capacity;
    grid.max_partition_nodes = config->max_partition_nodes;
    grid.read_partition = av1_tile_read_partition;
    grid.decode_block = av1_tile_decode_block;
    grid.user_data = &context;
    trace->partition_nodes = 0U;
    trace->block_count = 0U;
    trace->max_depth = 0U;
    trace->checksum = (uint64_t)1469598103934665603ULL;
    for (row = config->tile_row_start; row < config->tile_row_end;
         row += config->superblock_mi) {
        for (column = config->tile_column_start; column < config->tile_column_end;
             column += config->superblock_mi) {
            Av1PartitionTrace superblock_trace;

            if (config->before_superblock != 0) {
                status = config->before_superblock(
                    config->before_superblock_user_data, &decoder, tile_cdfs,
                    row, column, config->superblock_mi);
                if (status != AVIFDEC_OK) return status;
            }
            status = av1_partition_superblock(&grid, row, column,
                                              config->superblock_mi,
                                              &superblock_trace);
            if (status != AVIFDEC_OK) return status;
            if (trace->partition_nodes > SIZE_MAX - superblock_trace.partition_nodes ||
                trace->block_count > SIZE_MAX - superblock_trace.block_count) {
                return AVIFDEC_OVERFLOW;
            }
            trace->partition_nodes += superblock_trace.partition_nodes;
            trace->block_count += superblock_trace.block_count;
            if (superblock_trace.max_depth > trace->max_depth) {
                trace->max_depth = superblock_trace.max_depth;
            }
            av1_tile_partition_hash(trace, row);
            av1_tile_partition_hash(trace, column);
            av1_tile_partition_hash(trace, superblock_trace.checksum);
        }
    }
    return av1_symbol_exit(&decoder);
}

static const uint8_t av1_block_width_mi[AV1_BLOCK_SIZES] = {
    1U, 1U, 2U, 2U, 2U, 4U, 4U, 4U, 8U, 8U, 8U,
    16U, 16U, 16U, 32U, 32U, 1U, 4U, 2U, 8U, 4U, 16U
};

static const uint8_t av1_block_height_mi[AV1_BLOCK_SIZES] = {
    1U, 2U, 1U, 2U, 4U, 2U, 4U, 8U, 4U, 8U, 16U,
    8U, 16U, 32U, 16U, 32U, 4U, 1U, 8U, 2U, 16U, 4U
};

static const uint8_t av1_max_tx_size[AV1_BLOCK_SIZES] = {
    0U, 5U, 6U, 1U, 7U, 8U, 2U, 9U, 10U, 3U, 11U,
    12U, 4U, 4U, 4U, 4U, 13U, 14U, 15U, 16U, 17U, 18U
};

static const uint8_t av1_max_tx_depth[AV1_BLOCK_SIZES] = {
    0U, 1U, 1U, 1U, 2U, 2U, 2U, 3U, 3U, 3U, 4U,
    4U, 4U, 4U, 4U, 4U, 2U, 2U, 3U, 3U, 4U, 4U
};

static const uint8_t av1_split_tx_size[19] = {
    0U, 0U, 1U, 2U, 3U, 0U, 0U, 1U, 1U, 2U,
    2U, 3U, 3U, 5U, 6U, 7U, 8U, 9U, 10U
};

static const uint8_t av1_tx_width[19] = {
    4U, 8U, 16U, 32U, 64U, 4U, 8U, 8U, 16U, 16U,
    32U, 32U, 64U, 4U, 16U, 8U, 32U, 16U, 64U
};

static const uint8_t av1_tx_height[19] = {
    4U, 8U, 16U, 32U, 64U, 8U, 4U, 16U, 8U, 32U,
    16U, 64U, 32U, 16U, 4U, 32U, 8U, 64U, 16U
};

static AvifdecStatus av1_tile_find_block_size(uint32_t width,
                                               uint32_t height,
                                               uint8_t *block_size) {
    unsigned int index;

    if (block_size == 0) return AVIFDEC_INVALID_ARGUMENT;
    for (index = 0U; index < AV1_BLOCK_SIZES; ++index) {
        if (av1_block_width_mi[index] == width &&
            av1_block_height_mi[index] == height) {
            *block_size = (uint8_t)index;
            return AVIFDEC_OK;
        }
    }
    return AVIFDEC_INVALID_DATA;
}

static AvifdecStatus av1_tile_read_cfl(Av1SymbolDecoder *decoder,
                                       Av1TileCdfs *cdfs,
                                       int8_t *alpha_u,
                                       int8_t *alpha_v) {
    uint32_t signs;
    uint32_t sign_u;
    uint32_t sign_v;

    signs = av1_symbol_read(decoder, cdfs->intra.cfl_sign,
                            AV1_CFL_JOINT_SIGNS);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    sign_u = (signs + 1U) / 3U;
    sign_v = (signs + 1U) % 3U;
    *alpha_u = 0;
    *alpha_v = 0;
    if (sign_u != 0U) {
        uint32_t alpha = av1_symbol_read(decoder,
                                         cdfs->intra.cfl_alpha[signs - 2U],
                                         AV1_CFL_ALPHABET_SIZE) + 1U;

        if (decoder->status != AVIFDEC_OK) return decoder->status;
        *alpha_u = sign_u == 1U ? -(int8_t)alpha : (int8_t)alpha;
    }
    if (sign_v != 0U) {
        unsigned int context = (sign_v - 1U) * 3U + sign_u;
        uint32_t alpha = av1_symbol_read(decoder,
                                         cdfs->intra.cfl_alpha[context],
                                         AV1_CFL_ALPHABET_SIZE) + 1U;

        if (decoder->status != AVIFDEC_OK) return decoder->status;
        *alpha_v = sign_v == 1U ? -(int8_t)alpha : (int8_t)alpha;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_tile_read_tx_size(Av1SymbolDecoder *decoder,
                                            Av1TileCdfs *cdfs,
                                            const Av1TileModeConfig *config,
                                            const Av1BlockAvailability *availability,
                                            Av1BlockTraceFields *fields,
                                            uint8_t block_size) {
    uint8_t tx_size;

    if (fields->lossless) {
        fields->tx_size = 0U;
        return AVIFDEC_OK;
    }
    tx_size = av1_max_tx_size[block_size];
    if (fields->is_inter) {
        fields->tx_size = tx_size;
        return AVIFDEC_OK;
    }
    if (!fields->skip && block_size != 0U && config->tx_mode == 2U) {
        unsigned int context = 0U;
        uint8_t depth;

        if (availability->above) {
            const Av1BlockCell *above = av1_block_cell(
                config->block_state, fields->row - 1U, fields->column);
            if (above == 0 || above->tx_size >= 19U) return AVIFDEC_INVALID_DATA;
            if ((above->is_inter ? (uint32_t)above->width * 4U
                                 : av1_tx_width[above->tx_size]) >=
                av1_tx_width[tx_size]) {
                ++context;
            }
        }
        if (availability->left) {
            const Av1BlockCell *left = av1_block_cell(
                config->block_state, fields->row, fields->column - 1U);
            if (left == 0 || left->tx_size >= 19U) return AVIFDEC_INVALID_DATA;
            if ((left->is_inter ? (uint32_t)left->height * 4U
                                : av1_tx_height[left->tx_size]) >=
                av1_tx_height[tx_size]) {
                ++context;
            }
        }
        unsigned int maximum_tx_log2 =
            av1_tx_size_info[tx_size].width_log2 >
                av1_tx_size_info[tx_size].height_log2
            ? av1_tx_size_info[tx_size].width_log2 - 2U
            : av1_tx_size_info[tx_size].height_log2 - 2U;

        if (av1_tile_read_tx_depth(decoder, cdfs,
                       maximum_tx_log2,
                                   av1_max_tx_depth[block_size], context,
                                   &depth) != AVIFDEC_OK) {
            return decoder->status;
        }
        while (depth-- != 0U) tx_size = av1_split_tx_size[tx_size];
    }
    fields->tx_size = tx_size;
    return AVIFDEC_OK;
}

typedef struct {
    Av1TileModeConfig config;
    int32_t current_q_index;
    int32_t delta_lf[4];
    uint8_t read_deltas;
} Av1TileModeContext;

enum {
    AV1_SEG_LVL_ALT_Q = 0,
    AV1_SEG_LVL_REF_FRAME = 5,
    AV1_SEG_LVL_SKIP = 6,
    AV1_SEG_LVL_GLOBALMV = 7
};

static unsigned int av1_tile_is_inter_context(
    const Av1BlockState *state,
    const Av1BlockAvailability *availability,
    uint32_t row,
    uint32_t column) {
    int above_intra = 0;
    int left_intra = 0;

    if (availability->above) {
        const Av1BlockCell *above = av1_block_cell(state, row - 1U, column);
        above_intra = above != 0 && !above->is_inter;
    }
    if (availability->left) {
        const Av1BlockCell *left = av1_block_cell(state, row, column - 1U);
        left_intra = left != 0 && !left->is_inter;
    }
    if (availability->above && availability->left) {
        return above_intra && left_intra ? 3U
             : above_intra || left_intra ? 1U : 0U;
    }
    if (availability->above) return above_intra ? 2U : 0U;
    if (availability->left) return left_intra ? 2U : 0U;
    return 0U;
}

static AvifdecStatus av1_tile_read_is_inter(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields) {
    size_t feature_offset = (size_t)fields->segment_id * 8U;

    if (fields->skip_mode) {
        fields->is_inter = 1U;
    } else if (config->segmentation_enabled &&
               config->feature_enabled[feature_offset +
                                       AV1_SEG_LVL_REF_FRAME]) {
        fields->is_inter = config->feature_data[
            feature_offset + AV1_SEG_LVL_REF_FRAME] != 0;
    } else if (config->segmentation_enabled &&
               config->feature_enabled[feature_offset +
                                       AV1_SEG_LVL_GLOBALMV]) {
        fields->is_inter = 1U;
    } else {
        unsigned int context = av1_tile_is_inter_context(
            config->block_state, availability, fields->row, fields->column);

        fields->is_inter = (uint8_t)av1_symbol_read(
            decoder, cdfs->inter.is_inter[context], 2U);
    }
    return decoder->status;
}

static void av1_tile_neighbor_ref_counts(
    const Av1BlockState *state,
    const Av1BlockAvailability *availability,
    uint32_t row,
    uint32_t column,
    uint8_t counts[8]) {
    const Av1BlockCell *neighbors[2];
    unsigned int neighbor_count = 0U;
    unsigned int neighbor;

    avifdec_memory_fill(counts, 0U, 8U);
    if (availability->above) {
        neighbors[neighbor_count++] = av1_block_cell(state, row - 1U, column);
    }
    if (availability->left) {
        neighbors[neighbor_count++] = av1_block_cell(state, row, column - 1U);
    }
    for (neighbor = 0U; neighbor < neighbor_count; ++neighbor) {
        unsigned int list;

        if (neighbors[neighbor] == 0 || !neighbors[neighbor]->is_inter) continue;
        for (list = 0U; list < 2U; ++list) {
            uint8_t reference = neighbors[neighbor]->ref_frame[list];

            if (reference > 0U && reference < 8U &&
                counts[reference] != UINT8_MAX) {
                ++counts[reference];
            }
        }
    }
}

static unsigned int av1_tile_count_context(unsigned int first,
                                            unsigned int second) {
    return first == second ? 1U : first < second ? 0U : 2U;
}

static unsigned int av1_tile_compound_context(
    const Av1BlockState *state,
    const Av1BlockAvailability *availability,
    uint32_t row,
    uint32_t column) {
    const Av1BlockCell *above = availability->above
        ? av1_block_cell(state, row - 1U, column) : 0;
    const Av1BlockCell *left = availability->left
        ? av1_block_cell(state, row, column - 1U) : 0;
    int above_compound = above != 0 && above->is_inter &&
                         above->ref_frame[1] > 0U;
    int left_compound = left != 0 && left->is_inter &&
                        left->ref_frame[1] > 0U;

    if (above != 0 && left != 0) {
        if (!above_compound && !left_compound) {
            int above_backward = above->ref_frame[0] >= 5U;
            int left_backward = left->ref_frame[0] >= 5U;
            return (unsigned int)(above_backward ^ left_backward);
        }
        if (!above_compound) {
            return 2U + (unsigned int)(above->ref_frame[0] >= 5U ||
                                      !above->is_inter);
        }
        if (!left_compound) {
            return 2U + (unsigned int)(left->ref_frame[0] >= 5U ||
                                      !left->is_inter);
        }
        return 4U;
    }
    if (above != 0 || left != 0) {
        const Av1BlockCell *edge = above != 0 ? above : left;
        int compound = edge->is_inter && edge->ref_frame[1] > 0U;

        return compound ? 3U : (unsigned int)(edge->ref_frame[0] >= 5U);
    }
    return 1U;
}

static int av1_tile_has_unidirectional_references(const Av1BlockCell *cell) {
    return cell->ref_frame[1] > 0U &&
        ((cell->ref_frame[0] <= 4U && cell->ref_frame[1] <= 4U) ||
         (cell->ref_frame[0] >= 5U && cell->ref_frame[1] >= 5U));
}

static unsigned int av1_tile_compound_reference_type_context(
    const Av1BlockState *state,
    const Av1BlockAvailability *availability,
    uint32_t row,
    uint32_t column) {
    const Av1BlockCell *above = availability->above
        ? av1_block_cell(state, row - 1U, column) : 0;
    const Av1BlockCell *left = availability->left
        ? av1_block_cell(state, row, column - 1U) : 0;

    if (above != 0 && left != 0) {
        int above_intra = !above->is_inter;
        int left_intra = !left->is_inter;

        if (above_intra && left_intra) return 2U;
        if (above_intra || left_intra) {
            const Av1BlockCell *inter = above_intra ? left : above;

            return inter->ref_frame[1] == 0U ? 2U
                : 1U + 2U *
                    (unsigned int)av1_tile_has_unidirectional_references(inter);
        }
        if (above->ref_frame[1] == 0U && left->ref_frame[1] == 0U) {
            return 1U + 2U * (unsigned int)
                (!((above->ref_frame[0] >= 5U) ^
                   (left->ref_frame[0] >= 5U)));
        }
        if (above->ref_frame[1] == 0U || left->ref_frame[1] == 0U) {
            const Av1BlockCell *compound = above->ref_frame[1] == 0U
                ? left : above;

            if (!av1_tile_has_unidirectional_references(compound)) return 1U;
            return 3U + (unsigned int)
                (!((above->ref_frame[0] >= 5U) ^
                   (left->ref_frame[0] >= 5U)));
        }
        if (!av1_tile_has_unidirectional_references(above) &&
            !av1_tile_has_unidirectional_references(left)) {
            return 0U;
        }
        if (!av1_tile_has_unidirectional_references(above) ||
            !av1_tile_has_unidirectional_references(left)) {
            return 2U;
        }
        return 3U + (unsigned int)
            (!((above->ref_frame[0] == 5U) ^
               (left->ref_frame[0] == 5U)));
    }
    if (above != 0 || left != 0) {
        const Av1BlockCell *edge = above != 0 ? above : left;

        if (!edge->is_inter || edge->ref_frame[1] == 0U) return 2U;
        return 4U *
            (unsigned int)av1_tile_has_unidirectional_references(edge);
    }
    return 2U;
}

static AvifdecStatus av1_tile_read_ref_frames(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields) {
    size_t feature_offset = (size_t)fields->segment_id * 8U;
    uint8_t counts[8];
    int compound = 0;

    if (fields->skip_mode) {
        fields->ref_frame[0] = config->skip_mode_frame[0];
        fields->ref_frame[1] = config->skip_mode_frame[1];
        return AVIFDEC_OK;
    }
    if (config->segmentation_enabled &&
        config->feature_enabled[feature_offset + AV1_SEG_LVL_REF_FRAME]) {
        int16_t reference = config->feature_data[
            feature_offset + AV1_SEG_LVL_REF_FRAME];
        if (reference < 0 || reference > 7) return AVIFDEC_INVALID_DATA;
        fields->ref_frame[0] = (uint8_t)reference;
        return AVIFDEC_OK;
    }
    if (config->segmentation_enabled &&
        (config->feature_enabled[feature_offset + AV1_SEG_LVL_SKIP] ||
         config->feature_enabled[feature_offset + AV1_SEG_LVL_GLOBALMV])) {
        fields->ref_frame[0] = 1U;
        return AVIFDEC_OK;
    }
    av1_tile_neighbor_ref_counts(config->block_state, availability,
                                 fields->row, fields->column, counts);
    if (config->reference_select && fields->width >= 2U &&
        fields->height >= 2U) {
        unsigned int context = av1_tile_compound_context(
            config->block_state, availability, fields->row, fields->column);
        compound = av1_symbol_read(
            decoder, cdfs->inter.compound_reference[context], 2U) != 0U;
    }
    if (compound) {
        unsigned int type_context =
            av1_tile_compound_reference_type_context(
                config->block_state, availability,
                fields->row, fields->column);
        uint32_t reference_type = av1_symbol_read(
            decoder, cdfs->inter.compound_reference_type[type_context], 2U);

        if (reference_type == 0U) {
            unsigned int context = av1_tile_count_context(
                counts[1] + counts[2] + counts[3] + counts[4],
                counts[5] + counts[6] + counts[7]);
            if (av1_symbol_read(
                    decoder,
                    cdfs->inter.unidirectional_reference[context][0], 2U)) {
                fields->ref_frame[0] = 5U;
                fields->ref_frame[1] = 7U;
            } else {
                context = av1_tile_count_context(counts[2],
                                                  counts[3] + counts[4]);
                if (!av1_symbol_read(
                        decoder,
                        cdfs->inter.unidirectional_reference[context][1], 2U)) {
                    fields->ref_frame[0] = 1U;
                    fields->ref_frame[1] = 2U;
                } else {
                    context = av1_tile_count_context(counts[3], counts[4]);
                    fields->ref_frame[0] = 1U;
                    fields->ref_frame[1] = av1_symbol_read(
                        decoder,
                        cdfs->inter.unidirectional_reference[context][2], 2U)
                        ? 4U : 3U;
                }
            }
        } else {
            unsigned int context = av1_tile_count_context(
                counts[1] + counts[2], counts[3] + counts[4]);
            if (!av1_symbol_read(
                    decoder, cdfs->inter.compound_forward_reference[context][0],
                    2U)) {
                context = av1_tile_count_context(counts[1], counts[2]);
                fields->ref_frame[0] = av1_symbol_read(
                    decoder,
                    cdfs->inter.compound_forward_reference[context][1], 2U)
                    ? 2U : 1U;
            } else {
                context = av1_tile_count_context(counts[3], counts[4]);
                fields->ref_frame[0] = av1_symbol_read(
                    decoder,
                    cdfs->inter.compound_forward_reference[context][2], 2U)
                    ? 4U : 3U;
            }
            context = av1_tile_count_context(counts[5] + counts[6], counts[7]);
            if (!av1_symbol_read(
                    decoder,
                    cdfs->inter.compound_backward_reference[context][0], 2U)) {
                context = av1_tile_count_context(counts[5], counts[6]);
                fields->ref_frame[1] = av1_symbol_read(
                    decoder,
                    cdfs->inter.compound_backward_reference[context][1], 2U)
                    ? 6U : 5U;
            } else {
                fields->ref_frame[1] = 7U;
            }
        }
    } else {
        unsigned int context = av1_tile_count_context(
            counts[1] + counts[2] + counts[3] + counts[4],
            counts[5] + counts[6] + counts[7]);
        if (av1_symbol_read(
                decoder, cdfs->inter.single_reference[context][0], 2U)) {
            context = av1_tile_count_context(counts[5] + counts[6], counts[7]);
            if (!av1_symbol_read(
                    decoder, cdfs->inter.single_reference[context][1], 2U)) {
                context = av1_tile_count_context(counts[5], counts[6]);
                fields->ref_frame[0] = av1_symbol_read(
                    decoder, cdfs->inter.single_reference[context][5], 2U)
                    ? 6U : 5U;
            } else {
                fields->ref_frame[0] = 7U;
            }
        } else {
            context = av1_tile_count_context(counts[1] + counts[2],
                                              counts[3] + counts[4]);
            if (av1_symbol_read(
                    decoder, cdfs->inter.single_reference[context][2], 2U)) {
                context = av1_tile_count_context(counts[3], counts[4]);
                fields->ref_frame[0] = av1_symbol_read(
                    decoder, cdfs->inter.single_reference[context][4], 2U)
                    ? 4U : 3U;
            } else {
                context = av1_tile_count_context(counts[1], counts[2]);
                fields->ref_frame[0] = av1_symbol_read(
                    decoder, cdfs->inter.single_reference[context][3], 2U)
                    ? 2U : 1U;
            }
        }
    }
    return decoder->status;
}

static AvifdecStatus av1_tile_global_mv(
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields,
    unsigned int list,
    Av1MotionVector *mv) {
    uint8_t reference = fields->ref_frame[list];

    if (reference == 0U) {
        mv->row = 0;
        mv->column = 0;
        return AVIFDEC_OK;
    }
    if (reference > 7U) return AVIFDEC_INVALID_DATA;
    return av1_mv_global(config->gm_type[reference - 1U],
                         config->gm_params[reference - 1U],
                         fields->row, fields->column,
                         fields->width << 2, fields->height << 2,
                         config->force_integer_mv,
                         config->allow_high_precision_mv, mv);
}

static AvifdecStatus av1_tile_add_neighbor_mv(
    const Av1TileModeConfig *config,
    const Av1MotionVector global[2],
    Av1MvStack *stack,
    const Av1BlockCell *neighbor,
    const Av1BlockTraceFields *fields,
    int compound,
    uint16_t weight) {
    Av1MotionVector first;
    Av1MotionVector second;
    int global_neighbor;
    unsigned int list;

    if (config == 0 || global == 0 || neighbor == 0 || !neighbor->is_inter) {
        return AVIFDEC_OK;
    }
    global_neighbor =
        (neighbor->y_mode == 15U || neighbor->y_mode == 23U) &&
        neighbor->width >= 2U && neighbor->height >= 2U;
    if (compound) {
        if (neighbor->ref_frame[0] != fields->ref_frame[0] ||
            neighbor->ref_frame[1] != fields->ref_frame[1]) {
            return AVIFDEC_OK;
        }
        first = global_neighbor && config->gm_type[fields->ref_frame[0] - 1U] > 1U
                ? global[0] : neighbor->mv[0];
        second = global_neighbor && config->gm_type[fields->ref_frame[1] - 1U] > 1U
                 ? global[1] : neighbor->mv[1];
        return av1_mv_stack_add(stack, first, second, 1, weight);
    }
    for (list = 0U; list < 2U; ++list) {
        if (neighbor->ref_frame[list] == fields->ref_frame[0]) {
            first = global_neighbor &&
                    config->gm_type[fields->ref_frame[0] - 1U] > 1U
                    ? global[0] : neighbor->mv[list];
            second.row = 0;
            second.column = 0;
            return av1_mv_stack_add(stack, first, second, 0, weight);
        }
    }
    return AVIFDEC_OK;
}

static uint32_t av1_tile_abs_int32(int32_t value) {
    return value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
}

static void av1_tile_sort_mv_range(Av1MvStack *stack,
                                   uint8_t start,
                                   uint8_t end) {
    uint8_t limit = end;

    while (limit > start + 1U) {
        uint8_t next_limit = start;
        uint8_t index;

        for (index = start + 1U; index < limit; ++index) {
            if (stack->candidates[index - 1U].weight <
                stack->candidates[index].weight) {
                Av1MvCandidate temporary = stack->candidates[index - 1U];

                stack->candidates[index - 1U] = stack->candidates[index];
                stack->candidates[index] = temporary;
                next_limit = index;
            }
        }
        if (next_limit == start) break;
        limit = next_limit;
    }
}

static AvifdecStatus av1_tile_add_temporal_mv(
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields,
    Av1MvStack *stack,
    const Av1MotionVector global[2],
    int compound,
    uint32_t delta_row,
    uint32_t delta_column,
    unsigned int *zero_mv_context) {
    uint32_t row = (fields->row + delta_row) | 1U;
    uint32_t column = (fields->column + delta_column) | 1U;
    size_t index;
    const Av1TemporalMotion *temporal;
    Av1MotionVector candidate[2];
    unsigned int list;

    if (delta_row == 0U && delta_column == 0U) *zero_mv_context = 1U;
    if (row >= config->block_state->tile_row_end ||
        column >= config->block_state->tile_column_end ||
        config->temporal_mvs == 0 || config->temporal_mv_stride == 0U) {
        return AVIFDEC_OK;
    }
    index = (size_t)(row >> 1) * config->temporal_mv_stride +
            (column >> 1);
    if (index >= config->temporal_mv_capacity) return AVIFDEC_INVALID_DATA;
    temporal = &config->temporal_mvs[index];
    if (!temporal->valid || temporal->ref_frame_offset <= 0) {
        return AVIFDEC_OK;
    }
    for (list = 0U; list < 1U + (unsigned int)compound; ++list) {
        uint8_t reference = fields->ref_frame[list];
        int32_t numerator;
        Av1MotionVector source;
        AvifdecStatus status;

        if (reference == 0U || reference > 7U) return AVIFDEC_INVALID_DATA;
        numerator = av1_relative_distance(
            config->enable_order_hint, config->order_hint_bits,
            config->current_order_hint,
            config->ref_order_hint[reference - 1U]);
        source.row = temporal->mv.row;
        source.column = temporal->mv.column;
        status = av1_mv_project(
            source, numerator, (uint32_t)temporal->ref_frame_offset,
            &candidate[list]);
        if (status != AVIFDEC_OK) return status;
        av1_mv_lower_precision(&candidate[list], config->force_integer_mv,
                               config->allow_high_precision_mv);
    }
    if (!compound) {
        candidate[1].row = 0;
        candidate[1].column = 0;
    }
    if (delta_row == 0U && delta_column == 0U) {
        *zero_mv_context = 0U;
        for (list = 0U; list < 1U + (unsigned int)compound; ++list) {
            if (av1_tile_abs_int32(
                    candidate[list].row - global[list].row) >= 16U ||
                av1_tile_abs_int32(candidate[list].column -
                                   global[list].column) >= 16U) {
                *zero_mv_context = 1U;
            }
        }
    }
    return av1_mv_stack_add(stack, candidate[0], candidate[1], compound, 2U);
}

static int av1_tile_reference_sign_bias(
    const Av1TileModeConfig *config,
    uint8_t reference) {
    if (reference == 0U || reference > AV1_REFS_PER_FRAME) return 0;
    return av1_relative_distance(
        config->enable_order_hint, config->order_hint_bits,
        config->ref_order_hint[reference - 1U],
        config->current_order_hint) > 0;
}

static void av1_tile_collect_compound_mv(
    const Av1TileModeConfig *config,
    const Av1BlockCell *neighbor,
    const Av1BlockTraceFields *fields,
    Av1MotionVector exact[2][2],
    uint8_t exact_count[2],
    Av1MotionVector different[2][2],
    uint8_t different_count[2]) {
    unsigned int neighbor_list;

    if (neighbor == 0 || !neighbor->is_inter) return;
    for (neighbor_list = 0U; neighbor_list < 2U; ++neighbor_list) {
        uint8_t neighbor_reference = neighbor->ref_frame[neighbor_list];
        unsigned int target_list;

        if (neighbor_reference == 0U ||
            neighbor_reference > AV1_REFS_PER_FRAME) {
            continue;
        }
        for (target_list = 0U; target_list < 2U; ++target_list) {
            uint8_t target_reference = fields->ref_frame[target_list];

            if (neighbor_reference == target_reference) {
                if (exact_count[target_list] < 2U) {
                    exact[target_list][exact_count[target_list]++] =
                        neighbor->mv[neighbor_list];
                }
            } else if (different_count[target_list] < 2U) {
                Av1MotionVector mv = neighbor->mv[neighbor_list];

                if (av1_tile_reference_sign_bias(
                        config, neighbor_reference) !=
                    av1_tile_reference_sign_bias(
                        config, target_reference)) {
                    mv.row = -mv.row;
                    mv.column = -mv.column;
                }
                different[target_list][different_count[target_list]++] = mv;
            }
        }
    }
}

static AvifdecStatus av1_tile_extend_compound_mv_stack(
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields,
    Av1MvStack *stack,
    const Av1MotionVector global[2]) {
    Av1MotionVector exact[2][2];
    Av1MotionVector different[2][2];
    Av1MotionVector candidates[2][2];
    uint8_t exact_count[2] = { 0U, 0U };
    uint8_t different_count[2] = { 0U, 0U };
    uint32_t scan_size = fields->width < fields->height
                         ? fields->width : fields->height;
    uint32_t offset;
    unsigned int list;

    if (scan_size > 16U) scan_size = 16U;
    if (fields->row > config->block_state->tile_row_start) {
        for (offset = 0U; offset < scan_size;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row - 1U,
                fields->column + offset);
            uint32_t step = neighbor != 0 && neighbor->width != 0U
                            ? neighbor->width : 1U;

            av1_tile_collect_compound_mv(
                config, neighbor, fields, exact, exact_count,
                different, different_count);
            if (step > scan_size - offset) step = scan_size - offset;
            offset += step;
        }
    }
    if (fields->column > config->block_state->tile_column_start) {
        for (offset = 0U; offset < scan_size;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row + offset,
                fields->column - 1U);
            uint32_t step = neighbor != 0 && neighbor->height != 0U
                            ? neighbor->height : 1U;

            av1_tile_collect_compound_mv(
                config, neighbor, fields, exact, exact_count,
                different, different_count);
            if (step > scan_size - offset) step = scan_size - offset;
            offset += step;
        }
    }
    for (list = 0U; list < 2U; ++list) {
        unsigned int index = 0U;
        unsigned int source;

        for (source = 0U; source < exact_count[list] && index < 2U;
             ++source) {
            candidates[index++][list] = exact[list][source];
        }
        for (source = 0U; source < different_count[list] && index < 2U;
             ++source) {
            candidates[index++][list] = different[list][source];
        }
        while (index < 2U) candidates[index++][list] = global[list];
    }
    if (stack->count == 0U) {
        stack->candidates[0].mv[0] = candidates[0][0];
        stack->candidates[0].mv[1] = candidates[0][1];
        stack->candidates[0].weight = 2U;
        stack->candidates[1].mv[0] = candidates[1][0];
        stack->candidates[1].mv[1] = candidates[1][1];
        stack->candidates[1].weight = 2U;
        stack->count = 2U;
        return AVIFDEC_OK;
    }
    if (stack->count == 1U) {
        const Av1MvCandidate *first = &stack->candidates[0];
        unsigned int candidate =
            first->mv[0].row == candidates[0][0].row &&
            first->mv[0].column == candidates[0][0].column &&
            first->mv[1].row == candidates[0][1].row &&
            first->mv[1].column == candidates[0][1].column
            ? 1U : 0U;

        stack->candidates[1].mv[0] = candidates[candidate][0];
        stack->candidates[1].mv[1] = candidates[candidate][1];
        stack->candidates[1].weight = 2U;
        stack->count = 2U;
    }
    return AVIFDEC_OK;
}

static void av1_tile_collect_single_mv(
    const Av1TileModeConfig *config,
    const Av1BlockCell *neighbor,
    uint8_t reference,
    Av1MvStack *stack) {
    unsigned int neighbor_list;

    if (neighbor == 0 || !neighbor->is_inter || stack->count >= 2U) return;
    for (neighbor_list = 0U;
         neighbor_list < 2U && stack->count < 2U; ++neighbor_list) {
        uint8_t neighbor_reference = neighbor->ref_frame[neighbor_list];
        Av1MotionVector mv;
        unsigned int index;

        if (neighbor_reference == 0U ||
            neighbor_reference > AV1_REFS_PER_FRAME) {
            continue;
        }
        mv = neighbor->mv[neighbor_list];
        if (av1_tile_reference_sign_bias(config, neighbor_reference) !=
            av1_tile_reference_sign_bias(config, reference)) {
            mv.row = -mv.row;
            mv.column = -mv.column;
        }
        for (index = 0U; index < stack->count; ++index) {
            if (stack->candidates[index].mv[0].row == mv.row &&
                stack->candidates[index].mv[0].column == mv.column) {
                break;
            }
        }
        if (index == stack->count) {
            stack->candidates[index].mv[0] = mv;
            stack->candidates[index].mv[1].row = 0;
            stack->candidates[index].mv[1].column = 0;
            stack->candidates[index].weight = 2U;
            ++stack->count;
        }
    }
}

static void av1_tile_extend_single_mv_stack(
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields,
    Av1MvStack *stack) {
    uint32_t scan_size = fields->width < fields->height
                         ? fields->width : fields->height;
    uint32_t offset;

    if (scan_size > 16U) scan_size = 16U;
    if (fields->row > config->block_state->tile_row_start) {
        for (offset = 0U; offset < scan_size && stack->count < 2U;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row - 1U,
                fields->column + offset);
            uint32_t step = neighbor != 0 && neighbor->width != 0U
                            ? neighbor->width : 1U;

            av1_tile_collect_single_mv(
                config, neighbor, fields->ref_frame[0], stack);
            if (step > scan_size - offset) step = scan_size - offset;
            offset += step;
        }
    }
    if (fields->column > config->block_state->tile_column_start) {
        for (offset = 0U; offset < scan_size && stack->count < 2U;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row + offset,
                fields->column - 1U);
            uint32_t step = neighbor != 0 && neighbor->height != 0U
                            ? neighbor->height : 1U;

            av1_tile_collect_single_mv(
                config, neighbor, fields->ref_frame[0], stack);
            if (step > scan_size - offset) step = scan_size - offset;
            offset += step;
        }
    }
}

static AvifdecStatus av1_tile_find_mv_stack(
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    Av1MvStack *stack,
    unsigned int *zero_mv_context) {
    Av1MotionVector global[2];
    int compound = fields->ref_frame[1] > 0U;
    uint8_t nearest_count;
    uint8_t found_count;
    uint32_t row_adjust;
    uint32_t column_adjust;
    uint32_t max_row_distance;
    uint32_t max_column_distance;
    uint32_t offset;
    AvifdecStatus status;

    avifdec_memory_fill(stack, 0U, sizeof(*stack));
    *zero_mv_context = 0U;
    status = av1_tile_global_mv(config, fields, 0U, &global[0]);
    if (status != AVIFDEC_OK) return status;
    global[1].row = 0;
    global[1].column = 0;
    if (compound) {
        status = av1_tile_global_mv(config, fields, 1U, &global[1]);
        if (status != AVIFDEC_OK) return status;
    }
    if (availability->above) {
        for (offset = 0U; offset < fields->width;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row - 1U,
                fields->column + offset);
            uint32_t overlap = neighbor != 0 && neighbor->width != 0U
                               ? neighbor->width : 1U;

            if (overlap > fields->width - offset) {
                overlap = fields->width - offset;
            }
            status = av1_tile_add_neighbor_mv(
                config, global, stack, neighbor, fields, compound,
                (uint16_t)(2U * overlap));
            if (status != AVIFDEC_OK) return status;
            offset += overlap;
        }
    }
    if (availability->left) {
        for (offset = 0U; offset < fields->height;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row + offset,
                fields->column - 1U);
            uint32_t overlap = neighbor != 0 && neighbor->height != 0U
                               ? neighbor->height : 1U;

            if (overlap > fields->height - offset) {
                overlap = fields->height - offset;
            }
            status = av1_tile_add_neighbor_mv(
                config, global, stack, neighbor, fields, compound,
                (uint16_t)(2U * overlap));
            if (status != AVIFDEC_OK) return status;
            offset += overlap;
        }
    }
    if (fields->row > config->block_state->tile_row_start &&
        fields->column + fields->width <
            config->block_state->tile_column_end) {
        status = av1_tile_add_neighbor_mv(
            config, global, stack,
            av1_block_cell(config->block_state, fields->row - 1U,
                           fields->column + fields->width),
            fields, compound, 4U);
        if (status != AVIFDEC_OK) return status;
    }
    for (offset = 0U; offset < stack->count; ++offset) {
        stack->candidates[offset].weight = (uint16_t)
            (stack->candidates[offset].weight + 640U);
    }
    nearest_count = stack->count;
    if (config->use_ref_frame_mvs) {
        uint32_t row_step = fields->height >= 16U ? 4U : 2U;
        uint32_t column_step = fields->width >= 16U ? 4U : 2U;
        uint32_t row_limit = fields->height < 16U ? fields->height : 16U;
        uint32_t column_limit = fields->width < 16U ? fields->width : 16U;
        uint32_t row;

        for (row = 0U; row < row_limit; row += row_step) {
            uint32_t column;

            for (column = 0U; column < column_limit;
                 column += column_step) {
                status = av1_tile_add_temporal_mv(
                    config, fields, stack, global, compound, row, column,
                    zero_mv_context);
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    if (fields->row > config->block_state->tile_row_start &&
        fields->column > config->block_state->tile_column_start) {
        status = av1_tile_add_neighbor_mv(
            config, global, stack,
            av1_block_cell(config->block_state, fields->row - 1U,
                           fields->column - 1U),
            fields, compound, 4U);
        if (status != AVIFDEC_OK) return status;
    }
    row_adjust = fields->height < 2U && (fields->row & 1U) != 0U;
    column_adjust = fields->width < 2U && (fields->column & 1U) != 0U;
    max_row_distance = (fields->height < 2U ? 4U : 6U) - row_adjust;
    max_column_distance =
        (fields->width < 2U ? 4U : 6U) - column_adjust;
    for (offset = 2U; offset <= 3U; ++offset) {
        uint32_t row_distance = 2U * offset - 1U - row_adjust;
        uint32_t column_distance = 2U * offset - 1U - column_adjust;
        uint32_t scan;
        uint32_t start;

        if (row_distance <= max_row_distance &&
            fields->row >=
                config->block_state->tile_row_start + row_distance) {
            start = fields->width < 2U && (fields->column & 1U) != 0U
                    ? 0U : 1U;
            for (scan = 0U; scan < fields->width;) {
                const Av1BlockCell *neighbor = av1_block_cell(
                    config->block_state, fields->row - row_distance,
                    fields->column + start + scan);
                uint32_t step = neighbor != 0 && neighbor->width != 0U
                                ? neighbor->width : 1U;

                if (step > fields->width) step = fields->width;
                if (step < 2U) step = 2U;
                status = av1_tile_add_neighbor_mv(
                    config, global, stack, neighbor, fields, compound,
                    (uint16_t)(2U * step));
                if (status != AVIFDEC_OK) return status;
                scan += step;
            }
        }
        if (column_distance <= max_column_distance &&
            fields->column >=
                config->block_state->tile_column_start + column_distance) {
            start = fields->height < 2U && (fields->row & 1U) != 0U
                    ? 0U : 1U;
            for (scan = 0U; scan < fields->height;) {
                const Av1BlockCell *neighbor = av1_block_cell(
                    config->block_state, fields->row + start + scan,
                    fields->column - column_distance);
                uint32_t step = neighbor != 0 && neighbor->height != 0U
                                ? neighbor->height : 1U;

                if (step > fields->height) step = fields->height;
                if (step < 2U) step = 2U;
                status = av1_tile_add_neighbor_mv(
                    config, global, stack, neighbor, fields, compound,
                    (uint16_t)(2U * step));
                if (status != AVIFDEC_OK) return status;
                scan += step;
            }
        }
    }
    av1_tile_sort_mv_range(stack, 0U, nearest_count);
    av1_mv_stack_sort(stack, nearest_count);
    if (compound && stack->count < 2U) {
        status = av1_tile_extend_compound_mv_stack(
            config, fields, stack, global);
        if (status != AVIFDEC_OK) return status;
    } else if (!compound && stack->count < 2U) {
        av1_tile_extend_single_mv_stack(config, fields, stack);
    }
    found_count = stack->count;
    for (offset = 0U; offset < stack->count; ++offset) {
        unsigned int list;

        for (list = 0U; list < 1U + (unsigned int)compound; ++list) {
            av1_mv_clamp(
                &stack->candidates[offset].mv[list],
                fields->row, fields->column, fields->width, fields->height,
                config->block_state->mi_rows,
                config->block_state->mi_columns, 128U);
        }
    }
    fields->mv_stack_count = found_count;
    avifdec_memory_copy(fields->mv_stack, stack->candidates,
                        sizeof(fields->mv_stack));
    while (stack->count < 2U) {
        uint8_t before = stack->count;

        status = av1_mv_stack_add(stack, global[0], global[1], compound, 2U);
        if (status != AVIFDEC_OK) return status;
        if (stack->count == before) {
            Av1MotionVector zero;
            zero.row = 0;
            zero.column = 0;
            status = av1_mv_stack_add(stack, zero, zero, compound, 1U);
            if (status != AVIFDEC_OK) return status;
            if (stack->count == before) break;
        }
    }
    return AVIFDEC_OK;
}

static unsigned int av1_tile_drl_context(const Av1MvStack *stack,
                                          unsigned int index) {
    const uint16_t category = 640U;
    uint16_t first = stack->candidates[index].weight;
    uint16_t second = stack->candidates[index + 1U].weight;

    if (first >= category && second >= category) return 0U;
    if (first >= category && second < category) return 1U;
    if (first < category && second < category) return 2U;
    return 0U;
}

static uint8_t av1_tile_component_mode(uint8_t mode, unsigned int list) {
    static const uint8_t compound_modes[8][2] = {
        { 13U, 13U }, { 14U, 14U }, { 13U, 16U }, { 16U, 13U },
        { 14U, 16U }, { 16U, 14U }, { 15U, 15U }, { 16U, 16U }
    };

    return mode >= 17U && mode <= 24U
           ? compound_modes[mode - 17U][list] : mode;
}

static int av1_tile_mode_has_new_mv(uint8_t mode) {
    return mode == 16U || (mode >= 19U && mode <= 22U) || mode == 24U;
}

static void av1_tile_count_mode_neighbor(
    const Av1BlockCell *neighbor,
    const Av1BlockTraceFields *fields,
    int row_neighbor,
    unsigned int *row_match,
    unsigned int *column_match,
    unsigned int *new_count) {
    unsigned int list;

    if (neighbor == 0 || !neighbor->is_inter) return;
    if (fields->ref_frame[1] > 0U) {
        if (neighbor->ref_frame[0] != fields->ref_frame[0] ||
            neighbor->ref_frame[1] != fields->ref_frame[1]) {
            return;
        }
        if (row_neighbor) *row_match = 1U;
        else *column_match = 1U;
        if (new_count != 0 &&
            av1_tile_mode_has_new_mv(neighbor->y_mode)) {
            ++*new_count;
        }
        return;
    }
    for (list = 0U; list < 2U; ++list) {
        if (neighbor->ref_frame[list] != fields->ref_frame[0]) continue;
        if (row_neighbor) *row_match = 1U;
        else *column_match = 1U;
        if (new_count != 0 &&
            av1_tile_mode_has_new_mv(neighbor->y_mode)) {
            ++*new_count;
        }
        return;
    }
}

static void av1_tile_mode_contexts(
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    const Av1BlockTraceFields *fields,
    unsigned int *new_mv_context,
    unsigned int *reference_mv_context) {
    unsigned int row_match = 0U;
    unsigned int column_match = 0U;
    unsigned int new_count = 0U;
    unsigned int nearest_matches;
    unsigned int total_matches;
    uint32_t row_adjust;
    uint32_t column_adjust;
    uint32_t max_row_distance;
    uint32_t max_column_distance;
    uint32_t offset;

    if (availability->above) {
        for (offset = 0U; offset < fields->width;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row - 1U,
                fields->column + offset);
            uint32_t step = neighbor != 0 && neighbor->width != 0U
                            ? neighbor->width : 1U;

            if (step > fields->width - offset) {
                step = fields->width - offset;
            }
            av1_tile_count_mode_neighbor(
                neighbor, fields, 1,
                &row_match, &column_match, &new_count);
            offset += step;
        }
    }
    if (availability->left) {
        for (offset = 0U; offset < fields->height;) {
            const Av1BlockCell *neighbor = av1_block_cell(
                config->block_state, fields->row + offset,
                fields->column - 1U);
            uint32_t step = neighbor != 0 && neighbor->height != 0U
                            ? neighbor->height : 1U;

            if (step > fields->height - offset) {
                step = fields->height - offset;
            }
            av1_tile_count_mode_neighbor(
                neighbor, fields, 0,
                &row_match, &column_match, &new_count);
            offset += step;
        }
    }
    if (fields->row > config->block_state->tile_row_start &&
        fields->column + fields->width <
            config->block_state->tile_column_end &&
        (fields->width > fields->height ? fields->width : fields->height) <=
            16U) {
        av1_tile_count_mode_neighbor(
            av1_block_cell(config->block_state, fields->row - 1U,
                           fields->column + fields->width),
            fields, 1,
            &row_match, &column_match, &new_count);
    }
    nearest_matches = row_match + column_match;
    if (fields->row > config->block_state->tile_row_start &&
        fields->column > config->block_state->tile_column_start) {
        av1_tile_count_mode_neighbor(
            av1_block_cell(config->block_state, fields->row - 1U,
                           fields->column - 1U),
            fields, 1,
            &row_match, &column_match, 0);
    }
    row_adjust = fields->height < 2U && (fields->row & 1U) != 0U;
    column_adjust = fields->width < 2U && (fields->column & 1U) != 0U;
    max_row_distance = (fields->height < 2U ? 4U : 6U) - row_adjust;
    max_column_distance =
        (fields->width < 2U ? 4U : 6U) - column_adjust;
    for (offset = 2U; offset <= 3U; ++offset) {
        uint32_t row_distance = 2U * offset - 1U - row_adjust;
        uint32_t column_distance = 2U * offset - 1U - column_adjust;
        uint32_t scan;
        uint32_t start;

        if (row_distance <= max_row_distance &&
            fields->row >=
                config->block_state->tile_row_start + row_distance) {
            start = fields->width < 2U && (fields->column & 1U) != 0U
                    ? 0U : 1U;
            for (scan = 0U; scan < fields->width;) {
                const Av1BlockCell *neighbor = av1_block_cell(
                    config->block_state, fields->row - row_distance,
                    fields->column + start + scan);
                uint32_t step = neighbor != 0 && neighbor->width != 0U
                                ? neighbor->width : 1U;

                if (step > fields->width) step = fields->width;
                if (step < 2U) step = 2U;
                av1_tile_count_mode_neighbor(
                    neighbor, fields, 1,
                    &row_match, &column_match, 0);
                scan += step;
            }
        }
        if (column_distance <= max_column_distance &&
            fields->column >=
                config->block_state->tile_column_start + column_distance) {
            start = fields->height < 2U && (fields->row & 1U) != 0U
                    ? 0U : 1U;
            for (scan = 0U; scan < fields->height;) {
                const Av1BlockCell *neighbor = av1_block_cell(
                    config->block_state, fields->row + start + scan,
                    fields->column - column_distance);
                uint32_t step = neighbor != 0 && neighbor->height != 0U
                                ? neighbor->height : 1U;

                if (step > fields->height) step = fields->height;
                if (step < 2U) step = 2U;
                av1_tile_count_mode_neighbor(
                    neighbor, fields, 0,
                    &row_match, &column_match, 0);
                scan += step;
            }
        }
    }
    total_matches = row_match + column_match;
    if (nearest_matches == 0U) {
        *new_mv_context = total_matches == 0U ? 0U : 1U;
        *reference_mv_context = total_matches == 0U ? 0U
                              : total_matches == 1U ? 1U : 2U;
    } else if (nearest_matches == 1U) {
        *new_mv_context = new_count != 0U ? 2U : 3U;
        *reference_mv_context = total_matches == 1U ? 3U : 4U;
    } else {
        *new_mv_context = new_count != 0U ? 4U : 5U;
        *reference_mv_context = 5U;
    }
}

static AvifdecStatus av1_tile_read_inter_mode_and_mvs(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields) {
    Av1MvStack stack;
    size_t feature_offset = (size_t)fields->segment_id * 8U;
    int compound = fields->ref_frame[1] > 0U;
    int forced_global = config->segmentation_enabled &&
        (config->feature_enabled[feature_offset + AV1_SEG_LVL_SKIP] ||
         config->feature_enabled[feature_offset + AV1_SEG_LVL_GLOBALMV]);
    AvifdecStatus status;
    unsigned int new_mv_context = 0U;
    unsigned int zero_mv_context = 0U;
    unsigned int reference_mv_context = 0U;
    unsigned int list;

    status = av1_tile_find_mv_stack(
        config, availability, fields, &stack, &zero_mv_context);
    if (status != AVIFDEC_OK) return status;
    av1_tile_mode_contexts(
        config, availability, fields, &new_mv_context,
        &reference_mv_context);
    if (fields->skip_mode) {
        fields->y_mode = 17U;
    } else if (forced_global) {
        fields->y_mode = 15U;
    } else if (compound) {
        static const uint8_t compound_context[3][5] = {
            { 0U, 1U, 1U, 1U, 1U },
            { 1U, 2U, 3U, 4U, 4U },
            { 4U, 4U, 5U, 6U, 7U }
        };
        unsigned int row = reference_mv_context >> 1;
        unsigned int column = new_mv_context < 5U ? new_mv_context : 4U;

        if (row >= 3U) return AVIFDEC_INVALID_DATA;
        fields->y_mode = (uint8_t)(17U + av1_symbol_read(
            decoder, cdfs->inter.compound_mode[
                compound_context[row][column]], 8U));
    } else if (av1_symbol_read(
                   decoder, cdfs->inter.new_mv[new_mv_context], 2U) == 0U) {
        fields->y_mode = 16U;
    } else if (av1_symbol_read(
                   decoder, cdfs->inter.zero_mv[zero_mv_context], 2U) == 0U) {
        fields->y_mode = 15U;
    } else {
        fields->y_mode = av1_symbol_read(
            decoder, cdfs->inter.reference_mv[reference_mv_context], 2U) == 0U
            ? 13U : 14U;
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    if (fields->y_mode == 16U || fields->y_mode == 24U) {
        unsigned int index;

           for (index = 0U; index < 2U &&
               fields->mv_stack_count > index + 1U; ++index) {
            unsigned int drl_context = av1_tile_drl_context(&stack, index);

            if (av1_symbol_read(
                    decoder, cdfs->inter.drl_mode[drl_context], 2U) == 0U) {
                fields->ref_mv_index = (uint8_t)index;
                break;
            }
            fields->ref_mv_index = (uint8_t)(index + 1U);
        }
    } else if (fields->y_mode == 14U || fields->y_mode == 18U ||
               fields->y_mode == 21U || fields->y_mode == 22U) {
        unsigned int index;

        fields->ref_mv_index = 0U;
           for (index = 1U; index < 3U &&
               fields->mv_stack_count > index + 1U; ++index) {
            unsigned int drl_context = av1_tile_drl_context(&stack, index);

            if (av1_symbol_read(
                    decoder, cdfs->inter.drl_mode[drl_context], 2U) == 0U) {
                fields->ref_mv_index = (uint8_t)(index - 1U);
                break;
            }
            fields->ref_mv_index = (uint8_t)index;
        }
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    for (list = 0U; list < 1U + (unsigned int)compound; ++list) {
        uint8_t component_mode = av1_tile_component_mode(fields->y_mode, list);
        unsigned int position = component_mode == 13U ? 0U
            : fields->ref_mv_index;

        if (component_mode == 14U) ++position;
        if (component_mode == 16U &&
            (fields->y_mode == 21U || fields->y_mode == 22U)) {
            ++position;
        }

        if (component_mode == 15U) {
            status = av1_tile_global_mv(config, fields, list,
                                        &fields->pred_mv[list]);
        } else {
            if (stack.count == 0U) return AVIFDEC_INVALID_DATA;
            if (component_mode == 16U && fields->mv_stack_count <= 1U) {
                position = 0U;
            }
            if (position >= stack.count) position = stack.count - 1U;
            fields->pred_mv[list] = stack.candidates[position].mv[list];
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
        if (component_mode == 16U) {
            status = av1_mv_read(
                decoder, &cdfs->inter, config->force_integer_mv,
                config->allow_high_precision_mv, fields->pred_mv[list],
                &fields->mv[list]);
            if (status != AVIFDEC_OK) return status;
        } else {
            fields->mv[list] = fields->pred_mv[list];
        }
        if (fields->mv[list].row <= -16384 ||
            fields->mv[list].row >= 16384 ||
            fields->mv[list].column <= -16384 ||
            fields->mv[list].column >= 16384) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    return decoder->status;
}

static int av1_tile_intrabc_mv_valid(
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields,
    Av1MotionVector mv) {
    const int64_t block_width = (int64_t)fields->width << 2;
    const int64_t block_height = (int64_t)fields->height << 2;
    const int64_t tile_top =
        (int64_t)config->block_state->tile_row_start << 2;
    const int64_t tile_left =
        (int64_t)config->block_state->tile_column_start << 2;
    const int64_t tile_bottom =
        (int64_t)config->block_state->tile_row_end << 2;
    const int64_t tile_right =
        (int64_t)config->block_state->tile_column_end << 2;
    int64_t source_top;
    int64_t source_left;
    int64_t source_bottom;
    int64_t source_right;
    int64_t active_sb_row;
    int64_t active_sb64_column;
    int64_t source_sb_row;
    int64_t source_sb64_column;
    int64_t total_sb64_per_row;
    int64_t active_sb64;
    int64_t source_sb64;
    int64_t wavefront_offset;
    int64_t sb_height;

    if ((mv.row & 7) != 0 || (mv.column & 7) != 0 ||
        mv.row <= -16384 || mv.row >= 16384 ||
        mv.column <= -16384 || mv.column >= 16384) {
        return 0;
    }
    source_top = ((int64_t)fields->row << 2) + (mv.row >> 3);
    source_left = ((int64_t)fields->column << 2) + (mv.column >> 3);
    source_bottom = source_top + block_height;
    source_right = source_left + block_width;
    if (!config->monochrome) {
        if (block_width < 8 && config->block_state->subsampling_x) {
            source_left -= 4;
        }
        if (block_height < 8 && config->block_state->subsampling_y) {
            source_top -= 4;
        }
    }
    if (source_top < tile_top || source_left < tile_left ||
        source_bottom > tile_bottom || source_right > tile_right) {
        return 0;
    }
    sb_height = (int64_t)config->superblock_mi << 2;
    active_sb_row = ((int64_t)fields->row << 2) / sb_height;
    active_sb64_column = ((int64_t)fields->column << 2) >> 6;
    source_sb_row = (source_bottom - 1) / sb_height;
    source_sb64_column = (source_right - 1) >> 6;
    total_sb64_per_row =
        (((int64_t)config->block_state->tile_column_end -
          config->block_state->tile_column_start - 1) >>
         4) +
        1;
    active_sb64 =
        active_sb_row * total_sb64_per_row + active_sb64_column;
    source_sb64 =
        source_sb_row * total_sb64_per_row + source_sb64_column;
    if (source_sb64 >= active_sb64 - 4) return 0;
    wavefront_offset =
        (5 + (config->superblock_mi == 32U)) *
        (active_sb_row - source_sb_row);
    if (source_sb_row > active_sb_row ||
        source_sb64_column >=
            active_sb64_column - 4 + wavefront_offset) {
        return 0;
    }
    return 1;
}

static AvifdecStatus av1_tile_read_intrabc(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields) {
    Av1MvStack stack;
    unsigned int zero_mv_context;
    Av1MotionVector prediction;
    AvifdecStatus status;

    fields->use_intrabc = (uint8_t)av1_symbol_read(
        decoder, cdfs->intrabc, 2U);
    if (decoder->status != AVIFDEC_OK || !fields->use_intrabc) {
        return decoder->status;
    }
    fields->is_inter = 1U;
    fields->y_mode = 0U;
    fields->uv_mode = 0U;
    fields->motion_mode = 0U;
    fields->warp_sample_count = 0U;
    fields->compound_type = 0U;
    fields->palette_size_y = 0U;
    fields->palette_size_uv = 0U;
    fields->interp_filter[0] = AV1_INTERP_BILINEAR;
    fields->interp_filter[1] = AV1_INTERP_BILINEAR;
    fields->ref_frame[0] = 0U;
    fields->ref_frame[1] = 0U;
    status = av1_tile_find_mv_stack(
        config, availability, fields, &stack, &zero_mv_context);
    if (status != AVIFDEC_OK) return status;
    prediction = stack.candidates[0].mv[0];
    if (prediction.row == 0 && prediction.column == 0) {
        prediction = stack.candidates[1].mv[0];
    }
    if (prediction.row == 0 && prediction.column == 0) {
        if (fields->row < config->block_state->tile_row_start +
                              config->superblock_mi) {
            prediction.row = 0;
            prediction.column =
                -((int32_t)config->superblock_mi * 4 + 256) * 8;
        } else {
            prediction.row =
                -(int32_t)config->superblock_mi * 4 * 8;
            prediction.column = 0;
        }
    }
    av1_mv_lower_precision(&prediction, 1, 0);
    fields->pred_mv[0] = prediction;
    status = av1_mv_read_cdfs(
        decoder, cdfs->dv_joint, cdfs->dv_component,
        1, 0, prediction, &fields->mv[0]);
    if (status != AVIFDEC_OK) return status;
    return av1_tile_intrabc_mv_valid(config, fields, fields->mv[0])
               ? AVIFDEC_OK
               : AVIFDEC_INVALID_DATA;
}

static uint8_t av1_tile_neighbor_interp_filter(
    const Av1BlockCell *neighbor,
    uint8_t reference,
    unsigned int direction) {
    if (neighbor == 0 || !neighbor->is_inter ||
        direction >= 2U ||
        (neighbor->ref_frame[0] != reference &&
         neighbor->ref_frame[1] != reference)) {
        return 3U;
    }
    return neighbor->interp_filter[direction] < 3U
           ? neighbor->interp_filter[direction] : 3U;
}

static unsigned int av1_tile_interp_context(
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    const Av1BlockTraceFields *fields,
    unsigned int direction) {
    const Av1BlockCell *above = availability->above
        ? av1_block_cell(
            config->block_state, fields->row - 1U, fields->column)
        : 0;
    const Av1BlockCell *left = availability->left
        ? av1_block_cell(
            config->block_state, fields->row, fields->column - 1U)
        : 0;
    uint8_t above_filter = av1_tile_neighbor_interp_filter(
        above, fields->ref_frame[0], direction);
    uint8_t left_filter = av1_tile_neighbor_interp_filter(
        left, fields->ref_frame[0], direction);
    unsigned int context = fields->ref_frame[1] > 0U ? 4U : 0U;

    context += direction != 0U ? 8U : 0U;
    if (left_filter == above_filter) return context + left_filter;
    if (left_filter == 3U) return context + above_filter;
    if (above_filter == 3U) return context + left_filter;
    return context + 3U;
}

static AvifdecStatus av1_tile_read_interp_filters(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields) {
    unsigned int list_count = fields->ref_frame[1] > 0U ? 2U : 1U;
    unsigned int list;
    int interpolation_needed = !fields->skip_mode;

    for (list = 0U; list < list_count; ++list) {
        uint8_t reference = fields->ref_frame[list];
        uint8_t component_mode =
            av1_tile_component_mode(fields->y_mode, list);

        if (!config->force_integer_mv &&
            reference > 0U && reference <= 7U &&
            component_mode == 15U &&
            fields->width >= 2U && fields->height >= 2U &&
            config->gm_type[reference - 1U] != 1U) {
            interpolation_needed = 0;
        }
    }
    if (!interpolation_needed || config->interpolation_filter != 4U) {
        uint8_t filter = config->interpolation_filter < 3U
                         ? config->interpolation_filter : 0U;

        fields->interp_filter[0] = filter;
        fields->interp_filter[1] = filter;
        return AVIFDEC_OK;
    }
    fields->interp_filter[0] = (uint8_t)av1_symbol_read(
        decoder,
        cdfs->inter.switchable_interp[
            av1_tile_interp_context(
                config, availability, fields, 0U)],
        3U);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    if (config->enable_dual_filter) {
        fields->interp_filter[1] = (uint8_t)av1_symbol_read(
            decoder,
            cdfs->inter.switchable_interp[
                av1_tile_interp_context(
                    config, availability, fields, 1U)],
            3U);
    } else {
        fields->interp_filter[1] = fields->interp_filter[0];
    }
    return decoder->status;
}

static unsigned int av1_tile_compound_group_context(
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    const Av1BlockTraceFields *fields) {
    const Av1BlockCell *neighbors[2] = { 0, 0 };
    unsigned int context = 0U;
    unsigned int index;

    if (availability->above) {
        neighbors[0] = av1_block_cell(
            config->block_state, fields->row - 1U, fields->column);
    }
    if (availability->left) {
        neighbors[1] = av1_block_cell(
            config->block_state, fields->row, fields->column - 1U);
    }
    for (index = 0U; index < 2U; ++index) {
        if (neighbors[index] == 0) continue;
        if (neighbors[index]->ref_frame[1] > 0U) {
            context += neighbors[index]->compound_group_index;
        } else if (neighbors[index]->ref_frame[0] == 7U) {
            context += 3U;
        }
    }
    return context < 6U ? context : 5U;
}

static unsigned int av1_tile_compound_index_context(
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    const Av1BlockTraceFields *fields) {
    const Av1BlockCell *neighbors[2] = { 0, 0 };
    int32_t first_distance;
    int32_t second_distance;
    unsigned int context = 0U;
    unsigned int index;

    first_distance = av1_relative_distance(
        config->enable_order_hint, config->order_hint_bits,
        config->current_order_hint,
        config->ref_order_hint[fields->ref_frame[0] - 1U]);
    second_distance = av1_relative_distance(
        config->enable_order_hint, config->order_hint_bits,
        config->ref_order_hint[fields->ref_frame[1] - 1U],
        config->current_order_hint);
    if (first_distance < 0) first_distance = -first_distance;
    if (second_distance < 0) second_distance = -second_distance;
    if (first_distance == second_distance) context = 3U;
    if (availability->above) {
        neighbors[0] = av1_block_cell(
            config->block_state, fields->row - 1U, fields->column);
    }
    if (availability->left) {
        neighbors[1] = av1_block_cell(
            config->block_state, fields->row, fields->column - 1U);
    }
    for (index = 0U; index < 2U; ++index) {
        if (neighbors[index] == 0) continue;
        if (neighbors[index]->ref_frame[1] > 0U) {
            context += neighbors[index]->compound_index;
        } else if (neighbors[index]->ref_frame[0] == 7U) {
            ++context;
        }
    }
    return context;
}

static int av1_tile_wedge_allowed(uint8_t block_size) {
    return (block_size >= 3U && block_size <= 9U) ||
           block_size == 18U || block_size == 19U;
}

static int av1_tile_has_overlappable_neighbor(
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    const Av1BlockTraceFields *fields) {
    uint32_t position4;

    if (availability->above) {
        const uint32_t right =
            fields->column + fields->width <
                    config->block_state->mi_columns
                ? fields->column + fields->width
                : config->block_state->mi_columns;
        position4 = fields->column;
        while (position4 < right) {
            const Av1BlockCell *cell = av1_block_cell(
                config->block_state, fields->row - 1U,
                position4 | 1U);
            uint32_t step4 = cell == 0 ? 2U : cell->width;
            if (step4 < 2U) step4 = 2U;
            if (step4 > 16U) step4 = 16U;
            if (cell != 0 && cell->is_inter &&
                !cell->use_intrabc && cell->ref_frame[0] > 0U) {
                return 1;
            }
            position4 += step4;
        }
    }
    if (availability->left) {
        const uint32_t bottom =
            fields->row + fields->height <
                    config->block_state->mi_rows
                ? fields->row + fields->height
                : config->block_state->mi_rows;
        position4 = fields->row;
        while (position4 < bottom) {
            const Av1BlockCell *cell = av1_block_cell(
                config->block_state, position4 | 1U,
                fields->column - 1U);
            uint32_t step4 = cell == 0 ? 2U : cell->height;
            if (step4 < 2U) step4 = 2U;
            if (step4 > 16U) step4 = 16U;
            if (cell != 0 && cell->is_inter &&
                !cell->use_intrabc && cell->ref_frame[0] > 0U) {
                return 1;
            }
            position4 += step4;
        }
    }
    return 0;
}

static void av1_tile_add_warp_sample(
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields,
    int32_t delta_row,
    int32_t delta_column,
    Av1WarpSample samples[AV1_WARP_MAX_SAMPLES],
    size_t *sample_count,
    size_t *scanned_count) {
    int64_t candidate_row = (int64_t)fields->row + delta_row;
    int64_t candidate_column = (int64_t)fields->column + delta_column;
    const Av1BlockCell *cell;
    uint32_t top;
    uint32_t left;
    int32_t middle_x;
    int32_t middle_y;
    uint32_t threshold;
    uint32_t difference;
    int valid;

    if (*scanned_count >= AV1_WARP_MAX_SAMPLES ||
        candidate_row < config->block_state->tile_row_start ||
        candidate_column < config->block_state->tile_column_start ||
        candidate_row >= config->block_state->tile_row_end ||
        candidate_column >= config->block_state->tile_column_end) {
        return;
    }
    cell = av1_block_cell(
        config->block_state, (uint32_t)candidate_row,
        (uint32_t)candidate_column);
    if (cell == 0 || cell->width == 0U || cell->height == 0U ||
        !cell->is_inter || cell->use_intrabc ||
        cell->ref_frame[0] != fields->ref_frame[0] ||
        cell->ref_frame[1] != 0U) {
        return;
    }
    top = (uint32_t)candidate_row & ~(uint32_t)(cell->height - 1U);
    left = (uint32_t)candidate_column & ~(uint32_t)(cell->width - 1U);
    middle_x = (int32_t)(left << 2) + (int32_t)cell->width * 2 - 1;
    middle_y = (int32_t)(top << 2) + (int32_t)cell->height * 2 - 1;
    threshold = fields->width > fields->height
                    ? fields->width << 2
                    : fields->height << 2;
    if (threshold < 16U) threshold = 16U;
    if (threshold > 112U) threshold = 112U;
    difference =
        av1_tile_abs_int32(cell->mv[0].row - fields->mv[0].row) +
        av1_tile_abs_int32(cell->mv[0].column - fields->mv[0].column);
    valid = difference <= threshold;
    ++*scanned_count;
    if (!valid && *scanned_count > 1U) return;
    samples[*sample_count].source_x = middle_x * 8;
    samples[*sample_count].source_y = middle_y * 8;
    samples[*sample_count].destination_x =
        middle_x * 8 + cell->mv[0].column;
    samples[*sample_count].destination_y =
        middle_y * 8 + cell->mv[0].row;
    if (valid) ++*sample_count;
}

static size_t av1_tile_find_warp_samples(
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    const Av1BlockTraceFields *fields,
    Av1WarpSample samples[AV1_WARP_MAX_SAMPLES]) {
    int do_top_left = 1;
    int do_top_right = 1;
    size_t sample_count = 0U;
    size_t scanned_count = 0U;

    if (availability->above) {
        const Av1BlockCell *cell = av1_block_cell(
            config->block_state, fields->row - 1U, fields->column);
        uint32_t source_width = cell == 0 ? 1U : cell->width;

        if (fields->width <= source_width) {
            int32_t column_offset =
                -(int32_t)(fields->column & (source_width - 1U));
            if (column_offset < 0) do_top_left = 0;
            if (column_offset + (int32_t)source_width >
                (int32_t)fields->width) {
                do_top_right = 0;
            }
            av1_tile_add_warp_sample(
                config, fields, -1, 0, samples,
                &sample_count, &scanned_count);
        } else {
            uint32_t offset = 0U;
            uint32_t limit = fields->width;
            if (limit > config->block_state->mi_columns - fields->column) {
                limit =
                    config->block_state->mi_columns - fields->column;
            }
            while (offset < limit) {
                uint32_t step;
                cell = av1_block_cell(
                    config->block_state, fields->row - 1U,
                    fields->column + offset);
                source_width = cell == 0 ? 1U : cell->width;
                step = fields->width < source_width
                           ? fields->width
                           : source_width;
                av1_tile_add_warp_sample(
                    config, fields, -1, (int32_t)offset, samples,
                    &sample_count, &scanned_count);
                offset += step;
            }
        }
    }
    if (availability->left) {
        const Av1BlockCell *cell = av1_block_cell(
            config->block_state, fields->row, fields->column - 1U);
        uint32_t source_height = cell == 0 ? 1U : cell->height;

        if (fields->height <= source_height) {
            int32_t row_offset =
                -(int32_t)(fields->row & (source_height - 1U));
            if (row_offset < 0) do_top_left = 0;
            av1_tile_add_warp_sample(
                config, fields, 0, -1, samples,
                &sample_count, &scanned_count);
        } else {
            uint32_t offset = 0U;
            uint32_t limit = fields->height;
            if (limit > config->block_state->mi_rows - fields->row) {
                limit = config->block_state->mi_rows - fields->row;
            }
            while (offset < limit) {
                uint32_t step;
                cell = av1_block_cell(
                    config->block_state, fields->row + offset,
                    fields->column - 1U);
                source_height = cell == 0 ? 1U : cell->height;
                step = fields->height < source_height
                           ? fields->height
                           : source_height;
                av1_tile_add_warp_sample(
                    config, fields, (int32_t)offset, -1, samples,
                    &sample_count, &scanned_count);
                offset += step;
            }
        }
    }
    if (do_top_left) {
        av1_tile_add_warp_sample(
            config, fields, -1, -1, samples,
            &sample_count, &scanned_count);
    }
    if (do_top_right &&
        (fields->width > fields->height
             ? fields->width
             : fields->height) <= 16U) {
        av1_tile_add_warp_sample(
            config, fields, -1, (int32_t)fields->width, samples,
            &sample_count, &scanned_count);
    }
    if (sample_count == 0U && scanned_count > 0U) sample_count = 1U;
    return sample_count;
}

static AvifdecStatus av1_tile_read_motion_mode(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    uint8_t block_size) {
    Av1WarpSample samples[AV1_WARP_MAX_SAMPLES];
    size_t sample_count;
    unsigned int reference_index;
    unsigned int list;
    int scaled;

    fields->motion_mode = 0U;
    for (list = 0U;
         list < 1U + (unsigned int)(fields->ref_frame[1] > 0U);
         ++list) {
        uint8_t reference = fields->ref_frame[list];
        if (reference > 0U && reference <= 7U &&
            av1_tile_component_mode(fields->y_mode, list) == 15U &&
            config->gm_type[reference - 1U] > 1U) {
            avifdec_memory_copy(
                fields->warp_params[list],
                config->gm_params[reference - 1U],
                sizeof(fields->warp_params[list]));
        }
    }
    if (fields->skip_mode || !config->is_motion_mode_switchable ||
        fields->width < 2U || fields->height < 2U) {
        return AVIFDEC_OK;
    }
    reference_index = fields->ref_frame[0] - 1U;
    if (fields->ref_frame[0] == 0U || reference_index >= 7U) {
        return AVIFDEC_INVALID_DATA;
    }
    if (!config->force_integer_mv &&
        av1_tile_component_mode(fields->y_mode, 0U) == 15U &&
        config->gm_type[reference_index] > 1U) {
        return AVIFDEC_OK;
    }
    if (fields->interintra || fields->ref_frame[1] > 0U ||
        !av1_tile_has_overlappable_neighbor(config, availability, fields)) {
        return AVIFDEC_OK;
    }
    sample_count = av1_tile_find_warp_samples(
        config, availability, fields, samples);
    fields->warp_sample_count = (uint8_t)sample_count;
    scaled =
        config->reference_width[reference_index] !=
            config->current_frame_width ||
        config->reference_height[reference_index] !=
            config->current_frame_height;
    if (!config->force_integer_mv && sample_count > 0U &&
        config->allow_warped_motion && !scaled) {
        Av1WarpModel model;
        Av1WarpStatus warp_status;
        int32_t middle_x =
            (int32_t)(fields->column << 2) +
            (int32_t)fields->width * 2 - 1;
        int32_t middle_y =
            (int32_t)(fields->row << 2) +
            (int32_t)fields->height * 2 - 1;

        fields->motion_mode = (uint8_t)av1_symbol_read(
            decoder, cdfs->inter.motion_mode[block_size], 3U);
        if (decoder->status != AVIFDEC_OK) return decoder->status;
        if (fields->motion_mode != 2U) return AVIFDEC_OK;
        warp_status = av1_warp_project_samples(
            &model, samples, sample_count, middle_x, middle_y,
            fields->mv[0].column, fields->mv[0].row);
        if (warp_status != AV1_WARP_OK) return AVIFDEC_INVALID_DATA;
        avifdec_memory_copy(fields->warp_params[0], model.matrix,
                            sizeof(fields->warp_params[0]));
        return AVIFDEC_OK;
    }
    fields->motion_mode = (uint8_t)av1_symbol_read(
        decoder, cdfs->inter.obmc[block_size], 2U);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    return AVIFDEC_OK;
}

static AvifdecStatus av1_tile_read_compound_syntax(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    uint8_t block_size) {
    static const uint8_t size_group[22] = {
        0U, 0U, 0U, 1U, 1U, 1U, 2U, 2U, 2U, 3U, 3U,
        3U, 3U, 3U, 3U, 3U, 0U, 0U, 1U, 1U, 2U, 2U
    };
    const int has_second_reference = fields->ref_frame[1] > 0U;

    fields->compound_index = 1U;
    fields->compound_type = 0U;
    if (config->enable_interintra_compound && !fields->skip_mode &&
        !has_second_reference && block_size >= 3U && block_size <= 9U &&
        fields->y_mode >= 13U && fields->y_mode <= 16U) {
        unsigned int group = size_group[block_size];

        fields->interintra = (uint8_t)av1_symbol_read(
            decoder, cdfs->inter.interintra[group], 2U);
        if (decoder->status != AVIFDEC_OK) return decoder->status;
        if (fields->interintra) {
            fields->interintra_mode = (uint8_t)av1_symbol_read(
                decoder, cdfs->inter.interintra_mode[group], 4U);
            if (decoder->status != AVIFDEC_OK) return decoder->status;
            if (av1_tile_wedge_allowed(block_size)) {
                fields->use_wedge_interintra = (uint8_t)av1_symbol_read(
                    decoder, cdfs->inter.wedge_interintra[block_size], 2U);
                if (decoder->status != AVIFDEC_OK) return decoder->status;
                if (fields->use_wedge_interintra) {
                    fields->interintra_wedge_index =
                        (uint8_t)av1_symbol_read(
                            decoder, cdfs->inter.wedge_index[block_size], 16U);
                    if (decoder->status != AVIFDEC_OK) {
                        return decoder->status;
                    }
                }
            }
        }
    }
    {
        AvifdecStatus status = av1_tile_read_motion_mode(
            decoder, cdfs, config, availability, fields, block_size);
        if (status != AVIFDEC_OK) return status;
    }
    if (!has_second_reference || fields->skip_mode) return AVIFDEC_OK;
    if (config->enable_masked_compound &&
        fields->width >= 2U && fields->height >= 2U) {
        unsigned int context = av1_tile_compound_group_context(
            config, availability, fields);

        fields->compound_group_index = (uint8_t)av1_symbol_read(
            decoder, cdfs->inter.compound_group_index[context], 2U);
        if (decoder->status != AVIFDEC_OK) return decoder->status;
    }
    if (fields->compound_group_index == 0U) {
        if (config->enable_dist_wtd_comp) {
            unsigned int context = av1_tile_compound_index_context(
                config, availability, fields);

            fields->compound_index = (uint8_t)av1_symbol_read(
                decoder, cdfs->inter.compound_index[context], 2U);
            if (decoder->status != AVIFDEC_OK) return decoder->status;
            fields->compound_type =
                fields->compound_index != 0U ? 0U : 1U;
        }
        return AVIFDEC_OK;
    }
    if (av1_tile_wedge_allowed(block_size)) {
        fields->compound_type = (uint8_t)(2U + av1_symbol_read(
            decoder, cdfs->inter.compound_type[block_size], 2U));
        if (decoder->status != AVIFDEC_OK) return decoder->status;
    } else {
        fields->compound_type = 3U;
    }
    if (fields->compound_type == 2U) {
        fields->wedge_index = (uint8_t)av1_symbol_read(
            decoder, cdfs->inter.wedge_index[block_size], 16U);
        fields->wedge_sign =
            (uint8_t)av1_symbol_read_literal(decoder, 1U);
    } else {
        fields->diff_mask_inverse =
            (uint8_t)av1_symbol_read_literal(decoder, 1U);
    }
    return decoder->status;
}

static int32_t av1_tile_clip_int32(int32_t minimum,
                                    int32_t maximum,
                                    int32_t value) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static AvifdecStatus av1_tile_decode_mode_block(
    void *user_data,
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    uint32_t row,
    uint32_t column,
    uint32_t width,
    uint32_t height) {
    Av1TileModeContext *context = (Av1TileModeContext *)user_data;
    const Av1TileModeConfig *config = &context->config;
    Av1BlockAvailability availability;
    Av1BlockTraceFields fields;
    uint8_t block_size;
    int cfl_allowed;
    AvifdecStatus status;

    status = av1_tile_find_block_size(width, height, &block_size);
    if (status != AVIFDEC_OK) return status;
    status = av1_block_state_availability(config->block_state, row, column,
                                          width, height, &availability);
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(&fields, 0U, sizeof(fields));
    fields.row = row;
    fields.column = column;
    fields.width = width;
    fields.height = height;
    fields.has_chroma = availability.has_chroma;
    if (row % config->superblock_mi == 0U &&
        column % config->superblock_mi == 0U) {
        context->read_deltas = config->delta_q_present;
    }
    if (config->segmentation_enabled && config->seg_id_pre_skip) {
        status = av1_tile_read_segment_id(
            decoder, cdfs, config->block_state, &availability, row, column,
            config->last_active_segment, 0, &fields.segment_id);
        if (status != AVIFDEC_OK) return status;
    }
    if (config->inter_frame) {
        size_t feature_offset = (size_t)fields.segment_id * 8U;
        int skip_mode_allowed =
            config->skip_mode_present && width >= 2U && height >= 2U;

        if (config->segmentation_enabled &&
            (config->feature_enabled[feature_offset + AV1_SEG_LVL_REF_FRAME] ||
             config->feature_enabled[feature_offset + AV1_SEG_LVL_SKIP] ||
             config->feature_enabled[feature_offset + AV1_SEG_LVL_GLOBALMV])) {
            skip_mode_allowed = 0;
        }
        status = av1_tile_read_skip_mode(
            decoder, cdfs, config->block_state, &availability, row, column,
            skip_mode_allowed, &fields.skip_mode);
        if (status != AVIFDEC_OK) return status;
    }
    status = av1_tile_read_skip(decoder, cdfs, config->block_state,
        &availability, row, column,
        fields.skip_mode ||
        (config->segmentation_enabled && config->seg_id_pre_skip &&
         config->feature_enabled[fields.segment_id * 8U + AV1_SEG_LVL_SKIP]),
        &fields.skip);
    if (status != AVIFDEC_OK) return status;
    if (config->segmentation_enabled && !config->seg_id_pre_skip) {
        status = av1_tile_read_segment_id(
            decoder, cdfs, config->block_state, &availability, row, column,
            config->last_active_segment, fields.skip, &fields.segment_id);
        if (status != AVIFDEC_OK) return status;
    }
    fields.lossless = config->segmentation_enabled
                      ? config->lossless_array[fields.segment_id]
                      : config->lossless;
    if (!fields.skip && !config->lossless && config->enable_cdef &&
        !config->allow_intrabc) {
        uint32_t cdef_row = row & ~15U;
        uint32_t cdef_column = column & ~15U;
        size_t cdef_index = (size_t)cdef_row *
                            config->block_state->mi_columns + cdef_column;

        if (config->cdef_indices == 0 ||
            cdef_index >= config->cdef_capacity) {
            return AVIFDEC_LIMIT_EXCEEDED;
        }
        if (config->cdef_indices[cdef_index] == 0xffU) {
            uint8_t cdef = (uint8_t)av1_symbol_read_literal(
                decoder, config->cdef_bits);
            uint32_t cdef_y;
            uint32_t cdef_x;

            if (decoder->status != AVIFDEC_OK) return decoder->status;
            for (cdef_y = cdef_row; cdef_y < row + height; cdef_y += 16U) {
                for (cdef_x = cdef_column; cdef_x < column + width;
                     cdef_x += 16U) {
                    size_t index;

                    if (cdef_y >= config->block_state->mi_rows ||
                        cdef_x >= config->block_state->mi_columns) {
                        continue;
                    }
                    index = (size_t)cdef_y *
                            config->block_state->mi_columns + cdef_x;
                    if (index >= config->cdef_capacity) {
                        return AVIFDEC_LIMIT_EXCEEDED;
                    }
                    config->cdef_indices[index] = cdef;
                }
            }
        }
    }
    if (!(width == config->superblock_mi &&
          height == config->superblock_mi && fields.skip)) {
        if (context->read_deltas) {
            int32_t delta;

            status = av1_tile_read_delta(decoder, cdfs->delta_q, &delta);
            if (status != AVIFDEC_OK) return status;
            if (delta != 0) {
                context->current_q_index = av1_tile_clip_int32(
                    1, 255, context->current_q_index +
                    (delta << config->delta_q_res));
            }
            fields.q_index = (uint8_t)context->current_q_index;
        }
        if (context->read_deltas && config->delta_lf_present) {
            unsigned int count = config->delta_lf_multi
                                 ? (config->monochrome ? 2U : 4U) : 1U;
            unsigned int index;

            for (index = 0U; index < count; ++index) {
                int32_t delta;

                status = av1_tile_read_delta(decoder, cdfs->delta_lf, &delta);
                if (status != AVIFDEC_OK) return status;
                if (delta != 0) {
                    context->delta_lf[index] = av1_tile_clip_int32(
                        -63, 63, context->delta_lf[index] +
                        (delta << config->delta_lf_res));
                }
            }
        }
    }
    context->read_deltas = 0U;
    avifdec_memory_copy(fields.delta_lf, context->delta_lf,
                        sizeof(fields.delta_lf));
    fields.q_index = (uint8_t)(config->delta_q_present
        ? context->current_q_index : config->base_q_index);
    if (config->segmentation_enabled &&
        config->feature_enabled[fields.segment_id * 8U + AV1_SEG_LVL_ALT_Q]) {
        fields.q_index = (uint8_t)av1_tile_clip_int32(
            0, 255, fields.q_index +
            config->feature_data[fields.segment_id * 8U + AV1_SEG_LVL_ALT_Q]);
    }
    if (config->allow_intrabc) {
        status = av1_tile_read_intrabc(
            decoder, cdfs, config, &availability, &fields);
        if (status != AVIFDEC_OK) return status;
    }
    if (!fields.use_intrabc && config->inter_frame) {
        status = av1_tile_read_is_inter(
            decoder, cdfs, config, &availability, &fields);
        if (status != AVIFDEC_OK) return status;
        if (fields.is_inter) {
            status = av1_tile_read_ref_frames(
                decoder, cdfs, config, &availability, &fields);
            if (status != AVIFDEC_OK) return status;
            status = av1_tile_read_inter_mode_and_mvs(
                decoder, cdfs, config, &availability, &fields);
            if (status != AVIFDEC_OK) return status;
            status = av1_tile_read_compound_syntax(
                decoder, cdfs, config, &availability, &fields, block_size);
            if (status != AVIFDEC_OK) return status;
            status = av1_tile_read_interp_filters(
                decoder, cdfs, config, &availability, &fields);
            if (status != AVIFDEC_OK) return status;
        }
        if (!fields.is_inter) {
            status = av1_tile_read_y_mode(
                decoder, cdfs, width, height, &fields.y_mode);
        }
    } else if (!fields.use_intrabc) {
        status = av1_tile_read_intra_frame_y_mode(
            decoder, cdfs, config->block_state, &availability, row, column,
            &fields.y_mode);
    }
    if (status != AVIFDEC_OK) return status;
    if (!fields.is_inter) {
        if (block_size >= 3U && fields.y_mode >= 1U && fields.y_mode <= 8U) {
            status = av1_tile_read_intra_angle_delta(
                decoder, cdfs, fields.y_mode, &fields.angle_delta_y);
            if (status != AVIFDEC_OK) return status;
        }
        if (availability.has_chroma) {
            cfl_allowed = fields.lossless
                  ? width <= ((uint32_t)1U <<
                          config->block_state->subsampling_x) &&
                    height <= ((uint32_t)1U <<
                           config->block_state->subsampling_y)
                          : width <= 8U && height <= 8U;
            fields.uv_mode = (uint8_t)av1_symbol_read(
                decoder,
                cfl_allowed
                    ? cdfs->intra.uv_mode_cfl_allowed[fields.y_mode]
                    : cdfs->intra.uv_mode_cfl_not_allowed[fields.y_mode],
                cfl_allowed ? AV1_UV_INTRA_MODES_CFL_ALLOWED
                            : AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED);
            if (decoder->status != AVIFDEC_OK) return decoder->status;
            if (fields.uv_mode == 13U) {
                status = av1_tile_read_cfl(
                    decoder, cdfs, &fields.cfl_alpha_u, &fields.cfl_alpha_v);
                if (status != AVIFDEC_OK) return status;
            } else if (block_size >= 3U && fields.uv_mode >= 1U &&
                       fields.uv_mode <= 8U) {
                status = av1_tile_read_intra_angle_delta(
                    decoder, cdfs, fields.uv_mode, &fields.angle_delta_uv);
                if (status != AVIFDEC_OK) return status;
            }
        }
        if (config->allow_screen_content_tools) {
            status = av1_tile_read_palette_info(
                decoder, cdfs, config, &availability, &fields, block_size);
            if (status != AVIFDEC_OK) return status;
        }
        if (config->enable_filter_intra && fields.y_mode == 0U &&
            fields.palette_size_y == 0U && width <= 8U && height <= 8U) {
            fields.use_filter_intra = (uint8_t)av1_symbol_read(
                decoder, cdfs->intra.filter_intra[block_size], 2U);
            if (decoder->status != AVIFDEC_OK) return decoder->status;
            if (fields.use_filter_intra) {
                fields.filter_intra_mode = (uint8_t)av1_symbol_read(
                    decoder, cdfs->intra.filter_intra_mode, 5U);
                if (decoder->status != AVIFDEC_OK) return decoder->status;
            }
        }
        status = av1_tile_read_palette_tokens(decoder, cdfs, config, &fields);
        if (status != AVIFDEC_OK) return status;
    }
    status = av1_tile_read_tx_size(decoder, cdfs, config, &availability,
                                   &fields, block_size);
    if (status != AVIFDEC_OK) return status;
    status = av1_block_state_record(config->block_state, &fields,
                                    config->disable_trace
                                        ? 0 : config->block_trace);
    if (status != AVIFDEC_OK) return status;
    if (config->before_residual != 0) {
        status = config->before_residual(config->user_data, decoder, &fields);
        if (status != AVIFDEC_OK) return status;
        return AVIFDEC_OK;
    }
    return AVIFDEC_UNSUPPORTED;
}

AvifdecStatus av1_tile_decode_modes(
    const Av1TilePartitionConfig *partition_config,
    const Av1TileModeConfig *mode_config,
    const Av1TileCdfs *frame_cdfs,
    Av1TileCdfs *tile_cdfs,
    Av1PartitionTrace *partition_trace) {
    Av1TilePartitionConfig config;
    Av1TileModeContext context;

    if (partition_config == 0 || mode_config == 0 ||
        mode_config->block_state == 0 || mode_config->block_trace == 0 ||
        mode_config->disable_trace > 1U ||
        mode_config->segmentation_enabled > 1U || mode_config->allow_intrabc > 1U ||
        mode_config->seg_id_pre_skip > 1U ||
        mode_config->last_active_segment >= 8U ||
        mode_config->allow_screen_content_tools > 1U ||
        mode_config->enable_filter_intra > 1U || mode_config->lossless > 1U ||
        mode_config->enable_cdef > 1U || mode_config->cdef_bits > 3U ||
        mode_config->tx_mode > 2U || mode_config->delta_q_present > 1U ||
        mode_config->delta_q_res > 3U || mode_config->delta_lf_present > 1U ||
        mode_config->delta_lf_res > 3U || mode_config->delta_lf_multi > 1U ||
        mode_config->monochrome > 1U ||
        (mode_config->bit_depth != 8U && mode_config->bit_depth != 10U &&
         mode_config->bit_depth != 12U) ||
        (mode_config->superblock_mi != 16U &&
         mode_config->superblock_mi != 32U) ||
        partition_config->decode_block != 0 ||
        partition_config->decode_mode_block != 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (mode_config->segmentation_enabled &&
        (mode_config->feature_enabled == 0 || mode_config->feature_data == 0 ||
         mode_config->lossless_array == 0)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (mode_config->block_state->mi_rows != partition_config->mi_rows ||
        mode_config->block_state->mi_columns != partition_config->mi_columns ||
        mode_config->block_state->tile_row_start != partition_config->tile_row_start ||
        mode_config->block_state->tile_row_end != partition_config->tile_row_end ||
        mode_config->block_state->tile_column_start !=
            partition_config->tile_column_start ||
        mode_config->block_state->tile_column_end !=
            partition_config->tile_column_end) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    config = *partition_config;
    avifdec_memory_fill(&context, 0U, sizeof(context));
    context.config = *mode_config;
    context.current_q_index = mode_config->base_q_index;
    context.read_deltas = mode_config->delta_q_present;
    config.decode_mode_block = av1_tile_decode_mode_block;
    config.user_data = &context;
    return av1_tile_decode_partitions(&config, frame_cdfs, tile_cdfs,
                                      partition_trace);
}

static const uint8_t av1_tx_size_sqr[AV1_TX_SIZES_ALL] = {
    0U, 1U, 2U, 3U, 4U, 0U, 0U, 1U, 1U, 2U,
    2U, 3U, 3U, 0U, 0U, 1U, 1U, 2U, 2U
};

static const uint8_t av1_tx_size_sqr_up[AV1_TX_SIZES_ALL] = {
    0U, 1U, 2U, 3U, 4U, 1U, 1U, 2U, 2U, 3U,
    3U, 4U, 4U, 2U, 2U, 3U, 3U, 4U, 4U
};

static const uint8_t av1_intra_tx_set1[7] = {
    AV1_TX_IDTX, AV1_TX_DCT_DCT, AV1_TX_V_DCT, AV1_TX_H_DCT,
    AV1_TX_ADST_ADST, AV1_TX_ADST_DCT, AV1_TX_DCT_ADST
};

static const uint8_t av1_intra_tx_set2[5] = {
    AV1_TX_IDTX, AV1_TX_DCT_DCT, AV1_TX_ADST_ADST,
    AV1_TX_ADST_DCT, AV1_TX_DCT_ADST
};

static const uint8_t av1_mode_to_tx_type[14] = {
    AV1_TX_DCT_DCT, AV1_TX_ADST_DCT, AV1_TX_DCT_ADST, AV1_TX_DCT_DCT,
    AV1_TX_ADST_ADST, AV1_TX_ADST_DCT, AV1_TX_DCT_ADST, AV1_TX_DCT_ADST,
    AV1_TX_ADST_DCT, AV1_TX_ADST_ADST, AV1_TX_ADST_DCT, AV1_TX_DCT_ADST,
    AV1_TX_ADST_ADST, AV1_TX_DCT_DCT
};

static unsigned int av1_tile_tx_set(const Av1TileResidualState *state,
                                     Av1TxSize tx_size,
                                     int is_inter) {
    if (av1_tx_size_sqr_up[tx_size] > AV1_TX_32X32) return 0U;
    if (is_inter) {
        if (state->reduced_tx_set ||
            av1_tx_size_sqr_up[tx_size] == AV1_TX_32X32) return 5U;
        if (av1_tx_size_sqr[tx_size] == AV1_TX_16X16) return 4U;
        return 3U;
    }
    if (av1_tx_size_sqr_up[tx_size] == AV1_TX_32X32) return 0U;
    if (state->reduced_tx_set || av1_tx_size_sqr[tx_size] == AV1_TX_16X16) {
        return 2U;
    }
    return 1U;
}

static int av1_tile_tx_type_in_set(unsigned int set, Av1TxType tx_type) {
    if (set == 0U) return tx_type == AV1_TX_DCT_DCT;
    if (set == 2U) {
        return tx_type == AV1_TX_DCT_DCT || tx_type == AV1_TX_ADST_DCT ||
               tx_type == AV1_TX_DCT_ADST || tx_type == AV1_TX_ADST_ADST ||
               tx_type == AV1_TX_IDTX;
    }
    if (set == 5U) {
        return tx_type == AV1_TX_DCT_DCT || tx_type == AV1_TX_IDTX;
    }
    if (set == 4U) {
        return tx_type == AV1_TX_IDTX || tx_type == AV1_TX_V_DCT ||
               tx_type == AV1_TX_H_DCT || tx_type == AV1_TX_DCT_DCT ||
               tx_type == AV1_TX_ADST_DCT || tx_type == AV1_TX_DCT_ADST ||
               tx_type == AV1_TX_FLIPADST_DCT ||
               tx_type == AV1_TX_DCT_FLIPADST ||
               tx_type == AV1_TX_ADST_ADST ||
               tx_type == AV1_TX_FLIPADST_FLIPADST ||
               tx_type == AV1_TX_ADST_FLIPADST ||
               tx_type == AV1_TX_FLIPADST_ADST;
    }
    if (set == 3U) return tx_type < AV1_TX_TYPES;
    return tx_type == AV1_TX_DCT_DCT || tx_type == AV1_TX_ADST_DCT ||
           tx_type == AV1_TX_DCT_ADST || tx_type == AV1_TX_ADST_ADST ||
           tx_type == AV1_TX_IDTX || tx_type == AV1_TX_V_DCT ||
           tx_type == AV1_TX_H_DCT;
}

static void av1_tile_residual_hash(Av1TileResidualState *state,
                                   uint64_t value) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        state->checksum ^= (uint8_t)(value >> (index * 8U));
        state->checksum *= (uint64_t)1099511628211ULL;
    }
}

static void av1_tile_checkpoint_hash(uint64_t *checksum, uint64_t value) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        *checksum ^= (uint8_t)(value >> (index * 8U));
        *checksum *= (uint64_t)1099511628211ULL;
    }
}

static AvifdecStatus av1_tile_store_tx_type(Av1TileResidualState *state,
                                             size_t x4,
                                             size_t y4,
                                             Av1TxSize tx_size,
                                             Av1TxType tx_type) {
    size_t width4 = av1_tx_size_info[tx_size].width >> 2;
    size_t height4 = av1_tx_size_info[tx_size].height >> 2;
    size_t row;
    size_t column;

    for (row = 0U; row < height4 && y4 + row < state->mi_rows; ++row) {
        for (column = 0U; column < width4 && x4 + column < state->mi_columns;
             ++column) {
            size_t index = (y4 + row) * state->mi_columns + x4 + column;
            if (index >= state->tx_type_capacity) return AVIFDEC_LIMIT_EXCEEDED;
            state->tx_types[index] = (uint8_t)tx_type;
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_tile_store_loop_filter_tx_size(
    Av1TileResidualState *state,
    unsigned int plane,
    size_t x4,
    size_t y4,
    Av1TxSize tx_size) {
    unsigned int sub_x = plane == 0U ? 0U : state->subsampling_x;
    unsigned int sub_y = plane == 0U ? 0U : state->subsampling_y;
    size_t start_x = x4 << sub_x;
    size_t start_y = y4 << sub_y;
    size_t width4 = (av1_tx_size_info[tx_size].width >> 2) << sub_x;
    size_t height4 = (av1_tx_size_info[tx_size].height >> 2) << sub_y;
    size_t row;
    size_t column;

    if (state->loop_filter_tx_sizes[plane] == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (row = 0U; row < height4 && start_y + row < state->mi_rows; ++row) {
        for (column = 0U;
             column < width4 && start_x + column < state->mi_columns;
             ++column) {
            size_t index = (start_y + row) * state->mi_columns +
                           start_x + column;
            if (index >= state->loop_filter_tx_size_capacity) {
                return AVIFDEC_LIMIT_EXCEEDED;
            }
            state->loop_filter_tx_sizes[plane][index] = (uint8_t)tx_size;
        }
    }
    return AVIFDEC_OK;
}

typedef struct {
    Av1TileResidualState *state;
    const Av1BlockTraceFields *block;
    size_t x4;
    size_t y4;
} Av1TileTxSelector;

static AvifdecStatus av1_tile_select_tx_type(void *user_data,
                                              Av1SymbolDecoder *decoder,
                                              Av1TxSize tx_size,
                                              Av1TxType *tx_type) {
    static const uint8_t filter_mode_to_intra[5] = { 0U, 1U, 2U, 6U, 0U };
    Av1TileTxSelector *selector = (Av1TileTxSelector *)user_data;
    Av1TileResidualState *state = selector->state;
    static const uint8_t inter_set1[16] = {
        AV1_TX_IDTX, AV1_TX_V_DCT, AV1_TX_H_DCT, AV1_TX_V_ADST,
        AV1_TX_H_ADST, AV1_TX_V_FLIPADST, AV1_TX_H_FLIPADST,
        AV1_TX_DCT_DCT, AV1_TX_ADST_DCT, AV1_TX_DCT_ADST,
        AV1_TX_FLIPADST_DCT, AV1_TX_DCT_FLIPADST, AV1_TX_ADST_ADST,
        AV1_TX_FLIPADST_FLIPADST, AV1_TX_ADST_FLIPADST,
        AV1_TX_FLIPADST_ADST
    };
    static const uint8_t inter_set2[12] = {
        AV1_TX_IDTX, AV1_TX_V_DCT, AV1_TX_H_DCT, AV1_TX_DCT_DCT,
        AV1_TX_ADST_DCT, AV1_TX_DCT_ADST, AV1_TX_FLIPADST_DCT,
        AV1_TX_DCT_FLIPADST, AV1_TX_ADST_ADST,
        AV1_TX_FLIPADST_FLIPADST, AV1_TX_ADST_FLIPADST,
        AV1_TX_FLIPADST_ADST
    };
    static const uint8_t inter_set3[2] = { AV1_TX_IDTX, AV1_TX_DCT_DCT };
    unsigned int set = av1_tile_tx_set(
        state, tx_size, selector->block->is_inter);
    unsigned int intra_mode = selector->block->use_filter_intra
                              ? filter_mode_to_intra[selector->block->filter_intra_mode]
                              : selector->block->y_mode;
    uint32_t symbol;

    *tx_type = AV1_TX_DCT_DCT;
    if (set == 0U || selector->block->q_index == 0U) {
        return av1_tile_store_tx_type(state, selector->x4, selector->y4,
                                      tx_size, *tx_type);
    }
    if (selector->block->is_inter) {
        if (set == 3U) {
            symbol = av1_symbol_read(
                decoder,
                state->cdfs->inter.tx_type_set1[av1_tx_size_sqr[tx_size]],
                16U);
            *tx_type = (Av1TxType)inter_set1[symbol];
        } else if (set == 4U) {
            symbol = av1_symbol_read(
                decoder, state->cdfs->inter.tx_type_set2, 12U);
            *tx_type = (Av1TxType)inter_set2[symbol];
        } else if (set == 5U) {
            symbol = av1_symbol_read(
                decoder,
                state->cdfs->inter.tx_type_set3[av1_tx_size_sqr[tx_size]],
                2U);
            *tx_type = (Av1TxType)inter_set3[symbol];
        }
    } else if (intra_mode >= AV1_INTRA_MODES) {
        return AVIFDEC_INVALID_DATA;
    } else if (set == 1U) {
        symbol = av1_symbol_read(
            decoder,
            state->cdfs->intra.tx_type_set1[av1_tx_size_sqr[tx_size]][intra_mode],
            7U);
        if (symbol >= 7U) return AVIFDEC_INVALID_DATA;
        *tx_type = (Av1TxType)av1_intra_tx_set1[symbol];
    } else {
        symbol = av1_symbol_read(
            decoder,
            state->cdfs->intra.tx_type_set2[av1_tx_size_sqr[tx_size]][intra_mode],
            5U);
        if (symbol >= 5U) return AVIFDEC_INVALID_DATA;
        *tx_type = (Av1TxType)av1_intra_tx_set2[symbol];
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    return av1_tile_store_tx_type(state, selector->x4, selector->y4,
                                  tx_size, *tx_type);
}

static AvifdecStatus av1_tile_reset_block_context(
    Av1TileResidualState *state,
    const Av1BlockTraceFields *block) {
    unsigned int plane_count = block->has_chroma ? 3U : 1U;
    unsigned int plane;

    for (plane = 0U; plane < plane_count; ++plane) {
        Av1CoeffPlaneContext *context = &state->coeff_contexts->plane[plane];
        unsigned int sub_x = plane == 0U ? 0U : state->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : state->subsampling_y;
        size_t start_x = block->column >> sub_x;
        size_t end_x = (block->column + block->width) >> sub_x;
        size_t start_y = block->row >> sub_y;
        size_t end_y = (block->row + block->height) >> sub_y;
        size_t index;

        if (end_x > context->width4) end_x = context->width4;
        if (end_y > context->height4) end_y = context->height4;
        for (index = start_x; index < end_x; ++index) {
            context->above_level_context[index] = 0U;
            context->above_dc_context[index] = 0U;
        }
        for (index = start_y; index < end_y; ++index) {
            context->left_level_context[index] = 0U;
            context->left_dc_context[index] = 0U;
        }
    }
    return AVIFDEC_OK;
}

static Av1TxSize av1_tile_chroma_tx_size(uint32_t width4,
                                          uint32_t height4) {
    uint8_t block_size;
    Av1TxSize tx_size;

    if (av1_tile_find_block_size(width4, height4, &block_size) != AVIFDEC_OK) {
        return AV1_TX_SIZES_ALL;
    }
    tx_size = (Av1TxSize)av1_max_tx_size[block_size];
    if (av1_tx_size_info[tx_size].width == 64U ||
        av1_tx_size_info[tx_size].height == 64U) {
        if (av1_tx_size_info[tx_size].width == 16U) return AV1_TX_16X32;
        if (av1_tx_size_info[tx_size].height == 16U) return AV1_TX_32X16;
        return AV1_TX_32X32;
    }
    return tx_size;
}

static const Av1BlockCell *av1_tile_cell_at(const Av1BlockState *state,
                                            int64_t row,
                                            int64_t column) {
    size_t index;
    if (!av1_block_state_is_inside(state, row, column)) return 0;
    index = (size_t)row * state->mi_columns + (size_t)column;
    if (index >= state->cell_capacity || state->cells[index].width == 0U) return 0;
    return &state->cells[index];
}

static int av1_tile_mode_is_smooth(uint8_t mode) {
    return mode == 9U || mode == 10U || mode == 11U;
}

static uint8_t av1_tile_intra_filter_type(const Av1TileResidualState *state,
                                          const Av1BlockTraceFields *block,
                                          unsigned int plane,
                                          const Av1BlockAvailability *availability) {
    unsigned int sub_x = plane == 0U ? 0U : state->subsampling_x;
    unsigned int sub_y = plane == 0U ? 0U : state->subsampling_y;
    int64_t base_row = (int64_t)(block->row & ~sub_y);
    int64_t base_column = (int64_t)(block->column & ~sub_x);
    int above_smooth = 0;
    int left_smooth = 0;

    if (plane == 0U ? availability->above : availability->above_chroma) {
        int64_t row = base_row - 1;
        int64_t column = base_column + sub_x;
        const Av1BlockCell *cell;
        cell = av1_tile_cell_at(state->block_state, row, column);
        if (cell != 0) above_smooth = av1_tile_mode_is_smooth(
            plane == 0U ? cell->y_mode : cell->uv_mode);
    }
    if (plane == 0U ? availability->left : availability->left_chroma) {
        int64_t row = base_row + sub_y;
        int64_t column = base_column - 1;
        const Av1BlockCell *cell;
        cell = av1_tile_cell_at(state->block_state, row, column);
        if (cell != 0) left_smooth = av1_tile_mode_is_smooth(
            plane == 0U ? cell->y_mode : cell->uv_mode);
    }
    return (uint8_t)(above_smooth || left_smooth);
}

static AvifdecStatus av1_tile_predict_chunk(
    Av1TileResidualState *state,
    const Av1BlockTraceFields *block,
    unsigned int plane,
    uint32_t plane_offset_x4,
    uint32_t plane_offset_y4,
    uint32_t plane_width4,
    uint32_t plane_height4) {
    static const uint16_t directional_angle[9] = {
        0U, 90U, 180U, 45U, 135U, 113U, 157U, 203U, 67U
    };
    Av1BlockAvailability availability;
    Av1PreparedReferences prepared;
    uint16_t *plane_data = state->frame_planes.data[plane];
    size_t stride = state->frame_planes.stride[plane];
    unsigned int sub_x = plane == 0U ? 0U : state->subsampling_x;
    unsigned int sub_y = plane == 0U ? 0U : state->subsampling_y;
    uint32_t x = ((block->column >> sub_x) + plane_offset_x4) << 2;
    uint32_t y = ((block->row >> sub_y) + plane_offset_y4) << 2;
    uint32_t width = plane_width4 << 2;
    uint32_t height = plane_height4 << 2;
    uint32_t block_base_column = block->column;
    uint32_t block_base_row = block->row;
    uint8_t mode = plane == 0U ? block->y_mode : block->uv_mode;
    int8_t angle_delta = plane == 0U ? block->angle_delta_y
                                     : block->angle_delta_uv;
    uint8_t have_above;
    uint8_t have_left;
    uint8_t have_above_right = 0U;
    uint8_t have_below_left = 0U;
    uint8_t filter_type;
    uint16_t *destination;
    AvifdecStatus status;

    if (plane != 0U) {
        block_base_column &= ~sub_x;
        block_base_row &= ~sub_y;
    }
    if (width < 4U) width += 2U;
    if (height < 4U) height += 2U;
    if (plane_data == 0 || x > state->frame_planes.width[plane] ||
        width > state->frame_planes.width[plane] - x ||
        y > state->frame_planes.height[plane] ||
        height > state->frame_planes.height[plane] - y) {
        return AVIFDEC_LIMIT_EXCEEDED;
    }
    status = av1_block_state_availability(
        state->block_state, block->row, block->column,
        block->width, block->height, &availability);
    if (status != AVIFDEC_OK) return status;
    have_above = plane_offset_y4 != 0U ||
        (plane == 0U ? availability.above : availability.above_chroma);
    have_left = plane_offset_x4 != 0U ||
        (plane == 0U ? availability.left : availability.left_chroma);
    if (have_above) {
        int64_t reference_row = (int64_t)block_base_row +
                                (int64_t)(plane_offset_y4 << sub_y) - 1;
        int64_t reference_column = (int64_t)block_base_column +
                                   (int64_t)((plane_offset_x4 +
                                       plane_width4) << sub_x);
        if (plane_offset_y4 != 0U) {
            uint32_t block_width4 =
                (block->width + sub_x) >> sub_x;
            have_above_right = plane_offset_x4 + 2U * plane_width4 <=
                               block_width4;
        } else {
            have_above_right =
                av1_tile_cell_at(state->block_state, reference_row,
                                 reference_column) != 0;
        }
    }
    if (have_left) {
        int64_t reference_row = (int64_t)block_base_row +
                                (int64_t)((plane_offset_y4 +
                                    plane_height4) << sub_y);
        int64_t reference_column = (int64_t)block_base_column +
                                   (int64_t)(plane_offset_x4 << sub_x) - 1;
        have_below_left = plane_offset_x4 == 0U &&
            av1_tile_cell_at(state->block_state, reference_row,
                             reference_column) != 0;
    }
    status = av1_predict_prepare_references(
        plane_data, stride, state->frame_planes.width[plane],
        state->frame_planes.height[plane],
        (uint32_t)(((state->mi_columns + sub_x) >> sub_x) << 2),
        (uint32_t)(((state->mi_rows + sub_y) >> sub_y) << 2),
        x, y, width, height,
        state->dequant_params.bit_depth, have_above, have_left,
        have_above_right, have_below_left, &prepared);
    if (status != AVIFDEC_OK) return status;
    destination = plane_data + (size_t)y * stride + x;
    if (plane == 0U && block->palette_size_y != 0U) {
        size_t map_stride = (size_t)block->width << 2;
        size_t map_offset = (size_t)(plane_offset_y4 << 2) * map_stride +
                            (plane_offset_x4 << 2);
        return av1_predict_palette(destination, stride, width, height,
                                   state->dequant_params.bit_depth,
                                   block->palette_colors_y,
                                   block->palette_size_y,
                                   state->palette_map_y + map_offset,
                                   map_stride);
    }
    if (plane != 0U && block->palette_size_uv != 0U) {
        size_t map_stride = ((size_t)block->width << 2) >> state->subsampling_x;
        size_t map_x = (size_t)plane_offset_x4 << 2;
        size_t map_y = (size_t)plane_offset_y4 << 2;
        const uint16_t *colors = plane == 1U ? block->palette_colors_u
                                             : block->palette_colors_v;
        if (map_stride < 4U) map_stride += 2U;
        return av1_predict_palette(destination, stride, width, height,
                                   state->dequant_params.bit_depth, colors,
                                   block->palette_size_uv,
                                   state->palette_map_uv + map_y * map_stride + map_x,
                                   map_stride);
    }
    if (plane == 0U && block->use_filter_intra != 0U) {
        return av1_predict_filter_intra(destination, stride, width, height,
                                        state->dequant_params.bit_depth,
                                        block->filter_intra_mode,
                                        &prepared.references);
    }
    if (mode == 0U || mode == 13U) {
        status = av1_predict_dc(destination, stride, width, height,
                                state->dequant_params.bit_depth,
                                &prepared.references);
        if (status != AVIFDEC_OK || mode != 13U) return status;
        return av1_predict_cfl(
            destination, stride,
            state->frame_planes.data[0], state->frame_planes.stride[0],
            (uint32_t)(state->mi_columns << 2),
            (uint32_t)(state->mi_rows << 2),
            x << state->subsampling_x, y << state->subsampling_y,
            width, height,
            state->subsampling_x, state->subsampling_y,
            plane == 1U ? block->cfl_alpha_u : block->cfl_alpha_v,
            state->dequant_params.bit_depth);
    }
    if (mode >= 1U && mode <= 8U) {
        filter_type = av1_tile_intra_filter_type(
            state, block, plane, &availability);
        return av1_predict_directional_edges(
            destination, stride, width, height,
            state->dequant_params.bit_depth,
            (uint16_t)(directional_angle[mode] + 3 * angle_delta),
            filter_type, &prepared);
    }
    if (mode == 9U) {
        return av1_predict_nondirectional(destination, stride, width, height,
            state->dequant_params.bit_depth, AV1_PREDICT_SMOOTH,
            &prepared.references);
    }
    if (mode == 10U) {
        return av1_predict_nondirectional(destination, stride, width, height,
            state->dequant_params.bit_depth, AV1_PREDICT_SMOOTH_VERTICAL,
            &prepared.references);
    }
    if (mode == 11U) {
        return av1_predict_nondirectional(destination, stride, width, height,
            state->dequant_params.bit_depth, AV1_PREDICT_SMOOTH_HORIZONTAL,
            &prepared.references);
    }
    if (mode == 12U) {
        return av1_predict_nondirectional(destination, stride, width, height,
            state->dequant_params.bit_depth, AV1_PREDICT_PAETH,
            &prepared.references);
    }
    return AVIFDEC_INVALID_DATA;
}

AvifdecStatus av1_tile_residual_state_init(
    Av1TileResidualState *state,
    Av1CoeffContextState *coeff_contexts,
    Av1TileCdfs *cdfs,
    uint8_t *tx_types,
    size_t tx_type_capacity,
    uint8_t *loop_filter_tx_sizes[3],
    size_t loop_filter_tx_size_capacity,
    uint32_t mi_rows,
    uint32_t mi_columns,
    int monochrome,
    int subsampling_x,
    int subsampling_y,
    int reduced_tx_set,
    uint8_t base_q_index,
    int lossless,
    int32_t *quantized,
    size_t quantized_capacity,
    int32_t *dequantized,
    size_t dequantized_capacity,
    int32_t *residual,
    size_t residual_capacity,
    const Av1DequantParams *dequant_params,
    uint8_t qm_y,
    uint8_t qm_u,
    uint8_t qm_v,
    Av1BlockState *block_state,
    const Av1FramePlanes *frame_planes,
    uint8_t *palette_map_y,
    uint8_t *palette_map_uv,
    size_t palette_map_capacity) {
    size_t cells;
    uint32_t row;
    uint32_t column;

    if (state == 0 || coeff_contexts == 0 || cdfs == 0 || tx_types == 0 ||
        loop_filter_tx_sizes == 0 || loop_filter_tx_sizes[0] == 0 ||
        (!monochrome &&
         (loop_filter_tx_sizes[1] == 0 || loop_filter_tx_sizes[2] == 0)) ||
        quantized == 0 || dequantized == 0 || residual == 0 ||
        dequant_params == 0 || quantized_capacity < 1024U ||
        dequantized_capacity < 1024U || residual_capacity < 4096U ||
        qm_y > 15U || qm_u > 15U || qm_v > 15U ||
        block_state == 0 || frame_planes == 0 ||
        palette_map_y == 0 || palette_map_uv == 0 ||
        palette_map_capacity < 64U * 64U ||
        mi_rows == 0U || mi_columns == 0U ||
        (monochrome != 0 && monochrome != 1) ||
        (subsampling_x != 0 && subsampling_x != 1) ||
        (subsampling_y != 0 && subsampling_y != 1) ||
        (reduced_tx_set != 0 && reduced_tx_set != 1) ||
        (lossless != 0 && lossless != 1) ||
        block_state->tile_row_start > block_state->tile_row_end ||
        block_state->tile_row_end > mi_rows ||
        block_state->tile_column_start > block_state->tile_column_end ||
        block_state->tile_column_end > mi_columns ||
        !avifdec_size_multiply(mi_rows, mi_columns, &cells)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (cells > tx_type_capacity || cells > loop_filter_tx_size_capacity) {
        return AVIFDEC_LIMIT_EXCEEDED;
    }
    avifdec_memory_fill(state, 0U, sizeof(*state));
    for (row = block_state->tile_row_start;
         row < block_state->tile_row_end; ++row) {
        for (column = block_state->tile_column_start;
             column < block_state->tile_column_end; ++column) {
            size_t index = (size_t)row * mi_columns + column;
            tx_types[index] = AV1_TX_DCT_DCT;
            loop_filter_tx_sizes[0][index] = AV1_TX_4X4;
            if (!monochrome) {
                loop_filter_tx_sizes[1][index] = AV1_TX_4X4;
                loop_filter_tx_sizes[2][index] = AV1_TX_4X4;
            }
        }
    }
    state->coeff_contexts = coeff_contexts;
    state->cdfs = cdfs;
    state->tx_types = tx_types;
    state->tx_type_capacity = tx_type_capacity;
    state->loop_filter_tx_sizes[0] = loop_filter_tx_sizes[0];
    state->loop_filter_tx_sizes[1] = loop_filter_tx_sizes[1];
    state->loop_filter_tx_sizes[2] = loop_filter_tx_sizes[2];
    state->loop_filter_tx_size_capacity = loop_filter_tx_size_capacity;
    state->mi_rows = mi_rows;
    state->mi_columns = mi_columns;
    state->monochrome = (uint8_t)monochrome;
    state->subsampling_x = (uint8_t)subsampling_x;
    state->subsampling_y = (uint8_t)subsampling_y;
    state->reduced_tx_set = (uint8_t)reduced_tx_set;
    state->base_q_index = base_q_index;
    state->lossless = (uint8_t)lossless;
    state->checksum = (uint64_t)1469598103934665603ULL;
    state->predictor_checksum = (uint64_t)1469598103934665603ULL;
    state->quantized_checksum = (uint64_t)1469598103934665603ULL;
    state->dequantized_checksum = (uint64_t)1469598103934665603ULL;
    state->residual_checksum = (uint64_t)1469598103934665603ULL;
    state->quantized = quantized;
    state->dequantized = dequantized;
    state->residual = residual;
    state->quantized_capacity = quantized_capacity;
    state->dequantized_capacity = dequantized_capacity;
    state->residual_capacity = residual_capacity;
    state->dequant_params = *dequant_params;
    state->qm_y = qm_y;
    state->qm_u = qm_u;
    state->qm_v = qm_v;
    state->block_state = block_state;
    state->frame_planes = *frame_planes;
    state->palette_map_y = palette_map_y;
    state->palette_map_uv = palette_map_uv;
    state->palette_map_capacity = palette_map_capacity;
    return AVIFDEC_OK;
}

static AvifdecStatus av1_tile_record_inter_tx_leaf(
    Av1TileResidualState *state,
    const Av1BlockTraceFields *block,
    uint32_t offset_x4,
    uint32_t offset_y4,
    Av1TxSize tx_size) {
    const uint32_t width4 = av1_tx_size_info[tx_size].width >> 2;
    const uint32_t height4 = av1_tx_size_info[tx_size].height >> 2;
    uint32_t y;

    if (offset_x4 >= block->width || offset_y4 >= block->height) {
        return AVIFDEC_OK;
    }
    for (y = 0U; y < height4 && offset_y4 + y < block->height; ++y) {
        uint32_t x;
        for (x = 0U; x < width4 && offset_x4 + x < block->width; ++x) {
            const uint32_t row = block->row + offset_y4 + y;
            const uint32_t column = block->column + offset_x4 + x;
            const size_t index = (size_t)row * state->mi_columns + column;

            if (row >= state->mi_rows || column >= state->mi_columns) {
                continue;
            }
            if (index >= state->block_state->cell_capacity) {
                return AVIFDEC_LIMIT_EXCEEDED;
            }
            state->block_state->cells[index].tx_size = (uint8_t)tx_size;
        }
    }
    {
        const uint32_t row = block->row + offset_y4;
        const uint32_t column = block->column + offset_x4;
        const size_t index = (size_t)row * state->mi_columns + column;

        if (row < state->mi_rows && column < state->mi_columns) {
            if (index >= state->tx_type_capacity) {
                return AVIFDEC_LIMIT_EXCEEDED;
            }
            state->tx_types[index] = 0xffU;
        }
    }
    return AVIFDEC_OK;
}

static unsigned int av1_tile_txfm_partition_context(
    const Av1TileResidualState *state,
    const Av1BlockTraceFields *block,
    uint32_t offset_x4,
    uint32_t offset_y4,
    Av1TxSize tx_size) {
    const Av1BlockCell *above = av1_tile_cell_at(
        state->block_state, (int64_t)block->row + offset_y4 - 1,
        (int64_t)block->column + offset_x4);
    const Av1BlockCell *left = av1_tile_cell_at(
        state->block_state, (int64_t)block->row + offset_y4,
        (int64_t)block->column + offset_x4 - 1);
    uint32_t maximum = block->width > block->height
                       ? block->width : block->height;
    unsigned int maximum_square = 0U;
    unsigned int category;
    unsigned int context;

    while (maximum > 1U && maximum_square < AV1_TX_64X64) {
        maximum >>= 1;
        ++maximum_square;
    }
    if (maximum_square > AV1_TX_64X64) maximum_square = AV1_TX_64X64;
    category =
        (av1_tx_size_sqr_up[tx_size] != maximum_square &&
         maximum_square > AV1_TX_8X8) +
        (AV1_TX_64X64 - maximum_square) * 2U;
    context = category * 3U;
    if (above != 0 &&
        (above->tx_size >= AV1_TX_SIZES_ALL ||
         av1_tx_size_info[above->tx_size].width <
             av1_tx_size_info[tx_size].width)) {
        ++context;
    }
    if (left != 0 &&
        (left->tx_size >= AV1_TX_SIZES_ALL ||
         av1_tx_size_info[left->tx_size].height <
             av1_tx_size_info[tx_size].height)) {
        ++context;
    }
    return context;
}

static AvifdecStatus av1_tile_read_inter_tx_partition(
    Av1TileResidualState *state,
    Av1SymbolDecoder *decoder,
    const Av1BlockTraceFields *block,
    Av1TxSize tx_size,
    unsigned int depth,
    uint32_t offset_x4,
    uint32_t offset_y4) {
    Av1TxSize sub_tx_size;
    uint32_t sub_width4;
    uint32_t sub_height4;
    uint32_t y;

    if (offset_x4 >= block->width || offset_y4 >= block->height) {
        return AVIFDEC_OK;
    }
    if (depth == 2U) {
        return av1_tile_record_inter_tx_leaf(
            state, block, offset_x4, offset_y4, tx_size);
    }
    {
        unsigned int context = av1_tile_txfm_partition_context(
            state, block, offset_x4, offset_y4, tx_size);
        uint32_t split;

        if (context >= 21U) return AVIFDEC_INVALID_DATA;
        split = av1_symbol_read(
            decoder, state->cdfs->txfm_partition[context], 2U);
        if (decoder->status != AVIFDEC_OK) return decoder->status;
        if (split == 0U) {
            return av1_tile_record_inter_tx_leaf(
                state, block, offset_x4, offset_y4, tx_size);
        }
    }
    sub_tx_size = (Av1TxSize)av1_split_tx_size[tx_size];
    sub_width4 = av1_tx_size_info[sub_tx_size].width >> 2;
    sub_height4 = av1_tx_size_info[sub_tx_size].height >> 2;
    if (sub_tx_size == AV1_TX_4X4) {
        const uint32_t parent_width4 =
            av1_tx_size_info[tx_size].width >> 2;
        const uint32_t parent_height4 =
            av1_tx_size_info[tx_size].height >> 2;

        for (y = 0U; y < parent_height4; ++y) {
            uint32_t x;
            for (x = 0U; x < parent_width4; ++x) {
                AvifdecStatus status = av1_tile_record_inter_tx_leaf(
                    state, block, offset_x4 + x, offset_y4 + y,
                    AV1_TX_4X4);
                if (status != AVIFDEC_OK) return status;
            }
        }
        return AVIFDEC_OK;
    }
    for (y = 0U; y < (av1_tx_size_info[tx_size].height >> 2);
         y += sub_height4) {
        uint32_t x;
        for (x = 0U; x < (av1_tx_size_info[tx_size].width >> 2);
             x += sub_width4) {
            AvifdecStatus status = av1_tile_read_inter_tx_partition(
                state, decoder, block, sub_tx_size, depth + 1U,
                offset_x4 + x, offset_y4 + y);
            if (status != AVIFDEC_OK) return status;
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_tile_read_inter_tx_partitions(
    Av1TileResidualState *state,
    Av1SymbolDecoder *decoder,
    const Av1BlockTraceFields *block) {
    const Av1TxSize max_tx_size = (Av1TxSize)block->tx_size;
    const uint32_t width4 = av1_tx_size_info[max_tx_size].width >> 2;
    const uint32_t height4 = av1_tx_size_info[max_tx_size].height >> 2;
    uint32_t y;

    for (y = 0U; y < block->height; y += height4) {
        uint32_t x;
        for (x = 0U; x < block->width; x += width4) {
            AvifdecStatus status = av1_tile_read_inter_tx_partition(
                state, decoder, block, max_tx_size, 0U, x, y);
            if (status != AVIFDEC_OK) return status;
        }
    }
    return AVIFDEC_OK;
}

static void av1_tile_copy_prediction(uint16_t *destination,
                                     size_t destination_stride,
                                     const uint16_t *source,
                                     size_t source_stride,
                                     uint32_t width,
                                     uint32_t height) {
    uint32_t y;

    for (y = 0U; y < height; ++y) {
        avifdec_memory_copy(
            destination + (size_t)y * destination_stride,
            source + (size_t)y * source_stride,
            width * sizeof(uint16_t));
    }
}

static void av1_tile_distance_weights(
    const Av1TileResidualState *state,
    const Av1BlockTraceFields *block,
    uint8_t *weight0,
    uint8_t *weight1) {
    static const uint8_t quant_dist_weight[4][2] = {
        { 2U, 3U }, { 2U, 5U }, { 2U, 7U }, { 1U, 31U }
    };
    static const uint8_t lookup[4][2] = {
        { 9U, 7U }, { 11U, 5U }, { 12U, 4U }, { 13U, 3U }
    };
    int32_t d0 = av1_relative_distance(
        state->enable_order_hint, state->order_hint_bits,
        state->reference_order_hint[block->ref_frame[1] - 1U],
        state->current_order_hint);
    int32_t d1 = av1_relative_distance(
        state->enable_order_hint, state->order_hint_bits,
        state->current_order_hint,
        state->reference_order_hint[block->ref_frame[0] - 1U]);
    unsigned int order;
    unsigned int index;

    if (d0 < 0) d0 = -d0;
    if (d1 < 0) d1 = -d1;
    if (d0 > 31) d0 = 31;
    if (d1 > 31) d1 = 31;
    order = d0 <= d1;
    if (d0 == 0 || d1 == 0) {
        index = 3U;
    } else {
        for (index = 0U; index < 3U; ++index) {
            int32_t first =
                d0 * quant_dist_weight[index][order];
            int32_t second =
                d1 * quant_dist_weight[index][1U - order];
            if ((d0 > d1 && first < second) ||
                (d0 <= d1 && first > second)) {
                break;
            }
        }
    }
    *weight0 = lookup[index][order];
    *weight1 = lookup[index][1U - order];
}

static void av1_tile_subsample_mask(
    uint8_t *mask,
    uint32_t luma_width,
    unsigned int sub_x,
    unsigned int sub_y,
    uint32_t plane_width,
    uint32_t plane_height) {
    uint32_t y;
    if (sub_x == 0U && sub_y == 0U) return;
    for (y = 0U; y < plane_height; ++y) {
        uint32_t x;
        for (x = 0U; x < plane_width; ++x) {
            const uint32_t source_x = x << sub_x;
            const uint32_t source_y = y << sub_y;
            unsigned int value =
                mask[(size_t)source_y * luma_width + source_x];

            if (sub_x) {
                value += mask[
                    (size_t)source_y * luma_width + source_x + 1U];
            }
            if (sub_y) {
                value += mask[
                    (size_t)(source_y + 1U) * luma_width + source_x];
                if (sub_x) {
                    value += mask[
                        (size_t)(source_y + 1U) * luma_width +
                        source_x + 1U];
                }
            }
            if (sub_x && sub_y) value = (value + 2U) >> 2;
            else value = (value + 1U) >> 1;
            mask[(size_t)y * plane_width + x] = (uint8_t)value;
        }
    }
}

static AvifdecStatus av1_tile_build_wedge_plane_mask(
    Av1TileResidualState *state,
    const Av1BlockTraceFields *block,
    uint8_t wedge_index,
    uint8_t wedge_sign,
    unsigned int sub_x,
    unsigned int sub_y,
    uint32_t plane_width,
    uint32_t plane_height) {
    const uint32_t luma_width = block->width << 2;
    const uint32_t luma_height = block->height << 2;
    AvifdecStatus status;

    if ((size_t)luma_width * luma_height > state->inter_scratch_capacity) {
        return AVIFDEC_LIMIT_EXCEEDED;
    }
    status = av1_inter_build_wedge_mask(
        state->inter_mask, luma_width, luma_width, luma_height,
        wedge_index, wedge_sign);
    if (status != AVIFDEC_OK) return status;
    av1_tile_subsample_mask(
        state->inter_mask, luma_width, sub_x, sub_y,
        plane_width, plane_height);
    return AVIFDEC_OK;
}

static uint32_t av1_tile_obmc_neighbor_limit(uint32_t blocks4) {
    uint32_t log2 = 0U;
    while (blocks4 > 1U) {
        blocks4 >>= 1;
        ++log2;
    }
    return log2 < 4U ? log2 : 4U;
}

static uint32_t av1_tile_clip_obmc_step(uint32_t step4) {
    if (step4 < 2U) return 2U;
    if (step4 > 16U) return 16U;
    return step4;
}

static AvifdecStatus av1_tile_predict_obmc_neighbor(
    Av1TileResidualState *state,
    const Av1BlockCell *cell,
    unsigned int plane,
    unsigned int sub_x,
    unsigned int sub_y,
    uint32_t pred_x,
    uint32_t pred_y,
    uint32_t pred_width,
    uint32_t pred_height) {
    const uint8_t reference = cell->ref_frame[0];
    const unsigned int reference_index = reference - 1U;
    Av1InterPredictParams prediction;

    if (reference == 0U || reference > 7U ||
        !state->reference_pixels_valid[reference_index] ||
        (size_t)pred_width * pred_height > state->inter_scratch_capacity) {
        return AVIFDEC_INVALID_DATA;
    }
    avifdec_memory_fill(&prediction, 0U, sizeof(prediction));
    prediction.src =
        state->reference_planes[reference_index].data[plane];
    prediction.src_stride =
        state->reference_planes[reference_index].stride[plane];
    prediction.dst = state->inter_pred0;
    prediction.dst_stride = pred_width;
    prediction.src_width =
        (state->reference_width[reference_index] +
         ((1U << sub_x) - 1U)) >> sub_x;
    prediction.src_height =
        (state->reference_height[reference_index] +
         ((1U << sub_y) - 1U)) >> sub_y;
    prediction.frame_width =
        (state->current_frame_width + ((1U << sub_x) - 1U)) >> sub_x;
    prediction.frame_height =
        (state->current_frame_height + ((1U << sub_y) - 1U)) >> sub_y;
    prediction.block_x = pred_x;
    prediction.block_y = pred_y;
    prediction.block_width = pred_width;
    prediction.block_height = pred_height;
    prediction.mv_col = cell->mv[0].column;
    prediction.mv_row = cell->mv[0].row;
    prediction.subsampling_x = (uint8_t)sub_x;
    prediction.subsampling_y = (uint8_t)sub_y;
    prediction.bit_depth = state->dequant_params.bit_depth;
    prediction.filter_x = cell->interp_filter[1];
    prediction.filter_y = cell->interp_filter[0];
    if (prediction.src == NULL || prediction.src_width == 0U ||
        prediction.src_height == 0U) {
        return AVIFDEC_INVALID_DATA;
    }
    return av1_inter_predict_single(&prediction);
}

static AvifdecStatus av1_tile_apply_obmc(
    Av1TileResidualState *state,
    const Av1BlockTraceFields *block,
    unsigned int plane,
    uint16_t *destination,
    uint32_t nominal_width,
    uint32_t nominal_height,
    uint32_t visible_width,
    uint32_t visible_height) {
    const unsigned int sub_x =
        plane == 0U ? 0U : state->subsampling_x;
    const unsigned int sub_y =
        plane == 0U ? 0U : state->subsampling_y;
    const uint32_t base_x = (block->column >> sub_x) << 2;
    const uint32_t base_y = (block->row >> sub_y) << 2;
    const uint32_t right =
        block->column + block->width < state->mi_columns
            ? block->column + block->width
            : state->mi_columns;
    const uint32_t bottom =
        block->row + block->height < state->mi_rows
            ? block->row + block->height
            : state->mi_rows;
    uint32_t count;
    uint32_t limit;
    uint32_t position4;

    if (block->row > state->block_state->tile_row_start &&
        nominal_width >= 8U && nominal_height >= 8U) {
        count = 0U;
        limit = av1_tile_obmc_neighbor_limit(block->width);
        position4 = block->column;
        while (count < limit && position4 < right) {
            const Av1BlockCell *cell = av1_tile_cell_at(
                state->block_state, (int64_t)block->row - 1,
                (int64_t)(position4 | 1U));
            uint32_t step4 = cell == NULL
                ? 2U
                : av1_tile_clip_obmc_step(cell->width);

            if (cell != NULL && cell->is_inter && !cell->use_intrabc &&
                cell->ref_frame[0] > 0U) {
                const uint32_t pred_x = (position4 << 2) >> sub_x;
                uint32_t pred_width = (step4 << 2) >> sub_x;
                uint32_t pred_height = nominal_height >> 1;
                AvifdecStatus status;

                ++count;
                if (pred_width > nominal_width) pred_width = nominal_width;
                if (pred_height > (32U >> sub_y)) {
                    pred_height = 32U >> sub_y;
                }
                if (pred_x < base_x ||
                    pred_x - base_x >= visible_width) {
                    return AVIFDEC_INVALID_DATA;
                }
                if (pred_width > visible_width - (pred_x - base_x)) {
                    pred_width = visible_width - (pred_x - base_x);
                }
                if (pred_height > visible_height) {
                    pred_height = visible_height;
                }
                status = av1_tile_predict_obmc_neighbor(
                    state, cell, plane, sub_x, sub_y,
                    pred_x, base_y, pred_width, pred_height);
                if (status != AVIFDEC_OK) return status;
                status = av1_inter_blend_obmc(
                    destination + (pred_x - base_x),
                    state->frame_planes.stride[plane],
                    state->inter_pred0, pred_width,
                    pred_width, pred_height, 0);
                if (status != AVIFDEC_OK) return status;
            }
            position4 += step4;
        }
    }

    if (block->column > state->block_state->tile_column_start) {
        count = 0U;
        limit = av1_tile_obmc_neighbor_limit(block->height);
        position4 = block->row;
        while (count < limit && position4 < bottom) {
            const Av1BlockCell *cell = av1_tile_cell_at(
                state->block_state, (int64_t)(position4 | 1U),
                (int64_t)block->column - 1);
            uint32_t step4 = cell == NULL
                ? 2U
                : av1_tile_clip_obmc_step(cell->height);

            if (cell != NULL && cell->is_inter && !cell->use_intrabc &&
                cell->ref_frame[0] > 0U) {
                const uint32_t pred_y = (position4 << 2) >> sub_y;
                uint32_t pred_width = nominal_width >> 1;
                uint32_t pred_height = (step4 << 2) >> sub_y;
                AvifdecStatus status;

                ++count;
                if (pred_width > (32U >> sub_x)) {
                    pred_width = 32U >> sub_x;
                }
                if (pred_height > nominal_height) pred_height = nominal_height;
                if (pred_y < base_y ||
                    pred_y - base_y >= visible_height) {
                    return AVIFDEC_INVALID_DATA;
                }
                if (pred_width > visible_width) pred_width = visible_width;
                if (pred_height > visible_height - (pred_y - base_y)) {
                    pred_height = visible_height - (pred_y - base_y);
                }
                status = av1_tile_predict_obmc_neighbor(
                    state, cell, plane, sub_x, sub_y,
                    base_x, pred_y, pred_width, pred_height);
                if (status != AVIFDEC_OK) return status;
                status = av1_inter_blend_obmc(
                    destination +
                        (size_t)(pred_y - base_y) *
                            state->frame_planes.stride[plane],
                    state->frame_planes.stride[plane],
                    state->inter_pred0, pred_width,
                    pred_width, pred_height, 1);
                if (status != AVIFDEC_OK) return status;
            }
            position4 += step4;
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_tile_predict_inter_plane(
    Av1TileResidualState *state,
    const Av1BlockTraceFields *block,
    unsigned int plane) {
    const unsigned int sub_x =
        plane == 0U ? 0U : state->subsampling_x;
    const unsigned int sub_y =
        plane == 0U ? 0U : state->subsampling_y;
    uint32_t nominal_width = (block->width << 2) >> sub_x;
    uint32_t nominal_height = (block->height << 2) >> sub_y;
    const uint32_t frame_width =
        (state->current_frame_width + ((1U << sub_x) - 1U)) >> sub_x;
    const uint32_t frame_height =
        (state->current_frame_height + ((1U << sub_y) - 1U)) >> sub_y;
    const uint32_t x = (block->column >> sub_x) << 2;
    const uint32_t y = (block->row >> sub_y) << 2;
    uint32_t visible_width;
    uint32_t visible_height;
    uint16_t *destination;
    Av1InterPredictParams prediction;
    int use_sub8x8_chroma = 0;
    unsigned int list_count;
    unsigned int list;
    AvifdecStatus status;

    if (nominal_width < 4U) nominal_width = 4U;
    if (nominal_height < 4U) nominal_height = 4U;
    if (x >= frame_width || y >= frame_height ||
        (size_t)nominal_width * nominal_height >
            state->inter_scratch_capacity) {
        return AVIFDEC_LIMIT_EXCEEDED;
    }
    visible_width = nominal_width;
    visible_height = nominal_height;
    if (visible_width > frame_width - x) visible_width = frame_width - x;
    if (visible_height > frame_height - y) visible_height = frame_height - y;
    destination = state->frame_planes.data[plane] +
                  (size_t)y * state->frame_planes.stride[plane] + x;
    list_count = block->ref_frame[1] > 0U ? 2U : 1U;
    avifdec_memory_fill(&prediction, 0U, sizeof(prediction));
    prediction.dst = state->inter_pred0;
    prediction.dst_stride = nominal_width;
    prediction.frame_width = frame_width;
    prediction.frame_height = frame_height;
    prediction.block_x = x;
    prediction.block_y = y;
    prediction.block_width = nominal_width;
    prediction.block_height = nominal_height;
    prediction.subsampling_x = (uint8_t)sub_x;
    prediction.subsampling_y = (uint8_t)sub_y;
    prediction.bit_depth = state->dequant_params.bit_depth;
    prediction.filter_x = block->interp_filter[1];
    prediction.filter_y = block->interp_filter[0];
    if (plane != 0U &&
        ((sub_x && block->width == 1U) ||
         (sub_y && block->height == 1U))) {
        const int row_start = sub_y && block->height == 1U ? -1 : 0;
        const int column_start = sub_x && block->width == 1U ? -1 : 0;
        int row_offset;

        use_sub8x8_chroma = 1;
        for (row_offset = row_start; row_offset <= 0; ++row_offset) {
            int column_offset;
            for (column_offset = column_start;
                 column_offset <= 0; ++column_offset) {
                const Av1BlockCell *cell = av1_tile_cell_at(
                    state->block_state,
                    (int64_t)block->row + row_offset,
                    (int64_t)block->column + column_offset);
                if (cell == 0 || !cell->is_inter) {
                    use_sub8x8_chroma = 0;
                }
            }
        }
    }
    if (use_sub8x8_chroma) {
        const uint32_t sub_width = (block->width << 2) >> sub_x;
        const uint32_t sub_height = (block->height << 2) >> sub_y;
        const int row_start = sub_y && block->height == 1U ? -1 : 0;
        const int column_start = sub_x && block->width == 1U ? -1 : 0;
        uint32_t sub_y_offset;

        if (list_count != 1U || block->interintra ||
            sub_width == 0U || sub_height == 0U) {
            return AVIFDEC_INVALID_DATA;
        }
        for (sub_y_offset = 0U; sub_y_offset < nominal_height;
             sub_y_offset += sub_height) {
            uint32_t sub_x_offset;
            int row_offset =
                row_start + (int)(sub_y_offset / sub_height);
            for (sub_x_offset = 0U; sub_x_offset < nominal_width;
                 sub_x_offset += sub_width) {
                int column_offset =
                    column_start + (int)(sub_x_offset / sub_width);
                const Av1BlockCell *cell = av1_tile_cell_at(
                    state->block_state,
                    (int64_t)block->row + row_offset,
                    (int64_t)block->column + column_offset);
                const uint8_t reference =
                    cell == 0 ? 0U : cell->ref_frame[0];
                const unsigned int reference_index =
                    reference == 0U ? 0U : reference - 1U;

                if (cell == 0 || !cell->is_inter ||
                    (!cell->use_intrabc &&
                     (reference == 0U || reference > 7U ||
                      !state->reference_pixels_valid[reference_index]))) {
                    return AVIFDEC_UNSUPPORTED;
                }
                prediction.dst =
                    state->inter_pred0 +
                    (size_t)sub_y_offset * nominal_width + sub_x_offset;
                prediction.block_x = x + sub_x_offset;
                prediction.block_y = y + sub_y_offset;
                prediction.block_width = sub_width;
                prediction.block_height = sub_height;
                prediction.filter_x = cell->interp_filter[1];
                prediction.filter_y = cell->interp_filter[0];
                if (cell->use_intrabc) {
                    prediction.src = state->frame_planes.data[plane];
                    prediction.src_stride =
                        state->frame_planes.stride[plane];
                    prediction.src_width = frame_width;
                    prediction.src_height = frame_height;
                } else {
                    prediction.src =
                        state->reference_planes[reference_index].data[plane];
                    prediction.src_stride =
                        state->reference_planes[reference_index].stride[plane];
                    prediction.src_width =
                        (state->reference_width[reference_index] +
                         ((1U << sub_x) - 1U)) >> sub_x;
                    prediction.src_height =
                        (state->reference_height[reference_index] +
                         ((1U << sub_y) - 1U)) >> sub_y;
                }
                prediction.mv_col = cell->mv[0].column;
                prediction.mv_row = cell->mv[0].row;
                if (prediction.src == 0 ||
                    prediction.src_width == 0U ||
                    prediction.src_height == 0U) {
                    return AVIFDEC_INVALID_DATA;
                }
                status = av1_inter_predict_single(&prediction);
                if (status != AVIFDEC_OK) return status;
            }
        }
    } else {
        for (list = 0U; list < list_count; ++list) {
            const uint8_t reference = block->ref_frame[list];
            const unsigned int reference_index =
                reference == 0U ? 0U : reference - 1U;
            uint16_t *output =
                list == 0U ? state->inter_pred0 : state->inter_pred1;

            if (!block->use_intrabc &&
                (reference == 0U || reference > 7U ||
                 !state->reference_pixels_valid[reference_index])) {
                return AVIFDEC_UNSUPPORTED;
            }
            if (block->use_intrabc) {
                prediction.src = state->frame_planes.data[plane];
                prediction.src_stride =
                    state->frame_planes.stride[plane];
                prediction.src_width = frame_width;
                prediction.src_height = frame_height;
            } else {
                prediction.src =
                    state->reference_planes[reference_index].data[plane];
                prediction.src_stride =
                    state->reference_planes[reference_index].stride[plane];
                prediction.src_width =
                    (state->reference_width[reference_index] +
                     ((1U << sub_x) - 1U)) >> sub_x;
                prediction.src_height =
                    (state->reference_height[reference_index] +
                     ((1U << sub_y) - 1U)) >> sub_y;
            }
            prediction.mv_col = block->mv[list].column;
            prediction.mv_row = block->mv[list].row;
            if (prediction.src == 0 || prediction.src_width == 0U ||
                prediction.src_height == 0U) {
                return AVIFDEC_INVALID_DATA;
            }
            if (!block->use_intrabc &&
                prediction.src_width == prediction.frame_width &&
                prediction.src_height == prediction.frame_height &&
                ((block->motion_mode == 2U && list == 0U) ||
                 (av1_tile_component_mode(block->y_mode, list) == 15U &&
                  block->warp_params[list][2] > 0))) {
                Av1WarpModel model;
                Av1WarpPlaneParams warp;
                Av1WarpStatus warp_status;

                avifdec_memory_copy(
                    model.matrix, block->warp_params[list],
                    sizeof(model.matrix));
                avifdec_memory_fill(&warp, 0U, sizeof(warp));
                warp.source = prediction.src;
                warp.source_stride = prediction.src_stride;
                warp.source_width = prediction.src_width;
                warp.source_height = prediction.src_height;
                warp.block_x = prediction.block_x;
                warp.block_y = prediction.block_y;
                warp.block_width = prediction.block_width;
                warp.block_height = prediction.block_height;
                warp.subsampling_x = prediction.subsampling_x;
                warp.subsampling_y = prediction.subsampling_y;
                warp.bit_depth = prediction.bit_depth;
                warp.model = &model;
                warp_status = list_count == 1U
                    ? av1_warp_predict_single(
                        &warp, prediction.dst, prediction.dst_stride)
                    : av1_warp_predict_compound(
                        &warp, output, nominal_width);
                status = warp_status == AV1_WARP_OK
                             ? AVIFDEC_OK
                             : AVIFDEC_INVALID_DATA;
            } else {
                status = list_count == 1U
                    ? av1_inter_predict_single(&prediction)
                    : av1_inter_predict_compound(
                        &prediction, output, nominal_width);
            }
            if (status != AVIFDEC_OK) return status;
        }
    }
    if (list_count == 2U) {
        Av1CompoundParams compound;

        avifdec_memory_fill(&compound, 0U, sizeof(compound));
        compound.dst = state->inter_pred0;
        compound.dst_stride = nominal_width;
        compound.width = nominal_width;
        compound.height = nominal_height;
        compound.bit_depth = state->dequant_params.bit_depth;
        if (block->compound_type == 0U || block->compound_type == 1U) {
            compound.weight0 = 8U;
            compound.weight1 = 8U;
            if (block->compound_type == 1U) {
                av1_tile_distance_weights(
                    state, block, &compound.weight0, &compound.weight1);
            }
            status = av1_inter_blend_average(
                &compound, state->inter_pred0, nominal_width,
                state->inter_pred1, nominal_width);
        } else {
            if (block->compound_type == 2U) {
                status = av1_tile_build_wedge_plane_mask(
                    state, block, block->wedge_index, block->wedge_sign,
                    sub_x, sub_y, nominal_width, nominal_height);
            } else {
                status = AVIFDEC_OK;
                if (plane == 0U) {
                    status = av1_inter_build_diff_mask(
                        state->inter_mask, nominal_width,
                        state->inter_pred0, nominal_width,
                        state->inter_pred1, nominal_width,
                        nominal_width, nominal_height,
                        state->dequant_params.bit_depth,
                        block->diff_mask_inverse);
                } else if (plane == 1U) {
                    av1_tile_subsample_mask(
                        state->inter_mask, block->width << 2,
                        sub_x, sub_y, nominal_width, nominal_height);
                }
            }
            if (status != AVIFDEC_OK) return status;
            compound.mask = state->inter_mask;
            compound.mask_stride = nominal_width;
            status = av1_inter_blend_masked(
                &compound, state->inter_pred0, nominal_width,
                state->inter_pred1, nominal_width);
        }
        if (status != AVIFDEC_OK) return status;
    }
    if (block->interintra) {
        static const uint8_t intra_mode[4] = { 0U, 1U, 2U, 9U };
        Av1BlockTraceFields intra = *block;
        uint32_t width4 = nominal_width >> 2;
        uint32_t height4 = nominal_height >> 2;

        intra.is_inter = 0U;
        intra.y_mode = intra_mode[block->interintra_mode];
        intra.uv_mode = intra.y_mode;
        intra.angle_delta_y = 0;
        intra.angle_delta_uv = 0;
        status = av1_tile_predict_chunk(
            state, &intra, plane, 0U, 0U, width4, height4);
        if (status != AVIFDEC_OK) return status;
        if (block->use_wedge_interintra) {
            status = av1_tile_build_wedge_plane_mask(
                state, block, block->interintra_wedge_index, 0U,
                sub_x, sub_y, nominal_width, nominal_height);
        } else {
            status = av1_inter_build_interintra_mask(
                state->inter_mask, nominal_width,
                nominal_width, nominal_height, block->interintra_mode);
        }
        if (status != AVIFDEC_OK) return status;
        av1_inter_blend_interintra(
            destination, state->frame_planes.stride[plane],
            state->inter_pred0, nominal_width,
            destination, state->frame_planes.stride[plane],
            state->inter_mask, nominal_width,
            visible_width, visible_height);
    } else {
        av1_tile_copy_prediction(
            destination, state->frame_planes.stride[plane],
            state->inter_pred0, nominal_width,
            visible_width, visible_height);
    }
    if (block->motion_mode == 1U) {
        status = av1_tile_apply_obmc(
            state, block, plane, destination,
            nominal_width, nominal_height,
            visible_width, visible_height);
        if (status != AVIFDEC_OK) return status;
    }
    {
        uint32_t row;
        av1_tile_checkpoint_hash(&state->predictor_checksum, plane);
        av1_tile_checkpoint_hash(&state->predictor_checksum, x >> 2);
        av1_tile_checkpoint_hash(&state->predictor_checksum, y >> 2);
        for (row = 0U; row < visible_height; ++row) {
            uint32_t column;
            for (column = 0U; column < visible_width; ++column) {
                av1_tile_checkpoint_hash(
                    &state->predictor_checksum,
                    destination[(size_t)row *
                                    state->frame_planes.stride[plane] +
                                column]);
            }
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_tile_parse_residual(void *user_data,
                                      Av1SymbolDecoder *decoder,
                                      const Av1BlockTraceFields *block) {
    Av1TileResidualState *state = (Av1TileResidualState *)user_data;
    unsigned int plane_count;
    uint32_t width_chunks;
    uint32_t height_chunks;
    uint32_t chunk_y;
    uint32_t chunk_x;

    if (state == 0 || decoder == 0 || block == 0 || state->coeff_contexts == 0 ||
        state->cdfs == 0 || block->tx_size >= AV1_TX_SIZES_ALL) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (block->is_inter && !block->skip && !block->lossless &&
        state->tx_mode == 2U && (block->width > 1U || block->height > 1U)) {
        AvifdecStatus status = av1_tile_read_inter_tx_partitions(
            state, decoder, block);
        if (status != AVIFDEC_OK) return status;
    }
    plane_count = block->has_chroma ? 3U : 1U;
    if (block->is_inter) {
        unsigned int plane;

        for (plane = 0U; plane < plane_count; ++plane) {
            AvifdecStatus status = av1_tile_predict_inter_plane(
                state, block, plane);
            if (status != AVIFDEC_OK) return status;
        }
    }
    width_chunks = block->width >> 4;
    height_chunks = block->height >> 4;
    if (width_chunks == 0U) width_chunks = 1U;
    if (height_chunks == 0U) height_chunks = 1U;
    for (chunk_y = 0U; chunk_y < height_chunks; ++chunk_y) {
        for (chunk_x = 0U; chunk_x < width_chunks; ++chunk_x) {
            unsigned int plane;
            uint32_t chunk_width4 = width_chunks > 1U || height_chunks > 1U
                                    ? 16U : block->width;
            uint32_t chunk_height4 = width_chunks > 1U || height_chunks > 1U
                                     ? 16U : block->height;

            for (plane = 0U; plane < plane_count; ++plane) {
                unsigned int sub_x = plane == 0U ? 0U : state->subsampling_x;
                unsigned int sub_y = plane == 0U ? 0U : state->subsampling_y;
                size_t max_x4 = (state->mi_columns + sub_x) >> sub_x;
                size_t max_y4 = (state->mi_rows + sub_y) >> sub_y;
                uint32_t plane_width4 = chunk_width4 >> sub_x;
                uint32_t plane_height4 = chunk_height4 >> sub_y;
                uint32_t plane_block_width4 = block->width >> sub_x;
                uint32_t plane_block_height4 = block->height >> sub_y;
                Av1TxSize tx_size;
                uint32_t step_x;
                uint32_t step_y;
                uint32_t y;
                uint32_t x;

                if (plane_width4 == 0U) plane_width4 = 1U;
                if (plane_height4 == 0U) plane_height4 = 1U;
                if (plane_block_width4 == 0U) plane_block_width4 = 1U;
                if (plane_block_height4 == 0U) plane_block_height4 = 1U;
                tx_size = block->lossless ? AV1_TX_4X4
                          : plane == 0U ? (Av1TxSize)block->tx_size
                          : av1_tile_chroma_tx_size(plane_width4, plane_height4);
                if (tx_size >= AV1_TX_SIZES_ALL) return AVIFDEC_UNSUPPORTED;
                step_x = av1_tx_size_info[tx_size].width >> 2;
                step_y = av1_tx_size_info[tx_size].height >> 2;
                for (y = 0U; y < plane_height4;
                     y += block->is_inter && plane == 0U &&
                                  state->tx_mode == 2U && !block->skip &&
                                  !block->lossless
                              ? 1U : step_y) {
                    for (x = 0U; x < plane_width4;
                         x += block->is_inter && plane == 0U &&
                                      state->tx_mode == 2U && !block->skip &&
                                      !block->lossless
                                  ? 1U : step_x) {
                        size_t x4 = (block->column >> sub_x) + x +
                                    ((chunk_x << 4) >> sub_x);
                        size_t y4 = (block->row >> sub_y) + y +
                                    ((chunk_y << 4) >> sub_y);
                        Av1CoeffBlockResult result;
                        Av1TxType tx_type = AV1_TX_DCT_DCT;
                        Av1TxType entropy_tx_type;
                        Av1TileTxSelector selector;
                        AvifdecStatus status;
                        size_t packed_width;
                        size_t packed_height;
                        size_t packed_count;
                        size_t residual_count;
                        size_t checkpoint;
                        uint32_t predict_width4;
                        uint32_t predict_height4;
                        uint32_t checkpoint_width4;
                        uint32_t checkpoint_height4;
                        const int variable_inter_tx =
                            block->is_inter && plane == 0U &&
                            state->tx_mode == 2U && !block->skip &&
                            !block->lossless &&
                            (block->width > 1U || block->height > 1U);

                        if (variable_inter_tx) {
                            size_t tx_index;

                            if (x4 >= max_x4 || y4 >= max_y4) continue;
                            tx_index = y4 * state->mi_columns + x4;
                            if (tx_index >= state->tx_type_capacity) {
                                return AVIFDEC_LIMIT_EXCEEDED;
                            }
                            if (state->tx_types[tx_index] != 0xffU) continue;
                            {
                                const Av1BlockCell *cell =
                                    av1_tile_cell_at(
                                        state->block_state, (int64_t)y4,
                                        (int64_t)x4);
                                if (cell == 0 ||
                                    cell->tx_size >= AV1_TX_SIZES_ALL) {
                                    return AVIFDEC_INVALID_DATA;
                                }
                                tx_size = (Av1TxSize)cell->tx_size;
                                step_x =
                                    av1_tx_size_info[tx_size].width >> 2;
                                step_y =
                                    av1_tx_size_info[tx_size].height >> 2;
                            }
                        }

                        predict_width4 = step_x;
                        predict_height4 = step_y;
                        if (predict_width4 > plane_width4 - x) {
                            predict_width4 = plane_width4 - x;
                        }
                        if (predict_height4 > plane_height4 - y) {
                            predict_height4 = plane_height4 - y;
                        }
                        if (x4 >= max_x4 || y4 >= max_y4) {
                            continue;
                        }
                        if (!block->is_inter) {
                            status = av1_tile_predict_chunk(
                                state, block, plane,
                                ((chunk_x << 4) >> sub_x) + x,
                                ((chunk_y << 4) >> sub_y) + y,
                                predict_width4, predict_height4);
                            if (status != AVIFDEC_OK) return status;
                        }
                        status = av1_tile_store_loop_filter_tx_size(
                            state, plane, x4, y4, tx_size);
                        if (status != AVIFDEC_OK) return status;
                        checkpoint_width4 = predict_width4;
                        checkpoint_height4 = predict_height4;
                        if (checkpoint_width4 > max_x4 - x4) {
                            checkpoint_width4 = (uint32_t)(max_x4 - x4);
                        }
                        if (checkpoint_height4 > max_y4 - y4) {
                            checkpoint_height4 = (uint32_t)(max_y4 - y4);
                        }
                        if (!block->is_inter) {
                            av1_tile_checkpoint_hash(
                                &state->predictor_checksum, plane);
                            av1_tile_checkpoint_hash(
                                &state->predictor_checksum, x4);
                            av1_tile_checkpoint_hash(
                                &state->predictor_checksum, y4);
                            av1_tile_checkpoint_hash(
                                &state->predictor_checksum, tx_size);
                            {
                            uint32_t predictor_row;
                            uint32_t predictor_column;
                            uint16_t *predictor = state->frame_planes.data[plane] +
                                ((size_t)y4 << 2) *
                                    state->frame_planes.stride[plane] +
                                ((size_t)x4 << 2);
                            for (predictor_row = 0U;
                                   predictor_row < (checkpoint_height4 << 2);
                                 ++predictor_row) {
                                for (predictor_column = 0U;
                                      predictor_column < (checkpoint_width4 << 2);
                                     ++predictor_column) {
                                    av1_tile_checkpoint_hash(
                                        &state->predictor_checksum,
                                        predictor[(size_t)predictor_row *
                                            state->frame_planes.stride[plane] +
                                            predictor_column]);
                                }
                            }
                            }
                        }
                        if (block->skip) continue;
                        if (plane != 0U && !block->lossless) {
                            unsigned int set = av1_tile_tx_set(
                                state, tx_size, block->is_inter);
                            if (block->is_inter) {
                                size_t luma_x4 = block->column +
                                    ((size_t)x << sub_x) + (chunk_x << 4);
                                size_t luma_y4 = block->row +
                                    ((size_t)y << sub_y) + (chunk_y << 4);
                                size_t tx_index = luma_y4 * state->mi_columns +
                                                  luma_x4;

                                if (tx_index >= state->tx_type_capacity) {
                                    return AVIFDEC_LIMIT_EXCEEDED;
                                }
                                tx_type = (Av1TxType)state->tx_types[tx_index];
                            } else {
                                tx_type = (Av1TxType)
                                    av1_mode_to_tx_type[block->uv_mode];
                            }
                            if (!av1_tile_tx_type_in_set(set, tx_type)) {
                                tx_type = AV1_TX_DCT_DCT;
                            }
                        }
                        entropy_tx_type = tx_type;
                        selector.state = state;
                        selector.block = block;
                        selector.x4 = x4;
                        selector.y4 = y4;
                        status = av1_coeff_parse_block_select(
                            decoder, &state->cdfs->coeff, state->coeff_contexts,
                            plane, tx_size, tx_type,
                            (size_t)plane_block_width4 << 2,
                            (size_t)plane_block_height4 << 2, x4, y4,
                            plane == 0U ? av1_tile_select_tx_type : 0,
                            plane == 0U ? &selector : 0,
                            state->quantized, state->quantized_capacity, &result);
                        if (status != AVIFDEC_OK) return status;
                        if (plane == 0U) {
                            size_t tx_index = y4 * state->mi_columns + x4;
                            if (tx_index >= state->tx_type_capacity) {
                                return AVIFDEC_LIMIT_EXCEEDED;
                            }
                            tx_type = (Av1TxType)state->tx_types[tx_index];
                        }
                        if (plane == 0U && result.eob == 0U) {
                            status = av1_tile_store_tx_type(
                                state, x4, y4, tx_size, AV1_TX_DCT_DCT);
                            if (status != AVIFDEC_OK) return status;
                            tx_type = AV1_TX_DCT_DCT;
                        }
                        state->transform_size_mask |= (uint32_t)1U << tx_size;
                        state->transform_type_mask |= (uint32_t)1U << tx_type;
                        if (state->transform_count == SIZE_MAX ||
                            state->coefficient_count > SIZE_MAX - result.eob) {
                            return AVIFDEC_OVERFLOW;
                        }
                        ++state->transform_count;
                        if (result.eob != 0U) ++state->nonzero_transform_count;
                        state->coefficient_count += result.eob;
                        packed_width = av1_tx_size_info[tx_size].width < 32U
                                       ? av1_tx_size_info[tx_size].width : 32U;
                        packed_height = av1_tx_size_info[tx_size].height < 32U
                                        ? av1_tx_size_info[tx_size].height : 32U;
                        packed_count = packed_width * packed_height;
                        residual_count = (size_t)av1_tx_size_info[tx_size].width *
                                         av1_tx_size_info[tx_size].height;
                        state->dequant_params.q_index = block->q_index;
                        state->dequant_params.plane = (uint8_t)plane;
                        state->dequant_params.qm_level = block->lossless != 0U
                            ? 15U : plane == 0U ? state->qm_y
                                  : plane == 1U ? state->qm_u : state->qm_v;
                        status = av1_recon_dequantize(
                            state->quantized, packed_count, tx_size, tx_type,
                            &state->dequant_params, state->dequantized,
                            state->dequantized_capacity);
                        if (status != AVIFDEC_OK) return status;
                        status = av1_recon_inverse_transform(
                            state->dequantized, packed_count, tx_size, tx_type,
                            state->dequant_params.bit_depth, block->lossless,
                            state->residual, state->residual_capacity);
                        if (status != AVIFDEC_OK) return status;
                        {
                            unsigned int flip_lr =
                                tx_type == AV1_TX_DCT_FLIPADST ||
                                tx_type == AV1_TX_ADST_FLIPADST ||
                                tx_type == AV1_TX_FLIPADST_FLIPADST ||
                                tx_type == AV1_TX_H_FLIPADST;
                            unsigned int flip_ud =
                                tx_type == AV1_TX_FLIPADST_DCT ||
                                tx_type == AV1_TX_FLIPADST_ADST ||
                                tx_type == AV1_TX_FLIPADST_FLIPADST ||
                                tx_type == AV1_TX_V_FLIPADST;
                            uint32_t pixel_x = (uint32_t)x4 << 2;
                            uint32_t pixel_y = (uint32_t)y4 << 2;
                            status = av1_recon_add_residual(
                                state->frame_planes.data[plane] +
                                    (size_t)pixel_y * state->frame_planes.stride[plane] +
                                    pixel_x,
                                state->frame_planes.stride[plane],
                                av1_tx_size_info[tx_size].width,
                                av1_tx_size_info[tx_size].height,
                                state->residual, residual_count,
                                state->dequant_params.bit_depth,
                                (uint8_t)flip_lr, (uint8_t)flip_ud);
                            if (status != AVIFDEC_OK) return status;
                        }
                        if (!state->disable_trace) {
                            av1_tile_checkpoint_hash(
                                &state->quantized_checksum, plane);
                            av1_tile_checkpoint_hash(
                                &state->quantized_checksum, x4);
                            av1_tile_checkpoint_hash(
                                &state->quantized_checksum, y4);
                            av1_tile_checkpoint_hash(
                                &state->quantized_checksum, tx_type);
                            for (checkpoint = 0U; checkpoint < packed_count;
                                 ++checkpoint) {
                                av1_tile_checkpoint_hash(
                                    &state->quantized_checksum,
                                    (uint32_t)state->quantized[checkpoint]);
                                av1_tile_checkpoint_hash(
                                    &state->dequantized_checksum,
                                    (uint32_t)state->dequantized[checkpoint]);
                            }
                            for (checkpoint = 0U; checkpoint < residual_count;
                                 ++checkpoint) {
                                av1_tile_checkpoint_hash(
                                    &state->residual_checksum,
                                    (uint32_t)state->residual[checkpoint]);
                            }
                            av1_tile_residual_hash(state, plane);
                            av1_tile_residual_hash(state, x4);
                            av1_tile_residual_hash(state, y4);
                            av1_tile_residual_hash(state, tx_size);
                            av1_tile_residual_hash(state, entropy_tx_type);
                            av1_tile_residual_hash(state, result.eob);
                            av1_tile_residual_hash(state, result.cul_level);
                            av1_tile_residual_hash(state, result.dc_category);
                        }
                    }
                }
            }
        }
    }
    if (block->skip) return av1_tile_reset_block_context(state, block);
    return AVIFDEC_OK;
}
