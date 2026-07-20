#include "encoder/cli/image_input_jpeg.h"

#include "encoder/cli/image_input_internal.h"

#include "base.h"

#define JPEG_MAX_COMPONENTS 3U
#define JPEG_MAX_TABLES 4U
#define JPEG_BLOCK_SIZE 64U
#define JPEG_MAX_BLOCKS_PER_COMPONENT 4U

typedef struct {
    uint8_t counts[16];
    uint8_t symbols[256];
    uint16_t symbol_count;
    uint8_t valid;
} JpegHuffman;

typedef struct {
    uint8_t identifier;
    uint8_t horizontal_sampling;
    uint8_t vertical_sampling;
    uint8_t quantization_table;
    uint8_t dc_table;
    uint8_t ac_table;
    int32_t dc_prediction;
    int16_t *samples;
} JpegComponent;

typedef struct {
    uint16_t quantization[JPEG_MAX_TABLES][JPEG_BLOCK_SIZE];
    JpegHuffman huffman[2][JPEG_MAX_TABLES];
    JpegComponent components[JPEG_MAX_COMPONENTS];
    size_t entropy_offset;
    uint32_t width;
    uint32_t height;
    uint16_t restart_interval;
    uint8_t quantization_valid[JPEG_MAX_TABLES];
    uint8_t scan_order[JPEG_MAX_COMPONENTS];
    uint8_t component_count;
    uint8_t horizontal_maximum;
    uint8_t vertical_maximum;
    uint8_t rgb_components;
} JpegState;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint32_t bit_buffer;
    unsigned int bit_count;
    unsigned int marker;
    ImageInputStatus status;
} JpegBitReader;

