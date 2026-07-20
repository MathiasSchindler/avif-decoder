#include "av1_tile_internal.h"
#include "base.h"

static unsigned int av1_tile_log2_mi(uint32_t value) {
    unsigned int result = 0U;

    while (value > 1U) {
        value >>= 1;
        ++result;
    }
    return result;
}

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

static void av1_tile_sort_colors(uint16_t colors[8], uint8_t count) {
    unsigned int index;

    for (index = 1U; index < count; ++index) {
        uint16_t value = colors[index];
        unsigned int position = index;

        while (position > 0U && colors[position - 1U] > value) {
            colors[position] = colors[position - 1U];
            --position;
        }
        colors[position] = value;
    }
}

static size_t av1_tile_palette_cache(const Av1BlockState *state,
                                     const Av1TilePaletteContext *color_context,
                                     const Av1BlockAvailability *availability,
                                     const Av1BlockTraceFields *fields,
                                     unsigned int plane,
                                     uint16_t cache[16]) {
    const Av1BlockCell *above = 0;
    const Av1BlockCell *left = 0;
    const uint16_t *above_colors = 0;
    const uint16_t *left_colors = 0;
    uint8_t above_count = 0U;
    uint8_t left_count = 0U;
    size_t above_index = 0U;
    size_t left_index = 0U;
    size_t count = 0U;

    if (availability->above && ((fields->row * 4U) & 63U) != 0U) {
        above = av1_block_cell(state, fields->row - 1U, fields->column);
    }
    if (availability->left) {
        left = av1_block_cell(state, fields->row, fields->column - 1U);
    }
    /* Colors themselves come from the tile-local rolling context, not the
       cell grid; `above`/`left` are only consulted for palette_size_*,
       which is still tracked per mi cell. */
    if (above != 0) {
        size_t index = fields->column - color_context->tile_column_start;

        above_count = plane == 0U ? above->palette_size_y : above->palette_size_uv;
        if (index < AV1_TILE_PALETTE_ABOVE_MI) {
            above_colors = plane == 0U ? color_context->above[index].y
                                       : color_context->above[index].u;
        } else {
            above_count = 0U;
        }
    }
    if (left != 0) {
        size_t index = fields->row - color_context->row_band_start;

        left_count = plane == 0U ? left->palette_size_y : left->palette_size_uv;
        if (index < AV1_TILE_PALETTE_LEFT_MI) {
            left_colors = plane == 0U ? color_context->left[index].y
                                      : color_context->left[index].u;
        } else {
            left_count = 0U;
        }
    }
    while (above_index < above_count || left_index < left_count) {
        uint16_t value;

        if (left_index < left_count &&
            (above_index >= above_count ||
             left_colors[left_index] < above_colors[above_index])) {
            value = left_colors[left_index++];
        } else {
            value = above_colors[above_index++];
            if (left_index < left_count && left_colors[left_index] == value) {
                ++left_index;
            }
        }
        if (count == 0U || cache[count - 1U] != value) cache[count++] = value;
    }
    return count;
}

static AvifdecStatus av1_tile_read_palette_plane_colors(
    Av1SymbolDecoder *decoder,
    const Av1TileModeConfig *config,
    const Av1TilePaletteContext *color_context,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    unsigned int plane,
    uint8_t palette_size,
    uint16_t colors[8]) {
    uint16_t cache[16];
    size_t cache_count = av1_tile_palette_cache(
        config->block_state, color_context, availability, fields, plane,
        cache);
    uint32_t maximum = (1U << config->bit_depth) - 1U;
    unsigned int index = 0U;
    size_t cache_index;
    unsigned int bits = 0U;

    for (cache_index = 0U; cache_index < cache_count && index < palette_size;
         ++cache_index) {
        if (av1_symbol_read_literal(decoder, 1U) != 0U) {
            colors[index++] = cache[cache_index];
        }
    }
    if (index < palette_size) {
        colors[index++] = (uint16_t)av1_symbol_read_literal(decoder,
                                                            config->bit_depth);
    }
    if (index < palette_size) {
        bits = config->bit_depth - 3U + av1_symbol_read_literal(decoder, 2U);
    }
    while (index < palette_size) {
        uint32_t delta = av1_symbol_read_literal(decoder, bits);
        uint32_t value;
        uint32_t range;

        if (plane == 0U) ++delta;
        value = colors[index - 1U] + delta;
        if (value > maximum) value = maximum;
        colors[index++] = (uint16_t)value;
        range = maximum - value + (plane == 0U ? 0U : 1U);
        {
            unsigned int range_bits = av1_tile_ceil_log2(range);
            if (range_bits < bits) bits = range_bits;
        }
    }
    av1_tile_sort_colors(colors, palette_size);
    return decoder->status;
}

