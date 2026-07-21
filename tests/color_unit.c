#include "avif_color.h"
#include "avif_gain_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        return __LINE__; \
    } \
} while (0)

#define CHECK_STATUS(expression, expected) do { \
    AvifdecStatus check_status_value; \
    check_status_value = (expression); \
    if (check_status_value != (expected)) { \
        fprintf(stderr, "%s:%d: status %d, expected %d\n", \
                __FILE__, __LINE__, (int)check_status_value, \
                (int)(expected)); \
        return __LINE__; \
    } \
} while (0)

static int close_double(double actual,
                        double expected,
                        double tolerance) {
    return fabs(actual - expected) <= tolerance;
}

static uint32_t max_sample(uint8_t bit_depth) {
    return ((uint32_t)1U << bit_depth) - 1U;
}

static uint16_t quantize(double value, uint8_t bit_depth) {
    double scaled;

    scaled = value * (double)max_sample(bit_depth);
    if (scaled <= 0.0) {
        return 0U;
    }
    if (scaled >= (double)max_sample(bit_depth)) {
        return (uint16_t)max_sample(bit_depth);
    }
    return (uint16_t)(scaled + 0.5);
}

static uint16_t quantize_chroma(double value, uint8_t bit_depth) {
    double scaled;

    scaled = value * (double)max_sample(bit_depth) +
             (double)((uint32_t)1U << (bit_depth - 1U));
    if (scaled <= 0.0) {
        return 0U;
    }
    if (scaled >= (double)max_sample(bit_depth)) {
        return (uint16_t)max_sample(bit_depth);
    }
    return (uint16_t)(scaled + 0.5);
}

static double normalize_luma(uint16_t sample,
                             uint8_t bit_depth,
                             uint8_t full_range) {
    uint32_t shift;

    if (full_range != 0U) {
        return (double)sample / (double)max_sample(bit_depth);
    }
    shift = bit_depth - 8U;
    return ((double)sample - (double)(16U << shift)) /
           (double)(219U << shift);
}

static double normalize_chroma(uint16_t sample,
                               uint8_t bit_depth,
                               uint8_t full_range) {
    uint32_t range;
    uint32_t center;

    center = (uint32_t)1U << (bit_depth - 1U);
    range = full_range != 0U
        ? max_sample(bit_depth)
        : (uint32_t)224U << (bit_depth - 8U);
    return ((double)sample - (double)center) / (double)range;
}

static double reference_transfer_encode(uint16_t identifier,
                                        double linear) {
    const double alpha = 1.099296826809442;
    const double beta = 0.018053968510807;
    const double smpte240_alpha = 1.111572195921731;
    const double smpte240_beta = 0.022821585529445;
    const double bt1361_gamma = beta / 4.0;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    const double m = 78.84375;
    const double n = 0.1593017578125;
    double p;

    if (identifier == 1U || identifier == 6U ||
        identifier == 14U || identifier == 15U) {
        return linear < beta
            ? 4.5 * linear
            : alpha * pow(linear, 0.45) - (alpha - 1.0);
    }
    if (identifier == 4U) {
        return pow(linear, 1.0 / 2.2);
    }
    if (identifier == 5U) {
        return pow(linear, 1.0 / 2.8);
    }
    if (identifier == 7U) {
        return linear < smpte240_beta
            ? 4.0 * linear
            : smpte240_alpha * pow(linear, 0.45) -
                (smpte240_alpha - 1.0);
    }
    if (identifier == 8U) {
        return linear;
    }
    if (identifier == 9U) {
        return linear < 0.01 ? 0.0 : 1.0 + log10(linear) / 2.0;
    }
    if (identifier == 10U) {
        return linear < sqrt(10.0) / 1000.0
            ? 0.0
            : 1.0 + log10(linear) / 2.5;
    }
    if (identifier == 11U) {
        if (linear <= -beta) {
            return -alpha * pow(-linear, 0.45) + alpha - 1.0;
        }
        return linear < beta
            ? 4.5 * linear
            : alpha * pow(linear, 0.45) - alpha + 1.0;
    }
    if (identifier == 12U) {
        if (linear <= -bt1361_gamma) {
            return -(alpha * pow(-4.0 * linear, 0.45) -
                     (alpha - 1.0)) / 4.0;
        }
        return linear < beta
            ? 4.5 * linear
            : alpha * pow(linear, 0.45) - alpha + 1.0;
    }
    if (identifier == 13U) {
        return linear < 0.0031308
            ? 12.92 * linear
            : 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
    }
    if (identifier == 16U) {
        p = pow(linear, n);
        return pow((c1 + c2 * p) / (1.0 + c3 * p), m);
    }
    if (identifier == 17U) {
        return pow(48.0 * linear / 52.37, 1.0 / 2.6);
    }
    if (identifier == 18U) {
        return linear <= 1.0 / 12.0
            ? sqrt(3.0 * linear)
            : 0.17883277 * log(12.0 * linear - 0.28466892) +
                0.55991073;
    }
    return -1.0;
}

static double reference_transfer_decode(uint16_t identifier,
                                        double encoded) {
    const double alpha = 1.099296826809442;
    const double beta = 0.018053968510807;
    const double smpte240_alpha = 1.111572195921731;
    const double smpte240_beta = 0.022821585529445;
    const double bt1361_gamma = beta / 4.0;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    const double m = 78.84375;
    const double n = 0.1593017578125;
    double p;

    if (identifier == 1U || identifier == 6U ||
        identifier == 14U || identifier == 15U) {
        return encoded < 4.5 * beta
            ? encoded / 4.5
            : pow((encoded + alpha - 1.0) / alpha, 1.0 / 0.45);
    }
    if (identifier == 4U) {
        return pow(encoded, 2.2);
    }
    if (identifier == 5U) {
        return pow(encoded, 2.8);
    }
    if (identifier == 7U) {
        return encoded < 4.0 * smpte240_beta
            ? encoded / 4.0
            : pow((encoded + smpte240_alpha - 1.0) /
                  smpte240_alpha, 1.0 / 0.45);
    }
    if (identifier == 8U) {
        return encoded;
    }
    if (identifier == 9U) {
        return pow(10.0, 2.0 * (encoded - 1.0));
    }
    if (identifier == 10U) {
        return pow(10.0, 2.5 * (encoded - 1.0));
    }
    if (identifier == 11U) {
        if (encoded <= -4.5 * beta) {
            return -pow((-encoded + alpha - 1.0) / alpha,
                        1.0 / 0.45);
        }
        return encoded < 4.5 * beta
            ? encoded / 4.5
            : pow((encoded + alpha - 1.0) / alpha, 1.0 / 0.45);
    }
    if (identifier == 12U) {
        if (encoded <= -4.5 * bt1361_gamma) {
            return -pow((-4.0 * encoded + (alpha - 1.0)) / alpha,
                        1.0 / 0.45) / 4.0;
        }
        return encoded < 4.5 * beta
            ? encoded / 4.5
            : pow((encoded + alpha - 1.0) / alpha, 1.0 / 0.45);
    }
    if (identifier == 13U) {
        return encoded < 0.04045
            ? encoded / 12.92
            : pow((encoded + 0.055) / 1.055, 2.4);
    }
    if (identifier == 16U) {
        p = pow(encoded, 1.0 / m);
        return pow(fmax(p - c1, 0.0) / (c2 - c3 * p), 1.0 / n);
    }
    if (identifier == 17U) {
        return (52.37 / 48.0) * pow(encoded, 2.6);
    }
    if (identifier == 18U) {
        return encoded <= 0.5
            ? encoded * encoded / 3.0
            : (exp((encoded - 0.55991073) / 0.17883277) +
               0.28466892) / 12.0;
    }
    return -1.0;
}

static void setup_info(AvifdecImageInfo *info,
                       uint32_t width,
                       uint32_t height,
                       uint8_t bit_depth,
                       uint16_t primaries,
                       uint16_t transfer,
                       uint16_t matrix,
                       uint8_t full_range,
                       uint8_t subsampling_x,
                       uint8_t subsampling_y,
                       uint8_t chroma_position) {
    memset(info, 0, sizeof(*info));
    info->width = width;
    info->height = height;
    info->presentation_width = width;
    info->presentation_height = height;
    info->crop.width = width;
    info->crop.height = height;
    info->bit_depth = bit_depth;
    info->color_primaries = primaries;
    info->transfer_characteristics = transfer;
    info->matrix_coefficients = matrix;
    info->color_range = full_range;
    info->has_nclx = 1U;
    info->subsampling_x = subsampling_x;
    info->subsampling_y = subsampling_y;
    info->chroma_sample_position = chroma_position;
}

static void setup_image(AvifdecImage *image,
                        uint16_t *y,
                        uint16_t *u,
                        uint16_t *v,
                        uint32_t width,
                        uint32_t height,
                        uint8_t bit_depth,
                        uint8_t subsampling_x,
                        uint8_t subsampling_y) {
    memset(image, 0, sizeof(*image));
    image->planes[0] = y;
    image->planes[1] = u;
    image->planes[2] = v;
    image->strides[0] = width;
    image->strides[1] =
        ((width - 1U) >> subsampling_x) + 1U;
    image->strides[2] = image->strides[1];
    image->widths[0] = width;
    image->heights[0] = height;
    image->widths[1] =
        ((width - 1U) >> subsampling_x) + 1U;
    image->widths[2] = image->widths[1];
    image->heights[1] =
        ((height - 1U) >> subsampling_y) + 1U;
    image->heights[2] = image->heights[1];
    image->bit_depth = bit_depth;
    image->subsampling_x = subsampling_x;
    image->subsampling_y = subsampling_y;
}

static void description_from_info(const AvifdecImageInfo *info,
                                  AvifdecColorDescription *description) {
    memset(description, 0, sizeof(*description));
    description->color_primaries = info->color_primaries;
    description->transfer_characteristics =
        info->transfer_characteristics;
    description->matrix_coefficients = info->matrix_coefficients;
    description->color_range = info->color_range;
    description->has_nclx = info->has_nclx;
    description->icc.data = info->icc_data;
    description->icc.size = info->icc_size;
}

static AvifdecStatus init_transform(
    const AvifdecImageInfo *info,
    uint16_t destination_primaries,
    uint16_t destination_transfer,
    uint8_t upsampling,
    uint8_t hdr_policy,
    AvifdecColorTransform *transform) {
    AvifdecColorDescription description;
    AvifdecColorOptions options;
    AvifdecError error;

    description_from_info(info, &description);
    avifdec_color_options_default(&options);
    options.destination_color_primaries = destination_primaries;
    options.destination_transfer_characteristics =
        destination_transfer;
    options.chroma_upsampling =
        (AvifdecChromaUpsampling)upsampling;
    options.hdr_policy = (AvifdecColorHdrPolicy)hdr_policy;
    if (hdr_policy != AVIFDEC_COLOR_HDR_REJECT) {
        options.reference_white_nits = 203.0f;
        options.display_peak_nits = 1000.0f;
    }
    return avifdec_color_transform_init(
        &description, &options, 0, 0, 0U, transform, &error);
}

static AvifdecStatus convert_f32(const AvifdecImage *image,
                                 const AvifdecImageInfo *info,
                                 const AvifdecColorTransform *transform,
                                 float output[4],
                                 uint8_t rgba,
                                 uint8_t alpha_mode) {
    AvifdecRgbImage rgb;
    AvifdecError error;

    memset(&rgb, 0, sizeof(rgb));
    rgb.pixels = output;
    rgb.stride = (rgba != 0U ? 4U : 3U) * sizeof(float);
    rgb.width = 1U;
    rgb.height = 1U;
    rgb.format = rgba != 0U ? AVIFDEC_RGBAF32 : AVIFDEC_RGBF32;
    rgb.alpha_mode = alpha_mode;
    return avifdec_image_to_rgb_with_transform(
        image, info, transform, &rgb, &error);
}

