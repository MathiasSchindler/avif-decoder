#ifndef AVIF_ITEM_INDEX_H
#define AVIF_ITEM_INDEX_H

#include "avifdec.h"
#include "bmff.h"

#define AVIF_ITEM_INDEX_MAX_ITEMS AVIFDEC_DEFAULT_MAX_ITEMS
#define AVIF_ITEM_INDEX_MAX_EXTENTS AVIFDEC_DEFAULT_MAX_EXTENTS
#define AVIF_ITEM_INDEX_MAX_PROPERTIES AVIFDEC_DEFAULT_MAX_PROPERTIES
#define AVIF_ITEM_INDEX_MAX_ASSOCIATIONS \
    (AVIF_ITEM_INDEX_MAX_ITEMS * 16U)
#define AVIF_ITEM_INDEX_MAX_REFERENCES \
    (AVIF_ITEM_INDEX_MAX_ITEMS * 4U)
#define AVIF_ITEM_INDEX_MAX_ENTITY_GROUPS AVIF_ITEM_INDEX_MAX_ITEMS
#define AVIF_ITEM_INDEX_MAX_GROUP_ENTITIES AVIF_ITEM_INDEX_MAX_REFERENCES
#define AVIF_ITEM_INDEX_MAX_DATA_BOXES 64U
#define AVIF_ITEM_INDEX_MAX_IPMA_BOXES 16U
#define AVIF_ITEM_INDEX_DEFAULT_MAX_METADATA_ITEMS 64U
#define AVIF_ITEM_INDEX_DEFAULT_MAX_METADATA_SPANS 4096U
#define AVIF_ITEM_INDEX_MAX_STACK_BYTES 131072U
#define AVIF_ITEM_INDEX_ITEM_FLAG_HIDDEN ((uint32_t)1U << 0)

typedef struct {
    const unsigned char *data;
    size_t size;
} AvifItemByteView;

typedef struct {
    uint32_t id;
    uint32_t type;
    uint32_t flags;
    uint16_t protection_index;
    AvifItemByteView name;
    AvifItemByteView content_type;
    AvifItemByteView content_encoding;
    AvifItemByteView item_uri_type;
    size_t source_offset;
    size_t protection_offset;
} AvifItemIndexItem;

typedef struct {
    uint64_t index;
    uint64_t offset;
    uint64_t length;
    size_t source_offset;
} AvifItemIndexExtent;

typedef struct {
    uint32_t item_id;
    uint16_t data_reference_index;
    uint8_t construction_method;
    uint8_t index_size;
    uint64_t base_offset;
    size_t extent_count;
    size_t source_offset;
    size_t construction_method_offset;
    size_t data_reference_offset;
    AvifItemIndexExtent extents[AVIF_ITEM_INDEX_MAX_EXTENTS];
} AvifItemIndexLocation;

typedef struct {
    uint32_t from_item_id;
    uint32_t to_item_id;
    uint32_t type;
    size_t source_offset;
} AvifItemIndexReference;

typedef struct {
    AvifdecBmffBox box;
    uint32_t type;
} AvifItemIndexProperty;

typedef struct {
    uint32_t item_id;
    uint16_t property_index;
    uint8_t essential;
} AvifItemIndexAssociation;

typedef struct {
    uint32_t type;
    uint32_t group_id;
    size_t entity_index;
    size_t entity_count;
    size_t source_offset;
} AvifItemIndexEntityGroup;

typedef struct {
    uint32_t entity_id;
    size_t source_offset;
} AvifItemIndexGroupEntity;

typedef enum {
    AVIF_ITEM_ALTERNATIVE_NONE = 0,
    AVIF_ITEM_ALTERNATIVE_FIRST_BEFORE_SECOND = 1,
    AVIF_ITEM_ALTERNATIVE_SECOND_BEFORE_FIRST = 2
} AvifItemAlternativeOrder;

typedef struct {
    size_t max_items;
    size_t max_extents;
    size_t max_properties;
    size_t max_metadata_items;
    size_t max_metadata_spans;
    size_t max_references;
    size_t max_associations;
    size_t max_data_boxes;
    size_t max_entity_groups;
    size_t max_group_entities;
    uint32_t max_width;
    uint32_t max_height;
    size_t max_pixels;
} AvifItemIndexLimits;

