#include "encoder/cli/image_input_png.h"

#include "encoder/cli/image_input_internal.h"

#include "base.h"

#define PNG_SIGNATURE_SIZE IMAGE_INPUT_PNG_SIGNATURE_SIZE
#define PNG_CHUNK_IHDR 0x49484452U
#define PNG_CHUNK_PLTE 0x504c5445U
#define PNG_CHUNK_IDAT 0x49444154U
#define PNG_CHUNK_IEND 0x49454e44U
#define PNG_CHUNK_TRNS 0x74524e53U

#define INFLATE_MAX_BITS 15U
#define INFLATE_MAX_TABLE_SIZE (1U << INFLATE_MAX_BITS)
#define INFLATE_CODE_TABLE_SIZE (1U << 7U)
#define INFLATE_LITERAL_SYMBOLS 288U
#define INFLATE_DISTANCE_SYMBOLS 32U
#define INFLATE_CODE_SYMBOLS 19U

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t row_bytes;
    size_t inflated_size;
    size_t idat_size;
    uint16_t palette_entries;
    uint8_t color_type;
    uint8_t channels;
} PngDescription;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_offset;
    uint64_t bit_buffer;
    unsigned int bit_count;
} InflateBitReader;

typedef struct {
    unsigned int table_size;
    uint16_t *symbols;
    uint8_t *lengths;
} InflateHuffman;

typedef struct {
    uint16_t *code_symbols;
    uint8_t *code_table_lengths;
    uint16_t *literal_symbols;
    uint8_t *literal_table_lengths;
    uint16_t *distance_symbols;
    uint8_t *distance_table_lengths;
} InflateTables;

static uint32_t image_crc32_update(uint32_t crc,
                                   const uint8_t *data,
                                   size_t size) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned int bit;

        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

static uint32_t image_adler32(const uint8_t *data, size_t size) {
    uint32_t first = 1U;
    uint32_t second = 0U;

    while (size != 0U) {
        size_t chunk = size > 5552U ? 5552U : size;
        size_t index;

        for (index = 0U; index < chunk; ++index) {
            first += data[index];
            second += first;
        }
        first %= 65521U;
        second %= 65521U;
        data += chunk;
        size -= chunk;
    }
    return (second << 16U) | first;
}

static int png_chunk_type_valid(const uint8_t *type) {
    unsigned int index;

    for (index = 0U; index < 4U; ++index) {
        if (!((type[index] >= 'A' && type[index] <= 'Z') ||
              (type[index] >= 'a' && type[index] <= 'z'))) {
            return 0;
        }
    }
    return (type[2] & 0x20U) == 0U;
}

