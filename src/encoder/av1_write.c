#include "encoder/av1_write.h"
#include "base.h"

#define AVIFENC_OBU_SEQUENCE_HEADER 1U
#define AVIFENC_OBU_FRAME_HEADER 3U
#define AVIFENC_OBU_TILE_GROUP 4U

typedef struct {
    uint8_t level;
    uint32_t picture_size;
    uint32_t width;
    uint32_t height;
} AvifencAv1LevelLimit;

static AvifencStatus av1_write_fail(AvifencByteWriter *writer,
                                    AvifencStatus status) {
    if (writer != 0 && writer->status == AVIFENC_OK) writer->status = status;
    return writer == 0 ? AVIFENC_INVALID_ARGUMENT : writer->status;
}

static unsigned int av1_write_width(uint32_t value) {
    unsigned int width = 0U;

    do {
        ++width;
        value >>= 1U;
    } while (value != 0U);
    return width;
}

static unsigned int av1_write_tile_log2(uint32_t block_size,
                                        uint32_t target) {
    unsigned int result = 0U;

    while (((uint64_t)block_size << result) < target) ++result;
    return result;
}

AvifencStatus avifenc_av1_tile_layout(
    uint32_t width,
    uint32_t height,
    AvifencAv1TileLayout *layout) {
    uint32_t sb_cols;
    uint32_t sb_rows;
    unsigned int min_cols_log2;
    unsigned int max_cols_log2;
    unsigned int max_rows_log2;
    unsigned int min_tiles_log2;
    unsigned int cols_log2;
    unsigned int rows_log2;

    if (layout == 0 || width == 0U || height == 0U ||
        width > AVIFENC_MAX_DIMENSION || height > AVIFENC_MAX_DIMENSION) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    sb_cols = (width + 63U) >> 6U;
    sb_rows = (height + 63U) >> 6U;
    min_cols_log2 = av1_write_tile_log2(64U, sb_cols);
    max_cols_log2 = av1_write_tile_log2(
        1U, sb_cols < 64U ? sb_cols : 64U);
    max_rows_log2 = av1_write_tile_log2(
        1U, sb_rows < 64U ? sb_rows : 64U);
    min_tiles_log2 = av1_write_tile_log2(2304U, sb_cols * sb_rows);
    if (min_tiles_log2 < min_cols_log2) {
        min_tiles_log2 = min_cols_log2;
    }
    cols_log2 = min_cols_log2;
    rows_log2 = min_tiles_log2 > cols_log2
        ? min_tiles_log2 - cols_log2 : 0U;
    if (cols_log2 > max_cols_log2 || rows_log2 > max_rows_log2) {
        return AVIFENC_LIMIT_EXCEEDED;
    }
    layout->tile_width_sb = (uint16_t)(
        (sb_cols + (1U << cols_log2) - 1U) >> cols_log2);
    layout->tile_height_sb = (uint16_t)(
        (sb_rows + (1U << rows_log2) - 1U) >> rows_log2);
    layout->columns = (uint16_t)(
        (sb_cols + layout->tile_width_sb - 1U) / layout->tile_width_sb);
    layout->rows = (uint16_t)(
        (sb_rows + layout->tile_height_sb - 1U) / layout->tile_height_sb);
    layout->columns_log2 = (uint8_t)cols_log2;
    layout->rows_log2 = (uint8_t)rows_log2;
    layout->tile_size_bytes = 1U;
    return AVIFENC_OK;
}

AvifencStatus avifenc_av1_select_level(uint32_t width,
                                       uint32_t height,
                                       uint8_t *level) {
    static const AvifencAv1LevelLimit limits[] = {
        { 0U, 147456U, 2048U, 1152U },
        { 1U, 278784U, 2816U, 1584U },
        { 4U, 665856U, 4352U, 2448U },
        { 5U, 1065024U, 5504U, 3096U },
        { 8U, 2359296U, 6144U, 3456U },
        { 12U, 8912896U, 8192U, 4352U },
        { 16U, 35651584U, 16384U, 8704U }
    };
    uint64_t picture_size;
    size_t index;

    if (level == 0 || width == 0U || height == 0U ||
        width > AVIFENC_MAX_DIMENSION || height > AVIFENC_MAX_DIMENSION) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    picture_size = (uint64_t)width * height;
    for (index = 0U; index < sizeof(limits) / sizeof(limits[0]); ++index) {
        if (width <= limits[index].width && height <= limits[index].height &&
            picture_size <= limits[index].picture_size) {
            *level = limits[index].level;
            return AVIFENC_OK;
        }
    }
    *level = 31U;
    return AVIFENC_OK;
}

