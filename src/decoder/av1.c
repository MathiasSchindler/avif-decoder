#include "av1.h"
#include "av1_bitstream.h"
#include "av1_copy.h"
#include "av1_filter.h"
#include "av1_frame_header.h"
#include "av1_metadata.h"
#include "av1_parse.h"
#include "av1_predict.h"
#include "av1_tile.h"
#include "av1_tile_internal.h"
#include "base.h"
#include "bmff.h"

#define AV1_OBU_SEQUENCE_HEADER 1U
#define AV1_OBU_TEMPORAL_DELIMITER 2U
#define AV1_OBU_FRAME_HEADER 3U
#define AV1_OBU_TILE_GROUP 4U
#define AV1_OBU_METADATA 5U
#define AV1_OBU_FRAME 6U
#define AV1_OBU_REDUNDANT_FRAME_HEADER 7U
#define AV1_OBU_TILE_LIST 8U
#define AV1_OBU_PADDING 15U

typedef struct {
    Av1PartitionTrace partition;
    Av1BlockTrace block;
    Av1TileResidualState residual;
    uint64_t cdf_checksum;
    AvifdecStatus status;
} Av1TileDecodeResult;

typedef struct {
    const Av1Bits *bits;
    size_t tile;
    size_t position;
    size_t size;
} Av1TileWorkItem;

typedef struct Av1TileWorker Av1TileWorker;

typedef struct Av1TraceState {
    AvifdecArena arena;
    AvifdecEntropyTrace *trace;
    Av1TileCdfs *frame_cdfs;
    Av1TileCdfs *tile_cdfs;
    Av1TileCdfs *context_update_cdfs;
    Av1TileCdfs *reference_cdfs;
    uint8_t reference_cdf_valid[AV1_NUM_REF_FRAMES];
    Av1SavedMotion *reference_motion;
    Av1SavedMotion *current_motion;
    Av1TemporalMotion *temporal_motion;
    size_t motion_field_capacity;
    uint32_t motion_field_stride;
    uint32_t current_order_hint;
    uint32_t ref_order_hint[AV1_REFS_PER_FRAME];
    uint32_t ref_width[AV1_REFS_PER_FRAME];
    uint32_t ref_height[AV1_REFS_PER_FRAME];
    uint8_t ref_pixels_valid[AV1_REFS_PER_FRAME];
    Av1FramePlanes logical_reference_planes[AV1_REFS_PER_FRAME];
    uint8_t enable_order_hint;
    uint8_t order_hint_bits;
    Av1BlockCell *cells;
    uint8_t *block_widths;
    uint8_t *block_heights;
    uint8_t *tx_types;
    uint8_t *loop_filter_tx_sizes[3];
    uint8_t *cdef_indices;
    uint8_t *palette_map;
    uint8_t *palette_map_uv;
    Av1RestorationUnit *restoration_units;
    int32_t *quantized;
    int32_t *dequantized;
    int32_t *residual_scratch;
    uint8_t *qmatrices[3];
    uint16_t *inter_pred0;
    uint16_t *inter_pred1;
    uint8_t *inter_mask;
    Av1FramePlanes frame_planes;
    Av1FramePlanes deblocked_planes;
    Av1FramePlanes cdef_planes;
    Av1FramePlanes upscaled_deblocked_planes;
    Av1FramePlanes upscaled_cdef_planes;
    Av1FramePlanes restored_planes;
    Av1FramePlanes reference_planes[AV1_NUM_REF_FRAMES];
    int16_t *film_grain_scratch;
    size_t film_grain_scratch_size;
    Av1CoeffContextState coeff_contexts;
    Av1BlockState block_state;
    Av1BlockTrace block_trace;
    Av1TileResidualState residual;
    Av1RestorationState restoration;
    size_t cell_count;
    size_t restoration_unit_capacity;
    const AvifdecExecutor *executor;
    Av1TileWorker *tile_workers;
    Av1TileWorkItem *tile_work_items;
    Av1TileDecodeResult *tile_results;
    size_t tile_worker_count;
    size_t tile_capacity;
    size_t planned_tile_capacity;
    AvifdecImage *image;
    uint32_t output_width;
    uint32_t output_height;
    uint8_t output_spatial_layer;
    uint8_t output_spatial_layer_set;
    uint8_t output_frame_seen;
    uint8_t trace_enabled;
    int initialized;
    int saved_context_update;
} Av1TraceState;

struct Av1TileWorker {
    Av1TraceState state;
};

static AvifdecStatus av1_fail(AvifdecError *error,
                              AvifdecStatus status,
                              size_t offset,
                              uint8_t obu_type) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = AVIFDEC_FOURCC('O', 'B', 'U', obu_type);
    }
    return status;
}

static int av1_stream_range_equal(const Av1Stream *stream,
                                  size_t left,
                                  size_t right,
                                  size_t size) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        uint8_t left_byte;
        uint8_t right_byte;

        if (!av1_stream_byte_at(
                stream, left + index, &left_byte, 0) ||
            !av1_stream_byte_at(
                stream, right + index, &right_byte, 0) ||
            left_byte != right_byte) {
            return 0;
        }
    }
    return 1;
}

static void av1_trace_hash_value(uint64_t *checksum, uint64_t value) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        *checksum ^= (uint8_t)(value >> (index * 8U));
        *checksum *= (uint64_t)1099511628211ULL;
    }
}

static void av1_trace_hash(AvifdecEntropyTrace *trace, uint64_t value) {
    av1_trace_hash_value(&trace->checksum, value);
}

static int av1_cdef_is_identity(const Av1Sequence *sequence,
                                const Av1Frame *frame) {
    uint32_t index;
    uint32_t count;

    if (sequence == 0 || frame == 0) return 0;
    count = (uint32_t)1U << frame->cdef_bits;
    for (index = 0U; index < count; ++index) {
        if (frame->cdef_y_pri_strength[index] != 0U ||
            frame->cdef_y_sec_strength[index] != 0U ||
            (!sequence->monochrome &&
             (frame->cdef_uv_pri_strength[index] != 0U ||
              frame->cdef_uv_sec_strength[index] != 0U))) {
            return 0;
        }
    }
    return 1;
}

static int av1_restoration_is_identity(const Av1Sequence *sequence,
                                       const Av1Frame *frame) {
    unsigned int plane;

    if (sequence == 0 || frame == 0) return 0;
    for (plane = 0U; plane < (sequence->monochrome ? 1U : 3U);
         ++plane) {
        if (frame->restoration_type[plane] != 0U) return 0;
    }
    return 1;
}

static uint32_t av1_motion_abs(int32_t value) {
    return value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
}

static void av1_store_current_motion(
    Av1TraceState *state,
    const Av1Sequence *sequence,
    const Av1ReferenceState *references,
    const Av1Frame *frame) {
    uint32_t rows = (frame->mi_rows + 1U) >> 1;
    uint32_t columns = (frame->mi_cols + 1U) >> 1;
    uint32_t y8;

    if (state == 0 || !state->initialized ||
        state->current_motion == 0) {
        return;
    }
    avifdec_memory_fill(state->current_motion, 0U,
                        state->motion_field_capacity *
                        sizeof(*state->current_motion));
    for (y8 = 0U; y8 < rows; ++y8) {
        uint32_t row = 2U * y8 + 1U;
        uint32_t x8;

        if (row >= frame->mi_rows) row = frame->mi_rows - 1U;
        for (x8 = 0U; x8 < columns; ++x8) {
            uint32_t column = 2U * x8 + 1U;
            const Av1BlockCell *cell;
            Av1SavedMotion *saved;
            unsigned int list;

            if (column >= frame->mi_cols) column = frame->mi_cols - 1U;
            cell = av1_block_cell(&state->block_state, row, column);
            if (cell == 0) continue;
            saved = &state->current_motion[
                (size_t)y8 * state->motion_field_stride + x8];
            for (list = 0U; list < 2U; ++list) {
                uint8_t reference = cell->ref_frame[list];
                uint8_t slot;
                int32_t distance;

                if (!cell->is_inter || reference == 0U || reference > 7U) {
                    continue;
                }
                slot = frame->ref_frame_idx[reference - 1U];
                if (slot >= AV1_NUM_REF_FRAMES ||
                    !references->slots[slot].valid) {
                    continue;
                }
                distance = av1_relative_distance(
                    sequence->enable_order_hint, sequence->order_hint_bits,
                    references->slots[slot].order_hint, frame->order_hint);
                if (distance >= 0 ||
                    av1_motion_abs(cell->mv[list].row) > 4095U ||
                    av1_motion_abs(cell->mv[list].column) > 4095U) {
                    continue;
                }
                saved->mv.row = (int16_t)cell->mv[list].row;
                saved->mv.column = (int16_t)cell->mv[list].column;
                saved->ref_frame = reference;
                saved->valid = 1U;
            }
        }
    }
}

static AvifdecStatus av1_apply_film_grain_output(
    Av1TraceState *state,
    const Av1Sequence *sequence,
    const Av1FilmGrainParams *params) {
    Av1FilmGrainImage grain_image;
    AvifdecImage *image = state->image;
    unsigned int plane;

    if (image == 0 || params == 0 || !params->apply_grain) return AVIFDEC_OK;
    avifdec_memory_fill(&grain_image, 0U, sizeof(grain_image));
    for (plane = 0U; plane < 3U; ++plane) {
        grain_image.plane[plane] = image->planes[plane];
        grain_image.stride[plane] = image->strides[plane];
    }
    grain_image.width = image->widths[0];
    grain_image.height = image->heights[0];
    grain_image.bit_depth = sequence->bit_depth;
    grain_image.mono_chrome = sequence->monochrome;
    grain_image.subsampling_x = sequence->subsampling_x;
    grain_image.subsampling_y = sequence->subsampling_y;
    grain_image.matrix_is_identity = sequence->matrix_coefficients == 0U;
    return av1_film_grain_apply_ex(
        params, &grain_image, state->film_grain_scratch,
        state->film_grain_scratch_size, state->executor);
}

static AvifdecStatus av1_reference_commit(
    Av1ReferenceState *references,
    const Av1Sequence *sequence,
    const Av1Frame *frame,
    Av1TraceState *state) {
    Av1ReferenceSlot saved;
    const Av1FramePlanes *saved_planes = 0;
    unsigned int slot_index;

    if (references == 0 || sequence == 0 || frame == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!frame->show_existing_frame) {
        av1_store_current_motion(state, sequence, references, frame);
    }
    avifdec_memory_fill(&saved, 0U, sizeof(saved));
    saved.frame_id = frame->current_frame_id;
    saved.order_hint = frame->order_hint;
    saved.upscaled_width = frame->upscaled_width;
    saved.frame_height = frame->frame_height;
    saved.render_width = frame->render_width;
    saved.render_height = frame->render_height;
    saved.mi_rows = frame->mi_rows;
    saved.mi_cols = frame->mi_cols;
    saved.frame_type = frame->frame_type;
    saved.valid = 1U;
    saved.showable_frame = frame->showable_frame;
    saved.film_grain = frame->film_grain;
    saved.film_grain_valid = sequence->film_grain_params_present;
    if (state != 0 && state->initialized) {
        if (frame->show_existing_frame) {
            const Av1ReferenceSlot *shown =
                &references->slots[frame->frame_to_show_map_idx];

            if (shown->pixels_valid) {
                saved.pixels_valid = 1U;
                saved_planes =
                    &state->reference_planes[frame->frame_to_show_map_idx];
            }
        } else if (state->reference_planes[0].data[0] != 0) {
            saved.pixels_valid = 1U;
            saved_planes = &state->restored_planes;
        }
    }
    saved.segmentation_enabled = frame->segmentation_enabled;
    avifdec_memory_copy(saved.feature_enabled, frame->feature_enabled,
                        sizeof(saved.feature_enabled));
    avifdec_memory_copy(saved.feature_data, frame->feature_data,
                        sizeof(saved.feature_data));
    avifdec_memory_copy(saved.gm_params, frame->gm_params,
                        sizeof(saved.gm_params));
    for (slot_index = 0U; slot_index < AV1_REFS_PER_FRAME; ++slot_index) {
        uint8_t reference_slot = frame->ref_frame_idx[slot_index];

        if (reference_slot < AV1_NUM_REF_FRAMES &&
            references->slots[reference_slot].valid) {
            saved.ref_order_hint[slot_index] =
                references->slots[reference_slot].order_hint;
        }
    }
    for (slot_index = 0U; slot_index < AV1_NUM_REF_FRAMES; ++slot_index) {
        if ((frame->refresh_frame_flags & (1U << slot_index)) != 0U) {
            AvifdecStatus status;
            const AvifdecExecutor *executor =
                state != 0 ? state->executor : 0;

            /*
             * Slots are committed strictly in ascending order below (the
             * reference commit order the caller relies on); only the
             * per-slot plane/motion copy work itself is parallelized
             * internally, via the executor, across that slot's own
             * disjoint rows.
             */
            if (saved_planes != 0) {
                status = av1_copy_planes(
                    &state->reference_planes[slot_index], saved_planes,
                    sequence, frame->upscaled_width, frame->frame_height,
                    executor);
                if (status != AVIFDEC_OK) return status;
            }
            references->slots[slot_index] = saved;
            if (state != 0 && state->initialized &&
                state->reference_motion != 0 &&
                state->current_motion != 0 &&
                !frame->show_existing_frame) {
                status = av1_flat_copy_dispatch(
                    executor,
                    &state->reference_motion[
                        (size_t)slot_index * state->motion_field_capacity],
                    state->current_motion,
                    state->motion_field_capacity,
                    sizeof(*state->current_motion));
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    if (!frame->show_existing_frame) {
        references->previous_frame_id = frame->current_frame_id;
        references->have_previous_frame_id = 1U;
    }
    if (state == 0 || !state->trace_enabled) return AVIFDEC_OK;
    ++state->trace->frame_count;
    if (frame->show_existing_frame) ++state->trace->show_existing_frame_count;
    av1_trace_hash_value(&state->trace->reference_state_checksum,
                         state->trace->frame_count);
    av1_trace_hash_value(&state->trace->reference_state_checksum,
                         frame->show_existing_frame);
    av1_trace_hash_value(&state->trace->reference_state_checksum,
                         frame->refresh_frame_flags);
    for (slot_index = 0U; slot_index < AV1_NUM_REF_FRAMES; ++slot_index) {
        const Av1ReferenceSlot *slot = &references->slots[slot_index];

        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot_index);
        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot->valid);
        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot->frame_id);
        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot->order_hint);
        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot->upscaled_width);
        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot->frame_height);
        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot->frame_type);
        av1_trace_hash_value(&state->trace->reference_state_checksum,
                             slot->showable_frame);
    }
    return AVIFDEC_OK;
}

static int av1_motion_field_position(
    uint32_t rows,
    uint32_t columns,
    uint32_t source_row,
    uint32_t source_column,
    Av1MotionVector projected,
    int sign_bias,
    uint32_t *destination_row,
    uint32_t *destination_column) {
    int32_t row_offset = projected.row >= 0
        ? projected.row >> 6 : -((-projected.row) >> 6);
    int32_t column_offset = projected.column >= 0
        ? projected.column >> 6 : -((-projected.column) >> 6);
    int32_t row = (int32_t)source_row +
        (sign_bias ? -row_offset : row_offset);
    int32_t column = (int32_t)source_column +
        (sign_bias ? -column_offset : column_offset);
    int32_t base_row = (int32_t)(source_row & ~7U);
    int32_t base_column = (int32_t)(source_column & ~7U);

    if (row < 0 || column < 0 || (uint32_t)row >= rows ||
        (uint32_t)column >= columns || row < base_row ||
        row >= base_row + 8 || column < base_column - 8 ||
        column >= base_column + 16) {
        return 0;
    }
    *destination_row = (uint32_t)row;
    *destination_column = (uint32_t)column;
    return 1;
}

