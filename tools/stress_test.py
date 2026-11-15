#!/usr/bin/env python3
"""Chrono Node stress test: drives the clock over USB serial and its web API.

Modes:
  stress        Randomly combines features (colour styles, effects, transitions,
                screens, preview, night mode, weather/geocode/OTA-check TLS calls,
                photo upload/delete, banner, web polling, command bursts) for
                --minutes, with a web firmware upload every --ota-every minutes.
  ota-upload    Uploads a firmware .bin through the web page's PIN flow.
  ota-release   Installs a GitHub release (tag) through the web page's PIN flow.

The serial port is opened without toggling DTR/RTS; if the adapter resets the
clock anyway, the test waits until it's back online.
It watches the log for crashes, unexpected reboots, errors and heap, and
prints a report. Restore your own settings afterwards (see README).

  uv run --with pyserial --with requests --with pillow tools/stress_test.py \\
      stress --host 192.168.1.93 --minutes 60 --bin .pio/build/cyd/chrono-node-fw.bin
"""
from __future__ import annotations

import argparse
import collections
import glob
import io
import random
import re
import sys
import threading
import time

import requests
import serial

CRASH_PATTERNS = ("Guru Meditation", "Backtrace:", "abort() was called", "Brownout",
                  "task_wdt", "Task watchdog", "stack overflow", "CORRUPT HEAP", "assert failed")
STYLES = ["time", "rainbow", "cycle", "digits", "sunset", "ocean", "aurora", "neon", "fire",
          "candy", "custom", "mix"]
SPEEDS = ["slow", "normal", "fast", "veryfast"]
TRANSITIONS = ["none", "wipe", "slide", "blinds", "fade"]
SCREENS = ["CLOCK", "FORECAST", "PHOTO", "INFO", "SETTINGS", "STORAGE", "UPDATE"]
PLACES = ["Da Nang", "Hue", "Ha Noi", "Can Tho", "Vung Tau", "Da Lat"]


def default_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.wchusbserial*") +
                   glob.glob("/dev/ttyUSB*"))
    if not ports:
        sys.exit("No serial port found; pass --port")
    return ports[0]


