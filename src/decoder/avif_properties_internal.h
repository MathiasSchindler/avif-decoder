#ifndef AVIF_PROPERTIES_INTERNAL_H
#define AVIF_PROPERTIES_INTERNAL_H

#include "avif_internal.h"

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
