#include "png.h"
#include "base.h"

#define PNG_ADLER_MODULUS 65521U
#define PNG_ADLER_BATCH 5552U
#define PNG_IDAT_BUFFER_SIZE 32768U
#define PNG_LZ77_HASH_SIZE 65536U
#define PNG_LZ77_WINDOW_SIZE 32768U
#define PNG_LZ77_EMPTY 0xffffffffU
#define PNG_LZ77_PROBES 8U

typedef struct {
    AvifdecPngWrite write_callback;
    void *user_data;
    uint32_t crc_table[256];
    uint32_t chunk_crc;
} PngWriter;

typedef struct {
    PngWriter *writer;
    unsigned char *data;
    size_t capacity;
    size_t size;
} PngIdatSink;

typedef struct {
    PngIdatSink *sink;
    uint32_t *head;
    uint32_t *previous;
    unsigned char *history;
    uint32_t position;
    uint32_t bit_buffer;
    unsigned int bit_count;
    uint32_t adler_a;
    uint32_t adler_b;
} PngDeflater;

typedef struct {
    uint32_t length;
    uint32_t distance;
} PngMatch;

static void png_store_u32(unsigned char output[4], uint32_t value) {
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static AvifdecStatus png_write_bytes(
    PngWriter *writer, const void *data, size_t size) {
    if (size != 0U &&
        writer->write_callback(writer->user_data, data, size) != 0) {
        return AVIFDEC_IO_ERROR;
    }
    return AVIFDEC_OK;
}

static void png_crc_initialize(PngWriter *writer) {
    uint32_t index;

    for (index = 0U; index < 256U; ++index) {
        uint32_t value = index;
        unsigned int bit;

        for (bit = 0U; bit < 8U; ++bit) {
            value = (value & 1U) != 0U
                ? 0xedb88320U ^ (value >> 1U)
                : value >> 1U;
        }
        writer->crc_table[index] = value;
    }
}

static void png_crc_update(
    PngWriter *writer, const unsigned char *data, size_t size) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        writer->chunk_crc =
            writer->crc_table[(writer->chunk_crc ^ data[index]) & 0xffU] ^
            (writer->chunk_crc >> 8U);
    }
}

static AvifdecStatus png_chunk_begin(
    PngWriter *writer, uint32_t length, const unsigned char type[4]) {
    unsigned char encoded_length[4];
    AvifdecStatus status;

    png_store_u32(encoded_length, length);
    status = png_write_bytes(writer, encoded_length, sizeof(encoded_length));
    if (status != AVIFDEC_OK) return status;
    status = png_write_bytes(writer, type, 4U);
    if (status != AVIFDEC_OK) return status;
    writer->chunk_crc = 0xffffffffU;
    png_crc_update(writer, type, 4U);
    return AVIFDEC_OK;
}

static AvifdecStatus png_chunk_data(
    PngWriter *writer, const void *data, size_t size) {
    AvifdecStatus status = png_write_bytes(writer, data, size);

    if (status == AVIFDEC_OK) {
        png_crc_update(writer, (const unsigned char *)data, size);
    }
    return status;
}

static AvifdecStatus png_chunk_end(PngWriter *writer) {
    unsigned char encoded_crc[4];

    png_store_u32(encoded_crc, writer->chunk_crc ^ 0xffffffffU);
    return png_write_bytes(writer, encoded_crc, sizeof(encoded_crc));
}

static AvifdecStatus png_write_chunk(
    PngWriter *writer,
    const unsigned char type[4],
    const void *data,
    uint32_t size) {
    AvifdecStatus status = png_chunk_begin(writer, size, type);

    if (status == AVIFDEC_OK) status = png_chunk_data(writer, data, size);
    if (status == AVIFDEC_OK) status = png_chunk_end(writer);
    return status;
}

