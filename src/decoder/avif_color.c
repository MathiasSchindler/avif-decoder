#include "avif_color.h"

#define AVIF_COLOR_MAGIC 0x41564946434f4c52ULL
#define AVIF_COLOR_INTERNAL_VERSION 4U
#define AVIF_COLOR_FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | (uint32_t)(d))

#define AVIF_COLOR_SIG_ACSP AVIF_COLOR_FOURCC('a', 'c', 's', 'p')
#define AVIF_COLOR_SIG_XYZ AVIF_COLOR_FOURCC('X', 'Y', 'Z', ' ')
#define AVIF_COLOR_SIG_RGB AVIF_COLOR_FOURCC('R', 'G', 'B', ' ')
#define AVIF_COLOR_SIG_GRAY AVIF_COLOR_FOURCC('G', 'R', 'A', 'Y')
#define AVIF_COLOR_SIG_CURV AVIF_COLOR_FOURCC('c', 'u', 'r', 'v')
#define AVIF_COLOR_SIG_PARA AVIF_COLOR_FOURCC('p', 'a', 'r', 'a')
#define AVIF_COLOR_SIG_MLUC AVIF_COLOR_FOURCC('m', 'l', 'u', 'c')
#define AVIF_COLOR_SIG_DESC AVIF_COLOR_FOURCC('d', 'e', 's', 'c')
#define AVIF_COLOR_SIG_TEXT AVIF_COLOR_FOURCC('t', 'e', 'x', 't')
#define AVIF_COLOR_MAX_ICC_TAGS 4096U

typedef enum {
    AVIF_COLOR_CURVE_IDENTITY = 0,
    AVIF_COLOR_CURVE_GAMMA = 1,
    AVIF_COLOR_CURVE_TABLE = 2,
    AVIF_COLOR_CURVE_PARAMETRIC = 3
} AvifColorCurveKind;

typedef struct {
    const unsigned char *samples;
    uint32_t count;
    uint8_t kind;
    uint8_t function_type;
    uint8_t parameter_count;
    uint8_t reserved;
    float parameters[7];
} AvifColorCurve;

typedef struct {
    uint32_t flags;
    AvifdecColorDescription source;
    AvifdecColorDescription destination;
    AvifdecColorOptions options;
    float color_matrix[9];
    float connection_matrix[9];
    float connection_inverse[9];
    float special_inverse[9];
    float special_to_rgb[9];
    AvifColorCurve curves[3];
    uint8_t curve_count;
    uint8_t source_is_gray;
    uint8_t source_is_icc;
    uint8_t special_linear;
    uint8_t connection_invertible;
    uint8_t reserved[3];
    size_t max_icc_bytes;
    size_t max_icc_curve_entries;
} AvifColorInternal;

typedef struct {
    uint64_t magic;
    uint64_t checksum;
    uint32_t version;
    uint32_t flags;
    AvifdecColorDescription source;
    AvifdecColorDescription destination;
    AvifdecColorOptions options;
    void *workspace;
    size_t workspace_size;
    size_t max_icc_bytes;
    size_t max_icc_curve_entries;
} AvifColorHandle;

typedef struct {
    double rgb_to_xyz[9];
    double media_white[3];
    AvifColorCurve curves[3];
    uint8_t curve_count;
    uint8_t gray;
} AvifColorIcc;

typedef struct {
    double x;
    double y;
} AvifColorXy;

typedef struct {
    AvifColorXy red;
    AvifColorXy green;
    AvifColorXy blue;
    AvifColorXy white;
    uint8_t xyz;
} AvifColorPrimaries;

_Static_assert(sizeof(AvifColorHandle) <= sizeof(AvifdecColorTransform),
               "private color handle exceeds public opaque storage");

static AvifdecStatus avif_color_prepare(
    const AvifdecColorDescription *source,
    const AvifdecColorOptions *options,
    const AvifdecLimits *limits,
    AvifColorInternal *internal,
    AvifdecColorTransformInfo *info,
    AvifdecError *error);

static void avif_color_copy(void *destination,
                            const void *source,
                            size_t count) {
    unsigned char *output;
    const unsigned char *input;
    size_t index;

    output = (unsigned char *)destination;
    input = (const unsigned char *)source;
    for (index = 0U; index < count; ++index) {
        output[index] = input[index];
    }
}

static void avif_color_fill(void *destination,
                            unsigned char value,
                            size_t count) {
    unsigned char *output;
    size_t index;

    output = (unsigned char *)destination;
    for (index = 0U; index < count; ++index) {
        output[index] = value;
    }
}

static int avif_color_size_add(size_t left,
                               size_t right,
                               size_t *result) {
    if (result == 0 || right > SIZE_MAX - left) {
        return 0;
    }
    *result = left + right;
    return 1;
}

static int avif_color_size_multiply(size_t left,
                                    size_t right,
                                    size_t *result) {
    if (result == 0 || (left != 0U && right > SIZE_MAX / left)) {
        return 0;
    }
    *result = left * right;
    return 1;
}

static uint16_t avif_color_u16be(const unsigned char *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static uint32_t avif_color_u32be(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static int32_t avif_color_s32be(const unsigned char *bytes) {
    uint32_t value;
    uint32_t magnitude;

    value = avif_color_u32be(bytes);
    if ((value & 0x80000000U) != 0U) {
        magnitude = (~value) + 1U;
        if (magnitude == 0x80000000U) {
            return (-2147483647 - 1);
        }
        return -(int32_t)magnitude;
    }
    return (int32_t)value;
}

static AvifdecStatus avif_color_fail(AvifdecError *error,
                                     AvifdecStatus status,
                                     size_t offset,
                                     uint32_t context) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = context;
    }
    return status;
}

static void avif_color_error_clear(AvifdecError *error) {
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
}

