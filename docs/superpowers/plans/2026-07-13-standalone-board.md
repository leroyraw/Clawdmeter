# Standalone Board (self-polls Anthropic) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the board poll Anthropic directly for the user's Max-plan usage with the Mac off — the board performs its own headless OAuth login (independent token chain), refreshes its own token, and reads rate-limit headers — no daemon.

**Architecture:** Builds on the WiFi foundation already on branch `feature/wifi-transport` (provisioning, station, mDNS, web server, `usage_payload`, `ui_update`). Adds: an HTTPS helper, PKCE crypto, a `claude_oauth` token module (authorize/exchange/refresh + NVS), a `claude_poll` module (calls `/v1/messages`, parses ratelimit headers → `UsageData`), a web login flow, a poll timer, and the bootup/setup-screen fix. Retires the daemon.

**Tech Stack:** ESP32 Arduino core (`WiFiClientSecure`, `HTTPClient`, mbedTLS for SHA-256/base64/random, `Preferences`, `ESPmDNS`, `WebServer`), ArduinoJson. Build via PlatformIO.

## Global Constraints

- Board-agnostic shared code in `firmware/src/`; **no `#ifdef BOARD_*`**.
- All four envs must build: `waveshare_amoled_216`, `waveshare_amoled_18`, `waveshare_amoled_216_c6`, `waveshare_amoled_18_c6`.
- C6 = no PSRAM; TLS RAM must fit. `LV_USE_SNAPSHOT=0` on C6 (verify by eye).
- Flash port this Mac: `/dev/cu.usbmodem1101`, 115200 baud. Non-interactive serial read: `~/.platformio/penv/bin/python <scratch>/read_serial.py /dev/cu.usbmodem1101 <secs>` (pyserial only in PlatformIO venv). Reading serial RESETS the board — the board already has WiFi creds saved (persists across reset); it does NOT persist a partial OAuth login until exchange succeeds.
- Exact OAuth constants (verbatim — do not alter):
  - `CLIENT_ID = 9d1c250a-e61b-44d9-88ed-5944d1962f5e`
  - `AUTHORIZE_URL = https://claude.ai/oauth/authorize`
  - `TOKEN_URL = https://console.anthropic.com/v1/oauth/token`
  - `REDIRECT_URI = https://console.anthropic.com/oauth/code/callback`
  - `SCOPE = org:create_api_key user:profile user:inference`
  - Token-call headers: `Content-Type: application/json`, `User-Agent: anthropic`.
- Exact API poll surface (verbatim):
  - `POST https://api.anthropic.com/v1/messages`
  - Headers: `anthropic-version: 2023-06-01`, `anthropic-beta: oauth-2025-04-20`, `Content-Type: application/json`, `User-Agent: claude-code/2.1.5`, `Authorization: Bearer <access_token>`.
  - Body: `{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}`.
  - Read response headers: `anthropic-ratelimit-unified-5h-utilization`, `-5h-reset`, `-7d-utilization`, `-7d-reset`, `-5h-status` (Pro/Max); `-overage-utilization`, `-overage-reset`, `-status` (enterprise/overage).
- PKCE: `code_verifier` = base64url(32 random bytes, no `=`); `code_challenge` = base64url(SHA-256(verifier), no `=`); `code_challenge_method=S256`; `state` = the verifier value.
- Commit messages end with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Branch: continue on `feature/wifi-transport` (has the foundation).
- **Firmware has no unit-test harness: a clean `pio` build is the "test", plus on-device serial self-tests against known vectors where noted. Ignore clangd "*.h not found" IDE noise — only `pio` results matter.**

---

## Task 1: HTTPS POST helper (`https`)

A thin `WiFiClientSecure` + `HTTPClient` wrapper the OAuth/poll modules call, returning status, body, and selected response headers.

