#ifndef AVIFDEC_PLATFORM_LINUX_COMMON_H
#define AVIFDEC_PLATFORM_LINUX_COMMON_H

#include "platform.h"

#if defined(__x86_64__)
#include "../../arch/x86_64/linux/syscall.h"
#else
#error "Unsupported Linux architecture for syscall definitions"
#endif

#define LINUX_AT_FDCWD (-100)

#define LINUX_O_RDONLY 0
#define LINUX_O_WRONLY 1
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_APPEND 02000
#define LINUX_O_TRUNC 01000
#define LINUX_O_CLOEXEC 02000000

#define LINUX_PROT_READ 1
#define LINUX_PROT_WRITE 2
#define LINUX_MAP_PRIVATE 2
#define LINUX_MAP_ANONYMOUS 0x20

#endif