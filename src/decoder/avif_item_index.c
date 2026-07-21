#include "avif_item_index.h"

#include "base.h"

typedef struct {
    AvifdecSpan *spans;
    size_t span_capacity;
    size_t span_count;
    size_t payload_size;
    unsigned char *copy_output;
    size_t copy_offset;
    size_t copy_size;
    size_t copied;
    size_t first_file_offset;
    size_t requested_span_index;
    AvifdecSpan *requested_span;
    int write_spans;
    int copy_bytes;
} AvifItemWalk;

static void avif_item_clear_error(AvifdecError *error) {
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
}

static AvifdecStatus avif_item_set_error(AvifdecError *error,
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

static AvifdecStatus avif_item_fail(AvifItemIndex *index,
                                    AvifdecStatus status,
                                    size_t offset,
                                    uint32_t context) {
    if (index->failure_status == AVIFDEC_OK) {
        index->failure_status = status;
        (void)avif_item_set_error(index->error, status, offset, context);
    }
    return status;
}

static int avif_item_box_is_set(const AvifdecBmffBox *box) {
    return box->size != 0U;
}

static int avif_item_direct_child(const AvifdecBmffBox *child,
                                  const AvifdecBmffBox *parent) {
    size_t parent_end;
    size_t child_end;

    if (!avif_item_box_is_set(parent) ||
        child->depth != parent->depth + 1U ||
        !avifdec_size_add(parent->offset, parent->size, &parent_end) ||
        !avifdec_size_add(child->offset, child->size, &child_end)) {
        return 0;
    }
    return child->offset >= parent->payload_offset && child_end <= parent_end;
}

static uint32_t avif_item_full_box_flags(const unsigned char *payload) {
    return ((uint32_t)payload[1] << 16) |
           ((uint32_t)payload[2] << 8) |
           (uint32_t)payload[3];
}

static AvifdecStatus avif_item_full_box(const AvifItemIndex *index,
                                        const AvifdecBmffBox *box,
                                        uint8_t *version,
                                        uint32_t *flags,
                                        AvifdecError *error) {
    const unsigned char *payload;

    if (box->payload_size < 4U) {
        return avif_item_set_error(error, AVIFDEC_TRUNCATED,
                                   box->payload_offset, box->type);
    }
    payload = index->data + box->payload_offset;
    *version = payload[0];
    *flags = avif_item_full_box_flags(payload);
    return AVIFDEC_OK;
}

void avif_item_index_default_limits(AvifItemIndexLimits *limits) {
    if (limits == 0) return;
    limits->max_items = AVIFDEC_DEFAULT_MAX_ITEMS;
    limits->max_extents = AVIFDEC_DEFAULT_MAX_EXTENTS;
    limits->max_properties = AVIFDEC_DEFAULT_MAX_PROPERTIES;
    limits->max_metadata_items =
        AVIF_ITEM_INDEX_DEFAULT_MAX_METADATA_ITEMS;
    limits->max_metadata_spans =
        AVIF_ITEM_INDEX_DEFAULT_MAX_METADATA_SPANS;
    limits->max_references = AVIF_ITEM_INDEX_MAX_REFERENCES;
    limits->max_associations = AVIF_ITEM_INDEX_MAX_ASSOCIATIONS;
    limits->max_data_boxes = AVIF_ITEM_INDEX_MAX_DATA_BOXES;
    limits->max_entity_groups = AVIF_ITEM_INDEX_MAX_ENTITY_GROUPS;
    limits->max_group_entities = AVIF_ITEM_INDEX_MAX_GROUP_ENTITIES;
    limits->max_width = 32768U;
    limits->max_height = 32768U;
    limits->max_pixels = 268435456U;
}

void avif_item_index_limits_from_public(
    const AvifdecLimits *limits,
    AvifItemIndexLimits *item_limits) {
    AvifdecLimits effective;

    if (item_limits == 0) return;
    avif_item_index_default_limits(item_limits);
    effective = avifdec_limits_effective(limits);
    item_limits->max_items = effective.max_items;
    item_limits->max_extents = effective.max_extents;
    item_limits->max_properties = effective.max_properties;
    item_limits->max_metadata_items = effective.max_metadata_items;
    item_limits->max_metadata_spans = effective.max_metadata_spans;
    item_limits->max_width = effective.max_width;
    item_limits->max_height = effective.max_height;
    item_limits->max_pixels = effective.max_pixels;
    item_limits->max_entity_groups = effective.max_items;
    item_limits->max_group_entities =
        effective.max_items <= SIZE_MAX / 4U
            ? effective.max_items * 4U
            : SIZE_MAX;
}

static AvifItemIndexLimits avif_item_effective_limits(
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
    if (limits->max_entity_groups != 0U) {
        result.max_entity_groups = limits->max_entity_groups;
    }
    if (limits->max_group_entities != 0U) {
        result.max_group_entities = limits->max_group_entities;
    }
    if (limits->max_width != 0U) result.max_width = limits->max_width;
    if (limits->max_height != 0U) result.max_height = limits->max_height;
    if (limits->max_pixels != 0U) result.max_pixels = limits->max_pixels;
    return result;
}

static AvifdecStatus avif_item_fixed_limit(AvifItemIndex *index,
                                           size_t count,
                                           size_t caller_limit,
                                           size_t implementation_limit,
                                           size_t offset,
                                           uint32_t context) {
    if (count > caller_limit) {
        return avif_item_fail(index, AVIFDEC_LIMIT_EXCEEDED,
                              offset, context);
    }
    if (count > implementation_limit) {
        return avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                              offset, context);
    }
    return AVIFDEC_OK;
}

static int avif_item_utf8_valid(const unsigned char *data,
                                size_t size,
                                size_t *bad_offset) {
    size_t offset = 0U;

    while (offset < size) {
        unsigned char first = data[offset];
        size_t length;
        size_t index;

        if (first < 0x80U) {
            ++offset;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4U;
        } else {
            *bad_offset = offset;
            return 0;
        }
        if (length > size - offset) {
            *bad_offset = offset;
            return 0;
        }
        for (index = 1U; index < length; ++index) {
            if ((data[offset + index] & 0xc0U) != 0x80U) {
                *bad_offset = offset + index;
                return 0;
            }
        }
        if ((first == 0xe0U && data[offset + 1U] < 0xa0U) ||
            (first == 0xedU && data[offset + 1U] >= 0xa0U) ||
            (first == 0xf0U && data[offset + 1U] < 0x90U) ||
            (first == 0xf4U && data[offset + 1U] >= 0x90U)) {
            *bad_offset = offset;
            return 0;
        }
        offset += length;
    }
    return 1;
}

static AvifdecStatus avif_item_read_string(AvifItemIndex *index,
                                            AvifdecByteReader *reader,
                                            AvifItemByteView *view,
                                            uint32_t context) {
    size_t start = reader->position;
    size_t end = start;
    size_t bad_offset;

    while (end < reader->size && reader->data[end] != 0U) ++end;
    if (end == reader->size) {
        return avif_item_fail(index, AVIFDEC_TRUNCATED,
                              reader->base_offset + end, context);
    }
    if (!avif_item_utf8_valid(reader->data + start, end - start,
                              &bad_offset)) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              reader->base_offset + start + bad_offset,
                              context);
    }
    view->data = reader->data + start;
    view->size = end - start;
    reader->position = end + 1U;
    return AVIFDEC_OK;
}

static int avif_item_id_exists(const AvifItemIndex *index,
                               uint32_t item_id) {
    size_t item_index;

    for (item_index = 0U; item_index < index->item_count; ++item_index) {
        if (index->items[item_index].id == item_id) return 1;
    }
    return 0;
}

