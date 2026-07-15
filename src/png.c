#include "png.h"
#include "base.h"

#define PNG_DEFLATE_BLOCK_MAX 65535U
#define PNG_ADLER_MODULUS 65521U
#define PNG_ADLER_BATCH 5552U

typedef struct {
    AvifdecPngWrite write_callback;
    void *user_data;
    uint32_t crc_table[256];
    uint32_t chunk_crc;
} PngWriter;

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

    if (status == AVIFDEC_OK) {
        status = png_chunk_data(writer, data, size);
    }
    if (status == AVIFDEC_OK) {
        status = png_chunk_end(writer);
    }
    return status;
}

static void png_adler_update(
    uint32_t *adler_a,
    uint32_t *adler_b,
    const unsigned char *data,
    size_t size) {
    while (size != 0U) {
        size_t count = size > PNG_ADLER_BATCH ? PNG_ADLER_BATCH : size;
        size_t index;

        for (index = 0U; index < count; ++index) {
            *adler_a += data[index];
            *adler_b += *adler_a;
        }
        *adler_a %= PNG_ADLER_MODULUS;
        *adler_b %= PNG_ADLER_MODULUS;
        data += count;
        size -= count;
    }
}

static AvifdecStatus png_idat_data(
    PngWriter *writer,
    const unsigned char *data,
    size_t size,
    uint32_t *adler_a,
    uint32_t *adler_b) {
    AvifdecStatus status = png_chunk_data(writer, data, size);

    if (status == AVIFDEC_OK) {
        png_adler_update(adler_a, adler_b, data, size);
    }
    return status;
}

