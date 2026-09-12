Cube3D —— HP Prime 上的 3D 线框演示
====================================

一个**工程性示例**（app-collection/examples/），用途有两个：

  1. 演示不依赖 hp_* 运行时、直接用固件接口自建渲染；
  2. 演示把 legacy 的 MicroPython 程序移植到 C ——
     旋转与投影公式取自 legacy/prime-mc/MC KMAT.hpappdir/KM3D.py，
     在 C 里用软浮点重写（TCC-ARM 无原生浮点）。

操作（仅触屏，同 suika）
------------------------
  触摸拖动   旋转立方体
  任意键     退出

实现要点
--------
- 320x240 ARGB 帧缓冲：`prime_sys_get_lcd()` 取 LCD 对象，vtable + 0x10 处是缓冲；
- **离屏 `framebuf` 渲染 + 每帧一次 `blit_fb` 整屏拷贝**（直接写 LCD 会屏闪/撕裂）；
- 每帧清屏 -> 8 顶点旋转（KM3D 的 turn）-> 透视投影（fov=130）-> Bresenham 画 12 条边；
- 自带 8x8 字模（数据为 **LSB 在左**，故位序用 `(bits >> col) & 1`）与三角函数；
- 输入经 toolchain/sdk 的固件钩子（见 prime_hook.h）；帧率由 `prime_sys_sleep(20)` 控制。

⚠️ **两项 ELF 硬要求**（suika 的注释里写明的，缺了加载器会行为异常）：
1. **至少保留一个运行时重定位**——用 `static uint32_t *volatile relocation_anchor = (uint32_t *)&relocation_anchor;`。
   否则 `R_ARM_RELATIVE` 计数为 0，**退出时会重启**；
2. **`main` 不要落在地址 0**——加 `entry_pad()`（`.text.entrypad`）并把 `main` 放进
   `.text.main`；HP 加载器把 `return 0` 当作失败。

构建
----
  source ../../../toolchain/scripts/env.sh
  make clean && make
输出 cube3d.elf（ELF32/DYN/ARM，soft-float）。
链接脚本与输入钩子取自 toolchain SDK（../../../toolchain/sdk/）。

部署
----
  make deploy      # 把产物以 my_app.elf 之名放进 cube3d.hpappdir/
然后把 cube3d.hpappdir/ 拷入计算器 C:\DATA\ 运行。

状态
----
结构与编译已验证；**真机行为未验证**。