static uint64_t avif_color_transform_checksum(
    const AvifdecColorTransform *transform) {
    const unsigned char *bytes;
    const size_t checksum_offset =
        offsetof(AvifColorHandle, checksum);
    const size_t checksum_end =
        checksum_offset + sizeof(uint64_t);
    size_t index;
    uint64_t checksum;

    bytes = (const unsigned char *)transform;
    checksum = 1469598103934665603ULL;
    for (index = 0U; index < sizeof(*transform); ++index) {
        unsigned char value;

        value = index >= checksum_offset &&
                index < checksum_end
            ? 0U
            : bytes[index];
        checksum ^= value;
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

static AvifdecStatus avif_color_transform_load(
    const AvifdecColorTransform *transform,
    AvifColorInternal *internal,
    AvifdecError *error) {
    AvifColorHandle handle;
    AvifdecLimits limits;
    uint64_t stored_checksum;
    AvifdecStatus status;

    if (transform == 0 || internal == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avif_color_fill(&handle, 0U, sizeof(handle));
    avif_color_copy(&handle, transform, sizeof(handle));
    stored_checksum = handle.checksum;
    if (handle.magic != AVIF_COLOR_MAGIC ||
        handle.version != AVIF_COLOR_INTERNAL_VERSION ||
        avif_color_transform_checksum(transform) != stored_checksum ||
        (handle.workspace == 0 &&
         handle.workspace_size != 0U)) {
        avif_color_fill(internal, 0U, sizeof(*internal));
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avif_color_fill(&limits, 0U, sizeof(limits));
    limits.max_icc_bytes = handle.max_icc_bytes;
    limits.max_icc_curve_entries =
        handle.max_icc_curve_entries;
    status = avif_color_prepare(
        &handle.source, &handle.options, &limits,
        internal, 0, error);
    if (status != AVIFDEC_OK) return status;
    if (internal->flags != handle.flags ||
        internal->destination.color_primaries !=
            handle.destination.color_primaries ||
        internal->destination.transfer_characteristics !=
            handle.destination.transfer_characteristics ||
        internal->destination.matrix_coefficients !=
            handle.destination.matrix_coefficients ||
        internal->destination.color_range !=
            handle.destination.color_range ||
        internal->destination.has_nclx !=
            handle.destination.has_nclx) {
        avif_color_fill(internal, 0U, sizeof(*internal));
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return AVIFDEC_OK;
}

static double avif_color_abs(double value) {
    return value < 0.0 ? -value : value;
}

static double avif_color_max(double left, double right) {
    return left > right ? left : right;
}

static double avif_color_clamp(double value,
                               double minimum,
                               double maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static int avif_color_finite(double value) {
    return value == value &&
           value <= 1.7976931348623157e308 &&
           value >= -1.7976931348623157e308;
}

static double avif_color_scale2(double value, int exponent) {
    int index;

    if (exponent > 1023) {
        return 1.7976931348623157e308;
    }
    if (exponent < -1074) {
        return 0.0;
    }
    while (exponent >= 32) {
        value *= 4294967296.0;
        exponent -= 32;
    }
    while (exponent <= -32) {
        value *= 2.3283064365386962890625e-10;
        exponent += 32;
    }
    if (exponent >= 0) {
        for (index = 0; index < exponent; ++index) {
            value *= 2.0;
        }
    } else {
        for (index = 0; index > exponent; --index) {
            value *= 0.5;
        }
    }
    return value;
}

static double avif_color_log(double value) {
    const double ln2 = 0.69314718055994530942;
    double y;
    double y_squared;
    double term;
    double sum;
    int exponent;
    int denominator;

    if (value <= 0.0) {
        return -1.7976931348623157e308;
    }
    exponent = 0;
    while (value >= 2.0 && exponent < 1023) {
        value *= 0.5;
        ++exponent;
    }
    while (value < 1.0 && exponent > -1074) {
        value *= 2.0;
        --exponent;
    }
    y = (value - 1.0) / (value + 1.0);
    y_squared = y * y;
    term = y;
    sum = 0.0;
    denominator = 1;
    while (denominator <= 31) {
        sum += term / (double)denominator;
        term *= y_squared;
        denominator += 2;
    }
    return 2.0 * sum + (double)exponent * ln2;
}

static double avif_color_exp(double value) {
    const double ln2 = 0.69314718055994530942;
    double quotient;
    double remainder;
    double term;
    double sum;
    int exponent;
    int index;

    if (value > 709.0) {
        return 1.7976931348623157e308;
    }
    if (value < -745.0) {
        return 0.0;
    }
    quotient = value / ln2;
    if (quotient >= 0.0) {
        exponent = (int)(quotient + 0.5);
    } else {
        exponent = (int)(quotient - 0.5);
    }
    remainder = value - (double)exponent * ln2;
    term = 1.0;
    sum = 1.0;
    for (index = 1; index <= 18; ++index) {
        term *= remainder / (double)index;
        sum += term;
    }
    return avif_color_scale2(sum, exponent);
}

static double avif_color_pow(double base, double exponent) {
    if (base < 0.0) {
        return 0.0;
    }
    if (base == 0.0) {
        return exponent == 0.0 ? 1.0 : 0.0;
    }
    return avif_color_exp(exponent * avif_color_log(base));
}

static double avif_color_log10(double value) {
    return avif_color_log(value) * 0.43429448190325182765;
}

static double avif_color_round_nonnegative(double value) {
    uint64_t integer;

    if (value <= 0.0) {
        return 0.0;
    }
    if (value >= 18446744073709549568.0) {
        return 18446744073709549568.0;
    }
    integer = (uint64_t)(value + 0.5);
    return (double)integer;
}

static void avif_color_store_u16(unsigned char *destination,
                                 uint16_t value) {
    avif_color_copy(destination, &value, sizeof(value));
}

static void avif_color_store_f32(unsigned char *destination,
                                 float value) {
    avif_color_copy(destination, &value, sizeof(value));
}

static void avif_color_matrix_identity(double matrix[9]) {
    size_t index;

    for (index = 0U; index < 9U; ++index) {
        matrix[index] = 0.0;
    }
    matrix[0] = 1.0;
    matrix[4] = 1.0;
    matrix[8] = 1.0;
}

static void avif_color_matrix_copy(double destination[9],
                                   const double source[9]) {
    size_t index;

    for (index = 0U; index < 9U; ++index) {
        destination[index] = source[index];
    }
}

static void avif_color_matrix_multiply(double output[9],
                                       const double left[9],
                                       const double right[9]) {
    double result[9];
    size_t row;
    size_t column;
    size_t term;

    for (row = 0U; row < 3U; ++row) {
        for (column = 0U; column < 3U; ++column) {
            result[row * 3U + column] = 0.0;
            for (term = 0U; term < 3U; ++term) {
                result[row * 3U + column] +=
                    left[row * 3U + term] *
                    right[term * 3U + column];
            }
        }
    }
    avif_color_matrix_copy(output, result);
}

static int avif_color_matrix_inverse(const double input[9],
                                     double output[9]) {
    double determinant;

    determinant =
        input[0] * (input[4] * input[8] - input[5] * input[7]) -
        input[1] * (input[3] * input[8] - input[5] * input[6]) +
        input[2] * (input[3] * input[7] - input[4] * input[6]);
    if (!avif_color_finite(determinant) ||
        avif_color_abs(determinant) < 1.0e-12) {
        return 0;
    }
    output[0] = (input[4] * input[8] - input[5] * input[7]) / determinant;
    output[1] = (input[2] * input[7] - input[1] * input[8]) / determinant;
    output[2] = (input[1] * input[5] - input[2] * input[4]) / determinant;
    output[3] = (input[5] * input[6] - input[3] * input[8]) / determinant;
    output[4] = (input[0] * input[8] - input[2] * input[6]) / determinant;
    output[5] = (input[2] * input[3] - input[0] * input[5]) / determinant;
    output[6] = (input[3] * input[7] - input[4] * input[6]) / determinant;
    output[7] = (input[1] * input[6] - input[0] * input[7]) / determinant;
    output[8] = (input[0] * input[4] - input[1] * input[3]) / determinant;
    return 1;
}

static void avif_color_matrix_vector(const double matrix[9],
                                     const double input[3],
                                     double output[3]) {
    double result[3];
    size_t row;

    for (row = 0U; row < 3U; ++row) {
        result[row] =
            matrix[row * 3U] * input[0] +
            matrix[row * 3U + 1U] * input[1] +
            matrix[row * 3U + 2U] * input[2];
    }
    output[0] = result[0];
    output[1] = result[1];
    output[2] = result[2];
}

static int avif_color_get_primaries(uint16_t identifier,
                                    AvifColorPrimaries *primaries) {
    if (primaries == 0) {
        return 0;
    }
    avif_color_fill(primaries, 0U, sizeof(*primaries));
    if (identifier == 1U) {
        primaries->red.x = 0.640;
        primaries->red.y = 0.330;
        primaries->green.x = 0.300;
        primaries->green.y = 0.600;
        primaries->blue.x = 0.150;
        primaries->blue.y = 0.060;
        primaries->white.x = 0.3127;
        primaries->white.y = 0.3290;
    } else if (identifier == 4U) {
        primaries->red.x = 0.670;
        primaries->red.y = 0.330;
        primaries->green.x = 0.210;
        primaries->green.y = 0.710;
        primaries->blue.x = 0.140;
        primaries->blue.y = 0.080;
        primaries->white.x = 0.310;
        primaries->white.y = 0.316;
    } else if (identifier == 5U) {
        primaries->red.x = 0.640;
        primaries->red.y = 0.330;
        primaries->green.x = 0.290;
        primaries->green.y = 0.600;
        primaries->blue.x = 0.150;
        primaries->blue.y = 0.060;
        primaries->white.x = 0.3127;
        primaries->white.y = 0.3290;
    } else if (identifier == 6U || identifier == 7U) {
        primaries->red.x = 0.630;
        primaries->red.y = 0.340;
        primaries->green.x = 0.310;
        primaries->green.y = 0.595;
        primaries->blue.x = 0.155;
        primaries->blue.y = 0.070;
        primaries->white.x = 0.3127;
        primaries->white.y = 0.3290;
    } else if (identifier == 8U) {
        primaries->red.x = 0.681;
        primaries->red.y = 0.319;
        primaries->green.x = 0.243;
        primaries->green.y = 0.692;
        primaries->blue.x = 0.145;
        primaries->blue.y = 0.049;
        primaries->white.x = 0.310;
        primaries->white.y = 0.316;
    } else if (identifier == 9U) {
        primaries->red.x = 0.708;
        primaries->red.y = 0.292;
        primaries->green.x = 0.170;
        primaries->green.y = 0.797;
        primaries->blue.x = 0.131;
        primaries->blue.y = 0.046;
        primaries->white.x = 0.3127;
        primaries->white.y = 0.3290;
    } else if (identifier == 10U) {
        primaries->white.x = 1.0 / 3.0;
        primaries->white.y = 1.0 / 3.0;
        primaries->xyz = 1U;
    } else if (identifier == 11U || identifier == 12U) {
        primaries->red.x = 0.680;
        primaries->red.y = 0.320;
        primaries->green.x = 0.265;
        primaries->green.y = 0.690;
        primaries->blue.x = 0.150;
        primaries->blue.y = 0.060;
        if (identifier == 11U) {
            primaries->white.x = 0.314;
            primaries->white.y = 0.351;
        } else {
            primaries->white.x = 0.3127;
            primaries->white.y = 0.3290;
        }
    } else if (identifier == 22U) {
        primaries->red.x = 0.630;
        primaries->red.y = 0.340;
        primaries->green.x = 0.295;
        primaries->green.y = 0.605;
        primaries->blue.x = 0.155;
        primaries->blue.y = 0.077;
        primaries->white.x = 0.3127;
        primaries->white.y = 0.3290;
    } else {
        return 0;
    }
    return 1;
}

static void avif_color_xy_to_xyz(const AvifColorXy *xy,
                                 double xyz[3]) {
    xyz[0] = xy->x / xy->y;
    xyz[1] = 1.0;
    xyz[2] = (1.0 - xy->x - xy->y) / xy->y;
}

static int avif_color_rgb_to_xyz(uint16_t identifier,
                                 double matrix[9],
                                 double white[3]) {
    AvifColorPrimaries primaries;
    double unscaled[9];
    double inverse[9];
    double scales[3];

    if (!avif_color_get_primaries(identifier, &primaries)) {
        return 0;
    }
    if (primaries.xyz != 0U) {
        avif_color_matrix_identity(matrix);
        white[0] = 1.0;
        white[1] = 1.0;
        white[2] = 1.0;
        return 1;
    }
    avif_color_xy_to_xyz(&primaries.red, unscaled);
    avif_color_xy_to_xyz(&primaries.green, unscaled + 3U);
    avif_color_xy_to_xyz(&primaries.blue, unscaled + 6U);
    matrix[0] = unscaled[0];
    matrix[1] = unscaled[3];
    matrix[2] = unscaled[6];
    matrix[3] = unscaled[1];
    matrix[4] = unscaled[4];
    matrix[5] = unscaled[7];
    matrix[6] = unscaled[2];
    matrix[7] = unscaled[5];
    matrix[8] = unscaled[8];
    avif_color_xy_to_xyz(&primaries.white, white);
    if (!avif_color_matrix_inverse(matrix, inverse)) {
        return 0;
    }
    avif_color_matrix_vector(inverse, white, scales);
    matrix[0] *= scales[0];
    matrix[3] *= scales[0];
    matrix[6] *= scales[0];
    matrix[1] *= scales[1];
    matrix[4] *= scales[1];
    matrix[7] *= scales[1];
    matrix[2] *= scales[2];
    matrix[5] *= scales[2];
    matrix[8] *= scales[2];
    return 1;
}

static int avif_color_bradford(const double source_white[3],
                               const double destination_white[3],
                               double matrix[9]) {
    static const double bradford[9] = {
        0.8951, 0.2664, -0.1614,
        -0.7502, 1.7135, 0.0367,
        0.0389, -0.0685, 1.0296
    };
    double inverse[9];
    double source_cone[3];
    double destination_cone[3];
    double scale[9];
    double temporary[9];
    size_t index;

    avif_color_matrix_vector(bradford, source_white, source_cone);
    avif_color_matrix_vector(bradford, destination_white, destination_cone);
    if (!avif_color_matrix_inverse(bradford, inverse)) {
        return 0;
    }
    for (index = 0U; index < 9U; ++index) {
        scale[index] = 0.0;
    }
    for (index = 0U; index < 3U; ++index) {
        if (avif_color_abs(source_cone[index]) < 1.0e-12) {
            return 0;
        }
        scale[index * 3U + index] =
            destination_cone[index] / source_cone[index];
    }
    avif_color_matrix_multiply(temporary, scale, bradford);
    avif_color_matrix_multiply(matrix, inverse, temporary);
    return 1;
}

static int avif_color_conversion_from_xyz(
    const double source_rgb_to_xyz[9],
    const double source_white[3],
    uint16_t destination_primaries,
    AvifdecColorIntent intent,
    double output[9]) {
    double destination_rgb_to_xyz[9];
    double destination_xyz_to_rgb[9];
    double destination_white[3];
    double adaptation[9];
    double temporary[9];

    if (!avif_color_rgb_to_xyz(destination_primaries,
                               destination_rgb_to_xyz,
                               destination_white) ||
        !avif_color_matrix_inverse(destination_rgb_to_xyz,
                                   destination_xyz_to_rgb)) {
        return 0;
    }
    if (intent == AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC) {
        if (!avif_color_bradford(source_white,
                                 destination_white,
                                 adaptation)) {
            return 0;
        }
    } else {
        avif_color_matrix_identity(adaptation);
    }
    avif_color_matrix_multiply(temporary, adaptation, source_rgb_to_xyz);
    avif_color_matrix_multiply(output, destination_xyz_to_rgb, temporary);
    return 1;
}

AvifdecStatus avif_color_primaries_conversion(
    uint16_t source_primaries,
    uint16_t destination_primaries,
    AvifdecColorIntent intent,
    double matrix[9]) {
    double source_rgb_to_xyz[9];
    double source_white[3];

    if (matrix == 0 ||
        intent > AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!avif_color_rgb_to_xyz(source_primaries,
                               source_rgb_to_xyz,
                               source_white) ||
        !avif_color_conversion_from_xyz(source_rgb_to_xyz,
                                        source_white,
                                        destination_primaries,
                                        intent,
                                        matrix)) {
        return AVIFDEC_UNSUPPORTED;
    }
    return AVIFDEC_OK;
}

static double avif_color_transfer_encode_raw(uint16_t identifier,
                                             uint16_t matrix,
                                             double linear) {
    const double alpha = 1.099296826809442;
    const double beta = 0.018053968510807;
    const double smpte240_alpha = 1.111572195921731;
    const double smpte240_beta = 0.022821585529445;
    const double bt1361_gamma = beta / 4.0;
    const double pq_c1 = 0.8359375;
    const double pq_c2 = 18.8515625;
    const double pq_c3 = 18.6875;
    const double pq_m = 78.84375;
    const double pq_n = 0.1593017578125;
    double value;
    double powered;

    if (identifier == 1U || identifier == 6U ||
        identifier == 14U || identifier == 15U) {
        if (linear < 0.0) {
            return 0.0;
        }
        if (linear < beta) {
            return 4.5 * linear;
        }
        return alpha * avif_color_pow(linear, 0.45) - (alpha - 1.0);
    }
    if (identifier == 4U) {
        return linear < 0.0 ? 0.0 : avif_color_pow(linear, 1.0 / 2.2);
    }
    if (identifier == 5U) {
        return linear < 0.0 ? 0.0 : avif_color_pow(linear, 1.0 / 2.8);
    }
    if (identifier == 7U) {
        if (linear < 0.0) {
            return 0.0;
        }
        if (linear < smpte240_beta) {
            return 4.0 * linear;
        }
        return smpte240_alpha * avif_color_pow(linear, 0.45) -
               (smpte240_alpha - 1.0);
    }
    if (identifier == 8U) {
        return linear;
    }
    if (identifier == 9U) {
        if (linear < 0.01) {
            return 0.0;
        }
        return 1.0 + avif_color_log10(linear) / 2.0;
    }
    if (identifier == 10U) {
        if (linear < 0.0031622776601683793) {
            return 0.0;
        }
        return 1.0 + avif_color_log10(linear) / 2.5;
    }
    if (identifier == 11U) {
        if (linear <= -beta) {
            return -alpha * avif_color_pow(-linear, 0.45) +
                   (alpha - 1.0);
        }
        if (linear < beta) {
            return 4.5 * linear;
        }
        return alpha * avif_color_pow(linear, 0.45) - (alpha - 1.0);
    }
    if (identifier == 12U) {
        if (linear <= -bt1361_gamma) {
            return -(alpha * avif_color_pow(-4.0 * linear, 0.45) -
                     (alpha - 1.0)) / 4.0;
        }
        if (linear < beta) {
            return 4.5 * linear;
        }
        return alpha * avif_color_pow(linear, 0.45) - (alpha - 1.0);
    }
    if (identifier == 13U) {
        if (matrix != 0U && linear <= -0.0031308) {
            return -1.055 * avif_color_pow(-linear, 1.0 / 2.4) + 0.055;
        }
        if (linear < 0.0) {
            return matrix == 0U ? 0.0 : 12.92 * linear;
        }
        if (linear < 0.0031308) {
            return 12.92 * linear;
        }
        return 1.055 * avif_color_pow(linear, 1.0 / 2.4) - 0.055;
    }
    if (identifier == 16U) {
        if (linear < 0.0) {
            return 0.0;
        }
        value = avif_color_pow(linear, pq_n);
        powered = (pq_c1 + pq_c2 * value) /
                  (1.0 + pq_c3 * value);
        return avif_color_pow(powered, pq_m);
    }
    if (identifier == 17U) {
        if (linear < 0.0) {
            return 0.0;
        }
        return avif_color_pow(48.0 * linear / 52.37, 1.0 / 2.6);
    }
    if (identifier == 18U) {
        if (linear < 0.0) {
            return 0.0;
        }
        if (linear <= 1.0 / 12.0) {
            return avif_color_pow(3.0 * linear, 0.5);
        }
        return 0.17883277 * avif_color_log(12.0 * linear - 0.28466892) +
               0.55991073;
    }
    return 0.0;
}

static double avif_color_transfer_decode_raw(uint16_t identifier,
                                             uint16_t matrix,
                                             double encoded) {
    const double alpha = 1.099296826809442;
    const double beta = 0.018053968510807;
    const double smpte240_alpha = 1.111572195921731;
    const double smpte240_beta = 0.022821585529445;
    const double bt1361_gamma = beta / 4.0;
    const double pq_c1 = 0.8359375;
    const double pq_c2 = 18.8515625;
    const double pq_c3 = 18.6875;
    const double pq_m = 78.84375;
    const double pq_n = 0.1593017578125;
    double value;
    double numerator;
    double denominator;

    if (identifier == 1U || identifier == 6U ||
        identifier == 14U || identifier == 15U) {
        if (encoded < 0.0) {
            return 0.0;
        }
        if (encoded < 4.5 * beta) {
            return encoded / 4.5;
        }
        return avif_color_pow((encoded + alpha - 1.0) / alpha,
                              1.0 / 0.45);
    }
    if (identifier == 4U) {
        return encoded < 0.0 ? 0.0 : avif_color_pow(encoded, 2.2);
    }
    if (identifier == 5U) {
        return encoded < 0.0 ? 0.0 : avif_color_pow(encoded, 2.8);
    }
    if (identifier == 7U) {
        if (encoded < 0.0) {
            return 0.0;
        }
        if (encoded < 4.0 * smpte240_beta) {
            return encoded / 4.0;
        }
        return avif_color_pow(
            (encoded + smpte240_alpha - 1.0) /
                smpte240_alpha,
            1.0 / 0.45);
    }
    if (identifier == 8U) {
        return encoded;
    }
    if (identifier == 9U) {
        if (encoded <= 0.0) {
            return 0.01;
        }
        return avif_color_exp((2.0 * (encoded - 1.0)) *
                              2.30258509299404568402);
    }
    if (identifier == 10U) {
        if (encoded <= 0.0) {
            return 0.0031622776601683793;
        }
        return avif_color_exp((2.5 * (encoded - 1.0)) *
                              2.30258509299404568402);
    }
    if (identifier == 11U) {
        if (encoded <= -4.5 * beta) {
            return -avif_color_pow((-encoded + alpha - 1.0) / alpha,
                                   1.0 / 0.45);
        }
        if (encoded < 4.5 * beta) {
            return encoded / 4.5;
        }
        return avif_color_pow((encoded + alpha - 1.0) / alpha,
                              1.0 / 0.45);
    }
    if (identifier == 12U) {
        if (encoded <= -4.5 * bt1361_gamma) {
            return -avif_color_pow((-4.0 * encoded + (alpha - 1.0)) /
                                   alpha, 1.0 / 0.45) / 4.0;
        }
        if (encoded < 4.5 * beta) {
            return encoded / 4.5;
        }
        return avif_color_pow((encoded + alpha - 1.0) / alpha,
                              1.0 / 0.45);
    }
    if (identifier == 13U) {
        if (matrix != 0U && encoded <= -0.04045) {
            return -avif_color_pow((-encoded + 0.055) / 1.055, 2.4);
        }
        if (encoded < 0.0) {
            return matrix == 0U ? 0.0 : encoded / 12.92;
        }
        if (encoded < 0.04045) {
            return encoded / 12.92;
        }
        return avif_color_pow((encoded + 0.055) / 1.055, 2.4);
    }
    if (identifier == 16U) {
        if (encoded <= 0.0) {
            return 0.0;
        }
        value = avif_color_pow(encoded, 1.0 / pq_m);
        numerator = avif_color_max(value - pq_c1, 0.0);
        denominator = pq_c2 - pq_c3 * value;
        if (denominator <= 0.0) {
            return 1.0;
        }
        return avif_color_pow(numerator / denominator, 1.0 / pq_n);
    }
    if (identifier == 17U) {
        if (encoded < 0.0) {
            return 0.0;
        }
        return (52.37 / 48.0) * avif_color_pow(encoded, 2.6);
    }
    if (identifier == 18U) {
        if (encoded < 0.0) {
            return 0.0;
        }
        if (encoded <= 0.5) {
            return encoded * encoded / 3.0;
        }
        return (avif_color_exp((encoded - 0.55991073) / 0.17883277) +
                0.28466892) / 12.0;
    }
    return 0.0;
}

static int avif_color_transfer_defined(uint16_t identifier) {
    return identifier == 1U ||
           (identifier >= 4U && identifier <= 18U);
}

AvifdecStatus avif_color_transfer_to_linear(
    uint16_t transfer_characteristics,
    double encoded,
    double *linear) {
    if (linear == 0 || !avif_color_finite(encoded)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!avif_color_transfer_defined(transfer_characteristics)) {
        return AVIFDEC_UNSUPPORTED;
    }
    *linear = avif_color_transfer_decode_raw(
        transfer_characteristics, 0U, encoded);
    return AVIFDEC_OK;
}

AvifdecStatus avif_color_transfer_from_linear(
    uint16_t transfer_characteristics,
    double linear,
    double *encoded) {
    if (encoded == 0 || !avif_color_finite(linear)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!avif_color_transfer_defined(transfer_characteristics)) {
        return AVIFDEC_UNSUPPORTED;
    }
    *encoded = avif_color_transfer_encode_raw(
        transfer_characteristics, 0U, linear);
    return AVIFDEC_OK;
}

static int avif_color_matrix_defined(uint16_t identifier) {
    return identifier == 0U ||
           identifier == 1U ||
           (identifier >= 4U && identifier <= 17U);
}

static int avif_color_matrix_kr_kb(uint16_t matrix_identifier,
                                   uint16_t primaries_identifier,
                                   double *kr,
                                   double *kb) {
    double rgb_to_xyz[9];
    double white[3];

    if (matrix_identifier == 1U) {
        *kr = 0.2126;
        *kb = 0.0722;
    } else if (matrix_identifier == 4U) {
        *kr = 0.30;
        *kb = 0.11;
    } else if (matrix_identifier == 5U ||
               matrix_identifier == 6U) {
        *kr = 0.299;
        *kb = 0.114;
    } else if (matrix_identifier == 7U) {
        *kr = 0.212;
        *kb = 0.087;
    } else if (matrix_identifier == 9U ||
               matrix_identifier == 10U) {
        *kr = 0.2627;
        *kb = 0.0593;
    } else if (matrix_identifier == 12U ||
               matrix_identifier == 13U) {
        if (!avif_color_rgb_to_xyz(primaries_identifier,
                                   rgb_to_xyz,
                                   white)) {
            return 0;
        }
        *kr = rgb_to_xyz[3];
        *kb = rgb_to_xyz[5];
    } else {
        return 0;
    }
    return *kr >= 0.0 && *kb >= 0.0 && *kr + *kb < 1.0;
}

static AvifdecStatus avif_color_cicp_supported(
    const AvifColorAv1Cicp *cicp) {
    uint16_t matrix;
    AvifColorPrimaries primaries;

    if (cicp == 0 ||
        (cicp->bit_depth != 8U &&
         cicp->bit_depth != 10U &&
         cicp->bit_depth != 12U) ||
        cicp->monochrome > 1U ||
        cicp->subsampling_x > 1U ||
        cicp->subsampling_y > 1U ||
        cicp->chroma_sample_position > 2U ||
        cicp->cicp.color_range > 1U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    matrix = cicp->cicp.matrix_coefficients;
    if (!avif_color_matrix_defined(matrix) ||
        !avif_color_transfer_defined(
            cicp->cicp.transfer_characteristics) ||
        !avif_color_get_primaries(cicp->cicp.color_primaries,
                                  &primaries)) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (cicp->chroma_sample_position != 0U &&
        (cicp->subsampling_x != 1U ||
         cicp->subsampling_y != 1U)) {
        return AVIFDEC_INVALID_DATA;
    }
    if (cicp->monochrome == 0U &&
        (matrix == 0U || matrix == 8U ||
         matrix == 16U || matrix == 17U) &&
        (cicp->subsampling_x != 0U ||
         cicp->subsampling_y != 0U)) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (matrix == 16U &&
        cicp->bit_depth < 10U &&
        cicp->cicp.color_range == 0U) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (matrix == 17U &&
        cicp->bit_depth < 9U &&
        cicp->cicp.color_range == 0U) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (matrix == 14U &&
        (cicp->cicp.color_primaries != 9U ||
         (cicp->cicp.transfer_characteristics != 16U &&
          cicp->cicp.transfer_characteristics != 18U))) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (matrix == 15U &&
        (cicp->cicp.color_primaries != 9U ||
         cicp->cicp.transfer_characteristics != 16U)) {
        return AVIFDEC_UNSUPPORTED;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_color_validate_nclx_av1(
    const AvifColorCicp *nclx,
    const AvifColorAv1Cicp *av1,
    AvifdecError *error) {
    AvifColorAv1Cicp merged;
    AvifdecStatus status;

    avif_color_error_clear(error);
    if (nclx == 0 || av1 == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if ((av1->cicp.color_primaries != 2U &&
         nclx->color_primaries != av1->cicp.color_primaries) ||
        (av1->cicp.transfer_characteristics != 2U &&
         nclx->transfer_characteristics !=
             av1->cicp.transfer_characteristics) ||
        (av1->cicp.matrix_coefficients != 2U &&
         nclx->matrix_coefficients !=
             av1->cicp.matrix_coefficients) ||
        nclx->color_range != av1->cicp.color_range) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U,
            AVIF_COLOR_FOURCC('n', 'c', 'l', 'x'));
    }
    merged = *av1;
    if (merged.cicp.color_primaries == 2U) {
        merged.cicp.color_primaries = nclx->color_primaries;
    }
    if (merged.cicp.transfer_characteristics == 2U) {
        merged.cicp.transfer_characteristics =
            nclx->transfer_characteristics;
    }
    if (merged.cicp.matrix_coefficients == 2U) {
        merged.cicp.matrix_coefficients = nclx->matrix_coefficients;
    }
    if (merged.cicp.color_primaries == 2U ||
        merged.cicp.transfer_characteristics == 2U ||
        merged.cicp.matrix_coefficients == 2U) {
        if ((merged.bit_depth != 8U &&
             merged.bit_depth != 10U &&
             merged.bit_depth != 12U) ||
            merged.monochrome > 1U ||
            merged.subsampling_x > 1U ||
            merged.subsampling_y > 1U ||
            merged.chroma_sample_position > 2U ||
            merged.cicp.color_range > 1U ||
            (merged.chroma_sample_position != 0U &&
             (merged.subsampling_x != 1U ||
              merged.subsampling_y != 1U))) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        return AVIFDEC_OK;
    }
    status = avif_color_cicp_supported(&merged);
    if (status != AVIFDEC_OK) {
        if (status == AVIFDEC_INVALID_ARGUMENT) {
            status = AVIFDEC_INVALID_DATA;
        }
        return avif_color_fail(error, status, 0U, 0U);
    }
    return AVIFDEC_OK;
}

void avifdec_color_options_default(AvifdecColorOptions *options) {
    if (options != 0) {
        avif_color_fill(options, 0U, sizeof(*options));
    }
}

AvifdecStatus avifdec_image_color_description(
    const AvifdecImageInfo *info,
    AvifdecColorDescription *description,
    AvifdecError *error) {
    avif_color_error_clear(error);
    if (description != 0) {
        avif_color_fill(description, 0U, sizeof(*description));
    }
    if (info == 0 || description == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    description->color_primaries = info->color_primaries;
    description->transfer_characteristics =
        info->transfer_characteristics;
    description->matrix_coefficients = info->matrix_coefficients;
    description->color_range = info->color_range;
    description->has_nclx = info->has_nclx;
    description->icc.data = info->icc_data;
    description->icc.size = info->icc_size;
    if ((description->icc.data == 0 && description->icc.size != 0U) ||
        description->has_nclx > 1U ||
        description->color_range > 1U) {
        avif_color_fill(description, 0U, sizeof(*description));
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    return AVIFDEC_OK;
}

static size_t avif_color_max_icc_bytes(const AvifdecLimits *limits) {
#if AVIFDEC_VERSION_MAJOR == 1U && AVIFDEC_VERSION_MINOR < 4U
    (void)limits;
    return AVIFDEC_DEFAULT_MAX_ICC_BYTES;
#else
    if (limits == 0 || limits->max_icc_bytes == 0U) {
        return AVIFDEC_DEFAULT_MAX_ICC_BYTES;
    }
    return limits->max_icc_bytes;
#endif
}

static size_t avif_color_max_icc_curve_entries(
    const AvifdecLimits *limits) {
#if AVIFDEC_VERSION_MAJOR == 1U && AVIFDEC_VERSION_MINOR < 4U
    (void)limits;
    return AVIFDEC_DEFAULT_MAX_ICC_CURVE_ENTRIES;
#else
    if (limits == 0 || limits->max_icc_curve_entries == 0U) {
        return AVIFDEC_DEFAULT_MAX_ICC_CURVE_ENTRIES;
    }
    return limits->max_icc_curve_entries;
#endif
}

static double avif_color_icc_fixed(const unsigned char *bytes) {
    return (double)avif_color_s32be(bytes) / 65536.0;
}

static int avif_color_icc_header_class(uint32_t signature,
                                       uint32_t color_space) {
    return signature == AVIF_COLOR_FOURCC('s', 'c', 'n', 'r') ||
           signature == AVIF_COLOR_FOURCC('m', 'n', 't', 'r') ||
           (signature == AVIF_COLOR_FOURCC('p', 'r', 't', 'r') &&
            color_space == AVIF_COLOR_SIG_GRAY);
}

static int avif_color_icc_date_valid(const unsigned char *data) {
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t days;
    int leap;

    year = avif_color_u16be(data + 24U);
    month = avif_color_u16be(data + 26U);
    day = avif_color_u16be(data + 28U);
    if (year < 1900U || month < 1U || month > 12U ||
        avif_color_u16be(data + 30U) > 23U ||
        avif_color_u16be(data + 32U) > 59U ||
        avif_color_u16be(data + 34U) > 59U) {
        return 0;
    }
    if (month == 2U) {
        leap = ((year % 4U) == 0U && (year % 100U) != 0U) ||
               (year % 400U) == 0U;
        days = leap ? 29U : 28U;
    } else if (month == 4U || month == 6U ||
               month == 9U || month == 11U) {
        days = 30U;
    } else {
        days = 31U;
    }
    return day >= 1U && day <= days;
}

static int avif_color_icc_unsupported_tag(uint32_t signature) {
    return signature == AVIF_COLOR_FOURCC('A', '2', 'B', '0') ||
           signature == AVIF_COLOR_FOURCC('A', '2', 'B', '1') ||
           signature == AVIF_COLOR_FOURCC('A', '2', 'B', '2') ||
           signature == AVIF_COLOR_FOURCC('B', '2', 'A', '0') ||
           signature == AVIF_COLOR_FOURCC('B', '2', 'A', '1') ||
           signature == AVIF_COLOR_FOURCC('B', '2', 'A', '2') ||
           signature == AVIF_COLOR_FOURCC('D', '2', 'B', '0') ||
           signature == AVIF_COLOR_FOURCC('D', '2', 'B', '1') ||
           signature == AVIF_COLOR_FOURCC('D', '2', 'B', '2') ||
           signature == AVIF_COLOR_FOURCC('D', '2', 'B', '3') ||
           signature == AVIF_COLOR_FOURCC('B', '2', 'D', '0') ||
           signature == AVIF_COLOR_FOURCC('B', '2', 'D', '1') ||
           signature == AVIF_COLOR_FOURCC('B', '2', 'D', '2') ||
           signature == AVIF_COLOR_FOURCC('B', '2', 'D', '3') ||
           signature == AVIF_COLOR_FOURCC('g', 'a', 'm', 't') ||
           signature == AVIF_COLOR_FOURCC('p', 'r', 'e', '0') ||
           signature == AVIF_COLOR_FOURCC('p', 'r', 'e', '1') ||
           signature == AVIF_COLOR_FOURCC('p', 'r', 'e', '2');
}

static int avif_color_icc_unsupported_type(uint32_t signature) {
    return signature == AVIF_COLOR_FOURCC('m', 'A', 'B', ' ') ||
           signature == AVIF_COLOR_FOURCC('m', 'B', 'A', ' ') ||
           signature == AVIF_COLOR_FOURCC('m', 'f', 't', '1') ||
           signature == AVIF_COLOR_FOURCC('m', 'f', 't', '2') ||
           signature == AVIF_COLOR_FOURCC('c', 'l', 'u', 't');
}

static AvifdecStatus avif_color_icc_validate_table(
    const unsigned char *data,
    size_t size,
    uint8_t version_major,
    uint32_t *tag_count,
    AvifdecError *error) {
    uint32_t count;
    size_t table_bytes;
    size_t table_end;
    uint32_t first;
    uint32_t unsupported_context;
    size_t unsupported_offset;
    int saw_first_data;
    int saw_last_data;

    count = avif_color_u32be(data + 128U);
    unsupported_context = 0U;
    unsupported_offset = 0U;
    saw_first_data = 0;
    saw_last_data = 0;
    if (count > AVIF_COLOR_MAX_ICC_TAGS) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 128U, AVIF_COLOR_SIG_ACSP);
    }
    if (!avif_color_size_multiply((size_t)count, 12U, &table_bytes) ||
        !avif_color_size_add(132U, table_bytes, &table_end)) {
        return avif_color_fail(
            error, AVIFDEC_OVERFLOW, 128U, AVIF_COLOR_SIG_ACSP);
    }
    if (table_end > size) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 128U, AVIF_COLOR_SIG_ACSP);
    }
    for (first = 0U; first < count; ++first) {
        const unsigned char *entry;
        uint32_t signature;
        uint32_t offset32;
        uint32_t length32;
        size_t offset;
        size_t length;
        size_t end;
        uint32_t second;
        uint32_t type;

        entry = data + 132U + (size_t)first * 12U;
        signature = avif_color_u32be(entry);
        offset32 = avif_color_u32be(entry + 4U);
        length32 = avif_color_u32be(entry + 8U);
        offset = (size_t)offset32;
        length = (size_t)length32;
        if (signature == 0U || (offset & 3U) != 0U || length < 8U ||
            offset < table_end ||
            !avif_color_size_add(offset, length, &end) ||
            end > size) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA,
                132U + (size_t)first * 12U, signature);
        }
        if (data[offset + 4U] != 0U ||
            data[offset + 5U] != 0U ||
            data[offset + 6U] != 0U ||
            data[offset + 7U] != 0U) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, signature);
        }
        type = avif_color_u32be(data + offset);
        if (avif_color_icc_unsupported_tag(signature) ||
            avif_color_icc_unsupported_type(type)) {
            if (unsupported_context == 0U) {
                unsupported_context = signature;
                unsupported_offset = offset;
            }
        }
        for (second = 0U; second < first; ++second) {
            const unsigned char *other;
            uint32_t other_signature;
            size_t other_offset;
            size_t other_length;
            size_t other_end;

            other = data + 132U + (size_t)second * 12U;
            other_signature = avif_color_u32be(other);
            other_offset = (size_t)avif_color_u32be(other + 4U);
            other_length = (size_t)avif_color_u32be(other + 8U);
            other_end = other_offset + other_length;
            if (other_signature == signature) {
                return avif_color_fail(
                    error, AVIFDEC_INVALID_DATA,
                    132U + (size_t)first * 12U, signature);
            }
            if ((offset < other_end && other_offset < end) &&
                (offset != other_offset || length != other_length)) {
                return avif_color_fail(
                    error, AVIFDEC_INVALID_DATA, offset, signature);
            }
        }
    }
    if (version_major >= 4U) {
    for (first = 0U; first < count; ++first) {
        const unsigned char *entry;
        size_t offset;
        size_t length;
        size_t end;
        size_t padded_end;
        uint32_t second;
        int alias;
        int predecessor;
        size_t padding;

        entry = data + 132U + (size_t)first * 12U;
        offset = (size_t)avif_color_u32be(entry + 4U);
        length = (size_t)avif_color_u32be(entry + 8U);
        end = offset + length;
        alias = 0;
        for (second = 0U; second < first; ++second) {
            const unsigned char *other;

            other = data + 132U + (size_t)second * 12U;
            if (offset ==
                    (size_t)avif_color_u32be(other + 4U) &&
                length ==
                    (size_t)avif_color_u32be(other + 8U)) {
                alias = 1;
                break;
            }
        }
        if (alias) {
            continue;
        }
        if (!avif_color_size_add(end, 3U, &padded_end)) {
            return avif_color_fail(
                error, AVIFDEC_OVERFLOW, offset,
                avif_color_u32be(entry));
        }
        padded_end &= ~(size_t)3U;
        if (padded_end > size) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset,
                avif_color_u32be(entry));
        }
        for (padding = end; padding < padded_end; ++padding) {
            if (data[padding] != 0U) {
                return avif_color_fail(
                    error, AVIFDEC_INVALID_DATA, padding,
                    avif_color_u32be(entry));
            }
        }
        predecessor = offset == table_end;
        if (predecessor) {
            saw_first_data = 1;
        } else {
            for (second = 0U; second < count; ++second) {
                const unsigned char *other;
                size_t other_offset;
                size_t other_length;
                size_t other_end;
                size_t other_padded_end;

                other = data + 132U + (size_t)second * 12U;
                other_offset =
                    (size_t)avif_color_u32be(other + 4U);
                other_length =
                    (size_t)avif_color_u32be(other + 8U);
                other_end = other_offset + other_length;
                if (!avif_color_size_add(
                        other_end, 3U, &other_padded_end)) {
                    return avif_color_fail(
                        error, AVIFDEC_OVERFLOW, other_offset,
                        avif_color_u32be(other));
                }
                other_padded_end &= ~(size_t)3U;
                if (other_padded_end == offset) {
                    predecessor = 1;
                    break;
                }
            }
        }
        if (!predecessor) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset,
                avif_color_u32be(entry));
        }
        if (padded_end == size) {
            saw_last_data = 1;
        }
    }
    if (count == 0U || !saw_first_data || !saw_last_data) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, table_end,
            AVIF_COLOR_SIG_ACSP);
    }
    }
    if (unsupported_context != 0U) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED,
            unsupported_offset, unsupported_context);
    }
    *tag_count = count;
    return AVIFDEC_OK;
}

