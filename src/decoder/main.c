#include "bmff.h"
#include "platform/platform.h"
#include "png.h"
#include "task_pool.h"
#include <stddef.h>
#include <stdint.h>

#define CLI_MAX_INPUT_SIZE (1024U * 1024U * 1024U)
#define CLI_PNG_ROWS_PER_WORKER 8U
#define CLI_PNG_MAX_CACHED_ROWS 64U

static size_t text_length(const char *text) {
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static int text_equal(const char *left, const char *right) {
    size_t index = 0U;

    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return 0;
        ++index;
    }
    return left[index] == right[index];
}

static int text_to_size(const char *text, size_t *value) {
    size_t result = 0U;
    size_t index = 0U;

    if (text[0] == '\0') return 0;
    while (text[index] != '\0') {
        unsigned int digit;

        if (text[index] < '0' || text[index] > '9') return 0;
        digit = (unsigned int)(text[index] - '0');
        if (result > (SIZE_MAX - digit) / 10U) return 0;
        result = result * 10U + digit;
        ++index;
    }
    *value = result;
    return 1;
}

static int extract_worker_option(
    int *argument_count,
    char **arguments,
    size_t *requested_workers) {
    int found = 0;
    int index = 1;

    while (index < *argument_count) {
        int shift;

        if (!text_equal(arguments[index], "--workers")) {
            ++index;
            continue;
        }
        if (found || index + 1 >= *argument_count ||
            !text_to_size(arguments[index + 1], requested_workers) ||
            *requested_workers > AVIFDEC_EXECUTOR_MAX_WORKERS) {
            return 0;
        }
        found = 1;
        for (shift = index; shift + 2 < *argument_count; ++shift) {
            arguments[shift] = arguments[shift + 2];
        }
        *argument_count -= 2;
        arguments[*argument_count] = 0;
    }
    return 1;
}

static int extract_flag_option(
    int *argument_count,
    char **arguments,
    const char *option,
    int *enabled) {
    int index = 1;

    while (index < *argument_count) {
        int shift;

        if (!text_equal(arguments[index], option)) {
            ++index;
            continue;
        }
        if (*enabled) return 0;
        *enabled = 1;
        for (shift = index; shift + 1 < *argument_count; ++shift) {
            arguments[shift] = arguments[shift + 1];
        }
        --*argument_count;
        arguments[*argument_count] = 0;
    }
    return 1;
}

