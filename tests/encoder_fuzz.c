#include "encoder/avifenc.h"
#include "avifdec.h"
#include "base.h"

#define ENCODER_FUZZ_MAX_WIDTH 4160U
#define ENCODER_FUZZ_MAX_HEIGHT 32U
#define ENCODER_FUZZ_MAX_PIXELS \
    ((size_t)ENCODER_FUZZ_MAX_WIDTH * ENCODER_FUZZ_MAX_HEIGHT)
#define ENCODER_FUZZ_WORKSPACE_SIZE (2U * 1024U * 1024U)
#define ENCODER_FUZZ_OUTPUT_SIZE (512U * 1024U)
#define ENCODER_FUZZ_DECODE_WORKSPACE_SIZE (8U * 1024U * 1024U)
#define ENCODER_FUZZ_ALIGNMENT 16U

static uint8_t fuzz_y[ENCODER_FUZZ_MAX_PIXELS];
static uint8_t fuzz_u[ENCODER_FUZZ_MAX_PIXELS / 4U];
static uint8_t fuzz_v[ENCODER_FUZZ_MAX_PIXELS / 4U];
static unsigned char fuzz_workspace[
    ENCODER_FUZZ_WORKSPACE_SIZE + ENCODER_FUZZ_ALIGNMENT];
static unsigned char fuzz_workspace_repeat[
    ENCODER_FUZZ_WORKSPACE_SIZE + ENCODER_FUZZ_ALIGNMENT];
static unsigned char fuzz_output[
    ENCODER_FUZZ_OUTPUT_SIZE + ENCODER_FUZZ_ALIGNMENT];
static unsigned char fuzz_output_repeat[
    ENCODER_FUZZ_OUTPUT_SIZE + ENCODER_FUZZ_ALIGNMENT];
static unsigned char fuzz_decode_workspace[
    ENCODER_FUZZ_DECODE_WORKSPACE_SIZE + ENCODER_FUZZ_ALIGNMENT];
static uint16_t fuzz_decoded_y[ENCODER_FUZZ_MAX_PIXELS];
static uint16_t fuzz_decoded_u[ENCODER_FUZZ_MAX_PIXELS / 4U];
static uint16_t fuzz_decoded_v[ENCODER_FUZZ_MAX_PIXELS / 4U];

static void fuzz_require(int condition) {
    if (!condition) __builtin_trap();
}

static uint8_t fuzz_byte(const unsigned char *data,
                         size_t size,
                         size_t index) {
    if (size == 0U) {
        return (uint8_t)(index * 37U + 11U);
    }
    return data[index % size];
}

static void fuzz_fill_plane(uint8_t *plane,
                            size_t size,
                            const unsigned char *data,
                            size_t data_size,
                            size_t offset) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        plane[index] = fuzz_byte(data, data_size, offset + index);
    }
}

