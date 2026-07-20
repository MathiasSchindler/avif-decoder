#include "encoder/avifenc.h"
#include "avifdec.h"
#include "base.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 4160U
#define HEIGHT 2U

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "encoder parallel test failed at line %d\n", \
                      __LINE__); \
        return 1; \
    } \
} while (0)

typedef struct {
    size_t worker_count;
} FakeExecutor;

static AvifencStatus fake_parallel_for(void *user_data,
                                       size_t count,
                                       size_t min_chunk,
                                       AvifencParallelBody body,
                                       void *arg) {
    FakeExecutor *executor = (FakeExecutor *)user_data;
    size_t index;

    if (executor == NULL || executor->worker_count == 0U ||
        min_chunk == 0U || body == NULL) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    for (index = count; index != 0U; --index) {
        AvifencStatus status = body(
            index - 1U, index, (index - 1U) % executor->worker_count, arg);

        if (status != AVIFENC_OK) return status;
    }
    return AVIFENC_OK;
}

typedef struct {
    AvifencParallelBody body;
    void *arg;
    size_t begin;
    size_t end;
    size_t worker_index;
    AvifencStatus status;
} PthreadWork;

static void *pthread_worker(void *arg) {
    PthreadWork *work = (PthreadWork *)arg;

    work->status = work->body(
        work->begin, work->end, work->worker_index, work->arg);
    return NULL;
}

static AvifencStatus pthread_parallel_for(void *user_data,
                                          size_t count,
                                          size_t min_chunk,
                                          AvifencParallelBody body,
                                          void *arg) {
    FakeExecutor *executor = (FakeExecutor *)user_data;
    pthread_t threads[AVIFENC_EXECUTOR_MAX_WORKERS - 1U];
    PthreadWork work[AVIFENC_EXECUTOR_MAX_WORKERS];
    size_t worker_count;
    size_t index;

    if (executor == NULL || executor->worker_count == 0U ||
        min_chunk == 0U || body == NULL) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    worker_count = executor->worker_count < count
        ? executor->worker_count : count;
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
                pthread_worker, &work[index]) != 0) {
            return AVIFENC_INVALID_ARGUMENT;
        }
    }
    (void)pthread_worker(&work[0]);
    for (index = 1U; index < worker_count; ++index) {
        if (pthread_join(threads[index - 1U], NULL) != 0) {
            return AVIFENC_INVALID_ARGUMENT;
        }
    }
    for (index = 0U; index < worker_count; ++index) {
        if (work[index].status != AVIFENC_OK) return work[index].status;
    }
    return AVIFENC_OK;
}

static void *allocate(size_t size) {
    void *result = malloc(size == 0U ? 1U : size);

    if (result == NULL) {
        (void)fprintf(stderr, "encoder parallel test allocation failed\n");
        exit(1);
    }
    return result;
}