static AvifdecStatus avif_item_parse_infe(AvifItemIndex *index,
                                          const AvifdecBmffBox *box) {
    AvifdecByteReader reader;
    AvifItemIndexItem item;
    uint8_t version;
    uint32_t flags;
    AvifdecStatus status;

    status = avif_item_full_box(index, box, &version, &flags, index->error);
    if (status != AVIFDEC_OK) {
        return avif_item_fail(index, status,
                              index->error == 0 ? box->payload_offset
                                                : index->error->offset,
                              box->type);
    }
    if (version != 2U && version != 3U) {
        return avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                              box->payload_offset, box->type);
    }
    if ((flags & ~AVIF_ITEM_INDEX_ITEM_FLAG_HIDDEN) != 0U) {
        size_t flags_offset = box->payload_offset + 1U;

        if (index->data[flags_offset] == 0U) {
            ++flags_offset;
            if (index->data[flags_offset] == 0U) ++flags_offset;
        }
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              flags_offset, box->type);
    }
    avifdec_memory_fill(&item, 0U, sizeof(item));
    item.source_offset = box->offset;
    item.flags = flags;
    avifdec_byte_reader_init(
        &reader, index->data + box->payload_offset + 4U,
        box->payload_size - 4U, box->payload_offset + 4U);
    item.id = version == 2U
        ? (uint32_t)avifdec_byte_reader_u16be(&reader)
        : avifdec_byte_reader_u32be(&reader);
    item.protection_offset = avifdec_byte_reader_offset(&reader);
    item.protection_index = avifdec_byte_reader_u16be(&reader);
    item.type = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avif_item_fail(index, reader.status,
                              avifdec_byte_reader_offset(&reader),
                              box->type);
    }
    if (item.id == 0U || avif_item_id_exists(index, item.id)) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              box->payload_offset + 4U, box->type);
    }
    status = avif_item_read_string(index, &reader, &item.name, box->type);
    if (status != AVIFDEC_OK) return status;
    if (item.type == AVIFDEC_FOURCC('m', 'i', 'm', 'e')) {
        status = avif_item_read_string(
            index, &reader, &item.content_type, box->type);
        if (status != AVIFDEC_OK) return status;
        if (avifdec_byte_reader_remaining(&reader) != 0U) {
            status = avif_item_read_string(
                index, &reader, &item.content_encoding, box->type);
            if (status != AVIFDEC_OK) return status;
        } else {
            item.content_encoding.data = reader.data + reader.position;
        }
        if (avifdec_byte_reader_remaining(&reader) != 0U) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  avifdec_byte_reader_offset(&reader),
                                  box->type);
        }
    } else if (item.type == AVIFDEC_FOURCC('u', 'r', 'i', ' ')) {
        status = avif_item_read_string(
            index, &reader, &item.item_uri_type, box->type);
        if (status != AVIFDEC_OK) return status;
        if (avifdec_byte_reader_remaining(&reader) != 0U) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  avifdec_byte_reader_offset(&reader),
                                  box->type);
        }
    } else if (avifdec_byte_reader_remaining(&reader) != 0U) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              avifdec_byte_reader_offset(&reader),
                              box->type);
    }
    status = avif_item_fixed_limit(
        index, index->item_count + 1U, index->limits.max_items,
        AVIF_ITEM_INDEX_MAX_ITEMS, box->offset, box->type);
    if (status != AVIFDEC_OK) return status;
    index->items[index->item_count++] = item;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_add_reference(
    AvifItemIndex *index,
    uint32_t from_item_id,
    uint32_t to_item_id,
    uint32_t type,
    size_t source_offset) {
    size_t reference_index;
    AvifdecStatus status;

    if (from_item_id == to_item_id) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              source_offset, type);
    }
    for (reference_index = 0U;
         reference_index < index->reference_count;
         ++reference_index) {
        const AvifItemIndexReference *reference =
            &index->references[reference_index];

        if (reference->from_item_id == from_item_id &&
            reference->to_item_id == to_item_id &&
            reference->type == type) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  source_offset, type);
        }
    }
    status = avif_item_fixed_limit(
        index, index->reference_count + 1U,
        index->limits.max_references,
        AVIF_ITEM_INDEX_MAX_REFERENCES, source_offset, type);
    if (status != AVIFDEC_OK) return status;
    index->references[index->reference_count].from_item_id = from_item_id;
    index->references[index->reference_count].to_item_id = to_item_id;
    index->references[index->reference_count].type = type;
    index->references[index->reference_count].source_offset = source_offset;
    ++index->reference_count;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_parse_reference_box(
    AvifItemIndex *index,
    const AvifdecBmffBox *box) {
    AvifdecByteReader reader;
    uint8_t version;
    uint32_t flags;
    uint32_t from_item_id;
    uint16_t reference_count;
    uint16_t reference_index;
    AvifdecStatus status;

    status = avif_item_full_box(
        index, &index->iref, &version, &flags, index->error);
    if (status != AVIFDEC_OK) {
        return avif_item_fail(index, status, index->iref.payload_offset,
                              index->iref.type);
    }
    if (version > 1U) {
        return avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                              index->iref.payload_offset,
                              index->iref.type);
    }
    if (flags != 0U) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              index->iref.payload_offset + 1U,
                              index->iref.type);
    }
    avifdec_byte_reader_init(
        &reader, index->data + box->payload_offset,
        box->payload_size, box->payload_offset);
    from_item_id = version == 0U
        ? (uint32_t)avifdec_byte_reader_u16be(&reader)
        : avifdec_byte_reader_u32be(&reader);
    reference_count = avifdec_byte_reader_u16be(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avif_item_fail(index, reader.status,
                              avifdec_byte_reader_offset(&reader),
                              box->type);
    }
    if (reference_count == 0U) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              box->payload_offset, box->type);
    }
    for (reference_index = 0U;
         reference_index < reference_count;
         ++reference_index) {
        size_t source_offset = avifdec_byte_reader_offset(&reader);
        uint32_t to_item_id = version == 0U
            ? (uint32_t)avifdec_byte_reader_u16be(&reader)
            : avifdec_byte_reader_u32be(&reader);

        if (reader.status != AVIFDEC_OK) break;
        status = avif_item_add_reference(
            index, from_item_id, to_item_id, box->type, source_offset);
        if (status != AVIFDEC_OK) return status;
    }
    if (reader.status != AVIFDEC_OK) {
        return avif_item_fail(index, reader.status,
                              avifdec_byte_reader_offset(&reader),
                              box->type);
    }
    if (avifdec_byte_reader_remaining(&reader) != 0U) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              avifdec_byte_reader_offset(&reader),
                              box->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_parse_entity_group(
    AvifItemIndex *index,
    const AvifdecBmffBox *box) {
    AvifdecByteReader reader;
    AvifItemIndexEntityGroup group;
    uint8_t version;
    uint32_t flags;
    uint32_t entity_count;
    uint32_t entity_index;
    size_t next_entity_count;
    size_t existing_group;
    AvifdecStatus status;

    if (box->type != AVIFDEC_FOURCC('a', 'l', 't', 'r')) {
        return AVIFDEC_OK;
    }
    status = avif_item_full_box(
        index, box, &version, &flags, index->error);
    if (status != AVIFDEC_OK) return status;
    if (version != 0U) {
        return avif_item_fail(
            index, AVIFDEC_UNSUPPORTED, box->payload_offset, box->type);
    }
    if (flags != 0U) {
        return avif_item_fail(
            index, AVIFDEC_INVALID_DATA,
            box->payload_offset + 1U, box->type);
    }
    if (box->payload_size < 12U) {
        return avif_item_fail(
            index, AVIFDEC_TRUNCATED, box->payload_offset, box->type);
    }
    avifdec_byte_reader_init(
        &reader, index->data + box->payload_offset + 4U,
        box->payload_size - 4U, box->payload_offset + 4U);
    avifdec_memory_fill(&group, 0U, sizeof(group));
    group.type = box->type;
    group.group_id = avifdec_byte_reader_u32be(&reader);
    entity_count = avifdec_byte_reader_u32be(&reader);
    group.entity_index = index->group_entity_count;
    group.entity_count = entity_count;
    group.source_offset = box->offset;
    if (reader.status != AVIFDEC_OK) {
        return avif_item_fail(
            index, reader.status,
            avifdec_byte_reader_offset(&reader), box->type);
    }
    if (entity_count < 2U) {
        return avif_item_fail(
            index, AVIFDEC_INVALID_DATA,
            box->payload_offset + 8U, box->type);
    }
    if ((size_t)entity_count >
        avifdec_byte_reader_remaining(&reader) / 4U) {
        return avif_item_fail(
            index, AVIFDEC_TRUNCATED,
            avifdec_byte_reader_offset(&reader), box->type);
    }
    status = avif_item_fixed_limit(
        index, index->entity_group_count + 1U,
        index->limits.max_entity_groups,
        AVIF_ITEM_INDEX_MAX_ENTITY_GROUPS,
        box->offset, box->type);
    if (status != AVIFDEC_OK) return status;
    if (!avifdec_size_add(
            index->group_entity_count, entity_count,
            &next_entity_count)) {
        return avif_item_fail(
            index, AVIFDEC_OVERFLOW,
            box->payload_offset + 8U, box->type);
    }
    status = avif_item_fixed_limit(
        index, next_entity_count,
        index->limits.max_group_entities,
        AVIF_ITEM_INDEX_MAX_GROUP_ENTITIES,
        box->offset, box->type);
    if (status != AVIFDEC_OK) return status;
    for (existing_group = 0U;
         existing_group < index->entity_group_count;
         ++existing_group) {
        if (index->entity_groups[existing_group].group_id ==
            group.group_id) {
            return avif_item_fail(
                index, AVIFDEC_INVALID_DATA,
                box->payload_offset + 4U, box->type);
        }
    }
    for (entity_index = 0U;
         entity_index < entity_count;
         ++entity_index) {
        AvifItemIndexGroupEntity entity;
        size_t existing_entity;

        entity.source_offset = avifdec_byte_reader_offset(&reader);
        entity.entity_id = avifdec_byte_reader_u32be(&reader);
        if (reader.status != AVIFDEC_OK) {
            return avif_item_fail(
                index, reader.status,
                entity.source_offset, box->type);
        }
        if (entity.entity_id == 0U) {
            return avif_item_fail(
                index, AVIFDEC_INVALID_DATA,
                entity.source_offset, box->type);
        }
        for (existing_entity = 0U;
             existing_entity < index->group_entity_count;
             ++existing_entity) {
            if (index->group_entities[existing_entity].entity_id ==
                entity.entity_id) {
                return avif_item_fail(
                    index, AVIFDEC_INVALID_DATA,
                    entity.source_offset, box->type);
            }
        }
        index->group_entities[index->group_entity_count++] = entity;
    }
    if (avifdec_byte_reader_remaining(&reader) != 0U) {
        return avif_item_fail(
            index, AVIFDEC_INVALID_DATA,
            avifdec_byte_reader_offset(&reader), box->type);
    }
    index->entity_groups[index->entity_group_count++] = group;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_set_unique_box(
    AvifItemIndex *index,
    AvifdecBmffBox *destination,
    const AvifdecBmffBox *box) {
    if (avif_item_box_is_set(destination)) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              box->offset, box->type);
    }
    *destination = *box;
    return AVIFDEC_OK;
}