static AvifdecStatus png_idat_pixels(
    PngWriter *writer,
    const unsigned char *row,
    size_t offset,
    size_t size,
    uint8_t bit_depth,
    uint32_t *adler_a,
    uint32_t *adler_b) {
    if (bit_depth == 8U) {
        return png_idat_data(
            writer, row + offset, size, adler_a, adler_b);
    }
    while (size != 0U) {
        unsigned char encoded[4096];
        size_t count = size > sizeof(encoded) ? sizeof(encoded) : size;
        const uint16_t *samples = (const uint16_t *)row;
        size_t index;

        for (index = 0U; index < count; ++index) {
            size_t byte_offset = offset + index;
            uint16_t sample = samples[byte_offset >> 1U];
            encoded[index] = (byte_offset & 1U) == 0U
                ? (unsigned char)(sample >> 8U)
                : (unsigned char)sample;
        }
        {
            AvifdecStatus status = png_idat_data(
                writer, encoded, count, adler_a, adler_b);
            if (status != AVIFDEC_OK) return status;
        }
        offset += count;
        size -= count;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus png_write_idat(
    PngWriter *writer,
    const unsigned char *pixels,
    size_t stride,
    size_t row_bytes,
    uint32_t height,
    uint8_t bit_depth,
    uint32_t compressed_size) {
    static const unsigned char type[4] = { 'I', 'D', 'A', 'T' };
    const unsigned char zlib_header[2] = { 0x78U, 0x01U };
    uint32_t adler_a = 1U;
    uint32_t adler_b = 0U;
    uint32_t row;
    AvifdecStatus status;

    status = png_chunk_begin(writer, compressed_size, type);
    if (status != AVIFDEC_OK) return status;
    status = png_chunk_data(writer, zlib_header, sizeof(zlib_header));
    if (status != AVIFDEC_OK) return status;
    for (row = 0U; row < height; ++row) {
        const unsigned char *source = pixels + (size_t)row * stride;
        size_t row_payload = row_bytes + 1U;
        size_t row_offset = 0U;

        while (row_offset < row_payload) {
            size_t remaining = row_payload - row_offset;
            uint16_t block_size = (uint16_t)(
                remaining > PNG_DEFLATE_BLOCK_MAX
                    ? PNG_DEFLATE_BLOCK_MAX : remaining);
            uint16_t complement = (uint16_t)~block_size;
            int final_block =
                row + 1U == height &&
                row_offset + block_size == row_payload;
            unsigned char block_header[5];

            block_header[0] = (unsigned char)(final_block ? 1U : 0U);
            block_header[1] = (unsigned char)block_size;
            block_header[2] = (unsigned char)(block_size >> 8U);
            block_header[3] = (unsigned char)complement;
            block_header[4] = (unsigned char)(complement >> 8U);
            status = png_chunk_data(
                writer, block_header, sizeof(block_header));
            if (status != AVIFDEC_OK) return status;
            if (row_offset == 0U) {
                const unsigned char filter = 0U;

                status = png_idat_data(
                    writer, &filter, 1U, &adler_a, &adler_b);
                if (status != AVIFDEC_OK) return status;
                if (block_size > 1U) {
                    status = png_idat_pixels(
                        writer, source, 0U, (size_t)block_size - 1U,
                        bit_depth, &adler_a, &adler_b);
                    if (status != AVIFDEC_OK) return status;
                }
            } else {
                status = png_idat_pixels(
                    writer, source, row_offset - 1U, block_size,
                    bit_depth, &adler_a, &adler_b);
                if (status != AVIFDEC_OK) return status;
            }
            row_offset += block_size;
        }
    }
    {
        unsigned char encoded_adler[4];

        png_store_u32(encoded_adler, (adler_b << 16U) | adler_a);
        status = png_chunk_data(
            writer, encoded_adler, sizeof(encoded_adler));
    }
    if (status == AVIFDEC_OK) {
        status = png_chunk_end(writer);
    }
    return status;
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
    static const unsigned char signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU
    };
    static const unsigned char ihdr_type[4] = { 'I', 'H', 'D', 'R' };
    static const unsigned char phys_type[4] = { 'p', 'H', 'Y', 's' };
    static const unsigned char cicp_type[4] = { 'c', 'I', 'C', 'P' };
    static const unsigned char iend_type[4] = { 'I', 'E', 'N', 'D' };
    PngWriter writer;
    unsigned char ihdr[13];
    size_t bytes_per_channel;
    size_t row_bytes;
    size_t row_payload;
    size_t raw_size;
    size_t blocks_per_row;
    size_t block_count;
    size_t block_overhead;
    size_t compressed_size;
    AvifdecStatus status;

    if (write_callback == 0 || pixels == 0 || width == 0U || height == 0U ||
        (channels != 3U && channels != 4U) ||
        (bit_depth != 8U && bit_depth != 16U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    bytes_per_channel = bit_depth == 16U ? 2U : 1U;
    if (!avifdec_size_multiply(width, channels, &row_bytes) ||
        !avifdec_size_multiply(
            row_bytes, bytes_per_channel, &row_bytes) ||
        !avifdec_size_add(row_bytes, 1U, &row_payload) ||
        stride < row_bytes ||
        !avifdec_size_multiply(row_payload, height, &raw_size) ||
        !avifdec_size_add(
            row_payload, PNG_DEFLATE_BLOCK_MAX - 1U, &blocks_per_row)) {
        return AVIFDEC_OVERFLOW;
    }
    blocks_per_row /= PNG_DEFLATE_BLOCK_MAX;
    if (!avifdec_size_multiply(blocks_per_row, height, &block_count) ||
        !avifdec_size_multiply(block_count, 5U, &block_overhead) ||
        !avifdec_size_add(raw_size, block_overhead, &compressed_size) ||
        !avifdec_size_add(compressed_size, 6U, &compressed_size) ||
        compressed_size > 0x7fffffffU) {
        return AVIFDEC_OVERFLOW;
    }
    writer.write_callback = write_callback;
    writer.user_data = user_data;
    writer.chunk_crc = 0U;
    png_crc_initialize(&writer);
    status = png_write_bytes(&writer, signature, sizeof(signature));
    if (status != AVIFDEC_OK) return status;

    png_store_u32(ihdr, width);
    png_store_u32(ihdr + 4U, height);
    ihdr[8] = bit_depth;
    ihdr[9] = (unsigned char)(channels == 4U ? 6U : 2U);
    ihdr[10] = 0U;
    ihdr[11] = 0U;
    ihdr[12] = 0U;
    status = png_write_chunk(
        &writer, ihdr_type, ihdr, sizeof(ihdr));
    if (status != AVIFDEC_OK) return status;

    if (metadata != 0 &&
        metadata->pixel_aspect_h_spacing != 0U &&
        metadata->pixel_aspect_v_spacing != 0U) {
        unsigned char phys[9];

        png_store_u32(phys, metadata->pixel_aspect_v_spacing);
        png_store_u32(phys + 4U, metadata->pixel_aspect_h_spacing);
        phys[8] = 0U;
        status = png_write_chunk(
            &writer, phys_type, phys, sizeof(phys));
        if (status != AVIFDEC_OK) return status;
    }
    if (metadata != 0 && metadata->has_nclx != 0U &&
        metadata->color_primaries <= 255U &&
        metadata->transfer_characteristics <= 255U) {
        unsigned char cicp[4];

        cicp[0] = (unsigned char)metadata->color_primaries;
        cicp[1] = (unsigned char)metadata->transfer_characteristics;
        cicp[2] = 0U;
        cicp[3] = 1U;
        status = png_write_chunk(
            &writer, cicp_type, cicp, sizeof(cicp));
        if (status != AVIFDEC_OK) return status;
    }
    status = png_write_idat(
        &writer, (const unsigned char *)pixels, stride, row_bytes,
        height, bit_depth, (uint32_t)compressed_size);
    if (status != AVIFDEC_OK) return status;
    return png_write_chunk(&writer, iend_type, 0, 0U);
}
