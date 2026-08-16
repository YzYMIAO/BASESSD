# 软件架构

主数据路径：Host Protocol -> Controller -> FTL -> Flash Controller -> NAND。

核心模块只能依赖抽象接口，不能直接访问操作系统 API 或芯片寄存器。