static AvifencStatus av1_write_trailing_bits(AvifencBitWriter *writer) {
    AvifencStatus status = avifenc_bit_writer_write(writer, 1U, 1U);

    if (status != AVIFENC_OK) return status;
    return avifenc_bit_writer_align(writer);
}

static AvifencStatus av1_write_sequence_header(
    AvifencBitWriter *writer,
    const AvifencAv1Config *config,
    uint8_t level) {
    unsigned int width_bits = av1_write_width(config->width - 1U);
    unsigned int height_bits = av1_write_width(config->height - 1U);

    /* seq_profile, still_picture, reduced_still_picture_header. */
    (void)avifenc_bit_writer_write(writer, 0U, 3U);
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(writer, level, 5U);
    (void)avifenc_bit_writer_write(writer, width_bits - 1U, 4U);
    (void)avifenc_bit_writer_write(writer, height_bits - 1U, 4U);
    (void)avifenc_bit_writer_write(
        writer, config->width - 1U, width_bits);
    (void)avifenc_bit_writer_write(
        writer, config->height - 1U, height_bits);

    /* use_128x128_superblock and the reduced-still sequence tool flags. */
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);

    /* color_config: 8-bit, non-monochrome, explicit 4:2:0 NCLX values. */
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(
        writer, config->color.color_primaries, 8U);
    (void)avifenc_bit_writer_write(
        writer, config->color.transfer_characteristics, 8U);
    (void)avifenc_bit_writer_write(
        writer, config->color.matrix_coefficients, 8U);
    (void)avifenc_bit_writer_write(writer, config->color.full_range, 1U);
    (void)avifenc_bit_writer_write(
        writer, config->color.chroma_sample_position, 2U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);

    /* film_grain_params_present. */
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    return av1_write_trailing_bits(writer);
}

static AvifencStatus av1_write_frame_header(AvifencBitWriter *writer,
                                            const AvifencAv1Config *config,
                                            const AvifencAv1TileLayout *layout) {
    uint32_t sb_cols = (config->width + 63U) >> 6U;
    uint32_t sb_rows = (config->height + 63U) >> 6U;
    unsigned int min_cols_log2 = av1_write_tile_log2(64U, sb_cols);
    unsigned int max_cols_log2 = av1_write_tile_log2(
        1U, sb_cols < 64U ? sb_cols : 64U);
    unsigned int max_rows_log2 = av1_write_tile_log2(
        1U, sb_rows < 64U ? sb_rows : 64U);
    unsigned int min_tiles_log2 = av1_write_tile_log2(
        2304U, sb_cols * sb_rows);
    unsigned int index;

    /* disable_cdf_update and per-frame screen-content tool selection. */
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);

    /* Uniform tile spacing followed by column and row stop bits. */
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    for (index = min_cols_log2; index < layout->columns_log2; ++index) {
        (void)avifenc_bit_writer_write(writer, 1U, 1U);
    }
    if (layout->columns_log2 < max_cols_log2) {
        (void)avifenc_bit_writer_write(writer, 0U, 1U);
    }
    if (min_tiles_log2 < min_cols_log2) min_tiles_log2 = min_cols_log2;
    index = min_tiles_log2 > layout->columns_log2
        ? min_tiles_log2 - layout->columns_log2 : 0U;
    for (; index < layout->rows_log2; ++index) {
        (void)avifenc_bit_writer_write(writer, 1U, 1U);
    }
    if (layout->rows_log2 < max_rows_log2) {
        (void)avifenc_bit_writer_write(writer, 0U, 1U);
    }
    if (layout->columns_log2 != 0U || layout->rows_log2 != 0U) {
        (void)avifenc_bit_writer_write(
            writer, 0U, layout->columns_log2 + layout->rows_log2);
        (void)avifenc_bit_writer_write(
            writer, layout->tile_size_bytes - 1U, 2U);
    }

    /* quantization_params: one base index, zero deltas, no qmatrix. */
    (void)avifenc_bit_writer_write(writer, config->quantizer, 8U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);

    /* segmentation_enabled and delta_q_present. */
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    if (config->quantizer != 0U) {
        (void)avifenc_bit_writer_write(writer, 0U, 1U);

        /* loop_filter_params: identity levels, sharpness, and deltas. */
        (void)avifenc_bit_writer_write(writer, 0U, 6U);
        (void)avifenc_bit_writer_write(writer, 0U, 6U);
        (void)avifenc_bit_writer_write(writer, 0U, 3U);
        (void)avifenc_bit_writer_write(writer, 0U, 1U);

        /* tx_mode_select: fixed largest transform size. */
        (void)avifenc_bit_writer_write(writer, 0U, 1U);
    }
    /* reduced_tx_set. */
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    return av1_write_trailing_bits(writer);
}

