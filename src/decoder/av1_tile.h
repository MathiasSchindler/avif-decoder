#ifndef AVIFDEC_AV1_TILE_H
#define AVIFDEC_AV1_TILE_H

#include "avifdec.h"
#include "av1_coeff.h"
#include "av1_intra.h"
#include "av1_inter.h"
#include "av1_warp.h"
#include "av1_partition.h"
#include "av1_recon.h"

typedef struct {
    uint16_t y_mode[7][3][3];
    uint16_t uv_mode[2][3];
    uint16_t y_size[7][8];
    uint16_t uv_size[7][8];
    uint16_t color[2][7][5][9];
} Av1PaletteCdfs;

typedef struct {
    Av1PartitionCdfs partition;
    uint16_t skip[3][3];
    uint16_t segment_id[3][9];
    uint16_t delta_q[5];
    uint16_t delta_lf[5];
    uint16_t y_mode[4][14];
    uint16_t angle_delta[8][8];
    uint16_t intrabc[3];
    uint16_t dv_joint[5];
    Av1MvComponentCdfs dv_component[2];
    uint16_t tx8[3][3];
    uint16_t tx16[3][4];
    uint16_t tx32[3][4];
    uint16_t tx64[3][4];
    uint16_t txfm_partition[21][3];
    uint16_t palette_y_mode[7][3][3];
    uint16_t palette_uv_mode[2][3];
    uint16_t palette_y_size[7][8];
    uint16_t palette_uv_size[7][8];
    uint16_t palette_color[2][7][5][9];
    uint16_t use_wiener[3];
    uint16_t use_sgrproj[3];
    uint16_t restoration_type[4];
    Av1InterCdfs inter;
    Av1IntraCdfs intra;
    Av1CoeffCdfs coeff;
} Av1TileCdfs;

void av1_palette_cdfs_init(Av1PaletteCdfs *cdfs);

typedef AvifdecStatus (*Av1TileDecodeBlock)(void *user_data,
                                            Av1SymbolDecoder *decoder,
                                            Av1TileCdfs *cdfs,
                                            uint32_t row,
                                            uint32_t column,
                                            uint32_t width,
                                            uint32_t height);

typedef AvifdecStatus (*Av1TileBeforeSuperblock)(void *user_data,
                                                  Av1SymbolDecoder *decoder,
                                                  Av1TileCdfs *cdfs,
                                                  uint32_t row,
                                                  uint32_t column,
                                                  uint32_t superblock_mi);

typedef struct {
    int8_t wiener[2][3];
    int8_t sgr_xqd[2];
    uint8_t type;
    uint8_t sgr_set;
    uint8_t parsed;
} Av1RestorationUnit;

typedef struct {
    Av1RestorationUnit *units;
    size_t unit_capacity;
    size_t plane_offset[3];
    uint32_t unit_rows[3];
    uint32_t unit_columns[3];
    uint16_t unit_size[3];
    uint8_t frame_type[3];
    int8_t ref_wiener[3][2][3];
    int8_t ref_sgr_xqd[3][2];
    uint32_t upscaled_width;
    uint32_t frame_height;
    uint8_t superres_denom;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
} Av1RestorationState;

typedef struct {
    uint8_t width;
    uint8_t height;
    uint8_t segment_id;
    uint8_t skip;
    uint8_t skip_mode;
    uint8_t is_inter;
    uint8_t y_mode;
    uint8_t uv_mode;
    uint8_t tx_size;
    uint8_t palette_size_y;
    uint8_t palette_size_uv;
    uint8_t use_filter_intra;
    uint8_t filter_intra_mode;
    int8_t angle_delta_y;
    int8_t angle_delta_uv;
    int8_t cfl_alpha_u;
    int8_t cfl_alpha_v;
    int8_t delta_lf[4];
    /* Palette colors are intentionally not stored here: they used to be
       broadcast into every mi cell a block covers (48 bytes/cell), which
       dominates whole-frame memory. Above/left palette color prediction
       now uses the tile-local rolling context in av1_tile_internal.h, and
       reconstruction reads colors from the current Av1BlockTraceFields
       instead of the persistent grid. */
    int32_t warp_params[2][6];
    uint8_t ref_frame[2];
    uint8_t ref_mv_index;
    uint8_t mv_stack_count;
    uint8_t interp_filter[2];
    uint8_t interintra;
    uint8_t interintra_mode;
    uint8_t use_wedge_interintra;
    uint8_t interintra_wedge_index;
    uint8_t motion_mode;
    uint8_t use_intrabc;
    uint8_t compound_group_index;
    uint8_t compound_index;
    uint8_t compound_type;
    uint8_t wedge_index;
    uint8_t wedge_sign;
    uint8_t diff_mask_inverse;
    Av1MotionVector pred_mv[2];
    Av1MotionVector mv[2];
} Av1BlockCell;

