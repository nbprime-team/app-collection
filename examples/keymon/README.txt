keymon —— 键盘/触摸监视器（app-collection 实用示例）
====================================================

依赖按键输入的实用程序，用于在真机上核对「nbprime 原生按键处理」是否正确。

显示内容
--------
* scan : 物理扫描码（十进制 + 十六进制）
* name : 用 toolchain/sdk/include/hp_input.h 的 HP_KEY_* 反查的键名
         （表未覆盖的键显示 "?"——那是**待补全**的键名，不是错误）
* ty4/w7/w8/w9 : 原始字段，便于与 toolchain/sdk/docs/hook_abi.md 对照
* last : 最近 8 个扫描码；events : 累计事件数
* 触摸时改为显示 TOUCH + x/y/act

操作
----
  任意键查看；ESC / ON 退出。

与 toolchain/examples/keys 的分工
---------------------------------
* keys（toolchain 侧）：只显示**原始字段**，用于验证 ABI 本身；
* keymon（本目录）：在 ABI 已确认的前提下显示**可读键名**，用于日常查键/核对键盘。

构建 / 部署
-----------
  source ../../../toolchain/scripts/env.sh
  make                 # -> keymon.elf
  make deploy          # -> keymon.hpappdir/
  把 keymon.hpappdir/ 拷入计算器 C:\DATA\ 运行。

实现要点
--------
* 输入：nbprime SDK 原生钩子（toolchain/sdk/src/prime_hook.c + platform/prime_input.S）；
* 显示：app-collection 共享字体资源 resources/prime-unifont；
* 运行支撑：toolchain/examples/app-common；
* 不依赖 libc——自带十进制/十六进制格式化（`prime.h` 的 `sprintf` 变参宏目标
  `hp_sprintf8` 在 SDK 中尚未实现，故不用）。

状态
----
构建通过（15572 B，0 未定义符号）；**真机未验证**。
