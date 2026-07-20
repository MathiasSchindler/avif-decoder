#ifndef AVIF_INTERNAL_H
#define AVIF_INTERNAL_H

#include "avifdec.h"
#include "bmff.h"

#define AVIF_MAX_ITEMS AVIFDEC_DEFAULT_MAX_ITEMS
#define AVIF_MAX_EXTENTS AVIFDEC_DEFAULT_MAX_EXTENTS
#define AVIF_MAX_PROPERTIES AVIFDEC_DEFAULT_MAX_PROPERTIES
#define AVIF_MAX_ASSOCIATIONS (AVIF_MAX_ITEMS * 16U)
#define AVIF_MAX_REFERENCE_BOXES AVIF_MAX_ITEMS
#define AVIF_MAX_REFERENCES (AVIF_MAX_ITEMS * 4U)
#define AVIF_MAX_DATA_BOXES 16U
#define AVIF_MAX_RESOLVED_SPANS (AVIF_MAX_EXTENTS * AVIF_MAX_DATA_BOXES)
#define AVIF_WORKSPACE_BASE_ALIGNMENT 16U

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
    uint32_t from_item_id;
    uint32_t to_item_id;
    uint32_t type;
} AvifReference;

typedef struct {
    uint32_t id;
    uint32_t type;
} AvifItem;

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

_Static_assert(
    _Alignof(AvifContext) <= AVIF_WORKSPACE_BASE_ALIGNMENT,
    "AvifContext alignment exceeds workspace sizing slack");

#endif
