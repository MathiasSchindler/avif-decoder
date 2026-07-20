#define _POSIX_C_SOURCE 200809L

#include "encoder/avifenc.h"
#include "encoder/image_input.h"
#include "avifdec.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCHMARK_PHOTO_PATH "images/image-check/tribu.png"

typedef enum {
    BENCHMARK_MINIMUM = 0,
    BENCHMARK_GRADIENT,
    BENCHMARK_TEXT,
    BENCHMARK_ANIMATION,
    BENCHMARK_NOISE,
    BENCHMARK_CHROMA,
    BENCHMARK_LARGE,
    BENCHMARK_PHOTO
} BenchmarkPattern;

typedef struct {
    const char *name;
    BenchmarkPattern pattern;
    uint32_t width;
    uint32_t height;
    uint16_t quantizer;
    uint8_t speed;
} BenchmarkCase;

typedef struct {
    AvifencImage image;
    uint8_t *storage;
} BenchmarkSource;

typedef struct {
    const BenchmarkCase *definition;
    AvifencRequirements requirements;
    AvifencStatistics statistics;
    size_t output_bytes;
    uint64_t output_checksum;
    uint64_t sse[3];
    uint64_t structure_error[3];
    uint64_t elapsed_ns;
    uint64_t pixels_per_second;
} BenchmarkResult;

static void benchmark_fail(const char *message);

typedef struct {
    AvifencParallelBody body;
    void *arg;
    size_t begin;
    size_t end;
    size_t worker_index;
    AvifencStatus status;
} BenchmarkThreadWork;

static void *benchmark_thread_worker(void *arg) {
    BenchmarkThreadWork *work = (BenchmarkThreadWork *)arg;

    work->status = work->body(
        work->begin, work->end, work->worker_index, work->arg);
    return NULL;
}

static AvifencStatus benchmark_parallel_for(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifencParallelBody body,
    void *arg) {
    size_t advertised_workers;
    size_t worker_count;
    pthread_t threads[AVIFENC_EXECUTOR_MAX_WORKERS - 1U];
    BenchmarkThreadWork work[AVIFENC_EXECUTOR_MAX_WORKERS];
    size_t index;

    if (user_data == NULL || min_chunk == 0U || body == NULL) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    advertised_workers = *(const size_t *)user_data;
    if (advertised_workers == 0U ||
        advertised_workers > AVIFENC_EXECUTOR_MAX_WORKERS) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    worker_count = advertised_workers < count ? advertised_workers : count;
    for (index = 0U; index < worker_count; ++index) {
        work[index].body = body;
        work[index].arg = arg;
        work[index].begin = index * count / worker_count;
        work[index].end = (index + 1U) * count / worker_count;
        work[index].worker_index = index;
        work[index].status = AVIFENC_OK;
    }
    for (index = 1U; index < worker_count; ++index) {
        if (pthread_create(
                &threads[index - 1U], NULL,
                benchmark_thread_worker, &work[index]) != 0) {
            benchmark_fail("cannot create benchmark worker");
        }
    }
    (void)benchmark_thread_worker(&work[0]);
    for (index = 1U; index < worker_count; ++index) {
        if (pthread_join(threads[index - 1U], NULL) != 0) {
            benchmark_fail("cannot join benchmark worker");
        }
    }
    for (index = 0U; index < worker_count; ++index) {
        if (work[index].status != AVIFENC_OK) return work[index].status;
    }
    return AVIFENC_OK;
}

static const BenchmarkCase benchmark_cases[] = {
    { "minimum", BENCHMARK_MINIMUM, 2U, 2U, 96U, 0U },
    { "gradient", BENCHMARK_GRADIENT, 64U, 48U, 96U, 0U },
    { "text-edge", BENCHMARK_TEXT, 66U, 50U, 96U, 1U },
    { "animation", BENCHMARK_ANIMATION, 64U, 48U, 96U, 1U },
    { "noise", BENCHMARK_NOISE, 64U, 48U, 128U, 2U },
    { "chroma-detail", BENCHMARK_CHROMA, 64U, 48U, 96U, 0U },
    { "large-practical", BENCHMARK_LARGE, 1024U, 768U, 128U, 2U },
    { "multi-tile-wide", BENCHMARK_LARGE, 8192U, 64U, 128U, 2U },
    { "photograph", BENCHMARK_PHOTO, 330U, 220U, 96U, 1U }
};