static void avif_item_collect_box(const AvifdecBmffBox *box,
                                  void *user_data) {
    AvifItemIndex *index = (AvifItemIndex *)user_data;
    uint32_t type = box->type;
    AvifdecStatus status = AVIFDEC_OK;

    if (index->failure_status != AVIFDEC_OK) return;
    if (type == AVIFDEC_FOURCC('m', 'e', 't', 'a') &&
        ((index->selected_meta_offset == SIZE_MAX &&
          box->depth == 0U) ||
         box->offset == index->selected_meta_offset)) {
        status = avif_item_set_unique_box(index, &index->meta, box);
    } else if (avif_item_direct_child(box, &index->meta)) {
        if (type == AVIFDEC_FOURCC('h', 'd', 'l', 'r')) {
            status = avif_item_set_unique_box(index, &index->handler, box);
        } else if (type == AVIFDEC_FOURCC('p', 'i', 't', 'm')) {
            status = avif_item_set_unique_box(index, &index->pitm, box);
        } else if (type == AVIFDEC_FOURCC('i', 'l', 'o', 'c')) {
            status = avif_item_set_unique_box(index, &index->iloc, box);
        } else if (type == AVIFDEC_FOURCC('i', 'i', 'n', 'f')) {
            status = avif_item_set_unique_box(index, &index->iinf, box);
        } else if (type == AVIFDEC_FOURCC('i', 'r', 'e', 'f')) {
            status = avif_item_set_unique_box(index, &index->iref, box);
        } else if (type == AVIFDEC_FOURCC('i', 'p', 'r', 'p')) {
            status = avif_item_set_unique_box(index, &index->iprp, box);
        } else if (type == AVIFDEC_FOURCC('g', 'r', 'p', 'l')) {
            status = avif_item_set_unique_box(index, &index->grpl, box);
        } else if (type == AVIFDEC_FOURCC('i', 'd', 'a', 't')) {
            status = avif_item_set_unique_box(index, &index->idat, box);
            if (status == AVIFDEC_OK) {
                status = avif_item_fixed_limit(
                    index, index->data_box_count + 1U,
                    index->limits.max_data_boxes,
                    AVIF_ITEM_INDEX_MAX_DATA_BOXES,
                    box->offset, type);
                if (status == AVIFDEC_OK) {
                    index->data_boxes[index->data_box_count++] = *box;
                }
            }
        }
    } else if (box->depth == 0U &&
               type == AVIFDEC_FOURCC('m', 'd', 'a', 't')) {
        status = avif_item_fixed_limit(
            index, index->data_box_count + 1U,
            index->limits.max_data_boxes,
            AVIF_ITEM_INDEX_MAX_DATA_BOXES, box->offset, type);
        if (status == AVIFDEC_OK) {
            index->data_boxes[index->data_box_count++] = *box;
        }
    } else if (type == AVIFDEC_FOURCC('i', 'p', 'c', 'o') &&
               avif_item_direct_child(box, &index->iprp)) {
        status = avif_item_set_unique_box(index, &index->ipco, box);
    } else if (type == AVIFDEC_FOURCC('i', 'p', 'm', 'a') &&
               avif_item_direct_child(box, &index->iprp)) {
        if (index->ipma_count >= AVIF_ITEM_INDEX_MAX_IPMA_BOXES) {
            status = avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                                    box->offset, type);
        } else {
            index->ipma[index->ipma_count++] = *box;
        }
    } else if (avif_item_direct_child(box, &index->grpl)) {
        status = avif_item_parse_entity_group(index, box);
    } else if (type == AVIFDEC_FOURCC('i', 'n', 'f', 'e') &&
               avif_item_direct_child(box, &index->iinf)) {
        status = avif_item_parse_infe(index, box);
    } else if (avif_item_direct_child(box, &index->iref)) {
        status = avif_item_parse_reference_box(index, box);
    } else if (avif_item_direct_child(box, &index->ipco)) {
        status = avif_item_fixed_limit(
            index, index->property_count + 1U,
            index->limits.max_properties,
            AVIF_ITEM_INDEX_MAX_PROPERTIES, box->offset, type);
        if (status == AVIFDEC_OK) {
            index->properties[index->property_count].box = *box;
            index->properties[index->property_count].type = type;
            ++index->property_count;
        }
    }
    (void)status;
}

static AvifdecStatus avif_item_read_sized_uint(
    AvifdecByteReader *reader,
    unsigned int byte_count,
    uint64_t *value) {
    const unsigned char *bytes;
    unsigned int index;

    if (byte_count > 8U) return AVIFDEC_UNSUPPORTED;
    bytes = avifdec_byte_reader_take(reader, byte_count);
    if (bytes == 0 && byte_count != 0U) return reader->status;
    *value = 0U;
    for (index = 0U; index < byte_count; ++index) {
        *value = (*value << 8) | bytes[index];
    }
    return AVIFDEC_OK;
}

static int avif_item_valid_iloc_field_size(unsigned int size) {
    return size == 0U || size == 4U || size == 8U;
}

