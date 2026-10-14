# Project context

ESP32-S3 / ESP32-C6 firmware for a desk-side Claude Code usage monitor. Each
supported board lives in its own `firmware/src/boards/<name>/` folder and is
selected via PlatformIO's `build_src_filter`. Adding a board means dropping in
a new folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Four ports today (two SoC families, two panel sizes):

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 portrait, XCA9554 IO expander). Build env: `waveshare_amoled_18`. **Two panel revisions are auto-detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`): original = SH8601 display + FT3168 touch (0x38); later = CO5300 display + CST816 touch (0x15). One binary drives both.
- `boards/waveshare_amoled_216_c6/` — Waveshare ESP32-C6-Touch-AMOLED-2.16 (SH8601, 480×480, CST9217 touch). Build env: `waveshare_amoled_216_c6`. ESP32-C6 SoC: single-core RISC-V, **no PSRAM**.
- `boards/waveshare_amoled_18_c6/` — Waveshare ESP32-C6-Touch-AMOLED-1.8 (368×448 portrait, SH8601, FT3168 touch, TCA9554 expander). Build env: `waveshare_amoled_18_c6`. Same panel as the S3 1.8 but on the C6 SoC. All subsystems (display, touch, BOOT + PWR buttons, battery, WiFi) verified on hardware.

**C6 ports have no PSRAM** — shared code gates on `BOARD_HAS_PSRAM` (absent on C6) to use `MALLOC_CAP_INTERNAL` for LVGL/splash buffers, and the `screenshot` serial command is disabled (`LV_USE_SNAPSHOT=0`), so UI changes on a C6 board must be eyeballed on hardware, not auto-captured.

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

**Fully standalone — no host daemon, no BLE.** The board joins your WiFi (captive-portal provisioning on first boot, SSID `Clawdmeter-Setup`), holds its own Claude OAuth login (PKCE flow via a tiny web page it serves at `http://clawdmeter.local`), and polls the Anthropic API directly every 60s. See "WiFi + on-device Claude login" below. This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (BOOT/left → wake; hold ~3s → wipe WiFi creds and re-provision), GPIO 18 (right → unused; was BLE HID mode-toggle, dropped when BLE was removed), AXP PKEY (middle → cycle screens; on splash → cycle animations)