static void benchmark_fail(const char *message) {
    (void)fprintf(stderr, "encoder benchmark: %s\n", message);
    exit(1);
}

static void *benchmark_allocate(size_t size) {
    void *allocation = malloc(size == 0U ? 1U : size);

    if (allocation == NULL) benchmark_fail("allocation failed");
    return allocation;
}

static uint64_t benchmark_checksum(const uint8_t *data, size_t size) {
    uint64_t checksum = 1469598103934665603ULL;
    size_t index;

    for (index = 0U; index < size; ++index) {
        checksum ^= data[index];
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

static uint8_t benchmark_clamp(int32_t value) {
    if (value < 0) return 0U;
    if (value > 255) return 255U;
    return (uint8_t)value;
}

static int32_t benchmark_divide_256(int32_t value) {
    return value >= 0
        ? (value + 128) / 256
        : -((-value + 128) / 256);
}

static uint8_t benchmark_luma(uint8_t red,
                              uint8_t green,
                              uint8_t blue) {
    return benchmark_clamp(
        16 + (47 * (int32_t)red + 157 * (int32_t)green +
              16 * (int32_t)blue + 128) / 256);
}

static uint8_t benchmark_blue_difference(uint8_t red,
                                         uint8_t green,
                                         uint8_t blue) {
    return benchmark_clamp(
        128 + benchmark_divide_256(
            -26 * (int32_t)red - 87 * (int32_t)green +
            112 * (int32_t)blue));
}

static uint8_t benchmark_red_difference(uint8_t red,
                                        uint8_t green,
                                        uint8_t blue) {
    return benchmark_clamp(
        128 + benchmark_divide_256(
            112 * (int32_t)red - 102 * (int32_t)green -
            10 * (int32_t)blue));
}

static uint8_t *benchmark_read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;

    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        benchmark_fail("cannot open photographic fixture");
    }
    length = ftell(file);
    if (length <= 0L || fseek(file, 0L, SEEK_SET) != 0) {
        benchmark_fail("cannot size photographic fixture");
    }
    data = (uint8_t *)benchmark_allocate((size_t)length);
    if (fread(data, 1U, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        benchmark_fail("cannot read photographic fixture");
    }
    *size_out = (size_t)length;
    return data;
}

static void benchmark_rgb_to_yuv420(const uint8_t *rgb,
                                    const ImageInputInfo *info,
                                    BenchmarkSource *source) {
    uint8_t *y_plane = source->storage;
    uint8_t *u_plane = y_plane + (size_t)info->width * info->height;
    uint8_t *v_plane = u_plane +
        (size_t)(info->width / 2U) * (info->height / 2U);
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < info->height; ++row) {
        for (column = 0U; column < info->width; ++column) {
            const uint8_t *pixel = rgb +
                (size_t)row * info->rgb_stride + (size_t)column * 3U;

            y_plane[(size_t)row * info->width + column] =
                benchmark_luma(pixel[0], pixel[1], pixel[2]);
        }
    }
    for (row = 0U; row < info->height / 2U; ++row) {
        for (column = 0U; column < info->width / 2U; ++column) {
            uint32_t red = 0U;
            uint32_t green = 0U;
            uint32_t blue = 0U;
            unsigned int offset_y;

            for (offset_y = 0U; offset_y < 2U; ++offset_y) {
                unsigned int offset_x;

                for (offset_x = 0U; offset_x < 2U; ++offset_x) {
                    const uint8_t *pixel = rgb +
                        (size_t)(row * 2U + offset_y) * info->rgb_stride +
                        (size_t)(column * 2U + offset_x) * 3U;

                    red += pixel[0];
                    green += pixel[1];
                    blue += pixel[2];
                }
            }
            red = (red + 2U) / 4U;
            green = (green + 2U) / 4U;
            blue = (blue + 2U) / 4U;
            u_plane[(size_t)row * (info->width / 2U) + column] =
                benchmark_blue_difference(
                    (uint8_t)red, (uint8_t)green, (uint8_t)blue);
            v_plane[(size_t)row * (info->width / 2U) + column] =
                benchmark_red_difference(
                    (uint8_t)red, (uint8_t)green, (uint8_t)blue);
        }
    }
}

