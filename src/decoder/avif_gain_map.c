#include "avif_gain_map.h"

#define AVIF_GAIN_MAP_INT32_MAX_VALUE 2147483647U
#define AVIF_GAIN_MAP_FLOAT_MAX 3.40282346638528859812e38
#define AVIF_GAIN_MAP_FLAG_MULTICHANNEL 0x80U
#define AVIF_GAIN_MAP_FLAG_USE_BASE_COLOR_SPACE 0x40U
#define AVIF_GAIN_MAP_FLAG_COMMON_DENOMINATOR 0x08U
#define AVIF_GAIN_MAP_FLAG_BACKWARD_DIRECTION 0x04U
#define AVIF_GAIN_MAP_FLAG_RESERVED 0x33U
#define AVIF_GAIN_MAP_KNOWN_TRANSFORMS \
    (AVIFDEC_TRANSFORM_CLAP | AVIFDEC_TRANSFORM_IROT | \
     AVIFDEC_TRANSFORM_IMIR | AVIFDEC_TRANSFORM_PASP)

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "gain-map arithmetic requires binary32 storage");

typedef union {
    float value;
    uint32_t bits;
} AvifGainMapFloatBits;

typedef struct {
    const AvifGainMapSpanSource *source;
    AvifdecSpan span;
    size_t span_index;
    size_t span_position;
    size_t logical_position;
    size_t logical_size;
    int span_loaded;
    AvifdecStatus status;
    AvifdecError *error;
} AvifGainMapReader;

typedef struct {
    const AvifdecSpan *spans;
    size_t span_count;
} AvifGainMapSpanArray;

static void avif_gain_map_zero(void *destination, size_t size) {
    volatile unsigned char *bytes =
        (volatile unsigned char *)destination;
    size_t index;

    for (index = 0U; index < size; ++index) bytes[index] = 0U;
}

static void avif_gain_map_copy(void *destination,
                               const void *source,
                               size_t size) {
    volatile unsigned char *to =
        (volatile unsigned char *)destination;
    const volatile unsigned char *from =
        (const volatile unsigned char *)source;
    size_t index;

    for (index = 0U; index < size; ++index) to[index] = from[index];
}

static int avif_gain_map_size_add(size_t left,
                                  size_t right,
                                  size_t *result) {
    if (result == 0 || left > SIZE_MAX - right) return 0;
    *result = left + right;
    return 1;
}

static int avif_gain_map_size_multiply(size_t left,
                                       size_t right,
                                       size_t *result) {
    if (result == 0 || (right != 0U && left > SIZE_MAX / right)) return 0;
    *result = left * right;
    return 1;
}

static void avif_gain_map_error_clear(AvifdecError *error) {
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
}

static AvifdecStatus avif_gain_map_fail(AvifdecError *error,
                                        AvifdecStatus status,
                                        size_t offset,
                                        uint32_t context) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = context;
    }
    return status;
}

static AvifdecStatus avif_gain_map_callback_failure(
    AvifdecError *error,
    AvifdecStatus status,
    uint32_t context) {
    if (status == AVIFDEC_OK) {
        return error != 0 && error->status != AVIFDEC_OK
            ? error->status : AVIFDEC_OK;
    }
    return avif_gain_map_fail(error, status, 0U, context);
}

static int avif_gain_map_float_finite(float value) {
    AvifGainMapFloatBits bits;

    bits.value = value;
    return (bits.bits & 0x7f800000U) != 0x7f800000U;
}

static float avif_gain_map_nan(void) {
    AvifGainMapFloatBits bits;

    bits.bits = 0x7fc00000U;
    return bits.value;
}

static int32_t avif_gain_map_signed_u32(uint32_t value) {
    if (value <= AVIF_GAIN_MAP_INT32_MAX_VALUE) return (int32_t)value;
    return -1 - (int32_t)(0xffffffffU - value);
}

static AvifdecStatus avif_gain_map_span_array_at(
    void *context,
    size_t span_index,
    AvifdecSpan *span,
    AvifdecError *error) {
    const AvifGainMapSpanArray *array =
        (const AvifGainMapSpanArray *)context;

    (void)error;
    if (array == 0 || span == 0 || span_index >= array->span_count) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    *span = array->spans[span_index];
    return AVIFDEC_OK;
}

