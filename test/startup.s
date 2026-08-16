; ============================================================
; 文件名: startup.s
; 描述: ARM Cortex-M 系列启动代码（完整版 + 超详细注释）
;       包含：向量表、复位处理、堆栈初始化、bss清零、跳转main
; 适用: Cortex-M3/M4/M7 (STM32F4/F7/H7等)
; ============================================================

; ============================================================
; 第一部分：堆栈大小定义
; ============================================================
Stack_Size      EQU     0x00000400          ; 定义堆栈大小 = 1KB
                                            ; EQU = 给常量起名字，类似C语言的 #define

                AREA    STACK, NOINIT, READWRITE, ALIGN=3
                ; AREA = 声明一个段（section），相当于告诉链接器"我要一块内存区域"
                ; STACK = 段的名字，可以随便起
                ; NOINIT = 不要初始化这段内存（上电时里面是垃圾数据也没关系，因为SP会直接指向栈顶）
                ; READWRITE = 这段内存可读可写（栈当然要读写啦）
                ; ALIGN=3 = 8字节对齐（2^3=8），ARM建议栈保持8字节对齐

Stack_Mem       SPACE   Stack_Size          ; SPACE = 保留 Stack_Size 字节的空间，全部填0
                                            ; Stack_Mem 是这块内存的起始地址（栈底，最低地址）

__initial_sp                                ; 这个标签代表栈顶地址（最高地址）
                                            ; 因为栈是向下增长的（从高地址往低地址），所以栈顶 = 起始地址 + 大小
                                            ; 注意：这个符号会被放到向量表的第1项，CPU上电自动读取

; ============================================================
; 第二部分：堆空间定义（用于 malloc 动态分配内存）
; ============================================================
Heap_Size       EQU     0x00000200          ; 堆大小 = 512字节

                AREA    HEAP, NOINIT, READWRITE, ALIGN=3
                ; 堆和栈类似，但堆是向上增长的（从低地址往高地址）
                ; 这个区域给C库的 malloc() 使用

__heap_base
Heap_Mem        SPACE   Heap_Size           ; 堆的起始地址
__heap_limit                                ; 堆的结束地址

; ============================================================
; 第三部分：中断向量表（最关键！！！）
; ============================================================
                PRESERVE8                      ; 告诉汇编器：保持8字节对齐
                THUMB                          ; 告诉汇编器：下面的代码是Thumb-2指令
                                               ; Cortex-M 只支持Thumb-2，不支持ARM（32位）指令

                AREA    RESET, DATA, READONLY ; 向量表放在只读数据段
                                               ; 这个段会被链接器放到Flash的起始地址（0x08000000）
                EXPORT  __Vectors              ; EXPORT = 导出这个符号，让链接器能找到它

; ════════════════════════════════════════════════════════════════
; 向量表的结构是固定的：前16项由ARM内核使用，后面的是厂商定义的外设中断
; 每一项占4字节（一个DCD），总共最多支持240个中断
; ════════════════════════════════════════════════════════════════
__Vectors       DCD     __initial_sp           ; 第1项：栈顶地址
                                               ; CPU上电后第一步：从这里读取SP的值！
                                               ; 相当于自动执行了 MSR MSP, __initial_sp

                DCD     Reset_Handler          ; 第2项：复位向量（最重要！）
                                               ; CPU上电后第二步：从这里读取PC的值，开始执行程序！
                                               ; 相当于自动执行了 B Reset_Handler

                DCD     NMI_Handler            ; 第3项：不可屏蔽中断（Non-Maskable Interrupt）
                DCD     HardFault_Handler      ; 第4项：硬故障（Hard Fault）
                                               ; 发生严重错误（如访问非法内存）时会跳到这里
                DCD     MemManage_Handler      ; 第5项：内存管理故障（MPU访问违规）
                DCD     BusFault_Handler       ; 第6项：总线故障（取指或数据访问失败）
                DCD     UsageFault_Handler     ; 第7项：用法故障（执行了非法指令）
                DCD     0                      ; 第8项：保留（填0，不用）
                DCD     0                      ; 第9项：保留
                DCD     0                      ; 第10项：保留
                DCD     0                      ; 第11项：保留
                DCD     SVC_Handler            ; 第12项：系统调用（SVC指令触发）
                                               ; 类似Linux的syscall，用户程序通过SVC请求内核服务
                DCD     DebugMon_Handler       ; 第13项：调试监视器
                DCD     0                      ; 第14项：保留
                DCD     PendSV_Handler         ; 第15项：可挂起的系统调用
                                               ; RTOS任务切换的核心机制！
                DCD     SysTick_Handler        ; 第16项：系统滴答定时器
                                               ; RTOS的心跳，通常用来触发任务调度

                ; ===== 以下是外设中断向量（芯片厂商定义，此处仅列几个示例） =====
                DCD     WWDG_IRQHandler        ; 窗口看门狗中断
                DCD     PVD_IRQHandler         ; 电源电压检测中断
                DCD     TAMPER_IRQHandler      ; 入侵检测中断
                DCD     RTC_IRQHandler         ; 实时时钟中断
                DCD     FLASH_IRQHandler       ; Flash操作完成中断
                DCD     RCC_IRQHandler         ; 时钟控制中断
                DCD     EXTI0_IRQHandler       ; 外部中断线0（GPIO Pin0触发）
                DCD     EXTI1_IRQHandler       ; 外部中断线1
                ; ... 实际芯片有上百个中断向量

                AREA    |.text|, CODE, READONLY ; 代码段开始
                                               ; |.text| 是标准名称，表示代码段