static ImageInputStatus png_parse(const uint8_t *input,
                                  size_t input_size,
                                  PngDescription *description,
                                  uint8_t *palette,
                                  uint8_t *compressed) {
    static const uint8_t signature[PNG_SIGNATURE_SIZE] = {
        137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U
    };
    size_t position = PNG_SIGNATURE_SIZE;
    size_t compressed_offset = 0U;
    int seen_header = 0;
    int seen_palette = 0;
    int seen_transparency = 0;
    int seen_data = 0;
    int data_ended = 0;
    int seen_end = 0;

    avifdec_memory_fill(description, 0U, sizeof(*description));
    if (input_size < PNG_SIGNATURE_SIZE) return IMAGE_INPUT_TRUNCATED;
    if (avifdec_memory_compare(input, signature, sizeof(signature)) != 0) {
        return IMAGE_INPUT_INVALID_DATA;
    }
    while (position < input_size) {
        const uint8_t *type_bytes;
        const uint8_t *chunk_data;
        uint32_t chunk_size;
        uint32_t chunk_type;
        uint32_t expected_crc;
        uint32_t actual_crc;
        size_t chunk_total;
        size_t chunk_end;

        if (input_size - position < 12U) return IMAGE_INPUT_TRUNCATED;
        chunk_size = avifdec_load_u32be(input + position);
        if (chunk_size > 0x7fffffffU) return IMAGE_INPUT_INVALID_DATA;
        if (!avifdec_size_add((size_t)chunk_size, 12U, &chunk_total)) {
            return IMAGE_INPUT_OVERFLOW;
        }
        if (chunk_total > input_size - position) {
            return IMAGE_INPUT_TRUNCATED;
        }
        chunk_end = position + chunk_total;
        type_bytes = input + position + 4U;
        if (!png_chunk_type_valid(type_bytes)) {
            return IMAGE_INPUT_INVALID_DATA;
        }
        chunk_type = avifdec_load_u32be(type_bytes);
        chunk_data = type_bytes + 4U;
        expected_crc = avifdec_load_u32be(chunk_data + chunk_size);
        actual_crc = image_crc32_update(0xffffffffU, type_bytes, 4U);
        actual_crc = image_crc32_update(actual_crc, chunk_data, chunk_size) ^
                     0xffffffffU;
        if (actual_crc != expected_crc) return IMAGE_INPUT_INVALID_DATA;

        if (chunk_type == PNG_CHUNK_IHDR) {
            size_t pixels;

            if (seen_header || position != PNG_SIGNATURE_SIZE ||
                chunk_size != 13U) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            description->width = avifdec_load_u32be(chunk_data);
            description->height = avifdec_load_u32be(chunk_data + 4U);
            if (description->width == 0U || description->height == 0U ||
                description->width > 0x7fffffffU ||
                description->height > 0x7fffffffU) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            if (chunk_data[8] != 8U || chunk_data[10] != 0U ||
                chunk_data[11] != 0U || chunk_data[12] != 0U) {
                return IMAGE_INPUT_UNSUPPORTED;
            }
            description->color_type = chunk_data[9];
            switch (description->color_type) {
                case 0U: description->channels = 1U; break;
                case 2U: description->channels = 3U; break;
                case 3U: description->channels = 1U; break;
                case 4U: description->channels = 2U; break;
                case 6U: description->channels = 4U; break;
                default: return IMAGE_INPUT_UNSUPPORTED;
            }
            if (!avifdec_size_multiply(description->width,
                                       description->channels,
                                       &description->row_bytes) ||
                !avifdec_size_add(description->row_bytes, 1U, &pixels) ||
                !avifdec_size_multiply(pixels, description->height,
                                       &description->inflated_size)) {
                return IMAGE_INPUT_OVERFLOW;
            }
            seen_header = 1;
        } else if (chunk_type == PNG_CHUNK_PLTE) {
            size_t entries;

            if (!seen_header || seen_palette || seen_data || chunk_size == 0U ||
                chunk_size % 3U != 0U || chunk_size > 768U ||
                description->color_type == 0U ||
                description->color_type == 4U) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            entries = chunk_size / 3U;
            description->palette_entries = (uint16_t)entries;
            if (palette != 0) {
                avifdec_memory_copy(palette, chunk_data, chunk_size);
            }
            seen_palette = 1;
        } else if (chunk_type == PNG_CHUNK_TRNS) {
            if (!seen_header || seen_data || seen_transparency ||
                (description->color_type == 3U &&
                 (!seen_palette || chunk_size >
                  description->palette_entries)) ||
                (description->color_type == 0U && chunk_size != 2U) ||
                (description->color_type == 2U && chunk_size != 6U) ||
                description->color_type == 4U ||
                description->color_type == 6U) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            seen_transparency = 1;
        } else if (chunk_type == PNG_CHUNK_IDAT) {
            size_t next_size;

            if (!seen_header || data_ended ||
                (description->color_type == 3U && !seen_palette) ||
                !avifdec_size_add(description->idat_size, chunk_size,
                                  &next_size)) {
                return seen_header ? IMAGE_INPUT_OVERFLOW
                                   : IMAGE_INPUT_INVALID_DATA;
            }
            if (compressed != 0 && chunk_size != 0U) {
                avifdec_memory_copy(compressed + compressed_offset,
                                    chunk_data, chunk_size);
                compressed_offset += chunk_size;
            }
            description->idat_size = next_size;
            seen_data = 1;
        } else if (chunk_type == PNG_CHUNK_IEND) {
            if (!seen_header || !seen_data || seen_end || chunk_size != 0U ||
                chunk_end != input_size) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            seen_end = 1;
        } else {
            if (!seen_header || (type_bytes[0] & 0x20U) == 0U) {
                return IMAGE_INPUT_UNSUPPORTED;
            }
        }
        if (seen_data && chunk_type != PNG_CHUNK_IDAT &&
            chunk_type != PNG_CHUNK_IEND) {
            data_ended = 1;
        }
        position = chunk_end;
        if (seen_end) break;
    }
    if (!seen_end || description->idat_size < 6U ||
        (description->color_type == 3U && !seen_palette)) {
        return IMAGE_INPUT_INVALID_DATA;
    }
    return IMAGE_INPUT_OK;
}