static void benchmark_source_init(BenchmarkSource *source,
                                  uint32_t width,
                                  uint32_t height) {
    size_t luma_size = (size_t)width * height;
    size_t chroma_size = (size_t)(width / 2U) * (height / 2U);

    (void)memset(source, 0, sizeof(*source));
    source->storage = (uint8_t *)benchmark_allocate(
        luma_size + 2U * chroma_size);
    source->image.planes[0] = source->storage;
    source->image.planes[1] = source->storage + luma_size;
    source->image.planes[2] = source->storage + luma_size + chroma_size;
    source->image.strides[0] = width;
    source->image.strides[1] = width / 2U;
    source->image.strides[2] = width / 2U;
    source->image.width = width;
    source->image.height = height;
    source->image.color.color_primaries = 1U;
    source->image.color.transfer_characteristics = 1U;
    source->image.color.matrix_coefficients = 1U;
}

static uint32_t benchmark_random(uint32_t *state) {
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static void benchmark_generate_source(const BenchmarkCase *definition,
                                      BenchmarkSource *source) {
    uint8_t *y_plane;
    uint8_t *u_plane;
    uint8_t *v_plane;
    uint32_t random_state = 0x6d2b79f5U;
    uint32_t row;
    uint32_t column;

    benchmark_source_init(source, definition->width, definition->height);
    y_plane = (uint8_t *)source->image.planes[0];
    u_plane = (uint8_t *)source->image.planes[1];
    v_plane = (uint8_t *)source->image.planes[2];
    for (row = 0U; row < definition->height; ++row) {
        for (column = 0U; column < definition->width; ++column) {
            uint32_t value;

            switch (definition->pattern) {
                case BENCHMARK_MINIMUM:
                    value = 128U;
                    break;
                case BENCHMARK_GRADIENT:
                    value = 16U +
                        (219U * (column + row)) /
                        (definition->width + definition->height - 2U);
                    break;
                case BENCHMARK_TEXT:
                    value = ((column / 3U) % 7U == 0U ||
                             (row / 3U) % 11U == 0U ||
                             (column > row && column < row + 3U))
                        ? 235U : 20U;
                    break;
                case BENCHMARK_ANIMATION:
                {
                    int32_t delta_x = (int32_t)column -
                        (int32_t)(definition->width / 2U);
                    int32_t delta_y = (int32_t)row -
                        (int32_t)(definition->height / 2U);

                    value = 32U + 48U *
                        ((column / 16U + row / 12U) % 4U);
                    if (delta_x * delta_x + delta_y * delta_y < 180) {
                        value = 220U;
                    }
                    break;
                }
                case BENCHMARK_NOISE:
                    value = 16U + (benchmark_random(&random_state) % 220U);
                    break;
                case BENCHMARK_CHROMA:
                    value = 128U;
                    break;
                case BENCHMARK_LARGE:
                    value = 16U + ((column * 5U + row * 3U +
                        ((column ^ row) & 31U) * 4U) % 220U);
                    break;
                case BENCHMARK_PHOTO:
                    value = 0U;
                    break;
            }
            y_plane[(size_t)row * definition->width + column] =
                (uint8_t)value;
        }
    }
    for (row = 0U; row < definition->height / 2U; ++row) {
        for (column = 0U; column < definition->width / 2U; ++column) {
            size_t index = (size_t)row * (definition->width / 2U) + column;

            if (definition->pattern == BENCHMARK_CHROMA) {
                u_plane[index] = (uint8_t)(
                    ((column + row) & 1U) != 0U ? 224U : 32U);
                v_plane[index] = (uint8_t)(
                    ((column / 2U + row / 2U) & 1U) != 0U ? 48U : 208U);
            } else if (definition->pattern == BENCHMARK_ANIMATION) {
                u_plane[index] = (uint8_t)(64U + 32U * (column / 8U % 4U));
                v_plane[index] = (uint8_t)(192U - 32U * (row / 6U % 4U));
            } else if (definition->pattern == BENCHMARK_NOISE) {
                u_plane[index] = (uint8_t)(
                    32U + benchmark_random(&random_state) % 192U);
                v_plane[index] = (uint8_t)(
                    32U + benchmark_random(&random_state) % 192U);
            } else if (definition->pattern == BENCHMARK_LARGE) {
                u_plane[index] = (uint8_t)(
                    64U + (column * 3U + row * 5U) % 128U);
                v_plane[index] = (uint8_t)(
                    192U - (column * 5U + row * 3U) % 128U);
            } else {
                u_plane[index] = 128U;
                v_plane[index] = 128U;
            }
        }
    }
}

static void benchmark_load_photo(const BenchmarkCase *definition,
                                 BenchmarkSource *source) {
    size_t file_size;
    uint8_t *file_data = benchmark_read_file(
        BENCHMARK_PHOTO_PATH, &file_size);
    ImageInputInfo info;
    uint8_t *workspace;
    uint8_t *rgb;

    if (image_input_query(file_data, file_size, &info) != IMAGE_INPUT_OK ||
        info.width != definition->width || info.height != definition->height ||
        (info.width & 1U) != 0U || (info.height & 1U) != 0U) {
        benchmark_fail("photographic fixture contract changed");
    }
    workspace = (uint8_t *)benchmark_allocate(info.workspace_size);
    rgb = (uint8_t *)benchmark_allocate(info.output_size);
    if (image_input_decode(
            file_data, file_size, workspace, info.workspace_size,
            rgb, info.output_size, &info) != IMAGE_INPUT_OK) {
        benchmark_fail("cannot decode photographic fixture");
    }
    benchmark_source_init(source, info.width, info.height);
    benchmark_rgb_to_yuv420(rgb, &info, source);
    free(rgb);
    free(workspace);
    free(file_data);
}

static void benchmark_source_create(const BenchmarkCase *definition,
                                    BenchmarkSource *source) {
    if (definition->pattern == BENCHMARK_PHOTO) {
        benchmark_load_photo(definition, source);
    } else {
        benchmark_generate_source(definition, source);
    }
}

static uint64_t benchmark_plane_sse(const uint8_t *source,
                                    size_t source_stride,
                                    const uint16_t *decoded,
                                    size_t decoded_stride,
                                    uint32_t width,
                                    uint32_t height) {
    uint64_t result = 0U;
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            int64_t difference =
                (int64_t)source[(size_t)row * source_stride + column] -
                (int64_t)decoded[(size_t)row * decoded_stride + column];

            result += (uint64_t)(difference * difference);
        }
    }
    return result;
}

