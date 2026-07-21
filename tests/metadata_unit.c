#include "avif_metadata_items.h"

#include "base.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
#define FIXTURE_CAPACITY 65536U
#define FIXTURE_MAX_EXTENTS 32U
#define FIXTURE_MAX_PAYLOAD 32U

typedef struct {
    unsigned char data[FIXTURE_CAPACITY];
    size_t size;
    int failed;
} TestWriter;

typedef struct {
    uint32_t item_id;
    const unsigned char *data;
    size_t payload_offset;
    size_t size;
    size_t index_offset;
    size_t offset_patch;
    size_t length_patch;
} TestExtent;

typedef struct {
    uint8_t iloc_version;
    uint8_t construction_method;
    uint8_t iref_version;
    uint8_t iinf_version;
    uint8_t infe_version;
    uint8_t index_size;
    uint8_t base_offset_size;
    uint64_t extent_index;
    uint64_t base_offset;
    size_t idat_prefix_size;
    int separate_data_boxes;
    int include_exif;
    int include_mime;
    int include_thumbnail;
    int include_properties;
    int wide_overflow;
    int duplicate_idat;
    int dimg_link;
    int dimg_cycle;
    int thmb_cycle;
    int arbitrary_reference;
    int omit_mime_encoding;
    int include_altr_group;
    int conflicting_altr_group;
    int duplicate_altr_entity;
    int missing_altr_entity;
    int group_id_item_conflict;
    int duplicate_group_id;
    int short_altr_group;
    int truncated_altr_group;
    int trailing_altr_byte;
    unsigned int cdsc_target_count;
    unsigned int thmb_target_count;
    uint32_t property_item_id;
    const char *mime_type;
    const char *mime_encoding;
    size_t exif_size;
    uint8_t altr_version;
    uint32_t altr_flags;
} TestConfig;

typedef struct {
    TestWriter writer;
    TestExtent extents[FIXTURE_MAX_EXTENTS];
    size_t extent_count;
    size_t item_file_offsets[6][FIXTURE_MAX_PAYLOAD];
    size_t infe_id_offsets[6];
    size_t infe_flags_offsets[6];
    size_t protection_offsets[6];
    size_t location_id_offsets[6];
    size_t data_reference_offsets[6];
    size_t method_offsets[6];
    size_t iloc_sizes_offset;
    size_t exif_name_utf8_offset;
    size_t exif_name_terminator;
    size_t mime_content_type_offset;
    size_t mime_encoding_offset;
    size_t first_extent_offset_patch;
    size_t cdsc_from_offset;
    size_t duplicate_cdsc_target_offset;
    size_t ispe_width_offset;
    size_t irot_type_offset;
    size_t irot_payload_offset;
    size_t ipma_first_association;
    size_t ipma_second_association;
} TestFixture;

static TestFixture fixture;
static unsigned char unaligned_data[FIXTURE_CAPACITY + 1U];

static const unsigned char test_exif_payload[] = {
    0x00U, 0x00U, 0x00U, 0x00U,
    0x49U, 0x49U, 0x2aU, 0x00U,
    0xdeU, 0xadU, 0xbeU, 0xefU
};

static const unsigned char test_mime_payload[] = {
    0x3cU, 0x78U, 0x2fU, 0x3eU
};

static const unsigned char test_thumbnail_payload[] = {
    0x11U, 0x22U, 0x33U
};

static void writer_u8(TestWriter *writer, uint8_t value) {
    if (writer->size >= sizeof(writer->data)) {
        writer->failed = 1;
        return;
    }
    writer->data[writer->size++] = value;
}

static void writer_u16(TestWriter *writer, uint16_t value) {
    writer_u8(writer, (uint8_t)(value >> 8));
    writer_u8(writer, (uint8_t)value);
}

static void writer_u32(TestWriter *writer, uint32_t value) {
    writer_u8(writer, (uint8_t)(value >> 24));
    writer_u8(writer, (uint8_t)(value >> 16));
    writer_u8(writer, (uint8_t)(value >> 8));
    writer_u8(writer, (uint8_t)value);
}

static void writer_u64(TestWriter *writer, uint64_t value) {
    writer_u32(writer, (uint32_t)(value >> 32));
    writer_u32(writer, (uint32_t)value);
}

static void writer_patch_u16(TestWriter *writer,
                             size_t offset,
                             uint16_t value) {
    if (offset > writer->size || writer->size - offset < 2U) {
        writer->failed = 1;
        return;
    }
    writer->data[offset] = (unsigned char)(value >> 8);
    writer->data[offset + 1U] = (unsigned char)value;
}

static void writer_patch_u32(TestWriter *writer,
                             size_t offset,
                             uint32_t value) {
    if (offset > writer->size || writer->size - offset < 4U) {
        writer->failed = 1;
        return;
    }
    writer->data[offset] = (unsigned char)(value >> 24);
    writer->data[offset + 1U] = (unsigned char)(value >> 16);
    writer->data[offset + 2U] = (unsigned char)(value >> 8);
    writer->data[offset + 3U] = (unsigned char)value;
}

static size_t writer_box_begin(TestWriter *writer, uint32_t type) {
    size_t offset = writer->size;

    writer_u32(writer, 0U);
    writer_u32(writer, type);
    return offset;
}

static void writer_box_end(TestWriter *writer, size_t offset) {
    size_t size;

    if (offset > writer->size) {
        writer->failed = 1;
        return;
    }
    size = writer->size - offset;
    if (size > UINT32_MAX) {
        writer->failed = 1;
        return;
    }
    writer_patch_u32(writer, offset, (uint32_t)size);
}

static void writer_full_box(TestWriter *writer,
                            uint8_t version,
                            uint32_t flags) {
    writer_u8(writer, version);
    writer_u8(writer, (uint8_t)(flags >> 16));
    writer_u8(writer, (uint8_t)(flags >> 8));
    writer_u8(writer, (uint8_t)flags);
}

static void writer_bytes(TestWriter *writer,
                         const unsigned char *bytes,
                         size_t size) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        writer_u8(writer, bytes[index]);
    }
}

static void writer_text(TestWriter *writer, const char *text) {
    size_t offset = 0U;

    while (text[offset] != '\0') {
        writer_u8(writer, (uint8_t)(unsigned char)text[offset]);
        ++offset;
    }
    writer_u8(writer, 0U);
}

static TestConfig test_default_config(void) {
    TestConfig config;

    avifdec_memory_fill(&config, 0U, sizeof(config));
    config.iloc_version = 2U;
    config.construction_method = 0U;
    config.iref_version = 0U;
    config.iinf_version = 0U;
    config.infe_version = 2U;
    config.separate_data_boxes = 1;
    config.include_exif = 1;
    config.include_mime = 1;
    config.include_thumbnail = 1;
    config.include_properties = 1;
    config.cdsc_target_count = 2U;
    config.thmb_target_count = 2U;
    config.property_item_id = 4U;
    config.mime_type = "application/rdf+xml";
    config.mime_encoding = "";
    config.exif_size = sizeof(test_exif_payload);
    return config;
}

static void fixture_add_extent(TestFixture *result,
                               uint32_t item_id,
                               const unsigned char *data,
                               size_t payload_offset,
                               size_t size) {
    TestExtent *extent;

    if (result->extent_count >=
        sizeof(result->extents) / sizeof(result->extents[0])) {
        result->writer.failed = 1;
        return;
    }
    extent = &result->extents[result->extent_count++];
    extent->item_id = item_id;
    extent->data = data;
    extent->payload_offset = payload_offset;
    extent->size = size;
    extent->index_offset = 0U;
    extent->offset_patch = 0U;
    extent->length_patch = 0U;
}