typedef struct {
    uint32_t item_id;
    size_t payload_size;
    size_t span_count;
} AvifItemPayload;

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t selected_meta_offset;
    AvifItemIndexLimits limits;
    uint32_t primary_item_id;
    AvifdecBmffBox meta;
    AvifdecBmffBox handler;
    AvifdecBmffBox pitm;
    AvifdecBmffBox iloc;
    AvifdecBmffBox iinf;
    AvifdecBmffBox iref;
    AvifdecBmffBox iprp;
    AvifdecBmffBox ipco;
    AvifdecBmffBox grpl;
    AvifdecBmffBox idat;
    AvifdecBmffBox ipma[AVIF_ITEM_INDEX_MAX_IPMA_BOXES];
    size_t ipma_count;
    AvifdecBmffBox data_boxes[AVIF_ITEM_INDEX_MAX_DATA_BOXES];
    size_t data_box_count;
    AvifItemIndexItem items[AVIF_ITEM_INDEX_MAX_ITEMS];
    size_t item_count;
    AvifItemIndexProperty properties[AVIF_ITEM_INDEX_MAX_PROPERTIES];
    size_t property_count;
    AvifItemIndexAssociation
        associations[AVIF_ITEM_INDEX_MAX_ASSOCIATIONS];
    size_t association_count;
    AvifItemIndexReference references[AVIF_ITEM_INDEX_MAX_REFERENCES];
    size_t reference_count;
    AvifItemIndexEntityGroup
        entity_groups[AVIF_ITEM_INDEX_MAX_ENTITY_GROUPS];
    size_t entity_group_count;
    AvifItemIndexGroupEntity
        group_entities[AVIF_ITEM_INDEX_MAX_GROUP_ENTITIES];
    size_t group_entity_count;
    AvifdecError *error;
    AvifdecStatus failure_status;
    uint8_t require_primary_item;
} AvifItemIndex;

_Static_assert(sizeof(AvifItemIndex) <= AVIF_ITEM_INDEX_MAX_STACK_BYTES,
               "AvifItemIndex exceeds its fixed stack budget");

void avif_item_index_default_limits(AvifItemIndexLimits *limits);
void avif_item_index_limits_from_public(
    const AvifdecLimits *limits,
    AvifItemIndexLimits *item_limits);

AvifdecStatus avif_item_index_build(
    const void *data,
    size_t size,
    const AvifItemIndexLimits *limits,
    AvifItemIndex *index,
    AvifdecError *error);

AvifdecStatus avif_item_index_build_meta(
    const void *data,
    size_t size,
    const AvifItemIndexLimits *limits,
    size_t meta_offset,
    AvifItemIndex *index,
    AvifdecError *error);

const AvifItemIndexItem *avif_item_index_find_item(
    const AvifItemIndex *index,
    uint32_t item_id);

AvifdecStatus avif_item_index_query_references(
    const AvifItemIndex *index,
    uint32_t type,
    uint32_t from_item_id,
    uint32_t *to_item_ids,
    size_t id_capacity,
    size_t *id_count,
    size_t *reference_offset,
    AvifdecError *error);

AvifdecStatus avif_item_index_alternative_order(
    const AvifItemIndex *index,
    uint32_t first_item_id,
    uint32_t second_item_id,
    AvifItemAlternativeOrder *order,
    size_t *group_offset,
    AvifdecError *error);

int avif_item_index_property_type_supported(uint32_t type);

AvifdecStatus avif_item_index_validate_essential_properties(
    const AvifItemIndex *index,
    uint32_t item_id,
    AvifdecError *error);

AvifdecStatus avif_item_index_find_location(
    const AvifItemIndex *index,
    uint32_t item_id,
    AvifItemIndexLocation *location,
    AvifdecError *error);

AvifdecStatus avif_item_index_resolve_location(
    const AvifItemIndex *index,
    const AvifItemIndexLocation *location,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemPayload *payload,
    AvifdecError *error);

AvifdecStatus avif_item_index_resolve_item(
    const AvifItemIndex *index,
    uint32_t item_id,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemPayload *payload,
    AvifdecError *error);

AvifdecStatus avif_item_index_item_span_at(
    const AvifItemIndex *index,
    uint32_t item_id,
    size_t span_index,
    AvifdecSpan *span,
    AvifdecError *error);

AvifdecStatus avif_item_index_read_item(
    const AvifItemIndex *index,
    uint32_t item_id,
    size_t payload_offset,
    void *output,
    size_t output_size,
    size_t *first_file_offset,
    AvifdecError *error);

AvifdecStatus avif_item_index_item_dimensions(
    const AvifItemIndex *index,
    uint32_t item_id,
    uint32_t *width,
    uint32_t *height,
    uint32_t *presentation_width,
    uint32_t *presentation_height,
    AvifdecError *error);

#endif
