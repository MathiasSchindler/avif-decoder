#include "encoder/avifenc.h"
#include "encoder/av1_tile_write.h"
#include "encoder/av1_write.h"
#include "encoder/avif_write.h"
#include "base.h"

#define AVIFENC_TEMP_FIXED_ALLOWANCE 4096U
#define AVIFENC_TEMP_BYTES_PER_PIXEL 16U

typedef struct {
    AvifencAv1TileSource source;
    AvifencAv1TileReconstruction reconstruction;
    AvifencAv1TileRequirements requirements;
    AvifencAv1TilePayload payload;
    size_t payload_capacity;
    AvifencStatistics statistics;
    AvifencStatus status;
} AvifencTileJob;

typedef struct {
    AvifencAv1TileSource tile_source;
    AvifencAv1TileRequirements tile_requirements;
    AvifencAv1TileLayout tile_layout;
    AvifencAv1TileReconstruction reconstruction;
    AvifencTileJob *tile_jobs;
    AvifencAv1TilePayload *tile_payloads;
    size_t tile_count;
    size_t worker_count;
    void *tile_workspace;
    size_t tile_workspace_stride;
    uint8_t *tile_payload;
    size_t tile_payload_capacity;
    uint8_t *av1_payload;
    size_t av1_payload_capacity;
    size_t workspace_required;
    size_t output_capacity_required;
    uint8_t level;
} AvifencAssembly;

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
    return "0.2.0";
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
        case AVIFENC_CONTEXT_EXECUTOR: return "executor";
        case AVIFENC_CONTEXT_QUANTIZATION: return "quantization";
        case AVIFENC_CONTEXT_RATE_CONTROL: return "rate control";
    }
    return "unknown context";
}

void avifenc_options_default(AvifencOptions *options) {
    if (options != 0) {
        avifdec_memory_fill(options, 0U, sizeof(*options));
        options->quantizer = AVIFENC_DEFAULT_QUANTIZER;
        options->speed = AVIFENC_DEFAULT_SPEED;
    }
}

static uint8_t avifenc_activity_matrix_level(const uint8_t *plane,
                                             size_t stride,
                                             uint32_t width,
                                             uint32_t height) {
    uint64_t activity = 0U;
    uint64_t edges = 0U;
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint8_t sample = plane[(size_t)row * stride + column];

            if (column != 0U) {
                uint8_t previous = plane[(size_t)row * stride + column - 1U];
                activity += sample > previous ? sample - previous
                                              : previous - sample;
                ++edges;
            }
            if (row != 0U) {
                uint8_t previous = plane[(size_t)(row - 1U) * stride + column];
                activity += sample > previous ? sample - previous
                                              : previous - sample;
                ++edges;
            }
        }
    }
    if (edges == 0U) return 8U;
    activity /= edges;
    if (activity < 4U) return 10U;
    if (activity < 12U) return 8U;
    if (activity < 32U) return 6U;
    return 4U;
}

static AvifencStatus avifenc_options_resolve(
    const AvifencImage *image,
    const AvifencOptions *options,
    AvifencOptions *resolved,
    AvifencError *error) {
    unsigned int plane;

    *resolved = *options;
    if (options->rate_control.mode > 2U ||
        options->rate_control.target_quality > AVIFENC_TARGET_QUALITY_MAX ||
        (options->rate_control.mode == 2U &&
         options->rate_control.target_size == 0U)) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_RATE_CONTROL,
                            options->rate_control.mode == 1U
                                ? AVIFENC_TARGET_QUALITY_MAX : 1U,
                            options->rate_control.mode == 1U
                                ? options->rate_control.target_quality
                                : options->rate_control.target_size);
    }
    if (options->quantization.matrix_mode > 2U ||
        options->quantization.adaptive_quantization > 1U ||
        options->quantization.aq_strength > 63U ||
        options->quantization.delta_q_y_dc < -64 ||
        options->quantization.delta_q_y_dc > 63 ||
        options->quantization.delta_q_u_dc < -64 ||
        options->quantization.delta_q_u_dc > 63 ||
        options->quantization.delta_q_u_ac < -64 ||
        options->quantization.delta_q_u_ac > 63 ||
        options->quantization.delta_q_v_dc < -64 ||
        options->quantization.delta_q_v_dc > 63 ||
        options->quantization.delta_q_v_ac < -64 ||
        options->quantization.delta_q_v_ac > 63) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_QUANTIZATION, 0U, 0U);
    }
    if (options->quantization.matrix_mode == 1U) {
        for (plane = 0U; plane < 3U; ++plane) {
            if (options->quantization.matrix_levels[plane] > 14U) {
                return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                                    AVIFENC_CONTEXT_QUANTIZATION, 14U,
                                    options->quantization.matrix_levels[plane]);
            }
        }
    } else if (options->quantization.matrix_mode == 2U) {
        for (plane = 0U; plane < 3U; ++plane) {
            resolved->quantization.matrix_levels[plane] =
                avifenc_activity_matrix_level(
                    image->planes[plane], image->strides[plane],
                    plane == 0U ? image->width : image->width / 2U,
                    plane == 0U ? image->height : image->height / 2U);
        }
        resolved->quantization.matrix_mode = 1U;
    }
    if (options->quantizer == 0U &&
        (resolved->quantization.delta_q_y_dc != 0 ||
         resolved->quantization.delta_q_u_dc != 0 ||
         resolved->quantization.delta_q_u_ac != 0 ||
         resolved->quantization.delta_q_v_dc != 0 ||
         resolved->quantization.delta_q_v_ac != 0 ||
         resolved->quantization.matrix_mode != 0U ||
         resolved->quantization.adaptive_quantization != 0U)) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_QUANTIZATION, 0U, 0U);
    }
    return AVIFENC_OK;
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

