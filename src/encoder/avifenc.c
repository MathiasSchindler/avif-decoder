#include "encoder/avifenc.h"
#include "encoder/av1_tile_write.h"
#include "encoder/av1_write.h"
#include "encoder/avif_write.h"
#include "base.h"

#define AVIFENC_TEMP_FIXED_ALLOWANCE 4096U
#define AVIFENC_TEMP_BYTES_PER_PIXEL 16U

typedef struct {
    AvifencAv1TileSource tile_source;
    AvifencAv1TileRequirements tile_requirements;
    AvifencAv1TileReconstruction reconstruction;
    void *tile_workspace;
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
    if (options != 0) {
        options->quantizer = AVIFENC_DEFAULT_QUANTIZER;
        options->speed = AVIFENC_DEFAULT_SPEED;
    }
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

static AvifencStatus avifenc_assembly_layout(
    const AvifencImage *image,
    const AvifencOptions *options,
    AvifdecArena *arena,
    AvifencAssembly *assembly) {
    static const uint8_t placeholder = 0U;
    AvifencAv1Config av1_config;
    AvifencAvifConfig avif_config;
    AvifencByteWriter sizing;
    size_t pixel_count;
    size_t temporary_capacity;
    size_t av1_overhead;
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
    assembly->tile_workspace = avifdec_arena_allocate(
        arena, assembly->tile_requirements.workspace_required, 1U);
    assembly->tile_payload = (uint8_t *)avifdec_arena_allocate(
        arena, assembly->tile_payload_capacity, 1U);
    assembly->av1_payload = (uint8_t *)avifdec_arena_allocate(
        arena, assembly->av1_payload_capacity, 1U);
    if (arena->status == AVIFDEC_OVERFLOW) return AVIFENC_OVERFLOW;
    if (arena->status != AVIFDEC_OK) return AVIFENC_OUT_OF_MEMORY;
    assembly->workspace_required = avifdec_arena_required(arena);
    if (!avifdec_size_add(
            assembly->workspace_required, _Alignof(uint16_t) - 1U,
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

AvifencStatus avifenc_query(const AvifencImage *image,
                            const AvifencOptions *options,
                            AvifencRequirements *requirements,
                            AvifencError *error) {
    AvifencStatus status;
    AvifencAssembly assembly;
    AvifdecArena sizing;

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
    if (options->quantizer == 0U) {
        return avifenc_fail(error, AVIFENC_UNSUPPORTED,
                            AVIFENC_CONTEXT_QUANTIZER, 1U, 0U);
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

    avifdec_memory_fill(&assembly, 0U, sizeof(assembly));
    avifdec_arena_init_sizing(&sizing);
    status = avifenc_assembly_layout(image, options, &sizing, &assembly);
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

    avifenc_error_reset(error);
    if (statistics != 0) {
        avifdec_memory_fill(statistics, 0U, sizeof(*statistics));
    }
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
    avifdec_memory_fill(&assembly, 0U, sizeof(assembly));
    avifdec_arena_init(&arena, workspace, workspace_size);
    status = avifenc_assembly_layout(image, options, &arena, &assembly);
    if (status != AVIFENC_OK) {
        return avifenc_fail(error, status, AVIFENC_CONTEXT_WORKSPACE,
                            requirements.workspace_required, workspace_size);
    }
    avifenc_av1_symbol_writer_init(
        &symbol_writer, assembly.tile_payload,
        assembly.tile_payload_capacity, 1);
    assembly.tile_source.statistics = statistics;
    status = avifenc_av1_tile_write(
        &symbol_writer, &assembly.tile_source, &assembly.reconstruction,
        assembly.tile_workspace,
        assembly.tile_requirements.workspace_required);
    if (status != AVIFENC_OK) {
        return avifenc_fail(error, status, AVIFENC_CONTEXT_IMPLEMENTATION,
                            assembly.tile_payload_capacity,
                            avifenc_av1_symbol_writer_size(&symbol_writer));
    }
    if (statistics != 0) {
        unsigned int plane;

        statistics->entropy_symbol_count = symbol_writer.symbol_count;
        statistics->literal_bit_count = symbol_writer.literal_bit_count;
        for (plane = 0U; plane < 3U; ++plane) {
            statistics->reconstruction_checksum[plane] =
                avifenc_reconstruction_checksum(
                    assembly.reconstruction.planes[plane],
                    assembly.reconstruction.strides[plane],
                    plane == 0U ? image->width : image->width / 2U,
                    plane == 0U ? image->height : image->height / 2U);
        }
    }
    tile_payload_size = avifenc_av1_symbol_writer_size(&symbol_writer);
    av1_config.width = image->width;
    av1_config.height = image->height;
    av1_config.color = image->color;
    av1_config.quantizer = options->quantizer;
    avifenc_byte_writer_init(
        &byte_writer, assembly.av1_payload, assembly.av1_payload_capacity);
    status = avifenc_av1_write_with_tile(
        &byte_writer, &av1_config,
        assembly.tile_payload, tile_payload_size);
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