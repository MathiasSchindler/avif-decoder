#include "avif_gain_map.h"

#ifdef AVIF_GAIN_MAP_HOSTED
#include <math.h>
#include <stdio.h>
#endif

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static const unsigned char one_channel_payload[62] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U, 0xffU, 0xffU,
    0xffU, 0xffU, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x40U, 0xffU, 0xffU,
    0xffU, 0xffU, 0x00U, 0x00U, 0x00U, 0x80U
};

static const unsigned char three_channel_payload[142] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x09U, 0x00U, 0x00U, 0x00U, 0x02U, 0xffU, 0xffU,
    0xffU, 0xfeU, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x40U, 0xffU, 0xffU,
    0xffU, 0xffU, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U,
    0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U,
    0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x20U, 0xffU, 0xffU,
    0xffU, 0xffU, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x03U, 0xffU, 0xffU,
    0xffU, 0xffU, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U, 0x00U,
    0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x80U
};

static const unsigned char common_one_channel_payload[38] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x4cU, 0x00U, 0x00U,
    0x03U, 0xe8U, 0x00U, 0x00U, 0x05U, 0x14U, 0x00U, 0x00U,
    0x00U, 0x00U, 0xffU, 0xffU, 0xfcU, 0x18U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0xe8U, 0x00U, 0x00U,
    0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x10U
};

static const unsigned char common_three_channel_payload[78] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x88U, 0x00U, 0x00U,
    0x03U, 0xe8U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x05U, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x05U, 0x14U, 0x00U, 0x00U, 0x03U, 0xe8U, 0x00U, 0x00U,
    0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x10U, 0xffU, 0xffU,
    0xfeU, 0x0cU, 0x00U, 0x00U, 0x05U, 0xdcU, 0x00U, 0x00U,
    0x03U, 0xe8U, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U, 0x00U,
    0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x04U, 0xb0U, 0x00U, 0x00U, 0x03U, 0xe8U, 0x00U, 0x00U,
    0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x10U
};

_Static_assert(sizeof(one_channel_payload) == 62U,
               "one-channel payload size");
_Static_assert(sizeof(three_channel_payload) == 142U,
               "three-channel payload size");
_Static_assert(sizeof(common_one_channel_payload) == 38U,
               "common one-channel payload size");
_Static_assert(sizeof(common_three_channel_payload) == 78U,
               "common three-channel payload size");

static void bytes_copy(unsigned char *destination,
                       const unsigned char *source,
                       size_t size) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        destination[index] = source[index];
    }
}

static float absolute_float(float value) {
    return value < 0.0f ? -value : value;
}

static int near_float(float left, float right, float tolerance) {
    return absolute_float(left - right) <= tolerance;
}

static AvifdecStatus parse_bytes(const unsigned char *bytes,
                                 size_t size,
                                 size_t offset,
                                 AvifGainMapMetadata *metadata,
                                 AvifdecError *error) {
    AvifdecSpan span;

    span.data = bytes;
    span.size = size;
    span.file_offset = offset;
    return avif_gain_map_parse_spans(
        &span, 1U, offset, metadata, error);
}

static int test_exact_payloads(void) {
    AvifGainMapMetadata metadata;
    AvifdecError error;
    size_t channel;

    CHECK(parse_bytes(
        one_channel_payload, sizeof(one_channel_payload), 100U,
        &metadata, &error) == AVIFDEC_OK);
    CHECK(metadata.metadata_version == 0U);
    CHECK(metadata.minimum_version == 0U);
    CHECK(metadata.writer_version == 0U);
    CHECK(metadata.channel_count == 1U);
    CHECK(metadata.use_base_color_space == 1U);
    CHECK(metadata.backward_direction == 0U);
    CHECK(metadata.common_denominator == 0U);
    CHECK(metadata.base_hdr_headroom.numerator == 0);
    CHECK(metadata.base_hdr_headroom.denominator == 1U);
    CHECK(metadata.alternate_hdr_headroom.numerator == 3);
    CHECK(metadata.gain_map_min[0].numerator == -1);
    CHECK(metadata.gain_map_max[0].numerator == 2);
    CHECK(metadata.gain_map_gamma[0].numerator == 2);
    CHECK(metadata.base_offset[0].numerator == 1);
    CHECK(metadata.base_offset[0].denominator == 64U);
    CHECK(metadata.alternate_offset[0].numerator == -1);
    for (channel = 1U; channel < 3U; ++channel) {
        CHECK(metadata.gain_map_min[channel].numerator == -1);
        CHECK(metadata.gain_map_gamma[channel].numerator == 2);
        CHECK(metadata.alternate_offset[channel].denominator == 128U);
    }

    CHECK(parse_bytes(
        three_channel_payload, sizeof(three_channel_payload), 500U,
        &metadata, &error) == AVIFDEC_OK);
    CHECK(metadata.channel_count == 3U);
    CHECK(metadata.use_base_color_space == 0U);
    CHECK(metadata.base_hdr_headroom.numerator == 1);
    CHECK(metadata.base_hdr_headroom.denominator == 2U);
    CHECK(metadata.alternate_hdr_headroom.numerator == 9);
    CHECK(metadata.gain_map_min[0].numerator == -2);
    CHECK(metadata.gain_map_min[1].numerator == -1);
    CHECK(metadata.gain_map_min[2].numerator == 0);
    CHECK(metadata.gain_map_max[2].numerator == 5);
    CHECK(metadata.gain_map_gamma[1].numerator == 3);
    CHECK(metadata.gain_map_gamma[1].denominator == 2U);
    CHECK(metadata.base_offset[2].numerator == -1);
    CHECK(metadata.alternate_offset[2].numerator == 3);

    CHECK(parse_bytes(
        common_one_channel_payload,
        sizeof(common_one_channel_payload), 800U,
        &metadata, &error) == AVIFDEC_OK);
    CHECK(metadata.channel_count == 1U);
    CHECK(metadata.use_base_color_space == 1U);
    CHECK(metadata.backward_direction == 1U);
    CHECK(metadata.common_denominator == 1U);
    CHECK(metadata.base_hdr_headroom.numerator == 1300U);
    CHECK(metadata.base_hdr_headroom.denominator == 1000U);
    CHECK(metadata.alternate_hdr_headroom.numerator == 0U);
    CHECK(metadata.gain_map_min[0].numerator == -1000);
    CHECK(metadata.gain_map_max[0].numerator == 0);
    CHECK(metadata.gain_map_gamma[0].numerator == 1000U);
    CHECK(metadata.gain_map_gamma[0].denominator == 1000U);
    CHECK(metadata.base_offset[2].numerator == 16);

    CHECK(parse_bytes(
        common_three_channel_payload,
        sizeof(common_three_channel_payload), 900U,
        &metadata, &error) == AVIFDEC_OK);
    CHECK(metadata.channel_count == 3U);
    CHECK(metadata.common_denominator == 1U);
    CHECK(metadata.backward_direction == 0U);
    CHECK(metadata.gain_map_min[1].numerator == -500);
    CHECK(metadata.gain_map_max[1].numerator == 1500);
    CHECK(metadata.gain_map_max[2].numerator == 1200);
    return 0;
}

static void zero_u32(unsigned char *bytes, size_t offset) {
    bytes[offset + 0U] = 0U;
    bytes[offset + 1U] = 0U;
    bytes[offset + 2U] = 0U;
    bytes[offset + 3U] = 0U;
}

static void write_u32(unsigned char *bytes,
                      size_t offset,
                      uint32_t value) {
    bytes[offset + 0U] = (unsigned char)(value >> 24);
    bytes[offset + 1U] = (unsigned char)(value >> 16);
    bytes[offset + 2U] = (unsigned char)(value >> 8);
    bytes[offset + 3U] = (unsigned char)value;
}

