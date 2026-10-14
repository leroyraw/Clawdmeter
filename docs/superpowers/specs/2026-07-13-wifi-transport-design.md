# WiFi transport (replace BLE) — design

Date: 2026-07-13
Status: Approved shape, pending spec review

## Motivation

The board currently talks to the host daemon over Bluetooth LE: the daemon polls
the Anthropic API and writes a small JSON payload to a custom GATT service, and
the board also presents a BLE HID keyboard so buttons can send keystrokes.

BLE pairing on macOS is the persistent pain point: one-sided bond clears leave
stale OS pairings, the daemon must run in "system-peripheral-only" mode (it only
attaches to a device the OS already holds), and launchd-spawned Python has
CoreBluetooth permission issues. The user does **not** use the HID keyboard
feature, so the keyboard is the only thing that genuinely needs Bluetooth.

**Goal:** replace the BLE transport with WiFi so there is no pairing, ever.

## Goals / non-goals

**Goals**
- Board joins the user's WiFi and receives usage data over the LAN.
- First-run provisioning via a SoftAP captive portal (no keyboard, no reflash to
  change networks, no secrets in the repo).
- Daemon keeps its Anthropic-API polling untouched; only the delivery transport
  changes (BLE write → HTTP POST).
- Works on all four boards (ESP32-S3 216/18 and ESP32-C6 216/18).
- Removing BLE eliminates the macOS pairing pain and the CoreBluetooth
  permission crash entirely.

