#include "encoder/av1_tile_palette.h"

static unsigned int tile_ceil_log2(uint32_t value) {
    unsigned int bits = 0U;

    if (value <= 1U) return 0U;
    --value;
    while (value != 0U) {
        value >>= 1U;
        ++bits;
    }
    return bits;
}

static unsigned int tile_log2_mi(uint32_t value) {
    unsigned int result = 0U;

    while (value > 1U) {
        value >>= 1U;
        ++result;
    }
    return result;
}

unsigned int avifenc_av1_tile_palette_block_context(
    uint32_t width_mi,
    uint32_t height_mi) {
    return tile_log2_mi(width_mi) + tile_log2_mi(height_mi) - 2U;
}

int avifenc_av1_tile_palette_classify_luma(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    AvifencAv1LumaDecision *decision) {
    uint32_t width = width_mi << 2U;
    uint32_t height = height_mi << 2U;
    uint32_t y;
    uint32_t x;
    uint8_t count = 0U;

    if (width_mi < 2U || height_mi < 2U || width_mi > 16U ||
        height_mi > 16U || ((uint64_t)column + width_mi) * 4U >
            state->source->width || ((uint64_t)row + height_mi) * 4U >
            state->source->height ||
        (row != 0U && state->palette_sizes_y[
            (size_t)(row - 1U) * state->mi_columns + column] != 0U) ||
        (column != 0U && state->palette_sizes_y[
            (size_t)row * state->mi_columns + column - 1U] != 0U)) {
        return 0;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source = state->source->planes[0] +
            ((size_t)(row << 2U) + y) * state->source->strides[0] +
            (column << 2U);

        for (x = 0U; x < width; ++x) {
            uint8_t sample = source[x];
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors[index] == sample) break;
            }
            if (index == count) {
                if (count == 8U) return 0;
                decision->palette_colors[count++] = sample;
            }
        }
    }
    if (count < 2U) return 0;
    for (x = 1U; x < count; ++x) {
        uint16_t value = decision->palette_colors[x];
        uint32_t position = x;

        while (position != 0U &&
               decision->palette_colors[position - 1U] > value) {
            decision->palette_colors[position] =
                decision->palette_colors[position - 1U];
            --position;
        }
        decision->palette_colors[position] = value;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source = state->source->planes[0] +
            ((size_t)(row << 2U) + y) * state->source->strides[0] +
            (column << 2U);

        for (x = 0U; x < width; ++x) {
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors[index] == source[x]) break;
            }
            state->palette_map_y[(size_t)y * width + x] = index;
        }
    }
    decision->mode = 0U;
    decision->angle_delta = 0;
    decision->filter_intra_mode = -1;
    decision->palette_size = count;
    return 1;
}

int avifenc_av1_tile_palette_classify_chroma(
    AvifencAv1TileState *state,
    uint32_t row,
    uint32_t column,
    uint32_t width_mi,
    uint32_t height_mi,
    AvifencAv1ChromaDecision *decision) {
    uint32_t width = width_mi << 1U;
    uint32_t height = height_mi << 1U;
    uint32_t y;
    uint32_t x;
    uint8_t count = 0U;

    if (width_mi < 2U || height_mi < 2U || width_mi > 8U ||
        height_mi > 8U || ((uint64_t)column + width_mi) * 4U >
            state->source->width || ((uint64_t)row + height_mi) * 4U >
            state->source->height ||
        (row != 0U && state->palette_sizes_uv[
            (size_t)(row - 1U) * state->mi_columns + column] != 0U) ||
        (column != 0U && state->palette_sizes_uv[
            (size_t)row * state->mi_columns + column - 1U] != 0U)) {
        return 0;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source_u = state->source->planes[1] +
            ((size_t)(row << 1U) + y) * state->source->strides[1] +
            (column << 1U);
        const uint8_t *source_v = state->source->planes[2] +
            ((size_t)(row << 1U) + y) * state->source->strides[2] +
            (column << 1U);

        for (x = 0U; x < width; ++x) {
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors_u[index] == source_u[x] &&
                    decision->palette_colors_v[index] == source_v[x]) {
                    break;
                }
            }
            if (index == count) {
                if (count == 8U) return 0;
                decision->palette_colors_u[count] = source_u[x];
                decision->palette_colors_v[count] = source_v[x];
                ++count;
            }
        }
    }
    if (count < 2U) return 0;
    for (x = 1U; x < count; ++x) {
        uint16_t value_u = decision->palette_colors_u[x];
        uint16_t value_v = decision->palette_colors_v[x];
        uint32_t position = x;

        while (position != 0U &&
               decision->palette_colors_u[position - 1U] > value_u) {
            decision->palette_colors_u[position] =
                decision->palette_colors_u[position - 1U];
            decision->palette_colors_v[position] =
                decision->palette_colors_v[position - 1U];
            --position;
        }
        decision->palette_colors_u[position] = value_u;
        decision->palette_colors_v[position] = value_v;
    }
    for (y = 0U; y < height; ++y) {
        const uint8_t *source_u = state->source->planes[1] +
            ((size_t)(row << 1U) + y) * state->source->strides[1] +
            (column << 1U);
        const uint8_t *source_v = state->source->planes[2] +
            ((size_t)(row << 1U) + y) * state->source->strides[2] +
            (column << 1U);

        for (x = 0U; x < width; ++x) {
            uint8_t index;

            for (index = 0U; index < count; ++index) {
                if (decision->palette_colors_u[index] == source_u[x] &&
                    decision->palette_colors_v[index] == source_v[x]) {
                    break;
                }
            }
            state->palette_map_uv[(size_t)y * width + x] = index;
        }
    }
    decision->mode = 0U;
    decision->angle_delta = 0;
    decision->alpha_u = 0;
    decision->alpha_v = 0;
    decision->palette_size = count;
    return 1;
}