static int write_bytes(int fd, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0;

    while (written < length) {
        long result = platform_write(fd, bytes + written, length - written);
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static int write_text(int fd, const char *text) {
    return write_bytes(fd, text, text_length(text));
}

static int write_unsigned(int fd, size_t value) {
    char reverse[32];
    char output[32];
    size_t count = 0U;
    size_t index;

    do {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    for (index = 0U; index < count; ++index) {
        output[index] = reverse[count - index - 1U];
    }
    return write_bytes(fd, output, count);
}

static int write_hex_u64(int fd, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char output[18];
    unsigned int index;

    output[0] = '0';
    output[1] = 'x';
    for (index = 0U; index < 16U; ++index) {
        output[2U + index] = digits[(value >> ((15U - index) * 4U)) & 15U];
    }
    return write_bytes(fd, output, sizeof(output));
}

static int write_fourcc(int fd, uint32_t type) {
    unsigned char text[4];
    size_t index;

    text[0] = (unsigned char)(type >> 24);
    text[1] = (unsigned char)(type >> 16);
    text[2] = (unsigned char)(type >> 8);
    text[3] = (unsigned char)type;
    for (index = 0U; index < sizeof(text); ++index) {
        if (text[index] < 0x20U || text[index] > 0x7eU) text[index] = '.';
    }
    return write_bytes(fd, text, sizeof(text));
}

static void write_entropy_trace(const AvifdecEntropyTrace *trace) {
    (void)write_text(1, "\nentropy_tiles=");
    (void)write_unsigned(1, trace->tile_count);
    (void)write_text(1, "\nentropy_partition_nodes=");
    (void)write_unsigned(1, trace->partition_nodes);
    (void)write_text(1, "\nentropy_blocks=");
    (void)write_unsigned(1, trace->block_count);
    (void)write_text(1, "\nframes=");
    (void)write_unsigned(1, trace->frame_count);
    (void)write_text(1, "\nshow_existing_frames=");
    (void)write_unsigned(1, trace->show_existing_frame_count);
    (void)write_text(1, "\ninter_blocks=");
    (void)write_unsigned(1, trace->inter_block_count);
    (void)write_text(1, "\ncompound_blocks=");
    (void)write_unsigned(1, trace->compound_block_count);
    (void)write_text(1, "\nentropy_transforms=");
    (void)write_unsigned(1, trace->transform_count);
    (void)write_text(1, "\nentropy_nonzero_transforms=");
    (void)write_unsigned(1, trace->nonzero_transform_count);
    (void)write_text(1, "\nentropy_coefficients=");
    (void)write_unsigned(1, trace->coefficient_count);
    (void)write_text(1, "\ntransform_size_mask=");
    (void)write_hex_u64(1, trace->transform_size_mask);
    (void)write_text(1, "\ntransform_type_mask=");
    (void)write_hex_u64(1, trace->transform_type_mask);
    (void)write_text(1, "\nentropy_checksum=");
    (void)write_hex_u64(1, trace->checksum);
    (void)write_text(1, "\nmode_checksum=");
    (void)write_hex_u64(1, trace->mode_checksum);
    (void)write_text(1, "\nreference_state_checksum=");
    (void)write_hex_u64(1, trace->reference_state_checksum);
    (void)write_text(1, "\ninter_mode_checksum=");
    (void)write_hex_u64(1, trace->inter_mode_checksum);
    (void)write_text(1, "\nmv_stack_checksum=");
    (void)write_hex_u64(1, trace->mv_stack_checksum);
    (void)write_text(1, "\nmv_checksum=");
    (void)write_hex_u64(1, trace->mv_checksum);
    (void)write_text(1, "\npredictor_checksum=");
    (void)write_hex_u64(1, trace->predictor_checksum);
    (void)write_text(1, "\nquantized_checksum=");
    (void)write_hex_u64(1, trace->quantized_checksum);
    (void)write_text(1, "\ndequantized_checksum=");
    (void)write_hex_u64(1, trace->dequantized_checksum);
    (void)write_text(1, "\nresidual_checksum=");
    (void)write_hex_u64(1, trace->residual_checksum);
    (void)write_text(1, "\nreconstruction_checksum=");
    (void)write_hex_u64(1, trace->reconstruction_checksum);
    (void)write_text(1, "\ndeblocked_checksum=");
    (void)write_hex_u64(1, trace->deblocked_checksum);
    (void)write_text(1, "\ncdef_checksum=");
    (void)write_hex_u64(1, trace->cdef_checksum);
    (void)write_text(1, "\nsuperres_checksum=");
    (void)write_hex_u64(1, trace->superres_checksum);
    (void)write_text(1, "\nrestoration_checksum=");
    (void)write_hex_u64(1, trace->restoration_checksum);
}

static int read_exact(int fd, unsigned char *buffer, size_t count) {
    size_t received = 0;

    while (received < count) {
        long result = platform_read(fd, buffer + received, count - received);
        if (result <= 0) {
            return -1;
        }
        received += (size_t)result;
    }
    return 0;
}

static int write_raw_image(const char *path, const AvifdecImage *image) {
    unsigned char *row_buffer;
    size_t row_buffer_size;
    unsigned int plane_count = image->monochrome ? 1U : 3U;
    unsigned int plane;
    int fd;

    row_buffer_size = (size_t)image->widths[0] * 2U;
    row_buffer = (unsigned char *)platform_allocate_pages(row_buffer_size);
    if (row_buffer == 0) return -1;
    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        (void)platform_free_pages(row_buffer, row_buffer_size);
        return -1;
    }
    for (plane = 0U; plane < plane_count; ++plane) {
        uint32_t row;
        for (row = 0U; row < image->heights[plane]; ++row) {
            const uint16_t *source = image->planes[plane] +
                (size_t)row * image->strides[plane];
            uint32_t column;
            size_t bytes;
            if (image->bit_depth == 8U) {
                for (column = 0U; column < image->widths[plane]; ++column) {
                    row_buffer[column] = (unsigned char)source[column];
                }
                bytes = image->widths[plane];
            } else {
                for (column = 0U; column < image->widths[plane]; ++column) {
                    row_buffer[2U * column] = (unsigned char)source[column];
                    row_buffer[2U * column + 1U] =
                        (unsigned char)(source[column] >> 8U);
                }
                bytes = (size_t)image->widths[plane] * 2U;
            }
            if (write_bytes(fd, row_buffer, bytes) != 0) {
                (void)platform_close(fd);
                (void)platform_free_pages(row_buffer, row_buffer_size);
                return -1;
            }
        }
    }
    if (platform_close(fd) != 0) {
        (void)platform_free_pages(row_buffer, row_buffer_size);
        return -1;
    }
    (void)platform_free_pages(row_buffer, row_buffer_size);
    return 0;
}

static int write_file(const char *path, const void *data, size_t size) {
    int fd = platform_open_write(path, 0644U);
    int result;

    if (fd < 0) return -1;
    result = write_bytes(fd, data, size);
    if (platform_close(fd) != 0) result = -1;
    return result;
}

static int write_png_bytes(void *user_data, const void *data, size_t size) {
    int fd = *(int *)user_data;

    return write_bytes(fd, data, size);
}

typedef struct {
    const AvifdecImage *image;
    const AvifdecImageInfo *info;
    AvifdecRgbImage rgb;
    uint32_t first_row;
    uint32_t error_rows[AVIFDEC_EXECUTOR_MAX_WORKERS];
    AvifdecError errors[AVIFDEC_EXECUTOR_MAX_WORKERS];
} CliRgbRows;

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

static AvifdecStatus convert_rgb_rows_parallel(
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
        status = convert_rgb_rows_parallel(
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

static AvifdecStatus write_png_file(
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

static int load_file(const char *path, unsigned char **data_out, size_t *size_out) {
    long long end;
    unsigned char *data;
    int fd = platform_open_read(path);

    if (fd < 0) return -1;
    end = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (end <= 0 || (unsigned long long)end > (unsigned long long)SIZE_MAX ||
        (unsigned long long)end > (unsigned long long)CLI_MAX_INPUT_SIZE ||
        platform_seek(fd, 0, PLATFORM_SEEK_SET) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    data = (unsigned char *)platform_allocate_pages((size_t)end);
    if (data == 0) {
        (void)platform_close(fd);
        return -1;
    }
    if (read_exact(fd, data, (size_t)end) != 0) {
        (void)platform_close(fd);
        (void)platform_free_pages(data, (size_t)end);
        return -1;
    }
    (void)platform_close(fd);
    *data_out = data;
    *size_out = (size_t)end;
    return 0;
}

typedef struct {
    int io_failed;
} PrintContext;

static void print_box(const AvifdecBmffBox *box, void *user_data) {
    PrintContext *context = (PrintContext *)user_data;
    size_t depth;

    if (context->io_failed) return;
    for (depth = 0U; depth < box->depth; ++depth) {
        if (write_text(1, "  ") != 0) context->io_failed = 1;
    }
    if (write_fourcc(1, box->type) != 0 ||
        write_text(1, " offset=") != 0 ||
        write_unsigned(1, box->offset) != 0 ||
        write_text(1, " size=") != 0 ||
        write_unsigned(1, box->size) != 0 ||
        write_text(1, "\n") != 0) {
        context->io_failed = 1;
    }
}

static void print_error(const AvifdecError *error) {
    (void)write_text(2, "avifdec: ");
    (void)write_text(2, avifdec_status_string(error->status));
    (void)write_text(2, " at offset ");
    (void)write_unsigned(2, error->offset);
    if (error->context != 0U) {
        (void)write_text(2, " in ");
        (void)write_fourcc(2, error->context);
    }
    (void)write_text(2, "\n");
}

static int bmff_has_brand(
    const AvifdecBmffInfo *info, uint32_t brand) {
    size_t index;

    if (info->major_brand == brand) return 1;
    for (index = 0U; index < info->compatible_brand_count; ++index) {
        if (info->compatible_brands[index] == brand) return 1;
    }
    return 0;
}

typedef struct {
    AvifdecParallelBody body;
    void *arg;
} CliParallelCall;

static int cli_parallel_body(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg) {
    CliParallelCall *call = (CliParallelCall *)arg;

    return (int)call->body(
        begin, end, worker_index, call->arg);
}

static AvifdecStatus cli_parallel_for(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifdecParallelBody body,
    void *arg) {
    CliParallelCall call;
    int status;

    if (user_data == 0 || body == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    call.body = body;
    call.arg = arg;
    status = rt_parallel_for(
        (RtTaskPool *)user_data, count, min_chunk,
        cli_parallel_body, &call);
    if (status < (int)AVIFDEC_OK ||
        status > (int)AVIFDEC_UNSUPPORTED) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    return (AvifdecStatus)status;
}

static AvifdecStatus allocate_image_planes(
    const AvifdecImageInfo *info,
    AvifdecImage *image,
    void **memory,
    size_t *memory_size) {
    size_t luma_samples;
    size_t chroma_samples = 0U;
    size_t output_samples;

    if (!avifdec_size_multiply(
            info->width, info->height, &luma_samples)) {
        return AVIFDEC_OVERFLOW;
    }
    output_samples = luma_samples;
    if (!info->monochrome) {
        size_t chroma_width =
            (info->width + ((size_t)1U << info->subsampling_x) - 1U) >>
            info->subsampling_x;
        size_t chroma_height =
            (info->height + ((size_t)1U << info->subsampling_y) - 1U) >>
            info->subsampling_y;

        if (!avifdec_size_multiply(
                chroma_width, chroma_height, &chroma_samples) ||
            chroma_samples > (SIZE_MAX - output_samples) / 2U) {
            return AVIFDEC_OVERFLOW;
        }
        output_samples += 2U * chroma_samples;
    }
    if (info->has_alpha) {
        if (luma_samples > SIZE_MAX - output_samples) {
            return AVIFDEC_OVERFLOW;
        }
        output_samples += luma_samples;
    }
    if (!avifdec_size_multiply(
            output_samples, sizeof(uint16_t), memory_size)) {
        return AVIFDEC_OVERFLOW;
    }
    *memory = platform_allocate_pages(*memory_size);
    if (*memory == 0) return AVIFDEC_OUT_OF_MEMORY;
    avifdec_memory_fill(image, 0U, sizeof(*image));
    image->planes[0] = (uint16_t *)*memory;
    image->strides[0] = info->width;
    if (!info->monochrome) {
        image->planes[1] = image->planes[0] + luma_samples;
        image->planes[2] = image->planes[1] + chroma_samples;
        image->strides[1] =
            (info->width + ((size_t)1U << info->subsampling_x) - 1U) >>
            info->subsampling_x;
        image->strides[2] = image->strides[1];
    }
    if (info->has_alpha) {
        image->alpha_plane =
            image->planes[0] + luma_samples + 2U * chroma_samples;
        image->alpha_stride = info->width;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus write_sequence_png(
        const unsigned char *data,
        size_t size,
        size_t frame_index,
        const char *output_path,
        const AvifdecExecutor *executor,
        AvifdecEntropyTrace *trace,
        AvifdecSequenceInfo *sequence,
        AvifdecFrameInfo *frame,
        AvifdecError *error) {
        AvifdecImage image;
        void *workspace = 0;
        void *image_memory = 0;
        size_t image_memory_size = 0U;
        AvifdecStatus status;

        status = avifdec_sequence_query_ex(
            data, size, 0, executor, sequence, error);
        if (status != AVIFDEC_OK) return status;
        status = avifdec_sequence_frame_query_ex(
            data, size, 0, executor, frame_index, frame, error);
        if (status != AVIFDEC_OK) return status;
        workspace = platform_allocate_pages(
            frame->image.workspace_required);
        if (workspace == 0) return AVIFDEC_OUT_OF_MEMORY;
        status = allocate_image_planes(
            &frame->image, &image, &image_memory, &image_memory_size);
        if (status != AVIFDEC_OK) goto cleanup;
        status = avifdec_sequence_decode_frame_ex(
            data, size, 0, executor, frame_index, workspace,
            frame->image.workspace_required, &image,
            trace, frame,
            error);
        if (status != AVIFDEC_OK) goto cleanup;

        status = write_png_file(
            output_path, &image, &frame->image, executor, error);

    cleanup:
        if (image_memory != 0) {
            (void)platform_free_pages(
                image_memory, image_memory_size);
        }
        if (workspace != 0) {
            (void)platform_free_pages(
                workspace, frame->image.workspace_required);
        }
    return status;
}

static AvifdecStatus write_sequence_raw(
    const unsigned char *data,
    size_t size,
    size_t frame_index,
    const char *output_path,
    const AvifdecExecutor *executor,
    AvifdecEntropyTrace *trace,
    AvifdecSequenceInfo *sequence,
    AvifdecFrameInfo *frame,
    AvifdecError *error) {
    AvifdecImage image;
    void *workspace = 0;
    void *image_memory = 0;
    size_t image_memory_size = 0U;
    AvifdecStatus status;

    status = avifdec_sequence_query_ex(
        data, size, 0, executor, sequence, error);
    if (status != AVIFDEC_OK) return status;
    status = avifdec_sequence_frame_query_ex(
        data, size, 0, executor, frame_index, frame, error);
    if (status != AVIFDEC_OK) return status;
    workspace = platform_allocate_pages(
        frame->image.workspace_required);
    if (workspace == 0) return AVIFDEC_OUT_OF_MEMORY;
    status = allocate_image_planes(
        &frame->image, &image, &image_memory, &image_memory_size);
    if (status == AVIFDEC_OK) {
        status = avifdec_sequence_decode_frame_ex(
            data, size, 0, executor, frame_index, workspace,
            frame->image.workspace_required, &image,
            trace, frame,
            error);
    }
    if (status == AVIFDEC_OK &&
        write_raw_image(output_path, &image) != 0) {
        status = AVIFDEC_IO_ERROR;
    }
    if (image_memory != 0) {
        (void)platform_free_pages(
            image_memory, image_memory_size);
    }
    (void)platform_free_pages(
        workspace, frame->image.workspace_required);
    return status;
}

int main(int argc, char **argv) {
    unsigned char *data = 0;
    void *workspace = 0;
    void *output_memory = 0;
    void *packed_memory = 0;
    size_t output_memory_size = 0U;
    size_t packed_memory_size = 0U;
    size_t size = 0U;
    AvifdecBmffLimits limits = { 32U, 100000U };
    AvifdecBmffInfo info;
    AvifdecImageInfo image_info;
    AvifdecEntropyTrace entropy_trace;
    AvifdecImage image = { 0 };
    AvifdecError error;
    AvifdecStatus status;
    PrintContext print_context = { 0 };
    size_t index;
    int boxes_only = 0;
    int raw_output = 0;
    int png_output = 0;
    int sequence_png_output = 0;
    int sequence_raw_output = 0;
    size_t sequence_frame_index = 0U;
    int packed_format = -1;
    int packed_alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    const char *input_path;
    const char *output_path = 0;
    size_t requested_workers = 1U;
    RtTaskPool task_pool;
    AvifdecExecutor executor;
    const AvifdecExecutor *decode_executor = 0;
    int task_pool_initialized = 0;
    int diagnostics = 0;

    if (!extract_worker_option(
            &argc, argv, &requested_workers)) {
        (void)write_text(
            2, "avifdec: invalid worker count\n");
        return 2;
    }
    if (!extract_flag_option(
            &argc, argv, "--diagnostics", &diagnostics)) {
        (void)write_text(
            2, "avifdec: duplicate diagnostics option\n");
        return 2;
    }
    if (argc == 3 && text_equal(argv[1], "--boxes")) {
        boxes_only = 1;
        input_path = argv[2];
    } else if (argc == 4 && text_equal(argv[1], "--raw")) {
        raw_output = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc == 4 && text_equal(argv[1], "--png")) {
        png_output = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc == 5 &&
               text_equal(argv[1], "--png-frame")) {
        if (!text_to_size(argv[2], &sequence_frame_index)) {
            (void)write_text(2, "avifdec: invalid frame index\n");
            return 2;
        }
        sequence_png_output = 1;
        input_path = argv[3];
        output_path = argv[4];
    } else if (argc == 5 &&
               text_equal(argv[1], "--raw-frame")) {
        if (!text_to_size(argv[2], &sequence_frame_index)) {
            (void)write_text(2, "avifdec: invalid frame index\n");
            return 2;
        }
        sequence_raw_output = 1;
        input_path = argv[3];
        output_path = argv[4];
    } else if (argc == 4 &&
               (text_equal(argv[1], "--rgb") ||
                text_equal(argv[1], "--rgba") ||
                text_equal(argv[1], "--rgba-premul") ||
                text_equal(argv[1], "--rgb16") ||
                text_equal(argv[1], "--rgba16") ||
                text_equal(argv[1], "--rgba16-premul"))) {
        packed_format = text_equal(argv[1], "--rgb")
            ? AVIFDEC_RGB8
            : (text_equal(argv[1], "--rgba") ||
               text_equal(argv[1], "--rgba-premul"))
                ? AVIFDEC_RGBA8
                : text_equal(argv[1], "--rgb16")
                    ? AVIFDEC_RGB16 : AVIFDEC_RGBA16;
        packed_alpha_mode =
            text_equal(argv[1], "--rgba-premul") ||
            text_equal(argv[1], "--rgba16-premul")
                ? AVIFDEC_ALPHA_PREMULTIPLIED
                : AVIFDEC_ALPHA_STRAIGHT;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc != 2) {
        (void)write_text(2,
            "usage: avifdec [--workers N] [--diagnostics] [--boxes INPUT.avif | --raw INPUT.avif OUTPUT.yuv | --raw-frame INDEX INPUT.avif OUTPUT.yuv | --png INPUT.avif OUTPUT.png | --png-frame INDEX INPUT.avif OUTPUT.png | --rgb INPUT.avif OUTPUT.rgb | --rgba INPUT.avif OUTPUT.rgba | --rgba-premul INPUT.avif OUTPUT.rgba | --rgb16 INPUT.avif OUTPUT.rgb16 | --rgba16 INPUT.avif OUTPUT.rgba16 | --rgba16-premul INPUT.avif OUTPUT.rgba16 | INPUT.avif]\n");
        return 2;
    } else {
        input_path = argv[1];
    }

    if (load_file(input_path, &data, &size) != 0) {
        (void)write_text(2, "avifdec: cannot read input\n");
        return 1;
    }
    status = avifdec_bmff_inspect(data, size, &limits, print_box, &print_context, &info, &error);
    if (status != AVIFDEC_OK) {
        print_error(&error);
        (void)platform_free_pages(data, size);
        return 1;
    }
    if (!info.has_avif_brand) {
        (void)write_text(2, "avifdec: ftyp does not declare avif or avis\n");
        (void)platform_free_pages(data, size);
        return 1;
    }
    (void)write_text(1, "major_brand=");
    (void)write_fourcc(1, info.major_brand);
    (void)write_text(1, "\n");
    for (index = 0U; index < info.compatible_brand_count; ++index) {
        (void)write_text(1, "compatible_brand=");
        (void)write_fourcc(1, info.compatible_brands[index]);
        (void)write_text(1, "\n");
    }
    (void)write_text(1, "boxes=");
    (void)write_unsigned(1, info.box_count);
    (void)write_text(1, "\n");
    if (!boxes_only &&
        bmff_has_brand(
            &info, AVIFDEC_FOURCC('a', 'v', 'i', 's')) &&
        (sequence_png_output || sequence_raw_output ||
         (!raw_output && !png_output && packed_format < 0))) {
        AvifdecSequenceInfo sequence;
        AvifdecFrameInfo frame;

        if ((sequence_png_output || sequence_raw_output) &&
            requested_workers != 1U) {
            AvifdecFrameInfo sizing_frame;
            size_t work_count;
            size_t worker_count = requested_workers;

            status = avifdec_sequence_frame_query(
                data, size, 0, sequence_frame_index,
                &sizing_frame, &error);
            if (status != AVIFDEC_OK) {
                print_error(&error);
                (void)platform_free_pages(data, size);
                return 1;
            }
            work_count =
                ((size_t)sizing_frame.image.height + 3U) / 4U;
            if (!sizing_frame.image.monochrome &&
                !avifdec_size_multiply(
                    work_count, 3U, &work_count)) {
                (void)platform_free_pages(data, size);
                return 1;
            }
            if (sizing_frame.image.workspace_tile_capacity > work_count) {
                work_count =
                    sizing_frame.image.workspace_tile_capacity;
            }
            if (worker_count == 0U) {
                worker_count = rt_task_pool_available_width();
            }
            if (worker_count > work_count) worker_count = work_count;
            if (worker_count > 1U &&
                rt_task_pool_init(
                    &task_pool, (unsigned int)worker_count) == 0 &&
                rt_task_pool_width(&task_pool) > 1U) {
                task_pool_initialized = 1;
                executor.user_data = &task_pool;
                executor.worker_count = rt_task_pool_width(&task_pool);
                executor.parallel_for = cli_parallel_for;
                decode_executor = &executor;
            }
        }
        if (sequence_png_output) {
            status = write_sequence_png(
                data, size, sequence_frame_index, output_path,
                decode_executor, diagnostics ? &entropy_trace : 0,
                &sequence, &frame, &error);
        } else if (sequence_raw_output) {
            status = write_sequence_raw(
                data, size, sequence_frame_index, output_path,
                decode_executor, diagnostics ? &entropy_trace : 0,
                &sequence, &frame, &error);
        } else {
            status = avifdec_sequence_query_ex(
                data, size, 0, decode_executor, &sequence, &error);
            avifdec_memory_fill(&frame, 0U, sizeof(frame));
        }
        if (status != AVIFDEC_OK) {
            if (error.status == AVIFDEC_OK) {
                error.status = status;
                error.offset = 0U;
                error.context = 0U;
            }
            print_error(&error);
            if (task_pool_initialized) {
                rt_task_pool_destroy(&task_pool);
            }
            (void)platform_free_pages(data, size);
            return 1;
        }
        (void)write_text(1, "sequence_frames=");
        (void)write_unsigned(1, sequence.frame_count);
        (void)write_text(1, "\nsequence_timescale=");
        (void)write_unsigned(1, sequence.timescale);
        (void)write_text(1, "\nsequence_duration=");
        (void)write_unsigned(1, sequence.duration);
        (void)write_text(1, "\nsequence_workspace_required=");
        (void)write_unsigned(1, sequence.workspace_required);
        (void)write_text(1, "\nsequence_alpha=");
        (void)write_unsigned(1, sequence.has_alpha);
        (void)write_text(1, "\nsequence_alpha_premultiplied=");
        (void)write_unsigned(1, sequence.alpha_premultiplied);
        (void)write_text(1, "\nsequence_repeat_forever=");
        (void)write_unsigned(1, sequence.repeat_forever);
        if (sequence.repeat_count_present) {
            (void)write_text(1, "\nsequence_repeat_count=");
            (void)write_unsigned(
                1, (size_t)sequence.repeat_count);
        }
        if (sequence_png_output || sequence_raw_output) {
            (void)write_text(1, "\nframe_index=");
            (void)write_unsigned(1, frame.frame_index);
            (void)write_text(1, "\nframe_sync_index=");
            (void)write_unsigned(1, frame.sync_frame_index);
            (void)write_text(1, "\nframe_dts=");
            (void)write_unsigned(1, frame.dts);
            (void)write_text(1, "\nframe_duration=");
            (void)write_unsigned(1, frame.duration);
            (void)write_text(1, "\nframe_workspace_required=");
            (void)write_unsigned(
                1, frame.image.workspace_required);
            (void)write_text(
                1, sequence_png_output
                    ? "\npacked_format=png"
                    : "\nraw_plane_order=YUV");
            if (diagnostics) write_entropy_trace(&entropy_trace);
        }
        (void)write_text(1, "\n");
        if (task_pool_initialized) {
            rt_task_pool_destroy(&task_pool);
        }
        (void)platform_free_pages(data, size);
        return 0;
    }
    if (!boxes_only) {
        status = avifdec_query(data, size, 0, 0, 0U, &image_info, &error);
        if (status != AVIFDEC_OK) {
            print_error(&error);
            (void)platform_free_pages(data, size);
            return 1;
        }
        if (requested_workers != 1U &&
            image_info.tone_map_base_item_id == 0U &&
            ((raw_output || png_output || packed_format >= 0)
                 ? (!image_info.is_grid ||
                    image_info.grid_rows > 1U ||
                    image_info.grid_columns > 1U)
                 : (!image_info.is_grid &&
                    !image_info.sample_transform_present))) {
            size_t work_count;
            size_t worker_count = requested_workers;

            if (image_info.is_grid &&
                (image_info.grid_rows > 1U ||
                 image_info.grid_columns > 1U)) {
                if (!avifdec_size_multiply(
                        image_info.grid_rows,
                        image_info.grid_columns,
                        &work_count)) {
                    (void)platform_free_pages(data, size);
                    return 1;
                }
            } else if (image_info.sample_transform_present) {
                work_count = image_info.height;
                if (!image_info.monochrome) {
                    size_t chroma_rows =
                        (image_info.height +
                         ((size_t)1U <<
                          image_info.subsampling_y) - 1U) >>
                            image_info.subsampling_y;

                    if (!avifdec_size_multiply(
                            chroma_rows, 2U,
                            &chroma_rows) ||
                        !avifdec_size_add(
                            work_count, chroma_rows,
                            &work_count)) {
                        (void)platform_free_pages(data, size);
                        return 1;
                    }
                }
            } else {
                work_count =
                    ((size_t)image_info.height + 3U) / 4U;
                if (!image_info.monochrome &&
                    !avifdec_size_multiply(
                        work_count, 3U, &work_count)) {
                    (void)platform_free_pages(data, size);
                    return 1;
                }
            }
            if (worker_count == 0U) {
                worker_count =
                    rt_task_pool_available_width();
            }
            if (worker_count > work_count) {
                worker_count = work_count;
            }
            if (worker_count > 1U &&
                rt_task_pool_init(
                    &task_pool,
                    (unsigned int)worker_count) == 0 &&
                rt_task_pool_width(&task_pool) > 1U) {
                task_pool_initialized = 1;
                executor.user_data = &task_pool;
                executor.worker_count =
                    rt_task_pool_width(&task_pool);
                executor.parallel_for = cli_parallel_for;
                decode_executor = &executor;
                status = avifdec_query_ex(
                    data, size, 0, decode_executor,
                    0, 0U, &image_info, &error);
                if (status != AVIFDEC_OK) {
                    print_error(&error);
                    rt_task_pool_destroy(&task_pool);
                    (void)platform_free_pages(data, size);
                    return 1;
                }
            }
        }
        workspace = platform_allocate_pages(image_info.workspace_required);
        if (workspace == 0) {
            (void)write_text(2, "avifdec: cannot allocate trace workspace\n");
            if (task_pool_initialized) {
                rt_task_pool_destroy(&task_pool);
            }
            (void)platform_free_pages(data, size);
            return 1;
        }
        if (raw_output || png_output || packed_format >= 0) {
            size_t luma_samples;
            size_t output_samples;
            size_t chroma_samples = 0U;
            size_t alpha_samples = 0U;
            if (image_info.width > SIZE_MAX / image_info.height) {
                status = AVIFDEC_OVERFLOW;
            } else {
                luma_samples = (size_t)image_info.width * image_info.height;
                output_samples = luma_samples;
                if (!image_info.monochrome) {
                    size_t chroma_width = (image_info.width +
                        ((size_t)1U << image_info.subsampling_x) - 1U) >>
                        image_info.subsampling_x;
                    size_t chroma_height = (image_info.height +
                        ((size_t)1U << image_info.subsampling_y) - 1U) >>
                        image_info.subsampling_y;
                    if (chroma_width > SIZE_MAX / chroma_height) {
                        status = AVIFDEC_OVERFLOW;
                    } else {
                        chroma_samples = chroma_width * chroma_height;
                        status = chroma_samples > (SIZE_MAX - output_samples) / 2U
                            ? AVIFDEC_OVERFLOW : AVIFDEC_OK;
                        if (status == AVIFDEC_OK) {
                            output_samples += 2U * chroma_samples;
                        }
                    }
                } else {
                    status = AVIFDEC_OK;
                }
                if (status == AVIFDEC_OK && image_info.has_alpha) {
                    alpha_samples = luma_samples;
                    if (alpha_samples > SIZE_MAX - output_samples) {
                        status = AVIFDEC_OVERFLOW;
                    } else {
                        output_samples += alpha_samples;
                    }
                }
                if (status == AVIFDEC_OK && output_samples > SIZE_MAX / 2U) {
                    status = AVIFDEC_OVERFLOW;
                }
                if (status == AVIFDEC_OK) {
                    output_memory_size = output_samples * sizeof(uint16_t);
                    output_memory = platform_allocate_pages(output_memory_size);
                    if (output_memory == 0) status = AVIFDEC_OUT_OF_MEMORY;
                }
                if (status == AVIFDEC_OK) {
                    image.planes[0] = (uint16_t *)output_memory;
                    image.strides[0] = image_info.width;
                    if (!image_info.monochrome) {
                        image.planes[1] = image.planes[0] + luma_samples;
                        image.planes[2] = image.planes[1] + chroma_samples;
                        image.strides[1] = (image_info.width +
                            ((size_t)1U << image_info.subsampling_x) - 1U) >>
                            image_info.subsampling_x;
                        image.strides[2] = image.strides[1];
                    }
                    if (image_info.has_alpha) {
                        image.alpha_plane =
                            image.planes[0] + luma_samples +
                            2U * chroma_samples;
                        image.alpha_stride = image_info.width;
                    }
                    status = avifdec_decode_ex(
                        data, size, 0, decode_executor,
                        workspace, image_info.workspace_required,
                        &image, diagnostics ? &entropy_trace : 0, &error);
                    if (status == AVIFDEC_OK) {
                        if (raw_output) {
                            if (write_raw_image(
                                    output_path, &image) != 0) {
                                status = AVIFDEC_IO_ERROR;
                            }
                        } else if (png_output) {
                            status = write_png_file(
                                output_path, &image,
                                &image_info, decode_executor,
                                &error);
                        } else {
                            AvifdecRgbImage rgb;
                            size_t channels =
                                packed_format == AVIFDEC_RGB8 ||
                                packed_format == AVIFDEC_RGB16
                                    ? 3U : 4U;
                            size_t bytes_per_channel =
                                packed_format == AVIFDEC_RGB16 ||
                                packed_format == AVIFDEC_RGBA16
                                    ? 2U : 1U;
                            size_t row_bytes;

                            if (!avifdec_size_multiply(
                                    image_info.presentation_width,
                                    channels, &row_bytes) ||
                                !avifdec_size_multiply(
                                    row_bytes, bytes_per_channel,
                                    &row_bytes) ||
                                !avifdec_size_multiply(
                                    row_bytes,
                                    image_info.presentation_height,
                                    &packed_memory_size)) {
                                status = AVIFDEC_OVERFLOW;
                            } else {
                                packed_memory = platform_allocate_pages(
                                    packed_memory_size);
                                if (packed_memory == 0) {
                                    status = AVIFDEC_OUT_OF_MEMORY;
                                }
                            }
                            if (status == AVIFDEC_OK) {
                                rgb.pixels = packed_memory;
                                rgb.stride = row_bytes;
                                rgb.width =
                                    image_info.presentation_width;
                                rgb.height =
                                    image_info.presentation_height;
                                rgb.format = (uint8_t)packed_format;
                                rgb.alpha_mode =
                                    (uint8_t)packed_alpha_mode;
                                status = decode_executor != 0 &&
                                         rgb.height > 1U
                                    ? convert_rgb_rows_parallel(
                                        decode_executor,
                                        &image, &image_info,
                                        &rgb, 0U, rgb.height,
                                        &error)
                                    : avifdec_image_to_rgb(
                                        &image, &image_info,
                                        &rgb, &error);
                            }
                            if (status == AVIFDEC_OK) {
                                if (write_file(
                                        output_path, packed_memory,
                                        packed_memory_size) != 0) {
                                    status = AVIFDEC_IO_ERROR;
                                }
                            }
                        }
                        if (status != AVIFDEC_OK &&
                            error.status == AVIFDEC_OK) {
                            error.status = status;
                            error.offset = 0U;
                            error.context = 0U;
                        }
                    }
                }
            }
        } else {
            status = avifdec_trace_ex(
                data, size, 0, decode_executor, workspace,
                image_info.workspace_required, &entropy_trace,
                &error);
        }
        if (status != AVIFDEC_OK) {
            print_error(&error);
            if (output_memory != 0) {
                (void)platform_free_pages(output_memory, output_memory_size);
            }
            if (packed_memory != 0) {
                (void)platform_free_pages(
                    packed_memory, packed_memory_size);
            }
            (void)platform_free_pages(workspace, image_info.workspace_required);
            if (task_pool_initialized) {
                rt_task_pool_destroy(&task_pool);
            }
            (void)platform_free_pages(data, size);
            return 1;
        }
        (void)write_text(1, "width=");
        (void)write_unsigned(1, image_info.width);
        (void)write_text(1, "\nheight=");
        (void)write_unsigned(1, image_info.height);
        (void)write_text(1, "\nrender_width=");
        (void)write_unsigned(1, image_info.render_width);
        (void)write_text(1, "\nrender_height=");
        (void)write_unsigned(1, image_info.render_height);
        (void)write_text(1, "\nbit_depth=");
        (void)write_unsigned(1, image_info.bit_depth);
        (void)write_text(1, "\nprofile=");
        (void)write_unsigned(1, image_info.profile);
        (void)write_text(1, "\nlevel=");
        (void)write_unsigned(1, image_info.level);
        (void)write_text(1, "\ntier=");
        (void)write_unsigned(1, image_info.tier);
        (void)write_text(1, "\noperating_point=");
        (void)write_unsigned(1, image_info.operating_point);
        (void)write_text(1, "\noperating_point_count=");
        (void)write_unsigned(1, image_info.operating_point_count);
        (void)write_text(1, "\noperating_point_idc=");
        (void)write_unsigned(1, image_info.operating_point_idc);
        (void)write_text(1, "\nsubsampling_x=");
        (void)write_unsigned(1, image_info.subsampling_x);
        (void)write_text(1, "\nsubsampling_y=");
        (void)write_unsigned(1, image_info.subsampling_y);
        (void)write_text(1, "\ncolor_range=");
        (void)write_unsigned(1, image_info.color_range);
        (void)write_text(1, "\nprimary_item_id=");
        (void)write_unsigned(1, image_info.primary_item_id);
        (void)write_text(1, "\nprimary_item_type=");
        (void)write_fourcc(1, image_info.primary_item_type);
        (void)write_text(1, "\npresentation_width=");
        (void)write_unsigned(1, image_info.presentation_width);
        (void)write_text(1, "\npresentation_height=");
        (void)write_unsigned(1, image_info.presentation_height);
        (void)write_text(1, "\ntransform_flags=");
        (void)write_unsigned(1, image_info.transform_flags);
        (void)write_text(1, "\nalpha_present=");
        (void)write_unsigned(1, image_info.has_alpha);
        (void)write_text(1, "\nalpha_premultiplied=");
        (void)write_unsigned(1, image_info.alpha_premultiplied);
        (void)write_text(1, "\ngrid_rows=");
        (void)write_unsigned(1, image_info.grid_rows);
        (void)write_text(1, "\ngrid_columns=");
        (void)write_unsigned(1, image_info.grid_columns);
        (void)write_text(1, "\nlayer_count=");
        (void)write_unsigned(1, image_info.layer_count);
        (void)write_text(1, "\nselected_layer=");
        (void)write_unsigned(1, image_info.selected_layer);
        (void)write_text(1, "\nsample_transform_present=");
        (void)write_unsigned(
            1, image_info.sample_transform_present);
        (void)write_text(1, "\ngain_map_present=");
        (void)write_unsigned(1, image_info.gain_map_present);
        (void)write_text(1, "\npayload_size=");
        (void)write_unsigned(1, image_info.payload_size);
        (void)write_text(1, "\nextents=");
        (void)write_unsigned(1, image_info.extent_count);
        (void)write_text(1, "\nobus=");
        (void)write_unsigned(1, image_info.obu_count);
        (void)write_text(1, "\nmetadata_obus=");
        (void)write_unsigned(1, image_info.metadata_obu_count);
        (void)write_text(1, "\nmetadata_present_mask=");
        (void)write_unsigned(1, image_info.metadata_present_mask);
        (void)write_text(1, "\nfilm_grain_params_present=");
        (void)write_unsigned(
            1, image_info.film_grain_params_present);
        (void)write_text(1, "\nfilm_grain_applied=");
        (void)write_unsigned(1, image_info.film_grain_applied);
        if (image_info.film_grain_applied) {
            (void)write_text(1, "\nfilm_grain_seed=");
            (void)write_unsigned(1, image_info.film_grain_seed);
            (void)write_text(1, "\nfilm_grain_overlap=");
            (void)write_unsigned(
                1, image_info.film_grain_overlap);
            (void)write_text(1, "\nfilm_grain_clip_restricted=");
            (void)write_unsigned(
                1, image_info.film_grain_clip_restricted);
        }
        (void)write_text(1, "\ntiming_info_present=");
        (void)write_unsigned(1, image_info.timing_info_present);
        if (image_info.timing_info_present) {
            (void)write_text(1, "\nnum_units_in_display_tick=");
            (void)write_unsigned(
                1, image_info.num_units_in_display_tick);
            (void)write_text(1, "\ntime_scale=");
            (void)write_unsigned(1, image_info.time_scale);
        }
        (void)write_text(1, "\ndecoder_model_info_present=");
        (void)write_unsigned(
            1, image_info.decoder_model_info_present);
        if (image_info.operating_point_decoder_model_present) {
            (void)write_text(1, "\ndecoder_buffer_delay=");
            (void)write_unsigned(
                1, image_info.decoder_buffer_delay);
            (void)write_text(1, "\nencoder_buffer_delay=");
            (void)write_unsigned(
                1, image_info.encoder_buffer_delay);
            (void)write_text(1, "\nlow_delay_mode=");
            (void)write_unsigned(1, image_info.low_delay_mode);
        }
        (void)write_text(1, "\ntile_columns=");
        (void)write_unsigned(1, image_info.tile_columns);
        (void)write_text(1, "\ntiles=");
        (void)write_unsigned(1, image_info.tile_count);
        (void)write_text(1, "\ntile_data_size=");
        (void)write_unsigned(1, image_info.tile_data_size);
        (void)write_text(1, "\ntile_rows=");
        (void)write_unsigned(1, image_info.tile_rows);
        (void)write_text(1, "\nbase_q_index=");
        (void)write_unsigned(1, image_info.base_q_index);
        (void)write_text(1, "\ncoded_lossless=");
        (void)write_unsigned(1, image_info.coded_lossless);
        (void)write_text(1, "\nallow_screen_content_tools=");
        (void)write_unsigned(1, image_info.allow_screen_content_tools);
        (void)write_text(1, "\nallow_intrabc=");
        (void)write_unsigned(1, image_info.allow_intrabc);
        (void)write_text(1, "\nsegmentation_enabled=");
        (void)write_unsigned(1, image_info.segmentation_enabled);
        (void)write_text(1, "\ndelta_q_present=");
        (void)write_unsigned(1, image_info.delta_q_present);
        (void)write_text(1, "\ndelta_lf_present=");
        (void)write_unsigned(1, image_info.delta_lf_present);
        (void)write_text(1, "\ntx_mode=");
        (void)write_unsigned(1, image_info.tx_mode);
        (void)write_text(1, "\nreduced_tx_set=");
        (void)write_unsigned(1, image_info.reduced_tx_set);
        (void)write_text(1, "\nworkspace_required=");
        (void)write_unsigned(1, image_info.workspace_required);
        (void)write_text(1, "\ndecode_workers=");
        (void)write_unsigned(
            1, decode_executor == 0
                ? 1U : decode_executor->worker_count);
        if (diagnostics ||
            (!raw_output && !png_output && packed_format < 0)) {
            write_entropy_trace(&entropy_trace);
        }
        if (raw_output) {
            (void)write_text(1, "\nraw_sample_bytes=");
            (void)write_unsigned(1, image.bit_depth == 8U ? 1U : 2U);
            (void)write_text(1, "\nraw_byte_order=little-endian\nraw_plane_order=YUV");
        } else if (png_output) {
            int png_16_bit =
                image_info.bit_depth > 8U ||
                (image_info.has_alpha &&
                 image_info.alpha_bit_depth > 8U);

            (void)write_text(1, "\npacked_format=");
            (void)write_text(
                1, image_info.has_alpha
                    ? (png_16_bit ? "png-rgba16" : "png-rgba8")
                    : (png_16_bit ? "png-rgb16" : "png-rgb8"));
        } else if (packed_format >= 0) {
            (void)write_text(1, "\npacked_format=");
            (void)write_text(
                1, packed_format == AVIFDEC_RGB8 ? "rgb8" :
                   packed_format == AVIFDEC_RGBA8 ? "rgba8" :
                   packed_format == AVIFDEC_RGB16 ? "rgb16" :
                   "rgba16");
        }
        (void)write_text(1, "\n");
        if (output_memory != 0) {
            (void)platform_free_pages(output_memory, output_memory_size);
        }
        if (packed_memory != 0) {
            (void)platform_free_pages(packed_memory, packed_memory_size);
        }
        (void)platform_free_pages(workspace, image_info.workspace_required);
        if (task_pool_initialized) {
            rt_task_pool_destroy(&task_pool);
        }
    }
    (void)platform_free_pages(data, size);
    if (print_context.io_failed) return 1;
    return 0;
}