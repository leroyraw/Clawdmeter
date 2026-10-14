// firmware/src/claude_oauth.cpp
#include "claude_oauth.h"
#include "https.h"
#include "pkce.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

static const char* NVS_NS = "claude";

// PKCE verifier for the in-flight authorize/exchange round trip. Also
// reused verbatim as `state` (see claude_oauth_authorize_url()).
static String s_verifier;

// --- time helpers -----------------------------------------------------

// Wall-clock seconds since epoch. net.cpp calls configTime() (SNTP) right
// after WiFi connects; until that sync lands, time(nullptr) is small
// (seconds since boot-ish, or 0), so callers must not trust expiry math
// against it — see the guard in claude_oauth_ensure_fresh().
static uint32_t time_now_s() {
    return (uint32_t)time(nullptr);
}

// 2023-11-14 ~00:00 UTC — well before any real deployment, comfortably
// after any unsynced clock's default value. Used solely to detect "clock
// not synced yet" so we never trust expiry math against a bogus clock.
static const time_t UNSYNCED_CLOCK_THRESHOLD = 1700000000;

static bool clock_is_synced() {
    return time(nullptr) >= UNSYNCED_CLOCK_THRESHOLD;
}

// --- minimal percent-encoder for authorize URL query params -----------

static String urlencode(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
        uint8_t c = (uint8_t)s[i];
        bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// --- NVS persistence ----------------------------------------------------

// Persists a token response {access_token, refresh_token?, expires_in}.
// refresh_token is only present in the response when Anthropic rotates it
// (i.e. essentially always) — always persist it when present so the next
// refresh call doesn't use a stale/invalidated token.
static bool store_tokens(JsonDocument& d) {
    if (!d["access_token"].is<const char*>()) return false;

    Preferences p;
    p.begin(NVS_NS, false);
    p.putString("access", d["access_token"].as<const char*>());
    if (d["refresh_token"].is<const char*>()) {
        p.putString("refresh", d["refresh_token"].as<const char*>());
    }
    long exp_in = d["expires_in"] | 3600;
    p.putUInt("expires", (uint32_t)(time_now_s() + (uint32_t)exp_in));
    p.end();
    return true;
}

// --- public interface -----------------------------------------------------

String claude_oauth_authorize_url() {
    s_verifier = pkce_make_verifier();
    String challenge = pkce_challenge(s_verifier);

    String u = String(CLAUDE_OAUTH_AUTHORIZE_URL) + "?response_type=code"
        + "&client_id=" + CLAUDE_OAUTH_CLIENT_ID
        + "&redirect_uri=" + urlencode(CLAUDE_OAUTH_REDIRECT_URI)
        + "&scope=" + urlencode(CLAUDE_OAUTH_SCOPE)
        + "&code_challenge=" + challenge
        + "&code_challenge_method=S256"
        + "&state=" + s_verifier
        + "&code=true";
    return u;
}

bool claude_oauth_exchange(const String& pasted_code) {
    // Anthropic's callback page shows "<code>#<state>". Since state is the
    // verifier (see claude_oauth_authorize_url()), take it from the pasted
    // string rather than s_verifier: s_verifier is regenerated on every GET /
    // (page reload) and lost on reboot, which made pasted codes fail as if
    // they had expired.
    String code = pasted_code;
    code.trim();
    String verifier = s_verifier;
    int hash = code.indexOf('#');
    if (hash >= 0) {
        String state = code.substring(hash + 1);
        code = code.substring(0, hash);
        if (state.length()) verifier = state;
    }
    if (!code.length() || !verifier.length()) return false;

    JsonDocument req;
    req["grant_type"] = "authorization_code";
    req["code"] = code;
    req["code_verifier"] = verifier;
    req["client_id"] = CLAUDE_OAUTH_CLIENT_ID;
    req["redirect_uri"] = CLAUDE_OAUTH_REDIRECT_URI;
    req["state"] = verifier;
    String body;
    serializeJson(req, body);

    HttpsResult r = https_post_json(CLAUDE_OAUTH_TOKEN_URL,
        {{"Content-Type", "application/json"}, {"User-Agent", "anthropic"}}, body);
    if (r.status != 200) {
        Serial.printf("oauth exchange HTTP %d: %s\n", r.status, r.body.c_str());
        return false;
    }

    JsonDocument d;
    if (deserializeJson(d, r.body)) return false;
    return store_tokens(d);
}

bool claude_oauth_refresh() {
    Preferences p;
    p.begin(NVS_NS, true);
    String refresh_token = p.getString("refresh", "");
    p.end();
    if (!refresh_token.length()) return false;

    JsonDocument req;
    req["grant_type"] = "refresh_token";
    req["refresh_token"] = refresh_token;
    req["client_id"] = CLAUDE_OAUTH_CLIENT_ID;
    String body;
    serializeJson(req, body);

    HttpsResult r = https_post_json(CLAUDE_OAUTH_TOKEN_URL,
        {{"Content-Type", "application/json"}, {"User-Agent", "anthropic"}}, body);
    if (r.status != 200) {
        Serial.printf("oauth refresh HTTP %d: %s\n", r.status, r.body.c_str());
        // Refresh token rejected for good (revoked, or already rotated away)
        // -> drop the login so the UI asks to log in again instead of sitting
        // on an empty usage screen forever. Transient failures keep it.
        if ((r.status == 400 || r.status == 401) && r.body.indexOf("invalid_grant") >= 0) {
            claude_oauth_logout();
        }
        return false;
    }

    JsonDocument d;
    if (deserializeJson(d, r.body)) return false;
    return store_tokens(d);   // persists rotated refresh token
}

bool claude_oauth_ensure_fresh() {
    Preferences p;
    p.begin(NVS_NS, true);
    uint32_t expires = p.getUInt("expires", 0);
    bool have_login = p.getString("refresh", "").length() > 0;
    p.end();

    if (!have_login) return false;

    // Clock not synced yet -> can't trust expiry math against it. Use the
    // stored access token anyway; if it has expired, the poll's 401 path
    // refreshes. (Refreshing unconditionally here rotated the refresh token
    // on every boot, and a reboot mid-rotation loses the login.)
    if (!clock_is_synced()) return true;

    if (time_now_s() + 300 >= expires) return claude_oauth_refresh();
    return true;
}

bool claude_oauth_have_login() {
    Preferences p;
    p.begin(NVS_NS, true);
    bool have = p.getString("refresh", "").length() > 0;
    p.end();
    return have;
}

String claude_oauth_access_token() {
    Preferences p;
    p.begin(NVS_NS, true);
    String token = p.getString("access", "");
    p.end();
    return token;
}

void claude_oauth_logout() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.clear();
    p.end();
    s_verifier = "";
}