static AvifdecStatus avif_item_scan_locations(
    const AvifItemIndex *constant_index,
    uint32_t requested_item_id,
    AvifItemIndexLocation *requested_location,
    AvifdecError *error) {
    AvifItemIndex *index = (AvifItemIndex *)constant_index;
    AvifdecByteReader reader;
    uint32_t location_ids[AVIF_ITEM_INDEX_MAX_ITEMS];
    size_t location_id_count = 0U;
    uint8_t version;
    uint32_t flags;
    uint8_t first_sizes;
    uint8_t second_sizes;
    unsigned int offset_size;
    unsigned int length_size;
    unsigned int base_offset_size;
    unsigned int index_size;
    uint32_t item_count;
    uint32_t item_index;
    int found = 0;
    AvifdecStatus status;

    status = avif_item_full_box(
        index, &index->iloc, &version, &flags, error);
    if (status != AVIFDEC_OK) return status;
    if (version > 2U) {
        return avif_item_set_error(error, AVIFDEC_UNSUPPORTED,
                                   index->iloc.payload_offset,
                                   index->iloc.type);
    }
    if (flags != 0U) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   index->iloc.payload_offset + 1U,
                                   index->iloc.type);
    }
    avifdec_byte_reader_init(
        &reader, index->data + index->iloc.payload_offset + 4U,
        index->iloc.payload_size - 4U, index->iloc.payload_offset + 4U);
    first_sizes = avifdec_byte_reader_u8(&reader);
    second_sizes = avifdec_byte_reader_u8(&reader);
    offset_size = first_sizes >> 4;
    length_size = first_sizes & 15U;
    base_offset_size = second_sizes >> 4;
    index_size = version == 0U ? 0U : second_sizes & 15U;
    if (version == 0U && (second_sizes & 15U) != 0U) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   index->iloc.payload_offset + 5U,
                                   index->iloc.type);
    }
    if (!avif_item_valid_iloc_field_size(offset_size) ||
        !avif_item_valid_iloc_field_size(length_size) ||
        !avif_item_valid_iloc_field_size(base_offset_size) ||
        !avif_item_valid_iloc_field_size(index_size)) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   index->iloc.payload_offset + 4U,
                                   index->iloc.type);
    }
    item_count = version < 2U
        ? (uint32_t)avifdec_byte_reader_u16be(&reader)
        : avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avif_item_set_error(error, reader.status,
                                   avifdec_byte_reader_offset(&reader),
                                   index->iloc.type);
    }
    if ((size_t)item_count > index->limits.max_items) {
        return avif_item_set_error(error, AVIFDEC_LIMIT_EXCEEDED,
                                   avifdec_byte_reader_offset(&reader),
                                   index->iloc.type);
    }
    if ((size_t)item_count > AVIF_ITEM_INDEX_MAX_ITEMS) {
        return avif_item_set_error(error, AVIFDEC_UNSUPPORTED,
                                   avifdec_byte_reader_offset(&reader),
                                   index->iloc.type);
    }
    if (requested_location != 0) {
        avifdec_memory_fill(
            requested_location, 0U, sizeof(*requested_location));
    }
    for (item_index = 0U; item_index < item_count; ++item_index) {
        size_t item_source_offset = avifdec_byte_reader_offset(&reader);
        uint32_t item_id = version < 2U
            ? (uint32_t)avifdec_byte_reader_u16be(&reader)
            : avifdec_byte_reader_u32be(&reader);
        uint8_t construction_method = 0U;
        uint16_t data_reference_index;
        size_t method_source_offset = avifdec_byte_reader_offset(&reader);
        size_t data_reference_source_offset;
        uint64_t base_offset = 0U;
        uint16_t extent_count;
        uint16_t extent_index;
        size_t duplicate_index;
        int selected;

        if (version > 0U) {
            uint16_t method = avifdec_byte_reader_u16be(&reader);

            if ((method & 0xfff0U) != 0U) {
                return avif_item_set_error(
                    error, AVIFDEC_INVALID_DATA,
                    avifdec_byte_reader_offset(&reader) - 2U,
                    index->iloc.type);
            }
            construction_method = (uint8_t)(method & 15U);
        }
        data_reference_source_offset =
            avifdec_byte_reader_offset(&reader);
        data_reference_index = avifdec_byte_reader_u16be(&reader);
        if (data_reference_index != 0U) {
            return avif_item_set_error(
                error, AVIFDEC_UNSUPPORTED,
                data_reference_source_offset, index->iloc.type);
        }
        if (avif_item_read_sized_uint(
                &reader, base_offset_size, &base_offset) != AVIFDEC_OK) {
            break;
        }
        extent_count = avifdec_byte_reader_u16be(&reader);
        if (reader.status != AVIFDEC_OK) break;
        if ((size_t)extent_count > index->limits.max_extents) {
            return avif_item_set_error(
                error, AVIFDEC_LIMIT_EXCEEDED,
                avifdec_byte_reader_offset(&reader) - 2U,
                index->iloc.type);
        }
        if ((size_t)extent_count > AVIF_ITEM_INDEX_MAX_EXTENTS) {
            return avif_item_set_error(
                error, AVIFDEC_UNSUPPORTED,
                avifdec_byte_reader_offset(&reader) - 2U,
                index->iloc.type);
        }
        if (item_id == 0U || !avif_item_id_exists(index, item_id)) {
            return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                       item_source_offset,
                                       index->iloc.type);
        }
        for (duplicate_index = 0U;
             duplicate_index < location_id_count;
             ++duplicate_index) {
            if (location_ids[duplicate_index] == item_id) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           item_source_offset,
                                           index->iloc.type);
            }
        }
        location_ids[location_id_count++] = item_id;
        selected = requested_location != 0 && item_id == requested_item_id;
        if (selected) {
            if (found) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           item_source_offset,
                                           index->iloc.type);
            }
            found = 1;
            requested_location->item_id = item_id;
            requested_location->data_reference_index =
                data_reference_index;
            requested_location->construction_method =
                construction_method;
            requested_location->index_size = (uint8_t)index_size;
            requested_location->base_offset = base_offset;
            requested_location->extent_count = extent_count;
            requested_location->source_offset = item_source_offset;
            requested_location->construction_method_offset =
                version == 0U ? item_source_offset : method_source_offset;
            requested_location->data_reference_offset =
                data_reference_source_offset;
        }
        for (extent_index = 0U;
             extent_index < extent_count;
             ++extent_index) {
            uint64_t parsed_index = 0U;
            uint64_t extent_offset = 0U;
            uint64_t extent_length = 0U;
            size_t extent_source_offset =
                avifdec_byte_reader_offset(&reader);

            if (index_size != 0U &&
                avif_item_read_sized_uint(
                    &reader, index_size, &parsed_index) != AVIFDEC_OK) {
                break;
            }
            if (avif_item_read_sized_uint(
                    &reader, offset_size, &extent_offset) != AVIFDEC_OK ||
                avif_item_read_sized_uint(
                    &reader, length_size, &extent_length) != AVIFDEC_OK) {
                break;
            }
            if (selected) {
                requested_location->extents[extent_index].index =
                    parsed_index;
                requested_location->extents[extent_index].offset =
                    extent_offset;
                requested_location->extents[extent_index].length =
                    extent_length;
                requested_location->extents[extent_index].source_offset =
                    extent_source_offset;
            }
        }
        if (reader.status != AVIFDEC_OK) break;
    }
    if (reader.status != AVIFDEC_OK) {
        return avif_item_set_error(error, reader.status,
                                   avifdec_byte_reader_offset(&reader),
                                   index->iloc.type);
    }
    if (avifdec_byte_reader_remaining(&reader) != 0U) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   avifdec_byte_reader_offset(&reader),
                                   index->iloc.type);
    }
    if (requested_location != 0 && !found) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   index->iloc.offset,
                                   index->iloc.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_parse_associations(AvifItemIndex *index) {
    size_t box_index;

    for (box_index = 0U; box_index < index->ipma_count; ++box_index) {
        const AvifdecBmffBox *box = &index->ipma[box_index];
        AvifdecByteReader reader;
        uint8_t version;
        uint32_t flags;
        uint32_t entry_count;
        uint32_t entry_index;
        AvifdecStatus status;

        status = avif_item_full_box(
            index, box, &version, &flags, index->error);
        if (status != AVIFDEC_OK) return status;
        if (version > 1U) {
            return avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                                  box->payload_offset, box->type);
        }
        if ((flags & ~1U) != 0U) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  box->payload_offset + 1U, box->type);
        }
        avifdec_byte_reader_init(
            &reader, index->data + box->payload_offset + 4U,
            box->payload_size - 4U, box->payload_offset + 4U);
        entry_count = avifdec_byte_reader_u32be(&reader);
        if (reader.status != AVIFDEC_OK) {
            return avif_item_fail(index, reader.status,
                                  avifdec_byte_reader_offset(&reader),
                                  box->type);
        }
        for (entry_index = 0U; entry_index < entry_count; ++entry_index) {
            uint32_t item_id = version == 0U
                ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                : avifdec_byte_reader_u32be(&reader);
            uint8_t association_count =
                avifdec_byte_reader_u8(&reader);
            uint8_t association_index;

            if (reader.status != AVIFDEC_OK) break;
            if (!avif_item_id_exists(index, item_id)) {
                return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                      avifdec_byte_reader_offset(&reader),
                                      box->type);
            }
            for (association_index = 0U;
                 association_index < association_count;
                 ++association_index) {
                size_t source_offset =
                    avifdec_byte_reader_offset(&reader);
                uint16_t value = (flags & 1U) != 0U
                    ? avifdec_byte_reader_u16be(&reader)
                    : (uint16_t)avifdec_byte_reader_u8(&reader);
                unsigned int essential_bit =
                    (flags & 1U) != 0U ? 15U : 7U;
                uint16_t property_index = (uint16_t)(
                    value & ((1U << essential_bit) - 1U));
                uint8_t essential =
                    (uint8_t)((value >> essential_bit) & 1U);
                size_t existing;

                if (reader.status != AVIFDEC_OK) break;
                if (property_index == 0U) {
                    if (essential) {
                        return avif_item_fail(
                            index, AVIFDEC_INVALID_DATA,
                            source_offset, box->type);
                    }
                    continue;
                }
                if ((size_t)property_index > index->property_count) {
                    return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                          source_offset, box->type);
                }
                for (existing = 0U;
                     existing < index->association_count;
                     ++existing) {
                    const AvifItemIndexAssociation *association =
                        &index->associations[existing];
                    if (association->item_id == item_id &&
                        association->property_index == property_index) {
                        return avif_item_fail(
                            index, AVIFDEC_INVALID_DATA,
                            source_offset, box->type);
                    }
                }
                status = avif_item_fixed_limit(
                    index, index->association_count + 1U,
                    index->limits.max_associations,
                    AVIF_ITEM_INDEX_MAX_ASSOCIATIONS,
                    source_offset, box->type);
                if (status != AVIFDEC_OK) return status;
                index->associations[index->association_count].item_id =
                    item_id;
                index->associations[
                    index->association_count].property_index =
                    property_index;
                index->associations[
                    index->association_count].essential = essential;
                ++index->association_count;
            }
            if (reader.status != AVIFDEC_OK) break;
        }
        if (reader.status != AVIFDEC_OK) {
            return avif_item_fail(index, reader.status,
                                  avifdec_byte_reader_offset(&reader),
                                  box->type);
        }
        if (avifdec_byte_reader_remaining(&reader) != 0U) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  avifdec_byte_reader_offset(&reader),
                                  box->type);
        }
    }
    return AVIFDEC_OK;
}

static int avif_item_find_index(const AvifItemIndex *index,
                                uint32_t item_id) {
    size_t item_index;

    for (item_index = 0U; item_index < index->item_count; ++item_index) {
        if (index->items[item_index].id == item_id) {
            return (int)item_index;
        }
    }
    return -1;
}