static void fixture_split_payload(TestFixture *result,
                                  uint32_t item_id,
                                  const unsigned char *data,
                                  size_t size,
                                  const size_t *splits,
                                  size_t split_count) {
    size_t payload_offset = 0U;
    size_t split_index;

    for (split_index = 0U;
         split_index < split_count && payload_offset < size;
         ++split_index) {
        size_t take = splits[split_index];

        if (take > size - payload_offset) take = size - payload_offset;
        if (take != 0U) {
            fixture_add_extent(
                result, item_id, data, payload_offset, take);
            payload_offset += take;
        }
    }
    if (payload_offset < size) {
        fixture_add_extent(
            result, item_id, data, payload_offset,
            size - payload_offset);
    }
}

static void fixture_prepare_extents(TestFixture *result,
                                    const TestConfig *config) {
    static const size_t exif_splits[] = {
        1U, 2U, 1U, 1U, 1U, 2U, 4U
    };
    static const size_t mime_splits[] = { 1U, 3U };
    static const size_t thumbnail_splits[] = { 1U, 2U };

    if (config->include_exif) {
        fixture_split_payload(
            result, 2U, test_exif_payload, config->exif_size,
            exif_splits,
            sizeof(exif_splits) / sizeof(exif_splits[0]));
    }
    if (config->include_mime) {
        fixture_split_payload(
            result, 3U, test_mime_payload,
            sizeof(test_mime_payload), mime_splits,
            sizeof(mime_splits) / sizeof(mime_splits[0]));
    }
    if (config->include_thumbnail) {
        fixture_split_payload(
            result, 4U, test_thumbnail_payload,
            sizeof(test_thumbnail_payload), thumbnail_splits,
            sizeof(thumbnail_splits) /
                sizeof(thumbnail_splits[0]));
    }
}

static size_t fixture_item_count(const TestConfig *config) {
    size_t count = 2U;

    if (config->include_exif) ++count;
    if (config->include_mime) ++count;
    if (config->include_thumbnail) ++count;
    return count;
}

static void fixture_infe(TestFixture *result,
                         uint16_t item_id,
                         uint32_t item_type,
                         const char *name,
                         const TestConfig *config) {
    TestWriter *writer = &result->writer;
    size_t box = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'n', 'f', 'e'));

    writer_full_box(writer, config->infe_version, 0U);
    result->infe_flags_offsets[item_id] = writer->size - 1U;
    result->infe_id_offsets[item_id] = writer->size;
    if (config->infe_version == 2U) {
        writer_u16(writer, item_id);
    } else {
        writer_u32(writer, item_id);
    }
    result->protection_offsets[item_id] = writer->size;
    writer_u16(writer, 0U);
    writer_u32(writer, item_type);
    if (item_id == 2U) {
        static const unsigned char exif_name[] = {
            'E', 'x', 'i', 'f', ' ', 0xc2U, 0xa9U
        };
        writer_bytes(writer, exif_name, sizeof(exif_name));
        result->exif_name_utf8_offset = writer->size - 2U;
        result->exif_name_terminator = writer->size;
        writer_u8(writer, 0U);
    } else {
        writer_text(writer, name);
    }
    if (item_type == AVIFDEC_FOURCC('m', 'i', 'm', 'e')) {
        result->mime_content_type_offset = writer->size;
        writer_text(writer, config->mime_type);
        if (!config->omit_mime_encoding) {
            result->mime_encoding_offset = writer->size;
            writer_text(writer, config->mime_encoding);
        }
    }
    writer_box_end(writer, box);
}

static size_t fixture_extent_count(const TestFixture *result,
                                   uint32_t item_id) {
    size_t count = 0U;
    size_t extent_index;

    for (extent_index = 0U;
         extent_index < result->extent_count;
         ++extent_index) {
        if (result->extents[extent_index].item_id == item_id) ++count;
    }
    return count;
}

static void fixture_write_sized(TestWriter *writer,
                                unsigned int size,
                                uint64_t value) {
    unsigned int index;

    for (index = 0U; index < size; ++index) {
        unsigned int shift = (size - index - 1U) * 8U;
        writer_u8(writer, (uint8_t)(value >> shift));
    }
}

static void fixture_iloc_item(TestFixture *result,
                              const TestConfig *config,
                              uint32_t item_id) {
    TestWriter *writer = &result->writer;
    size_t extent_count = fixture_extent_count(result, item_id);
    size_t extent_index;
    unsigned int integer_size = config->wide_overflow ? 8U : 4U;

    result->location_id_offsets[item_id] = writer->size;
    if (config->iloc_version < 2U) {
        writer_u16(writer, (uint16_t)item_id);
    } else {
        writer_u32(writer, item_id);
    }
    if (config->iloc_version > 0U) {
        result->method_offsets[item_id] = writer->size;
        writer_u16(writer, config->construction_method);
    }
    result->data_reference_offsets[item_id] = writer->size;
    writer_u16(writer, 0U);
    if (config->wide_overflow) {
        writer_u64(writer, UINT64_MAX);
    } else {
        fixture_write_sized(
            writer, config->base_offset_size, config->base_offset);
    }
    writer_u16(writer, (uint16_t)extent_count);
    for (extent_index = 0U;
         extent_index < result->extent_count;
         ++extent_index) {
        TestExtent *extent = &result->extents[extent_index];

        if (extent->item_id != item_id) continue;
        if (config->iloc_version > 0U && config->index_size != 0U) {
            extent->index_offset = writer->size;
            fixture_write_sized(
                writer, config->index_size, config->extent_index);
        }
        extent->offset_patch = writer->size;
        fixture_write_sized(
            writer, integer_size, config->wide_overflow ? 1U : 0U);
        extent->length_patch = writer->size;
        fixture_write_sized(writer, integer_size, extent->size);
        if (result->first_extent_offset_patch == 0U) {
            result->first_extent_offset_patch = extent->offset_patch;
        }
    }
}

static size_t fixture_write_reference(TestWriter *writer,
                                      uint8_t version,
                                      uint32_t type,
                                      uint32_t from_item_id,
                                      const uint32_t *targets,
                                      size_t target_count,
                                      size_t *second_target_offset) {
    size_t box = writer_box_begin(writer, type);
    size_t from_offset = writer->size;
    size_t target_index;

    if (version == 0U) {
        writer_u16(writer, (uint16_t)from_item_id);
    } else {
        writer_u32(writer, from_item_id);
    }
    writer_u16(writer, (uint16_t)target_count);
    for (target_index = 0U; target_index < target_count; ++target_index) {
        if (target_index == 1U && second_target_offset != 0) {
            *second_target_offset = writer->size;
        }
        if (version == 0U) {
            writer_u16(writer, (uint16_t)targets[target_index]);
        } else {
            writer_u32(writer, targets[target_index]);
        }
    }
    writer_box_end(writer, box);
    return from_offset;
}

