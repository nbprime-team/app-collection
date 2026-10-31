# sh 0.2 —— 极简终端（包格式）

> AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

- 运行：把本目录放到 `C:\DATA\pkg\sh-0.2\`，用 `main.py`（Python 启动器）装载
  `bin/sh.elf`；shell 自己从 `C:\DATA\pkg\` 读包。
- 源码在 `src/`；构建见 `src/Makefile`（需要 toolchain 与本包的依赖资源）。
- 路径只有反斜杠；`~` = `C:\DATA`；`/` 会直接报错。
