#include "platform/platform.h"

enum {
    MACOS_UL_COMPARE_AND_WAIT = 1U,
    MACOS_ULF_WAKE_ALL = 0x00000100U
};

extern int pthread_create(
    void **thread,
    const void *attributes,
    void *(*entry)(void *),
    void *arg);
extern int pthread_join(void *thread, void **result);
extern int __ulock_wait(
    unsigned int operation,
    volatile void *address,
    unsigned long long value,
    unsigned int timeout);
extern int __ulock_wake(
    unsigned int operation,
    volatile void *address,
    unsigned long long wake_value);
extern int sysctlbyname(
    const char *name,
    void *old_value,
    size_t *old_size,
    void *new_value,
    size_t new_size);

static void *macos_worker_entry(void *arg) {
    PlatformWorkerThread *thread = (PlatformWorkerThread *)arg;

    thread->result = thread->entry(thread->arg);
    return 0;
}

int platform_worker_threads_supported(void) {
    return 1;
}

unsigned int platform_worker_thread_count(void) {
    unsigned int count = 0U;
    size_t count_size = sizeof(count);

    if (sysctlbyname(
            "hw.logicalcpu", &count, &count_size,
            0, 0U) != 0 || count == 0U) {
        return 2U;
    }
    return count;
}

void platform_wait_word(
    volatile unsigned int *word,
    unsigned int expected) {
    (void)__ulock_wait(
        MACOS_UL_COMPARE_AND_WAIT, word,
        expected, 0U);
}

void platform_wake_word_count(
    volatile unsigned int *word,
    unsigned int count) {
    if (count != 0U) {
        (void)__ulock_wake(
            MACOS_UL_COMPARE_AND_WAIT | MACOS_ULF_WAKE_ALL,
            word, 0U);
    }
}

void platform_wake_word_all(
    volatile unsigned int *word) {
    (void)__ulock_wake(
        MACOS_UL_COMPARE_AND_WAIT | MACOS_ULF_WAKE_ALL,
        word, 0U);
}

int platform_worker_thread_start(
    PlatformWorkerThread *thread,
    PlatformWorkerMain entry,
    void *arg,
    size_t stack_size) {
    int status;

    if (thread == 0 || entry == 0) return -1;
    (void)stack_size;
    thread->handle = 0;
    thread->entry = entry;
    thread->arg = arg;
    thread->result = 0;
    status = pthread_create(
        &thread->handle, 0,
        macos_worker_entry, thread);
    if (status != 0) {
        thread->handle = 0;
        thread->entry = 0;
        thread->arg = 0;
        return -1;
    }
    return 0;
}

int platform_worker_thread_join(
    PlatformWorkerThread *thread,
    int *result_out) {
    if (thread == 0 || thread->handle == 0) return -1;
    if (pthread_join(thread->handle, 0) != 0) return -1;
    if (result_out != 0) *result_out = thread->result;
    thread->handle = 0;
    thread->entry = 0;
    thread->arg = 0;
    return 0;
}
