#ifndef AVIFENC_H
#define AVIFENC_H

#include <stddef.h>
#include <stdint.h>

#define AVIFENC_VERSION_MAJOR 0U
#define AVIFENC_VERSION_MINOR 1U
#define AVIFENC_VERSION_PATCH 0U

#define AVIFENC_MAX_DIMENSION 65536U
#define AVIFENC_DEFAULT_QUANTIZER 128U
#define AVIFENC_DEFAULT_SPEED 0U
#define AVIFENC_MAX_SPEED 2U

typedef enum {
    AVIFENC_OK = 0,
    AVIFENC_INVALID_ARGUMENT,
    AVIFENC_OVERFLOW,
    AVIFENC_LIMIT_EXCEEDED,
    AVIFENC_OUT_OF_MEMORY,
    AVIFENC_OUTPUT_TOO_SMALL,
    AVIFENC_UNSUPPORTED
} AvifencStatus;

typedef enum {
    AVIFENC_CONTEXT_NONE = 0,
    AVIFENC_CONTEXT_IMAGE,
    AVIFENC_CONTEXT_DIMENSIONS,
    AVIFENC_CONTEXT_PLANE_Y,
    AVIFENC_CONTEXT_PLANE_U,
    AVIFENC_CONTEXT_PLANE_V,
    AVIFENC_CONTEXT_COLOR,
    AVIFENC_CONTEXT_OPTIONS,
    AVIFENC_CONTEXT_QUANTIZER,
    AVIFENC_CONTEXT_REQUIREMENTS,
    AVIFENC_CONTEXT_WORKSPACE,
    AVIFENC_CONTEXT_OUTPUT,
    AVIFENC_CONTEXT_IMPLEMENTATION
} AvifencErrorContext;

typedef struct {
    AvifencStatus status;
    AvifencErrorContext context;
    size_t required_size;
    size_t provided_size;
} AvifencError;

typedef struct {
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t full_range;
    uint8_t chroma_sample_position;
} AvifencColor;

typedef struct {
    const uint8_t *planes[3];
    size_t strides[3];
    uint32_t width;
    uint32_t height;
    AvifencColor color;
} AvifencImage;

typedef struct {
    uint16_t quantizer;
    /* 0 searches five luma modes, 1 searches three, and 2 uses DC only. */
    uint8_t speed;
} AvifencOptions;

typedef struct {
    size_t workspace_required;
    size_t output_capacity_required;
} AvifencRequirements;

typedef struct {
    /* Stable measurements; timing is intentionally outside the core API. */
    uint64_t tile_count;
    uint64_t partition_node_count;
    uint64_t block_count;
    uint64_t prediction_trial_count;
    uint64_t transform_trial_count;
    uint64_t transform_count;
    uint64_t entropy_symbol_count;
    uint64_t literal_bit_count;
    uint64_t filter_unit_count;
    uint64_t reconstruction_checksum[3];
} AvifencStatistics;

const char *avifenc_version_string(void);
const char *avifenc_status_string(AvifencStatus status);
const char *avifenc_error_context_string(AvifencErrorContext context);
void avifenc_options_default(AvifencOptions *options);

/*
 * Validate an 8-bit 4:2:0 image and return conservative caller-owned buffer
 * requirements. Both dimensions must be nonzero and even. Plane strides are
 * measured in bytes.
 */
AvifencStatus avifenc_query(const AvifencImage *image,
                            const AvifencOptions *options,
                            AvifencRequirements *requirements,
                            AvifencError *error);

/*
 * Encode one reduced-still-picture AVIF image. Capacities must meet the values
 * returned by avifenc_query() for the same image and options. output_written is
 * set to zero on failure and to the exact encoded byte count on success.
 */
AvifencStatus avifenc_encode(const AvifencImage *image,
                             const AvifencOptions *options,
                             void *workspace,
                             size_t workspace_size,
                             void *output,
                             size_t output_capacity,
                             size_t *output_written,
                             AvifencError *error);

/*
 * Encode while reporting deterministic work counts. Statistics are cleared on
 * entry and are valid after a successful call. Passing null disables reporting.
 */
AvifencStatus avifenc_encode_ex(const AvifencImage *image,
                                const AvifencOptions *options,
                                void *workspace,
                                size_t workspace_size,
                                void *output,
                                size_t output_capacity,
                                size_t *output_written,
                                AvifencStatistics *statistics,
                                AvifencError *error);

#endif