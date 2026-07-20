#include "av1_tile.h"
#include "av1_tile_geometry.h"
#include "av1_tile_internal.h"
#include "av1_tile_inter_mode.h"
#include "av1_tile_inter_mv.h"
#include "av1.h"
#include "av1_inter_predict.h"
#include "av1_predict.h"
#include "base.h"

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
    tx_size = (Av1TxSize)av1_tile_max_tx_size[block_size];
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
    const Av1BlockCell *cell;

    if (!av1_block_state_is_inside(state, row, column)) return 0;
    cell = av1_block_cell(state, (uint32_t)row, (uint32_t)column);
    return cell != 0 && cell->width != 0U ? cell : 0;
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
        uint16_t angle = (uint16_t)(
            directional_angle[mode] + 3 * angle_delta);

        if (!state->enable_intra_edge_filter) {
            return av1_predict_directional(
                destination, stride, width, height,
                state->dequant_params.bit_depth, angle,
                &prepared.references);
        }
        filter_type = av1_tile_intra_filter_type(
            state, block, plane, &availability);
        return av1_predict_directional_edges(
            destination, stride, width, height,
            state->dequant_params.bit_depth,
            angle,
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
            Av1BlockCell *cell = av1_block_cell_mutable(
                state->block_state, row, column);

            if (cell == 0) return AVIFDEC_LIMIT_EXCEEDED;
            cell->tx_size = (uint8_t)tx_size;
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
    sub_tx_size = (Av1TxSize)av1_tile_split_tx_size[tx_size];
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
                 (av1_tile_inter_component_mode(block->y_mode, list) == 15U &&
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
    if (!state->disable_trace) {
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
                        if (!block->is_inter && !state->disable_trace) {
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
                        state->dequant_params.qmatrix =
                            state->qmatrices[plane];
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
