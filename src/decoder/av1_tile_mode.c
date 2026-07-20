#include "av1_tile_mode.h"
#include "av1_tile_geometry.h"
#include "av1_tile_internal.h"
#include "av1_tile_inter_mode.h"
#include "av1_tile_inter_mv.h"
#include "av1.h"
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

const uint8_t av1_tile_block_width_mi[AV1_BLOCK_SIZES] = {
    1U, 1U, 2U, 2U, 2U, 4U, 4U, 4U, 8U, 8U, 8U,
    16U, 16U, 16U, 32U, 32U, 1U, 4U, 2U, 8U, 4U, 16U
};

const uint8_t av1_tile_block_height_mi[AV1_BLOCK_SIZES] = {
    1U, 2U, 1U, 2U, 4U, 2U, 4U, 8U, 4U, 8U, 16U,
    8U, 16U, 32U, 16U, 32U, 4U, 1U, 8U, 2U, 16U, 4U
};

const uint8_t av1_tile_max_tx_size[AV1_BLOCK_SIZES] = {
    0U, 5U, 6U, 1U, 7U, 8U, 2U, 9U, 10U, 3U, 11U,
    12U, 4U, 4U, 4U, 4U, 13U, 14U, 15U, 16U, 17U, 18U
};

const uint8_t av1_tile_max_tx_depth[AV1_BLOCK_SIZES] = {
    0U, 1U, 1U, 1U, 2U, 2U, 2U, 3U, 3U, 3U, 4U,
    4U, 4U, 4U, 4U, 4U, 2U, 2U, 3U, 3U, 4U, 4U
};

const uint8_t av1_tile_split_tx_size[19] = {
    0U, 0U, 1U, 2U, 3U, 0U, 0U, 1U, 1U, 2U,
    2U, 3U, 3U, 5U, 6U, 7U, 8U, 9U, 10U
};

const uint8_t av1_tile_tx_width[19] = {
    4U, 8U, 16U, 32U, 64U, 4U, 8U, 8U, 16U, 16U,
    32U, 32U, 64U, 4U, 16U, 8U, 32U, 16U, 64U
};

const uint8_t av1_tile_tx_height[19] = {
    4U, 8U, 16U, 32U, 64U, 8U, 4U, 16U, 8U, 32U,
    16U, 64U, 32U, 16U, 4U, 32U, 8U, 64U, 16U
};

AvifdecStatus av1_tile_find_block_size(uint32_t width,
                                       uint32_t height,
                                       uint8_t *block_size) {
    unsigned int index;

    if (block_size == 0) return AVIFDEC_INVALID_ARGUMENT;
    for (index = 0U; index < AV1_BLOCK_SIZES; ++index) {
        if (av1_tile_block_width_mi[index] == width &&
            av1_tile_block_height_mi[index] == height) {
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
    tx_size = av1_tile_max_tx_size[block_size];
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
                                 : av1_tile_tx_width[above->tx_size]) >=
                av1_tile_tx_width[tx_size]) {
                ++context;
            }
        }
        if (availability->left) {
            const Av1BlockCell *left = av1_block_cell(
                config->block_state, fields->row, fields->column - 1U);
            if (left == 0 || left->tx_size >= 19U) return AVIFDEC_INVALID_DATA;
            if ((left->is_inter ? (uint32_t)left->height * 4U
                                : av1_tile_tx_height[left->tx_size]) >=
                av1_tile_tx_height[tx_size]) {
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
                                   av1_tile_max_tx_depth[block_size], context,
                                   &depth) != AVIFDEC_OK) {
            return decoder->status;
        }
        while (depth-- != 0U) tx_size = av1_tile_split_tx_size[tx_size];
    }
    fields->tx_size = tx_size;
    return AVIFDEC_OK;
}

typedef struct {
    Av1TileModeConfig config;
    int32_t current_q_index;
    int32_t delta_lf[4];
    uint8_t read_deltas;
    /* Tile-local rolling above/left palette color state (replaces the
       persistent palette_colors_* fields formerly in Av1BlockCell). Lives
       for the duration of one tile's mode decode, like `config` above. */
    Av1TilePaletteContext palette_context;
    Av1TileBeforeSuperblock original_before_superblock;
    void *original_before_superblock_user_data;
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

static int32_t av1_tile_clip_int32(int32_t minimum,
                                    int32_t maximum,
                                    int32_t value) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static AvifdecStatus av1_tile_mode_before_superblock(void *user_data,
                                                     Av1SymbolDecoder *decoder,
                                                     Av1TileCdfs *cdfs,
                                                     uint32_t row,
                                                     uint32_t column,
                                                     uint32_t superblock_mi) {
    Av1TileModeContext *context = (Av1TileModeContext *)user_data;
    AvifdecStatus status;

    /* The "left" palette color context only spans one superblock row
       band; reset it whenever the tile loop starts a new band, before
       chaining to whatever before_superblock callback the caller set
       (e.g. loop restoration parsing). */
    if (column == context->config.block_state->tile_column_start) {
        status = av1_tile_palette_context_new_row_band(
            &context->palette_context, row);
        if (status != AVIFDEC_OK) return status;
    }
    if (context->original_before_superblock == 0) return AVIFDEC_OK;
    return context->original_before_superblock(
        context->original_before_superblock_user_data, decoder, cdfs, row,
        column, superblock_mi);
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
                    delta * ((int32_t)1 << config->delta_q_res));
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
                        delta * ((int32_t)1 << config->delta_lf_res));
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
        status = av1_tile_inter_read_intrabc(
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
            status = av1_tile_inter_read_mode_and_mvs(
                decoder, cdfs, config, &availability, &fields);
            if (status != AVIFDEC_OK) return status;
            status = av1_tile_inter_read_compound_syntax(
                decoder, cdfs, config, &availability, &fields, block_size);
            if (status != AVIFDEC_OK) return status;
            status = av1_tile_inter_read_interp_filters(
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
                decoder, cdfs, config, &availability, &fields, block_size,
                &context->palette_context);
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
    AvifdecStatus status;

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
    status = av1_tile_palette_context_init(
        &context.palette_context, partition_config->tile_row_start,
        partition_config->tile_column_start,
        partition_config->tile_column_end);
    if (status != AVIFDEC_OK) return status;
    context.original_before_superblock = partition_config->before_superblock;
    context.original_before_superblock_user_data =
        partition_config->before_superblock_user_data;
    config.decode_mode_block = av1_tile_decode_mode_block;
    config.user_data = &context;
    config.before_superblock = av1_tile_mode_before_superblock;
    config.before_superblock_user_data = &context;
    return av1_tile_decode_partitions(&config, frame_cdfs, tile_cdfs,
                                      partition_trace);
}
