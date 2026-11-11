#!/usr/bin/env python3
"""Capture the Chrono Node screen over USB serial and save it as a PNG.

Usage (from the repository root):
    uv run --with pyserial --with pillow tools/screenshot.py [-p PORT] [-o shot.png]
        [--send "WIFI PORTAL"] [--wait-for "[WEATHER] "]

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


def capture(s):
    """Returns (w, h, data), or None if the frame was corrupted."""
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
    while len(data) < need + 12 and time.time() < deadline:
        data += s.read(need + 12 - len(data))
    # The frame must be followed immediately by the end marker.
    if not data[need:].startswith(b"\n[SHOT] END"):
        return None
    return w, h, data[:need]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port")
    ap.add_argument("-o", "--output", default="screenshot.png")
    ap.add_argument("--wait", type=float, default=8.0, help="seconds to wait for boot")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--wait-for", metavar="TEXT",
                    help="after --wait, keep reading until TEXT appears in the log (max 10 min)")
    ap.add_argument("--send", action="append", default=[],
                    help="console command to send before the capture (repeatable)")
    args = ap.parse_args()

    port = args.port or find_port()
    # Open with DTR/RTS released: asserting them can hold the ESP32 in reset.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 1, False, False
    with s:
        time.sleep(args.wait)
        if args.wait_for:
            seen, deadline = b"", time.time() + 600
            while args.wait_for.encode() not in seen:
                if time.time() > deadline:
                    sys.exit(f"Timed out waiting for {args.wait_for!r}")
                seen = (seen + s.read(512))[-4096:]
        for cmd in args.send:
            s.write(cmd.encode() + b"\r\n")
            time.sleep(3)
        for attempt in range(1, 4):
            frame = capture(s)
            if frame:
                break
            # Log lines from other firmware tasks can land inside the binary stream.
            print(f"Corrupted frame (attempt {attempt}), retrying", file=sys.stderr)
        else:
            sys.exit("No clean frame after 3 attempts")
    w, h, data = frame

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
