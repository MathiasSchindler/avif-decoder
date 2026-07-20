#include "encoder/avifenc.h"
#include "encoder/av1_symbol_write.h"
#include "encoder/av1_tile_write.h"
#include "encoder/av1_transform_write.h"
#include "encoder/av1_write.h"
#include "encoder/avif_write.h"
#include "encoder/write.h"
#include "av1.h"
#include "av1_bitstream.h"
#include "av1_symbol.h"
#include "base.h"
#include "bmff.h"

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

static int test_av1_symbol_writer_vectors(void) {
      static const unsigned char expected_one[1] = { 0xc0U };
      static const unsigned char expected_zero[1] = { 0x20U };
    unsigned char output[8];
    AvifencAv1SymbolWriter writer;
    Av1SymbolDecoder decoder;
    AvifdecSpan span;
    uint16_t cdf[3];

    cdf[0] = 16384U;
    cdf[1] = 32768U;
    cdf[2] = 0U;
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_write(&writer, cdf, 2U, 1U) ==
          AVIFENC_OK);
    CHECK(cdf[0] == 15360U && cdf[1] == 32768U && cdf[2] == 1U);
    CHECK(avifenc_av1_symbol_writer_finish(&writer) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_size(&writer) == sizeof(expected_one));
    CHECK(avifdec_memory_compare(
              output, expected_one, sizeof(expected_one)) == 0);

    span.data = output;
    span.size = avifenc_av1_symbol_writer_size(&writer);
    span.file_offset = 0U;
    cdf[0] = 16384U;
    cdf[1] = 32768U;
    cdf[2] = 0U;
    CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) ==
          AVIFDEC_OK);
    CHECK(av1_symbol_read(&decoder, cdf, 2U) == 1U);
    CHECK(av1_symbol_exit(&decoder) == AVIFDEC_OK);

    cdf[0] = 16384U;
    cdf[1] = 32768U;
    cdf[2] = 0U;
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_write(&writer, cdf, 2U, 0U) ==
          AVIFENC_OK);
    CHECK(cdf[0] == 17408U && cdf[1] == 32768U && cdf[2] == 1U);
    CHECK(avifenc_av1_symbol_writer_finish(&writer) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_size(&writer) == sizeof(expected_zero));
    CHECK(avifdec_memory_compare(
              output, expected_zero, sizeof(expected_zero)) == 0);
    return 0;
}

static void init_symbol_test_cdfs(uint16_t cdf2[3],
                                  uint16_t cdf3[4],
                                  uint16_t cdf5[6],
                                  uint16_t cdf16[17]) {
    size_t index;

    cdf2[0] = 9000U;
    cdf2[1] = 32768U;
    cdf2[2] = 0U;
    cdf3[0] = 4000U;
    cdf3[1] = 20000U;
    cdf3[2] = 32768U;
    cdf3[3] = 0U;
    cdf5[0] = 1000U;
    cdf5[1] = 6000U;
    cdf5[2] = 14000U;
    cdf5[3] = 25000U;
    cdf5[4] = 32768U;
    cdf5[5] = 0U;
    for (index = 0U; index < 16U; ++index) {
        cdf16[index] = (uint16_t)((index + 1U) * 2048U);
    }
    cdf16[16] = 0U;
}

static int test_av1_symbol_writer_round_trip(void) {
    unsigned char output[512];
    AvifencAv1SymbolWriter writer;
    Av1SymbolDecoder decoder;
    AvifdecSpan span;
    uint16_t encode2[3];
    uint16_t encode3[4];
    uint16_t encode5[6];
    uint16_t encode16[17];
    uint16_t decode2[3];
    uint16_t decode3[4];
    uint16_t decode5[6];
    uint16_t decode16[17];
    unsigned int iteration;

    init_symbol_test_cdfs(encode2, encode3, encode5, encode16);
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    for (iteration = 0U; iteration < 96U; ++iteration) {
        unsigned int bits = iteration % 7U + 1U;
        uint32_t mask = ((uint32_t)1U << bits) - 1U;
        uint32_t literal = (iteration * 37U + 5U) & mask;

        CHECK(avifenc_av1_symbol_writer_literal(
                  &writer, literal, bits) == AVIFENC_OK);
        CHECK(avifenc_av1_symbol_writer_write(
                  &writer, encode2, 2U, iteration % 2U) == AVIFENC_OK);
        CHECK(avifenc_av1_symbol_writer_write(
                  &writer, encode3, 3U, (iteration * 2U) % 3U) ==
              AVIFENC_OK);
        CHECK(avifenc_av1_symbol_writer_write(
                  &writer, encode5, 5U, (iteration * 3U) % 5U) ==
              AVIFENC_OK);
        CHECK(avifenc_av1_symbol_writer_write(
                  &writer, encode16, 16U, (iteration * 7U) % 16U) ==
              AVIFENC_OK);
    }
    CHECK(avifenc_av1_symbol_writer_finish(&writer) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_size(&writer) > 100U);

    span.data = output;
    span.size = avifenc_av1_symbol_writer_size(&writer);
    span.file_offset = 0U;
    init_symbol_test_cdfs(decode2, decode3, decode5, decode16);
    CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) ==
          AVIFDEC_OK);
    for (iteration = 0U; iteration < 96U; ++iteration) {
        unsigned int bits = iteration % 7U + 1U;
        uint32_t mask = ((uint32_t)1U << bits) - 1U;
        uint32_t literal = (iteration * 37U + 5U) & mask;

        CHECK(av1_symbol_read_literal(&decoder, bits) == literal);
        CHECK(av1_symbol_read(&decoder, decode2, 2U) == iteration % 2U);
        CHECK(av1_symbol_read(&decoder, decode3, 3U) ==
              (iteration * 2U) % 3U);
        CHECK(av1_symbol_read(&decoder, decode5, 5U) ==
              (iteration * 3U) % 5U);
        CHECK(av1_symbol_read(&decoder, decode16, 16U) ==
              (iteration * 7U) % 16U);
    }
    CHECK(av1_symbol_exit(&decoder) == AVIFDEC_OK);
    CHECK(avifdec_memory_compare(encode2, decode2, sizeof(encode2)) == 0);
    CHECK(avifdec_memory_compare(encode3, decode3, sizeof(encode3)) == 0);
    CHECK(avifdec_memory_compare(encode5, decode5, sizeof(encode5)) == 0);
    CHECK(avifdec_memory_compare(encode16, decode16, sizeof(encode16)) == 0);
    return 0;
}

static int test_av1_symbol_writer_carry_and_tail(void) {
    unsigned char output[16];
    AvifencAv1SymbolWriter writer;
    Av1SymbolDecoder decoder;
    AvifdecSpan span;
    uint16_t equal[3] = { 16384U, 32768U, 0U };
    uint16_t rare_one[3] = { 32256U, 32768U, 0U };
    uint16_t likely_zero[3] = { 24576U, 32768U, 0U };

    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 1);
    CHECK(avifenc_av1_symbol_writer_write(&writer, equal, 2U, 0U) ==
          AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_write(&writer, equal, 2U, 0U) ==
          AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_write(&writer, rare_one, 2U, 1U) ==
          AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_write(&writer, likely_zero, 2U, 0U) ==
          AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_finish(&writer) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_size(&writer) == 2U &&
          output[0] == 63U);
    CHECK(equal[0] == 16384U && equal[2] == 0U &&
          rare_one[0] == 32256U && likely_zero[0] == 24576U);

    span.data = output;
    span.size = avifenc_av1_symbol_writer_size(&writer);
    span.file_offset = 0U;
    CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 1) ==
          AVIFDEC_OK);
    CHECK(av1_symbol_read(&decoder, equal, 2U) == 0U);
    CHECK(av1_symbol_read(&decoder, equal, 2U) == 0U);
    CHECK(av1_symbol_read(&decoder, rare_one, 2U) == 1U);
    CHECK(av1_symbol_read(&decoder, likely_zero, 2U) == 0U);
    CHECK(av1_symbol_exit(&decoder) == AVIFDEC_OK);

    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 1);
    CHECK(avifenc_av1_symbol_writer_literal(&writer, 1U, 1U) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_finish(&writer) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_size(&writer) == 1U &&
          output[0] == 0xc0U);
    output[1] = 0x01U;
    span.size = 2U;
    CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 1) ==
          AVIFDEC_OK);
    CHECK(av1_symbol_read_literal(&decoder, 1U) == 1U);
    CHECK(av1_symbol_exit(&decoder) == AVIFDEC_INVALID_DATA);
    return 0;
}