class Monitor:
    """Reads the serial log in the background and keeps the numbers we care about."""

    def __init__(self, port: str, log_path: str):
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.log = open(log_path, "a", encoding="utf-8")
        self.lock = threading.Lock()
        self.cond = threading.Condition()
        self.lines: collections.deque[tuple[float, str]] = collections.deque(maxlen=4000)
        self.boots: list[float] = []
        self.crashes: list[str] = []
        self.errors: collections.Counter[str] = collections.Counter()
        self.heap: list[tuple[float, int, int, int]] = []  # time, free, min, largest
        self.stack: list[tuple[int, int]] = []             # loop, net
        self.reset_reasons: list[str] = []
        self.weather_ok = 0
        self.pin: str | None = None
        self.stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        buf = b""
        while not self.stop:
            try:
                buf += self.ser.read(4096)
            except serial.SerialException as e:
                self.note(f"!! serial error: {e}")
                time.sleep(1)
                continue
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._line(raw.decode(errors="replace").rstrip("\r"))

    def note(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        self.log.write(f"{stamp} {text}\n")
        self.log.flush()

    def _line(self, line: str) -> None:
        now = time.time()
        self.note(line)
        with self.cond:
            self.lines.append((now, line))
            if "] Booting " in line:
                self.boots.append(now)
            if any(p in line for p in CRASH_PATTERNS):
                self.crashes.append(time.strftime("%H:%M:%S ") + line)
            if "ERROR" in line or "failed" in line.lower() or "X509" in line:
                self.errors[re.sub(r"\d+", "#", line)[:110]] += 1
            if line.startswith("[WEATHER] ") and " C, " in line:
                self.weather_ok += 1
            m = re.search(r"heap free (\d+) B \(min (\d+) B, largest block (\d+) B\)", line)
            if m:
                self.heap.append((now, *map(int, m.groups())))
            m = re.search(r"Last reset: ([^,]+), stack free: loop (\d+) B, net (\d+) B", line)
            if m:
                self.reset_reasons.append(m.group(1))
                self.stack.append((int(m.group(2)), int(m.group(3))))
            m = re.search(r"Update PIN (\d{4})", line)
            if m:
                self.pin = m.group(1)
            self.cond.notify_all()

    def send(self, cmd: str) -> None:
        with self.lock:
            self.note(f">> {cmd}")
            self.ser.write((cmd + "\r\n").encode())
            time.sleep(0.15)

    def wait_for(self, text: str, timeout: float, since: float | None = None) -> str | None:
        since = time.time() if since is None else since
        end = time.time() + timeout
        with self.cond:
            while True:
                for t, line in self.lines:
                    if t >= since and text in line:
                        return line
                left = end - time.time()
                if left <= 0:
                    return None
                self.cond.wait(min(left, 0.5))


class Web:
    """Polls the web API like an open settings page, recording failures."""

    def __init__(self, host: str, mon: Monitor):
        self.base = f"http://{host}"
        self.mon = mon
        self.ok = 0
        self.fail: collections.Counter[str] = collections.Counter()
        self.slow = 0
        self.paused = threading.Event()
        self.stop = False

    def get(self, path: str, timeout: float = 15) -> requests.Response | None:
        t = time.time()
        try:
            r = requests.get(self.base + path, timeout=timeout)
            if time.time() - t > 3:
                self.slow += 1
            if r.status_code == 200:
                self.ok += 1
                return r
            self.fail[f"{path.split('?')[0]} HTTP {r.status_code}"] += 1
        except requests.RequestException as e:
            self.fail[f"{path.split('?')[0]} {type(e).__name__}"] += 1
        return None

    def poll_forever(self) -> None:
        n = 0
        while not self.stop:
            if not self.paused.is_set():
                self.get("/api/status")
                if n % 5 == 0:
                    self.get("/api/settings")
                n += 1
            time.sleep(2)

    def command(self, cmd: str) -> bool:
        try:
            r = requests.post(self.base + "/api/command", data=cmd.encode(),
                              headers={"Content-Type": "text/plain"}, timeout=15)
            self.mon.note(f">> web {cmd} -> {r.status_code}")
            return r.status_code == 202
        except requests.RequestException as e:
            self.fail[f"/api/command {type(e).__name__}"] += 1
            return False

    def request_pin(self) -> str | None:
        self.mon.pin = None
        since = time.time()
        try:
            requests.post(self.base + "/api/ota/pin", timeout=25)
        except requests.RequestException:
            return None
        line = self.mon.wait_for("Update PIN", 25, since)
        return re.search(r"PIN (\d{4})", line).group(1) if line else None


def test_jpeg() -> bytes:
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (320, 240))
    d = ImageDraw.Draw(img)
    for y in range(240):
        d.line([(0, y), (319, y)], fill=(random.randrange(256), y, 255 - y))
    d.text((120, 110), "STRESS TEST", fill=(255, 255, 255))
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85)
    return out.getvalue()


def wait_reboot(mon: Monitor, since: float, timeout: float = 90) -> float | None:
    """Waits for a boot after `since`; returns its time."""
    end = time.time() + timeout
    while time.time() < end:
        boots = [b for b in mon.boots if b >= since]
        if boots:
            return boots[0]
        time.sleep(0.5)
    return None


def ota_upload(web: Web, mon: Monitor, path: str, confirm: bool = True) -> tuple[bool, str]:
    """Web firmware upload through the PIN flow; True if the new image boots (and confirms)."""
    data = open(path, "rb").read()
    web.paused.set()
    try:
        pin = web.request_pin()
        if not pin:
            return False, "no PIN"
        t0 = time.time()
        try:
            r = requests.post(f"{web.base}/api/ota?pin={pin}",
                              files={"firmware": ("firmware.bin", data, "application/octet-stream")},
                              timeout=180)
        except requests.RequestException as e:
            return False, f"upload {type(e).__name__}"
        up = time.time() - t0
        if r.status_code != 200:
            return False, f"upload HTTP {r.status_code} {r.text[:80]}"
        boot = wait_reboot(mon, t0)
        if not boot:
            return False, f"uploaded in {up:.0f}s but no reboot"
        if not confirm:
            return True, f"uploaded {len(data) // 1024} KB in {up:.0f}s, rebooted"
        ok = mon.wait_for("New firmware confirmed", 100, boot)
        if not ok:
            return False, f"uploaded in {up:.0f}s, rebooted, never confirmed"
        return True, f"uploaded {len(data) // 1024} KB in {up:.0f}s, confirmed {ok and time.time() - boot:.0f}s after boot"
    finally:
        time.sleep(5)
        web.paused.clear()


