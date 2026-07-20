#include "avifdec.h"
#include "av1.h"
#include "avif_internal.h"
#include "avif_parse.h"
#include "avif_properties_internal.h"
#include "avif_sato.h"
#include "base.h"
#include "bmff.h"

#define AVIF_MAX_DERIVATION_DEPTH 16U

typedef struct {
    AvifContext *context;
    unsigned char *tile_memory;
    size_t tile_memory_size;
    void *child_workspace;
    size_t child_workspace_size;
} AvifGridWorker;

typedef struct {
    AvifdecStatus status;
    AvifdecError error;
    AvifdecEntropyTrace trace;
    uint8_t completed;
} AvifGridResult;

_Static_assert(
    _Alignof(AvifGridWorker) <= AVIF_WORKSPACE_BASE_ALIGNMENT,
    "AvifGridWorker alignment exceeds workspace sizing slack");
_Static_assert(
    _Alignof(AvifGridResult) <= AVIF_WORKSPACE_BASE_ALIGNMENT,
    "AvifGridResult alignment exceeds workspace sizing slack");

static int avif_executor_valid(
    const AvifdecExecutor *executor) {
    return executor == 0 ||
           (executor->parallel_for != 0 &&
            executor->worker_count != 0U &&
            executor->worker_count <=
                AVIFDEC_EXECUTOR_MAX_WORKERS);
}

static size_t avif_executor_width(
    const AvifdecExecutor *executor) {
    return executor == 0 ? 1U : executor->worker_count;
}

static int avif_crop_axis(uint32_t image_size,
                          uint32_t aperture_size,
                          int32_t offset_n,
                          uint32_t offset_d,
                          uint32_t *start) {
    int64_t delta = (int64_t)image_size - (int64_t)aperture_size;
    int64_t scaled_delta;
    int64_t offset_twice = (int64_t)offset_n * 2;
    int64_t numerator;
    int64_t denominator;
    int64_t result;

    if (offset_d == 0U ||
        (delta != 0 &&
         (delta > INT64_MAX / (int64_t)offset_d ||
          delta < INT64_MIN / (int64_t)offset_d))) {
        return 0;
    }
    scaled_delta = delta * (int64_t)offset_d;
    if ((offset_twice > 0 &&
         scaled_delta > INT64_MAX - offset_twice) ||
        (offset_twice < 0 &&
         scaled_delta < INT64_MIN - offset_twice)) {
        return 0;
    }
    numerator = scaled_delta + offset_twice;
    denominator = (int64_t)offset_d * 2;
    if (numerator % denominator != 0) return 0;
    result = numerator / denominator;
    if (result < 0 || (uint64_t)result > UINT32_MAX) return 0;
    *start = (uint32_t)result;
    return 1;
}

int avifdec_clap_to_crop_rect(const AvifdecCleanAperture *clap,
                              uint32_t width,
                              uint32_t height,
                              AvifdecCropRect *crop) {
    uint32_t crop_width;
    uint32_t crop_height;

    if (clap == 0 || crop == 0 || clap->width_d == 0U ||
        clap->height_d == 0U || clap->horizontal_offset_d == 0U ||
        clap->vertical_offset_d == 0U ||
        clap->width_n == 0U || clap->height_n == 0U ||
        clap->width_n % clap->width_d != 0U ||
        clap->height_n % clap->height_d != 0U) {
        return 0;
    }
    crop_width = clap->width_n / clap->width_d;
    crop_height = clap->height_n / clap->height_d;
    if (crop_width == 0U || crop_height == 0U ||
        crop_width > width || crop_height > height ||
        !avif_crop_axis(width, crop_width,
                        clap->horizontal_offset_n,
                        clap->horizontal_offset_d, &crop->x) ||
        !avif_crop_axis(height, crop_height,
                        clap->vertical_offset_n,
                        clap->vertical_offset_d, &crop->y) ||
        crop->x > width - crop_width ||
        crop->y > height - crop_height) {
        return 0;
    }
    crop->width = crop_width;
    crop->height = crop_height;
    return 1;
}

static AvifdecStatus avif_parse_properties(AvifContext *context,
                                            uint32_t item_id,
                                            AvifdecImageInfo *info) {
    AvifPropertyParseContext property_context;

    property_context.data = context->data;
    property_context.items = context->items;
    property_context.item_count = context->item_count;
    property_context.properties = context->properties;
    property_context.associations = context->associations;
    property_context.association_count = context->association_count;
    property_context.iinf = &context->iinf;
    property_context.ipco = &context->ipco;
    property_context.error = context->error;
    property_context.failed = &context->failed;
    return avif_properties_parse(&property_context, item_id, info);
}

static AvifdecStatus avif_apply_layer_slice(AvifContext *context,
                                             AvifdecSpan *spans,
                                             AvifdecImageInfo *info) {
    size_t layer;
    size_t preceding_size = 0U;
    size_t selected_size = 0U;
    size_t remaining;
    size_t span_index;

    if (!info->is_layered) return AVIFDEC_OK;
    for (layer = 0U; layer + 1U < info->layer_count; ++layer) {
        if (!avifdec_size_add(
                preceding_size, info->layer_sizes[layer],
                &preceding_size) ||
            preceding_size >= info->payload_size) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->ipco.offset,
                             AVIFDEC_FOURCC('a', '1', 'l', 'x'));
        }
    }
    info->layer_sizes[info->layer_count - 1U] =
        info->payload_size - preceding_size;
    if (info->selected_layer == 0xffU) return AVIFDEC_OK;
    if (info->selected_layer >= info->layer_count) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('l', 's', 'e', 'l'));
    }
    for (layer = 0U; layer <= info->selected_layer; ++layer) {
        if (!avifdec_size_add(
                selected_size, info->layer_sizes[layer],
                &selected_size)) {
            return avif_fail(context, AVIFDEC_OVERFLOW,
                             context->ipco.offset,
                             AVIFDEC_FOURCC('a', '1', 'l', 'x'));
        }
    }
    remaining = selected_size;
    for (span_index = 0U;
         span_index < info->extent_count;
         ++span_index) {
        if (remaining >= spans[span_index].size) {
            remaining -= spans[span_index].size;
            continue;
        }
        spans[span_index].size = remaining;
        ++span_index;
        info->extent_count = span_index;
        remaining = 0U;
        break;
    }
    if (remaining != 0U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iloc.offset,
                         AVIFDEC_FOURCC('a', '1', 'l', 'x'));
    }
    info->payload_size = selected_size;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_add_direct_workspace(
    AvifContext *context,
    AvifdecImageInfo *info) {
    size_t span_workspace;
    AvifdecStatus status = AVIFDEC_OK;

    if (info->workspace_required == 0U) {
        status = avifdec_av1_workspace_requirement(
            info, &info->workspace_required);
    }

    if (status != AVIFDEC_OK ||
        !avifdec_size_multiply(
            info->extent_count, sizeof(AvifdecSpan),
            &span_workspace) ||
        !avifdec_size_add(
            span_workspace, _Alignof(AvifdecSpan) - 1U,
            &span_workspace) ||
        !avifdec_size_add(
            info->workspace_required, span_workspace,
            &info->workspace_required) ||
        !avifdec_size_add(
            info->workspace_required,
            AVIF_WORKSPACE_BASE_ALIGNMENT - 1U,
            &info->workspace_required)) {
        return avif_fail(context, AVIFDEC_OVERFLOW,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('i', 's', 'p', 'e'));
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_query_av1_item(
    AvifContext *context,
    uint32_t item_id,
    AvifdecSpan *spans,
    size_t span_capacity,
    size_t worker_count,
    AvifdecImageInfo *info) {
    AvifLocation location;
    AvifdecLimits item_limits = context->limits;
    AvifdecStatus status;

    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = avif_parse_location(context, item_id, &location);
    if (status != AVIFDEC_OK) return status;
    status = avif_parse_properties(context, item_id, info);
    if (status != AVIFDEC_OK) return status;
    if (info->auxiliary_type == AVIFDEC_AUXILIARY_ALPHA) {
        info->has_nclx = 0U;
        info->color_primaries = 0U;
        info->transfer_characteristics = 0U;
        info->matrix_coefficients = 0U;
        info->color_range = 0U;
        info->icc_data = 0;
        info->icc_size = 0U;
    }
    status = avif_resolve_extents(
        context, &location, spans, span_capacity, info);
    if (status != AVIFDEC_OK) return status;
    status = avif_apply_layer_slice(context, spans, info);
    if (status != AVIFDEC_OK) return status;
    if (info->has_a1op) item_limits.operating_point = info->a1op_index;
    if (info->has_lsel && info->selected_layer != 0xffU) {
        item_limits.spatial_layer = info->selected_layer;
        item_limits.spatial_layer_set = 1U;
    }
    status = avifdec_av1_query_ex(
        spans, info->extent_count, &item_limits, worker_count, info,
        context->error);
    if (status != AVIFDEC_OK) return status;
    if (info->has_a1op &&
        info->a1op_index >= info->operating_point_count) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('a', '1', 'o', 'p'));
    }
    return avif_add_direct_workspace(context, info);
}

