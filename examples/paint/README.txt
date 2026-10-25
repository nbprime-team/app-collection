paint —— 触摸绘图板（app-collection 工程示例）
==============================================

⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

用途：在 Prime 上用手指画画。同时是**新人实操项目**的参考实现：
把"屏幕 / 输入 / 绘图 / 工具条"这几件常用事集中在一个文件里，便于对照阅读。

操作
----
画布区（上方 200 行）
  触摸拖动 = 用当前颜色与笔刷画线
工具条（底部 40 行）
  8 个色块  = 选颜色（选中项有黄色描边）
  3 个方块  = 选笔刷粗细（1 / 3 / 5 像素）
  CLEAR     = 清空画布（白色）
退出
  ESC 或 ON（退出前会还原固件钩子）

构建 / 部署
-----------
  source ../../../toolchain/scripts/env.sh
  make                 # -> paint.elf
  make deploy          # -> paint.hpappdir/
  把 paint.hpappdir/ 拷入计算器 C:\DATA\ 运行。

实现要点
--------
* 输入：SDK 原生钩子（prime_hook.c + platform/prime_input.S）；触摸事件是
  **多触点列表**（APP_EV_TICK + count/每项 12 字节），不是单点轮询；
* 绘图：Bresenham 连线 + 方形笔头盖章（触屏采样点稀疏，必须连线）；
* 内存：**不额外开画布缓冲**——画好的像素直接留在帧缓冲里，每帧只重绘
  会变的部分（工具条），这是最小内存做法；
* 命中检测：工具条上的色块/按钮用矩形包含判断，命中则不当作画画；
* 文本：共享字体资源 resources/prime-unifont；
* 运行支撑：toolchain/examples/app-common。

状态
----
构建通过（ELF32/DYN/ARM，soft-float；含 R_ARM_RELATIVE）；**真机未验证**。