AvifdecStatus av1_tile_palette_context_init(Av1TilePaletteContext *context,
                                            uint32_t tile_row_start,
                                            uint32_t tile_column_start,
                                            uint32_t tile_column_end) {
    if (context == 0 || tile_column_end <= tile_column_start ||
        tile_column_end - tile_column_start > AV1_TILE_PALETTE_ABOVE_MI) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    avifdec_memory_fill(context, 0U, sizeof(*context));
    context->tile_column_start = tile_column_start;
    context->row_band_start = tile_row_start;
    return AVIFDEC_OK;
}

AvifdecStatus av1_tile_palette_context_new_row_band(
    Av1TilePaletteContext *context,
    uint32_t row_band_start) {
    if (context == 0) return AVIFDEC_INVALID_ARGUMENT;
    /* Only "left" is band-local; "above" must survive across row bands
       for the whole tile, so it is left untouched here. */
    avifdec_memory_fill(context->left, 0U, sizeof(context->left));
    context->row_band_start = row_band_start;
    return AVIFDEC_OK;
}

/* Publish this block's own palette colors into the rolling context so
   later blocks below/right of it see them as above/left neighbors. Only
   entries actually covered by the block are updated (clipped to the
   fixed capacities, which tile width/superblock size already guarantee
   are never exceeded); non-palette blocks need not clear anything since
   readers gate on the always up to date palette_size_* in the cell grid. */
static void av1_tile_palette_context_store(
    Av1TilePaletteContext *color_context,
    const Av1BlockTraceFields *fields) {
    uint32_t above_start;
    uint32_t above_end;
    uint32_t left_start;
    uint32_t left_end;
    uint32_t index;

    if (fields->palette_size_y == 0U && fields->palette_size_uv == 0U) return;
    above_start = fields->column - color_context->tile_column_start;
    above_end = above_start + fields->width;
    if (above_end > AV1_TILE_PALETTE_ABOVE_MI) above_end = AV1_TILE_PALETTE_ABOVE_MI;
    left_start = fields->row - color_context->row_band_start;
    left_end = left_start + fields->height;
    if (left_end > AV1_TILE_PALETTE_LEFT_MI) left_end = AV1_TILE_PALETTE_LEFT_MI;
    for (index = above_start; index < above_end; ++index) {
        if (fields->palette_size_y != 0U) {
            avifdec_memory_copy(color_context->above[index].y,
                                fields->palette_colors_y,
                                sizeof(color_context->above[index].y));
        }
        if (fields->palette_size_uv != 0U) {
            avifdec_memory_copy(color_context->above[index].u,
                                fields->palette_colors_u,
                                sizeof(color_context->above[index].u));
        }
    }
    for (index = left_start; index < left_end; ++index) {
        if (fields->palette_size_y != 0U) {
            avifdec_memory_copy(color_context->left[index].y,
                                fields->palette_colors_y,
                                sizeof(color_context->left[index].y));
        }
        if (fields->palette_size_uv != 0U) {
            avifdec_memory_copy(color_context->left[index].u,
                                fields->palette_colors_u,
                                sizeof(color_context->left[index].u));
        }
    }
}

