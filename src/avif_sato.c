#include "avif_sato.h"

#include "base.h"

#define AVIF_SATO_INT64_MAX ((int64_t)(UINT64_MAX >> 1U))
#define AVIF_SATO_INT64_MIN (-AVIF_SATO_INT64_MAX - 1)

typedef struct {
    const AvifdecSpan *spans;
    size_t span_count;
    size_t span_index;
    size_t span_position;
    AvifdecStatus status;
} AvifSatoReader;

static size_t avif_sato_offset_add(size_t left, size_t right) {
    if (right > SIZE_MAX - left) return SIZE_MAX;
    return left + right;
}

static void avif_sato_reader_advance(AvifSatoReader *reader) {
    while (reader->span_index < reader->span_count &&
           reader->span_position >=
               reader->spans[reader->span_index].size) {
        ++reader->span_index;
        reader->span_position = 0U;
    }
}

static size_t avif_sato_reader_offset(const AvifSatoReader *reader) {
    size_t index = reader->span_index;
    size_t position = reader->span_position;

    while (index < reader->span_count &&
           position >= reader->spans[index].size) {
        ++index;
        position = 0U;
    }
    if (index < reader->span_count) {
        return avif_sato_offset_add(reader->spans[index].file_offset,
                                    position);
    }
    if (reader->span_count != 0U) {
        const AvifdecSpan *last =
            &reader->spans[reader->span_count - 1U];
        return avif_sato_offset_add(last->file_offset, last->size);
    }
    return 0U;
}

static uint8_t avif_sato_reader_read(AvifSatoReader *reader) {
    uint8_t value;

    if (reader->status != AVIFDEC_OK) return 0U;
    avif_sato_reader_advance(reader);
    if (reader->span_index >= reader->span_count) {
        reader->status = AVIFDEC_TRUNCATED;
        return 0U;
    }
    value = reader->spans[reader->span_index]
                .data[reader->span_position];
    ++reader->span_position;
    return value;
}

static int avif_sato_reader_has_data(const AvifSatoReader *reader) {
    size_t index = reader->span_index;
    size_t position = reader->span_position;

    while (index < reader->span_count) {
        if (position < reader->spans[index].size) return 1;
        ++index;
        position = 0U;
    }
    return 0;
}

