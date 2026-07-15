#include "av1_tile_internal.h"
#include "av1.h"
#include "base.h"

static size_t av1_block_index(const Av1BlockState *state,
                              uint32_t row,
                              uint32_t column) {
    return (size_t)row * state->mi_columns + column;
}

const Av1BlockCell *av1_block_cell(const Av1BlockState *state,
                                   uint32_t row,
                                   uint32_t column) {
    size_t index = av1_block_index(state, row, column);

    if (index >= state->cell_capacity) return 0;
    return &state->cells[index];
}

static void av1_block_hash_u64(uint64_t *checksum, uint64_t value) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        *checksum ^= (uint8_t)(value >> (index * 8U));
        *checksum *= (uint64_t)1099511628211ULL;
    }
}

AvifdecStatus av1_tile_workspace_requirement(uint32_t width,
                                             uint32_t height,
                                             size_t *required) {
    size_t mi_columns;
    size_t mi_rows;
    size_t cells;
    size_t grid_bytes;
    size_t context_bytes;
    size_t cdf_bytes;

    if (width == 0U || height == 0U || required == 0) return AVIFDEC_INVALID_ARGUMENT;
    mi_columns = 2U * (((size_t)width + 7U) / 8U);
    mi_rows = 2U * (((size_t)height + 7U) / 8U);
    if (!avifdec_size_multiply(mi_rows, mi_columns, &cells) ||
        !avifdec_size_multiply(
            cells, sizeof(Av1BlockCell) + 7U +
                3U * sizeof(Av1RestorationUnit), &grid_bytes) ||
        !avifdec_size_add(mi_rows, mi_columns, &context_bytes) ||
        !avifdec_size_add(context_bytes, 64U, &context_bytes) ||
        !avifdec_size_multiply(context_bytes, 12U, &context_bytes) ||
        !avifdec_size_multiply(
            sizeof(Av1TileCdfs), 3U + AV1_NUM_REF_FRAMES, &cdf_bytes) ||
        !avifdec_size_add(grid_bytes, context_bytes, required) ||
        !avifdec_size_add(*required, cdf_bytes, required) ||
        !avifdec_size_add(*required, 64U * 64U, required) ||
        !avifdec_size_add(*required, 7U, required)) {
        return AVIFDEC_OVERFLOW;
    }
    return AVIFDEC_OK;
}

static void av1_block_hash_byte(Av1BlockTrace *trace, uint8_t value) {
    trace->checksum ^= value;
    trace->checksum *= (uint64_t)1099511628211ULL;
}

static void av1_block_hash_u32(Av1BlockTrace *trace, uint32_t value) {
    unsigned int index;

    for (index = 0U; index < 4U; ++index) {
        av1_block_hash_byte(trace, (uint8_t)(value >> (index * 8U)));
    }
}

