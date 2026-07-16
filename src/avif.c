#include "avifdec.h"
#include "av1.h"
#include "avif_sato.h"
#include "base.h"
#include "bmff.h"

#define AVIF_MAX_ITEMS AVIFDEC_DEFAULT_MAX_ITEMS
#define AVIF_MAX_EXTENTS AVIFDEC_DEFAULT_MAX_EXTENTS
#define AVIF_MAX_PROPERTIES AVIFDEC_DEFAULT_MAX_PROPERTIES
#define AVIF_MAX_ASSOCIATIONS (AVIF_MAX_ITEMS * 16U)
#define AVIF_MAX_REFERENCE_BOXES AVIF_MAX_ITEMS
#define AVIF_MAX_REFERENCES (AVIF_MAX_ITEMS * 4U)
#define AVIF_MAX_DATA_BOXES 16U
#define AVIF_MAX_RESOLVED_SPANS (AVIF_MAX_EXTENTS * AVIF_MAX_DATA_BOXES)
#define AVIF_MAX_DERIVATION_DEPTH 16U
#define AVIF_WORKSPACE_BASE_ALIGNMENT 16U

typedef struct {
    uint32_t id;
    uint32_t type;
} AvifItem;

typedef struct {
    uint64_t offset;
    uint64_t length;
} AvifExtent;

typedef struct {
    uint32_t item_id;
    uint8_t construction_method;
    uint64_t base_offset;
    size_t extent_count;
    AvifExtent extents[AVIF_MAX_EXTENTS];
} AvifLocation;

typedef struct {
    AvifdecBmffBox box;
    uint32_t type;
} AvifProperty;

typedef struct {
    uint32_t item_id;
    uint16_t property_index;
    uint8_t essential;
} AvifAssociation;

typedef struct {
    uint32_t from_item_id;
    uint32_t to_item_id;
    uint32_t type;
} AvifReference;

typedef struct {
    const unsigned char *data;
    size_t size;
    AvifdecLimits limits;
    AvifdecError *error;
    AvifdecBmffBox meta;
    AvifdecBmffBox handler;
    AvifdecBmffBox pitm;
    AvifdecBmffBox iloc;
    AvifdecBmffBox iinf;
    AvifdecBmffBox iref;
    AvifdecBmffBox ipco;
    AvifdecBmffBox ipma[16];
    size_t ipma_count;
    AvifdecBmffBox reference_boxes[AVIF_MAX_REFERENCE_BOXES];
    size_t reference_box_count;
    AvifdecBmffBox data_boxes[AVIF_MAX_DATA_BOXES];
    size_t data_box_count;
    AvifItem items[AVIF_MAX_ITEMS];
    size_t item_count;
    AvifProperty properties[AVIF_MAX_PROPERTIES];
    size_t property_count;
    AvifAssociation associations[AVIF_MAX_ASSOCIATIONS];
    size_t association_count;
    AvifReference references[AVIF_MAX_REFERENCES];
    size_t reference_count;
    AvifdecSpan query_spans[AVIF_MAX_RESOLVED_SPANS];
    int failed;
} AvifContext;

