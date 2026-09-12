
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
