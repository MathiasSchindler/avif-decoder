#include "base.h"

const char *avifdec_status_string(AvifdecStatus status) {
    switch (status) {
        case AVIFDEC_OK: return "ok";
        case AVIFDEC_INVALID_ARGUMENT: return "invalid argument";
        case AVIFDEC_TRUNCATED: return "truncated input";
        case AVIFDEC_INVALID_DATA: return "invalid data";
        case AVIFDEC_OVERFLOW: return "integer overflow";
        case AVIFDEC_LIMIT_EXCEEDED: return "limit exceeded";
        case AVIFDEC_OUT_OF_MEMORY: return "out of memory";
        case AVIFDEC_IO_ERROR: return "I/O error";
        case AVIFDEC_UNSUPPORTED: return "unsupported feature";
    }
    return "unknown error";
}

uint64_t avifdec_capabilities(void) {
    return AVIFDEC_CAP_AV1_LOW_OVERHEAD |
           AVIFDEC_CAP_AV1_ANNEX_B |
           AVIFDEC_CAP_AV1_OPERATING_POINTS |
           AVIFDEC_CAP_AV1_METADATA |
           AVIFDEC_CAP_AV1_PROFILE_LEVELS |
           AVIFDEC_CAP_AV1_FILM_GRAIN |
           AVIFDEC_CAP_AVIF_PRESENTATION |
           AVIFDEC_CAP_AVIF_ALPHA |
           AVIFDEC_CAP_AVIF_GRID |
           AVIFDEC_CAP_AVIF_LAYERED |
           AVIFDEC_CAP_AVIF_SAMPLE_TRANSFORM |
           AVIFDEC_CAP_RGB_CONVERSION |
           AVIFDEC_CAP_AVIF_TONE_MAP_METADATA |
           AVIFDEC_CAP_AVIF_SEQUENCE |
           AVIFDEC_CAP_PARALLEL_EXECUTOR |
           AVIFDEC_CAP_AVIF_METADATA |
           AVIFDEC_CAP_COLOR_CICP |
           AVIFDEC_CAP_COLOR_ICC_MATRIX_TRC |
           AVIFDEC_CAP_AVIF_GAIN_MAP_METADATA |
           AVIFDEC_CAP_AVIF_GAIN_MAP_APPLICATION |
           AVIFDEC_CAP_AVIF_SEQUENCE_INDEX |
           AVIFDEC_CAP_AVIF_SEQUENCE_EDITS |
           AVIFDEC_CAP_AVIF_SEQUENCE_FRAGMENTS |
           AVIFDEC_CAP_AVIF_SEQUENCE_TRACK_SELECTION;
}

const char *avifdec_version_string(void) {
    return "1.4.0";
}

void avifdec_limits_default(AvifdecLimits *limits) {
    if (limits == 0) return;
    avifdec_memory_fill(limits, 0U, sizeof(*limits));
    limits->max_width = 32768U;
    limits->max_height = 32768U;
    limits->max_pixels = 268435456U;
    limits->max_items = AVIFDEC_DEFAULT_MAX_ITEMS;
    limits->max_extents = AVIFDEC_DEFAULT_MAX_EXTENTS;
    limits->max_properties = AVIFDEC_DEFAULT_MAX_PROPERTIES;
    limits->max_obus = AVIFDEC_DEFAULT_MAX_OBUS;
    limits->max_frames = AVIFDEC_DEFAULT_MAX_FRAMES;
    limits->max_metadata_items = AVIFDEC_DEFAULT_MAX_METADATA_ITEMS;
    limits->max_metadata_spans = AVIFDEC_DEFAULT_MAX_METADATA_SPANS;
    limits->max_tracks = AVIFDEC_DEFAULT_MAX_TRACKS;
    limits->max_edits = AVIFDEC_DEFAULT_MAX_EDITS;
    limits->max_fragments = AVIFDEC_DEFAULT_MAX_FRAGMENTS;
    limits->max_icc_bytes = AVIFDEC_DEFAULT_MAX_ICC_BYTES;
    limits->max_icc_curve_entries =
        AVIFDEC_DEFAULT_MAX_ICC_CURVE_ENTRIES;
    limits->av1_framing = AVIFDEC_AV1_LOW_OVERHEAD;
}