static AvifencStatus write_symbol_sizing_sequence(
    AvifencAv1SymbolWriter *writer) {
    unsigned int index;

    for (index = 0U; index < 512U; ++index) {
        AvifencStatus status = avifenc_av1_symbol_writer_literal(
            writer, (index * 29U + 7U) & 255U, 8U);

        if (status != AVIFENC_OK) return status;
    }
    return avifenc_av1_symbol_writer_finish(writer);
}

static int test_av1_symbol_writer_boundaries(void) {
    unsigned char output[1024];
    unsigned char bounded[1024];
    AvifencAv1SymbolWriter writer;
    AvifencAv1SymbolWriter sizing;
    uint16_t cdf[3] = { 16384U, 32768U, 0U };
    uint16_t invalid_cdf[3] = { 32768U, 16384U, 0U };
    size_t required;
    size_t index;

    avifenc_av1_symbol_writer_init_sizing(&sizing, 1);
    CHECK(write_symbol_sizing_sequence(&sizing) == AVIFENC_OK);
    required = avifenc_av1_symbol_writer_size(&sizing);
    CHECK(required > 512U && required < sizeof(output));
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 1);
    CHECK(write_symbol_sizing_sequence(&writer) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_size(&writer) == required);

    for (index = 0U; index < sizeof(bounded); ++index) bounded[index] = 0x5aU;
    avifenc_av1_symbol_writer_init(&writer, bounded, required - 1U, 1);
    CHECK(write_symbol_sizing_sequence(&writer) == AVIFENC_OUTPUT_TOO_SMALL);
    CHECK(avifenc_av1_symbol_writer_size(&writer) <= required - 1U);
    for (index = required - 1U; index < sizeof(bounded); ++index) {
        CHECK(bounded[index] == 0x5aU);
    }

    avifenc_av1_symbol_writer_init(&writer, 0, 1U, 0);
    CHECK(writer.status == AVIFENC_INVALID_ARGUMENT);
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_write(&writer, 0, 2U, 0U) ==
          AVIFENC_INVALID_ARGUMENT);
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_write(
              &writer, invalid_cdf, 2U, 0U) == AVIFENC_INVALID_ARGUMENT);
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_write(&writer, cdf, 2U, 2U) ==
          AVIFENC_INVALID_ARGUMENT);
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_literal(&writer, 0U, 33U) ==
          AVIFENC_INVALID_ARGUMENT);
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_literal(&writer, 8U, 3U) ==
          AVIFENC_INVALID_ARGUMENT);
    avifenc_av1_symbol_writer_init(&writer, output, sizeof(output), 0);
    CHECK(avifenc_av1_symbol_writer_literal(&writer, UINT32_MAX, 0U) ==
          AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_finish(&writer) == AVIFENC_OK);
    CHECK(avifenc_av1_symbol_writer_finish(&writer) ==
          AVIFENC_INVALID_ARGUMENT);

    avifenc_av1_symbol_writer_init_sizing(&sizing, 1);
    sizing.position = SIZE_MAX;
    for (index = 0U; index < 64U && sizing.status == AVIFENC_OK; ++index) {
        (void)avifenc_av1_symbol_writer_literal(&sizing, 0U, 1U);
    }
    CHECK(sizing.status == AVIFENC_OVERFLOW && sizing.position == SIZE_MAX);
    return 0;
}

typedef struct {
      uint32_t width;
      uint32_t height;
      uint8_t level;
      const uint8_t *bytes;
      size_t size;
} Av1HeaderGolden;

static int test_av1_header_writer(void) {
      static const uint8_t golden_2x2[] = {
            0x0aU, 0x08U, 0x18U, 0x00U, 0x34U, 0x08U, 0x08U, 0x08U,
            0x08U, 0x20U, 0x1aU, 0x05U, 0xe6U, 0x00U, 0x00U, 0x00U,
            0x06U, 0x22U, 0x01U, 0x00U
      };
      static const uint8_t golden_64x64[] = {
            0x0aU, 0x09U, 0x18U, 0x15U, 0x7fU, 0xfdU, 0x02U, 0x02U,
            0x02U, 0x02U, 0x08U, 0x1aU, 0x05U, 0xe6U, 0x00U, 0x00U,
            0x00U, 0x06U, 0x22U, 0x01U, 0x00U
      };
      static const uint8_t golden_130x66[] = {
            0x0aU, 0x09U, 0x18U, 0x1dU, 0xa0U, 0x60U, 0xa0U, 0x40U,
            0x40U, 0x40U, 0x41U, 0x1aU, 0x06U, 0xe4U, 0x80U, 0x00U,
            0x00U, 0x01U, 0x80U, 0x22U, 0x01U, 0x00U
      };
      static const uint8_t golden_640x480[] = {
            0x0aU, 0x0aU, 0x19U, 0x26U, 0x27U, 0xfeU, 0xfaU, 0x04U,
            0x04U, 0x04U, 0x04U, 0x10U, 0x1aU, 0x06U, 0xe4U, 0x80U,
            0x00U, 0x00U, 0x01U, 0x80U, 0x22U, 0x01U, 0x00U
      };
      static const Av1HeaderGolden golden[] = {
            { 2U, 2U, 0U, golden_2x2, sizeof(golden_2x2) },
            { 64U, 64U, 0U, golden_64x64, sizeof(golden_64x64) },
            { 130U, 66U, 0U, golden_130x66, sizeof(golden_130x66) },
            { 640U, 480U, 4U, golden_640x480, sizeof(golden_640x480) }
      };
      static unsigned char trace_workspace[700000U];
      unsigned char output[64];
      unsigned char bounded[64];
      AvifencAv1Config config = { 0 };
      AvifencByteWriter writer;
      AvifencByteWriter sizing;
      AvifdecImageInfo info;
      AvifdecEntropyTrace trace;
      AvifdecError error;
      AvifdecSpan span;
      AvifdecStatus trace_status;
      size_t index;
      size_t byte;
      uint8_t level;

      config.color.color_primaries = 1U;
      config.color.transfer_characteristics = 1U;
      config.color.matrix_coefficients = 1U;
      config.quantizer = 128U;
      for (index = 0U; index < sizeof(golden) / sizeof(golden[0]); ++index) {
            config.width = golden[index].width;
            config.height = golden[index].height;
            avifenc_byte_writer_init_sizing(&sizing);
            CHECK(avifenc_av1_write(&sizing, &config) == AVIFENC_OK);
            CHECK(avifenc_byte_writer_size(&sizing) == golden[index].size);
            avifenc_byte_writer_init(&writer, output, sizeof(output));
            CHECK(avifenc_av1_write(&writer, &config) == AVIFENC_OK);
            CHECK(avifenc_byte_writer_size(&writer) == golden[index].size);
            CHECK(avifdec_memory_compare(
                          output, golden[index].bytes, golden[index].size) == 0);

            span.data = output;
            span.size = avifenc_byte_writer_size(&writer);
            span.file_offset = 0U;
            avifdec_memory_fill(&info, 0U, sizeof(info));
            CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);
            CHECK(info.width == config.width && info.height == config.height &&
                    info.render_width == config.width &&
                    info.render_height == config.height && info.profile == 0U &&
                    info.level == golden[index].level && info.bit_depth == 8U &&
                    info.channel_count == 3U && info.subsampling_x == 1U &&
                    info.subsampling_y == 1U && info.chroma_sample_position == 0U);
            CHECK(info.reduced_still_picture_header == 1U &&
                    info.frame_type == 0U && info.base_q_index == 128U &&
                    info.coded_lossless == 0U &&
                    info.allow_screen_content_tools == 1U &&
                    info.allow_intrabc == 0U && info.enable_filter_intra == 1U &&
                    info.enable_intra_edge_filter == 0U &&
                    info.segmentation_enabled == 0U &&
                    info.delta_q_present == 0U && info.tx_mode == 1U &&
                    info.reduced_tx_set == 1U && info.superblock_size == 64U);
            CHECK(info.tile_columns == 1U && info.tile_rows == 1U &&
                    info.tile_count == 1U && info.tile_data_size == 1U &&
                    info.obu_count == 3U && info.film_grain_params_present == 0U);
            CHECK(info.color_primaries == 1U &&
                    info.transfer_characteristics == 1U &&
                    info.matrix_coefficients == 1U && info.color_range == 0U);
      }

      span.data = golden_2x2;
      span.size = sizeof(golden_2x2);
      span.file_offset = 0U;
      avifdec_memory_fill(&info, 0U, sizeof(info));
      CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);
      CHECK(info.workspace_required <= sizeof(trace_workspace));
      trace_status = avifdec_av1_trace(
            &span, 1U, 0, &info, trace_workspace,
            sizeof(trace_workspace), &trace, &error);
      CHECK(trace_status != AVIFDEC_OK);

      config.width = 2U;
      config.height = 2U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_av1_write(&sizing, &config) == AVIFENC_OK);
      for (byte = 0U; byte < sizeof(bounded); ++byte) bounded[byte] = 0x5aU;
      avifenc_byte_writer_init(
            &writer, bounded, avifenc_byte_writer_size(&sizing) - 1U);
      CHECK(avifenc_av1_write(&writer, &config) == AVIFENC_OUTPUT_TOO_SMALL);
      for (byte = avifenc_byte_writer_size(&sizing) - 1U;
             byte < sizeof(bounded); ++byte) {
            CHECK(bounded[byte] == 0x5aU);
      }

      CHECK(avifenc_av1_select_level(2U, 2U, &level) == AVIFENC_OK &&
              level == 0U);
      CHECK(avifenc_av1_select_level(2048U, 1152U, &level) == AVIFENC_OK &&
              level == 8U);
      CHECK(avifenc_av1_select_level(4096U, 2304U, &level) == AVIFENC_OK &&
              level == 16U);
      CHECK(avifenc_av1_select_level(16384U, 8704U, &level) == AVIFENC_OK &&
              level == 31U);
      CHECK(avifenc_av1_select_level(0U, 2U, &level) ==
              AVIFENC_INVALID_ARGUMENT);
      CHECK(avifenc_av1_select_level(2U, 2U, 0) == AVIFENC_INVALID_ARGUMENT);

      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_av1_write(&sizing, 0) == AVIFENC_INVALID_ARGUMENT);
      config.width = 3U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_av1_write(&sizing, &config) == AVIFENC_INVALID_ARGUMENT);
      config.width = 4098U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_av1_write(&sizing, &config) == AVIFENC_UNSUPPORTED);
      {
            AvifencAv1TileLayout layout;

            CHECK(avifenc_av1_tile_layout(
                        4096U, 2U, &layout) == AVIFENC_OK &&
                  layout.columns == 1U && layout.rows == 1U);
            CHECK(avifenc_av1_tile_layout(
                        4098U, 2U, &layout) == AVIFENC_OK &&
                  layout.columns == 2U && layout.rows == 1U &&
                  layout.columns_log2 == 1U);
            CHECK(avifenc_av1_tile_layout(
                        8192U, 64U, &layout) == AVIFENC_OK &&
                  layout.columns == 2U && layout.tile_width_sb == 64U);
            CHECK(avifenc_av1_tile_layout(
                        2242U, 4098U, &layout) == AVIFENC_OK &&
                  layout.columns == 1U && layout.rows == 2U &&
                  layout.rows_log2 == 1U);
            CHECK(avifenc_av1_tile_layout(
                        4098U, 4482U, &layout) == AVIFENC_OK &&
                  layout.columns == 2U && layout.rows == 2U &&
                  layout.columns_log2 == 1U && layout.rows_log2 == 1U);
      }
      config.width = 2U;
      config.quantizer = 256U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_av1_write(&sizing, &config) == AVIFENC_INVALID_ARGUMENT);
      config.quantizer = 128U;
      config.color.transfer_characteristics = 13U;
      config.color.matrix_coefficients = 0U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_av1_write(&sizing, &config) == AVIFENC_INVALID_ARGUMENT);
      return 0;
}