static int avif_color_icc_find_tag(const unsigned char *data,
                                   uint32_t tag_count,
                                   uint32_t wanted,
                                   size_t *offset,
                                   size_t *size) {
    uint32_t index;

    for (index = 0U; index < tag_count; ++index) {
        const unsigned char *entry;

        entry = data + 132U + (size_t)index * 12U;
        if (avif_color_u32be(entry) == wanted) {
            *offset = (size_t)avif_color_u32be(entry + 4U);
            *size = (size_t)avif_color_u32be(entry + 8U);
            return 1;
        }
    }
    return 0;
}

static AvifdecStatus avif_color_icc_required_text_tag(
    const unsigned char *data,
    uint32_t tag_count,
    uint32_t tag,
    uint8_t version_major,
    AvifdecError *error) {
    size_t offset;
    size_t size;
    uint32_t type;
    uint32_t expected;

    if (!avif_color_icc_find_tag(
            data, tag_count, tag, &offset, &size)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 128U, tag);
    }
    type = avif_color_u32be(data + offset);
    if (version_major >= 4U) {
        expected = AVIF_COLOR_SIG_MLUC;
    } else {
        expected = tag == AVIF_COLOR_SIG_DESC
            ? AVIF_COLOR_SIG_DESC
            : AVIF_COLOR_SIG_TEXT;
    }
    if (type != expected) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, offset, tag);
    }
    if (type == AVIF_COLOR_SIG_MLUC) {
        uint32_t count;
        uint32_t record_size;
        size_t records_size;
        size_t records_end;
        uint32_t index;

        if (size < 16U) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, tag);
        }
        count = avif_color_u32be(data + offset + 8U);
        record_size = avif_color_u32be(data + offset + 12U);
        if (count == 0U || record_size != 12U ||
            !avif_color_size_multiply(
                (size_t)count, 12U, &records_size) ||
            !avif_color_size_add(
                16U, records_size, &records_end) ||
            records_end > size) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, tag);
        }
        for (index = 0U; index < count; ++index) {
            const unsigned char *record;
            uint32_t length;
            uint32_t string_offset;
            size_t string_end;

            record = data + offset + 16U + (size_t)index * 12U;
            length = avif_color_u32be(record + 4U);
            string_offset = avif_color_u32be(record + 8U);
            if ((length & 1U) != 0U ||
                (size_t)string_offset < records_end ||
                !avif_color_size_add(
                    (size_t)string_offset,
                    (size_t)length, &string_end) ||
                string_end > size) {
                return avif_color_fail(
                    error, AVIFDEC_INVALID_DATA, offset, tag);
            }
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_color_icc_xyz(
    const unsigned char *data,
    size_t offset,
    size_t size,
    uint32_t tag,
    double xyz[3],
    AvifdecError *error) {
    if (size != 20U ||
        avif_color_u32be(data + offset) != AVIF_COLOR_SIG_XYZ) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, offset, tag);
    }
    xyz[0] = avif_color_icc_fixed(data + offset + 8U);
    xyz[1] = avif_color_icc_fixed(data + offset + 12U);
    xyz[2] = avif_color_icc_fixed(data + offset + 16U);
    if (!avif_color_finite(xyz[0]) ||
        !avif_color_finite(xyz[1]) ||
        !avif_color_finite(xyz[2]) ||
        avif_color_abs(xyz[0]) > 16.0 ||
        avif_color_abs(xyz[1]) > 16.0 ||
        avif_color_abs(xyz[2]) > 16.0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, offset, tag);
    }
    return AVIFDEC_OK;
}