static void fuzz_check_guard(const unsigned char *buffer,
                             size_t begin,
                             size_t end,
                             size_t total) {
    size_t index;

    for (index = 0U; index < begin; ++index) {
        fuzz_require(buffer[index] == 0xa5U);
    }
    for (index = end; index < total; ++index) {
        fuzz_require(buffer[index] == 0xa5U);
    }
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    AvifencImage image = { 0 };
    AvifencOptions options;
    AvifencRequirements requirements;
    AvifencRequirements repeated_requirements;
    AvifencError error;
    AvifdecImageInfo decoded_info;
    AvifdecImage decoded = {
        { fuzz_decoded_y, fuzz_decoded_u, fuzz_decoded_v },
                { ENCODER_FUZZ_MAX_WIDTH,
                    ENCODER_FUZZ_MAX_WIDTH / 2U,
                    ENCODER_FUZZ_MAX_WIDTH / 2U },
        { 0U, 0U, 0U }, { 0U, 0U, 0U }, 0U, 0U, 0U, 0U,
        0, 0U, 0U, 0U, 0U, 0U, 0U
    };
    AvifdecError decode_error;
    int wide = fuzz_byte(data, size, 0U) == 255U;
    uint32_t width = wide
        ? 4098U : 2U + 2U * (fuzz_byte(data, size, 0U) % 16U);
    uint32_t height = wide
        ? 2U : 2U + 2U * (fuzz_byte(data, size, 1U) % 16U);
    size_t luma_size = (size_t)width * height;
    size_t chroma_size = (size_t)(width / 2U) * (height / 2U);
    size_t workspace_offset =
        fuzz_byte(data, size, 4U) & (ENCODER_FUZZ_ALIGNMENT - 1U);
    size_t repeat_offset =
        fuzz_byte(data, size, 5U) & (ENCODER_FUZZ_ALIGNMENT - 1U);
    size_t decode_offset =
        fuzz_byte(data, size, 6U) & (ENCODER_FUZZ_ALIGNMENT - 1U);
    size_t output_written = 0U;
    size_t repeated_written = 0U;
    AvifencStatus status;

    fuzz_fill_plane(fuzz_y, luma_size, data, size, 7U);
    fuzz_fill_plane(fuzz_u, chroma_size, data, size, 7U + luma_size);
    fuzz_fill_plane(
        fuzz_v, chroma_size, data, size, 7U + luma_size + chroma_size);
    image.planes[0] = fuzz_y;
    image.planes[1] = fuzz_u;
    image.planes[2] = fuzz_v;
    image.strides[0] = width;
    image.strides[1] = width / 2U;
    image.strides[2] = width / 2U;
    image.width = width;
    image.height = height;
    image.color.color_primaries = 1U;
    image.color.transfer_characteristics = 1U;
    image.color.matrix_coefficients = 1U;
    avifenc_options_default(&options);
    options.quantizer = fuzz_byte(data, size, 2U);
    options.speed = (uint8_t)(fuzz_byte(data, size, 3U) %
                              (AVIFENC_MAX_SPEED + 1U));
    if (options.quantizer != 0U) {
        options.quantization.delta_q_y_dc =
            (int8_t)((int)(fuzz_byte(data, size, 8U) % 128U) - 64);
        options.quantization.delta_q_u_dc =
            (int8_t)((int)(fuzz_byte(data, size, 9U) % 128U) - 64);
        options.quantization.delta_q_u_ac =
            (int8_t)((int)(fuzz_byte(data, size, 10U) % 128U) - 64);
        options.quantization.delta_q_v_dc =
            (int8_t)((int)(fuzz_byte(data, size, 11U) % 128U) - 64);
        options.quantization.delta_q_v_ac =
            (int8_t)((int)(fuzz_byte(data, size, 12U) % 128U) - 64);
        options.quantization.matrix_mode =
            (uint8_t)(fuzz_byte(data, size, 13U) % 3U);
        options.quantization.matrix_levels[0] =
            (uint8_t)(fuzz_byte(data, size, 14U) % 15U);
        options.quantization.matrix_levels[1] =
            (uint8_t)(fuzz_byte(data, size, 15U) % 15U);
        options.quantization.matrix_levels[2] =
            (uint8_t)(fuzz_byte(data, size, 16U) % 15U);
        options.quantization.adaptive_quantization =
            (uint8_t)(fuzz_byte(data, size, 17U) & 1U);
        options.quantization.aq_strength =
            (uint8_t)(fuzz_byte(data, size, 18U) % 64U);
        options.rate_control.mode =
            (uint8_t)(fuzz_byte(data, size, 19U) % 2U);
        options.rate_control.target_quality =
            (uint16_t)(7000U + fuzz_byte(data, size, 20U) * 7U);
    }

    fuzz_require(avifenc_query(
        &image, &options, &requirements, &error) == AVIFENC_OK);
    fuzz_require(avifenc_query(
        &image, &options, &repeated_requirements, &error) == AVIFENC_OK);
    fuzz_require(requirements.workspace_required ==
                     repeated_requirements.workspace_required &&
                 requirements.output_capacity_required ==
                     repeated_requirements.output_capacity_required);
    fuzz_require(requirements.workspace_required <=
                     ENCODER_FUZZ_WORKSPACE_SIZE &&
                 requirements.output_capacity_required <=
                     ENCODER_FUZZ_OUTPUT_SIZE);

    avifdec_memory_fill(fuzz_workspace, 0xa5U, sizeof(fuzz_workspace));
    avifdec_memory_fill(fuzz_output, 0xa5U, sizeof(fuzz_output));
    if (requirements.workspace_required != 0U) {
        status = avifenc_encode(
            &image, &options, fuzz_workspace + workspace_offset,
            requirements.workspace_required - 1U,
            fuzz_output + workspace_offset,
            requirements.output_capacity_required,
            &output_written, &error);
        fuzz_require(status == AVIFENC_OUT_OF_MEMORY && output_written == 0U);
    }
    status = avifenc_encode(
        &image, &options, fuzz_workspace + workspace_offset,
        requirements.workspace_required,
        fuzz_output + workspace_offset,
        requirements.output_capacity_required - 1U,
        &output_written, &error);
    fuzz_require(status == AVIFENC_OUTPUT_TOO_SMALL && output_written == 0U);

    avifdec_memory_fill(fuzz_workspace, 0xa5U, sizeof(fuzz_workspace));
    avifdec_memory_fill(
        fuzz_workspace_repeat, 0xa5U, sizeof(fuzz_workspace_repeat));
    avifdec_memory_fill(fuzz_output, 0xa5U, sizeof(fuzz_output));
    avifdec_memory_fill(
        fuzz_output_repeat, 0xa5U, sizeof(fuzz_output_repeat));
    status = avifenc_encode(
        &image, &options, fuzz_workspace + workspace_offset,
        requirements.workspace_required,
        fuzz_output + workspace_offset,
        requirements.output_capacity_required,
        &output_written, &error);
    {
        AvifencStatus repeated_status = avifenc_encode(
        &image, &options, fuzz_workspace_repeat + repeat_offset,
        requirements.workspace_required,
        fuzz_output_repeat + repeat_offset,
        requirements.output_capacity_required,
        &repeated_written, &error);

        fuzz_require(status == repeated_status);
    }
    if (status == AVIFENC_LIMIT_EXCEEDED) {
        fuzz_require(options.rate_control.mode != 0U &&
                     output_written == 0U && repeated_written == 0U);
        return 0;
    }
    fuzz_require(status == AVIFENC_OK);
    fuzz_require(output_written == repeated_written && output_written != 0U &&
                 avifdec_memory_compare(
                     fuzz_output + workspace_offset,
                     fuzz_output_repeat + repeat_offset,
                     output_written) == 0);
    fuzz_check_guard(
        fuzz_output, workspace_offset, workspace_offset + output_written,
        sizeof(fuzz_output));
    fuzz_check_guard(
        fuzz_output_repeat, repeat_offset, repeat_offset + repeated_written,
        sizeof(fuzz_output_repeat));

    fuzz_require(avifdec_query(
        fuzz_output + workspace_offset, output_written,
        0, 0, 0U, &decoded_info, &decode_error) == AVIFDEC_OK);
    fuzz_require(decoded_info.width == width && decoded_info.height == height &&
                 (options.rate_control.mode != 0U ||
                  decoded_info.base_q_index == options.quantizer) &&
                 decoded_info.workspace_required <=
                     ENCODER_FUZZ_DECODE_WORKSPACE_SIZE);
    fuzz_require(avifdec_decode(
        fuzz_output + workspace_offset, output_written, 0,
        fuzz_decode_workspace + decode_offset,
        decoded_info.workspace_required,
        &decoded, 0, &decode_error) == AVIFDEC_OK);
    fuzz_require(decoded.widths[0] == width && decoded.heights[0] == height &&
                 decoded.widths[1] == width / 2U &&
                 decoded.heights[1] == height / 2U &&
                 decoded.widths[2] == width / 2U &&
                 decoded.heights[2] == height / 2U);
    if (options.quantizer == 0U) {
        uint32_t row;
        uint32_t column;

        fuzz_require(decoded_info.coded_lossless == 1U);
        for (row = 0U; row < height; ++row) {
            for (column = 0U; column < width; ++column) {
                fuzz_require(fuzz_decoded_y[
                    (size_t)row * ENCODER_FUZZ_MAX_WIDTH + column] ==
                    fuzz_y[(size_t)row * width + column]);
            }
        }
        for (row = 0U; row < height / 2U; ++row) {
            for (column = 0U; column < width / 2U; ++column) {
                size_t decoded_index =
                    (size_t)row * (ENCODER_FUZZ_MAX_WIDTH / 2U) + column;
                size_t source_index = (size_t)row * (width / 2U) + column;

                fuzz_require(fuzz_decoded_u[decoded_index] ==
                                 fuzz_u[source_index] &&
                             fuzz_decoded_v[decoded_index] ==
                                 fuzz_v[source_index]);
            }
        }
    }
    return 0;
}