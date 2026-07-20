#include "av1_tile_inter_mv.h"
#include "av1_tile_internal.h"
#include "av1.h"
#include "av1_inter_predict.h"
#include "base.h"

enum {
    AV1_TILE_INTER_SEG_LVL_SKIP = 6,
    AV1_TILE_INTER_SEG_LVL_GLOBALMV = 7
};

static AvifdecStatus av1_tile_inter_global_mv(
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

static AvifdecStatus av1_tile_inter_find_mv_stack(
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
    status = av1_tile_inter_global_mv(config, fields, 0U, &global[0]);
    if (status != AVIFDEC_OK) return status;
    global[1].row = 0;
    global[1].column = 0;
    if (compound) {
        status = av1_tile_inter_global_mv(config, fields, 1U, &global[1]);
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

static unsigned int av1_tile_inter_drl_context(const Av1MvStack *stack,
                                                unsigned int index) {
    const uint16_t category = 640U;
    uint16_t first = stack->candidates[index].weight;
    uint16_t second = stack->candidates[index + 1U].weight;

    if (first >= category && second >= category) return 0U;
    if (first >= category && second < category) return 1U;
    if (first < category && second < category) return 2U;
    return 0U;
}

uint8_t av1_tile_inter_component_mode(uint8_t mode, unsigned int list) {
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

AvifdecStatus av1_tile_inter_read_mode_and_mvs(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields) {
    Av1MvStack stack;
    size_t feature_offset = (size_t)fields->segment_id * 8U;
    int compound = fields->ref_frame[1] > 0U;
    int forced_global =
        config->segmentation_enabled &&
        (config->feature_enabled[
             feature_offset + AV1_TILE_INTER_SEG_LVL_SKIP] ||
         config->feature_enabled[
             feature_offset + AV1_TILE_INTER_SEG_LVL_GLOBALMV]);
    AvifdecStatus status;
    unsigned int new_mv_context = 0U;
    unsigned int zero_mv_context = 0U;
    unsigned int reference_mv_context = 0U;
    unsigned int list;

    status = av1_tile_inter_find_mv_stack(
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
            unsigned int drl_context =
                av1_tile_inter_drl_context(&stack, index);

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
            unsigned int drl_context =
                av1_tile_inter_drl_context(&stack, index);

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
        uint8_t component_mode = av1_tile_inter_component_mode(fields->y_mode, list);
        unsigned int position = component_mode == 13U ? 0U
            : fields->ref_mv_index;

        if (component_mode == 14U) ++position;
        if (component_mode == 16U &&
            (fields->y_mode == 21U || fields->y_mode == 22U)) {
            ++position;
        }

        if (component_mode == 15U) {
            status = av1_tile_inter_global_mv(config, fields, list,
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

AvifdecStatus av1_tile_inter_read_intrabc(
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
    status = av1_tile_inter_find_mv_stack(
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