static int test_av1_forward_transform(void) {
      static const int16_t constant[16] = {
            1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1
      };
      static const int32_t constant_golden[16] = {
            31, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0
      };
      static const int16_t impulse[16] = {
            1, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0
      };
      static const int32_t impulse_golden[16] = {
            2, 3, 2, 1, 3, 4, 3, 2,
            2, 3, 2, 1, 1, 2, 1, 1
      };
      int32_t output[16];
      AvifencAv1TransformBlock block;

      CHECK(avifenc_av1_forward_dct_4x4(constant, 4U, output) == AVIFENC_OK);
      CHECK(avifdec_memory_compare(
                  output, constant_golden, sizeof(output)) == 0);
      CHECK(avifenc_av1_forward_dct_4x4(impulse, 4U, output) == AVIFENC_OK);
      CHECK(avifdec_memory_compare(
                  output, impulse_golden, sizeof(output)) == 0);
      CHECK(avifenc_av1_quantize_4x4(output, 128U, &block) == AVIFENC_OK &&
            block.eob == 0U);
      CHECK(avifenc_av1_forward_dct_4x4(0, 4U, output) ==
            AVIFENC_INVALID_ARGUMENT);
      CHECK(avifenc_av1_forward_dct_4x4(constant, 3U, output) ==
            AVIFENC_INVALID_ARGUMENT);
      CHECK(avifenc_av1_quantize_4x4(output, 0U, &block) ==
            AVIFENC_UNSUPPORTED);
      return 0;
}

static int test_av1_transform_trial(void) {
      static const uint8_t source[16] = {
            16U, 32U, 48U, 64U,
            24U, 40U, 56U, 72U,
            80U, 96U, 112U, 128U,
            88U, 104U, 120U, 136U
      };
      uint16_t reconstruction[16];
      uint8_t workspace[16];
      uint8_t workspace_snapshot[16];
      AvifencAv1TransformState state;
      AvifencAv1TransformState state_snapshot;
      AvifencAv1TransformBlock block;
      uint64_t distortion;
      uint64_t rate_cost;
      size_t required;

      CHECK(avifenc_av1_transform_context_size(1U, 1U, &required) ==
            AVIFENC_OK);
      CHECK(required <= sizeof(workspace));
      avifdec_memory_fill(reconstruction, 128U, sizeof(reconstruction));
      CHECK(avifenc_av1_transform_state_init(
                  &state, 128U, 1U, 1U, workspace, sizeof(workspace)) ==
            AVIFENC_OK);
      avifdec_memory_copy(&state_snapshot, &state, sizeof(state));
      avifdec_memory_copy(
            workspace_snapshot, workspace, sizeof(workspace));
      CHECK(avifenc_av1_transform_trial_4x4(
                  &state, 0U, 0U, 0U, source, 4U, 4U, 4U,
                  reconstruction, 4U, 128U, 1, &block,
                  &distortion, &rate_cost) == AVIFENC_OK);
      CHECK(distortion != 0U && rate_cost != 0U && block.eob != 0U);
      CHECK(avifdec_memory_compare(
                  &state, &state_snapshot, sizeof(state)) == 0);
      CHECK(avifdec_memory_compare(
                  workspace, workspace_snapshot, sizeof(workspace)) == 0);
      return 0;
}

static void test_av1_tile_source_pattern(uint8_t *plane,
                                         size_t stride,
                                         uint32_t width,
                                         uint32_t height,
                                         unsigned int pattern,
                                         unsigned int plane_index) {
      uint32_t state = 0x9e3779b9U + plane_index * 0x10203U;
      uint32_t row;
      uint32_t column;

      for (row = 0U; row < height; ++row) {
            for (column = 0U; column < width; ++column) {
                  uint8_t value = 128U;

                  if (pattern == 1U) {
                        value = (uint8_t)(plane_index == 0U ? 160U
                                          : plane_index == 1U ? 144U : 112U);
                  } else if (pattern == 2U && row == height / 2U &&
                             column == width / 2U) {
                        value = plane_index == 2U ? 0U : 255U;
                  } else if (pattern == 3U) {
                        value = ((row + column + plane_index) & 1U) != 0U
                              ? 255U : 0U;
                  } else if (pattern == 4U) {
                        state = state * 1664525U + 1013904223U;
                        value = (uint8_t)(state >> 24U);
                  } else if (pattern == 5U) {
                        value = column < width / 2U ? 32U : 224U;
                  } else if (pattern == 6U) {
                        value = row < height / 2U ? 32U : 224U;
                  }
                  plane[(size_t)row * stride + column] = value;
            }
      }
}