static uint64_t avifenc_reconstruction_checksum(const uint16_t *plane,
                                                size_t stride,
                                                uint32_t width,
                                                uint32_t height) {
    uint64_t checksum = 1469598103934665603ULL;
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            checksum ^= plane[(size_t)row * stride + column];
            checksum *= 1099511628211ULL;
        }
    }
    return checksum;
}

    static uint64_t avifenc_reconstruction_sse(const uint8_t *source,
                                               size_t source_stride,
                                               const uint16_t *reconstruction,
                                               size_t reconstruction_stride,
                                               uint32_t width,
                                               uint32_t height) {
        uint64_t distortion = 0U;
        uint32_t row;
        uint32_t column;

        for (row = 0U; row < height; ++row) {
            for (column = 0U; column < width; ++column) {
                int32_t difference =
                    (int32_t)source[(size_t)row * source_stride + column] -
                    (int32_t)reconstruction[
                        (size_t)row * reconstruction_stride + column];

                distortion += (uint64_t)(difference * difference);
            }
        }
        return distortion;
    }

    static uint16_t avifenc_quality_score(const AvifencStatistics *statistics,
                                          uint32_t width,
                                          uint32_t height) {
        uint64_t weighted = statistics->reconstruction_sse[0] +
            2U * (statistics->reconstruction_sse[1] +
                  statistics->reconstruction_sse[2]);
        uint64_t denominator = 2U * (uint64_t)width * height;
        uint64_t mse = denominator == 0U ? 65025U : weighted / denominator;

        if (mse > 65025U) mse = 65025U;
        return (uint16_t)(((65025U - mse) * AVIFENC_TARGET_QUALITY_MAX) /
                          65025U);
    }
static int avifenc_executor_valid(const AvifencExecutor *executor) {
    return executor == 0 ||
        (executor->parallel_for != 0 && executor->worker_count != 0U &&
         executor->worker_count <= AVIFENC_EXECUTOR_MAX_WORKERS);
}

static void avifenc_tile_source_init(
    const AvifencImage *image,
    const AvifencOptions *options,
    const AvifencAv1TileLayout *layout,
    size_t tile_index,
    AvifencAv1TileSource *source,
    uint32_t *x_offset,
    uint32_t *y_offset) {
    uint32_t tile_column = (uint32_t)(tile_index % layout->columns);
    uint32_t tile_row = (uint32_t)(tile_index / layout->columns);
    uint32_t x = tile_column * layout->tile_width_sb * 64U;
    uint32_t y = tile_row * layout->tile_height_sb * 64U;
    uint32_t tile_width = layout->tile_width_sb * 64U;
    uint32_t tile_height = layout->tile_height_sb * 64U;

    if (tile_width > image->width - x) tile_width = image->width - x;
    if (tile_height > image->height - y) tile_height = image->height - y;
    source->planes[0] = image->planes[0] + (size_t)y * image->strides[0] + x;
    source->planes[1] = image->planes[1] +
        (size_t)(y / 2U) * image->strides[1] + x / 2U;
    source->planes[2] = image->planes[2] +
        (size_t)(y / 2U) * image->strides[2] + x / 2U;
    source->strides[0] = image->strides[0];
    source->strides[1] = image->strides[1];
    source->strides[2] = image->strides[2];
    source->width = tile_width;
    source->height = tile_height;
    source->quantizer = options->quantizer;
    source->speed = options->speed;
    source->statistics = 0;
    source->quantization = options->quantization;
    *x_offset = x;
    *y_offset = y;
}

