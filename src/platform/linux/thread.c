#include "platform/platform.h"
#include "common.h"

/* Reduced from newos's Linux clone/futex worker substrate. */
#define LINUX_THREAD_DEFAULT_STACK_SIZE (512U * 1024U)

typedef struct {
    PlatformWorkerMain entry;
    void *arg;
    int result;
} LinuxThreadStart;

static int linux_thread_entry(void *arg) {
    LinuxThreadStart *start = (LinuxThreadStart *)arg;

    start->result = start->entry(start->arg);
    return 0;
}

static size_t linux_page_align(size_t value) {
    const size_t page_size = 4096U;

    return (value + page_size - 1U) & ~(page_size - 1U);
}

static int linux_futex_wait(
    volatile int *address,
    int expected,
    int private_wait) {
    int operation = LINUX_FUTEX_WAIT;
    long result;

    if (private_wait) operation |= LINUX_FUTEX_PRIVATE_FLAG;
    result = linux_syscall6(
        LINUX_SYS_FUTEX, (long)address, operation,
        expected, 0, 0, 0);
    return result < 0 &&
           result != -LINUX_EAGAIN &&
           result != -LINUX_EINTR ? -1 : 0;
}

static int linux_futex_wake(
    volatile int *address,
    int count) {
    return linux_syscall6(
        LINUX_SYS_FUTEX, (long)address,
        LINUX_FUTEX_WAKE | LINUX_FUTEX_PRIVATE_FLAG,
        count, 0, 0, 0) < 0 ? -1 : 0;
}

int platform_worker_threads_supported(void) {
    return 1;
}

unsigned int platform_worker_thread_count(void) {
    unsigned long masks[16];
    unsigned int count = 0U;
    size_t index;

    for (index = 0U;
         index < sizeof(masks) / sizeof(masks[0]);
         ++index) {
        masks[index] = 0UL;
    }
    if (linux_syscall3(
            LINUX_SYS_SCHED_GETAFFINITY, 0,
            sizeof(masks), (long)masks) < 0) {
        return 2U;
    }
    for (index = 0U;
         index < sizeof(masks) / sizeof(masks[0]);
         ++index) {
        unsigned long value = masks[index];

        while (value != 0UL) {
            count += (unsigned int)(value & 1UL);
            value >>= 1U;
        }
    }
    return count == 0U ? 2U : count;
}

void platform_wait_word(
    volatile unsigned int *word,
    unsigned int expected) {
    (void)linux_futex_wait(
        (volatile int *)word, (int)expected, 1);
}

void platform_wake_word_count(
    volatile unsigned int *word,
    unsigned int count) {
    if (count != 0U) {
        (void)linux_futex_wake(
            (volatile int *)word, (int)count);
    }
}

void platform_wake_word_all(
    volatile unsigned int *word) {
    (void)linux_futex_wake(
        (volatile int *)word, 2147483647);
}

int platform_worker_thread_start(
    PlatformWorkerThread *thread,
    PlatformWorkerMain entry,
    void *arg,
    size_t stack_size) {
    unsigned char *stack;
    LinuxThreadStart *start;
    unsigned long flags;
    long tid;

    if (thread == 0 || entry == 0) return -1;
    if (stack_size == 0U) {
        stack_size = LINUX_THREAD_DEFAULT_STACK_SIZE;
    }
    stack_size = linux_page_align(stack_size);
    if (stack_size < sizeof(*start) + 4096U) {
        stack_size = linux_page_align(
            sizeof(*start) + 4096U);
    }
    stack = (unsigned char *)platform_allocate_pages(
        stack_size);
    if (stack == 0) return -1;
    start = (LinuxThreadStart *)stack;
    start->entry = entry;
    start->arg = arg;
    start->result = 0;
    thread->tid = -1;
    thread->clear_tid = 0;
    thread->stack = stack;
    thread->stack_size = stack_size;
    flags = LINUX_CLONE_VM |
            LINUX_CLONE_FS |
            LINUX_CLONE_FILES |
            LINUX_CLONE_SIGHAND |
            LINUX_CLONE_THREAD |
            LINUX_CLONE_SYSVSEM |
            LINUX_CLONE_PARENT_SETTID |
            LINUX_CLONE_CHILD_CLEARTID;
    tid = linux_clone_thread(
        (long)flags, stack + stack_size,
        (int *)&thread->clear_tid,
        linux_thread_entry, start);
    if (tid < 0) {
        (void)platform_free_pages(stack, stack_size);
        thread->stack = 0;
        thread->stack_size = 0U;
        return -1;
    }
    thread->tid = (int)tid;
    return 0;
}

int platform_worker_thread_join(
    PlatformWorkerThread *thread,
    int *result_out) {
    if (thread == 0) return -1;
    for (;;) {
        int value = __atomic_load_n(
            &thread->clear_tid, __ATOMIC_ACQUIRE);

        if (value == 0) break;
        if (value > 0 &&
            linux_futex_wait(
                &thread->clear_tid, value, 0) != 0) {
            return -1;
        }
    }
    if (result_out != 0 && thread->stack != 0) {
        *result_out =
            ((LinuxThreadStart *)thread->stack)->result;
    }
    if (thread->stack != 0) {
        (void)platform_free_pages(
            thread->stack, thread->stack_size);
    }
    thread->tid = 0;
    thread->stack = 0;
    thread->stack_size = 0U;
    return 0;
}