**Files:**
- Create: `firmware/src/https.h`, `firmware/src/https.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct HttpsResult { int status; String body; std::vector<String> header_values; };
  // header_names: response headers to capture (order preserved into header_values).
  HttpsResult https_post_json(const char* url, const std::vector<std::pair<String,String>>& headers,
                              const String& json_body, const std::vector<String>& capture_headers = {});
  ```
  Consumed by Tasks 3 and 4.

- [ ] **Step 1: Write `https.h`**
```cpp
#pragma once
#include <Arduino.h>
#include <vector>
#include <utility>
struct HttpsResult { int status = -1; String body; std::vector<String> header_values; };
HttpsResult https_post_json(const char* url,
                            const std::vector<std::pair<String,String>>& headers,
                            const String& json_body,
                            const std::vector<String>& capture_headers = {});
```

- [ ] **Step 2: Write `https.cpp`**
```cpp
#include "https.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

HttpsResult https_post_json(const char* url,
                            const std::vector<std::pair<String,String>>& headers,
                            const String& json_body,
                            const std::vector<String>& capture_headers) {
    HttpsResult r;
    WiFiClientSecure client;
    client.setInsecure();               // v1: skip cert verify (LAN egress; user's own account).
                                        // Hardening follow-up: bundle the Anthropic root CA.
    HTTPClient http;
    if (!http.begin(client, url)) { r.status = -1; return r; }
    for (auto& h : headers) http.addHeader(h.first, h.second);
    if (!capture_headers.empty()) {
        const char* keys[16];
        size_t n = capture_headers.size(); if (n > 16) n = 16;
        for (size_t i = 0; i < n; i++) keys[i] = capture_headers[i].c_str();
        http.collectHeaders(keys, n);
    }
    r.status = http.POST((uint8_t*)json_body.c_str(), json_body.length());
    if (r.status > 0) r.body = http.getString();
    for (auto& name : capture_headers) r.header_values.push_back(http.header(name.c_str()));
    http.end();
    return r;
}
```

- [ ] **Step 3: Add a TEMP on-device self-test in `main.cpp` (behind the existing serial command handler)**
Add a `httpstest` serial command (next to `screenshot`/`buzz`) that runs:
`https_post_json("https://api.anthropic.com/v1/messages", {{"Content-Type","application/json"}}, "{}")` and `Serial.printf("httpstest status=%d bodylen=%d\n", r.status, r.body.length());`
A `401` (or any HTTP status > 0) proves TLS handshake + reachability. (Remove this temp command in Task 7.)

- [ ] **Step 4: Build**
`for e in waveshare_amoled_216 waveshare_amoled_18 waveshare_amoled_216_c6 waveshare_amoled_18_c6; do echo "== $e =="; pio run -d firmware -e $e 2>&1 | tail -1; done` → four SUCCESS.

- [ ] **Step 5: Hardware check (S3)**
Flash `waveshare_amoled_18`; ensure it's on WiFi; over serial send `httpstest`. Expect `httpstest status=401 ...` (TLS works). Record output in the report.

- [ ] **Step 6: Commit**
```bash
git add firmware/src/https.h firmware/src/https.cpp firmware/src/main.cpp
git commit -m "feat(fw): WiFiClientSecure HTTPS POST helper + httpstest cmd

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: PKCE crypto (`pkce`)

Base64url, SHA-256, and random-verifier helpers, with an on-device self-test against the RFC 7636 vector.

**Files:**
- Create: `firmware/src/pkce.h`, `firmware/src/pkce.cpp`

**Interfaces:**
- Produces:
  ```cpp
  String base64url_encode(const uint8_t* data, size_t len);  // no '=' padding
  String pkce_make_verifier();                                // base64url(32 random bytes)
  String pkce_challenge(const String& verifier);              // base64url(sha256(verifier))
  ```
  Consumed by Task 3.

- [ ] **Step 1: Write `pkce.h`**
```cpp
#pragma once
#include <Arduino.h>
String base64url_encode(const uint8_t* data, size_t len);
String pkce_make_verifier();
String pkce_challenge(const String& verifier);
bool   pkce_selftest();   // true if RFC 7636 vector matches
```

- [ ] **Step 2: Write `pkce.cpp`**
```cpp
#include "pkce.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include <esp_system.h>