static uint64_t benchmark_structure_error(const uint8_t *source,
                                          size_t source_stride,
                                          const uint16_t *decoded,
                                          size_t decoded_stride,
                                          uint32_t width,
                                          uint32_t height) {
    uint64_t result = 0U;
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            size_t source_index = (size_t)row * source_stride + column;
            size_t decoded_index = (size_t)row * decoded_stride + column;

            if (column != 0U) {
                int32_t source_gradient = (int32_t)source[source_index] -
                    source[source_index - 1U];
                int32_t decoded_gradient = (int32_t)decoded[decoded_index] -
                    decoded[decoded_index - 1U];
                int32_t difference = source_gradient - decoded_gradient;

                result += (uint64_t)(difference < 0 ? -difference : difference);
            }
            if (row != 0U) {
                int32_t source_gradient = (int32_t)source[source_index] -
                    source[source_index - source_stride];
                int32_t decoded_gradient = (int32_t)decoded[decoded_index] -
                    decoded[decoded_index - decoded_stride];
                int32_t difference = source_gradient - decoded_gradient;

                result += (uint64_t)(difference < 0 ? -difference : difference);
            }
        }
    }
    return result;
}

static uint64_t benchmark_reconstruction_checksum(const uint16_t *plane,
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

static uint64_t benchmark_psnr_milli(uint64_t sse, uint64_t samples) {
    double ratio;

    if (sse == 0U) return 99999U;
    ratio = (255.0 * 255.0 * (double)samples) / (double)sse;
    return (uint64_t)llround(10000.0 * log10(ratio));
}

static uint64_t benchmark_elapsed_ns(const struct timespec *start,
                                     const struct timespec *end) {
    int64_t seconds = (int64_t)end->tv_sec - start->tv_sec;
    int64_t nanoseconds = (int64_t)end->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000LL;
    }
    if (seconds < 0) return 0U;
    return (uint64_t)seconds * 1000000000ULL + (uint64_t)nanoseconds;
}

