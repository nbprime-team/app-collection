# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（本次修订，未人工审查）
#
# CC 是 make 的内置变量（默认 cc），因此 `CC ?= arm-none-eabi-gcc` 不会生效——
# 构建会静默退回宿主 cc 并报 “unrecognized command-line option '-marm'”。
# 这里显式赋值；仍可用 `make CC=...` 从命令行覆盖。
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

CFLAGS ?= -mcpu=arm926ej-s -march=armv5tej -marm -mfloat-abi=soft \
          -fPIC -ffreestanding -fno-strict-aliasing -fno-builtin \
          -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -O2 -Wall -Wextra
CFLAGS += -I$(SDK)/sdk

LDFLAGS ?= -shared -nostdlib -nodefaultlibs -nostartfiles \
           -Wl,-Bsymbolic -Wl,--no-undefined -Wl,--build-id=none \
           -Wl,-z,norelro -Wl,-z,max-page-size=0x1000 -Wl,--hash-style=sysv
LDFLAGS += -Wl,-T,$(SDK)/sdk/prime_dyn.ld

LDLIBS ?= -lgcc

.PHONY: all font fetch-unifont check deploy clean

# 可上传包目录：结构已在仓库中（main.py + suika.hpapp*），
# deploy 只需把构建产物放进去。
APPDIR := suika.hpappdir

all: suika_prime.elf

font: $(FONT_C) $(FONT_H)

fetch-unifont: $(FONT_HEX)

$(FONT_HEX):
	$(PYTHON) tools/fetch_unifont.py

$(FONT_C) $(FONT_H): $(FONT_HEX) tools/fontgen.py
	$(PYTHON) tools/fontgen.py $(FONT_HEX) $(FONT_C) $(FONT_H)

suika_prime.elf: suika_prime.o prime_input.o $(FONT_O) $(SDK)/sdk/prime_dyn.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ suika_prime.o prime_input.o $(FONT_O) $(LDLIBS)

suika_prime.o: suika_prime.c $(FONT_H)
	$(CC) $(CFLAGS) -c -o $@ $<

prime_input.o: $(SDK)/sdk/prime_input.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(FONT_O): $(FONT_C) $(FONT_H)
	$(CC) $(CFLAGS) -c -o $@ $(FONT_C)

check: suika_prime.elf
	$(READELF) -h -l -d -r $<

# 产出一个可直接拷入计算器的包（C:\DATA\suika.hpappdir）
deploy: suika_prime.elf
	cp -f suika_prime.elf $(APPDIR)/my_app.elf
	@echo "==> $(APPDIR)/ 已就绪（含 main.py + 元数据 + my_app.elf）"

clean:
	rm -f suika_prime.o prime_input.o $(FONT_O) $(FONT_C) $(FONT_H) suika_prime.elf
	rm -f $(APPDIR)/my_app.elf