static uint8_t avif_color_icc_param_count(uint16_t function_type) {
    if (function_type == 0U) {
        return 1U;
    }
    if (function_type == 1U) {
        return 3U;
    }
    if (function_type == 2U) {
        return 4U;
    }
    if (function_type == 3U) {
        return 5U;
    }
    if (function_type == 4U) {
        return 7U;
    }
    return 0U;
}

static AvifdecStatus avif_color_icc_curve(
    const unsigned char *data,
    size_t offset,
    size_t size,
    uint32_t tag,
    size_t max_entries,
    uint8_t version_major,
    AvifColorCurve *curve,
    AvifdecError *error) {
    uint32_t type;

    avif_color_fill(curve, 0U, sizeof(*curve));
    type = avif_color_u32be(data + offset);
    if (type == AVIF_COLOR_SIG_CURV) {
        uint32_t count;
        size_t sample_bytes;
        size_t required;

        if (size < 12U) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, tag);
        }
        count = avif_color_u32be(data + offset + 8U);
        if ((size_t)count > max_entries) {
            return avif_color_fail(
                error, AVIFDEC_LIMIT_EXCEEDED, offset, tag);
        }
        if (!avif_color_size_multiply((size_t)count, 2U,
                                      &sample_bytes) ||
            !avif_color_size_add(12U, sample_bytes, &required)) {
            return avif_color_fail(
                error, AVIFDEC_OVERFLOW, offset, tag);
        }
        if (required != size) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, tag);
        }
        if (count == 0U) {
            curve->kind = AVIF_COLOR_CURVE_IDENTITY;
        } else if (count == 1U) {
            uint16_t gamma;

            gamma = avif_color_u16be(data + offset + 12U);
            if (gamma == 0U) {
                return avif_color_fail(
                    error, AVIFDEC_INVALID_DATA, offset, tag);
            }
            curve->kind = AVIF_COLOR_CURVE_GAMMA;
            curve->parameters[0] = (float)gamma / 256.0f;
        } else {
            curve->kind = AVIF_COLOR_CURVE_TABLE;
            curve->samples = data + offset + 12U;
            curve->count = count;
        }
        return AVIFDEC_OK;
    }
    if (type == AVIF_COLOR_SIG_PARA) {
        uint16_t function_type;
        uint8_t parameter_count;
        size_t parameter_bytes;
        size_t required;
        uint8_t index;
        double gamma;
        double a;
        double b;
        double d;

        if (version_major < 4U) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, tag);
        }
        if (size < 12U ||
            data[offset + 10U] != 0U ||
            data[offset + 11U] != 0U) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, tag);
        }
        function_type = avif_color_u16be(data + offset + 8U);
        parameter_count = avif_color_icc_param_count(function_type);
        if (parameter_count == 0U ||
            !avif_color_size_multiply(
                (size_t)parameter_count, 4U, &parameter_bytes) ||
            !avif_color_size_add(12U, parameter_bytes, &required) ||
            required != size) {
            return avif_color_fail(
                error, function_type > 4U
                    ? AVIFDEC_UNSUPPORTED
                    : AVIFDEC_INVALID_DATA,
                offset, tag);
        }
        for (index = 0U; index < parameter_count; ++index) {
            curve->parameters[index] = (float)avif_color_icc_fixed(
                data + offset + 12U + (size_t)index * 4U);
        }
        gamma = curve->parameters[0];
        if (!(gamma > 0.0)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset, tag);
        }
        if (function_type != 0U) {
            a = curve->parameters[1];
            b = curve->parameters[2];
            if (!(a > 0.0)) {
                return avif_color_fail(
                    error, AVIFDEC_INVALID_DATA, offset, tag);
            }
        }
        if (function_type >= 3U) {
            d = curve->parameters[4];
            if (d < -b / a) {
                return avif_color_fail(
                    error, AVIFDEC_INVALID_DATA, offset, tag);
            }
        }
        curve->kind = AVIF_COLOR_CURVE_PARAMETRIC;
        curve->function_type = (uint8_t)function_type;
        curve->parameter_count = parameter_count;
        return AVIFDEC_OK;
    }
    return avif_color_fail(
        error, AVIFDEC_UNSUPPORTED, offset, tag);
}

static AvifdecStatus avif_color_icc_parse(
    AvifdecByteView profile,
    const AvifdecLimits *limits,
    AvifColorIcc *icc,
    AvifdecError *error) {
    const unsigned char *data;
    size_t size;
    uint32_t declared_size;
    uint32_t profile_class;
    uint32_t color_space;
    uint32_t pcs;
    uint32_t tag_count;
    size_t offset;
    size_t tag_size;
    size_t max_entries;
    double header_white[3];
    double colorant[3];
    double inverse_matrix[9];
    AvifdecStatus status;
    size_t index;
    uint8_t version_major;

    avif_color_fill(icc, 0U, sizeof(*icc));
    data = profile.data;
    size = profile.size;
    if (data == 0 && size != 0U) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (size > avif_color_max_icc_bytes(limits)) {
        return avif_color_fail(
            error, AVIFDEC_LIMIT_EXCEEDED, 0U, AVIF_COLOR_SIG_ACSP);
    }
    if (size < 128U) {
        return avif_color_fail(
            error, AVIFDEC_TRUNCATED, size, AVIF_COLOR_SIG_ACSP);
    }
    declared_size = avif_color_u32be(data);
    if ((size_t)declared_size > size) {
        return avif_color_fail(
            error, AVIFDEC_TRUNCATED, size, AVIF_COLOR_SIG_ACSP);
    }
    if ((size_t)declared_size != size || declared_size < 132U ||
        (declared_size & 3U) != 0U) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U, AVIF_COLOR_SIG_ACSP);
    }
    version_major = data[8];
    if ((version_major != 2U && version_major != 4U) ||
        (data[9] & 0x0fU) > 9U ||
        (data[9] >> 4) > 9U ||
        data[10] != 0U ||
        data[11] != 0U) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 8U, AVIF_COLOR_SIG_ACSP);
    }
    profile_class = avif_color_u32be(data + 12U);
    color_space = avif_color_u32be(data + 16U);
    pcs = avif_color_u32be(data + 20U);
    if (!avif_color_icc_header_class(profile_class, color_space)) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 12U, profile_class);
    }
    if (color_space != AVIF_COLOR_SIG_RGB &&
        color_space != AVIF_COLOR_SIG_GRAY) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 16U, color_space);
    }
    if (pcs != AVIF_COLOR_SIG_XYZ) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 20U, pcs);
    }
    if (    !avif_color_icc_date_valid(data) ||
    avif_color_u32be(data + 36U) != AVIF_COLOR_SIG_ACSP ||
    (avif_color_u32be(data + 44U) & 0x0000fffcU) != 0U ||
    (avif_color_u32be(data + 60U) & ~15U) != 0U ||
        avif_color_u32be(data + 64U) > 3U) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 36U, AVIF_COLOR_SIG_ACSP);
    }
    for (index = 100U; index < 128U; ++index) {
        if (data[index] != 0U) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, index,
                AVIF_COLOR_SIG_ACSP);
        }
    }
    header_white[0] = avif_color_icc_fixed(data + 68U);
    header_white[1] = avif_color_icc_fixed(data + 72U);
    header_white[2] = avif_color_icc_fixed(data + 76U);
    if (avif_color_abs(header_white[0] - 0.9642) > 0.00005 ||
        avif_color_abs(header_white[1] - 1.0) > 0.00005 ||
        avif_color_abs(header_white[2] - 0.8249) > 0.00005) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 68U, AVIF_COLOR_SIG_ACSP);
    }
    status = avif_color_icc_validate_table(
        data, size, version_major, &tag_count, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_color_icc_required_text_tag(
        data, tag_count, AVIF_COLOR_SIG_DESC,
        version_major, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_color_icc_required_text_tag(
        data, tag_count,
        AVIF_COLOR_FOURCC('c', 'p', 'r', 't'),
        version_major, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    if (!avif_color_icc_find_tag(
            data, tag_count,
            AVIF_COLOR_FOURCC('w', 't', 'p', 't'),
            &offset, &tag_size)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 128U,
            AVIF_COLOR_FOURCC('w', 't', 'p', 't'));
    }
    status = avif_color_icc_xyz(
        data, offset, tag_size,
        AVIF_COLOR_FOURCC('w', 't', 'p', 't'),
        icc->media_white, error);
    if (status != AVIFDEC_OK ||
        icc->media_white[0] < 0.0 ||
        icc->media_white[1] <= 0.0 ||
        icc->media_white[2] < 0.0) {
        return status != AVIFDEC_OK
            ? status
            : avif_color_fail(
                error, AVIFDEC_INVALID_DATA, offset,
                AVIF_COLOR_FOURCC('w', 't', 'p', 't'));
    }
    max_entries = avif_color_max_icc_curve_entries(limits);
    if (color_space == AVIF_COLOR_SIG_GRAY) {
        if (!avif_color_icc_find_tag(
                data, tag_count,
                AVIF_COLOR_FOURCC('k', 'T', 'R', 'C'),
                &offset, &tag_size)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 128U,
                AVIF_COLOR_FOURCC('k', 'T', 'R', 'C'));
        }
        status = avif_color_icc_curve(
            data, offset, tag_size,
            AVIF_COLOR_FOURCC('k', 'T', 'R', 'C'),
            max_entries, version_major, &icc->curves[0], error);
        if (status != AVIFDEC_OK) {
            return status;
        }
        icc->rgb_to_xyz[0] = header_white[0];
        icc->rgb_to_xyz[3] = header_white[1];
        icc->rgb_to_xyz[6] = header_white[2];
        icc->curve_count = 1U;
        icc->gray = 1U;
        return AVIFDEC_OK;
    }
    for (index = 0U; index < 3U; ++index) {
        static const uint32_t xyz_tags[3] = {
            AVIF_COLOR_FOURCC('r', 'X', 'Y', 'Z'),
            AVIF_COLOR_FOURCC('g', 'X', 'Y', 'Z'),
            AVIF_COLOR_FOURCC('b', 'X', 'Y', 'Z')
        };
        static const uint32_t trc_tags[3] = {
            AVIF_COLOR_FOURCC('r', 'T', 'R', 'C'),
            AVIF_COLOR_FOURCC('g', 'T', 'R', 'C'),
            AVIF_COLOR_FOURCC('b', 'T', 'R', 'C')
        };

        if (!avif_color_icc_find_tag(
                data, tag_count, xyz_tags[index],
                &offset, &tag_size)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 128U, xyz_tags[index]);
        }
        status = avif_color_icc_xyz(
            data, offset, tag_size, xyz_tags[index],
            colorant, error);
        if (status != AVIFDEC_OK) {
            return status;
        }
        icc->rgb_to_xyz[index] = colorant[0];
        icc->rgb_to_xyz[3U + index] = colorant[1];
        icc->rgb_to_xyz[6U + index] = colorant[2];
        if (!avif_color_icc_find_tag(
                data, tag_count, trc_tags[index],
                &offset, &tag_size)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 128U, trc_tags[index]);
        }
        status = avif_color_icc_curve(
            data, offset, tag_size, trc_tags[index],
            max_entries, version_major, &icc->curves[index], error);
        if (status != AVIFDEC_OK) {
            return status;
        }
    }
    if (!avif_color_matrix_inverse(
            icc->rgb_to_xyz, inverse_matrix)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 128U, AVIF_COLOR_SIG_RGB);
    }
    icc->curve_count = 3U;
    return AVIFDEC_OK;
}

static double avif_color_curve_evaluate(const AvifColorCurve *curve,
                                        double input) {
    double x;
    double result;

    x = avif_color_clamp(input, 0.0, 1.0);
    if (curve->kind == AVIF_COLOR_CURVE_IDENTITY) {
        return x;
    }
    if (curve->kind == AVIF_COLOR_CURVE_GAMMA) {
        result = avif_color_pow(x, curve->parameters[0]);
        return avif_color_clamp(result, 0.0, 1.0);
    }
    if (curve->kind == AVIF_COLOR_CURVE_TABLE) {
        double position;
        uint32_t index;
        double fraction;
        double first;
        double second;

        position = x * (double)(curve->count - 1U);
        index = (uint32_t)position;
        if (index >= curve->count - 1U) {
            return (double)avif_color_u16be(
                curve->samples + (size_t)(curve->count - 1U) * 2U) /
                65535.0;
        }
        fraction = position - (double)index;
        first = (double)avif_color_u16be(
            curve->samples + (size_t)index * 2U) / 65535.0;
        second = (double)avif_color_u16be(
            curve->samples + (size_t)(index + 1U) * 2U) / 65535.0;
        return first + (second - first) * fraction;
    }
    if (curve->kind == AVIF_COLOR_CURVE_PARAMETRIC) {
        double g;
        double a;
        double b;
        double c;
        double d;
        double e;
        double f;
        double threshold;

        g = curve->parameters[0];
        a = curve->parameters[1];
        b = curve->parameters[2];
        c = curve->parameters[3];
        d = curve->parameters[4];
        e = curve->parameters[5];
        f = curve->parameters[6];
        if (curve->function_type == 0U) {
            result = avif_color_pow(x, g);
            return avif_color_clamp(result, 0.0, 1.0);
        }
        if (curve->function_type == 1U ||
            curve->function_type == 2U) {
            threshold = -b / a;
            if (x < threshold) {
                result = curve->function_type == 1U ? 0.0 : c;
            } else {
                result = avif_color_pow(a * x + b, g) +
                    (curve->function_type == 2U ? c : 0.0);
            }
        } else if (x < d) {
            result = c * x +
                (curve->function_type == 4U ? f : 0.0);
        } else {
            result = avif_color_pow(a * x + b, g) +
                (curve->function_type == 4U ? e : 0.0);
        }
        return avif_color_clamp(result, 0.0, 1.0);
    }
    return 0.0;
}

