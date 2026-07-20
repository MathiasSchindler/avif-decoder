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