#define AV1_BLOCK_BASE_CELL_SIZE offsetof(Av1BlockCell, warp_params)

typedef struct {
    uint32_t mi_rows;
    uint32_t mi_columns;
    uint32_t tile_row_start;
    uint32_t tile_row_end;
    uint32_t tile_column_start;
    uint32_t tile_column_end;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    Av1BlockCell *cells;
    size_t cell_capacity;
    size_t cell_stride;
} Av1BlockState;

typedef struct {
    uint8_t has_chroma;
    uint8_t above;
    uint8_t left;
    uint8_t above_chroma;
    uint8_t left_chroma;
} Av1BlockAvailability;

typedef struct {
    uint32_t row;
    uint32_t column;
    uint32_t width;
    uint32_t height;
    uint8_t segment_id;
    uint8_t skip;
    uint8_t skip_mode;
    uint8_t is_inter;
    uint8_t has_chroma;
    uint8_t y_mode;
    uint8_t uv_mode;
    uint8_t tx_size;
    uint8_t palette_size_y;
    uint8_t palette_size_uv;
    uint8_t use_filter_intra;
    uint8_t filter_intra_mode;
    uint8_t q_index;
    uint8_t lossless;
    uint8_t ref_frame[2];
    uint8_t ref_mv_index;
    uint8_t mv_stack_count;
    uint8_t interp_filter[2];
    uint8_t interintra;
    uint8_t interintra_mode;
    uint8_t use_wedge_interintra;
    uint8_t interintra_wedge_index;
    uint8_t motion_mode;
    uint8_t use_intrabc;
    uint8_t warp_sample_count;
    int32_t warp_params[2][6];
    uint8_t compound_group_index;
    uint8_t compound_index;
    uint8_t compound_type;
    uint8_t wedge_index;
    uint8_t wedge_sign;
    uint8_t diff_mask_inverse;
    Av1MotionVector pred_mv[2];
    Av1MotionVector mv[2];
    Av1MvCandidate mv_stack[AV1_MAX_MV_STACK_SIZE];
    int8_t angle_delta_y;
    int8_t angle_delta_uv;
    int8_t cfl_alpha_u;
    int8_t cfl_alpha_v;
    int8_t delta_lf[4];
    uint16_t palette_colors_y[8];
    uint16_t palette_colors_u[8];
    uint16_t palette_colors_v[8];
} Av1BlockTraceFields;

typedef struct {
    size_t block_count;
    size_t inter_block_count;
    size_t compound_block_count;
    uint64_t checksum;
    uint64_t mode_checksum;
    uint64_t inter_mode_checksum;
    uint64_t mv_stack_checksum;
    uint64_t mv_checksum;
} Av1BlockTrace;

typedef AvifdecStatus (*Av1TileBeforeResidual)(void *user_data,
                                               Av1SymbolDecoder *decoder,
                                               const Av1BlockTraceFields *block);