static void fixture_write_references(TestFixture *result,
                                     const TestConfig *config) {
    TestWriter *writer = &result->writer;
    uint32_t cdsc_targets[2] = { 1U, 5U };
    uint32_t thmb_targets[2] = { 1U, 5U };
    uint32_t one_target[1];
    size_t iref;

    if (config->cdsc_target_count == 0U &&
        config->thmb_target_count == 0U &&
        !config->dimg_link && !config->dimg_cycle &&
        !config->thmb_cycle && !config->arbitrary_reference) {
        return;
    }
    iref = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'r', 'e', 'f'));
    writer_full_box(writer, config->iref_version, 0U);
    if (config->include_exif && config->cdsc_target_count != 0U) {
        if (config->cdsc_target_count == 1U) cdsc_targets[0] = 5U;
        result->cdsc_from_offset = fixture_write_reference(
            writer, config->iref_version,
            AVIFDEC_FOURCC('c', 'd', 's', 'c'), 2U,
            cdsc_targets, config->cdsc_target_count,
            &result->duplicate_cdsc_target_offset);
    }
    if (config->include_thumbnail &&
        config->thmb_target_count != 0U) {
        fixture_write_reference(
            writer, config->iref_version,
            AVIFDEC_FOURCC('t', 'h', 'm', 'b'), 4U,
            thmb_targets, config->thmb_target_count, 0);
    }
    if (config->dimg_link || config->dimg_cycle) {
        one_target[0] = 5U;
        fixture_write_reference(
            writer, config->iref_version,
            AVIFDEC_FOURCC('d', 'i', 'm', 'g'), 1U,
            one_target, 1U, 0);
    }
    if (config->dimg_cycle) {
        one_target[0] = 1U;
        fixture_write_reference(
            writer, config->iref_version,
            AVIFDEC_FOURCC('d', 'i', 'm', 'g'), 5U,
            one_target, 1U, 0);
    }
    if (config->thmb_cycle) {
        one_target[0] = 4U;
        fixture_write_reference(
            writer, config->iref_version,
            AVIFDEC_FOURCC('t', 'h', 'm', 'b'), 1U,
            one_target, 1U, 0);
    }
    if (config->arbitrary_reference) {
        one_target[0] = 5U;
        fixture_write_reference(
            writer, config->iref_version,
            AVIFDEC_FOURCC('a', 'b', 'c', 'd'), 1U,
            one_target, 1U, 0);
    }
    writer_box_end(writer, iref);
}

static void fixture_write_properties(TestFixture *result,
                                     const TestConfig *config) {
    TestWriter *writer = &result->writer;
    size_t iprp;
    size_t ipco;
    size_t box;

    if (!config->include_properties) return;
    iprp = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'p', 'r', 'p'));
    ipco = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'p', 'c', 'o'));
    box = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 's', 'p', 'e'));
    writer_full_box(writer, 0U, 0U);
    result->ispe_width_offset = writer->size;
    writer_u32(writer, 3U);
    writer_u32(writer, 2U);
    writer_box_end(writer, box);
    result->irot_type_offset = writer->size + 4U;
    box = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'r', 'o', 't'));
    result->irot_payload_offset = writer->size;
    writer_u8(writer, 1U);
    writer_box_end(writer, box);
    writer_box_end(writer, ipco);
    box = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'p', 'm', 'a'));
    writer_full_box(writer, 0U, 0U);
    writer_u32(writer, 1U);
    writer_u16(writer, (uint16_t)config->property_item_id);
    writer_u8(writer, 2U);
    result->ipma_first_association = writer->size;
    writer_u8(writer, 1U);
    result->ipma_second_association = writer->size;
    writer_u8(writer, 0x82U);
    writer_box_end(writer, box);
    writer_box_end(writer, iprp);
}

static void fixture_write_groups(TestFixture *result,
                                 const TestConfig *config) {
    TestWriter *writer = &result->writer;
    uint32_t first_entities[2] = { 1U, 5U };
    uint32_t second_entities[2] = { 1U, 2U };
    uint32_t group_id =
        config->group_id_item_conflict ? 1U : 6U;
    size_t grpl;
    size_t altr;
    size_t entity_index;

    if (!config->include_altr_group) return;
    if (config->duplicate_altr_entity) {
        first_entities[1] = first_entities[0];
    } else if (config->missing_altr_entity) {
        first_entities[1] = 99U;
    }
    grpl = writer_box_begin(
        writer, AVIFDEC_FOURCC('g', 'r', 'p', 'l'));
    altr = writer_box_begin(
        writer, AVIFDEC_FOURCC('a', 'l', 't', 'r'));
    writer_full_box(
        writer, config->altr_version, config->altr_flags);
    writer_u32(writer, group_id);
    writer_u32(
        writer,
        config->truncated_altr_group
            ? 3U : (config->short_altr_group ? 1U : 2U));
    for (entity_index = 0U;
         entity_index < (config->short_altr_group ? 1U : 2U);
         ++entity_index) {
        writer_u32(writer, first_entities[entity_index]);
    }
    if (config->trailing_altr_byte) writer_u8(writer, 0U);
    writer_box_end(writer, altr);
    if (config->conflicting_altr_group ||
        config->duplicate_group_id) {
        altr = writer_box_begin(
            writer, AVIFDEC_FOURCC('a', 'l', 't', 'r'));
        writer_full_box(writer, 0U, 0U);
        writer_u32(
            writer, config->duplicate_group_id ? group_id : 7U);
        writer_u32(writer, 2U);
        for (entity_index = 0U; entity_index < 2U; ++entity_index) {
            writer_u32(writer, second_entities[entity_index]);
        }
        writer_box_end(writer, altr);
    }
    writer_box_end(writer, grpl);
}

static void fixture_record_payload(TestFixture *result,
                                   const TestExtent *extent,
                                   size_t file_offset) {
    size_t byte_index;

    if (extent->item_id >= 6U ||
        extent->payload_offset + extent->size > FIXTURE_MAX_PAYLOAD) {
        result->writer.failed = 1;
        return;
    }
    for (byte_index = 0U; byte_index < extent->size; ++byte_index) {
        result->item_file_offsets[extent->item_id]
                                 [extent->payload_offset + byte_index] =
            file_offset + byte_index;
    }
}

static void fixture_write_data(TestFixture *result,
                               const TestConfig *config,
                               uint32_t type) {
    TestWriter *writer = &result->writer;
    size_t logical_offset = 0U;
    size_t extent_index;
    size_t data_box = 0U;

    if (!config->separate_data_boxes) {
        size_t prefix_index;

        data_box = writer_box_begin(writer, type);
        for (prefix_index = 0U;
             prefix_index < config->idat_prefix_size;
             ++prefix_index) {
            writer_u8(writer, 0xeeU);
            ++logical_offset;
        }
    }
    for (extent_index = 0U;
         extent_index < result->extent_count;
         ++extent_index) {
        TestExtent *extent = &result->extents[extent_index];
        size_t file_offset;

        if (config->separate_data_boxes) {
            data_box = writer_box_begin(writer, type);
        }
        file_offset = writer->size;
        writer_bytes(
            writer, extent->data + extent->payload_offset, extent->size);
        fixture_record_payload(result, extent, file_offset);
        if (!config->wide_overflow) {
            uint64_t stored_offset;

            if (config->construction_method == 1U) {
                if ((uint64_t)logical_offset < config->base_offset) {
                    writer->failed = 1;
                    return;
                }
                stored_offset =
                    (uint64_t)logical_offset - config->base_offset;
            } else {
                if ((uint64_t)file_offset < config->base_offset) {
                    writer->failed = 1;
                    return;
                }
                stored_offset =
                    (uint64_t)file_offset - config->base_offset;
            }
            writer_patch_u32(
                writer, extent->offset_patch, (uint32_t)stored_offset);
        }
        if (config->separate_data_boxes) {
            writer_box_end(writer, data_box);
        }
        logical_offset += extent->size;
    }
    if (!config->separate_data_boxes) {
        writer_box_end(writer, data_box);
    }
}

