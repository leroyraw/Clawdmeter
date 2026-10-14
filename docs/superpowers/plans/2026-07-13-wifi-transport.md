# WiFi Transport (replace BLE) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the board's BLE transport with WiFi — captive-portal provisioning, mDNS discovery, and daemon HTTP push of the existing usage payload — removing all Bluetooth/HID and the macOS pairing pain.

**Architecture:** The daemon keeps polling the Anthropic API and building the same JSON payload, but delivers it via `HTTP POST http://clawdmeter.local/usage` instead of a BLE GATT write. The board joins WiFi (creds captured once via a SoftAP captive portal, stored in NVS), advertises `clawdmeter.local` over mDNS, and runs a tiny HTTP server that applies the payload to the existing UI. NimBLE and the HID keyboard are deleted.

**Tech Stack:** ESP32 Arduino core (WiFi.h, ESPmDNS.h, WebServer.h, DNSServer.h, Preferences.h, ArduinoJson) on firmware; Python 3 + aiohttp on the daemon; PlatformIO build.

## Global Constraints

- Firmware is board-agnostic shared code in `firmware/src/`; **no `#ifdef BOARD_*`** — use `BoardCaps`/`BOARD_HAS_*` if a board difference is ever needed (WiFi is identical across S3 and C6, so none is expected).
- All four PlatformIO envs must build: `waveshare_amoled_216`, `waveshare_amoled_18`, `waveshare_amoled_216_c6`, `waveshare_amoled_18_c6`.
- C6 boards have **no PSRAM** and `LV_USE_SNAPSHOT=0` (no `screenshot` command) — verify C6 UI changes by eye.
- Usage payload JSON schema is fixed and shared with the daemon: `{"s":<int 5h %>, "w":<int 7d %>, "c":<0|1 optional>, "t":<int epoch optional>, "tf":<str optional>}`.
- Flash port on this Mac: `/dev/cu.usbmodem1101`. Serial baud 115200. Reader script: `~/.platformio/penv/bin/python <scratch>/read_serial.py /dev/cu.usbmodem1101 <seconds>` (pyserial only in PlatformIO's venv, not system python).
- Commit messages end with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Base branch: `fix/amoled18-cst820-and-buttons` (has the verified CST820 touch fix). Discard the uncommitted button/pairing-gesture changes first — the WiFi work removes the pairing path entirely, so they are superseded (KEEP the committed touch fix).

---

## Task 0: Establish clean base branch

**Files:**
- Modify: git working tree only

- [ ] **Step 1: Discard superseded button/pairing changes, keep the touch fix (already committed)**

```bash
cd /Users/leroyderouwe/www/Clawdmeter
git checkout -- firmware/src/hal/board_caps.h \
  firmware/src/boards/*/caps.cpp \
  firmware/src/main.cpp firmware/src/ui.cpp
git status --short   # expect: clean (touch fix + spec + this plan already committed)
```

- [ ] **Step 2: Create the feature branch off the current tip**

```bash
git checkout -b feature/wifi-transport
git log --oneline -3   # touch fix + spec commit present
```

- [ ] **Step 3: Baseline build to confirm the starting point compiles**

Run: `pio run -d firmware -e waveshare_amoled_18`
Expected: `SUCCESS`.

---

## Task 1: Usage payload parser (`usage_payload`)

Extract the JSON→`UsageData` parsing (currently inside `ble.cpp`) into a standalone module both the (soon-to-be) HTTP handler and future tests can call. This is the one piece of behavior we can pin down before the network exists.

**Files:**
- Create: `firmware/src/usage_payload.h`
- Create: `firmware/src/usage_payload.cpp`
- Reference (read the existing parse, then remove it in Task 5): `firmware/src/ble.cpp`
- Reference: `firmware/src/data.h` (the `UsageData` struct — do not change its fields)

**Interfaces:**
- Produces: `bool usage_payload_parse(const char* json, size_t len, UsageData& out);` — returns true on valid JSON with at least `s` and `w`; fills `out` (s, w, chime flag, clock epoch/format when present). Consumed by `net.cpp` (Task 4).

- [ ] **Step 1: Read the current parser to preserve exact field handling**

Run: `grep -nE "deserializeJson|doc\[|\\\"s\\\"|\\\"w\\\"|\\\"c\\\"|\\\"t\\\"|\\\"tf\\\"|UsageData" firmware/src/ble.cpp`
Note every field name and how it maps into `UsageData` (and how `t`/`tf`/`c` feed the clock/chime). Reproduce that mapping exactly below.

- [ ] **Step 2: Write the header**

```cpp
// firmware/src/usage_payload.h
#pragma once
#include <stddef.h>
#include "data.h"

// Parse the daemon's usage payload JSON into a UsageData.
// Schema: {"s":int, "w":int, "c":0|1?, "t":epoch?, "tf":"..."?}
// Returns true when the JSON is valid and contains at least "s" and "w".
bool usage_payload_parse(const char* json, size_t len, UsageData& out);
```

- [ ] **Step 3: Write the implementation (mirror the fields found in Step 1)**

```cpp
// firmware/src/usage_payload.cpp
#include "usage_payload.h"
#include <ArduinoJson.h>

bool usage_payload_parse(const char* json, size_t len, UsageData& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
    if (!doc["s"].is<int>() || !doc["w"].is<int>()) return false;

    out.session_pct = doc["s"].as<int>();   // rename to the real UsageData fields
    out.week_pct    = doc["w"].as<int>();    // from Step 1
    out.chime       = doc["c"].as<int>() == 1;          // if present
    if (doc["t"].is<long>())  out.clock_epoch = doc["t"].as<long>();
    if (doc["tf"].is<const char*>()) out.clock_fmt = doc["tf"].as<const char*>();
    return true;
}
```

> Adjust the assigned struct-member names to match `data.h` exactly (Step 1 tells you the real names). Do not invent fields.

- [ ] **Step 4: Add the two files to the shared build and compile**

`build_src_filter` in `firmware/platformio.ini` already includes `firmware/src/*` shared sources; confirm no per-file exclusion is needed. Then:

Run: `pio run -d firmware -e waveshare_amoled_18`
Expected: `SUCCESS` (file compiles; still unused).

- [ ] **Step 5: Commit**

```bash
git add firmware/src/usage_payload.h firmware/src/usage_payload.cpp
git commit -m "feat(fw): extract usage payload JSON parser into usage_payload

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: WiFi credential storage (`wifi_creds`)

**Files:**
- Create: `firmware/src/wifi_creds.h`
- Create: `firmware/src/wifi_creds.cpp`

**Interfaces:**
- Produces:
  - `bool wifi_creds_load(String& ssid, String& pass);` — true if creds saved.
  - `void wifi_creds_save(const String& ssid, const String& pass);`
  - `void wifi_creds_clear();`
  Consumed by `provisioning.cpp` and `net.cpp` (Tasks 3–4).

- [ ] **Step 1: Write the header**

```cpp
// firmware/src/wifi_creds.h
#pragma once
#include <Arduino.h>
bool wifi_creds_load(String& ssid, String& pass);
void wifi_creds_save(const String& ssid, const String& pass);
void wifi_creds_clear();
```

- [ ] **Step 2: Write the implementation (Preferences/NVS, namespace "wifi")**

```cpp
// firmware/src/wifi_creds.cpp
#include "wifi_creds.h"
#include <Preferences.h>

static const char* NS = "wifi";

bool wifi_creds_load(String& ssid, String& pass) {
    Preferences p; p.begin(NS, true);
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
    return ssid.length() > 0;
}

void wifi_creds_save(const String& ssid, const String& pass) {
    Preferences p; p.begin(NS, false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
}

void wifi_creds_clear() {
    Preferences p; p.begin(NS, false);
    p.clear();
    p.end();
}
```

- [ ] **Step 3: Build**

Run: `pio run -d firmware -e waveshare_amoled_18`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/wifi_creds.h firmware/src/wifi_creds.cpp
git commit -m "feat(fw): NVS-backed WiFi credential storage

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Captive-portal provisioning (`provisioning`)

SoftAP `Clawdmeter-Setup` + a DNS catch-all + a WebServer form that lists nearby SSIDs and saves the chosen one. Runs to completion (blocking loop) then reboots into station mode.

**Files:**
- Create: `firmware/src/provisioning.h`
- Create: `firmware/src/provisioning.cpp`

**Interfaces:**
- Consumes: `wifi_creds_save()` (Task 2).
- Produces: `void provisioning_run();` — never returns; ends by calling `ESP.restart()` after creds are saved. Consumed by `net.cpp` (Task 4) and optionally `main.cpp` re-provision.
  Also: `const char* PROVISION_AP_SSID;` (`"Clawdmeter-Setup"`).

- [ ] **Step 1: Write the header**

```cpp
// firmware/src/provisioning.h
#pragma once
extern const char* PROVISION_AP_SSID;   // "Clawdmeter-Setup"
void provisioning_run();                 // starts SoftAP + portal; restarts on save
```

- [ ] **Step 2: Write the implementation**

```cpp
// firmware/src/provisioning.cpp
#include "provisioning.h"
#include "wifi_creds.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

const char* PROVISION_AP_SSID = "Clawdmeter-Setup";

static WebServer  server(80);
static DNSServer  dns;

static String page() {
    String opts;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        opts += "<option value='" + s + "'>" + s + "</option>";
    }
    return String("<!doctype html><meta name=viewport content='width=device-width'>"
        "<h2>Clawdmeter WiFi setup</h2>"
        "<form method=POST action=/save>"
        "<p>Network:<br><select name=ssid>") + opts + "</select>"
        "<p>Password:<br><input name=pass type=password>"
        "<p><button type=submit>Save &amp; connect</button></form>";
}

void provisioning_run() {
    WiFi.mode(WIFI_AP_STA);          // STA active so scanNetworks() works
    WiFi.softAP(PROVISION_AP_SSID);  // open network
    IPAddress ip = WiFi.softAPIP();
    dns.start(53, "*", ip);          // captive: resolve every host to us

    server.onNotFound([]() {         // captive-portal redirect
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString());
        server.send(302, "text/plain", "");
    });
    server.on("/", []() { server.send(200, "text/html", page()); });
    server.on("/save", HTTP_POST, []() {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");
        if (ssid.length()) {
            wifi_creds_save(ssid, pass);
            server.send(200, "text/html",
                        "<h3>Saved. Rebooting to connect&hellip;</h3>");
            delay(1500);
            ESP.restart();
        } else {
            server.send(400, "text/html", "<h3>Pick a network.</h3>");
        }
    });
    server.begin();
    Serial.printf("Provisioning: join WiFi '%s' then open http://%s\n",
                  PROVISION_AP_SSID, ip.toString().c_str());

    for (;;) { dns.processNextRequest(); server.handleClient(); delay(2); }
}
```

- [ ] **Step 3: Build**

Run: `pio run -d firmware -e waveshare_amoled_18`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/provisioning.h firmware/src/provisioning.cpp
git commit -m "feat(fw): SoftAP captive-portal WiFi provisioning

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Network module (`net`) — station, mDNS, HTTP receiver

Station connect/reconnect, `clawdmeter.local` mDNS, and a `POST /usage` HTTP endpoint that parses via `usage_payload_parse` and hands data to the UI. Exposes a state enum for the UI.

**Files:**
- Create: `firmware/src/net.h`
- Create: `firmware/src/net.cpp`
- Reference: `firmware/src/data.h`, `firmware/src/ui.h` (whatever call applies fresh usage — see Step 1)

**Interfaces:**
- Consumes: `wifi_creds_load()` (T2), `provisioning_run()` (T3), `usage_payload_parse()` (T1), the UI's "apply usage data" entry point (find in Step 1).
- Produces:
  - `enum NetState { NET_PROVISIONING, NET_CONNECTING, NET_ONLINE_NODATA, NET_ONLINE_FRESH };`
  - `void net_init();` — loads creds → provisioning or station+mDNS+HTTP.
  - `void net_tick();` — pump HTTP server, handle reconnect, expire freshness.
  - `NetState net_state();`
  - `IPAddress net_ip();`
  Consumed by `main.cpp` (T5) and `ui.cpp` (T5).

- [ ] **Step 1: Find how usage data currently reaches the UI**

Run: `grep -nE "UsageData|ui_update_usage|ui_show|apply.*usage|has_received_data" firmware/src/ble.cpp firmware/src/ui.h`
Record the exact function the BLE write path calls to push a `UsageData` into the UI (e.g. `ui_update_usage(const UsageData&)`). Call that same function from the HTTP handler below. Do not create a parallel UI path.

- [ ] **Step 2: Write the header**

```cpp
// firmware/src/net.h
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

