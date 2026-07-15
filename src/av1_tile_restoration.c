#include "av1_tile.h"
#include "base.h"

static unsigned int av1_tile_ceil_log2(uint32_t value) {
    unsigned int bits = 0U;

    if (value <= 1U) return 0U;
    --value;
    while (value != 0U) {
        value >>= 1U;
        ++bits;
    }
    return bits;
}

static uint32_t av1_tile_read_ns(Av1SymbolDecoder *decoder, uint32_t symbols) {
    unsigned int width;
    uint32_t minimum;
    uint32_t value;

    if (symbols <= 1U) return 0U;
    width = av1_tile_ceil_log2(symbols);
    minimum = (1U << width) - symbols;
    value = av1_symbol_read_literal(decoder, width - 1U);
    if (value < minimum) return value;
    return (value << 1U) - minimum + av1_symbol_read_literal(decoder, 1U);
}

static uint32_t av1_tile_decode_subexp(Av1SymbolDecoder *decoder,
                                       uint32_t symbols,
                                       unsigned int k) {
    unsigned int index = 0U;
    uint32_t consumed = 0U;

    for (;;) {
        unsigned int bits = index == 0U ? k : k + index - 1U;
        uint32_t count = 1U << bits;
        if (symbols <= consumed + 3U * count) {
            return consumed + av1_tile_read_ns(decoder, symbols - consumed);
        }
        if (av1_symbol_read_literal(decoder, 1U) == 0U) {
            return consumed + av1_symbol_read_literal(decoder, bits);
        }
        consumed += count;
        ++index;
    }
}

static uint32_t av1_tile_inverse_recenter(uint32_t reference,
                                          uint32_t value) {
    if (value > 2U * reference) return value;
    if ((value & 1U) != 0U) return reference - ((value + 1U) >> 1U);
    return reference + (value >> 1U);
}

static int av1_tile_decode_signed_subexp(Av1SymbolDecoder *decoder,
                                         int low,
                                         int high,
                                         unsigned int k,
                                         int reference) {
    uint32_t symbols = (uint32_t)(high - low);
    uint32_t shifted_reference = (uint32_t)(reference - low);
    uint32_t value = av1_tile_decode_subexp(decoder, symbols, k);
    uint32_t result;

    if (2U * shifted_reference <= symbols) {
        result = av1_tile_inverse_recenter(shifted_reference, value);
    } else {
        result = symbols - 1U - av1_tile_inverse_recenter(
            symbols - 1U - shifted_reference, value);
    }
    return (int)result + low;
}

static uint32_t av1_restoration_count_units(uint32_t unit_size,
                                             uint32_t frame_size) {
    uint32_t count = (frame_size + (unit_size >> 1U)) / unit_size;
    return count == 0U ? 1U : count;
}

void av1_restoration_reset_tile(Av1RestorationState *state) {
    static const int8_t wiener_mid[3] = { 3, -7, 15 };
    static const int8_t sgr_mid[2] = { -32, 31 };
    unsigned int plane;

    if (state == 0) return;
    for (plane = 0U; plane < (state->monochrome ? 1U : 3U); ++plane) {
        unsigned int pass;
        for (pass = 0U; pass < 2U; ++pass) {
            avifdec_memory_copy(state->ref_wiener[plane][pass], wiener_mid,
                                sizeof(wiener_mid));
            state->ref_sgr_xqd[plane][pass] = sgr_mid[pass];
        }
    }
}