ImageInputStatus image_input_jpeg_query(const uint8_t *input,
                                        size_t input_size,
                                        ImageInputInfo *info) {
    AvifdecArena sizing;
    JpegState *state;
    int32_t *coefficients;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint8_t component_count = 0U;
    uint8_t horizontal_maximum = 0U;
    uint8_t vertical_maximum = 0U;
    size_t position = 2U;
    size_t workspace_size;
    uint8_t component_identifiers[JPEG_MAX_COMPONENTS];
    uint8_t component_quantization[JPEG_MAX_COMPONENTS];
    uint8_t quantization_valid = 0U;
    uint8_t dc_huffman_valid = 0U;
    uint8_t ac_huffman_valid = 0U;
    int seen_frame = 0;
    int seen_scan = 0;

    if (input_size < 2U) return IMAGE_INPUT_TRUNCATED;
    while (position < input_size && !seen_scan) {
        unsigned int marker;
        uint16_t segment_length;
        const uint8_t *data;
        size_t data_size;

        if (input[position++] != 0xffU) return IMAGE_INPUT_INVALID_DATA;
        while (position < input_size && input[position] == 0xffU) ++position;
        if (position >= input_size) return IMAGE_INPUT_TRUNCATED;
        marker = input[position++];
        if (marker == 0U || marker == 0xd8U || marker == 0xd9U ||
            (marker >= 0xd0U && marker <= 0xd7U)) {
            return IMAGE_INPUT_INVALID_DATA;
        }
        if (marker == 0x01U) continue;
        if (input_size - position < 2U) return IMAGE_INPUT_TRUNCATED;
        segment_length = avifdec_load_u16be(input + position);
        position += 2U;
        if (segment_length < 2U) return IMAGE_INPUT_INVALID_DATA;
        data_size = segment_length - 2U;
        if (data_size > input_size - position) return IMAGE_INPUT_TRUNCATED;
        data = input + position;
        position += data_size;
        if (marker == 0xc0U) {
            unsigned int components;
            unsigned int index;
            unsigned int blocks = 0U;

            if (seen_frame || data_size < 6U) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            if (data[0] != 8U) return IMAGE_INPUT_UNSUPPORTED;
            height = avifdec_load_u16be(data + 1U);
            width = avifdec_load_u16be(data + 3U);
            components = data[5];
            if (height == 0U || width == 0U ||
                (components != 1U && components != 3U)) {
                return IMAGE_INPUT_UNSUPPORTED;
            }
            if (data_size != 6U + components * 3U) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            horizontal_maximum = 0U;
            vertical_maximum = 0U;
            for (index = 0U; index < components; ++index) {
                const uint8_t *component = data + 6U + index * 3U;
                unsigned int horizontal = component[1] >> 4U;
                unsigned int vertical = component[1] & 0x0fU;
                unsigned int previous;

                if (horizontal == 0U || vertical == 0U || horizontal > 2U ||
                    vertical > 2U || component[2] >= JPEG_MAX_TABLES) {
                    return IMAGE_INPUT_UNSUPPORTED;
                }
                for (previous = 0U; previous < index; ++previous) {
                    if (data[6U + previous * 3U] == component[0]) {
                        return IMAGE_INPUT_INVALID_DATA;
                    }
                }
                blocks += horizontal * vertical;
                if (horizontal > horizontal_maximum) {
                    horizontal_maximum = (uint8_t)horizontal;
                }
                if (vertical > vertical_maximum) {
                    vertical_maximum = (uint8_t)vertical;
                }
                component_identifiers[index] = component[0];
                component_quantization[index] = component[2];
            }
            if (blocks > 10U || (components == 1U && blocks != 1U)) {
                return IMAGE_INPUT_UNSUPPORTED;
            }
            component_count = (uint8_t)components;
            seen_frame = 1;
        } else if ((marker >= 0xc1U && marker <= 0xcfU) &&
                   marker != 0xc4U && marker != 0xc8U) {
            return IMAGE_INPUT_UNSUPPORTED;
        } else if (marker == 0xc8U || marker == 0xdcU ||
                   marker == 0xdeU || marker == 0xdfU) {
            return IMAGE_INPUT_UNSUPPORTED;
        } else if (marker == 0xdbU) {
            size_t table_position = 0U;

            while (table_position < data_size) {
                unsigned int table_info;
                unsigned int index;

                if (data_size - table_position < 65U) {
                    return IMAGE_INPUT_TRUNCATED;
                }
                table_info = data[table_position++];
                if ((table_info >> 4U) != 0U ||
                    (table_info & 0x0fU) >= JPEG_MAX_TABLES) {
                    return (table_info >> 4U) != 0U
                        ? IMAGE_INPUT_UNSUPPORTED : IMAGE_INPUT_INVALID_DATA;
                }
                for (index = 0U; index < JPEG_BLOCK_SIZE; ++index) {
                    if (data[table_position++] == 0U) {
                        return IMAGE_INPUT_INVALID_DATA;
                    }
                }
                quantization_valid |=
                    (uint8_t)(1U << (table_info & 0x0fU));
            }
        } else if (marker == 0xc4U) {
            size_t table_position = 0U;

            while (table_position < data_size) {
                unsigned int table_info;
                unsigned int table_class;
                unsigned int symbol_count = 0U;
                unsigned int available = 1U;
                unsigned int index;

                if (data_size - table_position < 17U) {
                    return IMAGE_INPUT_TRUNCATED;
                }
                table_info = data[table_position++];
                table_class = table_info >> 4U;
                if (table_class > 1U ||
                    (table_info & 0x0fU) >= JPEG_MAX_TABLES) {
                    return IMAGE_INPUT_INVALID_DATA;
                }
                for (index = 0U; index < 16U; ++index) {
                    unsigned int count = data[table_position++];

                    available <<= 1U;
                    if (count > available) return IMAGE_INPUT_INVALID_DATA;
                    available -= count;
                    symbol_count += count;
                }
                if (symbol_count == 0U || symbol_count > 256U ||
                    symbol_count > data_size - table_position) {
                    return IMAGE_INPUT_INVALID_DATA;
                }
                for (index = 0U; index < symbol_count; ++index) {
                    unsigned int symbol = data[table_position + index];

                    if ((table_class == 0U && symbol > 11U) ||
                        (table_class == 1U && (symbol & 0x0fU) > 10U)) {
                        return IMAGE_INPUT_INVALID_DATA;
                    }
                }
                table_position += symbol_count;
                if (table_class == 0U) {
                    dc_huffman_valid |=
                        (uint8_t)(1U << (table_info & 0x0fU));
                } else {
                    ac_huffman_valid |=
                        (uint8_t)(1U << (table_info & 0x0fU));
                }
            }
        } else if (marker == 0xddU && data_size != 2U) {
            return IMAGE_INPUT_INVALID_DATA;
        } else if (marker == 0xeeU && data_size >= 12U &&
                   data[0] == 'A' && data[1] == 'd' && data[2] == 'o' &&
                   data[3] == 'b' && data[4] == 'e' && data[11] > 1U) {
            return IMAGE_INPUT_UNSUPPORTED;
        } else if (marker == 0xdaU) {
            unsigned int scan_components;
            uint8_t components_seen = 0U;
            unsigned int index;

            if (!seen_frame || data_size < 4U) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            scan_components = data[0];
            if (scan_components != component_count ||
                data_size != 1U + scan_components * 2U + 3U ||
                data[1U + scan_components * 2U] != 0U ||
                data[2U + scan_components * 2U] != 63U ||
                data[3U + scan_components * 2U] != 0U) {
                return IMAGE_INPUT_UNSUPPORTED;
            }
            for (index = 0U; index < scan_components; ++index) {
                unsigned int identifier = data[1U + index * 2U];
                unsigned int selectors = data[2U + index * 2U];
                unsigned int dc_table = selectors >> 4U;
                unsigned int ac_table = selectors & 0x0fU;
                unsigned int component_index;

                for (component_index = 0U;
                     component_index < component_count; ++component_index) {
                    if (component_identifiers[component_index] == identifier) {
                        break;
                    }
                }
                if (component_index == component_count ||
                    (components_seen & (1U << component_index)) != 0U ||
                    dc_table >= JPEG_MAX_TABLES ||
                    ac_table >= JPEG_MAX_TABLES ||
                    (quantization_valid &
                     (1U << component_quantization[component_index])) == 0U ||
                    (dc_huffman_valid & (1U << dc_table)) == 0U ||
                    (ac_huffman_valid & (1U << ac_table)) == 0U) {
                    return IMAGE_INPUT_INVALID_DATA;
                }
                components_seen |= (uint8_t)(1U << component_index);
            }
            seen_scan = 1;
        }
    }
    if (!seen_frame || !seen_scan) return IMAGE_INPUT_TRUNCATED;
    avifdec_arena_init_sizing(&sizing);
    state = avifdec_arena_allocate(&sizing, sizeof(*state), 8U);
    coefficients = avifdec_arena_allocate(
        &sizing, sizeof(*coefficients) * JPEG_BLOCK_SIZE, 4U);
    (void)state;
    (void)coefficients;
    {
        unsigned int component;

        for (component = 0U; component < JPEG_MAX_COMPONENTS; ++component) {
            (void)avifdec_arena_allocate(
                &sizing,
                sizeof(int16_t) * JPEG_BLOCK_SIZE *
                    JPEG_MAX_BLOCKS_PER_COMPONENT,
                2U);
        }
    }
    if (sizing.status != AVIFDEC_OK ||
        !avifdec_size_add(avifdec_arena_required(&sizing),
                          IMAGE_INPUT_WORKSPACE_ALIGNMENT - 1U,
                          &workspace_size) ||
        !avifdec_size_multiply(width, 3U, &info->rgb_stride) ||
        !avifdec_size_multiply(info->rgb_stride, height,
                               &info->output_size)) {
        return IMAGE_INPUT_OVERFLOW;
    }
    info->format = IMAGE_INPUT_FORMAT_JPEG;
    info->width = width;
    info->height = height;
    info->workspace_size = workspace_size;
    return IMAGE_INPUT_OK;
}

