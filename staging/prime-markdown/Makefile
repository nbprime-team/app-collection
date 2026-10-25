# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
#
# app-collection/staging/prime-markdown/Makefile
# =============================================
# 产物：build/prime-markdown.elf（HP Prime **原生**，形态与 puredoom.elf 一致）
#   - EM_ARM, ET_DYN(-shared), PIC；首 LOAD 段 vaddr 从 0
#   - .rel.dyn 全 R_ARM_RELATIVE、无未定义符号（--no-undefined）
#   - 入口 app_entry（加载器以 R0=cfg 调用，与 runelf 的 dbg.call(entry,cfg,0) 兼容）
#
# 来源：nMarkdown 0.2.0（原版）的 src/ include/ third_party/（Markdown + TeX 渲染核心）
# 参考：hp-prime-nmarkdown 的 Makefile.prime（同形态的原生构建）与 port/
#
# 用法：
#   source ../../../toolchain/scripts/env.sh
#   make            构建
#   make check      ELF 形态摘要
#   make clean

SDK      ?= ../../../toolchain
GXX      ?= arm-none-eabi-g++
GCC      ?= arm-none-eabi-gcc
READELF  ?= arm-none-eabi-readelf

# 仓库内工具链的 newlib / libstdc++ 头与库路径（与 hp-prime-nmarkdown 的
# Makefile.prime 相同的做法：解包工具链不注入这些路径，需显式给出）。
TOOLCHAIN_ROOT ?= $(SDK)/armtc/root
GCC_VER := $(shell $(GXX) -dumpversion)
TC_INC_CXX    := $(TOOLCHAIN_ROOT)/usr/include/newlib/c++/$(GCC_VER)
TC_INC_VAR    := $(TC_INC_CXX)/arm-none-eabi/arm/v5te/softfp
TC_INC_GCC    := $(TOOLCHAIN_ROOT)/usr/lib/gcc/arm-none-eabi/$(GCC_VER)/include
TC_INC_SYS    := $(TOOLCHAIN_ROOT)/usr/include
TC_INC_NEWLIB := $(TOOLCHAIN_ROOT)/usr/include/newlib
TC_BIN_GCC    := $(TOOLCHAIN_ROOT)/usr/lib/gcc/arm-none-eabi/$(GCC_VER)
TC_LIB_NEWLIB := $(TOOLCHAIN_ROOT)/usr/lib/arm-none-eabi/newlib
EXTRA_CPPFLAGS := -I"$(TC_INC_VAR)" -I"$(TC_INC_CXX)/arm-none-eabi" \
	-I"$(TC_INC_CXX)" -I"$(TC_INC_SYS)" -I"$(TC_INC_NEWLIB)" -I"$(TC_INC_GCC)"
EXTRA_LDFLAGS := -B"$(TC_BIN_GCC)" -L"$(TC_LIB_NEWLIB)"
export CPLUS_INCLUDE_PATH := $(TC_INC_VAR):$(TC_INC_CXX)/arm-none-eabi:$(TC_INC_CXX)/backward:$(TC_INC_CXX):$(TC_INC_SYS):$(TC_INC_NEWLIB):$(TC_INC_GCC)
export C_INCLUDE_PATH := $(TC_INC_SYS):$(TC_INC_NEWLIB):$(TC_INC_GCC)

# 目标：G1（ARM926EJ-S / ARMv5TE，软浮点）
ARCH ?= arm926ej-s
ISA  ?= -marm -mfloat-abi=soft

BUILD  := build
TARGET := $(BUILD)/prime-markdown.elf

# ---------- 编译单元 ----------
PORT_SOURCES := \
	port/prime_log.cpp \
	port/prime_runtime.cpp \
	port/prime_svc.cpp \
	port/prime_platform.cpp \
	port/prime_render_api.cpp \
	port/prime_layout.cpp \
	port/prime_demo.cpp

