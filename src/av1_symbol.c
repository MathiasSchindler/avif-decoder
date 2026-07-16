#include "av1_symbol.h"

#define AV1_CDF_PROB_TOP 32768U
#define AV1_EC_PROB_SHIFT 6U
#define AV1_EC_MIN_PROB 4U

static int av1_symbol_byte_at(const Av1SymbolDecoder *decoder,
                              size_t position,
                              uint8_t *value) {
    size_t index;

    for (index = 0U; index < decoder->span_count; ++index) {
        if (position < decoder->spans[index].size) {
            *value = decoder->spans[index].data[position];
            return 1;
        }
        position -= decoder->spans[index].size;
    }
    return 0;
}

static uint32_t av1_symbol_raw_bits(Av1SymbolDecoder *decoder, unsigned int count) {
    uint32_t value = 0U;

    if (decoder->status != AVIFDEC_OK) return 0U;
    if (count > 32U || decoder->bit_position > decoder->size * 8U ||
        count > decoder->size * 8U - decoder->bit_position) {
        decoder->status = count > 32U ? AVIFDEC_INVALID_ARGUMENT : AVIFDEC_TRUNCATED;
        return 0U;
    }
    if (count != 0U && decoder->contiguous_data != 0) {
        size_t byte_offset = decoder->bit_position >> 3U;
        unsigned int leading =
            (unsigned int)(decoder->bit_position & 7U);
        unsigned int byte_count =
            (leading + count + 7U) >> 3U;
        unsigned int trailing =
            byte_count * 8U - leading - count;
        uint64_t window = 0U;
        unsigned int index;

        for (index = 0U; index < byte_count; ++index) {
            window = (window << 8U) |
                decoder->contiguous_data[byte_offset + index];
        }
        window >>= trailing;
        value = count == 32U
            ? (uint32_t)window
            : (uint32_t)(window & (((uint64_t)1U << count) - 1U));
        decoder->bit_position += count;
    } else {
        unsigned int index;

        for (index = 0U; index < count; ++index) {
            size_t position = decoder->bit_position++;
            uint8_t byte;

            if (!av1_symbol_byte_at(
                    decoder, decoder->start + position / 8U, &byte)) {
                decoder->status = AVIFDEC_TRUNCATED;
                return 0U;
            }
            value = (value << 1U) |
                ((byte >> (7U - position % 8U)) & 1U);
        }
    }
    return value;
}

static unsigned int av1_symbol_floor_log2(uint32_t value) {
    return 31U - (unsigned int)__builtin_clz(value);
}

static void av1_symbol_update_cdf(uint16_t *cdf, size_t symbols, size_t symbol) {
    unsigned int rate = 3U + (cdf[symbols] > 15U) + (cdf[symbols] > 31U);
    unsigned int symbol_bits =
        av1_symbol_floor_log2((uint32_t)symbols);
    uint32_t target = 0U;
    size_t index;

    rate += symbol_bits < 2U ? symbol_bits : 2U;
    for (index = 0U; index + 1U < symbols; ++index) {
        if (index == symbol) target = AV1_CDF_PROB_TOP;
        if (target < cdf[index]) {
            cdf[index] = (uint16_t)(cdf[index] - ((cdf[index] - target) >> rate));
        } else {
            cdf[index] = (uint16_t)(cdf[index] + ((target - cdf[index]) >> rate));
        }
    }
    if (cdf[symbols] < 32U) ++cdf[symbols];
}

