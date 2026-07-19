#include "encoder/av1_write.h"

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
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
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
                                            const AvifencAv1Config *config) {
    uint32_t sb_cols = (config->width + 63U) >> 6U;
    uint32_t sb_rows = (config->height + 63U) >> 6U;

    /* disable_cdf_update, allow_screen_content_tools, render_size_different. */
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);
    (void)avifenc_bit_writer_write(writer, 0U, 1U);

    /* uniform_tile_spacing_flag followed by stop bits for one tile. */
    (void)avifenc_bit_writer_write(writer, 1U, 1U);
    if (sb_cols > 1U) (void)avifenc_bit_writer_write(writer, 0U, 1U);
    if (sb_rows > 1U) (void)avifenc_bit_writer_write(writer, 0U, 1U);

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
    uint8_t sequence_payload[32];
    uint8_t frame_payload[32];
    static const uint8_t tile_stub[1] = { 0U };
    AvifencBitWriter bits;
    AvifencStatus status;
    uint8_t level;
    uint32_t sb_cols;
    uint32_t sb_rows;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (config == 0 || config->width == 0U || config->height == 0U ||
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
    sb_cols = (config->width + 63U) >> 6U;
    sb_rows = (config->height + 63U) >> 6U;
    if (sb_cols > 64U || (uint64_t)sb_cols * sb_rows > 2304U) {
        return av1_write_fail(writer, AVIFENC_UNSUPPORTED);
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
    status = av1_write_frame_header(&bits, config);
    if (status != AVIFENC_OK) return av1_write_fail(writer, status);
    status = av1_write_obu(
        writer, AVIFENC_OBU_FRAME_HEADER, frame_payload,
        avifenc_bit_writer_bytes(&bits));
    if (status != AVIFENC_OK) return status;
    return av1_write_obu(
        writer, AVIFENC_OBU_TILE_GROUP, tile_stub, sizeof(tile_stub));
}