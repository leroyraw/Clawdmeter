# Standalone board (self-polls Anthropic, no daemon) — design

Date: 2026-07-13
Status: Approved shape (full standalone, board does its own OAuth), pending spec review
Supersedes: the daemon half of `2026-07-13-wifi-transport-design.md` (Tasks 8–10 of that plan are dropped).

## Motivation

The user wants the monitor to work with their Mac **off** — no daemon, no host.
The board should poll Anthropic directly and show their real Max-plan usage.

The blocker we designed around: Anthropic OAuth **rotates the refresh token** on
every refresh, so a client that shares another client's token invalidates it.
Resolution (confirmed by how Claude in Chrome + Claude Code + phone coexist):
the board performs its **own** OAuth login and gets an **independent** token
chain — it becomes a 4th client, conflicting with nothing.

## Goals / non-goals

**Goals**
- Board self-authenticates to Claude (its own OAuth login, headless via paste-code).
- Board refreshes its own access token (rotation persisted) and polls
  `api.anthropic.com` directly over HTTPS to read subscription rate-limit headers.
- Fully standalone: Mac off, no daemon.
- Token provisioning via the board's web page (extends the existing captive-portal
  / mDNS web server).
- Fold in the bootup fix: paint a setup/connecting/login screen before any blocking wait.
- Works on S3; verify on C6 (no PSRAM) given TLS RAM cost.

**Non-goals**
- No daemon (removed/retired). The board's `POST /usage` receiver is kept only as
  an optional debug override (curl a payload), not a required path.
- No BLE (already removed).
- Not solving multi-account; one Claude login per board.

## Known risks (accepted)

- **Undocumented OAuth.** Endpoints/client_id (`9d1c250a-e61b-44d9-88ed-5944d1962f5e`,
  `claude.ai/oauth/authorize`, `console.anthropic.com/v1/oauth/token`) are
  community-derived, not public API. Anthropic can change them and break the board.
