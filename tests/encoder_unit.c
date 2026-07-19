#include "encoder/avifenc.h"
#include "encoder/write.h"
#include "av1_bitstream.h"
#include "base.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static int text_equal(const char *left, const char *right) {
    size_t index = 0U;

    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return 0;
        ++index;
    }
    return left[index] == right[index];
}

static int test_byte_writer(void) {
    static const unsigned char raw[2] = { 0xaaU, 0xbbU };
    unsigned char output[24];
    unsigned char short_output[4];
    AvifencByteWriter writer;
    AvifencByteWriter sizing;
    AvifdecByteReader reader;
    const unsigned char *decoded_raw;
    size_t patch_offset;
      size_t patch_offsets[4];
    size_t sizing_offset;
    size_t index;

    avifenc_byte_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_byte_writer_u8(&writer, 0x11U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_u16be(&writer, 0x2233U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_reserve(&writer, 4U, &patch_offset) ==
          AVIFENC_OK);
    CHECK(avifenc_byte_writer_write(&writer, raw, sizeof(raw)) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_u64be(&writer, 0x8899aabbccddeeffULL) ==
          AVIFENC_OK);
    CHECK(avifenc_byte_writer_u32be(&writer, 0x10203040U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_patch_u32be(
              &writer, patch_offset, 0x44556677U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_size(&writer) == 21U);

    avifdec_byte_reader_init(&reader, output,
                             avifenc_byte_writer_size(&writer), 0U);
    CHECK(avifdec_byte_reader_u8(&reader) == 0x11U);
    CHECK(avifdec_byte_reader_u16be(&reader) == 0x2233U);
    CHECK(avifdec_byte_reader_u32be(&reader) == 0x44556677U);
    decoded_raw = avifdec_byte_reader_take(&reader, sizeof(raw));
    CHECK(decoded_raw != 0 && decoded_raw[0] == 0xaaU &&
          decoded_raw[1] == 0xbbU);
    CHECK(avifdec_byte_reader_u64be(&reader) == 0x8899aabbccddeeffULL);
    CHECK(avifdec_byte_reader_u32be(&reader) == 0x10203040U);
    CHECK(reader.status == AVIFDEC_OK &&
          avifdec_byte_reader_remaining(&reader) == 0U);

    avifenc_byte_writer_init_sizing(&sizing);
    CHECK(avifenc_byte_writer_u8(&sizing, 0x11U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_u16be(&sizing, 0x2233U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_reserve(&sizing, 4U, &sizing_offset) ==
          AVIFENC_OK && sizing_offset == patch_offset);
    CHECK(avifenc_byte_writer_write(&sizing, raw, sizeof(raw)) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_u64be(&sizing, 0x8899aabbccddeeffULL) ==
          AVIFENC_OK);
      CHECK(avifenc_byte_writer_u32be(&sizing, 0x10203040U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_patch_u32be(
              &sizing, sizing_offset, 0x44556677U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_size(&sizing) ==
          avifenc_byte_writer_size(&writer));

    avifenc_byte_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_byte_writer_reserve(&writer, 1U, &patch_offsets[0]) ==
          AVIFENC_OK);
    CHECK(avifenc_byte_writer_reserve(&writer, 2U, &patch_offsets[1]) ==
          AVIFENC_OK);
    CHECK(avifenc_byte_writer_reserve(&writer, 4U, &patch_offsets[2]) ==
          AVIFENC_OK);
    CHECK(avifenc_byte_writer_reserve(&writer, 8U, &patch_offsets[3]) ==
          AVIFENC_OK);
    CHECK(avifenc_byte_writer_patch_u8(
              &writer, patch_offsets[0], 0x12U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_patch_u16be(
              &writer, patch_offsets[1], 0x3456U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_patch_u32be(
              &writer, patch_offsets[2], 0x789abcdeU) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_patch_u64be(
              &writer, patch_offsets[3], 0xfedcba9876543210ULL) ==
          AVIFENC_OK);
    avifdec_byte_reader_init(&reader, output,
                             avifenc_byte_writer_size(&writer), 0U);
    CHECK(avifdec_byte_reader_u8(&reader) == 0x12U);
    CHECK(avifdec_byte_reader_u16be(&reader) == 0x3456U);
    CHECK(avifdec_byte_reader_u32be(&reader) == 0x789abcdeU);
    CHECK(avifdec_byte_reader_u64be(&reader) == 0xfedcba9876543210ULL);

    for (index = 0U; index < sizeof(short_output); ++index) {
        short_output[index] = 0x5aU;
    }
    avifenc_byte_writer_init(&writer, short_output, 3U);
    CHECK(avifenc_byte_writer_u32be(&writer, 0x01020304U) ==
          AVIFENC_OUTPUT_TOO_SMALL);
    CHECK(avifenc_byte_writer_size(&writer) == 0U);
    CHECK(avifenc_byte_writer_u8(&writer, 1U) == AVIFENC_OUTPUT_TOO_SMALL);
    for (index = 0U; index < sizeof(short_output); ++index) {
        CHECK(short_output[index] == 0x5aU);
    }

    avifenc_byte_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_byte_writer_write(&writer, 0, 1U) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(avifenc_byte_writer_size(&writer) == 0U);
    avifenc_byte_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_byte_writer_reserve(&writer, 1U, 0) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(writer.status == AVIFENC_INVALID_ARGUMENT && writer.position == 0U);
    CHECK(avifenc_byte_writer_u8(&writer, 0U) == AVIFENC_INVALID_ARGUMENT);
    avifenc_byte_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_byte_writer_u8(&writer, 0U) == AVIFENC_OK);
    CHECK(avifenc_byte_writer_patch_u16be(&writer, 0U, 0U) ==
          AVIFENC_INVALID_ARGUMENT);

    avifenc_byte_writer_init_sizing(&sizing);
    sizing.position = SIZE_MAX;
    CHECK(avifenc_byte_writer_u8(&sizing, 0U) == AVIFENC_OVERFLOW);
    CHECK(sizing.position == SIZE_MAX);
    return 0;
}

static int test_leb128_writer(void) {
    static const size_t values[] = {
        0U, 127U, 128U, 16384U, (size_t)0x00ffffffffffffffULL
    };
    unsigned char output[32];
    unsigned char short_output[1] = { 0x5aU };
    AvifencByteWriter writer;
    AvifencByteWriter sizing;
    AvifdecSpan span;
    Av1Stream stream;
    size_t index;

    avifenc_byte_writer_init(&writer, output, sizeof(output));
    avifenc_byte_writer_init_sizing(&sizing);
    for (index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) {
        CHECK(avifenc_byte_writer_leb128(&writer, values[index]) ==
              AVIFENC_OK);
        CHECK(avifenc_byte_writer_leb128(&sizing, values[index]) ==
              AVIFENC_OK);
    }
    CHECK(avifenc_byte_writer_size(&writer) ==
          avifenc_byte_writer_size(&sizing));
    span.data = output;
    span.size = avifenc_byte_writer_size(&writer);
    span.file_offset = 0U;
    stream.spans = &span;
    stream.span_count = 1U;
    stream.size = span.size;
    stream.position = 0U;
    stream.status = AVIFDEC_OK;
    for (index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) {
        size_t decoded = 0U;

        CHECK(av1_leb128(&stream, &decoded) == AVIFDEC_OK);
        CHECK(decoded == values[index]);
    }
    CHECK(stream.position == stream.size);

    avifenc_byte_writer_init(&writer, short_output, sizeof(short_output));
    CHECK(avifenc_byte_writer_leb128(&writer, 128U) ==
          AVIFENC_OUTPUT_TOO_SMALL);
    CHECK(writer.position == 0U && short_output[0] == 0x5aU);
    avifenc_byte_writer_init_sizing(&sizing);
    CHECK(avifenc_byte_writer_leb128(&sizing, SIZE_MAX) ==
          AVIFENC_LIMIT_EXCEEDED);
    CHECK(sizing.position == 0U);
    return 0;
}

static int test_bit_writer(void) {
    unsigned char output[16];
    unsigned char short_output[2] = { 0x5aU, 0xa5U };
    AvifencBitWriter writer;
    AvifencBitWriter sizing;
    AvifdecBitReader reader;
    size_t index;

    avifenc_bit_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_bit_writer_write(&writer, 5U, 3U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&writer, UINT64_MAX, 0U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&writer, 18U, 5U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&writer, 6U, 4U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&writer, 1U, 4U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_align(&writer) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_bits(&writer) == 16U &&
          avifenc_bit_writer_bytes(&writer) == 2U);
    CHECK(output[0] == 0xb2U && output[1] == 0x61U);
    avifdec_bit_reader_init(&reader, output, 2U, 0U);
    CHECK(avifdec_bit_reader_read(&reader, 3U) == 5U);
    CHECK(avifdec_bit_reader_read(&reader, 5U) == 18U);
    CHECK(avifdec_bit_reader_read(&reader, 4U) == 6U);
    CHECK(avifdec_bit_reader_read(&reader, 4U) == 1U);

    avifenc_bit_writer_init_sizing(&sizing);
    CHECK(avifenc_bit_writer_write(&sizing, 5U, 3U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&sizing, UINT64_MAX, 0U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&sizing, 18U, 5U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&sizing, 6U, 4U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_write(&sizing, 1U, 4U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_align(&sizing) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_bits(&sizing) ==
          avifenc_bit_writer_bits(&writer));

    avifenc_bit_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_bit_writer_write(
              &writer, 0x0123456789abcdefULL, 64U) == AVIFENC_OK);
    avifdec_bit_reader_init(&reader, output, 8U, 0U);
    CHECK(avifdec_bit_reader_read(&reader, 32U) == 0x01234567U);
    CHECK(avifdec_bit_reader_read(&reader, 32U) == 0x89abcdefU);

    avifenc_bit_writer_init(&writer, short_output, 1U);
    CHECK(avifenc_bit_writer_write(&writer, 0x1ffU, 9U) ==
          AVIFENC_OUTPUT_TOO_SMALL);
    CHECK(writer.bit_position == 0U);
    for (index = 0U; index < sizeof(short_output); ++index) {
        CHECK(short_output[index] == (index == 0U ? 0x5aU : 0xa5U));
    }
    CHECK(avifenc_bit_writer_write(&writer, 0U, 0U) ==
          AVIFENC_OUTPUT_TOO_SMALL);

    avifenc_bit_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_bit_writer_write(&writer, 8U, 3U) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(writer.bit_position == 0U);
    avifenc_bit_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_bit_writer_write(&writer, 0U, 65U) ==
          AVIFENC_INVALID_ARGUMENT);
    avifenc_bit_writer_init_sizing(&sizing);
    sizing.bit_position = SIZE_MAX;
    CHECK(avifenc_bit_writer_write(&sizing, 0U, 1U) == AVIFENC_OVERFLOW);
    CHECK(sizing.bit_position == SIZE_MAX);

    avifenc_bit_writer_init(&writer, output, sizeof(output));
    CHECK(avifenc_bit_writer_write(&writer, 5U, 3U) == AVIFENC_OK);
    CHECK(avifenc_bit_writer_align(&writer) == AVIFENC_OK);
    CHECK(writer.bit_position == 8U && output[0] == 0xa0U);
    return 0;
}

static int test_av1_code_writers(void) {
    static const uint32_t uvlc_values[] = {
        0U, 1U, 2U, 3U, 14U, 255U, UINT32_MAX
    };
      static const uint32_t ns_counts[] = {
            1U, 2U, 3U, 5U, 16U, 17U, UINT32_MAX
      };
      static const uint32_t ns_values[] = {
            0U, 1U, 2U, 3U, 15U, 16U, UINT32_MAX - 1U
      };
    unsigned char output[32];
    unsigned char short_output[2] = { 0x5aU, 0xa5U };
    AvifencBitWriter writer;
    AvifencBitWriter sizing;
    AvifdecSpan span;
    Av1Stream stream;
    Av1Bits bits;
    size_t index;

    avifenc_bit_writer_init(&writer, output, sizeof(output));
    avifenc_bit_writer_init_sizing(&sizing);
    for (index = 0U;
         index < sizeof(uvlc_values) / sizeof(uvlc_values[0]); ++index) {
        CHECK(avifenc_bit_writer_uvlc(&writer, uvlc_values[index]) ==
              AVIFENC_OK);
        CHECK(avifenc_bit_writer_uvlc(&sizing, uvlc_values[index]) ==
              AVIFENC_OK);
    }
    for (index = 0U;
         index < sizeof(ns_counts) / sizeof(ns_counts[0]); ++index) {
        CHECK(avifenc_bit_writer_ns(
                  &writer, ns_values[index], ns_counts[index]) == AVIFENC_OK);
        CHECK(avifenc_bit_writer_ns(
                  &sizing, ns_values[index], ns_counts[index]) == AVIFENC_OK);
    }
    CHECK(avifenc_bit_writer_bits(&writer) ==
          avifenc_bit_writer_bits(&sizing));

    span.data = output;
    span.size = avifenc_bit_writer_bytes(&writer);
    span.file_offset = 0U;
    stream.spans = &span;
    stream.span_count = 1U;
    stream.size = span.size;
    stream.position = 0U;
    stream.status = AVIFDEC_OK;
    av1_bits_init(&bits, &stream, 0U, span.size);
    for (index = 0U;
         index < sizeof(uvlc_values) / sizeof(uvlc_values[0]); ++index) {
        CHECK(av1_bits_uvlc(&bits) == uvlc_values[index]);
        CHECK(bits.status == AVIFDEC_OK);
    }
    for (index = 0U;
         index < sizeof(ns_counts) / sizeof(ns_counts[0]); ++index) {
        CHECK(av1_bits_ns(&bits, ns_counts[index]) == ns_values[index]);
        CHECK(bits.status == AVIFDEC_OK);
    }
    CHECK(bits.bit_position == avifenc_bit_writer_bits(&writer));

    avifenc_bit_writer_init(&writer, short_output, sizeof(short_output));
    CHECK(avifenc_bit_writer_uvlc(&writer, 255U) ==
          AVIFENC_OUTPUT_TOO_SMALL);
    CHECK(writer.bit_position == 0U && short_output[0] == 0x5aU &&
          short_output[1] == 0xa5U);
    avifenc_bit_writer_init_sizing(&sizing);
    CHECK(avifenc_bit_writer_ns(&sizing, 5U, 5U) ==
          AVIFENC_INVALID_ARGUMENT);
    avifenc_bit_writer_init_sizing(&sizing);
      CHECK(avifenc_bit_writer_ns(&sizing, 0U, 0x80000001U) == AVIFENC_OK);
      CHECK(sizing.bit_position == 31U);
    return 0;
}

static AvifencImage valid_image(void) {
    static const uint8_t y[4] = { 16U, 32U, 48U, 64U };
    static const uint8_t u[1] = { 128U };
    static const uint8_t v[1] = { 128U };
    AvifencImage image = { 0 };

    image.planes[0] = y;
    image.planes[1] = u;
    image.planes[2] = v;
    image.strides[0] = 2U;
    image.strides[1] = 1U;
    image.strides[2] = 1U;
    image.width = 2U;
    image.height = 2U;
    image.color.color_primaries = 1U;
    image.color.transfer_characteristics = 1U;
    image.color.matrix_coefficients = 1U;
    return image;
}

static int test_public_contract(void) {
    AvifencOptions options;

    avifenc_options_default(&options);
    CHECK(options.quantizer == AVIFENC_DEFAULT_QUANTIZER);
    avifenc_options_default(0);
    CHECK(text_equal(avifenc_version_string(), "0.1.0"));
    CHECK(text_equal(avifenc_status_string(AVIFENC_OK), "ok"));
    CHECK(text_equal(avifenc_status_string((AvifencStatus)99),
                     "unknown error"));
    CHECK(text_equal(
        avifenc_error_context_string(AVIFENC_CONTEXT_PLANE_Y), "Y plane"));
    CHECK(text_equal(
        avifenc_error_context_string((AvifencErrorContext)99),
        "unknown context"));
    return 0;
}

static int test_query_validation(void) {
    AvifencImage image = valid_image();
    AvifencOptions options;
    AvifencRequirements requirements = { 9U, 9U };
    AvifencError error;
    unsigned int plane;

    avifenc_options_default(&options);
    CHECK(avifenc_query(0, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_IMAGE);
    CHECK(requirements.workspace_required == 0U &&
          requirements.output_capacity_required == 0U);
    CHECK(avifenc_query(&image, 0, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_OPTIONS);
    CHECK(avifenc_query(&image, &options, 0, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_REQUIREMENTS);

    image.width = 0U;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_DIMENSIONS);
    image = valid_image();
    image.width = 3U;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    image = valid_image();
    image.height = 3U;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    image = valid_image();
    image.width = AVIFENC_MAX_DIMENSION + 2U;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_LIMIT_EXCEEDED);

    options.quantizer = 256U;
    image = valid_image();
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_QUANTIZER &&
          error.required_size == 255U && error.provided_size == 256U);
    avifenc_options_default(&options);

    for (plane = 0U; plane < 3U; ++plane) {
        image = valid_image();
        image.planes[plane] = 0;
        CHECK(avifenc_query(&image, &options, &requirements, &error) ==
              AVIFENC_INVALID_ARGUMENT);
        CHECK(error.context ==
              (AvifencErrorContext)(AVIFENC_CONTEXT_PLANE_Y + plane));
    }
    for (plane = 0U; plane < 3U; ++plane) {
        image = valid_image();
        image.strides[plane] = 0U;
        CHECK(avifenc_query(&image, &options, &requirements, &error) ==
              AVIFENC_INVALID_ARGUMENT);
        CHECK(error.context ==
              (AvifencErrorContext)(AVIFENC_CONTEXT_PLANE_Y + plane));
    }

    image = valid_image();
    image.color.full_range = 2U;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_COLOR);
    image = valid_image();
    image.color.chroma_sample_position = 4U;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    image = valid_image();
    image.height = 4U;
    image.strides[0] = SIZE_MAX;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_OVERFLOW);
    CHECK(error.context == AVIFENC_CONTEXT_PLANE_Y);

    image = valid_image();
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_OK);
    CHECK(error.status == AVIFENC_OK && error.context == AVIFENC_CONTEXT_NONE);
    CHECK(requirements.workspace_required == 12U);
    CHECK(requirements.output_capacity_required == 65600U);
    return 0;
}

static int test_encode_boundaries(void) {
    static unsigned char workspace[12];
    static unsigned char output[65600];
    AvifencImage image = valid_image();
    AvifencOptions options;
    AvifencRequirements requirements;
    AvifencError error;
    size_t output_written = 99U;
    size_t index;

    avifenc_options_default(&options);
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_OK);
    CHECK(avifenc_encode(&image, &options, workspace, sizeof(workspace),
                         output, sizeof(output), 0, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_OUTPUT);
    CHECK(avifenc_encode(&image, &options, workspace, sizeof(workspace) - 1U,
                         output, sizeof(output), &output_written, &error) ==
          AVIFENC_OUT_OF_MEMORY);
    CHECK(output_written == 0U && error.context == AVIFENC_CONTEXT_WORKSPACE);
    CHECK(error.required_size == requirements.workspace_required &&
          error.provided_size == sizeof(workspace) - 1U);
    CHECK(avifenc_encode(&image, &options, 0, sizeof(workspace),
                         output, sizeof(output), &output_written, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_WORKSPACE);
    CHECK(avifenc_encode(&image, &options, workspace, sizeof(workspace),
                         output, sizeof(output) - 1U,
                         &output_written, &error) ==
          AVIFENC_OUTPUT_TOO_SMALL);
    CHECK(error.required_size == requirements.output_capacity_required &&
          error.provided_size == sizeof(output) - 1U);
    CHECK(avifenc_encode(&image, &options, workspace, sizeof(workspace),
                         0, sizeof(output), &output_written, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_OUTPUT);

    for (index = 0U; index < sizeof(workspace); ++index) workspace[index] = 0x5aU;
    for (index = 0U; index < sizeof(output); ++index) output[index] = 0xa5U;
    CHECK(avifenc_encode(&image, &options, workspace, sizeof(workspace),
                         output, sizeof(output), &output_written, &error) ==
          AVIFENC_UNSUPPORTED);
    CHECK(output_written == 0U && error.context == AVIFENC_CONTEXT_IMPLEMENTATION);
    for (index = 0U; index < sizeof(workspace); ++index) {
        CHECK(workspace[index] == 0x5aU);
    }
    for (index = 0U; index < sizeof(output); ++index) {
        CHECK(output[index] == 0xa5U);
    }
    return 0;
}

int main(int argc, char **argv) {
    int result;

    (void)argc;
    (void)argv;
      result = test_byte_writer();
      if (result != 0) return result;
      result = test_leb128_writer();
      if (result != 0) return result;
      result = test_bit_writer();
      if (result != 0) return result;
      result = test_av1_code_writers();
      if (result != 0) return result;
      result = test_public_contract();
    if (result != 0) return result;
    result = test_query_validation();
    if (result != 0) return result;
    return test_encode_boundaries();
}