String base64url_encode(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::vector<uint8_t> out(olen + 1, 0);
    mbedtls_base64_encode(out.data(), out.size(), &olen, data, len);
    String s((char*)out.data());
    s.replace('+', '-'); s.replace('/', '_');
    while (s.endsWith("=")) s.remove(s.length() - 1);
    return s;
}
String pkce_make_verifier() {
    uint8_t rnd[32]; esp_fill_random(rnd, sizeof(rnd));
    return base64url_encode(rnd, sizeof(rnd));
}
String pkce_challenge(const String& verifier) {
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t*)verifier.c_str(), verifier.length(), hash, 0);
    return base64url_encode(hash, sizeof(hash));
}
bool pkce_selftest() {
    // RFC 7636 Appendix B vector.
    String v = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    return pkce_challenge(v) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
}
```
> `mbedtls_sha256(...,0)` selects SHA-256 (not 224). Include `<vector>` if needed.

- [ ] **Step 3: Wire the self-test into boot**
In `main.cpp setup()`, after `Serial.begin`, add `Serial.printf("pkce_selftest=%d\n", pkce_selftest());` (temporary; remove in Task 7).

- [ ] **Step 4: Build all four envs** → SUCCESS.

- [ ] **Step 5: Hardware check** — flash S3, read serial, expect `pkce_selftest=1`. Record it.

- [ ] **Step 6: Commit**
```bash
git add firmware/src/pkce.h firmware/src/pkce.cpp firmware/src/main.cpp
git commit -m "feat(fw): PKCE (base64url/sha256) with RFC7636 self-test

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Claude OAuth token module (`claude_oauth`)

Authorize-URL build, code exchange, refresh (with rotation persistence), NVS storage, freshness.

**Files:**
- Create: `firmware/src/claude_oauth.h`, `firmware/src/claude_oauth.cpp`

**Interfaces:**
- Produces:
  ```cpp
  String claude_oauth_authorize_url();            // builds URL, stashes verifier in RAM
  bool   claude_oauth_exchange(const String& pasted_code);   // code (maybe "code#state") -> tokens, persist
  bool   claude_oauth_refresh();                  // uses stored refresh token; persist rotated token
  bool   claude_oauth_ensure_fresh();             // refresh if <5min to expiry; true if a valid access token exists
  bool   claude_oauth_have_login();               // true if a refresh token is stored
  String claude_oauth_access_token();
  void   claude_oauth_logout();                   // clear NVS "claude"
  ```
  Consumed by Tasks 4, 5, 6.

- [ ] **Step 1: Write `claude_oauth.h`** — declare the interface above plus the constants from Global Constraints as `static const char*`.

