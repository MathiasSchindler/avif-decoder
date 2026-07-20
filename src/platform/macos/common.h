#ifndef AVIFDEC_PLATFORM_MACOS_COMMON_H
#define AVIFDEC_PLATFORM_MACOS_COMMON_H

#include "platform/platform.h"
#include "../../arch/aarch64/macos/syscall.h"

#define DARWIN_O_RDONLY 0
#define DARWIN_O_WRONLY 0x0001
#define DARWIN_O_APPEND 0x0008
#define DARWIN_O_CREAT 0x0200
#define DARWIN_O_TRUNC 0x0400
#define DARWIN_O_EXCL 0x0800

#define DARWIN_PROT_READ 1
#define DARWIN_PROT_WRITE 2
#define DARWIN_MAP_PRIVATE 2
#define DARWIN_MAP_ANONYMOUS 0x1000

#endif