static AvifdecStatus avif_gain_map_reader_init(
    AvifGainMapReader *reader,
    const AvifGainMapSpanSource *source,
    AvifdecError *error) {
    size_t span_index;

    avif_gain_map_zero(reader, sizeof(*reader));
    reader->source = source;
    reader->error = error;
    if (source == 0 ||
        (source->span_count != 0U && source->span_at == 0)) {
        reader->status = AVIFDEC_INVALID_ARGUMENT;
        return avif_gain_map_fail(
            error, reader->status, 0U, AVIF_GAIN_MAP_TMAP);
    }
    for (span_index = 0U; span_index < source->span_count; ++span_index) {
        AvifdecSpan span;
        AvifdecStatus status =
            AVIFDEC_OK;

        avif_gain_map_zero(&span, sizeof(span));
        status = source->span_at(
            source->context, span_index, &span, error);
        status = avif_gain_map_callback_failure(
            error, status, AVIF_GAIN_MAP_TMAP);
        if (status != AVIFDEC_OK) {
            reader->status = status;
            return status;
        }
        if (span.data == 0 && span.size != 0U) {
            reader->status = AVIFDEC_INVALID_ARGUMENT;
            return avif_gain_map_fail(
                error, reader->status, span.file_offset,
                AVIF_GAIN_MAP_TMAP);
        }
        if (span.size > SIZE_MAX - span.file_offset) {
            reader->status = AVIFDEC_OVERFLOW;
            return avif_gain_map_fail(
                error, reader->status, span.file_offset,
                AVIF_GAIN_MAP_TMAP);
        }
        if (!avif_gain_map_size_add(
                reader->logical_size, span.size,
                &reader->logical_size)) {
            reader->status = AVIFDEC_OVERFLOW;
            return avif_gain_map_fail(
                error, reader->status, span.file_offset,
                AVIF_GAIN_MAP_TMAP);
        }
    }
    reader->status = AVIFDEC_OK;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_gain_map_reader_load_span(
    AvifGainMapReader *reader) {
    while (reader->span_index < reader->source->span_count) {
        AvifdecStatus status;

        avif_gain_map_zero(&reader->span, sizeof(reader->span));
        status = reader->source->span_at(
            reader->source->context, reader->span_index,
            &reader->span, reader->error);
        status = avif_gain_map_callback_failure(
            reader->error, status, AVIF_GAIN_MAP_TMAP);
        if (status != AVIFDEC_OK) {
            reader->status = status;
            return status;
        }
        if ((reader->span.data == 0 && reader->span.size != 0U) ||
            reader->span.size >
                SIZE_MAX - reader->span.file_offset) {
            reader->status =
                reader->span.size >
                    SIZE_MAX - reader->span.file_offset
                    ? AVIFDEC_OVERFLOW
                    : AVIFDEC_INVALID_ARGUMENT;
            return avif_gain_map_fail(
                reader->error, reader->status,
                reader->span.file_offset, AVIF_GAIN_MAP_TMAP);
        }
        reader->span_loaded = 1;
        reader->span_position = 0U;
        if (reader->span.size != 0U) return AVIFDEC_OK;
        reader->span_loaded = 0;
        ++reader->span_index;
    }
    return AVIFDEC_TRUNCATED;
}

static size_t avif_gain_map_reader_offset(AvifGainMapReader *reader) {
    if (reader->logical_position < reader->logical_size) {
        if (!reader->span_loaded ||
            reader->span_position >= reader->span.size) {
            reader->span_loaded = 0;
            if (avif_gain_map_reader_load_span(reader) != AVIFDEC_OK) {
                return reader->source->payload_offset;
            }
        }
        return reader->span.file_offset + reader->span_position;
    }
    if (reader->span_loaded) {
        return reader->span.file_offset + reader->span.size;
    }
    if (reader->source->span_count != 0U) {
        AvifdecSpan last;
        AvifdecStatus status;

        avif_gain_map_zero(&last, sizeof(last));
        status = reader->source->span_at(
            reader->source->context,
            reader->source->span_count - 1U, &last, reader->error);

        if (status == AVIFDEC_OK && last.size <= SIZE_MAX - last.file_offset) {
            return last.file_offset + last.size;
        }
    }
    return reader->source->payload_offset;
}

static uint8_t avif_gain_map_reader_u8(AvifGainMapReader *reader,
                                       size_t *field_offset) {
    uint8_t value;

    if (field_offset != 0) {
        *field_offset = avif_gain_map_reader_offset(reader);
    }
    if (reader->status != AVIFDEC_OK) return 0U;
    if (reader->logical_position >= reader->logical_size) {
        reader->status = avif_gain_map_fail(
            reader->error, AVIFDEC_TRUNCATED,
            avif_gain_map_reader_offset(reader), AVIF_GAIN_MAP_TMAP);
        return 0U;
    }
    if (!reader->span_loaded ||
        reader->span_position >= reader->span.size) {
        reader->span_loaded = 0;
        if (avif_gain_map_reader_load_span(reader) != AVIFDEC_OK) {
            reader->status = avif_gain_map_fail(
                reader->error, AVIFDEC_TRUNCATED,
                avif_gain_map_reader_offset(reader),
                AVIF_GAIN_MAP_TMAP);
            return 0U;
        }
    }
    value = reader->span.data[reader->span_position++];
    ++reader->logical_position;
    if (reader->span_position == reader->span.size) {
        reader->span_loaded = 0;
        ++reader->span_index;
    }
    return value;
}

static uint16_t avif_gain_map_reader_u16(AvifGainMapReader *reader,
                                         size_t *field_offset) {
    uint16_t value;
    size_t offset = avif_gain_map_reader_offset(reader);

    value = (uint16_t)avif_gain_map_reader_u8(reader, 0) << 8;
    value |= avif_gain_map_reader_u8(reader, 0);
    if (field_offset != 0) *field_offset = offset;
    return value;
}

static uint32_t avif_gain_map_reader_u32(AvifGainMapReader *reader,
                                         size_t *field_offset) {
    uint32_t value;
    size_t offset = avif_gain_map_reader_offset(reader);

    value = (uint32_t)avif_gain_map_reader_u8(reader, 0) << 24;
    value |= (uint32_t)avif_gain_map_reader_u8(reader, 0) << 16;
    value |= (uint32_t)avif_gain_map_reader_u8(reader, 0) << 8;
    value |= avif_gain_map_reader_u8(reader, 0);
    if (field_offset != 0) *field_offset = offset;
    return value;
}

static int avif_gain_map_rational_less(AvifGainMapRational left,
                                       AvifGainMapRational right) {
    int64_t left_scaled =
        (int64_t)left.numerator * (int64_t)right.denominator;
    int64_t right_scaled =
        (int64_t)right.numerator * (int64_t)left.denominator;

    return left_scaled < right_scaled;
}

static int avif_gain_map_unsigned_rational_equal(
    AvifGainMapUnsignedRational left,
    AvifGainMapUnsignedRational right) {
    uint64_t left_scaled =
        (uint64_t)left.numerator * (uint64_t)right.denominator;
    uint64_t right_scaled =
        (uint64_t)right.numerator * (uint64_t)left.denominator;

    return left_scaled == right_scaled;
}

static AvifdecStatus avif_gain_map_metadata_validate(
    const AvifGainMapMetadata *metadata,
    AvifdecError *error) {
    size_t channel;

    if (metadata == 0) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (metadata->metadata_version != 0U ||
        metadata->minimum_version != 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_UNSUPPORTED, 0U, AVIF_GAIN_MAP_TMAP);
    }
    if (metadata->writer_version < metadata->minimum_version ||
        (metadata->channel_count != 1U &&
         metadata->channel_count != 3U) ||
        metadata->use_base_color_space > 1U ||
        metadata->backward_direction > 1U ||
        metadata->common_denominator > 1U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, 0U, AVIF_GAIN_MAP_TMAP);
    }
    if (metadata->base_hdr_headroom.denominator == 0U ||
        metadata->alternate_hdr_headroom.denominator == 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, 0U, AVIF_GAIN_MAP_TMAP);
    }
    for (channel = 0U; channel < metadata->channel_count; ++channel) {
        if (metadata->gain_map_min[channel].denominator == 0U ||
            metadata->gain_map_max[channel].denominator == 0U ||
            metadata->gain_map_gamma[channel].denominator == 0U ||
            metadata->gain_map_gamma[channel].numerator == 0U ||
            metadata->base_offset[channel].denominator == 0U ||
            metadata->alternate_offset[channel].denominator == 0U ||
            avif_gain_map_rational_less(
                metadata->gain_map_max[channel],
                metadata->gain_map_min[channel])) {
            return avif_gain_map_fail(
                error, AVIFDEC_INVALID_DATA, 0U,
                AVIF_GAIN_MAP_TMAP);
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_gain_map_parse_payload(
    const AvifGainMapSpanSource *source,
    AvifGainMapMetadata *metadata,
    AvifdecError *error) {
    AvifGainMapReader reader;
    size_t version_offset;
    size_t minimum_offset;
    size_t writer_offset;
    size_t flags_offset;
    size_t numerator_offset;
    size_t denominator_offset;
    uint8_t flags;
    uint32_t raw_value;
    size_t channel;
    AvifdecStatus status;
    AvifGainMapMetadata parsed_metadata;
    AvifGainMapMetadata *output_metadata = metadata;

    avif_gain_map_error_clear(error);
    if (output_metadata != 0) {
        avif_gain_map_zero(output_metadata, sizeof(*output_metadata));
    }
    if (output_metadata == 0) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avif_gain_map_zero(&parsed_metadata, sizeof(parsed_metadata));
    metadata = &parsed_metadata;
    status = avif_gain_map_reader_init(&reader, source, error);
    if (status != AVIFDEC_OK) return status;

    metadata->metadata_version =
        avif_gain_map_reader_u8(&reader, &version_offset);
    if (reader.status != AVIFDEC_OK) return reader.status;
    if (metadata->metadata_version != 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_UNSUPPORTED, version_offset,
            AVIF_GAIN_MAP_TMAP);
    }
    metadata->minimum_version =
        avif_gain_map_reader_u16(&reader, &minimum_offset);
    if (reader.status != AVIFDEC_OK) return reader.status;
    if (metadata->minimum_version != 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_UNSUPPORTED, minimum_offset,
            AVIF_GAIN_MAP_TMAP);
    }
    metadata->writer_version =
        avif_gain_map_reader_u16(&reader, &writer_offset);
    if (reader.status != AVIFDEC_OK) return reader.status;
    if (metadata->writer_version < metadata->minimum_version) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, writer_offset,
            AVIF_GAIN_MAP_TMAP);
    }

    flags = avif_gain_map_reader_u8(&reader, &flags_offset);
    if (reader.status != AVIFDEC_OK) return reader.status;
    if ((flags & AVIF_GAIN_MAP_FLAG_RESERVED) != 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, flags_offset,
            AVIF_GAIN_MAP_TMAP);
    }
    metadata->channel_count =
        (flags & AVIF_GAIN_MAP_FLAG_MULTICHANNEL) != 0U ? 3U : 1U;
    metadata->use_base_color_space =
        (uint8_t)((flags &
                   AVIF_GAIN_MAP_FLAG_USE_BASE_COLOR_SPACE) != 0U);
    metadata->backward_direction =
        (uint8_t)((flags &
                   AVIF_GAIN_MAP_FLAG_BACKWARD_DIRECTION) != 0U);
    metadata->common_denominator =
        (uint8_t)((flags &
                   AVIF_GAIN_MAP_FLAG_COMMON_DENOMINATOR) != 0U);

    if (metadata->common_denominator != 0U) {
        uint32_t common_denominator =
            avif_gain_map_reader_u32(
                &reader, &denominator_offset);

        if (reader.status != AVIFDEC_OK) return reader.status;
        if (common_denominator == 0U) {
            return avif_gain_map_fail(
                error, AVIFDEC_INVALID_DATA, denominator_offset,
                AVIF_GAIN_MAP_TMAP);
        }
        metadata->base_hdr_headroom.numerator =
            avif_gain_map_reader_u32(&reader, 0);
        metadata->base_hdr_headroom.denominator =
            common_denominator;
        metadata->alternate_hdr_headroom.numerator =
            avif_gain_map_reader_u32(&reader, 0);
        metadata->alternate_hdr_headroom.denominator =
            common_denominator;
        if (reader.status != AVIFDEC_OK) return reader.status;

        for (channel = 0U;
             channel < metadata->channel_count; ++channel) {
            size_t maximum_offset;

            raw_value = avif_gain_map_reader_u32(
                &reader, &numerator_offset);
            metadata->gain_map_min[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->gain_map_min[channel].denominator =
                common_denominator;

            raw_value = avif_gain_map_reader_u32(
                &reader, &maximum_offset);
            metadata->gain_map_max[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->gain_map_max[channel].denominator =
                common_denominator;
            if (reader.status != AVIFDEC_OK) return reader.status;
            if (avif_gain_map_rational_less(
                    metadata->gain_map_max[channel],
                    metadata->gain_map_min[channel])) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, maximum_offset,
                    AVIF_GAIN_MAP_TMAP);
            }

            metadata->gain_map_gamma[channel].numerator =
                avif_gain_map_reader_u32(
                    &reader, &numerator_offset);
            metadata->gain_map_gamma[channel].denominator =
                common_denominator;
            if (reader.status != AVIFDEC_OK) return reader.status;
            if (metadata->gain_map_gamma[channel].numerator == 0U) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, numerator_offset,
                    AVIF_GAIN_MAP_TMAP);
            }

            raw_value = avif_gain_map_reader_u32(&reader, 0);
            metadata->base_offset[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->base_offset[channel].denominator =
                common_denominator;
            raw_value = avif_gain_map_reader_u32(&reader, 0);
            metadata->alternate_offset[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->alternate_offset[channel].denominator =
                common_denominator;
            if (reader.status != AVIFDEC_OK) return reader.status;
        }
    } else {
        metadata->base_hdr_headroom.numerator =
            avif_gain_map_reader_u32(&reader, 0);
        metadata->base_hdr_headroom.denominator =
            avif_gain_map_reader_u32(
                &reader, &denominator_offset);
        if (reader.status != AVIFDEC_OK) return reader.status;
        if (metadata->base_hdr_headroom.denominator == 0U) {
            return avif_gain_map_fail(
                error, AVIFDEC_INVALID_DATA, denominator_offset,
                AVIF_GAIN_MAP_TMAP);
        }

        metadata->alternate_hdr_headroom.numerator =
            avif_gain_map_reader_u32(&reader, 0);
        metadata->alternate_hdr_headroom.denominator =
            avif_gain_map_reader_u32(
                &reader, &denominator_offset);
        if (reader.status != AVIFDEC_OK) return reader.status;
        if (metadata->alternate_hdr_headroom.denominator == 0U) {
            return avif_gain_map_fail(
                error, AVIFDEC_INVALID_DATA, denominator_offset,
                AVIF_GAIN_MAP_TMAP);
        }

        for (channel = 0U;
             channel < metadata->channel_count; ++channel) {
            size_t maximum_offset;

            raw_value = avif_gain_map_reader_u32(&reader, 0);
            metadata->gain_map_min[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->gain_map_min[channel].denominator =
                avif_gain_map_reader_u32(
                    &reader, &denominator_offset);
            if (reader.status != AVIFDEC_OK) return reader.status;
            if (metadata->gain_map_min[channel].denominator == 0U) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, denominator_offset,
                    AVIF_GAIN_MAP_TMAP);
            }

            raw_value = avif_gain_map_reader_u32(
                &reader, &maximum_offset);
            metadata->gain_map_max[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->gain_map_max[channel].denominator =
                avif_gain_map_reader_u32(
                    &reader, &denominator_offset);
            if (reader.status != AVIFDEC_OK) return reader.status;
            if (metadata->gain_map_max[channel].denominator == 0U) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, denominator_offset,
                    AVIF_GAIN_MAP_TMAP);
            }
            if (avif_gain_map_rational_less(
                    metadata->gain_map_max[channel],
                    metadata->gain_map_min[channel])) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, maximum_offset,
                    AVIF_GAIN_MAP_TMAP);
            }

            metadata->gain_map_gamma[channel].numerator =
                avif_gain_map_reader_u32(
                    &reader, &numerator_offset);
            metadata->gain_map_gamma[channel].denominator =
                avif_gain_map_reader_u32(
                    &reader, &denominator_offset);
            if (reader.status != AVIFDEC_OK) return reader.status;
            if (metadata->gain_map_gamma[channel].numerator == 0U) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, numerator_offset,
                    AVIF_GAIN_MAP_TMAP);
            }
            if (metadata->gain_map_gamma[channel].denominator == 0U) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, denominator_offset,
                    AVIF_GAIN_MAP_TMAP);
            }

            raw_value = avif_gain_map_reader_u32(&reader, 0);
            metadata->base_offset[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->base_offset[channel].denominator =
                avif_gain_map_reader_u32(
                    &reader, &denominator_offset);
            if (reader.status != AVIFDEC_OK) return reader.status;
            if (metadata->base_offset[channel].denominator == 0U) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, denominator_offset,
                    AVIF_GAIN_MAP_TMAP);
            }

            raw_value = avif_gain_map_reader_u32(&reader, 0);
            metadata->alternate_offset[channel].numerator =
                avif_gain_map_signed_u32(raw_value);
            metadata->alternate_offset[channel].denominator =
                avif_gain_map_reader_u32(
                    &reader, &denominator_offset);
            if (reader.status != AVIFDEC_OK) return reader.status;
            if (metadata->alternate_offset[channel].denominator == 0U) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, denominator_offset,
                    AVIF_GAIN_MAP_TMAP);
            }
        }
    }
    if (metadata->channel_count == 1U) {
        for (channel = 1U; channel < 3U; ++channel) {
            metadata->gain_map_min[channel] =
                metadata->gain_map_min[0];
            metadata->gain_map_max[channel] =
                metadata->gain_map_max[0];
            metadata->gain_map_gamma[channel] =
                metadata->gain_map_gamma[0];
            metadata->base_offset[channel] =
                metadata->base_offset[0];
            metadata->alternate_offset[channel] =
                metadata->alternate_offset[0];
        }
    }
    if (metadata->writer_version == 0U &&
        reader.logical_position != reader.logical_size) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA,
            avif_gain_map_reader_offset(&reader),
            AVIF_GAIN_MAP_TMAP);
    }
    status = avif_gain_map_metadata_validate(metadata, error);
    if (status != AVIFDEC_OK) return status;
    *output_metadata = *metadata;
    return AVIFDEC_OK;
}