enum NetState { NET_PROVISIONING, NET_CONNECTING, NET_ONLINE_NODATA, NET_ONLINE_FRESH };

void      net_init();
void      net_tick();
NetState  net_state();
IPAddress net_ip();
```

- [ ] **Step 3: Write the implementation**

```cpp
// firmware/src/net.cpp
#include "net.h"
#include "wifi_creds.h"
#include "provisioning.h"
#include "usage_payload.h"
#include "data.h"
#include "ui.h"                 // for the UI apply-usage call found in Step 1
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

static WebServer server(80);
static NetState  state = NET_CONNECTING;
static uint32_t  last_data_ms = 0;
static const uint32_t DATA_FRESH_MS = 15UL * 60UL * 1000UL;  // match ui.cpp

static void handle_usage() {
    String body = server.arg("plain");
    UsageData d{};
    if (usage_payload_parse(body.c_str(), body.length(), d)) {
        ui_update_usage(d);          // <-- exact call from Step 1
        last_data_ms = millis();
        state = NET_ONLINE_FRESH;
        server.send(200, "text/plain", "ok");
    } else {
        server.send(400, "text/plain", "bad json");
    }
}

void net_init() {
    String ssid, pass;
    if (!wifi_creds_load(ssid, pass)) {
        state = NET_PROVISIONING;
        provisioning_run();          // never returns (reboots on save)
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    state = NET_CONNECTING;
    Serial.printf("WiFi: connecting to '%s'\n", ssid.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(200);
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi: connect failed, re-provisioning");
        provisioning_run();          // bad creds → back to portal
    }

    Serial.printf("WiFi: online, IP=%s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("clawdmeter")) { MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: clawdmeter.local"); }
    server.on("/usage", HTTP_POST, handle_usage);
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/plain",
            String("Clawdmeter online, IP=") + WiFi.localIP().toString());
    });
    server.begin();
    state = NET_ONLINE_NODATA;
}