- [ ] **Step 2: Write `claude_oauth.cpp`**
Key logic (fill in with the exact constants; use `https_post_json`, `pkce`, `Preferences` ns `"claude"`, ArduinoJson):
```cpp
// state (RAM): static String s_verifier;
// NVS keys in ns "claude": access, refresh, expires (uint32 epoch secs)

String claude_oauth_authorize_url() {
    s_verifier = pkce_make_verifier();
    String ch = pkce_challenge(s_verifier);
    String u = String(AUTHORIZE_URL) + "?response_type=code"
        + "&client_id=" + CLIENT_ID
        + "&redirect_uri=" + urlencode(REDIRECT_URI)
        + "&scope=" + urlencode(SCOPE)
        + "&code_challenge=" + ch + "&code_challenge_method=S256"
        + "&state=" + s_verifier + "&code=true";
    return u;
}

static bool store_tokens(JsonDocument& d) {   // from a token response
    if (!d["access_token"].is<const char*>()) return false;
    Preferences p; p.begin("claude", false);
    p.putString("access", d["access_token"].as<const char*>());
    if (d["refresh_token"].is<const char*>())
        p.putString("refresh", d["refresh_token"].as<const char*>());   // ROTATION: always persist if present
    long exp_in = d["expires_in"] | 3600;
    p.putUInt("expires", (uint32_t)(time_now_s() + exp_in));
    p.end();
    return true;
}

bool claude_oauth_exchange(const String& pasted) {
    String code = pasted; int h = code.indexOf('#'); if (h >= 0) code = code.substring(0, h);
    String body = /* JSON: grant_type=authorization_code, code, code_verifier=s_verifier,
                     client_id=CLIENT_ID, redirect_uri=REDIRECT_URI, state=s_verifier */;
    auto r = https_post_json(TOKEN_URL, {{"Content-Type","application/json"},{"User-Agent","anthropic"}}, body);
    if (r.status != 200) { Serial.printf("oauth exchange HTTP %d: %s\n", r.status, r.body.c_str()); return false; }
    JsonDocument d; if (deserializeJson(d, r.body)) return false;
    return store_tokens(d);
}

bool claude_oauth_refresh() {
    Preferences p; p.begin("claude", true); String rt = p.getString("refresh",""); p.end();
    if (!rt.length()) return false;
    String body = /* JSON: grant_type=refresh_token, refresh_token=rt, client_id=CLIENT_ID */;
    auto r = https_post_json(TOKEN_URL, {{"Content-Type","application/json"},{"User-Agent","anthropic"}}, body);
    if (r.status != 200) { Serial.printf("oauth refresh HTTP %d\n", r.status); return false; }
    JsonDocument d; if (deserializeJson(d, r.body)) return false;
    return store_tokens(d);   // persists rotated refresh token
}

bool claude_oauth_ensure_fresh() {
    Preferences p; p.begin("claude", true);
    uint32_t exp = p.getUInt("expires", 0); bool have = p.getString("refresh","").length() > 0; p.end();
    if (!have) return false;
    if (time_now_s() + 300 >= exp) return claude_oauth_refresh();
    return true;
}
```
Provide a `time_now_s()` using the RTC/`time(nullptr)` if NTP is set, else a monotonic fallback seeded at boot (the board has no wall clock unless the daemon set it — add a lightweight SNTP sync in `net.cpp` on connect, OR track expiry via `millis()` + `expires_in` deltas). **Decision for the implementer to surface if unclear:** simplest robust approach is `configTime()` SNTP on WiFi connect so `time(nullptr)` is real; note it in the report.
`urlencode` — minimal percent-encoder for the URL params.