static int test_payload_failures(void) {
    static const size_t denominator_offsets[17] = {
        10U, 18U, 26U, 34U, 42U, 50U, 58U,
        66U, 74U, 82U, 90U, 98U,
        106U, 114U, 122U, 130U, 138U
    };
    unsigned char bytes[143];
    AvifGainMapMetadata metadata;
    AvifdecError error;
    size_t index;

    for (index = 1U; index <= 255U; index += 254U) {
        bytes_copy(
            bytes, one_channel_payload, sizeof(one_channel_payload));
        bytes[0] = (unsigned char)index;
        CHECK(parse_bytes(bytes, 1U, 700U, &metadata, &error) ==
              AVIFDEC_UNSUPPORTED);
        CHECK(error.offset == 700U &&
              error.context == AVIF_GAIN_MAP_TMAP);
    }

    bytes_copy(bytes, one_channel_payload, sizeof(one_channel_payload));
    bytes[2] = 1U;
    CHECK(parse_bytes(bytes, 62U, 700U, &metadata, &error) ==
          AVIFDEC_UNSUPPORTED);
    CHECK(error.offset == 701U);
    bytes[4] = 1U;
    CHECK(parse_bytes(bytes, 62U, 700U, &metadata, &error) ==
          AVIFDEC_UNSUPPORTED);
    CHECK(error.offset == 701U);

    {
        static const unsigned int reserved_bits[4] = {
            0U, 1U, 4U, 5U
        };

        for (index = 0U; index < 4U; ++index) {
            bytes_copy(
                bytes, one_channel_payload,
                sizeof(one_channel_payload));
            bytes[5] |= (unsigned char)(
                (uint32_t)1U << reserved_bits[index]);
            CHECK(parse_bytes(bytes, 62U, 700U, &metadata, &error) ==
                  AVIFDEC_INVALID_DATA);
            CHECK(error.offset == 705U);
        }
    }
    bytes_copy(bytes, one_channel_payload, sizeof(one_channel_payload));
    bytes[5] |= 0x04U;
    CHECK(parse_bytes(bytes, 62U, 700U, &metadata, &error) ==
          AVIFDEC_OK);
    CHECK(metadata.backward_direction == 1U);

    for (index = 0U; index < 7U; ++index) {
        bytes_copy(bytes, one_channel_payload, sizeof(one_channel_payload));
        zero_u32(bytes, denominator_offsets[index]);
        CHECK(parse_bytes(bytes, 62U, 700U, &metadata, &error) ==
              AVIFDEC_INVALID_DATA);
        CHECK(error.offset == 700U + denominator_offsets[index]);
    }
    for (index = 0U; index < 17U; ++index) {
        bytes_copy(
            bytes, three_channel_payload,
            sizeof(three_channel_payload));
        zero_u32(bytes, denominator_offsets[index]);
        CHECK(parse_bytes(bytes, 142U, 900U, &metadata, &error) ==
              AVIFDEC_INVALID_DATA);
        CHECK(error.offset == 900U + denominator_offsets[index]);
    }

    bytes_copy(
        bytes, common_one_channel_payload,
        sizeof(common_one_channel_payload));
    zero_u32(bytes, 6U);
    CHECK(parse_bytes(bytes, 38U, 1000U, &metadata, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1006U);
    CHECK(metadata.channel_count == 0U);
    {
        static const size_t common_gamma_offsets[3] = {
            26U, 46U, 66U
        };

        for (index = 0U; index < 3U; ++index) {
            bytes_copy(
                bytes, common_three_channel_payload,
                sizeof(common_three_channel_payload));
            zero_u32(bytes, common_gamma_offsets[index]);
            CHECK(parse_bytes(bytes, 78U, 1100U, &metadata, &error) ==
                  AVIFDEC_INVALID_DATA);
            CHECK(error.offset ==
                  1100U + common_gamma_offsets[index]);
        }
    }

    {
        static const size_t gamma_offsets[3] = { 38U, 78U, 118U };

        for (index = 0U; index < 3U; ++index) {
            bytes_copy(
                bytes, three_channel_payload,
                sizeof(three_channel_payload));
            zero_u32(bytes, gamma_offsets[index]);
            CHECK(parse_bytes(bytes, 142U, 0U, &metadata, &error) ==
                  AVIFDEC_INVALID_DATA);
            CHECK(error.offset == gamma_offsets[index]);
        }
    }

    for (index = 0U; index < 2U; ++index) {
        size_t headroom_offset = index == 0U ? 6U : 14U;

        bytes_copy(
            bytes, one_channel_payload, sizeof(one_channel_payload));
        write_u32(bytes, headroom_offset, 0xffffffffU);
        CHECK(parse_bytes(bytes, 62U, 20U, &metadata, &error) ==
              AVIFDEC_OK);
        if (index == 0U) {
            CHECK(metadata.base_hdr_headroom.numerator ==
                  0xffffffffU);
        } else {
            CHECK(metadata.alternate_hdr_headroom.numerator ==
                  0xffffffffU);
        }
    }
    {
        static const size_t gamma_offsets[3] = { 38U, 78U, 118U };

        for (index = 0U; index < 3U; ++index) {
            bytes_copy(
                bytes, three_channel_payload,
                sizeof(three_channel_payload));
            write_u32(bytes, gamma_offsets[index], 0xffffffffU);
            CHECK(parse_bytes(bytes, 142U, 20U, &metadata, &error) ==
                  AVIFDEC_OK);
            CHECK(metadata.gain_map_gamma[index].numerator ==
                  0xffffffffU);
        }
    }

    {
        static const size_t minimum_offsets[3] = { 22U, 62U, 102U };
        static const size_t maximum_offsets[3] = { 30U, 70U, 110U };

        for (index = 0U; index < 3U; ++index) {
            bytes_copy(
                bytes, three_channel_payload,
                sizeof(three_channel_payload));
            write_u32(bytes, minimum_offsets[index], 0x7fffffffU);
            write_u32(bytes, maximum_offsets[index], 0x80000000U);
            CHECK(parse_bytes(bytes, 142U, 20U, &metadata, &error) ==
                  AVIFDEC_INVALID_DATA);
            CHECK(error.offset == 20U + maximum_offsets[index]);
        }
    }

    bytes_copy(bytes, one_channel_payload, sizeof(one_channel_payload));
    bytes[62] = 0x5aU;
    CHECK(parse_bytes(bytes, 63U, 100U, &metadata, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 162U);
    bytes[4] = 1U;
    CHECK(parse_bytes(bytes, 63U, 100U, &metadata, &error) ==
          AVIFDEC_OK);
    CHECK(metadata.writer_version == 1U);
    CHECK(parse_bytes(bytes, 62U, 100U, &metadata, &error) ==
          AVIFDEC_OK);

    bytes_copy(
        bytes, common_one_channel_payload,
        sizeof(common_one_channel_payload));
    bytes[38] = 0xa5U;
    CHECK(parse_bytes(bytes, 39U, 200U, &metadata, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 238U);
    bytes[4] = 1U;
    CHECK(parse_bytes(bytes, 39U, 200U, &metadata, &error) ==
          AVIFDEC_OK);

    for (index = 0U; index < sizeof(one_channel_payload); ++index) {
        CHECK(parse_bytes(
            one_channel_payload, index, 1234U,
            &metadata, &error) == AVIFDEC_TRUNCATED);
        CHECK(error.context == AVIF_GAIN_MAP_TMAP);
    }
    for (index = 0U;
         index < sizeof(common_one_channel_payload); ++index) {
        CHECK(parse_bytes(
            common_one_channel_payload, index, 1300U,
            &metadata, &error) == AVIFDEC_TRUNCATED);
    }
    return 0;
}

static int test_fragmented_payload(void) {
    AvifdecSpan spans[62];
    AvifGainMapMetadata metadata;
    AvifdecError error;
    unsigned char bytes[62];
    size_t index;

    bytes_copy(bytes, one_channel_payload, sizeof(bytes));
    for (index = 0U; index < 62U; ++index) {
        spans[index].data = &bytes[index];
        spans[index].size = 1U;
        spans[index].file_offset = 2000U + index * 3U;
    }
    CHECK(avif_gain_map_parse_spans(
        spans, 62U, 2000U, &metadata, &error) == AVIFDEC_OK);
    CHECK(metadata.alternate_hdr_headroom.numerator == 3);

    zero_u32(bytes, 42U);
    CHECK(avif_gain_map_parse_spans(
        spans, 62U, 2000U, &metadata, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 2000U + 42U * 3U);

    bytes_copy(bytes, one_channel_payload, sizeof(bytes));
    CHECK(avif_gain_map_parse_spans(
        spans, 61U, 2000U, &metadata, &error) ==
          AVIFDEC_TRUNCATED);
    CHECK(error.offset == spans[60].file_offset + 1U);

    spans[10].data = 0;
    CHECK(avif_gain_map_parse_spans(
        spans, 62U, 2000U, &metadata, &error) ==
          AVIFDEC_INVALID_ARGUMENT);
    CHECK(error.offset == spans[10].file_offset);

    for (index = 0U;
         index < sizeof(common_one_channel_payload); ++index) {
        spans[index].data = &common_one_channel_payload[index];
        spans[index].size = 1U;
        spans[index].file_offset = 3000U + index * 2U;
    }
    CHECK(avif_gain_map_parse_spans(
        spans, sizeof(common_one_channel_payload), 3000U,
        &metadata, &error) == AVIFDEC_OK);
    CHECK(metadata.common_denominator == 1U);
    CHECK(metadata.backward_direction == 1U);

    spans[0].data = one_channel_payload;
    spans[0].size = SIZE_MAX;
    spans[0].file_offset = 0U;
    spans[1].data = one_channel_payload;
    spans[1].size = 1U;
    spans[1].file_offset = 0U;
    CHECK(avif_gain_map_parse_spans(
        spans, 2U, 1U, &metadata, &error) == AVIFDEC_OVERFLOW);
    spans[0].size = 2U;
    spans[0].file_offset = SIZE_MAX - 1U;
    CHECK(avif_gain_map_parse_spans(
        spans, 1U, SIZE_MAX - 1U,
        &metadata, &error) == AVIFDEC_OVERFLOW);
    return 0;
}

static void object_zero(void *object, size_t size) {
    unsigned char *bytes = (unsigned char *)object;
    size_t index;

    for (index = 0U; index < size; ++index) bytes[index] = 0U;
}

typedef struct {
    AvifGainMapIndexedItem items[5];
    size_t item_count;
    uint32_t primary_id;
    uint32_t dimg_ids[3];
    size_t dimg_count;
    size_t dimg_offset;
    AvifGainMapAlternativeOrder alternative;
    size_t alternative_offset;
    AvifdecSpan payload_span;
    AvifdecImageInfo base_info;
    AvifdecImageInfo gain_info;
    AvifdecStatus query_status;
    size_t query_count;
} TestItemIndex;

static AvifdecStatus test_item_at(void *context,
                                  size_t item_index,
                                  AvifGainMapIndexedItem *item,
                                  AvifdecError *error) {
    TestItemIndex *index = (TestItemIndex *)context;

    (void)error;
    if (item_index >= index->item_count) return AVIFDEC_INVALID_ARGUMENT;
    *item = index->items[item_index];
    return AVIFDEC_OK;
}

static AvifdecStatus test_dimg(void *context,
                               uint32_t from_item_id,
                               uint32_t *ids,
                               size_t capacity,
                               size_t *count,
                               size_t *offset,
                               AvifdecError *error) {
    TestItemIndex *index = (TestItemIndex *)context;
    size_t copy_count;
    size_t item;

    (void)error;
    if (from_item_id != 20U && from_item_id != 21U) {
        return AVIFDEC_INVALID_DATA;
    }
    *count = index->dimg_count;
    *offset = index->dimg_offset;
    copy_count = index->dimg_count < capacity
        ? index->dimg_count : capacity;
    for (item = 0U; item < copy_count; ++item) {
        ids[item] = index->dimg_ids[item];
    }
    return AVIFDEC_OK;
}

static AvifdecStatus test_alternative(
    void *context,
    uint32_t first_item_id,
    uint32_t second_item_id,
    AvifGainMapAlternativeOrder *order,
    size_t *offset,
    AvifdecError *error) {
    TestItemIndex *index = (TestItemIndex *)context;

    (void)first_item_id;
    (void)second_item_id;
    (void)error;
    *order = index->alternative;
    *offset = index->alternative_offset;
    return AVIFDEC_OK;
}

static AvifdecStatus test_payload_span_at(
    void *context,
    size_t span_index,
    AvifdecSpan *span,
    AvifdecError *error) {
    const AvifdecSpan *payload = (const AvifdecSpan *)context;

    (void)error;
    if (span_index != 0U) return AVIFDEC_INVALID_ARGUMENT;
    *span = *payload;
    return AVIFDEC_OK;
}

static AvifdecStatus test_item_payload(
    void *context,
    uint32_t item_id,
    AvifGainMapSpanSource *source,
    AvifdecError *error) {
    TestItemIndex *index = (TestItemIndex *)context;

    (void)error;
    if (item_id != 20U && item_id != 21U) {
        return AVIFDEC_INVALID_DATA;
    }
    source->context = &index->payload_span;
    source->span_count = 1U;
    source->payload_offset = index->payload_span.file_offset;
    source->span_at = test_payload_span_at;
    return AVIFDEC_OK;
}

static AvifdecStatus test_query_child(
    void *context,
    uint32_t item_id,
    const AvifdecExecutor *executor,
    AvifdecImageInfo *info,
    AvifdecError *error) {
    TestItemIndex *index = (TestItemIndex *)context;

    (void)executor;
    (void)error;
    ++index->query_count;
    if (index->query_status != AVIFDEC_OK) {
        return index->query_status;
    }
    if (item_id == 10U) {
        *info = index->base_info;
    } else if (item_id == 30U) {
        *info = index->gain_info;
    } else {
        return AVIFDEC_INVALID_DATA;
    }
    return AVIFDEC_OK;
}

static AvifGainMapItemIndex test_index_adapter(TestItemIndex *test) {
    AvifGainMapItemIndex index;

    object_zero(&index, sizeof(index));
    index.context = test;
    index.primary_item_id = test->primary_id;
    index.item_count = test->item_count;
    index.item_at = test_item_at;
    index.dimg = test_dimg;
    index.alternative_order = test_alternative;
    index.item_payload = test_item_payload;
    index.query_child = test_query_child;
    return index;
}

static AvifdecImageInfo test_image_info(uint32_t width,
                                        uint32_t height,
                                        uint8_t monochrome) {
    AvifdecImageInfo info;

    object_zero(&info, sizeof(info));
    info.width = width;
    info.height = height;
    info.presentation_width = width;
    info.presentation_height = height;
    info.render_width = width;
    info.render_height = height;
    info.crop.width = width;
    info.crop.height = height;
    info.bit_depth = 8U;
    info.monochrome = monochrome;
    info.channel_count = monochrome != 0U ? 1U : 3U;
    info.color_range = 1U;
    return info;
}

static void init_test_index(TestItemIndex *test) {
    object_zero(test, sizeof(*test));
    test->item_count = 3U;
    test->primary_id = 10U;
    test->items[0].id = 10U;
    test->items[0].type =
        AVIF_GAIN_MAP_FOURCC('a', 'v', '0', '1');
    test->items[0].source_offset = 100U;
    test->items[0].color.has_nclx = 1U;
    test->items[0].color.color_range = 1U;
    test->items[0].color.color_primaries = 1U;
    test->items[0].color.transfer_characteristics = 13U;

    test->items[1].id = 20U;
    test->items[1].type = AVIF_GAIN_MAP_TMAP;
    test->items[1].source_offset = 200U;
    test->items[1].properties.width = 4U;
    test->items[1].properties.height = 2U;
    test->items[1].color.has_nclx = 1U;
    test->items[1].color.color_range = 1U;
    test->items[1].color.color_primaries = 9U;
    test->items[1].color.transfer_characteristics = 16U;

    test->items[2].id = 30U;
    test->items[2].type =
        AVIF_GAIN_MAP_FOURCC('a', 'v', '0', '1');
    test->items[2].source_offset = 300U;
    test->items[2].hidden = 1U;
    test->dimg_ids[0] = 10U;
    test->dimg_ids[1] = 30U;
    test->dimg_count = 2U;
    test->dimg_offset = 400U;
    test->alternative =
        AVIF_GAIN_MAP_ALTERNATIVE_FIRST_BEFORE_SECOND;
    test->alternative_offset = 500U;
    test->payload_span.data = one_channel_payload;
    test->payload_span.size = sizeof(one_channel_payload);
    test->payload_span.file_offset = 600U;
    test->base_info = test_image_info(4U, 2U, 0U);
    test->base_info.workspace_required = 101U;
    test->base_info.has_nclx = 1U;
    test->base_info.color_primaries = 1U;
    test->base_info.transfer_characteristics = 13U;
    test->base_info.matrix_coefficients = 0U;
    test->gain_info = test_image_info(2U, 1U, 1U);
    test->gain_info.workspace_required = 203U;
    test->query_status = AVIFDEC_OK;
}

static int test_discovery_plans(void) {
    TestItemIndex test;
    AvifGainMapItemIndex index;
    AvifGainMapDecodePlan plan;
    AvifdecError error;

    init_test_index(&test);
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    CHECK(plan.info.present == 1U);
    CHECK(plan.info.base_item_id == 10U);
    CHECK(plan.info.alternate_item_id == 20U);
    CHECK(plan.info.gain_map_item_id == 30U);
    CHECK(plan.info.workspace_required == 203U);
    CHECK(plan.info.metadata.channel_count == 1U);
    CHECK(plan.info.base_is_hdr == 0U);
    CHECK(plan.info.base_color.color_primaries == 1U);
    CHECK(plan.info.alternate_color.color_primaries == 9U);
    CHECK(test.query_count == 2U);

    init_test_index(&test);
    object_zero(&test.items[0].color, sizeof(test.items[0].color));
    test.base_info.color_primaries = 9U;
    test.base_info.transfer_characteristics = 16U;
    test.base_info.matrix_coefficients = 9U;
    test.base_info.color_range = 1U;
    test.base_info.has_nclx = 1U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    CHECK(plan.info.base_color.color_primaries == 9U);
    CHECK(plan.info.base_color.transfer_characteristics == 16U);
    CHECK(plan.info.base_color.matrix_coefficients == 9U);
    CHECK(plan.info.base_color.has_nclx == 1U);

    init_test_index(&test);
    test.payload_span.data = common_one_channel_payload;
    test.payload_span.size = sizeof(common_one_channel_payload);
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    CHECK(plan.info.base_is_hdr == 1U);
    CHECK(plan.info.metadata.backward_direction == 1U);

    init_test_index(&test);
    test.alternative = AVIF_GAIN_MAP_ALTERNATIVE_NONE;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    CHECK(plan.info.present == 0U && test.query_count == 0U);

    test.alternative =
        AVIF_GAIN_MAP_ALTERNATIVE_SECOND_BEFORE_FIRST;
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    CHECK(plan.info.present == 0U);

    init_test_index(&test);
    test.primary_id = 20U;
    test.alternative = AVIF_GAIN_MAP_ALTERNATIVE_NONE;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    CHECK(plan.info.present == 1U && plan.info.base_item_id == 10U);

    init_test_index(&test);
    test.dimg_count = 1U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 400U && error.context == AVIF_GAIN_MAP_DIMG);
    test.dimg_count = 3U;
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);
    test.dimg_count = 2U;
    test.dimg_ids[1] = 10U;
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);

    init_test_index(&test);
    test.dimg_ids[0] = 30U;
    test.dimg_ids[1] = 10U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);

    init_test_index(&test);
    test.items[2].hidden = 0U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 300U);

    init_test_index(&test);
    test.items[1].hidden = 1U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 200U);

    init_test_index(&test);
    test.items[1].properties.transform_flags = AVIFDEC_TRANSFORM_IROT;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);

    init_test_index(&test);
    test.gain_info.transform_flags = AVIFDEC_TRANSFORM_IROT;
    test.gain_info.irot_angle = 1U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);

    init_test_index(&test);
    test.base_info.transform_flags = AVIFDEC_TRANSFORM_IROT;
    test.base_info.irot_angle = 1U;
    test.gain_info.transform_flags = AVIFDEC_TRANSFORM_IROT;
    test.gain_info.irot_angle = 1U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);

    init_test_index(&test);
    test.items[1].properties.width = 5U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);

    init_test_index(&test);
    test.payload_span.data = three_channel_payload;
    test.payload_span.size = sizeof(three_channel_payload);
    test.gain_info = test_image_info(2U, 1U, 0U);
    test.gain_info.workspace_required = 303U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    CHECK(plan.info.metadata.channel_count == 3U);
    CHECK(plan.info.workspace_required == 303U);

    init_test_index(&test);
    test.item_count = 4U;
    test.items[3] = test.items[1];
    test.items[3].id = 21U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.context == AVIF_GAIN_MAP_ALTR);

    init_test_index(&test);
    test.items[1].has_unsupported_essential_property = 1U;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_UNSUPPORTED);

    init_test_index(&test);
    test.query_status = AVIFDEC_LIMIT_EXCEEDED;
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_LIMIT_EXCEEDED);
    CHECK(plan.info.present == 0U);
    CHECK(plan.info.base_item_id == 0U);
    CHECK(plan.info.metadata.channel_count == 0U);
    return 0;
}