void net_tick() {
    if (state == NET_PROVISIONING) return;
    if (WiFi.status() != WL_CONNECTED) { state = NET_CONNECTING; WiFi.reconnect(); return; }
    server.handleClient();
    if (state == NET_CONNECTING) state = NET_ONLINE_NODATA;
    if (state == NET_ONLINE_FRESH && millis() - last_data_ms > DATA_FRESH_MS)
        state = NET_ONLINE_NODATA;
}

NetState  net_state() { return state; }
IPAddress net_ip()    { return WiFi.localIP(); }
```

> Match `DATA_FRESH_MS` to the freshness constant already in `ui.cpp` (Step 1 shows it). Use the real UI apply-usage function name.

- [ ] **Step 4: Build**

Run: `pio run -d firmware -e waveshare_amoled_18`
Expected: `SUCCESS`.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/net.h firmware/src/net.cpp
git commit -m "feat(fw): WiFi station + mDNS + HTTP /usage receiver

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Wire net into main/UI, delete BLE, fix build deps

Swap the BLE calls in `main.cpp` for `net_*`, replace BLE status in the UI with WiFi status, delete `ble.{h,cpp}`, and drop the NimBLE dependency. This is the task that makes the board actually run on WiFi.

**Files:**
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/src/ui.h`, `firmware/src/ui.cpp`
- Delete: `firmware/src/ble.h`, `firmware/src/ble.cpp`
- Modify: `firmware/platformio.ini`
- Modify: `firmware/src/hal/board_caps.h` + all `caps.cpp` only if `pwr_hold_powers_off` was ever committed (it was not — skip).

