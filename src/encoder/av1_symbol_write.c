#include "encoder/av1_symbol_write.h"
#include "av1_cdf.h"
#include "base.h"

#define AVIFENC_AV1_CDF_PROB_TOP 32768U
#define AVIFENC_AV1_EC_PROB_SHIFT 6U
#define AVIFENC_AV1_EC_MIN_PROB 4U

static AvifencStatus symbol_writer_fail(AvifencAv1SymbolWriter *writer,
                                        AvifencStatus status) {
    if (writer != 0 && writer->status == AVIFENC_OK) writer->status = status;
    return writer == 0 ? AVIFENC_INVALID_ARGUMENT : writer->status;
}

static void symbol_writer_reset(AvifencAv1SymbolWriter *writer,
                                int disable_cdf_update) {
    writer->position = 0U;
    writer->low = 0U;
    writer->range = 0x8000U;
    writer->count = -9;
    writer->disable_cdf_update = disable_cdf_update != 0;
    writer->finalized = 0U;
}

void avifenc_av1_symbol_writer_init(AvifencAv1SymbolWriter *writer,
                                    void *data,
                                    size_t capacity,
                                    int disable_cdf_update) {
    if (writer == 0) return;
    writer->data = (uint8_t *)data;
    writer->capacity = capacity;
    writer->status = data == 0 && capacity != 0U
        ? AVIFENC_INVALID_ARGUMENT : AVIFENC_OK;
    writer->sizing_only = 0U;
    symbol_writer_reset(writer, disable_cdf_update);
}

void avifenc_av1_symbol_writer_init_sizing(
    AvifencAv1SymbolWriter *writer,
    int disable_cdf_update) {
    if (writer == 0) return;
    writer->data = 0;
    writer->capacity = 0U;
    writer->status = AVIFENC_OK;
    writer->sizing_only = 1U;
    symbol_writer_reset(writer, disable_cdf_update);
}

size_t avifenc_av1_symbol_writer_size(
    const AvifencAv1SymbolWriter *writer) {
    return writer == 0 ? 0U : writer->position;
}

static unsigned int symbol_writer_floor_log2(uint32_t value) {
    unsigned int result = 0U;

    while (value > 1U) {
        value >>= 1U;
        ++result;
    }
    return result;
}

static int symbol_writer_can_carry(
    const AvifencAv1SymbolWriter *writer) {
    size_t position = writer->position;

    if (writer->sizing_only) return 1;
    while (position != 0U) {
        --position;
        if (writer->data[position] != 0xffU) return 1;
    }
    return 0;
}

static void symbol_writer_propagate_carry(
    AvifencAv1SymbolWriter *writer) {
    size_t position = writer->position;
    uint16_t carry = 1U;

    while (carry != 0U) {
        uint16_t sum;

        --position;
        sum = (uint16_t)writer->data[position] + carry;
        writer->data[position] = (uint8_t)sum;
        carry = sum >> 8U;
    }
}

static AvifencStatus symbol_writer_prepare(
    AvifencAv1SymbolWriter *writer,
    size_t bytes,
    int carry) {
    size_t end;

    if (!avifdec_size_add(writer->position, bytes, &end)) {
        return symbol_writer_fail(writer, AVIFENC_OVERFLOW);
    }
    if (!writer->sizing_only && end > writer->capacity) {
        return symbol_writer_fail(writer, AVIFENC_OUTPUT_TOO_SMALL);
    }
    if (carry && !symbol_writer_can_carry(writer)) {
        return symbol_writer_fail(writer, AVIFENC_OVERFLOW);
    }
    return AVIFENC_OK;
}

static AvifencStatus symbol_writer_normalize(
    AvifencAv1SymbolWriter *writer,
    uint64_t low,
    uint32_t range) {
    unsigned int shift;
    int count;
    int pending;

    if (range == 0U || range > 65535U) {
        return symbol_writer_fail(writer, AVIFENC_OVERFLOW);
    }
    shift = 15U - symbol_writer_floor_log2(range);
    count = writer->count;
    pending = count + (int)shift;
    if (pending >= 40) {
        unsigned int ready = (unsigned int)(pending >> 3) + 1U;
        uint64_t output;
        uint64_t carry_mask;
        uint64_t output_mask;
        uint64_t remaining_mask;
        int carry;
        unsigned int index;

        if (ready > 7U) {
            return symbol_writer_fail(writer, AVIFENC_OVERFLOW);
        }
        count += 24 - (int)(ready << 3U);
        if (count < 0 || count >= 64) {
            return symbol_writer_fail(writer, AVIFENC_OVERFLOW);
        }
        output = low >> (unsigned int)count;
        remaining_mask = count == 0
            ? 0U : (((uint64_t)1U << (unsigned int)count) - 1U);
        low &= remaining_mask;
        carry_mask = (uint64_t)1U << (ready << 3U);
        carry = (output & carry_mask) != 0U;
        output_mask = carry_mask - 1U;
        output &= output_mask;
        if (symbol_writer_prepare(writer, ready, carry) != AVIFENC_OK) {
            return writer->status;
        }
        if (!writer->sizing_only) {
            if (carry) symbol_writer_propagate_carry(writer);
            for (index = 0U; index < ready; ++index) {
                writer->data[writer->position + index] = (uint8_t)(
                    output >> ((ready - index - 1U) * 8U));
            }
        }
        writer->position += ready;
        pending = count + (int)shift - 24;
    }
    writer->low = low << shift;
    writer->range = (uint16_t)(range << shift);
    writer->count = (int16_t)pending;
    return AVIFENC_OK;
}