typedef struct {
    uint16_t base_plane[3][32];
    uint16_t gain_plane[3][32];
    uint16_t alpha[32];
} TestPlaneStorage;

static void bind_test_image(const AvifdecImageInfo *info,
                            uint16_t storage[3][32],
                            uint16_t *alpha,
                            AvifdecImage *image) {
    uint32_t chroma_width;
    uint32_t chroma_height;

    object_zero(image, sizeof(*image));
    image->planes[0] = storage[0];
    image->strides[0] = info->width;
    image->widths[0] = info->width;
    image->heights[0] = info->height;
    if (info->monochrome == 0U) {
        chroma_width =
            ((info->width - 1U) >> info->subsampling_x) + 1U;
        chroma_height =
            ((info->height - 1U) >> info->subsampling_y) + 1U;
        image->planes[1] = storage[1];
        image->planes[2] = storage[2];
        image->strides[1] = chroma_width;
        image->strides[2] = chroma_width;
        image->widths[1] = chroma_width;
        image->widths[2] = chroma_width;
        image->heights[1] = chroma_height;
        image->heights[2] = chroma_height;
    }
    image->bit_depth = info->bit_depth;
    image->monochrome = info->monochrome;
    image->subsampling_x = info->subsampling_x;
    image->subsampling_y = info->subsampling_y;
    if (info->has_alpha != 0U) {
        image->alpha_plane = alpha;
        image->alpha_stride = info->width;
        image->alpha_width = info->width;
        image->alpha_height = info->height;
        image->alpha_bit_depth = info->alpha_bit_depth;
        image->alpha_color_range = info->alpha_color_range;
        image->alpha_premultiplied = info->alpha_premultiplied;
    }
}

