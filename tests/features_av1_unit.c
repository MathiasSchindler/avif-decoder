#include "av1_avif_conformance.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static uint32_t obu_context(uint8_t obu_type) {
    return ((uint32_t)'O' << 24) |
           ((uint32_t)'B' << 16) |
           ((uint32_t)'U' << 8) |
           (uint32_t)obu_type;
}

static void reset_error(AvifdecError *error) {
    error->status = AVIFDEC_OK;
    error->offset = 0U;
    error->context = 0U;
}

static AvifdecStatus validate_bytes(
    const unsigned char *data,
    size_t size,
    size_t file_offset,
    uint8_t framing,
    AvifdecError *error) {
    AvifdecSpan span;

    span.data = data;
    span.size = size;
    span.file_offset = file_offset;
    reset_error(error);
    return av1_avif_validate_obu_stream(
        &span, 1U, framing, error);
}

static int test_low_overhead_tile_list(void) {
    static const unsigned char tile_list[] = { 0x42U, 0x00U };
    static const unsigned char selected_extension[] = {
        0x46U, 0x00U, 0x00U
    };
    static const unsigned char unselected_extension[] = {
        0x46U, 0xb8U, 0x00U
    };
    static const unsigned char truncated_size[] = { 0x42U };
    static const unsigned char after_normal_obu[] = {
        0x0aU, 0x01U, 0x80U, 0x46U, 0xb8U, 0x00U
    };
    AvifdecError error;

    CHECK(validate_bytes(
              tile_list, sizeof(tile_list), 41U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.status == AVIFDEC_INVALID_DATA &&
          error.offset == 41U &&
          error.context == obu_context(8U));
    CHECK(validate_bytes(
              selected_extension, sizeof(selected_extension), 100U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 100U && error.context == obu_context(8U));
    CHECK(validate_bytes(
              unselected_extension, sizeof(unselected_extension), 200U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 200U && error.context == obu_context(8U));
    CHECK(validate_bytes(
              truncated_size, sizeof(truncated_size), 250U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 250U && error.context == obu_context(8U));
    CHECK(validate_bytes(
              after_normal_obu, sizeof(after_normal_obu), 300U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 303U && error.context == obu_context(8U));
    return 0;
}

static int test_annex_b_tile_list(void) {
    static const unsigned char tile_list[] = {
        0x05U, 0x04U, 0x01U, 0x10U, 0x01U, 0x40U
    };
    static const unsigned char selected_extension[] = {
        0x06U, 0x05U, 0x01U, 0x10U, 0x02U, 0x44U, 0x00U
    };
    static const unsigned char unselected_extension[] = {
        0x06U, 0x05U, 0x01U, 0x10U, 0x02U, 0x44U, 0xb8U
    };
    AvifdecError error;

    CHECK(validate_bytes(
              tile_list, sizeof(tile_list), 500U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.status == AVIFDEC_INVALID_DATA &&
          error.offset == 505U &&
          error.context == obu_context(8U));
    CHECK(validate_bytes(
              selected_extension, sizeof(selected_extension), 600U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 605U && error.context == obu_context(8U));
    CHECK(validate_bytes(
              unselected_extension, sizeof(unselected_extension), 700U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 705U && error.context == obu_context(8U));
    return 0;
}

static int test_valid_and_skipped_obus(void) {
    static const unsigned char low_overhead[] = {
        0x12U, 0x00U,
        0x0aU, 0x01U, 0x80U,
        0x4aU, 0x02U, 0xdeU, 0xadU,
        0x02U, 0x01U, 0xffU,
        0x7aU, 0x03U, 0x00U, 0x00U, 0x00U,
        0x1aU, 0x00U
    };
    static const unsigned char annex_b[] = {
        0x0bU, 0x0aU,
        0x01U, 0x10U,
        0x03U, 0x48U, 0xaaU, 0xbbU,
        0x03U, 0x78U, 0x00U, 0x00U
    };
    static const unsigned char low_bad_padding[] = {
        0x7aU, 0x02U, 0x00U, 0x01U
    };
    static const unsigned char low_bad_delimiter[] = {
        0x12U, 0x01U, 0x00U
    };
    static const unsigned char reserved_contains_tile_header[] = {
        0x4aU, 0x02U, 0x42U, 0x00U
    };
    static const unsigned char annex_missing_delimiter[] = {
        0x03U, 0x02U, 0x01U, 0x18U
    };
    static const unsigned char annex_second_delimiter[] = {
        0x05U, 0x04U, 0x01U, 0x10U, 0x01U, 0x10U
    };
    static const unsigned char annex_bad_delimiter_payload[] = {
        0x04U, 0x03U, 0x02U, 0x10U, 0x00U
    };
    AvifdecError error;

    CHECK(validate_bytes(
              low_overhead, sizeof(low_overhead), 0U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) == AVIFDEC_OK);
    CHECK(error.status == AVIFDEC_OK);
    CHECK(validate_bytes(
              annex_b, sizeof(annex_b), 0U,
              AVIFDEC_AV1_ANNEX_B, &error) == AVIFDEC_OK);
    CHECK(error.status == AVIFDEC_OK);
    CHECK(validate_bytes(
              low_bad_padding, sizeof(low_bad_padding), 80U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 80U && error.context == obu_context(15U));
    CHECK(validate_bytes(
              low_bad_delimiter, sizeof(low_bad_delimiter), 90U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 90U && error.context == obu_context(2U));
    CHECK(validate_bytes(
              reserved_contains_tile_header,
              sizeof(reserved_contains_tile_header), 95U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) == AVIFDEC_OK);
    CHECK(validate_bytes(
              annex_missing_delimiter,
              sizeof(annex_missing_delimiter), 100U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 103U && error.context == obu_context(3U));
    CHECK(validate_bytes(
              annex_second_delimiter,
              sizeof(annex_second_delimiter), 110U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 115U && error.context == obu_context(2U));
    CHECK(validate_bytes(
              annex_bad_delimiter_payload,
              sizeof(annex_bad_delimiter_payload), 120U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 123U && error.context == obu_context(2U));
    return 0;
}

static int test_malformed_lengths(void) {
    static const unsigned char low_short_payload[] = {
        0x0aU, 0x03U, 0xaaU
    };
    static const unsigned char low_short_leb[] = {
        0x0aU, 0x80U
    };
    static const unsigned char low_invalid_leb[] = {
        0x0aU, 0x80U, 0x80U, 0x80U, 0x80U,
        0x80U, 0x80U, 0x80U, 0x80U
    };
    static const unsigned char annex_short_temporal_unit[] = {
        0x06U, 0x04U, 0x01U, 0x10U, 0x01U, 0x18U
    };
    static const unsigned char annex_short_obu[] = {
        0x05U, 0x04U, 0x01U, 0x10U, 0x02U, 0x18U
    };
    static const unsigned char annex_inner_size_mismatch[] = {
        0x07U, 0x06U, 0x01U, 0x10U,
        0x03U, 0x1aU, 0x00U, 0xffU
    };
    static const unsigned char annex_short_leb[] = { 0x80U };
    static const unsigned char annex_short_frame_leb[] = {
        0x01U, 0x80U
    };
    static const unsigned char annex_short_obu_leb[] = {
        0x02U, 0x01U, 0x80U
    };
    AvifdecError error;

    CHECK(validate_bytes(
              low_short_payload, sizeof(low_short_payload), 1000U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_TRUNCATED);
    CHECK(error.offset == 1000U && error.context == obu_context(1U));
    CHECK(validate_bytes(
              low_short_leb, sizeof(low_short_leb), 1010U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_TRUNCATED);
    CHECK(error.offset == 1010U && error.context == obu_context(1U));
    CHECK(validate_bytes(
              low_invalid_leb, sizeof(low_invalid_leb), 1020U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1020U && error.context == obu_context(1U));
    CHECK(validate_bytes(
              annex_short_temporal_unit,
              sizeof(annex_short_temporal_unit), 1030U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1030U && error.context == obu_context(0U));
    CHECK(validate_bytes(
              annex_short_obu, sizeof(annex_short_obu), 1040U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1044U && error.context == obu_context(0U));
    CHECK(validate_bytes(
              annex_inner_size_mismatch,
              sizeof(annex_inner_size_mismatch), 1050U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1055U && error.context == obu_context(3U));
    CHECK(validate_bytes(
              annex_short_leb, sizeof(annex_short_leb), 1060U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_TRUNCATED);
    CHECK(error.offset == 1060U && error.context == obu_context(0U));
    CHECK(validate_bytes(
              annex_short_frame_leb,
              sizeof(annex_short_frame_leb), 1070U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_TRUNCATED);
    CHECK(error.offset == 1071U && error.context == obu_context(0U));
    CHECK(validate_bytes(
              annex_short_obu_leb,
              sizeof(annex_short_obu_leb), 1080U,
              AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_TRUNCATED);
    CHECK(error.offset == 1082U && error.context == obu_context(0U));
    return 0;
}

static int test_multi_span_reads(void) {
    static const unsigned char part0[] = {
        0x12U, 0x00U, 0x4aU
    };
    static const unsigned char part1[] = {
        0x02U, 0xdeU
    };
    static const unsigned char part2[] = {
        0xadU, 0x7aU, 0x03U, 0x00U
    };
    static const unsigned char part3[] = {
        0x00U, 0x00U
    };
    static const unsigned char tile_prefix[] = {
        0x0aU, 0x01U
    };
    static const unsigned char tile_middle[] = {
        0x80U, 0x46U
    };
    static const unsigned char tile_tail[] = {
        0xb8U, 0x00U
    };
    static const unsigned char annex_part0[] = {
        0x05U, 0x04U, 0x01U
    };
    static const unsigned char annex_part1[] = {
        0x10U, 0x01U
    };
    static const unsigned char annex_part2[] = { 0x40U };
    static const unsigned char zero = 0U;
    AvifdecSpan spans[5];
    AvifdecError error;

    spans[0].data = part0;
    spans[0].size = sizeof(part0);
    spans[0].file_offset = 10U;
    spans[1].data = &zero;
    spans[1].size = 0U;
    spans[1].file_offset = 99U;
    spans[2].data = part1;
    spans[2].size = sizeof(part1);
    spans[2].file_offset = 200U;
    spans[3].data = part2;
    spans[3].size = sizeof(part2);
    spans[3].file_offset = 400U;
    spans[4].data = part3;
    spans[4].size = sizeof(part3);
    spans[4].file_offset = 800U;
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              spans, 5U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_OK);

    spans[0].data = tile_prefix;
    spans[0].size = sizeof(tile_prefix);
    spans[0].file_offset = 900U;
    spans[1].data = tile_middle;
    spans[1].size = sizeof(tile_middle);
    spans[1].file_offset = 1000U;
    spans[2].data = tile_tail;
    spans[2].size = sizeof(tile_tail);
    spans[2].file_offset = 1100U;
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              spans, 3U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1001U && error.context == obu_context(8U));

    spans[0].data = annex_part0;
    spans[0].size = sizeof(annex_part0);
    spans[0].file_offset = 1200U;
    spans[1].data = annex_part1;
    spans[1].size = sizeof(annex_part1);
    spans[1].file_offset = 1300U;
    spans[2].data = annex_part2;
    spans[2].size = sizeof(annex_part2);
    spans[2].file_offset = 1400U;
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              spans, 3U, AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1400U && error.context == obu_context(8U));
    return 0;
}

static int test_large_payload_across_spans(void) {
    static const unsigned char prefix[] = { 0x4aU, 0x80U };
    static const unsigned char remainder[129] = { 0x01U };
    AvifdecSpan spans[2];
    AvifdecError error;

    spans[0].data = prefix;
    spans[0].size = sizeof(prefix);
    spans[0].file_offset = 20U;
    spans[1].data = remainder;
    spans[1].size = sizeof(remainder);
    spans[1].file_offset = 900U;
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              spans, 2U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_OK);
    return 0;
}

static int test_overflow_arguments_and_first_error(void) {
    static const unsigned char tile_list[] = { 0x42U, 0x00U };
    static const unsigned char first_padding_error[] = {
        0x7aU, 0x01U, 0x01U, 0x42U, 0x00U
    };
    static const unsigned char byte = 0U;
    AvifdecSpan spans[2];
    AvifdecError error;

    spans[0].data = &byte;
    spans[0].size = 0U;
    spans[0].file_offset = 70U;
    spans[1].data = 0;
    spans[1].size = 0U;
    spans[1].file_offset = 71U;
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              spans, 2U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 71U && error.context == obu_context(0U));
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              0, 0U, AVIFDEC_AV1_ANNEX_B, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 0U && error.context == obu_context(0U));

    spans[0].data = &byte;
    spans[0].size = SIZE_MAX;
    spans[0].file_offset = 0U;
    spans[1].data = &byte;
    spans[1].size = 1U;
    spans[1].file_offset = 77U;
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              spans, 2U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_OVERFLOW);
    CHECK(error.offset == 77U && error.context == obu_context(0U));

    spans[0].data = &byte;
    spans[0].size = 2U;
    spans[0].file_offset = SIZE_MAX;
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              spans, 1U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_OVERFLOW);
    CHECK(error.offset == SIZE_MAX &&
          error.context == obu_context(0U));

    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              0, 1U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_ARGUMENT);
    CHECK(error.status == AVIFDEC_INVALID_ARGUMENT &&
          error.context == obu_context(0U));
    reset_error(&error);
    CHECK(av1_avif_validate_obu_stream(
              0, 0U, 2U, &error) == AVIFDEC_INVALID_ARGUMENT);
    CHECK(error.status == AVIFDEC_OK);

    CHECK(validate_bytes(
              first_padding_error, sizeof(first_padding_error), 1500U,
              AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 1500U &&
          error.context == obu_context(15U));

    spans[0].data = tile_list;
    spans[0].size = sizeof(tile_list);
    spans[0].file_offset = 1600U;
    error.status = AVIFDEC_IO_ERROR;
    error.offset = 12U;
    error.context = 34U;
    CHECK(av1_avif_validate_obu_stream(
              spans, 1U, AVIFDEC_AV1_LOW_OVERHEAD, &error) ==
          AVIFDEC_INVALID_DATA);
    CHECK(error.status == AVIFDEC_IO_ERROR &&
          error.offset == 12U && error.context == 34U);
    return 0;
}

static int test_large_scale_and_capabilities(void) {
    uint64_t capabilities = avifdec_capabilities();

    CHECK(av1_avif_validate_large_scale_tile(0) == AVIFDEC_OK);
    CHECK(av1_avif_validate_large_scale_tile(1) ==
          AVIFDEC_INVALID_DATA);
    CHECK(av1_avif_validate_large_scale_tile(-1) ==
          AVIFDEC_INVALID_DATA);

    /*
     * AVIF forbids both modes; these validators intentionally do not implement
     * generic tile-list decoding. Integration must keep capability bits 6 and 7
     * clear even after the conformance module is wired into the main parser.
     */
    CHECK((capabilities & AVIFDEC_CAP_AV1_TILE_LIST) == 0U);
    CHECK((capabilities & AVIFDEC_CAP_AV1_LARGE_SCALE_TILE) == 0U);
    return 0;
}

int main(int argc, char **argv) {
    int result;

    (void)argc;
    (void)argv;
    result = test_low_overhead_tile_list();
    if (result != 0) return result;
    result = test_annex_b_tile_list();
    if (result != 0) return result;
    result = test_valid_and_skipped_obus();
    if (result != 0) return result;
    result = test_malformed_lengths();
    if (result != 0) return result;
    result = test_multi_span_reads();
    if (result != 0) return result;
    result = test_large_payload_across_spans();
    if (result != 0) return result;
    result = test_overflow_arguments_and_first_error();
    if (result != 0) return result;
    result = test_large_scale_and_capabilities();
    if (result != 0) return result;
    return 0;
}
