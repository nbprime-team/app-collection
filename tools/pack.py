#!/usr/bin/env python3
# AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
"""pack.py —— 从 packages/<name>-<ver>/ 生成三种形态：

  sys    共享核心区（C:\\DATA\\sys\\<kind>\\<name>-<ver>\\） + core.db 行
  entry  薄 .hpappdir 入口（只含 main.py + 元数据/图标，ELF 指向 sys）
  bundle 自包含 .hpappdir（payload 与 main.py 同目录，可直接拷给别人）

用法：
  python3 tools/pack.py sys    packages/hello-0.1  out/sys
  python3 tools/pack.py entry  packages/hello-0.1  out/hello.hpappdir
  python3 tools/pack.py bundle packages/hello-0.1  out/hello.hpappdir
  python3 tools/pack.py verify out/sys

约定：包根 = pkg.cfg + payload；src/ 不进入产物。
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, "packages", "sh-0.2", "main.py")
SYS_DEVICE = "C:\\DATA\\sys"


def read_cfg(pkgdir):
    cfg = {}
    with open(os.path.join(pkgdir, "pkg.cfg"), "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            cfg[k.strip()] = v.strip()
    return cfg


def payload(pkgdir):
    for name in sorted(os.listdir(pkgdir)):
        if name in ("pkg.cfg", "src", "README.md", "__pycache__"):
            continue
        yield name


def copy_payload(pkgdir, dst):
    os.makedirs(dst, exist_ok=True)
    for name in payload(pkgdir):
        src = os.path.join(pkgdir, name)
        d = os.path.join(dst, name)
        if os.path.isdir(src):
            shutil.copytree(src, d, dirs_exist_ok=True)
        else:
            shutil.copy2(src, d)


def make_main(pkg, app_dir, elf):
    with open(TEMPLATE, "r", encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r'APP_DIR\s*=\s*"[^"]*"',
                  lambda m: 'APP_DIR = "%s"' % app_dir.replace("\\", "\\\\"),
                  text, count=1)
    text = re.sub(r'APP_ELF\s*=\s*"[^"]*"', 'APP_ELF = "%s"' % elf, text, count=1)
    text = text.replace("[sh]", "[%s]" % pkg.get("name", "app"))
    return text


def is_elf_kind(cfg):
    return cfg.get("kind", "app") in ("app", "tool")


def cmd_sys(pkgdir, dest):
    cfg = read_cfg(pkgdir)
    name, ver = cfg["name"], cfg["version"]
    kind = "lib" if cfg.get("kind") == "lib" else "bin"
    target = os.path.join(dest, kind, "%s-%s" % (name, ver))
    copy_payload(pkgdir, target)
    line = "%s %s %s %s" % (name, ver, kind, "/".join([kind, "%s-%s" % (name, ver)]))
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(dest, "core.db"), "a", encoding="utf-8") as f:
        f.write(line + "\n")
    print("sys:", target)


def cmd_entry(pkgdir, dest):
    cfg = read_cfg(pkgdir)
    name, ver = cfg["name"], cfg["version"]
    os.makedirs(dest, exist_ok=True)
    # 元数据与图标（小文件）拷过去；payload 留在 sys
    for n in payload(pkgdir):
        if n.lower().endswith((".hpapp", ".hpappnote", ".hpappprgm", ".png", ".hpappdir")) or n in ("icon.png",):
            s = os.path.join(pkgdir, n)
            if os.path.isfile(s):
                shutil.copy2(s, os.path.join(dest, n))
    if is_elf_kind(cfg):
        entry = cfg["entry"]
        elf = os.path.basename(entry)
        sub = "lib" if cfg.get("kind") == "lib" else "bin"
        app_dir = "%s\\%s\\%s-%s" % (SYS_DEVICE, sub, name, ver)
        with open(os.path.join(dest, "main.py"), "w", encoding="utf-8") as f:
            f.write(make_main(cfg, app_dir, elf))
    print("entry:", dest, "(payload 在 %s)" % SYS_DEVICE)


def cmd_bundle(pkgdir, dest):
    cfg = read_cfg(pkgdir)
    copy_payload(pkgdir, dest)
    if is_elf_kind(cfg):
        base = os.path.basename(dest.rstrip("\\/"))
        app_dir = "C:\\DATA\\" + base
        with open(os.path.join(dest, "main.py"), "w", encoding="utf-8") as f:
            f.write(make_main(cfg, app_dir, os.path.basename(cfg["entry"])))
    print("bundle:", dest)


def cmd_verify(sysroot):
    db = os.path.join(sysroot, "core.db")
    bad = 0
    with open(db, "r", encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            rel = parts[3].replace("/", os.sep)
            if not os.path.isdir(os.path.join(sysroot, rel)):
                print("MISSING:", rel)
                bad += 1
    print("verify:", "ok" if not bad else "%d missing" % bad)
    return 0 if not bad else 1


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, a = sys.argv[1], sys.argv[2]
    b = sys.argv[3] if len(sys.argv) > 3 else None
    if cmd == "sys":
        cmd_sys(a, b)
    elif cmd == "entry":
        cmd_entry(a, b)
    elif cmd == "bundle":
        cmd_bundle(a, b)
    elif cmd == "verify":
        return cmd_verify(a)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
