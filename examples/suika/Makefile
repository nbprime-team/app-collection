# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# app-collection/examples/suika/Makefile —— Suika 水果游戏
#
#   make                编译 -> suika_prime.elf
#   make check          readelf 检查 ELF 形状
#   make deploy         产出可上传包 suika.hpappdir/
#   make font           只生成字体源（unifont_font.c/.h）
#   make fetch-unifont  只获取 unifont hex（已内置）
#   make clean
#
# 公共规则见 toolchain/templates/app.mk；字体来自
# app-collection/resources/prime-unifont（与 cube3d 同源）。

SDK    ?= ../../../toolchain
TARGET := suika_prime
APPDIR := suika.hpappdir

# 本工程的目标架构（ARMv5TEJ）
ARCHFLAGS ?= -mcpu=arm926ej-s -march=armv5tej -marm -mfloat-abi=soft

# prime_input.S 提供 prime_sys_* 的 SVC 包装；输入钩子统一走 SDK 的 prime_hook.c
OBJS   := $(TARGET).o prime_input.o prime_hook.o

include $(SDK)/templates/app.mk
include ../../resources/prime-unifont/unifont.mk

# 字体头由 unifont.mk 生成，先于本工程源编译
$(TARGET).o: $(UNIFONT_H)