static AvifencStatus avifenc_assembly_layout(
    const AvifencImage *image,
    const AvifencOptions *options,
    size_t requested_workers,
    AvifdecArena *arena,
    AvifencAssembly *assembly) {
    static const uint8_t placeholder = 0U;
    AvifencAv1Config av1_config;
    AvifencAvifConfig avif_config;
    AvifencByteWriter sizing;
    size_t pixel_count;
    size_t temporary_capacity;
    size_t av1_overhead;
    size_t tile_payload_total = 0U;
    size_t tile_workspace_max = 0U;
    size_t tile_index;
    unsigned int plane;
    AvifencStatus status;

    assembly->tile_source.planes[0] = image->planes[0];
    assembly->tile_source.planes[1] = image->planes[1];
    assembly->tile_source.planes[2] = image->planes[2];
    assembly->tile_source.strides[0] = image->strides[0];
    assembly->tile_source.strides[1] = image->strides[1];
    assembly->tile_source.strides[2] = image->strides[2];
    assembly->tile_source.width = image->width;
    assembly->tile_source.height = image->height;
    assembly->tile_source.quantizer = options->quantizer;
    assembly->tile_source.speed = options->speed;
    assembly->tile_source.quantization = options->quantization;
    status = avifenc_av1_tile_layout(
        image->width, image->height, &assembly->tile_layout);
    if (status != AVIFENC_OK) return status;
    assembly->tile_count =
        (size_t)assembly->tile_layout.columns * assembly->tile_layout.rows;
    assembly->worker_count = requested_workers;
    status = avifenc_av1_tile_query(
        &assembly->tile_source, &assembly->tile_requirements);
    if (status != AVIFENC_OK) return status;
    if (!avifdec_size_multiply(image->width, image->height, &pixel_count) ||
        !avifdec_size_multiply(
            pixel_count, AVIFENC_TEMP_BYTES_PER_PIXEL, &temporary_capacity) ||
        !avifdec_size_add(
            temporary_capacity, AVIFENC_TEMP_FIXED_ALLOWANCE,
            &temporary_capacity)) {
        return AVIFENC_OVERFLOW;
    }
    assembly->tile_payload_capacity = temporary_capacity;
    av1_config.width = image->width;
    av1_config.height = image->height;
    av1_config.color = image->color;
    av1_config.quantizer = options->quantizer;
    av1_config.quantization = options->quantization;
    if (assembly->tile_count == 1U) {
        avifenc_byte_writer_init_sizing(&sizing);
        status = avifenc_av1_write_with_tile(
            &sizing, &av1_config, &placeholder, 1U);
        if (status != AVIFENC_OK) return status;
        if (avifenc_byte_writer_size(&sizing) < 1U) return AVIFENC_OVERFLOW;
        av1_overhead = avifenc_byte_writer_size(&sizing) - 1U;
        if (!avifdec_size_add(
                temporary_capacity, av1_overhead,
                &assembly->av1_payload_capacity)) {
            return AVIFENC_OVERFLOW;
        }
    } else {
        for (tile_index = 0U;
             tile_index < assembly->tile_count; ++tile_index) {
            AvifencAv1TileSource tile_source;
            AvifencAv1TileRequirements tile_requirements;
            uint32_t x_offset;
            uint32_t y_offset;
            size_t tile_pixels;
            size_t tile_capacity;

            avifenc_tile_source_init(
                image, options, &assembly->tile_layout, tile_index,
                &tile_source, &x_offset, &y_offset);
            status = avifenc_av1_tile_query(
                &tile_source, &tile_requirements);
            if (status != AVIFENC_OK) return status;
            if (tile_requirements.workspace_required > tile_workspace_max) {
                tile_workspace_max = tile_requirements.workspace_required;
            }
            if (!avifdec_size_multiply(
                    tile_source.width, tile_source.height, &tile_pixels) ||
                !avifdec_size_multiply(
                    tile_pixels, AVIFENC_TEMP_BYTES_PER_PIXEL,
                    &tile_capacity) ||
                !avifdec_size_add(
                    tile_capacity, AVIFENC_TEMP_FIXED_ALLOWANCE,
                    &tile_capacity) ||
                !avifdec_size_add(
                    tile_payload_total, tile_capacity,
                    &tile_payload_total)) {
                return AVIFENC_OVERFLOW;
            }
        }
        assembly->tile_workspace_stride = tile_workspace_max;
        assembly->tile_layout.tile_size_bytes = 4U;
        if (!avifdec_size_multiply(
                assembly->tile_count - 1U, 4U, &av1_overhead) ||
            !avifdec_size_add(av1_overhead, 1U, &av1_overhead) ||
            !avifdec_size_add(
                av1_overhead, AVIFENC_TEMP_FIXED_ALLOWANCE, &av1_overhead) ||
            !avifdec_size_add(
                tile_payload_total, av1_overhead,
                &assembly->av1_payload_capacity)) {
            return AVIFENC_OVERFLOW;
        }
    }
    for (plane = 0U; plane < 3U; ++plane) {
        size_t samples;
        size_t bytes;

        assembly->reconstruction.widths[plane] =
            assembly->tile_requirements.reconstruction_widths[plane];
        assembly->reconstruction.heights[plane] =
            assembly->tile_requirements.reconstruction_heights[plane];
        assembly->reconstruction.strides[plane] =
            assembly->reconstruction.widths[plane];
        if (!avifdec_size_multiply(
                assembly->reconstruction.widths[plane],
                assembly->reconstruction.heights[plane], &samples) ||
            !avifdec_size_multiply(samples, sizeof(uint16_t), &bytes)) {
            return AVIFENC_OVERFLOW;
        }
        assembly->reconstruction.planes[plane] =
            (uint16_t *)avifdec_arena_allocate(
                arena, bytes, _Alignof(uint16_t));
    }
    if (assembly->tile_count == 1U) {
        assembly->tile_workspace = avifdec_arena_allocate(
            arena, assembly->tile_requirements.workspace_required, 1U);
        assembly->tile_payload = (uint8_t *)avifdec_arena_allocate(
            arena, assembly->tile_payload_capacity, 1U);
    } else {
        size_t scratch_bytes;

        assembly->tile_jobs = (AvifencTileJob *)avifdec_arena_allocate(
            arena, assembly->tile_count * sizeof(*assembly->tile_jobs),
            _Alignof(AvifencTileJob));
        assembly->tile_payloads =
            (AvifencAv1TilePayload *)avifdec_arena_allocate(
                arena,
                assembly->tile_count * sizeof(*assembly->tile_payloads),
                _Alignof(AvifencAv1TilePayload));
        if (!avifdec_size_multiply(
                assembly->tile_workspace_stride, assembly->worker_count,
                &scratch_bytes)) {
            return AVIFENC_OVERFLOW;
        }
        assembly->tile_workspace = avifdec_arena_allocate(
            arena, scratch_bytes, 1U);
        for (tile_index = 0U;
             tile_index < assembly->tile_count; ++tile_index) {
            AvifencAv1TileSource tile_source;
            AvifencAv1TileRequirements tile_requirements;
            uint32_t x_offset;
            uint32_t y_offset;
            size_t tile_pixels;
            size_t tile_capacity;
            uint8_t *payload;

            avifenc_tile_source_init(
                image, options, &assembly->tile_layout, tile_index,
                &tile_source, &x_offset, &y_offset);
            status = avifenc_av1_tile_query(
                &tile_source, &tile_requirements);
            if (status != AVIFENC_OK) return status;
            if (!avifdec_size_multiply(
                    tile_source.width, tile_source.height, &tile_pixels) ||
                !avifdec_size_multiply(
                    tile_pixels, AVIFENC_TEMP_BYTES_PER_PIXEL,
                    &tile_capacity) ||
                !avifdec_size_add(
                    tile_capacity, AVIFENC_TEMP_FIXED_ALLOWANCE,
                    &tile_capacity)) {
                return AVIFENC_OVERFLOW;
            }
            payload = (uint8_t *)avifdec_arena_allocate(
                arena, tile_capacity, 1U);
            if (!arena->sizing_only && assembly->tile_jobs != 0 &&
                assembly->tile_payloads != 0) {
                AvifencTileJob *job = &assembly->tile_jobs[tile_index];

                avifdec_memory_fill(job, 0U, sizeof(*job));
                job->source = tile_source;
                job->requirements = tile_requirements;
                job->payload.data = payload;
                job->payload_capacity = tile_capacity;
                for (plane = 0U; plane < 3U; ++plane) {
                    uint32_t plane_x = plane == 0U
                        ? x_offset : x_offset / 2U;
                    uint32_t plane_y = plane == 0U
                        ? y_offset : y_offset / 2U;

                    job->reconstruction.planes[plane] =
                        assembly->reconstruction.planes[plane] +
                        (size_t)plane_y *
                            assembly->reconstruction.strides[plane] +
                        plane_x;
                    job->reconstruction.strides[plane] =
                        assembly->reconstruction.strides[plane];
                    job->reconstruction.widths[plane] =
                        tile_requirements.reconstruction_widths[plane];
                    job->reconstruction.heights[plane] =
                        tile_requirements.reconstruction_heights[plane];
                }
                assembly->tile_payloads[tile_index] = job->payload;
            }
        }
    }
    assembly->av1_payload = (uint8_t *)avifdec_arena_allocate(
        arena, assembly->av1_payload_capacity, 1U);
    if (arena->status == AVIFDEC_OVERFLOW) return AVIFENC_OVERFLOW;
    if (arena->status != AVIFDEC_OK) return AVIFENC_OUT_OF_MEMORY;
    assembly->workspace_required = avifdec_arena_required(arena);
    if (!avifdec_size_add(
            assembly->workspace_required,
            (assembly->tile_count == 1U
                ? _Alignof(uint16_t) : _Alignof(AvifencTileJob)) - 1U,
            &assembly->workspace_required)) {
        return AVIFENC_OVERFLOW;
    }

    status = avifenc_av1_select_level(
        image->width, image->height, &assembly->level);
    if (status != AVIFENC_OK) return status;
    avif_config.width = image->width;
    avif_config.height = image->height;
    avif_config.color = image->color;
    avif_config.seq_level_idx_0 = assembly->level;
    avifenc_byte_writer_init_sizing(&sizing);
    status = avifenc_avif_write(
        &sizing, &avif_config, &placeholder,
        assembly->av1_payload_capacity);
    if (status != AVIFENC_OK) return status;
    assembly->output_capacity_required = avifenc_byte_writer_size(&sizing);
    return AVIFENC_OK;
}