static AvifdecStatus avif_item_visit_graph(
    AvifItemIndex *index,
    size_t item_index,
    uint32_t type,
    uint8_t *states) {
    size_t reference_index;

    if (states[item_index] == 1U) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              index->iref.offset, type);
    }
    if (states[item_index] == 2U) return AVIFDEC_OK;
    states[item_index] = 1U;
    for (reference_index = 0U;
         reference_index < index->reference_count;
         ++reference_index) {
        const AvifItemIndexReference *reference =
            &index->references[reference_index];
        int target_index;
        AvifdecStatus status;

        if (reference->type != type ||
            reference->from_item_id != index->items[item_index].id) {
            continue;
        }
        target_index = avif_item_find_index(
            index, reference->to_item_id);
        if (target_index < 0) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  reference->source_offset, type);
        }
        status = avif_item_visit_graph(
            index, (size_t)target_index, type, states);
        if (status != AVIFDEC_OK) return status;
    }
    states[item_index] = 2U;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_validate_graph(
    AvifItemIndex *index,
    uint32_t type) {
    uint8_t states[AVIF_ITEM_INDEX_MAX_ITEMS];
    size_t item_index;

    avifdec_memory_fill(states, 0U, sizeof(states));
    for (item_index = 0U; item_index < index->item_count; ++item_index) {
        AvifdecStatus status = avif_item_visit_graph(
            index, item_index, type, states);
        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_validate_structure(
    AvifItemIndex *index,
    const AvifdecBmffInfo *bmff_info) {
    const unsigned char *payload;
    uint8_t version;
    uint32_t flags;
    uint32_t declared_item_count;
    size_t reference_index;
    size_t group_index;
    AvifdecStatus status;

    if (!bmff_info->has_avif_brand) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA, 0U,
                              AVIFDEC_FOURCC('f', 't', 'y', 'p'));
    }
    if (!avif_item_box_is_set(&index->meta) ||
        !avif_item_box_is_set(&index->handler) ||
        (index->require_primary_item &&
         !avif_item_box_is_set(&index->pitm)) ||
        !avif_item_box_is_set(&index->iloc) ||
        !avif_item_box_is_set(&index->iinf)) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA, 0U,
                              AVIFDEC_FOURCC('m', 'e', 't', 'a'));
    }
    status = avif_item_full_box(
        index, &index->meta, &version, &flags, index->error);
    if (status != AVIFDEC_OK) return status;
    if (version != 0U || flags != 0U) {
        return avif_item_fail(
            index, version != 0U ? AVIFDEC_UNSUPPORTED
                                 : AVIFDEC_INVALID_DATA,
            index->meta.payload_offset, index->meta.type);
    }
    status = avif_item_full_box(
        index, &index->handler, &version, &flags, index->error);
    if (status != AVIFDEC_OK) return status;
    payload = index->data + index->handler.payload_offset;
    if (index->handler.payload_size < 12U) {
        return avif_item_fail(index, AVIFDEC_TRUNCATED,
                              index->handler.payload_offset,
                              index->handler.type);
    }
    if (version != 0U || flags != 0U ||
        (index->require_primary_item &&
         avifdec_load_u32be(payload + 8U) !=
             AVIFDEC_FOURCC('p', 'i', 'c', 't')) ||
        (!index->require_primary_item &&
         avifdec_load_u32be(payload + 8U) !=
             AVIFDEC_FOURCC('p', 'i', 'c', 't') &&
         avifdec_load_u32be(payload + 8U) !=
             AVIFDEC_FOURCC('m', 'e', 't', 'a') &&
         avifdec_load_u32be(payload + 8U) !=
             AVIFDEC_FOURCC('m', 'd', 't', 'a'))) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              index->handler.offset,
                              index->handler.type);
    }
    if (avif_item_box_is_set(&index->pitm)) {
        status = avif_item_full_box(
            index, &index->pitm, &version, &flags, index->error);
        if (status != AVIFDEC_OK) return status;
        if (version > 1U) {
            return avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                                  index->pitm.payload_offset,
                                  index->pitm.type);
        }
        if (index->pitm.payload_size < (version == 0U ? 6U : 8U)) {
            return avif_item_fail(index, AVIFDEC_TRUNCATED,
                                  index->pitm.payload_offset,
                                  index->pitm.type);
        }
        if (flags != 0U ||
            index->pitm.payload_size != (version == 0U ? 6U : 8U)) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  index->pitm.offset, index->pitm.type);
        }
        payload = index->data + index->pitm.payload_offset + 4U;
        index->primary_item_id = version == 0U
            ? (uint32_t)avifdec_load_u16be(payload)
            : avifdec_load_u32be(payload);
        if (!avif_item_id_exists(index, index->primary_item_id)) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  index->pitm.payload_offset + 4U,
                                  index->pitm.type);
        }
    }
    status = avif_item_full_box(
        index, &index->iinf, &version, &flags, index->error);
    if (status != AVIFDEC_OK) return status;
    if (version > 1U) {
        return avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                              index->iinf.payload_offset,
                              index->iinf.type);
    }
    if (index->iinf.payload_size < (version == 0U ? 6U : 8U)) {
        return avif_item_fail(index, AVIFDEC_TRUNCATED,
                              index->iinf.payload_offset,
                              index->iinf.type);
    }
    if (flags != 0U) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              index->iinf.payload_offset + 1U,
                              index->iinf.type);
    }
    payload = index->data + index->iinf.payload_offset + 4U;
    declared_item_count = version == 0U
        ? (uint32_t)avifdec_load_u16be(payload)
        : avifdec_load_u32be(payload);
    if ((size_t)declared_item_count > index->limits.max_items) {
        return avif_item_fail(index, AVIFDEC_LIMIT_EXCEEDED,
                              index->iinf.payload_offset + 4U,
                              index->iinf.type);
    }
    if ((size_t)declared_item_count > AVIF_ITEM_INDEX_MAX_ITEMS) {
        return avif_item_fail(index, AVIFDEC_UNSUPPORTED,
                              index->iinf.payload_offset + 4U,
                              index->iinf.type);
    }
    if ((size_t)declared_item_count != index->item_count) {
        return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                              index->iinf.payload_offset + 4U,
                              index->iinf.type);
    }
    status = avif_item_scan_locations(index, 0U, 0, index->error);
    if (status != AVIFDEC_OK) {
        index->failure_status = status;
        return status;
    }
    for (reference_index = 0U;
         reference_index < index->reference_count;
         ++reference_index) {
        const AvifItemIndexReference *reference =
            &index->references[reference_index];
        if (!avif_item_id_exists(index, reference->from_item_id) ||
            !avif_item_id_exists(index, reference->to_item_id)) {
            return avif_item_fail(index, AVIFDEC_INVALID_DATA,
                                  reference->source_offset,
                                  reference->type);
        }
    }
    for (group_index = 0U;
         group_index < index->entity_group_count;
         ++group_index) {
        const AvifItemIndexEntityGroup *group =
            &index->entity_groups[group_index];
        size_t entity_index;

        if (avif_item_id_exists(index, group->group_id) ||
            group->entity_index > index->group_entity_count ||
            group->entity_count >
                index->group_entity_count - group->entity_index) {
            return avif_item_fail(
                index, AVIFDEC_INVALID_DATA,
                group->source_offset, group->type);
        }
        for (entity_index = 0U;
             entity_index < group->entity_count;
             ++entity_index) {
            const AvifItemIndexGroupEntity *entity =
                &index->group_entities[
                    group->entity_index + entity_index];

            if (!avif_item_id_exists(index, entity->entity_id)) {
                return avif_item_fail(
                    index, AVIFDEC_INVALID_DATA,
                    entity->source_offset, group->type);
            }
        }
    }
    status = avif_item_parse_associations(index);
    if (status != AVIFDEC_OK) return status;
    status = avif_item_validate_graph(
        index, AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    if (status != AVIFDEC_OK) return status;
    return avif_item_validate_graph(
        index, AVIFDEC_FOURCC('t', 'h', 'm', 'b'));
}

static AvifdecStatus avif_item_index_build_selected(
    const void *data,
    size_t size,
    const AvifItemIndexLimits *limits,
    size_t meta_offset,
    int require_primary_item,
    AvifItemIndex *index,
    AvifdecError *error) {
    AvifdecBmffLimits bmff_limits;
    AvifdecBmffInfo bmff_info;
    AvifdecStatus status;

    avif_item_clear_error(error);
    if (index == 0 || (data == 0 && size != 0U)) {
        return avif_item_set_error(error, AVIFDEC_INVALID_ARGUMENT,
                                   0U, 0U);
    }
    avifdec_memory_fill(index, 0U, sizeof(*index));
    index->data = (const unsigned char *)data;
    index->size = size;
    index->selected_meta_offset = meta_offset;
    index->require_primary_item = (uint8_t)(require_primary_item != 0);
    index->limits = avif_item_effective_limits(limits);
    index->error = error;
    index->failure_status = AVIFDEC_OK;
    bmff_limits.max_depth = 32U;
    bmff_limits.max_boxes = 100000U;
    status = avifdec_bmff_inspect(
        data, size, &bmff_limits, avif_item_collect_box, index,
        &bmff_info, error);
    if (index->failure_status != AVIFDEC_OK) {
        return index->failure_status;
    }
    if (status != AVIFDEC_OK) return status;
    status = avif_item_validate_structure(index, &bmff_info);
    if (status != AVIFDEC_OK) return status;
    index->error = 0;
    index->failure_status = AVIFDEC_OK;
    return AVIFDEC_OK;
}

