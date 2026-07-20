#include "av1_tile_inter_mode.h"
#include "av1_tile_inter_mv.h"
#include "av1_tile_internal.h"
#include "av1.h"
#include "base.h"

static uint32_t av1_tile_inter_abs_int32(int32_t value) {
    return value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
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

AvifdecStatus av1_tile_inter_read_interp_filters(
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
            av1_tile_inter_component_mode(fields->y_mode, list);

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
        av1_tile_inter_abs_int32(cell->mv[0].row - fields->mv[0].row) +
        av1_tile_inter_abs_int32(cell->mv[0].column - fields->mv[0].column);
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

static AvifdecStatus av1_tile_inter_read_motion_mode(
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
            av1_tile_inter_component_mode(fields->y_mode, list) == 15U &&
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
        av1_tile_inter_component_mode(fields->y_mode, 0U) == 15U &&
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

AvifdecStatus av1_tile_inter_read_compound_syntax(
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
        AvifdecStatus status = av1_tile_inter_read_motion_mode(
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