AvifencStatus avifenc_query_with_executor(
    const AvifencImage *image,
    const AvifencOptions *options,
    const AvifencExecutor *executor,
    AvifencRequirements *requirements,
    AvifencError *error) {
    AvifencStatus status;
    AvifencAssembly assembly;
    AvifdecArena sizing;
    AvifencOptions resolved;

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
    if (!avifenc_executor_valid(executor)) {
        return avifenc_fail(
            error, AVIFENC_INVALID_ARGUMENT, AVIFENC_CONTEXT_EXECUTOR,
            AVIFENC_EXECUTOR_MAX_WORKERS,
            executor == 0 ? 0U : executor->worker_count);
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
    if (options->speed > AVIFENC_MAX_SPEED) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_OPTIONS,
                            AVIFENC_MAX_SPEED, options->speed);
    }
    if (image->color.full_range > 1U ||
        image->color.chroma_sample_position > 3U ||
        image->color.color_primaries > 255U ||
        image->color.transfer_characteristics > 255U ||
        image->color.matrix_coefficients > 255U ||
        (image->color.color_primaries == 1U &&
         image->color.transfer_characteristics == 13U &&
         image->color.matrix_coefficients == 0U)) {
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

    status = avifenc_options_resolve(image, options, &resolved, error);
    if (status != AVIFENC_OK) return status;

    avifdec_memory_fill(&assembly, 0U, sizeof(assembly));
    avifdec_arena_init_sizing(&sizing);
    status = avifenc_assembly_layout(
        image, &resolved, executor == 0 ? 1U : executor->worker_count,
        &sizing, &assembly);
    if (status != AVIFENC_OK) {
        requirements->workspace_required = 0U;
        requirements->output_capacity_required = 0U;
        return avifenc_fail(
            error, status,
            status == AVIFENC_UNSUPPORTED || status == AVIFENC_LIMIT_EXCEEDED
                ? AVIFENC_CONTEXT_DIMENSIONS
                : AVIFENC_CONTEXT_REQUIREMENTS,
            0U, 0U);
    }
    requirements->workspace_required = assembly.workspace_required;
    requirements->output_capacity_required =
        assembly.output_capacity_required;
    if (resolved.rate_control.mode != 0U &&
        !avifdec_size_add(requirements->workspace_required,
                          requirements->output_capacity_required,
                          &requirements->workspace_required)) {
        requirements->workspace_required = 0U;
        requirements->output_capacity_required = 0U;
        return avifenc_fail(error, AVIFENC_OVERFLOW,
                            AVIFENC_CONTEXT_REQUIREMENTS, 0U, 0U);
    }
    return AVIFENC_OK;
}