static int test_av1_tile_writer(void) {
      static const uint32_t dimensions[][2] = {
            { 2U, 2U }, { 8U, 8U }, { 8U, 8U },
            { 10U, 6U }, { 66U, 66U }, { 32U, 32U }, { 32U, 32U },
            { 32U, 32U }, { 32U, 32U }, { 16U, 16U }, { 16U, 16U }
      };
      static const uint8_t quantizers[] = {
            128U, 255U, 1U, 60U, 100U, 96U, 96U, 96U, 96U, 96U, 96U
      };
      static const uint8_t patterns[] = {
            0U, 1U, 2U, 3U, 4U, 1U, 3U, 5U, 6U, 5U, 6U
      };
      static uint8_t source_y[66U * 66U];
      static uint8_t source_u[33U * 33U];
      static uint8_t source_v[33U * 33U];
      static uint16_t reconstructed_y[72U * 72U];
      static uint16_t reconstructed_u[36U * 36U];
      static uint16_t reconstructed_v[36U * 36U];
      static uint16_t decoded_y[66U * 66U];
      static uint16_t decoded_u[33U * 33U];
      static uint16_t decoded_v[33U * 33U];
      static uint8_t decode_workspace[800000U];
      uint8_t tile[8192U];
      uint8_t repeated_tile[8192U];
      uint8_t short_tile[8192U];
      uint8_t av1[9216U];
      uint8_t tile_workspace[8192U];
      AvifencAv1TileSource source = {
            { 0 }, { 0 }, 0U, 0U, 128U, 0U, 0
      };
      AvifencAv1TileReconstruction reconstruction = {
            { reconstructed_y, reconstructed_u, reconstructed_v },
            { 72U, 36U, 36U }, { 72U, 36U, 36U }, { 72U, 36U, 36U }
      };
      AvifencAv1TileRequirements requirements;
      AvifencAv1SymbolWriter symbol_writer;
      AvifencByteWriter av1_writer;
      AvifencAv1Config config = { 0 };
      AvifdecSpan span;
      AvifdecImageInfo info;
      AvifdecImage image = { { 0 }, { 0 }, { 0 }, { 0 }, 0U, 0U, 0U, 0U,
                             0, 0U, 0U, 0U, 0U, 0U, 0U };
      AvifdecEntropyTrace trace;
      AvifdecError error;
      AvifdecStatus decode_status;
      AvifencStatistics statistics;
      size_t tile_size;
      size_t repeated_size;
      size_t index;
      size_t row;
      size_t column;
      size_t workspace_offset;

      source.planes[0] = source_y;
      source.planes[1] = source_u;
      source.planes[2] = source_v;
      source.statistics = &statistics;
      source.strides[0] = 66U;
      source.strides[1] = 33U;
      source.strides[2] = 33U;
      config.quantizer = 128U;
      config.color.color_primaries = 1U;
      config.color.transfer_characteristics = 1U;
      config.color.matrix_coefficients = 1U;
      image.planes[0] = decoded_y;
      image.planes[1] = decoded_u;
      image.planes[2] = decoded_v;

      for (index = 0U; index < sizeof(dimensions) / sizeof(dimensions[0]);
           ++index) {
            avifdec_memory_fill(&statistics, 0U, sizeof(statistics));
            source.width = dimensions[index][0];
            source.height = dimensions[index][1];
            source.quantizer = quantizers[index];
            avifdec_memory_fill(source_y, 128U, sizeof(source_y));
            avifdec_memory_fill(source_u, 128U, sizeof(source_u));
            avifdec_memory_fill(source_v, 128U, sizeof(source_v));
            test_av1_tile_source_pattern(
                  source_y, source.strides[0], source.width, source.height,
                  patterns[index], 0U);
            test_av1_tile_source_pattern(
                  source_u, source.strides[1], source.width >> 1U,
                  source.height >> 1U, patterns[index], 1U);
            test_av1_tile_source_pattern(
                  source_v, source.strides[2], source.width >> 1U,
                  source.height >> 1U, patterns[index], 2U);
            config.width = source.width;
            config.height = source.height;
            config.quantizer = source.quantizer;
            CHECK(avifenc_av1_tile_query(&source, &requirements) ==
                  AVIFENC_OK);
            CHECK(requirements.workspace_required <= sizeof(tile_workspace));
            CHECK(requirements.reconstruction_widths[0] ==
                  8U * ((source.width + 7U) >> 3U));
            CHECK(requirements.reconstruction_heights[0] ==
                  8U * ((source.height + 7U) >> 3U));

            avifenc_av1_symbol_writer_init_sizing(&symbol_writer, 1);
            CHECK(avifenc_av1_tile_write(
                        &symbol_writer, &source, &reconstruction,
                        tile_workspace, sizeof(tile_workspace)) == AVIFENC_OK);
            tile_size = avifenc_av1_symbol_writer_size(&symbol_writer);
            CHECK(tile_size != 0U && tile_size <= sizeof(tile));

            avifdec_memory_fill(reconstructed_y, 0xffU,
                                sizeof(reconstructed_y));
            avifdec_memory_fill(reconstructed_u, 0xffU,
                                sizeof(reconstructed_u));
            avifdec_memory_fill(reconstructed_v, 0xffU,
                                sizeof(reconstructed_v));
            avifenc_av1_symbol_writer_init(
                  &symbol_writer, tile, sizeof(tile), 1);
            CHECK(avifenc_av1_tile_write(
                        &symbol_writer, &source, &reconstruction,
                        tile_workspace, sizeof(tile_workspace)) == AVIFENC_OK);
            CHECK(avifenc_av1_symbol_writer_size(&symbol_writer) == tile_size);
            if (index == 0U) {
                  for (row = 0U;
                       row < requirements.reconstruction_heights[0]; ++row) {
                        for (column = 0U;
                             column < requirements.reconstruction_widths[0];
                             ++column) {
                              CHECK(reconstructed_y[row * 72U + column] == 128U);
                        }
                  }
                  for (row = 0U;
                       row < requirements.reconstruction_heights[1]; ++row) {
                        for (column = 0U;
                             column < requirements.reconstruction_widths[1];
                             ++column) {
                              CHECK(reconstructed_u[row * 36U + column] == 128U);
                              CHECK(reconstructed_v[row * 36U + column] == 128U);
                        }
                  }
            }

            avifenc_av1_symbol_writer_init(
                  &symbol_writer, repeated_tile, sizeof(repeated_tile), 1);
            CHECK(avifenc_av1_tile_write(
                        &symbol_writer, &source, &reconstruction,
                        tile_workspace, sizeof(tile_workspace)) == AVIFENC_OK);
            repeated_size = avifenc_av1_symbol_writer_size(&symbol_writer);
            CHECK(repeated_size == tile_size &&
                  avifdec_memory_compare(tile, repeated_tile, tile_size) == 0);
            if (index + 1U == sizeof(dimensions) / sizeof(dimensions[0])) {
                  for (workspace_offset = 0U; workspace_offset < 16U;
                       ++workspace_offset) {
                        avifenc_av1_symbol_writer_init(
                              &symbol_writer, repeated_tile,
                              sizeof(repeated_tile), 1);
                        CHECK(avifenc_av1_tile_write(
                                    &symbol_writer, &source, &reconstruction,
                                    tile_workspace + workspace_offset,
                                    sizeof(tile_workspace) - workspace_offset) ==
                              AVIFENC_OK);
                        CHECK(avifenc_av1_symbol_writer_size(&symbol_writer) ==
                              tile_size);
                        CHECK(avifdec_memory_compare(
                                    tile, repeated_tile, tile_size) == 0);
                  }
            }

            avifenc_byte_writer_init(&av1_writer, av1, sizeof(av1));
            CHECK(avifenc_av1_write_with_tile(
                        &av1_writer, &config, tile, tile_size) == AVIFENC_OK);
            span.data = av1;
            span.size = avifenc_byte_writer_size(&av1_writer);
            span.file_offset = 0U;
            avifdec_memory_fill(&info, 0U, sizeof(info));
            CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) ==
                  AVIFDEC_OK);
            CHECK(info.workspace_required <= sizeof(decode_workspace));
            image.strides[0] = source.width;
            image.strides[1] = source.width >> 1U;
            image.strides[2] = source.width >> 1U;
            avifdec_memory_fill(&trace, 0U, sizeof(trace));
            decode_status = avifdec_av1_decode(
                  &span, 1U, 0, &info, decode_workspace,
                  sizeof(decode_workspace), &image, &trace, &error);
            CHECK(decode_status == AVIFDEC_OK);
            if (index == 5U) {
                  CHECK(trace.block_count == 1U &&
                        trace.transform_size_mask ==
                              ((uint32_t)1U << AV1_TX_16X16 | 1U << AV1_TX_32X32));
            }
            if (index == 2U) {
                  CHECK(statistics.palette_block_count == 6U &&
                        statistics.luma_mode_mask == 1U &&
                        statistics.chroma_mode_mask == 1U);
            }
            if (index == 6U) {
                  CHECK(trace.block_count == 28U);
                  CHECK(trace.transform_size_mask ==
                        ((uint32_t)1U << AV1_TX_4X4 |
                         (uint32_t)1U << AV1_TX_8X8));
                  CHECK(statistics.palette_block_count == 0U &&
                        statistics.luma_mode_mask == 0x19U &&
                        statistics.chroma_mode_mask == 0x19U);
            }
            if (index == 7U) {
                  CHECK(trace.block_count == 2U &&
                        trace.transform_size_mask ==
                              ((uint32_t)1U << AV1_TX_8X16 |
                               (uint32_t)1U << AV1_TX_16X32));
            }
            if (index == 8U) {
                  CHECK(trace.block_count == 2U &&
                        trace.transform_size_mask ==
                              ((uint32_t)1U << AV1_TX_16X8 |
                               (uint32_t)1U << AV1_TX_32X16));
            }
            if (index == 9U) {
                  CHECK(trace.block_count == 2U &&
                        trace.transform_size_mask ==
                              ((uint32_t)1U << AV1_TX_4X8 |
                               (uint32_t)1U << AV1_TX_8X16));
            }
            if (index == 10U) {
                  CHECK(trace.block_count == 2U &&
                        trace.transform_size_mask ==
                              ((uint32_t)1U << AV1_TX_8X4 |
                               (uint32_t)1U << AV1_TX_16X8));
            }
                                    CHECK(index == 0U || index == 2U
                                                      ? trace.nonzero_transform_count == 0U &&
                                                            trace.coefficient_count == 0U
                                                      : trace.nonzero_transform_count != 0U &&
                                                            trace.coefficient_count != 0U);
            CHECK(image.widths[0] == source.width &&
                  image.heights[0] == source.height &&
                  image.widths[1] == (source.width >> 1U) &&
                  image.heights[1] == (source.height >> 1U));
            for (row = 0U; row < source.height; ++row) {
                  for (column = 0U; column < source.width; ++column) {
                        CHECK(decoded_y[row * source.width + column] ==
                              reconstructed_y[row * 72U + column]);
                  }
            }
            for (row = 0U; row < (source.height >> 1U); ++row) {
                  for (column = 0U; column < (source.width >> 1U); ++column) {
                        CHECK(decoded_u[row * (source.width >> 1U) + column] ==
                              reconstructed_u[row * 36U + column]);
                        CHECK(decoded_v[row * (source.width >> 1U) + column] ==
                              reconstructed_v[row * 36U + column]);
                  }
            }

            avifdec_memory_fill(short_tile, 0x5aU, sizeof(short_tile));
            avifenc_av1_symbol_writer_init(
                  &symbol_writer, short_tile, tile_size - 1U, 1);
            CHECK(avifenc_av1_tile_write(
                        &symbol_writer, &source, &reconstruction,
                        tile_workspace, sizeof(tile_workspace)) ==
                  AVIFENC_OUTPUT_TOO_SMALL);
            for (row = tile_size - 1U; row < sizeof(short_tile); ++row) {
                  CHECK(short_tile[row] == 0x5aU);
            }
      }

      source.width = 3U;
      CHECK(avifenc_av1_tile_query(&source, &requirements) ==
            AVIFENC_INVALID_ARGUMENT);
      source.width = 2U;
      source.height = 2U;
      source.quantizer = 0U;
      CHECK(avifenc_av1_tile_query(&source, &requirements) ==
            AVIFENC_UNSUPPORTED);
      source.quantizer = 128U;
      CHECK(avifenc_av1_tile_query(0, &requirements) ==
            AVIFENC_INVALID_ARGUMENT);
      CHECK(avifenc_av1_tile_query(&source, 0) == AVIFENC_INVALID_ARGUMENT);
      CHECK(avifenc_av1_tile_query(&source, &requirements) == AVIFENC_OK);
      avifenc_av1_symbol_writer_init(
            &symbol_writer, tile, sizeof(tile), 1);
      CHECK(avifenc_av1_tile_write(
                  &symbol_writer, &source, &reconstruction,
                  tile_workspace, requirements.workspace_required - 1U) ==
            AVIFENC_OUT_OF_MEMORY);
      return 0;
}

