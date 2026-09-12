PrimeCode for HP Prime
======================

本目录是 PrimeCode 编辑器（HP Prime 的机上代码编辑器）的实现，含两种：

  primecode.c           C 实现 —— 用组织内工具链交叉编译为 ELF，由加载器装入运行
  primecode.hpappdir/   MicroPython 实现 —— 可直接拷入计算器运行

功能（据 primecode.c）
-----------------------
- 语法高亮，HPPL / Python 模式可切换（hppl_mode）
- 搜索与替换
- 自动补全（候选列表）
- 行号栏、gutter、滚动条、标题栏
- 键盘与触摸输入（含 SHIFT / ALPHA / SYM 等修饰键）
- 编辑缓冲区上限：512 行 × 512 字符

构建（C 实现）
--------------
  source ../../toolchain/scripts/env.sh
  make clean && make

输出 primecode.elf（ELF32 / DYN / ARM / soft-float）。
构建需要 font/unifont-17.0.04.hex（已内置，无需联网）。

用法
----
C 实现：把 primecode.elf 交给 ELF 加载器。加载器可参考
  app-collection/tools/runelf/runelf.hpappdir/main.py
（改其中 APP_ELF_FILENAME 与 APP_DIR_FOR_C_CODE 两个常量即可）。

MicroPython 实现：把 primecode.hpappdir/ 整个拷入计算器 C:\DATA\ 后运行。

说明
----
本文件此前复制了另一个程序（Suika 水果游戏）的说明，与 primecode.c 的实际
内容不符；现已按源码更正。Suika 项目见 app-collection/examples/suika/。
