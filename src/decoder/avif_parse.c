#include "avif_parse.h"
#include "base.h"
#include "bmff.h"

AvifdecStatus avif_fail(AvifContext *context,
                        AvifdecStatus status,
                        size_t offset,
                        uint32_t box_type) {
    if (context->error != 0 && context->error->status == AVIFDEC_OK) {
        context->error->status = status;
        context->error->offset = offset;
        context->error->context = box_type;
    }
    context->failed = 1;
    return status;
}

static int avif_box_is_set(const AvifdecBmffBox *box) {
    return box->size != 0U;
}

static int avif_inside(const AvifdecBmffBox *child, const AvifdecBmffBox *parent) {
    size_t parent_end;
    size_t child_end;

    if (!avif_box_is_set(parent) || child->depth != parent->depth + 1U) return 0;
    if (!avifdec_size_add(parent->offset, parent->size, &parent_end) ||
        !avifdec_size_add(child->offset, child->size, &child_end)) return 0;
    return child->offset >= parent->payload_offset && child_end <= parent_end;
}

static uint8_t avif_full_box_version(const unsigned char *data,
                                     const AvifdecBmffBox *box,
                                     int *valid) {
    if (box->payload_size < 4U) {
        *valid = 0;
        return 0U;
    }
    *valid = 1;
    return data[box->payload_offset];
}

