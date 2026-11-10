# Chrono Node

Chrono Node is an offline-first desktop smart clock built on the ESP32-2432S028R
("Cheap Yellow Display", CYD) with an external DS3231 RTC as its time source.

**Milestone 1 (current):** TFT bring-up, I2C on GPIO27/GPIO22, DS3231 detection,
and a 24-hour clock read directly from the DS3231. No Wi-Fi or NTP, and no touch input.

## Hardware

| Part | Notes |
|------|-------|
| ESP32-2432S028R (CYD) | 320×240 ILI9341 TFT, CH340 USB-UART |
| DS3231 RTC module | I2C address `0x68` (ZS-042 modules also show an EEPROM at `0x57`) |

### Wiring: use the CYD **LEFT** 4-pin connector

```text
CYD LEFT CONNECTOR           DS3231
3.3V   --------------------> VCC
GPIO27 --------------------> SDA
GPIO22 --------------------> SCL
GND    --------------------> GND
```

Don't use the right "Extended IO" connector. It has no 3.3V pin, and GPIO21 is the
CYD's TFT backlight. The firmware always calls `Wire.begin(27, 22)` explicitly.

> ZS-042 style DS3231 modules include a trickle-charge circuit intended for LIR2032 cells.
> If you fit a non-rechargeable CR2032, consider removing the charge resistor or diode.

## Development setup (macOS)

```bash
uv tool install platformio      # or: brew install platformio / pipx install platformio
pio --version
```

VS Code with the PlatformIO IDE extension is optional. The CLI is enough.

## Build, flash, monitor

```bash
pio run                 # build -> .pio/build/cyd/chrono-node-fw.bin
pio device list         # find the board's port
pio run -t upload       # flash (add --upload-port <port> if several ports exist)
pio device monitor      # serial console, 115200 baud
```

All TFT_eSPI configuration lives in `platformio.ini` (`build_flags`), so a fresh
checkout builds without editing anything in the global PlatformIO packages directory.

## Connecting the device

1. Plug the CYD directly into the computer with a **data-capable** USB cable.
2. `pio device list` should show the CH340 bridge, e.g. `/dev/cu.usbserial-1210`,
   `USB VID:PID=1A86:7523`. macOS includes the CH340 driver. Boards with a CH9102
   appear as `/dev/cu.wchusbserial-*` and may need the WCH driver.
3. If uploading fails at high baud, the project already uses `upload_speed = 460800`
   (921600 is unreliable with the CH340). If auto-reset fails, hold **BOOT** while
   the upload starts connecting.

## Setting the RTC time (development)

Firmware **never** writes the RTC at boot. Normal operation only reads the DS3231.
To set it, open `pio device monitor` and type:

```text
SET 2026-11-08 14:32:51     # 24-hour local time, written once to the DS3231
GET                         # print the current DS3231 time
HELP
```

`SET` also clears the DS3231 "oscillator stopped" (lost power) flag. If that flag is
set at boot, the serial log warns about it and the screen shows
`RTC lost power - time not set` until you set the time.

## Expected serial output

```text
[Chrono Node] Booting chrono-node-fw v0.1.0
[DISPLAY] Initialized
[I2C] SDA=GPIO27 SCL=GPIO22 @ 100000 Hz
[RTC] DS3231 detected at 0x68
[RTC] 14:32:51 2026-11-08
```

If the RTC is missing, you get `[RTC] ERROR: DS3231 not detected` and an error
screen. The firmware keeps running and re-probes every 2 s, so a reconnected RTC is
picked up without a reboot.

To disable the once-per-second time log, set `kLogEverySecond = false` in `include/config.h`.

## Project layout

```text
platformio.ini            board, libraries, TFT_eSPI pin configuration
scripts/firmware_name.py  names build artifacts chrono-node-fw.*
include/config.h          pins, I2C address, timing, firmware metadata
src/main.cpp              setup/loop, 1 Hz update via millis()
src/rtc_manager.*         I2C + DS3231 detection/read/write
src/display_manager.*     TFT rendering (sprite-based, flicker-free time)
src/serial_console.*      SET/GET/HELP development commands
```

## Troubleshooting

- **Garbled or colour-inverted display:** some CYD variants use an ST7789 panel. See the
  comment at the end of `platformio.ini`.
- **`DS3231 not detected`:** check that the RTC is on the LEFT connector and that SDA and SCL
  aren't swapped.