NMD_SOURCES := $(wildcard src/app/*.cpp) $(wildcard src/document/*.cpp) \
	$(wildcard src/layout/*.cpp) $(wildcard src/math/*.cpp) \
	$(wildcard src/render/*.cpp) $(wildcard src/text/*.cpp) \
	$(wildcard src/generated/*.cpp)

# FreeType：与上游一致的裁剪单元集
FREETYPE_SOURCES := \
	third_party/freetype/src/base/ftsystem.c \
	third_party/freetype/src/base/ftdebug.c \
	third_party/freetype/src/base/ftinit.c \
	third_party/freetype/src/base/ftbase.c \
	third_party/freetype/src/base/ftbitmap.c \
	third_party/freetype/src/base/ftmm.c \
	third_party/freetype/src/sfnt/sfnt.c \
	third_party/freetype/src/truetype/truetype.c \
	third_party/freetype/src/cff/cff.c \
	third_party/freetype/src/psaux/psaux.c \
	third_party/freetype/src/pshinter/pshinter.c \
	third_party/freetype/src/psnames/psnames.c \
	third_party/freetype/src/smooth/smooth.c \
	third_party/freetype/src/autofit/autofit.c

HARFBUZZ_SOURCES := third_party/harfbuzz/src/harfbuzz.cc

# Markdown 解析器（md4c，MIT）
MD4C_SOURCES := third_party/md4c/md4c.c

# nbprime SDK 资源：原生输入钩子 + SVC 包装（原生平台，不是 Upsilon）
SDK_C_SOURCES   := $(SDK)/sdk/src/prime_hook.c
SDK_ASM_SOURCES := $(SDK)/sdk/platform/prime_input.S

CPP_SOURCES := $(PORT_SOURCES) $(NMD_SOURCES)
CC_SOURCES  := $(HARFBUZZ_SOURCES)
C_SOURCES   := $(FREETYPE_SOURCES) $(MD4C_SOURCES)

OBJECTS := $(addprefix $(BUILD)/,$(CPP_SOURCES:.cpp=.o) $(CC_SOURCES:.cc=.o)) \
           $(addprefix $(BUILD)/,$(C_SOURCES:.c=.o)) \
           $(BUILD)/sdk/prime_hook.o $(BUILD)/sdk/prime_input.o

# ---------- 选项 ----------
INCLUDES := -Iinclude -Iport -I$(SDK)/sdk/include \
	-Ithird_party/freetype/nmarkdown -Ithird_party/freetype/include \
	-Ithird_party/harfbuzz/src -Ithird_party/md4c

CPPFLAGS := $(INCLUDES) -DHB_TINY=1 -DNMARKDOWN_HARFBUZZ_MATH=1 -DNMARKDOWN_PRIME=1 \
	$(EXTRA_CPPFLAGS)

CFLAGS   := -std=c99 -Os -Wall -Wextra -DFT2_BUILD_LIBRARY -fPIC \
	-ffunction-sections -fdata-sections -MMD -MP $(ISA) -mcpu=$(ARCH)
CXXFLAGS := -std=c++17 -Os -Wall -Wextra -fPIC -fexceptions -fno-rtti \
	-fno-builtin -ffunction-sections -fdata-sections -MMD -MP $(ISA) -mcpu=$(ARCH)

LDFLAGS := -shared -Wl,-e,app_entry -Wl,--no-undefined -Wl,--gc-sections \
	-Wl,--no-dynamic-linker -Wl,--build-id=none \
	-Wl,-Bsymbolic -Wl,--exclude-libs,ALL \
	-Wl,-Map=$(BUILD)/prime-markdown.map $(EXTRA_LDFLAGS)
LIBS := -lstdc++ -lsupc++ -lgcc

.PHONY: all check clean

all: $(TARGET)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(GXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(GXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(GCC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# nbprime SDK（toolchain/sdk）：钩子（C）与 SVC 包装（汇编）
$(BUILD)/sdk/prime_hook.o: $(SDK_C_SOURCES)
	@mkdir -p $(dir $@)
	$(GCC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/sdk/prime_input.o: $(SDK_ASM_SOURCES)
	@mkdir -p $(dir $@)
	$(GCC) $(ISA) -mcpu=$(ARCH) -c $< -o $@

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(GXX) -nostdlib $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@
	@echo "==> $@"

check: $(TARGET)
	$(READELF) -h $(TARGET) | grep -E "Class|Machine|Type|Entry"
	$(READELF) -l $(TARGET) | grep LOAD
	@echo "R_ARM_RELATIVE: $$($(READELF) -r $(TARGET) | grep -c R_ARM_RELATIVE)"

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)
