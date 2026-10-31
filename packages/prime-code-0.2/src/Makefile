# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# app-collection/staging/prime-code/Makefile
#
#   make                编译 -> primecode.elf
#   make check          readelf 检查 ELF 形状
#   make font           只生成字体源（unifont_font.c/.h）
#   make fetch-unifont  只获取 unifont hex（已内置）
#   make clean
#
# 注意：C 版产物 primecode.elf 是独立可执行体，**不**部署到
# primecode.hpappdir（那是 MicroPython 版应用），因此这里不定义 APPDIR，
# 模板也就不会生成 deploy 目标。
#
# 公共变量与规则见 toolchain/templates/app.mk。

SDK    ?= ../../../toolchain
# 产物名与主源文件名（与源码内容一致：PrimeCode 编辑器）
TARGET := primecode

# 本工程的目标架构（ARMv5TE）
ARCHFLAGS ?= -mcpu=arm926ej-s -march=armv5te -marm -mfloat-abi=soft

UNIFONT_VERSION := 17.0.04
FONT_DIR   := font
FONT_HEX   := $(FONT_DIR)/unifont-$(UNIFONT_VERSION).hex
FONT_C     := unifont_font.c
FONT_H     := unifont_font.h
FONT_O     := unifont_font.o

OBJS        := $(TARGET).o prime_input.o $(FONT_O)
EXTRA_CLEAN := $(FONT_O) $(FONT_C) $(FONT_H)

include $(SDK)/templates/app.mk

.PHONY: font fetch-unifont

font: $(FONT_C) $(FONT_H)

fetch-unifont: $(FONT_HEX)

$(FONT_HEX):
	$(PYTHON) tools/fetch_unifont.py

# fontgen.py 一次同时写出 .c 与 .h。拆成两条规则（.h 依赖 .c），避免 make -j
# 把同一条 recipe 并发执行两次、同时写这两个文件。
$(FONT_C): $(FONT_HEX) tools/fontgen.py
	$(PYTHON) tools/fontgen.py $(FONT_HEX) $(FONT_C) $(FONT_H)

$(FONT_H): $(FONT_C)

$(FONT_O): $(FONT_C) $(FONT_H)
	$(ARMCC) $(CFLAGS) -c -o $@ $(FONT_C)

$(TARGET).o: $(FONT_H)