static int jpeg_find_component(const JpegState *state,
                               unsigned int identifier) {
    unsigned int index;

    for (index = 0U; index < state->component_count; ++index) {
        if (state->components[index].identifier == identifier) {
            return (int)index;
        }
    }
    return -1;
}

static ImageInputStatus jpeg_load_headers(const uint8_t *input,
                                          size_t input_size,
                                          JpegState *state) {
    static const uint8_t zigzag[JPEG_BLOCK_SIZE] = {
        0U, 1U, 8U, 16U, 9U, 2U, 3U, 10U,
        17U, 24U, 32U, 25U, 18U, 11U, 4U, 5U,
        12U, 19U, 26U, 33U, 40U, 48U, 41U, 34U,
        27U, 20U, 13U, 6U, 7U, 14U, 21U, 28U,
        35U, 42U, 49U, 56U, 57U, 50U, 43U, 36U,
        29U, 22U, 15U, 23U, 30U, 37U, 44U, 51U,
        58U, 59U, 52U, 45U, 38U, 31U, 39U, 46U,
        53U, 60U, 61U, 54U, 47U, 55U, 62U, 63U
    };
    size_t position = 2U;
    int seen_frame = 0;
    int adobe_transform = -1;

    while (position < input_size) {
        unsigned int marker;
        uint16_t segment_length;
        const uint8_t *data;
        size_t data_size;

        if (input[position++] != 0xffU) return IMAGE_INPUT_INVALID_DATA;
        while (position < input_size && input[position] == 0xffU) ++position;
        if (position >= input_size) return IMAGE_INPUT_TRUNCATED;
        marker = input[position++];
        if (marker == 0U || marker == 0xd8U || marker == 0xd9U ||
            (marker >= 0xd0U && marker <= 0xd7U)) {
            return IMAGE_INPUT_INVALID_DATA;
        }
        if (marker == 0x01U) continue;
        if (input_size - position < 2U) return IMAGE_INPUT_TRUNCATED;
        segment_length = avifdec_load_u16be(input + position);
        position += 2U;
        if (segment_length < 2U) return IMAGE_INPUT_INVALID_DATA;
        data_size = segment_length - 2U;
        if (data_size > input_size - position) return IMAGE_INPUT_TRUNCATED;
        data = input + position;
        position += data_size;
        if (marker == 0xc0U) {
            unsigned int components = data[5];
            unsigned int index;

            if (seen_frame || data_size != 6U + components * 3U) {
                return IMAGE_INPUT_INVALID_DATA;
            }
            state->height = avifdec_load_u16be(data + 1U);
            state->width = avifdec_load_u16be(data + 3U);
            state->component_count = (uint8_t)components;
            for (index = 0U; index < components; ++index) {
                const uint8_t *source = data + 6U + index * 3U;
                JpegComponent *component = &state->components[index];

                component->identifier = source[0];
                component->horizontal_sampling = source[1] >> 4U;
                component->vertical_sampling = source[1] & 0x0fU;
                component->quantization_table = source[2];
                if (component->horizontal_sampling >
                    state->horizontal_maximum) {
                    state->horizontal_maximum =
                        component->horizontal_sampling;
                }
                if (component->vertical_sampling > state->vertical_maximum) {
                    state->vertical_maximum = component->vertical_sampling;
                }
            }
            seen_frame = 1;
        } else if (marker == 0xdbU) {
            size_t table_position = 0U;

            while (table_position < data_size) {
                unsigned int table_index = data[table_position++] & 0x0fU;
                unsigned int index;

                for (index = 0U; index < JPEG_BLOCK_SIZE; ++index) {
                    state->quantization[table_index][zigzag[index]] =
                        data[table_position++];
                }
                state->quantization_valid[table_index] = 1U;
            }
        } else if (marker == 0xc4U) {
            size_t table_position = 0U;

            while (table_position < data_size) {
                unsigned int table_info = data[table_position++];
                unsigned int table_class = table_info >> 4U;
                unsigned int table_index = table_info & 0x0fU;
                JpegHuffman *table =
                    &state->huffman[table_class][table_index];
                unsigned int symbol_count = 0U;
                unsigned int index;

                for (index = 0U; index < 16U; ++index) {
                    table->counts[index] = data[table_position++];
                    symbol_count += table->counts[index];
                }
                avifdec_memory_copy(table->symbols, data + table_position,
                                    symbol_count);
                table_position += symbol_count;
                table->symbol_count = (uint16_t)symbol_count;
                table->valid = 1U;
            }
        } else if (marker == 0xddU) {
            state->restart_interval = avifdec_load_u16be(data);
        } else if (marker == 0xeeU && data_size >= 12U &&
                   data[0] == 'A' && data[1] == 'd' && data[2] == 'o' &&
                   data[3] == 'b' && data[4] == 'e') {
            adobe_transform = data[11];
        } else if (marker == 0xdaU) {
            unsigned int scan_components = data[0];
            unsigned int index;

            for (index = 0U; index < scan_components; ++index) {
                unsigned int identifier = data[1U + index * 2U];
                unsigned int selectors = data[2U + index * 2U];
                int component_index = jpeg_find_component(state, identifier);
                JpegComponent *component;

                if (component_index < 0) return IMAGE_INPUT_INVALID_DATA;
                component = &state->components[component_index];
                component->dc_table = selectors >> 4U;
                component->ac_table = selectors & 0x0fU;
                if (!state->huffman[0][component->dc_table].valid ||
                    !state->huffman[1][component->ac_table].valid ||
                    !state->quantization_valid[
                        component->quantization_table]) {
                    return IMAGE_INPUT_INVALID_DATA;
                }
                state->scan_order[index] = (uint8_t)component_index;
            }
            state->rgb_components = (uint8_t)(
                state->component_count == 3U &&
                (adobe_transform == 0 ||
                 (adobe_transform < 0 &&
                  state->components[0].identifier == 'R' &&
                  state->components[1].identifier == 'G' &&
                  state->components[2].identifier == 'B')));
            state->entropy_offset = position;
            return IMAGE_INPUT_OK;
        }
    }
    return IMAGE_INPUT_TRUNCATED;
}

