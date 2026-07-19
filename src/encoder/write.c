#include "encoder/write.h"
#include "base.h"

static AvifencStatus byte_writer_fail(AvifencByteWriter *writer,
                                      AvifencStatus status) {
    if (writer != 0 && writer->status == AVIFENC_OK) writer->status = status;
    return writer == 0 ? AVIFENC_INVALID_ARGUMENT : writer->status;
}

void avifenc_byte_writer_init(AvifencByteWriter *writer,
                              void *data,
                              size_t capacity) {
    if (writer == 0) return;
    writer->data = (uint8_t *)data;
    writer->capacity = capacity;
    writer->position = 0U;
    writer->status = data == 0 && capacity != 0U
        ? AVIFENC_INVALID_ARGUMENT : AVIFENC_OK;
    writer->sizing_only = 0;
}

void avifenc_byte_writer_init_sizing(AvifencByteWriter *writer) {
    if (writer == 0) return;
    writer->data = 0;
    writer->capacity = 0U;
    writer->position = 0U;
    writer->status = AVIFENC_OK;
    writer->sizing_only = 1;
}

size_t avifenc_byte_writer_size(const AvifencByteWriter *writer) {
    return writer == 0 ? 0U : writer->position;
}

static AvifencStatus byte_writer_prepare(AvifencByteWriter *writer,
                                         size_t size,
                                         size_t *offset) {
    size_t end;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (offset == 0) {
        return byte_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    if (!avifdec_size_add(writer->position, size, &end)) {
        return byte_writer_fail(writer, AVIFENC_OVERFLOW);
    }
    if (!writer->sizing_only && end > writer->capacity) {
        return byte_writer_fail(writer, AVIFENC_OUTPUT_TOO_SMALL);
    }
    *offset = writer->position;
    writer->position = end;
    return AVIFENC_OK;
}

AvifencStatus avifenc_byte_writer_write(AvifencByteWriter *writer,
                                        const void *data,
                                        size_t size) {
    size_t offset;
    AvifencStatus status;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (data == 0 && size != 0U) {
        return byte_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    status = byte_writer_prepare(writer, size, &offset);
    if (status == AVIFENC_OK && !writer->sizing_only && size != 0U) {
        avifdec_memory_copy(writer->data + offset, data, size);
    }
    return status;
}

AvifencStatus avifenc_byte_writer_u8(AvifencByteWriter *writer,
                                     uint8_t value) {
    return avifenc_byte_writer_write(writer, &value, 1U);
}

AvifencStatus avifenc_byte_writer_u16be(AvifencByteWriter *writer,
                                        uint16_t value) {
    uint8_t encoded[2];

    encoded[0] = (uint8_t)(value >> 8U);
    encoded[1] = (uint8_t)value;
    return avifenc_byte_writer_write(writer, encoded, sizeof(encoded));
}

AvifencStatus avifenc_byte_writer_u32be(AvifencByteWriter *writer,
                                        uint32_t value) {
    uint8_t encoded[4];

    encoded[0] = (uint8_t)(value >> 24U);
    encoded[1] = (uint8_t)(value >> 16U);
    encoded[2] = (uint8_t)(value >> 8U);
    encoded[3] = (uint8_t)value;
    return avifenc_byte_writer_write(writer, encoded, sizeof(encoded));
}

AvifencStatus avifenc_byte_writer_u64be(AvifencByteWriter *writer,
                                        uint64_t value) {
    uint8_t encoded[8];
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        encoded[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
    return avifenc_byte_writer_write(writer, encoded, sizeof(encoded));
}

AvifencStatus avifenc_byte_writer_leb128(AvifencByteWriter *writer,
                                         size_t value) {
    uint8_t encoded[8];
    size_t remaining = value;
    size_t count = 0U;

    do {
        uint8_t byte;

        if (count == sizeof(encoded)) {
            return byte_writer_fail(writer, AVIFENC_LIMIT_EXCEEDED);
        }
        byte = (uint8_t)(remaining & 0x7fU);
        remaining >>= 7U;
        if (remaining != 0U) byte |= 0x80U;
        encoded[count++] = byte;
    } while (remaining != 0U);
    return avifenc_byte_writer_write(writer, encoded, count);
}

AvifencStatus avifenc_byte_writer_reserve(AvifencByteWriter *writer,
                                          size_t size,
                                          size_t *offset) {
    AvifencStatus status = byte_writer_prepare(writer, size, offset);

    if (status == AVIFENC_OK && !writer->sizing_only && size != 0U) {
        avifdec_memory_fill(writer->data + *offset, 0U, size);
    }
    return status;
}

static AvifencStatus byte_writer_patch(AvifencByteWriter *writer,
                                       size_t offset,
                                       const uint8_t *data,
                                       size_t size) {
    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if ((data == 0 && size != 0U) || offset > writer->position ||
        size > writer->position - offset) {
        return byte_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    if (!writer->sizing_only && size != 0U) {
        avifdec_memory_copy(writer->data + offset, data, size);
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_byte_writer_patch_u8(AvifencByteWriter *writer,
                                           size_t offset,
                                           uint8_t value) {
    return byte_writer_patch(writer, offset, &value, 1U);
}

AvifencStatus avifenc_byte_writer_patch_u16be(AvifencByteWriter *writer,
                                              size_t offset,
                                              uint16_t value) {
    uint8_t encoded[2];

    encoded[0] = (uint8_t)(value >> 8U);
    encoded[1] = (uint8_t)value;
    return byte_writer_patch(writer, offset, encoded, sizeof(encoded));
}

AvifencStatus avifenc_byte_writer_patch_u32be(AvifencByteWriter *writer,
                                              size_t offset,
                                              uint32_t value) {
    uint8_t encoded[4];

    encoded[0] = (uint8_t)(value >> 24U);
    encoded[1] = (uint8_t)(value >> 16U);
    encoded[2] = (uint8_t)(value >> 8U);
    encoded[3] = (uint8_t)value;
    return byte_writer_patch(writer, offset, encoded, sizeof(encoded));
}

AvifencStatus avifenc_byte_writer_patch_u64be(AvifencByteWriter *writer,
                                              size_t offset,
                                              uint64_t value) {
    uint8_t encoded[8];
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        encoded[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
    return byte_writer_patch(writer, offset, encoded, sizeof(encoded));
}

static AvifencStatus bit_writer_fail(AvifencBitWriter *writer,
                                     AvifencStatus status) {
    if (writer != 0 && writer->status == AVIFENC_OK) writer->status = status;
    return writer == 0 ? AVIFENC_INVALID_ARGUMENT : writer->status;
}

void avifenc_bit_writer_init(AvifencBitWriter *writer,
                             void *data,
                             size_t capacity) {
    if (writer == 0) return;
    writer->data = (uint8_t *)data;
    writer->capacity = capacity;
    writer->bit_position = 0U;
    writer->status = data == 0 && capacity != 0U
        ? AVIFENC_INVALID_ARGUMENT : AVIFENC_OK;
    writer->sizing_only = 0;
}

void avifenc_bit_writer_init_sizing(AvifencBitWriter *writer) {
    if (writer == 0) return;
    writer->data = 0;
    writer->capacity = 0U;
    writer->bit_position = 0U;
    writer->status = AVIFENC_OK;
    writer->sizing_only = 1;
}

size_t avifenc_bit_writer_bits(const AvifencBitWriter *writer) {
    return writer == 0 ? 0U : writer->bit_position;
}

size_t avifenc_bit_writer_bytes(const AvifencBitWriter *writer) {
    if (writer == 0) return 0U;
    return writer->bit_position / 8U +
        (writer->bit_position % 8U != 0U ? 1U : 0U);
}

static AvifencStatus bit_writer_prepare(AvifencBitWriter *writer,
                                        size_t bit_count) {
    size_t end;
    size_t bytes;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (!avifdec_size_add(writer->bit_position, bit_count, &end)) {
        return bit_writer_fail(writer, AVIFENC_OVERFLOW);
    }
    bytes = end / 8U + (end % 8U != 0U ? 1U : 0U);
    if (!writer->sizing_only && bytes > writer->capacity) {
        return bit_writer_fail(writer, AVIFENC_OUTPUT_TOO_SMALL);
    }
    return AVIFENC_OK;
}

static void bit_writer_write_unchecked(AvifencBitWriter *writer,
                                       uint64_t value,
                                       unsigned int bit_count) {
    unsigned int index;

    if (writer->sizing_only) {
        writer->bit_position += bit_count;
        return;
    }
    for (index = 0U; index < bit_count; ++index) {
        size_t position = writer->bit_position++;
        size_t byte = position / 8U;
        unsigned int shift = 7U - (unsigned int)(position % 8U);

        if ((position & 7U) == 0U) writer->data[byte] = 0U;
        writer->data[byte] |= (uint8_t)(
            ((value >> (bit_count - index - 1U)) & 1U) << shift);
    }
}

AvifencStatus avifenc_bit_writer_write(AvifencBitWriter *writer,
                                       uint64_t value,
                                       unsigned int bit_count) {
    AvifencStatus status;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (bit_count > 64U ||
        (bit_count != 0U && bit_count < 64U &&
         (value >> bit_count) != 0U)) {
        return bit_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    status = bit_writer_prepare(writer, bit_count);
    if (status == AVIFENC_OK) {
        bit_writer_write_unchecked(writer, value, bit_count);
    }
    return status;
}

AvifencStatus avifenc_bit_writer_align(AvifencBitWriter *writer) {
    unsigned int padding;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    padding = (unsigned int)((8U - writer->bit_position % 8U) % 8U);
    return avifenc_bit_writer_write(writer, 0U, padding);
}

AvifencStatus avifenc_bit_writer_uvlc(AvifencBitWriter *writer,
                                      uint32_t value) {
    uint64_t code;
    unsigned int bit_length = 0U;
    unsigned int leading_zeros;
    size_t total_bits;
    AvifencStatus status;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (value == UINT32_MAX) {
        status = bit_writer_prepare(writer, 32U);
        if (status == AVIFENC_OK) bit_writer_write_unchecked(writer, 0U, 32U);
        return status;
    }
    code = (uint64_t)value + 1U;
    while ((code >> bit_length) != 0U) ++bit_length;
    leading_zeros = bit_length - 1U;
    total_bits = (size_t)leading_zeros * 2U + 1U;
    status = bit_writer_prepare(writer, total_bits);
    if (status == AVIFENC_OK) {
        bit_writer_write_unchecked(writer, 0U, leading_zeros);
        bit_writer_write_unchecked(writer, code, bit_length);
    }
    return status;
}

AvifencStatus avifenc_bit_writer_ns(AvifencBitWriter *writer,
                                    uint32_t value,
                                    uint32_t symbol_count) {
    uint64_t power = 1U;
    uint32_t minimum;
    unsigned int width = 0U;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (symbol_count <= 1U) {
        return value == 0U
            ? AVIFENC_OK
            : bit_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    if (value >= symbol_count) {
        return bit_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    while (power < symbol_count) {
        power <<= 1U;
        ++width;
    }
    minimum = (uint32_t)power - symbol_count;
    if (value < minimum) {
        return avifenc_bit_writer_write(writer, value, width - 1U);
    }
    return avifenc_bit_writer_write(
        writer, (uint64_t)value + minimum, width);
}