#include "base.h"
#include "task_pool.h"

#define THREAD_TEST_ITEMS 4096U

typedef struct {
    unsigned char visits[THREAD_TEST_ITEMS];
    size_t worker_items[RT_TASK_POOL_MAX_WORKERS];
    size_t fail_index;
    volatile unsigned int worker_seen;
    volatile unsigned int main_seen;
    unsigned int require_parallel;
} ThreadTestContext;

static int visit_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg) {
    ThreadTestContext *context =
        (ThreadTestContext *)arg;
    size_t index;

    if (worker_index >= RT_TASK_POOL_MAX_WORKERS) return 1;
    if (context->require_parallel && worker_index == 0U) {
        __atomic_store_n(
            &context->main_seen, 1U,
            __ATOMIC_RELEASE);
        while (__atomic_load_n(
                   &context->worker_seen,
                   __ATOMIC_ACQUIRE) == 0U) {
        }
    } else if (context->require_parallel) {
        __atomic_store_n(
            &context->worker_seen, 1U,
            __ATOMIC_RELEASE);
        while (__atomic_load_n(
                   &context->main_seen,
                   __ATOMIC_ACQUIRE) == 0U) {
        }
    }
    for (index = begin; index < end; ++index) {
        if (context->visits[index] != 0U) return 2;
        context->visits[index] = 1U;
        ++context->worker_items[worker_index];
    }
    return 0;
}

static int fail_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg) {
    ThreadTestContext *context =
        (ThreadTestContext *)arg;

    (void)worker_index;
    return context->fail_index >= begin &&
           context->fail_index < end ? 7 : 0;
}

int main(void) {
    RtTaskPool pool;
    ThreadTestContext context;
    unsigned int width;
    unsigned int active_workers = 0U;
    unsigned int worker_index;
    size_t index;

    int result = 0;

    if (rt_task_pool_init(&pool, 4U) != 0) return __LINE__;
    width = rt_task_pool_width(&pool);
    if (width == 0U || width > 4U ||
        (platform_worker_threads_supported() && width < 2U) ||
        (!platform_worker_threads_supported() && width != 1U)) {
        result = __LINE__;
        goto cleanup;
    }
    avifdec_memory_fill(&context, 0U, sizeof(context));
    context.require_parallel = width > 1U;
    if (rt_parallel_for(
            &pool, THREAD_TEST_ITEMS, 1U,
            visit_range, &context) != 0) {
        result = __LINE__;
        goto cleanup;
    }
    for (index = 0U; index < THREAD_TEST_ITEMS; ++index) {
        if (context.visits[index] != 1U) {
            result = __LINE__;
            goto cleanup;
        }
    }
    for (worker_index = 0U;
         worker_index < width;
         ++worker_index) {
        if (context.worker_items[worker_index] != 0U) {
            ++active_workers;
        }
    }
    if ((width > 1U && active_workers < 2U) ||
        (width == 1U && active_workers != 1U)) {
        result = __LINE__;
        goto cleanup;
    }
    context.fail_index = THREAD_TEST_ITEMS / 2U;
    if (rt_parallel_for(
            &pool, THREAD_TEST_ITEMS, 3U,
            fail_range, &context) != 7) {
        result = __LINE__;
    }
cleanup:
    rt_task_pool_destroy(&pool);
    return result;
}
