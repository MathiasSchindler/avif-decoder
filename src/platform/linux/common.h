#ifndef AVIFDEC_PLATFORM_LINUX_COMMON_H
#define AVIFDEC_PLATFORM_LINUX_COMMON_H

#include "platform/platform.h"

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

#define LINUX_CLONE_VM             0x00000100UL
#define LINUX_CLONE_FS             0x00000200UL
#define LINUX_CLONE_FILES          0x00000400UL
#define LINUX_CLONE_SIGHAND        0x00000800UL
#define LINUX_CLONE_THREAD         0x00010000UL
#define LINUX_CLONE_SYSVSEM        0x00040000UL
#define LINUX_CLONE_PARENT_SETTID  0x00100000UL
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000UL

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_PRIVATE_FLAG 128

#define LINUX_EINTR 4
#define LINUX_EAGAIN 11

#endif