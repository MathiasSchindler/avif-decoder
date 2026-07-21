#include "avif_metadata_items.h"

#include "base.h"

typedef struct {
    AvifItemMetadataInfo *metadata;
    AvifItemThumbnailInfo *thumbnails;
    AvifdecSpan *spans;
    AvifItemMetadataResult result;
    int write;
} AvifMetadataEnumeration;

static void avif_metadata_clear_error(AvifdecError *error) {
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
}

static AvifdecStatus avif_metadata_fail(AvifdecError *error,
                                        AvifdecStatus status,
                                        size_t offset,
                                        uint32_t context) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = context;
    }
    return status;
}

static int avif_metadata_text_equal(const unsigned char *data,
                                    size_t size,
                                    const char *text) {
    size_t offset = 0U;

    while (text[offset] != '\0') {
        if (offset >= size ||
            data[offset] != (unsigned char)text[offset]) {
            return 0;
        }
        ++offset;
    }
    return offset == size;
}

static AvifdecStatus avif_metadata_payload_end_offset(
    const AvifItemIndex *index,
    const AvifItemIndexItem *item,
    const AvifItemPayload *payload,
    size_t *offset,
    AvifdecError *error) {
    AvifdecSpan last;
    AvifdecStatus status;

    *offset = item->source_offset;
    if (payload->span_count == 0U) return AVIFDEC_OK;
    status = avif_item_index_item_span_at(
        index, item->id, payload->span_count - 1U, &last, error);
    if (status != AVIFDEC_OK) return status;
    if (!avifdec_size_add(last.file_offset, last.size, offset)) {
        return avif_metadata_fail(
            error, AVIFDEC_OVERFLOW, last.file_offset, item->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_metadata_validate_exif(
    const AvifItemIndex *index,
    const AvifItemIndexItem *item,
    const AvifItemPayload *payload,
    size_t *tiff_offset,
    AvifItemTiffByteOrder *byte_order,
    AvifdecError *error) {
    unsigned char displacement_bytes[4];
    unsigned char byte_order_bytes[2];
    unsigned char magic_bytes[2];
    uint32_t displacement;
    size_t marker_file_offset;
    size_t magic_file_offset;
    size_t payload_end_offset;
    AvifdecStatus status;

    *tiff_offset = 0U;
    *byte_order = AVIF_ITEM_TIFF_BYTE_ORDER_NONE;
    if (payload->payload_size < 4U) {
        status = avif_metadata_payload_end_offset(
            index, item, payload, &payload_end_offset, error);
        if (status != AVIFDEC_OK) return status;
        return avif_metadata_fail(
            error, AVIFDEC_TRUNCATED, payload_end_offset, item->type);
    }
    status = avif_item_index_read_item(
        index, item->id, 0U, displacement_bytes,
        sizeof(displacement_bytes), 0, error);
    if (status != AVIFDEC_OK) return status;
    displacement = avifdec_load_u32be(displacement_bytes);
    if (!avifdec_size_add(4U, (size_t)displacement, tiff_offset)) {
        return avif_metadata_fail(
            error, AVIFDEC_OVERFLOW, item->source_offset, item->type);
    }
    if (*tiff_offset > payload->payload_size ||
        payload->payload_size - *tiff_offset < 4U) {
        status = avif_metadata_payload_end_offset(
            index, item, payload, &payload_end_offset, error);
        if (status != AVIFDEC_OK) return status;
        return avif_metadata_fail(
            error, AVIFDEC_TRUNCATED, payload_end_offset, item->type);
    }
    status = avif_item_index_read_item(
        index, item->id, *tiff_offset, byte_order_bytes,
        sizeof(byte_order_bytes), &marker_file_offset, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_item_index_read_item(
        index, item->id, *tiff_offset + 2U, magic_bytes,
        sizeof(magic_bytes), &magic_file_offset, error);
    if (status != AVIFDEC_OK) return status;
    if (byte_order_bytes[0] == (unsigned char)'I' &&
        byte_order_bytes[1] == (unsigned char)'I') {
        *byte_order = AVIF_ITEM_TIFF_BYTE_ORDER_LITTLE;
        if (magic_bytes[0] != 42U || magic_bytes[1] != 0U) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA,
                magic_file_offset, item->type);
        }
    } else if (byte_order_bytes[0] == (unsigned char)'M' &&
               byte_order_bytes[1] == (unsigned char)'M') {
        *byte_order = AVIF_ITEM_TIFF_BYTE_ORDER_BIG;
        if (magic_bytes[0] != 0U || magic_bytes[1] != 42U) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA,
                magic_file_offset, item->type);
        }
    } else {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_DATA,
            marker_file_offset, item->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_metadata_add_counts(
    const AvifItemIndex *index,
    AvifMetadataEnumeration *enumeration,
    size_t span_count,
    int thumbnail,
    size_t source_offset,
    uint32_t context,
    AvifdecError *error) {
    size_t row_count;
    size_t next_span_count;

    if (!avifdec_size_add(
            enumeration->result.metadata_count,
            enumeration->result.thumbnail_count, &row_count) ||
        row_count == SIZE_MAX) {
        return avif_metadata_fail(
            error, AVIFDEC_OVERFLOW, source_offset, context);
    }
    ++row_count;
    if (row_count > index->limits.max_metadata_items) {
        return avif_metadata_fail(
            error, AVIFDEC_LIMIT_EXCEEDED, source_offset, context);
    }
    if (!avifdec_size_add(
            enumeration->result.span_count, span_count,
            &next_span_count)) {
        return avif_metadata_fail(
            error, AVIFDEC_OVERFLOW, source_offset, context);
    }
    if (next_span_count > index->limits.max_metadata_spans) {
        return avif_metadata_fail(
            error, AVIFDEC_LIMIT_EXCEEDED, source_offset, context);
    }
    if (thumbnail) {
        ++enumeration->result.thumbnail_count;
    } else {
        ++enumeration->result.metadata_count;
    }
    enumeration->result.span_count = next_span_count;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_metadata_emit_metadata(
    const AvifItemIndex *index,
    const AvifItemIndexItem *item,
    const AvifItemPayload *payload,
    uint32_t target_item_id,
    size_t tiff_offset,
    AvifItemTiffByteOrder byte_order,
    AvifMetadataEnumeration *enumeration,
    AvifdecError *error) {
    size_t metadata_index = enumeration->result.metadata_count;
    size_t span_index = enumeration->result.span_count;
    AvifdecStatus status;

    status = avif_metadata_add_counts(
        index, enumeration, payload->span_count, 0,
        item->source_offset, item->type, error);
    if (status != AVIFDEC_OK) return status;
    if (enumeration->write) {
        AvifItemMetadataInfo info;
        AvifItemPayload filled_payload;

        avifdec_memory_fill(&info, 0U, sizeof(info));
        info.item_id = item->id;
        info.target_item_id = target_item_id;
        info.item_type = item->type;
        info.relationship_type = target_item_id == 0U
            ? 0U : AVIFDEC_FOURCC('c', 'd', 's', 'c');
        info.scope = target_item_id == 0U
            ? AVIF_ITEM_METADATA_SCOPE_UNSCOPED
            : AVIF_ITEM_METADATA_SCOPE_ITEM;
        info.payload_size = payload->payload_size;
        info.span_index = span_index;
        info.span_count = payload->span_count;
        info.item_name.data = item->name.data;
        info.item_name.size = item->name.size;
        info.content_type.data = item->content_type.data;
        info.content_type.size = item->content_type.size;
        info.content_encoding.data = item->content_encoding.data;
        info.content_encoding.size = item->content_encoding.size;
        if (item->type == AVIFDEC_FOURCC('E', 'x', 'i', 'f')) {
            info.kind = AVIF_ITEM_METADATA_EXIF;
            info.content_offset = 4U;
            info.exif_tiff_offset = tiff_offset;
            info.exif_byte_order = byte_order;
        } else if (avif_metadata_text_equal(
                       item->content_type.data,
                       item->content_type.size,
                       "application/rdf+xml")) {
            info.kind = AVIF_ITEM_METADATA_XMP;
            info.flags |= AVIF_ITEM_METADATA_FLAG_CANONICAL_XMP;
        } else {
            info.kind = AVIF_ITEM_METADATA_MIME;
        }
        status = avif_item_index_resolve_item(
            index, item->id, enumeration->spans + span_index,
            payload->span_count, &filled_payload, error);
        if (status != AVIFDEC_OK) return status;
        if (filled_payload.payload_size != payload->payload_size ||
            filled_payload.span_count != payload->span_count) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA,
                item->source_offset, item->type);
        }
        enumeration->metadata[metadata_index] = info;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_metadata_emit_thumbnail(
    const AvifItemIndex *index,
    const AvifItemIndexReference *reference,
    const AvifItemIndexItem *item,
    const AvifItemPayload *payload,
    uint32_t width,
    uint32_t height,
    uint32_t presentation_width,
    uint32_t presentation_height,
    AvifMetadataEnumeration *enumeration,
    AvifdecError *error) {
    size_t thumbnail_index = enumeration->result.thumbnail_count;
    size_t span_index = enumeration->result.span_count;
    AvifdecStatus status;

    status = avif_metadata_add_counts(
        index, enumeration, payload->span_count, 1,
        reference->source_offset, reference->type, error);
    if (status != AVIFDEC_OK) return status;
    if (enumeration->write) {
        AvifItemThumbnailInfo info;
        AvifItemPayload filled_payload;

        avifdec_memory_fill(&info, 0U, sizeof(info));
        info.thumbnail_item_id = item->id;
        info.target_item_id = reference->to_item_id;
        info.item_type = item->type;
        info.relationship_type = reference->type;
        info.width = width;
        info.height = height;
        info.presentation_width = presentation_width;
        info.presentation_height = presentation_height;
        info.payload_size = payload->payload_size;
        info.span_index = span_index;
        info.span_count = payload->span_count;
        status = avif_item_index_resolve_item(
            index, item->id, enumeration->spans + span_index,
            payload->span_count, &filled_payload, error);
        if (status != AVIFDEC_OK) return status;
        if (filled_payload.payload_size != payload->payload_size ||
            filled_payload.span_count != payload->span_count) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA,
                reference->source_offset, reference->type);
        }
        enumeration->thumbnails[thumbnail_index] = info;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_metadata_enumerate(
    const AvifItemIndex *index,
    AvifMetadataEnumeration *enumeration,
    AvifdecError *error) {
    size_t item_index;
    size_t reference_index;

    avifdec_memory_fill(&enumeration->result, 0U,
                        sizeof(enumeration->result));
    enumeration->result.primary_item_id = index->primary_item_id;
    for (item_index = 0U; item_index < index->item_count; ++item_index) {
        const AvifItemIndexItem *item = &index->items[item_index];
        AvifItemPayload payload;
        size_t tiff_offset = 0U;
        AvifItemTiffByteOrder byte_order =
            AVIF_ITEM_TIFF_BYTE_ORDER_NONE;
        size_t target_count = 0U;
        AvifdecStatus status;

        if (item->type != AVIFDEC_FOURCC('E', 'x', 'i', 'f') &&
            item->type != AVIFDEC_FOURCC('m', 'i', 'm', 'e')) {
            continue;
        }
        if (item->type == AVIFDEC_FOURCC('m', 'i', 'm', 'e') &&
            item->content_encoding.size != 0U) {
            size_t offset = item->source_offset;
            if (item->content_encoding.data >= index->data &&
                item->content_encoding.data <= index->data + index->size) {
                offset = (size_t)(
                    item->content_encoding.data - index->data);
            }
            return avif_metadata_fail(
                error, AVIFDEC_UNSUPPORTED, offset,
                AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
        }
        status = avif_item_index_validate_essential_properties(
            index, item->id, error);
        if (status != AVIFDEC_OK) return status;
        status = avif_item_index_resolve_item(
            index, item->id, 0, 0U, &payload, error);
        if (status != AVIFDEC_OK) return status;
        if (item->type == AVIFDEC_FOURCC('E', 'x', 'i', 'f')) {
            status = avif_metadata_validate_exif(
                index, item, &payload, &tiff_offset,
                &byte_order, error);
            if (status != AVIFDEC_OK) return status;
        }
        for (reference_index = 0U;
             reference_index < index->reference_count;
             ++reference_index) {
            const AvifItemIndexReference *reference =
                &index->references[reference_index];

            if (reference->type !=
                    AVIFDEC_FOURCC('c', 'd', 's', 'c') ||
                reference->from_item_id != item->id) {
                continue;
            }
            status = avif_metadata_emit_metadata(
                index, item, &payload, reference->to_item_id,
                tiff_offset, byte_order, enumeration, error);
            if (status != AVIFDEC_OK) return status;
            ++target_count;
        }
        if (target_count == 0U) {
            status = avif_metadata_emit_metadata(
                index, item, &payload, 0U, tiff_offset,
                byte_order, enumeration, error);
            if (status != AVIFDEC_OK) return status;
        }
    }
    for (reference_index = 0U;
         reference_index < index->reference_count;
         ++reference_index) {
        const AvifItemIndexReference *reference =
            &index->references[reference_index];
        const AvifItemIndexItem *item;
        AvifItemPayload payload;
        uint32_t width;
        uint32_t height;
        uint32_t presentation_width;
        uint32_t presentation_height;
        AvifdecStatus status;

        if (reference->type !=
            AVIFDEC_FOURCC('t', 'h', 'm', 'b')) {
            continue;
        }
        item = avif_item_index_find_item(
            index, reference->from_item_id);
        if (item == 0) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA,
                reference->source_offset, reference->type);
        }
        status = avif_item_index_resolve_item(
            index, item->id, 0, 0U, &payload, error);
        if (status != AVIFDEC_OK) return status;
        status = avif_item_index_item_dimensions(
            index, item->id, &width, &height,
            &presentation_width, &presentation_height, error);
        if (status != AVIFDEC_OK) return status;
        status = avif_metadata_emit_thumbnail(
            index, reference, item, &payload,
            width, height, presentation_width,
            presentation_height, enumeration, error);
        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_metadata_validate_arguments(
    AvifItemMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifItemThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemMetadataResult *result,
    AvifdecError *error) {
    if (result != 0) {
        avifdec_memory_fill(result, 0U, sizeof(*result));
    }
    avif_metadata_clear_error(error);
    if (result == 0 ||
        (metadata == 0 && metadata_capacity != 0U) ||
        (thumbnails == 0 && thumbnail_capacity != 0U) ||
        (spans == 0 && span_capacity != 0U)) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_metadata_items_query_index(
    const AvifItemIndex *index,
    AvifItemMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifItemThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemMetadataResult *result,
    AvifdecError *error) {
    AvifMetadataEnumeration count;
    AvifMetadataEnumeration fill;
    AvifdecStatus status;

    status = avif_metadata_validate_arguments(
        metadata, metadata_capacity, thumbnails, thumbnail_capacity,
        spans, span_capacity, result, error);
    if (status != AVIFDEC_OK) return status;
    if (index == 0) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avifdec_memory_fill(&count, 0U, sizeof(count));
    status = avif_metadata_enumerate(index, &count, error);
    if (status != AVIFDEC_OK) return status;
    *result = count.result;
    if (metadata == 0 && metadata_capacity == 0U &&
        thumbnails == 0 && thumbnail_capacity == 0U &&
        spans == 0 && span_capacity == 0U) {
        return AVIFDEC_OK;
    }
    if (metadata_capacity < result->metadata_count ||
        thumbnail_capacity < result->thumbnail_count ||
        span_capacity < result->span_count ||
        (result->metadata_count != 0U && metadata == 0) ||
        (result->thumbnail_count != 0U && thumbnails == 0) ||
        (result->span_count != 0U && spans == 0)) {
        return avif_metadata_fail(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    avifdec_memory_fill(&fill, 0U, sizeof(fill));
    fill.metadata = metadata;
    fill.thumbnails = thumbnails;
    fill.spans = spans;
    fill.write = 1;
    status = avif_metadata_enumerate(index, &fill, error);
    if (status != AVIFDEC_OK) return status;
    if (fill.result.primary_item_id != result->primary_item_id ||
        fill.result.metadata_count != result->metadata_count ||
        fill.result.thumbnail_count != result->thumbnail_count ||
        fill.result.span_count != result->span_count) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_metadata_items_query(
    const void *data,
    size_t size,
    const AvifItemIndexLimits *limits,
    AvifItemMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifItemThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemMetadataResult *result,
    AvifdecError *error) {
    AvifItemIndex index;
    AvifdecStatus status;

    status = avif_metadata_validate_arguments(
        metadata, metadata_capacity, thumbnails, thumbnail_capacity,
        spans, span_capacity, result, error);
    if (status != AVIFDEC_OK) return status;
    if (data == 0 && size != 0U) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_item_index_build(
        data, size, limits, &index, error);
    if (status != AVIFDEC_OK) return status;
    return avif_metadata_items_query_index(
        &index, metadata, metadata_capacity,
        thumbnails, thumbnail_capacity,
        spans, span_capacity, result, error);
}

AvifdecStatus avif_metadata_items_query_meta(
    const void *data,
    size_t size,
    const AvifItemIndexLimits *limits,
    size_t meta_offset,
    AvifItemMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifItemThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemMetadataResult *result,
    AvifdecError *error) {
    AvifItemIndex index;
    AvifdecStatus status;

    status = avif_metadata_validate_arguments(
        metadata, metadata_capacity, thumbnails, thumbnail_capacity,
        spans, span_capacity, result, error);
    if (status != AVIFDEC_OK) return status;
    if (data == 0 && size != 0U) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_item_index_build_meta(
        data, size, limits, meta_offset, &index, error);
    if (status != AVIFDEC_OK) return status;
    return avif_metadata_items_query_index(
        &index, metadata, metadata_capacity,
        thumbnails, thumbnail_capacity,
        spans, span_capacity, result, error);
}

AvifdecStatus avifdec_metadata_query(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    AvifdecMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifdecThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifdecMetadataResult *result,
    AvifdecError *error) {
    AvifItemIndexLimits item_limits;

    avif_item_index_limits_from_public(limits, &item_limits);
    return avif_metadata_items_query(
        data, size, &item_limits, metadata, metadata_capacity,
        thumbnails, thumbnail_capacity, spans, span_capacity,
        result, error);
}

static AvifItemIndexLimits avif_metadata_effective_limits(
    const AvifItemIndexLimits *limits) {
    AvifItemIndexLimits result;

    avif_item_index_default_limits(&result);
    if (limits == 0) return result;
    if (limits->max_items != 0U) result.max_items = limits->max_items;
    if (limits->max_extents != 0U) result.max_extents = limits->max_extents;
    if (limits->max_properties != 0U) {
        result.max_properties = limits->max_properties;
    }
    if (limits->max_metadata_items != 0U) {
        result.max_metadata_items = limits->max_metadata_items;
    }
    if (limits->max_metadata_spans != 0U) {
        result.max_metadata_spans = limits->max_metadata_spans;
    }
    if (limits->max_references != 0U) {
        result.max_references = limits->max_references;
    }
    if (limits->max_associations != 0U) {
        result.max_associations = limits->max_associations;
    }
    if (limits->max_data_boxes != 0U) {
        result.max_data_boxes = limits->max_data_boxes;
    }
    if (limits->max_width != 0U) result.max_width = limits->max_width;
    if (limits->max_height != 0U) result.max_height = limits->max_height;
    if (limits->max_pixels != 0U) result.max_pixels = limits->max_pixels;
    return result;
}

static AvifdecStatus avif_metadata_validate_view_row(
    size_t span_index,
    size_t span_count,
    size_t payload_size,
    const AvifItemTrackMetadataView *view,
    AvifdecError *error) {
    size_t span_end;
    size_t computed_size = 0U;
    size_t index;

    if (!avifdec_size_add(span_index, span_count, &span_end)) {
        return avif_metadata_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    if (span_end > view->span_count) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    for (index = span_index; index < span_end; ++index) {
        const AvifdecSpan *span = &view->spans[index];

        if (span->data == 0 && span->size != 0U) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA,
                span->file_offset, 0U);
        }
        if (!avifdec_size_add(
                computed_size, span->size, &computed_size)) {
            return avif_metadata_fail(
                error, AVIFDEC_OVERFLOW,
                span->file_offset, 0U);
        }
    }
    if (computed_size != payload_size) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_metadata_items_query_track_source(
    const AvifItemTrackMetadataSource *source,
    uint32_t track_id,
    const AvifItemIndexLimits *limits,
    AvifItemMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifItemThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemMetadataResult *result,
    AvifdecError *error) {
    AvifItemTrackMetadataView view;
    AvifItemIndexLimits effective_limits;
    size_t row_count;
    size_t index;
    AvifdecStatus status;

    status = avif_metadata_validate_arguments(
        metadata, metadata_capacity, thumbnails, thumbnail_capacity,
        spans, span_capacity, result, error);
    if (status != AVIFDEC_OK) return status;
    if (source == 0 || source->view == 0) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avifdec_memory_fill(&view, 0U, sizeof(view));
    status = source->view(source->context, track_id, &view, error);
    if (status != AVIFDEC_OK) {
        if (error != 0 && error->status == AVIFDEC_OK) {
            (void)avif_metadata_fail(error, status, 0U, 0U);
        }
        return status;
    }
    if ((view.metadata == 0 && view.metadata_count != 0U) ||
        (view.thumbnails == 0 && view.thumbnail_count != 0U) ||
        (view.spans == 0 && view.span_count != 0U)) {
        return avif_metadata_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    effective_limits = avif_metadata_effective_limits(limits);
    if (!avifdec_size_add(
            view.metadata_count, view.thumbnail_count, &row_count)) {
        return avif_metadata_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    if (row_count > effective_limits.max_metadata_items ||
        view.span_count > effective_limits.max_metadata_spans) {
        return avif_metadata_fail(
            error, AVIFDEC_LIMIT_EXCEEDED, 0U, 0U);
    }
    for (index = 0U; index < view.metadata_count; ++index) {
        const AvifItemMetadataInfo *info = &view.metadata[index];

        if ((info->flags &
             AVIF_ITEM_METADATA_FLAG_SEQUENCE_WIDE) == 0U ||
            (info->scope != AVIF_ITEM_METADATA_SCOPE_TRACK &&
             info->scope != AVIF_ITEM_METADATA_SCOPE_UNSCOPED) ||
            (track_id != 0U &&
             (info->scope != AVIF_ITEM_METADATA_SCOPE_TRACK ||
              info->target_track_id != track_id))) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        status = avif_metadata_validate_view_row(
            info->span_index, info->span_count,
            info->payload_size, &view, error);
        if (status != AVIFDEC_OK) return status;
    }
    for (index = 0U; index < view.thumbnail_count; ++index) {
        const AvifItemThumbnailInfo *info = &view.thumbnails[index];

        if ((info->flags &
             AVIF_ITEM_METADATA_FLAG_SEQUENCE_WIDE) == 0U ||
            (track_id != 0U &&
             info->target_track_id != track_id)) {
            return avif_metadata_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        status = avif_metadata_validate_view_row(
            info->span_index, info->span_count,
            info->payload_size, &view, error);
        if (status != AVIFDEC_OK) return status;
    }
    result->metadata_count = view.metadata_count;
    result->thumbnail_count = view.thumbnail_count;
    result->span_count = view.span_count;
    if (metadata == 0 && metadata_capacity == 0U &&
        thumbnails == 0 && thumbnail_capacity == 0U &&
        spans == 0 && span_capacity == 0U) {
        return AVIFDEC_OK;
    }
    if (metadata_capacity < result->metadata_count ||
        thumbnail_capacity < result->thumbnail_count ||
        span_capacity < result->span_count ||
        (result->metadata_count != 0U && metadata == 0) ||
        (result->thumbnail_count != 0U && thumbnails == 0) ||
        (result->span_count != 0U && spans == 0)) {
        return avif_metadata_fail(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    for (index = 0U; index < view.metadata_count; ++index) {
        metadata[index] = view.metadata[index];
    }
    for (index = 0U; index < view.thumbnail_count; ++index) {
        thumbnails[index] = view.thumbnails[index];
    }
    for (index = 0U; index < view.span_count; ++index) {
        spans[index] = view.spans[index];
    }
    return AVIFDEC_OK;
}