static AvifdecStatus avif_fail(AvifContext *context,
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

static AvifdecStatus avif_parse_location(AvifContext *context,
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

static int avif_find_item(const AvifContext *context, uint32_t item_id) {
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
        for (entry_index = 0U; entry_index < entry_count; ++entry_index) {
            uint32_t item_id = version == 0U ? (uint32_t)avifdec_byte_reader_u16be(&reader)
                                             : avifdec_byte_reader_u32be(&reader);
            uint8_t association_count = avifdec_byte_reader_u8(&reader);
            uint8_t association_index;

            for (association_index = 0U; association_index < association_count; ++association_index) {
                uint16_t value = (flags & 1U) != 0U ? avifdec_byte_reader_u16be(&reader)
                                                    : (uint16_t)avifdec_byte_reader_u8(&reader);
                unsigned int essential_bit = (flags & 1U) != 0U ? 15U : 7U;
                uint16_t property_index = (uint16_t)(value & ((1U << essential_bit) - 1U));

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

static int avif_text_equal(const unsigned char *bytes,
                           size_t length,
                           const char *text) {
    size_t index = 0U;

    while (text[index] != '\0') {
        if (index >= length || bytes[index] != (unsigned char)text[index]) {
            return 0;
        }
        ++index;
    }
    return index == length;
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
    int item_index = avif_find_item(context, item_id);
    uint32_t item_type;
    size_t association_index;
    int seen_av1c = 0;
    int seen_ispe = 0;
    int seen_pixi = 0;
    int seen_nclx = 0;
    int seen_icc = 0;
    int seen_auxc = 0;
    unsigned int transform_stage = 0U;
    uint8_t pixel_bit_depth = 0U;

    if (item_index < 0) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->iinf.offset,
                         AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    item_type = context->items[item_index].type;
    info->primary_item_id = item_id;
    info->primary_item_type = item_type;
    info->selected_layer = 0xffU;

    for (association_index = 0U; association_index < context->association_count; ++association_index) {
        const AvifAssociation *association = &context->associations[association_index];
        const AvifProperty *property;
        const unsigned char *payload;
        size_t payload_size;

        if (association->item_id != item_id) continue;
        property = &context->properties[association->property_index - 1U];
        payload = context->data + property->box.payload_offset;
        payload_size = property->box.payload_size;
        if (property->type == AVIFDEC_FOURCC('a', 'v', '1', 'C')) {
            if (seen_av1c || payload_size < 4U || payload[0] != 0x81U || (payload[3] & 0xe0U) != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA, property->box.offset, property->type);
            }
            info->profile = payload[1] >> 5;
            info->level = payload[1] & 31U;
            info->tier = payload[2] >> 7;
            info->bit_depth = (payload[2] & 0x40U) == 0U ? 8U
                              : (payload[2] & 0x20U) == 0U ? 10U : 12U;
            info->monochrome = (payload[2] >> 4) & 1U;
            info->subsampling_x = (payload[2] >> 3) & 1U;
            info->subsampling_y = (payload[2] >> 2) & 1U;
            info->chroma_sample_position = payload[2] & 3U;
            seen_av1c = 1;
        } else if (property->type == AVIFDEC_FOURCC('i', 's', 'p', 'e')) {
            if (seen_ispe || payload_size != 12U || payload[0] != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA, property->box.offset, property->type);
            }
            info->width = avifdec_load_u32be(payload + 4U);
            info->height = avifdec_load_u32be(payload + 8U);
            if (info->width == 0U || info->height == 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA, property->box.offset, property->type);
            }
            seen_ispe = 1;
        } else if (property->type == AVIFDEC_FOURCC('p', 'i', 'x', 'i')) {
            size_t channel;

            if (seen_pixi || payload_size < 5U || payload[0] != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA, property->box.offset, property->type);
            }
            info->channel_count = payload[4U];
            if (info->channel_count == 0U || payload_size != 5U + info->channel_count) {
                return avif_fail(context, AVIFDEC_INVALID_DATA, property->box.offset, property->type);
            }
            for (channel = 0U; channel < info->channel_count; ++channel) {
                if (channel == 0U) pixel_bit_depth = payload[5U];
                if (payload[5U + channel] != pixel_bit_depth) {
                    return avif_fail(context, AVIFDEC_INVALID_DATA, property->box.offset, property->type);
                }
            }
            seen_pixi = 1;
        } else if (property->type == AVIFDEC_FOURCC('c', 'o', 'l', 'r')) {
            uint32_t color_type;

            if (payload_size < 4U) {
                return avif_fail(context, AVIFDEC_TRUNCATED, property->box.offset, property->type);
            }
            color_type = avifdec_load_u32be(payload);
            if (color_type == AVIFDEC_FOURCC('n', 'c', 'l', 'x')) {
                if (seen_nclx || payload_size != 11U ||
                    (payload[10U] & 0x7fU) != 0U) {
                    return avif_fail(context, AVIFDEC_INVALID_DATA, property->box.offset, property->type);
                }
                info->color_primaries = avifdec_load_u16be(payload + 4U);
                info->transfer_characteristics = avifdec_load_u16be(payload + 6U);
                info->matrix_coefficients = avifdec_load_u16be(payload + 8U);
                info->color_range = payload[10U] >> 7;
                info->has_nclx = 1U;
                seen_nclx = 1;
            } else if (color_type == AVIFDEC_FOURCC('r', 'I', 'C', 'C') ||
                       color_type == AVIFDEC_FOURCC('p', 'r', 'o', 'f')) {
                if (seen_icc) {
                    return avif_fail(context, AVIFDEC_INVALID_DATA,
                                     property->box.offset, property->type);
                }
                info->icc_data = payload + 4U;
                info->icc_size = payload_size - 4U;
                seen_icc = 1;
            } else if (association->essential) {
                return avif_fail(context, AVIFDEC_UNSUPPORTED, property->box.offset, property->type);
            }
        } else if (property->type == AVIFDEC_FOURCC('p', 'a', 's', 'p')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_PASP) != 0U ||
                payload_size != 8U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->pixel_aspect_h_spacing = avifdec_load_u32be(payload);
            info->pixel_aspect_v_spacing = avifdec_load_u32be(payload + 4U);
            if (info->pixel_aspect_h_spacing == 0U ||
                info->pixel_aspect_v_spacing == 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->transform_flags |= AVIFDEC_TRANSFORM_PASP;
        } else if (property->type == AVIFDEC_FOURCC('c', 'l', 'a', 'p')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U ||
                payload_size != 32U || !association->essential ||
                transform_stage != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->clean_aperture.width_n = avifdec_load_u32be(payload);
            info->clean_aperture.width_d = avifdec_load_u32be(payload + 4U);
            info->clean_aperture.height_n = avifdec_load_u32be(payload + 8U);
            info->clean_aperture.height_d = avifdec_load_u32be(payload + 12U);
            info->clean_aperture.horizontal_offset_n =
                (int32_t)avifdec_load_u32be(payload + 16U);
            info->clean_aperture.horizontal_offset_d =
                avifdec_load_u32be(payload + 20U);
            info->clean_aperture.vertical_offset_n =
                (int32_t)avifdec_load_u32be(payload + 24U);
            info->clean_aperture.vertical_offset_d =
                avifdec_load_u32be(payload + 28U);
            info->transform_flags |= AVIFDEC_TRANSFORM_CLAP;
            transform_stage = 1U;
        } else if (property->type == AVIFDEC_FOURCC('i', 'r', 'o', 't')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U ||
                payload_size != 1U || (payload[0] & 0xfcU) != 0U ||
                !association->essential || transform_stage > 1U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->irot_angle = payload[0] & 3U;
            info->transform_flags |= AVIFDEC_TRANSFORM_IROT;
            transform_stage = 2U;
        } else if (property->type == AVIFDEC_FOURCC('i', 'm', 'i', 'r')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U ||
                payload_size != 1U || (payload[0] & 0xfeU) != 0U ||
                !association->essential || transform_stage > 2U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->imir_axis = payload[0] & 1U;
            info->transform_flags |= AVIFDEC_TRANSFORM_IMIR;
            transform_stage = 3U;
        } else if (property->type == AVIFDEC_FOURCC('a', 'u', 'x', 'C')) {
            size_t string_size;

            if (seen_auxc || payload_size < 5U || payload[0] != 0U ||
                payload[1] != 0U || payload[2] != 0U ||
                payload[3] != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            seen_auxc = 1;
            for (string_size = 0U;
                 4U + string_size < payload_size &&
                 payload[4U + string_size] != 0U;
                 ++string_size) {
            }
            if (4U + string_size >= payload_size) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            if (avif_text_equal(
                    payload + 4U, string_size,
                    "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha") ||
                avif_text_equal(
                    payload + 4U, string_size,
                    "urn:mpeg:hevc:2015:auxid:1")) {
                info->auxiliary_type = AVIFDEC_AUXILIARY_ALPHA;
            } else if (avif_text_equal(
                           payload + 4U, string_size,
                           "urn:mpeg:mpegB:cicp:systems:auxiliary:depth")) {
                info->auxiliary_type = AVIFDEC_AUXILIARY_DEPTH;
            }
        } else if (property->type == AVIFDEC_FOURCC('c', 'l', 'l', 'i')) {
            if (info->item_hdr_cll_present || payload_size != 4U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->item_hdr_cll.max_cll = avifdec_load_u16be(payload);
            info->item_hdr_cll.max_fall =
                avifdec_load_u16be(payload + 2U);
            info->item_hdr_cll_present = 1U;
        } else if (property->type == AVIFDEC_FOURCC('m', 'd', 'c', 'v')) {
            size_t primary;

            if (info->item_hdr_mdcv_present || payload_size != 24U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            for (primary = 0U; primary < 3U; ++primary) {
                info->item_hdr_mdcv.primary_x[primary] =
                    avifdec_load_u16be(payload + primary * 4U);
                info->item_hdr_mdcv.primary_y[primary] =
                    avifdec_load_u16be(payload + primary * 4U + 2U);
            }
            info->item_hdr_mdcv.white_point_x =
                avifdec_load_u16be(payload + 12U);
            info->item_hdr_mdcv.white_point_y =
                avifdec_load_u16be(payload + 14U);
            info->item_hdr_mdcv.luminance_max =
                avifdec_load_u32be(payload + 16U);
            info->item_hdr_mdcv.luminance_min =
                avifdec_load_u32be(payload + 20U);
            info->item_hdr_mdcv_present = 1U;
        } else if (property->type == AVIFDEC_FOURCC('a', '1', 'o', 'p')) {
            if (info->has_a1op || payload_size != 1U ||
                !association->essential) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->has_a1op = 1U;
            info->a1op_index = payload[0];
        } else if (property->type == AVIFDEC_FOURCC('l', 's', 'e', 'l')) {
            uint16_t layer_id;

            if (info->has_lsel || payload_size != 2U ||
                !association->essential) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            layer_id = avifdec_load_u16be(payload);
            if (layer_id > 3U && layer_id != 0xffffU) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->has_lsel = 1U;
            info->lsel_layer_id = layer_id;
            info->selected_layer =
                layer_id == 0xffffU ? 0xffU : (uint8_t)layer_id;
        } else if (property->type == AVIFDEC_FOURCC('a', '1', 'l', 'x')) {
            size_t layer;
            size_t field_size;
            int zero_seen = 0;

            if (info->is_layered || association->essential ||
                (payload_size != 7U &&
                                     payload_size != 13U) ||
                (payload[0] & 0xfeU) != 0U) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            field_size = (payload[0] & 1U) != 0U ? 4U : 2U;
            if (payload_size != 1U + 3U * field_size) {
                return avif_fail(context, AVIFDEC_INVALID_DATA,
                                 property->box.offset, property->type);
            }
            info->is_layered = 1U;
            info->layer_count = 1U;
            for (layer = 0U; layer < 3U; ++layer) {
                size_t layer_size = field_size == 2U
                    ? avifdec_load_u16be(payload + 1U + layer * field_size)
                    : avifdec_load_u32be(payload + 1U + layer * field_size);

                if (layer_size == 0U) {
                    zero_seen = 1;
                } else {
                    if (zero_seen) {
                        return avif_fail(context, AVIFDEC_INVALID_DATA,
                                         property->box.offset,
                                         property->type);
                    }
                    ++info->layer_count;
                }
                info->layer_sizes[layer] = layer_size;
            }
        } else if (association->essential) {
            return avif_fail(context, AVIFDEC_UNSUPPORTED, property->box.offset, property->type);
        }
    }
    if (!seen_ispe || !seen_pixi ||
        (item_type == AVIFDEC_FOURCC('a', 'v', '0', '1') &&
         !seen_av1c)) {
        return avif_fail(context, AVIFDEC_INVALID_DATA, context->ipco.offset, context->ipco.type);
    }
    if (seen_av1c &&
        (pixel_bit_depth != info->bit_depth ||
         info->channel_count != (info->monochrome ? 1U : 3U))) {
        return avif_fail(context, AVIFDEC_INVALID_DATA, context->ipco.offset,
                         AVIFDEC_FOURCC('p', 'i', 'x', 'i'));
    }
    if (!seen_av1c) {
        info->bit_depth = pixel_bit_depth;
        info->monochrome = info->channel_count == 1U;
    }
    info->crop.x = 0U;
    info->crop.y = 0U;
    info->crop.width = info->width;
    info->crop.height = info->height;
    if ((info->transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U &&
        !avifdec_clap_to_crop_rect(
            &info->clean_aperture, info->width, info->height,
            &info->crop)) {
        return avif_fail(context, AVIFDEC_INVALID_DATA,
                         context->ipco.offset,
                         AVIFDEC_FOURCC('c', 'l', 'a', 'p'));
    }
    info->presentation_width = info->crop.width;
    info->presentation_height = info->crop.height;
    if ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
        (info->irot_angle & 1U) != 0U) {
        uint32_t swap = info->presentation_width;

        info->presentation_width = info->presentation_height;
        info->presentation_height = swap;
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

static AvifdecStatus avif_resolve_extents(AvifContext *context,
                                          const AvifLocation *location,
                                          AvifdecSpan *spans,
                                          size_t span_capacity,
                                          AvifdecImageInfo *info) {
    size_t extent_index;

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
    AvifdecLimits result;

    result.max_width = limits == 0 || limits->max_width == 0U ? 32768U : limits->max_width;
    result.max_height = limits == 0 || limits->max_height == 0U ? 32768U : limits->max_height;
    result.max_pixels = limits == 0 || limits->max_pixels == 0U ? 268435456U : limits->max_pixels;
    result.max_items = limits == 0 || limits->max_items == 0U ? AVIFDEC_DEFAULT_MAX_ITEMS : limits->max_items;
    result.max_extents = limits == 0 || limits->max_extents == 0U ? AVIFDEC_DEFAULT_MAX_EXTENTS : limits->max_extents;
    result.max_properties = limits == 0 || limits->max_properties == 0U ? AVIFDEC_DEFAULT_MAX_PROPERTIES : limits->max_properties;
    result.max_obus = limits == 0 || limits->max_obus == 0U ? AVIFDEC_DEFAULT_MAX_OBUS : limits->max_obus;
    result.max_frames = limits == 0 || limits->max_frames == 0U ? AVIFDEC_DEFAULT_MAX_FRAMES : limits->max_frames;
    result.operating_point = limits == 0 ? 0U : limits->operating_point;
    result.av1_framing = limits == 0
        ? AVIFDEC_AV1_LOW_OVERHEAD : limits->av1_framing;
    result.spatial_layer = limits == 0 ? 0U : limits->spatial_layer;
    result.spatial_layer_set =
        limits == 0 ? 0U : limits->spatial_layer_set;
    return result;
}

static AvifdecStatus avif_open_context(AvifContext *context,
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
    status = avifdec_av1_query(
        spans, info->extent_count, &item_limits, info,
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
        if (status != AVIFDEC_OK ||
            !avifdec_size_add(
                tile_bytes, _Alignof(uint16_t) - 1U, &tile_bytes) ||
            !avifdec_size_add(
                tile_bytes, tile.workspace_required, &peak)) {
            return avif_fail(context, AVIFDEC_OVERFLOW,
                             context->ipco.offset, reference->type);
        }
        if (tile_bytes > tile_storage) tile_storage = tile_bytes;
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

static AvifdecStatus avif_query_item(AvifContext *context,
                                      uint32_t item_id,
                                      size_t depth,
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
            AVIF_MAX_RESOLVED_SPANS, info);
    }
    if (item_type == AVIFDEC_FOURCC('g', 'r', 'i', 'd')) {
        return avif_query_grid_item(
            context, item_id, depth, info);
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

static AvifdecStatus avif_decode_item(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
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
            AVIF_MAX_RESOLVED_SPANS, &info);

        if (status != AVIFDEC_OK) return status;
        if (info.has_a1op) {
            item_limits.operating_point = info.a1op_index;
        }
        if (info.has_lsel && info.selected_layer != 0xffU) {
            item_limits.spatial_layer = info.selected_layer;
            item_limits.spatial_layer_set = 1U;
        }
        return avifdec_av1_decode(
            context->query_spans, info.extent_count,
            &item_limits, &info, workspace, workspace_size,
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
            context, item_id, depth, &grid_info);

        if (status != AVIFDEC_OK) return status;
        status = avif_validate_output_image(&grid_info, image);
        if (status != AVIFDEC_OK) return status;
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
                context, reference->to_item_id, depth + 1U,
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
        image->widths[0] = grid_info.width;
        image->heights[0] = grid_info.height;
        image->bit_depth = grid_info.bit_depth;
        image->monochrome = grid_info.monochrome;
        image->subsampling_x = grid_info.subsampling_x;
        image->subsampling_y = grid_info.subsampling_y;
        if (!grid_info.monochrome) {
            image->widths[1] =
                (grid_info.width +
                 ((uint32_t)1U << grid_info.subsampling_x) - 1U) >>
                grid_info.subsampling_x;
            image->widths[2] = image->widths[1];
            image->heights[1] =
                (grid_info.height +
                 ((uint32_t)1U << grid_info.subsampling_y) - 1U) >>
                grid_info.subsampling_y;
            image->heights[2] = image->heights[1];
        }
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
                context, input_ids[input_index], depth + 1U,
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
            unsigned int plane_count =
                sato_info.monochrome ? 1U : 3U;
            unsigned int plane;

            for (plane = 0U; plane < plane_count; ++plane) {
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
                uint32_t y;

                if (!sato_info.color_range) {
                    unsigned int shift =
                        sato_info.bit_depth - 8U;

                    output_minimum = (int64_t)16 << shift;
                    output_maximum =
                        (int64_t)(plane == 0U ? 235U : 240U)
                        << shift;
                }
                for (y = 0U; y < plane_height; ++y) {
                    uint32_t x;

                    for (x = 0U; x < plane_width; ++x) {
                        int64_t samples[AVIF_SATO_MAX_INPUTS];
                        int64_t result;

                        for (input_index = 0U;
                             input_index < input_count;
                             ++input_index) {
                            samples[input_index] =
                                input_images[input_index]
                                    .planes[plane][
                                        (size_t)y *
                                        input_images[input_index]
                                            .strides[plane] +
                                        x];
                        }
                        status = avif_sato_evaluate(
                            &program, samples, input_count,
                            output_minimum, output_maximum,
                            &result);
                        if (status != AVIFDEC_OK) return status;
                        image->planes[plane][
                            (size_t)y * image->strides[plane] + x] =
                            (uint16_t)result;
                    }
                }
                image->widths[plane] = plane_width;
                image->heights[plane] = plane_height;
            }
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
            depth + 1U, workspace, workspace_size,
            image, trace);
    }
    return avif_fail(context, AVIFDEC_UNSUPPORTED,
                     context->iinf.offset, item_type);
}

static AvifdecStatus avif_trace_item(
    AvifContext *context,
    uint32_t item_id,
    size_t depth,
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
            AVIF_MAX_RESOLVED_SPANS, &info);

        if (status != AVIFDEC_OK) return status;
        if (info.has_a1op) {
            item_limits.operating_point = info.a1op_index;
        }
        if (info.has_lsel && info.selected_layer != 0xffU) {
            item_limits.spatial_layer = info.selected_layer;
            item_limits.spatial_layer_set = 1U;
        }
        return avifdec_av1_trace(
            context->query_spans, info.extent_count,
            &item_limits, &info, workspace, workspace_size,
            trace, context->error);
    }
    if (item_type == AVIFDEC_FOURCC('g', 'r', 'i', 'd')) {
        AvifdecImageInfo grid_info;
        size_t reference_index;
        AvifdecStatus status = avif_query_grid_item(
            context, item_id, depth, &grid_info);

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
                workspace, workspace_size, &child);
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
                workspace, workspace_size, &child);
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
            depth + 1U, workspace, workspace_size, trace);
    }
    return avif_fail(context, AVIFDEC_UNSUPPORTED,
                     context->iinf.offset, item_type);
}