AvifdecStatus avif_gain_map_parse_spans(
    const AvifdecSpan *spans,
    size_t span_count,
    size_t payload_offset,
    AvifGainMapMetadata *metadata,
    AvifdecError *error) {
    AvifGainMapSpanArray array;
    AvifGainMapSpanSource source;

    if (spans == 0 && span_count != 0U) {
        if (metadata != 0) avif_gain_map_zero(metadata, sizeof(*metadata));
        avif_gain_map_error_clear(error);
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    array.spans = spans;
    array.span_count = span_count;
    source.context = &array;
    source.span_count = span_count;
    source.payload_offset = payload_offset;
    source.span_at = avif_gain_map_span_array_at;
    return avif_gain_map_parse_payload(&source, metadata, error);
}

static AvifdecStatus avif_gain_map_index_item_at(
    const AvifGainMapItemIndex *index,
    size_t item_index,
    AvifGainMapIndexedItem *item,
    AvifdecError *error) {
    AvifdecStatus status;

    avif_gain_map_zero(item, sizeof(*item));
    status = index->item_at(
        index->context, item_index, item, error);
    status = avif_gain_map_callback_failure(
        error, status, AVIF_GAIN_MAP_TMAP);
    if (status != AVIFDEC_OK) {
        return status;
    }
    if (item->id == 0U || item->hidden > 1U ||
        item->has_unsupported_essential_property > 1U ||
        item->is_thumbnail > 1U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, item->source_offset,
            item->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_gain_map_find_item(
    const AvifGainMapItemIndex *index,
    uint32_t item_id,
    AvifGainMapIndexedItem *found_item,
    AvifdecError *error) {
    size_t item_index;
    int found = 0;

    for (item_index = 0U; item_index < index->item_count; ++item_index) {
        AvifGainMapIndexedItem item;
        AvifdecStatus status = avif_gain_map_index_item_at(
            index, item_index, &item, error);

        if (status != AVIFDEC_OK) return status;
        if (item.id != item_id) continue;
        if (found) {
            return avif_gain_map_fail(
                error, AVIFDEC_INVALID_DATA, item.source_offset,
                item.type);
        }
        *found_item = item;
        found = 1;
    }
    if (!found) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, 0U,
            AVIF_GAIN_MAP_FOURCC('i', 'i', 'n', 'f'));
    }
    return AVIFDEC_OK;
}

static int avif_gain_map_clap_equal(
    const AvifdecCleanAperture *left,
    const AvifdecCleanAperture *right) {
    return left->width_n == right->width_n &&
           left->width_d == right->width_d &&
           left->height_n == right->height_n &&
           left->height_d == right->height_d &&
           left->horizontal_offset_n == right->horizontal_offset_n &&
           left->horizontal_offset_d == right->horizontal_offset_d &&
           left->vertical_offset_n == right->vertical_offset_n &&
           left->vertical_offset_d == right->vertical_offset_d;
}

static int avif_gain_map_transforms_equal(
    const AvifdecImageInfo *base,
    const AvifdecImageInfo *gain) {
    if (base->transform_flags != gain->transform_flags ||
        (base->transform_flags &
         (uint8_t)~AVIF_GAIN_MAP_KNOWN_TRANSFORMS) != 0U) {
        return 0;
    }
    if ((base->transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U &&
        !avif_gain_map_clap_equal(
            &base->clean_aperture, &gain->clean_aperture)) {
        return 0;
    }
    if ((base->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
        (base->irot_angle != gain->irot_angle ||
         base->irot_angle > 3U)) {
        return 0;
    }
    if ((base->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U &&
        (base->imir_axis != gain->imir_axis ||
         base->imir_axis > 1U)) {
        return 0;
    }
    if ((base->transform_flags & AVIFDEC_TRANSFORM_PASP) != 0U &&
        (base->pixel_aspect_h_spacing !=
             gain->pixel_aspect_h_spacing ||
         base->pixel_aspect_v_spacing !=
             gain->pixel_aspect_v_spacing)) {
        return 0;
    }
    return 1;
}

static int avif_gain_map_color_valid(
    const AvifGainMapColorDescription *color) {
    return color->has_nclx <= 1U &&
           color->color_range <= 1U &&
           (color->icc.data != 0 || color->icc.size == 0U);
}

AvifdecStatus avif_gain_map_child_workspace(
    const AvifdecImageInfo *base_info,
    const AvifdecImageInfo *gain_info,
    size_t *workspace_required) {
    if (workspace_required != 0) *workspace_required = 0U;
    if (base_info == 0 || gain_info == 0 ||
        workspace_required == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    *workspace_required =
        base_info->workspace_required > gain_info->workspace_required
            ? base_info->workspace_required
            : gain_info->workspace_required;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_gain_map_get_dimg(
    const AvifGainMapItemIndex *index,
    uint32_t tmap_item_id,
    uint32_t ids[2],
    size_t *reference_offset,
    AvifdecError *error) {
    size_t count = 0U;
    AvifdecStatus status;

    ids[0] = 0U;
    ids[1] = 0U;
    *reference_offset = 0U;
    status = index->dimg(
        index->context, tmap_item_id, ids, 2U, &count,
        reference_offset, error);
    status = avif_gain_map_callback_failure(
        error, status, AVIF_GAIN_MAP_DIMG);
    if (status != AVIFDEC_OK) {
        return status;
    }
    if (count != 2U || ids[0] == 0U || ids[1] == 0U ||
        ids[0] == ids[1]) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, *reference_offset,
            AVIF_GAIN_MAP_DIMG);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_gain_map_find_tmap(
    const AvifGainMapItemIndex *index,
    const AvifGainMapIndexedItem *primary,
    AvifGainMapIndexedItem *tmap,
    AvifdecError *error) {
    size_t item_index;
    int found = 0;

    if (primary->type == AVIF_GAIN_MAP_TMAP) {
        *tmap = *primary;
        return AVIFDEC_OK;
    }
    for (item_index = 0U; item_index < index->item_count; ++item_index) {
        AvifGainMapIndexedItem candidate;
        AvifGainMapAlternativeOrder order;
        size_t group_offset = 0U;
        AvifdecStatus status = avif_gain_map_index_item_at(
            index, item_index, &candidate, error);

        if (status != AVIFDEC_OK) return status;
        if (candidate.type != AVIF_GAIN_MAP_TMAP ||
            candidate.is_thumbnail != 0U) {
            continue;
        }
        order = AVIF_GAIN_MAP_ALTERNATIVE_NONE;
        status = index->alternative_order(
            index->context, candidate.id, primary->id,
            &order, &group_offset, error);
        status = avif_gain_map_callback_failure(
            error, status, AVIF_GAIN_MAP_ALTR);
        if (status != AVIFDEC_OK) {
            return status;
        }
        if (order !=
            AVIF_GAIN_MAP_ALTERNATIVE_FIRST_BEFORE_SECOND) {
            continue;
        }
        if (found) {
            return avif_gain_map_fail(
                error, AVIFDEC_INVALID_DATA, group_offset,
                AVIF_GAIN_MAP_ALTR);
        }
        *tmap = candidate;
        found = 1;
    }
    if (!found) avif_gain_map_zero(tmap, sizeof(*tmap));
    return AVIFDEC_OK;
}

AvifdecStatus avif_gain_map_query_decode_plan(
    const AvifGainMapItemIndex *index,
    const AvifdecExecutor *executor,
    AvifGainMapDecodePlan *plan,
    AvifdecError *error) {
    AvifGainMapIndexedItem primary;
    AvifGainMapIndexedItem tmap;
    AvifGainMapIndexedItem base_item;
    AvifGainMapIndexedItem gain_item;
    AvifGainMapSpanSource payload;
    AvifGainMapDecodePlan parsed;
    uint32_t dimg_ids[2];
    size_t reference_offset;
    AvifdecStatus status;

    avif_gain_map_error_clear(error);
    if (plan != 0) avif_gain_map_zero(plan, sizeof(*plan));
    avif_gain_map_zero(&parsed, sizeof(parsed));
    if (index == 0 || plan == 0 || index->primary_item_id == 0U ||
        index->item_at == 0 || index->dimg == 0 ||
        index->alternative_order == 0 || index->item_payload == 0 ||
        index->query_child == 0) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_gain_map_find_item(
        index, index->primary_item_id, &primary, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_gain_map_find_tmap(index, &primary, &tmap, error);
    if (status != AVIFDEC_OK) return status;
    if (tmap.id == 0U) return AVIFDEC_OK;
    if (tmap.has_unsupported_essential_property != 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_UNSUPPORTED, tmap.source_offset,
            AVIF_GAIN_MAP_TMAP);
    }
    if (tmap.hidden != 0U || tmap.is_thumbnail != 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, tmap.source_offset,
            AVIF_GAIN_MAP_TMAP);
    }

    status = avif_gain_map_get_dimg(
        index, tmap.id, dimg_ids, &reference_offset, error);
    if (status != AVIFDEC_OK) return status;
    if (primary.type != AVIF_GAIN_MAP_TMAP &&
        dimg_ids[0] != primary.id) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, reference_offset,
            AVIF_GAIN_MAP_DIMG);
    }
    status = avif_gain_map_find_item(
        index, dimg_ids[0], &base_item, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_gain_map_find_item(
        index, dimg_ids[1], &gain_item, error);
    if (status != AVIFDEC_OK) return status;
    if (base_item.has_unsupported_essential_property != 0U ||
        gain_item.has_unsupported_essential_property != 0U) {
        const AvifGainMapIndexedItem *unsupported =
            base_item.has_unsupported_essential_property != 0U
                ? &base_item : &gain_item;
        return avif_gain_map_fail(
            error, AVIFDEC_UNSUPPORTED, unsupported->source_offset,
            unsupported->type);
    }
    if (base_item.is_thumbnail != 0U ||
        gain_item.is_thumbnail != 0U ||
        gain_item.hidden == 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, gain_item.source_offset,
            gain_item.type);
    }
    if (tmap.properties.transform_flags != 0U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, tmap.source_offset,
            AVIF_GAIN_MAP_TMAP);
    }
    if (!avif_gain_map_color_valid(&base_item.color) ||
        !avif_gain_map_color_valid(&tmap.color)) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, tmap.source_offset,
            AVIF_GAIN_MAP_FOURCC('c', 'o', 'l', 'r'));
    }

    avif_gain_map_zero(&payload, sizeof(payload));
    status = index->item_payload(
        index->context, tmap.id, &payload, error);
    status = avif_gain_map_callback_failure(
        error, status, AVIF_GAIN_MAP_TMAP);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_gain_map_parse_payload(
        &payload, &parsed.info.metadata, error);
    if (status != AVIFDEC_OK) return status;

    status = index->query_child(
        index->context, base_item.id, executor,
        &parsed.info.base_image, error);
    status = avif_gain_map_callback_failure(
        error, status, base_item.type);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = index->query_child(
        index->context, gain_item.id, executor,
        &parsed.info.gain_map_image, error);
    status = avif_gain_map_callback_failure(
        error, status, gain_item.type);
    if (status != AVIFDEC_OK) {
        return status;
    }

    if (parsed.info.base_image.width == 0U ||
        parsed.info.base_image.height == 0U ||
        parsed.info.base_image.presentation_width == 0U ||
        parsed.info.base_image.presentation_height == 0U ||
        parsed.info.gain_map_image.width == 0U ||
        parsed.info.gain_map_image.height == 0U ||
        parsed.info.gain_map_image.presentation_width == 0U ||
        parsed.info.gain_map_image.presentation_height == 0U ||
        tmap.properties.width != parsed.info.base_image.width ||
        tmap.properties.height != parsed.info.base_image.height ||
        parsed.info.gain_map_image.has_alpha != 0U ||
        !avif_gain_map_transforms_equal(
            &parsed.info.base_image,
            &parsed.info.gain_map_image)) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, tmap.source_offset,
            AVIF_GAIN_MAP_TMAP);
    }
    if ((parsed.info.metadata.channel_count == 1U &&
         (parsed.info.gain_map_image.monochrome != 1U ||
          parsed.info.gain_map_image.channel_count != 1U)) ||
        (parsed.info.metadata.channel_count == 3U &&
         (parsed.info.gain_map_image.monochrome != 0U ||
          parsed.info.gain_map_image.channel_count != 3U))) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_DATA, gain_item.source_offset,
            gain_item.type);
    }
    status = avif_gain_map_child_workspace(
        &parsed.info.base_image, &parsed.info.gain_map_image,
        &parsed.info.workspace_required);
    if (status != AVIFDEC_OK) {
        return avif_gain_map_fail(error, status, 0U, 0U);
    }
    parsed.info.base_color.color_primaries =
        parsed.info.base_image.color_primaries;
    parsed.info.base_color.transfer_characteristics =
        parsed.info.base_image.transfer_characteristics;
    parsed.info.base_color.matrix_coefficients =
        parsed.info.base_image.matrix_coefficients;
    parsed.info.base_color.color_range =
        parsed.info.base_image.color_range;
    parsed.info.base_color.has_nclx =
        parsed.info.base_image.has_nclx;
    parsed.info.base_color.icc.data =
        parsed.info.base_image.icc_data;
    parsed.info.base_color.icc.size =
        parsed.info.base_image.icc_size;
    parsed.info.alternate_color = tmap.color;
    parsed.info.base_item_id = base_item.id;
    parsed.info.alternate_item_id = tmap.id;
    parsed.info.gain_map_item_id = gain_item.id;
    parsed.info.base_is_hdr =
        parsed.info.metadata.backward_direction;
    parsed.info.present = 1U;
    *plan = parsed;
    return AVIFDEC_OK;
}