typedef struct {
    uint32_t ids[2];
    size_t workspace_sizes[2];
    size_t call_count;
    AvifdecStatus status;
} TestDecoder;

static AvifdecStatus test_decode_child(
    void *context,
    uint32_t item_id,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecError *error) {
    TestDecoder *decoder = (TestDecoder *)context;

    (void)executor;
    (void)workspace;
    (void)image;
    (void)trace;
    (void)error;
    if (decoder->call_count < 2U) {
        decoder->ids[decoder->call_count] = item_id;
        decoder->workspace_sizes[decoder->call_count] = workspace_size;
    }
    ++decoder->call_count;
    return decoder->status;
}

static int test_workspace_and_planes(void) {
    TestItemIndex test;
    AvifGainMapItemIndex index;
    AvifGainMapDecodePlan plan;
    TestPlaneStorage storage;
    AvifdecImage base;
    AvifdecImage gain;
    AvifGainMapChildDecoder child;
    TestDecoder decoder;
    AvifdecError error;
    unsigned char workspace[203];
    size_t required = 0U;

    object_zero(&storage, sizeof(storage));
    init_test_index(&test);
    index = test_index_adapter(&test);
    CHECK(avif_gain_map_query_decode_plan(
        &index, 0, &plan, &error) == AVIFDEC_OK);
    bind_test_image(
        &plan.info.base_image, storage.base_plane, storage.alpha,
        &base);
    bind_test_image(
        &plan.info.gain_map_image, storage.gain_plane, 0, &gain);
    CHECK(avif_gain_map_validate_decode_images(
        &plan, &base, &gain, &error) == AVIFDEC_OK);
    CHECK(avif_gain_map_child_workspace(
        &plan.info.base_image, &plan.info.gain_map_image,
        &required) == AVIFDEC_OK);
    CHECK(required == 203U);

    base.strides[0] = 3U;
    CHECK(avif_gain_map_validate_decode_images(
        &plan, &base, &gain, &error) == AVIFDEC_INVALID_ARGUMENT);
    base.strides[0] = 4U;
    gain.planes[0] = 0;
    CHECK(avif_gain_map_validate_decode_images(
        &plan, &base, &gain, &error) == AVIFDEC_INVALID_ARGUMENT);
    gain.planes[0] = storage.gain_plane[0];
    gain.alpha_plane = storage.alpha;
    CHECK(avif_gain_map_validate_decode_images(
        &plan, &base, &gain, &error) == AVIFDEC_INVALID_ARGUMENT);
    gain.alpha_plane = 0;

    object_zero(&decoder, sizeof(decoder));
    decoder.status = AVIFDEC_OK;
    child.context = &decoder;
    child.decode = test_decode_child;
    CHECK(avif_gain_map_execute_decode_plan(
        &plan, &child, 0, workspace, sizeof(workspace) - 1U,
        &base, &gain, 0, 0, &error) == AVIFDEC_OUT_OF_MEMORY);
    CHECK(decoder.call_count == 0U);
    CHECK(avif_gain_map_execute_decode_plan(
        &plan, &child, 0, 0, sizeof(workspace),
        &base, &gain, 0, 0, &error) == AVIFDEC_INVALID_ARGUMENT);
    CHECK(avif_gain_map_execute_decode_plan(
        &plan, &child, 0, workspace, sizeof(workspace),
        &base, &gain, 0, 0, &error) == AVIFDEC_OK);
    CHECK(decoder.call_count == 2U);
    CHECK(decoder.ids[0] == 10U && decoder.ids[1] == 30U);
    CHECK(decoder.workspace_sizes[0] == 203U);
    CHECK(decoder.workspace_sizes[1] == 203U);

    object_zero(&decoder, sizeof(decoder));
    decoder.status = AVIFDEC_INVALID_DATA;
    CHECK(avif_gain_map_execute_decode_plan(
        &plan, &child, 0, workspace, sizeof(workspace),
        &base, &gain, 0, 0, &error) == AVIFDEC_INVALID_DATA);
    CHECK(decoder.call_count == 1U);
    return 0;
}

