#!/usr/bin/env python3
"""把 GUI 截图钩子落盘的 raw RGBA 转成 PNG。

两个 GUI 的 AQUA_GUI_SCREENSHOT=<目录> 会写两个文件：
    aqua_gui.raw   宽×高×4 字节，RGBA 行主序（无 padding、无文件头）
    aqua_gui.meta  "宽 高"（一行文本）

本脚本只依赖标准库（zlib），不需要 Pillow / ImageMagick —— 截图流程不该额外要求
装东西，CI 与本地都直接用。

用法：
    python raw2png.py <目录>              # 读 <目录>/aqua_gui.raw，写 <目录>/aqua_gui.png
    python raw2png.py <xxx.raw> [out.png] # 也接受直接给 .raw 文件
"""

import struct
import sys
import zlib
from pathlib import Path


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    stride = width * 4
    # PNG 每行前有一个 filter 字节，这里一律 0（None）。
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2

    src = Path(sys.argv[1])
    if src.is_dir():
        raw_path = src / "aqua_gui.raw"
        meta_path = src / "aqua_gui.meta"
        out = Path(sys.argv[2]) if len(sys.argv) > 2 else src / "aqua_gui.png"
    else:
        # 直接给 .raw：meta 视为同名 .meta（钩子总是成对写）。
        raw_path = src
        meta_path = src.with_suffix(".meta")
        out = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".png")

    if not raw_path.is_file() or not meta_path.is_file():
        print(f"missing {raw_path} or {meta_path}", file=sys.stderr)
        return 1

    width, height = (int(v) for v in meta_path.read_text().split())
    rgba = raw_path.read_bytes()
    expected = width * height * 4
    if len(rgba) != expected:
        print(f"size mismatch: {len(rgba)} bytes, expected {expected} ({width}x{height})",
              file=sys.stderr)
        return 1

    write_png(out, width, height, rgba)
    print(f"wrote {out} ({width}x{height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
