# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# app-collection/examples/keymon/Makefile —— 键盘/触摸监视器（依赖按键的实用示例）
#
#   make / make check / make deploy / make clean

SDK    ?= ../../../toolchain
TARGET := keymon
APPDIR := $(TARGET).hpappdir

OBJS   := $(TARGET).o prime_input.o prime_hook.o

include $(SDK)/templates/app.mk
include $(SDK)/examples/app-common/app_common.mk
include $(SDK)/sdk/unifont.mk

# 字体头由 unifont.mk 生成，先于本工程源编译
$(TARGET).o: $(UNIFONT_H)