static AvifencStatus av1_write_obu(AvifencByteWriter *writer,
                                   uint8_t type,
                                   const uint8_t *payload,
                                   size_t payload_size) {
    AvifencStatus status = avifenc_byte_writer_u8(
        writer, (uint8_t)((type << 3U) | 2U));

    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_leb128(writer, payload_size);
    if (status != AVIFENC_OK) return status;
    return avifenc_byte_writer_write(writer, payload, payload_size);
}

AvifencStatus avifenc_av1_write(AvifencByteWriter *writer,
                                const AvifencAv1Config *config) {
    static const uint8_t tile_stub[1] = { 0U };

    return avifenc_av1_write_with_tile(
        writer, config, tile_stub, sizeof(tile_stub));
}

AvifencStatus avifenc_av1_write_with_tile(
    AvifencByteWriter *writer,
    const AvifencAv1Config *config,
    const uint8_t *tile_payload,
    size_t tile_payload_size) {
    AvifencAv1TileLayout layout;
    AvifencAv1TilePayload payload;
    AvifencStatus status;

    status = avifenc_av1_tile_layout(
        config != 0 ? config->width : 0U,
        config != 0 ? config->height : 0U, &layout);
    if (status != AVIFENC_OK) return av1_write_fail(writer, status);
    if ((size_t)layout.columns * layout.rows != 1U) {
        return av1_write_fail(writer, AVIFENC_UNSUPPORTED);
    }
    payload.data = tile_payload;
    payload.size = tile_payload_size;
    return avifenc_av1_write_with_tiles(
        writer, config, &layout, &payload, 1U);
}