static int av1_project_motion_field(
    Av1TraceState *state,
    const Av1Sequence *sequence,
    const Av1ReferenceState *references,
    const Av1Frame *frame,
    uint8_t source_reference,
    int direction) {
    uint8_t slot_index;
    const Av1ReferenceSlot *slot;
    const Av1SavedMotion *source;
    uint32_t rows = (frame->mi_rows + 1U) >> 1;
    uint32_t columns = (frame->mi_cols + 1U) >> 1;
    int32_t source_to_current;
    uint32_t row;

    if (source_reference == 0U || source_reference > 7U) return 0;
    slot_index = frame->ref_frame_idx[source_reference - 1U];
    if (slot_index >= AV1_NUM_REF_FRAMES ||
        !references->slots[slot_index].valid) {
        return 0;
    }
    slot = &references->slots[slot_index];
    if (slot->frame_type == 0U || slot->frame_type == 2U ||
        slot->mi_rows != frame->mi_rows || slot->mi_cols != frame->mi_cols) {
        return 0;
    }
    source_to_current = av1_relative_distance(
        sequence->enable_order_hint, sequence->order_hint_bits,
        slot->order_hint, frame->order_hint);
    if (direction == 2) source_to_current = -source_to_current;
    source = &state->reference_motion[
        (size_t)slot_index * state->motion_field_capacity];
    for (row = 0U; row < rows; ++row) {
        uint32_t column;

        for (column = 0U; column < columns; ++column) {
            const Av1SavedMotion *saved = &source[
                (size_t)row * state->motion_field_stride + column];
            int32_t reference_offset;
            Av1MotionVector original;
            Av1MotionVector projected;
            uint32_t destination_row;
            uint32_t destination_column;
            Av1TemporalMotion *destination;

            if (!saved->valid || saved->ref_frame == 0U ||
                saved->ref_frame > 7U) {
                continue;
            }
            reference_offset = av1_relative_distance(
                sequence->enable_order_hint, sequence->order_hint_bits,
                slot->order_hint,
                slot->ref_order_hint[saved->ref_frame - 1U]);
            if (reference_offset <= 0 || reference_offset > 31 ||
                source_to_current < -31 || source_to_current > 31) {
                continue;
            }
            original.row = saved->mv.row;
            original.column = saved->mv.column;
            if (av1_mv_project(original, source_to_current,
                               (uint32_t)reference_offset,
                               &projected) != AVIFDEC_OK ||
                !av1_motion_field_position(
                    rows, columns, row, column, projected,
                    direction >> 1, &destination_row,
                    &destination_column)) {
                continue;
            }
            destination = &state->temporal_motion[
                (size_t)destination_row * state->motion_field_stride +
                destination_column];
            destination->mv = saved->mv;
            destination->ref_frame_offset = (int16_t)reference_offset;
            destination->valid = 1U;
        }
    }
    return 1;
}

static int av1_reference_is_future(
    const Av1Sequence *sequence,
    const Av1ReferenceState *references,
    const Av1Frame *frame,
    uint8_t reference) {
    uint8_t slot = frame->ref_frame_idx[reference - 1U];

    return slot < AV1_NUM_REF_FRAMES && references->slots[slot].valid &&
        av1_relative_distance(
            sequence->enable_order_hint, sequence->order_hint_bits,
            references->slots[slot].order_hint, frame->order_hint) > 0;
}

static void av1_setup_motion_field(
    Av1TraceState *state,
    const Av1Sequence *sequence,
    const Av1ReferenceState *references,
    const Av1Frame *frame) {
    int reference_stamp = 2;
    uint8_t last_slot = frame->ref_frame_idx[0];
    uint8_t golden_slot = frame->ref_frame_idx[3];
    int last_overlay = 0;
    unsigned int reference;

    state->current_order_hint = frame->order_hint;
    state->enable_order_hint = sequence->enable_order_hint;
    state->order_hint_bits = sequence->order_hint_bits;
    for (reference = 0U; reference < AV1_REFS_PER_FRAME; ++reference) {
        uint8_t slot = frame->ref_frame_idx[reference];

        state->ref_order_hint[reference] =
            slot < AV1_NUM_REF_FRAMES && references->slots[slot].valid
            ? references->slots[slot].order_hint : 0U;
        state->ref_width[reference] =
            slot < AV1_NUM_REF_FRAMES && references->slots[slot].valid
            ? references->slots[slot].upscaled_width : 0U;
        state->ref_height[reference] =
            slot < AV1_NUM_REF_FRAMES && references->slots[slot].valid
            ? references->slots[slot].frame_height : 0U;
        state->ref_pixels_valid[reference] =
            slot < AV1_NUM_REF_FRAMES &&
            references->slots[slot].valid &&
            references->slots[slot].pixels_valid;
        if (slot < AV1_NUM_REF_FRAMES) {
            state->logical_reference_planes[reference] =
                state->reference_planes[slot];
        } else {
            avifdec_memory_fill(
                &state->logical_reference_planes[reference], 0U,
                sizeof(state->logical_reference_planes[reference]));
        }
    }
    if (state->temporal_motion == 0) return;
    avifdec_memory_fill(state->temporal_motion, 0U,
                        state->motion_field_capacity *
                        sizeof(*state->temporal_motion));
    if (!frame->use_ref_frame_mvs || !sequence->enable_order_hint) return;
    if (last_slot < AV1_NUM_REF_FRAMES && golden_slot < AV1_NUM_REF_FRAMES &&
        references->slots[last_slot].valid &&
        references->slots[golden_slot].valid) {
        last_overlay = references->slots[last_slot].ref_order_hint[6] ==
                       references->slots[golden_slot].order_hint;
    }
    if (last_slot < AV1_NUM_REF_FRAMES &&
        references->slots[last_slot].valid) {
        if (!last_overlay) {
            (void)av1_project_motion_field(
                state, sequence, references, frame, 1U, 2);
        }
        --reference_stamp;
    }
    if (av1_reference_is_future(
            sequence, references, frame, 5U) &&
        av1_project_motion_field(
            state, sequence, references, frame, 5U, 0)) {
        --reference_stamp;
    }
    if (av1_reference_is_future(
            sequence, references, frame, 6U) &&
        av1_project_motion_field(
            state, sequence, references, frame, 6U, 0)) {
        --reference_stamp;
    }
    if (reference_stamp >= 0 && av1_reference_is_future(
            sequence, references, frame, 7U) &&
        av1_project_motion_field(
            state, sequence, references, frame, 7U, 0)) {
        --reference_stamp;
    }
    if (reference_stamp >= 0) {
        (void)av1_project_motion_field(
            state, sequence, references, frame, 2U, 2);
    }
}