static int avif_color_description_matrix_supported(
    const AvifdecColorDescription *source,
    int require_all_cicp) {
    uint16_t matrix;
    double kr;
    double kb;
    AvifColorPrimaries primaries;

    matrix = source->matrix_coefficients;
    if (!avif_color_matrix_defined(matrix)) {
        return 0;
    }
    if (matrix == 12U || matrix == 13U) {
        if (!avif_color_matrix_kr_kb(
                matrix, source->color_primaries, &kr, &kb)) {
            return 0;
        }
    }
    if (matrix == 10U || matrix == 13U) {
        if (!avif_color_transfer_defined(
                source->transfer_characteristics)) {
            return 0;
        }
    }
    if (matrix == 14U &&
        (source->color_primaries != 9U ||
         (source->transfer_characteristics != 16U &&
          source->transfer_characteristics != 18U))) {
        return 0;
    }
    if (matrix == 15U &&
        (source->color_primaries != 9U ||
         source->transfer_characteristics != 16U)) {
        return 0;
    }
    if (require_all_cicp &&
        (!avif_color_transfer_defined(
             source->transfer_characteristics) ||
         !avif_color_get_primaries(
             source->color_primaries, &primaries))) {
        return 0;
    }
    return 1;
}

static void avif_color_matrix_to_float(float output[9],
                                       const double input[9]) {
    size_t index;

    for (index = 0U; index < 9U; ++index) {
        output[index] = (float)input[index];
    }
}

static int avif_color_prepare_special(AvifColorInternal *internal) {
    static const double ictcp_to_lms_source[9] = {
        0.5, 0.5, 0.0,
        6610.0 / 4096.0, -13613.0 / 4096.0, 7003.0 / 4096.0,
        17933.0 / 4096.0, -17390.0 / 4096.0, -543.0 / 4096.0
    };
    static const double ictcp_hlg_to_lms_source[9] = {
        0.5, 0.5, 0.0,
        3625.0 / 4096.0, -7465.0 / 4096.0, 3840.0 / 4096.0,
        9500.0 / 4096.0, -9212.0 / 4096.0, -288.0 / 4096.0
    };
    static const double ictcp_rgb_to_lms[9] = {
        1688.0 / 4096.0, 2146.0 / 4096.0, 262.0 / 4096.0,
        683.0 / 4096.0, 2951.0 / 4096.0, 462.0 / 4096.0,
        99.0 / 4096.0, 309.0 / 4096.0, 3688.0 / 4096.0
    };
    static const double ipt_to_lms_source[9] = {
        1638.0 / 4096.0, 1638.0 / 4096.0, 820.0 / 4096.0,
        18248.0 / 4096.0, -19870.0 / 4096.0, 1622.0 / 4096.0,
        3300.0 / 4096.0, 1463.0 / 4096.0, -4763.0 / 4096.0
    };
    static const double ipt_rgb_to_lms[9] = {
        1747.0 / 4096.0, 2169.0 / 4096.0, 180.0 / 4096.0,
        673.0 / 4096.0, 3029.0 / 4096.0, 394.0 / 4096.0,
        50.0 / 4096.0, 207.0 / 4096.0, 3839.0 / 4096.0
    };
    const double *source_matrix;
    const double *rgb_to_lms;
    double inverse_source[9];
    double inverse_lms[9];

    if (internal->source.matrix_coefficients == 14U) {
        source_matrix =
            internal->source.transfer_characteristics == 18U
                ? ictcp_hlg_to_lms_source
                : ictcp_to_lms_source;
        rgb_to_lms = ictcp_rgb_to_lms;
    } else if (internal->source.matrix_coefficients == 15U) {
        source_matrix = ipt_to_lms_source;
        rgb_to_lms = ipt_rgb_to_lms;
    } else {
        return 1;
    }
    if (!avif_color_matrix_inverse(source_matrix, inverse_source) ||
        !avif_color_matrix_inverse(rgb_to_lms, inverse_lms)) {
        return 0;
    }
    avif_color_matrix_to_float(
        internal->special_inverse, inverse_source);
    avif_color_matrix_to_float(
        internal->special_to_rgb, inverse_lms);
    internal->special_linear = 1U;
    return 1;
}