static int test_transfer_functions(void) {
    static const uint16_t identifiers[] = {
        1U, 4U, 5U, 6U, 7U, 8U, 9U, 10U,
        11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U
    };
    static const double linear_samples[] = {
        0.0, 0.0001, 0.003, 0.01, 0.018,
        0.05, 0.18, 0.5, 1.0
    };
    static const double encoded_samples[] = {
        0.0, 0.01, 0.04, 0.1, 0.5, 0.75, 1.0
    };
    size_t index;

    for (index = 0U;
         index < sizeof(identifiers) / sizeof(identifiers[0]);
         ++index) {
        uint16_t identifier;
        double linear;
        double actual_encoded;
        double expected_encoded;
        double actual_linear;
        double expected_linear;
        double tolerance;
        size_t sample_index;

        identifier = identifiers[index];
        linear = identifier == 9U
            ? 0.1
            : (identifier == 10U ? 0.05 : 0.18);
        expected_encoded =
            reference_transfer_encode(identifier, linear);
        CHECK_STATUS(avif_color_transfer_from_linear(
            identifier, linear, &actual_encoded), AVIFDEC_OK);
        tolerance = identifier == 16U ? 3.0e-10 : 2.0e-11;
        CHECK(close_double(actual_encoded, expected_encoded, tolerance));
        expected_linear =
            reference_transfer_decode(identifier, expected_encoded);
        CHECK_STATUS(avif_color_transfer_to_linear(
            identifier, expected_encoded, &actual_linear), AVIFDEC_OK);
        CHECK(close_double(actual_linear, expected_linear, 4.0e-10));
        CHECK(close_double(actual_linear, linear, 4.0e-10));
        for (sample_index = 0U;
             sample_index <
                sizeof(linear_samples) / sizeof(linear_samples[0]);
             ++sample_index) {
            expected_encoded = reference_transfer_encode(
                identifier, linear_samples[sample_index]);
            CHECK_STATUS(avif_color_transfer_from_linear(
                identifier, linear_samples[sample_index],
                &actual_encoded), AVIFDEC_OK);
            CHECK(close_double(
                actual_encoded, expected_encoded, 3.0e-9));
        }
        for (sample_index = 0U;
             sample_index <
                sizeof(encoded_samples) / sizeof(encoded_samples[0]);
             ++sample_index) {
            expected_linear = reference_transfer_decode(
                identifier, encoded_samples[sample_index]);
            CHECK_STATUS(avif_color_transfer_to_linear(
                identifier, encoded_samples[sample_index],
                &actual_linear), AVIFDEC_OK);
            CHECK(close_double(
                actual_linear, expected_linear, 3.0e-9));
        }
    }
    {
        double encoded;
        double linear;

        CHECK_STATUS(avif_color_transfer_from_linear(
            11U, -0.2, &encoded), AVIFDEC_OK);
        CHECK(close_double(
            encoded, reference_transfer_encode(11U, -0.2), 2.0e-11));
        CHECK_STATUS(avif_color_transfer_to_linear(
            11U, encoded, &linear), AVIFDEC_OK);
        CHECK(close_double(linear, -0.2, 4.0e-10));
        CHECK_STATUS(avif_color_transfer_from_linear(
            12U, -0.1, &encoded), AVIFDEC_OK);
        CHECK(close_double(
            encoded, reference_transfer_encode(12U, -0.1), 2.0e-11));
        CHECK_STATUS(avif_color_transfer_to_linear(
            12U, encoded, &linear), AVIFDEC_OK);
        CHECK(close_double(linear, -0.1, 4.0e-10));
    }
    {
        double value;

        CHECK_STATUS(avif_color_transfer_to_linear(
            2U, 0.5, &value), AVIFDEC_UNSUPPORTED);
        CHECK_STATUS(avif_color_transfer_to_linear(
            13U, 0.5, 0), AVIFDEC_INVALID_ARGUMENT);
    }
    {
        const double beta = 0.018053968510807;
        const double smpte240_beta = 0.022821585529445;
        double encoded;
        double linear;

        CHECK_STATUS(avif_color_transfer_from_linear(
            7U, smpte240_beta, &encoded), AVIFDEC_OK);
        CHECK(close_double(
            encoded, 4.0 * smpte240_beta, 2.0e-12));
        CHECK_STATUS(avif_color_transfer_from_linear(
            12U, -beta / 4.0, &encoded), AVIFDEC_OK);
        CHECK(close_double(
            encoded, -4.5 * beta / 4.0, 2.0e-12));
        CHECK_STATUS(avif_color_transfer_to_linear(
            12U, encoded, &linear), AVIFDEC_OK);
        CHECK(close_double(linear, -beta / 4.0, 2.0e-12));
        CHECK_STATUS(avif_color_transfer_from_linear(
            16U, 0.0, &encoded), AVIFDEC_OK);
        CHECK(encoded > 0.0 && encoded < 1.0e-6);
    }
    {
        AvifdecImageInfo info;
        AvifdecImage image;
        AvifdecColorTransform transform;
        uint16_t y_plane[1];
        uint16_t u_plane[1];
        uint16_t v_plane[1];
        float output[4];
        double y;
        double cr;
        double encoded_red;
        double expected_red;

        y_plane[0] = quantize(0.02, 12U);
        u_plane[0] = 2048U;
        v_plane[0] = quantize_chroma(-0.10, 12U);
        setup_info(&info, 1U, 1U, 12U,
                   1U, 13U, 5U, 1U, 0U, 0U, 0U);
        setup_image(&image, y_plane, u_plane, v_plane,
                    1U, 1U, 12U, 0U, 0U);
        CHECK_STATUS(init_transform(
            &info, 1U, 13U,
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
            AVIFDEC_COLOR_HDR_REJECT,
            &transform), AVIFDEC_OK);
        CHECK_STATUS(convert_f32(
            &image, &info, &transform, output, 0U,
            AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
        y = normalize_luma(y_plane[0], 12U, 1U);
        cr = normalize_chroma(v_plane[0], 12U, 1U);
        encoded_red = y + 2.0 * (1.0 - 0.299) * cr;
        expected_red = encoded_red <= -0.04045
            ? -pow((-encoded_red + 0.055) / 1.055, 2.4)
            : encoded_red / 12.92;
        CHECK(expected_red < 0.0);
        CHECK(close_double(output[0], expected_red, 3.0e-7));
    }
    return 0;
}

static void multiply3(const double matrix[9],
                      const double input[3],
                      double output[3]) {
    size_t row;

    for (row = 0U; row < 3U; ++row) {
        output[row] =
            matrix[row * 3U] * input[0] +
            matrix[row * 3U + 1U] * input[1] +
            matrix[row * 3U + 2U] * input[2];
    }
}

static int inverse3(const double input[9], double output[9]) {
    double determinant;

    determinant =
        input[0] * (input[4] * input[8] - input[5] * input[7]) -
        input[1] * (input[3] * input[8] - input[5] * input[6]) +
        input[2] * (input[3] * input[7] - input[4] * input[6]);
    if (fabs(determinant) < 1.0e-15) {
        return 0;
    }
    output[0] = (input[4] * input[8] - input[5] * input[7]) /
        determinant;
    output[1] = (input[2] * input[7] - input[1] * input[8]) /
        determinant;
    output[2] = (input[1] * input[5] - input[2] * input[4]) /
        determinant;
    output[3] = (input[5] * input[6] - input[3] * input[8]) /
        determinant;
    output[4] = (input[0] * input[8] - input[2] * input[6]) /
        determinant;
    output[5] = (input[2] * input[3] - input[0] * input[5]) /
        determinant;
    output[6] = (input[3] * input[7] - input[4] * input[6]) /
        determinant;
    output[7] = (input[1] * input[6] - input[0] * input[7]) /
        determinant;
    output[8] = (input[0] * input[4] - input[1] * input[3]) /
        determinant;
    return 1;
}

static int test_primaries(void) {
    static const uint16_t identifiers[] = {
        1U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 22U
    };
    size_t index;

    for (index = 0U;
         index < sizeof(identifiers) / sizeof(identifiers[0]);
         ++index) {
        double matrix[9];
        double white[3] = { 1.0, 1.0, 1.0 };
        double mapped[3];
        size_t element;

        CHECK_STATUS(avif_color_primaries_conversion(
            identifiers[index], identifiers[index],
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
            matrix), AVIFDEC_OK);
        for (element = 0U; element < 9U; ++element) {
            double expected;

            expected = element == 0U ||
                       element == 4U ||
                       element == 8U ? 1.0 : 0.0;
            CHECK(close_double(matrix[element], expected, 2.0e-9));
        }
        CHECK_STATUS(avif_color_primaries_conversion(
            identifiers[index], 1U,
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
            matrix), AVIFDEC_OK);
        multiply3(matrix, white, mapped);
        CHECK(close_double(mapped[0], 1.0, 3.0e-9));
        CHECK(close_double(mapped[1], 1.0, 3.0e-9));
        CHECK(close_double(mapped[2], 1.0, 3.0e-9));
    }
    {
        double matrix[9];
        double absolute[9];
        double white[3] = { 1.0, 1.0, 1.0 };
        double relative_white[3];
        double absolute_white[3];

        CHECK_STATUS(avif_color_primaries_conversion(
            12U, 1U,
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
            matrix), AVIFDEC_OK);
        CHECK(close_double(matrix[0], 1.2249402, 3.0e-5));
        CHECK(close_double(matrix[1], -0.2249402, 3.0e-5));
        CHECK(close_double(matrix[3], -0.0420570, 3.0e-5));
        CHECK(close_double(matrix[4], 1.0420570, 3.0e-5));
        CHECK(close_double(matrix[8], 1.0982736, 3.0e-5));
        CHECK_STATUS(avif_color_primaries_conversion(
            4U, 1U,
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
            matrix), AVIFDEC_OK);
        CHECK_STATUS(avif_color_primaries_conversion(
            4U, 1U,
            AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC,
            absolute), AVIFDEC_OK);
        multiply3(matrix, white, relative_white);
        multiply3(absolute, white, absolute_white);
        CHECK(close_double(relative_white[0], 1.0, 3.0e-9));
        CHECK(close_double(relative_white[1], 1.0, 3.0e-9));
        CHECK(close_double(relative_white[2], 1.0, 3.0e-9));
        CHECK(fabs(absolute_white[0] - 1.0) > 0.01 ||
              fabs(absolute_white[1] - 1.0) > 0.01 ||
              fabs(absolute_white[2] - 1.0) > 0.01);
        CHECK_STATUS(avif_color_primaries_conversion(
            2U, 1U,
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
            matrix), AVIFDEC_UNSUPPORTED);
        CHECK_STATUS(avif_color_primaries_conversion(
            1U, 1U, (AvifdecColorIntent)2,
            matrix), AVIFDEC_INVALID_ARGUMENT);
    }
    return 0;
}

static int matrix_single_pixel(uint16_t matrix,
                               uint8_t bit_depth,
                               uint8_t full_range,
                               uint16_t y,
                               uint16_t u,
                               uint16_t v,
                               float output[4]) {
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorTransform transform;
    uint16_t y_plane[1];
    uint16_t u_plane[1];
    uint16_t v_plane[1];
    uint16_t primaries;
    uint16_t transfer;
    uint8_t hdr_policy;
    AvifdecStatus status;

    primaries = 9U;
    transfer = 8U;
    hdr_policy = AVIFDEC_COLOR_HDR_REJECT;
    if (matrix == 14U || matrix == 15U) {
        transfer = 16U;
        hdr_policy = AVIFDEC_COLOR_HDR_PRESERVE_RELATIVE;
    }
    y_plane[0] = y;
    u_plane[0] = u;
    v_plane[0] = v;
    setup_info(&info, 1U, 1U, bit_depth,
               primaries, transfer, matrix, full_range,
               0U, 0U, 0U);
    setup_image(&image, y_plane, u_plane, v_plane,
                1U, 1U, bit_depth, 0U, 0U);
    status = init_transform(
        &info, primaries, transfer,
        AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
        hdr_policy, &transform);
    if (status != AVIFDEC_OK) {
        return -(int)status;
    }
    status = convert_f32(
        &image, &info, &transform, output, 0U,
        AVIFDEC_ALPHA_STRAIGHT);
    return status == AVIFDEC_OK ? 0 : -(int)status;
}

static int test_matrix_coefficients(void) {
    static const uint16_t ncl_matrices[] = {
        1U, 4U, 5U, 6U, 7U, 9U, 12U
    };
    size_t index;
    uint8_t bit_depth;
    uint16_t y;
    uint16_t u;
    uint16_t v;
    double yn;
    double cb;
    double cr;
    float output[4];

    bit_depth = 12U;
    y = quantize(0.45, bit_depth);
    u = quantize_chroma(0.08, bit_depth);
    v = quantize_chroma(-0.04, bit_depth);
    yn = normalize_luma(y, bit_depth, 1U);
    cb = normalize_chroma(u, bit_depth, 1U);
    cr = normalize_chroma(v, bit_depth, 1U);
    for (index = 0U;
         index < sizeof(ncl_matrices) / sizeof(ncl_matrices[0]);
         ++index) {
        double kr;
        double kb;
        double expected[3];

        if (ncl_matrices[index] == 1U) {
            kr = 0.2126;
            kb = 0.0722;
        } else if (ncl_matrices[index] == 4U) {
            kr = 0.30;
            kb = 0.11;
        } else if (ncl_matrices[index] == 5U ||
                   ncl_matrices[index] == 6U) {
            kr = 0.299;
            kb = 0.114;
        } else if (ncl_matrices[index] == 7U) {
            kr = 0.212;
            kb = 0.087;
        } else {
            kr = 0.2627;
            kb = 0.0593;
        }
        expected[0] = yn + 2.0 * (1.0 - kr) * cr;
        expected[2] = yn + 2.0 * (1.0 - kb) * cb;
        expected[1] =
            (yn - kr * expected[0] - kb * expected[2]) /
            (1.0 - kr - kb);
        CHECK(matrix_single_pixel(
            ncl_matrices[index], bit_depth, 1U,
            y, u, v, output) == 0);
        CHECK(close_double(output[0], expected[0], 3.0e-6));
        CHECK(close_double(output[1], expected[1], 3.0e-6));
        CHECK(close_double(output[2], expected[2], 3.0e-6));
    }
    {
        uint16_t g;
        uint16_t b;
        uint16_t r;

        g = quantize(0.25, bit_depth);
        b = quantize(0.5, bit_depth);
        r = quantize(0.75, bit_depth);
        CHECK(matrix_single_pixel(
            0U, bit_depth, 1U, g, b, r, output) == 0);
        CHECK(close_double(
            output[0], normalize_luma(r, bit_depth, 1U), 2.0e-6));
        CHECK(close_double(
            output[1], normalize_luma(g, bit_depth, 1U), 2.0e-6));
        CHECK(close_double(
            output[2], normalize_luma(b, bit_depth, 1U), 2.0e-6));
    }
    {
        double center;
        double temporary;
        double maximum;
        double expected[3];

        center = (double)((uint32_t)1U << (bit_depth - 1U));
        maximum = max_sample(bit_depth);
        temporary = (double)y - ((double)u - center);
        expected[0] = normalize_luma(
            (uint16_t)fmin(fmax(temporary + ((double)v - center), 0.0),
                           maximum),
            bit_depth, 1U);
        expected[1] = normalize_luma(
            (uint16_t)fmin(fmax((double)y + ((double)u - center), 0.0),
                           maximum),
            bit_depth, 1U);
        expected[2] = normalize_luma(
            (uint16_t)fmin(fmax(temporary - ((double)v - center), 0.0),
                           maximum),
            bit_depth, 1U);
        CHECK(matrix_single_pixel(
            8U, bit_depth, 1U, y, u, v, output) == 0);
        CHECK(close_double(output[0], expected[0], 4.0e-4));
        CHECK(close_double(output[1], expected[1], 4.0e-4));
        CHECK(close_double(output[2], expected[2], 4.0e-4));
    }
    {
        double expected[3];

        expected[0] = 2.0 * cr + 0.991902 * yn;
        expected[1] = yn;
        expected[2] = (2.0 * cb + yn) / 0.986566;
        CHECK(matrix_single_pixel(
            11U, bit_depth, 1U, y, u, v, output) == 0);
        CHECK(close_double(output[0], expected[0], 3.0e-6));
        CHECK(close_double(output[1], expected[1], 3.0e-6));
        CHECK(close_double(output[2], expected[2], 3.0e-6));
    }
    {
        static const uint16_t constant_matrices[] = { 10U, 13U };

        for (index = 0U; index < 2U; ++index) {
            double expected[3];
            double kr;
            double kb;

            kr = 0.2627;
            kb = 0.0593;
            expected[0] = yn + 2.0 * (1.0 - kr) * cr;
            expected[2] = yn + 2.0 * (1.0 - kb) * cb;
            expected[1] =
                (yn - kr * expected[0] - kb * expected[2]) /
                (1.0 - kr - kb);
            CHECK(matrix_single_pixel(
                constant_matrices[index], bit_depth, 1U,
                y, u, v, output) == 0);
            CHECK(close_double(output[0], expected[0], 4.0e-6));
            CHECK(close_double(output[1], expected[1], 4.0e-6));
            CHECK(close_double(output[2], expected[2], 4.0e-6));
        }
    }
    {
        AvifdecImageInfo info;
        AvifdecImage image;
        AvifdecColorTransform transform;
        uint16_t y_plane[1];
        uint16_t u_plane[1];
        uint16_t v_plane[1];
        double y_encoded;
        double cb_encoded;
        double cr_encoded;
        double nb;
        double pb;
        double nr;
        double pr;
        double red_encoded;
        double blue_encoded;
        double expected[3];

        y_plane[0] = quantize(0.45, 12U);
        u_plane[0] = quantize_chroma(0.08, 12U);
        v_plane[0] = quantize_chroma(-0.04, 12U);
        setup_info(&info, 1U, 1U, 12U,
                   9U, 14U, 10U, 1U, 0U, 0U, 0U);
        setup_image(&image, y_plane, u_plane, v_plane,
                    1U, 1U, 12U, 0U, 0U);
        CHECK_STATUS(init_transform(
            &info, 9U, 14U,
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
            AVIFDEC_COLOR_HDR_REJECT,
            &transform), AVIFDEC_OK);
        CHECK_STATUS(convert_f32(
            &image, &info, &transform, output, 0U,
            AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
        y_encoded = normalize_luma(y_plane[0], 12U, 1U);
        cb_encoded = normalize_chroma(u_plane[0], 12U, 1U);
        cr_encoded = normalize_chroma(v_plane[0], 12U, 1U);
        nb = reference_transfer_encode(14U, 1.0 - 0.0593);
        pb = 1.0 - reference_transfer_encode(14U, 0.0593);
        nr = reference_transfer_encode(14U, 1.0 - 0.2627);
        pr = 1.0 - reference_transfer_encode(14U, 0.2627);
        blue_encoded = y_encoded +
            2.0 * (cb_encoded <= 0.0 ? nb : pb) * cb_encoded;
        red_encoded = y_encoded +
            2.0 * (cr_encoded <= 0.0 ? nr : pr) * cr_encoded;
        expected[0] = reference_transfer_decode(14U, red_encoded);
        expected[2] = reference_transfer_decode(14U, blue_encoded);
        expected[1] =
            (reference_transfer_decode(14U, y_encoded) -
             0.2627 * expected[0] - 0.0593 * expected[2]) /
            0.6780;
        CHECK(close_double(output[0], expected[0], 4.0e-6));
        CHECK(close_double(output[1], expected[1], 4.0e-6));
        CHECK(close_double(output[2], expected[2], 4.0e-6));
    }
    {
        uint16_t neutral;
        double encoded;
        double expected;

        neutral = (uint16_t)((uint32_t)1U << (bit_depth - 1U));
        encoded = normalize_luma(y, bit_depth, 1U);
        expected = reference_transfer_decode(16U, encoded) *
                   (10000.0 / 203.0);
        CHECK(matrix_single_pixel(
            14U, bit_depth, 1U, y, neutral, neutral, output) == 0);
        CHECK(close_double(output[0], expected, 3.0e-4));
        CHECK(close_double(output[1], expected, 3.0e-4));
        CHECK(close_double(output[2], expected, 3.0e-4));
        CHECK(matrix_single_pixel(
            15U, bit_depth, 1U, y, neutral, neutral, output) == 0);
        CHECK(close_double(output[0], expected, 4.0e-4));
        CHECK(close_double(output[1], expected, 4.0e-4));
        CHECK(close_double(output[2], expected, 4.0e-4));
    }
    {
        static const double ictcp_lms_to_signal[9] = {
            0.5, 0.5, 0.0,
            6610.0 / 4096.0, -13613.0 / 4096.0,
                7003.0 / 4096.0,
            17933.0 / 4096.0, -17390.0 / 4096.0,
                -543.0 / 4096.0
        };
        static const double ictcp_rgb_to_lms[9] = {
            1688.0 / 4096.0, 2146.0 / 4096.0,
                262.0 / 4096.0,
            683.0 / 4096.0, 2951.0 / 4096.0,
                462.0 / 4096.0,
            99.0 / 4096.0, 309.0 / 4096.0,
                3688.0 / 4096.0
        };
        static const double ipt_lms_to_signal[9] = {
            1638.0 / 4096.0, 1638.0 / 4096.0,
                820.0 / 4096.0,
            18248.0 / 4096.0, -19870.0 / 4096.0,
                1622.0 / 4096.0,
            3300.0 / 4096.0, 1463.0 / 4096.0,
                -4763.0 / 4096.0
        };
        static const double ipt_rgb_to_lms[9] = {
            1747.0 / 4096.0, 2169.0 / 4096.0,
                180.0 / 4096.0,
            673.0 / 4096.0, 3029.0 / 4096.0,
                394.0 / 4096.0,
            50.0 / 4096.0, 207.0 / 4096.0,
                3839.0 / 4096.0
        };
        static const uint16_t matrices[2] = { 14U, 15U };
        size_t special_index;

        y = quantize(0.52, bit_depth);
        u = quantize_chroma(0.025, bit_depth);
        v = quantize_chroma(-0.018, bit_depth);
        for (special_index = 0U;
             special_index < 2U;
             ++special_index) {
            const double *lms_to_signal;
            const double *rgb_to_lms;
            double signal_to_lms[9];
            double lms_to_rgb[9];
            double signal[3];
            double encoded_lms[3];
            double linear_lms[3];
            double expected[3];
            size_t channel;

            lms_to_signal = special_index == 0U
                ? ictcp_lms_to_signal
                : ipt_lms_to_signal;
            rgb_to_lms = special_index == 0U
                ? ictcp_rgb_to_lms
                : ipt_rgb_to_lms;
            CHECK(inverse3(lms_to_signal, signal_to_lms));
            CHECK(inverse3(rgb_to_lms, lms_to_rgb));
            signal[0] = normalize_luma(y, bit_depth, 1U);
            signal[1] = normalize_chroma(u, bit_depth, 1U);
            signal[2] = normalize_chroma(v, bit_depth, 1U);
            multiply3(signal_to_lms, signal, encoded_lms);
            for (channel = 0U; channel < 3U; ++channel) {
                linear_lms[channel] =
                    reference_transfer_decode(
                        16U, encoded_lms[channel]);
            }
            multiply3(lms_to_rgb, linear_lms, expected);
            for (channel = 0U; channel < 3U; ++channel) {
                expected[channel] *= 10000.0 / 203.0;
            }
            CHECK(matrix_single_pixel(
                matrices[special_index], bit_depth, 1U,
                y, u, v, output) == 0);
            CHECK(close_double(output[0], expected[0], 1.5e-3));
            CHECK(close_double(output[1], expected[1], 1.5e-3));
            CHECK(close_double(output[2], expected[2], 1.5e-3));
        }
    }
    {
        AvifdecImageInfo info;
        AvifdecImage image;
        AvifdecColorTransform transform;
        uint16_t y_plane[1];
        uint16_t u_plane[1];
        uint16_t v_plane[1];

        y_plane[0] = quantize(0.75, 12U);
        u_plane[0] = 2048U;
        v_plane[0] = 2048U;
        setup_info(&info, 1U, 1U, 12U,
                   9U, 18U, 14U, 1U, 0U, 0U, 0U);
        setup_image(&image, y_plane, u_plane, v_plane,
                    1U, 1U, 12U, 0U, 0U);
        CHECK_STATUS(init_transform(
            &info, 9U, 18U,
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
            AVIFDEC_COLOR_HDR_PRESERVE_RELATIVE,
            &transform), AVIFDEC_OK);
        CHECK_STATUS(convert_f32(
            &image, &info, &transform, output, 0U,
            AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
        CHECK(close_double(output[0], 1.0, 3.0e-3));
        CHECK(close_double(output[1], 1.0, 3.0e-3));
        CHECK(close_double(output[2], 1.0, 3.0e-3));
    }
    {
        static const uint16_t reversible[] = { 16U, 17U };

        for (index = 0U; index < 2U; ++index) {
            uint8_t rgb_depth;
            uint16_t yy;
            uint16_t cg;
            uint16_t co;
            int32_t center;
            int32_t cg_signed;
            int32_t co_signed;
            int32_t temporary;
            int32_t green;
            int32_t blue;
            int32_t red;

            rgb_depth = (uint8_t)(
                bit_depth - (reversible[index] == 16U ? 2U : 1U));
            center = (int32_t)1 << (bit_depth - 1U);
            yy = 500U;
            cg = (uint16_t)(center + 120);
            co = (uint16_t)(center - 80);
            cg_signed = 120;
            co_signed = -80;
            temporary = (int32_t)yy - cg_signed / 2;
            green = temporary + cg_signed;
            blue = temporary - co_signed / 2;
            red = blue + co_signed;
            CHECK(matrix_single_pixel(
                reversible[index], bit_depth, 1U,
                yy, cg, co, output) == 0);
            CHECK(close_double(
                output[0], (double)red / max_sample(rgb_depth),
                3.0e-6));
            CHECK(close_double(
                output[1], (double)green / max_sample(rgb_depth),
                3.0e-6));
            CHECK(close_double(
                output[2], (double)blue / max_sample(rgb_depth),
                3.0e-6));
        }
        for (index = 0U; index < 2U; ++index) {
            uint8_t rgb_depth;
            uint16_t yy;
            uint16_t cg;
            uint16_t co;
            int32_t temporary;
            int32_t green;
            int32_t blue;
            int32_t red;

            rgb_depth = (uint8_t)(
                8U - (reversible[index] == 16U ? 2U : 1U));
            yy = 30U;
            cg = 138U;
            co = 120U;
            temporary = (int32_t)yy - 5;
            green = temporary + 10;
            blue = temporary + 4;
            red = blue - 8;
            CHECK(matrix_single_pixel(
                reversible[index], 8U, 1U,
                yy, cg, co, output) == 0);
            CHECK(close_double(
                output[0], (double)red / max_sample(rgb_depth),
                3.0e-6));
            CHECK(close_double(
                output[1], (double)green / max_sample(rgb_depth),
                3.0e-6));
            CHECK(close_double(
                output[2], (double)blue / max_sample(rgb_depth),
                3.0e-6));
        }
    }
    return 0;
}

static int test_depths_and_ranges(void) {
    static const uint8_t depths[] = { 8U, 10U, 12U };
    size_t depth_index;
    uint8_t full_range;

    for (depth_index = 0U;
         depth_index < sizeof(depths) / sizeof(depths[0]);
         ++depth_index) {
        for (full_range = 0U; full_range <= 1U; ++full_range) {
            uint8_t bit_depth;
            uint32_t shift;
            uint16_t y;
            uint16_t center;
            float output[4];
            double expected;

            bit_depth = depths[depth_index];
            shift = bit_depth - 8U;
            center = (uint16_t)((uint32_t)1U << (bit_depth - 1U));
            if (full_range != 0U) {
                y = quantize(0.5, bit_depth);
            } else {
                y = (uint16_t)((16U << shift) + (109U << shift));
            }
            expected = normalize_luma(y, bit_depth, full_range);
            CHECK(matrix_single_pixel(
                1U, bit_depth, full_range,
                y, center, center, output) == 0);
            CHECK(close_double(output[0], expected, 3.0e-6));
            CHECK(close_double(output[1], expected, 3.0e-6));
            CHECK(close_double(output[2], expected, 3.0e-6));
        }
    }
    return 0;
}

static AvifdecStatus convert_image_f32(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    float *output,
    size_t stride) {
    AvifdecRgbImage rgb;
    AvifdecError error;

    memset(&rgb, 0, sizeof(rgb));
    rgb.pixels = output;
    rgb.stride = stride;
    rgb.width = info->presentation_width;
    rgb.height = info->presentation_height;
    rgb.format = AVIFDEC_RGBF32;
    rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    return avifdec_image_to_rgb_with_transform(
        image, info, transform, &rgb, &error);
}

static double inferred_cb(const float *pixel, double y) {
    return ((double)pixel[2] - y) / (2.0 * (1.0 - 0.0593));
}

static int test_chroma_siting_and_odd_edges(void) {
    uint16_t y_plane[15];
    uint16_t u420[6];
    uint16_t v420[6];
    uint16_t u422[9];
    uint16_t v422[9];
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorTransform transform;
    float output[45];
    uint16_t center;
    uint16_t y_value;
    double y_normalized;
    uint8_t position;
    size_t index;

    center = 2048U;
    y_value = quantize(0.5, 12U);
    y_normalized = normalize_luma(y_value, 12U, 1U);
    for (index = 0U; index < 15U; ++index) {
        y_plane[index] = y_value;
    }
    u420[0] = quantize_chroma(-0.20, 12U);
    u420[1] = quantize_chroma(0.00, 12U);
    u420[2] = quantize_chroma(0.20, 12U);
    u420[3] = quantize_chroma(0.20, 12U);
    u420[4] = quantize_chroma(0.00, 12U);
    u420[5] = quantize_chroma(-0.20, 12U);
    for (index = 0U; index < 6U; ++index) {
        v420[index] = center;
    }
    for (position = 0U; position <= 2U; ++position) {
        double a;
        double b;
        double c;
        double d;
        double fx;
        double fy;
        double expected;
        const float *pixel;

        setup_info(&info, 5U, 3U, 12U,
                   9U, 8U, 9U, 1U, 1U, 1U, position);
        setup_image(&image, y_plane, u420, v420,
                    5U, 3U, 12U, 1U, 1U);
        CHECK_STATUS(init_transform(
            &info, 9U, 8U,
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
            AVIFDEC_COLOR_HDR_REJECT,
            &transform), AVIFDEC_OK);
        memset(output, 0, sizeof(output));
        CHECK_STATUS(convert_image_f32(
            &image, &info, &transform,
            output, 5U * 3U * sizeof(float)), AVIFDEC_OK);
        a = normalize_chroma(u420[0], 12U, 1U);
        b = normalize_chroma(u420[1], 12U, 1U);
        c = normalize_chroma(u420[3], 12U, 1U);
        d = normalize_chroma(u420[4], 12U, 1U);
        fx = position == 0U ? 0.25 : 0.5;
        fy = position == 2U ? 0.5 : 0.25;
        expected = (a + (b - a) * fx) * (1.0 - fy) +
                   (c + (d - c) * fx) * fy;
        pixel = output + (1U * 5U + 1U) * 3U;
        CHECK(close_double(
            inferred_cb(pixel, y_normalized), expected, 5.0e-6));
        pixel = output + (2U * 5U + 4U) * 3U;
        if (position == 2U) {
            expected = normalize_chroma(u420[5], 12U, 1U);
        } else if (position == 1U) {
            expected =
                normalize_chroma(u420[2], 12U, 1U) * 0.25 +
                normalize_chroma(u420[5], 12U, 1U) * 0.75;
        } else {
            double top;
            double bottom;

            top = normalize_chroma(u420[1], 12U, 1U) * 0.25 +
                  normalize_chroma(u420[2], 12U, 1U) * 0.75;
            bottom = normalize_chroma(u420[4], 12U, 1U) * 0.25 +
                     normalize_chroma(u420[5], 12U, 1U) * 0.75;
            expected = top * 0.25 + bottom * 0.75;
        }
        CHECK(close_double(
            inferred_cb(pixel, y_normalized), expected, 6.0e-6));
    }
    setup_info(&info, 5U, 3U, 12U,
               9U, 8U, 9U, 1U, 1U, 1U, 0U);
    setup_image(&image, y_plane, u420, v420,
                5U, 3U, 12U, 1U, 1U);
    CHECK_STATUS(init_transform(
        &info, 9U, 8U,
        AVIFDEC_CHROMA_UPSAMPLING_NEAREST,
        AVIFDEC_COLOR_HDR_REJECT,
        &transform), AVIFDEC_OK);
    CHECK_STATUS(convert_image_f32(
        &image, &info, &transform,
        output, 5U * 3U * sizeof(float)), AVIFDEC_OK);
    CHECK(close_double(
        inferred_cb(output + (0U * 5U + 0U) * 3U, y_normalized),
        normalize_chroma(u420[0], 12U, 1U), 5.0e-6));
    CHECK(close_double(
        inferred_cb(output + (0U * 5U + 1U) * 3U, y_normalized),
        normalize_chroma(u420[0], 12U, 1U), 5.0e-6));
    CHECK(close_double(
        inferred_cb(output + (2U * 5U + 4U) * 3U, y_normalized),
        normalize_chroma(u420[5], 12U, 1U), 5.0e-6));

    for (index = 0U; index < 9U; ++index) {
        u422[index] = quantize_chroma(
            ((double)(index % 3U) - 1.0) * 0.15, 12U);
        v422[index] = center;
    }
    setup_info(&info, 5U, 3U, 12U,
               9U, 8U, 9U, 1U, 1U, 0U, 0U);
    setup_image(&image, y_plane, u422, v422,
                5U, 3U, 12U, 1U, 0U);
    CHECK_STATUS(init_transform(
        &info, 9U, 8U,
        AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
        AVIFDEC_COLOR_HDR_REJECT,
        &transform), AVIFDEC_OK);
    CHECK_STATUS(convert_image_f32(
        &image, &info, &transform,
        output, 5U * 3U * sizeof(float)), AVIFDEC_OK);
    CHECK(close_double(
        inferred_cb(output + (1U * 5U + 1U) * 3U, y_normalized),
        normalize_chroma(u422[3], 12U, 1U) * 0.5 +
        normalize_chroma(u422[4], 12U, 1U) * 0.5,
        5.0e-6));
    return 0;
}

static int test_hdr_policy(void) {
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorDescription description;
    AvifdecColorOptions options;
    AvifdecColorTransformInfo query;
    AvifdecColorTransform transform;
    AvifdecError error;
    uint16_t y_plane[1];
    uint16_t u_plane[1];
    uint16_t v_plane[1];
    float output[4];

    y_plane[0] = quantize(0.75, 12U);
    u_plane[0] = 2048U;
    v_plane[0] = 2048U;
    setup_info(&info, 1U, 1U, 12U,
               9U, 16U, 9U, 1U, 0U, 0U, 0U);
    setup_image(&image, y_plane, u_plane, v_plane,
                1U, 1U, 12U, 0U, 0U);
    description_from_info(&info, &description);
    avifdec_color_options_default(&options);
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_UNSUPPORTED);
    options.hdr_policy = AVIFDEC_COLOR_HDR_PRESERVE_RELATIVE;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_INVALID_ARGUMENT);
    options.reference_white_nits = 203.0f;
    options.display_peak_nits = 1000.0f;
    CHECK_STATUS(avifdec_color_transform_init(
        &description, &options, 0, 0, 0U, &transform, &error),
        AVIFDEC_OK);
    CHECK_STATUS(convert_f32(
        &image, &info, &transform, output, 0U,
        AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
    CHECK(output[0] > 1.0f);
    CHECK(close_double(
        output[1],
        reference_transfer_decode(
            16U, normalize_luma(y_plane[0], 12U, 1U)) *
            (10000.0 / 203.0),
        3.0e-4));

    options.hdr_policy = AVIFDEC_COLOR_HDR_CLIP_TO_DISPLAY;
    CHECK_STATUS(avifdec_color_transform_init(
        &description, &options, 0, 0, 0U, &transform, &error),
        AVIFDEC_OK);
    y_plane[0] = 4095U;
    CHECK_STATUS(convert_f32(
        &image, &info, &transform, output, 0U,
        AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
    CHECK(output[0] <= 1000.0f / 203.0f + 1.0e-5f);
    CHECK(output[1] <= 1000.0f / 203.0f + 1.0e-5f);
    CHECK(output[2] <= 1000.0f / 203.0f + 1.0e-5f);

    y_plane[0] = quantize(0.75, 12U);
    setup_info(&info, 1U, 1U, 12U,
               9U, 18U, 9U, 1U, 0U, 0U, 0U);
    description_from_info(&info, &description);
    options.hdr_policy = AVIFDEC_COLOR_HDR_PRESERVE_RELATIVE;
    CHECK_STATUS(avifdec_color_transform_init(
        &description, &options, 0, 0, 0U, &transform, &error),
        AVIFDEC_OK);
    CHECK_STATUS(convert_f32(
        &image, &info, &transform, output, 0U,
        AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
    CHECK(close_double(output[0], 1.0, 3.0e-3));
    CHECK(close_double(output[1], 1.0, 3.0e-3));
    CHECK(close_double(output[2], 1.0, 3.0e-3));
    {
        AvifdecRgbImage rgb;
        uint16_t encoded_output[3];
        uint16_t expected;

        y_plane[0] = 4095U;
        u_plane[0] = 4095U;
        v_plane[0] = 4095U;
        setup_info(&info, 1U, 1U, 12U,
                   9U, 8U, 0U, 1U, 0U, 0U, 0U);
        setup_image(&image, y_plane, u_plane, v_plane,
                    1U, 1U, 12U, 0U, 0U);
        memset(&rgb, 0, sizeof(rgb));
        rgb.pixels = encoded_output;
        rgb.stride = 3U * sizeof(uint16_t);
        rgb.width = 1U;
        rgb.height = 1U;
        rgb.format = AVIFDEC_RGB16;
        rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
        CHECK_STATUS(init_transform(
            &info, 9U, 16U,
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
            AVIFDEC_COLOR_HDR_PRESERVE_RELATIVE,
            &transform), AVIFDEC_OK);
        CHECK_STATUS(avifdec_image_to_rgb_with_transform(
            &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
        expected = (uint16_t)llround(
            reference_transfer_encode(16U, 203.0 / 10000.0) *
            65535.0);
        CHECK(abs((int)encoded_output[0] - (int)expected) <= 2);
        CHECK(abs((int)encoded_output[1] - (int)expected) <= 2);
        CHECK(abs((int)encoded_output[2] - (int)expected) <= 2);
        CHECK_STATUS(init_transform(
            &info, 9U, 18U,
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
            AVIFDEC_COLOR_HDR_PRESERVE_RELATIVE,
            &transform), AVIFDEC_OK);
        CHECK_STATUS(avifdec_image_to_rgb_with_transform(
            &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
        expected = (uint16_t)llround(0.75 * 65535.0);
        CHECK(abs((int)encoded_output[0] - (int)expected) <= 3);
        CHECK(abs((int)encoded_output[1] - (int)expected) <= 3);
        CHECK(abs((int)encoded_output[2] - (int)expected) <= 3);
    }
    return 0;
}

static int test_alpha_association(void) {
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorTransform transform;
    uint16_t y_plane[1];
    uint16_t u_plane[1];
    uint16_t v_plane[1];
    uint16_t alpha_plane[1];
    float output[4];
    double alpha;
    double red_straight;
    double green_straight;
    double blue_straight;

    alpha = 0.5;
    red_straight = 0.8;
    green_straight = 0.4;
    blue_straight = 0.2;
    y_plane[0] = quantize(green_straight * alpha, 12U);
    u_plane[0] = quantize(blue_straight * alpha, 12U);
    v_plane[0] = quantize(red_straight * alpha, 12U);
    alpha_plane[0] = quantize(alpha, 12U);
    setup_info(&info, 1U, 1U, 12U,
               1U, 8U, 0U, 1U, 0U, 0U, 0U);
    setup_image(&image, y_plane, u_plane, v_plane,
                1U, 1U, 12U, 0U, 0U);
    info.has_alpha = 1U;
    info.alpha_bit_depth = 12U;
    info.alpha_color_range = 1U;
    info.alpha_premultiplied = 1U;
    image.alpha_plane = alpha_plane;
    image.alpha_stride = 1U;
    image.alpha_width = 1U;
    image.alpha_height = 1U;
    image.alpha_bit_depth = 12U;
    image.alpha_color_range = 1U;
    image.alpha_premultiplied = 1U;
    CHECK_STATUS(init_transform(
        &info, 1U, 8U,
        AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
        AVIFDEC_COLOR_HDR_REJECT,
        &transform), AVIFDEC_OK);
    CHECK_STATUS(convert_f32(
        &image, &info, &transform, output, 1U,
        AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
    CHECK(close_double(output[0], red_straight, 1.5e-3));
    CHECK(close_double(output[1], green_straight, 1.5e-3));
    CHECK(close_double(output[2], blue_straight, 1.5e-3));
    CHECK(close_double(output[3], alpha, 3.0e-4));
    CHECK_STATUS(convert_f32(
        &image, &info, &transform, output, 1U,
        AVIFDEC_ALPHA_PREMULTIPLIED), AVIFDEC_OK);
    CHECK(close_double(output[0], red_straight * alpha, 1.5e-3));
    CHECK(close_double(output[1], green_straight * alpha, 1.5e-3));
    CHECK(close_double(output[2], blue_straight * alpha, 1.5e-3));
    {
        AvifdecRgbImage rgb;
        AvifdecError error;
        unsigned char pixels[4];
        unsigned char expected_red;
        unsigned char expected_green;
        unsigned char expected_blue;

        info.transfer_characteristics = 13U;
        CHECK_STATUS(init_transform(
            &info, 1U, 13U,
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
            AVIFDEC_COLOR_HDR_REJECT,
            &transform), AVIFDEC_OK);
        memset(&rgb, 0, sizeof(rgb));
        rgb.pixels = pixels;
        rgb.stride = sizeof(pixels);
        rgb.width = 1U;
        rgb.height = 1U;
        rgb.format = AVIFDEC_RGBA8;
        rgb.alpha_mode = AVIFDEC_ALPHA_PREMULTIPLIED;
        CHECK_STATUS(avifdec_image_to_rgb_with_transform(
            &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
        expected_red = (unsigned char)llround(
            normalize_luma(v_plane[0], 12U, 1U) * 255.0);
        expected_green = (unsigned char)llround(
            normalize_luma(y_plane[0], 12U, 1U) * 255.0);
        expected_blue = (unsigned char)llround(
            normalize_luma(u_plane[0], 12U, 1U) * 255.0);
        CHECK(abs((int)pixels[0] - (int)expected_red) <= 1);
        CHECK(abs((int)pixels[1] - (int)expected_green) <= 1);
        CHECK(abs((int)pixels[2] - (int)expected_blue) <= 1);
        CHECK(abs((int)pixels[3] - 128) <= 1);
    }
    alpha_plane[0] = 0U;
    CHECK_STATUS(convert_f32(
        &image, &info, &transform, output, 1U,
        AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
    CHECK(output[0] == 0.0f);
    CHECK(output[1] == 0.0f);
    CHECK(output[2] == 0.0f);
    CHECK(output[3] == 0.0f);
    return 0;
}

#define TEST_FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | (uint32_t)(d))

typedef struct {
    size_t size;
    size_t curve_offset;
    size_t description_offset;
    size_t copyright_offset;
    size_t red_xyz_offset;
    size_t green_xyz_offset;
    size_t blue_xyz_offset;
} TestIccBuild;

static void put_u16be(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value >> 8);
    bytes[1] = (unsigned char)value;
}

static void put_u32be(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

static void put_fixed(unsigned char *bytes, double value) {
    int32_t fixed;

    fixed = (int32_t)llround(value * 65536.0);
    put_u32be(bytes, (uint32_t)fixed);
}

static size_t align4(size_t value) {
    return (value + 3U) & ~(size_t)3U;
}

static void put_tag(unsigned char *data,
                    uint32_t index,
                    uint32_t signature,
                    size_t offset,
                    size_t size) {
    unsigned char *entry;

    entry = data + 132U + (size_t)index * 12U;
    put_u32be(entry, signature);
    put_u32be(entry + 4U, (uint32_t)offset);
    put_u32be(entry + 8U, (uint32_t)size);
}

static void put_xyz(unsigned char *data,
                    size_t offset,
                    double x,
                    double y,
                    double z) {
    put_u32be(data + offset, TEST_FOURCC('X', 'Y', 'Z', ' '));
    put_fixed(data + offset + 8U, x);
    put_fixed(data + offset + 12U, y);
    put_fixed(data + offset + 16U, z);
}

static size_t put_mluc(unsigned char *data, size_t offset) {
    put_u32be(data + offset, TEST_FOURCC('m', 'l', 'u', 'c'));
    put_u32be(data + offset + 8U, 1U);
    put_u32be(data + offset + 12U, 12U);
    data[offset + 16U] = 'e';
    data[offset + 17U] = 'n';
    data[offset + 18U] = 'U';
    data[offset + 19U] = 'S';
    put_u32be(data + offset + 20U, 2U);
    put_u32be(data + offset + 24U, 28U);
    data[offset + 28U] = 0U;
    data[offset + 29U] = 'x';
    return 30U;
}

static size_t put_curve(unsigned char *data,
                        size_t offset,
                        uint8_t curve_mode) {
    if (curve_mode == 7U) {
        put_u32be(data + offset, TEST_FOURCC('c', 'u', 'r', 'v'));
        put_u32be(data + offset + 8U, 0U);
        return 12U;
    }
    if (curve_mode == 1U || curve_mode >= 3U) {
        uint16_t function_type;
        size_t parameter_count;
        size_t index;
        double parameters[7] = {
            2.0, 1.0, 0.0, 0.25, 0.25, 0.0, 0.0
        };

        function_type = curve_mode == 1U
            ? 0U
            : (uint16_t)(curve_mode - 2U);
        parameter_count = function_type == 0U
            ? 1U
            : (function_type == 1U
                ? 3U
                : (function_type == 2U
                    ? 4U
                    : (function_type == 3U ? 5U : 7U)));
        put_u32be(data + offset, TEST_FOURCC('p', 'a', 'r', 'a'));
        put_u16be(data + offset + 8U, function_type);
        for (index = 0U; index < parameter_count; ++index) {
            put_fixed(data + offset + 12U + index * 4U,
                      parameters[index]);
        }
        return 12U + parameter_count * 4U;
    }
    put_u32be(data + offset, TEST_FOURCC('c', 'u', 'r', 'v'));
    if (curve_mode == 2U) {
        put_u32be(data + offset + 8U, 3U);
        put_u16be(data + offset + 12U, 0U);
        put_u16be(data + offset + 14U, 16384U);
        put_u16be(data + offset + 16U, 65535U);
        return 18U;
    }
    put_u32be(data + offset + 8U, 1U);
    put_u16be(data + offset + 12U, 256U);
    return 14U;
}

static void put_icc_header(unsigned char *data,
                           size_t profile_size,
                           uint32_t color_space) {
    put_u32be(data, (uint32_t)profile_size);
    data[8] = 4U;
    data[9] = 0x30U;
    put_u16be(data + 24U, 2026U);
    put_u16be(data + 26U, 7U);
    put_u16be(data + 28U, 20U);
    put_u16be(data + 30U, 12U);
    put_u32be(data + 12U, TEST_FOURCC('m', 'n', 't', 'r'));
    put_u32be(data + 16U, color_space);
    put_u32be(data + 20U, TEST_FOURCC('X', 'Y', 'Z', ' '));
    put_u32be(data + 36U, TEST_FOURCC('a', 'c', 's', 'p'));
    put_u32be(data + 64U, 1U);
    put_fixed(data + 68U, 0.9642);
    put_fixed(data + 72U, 1.0);
    put_fixed(data + 76U, 0.8249);
}

static TestIccBuild build_rgb_icc(unsigned char *data,
                                  size_t capacity,
                                  uint8_t curve_mode,
                                  double media_x,
                                  double media_y,
                                  double media_z) {
    TestIccBuild result;
    size_t offset;
    size_t description_offset;
    size_t copyright_offset;
    size_t white_offset;
    size_t red_offset;
    size_t green_offset;
    size_t blue_offset;
    size_t curve_offset;
    size_t curve_size;
    size_t profile_size;

    memset(data, 0, capacity);
    put_u32be(data + 128U, 9U);
    offset = align4(132U + 9U * 12U);
    description_offset = offset;
    offset = align4(offset + put_mluc(data, offset));
    copyright_offset = offset;
    offset = align4(offset + put_mluc(data, offset));
    white_offset = offset;
    put_xyz(data, white_offset, media_x, media_y, media_z);
    offset = align4(offset + 20U);
    red_offset = offset;
    put_xyz(data, red_offset, 0.4360747, 0.2225045, 0.0139322);
    offset = align4(offset + 20U);
    green_offset = offset;
    put_xyz(data, green_offset, 0.3850649, 0.7168786, 0.0971045);
    offset = align4(offset + 20U);
    blue_offset = offset;
    put_xyz(data, blue_offset, 0.1430804, 0.0606169, 0.7141733);
    offset = align4(offset + 20U);
    curve_offset = offset;
    curve_size = put_curve(data, curve_offset, curve_mode);
    profile_size = align4(curve_offset + curve_size);
    put_tag(data, 0U, TEST_FOURCC('w', 't', 'p', 't'),
            white_offset, 20U);
    put_tag(data, 1U, TEST_FOURCC('r', 'X', 'Y', 'Z'),
            red_offset, 20U);
    put_tag(data, 2U, TEST_FOURCC('g', 'X', 'Y', 'Z'),
            green_offset, 20U);
    put_tag(data, 3U, TEST_FOURCC('b', 'X', 'Y', 'Z'),
            blue_offset, 20U);
    put_tag(data, 4U, TEST_FOURCC('r', 'T', 'R', 'C'),
            curve_offset, curve_size);
    put_tag(data, 5U, TEST_FOURCC('g', 'T', 'R', 'C'),
            curve_offset, curve_size);
    put_tag(data, 6U, TEST_FOURCC('b', 'T', 'R', 'C'),
            curve_offset, curve_size);
    put_tag(data, 7U, TEST_FOURCC('d', 'e', 's', 'c'),
            description_offset, 30U);
    put_tag(data, 8U, TEST_FOURCC('c', 'p', 'r', 't'),
            copyright_offset, 30U);
    put_icc_header(data, profile_size, TEST_FOURCC('R', 'G', 'B', ' '));
    result.size = profile_size;
    result.curve_offset = curve_offset;
    result.description_offset = description_offset;
    result.copyright_offset = copyright_offset;
    result.red_xyz_offset = red_offset;
    result.green_xyz_offset = green_offset;
    result.blue_xyz_offset = blue_offset;
    return result;
}

static TestIccBuild build_gray_icc(unsigned char *data,
                                   size_t capacity) {
    TestIccBuild result;
    size_t white_offset;
    size_t description_offset;
    size_t copyright_offset;
    size_t curve_offset;
    size_t curve_size;
    size_t profile_size;

    memset(data, 0, capacity);
    put_u32be(data + 128U, 4U);
    description_offset = align4(132U + 4U * 12U);
    copyright_offset = align4(
        description_offset + put_mluc(data, description_offset));
    white_offset = align4(
        copyright_offset + put_mluc(data, copyright_offset));
    put_xyz(data, white_offset, 0.9642, 1.0, 0.8249);
    curve_offset = align4(white_offset + 20U);
    curve_size = put_curve(data, curve_offset, 0U);
    profile_size = align4(curve_offset + curve_size);
    put_tag(data, 0U, TEST_FOURCC('w', 't', 'p', 't'),
            white_offset, 20U);
    put_tag(data, 1U, TEST_FOURCC('k', 'T', 'R', 'C'),
            curve_offset, curve_size);
    put_tag(data, 2U, TEST_FOURCC('d', 'e', 's', 'c'),
            description_offset, 30U);
    put_tag(data, 3U, TEST_FOURCC('c', 'p', 'r', 't'),
            copyright_offset, 30U);
    put_icc_header(data, profile_size, TEST_FOURCC('G', 'R', 'A', 'Y'));
    result.size = profile_size;
    result.curve_offset = curve_offset;
    result.description_offset = description_offset;
    result.copyright_offset = copyright_offset;
    result.red_xyz_offset = 0U;
    result.green_xyz_offset = 0U;
    result.blue_xyz_offset = 0U;
    return result;
}

static AvifdecStatus query_icc(unsigned char *profile,
                               size_t profile_size,
                               uint8_t source_policy,
                               uint8_t intent,
                               AvifdecColorTransformInfo *query,
                               AvifdecError *error) {
    AvifdecColorDescription description;
    AvifdecColorOptions options;

    memset(&description, 0, sizeof(description));
    description.color_primaries = 1U;
    description.transfer_characteristics = 8U;
    description.matrix_coefficients = 0U;
    description.color_range = 1U;
    description.has_nclx = 1U;
    description.icc.data = profile;
    description.icc.size = profile_size;
    avifdec_color_options_default(&options);
    options.destination_color_primaries = 1U;
    options.destination_transfer_characteristics = 8U;
    options.source = (AvifdecColorSource)source_policy;
    options.intent = (AvifdecColorIntent)intent;
    return avifdec_color_transform_query(
        &description, &options, 0, query, error);
}

static int convert_icc_rgb(unsigned char *profile,
                           size_t profile_size,
                           uint8_t curve_mode,
                           uint8_t intent,
                           float output[4]) {
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorDescription description;
    AvifdecColorOptions options;
    AvifdecColorTransform transform;
    AvifdecError error;
    uint16_t y_plane[1];
    uint16_t u_plane[1];
    uint16_t v_plane[1];
    double red;

    red = curve_mode == 0U ? 0.75 : 0.5;
    y_plane[0] = 0U;
    u_plane[0] = 0U;
    v_plane[0] = quantize(red, 12U);
    setup_info(&info, 1U, 1U, 12U,
               1U, 8U, 0U, 1U, 0U, 0U, 0U);
    info.icc_data = profile;
    info.icc_size = profile_size;
    setup_image(&image, y_plane, u_plane, v_plane,
                1U, 1U, 12U, 0U, 0U);
    description_from_info(&info, &description);
    avifdec_color_options_default(&options);
    options.destination_color_primaries = 1U;
    options.destination_transfer_characteristics = 8U;
    options.intent = (AvifdecColorIntent)intent;
    if (avifdec_color_transform_init(
            &description, &options, 0, 0, 0U,
            &transform, &error) != AVIFDEC_OK) {
        return 0;
    }
    return convert_f32(
        &image, &info, &transform, output, 0U,
        AVIFDEC_ALPHA_STRAIGHT) == AVIFDEC_OK;
}

static int test_icc_profiles(void) {
    unsigned char profile[1024];
    TestIccBuild built;
    TestIccBuild gray_built;
    AvifdecColorTransformInfo query;
    AvifdecError error;
    float output[4];
    float relative_output[4];
    float absolute_output[4];

    built = build_rgb_icc(
        profile, sizeof(profile), 0U, 0.9642, 1.0, 0.8249);
    CHECK_STATUS(query_icc(
        profile, built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    CHECK((query.flags & AVIFDEC_COLOR_TRANSFORM_SOURCE_ICC) != 0U);
    CHECK(query.workspace_required == 0U);
    CHECK(convert_icc_rgb(
        profile, built.size, 0U,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        output));
    CHECK(close_double(output[0], 0.75, 8.0e-4));
    CHECK(fabs(output[1]) < 8.0e-4);
    CHECK(fabs(output[2]) < 8.0e-4);

    built = build_rgb_icc(
        profile, sizeof(profile), 1U, 0.9642, 1.0, 0.8249);
    CHECK_STATUS(query_icc(
        profile, built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    CHECK(convert_icc_rgb(
        profile, built.size, 1U,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        output));
    CHECK(close_double(output[0], 0.25, 9.0e-4));
    {
        static const uint8_t modes[] = { 3U, 4U, 5U, 6U };
        static const double expected[] = { 0.25, 0.50, 0.25, 0.25 };
        size_t mode_index;

        for (mode_index = 0U;
             mode_index < sizeof(modes) / sizeof(modes[0]);
             ++mode_index) {
            built = build_rgb_icc(
                profile, sizeof(profile), modes[mode_index],
                0.9642, 1.0, 0.8249);
            CHECK_STATUS(query_icc(
                profile, built.size,
                AVIFDEC_COLOR_SOURCE_AUTO,
                AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
                &query, &error), AVIFDEC_OK);
            CHECK(convert_icc_rgb(
                profile, built.size, modes[mode_index],
                AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
                output));
            CHECK(close_double(
                output[0], expected[mode_index], 1.2e-3));
        }
    }

    built = build_rgb_icc(
        profile, sizeof(profile), 2U, 0.9642, 1.0, 0.8249);
    CHECK_STATUS(query_icc(
        profile, built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    CHECK(convert_icc_rgb(
        profile, built.size, 2U,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        output));
    CHECK(close_double(output[0], 0.25, 1.2e-3));

    built = build_rgb_icc(
        profile, sizeof(profile), 4U, 0.9642, 1.0, 0.8249);
    put_fixed(profile + built.curve_offset + 24U, 1.0);
    CHECK_STATUS(query_icc(
        profile, built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    CHECK(convert_icc_rgb(
        profile, built.size, 4U,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        output));
    CHECK(close_double(output[0], 1.0, 1.2e-3));

    built = build_rgb_icc(
        profile, sizeof(profile), 7U, 0.9642, 1.0, 0.8249);
    CHECK_STATUS(query_icc(
        profile, built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    CHECK(convert_icc_rgb(
        profile, built.size, 7U,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        output));
    CHECK(close_double(output[0], 0.5, 9.0e-4));

    built = build_rgb_icc(
        profile, sizeof(profile), 0U, 0.80, 1.0, 0.70);
    CHECK(convert_icc_rgb(
        profile, built.size, 0U,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        relative_output));
    CHECK(convert_icc_rgb(
        profile, built.size, 0U,
        AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC,
        absolute_output));
    CHECK(fabs(relative_output[0] - absolute_output[0]) > 0.02f ||
          fabs(relative_output[1] - absolute_output[1]) > 0.02f ||
          fabs(relative_output[2] - absolute_output[2]) > 0.02f);

    gray_built = build_gray_icc(profile, sizeof(profile));
    CHECK_STATUS(query_icc(
        profile, gray_built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    CHECK((query.flags & AVIFDEC_COLOR_TRANSFORM_SOURCE_GRAY) != 0U);
    {
        AvifdecImageInfo info;
        AvifdecImage image;
        AvifdecColorDescription description;
        AvifdecColorOptions options;
        AvifdecColorTransform transform;
        uint16_t y_plane[1];

        y_plane[0] = quantize(0.5, 12U);
        setup_info(&info, 1U, 1U, 12U,
                   1U, 8U, 1U, 1U, 0U, 0U, 0U);
        info.monochrome = 1U;
        info.icc_data = profile;
        info.icc_size = gray_built.size;
        setup_image(&image, y_plane, 0, 0,
                    1U, 1U, 12U, 0U, 0U);
        image.monochrome = 1U;
        image.planes[1] = 0;
        image.planes[2] = 0;
        description_from_info(&info, &description);
        avifdec_color_options_default(&options);
        options.destination_color_primaries = 1U;
        options.destination_transfer_characteristics = 8U;
        CHECK_STATUS(avifdec_color_transform_init(
            &description, &options, 0, 0, 0U,
            &transform, &error), AVIFDEC_OK);
        CHECK_STATUS(convert_f32(
            &image, &info, &transform, output, 0U,
            AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
        CHECK(close_double(output[0], 0.5, 1.2e-3));
        CHECK(close_double(output[1], 0.5, 1.2e-3));
        CHECK(close_double(output[2], 0.5, 1.2e-3));
    }
    put_u32be(profile + 12U, TEST_FOURCC('p', 'r', 't', 'r'));
    CHECK_STATUS(query_icc(
        profile, gray_built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    built = build_rgb_icc(
        profile, sizeof(profile), 0U, 0.9642, 1.0, 0.8249);
    profile[8] = 2U;
    profile[9] = 0x40U;
    put_u32be(profile + built.description_offset,
              TEST_FOURCC('d', 'e', 's', 'c'));
    put_u32be(profile + built.copyright_offset,
              TEST_FOURCC('t', 'e', 'x', 't'));
    CHECK_STATUS(query_icc(
        profile, built.size,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    memset(profile + built.size, 0U, 4U);
    put_u32be(profile, (uint32_t)(built.size + 4U));
    CHECK_STATUS(query_icc(
        profile, built.size + 4U,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_OK);
    return 0;
}

static int test_icc_rejections(void) {
    unsigned char profile[1024];
    unsigned char malformed[1024];
    TestIccBuild built;
    AvifdecColorTransformInfo query;
    AvifdecError error;
    size_t first_offset;

    built = build_rgb_icc(
        profile, sizeof(profile), 0U, 0.9642, 1.0, 0.8249);
    CHECK_STATUS(query_icc(
        profile, built.size - 1U,
        AVIFDEC_COLOR_SOURCE_AUTO,
        AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
        &query, &error), AVIFDEC_TRUNCATED);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 36U, 0U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 132U + 7U * 12U,
              TEST_FOURCC('z', 'z', 'z', 'z'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + built.description_offset,
              TEST_FOURCC('t', 'e', 'x', 't'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + built.description_offset + 12U, 11U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    malformed[built.description_offset + 30U] = 1U;
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    memset(malformed + built.size, 0U, 4U);
    put_u32be(malformed, (uint32_t)(built.size + 4U));
    CHECK_STATUS(query_icc(
        malformed, built.size + 4U,
        AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    malformed[8] = 3U;
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);

    memcpy(malformed, profile, built.size);
    put_u16be(malformed + 26U, 13U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 44U, 0x12340003U);
    put_u32be(malformed + 56U, 0xdeadbeefU);
    put_u32be(malformed + 60U, 0x0000000fU);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_OK);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 44U, 0x00000004U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 60U, 0x00000010U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 12U, TEST_FOURCC('l', 'i', 'n', 'k'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 12U, TEST_FOURCC('p', 'r', 't', 'r'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 16U, TEST_FOURCC('C', 'M', 'Y', 'K'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 16U, TEST_FOURCC('2', 'C', 'L', 'R'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 20U, TEST_FOURCC('L', 'a', 'b', ' '));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed, (uint32_t)(built.size - 4U));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 132U + 4U, 0U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 132U + 12U,
              TEST_FOURCC('w', 't', 'p', 't'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    first_offset = (size_t)(
        ((uint32_t)malformed[136] << 24) |
        ((uint32_t)malformed[137] << 16) |
        ((uint32_t)malformed[138] << 8) |
        malformed[139]);
    put_u32be(malformed + 132U + 12U + 4U,
              (uint32_t)(first_offset + 4U));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    memcpy(malformed, profile, built.size);
    malformed[first_offset + 4U] = 1U;
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    {
        static const uint32_t unsupported_types[] = {
            TEST_FOURCC('m', 'A', 'B', ' '),
            TEST_FOURCC('m', 'B', 'A', ' '),
            TEST_FOURCC('m', 'f', 't', '1'),
            TEST_FOURCC('m', 'f', 't', '2'),
            TEST_FOURCC('c', 'l', 'u', 't')
        };
        size_t type_index;

        for (type_index = 0U;
             type_index <
                sizeof(unsupported_types) / sizeof(unsupported_types[0]);
             ++type_index) {
            memcpy(malformed, profile, built.size);
            put_u32be(malformed + built.curve_offset,
                      unsupported_types[type_index]);
            CHECK_STATUS(query_icc(
                malformed, built.size,
                AVIFDEC_COLOR_SOURCE_AUTO, 0U,
                &query, &error), AVIFDEC_UNSUPPORTED);
        }
    }

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + 132U + 4U * 12U,
              TEST_FOURCC('A', '2', 'B', '0'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);

    built = build_rgb_icc(
        profile, sizeof(profile), 2U, 0.9642, 1.0, 0.8249);
    memcpy(malformed, profile, built.size);
    put_u16be(malformed + built.curve_offset + 14U, 50000U);
    put_u16be(malformed + built.curve_offset + 16U, 40000U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_OK);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + built.curve_offset + 8U, 4097U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_LIMIT_EXCEEDED);

    built = build_rgb_icc(
        profile, sizeof(profile), 1U, 0.9642, 1.0, 0.8249);
    memcpy(malformed, profile, built.size);
    put_fixed(malformed + built.curve_offset + 12U, 0.0);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    built = build_rgb_icc(
        profile, sizeof(profile), 5U, 0.9642, 1.0, 0.8249);
    memcpy(malformed, profile, built.size);
    put_fixed(malformed + built.curve_offset + 20U, -0.5);
    put_fixed(malformed + built.curve_offset + 28U, 0.25);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);

    built = build_rgb_icc(
        profile, sizeof(profile), 1U, 0.9642, 1.0, 0.8249);
    profile[8] = 2U;
    profile[9] = 0x40U;
    put_u32be(profile + built.description_offset,
              TEST_FOURCC('d', 'e', 's', 'c'));
    put_u32be(profile + built.copyright_offset,
              TEST_FOURCC('t', 'e', 'x', 't'));
    CHECK_STATUS(query_icc(
        profile, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_INVALID_DATA);
    profile[8] = 4U;
    profile[9] = 0x30U;
    put_u32be(profile + built.description_offset,
              TEST_FOURCC('m', 'l', 'u', 'c'));
    put_u32be(profile + built.copyright_offset,
              TEST_FOURCC('m', 'l', 'u', 'c'));

    CHECK_STATUS(query_icc(
        profile, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 2U,
        &query, &error), AVIFDEC_INVALID_ARGUMENT);
    CHECK_STATUS(query_icc(
        profile, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 3U,
        &query, &error), AVIFDEC_INVALID_ARGUMENT);

    CHECK_STATUS(query_icc(
        profile, (size_t)AVIFDEC_DEFAULT_MAX_ICC_BYTES + 1U,
        AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_LIMIT_EXCEEDED);

    memcpy(malformed, profile, built.size);
    put_u32be(malformed + built.curve_offset,
              TEST_FOURCC('m', 'f', 't', '2'));
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_CICP, 0U,
        &query, &error), AVIFDEC_OK);
    CHECK((query.flags & AVIFDEC_COLOR_TRANSFORM_SOURCE_CICP) != 0U);
    CHECK_STATUS(query_icc(
        malformed, built.size, AVIFDEC_COLOR_SOURCE_AUTO, 0U,
        &query, &error), AVIFDEC_UNSUPPORTED);
    return 0;
}

static int test_description_and_nclx(void) {
    AvifdecImageInfo info;
    AvifdecColorDescription description;
    AvifColorCicp nclx;
    AvifColorAv1Cicp av1;
    AvifdecColorOptions options;
    AvifdecColorTransformInfo query;
    AvifdecError error;
    unsigned char empty_icc_marker = 0U;

    setup_info(&info, 3U, 2U, 10U,
               9U, 14U, 9U, 0U, 1U, 1U, 1U);
    CHECK_STATUS(avifdec_image_color_description(
        &info, &description, &error), AVIFDEC_OK);
    CHECK(description.color_primaries == 9U);
    CHECK(description.transfer_characteristics == 14U);
    CHECK(description.matrix_coefficients == 9U);
    CHECK(description.color_range == 0U);
    CHECK(description.has_nclx == 1U);

    memset(&nclx, 0, sizeof(nclx));
    memset(&av1, 0, sizeof(av1));
    nclx.color_primaries = 9U;
    nclx.transfer_characteristics = 14U;
    nclx.matrix_coefficients = 9U;
    nclx.color_range = 0U;
    av1.cicp = nclx;
    av1.bit_depth = 10U;
    av1.subsampling_x = 1U;
    av1.subsampling_y = 1U;
    av1.chroma_sample_position = 1U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_OK);
    av1.cicp.color_range = 1U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('n', 'c', 'l', 'x'));
    av1.cicp = nclx;
    av1.cicp.color_primaries = 1U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_INVALID_DATA);
    av1.cicp = nclx;
    av1.cicp.transfer_characteristics = 13U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_INVALID_DATA);
    av1.cicp = nclx;
    av1.cicp.matrix_coefficients = 1U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_INVALID_DATA);
    av1.cicp = nclx;
    av1.chroma_sample_position = 3U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_INVALID_DATA);
    av1.chroma_sample_position = 1U;
    av1.subsampling_y = 0U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_INVALID_DATA);
    av1.subsampling_x = 0U;
    CHECK_STATUS(avif_color_validate_nclx_av1(
        &nclx, &av1, &error), AVIFDEC_INVALID_DATA);
    description.has_nclx = 0U;
    avifdec_color_options_default(&options);
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error), AVIFDEC_OK);
    CHECK((query.flags & AVIFDEC_COLOR_TRANSFORM_SOURCE_CICP) != 0U);
    description.transfer_characteristics = 2U;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_UNSUPPORTED);
    description.transfer_characteristics = 14U;
    description.color_primaries = 2U;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_UNSUPPORTED);
    description.color_primaries = 9U;
    description.matrix_coefficients = 2U;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_UNSUPPORTED);
    description.matrix_coefficients = 14U;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_UNSUPPORTED);
    description.matrix_coefficients = 9U;
    description.icc.data = &empty_icc_marker;
    description.icc.size = 0U;
    options.source = AVIFDEC_COLOR_SOURCE_AUTO;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_TRUNCATED);
    options.source = AVIFDEC_COLOR_SOURCE_CICP;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_OK);
    options.source = AVIFDEC_COLOR_SOURCE_ICC;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error),
        AVIFDEC_TRUNCATED);
    return 0;
}

static uint64_t checksum_bytes(const unsigned char *data, size_t size) {
    uint64_t checksum;
    size_t index;

    checksum = 1469598103934665603ULL;
    for (index = 0U; index < size; ++index) {
        checksum ^= data[index];
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

static int test_rows_bounds_and_determinism(void) {
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorDescription description;
    AvifdecColorOptions options;
    AvifdecColorTransformInfo query;
    AvifdecColorTransform transform;
    AvifdecColorTransform stale;
    AvifdecColorTransform tampered;
    AvifdecRgbImage rgb;
    AvifdecError error;
    uint16_t y_plane[15];
    uint16_t u_plane[6];
    uint16_t v_plane[6];
    unsigned char workspace[16];
    unsigned char full[5U * 3U * 3U * sizeof(float)];
    unsigned char repeated[sizeof(full)];
    unsigned char rows[sizeof(full)];
    unsigned char row_buffer[5U * 3U * sizeof(float)];
    unsigned char guarded[256];
    unsigned char rgb8[5U * 3U * 3U];
    size_t row_bytes;
    size_t stride;
    size_t index;
    uint32_t row;
    uint64_t checksum;

    for (index = 0U; index < 15U; ++index) {
        y_plane[index] = (uint16_t)(900U + index * 113U);
    }
    for (index = 0U; index < 6U; ++index) {
        u_plane[index] = (uint16_t)(1200U + index * 271U);
        v_plane[index] = (uint16_t)(3000U - index * 199U);
    }
    setup_info(&info, 5U, 3U, 12U,
               9U, 13U, 9U, 1U, 1U, 1U, 0U);
    setup_image(&image, y_plane, u_plane, v_plane,
                5U, 3U, 12U, 1U, 1U);
    description_from_info(&info, &description);
    avifdec_color_options_default(&options);
    options.destination_color_primaries = 1U;
    options.destination_transfer_characteristics = 13U;
    CHECK_STATUS(avifdec_color_transform_query(
        &description, &options, 0, &query, &error), AVIFDEC_OK);
    CHECK(query.workspace_required == 0U);
    memset(workspace, 0x7c, sizeof(workspace));
    CHECK_STATUS(avifdec_color_transform_init(
        &description, &options, 0,
        workspace + 1U, sizeof(workspace) - 1U,
        &transform, &error), AVIFDEC_OK);

    memset(&rgb, 0, sizeof(rgb));
    rgb.pixels = full;
    rgb.stride = 5U * 3U * sizeof(float);
    rgb.width = 5U;
    rgb.height = 3U;
    rgb.format = AVIFDEC_RGBF32;
    rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    CHECK_STATUS(avifdec_image_to_rgb_with_transform(
        &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
    rgb.pixels = repeated;
    CHECK_STATUS(avifdec_image_to_rgb_with_transform(
        &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
    CHECK(memcmp(full, repeated, sizeof(full)) == 0);

    row_bytes = 5U * 3U * sizeof(float);
    memset(rows, 0, sizeof(rows));
    for (row = 0U; row < 3U; ++row) {
        memset(row_buffer, 0xa5, sizeof(row_buffer));
        rgb.pixels = row_buffer;
        rgb.stride = row_bytes;
        CHECK_STATUS(avifdec_image_to_rgb_row_with_transform(
            &image, &info, &transform, &rgb, row, &error),
            AVIFDEC_OK);
        memcpy(rows + (size_t)row * row_bytes,
               row_buffer, row_bytes);
    }
    CHECK(memcmp(full, rows, sizeof(full)) == 0);

    memset(guarded, 0xa5, sizeof(guarded));
    stride = row_bytes + 5U;
    rgb.pixels = guarded + 1U;
    rgb.stride = stride;
    CHECK_STATUS(avifdec_image_to_rgb_with_transform(
        &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
    CHECK(guarded[0] == 0xa5U);
    for (row = 0U; row < 3U; ++row) {
        size_t gap;

        for (gap = row_bytes;
             gap < stride && 1U + (size_t)row * stride + gap <
                 sizeof(guarded);
             ++gap) {
            CHECK(guarded[
                1U + (size_t)row * stride + gap] == 0xa5U);
        }
    }
    CHECK(guarded[1U + 2U * stride + row_bytes] == 0xa5U);

    rgb.pixels = rgb8;
    rgb.stride = 5U * 3U;
    rgb.format = AVIFDEC_RGB8;
    CHECK_STATUS(avifdec_image_to_rgb_with_transform(
        &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
    checksum = checksum_bytes(rgb8, sizeof(rgb8));
    CHECK(checksum == 17735259792848881138ULL);

    memset(&stale, 0, sizeof(stale));
    rgb.pixels = rgb8;
    CHECK_STATUS(avifdec_image_to_rgb_with_transform(
        &image, &info, &stale, &rgb, &error),
        AVIFDEC_INVALID_ARGUMENT);
    memcpy(&tampered, &transform, sizeof(tampered));
    tampered.opaque[10] ^= (uintptr_t)1U;
    CHECK_STATUS(avifdec_image_to_rgb_with_transform(
        &image, &info, &tampered, &rgb, &error),
        AVIFDEC_INVALID_ARGUMENT);
    y_plane[0] = 4096U;
    memset(rgb8, 0xa5, sizeof(rgb8));
    CHECK_STATUS(avifdec_image_to_rgb_with_transform(
        &image, &info, &transform, &rgb, &error),
        AVIFDEC_INVALID_DATA);
    CHECK(rgb8[0] == 0xa5U);
    return 0;
}

static int test_output_formats_and_clipping(void) {
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorTransform transform;
    AvifdecRgbImage rgb;
    AvifdecError error;
    uint16_t y_plane[1];
    uint16_t u_plane[1];
    uint16_t v_plane[1];
    unsigned char guarded[80];
    uint8_t format;

    y_plane[0] = quantize(0.5, 12U);
    u_plane[0] = quantize_chroma(-0.45, 12U);
    v_plane[0] = quantize_chroma(0.45, 12U);
    setup_info(&info, 1U, 1U, 12U,
               1U, 8U, 1U, 1U, 0U, 0U, 0U);
    setup_image(&image, y_plane, u_plane, v_plane,
                1U, 1U, 12U, 0U, 0U);
    CHECK_STATUS(init_transform(
        &info, 1U, 13U,
        AVIFDEC_CHROMA_UPSAMPLING_BILINEAR,
        AVIFDEC_COLOR_HDR_REJECT,
        &transform), AVIFDEC_OK);
    for (format = AVIFDEC_RGB8;
         format <= AVIFDEC_RGBAF32;
         ++format) {
        size_t channels;
        size_t channel_bytes;
        size_t pixel_bytes;
        int has_alpha;
        int is_float;
        int is_16;

        has_alpha = format == AVIFDEC_RGBA8 ||
                    format == AVIFDEC_RGBA16 ||
                    format == AVIFDEC_RGBAF32;
        is_float = format == AVIFDEC_RGBF32 ||
                   format == AVIFDEC_RGBAF32;
        is_16 = format == AVIFDEC_RGB16 ||
                format == AVIFDEC_RGBA16;
        channels = has_alpha ? 4U : 3U;
        channel_bytes = is_float ? sizeof(float) : (is_16 ? 2U : 1U);
        pixel_bytes = channels * channel_bytes;
        memset(guarded, 0xa5, sizeof(guarded));
        memset(&rgb, 0, sizeof(rgb));
        rgb.pixels = guarded + 1U;
        rgb.stride = pixel_bytes;
        rgb.width = 1U;
        rgb.height = 1U;
        rgb.format = format;
        rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
        CHECK_STATUS(avifdec_image_to_rgb_with_transform(
            &image, &info, &transform, &rgb, &error), AVIFDEC_OK);
        CHECK(guarded[0] == 0xa5U);
        CHECK(guarded[1U + pixel_bytes] == 0xa5U);
        if (!is_float) {
            if (is_16) {
                uint16_t first;
                uint16_t third;

                memcpy(&first, guarded + 1U, sizeof(first));
                memcpy(&third, guarded + 1U + 4U, sizeof(third));
                CHECK(first == 65535U);
                CHECK(third == 0U);
            } else {
                CHECK(guarded[1U] == 255U);
                CHECK(guarded[3U] == 0U);
            }
        } else {
            float first;
            float third;

            memcpy(&first, guarded + 1U, sizeof(first));
            memcpy(&third,
                   guarded + 1U + 2U * sizeof(float),
                   sizeof(third));
            CHECK(first > 1.0f);
            CHECK(third < 0.0f);
        }
    }
    return 0;
}

typedef struct {
    const AvifdecColorTransform *output;
    const AvifdecColorTransform *source;
} TestGainColorContext;

static AvifdecStatus test_gain_validate_transform(
    void *opaque,
    const AvifGainMapColorDescription *working,
    uint8_t output_format,
    AvifdecError *error) {
    TestGainColorContext *context = (TestGainColorContext *)opaque;

    return avif_color_transform_validate_working(
        context->output, working, output_format, error);
}

static AvifdecStatus test_gain_base_to_working(
    void *opaque,
    const AvifdecImage *base_image,
    const AvifdecImageInfo *base_info,
    const AvifGainMapColorDescription *working,
    uint32_t x,
    uint32_t y,
    float rgba[4],
    AvifdecError *error) {
    TestGainColorContext *context = (TestGainColorContext *)opaque;

    (void)working;
    return avif_color_image_pixel_to_working(
        base_image, base_info, context->source, context->output,
        x, y, rgba, error);
}

static AvifdecStatus test_gain_texel(
    void *opaque,
    const AvifdecImage *gain_image,
    const AvifdecImageInfo *gain_info,
    uint32_t x,
    uint32_t y,
    uint8_t channel_count,
    float gain[3],
    AvifdecError *error) {
    (void)opaque;
    (void)gain_image;
    (void)gain_info;
    (void)x;
    (void)y;
    (void)error;
    gain[0] = 0.0f;
    if (channel_count == 3U) {
        gain[1] = 0.0f;
        gain[2] = 0.0f;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus test_gain_working_to_linear(
    void *opaque,
    const float working[3],
    float output[3],
    AvifdecError *error) {
    TestGainColorContext *context = (TestGainColorContext *)opaque;

    return avif_color_transform_linear_to_linear(
        context->output, working, output, error);
}

static AvifdecStatus test_gain_working_to_encoded16(
    void *opaque,
    const float working[3],
    uint16_t output[3],
    AvifdecError *error) {
    TestGainColorContext *context = (TestGainColorContext *)opaque;

    return avif_color_transform_linear_to_encoded16(
        context->output, working, output, error);
}

static int test_gain_map_color_adapter(void) {
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecColorDescription description;
    AvifdecColorDescription mismatch;
    AvifdecColorOptions options;
    AvifdecColorTransform transform;
    AvifdecColorTransform inherited;
    AvifdecColorTransform tampered;
    AvifdecColorTransform icc_output;
    AvifdecColorTransform source_to_working;
    AvifdecColorTransform direct_absolute;
    AvifdecError error;
    uint16_t y_plane[1];
    uint16_t u_plane[1];
    uint16_t v_plane[1];
    float full[4];
    float pixel[4];
    float working[3] = { 0.25f, 0.5f, 0.75f };
    float linear[3];
    uint16_t encoded[3];
    unsigned char profile[16384];
    TestIccBuild icc_built;
    size_t index;

    y_plane[0] = quantize(0.5, 12U);
    u_plane[0] = quantize(0.75, 12U);
    v_plane[0] = quantize(0.25, 12U);
    setup_info(&info, 1U, 1U, 12U,
               1U, 13U, 0U, 1U, 0U, 0U, 0U);
    setup_image(&image, y_plane, u_plane, v_plane,
                1U, 1U, 12U, 0U, 0U);
    description_from_info(&info, &description);
    avifdec_color_options_default(&options);
    options.destination_color_primaries = 1U;
    options.destination_transfer_characteristics = 13U;
    CHECK_STATUS(avifdec_color_transform_init(
        &description, &options, 0, 0, 0U,
        &transform, &error), AVIFDEC_OK);
    CHECK_STATUS(avif_color_transform_validate_working(
        &transform, &description, AVIFDEC_RGBF32, &error),
        AVIFDEC_OK);
    CHECK_STATUS(avif_color_transform_validate_working(
        &transform, &description, AVIFDEC_RGB8, &error),
        AVIFDEC_INVALID_ARGUMENT);
    mismatch = description;
    mismatch.color_range = 0U;
    CHECK_STATUS(avif_color_transform_validate_working(
        &transform, &mismatch, AVIFDEC_RGBF32, &error),
        AVIFDEC_INVALID_ARGUMENT);

    avifdec_color_options_default(&options);
    CHECK_STATUS(avifdec_color_transform_init(
        &description, &options, 0, 0, 0U,
        &inherited, &error), AVIFDEC_OK);
    CHECK_STATUS(avif_color_transform_validate_working(
        &inherited, &description, AVIFDEC_RGBF32, &error),
        AVIFDEC_INVALID_ARGUMENT);
    CHECK_STATUS(avif_color_transform_init_source_to_working(
        &description, &inherited,
        &source_to_working, &error), AVIFDEC_INVALID_ARGUMENT);

    CHECK_STATUS(convert_f32(
        &image, &info, &transform, full, 1U,
        AVIFDEC_ALPHA_STRAIGHT), AVIFDEC_OK);
    CHECK_STATUS(avif_color_image_pixel_to_linear(
        &image, &info, &transform, 0U, 0U, pixel, &error),
        AVIFDEC_OK);
    for (index = 0U; index < 4U; ++index) {
        CHECK(memcmp(&full[index], &pixel[index], sizeof(float)) == 0);
    }
    CHECK_STATUS(avif_color_image_pixel_to_linear(
        &image, &info, &transform, 1U, 0U, pixel, &error),
        AVIFDEC_INVALID_ARGUMENT);

    CHECK_STATUS(avif_color_transform_linear_to_linear(
        &transform, working, linear, &error), AVIFDEC_OK);
    for (index = 0U; index < 3U; ++index) {
        CHECK(close_double(linear[index], working[index], 2.0e-7));
    }
    CHECK_STATUS(avif_color_transform_linear_to_encoded16(
        &transform, working, encoded, &error), AVIFDEC_OK);
    for (index = 0U; index < 3U; ++index) {
        uint16_t expected;

        expected = (uint16_t)llround(
            reference_transfer_encode(13U, working[index]) * 65535.0);
        CHECK(abs((int)encoded[index] - (int)expected) <= 2);
    }
    {
        AvifdecColorDescription wide;
        AvifdecColorTransform wide_transform;
        double matrix[9];
        double input[3];
        double expected[3];

        wide = description;
        wide.color_primaries = 12U;
        avifdec_color_options_default(&options);
        options.destination_color_primaries = 1U;
        options.destination_transfer_characteristics = 13U;
        CHECK_STATUS(avifdec_color_transform_init(
            &wide, &options, 0, 0, 0U,
            &wide_transform, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_primaries_conversion(
            12U, 1U,
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC,
            matrix), AVIFDEC_OK);
        input[0] = working[0];
        input[1] = working[1];
        input[2] = working[2];
        multiply3(matrix, input, expected);
        CHECK_STATUS(avif_color_transform_linear_to_linear(
            &wide_transform, working, linear, &error), AVIFDEC_OK);
        CHECK(close_double(linear[0], expected[0], 3.0e-6));
        CHECK(close_double(linear[1], expected[1], 3.0e-6));
        CHECK(close_double(linear[2], expected[2], 3.0e-6));
    }
    {
        AvifdecColorDescription icc_working;
        float working_pixel[4];
        float converted[3];
        float absolute_expected[4];

        icc_built = build_rgb_icc(
            profile, sizeof(profile), 0U,
            0.9642, 1.0, 0.8249);
        put_xyz(profile, icc_built.red_xyz_offset,
                0.5151187, 0.2411892, -0.00105045);
        put_xyz(profile, icc_built.green_xyz_offset,
                0.2919778, 0.6922441, 0.0418791);
        put_xyz(profile, icc_built.blue_xyz_offset,
                0.1571035, 0.0665668, 0.7840713);
        icc_working = description;
        icc_working.color_primaries = 12U;
        icc_working.icc.data = profile;
        icc_working.icc.size = icc_built.size;
        avifdec_color_options_default(&options);
        options.destination_color_primaries = 1U;
        options.destination_transfer_characteristics = 13U;
        options.source = AVIFDEC_COLOR_SOURCE_ICC;
        CHECK_STATUS(avifdec_color_transform_init(
            &icc_working, &options, 0, 0, 0U,
            &icc_output, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_transform_validate_working(
            &icc_output, &icc_working,
            AVIFDEC_RGBF32, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_transform_init_source_to_working(
            &description, &icc_output,
            &source_to_working, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_image_pixel_to_working(
            &image, &info, &source_to_working,
            &icc_output, 0U, 0U,
            working_pixel, &error), AVIFDEC_OK);
        CHECK(fabs(working_pixel[0] - full[0]) > 0.01f ||
              fabs(working_pixel[1] - full[1]) > 0.01f ||
              fabs(working_pixel[2] - full[2]) > 0.01f);
        CHECK_STATUS(avif_color_transform_linear_to_linear(
            &icc_output, working_pixel,
            converted, &error), AVIFDEC_OK);
        CHECK(close_double(converted[0], full[0], 4.0e-5));
        CHECK(close_double(converted[1], full[1], 4.0e-5));
        CHECK(close_double(converted[2], full[2], 4.0e-5));
        CHECK(working_pixel[3] == 1.0f);

        options.intent =
            AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC;
        CHECK_STATUS(avifdec_color_transform_init(
            &icc_working, &options, 0, 0, 0U,
            &icc_output, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_transform_init_source_to_working(
            &description, &icc_output,
            &source_to_working, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_image_pixel_to_working(
            &image, &info, &source_to_working,
            &icc_output, 0U, 0U,
            working_pixel, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_transform_linear_to_linear(
            &icc_output, working_pixel,
            converted, &error), AVIFDEC_OK);
        options.source = AVIFDEC_COLOR_SOURCE_AUTO;
        CHECK_STATUS(avifdec_color_transform_init(
            &description, &options, 0, 0, 0U,
            &direct_absolute, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_image_pixel_to_linear(
            &image, &info, &direct_absolute,
            0U, 0U, absolute_expected, &error), AVIFDEC_OK);
        CHECK(close_double(
            converted[0], absolute_expected[0], 4.0e-5));
        CHECK(close_double(
            converted[1], absolute_expected[1], 4.0e-5));
        CHECK(close_double(
            converted[2], absolute_expected[2], 4.0e-5));

        icc_built = build_gray_icc(profile, sizeof(profile));
        icc_working = description;
        icc_working.icc.data = profile;
        icc_working.icc.size = icc_built.size;
        avifdec_color_options_default(&options);
        options.destination_color_primaries = 1U;
        options.destination_transfer_characteristics = 13U;
        CHECK_STATUS(avifdec_color_transform_init(
            &icc_working, &options, 0, 0, 0U,
            &icc_output, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_transform_init_source_to_working(
            &description, &icc_output,
            &source_to_working, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_image_pixel_to_working(
            &image, &info, &source_to_working,
            &icc_output, 0U, 0U,
            working_pixel, &error), AVIFDEC_OK);
        CHECK(working_pixel[0] == working_pixel[1]);
        CHECK(working_pixel[0] == working_pixel[2]);
        CHECK_STATUS(avif_color_transform_linear_to_linear(
            &icc_output, working_pixel,
            converted, &error), AVIFDEC_OK);
        CHECK(close_double(converted[0], converted[1], 3.0e-6));
        CHECK(close_double(converted[0], converted[2], 3.0e-6));
        working_pixel[1] += 0.01f;
        CHECK_STATUS(avif_color_transform_linear_to_linear(
            &icc_output, working_pixel,
            converted, &error), AVIFDEC_UNSUPPORTED);
    }
    {
        const uint32_t curve_entries = 4097U;
        const size_t curve_size =
            12U + (size_t)curve_entries * 2U;
        AvifdecColorDescription large_source;
        AvifdecColorOptions large_options;
        AvifdecColorTransform large_output;
        AvifdecColorTransform large_source_to_working;
        AvifdecLimits large_limits;
        AvifdecImageInfo large_info;
        AvifdecImageInfo gain_info;
        AvifdecImage gain_image;
        AvifGainMapInfo gain_map;
        AvifGainMapApplyOptions apply_options;
        AvifGainMapColorAdapter adapter;
        TestGainColorContext gain_context;
        AvifdecRgbImage output;
        uint16_t gain_sample = 0U;
        float gain_output[3];
        size_t profile_size;
        uint32_t sample_index;
        size_t tag_index;

        icc_built = build_rgb_icc(
            profile, sizeof(profile), 2U,
            0.9642, 1.0, 0.8249);
        CHECK(icc_built.curve_offset + curve_size <=
              sizeof(profile));
        memset(
            profile + icc_built.curve_offset, 0U, curve_size);
        put_u32be(
            profile + icc_built.curve_offset,
            TEST_FOURCC('c', 'u', 'r', 'v'));
        put_u32be(
            profile + icc_built.curve_offset + 8U,
            curve_entries);
        for (sample_index = 0U;
             sample_index < curve_entries;
             ++sample_index) {
            uint16_t value = (uint16_t)(
                ((uint32_t)sample_index * 65535U) /
                (curve_entries - 1U));

            put_u16be(
                profile + icc_built.curve_offset + 12U +
                    (size_t)sample_index * 2U,
                value);
        }
        profile_size = align4(
            icc_built.curve_offset + curve_size);
        put_u32be(profile, (uint32_t)profile_size);
        for (tag_index = 4U; tag_index <= 6U; ++tag_index) {
            put_u32be(
                profile + 132U + tag_index * 12U + 8U,
                (uint32_t)curve_size);
        }
        large_source = description;
        large_source.icc.data = profile;
        large_source.icc.size = profile_size;
        avifdec_color_options_default(&large_options);
        large_options.destination_color_primaries = 1U;
        large_options.destination_transfer_characteristics = 13U;
        CHECK_STATUS(avifdec_color_transform_init(
            &large_source, &large_options, 0, 0, 0U,
            &large_output, &error), AVIFDEC_LIMIT_EXCEEDED);
        memset(&large_limits, 0, sizeof(large_limits));
        large_limits.max_icc_curve_entries = curve_entries;
        CHECK_STATUS(avifdec_color_transform_init(
            &large_source, &large_options, &large_limits, 0, 0U,
            &large_output, &error), AVIFDEC_OK);
        CHECK_STATUS(avif_color_transform_init_source_to_working(
            &large_source, &large_output,
            &large_source_to_working, &error), AVIFDEC_OK);

        large_info = info;
        large_info.icc_data = profile;
        large_info.icc_size = profile_size;
        gain_info = large_info;
        gain_info.monochrome = 1U;
        gain_info.channel_count = 1U;
        gain_info.subsampling_x = 0U;
        gain_info.subsampling_y = 0U;
        gain_info.icc_data = 0;
        gain_info.icc_size = 0U;
        memset(&gain_image, 0, sizeof(gain_image));
        gain_image.planes[0] = &gain_sample;
        gain_image.strides[0] = 1U;
        gain_image.widths[0] = 1U;
        gain_image.heights[0] = 1U;
        gain_image.bit_depth = gain_info.bit_depth;
        gain_image.monochrome = 1U;
        memset(&gain_map, 0, sizeof(gain_map));
        gain_map.present = 1U;
        gain_map.base_image = large_info;
        gain_map.gain_map_image = gain_info;
        gain_map.base_color = large_source;
        gain_map.alternate_color = large_source;
        gain_map.metadata.channel_count = 1U;
        gain_map.metadata.use_base_color_space = 1U;
        gain_map.metadata.base_hdr_headroom.denominator = 1U;
        gain_map.metadata.alternate_hdr_headroom.denominator = 1U;
        gain_map.metadata.gain_map_min[0].denominator = 1U;
        gain_map.metadata.gain_map_max[0].denominator = 1U;
        gain_map.metadata.gain_map_gamma[0].numerator = 1U;
        gain_map.metadata.gain_map_gamma[0].denominator = 1U;
        gain_map.metadata.base_offset[0].denominator = 1U;
        gain_map.metadata.alternate_offset[0].denominator = 1U;
        gain_context.output = &large_output;
        gain_context.source = &large_source_to_working;
        memset(&adapter, 0, sizeof(adapter));
        adapter.context = &gain_context;
        adapter.validate_transform = test_gain_validate_transform;
        adapter.base_to_working = test_gain_base_to_working;
        adapter.gain_texel = test_gain_texel;
        adapter.working_to_linear = test_gain_working_to_linear;
        adapter.working_to_encoded16 =
            test_gain_working_to_encoded16;
        apply_options.display_headroom = 1.0f;
        apply_options.flags = 0U;
        memset(&output, 0, sizeof(output));
        output.pixels = gain_output;
        output.stride = sizeof(gain_output);
        output.width = 1U;
        output.height = 1U;
        output.format = AVIFDEC_RGBF32;
        output.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
        CHECK_STATUS(avif_gain_map_apply(
            &image, &gain_image, &gain_map, &adapter,
            &apply_options, &output, &error), AVIFDEC_OK);
        CHECK(isfinite(gain_output[0]) &&
              isfinite(gain_output[1]) &&
              isfinite(gain_output[2]));
    }
    working[1] = nanf("");
    CHECK_STATUS(avif_color_transform_linear_to_linear(
        &transform, working, linear, &error),
        AVIFDEC_INVALID_ARGUMENT);
    CHECK(linear[0] == 0.0f &&
          linear[1] == 0.0f &&
          linear[2] == 0.0f);

    memcpy(&tampered, &transform, sizeof(tampered));
    tampered.opaque[20] ^= (uintptr_t)1U;
    CHECK_STATUS(avif_color_transform_linear_to_encoded16(
        &tampered, (const float[3]){ 0.1f, 0.2f, 0.3f },
        encoded, &error), AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int run_test(const char *name, int (*test)(void)) {
    int line;

    line = test();
    if (line != 0) {
        fprintf(stderr, "color test failed: %s (line %d)\n", name, line);
        return line;
    }
    return 0;
}

int main(void) {
    int result;

    result = run_test("transfer functions", test_transfer_functions);
    if (result != 0) return result;
    result = run_test("primaries", test_primaries);
    if (result != 0) return result;
    result = run_test("matrix coefficients", test_matrix_coefficients);
    if (result != 0) return result;
    result = run_test("depths and ranges", test_depths_and_ranges);
    if (result != 0) return result;
    result = run_test(
        "chroma siting and odd edges", test_chroma_siting_and_odd_edges);
    if (result != 0) return result;
    result = run_test("HDR policy", test_hdr_policy);
    if (result != 0) return result;
    result = run_test("alpha association", test_alpha_association);
    if (result != 0) return result;
    result = run_test("ICC profiles", test_icc_profiles);
    if (result != 0) return result;
    result = run_test("ICC rejections", test_icc_rejections);
    if (result != 0) return result;
    result = run_test("description and NCLX", test_description_and_nclx);
    if (result != 0) return result;
    result = run_test(
        "rows, bounds, determinism", test_rows_bounds_and_determinism);
    if (result != 0) return result;
    result = run_test(
        "output formats and clipping", test_output_formats_and_clipping);
    if (result != 0) return result;
    result = run_test(
        "gain-map color adapter", test_gain_map_color_adapter);
    if (result != 0) return result;
    puts("color_unit: all tests passed");
    return 0;
}
