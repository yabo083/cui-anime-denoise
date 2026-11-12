#!/usr/bin/env python3
"""Regenerate the bundled fonts in assets/fonts.

Two families are shipped:

  HarmonyOS-Sans-SC-{Regular,Medium}.ttf
      Huawei's HarmonyOS Sans SC, subset to Latin-1 + general/CJK punctuation +
      the GB2312 repertoire (7 370 codepoints). Full files are 8.2 MB each and
      the subset is 1.7 MB; rarer CJK falls back to the system face at runtime
      (C:\\Windows\\Fonts\\msyh.ttc).

  lucide.ttf
      The complete Lucide icon font (2 376 icons, U+E000.. range). CUI's
      built-in icon subset uses the same codepoints, so loading this font as
      CUI_FONT_ICONS adds the extras in src/icons.h without losing any.

Usage
    python tools/gen_assets.py --models-dir _dl            # both families
    python tools/gen_assets.py --models-dir _dl --icons-only
    python tools/gen_assets.py --check                     # coverage report only

Downloads (only needed for the first run, then keep them in _dl/):
    npm pack lucide-static        # -> package/font/lucide.woff2 + codepoints.json
    HarmonyOS_Sans_SC_Regular.ttf / _Medium.ttf from
    https://github.com/ajacocks/harmonyos-sans-font/tree/master/HarmonyOS_Sans_SC

Requirements: pip install fonttools brotli
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "assets", "fonts")


def gb2312_chars():
    chars = set(range(0x20, 0x7F))      # ASCII
    chars |= set(range(0xA0, 0x100))    # Latin-1
    chars |= set(range(0x2000, 0x2070)) # general punctuation
    chars |= set(range(0x2190, 0x2200)) # arrows
    chars |= set(range(0x3000, 0x3040)) # CJK punctuation
    chars |= set(range(0xFF00, 0xFFF0)) # fullwidth forms
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(ord(bytes([hi, lo]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return sorted(chars)


def subset_font(src, dst, codepoints):
    from fontTools import subset
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.name_legacy = False
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.drop_tables += ["DSIG", "GSUB"]
    font = subset.load_font(src, opts)
    sub = subset.Subsetter(options=opts)
    sub.populate(unicodes=codepoints)
    sub.subset(font)
    subset.save_font(font, dst, opts)


def build_lucide(models, icons_only):
    from fontTools.ttLib import TTFont
    woff2 = os.path.join(models, "package", "font", "lucide.woff2")
    if not os.path.exists(woff2):
        sys.exit(f"missing {woff2} (run: npm pack lucide-static, then untar into {models})")
    tmp = os.path.join(models, "lucide-full.ttf")
    font = TTFont(woff2)
    font.flavor = None
    font.save(tmp)
    dst = os.path.join(FONTS, "lucide.ttf")
    os.replace(tmp, dst)
    print(f"lucide.ttf            {os.path.getsize(dst):>9} bytes")


def build_harmony(models, icons_only):
    if icons_only:
        return
    chars = gb2312_chars()
    for weight in ("Regular", "Medium"):
        src = os.path.join(models, f"HarmonyOS_Sans_SC_{weight}.ttf")
        if not os.path.exists(src):
            sys.exit(f"missing {src}")
        dst = os.path.join(FONTS, f"HarmonyOS-Sans-SC-{weight}.ttf")
        subset_font(src, dst, chars)
        print(f"HarmonyOS-Sans-SC-{weight} {os.path.getsize(dst):>9} bytes")


def check_coverage():
    """Every non-ASCII character in the C sources must exist in the UI font."""
    from fontTools.ttLib import TTFont
    cmap = TTFont(os.path.join(FONTS, "HarmonyOS-Sans-SC-Regular.ttf")).getBestCmap()
    missing = {}
    for root, _dirs, files in os.walk(os.path.join(ROOT, "src")):
        for name in files:
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(root, name)
            text = open(path, encoding="utf-8").read()
            for ch in text:
                if ord(ch) > 0x7F and ord(ch) not in cmap:
                    missing.setdefault(path, set()).add(ch)
    if missing:
        for path, chars in missing.items():
            print(f"{os.path.relpath(path, ROOT)}: {''.join(sorted(chars))}")
        sys.exit(1)
    print("font coverage OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models-dir", default=os.path.join(ROOT, "_dl"),
                    help="where the downloaded source fonts live")
    ap.add_argument("--icons-only", action="store_true")
    ap.add_argument("--check", action="store_true", help="only verify glyph coverage")
    args = ap.parse_args()

    if args.check:
        check_coverage()
        return
    os.makedirs(FONTS, exist_ok=True)
    build_lucide(args.models_dir, args.icons_only)
    build_harmony(args.models_dir, args.icons_only)
    check_coverage()


if __name__ == "__main__":
    main()