- [ ] **Step 3: Build all four envs** → SUCCESS. (No hardware login yet — that's Task 8.)

- [ ] **Step 4: Commit**
```bash
git add firmware/src/claude_oauth.h firmware/src/claude_oauth.cpp firmware/src/net.cpp
git commit -m "feat(fw): Claude OAuth module (authorize/exchange/refresh + NVS, rotation-safe)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Poll module (`claude_poll`)

Call `/v1/messages`, read ratelimit headers → `UsageData` (mirror the daemon's `poll_api`).

**Files:**
- Create: `firmware/src/claude_poll.h`, `firmware/src/claude_poll.cpp`
- Reference: `daemon/claude_usage_daemon.py::poll_api` (lines ~376-436) for exact field mapping; `firmware/src/data.h`.

**Interfaces:**
- Produces: `bool claude_poll(UsageData& out);` — true on a successful poll that filled `out`. Consumed by Task 6.

- [ ] **Step 1: Write the module**
```cpp
bool claude_poll(UsageData& out) {
    if (!claude_oauth_ensure_fresh()) return false;
    String tok = claude_oauth_access_token();
    std::vector<std::pair<String,String>> h = {
        {"anthropic-version","2023-06-01"}, {"anthropic-beta","oauth-2025-04-20"},
        {"Content-Type","application/json"}, {"User-Agent","claude-code/2.1.5"},
        {"Authorization","Bearer " + tok}};
    std::vector<String> caps = {
        "anthropic-ratelimit-unified-5h-utilization","anthropic-ratelimit-unified-5h-reset",
        "anthropic-ratelimit-unified-7d-utilization","anthropic-ratelimit-unified-7d-reset",
        "anthropic-ratelimit-unified-5h-status"};
    const char* body = "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    auto r = https_post_json("https://api.anthropic.com/v1/messages", h, body, caps);
    if (r.status == 401) { if (!claude_oauth_refresh()) return false; return claude_poll(out); } // one retry
    if (r.status <= 0) return false;
    // headers present even on 200; map like poll_api:
    // out.session_pct = round(util5h*100); out.session_reset_mins = minutes_until(reset5h);
    // out.weekly_pct = round(util7d*100); out.weekly_reset_mins = minutes_until(reset7d);
    // out.status = status5h; out.enterprise=false; out.ok=true; out.valid=true;
    // (add the overage/enterprise branch if 5h util header absent, per poll_api)
    return true;
}
```
Guard the 401-retry against infinite recursion (retry once).

- [ ] **Step 2: Build all four envs** → SUCCESS.

- [ ] **Step 3: Commit**
```bash
git add firmware/src/claude_poll.h firmware/src/claude_poll.cpp
git commit -m "feat(fw): poll api.anthropic.com ratelimit headers -> UsageData

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Web login flow + auth state

Extend `net.cpp`'s web server with the login UI, and add an auth state the UI reflects.

**Files:**
- Modify: `firmware/src/net.h`, `firmware/src/net.cpp`

- [ ] **Step 1: Auth state** — extend `NetState` to `{ NET_PROVISIONING, NET_CONNECTING, NET_ONLINE_NEEDS_LOGIN, NET_ONLINE_OK }`. In `net_tick()`/after connect, set `NET_ONLINE_NEEDS_LOGIN` if `!claude_oauth_have_login()`, else `NET_ONLINE_OK`.

- [ ] **Step 2: Routes** in `net_init()`:
  - `GET /` — if `!claude_oauth_have_login()`: page with a link to `claude_oauth_authorize_url()` (target=_blank) and a `POST /login` form (text input `code`). Else: "Logged in — usage on the device" + a `POST /logout` button + IP/status.
  - `POST /login` — `String code = server.arg("code"); bool ok = claude_oauth_exchange(code);` → success/error page; on success the poll loop (Task 6) picks it up.
  - `POST /logout` — `claude_oauth_logout()`.
  Keep the existing `POST /usage` debug route.

- [ ] **Step 3: Build all four envs** → SUCCESS.

- [ ] **Step 4: Commit**
```bash
git add firmware/src/net.h firmware/src/net.cpp
git commit -m "feat(fw): web Claude-login flow + auth state

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Poll timer, UI hints, bootup/setup-screen fix

**Files:**
- Modify: `firmware/src/main.cpp`, `firmware/src/ui.cpp`, `firmware/src/ui.h`

- [ ] **Step 1: Poll timer** in `loop()` — every 60s when `net_state()==NET_ONLINE_OK`: `UsageData u{}; if (claude_poll(u)) ui_update(&u);` (reuse the chime/usage_rate/splash side-effects already in `net.cpp`'s `/usage` handler — factor that into a shared `apply_usage(const UsageData&)` in `net.cpp` and call it from both the poll timer and `/usage`). Use a `static uint32_t last_poll` + `millis()` gate; poll once immediately on entering `NET_ONLINE_OK`.

- [ ] **Step 2: UI hints** — `ui_update_net_status` maps: `NET_ONLINE_NEEDS_LOGIN` → "Log in to Claude\nopen clawdmeter.local"; `NET_PROVISIONING`/`NET_CONNECTING` as before; `NET_ONLINE_OK` → connected (usage view).

- [ ] **Step 3: Bootup/setup-screen fix** — ensure the panel is never black during setup: before the blocking WiFi-connect wait in `net_init()` and before `provisioning_run()`, call `ui_update_net_status(state, ...)` then pump `lv_timer_handler()` ~5× with small delays to flush; and make `provisioning_run()`'s loop call `lv_timer_handler()` each iteration so the setup screen stays live. (net.cpp/provisioning.cpp `#include <lvgl.h>` as needed.)

- [ ] **Step 4: Build all four envs** → SUCCESS.

- [ ] **Step 5: Commit**
```bash
git add -A
git commit -m "feat(fw): 60s poll loop, login UI hints, non-black setup screen

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Retire daemon, remove temp self-tests, docs

**Files:**
- Modify: `firmware/src/main.cpp` (remove the `httpstest` cmd + `pkce_selftest` boot print)
- Modify: `install.sh`, `install-mac.sh` (drop daemon/launchd install)
- Modify: `README.md`, `CLAUDE.md`
- Optionally: leave `daemon/` in place (unused) — do NOT delete history; just stop installing it.

- [ ] **Step 1** Remove the temp `httpstest` serial command and the `pkce_selftest=` boot print from `main.cpp`.
- [ ] **Step 2** Installers: remove daemon/launchd steps; the board is now self-contained (no host needed).
- [ ] **Step 3** Docs: `README.md` + `CLAUDE.md` — new setup: flash → WiFi captive portal → open `clawdmeter.local` → "Log in to Claude" (paste code) → usage. Note: no daemon/Mac; board holds its own Claude login; known-risk note (undocumented OAuth).
- [ ] **Step 4** Build all four envs → SUCCESS.
- [ ] **Step 5** Commit
```bash
git add -A
git commit -m "chore: retire daemon, remove temp self-tests, document standalone setup

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Hardware bring-up (WITH THE USER) + C6 + finish

The first real login mints a live token chain — do it once, carefully, with the user.

**Files:** none (verify/fixup), then PR.

- [ ] **Step 1: Flash S3** `pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem1101`.
- [ ] **Step 2: Setup screen not black** — confirm on-panel "Connecting…" then "Log in to Claude / open clawdmeter.local" (no black screen).
- [ ] **Step 3: Login (user)** — open `http://clawdmeter.local`, click the authorize link, approve in the browser, copy the code from Anthropic's callback page, paste into the board's form, submit. Serial should show a successful exchange (HTTP 200). If it fails, capture the serial `oauth exchange HTTP <n>: <body>` and diagnose (scope/param/UA issues).
- [ ] **Step 4: Usage** — within ~60s the board polls and shows real Max usage. Confirm the numbers match `claude.ai` / Claude Code.
- [ ] **Step 5: No conflict** — use Claude Code on the Mac; confirm it still works (independent chains). Optionally leave the board a few hours to confirm a refresh cycle persists the rotated token (board keeps working after access-token expiry).
- [ ] **Step 6: C6** — if a C6 board is available, flash `waveshare_amoled_18_c6`, repeat 2–4, watch serial for TLS/alloc failures (no PSRAM). If unavailable, note "C6 unverified".
- [ ] **Step 7: Build matrix** — all four envs SUCCESS.
- [ ] **Step 8: Commit fixups** (if any) and hold the PR per the user's "keep local" preference (or open per their call).

---

## Self-review notes

- **Spec coverage:** HTTPS (T1), PKCE (T2), OAuth authorize/exchange/refresh+NVS+rotation (T3), poll+header-map (T4), web login+auth state (T5), poll timer + login hint + bootup fix (T6), daemon retire + docs (T7), hardware login + C6 (T8). All mapped.
- **Placeholders:** T3/T4 leave JSON-body construction and the header→field mapping as guided fills because they must be transcribed from exact constants (Global Constraints) and the daemon's `poll_api`; the module skeleton, endpoints, headers, and return contracts are complete. Every other step carries full code.
- **Time source risk called out** in T3 (SNTP vs millis) for the implementer to resolve and report.
- **Type consistency:** `https_post_json`/`HttpsResult`, `base64url_encode`/`pkce_challenge`/`pkce_make_verifier`, `claude_oauth_*`, `claude_poll`, extended `NetState` used consistently across tasks.
