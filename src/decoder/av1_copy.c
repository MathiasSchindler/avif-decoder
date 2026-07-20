#include "av1_copy.h"
#include "base.h"

AvifdecStatus av1_plane_copy_range(size_t begin,
                                   size_t end,
                                   size_t worker_index,
                                   void *arg) {
    const Av1PlaneCopyPlan *plan = (const Av1PlaneCopyPlan *)arg;
    unsigned int plane;

    (void)worker_index;
    if (plan == 0 || begin > end ||
        end > plan->row_offset[plan->plane_count]) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (plane = 0U; plane < plan->plane_count; ++plane) {
        size_t plane_begin = plan->row_offset[plane];
        size_t plane_end = plan->row_offset[plane + 1U];
        size_t range_begin = begin > plane_begin ? begin : plane_begin;
        size_t range_end = end < plane_end ? end : plane_end;
        size_t row;

        for (row = range_begin; row < range_end; ++row) {
            size_t plane_row = row - plane_begin;

            avifdec_memory_copy(
                plan->dst_data[plane] +
                    plane_row * plan->dst_stride[plane],
                plan->src_data[plane] +
                    plane_row * plan->src_stride[plane],
                (size_t)plan->width[plane] * sizeof(uint16_t));
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_flat_copy_range(size_t begin,
                                  size_t end,
                                  size_t worker_index,
                                  void *arg) {
    const Av1FlatCopyPlan *plan = (const Av1FlatCopyPlan *)arg;

    (void)worker_index;
    if (plan == 0 || begin > end) return AVIFDEC_INVALID_ARGUMENT;
    avifdec_memory_copy(
        (unsigned char *)plan->dst + begin * plan->element_size,
        (const unsigned char *)plan->src + begin * plan->element_size,
        (end - begin) * plan->element_size);
    return AVIFDEC_OK;
}

static void av1_copy_image_scale_row(const Av1ImageScaleContext *ctx,
                                     uint32_t row) {
    uint64_t output_y0 = (uint64_t)row * ctx->source_plane_height;
    uint64_t output_y1 =
        (uint64_t)(row + 1U) * ctx->source_plane_height;
    uint32_t source_y0 =
        (uint32_t)(output_y0 / ctx->output_plane_height);
    uint32_t source_y1 =
        (uint32_t)((output_y1 + ctx->output_plane_height - 1U) /
                   ctx->output_plane_height);
    uint32_t column;

    for (column = 0U; column < ctx->output_plane_width; ++column) {
        uint64_t output_x0 =
            (uint64_t)column * ctx->source_plane_width;
        uint64_t output_x1 =
            (uint64_t)(column + 1U) * ctx->source_plane_width;
        uint32_t source_x0 =
            (uint32_t)(output_x0 / ctx->output_plane_width);
        uint32_t source_x1 =
            (uint32_t)((output_x1 + ctx->output_plane_width - 1U) /
                       ctx->output_plane_width);
        uint64_t sum = 0U;
        uint32_t source_y;

        for (source_y = source_y0; source_y < source_y1; ++source_y) {
            uint64_t source_cell_y0 =
                (uint64_t)source_y * ctx->output_plane_height;
            uint64_t source_cell_y1 =
                source_cell_y0 + ctx->output_plane_height;
            uint64_t overlap_y0 =
                output_y0 > source_cell_y0 ? output_y0 : source_cell_y0;
            uint64_t overlap_y1 =
                output_y1 < source_cell_y1 ? output_y1 : source_cell_y1;
            uint64_t weight_y = overlap_y1 - overlap_y0;
            uint32_t source_x;

            for (source_x = source_x0; source_x < source_x1; ++source_x) {
                uint64_t source_cell_x0 =
                    (uint64_t)source_x * ctx->output_plane_width;
                uint64_t source_cell_x1 =
                    source_cell_x0 + ctx->output_plane_width;
                uint64_t overlap_x0 =
                    output_x0 > source_cell_x0
                        ? output_x0
                        : source_cell_x0;
                uint64_t overlap_x1 =
                    output_x1 < source_cell_x1
                        ? output_x1
                        : source_cell_x1;
                uint64_t weight_x = overlap_x1 - overlap_x0;

                sum += ctx->source->data[ctx->plane][
                    (size_t)source_y *
                        ctx->source->stride[ctx->plane] +
                    source_x] * weight_x * weight_y;
            }
        }
        ctx->image->planes[ctx->plane][
            (size_t)row * ctx->image->strides[ctx->plane] + column] =
            (uint16_t)((sum + ctx->divisor / 2U) / ctx->divisor);
    }
}

AvifdecStatus av1_copy_image_scale_range(size_t begin,
                                         size_t end,
                                         size_t worker_index,
                                         void *arg) {
    Av1ImageScalePlan *plan = (Av1ImageScalePlan *)arg;
    unsigned int plane;

    (void)worker_index;
    if (plan == 0 || begin > end ||
        end > plan->row_offset[plan->plane_count]) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (plane = 0U; plane < plan->plane_count; ++plane) {
        size_t plane_begin = plan->row_offset[plane];
        size_t plane_end = plan->row_offset[plane + 1U];
        size_t range_begin = begin > plane_begin ? begin : plane_begin;
        size_t range_end = end < plane_end ? end : plane_end;
        size_t row;

        for (row = range_begin; row < range_end; ++row) {
            av1_copy_image_scale_row(
                &plan->planes[plane], (uint32_t)(row - plane_begin));
        }
    }
    return AVIFDEC_OK;
}

/*
 * Shared executor-aware helpers for row/plane-independent copy work.
 *
 * Each helper packages a small, stack-resident description of disjoint
 * per-plane (or flat-array) copy ranges and dispatches it across
 * the supplied executor when profitable, falling back to an equivalent serial
 * copy otherwise. Callers validate shapes before building a plan; the
 * range functions in av1_copy.c only validate the parallel range itself.
 * Dispatch remains synchronous. None of these
 * helpers retain the executor or allocate decoder workspace; plans are
 * fixed-size and live on the caller's stack during synchronous dispatch.
 */
AvifdecStatus av1_plane_copy_dispatch(
    const AvifdecExecutor *executor,
    Av1PlaneCopyPlan *plan) {
    size_t total_rows = plan->row_offset[plan->plane_count];

    if (total_rows == 0U) return AVIFDEC_OK;
    if (executor != 0 && executor->worker_count > 1U && total_rows > 1U) {
        return executor->parallel_for(
            executor->user_data, total_rows, 1U,
            av1_plane_copy_range, plan);
    }
    return av1_plane_copy_range(0U, total_rows, 0U, plan);
}

/* Splits one large flat-array memcpy (e.g. a saved motion field) into
 * disjoint element ranges across state->executor. */
AvifdecStatus av1_flat_copy_dispatch(
    const AvifdecExecutor *executor,
    void *destination,
    const void *source,
    size_t count,
    size_t element_size) {
    Av1FlatCopyPlan plan;

    if (count == 0U) return AVIFDEC_OK;
    plan.dst = destination;
    plan.src = source;
    plan.element_size = element_size;
    if (executor != 0 && executor->worker_count > 1U && count > 1U) {
        return executor->parallel_for(
            executor->user_data, count, 1U, av1_flat_copy_range, &plan);
    }
    return av1_flat_copy_range(0U, count, 0U, &plan);
}

AvifdecStatus av1_copy_planes(
    Av1FramePlanes *destination,
    const Av1FramePlanes *source,
    const Av1Sequence *sequence,
    uint32_t width,
    uint32_t height,
    const AvifdecExecutor *executor) {
    unsigned int plane_count = sequence->monochrome ? 1U : 3U;
    unsigned int plane;
    Av1PlaneCopyPlan plan;

    if (destination == 0 || source == 0 || sequence == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    avifdec_memory_fill(&plan, 0U, sizeof(plan));
    plan.plane_count = plane_count;
    for (plane = 0U; plane < plane_count; ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;
        uint32_t plane_width =
            (width + ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        uint32_t plane_height =
            (height + ((uint32_t)1U << sub_y) - 1U) >> sub_y;

        if (destination->data[plane] == 0 || source->data[plane] == 0 ||
            destination->stride[plane] < plane_width ||
            source->stride[plane] < plane_width ||
            destination->height[plane] < plane_height ||
            source->height[plane] < plane_height) {
            return AVIFDEC_INVALID_DATA;
        }
        plan.dst_data[plane] = destination->data[plane];
        plan.dst_stride[plane] = destination->stride[plane];
        plan.src_data[plane] = source->data[plane];
        plan.src_stride[plane] = source->stride[plane];
        plan.width[plane] = plane_width;
        plan.row_offset[plane + 1U] = plan.row_offset[plane] + plane_height;
    }
    return av1_plane_copy_dispatch(executor, &plan);
}

static AvifdecStatus av1_copy_image_scale_dispatch(
    const AvifdecExecutor *executor,
    Av1ImageScalePlan *plan) {
    size_t total_rows = plan->row_offset[plan->plane_count];

    if (total_rows == 0U) return AVIFDEC_OK;
    if (executor != 0 && executor->worker_count > 1U && total_rows > 1U) {
        return executor->parallel_for(
            executor->user_data, total_rows, 1U,
            av1_copy_image_scale_range, plan);
    }
    return av1_copy_image_scale_range(0U, total_rows, 0U, plan);
}

AvifdecStatus av1_copy_image(
    AvifdecImage *image,
    const Av1FramePlanes *source,
    const Av1Sequence *sequence,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t output_width,
    uint32_t output_height,
    const AvifdecExecutor *executor) {
    unsigned int plane_count = sequence->monochrome ? 1U : 3U;
    unsigned int plane;
    Av1PlaneCopyPlan copy_plan;
    Av1ImageScalePlan scale_plan;
    AvifdecStatus status;

    if (image == 0 || source == 0 || sequence == 0 ||
        output_width == 0U || output_height == 0U ||
        output_width > source_width || output_height > source_height) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    image->bit_depth = sequence->bit_depth;
    image->monochrome = sequence->monochrome;
    image->subsampling_x = sequence->subsampling_x;
    image->subsampling_y = sequence->subsampling_y;
    avifdec_memory_fill(&copy_plan, 0U, sizeof(copy_plan));
    avifdec_memory_fill(&scale_plan, 0U, sizeof(scale_plan));
    for (plane = 0U; plane < plane_count; ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : sequence->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : sequence->subsampling_y;
        uint32_t source_plane_width =
            (source_width + ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        uint32_t source_plane_height =
            (source_height + ((uint32_t)1U << sub_y) - 1U) >> sub_y;
        uint32_t output_plane_width =
            (output_width + ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        uint32_t output_plane_height =
            (output_height + ((uint32_t)1U << sub_y) - 1U) >> sub_y;

        if (image->planes[plane] == 0 ||
            image->strides[plane] < output_plane_width ||
            source->data[plane] == 0 ||
            source->stride[plane] < source_plane_width ||
            source->height[plane] < source_plane_height) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        image->widths[plane] = output_plane_width;
        image->heights[plane] = output_plane_height;
        if (source_plane_width == output_plane_width &&
            source_plane_height == output_plane_height) {
            unsigned int copy_index = copy_plan.plane_count++;

            copy_plan.dst_data[copy_index] = image->planes[plane];
            copy_plan.dst_stride[copy_index] = image->strides[plane];
            copy_plan.src_data[copy_index] = source->data[plane];
            copy_plan.src_stride[copy_index] = source->stride[plane];
            copy_plan.width[copy_index] = output_plane_width;
            copy_plan.row_offset[copy_index + 1U] =
                copy_plan.row_offset[copy_index] + output_plane_height;
        } else {
            unsigned int scale_index = scale_plan.plane_count++;
            Av1ImageScaleContext *ctx = &scale_plan.planes[scale_index];

            ctx->image = image;
            ctx->source = source;
            ctx->plane = plane;
            ctx->output_plane_width = output_plane_width;
            ctx->output_plane_height = output_plane_height;
            ctx->source_plane_width = source_plane_width;
            ctx->source_plane_height = source_plane_height;
            ctx->divisor =
                (uint64_t)source_plane_width * source_plane_height;
            scale_plan.row_offset[scale_index + 1U] =
                scale_plan.row_offset[scale_index] + output_plane_height;
        }
    }
    status = av1_plane_copy_dispatch(executor, &copy_plan);
    if (status != AVIFDEC_OK) return status;
    return av1_copy_image_scale_dispatch(executor, &scale_plan);
}