static int test_av1_avif_integration(void) {
      unsigned char av1[64];
      unsigned char avif[512];
      AvifencAv1Config av1_config = { 0 };
      AvifencAvifConfig avif_config = { 0 };
      AvifencByteWriter av1_writer;
      AvifencByteWriter avif_writer;
      AvifdecImageInfo info;
      AvifdecError error;
      uint8_t level;

      av1_config.width = 640U;
      av1_config.height = 480U;
      av1_config.quantizer = 128U;
      av1_config.color.color_primaries = 9U;
      av1_config.color.transfer_characteristics = 16U;
      av1_config.color.matrix_coefficients = 9U;
      av1_config.color.full_range = 1U;
      av1_config.color.chroma_sample_position = 2U;
      avifenc_byte_writer_init(&av1_writer, av1, sizeof(av1));
      CHECK(avifenc_av1_write(&av1_writer, &av1_config) == AVIFENC_OK);
      CHECK(avifenc_av1_select_level(
                    av1_config.width, av1_config.height, &level) == AVIFENC_OK);

      avif_config.width = av1_config.width;
      avif_config.height = av1_config.height;
      avif_config.color = av1_config.color;
      avif_config.seq_level_idx_0 = level;
      avifenc_byte_writer_init(&avif_writer, avif, sizeof(avif));
      CHECK(avifenc_avif_write(
                    &avif_writer, &avif_config, av1,
                    avifenc_byte_writer_size(&av1_writer)) == AVIFENC_OK);
      avifdec_memory_fill(&info, 0U, sizeof(info));
      CHECK(avifdec_query(
                    avif, avifenc_byte_writer_size(&avif_writer), 0, 0, 0,
                    &info, &error) == AVIFDEC_OK);
      CHECK(info.width == av1_config.width && info.height == av1_config.height &&
              info.level == level && info.profile == 0U &&
              info.bit_depth == 8U && info.subsampling_x == 1U &&
              info.subsampling_y == 1U &&
              info.chroma_sample_position ==
                    av1_config.color.chroma_sample_position &&
              info.color_primaries == av1_config.color.color_primaries &&
              info.transfer_characteristics ==
                    av1_config.color.transfer_characteristics &&
              info.matrix_coefficients ==
                    av1_config.color.matrix_coefficients &&
              info.color_range == av1_config.color.full_range);
      return 0;
}

typedef struct {
      AvifdecBmffBox boxes[32];
      size_t count;
} EncoderBoxTrace;

static void encoder_box_visitor(const AvifdecBmffBox *box, void *user_data) {
      EncoderBoxTrace *trace = (EncoderBoxTrace *)user_data;

      if (trace->count < sizeof(trace->boxes) / sizeof(trace->boxes[0])) {
            trace->boxes[trace->count++] = *box;
      }
}

static const AvifdecBmffBox *encoder_find_box(const EncoderBoxTrace *trace,
                                                                    uint32_t type) {
      size_t index;

      for (index = 0U; index < trace->count; ++index) {
            if (trace->boxes[index].type == type) return &trace->boxes[index];
      }
      return 0;
}

