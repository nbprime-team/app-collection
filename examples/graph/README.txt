graph —— 函数绘图器（app-collection 工程示例）
==============================================

⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

用途：在 Prime 上画 y = f(x) 的曲线。与 paint.c 互补——paint 演示**触摸**，
本示例演示**键盘**、软浮点数学与坐标变换，切合 Prime "图形计算器"的本职。

操作
----
  LEFT / RIGHT   左右平移视图
  UP / DOWN      缩放（UP 放大，DOWN 缩小）
  x² 键          切换函数：sin(x) → cos(x) → x*x/4 → 1/x
  PLOT 键        复位视图（缩放 24 px/单位，中心回到原点）
  ESC 或 ON      退出

构建 / 部署
-----------
  source ../../../toolchain/scripts/env.sh
  make                 # -> graph.elf
  make deploy          # -> graph.hpappdir/
  把 graph.hpappdir/ 拷入计算器 C:\DATA\ 运行。

实现要点
--------
* 键盘：APP_EV_KEY → APP_KEY_DOWN 时取扫描码 (w8>>16)，用 hp_input.h 的 HP_KEY_*
  判定；`hp_input.h` 的键码表来自 hook_abi.md 的物理矩阵；
* 数学：目标为软浮点且**无 libm**，sin/cos 用泰勒展开自实现（与 cube3d 同一份）；
* 坐标：世界坐标 <-> 屏幕像素的正交映射（MID_X/MID_Y + scale），平移即改视图中心，
  缩放即改 scale；网格与坐标轴由世界整数位置反算屏幕位置；
* 绘图：逐像素列采样，相邻列之间做竖直填充；越界、奇点（1/x）或跳变过大处断开；
* 刷新：只有视图变化（g_dirty）时才重画并上屏，静止时不刷屏；
* 文本：共享字体资源 resources/prime-unifont；运行支撑：app-common。

限制
----
* 只内置 4 个函数，不支持输入表达式（那需要表达式解析器）；
* 1/x 在 |x| < 0.02 附近断开设为无定义，避免巨大值；
* 网格仅在 scale >= 12 px/单位时绘制。

状态
----
构建通过（ELF32/DYN/ARM，soft-float；含 R_ARM_RELATIVE）；**真机未验证**。
