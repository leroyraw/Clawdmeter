// firmware/src/claude_poll.cpp
#include "claude_poll.h"
#include "claude_oauth.h"
#include "https.h"
#include <math.h>
#include <string.h>
#include <time.h>

static const char* API_URL = "https://api.anthropic.com/v1/messages";

// max_tokens=1 — this call exists solely to harvest the response headers,
// not for the completion itself.
static const char* API_BODY =
    "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,"
    "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

// Headers captured from the response, in this exact order — code below
// indexes HttpsResult::header_values positionally, so this order and the
// enum below must stay in lockstep.
static const std::vector<String> CAPTURE_HEADERS = {
    "anthropic-ratelimit-unified-5h-utilization",   // 0
    "anthropic-ratelimit-unified-5h-reset",         // 1
    "anthropic-ratelimit-unified-7d-utilization",   // 2
    "anthropic-ratelimit-unified-7d-reset",         // 3
    "anthropic-ratelimit-unified-5h-status",        // 4
    "anthropic-ratelimit-unified-overage-utilization", // 5
    "anthropic-ratelimit-unified-overage-reset",    // 6
    "anthropic-ratelimit-unified-status",           // 7
};
enum {
    IDX_5H_UTIL = 0,
    IDX_5H_RESET,
    IDX_7D_UTIL,
    IDX_7D_RESET,
    IDX_5H_STATUS,
    IDX_OVERAGE_UTIL,
    IDX_OVERAGE_RESET,
    IDX_STATUS,
};

// --- header -> field helpers (mirrors poll_api's pct()/reset_minutes()) ---

// Utilization header (e.g. "0.42") -> whole percent 0-100, rounded.
static int pct(const String& util) {
    if (!util.length()) return 0;
    return (int)lroundf(strtof(util.c_str(), nullptr) * 100.0f);
}

// Reset header (unix epoch seconds, e.g. "1731500000") -> minutes from now,
// clamped to >= 0. Malformed/empty/non-positive -> 0.
static int minutes_until(const String& reset_ts) {
    if (!reset_ts.length()) return 0;
    double reset = strtod(reset_ts.c_str(), nullptr);
    if (reset <= 0) return 0;
    double mins = (reset - (double)time(nullptr)) / 60.0;
    return mins > 0 ? (int)lround(mins) : 0;
}

static void set_status(char* dst, size_t dst_size, const String& s) {
    strlcpy(dst, s.length() ? s.c_str() : "unknown", dst_size);
}

// allow_retry guards the single 401-refresh-retry against unbounded
// recursion (a second consecutive 401 returns false rather than looping).
static bool do_poll(UsageData& out, bool allow_retry) {
    if (!claude_oauth_ensure_fresh()) return false;
    String tok = claude_oauth_access_token();

    std::vector<std::pair<String, String>> headers = {
        {"anthropic-version", "2023-06-01"},
        {"anthropic-beta", "oauth-2025-04-20"},
        {"Content-Type", "application/json"},
        {"User-Agent", "claude-code/2.1.5"},
        {"Authorization", "Bearer " + tok},
    };

    HttpsResult r = https_post_json(API_URL, headers, API_BODY, CAPTURE_HEADERS);

    if (r.status == 401) {
        if (!allow_retry) return false;
        if (!claude_oauth_refresh()) return false;
        return do_poll(out, /*allow_retry=*/false);
    }
    if (r.status <= 0) return false;

    // Ratelimit headers are present on any status > 0 (even non-2xx), same
    // as poll_api reading them off a 200 — map them regardless of status.
    const String& util5h = r.header_values[IDX_5H_UTIL];

    if (util5h.length()) {
        // Pro/Max: rolling 5h + 7d windows.
        out.session_pct = (float)pct(util5h);
        out.session_reset_mins = minutes_until(r.header_values[IDX_5H_RESET]);
        out.weekly_pct = (float)pct(r.header_values[IDX_7D_UTIL]);
        out.weekly_reset_mins = minutes_until(r.header_values[IDX_7D_RESET]);
        set_status(out.status, sizeof(out.status), r.header_values[IDX_5H_STATUS]);
        out.enterprise = false;
    } else {
        // Enterprise/overage: single spending-limit window.
        out.session_pct = (float)pct(r.header_values[IDX_OVERAGE_UTIL]);
        out.session_reset_mins = minutes_until(r.header_values[IDX_OVERAGE_RESET]);
        out.weekly_pct = 0.0f;
        out.weekly_reset_mins = 0;
        set_status(out.status, sizeof(out.status), r.header_values[IDX_STATUS]);
        out.enterprise = true;
    }

    // Billing-period derivation left minimal for v1 (see task brief) — the
    // board doesn't yet compute the Enterprise time_pct/reset_date the
    // daemon's _billing_period_info() does.
    out.time_pct = 0;
    out.period_days = 30;
    out.reset_date[0] = '\0';

    // Not provided by this call; defaults match usage_payload_parse().
    out.chime = false;
    out.clock_epoch = 0;
    out.clock_fmt = 24;

    out.ok = true;
    out.valid = true;
    return true;
}

bool claude_poll(UsageData& out) {
    return do_poll(out, /*allow_retry=*/true);
}
