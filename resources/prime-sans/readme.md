# Prime Sans

本目录存放从固件中提取的 Prime Sans 字体，以及一次**已搁置**的固件字体接口尝试记录。

## 来源

```
PRIME_APP.DAT
└─ FAT16 
   └─ PROGRAMS\MISC\ARMFIR.DAT      （8897536 字节，"数据分区"容器）
      └─ 段表 {offset,size} × 82
         ├─ 段 79 = Prime Sans Bold（102 KB，606 字形）
         ├─ 段 80 = Prime Sans      （4.6 MB，51285 字形，含 CJK）
         └─ 段 81 = Prime Sans Mono （44 KB，618 字形）
```

- **段 80**：文件内偏移 `0x40B124`，段大小 `0x466590`（4613520 字节）
  - 前 `0x1C`（28）字节 = 段头（含 3 个大端 hash，算法未知）
  - 之后 = TrueType 字体（sfnt，从 `0x40B140` 起）
  - **TTF 数据区容量 = 4613492 字节**
- 渲染：固件内置 FreeType（`ftglyph.o` 等）加载 TTF

> 目录里的三个 `.ttf` 是从固件提取的**参考副本**。应用目前**不引用**它们。

## 固件字体接口：已尝试，暂时搁置

> ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

曾按 `g1sdk`（Project Muteki）逆向出的 SVC 表接入固件字体渲染：

| 函数 | SVC |
|---|---|
| `GetCharWidth` / `GetFontHeight` | `0x1004d` / `0x1004e` |
| `GetFontType` / `SetFontType` | `0x1004f` / `0x10051` |
| `WriteChar` / `WriteString` | `0x10053` / `0x10054` |
| `rgbSetBkColor` / `rgbSetColor` | `0x10067` / `0x10068` |

**真机结果：程序重启** —— 说明这组 SVC 编号对**原生固件无效**（疑为 Upsilon 侧地址，
与 HP 原厂固件不是一套）。固件内部真正的字体 API 地址**目前未知**。

处置：

- 本方向**暂时搁置**，直到确认可用地址；
- 相关接口（`sans_draw.*`、`sans_svc.S`、`sans.mk`）与验证程序
  `examples/fonttest/` 已**回退删除**；
- 需要绘制字符串时，请使用 [`../prime-unifont/`](../prime-unifont/)
  —— 自带位图字模，**与固件无关**，在任意固件上都表现一致。

> 结论记于此，以免后人重复踩坑；待地址确认后再重启本方向。
