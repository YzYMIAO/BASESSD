# STM32H743 SRAM 占用汇总：按 linker 区域容量统计 `size -A` 的段尺寸。
#
# 只用 POSIX awk 特性（Ubuntu 默认 mawk 没有 gawk 的 strtonum）。
# 输入：arm-none-eabi-size -A <elf> 的输出（section / size / addr）。
# 输出：Region / Capacity / Used / % 表，供构建日志与 .size 文件使用。
#
# 区域容量必须与 src/platform/stm32h743/linker/stm32h743xx_flash*.ld 的 MEMORY 定义一致；
# 改链接脚本后同步这里，否则汇总会静默算错。

function hex2num(h,    i, c, v) {
    sub(/^0[xX]/, "", h)
    v = 0
    for (i = 1; i <= length(h); i++) {
        c = tolower(substr(h, i, 1))
        v = v * 16 + index("0123456789abcdef", c) - 1
    }
    return v
}

BEGIN {
    r[0] = "ITCM";        a[0] = hex2num("0x00000000"); c[0] = 64
    r[1] = "AXI_RAM";     a[1] = hex2num("0x24000000"); c[1] = 384
    r[2] = "AXI_NOCACHE"; a[2] = hex2num("0x24060000"); c[2] = 128
    r[3] = "DTCM";        a[3] = hex2num("0x20000000"); c[3] = 128
    r[4] = "SRAM123";     a[4] = hex2num("0x30000000"); c[4] = 288
    r[5] = "SRAM4";       a[5] = hex2num("0x38000000"); c[5] = 64
    n = 5
}

# 调试/符号类段不占 SRAM，不参与统计。
$1 !~ /^\.(debug|comment|ARM|stab|symtab|strtab)/ && NF == 3 {
    sz = $2 + 0
    addr = $3 + 0
    for (i = 0; i <= n; i++) {
        if (addr >= a[i] && addr < a[i] + c[i] * 1024) {
            u[i] += sz
        }
    }
}

END {
    printf "=== SRAM Usage (stm32h743) ===\n"
    printf "%-16s %8s %10s %7s\n", "Region", "Capacity", "Used", "%"
    for (i = 0; i <= n; i++) {
        printf "%-16s %6d KB %8.1f KB %6.1f%%\n", r[i], c[i], u[i] / 1024, u[i] / c[i] / 1024 * 100
    }
    totc = 0
    totu = 0
    for (i = 0; i <= n; i++) {
        totc += c[i]
        totu += u[i]
    }
    printf "%-16s %6d KB %8.1f KB %6.1f%%\n", "Total", totc, totu / 1024, totu / totc / 1024 * 100
}