AvifdecStatus av1_restoration_state_init(Av1RestorationState *state,
                                         Av1RestorationUnit *units,
                                         size_t unit_capacity,
                                         uint32_t upscaled_width,
                                         uint32_t frame_height,
                                         uint8_t superres_denom,
                                         const uint8_t frame_type[3],
                                         const uint16_t unit_size[3],
                                         int monochrome,
                                         int subsampling_x,
                                         int subsampling_y) {
    size_t required = 0U;
    unsigned int plane;

    if (state == 0 || units == 0 || frame_type == 0 || unit_size == 0 ||
        upscaled_width == 0U || frame_height == 0U ||
        superres_denom < 8U || superres_denom > 16U ||
        (monochrome != 0 && monochrome != 1) ||
        (subsampling_x != 0 && subsampling_x != 1) ||
        (subsampling_y != 0 && subsampling_y != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    avifdec_memory_fill(state, 0U, sizeof(*state));
    state->units = units;
    state->unit_capacity = unit_capacity;
    state->upscaled_width = upscaled_width;
    state->frame_height = frame_height;
    state->superres_denom = superres_denom;
    state->monochrome = (uint8_t)monochrome;
    state->subsampling_x = (uint8_t)subsampling_x;
    state->subsampling_y = (uint8_t)subsampling_y;
    for (plane = 0U; plane < (monochrome ? 1U : 3U); ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : (unsigned int)subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : (unsigned int)subsampling_y;
        uint32_t plane_width =
            (upscaled_width + ((uint32_t)1U << sub_x) - 1U) >> sub_x;
        uint32_t plane_height =
            (frame_height + ((uint32_t)1U << sub_y) - 1U) >> sub_y;
        size_t count;

        state->frame_type[plane] = frame_type[plane];
        state->unit_size[plane] = unit_size[plane];
        if (frame_type[plane] == 0U) continue;
        if (unit_size[plane] < 32U || unit_size[plane] > 256U) {
            return AVIFDEC_INVALID_DATA;
        }
        state->unit_rows[plane] = av1_restoration_count_units(
            unit_size[plane], plane_height);
        state->unit_columns[plane] = av1_restoration_count_units(
            unit_size[plane], plane_width);
        if (!avifdec_size_multiply(state->unit_rows[plane],
                                   state->unit_columns[plane], &count) ||
            !avifdec_size_add(required, count, &required)) {
            return AVIFDEC_OVERFLOW;
        }
        state->plane_offset[plane] = required - count;
    }
    if (required > unit_capacity) return AVIFDEC_OUT_OF_MEMORY;
    avifdec_memory_fill(units, 0U, required * sizeof(*units));
    av1_restoration_reset_tile(state);
    return AVIFDEC_OK;
}

static AvifdecStatus av1_tile_read_restoration_unit(
    Av1RestorationState *state,
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    unsigned int plane,
    uint32_t unit_row,
    uint32_t unit_column) {
    static const int wiener_min[3] = { -5, -23, -17 };
    static const int wiener_max[3] = { 10, 8, 46 };
    static const unsigned int wiener_k[3] = { 1U, 2U, 3U };
    static const int sgr_min[2] = { -96, -32 };
    static const int sgr_max[2] = { 31, 95 };
    static const uint8_t sgr_radius[16][2] = {
        { 2U, 1U }, { 2U, 1U }, { 2U, 1U }, { 2U, 1U },
        { 2U, 1U }, { 2U, 1U }, { 2U, 1U }, { 2U, 1U },
        { 2U, 1U }, { 2U, 1U }, { 0U, 1U }, { 0U, 1U },
        { 0U, 1U }, { 0U, 1U }, { 2U, 0U }, { 2U, 0U }
    };
    size_t index = state->plane_offset[plane] +
                   (size_t)unit_row * state->unit_columns[plane] + unit_column;
    Av1RestorationUnit *unit;
    unsigned int pass;

    if (index >= state->unit_capacity) return AVIFDEC_LIMIT_EXCEEDED;
    unit = &state->units[index];
    if (unit->parsed) return AVIFDEC_INVALID_DATA;
    unit->parsed = 1U;
    if (state->frame_type[plane] == 1U) {
        unit->type = av1_symbol_read(decoder, cdfs->use_wiener, 2U) ? 1U : 0U;
    } else if (state->frame_type[plane] == 2U) {
        unit->type = av1_symbol_read(decoder, cdfs->use_sgrproj, 2U) ? 2U : 0U;
    } else if (state->frame_type[plane] == 3U) {
        unit->type = (uint8_t)av1_symbol_read(decoder,
                                              cdfs->restoration_type, 3U);
    } else {
        return AVIFDEC_INVALID_DATA;
    }
    if (decoder->status != AVIFDEC_OK || unit->type == 0U) {
        return decoder->status;
    }
    if (unit->type == 1U) {
        for (pass = 0U; pass < 2U; ++pass) {
            unsigned int coefficient;
            unsigned int first = plane == 0U ? 0U : 1U;
            for (coefficient = first; coefficient < 3U; ++coefficient) {
                int value = av1_tile_decode_signed_subexp(
                    decoder, wiener_min[coefficient],
                    wiener_max[coefficient] + 1, wiener_k[coefficient],
                    state->ref_wiener[plane][pass][coefficient]);
                unit->wiener[pass][coefficient] = (int8_t)value;
                state->ref_wiener[plane][pass][coefficient] = (int8_t)value;
            }
        }
    } else if (unit->type == 2U) {
        unit->sgr_set = (uint8_t)av1_symbol_read_literal(decoder, 4U);
        for (pass = 0U; pass < 2U; ++pass) {
            int value;
            if (sgr_radius[unit->sgr_set][pass] != 0U) {
                value = av1_tile_decode_signed_subexp(
                    decoder, sgr_min[pass], sgr_max[pass] + 1, 4U,
                    state->ref_sgr_xqd[plane][pass]);
            } else if (pass == 1U) {
                value = 128 - state->ref_sgr_xqd[plane][0];
                if (value < sgr_min[pass]) value = sgr_min[pass];
                if (value > sgr_max[pass]) value = sgr_max[pass];
            } else {
                value = 0;
            }
            unit->sgr_xqd[pass] = (int8_t)value;
            state->ref_sgr_xqd[plane][pass] = (int8_t)value;
        }
    } else {
        return AVIFDEC_INVALID_DATA;
    }
    return decoder->status;
}

AvifdecStatus av1_tile_read_restoration(void *user_data,
                                        Av1SymbolDecoder *decoder,
                                        Av1TileCdfs *cdfs,
                                        uint32_t row,
                                        uint32_t column,
                                        uint32_t superblock_mi) {
    Av1RestorationState *state = (Av1RestorationState *)user_data;
    unsigned int planes;
    unsigned int plane;

    if (state == 0 || decoder == 0 || cdfs == 0 ||
        (superblock_mi != 16U && superblock_mi != 32U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    planes = state->monochrome ? 1U : 3U;
    for (plane = 0U; plane < planes; ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : state->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : state->subsampling_y;
        uint32_t unit_size = state->unit_size[plane];
        uint32_t unit_row_start;
        uint32_t unit_row_end;
        uint32_t unit_column_start;
        uint32_t unit_column_end;
        uint32_t numerator;
        uint32_t denominator;
        uint32_t unit_row;
        uint32_t unit_column;

        if (state->frame_type[plane] == 0U) continue;
        unit_row_start = (row * (4U >> sub_y) + unit_size - 1U) / unit_size;
        unit_row_end = ((row + superblock_mi) * (4U >> sub_y) +
                        unit_size - 1U) / unit_size;
        if (unit_row_end > state->unit_rows[plane]) {
            unit_row_end = state->unit_rows[plane];
        }
        numerator = (4U >> sub_x) * state->superres_denom;
        denominator = unit_size * 8U;
        unit_column_start = (column * numerator + denominator - 1U) /
                            denominator;
        unit_column_end = ((column + superblock_mi) * numerator +
                           denominator - 1U) / denominator;
        if (unit_column_end > state->unit_columns[plane]) {
            unit_column_end = state->unit_columns[plane];
        }
        for (unit_row = unit_row_start; unit_row < unit_row_end; ++unit_row) {
            for (unit_column = unit_column_start;
                 unit_column < unit_column_end; ++unit_column) {
                AvifdecStatus status = av1_tile_read_restoration_unit(
                    state, decoder, cdfs, plane, unit_row, unit_column);
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    return decoder->status;
}
