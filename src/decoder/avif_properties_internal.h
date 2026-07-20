#ifndef AVIF_PROPERTIES_INTERNAL_H
#define AVIF_PROPERTIES_INTERNAL_H

#include "avifdec.h"
#include "bmff.h"

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
    const AvifItem *items;
    size_t item_count;
    const AvifProperty *properties;
    const AvifAssociation *associations;
    size_t association_count;
    const AvifdecBmffBox *iinf;
    const AvifdecBmffBox *ipco;
    AvifdecError *error;
    int *failed;
} AvifPropertyParseContext;

AvifdecStatus avif_properties_parse(
    const AvifPropertyParseContext *context,
    uint32_t item_id,
    AvifdecImageInfo *info);

#endif
