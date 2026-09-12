# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（本次修订，未人工审查）
CC := arm-none-eabi-gcc
READELF ?= arm-none-eabi-readelf
PYTHON ?= python3

# 构建件（prime.h / prime_dyn.ld / prime_input.S）统一来自 prime-tcc SDK，
# 本目录不再保留副本（详见 $(SDK)/sdk/README.md）
SDK ?= ../../../toolchain

UNIFONT_VERSION := 17.0.04
FONT_DIR := font
FONT_HEX := $(FONT_DIR)/unifont-$(UNIFONT_VERSION).hex
FONT_C := unifont_font.c
FONT_H := unifont_font.h
FONT_O := unifont_font.o

# 产物名与主源文件名。原名 suika_prime 与实际内容（PrimeCode 编辑器）不符，
# 已按源码更名为 primecode（详见 ../README.md）。
TARGET := primecode

CFLAGS ?= -mcpu=arm926ej-s -march=armv5te -marm -mfloat-abi=soft \
          -fPIC -ffreestanding -fno-strict-aliasing -fno-builtin \
          -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -O2 -Wall -Wextra
CFLAGS += -I$(SDK)/sdk

LDFLAGS ?= -shared -nostdlib -nodefaultlibs -nostartfiles \
           -Wl,-Bsymbolic -Wl,--no-undefined -Wl,--build-id=none \
           -Wl,-z,norelro -Wl,-z,max-page-size=0x1000 -Wl,--hash-style=sysv
LDFLAGS += -Wl,-T,$(SDK)/sdk/prime_dyn.ld

LDLIBS ?= -lgcc

.PHONY: all font fetch-unifont check clean

all: $(TARGET).elf

font: $(FONT_C) $(FONT_H)

fetch-unifont: $(FONT_HEX)

$(FONT_HEX):
	$(PYTHON) tools/fetch_unifont.py

# fontgen.py 一次同时写出 .c 与 .h。拆成两条规则（.h 依赖 .c），避免 make -j
# 把同一条 recipe 并发执行两次、同时写这两个文件。
$(FONT_C): $(FONT_HEX) tools/fontgen.py
	$(PYTHON) tools/fontgen.py $(FONT_HEX) $(FONT_C) $(FONT_H)

$(FONT_H): $(FONT_C)

$(TARGET).elf: $(TARGET).o prime_input.o $(FONT_O) $(SDK)/sdk/prime_dyn.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TARGET).o prime_input.o $(FONT_O) $(LDLIBS)

$(TARGET).o: $(TARGET).c $(FONT_H)
	$(CC) $(CFLAGS) -c -o $@ $<

prime_input.o: $(SDK)/sdk/prime_input.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(FONT_O): $(FONT_C) $(FONT_H)
	$(CC) $(CFLAGS) -c -o $@ $(FONT_C)

check: $(TARGET).elf
	$(READELF) -h -l -d -r $<

clean:
	rm -f $(TARGET).o prime_input.o $(FONT_O) $(FONT_C) $(FONT_H) $(TARGET).elf
