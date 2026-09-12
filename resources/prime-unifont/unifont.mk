# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# resources/prime-unifont/unifont.mk —— unifont 资源的公共构建片段。
#
# 用法（在应用的 Makefile 里，务必排在 toolchain/templates/app.mk 之后）：
#
#   include $(SDK)/templates/app.mk
#   include ../../resources/prime-unifont/unifont.mk
#
# 提供：字体包含路径、unifont_font.c/.h 的生成规则、unifont_font.o /
#       unifont_draw.o 的编译规则，并把二者并入 OBJS。

UNIFONT_DIR ?= ../../resources/prime-unifont
UNIFONT_HEX := $(UNIFONT_DIR)/unifont-17.0.04.hex
UNIFONT_C   := $(UNIFONT_DIR)/unifont_font.c
UNIFONT_H   := $(UNIFONT_DIR)/unifont_font.h

UNIFONT_OBJS := unifont_font.o unifont_draw.o
OBJS         += $(UNIFONT_OBJS)
EXTRA_CLEAN  += $(UNIFONT_OBJS)
CFLAGS       += -I$(UNIFONT_DIR)

# 模板里 $(TARGET).elf 的依赖在此之前已展开，故显式追加一次
$(TARGET).elf: $(UNIFONT_OBJS)

.PHONY: font fetch-unifont
font: $(UNIFONT_C) $(UNIFONT_H)
fetch-unifont: $(UNIFONT_HEX)

# 缺失时从 unifoundry.com 获取（需要网络）
$(UNIFONT_HEX):
	$(PYTHON) $(UNIFONT_DIR)/fetch_unifont.py

# fontgen.py 一次同时写出 .c 与 .h。拆成两条规则（.h 依赖 .c），避免 make -j
# 把同一条 recipe 并发执行两次、同时写这两个文件。
$(UNIFONT_C): $(UNIFONT_HEX) $(UNIFONT_DIR)/fontgen.py
	$(PYTHON) $(UNIFONT_DIR)/fontgen.py $(UNIFONT_HEX) $(UNIFONT_C) $(UNIFONT_H)

$(UNIFONT_H): $(UNIFONT_C)

unifont_font.o: $(UNIFONT_C) $(UNIFONT_H)
	$(ARMCC) $(CFLAGS) -c -o $@ $(UNIFONT_C)

unifont_draw.o: $(UNIFONT_DIR)/unifont_draw.c $(UNIFONT_DIR)/unifont_draw.h $(UNIFONT_H)
	$(ARMCC) $(CFLAGS) -c -o $@ $(UNIFONT_DIR)/unifont_draw.c
