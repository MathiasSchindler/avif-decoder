#include "platform.h"

int platform_worker_threads_supported(void) {
    return 0;
}

unsigned int platform_worker_thread_count(void) {
    return 1U;
}

void platform_wait_word(
    volatile unsigned int *word,
    unsigned int expected) {
    (void)word;
    (void)expected;
}

void platform_wake_word_count(
    volatile unsigned int *word,
    unsigned int count) {
    (void)word;
    (void)count;
}

void platform_wake_word_all(
    volatile unsigned int *word) {
    (void)word;
}

int platform_worker_thread_start(
    PlatformWorkerThread *thread,
    PlatformWorkerMain entry,
    void *arg,
    size_t stack_size) {
    (void)thread;
    (void)entry;
    (void)arg;
    (void)stack_size;
    return -1;
}

int platform_worker_thread_join(
    PlatformWorkerThread *thread,
    int *result_out) {
    (void)thread;
    if (result_out != 0) *result_out = -1;
    return -1;
}