static int fixture_build(TestFixture *result,
                         const TestConfig *config) {
    TestWriter *writer;
    size_t box;
    size_t meta;
    size_t iinf;
    size_t iloc;
    size_t item_count;

    avifdec_memory_fill(result, 0U, sizeof(*result));
    writer = &result->writer;
    fixture_prepare_extents(result, config);
    box = writer_box_begin(
        writer, AVIFDEC_FOURCC('f', 't', 'y', 'p'));
    writer_u32(writer, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    writer_u32(writer, 0U);
    writer_u32(writer, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    writer_box_end(writer, box);
    meta = writer_box_begin(
        writer, AVIFDEC_FOURCC('m', 'e', 't', 'a'));
    writer_full_box(writer, 0U, 0U);
    box = writer_box_begin(
        writer, AVIFDEC_FOURCC('h', 'd', 'l', 'r'));
    writer_full_box(writer, 0U, 0U);
    writer_u32(writer, 0U);
    writer_u32(writer, AVIFDEC_FOURCC('p', 'i', 'c', 't'));
    writer_u32(writer, 0U);
    writer_u32(writer, 0U);
    writer_u32(writer, 0U);
    writer_u8(writer, 0U);
    writer_box_end(writer, box);
    box = writer_box_begin(
        writer, AVIFDEC_FOURCC('p', 'i', 't', 'm'));
    writer_full_box(writer, 0U, 0U);
    writer_u16(writer, 1U);
    writer_box_end(writer, box);
    item_count = fixture_item_count(config);
    iinf = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'i', 'n', 'f'));
    writer_full_box(writer, config->iinf_version, 0U);
    if (config->iinf_version == 0U) {
        writer_u16(writer, (uint16_t)item_count);
    } else {
        writer_u32(writer, (uint32_t)item_count);
    }
    fixture_infe(
        result, 1U, AVIFDEC_FOURCC('a', 'v', '0', '1'),
        "primary", config);
    if (config->include_exif) {
        fixture_infe(
            result, 2U, AVIFDEC_FOURCC('E', 'x', 'i', 'f'),
            "exif", config);
    }
    if (config->include_mime) {
        fixture_infe(
            result, 3U, AVIFDEC_FOURCC('m', 'i', 'm', 'e'),
            "mime", config);
    }
    if (config->include_thumbnail) {
        fixture_infe(
            result, 4U, AVIFDEC_FOURCC('a', 'v', '0', '1'),
            "thumbnail", config);
    }
    fixture_infe(
        result, 5U, AVIFDEC_FOURCC('a', 'v', '0', '1'),
        "target", config);
    writer_box_end(writer, iinf);
    iloc = writer_box_begin(
        writer, AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
    writer_full_box(writer, config->iloc_version, 0U);
    result->iloc_sizes_offset = writer->size;
    writer_u8(writer, config->wide_overflow ? 0x88U : 0x44U);
    writer_u8(
        writer,
        (uint8_t)((config->wide_overflow
                       ? 0x80U
                       : (uint8_t)(config->base_offset_size << 4)) |
                  (config->iloc_version > 0U
                       ? config->index_size : 0U)));
    if (config->iloc_version < 2U) {
        writer_u16(writer, (uint16_t)(
            (config->include_exif ? 1U : 0U) +
            (config->include_mime ? 1U : 0U) +
            (config->include_thumbnail ? 1U : 0U)));
    } else {
        writer_u32(writer, (uint32_t)(
            (config->include_exif ? 1U : 0U) +
            (config->include_mime ? 1U : 0U) +
            (config->include_thumbnail ? 1U : 0U)));
    }
    if (config->include_exif) fixture_iloc_item(result, config, 2U);
    if (config->include_mime) fixture_iloc_item(result, config, 3U);
    if (config->include_thumbnail) fixture_iloc_item(result, config, 4U);
    writer_box_end(writer, iloc);
    fixture_write_references(result, config);
    fixture_write_properties(result, config);
    fixture_write_groups(result, config);
    if (config->construction_method == 1U) {
        fixture_write_data(
            result, config, AVIFDEC_FOURCC('i', 'd', 'a', 't'));
        if (config->duplicate_idat) {
            box = writer_box_begin(
                writer, AVIFDEC_FOURCC('i', 'd', 'a', 't'));
            writer_box_end(writer, box);
        }
    }
    writer_box_end(writer, meta);
    if (config->construction_method != 1U) {
        fixture_write_data(
            result, config, AVIFDEC_FOURCC('m', 'd', 'a', 't'));
    }
    return !writer->failed;
}

static int test_count_fill_and_views(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataInfo metadata[3];
    AvifItemThumbnailInfo thumbnails[2];
    AvifdecSpan spans[20];
    AvifItemMetadataResult count;
    AvifItemMetadataResult fill;
    AvifdecMetadataResult public_count;
    AvifdecError error;
    unsigned char reconstructed[sizeof(test_exif_payload)];
    size_t reconstructed_size = 0U;
    size_t span_index;

    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &count, &error) == AVIFDEC_OK);
    CHECK(count.primary_item_id == 1U &&
          count.metadata_count == 3U &&
          count.thumbnail_count == 2U &&
          count.span_count == 20U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 3U, thumbnails, 2U, spans, 20U,
              &fill, &error) == AVIFDEC_OK);
    CHECK(fill.primary_item_id == count.primary_item_id &&
          fill.metadata_count == count.metadata_count &&
          fill.thumbnail_count == count.thumbnail_count &&
          fill.span_count == count.span_count);
    CHECK(avifdec_metadata_query(
             fixture.writer.data, fixture.writer.size, 0,
             0, 0U, 0, 0U, 0, 0U,
             &public_count, &error) == AVIFDEC_OK);
    CHECK(public_count.primary_item_id == count.primary_item_id &&
          public_count.metadata_count == count.metadata_count &&
          public_count.thumbnail_count == count.thumbnail_count &&
          public_count.span_count == count.span_count);
    CHECK(avifdec_metadata_query(
             fixture.writer.data, fixture.writer.size, 0,
             metadata, 3U, thumbnails, 2U, spans, 20U,
             &public_count, &error) == AVIFDEC_OK);
    CHECK(avifdec_metadata_query(
             fixture.writer.data, fixture.writer.size, 0,
             metadata, 2U, thumbnails, 2U, spans, 20U,
             &public_count, &error) == AVIFDEC_OUT_OF_MEMORY);
    CHECK(public_count.metadata_count == 3U &&
          public_count.thumbnail_count == 2U &&
          public_count.span_count == 20U);
    CHECK(metadata[0].kind == AVIF_ITEM_METADATA_EXIF &&
          metadata[0].target_item_id == 1U &&
          metadata[0].scope == AVIF_ITEM_METADATA_SCOPE_ITEM &&
          metadata[0].relationship_type ==
              AVIFDEC_FOURCC('c', 'd', 's', 'c') &&
          metadata[0].content_offset == 4U &&
          metadata[0].exif_tiff_offset == 4U &&
          metadata[0].exif_byte_order ==
              AVIF_ITEM_TIFF_BYTE_ORDER_LITTLE &&
          metadata[0].span_count == 7U);
    CHECK(metadata[1].target_item_id == 5U &&
          metadata[1].span_count == metadata[0].span_count);
    CHECK(metadata[2].kind == AVIF_ITEM_METADATA_XMP &&
          metadata[2].scope == AVIF_ITEM_METADATA_SCOPE_UNSCOPED &&
          metadata[2].target_item_id == 0U &&
          (metadata[2].flags &
           AVIF_ITEM_METADATA_FLAG_CANONICAL_XMP) != 0U &&
          metadata[2].content_type.size == 19U &&
          metadata[2].content_encoding.size == 0U);
    CHECK(metadata[0].item_name.data ==
              fixture.writer.data + fixture.exif_name_utf8_offset - 5U &&
          metadata[0].item_name.size == 7U);
    CHECK(thumbnails[0].thumbnail_item_id == 4U &&
          thumbnails[0].target_item_id == 1U &&
          thumbnails[0].width == 3U &&
          thumbnails[0].height == 2U &&
          thumbnails[0].presentation_width == 2U &&
          thumbnails[0].presentation_height == 3U &&
          thumbnails[1].target_item_id == 5U);
    for (span_index = metadata[0].span_index;
         span_index < metadata[0].span_index + metadata[0].span_count;
         ++span_index) {
        CHECK(spans[span_index].data ==
              fixture.writer.data + spans[span_index].file_offset);
        avifdec_memory_copy(
            reconstructed + reconstructed_size,
            spans[span_index].data, spans[span_index].size);
        reconstructed_size += spans[span_index].size;
    }
    CHECK(reconstructed_size == sizeof(test_exif_payload) &&
          avifdec_memory_compare(
              reconstructed, test_exif_payload,
              sizeof(test_exif_payload)) == 0);
    return 0;
}

