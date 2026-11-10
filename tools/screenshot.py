#!/usr/bin/env python3
"""Capture the Chrono Node screen over USB serial and save it as a PNG.

Usage (from the repository root):
    uv run --with pyserial --with pillow tools/screenshot.py [-p PORT] [-o shot.png]
        [--send "WIFI PORTAL"]

Sends the SHOT console command and decodes the big-endian RGB565 frame the
firmware streams back. Opening the port resets the board on most CYDs, so the
script first waits for it to boot (--wait). Takes ~15 s at 115200 baud.
"""

import argparse
import glob
import re
import sys
import time

import serial
from PIL import Image


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.wchusbserial*") +
                   glob.glob("/dev/ttyUSB*"))
    if not ports:
        sys.exit("No serial port found; pass --port")
    return ports[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port")
    ap.add_argument("-o", "--output", default="screenshot.png")
    ap.add_argument("--wait", type=float, default=8.0, help="seconds to wait for boot")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--send", action="append", default=[],
                    help="console command to send before the capture (repeatable)")
    args = ap.parse_args()

    port = args.port or find_port()
    with serial.Serial(port, 115200, timeout=1) as s:
        time.sleep(args.wait)
        for cmd in args.send:
            s.write(cmd.encode() + b"\r\n")
            time.sleep(3)
        s.reset_input_buffer()
        s.write(b"SHOT\r\n")

        buf = b""
        deadline = time.time() + 10
        while (m := re.search(rb"\[SHOT\] BEGIN (\d+) (\d+)\n", buf)) is None:
            if time.time() > deadline:
                sys.exit("No SHOT response")
            buf += s.read(256)
        w, h = int(m.group(1)), int(m.group(2))
        data = buf[m.end():]
        need = w * h * 2
        deadline = time.time() + 60
        while len(data) < need and time.time() < deadline:
            data += s.read(need - len(data))
        if len(data) < need:
            sys.exit(f"Short frame: {len(data)}/{need} bytes")

    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        c = (data[2 * i] << 8) | data[2 * i + 1]
        r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
        px[i % w, i // w] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    if args.scale > 1:
        img = img.resize((w * args.scale, h * args.scale), Image.NEAREST)
    img.save(args.output)
    print(f"Saved {args.output} ({w}x{h}) from {port}")


if __name__ == "__main__":
    main()