static void inflate_reader_init(InflateBitReader *reader,
                                const uint8_t *data,
                                size_t size) {
    reader->data = data;
    reader->size = size;
    reader->byte_offset = 0U;
    reader->bit_buffer = 0U;
    reader->bit_count = 0U;
}

static int inflate_ensure_bits(InflateBitReader *reader,
                               unsigned int count) {
    while (reader->bit_count < count) {
        if (reader->byte_offset >= reader->size) return 0;
        reader->bit_buffer |=
            (uint64_t)reader->data[reader->byte_offset++] <<
            reader->bit_count;
        reader->bit_count += 8U;
    }
    return 1;
}

static int inflate_read_bits(InflateBitReader *reader,
                             unsigned int count,
                             unsigned int *value) {
    uint64_t mask;

    if (count == 0U) {
        *value = 0U;
        return 1;
    }
    if (count > 16U || !inflate_ensure_bits(reader, count)) return 0;
    mask = ((uint64_t)1U << count) - 1U;
    *value = (unsigned int)(reader->bit_buffer & mask);
    reader->bit_buffer >>= count;
    reader->bit_count -= count;
    return 1;
}

static void inflate_prefill(InflateBitReader *reader) {
    while (reader->bit_count < 32U && reader->byte_offset < reader->size) {
        reader->bit_buffer |=
            (uint64_t)reader->data[reader->byte_offset++] <<
            reader->bit_count;
        reader->bit_count += 8U;
    }
}

static void inflate_align_byte(InflateBitReader *reader) {
    unsigned int drop = reader->bit_count & 7U;
    unsigned int unread;

    reader->bit_buffer >>= drop;
    reader->bit_count -= drop;
    unread = reader->bit_count / 8U;
    if ((size_t)unread <= reader->byte_offset) {
        reader->byte_offset -= unread;
    }
    reader->bit_buffer = 0U;
    reader->bit_count = 0U;
}

static unsigned int inflate_reverse_bits(unsigned int value,
                                         unsigned int count) {
    unsigned int result = 0U;
    unsigned int index;

    for (index = 0U; index < count; ++index) {
        result = (result << 1U) | (value & 1U);
        value >>= 1U;
    }
    return result;
}

static int inflate_huffman_build(InflateHuffman *huffman,
                                 const uint8_t *code_lengths,
                                 unsigned int symbol_count,
                                 uint16_t *symbol_table,
                                 uint8_t *length_table,
                                 unsigned int table_capacity) {
    unsigned int length_counts[INFLATE_MAX_BITS + 1U];
    unsigned int next_code[INFLATE_MAX_BITS + 1U];
    unsigned int code = 0U;
    unsigned int max_bits = 0U;
    unsigned int available = 1U;
    unsigned int symbol;
    unsigned int bits;

    avifdec_memory_fill(length_counts, 0U, sizeof(length_counts));
    avifdec_memory_fill(next_code, 0U, sizeof(next_code));
    for (symbol = 0U; symbol < symbol_count; ++symbol) {
        unsigned int length = code_lengths[symbol];

        if (length > INFLATE_MAX_BITS) return 0;
        if (length != 0U) {
            ++length_counts[length];
            if (length > max_bits) max_bits = length;
        }
    }
    if (max_bits == 0U) return 0;
    for (bits = 1U; bits <= INFLATE_MAX_BITS; ++bits) {
        available <<= 1U;
        if (length_counts[bits] > available) return 0;
        available -= length_counts[bits];
        code = (code + length_counts[bits - 1U]) << 1U;
        next_code[bits] = code;
    }
    huffman->table_size = 1U << max_bits;
    if (huffman->table_size > table_capacity) return 0;
    huffman->symbols = symbol_table;
    huffman->lengths = length_table;
    avifdec_memory_fill(symbol_table, 0U,
                        sizeof(*symbol_table) * huffman->table_size);
    avifdec_memory_fill(length_table, 0U,
                        sizeof(*length_table) * huffman->table_size);
    for (symbol = 0U; symbol < symbol_count; ++symbol) {
        unsigned int length = code_lengths[symbol];
        unsigned int reversed;
        unsigned int step;
        unsigned int fill;

        if (length == 0U) continue;
        reversed = inflate_reverse_bits(next_code[length]++, length);
        step = 1U << length;
        for (fill = reversed; fill < huffman->table_size; fill += step) {
            symbol_table[fill] = (uint16_t)symbol;
            length_table[fill] = (uint8_t)length;
        }
    }
    return 1;
}