typedef union {
    float value;
    uint32_t bits;
} TestFloatBits;

typedef struct {
    float base[32][4];
    float gain[32][3];
    uint32_t base_width;
    uint32_t gain_width;
    uint16_t expected_working_primaries;
    uint16_t validated_working_primaries;
    uint32_t gain_x[256];
    uint32_t gain_y[256];
    size_t gain_call_count;
    uint32_t last_base_y;
    char events[512];
    size_t event_count;
    int return_nan;
    size_t base_write_count;
    size_t gain_write_count;
    size_t linear_write_count;
    AvifdecStatus base_status;
    AvifdecStatus gain_status;
    AvifdecStatus linear_status;
    float linear_scale;
    int base_sets_error;
} TestColor;

static void test_color_event(TestColor *color, char event) {
    if (color->event_count < sizeof(color->events)) {
        color->events[color->event_count] = event;
    }
    ++color->event_count;
}

static AvifdecStatus test_validate_transform(
    void *context,
    const AvifGainMapColorDescription *working_color,
    uint8_t output_format,
    AvifdecError *error) {
    TestColor *color = (TestColor *)context;

    (void)output_format;
    (void)error;
    test_color_event(color, 'V');
    color->validated_working_primaries =
        working_color->color_primaries;
    if (color->expected_working_primaries != 0U &&
        color->expected_working_primaries !=
            working_color->color_primaries) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus test_base_to_working(
    void *context,
    const AvifdecImage *base_image,
    const AvifdecImageInfo *base_info,
    const AvifGainMapColorDescription *working_color,
    uint32_t x,
    uint32_t y,
    float rgba[4],
    AvifdecError *error) {
    TestColor *color = (TestColor *)context;
    size_t channel;
    size_t pixel = (size_t)y * color->base_width + x;

    (void)base_image;
    (void)base_info;
    (void)working_color;
    test_color_event(color, 'B');
    color->last_base_y = y;
    for (channel = 0U;
         channel < color->base_write_count && channel < 4U;
         ++channel) {
        rgba[channel] = color->base[pixel][channel];
    }
    if (color->return_nan) {
        TestFloatBits bits;

        bits.bits = 0x7fc00000U;
        rgba[0] = bits.value;
    }
    if (color->base_sets_error && error != 0) {
        error->status = AVIFDEC_INVALID_DATA;
        error->offset = 17U;
        error->context = AVIF_GAIN_MAP_TMAP;
    }
    return color->base_status;
}

static AvifdecStatus test_gain_texel(
    void *context,
    const AvifdecImage *gain_map_image,
    const AvifdecImageInfo *gain_map_info,
    uint32_t x,
    uint32_t y,
    uint8_t channel_count,
    float gain[3],
    AvifdecError *error) {
    TestColor *color = (TestColor *)context;
    size_t channel;
    size_t pixel = (size_t)y * color->gain_width + x;

    (void)gain_map_image;
    (void)gain_map_info;
    (void)error;
    test_color_event(color, 'G');
    if (color->gain_call_count <
        sizeof(color->gain_x) / sizeof(color->gain_x[0])) {
        color->gain_x[color->gain_call_count] = x;
        color->gain_y[color->gain_call_count] = y;
    }
    ++color->gain_call_count;
    for (channel = 0U;
         channel < channel_count &&
             channel < color->gain_write_count;
         ++channel) {
        gain[channel] = color->gain[pixel][channel];
    }
    return color->gain_status;
}

static AvifdecStatus test_working_to_linear(
    void *context,
    const float working_rgb[3],
    float output_rgb[3],
    AvifdecError *error) {
    TestColor *color = (TestColor *)context;

    (void)error;
    test_color_event(color, 'F');
    if (color->linear_write_count > 0U) {
        output_rgb[0] = working_rgb[0] * color->linear_scale;
    }
    if (color->linear_write_count > 1U) {
        output_rgb[1] = working_rgb[1] * color->linear_scale;
    }
    if (color->linear_write_count > 2U) {
        output_rgb[2] = working_rgb[2] * color->linear_scale;
    }
    return color->linear_status;
}

static uint16_t test_encode_u16(float value) {
    if (value <= 0.0f) return 0U;
    if (value >= 1.0f) return 65535U;
    return (uint16_t)((uint32_t)((double)value * 65535.0 + 0.5));
}

static AvifdecStatus test_working_to_encoded16(
    void *context,
    const float working_rgb[3],
    uint16_t output_rgb[3],
    AvifdecError *error) {
    TestColor *color = (TestColor *)context;

    (void)error;
    test_color_event(color, 'U');
    output_rgb[0] = test_encode_u16(working_rgb[0]);
    output_rgb[1] = test_encode_u16(working_rgb[1]);
    output_rgb[2] = test_encode_u16(working_rgb[2]);
    return AVIFDEC_OK;
}

static AvifGainMapColorAdapter test_color_adapter(TestColor *test) {
    AvifGainMapColorAdapter color;

    object_zero(&color, sizeof(color));
    color.context = test;
    color.validate_transform = test_validate_transform;
    color.base_to_working = test_base_to_working;
    color.gain_texel = test_gain_texel;
    color.working_to_linear = test_working_to_linear;
    color.working_to_encoded16 = test_working_to_encoded16;
    return color;
}

static AvifGainMapRational test_rational(int32_t numerator,
                                         uint32_t denominator) {
    AvifGainMapRational rational;

    rational.numerator = numerator;
    rational.denominator = denominator;
    return rational;
}

static AvifGainMapUnsignedRational test_unsigned_rational(
    uint32_t numerator,
    uint32_t denominator) {
    AvifGainMapUnsignedRational rational;

    rational.numerator = numerator;
    rational.denominator = denominator;
    return rational;
}

typedef struct {
    AvifGainMapInfo info;
    TestPlaneStorage planes;
    AvifdecImage base_image;
    AvifdecImage gain_image;
    TestColor color;
    AvifGainMapColorAdapter adapter;
    AvifGainMapApplyOptions options;
} TestApply;

static void init_apply(TestApply *test,
                       uint32_t width,
                       uint32_t height,
                       uint32_t gain_width,
                       uint32_t gain_height,
                       uint8_t gain_channels) {
    size_t channel;
    size_t pixel;

    object_zero(test, sizeof(*test));
    test->info.present = 1U;
    test->info.base_image = test_image_info(width, height, 0U);
    test->info.base_image.has_alpha = 1U;
    test->info.base_image.alpha_bit_depth = 8U;
    test->info.base_image.alpha_color_range = 1U;
    test->info.gain_map_image = test_image_info(
        gain_width, gain_height,
        gain_channels == 1U ? 1U : 0U);
    test->info.metadata.metadata_version = 0U;
    test->info.metadata.minimum_version = 0U;
    test->info.metadata.writer_version = 0U;
    test->info.metadata.channel_count = gain_channels;
    test->info.metadata.use_base_color_space = 1U;
    test->info.metadata.base_hdr_headroom =
        test_unsigned_rational(0U, 1U);
    test->info.metadata.alternate_hdr_headroom =
        test_unsigned_rational(2U, 1U);
    for (channel = 0U; channel < 3U; ++channel) {
        test->info.metadata.gain_map_min[channel] =
            test_rational(0, 1U);
        test->info.metadata.gain_map_max[channel] =
            test_rational(1, 1U);
        test->info.metadata.gain_map_gamma[channel] =
            test_unsigned_rational(1U, 1U);
        test->info.metadata.base_offset[channel] =
            test_rational(0, 1U);
        test->info.metadata.alternate_offset[channel] =
            test_rational(0, 1U);
    }
    test->info.base_color.has_nclx = 1U;
    test->info.base_color.color_range = 1U;
    test->info.base_color.color_primaries = 1U;
    test->info.base_color.transfer_characteristics = 13U;
    test->info.alternate_color.has_nclx = 1U;
    test->info.alternate_color.color_range = 1U;
    test->info.alternate_color.color_primaries = 9U;
    test->info.alternate_color.transfer_characteristics = 16U;
    bind_test_image(
        &test->info.base_image, test->planes.base_plane,
        test->planes.alpha, &test->base_image);
    bind_test_image(
        &test->info.gain_map_image, test->planes.gain_plane,
        0, &test->gain_image);
    test->color.base_width = width;
    test->color.gain_width = gain_width;
    test->color.base_write_count = 4U;
    test->color.gain_write_count = 3U;
    test->color.linear_write_count = 3U;
    test->color.base_status = AVIFDEC_OK;
    test->color.gain_status = AVIFDEC_OK;
    test->color.linear_status = AVIFDEC_OK;
    test->color.linear_scale = 1.0f;
    test->color.expected_working_primaries = 1U;
    for (pixel = 0U; pixel < (size_t)width * height; ++pixel) {
        test->color.base[pixel][0] = 0.25f;
        test->color.base[pixel][1] = 0.5f;
        test->color.base[pixel][2] = 0.75f;
        test->color.base[pixel][3] = 1.0f;
    }
    for (pixel = 0U;
         pixel < (size_t)gain_width * gain_height; ++pixel) {
        test->color.gain[pixel][0] = 1.0f;
        test->color.gain[pixel][1] = 1.0f;
        test->color.gain[pixel][2] = 1.0f;
    }
    test->adapter = test_color_adapter(&test->color);
    test->options.display_headroom = 4.0f;
}

static void reset_color_events(TestColor *color) {
    color->event_count = 0U;
    color->gain_call_count = 0U;
}

static AvifdecRgbImage float_output_image(void *pixels,
                                          size_t stride,
                                          uint32_t width,
                                          uint32_t height,
                                          uint8_t format,
                                          uint8_t alpha_mode) {
    AvifdecRgbImage output;

    object_zero(&output, sizeof(output));
    output.pixels = pixels;
    output.stride = stride;
    output.width = width;
    output.height = height;
    output.format = format;
    output.alpha_mode = alpha_mode;
    return output;
}

static int test_headroom_and_channel_math(void) {
    TestApply test;
    float pixels[4];
    AvifdecRgbImage output;
    AvifdecError error;
    const float square_root_two = 1.41421356237f;

    init_apply(&test, 1U, 1U, 1U, 1U, 1U);
    output = float_output_image(
        pixels, 3U * sizeof(float), 1U, 1U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);

    test.options.display_headroom = 1.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(pixels[0] == 0.25f && pixels[1] == 0.5f &&
          pixels[2] == 0.75f);
    CHECK(test.color.gain_call_count == 0U);
    test.color.linear_scale = 2.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(pixels[0] == 0.5f && pixels[1] == 1.0f &&
          pixels[2] == 1.5f);
    test.color.linear_scale = 1.0f;

    reset_color_events(&test.color);
    test.options.display_headroom = 4.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 0.5f, 0.000002f));
    CHECK(near_float(pixels[1], 1.0f, 0.000002f));
    CHECK(near_float(pixels[2], 1.5f, 0.000003f));
    CHECK(test.color.event_count == 4U);
    CHECK(test.color.events[0] == 'V' && test.color.events[1] == 'B' &&
          test.color.events[2] == 'G' && test.color.events[3] == 'F');

    test.options.display_headroom = 2.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(
        pixels[0], 0.25f * square_root_two, 0.000003f));
    CHECK(near_float(
        pixels[1], 0.5f * square_root_two, 0.000003f));

    reset_color_events(&test.color);
    test.info.metadata.alternate_hdr_headroom =
        test.info.metadata.base_hdr_headroom;
    test.options.display_headroom = 8.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(pixels[0] == 0.25f && pixels[1] == 0.5f);
    CHECK(test.color.gain_call_count == 0U);

    test.info.metadata.base_hdr_headroom =
        test_unsigned_rational(2U, 1U);
    test.info.metadata.alternate_hdr_headroom =
        test_unsigned_rational(0U, 1U);
    test.info.metadata.backward_direction = 1U;
    test.info.base_is_hdr = 1U;
    test.options.display_headroom = 4.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(pixels[0] == 0.25f);
    test.options.display_headroom = 1.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 0.125f, 0.000002f));
    CHECK(near_float(pixels[1], 0.25f, 0.000002f));
    test.color.base[0][0] = 1.0f;
    test.info.metadata.base_offset[0] = test_rational(1, 5U);
    test.info.metadata.alternate_offset[0] =
        test_rational(1, 10U);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 0.5f, 0.000003f));
    test.options.display_headroom = 2.0f;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(
        pixels[0], 1.2f / square_root_two - 0.1f, 0.000004f));

    init_apply(&test, 1U, 1U, 1U, 1U, 1U);
    output = float_output_image(
        pixels, 3U * sizeof(float), 1U, 1U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);
    test.color.base[0][0] = 1.0f;
    test.info.metadata.backward_direction = 1U;
    test.info.base_is_hdr = 1U;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 0.5f, 0.000003f));

    init_apply(&test, 1U, 1U, 1U, 1U, 1U);
    output = float_output_image(
        pixels, 3U * sizeof(float), 1U, 1U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);
    test.color.gain[0][0] = 0.25f;
    test.info.metadata.gain_map_gamma[0] =
        test_unsigned_rational(2U, 1U);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(
        pixels[0], 0.25f * square_root_two, 0.000003f));

    test.color.gain[0][0] = 1.0f;
    test.info.metadata.gain_map_gamma[0] =
        test_unsigned_rational(1U, 1U);
    test.info.metadata.base_offset[0] = test_rational(1, 10U);
    test.info.metadata.alternate_offset[0] =
        test_rational(1, 5U);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 0.5f, 0.000003f));

    init_apply(&test, 1U, 1U, 1U, 1U, 3U);
    output = float_output_image(
        pixels, 3U * sizeof(float), 1U, 1U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);
    test.info.metadata.gain_map_max[0] = test_rational(1, 1U);
    test.info.metadata.gain_map_max[1] = test_rational(2, 1U);
    test.info.metadata.gain_map_min[2] = test_rational(-2, 1U);
    test.info.metadata.gain_map_max[2] = test_rational(-1, 1U);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 0.5f, 0.000003f));
    CHECK(near_float(pixels[1], 2.0f, 0.000004f));
    CHECK(near_float(pixels[2], 0.375f, 0.000003f));

    test.info.metadata.use_base_color_space = 0U;
    test.color.expected_working_primaries = 9U;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(test.color.validated_working_primaries == 9U);
    return 0;
}