- **Refresh-endpoint rate limiting.** Community reports of 429s on headless refresh
  ([claude-code#38248]). An independent chain refreshing ~daily should behave like
  any device, but this is a live risk.
- **Refresh-token rotation.** Each refresh returns a NEW refresh token; the board
  MUST persist it atomically or it locks itself out (→ re-login).
- **Token on device.** The board's own Claude refresh token lives in NVS. If the
  device is lost/flash-dumped it leaks (mints inference tokens for the account).
  Lower blast radius than sharing the Mac's token, but real.
- **Polling consumes usage.** Each 60s poll is a real `max_tokens:1` inference call
  (same as today's daemon) — counts toward the account minutely.

## Architecture

```
                 (one-time login, in the USER's browser)
 board web page ──shows──> claude.ai/oauth/authorize?PKCE ──approve──> code
        ▲ paste code                                                    │
        └──────────────── user pastes code ─────────────────────────────┘
 board ──POST code+verifier──> console.anthropic.com/v1/oauth/token ──> {access,refresh,expires}
        store in NVS (own chain)

                 (every 60s, Mac off)
 board: ensure token fresh (refresh via console.anthropic.com if <5min to expiry / on 401)
      → POST api.anthropic.com/v1/messages (Bearer access, tiny body)
      → read anthropic-ratelimit-unified-* headers → UsageData → ui_update()
```

## Firmware components (shared code, builds on the WiFi foundation from Tasks 1–6)

### New modules
- `claude_oauth.{h,cpp}` — the auth state machine and token store:
  - PKCE: `code_verifier` = base64url(32 random bytes, no pad); `code_challenge`
    = base64url(SHA-256(verifier)) via mbedTLS; `state` = verifier (Claude Code quirk).
  - `String claude_oauth_authorize_url()` — builds the authorize URL (params:
    `response_type=code`, `client_id`, `redirect_uri=https://console.anthropic.com/oauth/code/callback`,
    `scope=org:create_api_key user:profile user:inference` (Claude Code's exact known-working scope string — do not trim), `code_challenge`, `code_challenge_method=S256`,
    `state`, `code=true`). Stashes the verifier for the pending exchange.
  - `bool claude_oauth_exchange(const String& pasted_code)` — POST
    `console.anthropic.com/v1/oauth/token` (`grant_type=authorization_code`, `code`,
    `code_verifier`, `client_id`, `redirect_uri`). On success persist access/refresh/
    expiresAt to NVS. Handle a `code#state` paste (split on `#`).
  - `bool claude_oauth_refresh()` — POST same endpoint (`grant_type=refresh_token`,
    `refresh_token`, `client_id`). **Persist the rotated refresh_token + new expiry.**
  - `bool claude_oauth_ensure_fresh()` — refresh if now >= expiresAt − 5 min; returns
    whether a valid access token is available.
  - `String claude_oauth_access_token()`, `bool claude_oauth_have_login()`,
    `void claude_oauth_logout()` (clears NVS namespace `"claude"`).
  - Headers on token calls: `Content-Type: application/json`, `User-Agent: anthropic`.
- `claude_poll.{h,cpp}` — `bool claude_poll(UsageData& out)`:
  - POST `api.anthropic.com/v1/messages` with `Authorization: Bearer <access>`,
    `anthropic-version` + the tiny body (`model: claude-haiku-4-5-...`, `max_tokens:1`,
    `messages:[{role:user,content:"hi"}]`).
  - On 401 → one `claude_oauth_refresh()` + retry.
  - Parse `anthropic-ratelimit-unified-5h-utilization`/`-5h-reset`/`-7d-utilization`/
    `-7d-reset`/`-5h-status` into `UsageData` (percent = round(util*100); reset minutes
    from epoch). Pro/Max path; overage/enterprise path mirrors the daemon's `poll_api`.
- `https.{h,cpp}` (thin) — a `WiFiClientSecure`-based POST helper returning status +
  body + a selected set of response headers (so `claude_poll` can read the ratelimit
  headers). Root-CA strategy below.

### TLS / root CA
- Board makes HTTPS to `console.anthropic.com` and `api.anthropic.com`.
- v1: bundle the root CA cert(s) these hosts chain to (compile-time PEM) and verify.
  Fallback if cert management is painful: `setInsecure()` with a logged warning
  (skips verification — acceptable only because it's the user's own account on their
  LAN egress, but bundling the CA is preferred and is the default).

### Token/UI provisioning flow
- Reuse the existing web server (`net.cpp`). Add routes:
  - `GET /` — status; if not logged in, show the **authorize link** + a
    paste-code form; if logged in, show usage/status.
  - `POST /login` — takes the pasted code → `claude_oauth_exchange` → success/err page.
  - `POST /logout` — `claude_oauth_logout`.
- On-screen (UI): a `NET`/auth state hint — "Log in: open clawdmeter.local" when
  WiFi is up but not logged in; usage once data flows. (A QR of the authorize URL
  on-screen is a nice-to-have, not v1.)

### Main loop / state
- Extend `NetState` (or add an auth layer): PROVISIONING (WiFi) → CONNECTING →
  ONLINE_NEEDS_LOGIN → ONLINE_OK. UI maps each to a hint; ONLINE_OK shows usage.
- Poll timer: every 60s when ONLINE_OK → `claude_oauth_ensure_fresh()` → `claude_poll()`
  → `ui_update()`. Backoff/log on errors; surface persistent auth failure as
  "Please log in again" (refresh token expired/revoked).

### Bootup fix (folded in)
- Before any blocking wait in `net_init()` (WiFi connect) and before entering the
  provisioning loop, paint the appropriate screen and flush LVGL once
  (`ui_update_net_status(...)` + a few `lv_timer_handler()` pumps) so the panel is
  never black during setup/connect. Prefer making the provisioning loop pump
  `lv_timer_handler()` so the setup screen stays live.

### Retire the daemon
- Drop plan Tasks 8–10. Remove/park `daemon/` (keep in repo history; delete the
  launchd plist install from installers). `README`/`CLAUDE.md` document the
  board-only setup: flash → WiFi setup → Claude login → usage.

## Data model
`UsageData` unchanged (same fields the current firmware renders). `claude_poll`
fills it exactly as the daemon's `poll_api` built its payload.

## Error handling
- No login → UI "log in" hint + web form; no polling attempted.
- Refresh 401/invalid_grant (refresh token dead) → clear tokens, revert to
  NEEDS_LOGIN, UI "please log in again".
- Refresh 429 → backoff and keep using the current access token until it truly
  expires; log; surface only if polling then 401s.
- API/network error → keep last good usage on screen; retry next cycle.
- NVS write of rotated refresh token must complete before the old token is
  considered gone (persist-then-use).

## Testing / verification
- No on-device unit harness; verify on hardware. PKCE/base64url/SHA-256 can be
  cross-checked against a known vector at build bring-up via a serial self-test.
- Auth is delicate: the FIRST real login on hardware is the acceptance test
  (authorize → paste code → exchange → NVS → poll → usage on screen). Do it once,
  carefully, with the user, since it mints a real independent token chain.
- Confirm the Mac's Claude Code is UNAFFECTED after the board logs in and refreshes
  (independent chains).
- C6: confirm TLS + poll fit in internal RAM (no PSRAM).

## Rollout
One-time: flash → WiFi captive portal → open `clawdmeter.local` → Claude login
(paste code) → done. Mac/daemon no longer needed.

## Future work
- On-screen QR of the authorize URL (skip the "open clawdmeter.local" step).
- OTA firmware updates over WiFi.
- Optional shared-secret on the debug `POST /usage`.
