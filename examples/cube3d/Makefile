# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# app-collection/examples/cube3d/Makefile —— 工程性示例
#
#   make            编译 -> cube3d.elf
#   make check      readelf 检查 ELF 形状
#   make deploy     产出可上传包 cube3d.hpappdir/
#   make clean
#
# 本示例**自包含**：不依赖 hp_* 运行时（其实现缺失），也不需要 unifont 字体，
# 因此只有单个源文件；链接脚本来自 prime-tcc SDK。
#
# 注意 ARMCC 不能用 CC（CC 是 make 内置变量，`CC ?=` 不会生效）。

ARMCC    ?= arm-none-eabi-gcc
READELF  ?= arm-none-eabi-readelf
SDK      ?= ../../../prime-tcc

TARGET   := cube3d
APPDIR   := $(TARGET).hpappdir
# prime_input.S（来自 SDK）提供 prime_sys_* 的 SVC 包装，所有应用都需要
OBJS     := $(TARGET).o prime_input.o

CFLAGS   := -mcpu=arm926ej-s -marm -mfloat-abi=soft \
            -fPIC -ffreestanding -fno-strict-aliasing -fno-builtin \
            -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -O2 -Wall -Wextra

LDFLAGS  := -shared -nostdlib -nodefaultlibs -nostartfiles \
            -Wl,-Bsymbolic -Wl,--no-undefined -Wl,--build-id=none \
            -Wl,-z,norelro -Wl,-z,max-page-size=0x1000 -Wl,--hash-style=sysv \
            -Wl,-T,$(SDK)/sdk/prime_dyn.ld

LDLIBS   := -lgcc

.PHONY: all check deploy clean

all: $(TARGET).elf

$(TARGET).elf: $(OBJS) $(SDK)/sdk/prime_dyn.ld
	$(ARMCC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(TARGET).o: $(TARGET).c
	$(ARMCC) $(CFLAGS) -c -o $@ $<

prime_input.o: $(SDK)/sdk/prime_input.S
	$(ARMCC) $(CFLAGS) -c -o $@ $<

check: $(TARGET).elf
	$(READELF) -h -l -d -r $<

# 产出一个可直接拷入计算器的包（C:\DATA\cube3d.hpappdir）
deploy: $(TARGET).elf
	cp -f $(TARGET).elf $(APPDIR)/$(TARGET).elf
	@echo "==> $(APPDIR)/ 已就绪（含 main.py + 元数据 + $(TARGET).elf）"

clean:
	rm -f $(TARGET).o prime_input.o $(TARGET).elf
	rm -f $(APPDIR)/$(TARGET).elf
