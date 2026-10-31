# 仓库划分

> ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

## 决定

**小型应用保留统一放在本仓库**（`packages/`，清单 + 小载荷）。它们共享同一套
构建/发布流程，单独开仓库只会增加维护面。

**大型或独立应用**（prime-code、prime-markdown、prime-tcc、prime-lua、nb-manager）
源码留在各自仓库；本仓库只保留**清单**与载荷指针（`payload_url`）或发布产物。

## 归属表

| 仓库 | 负责 | 不负责 |
|---|---|---|
| `nb-package-index`（本仓库） | 所有包的清单与载荷；`examples/`；索引文档 | 工具代码、SDK、应用源码（大型） |
| `nb-pkg` | PC 端校验/生成/取载荷工具、打包规范实现 | 任何应用或载荷 |
| `toolchain` | 交叉编译器包装、SDK 头文件/运行时/链接脚本、构建模板、API 示例 | 应用、包清单 |
| `prime-tcc` / `prime-lua` / `nb-manager` | 各自源码 + `package/<name>-<ver>/` 清单 | 其他应用的包 |
| 大型应用仓库（prime-code、prime-markdown） | 应用源码与发布 | 索引里的其他包 |
| `examples/` | 教学用 `.hpappdir`（兼容形态） | 参与版本管理 |

## 仍在迁移中

1. ~~**字体/unifont**~~：已并入 `toolchain/sdk`（`sdk/src/unifont/`、`sdk/include/unifont_draw.h`、
   `sdk/unifont.mk`）；Prime Sans 仍待作为 `kind=asset` 包或归档。
2. **`packages/*/src/`**：`prime-code`、`prime-markdown` 的源码应回各自仓库；
   索引只留 `pkg.cfg` + 载荷。`sh-0.2/src/` 暂时保留（体量小、无独立仓库）。
3. **`tools/make-icons.py`**：属于打包工具，应移入 `nb-pkg`。
4. 以上完成后，本仓库 ≈ `packages/` + `examples/` + 文档。

## 版本规则

- 版本目录发布后**只增不改**；改动发新版本。
- 大版本变化必须改 `abi`，装载器据此拒绝不兼容载荷。
- 载荷可选的 `hashes.txt` 由 `nb-pkg verify` 强制校验。