static int test_bilinear_sampling(void) {
    TestApply test;
    float pixels[3U * 3U * 3U];
    AvifdecRgbImage output;
    AvifdecError error;
    size_t pixel;
    const float square_root_two = 1.41421356237f;

    init_apply(&test, 3U, 3U, 2U, 2U, 1U);
    for (pixel = 0U; pixel < 9U; ++pixel) {
        test.color.base[pixel][0] = 1.0f;
        test.color.base[pixel][1] = 1.0f;
        test.color.base[pixel][2] = 1.0f;
    }
    test.color.gain[0][0] = 0.0f;
    test.color.gain[1][0] = 1.0f;
    test.color.gain[2][0] = 1.0f;
    test.color.gain[3][0] = 0.0f;
    output = float_output_image(
        pixels, 3U * 3U * sizeof(float), 3U, 3U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0U * 9U + 0U], 1.0f, 0.000002f));
    CHECK(near_float(pixels[0U * 9U + 6U], 2.0f, 0.000003f));
    CHECK(near_float(pixels[2U * 9U + 0U], 2.0f, 0.000003f));
    CHECK(near_float(pixels[2U * 9U + 6U], 1.0f, 0.000002f));
    CHECK(near_float(pixels[1U * 9U + 3U],
                     square_root_two, 0.000004f));
    CHECK(near_float(pixels[0U * 9U + 3U],
                     square_root_two, 0.000004f));

    init_apply(&test, 4U, 2U, 2U, 1U, 1U);
    for (pixel = 0U; pixel < 8U; ++pixel) {
        test.color.base[pixel][0] = 1.0f;
        test.color.base[pixel][1] = 1.0f;
        test.color.base[pixel][2] = 1.0f;
    }
    test.color.gain[0][0] = 0.0f;
    test.color.gain[1][0] = 1.0f;
    output = float_output_image(
        pixels, 4U * 3U * sizeof(float), 4U, 2U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 1.0f, 0.000002f));
    CHECK(near_float(pixels[3], 1.189207115f, 0.000004f));
    CHECK(near_float(pixels[6], 1.681792831f, 0.000005f));
    CHECK(near_float(pixels[9], 2.0f, 0.000004f));
    CHECK(test.color.gain_x[0] == 0U && test.color.gain_y[0] == 0U);

    init_apply(&test, 2U, 1U, 4U, 1U, 1U);
    for (pixel = 0U; pixel < 2U; ++pixel) {
        test.color.base[pixel][0] = 1.0f;
        test.color.base[pixel][1] = 1.0f;
        test.color.base[pixel][2] = 1.0f;
    }
    test.color.gain[0][0] = 0.0f;
    test.color.gain[1][0] = 0.5f;
    test.color.gain[2][0] = 0.5f;
    test.color.gain[3][0] = 1.0f;
    output = float_output_image(
        pixels, 2U * 3U * sizeof(float), 2U, 1U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(pixels[0], 1.189207115f, 0.000004f));
    CHECK(near_float(pixels[3], 1.681792831f, 0.000005f));
    return 0;
}