static unsigned int tile_palette_color_context(const uint8_t *map,
                                                uint32_t stride,
                                                uint32_t row,
                                                uint32_t column,
                                                uint8_t colors,
                                                uint8_t order[8]) {
    static const int8_t contexts[9] = {
        -1, -1, 0, -1, -1, 4, 3, 2, 1
    };
    uint8_t scores[8] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    unsigned int index;
    unsigned int hash;

    for (index = 0U; index < colors; ++index) order[index] = (uint8_t)index;
    if (column != 0U) scores[map[(size_t)row * stride + column - 1U]] += 2U;
    if (row != 0U && column != 0U) {
        ++scores[map[(size_t)(row - 1U) * stride + column - 1U]];
    }
    if (row != 0U) scores[map[(size_t)(row - 1U) * stride + column]] += 2U;
    for (index = 0U; index < 3U; ++index) {
        unsigned int best = index;
        unsigned int candidate;

        for (candidate = index + 1U; candidate < colors; ++candidate) {
            if (scores[candidate] > scores[best]) best = candidate;
        }
        if (best != index) {
            uint8_t best_score = scores[best];
            uint8_t best_color = order[best];

            for (candidate = best; candidate > index; --candidate) {
                scores[candidate] = scores[candidate - 1U];
                order[candidate] = order[candidate - 1U];
            }
            scores[index] = best_score;
            order[index] = best_color;
        }
    }
    hash = scores[0] + 2U * scores[1] + 2U * scores[2];
    return (unsigned int)contexts[hash];
}

static uint64_t tile_palette_map_rate_cost(
    const AvifencAv1TileState *state,
    const uint8_t *map,
    uint32_t width,
    uint32_t height,
    uint8_t palette_size,
    unsigned int plane) {
    unsigned int first_width = tile_ceil_log2(palette_size);
    uint32_t minimum = (1U << first_width) - palette_size;
    uint64_t cost = (first_width -
        (map[0] < minimum ? 1U : 0U)) * 256U;
    uint32_t diagonal;

    for (diagonal = 1U; diagonal < width + height - 1U; ++diagonal) {
        uint32_t column = diagonal < width ? diagonal : width - 1U;
        uint32_t minimum = diagonal >= height
            ? diagonal - height + 1U : 0U;

        for (;;) {
            uint32_t token_row = diagonal - column;
            uint8_t order[8];
            unsigned int context = tile_palette_color_context(
                map, width, token_row, column, palette_size, order);
            unsigned int symbol;

            for (symbol = 0U; symbol < palette_size; ++symbol) {
                if (order[symbol] == map[(size_t)token_row * width + column]) {
                    break;
                }
            }
            cost += tile_symbol_cost(
                state->palette_cdfs.color[plane][palette_size - 2U][context],
                symbol);
            if (column == minimum) break;
            --column;
        }
    }
    return cost;
}

uint64_t avifenc_av1_tile_palette_luma_rate_cost(
    const AvifencAv1TileState *state,
    uint32_t width_mi,
    uint32_t height_mi,
    const AvifencAv1LumaDecision *decision) {
    unsigned int block_context = avifenc_av1_tile_palette_block_context(
        width_mi, height_mi);
    unsigned int bits = 8U;
    uint64_t cost = tile_symbol_cost(
        state->palette_cdfs.y_mode[block_context][0], 1U);
    uint8_t index;

    cost += tile_symbol_cost(
        state->palette_cdfs.y_size[block_context],
        decision->palette_size - 2U);
    cost += (8U + 2U) * 256U;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors[index];
        uint32_t range;
        unsigned int range_bits;

        cost += bits * 256U;
        range = 255U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    return cost + tile_palette_map_rate_cost(
        state, state->palette_map_y, width_mi << 2U, height_mi << 2U,
        decision->palette_size, 0U);
}

