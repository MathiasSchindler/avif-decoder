#include "av1_avif_conformance.h"

#define AV1_AVIF_OBU_TEMPORAL_DELIMITER 2U
#define AV1_AVIF_OBU_TILE_LIST 8U
#define AV1_AVIF_OBU_PADDING 15U

typedef struct {
    const AvifdecSpan *spans;
    size_t span_count;
    size_t span_index;
    size_t span_position;
    size_t position;
    size_t size;
} Av1AvifReader;

static uint32_t av1_avif_obu_context(uint8_t obu_type) {
    return ((uint32_t)'O' << 24) |
           ((uint32_t)'B' << 16) |
           ((uint32_t)'U' << 8) |
           (uint32_t)obu_type;
}

static AvifdecStatus av1_avif_fail(AvifdecError *error,
                                   AvifdecStatus status,
                                   size_t offset,
                                   uint8_t obu_type) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = av1_avif_obu_context(obu_type);
    }
    return status;
}

static void av1_avif_reader_normalize(Av1AvifReader *reader) {
    while (reader->span_index < reader->span_count &&
           reader->span_position ==
               reader->spans[reader->span_index].size) {
        ++reader->span_index;
        reader->span_position = 0U;
    }
}

static size_t av1_avif_reader_offset(const Av1AvifReader *reader) {
    size_t index = reader->span_index;
    size_t span_position = reader->span_position;

    while (index < reader->span_count &&
           span_position == reader->spans[index].size) {
        ++index;
        span_position = 0U;
    }
    if (index < reader->span_count) {
        return reader->spans[index].file_offset + span_position;
    }
    if (reader->span_count != 0U) {
        const AvifdecSpan *last = &reader->spans[reader->span_count - 1U];

        return last->file_offset + last->size;
    }
    return 0U;
}

static AvifdecStatus av1_avif_reader_init(
    Av1AvifReader *reader,
    const AvifdecSpan *spans,
    size_t span_count,
    AvifdecError *error) {
    size_t index;
    size_t size = 0U;

    if (reader == 0 || (spans == 0 && span_count != 0U)) {
        return av1_avif_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    for (index = 0U; index < span_count; ++index) {
        if (spans[index].data == 0 && spans[index].size != 0U) {
            return av1_avif_fail(
                error, AVIFDEC_INVALID_ARGUMENT,
                spans[index].file_offset, 0U);
        }
        if (spans[index].size > SIZE_MAX - spans[index].file_offset ||
            spans[index].size > SIZE_MAX - size) {
            return av1_avif_fail(
                error, AVIFDEC_OVERFLOW, spans[index].file_offset, 0U);
        }
        size += spans[index].size;
    }
    reader->spans = spans;
    reader->span_count = span_count;
    reader->span_index = 0U;
    reader->span_position = 0U;
    reader->position = 0U;
    reader->size = size;
    av1_avif_reader_normalize(reader);
    return AVIFDEC_OK;
}

static AvifdecStatus av1_avif_reader_read(Av1AvifReader *reader,
                                          size_t limit,
                                          uint8_t *value) {
    const AvifdecSpan *span;

    if (reader->position >= limit || reader->position >= reader->size) {
        return AVIFDEC_TRUNCATED;
    }
    av1_avif_reader_normalize(reader);
    if (reader->span_index >= reader->span_count) {
        return AVIFDEC_TRUNCATED;
    }
    span = &reader->spans[reader->span_index];
    *value = span->data[reader->span_position];
    ++reader->span_position;
    ++reader->position;
    av1_avif_reader_normalize(reader);
    return AVIFDEC_OK;
}

static int av1_avif_reader_skip(Av1AvifReader *reader, size_t count) {
    if (count > reader->size - reader->position) return 0;
    while (count != 0U) {
        const AvifdecSpan *span;
        size_t available;
        size_t step;

        av1_avif_reader_normalize(reader);
        if (reader->span_index >= reader->span_count) return 0;
        span = &reader->spans[reader->span_index];
        available = span->size - reader->span_position;
        step = count < available ? count : available;
        reader->span_position += step;
        reader->position += step;
        count -= step;
    }
    av1_avif_reader_normalize(reader);
    return 1;
}

static AvifdecStatus av1_avif_leb128(Av1AvifReader *reader,
                                     size_t limit,
                                     AvifdecStatus boundary_status,
                                     size_t *value) {
    size_t result = 0U;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        uint8_t byte;
        size_t bits;
        unsigned int shift = index * 7U;

        if (reader->position >= limit) {
            return reader->position >= reader->size
                       ? AVIFDEC_TRUNCATED
                       : boundary_status;
        }
        if (av1_avif_reader_read(reader, limit, &byte) != AVIFDEC_OK) {
            return AVIFDEC_TRUNCATED;
        }
        bits = (size_t)(byte & 0x7fU);
        if (shift >= sizeof(size_t) * 8U ||
            bits > (SIZE_MAX >> shift)) {
            return AVIFDEC_OVERFLOW;
        }
        result |= bits << shift;
        if ((byte & 0x80U) == 0U) {
            *value = result;
            return AVIFDEC_OK;
        }
    }
    return AVIFDEC_INVALID_DATA;
}