static AvifdecStatus png_idat_flush(PngIdatSink *sink) {
    static const unsigned char type[4] = { 'I', 'D', 'A', 'T' };
    AvifdecStatus status;

    if (sink->size == 0U) return AVIFDEC_OK;
    status = png_write_chunk(
        sink->writer, type, sink->data, (uint32_t)sink->size);
    if (status == AVIFDEC_OK) sink->size = 0U;
    return status;
}

static AvifdecStatus png_idat_emit(
    PngIdatSink *sink, const unsigned char *data, size_t size) {
    while (size != 0U) {
        size_t available = sink->capacity - sink->size;
        size_t count = size < available ? size : available;

        avifdec_memory_copy(sink->data + sink->size, data, count);
        sink->size += count;
        data += count;
        size -= count;
        if (sink->size == sink->capacity) {
            AvifdecStatus status = png_idat_flush(sink);

            if (status != AVIFDEC_OK) return status;
        }
    }
    return AVIFDEC_OK;
}

static uint32_t png_reverse_bits(uint32_t value, unsigned int count) {
    uint32_t reversed = 0U;
    unsigned int index;

    for (index = 0U; index < count; ++index) {
        reversed = (reversed << 1U) | (value & 1U);
        value >>= 1U;
    }
    return reversed;
}

