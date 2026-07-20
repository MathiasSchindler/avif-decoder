#ifndef AVIFDEC_AV1_BITSTREAM_H
#define AVIFDEC_AV1_BITSTREAM_H

#include "avifdec.h"

typedef struct {
    const AvifdecSpan *spans;
    size_t span_count;
    size_t size;
    size_t position;
    AvifdecStatus status;
} Av1Stream;

typedef struct {
    const Av1Stream *stream;
    size_t start;
    size_t size;
    size_t bit_position;
    AvifdecStatus status;
} Av1Bits;

int av1_stream_byte_at(const Av1Stream *stream,
                       size_t position,
                       uint8_t *value,
                       size_t *file_offset);
size_t av1_stream_file_offset(const Av1Stream *stream, size_t position);
uint8_t av1_stream_read(Av1Stream *stream);
AvifdecStatus av1_leb128(Av1Stream *stream, size_t *value);

void av1_bits_init(Av1Bits *bits,
                   const Av1Stream *stream,
                   size_t start,
                   size_t size);
uint32_t av1_bits_read(Av1Bits *bits, unsigned int count);
size_t av1_bits_offset(const Av1Bits *bits);
AvifdecStatus av1_bits_skip(Av1Bits *bits, size_t count);
uint32_t av1_bits_uvlc(Av1Bits *bits);
uint32_t av1_bits_ns(Av1Bits *bits, uint32_t n);
int32_t av1_bits_signed(Av1Bits *bits, unsigned int count);
AvifdecStatus av1_trailing_bits(Av1Bits *bits);
AvifdecStatus av1_byte_alignment(Av1Bits *bits);

#endif
