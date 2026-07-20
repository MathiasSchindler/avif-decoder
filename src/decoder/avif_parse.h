#ifndef AVIF_PARSE_H
#define AVIF_PARSE_H

#include "avif_internal.h"

AvifdecStatus avif_fail(AvifContext *context,
                        AvifdecStatus status,
                        size_t offset,
                        uint32_t box_type);
int avif_find_item(const AvifContext *context, uint32_t item_id);
AvifdecStatus avif_parse_location(AvifContext *context,
                                  uint32_t primary_id,
                                  AvifLocation *location);
AvifdecStatus avif_resolve_extents(AvifContext *context,
                                   const AvifLocation *location,
                                   AvifdecSpan *spans,
                                   size_t span_capacity,
                                   AvifdecImageInfo *info);
AvifdecStatus avif_open_context(AvifContext *context,
                                const void *data,
                                size_t size,
                                const AvifdecLimits *limits,
                                uint32_t *primary_id,
                                AvifdecError *error);

#endif