static uint64_t test_u16_checksum(const uint16_t *values,
                                  size_t count) {
    uint64_t checksum = 1469598103934665603ULL;
    size_t index;

    for (index = 0U; index < count; ++index) {
        checksum ^= values[index];
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

static int test_alpha_and_outputs(void) {
    TestApply test;
    float float_pixels[12];
    uint16_t u16_pixels[12];
    AvifdecRgbImage output;
    AvifdecError error;
    size_t pixel;

    init_apply(&test, 3U, 1U, 1U, 1U, 1U);
    for (pixel = 0U; pixel < 3U; ++pixel) {
        test.color.base[pixel][0] = 0.5f;
        test.color.base[pixel][1] = 0.5f;
        test.color.base[pixel][2] = 0.5f;
    }
    test.color.base[0][3] = 0.0f;
    test.color.base[1][3] = 0.5f;
    test.color.base[2][3] = 1.0f;

    output = float_output_image(
        float_pixels, 12U * sizeof(float), 3U, 1U,
        AVIF_GAIN_MAP_RGBAF32, AVIFDEC_ALPHA_STRAIGHT);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(near_float(float_pixels[0], 1.0f, 0.000003f));
    CHECK(float_pixels[3] == 0.0f);
    CHECK(near_float(float_pixels[4], 1.0f, 0.000003f));
    CHECK(float_pixels[7] == 0.5f);
    CHECK(float_pixels[11] == 1.0f);

    output.alpha_mode = AVIFDEC_ALPHA_PREMULTIPLIED;
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(float_pixels[0] == 0.0f);
    CHECK(near_float(float_pixels[4], 0.5f, 0.000003f));
    CHECK(near_float(float_pixels[8], 1.0f, 0.000003f));
    CHECK(float_pixels[3] == 0.0f &&
          float_pixels[7] == 0.5f &&
          float_pixels[11] == 1.0f);

    output = float_output_image(
        u16_pixels, 12U * sizeof(uint16_t), 3U, 1U,
        AVIFDEC_RGBA16, AVIFDEC_ALPHA_PREMULTIPLIED);
    CHECK(avif_gain_map_apply(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, &error) == AVIFDEC_OK);
    CHECK(u16_pixels[0] == 0U && u16_pixels[3] == 0U);
    CHECK(u16_pixels[4] == 32768U && u16_pixels[7] == 32768U);
    CHECK(u16_pixels[8] == 65535U && u16_pixels[11] == 65535U);
    CHECK(test_u16_checksum(u16_pixels, 12U) ==
          0x72631edaaa85b4efULL);
    return 0;
}

static int test_rows_and_finiteness(void) {
    TestApply test;
    float pixels[6];
    AvifdecRgbImage output;
    AvifdecError error;

    init_apply(&test, 1U, 2U, 1U, 1U, 1U);
    pixels[0] = 88.0f;
    pixels[1] = 88.0f;
    pixels[2] = 88.0f;
    pixels[3] = 99.0f;
    pixels[4] = 99.0f;
    pixels[5] = 99.0f;
    output = float_output_image(
        pixels, 3U * sizeof(float), 1U, 2U,
        AVIF_GAIN_MAP_RGBF32, AVIFDEC_ALPHA_STRAIGHT);
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 1U,
        &error) == AVIFDEC_OK);
    CHECK(test.color.last_base_y == 1U);
    CHECK(near_float(pixels[0], 0.5f, 0.000003f));
    CHECK(pixels[3] == 99.0f && pixels[4] == 99.0f &&
          pixels[5] == 99.0f);

    test.color.return_nan = 1;
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 0U && error.context == 0U);
    test.color.return_nan = 0;

    pixels[0] = 77.0f;
    pixels[1] = 77.0f;
    pixels[2] = 77.0f;
    test.color.base_write_count = 3U;
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_INVALID_DATA);
    CHECK(pixels[0] == 77.0f && pixels[1] == 77.0f &&
          pixels[2] == 77.0f);
    test.color.base_write_count = 4U;
    test.color.gain_write_count = 0U;
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_INVALID_DATA);
    CHECK(pixels[0] == 77.0f);
    test.color.gain_write_count = 3U;
    test.color.linear_write_count = 2U;
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_INVALID_DATA);
    CHECK(pixels[0] == 77.0f);
    test.color.linear_write_count = 3U;
    test.color.base_status = AVIFDEC_LIMIT_EXCEEDED;
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_LIMIT_EXCEEDED);
    CHECK(pixels[0] == 77.0f);
    test.color.base_status = AVIFDEC_OK;
    test.color.base_sets_error = 1;
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 17U && pixels[0] == 77.0f);
    test.color.base_sets_error = 0;

    test.options.display_headroom = 0.0f;
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_INVALID_ARGUMENT);
    test.options.display_headroom = 4.0f;
    test.info.metadata.gain_map_max[0] =
        test_rational(1000, 1U);
    CHECK(avif_gain_map_apply_row(
        &test.base_image, &test.gain_image, &test.info,
        &test.adapter, &test.options, &output, 0U,
        &error) == AVIFDEC_INVALID_DATA);
    return 0;
}