AvifdecLimits avifdec_limits_effective(const AvifdecLimits *limits) {
    AvifdecLimits result;

    avifdec_limits_default(&result);
    if (limits == 0) return result;
    if (limits->max_width != 0U) result.max_width = limits->max_width;
    if (limits->max_height != 0U) result.max_height = limits->max_height;
    if (limits->max_pixels != 0U) result.max_pixels = limits->max_pixels;
    if (limits->max_items != 0U) result.max_items = limits->max_items;
    if (limits->max_extents != 0U) result.max_extents = limits->max_extents;
    if (limits->max_properties != 0U) {
        result.max_properties = limits->max_properties;
    }
    if (limits->max_obus != 0U) result.max_obus = limits->max_obus;
    if (limits->max_frames != 0U) result.max_frames = limits->max_frames;
    if (limits->max_metadata_items != 0U) {
        result.max_metadata_items = limits->max_metadata_items;
    }
    if (limits->max_metadata_spans != 0U) {
        result.max_metadata_spans = limits->max_metadata_spans;
    }
    if (limits->max_tracks != 0U) result.max_tracks = limits->max_tracks;
    if (limits->max_edits != 0U) result.max_edits = limits->max_edits;
    if (limits->max_fragments != 0U) {
        result.max_fragments = limits->max_fragments;
    }
    if (limits->max_icc_bytes != 0U) {
        result.max_icc_bytes = limits->max_icc_bytes;
    }
    if (limits->max_icc_curve_entries != 0U) {
        result.max_icc_curve_entries = limits->max_icc_curve_entries;
    }
    result.operating_point = limits->operating_point;
    result.av1_framing = limits->av1_framing;
    result.spatial_layer = limits->spatial_layer;
    result.spatial_layer_set = limits->spatial_layer_set;
    return result;
}

int avifdec_size_add(size_t left, size_t right, size_t *result) {
    if (result == 0 || right > SIZE_MAX - left) {
        return 0;
    }
    *result = left + right;
    return 1;
}

int avifdec_size_multiply(size_t left, size_t right, size_t *result) {
    if (result == 0 || (left != 0U && right > SIZE_MAX / left)) {
        return 0;
    }
    *result = left * right;
    return 1;
}

int avifdec_size_align(size_t value, size_t alignment, size_t *result) {
    size_t mask;

    if (result == 0 || alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        return 0;
    }
    mask = alignment - 1U;
    if (value > SIZE_MAX - mask) {
        return 0;
    }
    *result = (value + mask) & ~mask;
    return 1;
}

void avifdec_memory_copy(void *destination, const void *source, size_t count) {
    unsigned char *output = (unsigned char *)destination;
    const unsigned char *input = (const unsigned char *)source;
    size_t index;

    for (index = 0U; index < count; ++index) {
        output[index] = input[index];
    }
}

#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
void *memcpy(void *destination, const void *source, size_t count) {
    avifdec_memory_copy(destination, source, count);
    return destination;
}
#endif

void avifdec_memory_fill(void *destination, unsigned char value, size_t count) {
    unsigned char *output = (unsigned char *)destination;
    size_t index;

    for (index = 0U; index < count; ++index) {
        output[index] = value;
    }
}

int avifdec_memory_compare(const void *left, const void *right, size_t count) {
    const unsigned char *left_bytes = (const unsigned char *)left;
    const unsigned char *right_bytes = (const unsigned char *)right;
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (left_bytes[index] < right_bytes[index]) return -1;
        if (left_bytes[index] > right_bytes[index]) return 1;
    }
    return 0;
}