static void jpeg_reader_init(JpegBitReader *reader,
                             const uint8_t *data,
                             size_t size,
                             size_t position) {
    reader->data = data;
    reader->size = size;
    reader->position = position;
    reader->bit_buffer = 0U;
    reader->bit_count = 0U;
    reader->marker = 0U;
    reader->status = IMAGE_INPUT_OK;
}

static int jpeg_entropy_byte(JpegBitReader *reader, unsigned int *value) {
    unsigned int byte;

    if (reader->position >= reader->size) {
        reader->status = IMAGE_INPUT_TRUNCATED;
        return 0;
    }
    byte = reader->data[reader->position++];
    if (byte != 0xffU) {
        *value = byte;
        return 1;
    }
    do {
        if (reader->position >= reader->size) {
            reader->status = IMAGE_INPUT_TRUNCATED;
            return 0;
        }
        byte = reader->data[reader->position++];
    } while (byte == 0xffU);
    if (byte == 0U) {
        *value = 0xffU;
        return 1;
    }
    reader->marker = byte;
    reader->status = IMAGE_INPUT_INVALID_DATA;
    return 0;
}

static int jpeg_read_bits(JpegBitReader *reader,
                          unsigned int count,
                          unsigned int *value) {
    while (reader->bit_count < count) {
        unsigned int byte;

        if (!jpeg_entropy_byte(reader, &byte)) return 0;
        reader->bit_buffer = (reader->bit_buffer << 8U) | byte;
        reader->bit_count += 8U;
    }
    reader->bit_count -= count;
    *value = count == 0U ? 0U :
        (reader->bit_buffer >> reader->bit_count) &
        (((unsigned int)1U << count) - 1U);
    if (reader->bit_count == 0U) {
        reader->bit_buffer = 0U;
    } else {
        reader->bit_buffer &= ((unsigned int)1U << reader->bit_count) - 1U;
    }
    return 1;
}