static uint32_t avif_gain_map_plane_dimension(
    uint32_t dimension,
    uint8_t subsampling) {
    return ((dimension - 1U) >> subsampling) + 1U;
}

static int avif_gain_map_plane_valid(const uint16_t *plane,
                                     size_t stride,
                                     uint32_t width,
                                     uint32_t height) {
    size_t last_row;
    size_t last_sample;

    if (plane == 0 || width == 0U || height == 0U ||
        stride < (size_t)width) {
        return 0;
    }
    if (!avif_gain_map_size_multiply(
            (size_t)height - 1U, stride, &last_row) ||
        !avif_gain_map_size_add(
            last_row, (size_t)width - 1U, &last_sample) ||
        last_sample > SIZE_MAX / sizeof(uint16_t)) {
        return 0;
    }
    return 1;
}

static int avif_gain_map_image_valid(
    const AvifdecImageInfo *info,
    const AvifdecImage *image) {
    uint32_t chroma_width;
    uint32_t chroma_height;

    if (info == 0 || image == 0 || info->width == 0U ||
        info->height == 0U || info->monochrome > 1U ||
        info->subsampling_x > 1U || info->subsampling_y > 1U ||
        info->has_alpha > 1U ||
        image->bit_depth != info->bit_depth ||
        image->monochrome != info->monochrome ||
        image->subsampling_x != info->subsampling_x ||
        image->subsampling_y != info->subsampling_y ||
        image->widths[0] != info->width ||
        image->heights[0] != info->height ||
        !avif_gain_map_plane_valid(
            image->planes[0], image->strides[0],
            info->width, info->height)) {
        return 0;
    }
    if (info->monochrome == 0U) {
        chroma_width = avif_gain_map_plane_dimension(
            info->width, info->subsampling_x);
        chroma_height = avif_gain_map_plane_dimension(
            info->height, info->subsampling_y);
        if (image->widths[1] != chroma_width ||
            image->widths[2] != chroma_width ||
            image->heights[1] != chroma_height ||
            image->heights[2] != chroma_height ||
            !avif_gain_map_plane_valid(
                image->planes[1], image->strides[1],
                chroma_width, chroma_height) ||
            !avif_gain_map_plane_valid(
                image->planes[2], image->strides[2],
                chroma_width, chroma_height)) {
            return 0;
        }
    }
    if (info->has_alpha != 0U) {
        if (image->alpha_plane == 0 ||
            image->alpha_width != info->width ||
            image->alpha_height != info->height ||
            image->alpha_bit_depth != info->alpha_bit_depth ||
            image->alpha_color_range != info->alpha_color_range ||
            image->alpha_premultiplied !=
                info->alpha_premultiplied ||
            !avif_gain_map_plane_valid(
                image->alpha_plane, image->alpha_stride,
                info->width, info->height)) {
            return 0;
        }
    }
    return 1;
}