static AvifdecStatus avif_color_prepare(
    const AvifdecColorDescription *source,
    const AvifdecColorOptions *options,
    const AvifdecLimits *limits,
    AvifColorInternal *internal,
    AvifdecColorTransformInfo *info,
    AvifdecError *error) {
    AvifdecColorOptions normalized;
    AvifColorIcc icc;
    double source_rgb_to_xyz[9];
    double source_white[3];
    double conversion[9];
    double connection[9];
    double connection_inverse[9];
    uint16_t destination_primaries;
    uint16_t destination_transfer;
    int use_icc;
    int hdr;
    AvifdecStatus status;
    size_t index;
    AvifColorPrimaries destination_description;

    avif_color_fill(internal, 0U, sizeof(*internal));
    avif_color_fill(&normalized, 0U, sizeof(normalized));
    if (options != 0) {
        avif_color_copy(&normalized, options, sizeof(normalized));
    }
    if (source == 0 ||
        (source->icc.data == 0 && source->icc.size != 0U) ||
        source->has_nclx > 1U ||
        source->color_range > 1U ||
        normalized.source < AVIFDEC_COLOR_SOURCE_AUTO ||
        normalized.source > AVIFDEC_COLOR_SOURCE_ICC ||
        normalized.intent <
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC ||
        normalized.intent >
            AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC ||
        normalized.chroma_upsampling <
            AVIFDEC_CHROMA_UPSAMPLING_BILINEAR ||
        normalized.chroma_upsampling >
            AVIFDEC_CHROMA_UPSAMPLING_NEAREST ||
        normalized.hdr_policy < AVIFDEC_COLOR_HDR_REJECT ||
        normalized.hdr_policy > AVIFDEC_COLOR_HDR_CLIP_TO_DISPLAY ||
        !avif_color_finite(normalized.reference_white_nits) ||
        !avif_color_finite(normalized.display_peak_nits) ||
        normalized.reference_white_nits < 0.0f ||
        normalized.display_peak_nits < 0.0f) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    use_icc = normalized.source == AVIFDEC_COLOR_SOURCE_ICC ||
              (normalized.source == AVIFDEC_COLOR_SOURCE_AUTO &&
               source->icc.data != 0);
    if (normalized.source == AVIFDEC_COLOR_SOURCE_ICC &&
        source->icc.data == 0) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 0U, AVIF_COLOR_SIG_ACSP);
    }
    if (!avif_color_description_matrix_supported(source, !use_icc)) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 0U, 0U);
    }
    destination_primaries =
        normalized.destination_color_primaries != 0U
            ? normalized.destination_color_primaries
            : source->color_primaries;
    destination_transfer =
        normalized.destination_transfer_characteristics != 0U
            ? normalized.destination_transfer_characteristics
            : source->transfer_characteristics;
    if (!avif_color_get_primaries(
            destination_primaries, &destination_description) ||
        !avif_color_transfer_defined(destination_transfer)) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 0U, 0U);
    }
    hdr = source->transfer_characteristics == 16U ||
          source->transfer_characteristics == 18U ||
          destination_transfer == 16U ||
          destination_transfer == 18U;
    if (hdr) {
        if (normalized.hdr_policy == AVIFDEC_COLOR_HDR_REJECT) {
            return avif_color_fail(
                error, AVIFDEC_UNSUPPORTED, 0U, 0U);
        }
        if (!(normalized.reference_white_nits > 0.0f) ||
            !(normalized.display_peak_nits > 0.0f) ||
            normalized.display_peak_nits <
                normalized.reference_white_nits) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
        }
    } else {
        if (normalized.reference_white_nits == 0.0f) {
            normalized.reference_white_nits = 203.0f;
        }
        if (normalized.display_peak_nits == 0.0f) {
            normalized.display_peak_nits =
                normalized.reference_white_nits;
        }
    }
    if (use_icc) {
        status = avif_color_icc_parse(
            source->icc, limits, &icc, error);
        if (status != AVIFDEC_OK) {
            return status;
        }
        avif_color_matrix_copy(source_rgb_to_xyz, icc.rgb_to_xyz);
        source_white[0] = 0.9642;
        source_white[1] = 1.0;
        source_white[2] = 0.8249;
        if (normalized.intent ==
            AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC) {
            double scale[9];
            double scaled[9];

            avif_color_matrix_identity(scale);
            scale[0] = icc.media_white[0] / source_white[0];
            scale[4] = icc.media_white[1] / source_white[1];
            scale[8] = icc.media_white[2] / source_white[2];
            avif_color_matrix_multiply(
                scaled, scale, source_rgb_to_xyz);
            avif_color_matrix_copy(source_rgb_to_xyz, scaled);
        }
        avif_color_matrix_copy(connection, source_rgb_to_xyz);
        if (!avif_color_conversion_from_xyz(
                source_rgb_to_xyz, source_white,
                destination_primaries, normalized.intent,
                conversion)) {
            return avif_color_fail(
                error, AVIFDEC_UNSUPPORTED, 0U, 0U);
        }
        for (index = 0U; index < 3U; ++index) {
            avif_color_copy(
                &internal->curves[index],
                &icc.curves[index],
                sizeof(icc.curves[index]));
        }
        internal->curve_count = icc.curve_count;
        internal->source_is_gray = icc.gray;
        internal->source_is_icc = 1U;
        internal->flags |= AVIFDEC_COLOR_TRANSFORM_SOURCE_ICC;
        if (icc.gray != 0U) {
            internal->flags |= AVIFDEC_COLOR_TRANSFORM_SOURCE_GRAY;
        }
    } else {
        double d50[3];
        double adaptation[9];

        if (!avif_color_rgb_to_xyz(
                source->color_primaries,
                source_rgb_to_xyz, source_white)) {
            return avif_color_fail(
                error, AVIFDEC_UNSUPPORTED, 0U, 0U);
        }
        if (normalized.intent ==
            AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC) {
            d50[0] = 0.9642;
            d50[1] = 1.0;
            d50[2] = 0.8249;
            if (!avif_color_bradford(
                    source_white, d50, adaptation)) {
                return avif_color_fail(
                    error, AVIFDEC_UNSUPPORTED, 0U, 0U);
            }
            avif_color_matrix_multiply(
                connection, adaptation, source_rgb_to_xyz);
        } else {
            avif_color_matrix_copy(
                connection, source_rgb_to_xyz);
        }
        status = avif_color_primaries_conversion(
            source->color_primaries,
            destination_primaries,
            normalized.intent,
            conversion);
        if (status != AVIFDEC_OK) {
            return avif_color_fail(error, status, 0U, 0U);
        }
        internal->flags |= AVIFDEC_COLOR_TRANSFORM_SOURCE_CICP;
    }
    avif_color_matrix_to_float(
        internal->connection_matrix, connection);
    if (avif_color_matrix_inverse(
            connection, connection_inverse)) {
        avif_color_matrix_to_float(
            internal->connection_inverse,
            connection_inverse);
        internal->connection_invertible = 1U;
    } else if (internal->source_is_gray == 0U) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    if (hdr) {
        internal->flags |= AVIFDEC_COLOR_TRANSFORM_HDR;
    }
    if (normalized.intent ==
        AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC) {
        internal->flags |= AVIFDEC_COLOR_TRANSFORM_ABSOLUTE;
    }
    avif_color_matrix_to_float(internal->color_matrix, conversion);
    avif_color_copy(&internal->source, source, sizeof(*source));
    internal->destination.color_primaries = destination_primaries;
    internal->destination.transfer_characteristics =
        destination_transfer;
    internal->destination.matrix_coefficients = 0U;
    internal->destination.color_range = 1U;
    internal->destination.has_nclx = 1U;
    internal->options = normalized;
    internal->max_icc_bytes = avif_color_max_icc_bytes(limits);
    internal->max_icc_curve_entries =
        avif_color_max_icc_curve_entries(limits);
    if (!avif_color_prepare_special(internal)) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 0U, 0U);
    }
    if (info != 0) {
        info->workspace_required = 0U;
        avif_color_copy(&info->source,
                        &internal->source,
                        sizeof(info->source));
        avif_color_copy(&info->destination,
                        &internal->destination,
                        sizeof(info->destination));
        info->flags = internal->flags;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_color_transform_query(
    const AvifdecColorDescription *source,
    const AvifdecColorOptions *options,
    const AvifdecLimits *limits,
    AvifdecColorTransformInfo *info,
    AvifdecError *error) {
    AvifColorInternal internal;

    avif_color_error_clear(error);
    if (info != 0) {
        avif_color_fill(info, 0U, sizeof(*info));
    }
    if (source == 0 || info == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return avif_color_prepare(
        source, options, limits, &internal, info, error);
}

AvifdecStatus avifdec_color_transform_init(
    const AvifdecColorDescription *source,
    const AvifdecColorOptions *options,
    const AvifdecLimits *limits,
    void *workspace,
    size_t workspace_size,
    AvifdecColorTransform *transform,
    AvifdecError *error) {
    AvifColorInternal internal;
    AvifColorHandle handle;
    AvifdecStatus status;

    avif_color_error_clear(error);
    if (transform != 0) {
        avif_color_fill(transform, 0U, sizeof(*transform));
    }
    if (source == 0 || transform == 0 ||
        (workspace == 0 && workspace_size != 0U)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_prepare(
        source, options, limits, &internal, 0, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    avif_color_fill(&handle, 0U, sizeof(handle));
    handle.magic = AVIF_COLOR_MAGIC;
    handle.version = AVIF_COLOR_INTERNAL_VERSION;
    handle.flags = internal.flags;
    handle.source = internal.source;
    handle.destination = internal.destination;
    handle.options = internal.options;
    handle.workspace = workspace;
    handle.workspace_size = workspace_size;
    handle.max_icc_bytes = avif_color_max_icc_bytes(limits);
    handle.max_icc_curve_entries =
        avif_color_max_icc_curve_entries(limits);
    handle.checksum = 0U;
    avif_color_copy(transform, &handle, sizeof(handle));
    handle.checksum = avif_color_transform_checksum(transform);
    avif_color_copy(transform, &handle, sizeof(handle));
    return AVIFDEC_OK;
}

static uint32_t avif_color_max_sample(uint8_t bit_depth) {
    return ((uint32_t)1U << bit_depth) - 1U;
}

static uint32_t avif_color_subsampled_dimension(uint32_t dimension,
                                                uint8_t subsampling) {
    return ((dimension - 1U) >> subsampling) + 1U;
}

static int avif_color_plane_valid(const uint16_t *plane,
                                  size_t stride,
                                  uint32_t width,
                                  uint32_t height) {
    size_t row_offset;
    size_t last_sample;

    if (plane == 0 || width == 0U || height == 0U ||
        stride < (size_t)width) {
        return 0;
    }
    if (!avif_color_size_multiply(
            (size_t)height - 1U, stride, &row_offset) ||
        !avif_color_size_add(
            row_offset, (size_t)width - 1U, &last_sample) ||
        last_sample > SIZE_MAX / sizeof(uint16_t)) {
        return 0;
    }
    return 1;
}

static void avif_color_inverse_map(const AvifdecImageInfo *info,
                                   uint32_t output_x,
                                   uint32_t output_y,
                                   uint32_t *source_x,
                                   uint32_t *source_y) {
    uint32_t crop_width;
    uint32_t crop_height;
    uint8_t angle;
    uint32_t rotated_width;
    uint32_t rotated_height;
    uint32_t rotated_x;
    uint32_t rotated_y;
    uint32_t crop_x;
    uint32_t crop_y;

    crop_width = info->crop.width;
    crop_height = info->crop.height;
    angle = (info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U
        ? info->irot_angle
        : 0U;
    rotated_width = (angle & 1U) != 0U
        ? crop_height
        : crop_width;
    rotated_height = (angle & 1U) != 0U
        ? crop_width
        : crop_height;
    rotated_x = output_x;
    rotated_y = output_y;
    if ((info->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U) {
        if (info->imir_axis == 0U) {
            rotated_y = rotated_height - 1U - output_y;
        } else {
            rotated_x = rotated_width - 1U - output_x;
        }
    }
    if (angle == 1U) {
        crop_x = crop_width - 1U - rotated_y;
        crop_y = rotated_x;
    } else if (angle == 2U) {
        crop_x = crop_width - 1U - rotated_x;
        crop_y = crop_height - 1U - rotated_y;
    } else if (angle == 3U) {
        crop_x = rotated_y;
        crop_y = crop_height - 1U - rotated_x;
    } else {
        crop_x = rotated_x;
        crop_y = rotated_y;
    }
    *source_x = info->crop.x + crop_x;
    *source_y = info->crop.y + crop_y;
}

static AvifdecStatus avif_color_validate_runtime_cicp(
    const AvifdecColorDescription *source,
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    int source_is_icc) {
    AvifColorAv1Cicp cicp;
    AvifdecStatus status;
    uint16_t matrix;

    avif_color_fill(&cicp, 0U, sizeof(cicp));
    cicp.cicp.color_primaries = source->color_primaries;
    cicp.cicp.transfer_characteristics =
        source->transfer_characteristics;
    cicp.cicp.matrix_coefficients = source->matrix_coefficients;
    cicp.cicp.color_range = source->color_range;
    cicp.bit_depth = image->bit_depth;
    cicp.monochrome = image->monochrome;
    cicp.subsampling_x = image->subsampling_x;
    cicp.subsampling_y = image->subsampling_y;
    cicp.chroma_sample_position = info->chroma_sample_position;
    if (!source_is_icc) {
        return avif_color_cicp_supported(&cicp);
    }
    matrix = source->matrix_coefficients;
    if (!avif_color_matrix_defined(matrix) ||
        source->color_range > 1U ||
        (image->bit_depth != 8U &&
         image->bit_depth != 10U &&
         image->bit_depth != 12U) ||
        image->subsampling_x > 1U ||
        image->subsampling_y > 1U ||
        info->chroma_sample_position > 2U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (info->chroma_sample_position != 0U &&
        (image->subsampling_x != 1U ||
         image->subsampling_y != 1U)) {
        return AVIFDEC_INVALID_DATA;
    }
    if ((matrix == 0U || matrix == 8U ||
         matrix == 16U || matrix == 17U) &&
        image->monochrome == 0U &&
        (image->subsampling_x != 0U ||
         image->subsampling_y != 0U)) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (matrix == 16U &&
        image->bit_depth < 10U &&
        source->color_range == 0U) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (matrix == 17U &&
        image->bit_depth < 9U &&
        source->color_range == 0U) {
        return AVIFDEC_UNSUPPORTED;
    }
    if (matrix == 10U || matrix == 13U ||
        matrix == 14U || matrix == 15U) {
        status = avif_color_cicp_supported(&cicp);
        return status;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_color_validate_image(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifColorInternal *internal,
    const AvifdecRgbImage *rgb,
    uint32_t first_output_y,
    uint32_t output_rows,
    size_t *bytes_per_pixel,
    AvifdecError *error) {
    uint32_t source_width;
    uint32_t source_height;
    uint32_t presentation_width;
    uint32_t presentation_height;
    uint32_t chroma_width;
    uint32_t chroma_height;
    size_t channel_count;
    size_t bytes_per_channel;
    size_t row_bytes;
    size_t last_row;
    size_t output_size;
    AvifdecStatus status;
    int has_output_alpha;
    int is_float;
    int is_16_bit;

    if (image == 0 || info == 0 || rgb == 0 ||
        rgb->pixels == 0 || output_rows == 0U) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (rgb->format > AVIFDEC_RGBAF32 ||
        rgb->alpha_mode > AVIFDEC_ALPHA_PREMULTIPLIED ||
        image->bit_depth != info->bit_depth ||
        image->monochrome > 1U ||
        info->monochrome > 1U ||
        image->monochrome != info->monochrome ||
        image->subsampling_x != info->subsampling_x ||
        image->subsampling_y != info->subsampling_y ||
        image->subsampling_x > 1U ||
        image->subsampling_y > 1U ||
        info->has_alpha > 1U ||
        info->color_range > 1U ||
        info->has_nclx > 1U) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (info->color_primaries != internal->source.color_primaries ||
        info->transfer_characteristics !=
            internal->source.transfer_characteristics ||
        info->matrix_coefficients !=
            internal->source.matrix_coefficients ||
        info->color_range != internal->source.color_range ||
        info->has_nclx != internal->source.has_nclx ||
        info->icc_data != internal->source.icc.data ||
        info->icc_size != internal->source.icc.size) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_validate_runtime_cicp(
        &internal->source, image, info, internal->source_is_icc);
    if (status != AVIFDEC_OK) {
        return avif_color_fail(error, status, 0U, 0U);
    }
    if ((info->transform_flags &
         (uint8_t)~(AVIFDEC_TRANSFORM_CLAP |
                    AVIFDEC_TRANSFORM_IROT |
                    AVIFDEC_TRANSFORM_IMIR |
                    AVIFDEC_TRANSFORM_PASP)) != 0U ||
        ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
         info->irot_angle > 3U) ||
        ((info->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U &&
         info->imir_axis > 1U)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    source_width = info->width;
    source_height = info->height;
    if (source_width == 0U || source_height == 0U ||
        image->widths[0] != source_width ||
        image->heights[0] != source_height ||
        info->crop.width == 0U ||
        info->crop.height == 0U ||
        info->crop.x > source_width ||
        info->crop.y > source_height ||
        info->crop.width > source_width - info->crop.x ||
        info->crop.height > source_height - info->crop.y ||
        !avif_color_plane_valid(
            image->planes[0], image->strides[0],
            source_width, source_height)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    presentation_width = info->crop.width;
    presentation_height = info->crop.height;
    if ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
        (info->irot_angle & 1U) != 0U) {
        uint32_t swap;

        swap = presentation_width;
        presentation_width = presentation_height;
        presentation_height = swap;
    }
    if (info->presentation_width != presentation_width ||
        info->presentation_height != presentation_height ||
        rgb->width != presentation_width ||
        rgb->height != presentation_height ||
        first_output_y >= presentation_height ||
        output_rows > presentation_height - first_output_y) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (image->monochrome == 0U) {
        chroma_width = avif_color_subsampled_dimension(
            source_width, image->subsampling_x);
        chroma_height = avif_color_subsampled_dimension(
            source_height, image->subsampling_y);
        if (image->widths[1] != chroma_width ||
            image->widths[2] != chroma_width ||
            image->heights[1] != chroma_height ||
            image->heights[2] != chroma_height ||
            !avif_color_plane_valid(
                image->planes[1], image->strides[1],
                chroma_width, chroma_height) ||
            !avif_color_plane_valid(
                image->planes[2], image->strides[2],
                chroma_width, chroma_height)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
        }
    }
    if (internal->source_is_gray != 0U &&
        image->monochrome == 0U) {
        return avif_color_fail(
            error, AVIFDEC_UNSUPPORTED, 0U, AVIF_COLOR_SIG_GRAY);
    }
    if (info->has_alpha != 0U) {
        if ((image->alpha_bit_depth != 8U &&
             image->alpha_bit_depth != 10U &&
             image->alpha_bit_depth != 12U) ||
            image->alpha_bit_depth != info->alpha_bit_depth ||
            image->alpha_color_range > 1U ||
            image->alpha_color_range != info->alpha_color_range ||
            image->alpha_premultiplied > 1U ||
            image->alpha_premultiplied !=
                info->alpha_premultiplied ||
            image->alpha_width != source_width ||
            image->alpha_height != source_height ||
            !avif_color_plane_valid(
                image->alpha_plane, image->alpha_stride,
                source_width, source_height)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
        }
    }
    has_output_alpha =
        rgb->format == AVIFDEC_RGBA8 ||
        rgb->format == AVIFDEC_RGBA16 ||
        rgb->format == AVIFDEC_RGBAF32;
    is_float =
        rgb->format == AVIFDEC_RGBF32 ||
        rgb->format == AVIFDEC_RGBAF32;
    is_16_bit =
        rgb->format == AVIFDEC_RGB16 ||
        rgb->format == AVIFDEC_RGBA16;
    channel_count = has_output_alpha ? 4U : 3U;
    bytes_per_channel = is_float
        ? sizeof(float)
        : (is_16_bit ? sizeof(uint16_t) : 1U);
    if (!avif_color_size_multiply(
            channel_count, bytes_per_channel,
            bytes_per_pixel) ||
        !avif_color_size_multiply(
            (size_t)presentation_width,
            *bytes_per_pixel, &row_bytes)) {
        return avif_color_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    if (rgb->stride < row_bytes) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (!avif_color_size_multiply(
            (size_t)output_rows - 1U,
            rgb->stride, &last_row) ||
        !avif_color_size_add(
            last_row, row_bytes, &output_size)) {
        return avif_color_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    (void)output_size;
    return AVIFDEC_OK;
}

static void avif_color_axis_weights(uint32_t coordinate,
                                    uint8_t subsampling,
                                    uint8_t centered,
                                    uint32_t sample_count,
                                    uint8_t nearest,
                                    uint32_t *first,
                                    uint32_t *second,
                                    double *fraction) {
    uint32_t base;

    if (subsampling == 0U) {
        *first = coordinate;
        *second = coordinate;
        *fraction = 0.0;
        return;
    }
    if (nearest != 0U) {
        base = coordinate >> 1;
        if (base >= sample_count) {
            base = sample_count - 1U;
        }
        *first = base;
        *second = base;
        *fraction = 0.0;
        return;
    }
    if (centered != 0U) {
        if (coordinate == 0U) {
            base = 0U;
            *fraction = 0.0;
        } else {
            base = (coordinate - 1U) >> 1;
            *fraction = (coordinate & 1U) != 0U ? 0.25 : 0.75;
        }
    } else {
        base = coordinate >> 1;
        *fraction = (coordinate & 1U) != 0U ? 0.5 : 0.0;
    }
    if (base >= sample_count - 1U) {
        base = sample_count - 1U;
        *fraction = 0.0;
    }
    *first = base;
    *second = *fraction == 0.0 ? base : base + 1U;
}

static AvifdecStatus avif_color_chroma_sample(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    uint32_t source_x,
    uint32_t source_y,
    uint8_t plane,
    uint8_t nearest,
    double *sample,
    AvifdecError *error) {
    uint8_t center_x;
    uint8_t center_y;
    uint32_t x0;
    uint32_t x1;
    uint32_t y0;
    uint32_t y1;
    double fx;
    double fy;
    uint16_t s00;
    uint16_t s01;
    uint16_t s10;
    uint16_t s11;
    uint32_t maximum;
    double top;
    double bottom;

    center_x =
        image->subsampling_y != 0U &&
        info->chroma_sample_position == 0U ? 1U : 0U;
    center_y = info->chroma_sample_position == 2U ? 0U : 1U;
    avif_color_axis_weights(
        source_x, image->subsampling_x, center_x,
        image->widths[plane], nearest, &x0, &x1, &fx);
    avif_color_axis_weights(
        source_y, image->subsampling_y, center_y,
        image->heights[plane], nearest, &y0, &y1, &fy);
    s00 = image->planes[plane][
        (size_t)y0 * image->strides[plane] + x0];
    s01 = image->planes[plane][
        (size_t)y0 * image->strides[plane] + x1];
    s10 = image->planes[plane][
        (size_t)y1 * image->strides[plane] + x0];
    s11 = image->planes[plane][
        (size_t)y1 * image->strides[plane] + x1];
    maximum = avif_color_max_sample(image->bit_depth);
    if (s00 > maximum || s01 > maximum ||
        s10 > maximum || s11 > maximum) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    top = (double)s00 + ((double)s01 - (double)s00) * fx;
    bottom = (double)s10 + ((double)s11 - (double)s10) * fx;
    *sample = top + (bottom - top) * fy;
    return AVIFDEC_OK;
}

static double avif_color_normalize_luma(double sample,
                                        uint8_t bit_depth,
                                        uint8_t full_range) {
    uint32_t maximum;
    uint32_t shift;
    uint32_t minimum;
    uint32_t range;

    maximum = avif_color_max_sample(bit_depth);
    if (full_range != 0U) {
        return sample / (double)maximum;
    }
    shift = (uint32_t)bit_depth - 8U;
    minimum = (uint32_t)16U << shift;
    range = (uint32_t)219U << shift;
    return (sample - (double)minimum) / (double)range;
}

static double avif_color_normalize_chroma(double sample,
                                          uint8_t bit_depth,
                                          uint8_t full_range) {
    uint32_t center;
    uint32_t range;

    center = (uint32_t)1U << ((uint32_t)bit_depth - 1U);
    range = full_range != 0U
        ? avif_color_max_sample(bit_depth)
        : (uint32_t)224U << ((uint32_t)bit_depth - 8U);
    return (sample - (double)center) / (double)range;
}

static int32_t avif_color_floor_div2(int32_t value) {
    if (value >= 0) {
        return value / 2;
    }
    return -((-value + 1) / 2);
}

static int32_t avif_color_clip_i32(int32_t value,
                                   int32_t minimum,
                                   int32_t maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static void avif_color_matrix_float_vector(const float matrix[9],
                                           const double input[3],
                                           double output[3]) {
    double result[3];
    size_t row;

    for (row = 0U; row < 3U; ++row) {
        result[row] =
            (double)matrix[row * 3U] * input[0] +
            (double)matrix[row * 3U + 1U] * input[1] +
            (double)matrix[row * 3U + 2U] * input[2];
    }
    output[0] = result[0];
    output[1] = result[1];
    output[2] = result[2];
}

static AvifdecStatus avif_color_matrix_decode(
    const AvifColorInternal *internal,
    uint8_t bit_depth,
    double y_sample,
    double cb_sample,
    double cr_sample,
    double output[3],
    uint8_t *linear_output) {
    uint16_t matrix;
    double y;
    double cb;
    double cr;
    double kr;
    double kb;
    double kg;

    matrix = internal->source.matrix_coefficients;
    *linear_output = 0U;
    if (matrix == 0U) {
        output[0] = avif_color_normalize_luma(
            cr_sample, bit_depth, internal->source.color_range);
        output[1] = avif_color_normalize_luma(
            y_sample, bit_depth, internal->source.color_range);
        output[2] = avif_color_normalize_luma(
            cb_sample, bit_depth, internal->source.color_range);
        return AVIFDEC_OK;
    }
    if (matrix == 8U) {
        double center;
        double maximum;
        double cg;
        double co;
        double temporary;

        center = (double)((uint32_t)1U << (bit_depth - 1U));
        maximum = (double)avif_color_max_sample(bit_depth);
        cg = cb_sample - center;
        co = cr_sample - center;
        temporary = y_sample - cg;
        output[0] = avif_color_normalize_luma(
            avif_color_clamp(temporary + co, 0.0, maximum),
            bit_depth, internal->source.color_range);
        output[1] = avif_color_normalize_luma(
            avif_color_clamp(y_sample + cg, 0.0, maximum),
            bit_depth, internal->source.color_range);
        output[2] = avif_color_normalize_luma(
            avif_color_clamp(temporary - co, 0.0, maximum),
            bit_depth, internal->source.color_range);
        return AVIFDEC_OK;
    }
    if (matrix == 16U || matrix == 17U) {
        uint8_t rgb_depth;
        int32_t center;
        int32_t maximum;
        int32_t cg;
        int32_t co;
        int32_t temporary;
        int32_t red;
        int32_t green;
        int32_t blue;

        rgb_depth = (uint8_t)(bit_depth - (matrix == 16U ? 2U : 1U));
        center = (int32_t)1 << (bit_depth - 1U);
        maximum = ((int32_t)1 << rgb_depth) - 1;
        cg = (int32_t)cb_sample - center;
        co = (int32_t)cr_sample - center;
        temporary = (int32_t)y_sample - avif_color_floor_div2(cg);
        green = avif_color_clip_i32(temporary + cg, 0, maximum);
        blue = avif_color_clip_i32(
            temporary - avif_color_floor_div2(co), 0, maximum);
        red = avif_color_clip_i32(blue + co, 0, maximum);
        output[0] = avif_color_normalize_luma(
            (double)red, rgb_depth, internal->source.color_range);
        output[1] = avif_color_normalize_luma(
            (double)green, rgb_depth, internal->source.color_range);
        output[2] = avif_color_normalize_luma(
            (double)blue, rgb_depth, internal->source.color_range);
        return AVIFDEC_OK;
    }
    y = avif_color_normalize_luma(
        y_sample, bit_depth, internal->source.color_range);
    cb = avif_color_normalize_chroma(
        cb_sample, bit_depth, internal->source.color_range);
    cr = avif_color_normalize_chroma(
        cr_sample, bit_depth, internal->source.color_range);
    if (matrix == 11U) {
        output[0] = 2.0 * cr + 0.991902 * y;
        output[1] = y;
        output[2] = (2.0 * cb + y) / 0.986566;
        return AVIFDEC_OK;
    }
    if (matrix == 14U || matrix == 15U) {
        double encoded_lms[3];
        double linear_lms[3];
        size_t index;

        encoded_lms[0] = y;
        encoded_lms[1] = cb;
        encoded_lms[2] = cr;
        avif_color_matrix_float_vector(
            internal->special_inverse, encoded_lms, encoded_lms);
        for (index = 0U; index < 3U; ++index) {
            linear_lms[index] = avif_color_transfer_decode_raw(
                internal->source.transfer_characteristics,
                matrix, encoded_lms[index]);
        }
        avif_color_matrix_float_vector(
            internal->special_to_rgb, linear_lms, output);
        *linear_output = 1U;
        return AVIFDEC_OK;
    }
    if (!avif_color_matrix_kr_kb(
            matrix, internal->source.color_primaries, &kr, &kb)) {
        return AVIFDEC_UNSUPPORTED;
    }
    kg = 1.0 - kr - kb;
    if (matrix == 10U || matrix == 13U) {
        double nb;
        double pb;
        double nr;
        double pr;
        double linear_y;
        double linear_red;
        double linear_blue;
        double linear_green;

        nb = avif_color_transfer_encode_raw(
            internal->source.transfer_characteristics,
            matrix, 1.0 - kb);
        pb = 1.0 - avif_color_transfer_encode_raw(
            internal->source.transfer_characteristics,
            matrix, kb);
        nr = avif_color_transfer_encode_raw(
            internal->source.transfer_characteristics,
            matrix, 1.0 - kr);
        pr = 1.0 - avif_color_transfer_encode_raw(
            internal->source.transfer_characteristics,
            matrix, kr);
        output[2] = y + 2.0 * (cb <= 0.0 ? nb : pb) * cb;
        output[0] = y + 2.0 * (cr <= 0.0 ? nr : pr) * cr;
        linear_y = avif_color_transfer_decode_raw(
            internal->source.transfer_characteristics, matrix, y);
        linear_red = avif_color_transfer_decode_raw(
            internal->source.transfer_characteristics,
            matrix, output[0]);
        linear_blue = avif_color_transfer_decode_raw(
            internal->source.transfer_characteristics,
            matrix, output[2]);
        linear_green =
            (linear_y - kr * linear_red - kb * linear_blue) / kg;
        output[1] = avif_color_transfer_encode_raw(
            internal->source.transfer_characteristics,
            matrix, linear_green);
        return AVIFDEC_OK;
    }
    output[0] = y + 2.0 * (1.0 - kr) * cr;
    output[2] = y + 2.0 * (1.0 - kb) * cb;
    output[1] = (y - kr * output[0] - kb * output[2]) / kg;
    return AVIFDEC_OK;
}

static double avif_color_hlg_gamma(double display_peak_nits) {
    double gamma;

    gamma = 1.2 + 0.42 * avif_color_log10(
        display_peak_nits / 1000.0);
    return avif_color_clamp(gamma, 1.0, 1.5);
}

static void avif_color_luma_coefficients(uint16_t primaries,
                                         double coefficients[3]) {
    double rgb_to_xyz[9];
    double white[3];

    if (avif_color_rgb_to_xyz(primaries, rgb_to_xyz, white)) {
        coefficients[0] = rgb_to_xyz[3];
        coefficients[1] = rgb_to_xyz[4];
        coefficients[2] = rgb_to_xyz[5];
    } else {
        coefficients[0] = 0.2627;
        coefficients[1] = 0.6780;
        coefficients[2] = 0.0593;
    }
}

static void avif_color_source_scale(
    const AvifColorInternal *internal,
    double color[3]) {
    uint16_t transfer;
    double scale;
    size_t index;

    transfer = internal->source.transfer_characteristics;
    if (transfer == 16U) {
        scale = 10000.0 / 203.0;
        for (index = 0U; index < 3U; ++index) {
            color[index] *= scale;
        }
    } else if (transfer == 18U) {
        double luminance;
        double coefficients[3];
        double scene_white;
        double gamma;
        double factor;

        avif_color_luma_coefficients(
            internal->source.color_primaries, coefficients);
        luminance = coefficients[0] * color[0] +
                    coefficients[1] * color[1] +
                    coefficients[2] * color[2];
        scene_white = avif_color_transfer_decode_raw(
            18U, 0U, 0.75);
        gamma = avif_color_hlg_gamma(
            internal->options.display_peak_nits);
        if (luminance <= 0.0) {
            color[0] = 0.0;
            color[1] = 0.0;
            color[2] = 0.0;
        } else {
            factor =
                avif_color_pow(luminance, gamma - 1.0) /
                avif_color_pow(scene_white, gamma) *
                ((double)internal->options.reference_white_nits / 203.0);
            for (index = 0U; index < 3U; ++index) {
                color[index] *= factor;
            }
        }
    } else {
        scale = (double)internal->options.reference_white_nits / 203.0;
        for (index = 0U; index < 3U; ++index) {
            color[index] *= scale;
        }
    }
}

static void avif_color_icc_source_scale(
    const AvifColorInternal *internal,
    double color[3]) {
    double scale;
    size_t index;

    scale = (double)internal->options.reference_white_nits / 203.0;
    for (index = 0U; index < 3U; ++index) {
        color[index] *= scale;
    }
}

static void avif_color_final_clip(
    const AvifColorInternal *internal,
    double color[3]) {
    size_t index;

    if (internal->options.hdr_policy ==
        AVIFDEC_COLOR_HDR_CLIP_TO_DISPLAY) {
        double peak;

        peak = (double)internal->options.display_peak_nits / 203.0;
        for (index = 0U; index < 3U; ++index) {
            color[index] = avif_color_clamp(color[index], 0.0, peak);
        }
    }
}

static void avif_color_destination_encode(
    const AvifColorInternal *internal,
    const double linear[3],
    double encoded[3]) {
    uint16_t transfer;
    double raw[3];
    size_t index;

    transfer = internal->destination.transfer_characteristics;
    if (transfer == 16U) {
        for (index = 0U; index < 3U; ++index) {
            raw[index] = avif_color_clamp(
                linear[index] * 203.0 / 10000.0, 0.0, 1.0);
        }
    } else if (transfer == 18U) {
        double relative[3];
        double luminance;
        double coefficients[3];
        double scene_white;
        double gamma;
        double scene_luminance;
        double factor;

        for (index = 0U; index < 3U; ++index) {
            relative[index] = avif_color_max(
                linear[index] * 203.0 /
                    (double)internal->options.reference_white_nits,
                0.0);
        }
        avif_color_luma_coefficients(
            internal->destination.color_primaries, coefficients);
        luminance = coefficients[0] * relative[0] +
                    coefficients[1] * relative[1] +
                    coefficients[2] * relative[2];
        scene_white = avif_color_transfer_decode_raw(
            18U, 0U, 0.75);
        gamma = avif_color_hlg_gamma(
            internal->options.display_peak_nits);
        if (luminance <= 0.0) {
            raw[0] = 0.0;
            raw[1] = 0.0;
            raw[2] = 0.0;
        } else {
            scene_luminance =
                scene_white * avif_color_pow(luminance, 1.0 / gamma);
            factor = scene_luminance / luminance;
            for (index = 0U; index < 3U; ++index) {
                raw[index] = avif_color_clamp(
                    relative[index] * factor, 0.0, 1.0);
            }
        }
    } else {
        for (index = 0U; index < 3U; ++index) {
            raw[index] = linear[index] * 203.0 /
                (double)internal->options.reference_white_nits;
        }
    }
    for (index = 0U; index < 3U; ++index) {
        encoded[index] = avif_color_transfer_encode_raw(
            transfer, 0U, raw[index]);
    }
}

static AvifdecStatus avif_color_pixel_native(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifColorInternal *internal,
    uint32_t source_x,
    uint32_t source_y,
    double output[3],
    double *alpha,
    AvifdecError *error) {
    size_t luma_index;
    uint16_t y_sample_u16;
    double y_sample;
    double cb_sample;
    double cr_sample;
    double encoded[3];
    double linear[3];
    uint8_t linear_output;
    uint32_t maximum;
    AvifdecStatus status;
    size_t index;

    luma_index = (size_t)source_y * image->strides[0] + source_x;
    y_sample_u16 = image->planes[0][luma_index];
    maximum = avif_color_max_sample(image->bit_depth);
    if (y_sample_u16 > maximum) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    y_sample = y_sample_u16;
    cb_sample = (double)((uint32_t)1U << (image->bit_depth - 1U));
    cr_sample = cb_sample;
    if (image->monochrome == 0U) {
        status = avif_color_chroma_sample(
            image, info, source_x, source_y, 1U,
            internal->options.chroma_upsampling ==
                AVIFDEC_CHROMA_UPSAMPLING_NEAREST,
            &cb_sample, error);
        if (status != AVIFDEC_OK) {
            return status;
        }
        status = avif_color_chroma_sample(
            image, info, source_x, source_y, 2U,
            internal->options.chroma_upsampling ==
                AVIFDEC_CHROMA_UPSAMPLING_NEAREST,
            &cr_sample, error);
        if (status != AVIFDEC_OK) {
            return status;
        }
    }
    if (image->monochrome != 0U) {
        encoded[0] = avif_color_normalize_luma(
            y_sample, image->bit_depth, internal->source.color_range);
        encoded[1] = encoded[0];
        encoded[2] = encoded[0];
        linear_output = 0U;
    } else {
        status = avif_color_matrix_decode(
            internal, image->bit_depth,
            y_sample, cb_sample, cr_sample,
            encoded, &linear_output);
        if (status != AVIFDEC_OK) {
            return avif_color_fail(error, status, 0U, 0U);
        }
    }
    *alpha = 1.0;
    if (info->has_alpha != 0U) {
        size_t alpha_index;
        uint16_t alpha_sample;

        alpha_index =
            (size_t)source_y * image->alpha_stride + source_x;
        alpha_sample = image->alpha_plane[alpha_index];
        if (alpha_sample >
            avif_color_max_sample(image->alpha_bit_depth)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        *alpha = avif_color_clamp(
            avif_color_normalize_luma(
                alpha_sample, image->alpha_bit_depth,
                image->alpha_color_range),
            0.0, 1.0);
    }
    if (linear_output != 0U) {
        linear[0] = encoded[0];
        linear[1] = encoded[1];
        linear[2] = encoded[2];
        for (index = 0U; index < 3U; ++index) {
            encoded[index] = avif_color_transfer_encode_raw(
                internal->source.transfer_characteristics,
                internal->source.matrix_coefficients,
                linear[index]);
        }
    }
    if (image->alpha_premultiplied != 0U) {
        if (*alpha <= 0.0) {
            encoded[0] = 0.0;
            encoded[1] = 0.0;
            encoded[2] = 0.0;
        } else {
            for (index = 0U; index < 3U; ++index) {
                encoded[index] /= *alpha;
            }
        }
        linear_output = 0U;
    }
    if (internal->source_is_icc != 0U) {
        if (internal->source_is_gray != 0U) {
            linear[0] = avif_color_curve_evaluate(
                &internal->curves[0], encoded[0]);
            linear[1] = 0.0;
            linear[2] = 0.0;
        } else {
            for (index = 0U; index < 3U; ++index) {
                linear[index] = avif_color_curve_evaluate(
                    &internal->curves[index], encoded[index]);
            }
        }
        avif_color_icc_source_scale(internal, linear);
    } else {
        if (linear_output == 0U) {
            for (index = 0U; index < 3U; ++index) {
                linear[index] = avif_color_transfer_decode_raw(
                    internal->source.transfer_characteristics,
                    internal->source.matrix_coefficients,
                    encoded[index]);
            }
        }
        avif_color_source_scale(internal, linear);
    }
    output[0] = linear[0];
    output[1] = linear[1];
    output[2] = linear[2];
    return AVIFDEC_OK;
}

static AvifdecStatus avif_color_pixel(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifColorInternal *internal,
    uint32_t source_x,
    uint32_t source_y,
    double output[3],
    double *alpha,
    AvifdecError *error) {
    double native[3];
    AvifdecStatus status;

    status = avif_color_pixel_native(
        image, info, internal, source_x, source_y,
        native, alpha, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    avif_color_matrix_float_vector(
        internal->color_matrix, native, output);
    avif_color_final_clip(internal, output);
    return AVIFDEC_OK;
}

static AvifdecStatus avif_color_store_pixel(
    const AvifColorInternal *internal,
    const AvifdecRgbImage *rgb,
    unsigned char *pixel,
    const double linear[3],
    double alpha) {
    int has_alpha;
    int is_float;
    int is_16_bit;
    double color[3];
    size_t index;

    has_alpha =
        rgb->format == AVIFDEC_RGBA8 ||
        rgb->format == AVIFDEC_RGBA16 ||
        rgb->format == AVIFDEC_RGBAF32;
    is_float =
        rgb->format == AVIFDEC_RGBF32 ||
        rgb->format == AVIFDEC_RGBAF32;
    is_16_bit =
        rgb->format == AVIFDEC_RGB16 ||
        rgb->format == AVIFDEC_RGBA16;
    if (is_float) {
        color[0] = linear[0];
        color[1] = linear[1];
        color[2] = linear[2];
        if (rgb->alpha_mode == AVIFDEC_ALPHA_PREMULTIPLIED) {
            color[0] *= alpha;
            color[1] *= alpha;
            color[2] *= alpha;
        }
        for (index = 0U; index < 3U; ++index) {
            avif_color_store_f32(
                pixel + index * sizeof(float),
                (float)color[index]);
        }
        if (has_alpha) {
            avif_color_store_f32(
                pixel + 3U * sizeof(float), (float)alpha);
        }
        return AVIFDEC_OK;
    }
    avif_color_destination_encode(internal, linear, color);
    if (rgb->alpha_mode == AVIFDEC_ALPHA_PREMULTIPLIED) {
        color[0] *= alpha;
        color[1] *= alpha;
        color[2] *= alpha;
    }
    if (is_16_bit) {
        for (index = 0U; index < 3U; ++index) {
            uint16_t value;

            value = (uint16_t)avif_color_round_nonnegative(
                avif_color_clamp(color[index], 0.0, 1.0) * 65535.0);
            avif_color_store_u16(
                pixel + index * sizeof(uint16_t), value);
        }
        if (has_alpha) {
            uint16_t value;

            value = (uint16_t)avif_color_round_nonnegative(
                avif_color_clamp(alpha, 0.0, 1.0) * 65535.0);
            avif_color_store_u16(
                pixel + 3U * sizeof(uint16_t), value);
        }
    } else {
        for (index = 0U; index < 3U; ++index) {
            pixel[index] = (unsigned char)avif_color_round_nonnegative(
                avif_color_clamp(color[index], 0.0, 1.0) * 255.0);
        }
        if (has_alpha) {
            pixel[3] = (unsigned char)avif_color_round_nonnegative(
                avif_color_clamp(alpha, 0.0, 1.0) * 255.0);
        }
    }
    return AVIFDEC_OK;
}

static int avif_color_description_equal(
    const AvifdecColorDescription *left,
    const AvifdecColorDescription *right) {
    return left->color_primaries == right->color_primaries &&
           left->transfer_characteristics ==
               right->transfer_characteristics &&
           left->matrix_coefficients == right->matrix_coefficients &&
           left->color_range == right->color_range &&
           left->has_nclx == right->has_nclx &&
           left->icc.data == right->icc.data &&
           left->icc.size == right->icc.size;
}

AvifdecStatus avif_color_transform_validate_working(
    const AvifdecColorTransform *transform,
    const AvifdecColorDescription *working,
    uint8_t output_format,
    AvifdecError *error) {
    AvifColorInternal internal;
    AvifdecStatus status;

    avif_color_error_clear(error);
    if (working == 0 ||
        (output_format != AVIFDEC_RGB16 &&
         output_format != AVIFDEC_RGBA16 &&
         output_format != AVIFDEC_RGBF32 &&
         output_format != AVIFDEC_RGBAF32)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_transform_load(
        transform, &internal, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    if (!avif_color_description_equal(
            &internal.source, working) ||
        internal.options.destination_color_primaries == 0U ||
        internal.options.destination_transfer_characteristics == 0U) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_color_image_native_validated(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifColorInternal *internal,
    uint32_t x,
    uint32_t y,
    double native[3],
    double *alpha,
    AvifdecError *error) {
    AvifdecRgbImage validation_rgb;
    AvifdecStatus status;
    size_t validation_stride;
    size_t bytes_per_pixel;
    uint32_t source_x;
    uint32_t source_y;

    if (image == 0 || info == 0 || internal == 0 ||
        native == 0 || alpha == 0 ||
        x >= info->presentation_width ||
        y >= info->presentation_height ||
        !avif_color_size_multiply(
            (size_t)info->presentation_width,
            4U * sizeof(float), &validation_stride)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avif_color_fill(
        &validation_rgb, 0U, sizeof(validation_rgb));
    validation_rgb.pixels = native;
    validation_rgb.stride = validation_stride;
    validation_rgb.width = info->presentation_width;
    validation_rgb.height = info->presentation_height;
    validation_rgb.format = AVIFDEC_RGBAF32;
    validation_rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    status = avif_color_validate_image(
        image, info, internal, &validation_rgb,
        y, 1U, &bytes_per_pixel, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    (void)bytes_per_pixel;
    avif_color_inverse_map(
        info, x, y, &source_x, &source_y);
    return avif_color_pixel_native(
        image, info, internal, source_x, source_y,
        native, alpha, error);
}

AvifdecStatus avif_color_gain_map_texel(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *source_transform,
    uint32_t x,
    uint32_t y,
    uint8_t channel_count,
    float gain[3],
    AvifdecError *error) {
    AvifColorInternal internal;
    AvifdecRgbImage validation_rgb;
    uint32_t source_x;
    uint32_t source_y;
    size_t validation_stride;
    size_t bytes_per_pixel;
    size_t luma_index;
    uint32_t maximum;
    double y_sample;
    double cb_sample;
    double cr_sample;
    double encoded[3];
    uint8_t linear_output;
    size_t channel;
    AvifdecStatus status;

    avif_color_error_clear(error);
    if (gain != 0) {
        gain[0] = 0.0f;
        gain[1] = 0.0f;
        gain[2] = 0.0f;
    }
    if (image == 0 || info == 0 || gain == 0 ||
        (channel_count != 1U && channel_count != 3U) ||
        (channel_count == 1U && info->monochrome == 0U) ||
        (channel_count == 3U && info->monochrome != 0U) ||
        x >= info->presentation_width ||
        y >= info->presentation_height ||
        !avif_color_size_multiply(
            info->presentation_width, 3U * sizeof(float),
            &validation_stride)) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (channel_count == 1U) {
        uint32_t presentation_width = info->crop.width;
        uint32_t presentation_height = info->crop.height;

        if (image->bit_depth != info->bit_depth ||
            image->monochrome == 0U ||
            image->monochrome != info->monochrome ||
            image->widths[0] != info->width ||
            image->heights[0] != info->height ||
            info->color_range > 1U ||
            info->crop.width == 0U || info->crop.height == 0U ||
            info->crop.x > info->width ||
            info->crop.y > info->height ||
            info->crop.width > info->width - info->crop.x ||
            info->crop.height > info->height - info->crop.y ||
            ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
             info->irot_angle > 3U) ||
            ((info->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U &&
             info->imir_axis > 1U) ||
            !avif_color_plane_valid(
                image->planes[0], image->strides[0],
                info->width, info->height)) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
        }
        if ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
            (info->irot_angle & 1U) != 0U) {
            uint32_t temporary = presentation_width;

            presentation_width = presentation_height;
            presentation_height = temporary;
        }
        if (presentation_width != info->presentation_width ||
            presentation_height != info->presentation_height) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
        }
        avif_color_inverse_map(info, x, y, &source_x, &source_y);
        luma_index =
            (size_t)source_y * image->strides[0] + source_x;
        maximum = avif_color_max_sample(image->bit_depth);
        if (image->planes[0][luma_index] > maximum) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        gain[0] = (float)avif_color_clamp(
            avif_color_normalize_luma(
                image->planes[0][luma_index], image->bit_depth,
                info->color_range),
            0.0, 1.0);
        gain[1] = gain[0];
        gain[2] = gain[0];
        return AVIFDEC_OK;
    }
    status = avif_color_transform_load(
        source_transform, &internal, error);
    if (status != AVIFDEC_OK) return status;
    avif_color_fill(&validation_rgb, 0U, sizeof(validation_rgb));
    validation_rgb.pixels = gain;
    validation_rgb.stride = validation_stride;
    validation_rgb.width = info->presentation_width;
    validation_rgb.height = info->presentation_height;
    validation_rgb.format = AVIFDEC_RGBF32;
    validation_rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    status = avif_color_validate_image(
        image, info, &internal, &validation_rgb,
        y, 1U, &bytes_per_pixel, error);
    if (status != AVIFDEC_OK) return status;
    (void)bytes_per_pixel;

    avif_color_inverse_map(info, x, y, &source_x, &source_y);
    luma_index = (size_t)source_y * image->strides[0] + source_x;
    maximum = avif_color_max_sample(image->bit_depth);
    if (image->planes[0][luma_index] > maximum) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    y_sample = image->planes[0][luma_index];
    cb_sample = (double)((uint32_t)1U << (image->bit_depth - 1U));
    cr_sample = cb_sample;
    status = avif_color_chroma_sample(
        image, info, source_x, source_y, 1U, 0U,
        &cb_sample, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_color_chroma_sample(
        image, info, source_x, source_y, 2U, 0U,
        &cr_sample, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_color_matrix_decode(
        &internal, image->bit_depth, y_sample, cb_sample, cr_sample,
        encoded, &linear_output);
    if (status != AVIFDEC_OK) {
        return avif_color_fail(error, status, 0U, 0U);
    }
    if (linear_output != 0U) {
        for (channel = 0U; channel < 3U; ++channel) {
            encoded[channel] = avif_color_transfer_encode_raw(
                internal.source.transfer_characteristics,
                internal.source.matrix_coefficients,
                encoded[channel]);
        }
    }
    for (channel = 0U; channel < 3U; ++channel) {
        if (!avif_color_finite(encoded[channel])) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        gain[channel] = (float)avif_color_clamp(
            encoded[channel], 0.0, 1.0);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_color_float_rgba(
    const double linear[3],
    double alpha,
    float rgba[4],
    AvifdecError *error) {
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        if (!avif_color_finite(linear[index]) ||
            avif_color_abs(linear[index]) >
                3.40282346638528859812e38) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        rgba[index] = (float)linear[index];
    }
    if (!avif_color_finite(alpha) ||
        alpha < 0.0 || alpha > 1.0) {
        rgba[0] = 0.0f;
        rgba[1] = 0.0f;
        rgba[2] = 0.0f;
        return avif_color_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    rgba[3] = (float)alpha;
    return AVIFDEC_OK;
}

AvifdecStatus avif_color_image_pixel_to_linear(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    uint32_t x,
    uint32_t y,
    float rgba[4],
    AvifdecError *error) {
    AvifColorInternal internal;
    AvifdecStatus status;
    double native[3];
    double linear[3];
    double alpha;

    avif_color_error_clear(error);
    if (rgba != 0) {
        rgba[0] = 0.0f;
        rgba[1] = 0.0f;
        rgba[2] = 0.0f;
        rgba[3] = 0.0f;
    }
    if (rgba == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_transform_load(
        transform, &internal, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_color_image_native_validated(
        image, info, &internal, x, y,
        native, &alpha, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    avif_color_matrix_float_vector(
        internal.color_matrix, native, linear);
    avif_color_final_clip(&internal, linear);
    return avif_color_float_rgba(
        linear, alpha, rgba, error);
}

AvifdecStatus avif_color_transform_init_source_to_working(
    const AvifdecColorDescription *source,
    const AvifdecColorTransform *working_to_output,
    AvifdecColorTransform *source_transform,
    AvifdecError *error) {
    AvifColorInternal working;
    AvifdecColorOptions options;
    AvifdecLimits limits;
    AvifdecStatus status;

    avif_color_error_clear(error);
    if (source == 0 || source_transform == 0 ||
        working_to_output == source_transform) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_transform_load(
        working_to_output, &working, error);
    if (status != AVIFDEC_OK) {
        avif_color_fill(
            source_transform, 0U, sizeof(*source_transform));
        return status;
    }
    if (working.options.destination_color_primaries == 0U ||
        working.options.destination_transfer_characteristics == 0U) {
        avif_color_fill(
            source_transform, 0U, sizeof(*source_transform));
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avif_color_copy(
        &options, &working.options, sizeof(options));
    options.source = AVIFDEC_COLOR_SOURCE_AUTO;
    avif_color_fill(&limits, 0U, sizeof(limits));
    limits.max_icc_bytes = working.max_icc_bytes;
    limits.max_icc_curve_entries =
        working.max_icc_curve_entries;
    return avifdec_color_transform_init(
        source, &options, &limits, 0, 0U,
        source_transform, error);
}

AvifdecStatus avif_color_image_pixel_to_working(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *source_transform,
    const AvifdecColorTransform *working_to_output,
    uint32_t x,
    uint32_t y,
    float rgba[4],
    AvifdecError *error) {
    AvifColorInternal source;
    AvifColorInternal working;
    AvifdecStatus status;
    double native[3];
    double connection[3];
    double working_native[3];
    double alpha;

    avif_color_error_clear(error);
    if (rgba != 0) {
        rgba[0] = 0.0f;
        rgba[1] = 0.0f;
        rgba[2] = 0.0f;
        rgba[3] = 0.0f;
    }
    if (rgba == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_transform_load(
        source_transform, &source, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_color_transform_load(
        working_to_output, &working, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    if (source.options.intent != working.options.intent ||
        source.options.hdr_policy != working.options.hdr_policy ||
        source.options.reference_white_nits !=
            working.options.reference_white_nits ||
        source.options.display_peak_nits !=
            working.options.display_peak_nits) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_image_native_validated(
        image, info, &source, x, y,
        native, &alpha, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    avif_color_matrix_float_vector(
        source.connection_matrix, native, connection);
    if (working.source_is_gray != 0U) {
        double white_y;
        double gray;

        white_y = working.connection_matrix[3];
        if (avif_color_abs(white_y) < 1.0e-12) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U,
                AVIF_COLOR_SIG_GRAY);
        }
        gray = connection[1] / white_y;
        working_native[0] = gray;
        working_native[1] = gray;
        working_native[2] = gray;
    } else {
        if (working.connection_invertible == 0U) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        avif_color_matrix_float_vector(
            working.connection_inverse,
            connection, working_native);
    }
    return avif_color_float_rgba(
        working_native, alpha, rgba, error);
}

static AvifdecStatus avif_color_transform_linear(
    const AvifColorInternal *internal,
    const float working_rgb[3],
    double output[3],
    AvifdecError *error) {
    double input[3];
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        if (!avif_color_finite(working_rgb[index])) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
        }
        input[index] = working_rgb[index];
    }
    if (internal->source_is_gray != 0U) {
        if (working_rgb[1] != working_rgb[0] ||
            working_rgb[2] != working_rgb[0]) {
            return avif_color_fail(
                error, AVIFDEC_UNSUPPORTED, 0U,
                AVIF_COLOR_SIG_GRAY);
        }
        input[1] = 0.0;
        input[2] = 0.0;
    }
    avif_color_matrix_float_vector(
        internal->color_matrix, input, output);
    avif_color_final_clip(internal, output);
    for (index = 0U; index < 3U; ++index) {
        if (!avif_color_finite(output[index]) ||
            avif_color_abs(output[index]) >
                3.40282346638528859812e38) {
            return avif_color_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_color_transform_linear_to_linear(
    const AvifdecColorTransform *transform,
    const float working_rgb[3],
    float output_rgb[3],
    AvifdecError *error) {
    AvifColorInternal internal;
    double output[3];
    AvifdecStatus status;
    size_t index;

    avif_color_error_clear(error);
    if (output_rgb != 0) {
        output_rgb[0] = 0.0f;
        output_rgb[1] = 0.0f;
        output_rgb[2] = 0.0f;
    }
    if (working_rgb == 0 || output_rgb == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_transform_load(
        transform, &internal, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_color_transform_linear(
        &internal, working_rgb, output, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    for (index = 0U; index < 3U; ++index) {
        output_rgb[index] = (float)output[index];
    }
    return AVIFDEC_OK;
}

AvifdecStatus avif_color_transform_linear_to_encoded16(
    const AvifdecColorTransform *transform,
    const float working_rgb[3],
    uint16_t output_rgb[3],
    AvifdecError *error) {
    AvifColorInternal internal;
    double linear[3];
    double encoded[3];
    AvifdecStatus status;
    size_t index;

    avif_color_error_clear(error);
    if (output_rgb != 0) {
        output_rgb[0] = 0U;
        output_rgb[1] = 0U;
        output_rgb[2] = 0U;
    }
    if (working_rgb == 0 || output_rgb == 0) {
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_color_transform_load(
        transform, &internal, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_color_transform_linear(
        &internal, working_rgb, linear, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    avif_color_destination_encode(
        &internal, linear, encoded);
    for (index = 0U; index < 3U; ++index) {
        output_rgb[index] =
            (uint16_t)avif_color_round_nonnegative(
                avif_color_clamp(
                    encoded[index], 0.0, 1.0) * 65535.0);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_color_image_rows(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    AvifdecRgbImage *rgb,
    uint32_t first_output_y,
    uint32_t output_rows,
    AvifdecError *error) {
    AvifColorInternal internal;
    size_t bytes_per_pixel;
    uint32_t output_y;
    AvifdecStatus status;

    avif_color_error_clear(error);
    status = avif_color_transform_load(
        transform, &internal, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    status = avif_color_validate_image(
        image, info, &internal, rgb,
        first_output_y, output_rows,
        &bytes_per_pixel, error);
    if (status != AVIFDEC_OK) {
        return status;
    }
    for (output_y = first_output_y;
         output_y < first_output_y + output_rows;
         ++output_y) {
        unsigned char *output_row;
        uint32_t output_x;

        output_row = (unsigned char *)rgb->pixels +
            (size_t)(output_y - first_output_y) * rgb->stride;
        for (output_x = 0U;
             output_x < info->presentation_width;
             ++output_x) {
            uint32_t source_x;
            uint32_t source_y;
            double linear[3];
            double alpha;
            unsigned char *pixel;

            avif_color_inverse_map(
                info, output_x, output_y, &source_x, &source_y);
            status = avif_color_pixel(
                image, info, &internal,
                source_x, source_y,
                linear, &alpha, error);
            if (status != AVIFDEC_OK) {
                return status;
            }
            pixel = output_row + (size_t)output_x * bytes_per_pixel;
            status = avif_color_store_pixel(
                &internal, rgb, pixel, linear, alpha);
            if (status != AVIFDEC_OK) {
                return avif_color_fail(error, status, 0U, 0U);
            }
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_image_to_rgb_with_transform(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    AvifdecRgbImage *rgb,
    AvifdecError *error) {
    if (rgb == 0) {
        avif_color_error_clear(error);
        return avif_color_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return avif_color_image_rows(
        image, info, transform, rgb, 0U, rgb->height, error);
}

AvifdecStatus avifdec_image_to_rgb_row_with_transform(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    AvifdecRgbImage *rgb,
    uint32_t row,
    AvifdecError *error) {
    return avif_color_image_rows(
        image, info, transform, rgb, row, 1U, error);
}