AvifencStatus avifenc_query(const AvifencImage *image,
                            const AvifencOptions *options,
                            AvifencRequirements *requirements,
                            AvifencError *error) {
    return avifenc_query_with_executor(
        image, options, 0, requirements, error);
}

typedef struct {
    AvifencAssembly *assembly;
    int collect_statistics;
} AvifencParallelTiles;

static AvifencStatus avifenc_encode_tile_range(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg) {
    AvifencParallelTiles *parallel = (AvifencParallelTiles *)arg;
    size_t tile_index;

    if (parallel == 0 || parallel->assembly == 0 ||
        worker_index >= parallel->assembly->worker_count ||
        begin > end || end > parallel->assembly->tile_count) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    for (tile_index = begin; tile_index < end; ++tile_index) {
        AvifencTileJob *job = &parallel->assembly->tile_jobs[tile_index];
        AvifencAv1SymbolWriter symbol_writer;
        uint8_t *worker_workspace =
            (uint8_t *)parallel->assembly->tile_workspace +
            worker_index * parallel->assembly->tile_workspace_stride;

        avifdec_memory_fill(&job->statistics, 0U, sizeof(job->statistics));
        job->source.statistics = parallel->collect_statistics
            ? &job->statistics : 0;
        avifenc_av1_symbol_writer_init(
            &symbol_writer, (void *)job->payload.data,
            job->payload_capacity, 1);
        job->status = avifenc_av1_tile_write(
            &symbol_writer, &job->source, &job->reconstruction,
            worker_workspace, job->requirements.workspace_required);
        if (job->status == AVIFENC_OK) {
            job->payload.size =
                avifenc_av1_symbol_writer_size(&symbol_writer);
            parallel->assembly->tile_payloads[tile_index] = job->payload;
            if (parallel->collect_statistics) {
                job->statistics.entropy_symbol_count =
                    symbol_writer.symbol_count;
                job->statistics.literal_bit_count =
                    symbol_writer.literal_bit_count;
            }
        }
    }
    return AVIFENC_OK;
}

static void avifenc_statistics_add(AvifencStatistics *total,
                                   const AvifencStatistics *part) {
    total->tile_count += part->tile_count;
    total->partition_node_count += part->partition_node_count;
    total->block_count += part->block_count;
    total->prediction_trial_count += part->prediction_trial_count;
    total->transform_trial_count += part->transform_trial_count;
    total->transform_count += part->transform_count;
    total->entropy_symbol_count += part->entropy_symbol_count;
    total->literal_bit_count += part->literal_bit_count;
    total->filter_unit_count += part->filter_unit_count;
}