static AvifdecStatus av1_trace_prepare(Av1TraceState *state,
                                        const Av1Sequence *sequence,
                                        const Av1Frame *frame) {
    Av1CoeffPlaneContext planes[3];
    uint32_t max_mi_rows;
    uint32_t max_mi_columns;
    size_t cells;
    size_t motion_rows;
    size_t motion_columns;
    size_t motion_samples;
    size_t reference_motion_samples;
    size_t temporal_motion_samples;
    size_t restoration_units;
    size_t cell_stride;
    size_t cell_bytes;
    size_t tile_count;
    size_t tile_capacity;
    size_t worker_count;
    size_t worker_index;
    size_t tile_worker_bytes = 0U;
    size_t tile_item_bytes = 0U;
    size_t tile_result_bytes = 0U;
    unsigned int plane;
    int alias_cdef;
    int alias_superres;
    int alias_restoration;
    int compact_block_state;

    if (state->initialized) return AVIFDEC_OK;
    alias_cdef = sequence->reduced_still_picture_header &&
        av1_cdef_is_identity(sequence, frame);
    alias_superres = sequence->reduced_still_picture_header &&
        frame->upscaled_width == frame->frame_width;
    alias_restoration = sequence->reduced_still_picture_header &&
        av1_restoration_is_identity(sequence, frame);
    compact_block_state = sequence->reduced_still_picture_header &&
        !frame->allow_intrabc;
    max_mi_columns = 2U * ((sequence->max_width + 7U) >> 3);
    max_mi_rows = 2U * ((sequence->max_height + 7U) >> 3);
    if (max_mi_rows == 0U || max_mi_columns == 0U ||
        !avifdec_size_multiply(max_mi_rows, max_mi_columns, &cells)) {
        return AVIFDEC_OVERFLOW;
    }
    if (!avifdec_size_multiply(
            frame->tile_columns, frame->tile_rows, &tile_count)) {
        return AVIFDEC_OVERFLOW;
    }
    tile_capacity = state->planned_tile_capacity > tile_count
        ? state->planned_tile_capacity : tile_count;
    worker_count = state->executor != 0 ? state->executor->worker_count : 1U;
    if (tile_capacity < 2U || worker_count < 2U) worker_count = 1U;
    if (worker_count > 1U &&
        (!avifdec_size_multiply(
             worker_count - 1U, sizeof(Av1TileWorker),
             &tile_worker_bytes) ||
         !avifdec_size_multiply(
             tile_capacity, sizeof(Av1TileWorkItem), &tile_item_bytes) ||
         !avifdec_size_multiply(
             tile_capacity, sizeof(Av1TileDecodeResult),
             &tile_result_bytes))) {
        return AVIFDEC_OVERFLOW;
    }
    cell_stride = compact_block_state
                      ? AV1_BLOCK_BASE_CELL_SIZE
                      : sizeof(Av1BlockCell);
    if (!avifdec_size_multiply(cells, cell_stride, &cell_bytes)) {
        return AVIFDEC_OVERFLOW;
    }
    if (av1_restoration_unit_capacity(
            sequence->max_width, sequence->max_height,
            sequence->monochrome, sequence->subsampling_x,
            sequence->subsampling_y,
            &restoration_units) != AVIFDEC_OK) {
        return AVIFDEC_OVERFLOW;
    }
    motion_columns = ((size_t)sequence->max_width + 7U) / 8U;
    motion_rows = ((size_t)sequence->max_height + 7U) / 8U;
    if (!avifdec_size_multiply(motion_rows, motion_columns,
                               &motion_samples) ||
        !avifdec_size_multiply(motion_samples, AV1_NUM_REF_FRAMES,
                               &reference_motion_samples) ||
        !avifdec_size_multiply(motion_samples, AV1_REFS_PER_FRAME,
                               &temporal_motion_samples) ||
        motion_columns > UINT32_MAX) {
        return AVIFDEC_OVERFLOW;
    }
    state->frame_cdfs = (Av1TileCdfs *)avifdec_arena_allocate(
        &state->arena, sizeof(*state->frame_cdfs), _Alignof(Av1TileCdfs));
    state->tile_cdfs = (Av1TileCdfs *)avifdec_arena_allocate(
        &state->arena, sizeof(*state->tile_cdfs), _Alignof(Av1TileCdfs));
    state->context_update_cdfs = (Av1TileCdfs *)avifdec_arena_allocate(
        &state->arena, sizeof(*state->context_update_cdfs), _Alignof(Av1TileCdfs));
    state->reference_cdfs = (Av1TileCdfs *)avifdec_arena_allocate(
        &state->arena,
        AV1_NUM_REF_FRAMES * sizeof(*state->reference_cdfs),
        _Alignof(Av1TileCdfs));
    if (!sequence->reduced_still_picture_header) {
        state->reference_motion =
            (Av1SavedMotion *)avifdec_arena_allocate(
                &state->arena,
                reference_motion_samples *
                sizeof(*state->reference_motion),
                _Alignof(Av1SavedMotion));
        state->current_motion =
            (Av1SavedMotion *)avifdec_arena_allocate(
                &state->arena,
                motion_samples * sizeof(*state->current_motion),
                _Alignof(Av1SavedMotion));
        state->temporal_motion =
            (Av1TemporalMotion *)avifdec_arena_allocate(
                &state->arena,
                temporal_motion_samples *
                sizeof(*state->temporal_motion),
                _Alignof(Av1TemporalMotion));
    }
    state->cells = (Av1BlockCell *)avifdec_arena_allocate(
        &state->arena, cell_bytes, _Alignof(Av1BlockCell));
    state->block_widths = (uint8_t *)avifdec_arena_allocate(
        &state->arena, cells, _Alignof(uint8_t));
    state->block_heights = (uint8_t *)avifdec_arena_allocate(
        &state->arena, cells, _Alignof(uint8_t));
    state->tx_types = (uint8_t *)avifdec_arena_allocate(
        &state->arena, cells, _Alignof(uint8_t));
    for (plane = 0U; plane < 3U; ++plane) {
        state->loop_filter_tx_sizes[plane] =
            (uint8_t *)avifdec_arena_allocate(
                &state->arena, cells, _Alignof(uint8_t));
    }
    state->cdef_indices = (uint8_t *)avifdec_arena_allocate(
        &state->arena, cells, _Alignof(uint8_t));
    state->palette_map = (uint8_t *)avifdec_arena_allocate(
        &state->arena, 64U * 64U, _Alignof(uint8_t));
    state->palette_map_uv = (uint8_t *)avifdec_arena_allocate(
        &state->arena, 64U * 64U, _Alignof(uint8_t));
    state->restoration_units = (Av1RestorationUnit *)avifdec_arena_allocate(
        &state->arena, restoration_units * sizeof(*state->restoration_units),
        _Alignof(Av1RestorationUnit));
    state->quantized = (int32_t *)avifdec_arena_allocate(
        &state->arena, 1024U * sizeof(int32_t), _Alignof(int32_t));
    state->dequantized = (int32_t *)avifdec_arena_allocate(
        &state->arena, 1024U * sizeof(int32_t), _Alignof(int32_t));
    state->residual_scratch = (int32_t *)avifdec_arena_allocate(
        &state->arena, 4096U * sizeof(int32_t), _Alignof(int32_t));
    for (plane = 0U; plane < 3U; ++plane) {
        state->qmatrices[plane] = (uint8_t *)avifdec_arena_allocate(
            &state->arena, AV1_QM_TOTAL_SIZE, _Alignof(uint8_t));
    }
    if (!compact_block_state) {
        state->inter_pred0 = (uint16_t *)avifdec_arena_allocate(
            &state->arena, 128U * 128U * sizeof(uint16_t),
            _Alignof(uint16_t));
        state->inter_pred1 = (uint16_t *)avifdec_arena_allocate(
            &state->arena, 128U * 128U * sizeof(uint16_t),
            _Alignof(uint16_t));
        state->inter_mask = (uint8_t *)avifdec_arena_allocate(
            &state->arena, 128U * 128U, _Alignof(uint8_t));
    }
    avifdec_memory_fill(planes, 0U, sizeof(planes));
    for (plane = 0U; plane < (sequence->monochrome ? 1U : 3U); ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;
        size_t superblock_mi = sequence->use_128x128_superblock ? 32U : 16U;
        size_t padded_width4 =
            ((max_mi_columns + superblock_mi - 1U) / superblock_mi) *
            superblock_mi;
        size_t padded_height4 =
            ((max_mi_rows + superblock_mi - 1U) / superblock_mi) *
            superblock_mi;
        size_t width4 = (padded_width4 + ((size_t)1U << sub_x) - 1U) >> sub_x;
        size_t height4 = (padded_height4 + ((size_t)1U << sub_y) - 1U) >> sub_y;
        size_t plane_width;
        size_t plane_height;
        size_t plane_samples;

        planes[plane].above_level_context = (uint8_t *)avifdec_arena_allocate(
            &state->arena, width4, _Alignof(uint8_t));
        planes[plane].above_dc_context = (uint8_t *)avifdec_arena_allocate(
            &state->arena, width4, _Alignof(uint8_t));
        planes[plane].left_level_context = (uint8_t *)avifdec_arena_allocate(
            &state->arena, height4, _Alignof(uint8_t));
        planes[plane].left_dc_context = (uint8_t *)avifdec_arena_allocate(
            &state->arena, height4, _Alignof(uint8_t));
        planes[plane].above_capacity = width4;
        planes[plane].left_capacity = height4;
        planes[plane].width4 = width4;
        planes[plane].height4 = height4;
        if (!avifdec_size_multiply(width4, 4U, &plane_width) ||
            !avifdec_size_multiply(height4, 4U, &plane_height) ||
            !avifdec_size_multiply(plane_width, plane_height, &plane_samples) ||
            plane_width > UINT32_MAX || plane_height > UINT32_MAX) {
            return AVIFDEC_OVERFLOW;
        }
        state->frame_planes.data[plane] = (uint16_t *)avifdec_arena_allocate(
            &state->arena, plane_samples * sizeof(uint16_t), _Alignof(uint16_t));
        state->frame_planes.stride[plane] = plane_width;
        state->frame_planes.width[plane] = (uint32_t)plane_width;
        state->frame_planes.height[plane] = (uint32_t)plane_height;
    }
    if (worker_count > 1U) {
        state->tile_workers = (Av1TileWorker *)avifdec_arena_allocate(
            &state->arena, tile_worker_bytes,
            _Alignof(Av1TileWorker));
        state->tile_work_items = (Av1TileWorkItem *)avifdec_arena_allocate(
            &state->arena, tile_item_bytes,
            _Alignof(Av1TileWorkItem));
        state->tile_results = (Av1TileDecodeResult *)avifdec_arena_allocate(
            &state->arena, tile_result_bytes,
            _Alignof(Av1TileDecodeResult));
        state->tile_worker_count = worker_count;
        state->tile_capacity = tile_capacity;
        for (worker_index = 0U; worker_index + 1U < worker_count;
             ++worker_index) {
            Av1TraceState *worker = &state->tile_workers[worker_index].state;

            avifdec_memory_fill(worker, 0U, sizeof(*worker));
            worker->tile_cdfs = (Av1TileCdfs *)avifdec_arena_allocate(
                &state->arena, sizeof(*worker->tile_cdfs),
                _Alignof(Av1TileCdfs));
            worker->quantized = (int32_t *)avifdec_arena_allocate(
                &state->arena, 1024U * sizeof(int32_t), _Alignof(int32_t));
            worker->dequantized = (int32_t *)avifdec_arena_allocate(
                &state->arena, 1024U * sizeof(int32_t), _Alignof(int32_t));
            worker->residual_scratch = (int32_t *)avifdec_arena_allocate(
                &state->arena, 4096U * sizeof(int32_t), _Alignof(int32_t));
            if (!compact_block_state) {
                worker->inter_pred0 = (uint16_t *)avifdec_arena_allocate(
                    &state->arena, 128U * 128U * sizeof(uint16_t),
                    _Alignof(uint16_t));
                worker->inter_pred1 = (uint16_t *)avifdec_arena_allocate(
                    &state->arena, 128U * 128U * sizeof(uint16_t),
                    _Alignof(uint16_t));
                worker->inter_mask = (uint8_t *)avifdec_arena_allocate(
                    &state->arena, 128U * 128U, _Alignof(uint8_t));
            }
            worker->palette_map = (uint8_t *)avifdec_arena_allocate(
                &state->arena, 64U * 64U, _Alignof(uint8_t));
            worker->palette_map_uv = (uint8_t *)avifdec_arena_allocate(
                &state->arena, 64U * 64U, _Alignof(uint8_t));
            for (plane = 0U;
                 plane < (sequence->monochrome ? 1U : 3U);
                 ++plane) {
                Av1CoeffPlaneContext *context =
                    &worker->coeff_contexts.plane[plane];

                *context = planes[plane];
                context->above_level_context =
                    (uint8_t *)avifdec_arena_allocate(
                        &state->arena, context->above_capacity,
                        _Alignof(uint8_t));
                context->above_dc_context =
                    (uint8_t *)avifdec_arena_allocate(
                        &state->arena, context->above_capacity,
                        _Alignof(uint8_t));
                context->left_level_context =
                    (uint8_t *)avifdec_arena_allocate(
                        &state->arena, context->left_capacity,
                        _Alignof(uint8_t));
                context->left_dc_context =
                    (uint8_t *)avifdec_arena_allocate(
                        &state->arena, context->left_capacity,
                        _Alignof(uint8_t));
            }
        }
    }
    for (plane = 0U; plane < (sequence->monochrome ? 1U : 3U); ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;
        uint32_t upscaled_width = (sequence->max_width +
            ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        uint32_t upscaled_height = (sequence->max_height +
            ((uint32_t)1U << sub_y) - 1U) >> sub_y;
        size_t coded_samples;
        size_t upscaled_samples;
        unsigned int slot;

        if (!avifdec_size_multiply(state->frame_planes.width[plane],
                                   state->frame_planes.height[plane],
                                   &coded_samples) ||
            !avifdec_size_multiply(upscaled_width, upscaled_height,
                                   &upscaled_samples)) {
            return AVIFDEC_OVERFLOW;
        }
        state->deblocked_planes.data[plane] =
            (uint16_t *)avifdec_arena_allocate(
                &state->arena, coded_samples * sizeof(uint16_t),
                _Alignof(uint16_t));
        state->deblocked_planes.stride[plane] =
            state->frame_planes.width[plane];
        state->deblocked_planes.width[plane] = state->frame_planes.width[plane];
        state->deblocked_planes.height[plane] = state->frame_planes.height[plane];
        if (alias_cdef) {
            state->cdef_planes = state->deblocked_planes;
        } else {
            state->cdef_planes.data[plane] =
                (uint16_t *)avifdec_arena_allocate(
                    &state->arena, coded_samples * sizeof(uint16_t),
                    _Alignof(uint16_t));
            state->cdef_planes.stride[plane] =
                state->frame_planes.width[plane];
            state->cdef_planes.width[plane] =
                state->frame_planes.width[plane];
            state->cdef_planes.height[plane] =
                state->frame_planes.height[plane];
        }
        if (alias_superres) {
            state->upscaled_deblocked_planes =
                state->deblocked_planes;
            state->upscaled_cdef_planes = state->cdef_planes;
        } else {
            state->upscaled_deblocked_planes.data[plane] =
                (uint16_t *)avifdec_arena_allocate(
                    &state->arena, upscaled_samples * sizeof(uint16_t),
                    _Alignof(uint16_t));
            state->upscaled_cdef_planes.data[plane] =
                (uint16_t *)avifdec_arena_allocate(
                    &state->arena, upscaled_samples * sizeof(uint16_t),
                    _Alignof(uint16_t));
            state->upscaled_deblocked_planes.stride[plane] =
                upscaled_width;
            state->upscaled_cdef_planes.stride[plane] =
                upscaled_width;
            state->upscaled_deblocked_planes.width[plane] =
                upscaled_width;
            state->upscaled_cdef_planes.width[plane] =
                upscaled_width;
            state->upscaled_deblocked_planes.height[plane] =
                upscaled_height;
            state->upscaled_cdef_planes.height[plane] =
                upscaled_height;
        }
        if (alias_restoration) {
            state->restored_planes =
                state->upscaled_cdef_planes;
        } else {
            state->restored_planes.data[plane] =
                (uint16_t *)avifdec_arena_allocate(
                    &state->arena, upscaled_samples * sizeof(uint16_t),
                    _Alignof(uint16_t));
            state->restored_planes.stride[plane] = upscaled_width;
            state->restored_planes.width[plane] = upscaled_width;
            state->restored_planes.height[plane] = upscaled_height;
        }
        if (!sequence->reduced_still_picture_header) {
            for (slot = 0U; slot < AV1_NUM_REF_FRAMES; ++slot) {
                state->reference_planes[slot].data[plane] =
                    (uint16_t *)avifdec_arena_allocate(
                        &state->arena,
                        upscaled_samples * sizeof(uint16_t),
                        _Alignof(uint16_t));
                state->reference_planes[slot].stride[plane] =
                    upscaled_width;
                state->reference_planes[slot].width[plane] =
                    upscaled_width;
                state->reference_planes[slot].height[plane] =
                    upscaled_height;
                if (state->reference_planes[slot].data[plane] != 0) {
                    avifdec_memory_fill(
                        state->reference_planes[slot].data[plane], 0U,
                        upscaled_samples * sizeof(uint16_t));
                }
            }
        }
    }
    if (sequence->film_grain_params_present) {
        size_t film_grain_scratch_bytes = 0U;

        size_t worker_count =
            state->executor == 0 ? 1U : state->executor->worker_count;

        if (av1_film_grain_scratch_size_ex(
                sequence->max_width, worker_count,
                &film_grain_scratch_bytes) !=
            AVIFDEC_OK) {
            return AVIFDEC_OVERFLOW;
        }
        state->film_grain_scratch = (int16_t *)avifdec_arena_allocate(
            &state->arena, film_grain_scratch_bytes, _Alignof(int16_t));
        state->film_grain_scratch_size = film_grain_scratch_bytes;
    }
    if (state->arena.status != AVIFDEC_OK || state->frame_cdfs == 0 ||
        state->tile_cdfs == 0 || state->context_update_cdfs == 0 ||
        state->reference_cdfs == 0 ||
        (sequence->film_grain_params_present &&
         state->film_grain_scratch == 0) ||
        (!sequence->reduced_still_picture_header &&
         (state->reference_motion == 0 ||
          state->current_motion == 0 ||
          state->temporal_motion == 0)) ||
        state->cells == 0 || state->block_widths == 0 ||
        state->block_heights == 0 || state->tx_types == 0 ||
        state->loop_filter_tx_sizes[0] == 0 ||
        state->loop_filter_tx_sizes[1] == 0 ||
        state->loop_filter_tx_sizes[2] == 0 ||
        state->palette_map == 0 || state->palette_map_uv == 0 ||
        state->restoration_units == 0 ||
        state->deblocked_planes.data[0] == 0 ||
        state->cdef_planes.data[0] == 0 ||
        state->upscaled_deblocked_planes.data[0] == 0 ||
        state->upscaled_cdef_planes.data[0] == 0 ||
        state->restored_planes.data[0] == 0 ||
        (!sequence->reduced_still_picture_header &&
         (state->reference_planes[0].data[0] == 0 ||
          state->reference_planes[
              AV1_NUM_REF_FRAMES - 1U].data[0] == 0)) ||
        state->frame_planes.data[0] == 0 ||
        (!sequence->monochrome &&
         (state->frame_planes.data[1] == 0 ||
          state->frame_planes.data[2] == 0 ||
          (!sequence->reduced_still_picture_header &&
           (state->reference_planes[0].data[1] == 0 ||
            state->reference_planes[0].data[2] == 0 ||
            state->reference_planes[
                AV1_NUM_REF_FRAMES - 1U].data[1] == 0 ||
            state->reference_planes[
                AV1_NUM_REF_FRAMES - 1U].data[2] == 0)))) ||
        state->quantized == 0 ||
        state->dequantized == 0 || state->residual_scratch == 0 ||
        state->qmatrices[0] == 0 || state->qmatrices[1] == 0 ||
        state->qmatrices[2] == 0 ||
        (!compact_block_state &&
         (state->inter_pred0 == 0 || state->inter_pred1 == 0 ||
          state->inter_mask == 0))) {
        return state->arena.status;
    }
    state->cell_count = cells;
    state->restoration_unit_capacity = restoration_units;
    avifdec_memory_fill(state->cdef_indices, 0xffU, state->cell_count);
    if (av1_coeff_context_init(&state->coeff_contexts, planes) != AVIFDEC_OK ||
        (compact_block_state
             ? av1_block_state_init_compact(
                   &state->block_state, frame->mi_rows, frame->mi_cols,
                   state->cells, cells, sequence->monochrome,
                   sequence->subsampling_x, sequence->subsampling_y)
             : av1_block_state_init(
                   &state->block_state, frame->mi_rows, frame->mi_cols,
                   state->cells, cells, sequence->monochrome,
                   sequence->subsampling_x, sequence->subsampling_y)) !=
            AVIFDEC_OK) {
        return AVIFDEC_INVALID_DATA;
    }
    av1_tile_cdfs_init_frame(state->frame_cdfs, frame->base_q_index);
    avifdec_memory_fill(state->reference_cdf_valid, 0U,
                        sizeof(state->reference_cdf_valid));
    if (!sequence->reduced_still_picture_header) {
        avifdec_memory_fill(state->reference_motion, 0U,
                            reference_motion_samples *
                            sizeof(*state->reference_motion));
        avifdec_memory_fill(state->current_motion, 0U,
                            motion_samples *
                            sizeof(*state->current_motion));
        avifdec_memory_fill(state->temporal_motion, 0U,
                            temporal_motion_samples *
                            sizeof(*state->temporal_motion));
    }
    if (av1_restoration_state_init(
            &state->restoration, state->restoration_units, restoration_units,
            frame->upscaled_width, frame->frame_height, frame->superres_denom,
            frame->restoration_type, frame->loop_restoration_size,
            sequence->monochrome, sequence->subsampling_x,
            sequence->subsampling_y) != AVIFDEC_OK) {
        return AVIFDEC_INVALID_DATA;
    }
    state->motion_field_capacity = motion_samples;
    state->motion_field_stride = (uint32_t)motion_columns;
    state->initialized = 1;
    return AVIFDEC_OK;
}

static AvifdecStatus av1_trace_begin_frame(
    Av1TraceState *state,
    const Av1Sequence *sequence,
    const Av1ReferenceState *references,
    const Av1Frame *frame) {
    AvifdecStatus status;
    unsigned int plane;
    uint8_t slot;

    if (state == 0 || sequence == 0 || references == 0 || frame == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    state->saved_context_update = 0U;
    status = av1_trace_prepare(state, sequence, frame);
    if (status != AVIFDEC_OK) return status;
    status = sequence->reduced_still_picture_header && !frame->allow_intrabc
                 ? av1_block_state_init_compact(
                       &state->block_state, frame->mi_rows, frame->mi_cols,
                       state->cells, state->cell_count, sequence->monochrome,
                       sequence->subsampling_x, sequence->subsampling_y)
                 : av1_block_state_init(
                       &state->block_state, frame->mi_rows, frame->mi_cols,
                       state->cells, state->cell_count, sequence->monochrome,
                       sequence->subsampling_x, sequence->subsampling_y);
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(state->block_widths, 0U, state->cell_count);
    avifdec_memory_fill(state->block_heights, 0U, state->cell_count);
    if (frame->using_qmatrix) {
        const uint8_t levels[3] = {
            frame->qm_y, frame->qm_u, frame->qm_v
        };

        for (plane = 0U; plane < 3U; ++plane) {
            if (levels[plane] < 15U) {
                status = av1_recon_qmatrix_decode(
                    levels[plane], plane != 0U,
                    state->qmatrices[plane], AV1_QM_TOTAL_SIZE);
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    status = av1_restoration_state_init(
        &state->restoration, state->restoration_units,
        state->restoration_unit_capacity, frame->upscaled_width,
        frame->frame_height, frame->superres_denom,
        frame->restoration_type, frame->loop_restoration_size,
        sequence->monochrome, sequence->subsampling_x,
        sequence->subsampling_y);
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(
        state->cdef_indices, 0xffU, state->cell_count);
    for (plane = 0U; plane < (sequence->monochrome ? 1U : 3U); ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;
        uint32_t plane_width = (frame->mi_cols * 4U) >> sub_x;
        uint32_t plane_height = (frame->mi_rows * 4U) >> sub_y;
        uint32_t row;

        if (plane_width == state->frame_planes.stride[plane]) {
            avifdec_memory_fill(
                state->frame_planes.data[plane], 0U,
                (size_t)plane_width * plane_height * sizeof(uint16_t));
            continue;
        }
        for (row = 0U; row < plane_height; ++row) {
            avifdec_memory_fill(
                state->frame_planes.data[plane] +
                    (size_t)row * state->frame_planes.stride[plane],
                0U, (size_t)plane_width * sizeof(uint16_t));
        }
    }
    av1_setup_motion_field(state, sequence, references, frame);
    if (frame->primary_ref_frame == 7U) {
        av1_tile_cdfs_init_frame(state->frame_cdfs, frame->base_q_index);
        return AVIFDEC_OK;
    }
    if (frame->primary_ref_frame >= AV1_REFS_PER_FRAME) {
        return AVIFDEC_INVALID_DATA;
    }
    slot = frame->ref_frame_idx[frame->primary_ref_frame];
    if (slot >= AV1_NUM_REF_FRAMES || !references->slots[slot].valid ||
        !state->reference_cdf_valid[slot]) {
        return AVIFDEC_INVALID_DATA;
    }
    avifdec_memory_copy(state->frame_cdfs, &state->reference_cdfs[slot],
                        sizeof(*state->frame_cdfs));
    return AVIFDEC_OK;
}

static AvifdecStatus av1_trace_finish_cdfs(Av1TraceState *state,
                                           const Av1Frame *frame) {
    const Av1TileCdfs *final_cdfs;
    unsigned int slot;

    if (state == 0 || frame == 0 || !state->initialized) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!frame->disable_frame_end_update_cdf &&
        !state->saved_context_update) {
        return AVIFDEC_INVALID_DATA;
    }
    final_cdfs = frame->disable_frame_end_update_cdf
                 ? state->frame_cdfs : state->context_update_cdfs;
    if (!frame->disable_frame_end_update_cdf) {
        av1_tile_cdfs_reset_counts(state->context_update_cdfs);
    }
    avifdec_memory_copy(state->frame_cdfs, final_cdfs,
                        sizeof(*state->frame_cdfs));
    for (slot = 0U; slot < AV1_NUM_REF_FRAMES; ++slot) {
        if ((frame->refresh_frame_flags & (1U << slot)) != 0U) {
            avifdec_memory_copy(&state->reference_cdfs[slot], final_cdfs,
                                sizeof(*final_cdfs));
            state->reference_cdf_valid[slot] = 1U;
        }
    }
    return AVIFDEC_OK;
}

typedef struct {
    const Av1FramePlanes *planes;
    uint32_t plane_width[3];
    uint32_t plane_height[3];
    uint64_t plane_checksum[3];
    AvifdecStatus plane_status[3];
} Av1StageChecksumPlan;

static AvifdecStatus av1_trace_stage_checksum_range(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg) {
    Av1StageChecksumPlan *plan = (Av1StageChecksumPlan *)arg;
    size_t plane;

    (void)worker_index;
    if (plan == 0 || begin > end || end > 3U) return AVIFDEC_INVALID_ARGUMENT;
    for (plane = begin; plane < end; ++plane) {
        plan->plane_status[plane] = av1_predict_checksum(
            plan->planes->data[plane], plan->planes->stride[plane],
            plan->plane_width[plane], plan->plane_height[plane],
            (uint8_t)plane, &plan->plane_checksum[plane]);
    }
    return AVIFDEC_OK;
}

/*
 * Computes one diagnostic stage checksum. Each plane's independent
 * av1_predict_checksum scan (the expensive per-pixel work) may run in
 * parallel across state->executor; the resulting per-plane checksums are
 * always folded into *checksum serially in plane order (0, 1, 2), stopping
 * at the first failing plane exactly as the previous serial loop did, so
 * both the final checksum value and its ordered combination are unchanged.
 */
static AvifdecStatus av1_trace_stage_checksum(
    uint64_t *checksum,
    const Av1FramePlanes *planes,
    const Av1Sequence *sequence,
    uint32_t width,
    uint32_t height,
    const AvifdecExecutor *executor) {
    unsigned int plane_count = sequence->monochrome ? 1U : 3U;
    unsigned int plane;
    Av1StageChecksumPlan plan;
    AvifdecStatus status;

    avifdec_memory_fill(&plan, 0U, sizeof(plan));
    plan.planes = planes;
    for (plane = 0U; plane < plane_count; ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;

        plan.plane_width[plane] =
            (width + ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        plan.plane_height[plane] =
            (height + ((uint32_t)1U << sub_y) - 1U) >> sub_y;
    }
    if (executor != 0 && executor->worker_count > 1U && plane_count > 1U) {
        status = executor->parallel_for(
            executor->user_data, plane_count, 1U,
            av1_trace_stage_checksum_range, &plan);
    } else {
        status = av1_trace_stage_checksum_range(0U, plane_count, 0U, &plan);
    }
    if (status != AVIFDEC_OK) return status;
    for (plane = 0U; plane < plane_count; ++plane) {
        if (plan.plane_status[plane] != AVIFDEC_OK) {
            return plan.plane_status[plane];
        }
        av1_trace_hash_value(checksum, plan.plane_checksum[plane]);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_trace_copy_visible_planes(
    Av1FramePlanes *output,
    const Av1FramePlanes *input,
    const Av1Sequence *sequence,
    uint32_t width,
    uint32_t height,
    const AvifdecExecutor *executor) {
    unsigned int plane_count = sequence->monochrome ? 1U : 3U;
    unsigned int plane;
    Av1PlaneCopyPlan plan;

    avifdec_memory_fill(&plan, 0U, sizeof(plan));
    plan.plane_count = plane_count;
    for (plane = 0U; plane < plane_count; ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;
        uint32_t plane_width = (width + ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        uint32_t plane_height = (height + ((uint32_t)1U << sub_y) - 1U) >> sub_y;

        if (output->data[plane] == 0 || input->data[plane] == 0 ||
            output->stride[plane] < plane_width ||
            input->stride[plane] < plane_width) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        plan.dst_data[plane] = output->data[plane];
        plan.dst_stride[plane] = output->stride[plane];
        plan.src_data[plane] = input->data[plane];
        plan.src_stride[plane] = input->stride[plane];
        plan.width[plane] = plane_width;
        plan.row_offset[plane + 1U] = plan.row_offset[plane] + plane_height;
    }
    return av1_plane_copy_dispatch(executor, &plan);
}

static AvifdecStatus av1_trace_finish_frame(Av1TraceState *state,
                                             const Av1Sequence *sequence,
                                             const Av1Frame *frame) {
    Av1LoopFilterParams loop_filter;
    Av1CdefParams cdef;
    unsigned int plane_count = sequence->monochrome ? 1U : 3U;
    unsigned int plane;
    AvifdecStatus status;

    status = av1_trace_copy_visible_planes(
        &state->deblocked_planes, &state->frame_planes, sequence,
        frame->mi_cols * 4U, frame->mi_rows * 4U, state->executor);
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(&loop_filter, 0U, sizeof(loop_filter));
    loop_filter.frame_width = frame->frame_width;
    loop_filter.frame_height = frame->frame_height;
    loop_filter.mi_rows = frame->mi_rows;
    loop_filter.mi_columns = frame->mi_cols;
    loop_filter.bit_depth = sequence->bit_depth;
    loop_filter.monochrome = sequence->monochrome;
    loop_filter.subsampling_x = sequence->subsampling_x;
    loop_filter.subsampling_y = sequence->subsampling_y;
    avifdec_memory_copy(loop_filter.level, frame->loop_filter_level,
                        sizeof(loop_filter.level));
    loop_filter.sharpness = frame->loop_filter_sharpness;
    loop_filter.delta_enabled = frame->loop_filter_delta_enabled;
    loop_filter.delta_lf_multi = frame->delta_lf_multi;
    avifdec_memory_copy(loop_filter.ref_deltas,
                        frame->loop_filter_ref_deltas,
                        sizeof(loop_filter.ref_deltas));
    avifdec_memory_copy(loop_filter.mode_deltas,
                        frame->loop_filter_mode_deltas,
                        sizeof(loop_filter.mode_deltas));
    loop_filter.segment_feature_enabled = &frame->feature_enabled[0][0];
    loop_filter.segment_feature_data = &frame->feature_data[0][0];
    loop_filter.segment_lossless = frame->lossless;
    loop_filter.tx_sizes[0] = state->loop_filter_tx_sizes[0];
    loop_filter.tx_sizes[1] = state->loop_filter_tx_sizes[1];
    loop_filter.tx_sizes[2] = state->loop_filter_tx_sizes[2];
    loop_filter.tx_size_capacity = state->cell_count;
    status = av1_loop_filter_frame_ex(
        &state->deblocked_planes, &state->block_state, &loop_filter,
        state->executor);
    if (status != AVIFDEC_OK) return status;
    if (state->trace_enabled) {
        status = av1_trace_stage_checksum(
            &state->trace->deblocked_checksum, &state->deblocked_planes,
            sequence, frame->frame_width, frame->frame_height,
            state->executor);
        if (status != AVIFDEC_OK) return status;
    }

    avifdec_memory_fill(&cdef, 0U, sizeof(cdef));
    cdef.frame_width = frame->frame_width;
    cdef.frame_height = frame->frame_height;
    cdef.mi_rows = frame->mi_rows;
    cdef.mi_columns = frame->mi_cols;
    cdef.bit_depth = sequence->bit_depth;
    cdef.monochrome = sequence->monochrome;
    cdef.subsampling_x = sequence->subsampling_x;
    cdef.subsampling_y = sequence->subsampling_y;
    cdef.damping = frame->cdef_damping;
    cdef.bits = frame->cdef_bits;
    cdef.y_pri_strength = frame->cdef_y_pri_strength;
    cdef.y_sec_strength = frame->cdef_y_sec_strength;
    cdef.uv_pri_strength = frame->cdef_uv_pri_strength;
    cdef.uv_sec_strength = frame->cdef_uv_sec_strength;
    cdef.indices = state->cdef_indices;
    cdef.index_capacity = state->cell_count;
    if (state->cdef_planes.data[0] !=
        state->deblocked_planes.data[0]) {
        status = av1_cdef_frame_ex(
            &state->cdef_planes, &state->deblocked_planes,
            &state->block_state, &cdef, state->executor);
        if (status != AVIFDEC_OK) return status;
    }
    if (state->trace_enabled) {
        if (state->cdef_planes.data[0] == state->deblocked_planes.data[0]) {
            /*
             * cdef_planes aliases deblocked_planes (identity CDEF for this
             * stream) and av1_cdef_frame_ex above was skipped, so the
             * buffer contents and dimensions are byte-for-byte identical
             * to what deblocked_checksum already scanned: reuse it
             * instead of rescanning the same pixels.
             */
            state->trace->cdef_checksum = state->trace->deblocked_checksum;
        } else {
            status = av1_trace_stage_checksum(
                &state->trace->cdef_checksum, &state->cdef_planes, sequence,
                frame->frame_width, frame->frame_height, state->executor);
            if (status != AVIFDEC_OK) return status;
        }
    }

    if (state->upscaled_deblocked_planes.data[0] !=
        state->deblocked_planes.data[0]) {
        if (frame->upscaled_width == frame->frame_width) {
            status = av1_trace_copy_visible_planes(
                &state->upscaled_deblocked_planes,
                &state->deblocked_planes, sequence,
                frame->upscaled_width, frame->frame_height,
                state->executor);
            if (status == AVIFDEC_OK) {
                status = av1_trace_copy_visible_planes(
                    &state->upscaled_cdef_planes,
                    &state->cdef_planes, sequence,
                    frame->upscaled_width, frame->frame_height,
                    state->executor);
            }
            if (status != AVIFDEC_OK) return status;
        } else {
            for (plane = 0U; plane < plane_count; ++plane) {
                unsigned int sub_x =
                    plane == 0U ? 0U : sequence->subsampling_x;
                unsigned int sub_y =
                    plane == 0U ? 0U : sequence->subsampling_y;
                uint32_t input_width = (frame->frame_width +
                    ((uint32_t)1U << sub_x) - 1U) >> sub_x;
                uint32_t padded_input_width =
                    (frame->mi_cols * 4U) >> sub_x;
                uint32_t output_width = (frame->upscaled_width +
                    ((uint32_t)1U << sub_x) - 1U) >> sub_x;
                uint32_t height = (frame->frame_height +
                    ((uint32_t)1U << sub_y) - 1U) >> sub_y;
                status = av1_superres_upscale_plane_ex(
                    state->upscaled_deblocked_planes.data[plane],
                    output_width, output_width,
                    state->deblocked_planes.data[plane],
                    state->deblocked_planes.stride[plane],
                    input_width, padded_input_width, height,
                    sequence->bit_depth, state->executor);
                if (status != AVIFDEC_OK) return status;
                status = av1_superres_upscale_plane_ex(
                    state->upscaled_cdef_planes.data[plane],
                    output_width, output_width,
                    state->cdef_planes.data[plane],
                    state->cdef_planes.stride[plane],
                    input_width, padded_input_width, height,
                    sequence->bit_depth, state->executor);
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    if (state->trace_enabled) {
        status = av1_trace_stage_checksum(
            &state->trace->superres_checksum, &state->upscaled_cdef_planes,
            sequence, frame->upscaled_width, frame->frame_height,
            state->executor);
        if (status != AVIFDEC_OK) return status;
    }
    if (state->restored_planes.data[0] !=
        state->upscaled_cdef_planes.data[0]) {
        status = av1_loop_restoration_frame_ex(
            &state->restored_planes, &state->upscaled_cdef_planes,
            &state->upscaled_deblocked_planes, &state->restoration,
            sequence->bit_depth, state->executor);
        if (status != AVIFDEC_OK) return status;
    }
    if (state->trace_enabled) {
        if (state->restored_planes.data[0] ==
            state->upscaled_cdef_planes.data[0]) {
            /*
             * restored_planes aliases upscaled_cdef_planes (identity
             * restoration for this stream) and av1_loop_restoration_frame_ex
             * above was skipped, so restoration_checksum is guaranteed to
             * equal the already-computed superres_checksum: reuse it
             * instead of rescanning the same pixels.
             */
            state->trace->restoration_checksum =
                state->trace->superres_checksum;
        } else {
            status = av1_trace_stage_checksum(
                &state->trace->restoration_checksum, &state->restored_planes,
                sequence, frame->upscaled_width, frame->frame_height,
                state->executor);
            if (status != AVIFDEC_OK) return status;
        }
    }
    if (state->image == 0 || !frame->show_frame) return AVIFDEC_OK;
    if (state->output_spatial_layer_set &&
        frame->spatial_id != state->output_spatial_layer) {
        return AVIFDEC_OK;
    }
    if (frame->upscaled_width < state->output_width ||
        frame->frame_height < state->output_height) {
        return AVIFDEC_OK;
    }
    status = av1_copy_image(
        state->image, &state->restored_planes, sequence,
        frame->upscaled_width, frame->frame_height,
        state->output_width, state->output_height, state->executor);
    if (status != AVIFDEC_OK) return status;
    state->output_frame_seen = 1U;
    if (frame->show_frame && frame->film_grain.apply_grain) {
        status = av1_apply_film_grain_output(state, sequence,
                                             &frame->film_grain);
    }
    return status;
}

static AvifdecStatus av1_trace_tile(Av1TraceState *state,
                                     const Av1Sequence *sequence,
                                     const Av1Frame *frame,
                                     const Av1Bits *bits,
                                     size_t tile,
                                     size_t position,
                                     size_t tile_size,
                                     Av1TileDecodeResult *result) {
    Av1TilePartitionConfig partition_config;
    Av1TileModeConfig mode_config;
    Av1PartitionTrace partition_trace;
    Av1CoeffPlaneContext planes[3];
    uint32_t tile_row = (uint32_t)(tile / frame->tile_columns);
    uint32_t tile_column = (uint32_t)(tile % frame->tile_columns);
    size_t max_partition_nodes;
    unsigned int plane;
    AvifdecStatus status;
    Av1DequantParams dequant_params;

    if (result == 0) return AVIFDEC_INVALID_ARGUMENT;
    status = av1_trace_prepare(state, sequence, frame);
    if (status != AVIFDEC_OK) return status;
    for (plane = 0U; plane < 3U; ++plane) {
        planes[plane] = state->coeff_contexts.plane[plane];
        if (plane == 0U || !sequence->monochrome) {
            unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
            unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;

            planes[plane].width4 =
                (frame->mi_cols + ((uint32_t)1U << sub_x) - 1U) >> sub_x;
            planes[plane].height4 =
                (frame->mi_rows + ((uint32_t)1U << sub_y) - 1U) >> sub_y;
        }
    }
    status = av1_coeff_context_init(&state->coeff_contexts, planes);
    if (status != AVIFDEC_OK) return status;
    status = av1_block_state_set_tile(
        &state->block_state, frame->mi_row_starts[tile_row],
        frame->mi_row_starts[tile_row + 1U],
        frame->mi_col_starts[tile_column],
        frame->mi_col_starts[tile_column + 1U]);
    if (status != AVIFDEC_OK) return status;
    av1_block_trace_init(&state->block_trace);
    av1_restoration_reset_tile(&state->restoration);
    avifdec_memory_fill(&dequant_params, 0U, sizeof(dequant_params));
    dequant_params.bit_depth = sequence->bit_depth;
    dequant_params.using_qmatrix = frame->using_qmatrix;
    dequant_params.delta_q_y_dc = frame->delta_q_y_dc;
    dequant_params.delta_q_u_dc = frame->delta_q_u_dc;
    dequant_params.delta_q_u_ac = frame->delta_q_u_ac;
    dequant_params.delta_q_v_dc = frame->delta_q_v_dc;
    dequant_params.delta_q_v_ac = frame->delta_q_v_ac;
    status = av1_tile_residual_state_init(
        &state->residual, &state->coeff_contexts, state->tile_cdfs,
        state->tx_types, state->cell_count, state->loop_filter_tx_sizes,
        state->cell_count, frame->mi_rows, frame->mi_cols,
        sequence->monochrome, sequence->subsampling_x,
        sequence->subsampling_y, frame->reduced_tx_set, frame->base_q_index,
        frame->coded_lossless, state->quantized, 1024U,
        state->dequantized, 1024U, state->residual_scratch, 4096U,
        &dequant_params, frame->qm_y, frame->qm_u, frame->qm_v,
        &state->block_state, &state->frame_planes,
        state->palette_map, state->palette_map_uv, 64U * 64U);
    if (status != AVIFDEC_OK) return status;
    for (plane = 0U; plane < 3U; ++plane) {
        state->residual.qmatrices[plane] = state->qmatrices[plane];
    }
    state->residual.current_frame_width = frame->frame_width;
    state->residual.current_frame_height = frame->frame_height;
    state->residual.current_order_hint = frame->order_hint;
    state->residual.enable_intra_edge_filter =
        sequence->enable_intra_edge_filter;
    state->residual.enable_order_hint = sequence->enable_order_hint;
    state->residual.order_hint_bits = sequence->order_hint_bits;
    state->residual.tx_mode = frame->tx_mode;
    state->residual.disable_trace = !state->trace_enabled;
    state->residual.inter_pred0 = state->inter_pred0;
    state->residual.inter_pred1 = state->inter_pred1;
    state->residual.inter_mask = state->inter_mask;
    state->residual.inter_scratch_capacity =
        state->inter_pred0 != 0 ? 128U * 128U : 0U;
    for (plane = 0U; plane < AV1_REFS_PER_FRAME; ++plane) {
        state->residual.reference_planes[plane] =
            state->logical_reference_planes[plane];
        state->residual.reference_width[plane] = state->ref_width[plane];
        state->residual.reference_height[plane] = state->ref_height[plane];
        state->residual.reference_pixels_valid[plane] =
            state->ref_pixels_valid[plane];
        state->residual.reference_order_hint[plane] =
            state->ref_order_hint[plane];
    }
    avifdec_memory_fill(&partition_config, 0U, sizeof(partition_config));
    partition_config.spans = bits->stream->spans;
    partition_config.span_count = bits->stream->span_count;
    partition_config.start = position;
    partition_config.size = tile_size;
    partition_config.mi_rows = frame->mi_rows;
    partition_config.mi_columns = frame->mi_cols;
    partition_config.tile_row_start = frame->mi_row_starts[tile_row];
    partition_config.tile_row_end = frame->mi_row_starts[tile_row + 1U];
    partition_config.tile_column_start = frame->mi_col_starts[tile_column];
    partition_config.tile_column_end = frame->mi_col_starts[tile_column + 1U];
    partition_config.superblock_mi = sequence->use_128x128_superblock ? 32U : 16U;
    partition_config.block_widths = state->block_widths;
    partition_config.block_heights = state->block_heights;
    partition_config.grid_capacity = state->cell_count;
    if (!avifdec_size_multiply(state->cell_count, 4U, &max_partition_nodes)) {
        return AVIFDEC_OVERFLOW;
    }
    partition_config.max_partition_nodes = max_partition_nodes;
    partition_config.disable_cdf_update = frame->disable_cdf_update;
    partition_config.before_superblock = av1_tile_read_restoration;
    partition_config.before_superblock_user_data = &state->restoration;
    avifdec_memory_fill(&mode_config, 0U, sizeof(mode_config));
    mode_config.block_state = &state->block_state;
    mode_config.block_trace = &state->block_trace;
    mode_config.before_residual = av1_tile_parse_residual;
    mode_config.user_data = &state->residual;
    mode_config.cdef_indices = state->cdef_indices;
    mode_config.cdef_capacity = state->cell_count;
    mode_config.palette_map = state->palette_map;
    mode_config.palette_map_capacity = 64U * 64U;
    mode_config.palette_map_uv = state->palette_map_uv;
    mode_config.palette_map_uv_capacity = 64U * 64U;
    mode_config.segmentation_enabled = frame->segmentation_enabled;
    mode_config.seg_id_pre_skip = frame->seg_id_pre_skip;
    mode_config.last_active_segment = frame->last_active_segment;
    mode_config.feature_enabled = &frame->feature_enabled[0][0];
    mode_config.feature_data = &frame->feature_data[0][0];
    mode_config.lossless_array = frame->lossless;
    mode_config.allow_intrabc = frame->allow_intrabc;
    mode_config.allow_screen_content_tools = frame->allow_screen_content_tools;
    mode_config.inter_frame = frame->frame_type == 1U || frame->frame_type == 3U;
    mode_config.reference_select = frame->reference_select;
    mode_config.skip_mode_present = frame->skip_mode_present;
    mode_config.skip_mode_frame[0] = frame->skip_mode_frame[0];
    mode_config.skip_mode_frame[1] = frame->skip_mode_frame[1];
    mode_config.force_integer_mv = frame->force_integer_mv;
    mode_config.allow_high_precision_mv = frame->allow_high_precision_mv;
    mode_config.use_ref_frame_mvs = frame->use_ref_frame_mvs;
    mode_config.temporal_mvs = state->temporal_motion;
    mode_config.temporal_mv_capacity = state->motion_field_capacity;
    mode_config.temporal_mv_stride = state->motion_field_stride;
    mode_config.current_order_hint = state->current_order_hint;
    avifdec_memory_copy(mode_config.ref_order_hint, state->ref_order_hint,
                        sizeof(mode_config.ref_order_hint));
    mode_config.enable_order_hint = state->enable_order_hint;
    mode_config.order_hint_bits = state->order_hint_bits;
    mode_config.interpolation_filter = frame->interpolation_filter;
    mode_config.is_motion_mode_switchable = frame->is_motion_mode_switchable;
    mode_config.enable_interintra_compound =
        sequence->enable_interintra_compound;
    mode_config.enable_masked_compound = sequence->enable_masked_compound;
    mode_config.enable_dist_wtd_comp = sequence->enable_jnt_comp;
    mode_config.enable_dual_filter = sequence->enable_dual_filter;
    mode_config.allow_warped_motion = frame->allow_warped_motion;
    avifdec_memory_copy(mode_config.gm_type, frame->gm_type,
                        sizeof(mode_config.gm_type));
    avifdec_memory_copy(mode_config.gm_params, frame->gm_params,
                        sizeof(mode_config.gm_params));
    mode_config.current_frame_width = frame->frame_width;
    mode_config.current_frame_height = frame->frame_height;
    avifdec_memory_copy(mode_config.reference_width, state->ref_width,
                        sizeof(mode_config.reference_width));
    avifdec_memory_copy(mode_config.reference_height, state->ref_height,
                        sizeof(mode_config.reference_height));
    mode_config.enable_filter_intra = sequence->enable_filter_intra;
    mode_config.enable_cdef = sequence->enable_cdef;
    mode_config.cdef_bits = frame->cdef_bits;
    mode_config.lossless = frame->coded_lossless;
    mode_config.tx_mode = frame->tx_mode;
    mode_config.base_q_index = frame->base_q_index;
    mode_config.delta_q_present = frame->delta_q_present;
    mode_config.delta_q_res = frame->delta_q_res;
    mode_config.delta_lf_present = frame->delta_lf_present;
    mode_config.delta_lf_res = frame->delta_lf_res;
    mode_config.delta_lf_multi = frame->delta_lf_multi;
    mode_config.monochrome = sequence->monochrome;
    mode_config.bit_depth = sequence->bit_depth;
    mode_config.superblock_mi = partition_config.superblock_mi;
    mode_config.disable_trace = !state->trace_enabled;
    status = av1_tile_decode_modes(
        &partition_config, &mode_config, state->frame_cdfs, state->tile_cdfs,
        &partition_trace);
    if (status != AVIFDEC_OK) return status;
    result->partition = partition_trace;
    result->block = state->block_trace;
    result->residual = state->residual;
    result->cdf_checksum = av1_tile_cdfs_checksum(state->tile_cdfs);
    if (tile == frame->context_update_tile_id) {
        avifdec_memory_copy(state->context_update_cdfs, state->tile_cdfs,
                            sizeof(*state->context_update_cdfs));
        state->saved_context_update = 1;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_trace_commit_tile(
    Av1TraceState *state,
    size_t tile,
    const Av1TileDecodeResult *result) {
    AvifdecEntropyTrace *trace;

    if (!state->trace_enabled) return AVIFDEC_OK;
    trace = state->trace;
    if (trace->tile_count == SIZE_MAX ||
        trace->partition_nodes >
            SIZE_MAX - result->partition.partition_nodes ||
        trace->block_count > SIZE_MAX - result->block.block_count ||
        trace->inter_block_count >
            SIZE_MAX - result->block.inter_block_count ||
        trace->compound_block_count >
            SIZE_MAX - result->block.compound_block_count ||
        trace->transform_count >
            SIZE_MAX - result->residual.transform_count ||
        trace->nonzero_transform_count >
            SIZE_MAX - result->residual.nonzero_transform_count ||
        trace->coefficient_count >
            SIZE_MAX - result->residual.coefficient_count) {
        return AVIFDEC_OVERFLOW;
    }
    ++trace->tile_count;
    trace->partition_nodes += result->partition.partition_nodes;
    trace->block_count += result->block.block_count;
    trace->inter_block_count += result->block.inter_block_count;
    trace->compound_block_count += result->block.compound_block_count;
    trace->transform_count += result->residual.transform_count;
    trace->nonzero_transform_count +=
        result->residual.nonzero_transform_count;
    trace->coefficient_count += result->residual.coefficient_count;
    trace->transform_size_mask |= result->residual.transform_size_mask;
    trace->transform_type_mask |= result->residual.transform_type_mask;
    av1_trace_hash(trace, tile);
    av1_trace_hash(trace, result->partition.checksum);
    av1_trace_hash(trace, result->block.checksum);
    av1_trace_hash(trace, result->residual.checksum);
    av1_trace_hash_value(
        &trace->quantized_checksum, result->residual.quantized_checksum);
    av1_trace_hash_value(
        &trace->mode_checksum, result->block.mode_checksum);
    av1_trace_hash_value(
        &trace->inter_mode_checksum, result->block.inter_mode_checksum);
    av1_trace_hash_value(
        &trace->mv_stack_checksum, result->block.mv_stack_checksum);
    av1_trace_hash_value(
        &trace->mv_checksum, result->block.mv_checksum);
    av1_trace_hash_value(
        &trace->predictor_checksum, result->residual.predictor_checksum);
    av1_trace_hash_value(
        &trace->dequantized_checksum,
        result->residual.dequantized_checksum);
    av1_trace_hash_value(
        &trace->residual_checksum, result->residual.residual_checksum);
    av1_trace_hash(trace, result->cdf_checksum);
    return AVIFDEC_OK;
}

static AvifdecStatus av1_trace_finish_tiles(
    Av1TraceState *state,
    const Av1Sequence *sequence,
    const Av1Frame *frame) {
    if (state->trace_enabled) {
        AvifdecStatus status = av1_trace_stage_checksum(
            &state->trace->reconstruction_checksum, &state->frame_planes,
            sequence, frame->frame_width, frame->frame_height,
            state->executor);
        if (status != AVIFDEC_OK) return status;
    }
    return av1_trace_finish_frame(state, sequence, frame);
}

typedef struct {
    Av1TraceState *state;
    const Av1Sequence *sequence;
    const Av1Frame *frame;
    size_t count;
} Av1TileParallelContext;

static void av1_trace_prepare_tile_worker(Av1TraceState *worker,
                                          const Av1TraceState *state) {
    Av1TileCdfs *tile_cdfs = worker->tile_cdfs;
    Av1CoeffContextState coeff_contexts = worker->coeff_contexts;
    int32_t *quantized = worker->quantized;
    int32_t *dequantized = worker->dequantized;
    int32_t *residual_scratch = worker->residual_scratch;
    uint16_t *inter_pred0 = worker->inter_pred0;
    uint16_t *inter_pred1 = worker->inter_pred1;
    uint8_t *inter_mask = worker->inter_mask;
    uint8_t *palette_map = worker->palette_map;
    uint8_t *palette_map_uv = worker->palette_map_uv;

    *worker = *state;
    worker->tile_cdfs = tile_cdfs;
    worker->coeff_contexts = coeff_contexts;
    worker->quantized = quantized;
    worker->dequantized = dequantized;
    worker->residual_scratch = residual_scratch;
    worker->inter_pred0 = inter_pred0;
    worker->inter_pred1 = inter_pred1;
    worker->inter_mask = inter_mask;
    worker->palette_map = palette_map;
    worker->palette_map_uv = palette_map_uv;
    worker->tile_workers = 0;
    worker->tile_work_items = 0;
    worker->tile_results = 0;
    worker->tile_worker_count = 1U;
    worker->tile_capacity = 0U;
}

static AvifdecStatus av1_trace_tile_range(size_t begin,
                                          size_t end,
                                          size_t worker_index,
                                          void *arg) {
    Av1TileParallelContext *parallel = (Av1TileParallelContext *)arg;
    Av1TraceState *worker;
    size_t index;

    if (parallel == 0 || worker_index >= parallel->state->tile_worker_count ||
        begin > end || end > parallel->count) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    worker = worker_index == 0U
        ? parallel->state
        : &parallel->state->tile_workers[worker_index - 1U].state;
    for (index = begin; index < end; ++index) {
        const Av1TileWorkItem *item =
            &parallel->state->tile_work_items[index];
        Av1TileDecodeResult *result =
            &parallel->state->tile_results[index];

        result->status = av1_trace_tile(
            worker, parallel->sequence, parallel->frame,
            item->bits, item->tile, item->position, item->size, result);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_parse_tile_group(Av1Bits *bits,
                                           const Av1Frame *frame,
                                           const Av1Sequence *sequence,
                                           size_t *next_tile,
                                           AvifdecImageInfo *info,
                                           Av1TraceState *trace_state) {
    size_t num_tiles = (size_t)frame->tile_columns * frame->tile_rows;
    size_t tile_start = 0U;
    size_t tile_end = num_tiles - 1U;
    size_t position;
    size_t remaining;
    size_t tile;
    size_t group_count;
    int parallel_tiles;

    if (num_tiles == 0U) return AVIFDEC_INVALID_DATA;
    if (num_tiles > 1U && av1_bits_read(bits, 1U)) {
        unsigned int tile_bits = frame->tile_columns_log2 + frame->tile_rows_log2;

        tile_start = av1_bits_read(bits, tile_bits);
        tile_end = av1_bits_read(bits, tile_bits);
    }
    if (bits->status != AVIFDEC_OK || tile_start > tile_end || tile_end >= num_tiles ||
        tile_start != *next_tile || av1_byte_alignment(bits) != AVIFDEC_OK) {
        return bits->status != AVIFDEC_OK ? bits->status : AVIFDEC_INVALID_DATA;
    }
    position = bits->start + bits->bit_position / 8U;
    remaining = bits->size - bits->bit_position / 8U;
    group_count = tile_end - tile_start + 1U;
    parallel_tiles = trace_state != 0 &&
        trace_state->executor != 0 &&
        trace_state->tile_worker_count > 1U &&
        group_count > 1U &&
        group_count <= trace_state->tile_capacity;
    for (tile = tile_start; tile <= tile_end; ++tile) {
        size_t tile_size;

        if (tile == tile_end) {
            tile_size = remaining;
        } else {
            unsigned int index;
            size_t encoded_size = 0U;

            if (frame->tile_size_bytes == 0U || frame->tile_size_bytes > remaining) {
                return AVIFDEC_TRUNCATED;
            }
            for (index = 0U; index < frame->tile_size_bytes; ++index) {
                uint8_t byte;

                if (!av1_stream_byte_at(bits->stream, position + index, &byte, 0)) {
                    return AVIFDEC_TRUNCATED;
                }
                encoded_size |= (size_t)byte << (index * 8U);
            }
            position += frame->tile_size_bytes;
            remaining -= frame->tile_size_bytes;
            if (encoded_size == SIZE_MAX) return AVIFDEC_OVERFLOW;
            tile_size = encoded_size + 1U;
        }
        if (tile_size == 0U || tile_size > remaining) return AVIFDEC_TRUNCATED;
        if (trace_state != 0) {
            if (parallel_tiles) {
                Av1TileWorkItem *item =
                    &trace_state->tile_work_items[tile - tile_start];

                item->bits = bits;
                item->tile = tile;
                item->position = position;
                item->size = tile_size;
            } else {
                Av1TileDecodeResult result;
                AvifdecStatus status = av1_trace_tile(
                    trace_state, sequence, frame, bits, tile, position,
                    tile_size, &result);
                if (status == AVIFDEC_OK) {
                    status =
                        av1_trace_commit_tile(trace_state, tile, &result);
                }
                if (status != AVIFDEC_OK) return status;
            }
        }
        position += tile_size;
        remaining -= tile_size;
        ++info->tile_count;
        if (!avifdec_size_add(info->tile_data_size, tile_size, &info->tile_data_size)) {
            return AVIFDEC_OVERFLOW;
        }
    }
    if (remaining != 0U) return AVIFDEC_INVALID_DATA;
    if (parallel_tiles) {
        Av1TileParallelContext parallel;
        AvifdecStatus executor_status;
        size_t worker_index;
        size_t index;

        for (worker_index = 1U;
             worker_index < trace_state->tile_worker_count;
             ++worker_index) {
            av1_trace_prepare_tile_worker(
                &trace_state->tile_workers[worker_index - 1U].state,
                trace_state);
        }
        parallel.state = trace_state;
        parallel.sequence = sequence;
        parallel.frame = frame;
        parallel.count = group_count;
        executor_status = trace_state->executor->parallel_for(
            trace_state->executor->user_data, group_count, 1U,
            av1_trace_tile_range, &parallel);
        if (executor_status != AVIFDEC_OK) return executor_status;
        for (index = 0U; index < group_count; ++index) {
            Av1TileDecodeResult *result =
                &trace_state->tile_results[index];
            const Av1TileWorkItem *item =
                &trace_state->tile_work_items[index];
            AvifdecStatus status = result->status;

            if (status == AVIFDEC_OK) {
                status = av1_trace_commit_tile(
                    trace_state, item->tile, result);
            }
            if (status != AVIFDEC_OK) return status;
            if (item->tile == frame->context_update_tile_id) {
                trace_state->saved_context_update = 1U;
            }
        }
    }
    *next_tile = tile_end + 1U;
    if (trace_state != 0 && *next_tile == num_tiles) {
        AvifdecStatus status =
            av1_trace_finish_tiles(trace_state, sequence, frame);
        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_workspace_requirement(
    const AvifdecImageInfo *info,
    uint32_t width,
    uint32_t height,
    size_t worker_count,
    size_t *required) {
    size_t tile_workspace;
    size_t padded_width;
    size_t padded_height;
    size_t plane_samples;
    size_t chroma_samples = 0U;
    size_t plane_workspace;
    size_t result;
    int compact_block_state;

    if (info == 0 || required == 0 || width == 0U || height == 0U ||
        worker_count == 0U ||
        (info->superblock_size != 64U &&
         info->superblock_size != 128U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    compact_block_state =
        info->reduced_still_picture_header && !info->allow_intrabc;
    if (!avifdec_size_align(
            width, info->superblock_size, &padded_width) ||
        !avifdec_size_align(
            height, info->superblock_size, &padded_height) ||
        !avifdec_size_multiply(
            padded_width, padded_height, &plane_samples)) {
        return AVIFDEC_OVERFLOW;
    }
    if (!info->monochrome) {
        size_t chroma_width = padded_width >> info->subsampling_x;
        size_t chroma_height = padded_height >> info->subsampling_y;

        if (!avifdec_size_multiply(
                chroma_width, chroma_height, &chroma_samples) ||
            !avifdec_size_multiply(
                chroma_samples, 2U, &chroma_samples) ||
            !avifdec_size_add(
                plane_samples, chroma_samples, &plane_samples)) {
            return AVIFDEC_OVERFLOW;
        }
    }
    if (!avifdec_size_multiply(
            plane_samples, sizeof(uint16_t), &plane_workspace) ||
        !avifdec_size_multiply(
            plane_workspace,
            info->workspace_plane_buffer_count != 0U
                ? info->workspace_plane_buffer_count
                : (info->reduced_still_picture_header ? 6U : 14U),
            &plane_workspace) ||
        av1_tile_workspace_requirement(
            width, height, info->monochrome,
            info->subsampling_x, info->subsampling_y,
            compact_block_state,
            &tile_workspace) != AVIFDEC_OK) {
        return AVIFDEC_OVERFLOW;
    }
    result = tile_workspace;
    if (!avifdec_size_add(
            result, 6144U * sizeof(int32_t) +
                        3U * AV1_QM_TOTAL_SIZE +
                        _Alignof(int32_t) - 1U,
            &result) ||
        !avifdec_size_add(
            result, plane_workspace + 64U * 64U +
                        _Alignof(uint16_t) - 1U,
            &result)) {
        return AVIFDEC_OVERFLOW;
    }
    if (!compact_block_state &&
        !avifdec_size_add(
            result, 2U * 128U * 128U * sizeof(uint16_t) +
                        128U * 128U + _Alignof(uint16_t) - 1U,
            &result)) {
        return AVIFDEC_OVERFLOW;
    }
    if (!info->reduced_still_picture_header) {
        size_t motion_columns = ((size_t)width + 7U) / 8U;
        size_t motion_rows = ((size_t)height + 7U) / 8U;
        size_t motion_samples;
        size_t motion_workspace;
        size_t temporal_workspace;

        if (!avifdec_size_multiply(
                motion_rows, motion_columns, &motion_samples) ||
            !avifdec_size_multiply(
                motion_samples,
                (AV1_NUM_REF_FRAMES + 1U) * sizeof(Av1SavedMotion),
                &motion_workspace) ||
            !avifdec_size_multiply(
                motion_samples,
                AV1_REFS_PER_FRAME * sizeof(Av1TemporalMotion),
                &temporal_workspace) ||
            !avifdec_size_add(
                motion_workspace, temporal_workspace, &motion_workspace) ||
            !avifdec_size_add(
                result,
                motion_workspace + _Alignof(Av1TemporalMotion) - 1U,
                &result)) {
            return AVIFDEC_OVERFLOW;
        }
    }
    if (info->film_grain_params_present) {
        size_t film_grain_scratch = 0U;

        if (av1_film_grain_scratch_size_ex(
                width, worker_count, &film_grain_scratch) !=
                AVIFDEC_OK ||
            !avifdec_size_add(
                result, film_grain_scratch + _Alignof(int16_t) - 1U,
                &result)) {
            return AVIFDEC_OVERFLOW;
        }
    }
    if (worker_count > 1U && info->tile_count != 0U) {
        AvifdecArena sizing;
        size_t tile_count;
        size_t active_workers;
        size_t worker_index;
        size_t max_mi_columns = 2U * (((size_t)width + 7U) / 8U);
        size_t max_mi_rows = 2U * (((size_t)height + 7U) / 8U);
        size_t superblock_mi =
            info->superblock_size == 128U ? 32U : 16U;
        size_t padded_width4 =
            ((max_mi_columns + superblock_mi - 1U) / superblock_mi) *
            superblock_mi;
        size_t padded_height4 =
            ((max_mi_rows + superblock_mi - 1U) / superblock_mi) *
            superblock_mi;

        tile_count = info->workspace_tile_capacity != 0U
            ? info->workspace_tile_capacity : info->tile_count;
        active_workers = worker_count;
        if (active_workers > 1U) {
            avifdec_arena_init_sizing(&sizing);
            (void)avifdec_arena_allocate(
                &sizing,
                (active_workers - 1U) * sizeof(Av1TileWorker),
                _Alignof(Av1TileWorker));
            (void)avifdec_arena_allocate(
                &sizing, tile_count * sizeof(Av1TileWorkItem),
                _Alignof(Av1TileWorkItem));
            (void)avifdec_arena_allocate(
                &sizing, tile_count * sizeof(Av1TileDecodeResult),
                _Alignof(Av1TileDecodeResult));
            for (worker_index = 1U; worker_index < active_workers;
                 ++worker_index) {
                unsigned int plane;

                (void)avifdec_arena_allocate(
                    &sizing, sizeof(Av1TileCdfs),
                    _Alignof(Av1TileCdfs));
                (void)avifdec_arena_allocate(
                    &sizing, 6144U * sizeof(int32_t),
                    _Alignof(int32_t));
                if (!compact_block_state) {
                    (void)avifdec_arena_allocate(
                        &sizing, 2U * 128U * 128U * sizeof(uint16_t),
                        _Alignof(uint16_t));
                }
                (void)avifdec_arena_allocate(
                    &sizing,
                    (compact_block_state ? 0U : 128U * 128U) +
                        2U * 64U * 64U,
                    _Alignof(uint8_t));
                for (plane = 0U;
                     plane < (info->monochrome ? 1U : 3U);
                     ++plane) {
                    unsigned int sub_x =
                        plane == 0U ? 0U : info->subsampling_x;
                    unsigned int sub_y =
                        plane == 0U ? 0U : info->subsampling_y;
                    size_t width4 =
                        (padded_width4 + ((size_t)1U << sub_x) - 1U) >>
                        sub_x;
                    size_t height4 =
                        (padded_height4 + ((size_t)1U << sub_y) - 1U) >>
                        sub_y;

                    (void)avifdec_arena_allocate(
                        &sizing, 2U * width4 + 2U * height4,
                        _Alignof(uint8_t));
                }
            }
            if (sizing.status != AVIFDEC_OK ||
                !avifdec_size_add(
                    result, avifdec_arena_required(&sizing), &result) ||
                !avifdec_size_add(
                    result, _Alignof(Av1TileWorker) - 1U, &result)) {
                return AVIFDEC_OVERFLOW;
            }
        }
    }
    *required = result;
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_av1_workspace_requirement(
    const AvifdecImageInfo *info,
    size_t *required) {
    if (info == 0) return AVIFDEC_INVALID_ARGUMENT;
    return av1_workspace_requirement(
        info, info->width, info->height, 1U, required);
}

static AvifdecStatus av1_parse_stream(const AvifdecSpan *spans,
                                      size_t span_count,
                                      const AvifdecLimits *limits,
                                      AvifdecImageInfo *info,
                                      AvifdecError *error,
                                      size_t workspace_worker_count,
                                      Av1TraceState *trace_state) {
    Av1Stream stream;
    Av1Sequence sequence;
    Av1ReferenceState references;
    Av1Frame frame;
    Av1Frame selected_frame;
    size_t index;
    size_t max_obus = limits == 0 || limits->max_obus == 0U
                      ? AVIFDEC_DEFAULT_MAX_OBUS : limits->max_obus;
    size_t max_frames = limits == 0 || limits->max_frames == 0U
                        ? AVIFDEC_DEFAULT_MAX_FRAMES : limits->max_frames;
    size_t frame_count = 0U;
    uint8_t framing =
        limits == 0 ? AVIFDEC_AV1_LOW_OVERHEAD : limits->av1_framing;
    int seen_sequence = 0;
    int seen_frame = 0;
    int frame_open = 0;
    size_t next_tile = 0U;
    size_t frame_header_payload_start = 0U;
    size_t frame_header_payload_size = 0U;
    int have_frame_header_copy = 0;
    size_t temporal_unit_end = 0U;
    size_t frame_unit_end = 0U;
    size_t frame_unit_frame_headers = 0U;
    int first_obu_in_temporal_unit = 0;
    int selected_frame_found = 0;
    size_t workspace_tile_capacity = 0U;
    uint8_t selected_spatial_layer =
        limits == 0 ? 0U : limits->spatial_layer;
    int select_spatial_layer =
        limits != 0 && limits->spatial_layer_set;

    if (framing > AVIFDEC_AV1_ANNEX_B) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    info->obu_count = 0U;
    info->metadata_obu_count = 0U;
    info->tile_count = 0U;
    info->workspace_tile_capacity = 0U;
    info->tile_data_size = 0U;
    av1_metadata_reset(info);
    avifdec_memory_fill(&stream, 0U, sizeof(stream));
    avifdec_memory_fill(&sequence, 0U, sizeof(sequence));
    avifdec_memory_fill(&references, 0U, sizeof(references));
    avifdec_memory_fill(&frame, 0U, sizeof(frame));
    avifdec_memory_fill(&selected_frame, 0U, sizeof(selected_frame));
    stream.spans = spans;
    stream.span_count = span_count;
    stream.status = AVIFDEC_OK;
    for (index = 0U; index < span_count; ++index) {
        if (!avifdec_size_add(stream.size, spans[index].size, &stream.size)) {
            return av1_fail(error, AVIFDEC_OVERFLOW, spans[index].file_offset, 0U);
        }
    }
    while (stream.position < stream.size) {
        size_t obu_end = 0U;
        size_t header_offset;
        int first_obu_in_frame_unit = 0;
        uint8_t header;
        uint8_t obu_type;
        uint8_t extension_flag;
        uint8_t has_size_field;
        uint8_t temporal_id = 0U;
        uint8_t spatial_id = 0U;
        size_t payload_size;
        size_t payload_start;
        AvifdecStatus status;
        Av1Bits bits;

        if (framing == AVIFDEC_AV1_ANNEX_B) {
            size_t unit_size;
            size_t prefix_start;

            if (stream.position == temporal_unit_end) {
                if (frame_open || (frame_unit_end != 0U &&
                    stream.position != frame_unit_end)) {
                    return av1_fail(
                        error, AVIFDEC_INVALID_DATA,
                        av1_stream_file_offset(
                            &stream, stream.position),
                        0U);
                }
                prefix_start = stream.position;
                status = av1_leb128(&stream, &unit_size);
                if (status != AVIFDEC_OK || unit_size == 0U ||
                    !avifdec_size_add(
                        stream.position, unit_size,
                        &temporal_unit_end) ||
                    temporal_unit_end > stream.size) {
                    return av1_fail(
                        error,
                        status != AVIFDEC_OK ? status :
                            AVIFDEC_INVALID_DATA,
                        av1_stream_file_offset(
                            &stream, prefix_start),
                        0U);
                }
                frame_unit_end = stream.position;
                first_obu_in_temporal_unit = 1;
            }
            if (stream.position == frame_unit_end) {
                if (frame_open) {
                    return av1_fail(
                        error, AVIFDEC_INVALID_DATA,
                        av1_stream_file_offset(
                            &stream, stream.position),
                        AV1_OBU_TILE_GROUP);
                }
                prefix_start = stream.position;
                status = av1_leb128(&stream, &unit_size);
                if (status != AVIFDEC_OK || unit_size == 0U ||
                    !avifdec_size_add(
                        stream.position, unit_size, &frame_unit_end) ||
                    frame_unit_end > temporal_unit_end) {
                    return av1_fail(
                        error,
                        status != AVIFDEC_OK ? status :
                            AVIFDEC_INVALID_DATA,
                        av1_stream_file_offset(
                            &stream, prefix_start),
                        0U);
                }
                frame_unit_frame_headers = 0U;
                first_obu_in_frame_unit = 1;
            }
            prefix_start = stream.position;
            status = av1_leb128(&stream, &unit_size);
            if (status != AVIFDEC_OK || unit_size == 0U ||
                !avifdec_size_add(
                    stream.position, unit_size, &obu_end) ||
                obu_end > frame_unit_end) {
                return av1_fail(
                    error,
                    status != AVIFDEC_OK ? status :
                        AVIFDEC_INVALID_DATA,
                    av1_stream_file_offset(&stream, prefix_start),
                    0U);
            }
        }
        header_offset = av1_stream_file_offset(
            &stream, stream.position);
        header = av1_stream_read(&stream);
        obu_type = (header >> 3) & 15U;
        extension_flag = (header >> 2) & 1U;
        has_size_field = (header >> 1) & 1U;

        if (info->obu_count >= max_obus) {
            return av1_fail(error, AVIFDEC_LIMIT_EXCEEDED, header_offset, obu_type);
        }
        ++info->obu_count;
        if ((header & 0x81U) != 0U ||
            (framing == AVIFDEC_AV1_LOW_OVERHEAD &&
             !has_size_field)) {
            return av1_fail(error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
        }
        if (extension_flag) {
            uint8_t extension = av1_stream_read(&stream);
            temporal_id = extension >> 5;
            spatial_id = (extension >> 3) & 3U;
            if ((extension & 7U) != 0U) {
                return av1_fail(error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            }
        }
        if (has_size_field) {
            status = av1_leb128(&stream, &payload_size);
            if (status != AVIFDEC_OK) {
                return av1_fail(
                    error, status, header_offset, obu_type);
            }
        } else {
            payload_size = obu_end - stream.position;
        }
        payload_start = stream.position;
        if (payload_size > stream.size - stream.position) {
            return av1_fail(error, AVIFDEC_TRUNCATED, header_offset, obu_type);
        }
        if (framing == AVIFDEC_AV1_ANNEX_B &&
            (payload_start > obu_end ||
             payload_size != obu_end - payload_start)) {
            return av1_fail(
                error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
        }
        if (framing == AVIFDEC_AV1_ANNEX_B) {
            if (first_obu_in_temporal_unit) {
                if (!first_obu_in_frame_unit ||
                    obu_type != AV1_OBU_TEMPORAL_DELIMITER) {
                    return av1_fail(
                        error, AVIFDEC_INVALID_DATA,
                        header_offset, obu_type);
                }
                first_obu_in_temporal_unit = 0;
            } else if (obu_type == AV1_OBU_TEMPORAL_DELIMITER) {
                return av1_fail(
                    error, AVIFDEC_INVALID_DATA,
                    header_offset, obu_type);
            }
        }
        av1_bits_init(&bits, &stream, payload_start, payload_size);
        if (extension_flag && seen_sequence &&
            obu_type != AV1_OBU_SEQUENCE_HEADER &&
            !av1_layer_in_operating_point(
                &sequence, temporal_id, spatial_id)) {
            stream.position = payload_start + payload_size;
            continue;
        }
        if (framing == AVIFDEC_AV1_ANNEX_B &&
            (obu_type == AV1_OBU_FRAME_HEADER ||
             obu_type == AV1_OBU_FRAME) &&
            ++frame_unit_frame_headers > 1U) {
            return av1_fail(
                error, AVIFDEC_INVALID_DATA,
                header_offset, obu_type);
        }
        if (obu_type == AV1_OBU_SEQUENCE_HEADER) {
            if (seen_sequence || seen_frame) {
                return av1_fail(error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            }
            if (extension_flag && (temporal_id != 0U || spatial_id != 0U)) {
                return av1_fail(
                    error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            }
            status = av1_parse_sequence_header(
                &bits, limits == 0 ? 0U : limits->operating_point,
                &sequence);
            if (status != AVIFDEC_OK) return av1_fail(error, status, av1_bits_offset(&bits), obu_type);
            seen_sequence = 1;
        } else if (obu_type == AV1_OBU_FRAME_HEADER || obu_type == AV1_OBU_FRAME) {
            if (!seen_sequence || frame_open) {
                return av1_fail(error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            }
            if (sequence.still_picture && frame_count != 0U) {
                return av1_fail(
                    error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            }
            if (frame_count >= max_frames) {
                return av1_fail(
                    error, AVIFDEC_LIMIT_EXCEEDED, header_offset, obu_type);
            }
            status = av1_parse_frame_header(
                &bits, &sequence, temporal_id, spatial_id, &references,
                &frame);
            if (status != AVIFDEC_OK) return av1_fail(error, status, av1_bits_offset(&bits), obu_type);
            if (!frame.show_existing_frame) {
                size_t frame_tile_count;

                if (!avifdec_size_multiply(
                        frame.tile_columns, frame.tile_rows,
                        &frame_tile_count)) {
                    return av1_fail(
                        error, AVIFDEC_OVERFLOW,
                        av1_bits_offset(&bits), obu_type);
                }
                if (frame_tile_count > workspace_tile_capacity) {
                    workspace_tile_capacity = frame_tile_count;
                }
            }
            frame.spatial_id = spatial_id;
            if (frame.show_frame &&
                (!select_spatial_layer ||
                 spatial_id == selected_spatial_layer)) {
                selected_frame = frame;
                selected_frame_found = 1;
            }
            if (obu_type == AV1_OBU_FRAME_HEADER) {
                status = av1_trailing_bits(&bits);
                if (status != AVIFDEC_OK) {
                    return av1_fail(
                        error, status, av1_bits_offset(&bits),
                        obu_type);
                }
            }
            ++frame_count;
            seen_frame = 1;
            next_tile = 0U;
            if (obu_type == AV1_OBU_FRAME_HEADER &&
                !frame.show_existing_frame) {
                frame_header_payload_start = payload_start;
                frame_header_payload_size = payload_size;
                have_frame_header_copy = 1;
            } else {
                have_frame_header_copy = 0;
            }
            if (frame.show_existing_frame) {
                if (trace_state != 0 && trace_state->image != 0) {
                    const Av1ReferenceSlot *shown =
                        &references.slots[frame.frame_to_show_map_idx];

                    if (!trace_state->initialized ||
                        !shown->pixels_valid) {
                        return av1_fail(
                            error, AVIFDEC_UNSUPPORTED,
                            av1_bits_offset(&bits), obu_type);
                    }
                    if (!trace_state->output_spatial_layer_set ||
                        frame.spatial_id ==
                            trace_state->output_spatial_layer) {
                        status = av1_copy_image(
                            trace_state->image,
                            &trace_state->reference_planes[
                                frame.frame_to_show_map_idx],
                            &sequence, frame.upscaled_width,
                            frame.frame_height, trace_state->output_width,
                            trace_state->output_height,
                            trace_state->executor);
                        if (status != AVIFDEC_OK) {
                            return av1_fail(
                                error, status, av1_bits_offset(&bits),
                                obu_type);
                        }
                        trace_state->output_frame_seen = 1U;
                        if (frame.film_grain.apply_grain) {
                            status = av1_apply_film_grain_output(
                                trace_state, &sequence,
                                &frame.film_grain);
                            if (status != AVIFDEC_OK) {
                                return av1_fail(
                                    error, status,
                                    av1_bits_offset(&bits),
                                    obu_type);
                            }
                        }
                    }
                }
                status = av1_reference_commit(
                    &references, &sequence, &frame, trace_state);
                if (status != AVIFDEC_OK) {
                    return av1_fail(
                        error, status, av1_bits_offset(&bits), obu_type);
                }
            } else {
                if (trace_state != 0) {
                    status = av1_trace_begin_frame(
                        trace_state, &sequence, &references, &frame);
                    if (status != AVIFDEC_OK) {
                        return av1_fail(error, status,
                                        av1_bits_offset(&bits), obu_type);
                    }
                }
                frame_open = 1;
            }
            if (obu_type == AV1_OBU_FRAME && frame_open) {
                status = av1_byte_alignment(&bits);
                if (status == AVIFDEC_OK) {
                    Av1Bits tile_bits;
                    size_t header_bytes = bits.bit_position / 8U;

                    av1_bits_init(&tile_bits, &stream, payload_start + header_bytes,
                                  payload_size - header_bytes);
                    status = av1_parse_tile_group(&tile_bits, &frame, &sequence,
                                                  &next_tile, info, trace_state);
                }
                if (status != AVIFDEC_OK) {
                    return av1_fail(error, status, av1_bits_offset(&bits), obu_type);
                }
                if (next_tile == (size_t)frame.tile_columns * frame.tile_rows) {
                    if (trace_state != 0) {
                        status = av1_trace_finish_cdfs(trace_state, &frame);
                        if (status != AVIFDEC_OK) {
                            return av1_fail(error, status,
                                            av1_bits_offset(&bits), obu_type);
                        }
                    }
                    status = av1_reference_commit(
                        &references, &sequence, &frame, trace_state);
                    if (status != AVIFDEC_OK) {
                        return av1_fail(
                            error, status, av1_bits_offset(&bits),
                            obu_type);
                    }
                    frame_open = 0;
                    have_frame_header_copy = 0;
                }
            }
        } else if (obu_type == AV1_OBU_METADATA) {
            Av1MetadataConfig metadata_config;

            metadata_config.max_width = sequence.max_width;
            metadata_config.max_height = sequence.max_height;
            metadata_config.num_units_in_display_tick =
                sequence.num_units_in_display_tick;
            metadata_config.time_scale = sequence.time_scale;
            metadata_config.num_ticks_per_picture_minus_1 =
                sequence.num_ticks_per_picture_minus_1;
            metadata_config.timing_info_present =
                sequence.timing_info_present;
            metadata_config.equal_picture_interval =
                sequence.equal_picture_interval;
            status = av1_metadata_parse(
                &bits, &metadata_config, extension_flag, info);
            if (status != AVIFDEC_OK) return av1_fail(error, status, header_offset, obu_type);
            ++info->metadata_obu_count;
        } else if (obu_type == AV1_OBU_REDUNDANT_FRAME_HEADER) {
            if (!frame_open || !have_frame_header_copy ||
                payload_size != frame_header_payload_size ||
                !av1_stream_range_equal(
                    &stream, frame_header_payload_start,
                    payload_start, payload_size)) {
                return av1_fail(
                    error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            }
        } else if (obu_type == AV1_OBU_TILE_GROUP) {
            if (!frame_open) return av1_fail(error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            status = av1_parse_tile_group(&bits, &frame, &sequence, &next_tile,
                                          info, trace_state);
            if (status != AVIFDEC_OK) return av1_fail(error, status, av1_bits_offset(&bits), obu_type);
            if (next_tile == (size_t)frame.tile_columns * frame.tile_rows) {
                if (trace_state != 0) {
                    status = av1_trace_finish_cdfs(trace_state, &frame);
                    if (status != AVIFDEC_OK) {
                        return av1_fail(error, status,
                                        av1_bits_offset(&bits), obu_type);
                    }
                }
                status = av1_reference_commit(
                    &references, &sequence, &frame, trace_state);
                if (status != AVIFDEC_OK) {
                    return av1_fail(
                        error, status, av1_bits_offset(&bits), obu_type);
                }
                frame_open = 0;
                have_frame_header_copy = 0;
            }
        } else if (obu_type == AV1_OBU_TEMPORAL_DELIMITER) {
            if (frame_open || payload_size != 0U) {
                return av1_fail(
                    error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
            }
        } else if (obu_type == AV1_OBU_TILE_LIST) {
            return av1_fail(error, AVIFDEC_UNSUPPORTED, header_offset, obu_type);
        } else if (obu_type == AV1_OBU_PADDING) {
            size_t padding_index;

            for (padding_index = 0U; padding_index < payload_size;
                 ++padding_index) {
                uint8_t padding_byte;

                if (!av1_stream_byte_at(
                        &stream, payload_start + padding_index,
                        &padding_byte, 0)) {
                    return av1_fail(
                        error, AVIFDEC_TRUNCATED, header_offset, obu_type);
                }
                if (padding_byte != 0U) {
                    return av1_fail(
                        error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
                }
            }
        } else if (obu_type != 0U && obu_type > AV1_OBU_TILE_LIST) {
            /* Reserved OBUs are skipped by their declared size. */
        }
        stream.position = payload_start + payload_size;
    }
    if (!seen_sequence || !seen_frame) {
        return av1_fail(error, AVIFDEC_INVALID_DATA, av1_stream_file_offset(&stream, stream.size), 0U);
    }
    if (frame_open || (!frame.show_existing_frame &&
        next_tile != (size_t)frame.tile_columns * frame.tile_rows)) {
        return av1_fail(error, AVIFDEC_TRUNCATED, av1_stream_file_offset(&stream, stream.size),
                        AV1_OBU_TILE_GROUP);
    }
    if (!selected_frame_found ||
        (trace_state != 0 && trace_state->image != 0 &&
         !trace_state->output_frame_seen)) {
        return av1_fail(
            error, AVIFDEC_INVALID_DATA,
            av1_stream_file_offset(&stream, stream.size), 0U);
    }
    if (selected_frame_found) {
        frame = selected_frame;
    }
    if (info->width == 0U && info->height == 0U) {
        info->profile = sequence.profile;
        info->level = sequence.level;
        info->tier = sequence.tier;
        info->bit_depth = sequence.bit_depth;
        info->monochrome = sequence.monochrome;
        info->subsampling_x = sequence.subsampling_x;
        info->subsampling_y = sequence.subsampling_y;
        info->chroma_sample_position = sequence.chroma_sample_position;
        info->width = sequence.max_width;
        info->height = sequence.max_height;
        info->channel_count = sequence.monochrome ? 1U : 3U;
    } else if (sequence.profile != info->profile ||
               sequence.operating_point_level[0] != info->level ||
               sequence.operating_point_tier[0] != info->tier ||
               sequence.bit_depth != info->bit_depth ||
               sequence.monochrome != info->monochrome ||
               sequence.subsampling_x != info->subsampling_x ||
               sequence.subsampling_y != info->subsampling_y ||
               sequence.chroma_sample_position !=
                   info->chroma_sample_position ||
               (!select_spatial_layer &&
                sequence.operating_point_idc[
                    sequence.selected_operating_point] == 0U &&
                (sequence.max_width != info->width ||
                 sequence.max_height != info->height)) ||
               (select_spatial_layer
                    ? frame.upscaled_width < info->width
                    : frame.upscaled_width != info->width) ||
               (select_spatial_layer
                    ? frame.frame_height < info->height
                    : frame.frame_height != info->height)) {
        return av1_fail(
            error, AVIFDEC_INVALID_DATA,
            av1_stream_file_offset(&stream, 0U),
            AV1_OBU_SEQUENCE_HEADER);
    }
    info->level = sequence.level;
    info->tier = sequence.tier;
    info->operating_point = sequence.selected_operating_point;
    info->operating_point_count = sequence.operating_points_count;
    info->operating_point_idc = sequence.operating_point_idc[
        sequence.selected_operating_point];
    info->timing_info_present = sequence.timing_info_present;
    info->equal_picture_interval = sequence.equal_picture_interval;
    info->decoder_model_info_present =
        sequence.decoder_model_info_present;
    info->buffer_delay_length = sequence.buffer_delay_length;
    info->buffer_removal_time_length =
        sequence.buffer_removal_time_length;
    info->frame_presentation_time_length =
        sequence.frame_presentation_time_length;
    info->operating_point_decoder_model_present =
        sequence.decoder_model_present[
            sequence.selected_operating_point];
    info->decoder_buffer_delay = sequence.decoder_buffer_delay[
        sequence.selected_operating_point];
    info->encoder_buffer_delay = sequence.encoder_buffer_delay[
        sequence.selected_operating_point];
    info->low_delay_mode = sequence.low_delay_mode[
        sequence.selected_operating_point];
    info->initial_display_delay_present =
        sequence.initial_display_delay_present[
            sequence.selected_operating_point];
    info->initial_display_delay_minus_1 =
        sequence.initial_display_delay_minus_1[
            sequence.selected_operating_point];
    info->buffer_removal_time_present =
        frame.buffer_removal_time_present;
    info->buffer_removal_time = frame.buffer_removal_time;
    info->frame_presentation_time_present =
        frame.frame_presentation_time_present;
    info->frame_presentation_time =
        frame.frame_presentation_time;
    info->num_units_in_display_tick =
        sequence.num_units_in_display_tick;
    info->time_scale = sequence.time_scale;
    info->num_ticks_per_picture_minus_1 =
        sequence.num_ticks_per_picture_minus_1;
    info->num_units_in_decoding_tick =
        sequence.num_units_in_decoding_tick;
    info->render_width = frame.render_width;
    info->render_height = frame.render_height;
    info->reduced_still_picture_header = sequence.reduced_still_picture_header;
    info->workspace_plane_buffer_count =
        sequence.reduced_still_picture_header ? 6U : 14U;
    if (sequence.reduced_still_picture_header) {
        if (av1_cdef_is_identity(&sequence, &frame)) {
            --info->workspace_plane_buffer_count;
        }
        if (frame.upscaled_width == frame.frame_width) {
            info->workspace_plane_buffer_count -= 2U;
        }
        if (av1_restoration_is_identity(&sequence, &frame)) {
            --info->workspace_plane_buffer_count;
        }
    }
    info->film_grain_params_present =
        sequence.film_grain_params_present;
    info->film_grain_applied = frame.film_grain.apply_grain;
    info->film_grain_update = frame.film_grain.update_grain;
    info->film_grain_overlap = frame.film_grain.overlap_flag;
    info->film_grain_clip_restricted =
        frame.film_grain.clip_to_restricted_range;
    info->film_grain_seed = frame.film_grain.grain_seed;
    info->frame_type = frame.frame_type;
    info->base_q_index = frame.base_q_index;
    info->coded_lossless = frame.coded_lossless;
    info->allow_screen_content_tools = frame.allow_screen_content_tools;
    info->allow_intrabc = frame.allow_intrabc;
    info->enable_filter_intra = sequence.enable_filter_intra;
    info->enable_intra_edge_filter = sequence.enable_intra_edge_filter;
    info->segmentation_enabled = frame.segmentation_enabled;
    info->delta_q_present = frame.delta_q_present;
    info->delta_lf_present = frame.delta_lf_present;
    info->tx_mode = frame.tx_mode;
    info->reduced_tx_set = frame.reduced_tx_set;
    info->superblock_size = sequence.use_128x128_superblock ? 128U : 64U;
    info->tile_columns = frame.tile_columns;
    info->tile_rows = frame.tile_rows;
    info->workspace_tile_capacity = workspace_tile_capacity;
    if (info->workspace_required == 0U) {
        AvifdecStatus requirement_status =
            av1_workspace_requirement(
                info, sequence.max_width, sequence.max_height,
                workspace_worker_count,
                &info->workspace_required);
        if (requirement_status != AVIFDEC_OK) {
            return av1_fail(
                error, requirement_status,
                av1_stream_file_offset(&stream, 0U),
                AV1_OBU_SEQUENCE_HEADER);
        }
    }
    if (trace_state != 0) {
        if ((!frame.show_existing_frame && !trace_state->initialized) ||
            (!frame.show_existing_frame && !trace_state->saved_context_update)) {
            return av1_fail(error, AVIFDEC_INVALID_DATA,
                            av1_stream_file_offset(&stream, stream.size),
                            AV1_OBU_TILE_GROUP);
        }
        if (!frame.disable_frame_end_update_cdf) {
            avifdec_memory_copy(trace_state->frame_cdfs,
                                trace_state->context_update_cdfs,
                                sizeof(*trace_state->frame_cdfs));
        }
    }
    if (!info->has_nclx) {
        info->color_range = sequence.color_range;
        info->color_primaries = sequence.color_primaries;
        info->transfer_characteristics = sequence.transfer_characteristics;
        info->matrix_coefficients = sequence.matrix_coefficients;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_av1_query_ex(const AvifdecSpan *spans,
                                   size_t span_count,
                                   const AvifdecLimits *limits,
                                   size_t worker_count,
                                   AvifdecImageInfo *info,
                                   AvifdecError *error) {
    return av1_parse_stream(
        spans, span_count, limits, info, error, worker_count, 0);
}

AvifdecStatus avifdec_av1_query(const AvifdecSpan *spans,
                                size_t span_count,
                                const AvifdecLimits *limits,
                                AvifdecImageInfo *info,
                                AvifdecError *error) {
    return avifdec_av1_query_ex(
        spans, span_count, limits, 1U, info, error);
}

AvifdecStatus avifdec_av1_trace_ex(
    const AvifdecSpan *spans,
    size_t span_count,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    AvifdecImageInfo *info,
    void *workspace,
    size_t workspace_size,
    AvifdecEntropyTrace *trace,
    AvifdecError *error) {
    Av1TraceState state;

    if (spans == 0 || span_count == 0U || info == 0 || trace == 0 ||
        (executor != 0 &&
         (executor->parallel_for == 0 ||
          executor->worker_count == 0U ||
          executor->worker_count > AVIFDEC_EXECUTOR_MAX_WORKERS)) ||
        (workspace == 0 && workspace_size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    avifdec_memory_fill(trace, 0U, sizeof(*trace));
    trace->checksum = (uint64_t)1469598103934665603ULL;
    trace->reference_state_checksum = (uint64_t)1469598103934665603ULL;
    trace->mode_checksum = (uint64_t)1469598103934665603ULL;
    trace->inter_mode_checksum = (uint64_t)1469598103934665603ULL;
    trace->mv_stack_checksum = (uint64_t)1469598103934665603ULL;
    trace->mv_checksum = (uint64_t)1469598103934665603ULL;
    trace->predictor_checksum = (uint64_t)1469598103934665603ULL;
    trace->quantized_checksum = (uint64_t)1469598103934665603ULL;
    trace->dequantized_checksum = (uint64_t)1469598103934665603ULL;
    trace->residual_checksum = (uint64_t)1469598103934665603ULL;
    trace->reconstruction_checksum = (uint64_t)1469598103934665603ULL;
    trace->deblocked_checksum = (uint64_t)1469598103934665603ULL;
    trace->cdef_checksum = (uint64_t)1469598103934665603ULL;
    trace->superres_checksum = (uint64_t)1469598103934665603ULL;
    trace->restoration_checksum = (uint64_t)1469598103934665603ULL;
    avifdec_memory_fill(&state, 0U, sizeof(state));
    avifdec_arena_init(&state.arena, workspace, workspace_size);
    state.trace = trace;
    state.trace_enabled = 1U;
    state.executor = executor;
    state.planned_tile_capacity =
        info->workspace_tile_capacity != 0U
            ? info->workspace_tile_capacity : info->tile_count;
    state.output_spatial_layer =
        limits == 0 ? 0U : limits->spatial_layer;
    state.output_spatial_layer_set =
        limits == 0 ? 0U : limits->spatial_layer_set;
    return av1_parse_stream(
        spans, span_count, limits, info, error,
        executor == 0 ? 1U : executor->worker_count, &state);
}

AvifdecStatus avifdec_av1_trace(const AvifdecSpan *spans,
                                size_t span_count,
                                const AvifdecLimits *limits,
                                AvifdecImageInfo *info,
                                void *workspace,
                                size_t workspace_size,
                                AvifdecEntropyTrace *trace,
                                AvifdecError *error) {
    return avifdec_av1_trace_ex(
        spans, span_count, limits, 0, info, workspace,
        workspace_size, trace, error);
}

AvifdecStatus avifdec_av1_decode_ex(
    const AvifdecSpan *spans,
    size_t span_count,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    AvifdecImageInfo *info,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecError *error) {
    Av1TraceState state;
    AvifdecEntropyTrace local_trace;
    uint8_t trace_enabled = trace != 0;

    if (spans == 0 || span_count == 0U || info == 0 || image == 0 ||
        (executor != 0 &&
         (executor->worker_count == 0U ||
          executor->worker_count > AVIFDEC_EXECUTOR_MAX_WORKERS ||
          executor->parallel_for == 0)) ||
        (workspace == 0 && workspace_size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (trace == 0) trace = &local_trace;
    avifdec_memory_fill(trace, 0U, sizeof(*trace));
    trace->checksum = (uint64_t)1469598103934665603ULL;
    trace->reference_state_checksum = (uint64_t)1469598103934665603ULL;
    trace->mode_checksum = (uint64_t)1469598103934665603ULL;
    trace->inter_mode_checksum = (uint64_t)1469598103934665603ULL;
    trace->mv_stack_checksum = (uint64_t)1469598103934665603ULL;
    trace->mv_checksum = (uint64_t)1469598103934665603ULL;
    trace->predictor_checksum = (uint64_t)1469598103934665603ULL;
    trace->quantized_checksum = (uint64_t)1469598103934665603ULL;
    trace->dequantized_checksum = (uint64_t)1469598103934665603ULL;
    trace->residual_checksum = (uint64_t)1469598103934665603ULL;
    trace->reconstruction_checksum = (uint64_t)1469598103934665603ULL;
    trace->deblocked_checksum = (uint64_t)1469598103934665603ULL;
    trace->cdef_checksum = (uint64_t)1469598103934665603ULL;
    trace->superres_checksum = (uint64_t)1469598103934665603ULL;
    trace->restoration_checksum = (uint64_t)1469598103934665603ULL;
    avifdec_memory_fill(&state, 0U, sizeof(state));
    avifdec_arena_init(&state.arena, workspace, workspace_size);
    state.trace = trace;
    state.trace_enabled = trace_enabled;
    state.executor = executor;
    state.planned_tile_capacity = info->workspace_tile_capacity != 0U
        ? info->workspace_tile_capacity : info->tile_count;
    state.image = image;
    state.output_width = info->width;
    state.output_height = info->height;
    state.output_spatial_layer =
        limits == 0 ? 0U : limits->spatial_layer;
    state.output_spatial_layer_set =
        limits == 0 ? 0U : limits->spatial_layer_set;
    return av1_parse_stream(
        spans, span_count, limits, info, error,
        executor == 0 ? 1U : executor->worker_count, &state);
}

AvifdecStatus avifdec_av1_decode(const AvifdecSpan *spans,
                                 size_t span_count,
                                 const AvifdecLimits *limits,
                                 AvifdecImageInfo *info,
                                 void *workspace,
                                 size_t workspace_size,
                                 AvifdecImage *image,
                                 AvifdecEntropyTrace *trace,
                                 AvifdecError *error) {
    return avifdec_av1_decode_ex(
        spans, span_count, limits, 0, info,
        workspace, workspace_size, image, trace, error);
}