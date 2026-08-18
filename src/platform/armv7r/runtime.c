#include "platform/cpu_armv7r.h"
#include "platform/soc.h"

/*
 * 这些地址符号由 reference_soc.ld 定义，不会真正分配 C 对象：
 * __data_load__  是 .data 初始值在固件镜像中的加载地址 LMA；
 * __data_start__ 是 .data 在 SRAM 中的运行地址 VMA；
 * __data_end__   是 .data 运行区间的结束地址；
 * __bss_start__  和 __bss_end__ 定义需要清零的 SRAM 区间。
 */
extern unsigned char __data_load__;
extern unsigned char __data_start__;
extern unsigned char __data_end__;
extern unsigned char __bss_start__;
extern unsigned char __bss_end__;

/*
 * Reset_Handler 在 SVC 栈建立后调用本函数。函数返回之前，除局部变量外
 * 不应依赖普通全局状态，因为 .data/.bss 仍处于未初始化状态。
 */
void armv7r_runtime_init(void)
{
    unsigned char *source      = &__data_load__;
    unsigned char *destination = &__data_start__;
    unsigned char *end         = &__data_end__;

    /* 把固件镜像保存的非零初始值逐字节复制到 SRAM 运行地址。 */
    while (destination < end)
    {
        *destination++ = *source++;
    }

    /* 未初始化和零初始化全局变量不占镜像空间，启动时统一清零。 */
    destination = &__bss_start__;
    end         = &__bss_end__;
    while (destination < end)
    {
        *destination++ = 0U;
    }

    /*
     * 到这里普通 C 全局变量已经可用，SoC 内存后续初始化可以安全使用
     * 静态状态；但是否能够访问 DDR 仍取决于真实 BSP 的初始化结果。
     */
    soc_memory_init();

    /* 确保内存初始化结果在进入 main() 前对后续数据访问和取指可见。 */
    armv7r_data_sync_barrier();
    armv7r_instruction_sync_barrier();
}
