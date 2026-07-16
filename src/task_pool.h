#ifndef AVIFDEC_TASK_POOL_H
#define AVIFDEC_TASK_POOL_H

#include "platform.h"
#include <stddef.h>

#define RT_TASK_POOL_MAX_WORKERS 32U
#define RT_TASK_POOL_DEFAULT_STACK_SIZE (512U * 1024U)

typedef int (*RtParallelBody)(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg);

typedef struct {
    unsigned int index;
    struct RtTaskPool *pool;
    PlatformWorkerThread thread;
} RtTaskWorker;

typedef struct RtTaskPool {
    unsigned int width;
    unsigned int native_width;
    unsigned int stop;
    unsigned int generation;
    unsigned int active_workers;
    unsigned int finished_workers;
    unsigned long long claim_state;
    int first_error;
    size_t next_index;
    size_t count;
    size_t min_chunk;
    RtParallelBody body;
    void *work_arg;
    RtTaskWorker workers[RT_TASK_POOL_MAX_WORKERS];
} RtTaskPool;

unsigned int rt_task_pool_available_width(void);
int rt_task_pool_init(RtTaskPool *pool, unsigned int worker_count);
void rt_task_pool_destroy(RtTaskPool *pool);
unsigned int rt_task_pool_width(const RtTaskPool *pool);
int rt_parallel_for(
    RtTaskPool *pool,
    size_t count,
    size_t min_chunk,
    RtParallelBody body,
    void *arg);

#endif