static int test_avif_writer(void) {
      static const unsigned char placeholder_av1[] = { 0xffU, 0x00U, 0x55U };
      static const uint32_t expected_types[] = {
            AVIFDEC_FOURCC('f', 't', 'y', 'p'),
            AVIFDEC_FOURCC('m', 'e', 't', 'a'),
            AVIFDEC_FOURCC('h', 'd', 'l', 'r'),
            AVIFDEC_FOURCC('p', 'i', 't', 'm'),
            AVIFDEC_FOURCC('i', 'l', 'o', 'c'),
            AVIFDEC_FOURCC('i', 'i', 'n', 'f'),
            AVIFDEC_FOURCC('i', 'n', 'f', 'e'),
            AVIFDEC_FOURCC('i', 'p', 'r', 'p'),
            AVIFDEC_FOURCC('i', 'p', 'c', 'o'),
            AVIFDEC_FOURCC('i', 's', 'p', 'e'),
            AVIFDEC_FOURCC('p', 'i', 'x', 'i'),
            AVIFDEC_FOURCC('a', 'v', '1', 'C'),
            AVIFDEC_FOURCC('c', 'o', 'l', 'r'),
            AVIFDEC_FOURCC('i', 'p', 'm', 'a'),
            AVIFDEC_FOURCC('m', 'd', 'a', 't')
      };
      static const size_t expected_depths[] = {
            0U, 0U, 1U, 1U, 1U, 1U, 2U, 1U, 2U, 3U, 3U, 3U, 3U, 2U, 0U
      };
      unsigned char output[512];
      unsigned char repeated[512];
      unsigned char bounded[512];
      AvifencAvifConfig config = { 0 };
      AvifencByteWriter writer;
      AvifencByteWriter sizing;
      AvifdecBmffInfo bmff_info;
      AvifdecBmffLimits bmff_limits = { 8U, 32U };
      AvifdecImageInfo image_info;
      AvifdecError error;
      EncoderBoxTrace trace = { { { 0 } }, 0U };
      const AvifdecBmffBox *iloc;
      const AvifdecBmffBox *mdat;
      size_t required;
      size_t index;
      AvifdecStatus decode_status;

      config.width = 16U;
      config.height = 8U;
      config.color.color_primaries = 1U;
      config.color.transfer_characteristics = 13U;
      config.color.matrix_coefficients = 6U;
      config.color.full_range = 1U;
      config.color.chroma_sample_position = 2U;
      config.seq_level_idx_0 = 4U;

      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_avif_write(
                    &sizing, &config, placeholder_av1,
                    sizeof(placeholder_av1)) == AVIFENC_OK);
      required = avifenc_byte_writer_size(&sizing);
      CHECK(required != 0U && required <= sizeof(output));

      avifenc_byte_writer_init(&writer, output, sizeof(output));
      CHECK(avifenc_avif_write(
                    &writer, &config, placeholder_av1,
                    sizeof(placeholder_av1)) == AVIFENC_OK);
      CHECK(avifenc_byte_writer_size(&writer) == required);
      avifenc_byte_writer_init(&writer, repeated, sizeof(repeated));
      CHECK(avifenc_avif_write(
                    &writer, &config, placeholder_av1,
                    sizeof(placeholder_av1)) == AVIFENC_OK);
      CHECK(avifenc_byte_writer_size(&writer) == required);
      CHECK(avifdec_memory_compare(output, repeated, required) == 0);

      CHECK(avifdec_bmff_inspect(
                    output, required, &bmff_limits, encoder_box_visitor, &trace,
                    &bmff_info, &error) == AVIFDEC_OK);
      CHECK(trace.count == sizeof(expected_types) / sizeof(expected_types[0]));
      for (index = 0U; index < trace.count; ++index) {
            CHECK(trace.boxes[index].type == expected_types[index]);
            CHECK(trace.boxes[index].depth == expected_depths[index]);
      }
      CHECK(bmff_info.has_avif_brand && bmff_info.meta_count == 1U &&
              bmff_info.handler_count == 1U && bmff_info.media_data_count == 1U);
      CHECK(bmff_info.maximum_depth == 3U);

      iloc = encoder_find_box(&trace, AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
      mdat = encoder_find_box(&trace, AVIFDEC_FOURCC('m', 'd', 'a', 't'));
      CHECK(iloc != 0 && mdat != 0 && iloc->payload_size == 22U);
      CHECK(avifdec_load_u32be(output + iloc->payload_offset + 14U) ==
              mdat->payload_offset);
      CHECK(avifdec_load_u32be(output + iloc->payload_offset + 18U) ==
              sizeof(placeholder_av1));
      CHECK(mdat->payload_size == sizeof(placeholder_av1));
      CHECK(avifdec_memory_compare(
                    output + mdat->payload_offset, placeholder_av1,
                    sizeof(placeholder_av1)) == 0);

      decode_status = avifdec_query(
            output, required, 0, 0, 0, &image_info, &error);
      CHECK(decode_status != AVIFDEC_OK);
      CHECK(image_info.width == config.width &&
              image_info.height == config.height && image_info.profile == 0U &&
              image_info.level == config.seq_level_idx_0 &&
              image_info.bit_depth == 8U && image_info.channel_count == 3U &&
              image_info.subsampling_x == 1U && image_info.subsampling_y == 1U &&
              image_info.chroma_sample_position ==
                    config.color.chroma_sample_position &&
              image_info.color_primaries == config.color.color_primaries &&
              image_info.transfer_characteristics ==
                    config.color.transfer_characteristics &&
              image_info.matrix_coefficients == config.color.matrix_coefficients &&
              image_info.color_range == config.color.full_range);

      for (index = 0U; index < sizeof(bounded); ++index) bounded[index] = 0x5aU;
      avifenc_byte_writer_init(&writer, bounded, required - 1U);
      CHECK(avifenc_avif_write(
                    &writer, &config, placeholder_av1,
                    sizeof(placeholder_av1)) == AVIFENC_OUTPUT_TOO_SMALL);
      CHECK(writer.position <= required - 1U);
      for (index = required - 1U; index < sizeof(bounded); ++index) {
            CHECK(bounded[index] == 0x5aU);
      }

      config.width = 15U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_avif_write(
                    &sizing, &config, placeholder_av1,
                    sizeof(placeholder_av1)) == AVIFENC_INVALID_ARGUMENT);
      config.width = 16U;
      config.seq_level_idx_0 = 32U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_avif_write(
                    &sizing, &config, placeholder_av1,
                    sizeof(placeholder_av1)) == AVIFENC_INVALID_ARGUMENT);
      config.seq_level_idx_0 = 4U;
      avifenc_byte_writer_init_sizing(&sizing);
      CHECK(avifenc_avif_write(
                    &sizing, &config, placeholder_av1,
                    (size_t)UINT32_MAX) == AVIFENC_LIMIT_EXCEEDED);
      CHECK(sizing.position == 0U);
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
      CHECK(options.speed == AVIFENC_DEFAULT_SPEED);
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
    options.speed = AVIFENC_MAX_SPEED + 1U;
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_OPTIONS &&
          error.required_size == AVIFENC_MAX_SPEED &&
          error.provided_size == AVIFENC_MAX_SPEED + 1U);
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
    CHECK(requirements.workspace_required != 0U);
    CHECK(requirements.output_capacity_required != 0U);
    return 0;
}

