#include "bmff_child.h"

#include "base.h"

#define AVIF_BMFF_UUID \
    (((uint32_t)'u' << 24) | ((uint32_t)'u' << 16) | \
     ((uint32_t)'i' << 8) | (uint32_t)'d')

static AvifdecStatus avif_bmff_child_fail(
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

AvifdecStatus avif_bmff_child_iterator_init(
    AvifBmffChildIterator *iterator,
    const void *data,
    size_t data_size,
    size_t payload_offset,
    size_t payload_size,
    uint32_t parent_type,
    AvifdecError *error) {
    size_t parent_end;

    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
    if (iterator == 0 || (data == 0 && data_size != 0U)) {
        return avif_bmff_child_fail(
            AVIFDEC_INVALID_ARGUMENT, payload_offset, parent_type, error);
    }
    avifdec_memory_fill(iterator, 0U, sizeof(*iterator));
    if (!avifdec_size_add(payload_offset, payload_size, &parent_end)) {
        return avif_bmff_child_fail(
            AVIFDEC_OVERFLOW, payload_offset, parent_type, error);
    }
    if (payload_offset > data_size || parent_end > data_size) {
        return avif_bmff_child_fail(
            AVIFDEC_TRUNCATED, payload_offset, parent_type, error);
    }
    iterator->data = (const unsigned char *)data;
    iterator->data_size = data_size;
    iterator->next_offset = payload_offset;
    iterator->parent_end = parent_end;
    iterator->parent_type = parent_type;
    return AVIFDEC_OK;
}

AvifdecStatus avif_bmff_child_next(
    AvifBmffChildIterator *iterator,
    AvifBmffChild *child,
    int *has_child,
    AvifdecError *error) {
    const unsigned char *bytes;
    uint32_t size32;
    uint64_t declared_size;
    size_t remaining;
    size_t header_size = 8U;
    size_t box_size;

    if (iterator == 0 || child == 0 || has_child == 0 ||
        (iterator->data == 0 &&
         iterator->next_offset != iterator->parent_end) ||
        iterator->next_offset > iterator->parent_end ||
        iterator->parent_end > iterator->data_size) {
        return avif_bmff_child_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    avifdec_memory_fill(child, 0U, sizeof(*child));
    *has_child = 0;
    if (iterator->next_offset == iterator->parent_end) return AVIFDEC_OK;
    if (iterator->saw_to_end_child) {
        return avif_bmff_child_fail(
            AVIFDEC_INVALID_DATA, iterator->next_offset,
            iterator->parent_type, error);
    }
    remaining = iterator->parent_end - iterator->next_offset;
    if (remaining < 8U) {
        return avif_bmff_child_fail(
            AVIFDEC_TRUNCATED, iterator->next_offset,
            iterator->parent_type, error);
    }
    bytes = iterator->data + iterator->next_offset;
    size32 = avifdec_load_u32be(bytes);
    child->type = avifdec_load_u32be(bytes + 4U);
    declared_size = size32;
    if (size32 == 1U) {
        if (remaining < 16U) {
            return avif_bmff_child_fail(
                AVIFDEC_TRUNCATED, iterator->next_offset,
                child->type, error);
        }
        declared_size = avifdec_load_u64be(bytes + 8U);
        header_size = 16U;
    } else if (size32 == 0U) {
        declared_size = remaining;
        child->extends_to_parent_end = 1U;
    }
    if (child->type == AVIF_BMFF_UUID) {
        if (header_size > SIZE_MAX - 16U) {
            return avif_bmff_child_fail(
                AVIFDEC_OVERFLOW, iterator->next_offset,
                child->type, error);
        }
        header_size += 16U;
    }
    if (declared_size > (uint64_t)SIZE_MAX) {
        return avif_bmff_child_fail(
            AVIFDEC_OVERFLOW, iterator->next_offset, child->type, error);
    }
    box_size = (size_t)declared_size;
    if (box_size < header_size) {
        return avif_bmff_child_fail(
            AVIFDEC_INVALID_DATA, iterator->next_offset,
            child->type, error);
    }
    if (box_size > remaining) {
        return avif_bmff_child_fail(
            AVIFDEC_TRUNCATED, iterator->next_offset, child->type, error);
    }
    child->offset = iterator->next_offset;
    child->size = box_size;
    child->header_size = header_size;
    child->payload_offset = child->offset + header_size;
    child->payload_size = box_size - header_size;
    if (child->type == AVIF_BMFF_UUID) {
        avifdec_memory_copy(
            child->user_type,
            iterator->data + child->payload_offset - 16U,
            sizeof(child->user_type));
        child->has_user_type = 1U;
    }
    iterator->next_offset += box_size;
    if (child->extends_to_parent_end) {
        iterator->saw_to_end_child = 1U;
        if (iterator->next_offset != iterator->parent_end) {
            return avif_bmff_child_fail(
                AVIFDEC_INVALID_DATA, child->offset, child->type, error);
        }
    }
    *has_child = 1;
    return AVIFDEC_OK;
}
