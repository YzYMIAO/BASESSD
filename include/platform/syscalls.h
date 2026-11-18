#ifndef PLATFORM_SYSCALLS_H
#define PLATFORM_SYSCALLS_H

#include "ssd_types.h" /* DWORD */

/*
 * syscalls.h — 裸机 libc 系统调用桩的对外诊断接口
 *
 * _sbrk 等 libc 钩子由实现提供；本头只暴露需要被其他模块查询的堆使用量。
 */

/** @brief 当前堆已使用字节数（DTCM 堆；不含栈）。 */
DWORD syscalls_heap_used(void);

#endif /* PLATFORM_SYSCALLS_H */