static int inflate_huffman_decode(InflateBitReader *reader,
                                  const InflateHuffman *huffman,
                                  unsigned int *symbol) {
    unsigned int key;
    unsigned int length;

    inflate_prefill(reader);
    if (reader->bit_count == 0U) return 0;
    key = (unsigned int)reader->bit_buffer & (huffman->table_size - 1U);
    length = huffman->lengths[key];
    if (length == 0U || length > reader->bit_count) return 0;
    *symbol = huffman->symbols[key];
    reader->bit_buffer >>= length;
    reader->bit_count -= length;
    return 1;
}

static int inflate_copy_match(uint8_t *output,
                              size_t output_capacity,
                              size_t *output_offset,
                              unsigned int distance,
                              unsigned int length) {
    unsigned int index;

    if (distance == 0U || distance > *output_offset ||
        length > output_capacity - *output_offset) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        output[*output_offset] = output[*output_offset - distance];
        ++*output_offset;
    }
    return 1;
}

static int inflate_codes(InflateBitReader *reader,
                         const InflateHuffman *literal,
                         const InflateHuffman *distance_tree,
                         uint8_t *output,
                         size_t output_capacity,
                         size_t *output_offset) {
    static const uint16_t length_base[29] = {
        3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U, 15U, 17U,
        19U, 23U, 27U, 31U, 35U, 43U, 51U, 59U, 67U, 83U, 99U,
        115U, 131U, 163U, 195U, 227U, 258U
    };
    static const uint8_t length_extra[29] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 2U,
        2U, 2U, 2U, 3U, 3U, 3U, 3U, 4U, 4U, 4U, 4U, 5U, 5U,
        5U, 5U, 0U
    };
    static const uint16_t distance_base[30] = {
        1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U, 17U, 25U, 33U, 49U,
        65U, 97U, 129U, 193U, 257U, 385U, 513U, 769U, 1025U,
        1537U, 2049U, 3073U, 4097U, 6145U, 8193U, 12289U, 16385U,
        24577U
    };
    static const uint8_t distance_extra[30] = {
        0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U, 4U, 4U, 5U,
        5U, 6U, 6U, 7U, 7U, 8U, 8U, 9U, 9U, 10U, 10U, 11U, 11U,
        12U, 12U, 13U, 13U
    };

    for (;;) {
        unsigned int symbol;

        if (!inflate_huffman_decode(reader, literal, &symbol)) return 0;
        if (symbol < 256U) {
            if (*output_offset >= output_capacity) return 0;
            output[(*output_offset)++] = (uint8_t)symbol;
        } else if (symbol == 256U) {
            return 1;
        } else if (symbol <= 285U) {
            unsigned int index = symbol - 257U;
            unsigned int length = length_base[index];
            unsigned int distance_symbol;
            unsigned int distance;
            unsigned int extra;

            if (length_extra[index] != 0U) {
                if (!inflate_read_bits(reader, length_extra[index], &extra)) {
                    return 0;
                }
                length += extra;
            }
            if (!inflate_huffman_decode(reader, distance_tree,
                                        &distance_symbol) ||
                distance_symbol >= 30U) {
                return 0;
            }
            distance = distance_base[distance_symbol];
            if (distance_extra[distance_symbol] != 0U) {
                if (!inflate_read_bits(reader,
                                       distance_extra[distance_symbol],
                                       &extra)) {
                    return 0;
                }
                distance += extra;
            }
            if (!inflate_copy_match(output, output_capacity, output_offset,
                                    distance, length)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
}

static int inflate_stored(InflateBitReader *reader,
                          uint8_t *output,
                          size_t output_capacity,
                          size_t *output_offset) {
    unsigned int length;
    unsigned int inverse_length;

    inflate_align_byte(reader);
    if (reader->size - reader->byte_offset < 4U) return 0;
    length = (unsigned int)reader->data[reader->byte_offset] |
             ((unsigned int)reader->data[reader->byte_offset + 1U] << 8U);
    inverse_length =
        (unsigned int)reader->data[reader->byte_offset + 2U] |
        ((unsigned int)reader->data[reader->byte_offset + 3U] << 8U);
    reader->byte_offset += 4U;
    if (((length ^ 0xffffU) & 0xffffU) != inverse_length ||
        length > reader->size - reader->byte_offset ||
        length > output_capacity - *output_offset) {
        return 0;
    }
    avifdec_memory_copy(output + *output_offset,
                        reader->data + reader->byte_offset, length);
    reader->byte_offset += length;
    *output_offset += length;
    return 1;
}

static int inflate_fixed(InflateBitReader *reader,
                         InflateTables *tables,
                         uint8_t *output,
                         size_t output_capacity,
                         size_t *output_offset) {
    uint8_t literal_lengths[INFLATE_LITERAL_SYMBOLS];
    uint8_t distance_lengths[INFLATE_DISTANCE_SYMBOLS];
    InflateHuffman literal;
    InflateHuffman distance_tree;
    unsigned int index;

    for (index = 0U; index <= 143U; ++index) literal_lengths[index] = 8U;
    for (; index <= 255U; ++index) literal_lengths[index] = 9U;
    for (; index <= 279U; ++index) literal_lengths[index] = 7U;
    for (; index < INFLATE_LITERAL_SYMBOLS; ++index) {
        literal_lengths[index] = 8U;
    }
    for (index = 0U; index < INFLATE_DISTANCE_SYMBOLS; ++index) {
        distance_lengths[index] = 5U;
    }
    if (!inflate_huffman_build(&literal, literal_lengths,
                               INFLATE_LITERAL_SYMBOLS,
                               tables->literal_symbols,
                               tables->literal_table_lengths,
                               INFLATE_MAX_TABLE_SIZE) ||
        !inflate_huffman_build(&distance_tree, distance_lengths,
                               INFLATE_DISTANCE_SYMBOLS,
                               tables->distance_symbols,
                               tables->distance_table_lengths,
                               INFLATE_MAX_TABLE_SIZE)) {
        return 0;
    }
    return inflate_codes(reader, &literal, &distance_tree, output,
                         output_capacity, output_offset);
}

static int inflate_dynamic(InflateBitReader *reader,
                           InflateTables *tables,
                           uint8_t *output,
                           size_t output_capacity,
                           size_t *output_offset) {
    static const uint8_t code_order[INFLATE_CODE_SYMBOLS] = {
        16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U, 11U, 4U,
        12U, 3U, 13U, 2U, 14U, 1U, 15U
    };
    uint8_t code_lengths[INFLATE_CODE_SYMBOLS];
    uint8_t literal_lengths[INFLATE_LITERAL_SYMBOLS];
    uint8_t distance_lengths[INFLATE_DISTANCE_SYMBOLS];
    InflateHuffman code_tree;
    InflateHuffman literal;
    InflateHuffman distance_tree;
    unsigned int literal_count;
    unsigned int distance_count;
    unsigned int code_count;
    unsigned int total;
    unsigned int value;
    unsigned int index;

    avifdec_memory_fill(code_lengths, 0U, sizeof(code_lengths));
    avifdec_memory_fill(literal_lengths, 0U, sizeof(literal_lengths));
    avifdec_memory_fill(distance_lengths, 0U, sizeof(distance_lengths));
    if (!inflate_read_bits(reader, 5U, &value)) return 0;
    literal_count = value + 257U;
    if (!inflate_read_bits(reader, 5U, &value)) return 0;
    distance_count = value + 1U;
    if (!inflate_read_bits(reader, 4U, &value)) return 0;
    code_count = value + 4U;
    if (literal_count > INFLATE_LITERAL_SYMBOLS ||
        distance_count > INFLATE_DISTANCE_SYMBOLS) {
        return 0;
    }
    for (index = 0U; index < code_count; ++index) {
        if (!inflate_read_bits(reader, 3U, &value)) return 0;
        code_lengths[code_order[index]] = (uint8_t)value;
    }
    if (!inflate_huffman_build(&code_tree, code_lengths,
                               INFLATE_CODE_SYMBOLS,
                               tables->code_symbols,
                               tables->code_table_lengths,
                               INFLATE_CODE_TABLE_SIZE)) {
        return 0;
    }
    total = literal_count + distance_count;
    index = 0U;
    while (index < total) {
        unsigned int symbol;
        unsigned int repeat = 1U;
        uint8_t repeated_value;

        if (!inflate_huffman_decode(reader, &code_tree, &symbol)) return 0;
        if (symbol <= 15U) {
            repeated_value = (uint8_t)symbol;
        } else if (symbol == 16U) {
            if (index == 0U || !inflate_read_bits(reader, 2U, &value)) {
                return 0;
            }
            repeat = value + 3U;
            repeated_value = index <= literal_count
                ? literal_lengths[index - 1U]
                : distance_lengths[index - literal_count - 1U];
        } else if (symbol == 17U || symbol == 18U) {
            unsigned int extra_bits = symbol == 17U ? 3U : 7U;
            unsigned int base = symbol == 17U ? 3U : 11U;

            if (!inflate_read_bits(reader, extra_bits, &value)) return 0;
            repeat = value + base;
            repeated_value = 0U;
        } else {
            return 0;
        }
        if (repeat > total - index) return 0;
        while (repeat-- != 0U) {
            if (index < literal_count) {
                literal_lengths[index] = repeated_value;
            } else {
                distance_lengths[index - literal_count] = repeated_value;
            }
            ++index;
        }
    }
    if (literal_lengths[256] == 0U ||
        !inflate_huffman_build(&literal, literal_lengths, literal_count,
                               tables->literal_symbols,
                               tables->literal_table_lengths,
                               INFLATE_MAX_TABLE_SIZE) ||
        !inflate_huffman_build(&distance_tree, distance_lengths,
                               distance_count, tables->distance_symbols,
                               tables->distance_table_lengths,
                               INFLATE_MAX_TABLE_SIZE)) {
        return 0;
    }
    return inflate_codes(reader, &literal, &distance_tree, output,
                         output_capacity, output_offset);
}

static int inflate_zlib(const uint8_t *input,
                        size_t input_size,
                        InflateTables *tables,
                        uint8_t *output,
                        size_t output_capacity) {
    InflateBitReader reader;
    size_t output_offset = 0U;
    size_t compressed_end;
    int final_block = 0;

    if (input_size < 6U || (input[0] & 0x0fU) != 8U ||
        (input[0] >> 4U) > 7U ||
        ((((unsigned int)input[0] << 8U) | input[1]) % 31U) != 0U ||
        (input[1] & 0x20U) != 0U) {
        return 0;
    }
    inflate_reader_init(&reader, input + 2U, input_size - 2U);
    while (!final_block) {
        unsigned int value;
        unsigned int block_type;

        if (!inflate_read_bits(&reader, 1U, &value)) return 0;
        final_block = value != 0U;
        if (!inflate_read_bits(&reader, 2U, &block_type)) return 0;
        if (block_type == 0U) {
            if (!inflate_stored(&reader, output, output_capacity,
                                &output_offset)) {
                return 0;
            }
        } else if (block_type == 1U) {
            if (!inflate_fixed(&reader, tables, output, output_capacity,
                               &output_offset)) {
                return 0;
            }
        } else if (block_type == 2U) {
            if (!inflate_dynamic(&reader, tables, output, output_capacity,
                                 &output_offset)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    if (reader.bit_count / 8U > reader.byte_offset) return 0;
    compressed_end = 2U + reader.byte_offset - reader.bit_count / 8U;
    if (compressed_end > input_size || input_size - compressed_end != 4U ||
        output_offset != output_capacity ||
        avifdec_load_u32be(input + compressed_end) !=
            image_adler32(output, output_offset)) {
        return 0;
    }
    return 1;
}

static uint8_t png_paeth(uint8_t left, uint8_t above, uint8_t upper_left) {
    int prediction = (int)left + (int)above - (int)upper_left;
    int left_distance = prediction - (int)left;
    int above_distance = prediction - (int)above;
    int corner_distance = prediction - (int)upper_left;

    if (left_distance < 0) left_distance = -left_distance;
    if (above_distance < 0) above_distance = -above_distance;
    if (corner_distance < 0) corner_distance = -corner_distance;
    if (left_distance <= above_distance &&
        left_distance <= corner_distance) {
        return left;
    }
    return above_distance <= corner_distance ? above : upper_left;
}

static int png_unfilter(uint8_t *rows,
                        const PngDescription *description) {
    size_t source_stride = description->row_bytes + 1U;
    uint32_t row;

    for (row = 0U; row < description->height; ++row) {
        uint8_t *current = rows + (size_t)row * source_stride + 1U;
        const uint8_t *previous = row == 0U
            ? 0 : rows + (size_t)(row - 1U) * source_stride + 1U;
        uint8_t filter = current[-1];
        size_t column;

        if (filter > 4U) return 0;
        for (column = 0U; column < description->row_bytes; ++column) {
            uint8_t left = column < description->channels
                ? 0U : current[column - description->channels];
            uint8_t above = previous == 0 ? 0U : previous[column];
            uint8_t upper_left = previous == 0 ||
                column < description->channels
                ? 0U : previous[column - description->channels];
            unsigned int predictor;

            if (filter == 0U) {
                predictor = 0U;
            } else if (filter == 1U) {
                predictor = left;
            } else if (filter == 2U) {
                predictor = above;
            } else if (filter == 3U) {
                predictor = ((unsigned int)left + above) >> 1U;
            } else {
                predictor = png_paeth(left, above, upper_left);
            }
            current[column] = (uint8_t)(current[column] + predictor);
        }
    }
    return 1;
}

static void png_to_rgb(const uint8_t *rows,
                       const PngDescription *description,
                       const uint8_t *palette,
                       uint8_t *output) {
    size_t source_stride = description->row_bytes + 1U;
    uint32_t row;

    for (row = 0U; row < description->height; ++row) {
        const uint8_t *source = rows + (size_t)row * source_stride + 1U;
        uint8_t *destination = output +
            (size_t)row * (size_t)description->width * 3U;
        uint32_t column;

        for (column = 0U; column < description->width; ++column) {
            if (description->color_type == 0U ||
                description->color_type == 4U) {
                uint8_t gray = source[(size_t)column * description->channels];
                destination[0] = gray;
                destination[1] = gray;
                destination[2] = gray;
            } else if (description->color_type == 3U) {
                size_t palette_offset = (size_t)source[column] * 3U;
                destination[0] = palette[palette_offset];
                destination[1] = palette[palette_offset + 1U];
                destination[2] = palette[palette_offset + 2U];
            } else {
                size_t source_offset =
                    (size_t)column * description->channels;
                destination[0] = source[source_offset];
                destination[1] = source[source_offset + 1U];
                destination[2] = source[source_offset + 2U];
            }
            destination += 3U;
        }
    }
}

static void png_allocate_workspace(AvifdecArena *arena,
                                   const PngDescription *description,
                                   uint8_t **compressed,
                                   uint8_t **rows,
                                   uint8_t **palette,
                                   InflateTables *tables) {
    *compressed = avifdec_arena_allocate(arena, description->idat_size, 1U);
    *rows = avifdec_arena_allocate(arena, description->inflated_size, 1U);
    *palette = avifdec_arena_allocate(arena, 768U, 1U);
    tables->code_symbols = avifdec_arena_allocate(
        arena, sizeof(*tables->code_symbols) * INFLATE_CODE_TABLE_SIZE, 2U);
    tables->code_table_lengths = avifdec_arena_allocate(
        arena, sizeof(*tables->code_table_lengths) * INFLATE_CODE_TABLE_SIZE,
        1U);
    tables->literal_symbols = avifdec_arena_allocate(
        arena, sizeof(*tables->literal_symbols) * INFLATE_MAX_TABLE_SIZE, 2U);
    tables->literal_table_lengths = avifdec_arena_allocate(
        arena,
        sizeof(*tables->literal_table_lengths) * INFLATE_MAX_TABLE_SIZE, 1U);
    tables->distance_symbols = avifdec_arena_allocate(
        arena, sizeof(*tables->distance_symbols) * INFLATE_MAX_TABLE_SIZE, 2U);
    tables->distance_table_lengths = avifdec_arena_allocate(
        arena,
        sizeof(*tables->distance_table_lengths) * INFLATE_MAX_TABLE_SIZE, 1U);
}

static ImageInputStatus png_info(const uint8_t *input,
                                 size_t input_size,
                                 ImageInputInfo *info,
                                 PngDescription *description) {
    AvifdecArena sizing;
    InflateTables tables;
    uint8_t *compressed;
    uint8_t *rows;
    uint8_t *palette;
    size_t output_size;
    size_t workspace_size;
    ImageInputStatus status = png_parse(input, input_size, description, 0, 0);

    if (status != IMAGE_INPUT_OK) return status;
    if (!avifdec_size_multiply(description->width, 3U,
                               &info->rgb_stride) ||
        !avifdec_size_multiply(info->rgb_stride, description->height,
                               &output_size)) {
        return IMAGE_INPUT_OVERFLOW;
    }
    avifdec_arena_init_sizing(&sizing);
    png_allocate_workspace(&sizing, description, &compressed, &rows,
                           &palette, &tables);
    if (sizing.status != AVIFDEC_OK ||
        !avifdec_size_add(avifdec_arena_required(&sizing),
                          IMAGE_INPUT_WORKSPACE_ALIGNMENT - 1U,
                          &workspace_size)) {
        return IMAGE_INPUT_OVERFLOW;
    }
    info->format = IMAGE_INPUT_FORMAT_PNG;
    info->width = description->width;
    info->height = description->height;
    info->output_size = output_size;
    info->workspace_size = workspace_size;
    return IMAGE_INPUT_OK;
}

ImageInputStatus image_input_png_query(const uint8_t *input,
                                       size_t input_size,
                                       ImageInputInfo *info) {
    PngDescription description;

    return png_info(input, input_size, info, &description);
}

ImageInputStatus image_input_png_decode(const uint8_t *input,
                                        size_t input_size,
                                        void *workspace,
                                        size_t workspace_size,
                                        uint8_t *output,
                                        size_t output_capacity,
                                        ImageInputInfo *info) {
    PngDescription description;
    ImageInputInfo queried;
    AvifdecArena arena;
    InflateTables tables;
    uint8_t *compressed;
    uint8_t *rows;
    uint8_t *palette;
    ImageInputStatus status = png_info(input, input_size, &queried,
                                       &description);

    if (status != IMAGE_INPUT_OK) return status;
    if (workspace_size < queried.workspace_size) {
        return IMAGE_INPUT_WORKSPACE_TOO_SMALL;
    }
    if (output_capacity < queried.output_size) {
        return IMAGE_INPUT_OUTPUT_TOO_SMALL;
    }
    avifdec_arena_init(&arena, workspace, workspace_size);
    png_allocate_workspace(&arena, &description, &compressed, &rows,
                           &palette, &tables);
    if (arena.status != AVIFDEC_OK || compressed == 0 || rows == 0 ||
        palette == 0 || tables.code_symbols == 0 ||
        tables.code_table_lengths == 0 || tables.literal_symbols == 0 ||
        tables.literal_table_lengths == 0 || tables.distance_symbols == 0 ||
        tables.distance_table_lengths == 0) {
        return IMAGE_INPUT_WORKSPACE_TOO_SMALL;
    }
    status = png_parse(input, input_size, &description, palette, compressed);
    if (status != IMAGE_INPUT_OK) return status;
    if (!inflate_zlib(compressed, description.idat_size, &tables, rows,
                      description.inflated_size) ||
        !png_unfilter(rows, &description)) {
        return IMAGE_INPUT_INVALID_DATA;
    }
    if (description.color_type == 3U) {
        size_t row;

        for (row = 0U; row < description.height; ++row) {
            const uint8_t *source = rows +
                row * (description.row_bytes + 1U) + 1U;
            size_t column;

            for (column = 0U; column < description.width; ++column) {
                if (source[column] >= description.palette_entries) {
                    return IMAGE_INPUT_INVALID_DATA;
                }
            }
        }
    }
    png_to_rgb(rows, &description, palette, output);
    *info = queried;
    return IMAGE_INPUT_OK;
}
