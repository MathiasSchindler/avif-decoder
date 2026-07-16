#ifndef AVIFDEC_ARCH_X86_64_LINUX_SYSCALL_H
#define AVIFDEC_ARCH_X86_64_LINUX_SYSCALL_H

#define LINUX_SYS_READ 0
#define LINUX_SYS_WRITE 1
#define LINUX_SYS_CLOSE 3
#define LINUX_SYS_LSEEK 8
#define LINUX_SYS_MMAP 9
#define LINUX_SYS_MUNMAP 11
#define LINUX_SYS_CLONE 56
#define LINUX_SYS_EXIT 60
#define LINUX_SYS_FUTEX 202
#define LINUX_SYS_SCHED_GETAFFINITY 204
#define LINUX_SYS_OPENAT 257

long linux_syscall0(long number);
long linux_syscall1(long number, long arg0);
long linux_syscall2(long number, long arg0, long arg1);
long linux_syscall3(long number, long arg0, long arg1, long arg2);
long linux_syscall4(long number, long arg0, long arg1, long arg2, long arg3);
long linux_syscall5(long number, long arg0, long arg1, long arg2, long arg3, long arg4);
long linux_syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5);
long linux_clone_thread(
    long flags,
    void *stack_top,
    int *child_tid,
    int (*entry)(void *),
    void *arg);

#endif