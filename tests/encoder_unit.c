#include "encoder/avifenc.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static int text_equal(const char *left, const char *right) {
    size_t index = 0U;

    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return 0;
        ++index;
    }
    return left[index] == right[index];
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
    result = test_public_contract();
    if (result != 0) return result;
    result = test_query_validation();
    if (result != 0) return result;
    return test_encode_boundaries();
}