static AvifencStatus symbol_writer_bool(
    AvifencAv1SymbolWriter *writer,
    uint32_t value) {
    uint64_t low = writer->low;
    uint32_t range = writer->range;
    uint32_t split;

    split = ((range >> 8U) *
             (16384U >> AVIFENC_AV1_EC_PROB_SHIFT)) >>
            (7U - AVIFENC_AV1_EC_PROB_SHIFT);
    split += AVIFENC_AV1_EC_MIN_PROB;
    if (value != 0U) low += range - split;
    range = value != 0U ? split : range - split;
    return symbol_writer_normalize(writer, low, range);
}

static int symbol_writer_cdf_valid(const uint16_t *cdf,
                                   size_t symbols) {
    size_t index;

    if (cdf == 0 || symbols < 2U || symbols > 16U ||
        cdf[symbols - 1U] != AVIFENC_AV1_CDF_PROB_TOP ||
        cdf[symbols] > 32U) {
        return 0;
    }
    for (index = 0U; index + 1U < symbols; ++index) {
        if (cdf[index] > cdf[index + 1U]) return 0;
    }
    return 1;
}

AvifencStatus avifenc_av1_symbol_writer_write(
    AvifencAv1SymbolWriter *writer,
    uint16_t *cdf,
    size_t symbols,
    size_t symbol) {
    uint64_t low;
    uint32_t range;
    uint32_t lower;
    uint32_t upper;
    uint32_t scaled_lower;
    uint32_t scaled_upper;
    size_t last;
    AvifencStatus status;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (writer->finalized || symbol >= symbols ||
        !symbol_writer_cdf_valid(cdf, symbols)) {
        return symbol_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    low = writer->low;
    range = writer->range;
    lower = symbol == 0U
        ? AVIFENC_AV1_CDF_PROB_TOP
        : AVIFENC_AV1_CDF_PROB_TOP - cdf[symbol - 1U];
    upper = AVIFENC_AV1_CDF_PROB_TOP - cdf[symbol];
    last = symbols - 1U;
    scaled_upper = ((range >> 8U) *
                    (upper >> AVIFENC_AV1_EC_PROB_SHIFT)) >>
                   (7U - AVIFENC_AV1_EC_PROB_SHIFT);
    scaled_upper += AVIFENC_AV1_EC_MIN_PROB *
                    (uint32_t)(last - symbol);
    if (lower < AVIFENC_AV1_CDF_PROB_TOP) {
        scaled_lower = ((range >> 8U) *
                        (lower >> AVIFENC_AV1_EC_PROB_SHIFT)) >>
                       (7U - AVIFENC_AV1_EC_PROB_SHIFT);
        scaled_lower += AVIFENC_AV1_EC_MIN_PROB *
                        (uint32_t)(last - symbol + 1U);
        low += range - scaled_lower;
        range = scaled_lower - scaled_upper;
    } else {
        range -= scaled_upper;
    }
    status = symbol_writer_normalize(writer, low, range);
    if (status == AVIFENC_OK && !writer->disable_cdf_update) {
        av1_cdf_update(cdf, symbols, symbol);
    }
    return status;
}

AvifencStatus avifenc_av1_symbol_writer_literal(
    AvifencAv1SymbolWriter *writer,
    uint32_t value,
    unsigned int bit_count) {
    unsigned int index;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (writer->finalized || bit_count > 32U ||
        (bit_count != 0U && bit_count < 32U &&
         (value >> bit_count) != 0U)) {
        return symbol_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    for (index = 0U; index < bit_count; ++index) {
        AvifencStatus status = symbol_writer_bool(
            writer, (value >> (bit_count - index - 1U)) & 1U);

        if (status != AVIFENC_OK) return status;
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_symbol_writer_finish(
    AvifencAv1SymbolWriter *writer) {
    uint64_t end;
    uint64_t mask = 0x3fffU;
    int count;
    int pending;
    size_t bytes;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (writer->finalized) {
        return symbol_writer_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    count = writer->count;
    pending = count + 10;
    bytes = pending > 0 ? (size_t)(pending + 7) >> 3U : 0U;
    if (symbol_writer_prepare(writer, bytes, 0) != AVIFENC_OK) {
        return writer->status;
    }
    end = ((writer->low + mask) & ~mask) | (mask + 1U);
    while (pending > 0) {
        uint64_t remaining_mask;
        uint16_t value;
        int carry;

        if (count < -15 || count > 47) {
            return symbol_writer_fail(writer, AVIFENC_OVERFLOW);
        }
        value = (uint16_t)(end >> (unsigned int)(count + 16));
        carry = (value & 0x0100U) != 0U;
        if (carry && !symbol_writer_can_carry(writer)) {
            return symbol_writer_fail(writer, AVIFENC_OVERFLOW);
        }
        if (!writer->sizing_only) {
            if (carry) symbol_writer_propagate_carry(writer);
            writer->data[writer->position] = (uint8_t)value;
        }
        ++writer->position;
        remaining_mask = (uint64_t)1U << (unsigned int)(count + 16);
        remaining_mask -= 1U;
        end &= remaining_mask;
        pending -= 8;
        count -= 8;
    }
    writer->finalized = 1U;
    return AVIFENC_OK;
}