/*
 * PC 端 SSD 模拟器入口。
 * 该目标使用主机编译器构建，不应与 Cortex-M 启动文件同时链接。
 */
int main(void)
{
    /* 后续依次初始化平台、模拟 NAND、FTL 和控制器。 */
    return 0;
}
