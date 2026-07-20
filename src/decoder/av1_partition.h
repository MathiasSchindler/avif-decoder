#ifndef AVIFDEC_AV1_PARTITION_H
#define AVIFDEC_AV1_PARTITION_H

#include "avifdec.h"
#include "av1_symbol.h"

typedef enum {
    AV1_PARTITION_NONE = 0,
    AV1_PARTITION_HORZ = 1,
    AV1_PARTITION_VERT = 2,
    AV1_PARTITION_SPLIT = 3,
    AV1_PARTITION_HORZ_A = 4,
    AV1_PARTITION_HORZ_B = 5,
    AV1_PARTITION_VERT_A = 6,
    AV1_PARTITION_VERT_B = 7,
    AV1_PARTITION_HORZ_4 = 8,
    AV1_PARTITION_VERT_4 = 9
} Av1Partition;

typedef AvifdecStatus (*Av1PartitionRead)(void *user_data,
                                          uint32_t row,
                                          uint32_t column,
                                          uint32_t block_mi,
                                          unsigned int context,
                                          int has_rows,
                                          int has_columns,
                                          Av1Partition *partition);

typedef AvifdecStatus (*Av1PartitionBlock)(void *user_data,
                                           uint32_t row,
                                           uint32_t column,
                                           uint32_t width,
                                           uint32_t height);

typedef struct {
    uint32_t mi_rows;
    uint32_t mi_columns;
    uint32_t tile_row_start;
    uint32_t tile_row_end;
    uint32_t tile_column_start;
    uint32_t tile_column_end;
    uint8_t *block_widths;
    uint8_t *block_heights;
    size_t grid_capacity;
    size_t max_partition_nodes;
    Av1PartitionRead read_partition;
    Av1PartitionBlock decode_block;
    void *user_data;
} Av1PartitionGrid;

typedef struct {
    size_t partition_nodes;
    size_t block_count;
    unsigned int max_depth;
    uint64_t checksum;
} Av1PartitionTrace;

typedef struct {
    uint16_t width8[4][5];
    uint16_t width16[4][11];
    uint16_t width32[4][11];
    uint16_t width64[4][11];
    uint16_t width128[4][9];
} Av1PartitionCdfs;

typedef struct {
    Av1SymbolDecoder *decoder;
    Av1PartitionCdfs *cdfs;
} Av1PartitionEntropy;

void av1_partition_cdfs_init(Av1PartitionCdfs *cdfs);
uint64_t av1_partition_cdfs_checksum(const Av1PartitionCdfs *cdfs);
AvifdecStatus av1_partition_read_entropy(void *user_data,
                                         uint32_t row,
                                         uint32_t column,
                                         uint32_t block_mi,
                                         unsigned int context,
                                         int has_rows,
                                         int has_columns,
                                         Av1Partition *partition);

AvifdecStatus av1_partition_superblock(Av1PartitionGrid *grid,
                                       uint32_t row,
                                       uint32_t column,
                                       uint32_t superblock_mi,
                                       Av1PartitionTrace *trace);

#endif