static int jpeg_huffman_decode(JpegBitReader *reader,
                               const JpegHuffman *table,
                               unsigned int *symbol) {
    unsigned int code = 0U;
    unsigned int first_code = 0U;
    unsigned int first_symbol = 0U;
    unsigned int length;

    for (length = 0U; length < 16U; ++length) {
        unsigned int bit;
        unsigned int count = table->counts[length];

        if (!jpeg_read_bits(reader, 1U, &bit)) return 0;
        code = (code << 1U) | bit;
        if (code >= first_code && code - first_code < count) {
            unsigned int index = first_symbol + code - first_code;

            if (index >= table->symbol_count) return 0;
            *symbol = table->symbols[index];
            return 1;
        }
        first_code = (first_code + count) << 1U;
        first_symbol += count;
    }
    reader->status = IMAGE_INPUT_INVALID_DATA;
    return 0;
}

static int jpeg_receive_extend(JpegBitReader *reader,
                               unsigned int size,
                               int32_t *value) {
    unsigned int bits;
    unsigned int threshold;

    if (size == 0U) {
        *value = 0;
        return 1;
    }
    if (size > 11U || !jpeg_read_bits(reader, size, &bits)) return 0;
    threshold = (unsigned int)1U << (size - 1U);
    *value = bits < threshold
        ? (int32_t)bits - (int32_t)(((unsigned int)1U << size) - 1U)
        : (int32_t)bits;
    return 1;
}

