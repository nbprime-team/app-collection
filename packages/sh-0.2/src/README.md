# sh —— 极简终端（M2：包 + 跨包符号）

> ⚠️ **AI 生成 / 辅助创作：DeepSeek V4.1 Flash**（未人工审查）

HP Prime 上的 C 终端。它自己装载 ELF，按 `pkg.cfg` 解析依赖，并在装载期把
「包 A 对包 B 的符号引用」回填成真实地址。

![demo 包](m2.png)

## 目录

```
sh/
├── sh.c            终端：渲染、按键、内建命令、控制台 API
├── loader.c        C 装载器：PT_LOAD / R_ARM_RELATIVE / 跨包符号 / cache 维护
├── pkg.c pkg.h     包索引：扫 pkg\，解析 pkg.cfg
├── fs.c fs.h       文件系统封装（读文件、列目录、判断目录）
├── fw_svc.S        固件 SVC 包装
├── sh_api.h        子程序 ABI v1：{ magic, abi, size, puts }
├── hello.c         无依赖的子 ELF（演示 run）
├── libdemo.c       库包：demo_add / demo_mul
├── hello2.c        应用包：导入 libdemo 的符号
├── emu_sh.py       宿主模拟器补丁：目录 SVC（仅供测试）
├── host_m2.py      宿主验证：脚本按键 + 截屏
├── sh.hpappdir/    终端本体（拷到 C:\DATA\）
│   ├── main.py     Python 引导（只装 sh.elf 一次）
│   ├── my_app.elf  sh 本体
│   └── icon.png / sh.hpapp*
└── pkg/            包（拷到 C:\DATA\pkg\，与终端分开）
    ├── hello-0.1/{pkg.cfg, hello.elf}        无依赖
    ├── libdemo-0.1/{pkg.cfg, libdemo.elf}    库
    ├── demo-0.1/{pkg.cfg, hello2.elf}        依赖 libdemo
    ├── echotool-0.1/                        旧 ABI（hp_config）测试
    ├── tcc-0.9/                             机上 C 编译器（1.2 MB）
    ├── lua-0.1/                             解释器**库**（导出 lua_*/plua_*）+ 示例脚本
    └── luahr-0.1/                           C 宿主：导入 lua 库，嵌入解释器
```

## 包格式

目录平铺：`pkg\<name>-<MAJ>.<MIN>\`，每包一个 `pkg.cfg`（key=value）。

```ini
# pkg/libdemo-0.1/pkg.cfg
name = libdemo
version = 0.1
kind = lib
lib = libdemo.elf
exports = demo_
```

```ini
# pkg/demo-0.1/pkg.cfg
name = demo
version = 0.1
kind = app
entry = hello2.elf
provides = demo
requires = libdemo-0.1
```

- `provides`：命令名。输入 `demo` 就等于运行这个包。
- `requires`：依赖包（`name-MAJ.MIN`）；装载器先装依赖，再登记其 `exports` 前缀下的符号。
- `exports`：该库允许被外部引用的符号前缀（空 = 不作为库）。

包目录只有一处：`C:\DATA\pkg\`——终端（`sh.hpappdir/`）与包分开存放。

## 装载器的解析规则

| 重定位 | 处理 |
|---|---|
| `R_ARM_RELATIVE` | `*slot += 映像基址` |
| `R_ARM_JUMP_SLOT` / `R_ARM_GLOB_DAT` | `*slot = 库基址 + st_value` |
| `R_ARM_ABS32` | `*slot = 库基址 + st_value + *slot` |
| `R_ARM_REL32` | `*slot = 库基址 + st_value + *slot - 该槽地址` |

找不到符号 → 返回 `-4`，终端打印 `load fail -4`，不猜、不跳到 0。

## 构建与验证

```bash
source ../../../toolchain/scripts/env.sh
make                 # sh.elf / hello.elf / libdemo.elf / hello2.elf
make deploy          # 拷进 sh.hpappdir/（含 pkg/）
python3 host_m2.py demo.png demo     # 包 + 依赖 + 符号
python3 host_m2.py hello.png hello   # 无依赖包
```

宿主验证输出（见 `m2.png`）：

```text
pkgs: 2
> demo
demo pkg: hello2.elf
demo_add(20,22)=42
demo_mul(6,7)=42
symbols came from libdemo
exit 0
```

这两个 42 是**另一个 ELF（libdemo.elf）算出来的**，主程序里只有 `extern` 声明。

## 上机

1. 把 `sh.hpappdir/` 拷进 `C:\DATA\`；
2. 把 `pkg/` 拷进 `C:\DATA\pkg\`（即 `C:\DATA\pkg\demo-0.1\...`）；
3. `import main`；
4. 输入 `pkg` 看包列表；输入 `demo`（依赖库）或 `hello`（无依赖）运行示例包。

输入：**直接用物理键盘**（映射表来自真机实测）：

- 字母：先按 **ALPHA** 再按对应键（Vars=A、toolbox=B … 2=Z）；ALPHA 一次 = 下一个字母，连按两次 = 锁定；
- 大写与 `= _ " ; : #` 等：先按 **SHIFT**（`=` 在 `.` 键、`_` 在空格键）；
- 数字与 `.,/*-+` 直接输入；**反斜杠 = SHIFT + `/`**，**`~` = SHIFT + `x²`**；
- **路径只有一种格式：反斜杠**。`C:\DATA\...` 或 `~`（= `C:\DATA`）/`~\x`；
  输入 `/` 会直接报 `bad path (use \)`，不做兼容；