static void avif_collect_box(const AvifdecBmffBox *box, void *user_data) {
    AvifContext *context = (AvifContext *)user_data;
    uint32_t type = box->type;

    if (context->failed) return;
    if (box->depth == 0U && type == AVIFDEC_FOURCC('m', 'e', 't', 'a')) {
        if (avif_box_is_set(&context->meta)) {
            (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->meta = *box;
    } else if (type == AVIFDEC_FOURCC('h', 'd', 'l', 'r') &&
               avif_inside(box, &context->meta)) {
        if (avif_box_is_set(&context->handler)) {
            (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->handler = *box;
    } else if (type == AVIFDEC_FOURCC('p', 'i', 't', 'm') &&
               avif_inside(box, &context->meta)) {
        if (avif_box_is_set(&context->pitm)) {
            (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->pitm = *box;
    } else if (type == AVIFDEC_FOURCC('i', 'l', 'o', 'c') &&
               avif_inside(box, &context->meta)) {
        if (avif_box_is_set(&context->iloc)) {
            (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->iloc = *box;
    } else if (type == AVIFDEC_FOURCC('i', 'i', 'n', 'f') &&
               avif_inside(box, &context->meta)) {
        if (avif_box_is_set(&context->iinf)) {
            (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->iinf = *box;
    } else if (type == AVIFDEC_FOURCC('i', 'r', 'e', 'f') &&
               avif_inside(box, &context->meta)) {
        if (avif_box_is_set(&context->iref)) {
            (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->iref = *box;
    } else if (type == AVIFDEC_FOURCC('i', 'p', 'c', 'o')) {
        if (avif_box_is_set(&context->ipco)) {
            (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->ipco = *box;
    } else if (type == AVIFDEC_FOURCC('i', 'p', 'm', 'a')) {
        if (context->ipma_count >= sizeof(context->ipma) / sizeof(context->ipma[0])) {
            (void)avif_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, type);
            return;
        }
        context->ipma[context->ipma_count++] = *box;
    } else if ((type == AVIFDEC_FOURCC('m', 'd', 'a', 't') && box->depth == 0U) ||
               (type == AVIFDEC_FOURCC('i', 'd', 'a', 't') &&
                avif_inside(box, &context->meta))) {
        if (context->data_box_count >= AVIF_MAX_DATA_BOXES) {
            (void)avif_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, type);
            return;
        }
        context->data_boxes[context->data_box_count++] = *box;
    }

    if (avif_inside(box, &context->iref)) {
        if (context->reference_box_count >= AVIF_MAX_REFERENCE_BOXES) {
            (void)avif_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, type);
            return;
        }
        context->reference_boxes[context->reference_box_count++] = *box;
    }

    if (type == AVIFDEC_FOURCC('i', 'n', 'f', 'e') && avif_inside(box, &context->iinf)) {
        AvifdecByteReader reader;
        uint8_t version;
        int valid;
        uint32_t item_id;
        uint32_t item_type;
        size_t index;

        version = avif_full_box_version(context->data, box, &valid);
        if (!valid || (version != 2U && version != 3U)) return;
        avifdec_byte_reader_init(&reader,
                                 context->data + box->payload_offset + 4U,
                                 box->payload_size - 4U,
                                 box->payload_offset + 4U);
        item_id = version == 2U ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                                : avifdec_byte_reader_u32be(&reader);
        (void)avifdec_byte_reader_u16be(&reader);
        item_type = avifdec_byte_reader_u32be(&reader);
        if (reader.status != AVIFDEC_OK) {
            (void)avif_fail(context, reader.status, avifdec_byte_reader_offset(&reader), type);
            return;
        }
        for (index = 0U; index < context->item_count; ++index) {
            if (context->items[index].id == item_id) {
                (void)avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
                return;
            }
        }
        if (context->item_count >= context->limits.max_items || context->item_count >= AVIF_MAX_ITEMS) {
            (void)avif_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, type);
            return;
        }
        context->items[context->item_count].id = item_id;
        context->items[context->item_count].type = item_type;
        ++context->item_count;
    }

    if (avif_inside(box, &context->ipco)) {
        if (context->property_count >= context->limits.max_properties ||
            context->property_count >= AVIF_MAX_PROPERTIES) {
            (void)avif_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, type);
            return;
        }
        context->properties[context->property_count].box = *box;
        context->properties[context->property_count].type = type;
        ++context->property_count;
    }
}

static AvifdecStatus avif_read_sized_uint(AvifdecByteReader *reader,
                                          unsigned int byte_count,
                                          uint64_t *value) {
    const unsigned char *bytes;
    unsigned int index;

    if (byte_count > 8U) return AVIFDEC_UNSUPPORTED;
    bytes = avifdec_byte_reader_take(reader, byte_count);
    if (bytes == 0) return reader->status;
    *value = 0U;
    for (index = 0U; index < byte_count; ++index) {
        *value = (*value << 8) | bytes[index];
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_parse_primary_id(AvifContext *context, uint32_t *item_id) {
    AvifdecByteReader reader;
    uint8_t version;
    int valid;

    if (!avif_box_is_set(&context->pitm)) {
        return avif_fail(context, AVIFDEC_INVALID_DATA, 0U, AVIFDEC_FOURCC('p', 'i', 't', 'm'));
    }
    version = avif_full_box_version(context->data, &context->pitm, &valid);
    if (!valid || version > 1U) {
        return avif_fail(context, valid ? AVIFDEC_UNSUPPORTED : AVIFDEC_TRUNCATED,
                         context->pitm.offset, context->pitm.type);
    }
    avifdec_byte_reader_init(&reader,
                             context->data + context->pitm.payload_offset + 4U,
                             context->pitm.payload_size - 4U,
                             context->pitm.payload_offset + 4U);
    *item_id = version == 0U ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                             : avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avif_fail(context, reader.status, avifdec_byte_reader_offset(&reader), context->pitm.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_validate_meta(AvifContext *context) {
    const unsigned char *payload;
    uint8_t version;
    uint32_t declared_item_count;
    int valid;

    if (!avif_box_is_set(&context->meta) || !avif_box_is_set(&context->handler) ||
        !avif_box_is_set(&context->iinf)) {
        return avif_fail(context, AVIFDEC_INVALID_DATA, 0U,
                         AVIFDEC_FOURCC('m', 'e', 't', 'a'));
    }
    version = avif_full_box_version(context->data, &context->meta, &valid);
    if (!valid || version != 0U) {
        return avif_fail(context, valid ? AVIFDEC_UNSUPPORTED : AVIFDEC_TRUNCATED,
                         context->meta.offset, context->meta.type);
    }
    payload = context->data + context->handler.payload_offset;
    if (context->handler.payload_size < 12U || payload[0] != 0U ||
        avifdec_load_u32be(payload + 8U) != AVIFDEC_FOURCC('p', 'i', 'c', 't')) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->handler.offset, context->handler.type);
    }
    version = avif_full_box_version(context->data, &context->iinf, &valid);
    if (!valid || version > 1U) {
        return avif_fail(context, valid ? AVIFDEC_UNSUPPORTED : AVIFDEC_TRUNCATED,
                         context->iinf.offset, context->iinf.type);
    }
    payload = context->data + context->iinf.payload_offset + 4U;
    if (context->iinf.payload_size < (version == 0U ? 6U : 8U)) {
        return avif_fail(context, AVIFDEC_TRUNCATED,
                         context->iinf.payload_offset, context->iinf.type);
    }
    declared_item_count = version == 0U ? avifdec_load_u16be(payload)
                                        : avifdec_load_u32be(payload);
    if (declared_item_count != context->item_count) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iinf.offset, context->iinf.type);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_parse_location(AvifContext *context,
                                  uint32_t primary_id,
                                  AvifLocation *location) {
    AvifdecByteReader reader;
    uint8_t version;
    uint8_t sizes;
    uint8_t sizes2;
    unsigned int offset_size;
    unsigned int length_size;
    unsigned int base_offset_size;
    unsigned int index_size;
    uint32_t item_count;
    uint32_t item_index;
    int valid;

    if (context->item_index != 0) {
        AvifItemIndexLocation indexed_location;
        AvifdecStatus status = avif_item_index_find_location(
            context->item_index, primary_id,
            &indexed_location, context->error);
        size_t extent_index;

        if (status != AVIFDEC_OK) return status;
        avifdec_memory_fill(location, 0U, sizeof(*location));
        location->item_id = indexed_location.item_id;
        location->construction_method =
            indexed_location.construction_method;
        location->base_offset = indexed_location.base_offset;
        location->extent_count = indexed_location.extent_count;
        for (extent_index = 0U;
             extent_index < indexed_location.extent_count;
             ++extent_index) {
            location->extents[extent_index].offset =
                indexed_location.extents[extent_index].offset;
            location->extents[extent_index].length =
                indexed_location.extents[extent_index].length;
        }
        return AVIFDEC_OK;
    }
    if (!avif_box_is_set(&context->iloc)) {
        return avif_fail(context, AVIFDEC_INVALID_DATA, 0U, AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
    }
    version = avif_full_box_version(context->data, &context->iloc, &valid);
    if (!valid || version > 2U) {
        return avif_fail(context, valid ? AVIFDEC_UNSUPPORTED : AVIFDEC_TRUNCATED,
                         context->iloc.offset, context->iloc.type);
    }
    avifdec_byte_reader_init(&reader,
                             context->data + context->iloc.payload_offset + 4U,
                             context->iloc.payload_size - 4U,
                             context->iloc.payload_offset + 4U);
    sizes = avifdec_byte_reader_u8(&reader);
    sizes2 = avifdec_byte_reader_u8(&reader);
    offset_size = sizes >> 4;
    length_size = sizes & 15U;
    base_offset_size = sizes2 >> 4;
    index_size = version == 0U ? 0U : sizes2 & 15U;
    if (offset_size > 8U || length_size > 8U || base_offset_size > 8U || index_size > 8U) {
        return avif_fail(context, AVIFDEC_UNSUPPORTED, context->iloc.payload_offset + 4U,
                         context->iloc.type);
    }
    item_count = version < 2U ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                              : avifdec_byte_reader_u32be(&reader);
    if (item_count > context->limits.max_items || item_count > AVIF_MAX_ITEMS) {
        return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED, avifdec_byte_reader_offset(&reader),
                         context->iloc.type);
    }
    avifdec_memory_fill(location, 0U, sizeof(*location));
    for (item_index = 0U; item_index < item_count; ++item_index) {
        uint32_t item_id = version < 2U ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                                        : avifdec_byte_reader_u32be(&reader);
        uint8_t construction_method = 0U;
        uint64_t base_offset = 0U;
        uint16_t extent_count;
        uint16_t extent_index;

        if (version > 0U) {
            uint16_t method_field = avifdec_byte_reader_u16be(&reader);

            if ((method_field & 0xfff0U) != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 avifdec_byte_reader_offset(&reader) - 2U, context->iloc.type);
            }
            construction_method = (uint8_t)(method_field & 15U);
        }
        if (avifdec_byte_reader_u16be(&reader) != 0U) {
            return avif_fail(context, AVIFDEC_UNSUPPORTED, avifdec_byte_reader_offset(&reader) - 2U,
                             context->iloc.type);
        }
        if (avif_read_sized_uint(&reader, base_offset_size, &base_offset) != AVIFDEC_OK) break;
        extent_count = avifdec_byte_reader_u16be(&reader);
        if (extent_count > context->limits.max_extents || extent_count > AVIF_MAX_EXTENTS) {
            return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                             avifdec_byte_reader_offset(&reader) - 2U, context->iloc.type);
        }
        if (item_id == primary_id) {
            if (extent_count == 0U || location->extent_count != 0U) {
                return avif_fail(context,
                                 extent_count == 0U ? AVIFDEC_INVALID_DATA : AVIFDEC_LIMIT_EXCEEDED,
                                 avifdec_byte_reader_offset(&reader) - 2U, context->iloc.type);
            }
            location->item_id = item_id;
            location->construction_method = construction_method;
            location->base_offset = base_offset;
            location->extent_count = extent_count;
        }
        for (extent_index = 0U; extent_index < extent_count; ++extent_index) {
            uint64_t ignored;
            uint64_t extent_offset = 0U;
            uint64_t extent_length = 0U;

            if (index_size != 0U && avif_read_sized_uint(&reader, index_size, &ignored) != AVIFDEC_OK) break;
            if (avif_read_sized_uint(&reader, offset_size, &extent_offset) != AVIFDEC_OK ||
                avif_read_sized_uint(&reader, length_size, &extent_length) != AVIFDEC_OK) break;
            if (item_id == primary_id) {
                location->extents[extent_index].offset = extent_offset;
                location->extents[extent_index].length = extent_length;
            }
        }
        if (reader.status != AVIFDEC_OK) break;
    }
    if (reader.status != AVIFDEC_OK) {
        return avif_fail(context, reader.status, avifdec_byte_reader_offset(&reader), context->iloc.type);
    }
    if (location->extent_count == 0U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA, context->iloc.offset, context->iloc.type);
    }
    if (location->construction_method > 1U) {
        return avif_fail(context, AVIFDEC_UNSUPPORTED, context->iloc.offset, context->iloc.type);
    }
    return AVIFDEC_OK;
}

int avif_find_item(const AvifContext *context, uint32_t item_id) {
    size_t index;

    for (index = 0U; index < context->item_count; ++index) {
        if (context->items[index].id == item_id) return (int)index;
    }
    return -1;
}

static AvifdecStatus avif_parse_associations(AvifContext *context) {
    size_t box_index;

    for (box_index = 0U; box_index < context->ipma_count; ++box_index) {
        const AvifdecBmffBox *box = &context->ipma[box_index];
        AvifdecByteReader reader;
        uint8_t version;
        uint32_t flags;
        uint32_t entry_count;
        uint32_t entry_index;
        size_t minimum_entry_size;
        int valid;

        version = avif_full_box_version(context->data, box, &valid);
        if (!valid || version > 1U) {
            return avif_fail(context, valid ? AVIFDEC_UNSUPPORTED : AVIFDEC_TRUNCATED,
                             box->offset, box->type);
        }
        flags = ((uint32_t)context->data[box->payload_offset + 1U] << 16) |
                ((uint32_t)context->data[box->payload_offset + 2U] << 8) |
                context->data[box->payload_offset + 3U];
        avifdec_byte_reader_init(&reader,
                                 context->data + box->payload_offset + 4U,
                                 box->payload_size - 4U,
                                 box->payload_offset + 4U);
        entry_count = avifdec_byte_reader_u32be(&reader);
        minimum_entry_size = version == 0U ? 3U : 5U;
        if (reader.status != AVIFDEC_OK ||
            entry_count >
                avifdec_byte_reader_remaining(&reader) /
                    minimum_entry_size) {
            return avif_fail(
                context, AVIFDEC_TRUNCATED,
                avifdec_byte_reader_offset(&reader), box->type);
        }
        for (entry_index = 0U;
             entry_index < entry_count && reader.status == AVIFDEC_OK;
             ++entry_index) {
            uint32_t item_id = version == 0U ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                                             : avifdec_byte_reader_u32be(&reader);
            uint8_t association_count = avifdec_byte_reader_u8(&reader);
            uint8_t association_index;

            if (reader.status != AVIFDEC_OK) break;
            for (association_index = 0U;
                 association_index < association_count &&
                     reader.status == AVIFDEC_OK;
                 ++association_index) {
                uint16_t value = (flags & 1U) != 0U ? avifdec_byte_reader_u16be(&reader)
                                                    : (uint16_t)avifdec_byte_reader_u8(&reader);
                unsigned int essential_bit = (flags & 1U) != 0U ? 15U : 7U;
                uint16_t property_index = (uint16_t)(value & ((1U << essential_bit) - 1U));

                if (reader.status != AVIFDEC_OK) break;
                if (property_index == 0U) continue;
                if (property_index > context->property_count) {
                    return avif_fail(context, AVIFDEC_INVALID_DATA,
                                     avifdec_byte_reader_offset(&reader), box->type);
                }
                if (avif_find_item(context, item_id) < 0) {
                    return avif_fail(context, AVIFDEC_INVALID_DATA,
                                     avifdec_byte_reader_offset(&reader), box->type);
                }
                {
                    size_t existing;

                    for (existing = 0U; existing < context->association_count; ++existing) {
                        if (context->associations[existing].item_id == item_id &&
                            context->associations[existing].property_index == property_index) {
                            return avif_fail(context, AVIFDEC_INVALID_DATA,
                                             avifdec_byte_reader_offset(&reader), box->type);
                        }
                    }
                }
                if (context->association_count >= AVIF_MAX_ASSOCIATIONS) {
                    return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                                     avifdec_byte_reader_offset(&reader), box->type);
                }
                context->associations[context->association_count].item_id = item_id;
                context->associations[context->association_count].property_index = property_index;
                context->associations[context->association_count].essential =
                    (uint8_t)((value >> essential_bit) & 1U);
                ++context->association_count;
            }
        }
        if (reader.status != AVIFDEC_OK) {
            return avif_fail(context, reader.status, avifdec_byte_reader_offset(&reader), box->type);
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_parse_references(AvifContext *context) {
    uint8_t version;
    size_t box_index;
    int valid;

    if (!avif_box_is_set(&context->iref)) return AVIFDEC_OK;
    version = avif_full_box_version(context->data, &context->iref, &valid);
    if (!valid || version > 1U) {
        return avif_fail(context, valid ? AVIFDEC_UNSUPPORTED : AVIFDEC_TRUNCATED,
                         context->iref.offset, context->iref.type);
    }
    for (box_index = 0U; box_index < context->reference_box_count; ++box_index) {
        const AvifdecBmffBox *box = &context->reference_boxes[box_index];
        AvifdecByteReader reader;
        uint32_t from_item_id;
        uint16_t count;
        uint16_t reference_index;

        avifdec_byte_reader_init(
            &reader, context->data + box->payload_offset,
            box->payload_size, box->payload_offset);
        from_item_id = version == 0U
            ? (uint32_t)avifdec_byte_reader_u16be(&reader)
            : avifdec_byte_reader_u32be(&reader);
        count = avifdec_byte_reader_u16be(&reader);
        if (count == 0U || avif_find_item(context, from_item_id) < 0) {
            return avif_fail(context, AVIFDEC_INVALID_DATA, box->offset, box->type);
        }
        for (reference_index = 0U; reference_index < count; ++reference_index) {
            uint32_t to_item_id = version == 0U
                ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                : avifdec_byte_reader_u32be(&reader);
            size_t existing;

            if (reader.status != AVIFDEC_OK) break;
            if (to_item_id == from_item_id ||
                avif_find_item(context, to_item_id) < 0) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 avifdec_byte_reader_offset(&reader), box->type);
            }
            for (existing = 0U; existing < context->reference_count; ++existing) {
                const AvifReference *reference = &context->references[existing];

                if (reference->from_item_id == from_item_id &&
                    reference->to_item_id == to_item_id &&
                    reference->type == box->type) {
                    return avif_fail(context, AVIFDEC_INVALID_DATA,
                                     avifdec_byte_reader_offset(&reader), box->type);
                }
            }
            if (context->reference_count >= AVIF_MAX_REFERENCES) {
                return avif_fail(context, AVIFDEC_LIMIT_EXCEEDED,
                                 avifdec_byte_reader_offset(&reader), box->type);
            }
            context->references[context->reference_count].from_item_id =
                from_item_id;
            context->references[context->reference_count].to_item_id =
                to_item_id;
            context->references[context->reference_count].type = box->type;
            ++context->reference_count;
        }
        if (reader.status != AVIFDEC_OK ||
            avifdec_byte_reader_remaining(&reader) != 0U) {
            return avif_fail(
                context,
                reader.status == AVIFDEC_OK
                    ? AVIFDEC_INVALID_DATA : reader.status,
                avifdec_byte_reader_offset(&reader), box->type);
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_visit_derived_item(AvifContext *context,
                                             size_t item_index,
                                             uint8_t *states) {
    size_t reference_index;

    if (states[item_index] == 1U) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iref.offset,
                         AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    }
    if (states[item_index] == 2U) return AVIFDEC_OK;
    states[item_index] = 1U;
    for (reference_index = 0U;
         reference_index < context->reference_count;
         ++reference_index) {
        const AvifReference *reference =
            &context->references[reference_index];
        int child_index;
        AvifdecStatus status;

        if (reference->type != AVIFDEC_FOURCC('d', 'i', 'm', 'g') ||
            reference->from_item_id != context->items[item_index].id) {
            continue;
        }
        child_index = avif_find_item(context, reference->to_item_id);
        if (child_index < 0) {
            return avif_fail(context, AVIFDEC_INVALID_DATA,
                             context->iref.offset, reference->type);
        }
        status = avif_visit_derived_item(
            context, (size_t)child_index, states);
        if (status != AVIFDEC_OK) return status;
    }
    states[item_index] = 2U;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_validate_derived_graph(AvifContext *context) {
    uint8_t states[AVIF_MAX_ITEMS];
    size_t item_index;

    avifdec_memory_fill(states, 0U, sizeof(states));
    for (item_index = 0U; item_index < context->item_count; ++item_index) {
        AvifdecStatus status = avif_visit_derived_item(
            context, item_index, states);

        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_append_span(AvifContext *context,
                                      AvifdecSpan *spans,
                                      size_t span_capacity,
                                      AvifdecImageInfo *info,
                                      size_t file_offset,
                                      size_t length) {
    size_t payload_size;

    if (length == 0U) return avif_fail(context, AVIFDEC_INVALID_DATA, file_offset, context->iloc.type);
    if (!avifdec_size_add(info->payload_size, length, &payload_size)) {
        return avif_fail(context, AVIFDEC_OVERFLOW, file_offset, context->iloc.type);
    }
    if (spans != 0 && info->extent_count < span_capacity) {
        spans[info->extent_count].data = context->data + file_offset;
        spans[info->extent_count].size = length;
        spans[info->extent_count].file_offset = file_offset;
    }
    info->payload_size = payload_size;
    ++info->extent_count;
    return AVIFDEC_OK;
}

AvifdecStatus avif_resolve_extents(AvifContext *context,
                                   const AvifLocation *location,
                                   AvifdecSpan *spans,
                                   size_t span_capacity,
                                   AvifdecImageInfo *info) {
    size_t extent_index;

    if (context->item_index != 0) {
        AvifItemPayload payload;
        AvifdecStatus status = avif_item_index_resolve_item(
            context->item_index, location->item_id, spans,
            span_capacity, &payload, context->error);

        if (status != AVIFDEC_OK) return status;
        info->payload_size = payload.payload_size;
        info->extent_count = payload.span_count;
        return AVIFDEC_OK;
    }
    for (extent_index = 0U; extent_index < location->extent_count; ++extent_index) {
        const AvifExtent *extent = &location->extents[extent_index];
        uint64_t logical_offset;
        uint64_t remaining = extent->length;
        size_t data_index;

        if (UINT64_MAX - location->base_offset < extent->offset) {
            return avif_fail(context, AVIFDEC_OVERFLOW, context->iloc.offset, context->iloc.type);
        }
        logical_offset = location->base_offset + extent->offset;
        if (location->construction_method == 0U) {
            uint64_t end;
            int found = 0;

            if (UINT64_MAX - logical_offset < remaining) {
                return avif_fail(context, AVIFDEC_OVERFLOW, context->iloc.offset, context->iloc.type);
            }
            end = logical_offset + remaining;
            for (data_index = 0U; data_index < context->data_box_count; ++data_index) {
                const AvifdecBmffBox *box = &context->data_boxes[data_index];
                uint64_t box_start;
                uint64_t box_end;

                if (box->type != AVIFDEC_FOURCC('m', 'd', 'a', 't')) continue;
                box_start = box->payload_offset;
                box_end = box_start + box->payload_size;
                if (logical_offset >= box_start && end <= box_end) {
                    found = 1;
                    break;
                }
            }
            if (!found || end > context->size || logical_offset > SIZE_MAX || remaining > SIZE_MAX) {
                return avif_fail(context, AVIFDEC_INVALID_DATA, context->iloc.offset, context->iloc.type);
            }
            if (avif_append_span(context, spans, span_capacity, info,
                                 (size_t)logical_offset, (size_t)remaining) != AVIFDEC_OK) {
                return context->error == 0 ? AVIFDEC_INVALID_DATA : context->error->status;
            }
        } else {
            uint64_t stream_position = 0U;

            for (data_index = 0U; data_index < context->data_box_count && remaining != 0U; ++data_index) {
                const AvifdecBmffBox *box = &context->data_boxes[data_index];
                uint64_t box_end;
                uint64_t local_offset;
                uint64_t available;
                uint64_t take;

                if (box->type != AVIFDEC_FOURCC('i', 'd', 'a', 't')) continue;
                box_end = stream_position + box->payload_size;
                if (logical_offset >= box_end) {
                    stream_position = box_end;
                    continue;
                }
                local_offset = logical_offset > stream_position ? logical_offset - stream_position : 0U;
                available = box->payload_size - local_offset;
                take = remaining < available ? remaining : available;
                if (take != 0U && avif_append_span(context, spans, span_capacity, info,
                                                   box->payload_offset + (size_t)local_offset,
                                                   (size_t)take) != AVIFDEC_OK) {
                    return context->error == 0 ? AVIFDEC_INVALID_DATA : context->error->status;
                }
                remaining -= take;
                logical_offset += take;
                stream_position = box_end;
            }
            if (remaining != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA, context->iloc.offset, context->iloc.type);
            }
        }
    }
    if (spans != 0 && info->extent_count > span_capacity) {
        return avif_fail(context, AVIFDEC_OUT_OF_MEMORY, context->iloc.offset, context->iloc.type);
    }
    return AVIFDEC_OK;
}

static AvifdecLimits avif_effective_limits(const AvifdecLimits *limits) {
    return avifdec_limits_effective(limits);
}

AvifdecStatus avif_open_context(AvifContext *context,
                                const void *data,
                                size_t size,
                                const AvifdecLimits *limits,
                                uint32_t *primary_id,
                                AvifdecError *error) {
    AvifdecBmffInfo bmff_info;
    AvifdecBmffLimits bmff_limits = { 32U, 100000U };
    AvifdecStatus status;

    avifdec_memory_fill(context, 0U, sizeof(*context));
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
    context->data = (const unsigned char *)data;
    context->size = size;
    context->limits = avif_effective_limits(limits);
    context->error = error;
    status = avifdec_bmff_inspect(
        data, size, &bmff_limits, avif_collect_box, context,
        &bmff_info, error);
    if (status != AVIFDEC_OK) return status;
    if (context->failed) {
        return error == 0 ? AVIFDEC_INVALID_DATA : error->status;
    }
    if (!bmff_info.has_avif_brand) {
        return avif_fail(context, AVIFDEC_INVALID_DATA, 0U,
                         AVIFDEC_FOURCC('f', 't', 'y', 'p'));
    }
    status = avif_validate_meta(context);
    if (status != AVIFDEC_OK) return status;
    status = avif_parse_primary_id(context, primary_id);
    if (status != AVIFDEC_OK) return status;
    if (avif_find_item(context, *primary_id) < 0) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->pitm.offset,
                         AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    status = avif_parse_associations(context);
    if (status != AVIFDEC_OK) return status;
    status = avif_parse_references(context);
    if (status != AVIFDEC_OK) return status;
    return avif_validate_derived_graph(context);
}
