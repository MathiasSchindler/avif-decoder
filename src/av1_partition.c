#include "av1_partition.h"
#include "base.h"

#define AV1_PARTITION_MAX_DEPTH 6U

static const uint16_t av1_default_partition_width8[4][5] = {
    { 19132U, 25510U, 30392U, 32768U, 0U },
    { 13928U, 19855U, 28540U, 32768U, 0U },
    { 12522U, 23679U, 28629U, 32768U, 0U },
    { 9896U, 18783U, 25853U, 32768U, 0U }
};

static const uint16_t av1_default_partition_width16[4][11] = {
    { 15597U, 20929U, 24571U, 26706U, 27664U, 28821U, 29601U, 30571U, 31902U, 32768U, 0U },
    { 7925U, 11043U, 16785U, 22470U, 23971U, 25043U, 26651U, 28701U, 29834U, 32768U, 0U },
    { 5414U, 13269U, 15111U, 20488U, 22360U, 24500U, 25537U, 26336U, 32117U, 32768U, 0U },
    { 2662U, 6362U, 8614U, 20860U, 23053U, 24778U, 26436U, 27829U, 31171U, 32768U, 0U }
};

static const uint16_t av1_default_partition_width32[4][11] = {
    { 18462U, 20920U, 23124U, 27647U, 28227U, 29049U, 29519U, 30178U, 31544U, 32768U, 0U },
    { 7689U, 9060U, 12056U, 24992U, 25660U, 26182U, 26951U, 28041U, 29052U, 32768U, 0U },
    { 6015U, 9009U, 10062U, 24544U, 25409U, 26545U, 27071U, 27526U, 32047U, 32768U, 0U },
    { 1394U, 2208U, 2796U, 28614U, 29061U, 29466U, 29840U, 30185U, 31899U, 32768U, 0U }
};

static const uint16_t av1_default_partition_width64[4][11] = {
    { 20137U, 21547U, 23078U, 29566U, 29837U, 30261U, 30524U, 30892U, 31724U, 32768U, 0U },
    { 6732U, 7490U, 9497U, 27944U, 28250U, 28515U, 28969U, 29630U, 30104U, 32768U, 0U },
    { 5945U, 7663U, 8348U, 28683U, 29117U, 29749U, 30064U, 30298U, 32238U, 32768U, 0U },
    { 870U, 1212U, 1487U, 31198U, 31394U, 31574U, 31743U, 31881U, 32332U, 32768U, 0U }
};

static const uint16_t av1_default_partition_width128[4][9] = {
    { 27899U, 28219U, 28529U, 32484U, 32539U, 32619U, 32639U, 32768U, 0U },
    { 6607U, 6990U, 8268U, 32060U, 32219U, 32338U, 32371U, 32768U, 0U },
    { 5429U, 6676U, 7122U, 32027U, 32227U, 32531U, 32582U, 32768U, 0U },
    { 711U, 966U, 1172U, 32448U, 32538U, 32617U, 32664U, 32768U, 0U }
};

static void av1_partition_hash(Av1PartitionTrace *trace, uint32_t value) {
    unsigned int index;

    for (index = 0U; index < 4U; ++index) {
        trace->checksum ^= (value >> (index * 8U)) & 0xffU;
        trace->checksum *= (uint64_t)1099511628211ULL;
    }
}

static void av1_partition_checksum_bytes(uint64_t *checksum,
                                         const void *data,
                                         size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;

    for (index = 0U; index < size; ++index) {
        *checksum ^= bytes[index];
        *checksum *= (uint64_t)1099511628211ULL;
    }
}

void av1_partition_cdfs_init(Av1PartitionCdfs *cdfs) {
    if (cdfs == 0) return;
    avifdec_memory_copy(cdfs->width8, av1_default_partition_width8,
                        sizeof(cdfs->width8));
    avifdec_memory_copy(cdfs->width16, av1_default_partition_width16,
                        sizeof(cdfs->width16));
    avifdec_memory_copy(cdfs->width32, av1_default_partition_width32,
                        sizeof(cdfs->width32));
    avifdec_memory_copy(cdfs->width64, av1_default_partition_width64,
                        sizeof(cdfs->width64));
    avifdec_memory_copy(cdfs->width128, av1_default_partition_width128,
                        sizeof(cdfs->width128));
}

