#ifndef AVIFDEC_AV1_AVIF_CONFORMANCE_H
#define AVIFDEC_AV1_AVIF_CONFORMANCE_H

#include "avifdec.h"

/*
 * Validate the complete AV1 OBU envelope before operating-point or spatial
 * layer filtering. This checks the explicitly selected framing, rejects every
 * tile-list OBU as invalid AVIF, and leaves payload syntax to the AV1 parser.
 *
 * The caller owns error initialization. A failure is recorded only while
 * error->status is AVIFDEC_OK, preserving the decoder's first-error contract.
 */
AvifdecStatus av1_avif_validate_obu_stream(
    const AvifdecSpan *spans,
    size_t span_count,
    uint8_t framing,
    AvifdecError *error);

/*
 * Call this hook only from AVIF parsing, at the point where already-parsed
 * sequence/frame state determines large-scale-tile mode. Keeping the input as
 * an explicit boolean avoids a second, incomplete frame-header parser and
 * leaves generic AV1 parsing unchanged. The enclosing OBU parser remains
 * responsible for attaching its current absolute syntax offset and OBU
 * context if this hook fails.
 */
AvifdecStatus av1_avif_validate_large_scale_tile(int large_scale_tile);

#endif
