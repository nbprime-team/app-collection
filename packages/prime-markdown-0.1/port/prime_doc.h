/*
 * prime_doc.h —— 内嵌示例文档（app-collection 无文件 API，文档编进 ELF）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * FileSystemPrime 只认这一个路径；内容随 ELF 一起加载，不依赖计算器文件系统。
 */
#ifndef PRIME_DOC_H
#define PRIME_DOC_H

/* 逻辑路径（ReaderOptions.document_root 之外，直接匹配即可） */
static const char kPrimeDocPath[] = "embedded.md";

static const char kPrimeDocText[] =
    "# Prime Markdown\n"
    "\n"
    "运行在 **HP Prime G1 原生固件** 上的 Markdown 阅读器。\n"
    "\n"
    "渲染核心来自 nMarkdown（md4c 解析 + FreeType/HarfBuzz 排版），\n"
    "输入走 nbprime 的原生钩子（`0x307FBFA0` + `svc 0x1003f`）。\n"
    "\n"
    "## 操作\n"
    "\n"
    "- 触屏：上/下滑或点按 —— 翻页\n"
    "- 触屏：左/右滑 —— 翻页\n"
    "- 键盘方向键 / ENTER —— 翻页\n"
    "- ESC / ON —— 退出\n"
    "\n"
    "## 列表与引用\n"
    "\n"
    "1. 第一项\n"
    "2. 第二项\n"
    "   - 嵌套\n"
    "\n"
    "> 引用：原生固件没有线程环境，所以这里用钩子而不是 Upsilon 的事件线程。\n"
    "\n"
    "## 代码\n"
    "\n"
    "```c\n"
    "prime_hook_install(on_event);   /* 装钩子 */\n"
    "prime_sys_get_event(event);     /* 回调内取事件 */\n"
    "prime_hook_remove();            /* 退出前还原 */\n"
    "```\n"
    "\n"
    "## 公式\n"
    "\n"
    "行内公式 $e^{i\\pi} + 1 = 0$，块级公式：\n"
    "\n"
    "$$\n"
    "\\int_0^1 x^2 \\, dx = \\frac{1}{3}\n"
    "$$\n"
    "\n"
    "---\n"
    "\n"
    "文档到此结束。\n";

static const unsigned int kPrimeDocLen = sizeof(kPrimeDocText) - 1;

#endif /* PRIME_DOC_H */
