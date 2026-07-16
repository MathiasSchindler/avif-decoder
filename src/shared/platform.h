#ifndef AVIFDEC_PLATFORM_H
#define AVIFDEC_PLATFORM_H

#include <stddef.h>

#define PLATFORM_SEEK_SET 0
#define PLATFORM_SEEK_CUR 1
#define PLATFORM_SEEK_END 2

typedef int (*PlatformWorkerMain)(void *arg);

typedef struct {
    int tid;
    volatile int clear_tid;
    void *stack;
    size_t stack_size;
} PlatformWorkerThread;

long platform_write(int fd, const void *buffer, size_t count);
long platform_read(int fd, void *buffer, size_t count);
size_t platform_page_size(void);
void *platform_allocate_pages(size_t size);
int platform_free_pages(void *ptr, size_t size);
int platform_open_read(const char *path);
int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing);
int platform_open_write_untraced(const char *path, unsigned int mode, int truncate_existing);
int platform_open_write(const char *path, unsigned int mode);
int platform_open_create_exclusive(const char *path, unsigned int mode);
int platform_open_append(const char *path, unsigned int mode);
int platform_open_append_existing(const char *path);
long long platform_seek(int fd, long long offset, int whence);
int platform_close(int fd);
int platform_worker_threads_supported(void);
unsigned int platform_worker_thread_count(void);
int platform_worker_thread_start(
    PlatformWorkerThread *thread,
    PlatformWorkerMain entry,
    void *arg,
    size_t stack_size);
int platform_worker_thread_join(
    PlatformWorkerThread *thread,
    int *result_out);
void platform_wait_word(
    volatile unsigned int *word,
    unsigned int expected);
void platform_wake_word_count(
    volatile unsigned int *word,
    unsigned int count);
void platform_wake_word_all(
    volatile unsigned int *word);

#endif