static AvifencStatus avifenc_encode_single_pass(
    const AvifencImage *image,
    const AvifencOptions *options,
    const AvifencExecutor *executor,
    void *workspace,
    size_t workspace_size,
    void *output,
    size_t output_capacity,
    size_t *output_written,
    AvifencStatistics *statistics,
    AvifencError *error) {
    AvifencRequirements requirements;
    AvifencAssembly assembly;
    AvifencAv1SymbolWriter symbol_writer;
    AvifencAv1Config av1_config;
    AvifencAvifConfig avif_config;
    AvifencByteWriter byte_writer;
    AvifdecArena arena;
    size_t tile_payload_size;
    size_t av1_payload_size;
    AvifencStatus status;
    AvifencOptions resolved;

    avifenc_error_reset(error);
    if (statistics != 0) {
        avifdec_memory_fill(statistics, 0U, sizeof(*statistics));
    }
    if (output_written == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_OUTPUT, 1U, 0U);
    }
    *output_written = 0U;
    status = avifenc_query_with_executor(
        image, options, executor, &requirements, error);
    if (status != AVIFENC_OK) return status;
    status = avifenc_options_resolve(image, options, &resolved, error);
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
    avifdec_memory_fill(&assembly, 0U, sizeof(assembly));
    avifdec_arena_init(&arena, workspace, workspace_size);
    status = avifenc_assembly_layout(
        image, &resolved, executor == 0 ? 1U : executor->worker_count,
        &arena, &assembly);
    if (status != AVIFENC_OK) {
        return avifenc_fail(error, status, AVIFENC_CONTEXT_WORKSPACE,
                            requirements.workspace_required, workspace_size);
    }
    if (assembly.tile_count == 1U) {
        avifenc_av1_symbol_writer_init(
            &symbol_writer, assembly.tile_payload,
            assembly.tile_payload_capacity, 1);
        assembly.tile_source.statistics = statistics;
        status = avifenc_av1_tile_write(
            &symbol_writer, &assembly.tile_source, &assembly.reconstruction,
            assembly.tile_workspace,
            assembly.tile_requirements.workspace_required);
        if (status != AVIFENC_OK) {
            return avifenc_fail(
                error, status, AVIFENC_CONTEXT_IMPLEMENTATION,
                assembly.tile_payload_capacity,
                avifenc_av1_symbol_writer_size(&symbol_writer));
        }
        if (statistics != 0) {
            statistics->entropy_symbol_count = symbol_writer.symbol_count;
            statistics->literal_bit_count = symbol_writer.literal_bit_count;
        }
        tile_payload_size = avifenc_av1_symbol_writer_size(&symbol_writer);
    } else {
        AvifencParallelTiles parallel;
        size_t tile_index;
        size_t maximum_size = 0U;

        parallel.assembly = &assembly;
        parallel.collect_statistics = statistics != 0;
        if (executor != 0 && executor->worker_count > 1U) {
            status = executor->parallel_for(
                executor->user_data, assembly.tile_count, 1U,
                avifenc_encode_tile_range, &parallel);
        } else {
            status = avifenc_encode_tile_range(
                0U, assembly.tile_count, 0U, &parallel);
        }
        if (status != AVIFENC_OK) {
            return avifenc_fail(
                error, status, AVIFENC_CONTEXT_EXECUTOR,
                assembly.tile_count, 0U);
        }
        for (tile_index = 0U;
             tile_index < assembly.tile_count; ++tile_index) {
            AvifencTileJob *job = &assembly.tile_jobs[tile_index];

            if (job->status != AVIFENC_OK) {
                return avifenc_fail(
                    error, job->status, AVIFENC_CONTEXT_IMPLEMENTATION,
                    job->payload_capacity, job->payload.size);
            }
            if (job->payload.size > maximum_size) {
                maximum_size = job->payload.size;
            }
            if (statistics != 0) {
                avifenc_statistics_add(statistics, &job->statistics);
            }
        }
        assembly.tile_layout.tile_size_bytes = 1U;
        maximum_size = maximum_size == 0U ? 0U : maximum_size - 1U;
        while (maximum_size > 255U) {
            ++assembly.tile_layout.tile_size_bytes;
            maximum_size >>= 8U;
        }
        if (assembly.tile_layout.tile_size_bytes > 4U) {
            return avifenc_fail(
                error, AVIFENC_LIMIT_EXCEEDED,
                AVIFENC_CONTEXT_IMPLEMENTATION, 4U,
                assembly.tile_layout.tile_size_bytes);
        }
    }
    if (statistics != 0) {
        unsigned int plane;

        for (plane = 0U; plane < 3U; ++plane) {
            uint32_t plane_width =
                plane == 0U ? image->width : image->width / 2U;
            uint32_t plane_height =
                plane == 0U ? image->height : image->height / 2U;

            statistics->reconstruction_checksum[plane] =
                avifenc_reconstruction_checksum(
                    assembly.reconstruction.planes[plane],
                    assembly.reconstruction.strides[plane],
                    plane_width, plane_height);
            statistics->reconstruction_sse[plane] =
                avifenc_reconstruction_sse(
                    image->planes[plane], image->strides[plane],
                    assembly.reconstruction.planes[plane],
                    assembly.reconstruction.strides[plane],
                    plane_width, plane_height);
        }
        statistics->selected_quantizer = resolved.quantizer;
        statistics->achieved_quality = avifenc_quality_score(
            statistics, image->width, image->height);
        statistics->encode_pass_count = 1U;
    }
    av1_config.width = image->width;
    av1_config.height = image->height;
    av1_config.color = image->color;
    av1_config.quantizer = options->quantizer;
    av1_config.quantization = resolved.quantization;
    avifenc_byte_writer_init(
        &byte_writer, assembly.av1_payload, assembly.av1_payload_capacity);
    if (assembly.tile_count == 1U) {
        status = avifenc_av1_write_with_tile(
            &byte_writer, &av1_config,
            assembly.tile_payload, tile_payload_size);
    } else {
        status = avifenc_av1_write_with_tiles(
            &byte_writer, &av1_config, &assembly.tile_layout,
            assembly.tile_payloads, assembly.tile_count);
    }
    if (status != AVIFENC_OK) {
        return avifenc_fail(error, status, AVIFENC_CONTEXT_IMPLEMENTATION,
                            assembly.av1_payload_capacity,
                            avifenc_byte_writer_size(&byte_writer));
    }
    av1_payload_size = avifenc_byte_writer_size(&byte_writer);
    avif_config.width = image->width;
    avif_config.height = image->height;
    avif_config.color = image->color;
    avif_config.seq_level_idx_0 = assembly.level;
    avifenc_byte_writer_init(&byte_writer, output, output_capacity);
    status = avifenc_avif_write(
        &byte_writer, &avif_config,
        assembly.av1_payload, av1_payload_size);
    if (status != AVIFENC_OK) {
        return avifenc_fail(error, status, AVIFENC_CONTEXT_OUTPUT,
                            requirements.output_capacity_required,
                            output_capacity);
    }
    *output_written = avifenc_byte_writer_size(&byte_writer);
    return AVIFENC_OK;
}

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
    AvifencError *error) {
    static const uint8_t pass_caps[3] = { 9U, 7U, 5U };
    AvifencOptions trial_options;
    AvifencStatistics trial_statistics;
    AvifencStatistics final_statistics;
    AvifencRequirements base_requirements;
    AvifencRequirements rate_requirements;
    uint8_t *trial_output;
    uint16_t low;
    uint16_t high;
    uint16_t selected = 0U;
    uint16_t under_quantizer = 0U;
    uint16_t over_quantizer = 0U;
    size_t under_size = 0U;
    size_t over_size = 0U;
    size_t trial_written = 0U;
    uint8_t pass_count = 0U;
    uint8_t pass_cap;
    int have_selected = 0;
    AvifencStatus status;

    if (options == 0 || options->rate_control.mode == 0U) {
        return avifenc_encode_single_pass(
            image, options, executor, workspace, workspace_size,
            output, output_capacity, output_written, statistics, error);
    }
    if (options->speed > AVIFENC_MAX_SPEED) {
        return avifenc_encode_single_pass(
            image, options, executor, workspace, workspace_size,
            output, output_capacity, output_written, statistics, error);
    }
    trial_options = *options;
    trial_options.rate_control.mode = 0U;
    trial_options.rate_control.target_quality = 0U;
    trial_options.rate_control.target_size = 0U;
    status = avifenc_query_with_executor(
        image, options, executor, &rate_requirements, error);
    if (status != AVIFENC_OK) return status;
    if (output_written == 0) {
        return avifenc_fail(error, AVIFENC_INVALID_ARGUMENT,
                            AVIFENC_CONTEXT_OUTPUT, 1U, 0U);
    }
    *output_written = 0U;
    if (output_capacity < rate_requirements.output_capacity_required) {
        return avifenc_fail(
            error, AVIFENC_OUTPUT_TOO_SMALL, AVIFENC_CONTEXT_OUTPUT,
            rate_requirements.output_capacity_required, output_capacity);
    }
    if (output == 0) {
        return avifenc_fail(
            error, AVIFENC_INVALID_ARGUMENT, AVIFENC_CONTEXT_OUTPUT,
            rate_requirements.output_capacity_required, output_capacity);
    }
    status = avifenc_query_with_executor(
        image, &trial_options, executor, &base_requirements, error);
    if (status != AVIFENC_OK) return status;
    if (workspace == 0 || workspace_size < rate_requirements.workspace_required) {
        if (output_written != 0) *output_written = 0U;
        return avifenc_fail(
            error, workspace == 0 ? AVIFENC_INVALID_ARGUMENT
                                  : AVIFENC_OUT_OF_MEMORY,
            AVIFENC_CONTEXT_WORKSPACE, rate_requirements.workspace_required,
            workspace_size);
    }
    trial_output = (uint8_t *)workspace + base_requirements.workspace_required;
    pass_cap = pass_caps[options->speed];
    low = options->rate_control.mode == 1U &&
          options->quantization.matrix_mode == 0U &&
          options->quantization.adaptive_quantization == 0U &&
          options->quantization.delta_q_y_dc == 0 &&
          options->quantization.delta_q_u_dc == 0 &&
          options->quantization.delta_q_u_ac == 0 &&
          options->quantization.delta_q_v_dc == 0 &&
          options->quantization.delta_q_v_ac == 0 ? 0U : 1U;
    high = 255U;

    if (options->rate_control.mode == 2U) {
        trial_options.quantizer = 255U;
        status = avifenc_encode_single_pass(
            image, &trial_options, executor, workspace,
            base_requirements.workspace_required,
            trial_output, base_requirements.output_capacity_required,
            &trial_written, &trial_statistics,
            error);
        if (status != AVIFENC_OK) return status;
        ++pass_count;
        if (trial_written > options->rate_control.target_size) {
            if (output_written != 0) *output_written = 0U;
            return avifenc_fail(
                error, AVIFENC_LIMIT_EXCEEDED,
                AVIFENC_CONTEXT_RATE_CONTROL,
                trial_written, options->rate_control.target_size);
        }
        selected = 255U;
        under_quantizer = 255U;
        under_size = trial_written;
        have_selected = 1;
    } else {
        trial_options.quantizer = low;
        status = avifenc_encode_single_pass(
            image, &trial_options, executor, workspace,
            base_requirements.workspace_required,
            trial_output, base_requirements.output_capacity_required,
            &trial_written, &trial_statistics,
            error);
        if (status != AVIFENC_OK) return status;
        ++pass_count;
        if (trial_statistics.achieved_quality <
            options->rate_control.target_quality) {
            if (output_written != 0) *output_written = 0U;
            return avifenc_fail(
                error, AVIFENC_LIMIT_EXCEEDED,
                AVIFENC_CONTEXT_RATE_CONTROL,
                options->rate_control.target_quality,
                trial_statistics.achieved_quality);
        }
        selected = low;
        have_selected = 1;
    }

    while (low <= high && pass_count + 1U < pass_cap) {
        uint16_t middle = (uint16_t)(low + (high - low) / 2U);
        int meets_target;

        if (options->rate_control.mode == 2U && over_size > under_size &&
            over_quantizer + 1U < under_quantizer) {
            size_t scaled;
            size_t size_span = over_size - under_size;
            size_t target_span = over_size -
                options->rate_control.target_size;
            size_t quantizer_span = under_quantizer - over_quantizer;

            if (avifdec_size_multiply(
                    target_span, quantizer_span, &scaled)) {
                size_t offset = scaled / size_span;

                if (scaled % size_span != 0U) ++offset;
                if (offset != 0U && offset < quantizer_span) {
                    uint16_t interpolated = (uint16_t)(
                        over_quantizer + offset);

                    if (interpolated >= low && interpolated <= high) {
                        middle = interpolated;
                    }
                }
            }
        }

        trial_options.quantizer = middle;
        status = avifenc_encode_single_pass(
            image, &trial_options, executor, workspace,
            base_requirements.workspace_required,
            trial_output, base_requirements.output_capacity_required,
            &trial_written, &trial_statistics,
            error);
        if (status != AVIFENC_OK) return status;
        ++pass_count;
        meets_target = options->rate_control.mode == 1U
            ? trial_statistics.achieved_quality >=
                options->rate_control.target_quality
            : trial_written <= options->rate_control.target_size;
        if (meets_target) {
            selected = middle;
            have_selected = 1;
            if (options->rate_control.mode == 1U) {
                low = (uint16_t)(middle + 1U);
            } else if (middle == 0U) {
                break;
            } else {
                under_quantizer = middle;
                under_size = trial_written;
                high = (uint16_t)(middle - 1U);
            }
        } else if (options->rate_control.mode == 1U) {
            if (middle == 0U) break;
            high = (uint16_t)(middle - 1U);
        } else {
            over_quantizer = middle;
            over_size = trial_written;
            low = (uint16_t)(middle + 1U);
        }
    }
    if (!have_selected) {
        if (output_written != 0) *output_written = 0U;
        return avifenc_fail(error, AVIFENC_LIMIT_EXCEEDED,
                            AVIFENC_CONTEXT_RATE_CONTROL, 0U, 0U);
    }
    trial_options.quantizer = selected;
    status = avifenc_encode_single_pass(
        image, &trial_options, executor, workspace,
        base_requirements.workspace_required,
        output, output_capacity, output_written, &final_statistics, error);
    if (status != AVIFENC_OK) return status;
    ++pass_count;
    final_statistics.encode_pass_count = pass_count;
    if (statistics != 0) *statistics = final_statistics;
    return AVIFENC_OK;
}

AvifencStatus avifenc_encode_ex(const AvifencImage *image,
                                const AvifencOptions *options,
                                void *workspace,
                                size_t workspace_size,
                                void *output,
                                size_t output_capacity,
                                size_t *output_written,
                                AvifencStatistics *statistics,
                                AvifencError *error) {
    return avifenc_encode_with_executor(
        image, options, 0, workspace, workspace_size, output,
        output_capacity, output_written, statistics, error);
}

AvifencStatus avifenc_encode(const AvifencImage *image,
                             const AvifencOptions *options,
                             void *workspace,
                             size_t workspace_size,
                             void *output,
                             size_t output_capacity,
                             size_t *output_written,
                             AvifencError *error) {
    return avifenc_encode_ex(
        image, options, workspace, workspace_size, output, output_capacity,
        output_written, 0, error);
}