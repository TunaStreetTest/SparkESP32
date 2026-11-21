#!/usr/bin/env python3
"""Bench/QA driver for a SparkESP32 display on USB: commands and screenshots.

  devctl.py status                   firmware, settings, heap/stack, last sample
  devctl.py demo [load]              synthetic sample (load 0-100, default 42)
  devctl.py shot out.png [--swap]    CRC-verified framebuffer dump -> PNG (needs Pillow)
  devctl.py demoshot out.png [load]  demo + shot in one session, so it stays ONLINE
  devctl.py wifi [show|scan|clear|ssids <a,b>|url <url>]
  devctl.py name "<text>"            header title
  devctl.py rot [none|ccw|cw]        screen rotation
  devctl.py send "<console line>"    anything else, raw
  devctl.py log [seconds]            passively capture the console (default 15 s)

Wi-Fi passwords go through provision.py, which never prints them.
Port: --port PATH, $SPARKESP32_PORT, or autodetect (Espressif USB 303a:1001).
"""
import base64
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sparklink import Link, LinkError, clean_lines, connect, console_quote  # noqa: E402

AREA = re.compile(r"AREA (-?\d+) (-?\d+) (-?\d+) (-?\d+) (\d+) ([0-9a-f]{8})")
B64 = re.compile(r"[A-Za-z0-9+/=]+")


def parse_shot(raw):
    """Reassemble the SHOT-BEGIN..SHOT-END dump. Returns (w, h, fb) or raises LinkError."""
    lines = clean_lines(raw)
    try:
        start = next(i for i, ln in enumerate(lines) if ln.startswith("SHOT-BEGIN"))
    except StopIteration:
        raise LinkError("no SHOT-BEGIN in reply") from None
    w, h = map(int, lines[start].split()[1:3])
    fb, covered, i = bytearray(w * h * 2), 0, start + 1
    while i < len(lines) and not lines[i].startswith("SHOT-END"):
        m = AREA.match(lines[i].strip())
        i += 1
        if not m:
            continue
        x1, y1, x2, y2, nbytes = map(int, m.groups()[:5])
        data = bytearray()
        while i < len(lines) and len(data) < nbytes and B64.fullmatch(lines[i].strip() or "!"):
            data += base64.b64decode(lines[i].strip())
            i += 1
        if len(data) != nbytes or format(zlib.crc32(bytes(data)), "08x") != m.group(6):
            raise LinkError(f"strip at {x1},{y1} failed its size/CRC check")
        sw = x2 - x1 + 1
        for row in range(y2 - y1 + 1):
            dst = ((y1 + row) * w + x1) * 2
            fb[dst:dst + sw * 2] = data[row * sw * 2:(row + 1) * sw * 2]
        covered += sw * (y2 - y1 + 1)
    if covered != w * h:
        raise LinkError(f"frame coverage {covered}/{w * h} px")
    return w, h, fb


def save_png(path, w, h, fb, swap):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("shot needs Pillow: pip install pillow  (or: uv run --with pyserial --with pillow ...)")
    rgb = bytearray(w * h * 3)
    for p in range(w * h):
        lo, hi = fb[2 * p], fb[2 * p + 1]
        v = (lo << 8 | hi) if swap else (hi << 8 | lo)
        r, g, b = v >> 11, (v >> 5) & 0x3F, v & 0x1F
        rgb[3 * p:3 * p + 3] = bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    Image.frombytes("RGB", (w, h), bytes(rgb)).save(path)
    print("saved", path)


def shot(port, path, swap, pre=None):
    for attempt in range(1, 4):
        try:
            with connect(port) as link:
                if pre:
                    link.check(pre)
                w, h, fb = parse_shot(link.raw("shot", timeout=90))
            return save_png(path, w, h, fb, swap)
        except LinkError as e:
            print(f"shot attempt {attempt}/3: {e}", file=sys.stderr)
            time.sleep(1)
    sys.exit("shot failed")


def main():
    argv = sys.argv[1:]
    port = os.environ.get("SPARKESP32_PORT")
    if "--port" in argv:
        k = argv.index("--port")
        port = argv[k + 1]
        del argv[k:k + 2]
    swap = "--swap" in argv
    argv = [x for x in argv if x != "--swap"]
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 1
    cmd, args = argv[0], argv[1:]

    if cmd == "shot":
        return shot(port, args[0] if args else "shot.png", swap)
    if cmd == "demoshot":
        return shot(port, args[0] if args else "shot.png", swap,
                    pre="demo " + (args[1] if len(args) > 1 else "60"))
    if cmd == "log":
        secs = float(args[0]) if args else 15
        with (Link(port) if port else connect()) as link:
            t0, buf = time.monotonic(), b""
            while time.monotonic() - t0 < secs:
                buf += link.ser.read(65536)
        print("\n".join(clean_lines(buf)))
        return 0

    if cmd == "status":
        line = "status"
    elif cmd == "demo":
        line = "demo " + (args[0] if args else "42")
    elif cmd == "wifi":
        if args and args[0] == "pass":
            sys.exit("use tools/provision.py --ssids ... for the password; it never prints it")
        line = "wifi " + " ".join(args[:1] + [console_quote(x) for x in args[1:]])
    elif cmd == "name":
        if len(args) != 1:
            sys.exit('usage: devctl.py name "<text>"')
        line = "name " + console_quote(args[0])
    elif cmd == "rot":
        line = "rot " + (args[0] if args else "")
    elif cmd == "send":
        line = " ".join(args)
    else:
        print(__doc__)
        return 1

    try:
        with connect(port) as link:
            out = link.cmd(line.strip(), timeout=30)
    except LinkError as e:
        sys.exit(f"error: {e}")
    print("\n".join(out))
    return 1 if out and out[-1].startswith("ERR") else 0


if __name__ == "__main__":
    sys.exit(main())