**Interfaces:**
- Consumes: `net_init/net_tick/net_state/net_ip` (T4).
- Produces: `void ui_update_net_status(NetState state, IPAddress ip);` (replaces `ui_update_ble_status`).

- [ ] **Step 1: Inventory every BLE reference to replace**

Run: `grep -rnE "ble_|ui_update_ble_status|NimBLE|ble\\.h" firmware/src/main.cpp firmware/src/ui.cpp firmware/src/ui.h`
You will replace: `ble_init()`→`net_init()`, `ble_tick()`→`net_tick()`, `ble_get_state()`→`net_state()`, `ui_update_ble_status(...)`→`ui_update_net_status(...)`, and delete both `ble_keyboard_press/release` call sites.

- [ ] **Step 2: main.cpp — includes + setup + loop**

Replace `#include "ble.h"` with `#include "net.h"`. In `setup()`, replace `ble_init();` with `net_init();`. In `loop()`, replace `ble_tick();` with `net_tick();`. Replace the BLE-state→UI block:

```cpp
    static NetState last_net = (NetState)-1;
    NetState ns = net_state();
    if (ns != last_net) { last_net = ns; ui_update_net_status(ns, net_ip()); }
```

- [ ] **Step 3: main.cpp — buttons: drop HID, add re-provision on BOOT hold**