static int test_capacity_and_canaries(void) {
    TestConfig config = test_default_config();
    struct {
        uint64_t before;
        AvifItemMetadataInfo values[3];
        uint64_t after;
    } metadata;
    struct {
        uint64_t before;
        AvifItemThumbnailInfo values[2];
        uint64_t after;
    } thumbnails;
    struct {
        uint64_t before;
        AvifdecSpan values[20];
        uint64_t after;
    } spans;
    unsigned char metadata_before[sizeof(metadata)];
    unsigned char thumbnails_before[sizeof(thumbnails)];
    unsigned char spans_before[sizeof(spans)];
    AvifItemMetadataResult result;
    AvifdecError error;

    CHECK(fixture_build(&fixture, &config));
    avifdec_memory_fill(&metadata, 0xa5U, sizeof(metadata));
    avifdec_memory_fill(&thumbnails, 0x5aU, sizeof(thumbnails));
    avifdec_memory_fill(&spans, 0x3cU, sizeof(spans));
    avifdec_memory_copy(metadata_before, &metadata, sizeof(metadata));
    avifdec_memory_copy(thumbnails_before, &thumbnails, sizeof(thumbnails));
    avifdec_memory_copy(spans_before, &spans, sizeof(spans));
    fixture.writer.data[fixture.item_file_offsets[2][6]] = 0U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata.values, 3U, thumbnails.values, 2U,
              spans.values, 20U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(avifdec_memory_compare(
              &metadata, metadata_before, sizeof(metadata)) == 0 &&
          avifdec_memory_compare(
              &thumbnails, thumbnails_before, sizeof(thumbnails)) == 0 &&
          avifdec_memory_compare(
              &spans, spans_before, sizeof(spans)) == 0);
    fixture.writer.data[fixture.item_file_offsets[2][6]] = 42U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata.values, 2U, thumbnails.values, 2U,
              spans.values, 20U, &result, &error) ==
          AVIFDEC_OUT_OF_MEMORY);
    CHECK(result.metadata_count == 3U &&
          result.thumbnail_count == 2U &&
          result.span_count == 20U &&
          avifdec_memory_compare(
              &metadata, metadata_before, sizeof(metadata)) == 0 &&
          avifdec_memory_compare(
              &thumbnails, thumbnails_before, sizeof(thumbnails)) == 0 &&
          avifdec_memory_compare(
              &spans, spans_before, sizeof(spans)) == 0);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata.values, 3U, thumbnails.values, 2U,
              spans.values, 19U, &result, &error) ==
          AVIFDEC_OUT_OF_MEMORY);
    CHECK(avifdec_memory_compare(
              &metadata, metadata_before, sizeof(metadata)) == 0 &&
          avifdec_memory_compare(
              &thumbnails, thumbnails_before, sizeof(thumbnails)) == 0 &&
          avifdec_memory_compare(
              &spans, spans_before, sizeof(spans)) == 0);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata.values, 3U, thumbnails.values, 2U,
              spans.values, 20U, &result, &error) == AVIFDEC_OK);
    CHECK(metadata.before == (uint64_t)0xa5a5a5a5a5a5a5a5ULL &&
          metadata.after == (uint64_t)0xa5a5a5a5a5a5a5a5ULL &&
          thumbnails.before == (uint64_t)0x5a5a5a5a5a5a5a5aULL &&
          thumbnails.after == (uint64_t)0x5a5a5a5a5a5a5a5aULL &&
          spans.before == (uint64_t)0x3c3c3c3c3c3c3c3cULL &&
          spans.after == (uint64_t)0x3c3c3c3c3c3c3c3cULL);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 1U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int test_idat_and_versions(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataInfo metadata[3];
    AvifItemThumbnailInfo thumbnails[2];
    AvifdecSpan spans[20];
    AvifItemMetadataResult result;
    AvifItemIndex index;
    AvifItemIndexLocation location;
    AvifdecError error;

    config.iloc_version = 1U;
    config.construction_method = 1U;
    config.iref_version = 1U;
    config.index_size = 4U;
    config.extent_index = 7U;
    config.base_offset_size = 4U;
    config.base_offset = 2U;
    config.idat_prefix_size = 2U;
    config.separate_data_boxes = 0;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 3U, thumbnails, 2U, spans, 20U,
              &result, &error) == AVIFDEC_OK);
    CHECK(result.span_count == 20U &&
          metadata[0].span_count == 7U &&
          spans[0].file_offset ==
              fixture.item_file_offsets[2][0]);
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_OK);
    CHECK(avif_item_index_find_location(
              &index, 2U, &location, &error) == AVIFDEC_OK);
    CHECK(location.construction_method == 1U &&
          location.index_size == 4U &&
          location.base_offset == 2U &&
          location.extent_count == 7U &&
          location.extents[0].index == 7U);
    fixture.writer.data[fixture.cdsc_from_offset] = 1U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    config = test_default_config();
    config.iloc_version = 0U;
    config.base_offset_size = 4U;
    config.base_offset = 1U;
    config.separate_data_boxes = 0;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) == AVIFDEC_OK);
    CHECK(result.metadata_count == 3U && result.thumbnail_count == 2U);
    config = test_default_config();
    config.iinf_version = 1U;
    config.infe_version = 3U;
    config.iref_version = 1U;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) == AVIFDEC_OK);
    CHECK(result.metadata_count == 3U && result.thumbnail_count == 2U);
    config = test_default_config();
    config.iloc_version = 2U;
    config.construction_method = 1U;
    config.separate_data_boxes = 0;
    config.duplicate_idat = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    config = test_default_config();
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.iloc_sizes_offset] = 0x24U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == fixture.iloc_sizes_offset &&
          error.context == AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
    config = test_default_config();
    config.iloc_version = 1U;
    config.construction_method = 1U;
    config.index_size = 2U;
    config.separate_data_boxes = 0;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    return 0;
}

