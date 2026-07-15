#include "bmff.h"

#define BMFF_DEFAULT_MAX_DEPTH 32U
#define BMFF_DEFAULT_MAX_BOXES 100000U

typedef struct {
    const unsigned char *data;
    size_t size;
    AvifdecBmffLimits limits;
    AvifdecBmffVisitor visitor;
    void *user_data;
    AvifdecBmffInfo *info;
    AvifdecError *error;
    int seen_file_type;
} BmffContext;

static AvifdecStatus bmff_fail(BmffContext *context,
                               AvifdecStatus status,
                               size_t offset,
                               uint32_t box_type) {
    if (context->error != 0 && context->error->status == AVIFDEC_OK) {
        context->error->status = status;
        context->error->offset = offset;
        context->error->context = box_type;
    }
    return status;
}

static int bmff_is_avif_brand(uint32_t brand) {
    return brand == AVIFDEC_FOURCC('a', 'v', 'i', 'f') ||
           brand == AVIFDEC_FOURCC('a', 'v', 'i', 's');
}

static AvifdecStatus bmff_parse_file_type(BmffContext *context,
                                          const AvifdecBmffBox *box) {
    const unsigned char *payload = context->data + box->payload_offset;
    size_t brand_count;
    size_t index;

    if (context->seen_file_type || box->depth != 0U || box->payload_size < 8U ||
        ((box->payload_size - 8U) & 3U) != 0U) {
        return bmff_fail(context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    brand_count = (box->payload_size - 8U) / 4U;
    if (brand_count > AVIFDEC_BMFF_MAX_BRANDS) {
        return bmff_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, box->type);
    }

    context->seen_file_type = 1;
    context->info->major_brand = avifdec_load_u32be(payload);
    context->info->minor_version = avifdec_load_u32be(payload + 4U);
    context->info->compatible_brand_count = brand_count;
    context->info->has_avif_brand = bmff_is_avif_brand(context->info->major_brand);
    for (index = 0U; index < brand_count; ++index) {
        uint32_t brand = avifdec_load_u32be(payload + 8U + index * 4U);
        context->info->compatible_brands[index] = brand;
        if (bmff_is_avif_brand(brand)) context->info->has_avif_brand = 1;
    }
    return AVIFDEC_OK;
}

static int bmff_direct_container(uint32_t type) {
    return type == AVIFDEC_FOURCC('i', 'p', 'r', 'p') ||
           type == AVIFDEC_FOURCC('i', 'p', 'c', 'o') ||
           type == AVIFDEC_FOURCC('d', 'i', 'n', 'f') ||
           type == AVIFDEC_FOURCC('m', 'o', 'o', 'v') ||
           type == AVIFDEC_FOURCC('t', 'r', 'a', 'k') ||
           type == AVIFDEC_FOURCC('m', 'd', 'i', 'a') ||
           type == AVIFDEC_FOURCC('m', 'i', 'n', 'f') ||
           type == AVIFDEC_FOURCC('s', 't', 'b', 'l') ||
           type == AVIFDEC_FOURCC('e', 'd', 't', 's') ||
           type == AVIFDEC_FOURCC('u', 'd', 't', 'a') ||
           type == AVIFDEC_FOURCC('g', 'r', 'p', 'l');
}

static AvifdecStatus bmff_container_prefix(BmffContext *context,
                                           const AvifdecBmffBox *box,
                                           size_t *prefix,
                                           int *is_container) {
    const unsigned char *payload = context->data + box->payload_offset;

    *prefix = 0U;
    *is_container = 1;
    if (box->type == AVIFDEC_FOURCC('m', 'e', 't', 'a') ||
        box->type == AVIFDEC_FOURCC('i', 'r', 'e', 'f')) {
        *prefix = 4U;
    } else if (box->type == AVIFDEC_FOURCC('i', 'i', 'n', 'f')) {
        if (box->payload_size < 4U) {
            return bmff_fail(context, AVIFDEC_TRUNCATED, box->payload_offset, box->type);
        }
        *prefix = payload[0] == 0U ? 6U : 8U;
    } else if (!bmff_direct_container(box->type)) {
        *is_container = 0;
        return AVIFDEC_OK;
    }
    if (*prefix > box->payload_size) {
        return bmff_fail(context, AVIFDEC_TRUNCATED, box->payload_offset, box->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus bmff_parse_range(BmffContext *context,
                                      size_t start,
                                      size_t end,
                                      size_t depth) {
    size_t offset = start;

    while (offset < end) {
        AvifdecBmffBox box;
        uint32_t size32;
        uint64_t declared_size;
        size_t remaining = end - offset;
        size_t prefix;
        int is_container;
        AvifdecStatus status;

        if (remaining < 8U) {
            return bmff_fail(context, AVIFDEC_TRUNCATED, offset, 0U);
        }
        avifdec_memory_fill(&box, 0U, sizeof(box));
        size32 = avifdec_load_u32be(context->data + offset);
        box.type = avifdec_load_u32be(context->data + offset + 4U);
        box.offset = offset;
        box.depth = depth;
        box.header_size = 8U;
        declared_size = (uint64_t)size32;

        if (size32 == 1U) {
            if (remaining < 16U) {
                return bmff_fail(context, AVIFDEC_TRUNCATED, offset, box.type);
            }
            declared_size = avifdec_load_u64be(context->data + offset + 8U);
            box.header_size = 16U;
        } else if (size32 == 0U) {
            declared_size = (uint64_t)remaining;
        }

        if (box.type == AVIFDEC_FOURCC('u', 'u', 'i', 'd')) {
            if (box.header_size > SIZE_MAX - 16U) {
                return bmff_fail(context, AVIFDEC_OVERFLOW, offset, box.type);
            }
            box.header_size += 16U;
        }
        if (declared_size > (uint64_t)SIZE_MAX) {
            return bmff_fail(context, AVIFDEC_OVERFLOW, offset, box.type);
        }
        box.size = (size_t)declared_size;
        if (box.size < box.header_size) {
            return bmff_fail(context, AVIFDEC_INVALID_DATA, offset, box.type);
        }
        if (box.size > remaining) {
            return bmff_fail(context, AVIFDEC_TRUNCATED, offset, box.type);
        }
        box.payload_offset = offset + box.header_size;
        box.payload_size = box.size - box.header_size;
        if (box.type == AVIFDEC_FOURCC('u', 'u', 'i', 'd')) {
            size_t user_type_offset = offset + box.header_size - 16U;
            avifdec_memory_copy(box.user_type, context->data + user_type_offset, 16U);
            box.has_user_type = 1;
        }

        if (depth == 0U && offset == 0U && box.type != AVIFDEC_FOURCC('f', 't', 'y', 'p')) {
            return bmff_fail(context, AVIFDEC_INVALID_DATA, offset, box.type);
        }
        if (context->info->box_count >= context->limits.max_boxes) {
            return bmff_fail(context, AVIFDEC_LIMIT_EXCEEDED, offset, box.type);
        }
        ++context->info->box_count;
        if (depth > context->info->maximum_depth) context->info->maximum_depth = depth;
        if (box.type == AVIFDEC_FOURCC('m', 'e', 't', 'a')) ++context->info->meta_count;
        if (box.type == AVIFDEC_FOURCC('h', 'd', 'l', 'r')) ++context->info->handler_count;
        if (box.type == AVIFDEC_FOURCC('m', 'd', 'a', 't')) ++context->info->media_data_count;

        if (box.type == AVIFDEC_FOURCC('f', 't', 'y', 'p')) {
            status = bmff_parse_file_type(context, &box);
            if (status != AVIFDEC_OK) return status;
        }
        if (context->visitor != 0) context->visitor(&box, context->user_data);

        status = bmff_container_prefix(context, &box, &prefix, &is_container);
        if (status != AVIFDEC_OK) return status;
        if (is_container && box.payload_size > prefix) {
            size_t child_start = box.payload_offset + prefix;
            size_t child_end = box.payload_offset + box.payload_size;

            if (depth >= context->limits.max_depth) {
                return bmff_fail(context, AVIFDEC_LIMIT_EXCEEDED, child_start, box.type);
            }
            status = bmff_parse_range(context, child_start, child_end, depth + 1U);
            if (status != AVIFDEC_OK) return status;
        }
        offset += box.size;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_bmff_inspect(const void *data,
                                   size_t size,
                                   const AvifdecBmffLimits *limits,
                                   AvifdecBmffVisitor visitor,
                                   void *user_data,
                                   AvifdecBmffInfo *info,
                                   AvifdecError *error) {
    BmffContext context;
    AvifdecStatus status;

    if (info == 0 || (data == 0 && size != 0U)) return AVIFDEC_INVALID_ARGUMENT;
    avifdec_memory_fill(info, 0U, sizeof(*info));
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
    context.data = (const unsigned char *)data;
    context.size = size;
    context.limits.max_depth = limits == 0 ? BMFF_DEFAULT_MAX_DEPTH : limits->max_depth;
    context.limits.max_boxes = limits == 0 ? BMFF_DEFAULT_MAX_BOXES : limits->max_boxes;
    context.visitor = visitor;
    context.user_data = user_data;
    context.info = info;
    context.error = error;
    context.seen_file_type = 0;

    if (size == 0U) return bmff_fail(&context, AVIFDEC_TRUNCATED, 0U, 0U);
    status = bmff_parse_range(&context, 0U, size, 0U);
    if (status != AVIFDEC_OK) return status;
    if (!context.seen_file_type) return bmff_fail(&context, AVIFDEC_INVALID_DATA, 0U, 0U);
    return AVIFDEC_OK;
}