AvifdecStatus av1_tile_read_palette_info(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    uint8_t block_size,
    Av1TilePaletteContext *color_context) {
    unsigned int block_context;
    unsigned int palette_context = 0U;

    if (block_size < 3U || fields->width > 16U || fields->height > 16U) {
        return AVIFDEC_OK;
    }
    block_context = av1_tile_log2_mi(fields->width) +
                    av1_tile_log2_mi(fields->height) - 2U;
    if (block_context >= 7U) return AVIFDEC_INVALID_DATA;
    if (fields->y_mode == 0U) {
        if (availability->above) {
            const Av1BlockCell *above = av1_block_cell(
                config->block_state, fields->row - 1U, fields->column);
            if (above == 0) return AVIFDEC_LIMIT_EXCEEDED;
            if (above->palette_size_y != 0U) ++palette_context;
        }
        if (availability->left) {
            const Av1BlockCell *left = av1_block_cell(
                config->block_state, fields->row, fields->column - 1U);
            if (left == 0) return AVIFDEC_LIMIT_EXCEEDED;
            if (left->palette_size_y != 0U) ++palette_context;
        }
        if (av1_symbol_read(decoder,
                cdfs->palette_y_mode[block_context][palette_context], 2U) != 0U) {
            fields->palette_size_y = (uint8_t)(av1_symbol_read(
                decoder, cdfs->palette_y_size[block_context], 7U) + 2U);
            if (decoder->status != AVIFDEC_OK) return decoder->status;
            if (av1_tile_read_palette_plane_colors(
                    decoder, config, color_context, availability, fields, 0U,
                    fields->palette_size_y, fields->palette_colors_y) != AVIFDEC_OK) {
                return decoder->status;
            }
        }
    }
    if (availability->has_chroma && fields->uv_mode == 0U &&
        av1_symbol_read(decoder,
            cdfs->palette_uv_mode[fields->palette_size_y != 0U], 2U) != 0U) {
        unsigned int index;

        fields->palette_size_uv = (uint8_t)(av1_symbol_read(
            decoder, cdfs->palette_uv_size[block_context], 7U) + 2U);
        if (decoder->status != AVIFDEC_OK) return decoder->status;
        if (av1_tile_read_palette_plane_colors(
                decoder, config, color_context, availability, fields, 1U,
                fields->palette_size_uv, fields->palette_colors_u) != AVIFDEC_OK) {
            return decoder->status;
        }
        if (av1_symbol_read_literal(decoder, 1U) != 0U) {
            unsigned int bits = config->bit_depth - 4U +
                                av1_symbol_read_literal(decoder, 2U);
            int maximum = 1 << config->bit_depth;

            fields->palette_colors_v[0] = (uint16_t)av1_symbol_read_literal(
                decoder, config->bit_depth);
            for (index = 1U; index < fields->palette_size_uv; ++index) {
                int delta = (int)av1_symbol_read_literal(decoder, bits);
                int value;

                if (delta != 0 && av1_symbol_read_literal(decoder, 1U)) {
                    delta = -delta;
                }
                value = fields->palette_colors_v[index - 1U] + delta;
                if (value < 0) value += maximum;
                if (value >= maximum) value -= maximum;
                fields->palette_colors_v[index] = (uint16_t)value;
            }
        } else {
            for (index = 0U; index < fields->palette_size_uv; ++index) {
                fields->palette_colors_v[index] = (uint16_t)av1_symbol_read_literal(
                    decoder, config->bit_depth);
            }
        }
    }
    av1_tile_palette_context_store(color_context, fields);
    return decoder->status;
}