static int test_exif_validation(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataInfo metadata[1];
    AvifdecSpan spans[7];
    AvifItemMetadataResult result;
    AvifdecError error;
    AvifdecStatus maximum_offset_status =
        sizeof(size_t) <= 4U ? AVIFDEC_OVERFLOW : AVIFDEC_TRUNCATED;

    config.include_mime = 0;
    config.include_thumbnail = 0;
    config.include_properties = 0;
    config.cdsc_target_count = 0U;
    config.thmb_target_count = 0U;
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.item_file_offsets[2][3]] = 2U;
    fixture.writer.data[fixture.item_file_offsets[2][4]] = 0xaaU;
    fixture.writer.data[fixture.item_file_offsets[2][5]] = 0xbbU;
    fixture.writer.data[fixture.item_file_offsets[2][6]] = 'I';
    fixture.writer.data[fixture.item_file_offsets[2][7]] = 'I';
    fixture.writer.data[fixture.item_file_offsets[2][8]] = 42U;
    fixture.writer.data[fixture.item_file_offsets[2][9]] = 0U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 1U, 0, 0U, spans, 7U,
              &result, &error) == AVIFDEC_OK);
    CHECK(metadata[0].content_offset == 4U &&
          metadata[0].exif_tiff_offset == 6U &&
          metadata[0].exif_byte_order ==
              AVIF_ITEM_TIFF_BYTE_ORDER_LITTLE);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.item_file_offsets[2][4]] = 'M';
    fixture.writer.data[fixture.item_file_offsets[2][5]] = 'M';
    fixture.writer.data[fixture.item_file_offsets[2][6]] = 0U;
    fixture.writer.data[fixture.item_file_offsets[2][7]] = 42U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 1U, 0, 0U, spans, 7U,
              &result, &error) == AVIFDEC_OK);
    CHECK(metadata[0].exif_byte_order ==
          AVIF_ITEM_TIFF_BYTE_ORDER_BIG);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.item_file_offsets[2][4]] = 'X';
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == fixture.item_file_offsets[2][4] &&
          error.context == AVIFDEC_FOURCC('E', 'x', 'i', 'f'));
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.item_file_offsets[2][6]] = 0U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == fixture.item_file_offsets[2][6]);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.item_file_offsets[2][3]] = 10U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_TRUNCATED);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.item_file_offsets[2][0]] = 0xffU;
    fixture.writer.data[fixture.item_file_offsets[2][1]] = 0xffU;
    fixture.writer.data[fixture.item_file_offsets[2][2]] = 0xffU;
    fixture.writer.data[fixture.item_file_offsets[2][3]] = 0xffU;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          maximum_offset_status);
    config.exif_size = 3U;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_TRUNCATED);
    return 0;
}

static int test_mime_and_strings(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataInfo metadata[1];
    AvifdecSpan spans[2];
    AvifItemMetadataResult result;
    AvifdecError error;

    config.include_exif = 0;
    config.include_thumbnail = 0;
    config.include_properties = 0;
    config.cdsc_target_count = 0U;
    config.thmb_target_count = 0U;
    config.mime_type = "application/octet-stream";
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 1U, 0, 0U, spans, 2U,
              &result, &error) == AVIFDEC_OK);
    CHECK(metadata[0].kind == AVIF_ITEM_METADATA_MIME &&
          metadata[0].flags == 0U);
    config.mime_type = "application/rdf+xml";
    config.omit_mime_encoding = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 1U, 0, 0U, spans, 2U,
              &result, &error) == AVIFDEC_OK);
    CHECK(metadata[0].kind == AVIF_ITEM_METADATA_XMP &&
          metadata[0].content_encoding.size == 0U);
    config.omit_mime_encoding = 0;
    config.mime_encoding = "gzip";
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_UNSUPPORTED);
    CHECK(error.offset == fixture.mime_encoding_offset &&
          error.context == AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    config = test_default_config();
    config.include_mime = 0;
    config.include_thumbnail = 0;
    config.include_properties = 0;
    config.cdsc_target_count = 0U;
    config.thmb_target_count = 0U;
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.exif_name_utf8_offset] = 0xc0U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == fixture.exif_name_utf8_offset);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.exif_name_terminator] = 'x';
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_TRUNCATED);
    return 0;
}

static int test_infe_flags(void) {
    TestConfig config = test_default_config();
    AvifItemIndex index;
    const AvifItemIndexItem *item;
    AvifdecError error;

    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.infe_flags_offsets[1]] = 1U;
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_OK);
    item = avif_item_index_find_item(&index, 1U);
    CHECK(item != 0 && item->flags == 1U);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.infe_flags_offsets[1]] = 2U;
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.offset == fixture.infe_flags_offsets[1] &&
          error.context == AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    return 0;
}

static int test_direct_scope_and_no_inheritance(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataInfo metadata[1];
    AvifdecSpan spans[7];
    AvifItemMetadataResult result;
    AvifdecError error;

    config.include_mime = 0;
    config.include_thumbnail = 0;
    config.include_properties = 0;
    config.cdsc_target_count = 1U;
    config.thmb_target_count = 0U;
    config.dimg_link = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 1U, 0, 0U, spans, 7U,
              &result, &error) == AVIFDEC_OK);
    CHECK(result.metadata_count == 1U &&
          metadata[0].target_item_id == 5U &&
          metadata[0].scope == AVIF_ITEM_METADATA_SCOPE_ITEM);
    config.cdsc_target_count = 0U;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 1U, 0, 0U, spans, 7U,
              &result, &error) == AVIFDEC_OK);
    CHECK(metadata[0].scope == AVIF_ITEM_METADATA_SCOPE_UNSCOPED &&
          metadata[0].target_item_id == 0U);
    return 0;
}

static int test_unsupported_access(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataResult result;
    AvifdecError error;

    config.include_mime = 0;
    config.include_thumbnail = 0;
    config.include_properties = 0;
    config.cdsc_target_count = 0U;
    config.thmb_target_count = 0U;
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u16(
        &fixture.writer, fixture.protection_offsets[2], 1U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_UNSUPPORTED);
    CHECK(error.offset == fixture.protection_offsets[2] &&
          error.context == AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u16(
        &fixture.writer, fixture.data_reference_offsets[2], 1U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_UNSUPPORTED);
    CHECK(error.offset == fixture.data_reference_offsets[2] &&
          error.context == AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
    config.construction_method = 2U;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_UNSUPPORTED);
    CHECK(error.offset == fixture.method_offsets[2] &&
          error.context == AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
    config = test_default_config();
    config.include_mime = 0;
    config.include_thumbnail = 0;
    config.include_properties = 0;
    config.cdsc_target_count = 0U;
    config.thmb_target_count = 0U;
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u32(
        &fixture.writer, fixture.first_extent_offset_patch, 0U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    config.wide_overflow = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_OVERFLOW);
    return 0;
}

static int test_properties(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataInfo metadata[3];
    AvifItemThumbnailInfo thumbnails[2];
    AvifdecSpan spans[20];
    AvifItemMetadataResult result;
    AvifdecError error;

    CHECK(fixture_build(&fixture, &config));
    writer_patch_u32(
        &fixture.writer, fixture.irot_type_offset,
        AVIFDEC_FOURCC('z', 'z', 'z', 'z'));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_UNSUPPORTED);
    fixture.writer.data[fixture.ipma_second_association] = 2U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              metadata, 3U, thumbnails, 2U, spans, 20U,
              &result, &error) == AVIFDEC_OK);
    CHECK(thumbnails[0].presentation_width == 3U &&
          thumbnails[0].presentation_height == 2U);
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u32(
        &fixture.writer, fixture.irot_type_offset,
        AVIFDEC_FOURCC('t', 'm', 'a', 'p'));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_UNSUPPORTED);
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u32(
        &fixture.writer, fixture.ispe_width_offset, 0U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.ipma_second_association] = 2U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    config = test_default_config();
    config.include_mime = 0;
    config.include_thumbnail = 0;
    config.cdsc_target_count = 0U;
    config.thmb_target_count = 0U;
    config.property_item_id = 2U;
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u32(
        &fixture.writer, fixture.irot_type_offset,
        AVIFDEC_FOURCC('z', 'z', 'z', 'z'));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_UNSUPPORTED);
    return 0;
}