In the PRIMARY (BOOT) button block, delete the `ble_keyboard_press(0x2C,0)` / `ble_keyboard_release()` calls. Delete the entire SECONDARY-button `ble_keyboard_press(0x2B,0x02)` block. Delete `pair_tick()` and its state machine and its call. Add a BOOT long-hold re-provision using the existing primary-held tracking:

```cpp
    // BOOT held ~3s → wipe WiFi creds and reboot into the captive portal.
    {
        static uint32_t primary_down_ms = 0;
        bool held = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (held && primary_down_ms == 0) primary_down_ms = millis();
        if (!held) primary_down_ms = 0;
        if (primary_down_ms && millis() - primary_down_ms > 3000) {
            Serial.println("BOOT long-press: clearing WiFi creds, re-provisioning");
            wifi_creds_clear();
            ESP.restart();
        }
    }
```

Add `#include "wifi_creds.h"` to main.cpp. Remove the now-unused primary/secondary HID edge-tracking statics.

- [ ] **Step 4: ui.h/ui.cpp — replace BLE status with net status**

In `ui.h`, replace the `ui_update_ble_status(...)` declaration with:

```cpp
#include "net.h"
void ui_update_net_status(NetState state, IPAddress ip);
```

In `ui.cpp`, rename the function and repurpose the "pair hint" text by state:

```cpp
void ui_update_net_status(NetState state, IPAddress ip) {
    switch (state) {
        case NET_PROVISIONING:
            lv_label_set_text(pair_line1, "WiFi setup");
            lv_label_set_text(pair_line2, "join 'Clawdmeter-Setup'");
            break;
        case NET_CONNECTING:
            lv_label_set_text(pair_line1, "Connecting");
            lv_label_set_text(pair_line2, "to WiFi…");
            break;
        default: /* online: hint hidden by the pick-view logic */ break;
    }
    // Reuse the existing connected/disconnected view selection: treat
    // NET_ONLINE_* as "connected" and the rest as "show the hint".
    ui_pick_view(/*connected=*/ state == NET_ONLINE_FRESH || state == NET_ONLINE_NODATA);
}
```

> Use the real label object names and view-selection function from `ui.cpp` (the ones the old `ui_update_ble_status` used — Step 1 of Task 4 found them). Keep `data.h`/`UsageData` unchanged.

- [ ] **Step 5: Delete BLE sources and drop the NimBLE dependency**

```bash
git rm firmware/src/ble.h firmware/src/ble.cpp
```

