#ifndef AVIFDEC_BMFF_CHILD_H
#define AVIFDEC_BMFF_CHILD_H

#include "avifdec.h"

typedef struct {
    uint32_t type;
    size_t offset;
    size_t size;
    size_t header_size;
    size_t payload_offset;
    size_t payload_size;
    unsigned char user_type[16];
    uint8_t has_user_type;
    uint8_t extends_to_parent_end;
} AvifBmffChild;

typedef struct {
    const unsigned char *data;
    size_t data_size;
    size_t next_offset;
    size_t parent_end;
    uint32_t parent_type;
    uint8_t saw_to_end_child;
} AvifBmffChildIterator;

AvifdecStatus avif_bmff_child_iterator_init(
    AvifBmffChildIterator *iterator,
    const void *data,
    size_t data_size,
    size_t payload_offset,
    size_t payload_size,
    uint32_t parent_type,
    AvifdecError *error);

AvifdecStatus avif_bmff_child_next(
    AvifBmffChildIterator *iterator,
    AvifBmffChild *child,
    int *has_child,
    AvifdecError *error);

#endif