**Non-goals**
- No HID keyboard / keystroke injection (user doesn't use it; dropped).
- No hybrid BLE+WiFi. Bluetooth is removed.
- No cloud/relay: board and daemon are on the same LAN.
- No TLS/auth on the LAN endpoint in v1 (see Future work).

## Architecture overview

```
Anthropic API ──poll──> daemon (Mac, holds token)
                          │  builds same JSON payload as today
                          ▼
                 HTTP POST http://clawdmeter.local/usage
                          │
                          ▼
        board (ESP32) HTTP server ──> apply payload ──> UI screen
```

The daemon stays the active party (it holds the OAuth token and does the
polling). The board is a passive HTTP receiver discovered by mDNS, so no IP is
hardcoded and a new DHCP lease doesn't break anything.

## Firmware

All WiFi code is board-agnostic and lives in shared `firmware/src/`, mirroring
how `ble.cpp` lived there (WiFi is identical across ESP32-S3 and C6; it is not a
per-board HAL concern).

### New / changed modules
- `net.{h,cpp}` — station connect + reconnect, mDNS responder
  (`clawdmeter.local`), the HTTP server (`POST /usage`), and applying the parsed
  payload to the UI. Exposes `net_init()`, `net_tick()`, and a `net_state()`
  enum (`NET_PROVISIONING`, `NET_CONNECTING`, `NET_ONLINE`, `NET_NO_DATA`,
  `NET_ONLINE_FRESH`) consumed by the UI.
- `provisioning.{h,cpp}` — SoftAP `Clawdmeter-Setup` + `DNSServer` captive
  portal + a small `WebServer` form that captures SSID/password and saves them
  to NVS, then reboots into station mode. Kept separate from `net.cpp` so each
  file has one job.
- `usage_payload.{h,cpp}` — the JSON→`UsageData` parser extracted from
  `ble.cpp` so the HTTP handler and any test can reuse it. Same schema as today.
- Delete `ble.{h,cpp}`; remove `NimBLE*`, `HID_REPORT_MAP`, all bond/pairing and
  `ble_keyboard_*` code.

### Libraries (all in the Arduino-ESP32 core, no new vendored deps)
- `WiFi.h` (station + SoftAP), `ESPmDNS.h`, `WebServer.h`, `DNSServer.h`,
  `Preferences.h` (NVS, already used).

### Credential storage
- `Preferences` namespace `"wifi"`, keys `ssid` / `pass`.
- Boot logic: creds present → station mode; absent (or station connect fails
  repeatedly) → provisioning mode.

### Provisioning UX
1. No creds saved → board starts SoftAP `Clawdmeter-Setup` (open network) and a
   captive portal; the on-screen hint shows "Join WiFi 'Clawdmeter-Setup' to set
   up".
2. User joins from phone/Mac; captive portal auto-opens a form listing scanned
   SSIDs + a password field.
3. On submit, creds are saved to NVS and the board reboots into station mode.
4. **Re-provision:** holding the BOOT button ~3 s wipes saved creds and returns
   to the captive portal (replaces the old hold-to-pair gesture).

### HTTP endpoint
- `POST /usage`, body = the existing payload JSON
  (`{"s":<5h %>, "w":<7d %>, optional "c":1, "t":<epoch>, "tf":"..."}`).
- Returns `200` on parse+apply success, `400` on malformed JSON.
- Optional `GET /` returns a tiny status page (board name, uptime, last-update
  age) for debugging.

### UI
- The current "pairing hint" sub-screen becomes a WiFi hint: "Join
  Clawdmeter-Setup…" in provisioning, "Connecting to WiFi…" while associating,
  and the normal idle/usage screens once data arrives.
- `ui_update_ble_status(...)` is renamed/replaced by `ui_update_net_status(state,
  ip)`; `data.h` `UsageData` is unchanged.

### Buttons after BLE removal
- BOOT: short press → (nothing HID); **hold ~3 s → re-provision WiFi**.
- PWR: unchanged (splash → cycle animation; usage → cycle brightness).
- The `pwr_hold_powers_off` cap flag and pair gesture in `main.cpp` are removed
  along with BLE; brightness/animation handling stays.

### main.cpp
- Replace `ble_init/ble_tick/ble_get_state/ui_update_ble_status` with
  `net_init/net_tick/net_state/ui_update_net_status`. Remove `ble_keyboard_*`
  calls in the button block.

## Daemon (macOS, `daemon/claude_usage_daemon.py`)

- **Keep unchanged:** token discovery (multi config dir), API polling, payload
  construction (`s`/`w`/`c`/`t`/`tf`), chime/clock config.
- **Remove:** all `bleak`/CoreBluetooth code, bond cache
  (`~/.config/claude-usage-monitor/ble-address`), `retrieve_connected_macos`,
  system-peripheral logic, `discover_target`.
- **Add:** an HTTP delivery function — `POST http://clawdmeter.local/usage` with
  the JSON payload, short timeout (~3 s), and the same retry cadence the BLE
  path used (retry next `POLL_INTERVAL`; log "board not reachable" like the old
  "device not found").
- **Config:** optional `board_host` key in
  `~/.config/claude-usage-monitor/config` to override `clawdmeter.local` (static
  IP fallback if mDNS is unreliable on the user's LAN).
- **launchd:** plist unchanged except it no longer needs Bluetooth; the
  CoreBluetooth `POWERED_OFF` failure mode disappears.
- Dependency: use `aiohttp` (or stdlib `urllib` in a thread) instead of `bleak`.

## Data flow & cadence

Unchanged from today except transport: daemon wakes every `POLL_INTERVAL`
(60 s), polls the API, POSTs the payload. The board applies it immediately and
tracks freshness (`DATA_FRESH_MS`) exactly as now to pick usage vs idle screen.

## Error handling / edge cases

- **Wrong WiFi password / AP gone:** station connect retries a bounded number of
  times, then falls back to the captive portal so the user can re-enter creds.
- **WiFi drops at runtime:** board auto-reconnects (WiFi event handler); UI shows
  "Connecting…"; mDNS re-registers on reconnect.
- **Board offline / rebooting:** daemon POST times out → logged, retried next
  cycle (no crash), mirroring current "device not found".
- **mDNS resolution fails on the Mac:** daemon retries; user can set `board_host`
  to the board's IP as a fallback (IP shown on the board's status screen).
- **Malformed payload:** board returns `400`, keeps last good data on screen.
- **C6 (no PSRAM):** WiFi + WebServer buffers must fit in internal RAM; verify on
  hardware. Removing NimBLE frees significant RAM, so net should be a wash or
  better.

## Board coverage

All four boards have WiFi (S3 and C6). WiFi/HTTP/mDNS/provisioning are shared
code, so each board gets it for free. C6 RAM headroom is the one thing to verify
on hardware (screenshot command is unavailable on C6, so eyeball).

## Testing

- **Firmware build:** all four envs compile (`pio run -e ...`).
- **Provisioning:** flash 1.8 S3, confirm `Clawdmeter-Setup` AP + captive portal,
  enter creds, board reboots and joins WiFi (serial log shows IP).
- **Discovery:** `ping clawdmeter.local` from the Mac resolves.
- **Data path:** `curl -X POST http://clawdmeter.local/usage -d '{"s":42,"w":13}'`
  updates the screen; verify via serial + on-panel readout.
- **Daemon:** unit-test the POST delivery against a local mock HTTP server;
  integration test end-to-end on the Mac (launchd) with the real board.
- **Re-provision:** hold BOOT ~3 s → returns to captive portal.

## Migration / rollout

One-time per user: flash the new firmware, run the captive-portal setup once,
update the daemon (it will no longer need Bluetooth permission). Update
`README.md` and `CLAUDE.md`: replace the BLE/pairing sections with WiFi setup,
and note that the HID keyboard feature is removed. Old BLE bond cache file can be
deleted.

## Future work (out of scope for v1)

- Optional shared-secret header on `POST /usage` to reject stray LAN writers.
- HTTPS (likely overkill on a trusted LAN).
- OTA firmware update over WiFi (now that WiFi exists).
- Re-introducing keystrokes via daemon-side macOS key injection (CGEvent +
  Accessibility) if a user ever wants push-to-talk back — deliberately excluded
  here since the user does not use it.
