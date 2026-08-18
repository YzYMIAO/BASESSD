/*
 * ARM 固件系统初始化。
 * 真正访问硬件前，应根据目标芯片手册实现时钟、TCM、Cache 和 DDR 初始化。
 */
void firmware_system_init(void)
{
    /* 当前仅保留接口，避免虚构不存在的主控寄存器地址。 */
}
