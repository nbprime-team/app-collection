# app-collection

> ⚠️ **AI 生成 / 辅助创作：DeepSeek V4.1 Flash**（LCD 已人工审查）

Prime 平台的应用与工具集合。**一个项目/应用一个目录**，编译产物不入库。

## 命名规则

| 类别 | 规则 | 位置 | 例子 |
|---|---|---|---|
| **演示应用** | 无前缀 | [`examples/`](examples/) | `examples/suika/` |
| **单仓库候选** | 必有前缀 `prime-<name>`，先在此**暂存** | [`staging/`](staging/) | `staging/prime-code/`、`staging/prime-file-manager/` |
| **工具** | 归入 | [`tools/`](tools/) | `tools/runelf/runelf.hpappdir/` |
| **资源** | 归入（应用运行时资源，不是构建工具） | [`resources/`](resources/) | `resources/prime-sans/` |

**所有小应用**都必须是「顶层文件夹 + 其下可上传 `.hpappdir/`」；大应用的 `.hpappdir` 可选。

## 内容

| 路径 | 内容 | 可上传包 | 状态 |
|---|---|---|---|
| `examples/suika/` | Suika 水果游戏（输入钩子经 SDK 的 `prime_hook`） | `suika.hpappdir/` | 可构建 |
| `examples/cube3d/` | 3D 线框演示（自包含；移植自 legacy 的 KM3D） | `cube3d.hpappdir/` | 可构建 |
| `staging/prime-code/` | PrimeCode 编辑器：C 实现 `primecode.c` + MicroPython 实现 | `primecode.hpappdir/` | 可构建 |
| `staging/prime-file-manager/` | FileManager 文件管理器（MicroPython GUI） | `FileManager.hpappdir/` | 计算器应用 |
| `tools/runelf/runelf.hpappdir/` | 通用 ELF 运行器（加载器 + 示例 `my_app.elf`） | 本身即 `.hpappdir` | 计算器工具 |
| `resources/prime-sans/` | Prime Sans 字体（固件提取，含 CJK） | — | 资源 |
| `resources/prime-unifont/` | GNU Unifont（ASCII 子集）与文本绘制（suika/cube3d 共用） | — | 资源 |

## 构建 / 打包

```bash
source ../toolchain/scripts/env.sh
make -C examples/suika deploy          # 编译 + 产出可上传包 suika.hpappdir/
make -C examples/cube3d deploy         # 同上（自包含示例，产物 18108 B）
make -C staging/prime-code             # -> primecode.elf (24644 B)
```

三个 C 工程的**公共构建件来自 C 核心库** [`toolchain/sdk/`](../toolchain/sdk/)
（`-I$(SDK)/sdk/include`、`-T$(SDK)/sdk/platform/prime_dyn.ld`、`prime_input.S`），各自不再保留副本；
字体、输入钩子与公共运行支撑都是共享件：`suika` 与 `cube3d` 都用
[`resources/prime-unifont/`](resources/prime-unifont/)（hex 已内置，无需联网）、
[`toolchain/sdk/src/prime_hook.c`](../toolchain/sdk/src/prime_hook.c)（输入钩子）与
[`toolchain/examples/app-common/`](../toolchain/examples/app-common/)（事件读取 / LCD / 
整屏拷贝 / ELF 硬要求）。产物为 ELF32/DYN/ARM（soft-float）。

## 新增一个应用

1. 按命名规则建目录（演示用 `examples/<name>/`；单仓库候选用 `staging/prime-<name>/`）；
2. 放源码与 `Makefile`；
3. 在顶层目录下建 `<name>.hpappdir/`，至少包含：
   - `main.py`：直接复用模范脚本 [`tools/runelf/runelf.hpappdir/main.py`](tools/runelf/runelf.hpappdir/main.py)，**只改 `APP_DIR_FOR_C_CODE` 一处**（改成实际安装路径 `C:\\DATA\\<name>.hpappdir`）；
   - **ELF 必须命名为 `my_app.elf`**（`APP_ELF_FILENAME` 保持默认）——加载器 Python 侧按 `filename` 打开、C 侧按 `app_dir + filename` 打开，改名会两边不一致；
   - `<name>.hpapp`、`.hpappnote`、`.hpappprgm`：应用元数据（可从 runelf 模板复制，
     **内容无关紧要**，应用名由**目录名**承载）；
4. `Makefile`：直接 include 公共片段，不必重复样板（见
   [`toolchain/templates/app.mk`](../toolchain/templates/app.mk)）：

   ```make
   SDK    ?= ../../../toolchain
   TARGET := <name>
   APPDIR := $(TARGET).hpappdir        # 定义后模板才生成 deploy
   OBJS   := $(TARGET).o prime_input.o prime_hook.o
   include $(SDK)/templates/app.mk
   include $(SDK)/examples/app-common/app_common.mk      # 公共运行支撑（必需）
   include ../../resources/prime-unifont/unifont.mk      # 字体（需要时）
   ```

   模板已提供 ARM 目标参数、`-I$(SDK)/sdk/include`、`-T$(SDK)/sdk/platform/prime_dyn.ld`、
   `prime_input.o` / `prime_hook.o` 规则与 `all` / `check` / `deploy` / `clean`。
   `prime_input.S` 是必需项（提供 `prime_sys_get_lcd` / `prime_sys_get_event` /
   `prime_sys_sleep` 的 SVC 包装，否则链接报 undefined reference）。
5. 在 `.gitignore` 里忽略该产物；
6. 更新本文件的内容表。

> 参考实现：[`examples/cube3d/`](examples/cube3d/)（最小工程，字体与钩子与 suika 同源）、
> [`examples/suika/`](examples/suika/)（含字体生成与 `deploy`）。

## 交付物：`.hpappdir/` 必须自包含

`.hpappdir/` 是**可直接拷入计算器的交付物**，因此其中的 ELF（`my_app.elf`）
**必须入库**——`.gitignore` 用 `!` 开例外，`make clean` 也**不删**它。
用户拿到仓库即可直接拷贝运行，**无需先构建**。

## 未完成 / 待维护者确认

- `staging/prime-code/README.txt` 描述的是 Suika 玩法，与 `primecode.c`（编辑器）不符；
  为避免臆断，**原文未改**。
- 各 `.hpappdir` 内的应用标识名（含空格/大写）若改名有破坏导入的风险。
- `tools/runelf/runelf.hpappdir/` 是**通用的 ELF 运行器**，其 `my_app.elf` **就是可运行的
  suika**——与 `examples/suika` 的构建产物对比：段布局、符号数(48)、重定位数(3)、
  `STT_FILE` 全部一致，仅 `.debug_str` 差 7 字节（调试路径）。
- `examples/suika/suika.hpappdir/` 的元数据沿用了 runelf 模板（按约定内容未动），
  大小/校验字段未与实际的 elf 对齐，**需真机验证**。