In `firmware/platformio.ini`, remove the NimBLE line from `lib_deps` (e.g. `h2zero/NimBLE-Arduino@...`) in every env that lists it. Leave `bblanchon/ArduinoJson` (still used by `usage_payload`). WiFi/WebServer/ESPmDNS/DNSServer are in the Arduino core — no `lib_deps` entry needed.

- [ ] **Step 6: Build ALL four envs**

```bash
for e in waveshare_amoled_216 waveshare_amoled_18 waveshare_amoled_216_c6 waveshare_amoled_18_c6; do
  echo "== $e =="; pio run -d firmware -e $e 2>&1 | tail -1; done
```
Expected: four `SUCCESS` lines. Fix any missed `ble_*` reference the compiler flags.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(fw): switch transport BLE->WiFi; remove NimBLE/HID; BOOT re-provision

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Hardware verification (1.8 S3)

The first end-to-end firmware check on real hardware. No code beyond fixups.

**Files:** none (verify/fix loop).

- [ ] **Step 1: Flash the 1.8 S3**

Run: `pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem1101`
Expected: `SUCCESS`.

- [ ] **Step 2: Confirm provisioning on first boot**

Read serial: `~/.platformio/penv/bin/python <scratch>/read_serial.py /dev/cu.usbmodem1101 10`
Expected: line `Provisioning: join WiFi 'Clawdmeter-Setup' then open http://192.168.4.1`.
From a phone/Mac, join `Clawdmeter-Setup`, complete the form with real WiFi creds. Board reboots.

- [ ] **Step 3: Confirm station + mDNS**

Read serial for ~15s after reboot. Expected: `WiFi: online, IP=...` and `mDNS: clawdmeter.local`.
From the Mac: `ping -c1 clawdmeter.local` resolves and replies.

- [ ] **Step 4: Confirm the data path updates the screen**

```bash
curl -sS -X POST http://clawdmeter.local/usage \
  -H 'Content-Type: application/json' -d '{"s":42,"w":13}'
```
Expected: `ok`; the panel shows the usage view with 42% / 13%.

- [ ] **Step 5: Confirm re-provision**

Hold BOOT ~3s. Serial: `BOOT long-press: clearing WiFi creds, re-provisioning`; board returns to `Clawdmeter-Setup`. Re-enter creds to restore.

- [ ] **Step 6: Commit any fixups made during verification**

```bash
git add -A && git commit -m "fix(fw): WiFi transport hardware-verification fixups

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"   # skip if nothing changed
```

---

## Task 7: Daemon — HTTP delivery function (TDD)

Add the delivery function that POSTs a payload to the board. Real TDD against a local mock HTTP server.

**Files:**
- Create: `daemon/http_delivery.py`
- Create: `daemon/tests/test_http_delivery.py`

**Interfaces:**
- Produces: `async def deliver(payload: dict, host: str = "clawdmeter.local", timeout: float = 3.0) -> bool` — POSTs `payload` as JSON to `http://{host}/usage`; returns True on HTTP 200, False on any error/timeout/non-200. Consumed by the daemon main loop (Task 8).

- [ ] **Step 1: Write the failing test (mock aiohttp server)**

```python
# daemon/tests/test_http_delivery.py
import asyncio, json, pytest
from aiohttp import web
from daemon.http_delivery import deliver

@pytest.fixture
async def server(aiohttp_server):
    seen = {}
    async def handler(request):
        seen["body"] = await request.json()
        return web.Response(text="ok")
    app = web.Application(); app.router.add_post("/usage", handler)
    srv = await aiohttp_server(app)
    return srv, seen

async def test_deliver_posts_payload_and_returns_true(server):
    srv, seen = server
    ok = await deliver({"s": 42, "w": 13}, host=f"{srv.host}:{srv.port}")
    assert ok is True
    assert seen["body"] == {"s": 42, "w": 13}

async def test_deliver_returns_false_when_unreachable():
    ok = await deliver({"s": 1, "w": 1}, host="127.0.0.1:1", timeout=0.5)
    assert ok is False
```