static int test_duplicates_cycles_and_edges(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataResult result;
    AvifItemIndex index;
    AvifdecError error;
    uint32_t reference_ids[2] = { 0xa5a5a5a5U, 0x5a5a5a5aU };
    size_t reference_count;
    size_t reference_offset;
    size_t reference_index;
    int found_arbitrary = 0;

    CHECK(fixture_build(&fixture, &config));
    writer_patch_u16(
        &fixture.writer, fixture.infe_id_offsets[3], 2U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u32(
        &fixture.writer, fixture.location_id_offsets[3], 2U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(fixture_build(&fixture, &config));
    writer_patch_u16(
        &fixture.writer, fixture.duplicate_cdsc_target_offset, 1U);
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(fixture_build(&fixture, &config));
    fixture.writer.data[fixture.ipma_second_association] = 0x81U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    config = test_default_config();
    config.dimg_cycle = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    config = test_default_config();
    config.thmb_cycle = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_INVALID_DATA);
    config = test_default_config();
    config.arbitrary_reference = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_OK);
    for (reference_index = 0U;
         reference_index < index.reference_count;
         ++reference_index) {
        if (index.references[reference_index].type ==
            AVIFDEC_FOURCC('a', 'b', 'c', 'd')) {
            found_arbitrary = 1;
        }
    }
    CHECK(found_arbitrary);
    CHECK(avif_item_index_query_references(
              &index, AVIFDEC_FOURCC('c', 'd', 's', 'c'), 2U,
              0, 0U, &reference_count, &reference_offset,
              &error) == AVIFDEC_OK);
    CHECK(reference_count == 2U && reference_offset != 0U);
    CHECK(avif_item_index_query_references(
              &index, AVIFDEC_FOURCC('c', 'd', 's', 'c'), 2U,
              reference_ids, 1U, &reference_count, &reference_offset,
              &error) == AVIFDEC_OUT_OF_MEMORY);
    CHECK(reference_count == 2U &&
          reference_ids[0] == 0xa5a5a5a5U &&
          reference_ids[1] == 0x5a5a5a5aU);
    CHECK(avif_item_index_query_references(
              &index, AVIFDEC_FOURCC('c', 'd', 's', 'c'), 2U,
              reference_ids, 2U, &reference_count, &reference_offset,
              &error) == AVIFDEC_OK);
    CHECK(reference_ids[0] == 1U && reference_ids[1] == 5U);
    return 0;
}

static int test_limits(void) {
    TestConfig config = test_default_config();
    AvifItemIndexLimits limits;
    AvifItemMetadataResult result;
    AvifdecError error;

    CHECK(fixture_build(&fixture, &config));
    avif_item_index_default_limits(&limits);
    limits.max_items = 4U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_extents = 6U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_properties = 1U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_metadata_items = 4U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_metadata_spans = 19U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_references = 3U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_associations = 1U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_data_boxes = 1U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    config.separate_data_boxes = 0;
    CHECK(fixture_build(&fixture, &config));
    avif_item_index_default_limits(&limits);
    limits.max_width = 2U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    avif_item_index_default_limits(&limits);
    limits.max_pixels = 5U;
    CHECK(avif_metadata_items_query(
              fixture.writer.data, fixture.writer.size, &limits,
              0, 0U, 0, 0U, 0, 0U, &result, &error) ==
          AVIFDEC_LIMIT_EXCEEDED);
    return 0;
}

static int test_alternative_entity_groups(void) {
    TestConfig config = test_default_config();
    AvifItemIndex index;
    AvifItemIndexLimits limits;
    AvifItemAlternativeOrder order;
    size_t group_offset;
    AvifdecError error;

    config.include_altr_group = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_OK);
    CHECK(index.entity_group_count == 1U &&
          index.group_entity_count == 2U &&
          index.reference_count == 4U);
    CHECK(avif_item_index_alternative_order(
              &index, 1U, 5U, &order, &group_offset,
              &error) == AVIFDEC_OK);
    CHECK(order == AVIF_ITEM_ALTERNATIVE_FIRST_BEFORE_SECOND &&
          group_offset != 0U);
    CHECK(avif_item_index_alternative_order(
              &index, 5U, 1U, &order, &group_offset,
              &error) == AVIFDEC_OK);
    CHECK(order == AVIF_ITEM_ALTERNATIVE_SECOND_BEFORE_FIRST);
    CHECK(avif_item_index_alternative_order(
              &index, 1U, 2U, &order, &group_offset,
              &error) == AVIFDEC_OK);
    CHECK(order == AVIF_ITEM_ALTERNATIVE_NONE);

    config.altr_version = 1U;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_UNSUPPORTED);
    config.altr_version = 0U;
    config.altr_flags = 1U;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    config.altr_flags = 0U;
    config.short_altr_group = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    config.short_altr_group = 0;
    config.truncated_altr_group = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_TRUNCATED);
    config.truncated_altr_group = 0;
    config.trailing_altr_byte = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    config.trailing_altr_byte = 0;
    config.duplicate_altr_entity = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    config.duplicate_altr_entity = 0;
    config.missing_altr_entity = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    config.missing_altr_entity = 0;
    config.group_id_item_conflict = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    config.group_id_item_conflict = 0;
    config.conflicting_altr_group = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);
    config.conflicting_altr_group = 0;
    config.duplicate_group_id = 1;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_INVALID_DATA);

    config.duplicate_group_id = 0;
    config.conflicting_altr_group = 1;
    CHECK(fixture_build(&fixture, &config));
    avif_item_index_default_limits(&limits);
    limits.max_entity_groups = 1U;
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, &limits,
              &index, &error) == AVIFDEC_LIMIT_EXCEEDED);
    config.conflicting_altr_group = 0;
    CHECK(fixture_build(&fixture, &config));
    avif_item_index_default_limits(&limits);
    limits.max_group_entities = 1U;
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, &limits,
              &index, &error) == AVIFDEC_LIMIT_EXCEEDED);
    return 0;
}