AvifencStatus avifenc_av1_write_with_tiles(
    AvifencByteWriter *writer,
    const AvifencAv1Config *config,
    const AvifencAv1TileLayout *layout,
    const AvifencAv1TilePayload *tile_payloads,
    size_t tile_count) {
    uint8_t sequence_payload[32];
    uint8_t frame_payload[32];
    AvifencBitWriter bits;
    AvifencStatus status;
    AvifencAv1TileLayout expected_layout;
    uint8_t level;
    size_t tile;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (config == 0 || layout == 0 || tile_payloads == 0 ||
        tile_count == 0U ||
        config->width == 0U || config->height == 0U ||
        (config->width & 1U) != 0U || (config->height & 1U) != 0U ||
        config->width > AVIFENC_MAX_DIMENSION ||
        config->height > AVIFENC_MAX_DIMENSION ||
        config->quantizer > 255U || config->color.color_primaries > 255U ||
        config->color.transfer_characteristics > 255U ||
        config->color.matrix_coefficients > 255U ||
        config->color.full_range > 1U ||
        config->color.chroma_sample_position > 3U ||
        (config->color.color_primaries == 1U &&
         config->color.transfer_characteristics == 13U &&
         config->color.matrix_coefficients == 0U)) {
        return av1_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    if (tile_count != (size_t)layout->columns * layout->rows ||
        layout->tile_size_bytes == 0U || layout->tile_size_bytes > 4U) {
        return av1_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    status = avifenc_av1_tile_layout(
        config->width, config->height, &expected_layout);
    if (status != AVIFENC_OK) return av1_write_fail(writer, status);
    if (layout->columns != expected_layout.columns ||
        layout->rows != expected_layout.rows ||
        layout->tile_width_sb != expected_layout.tile_width_sb ||
        layout->tile_height_sb != expected_layout.tile_height_sb ||
        layout->columns_log2 != expected_layout.columns_log2 ||
        layout->rows_log2 != expected_layout.rows_log2) {
        return av1_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    for (tile = 0U; tile < tile_count; ++tile) {
        if (tile_payloads[tile].data == 0 || tile_payloads[tile].size == 0U) {
            return av1_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
        }
    }
    status = avifenc_av1_select_level(config->width, config->height, &level);
    if (status != AVIFENC_OK) return av1_write_fail(writer, status);

    avifenc_bit_writer_init(&bits, sequence_payload, sizeof(sequence_payload));
    status = av1_write_sequence_header(&bits, config, level);
    if (status != AVIFENC_OK) return av1_write_fail(writer, status);
    status = av1_write_obu(
        writer, AVIFENC_OBU_SEQUENCE_HEADER, sequence_payload,
        avifenc_bit_writer_bytes(&bits));
    if (status != AVIFENC_OK) return status;

    avifenc_bit_writer_init(&bits, frame_payload, sizeof(frame_payload));
    status = av1_write_frame_header(&bits, config, layout);
    if (status != AVIFENC_OK) return av1_write_fail(writer, status);
    status = av1_write_obu(
        writer, AVIFENC_OBU_FRAME_HEADER, frame_payload,
        avifenc_bit_writer_bytes(&bits));
    if (status != AVIFENC_OK) return status;
    if (tile_count == 1U) {
        return av1_write_obu(
            writer, AVIFENC_OBU_TILE_GROUP,
            tile_payloads[0].data, tile_payloads[0].size);
    }
    {
        size_t group_size = 1U;

        for (tile = 0U; tile < tile_count; ++tile) {
            if (tile + 1U < tile_count &&
                layout->tile_size_bytes < sizeof(size_t) &&
                tile_payloads[tile].size - 1U >=
                    ((size_t)1U << (layout->tile_size_bytes * 8U))) {
                return av1_write_fail(writer, AVIFENC_LIMIT_EXCEEDED);
            }
            if (tile + 1U < tile_count &&
                !avifdec_size_add(
                    group_size, layout->tile_size_bytes, &group_size)) {
                return av1_write_fail(writer, AVIFENC_OVERFLOW);
            }
            if (!avifdec_size_add(
                    group_size, tile_payloads[tile].size, &group_size)) {
                return av1_write_fail(writer, AVIFENC_OVERFLOW);
            }
        }
        status = avifenc_byte_writer_u8(
            writer, (uint8_t)((AVIFENC_OBU_TILE_GROUP << 3U) | 2U));
        if (status == AVIFENC_OK) {
            status = avifenc_byte_writer_leb128(writer, group_size);
        }
        if (status == AVIFENC_OK) status = avifenc_byte_writer_u8(writer, 0U);
        for (tile = 0U; status == AVIFENC_OK && tile < tile_count; ++tile) {
            if (tile + 1U < tile_count) {
                size_t encoded_size = tile_payloads[tile].size - 1U;
                unsigned int byte;

                for (byte = 0U; byte < layout->tile_size_bytes; ++byte) {
                    status = avifenc_byte_writer_u8(
                        writer, (uint8_t)(encoded_size >> (byte * 8U)));
                    if (status != AVIFENC_OK) break;
                }
            }
            if (status == AVIFENC_OK) {
                status = avifenc_byte_writer_write(
                    writer, tile_payloads[tile].data,
                    tile_payloads[tile].size);
            }
        }
        return status;
    }
}