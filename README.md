# app-collection

> ⚠️ **AI 生成 / 辅助创作：DeepSeek V4.1 Flash**（未人工审查）

Prime 平台的应用与工具集合。**一个项目/应用一个目录**，编译产物不入库。

## 命名规则

| 类别 | 规则 | 例子 | 位置 |
|---|---|---|---|
| **小应用** | **无前缀** | `suika/`、`FileManager.hpappdir/` | 仓库根 |
| **单仓库大应用** | **必有前缀**（`prime-<name>`） | `prime-code/` | [`examples/`](examples/) |
| 工具 / 资源 | 归入 `tools/` | `tools/runelf.hpappdir/`、`tools/fonts/` | `tools/` |

> `prime-code/` 日后将独立成仓库，故带前缀并置于 `examples/`；名称**不变**。

## 内容

| 路径 | 内容 | 状态 |
|---|---|---|
| `suika/` | Suika 水果游戏（DOOM 派生的固件输入钩子，`suika_prime.c`） | 可构建 |
| `examples/prime-code/` | PrimeCode 编辑器：C 实现 `primecode.c` + MicroPython 实现 `primecode.hpappdir/` | 可构建 |
| `FileManager.hpappdir/` | FileManager 文件管理器（MicroPython GUI） | 计算器应用 |
| `tools/runelf.hpappdir/` | 通用 ELF 运行器（Python 加载器 + 示例 `my_app.elf`） | 计算器工具 |
| `tools/fonts/` | Prime Sans 字体（固件提取，`PrimeSans{,-Bold,-Mono}.ttf`） | 资源 |

## 构建

```bash
source ../toolchain/scripts/env.sh
make -C suika                    # -> suika_prime.elf (17584 B)
make -C examples/prime-code      # -> primecode.elf    (24644 B)
```

两个工程自带 `font/unifont-17.0.04.hex`，**无需联网**。产物为 ELF32/DYN/ARM
（soft-float），由计算器端加载器装入运行。

## 整理记录

- 名称依据**源码实际内容**：`primecode.c` 是编辑器、`suika_prime.c` 是水果游戏
  （两者历史上曾同处一名为 `suika_prime/` 的目录，现已分清）；
- 结构调整：`suika-prime/` → `suika/`（小应用去前缀）；`prime-code/` → `examples/`；
  `runelf.hpappdir/` → `tools/`；`tools/fonts/` 收录固件提取的 Prime Sans 字体；
- 删除：48 个 Windows `:Zone.Identifier` 残留、早期编译产物、空 `examples/` 目录；
- 规范化：`PrimeCode 2.hpappdir/PrimeCode 2.hpappdir/` → `primecode.hpappdir/`（去嵌套、去空格）；
- 修复：`suika/Makefile` 的 `CC ?= arm-none-eabi-gcc` 对 make 内置变量无效
  （会静默退回宿主 `cc`，报 `unrecognized command-line option '-marm'`）→ 改为 `CC :=`。

## 未完成 / 待维护者确认

- `examples/prime-code/README.txt` 描述的是 Suika 玩法，与 `primecode.c`（编辑器）不符；
  为避免臆断，**原文未改**。
- `primecode.hpappdir/`、`FileManager.hpappdir/`、`runelf.hpappdir/` 内的应用标识名
  （含空格/大写）若改名有破坏计算器导入的风险。
- `tools/runelf.hpappdir/my_app.elf` 的来源与用途尚未记录。