static int test_unaligned_input(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataResult result;
    AvifdecError error;

    CHECK(fixture_build(&fixture, &config));
    CHECK(fixture.writer.size <= FIXTURE_CAPACITY);
    avifdec_memory_copy(
        unaligned_data + 1U, fixture.writer.data, fixture.writer.size);
    CHECK(avif_metadata_items_query(
              unaligned_data + 1U, fixture.writer.size, 0,
              0, 0U, 0, 0U, 0, 0U, &result, &error) == AVIFDEC_OK);
    CHECK(result.metadata_count == 3U &&
          result.thumbnail_count == 2U);
    return 0;
}

static int test_common_resolver(void) {
    TestConfig config = test_default_config();
    AvifItemIndex index;
    AvifItemPayload count;
    AvifItemPayload fill;
    AvifdecSpan spans[7];
    AvifdecSpan selected_span;
    unsigned char spans_before[sizeof(spans)];
    unsigned char field[4];
    size_t file_offset;
    size_t span_index;
    AvifdecError error;

    config.include_mime = 0;
    config.include_thumbnail = 0;
    config.include_properties = 0;
    config.cdsc_target_count = 0U;
    config.thmb_target_count = 0U;
    CHECK(fixture_build(&fixture, &config));
    CHECK(avif_item_index_build(
              fixture.writer.data, fixture.writer.size, 0,
              &index, &error) == AVIFDEC_OK);
    CHECK(avif_item_index_resolve_item(
              &index, 2U, 0, 0U, &count, &error) == AVIFDEC_OK);
    CHECK(count.item_id == 2U &&
          count.payload_size == sizeof(test_exif_payload) &&
          count.span_count == 7U);
    avifdec_memory_fill(spans, 0xa7U, sizeof(spans));
    avifdec_memory_copy(spans_before, spans, sizeof(spans));
    CHECK(avif_item_index_resolve_item(
              &index, 2U, spans, 6U, &fill, &error) ==
          AVIFDEC_OUT_OF_MEMORY);
    CHECK(fill.payload_size == count.payload_size &&
          fill.span_count == count.span_count &&
          avifdec_memory_compare(
              spans, spans_before, sizeof(spans)) == 0);
    CHECK(avif_item_index_resolve_item(
              &index, 2U, spans, 7U, &fill, &error) == AVIFDEC_OK);
    CHECK(fill.payload_size == count.payload_size &&
          fill.span_count == count.span_count);
    for (span_index = 0U; span_index < fill.span_count; ++span_index) {
        CHECK(avif_item_index_item_span_at(
                  &index, 2U, span_index, &selected_span,
                  &error) == AVIFDEC_OK);
        CHECK(selected_span.data == spans[span_index].data &&
              selected_span.size == spans[span_index].size &&
              selected_span.file_offset ==
                  spans[span_index].file_offset);
    }
    CHECK(avif_item_index_item_span_at(
              &index, 2U, fill.span_count, &selected_span,
              &error) == AVIFDEC_INVALID_ARGUMENT);
    CHECK(avif_item_index_read_item(
              &index, 2U, 2U, field, sizeof(field),
              &file_offset, &error) == AVIFDEC_OK);
    CHECK(avifdec_memory_compare(
              field, test_exif_payload + 2U, sizeof(field)) == 0 &&
          file_offset == fixture.item_file_offsets[2][2]);
    return 0;
}

static int test_truncation_sweep(void) {
    TestConfig config = test_default_config();
    AvifItemMetadataResult result;
    AvifdecError error;
    size_t size;

    CHECK(fixture_build(&fixture, &config));
    for (size = 0U; size < fixture.writer.size; ++size) {
        CHECK(avif_metadata_items_query(
                  fixture.writer.data, size, 0,
                  0, 0U, 0, 0U, 0, 0U, &result, &error) != AVIFDEC_OK);
        CHECK(error.status != AVIFDEC_OK &&
              error.offset <= size);
    }
    return 0;
}

typedef struct {
    AvifItemTrackMetadataView view;
    size_t calls;
} TestTrackSource;

static AvifdecStatus test_track_view(const void *context,
                                     uint32_t track_id,
                                     AvifItemTrackMetadataView *view,
                                     AvifdecError *error) {
    TestTrackSource *source = (TestTrackSource *)context;

    (void)track_id;
    (void)error;
    ++source->calls;
    *view = source->view;
    return AVIFDEC_OK;
}

static int test_track_extension(void) {
    static const unsigned char payload[] = { 1U, 2U, 3U };
    AvifdecSpan source_span;
    AvifItemMetadataInfo source_info;
    AvifItemMetadataInfo output_info;
    AvifdecSpan output_span;
    TestTrackSource context;
    AvifItemTrackMetadataSource source;
    AvifItemMetadataResult result;
    AvifdecError error;

    avifdec_memory_fill(&source_info, 0U, sizeof(source_info));
    source_span.data = payload;
    source_span.size = sizeof(payload);
    source_span.file_offset = 123U;
    source_info.target_track_id = 7U;
    source_info.scope = AVIF_ITEM_METADATA_SCOPE_TRACK;
    source_info.flags = AVIF_ITEM_METADATA_FLAG_SEQUENCE_WIDE;
    source_info.kind = AVIF_ITEM_METADATA_MIME;
    source_info.payload_size = sizeof(payload);
    source_info.span_count = 1U;
    avifdec_memory_fill(&context, 0U, sizeof(context));
    context.view.metadata = &source_info;
    context.view.metadata_count = 1U;
    context.view.spans = &source_span;
    context.view.span_count = 1U;
    source.context = &context;
    source.view = test_track_view;
    CHECK(avif_metadata_items_query_track_source(
              &source, 7U, 0, 0, 0U, 0, 0U, 0, 0U,
              &result, &error) == AVIFDEC_OK);
    CHECK(result.metadata_count == 1U && result.span_count == 1U &&
          context.calls == 1U);
    CHECK(avif_metadata_items_query_track_source(
              &source, 7U, 0, &output_info, 1U, 0, 0U,
              &output_span, 1U, &result, &error) == AVIFDEC_OK);
    CHECK(context.calls == 2U &&
          output_info.target_track_id == 7U &&
          output_span.data == payload &&
          output_span.file_offset == 123U);
    CHECK(avif_metadata_items_query_track_source(
              &source, 8U, 0, 0, 0U, 0, 0U, 0, 0U,
              &result, &error) == AVIFDEC_INVALID_DATA);
    source_info.payload_size = 4U;
    CHECK(avif_metadata_items_query_track_source(
              &source, 7U, 0, 0, 0U, 0, 0U, 0, 0U,
              &result, &error) == AVIFDEC_INVALID_DATA);
    return 0;
}

int main(void) {
    int status;

    status = test_count_fill_and_views();
    if (status != 0) return status;
    status = test_capacity_and_canaries();
    if (status != 0) return status;
    status = test_idat_and_versions();
    if (status != 0) return status;
    status = test_exif_validation();
    if (status != 0) return status;
    status = test_mime_and_strings();
    if (status != 0) return status;
    status = test_infe_flags();
    if (status != 0) return status;
    status = test_direct_scope_and_no_inheritance();
    if (status != 0) return status;
    status = test_unsupported_access();
    if (status != 0) return status;
    status = test_properties();
    if (status != 0) return status;
    status = test_duplicates_cycles_and_edges();
    if (status != 0) return status;
    status = test_limits();
    if (status != 0) return status;
    status = test_alternative_entity_groups();
    if (status != 0) return status;
    status = test_unaligned_input();
    if (status != 0) return status;
    status = test_common_resolver();
    if (status != 0) return status;
    status = test_truncation_sweep();
    if (status != 0) return status;
    return test_track_extension();
}
