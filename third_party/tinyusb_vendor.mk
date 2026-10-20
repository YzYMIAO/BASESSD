# TinyUSB (MIT) — 只编入 USB device + MSC(BOT) 所需的最小源集合。
#
# 端口 = Synopsys DWC2（STM32H743 的 USB2_OTG_FS）。未编入 host 栈、其它
# class driver 与 on-chip PHY 相关端口，避免把无关代码计入 flash。
#
# 配置宏全部来自 include/tusb_config.h（TinyUSB 以裸名 "tusb_config.h" 包含）。

TINYUSB_DIR  := $(PROJECT_ROOT)/third_party/tinyusb
TINYUSB_SRC  := $(TINYUSB_DIR)/src

TINYUSB_INCS := \
    $(TINYUSB_SRC)

TINYUSB_SRCS := \
    $(TINYUSB_SRC)/tusb.c \
    $(TINYUSB_SRC)/common/tusb_fifo.c \
    $(TINYUSB_SRC)/device/usbd.c \
    $(TINYUSB_SRC)/class/msc/msc_device.c \
    $(TINYUSB_SRC)/portable/synopsys/dwc2/dcd_dwc2.c \
    $(TINYUSB_SRC)/portable/synopsys/dwc2/dwc2_common.c
