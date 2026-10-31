/*
 * prime_layout.h — 原版 block_layout 布局引擎的 C 接口
 * =====================================================
 * 复用 nMarkdown 的 VirtualDocumentLayout (block_layout.cpp):
 * 词级折行 + cluster 切片 + 标点禁则 + 空格压缩 + run 级混排
 * (文本/强调/代码/行内公式) + Fenwick 高度索引滚动。
 *
 * 使用流程:
 *   1. prime_render_init() 后调用 prime_layout_open(md, len) — 解析+初始化
 *   2. prime_layout_page_count(page_height) — 总页数
 *   3. prime_layout_render_page(page, page_height) — 渲染一页到 framebuffer
 *   4. 翻页直接换 page 号; 打开新文档前 prime_layout_close()
 */
#ifndef PRIME_LAYOUT_H
#define PRIME_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 打开 markdown 文本并初始化布局。md_len <= 0 时按 strlen。
 * 返回 0 = 成功; 非 0 = 解析/初始化失败 */
int prime_layout_open(const char* md_text, int md_len);

/* 文档总高 (px, 26.6 定点已转整数)。布局前为估计值, 逐页布局后收敛 */
int prime_layout_total_height(void);

/* 总页数 (page_height 为一页可用高度 px) */
int prime_layout_page_count(int page_height);

/* 渲染第 page 页 (0 起) 到 framebuffer。page_height 与 page_count 一致。
 * 返回 0 = 成功 */
int prime_layout_render_page(int page, int page_height);

/* 释放文档布局 (下次 open 前调用) */
void prime_layout_close(void);

#ifdef __cplusplus
}
#endif

#endif /* PRIME_LAYOUT_H */
