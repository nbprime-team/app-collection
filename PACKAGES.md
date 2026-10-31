# 包模型（nb-package-index）

> ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

本仓库以**包**为基本单位；`examples/` 仍保留 `.hpappdir` 作为教学示例。

## 三层

| 层 | 位置 | 谁用 | 形态 |
|---|---|---|---|
| 核心区 | 计算器 `C:\DATA\sys\` | 所有入口共享 | `<bin|lib>/<name>-<ver>/` + `core.db` |
| 包入口 | `C:\DATA\<name>.hpappdir\` | 包管理器 / 命令行 | 只有 `main.py` + 元数据/图标 |
| 自包含包 | `C:\DATA\<name>.hpappdir\` | 直接拷给别人 | 入口 + payload |
| 源 | 本仓库 `packages/<name>-<ver>/` | 开发者 | `pkg.cfg` + payload（`src/` 不发布） |

## pkg.cfg

```ini
name = lua
version = 0.1
kind = lib            # app | tool | lib | pyapp
abi = hpconfig        # shell | hpconfig | plua
entry = lib/lua.elf   # app/tool 的入口（相对包根）
lib = lib/lua.elf     # lib 的载荷
exports = lua,plua    # lib 允许导出的符号前缀
provides = lua        # 命令名（可被 shell 直接调用）
requires = lua-0.1
payload_url = https://…   # 大载荷不入库时指向 release
```

- `kind=pyapp`：payload 是 MicroPython 应用（`main.py`/`*.py` + 元数据），由 Python 启动器运行；包管理器只负责存放/安装。
- `payload_url`：仓库不放的大二进制（tcc/lua）从这里取。

## 生成器（`nb-pkg` 仓库）

```bash
git clone https://github.com/nbprime-team/nb-pkg.git
python3 -m nb_pkg install ../nb-package-index out     # sys + 全部入口
python3 -m nb_pkg bundle  ../nb-package-index hello out/hello.hpappdir
python3 -m nb_pkg verify  ../nb-package-index         # CI 同款校验
```

薄入口的 `main.py` 指向 `C:\DATA\sys\bin\<name>-<ver>\<elf>`；自包含形态的
`main.py` 指向同目录的 ELF。两者都复用 `packages/sh-0.2/main.py` 的加载器，
所以入口行为与终端一致。

## 安装 / 更新 / 回滚 / 卸载

1. 解包到 `sys\.staging\<name>-<ver>\`，逐文件校验；
2. 复制到 `sys\<bin|lib>\<name>-<ver>\`；
3. 写 `core.db`（写临时文件后整体覆盖，失败回滚）；
4. 生成/更新入口壳；
5. 删除 staging（启动时清理残留）。

- **更新**：新旧版本目录并存，改 `core.db` 指针即可；保留上一版用于回滚。
- **回滚**：把 `core.db` 指回旧版本，重启入口。
- **卸载**：删入口 → 引用计数减一 → 为 0 才删核心目录。
- **没有 `rename`**：所有"原子"都靠"临时文件 + 校验 + 覆盖 + 启动清理"模拟。

## 兼容性

- `examples/` 不动，仍然是可以直接拷进计算器的 `.hpappdir`。
- 任何包都能用 `pack.py bundle` 导出为自包含 `.hpappdir`；releases 可只发这个。
- 旧的 `staging/` 已迁入 `packages/`；`prime-code` / `prime-file-manager` 为 `kind=pyapp`，
  `prime-markdown` / `runelf` 为 `kind=app`（载荷在包内 `payload/` 或根目录）。