AvifdecStatus avif_item_index_build(
    const void *data,
    size_t size,
    const AvifItemIndexLimits *limits,
    AvifItemIndex *index,
    AvifdecError *error) {
    return avif_item_index_build_selected(
        data, size, limits, SIZE_MAX, 1, index, error);
}

AvifdecStatus avif_item_index_build_meta(
    const void *data,
    size_t size,
    const AvifItemIndexLimits *limits,
    size_t meta_offset,
    AvifItemIndex *index,
    AvifdecError *error) {
    if (meta_offset == SIZE_MAX) {
        avif_item_clear_error(error);
        return avif_item_set_error(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return avif_item_index_build_selected(
        data, size, limits, meta_offset, 0, index, error);
}

const AvifItemIndexItem *avif_item_index_find_item(
    const AvifItemIndex *index,
    uint32_t item_id) {
    int item_index;

    if (index == 0) return 0;
    item_index = avif_item_find_index(index, item_id);
    return item_index < 0 ? 0 : &index->items[item_index];
}

AvifdecStatus avif_item_index_query_references(
    const AvifItemIndex *index,
    uint32_t type,
    uint32_t from_item_id,
    uint32_t *to_item_ids,
    size_t id_capacity,
    size_t *id_count,
    size_t *reference_offset,
    AvifdecError *error) {
    size_t reference_index;
    size_t count = 0U;

    avif_item_clear_error(error);
    if (id_count != 0) *id_count = 0U;
    if (reference_offset != 0) *reference_offset = 0U;
    if (index == 0 || type == 0U || from_item_id == 0U ||
        id_count == 0 || (to_item_ids == 0 && id_capacity != 0U)) {
        return avif_item_set_error(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (avif_item_index_find_item(index, from_item_id) == 0) {
        return avif_item_set_error(
            error, AVIFDEC_INVALID_DATA, index->iinf.offset,
            AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    for (reference_index = 0U;
         reference_index < index->reference_count;
         ++reference_index) {
        const AvifItemIndexReference *reference =
            &index->references[reference_index];

        if (reference->type != type ||
            reference->from_item_id != from_item_id) {
            continue;
        }
        if (count == 0U && reference_offset != 0) {
            *reference_offset = reference->source_offset;
        }
        if (count == SIZE_MAX) {
            return avif_item_set_error(
                error, AVIFDEC_OVERFLOW,
                reference->source_offset, reference->type);
        }
        ++count;
    }
    *id_count = count;
    if (to_item_ids == 0) return AVIFDEC_OK;
    if (id_capacity < count) {
        return avif_item_set_error(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    count = 0U;
    for (reference_index = 0U;
         reference_index < index->reference_count;
         ++reference_index) {
        const AvifItemIndexReference *reference =
            &index->references[reference_index];

        if (reference->type == type &&
            reference->from_item_id == from_item_id) {
            to_item_ids[count++] = reference->to_item_id;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_item_index_alternative_order(
    const AvifItemIndex *index,
    uint32_t first_item_id,
    uint32_t second_item_id,
    AvifItemAlternativeOrder *order,
    size_t *group_offset,
    AvifdecError *error) {
    size_t group_index;

    avif_item_clear_error(error);
    if (order != 0) *order = AVIF_ITEM_ALTERNATIVE_NONE;
    if (group_offset != 0) *group_offset = 0U;
    if (index == 0 || first_item_id == 0U ||
        second_item_id == 0U || first_item_id == second_item_id ||
        order == 0 || group_offset == 0) {
        return avif_item_set_error(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (avif_item_index_find_item(index, first_item_id) == 0 ||
        avif_item_index_find_item(index, second_item_id) == 0) {
        return avif_item_set_error(
            error, AVIFDEC_INVALID_DATA, index->iinf.offset,
            AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    for (group_index = 0U;
         group_index < index->entity_group_count;
         ++group_index) {
        const AvifItemIndexEntityGroup *group =
            &index->entity_groups[group_index];
        size_t first_ordinal = SIZE_MAX;
        size_t second_ordinal = SIZE_MAX;
        size_t entity_index;

        if (group->type != AVIFDEC_FOURCC('a', 'l', 't', 'r')) {
            continue;
        }
        for (entity_index = 0U;
             entity_index < group->entity_count;
             ++entity_index) {
            uint32_t entity_id = index->group_entities[
                group->entity_index + entity_index].entity_id;

            if (entity_id == first_item_id) {
                first_ordinal = entity_index;
            } else if (entity_id == second_item_id) {
                second_ordinal = entity_index;
            }
        }
        if (first_ordinal != SIZE_MAX &&
            second_ordinal != SIZE_MAX) {
            *order = first_ordinal < second_ordinal
                ? AVIF_ITEM_ALTERNATIVE_FIRST_BEFORE_SECOND
                : AVIF_ITEM_ALTERNATIVE_SECOND_BEFORE_FIRST;
            *group_offset = group->source_offset;
            return AVIFDEC_OK;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_item_index_find_location(
    const AvifItemIndex *index,
    uint32_t item_id,
    AvifItemIndexLocation *location,
    AvifdecError *error) {
    avif_item_clear_error(error);
    if (index == 0 || location == 0 || item_id == 0U) {
        return avif_item_set_error(error, AVIFDEC_INVALID_ARGUMENT,
                                   0U, 0U);
    }
    return avif_item_scan_locations(
        index, item_id, location, error);
}

static AvifdecStatus avif_item_validate_access(
    const AvifItemIndex *index,
    const AvifItemIndexLocation *location,
    AvifdecError *error) {
    const AvifItemIndexItem *item =
        avif_item_index_find_item(index, location->item_id);

    if (item == 0) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   location->source_offset,
                                   index->iloc.type);
    }
    if (item->protection_index != 0U) {
        return avif_item_set_error(error, AVIFDEC_UNSUPPORTED,
                                   item->protection_offset,
                                   AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    if (location->data_reference_index != 0U) {
        return avif_item_set_error(error, AVIFDEC_UNSUPPORTED,
                                   location->data_reference_offset,
                                   index->iloc.type);
    }
    if (location->construction_method > 1U) {
        return avif_item_set_error(error, AVIFDEC_UNSUPPORTED,
                                   location->construction_method_offset,
                                   index->iloc.type);
    }
    if (location->extent_count == 0U) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   location->source_offset,
                                   index->iloc.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_walk_append(
    const AvifItemIndex *index,
    AvifItemWalk *walk,
    size_t file_offset,
    size_t length,
    size_t source_offset,
    AvifdecError *error) {
    size_t next_payload_size;

    if (length == 0U) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   source_offset, index->iloc.type);
    }
    if (!avifdec_size_add(
            walk->payload_size, length, &next_payload_size)) {
        return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                   source_offset, index->iloc.type);
    }
    if (walk->write_spans) {
        if (walk->span_count >= walk->span_capacity) {
            return avif_item_set_error(error, AVIFDEC_OUT_OF_MEMORY,
                                       source_offset,
                                       index->iloc.type);
        }
        walk->spans[walk->span_count].data = index->data + file_offset;
        walk->spans[walk->span_count].size = length;
        walk->spans[walk->span_count].file_offset = file_offset;
    }
    if (walk->requested_span != 0 &&
        walk->span_count == walk->requested_span_index) {
        walk->requested_span->data = index->data + file_offset;
        walk->requested_span->size = length;
        walk->requested_span->file_offset = file_offset;
    }
    if (walk->copy_bytes && walk->copy_size != 0U) {
        size_t chunk_start = walk->payload_size;
        size_t chunk_end = next_payload_size;
        size_t copy_end;

        if (!avifdec_size_add(
                walk->copy_offset, walk->copy_size, &copy_end)) {
            return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                       source_offset,
                                       index->iloc.type);
        }
        if (copy_end > chunk_start && walk->copy_offset < chunk_end) {
            size_t intersection_start =
                walk->copy_offset > chunk_start
                    ? walk->copy_offset : chunk_start;
            size_t intersection_end =
                copy_end < chunk_end ? copy_end : chunk_end;
            size_t local_offset = intersection_start - chunk_start;
            size_t copy_length =
                intersection_end - intersection_start;

            if (walk->copied == 0U) {
                walk->first_file_offset = file_offset + local_offset;
            }
            avifdec_memory_copy(
                walk->copy_output + walk->copied,
                index->data + file_offset + local_offset,
                copy_length);
            walk->copied += copy_length;
        }
    }
    walk->payload_size = next_payload_size;
    if (walk->span_count == SIZE_MAX) {
        return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                   source_offset, index->iloc.type);
    }
    ++walk->span_count;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_item_walk_location(
    const AvifItemIndex *index,
    const AvifItemIndexLocation *location,
    AvifItemWalk *walk,
    AvifdecError *error) {
    size_t extent_index;
    AvifdecStatus status;

    status = avif_item_validate_access(index, location, error);
    if (status != AVIFDEC_OK) return status;
    for (extent_index = 0U;
         extent_index < location->extent_count;
         ++extent_index) {
        const AvifItemIndexExtent *extent =
            &location->extents[extent_index];
        uint64_t logical_offset;
        uint64_t remaining = extent->length;

        if (UINT64_MAX - location->base_offset < extent->offset) {
            return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                       extent->source_offset,
                                       index->iloc.type);
        }
        logical_offset = location->base_offset + extent->offset;
        if (remaining == 0U) {
            return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                       extent->source_offset,
                                       index->iloc.type);
        }
        if (location->construction_method == 0U) {
            uint64_t end;
            size_t data_index;
            int found = 0;

            if (UINT64_MAX - logical_offset < remaining) {
                return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            end = logical_offset + remaining;
            if (logical_offset > (uint64_t)SIZE_MAX ||
                remaining > (uint64_t)SIZE_MAX) {
                return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            if (end > (uint64_t)index->size) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            for (data_index = 0U;
                 data_index < index->data_box_count;
                 ++data_index) {
                const AvifdecBmffBox *box =
                    &index->data_boxes[data_index];
                uint64_t box_start;
                uint64_t box_end;

                if (box->type !=
                    AVIFDEC_FOURCC('m', 'd', 'a', 't')) {
                    continue;
                }
                box_start = (uint64_t)box->payload_offset;
                box_end = box_start + (uint64_t)box->payload_size;
                if (logical_offset >= box_start && end <= box_end) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            status = avif_item_walk_append(
                index, walk, (size_t)logical_offset,
                (size_t)remaining, extent->source_offset, error);
            if (status != AVIFDEC_OK) return status;
        } else {
            uint64_t end;
            size_t file_offset;

            if (!avif_item_box_is_set(&index->idat)) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            if (UINT64_MAX - logical_offset < remaining) {
                return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            end = logical_offset + remaining;
            if (logical_offset > (uint64_t)SIZE_MAX ||
                remaining > (uint64_t)SIZE_MAX) {
                return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            if (end > (uint64_t)index->idat.payload_size) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            if (!avifdec_size_add(
                    index->idat.payload_offset,
                    (size_t)logical_offset, &file_offset)) {
                return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                           extent->source_offset,
                                           index->iloc.type);
            }
            status = avif_item_walk_append(
                index, walk, file_offset, (size_t)remaining,
                extent->source_offset, error);
            if (status != AVIFDEC_OK) return status;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_item_index_resolve_location(
    const AvifItemIndex *index,
    const AvifItemIndexLocation *location,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemPayload *payload,
    AvifdecError *error) {
    AvifItemWalk count_walk;
    AvifItemWalk fill_walk;
    AvifdecStatus status;

    avif_item_clear_error(error);
    if (payload != 0) avifdec_memory_fill(payload, 0U, sizeof(*payload));
    if (index == 0 || location == 0 || payload == 0 ||
        (spans == 0 && span_capacity != 0U)) {
        return avif_item_set_error(error, AVIFDEC_INVALID_ARGUMENT,
                                   0U, 0U);
    }
    avifdec_memory_fill(&count_walk, 0U, sizeof(count_walk));
    status = avif_item_walk_location(
        index, location, &count_walk, error);
    if (status != AVIFDEC_OK) return status;
    payload->item_id = location->item_id;
    payload->payload_size = count_walk.payload_size;
    payload->span_count = count_walk.span_count;
    if (spans == 0) return AVIFDEC_OK;
    if (span_capacity < count_walk.span_count) {
        return avif_item_set_error(error, AVIFDEC_OUT_OF_MEMORY,
                                   location->source_offset, 0U);
    }
    avifdec_memory_fill(&fill_walk, 0U, sizeof(fill_walk));
    fill_walk.spans = spans;
    fill_walk.span_capacity = span_capacity;
    fill_walk.write_spans = 1;
    return avif_item_walk_location(
        index, location, &fill_walk, error);
}

AvifdecStatus avif_item_index_resolve_item(
    const AvifItemIndex *index,
    uint32_t item_id,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemPayload *payload,
    AvifdecError *error) {
    AvifItemIndexLocation location;
    AvifdecStatus status;

    avif_item_clear_error(error);
    if (payload != 0) avifdec_memory_fill(payload, 0U, sizeof(*payload));
    if (index == 0 || item_id == 0U || payload == 0 ||
        (spans == 0 && span_capacity != 0U)) {
        return avif_item_set_error(error, AVIFDEC_INVALID_ARGUMENT,
                                   0U, 0U);
    }
    status = avif_item_index_find_location(
        index, item_id, &location, error);
    if (status != AVIFDEC_OK) return status;
    return avif_item_index_resolve_location(
        index, &location, spans, span_capacity, payload, error);
}

AvifdecStatus avif_item_index_item_span_at(
    const AvifItemIndex *index,
    uint32_t item_id,
    size_t span_index,
    AvifdecSpan *span,
    AvifdecError *error) {
    AvifItemIndexLocation location;
    AvifItemWalk walk;
    AvifdecStatus status;

    avif_item_clear_error(error);
    if (span != 0) avifdec_memory_fill(span, 0U, sizeof(*span));
    if (index == 0 || item_id == 0U || span == 0) {
        return avif_item_set_error(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_item_index_find_location(
        index, item_id, &location, error);
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(&walk, 0U, sizeof(walk));
    walk.requested_span_index = span_index;
    walk.requested_span = span;
    status = avif_item_walk_location(index, &location, &walk, error);
    if (status != AVIFDEC_OK) return status;
    if (span_index >= walk.span_count) {
        avifdec_memory_fill(span, 0U, sizeof(*span));
        return avif_item_set_error(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_item_index_read_item(
    const AvifItemIndex *index,
    uint32_t item_id,
    size_t payload_offset,
    void *output,
    size_t output_size,
    size_t *first_file_offset,
    AvifdecError *error) {
    AvifItemIndexLocation location;
    AvifItemWalk count_walk;
    AvifItemWalk copy_walk;
    size_t copy_end;
    AvifdecStatus status;

    avif_item_clear_error(error);
    if (first_file_offset != 0) *first_file_offset = 0U;
    if (index == 0 || item_id == 0U ||
        (output == 0 && output_size != 0U)) {
        return avif_item_set_error(error, AVIFDEC_INVALID_ARGUMENT,
                                   0U, 0U);
    }
    status = avif_item_index_find_location(
        index, item_id, &location, error);
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(&count_walk, 0U, sizeof(count_walk));
    status = avif_item_walk_location(
        index, &location, &count_walk, error);
    if (status != AVIFDEC_OK) return status;
    if (!avifdec_size_add(payload_offset, output_size, &copy_end)) {
        return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                   location.source_offset,
                                   index->iloc.type);
    }
    if (copy_end > count_walk.payload_size) {
        return avif_item_set_error(error, AVIFDEC_TRUNCATED,
                                   location.source_offset,
                                   index->iloc.type);
    }
    if (output_size == 0U) return AVIFDEC_OK;
    avifdec_memory_fill(&copy_walk, 0U, sizeof(copy_walk));
    copy_walk.copy_output = (unsigned char *)output;
    copy_walk.copy_offset = payload_offset;
    copy_walk.copy_size = output_size;
    copy_walk.copy_bytes = 1;
    status = avif_item_walk_location(
        index, &location, &copy_walk, error);
    if (status != AVIFDEC_OK) return status;
    if (copy_walk.copied != output_size) {
        return avif_item_set_error(error, AVIFDEC_TRUNCATED,
                                   location.source_offset,
                                   index->iloc.type);
    }
    if (first_file_offset != 0) {
        *first_file_offset = copy_walk.first_file_offset;
    }
    return AVIFDEC_OK;
}

int avif_item_index_property_type_supported(uint32_t type) {
    return type == AVIFDEC_FOURCC('a', 'v', '1', 'C') ||
           type == AVIFDEC_FOURCC('i', 's', 'p', 'e') ||
           type == AVIFDEC_FOURCC('p', 'i', 'x', 'i') ||
           type == AVIFDEC_FOURCC('c', 'o', 'l', 'r') ||
           type == AVIFDEC_FOURCC('a', 'u', 'x', 'C') ||
           type == AVIFDEC_FOURCC('p', 'a', 's', 'p') ||
           type == AVIFDEC_FOURCC('c', 'l', 'a', 'p') ||
           type == AVIFDEC_FOURCC('i', 'r', 'o', 't') ||
           type == AVIFDEC_FOURCC('i', 'm', 'i', 'r') ||
           type == AVIFDEC_FOURCC('c', 'l', 'l', 'i') ||
           type == AVIFDEC_FOURCC('m', 'd', 'c', 'v') ||
           type == AVIFDEC_FOURCC('a', '1', 'o', 'p') ||
           type == AVIFDEC_FOURCC('l', 's', 'e', 'l') ||
           type == AVIFDEC_FOURCC('a', '1', 'l', 'x');
}

AvifdecStatus avif_item_index_validate_essential_properties(
    const AvifItemIndex *index,
    uint32_t item_id,
    AvifdecError *error) {
    size_t association_index;

    avif_item_clear_error(error);
    if (index == 0 || item_id == 0U) {
        return avif_item_set_error(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (avif_item_index_find_item(index, item_id) == 0) {
        return avif_item_set_error(
            error, AVIFDEC_INVALID_DATA, index->iinf.offset,
            AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    for (association_index = 0U;
         association_index < index->association_count;
         ++association_index) {
        const AvifItemIndexAssociation *association =
            &index->associations[association_index];
        const AvifItemIndexProperty *property;

        if (association->item_id != item_id ||
            !association->essential) {
            continue;
        }
        property =
            &index->properties[association->property_index - 1U];
        if (!avif_item_index_property_type_supported(property->type)) {
            return avif_item_set_error(
                error, AVIFDEC_UNSUPPORTED,
                property->box.offset, property->type);
        }
    }
    return AVIFDEC_OK;
}

static int avif_item_crop_axis(uint32_t image_size,
                               uint32_t aperture_size,
                               int32_t offset_n,
                               uint32_t offset_d) {
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
    return result >= 0 &&
           (uint64_t)result + aperture_size <= image_size;
}

static AvifdecStatus avif_item_clap_dimensions(
    const unsigned char *payload,
    uint32_t image_width,
    uint32_t image_height,
    uint32_t *crop_width,
    uint32_t *crop_height) {
    uint32_t width_n = avifdec_load_u32be(payload);
    uint32_t width_d = avifdec_load_u32be(payload + 4U);
    uint32_t height_n = avifdec_load_u32be(payload + 8U);
    uint32_t height_d = avifdec_load_u32be(payload + 12U);
    int32_t horizontal_offset_n =
        (int32_t)avifdec_load_u32be(payload + 16U);
    uint32_t horizontal_offset_d =
        avifdec_load_u32be(payload + 20U);
    int32_t vertical_offset_n =
        (int32_t)avifdec_load_u32be(payload + 24U);
    uint32_t vertical_offset_d =
        avifdec_load_u32be(payload + 28U);

    if (width_d == 0U || height_d == 0U ||
        horizontal_offset_d == 0U || vertical_offset_d == 0U ||
        width_n == 0U || height_n == 0U ||
        width_n % width_d != 0U ||
        height_n % height_d != 0U) {
        return AVIFDEC_INVALID_DATA;
    }
    *crop_width = width_n / width_d;
    *crop_height = height_n / height_d;
    if (*crop_width == 0U || *crop_height == 0U ||
        *crop_width > image_width || *crop_height > image_height ||
        !avif_item_crop_axis(
            image_width, *crop_width,
            horizontal_offset_n, horizontal_offset_d) ||
        !avif_item_crop_axis(
            image_height, *crop_height,
            vertical_offset_n, vertical_offset_d)) {
        return AVIFDEC_INVALID_DATA;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_item_index_item_dimensions(
    const AvifItemIndex *index,
    uint32_t item_id,
    uint32_t *width,
    uint32_t *height,
    uint32_t *presentation_width,
    uint32_t *presentation_height,
    AvifdecError *error) {
    const AvifItemIndexItem *item;
    const AvifItemIndexProperty *ispe = 0;
    size_t association_index;
    size_t pixels;
    int seen_pasp = 0;
    unsigned int transform_stage = 0U;

    avif_item_clear_error(error);
    if (width != 0) *width = 0U;
    if (height != 0) *height = 0U;
    if (presentation_width != 0) *presentation_width = 0U;
    if (presentation_height != 0) *presentation_height = 0U;
    if (index == 0 || item_id == 0U || width == 0 || height == 0 ||
        presentation_width == 0 || presentation_height == 0) {
        return avif_item_set_error(error, AVIFDEC_INVALID_ARGUMENT,
                                   0U, 0U);
    }
    item = avif_item_index_find_item(index, item_id);
    if (item == 0) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   index->iinf.offset,
                                   AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    if (item->protection_index != 0U) {
        return avif_item_set_error(error, AVIFDEC_UNSUPPORTED,
                                   item->protection_offset,
                                   AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    for (association_index = 0U;
         association_index < index->association_count;
         ++association_index) {
        const AvifItemIndexAssociation *association =
            &index->associations[association_index];
        const AvifItemIndexProperty *property;
        const unsigned char *payload;

        if (association->item_id != item_id) continue;
        property = &index->properties[association->property_index - 1U];
        payload = index->data + property->box.payload_offset;
        if (!avif_item_index_property_type_supported(property->type) &&
            association->essential) {
            return avif_item_set_error(error, AVIFDEC_UNSUPPORTED,
                                       property->box.offset,
                                       property->type);
        }
        if (property->type == AVIFDEC_FOURCC('i', 's', 'p', 'e')) {
            if (ispe != 0 || property->box.payload_size != 12U ||
                payload[0] != 0U ||
                avif_item_full_box_flags(payload) != 0U) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           property->box.offset,
                                           property->type);
            }
            *width = avifdec_load_u32be(payload + 4U);
            *height = avifdec_load_u32be(payload + 8U);
            if (*width == 0U || *height == 0U) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           property->box.offset,
                                           property->type);
            }
            ispe = property;
        } else if (property->type ==
                   AVIFDEC_FOURCC('p', 'a', 's', 'p')) {
            if (seen_pasp || property->box.payload_size != 8U ||
                avifdec_load_u32be(payload) == 0U ||
                avifdec_load_u32be(payload + 4U) == 0U) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           property->box.offset,
                                           property->type);
            }
            seen_pasp = 1;
        }
    }
    if (ispe == 0) {
        return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                   index->ipco.offset,
                                   AVIFDEC_FOURCC('i', 's', 'p', 'e'));
    }
    if (*width > index->limits.max_width ||
        *height > index->limits.max_height) {
        return avif_item_set_error(error, AVIFDEC_LIMIT_EXCEEDED,
                                   ispe->box.offset, ispe->type);
    }
    if (!avifdec_size_multiply(*width, *height, &pixels)) {
        return avif_item_set_error(error, AVIFDEC_OVERFLOW,
                                   ispe->box.offset, ispe->type);
    }
    if (pixels > index->limits.max_pixels) {
        return avif_item_set_error(error, AVIFDEC_LIMIT_EXCEEDED,
                                   ispe->box.offset, ispe->type);
    }
    *presentation_width = *width;
    *presentation_height = *height;
    for (association_index = 0U;
         association_index < index->association_count;
         ++association_index) {
        const AvifItemIndexAssociation *association =
            &index->associations[association_index];
        const AvifItemIndexProperty *property;
        const unsigned char *payload;

        if (association->item_id != item_id) continue;
        property = &index->properties[association->property_index - 1U];
        payload = index->data + property->box.payload_offset;
        if (property->type == AVIFDEC_FOURCC('c', 'l', 'a', 'p')) {
            uint32_t crop_width;
            uint32_t crop_height;
            if (property->box.payload_size != 32U ||
                !association->essential || transform_stage != 0U ||
                avif_item_clap_dimensions(
                    payload, *width, *height,
                    &crop_width, &crop_height) != AVIFDEC_OK) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           property->box.offset,
                                           property->type);
            }
            *presentation_width = crop_width;
            *presentation_height = crop_height;
            transform_stage = 1U;
        } else if (property->type ==
                   AVIFDEC_FOURCC('i', 'r', 'o', 't')) {
            uint32_t temporary;
            if (property->box.payload_size != 1U ||
                (payload[0] & 0xfcU) != 0U ||
                !association->essential || transform_stage > 1U) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           property->box.offset,
                                           property->type);
            }
            if ((payload[0] & 1U) != 0U) {
                temporary = *presentation_width;
                *presentation_width = *presentation_height;
                *presentation_height = temporary;
            }
            transform_stage = 2U;
        } else if (property->type ==
                   AVIFDEC_FOURCC('i', 'm', 'i', 'r')) {
            if (property->box.payload_size != 1U ||
                (payload[0] & 0xfeU) != 0U ||
                !association->essential || transform_stage > 2U) {
                return avif_item_set_error(error, AVIFDEC_INVALID_DATA,
                                           property->box.offset,
                                           property->type);
            }
            transform_stage = 3U;
        }
    }
    return AVIFDEC_OK;
}
