Cube3D —— HP Prime 上的 3D 演示（KM3D 移植）
============================================

一个**工程性示例**（app-collection/examples/），用途：

  1. 演示不依赖 hp_* 运行时、直接用固件接口自建渲染；
  2. 演示把 legacy 的 MicroPython 程序移植到 C —— **3D 管线来自
     legacy/prime-mc/MC KMAT.hpappdir/KM3D.py**（CubeGame 的渲染核心），
     在 C 里用软浮点重写（TCC-ARM 无原生浮点）。

操作（仅触屏，同 suika）
------------------------
  触摸拖动   旋转视角
  任意键     退出

实现要点
--------
- **公共运行支撑**来自 `toolchain/examples/app-common`（事件读取、LCD 帧缓冲、
  整屏拷贝、ELF 硬要求），与 suika 同源；
- **3D 管线（逐函数移植 KM3D.py）**：`km_turn`（绕 Y 再绕 X）→ `km_to3d`
  （相机平移 + 旋转）→ `km_to2d`（透视投影，fov=130，中心 (160,120)）→
  `km_back`（近平面 z=1 裁剪）→ `line3d`（带裁剪的 3D 线段）；
- **世界**：KM3D 的 40 单位方块格；本示例为 5x4x5（y=0 为地面层）。
  `show_floor` 画地面网格，`draw_cube` 画方块（12 条边）；
- **离屏 `framebuf` 渲染 + 每帧一次 `app_blit_fb` 整屏拷贝**（直接写 LCD 会屏闪/撕裂）；
- 文字用共享字体资源 `app-collection/resources/prime-unifont`（GNU Unifont 的
  ASCII 子集，与 suika 同源）；三角函数自带；
- 输入经 toolchain/sdk 的固件钩子（见 prime_hook.h，与 suika 同源）；帧率由 `prime_sys_sleep(20)` 控制。

未移植（KM3D 的其余能力）
-------------------------
- 面剔除 `del_line` / `del_y`（需相邻方块查询；本示例画全部 12 条边）；
- 物理 `drop` / `Jump`、放置/破坏 `cube_set` / `cube_break`、菜单 `Mvar`；
- `ps_run`（沿视线前进）与键盘移动。

⚠️ **两项 ELF 硬要求**（HP 加载器需要，缺了会行为异常）——已由公共件满足：
1. 至少保留一个运行时重定位（`R_ARM_RELATIVE`）；
2. `main` 不落在地址 0（`main` 放在 `.text.main`，紧随 `.text.entrypad`）。
二者由 `app_elf_requirements()`（`toolchain/examples/app-common`）在 `main` 开头完成。

构建
----
  source ../../../toolchain/scripts/env.sh
  make clean && make
输出 cube3d.elf（ELF32/DYN/ARM，soft-float）。
链接脚本与输入钩子取自 toolchain SDK（../../../toolchain/sdk/），字体取自
共享资源 ../../resources/prime-unifont/。

部署
----
  make deploy      # 把产物以 my_app.elf 之名放进 cube3d.hpappdir/
然后把 cube3d.hpappdir/ 拷入计算器 C:\DATA\ 运行。

状态
----
结构与编译已验证；真机：**运行与退出均正常**。
> 早先"退出时会重启"在字体与输入钩子统一到共享架构（`resources/prime-unifont`、
> SDK `prime_hook`）后不再出现——两个应用的 ELF 特征一致（`entry=0x14`、
> `.rel.dyn` 3 条）。
