#include "av1_bitstream.h"

static int av1_stream_span(const Av1Stream *stream,
                           size_t span_index,
                           AvifdecSpan *span) {
    if (stream->spans == 0 && stream->source != 0) {
        return stream->source->span_at(
                   stream->source->context, span_index, span,
                   stream->source->error) == AVIFDEC_OK;
    }
    *span = stream->spans[span_index];
    return 1;
}

int av1_stream_byte_at(const Av1Stream *stream,
                       size_t position,
                       uint8_t *value,
                       size_t *file_offset) {
    size_t index;

    for (index = 0U; index < stream->span_count; ++index) {
        AvifdecSpan span;

        if (!av1_stream_span(stream, index, &span)) return 0;
        if (position < span.size) {
            *value = span.data[position];
            if (file_offset != 0) {
                *file_offset = span.file_offset + position;
            }
            return 1;
        }
        position -= span.size;
    }
    return 0;
}

size_t av1_stream_file_offset(const Av1Stream *stream, size_t position) {
    uint8_t ignored;
    size_t offset;

    if (av1_stream_byte_at(stream, position, &ignored, &offset)) return offset;
    if (stream->span_count != 0U) {
        AvifdecSpan last;

        if (av1_stream_span(
                stream, stream->span_count - 1U, &last)) {
            return last.file_offset + last.size;
        }
    }
    return 0U;
}

uint8_t av1_stream_read(Av1Stream *stream) {
    uint8_t value = 0U;

    if (stream->status != AVIFDEC_OK) return 0U;
    if (stream->position >= stream->size ||
        !av1_stream_byte_at(stream, stream->position, &value, 0)) {
        stream->status = AVIFDEC_TRUNCATED;
        return 0U;
    }
    ++stream->position;
    return value;
}

AvifdecStatus av1_leb128(Av1Stream *stream, size_t *value) {
    size_t result = 0U;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        uint8_t byte = av1_stream_read(stream);
        size_t bits = byte & 0x7fU;

        if (stream->status != AVIFDEC_OK) return stream->status;
        if (index * 7U >= sizeof(size_t) * 8U ||
            bits > (SIZE_MAX >> (index * 7U))) {
            return AVIFDEC_OVERFLOW;
        }
        result |= bits << (index * 7U);
        if ((byte & 0x80U) == 0U) {
            *value = result;
            return AVIFDEC_OK;
        }
    }
    return AVIFDEC_INVALID_DATA;
}

void av1_bits_init(Av1Bits *bits,
                   const Av1Stream *stream,
                   size_t start,
                   size_t size) {
    bits->stream = stream;
    bits->start = start;
    bits->size = size;
    bits->bit_position = 0U;
    bits->status = AVIFDEC_OK;
}

uint32_t av1_bits_read(Av1Bits *bits, unsigned int count) {
    uint32_t value = 0U;
    unsigned int index;

    if (bits->status != AVIFDEC_OK) return 0U;
    if (count > 32U || bits->bit_position > bits->size * 8U ||
        count > bits->size * 8U - bits->bit_position) {
        bits->status =
            count > 32U ? AVIFDEC_INVALID_ARGUMENT : AVIFDEC_TRUNCATED;
        return 0U;
    }
    for (index = 0U; index < count; ++index) {
        size_t position = bits->bit_position++;
        uint8_t byte;

        if (!av1_stream_byte_at(
                bits->stream, bits->start + position / 8U, &byte, 0)) {
            bits->status = AVIFDEC_TRUNCATED;
            return 0U;
        }
        value = (value << 1) | ((byte >> (7U - position % 8U)) & 1U);
    }
    return value;
}

size_t av1_bits_offset(const Av1Bits *bits) {
    return av1_stream_file_offset(
        bits->stream, bits->start + bits->bit_position / 8U);
}

AvifdecStatus av1_bits_skip(Av1Bits *bits, size_t count) {
    if (bits->status != AVIFDEC_OK) return bits->status;
    if (bits->bit_position > bits->size * 8U ||
        count > bits->size * 8U - bits->bit_position) {
        bits->status = AVIFDEC_TRUNCATED;
    } else {
        bits->bit_position += count;
    }
    return bits->status;
}

uint32_t av1_bits_uvlc(Av1Bits *bits) {
    unsigned int leading_zeros = 0U;

    while (av1_bits_read(bits, 1U) == 0U &&
           bits->status == AVIFDEC_OK) {
        if (++leading_zeros >= 32U) return 0xffffffffU;
    }
    if (bits->status != AVIFDEC_OK) return 0U;
    return ((1U << leading_zeros) - 1U) +
           av1_bits_read(bits, leading_zeros);
}

uint32_t av1_bits_ns(Av1Bits *bits, uint32_t n) {
    unsigned int width = 0U;
    uint64_t power = 1U;
    uint32_t value;
    uint32_t minimum;

    if (n <= 1U) return 0U;
    while (power < n) {
        power <<= 1U;
        ++width;
    }
    minimum = (uint32_t)(power - n);
    value = av1_bits_read(bits, width - 1U);
    if (value < minimum) return value;
    return (value << 1) - minimum + av1_bits_read(bits, 1U);
}

int32_t av1_bits_signed(Av1Bits *bits, unsigned int count) {
    uint32_t value = av1_bits_read(bits, count);
    uint32_t sign = 1U << (count - 1U);

    return (int32_t)((value ^ sign) - sign);
}

AvifdecStatus av1_trailing_bits(Av1Bits *bits) {
    if (av1_bits_read(bits, 1U) != 1U) return AVIFDEC_INVALID_DATA;
    while (bits->bit_position < bits->size * 8U) {
        if (av1_bits_read(bits, 1U) != 0U) return AVIFDEC_INVALID_DATA;
    }
    return bits->status;
}

AvifdecStatus av1_byte_alignment(Av1Bits *bits) {
    while ((bits->bit_position & 7U) != 0U) {
        if (av1_bits_read(bits, 1U) != 0U) return AVIFDEC_INVALID_DATA;
    }
    return bits->status;
}