static void benchmark_run_case(const BenchmarkCase *definition,
                               unsigned int iterations,
                               const AvifencExecutor *executor,
                               BenchmarkResult *result) {
    BenchmarkSource source;
    AvifencOptions options;
    AvifencError error;
    AvifdecImageInfo decoded_info;
    AvifdecError decode_error;
    AvifdecImage decoded = { 0 };
    uint8_t *workspace;
    uint8_t *output;
    uint8_t *decode_workspace;
    uint16_t *decoded_storage;
    size_t luma_samples = (size_t)definition->width * definition->height;
    size_t chroma_samples =
        (size_t)(definition->width / 2U) * (definition->height / 2U);
    size_t output_written = 0U;
    struct timespec start;
    struct timespec end;
    unsigned int iteration;
    unsigned int plane;
    AvifencStatus encode_status;

    benchmark_source_create(definition, &source);
    avifenc_options_default(&options);
    options.quantizer = definition->quantizer;
    options.speed = definition->speed;
    (void)memset(result, 0, sizeof(*result));
    result->definition = definition;
        if (avifenc_query_with_executor(
            &source.image, &options, executor,
            &result->requirements, &error) !=
        AVIFENC_OK) {
        benchmark_fail("encoder query failed");
    }
    workspace = (uint8_t *)benchmark_allocate(
        result->requirements.workspace_required);
    output = (uint8_t *)benchmark_allocate(
        result->requirements.output_capacity_required);
    encode_status = avifenc_encode_with_executor(
            &source.image, &options, executor,
            workspace, result->requirements.workspace_required,
            output, result->requirements.output_capacity_required,
            &output_written, &result->statistics, &error);
    if (encode_status != AVIFENC_OK) {
        (void)fprintf(
            stderr, "encoder benchmark case %s: status %u, context %u, "
                    "required %zu, provided %zu, nodes %llu, blocks %llu, "
                    "prediction trials %llu, transform trials %llu\n",
            definition->name, (unsigned int)encode_status,
            (unsigned int)error.context, error.required_size,
                error.provided_size,
                (unsigned long long)result->statistics.partition_node_count,
                (unsigned long long)result->statistics.block_count,
                (unsigned long long)result->statistics.prediction_trial_count,
                (unsigned long long)result->statistics.transform_trial_count);
        benchmark_fail("encoder statistics operation failed");
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        benchmark_fail("cannot read monotonic clock");
    }
    for (iteration = 0U; iteration < iterations; ++iteration) {
        if (avifenc_encode_with_executor(
            &source.image, &options, executor,
                workspace, result->requirements.workspace_required,
                output, result->requirements.output_capacity_required,
            &output_written, 0, &error) != AVIFENC_OK) {
            benchmark_fail("encoder operation failed");
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        benchmark_fail("cannot read monotonic clock");
    }
    result->output_bytes = output_written;
    result->output_checksum = benchmark_checksum(output, output_written);
    result->elapsed_ns = benchmark_elapsed_ns(&start, &end);
    if (result->elapsed_ns != 0U) {
        result->pixels_per_second = (uint64_t)(
            ((double)luma_samples * iterations * 1000000000.0) /
            (double)result->elapsed_ns);
    }
    if (avifdec_query(
            output, output_written, 0, 0, 0U,
            &decoded_info, &decode_error) != AVIFDEC_OK) {
        benchmark_fail("decoder query failed");
    }
    decode_workspace = (uint8_t *)benchmark_allocate(
        decoded_info.workspace_required);
    decoded_storage = (uint16_t *)benchmark_allocate(
        (luma_samples + 2U * chroma_samples) * sizeof(uint16_t));
    decoded.planes[0] = decoded_storage;
    decoded.planes[1] = decoded_storage + luma_samples;
    decoded.planes[2] = decoded_storage + luma_samples + chroma_samples;
    decoded.strides[0] = definition->width;
    decoded.strides[1] = definition->width / 2U;
    decoded.strides[2] = definition->width / 2U;
    if (avifdec_decode(
            output, output_written, 0, decode_workspace,
            decoded_info.workspace_required, &decoded, 0, &decode_error) !=
        AVIFDEC_OK) {
        benchmark_fail("decoder operation failed");
    }
    for (plane = 0U; plane < 3U; ++plane) {
        uint32_t width = plane == 0U
            ? definition->width : definition->width / 2U;
        uint32_t height = plane == 0U
            ? definition->height : definition->height / 2U;

        result->sse[plane] = benchmark_plane_sse(
            source.image.planes[plane], source.image.strides[plane],
            decoded.planes[plane], decoded.strides[plane], width, height);
        result->structure_error[plane] = benchmark_structure_error(
            source.image.planes[plane], source.image.strides[plane],
            decoded.planes[plane], decoded.strides[plane], width, height);
        if (benchmark_reconstruction_checksum(
                decoded.planes[plane], decoded.strides[plane], width, height) !=
            result->statistics.reconstruction_checksum[plane]) {
            benchmark_fail("encoder and decoder reconstruction differ");
        }
    }
    free(decoded_storage);
    free(decode_workspace);
    free(output);
    free(workspace);
    free(source.storage);
}

static const char *benchmark_platform(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#else
    return "other";
#endif
}

static void benchmark_print_json(const BenchmarkResult *result,
                                 int stable) {
    const BenchmarkCase *definition = result->definition;
    uint64_t luma_samples = (uint64_t)definition->width * definition->height;
    uint64_t chroma_samples = luma_samples / 4U;
    uint64_t bits_per_pixel_milli =
        ((uint64_t)result->output_bytes * 8000U + luma_samples / 2U) /
        luma_samples;

    (void)printf(
        "{\"type\":\"case\",\"name\":\"%s\",\"width\":%u,"
        "\"height\":%u,\"quantizer\":%u,\"speed\":%u,"
        "\"workspace\":%zu,\"output_capacity\":%zu,"
        "\"output_bytes\":%zu,\"bits_per_pixel_milli\":%llu,"
        "\"checksum\":\"%016llx\",\"sse_y\":%llu,\"sse_u\":%llu,"
        "\"sse_v\":%llu,\"structure_y\":%llu,\"structure_u\":%llu,"
        "\"structure_v\":%llu,\"tiles\":%llu,"
        "\"partition_nodes\":%llu,\"blocks\":%llu,"
        "\"prediction_trials\":%llu,\"transform_trials\":%llu,"
        "\"transforms\":%llu,\"entropy_symbols\":%llu,"
        "\"literal_bits\":%llu,\"filter_units\":%llu",
        definition->name, definition->width, definition->height,
        definition->quantizer, definition->speed,
        result->requirements.workspace_required,
        result->requirements.output_capacity_required,
        result->output_bytes, (unsigned long long)bits_per_pixel_milli,
        (unsigned long long)result->output_checksum,
        (unsigned long long)result->sse[0],
        (unsigned long long)result->sse[1],
        (unsigned long long)result->sse[2],
        (unsigned long long)result->structure_error[0],
        (unsigned long long)result->structure_error[1],
        (unsigned long long)result->structure_error[2],
        (unsigned long long)result->statistics.tile_count,
        (unsigned long long)result->statistics.partition_node_count,
        (unsigned long long)result->statistics.block_count,
        (unsigned long long)result->statistics.prediction_trial_count,
        (unsigned long long)result->statistics.transform_trial_count,
        (unsigned long long)result->statistics.transform_count,
        (unsigned long long)result->statistics.entropy_symbol_count,
        (unsigned long long)result->statistics.literal_bit_count,
        (unsigned long long)result->statistics.filter_unit_count);
    if (!stable) {
        (void)printf(
            ",\"psnr_y_milli_db\":%llu,\"psnr_u_milli_db\":%llu,"
            "\"psnr_v_milli_db\":%llu,\"elapsed_ns\":%llu,"
            "\"megapixels_per_second_milli\":%llu",
            (unsigned long long)benchmark_psnr_milli(
                result->sse[0], luma_samples),
            (unsigned long long)benchmark_psnr_milli(
                result->sse[1], chroma_samples),
            (unsigned long long)benchmark_psnr_milli(
                result->sse[2], chroma_samples),
            (unsigned long long)result->elapsed_ns,
            (unsigned long long)(result->pixels_per_second / 1000U));
    }
    (void)printf("}\n");
}

static void benchmark_print_human_header(unsigned int iterations) {
    (void)printf("encoder scorecard (%s, %u iteration%s)\n",
                 benchmark_platform(), iterations,
                 iterations == 1U ? "" : "s");
    (void)printf(
        "%-14s %9s %4s %5s %9s %9s %10s %9s %8s\n",
        "case", "dimensions", "spd", "q", "bytes", "ms", "MP/s",
        "Y-PSNR", "trials");
}

static void benchmark_print_human(const BenchmarkResult *result,
                                  unsigned int iterations) {
    const BenchmarkCase *definition = result->definition;
    uint64_t luma_samples = (uint64_t)definition->width * definition->height;
    double milliseconds = (double)result->elapsed_ns / 1000000.0;
    double megapixels_per_second =
        (double)result->pixels_per_second / 1000000.0;
    double psnr = (double)benchmark_psnr_milli(
        result->sse[0], luma_samples) / 1000.0;

    (void)iterations;
    (void)printf(
        "%-14s %4ux%-4u %4u %5u %9zu %9.3f %10.3f %9.3f %8llu\n",
        definition->name, definition->width, definition->height,
        definition->speed, definition->quantizer, result->output_bytes,
        milliseconds, megapixels_per_second, psnr,
        (unsigned long long)result->statistics.prediction_trial_count);
}

static unsigned int benchmark_parse_iterations(const char *text) {
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0' ||
        value == 0UL || value > 1000UL) {
        benchmark_fail("iterations must be in 1..1000");
    }
    return (unsigned int)value;
}