static AvifdecStatus avif_sato_fail(AvifdecError *error,
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

static uint64_t avif_sato_mask(unsigned int bits) {
    if (bits == 64U) return UINT64_MAX;
    return ((uint64_t)1U << bits) - 1U;
}

static int64_t avif_sato_from_bits(uint64_t value,
                                   unsigned int bits) {
    uint64_t mask = avif_sato_mask(bits);
    uint64_t sign = (uint64_t)1U << (bits - 1U);
    uint64_t magnitude;

    value &= mask;
    if ((value & sign) == 0U) return (int64_t)value;
    magnitude = ((~value) & mask) + 1U;
    if (magnitude == ((uint64_t)1U << 63U)) {
        return AVIF_SATO_INT64_MIN;
    }
    return -(int64_t)magnitude;
}

static AvifdecStatus avif_sato_read_constant(AvifSatoReader *reader,
                                             unsigned int bits,
                                             int64_t *constant) {
    uint64_t value = 0U;
    unsigned int index;

    for (index = 0U; index < bits / 8U; ++index) {
        value = (value << 8U) | avif_sato_reader_read(reader);
        if (reader->status != AVIFDEC_OK) return reader->status;
    }
    *constant = avif_sato_from_bits(value, bits);
    return AVIFDEC_OK;
}

static void avif_sato_limits(unsigned int bits,
                             int64_t *minimum,
                             int64_t *maximum) {
    if (bits == 64U) {
        *minimum = AVIF_SATO_INT64_MIN;
        *maximum = AVIF_SATO_INT64_MAX;
    } else {
        *maximum =
            (int64_t)(((uint64_t)1U << (bits - 1U)) - 1U);
        *minimum = -*maximum - 1;
    }
}

static int64_t avif_sato_clamp(int64_t value,
                               int64_t minimum,
                               int64_t maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int64_t avif_sato_add(int64_t left,
                             int64_t right,
                             int64_t minimum,
                             int64_t maximum) {
    if (right > 0 && left > maximum - right) return maximum;
    if (right < 0 && left < minimum - right) return minimum;
    return left + right;
}

static int64_t avif_sato_subtract(int64_t left,
                                  int64_t right,
                                  int64_t minimum,
                                  int64_t maximum) {
    if (right > 0 && left < minimum + right) return minimum;
    if (right < 0 && left > maximum + right) return maximum;
    return left - right;
}

static uint64_t avif_sato_magnitude(int64_t value) {
    if (value >= 0) return (uint64_t)value;
    return (uint64_t)(-(value + 1)) + 1U;
}

static int64_t avif_sato_multiply(int64_t left,
                                  int64_t right,
                                  int64_t minimum,
                                  int64_t maximum) {
    uint64_t left_magnitude;
    uint64_t right_magnitude;
    uint64_t limit;
    uint64_t product;
    int negative;

    if (left == 0 || right == 0) return 0;
    negative = (left < 0) != (right < 0);
    left_magnitude = avif_sato_magnitude(left);
    right_magnitude = avif_sato_magnitude(right);
    limit = negative ? avif_sato_magnitude(minimum)
                     : (uint64_t)maximum;
    if (left_magnitude > limit / right_magnitude) {
        return negative ? minimum : maximum;
    }
    product = left_magnitude * right_magnitude;
    if (!negative) return (int64_t)product;
    if (product == ((uint64_t)1U << 63U)) {
        return AVIF_SATO_INT64_MIN;
    }
    return -(int64_t)product;
}

static uint64_t avif_sato_to_bits(int64_t value,
                                  unsigned int bits) {
    return (uint64_t)value & avif_sato_mask(bits);
}

static int64_t avif_sato_power(int64_t base,
                               int64_t exponent,
                               int64_t minimum,
                               int64_t maximum) {
    uint64_t remaining;
    int64_t factor;
    int64_t result;

    if (base == 0) return 0;
    if (exponent < 0) {
        if (base == 1) return 1;
        if (base == -1) {
            return ((uint64_t)exponent & 1U) != 0U ? -1 : 1;
        }
        return 0;
    }
    remaining = (uint64_t)exponent;
    factor = base;
    result = 1;
    while (remaining != 0U) {
        if ((remaining & 1U) != 0U) {
            result = avif_sato_multiply(
                result, factor, minimum, maximum);
        }
        remaining >>= 1U;
        if (remaining != 0U) {
            factor = avif_sato_multiply(
                factor, factor, minimum, maximum);
        }
    }
    return result;
}

AvifdecStatus avif_sato_parse(AvifSatoProgram *program,
                              const AvifdecSpan *spans,
                              size_t span_count,
                              size_t input_count,
                              AvifdecError *error) {
    AvifSatoReader reader;
    size_t index;
    size_t depth = 0U;
    uint8_t header;
    uint8_t token_count;

    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
    if (program == 0 || (spans == 0 && span_count != 0U) ||
        input_count > AVIF_SATO_MAX_INPUTS) {
        return avif_sato_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avifdec_memory_fill(program, 0U, sizeof(*program));
    for (index = 0U; index < span_count; ++index) {
        if (spans[index].data == 0 && spans[index].size != 0U) {
            return avif_sato_fail(error,
                                  AVIFDEC_INVALID_ARGUMENT,
                                  spans[index].file_offset,
                                  0U);
        }
    }

    reader.spans = spans;
    reader.span_count = span_count;
    reader.span_index = 0U;
    reader.span_position = 0U;
    reader.status = AVIFDEC_OK;

    header = avif_sato_reader_read(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avif_sato_fail(error,
                              reader.status,
                              avif_sato_reader_offset(&reader),
                              0U);
    }
    program->bit_depth_code = header & 3U;
    program->intermediate_bits =
        (uint8_t)(1U << (program->bit_depth_code + 3U));
    if ((header >> 6U) != 0U) {
        return avif_sato_fail(error,
                              AVIFDEC_UNSUPPORTED,
                              avif_sato_offset_add(
                                  spans[reader.span_index].file_offset,
                                  reader.span_position - 1U),
                              0U);
    }

    token_count = avif_sato_reader_read(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avif_sato_fail(error,
                              reader.status,
                              avif_sato_reader_offset(&reader),
                              0U);
    }
    if (token_count == 0U) {
        return avif_sato_fail(error,
                              AVIFDEC_INVALID_DATA,
                              avif_sato_offset_add(
                                  spans[reader.span_index].file_offset,
                                  reader.span_position - 1U),
                              0U);
    }
    program->token_count = token_count;

    for (index = 0U; index < token_count; ++index) {
        size_t token_offset = avif_sato_reader_offset(&reader);
        uint32_t context = (uint32_t)index + 1U;
        uint8_t token = avif_sato_reader_read(&reader);

        if (reader.status != AVIFDEC_OK) {
            return avif_sato_fail(error,
                                  reader.status,
                                  avif_sato_reader_offset(&reader),
                                  context);
        }
        program->tokens[index] = token;
        if (token == 0U) {
            AvifdecStatus status;

            if (depth >= AVIF_SATO_MAX_STACK_DEPTH ||
                program->constant_count >=
                    AVIF_SATO_MAX_CONSTANTS) {
                return avif_sato_fail(error,
                                      AVIFDEC_LIMIT_EXCEEDED,
                                      token_offset,
                                      context);
            }
            status = avif_sato_read_constant(
                &reader,
                program->intermediate_bits,
                &program->constants[program->constant_count]);
            if (status != AVIFDEC_OK) {
                return avif_sato_fail(
                    error,
                    status,
                    avif_sato_reader_offset(&reader),
                    context);
            }
            ++program->constant_count;
            ++depth;
        } else if (token <= AVIF_SATO_MAX_INPUTS) {
            if ((size_t)token > input_count) {
                return avif_sato_fail(error,
                                      AVIFDEC_INVALID_DATA,
                                      token_offset,
                                      context);
            }
            if (depth >= AVIF_SATO_MAX_STACK_DEPTH) {
                return avif_sato_fail(error,
                                      AVIFDEC_LIMIT_EXCEEDED,
                                      token_offset,
                                      context);
            }
            if (token > program->required_input_count) {
                program->required_input_count = token;
            }
            ++depth;
        } else if (token >= 64U && token <= 67U) {
            if (depth == 0U) {
                return avif_sato_fail(error,
                                      AVIFDEC_INVALID_DATA,
                                      token_offset,
                                      context);
            }
        } else if (token >= 128U && token <= 137U) {
            if (depth < 2U) {
                return avif_sato_fail(error,
                                      AVIFDEC_INVALID_DATA,
                                      token_offset,
                                      context);
            }
            --depth;
        } else {
            return avif_sato_fail(error,
                                  AVIFDEC_UNSUPPORTED,
                                  token_offset,
                                  context);
        }
        if (depth > program->max_stack_depth) {
            program->max_stack_depth = (uint8_t)depth;
        }
    }

    if (avif_sato_reader_has_data(&reader)) {
        return avif_sato_fail(error,
                              AVIFDEC_INVALID_DATA,
                              avif_sato_reader_offset(&reader),
                              AVIF_SATO_ERROR_TRAILING);
    }
    if (depth != 1U) {
        return avif_sato_fail(error,
                              AVIFDEC_INVALID_DATA,
                              avif_sato_reader_offset(&reader),
                              AVIF_SATO_ERROR_EXPRESSION);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_sato_evaluate(const AvifSatoProgram *program,
                                 const int64_t *input_samples,
                                 size_t input_count,
                                 int64_t output_minimum,
                                 int64_t output_maximum,
                                 int64_t *result) {
    int64_t stack[AVIF_SATO_MAX_STACK_DEPTH];
    int64_t minimum;
    int64_t maximum;
    size_t depth = 0U;
    size_t constant_index = 0U;
    size_t index;
    unsigned int bits;

    if (program == 0 || result == 0 ||
        (input_samples == 0 && input_count != 0U) ||
        input_count > AVIF_SATO_MAX_INPUTS ||
        output_minimum > output_maximum) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    bits = program->intermediate_bits;
    if ((bits != 8U && bits != 16U && bits != 32U &&
         bits != 64U) ||
        program->token_count == 0U ||
        program->required_input_count > input_count ||
        program->constant_count > AVIF_SATO_MAX_CONSTANTS) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    avif_sato_limits(bits, &minimum, &maximum);

    for (index = 0U; index < program->token_count; ++index) {
        uint8_t token = program->tokens[index];

        if (token == 0U) {
            if (constant_index >= program->constant_count ||
                depth >= AVIF_SATO_MAX_STACK_DEPTH) {
                return AVIFDEC_INVALID_DATA;
            }
            stack[depth++] = program->constants[constant_index++];
        } else if (token <= AVIF_SATO_MAX_INPUTS) {
            if ((size_t)token > input_count ||
                depth >= AVIF_SATO_MAX_STACK_DEPTH) {
                return AVIFDEC_INVALID_DATA;
            }
            stack[depth++] = avif_sato_clamp(
                input_samples[token - 1U], minimum, maximum);
        } else if (token >= 64U && token <= 67U) {
            int64_t left;

            if (depth == 0U) return AVIFDEC_INVALID_DATA;
            left = stack[depth - 1U];
            if (token == 64U || token == 65U) {
                stack[depth - 1U] =
                    left == minimum
                        ? maximum
                        : (left < 0 ? -left : left);
                if (token == 64U && left > 0) {
                    stack[depth - 1U] = -left;
                }
            } else if (token == 66U) {
                stack[depth - 1U] = avif_sato_from_bits(
                    ~avif_sato_to_bits(left, bits), bits);
            } else {
                uint64_t value;
                int64_t bit = 0;

                if (left <= 0) {
                    stack[depth - 1U] = 0;
                    continue;
                }
                value = (uint64_t)left;
                while (value > 1U) {
                    value >>= 1U;
                    ++bit;
                }
                stack[depth - 1U] = bit;
            }
        } else if (token >= 128U && token <= 137U) {
            int64_t right;
            int64_t left;
            int64_t value;

            if (depth < 2U) return AVIFDEC_INVALID_DATA;
            right = stack[--depth];
            left = stack[depth - 1U];
            if (token == 128U) {
                value = avif_sato_add(
                    left, right, minimum, maximum);
            } else if (token == 129U) {
                value = avif_sato_subtract(
                    left, right, minimum, maximum);
            } else if (token == 130U) {
                value = avif_sato_multiply(
                    left, right, minimum, maximum);
            } else if (token == 131U) {
                if (right == 0) {
                    value = left;
                } else if (left == minimum && right == -1) {
                    value = maximum;
                } else {
                    value = left / right;
                }
            } else if (token >= 132U && token <= 134U) {
                uint64_t left_bits = avif_sato_to_bits(left, bits);
                uint64_t right_bits =
                    avif_sato_to_bits(right, bits);
                uint64_t value_bits;

                if (token == 132U) {
                    value_bits = left_bits & right_bits;
                } else if (token == 133U) {
                    value_bits = left_bits | right_bits;
                } else {
                    value_bits = left_bits ^ right_bits;
                }
                value = avif_sato_from_bits(value_bits, bits);
            } else if (token == 135U) {
                value = avif_sato_power(
                    left, right, minimum, maximum);
            } else if (token == 136U) {
                value = left <= right ? left : right;
            } else {
                value = left <= right ? right : left;
            }
            stack[depth - 1U] = value;
        } else {
            return AVIFDEC_UNSUPPORTED;
        }
    }
    if (depth != 1U || constant_index != program->constant_count) {
        return AVIFDEC_INVALID_DATA;
    }
    *result = avif_sato_clamp(
        stack[0], output_minimum, output_maximum);
    return AVIFDEC_OK;
}

AvifdecStatus avif_sato_apply_rows(
    const AvifSatoProgram *program,
    const AvifdecImage *inputs,
    size_t input_count,
    unsigned int plane,
    uint32_t width,
    uint32_t row_begin,
    uint32_t row_end,
    int64_t output_minimum,
    int64_t output_maximum,
    AvifdecImage *output) {
    uint32_t row;
    size_t input_index;

    if (program == 0 || inputs == 0 || output == 0 ||
        input_count == 0U ||
        input_count > AVIF_SATO_MAX_INPUTS ||
        plane >= 3U || width == 0U ||
        row_begin > row_end ||
        output_minimum > output_maximum ||
        output->planes[plane] == 0 ||
        output->strides[plane] < width) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (input_index = 0U;
         input_index < input_count;
         ++input_index) {
        if (inputs[input_index].planes[plane] == 0 ||
            inputs[input_index].strides[plane] < width ||
            inputs[input_index].widths[plane] < width ||
            inputs[input_index].heights[plane] < row_end) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
    }
    for (row = row_begin; row < row_end; ++row) {
        uint32_t column;

        for (column = 0U; column < width; ++column) {
            int64_t samples[AVIF_SATO_MAX_INPUTS];
            int64_t result;
            AvifdecStatus status;

            for (input_index = 0U;
                 input_index < input_count;
                 ++input_index) {
                samples[input_index] =
                    inputs[input_index].planes[plane][
                        (size_t)row *
                        inputs[input_index].strides[plane] +
                        column];
            }
            status = avif_sato_evaluate(
                program, samples, input_count,
                output_minimum, output_maximum,
                &result);
            if (status != AVIFDEC_OK) return status;
            output->planes[plane][
                (size_t)row * output->strides[plane] +
                column] = (uint16_t)result;
        }
    }
    return AVIFDEC_OK;
}
