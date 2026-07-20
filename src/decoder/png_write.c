#include "png_write.h"
#include "base.h"
#include "platform/platform.h"
#include "png.h"
#include <stddef.h>
#include <stdint.h>

#define CLI_PNG_ROWS_PER_WORKER 8U
#define CLI_PNG_MAX_CACHED_ROWS 64U

typedef struct {
    const AvifdecImage *image;
    const AvifdecImageInfo *info;
    AvifdecRgbImage rgb;
    uint32_t first_row;
    uint32_t error_rows[AVIFDEC_EXECUTOR_MAX_WORKERS];
    AvifdecError errors[AVIFDEC_EXECUTOR_MAX_WORKERS];
} CliRgbRows;

typedef struct {
    const AvifdecImage *image;
    const AvifdecImageInfo *info;
    const AvifdecExecutor *executor;
    unsigned char *cache;
    size_t row_bytes;
    uint32_t cache_start;
    uint32_t cache_count;
    uint32_t cache_capacity;
    uint8_t format;
    AvifdecError *error;
} PngRgbRows;

static int write_png_bytes(void *user_data, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0U;
    int fd = *(int *)user_data;

    while (written < size) {
        long result = platform_write(fd, bytes + written, size - written);

        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static AvifdecStatus convert_rgb_row_ranges(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *user_data) {
    CliRgbRows *rows = (CliRgbRows *)user_data;
    size_t index;

    if (worker_index >= AVIFDEC_EXECUTOR_MAX_WORKERS) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (rows->errors[worker_index].status != AVIFDEC_OK) {
        return rows->errors[worker_index].status;
    }
    for (index = begin; index < end; ++index) {
        AvifdecRgbImage rgb = rows->rgb;
        uint32_t row = rows->first_row + (uint32_t)index;
        AvifdecStatus status;

        rgb.pixels = (unsigned char *)rows->rgb.pixels +
            index * rows->rgb.stride;
        status = avifdec_image_to_rgb_row(
            rows->image, rows->info, &rgb, row,
            &rows->errors[worker_index]);
        if (status != AVIFDEC_OK) {
            rows->error_rows[worker_index] = row;
            return status;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus cli_convert_rgb_rows_parallel(
    const AvifdecExecutor *executor,
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    AvifdecRgbImage *rgb,
    uint32_t first_row,
    uint32_t row_count,
    AvifdecError *error) {
    CliRgbRows rows;
    AvifdecStatus status;
    uint32_t earliest_error_row = UINT32_MAX;
    unsigned int worker_index;

    avifdec_memory_fill(&rows, 0U, sizeof(rows));
    rows.image = image;
    rows.info = info;
    rows.rgb = *rgb;
    rows.first_row = first_row;
    for (worker_index = 0U;
         worker_index < AVIFDEC_EXECUTOR_MAX_WORKERS;
         ++worker_index) {
        rows.error_rows[worker_index] = UINT32_MAX;
    }
    status = executor->parallel_for(
        executor->user_data, row_count, 1U,
        convert_rgb_row_ranges, &rows);
    if (status == AVIFDEC_OK) return AVIFDEC_OK;
    for (worker_index = 0U;
         worker_index < executor->worker_count;
         ++worker_index) {
        if (rows.error_rows[worker_index] < earliest_error_row) {
            earliest_error_row = rows.error_rows[worker_index];
            if (error != 0) *error = rows.errors[worker_index];
        }
    }
    if (error != 0 && earliest_error_row == UINT32_MAX) {
        error->status = status;
        error->offset = 0U;
        error->context = 0U;
    }
    return status;
}

static AvifdecStatus read_png_rgb_row(
    void *user_data, uint32_t row, void *pixels, size_t size) {
    PngRgbRows *rows = (PngRgbRows *)user_data;
    AvifdecRgbImage rgb;
    AvifdecStatus status;

    rgb.width = rows->info->presentation_width;
    rgb.height = rows->info->presentation_height;
    rgb.format = rows->format;
    rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    if (rows->cache == 0) {
        rgb.pixels = pixels;
        rgb.stride = size;
        return avifdec_image_to_rgb_row(
            rows->image, rows->info, &rgb, row, rows->error);
    }
    if (size != rows->row_bytes) return AVIFDEC_INVALID_ARGUMENT;
    if (row < rows->cache_start ||
        row >= rows->cache_start + rows->cache_count) {
        uint32_t count = rows->info->presentation_height - row;

        if (count > rows->cache_capacity) {
            count = rows->cache_capacity;
        }
        rgb.pixels = rows->cache;
        rgb.stride = rows->row_bytes;
        status = cli_convert_rgb_rows_parallel(
            rows->executor, rows->image, rows->info, &rgb,
            row, count, rows->error);
        if (status != AVIFDEC_OK) return status;
        rows->cache_start = row;
        rows->cache_count = count;
    }
    avifdec_memory_copy(
        pixels,
        rows->cache +
            (size_t)(row - rows->cache_start) * rows->row_bytes,
        rows->row_bytes);
    return AVIFDEC_OK;
}

AvifdecStatus cli_write_png_file(
    const char *path,
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecExecutor *executor,
    AvifdecError *error) {
    AvifdecPngMetadata metadata;
    PngRgbRows rows;
    void *row_cache = 0;
    size_t row_cache_size = 0U;
    void *workspace;
    size_t workspace_size;
    uint8_t channels = info->has_alpha ? 4U : 3U;
    uint8_t bit_depth =
        info->bit_depth > 8U ||
        (info->has_alpha && info->alpha_bit_depth > 8U)
            ? 16U : 8U;
    AvifdecStatus status;
    int fd;

    status = avifdec_png_workspace_requirement(
        info->presentation_width, channels, bit_depth,
        &workspace_size);
    if (status != AVIFDEC_OK) return status;
    workspace = platform_allocate_pages(workspace_size);
    if (workspace == 0) return AVIFDEC_OUT_OF_MEMORY;
    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        (void)platform_free_pages(workspace, workspace_size);
        return AVIFDEC_IO_ERROR;
    }
    avifdec_memory_fill(&rows, 0U, sizeof(rows));
    metadata.pixel_aspect_h_spacing = info->pixel_aspect_h_spacing;
    metadata.pixel_aspect_v_spacing = info->pixel_aspect_v_spacing;
    metadata.color_primaries = info->color_primaries;
    metadata.transfer_characteristics = info->transfer_characteristics;
    metadata.has_nclx = info->has_nclx;
    rows.image = image;
    rows.info = info;
    rows.executor = executor;
    rows.format = (uint8_t)(
        channels == 4U
            ? (bit_depth == 16U
                ? AVIFDEC_RGBA16 : AVIFDEC_RGBA8)
            : (bit_depth == 16U
                ? AVIFDEC_RGB16 : AVIFDEC_RGB8));
    rows.error = error;
    if (executor != 0 && executor->worker_count > 1U &&
        info->presentation_height > 1U) {
        size_t bytes_per_channel = bit_depth == 16U ? 2U : 1U;
        size_t capacity =
            (size_t)executor->worker_count * CLI_PNG_ROWS_PER_WORKER;

        if (capacity > CLI_PNG_MAX_CACHED_ROWS) {
            capacity = CLI_PNG_MAX_CACHED_ROWS;
        }
        if (capacity > info->presentation_height) {
            capacity = info->presentation_height;
        }
        if (!avifdec_size_multiply(
                info->presentation_width, channels,
                &rows.row_bytes) ||
            !avifdec_size_multiply(
                rows.row_bytes, bytes_per_channel,
                &rows.row_bytes) ||
            !avifdec_size_multiply(
                rows.row_bytes, capacity, &row_cache_size)) {
            status = AVIFDEC_OVERFLOW;
        } else {
            row_cache = platform_allocate_pages(row_cache_size);
            if (row_cache == 0) status = AVIFDEC_OUT_OF_MEMORY;
        }
        if (status != AVIFDEC_OK) {
            (void)platform_close(fd);
            (void)platform_free_pages(workspace, workspace_size);
            return status;
        }
        rows.cache = (unsigned char *)row_cache;
        rows.cache_capacity = (uint32_t)capacity;
    }
    status = avifdec_png_write_rows(
        write_png_bytes, &fd, read_png_rgb_row, &rows,
        workspace, workspace_size,
        info->presentation_width, info->presentation_height,
        channels, bit_depth, &metadata);
    if (platform_close(fd) != 0 && status == AVIFDEC_OK) {
        status = AVIFDEC_IO_ERROR;
    }
    if (row_cache != 0) {
        (void)platform_free_pages(row_cache, row_cache_size);
    }
    (void)platform_free_pages(workspace, workspace_size);
    return status;
}
