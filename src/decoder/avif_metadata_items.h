#ifndef AVIF_METADATA_ITEMS_H
#define AVIF_METADATA_ITEMS_H

#include "avif_item_index.h"

#if AVIFDEC_VERSION_MAJOR > 1U || AVIFDEC_VERSION_MINOR >= 4U
typedef AvifdecByteView AvifItemMetadataByteView;
typedef AvifdecMetadataKind AvifItemMetadataKind;
typedef AvifdecMetadataScope AvifItemMetadataScope;
typedef AvifdecTiffByteOrder AvifItemTiffByteOrder;
typedef AvifdecMetadataInfo AvifItemMetadataInfo;
typedef AvifdecThumbnailInfo AvifItemThumbnailInfo;
typedef AvifdecMetadataResult AvifItemMetadataResult;

#define AVIF_ITEM_METADATA_UNKNOWN AVIFDEC_METADATA_UNKNOWN
#define AVIF_ITEM_METADATA_EXIF AVIFDEC_METADATA_EXIF
#define AVIF_ITEM_METADATA_XMP AVIFDEC_METADATA_XMP
#define AVIF_ITEM_METADATA_MIME AVIFDEC_METADATA_MIME
#define AVIF_ITEM_METADATA_SCOPE_UNSCOPED AVIFDEC_METADATA_SCOPE_UNSCOPED
#define AVIF_ITEM_METADATA_SCOPE_ITEM AVIFDEC_METADATA_SCOPE_ITEM
#define AVIF_ITEM_METADATA_SCOPE_TRACK AVIFDEC_METADATA_SCOPE_TRACK
#define AVIF_ITEM_TIFF_BYTE_ORDER_NONE AVIFDEC_TIFF_BYTE_ORDER_NONE
#define AVIF_ITEM_TIFF_BYTE_ORDER_LITTLE AVIFDEC_TIFF_BYTE_ORDER_LITTLE
#define AVIF_ITEM_TIFF_BYTE_ORDER_BIG AVIFDEC_TIFF_BYTE_ORDER_BIG
#define AVIF_ITEM_METADATA_FLAG_SEQUENCE_WIDE \
    AVIFDEC_METADATA_FLAG_SEQUENCE_WIDE
#define AVIF_ITEM_METADATA_FLAG_CANONICAL_XMP \
    AVIFDEC_METADATA_FLAG_CANONICAL_XMP
#else
typedef struct {
    const unsigned char *data;
    size_t size;
} AvifItemMetadataByteView;

typedef enum {
    AVIF_ITEM_METADATA_UNKNOWN = 0,
    AVIF_ITEM_METADATA_EXIF = 1,
    AVIF_ITEM_METADATA_XMP = 2,
    AVIF_ITEM_METADATA_MIME = 3
} AvifItemMetadataKind;

typedef enum {
    AVIF_ITEM_METADATA_SCOPE_UNSCOPED = 0,
    AVIF_ITEM_METADATA_SCOPE_ITEM = 1,
    AVIF_ITEM_METADATA_SCOPE_TRACK = 2
} AvifItemMetadataScope;

typedef enum {
    AVIF_ITEM_TIFF_BYTE_ORDER_NONE = 0,
    AVIF_ITEM_TIFF_BYTE_ORDER_LITTLE = 1,
    AVIF_ITEM_TIFF_BYTE_ORDER_BIG = 2
} AvifItemTiffByteOrder;

#define AVIF_ITEM_METADATA_FLAG_SEQUENCE_WIDE ((uint32_t)1U << 0)
#define AVIF_ITEM_METADATA_FLAG_CANONICAL_XMP ((uint32_t)1U << 1)

typedef struct {
    uint32_t item_id;
    uint32_t target_item_id;
    uint32_t target_track_id;
    uint32_t item_type;
    uint32_t relationship_type;
    AvifItemMetadataKind kind;
    AvifItemMetadataScope scope;
    uint32_t flags;
    size_t payload_size;
    size_t span_index;
    size_t span_count;
    size_t content_offset;
    size_t exif_tiff_offset;
    AvifItemTiffByteOrder exif_byte_order;
    AvifItemMetadataByteView item_name;
    AvifItemMetadataByteView content_type;
    AvifItemMetadataByteView content_encoding;
} AvifItemMetadataInfo;

typedef struct {
    uint32_t thumbnail_item_id;
    uint32_t target_item_id;
    uint32_t target_track_id;
    uint32_t item_type;
    uint32_t relationship_type;
    uint32_t width;
    uint32_t height;
    uint32_t presentation_width;
    uint32_t presentation_height;
    uint32_t flags;
    size_t payload_size;
    size_t span_index;
    size_t span_count;
} AvifItemThumbnailInfo;

typedef struct {
    uint32_t primary_item_id;
    size_t metadata_count;
    size_t thumbnail_count;
    size_t span_count;
} AvifItemMetadataResult;
#endif

typedef struct {
    const AvifItemMetadataInfo *metadata;
    size_t metadata_count;
    const AvifItemThumbnailInfo *thumbnails;
    size_t thumbnail_count;
    const AvifdecSpan *spans;
    size_t span_count;
} AvifItemTrackMetadataView;

typedef AvifdecStatus (*AvifItemTrackMetadataViewFunction)(
    const void *context,
    uint32_t track_id,
    AvifItemTrackMetadataView *view,
    AvifdecError *error);

typedef struct {
    const void *context;
    AvifItemTrackMetadataViewFunction view;
} AvifItemTrackMetadataSource;

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
    AvifdecError *error);

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
    AvifdecError *error);

AvifdecStatus avif_metadata_items_query_index(
    const AvifItemIndex *index,
    AvifItemMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifItemThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifItemMetadataResult *result,
    AvifdecError *error);

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
    AvifdecError *error);

#endif
