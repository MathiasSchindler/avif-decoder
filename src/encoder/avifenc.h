#ifndef AVIFENC_H
#define AVIFENC_H

#include <stddef.h>
#include <stdint.h>

#define AVIFENC_VERSION_MAJOR 0U
#define AVIFENC_VERSION_MINOR 2U
#define AVIFENC_VERSION_PATCH 0U

#define AVIFENC_MAX_DIMENSION 65536U
#define AVIFENC_DEFAULT_QUANTIZER 128U
#define AVIFENC_DEFAULT_SPEED 0U
#define AVIFENC_MAX_SPEED 2U
#define AVIFENC_EXECUTOR_MAX_WORKERS 32U
#define AVIFENC_TARGET_QUALITY_MAX 10000U

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
    AVIFENC_CONTEXT_IMPLEMENTATION,
    AVIFENC_CONTEXT_EXECUTOR,
    AVIFENC_CONTEXT_QUANTIZATION,
    AVIFENC_CONTEXT_RATE_CONTROL
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
    int8_t delta_q_y_dc;
    int8_t delta_q_u_dc;
    int8_t delta_q_u_ac;
    int8_t delta_q_v_dc;
    int8_t delta_q_v_ac;
    /* 0 disables matrices, 1 uses explicit levels, 2 selects by activity. */
    uint8_t matrix_mode;
    uint8_t matrix_levels[3];
    /* 0 disables spatial adaptation; 1 selects bounded activity AQ. */
    uint8_t adaptive_quantization;
    uint8_t aq_strength;
} AvifencQuantization;

typedef struct {
    /* 0 uses quantizer, 1 targets quality, 2 targets output byte size. */
    uint8_t mode;
    uint16_t target_quality;
    size_t target_size;
} AvifencRateControl;

typedef struct {
    uint16_t quantizer;
    /* Lower values search broader bounded mode, angle, and partition sets. */
    uint8_t speed;
    AvifencQuantization quantization;
    AvifencRateControl rate_control;
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
    uint64_t luma_mode_mask;
    uint64_t chroma_mode_mask;
    uint64_t angle_delta_mask;
    uint64_t cfl_block_count;
    uint64_t filter_intra_block_count;
    uint64_t palette_block_count;
    uint64_t reconstruction_checksum[3];
    uint64_t reconstruction_sse[3];
    uint16_t selected_quantizer;
    uint16_t achieved_quality;
    uint8_t encode_pass_count;
} AvifencStatistics;

typedef AvifencStatus (*AvifencParallelBody)(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg);

typedef AvifencStatus (*AvifencParallelFor)(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifencParallelBody body,
    void *arg);

typedef struct {
    void *user_data;
    size_t worker_count;
    AvifencParallelFor parallel_for;
} AvifencExecutor;

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
AvifencStatus avifenc_query_with_executor(
    const AvifencImage *image,
    const AvifencOptions *options,
    const AvifencExecutor *executor,
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

AvifencStatus avifenc_encode_with_executor(
    const AvifencImage *image,
    const AvifencOptions *options,
    const AvifencExecutor *executor,
    void *workspace,
    size_t workspace_size,
    void *output,
    size_t output_capacity,
    size_t *output_written,
    AvifencStatistics *statistics,
    AvifencError *error);

#endif