static AvifdecStatus av1_avif_validate_obu(
    Av1AvifReader *reader,
    uint8_t framing,
    size_t obu_end,
    int *first_obu_in_temporal_unit,
    AvifdecError *error) {
    size_t header_offset = av1_avif_reader_offset(reader);
    size_t payload_size;
    size_t payload_end;
    uint8_t header;
    uint8_t obu_type;
    uint8_t extension_flag;
    uint8_t has_size_field;
    AvifdecStatus status;

    status = av1_avif_reader_read(reader, obu_end, &header);
    if (status != AVIFDEC_OK) {
        return av1_avif_fail(
            error, status, header_offset, 0U);
    }
    obu_type = (uint8_t)((header >> 3U) & 15U);
    extension_flag = (uint8_t)((header >> 2U) & 1U);
    has_size_field = (uint8_t)((header >> 1U) & 1U);
    if ((header & 0x81U) != 0U ||
        (framing == AVIFDEC_AV1_LOW_OVERHEAD && !has_size_field)) {
        return av1_avif_fail(
            error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
    }
    if (obu_type == AV1_AVIF_OBU_TILE_LIST) {
        return av1_avif_fail(
            error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
    }
    if (extension_flag) {
        uint8_t extension;

        if (reader->position >= obu_end) {
            status = reader->position >= reader->size
                         ? AVIFDEC_TRUNCATED
                         : AVIFDEC_INVALID_DATA;
            return av1_avif_fail(
                error, status, header_offset, obu_type);
        }
        status = av1_avif_reader_read(reader, obu_end, &extension);
        if (status != AVIFDEC_OK) {
            return av1_avif_fail(
                error, status, header_offset, obu_type);
        }
        if ((extension & 7U) != 0U) {
            return av1_avif_fail(
                error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
        }
    }
    if (has_size_field) {
        status = av1_avif_leb128(
            reader, obu_end,
            framing == AVIFDEC_AV1_ANNEX_B
                ? AVIFDEC_INVALID_DATA
                : AVIFDEC_TRUNCATED,
            &payload_size);
        if (status != AVIFDEC_OK) {
            return av1_avif_fail(
                error, status, header_offset, obu_type);
        }
    } else {
        payload_size = obu_end - reader->position;
    }
    if (framing == AVIFDEC_AV1_ANNEX_B) {
        if (payload_size != obu_end - reader->position) {
            return av1_avif_fail(
                error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
        }
        payload_end = obu_end;
        if (first_obu_in_temporal_unit != 0 &&
            *first_obu_in_temporal_unit) {
            if (obu_type != AV1_AVIF_OBU_TEMPORAL_DELIMITER) {
                return av1_avif_fail(
                    error, AVIFDEC_INVALID_DATA,
                    header_offset, obu_type);
            }
            *first_obu_in_temporal_unit = 0;
        } else if (obu_type == AV1_AVIF_OBU_TEMPORAL_DELIMITER) {
            return av1_avif_fail(
                error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
        }
    } else {
        if (payload_size > reader->size - reader->position) {
            return av1_avif_fail(
                error, AVIFDEC_TRUNCATED, header_offset, obu_type);
        }
        payload_end = reader->position + payload_size;
    }
    if (obu_type == AV1_AVIF_OBU_TEMPORAL_DELIMITER &&
        payload_size != 0U) {
        return av1_avif_fail(
            error, AVIFDEC_INVALID_DATA, header_offset, obu_type);
    }
    if (obu_type == AV1_AVIF_OBU_PADDING) {
        while (reader->position < payload_end) {
            uint8_t padding_byte;

            status = av1_avif_reader_read(
                reader, payload_end, &padding_byte);
            if (status != AVIFDEC_OK) {
                return av1_avif_fail(
                    error, status, header_offset, obu_type);
            }
            if (padding_byte != 0U) {
                return av1_avif_fail(
                    error, AVIFDEC_INVALID_DATA,
                    header_offset, obu_type);
            }
        }
    } else if (!av1_avif_reader_skip(reader, payload_size)) {
        return av1_avif_fail(
            error, AVIFDEC_TRUNCATED, header_offset, obu_type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_avif_validate_low_overhead(
    Av1AvifReader *reader,
    AvifdecError *error) {
    while (reader->position < reader->size) {
        AvifdecStatus status = av1_avif_validate_obu(
            reader, AVIFDEC_AV1_LOW_OVERHEAD,
            reader->size, 0, error);

        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus av1_avif_validate_annex_b(
    Av1AvifReader *reader,
    AvifdecError *error) {
    while (reader->position < reader->size) {
        size_t temporal_prefix_offset =
            av1_avif_reader_offset(reader);
        size_t temporal_unit_size;
        size_t temporal_unit_end;
        int first_obu_in_temporal_unit = 1;
        AvifdecStatus status = av1_avif_leb128(
            reader, reader->size, AVIFDEC_TRUNCATED,
            &temporal_unit_size);

        if (status != AVIFDEC_OK) {
            return av1_avif_fail(
                error, status, temporal_prefix_offset, 0U);
        }
        if (temporal_unit_size == 0U ||
            temporal_unit_size > reader->size - reader->position) {
            return av1_avif_fail(
                error, AVIFDEC_INVALID_DATA,
                temporal_prefix_offset, 0U);
        }
        temporal_unit_end = reader->position + temporal_unit_size;
        while (reader->position < temporal_unit_end) {
            size_t frame_prefix_offset =
                av1_avif_reader_offset(reader);
            size_t frame_unit_size;
            size_t frame_unit_end;

            status = av1_avif_leb128(
                reader, temporal_unit_end,
                AVIFDEC_INVALID_DATA, &frame_unit_size);
            if (status != AVIFDEC_OK) {
                return av1_avif_fail(
                    error, status, frame_prefix_offset, 0U);
            }
            if (frame_unit_size == 0U ||
                frame_unit_size >
                    temporal_unit_end - reader->position) {
                return av1_avif_fail(
                    error, AVIFDEC_INVALID_DATA,
                    frame_prefix_offset, 0U);
            }
            frame_unit_end = reader->position + frame_unit_size;
            while (reader->position < frame_unit_end) {
                size_t obu_prefix_offset =
                    av1_avif_reader_offset(reader);
                size_t obu_size;
                size_t obu_end;

                status = av1_avif_leb128(
                    reader, frame_unit_end,
                    AVIFDEC_INVALID_DATA, &obu_size);
                if (status != AVIFDEC_OK) {
                    return av1_avif_fail(
                        error, status, obu_prefix_offset, 0U);
                }
                if (obu_size == 0U ||
                    obu_size > frame_unit_end - reader->position) {
                    return av1_avif_fail(
                        error, AVIFDEC_INVALID_DATA,
                        obu_prefix_offset, 0U);
                }
                obu_end = reader->position + obu_size;
                status = av1_avif_validate_obu(
                    reader, AVIFDEC_AV1_ANNEX_B, obu_end,
                    &first_obu_in_temporal_unit, error);
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_avif_validate_obu_stream(
    const AvifdecSpan *spans,
    size_t span_count,
    uint8_t framing,
    AvifdecError *error) {
    Av1AvifReader reader;
    AvifdecStatus status;

    if (framing > AVIFDEC_AV1_ANNEX_B) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = av1_avif_reader_init(
        &reader, spans, span_count, error);
    if (status != AVIFDEC_OK) return status;
    if (reader.size == 0U) {
        return av1_avif_fail(
            error, AVIFDEC_INVALID_DATA,
            av1_avif_reader_offset(&reader), 0U);
    }
    if (framing == AVIFDEC_AV1_ANNEX_B) {
        return av1_avif_validate_annex_b(&reader, error);
    }
    return av1_avif_validate_low_overhead(&reader, error);
}

AvifdecStatus av1_avif_validate_large_scale_tile(
    int large_scale_tile) {
    return large_scale_tile
               ? AVIFDEC_INVALID_DATA
               : AVIFDEC_OK;
}