AvifdecStatus av1_symbol_init(Av1SymbolDecoder *decoder,
                              const AvifdecSpan *spans,
                              size_t span_count,
                              size_t start,
                              size_t size,
                              int disable_cdf_update) {
    size_t stream_size = 0U;
    size_t index;
    size_t local_start;
    unsigned int initial_bits;
    uint32_t buffer;

    if (decoder == 0 || spans == 0 || span_count == 0U ||
        size > (size_t)INT64_MAX / 8U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (index = 0U; index < span_count; ++index) {
        if (spans[index].size > SIZE_MAX - stream_size) return AVIFDEC_OVERFLOW;
        stream_size += spans[index].size;
    }
    if (start > stream_size || size > stream_size - start) return AVIFDEC_TRUNCATED;
    decoder->spans = spans;
    decoder->span_count = span_count;
    decoder->contiguous_data = 0;
    decoder->start = start;
    decoder->size = size;
    decoder->bit_position = 0U;
    decoder->range = 1U << 15;
    decoder->max_bits = (int64_t)(size * 8U) - 15;
    decoder->status = AVIFDEC_OK;
    decoder->disable_cdf_update = disable_cdf_update != 0;
    local_start = start;
    for (index = 0U; index < span_count; ++index) {
        if (local_start <= spans[index].size &&
            size <= spans[index].size - local_start) {
            decoder->contiguous_data =
                spans[index].data + local_start;
            break;
        }
        if (local_start < spans[index].size) break;
        local_start -= spans[index].size;
    }
    initial_bits = size * 8U < 15U ? (unsigned int)(size * 8U) : 15U;
    buffer = av1_symbol_raw_bits(decoder, initial_bits);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    decoder->value = (AV1_CDF_PROB_TOP - 1U) ^ (buffer << (15U - initial_bits));
    return AVIFDEC_OK;
}

static uint32_t av1_symbol_read_bool(Av1SymbolDecoder *decoder) {
    uint32_t current;
    uint32_t symbol;
    unsigned int bits;
    unsigned int read_bits;
    uint32_t new_data;

    if (decoder == 0 || decoder->status != AVIFDEC_OK) {
        if (decoder == 0) return 0U;
        return 0U;
    }
    current = (decoder->range >> 1U) & ~((uint32_t)127U);
    current += AV1_EC_MIN_PROB;
    if (decoder->value >= current) {
        symbol = 0U;
        decoder->range -= current;
        decoder->value -= current;
    } else {
        symbol = 1U;
        decoder->range = current;
    }
    if (decoder->range == 0U) {
        decoder->status = AVIFDEC_INVALID_DATA;
        return 0U;
    }
    bits = 15U - av1_symbol_floor_log2(decoder->range);
    decoder->range <<= bits;
    read_bits = decoder->max_bits > 0
        ? (unsigned int)(decoder->max_bits < (int64_t)bits
            ? decoder->max_bits : (int64_t)bits)
        : 0U;
    new_data = av1_symbol_raw_bits(decoder, read_bits);
    decoder->value = (new_data << (bits - read_bits)) ^
        (((decoder->value + 1U) << bits) - 1U);
    decoder->max_bits -= bits;
    return symbol;
}

uint32_t av1_symbol_read(Av1SymbolDecoder *decoder, uint16_t *cdf, size_t symbols) {
    uint32_t current;
    uint32_t previous = 0U;
    size_t symbol = 0U;
    unsigned int bits;
    unsigned int read_bits;
    uint32_t new_data;
    size_t index;

    if (decoder == 0 || cdf == 0 || symbols < 2U || symbols > 16U ||
        cdf[symbols - 1U] != AV1_CDF_PROB_TOP || cdf[symbols] > 32U) {
        if (decoder != 0) decoder->status = AVIFDEC_INVALID_ARGUMENT;
        return 0U;
    }
    if (decoder->status != AVIFDEC_OK) return 0U;
    for (index = 0U; index + 1U < symbols; ++index) {
        if (cdf[index] > cdf[index + 1U]) {
            decoder->status = AVIFDEC_INVALID_DATA;
            return 0U;
        }
    }
    current = decoder->range;
    for (;;) {
        uint32_t inverse_probability;

        previous = current;
        inverse_probability = AV1_CDF_PROB_TOP - cdf[symbol];
        current = ((decoder->range >> 8) *
                   (inverse_probability >> AV1_EC_PROB_SHIFT)) >>
                  (7U - AV1_EC_PROB_SHIFT);
        current += AV1_EC_MIN_PROB * (uint32_t)(symbols - symbol - 1U);
        if (decoder->value >= current) break;
        if (++symbol >= symbols) {
            decoder->status = AVIFDEC_INVALID_DATA;
            return 0U;
        }
    }
    decoder->range = previous - current;
    decoder->value -= current;
    if (decoder->range == 0U) {
        decoder->status = AVIFDEC_INVALID_DATA;
        return 0U;
    }
    bits = 15U - av1_symbol_floor_log2(decoder->range);
    decoder->range <<= bits;
    read_bits = decoder->max_bits > 0
                ? (unsigned int)(decoder->max_bits < (int64_t)bits
                                 ? decoder->max_bits : (int64_t)bits)
                : 0U;
    new_data = av1_symbol_raw_bits(decoder, read_bits);
    decoder->value = (new_data << (bits - read_bits)) ^
                     (((decoder->value + 1U) << bits) - 1U);
    decoder->max_bits -= bits;
    if (!decoder->disable_cdf_update) av1_symbol_update_cdf(cdf, symbols, symbol);
    return (uint32_t)symbol;
}

uint32_t av1_symbol_read_literal(Av1SymbolDecoder *decoder, unsigned int bits) {
    uint32_t value = 0U;
    unsigned int index;

    if (bits > 32U) {
        if (decoder != 0) decoder->status = AVIFDEC_INVALID_ARGUMENT;
        return 0U;
    }
    for (index = 0U; index < bits; ++index) {
        value = (value << 1U) | av1_symbol_read_bool(decoder);
    }
    return value;
}

AvifdecStatus av1_symbol_exit(Av1SymbolDecoder *decoder) {
    size_t padding_end;
    size_t trailing_position;
    size_t consumed;
    size_t index;

    if (decoder == 0) return AVIFDEC_INVALID_ARGUMENT;
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    if (decoder->max_bits < -14) return AVIFDEC_INVALID_DATA;
    padding_end = decoder->size * 8U;
    consumed = (size_t)(decoder->max_bits + 15 < 15 ? decoder->max_bits + 15 : 15);
    if (decoder->bit_position < consumed) return AVIFDEC_INVALID_DATA;
    trailing_position = decoder->bit_position - consumed;
    if (decoder->max_bits > 0) decoder->bit_position += (size_t)decoder->max_bits;
    if (decoder->bit_position != padding_end) return AVIFDEC_INVALID_DATA;
    for (index = trailing_position; index < padding_end; ++index) {
        size_t saved_position = decoder->bit_position;
        uint32_t bit;

        decoder->bit_position = index;
        bit = av1_symbol_raw_bits(decoder, 1U);
        decoder->bit_position = saved_position;
        if ((index == trailing_position && bit != 1U) ||
            (index != trailing_position && bit != 0U)) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    return decoder->status;
}