static unsigned int av1_tile_palette_color_context(const uint8_t *map,
                                                    uint32_t stride,
                                                    uint32_t row,
                                                    uint32_t column,
                                                    uint8_t colors,
                                                    uint8_t order[8]) {
    static const int8_t contexts[9] = { -1, -1, 0, -1, -1, 4, 3, 2, 1 };
    uint8_t scores[8] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    unsigned int index;
    unsigned int hash;

    for (index = 0U; index < colors; ++index) order[index] = (uint8_t)index;
    if (column > 0U) scores[map[(size_t)row * stride + column - 1U]] += 2U;
    if (row > 0U && column > 0U) {
        ++scores[map[(size_t)(row - 1U) * stride + column - 1U]];
    }
    if (row > 0U) scores[map[(size_t)(row - 1U) * stride + column]] += 2U;
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

static AvifdecStatus av1_tile_read_palette_tokens_plane(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    uint8_t *map,
    uint32_t block_width,
    uint32_t block_height,
    uint32_t onscreen_width,
    uint32_t onscreen_height,
    uint8_t palette_size,
    unsigned int plane) {
    uint32_t diagonal;
    uint32_t row;

    map[0] = (uint8_t)av1_tile_read_ns(decoder, palette_size);
    for (diagonal = 1U;
         diagonal < onscreen_width + onscreen_height - 1U;
         ++diagonal) {
        uint32_t column = diagonal < onscreen_width
                          ? diagonal : onscreen_width - 1U;
        uint32_t minimum = diagonal >= onscreen_height
                           ? diagonal - onscreen_height + 1U : 0U;

        for (;;) {
            uint32_t token_row = diagonal - column;
            uint8_t order[8];
            unsigned int context = av1_tile_palette_color_context(
                map, block_width, token_row, column, palette_size, order);
            uint32_t symbol = av1_symbol_read(
                decoder, cdfs->palette_color[plane][palette_size - 2U][context],
                palette_size);

            if (decoder->status != AVIFDEC_OK) return decoder->status;
            map[(size_t)token_row * block_width + column] = order[symbol];
            if (column == minimum) break;
            --column;
        }
    }
    for (row = 0U; row < onscreen_height; ++row) {
        uint32_t column;
        for (column = onscreen_width; column < block_width; ++column) {
            map[(size_t)row * block_width + column] =
                map[(size_t)row * block_width + onscreen_width - 1U];
        }
    }
    for (row = onscreen_height; row < block_height; ++row) {
        uint32_t column;
        for (column = 0U; column < block_width; ++column) {
            map[(size_t)row * block_width + column] =
                map[(size_t)(onscreen_height - 1U) * block_width + column];
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_tile_read_palette_tokens(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields) {
    uint32_t block_width = fields->width * 4U;
    uint32_t block_height = fields->height * 4U;
    uint32_t onscreen_width = (config->block_state->mi_columns - fields->column) * 4U;
    uint32_t onscreen_height = (config->block_state->mi_rows - fields->row) * 4U;
    uint8_t *map = config->palette_map;
    size_t samples;

    if (fields->palette_size_y == 0U && fields->palette_size_uv == 0U) {
        return AVIFDEC_OK;
    }
    if (onscreen_width > block_width) onscreen_width = block_width;
    if (onscreen_height > block_height) onscreen_height = block_height;
    if (config->palette_map == 0 ||
        !avifdec_size_multiply(block_width, block_height, &samples) ||
        samples > config->palette_map_capacity) {
        return AVIFDEC_LIMIT_EXCEEDED;
    }
    if (fields->palette_size_y != 0U) {
        AvifdecStatus status = av1_tile_read_palette_tokens_plane(
            decoder, cdfs, map, block_width, block_height,
            onscreen_width, onscreen_height,
            fields->palette_size_y, 0U);
        if (status != AVIFDEC_OK) return status;
    }
    if (fields->palette_size_uv != 0U) {
        if (config->palette_map_uv != 0) {
            if (config->palette_map_uv_capacity < samples) {
                return AVIFDEC_LIMIT_EXCEEDED;
            }
            map = config->palette_map_uv;
        }
        block_width >>= config->block_state->subsampling_x;
        block_height >>= config->block_state->subsampling_y;
        onscreen_width >>= config->block_state->subsampling_x;
        onscreen_height >>= config->block_state->subsampling_y;
        if (block_width < 4U) {
            block_width += 2U;
            onscreen_width += 2U;
        }
        if (block_height < 4U) {
            block_height += 2U;
            onscreen_height += 2U;
        }
        return av1_tile_read_palette_tokens_plane(
            decoder, cdfs, map, block_width, block_height,
            onscreen_width, onscreen_height,
            fields->palette_size_uv, 1U);
    }
    return AVIFDEC_OK;
}
