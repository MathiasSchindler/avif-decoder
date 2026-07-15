#include "platform.h"
#include "common.h"

long platform_write(int fd, const void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_WRITE, fd, (long)buffer, (long)count);
}

long platform_read(int fd, void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_READ, fd, (long)buffer, (long)count);
}

size_t platform_page_size(void) {
    return 16384U;
}

void *platform_allocate_pages(size_t size) {
    long mapped = darwin_syscall6(
        DARWIN_SYS_MMAP,
        0,
        (long)size,
        DARWIN_PROT_READ | DARWIN_PROT_WRITE,
        DARWIN_MAP_PRIVATE | DARWIN_MAP_ANONYMOUS,
        -1,
        0
    );
    return mapped < 0 ? 0 : (void *)mapped;
}

int platform_free_pages(void *ptr, size_t size) {
    if (ptr == 0 || size == 0U) return 0;
    return darwin_syscall2(DARWIN_SYS_MUNMAP, (long)ptr, (long)size) < 0 ? -1 : 0;
}

int platform_open_read(const char *path) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 0;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, DARWIN_O_RDONLY, 0);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing) {
    long fd;
    long flags = DARWIN_O_WRONLY | DARWIN_O_CREAT;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    if (truncate_existing) {
        flags |= DARWIN_O_TRUNC;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, flags, (long)mode);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_write_untraced(const char *path, unsigned int mode, int truncate_existing) {
    return platform_open_write_mode(path, mode, truncate_existing);
}

int platform_open_write(const char *path, unsigned int mode) {
    return platform_open_write_mode(path, mode, 1);
}

int platform_open_create_exclusive(const char *path, unsigned int mode) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return -1;
    }

    fd = darwin_syscall3(
        DARWIN_SYS_OPEN,
        (long)path,
        DARWIN_O_WRONLY | DARWIN_O_CREAT | DARWIN_O_EXCL,
        (long)mode
    );
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_append(const char *path, unsigned int mode) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    fd = darwin_syscall3(
        DARWIN_SYS_OPEN,
        (long)path,
        DARWIN_O_WRONLY | DARWIN_O_CREAT | DARWIN_O_APPEND,
        (long)mode
    );
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_append_existing(const char *path) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    fd = darwin_syscall3(
        DARWIN_SYS_OPEN,
        (long)path,
        DARWIN_O_WRONLY | DARWIN_O_APPEND,
        0
    );
    return fd < 0 ? -1 : (int)fd;
}

long long platform_seek(int fd, long long offset, int whence) {
    long result = darwin_syscall3(DARWIN_SYS_LSEEK, fd, (long)offset, whence);
    return result < 0 ? -1 : (long long)result;
}

int platform_close(int fd) {
    if (fd == 0 || fd == 1) {
        return 0;
    }

    return darwin_syscall1(DARWIN_SYS_CLOSE, fd) < 0 ? -1 : 0;
}