def ota_release(web: Web, mon: Monitor, tag: str) -> tuple[bool, str]:
    web.paused.set()
    try:
        pin = web.request_pin()
        if not pin:
            return False, "no PIN"
        t0 = time.time()
        r = requests.post(f"{web.base}/api/ota/release?tag={tag}&pin={pin}", timeout=10)
        if r.status_code != 202:
            return False, f"HTTP {r.status_code} {r.text[:80]}"
        boot = wait_reboot(mon, t0, 240)
        if not boot:
            return False, "no reboot within 4 min"
        ok = mon.wait_for("New firmware confirmed", 100, boot)
        return bool(ok), f"downloaded+installed in {boot - t0:.0f}s, " + (
            "confirmed" if ok else "never confirmed")
    finally:
        time.sleep(5)
        web.paused.clear()


def stress(args, web: Web, mon: Monitor) -> list[str]:
    results = []
    jpeg = test_jpeg()
    start = time.time()
    end = start + args.minutes * 60
    next_info = 0.0
    next_ota = start + args.ota_every * 60 if args.ota_every and args.bin else float("inf")
    counts: collections.Counter[str] = collections.Counter()
    expected_boots = 0
    next_progress = start + 300

    def act() -> str:
        a = random.choices(
            ["style", "speed", "effects", "transition", "screen", "preview", "weather", "otacheck",
             "geocode", "photo", "upload", "banner", "night", "burst", "webburst", "mix"],
            weights=[10, 4, 8, 5, 14, 4, 4, 3, 3, 3, 2, 3, 3, 3, 3, 2])[0]
        via = web.command if random.random() < 0.5 else (lambda c: mon.send(c) or True)
        if a == "style":
            via(f"COLOR {random.choice(STYLES)}")
        elif a == "speed":
            via(f"COLOR SPEED {random.choice(SPEEDS)}")
        elif a == "mix":
            via(f"COLOR MIX EVERY {random.choice([1, 2, 5])}")
        elif a == "effects":
            for fx in ("SHINE", "PULSE", "GLOW"):
                via(f"EFFECT {fx} {random.choice(['ON', 'OFF'])}")
        elif a == "transition":
            via(f"TRANSITION {random.choice(TRANSITIONS)}")
        elif a == "screen":
            via(f"SCREEN {random.choice(SCREENS)}")
        elif a == "preview":
            via("PREVIEW")
        elif a == "weather":
            via("WEATHER UPDATE")
        elif a == "otacheck":
            mon.send("OTA CHECK")
        elif a == "geocode":
            web.get("/api/geocode?q=" + random.choice(PLACES), timeout=20)
        elif a == "photo":
            via("PHOTO SHOW")
        elif a == "upload":
            try:
                r = requests.post(web.base + "/api/photo",
                                  files={"photo": ("photo.jpg", jpeg, "image/jpeg")}, timeout=30)
                slot = r.json().get("slot") if r.status_code == 200 else None
                if slot:
                    via(f"PHOTO SHOW {slot}")
                    time.sleep(3)
                    via(f"PHOTO DEL {slot}")
                else:
                    web.fail[f"photo upload HTTP {r.status_code} {r.text[:60]}"] += 1
            except requests.RequestException as e:
                web.fail[f"photo upload {type(e).__name__}"] += 1
        elif a == "banner":
            via("BANNER SHOW")
        elif a == "night":
            via(random.choice(["NIGHT ON", "NIGHT OFF"]))
        elif a == "burst":  # screen changes faster than transitions finish
            for _ in range(8):
                mon.send(f"SCREEN {random.choice(SCREENS)}")
        elif a == "webburst":
            ts = [threading.Thread(target=web.get, args=(p,)) for p in
                  ["/", "/api/status", "/api/settings", "/api/photo?n=1", "/api/status"]]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
        return a

    while time.time() < end:
        now = time.time()
        if now >= next_info:
            mon.send("INFO")
            next_info = now + 30
        if now >= next_ota:
            mon.note("== OTA upload")
            ok, why = ota_upload(web, mon, args.bin)
            expected_boots += 1
            results.append(f"OTA upload during stress: {'OK' if ok else 'FAIL'} - {why}")
            print(results[-1], flush=True)
            next_ota = time.time() + args.ota_every * 60
            continue
        counts[act()] += 1
        time.sleep(random.uniform(2, 8))
        if time.time() >= next_progress:
            next_progress = time.time() + 300
            elapsed = (time.time() - start) / 60
            h = mon.heap[-1] if mon.heap else None
            print(f"[{elapsed:5.1f} min] actions {counts.total()}, boots {len(mon.boots)}, "
                  f"crashes {len(mon.crashes)}, heap {h[1:] if h else '-'}", flush=True)

    mon.send("INFO")
    time.sleep(2)
    unexpected = len([b for b in mon.boots if b >= start]) - expected_boots
    results.append(f"Actions: {counts.total()} ({', '.join(f'{k} {v}' for k, v in counts.most_common())})")
    results.append(f"Unexpected reboots: {unexpected}")
    return results


