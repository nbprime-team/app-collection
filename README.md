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
| `examples/suika/` | Suika 水果游戏（DOOM 派生的固件输入钩子） | `suika.hpappdir/` | 可构建 |
| `staging/prime-code/` | PrimeCode 编辑器：C 实现 `primecode.c` + MicroPython 实现 | `primecode.hpappdir/` | 可构建 |
| `staging/prime-file-manager/` | FileManager 文件管理器（MicroPython GUI） | `FileManager.hpappdir/` | 计算器应用 |
| `tools/runelf/runelf.hpappdir/` | 通用 ELF 运行器（加载器 + 示例 `my_app.elf`） | 本身即 `.hpappdir` | 计算器工具 |
| `resources/prime-sans/` | Prime Sans 字体（固件提取，含 CJK） | — | 资源 |

## 构建 / 打包

```bash
source ../toolchain/scripts/env.sh
make -C examples/suika deploy          # 编译 + 产出可上传包 suika.hpappdir/
make -C staging/prime-code             # -> primecode.elf (24644 B)
```

两个 C 工程自带 `font/unifont-17.0.04.hex`，**无需联网**。产物为 ELF32/DYN/ARM
（soft-float），由计算器端加载器装入运行。

## 新增一个应用

1. 按命名规则建目录（演示用 `examples/<name>/`；单仓库候选用 `staging/prime-<name>/`）；
2. 放源码与 `Makefile`；
3. 在顶层目录下建 `<name>.hpappdir/`，至少包含：
   - `main.py`：计算器端加载器（ELF 运行器可直接复用 [`tools/runelf/runelf.hpappdir/main.py`](tools/runelf/runelf.hpappdir/main.py)，改 `APP_ELF_FILENAME` / `APP_DIR_FOR_C_CODE` 两个常量）；
   - `<name>.hpapp`、`.hpappnote`、`.hpappprgm`：应用元数据（可从 runelf 模板复制，
     **内容无关紧要**，应用名由**目录名**承载）；
4. `Makefile` 加 `deploy` 目标，把产物拷进 `.hpappdir/`；
5. 在 `.gitignore` 里忽略该产物；
6. 更新本文件的内容表。

> 示例见 [`examples/suika/Makefile`](examples/suika/Makefile) 的 `deploy` 目标。

## 整理记录

- 名称依据**源码实际内容**：`primecode.c` 是编辑器、`suika_prime.c` 是水果游戏
  （两者历史上曾被混放在同一目录）；
- 结构：演示应用入 `examples/`，单仓库候选入 `staging/`（前缀），工具入 `tools/`，资源入 `resources/`；
- `tools/runelf/` 包一层顶层文件夹（与其他应用同构，其下才是 `.hpappdir/`）；
- 抢救自废弃仓库：`FileManager.hpappdir/`、`resources/prime-sans/`（Prime Sans）；
- 删除：48 个 Windows `:Zone.Identifier` 残留与早期编译产物；
- 修复：`suika/Makefile` 的 `CC ?=`（对 make 内置变量无效，会退回宿主 `cc`）。

## 未完成 / 待维护者确认

- `staging/prime-code/README.txt` 描述的是 Suika 玩法，与 `primecode.c`（编辑器）不符；
  为避免臆断，**原文未改**。
- 各 `.hpappdir` 内的应用标识名（含空格/大写）若改名有破坏导入的风险。
- `tools/runelf/runelf.hpappdir/my_app.elf` 的来源与用途尚未记录。
- `examples/suika/suika.hpappdir/` 的元数据沿用了 runelf 模板（按约定内容未动），
  大小/校验字段未与实际的 elf 对齐，**需真机验证**。
