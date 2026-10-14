// firmware/src/claude_oauth.h
//
// Claude OAuth token module: the board's OWN OAuth login (independent of
// the host daemon's token chain). Builds the authorize URL (PKCE), exchanges
// a pasted authorization code for tokens, refreshes with rotation-safe
// persistence, and stores everything in NVS namespace "claude".
//
// Time source: expiry math needs a real wall clock. net.cpp kicks off an
// SNTP sync (configTime()) right after WiFi connects; claude_oauth treats
// the clock as "unsynced" until time(nullptr) reports a plausible epoch
// (see claude_oauth.cpp) and refreshes proactively until then.
#pragma once
#include <Arduino.h>

// --- Exact OAuth constants (see docs/porting or task briefs for provenance) ---
static const char* CLAUDE_OAUTH_CLIENT_ID     = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char* CLAUDE_OAUTH_AUTHORIZE_URL = "https://claude.ai/oauth/authorize";
static const char* CLAUDE_OAUTH_TOKEN_URL      = "https://console.anthropic.com/v1/oauth/token";
static const char* CLAUDE_OAUTH_REDIRECT_URI   = "https://console.anthropic.com/oauth/code/callback";
static const char* CLAUDE_OAUTH_SCOPE          = "org:create_api_key user:profile user:inference";

// Build the authorize URL for the user to open in a browser. Generates a
// fresh PKCE verifier/challenge pair and stashes the verifier in RAM (used
// again as `state` and later as `code_verifier` in the exchange call).
String claude_oauth_authorize_url();

// Exchange a pasted authorization code (may be "code#state", only the part
// before '#' is the code) for tokens. Persists access/refresh/expires to
// NVS on success.
bool claude_oauth_exchange(const String& pasted_code);

// Refresh using the stored refresh token. Persists the (possibly rotated)
// refresh token — Anthropic rotates refresh tokens on every use.
bool claude_oauth_refresh();

// Ensures a valid access token is available, refreshing proactively if
// within 5 minutes of expiry (or if the wall clock isn't trustworthy yet).
// Returns false if there's no stored login to refresh from.
bool claude_oauth_ensure_fresh();

// True if a refresh token is stored (i.e. the board has completed login).
bool claude_oauth_have_login();

// Current access token (may be stale — call claude_oauth_ensure_fresh()
// first if freshness matters).
String claude_oauth_access_token();

// Clears NVS namespace "claude" (access/refresh/expires) and the in-RAM
// PKCE verifier.
void claude_oauth_logout();