AvifdecStatus avif_gain_map_validate_decode_images(
    const AvifGainMapDecodePlan *plan,
    const AvifdecImage *base_image,
    const AvifdecImage *gain_map_image,
    AvifdecError *error) {
    avif_gain_map_error_clear(error);
    if (plan == 0 || base_image == 0 || gain_map_image == 0 ||
        plan->info.present != 1U) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (!avif_gain_map_image_valid(
            &plan->info.base_image, base_image) ||
        !avif_gain_map_image_valid(
            &plan->info.gain_map_image, gain_map_image) ||
        plan->info.gain_map_image.has_alpha != 0U ||
        gain_map_image->alpha_plane != 0) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_gain_map_execute_decode_plan(
    const AvifGainMapDecodePlan *plan,
    const AvifGainMapChildDecoder *decoder,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *base_image,
    AvifdecImage *gain_map_image,
    AvifdecEntropyTrace *base_trace,
    AvifdecEntropyTrace *gain_map_trace,
    AvifdecError *error) {
    AvifdecStatus status;

    avif_gain_map_error_clear(error);
    if (plan == 0 || decoder == 0 || decoder->decode == 0 ||
        base_image == 0 || gain_map_image == 0 ||
        plan->info.present != 1U ||
        (workspace == 0 && workspace_size != 0U)) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (workspace_size < plan->info.workspace_required) {
        return avif_gain_map_fail(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    if (plan->info.workspace_required != 0U && workspace == 0) {
        return avif_gain_map_fail(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    status = avif_gain_map_validate_decode_images(
        plan, base_image, gain_map_image, error);
    if (status != AVIFDEC_OK) return status;

    status = decoder->decode(
        decoder->context, plan->info.base_item_id, executor,
        workspace, workspace_size, base_image, base_trace, error);
    status = avif_gain_map_callback_failure(error, status, 0U);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = decoder->decode(
        decoder->context, plan->info.gain_map_item_id, executor,
        workspace, workspace_size, gain_map_image,
        gain_map_trace, error);
    status = avif_gain_map_callback_failure(error, status, 0U);
    if (status != AVIFDEC_OK) {
        return status;
    }
    return AVIFDEC_OK;
}

static int avif_gain_map_log2_double(float value, double *result) {
    AvifGainMapFloatBits bits;
    int exponent_adjustment = 0;
    int exponent;
    double mantissa;
    double z;
    double z_squared;
    double term;
    double sum;
    unsigned int divisor;

    if (result == 0 || !avif_gain_map_float_finite(value) ||
        value <= 0.0f) {
        return 0;
    }
    bits.value = value;
    if ((bits.bits & 0x7f800000U) == 0U) {
        value *= 8388608.0f;
        exponent_adjustment = -23;
        bits.value = value;
    }
    exponent =
        (int)((bits.bits >> 23) & 0xffU) - 127 +
        exponent_adjustment;
    bits.bits = (bits.bits & 0x007fffffU) | 0x3f800000U;
    mantissa = (double)bits.value;
    z = (mantissa - 1.0) / (mantissa + 1.0);
    z_squared = z * z;
    term = z;
    sum = term;
    for (divisor = 3U; divisor <= 15U; divisor += 2U) {
        term *= z_squared;
        sum += term / (double)divisor;
    }
    *result =
        (double)exponent +
        (2.0 * sum) / 0.693147180559945309417232121458176568;
    return 1;
}

static int avif_gain_map_exp2_double(double exponent, float *result) {
    AvifGainMapFloatBits scale;
    int integer_exponent;
    double fraction;
    double t;
    double polynomial;
    double value;

    if (result == 0 || !(exponent == exponent)) return 0;
    if (exponent <= -150.0) {
        *result = 0.0f;
        return 1;
    }
    if (exponent < -149.0) {
        scale.bits = 1U;
        *result = scale.value;
        return 1;
    }
    if (exponent >= 128.0) return 0;

    integer_exponent = (int)exponent;
    if ((double)integer_exponent > exponent) --integer_exponent;
    fraction = exponent - (double)integer_exponent;
    t = fraction *
        0.693147180559945309417232121458176568;

    polynomial = 1.0 / 479001600.0;
    polynomial = 1.0 / 39916800.0 + t * polynomial;
    polynomial = 1.0 / 3628800.0 + t * polynomial;
    polynomial = 1.0 / 362880.0 + t * polynomial;
    polynomial = 1.0 / 40320.0 + t * polynomial;
    polynomial = 1.0 / 5040.0 + t * polynomial;
    polynomial = 1.0 / 720.0 + t * polynomial;
    polynomial = 1.0 / 120.0 + t * polynomial;
    polynomial = 1.0 / 24.0 + t * polynomial;
    polynomial = 1.0 / 6.0 + t * polynomial;
    polynomial = 0.5 + t * polynomial;
    polynomial = 1.0 + t * polynomial;
    polynomial = 1.0 + t * polynomial;

    if (integer_exponent >= -126) {
        scale.bits =
            (uint32_t)(integer_exponent + 127) << 23;
    } else {
        scale.bits =
            (uint32_t)1U << (unsigned int)(integer_exponent + 149);
    }
    value = (double)scale.value * polynomial;
    if (value > AVIF_GAIN_MAP_FLOAT_MAX) return 0;
    *result = (float)value;
    return avif_gain_map_float_finite(*result);
}

static int avif_gain_map_pow_double(float base,
                                    double exponent,
                                    float *result) {
    double logarithm;

    if (result == 0 || !avif_gain_map_float_finite(base) ||
        base < 0.0f || base > 1.0f ||
        !(exponent > 0.0) || !(exponent == exponent)) {
        return 0;
    }
    if (base == 0.0f) {
        *result = 0.0f;
        return 1;
    }
    if (base == 1.0f) {
        *result = 1.0f;
        return 1;
    }
    if (!avif_gain_map_log2_double(base, &logarithm)) return 0;
    return avif_gain_map_exp2_double(logarithm * exponent, result);
}

AvifdecStatus avif_gain_map_approx_exp2(
    float exponent,
    float *result) {
    if (result != 0) *result = 0.0f;
    if (result == 0 || !avif_gain_map_float_finite(exponent)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!avif_gain_map_exp2_double((double)exponent, result)) {
        return AVIFDEC_INVALID_DATA;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_gain_map_approx_pow(
    float base,
    float exponent,
    float *result) {
    if (result != 0) *result = 0.0f;
    if (result == 0 || !avif_gain_map_float_finite(exponent) ||
        exponent <= 0.0f) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!avif_gain_map_pow_double(base, (double)exponent, result)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    return AVIFDEC_OK;
}

static double avif_gain_map_rational_double(
    AvifGainMapRational rational) {
    return (double)rational.numerator /
           (double)rational.denominator;
}

static double avif_gain_map_unsigned_rational_double(
    AvifGainMapUnsignedRational rational) {
    return (double)rational.numerator /
           (double)rational.denominator;
}

static void avif_gain_map_multiply_divide(
    uint64_t left,
    uint64_t right,
    uint64_t denominator,
    uint64_t *quotient,
    uint64_t *remainder) {
    uint64_t base_quotient = right / denominator;
    uint64_t base_remainder = right % denominator;
    uint64_t result_quotient = 0U;
    uint64_t result_remainder = 0U;
    unsigned int bit_index;

    for (bit_index = 64U; bit_index != 0U; --bit_index) {
        uint64_t bit = (left >> (bit_index - 1U)) & 1U;
        uint64_t next_remainder =
            result_remainder * 2U +
            (bit != 0U ? base_remainder : 0U);

        result_quotient =
            result_quotient * 2U +
            (bit != 0U ? base_quotient : 0U) +
            next_remainder / denominator;
        result_remainder = next_remainder % denominator;
    }
    *quotient = result_quotient;
    *remainder = result_remainder;
}

static void avif_gain_map_sample_axis(uint32_t position,
                                      uint32_t output_dimension,
                                      uint32_t map_dimension,
                                      uint32_t *first,
                                      uint32_t *second,
                                      double *fraction) {
    uint64_t denominator = (uint64_t)output_dimension * 2U;
    uint64_t center = (uint64_t)position * 2U + 1U;
    uint64_t quotient;
    uint64_t remainder;
    uint64_t half = output_dimension;
    uint64_t index;
    uint64_t fractional_numerator;

    if (map_dimension == 1U) {
        *first = 0U;
        *second = 0U;
        *fraction = 0.0;
        return;
    }
    avif_gain_map_multiply_divide(
        center, map_dimension, denominator,
        &quotient, &remainder);
    if (remainder >= half) {
        index = quotient;
        fractional_numerator = remainder - half;
    } else if (quotient != 0U) {
        index = quotient - 1U;
        fractional_numerator = remainder + half;
    } else {
        *first = 0U;
        *second = 0U;
        *fraction = 0.0;
        return;
    }
    if (index >= (uint64_t)map_dimension - 1U) {
        *first = map_dimension - 1U;
        *second = map_dimension - 1U;
        *fraction = 0.0;
        return;
    }
    *first = (uint32_t)index;
    *second = (uint32_t)index + 1U;
    *fraction =
        (double)fractional_numerator / (double)denominator;
}

static AvifdecStatus avif_gain_map_read_texel(
    const AvifGainMapColorAdapter *color,
    const AvifdecImage *gain_map_image,
    const AvifdecImageInfo *gain_map_info,
    uint32_t x,
    uint32_t y,
    uint8_t channel_count,
    float values[3],
    AvifdecError *error) {
    size_t channel;
    AvifdecStatus status;

    values[0] = avif_gain_map_nan();
    values[1] = avif_gain_map_nan();
    values[2] = avif_gain_map_nan();
    status = color->gain_texel(
        color->context, gain_map_image, gain_map_info,
        x, y, channel_count, values, error);
    status = avif_gain_map_callback_failure(error, status, 0U);
    if (status != AVIFDEC_OK) return status;
    for (channel = 0U; channel < channel_count; ++channel) {
        if (!avif_gain_map_float_finite(values[channel]) ||
            values[channel] < 0.0f || values[channel] > 1.0f) {
            return avif_gain_map_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_gain_map_sample_gain(
    const AvifGainMapColorAdapter *color,
    const AvifdecImage *gain_map_image,
    const AvifdecImageInfo *gain_map_info,
    uint32_t output_x,
    uint32_t output_y,
    uint32_t output_width,
    uint32_t output_height,
    uint8_t channel_count,
    float result[3],
    AvifdecError *error) {
    uint32_t x0;
    uint32_t x1;
    uint32_t y0;
    uint32_t y1;
    double x_fraction;
    double y_fraction;
    float samples[4][3];
    size_t channel;
    AvifdecStatus status;

    avif_gain_map_sample_axis(
        output_x, output_width,
        gain_map_info->presentation_width,
        &x0, &x1, &x_fraction);
    avif_gain_map_sample_axis(
        output_y, output_height,
        gain_map_info->presentation_height,
        &y0, &y1, &y_fraction);

    status = avif_gain_map_read_texel(
        color, gain_map_image, gain_map_info,
        x0, y0, channel_count, samples[0], error);
    if (status != AVIFDEC_OK) return status;
    if (x1 != x0) {
        status = avif_gain_map_read_texel(
            color, gain_map_image, gain_map_info,
            x1, y0, channel_count, samples[1], error);
        if (status != AVIFDEC_OK) return status;
    } else {
        for (channel = 0U; channel < channel_count; ++channel) {
            samples[1][channel] = samples[0][channel];
        }
    }
    if (y1 != y0) {
        status = avif_gain_map_read_texel(
            color, gain_map_image, gain_map_info,
            x0, y1, channel_count, samples[2], error);
        if (status != AVIFDEC_OK) return status;
    } else {
        for (channel = 0U; channel < channel_count; ++channel) {
            samples[2][channel] = samples[0][channel];
        }
    }
    if (x1 != x0 && y1 != y0) {
        status = avif_gain_map_read_texel(
            color, gain_map_image, gain_map_info,
            x1, y1, channel_count, samples[3], error);
        if (status != AVIFDEC_OK) return status;
    } else if (x1 != x0) {
        for (channel = 0U; channel < channel_count; ++channel) {
            samples[3][channel] = samples[1][channel];
        }
    } else {
        for (channel = 0U; channel < channel_count; ++channel) {
            samples[3][channel] = samples[2][channel];
        }
    }

    for (channel = 0U; channel < channel_count; ++channel) {
        double top =
            (1.0 - x_fraction) * (double)samples[0][channel] +
            x_fraction * (double)samples[1][channel];
        double bottom =
            (1.0 - x_fraction) * (double)samples[2][channel] +
            x_fraction * (double)samples[3][channel];
        double value =
            (1.0 - y_fraction) * top + y_fraction * bottom;

        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        result[channel] = (float)value;
    }
    if (channel_count == 1U) {
        result[1] = result[0];
        result[2] = result[0];
    }
    return AVIFDEC_OK;
}

static uint16_t avif_gain_map_alpha_u16(float alpha) {
    return (uint16_t)((uint32_t)
        ((double)alpha * 65535.0 + 0.5));
}

static uint16_t avif_gain_map_premultiply_u16(uint16_t value,
                                              uint16_t alpha) {
    return (uint16_t)(
        ((uint64_t)value * alpha + 32767U) / 65535U);
}

static void avif_gain_map_store_u16(unsigned char *destination,
                                    uint16_t value) {
    avif_gain_map_copy(destination, &value, sizeof(value));
}

static void avif_gain_map_store_float(unsigned char *destination,
                                      float value) {
    avif_gain_map_copy(destination, &value, sizeof(value));
}

static AvifdecStatus avif_gain_map_apply_rows(
    const AvifdecImage *base_image,
    const AvifdecImage *gain_map_image,
    const AvifGainMapInfo *info,
    const AvifGainMapColorAdapter *color,
    const AvifGainMapApplyOptions *options,
    AvifdecRgbImage *output,
    uint32_t first_row,
    uint32_t row_count,
    AvifdecError *error) {
    const AvifGainMapColorDescription *working_color;
    uint32_t width;
    uint32_t height;
    size_t channel_count;
    size_t bytes_per_channel;
    size_t bytes_per_pixel;
    size_t row_bytes;
    size_t last_row_offset;
    size_t output_size;
    double display_log2;
    double base_headroom;
    double alternate_headroom;
    double weight;
    uint32_t y;
    int float_output;
    int alpha_output;
    AvifdecStatus status;

    avif_gain_map_error_clear(error);
    if (base_image == 0 || gain_map_image == 0 || info == 0 ||
        color == 0 || options == 0 || output == 0 ||
        output->pixels == 0 || row_count == 0U ||
        info->present != 1U || options->flags != 0U ||
        !avif_gain_map_float_finite(options->display_headroom) ||
        options->display_headroom <= 0.0f ||
        color->validate_transform == 0 ||
        color->base_to_working == 0 || color->gain_texel == 0 ||
        output->alpha_mode > AVIFDEC_ALPHA_PREMULTIPLIED) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (output->format != AVIFDEC_RGB16 &&
        output->format != AVIFDEC_RGBA16 &&
        output->format != AVIF_GAIN_MAP_RGBF32 &&
        output->format != AVIF_GAIN_MAP_RGBAF32) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    float_output =
        output->format == AVIF_GAIN_MAP_RGBF32 ||
        output->format == AVIF_GAIN_MAP_RGBAF32;
    alpha_output =
        output->format == AVIFDEC_RGBA16 ||
        output->format == AVIF_GAIN_MAP_RGBAF32;
    if ((float_output && color->working_to_linear == 0) ||
        (!float_output && color->working_to_encoded16 == 0)) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_gain_map_metadata_validate(&info->metadata, error);
    if (status != AVIFDEC_OK) return status;
    if (!avif_gain_map_image_valid(&info->base_image, base_image) ||
        !avif_gain_map_image_valid(
            &info->gain_map_image, gain_map_image) ||
        info->gain_map_image.has_alpha != 0U ||
        gain_map_image->alpha_plane != 0 ||
        info->base_is_hdr != info->metadata.backward_direction ||
        (info->metadata.channel_count == 1U &&
         (info->gain_map_image.monochrome != 1U ||
          info->gain_map_image.channel_count != 1U)) ||
        (info->metadata.channel_count == 3U &&
         (info->gain_map_image.monochrome != 0U ||
          info->gain_map_image.channel_count != 3U)) ||
        !avif_gain_map_transforms_equal(
            &info->base_image, &info->gain_map_image) ||
        !avif_gain_map_color_valid(&info->base_color) ||
        !avif_gain_map_color_valid(&info->alternate_color)) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }

    width = info->base_image.presentation_width;
    height = info->base_image.presentation_height;
    if (width == 0U || height == 0U ||
        info->gain_map_image.presentation_width == 0U ||
        info->gain_map_image.presentation_height == 0U ||
        output->width != width || output->height != height ||
        first_row >= height || row_count > height - first_row) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    channel_count = alpha_output ? 4U : 3U;
    bytes_per_channel =
        float_output ? sizeof(float) : sizeof(uint16_t);
    if (!avif_gain_map_size_multiply(
            channel_count, bytes_per_channel, &bytes_per_pixel) ||
        !avif_gain_map_size_multiply(
            (size_t)width, bytes_per_pixel, &row_bytes)) {
        return avif_gain_map_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    if (output->stride < row_bytes) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (!avif_gain_map_size_multiply(
            (size_t)row_count - 1U, output->stride,
            &last_row_offset) ||
        !avif_gain_map_size_add(
            last_row_offset, row_bytes, &output_size)) {
        return avif_gain_map_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    (void)output_size;

    working_color = info->metadata.use_base_color_space != 0U
        ? &info->base_color : &info->alternate_color;
    status = color->validate_transform(
        color->context, working_color, output->format, error);
    status = avif_gain_map_callback_failure(error, status, 0U);
    if (status != AVIFDEC_OK) return status;
    if (!avif_gain_map_log2_double(
            options->display_headroom, &display_log2)) {
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    base_headroom = avif_gain_map_unsigned_rational_double(
        info->metadata.base_hdr_headroom);
    alternate_headroom = avif_gain_map_unsigned_rational_double(
        info->metadata.alternate_hdr_headroom);
    if (avif_gain_map_unsigned_rational_equal(
            info->metadata.base_hdr_headroom,
            info->metadata.alternate_hdr_headroom)) {
        weight = 0.0;
    } else {
        double fraction =
            (display_log2 - base_headroom) /
            (alternate_headroom - base_headroom);

        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;
        weight = info->metadata.backward_direction != 0U
            ? -fraction : fraction;
    }

    for (y = first_row; y < first_row + row_count; ++y) {
        unsigned char *output_row =
            (unsigned char *)output->pixels +
            (size_t)(y - first_row) * output->stride;
        uint32_t x;

        for (x = 0U; x < width; ++x) {
            float base_rgba[4];
            float gain[3] = { 0.0f, 0.0f, 0.0f };
            float working_rgb[3];
            float alpha;
            unsigned char *pixel =
                output_row + (size_t)x * bytes_per_pixel;
            size_t channel;

            base_rgba[0] = avif_gain_map_nan();
            base_rgba[1] = avif_gain_map_nan();
            base_rgba[2] = avif_gain_map_nan();
            base_rgba[3] = avif_gain_map_nan();
            status = color->base_to_working(
                color->context, base_image, &info->base_image,
                working_color, x, y, base_rgba, error);
            status = avif_gain_map_callback_failure(
                error, status, 0U);
            if (status != AVIFDEC_OK) return status;
            for (channel = 0U; channel < 4U; ++channel) {
                if (!avif_gain_map_float_finite(base_rgba[channel])) {
                    return avif_gain_map_fail(
                        error, AVIFDEC_INVALID_DATA, 0U, 0U);
                }
            }
            alpha = info->base_image.has_alpha != 0U
                ? base_rgba[3] : 1.0f;
            if (alpha < 0.0f || alpha > 1.0f) {
                return avif_gain_map_fail(
                    error, AVIFDEC_INVALID_DATA, 0U, 0U);
            }

            if (weight == 0.0) {
                working_rgb[0] = base_rgba[0];
                working_rgb[1] = base_rgba[1];
                working_rgb[2] = base_rgba[2];
            } else {
                status = avif_gain_map_sample_gain(
                    color, gain_map_image, &info->gain_map_image,
                    x, y, width, height,
                    info->metadata.channel_count, gain, error);
                if (status != AVIFDEC_OK) return status;
                for (channel = 0U; channel < 3U; ++channel) {
                    const size_t metadata_channel =
                        info->metadata.channel_count == 1U
                            ? 0U : channel;
                    double gamma_inverse =
                        (double)info->metadata
                            .gain_map_gamma[metadata_channel]
                            .denominator /
                        (double)info->metadata
                            .gain_map_gamma[metadata_channel]
                            .numerator;
                    float decoded_gain;
                    double minimum = avif_gain_map_rational_double(
                        info->metadata
                            .gain_map_min[metadata_channel]);
                    double maximum = avif_gain_map_rational_double(
                        info->metadata
                            .gain_map_max[metadata_channel]);
                    double gain_log;
                    float factor;
                    double mapped;

                    if (!avif_gain_map_pow_double(
                            gain[metadata_channel],
                            gamma_inverse, &decoded_gain)) {
                        return avif_gain_map_fail(
                            error, AVIFDEC_INVALID_DATA, 0U, 0U);
                    }
                    gain_log =
                        minimum + (maximum - minimum) *
                        (double)decoded_gain;
                    if (!avif_gain_map_exp2_double(
                            gain_log * weight, &factor)) {
                        return avif_gain_map_fail(
                            error, AVIFDEC_INVALID_DATA, 0U, 0U);
                    }
                    mapped =
                        ((double)base_rgba[channel] +
                         avif_gain_map_rational_double(
                             info->metadata
                                 .base_offset[metadata_channel])) *
                            (double)factor -
                        avif_gain_map_rational_double(
                            info->metadata
                                .alternate_offset[metadata_channel]);
                    if (!(mapped == mapped) ||
                        mapped > AVIF_GAIN_MAP_FLOAT_MAX ||
                        mapped < -AVIF_GAIN_MAP_FLOAT_MAX) {
                        return avif_gain_map_fail(
                            error, AVIFDEC_INVALID_DATA, 0U, 0U);
                    }
                    working_rgb[channel] = (float)mapped;
                    if (!avif_gain_map_float_finite(
                            working_rgb[channel])) {
                        return avif_gain_map_fail(
                            error, AVIFDEC_INVALID_DATA, 0U, 0U);
                    }
                }
            }

            if (float_output) {
                float converted[3];

                converted[0] = avif_gain_map_nan();
                converted[1] = avif_gain_map_nan();
                converted[2] = avif_gain_map_nan();
                status = color->working_to_linear(
                    color->context, working_rgb, converted, error);
                status = avif_gain_map_callback_failure(
                    error, status, 0U);
                if (status != AVIFDEC_OK) return status;
                for (channel = 0U; channel < 3U; ++channel) {
                    if (!avif_gain_map_float_finite(
                            converted[channel])) {
                        return avif_gain_map_fail(
                            error, AVIFDEC_INVALID_DATA, 0U, 0U);
                    }
                    if (output->alpha_mode ==
                        AVIFDEC_ALPHA_PREMULTIPLIED) {
                        converted[channel] *= alpha;
                    }
                }
                for (channel = 0U; channel < 3U; ++channel) {
                    avif_gain_map_store_float(
                        pixel + channel * sizeof(float),
                        converted[channel]);
                }
                if (alpha_output) {
                    avif_gain_map_store_float(
                        pixel + 3U * sizeof(float), alpha);
                }
            } else {
                uint16_t converted[3] = { 0U, 0U, 0U };
                uint16_t alpha_u16 =
                    avif_gain_map_alpha_u16(alpha);

                status = color->working_to_encoded16(
                    color->context, working_rgb, converted, error);
                status = avif_gain_map_callback_failure(
                    error, status, 0U);
                if (status != AVIFDEC_OK) return status;
                for (channel = 0U; channel < 3U; ++channel) {
                    if (output->alpha_mode ==
                        AVIFDEC_ALPHA_PREMULTIPLIED) {
                        converted[channel] =
                            avif_gain_map_premultiply_u16(
                                converted[channel], alpha_u16);
                    }
                    avif_gain_map_store_u16(
                        pixel + channel * sizeof(uint16_t),
                        converted[channel]);
                }
                if (alpha_output) {
                    avif_gain_map_store_u16(
                        pixel + 3U * sizeof(uint16_t), alpha_u16);
                }
            }
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_gain_map_apply(
    const AvifdecImage *base_image,
    const AvifdecImage *gain_map_image,
    const AvifGainMapInfo *info,
    const AvifGainMapColorAdapter *color,
    const AvifGainMapApplyOptions *options,
    AvifdecRgbImage *output,
    AvifdecError *error) {
    if (output == 0) {
        avif_gain_map_error_clear(error);
        return avif_gain_map_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return avif_gain_map_apply_rows(
        base_image, gain_map_image, info, color, options,
        output, 0U, output->height, error);
}

AvifdecStatus avif_gain_map_apply_row(
    const AvifdecImage *base_image,
    const AvifdecImage *gain_map_image,
    const AvifGainMapInfo *info,
    const AvifGainMapColorAdapter *color,
    const AvifGainMapApplyOptions *options,
    AvifdecRgbImage *output,
    uint32_t row,
    AvifdecError *error) {
    return avif_gain_map_apply_rows(
        base_image, gain_map_image, info, color, options,
        output, row, 1U, error);
}