- [ ] **Step 2: Run the test, expect failure**

Run: `cd daemon && python -m pytest tests/test_http_delivery.py -v`
Expected: FAIL (`ModuleNotFoundError: daemon.http_delivery` or import error). Ensure `aiohttp` and `pytest-aiohttp` are installed in the daemon venv first: `daemon/.venv/bin/pip install aiohttp pytest pytest-aiohttp`.

- [ ] **Step 3: Implement `deliver`**

```python
# daemon/http_delivery.py
import aiohttp

async def deliver(payload: dict, host: str = "clawdmeter.local",
                  timeout: float = 3.0) -> bool:
    url = f"http://{host}/usage"
    try:
        to = aiohttp.ClientTimeout(total=timeout)
        async with aiohttp.ClientSession(timeout=to) as s:
            async with s.post(url, json=payload) as resp:
                return resp.status == 200
    except Exception:
        return False
```

- [ ] **Step 4: Run the test, expect pass**

Run: `cd daemon && python -m pytest tests/test_http_delivery.py -v`
Expected: 2 passed.

- [ ] **Step 5: Commit**

```bash
git add daemon/http_delivery.py daemon/tests/test_http_delivery.py
git commit -m "feat(daemon): HTTP delivery of usage payload to the board

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Daemon — replace BLE with HTTP delivery

Rip out bleak/CoreBluetooth and route the poll loop through `deliver`. Keep API polling, token discovery, chime/clock config untouched.

**Files:**
- Modify: `daemon/claude_usage_daemon.py`
- Modify: `daemon/requirements.txt` (or wherever deps are pinned)
- Reference: `daemon/tests/*` (delete BLE-only tests that no longer apply)

**Interfaces:**
- Consumes: `deliver()` (Task 7).

- [ ] **Step 1: Identify the BLE surface to remove**

Run: `grep -nE "bleak|Bleak|CoreBluetooth|CentralManager|retrieve_connected|discover_target|SAVED_ADDR_FILE|write_payload|system.peripheral" daemon/claude_usage_daemon.py`
Everything matched (scanning, bond cache, GATT writer class, system-peripheral loop) is deleted or replaced in the next steps.

- [ ] **Step 2: Replace the main loop's delivery with `deliver`**

At the top: `from http_delivery import deliver`. Read `board_host` from config (default `clawdmeter.local`):

```python
def read_board_host() -> str:
    try:
        for line in CONFIG_FILE.read_text().splitlines():
            k, _, v = line.partition("=")
            if k.strip().lower() == "board_host":
                return v.strip()
    except FileNotFoundError:
        pass
    return "clawdmeter.local"
```

Replace the BLE connect/discover/write body of `main()` with a plain loop:

```python
async def main() -> None:
    host = read_board_host()
    while True:
        payload = await poll_active_payload()
        if payload is not None:
            ok = await deliver(payload, host=host)
            log(f"posted to {host}: {'ok' if ok else 'unreachable'}")
        else:
            log("no payload (token/plan issue); retrying")
        await asyncio.sleep(POLL_INTERVAL)
```

Delete: the `Writer`/GATT class, `retrieve_connected_macos`, `discover_target`, `_get_cb_manager`, `load_cached_address`, `SAVED_ADDR_FILE`, and all `bleak`/`objc`/CoreBluetooth imports.

- [ ] **Step 3: Update dependencies**

In `daemon/requirements.txt`: remove `bleak` (and any `pyobjc*` only used for CoreBluetooth); add `aiohttp`. Then:

Run: `daemon/.venv/bin/pip install -r daemon/requirements.txt`
Expected: installs `aiohttp`, no `bleak`.

- [ ] **Step 4: Prune dead BLE tests**

Run: `grep -rlE "bleak|CoreBluetooth|retrieve_connected|ble.address" daemon/tests`
Delete or rewrite each hit so the suite reflects the HTTP transport. Keep token/plan/config tests.

Run: `cd daemon && python -m pytest -q`
Expected: green (http_delivery + surviving token/config tests).

- [ ] **Step 5: End-to-end on the Mac**

With the board online (Task 6) and a valid Claude token present:

Run: `daemon/.venv/bin/python daemon/claude_usage_daemon.py` (foreground)
Expected: log `posted to clawdmeter.local: ok` within one `POLL_INTERVAL`; the panel shows real usage. Ctrl-C.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(daemon): replace BLE transport with HTTP POST to the board

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: launchd, installers, and docs

Make a fresh install work WiFi-only and document the new setup.

**Files:**
- Modify: `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist` (via installer) — no Bluetooth needed now.
- Modify: `install.sh`, `install-mac.sh`
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Installers — drop BLE, add board_host prompt**

Run: `grep -nE "bleak|ble-address|bluetooth|system.peripheral|pair" install.sh install-mac.sh`
Remove BLE setup steps. In the interactive config, add an optional `board_host` prompt (default `clawdmeter.local`) written to `~/.config/claude-usage-monitor/config`. Ensure the venv install line uses the updated `requirements.txt`.

- [ ] **Step 2: Restart the daemon under launchd and confirm**

```bash
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
launchctl load  ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
sleep 65; tail -5 ~/Library/Logs/claude-usage-daemon.out.log
```
Expected: `posted to clawdmeter.local: ok`. No CoreBluetooth `POWERED_OFF` errors in `...err.log`.

- [ ] **Step 3: Docs — README + CLAUDE.md**

In `README.md`: replace the BLE/pairing setup section with: flash firmware → join `Clawdmeter-Setup` → enter WiFi creds → daemon auto-delivers over `clawdmeter.local`. Note the HID keyboard feature is removed.
In `CLAUDE.md`: update the "Daemon / host side" section (BLE → WiFi/HTTP, mDNS discovery, `board_host` override) and the board descriptions (remove BLE/HID, add WiFi transport + captive portal + `POST /usage`).

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore: WiFi-only installers, launchd, and docs

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: C6 hardware sanity + finish

**Files:** none (verify), then open PR.

- [ ] **Step 1: Flash a C6 board and confirm WiFi fits in RAM**

If a C6 board is available: `pio run -d firmware -e waveshare_amoled_18_c6 -t upload --upload-port /dev/cu.usbmodem1101`, then repeat Task 6 Steps 2–4. Expected: provisioning, `WiFi: online`, `curl` updates the panel. Watch serial for allocation failures (no PSRAM). If none available, note "C6 unverified" in the PR.

- [ ] **Step 2: Full build matrix green**

```bash
for e in waveshare_amoled_216 waveshare_amoled_18 waveshare_amoled_216_c6 waveshare_amoled_18_c6; do
  echo "== $e =="; pio run -d firmware -e $e 2>&1 | tail -1; done
```
Expected: four `SUCCESS`.

- [ ] **Step 3: Open the PR**

```bash
git push -u origin feature/wifi-transport
gh pr create --title "WiFi transport (replace BLE)" \
  --body "Replaces the BLE transport with WiFi: captive-portal provisioning, mDNS (clawdmeter.local), daemon HTTP POST of the existing usage payload; removes NimBLE/HID and all pairing. Spec: docs/superpowers/specs/2026-07-13-wifi-transport-design.md. Hardware-verified on 1.8 S3; C6 per Step 1.

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

---

## Self-review notes

- **Spec coverage:** provisioning (T3), station+mDNS (T4), HTTP receiver (T4), payload reuse (T1), daemon push (T7–8), board_host override (T8/T9), BLE removal (T5), UI states (T5), re-provision gesture (T5), all-board build (T5/T10), C6 verify (T10), docs/migration (T9) — all mapped.
- **Placeholders:** none; each code step carries full code. Two deliberate "match the real name from Step 1" callouts (UsageData fields; UI apply-usage function) exist because those names must be read from the codebase, and the exact grep to find them is given.
- **Type consistency:** `NetState`, `net_init/net_tick/net_state/net_ip`, `usage_payload_parse`, `wifi_creds_load/save/clear`, `provisioning_run`, `deliver(payload, host, timeout)` used consistently across tasks.
