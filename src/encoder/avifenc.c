#include "encoder/avifenc.h"
#include "base.h"

#define AVIFENC_OUTPUT_FIXED_ALLOWANCE 65536U
#define AVIFENC_OUTPUT_BYTES_PER_PIXEL 16U
#define AVIFENC_WORKSPACE_BYTES_PER_PIXEL 3U

static AvifencStatus avifenc_fail(AvifencError *error,
                                  AvifencStatus status,
                                  AvifencErrorContext context,
                                  size_t required_size,
                                  size_t provided_size) {
    if (error != 0) {
        error->status = status;
        error->context = context;
        error->required_size = required_size;
        error->provided_size = provided_size;
    }
    return status;
}

static void avifenc_error_reset(AvifencError *error) {
    if (error != 0) {
        error->status = AVIFENC_OK;
        error->context = AVIFENC_CONTEXT_NONE;
        error->required_size = 0U;
        error->provided_size = 0U;
    }
}

const char *avifenc_version_string(void) {
    return "0.1.0";
}

const char *avifenc_status_string(AvifencStatus status) {
    switch (status) {
        case AVIFENC_OK: return "ok";
        case AVIFENC_INVALID_ARGUMENT: return "invalid argument";
        case AVIFENC_OVERFLOW: return "integer overflow";
        case AVIFENC_LIMIT_EXCEEDED: return "limit exceeded";
        case AVIFENC_OUT_OF_MEMORY: return "insufficient workspace";
        case AVIFENC_OUTPUT_TOO_SMALL: return "output buffer too small";
        case AVIFENC_UNSUPPORTED: return "unsupported feature";
    }
    return "unknown error";
}

const char *avifenc_error_context_string(AvifencErrorContext context) {
    switch (context) {
        case AVIFENC_CONTEXT_NONE: return "none";
        case AVIFENC_CONTEXT_IMAGE: return "image";
        case AVIFENC_CONTEXT_DIMENSIONS: return "dimensions";
        case AVIFENC_CONTEXT_PLANE_Y: return "Y plane";
        case AVIFENC_CONTEXT_PLANE_U: return "U plane";
        case AVIFENC_CONTEXT_PLANE_V: return "V plane";
        case AVIFENC_CONTEXT_COLOR: return "color properties";
        case AVIFENC_CONTEXT_OPTIONS: return "options";
        case AVIFENC_CONTEXT_QUANTIZER: return "quantizer";
        case AVIFENC_CONTEXT_REQUIREMENTS: return "requirements";
        case AVIFENC_CONTEXT_WORKSPACE: return "workspace";
        case AVIFENC_CONTEXT_OUTPUT: return "output";
        case AVIFENC_CONTEXT_IMPLEMENTATION: return "encoder implementation";
    }
    return "unknown context";
}

void avifenc_options_default(AvifencOptions *options) {
    if (options != 0) options->quantizer = AVIFENC_DEFAULT_QUANTIZER;
}