static AvifdecStatus avif_query_item(AvifContext *context,
                                      uint32_t item_id,
                                      size_t depth,
                                      AvifdecImageInfo *info);
static AvifdecStatus avif_query_item_with_workers(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    size_t worker_count,
    AvifdecImageInfo *info);

static AvifdecStatus avif_query_alpha_item(AvifContext *context,
                                            uint32_t master_item_id,
                                            AvifdecImageInfo *master_info) {
    AvifdecImageInfo alpha_info;
    AvifdecImageInfo selected_alpha_info;
    uint32_t alpha_item_id = 0U;
    size_t reference_index;
    int premultiplied = 0;

    for (reference_index = 0U;
         reference_index < context->reference_count;
         ++reference_index) {
        const AvifReference *reference =
            &context->references[reference_index];
        AvifdecStatus status;

        if (reference->type != AVIFDEC_FOURCC('a', 'u', 'x', 'l') ||
            reference->to_item_id != master_item_id) {
            continue;
        }
        status = avif_query_item(
            context, reference->from_item_id, 0U, &alpha_info);
        if (status != AVIFDEC_OK) return status;
        if (alpha_info.auxiliary_type != AVIFDEC_AUXILIARY_ALPHA) {
            continue;
        }
        if (alpha_item_id != 0U) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iref.offset,
                             AVIFDEC_FOURCC('a', 'u', 'x', 'l'));
        }
        alpha_item_id = reference->from_item_id;
        selected_alpha_info = alpha_info;
    }
    if (alpha_item_id == 0U) return AVIFDEC_OK;
    for (reference_index = 0U;
         reference_index < context->reference_count;
         ++reference_index) {
        const AvifReference *reference =
            &context->references[reference_index];

        if (reference->type != AVIFDEC_FOURCC('p', 'r', 'e', 'm') ||
            reference->from_item_id != master_item_id) {
            continue;
        }
        if (reference->to_item_id != alpha_item_id || premultiplied) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iref.offset,
                             AVIFDEC_FOURCC('p', 'r', 'e', 'm'));
        }
        premultiplied = 1;
    }
    if ((selected_alpha_info.primary_item_type !=
             AVIFDEC_FOURCC('a', 'v', '0', '1') &&
         selected_alpha_info.primary_item_type !=
             AVIFDEC_FOURCC('g', 'r', 'i', 'd')) ||
        !selected_alpha_info.monochrome ||
        selected_alpha_info.channel_count != 1U ||
        selected_alpha_info.width != master_info->width ||
        selected_alpha_info.height != master_info->height ||
        selected_alpha_info.bit_depth != master_info->bit_depth ||
        (selected_alpha_info.transform_flags &
         (AVIFDEC_TRANSFORM_CLAP | AVIFDEC_TRANSFORM_IROT |
          AVIFDEC_TRANSFORM_IMIR)) != 0U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iref.offset,
                         AVIFDEC_FOURCC('a', 'u', 'x', 'l'));
    }
    master_info->has_alpha = 1U;
    master_info->alpha_item_id = alpha_item_id;
    master_info->alpha_premultiplied = (uint8_t)premultiplied;
    master_info->alpha_bit_depth = selected_alpha_info.bit_depth;
    master_info->alpha_color_range = selected_alpha_info.color_range;
    if (selected_alpha_info.workspace_required >
        master_info->workspace_required) {
        master_info->workspace_required =
            selected_alpha_info.workspace_required;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_copy_small_payload(
    AvifContext *context,
    const AvifdecSpan *spans,
    size_t span_count,
    unsigned char *output,
    size_t output_size,
    size_t expected_size,
    uint32_t item_type) {
    size_t copied = 0U;
    size_t span_index;

    if (expected_size != output_size) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iloc.offset, item_type);
    }
    for (span_index = 0U; span_index < span_count; ++span_index) {
        if (spans[span_index].size > output_size - copied) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             spans[span_index].file_offset, item_type);
        }
        avifdec_memory_copy(
            output + copied, spans[span_index].data,
            spans[span_index].size);
        copied += spans[span_index].size;
    }
    if (copied != output_size) {
        return avif_fail(context, AVIFDEC_TRUNCATED,
                         context->iloc.offset, item_type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_image_storage_size(
    const AvifdecImageInfo *info,
    size_t *size) {
    size_t luma_samples;
    size_t sample_count;

    if (!avifdec_size_multiply(
            info->width, info->height, &luma_samples)) {
        return AVIFDEC_OVERFLOW;
    }
    sample_count = luma_samples;
    if (!info->monochrome) {
        size_t chroma_width =
            (info->width + ((size_t)1U << info->subsampling_x) - 1U) >>
            info->subsampling_x;
        size_t chroma_height =
            (info->height + ((size_t)1U << info->subsampling_y) - 1U) >>
            info->subsampling_y;
        size_t chroma_samples;

        if (!avifdec_size_multiply(
                chroma_width, chroma_height, &chroma_samples) ||
            chroma_samples > (SIZE_MAX - sample_count) / 2U) {
            return AVIFDEC_OVERFLOW;
        }
        sample_count += 2U * chroma_samples;
    }
    if (!avifdec_size_multiply(
            sample_count, sizeof(uint16_t), size)) {
        return AVIFDEC_OVERFLOW;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_query_grid_item(AvifContext *context,
                                           uint32_t item_id,
                                           size_t depth,
                                           size_t worker_count,
                                           AvifdecImageInfo *info) {
    AvifLocation location;
    unsigned char payload[12];
    size_t field_size;
    size_t expected_size;
    size_t reference_count = 0U;
    size_t reference_index;
    size_t required_tiles;
    uint32_t first_tile_id = 0U;
    AvifdecImageInfo first_tile = { 0 };
    size_t tile_storage = 0U;
    size_t tile_workspace = 0U;
    size_t tile_peak = 0U;
    size_t pixels;
    AvifdecStatus status;

    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = avif_parse_location(context, item_id, &location);
    if (status != AVIFDEC_OK) return status;
    status = avif_parse_properties(context, item_id, info);
    if (status != AVIFDEC_OK) return status;
    status = avif_resolve_extents(
        context, &location, context->query_spans,
        AVIF_MAX_RESOLVED_SPANS, info);
    if (status != AVIFDEC_OK) return status;
    if (info->payload_size != 8U && info->payload_size != 12U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iloc.offset,
                         AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
    }
    status = avif_copy_small_payload(
        context, context->query_spans, info->extent_count,
        payload, info->payload_size, info->payload_size,
        AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
    if (status != AVIFDEC_OK) return status;
    if (payload[0] != 0U || (payload[1] & 0xfeU) != 0U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iloc.offset,
                         AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
    }
    field_size = (payload[1] & 1U) != 0U ? 4U : 2U;
    expected_size = 4U + 2U * field_size;
    if (info->payload_size != expected_size) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iloc.offset,
                         AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
    }
    info->is_grid = 1U;
    info->grid_rows = (uint16_t)payload[2] + 1U;
    info->grid_columns = (uint16_t)payload[3] + 1U;
    {
        uint32_t output_width = field_size == 2U
            ? avifdec_load_u16be(payload + 4U)
            : avifdec_load_u32be(payload + 4U);
        uint32_t output_height = field_size == 2U
            ? avifdec_load_u16be(payload + 4U + field_size)
            : avifdec_load_u32be(payload + 4U + field_size);

        if (output_width == 0U || output_height == 0U ||
            output_width != info->width ||
            output_height != info->height) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iloc.offset,
                             AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
        }
    }
    if (!avifdec_size_multiply(
            info->grid_rows, info->grid_columns,
            &required_tiles)) {
        return avif_fail(context, AVIFDEC_OVERFLOW,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    for (reference_index = 0U;
         reference_index < context->reference_count;
         ++reference_index) {
        const AvifReference *reference =
            &context->references[reference_index];
        AvifdecImageInfo tile;
        size_t tile_bytes;
        size_t peak;

        if (reference->type != AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
            reference->from_item_id != item_id) {
            continue;
        }
        if (reference_count >= required_tiles) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iref.offset, reference->type);
        }
        status = avif_query_item(
            context, reference->to_item_id, depth + 1U, &tile);
        if (status != AVIFDEC_OK) return status;
        if ((tile.transform_flags &
             (AVIFDEC_TRANSFORM_CLAP | AVIFDEC_TRANSFORM_IROT |
              AVIFDEC_TRANSFORM_IMIR)) != 0U) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->ipco.offset, reference->type);
        }
        if (reference_count == 0U) {
            first_tile = tile;
            first_tile_id = reference->to_item_id;
        } else if (tile.bit_depth != first_tile.bit_depth ||
                   tile.monochrome != first_tile.monochrome ||
                   tile.subsampling_x != first_tile.subsampling_x ||
                   tile.subsampling_y != first_tile.subsampling_y ||
                   tile.channel_count != first_tile.channel_count ||
                   tile.color_range != first_tile.color_range ||
                   tile.has_nclx != first_tile.has_nclx ||
                   (tile.has_nclx &&
                    (tile.color_primaries != first_tile.color_primaries ||
                     tile.transfer_characteristics !=
                         first_tile.transfer_characteristics ||
                     tile.matrix_coefficients !=
                         first_tile.matrix_coefficients))) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->ipco.offset, reference->type);
        }
        if (reference_count != 0U) {
            size_t tile_row =
                reference_count / info->grid_columns;
            size_t tile_column =
                reference_count % info->grid_columns;
            size_t covered_width;
            size_t covered_height;
            size_t required_width;
            size_t required_height;

            if (!avifdec_size_multiply(
                    info->grid_columns - 1U,
                    first_tile.width, &covered_width) ||
                !avifdec_size_multiply(
                    info->grid_rows - 1U,
                    first_tile.height, &covered_height) ||
                info->width <= covered_width ||
                info->height <= covered_height) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 context->iloc.offset,
                                 AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
            }
            required_width = info->width - covered_width;
            required_height = info->height - covered_height;

            if ((tile_column + 1U < info->grid_columns &&
                 tile.width != first_tile.width) ||
                (tile_column + 1U == info->grid_columns &&
                 (tile.width < required_width ||
                  tile.width > first_tile.width)) ||
                (tile_row + 1U < info->grid_rows &&
                 tile.height != first_tile.height) ||
                (tile_row + 1U == info->grid_rows &&
                 (tile.height < required_height ||
                  tile.height > first_tile.height))) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 context->ipco.offset,
                                 reference->type);
            }
        }
        status = avif_image_storage_size(&tile, &tile_bytes);
        if (status != AVIFDEC_OK) {
            return status;
        }
        if (tile_bytes > tile_storage) {
            tile_storage = tile_bytes;
        }
        if (tile.workspace_required > tile_workspace) {
            tile_workspace = tile.workspace_required;
        }
        if (!avifdec_size_add(
                tile_bytes, _Alignof(uint16_t) - 1U,
                &peak) ||
            !avifdec_size_add(
                peak, tile.workspace_required, &peak)) {
            return avif_fail(context, AVIFDEC_OVERFLOW,
                             context->ipco.offset, reference->type);
        }
        if (peak > tile_peak) tile_peak = peak;
        ++reference_count;
    }
    if (reference_count != required_tiles || first_tile_id == 0U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    {
        size_t canvas_width;
        size_t canvas_height;
        size_t covered_width;
        size_t covered_height;

        if (!avifdec_size_multiply(
                info->grid_columns, first_tile.width,
                &canvas_width) ||
            !avifdec_size_multiply(
                info->grid_rows, first_tile.height,
                &canvas_height) ||
            !avifdec_size_multiply(
                info->grid_columns - 1U, first_tile.width,
                &covered_width) ||
            !avifdec_size_multiply(
                info->grid_rows - 1U, first_tile.height,
                &covered_height) ||
            info->width <= covered_width ||
            info->height <= covered_height ||
            info->width > canvas_width ||
            info->height > canvas_height) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iloc.offset,
                             AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
        }
    }
    if (info->bit_depth != first_tile.bit_depth ||
        info->channel_count != first_tile.channel_count) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('p', 'i', 'x', 'i'));
    }
    if (!first_tile.monochrome &&
        ((info->grid_columns > 1U &&
          (first_tile.width &
           (((uint32_t)1U << first_tile.subsampling_x) - 1U)) != 0U) ||
         (info->grid_rows > 1U &&
          (first_tile.height &
           (((uint32_t)1U << first_tile.subsampling_y) - 1U)) != 0U))) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
    }
    info->profile = first_tile.profile;
    info->level = first_tile.level;
    info->tier = first_tile.tier;
    info->monochrome = first_tile.monochrome;
    info->subsampling_x = first_tile.subsampling_x;
    info->subsampling_y = first_tile.subsampling_y;
    info->chroma_sample_position =
        first_tile.chroma_sample_position;
    info->grid_tile_width = first_tile.width;
    info->grid_tile_height = first_tile.height;
    if (!info->has_nclx) {
        info->has_nclx = first_tile.has_nclx;
        info->color_primaries = first_tile.color_primaries;
        info->transfer_characteristics =
            first_tile.transfer_characteristics;
        info->matrix_coefficients = first_tile.matrix_coefficients;
        info->color_range = first_tile.color_range;
    }
    info->render_width = info->width;
    info->render_height = info->height;
    info->workspace_required = tile_peak;
    if (info->width > context->limits.max_width ||
        info->height > context->limits.max_height ||
        !avifdec_size_multiply(
            info->width, info->height, &pixels) ||
        pixels > context->limits.max_pixels) {
        return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('i', 's', 'p', 'e'));
    }
    (void)tile_storage;
    if (worker_count > 1U && required_tiles > 1U) {
        AvifdecArena sizing;
        size_t worker_bytes;
        size_t tile_id_bytes;
        size_t result_bytes;
        size_t worker_index;
        size_t required;

        if (!avifdec_size_multiply(
                worker_count, sizeof(AvifGridWorker),
                &worker_bytes) ||
            !avifdec_size_multiply(
                required_tiles, sizeof(uint32_t),
                &tile_id_bytes) ||
            !avifdec_size_multiply(
                required_tiles, sizeof(AvifGridResult),
                &result_bytes)) {
            return avif_fail(
                context, AVIFDEC_OVERFLOW,
                context->ipco.offset,
                AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
        }
        avifdec_arena_init_sizing(&sizing);
        (void)avifdec_arena_allocate(
            &sizing, worker_bytes,
            _Alignof(AvifGridWorker));
        (void)avifdec_arena_allocate(
            &sizing, tile_id_bytes,
            _Alignof(uint32_t));
        (void)avifdec_arena_allocate(
            &sizing, result_bytes,
            _Alignof(AvifGridResult));
        for (worker_index = 0U;
             worker_index < worker_count;
             ++worker_index) {
            (void)avifdec_arena_allocate(
                &sizing, sizeof(AvifContext),
                _Alignof(AvifContext));
            (void)avifdec_arena_allocate(
                &sizing, tile_storage,
                _Alignof(uint16_t));
            (void)avifdec_arena_allocate(
                &sizing, tile_workspace, 1U);
        }
        required = avifdec_arena_required(&sizing);
        if (sizing.status != AVIFDEC_OK ||
            !avifdec_size_add(
                required,
                AVIF_WORKSPACE_BASE_ALIGNMENT - 1U,
                &info->workspace_required)) {
            return avif_fail(
                context, AVIFDEC_OVERFLOW,
                context->ipco.offset,
                AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_query_sato_item(AvifContext *context,
                                           uint32_t item_id,
                                           size_t depth,
                                           AvifdecImageInfo *info) {
    AvifLocation location;
    AvifSatoProgram program;
    uint32_t input_ids[AVIF_SATO_MAX_INPUTS];
    size_t input_count = 0U;
    size_t reference_index;
    size_t input_index;
    size_t input_storage = 0U;
    size_t maximum_workspace = 0U;
    AvifdecImageInfo first_input = { 0 };
    AvifdecStatus status;

    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = avif_parse_location(context, item_id, &location);
    if (status != AVIFDEC_OK) return status;
    status = avif_parse_properties(context, item_id, info);
    if (status != AVIFDEC_OK) return status;
    status = avif_resolve_extents(
        context, &location, context->query_spans,
        AVIF_MAX_RESOLVED_SPANS, info);
    if (status != AVIFDEC_OK) return status;
    for (reference_index = 0U;
         reference_index < context->reference_count;
         ++reference_index) {
        const AvifReference *reference =
            &context->references[reference_index];

        if (reference->type != AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
            reference->from_item_id != item_id) {
            continue;
        }
        if (input_count >= AVIF_SATO_MAX_INPUTS) {
            return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                             context->iref.offset, reference->type);
        }
        input_ids[input_count++] = reference->to_item_id;
    }
    if (input_count == 0U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    status = avif_sato_parse(
        &program, context->query_spans, info->extent_count,
        input_count, context->error);
    if (status != AVIFDEC_OK) return status;
    if (info->bit_depth < 8U || info->bit_depth > 16U) {
        return avif_fail(context, AVIFDEC_UNSUPPORTED,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('p', 'i', 'x', 'i'));
    }
    for (input_index = 0U; input_index < input_count; ++input_index) {
        AvifdecImageInfo input;
        size_t storage;
        size_t aligned_storage;

        status = avif_query_item(
            context, input_ids[input_index], depth + 1U, &input);
        if (status != AVIFDEC_OK) return status;
        if ((input.transform_flags &
             (AVIFDEC_TRANSFORM_CLAP | AVIFDEC_TRANSFORM_IROT |
              AVIFDEC_TRANSFORM_IMIR)) != 0U) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->ipco.offset,
                             AVIFDEC_FOURCC('s', 'a', 't', 'o'));
        }
        if (input_index == 0U) {
            first_input = input;
        } else if (input.width != first_input.width ||
                   input.height != first_input.height ||
                   input.channel_count != first_input.channel_count ||
                   input.monochrome != first_input.monochrome ||
                   input.subsampling_x != first_input.subsampling_x ||
                   input.subsampling_y != first_input.subsampling_y ||
                   input.has_nclx != first_input.has_nclx ||
                   (input.has_nclx &&
                    (input.color_primaries !=
                         first_input.color_primaries ||
                     input.transfer_characteristics !=
                         first_input.transfer_characteristics ||
                     input.matrix_coefficients !=
                         first_input.matrix_coefficients ||
                     input.color_range != first_input.color_range))) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->ipco.offset,
                             AVIFDEC_FOURCC('s', 'a', 't', 'o'));
        }
        status = avif_image_storage_size(&input, &storage);
        if (status != AVIFDEC_OK ||
            !avifdec_size_add(
                storage, _Alignof(uint16_t) - 1U,
                &aligned_storage) ||
            !avifdec_size_add(
                input_storage, aligned_storage,
                &input_storage)) {
            return avif_fail(context, AVIFDEC_OVERFLOW,
                             context->ipco.offset,
                             AVIFDEC_FOURCC('s', 'a', 't', 'o'));
        }
        if (input.workspace_required > maximum_workspace) {
            maximum_workspace = input.workspace_required;
        }
    }
    if (info->width != first_input.width ||
        info->height != first_input.height ||
        info->channel_count != first_input.channel_count) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('s', 'a', 't', 'o'));
    }
    info->monochrome = first_input.monochrome;
    info->subsampling_x = first_input.subsampling_x;
    info->subsampling_y = first_input.subsampling_y;
    info->chroma_sample_position =
        first_input.chroma_sample_position;
    if (!info->has_nclx) {
        info->has_nclx = first_input.has_nclx;
        info->color_primaries = first_input.color_primaries;
        info->transfer_characteristics =
            first_input.transfer_characteristics;
        info->matrix_coefficients =
            first_input.matrix_coefficients;
        info->color_range = first_input.color_range;
    }
    if (!avifdec_size_add(
            input_storage, maximum_workspace,
            &info->workspace_required)) {
        return avif_fail(context, AVIFDEC_OVERFLOW,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('s', 'a', 't', 'o'));
    }
    info->sample_transform_present = 1U;
    info->sample_transform_intermediate_bits =
        program.intermediate_bits;
    info->sample_transform_token_count = program.token_count;
    info->sample_transform_input_count = (uint8_t)input_count;
    info->render_width = info->width;
    info->render_height = info->height;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_query_tone_map_item(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    AvifdecImageInfo *info) {
    AvifLocation location;
    AvifdecImageInfo base;
    AvifdecImageInfo gain;
    uint32_t input_ids[2];
    size_t input_count = 0U;
    size_t reference_index;
    size_t span_index;
    uint64_t checksum = 1469598103934665603ULL;
    AvifdecStatus status;

    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = avif_parse_location(context, item_id, &location);
    if (status != AVIFDEC_OK) return status;
    status = avif_parse_properties(context, item_id, info);
    if (status != AVIFDEC_OK) return status;
    status = avif_resolve_extents(
        context, &location, context->query_spans,
        AVIF_MAX_RESOLVED_SPANS, info);
    if (status != AVIFDEC_OK) return status;
    for (span_index = 0U; span_index < info->extent_count; ++span_index) {
        size_t byte_index;

        for (byte_index = 0U;
             byte_index < context->query_spans[span_index].size;
             ++byte_index) {
            checksum ^=
                context->query_spans[span_index].data[byte_index];
            checksum *= 1099511628211ULL;
        }
    }
    for (reference_index = 0U;
         reference_index < context->reference_count;
         ++reference_index) {
        const AvifReference *reference =
            &context->references[reference_index];

        if (reference->type != AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
            reference->from_item_id != item_id) {
            continue;
        }
        if (input_count >= 2U) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iref.offset, reference->type);
        }
        input_ids[input_count++] = reference->to_item_id;
    }
    if (input_count != 2U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    status = avif_query_item(
        context, input_ids[0], depth + 1U, &base);
    if (status != AVIFDEC_OK) return status;
    status = avif_query_item(
        context, input_ids[1], depth + 1U, &gain);
    if (status != AVIFDEC_OK) return status;
    if (info->width != base.width ||
        info->height != base.height ||
        info->bit_depth != base.bit_depth ||
        info->channel_count != base.channel_count) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('t', 'm', 'a', 'p'));
    }
    info->profile = base.profile;
    info->level = base.level;
    info->tier = base.tier;
    info->monochrome = base.monochrome;
    info->subsampling_x = base.subsampling_x;
    info->subsampling_y = base.subsampling_y;
    info->chroma_sample_position = base.chroma_sample_position;
    if (!info->has_nclx) {
        info->has_nclx = base.has_nclx;
        info->color_primaries = base.color_primaries;
        info->transfer_characteristics =
            base.transfer_characteristics;
        info->matrix_coefficients = base.matrix_coefficients;
        info->color_range = base.color_range;
    }
    info->render_width = info->width;
    info->render_height = info->height;
    info->workspace_required = base.workspace_required;
    info->gain_map_present = 1U;
    info->tone_map_base_item_id = input_ids[0];
    info->tone_map_gain_item_id = input_ids[1];
    info->tone_map_metadata_size = info->payload_size;
    info->tone_map_metadata_checksum = checksum;
    (void)gain;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_query_item_with_workers(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    size_t worker_count,
    AvifdecImageInfo *info) {
    int item_index;
    uint32_t item_type;

    if (depth > AVIF_MAX_DERIVATION_DEPTH) {
        return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    item_index = avif_find_item(context, item_id);
    if (item_index < 0) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iinf.offset,
                         AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    item_type = context->items[item_index].type;
    if (item_type == AVIFDEC_FOURCC('a', 'v', '0', '1')) {
        return avif_query_av1_item(
            context, item_id, context->query_spans,
            AVIF_MAX_RESOLVED_SPANS, worker_count, info);
    }
    if (item_type == AVIFDEC_FOURCC('g', 'r', 'i', 'd')) {
        return avif_query_grid_item(
            context, item_id, depth, worker_count, info);
    }
    if (item_type == AVIFDEC_FOURCC('s', 'a', 't', 'o')) {
        return avif_query_sato_item(
            context, item_id, depth, info);
    }
    if (item_type == AVIFDEC_FOURCC('t', 'm', 'a', 'p')) {
        return avif_query_tone_map_item(
            context, item_id, depth, info);
    }
    return avif_fail(context, AVIFDEC_UNSUPPORTED,
                     context->iinf.offset, item_type);
}

static AvifdecStatus avif_query_item(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    AvifdecImageInfo *info) {
    return avif_query_item_with_workers(
        context, item_id, depth, 1U, info);
}

static AvifdecStatus avif_bind_image_storage(
    void *memory,
    size_t memory_size,
    const AvifdecImageInfo *info,
    AvifdecImage *image) {
    size_t required;
    size_t luma_samples;
    size_t chroma_width;
    size_t chroma_height;
    size_t chroma_samples;

    if (avif_image_storage_size(info, &required) != AVIFDEC_OK) {
        return AVIFDEC_OVERFLOW;
    }
    if (memory == 0 || memory_size < required) {
        return AVIFDEC_OUT_OF_MEMORY;
    }
    avifdec_memory_fill(image, 0U, sizeof(*image));
    luma_samples = (size_t)info->width * info->height;
    image->planes[0] = (uint16_t *)memory;
    image->strides[0] = info->width;
    if (!info->monochrome) {
        chroma_width =
            (info->width + ((size_t)1U << info->subsampling_x) - 1U) >>
            info->subsampling_x;
        chroma_height =
            (info->height + ((size_t)1U << info->subsampling_y) - 1U) >>
            info->subsampling_y;
        chroma_samples = chroma_width * chroma_height;
        image->planes[1] = image->planes[0] + luma_samples;
        image->planes[2] = image->planes[1] + chroma_samples;
        image->strides[1] = chroma_width;
        image->strides[2] = chroma_width;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_validate_output_image(
    const AvifdecImageInfo *info,
    const AvifdecImage *image) {
    unsigned int plane_count = info->monochrome ? 1U : 3U;
    unsigned int plane;

    for (plane = 0U; plane < plane_count; ++plane) {
        uint32_t width = plane == 0U
            ? info->width
            : (info->width +
               ((uint32_t)1U << info->subsampling_x) - 1U) >>
                  info->subsampling_x;

        if (image->planes[plane] == 0 ||
            image->strides[plane] < width) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
    }
    return AVIFDEC_OK;
}

static void avif_copy_grid_tile(const AvifdecImageInfo *grid_info,
                                const AvifdecImage *tile,
                                AvifdecImage *output,
                                size_t tile_index) {
    size_t row = tile_index / grid_info->grid_columns;
    size_t column = tile_index % grid_info->grid_columns;
    unsigned int plane_count = grid_info->monochrome ? 1U : 3U;
    unsigned int plane;

    for (plane = 0U; plane < plane_count; ++plane) {
        unsigned int sub_x =
            plane == 0U ? 0U : grid_info->subsampling_x;
        unsigned int sub_y =
            plane == 0U ? 0U : grid_info->subsampling_y;
        size_t destination_x =
            (column * grid_info->grid_tile_width) >> sub_x;
        size_t destination_y =
            (row * grid_info->grid_tile_height) >> sub_y;
        size_t output_width =
            (grid_info->width + ((size_t)1U << sub_x) - 1U) >>
            sub_x;
        size_t output_height =
            (grid_info->height + ((size_t)1U << sub_y) - 1U) >>
            sub_y;
        size_t copy_width = tile->widths[plane];
        size_t copy_height = tile->heights[plane];
        size_t source_y;

        if (copy_width > output_width - destination_x) {
            copy_width = output_width - destination_x;
        }
        if (copy_height > output_height - destination_y) {
            copy_height = output_height - destination_y;
        }
        for (source_y = 0U; source_y < copy_height; ++source_y) {
            avifdec_memory_copy(
                output->planes[plane] +
                    (destination_y + source_y) *
                        output->strides[plane] +
                    destination_x,
                tile->planes[plane] +
                    source_y * tile->strides[plane],
                copy_width * sizeof(uint16_t));
        }
    }
}

static void avif_finish_grid_image(
    const AvifdecImageInfo *grid_info,
    AvifdecImage *image) {
    image->widths[0] = grid_info->width;
    image->heights[0] = grid_info->height;
    image->bit_depth = grid_info->bit_depth;
    image->monochrome = grid_info->monochrome;
    image->subsampling_x = grid_info->subsampling_x;
    image->subsampling_y = grid_info->subsampling_y;
    if (!grid_info->monochrome) {
        image->widths[1] =
            (grid_info->width +
             ((uint32_t)1U <<
              grid_info->subsampling_x) - 1U) >>
            grid_info->subsampling_x;
        image->widths[2] = image->widths[1];
        image->heights[1] =
            (grid_info->height +
             ((uint32_t)1U <<
              grid_info->subsampling_y) - 1U) >>
            grid_info->subsampling_y;
        image->heights[2] = image->heights[1];
    }
}

static AvifdecStatus avif_decode_item(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace);

typedef struct {
    AvifGridWorker *workers;
    size_t worker_count;
    const uint32_t *tile_ids;
    AvifGridResult *results;
    size_t depth;
    const AvifdecImageInfo *grid_info;
    AvifdecImage *output;
    int trace_enabled;
} AvifGridParallelContext;

static AvifdecStatus avif_decode_grid_range(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg) {
    AvifGridParallelContext *parallel =
        (AvifGridParallelContext *)arg;
    AvifGridWorker *worker;
    size_t tile_index;

    if (parallel == 0 ||
        worker_index >= parallel->worker_count) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    worker = &parallel->workers[worker_index];
    for (tile_index = begin; tile_index < end; ++tile_index) {
        AvifGridResult *result =
            &parallel->results[tile_index];
        AvifdecImageInfo tile_info;
        AvifdecImage tile_image;
        AvifdecStatus status;

        avifdec_memory_fill(result, 0U, sizeof(*result));
        worker->context->error = &result->error;
        worker->context->failed = 0;
        status = avif_query_item(
            worker->context,
            parallel->tile_ids[tile_index],
            parallel->depth + 1U, &tile_info);
        if (status == AVIFDEC_OK) {
            status = avif_bind_image_storage(
                worker->tile_memory,
                worker->tile_memory_size,
                &tile_info, &tile_image);
        }
        if (status == AVIFDEC_OK) {
            status = avif_decode_item(
                worker->context,
                parallel->tile_ids[tile_index],
                parallel->depth + 1U, 0,
                worker->child_workspace,
                worker->child_workspace_size,
                &tile_image,
                parallel->trace_enabled
                    ? &result->trace : 0);
        }
        if (status == AVIFDEC_OK) {
            avif_copy_grid_tile(
                parallel->grid_info, &tile_image,
                parallel->output, tile_index);
        } else if (result->error.status == AVIFDEC_OK) {
            result->error.status = status;
        }
        result->status = status;
        result->completed = 1U;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_grid_propagate_error(
    AvifContext *context,
    const AvifGridResult *result,
    AvifdecStatus status) {
    if (context->error != 0 &&
        context->error->status == AVIFDEC_OK) {
        if (result != 0 &&
            result->error.status != AVIFDEC_OK) {
            *context->error = result->error;
        } else {
            context->error->status = status;
            context->error->offset = context->iref.offset;
            context->error->context =
                AVIFDEC_FOURCC('g', 'r', 'i', 'd');
        }
    }
    context->failed = 1;
    return status;
}

static void avif_grid_trace_add(
    AvifdecEntropyTrace *trace,
    const AvifdecEntropyTrace *tile) {
    trace->frame_count += tile->frame_count;
    trace->tile_count += tile->tile_count;
    trace->partition_nodes += tile->partition_nodes;
    trace->block_count += tile->block_count;
    trace->transform_count += tile->transform_count;
    trace->coefficient_count += tile->coefficient_count;
    trace->checksum =
        (trace->checksum * 0x100000001b3ULL) ^
        tile->checksum;
    trace->restoration_checksum =
        (trace->restoration_checksum *
         0x100000001b3ULL) ^
        tile->restoration_checksum;
}

typedef struct {
    const AvifSatoProgram *program;
    const AvifdecImage *inputs;
    size_t input_count;
    AvifdecImage *output;
    size_t plane_offsets[4];
    uint32_t plane_widths[3];
    int64_t output_minimums[3];
    int64_t output_maximums[3];
    unsigned int plane_count;
} AvifSatoParallelContext;

static AvifdecStatus avif_sato_apply_range(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg) {
    AvifSatoParallelContext *parallel =
        (AvifSatoParallelContext *)arg;

    (void)worker_index;
    if (parallel == 0 || begin > end ||
        end > parallel->plane_offsets[
            parallel->plane_count]) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    while (begin < end) {
        unsigned int plane = 0U;
        size_t segment_end;
        AvifdecStatus status;

        while (plane + 1U < parallel->plane_count &&
               begin >= parallel->plane_offsets[plane + 1U]) {
            ++plane;
        }
        segment_end = end;
        if (segment_end >
            parallel->plane_offsets[plane + 1U]) {
            segment_end =
                parallel->plane_offsets[plane + 1U];
        }
        status = avif_sato_apply_rows(
            parallel->program, parallel->inputs,
            parallel->input_count, plane,
            parallel->plane_widths[plane],
            (uint32_t)(
                begin - parallel->plane_offsets[plane]),
            (uint32_t)(
                segment_end -
                parallel->plane_offsets[plane]),
            parallel->output_minimums[plane],
            parallel->output_maximums[plane],
            parallel->output);
        if (status != AVIFDEC_OK) return status;
        begin = segment_end;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_decode_grid_parallel(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    const AvifdecImageInfo *grid_info) {
    AvifdecArena arena;
    AvifGridParallelContext parallel;
    AvifGridWorker *workers;
    AvifGridResult *results;
    uint32_t *tile_ids;
    size_t worker_bytes;
    size_t result_bytes;
    size_t tile_id_bytes;
    size_t tile_count;
    size_t tile_memory_size = 0U;
    size_t child_workspace_size = 0U;
    size_t reference_index;
    size_t tile_index = 0U;
    size_t worker_index;
    AvifdecStatus status;

    if (!avifdec_size_multiply(
            grid_info->grid_rows,
            grid_info->grid_columns,
            &tile_count) ||
        !avifdec_size_multiply(
            executor->worker_count,
            sizeof(*workers), &worker_bytes) ||
        !avifdec_size_multiply(
            tile_count, sizeof(*results),
            &result_bytes) ||
        !avifdec_size_multiply(
            tile_count, sizeof(*tile_ids),
            &tile_id_bytes)) {
        return avif_grid_propagate_error(
            context, 0, AVIFDEC_OVERFLOW);
    }
    avifdec_arena_init(&arena, workspace, workspace_size);
    workers = (AvifGridWorker *)avifdec_arena_allocate(
        &arena, worker_bytes, _Alignof(AvifGridWorker));
    tile_ids = (uint32_t *)avifdec_arena_allocate(
        &arena, tile_id_bytes, _Alignof(uint32_t));
    results = (AvifGridResult *)avifdec_arena_allocate(
        &arena, result_bytes, _Alignof(AvifGridResult));
    if (arena.status != AVIFDEC_OK ||
        workers == 0 || tile_ids == 0 || results == 0) {
        return avif_grid_propagate_error(
            context, 0, arena.status);
    }
    for (reference_index = 0U;
         reference_index < context->reference_count;
         ++reference_index) {
        const AvifReference *reference =
            &context->references[reference_index];

        if (reference->type !=
                AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
            reference->from_item_id != item_id) {
            continue;
        }
        if (tile_index >= tile_count) {
            return avif_grid_propagate_error(
                context, 0, AVIFDEC_INVALID_DATA);
        }
        tile_ids[tile_index++] = reference->to_item_id;
    }
    if (tile_index != tile_count) {
        return avif_grid_propagate_error(
            context, 0, AVIFDEC_INVALID_DATA);
    }
    for (tile_index = 0U;
         tile_index < tile_count;
         ++tile_index) {
        AvifdecImageInfo tile_info;
        size_t storage;

        status = avif_query_item(
            context, tile_ids[tile_index],
            depth + 1U, &tile_info);
        if (status != AVIFDEC_OK) return status;
        status = avif_image_storage_size(
            &tile_info, &storage);
        if (status != AVIFDEC_OK) return status;
        if (storage > tile_memory_size) {
            tile_memory_size = storage;
        }
        if (tile_info.workspace_required >
            child_workspace_size) {
            child_workspace_size =
                tile_info.workspace_required;
        }
    }
    avifdec_memory_fill(results, 0U, result_bytes);
    avifdec_memory_fill(workers, 0U, worker_bytes);
    for (worker_index = 0U;
         worker_index < executor->worker_count;
         ++worker_index) {
        workers[worker_index].context =
            (AvifContext *)avifdec_arena_allocate(
                &arena, sizeof(AvifContext),
                _Alignof(AvifContext));
        workers[worker_index].tile_memory =
            (unsigned char *)avifdec_arena_allocate(
                &arena, tile_memory_size,
                _Alignof(uint16_t));
        workers[worker_index].child_workspace =
            avifdec_arena_allocate(
                &arena, child_workspace_size, 1U);
        workers[worker_index].tile_memory_size =
            tile_memory_size;
        workers[worker_index].child_workspace_size =
            child_workspace_size;
        if (arena.status != AVIFDEC_OK ||
            workers[worker_index].context == 0 ||
            workers[worker_index].tile_memory == 0 ||
            workers[worker_index].child_workspace == 0) {
            return avif_grid_propagate_error(
                context, 0, arena.status);
        }
        /*
         * Parsed tables and span storage are inline. The worker only needs
         * its error pointer redirected before each decode.
         */
        avifdec_memory_copy(
            workers[worker_index].context,
            context, sizeof(*context));
    }
    if (trace != 0) {
        avifdec_memory_fill(trace, 0U, sizeof(*trace));
    }
    parallel.workers = workers;
    parallel.worker_count = executor->worker_count;
    parallel.tile_ids = tile_ids;
    parallel.results = results;
    parallel.depth = depth;
    parallel.grid_info = grid_info;
    parallel.output = image;
    parallel.trace_enabled = trace != 0;
    status = executor->parallel_for(
        executor->user_data, tile_count, 1U,
        avif_decode_grid_range, &parallel);
    if (status != AVIFDEC_OK) {
        return avif_grid_propagate_error(
            context, 0, status);
    }
    for (tile_index = 0U;
         tile_index < tile_count;
         ++tile_index) {
        if (!results[tile_index].completed) {
            return avif_grid_propagate_error(
                context, 0, AVIFDEC_INVALID_ARGUMENT);
        }
        if (results[tile_index].status != AVIFDEC_OK) {
            return avif_grid_propagate_error(
                context, &results[tile_index],
                results[tile_index].status);
        }
        if (trace != 0) {
            avif_grid_trace_add(
                trace, &results[tile_index].trace);
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_decode_item(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace) {
    int item_index = avif_find_item(context, item_id);
    uint32_t item_type;

    if (depth > AVIF_MAX_DERIVATION_DEPTH) {
        return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    if (item_index < 0) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iinf.offset,
                         AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    item_type = context->items[item_index].type;
    if (item_type == AVIFDEC_FOURCC('a', 'v', '0', '1')) {
        AvifdecImageInfo info;
        AvifdecLimits item_limits = context->limits;
        AvifdecStatus status = avif_query_av1_item(
            context, item_id, context->query_spans,
            AVIF_MAX_RESOLVED_SPANS, avif_executor_width(executor), &info);

        if (status != AVIFDEC_OK) return status;
        if (info.has_a1op) {
            item_limits.operating_point = info.a1op_index;
        }
        if (info.has_lsel && info.selected_layer != 0xffU) {
            item_limits.spatial_layer = info.selected_layer;
            item_limits.spatial_layer_set = 1U;
        }
        return avifdec_av1_decode_ex(
            context->query_spans, info.extent_count,
            &item_limits, executor, &info, workspace, workspace_size,
            image, trace, context->error);
    }
    if (item_type == AVIFDEC_FOURCC('g', 'r', 'i', 'd')) {
        AvifdecImageInfo grid_info;
        AvifdecImageInfo tile_info;
        AvifdecArena arena;
        AvifdecImage tile_image;
        AvifdecEntropyTrace tile_trace;
        unsigned char *tile_memory;
        void *child_workspace;
        size_t tile_bytes;
        size_t tile_index = 0U;
        size_t reference_index;
        uint32_t first_tile_id = 0U;
        AvifdecStatus status = avif_query_grid_item(
            context, item_id, depth,
            avif_executor_width(executor), &grid_info);

        if (status != AVIFDEC_OK) return status;
        status = avif_validate_output_image(&grid_info, image);
        if (status != AVIFDEC_OK) return status;
        if (executor != 0 &&
            executor->worker_count > 1U &&
            (grid_info.grid_rows > 1U ||
             grid_info.grid_columns > 1U)) {
            status = avif_decode_grid_parallel(
                context, item_id, depth, executor,
                workspace, workspace_size, image, trace,
                &grid_info);
            if (status != AVIFDEC_OK) return status;
            avif_finish_grid_image(&grid_info, image);
            return AVIFDEC_OK;
        }
        for (reference_index = 0U;
             reference_index < context->reference_count;
             ++reference_index) {
            const AvifReference *reference =
                &context->references[reference_index];

            if (reference->type ==
                    AVIFDEC_FOURCC('d', 'i', 'm', 'g') &&
                reference->from_item_id == item_id) {
                first_tile_id = reference->to_item_id;
                break;
            }
        }
        if (first_tile_id == 0U) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iref.offset,
                             AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
        }
        status = avif_query_item(
            context, first_tile_id, depth + 1U, &tile_info);
        if (status != AVIFDEC_OK) return status;
        status = avif_image_storage_size(&tile_info, &tile_bytes);
        if (status != AVIFDEC_OK) return status;
        avifdec_arena_init(&arena, workspace, workspace_size);
        tile_memory = (unsigned char *)avifdec_arena_allocate(
            &arena, tile_bytes, _Alignof(uint16_t));
        if (arena.status != AVIFDEC_OK || tile_memory == 0) {
            return arena.status;
        }
        child_workspace = arena.data + arena.used;
        if (trace != 0) {
            avifdec_memory_fill(trace, 0U, sizeof(*trace));
        }
        for (reference_index = 0U;
             reference_index < context->reference_count;
             ++reference_index) {
            const AvifReference *reference =
                &context->references[reference_index];

            if (reference->type !=
                    AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
                reference->from_item_id != item_id) {
                continue;
            }
            status = avif_query_item(
                context, reference->to_item_id,
                depth + 1U, &tile_info);
            if (status != AVIFDEC_OK) return status;
            status = avif_bind_image_storage(
                tile_memory, tile_bytes, &tile_info, &tile_image);
            if (status != AVIFDEC_OK) return status;
            status = avif_decode_item(
                context, reference->to_item_id, depth + 1U, 0,
                child_workspace, arena.size - arena.used,
                &tile_image, trace == 0 ? 0 : &tile_trace);
            if (status != AVIFDEC_OK) return status;
            avif_copy_grid_tile(
                &grid_info, &tile_image, image, tile_index);
            if (trace != 0) {
                trace->frame_count += tile_trace.frame_count;
                trace->tile_count += tile_trace.tile_count;
                trace->partition_nodes += tile_trace.partition_nodes;
                trace->block_count += tile_trace.block_count;
                trace->transform_count += tile_trace.transform_count;
                trace->coefficient_count +=
                    tile_trace.coefficient_count;
                trace->checksum =
                    (trace->checksum * 0x100000001b3ULL) ^
                    tile_trace.checksum;
                trace->restoration_checksum =
                    (trace->restoration_checksum *
                     0x100000001b3ULL) ^
                    tile_trace.restoration_checksum;
            }
            ++tile_index;
        }
        avif_finish_grid_image(&grid_info, image);
        return AVIFDEC_OK;
    }
    if (item_type == AVIFDEC_FOURCC('s', 'a', 't', 'o')) {
        AvifdecImageInfo sato_info;
        AvifLocation location;
        AvifSatoProgram program;
        uint32_t input_ids[AVIF_SATO_MAX_INPUTS];
        AvifdecImageInfo input_infos[AVIF_SATO_MAX_INPUTS];
        AvifdecImage input_images[AVIF_SATO_MAX_INPUTS];
        AvifdecArena arena;
        size_t input_count = 0U;
        size_t reference_index;
        size_t input_index;
        void *child_workspace;
        AvifdecStatus status = avif_query_sato_item(
            context, item_id, depth, &sato_info);

        if (status != AVIFDEC_OK) return status;
        status = avif_validate_output_image(&sato_info, image);
        if (status != AVIFDEC_OK) return status;
        status = avif_parse_location(context, item_id, &location);
        if (status != AVIFDEC_OK) return status;
        {
            AvifdecImageInfo payload_info;

            avifdec_memory_fill(
                &payload_info, 0U, sizeof(payload_info));
            status = avif_resolve_extents(
                context, &location, context->query_spans,
                AVIF_MAX_RESOLVED_SPANS, &payload_info);
            if (status != AVIFDEC_OK) return status;
            status = avif_sato_parse(
                &program, context->query_spans,
                payload_info.extent_count,
                sato_info.sample_transform_input_count,
                context->error);
            if (status != AVIFDEC_OK) return status;
        }
        for (reference_index = 0U;
             reference_index < context->reference_count;
             ++reference_index) {
            const AvifReference *reference =
                &context->references[reference_index];

            if (reference->type !=
                    AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
                reference->from_item_id != item_id) {
                continue;
            }
            input_ids[input_count++] = reference->to_item_id;
        }
        avifdec_arena_init(&arena, workspace, workspace_size);
        for (input_index = 0U;
             input_index < input_count;
             ++input_index) {
            size_t storage;
            void *input_memory;

            status = avif_query_item(
                context, input_ids[input_index],
                depth + 1U, &input_infos[input_index]);
            if (status != AVIFDEC_OK) return status;
            status = avif_image_storage_size(
                &input_infos[input_index], &storage);
            if (status != AVIFDEC_OK) return status;
            input_memory = avifdec_arena_allocate(
                &arena, storage, _Alignof(uint16_t));
            if (arena.status != AVIFDEC_OK ||
                input_memory == 0) {
                return arena.status;
            }
            status = avif_bind_image_storage(
                input_memory, storage, &input_infos[input_index],
                &input_images[input_index]);
            if (status != AVIFDEC_OK) return status;
        }
        child_workspace = arena.data + arena.used;
        if (trace != 0) {
            avifdec_memory_fill(trace, 0U, sizeof(*trace));
        }
        for (input_index = 0U;
             input_index < input_count;
             ++input_index) {
            AvifdecEntropyTrace input_trace;

            status = avif_decode_item(
                context, input_ids[input_index], depth + 1U, 0,
                child_workspace, arena.size - arena.used,
                &input_images[input_index],
                trace == 0 ? 0 : &input_trace);
            if (status != AVIFDEC_OK) return status;
            if (trace != 0) {
                trace->frame_count += input_trace.frame_count;
                trace->tile_count += input_trace.tile_count;
                trace->partition_nodes +=
                    input_trace.partition_nodes;
                trace->block_count += input_trace.block_count;
                trace->transform_count +=
                    input_trace.transform_count;
                trace->coefficient_count +=
                    input_trace.coefficient_count;
                trace->checksum =
                    (trace->checksum * 0x100000001b3ULL) ^
                    input_trace.checksum;
            }
        }
        {
            AvifSatoParallelContext parallel;
            size_t total_rows;
            unsigned int plane;

            avifdec_memory_fill(
                &parallel, 0U, sizeof(parallel));
            parallel.program = &program;
            parallel.inputs = input_images;
            parallel.input_count = input_count;
            parallel.output = image;
            parallel.plane_count =
                sato_info.monochrome ? 1U : 3U;
            for (plane = 0U;
                 plane < parallel.plane_count;
                 ++plane) {
                uint32_t plane_width = plane == 0U
                    ? sato_info.width
                    : (sato_info.width +
                       ((uint32_t)1U <<
                        sato_info.subsampling_x) - 1U) >>
                          sato_info.subsampling_x;
                uint32_t plane_height = plane == 0U
                    ? sato_info.height
                    : (sato_info.height +
                       ((uint32_t)1U <<
                        sato_info.subsampling_y) - 1U) >>
                          sato_info.subsampling_y;
                int64_t output_minimum = 0;
                int64_t output_maximum =
                    ((int64_t)1 << sato_info.bit_depth) - 1;

                if (!sato_info.color_range) {
                    unsigned int shift =
                        sato_info.bit_depth - 8U;

                    output_minimum = (int64_t)16 << shift;
                    output_maximum =
                        (int64_t)(plane == 0U ? 235U : 240U)
                        << shift;
                }
                parallel.plane_widths[plane] = plane_width;
                parallel.output_minimums[plane] =
                    output_minimum;
                parallel.output_maximums[plane] =
                    output_maximum;
                if (!avifdec_size_add(
                        parallel.plane_offsets[plane],
                        plane_height,
                        &parallel.plane_offsets[plane + 1U])) {
                    return avif_fail(
                        context, AVIFDEC_OVERFLOW,
                        location.base_offset,
                        AVIFDEC_FOURCC('s', 'a', 't', 'o'));
                }
                image->widths[plane] = plane_width;
                image->heights[plane] = plane_height;
            }
            total_rows =
                parallel.plane_offsets[parallel.plane_count];
            if (executor != 0 &&
                executor->worker_count > 1U &&
                total_rows > 1U) {
                status = executor->parallel_for(
                    executor->user_data, total_rows, 1U,
                    avif_sato_apply_range, &parallel);
            } else {
                status = avif_sato_apply_range(
                    0U, total_rows, 0U, &parallel);
            }
            if (status != AVIFDEC_OK) return status;
        }
        image->bit_depth = sato_info.bit_depth;
        image->monochrome = sato_info.monochrome;
        image->subsampling_x = sato_info.subsampling_x;
        image->subsampling_y = sato_info.subsampling_y;
        return AVIFDEC_OK;
    }
    if (item_type == AVIFDEC_FOURCC('t', 'm', 'a', 'p')) {
        AvifdecImageInfo tone_map;
        AvifdecStatus status = avif_query_tone_map_item(
            context, item_id, depth, &tone_map);

        if (status != AVIFDEC_OK) return status;
        return avif_decode_item(
            context, tone_map.tone_map_base_item_id,
            depth + 1U, 0, workspace, workspace_size,
            image, trace);
    }
    return avif_fail(context, AVIFDEC_UNSUPPORTED,
                     context->iinf.offset, item_type);
}

static AvifdecStatus avif_trace_item(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecEntropyTrace *trace) {
    int item_index = avif_find_item(context, item_id);
    uint32_t item_type;

    if (depth > AVIF_MAX_DERIVATION_DEPTH) {
        return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    if (item_index < 0) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iinf.offset,
                         AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    item_type = context->items[item_index].type;
    if (item_type == AVIFDEC_FOURCC('a', 'v', '0', '1')) {
        AvifdecImageInfo info;
        AvifdecLimits item_limits = context->limits;
        AvifdecStatus status = avif_query_av1_item(
            context, item_id, context->query_spans,
            AVIF_MAX_RESOLVED_SPANS,
            executor == 0 ? 1U : executor->worker_count, &info);

        if (status != AVIFDEC_OK) return status;
        if (info.has_a1op) {
            item_limits.operating_point = info.a1op_index;
        }
        if (info.has_lsel && info.selected_layer != 0xffU) {
            item_limits.spatial_layer = info.selected_layer;
            item_limits.spatial_layer_set = 1U;
        }
        return avifdec_av1_trace_ex(
            context->query_spans, info.extent_count,
            &item_limits, executor, &info, workspace, workspace_size,
            trace, context->error);
    }
    if (item_type == AVIFDEC_FOURCC('g', 'r', 'i', 'd')) {
        AvifdecImageInfo grid_info;
        size_t reference_index;
        AvifdecStatus status = avif_query_grid_item(
            context, item_id, depth, 1U, &grid_info);

        if (status != AVIFDEC_OK) return status;
        avifdec_memory_fill(trace, 0U, sizeof(*trace));
        for (reference_index = 0U;
             reference_index < context->reference_count;
             ++reference_index) {
            const AvifReference *reference =
                &context->references[reference_index];
            AvifdecEntropyTrace child;

            if (reference->type !=
                    AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
                reference->from_item_id != item_id) {
                continue;
            }
            status = avif_trace_item(
                context, reference->to_item_id, depth + 1U,
                0, workspace, workspace_size, &child);
            if (status != AVIFDEC_OK) return status;
            trace->frame_count += child.frame_count;
            trace->show_existing_frame_count +=
                child.show_existing_frame_count;
            trace->tile_count += child.tile_count;
            trace->partition_nodes += child.partition_nodes;
            trace->block_count += child.block_count;
            trace->inter_block_count += child.inter_block_count;
            trace->compound_block_count +=
                child.compound_block_count;
            trace->transform_count += child.transform_count;
            trace->nonzero_transform_count +=
                child.nonzero_transform_count;
            trace->coefficient_count += child.coefficient_count;
            trace->transform_size_mask |= child.transform_size_mask;
            trace->transform_type_mask |= child.transform_type_mask;
            trace->checksum =
                (trace->checksum * 0x100000001b3ULL) ^
                child.checksum;
            trace->restoration_checksum =
                (trace->restoration_checksum *
                 0x100000001b3ULL) ^
                child.restoration_checksum;
        }
        return AVIFDEC_OK;
    }
    if (item_type == AVIFDEC_FOURCC('s', 'a', 't', 'o')) {
        AvifdecImageInfo sato_info;
        size_t reference_index;
        AvifdecStatus status = avif_query_sato_item(
            context, item_id, depth, &sato_info);

        if (status != AVIFDEC_OK) return status;
        avifdec_memory_fill(trace, 0U, sizeof(*trace));
        for (reference_index = 0U;
             reference_index < context->reference_count;
             ++reference_index) {
            const AvifReference *reference =
                &context->references[reference_index];
            AvifdecEntropyTrace child;

            if (reference->type !=
                    AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
                reference->from_item_id != item_id) {
                continue;
            }
            status = avif_trace_item(
                context, reference->to_item_id, depth + 1U,
                0, workspace, workspace_size, &child);
            if (status != AVIFDEC_OK) return status;
            trace->frame_count += child.frame_count;
            trace->tile_count += child.tile_count;
            trace->partition_nodes += child.partition_nodes;
            trace->block_count += child.block_count;
            trace->transform_count += child.transform_count;
            trace->coefficient_count += child.coefficient_count;
            trace->checksum =
                (trace->checksum * 0x100000001b3ULL) ^
                child.checksum;
        }
        return AVIFDEC_OK;
    }
    if (item_type == AVIFDEC_FOURCC('t', 'm', 'a', 'p')) {
        AvifdecImageInfo tone_map;
        AvifdecStatus status = avif_query_tone_map_item(
            context, item_id, depth, &tone_map);

        if (status != AVIFDEC_OK) return status;
        return avif_trace_item(
            context, tone_map.tone_map_base_item_id,
            depth + 1U, executor, workspace, workspace_size, trace);
    }
    return avif_fail(context, AVIFDEC_UNSUPPORTED,
                     context->iinf.offset, item_type);
}

AvifdecStatus avifdec_query_ex(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifdecImageInfo *info,
    AvifdecError *error) {
    AvifContext context;
    AvifdecSpan resolved_spans[AVIF_MAX_RESOLVED_SPANS];
    AvifdecStatus status;
    uint32_t primary_id = 0U;
    size_t pixels;
    int primary_item_index;
    uint32_t primary_item_type;

    if (info == 0 || !avif_executor_valid(executor) ||
        (data == 0 && size != 0U) ||
        (spans == 0 && span_capacity != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = avif_open_context(
        &context, data, size, limits, &primary_id, error);
    if (status != AVIFDEC_OK) return status;
    primary_item_index = avif_find_item(&context, primary_id);
    if (primary_item_index < 0) {
        return avif_fail(&context, AVIFDEC_INVALID_DATA,
                         context.pitm.offset,
                         AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    primary_item_type = context.items[primary_item_index].type;
    if (primary_item_type == AVIFDEC_FOURCC('a', 'v', '0', '1')) {
        status = avif_query_av1_item(
            &context, primary_id, resolved_spans,
            AVIF_MAX_RESOLVED_SPANS,
            avif_executor_width(executor), info);
    } else {
        status = avif_query_item_with_workers(
            &context, primary_id, 0U,
            avif_executor_width(executor), info);
        if (status == AVIFDEC_OK && spans != 0) {
            AvifLocation location;
            AvifdecImageInfo payload_info;

            avifdec_memory_fill(
                &payload_info, 0U, sizeof(payload_info));
            status = avif_parse_location(
                &context, primary_id, &location);
            if (status == AVIFDEC_OK) {
                status = avif_resolve_extents(
                    &context, &location, resolved_spans,
                    AVIF_MAX_RESOLVED_SPANS, &payload_info);
            }
            if (status == AVIFDEC_OK &&
                payload_info.extent_count != info->extent_count) {
                status = avif_fail(
                    &context, AVIFDEC_INVALID_DATA,
                    context.iloc.offset, primary_item_type);
            }
        }
    }
    if (status != AVIFDEC_OK) return status;
    if (spans != 0) {
        if (span_capacity < info->extent_count) {
            return avif_fail(&context, AVIFDEC_OUT_OF_MEMORY, context.iloc.offset, context.iloc.type);
        }
        avifdec_memory_copy(spans, resolved_spans, info->extent_count * sizeof(*spans));
    }
    if (info->width > context.limits.max_width || info->height > context.limits.max_height ||
        !avifdec_size_multiply(info->width, info->height, &pixels) ||
        pixels > context.limits.max_pixels) {
        return avif_fail(&context, AVIFDEC_LIMIT_EXCEEDED, context.ipco.offset,
                         AVIFDEC_FOURCC('i', 's', 'p', 'e'));
    }
    status = avif_query_alpha_item(&context, primary_id, info);
    if (status != AVIFDEC_OK) return status;
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_query(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifdecImageInfo *info,
    AvifdecError *error) {
    return avifdec_query_ex(
        data, size, limits, 0, spans,
        span_capacity, info, error);
}

AvifdecStatus avifdec_trace_ex(const void *data,
                               size_t size,
                               const AvifdecLimits *limits,
                               const AvifdecExecutor *executor,
                               void *workspace,
                               size_t workspace_size,
                               AvifdecEntropyTrace *trace,
                               AvifdecError *error) {
    AvifdecImageInfo info;
    AvifContext context;
    uint32_t primary_id;
    AvifdecStatus status;

    if (trace == 0 || !avif_executor_valid(executor) ||
        (data == 0 && size != 0U) ||
        (workspace == 0 && workspace_size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = avifdec_query_ex(
        data, size, limits, executor, 0, 0U, &info, error);
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < info.workspace_required) return AVIFDEC_OUT_OF_MEMORY;
    status = avif_open_context(
        &context, data, size, limits, &primary_id, error);
    if (status != AVIFDEC_OK) return status;
    return avif_trace_item(
        &context, primary_id, 0U, executor,
        workspace, workspace_size, trace);
}

AvifdecStatus avifdec_trace(const void *data,
                            size_t size,
                            const AvifdecLimits *limits,
                            void *workspace,
                            size_t workspace_size,
                            AvifdecEntropyTrace *trace,
                            AvifdecError *error) {
    return avifdec_trace_ex(
        data, size, limits, 0, workspace,
        workspace_size, trace, error);
}

AvifdecStatus avifdec_decode_ex(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecError *error) {
    AvifdecImageInfo info;
    AvifContext context;
    uint32_t primary_id;
    AvifdecStatus status;

    if (image == 0 || !avif_executor_valid(executor) ||
        (data == 0 && size != 0U) ||
        (workspace == 0 && workspace_size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = avifdec_query_ex(
        data, size, limits, executor,
        0, 0U, &info, error);
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < info.workspace_required) return AVIFDEC_OUT_OF_MEMORY;
    status = avif_open_context(
        &context, data, size, limits, &primary_id, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_decode_item(
        &context, primary_id, 0U, executor,
        workspace, workspace_size,
        image, trace);
    if (status != AVIFDEC_OK || !info.has_alpha) return status;
    {
        AvifdecImageInfo alpha_info;
        AvifdecImage alpha_image;

        if (image->alpha_plane == 0 ||
            image->alpha_stride < info.width) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        if (primary_id != info.primary_item_id) {
            return avif_fail(&context, AVIFDEC_INVALID_DATA,
                             context.pitm.offset, context.pitm.type);
        }
        status = avif_query_item(
            &context, info.alpha_item_id, 0U, &alpha_info);
        if (status != AVIFDEC_OK) return status;
        avifdec_memory_fill(&alpha_image, 0U, sizeof(alpha_image));
        alpha_image.planes[0] = image->alpha_plane;
        alpha_image.strides[0] = image->alpha_stride;
        status = avif_decode_item(
            &context, info.alpha_item_id, 0U, 0,
            workspace, workspace_size, &alpha_image, 0);
        if (status != AVIFDEC_OK) return status;
        image->alpha_width = alpha_image.widths[0];
        image->alpha_height = alpha_image.heights[0];
        image->alpha_bit_depth = alpha_info.bit_depth;
        image->alpha_color_range = alpha_info.color_range;
        image->alpha_premultiplied = info.alpha_premultiplied;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_decode(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecError *error) {
    return avifdec_decode_ex(
        data, size, limits, 0, workspace,
        workspace_size, image, trace, error);
}