typedef struct {
    Av1BlockState *block_state;
    Av1BlockTrace *block_trace;
    Av1TileBeforeResidual before_residual;
    void *user_data;
    uint8_t *cdef_indices;
    size_t cdef_capacity;
    uint8_t *palette_map;
    size_t palette_map_capacity;
    uint8_t *palette_map_uv;
    size_t palette_map_uv_capacity;
    uint8_t segmentation_enabled;
    uint8_t seg_id_pre_skip;
    uint8_t last_active_segment;
    const uint8_t *feature_enabled;
    const int16_t *feature_data;
    const uint8_t *lossless_array;
    uint8_t allow_intrabc;
    uint8_t allow_screen_content_tools;
    uint8_t inter_frame;
    uint8_t reference_select;
    uint8_t skip_mode_present;
    uint8_t skip_mode_frame[2];
    uint8_t force_integer_mv;
    uint8_t allow_high_precision_mv;
    uint8_t use_ref_frame_mvs;
    const Av1TemporalMotion *temporal_mvs;
    size_t temporal_mv_capacity;
    uint32_t temporal_mv_stride;
    uint32_t current_order_hint;
    uint32_t ref_order_hint[7];
    uint8_t enable_order_hint;
    uint8_t order_hint_bits;
    uint8_t interpolation_filter;
    uint8_t is_motion_mode_switchable;
    uint8_t enable_interintra_compound;
    uint8_t enable_masked_compound;
    uint8_t enable_dist_wtd_comp;
    uint8_t enable_dual_filter;
    uint8_t allow_warped_motion;
    uint8_t gm_type[7];
    int32_t gm_params[7][6];
    uint32_t current_frame_width;
    uint32_t current_frame_height;
    uint32_t reference_width[7];
    uint32_t reference_height[7];
    uint8_t enable_filter_intra;
    uint8_t enable_cdef;
    uint8_t cdef_bits;
    uint8_t lossless;
    uint8_t tx_mode;
    uint8_t base_q_index;
    uint8_t delta_q_present;
    uint8_t delta_q_res;
    uint8_t delta_lf_present;
    uint8_t delta_lf_res;
    uint8_t delta_lf_multi;
    uint8_t monochrome;
    uint8_t bit_depth;
    uint8_t superblock_mi;
    uint8_t disable_trace;
} Av1TileModeConfig;

typedef struct {
    uint16_t *data[3];
    size_t stride[3];
    uint32_t width[3];
    uint32_t height[3];
} Av1FramePlanes;

typedef struct {
    Av1CoeffContextState *coeff_contexts;
    Av1TileCdfs *cdfs;
    uint8_t *tx_types;
    size_t tx_type_capacity;
    uint8_t *loop_filter_tx_sizes[3];
    size_t loop_filter_tx_size_capacity;
    uint32_t mi_rows;
    uint32_t mi_columns;
    size_t transform_count;
    size_t nonzero_transform_count;
    size_t coefficient_count;
    uint32_t transform_size_mask;
    uint32_t transform_type_mask;
    uint64_t checksum;
    uint64_t predictor_checksum;
    uint64_t quantized_checksum;
    uint64_t dequantized_checksum;
    uint64_t residual_checksum;
    int32_t *quantized;
    int32_t *dequantized;
    int32_t *residual;
    size_t quantized_capacity;
    size_t dequantized_capacity;
    size_t residual_capacity;
    Av1DequantParams dequant_params;
    uint8_t qm_y;
    uint8_t qm_u;
    uint8_t qm_v;
    const uint8_t *qmatrices[3];
    Av1BlockState *block_state;
    Av1FramePlanes frame_planes;
    Av1FramePlanes reference_planes[7];
    uint32_t reference_width[7];
    uint32_t reference_height[7];
    uint8_t reference_pixels_valid[7];
    uint32_t current_frame_width;
    uint32_t current_frame_height;
    uint32_t current_order_hint;
    uint32_t reference_order_hint[7];
    uint16_t *inter_pred0;
    uint16_t *inter_pred1;
    uint8_t *inter_mask;
    size_t inter_scratch_capacity;
    uint8_t *palette_map_y;
    uint8_t *palette_map_uv;
    size_t palette_map_capacity;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t reduced_tx_set;
    uint8_t tx_mode;
    uint8_t enable_intra_edge_filter;
    uint8_t enable_order_hint;
    uint8_t order_hint_bits;
    uint8_t base_q_index;
    uint8_t lossless;
    uint8_t disable_trace;
} Av1TileResidualState;

