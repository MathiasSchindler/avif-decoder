#include "encoder/avif_write.h"
#include "bmff.h"

#define AVIFENC_PRIMARY_ITEM_ID 1U
#define AVIFENC_PROPERTY_ISPE 1U
#define AVIFENC_PROPERTY_PIXI 2U
#define AVIFENC_PROPERTY_AV1C 3U
#define AVIFENC_PROPERTY_COLR 4U

static AvifencStatus avif_write_fail(AvifencByteWriter *writer,
                                     AvifencStatus status) {
    if (writer != 0 && writer->status == AVIFENC_OK) writer->status = status;
    return writer == 0 ? AVIFENC_INVALID_ARGUMENT : writer->status;
}

static AvifencStatus avif_write_box_begin(AvifencByteWriter *writer,
                                          uint32_t type,
                                          size_t *start) {
    size_t size_offset;
    AvifencStatus status;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (start == 0) return avif_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    *start = avifenc_byte_writer_size(writer);
    status = avifenc_byte_writer_reserve(writer, 4U, &size_offset);
    if (status != AVIFENC_OK) return status;
    if (size_offset != *start) {
        return avif_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    return avifenc_byte_writer_u32be(writer, type);
}

static AvifencStatus avif_write_box_end(AvifencByteWriter *writer,
                                        size_t start) {
    size_t end;
    size_t size;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    end = avifenc_byte_writer_size(writer);
    if (end < start) return avif_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    size = end - start;
    if (size > UINT32_MAX) {
        return avif_write_fail(writer, AVIFENC_LIMIT_EXCEEDED);
    }
    return avifenc_byte_writer_patch_u32be(writer, start, (uint32_t)size);
}

static AvifencStatus avif_write_full_box(AvifencByteWriter *writer,
                                         uint8_t version,
                                         uint32_t flags) {
    if (flags > 0x00ffffffU) {
        return avif_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    return avifenc_byte_writer_u32be(
        writer, ((uint32_t)version << 24U) | flags);
}

static AvifencStatus avif_write_ftyp(AvifencByteWriter *writer) {
    size_t box;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('f', 't', 'y', 'p'), &box);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(
        writer, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(writer, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(
        writer, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(
        writer, AVIFDEC_FOURCC('m', 'i', 'f', '1'));
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(
        writer, AVIFDEC_FOURCC('m', 'i', 'a', 'f'));
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_handler(AvifencByteWriter *writer) {
    size_t box;
    AvifencStatus status;
    unsigned int index;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('h', 'd', 'l', 'r'), &box);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(writer, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(
        writer, AVIFDEC_FOURCC('p', 'i', 'c', 't'));
    if (status != AVIFENC_OK) return status;
    for (index = 0U; index < 3U; ++index) {
        status = avifenc_byte_writer_u32be(writer, 0U);
        if (status != AVIFENC_OK) return status;
    }
    status = avifenc_byte_writer_u8(writer, 0U);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_primary_item(AvifencByteWriter *writer) {
    size_t box;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('p', 'i', 't', 'm'), &box);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, AVIFENC_PRIMARY_ITEM_ID);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_location(AvifencByteWriter *writer,
                                         uint32_t payload_size,
                                         size_t *extent_offset) {
    size_t box;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('i', 'l', 'o', 'c'), &box);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, 0x44U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, 1U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, AVIFENC_PRIMARY_ITEM_ID);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, 1U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_reserve(writer, 4U, extent_offset);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(writer, payload_size);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_item_info(AvifencByteWriter *writer) {
    size_t iinf;
    size_t infe;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('i', 'i', 'n', 'f'), &iinf);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, 1U);
    if (status != AVIFENC_OK) return status;
    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('i', 'n', 'f', 'e'), &infe);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 2U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, AVIFENC_PRIMARY_ITEM_ID);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(
        writer, AVIFDEC_FOURCC('a', 'v', '0', '1'));
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, 0U);
    if (status != AVIFENC_OK) return status;
    status = avif_write_box_end(writer, infe);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, iinf);
}

static AvifencStatus avif_write_ispe(AvifencByteWriter *writer,
                                     const AvifencAvifConfig *config) {
    size_t box;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('i', 's', 'p', 'e'), &box);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(writer, config->width);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(writer, config->height);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_pixi(AvifencByteWriter *writer) {
    size_t box;
    AvifencStatus status;
    unsigned int channel;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('p', 'i', 'x', 'i'), &box);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, 3U);
    if (status != AVIFENC_OK) return status;
    for (channel = 0U; channel < 3U; ++channel) {
        status = avifenc_byte_writer_u8(writer, 8U);
        if (status != AVIFENC_OK) return status;
    }
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_av1c(AvifencByteWriter *writer,
                                     const AvifencAvifConfig *config) {
    size_t box;
    AvifencStatus status;
    uint8_t format = (uint8_t)(
        0x0cU | config->color.chroma_sample_position);

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('a', 'v', '1', 'C'), &box);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, 0x81U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, config->seq_level_idx_0);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, format);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, 0U);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_colr(AvifencByteWriter *writer,
                                     const AvifencAvifConfig *config) {
    size_t box;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('c', 'o', 'l', 'r'), &box);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(
        writer, AVIFDEC_FOURCC('n', 'c', 'l', 'x'));
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(
        writer, config->color.color_primaries);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(
        writer, config->color.transfer_characteristics);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(
        writer, config->color.matrix_coefficients);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(
        writer, (uint8_t)(config->color.full_range << 7U));
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, box);
}

