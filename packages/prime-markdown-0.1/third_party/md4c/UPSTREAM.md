# MD4C upstream

- Repository: <https://github.com/mity/md4c>
- Pinned commit: `65c6c9d72cebd9a731aaa5597414ce04d9ea5de3`
- License: MIT; see `LICENSE.md`.
- Vendored files: `md4c.c` and `md4c.h`.
- Local compatibility patch: dollar math delimiters pair in source order
  instead of using emphasis punctuation rules. This makes adjacent spans such
  as `$a$或者$b$` parse as two formulas and accepts TeX's whitespace-tolerant
  `$ E = mc^2 $` spelling while preserving `$$` display delimiters.
  Single-dollar markers nested in `$$` spans are also retained as formula
  source, which supports TeX text such as `\text{if $n$ is even}` without
  breaking the outer display span.