uint64_t av1_partition_cdfs_checksum(const Av1PartitionCdfs *cdfs) {
    uint64_t checksum = (uint64_t)1469598103934665603ULL;

    if (cdfs == 0) return 0U;
    av1_partition_checksum_bytes(&checksum, cdfs->width8, sizeof(cdfs->width8));
    av1_partition_checksum_bytes(&checksum, cdfs->width16, sizeof(cdfs->width16));
    av1_partition_checksum_bytes(&checksum, cdfs->width32, sizeof(cdfs->width32));
    av1_partition_checksum_bytes(&checksum, cdfs->width64, sizeof(cdfs->width64));
    av1_partition_checksum_bytes(&checksum, cdfs->width128, sizeof(cdfs->width128));
    return checksum;
}

static uint32_t av1_partition_probability(const uint16_t *cdf, size_t symbol) {
    return cdf[symbol] - (symbol == 0U ? 0U : cdf[symbol - 1U]);
}

AvifdecStatus av1_partition_read_entropy(void *user_data,
                                         uint32_t row,
                                         uint32_t column,
                                         uint32_t block_mi,
                                         unsigned int context,
                                         int has_rows,
                                         int has_columns,
                                         Av1Partition *partition) {
    Av1PartitionEntropy *entropy = (Av1PartitionEntropy *)user_data;
    uint16_t *cdf;
    size_t symbols;
    uint32_t symbol;

    (void)row;
    (void)column;
    if (entropy == 0 || entropy->decoder == 0 || entropy->cdfs == 0 ||
        partition == 0 || context >= 4U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (block_mi == 2U) {
        cdf = entropy->cdfs->width8[context];
        symbols = 4U;
    } else if (block_mi == 4U) {
        cdf = entropy->cdfs->width16[context];
        symbols = 10U;
    } else if (block_mi == 8U) {
        cdf = entropy->cdfs->width32[context];
        symbols = 10U;
    } else if (block_mi == 16U) {
        cdf = entropy->cdfs->width64[context];
        symbols = 10U;
    } else if (block_mi == 32U) {
        cdf = entropy->cdfs->width128[context];
        symbols = 8U;
    } else {
        return AVIFDEC_INVALID_DATA;
    }
    if (has_rows && has_columns) {
        symbol = av1_symbol_read(entropy->decoder, cdf, symbols);
        *partition = (Av1Partition)symbol;
    } else {
        uint16_t binary_cdf[3];
        uint32_t split_probability;

        if (block_mi == 2U) return AVIFDEC_INVALID_DATA;
        binary_cdf[1] = 32768U;
        binary_cdf[2] = 0U;
        if (has_columns) {
            split_probability = av1_partition_probability(cdf, AV1_PARTITION_VERT) +
                                av1_partition_probability(cdf, AV1_PARTITION_SPLIT) +
                                av1_partition_probability(cdf, AV1_PARTITION_HORZ_A) +
                                av1_partition_probability(cdf, AV1_PARTITION_VERT_A) +
                                av1_partition_probability(cdf, AV1_PARTITION_VERT_B);
            if (block_mi != 32U) {
                split_probability += av1_partition_probability(cdf, AV1_PARTITION_VERT_4);
            }
            binary_cdf[0] = (uint16_t)(32768U - split_probability);
            *partition = av1_symbol_read(entropy->decoder, binary_cdf, 2U)
                         ? AV1_PARTITION_SPLIT : AV1_PARTITION_HORZ;
        } else {
            split_probability = av1_partition_probability(cdf, AV1_PARTITION_HORZ) +
                                av1_partition_probability(cdf, AV1_PARTITION_SPLIT) +
                                av1_partition_probability(cdf, AV1_PARTITION_HORZ_A) +
                                av1_partition_probability(cdf, AV1_PARTITION_HORZ_B) +
                                av1_partition_probability(cdf, AV1_PARTITION_VERT_A);
            if (block_mi != 32U) {
                split_probability += av1_partition_probability(cdf, AV1_PARTITION_HORZ_4);
            }
            binary_cdf[0] = (uint16_t)(32768U - split_probability);
            *partition = av1_symbol_read(entropy->decoder, binary_cdf, 2U)
                         ? AV1_PARTITION_SPLIT : AV1_PARTITION_VERT;
        }
    }
    return entropy->decoder->status;
}

static int av1_partition_inside(const Av1PartitionGrid *grid,
                                uint32_t row,
                                uint32_t column) {
    return row >= grid->tile_row_start && row < grid->tile_row_end &&
           column >= grid->tile_column_start && column < grid->tile_column_end &&
           row < grid->mi_rows && column < grid->mi_columns;
}

static size_t av1_partition_grid_index(const Av1PartitionGrid *grid,
                                       uint32_t row,
                                       uint32_t column) {
    return (size_t)row * grid->mi_columns + column;
}

static AvifdecStatus av1_partition_block(Av1PartitionGrid *grid,
                                         uint32_t row,
                                         uint32_t column,
                                         uint32_t width,
                                         uint32_t height,
                                         Av1PartitionTrace *trace) {
    uint32_t row_end = row + height;
    uint32_t column_end = column + width;
    uint32_t current_row;
    uint32_t current_column;

    if (row_end < row || column_end < column) return AVIFDEC_OVERFLOW;
    if (row_end > grid->mi_rows) row_end = grid->mi_rows;
    if (column_end > grid->mi_columns) column_end = grid->mi_columns;
    if (row_end > grid->tile_row_end) row_end = grid->tile_row_end;
    if (column_end > grid->tile_column_end) column_end = grid->tile_column_end;
    if (row >= row_end || column >= column_end) return AVIFDEC_OK;
    for (current_row = row; current_row < row_end; ++current_row) {
        for (current_column = column; current_column < column_end; ++current_column) {
            size_t index = av1_partition_grid_index(grid, current_row, current_column);

            if (index >= grid->grid_capacity) return AVIFDEC_LIMIT_EXCEEDED;
            grid->block_widths[index] = (uint8_t)width;
            grid->block_heights[index] = (uint8_t)height;
        }
    }
    if (grid->decode_block != 0) {
        AvifdecStatus status = grid->decode_block(grid->user_data, row, column,
                                                  width, height);

        if (status != AVIFDEC_OK) return status;
    }
    ++trace->block_count;
    av1_partition_hash(trace, row);
    av1_partition_hash(trace, column);
    av1_partition_hash(trace, width);
    av1_partition_hash(trace, height);
    return AVIFDEC_OK;
}

static int av1_partition_allowed(Av1Partition partition,
                                 uint32_t block_mi,
                                 int has_rows,
                                 int has_columns) {
    if (!has_rows && !has_columns) return partition == AV1_PARTITION_SPLIT;
    if (!has_rows) return partition == AV1_PARTITION_HORZ || partition == AV1_PARTITION_SPLIT;
    if (!has_columns) return partition == AV1_PARTITION_VERT || partition == AV1_PARTITION_SPLIT;
    if (block_mi == 2U) return partition <= AV1_PARTITION_SPLIT;
    if (block_mi == 32U) return partition <= AV1_PARTITION_VERT_B;
    return partition <= AV1_PARTITION_VERT_4;
}

static AvifdecStatus av1_partition_decode(Av1PartitionGrid *grid,
                                          uint32_t row,
                                          uint32_t column,
                                          uint32_t block_mi,
                                          unsigned int depth,
                                          Av1PartitionTrace *trace) {
    uint32_t half;
    uint32_t quarter;
    int has_rows;
    int has_columns;
    int available_above;
    int available_left;
    unsigned int context = 0U;
    Av1Partition partition;
    AvifdecStatus status;

    if (!av1_partition_inside(grid, row, column)) return AVIFDEC_OK;
    if (block_mi == 0U || (block_mi & (block_mi - 1U)) != 0U || block_mi > 32U ||
        depth > AV1_PARTITION_MAX_DEPTH) {
        return AVIFDEC_INVALID_DATA;
    }
    if (++trace->partition_nodes > grid->max_partition_nodes) return AVIFDEC_LIMIT_EXCEEDED;
    if (depth > trace->max_depth) trace->max_depth = depth;
    if (block_mi == 1U) return av1_partition_block(grid, row, column, 1U, 1U, trace);
    half = block_mi >> 1;
    quarter = half >> 1;
    has_rows = row + half < grid->mi_rows;
    has_columns = column + half < grid->mi_columns;
    available_above = av1_partition_inside(grid, row - (row != 0U), column) && row != 0U;
    available_left = av1_partition_inside(grid, row, column - (column != 0U)) && column != 0U;
    if (available_above) {
        size_t index = av1_partition_grid_index(grid, row - 1U, column);
        if (index >= grid->grid_capacity) return AVIFDEC_LIMIT_EXCEEDED;
        if (grid->block_widths[index] < block_mi) context |= 1U;
    }
    if (available_left) {
        size_t index = av1_partition_grid_index(grid, row, column - 1U);
        if (index >= grid->grid_capacity) return AVIFDEC_LIMIT_EXCEEDED;
        if (grid->block_heights[index] < block_mi) context |= 2U;
    }
    if (!has_rows && !has_columns) {
        partition = AV1_PARTITION_SPLIT;
    } else {
        if (grid->read_partition == 0) return AVIFDEC_INVALID_ARGUMENT;
        status = grid->read_partition(grid->user_data, row, column, block_mi, context,
                                      has_rows, has_columns, &partition);
        if (status != AVIFDEC_OK) return status;
    }
    if (!av1_partition_allowed(partition, block_mi, has_rows, has_columns)) {
        return AVIFDEC_INVALID_DATA;
    }
    av1_partition_hash(trace, (uint32_t)partition);
    if (partition == AV1_PARTITION_NONE) {
        return av1_partition_block(grid, row, column, block_mi, block_mi, trace);
    }
    if (partition == AV1_PARTITION_HORZ) {
        status = av1_partition_block(grid, row, column, block_mi, half, trace);
        if (status == AVIFDEC_OK && has_rows) {
            status = av1_partition_block(grid, row + half, column, block_mi, half, trace);
        }
        return status;
    }
    if (partition == AV1_PARTITION_VERT) {
        status = av1_partition_block(grid, row, column, half, block_mi, trace);
        if (status == AVIFDEC_OK && has_columns) {
            status = av1_partition_block(grid, row, column + half, half, block_mi, trace);
        }
        return status;
    }
    if (partition == AV1_PARTITION_SPLIT) {
        status = av1_partition_decode(grid, row, column, half, depth + 1U, trace);
        if (status == AVIFDEC_OK) status = av1_partition_decode(grid, row, column + half, half, depth + 1U, trace);
        if (status == AVIFDEC_OK) status = av1_partition_decode(grid, row + half, column, half, depth + 1U, trace);
        if (status == AVIFDEC_OK) status = av1_partition_decode(grid, row + half, column + half, half, depth + 1U, trace);
        return status;
    }
    if (partition == AV1_PARTITION_HORZ_A) {
        status = av1_partition_block(grid, row, column, half, half, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row, column + half, half, half, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row + half, column, block_mi, half, trace);
        return status;
    }
    if (partition == AV1_PARTITION_HORZ_B) {
        status = av1_partition_block(grid, row, column, block_mi, half, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row + half, column, half, half, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row + half, column + half, half, half, trace);
        return status;
    }
    if (partition == AV1_PARTITION_VERT_A) {
        status = av1_partition_block(grid, row, column, half, half, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row + half, column, half, half, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row, column + half, half, block_mi, trace);
        return status;
    }
    if (partition == AV1_PARTITION_VERT_B) {
        status = av1_partition_block(grid, row, column, half, block_mi, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row, column + half, half, half, trace);
        if (status == AVIFDEC_OK) status = av1_partition_block(grid, row + half, column + half, half, half, trace);
        return status;
    }
    if (partition == AV1_PARTITION_HORZ_4) {
        uint32_t index;

        for (index = 0U; index < 4U; ++index) {
            status = av1_partition_block(grid, row + index * quarter, column,
                                         block_mi, quarter, trace);
            if (status != AVIFDEC_OK) return status;
        }
        return AVIFDEC_OK;
    }
    if (partition == AV1_PARTITION_VERT_4) {
        uint32_t index;

        for (index = 0U; index < 4U; ++index) {
            status = av1_partition_block(grid, row, column + index * quarter,
                                         quarter, block_mi, trace);
            if (status != AVIFDEC_OK) return status;
        }
        return AVIFDEC_OK;
    }
    return AVIFDEC_INVALID_DATA;
}

AvifdecStatus av1_partition_superblock(Av1PartitionGrid *grid,
                                       uint32_t row,
                                       uint32_t column,
                                       uint32_t superblock_mi,
                                       Av1PartitionTrace *trace) {
    size_t required;

    if (grid == 0 || trace == 0 || grid->block_widths == 0 || grid->block_heights == 0 ||
        (superblock_mi != 16U && superblock_mi != 32U) ||
        grid->tile_row_start > grid->tile_row_end || grid->tile_row_end > grid->mi_rows ||
        grid->tile_column_start > grid->tile_column_end ||
        grid->tile_column_end > grid->mi_columns || grid->max_partition_nodes == 0U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!avifdec_size_multiply(grid->mi_rows, grid->mi_columns, &required)) {
        return AVIFDEC_OVERFLOW;
    }
    if (required > grid->grid_capacity) return AVIFDEC_LIMIT_EXCEEDED;
    trace->partition_nodes = 0U;
    trace->block_count = 0U;
    trace->max_depth = 0U;
    trace->checksum = (uint64_t)1469598103934665603ULL;
    return av1_partition_decode(grid, row, column, superblock_mi, 0U, trace);
}