uint16_t avifdec_load_u16be(const unsigned char *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

uint32_t avifdec_load_u32be(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

uint64_t avifdec_load_u64be(const unsigned char *bytes) {
    return ((uint64_t)avifdec_load_u32be(bytes) << 32) |
           (uint64_t)avifdec_load_u32be(bytes + 4U);
}

static void byte_reader_fail(AvifdecByteReader *reader, AvifdecStatus status) {
    if (reader->status == AVIFDEC_OK) {
        reader->status = status;
    }
}

void avifdec_byte_reader_init(AvifdecByteReader *reader,
                              const void *data,
                              size_t size,
                              size_t base_offset) {
    size_t end_offset;

    if (reader == 0) return;
    reader->data = (const unsigned char *)data;
    reader->size = size;
    reader->position = 0U;
    reader->base_offset = base_offset;
    reader->status = AVIFDEC_OK;
    if ((data == 0 && size != 0U) || !avifdec_size_add(base_offset, size, &end_offset)) {
        reader->status = AVIFDEC_INVALID_ARGUMENT;
    }
}

size_t avifdec_byte_reader_offset(const AvifdecByteReader *reader) {
    return reader->base_offset + reader->position;
}

size_t avifdec_byte_reader_remaining(const AvifdecByteReader *reader) {
    if (reader->position > reader->size) return 0U;
    return reader->size - reader->position;
}

const unsigned char *avifdec_byte_reader_take(AvifdecByteReader *reader, size_t count) {
    const unsigned char *result;

    if (reader == 0 || reader->status != AVIFDEC_OK) return 0;
    if (count > avifdec_byte_reader_remaining(reader)) {
        byte_reader_fail(reader, AVIFDEC_TRUNCATED);
        return 0;
    }
    result = reader->data == 0 ? 0 : reader->data + reader->position;
    reader->position += count;
    return result;
}

uint8_t avifdec_byte_reader_u8(AvifdecByteReader *reader) {
    const unsigned char *bytes = avifdec_byte_reader_take(reader, 1U);
    return bytes == 0 ? 0U : bytes[0];
}

uint16_t avifdec_byte_reader_u16be(AvifdecByteReader *reader) {
    const unsigned char *bytes = avifdec_byte_reader_take(reader, 2U);
    return bytes == 0 ? 0U : avifdec_load_u16be(bytes);
}

uint32_t avifdec_byte_reader_u32be(AvifdecByteReader *reader) {
    const unsigned char *bytes = avifdec_byte_reader_take(reader, 4U);
    return bytes == 0 ? 0U : avifdec_load_u32be(bytes);
}

uint64_t avifdec_byte_reader_u64be(AvifdecByteReader *reader) {
    const unsigned char *bytes = avifdec_byte_reader_take(reader, 8U);
    return bytes == 0 ? 0U : avifdec_load_u64be(bytes);
}

AvifdecStatus avifdec_byte_reader_skip(AvifdecByteReader *reader, size_t count) {
    (void)avifdec_byte_reader_take(reader, count);
    return reader == 0 ? AVIFDEC_INVALID_ARGUMENT : reader->status;
}

AvifdecStatus avifdec_byte_reader_subreader(AvifdecByteReader *reader,
                                            size_t count,
                                            AvifdecByteReader *subreader) {
    size_t start;
    const unsigned char *data;

    if (reader == 0 || subreader == 0) return AVIFDEC_INVALID_ARGUMENT;
    start = avifdec_byte_reader_offset(reader);
    data = avifdec_byte_reader_take(reader, count);
    if (reader->status != AVIFDEC_OK) {
        avifdec_byte_reader_init(subreader, 0, 0U, start);
        subreader->status = reader->status;
        return reader->status;
    }
    avifdec_byte_reader_init(subreader, data, count, start);
    return subreader->status;
}

static void bit_reader_fail(AvifdecBitReader *reader, AvifdecStatus status) {
    if (reader->status == AVIFDEC_OK) {
        reader->status = status;
    }
}

void avifdec_bit_reader_init(AvifdecBitReader *reader,
                             const void *data,
                             size_t size,
                             size_t base_offset) {
    size_t end_offset;

    if (reader == 0) return;
    reader->data = (const unsigned char *)data;
    reader->size = size;
    reader->bit_position = 0U;
    reader->base_offset = base_offset;
    reader->status = AVIFDEC_OK;
    if ((data == 0 && size != 0U) || !avifdec_size_add(base_offset, size, &end_offset)) {
        reader->status = AVIFDEC_INVALID_ARGUMENT;
    }
}

size_t avifdec_bit_reader_offset(const AvifdecBitReader *reader) {
    return reader->base_offset + (reader->bit_position >> 3);
}

uint32_t avifdec_bit_reader_read(AvifdecBitReader *reader, unsigned int bit_count) {
    uint32_t value = 0U;
    unsigned int index;

    if (reader == 0 || reader->status != AVIFDEC_OK) return 0U;
    if (bit_count > 32U) {
        bit_reader_fail(reader, AVIFDEC_INVALID_ARGUMENT);
        return 0U;
    }
    for (index = 0U; index < bit_count; ++index) {
        size_t byte_index = reader->bit_position >> 3;
        unsigned int shift;

        if (byte_index >= reader->size) {
            bit_reader_fail(reader, AVIFDEC_TRUNCATED);
            return 0U;
        }
        shift = 7U - (unsigned int)(reader->bit_position & 7U);
        value = (value << 1) | ((uint32_t)(reader->data[byte_index] >> shift) & 1U);
        ++reader->bit_position;
    }
    return value;
}

AvifdecStatus avifdec_bit_reader_skip(AvifdecBitReader *reader, size_t bit_count) {
    size_t remaining = bit_count;

    if (reader == 0) return AVIFDEC_INVALID_ARGUMENT;
    while (remaining != 0U && reader->status == AVIFDEC_OK) {
        unsigned int chunk = remaining > 32U ? 32U : (unsigned int)remaining;
        (void)avifdec_bit_reader_read(reader, chunk);
        remaining -= chunk;
    }
    return reader->status;
}

AvifdecStatus avifdec_bit_reader_align(AvifdecBitReader *reader) {
    size_t remainder;

    if (reader == 0) return AVIFDEC_INVALID_ARGUMENT;
    remainder = reader->bit_position & 7U;
    if (remainder != 0U) {
        (void)avifdec_bit_reader_skip(reader, 8U - remainder);
    }
    return reader->status;
}

void avifdec_arena_init(AvifdecArena *arena, void *data, size_t size) {
    if (arena == 0) return;
    arena->data = (unsigned char *)data;
    arena->size = size;
    arena->used = 0U;
    arena->status = data == 0 && size != 0U ? AVIFDEC_INVALID_ARGUMENT : AVIFDEC_OK;
    arena->sizing_only = 0;
}

void avifdec_arena_init_sizing(AvifdecArena *arena) {
    if (arena == 0) return;
    arena->data = 0;
    arena->size = 0U;
    arena->used = 0U;
    arena->status = AVIFDEC_OK;
    arena->sizing_only = 1;
}

void *avifdec_arena_allocate(AvifdecArena *arena, size_t size, size_t alignment) {
    size_t mask;
    size_t base_modulo;
    size_t used_modulo;
    size_t padding;
    size_t aligned;
    size_t end;

    if (arena == 0 || arena->status != AVIFDEC_OK) return 0;
    if (alignment == 0U ||
        (alignment & (alignment - 1U)) != 0U) {
        arena->status = AVIFDEC_INVALID_ARGUMENT;
        return 0;
    }
    mask = alignment - 1U;
    base_modulo = arena->sizing_only
        ? 0U : (size_t)((uintptr_t)arena->data & mask);
    used_modulo = arena->used & mask;
    padding = (alignment - ((base_modulo + used_modulo) & mask)) & mask;
    if (!avifdec_size_add(arena->used, padding, &aligned) ||
        !avifdec_size_add(aligned, size, &end)) {
        arena->status = AVIFDEC_OVERFLOW;
        return 0;
    }
    arena->used = end;
    if (arena->sizing_only) return 0;
    if (end > arena->size) {
        arena->status = AVIFDEC_OUT_OF_MEMORY;
        return 0;
    }
    return arena->data + aligned;
}

size_t avifdec_arena_required(const AvifdecArena *arena) {
    return arena == 0 ? 0U : arena->used;
}