# Prime Unifont —— GNU Unifont（ASCII 子集）与文本绘制

> ⚠️ **AI 生成 / 辅助创作：DeepSeek V4.1 Flash**（未人工审查）

app-collection 的**共享字体资源**：GNU Unifont 的 ASCII 子集（U+0020..U+007E）
与其绘制实现，供各应用统一使用（当前使用者：`examples/suika`、`examples/cube3d`）。

## 文件

| 文件 | 内容 | 入库 |
|---|---|---|
| `unifont-17.0.04.hex` | GNU Unifont 17.0.04 的 `.hex` 源（3.6 MB） | ✅ |
| `fetch_unifont.py` | 缺失时从 unifoundry.com 获取 `.hex`（需网络） | ✅ |
| `fontgen.py` | 从 `.hex` 生成 `unifont_font.c/.h`（ASCII 子集，8x16 / 16x16） | ✅ |
| `unifont_draw.h` / `unifont_draw.c` | 文本绘制与度量（`unifont_draw_text` / `unifont_text_width`） | ✅ |
| `unifont.mk` | 公共构建片段（包含路径、生成规则、`unifont_font.o`/`unifont_draw.o`） | ✅ |
| `unifont_font.c` / `unifont_font.h` | **生成物**（由 `fontgen.py` 产出，不入库） | ❌ |
| `UNIFONT_SOURCE.txt` | 上游来源说明 | ✅ |

字体来源与版本见 `UNIFONT_SOURCE.txt`（GNU Unifont 17.0.04，OFL-1.1）。

## 用法

应用 Makefile（务必排在 `toolchain/templates/app.mk` 之后）：

```make
include $(SDK)/templates/app.mk
include ../../resources/prime-unifont/unifont.mk
```

代码：

```c
#include "unifont_draw.h"

/* 帧缓冲、宽高、起点、文本、放大倍数、字符间距、颜色 */
unifont_draw_text(fb, LCD_W, LCD_H, 8, 8, "SCORE", 2, 1, 0xFFFFFFFFu);
int w = unifont_text_width("SCORE", 2, 1);
```

## 约定

- 字形 8x16 或 16x16（`unifont_widths` 给出宽度），**位序 MSB 在左**；
- `scale >= 1` 为放大倍数，`gap` 为字符间距；绘制自带裁剪；
- 生成物统一写在**本目录**（共享一份），各应用编译同一份源到自己的 `*.o`；
- `make font` 只生成字体源，`make fetch-unifont` 只获取 hex。