- 退格、回车、左右移动光标、上下翻历史、ESC 清行；输入超过 30 字符自动横向滚动。

命令：`help`、`ls [dir]`、`cd [dir]`、`pwd`、`mkdir`、`rmdir`、`rm`、`cp`、`mv`、
`touch`、`cat`、`clear`、`pkg [list|info <name>|scan]`、`run <pkg|elf>`、`runh <elf>`、
`cc <src.c> [-o out]`、`lua [script]`、`echo`、`exit`；
任何 `provides` 里声明的名字都可直接当命令用。

`run` 会**自动识别 ABI**：映像里带 `RT_RET:` 标记（TCC/Lua 产物都链了 `rt_core`）
就用旧 `hp_config`，否则用终端的 `sh_api`。`runh` 是强制旧 ABI 的显式形式，
留作诊断用。给 `hp_entry` 传错 config 会让它把 `sh_api` 当成 `hp_config` 读，
这是之前"程序退出就重启"的重点怀疑对象——现在 `run` 不会再犯。

### Lua：库包 + C 宿主（真实的包依赖）

`lua-0.1` 是**库包**（`kind=lib`，导出前缀 `lua,plua`），`luahr-0.1` 是依赖它的
C 程序（`requires = lua-0.1`）。`luahr.elf`（命令名 `lua`）里只有 `extern` 声明，装载器按名字把
`luaL_newstate` / `lua_pcallk` / `plua_console_init` 等解析到 `lua.elf` 的地址，
再把 Lua 的 `PLUARING` 输出抓回来打印：

```text
> lua
lua: starting lua
lua: lua ok
exit 0
hello from embedded lua
2 + 3 = 5
```

命令名就是 `lua`（包名 `luahr-0.1`，`provides = lua`）；带脚本参数：
`lua C:\DATA\pkg\lua-0.1\hello.lua`。

> 注意：`lua.elf` 的 `.dynsym` 是空的（符号都在 `.symtab`），装载器为此加了
> 一条 `.symtab` 回退；导出白名单写成逗号分隔（`lua,plua`），否则 `plua_*` 解析不到。

`cc` 用 `tcc` 包编译，`runh` 用旧 `hp_config` ABI 运行 TCC 的产物：

```text
> cc C:\DATA\pkg\tcc-0.9\demo.c -o C:\DATA\demo.elf
cc:
C:\DATA\pkg\tcc-0.9\demo.c
exit 2
out:
C:\DATA\demo.elf
compile ok, run with: runh <out.elf>
> runh C:\DATA\demo.elf
```

TCC 的输出（编译错误/警告）走它自己的 PRIMELOG 环，终端在程序返回后**会把环里的
最后 1.5 KB 打印出来**；`runh` 运行的程序同理（printf 也能看到）。成功判据仍是
"输出文件存在"。

`cc` 的两点便利：源文件找不到时回退到 tcc 包目录里的同名文件（所以 `cc demo.c`
可以直接用）；传给 TCC 的路径会把 `/` 归一化成 `\`（固件 fopen 只认反斜杠）。

## 已知限制

- 依赖只解析一层（包的直接 `requires`），不做版本范围、不做冲突检测。
- 没有安装/卸载命令（`pkg install/remove` 属 M5）。
- 库与应用**同时常驻**，退出时统一释放；没有引用计数/卸载。
- `ls` / `pkg` 依赖固件 `find_first`/`get_attr`。宿主模拟器是同签名复刻，
  真机上的这条 SVC 路径仍需实测（M2 的 `pkg`/`ls` 若异常，问题会隔离在这里）。

## 下一步（M3）

`cc` / `lua` 作为可执行包接入：`pkg/tcc-0.9/`、`pkg/lua-0.1/`，并让 `run` 支持
给子程序传 argv（编译源文件、执行脚本）。