int main(int argc, char **argv) {
    enum { OUTPUT_HUMAN, OUTPUT_JSON, OUTPUT_STABLE_JSON } output = OUTPUT_HUMAN;
    unsigned int iterations = 1U;
    size_t workers = 1U;
    AvifencExecutor executor;
    const AvifencExecutor *encode_executor = NULL;
    uint64_t total_ns = 0U;
    uint64_t total_pixels = 0U;
    size_t index;
    int argument;

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--human") == 0) {
            output = OUTPUT_HUMAN;
        } else if (strcmp(argv[argument], "--json") == 0) {
            output = OUTPUT_JSON;
        } else if (strcmp(argv[argument], "--stable-json") == 0) {
            output = OUTPUT_STABLE_JSON;
        } else if (strcmp(argv[argument], "--iterations") == 0 &&
                   argument + 1 < argc) {
            iterations = benchmark_parse_iterations(argv[++argument]);
        } else if (strcmp(argv[argument], "--workers") == 0 &&
                   argument + 1 < argc) {
            workers = benchmark_parse_iterations(argv[++argument]);
            if (workers > AVIFENC_EXECUTOR_MAX_WORKERS) {
                benchmark_fail("workers must be in 1..32");
            }
        } else {
            benchmark_fail(
                "usage: encoder-benchmark [--human|--json|--stable-json] "
                "[--iterations N] [--workers N]");
        }
    }
    if (output == OUTPUT_STABLE_JSON && workers != 1U) {
        benchmark_fail("stable JSON requires one worker");
    }
    if (workers > 1U) {
        executor.user_data = &workers;
        executor.worker_count = workers;
        executor.parallel_for = benchmark_parallel_for;
        encode_executor = &executor;
    }
    if (output == OUTPUT_HUMAN) benchmark_print_human_header(iterations);
    for (index = 0U;
         index < sizeof(benchmark_cases) / sizeof(benchmark_cases[0]);
         ++index) {
        BenchmarkResult result;

        benchmark_run_case(
            &benchmark_cases[index], iterations, encode_executor, &result);
        total_ns += result.elapsed_ns;
        total_pixels += (uint64_t)benchmark_cases[index].width *
            benchmark_cases[index].height * iterations;
        if (output == OUTPUT_HUMAN) {
            benchmark_print_human(&result, iterations);
        } else {
            benchmark_print_json(&result, output == OUTPUT_STABLE_JSON);
        }
    }
    if (output == OUTPUT_HUMAN) {
        double total_ms = (double)total_ns / 1000000.0;
        double throughput = total_ns == 0U ? 0.0 :
            ((double)total_pixels * 1000.0) / (double)total_ns;

        (void)printf(
            "total: %.3f ms, %.3f MP/s, workers=%zu\n",
            total_ms, throughput, workers);
    } else if (output == OUTPUT_JSON) {
        (void)printf(
            "{\"type\":\"summary\",\"platform\":\"%s\","
            "\"iterations\":%u,\"workers\":%zu,\"elapsed_ns\":%llu,"
            "\"megapixels_per_second_milli\":%llu}\n",
            benchmark_platform(), iterations, workers,
            (unsigned long long)total_ns,
            (unsigned long long)(total_ns == 0U ? 0U :
                (total_pixels * 1000000ULL / total_ns)));
    }
    return 0;
}