static AvifencStatus avifenc_validate_plane(const uint8_t *plane,
                                            size_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            AvifencErrorContext context,
                                            AvifencError *error) {
    size_t last_row;
    size_t extent;

    if (plane == 0) {
        return avifenc_fail(
            error, AVIFENC_INVALID_ARGUMENT, context, 1U, 0U);
    }
    if (stride < width) {
        return avifenc_fail(
            error, AVIFENC_INVALID_ARGUMENT, context, width, stride);
    }
    if (!avifdec_size_multiply((size_t)height - 1U, stride, &last_row) ||
        !avifdec_size_add(last_row, width, &extent)) {
        return avifenc_fail(error, AVIFENC_OVERFLOW, context, 0U, 0U);
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_query(const AvifencImage *image,
                            const AvifencOptions *options,
                            AvifencRequirements *requirements,
                            AvifencError *error) {
    AvifencStatus status;
    size_t pixel_count;
    size_t output_capacity;

    avifenc_error_reset(error);
    if (requirements == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_REQUIREMENTS, 1U, 0U);
    }
    requirements->workspace_required = 0U;
    requirements->output_capacity_required = 0U;
    if (image == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_IMAGE, 1U, 0U);
    }
    if (options == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_OPTIONS, 1U, 0U);
    }
    if (image->width == 0U || image->height == 0U ||
        (image->width & 1U) != 0U || (image->height & 1U) != 0U) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_DIMENSIONS, 2U, 0U);
    }
    if (image->width > AVIFENC_MAX_DIMENSION ||
        image->height > AVIFENC_MAX_DIMENSION) {
        return avifenc_fail(error, AVIFENC_LIMIT_EXCEEDED,
                            AVIFENC_CONTEXT_DIMENSIONS,
                            AVIFENC_MAX_DIMENSION,
                            image->width > image->height
                                ? image->width : image->height);
    }
    if (options->quantizer > 255U) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_QUANTIZER, 255U,
                            options->quantizer);
    }
    if (image->color.full_range > 1U ||
        image->color.chroma_sample_position > 3U) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_COLOR, 0U, 0U);
    }

    status = avifenc_validate_plane(
        image->planes[0], image->strides[0], image->width, image->height,
        AVIFENC_CONTEXT_PLANE_Y, error);
    if (status != AVIFENC_OK) return status;
    status = avifenc_validate_plane(
        image->planes[1], image->strides[1], image->width / 2U,
        image->height / 2U, AVIFENC_CONTEXT_PLANE_U, error);
    if (status != AVIFENC_OK) return status;
    status = avifenc_validate_plane(
        image->planes[2], image->strides[2], image->width / 2U,
        image->height / 2U, AVIFENC_CONTEXT_PLANE_V, error);
    if (status != AVIFENC_OK) return status;

    if (!avifdec_size_multiply(image->width, image->height, &pixel_count) ||
        !avifdec_size_multiply(pixel_count,
                              AVIFENC_WORKSPACE_BYTES_PER_PIXEL,
                              &requirements->workspace_required) ||
        !avifdec_size_multiply(pixel_count,
                              AVIFENC_OUTPUT_BYTES_PER_PIXEL,
                              &output_capacity) ||
        !avifdec_size_add(output_capacity,
                          AVIFENC_OUTPUT_FIXED_ALLOWANCE,
                          &requirements->output_capacity_required)) {
        requirements->workspace_required = 0U;
        requirements->output_capacity_required = 0U;
        return avifenc_fail(error, AVIFENC_OVERFLOW,
                            AVIFENC_CONTEXT_REQUIREMENTS, 0U, 0U);
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_encode(const AvifencImage *image,
                             const AvifencOptions *options,
                             void *workspace,
                             size_t workspace_size,
                             void *output,
                             size_t output_capacity,
                             size_t *output_written,
                             AvifencError *error) {
    AvifencRequirements requirements;
    AvifencStatus status;

    avifenc_error_reset(error);
    if (output_written == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_OUTPUT, 1U, 0U);
    }
    *output_written = 0U;
    status = avifenc_query(image, options, &requirements, error);
    if (status != AVIFENC_OK) return status;
    if (workspace_size < requirements.workspace_required) {
        return avifenc_fail(error, AVIFENC_OUT_OF_MEMORY,
                            AVIFENC_CONTEXT_WORKSPACE,
                            requirements.workspace_required,
                            workspace_size);
    }
    if (workspace == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_WORKSPACE,
                            requirements.workspace_required,
                            workspace_size);
    }
    if (output_capacity < requirements.output_capacity_required) {
        return avifenc_fail(error, AVIFENC_OUTPUT_TOO_SMALL,
                            AVIFENC_CONTEXT_OUTPUT,
                            requirements.output_capacity_required,
                            output_capacity);
    }
    if (output == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_OUTPUT,
                            requirements.output_capacity_required,
                            output_capacity);
    }
    return avifenc_fail(error, AVIFENC_UNSUPPORTED,
                        AVIFENC_CONTEXT_IMPLEMENTATION, 0U, 0U);
}