static int jpeg_decode_block(JpegBitReader *reader,
                             JpegState *state,
                             JpegComponent *component,
                             int32_t *coefficients) {
    static const uint8_t zigzag[JPEG_BLOCK_SIZE] = {
        0U, 1U, 8U, 16U, 9U, 2U, 3U, 10U,
        17U, 24U, 32U, 25U, 18U, 11U, 4U, 5U,
        12U, 19U, 26U, 33U, 40U, 48U, 41U, 34U,
        27U, 20U, 13U, 6U, 7U, 14U, 21U, 28U,
        35U, 42U, 49U, 56U, 57U, 50U, 43U, 36U,
        29U, 22U, 15U, 23U, 30U, 37U, 44U, 51U,
        58U, 59U, 52U, 45U, 38U, 31U, 39U, 46U,
        53U, 60U, 61U, 54U, 47U, 55U, 62U, 63U
    };
    const JpegHuffman *dc_table =
        &state->huffman[0][component->dc_table];
    const JpegHuffman *ac_table =
        &state->huffman[1][component->ac_table];
    const uint16_t *quantization =
        state->quantization[component->quantization_table];
    unsigned int symbol;
    int32_t difference;
    int32_t prediction;
    unsigned int coefficient = 1U;

    avifdec_memory_fill(coefficients, 0U,
                        sizeof(*coefficients) * JPEG_BLOCK_SIZE);
    if (!jpeg_huffman_decode(reader, dc_table, &symbol) || symbol > 11U ||
        !jpeg_receive_extend(reader, symbol, &difference)) {
        return 0;
    }
    prediction = component->dc_prediction + difference;
    if (prediction < -32768 || prediction > 32767) {
        reader->status = IMAGE_INPUT_INVALID_DATA;
        return 0;
    }
    component->dc_prediction = prediction;
    coefficients[0] = prediction * quantization[0];
    while (coefficient < JPEG_BLOCK_SIZE) {
        unsigned int run;
        unsigned int size;
        int32_t amplitude;

        if (!jpeg_huffman_decode(reader, ac_table, &symbol)) return 0;
        run = symbol >> 4U;
        size = symbol & 0x0fU;
        if (size == 0U) {
            if (run == 0U) break;
            if (run != 15U || coefficient > 48U) {
                reader->status = IMAGE_INPUT_INVALID_DATA;
                return 0;
            }
            coefficient += 16U;
            continue;
        }
        if (run >= JPEG_BLOCK_SIZE - coefficient) {
            reader->status = IMAGE_INPUT_INVALID_DATA;
            return 0;
        }
        coefficient += run;
        if (!jpeg_receive_extend(reader, size, &amplitude)) return 0;
        coefficients[zigzag[coefficient]] =
            amplitude * quantization[zigzag[coefficient]];
        ++coefficient;
    }
    return 1;
}

static int32_t jpeg_round_idct(int64_t value) {
    uint64_t magnitude;
    int32_t result;

    if (value >= 0) {
        magnitude = (uint64_t)value;
        return (int32_t)((magnitude + ((uint64_t)1U << 29U)) >> 30U);
    }
    magnitude = (uint64_t)(-value);
    result = (int32_t)((magnitude + ((uint64_t)1U << 29U)) >> 30U);
    return -result;
}

static void jpeg_idct(const int32_t *coefficients, int16_t *samples) {
    static const int16_t basis[JPEG_BLOCK_SIZE] = {
        11585, 16069, 15137, 13623, 11585, 9102, 6270, 3196,
        11585, 13623, 6270, -3196, -11585, -16069, -15137, -9102,
        11585, 9102, -6270, -16069, -11585, 3196, 15137, 13623,
        11585, 3196, -15137, -9102, 11585, 13623, -6270, -16069,
        11585, -3196, -15137, 9102, 11585, -13623, -6270, 16069,
        11585, -9102, -6270, 16069, -11585, -3196, 15137, -13623,
        11585, -13623, 6270, 3196, -11585, 16069, -15137, 9102,
        11585, -16069, 15137, -13623, 11585, -9102, 6270, -3196
    };
    unsigned int output_y;

    for (output_y = 0U; output_y < 8U; ++output_y) {
        unsigned int output_x;

        for (output_x = 0U; output_x < 8U; ++output_x) {
            int64_t sum = 0;
            unsigned int frequency_y;

            for (frequency_y = 0U; frequency_y < 8U; ++frequency_y) {
                unsigned int frequency_x;

                for (frequency_x = 0U; frequency_x < 8U; ++frequency_x) {
                    sum += (int64_t)coefficients[
                               frequency_y * 8U + frequency_x] *
                           basis[output_y * 8U + frequency_y] *
                           basis[output_x * 8U + frequency_x];
                }
            }
            sum = jpeg_round_idct(sum) + 128;
            if (sum < 0) sum = 0;
            if (sum > 255) sum = 255;
            samples[output_y * 8U + output_x] = (int16_t)sum;
        }
    }
}

static ImageInputStatus jpeg_take_marker(JpegBitReader *reader,
                                         unsigned int *marker) {
    unsigned int byte;

    if (reader->bit_count != 0U &&
        reader->bit_buffer !=
            (((uint32_t)1U << reader->bit_count) - 1U)) {
        return IMAGE_INPUT_INVALID_DATA;
    }
    reader->bit_buffer = 0U;
    reader->bit_count = 0U;
    if (reader->marker != 0U) {
        *marker = reader->marker;
        reader->marker = 0U;
        reader->status = IMAGE_INPUT_OK;
        return IMAGE_INPUT_OK;
    }
    if (reader->position >= reader->size) return IMAGE_INPUT_TRUNCATED;
    if (reader->data[reader->position++] != 0xffU) {
        return IMAGE_INPUT_INVALID_DATA;
    }
    do {
        if (reader->position >= reader->size) return IMAGE_INPUT_TRUNCATED;
        byte = reader->data[reader->position++];
    } while (byte == 0xffU);
    if (byte == 0U) return IMAGE_INPUT_INVALID_DATA;
    *marker = byte;
    reader->status = IMAGE_INPUT_OK;
    return IMAGE_INPUT_OK;
}

