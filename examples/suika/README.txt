Suika Prime —— 水果合并小游戏
================================

输入方式（不轮询 SVC）
----------------------
本程序**不在游戏循环里轮询 SVC #0x1003f**。固件输入分发入口（0x307FBFA0）被换成
指向本程序回调的 trampoline：

  固件 -> 0x307FBFA0 trampoline -> suika_event_hook()
                 -> SVC #0x1003f（取事件）
                 -> 解析 ui_event_prime_s
                 -> volatile 输入状态

钩子实现统一走 SDK 的 `toolchain/sdk/prime_hook.c`（trampoline 直达回调，回调内自行
调用 `prime_sys_get_event`）。钩子目标 0x307FBFA0 与特权 cache 刷新拷贝例程取自
DOOM/puredoom.elf，因此本构建**只适用于与该 DOOM 二进制相同的固件环境**。

操作
----
* 触摸按下/移动：左右移动当前水果。
* 触摸抬起：放下水果。
* 任意键按下或抬起：立即退出，并还原 0x307FBFA0 处的原始 16 字节。

画面为 320x240x32 位离屏渲染，每帧一次性拷贝到 LCD，减少撕裂/闪烁。

构建（Debian / Termux）
----------------------
  make clean
  make

字体与公共件
------------
* 字体：GNU Unifont 17.0.04 的 ASCII 子集，来自 `app-collection/resources/prime-unifont/`
  （hex 已内置；缺失时才联网获取）；
* 输入钩子：`toolchain/sdk/prime_hook.c`；
* 运行支撑（事件读取、LCD 帧缓冲、整屏拷贝、ELF 硬要求）：
  `toolchain/examples/app-common/`。
以上都与 cube3d 同源。

产物：
  suika_prime.elf

把它替换 Python 加载器所用的 ELF（例如 my_app.elf）。
不要在其它固件版本上测试本钩子构建：0x307FBFA0 取自提供的 DOOM 二进制。

安全
----
程序会保存并还原钩子地址处的原始 16 字节；若因按键退出，main() 在返回前调用
`prime_hook_remove()` 恢复固件原始代码。
