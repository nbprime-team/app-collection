# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# app-collection/examples/cube3d/Makefile —— 工程性示例
#
#   make            编译 -> cube3d.elf
#   make check      readelf 检查 ELF 形状
#   make deploy     产出可上传包 cube3d.hpappdir/
#   make font       只生成字体源
#   make clean
#
# 本示例**自包含**：不依赖 hp_* 运行时（其实现缺失）；文字与 suika 共用
# app-collection/resources/prime-unifont。公共规则见 toolchain/templates/app.mk。

SDK    ?= ../../../toolchain
TARGET := cube3d
APPDIR := $(TARGET).hpappdir

# prime_input.S（来自 SDK）提供 prime_sys_* 的 SVC 包装，所有应用都需要；
# 输入钩子统一走 SDK 的 prime_hook.c。
OBJS   := $(TARGET).o prime_input.o prime_hook.o

include $(SDK)/templates/app.mk
include ../../resources/prime-unifont/unifont.mk

# 字体头由 unifont.mk 生成，先于本工程源编译
$(TARGET).o: $(UNIFONT_H)