int main(void) {
    size_t luma_size = (size_t)WIDTH * HEIGHT;
    size_t chroma_size = luma_size / 4U;
    uint8_t *source = (uint8_t *)allocate(luma_size + 2U * chroma_size);
    AvifencImage image = { 0 };
    AvifencOptions options;
    AvifencRequirements serial_requirements;
    AvifencRequirements parallel_requirements;
    AvifencStatistics serial_statistics;
    AvifencStatistics parallel_statistics;
    AvifencStatistics concurrent_statistics;
    AvifencError error;
    FakeExecutor fake = { 2U };
    AvifencExecutor executor = { &fake, 2U, fake_parallel_for };
    void *serial_workspace;
    void *parallel_workspace;
    void *concurrent_workspace;
    uint8_t *serial_output;
    uint8_t *parallel_output;
    uint8_t *concurrent_output;
    AvifdecImageInfo info;
    AvifdecError decode_error;
    void *decode_workspace;
    uint16_t *decoded_storage;
    AvifdecImage decoded = { 0 };
    size_t serial_written;
    size_t parallel_written;
    size_t concurrent_written;
    size_t index;

    for (index = 0U; index < luma_size; ++index) {
        source[index] = (uint8_t)(index * 17U + index / 31U);
    }
    for (index = 0U; index < chroma_size; ++index) {
        source[luma_size + index] = (uint8_t)(96U + index % 53U);
        source[luma_size + chroma_size + index] =
            (uint8_t)(160U - index % 47U);
    }
    image.planes[0] = source;
    image.planes[1] = source + luma_size;
    image.planes[2] = source + luma_size + chroma_size;
    image.strides[0] = WIDTH;
    image.strides[1] = WIDTH / 2U;
    image.strides[2] = WIDTH / 2U;
    image.width = WIDTH;
    image.height = HEIGHT;
    image.color.color_primaries = 1U;
    image.color.transfer_characteristics = 1U;
    image.color.matrix_coefficients = 1U;
    avifenc_options_default(&options);
    options.speed = AVIFENC_MAX_SPEED;
    options.quantization.matrix_mode = 2U;
    options.quantization.adaptive_quantization = 1U;
    options.quantization.aq_strength = 8U;
    options.rate_control.mode = 1U;
    options.rate_control.target_quality = 7000U;

    CHECK(avifenc_query(
        &image, &options, &serial_requirements, &error) == AVIFENC_OK);
    CHECK(avifenc_query_with_executor(
        &image, &options, &executor,
        &parallel_requirements, &error) == AVIFENC_OK);
    CHECK(parallel_requirements.workspace_required >
          serial_requirements.workspace_required);
    CHECK(parallel_requirements.output_capacity_required ==
          serial_requirements.output_capacity_required);

    serial_workspace = allocate(serial_requirements.workspace_required);
    parallel_workspace = allocate(parallel_requirements.workspace_required);
    concurrent_workspace = allocate(parallel_requirements.workspace_required);
    serial_output = (uint8_t *)allocate(
        serial_requirements.output_capacity_required);
    parallel_output = (uint8_t *)allocate(
        parallel_requirements.output_capacity_required);
    concurrent_output = (uint8_t *)allocate(
        parallel_requirements.output_capacity_required);
    CHECK(avifenc_encode_ex(
        &image, &options, serial_workspace,
        serial_requirements.workspace_required,
        serial_output, serial_requirements.output_capacity_required,
        &serial_written, &serial_statistics, &error) == AVIFENC_OK);
    CHECK(avifenc_encode_with_executor(
        &image, &options, &executor, parallel_workspace,
        parallel_requirements.workspace_required,
        parallel_output, parallel_requirements.output_capacity_required,
        &parallel_written, &parallel_statistics, &error) == AVIFENC_OK);
    CHECK(serial_written == parallel_written);
    CHECK(avifdec_memory_compare(
        serial_output, parallel_output, serial_written) == 0);
    CHECK(avifdec_memory_compare(
        &serial_statistics, &parallel_statistics,
        sizeof(serial_statistics)) == 0);
    CHECK(serial_statistics.tile_count == 2U);
        CHECK(serial_statistics.encode_pass_count <= 5U &&
            serial_statistics.achieved_quality >= 7000U);

    executor.parallel_for = pthread_parallel_for;
    CHECK(avifenc_encode_with_executor(
        &image, &options, &executor, concurrent_workspace,
        parallel_requirements.workspace_required,
        concurrent_output, parallel_requirements.output_capacity_required,
        &concurrent_written, &concurrent_statistics, &error) == AVIFENC_OK);
    CHECK(serial_written == concurrent_written);
    CHECK(avifdec_memory_compare(
        serial_output, concurrent_output, serial_written) == 0);
    CHECK(avifdec_memory_compare(
        &serial_statistics, &concurrent_statistics,
        sizeof(serial_statistics)) == 0);

    CHECK(avifdec_query(
        parallel_output, parallel_written, 0, 0, 0U,
        &info, &decode_error) == AVIFDEC_OK);
    CHECK(info.width == WIDTH && info.height == HEIGHT &&
          info.tile_columns == 2U && info.tile_rows == 1U);
    decode_workspace = allocate(info.workspace_required);
    decoded_storage = (uint16_t *)allocate(
        (luma_size + 2U * chroma_size) * sizeof(uint16_t));
    decoded.planes[0] = decoded_storage;
    decoded.planes[1] = decoded_storage + luma_size;
    decoded.planes[2] = decoded_storage + luma_size + chroma_size;
    decoded.strides[0] = WIDTH;
    decoded.strides[1] = WIDTH / 2U;
    decoded.strides[2] = WIDTH / 2U;
    CHECK(avifdec_decode(
        parallel_output, parallel_written, 0, decode_workspace,
        info.workspace_required, &decoded, 0, &decode_error) == AVIFDEC_OK);
    CHECK(decoded.widths[0] == WIDTH && decoded.heights[0] == HEIGHT);

    executor.worker_count = 0U;
    CHECK(avifenc_query_with_executor(
        &image, &options, &executor,
        &parallel_requirements, &error) == AVIFENC_INVALID_ARGUMENT);
    CHECK(error.context == AVIFENC_CONTEXT_EXECUTOR);
    executor.worker_count = AVIFENC_EXECUTOR_MAX_WORKERS + 1U;
    CHECK(avifenc_query_with_executor(
        &image, &options, &executor,
        &parallel_requirements, &error) == AVIFENC_INVALID_ARGUMENT);
    executor.worker_count = 2U;
    executor.parallel_for = NULL;
    CHECK(avifenc_query_with_executor(
        &image, &options, &executor,
        &parallel_requirements, &error) == AVIFENC_INVALID_ARGUMENT);

    free(decoded_storage);
    free(decode_workspace);
    free(concurrent_output);
    free(concurrent_workspace);
    free(parallel_output);
    free(serial_output);
    free(parallel_workspace);
    free(serial_workspace);
    free(source);
    return 0;
}