static int test_encode_boundaries(void) {
    static unsigned char workspace[20000];
    static unsigned char decode_workspace[800000];
    static unsigned char output[10000];
      static unsigned char measured_output[10000];
    static uint16_t decoded_y[4];
    static uint16_t decoded_u[1];
    static uint16_t decoded_v[1];
    AvifencImage image = valid_image();
    AvifencOptions options;
    AvifencRequirements requirements;
    AvifdecImageInfo info;
    AvifdecImage decoded = {
        { decoded_y, decoded_u, decoded_v }, { 2U, 1U, 1U },
        { 0U, 0U, 0U }, { 0U, 0U, 0U }, 0U, 0U, 0U, 0U,
        0, 0U, 0U, 0U, 0U, 0U, 0U
    };
    AvifencError error;
      AvifencStatistics statistics;
    AvifdecError decode_error;
    size_t output_written = 99U;
      size_t measured_written = 0U;

    avifenc_options_default(&options);
    CHECK(avifenc_query(&image, &options, &requirements, &error) ==
          AVIFENC_OK);
    CHECK(requirements.workspace_required <= sizeof(workspace));
    CHECK(requirements.output_capacity_required <= sizeof(output));
    CHECK(avifenc_encode(&image, &options, workspace,
                         requirements.workspace_required,
                         output, sizeof(output), 0, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_OUTPUT);
    CHECK(avifenc_encode(&image, &options, workspace,
                         requirements.workspace_required - 1U,
                         output, sizeof(output), &output_written, &error) ==
          AVIFENC_OUT_OF_MEMORY);
    CHECK(output_written == 0U && error.context == AVIFENC_CONTEXT_WORKSPACE);
    CHECK(error.required_size == requirements.workspace_required &&
          error.provided_size == requirements.workspace_required - 1U);
    CHECK(avifenc_encode(&image, &options, 0,
                         requirements.workspace_required,
                         output, sizeof(output), &output_written, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_WORKSPACE);
    CHECK(avifenc_encode(&image, &options, workspace,
                         requirements.workspace_required,
                         output, requirements.output_capacity_required - 1U,
                         &output_written, &error) ==
          AVIFENC_OUTPUT_TOO_SMALL);
    CHECK(error.required_size == requirements.output_capacity_required &&
          error.provided_size == requirements.output_capacity_required - 1U);
    CHECK(avifenc_encode(&image, &options, workspace,
                         requirements.workspace_required,
                         0, sizeof(output), &output_written, &error) ==
          AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_OUTPUT);

    CHECK(avifenc_encode(&image, &options, workspace,
                         requirements.workspace_required,
                         output, sizeof(output), &output_written, &error) ==
          AVIFENC_OK);
    avifdec_memory_fill(&statistics, 0xffU, sizeof(statistics));
    CHECK(avifenc_encode_ex(
              &image, &options, workspace, requirements.workspace_required,
                    measured_output, sizeof(measured_output), &measured_written,
                    &statistics, &error) ==
          AVIFENC_OK);
      CHECK(measured_written == output_written &&
              avifdec_memory_compare(
                    measured_output, output, output_written) == 0);
    CHECK(statistics.tile_count == 1U &&
          statistics.partition_node_count == 8U &&
          statistics.block_count == 4U &&
          statistics.prediction_trial_count == 20U &&
          statistics.transform_trial_count == 20U &&
          statistics.transform_count == 6U &&
          statistics.entropy_symbol_count != 0U &&
          statistics.filter_unit_count == 0U);
    CHECK(output_written != 0U &&
          output_written <= requirements.output_capacity_required);
    avifdec_memory_fill(&info, 0U, sizeof(info));
    CHECK(avifdec_query(output, output_written, 0, 0, 0,
                        &info, &decode_error) == AVIFDEC_OK);
    CHECK(info.width == 2U && info.height == 2U &&
          info.base_q_index == options.quantizer &&
          info.workspace_required <= sizeof(decode_workspace));
    CHECK(avifdec_decode(output, output_written, 0,
                         decode_workspace, sizeof(decode_workspace),
                         &decoded, 0, &decode_error) == AVIFDEC_OK);
    CHECK(decoded.widths[0] == 2U && decoded.heights[0] == 2U &&
          decoded.widths[1] == 1U && decoded.heights[1] == 1U &&
          decoded.widths[2] == 1U && decoded.heights[2] == 1U &&
          decoded.bit_depth == 8U && decoded.subsampling_x == 1U &&
          decoded.subsampling_y == 1U);
    return 0;
}

static uint64_t quality_checksum(const uint8_t *data, size_t size) {
      uint64_t checksum = 1469598103934665603ULL;
      size_t index;

      for (index = 0U; index < size; ++index) {
            checksum ^= data[index];
            checksum *= 1099511628211ULL;
      }
      return checksum;
}

static uint64_t quality_plane_sse(const uint8_t *source,
                                                  size_t source_stride,
                                                  const uint16_t *decoded,
                                                  size_t decoded_stride,
                                                  uint32_t width,
                                                  uint32_t height) {
      uint64_t distortion = 0U;
      uint32_t row;
      uint32_t column;

      for (row = 0U; row < height; ++row) {
            for (column = 0U; column < width; ++column) {
                  int32_t difference =
                        (int32_t)source[(size_t)row * source_stride + column] -
                        (int32_t)decoded[(size_t)row * decoded_stride + column];

                  distortion += (uint64_t)(difference * difference);
            }
      }
      return distortion;
}

static int test_quality_controls(void) {
      static const uint16_t quantizers[3] = { 32U, 96U, 192U };
                  static const uint64_t expected_checksums[3][3] = {
                                    { 0xe7c40708e44cda83ULL, 0x319dfbf50fd1c14bULL,
                                          0x45e02d95bd14929eULL },
                                    { 0x5192b442f625d5eeULL, 0x3d80e1e445745082ULL,
                                          0x370ff10a169e887cULL },
                                    { 0xb87c854ffc96b7e8ULL, 0xe55db4ee70ebce27ULL,
                                          0x76f0789fe9a826e1ULL }
                  };
                  static const size_t minimum_sizes[3] = { 1366U, 1066U, 626U };
                  static const size_t maximum_sizes[3] = { 1400U, 1100U, 660U };
                  static const uint64_t minimum_sse[3] = { 220000U, 230000U, 400000U };
                  static const uint64_t maximum_sse[3] = { 250000U, 265000U, 450000U };
      static uint8_t source_y[32U * 32U];
      static uint8_t source_u[16U * 16U];
      static uint8_t source_v[16U * 16U];
      static uint8_t flat_y[32U * 32U];
      static uint8_t flat_u[16U * 16U];
      static uint8_t flat_v[16U * 16U];
      static uint8_t workspace[100000U];
      static uint8_t output[30000U];
      static uint8_t repeated[30000U];
      static uint8_t baseline[30000U];
      static uint8_t decode_workspace[800000U];
      static uint16_t decoded_y[32U * 32U];
      static uint16_t decoded_u[16U * 16U];
      static uint16_t decoded_v[16U * 16U];
      AvifencImage image = { 0 };
      AvifencImage flat = { 0 };
      AvifencOptions options;
      AvifencRequirements requirements;
      AvifencRequirements baseline_requirements;
      AvifencRequirements flat_requirements;
      AvifencError error;
      AvifdecImage decoded = {
            { decoded_y, decoded_u, decoded_v }, { 32U, 16U, 16U },
            { 0U, 0U, 0U }, { 0U, 0U, 0U }, 0U, 0U, 0U, 0U,
            0, 0U, 0U, 0U, 0U, 0U, 0U
      };
      AvifdecError decode_error;
      uint64_t previous_sse = 0U;
      size_t previous_size = SIZE_MAX;
      uint32_t row;
      uint32_t column;
      unsigned int index;

      for (row = 0U; row < 32U; ++row) {
            for (column = 0U; column < 32U; ++column) {
                  source_y[(size_t)row * 32U + column] = (uint8_t)(
                        column * 7U + row * 5U + ((column ^ row) & 7U) * 11U);
            }
      }
      for (row = 0U; row < 16U; ++row) {
            for (column = 0U; column < 16U; ++column) {
                  source_u[(size_t)row * 16U + column] =
                        (uint8_t)(32U + ((column + row) & 1U) * 96U);
                  source_v[(size_t)row * 16U + column] =
                        (uint8_t)(224U - ((column + row) & 1U) * 96U);
            }
      }
      avifdec_memory_fill(flat_y, 128U, sizeof(flat_y));
      avifdec_memory_fill(flat_u, 128U, sizeof(flat_u));
      avifdec_memory_fill(flat_v, 128U, sizeof(flat_v));
      image.planes[0] = source_y;
      image.planes[1] = source_u;
      image.planes[2] = source_v;
      image.strides[0] = 32U;
      image.strides[1] = 16U;
      image.strides[2] = 16U;
      image.width = 32U;
      image.height = 32U;
      image.color.color_primaries = 1U;
      image.color.transfer_characteristics = 1U;
      image.color.matrix_coefficients = 1U;
      flat = image;
      flat.planes[0] = flat_y;
      flat.planes[1] = flat_u;
      flat.planes[2] = flat_v;

      for (index = 0U; index < 3U; ++index) {
            size_t output_written;
            size_t repeated_written;
            size_t baseline_written;
            size_t middle_size;
            uint64_t full_sse;
            uint64_t middle_sse;
            uint64_t baseline_sse;
            uint64_t full_checksum;
            uint64_t middle_checksum;
            uint64_t baseline_checksum;

            avifenc_options_default(&options);
            options.quantizer = quantizers[index];
            CHECK(avifenc_query(&image, &options, &requirements, &error) ==
                    AVIFENC_OK);
            CHECK(avifenc_query(&flat, &options, &flat_requirements, &error) ==
                    AVIFENC_OK);
            CHECK(requirements.workspace_required ==
                          flat_requirements.workspace_required &&
                    requirements.output_capacity_required ==
                          flat_requirements.output_capacity_required &&
                    requirements.workspace_required <= sizeof(workspace) &&
                    requirements.output_capacity_required <= sizeof(output));
            options.speed = AVIFENC_MAX_SPEED;
            CHECK(avifenc_query(
                          &image, &options, &baseline_requirements, &error) ==
                    AVIFENC_OK);
            CHECK(requirements.workspace_required ==
                          baseline_requirements.workspace_required &&
                    requirements.output_capacity_required ==
                          baseline_requirements.output_capacity_required);

            options.speed = AVIFENC_DEFAULT_SPEED;
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          output, sizeof(output), &output_written, &error) ==
                    AVIFENC_OK);
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          repeated, sizeof(repeated), &repeated_written, &error) ==
                    AVIFENC_OK);
            CHECK(output_written == repeated_written &&
                    avifdec_memory_compare(
                          output, repeated, output_written) == 0);
            full_checksum = quality_checksum(output, output_written);
            CHECK(output_written >= minimum_sizes[index] &&
                    output_written <= maximum_sizes[index]);
            CHECK(avifdec_decode(
                          output, output_written, 0, decode_workspace,
                          sizeof(decode_workspace), &decoded, 0, &decode_error) ==
                    AVIFDEC_OK);
            full_sse = quality_plane_sse(
                  source_y, 32U, decoded_y, 32U, 32U, 32U) +
                  quality_plane_sse(
                        source_u, 16U, decoded_u, 16U, 16U, 16U) +
                  quality_plane_sse(
                        source_v, 16U, decoded_v, 16U, 16U, 16U);
            CHECK(full_sse >= minimum_sse[index] &&
                    full_sse <= maximum_sse[index]);

            options.speed = 1U;
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          repeated, sizeof(repeated), &repeated_written, &error) ==
                    AVIFENC_OK);
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          baseline, sizeof(baseline), &baseline_written, &error) ==
                    AVIFENC_OK);
            CHECK(repeated_written == baseline_written &&
                    avifdec_memory_compare(
                          repeated, baseline, repeated_written) == 0);
            middle_checksum = quality_checksum(repeated, repeated_written);
            middle_size = repeated_written;
            CHECK(avifdec_decode(
                          repeated, repeated_written, 0, decode_workspace,
                          sizeof(decode_workspace), &decoded, 0, &decode_error) ==
                    AVIFDEC_OK);
            middle_sse = quality_plane_sse(
                  source_y, 32U, decoded_y, 32U, 32U, 32U) +
                  quality_plane_sse(
                        source_u, 16U, decoded_u, 16U, 16U, 16U) +
                  quality_plane_sse(
                        source_v, 16U, decoded_v, 16U, 16U, 16U);

            options.speed = AVIFENC_MAX_SPEED;
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          baseline, sizeof(baseline), &baseline_written, &error) ==
                    AVIFENC_OK);
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          repeated, sizeof(repeated), &repeated_written, &error) ==
                    AVIFENC_OK);
            CHECK(baseline_written == repeated_written &&
                    avifdec_memory_compare(
                          baseline, repeated, baseline_written) == 0);
            baseline_checksum = quality_checksum(baseline, baseline_written);
            CHECK(avifdec_decode(
                          baseline, baseline_written, 0, decode_workspace,
                          sizeof(decode_workspace), &decoded, 0, &decode_error) ==
                    AVIFDEC_OK);
            baseline_sse = quality_plane_sse(
                  source_y, 32U, decoded_y, 32U, 32U, 32U) +
                  quality_plane_sse(
                        source_u, 16U, decoded_u, 16U, 16U, 16U) +
                  quality_plane_sse(
                        source_v, 16U, decoded_v, 16U, 16U, 16U);
            CHECK(full_checksum == expected_checksums[index][0] &&
                  middle_checksum == expected_checksums[index][1] &&
                  baseline_checksum == expected_checksums[index][2] &&
                  middle_size != 0U);
            CHECK(full_sse <= middle_sse && middle_sse < baseline_sse);
            if (index != 0U) {
                  CHECK(full_sse > previous_sse && output_written < previous_size);
            }
            previous_sse = full_sse;
            previous_size = output_written;
      }
      for (index = 0U; index < 2U; ++index) {
            size_t searched_written;
            size_t baseline_written;
            uint64_t searched_sse;
            uint64_t baseline_sse;

            for (row = 0U; row < 32U; ++row) {
                  for (column = 0U; column < 32U; ++column) {
                        source_y[(size_t)row * 32U + column] =
                              (uint8_t)((index == 0U ? column : row) * 8U);
                  }
            }
            avifdec_memory_fill(source_u, 128U, sizeof(source_u));
            avifdec_memory_fill(source_v, 128U, sizeof(source_v));
            avifenc_options_default(&options);
            options.quantizer = 96U;
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          output, sizeof(output), &searched_written, &error) ==
                    AVIFENC_OK);
            CHECK(avifdec_decode(
                          output, searched_written, 0, decode_workspace,
                          sizeof(decode_workspace), &decoded, 0, &decode_error) ==
                    AVIFDEC_OK);
            searched_sse = quality_plane_sse(
                  source_y, 32U, decoded_y, 32U, 32U, 32U);

            options.speed = AVIFENC_MAX_SPEED;
            CHECK(avifenc_encode(
                          &image, &options, workspace, sizeof(workspace),
                          baseline, sizeof(baseline), &baseline_written, &error) ==
                    AVIFENC_OK);
            CHECK(avifdec_decode(
                          baseline, baseline_written, 0, decode_workspace,
                          sizeof(decode_workspace), &decoded, 0, &decode_error) ==
                    AVIFDEC_OK);
            baseline_sse = quality_plane_sse(
                  source_y, 32U, decoded_y, 32U, 32U, 32U);
                CHECK(searched_sse * 2U < baseline_sse);
            CHECK(searched_written <=
                  baseline_written + baseline_written / 4U);
      }
      {
            AvifencStatistics effort[3];

            avifenc_options_default(&options);
            options.quantizer = 96U;
            for (index = 0U; index < 3U; ++index) {
                  size_t output_written;

                  options.speed = (uint8_t)index;
                  CHECK(avifenc_encode_ex(
                              &image, &options, workspace, sizeof(workspace),
                              output, sizeof(output), &output_written,
                              &effort[index], &error) == AVIFENC_OK);
            }
            CHECK(effort[0].prediction_trial_count >=
                        effort[1].prediction_trial_count &&
                  effort[1].prediction_trial_count >=
                        effort[2].prediction_trial_count &&
                  effort[0].prediction_trial_count >
                        effort[2].prediction_trial_count);
            CHECK(effort[0].transform_trial_count >=
                        effort[1].transform_trial_count &&
                  effort[1].transform_trial_count >=
                        effort[2].transform_trial_count &&
                  effort[0].transform_trial_count >
                        effort[2].transform_trial_count);
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
      result = test_av1_symbol_writer_vectors();
      if (result != 0) return result;
      result = test_av1_symbol_writer_round_trip();
      if (result != 0) return result;
      result = test_av1_symbol_writer_carry_and_tail();
      if (result != 0) return result;
      result = test_av1_symbol_writer_boundaries();
      if (result != 0) return result;
      result = test_av1_header_writer();
      if (result != 0) return result;
      result = test_av1_forward_transform();
      if (result != 0) return result;
      result = test_av1_transform_trial();
      if (result != 0) return result;
      result = test_av1_tile_writer();
      if (result != 0) return result;
      result = test_av1_avif_integration();
      if (result != 0) return result;
      result = test_avif_writer();
      if (result != 0) return result;
      result = test_public_contract();
    if (result != 0) return result;
    result = test_query_validation();
    if (result != 0) return result;
      result = test_encode_boundaries();
      if (result != 0) return result;
      result = test_quality_controls();
      if (result != 0) return result;
      return 0;
}