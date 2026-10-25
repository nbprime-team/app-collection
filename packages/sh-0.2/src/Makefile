# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# sh 0.2 源码构建（位于 packages/sh-0.2/src）
#   make        -> sh.elf
#   make install-> 拷到 ../bin/sh.elf
#
# 依赖：../../../../toolchain（SDK）、../../../resources/prime-unifont

SDK    ?= ../../../../toolchain
UNIFONT_DIR := ../../../resources/prime-unifont
TARGET := sh
OBJS   := sh.o loader.o fs.o pkg.o fw_svc.o prime_input.o prime_hook.o

include $(SDK)/templates/app.mk
include $(SDK)/examples/app-common/app_common.mk
include ../../../resources/prime-unifont/unifont.mk

$(TARGET).o: $(UNIFONT_H)

fw_svc.o: fw_svc.S
	$(ARMCC) $(CFLAGS) -c -o $@ $<

install: $(TARGET).elf
	cp -f $(TARGET).elf ../bin/sh.elf

clean:
	rm -f $(OBJS) $(TARGET).elf
