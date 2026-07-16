#include "task_pool.h"
#include "base.h"

/* Reduced from newos's structured parallel_for task-pool runtime. */
#define RT_TASK_POOL_CHUNKS_PER_WORKER 8U

static void rt_task_pool_clear(RtTaskPool *pool) {
    avifdec_memory_fill(pool, 0U, sizeof(*pool));
}

static void rt_task_pool_record_error(RtTaskPool *pool, int error) {
    int expected = 0;

    if (error != 0 &&
        __atomic_load_n(&pool->first_error, __ATOMIC_ACQUIRE) == 0) {
        (void)__atomic_compare_exchange_n(
            &pool->first_error, &expected, error, 0,
            __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}

static size_t rt_task_pool_effective_min_chunk(
    const RtTaskPool *pool,
    size_t count,
    size_t min_chunk) {
    unsigned int width =
        pool != 0 && pool->native_width != 0U
            ? pool->native_width : 1U;
    size_t target_chunks;
    size_t automatic_chunk;

    if (min_chunk == 0U) min_chunk = 1U;
    target_chunks = (size_t)width * RT_TASK_POOL_CHUNKS_PER_WORKER;
    if (target_chunks == 0U || count <= target_chunks) {
        return min_chunk;
    }
    automatic_chunk =
        (count + target_chunks - 1U) / target_chunks;
    return automatic_chunk > min_chunk
        ? automatic_chunk : min_chunk;
}

static unsigned int rt_task_pool_active_workers(
    const RtTaskPool *pool,
    size_t count,
    size_t chunk) {
    unsigned int width =
        pool != 0 && pool->native_width != 0U
            ? pool->native_width : 1U;
    size_t units;

    if (width <= 1U || count <= 1U) return 1U;
    if (chunk == 0U) chunk = 1U;
    units = (count + chunk - 1U) / chunk;
    return units < width ? (unsigned int)units : width;
}

static size_t rt_task_pool_next_chunk(
    RtTaskPool *pool,
    size_t *end_out) {
    size_t begin;
    size_t end;

    for (;;) {
        begin = __atomic_load_n(
            &pool->next_index, __ATOMIC_ACQUIRE);
        if (begin >= pool->count) {
            *end_out = begin;
            return begin;
        }
        end = begin + pool->min_chunk;
        if (end < begin || end > pool->count) end = pool->count;
        if (__atomic_compare_exchange_n(
                &pool->next_index, &begin, end, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            *end_out = end;
            return begin;
        }
    }
}

static void rt_task_pool_run(
    RtTaskPool *pool,
    unsigned int worker_index) {
    for (;;) {
        size_t end;
        size_t begin =
            rt_task_pool_next_chunk(pool, &end);

        if (begin >= end) break;
        rt_task_pool_record_error(
            pool,
            pool->body(
                begin, end, worker_index, pool->work_arg));
    }
}

static int rt_task_worker_main(void *arg) {
    RtTaskWorker *worker = (RtTaskWorker *)arg;
    RtTaskPool *pool = worker == 0 ? 0 : worker->pool;
    unsigned int observed_generation = 0U;

    if (pool == 0) return -1;
    for (;;) {
        unsigned int generation = __atomic_load_n(
            &pool->generation, __ATOMIC_ACQUIRE);

        while (generation == observed_generation &&
               __atomic_load_n(
                   &pool->stop, __ATOMIC_ACQUIRE) == 0U) {
            platform_wait_word(
                &pool->generation, observed_generation);
            generation = __atomic_load_n(
                &pool->generation, __ATOMIC_ACQUIRE);
        }
        if (__atomic_load_n(
                &pool->stop, __ATOMIC_ACQUIRE) != 0U) {
            break;
        }
        observed_generation = generation;
        for (;;) {
            unsigned long long claim =
                __atomic_load_n(
                    &pool->claim_state, __ATOMIC_ACQUIRE);
            unsigned int claim_generation =
                (unsigned int)(claim >> 32U);
            unsigned int participant =
                (unsigned int)claim;
            unsigned int active =
                __atomic_load_n(
                    &pool->active_workers, __ATOMIC_ACQUIRE);
            unsigned long long desired;

            if (claim_generation != observed_generation ||
                participant >= active) {
                break;
            }
            desired =
                ((unsigned long long)observed_generation << 32U) |
                (unsigned long long)(participant + 1U);
            if (__atomic_compare_exchange_n(
                    &pool->claim_state, &claim,
                    desired, 0,
                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                rt_task_pool_run(pool, participant);
                if (__atomic_fetch_add(
                        &pool->finished_workers, 1U,
                        __ATOMIC_ACQ_REL) + 1U >= active) {
                    platform_wake_word_all(
                        &pool->finished_workers);
                }
                break;
            }
        }
    }
    return 0;
}

unsigned int rt_task_pool_available_width(void) {
    unsigned int width = platform_worker_thread_count();

    if (width == 0U) width = 1U;
    if (width > RT_TASK_POOL_MAX_WORKERS) {
        width = RT_TASK_POOL_MAX_WORKERS;
    }
    return width;
}

int rt_task_pool_init(
    RtTaskPool *pool,
    unsigned int worker_count) {
    unsigned int index;

    if (pool == 0) return -1;
    rt_task_pool_clear(pool);
    if (worker_count == 0U) {
        worker_count = rt_task_pool_available_width();
    }
    if (worker_count == 0U) worker_count = 1U;
    if (worker_count > RT_TASK_POOL_MAX_WORKERS) {
        worker_count = RT_TASK_POOL_MAX_WORKERS;
    }
    if (!platform_worker_threads_supported()) {
        worker_count = 1U;
    }
    pool->width = worker_count;
    pool->native_width = 1U;
    pool->workers[0].pool = pool;
    if (worker_count == 1U) return 0;
    for (index = 1U; index < worker_count; ++index) {
        pool->workers[index].index = index;
        pool->workers[index].pool = pool;
        if (platform_worker_thread_start(
                &pool->workers[index].thread,
                rt_task_worker_main,
                &pool->workers[index],
                RT_TASK_POOL_DEFAULT_STACK_SIZE) != 0) {
            pool->native_width = index;
            rt_task_pool_destroy(pool);
            rt_task_pool_clear(pool);
            pool->width = 1U;
            pool->native_width = 1U;
            pool->workers[0].pool = pool;
            return -1;
        }
    }
    pool->native_width = worker_count;
    return 0;
}

void rt_task_pool_destroy(RtTaskPool *pool) {
    unsigned int index;

    if (pool == 0 || pool->width == 0U) return;
    __atomic_store_n(&pool->stop, 1U, __ATOMIC_RELEASE);
    (void)__atomic_fetch_add(
        &pool->generation, 1U, __ATOMIC_ACQ_REL);
    platform_wake_word_all(&pool->generation);
    for (index = 1U; index < pool->native_width; ++index) {
        (void)platform_worker_thread_join(
            &pool->workers[index].thread, 0);
    }
    rt_task_pool_clear(pool);
}

unsigned int rt_task_pool_width(const RtTaskPool *pool) {
    return pool == 0 || pool->native_width == 0U
        ? 1U : pool->native_width;
}

static int rt_parallel_for_serial(
    size_t count,
    size_t chunk,
    RtParallelBody body,
    void *arg) {
    size_t begin = 0U;
    int first_error = 0;

    while (begin < count) {
        size_t end = begin + chunk;
        int status;

        if (end < begin || end > count) end = count;
        status = body(begin, end, 0U, arg);
        if (first_error == 0 && status != 0) {
            first_error = status;
        }
        begin = end;
    }
    return first_error;
}

int rt_parallel_for(
    RtTaskPool *pool,
    size_t count,
    size_t min_chunk,
    RtParallelBody body,
    void *arg) {
    unsigned int expected;
    unsigned int generation;
    size_t chunk;

    if (body == 0) return -1;
    if (count == 0U) return 0;
    chunk = rt_task_pool_effective_min_chunk(
        pool, count, min_chunk);
    if (pool == 0 || pool->native_width <= 1U ||
        count <= chunk) {
        return rt_parallel_for_serial(
            count, chunk, body, arg);
    }
    pool->count = count;
    pool->min_chunk = chunk;
    pool->body = body;
    pool->work_arg = arg;
    pool->first_error = 0;
    pool->next_index = 0U;
    pool->active_workers =
        rt_task_pool_active_workers(pool, count, chunk);
    pool->finished_workers = 0U;
    generation = __atomic_load_n(
        &pool->generation, __ATOMIC_RELAXED) + 1U;
    __atomic_store_n(
        &pool->claim_state,
        ((unsigned long long)generation << 32U) | 1ULL,
        __ATOMIC_RELEASE);
    __atomic_store_n(
        &pool->generation, generation, __ATOMIC_RELEASE);
    platform_wake_word_count(
        &pool->generation, pool->active_workers - 1U);
    rt_task_pool_run(pool, 0U);
    if (__atomic_fetch_add(
            &pool->finished_workers, 1U,
            __ATOMIC_ACQ_REL) + 1U >= pool->active_workers) {
        platform_wake_word_all(&pool->finished_workers);
    }
    for (;;) {
        expected = __atomic_load_n(
            &pool->finished_workers, __ATOMIC_ACQUIRE);
        if (expected >= pool->active_workers) break;
        platform_wait_word(
            &pool->finished_workers, expected);
    }
    return pool->first_error;
}