AvifdecStatus avifdec_query(const void *data,
                            size_t size,
                            const AvifdecLimits *limits,
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

    if (info == 0 || (data == 0 && size != 0U) || (spans == 0 && span_capacity != 0U)) {
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
            AVIF_MAX_RESOLVED_SPANS, info);
    } else {
        status = avif_query_item(&context, primary_id, 0U, info);
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

AvifdecStatus avifdec_trace(const void *data,
                            size_t size,
                            const AvifdecLimits *limits,
                            void *workspace,
                            size_t workspace_size,
                            AvifdecEntropyTrace *trace,
                            AvifdecError *error) {
    AvifdecImageInfo info;
    AvifContext context;
    uint32_t primary_id;
    AvifdecStatus status;

    if (trace == 0 || (data == 0 && size != 0U) ||
        (workspace == 0 && workspace_size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = avifdec_query(data, size, limits, 0, 0U, &info, error);
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < info.workspace_required) return AVIFDEC_OUT_OF_MEMORY;
    status = avif_open_context(
        &context, data, size, limits, &primary_id, error);
    if (status != AVIFDEC_OK) return status;
    return avif_trace_item(
        &context, primary_id, 0U, workspace, workspace_size, trace);
}

AvifdecStatus avifdec_decode(const void *data,
                             size_t size,
                             const AvifdecLimits *limits,
                             void *workspace,
                             size_t workspace_size,
                             AvifdecImage *image,
                             AvifdecEntropyTrace *trace,
                             AvifdecError *error) {
    AvifdecImageInfo info;
    AvifContext context;
    uint32_t primary_id;
    AvifdecStatus status;

    if (image == 0 || (data == 0 && size != 0U) ||
        (workspace == 0 && workspace_size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = avifdec_query(data, size, limits, 0, 0U, &info, error);
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < info.workspace_required) return AVIFDEC_OUT_OF_MEMORY;
    status = avif_open_context(
        &context, data, size, limits, &primary_id, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_decode_item(
        &context, primary_id, 0U, workspace, workspace_size,
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
            &context, info.alpha_item_id, 0U,
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