static uint8_t jpeg_clamp(int32_t value) {
    if (value < 0) return 0U;
    if (value > 255) return 255U;
    return (uint8_t)value;
}

static int16_t jpeg_component_sample(const JpegState *state,
                                     unsigned int component_index,
                                     unsigned int local_x,
                                     unsigned int local_y) {
    const JpegComponent *component = &state->components[component_index];
    unsigned int sample_x =
        local_x * component->horizontal_sampling / state->horizontal_maximum;
    unsigned int sample_y =
        local_y * component->vertical_sampling / state->vertical_maximum;
    unsigned int block_x = sample_x >> 3U;
    unsigned int block_y = sample_y >> 3U;
    unsigned int block =
        block_y * component->horizontal_sampling + block_x;

    return component->samples[block * JPEG_BLOCK_SIZE +
                              (sample_y & 7U) * 8U + (sample_x & 7U)];
}

static void jpeg_render_mcu(const JpegState *state,
                            uint32_t mcu_x,
                            uint32_t mcu_y,
                            uint8_t *output,
                            size_t stride) {
    unsigned int mcu_width = state->horizontal_maximum * 8U;
    unsigned int mcu_height = state->vertical_maximum * 8U;
    unsigned int local_y;

    for (local_y = 0U; local_y < mcu_height; ++local_y) {
        uint32_t image_y = mcu_y * mcu_height + local_y;
        unsigned int local_x;

        if (image_y >= state->height) break;
        for (local_x = 0U; local_x < mcu_width; ++local_x) {
            uint32_t image_x = mcu_x * mcu_width + local_x;
            uint8_t *pixel;

            if (image_x >= state->width) break;
            pixel = output + (size_t)image_y * stride +
                    (size_t)image_x * 3U;
            if (state->component_count == 1U) {
                uint8_t gray = (uint8_t)jpeg_component_sample(
                    state, 0U, local_x, local_y);

                pixel[0] = gray;
                pixel[1] = gray;
                pixel[2] = gray;
            } else if (state->rgb_components) {
                pixel[0] = (uint8_t)jpeg_component_sample(
                    state, 0U, local_x, local_y);
                pixel[1] = (uint8_t)jpeg_component_sample(
                    state, 1U, local_x, local_y);
                pixel[2] = (uint8_t)jpeg_component_sample(
                    state, 2U, local_x, local_y);
            } else {
                int32_t luma = jpeg_component_sample(
                    state, 0U, local_x, local_y);
                int32_t blue_difference = jpeg_component_sample(
                    state, 1U, local_x, local_y) - 128;
                int32_t red_difference = jpeg_component_sample(
                    state, 2U, local_x, local_y) - 128;
                int32_t red = luma +
                    (91881 * red_difference +
                     (red_difference >= 0 ? 32768 : -32768)) / 65536;
                int32_t green_delta = 22554 * blue_difference +
                                      46802 * red_difference;
                int32_t blue = luma +
                    (116130 * blue_difference +
                     (blue_difference >= 0 ? 32768 : -32768)) / 65536;
                int32_t green = luma -
                    (green_delta + (green_delta >= 0 ? 32768 : -32768)) /
                    65536;

                pixel[0] = jpeg_clamp(red);
                pixel[1] = jpeg_clamp(green);
                pixel[2] = jpeg_clamp(blue);
            }
        }
    }
}

