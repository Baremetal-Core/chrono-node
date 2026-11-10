# Chrono Node

Repository name:

```text
chrono-node
```

Project display name:

```text
Chrono Node
```

Firmware/artifact naming should use:

```text
chrono-node-fw
```

Use the name **Chrono Node** consistently in:

- documentation
- serial startup banners
- firmware metadata
- project comments
- future release artifacts

Do not use alternative names such as `chrononode` unless required by a tool or identifier that does not support hyphens.

---

# Project Overview

Chrono Node is a compact desktop smart clock built around an ESP32 development board with an integrated 2.8-inch TFT touchscreen display.

The board is an ESP32-2432S028R, commonly known as the "Cheap Yellow Display" (CYD).

The device will eventually function as an offline-first smart clock with optional network and environmental-monitoring features.

The primary time source while operating offline will be an external DS3231 RTC module with battery backup.

Wi-Fi and NTP synchronization may be added later so that the RTC can periodically be corrected using Internet time.

The design principles are:

- The clock must work without Wi-Fi.
- The DS3231 must preserve time through power loss.
- When network synchronization is added later, NTP should correct the DS3231 rather than replace it as the offline time source.
- Network availability must never be required for basic clock functionality.

Future versions may also include:

- Wi-Fi configuration
- NTP synchronization
- automatic RTC correction
- touch-based settings
- screen brightness control
- automatic night dimming
- persistent configuration using ESP32 NVS
- OTA firmware updates
- environmental sensors
- temperature/humidity/air-quality display

Do NOT implement those features yet.

---

# Hardware

## Main Board

ESP32-2432S028R / CYD

Integrated hardware includes:

- ESP32
- 320x240 2.8-inch TFT display
- resistive touchscreen
- backlight
- microSD
- RGB LED
- light sensor
- speaker circuitry

Development environment:

- VS Code
- PlatformIO
- Arduino framework for ESP32
- CLI-first build and upload workflow

The repository directory should be:

```text
chrono-node/
```

The project must build using:

```bash
pio run
```

and flash using:

```bash
pio run -t upload
```

Serial debugging should work using:

```bash
pio device monitor
```

---

# External Connector Pinout

The CYD board exposes two small 4-pin connectors near the bottom edge.

For Chrono Node, use the LEFT connector, labeled on some diagrams as the temperature/humidity interface.

Its pins are:

```text
LEFT 4-PIN CONNECTOR

Pin 1: 3.3V
Pin 2: GPIO27
Pin 3: GPIO22
Pin 4: GND
```

For this project, configure those pins as:

```text
3.3V    -> DS3231 VCC
GPIO27  -> I2C SDA
GPIO22  -> I2C SCL
GND     -> DS3231 GND
```

Complete wiring:

```text
CYD LEFT CONNECTOR           DS3231

3.3V   --------------------> VCC
GPIO27 --------------------> SDA
GPIO22 --------------------> SCL
GND    --------------------> GND
```

Initialize I2C explicitly:

```cpp
Wire.begin(27, 22);
```

where:

```text
SDA = GPIO27
SCL = GPIO22
```

Do NOT use:

```cpp
Wire.begin();
```

without explicit pins.

Do NOT assume the ESP32 default I2C pins.

---

# Important GPIO Matrix Note

ESP32 supports flexible digital peripheral routing through its GPIO Matrix.

This means I2C is not restricted to the conventional GPIO21/GPIO22 combination.

GPIO27 can be assigned as SDA, while GPIO22 can be assigned as SCL.

Therefore this configuration is valid:

```cpp
Wire.begin(27, 22);
```

The fact that GPIO27 may be labeled with another alternate peripheral function on a board pinout does not prevent it from being used as I2C SDA.

The firmware should explicitly configure the I2C bus rather than relying on default pin assignments.

---

# Do Not Use the Right Connector for the RTC

The RIGHT connector may be labeled "Extended IO (I2C)" and typically exposes:

```text
GPIO21
GPIO22
GPIO35
GND
```

It may show:

```text
GPIO21 = SDA
GPIO22 = SCL
```

However, this connector does NOT provide a 3.3V power pin.

Therefore it is less convenient for the DS3231 module because an additional power connection would be required.

For Chrono Node, prefer the LEFT connector because it provides:

```text
3.3V
GPIO27
GPIO22
GND
```

which gives both power and I2C on a single 4-wire cable.

---

# GPIO Usage Constraints

Do not treat every ESP32 pin as freely available.

The CYD board already uses many GPIOs internally for:

- TFT display
- touchscreen
- backlight
- microSD
- RGB LED
- speaker
- onboard light sensor

Avoid changing board pin assignments unrelated to this project.

For the external RTC, use only:

```text
GPIO27 = SDA
GPIO22 = SCL
```

unless there is a documented hardware reason not to.

---

# External RTC

The RTC module is based on the DS3231.

Use I2C communication.

The DS3231 normally uses I2C address:

```text
0x68
```

The RTC has a backup battery and is expected to maintain time while the ESP32 is completely powered off.

---

# Software Architecture

Even though the first implementation is simple, avoid putting all functionality into one large `main.cpp`.

Prefer a structure that can grow cleanly:

```text
chrono-node/
├── platformio.ini
├── src/
│   ├── main.cpp
│   ├── rtc_manager.cpp
│   ├── rtc_manager.h
│   ├── display_manager.cpp
│   └── display_manager.h
├── include/
└── README.md
```

Exact naming may be changed if there is a good reason.

Keep hardware-specific code isolated where practical.

Do not over-engineer the first version with unnecessary abstractions, dependency injection, RTOS tasks, or complex state machines.

The goal is simple, readable embedded code that can evolve later.

---

# First Milestone

Implement ONLY the first hardware bring-up milestone.

## 1. Initialize the TFT Display

Configure the integrated CYD TFT display correctly.

Determine and use an appropriate display library supported by the ESP32-2432S028R.

Prefer a commonly maintained library such as TFT_eSPI or LovyanGFX if appropriate.

If board-specific configuration is required, keep that configuration inside the repository so that a fresh checkout can be compiled without manually modifying files inside the global PlatformIO package directory.

The build must be reproducible from the repository.

---

## 2. Initialize I2C

Initialize the I2C bus explicitly:

```cpp
Wire.begin(27, 22);
```

The mapping is:

```text
GPIO27 = SDA
GPIO22 = SCL
```

Do not use GPIO21 for SDA.

Do not use the default `Wire.begin()` configuration.

---

## 3. Detect the DS3231

Initialize the DS3231 and confirm that communication works.

At startup, log useful information to Serial.

Example:

```text
[Chrono Node] Booting
[DISPLAY] Initialized
[I2C] SDA=GPIO27 SCL=GPIO22
[RTC] DS3231 detected at 0x68
[RTC] 14:32:51 2026-11-08
```

If the RTC cannot be detected, report a clear error:

```text
[RTC] ERROR: DS3231 not detected
```

Do not crash or endlessly reboot when the RTC is missing.

The display should show an appropriate RTC error message instead.

---

## 4. Read the Current Time from DS3231

Read:

- hour
- minute
- second
- day
- month
- year

Do not use Wi-Fi.

Do not use NTP.

Do not derive the displayed time from the ESP32 system clock.

For this milestone, the displayed time must come directly from the DS3231.

---

# Initial UI

Keep the UI intentionally simple.

Use a dark/black background and display the time prominently in the center.

Example:

```text
        14:32:51

        08/11/2026
```

Requirements:

- Time should be the dominant element.
- Show hours, minutes, and seconds.
- Use 24-hour format.
- Display the date below the time.
- Avoid unnecessary graphics.
- Avoid animations.
- Avoid touch controls for now.

The first goal is functionality, not visual polish.

---

# Display Update Behavior

Update the displayed time once per second.

Avoid clearing and redrawing the entire screen every frame if doing so causes visible flickering.

Prefer updating only the regions containing changing values, or use another simple flicker-free technique supported by the selected display library.

Do not make the main loop unnecessarily busy.

A simple timing strategy using `millis()` is sufficient.

Avoid long blocking delays where practical.

---

# RTC Initialization Safety

Do NOT automatically overwrite the DS3231 time every time the ESP32 boots.

The RTC may already contain the correct time.

If the RTC reports that it lost power or contains invalid time, log the condition clearly.

For the first milestone, it is acceptable to provide a temporary/manual mechanism in the source code for setting the RTC time during development.

However:

- it must be clearly documented;
- it must not silently reset the RTC on every reboot;
- normal operation must simply read the existing RTC value.

---

# Serial Diagnostics

Use Serial at 115200 baud.

Provide concise logs useful for debugging through PlatformIO CLI.

At minimum report:

- Chrono Node firmware startup
- display initialization result
- I2C initialization
- exact I2C pin assignment
- RTC detection result
- RTC power-loss/invalid-time state if available
- current RTC date/time after initialization

Do not print excessively.

Once-per-second logging is acceptable during early development, but keep it easy to disable later.

---

# PlatformIO

Provide a complete `platformio.ini`.

Declare required libraries through `lib_deps` whenever possible.

The repository should contain everything required to reproduce the build except dependencies automatically downloaded by PlatformIO.

After implementation, run:

```bash
pio run
```

and resolve all compilation errors.

If hardware is connected and the serial port is available, identify it using:

```bash
pio device list
```

Do not assume a serial device path without checking.

---

# Scope Restrictions

For this milestone, DO NOT implement:

- Wi-Fi
- NTP
- timezone configuration
- web server
- captive portal
- OTA
- environmental sensors
- touch UI
- menus
- alarms
- weather
- Home Assistant
- MQTT
- cloud services
- automatic brightness
- SD card functionality

These belong to later milestones.

---

# Expected Result

After flashing the firmware:

1. Chrono Node boots.
2. The TFT initializes.
3. The I2C bus initializes using GPIO27 as SDA and GPIO22 as SCL.
4. The DS3231 is detected at I2C address 0x68.
5. The current time is read from the DS3231.
6. The TFT displays the current time in large text.
7. The current date appears underneath.
8. The seconds update once per second.
9. Power cycling the ESP32 does not reset the clock because the DS3231 keeps running from its backup battery.
10. If the RTC is unavailable, the firmware remains operational and displays a useful error instead of crashing.

Start by inspecting the existing repository, if any, and then implement this first milestone.

Keep the changes focused, simple, reproducible, and easy to extend in future Chrono Node milestones.