uint64_t avifenc_av1_tile_palette_chroma_rate_cost(
    const AvifencAv1TileState *state,
    uint32_t width_mi,
    uint32_t height_mi,
    const AvifencAv1ChromaDecision *decision) {
    unsigned int block_context = avifenc_av1_tile_palette_block_context(
        width_mi, height_mi);
    unsigned int bits = 8U;
    uint64_t cost = tile_symbol_cost(
        state->palette_cdfs.uv_mode[0], 1U);
    uint8_t index;

    cost += tile_symbol_cost(
        state->palette_cdfs.uv_size[block_context],
        decision->palette_size - 2U);
    cost += (8U + 2U + 1U + 8U * decision->palette_size) * 256U;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors_u[index];
        uint32_t range;
        unsigned int range_bits;

        cost += bits * 256U;
        range = 256U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    return cost + tile_palette_map_rate_cost(
        state, state->palette_map_uv, width_mi << 1U, height_mi << 1U,
        decision->palette_size, 1U);
}

static AvifencStatus tile_write_ns(AvifencAv1SymbolWriter *writer,
                                   uint32_t value,
                                   uint32_t symbols) {
    unsigned int width;
    uint32_t minimum;

    if (symbols <= 1U) return value == 0U ? AVIFENC_OK
                                          : AVIFENC_INVALID_ARGUMENT;
    if (value >= symbols) return AVIFENC_INVALID_ARGUMENT;
    width = tile_ceil_log2(symbols);
    minimum = (1U << width) - symbols;
    if (value < minimum) {
        return avifenc_av1_symbol_writer_literal(writer, value, width - 1U);
    }
    value += minimum;
    if (avifenc_av1_symbol_writer_literal(
            writer, value >> 1U, width - 1U) != AVIFENC_OK) {
        return writer->status;
    }
    return avifenc_av1_symbol_writer_literal(writer, value & 1U, 1U);
}

AvifencStatus avifenc_av1_tile_write_palette_luma_colors(
    AvifencAv1TileState *state,
    const AvifencAv1LumaDecision *decision) {
    unsigned int bits = 8U;
    uint8_t index;
    AvifencStatus status = avifenc_av1_symbol_writer_literal(
        state->writer, decision->palette_colors[0], 8U);

    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_symbol_writer_literal(state->writer, 3U, 2U);
    if (status != AVIFENC_OK) return status;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors[index];
        uint32_t delta = value - decision->palette_colors[index - 1U] - 1U;
        uint32_t range;
        unsigned int range_bits;

        status = avifenc_av1_symbol_writer_literal(
            state->writer, delta, bits);
        if (status != AVIFENC_OK) return status;
        range = 255U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_tile_write_palette_chroma_colors(
    AvifencAv1TileState *state,
    const AvifencAv1ChromaDecision *decision) {
    unsigned int bits = 8U;
    uint8_t index;
    AvifencStatus status = avifenc_av1_symbol_writer_literal(
        state->writer, decision->palette_colors_u[0], 8U);

    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_symbol_writer_literal(state->writer, 3U, 2U);
    if (status != AVIFENC_OK) return status;
    for (index = 1U; index < decision->palette_size; ++index) {
        uint32_t value = decision->palette_colors_u[index];
        uint32_t delta = value - decision->palette_colors_u[index - 1U];
        uint32_t range;
        unsigned int range_bits;

        status = avifenc_av1_symbol_writer_literal(
            state->writer, delta, bits);
        if (status != AVIFENC_OK) return status;
        range = 256U - value;
        range_bits = tile_ceil_log2(range);
        if (range_bits < bits) bits = range_bits;
    }
    status = avifenc_av1_symbol_writer_literal(state->writer, 0U, 1U);
    for (index = 0U; status == AVIFENC_OK && index < decision->palette_size;
         ++index) {
        status = avifenc_av1_symbol_writer_literal(
            state->writer, decision->palette_colors_v[index], 8U);
    }
    return status;
}

AvifencStatus avifenc_av1_tile_write_palette_map(
    AvifencAv1TileState *state,
    const uint8_t *map,
    uint32_t width,
    uint32_t height,
    uint8_t palette_size,
    unsigned int plane) {
    uint32_t diagonal;
    AvifencStatus status = tile_write_ns(
        state->writer, map[0], palette_size);

    if (status != AVIFENC_OK) return status;
    for (diagonal = 1U; diagonal < width + height - 1U; ++diagonal) {
        uint32_t column = diagonal < width ? diagonal : width - 1U;
        uint32_t minimum = diagonal >= height
            ? diagonal - height + 1U : 0U;

        for (;;) {
            uint32_t token_row = diagonal - column;
            uint8_t order[8];
            unsigned int context = tile_palette_color_context(
                map, width, token_row, column, palette_size, order);
            unsigned int symbol;

            for (symbol = 0U; symbol < palette_size; ++symbol) {
                if (order[symbol] == map[(size_t)token_row * width + column]) {
                    break;
                }
            }
            status = avifenc_av1_symbol_writer_write(
                state->writer,
                state->palette_cdfs.color[plane][palette_size - 2U][context],
                palette_size, symbol);
            if (status != AVIFENC_OK) return status;
            if (column == minimum) break;
            --column;
        }
    }
    return AVIFENC_OK;
}