static AvifencStatus avif_write_properties(AvifencByteWriter *writer,
                                           const AvifencAvifConfig *config) {
    size_t iprp;
    size_t ipco;
    size_t ipma;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('i', 'p', 'r', 'p'), &iprp);
    if (status != AVIFENC_OK) return status;
    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('i', 'p', 'c', 'o'), &ipco);
    if (status != AVIFENC_OK) return status;
    status = avif_write_ispe(writer, config);
    if (status != AVIFENC_OK) return status;
    status = avif_write_pixi(writer);
    if (status != AVIFENC_OK) return status;
    status = avif_write_av1c(writer, config);
    if (status != AVIFENC_OK) return status;
    status = avif_write_colr(writer, config);
    if (status != AVIFENC_OK) return status;
    status = avif_write_box_end(writer, ipco);
    if (status != AVIFENC_OK) return status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('i', 'p', 'm', 'a'), &ipma);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u32be(writer, 1U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u16be(writer, AVIFENC_PRIMARY_ITEM_ID);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, 4U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, AVIFENC_PROPERTY_ISPE);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, AVIFENC_PROPERTY_PIXI);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(
        writer, 0x80U | AVIFENC_PROPERTY_AV1C);
    if (status != AVIFENC_OK) return status;
    status = avifenc_byte_writer_u8(writer, AVIFENC_PROPERTY_COLR);
    if (status != AVIFENC_OK) return status;
    status = avif_write_box_end(writer, ipma);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, iprp);
}

static AvifencStatus avif_write_meta(AvifencByteWriter *writer,
                                     const AvifencAvifConfig *config,
                                     uint32_t payload_size,
                                     size_t *extent_offset) {
    size_t meta;
    AvifencStatus status;

    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('m', 'e', 't', 'a'), &meta);
    if (status != AVIFENC_OK) return status;
    status = avif_write_full_box(writer, 0U, 0U);
    if (status != AVIFENC_OK) return status;
    status = avif_write_handler(writer);
    if (status != AVIFENC_OK) return status;
    status = avif_write_primary_item(writer);
    if (status != AVIFENC_OK) return status;
    status = avif_write_location(writer, payload_size, extent_offset);
    if (status != AVIFENC_OK) return status;
    status = avif_write_item_info(writer);
    if (status != AVIFENC_OK) return status;
    status = avif_write_properties(writer, config);
    if (status != AVIFENC_OK) return status;
    return avif_write_box_end(writer, meta);
}

AvifencStatus avifenc_avif_write(AvifencByteWriter *writer,
                                 const AvifencAvifConfig *config,
                                 const void *av1_payload,
                                 size_t av1_payload_size) {
    size_t extent_offset;
    size_t mdat;
    size_t payload_offset;
    AvifencStatus status;

    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (config == 0 || av1_payload == 0 || av1_payload_size == 0U ||
        config->width == 0U || config->height == 0U ||
        (config->width & 1U) != 0U || (config->height & 1U) != 0U ||
        config->width > AVIFENC_MAX_DIMENSION ||
        config->height > AVIFENC_MAX_DIMENSION ||
        config->seq_level_idx_0 > 31U || config->color.full_range > 1U ||
        config->color.chroma_sample_position > 3U) {
        return avif_write_fail(writer, AVIFENC_INVALID_ARGUMENT);
    }
    if (av1_payload_size > UINT32_MAX - 8U) {
        return avif_write_fail(writer, AVIFENC_LIMIT_EXCEEDED);
    }
    status = avif_write_ftyp(writer);
    if (status != AVIFENC_OK) return status;
    status = avif_write_meta(
        writer, config, (uint32_t)av1_payload_size, &extent_offset);
    if (status != AVIFENC_OK) return status;
    status = avif_write_box_begin(
        writer, AVIFDEC_FOURCC('m', 'd', 'a', 't'), &mdat);
    if (status != AVIFENC_OK) return status;
    payload_offset = avifenc_byte_writer_size(writer);
    if (payload_offset > UINT32_MAX) {
        return avif_write_fail(writer, AVIFENC_LIMIT_EXCEEDED);
    }
    status = avifenc_byte_writer_write(
        writer, av1_payload, av1_payload_size);
    if (status != AVIFENC_OK) return status;
    status = avif_write_box_end(writer, mdat);
    if (status != AVIFENC_OK) return status;
    return avifenc_byte_writer_patch_u32be(
        writer, extent_offset, (uint32_t)payload_offset);
}