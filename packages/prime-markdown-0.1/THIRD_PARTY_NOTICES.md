# Third-party notices

## MD4C

The project vendors MD4C at commit
`65c6c9d72cebd9a731aaa5597414ce04d9ea5de3`. It is distributed under the
MIT license in [`third_party/md4c/LICENSE.md`](third_party/md4c/LICENSE.md).

## FreeType

The project vendors the modules used by its minimal FreeType 2.14.3 build.
FreeType is distributed under the FreeType Project License in
[`third_party/freetype/LICENSE.TXT`](third_party/freetype/LICENSE.TXT); archive
and checksum details are recorded in
[`third_party/freetype/UPSTREAM.md`](third_party/freetype/UPSTREAM.md).

## HarfBuzz

The project vendors HarfBuzz 14.2.1 from its official release archive. It is
distributed under the license in
[`third_party/harfbuzz/LICENSE`](third_party/harfbuzz/LICENSE); source and
archive checksum details are recorded in
[`third_party/harfbuzz/UPSTREAM.md`](third_party/harfbuzz/UPSTREAM.md).

## KaTeX symbol registry

The bounded, no-argument formula symbol catalog in
[`src/math/math_symbol_table.inc`](src/math/math_symbol_table.inc) is derived
from KaTeX's `src/symbols.ts` at commit
`2c6143a6dd7c168cef602c1e29f8add66f7fcc19`, then filtered to glyphs available
in the embedded Latin Modern Math face. KaTeX is distributed under the MIT
license in [`third_party/KATEX_LICENSE`](third_party/KATEX_LICENSE).

## DejaVu Sans and DejaVu Sans Mono

The built-in Body and Monospace faces are DejaVu Sans and DejaVu Sans Mono.
Their Bitstream Vera / Arev-derived license and notices are included in
[`assets/fonts/LICENSE_DEJAVU`](assets/fonts/LICENSE_DEJAVU).

## Sarasa Fixed SC

The separately loaded CJK face is a calculator-sized subset of Sarasa Fixed SC
Regular 1.0.40 by Renzhi Li and its upstream contributors. It is distributed
under the SIL Open Font License 1.1 in
[`assets/fonts/LICENSE_SARASA`](assets/fonts/LICENSE_SARASA).

Exact upstream versions, input/output hashes, and the reproducible Sarasa
subset command are recorded in
[`assets/fonts/UPSTREAM.md`](assets/fonts/UPSTREAM.md). License text is kept in
these repository notices; nMarkdown does not display it or copy it into a font
pack.

## Latin Modern Math

Formula rendering uses Latin Modern Math. Its GUST Font License is included in
[`assets/fonts/LICENSE_LATIN_MODERN_MATH`](assets/fonts/LICENSE_LATIN_MODERN_MATH).

The repository also retains two unused STIX Two Math comparison inputs from
renderer development. They are not linked or packaged. Their SIL Open Font
License 1.1 is included in
[`assets/fonts/LICENSE_STIX_TWO`](assets/fonts/LICENSE_STIX_TWO).

## Unicode Character Database

The generated case-folding and canonical-normalization tables are derived from
the Unicode Character Database, version 17.0.0. Unicode data files and software
are copyright © 1991-present Unicode, Inc. and distributed under the
[Unicode License v3](https://www.unicode.org/license.txt) (`Unicode-3.0`). The
generator downloads only the versioned `UnicodeData.txt`, `CaseFolding.txt`,
and `DerivedNormalizationProps.txt` data files.