AvifdecStatus av1_block_state_init(Av1BlockState *state,
                                   uint32_t mi_rows,
                                   uint32_t mi_columns,
                                   Av1BlockCell *cells,
                                   size_t cell_capacity,
                                   int monochrome,
                                   int subsampling_x,
                                   int subsampling_y) {
    size_t required;

    if (state == 0 || cells == 0 || mi_rows == 0U || mi_columns == 0U ||
        (monochrome != 0 && monochrome != 1) ||
        (subsampling_x != 0 && subsampling_x != 1) ||
        (subsampling_y != 0 && subsampling_y != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!avifdec_size_multiply(mi_rows, mi_columns, &required)) {
        return AVIFDEC_OVERFLOW;
    }
    if (required > cell_capacity) return AVIFDEC_LIMIT_EXCEEDED;
    avifdec_memory_fill(state, 0U, sizeof(*state));
    avifdec_memory_fill(cells, 0U, required * sizeof(*cells));
    state->mi_rows = mi_rows;
    state->mi_columns = mi_columns;
    state->tile_row_end = mi_rows;
    state->tile_column_end = mi_columns;
    state->monochrome = (uint8_t)monochrome;
    state->subsampling_x = (uint8_t)subsampling_x;
    state->subsampling_y = (uint8_t)subsampling_y;
    state->cells = cells;
    state->cell_capacity = cell_capacity;
    return AVIFDEC_OK;
}

AvifdecStatus av1_block_state_set_tile(Av1BlockState *state,
                                       uint32_t row_start,
                                       uint32_t row_end,
                                       uint32_t column_start,
                                       uint32_t column_end) {
    if (state == 0 || row_start >= row_end || row_end > state->mi_rows ||
        column_start >= column_end || column_end > state->mi_columns) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    state->tile_row_start = row_start;
    state->tile_row_end = row_end;
    state->tile_column_start = column_start;
    state->tile_column_end = column_end;
    return AVIFDEC_OK;
}

int av1_block_state_is_inside(const Av1BlockState *state,
                              int64_t row,
                              int64_t column) {
    return state != 0 && row >= (int64_t)state->tile_row_start &&
           row < (int64_t)state->tile_row_end &&
           column >= (int64_t)state->tile_column_start &&
           column < (int64_t)state->tile_column_end;
}

AvifdecStatus av1_block_state_availability(const Av1BlockState *state,
                                           uint32_t row,
                                           uint32_t column,
                                           uint32_t width,
                                           uint32_t height,
                                           Av1BlockAvailability *availability) {
    int has_chroma;

    if (state == 0 || availability == 0 || width == 0U || height == 0U ||
        !av1_block_state_is_inside(state, row, column)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    has_chroma = !state->monochrome;
    if (height == 1U && state->subsampling_y && (row & 1U) == 0U) has_chroma = 0;
    if (width == 1U && state->subsampling_x && (column & 1U) == 0U) has_chroma = 0;
    availability->has_chroma = (uint8_t)has_chroma;
    availability->above = (uint8_t)av1_block_state_is_inside(state,
                                                              (int64_t)row - 1,
                                                              column);
    availability->left = (uint8_t)av1_block_state_is_inside(state,
                                                             row,
                                                             (int64_t)column - 1);
    availability->above_chroma = availability->above;
    availability->left_chroma = availability->left;
    if (has_chroma && height == 1U && state->subsampling_y) {
        availability->above_chroma = (uint8_t)av1_block_state_is_inside(
            state, (int64_t)row - 2, column);
    }
    if (has_chroma && width == 1U && state->subsampling_x) {
        availability->left_chroma = (uint8_t)av1_block_state_is_inside(
            state, row, (int64_t)column - 2);
    }
    if (!has_chroma) {
        availability->above_chroma = 0U;
        availability->left_chroma = 0U;
    }
    return AVIFDEC_OK;
}

void av1_block_trace_init(Av1BlockTrace *trace) {
    if (trace == 0) return;
    trace->block_count = 0U;
    trace->inter_block_count = 0U;
    trace->compound_block_count = 0U;
    trace->checksum = (uint64_t)1469598103934665603ULL;
    trace->mode_checksum = (uint64_t)1469598103934665603ULL;
    trace->inter_mode_checksum = (uint64_t)1469598103934665603ULL;
    trace->mv_stack_checksum = (uint64_t)1469598103934665603ULL;
    trace->mv_checksum = (uint64_t)1469598103934665603ULL;
}

AvifdecStatus av1_block_state_record(Av1BlockState *state,
                                     const Av1BlockTraceFields *fields,
                                     Av1BlockTrace *trace) {
    uint32_t row_end;
    uint32_t column_end;
    uint32_t row;
    uint32_t column;
    Av1BlockCell cell;

    if (state == 0 || fields == 0 || fields->width == 0U ||
        fields->height == 0U || fields->width > 32U || fields->height > 32U ||
        fields->segment_id >= 8U || fields->skip > 1U ||
        fields->skip_mode > 1U || fields->is_inter > 1U ||
        fields->use_intrabc > 1U ||
        fields->y_mode >= 25U || fields->uv_mode >= 14U || fields->tx_size >= 19U ||
        fields->palette_size_y > 8U || fields->palette_size_uv > 8U ||
        fields->use_filter_intra > 1U || fields->filter_intra_mode >= 5U ||
        fields->angle_delta_y < -3 || fields->angle_delta_y > 3 ||
        fields->angle_delta_uv < -3 || fields->angle_delta_uv > 3 ||
        fields->cfl_alpha_u < -16 || fields->cfl_alpha_u > 16 ||
        fields->cfl_alpha_v < -16 || fields->cfl_alpha_v > 16 ||
        fields->ref_mv_index >= AV1_MAX_MV_STACK_SIZE ||
        fields->mv_stack_count > AV1_MAX_MV_STACK_SIZE ||
        !av1_block_state_is_inside(state, fields->row, fields->column)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    row_end = fields->row + fields->height;
    column_end = fields->column + fields->width;
    if (row_end < fields->row || column_end < fields->column) return AVIFDEC_OVERFLOW;
    if (row_end > state->mi_rows) row_end = state->mi_rows;
    if (column_end > state->mi_columns) column_end = state->mi_columns;
    if (row_end > state->tile_row_end) row_end = state->tile_row_end;
    if (column_end > state->tile_column_end) column_end = state->tile_column_end;
    cell.width = (uint8_t)fields->width;
    cell.height = (uint8_t)fields->height;
    cell.segment_id = fields->segment_id;
    cell.skip = fields->skip;
    cell.skip_mode = fields->skip_mode;
    cell.is_inter = fields->is_inter;
    cell.y_mode = fields->y_mode;
    cell.uv_mode = fields->uv_mode;
    cell.tx_size = fields->tx_size;
    cell.palette_size_y = fields->palette_size_y;
    cell.palette_size_uv = fields->palette_size_uv;
    cell.use_filter_intra = fields->use_filter_intra;
    cell.filter_intra_mode = fields->filter_intra_mode;
    cell.ref_frame[0] = fields->ref_frame[0];
    cell.ref_frame[1] = fields->ref_frame[1];
    cell.ref_mv_index = fields->ref_mv_index;
    cell.mv_stack_count = fields->mv_stack_count;
    cell.interp_filter[0] = fields->interp_filter[0];
    cell.interp_filter[1] = fields->interp_filter[1];
    cell.interintra = fields->interintra;
    cell.interintra_mode = fields->interintra_mode;
    cell.use_wedge_interintra = fields->use_wedge_interintra;
    cell.interintra_wedge_index = fields->interintra_wedge_index;
    cell.motion_mode = fields->motion_mode;
    cell.use_intrabc = fields->use_intrabc;
    avifdec_memory_copy(cell.warp_params, fields->warp_params,
                        sizeof(cell.warp_params));
    cell.compound_group_index = fields->compound_group_index;
    cell.compound_index = fields->compound_index;
    cell.compound_type = fields->compound_type;
    cell.wedge_index = fields->wedge_index;
    cell.wedge_sign = fields->wedge_sign;
    cell.diff_mask_inverse = fields->diff_mask_inverse;
    cell.pred_mv[0] = fields->pred_mv[0];
    cell.pred_mv[1] = fields->pred_mv[1];
    cell.mv[0] = fields->mv[0];
    cell.mv[1] = fields->mv[1];
    cell.angle_delta_y = fields->angle_delta_y;
    cell.angle_delta_uv = fields->angle_delta_uv;
    cell.cfl_alpha_u = fields->cfl_alpha_u;
    cell.cfl_alpha_v = fields->cfl_alpha_v;
    avifdec_memory_copy(cell.delta_lf, fields->delta_lf,
                        sizeof(cell.delta_lf));
    avifdec_memory_copy(cell.palette_colors_y, fields->palette_colors_y,
                        sizeof(cell.palette_colors_y));
    avifdec_memory_copy(cell.palette_colors_u, fields->palette_colors_u,
                        sizeof(cell.palette_colors_u));
    avifdec_memory_copy(cell.palette_colors_v, fields->palette_colors_v,
                        sizeof(cell.palette_colors_v));
    for (row = fields->row; row < row_end; ++row) {
        for (column = fields->column; column < column_end; ++column) {
            size_t index = av1_block_index(state, row, column);

            if (index >= state->cell_capacity) return AVIFDEC_LIMIT_EXCEEDED;
            state->cells[index] = cell;
        }
    }
    if (trace == 0) return AVIFDEC_OK;
    av1_block_hash_u32(trace, fields->row);
    av1_block_hash_u32(trace, fields->column);
    av1_block_hash_u32(trace, fields->width);
    av1_block_hash_u32(trace, fields->height);
    av1_block_hash_byte(trace, fields->segment_id);
    av1_block_hash_byte(trace, fields->skip);
    if (fields->skip_mode) av1_block_hash_byte(trace, fields->skip_mode);
    av1_block_hash_byte(trace, fields->is_inter);
    av1_block_hash_byte(trace, fields->y_mode);
    av1_block_hash_byte(trace, fields->uv_mode);
    av1_block_hash_byte(trace, fields->tx_size);
    av1_block_hash_byte(trace, fields->palette_size_y);
    av1_block_hash_byte(trace, fields->palette_size_uv);
    av1_block_hash_byte(trace, fields->use_filter_intra);
    av1_block_hash_byte(trace, fields->filter_intra_mode);
    av1_block_hash_byte(trace, (uint8_t)fields->angle_delta_y);
    av1_block_hash_byte(trace, (uint8_t)fields->angle_delta_uv);
    av1_block_hash_byte(trace, (uint8_t)fields->cfl_alpha_u);
    av1_block_hash_byte(trace, (uint8_t)fields->cfl_alpha_v);
    av1_block_hash_u64(&trace->mode_checksum, fields->row);
    av1_block_hash_u64(&trace->mode_checksum, fields->column);
    av1_block_hash_u64(&trace->mode_checksum, fields->width);
    av1_block_hash_u64(&trace->mode_checksum, fields->height);
    av1_block_hash_u64(&trace->mode_checksum, fields->y_mode);
    av1_block_hash_u64(&trace->mode_checksum, fields->uv_mode);
    if (fields->is_inter) {
        unsigned int stack_index;

        ++trace->inter_block_count;
        if (fields->ref_frame[1] > 0U && fields->ref_frame[1] < 8U) {
            ++trace->compound_block_count;
        }
        av1_block_hash_u64(&trace->inter_mode_checksum, fields->row);
        av1_block_hash_u64(&trace->inter_mode_checksum, fields->column);
        av1_block_hash_u64(&trace->inter_mode_checksum,
                                 fields->ref_frame[0]);
        av1_block_hash_u64(&trace->inter_mode_checksum,
                                 fields->ref_frame[1]);
        av1_block_hash_u64(&trace->inter_mode_checksum,
                                 fields->y_mode);
        av1_block_hash_u64(&trace->inter_mode_checksum,
                                 fields->ref_mv_index);
        av1_block_hash_u64(&trace->mv_stack_checksum,
                                 fields->mv_stack_count);
        for (stack_index = 0U; stack_index < fields->mv_stack_count;
             ++stack_index) {
            av1_block_hash_u64(&trace->mv_stack_checksum,
                (uint32_t)fields->mv_stack[stack_index].mv[0].row);
            av1_block_hash_u64(&trace->mv_stack_checksum,
                (uint32_t)fields->mv_stack[stack_index].mv[0].column);
            av1_block_hash_u64(&trace->mv_stack_checksum,
                (uint32_t)fields->mv_stack[stack_index].mv[1].row);
            av1_block_hash_u64(&trace->mv_stack_checksum,
                (uint32_t)fields->mv_stack[stack_index].mv[1].column);
            av1_block_hash_u64(&trace->mv_stack_checksum,
                fields->mv_stack[stack_index].weight);
        }
        av1_block_hash_u64(&trace->mv_checksum,
                                 (uint32_t)fields->mv[0].row);
        av1_block_hash_u64(&trace->mv_checksum,
                                 (uint32_t)fields->mv[0].column);
        av1_block_hash_u64(&trace->mv_checksum,
                                 (uint32_t)fields->mv[1].row);
        av1_block_hash_u64(&trace->mv_checksum,
                                 (uint32_t)fields->mv[1].column);
    }
    av1_block_hash_u64(&trace->mode_checksum,
                             (uint8_t)fields->angle_delta_y);
    av1_block_hash_u64(&trace->mode_checksum,
                             (uint8_t)fields->angle_delta_uv);
    av1_block_hash_u64(&trace->mode_checksum, fields->use_filter_intra);
    av1_block_hash_u64(&trace->mode_checksum, fields->filter_intra_mode);
    av1_block_hash_u64(&trace->mode_checksum, fields->palette_size_y);
    av1_block_hash_u64(&trace->mode_checksum, fields->palette_size_uv);
    av1_block_hash_u64(&trace->mode_checksum,
                             (uint8_t)fields->cfl_alpha_u);
    av1_block_hash_u64(&trace->mode_checksum,
                             (uint8_t)fields->cfl_alpha_v);
    for (column = 0U; column < fields->palette_size_y; ++column) {
        av1_block_hash_u64(&trace->mode_checksum,
                                 fields->palette_colors_y[column]);
    }
    for (column = 0U; column < fields->palette_size_uv; ++column) {
        av1_block_hash_u64(&trace->mode_checksum,
                                 fields->palette_colors_u[column]);
        av1_block_hash_u64(&trace->mode_checksum,
                                 fields->palette_colors_v[column]);
    }
    ++trace->block_count;
    return AVIFDEC_OK;
}
