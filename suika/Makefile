# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（本次修订，未人工审查）
#
# CC 是 make 的内置变量（默认 cc），因此 `CC ?= arm-none-eabi-gcc` 不会生效——
# 构建会静默退回宿主 cc 并报 “unrecognized command-line option '-marm'”。
# 这里显式赋值；仍可用 `make CC=...` 从命令行覆盖。
CC := arm-none-eabi-gcc
READELF ?= arm-none-eabi-readelf
PYTHON ?= python3

UNIFONT_VERSION := 17.0.04
FONT_DIR := font
FONT_HEX := $(FONT_DIR)/unifont-$(UNIFONT_VERSION).hex
FONT_C := unifont_font.c
FONT_H := unifont_font.h
FONT_O := unifont_font.o

CFLAGS ?= -mcpu=arm926ej-s -march=armv5tej -marm -mfloat-abi=soft \
          -fPIC -ffreestanding -fno-strict-aliasing -fno-builtin \
          -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -O2 -Wall -Wextra

LDFLAGS ?= -shared -nostdlib -nodefaultlibs -nostartfiles \
           -Wl,-Bsymbolic -Wl,--no-undefined -Wl,--build-id=none \
           -Wl,-T,prime_dyn.ld -Wl,-z,norelro \
           -Wl,-z,max-page-size=0x1000 -Wl,--hash-style=sysv

LDLIBS ?= -lgcc

.PHONY: all font fetch-unifont check clean

all: suika_prime.elf

font: $(FONT_C) $(FONT_H)

fetch-unifont: $(FONT_HEX)

$(FONT_HEX):
	$(PYTHON) tools/fetch_unifont.py

$(FONT_C) $(FONT_H): $(FONT_HEX) tools/fontgen.py
	$(PYTHON) tools/fontgen.py $(FONT_HEX) $(FONT_C) $(FONT_H)

suika_prime.elf: suika_prime.o prime_input.o $(FONT_O) prime_dyn.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ suika_prime.o prime_input.o $(FONT_O) $(LDLIBS)

suika_prime.o: suika_prime.c $(FONT_H)
	$(CC) $(CFLAGS) -c -o $@ $<

prime_input.o: prime_input.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(FONT_O): $(FONT_C) $(FONT_H)
	$(CC) $(CFLAGS) -c -o $@ $(FONT_C)

check: suika_prime.elf
	$(READELF) -h -l -d -r $<

clean:
	rm -f suika_prime.o prime_input.o $(FONT_O) $(FONT_C) $(FONT_H) suika_prime.elf
