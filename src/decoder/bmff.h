#ifndef AVIFDEC_BMFF_H
#define AVIFDEC_BMFF_H

#include "base.h"

#define AVIFDEC_BMFF_MAX_BRANDS 32U
#define AVIFDEC_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | \
     ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8) | \
     (uint32_t)(unsigned char)(d))

typedef struct {
    size_t max_depth;
    size_t max_boxes;
} AvifdecBmffLimits;

typedef struct {
    uint32_t type;
    size_t offset;
    size_t size;
    size_t header_size;
    size_t payload_offset;
    size_t payload_size;
    size_t depth;
    unsigned char user_type[16];
    int has_user_type;
} AvifdecBmffBox;

typedef void (*AvifdecBmffVisitor)(const AvifdecBmffBox *box, void *user_data);

typedef struct {
    uint32_t major_brand;
    uint32_t minor_version;
    uint32_t compatible_brands[AVIFDEC_BMFF_MAX_BRANDS];
    size_t compatible_brand_count;
    size_t box_count;
    size_t maximum_depth;
    size_t meta_count;
    size_t handler_count;
    size_t media_data_count;
    int has_avif_brand;
} AvifdecBmffInfo;

AvifdecStatus avifdec_bmff_inspect(const void *data,
                                   size_t size,
                                   const AvifdecBmffLimits *limits,
                                   AvifdecBmffVisitor visitor,
                                   void *user_data,
                                   AvifdecBmffInfo *info,
                                   AvifdecError *error);

#endif