static ImageInputStatus jpeg_decode_entropy(JpegState *state,
                                            int32_t *coefficients,
                                            const uint8_t *input,
                                            size_t input_size,
                                            uint8_t *output,
                                            size_t stride) {
    JpegBitReader reader;
    uint32_t mcu_columns =
        (state->width + state->horizontal_maximum * 8U - 1U) /
        (state->horizontal_maximum * 8U);
    uint32_t mcu_rows =
        (state->height + state->vertical_maximum * 8U - 1U) /
        (state->vertical_maximum * 8U);
    uint32_t mcu_index = 0U;
    uint32_t mcu_y;
    unsigned int expected_restart = 0U;

    jpeg_reader_init(&reader, input, input_size, state->entropy_offset);
    for (mcu_y = 0U; mcu_y < mcu_rows; ++mcu_y) {
        uint32_t mcu_x;

        for (mcu_x = 0U; mcu_x < mcu_columns; ++mcu_x) {
            unsigned int scan_index;

            if (state->restart_interval != 0U && mcu_index != 0U &&
                mcu_index % state->restart_interval == 0U) {
                unsigned int marker;
                unsigned int component_index;
                ImageInputStatus status = jpeg_take_marker(&reader, &marker);

                if (status != IMAGE_INPUT_OK) return status;
                if (marker != 0xd0U + expected_restart) {
                    return IMAGE_INPUT_INVALID_DATA;
                }
                expected_restart = (expected_restart + 1U) & 7U;
                for (component_index = 0U;
                     component_index < state->component_count;
                     ++component_index) {
                    state->components[component_index].dc_prediction = 0;
                }
            }
            for (scan_index = 0U; scan_index < state->component_count;
                 ++scan_index) {
                JpegComponent *component =
                    &state->components[state->scan_order[scan_index]];
                unsigned int block_y;

                for (block_y = 0U;
                     block_y < component->vertical_sampling; ++block_y) {
                    unsigned int block_x;

                    for (block_x = 0U;
                         block_x < component->horizontal_sampling; ++block_x) {
                        unsigned int block =
                            block_y * component->horizontal_sampling + block_x;

                        if (!jpeg_decode_block(&reader, state, component,
                                               coefficients)) {
                            return reader.status;
                        }
                        jpeg_idct(coefficients,
                                  component->samples +
                                      block * JPEG_BLOCK_SIZE);
                    }
                }
            }
            jpeg_render_mcu(state, mcu_x, mcu_y, output, stride);
            ++mcu_index;
        }
    }
    {
        unsigned int marker;
        ImageInputStatus status = jpeg_take_marker(&reader, &marker);

        if (status != IMAGE_INPUT_OK) return status;
        if (marker != 0xd9U) return IMAGE_INPUT_UNSUPPORTED;
    }
    return IMAGE_INPUT_OK;
}

ImageInputStatus image_input_jpeg_decode(const uint8_t *input,
                                         size_t input_size,
                                         void *workspace,
                                         size_t workspace_size,
                                         uint8_t *output,
                                         size_t output_capacity,
                                         ImageInputInfo *info) {
    ImageInputInfo queried;
    AvifdecArena arena;
    JpegState *state;
    int32_t *coefficients;
    int16_t *samples[JPEG_MAX_COMPONENTS];
    unsigned int component;
    ImageInputStatus status = image_input_jpeg_query(input, input_size, &queried);

    if (status != IMAGE_INPUT_OK) return status;
    if (workspace_size < queried.workspace_size) {
        return IMAGE_INPUT_WORKSPACE_TOO_SMALL;
    }
    if (output_capacity < queried.output_size) {
        return IMAGE_INPUT_OUTPUT_TOO_SMALL;
    }
    avifdec_arena_init(&arena, workspace, workspace_size);
    state = avifdec_arena_allocate(&arena, sizeof(*state), 8U);
    coefficients = avifdec_arena_allocate(
        &arena, sizeof(*coefficients) * JPEG_BLOCK_SIZE, 4U);
    for (component = 0U; component < JPEG_MAX_COMPONENTS; ++component) {
        samples[component] = avifdec_arena_allocate(
            &arena,
            sizeof(*samples[component]) * JPEG_BLOCK_SIZE *
                JPEG_MAX_BLOCKS_PER_COMPONENT,
            2U);
    }
    if (arena.status != AVIFDEC_OK || state == 0 || coefficients == 0) {
        return IMAGE_INPUT_WORKSPACE_TOO_SMALL;
    }
    avifdec_memory_fill(state, 0U, sizeof(*state));
    for (component = 0U; component < JPEG_MAX_COMPONENTS; ++component) {
        if (samples[component] == 0) return IMAGE_INPUT_WORKSPACE_TOO_SMALL;
        state->components[component].samples = samples[component];
    }
    status = jpeg_load_headers(input, input_size, state);
    if (status != IMAGE_INPUT_OK) return status;
    status = jpeg_decode_entropy(state, coefficients, input, input_size,
                                 output, queried.rgb_stride);
    if (status != IMAGE_INPUT_OK) return status;
    *info = queried;
    return IMAGE_INPUT_OK;
}