### AMOLED-1.8 (newer port)
**Two hardware revisions ship under this name; the firmware probes I2C at boot and picks drivers automatically (`board_rev()`):**
- Display: **SH8601** (original) or **CO5300** (later rev) AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1). Both are `Arduino_OLED` subclasses held behind one base pointer in `display.cpp`. The CO5300's 368-wide active area starts at GRAM column 16, so it gets `CO5300_COL_OFFSET 16` to center; SH8601 needs none.
- Touch: **FT3168** @ 0x38 (original) or **CST816** @ 0x15 (later rev), via I2C (SDA=15, SCL=14, INT=21). Both expose the same FocalTech-style data layout at regs 0x02..0x06, so one inline reader in `touch.cpp` serves both — only the address differs. Avoids vendoring the GPLv3 `Arduino_DriveBus` library. Revision is detected by which touch address ACKs (CST816 present ⇒ CO5300 panel).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST (EXIO1), TP_RST (EXIO0), and reads the PWR button (EXIO4, active HIGH). **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button level verified empirically with the deleted `iox` serial debug command.
  - **EXIO2 has different meanings per panel revision (verified on hardware).** On the original SH8601/FT3168 (V1) board it's the **audio amp enable**. On the CO5300/CST820 (V2) board it instead **gates the CST820 touch controller's power rail** — driving it low cuts power to touch (the chip won't even ACK its I2C address, and a TP_RST pulse can't revive it). The V2 amp enable is direct GPIO 46 instead. So `sound.cpp::amp_enable()` is revision-gated (never touches EXIO2 on V2) and `touch.cpp` asserts EXIO2 high before probing. Symptom if you regress this: touch dead on V2, while display/AXP/IMU (all init before sound) work fine. The original bug was `sound_hal_init()` → `amp_enable(false)` pulling EXIO2 low between board_init and touch_hal_init.
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → wake; hold ~3s → wipe WiFi creds and re-provision), XCA9554 EXIO4 (PWR → cycle screens; on splash → cycle animations). **No third button** (GPIO 18 button doesn't exist on this board).

### AMOLED-1.8 (C6) — `waveshare_amoled_18_c6`
ESP32-C6 sibling of the S3 1.8: same 368×448 SH8601 panel + FocalTech touch, different SoC and GPIO map. **All pins/edges below verified on hardware via temporary GPIO/IRQ scans, since Waveshare's wiki publishes no pin table and the third-party BSP's numbers were partly wrong.**
- Display: **SH8601** AMOLED via QSPI (CS=5, SCLK=0, SDIO0..3=1..4, no MCU reset pin — internal POR; effective reset is the TCA9554 power-cycle). Stock `Arduino_SH8601` init (no vendor-register patch — that's only needed on the C6 2.16).
- Touch: **FT3168** (some units FT6146) @ I2C 0x38, INT=15. Same inline FocalTech reader as the S3 1.8 (regs 0x02..0x06); no reset pin (gated by TCA9554 touch power).
- I2C bus: SDA=8, SCL=7 (shared by TCA9554, AXP2101, FT3168, QMI8658, PCF85063 RTC, ES8311 codec).
- IO expander: **TCA9554 / PCA9554** @ 0x20 — here it gates **power**, not reset: **P4 = display power, P5 = touch power, P7 = audio amp**. `io_expander_init()` runs the documented power-on sequence (P4/P5 LOW → 200 ms → HIGH) and **MUST run before `display_hal_init()`** or the panel stays unpowered. Amp (P7) left off (no audio path).
- PMU: AXP2101 @ 0x34 (owned by `power.cpp`, not `board_init` — LCD isn't on an ALDO rail here).
- IMU: QMI8658 @ 0x6B (init'd for bus health, rotation disabled).
- Orientation: **fixed at 0°**, no rotation (no PSRAM headroom).
- Buttons: **GPIO 9** (BOOT → wake; hold ~3s → wipe WiFi creds and re-provision; active LOW — *not* the docs' GPIO 0/9 guess; confirmed by scan), **AXP2101 PKEY** (PWR → cycle screens; on splash → cycle animations). The PKEY **SHORT-press IRQ fires on release** — that's the edge `power.cpp` acts on. No secondary button.

## Architecture

```text
firmware/src/
  hal/                      — board-agnostic interfaces shared code calls into
    board_caps.h            — runtime BoardCaps struct (W, H, button_count, has_* flags)
    display_hal.h           — init / begin / set_brightness / draw_bitmap / tick / round_area
    touch_hal.h             — init / read(&x, &y, &pressed)
    input_hal.h             — init / is_held(PRIMARY|SECONDARY)
    power_hal.h             — init / tick / battery_pct / is_charging / pwr_pressed (edge)
    imu_hal.h               — init / tick / rotation_quadrant
  boards/
    waveshare_amoled_216/   — CO5300 + CST9220 + AXP PKEY + QMI8658 rotation
    waveshare_amoled_18/    — SH8601 + FT3168 + AXP + XCA9554 (PWR via EXIO4), no rotation
    waveshare_amoled_216_c6/— C6: SH8601 + CST9217 + AXP PKEY, no PSRAM
    waveshare_amoled_18_c6/ — C6: SH8601 + FT3168 + AXP PKEY + TCA9554 (gates power), no PSRAM
    template/               — copy this to bootstrap a new port
  main.cpp                  — setup() + loop(): HAL calls only, zero #ifdef BOARD_*
  ui.{h,cpp}                — screen UI (splash, usage). compute_layout() picks fonts/positions from board_caps() (responsive — current breakpoint: H >= 460 → large, else compact)
  splash.{h,cpp}            — 20×20 pixel-art engine. CELL = min(W,H)/20, centered.
  net.{h,cpp}               — WiFi connect/reconnect state machine, mDNS (`clawdmeter.local`), tiny HTTP server (login page + status), on-device 60s Anthropic poll loop
  provisioning.{h,cpp}      — first-boot captive portal: SoftAP `Clawdmeter-Setup` + DNS spoof + WiFi-picker web form
  wifi_creds.{h,cpp}        — persist/clear saved WiFi SSID+password (NVS `Preferences`, namespace "wifi")
  claude_oauth.{h,cpp}      — Claude OAuth PKCE login/refresh/logout; tokens in NVS `Preferences` (namespace "claude")
  pkce.{h,cpp}              — PKCE code_verifier/code_challenge (S256) generation
  https.{h,cpp}             — minimal HTTPS POST/GET helper used by claude_oauth/claude_poll
  claude_poll.{h,cpp}       — calls the Anthropic API directly and parses usage out of response headers
  data.h                    — UsageData struct
  icons.h                   — icon arrays. Battery (5×) are RGB565A8 with alpha; rest are raw RGB565.
  logo.h                    — 80×80 RGB565 logo
  font_*.c                  — pre-compiled LVGL 9 bitmap fonts (Tiempos 56/34, Styrene 48/28/24/20/16/14/12, Mono 32/18)
  splash_animations.h       — generated, do not hand-edit
docs/porting/               — adding-a-board.md, hal-contract.md, capability-flags.md
```

Each board folder contains: `board.h` (pins, I2C addresses, `BOARD_HAS_*` flags),
`board_init.cpp` (Wire.begin + any IO expander), `display.cpp`, `touch.cpp`,
`input.cpp`, `power.cpp`, `imu.cpp`, `caps.cpp` (the `BoardCaps` instance), plus
any board-private hardware drivers (e.g. `io_expander.{h,cpp}` on AMOLED-1.8).
PlatformIO's `build_src_filter` includes shared code + one board's folder per env.

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (S3, default original)
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (S3)
pio run -d firmware -e waveshare_amoled_216_c6                                  # build 2.16 (C6)
pio run -d firmware -e waveshare_amoled_18_c6                                   # build 1.8 (C6)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101   # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0         # flash 2.16 on Linux
# C6 boards: same native USB-JTAG flashing; flag a chip mismatch ("This chip is ESP32-C6,
# not ESP32-S3") means you picked an S3 env — use a *_c6 env for C6 hardware.
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. Both expose the ESP32-S3 native USB-JTAG (no boot-mode dance needed).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

13 × 20×20 pixel-art creature animations sourced from
[claudepix.vercel.app](https://claudepix.vercel.app). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

Each animation has a per-animation 10-color RGB565 palette. Cell values 0..9 index it. Default boot screen.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Recent session highlights

- **Standalone board (2026-07-13).** Firmware now joins WiFi itself (captive-portal provisioning), logs into Claude directly via OAuth PKCE, and polls the Anthropic API on-device every 60s. BLE/NimBLE/HID and the host-side daemon (bash/Python/PowerShell under `daemon/`) are retired — `daemon/` is kept for git history but is no longer installed or used. `install.sh`/`install-mac.sh` now just point at the new flash + WiFi-provisioning + login flow instead of setting up a daemon.
- **Device-abstraction refactor (2026-05-18).** All board-conditional code moved out of shared files into `boards/<name>/` and behind a HAL in `hal/`. ~30 `#ifdef BOARD_*` blocks went to zero. UI is responsive via `compute_layout()` driven by `board_caps()`. New ports add a folder + a PlatformIO env — no shared file edits.
- Added second board port: Waveshare AMOLED-1.8 (368×448 portrait, SH8601, FT3168, XCA9554 IO expander).
- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.

## WiFi + on-device Claude login (no daemon, no host, no BLE)

The board used to need a host-side daemon talking BLE GATT; that whole transport
(NimBLE, HID keyboard, `ble.cpp`, the bash/Python/PowerShell daemons under
`daemon/`) has been removed from the firmware. The board is now fully
standalone:

1. **First boot / no saved WiFi**: `provisioning.cpp` brings up an open SoftAP
   `Clawdmeter-Setup` + a DNS-spoofing captive portal + a `WebServer` on port
   80. The user picks their home network and enters the password; `wifi_creds.cpp`
   persists it to NVS (`Preferences`, namespace `"wifi"`) and the board reboots.
2. **Normal boot**: `net.cpp`'s `net_init()`/`net_tick()` connect to the saved
   network (falling back into provisioning again on repeated failure), then
   call `MDNS.begin("clawdmeter")` so the device is reachable at
   `http://clawdmeter.local`.
3. **Claude login**: `net.cpp` serves a small HTML page at `/` — if not yet
   logged in, a "Log in to Claude" link opens `claude_oauth_authorize_url()`
   (PKCE, `pkce.cpp`, S256) against `claude.ai/oauth/authorize` in the user's
   browser; the user approves and pastes the resulting code back into a form
   (`POST /login`) which `claude_oauth.cpp` exchanges for tokens against
   `console.anthropic.com/v1/oauth/token`. Access/refresh tokens are stored in
   NVS (`Preferences`, namespace `"claude"`), refreshed automatically as they
   expire. `POST /logout` clears them.
4. **Usage polling**: once online + logged in, `main.cpp`'s `loop()` calls
   `claude_poll()` (via `https.cpp`) once every 60s (see the "On-device usage
   poll" block in `main.cpp`) and feeds the result straight into
   `apply_usage()` in `net.cpp` — the same path the old daemon's `/usage` POST
   handler used, so the UI update / usage-rate tracking / splash matching logic
   didn't need to change.

**Known risk:** this all rides on Anthropic's undocumented consumer OAuth
endpoints (`claude.ai/oauth/authorize`, `console.anthropic.com/v1/oauth/token`)
and could break if Anthropic changes them without notice. The board's login is
a completely independent token chain from any `claude` CLI login on your
computer — logging the board in or out never touches your machine's
`~/.claude/.credentials.json` or Keychain entry.