static AvifdecStatus png_deflate_write_bits(
    PngDeflater *deflater, uint32_t value, unsigned int count) {
    if (count > 16U) return AVIFDEC_INVALID_ARGUMENT;
    deflater->bit_buffer |= value << deflater->bit_count;
    deflater->bit_count += count;
    while (deflater->bit_count >= 8U) {
        unsigned char byte =
            (unsigned char)(deflater->bit_buffer & 0xffU);
        AvifdecStatus status =
            png_idat_emit(deflater->sink, &byte, 1U);

        if (status != AVIFDEC_OK) return status;
        deflater->bit_buffer >>= 8U;
        deflater->bit_count -= 8U;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus png_deflate_flush_bits(PngDeflater *deflater) {
    if (deflater->bit_count != 0U) {
        unsigned char byte =
            (unsigned char)(deflater->bit_buffer & 0xffU);
        AvifdecStatus status =
            png_idat_emit(deflater->sink, &byte, 1U);

        if (status != AVIFDEC_OK) return status;
        deflater->bit_buffer = 0U;
        deflater->bit_count = 0U;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus png_deflate_fixed_symbol(
    PngDeflater *deflater, uint32_t symbol) {
    uint32_t code;
    unsigned int length;

    if (symbol <= 143U) {
        code = 0x30U + symbol;
        length = 8U;
    } else if (symbol <= 255U) {
        code = 0x190U + (symbol - 144U);
        length = 9U;
    } else if (symbol <= 279U) {
        code = symbol - 256U;
        length = 7U;
    } else if (symbol <= 287U) {
        code = 0xc0U + (symbol - 280U);
        length = 8U;
    } else {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    return png_deflate_write_bits(
        deflater, png_reverse_bits(code, length), length);
}

static AvifdecStatus png_deflate_fixed_match(
    PngDeflater *deflater, uint32_t length, uint32_t distance) {
    static const uint16_t length_base[29] = {
        3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U, 15U, 17U,
        19U, 23U, 27U, 31U, 35U, 43U, 51U, 59U, 67U, 83U, 99U,
        115U, 131U, 163U, 195U, 227U, 258U
    };
    static const uint8_t length_extra[29] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U,
        2U, 2U, 2U, 2U, 3U, 3U, 3U, 3U, 4U, 4U, 4U, 4U,
        5U, 5U, 5U, 5U, 0U
    };
    static const uint16_t distance_base[30] = {
        1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U, 17U, 25U, 33U, 49U,
        65U, 97U, 129U, 193U, 257U, 385U, 513U, 769U, 1025U,
        1537U, 2049U, 3073U, 4097U, 6145U, 8193U, 12289U,
        16385U, 24577U
    };
    static const uint8_t distance_extra[30] = {
        0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U, 4U, 4U,
        5U, 5U, 6U, 6U, 7U, 7U, 8U, 8U, 9U, 9U, 10U, 10U,
        11U, 11U, 12U, 12U, 13U, 13U
    };
    unsigned int length_index;
    unsigned int distance_index;
    AvifdecStatus status;

    if (length < 3U || length > 258U ||
        distance == 0U || distance > PNG_LZ77_WINDOW_SIZE) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (length_index = 0U; length_index < 29U; ++length_index) {
        uint32_t maximum = length_base[length_index] +
            (((uint32_t)1U << length_extra[length_index]) - 1U);

        if (length <= maximum) break;
    }
    for (distance_index = 0U; distance_index < 30U;
         ++distance_index) {
        uint32_t maximum = distance_base[distance_index] +
            (((uint32_t)1U << distance_extra[distance_index]) - 1U);

        if (distance <= maximum) break;
    }
    if (length_index == 29U || distance_index == 30U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = png_deflate_fixed_symbol(
        deflater, 257U + length_index);
    if (status != AVIFDEC_OK) return status;
    if (length_extra[length_index] != 0U) {
        status = png_deflate_write_bits(
            deflater, length - length_base[length_index],
            length_extra[length_index]);
        if (status != AVIFDEC_OK) return status;
    }
    status = png_deflate_write_bits(
        deflater, png_reverse_bits(distance_index, 5U), 5U);
    if (status != AVIFDEC_OK) return status;
    if (distance_extra[distance_index] != 0U) {
        status = png_deflate_write_bits(
            deflater, distance - distance_base[distance_index],
            distance_extra[distance_index]);
    }
    return status;
}

static void png_adler_update(
    PngDeflater *deflater, const unsigned char *data, size_t size) {
    while (size != 0U) {
        size_t count = size > PNG_ADLER_BATCH ? PNG_ADLER_BATCH : size;
        size_t index;

        for (index = 0U; index < count; ++index) {
            deflater->adler_a += data[index];
            deflater->adler_b += deflater->adler_a;
        }
        deflater->adler_a %= PNG_ADLER_MODULUS;
        deflater->adler_b %= PNG_ADLER_MODULUS;
        data += count;
        size -= count;
    }
}

static AvifdecStatus png_deflate_begin(PngDeflater *deflater) {
    static const unsigned char zlib_header[2] = { 0x78U, 0x01U };
    AvifdecStatus status;

    deflater->position = 0U;
    deflater->bit_buffer = 0U;
    deflater->bit_count = 0U;
    deflater->adler_a = 1U;
    deflater->adler_b = 0U;
    status = png_idat_emit(
        deflater->sink, zlib_header, sizeof(zlib_header));
    if (status != AVIFDEC_OK) return status;
    return png_deflate_write_bits(deflater, 3U, 3U);
}

static AvifdecStatus png_deflate_finish(PngDeflater *deflater) {
    unsigned char adler[4];
    AvifdecStatus status =
        png_deflate_fixed_symbol(deflater, 256U);

    if (status != AVIFDEC_OK) return status;
    status = png_deflate_flush_bits(deflater);
    if (status != AVIFDEC_OK) return status;
    png_store_u32(
        adler, (deflater->adler_b << 16U) | deflater->adler_a);
    status = png_idat_emit(deflater->sink, adler, sizeof(adler));
    if (status != AVIFDEC_OK) return status;
    return png_idat_flush(deflater->sink);
}

static uint32_t png_lz77_hash(const unsigned char *data) {
    uint32_t value =
        ((uint32_t)data[0] << 16U) ^
        ((uint32_t)data[1] << 8U) ^
        data[2];

    value ^= value >> 9U;
    value *= 2654435761U;
    return (value >> 16U) & (PNG_LZ77_HASH_SIZE - 1U);
}

static void png_lz77_insert(PngDeflater *deflater,
                            const unsigned char *data,
                            size_t size,
                            size_t offset) {
    uint32_t position;
    uint32_t hash;

    if (offset + 2U >= size) return;
    position = deflater->position + (uint32_t)offset;
    hash = png_lz77_hash(data + offset);
    deflater->previous[
        position & (PNG_LZ77_WINDOW_SIZE - 1U)] =
        deflater->head[hash];
    deflater->head[hash] = position;
}

static unsigned char png_lz77_source_byte(
    const PngDeflater *deflater,
    const unsigned char *data,
    uint32_t absolute_position) {
    if (absolute_position >= deflater->position) {
        return data[absolute_position - deflater->position];
    }
    return deflater->history[
        absolute_position & (PNG_LZ77_WINDOW_SIZE - 1U)];
}

static PngMatch png_lz77_find_match(
    const PngDeflater *deflater,
    const unsigned char *data,
    size_t size,
    size_t offset) {
    PngMatch best = { 0U, 0U };

    if (offset + 2U < size) {
        uint32_t current =
            deflater->position + (uint32_t)offset;
        uint32_t candidate =
            deflater->head[png_lz77_hash(data + offset)];
        uint32_t maximum = (uint32_t)(
            size - offset > 258U ? 258U : size - offset);
        unsigned int probe;

        for (probe = 0U;
             probe < PNG_LZ77_PROBES &&
             candidate != PNG_LZ77_EMPTY;
             ++probe) {
            uint32_t distance;
            uint32_t next;
            uint32_t length = 0U;

            if (candidate >= current) break;
            distance = current - candidate;
            if (distance > PNG_LZ77_WINDOW_SIZE) break;
            next = deflater->previous[
                candidate & (PNG_LZ77_WINDOW_SIZE - 1U)];
            while (length < maximum &&
                   png_lz77_source_byte(
                       deflater, data, candidate + length) ==
                   data[offset + length]) {
                ++length;
            }
            if (length >= 3U && length > best.length) {
                best.length = length;
                best.distance = distance;
                if (length == maximum) break;
            }
            candidate = next;
        }
    }
    return best;
}

static AvifdecStatus png_deflate_feed_lz77(
    PngDeflater *deflater, const unsigned char *data, size_t size) {
    size_t offset = 0U;

    if (size > UINT32_MAX - deflater->position) {
        return AVIFDEC_OVERFLOW;
    }
    png_adler_update(deflater, data, size);
    while (offset < size) {
        PngMatch match =
            png_lz77_find_match(deflater, data, size, offset);
        size_t count;
        size_t index;
        AvifdecStatus status;

        png_lz77_insert(deflater, data, size, offset);
        if (match.length >= 3U) {
            status = png_deflate_fixed_match(
                deflater, match.length, match.distance);
            count = match.length;
            for (index = 1U; index < count; ++index) {
                png_lz77_insert(
                    deflater, data, size, offset + index);
            }
        } else {
            status = png_deflate_fixed_symbol(
                deflater, data[offset]);
            count = 1U;
        }
        if (status != AVIFDEC_OK) return status;
        for (index = 0U; index < count; ++index) {
            uint32_t position =
                deflater->position + (uint32_t)(offset + index);

            deflater->history[
                position & (PNG_LZ77_WINDOW_SIZE - 1U)] =
                data[offset + index];
        }
        offset += count;
    }
    deflater->position += (uint32_t)size;
    return AVIFDEC_OK;
}

static AvifdecStatus png_deflate_feed_literals(
    PngDeflater *deflater, const unsigned char *data, size_t size) {
    size_t index;

    if (size > UINT32_MAX - deflater->position) {
        return AVIFDEC_OVERFLOW;
    }
    png_adler_update(deflater, data, size);
    for (index = 0U; index < size; ++index) {
        AvifdecStatus status =
            png_deflate_fixed_symbol(deflater, data[index]);

        if (status != AVIFDEC_OK) return status;
    }
    deflater->position += (uint32_t)size;
    return AVIFDEC_OK;
}

static int png_paeth(int left, int above, int upper_left) {
    int estimate = left + above - upper_left;
    int left_distance = estimate > left
        ? estimate - left : left - estimate;
    int above_distance = estimate > above
        ? estimate - above : above - estimate;
    int corner_distance = estimate > upper_left
        ? estimate - upper_left : upper_left - estimate;

    if (left_distance <= above_distance &&
        left_distance <= corner_distance) {
        return left;
    }
    return above_distance <= corner_distance ? above : upper_left;
}

static uint64_t png_filter_score(unsigned char value) {
    return value < 128U ? value : 256U - value;
}

static unsigned int png_choose_filter(
    const unsigned char *current,
    const unsigned char *previous,
    size_t row_bytes,
    size_t bytes_per_pixel) {
    uint64_t scores[5] = { 0U, 0U, 0U, 0U, 0U };
    size_t index;
    unsigned int filter;
    unsigned int best = 0U;

    for (index = 0U; index < row_bytes; ++index) {
        unsigned int value = current[index];
        unsigned int left =
            index >= bytes_per_pixel
                ? current[index - bytes_per_pixel] : 0U;
        unsigned int above = previous[index];
        unsigned int upper_left =
            index >= bytes_per_pixel
                ? previous[index - bytes_per_pixel] : 0U;

        scores[0] += png_filter_score((unsigned char)value);
        scores[1] += png_filter_score(
            (unsigned char)(value - left));
        scores[2] += png_filter_score(
            (unsigned char)(value - above));
        scores[3] += png_filter_score(
            (unsigned char)(
                value - ((left + above) >> 1U)));
        scores[4] += png_filter_score(
            (unsigned char)(
                value - (unsigned int)png_paeth(
                    (int)left, (int)above, (int)upper_left)));
    }
    for (filter = 1U; filter < 5U; ++filter) {
        if (scores[filter] < scores[best]) best = filter;
    }
    return best;
}

static void png_apply_filter(
    unsigned int filter,
    const unsigned char *current,
    const unsigned char *previous,
    unsigned char *output,
    size_t row_bytes,
    size_t bytes_per_pixel) {
    size_t index;

    output[0] = (unsigned char)filter;
    for (index = 0U; index < row_bytes; ++index) {
        unsigned int value = current[index];
        unsigned int left =
            index >= bytes_per_pixel
                ? current[index - bytes_per_pixel] : 0U;
        unsigned int above = previous[index];
        unsigned int upper_left =
            index >= bytes_per_pixel
                ? previous[index - bytes_per_pixel] : 0U;
        unsigned int predictor = 0U;

        if (filter == 1U) predictor = left;
        else if (filter == 2U) predictor = above;
        else if (filter == 3U) predictor = (left + above) >> 1U;
        else if (filter == 4U) {
            predictor = (unsigned int)png_paeth(
                (int)left, (int)above, (int)upper_left);
        }
        output[index + 1U] =
            (unsigned char)(value - predictor);
    }
}

static AvifdecStatus png_write_header(
    PngWriter *writer,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    uint8_t bit_depth,
    const AvifdecPngMetadata *metadata) {
    static const unsigned char signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU
    };
    static const unsigned char ihdr_type[4] = { 'I', 'H', 'D', 'R' };
    static const unsigned char phys_type[4] = { 'p', 'H', 'Y', 's' };
    static const unsigned char cicp_type[4] = { 'c', 'I', 'C', 'P' };
    unsigned char ihdr[13];
    AvifdecStatus status =
        png_write_bytes(writer, signature, sizeof(signature));

    if (status != AVIFDEC_OK) return status;
    png_store_u32(ihdr, width);
    png_store_u32(ihdr + 4U, height);
    ihdr[8] = bit_depth;
    ihdr[9] = (unsigned char)(channels == 4U ? 6U : 2U);
    ihdr[10] = 0U;
    ihdr[11] = 0U;
    ihdr[12] = 0U;
    status = png_write_chunk(
        writer, ihdr_type, ihdr, sizeof(ihdr));
    if (status != AVIFDEC_OK) return status;
    if (metadata != 0 &&
        metadata->pixel_aspect_h_spacing != 0U &&
        metadata->pixel_aspect_v_spacing != 0U) {
        unsigned char phys[9];

        png_store_u32(
            phys, metadata->pixel_aspect_v_spacing);
        png_store_u32(
            phys + 4U, metadata->pixel_aspect_h_spacing);
        phys[8] = 0U;
        status = png_write_chunk(
            writer, phys_type, phys, sizeof(phys));
        if (status != AVIFDEC_OK) return status;
    }
    if (metadata != 0 && metadata->has_nclx != 0U &&
        metadata->color_primaries <= 255U &&
        metadata->transfer_characteristics <= 255U) {
        unsigned char cicp[4];

        cicp[0] = (unsigned char)metadata->color_primaries;
        cicp[1] =
            (unsigned char)metadata->transfer_characteristics;
        cicp[2] = 0U;
        cicp[3] = 1U;
        status = png_write_chunk(
            writer, cicp_type, cicp, sizeof(cicp));
    }
    return status;
}

static AvifdecStatus png_write_end(PngWriter *writer) {
    static const unsigned char type[4] = { 'I', 'E', 'N', 'D' };

    return png_write_chunk(writer, type, 0, 0U);
}

static AvifdecStatus png_row_bytes(
    uint32_t width,
    uint8_t channels,
    uint8_t bit_depth,
    size_t *row_bytes) {
    size_t bytes_per_channel;

    if (width == 0U || row_bytes == 0 ||
        (channels != 3U && channels != 4U) ||
        (bit_depth != 8U && bit_depth != 16U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    bytes_per_channel = bit_depth == 16U ? 2U : 1U;
    if (!avifdec_size_multiply(width, channels, row_bytes) ||
        !avifdec_size_multiply(
            *row_bytes, bytes_per_channel, row_bytes)) {
        return AVIFDEC_OVERFLOW;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_png_workspace_requirement(
    uint32_t width,
    uint8_t channels,
    uint8_t bit_depth,
    size_t *required) {
    AvifdecArena sizing;
    size_t row_bytes;
    size_t filtered_bytes;
    AvifdecStatus status =
        png_row_bytes(width, channels, bit_depth, &row_bytes);

    if (status != AVIFDEC_OK || required == 0) {
        return required == 0 ? AVIFDEC_INVALID_ARGUMENT : status;
    }
    if (!avifdec_size_add(row_bytes, 1U, &filtered_bytes)) {
        return AVIFDEC_OVERFLOW;
    }
    avifdec_arena_init_sizing(&sizing);
    (void)avifdec_arena_allocate(
        &sizing, PNG_LZ77_HASH_SIZE * sizeof(uint32_t),
        _Alignof(uint32_t));
    (void)avifdec_arena_allocate(
        &sizing, PNG_LZ77_WINDOW_SIZE * sizeof(uint32_t),
        _Alignof(uint32_t));
    (void)avifdec_arena_allocate(
        &sizing, PNG_LZ77_WINDOW_SIZE, 1U);
    (void)avifdec_arena_allocate(&sizing, row_bytes, 1U);
    (void)avifdec_arena_allocate(&sizing, row_bytes, 1U);
    (void)avifdec_arena_allocate(&sizing, filtered_bytes, 1U);
    (void)avifdec_arena_allocate(
        &sizing, PNG_IDAT_BUFFER_SIZE, 1U);
    if (sizing.status != AVIFDEC_OK ||
        !avifdec_size_add(
            avifdec_arena_required(&sizing),
            _Alignof(uint32_t) - 1U, required)) {
        return AVIFDEC_OVERFLOW;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_png_write_rows(
    AvifdecPngWrite write_callback,
    void *write_user_data,
    AvifdecPngReadRow read_row,
    void *row_user_data,
    void *workspace,
    size_t workspace_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    uint8_t bit_depth,
    const AvifdecPngMetadata *metadata) {
    PngWriter writer;
    PngIdatSink sink;
    PngDeflater deflater;
    AvifdecArena arena;
    unsigned char *current;
    unsigned char *previous_row;
    unsigned char *filtered;
    size_t row_bytes;
    size_t required;
    size_t bytes_per_pixel;
    size_t raw_size;
    uint32_t row;
    AvifdecStatus status;

    if (write_callback == 0 || read_row == 0 ||
        workspace == 0 || height == 0U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = png_row_bytes(
        width, channels, bit_depth, &row_bytes);
    if (status != AVIFDEC_OK) return status;
    if (!avifdec_size_add(row_bytes, 1U, &raw_size) ||
        !avifdec_size_multiply(raw_size, height, &raw_size) ||
        raw_size > UINT32_MAX) {
        return AVIFDEC_OVERFLOW;
    }
    status = avifdec_png_workspace_requirement(
        width, channels, bit_depth, &required);
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < required) return AVIFDEC_OUT_OF_MEMORY;

    avifdec_arena_init(&arena, workspace, workspace_size);
    deflater.head = (uint32_t *)avifdec_arena_allocate(
        &arena, PNG_LZ77_HASH_SIZE * sizeof(uint32_t),
        _Alignof(uint32_t));
    deflater.previous = (uint32_t *)avifdec_arena_allocate(
        &arena, PNG_LZ77_WINDOW_SIZE * sizeof(uint32_t),
        _Alignof(uint32_t));
    deflater.history = (unsigned char *)avifdec_arena_allocate(
        &arena, PNG_LZ77_WINDOW_SIZE, 1U);
    current = (unsigned char *)avifdec_arena_allocate(
        &arena, row_bytes, 1U);
    previous_row = (unsigned char *)avifdec_arena_allocate(
        &arena, row_bytes, 1U);
    filtered = (unsigned char *)avifdec_arena_allocate(
        &arena, row_bytes + 1U, 1U);
    sink.data = (unsigned char *)avifdec_arena_allocate(
        &arena, PNG_IDAT_BUFFER_SIZE, 1U);
    if (arena.status != AVIFDEC_OK) return arena.status;

    writer.write_callback = write_callback;
    writer.user_data = write_user_data;
    writer.chunk_crc = 0U;
    png_crc_initialize(&writer);
    sink.writer = &writer;
    sink.capacity = PNG_IDAT_BUFFER_SIZE;
    sink.size = 0U;
    deflater.sink = &sink;
    avifdec_memory_fill(
        deflater.head, 0xffU,
        PNG_LZ77_HASH_SIZE * sizeof(uint32_t));
    avifdec_memory_fill(
        deflater.previous, 0xffU,
        PNG_LZ77_WINDOW_SIZE * sizeof(uint32_t));
    avifdec_memory_fill(previous_row, 0U, row_bytes);

    status = png_write_header(
        &writer, width, height, channels, bit_depth, metadata);
    if (status != AVIFDEC_OK) return status;
    status = png_deflate_begin(&deflater);
    if (status != AVIFDEC_OK) return status;
    bytes_per_pixel =
        (size_t)channels * (bit_depth == 16U ? 2U : 1U);
    for (row = 0U; row < height; ++row) {
        unsigned int filter;
        unsigned char *swap;

        status = read_row(
            row_user_data, row, current, row_bytes);
        if (status != AVIFDEC_OK) return status;
        if (bit_depth == 16U) {
            size_t offset;

            for (offset = 0U; offset < row_bytes; offset += 2U) {
                uint16_t sample;

                avifdec_memory_copy(
                    &sample, current + offset, sizeof(sample));
                current[offset] = (unsigned char)(sample >> 8U);
                current[offset + 1U] = (unsigned char)sample;
            }
        }
        filter = png_choose_filter(
            current, previous_row, row_bytes, bytes_per_pixel);
        png_apply_filter(
            filter, current, previous_row, filtered,
            row_bytes, bytes_per_pixel);
        status = png_deflate_feed_lz77(
            &deflater, filtered, row_bytes + 1U);
        if (status != AVIFDEC_OK) return status;
        swap = previous_row;
        previous_row = current;
        current = swap;
    }
    status = png_deflate_finish(&deflater);
    if (status != AVIFDEC_OK) return status;
    return png_write_end(&writer);
}

AvifdecStatus avifdec_png_write(
    AvifdecPngWrite write_callback,
    void *user_data,
    const void *pixels,
    size_t stride,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    uint8_t bit_depth,
    const AvifdecPngMetadata *metadata) {
    unsigned char idat_buffer[4096];
    PngWriter writer;
    PngIdatSink sink;
    PngDeflater deflater;
    size_t row_bytes;
    size_t raw_size;
    uint32_t row;
    AvifdecStatus status;

    if (write_callback == 0 || pixels == 0 || height == 0U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = png_row_bytes(
        width, channels, bit_depth, &row_bytes);
    if (status != AVIFDEC_OK) return status;
    if (stride < row_bytes ||
        !avifdec_size_add(row_bytes, 1U, &raw_size) ||
        !avifdec_size_multiply(raw_size, height, &raw_size) ||
        raw_size > UINT32_MAX) {
        return AVIFDEC_OVERFLOW;
    }

    writer.write_callback = write_callback;
    writer.user_data = user_data;
    writer.chunk_crc = 0U;
    png_crc_initialize(&writer);
    sink.writer = &writer;
    sink.data = idat_buffer;
    sink.capacity = sizeof(idat_buffer);
    sink.size = 0U;
    deflater.sink = &sink;
    deflater.head = 0;
    deflater.previous = 0;
    deflater.history = 0;
    status = png_write_header(
        &writer, width, height, channels, bit_depth, metadata);
    if (status != AVIFDEC_OK) return status;
    status = png_deflate_begin(&deflater);
    if (status != AVIFDEC_OK) return status;
    for (row = 0U; row < height; ++row) {
        const unsigned char *source =
            (const unsigned char *)pixels + (size_t)row * stride;
        const unsigned char filter = 0U;

        status = png_deflate_feed_literals(
            &deflater, &filter, 1U);
        if (status != AVIFDEC_OK) return status;
        if (bit_depth == 8U) {
            status = png_deflate_feed_literals(
                &deflater, source, row_bytes);
        } else {
            size_t offset = 0U;

            while (offset < row_bytes) {
                unsigned char encoded[4096];
                size_t count = row_bytes - offset;
                size_t index;

                if (count > sizeof(encoded)) count = sizeof(encoded);
                for (index = 0U; index < count; ++index) {
                    size_t byte_offset = offset + index;
                    uint16_t sample;

                    avifdec_memory_copy(
                        &sample,
                        source + (byte_offset & ~(size_t)1U),
                        sizeof(sample));
                    encoded[index] =
                        (byte_offset & 1U) == 0U
                            ? (unsigned char)(sample >> 8U)
                            : (unsigned char)sample;
                }
                status = png_deflate_feed_literals(
                    &deflater, encoded, count);
                if (status != AVIFDEC_OK) return status;
                offset += count;
            }
        }
        if (status != AVIFDEC_OK) return status;
    }
    status = png_deflate_finish(&deflater);
    if (status != AVIFDEC_OK) return status;
    return png_write_end(&writer);
}