; ============================================================
; 第四部分：复位处理程序（上电后第一条执行的代码！！！）
; ============================================================
                EXPORT  Reset_Handler          ; 导出复位处理程序，让链接器能找到它

; ════════════════════════════════════════════════════════════════
; Reset_Handler 的工作流程（按顺序执行）：
; 1. 关中断 → 2. 复制.data → 3. 清零.bss → 4. 初始化C库 → 5. 开中断 → 6. 跳转main
; ════════════════════════════════════════════════════════════════
Reset_Handler   PROC

                ; --------------------------------------------
                ; 步骤1：关闭所有中断（防止初始化过程中被打断）
                ; --------------------------------------------
                CPSID   I                      ; CPSID I = Change Processor State, Disable IRQ
                                               ; 相当于 "关总中断"，保证初始化过程的原子性
                                               ; 对应的开中断指令是 CPSIE I

                ; --------------------------------------------
                ; 步骤2：初始化数据段（.data）从Flash复制到SRAM
                ; --------------------------------------------
                ; 什么是 .data 段？
                ;   → 已经初始化的全局变量，例如：int a = 10;
                ;   编译时，a的初始值10存在Flash中（只读），运行时必须拷贝到SRAM中（可读写）
                ;
                ; 链接器会生成以下符号（在链接脚本.ld中定义）：
                ;   __sidata = Flash中.data段的起始地址（加载地址，LMA）
                ;   __sdata  = SRAM中.data段的起始地址（运行地址，VMA）
                ;   __edata  = SRAM中.data段的结束地址
                ;   __sbss   = .bss段的起始地址
                ;   __ebss   = .bss段的结束地址

                LDR     r0, =__sidata          ; r0 = Flash中.data的起始地址（来源）
                                               ; = 伪指令，汇编器会把 label 的地址加载到寄存器
                                               ; 实际生成的指令可能是 LDR r0, [pc, #offset]

                LDR     r1, =__sdata           ; r1 = SRAM中.data的起始地址（目的地）
                LDR     r2, =__edata           ; r2 = SRAM中.data的结束地址（边界）

                CMP     r1, r2                 ; 比较起止地址是否相等
                BEQ     .LC0                   ; BEQ = Branch if Equal (Z标志=1)
                                               ; 如果相等，说明.data段为空（没有初始化的全局变量），跳过复制

; ─── 循环复制 .data 段 ───
.LC1            LDR     r3, [r0], #4           ; 从 r0 指向的 Flash 地址读取 4字节 到 r3
                                               ; [r0], #4 是"后索引寻址"：
                                               ;   → 先把 r0 地址的内容取出来
                                               ;   → 然后 r0 = r0 + 4（指向下一个数据）
                                               ; 这是 ARM 的特色！一条指令同时完成了"取数"和"指针移动"

                STR     r3, [r1], #4           ; 把 r3 的值存储到 r1 指向的 SRAM 地址
                                               ; 同样使用后索引：存完后 r1 = r1 + 4

                CMP     r1, r2                 ; 比较：r1（当前目标地址）是否达到了 r2（结束地址）？
                BCC     .LC1                   ; BCC = Branch if Carry Clear (C标志=0)
                                               ; 在减法中，C=0 表示"没有借位"，即 r1 < r2
                                               ; 等价于"如果还没复制完，继续循环"

                ; --------------------------------------------
                ; 步骤3：清零 .bss 段（未初始化的全局变量）
                ; --------------------------------------------
                ; 什么是 .bss 段？
                ;   → 未初始化的全局变量，例如：int a;
                ;   在C语言中，未初始化的全局变量默认值为0
                ;   所以上电后必须把这段内存全部清零！
.LC0            LDR     r0, =__sbss            ; r0 = .bss段起始地址
                LDR     r1, =__ebss            ; r1 = .bss段结束地址
                MOV     r2, #0                 ; r2 = 0（准备用来清零）

                CMP     r0, r1                 ; 比较起止地址
                BEQ     .LC2                   ; 如果相等，说明.bss为空，跳过清零

; ─── 循环清零 .bss 段 ───
.LC3            STR     r2, [r0], #4           ; 把 r2（0）存储到 r0 指向的地址
                                               ; 然后 r0 = r0 + 4（后索引寻址）
                CMP     r0, r1                 ; 比较：是否已经到达结束地址？
                BCC     .LC3                   ; 如果 r0 < r1，继续循环

                ; --------------------------------------------
                ; 步骤4：初始化C库和全局对对象
                ; --------------------------------------------
.LC2            BL      SystemInit             ; BL = Branch with Link
                                               ; 调用 SystemInit 函数（配置时钟、PLL等）
                                               ; BL 会自动把返回地址存入 LR（R14）寄存器
                                               ; 子程序末尾用 BX LR 返回

                BL      __libc_init_array      ; 初始化C库（如果使用C++，这里会调用全局对象的构造函数）
                                               ; 如果没有C++代码，这个函数可以跳过或为空

                ; --------------------------------------------
                ; 步骤5：开启全局中断，跳转到 main()
                ; --------------------------------------------
                CPSIE   I                      ; CPSIE I = 开总中断
                                               ; 所有初始化完成后，才允许中断发生

                LDR     r0, =main              ; 把 main 函数的地址加载到 r0
                                               ; main 是在 C 文件中实现的

                BX      r0                     ; BX = Branch with eXchange
                                               ; 跳转到 r0 指向的地址执行
                                               ; 注意：BX 可以切换指令集（ARM/Thumb），但这里不需要
                                               ; main() 永远不应该返回，如果返回了，下面的死循环会接住

; ─── 安全网：如果 main() 意外返回，进入死循环 ───
.LC4            B       .LC4                   ; 无条件跳转到自己（死循环，相当于 while(1);）
                ENDP                           ; ENDP = 函数结束

; ============================================================
; 第五部分：弱定义的默认中断处理程序
; ============================================================
; 什么是 [WEAK]？
;   → 弱定义：如果用户在其他地方实现了同名的函数，就使用用户的版本
;   → 如果用户没实现，就使用这里的默认版本（死循环）
;   这样用户只需要实现自己用到的中断处理函数，没用到的不需要写。

                EXPORT  NMI_Handler            [WEAK]
                EXPORT  HardFault_Handler      [WEAK]
                EXPORT  MemManage_Handler      [WEAK]
                EXPORT  BusFault_Handler       [WEAK]
                EXPORT  UsageFault_Handler     [WEAK]
                EXPORT  SVC_Handler            [WEAK]
                EXPORT  DebugMon_Handler       [WEAK]
                EXPORT  PendSV_Handler         [WEAK]
                EXPORT  SysTick_Handler        [WEAK]

                EXPORT  WWDG_IRQHandler        [WEAK]
                EXPORT  PVD_IRQHandler         [WEAK]
                EXPORT  TAMPER_IRQHandler      [WEAK]
                EXPORT  RTC_IRQHandler         [WEAK]
                EXPORT  FLASH_IRQHandler       [WEAK]
                EXPORT  RCC_IRQHandler         [WEAK]
                EXPORT  EXTI0_IRQHandler       [WEAK]
                EXPORT  EXTI1_IRQHandler       [WEAK]

; ─── 默认实现：死循环 ───
; 写法1：用 B . 表示跳转到当前地址（无限循环）
; 写法2：也可以用 B .LC4 跳转到上面的死循环标签
NMI_Handler     PROC
                B       .                      ; "." 代表当前PC地址，即原地跳转
                ENDP

HardFault_Handler PROC
                B       .
                ENDP

MemManage_Handler PROC
                B       .
                ENDP

BusFault_Handler PROC
                B       .
                ENDP

UsageFault_Handler PROC
                B       .
                ENDP

SVC_Handler     PROC
                B       .
                ENDP

DebugMon_Handler PROC
                B       .
                ENDP

PendSV_Handler  PROC
                B       .
                ENDP

SysTick_Handler PROC
                B       .
                ENDP

WWDG_IRQHandler PROC
                B       .
                ENDP

PVD_IRQHandler  PROC
                B       .
                ENDP

TAMPER_IRQHandler PROC
                B       .
                ENDP

RTC_IRQHandler  PROC
                B       .
                ENDP

FLASH_IRQHandler PROC
                B       .
                ENDP

RCC_IRQHandler  PROC
                B       .
                ENDP

EXTI0_IRQHandler PROC
                B       .
                ENDP

EXTI1_IRQHandler PROC
                B       .
                ENDP

; ============================================================
; 第六部分：外部符号声明（告诉链接器这些符号在别的地方定义）
; ============================================================
                IMPORT  main                  ; main() 在 C 文件中实现，需要导入
                IMPORT  __sidata              ; 这些符号由链接器脚本（.ld文件）提供
                IMPORT  __sdata
                IMPORT  __edata
                IMPORT  __sbss
                IMPORT  __ebss

                END                           ; 告诉汇编器：文件结束，不要再读了