typedef struct {
    const AvifdecSpan *spans;
    size_t span_count;
    size_t start;
    size_t size;
    uint32_t mi_rows;
    uint32_t mi_columns;
    uint32_t tile_row_start;
    uint32_t tile_row_end;
    uint32_t tile_column_start;
    uint32_t tile_column_end;
    uint32_t superblock_mi;
    uint8_t *block_widths;
    uint8_t *block_heights;
    size_t grid_capacity;
    size_t max_partition_nodes;
    uint8_t disable_cdf_update;
    Av1TileBeforeSuperblock before_superblock;
    void *before_superblock_user_data;
    Av1PartitionBlock decode_block;
    Av1TileDecodeBlock decode_mode_block;
    void *user_data;
} Av1TilePartitionConfig;

void av1_tile_cdfs_init(Av1TileCdfs *cdfs);
void av1_tile_cdfs_init_frame(Av1TileCdfs *cdfs, uint8_t base_q_index);
void av1_tile_cdfs_reset_counts(Av1TileCdfs *cdfs);
uint64_t av1_tile_cdfs_checksum(const Av1TileCdfs *cdfs);
AvifdecStatus av1_restoration_state_init(Av1RestorationState *state,
                                         Av1RestorationUnit *units,
                                         size_t unit_capacity,
                                         uint32_t upscaled_width,
                                         uint32_t frame_height,
                                         uint8_t superres_denom,
                                         const uint8_t frame_type[3],
                                         const uint16_t unit_size[3],
                                         int monochrome,
                                         int subsampling_x,
                                         int subsampling_y);
AvifdecStatus av1_restoration_unit_capacity(
    uint32_t width,
    uint32_t height,
    int monochrome,
    int subsampling_x,
    int subsampling_y,
    size_t *capacity);
void av1_restoration_reset_tile(Av1RestorationState *state);
AvifdecStatus av1_tile_read_restoration(void *user_data,
                                        Av1SymbolDecoder *decoder,
                                        Av1TileCdfs *cdfs,
                                        uint32_t row,
                                        uint32_t column,
                                        uint32_t superblock_mi);
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
    size_t palette_map_capacity);
AvifdecStatus av1_tile_parse_residual(void *user_data,
                                      Av1SymbolDecoder *decoder,
                                      const Av1BlockTraceFields *block);
AvifdecStatus av1_tile_workspace_requirement(uint32_t width,
                                             uint32_t height,
                                             int monochrome,
                                             int subsampling_x,
                                             int subsampling_y,
                                             int compact_block_state,
                                             size_t *required);
AvifdecStatus av1_block_state_init(Av1BlockState *state,
                                   uint32_t mi_rows,
                                   uint32_t mi_columns,
                                   Av1BlockCell *cells,
                                   size_t cell_capacity,
                                   int monochrome,
                                   int subsampling_x,
                                   int subsampling_y);
AvifdecStatus av1_block_state_init_compact(Av1BlockState *state,
                                           uint32_t mi_rows,
                                           uint32_t mi_columns,
                                           Av1BlockCell *cells,
                                           size_t cell_capacity,
                                           int monochrome,
                                           int subsampling_x,
                                           int subsampling_y);
AvifdecStatus av1_block_state_set_tile(Av1BlockState *state,
                                       uint32_t row_start,
                                       uint32_t row_end,
                                       uint32_t column_start,
                                       uint32_t column_end);
int av1_block_state_is_inside(const Av1BlockState *state,
                              int64_t row,
                              int64_t column);
AvifdecStatus av1_block_state_availability(const Av1BlockState *state,
                                           uint32_t row,
                                           uint32_t column,
                                           uint32_t width,
                                           uint32_t height,
                                           Av1BlockAvailability *availability);
void av1_block_trace_init(Av1BlockTrace *trace);
AvifdecStatus av1_block_state_record(Av1BlockState *state,
                                     const Av1BlockTraceFields *fields,
                                     Av1BlockTrace *trace);

#include "av1_tile_mode.h"

#endif