static int test_numeric_basics(void) {
    float result;

    CHECK(avif_gain_map_approx_exp2(0.0f, &result) == AVIFDEC_OK);
    CHECK(result == 1.0f);
    CHECK(avif_gain_map_approx_exp2(1.0f, &result) == AVIFDEC_OK);
    CHECK(result == 2.0f);
    CHECK(avif_gain_map_approx_exp2(-1.0f, &result) == AVIFDEC_OK);
    CHECK(result == 0.5f);
    CHECK(avif_gain_map_approx_exp2(-151.0f, &result) == AVIFDEC_OK);
    CHECK(result == 0.0f);
    CHECK(avif_gain_map_approx_exp2(128.0f, &result) ==
          AVIFDEC_INVALID_DATA);
    CHECK(avif_gain_map_approx_pow(0.0f, 0.5f, &result) == AVIFDEC_OK);
    CHECK(result == 0.0f);
    CHECK(avif_gain_map_approx_pow(1.0f, 7.0f, &result) == AVIFDEC_OK);
    CHECK(result == 1.0f);
    CHECK(avif_gain_map_approx_pow(0.25f, 0.5f, &result) ==
          AVIFDEC_OK);
    CHECK(near_float(result, 0.5f, 0.000002f));
    CHECK(avif_gain_map_approx_pow(-0.1f, 1.0f, &result) ==
          AVIFDEC_INVALID_ARGUMENT);
    CHECK(avif_gain_map_approx_pow(0.5f, 0.0f, &result) ==
          AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

#ifdef AVIF_GAIN_MAP_HOSTED
static float hosted_max_exp2_relative_error;
static float hosted_max_pow_absolute_error;

static int test_hosted_numeric_accuracy(void) {
    int step;
    int base_step;
    static const float exponents[8] = {
        0.125f, 0.25f, 0.5f, 0.75f,
        1.0f, 2.0f, 4.0f, 8.0f
    };
    size_t exponent_index;

    hosted_max_exp2_relative_error = 0.0f;
    for (step = -2000; step <= 2000; ++step) {
        float exponent = (float)step / 100.0f;
        float approximate;
        float reference = exp2f(exponent);
        float relative;

        CHECK(avif_gain_map_approx_exp2(exponent, &approximate) ==
              AVIFDEC_OK);
        relative = absolute_float(approximate - reference) / reference;
        if (relative > hosted_max_exp2_relative_error) {
            hosted_max_exp2_relative_error = relative;
        }
    }
    CHECK(hosted_max_exp2_relative_error < 0.0000015f);

    hosted_max_pow_absolute_error = 0.0f;
    for (base_step = 0; base_step <= 1000; ++base_step) {
        float base = (float)base_step / 1000.0f;

        for (exponent_index = 0U;
             exponent_index <
                 sizeof(exponents) / sizeof(exponents[0]);
             ++exponent_index) {
            float approximate;
            float reference = powf(base, exponents[exponent_index]);
            float error;

            CHECK(avif_gain_map_approx_pow(
                base, exponents[exponent_index], &approximate) ==
                  AVIFDEC_OK);
            error = absolute_float(approximate - reference);
            if (error > hosted_max_pow_absolute_error) {
                hosted_max_pow_absolute_error = error;
            }
        }
    }
    CHECK(hosted_max_pow_absolute_error < 0.0000015f);
    return 0;
}
#endif

int main(void) {
    int result;

    result = test_exact_payloads();
    if (result != 0) return result;
    result = test_payload_failures();
    if (result != 0) return result;
    result = test_fragmented_payload();
    if (result != 0) return result;
    result = test_discovery_plans();
    if (result != 0) return result;
    result = test_workspace_and_planes();
    if (result != 0) return result;
    result = test_headroom_and_channel_math();
    if (result != 0) return result;
    result = test_bilinear_sampling();
    if (result != 0) return result;
    result = test_alpha_and_outputs();
    if (result != 0) return result;
    result = test_rows_and_finiteness();
    if (result != 0) return result;
    result = test_numeric_basics();
    if (result != 0) return result;
#ifdef AVIF_GAIN_MAP_HOSTED
    result = test_hosted_numeric_accuracy();
    if (result != 0) return result;
    (void)printf(
        "gain-map numeric max: exp2 relative %.9g, pow absolute %.9g\n",
        (double)hosted_max_exp2_relative_error,
        (double)hosted_max_pow_absolute_error);
#endif
    return 0;
}
