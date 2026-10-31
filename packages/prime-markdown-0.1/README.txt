Prime Markdown —— HP Prime 原生 Markdown 阅读器
================================================

来源与移植
----------
* 渲染核心：nMarkdown 0.2.0（原版）—— md4c 解析 + FreeType/HarfBuzz 排版 +
  自研数学（LaTeX 子集）引擎，全部在 src/ 与 third_party/。
* HP Prime 适配层 port/：以 hp-prime-nmarkdown 的 port 为基线，**改为 nbprime 原生**：
  - **输入**：nbprime 原生钩子 —— toolchain/sdk/src/prime_hook.c +
    platform/prime_input.S；ABI 见 toolchain/sdk/docs/hook_abi.md
    （words[7]=类型、words[8]>>16=键码/X、words[9]=Y）；
  - **文档**：内嵌示例（port/prime_doc.h；app-collection 暂无文件 API）；
  - **显示/时钟**：固件 SVC（GetActiveLCD 0x1008d / GetSysTime 0x100a5）。

  未使用 Upsilon 的任何机制（OSCreateThread + 阻塞 GetEvent 的事件线程）——
  那是另一平台，原生固件下不可用。

构建
----
  source ../../../toolchain/scripts/env.sh
  make            # -> build/prime-markdown.elf
  make check      # ELF 形态摘要
  make clean

产物形态与 puredoom.elf 一致：ELF32 / ET_DYN / ARM、首 LOAD 段 vaddr=0、
.rel.dyn 全为 R_ARM_RELATIVE、无未定义符号、入口为 app_entry。

部署
----
  prime-markdown.hpappdir/  —— 直接拷入计算器 C:\DATA\ 运行
其中 main.py 复用 app-collection/tools/runelf/ 的权威加载器（仅改 APP_DIR_FOR_C_CODE）。

操作
----
  触屏上/下滑、左/右滑、点按  —— 翻页
  键盘方向键 / ENTER           —— 翻页
  ESC / ON                     —— 退出

状态
----
构建通过（3.4 MB，0 未定义符号）；**真机未验证**。

已知坑：原生加载下的 C++ 全局对象（重要）
-----------------------------------------
引导器（app-collection/tools/runelf 的 main.py + shellcode）是**直接跳 app_entry**
调用入口，**不会运行 .init_array**（ELF 里也没有 __init_array_start/__end 边界符号）。
后果：**非平凡的全局对象其构造函数不会执行**，一旦访问就是非法内存访问 ——
真机现象正是「**无日志、立刻重启**」（崩在 run_reader 里，dbg.call 不返回，
main.py 的日志回显因此看不到）。

本工程已修复：`port/prime_layout.cpp` 原先的三个全局对象
（`MarkdownDocument g_doc;` / `VirtualDocumentLayout g_layout;` / `std::string g_error;`）
全部改为**函数内 static**（`g_doc_inst()` 等），首次调用时才构造（C++ 保证），
不再依赖 `.init_array`。修复后 `_GLOBAL__sub_I_prime_layout.cpp` 从符号表消失。

**约定**：新增 port/ 代码时**不要定义非平凡全局对象**；需要时用「函数内 static」
或显式初始化。检查方法：

    arm-none-eabi-nm build/prime-markdown.elf | grep _GLOBAL__sub_I
    # 只应剩 libstdc++ 内部的两个（__freeres / __throw_system_error）

定位用开关
----------
    make CPPFLAGS_EXTRA... 或直接编辑 Makefile：
    加 -DPRIME_INPUT_OFF   -> 只跑「初始化 + 渲染」，不装输入钩子（二分重启来源）
    去掉该项                -> 完整版（渲染 + 原生钩子输入）
