#include "avif_sequence_decode.h"

#include "base.h"

#define SEQ_DECODE_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | \
     ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8) | \
     (uint32_t)(unsigned char)(d))

static void seq_decode_error_clear(AvifdecError *error) {
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
}

static AvifdecStatus seq_decode_fail(
    AvifdecStatus status,
    size_t offset,
    uint32_t context,
    AvifdecError *error) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = context;
    }
    return status;
}

static int seq_decode_executor_valid(
    const AvifdecExecutor *executor) {
    return executor == 0 ||
           (executor->parallel_for != 0 &&
            executor->worker_count != 0U &&
            executor->worker_count <=
                AVIFDEC_EXECUTOR_MAX_WORKERS);
}

static AvifdecStatus seq_replay_build(
    const AvifSequenceIndex *index,
    uint32_t track_id,
    size_t sample_index,
    int alpha,
    AvifSequenceReplay *replay,
    AvifdecError *error) {
    AvifSequenceSampleInfo sample;
    AvifSequenceSampleInfo sync_sample;
    size_t sample_span_count;
    AvifdecStatus status = avif_sequence_sample_query(
        index, track_id, sample_index, &sample, error);

    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_sample_query(
        index, track_id, sample.sync_sample_index,
        &sync_sample, error);
    if (status != AVIFDEC_OK) return status;
    if (sync_sample.is_sync == 0U ||
        sync_sample.sample_index > sample.sample_index) {
        return seq_decode_fail(
            AVIFDEC_INVALID_DATA, sample_index,
            SEQ_DECODE_FOURCC('s', 't', 's', 's'), error);
    }
    sample_span_count =
        sample.sample_index - sync_sample.sample_index + 1U;
    replay->index = index;
    replay->track_id = track_id;
    replay->first_sample_index = sync_sample.sample_index;
    replay->last_sample_index = sample.sample_index;
    replay->prepend_config = sync_sample.prepend_config;
    replay->alpha = (uint8_t)(alpha != 0);
    if (!avifdec_size_add(
            sample_span_count,
            replay->prepend_config != 0U ? 1U : 0U,
            &replay->span_count)) {
        return seq_decode_fail(
            AVIFDEC_OVERFLOW, sample_index,
            SEQ_DECODE_FOURCC('s', 't', 's', 'z'), error);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_replay_validate(
    const AvifSequenceReplay *replay,
    AvifSequenceSampleInfo *first,
    AvifdecError *error) {
    AvifSequenceSampleInfo last;
    size_t expected_count;
    AvifdecStatus status;

    if (replay == 0 || replay->index == 0 ||
        replay->track_id == 0U ||
        replay->first_sample_index >
            replay->last_sample_index) {
        return seq_decode_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = avif_sequence_sample_query(
        replay->index, replay->track_id,
        replay->first_sample_index, first, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_sample_query(
        replay->index, replay->track_id,
        replay->last_sample_index, &last, error);
    if (status != AVIFDEC_OK) return status;
    if (first->is_sync == 0U ||
        last.sync_sample_index != first->sample_index ||
        replay->prepend_config != first->prepend_config ||
        (replay->prepend_config != 0U &&
         first->config_size == 0U)) {
        return seq_decode_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U,
            SEQ_DECODE_FOURCC('s', 't', 's', 's'), error);
    }
    expected_count =
        replay->last_sample_index -
        replay->first_sample_index + 1U;
    if (!avifdec_size_add(
            expected_count,
            replay->prepend_config != 0U ? 1U : 0U,
            &expected_count) ||
        expected_count != replay->span_count) {
        return seq_decode_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U,
            SEQ_DECODE_FOURCC('s', 't', 's', 'z'), error);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_replay_span_query(
    const AvifSequenceReplay *replay,
    size_t span_index,
    AvifdecSpan *span,
    size_t *sample_index,
    AvifdecError *error) {
    AvifSequenceSampleInfo first;
    AvifSequenceSampleInfo sample;
    const unsigned char *data;
    size_t data_size;
    AvifdecLimits limits;
    size_t ordinal;
    AvifdecSpan result;
    size_t result_sample_index;
    AvifdecStatus status;

    seq_decode_error_clear(error);
    if (span == 0 || sample_index == 0) {
        return seq_decode_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_replay_validate(replay, &first, error);
    if (status != AVIFDEC_OK) return status;
    if (span_index >= replay->span_count) {
        return seq_decode_fail(
            AVIFDEC_INVALID_ARGUMENT, span_index, 0U, error);
    }
    status = avif_sequence_index_source(
        replay->index, &data, &data_size, &limits, error);
    (void)limits;
    if (status != AVIFDEC_OK) return status;
    if (replay->prepend_config != 0U && span_index == 0U) {
        if (first.config_offset > data_size ||
            first.config_size >
                data_size - first.config_offset) {
            return seq_decode_fail(
                AVIFDEC_INVALID_DATA, first.config_offset,
                SEQ_DECODE_FOURCC('a', 'v', '1', 'C'), error);
        }
        result.data = data + first.config_offset;
        result.size = first.config_size;
        result.file_offset = first.config_offset;
        result_sample_index = SIZE_MAX;
    } else {
        ordinal = span_index -
            (replay->prepend_config != 0U ? 1U : 0U);
        result_sample_index =
            replay->first_sample_index + ordinal;
        status = avif_sequence_sample_query(
            replay->index, replay->track_id,
            result_sample_index, &sample, error);
        if (status != AVIFDEC_OK) return status;
        if (sample.offset > (uint64_t)SIZE_MAX ||
            (size_t)sample.offset > data_size ||
            sample.size > data_size - (size_t)sample.offset) {
            return seq_decode_fail(
                AVIFDEC_INVALID_DATA,
                sample.offset > (uint64_t)SIZE_MAX
                    ? data_size : (size_t)sample.offset,
                SEQ_DECODE_FOURCC('m', 'd', 'a', 't'), error);
        }
        result.data = data + (size_t)sample.offset;
        result.size = sample.size;
        result.file_offset = (size_t)sample.offset;
    }
    *span = result;
    *sample_index = result_sample_index;
    return AVIFDEC_OK;
}

static int seq_query_matches(
    const AvifdecImageInfo *expected,
    const AvifdecImageInfo *queried,
    int alpha) {
    if (queried->width != expected->width ||
        queried->height != expected->height ||
        queried->bit_depth == 0U) {
        return 0;
    }
    if (alpha != 0) {
        return queried->monochrome != 0U &&
               queried->color_range == 1U;
    }
    return queried->bit_depth == expected->bit_depth &&
           queried->monochrome == expected->monochrome;
}

static void seq_merge_main_info(
    AvifdecImageInfo *destination,
    const AvifdecImageInfo *queried,
    const AvifdecImageInfo *container) {
    *destination = *queried;
    destination->primary_item_id = container->primary_item_id;
    destination->primary_item_type = container->primary_item_type;
    destination->width = container->width;
    destination->height = container->height;
    destination->presentation_width =
        container->presentation_width;
    destination->presentation_height =
        container->presentation_height;
    destination->render_width = container->render_width;
    destination->render_height = container->render_height;
    destination->color_primaries = container->color_primaries;
    destination->transfer_characteristics =
        container->transfer_characteristics;
    destination->matrix_coefficients =
        container->matrix_coefficients;
    destination->color_range = container->color_range;
    destination->has_nclx = container->has_nclx;
    destination->icc_data = container->icc_data;
    destination->icc_size = container->icc_size;
    destination->transform_flags = container->transform_flags;
    destination->irot_angle = container->irot_angle;
    destination->imir_axis = container->imir_axis;
    destination->pixel_aspect_h_spacing =
        container->pixel_aspect_h_spacing;
    destination->pixel_aspect_v_spacing =
        container->pixel_aspect_v_spacing;
    destination->clean_aperture = container->clean_aperture;
    destination->crop = container->crop;
    destination->has_alpha = container->has_alpha;
    destination->alpha_premultiplied =
        container->alpha_premultiplied;
    destination->alpha_bit_depth = container->alpha_bit_depth;
    destination->alpha_color_range =
        container->alpha_color_range;
    destination->alpha_item_id = container->alpha_item_id;
}

AvifdecStatus avif_sequence_decode_plan_query(
    const AvifSequenceIndex *index,
    const AvifSequenceSelection *selection,
    const AvifdecExecutor *executor,
    size_t presentation_index,
    const AvifSequenceDecodeCallbacks *callbacks,
    AvifSequenceDecodePlan *plan,
    AvifdecError *error) {
    AvifSequenceDecodePlan result;
    AvifdecImageInfo main_query;
    AvifdecImageInfo alpha_query;
    const unsigned char *data;
    size_t data_size;
    AvifdecLimits limits;
    AvifdecStatus status;

    seq_decode_error_clear(error);
    if (plan == 0 || callbacks == 0 ||
        callbacks->query == 0 ||
        !seq_decode_executor_valid(executor)) {
        return seq_decode_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    avifdec_memory_fill(&result, 0U, sizeof(result));
    status = avif_sequence_presentation_query(
        index, selection, presentation_index,
        &result.presentation, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_index_source(
        index, &data, &data_size, &limits, error);
    (void)data;
    (void)data_size;
    if (status != AVIFDEC_OK) return status;
    status = seq_replay_build(
        index, selection->main_track_id,
        result.presentation.main_sample_index, 0,
        &result.main, error);
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(&main_query, 0U, sizeof(main_query));
    status = callbacks->query(
        callbacks->user_data, &result.main, &limits,
        executor, &main_query, error);
    if (status != AVIFDEC_OK) {
        return seq_decode_fail(
            status, presentation_index,
            SEQ_DECODE_FOURCC('a', 'v', '0', '1'), error);
    }
    if (!seq_query_matches(
            &result.presentation.image, &main_query, 0)) {
        return seq_decode_fail(
            AVIFDEC_INVALID_DATA, presentation_index,
            SEQ_DECODE_FOURCC('a', 'v', '0', '1'), error);
    }
    {
        AvifdecImageInfo container = result.presentation.image;

        seq_merge_main_info(
            &result.presentation.image, &main_query, &container);
    }
    result.workspace_required = main_query.workspace_required;
    result.has_alpha = result.presentation.image.has_alpha;
    if (result.has_alpha != 0U) {
        status = seq_replay_build(
            index, selection->alpha_track_id,
            result.presentation.alpha_sample_index, 1,
            &result.alpha, error);
        if (status != AVIFDEC_OK) return status;
        status = avif_sequence_track_image_query(
            index, selection->alpha_track_id,
            &result.alpha_image, error);
        if (status != AVIFDEC_OK) return status;
        avifdec_memory_fill(&alpha_query, 0U, sizeof(alpha_query));
        status = callbacks->query(
            callbacks->user_data, &result.alpha, &limits,
            executor, &alpha_query, error);
        if (status != AVIFDEC_OK) {
            return seq_decode_fail(
                status, presentation_index,
                SEQ_DECODE_FOURCC('a', 'u', 'x', 'l'), error);
        }
        if (!seq_query_matches(
                &result.alpha_image, &alpha_query, 1) ||
            alpha_query.bit_depth !=
                result.alpha_image.bit_depth) {
            return seq_decode_fail(
                AVIFDEC_INVALID_DATA, presentation_index,
                SEQ_DECODE_FOURCC('a', 'u', 'x', 'l'), error);
        }
        {
            AvifdecImageInfo container = result.alpha_image;

            seq_merge_main_info(
                &result.alpha_image, &alpha_query, &container);
        }
        if (alpha_query.workspace_required >
            result.workspace_required) {
            result.workspace_required =
                alpha_query.workspace_required;
        }
    }
    result.presentation.image.workspace_required =
        result.workspace_required;
    *plan = result;
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_decode_presentation(
    const AvifSequenceIndex *index,
    const AvifSequenceSelection *selection,
    const AvifdecExecutor *executor,
    size_t presentation_index,
    const AvifSequenceDecodeCallbacks *callbacks,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifSequencePresentationInfo *presentation,
    AvifdecError *error) {
    AvifSequenceDecodePlan plan;
    AvifdecImage staged_image;
    AvifdecEntropyTrace staged_trace;
    AvifdecEntropyTrace alpha_trace;
    const unsigned char *data;
    size_t data_size;
    AvifdecLimits limits;
    AvifdecStatus status;

    seq_decode_error_clear(error);
    if (callbacks == 0 || callbacks->decode == 0 ||
        image == 0 || presentation == 0 ||
        (workspace == 0 && workspace_size != 0U)) {
        return seq_decode_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = avif_sequence_decode_plan_query(
        index, selection, executor, presentation_index,
        callbacks, &plan, error);
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < plan.workspace_required ||
        (workspace == 0 && plan.workspace_required != 0U)) {
        return seq_decode_fail(
            AVIFDEC_OUT_OF_MEMORY, 0U, 0U, error);
    }
    status = avif_sequence_index_source(
        index, &data, &data_size, &limits, error);
    (void)data;
    (void)data_size;
    if (status != AVIFDEC_OK) return status;
    staged_image = *image;
    avifdec_memory_fill(&staged_trace, 0U, sizeof(staged_trace));
    status = callbacks->decode(
        callbacks->user_data, &plan.main, &limits, executor,
        &plan.presentation.image, workspace, workspace_size,
        &staged_image, &staged_trace, error);
    if (status != AVIFDEC_OK) {
        return seq_decode_fail(
            status, presentation_index,
            SEQ_DECODE_FOURCC('a', 'v', '0', '1'), error);
    }
    if (staged_image.widths[0] !=
            plan.presentation.image.width ||
        staged_image.heights[0] !=
            plan.presentation.image.height ||
        staged_image.bit_depth !=
            plan.presentation.image.bit_depth ||
        staged_image.monochrome !=
            plan.presentation.image.monochrome) {
        return seq_decode_fail(
            AVIFDEC_INVALID_DATA, presentation_index,
            SEQ_DECODE_FOURCC('a', 'v', '0', '1'), error);
    }
    if (plan.has_alpha != 0U) {
        AvifdecImage alpha_image;

        if (staged_image.alpha_plane == 0 ||
            staged_image.alpha_stride <
                plan.alpha_image.width) {
            return seq_decode_fail(
                AVIFDEC_INVALID_ARGUMENT, presentation_index,
                SEQ_DECODE_FOURCC('a', 'u', 'x', 'l'), error);
        }
        avifdec_memory_fill(&alpha_image, 0U, sizeof(alpha_image));
        alpha_image.planes[0] = staged_image.alpha_plane;
        alpha_image.strides[0] = staged_image.alpha_stride;
        avifdec_memory_fill(&alpha_trace, 0U, sizeof(alpha_trace));
        status = callbacks->decode(
            callbacks->user_data, &plan.alpha, &limits, executor,
            &plan.alpha_image, workspace, workspace_size,
            &alpha_image, &alpha_trace, error);
        if (status != AVIFDEC_OK) {
            return seq_decode_fail(
                status, presentation_index,
                SEQ_DECODE_FOURCC('a', 'u', 'x', 'l'), error);
        }
        if (alpha_image.widths[0] !=
                plan.alpha_image.width ||
            alpha_image.heights[0] !=
                plan.alpha_image.height ||
            alpha_image.bit_depth !=
                plan.alpha_image.bit_depth ||
            alpha_image.monochrome == 0U) {
            return seq_decode_fail(
                AVIFDEC_INVALID_DATA, presentation_index,
                SEQ_DECODE_FOURCC('a', 'u', 'x', 'l'), error);
        }
        staged_image.alpha_width = alpha_image.widths[0];
        staged_image.alpha_height = alpha_image.heights[0];
        staged_image.alpha_bit_depth = alpha_image.bit_depth;
        staged_image.alpha_color_range =
            plan.presentation.image.alpha_color_range;
        staged_image.alpha_premultiplied =
            plan.presentation.image.alpha_premultiplied;
    }
    *image = staged_image;
    if (trace != 0) *trace = staged_trace;
    *presentation = plan.presentation;
    return AVIFDEC_OK;
}