def report(mon: Monitor, web: Web, results: list[str]) -> None:
    print("\n==== Report ====")
    for r in results:
        print(r)
    print(f"Crash/watchdog lines: {len(mon.crashes)}")
    for c in mon.crashes[:10]:
        print("  ", c)
    print(f"Reset reasons seen: {collections.Counter(mon.reset_reasons)}")
    if mon.heap:
        frees = [h[1] for h in mon.heap]
        largest = [h[3] for h in mon.heap]
        print(f"Heap free: first {frees[0]}, last {frees[-1]}, lowest {min(frees)}; "
              f"lowest ever (min) {min(h[2] for h in mon.heap)}; largest block lowest {min(largest)}")
    if mon.stack:
        print(f"Stack free lowest: loop {min(s[0] for s in mon.stack)} B, net {min(s[1] for s in mon.stack)} B")
    print(f"Weather fetches OK: {mon.weather_ok}")
    print(f"Web requests OK: {web.ok}, slow (>3 s): {web.slow}, failed: {sum(web.fail.values())}")
    for k, v in web.fail.most_common(10):
        print(f"   {v} x {k}")
    print(f"Error lines: {sum(mon.errors.values())}")
    for k, v in mon.errors.most_common(15):
        print(f"   {v} x {k}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["stress", "ota-upload", "ota-release"])
    ap.add_argument("target", nargs="?", help="ota-upload: .bin path; ota-release: tag")
    ap.add_argument("--port", default=None)
    ap.add_argument("--host", default="chrono-node.local")
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--ota-every", type=float, default=0, help="stress: web OTA every N min (needs --bin)")
    ap.add_argument("--bin", default=None)
    ap.add_argument("--log", default="stress.log")
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()
    random.seed(args.seed)

    mon = Monitor(args.port or default_port(), args.log)
    # Some USB-serial adapters reset the board on open: wait until it's back online.
    if mon.wait_for("] Booting ", 4, 0):
        print("Board reset on port open; waiting for Wi-Fi...", flush=True)
        if not mon.wait_for("[WEB] Settings at", 60, 0):
            sys.exit("Clock didn't come online")
        time.sleep(3)
    web = Web(args.host, mon)
    results: list[str] = []
    try:
        if args.mode == "stress":
            threading.Thread(target=web.poll_forever, daemon=True).start()
            results = stress(args, web, mon)
        elif args.mode == "ota-upload":
            ok, why = ota_upload(web, mon, args.target)
            results.append(f"OTA upload: {'OK' if ok else 'FAIL'} - {why}")
        else:
            ok, why = ota_release(web, mon, args.target)
            results.append(f"OTA release {args.target}: {'OK' if ok else 'FAIL'} - {why}")
        mon.send("INFO")
        time.sleep(2)
    finally:
        web.stop = True
        mon.stop = True
        report(mon, web, results)


if __name__ == "__main__":
    main()
