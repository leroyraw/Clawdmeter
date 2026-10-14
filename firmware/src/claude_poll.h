// firmware/src/claude_poll.h
//
// Polls api.anthropic.com/v1/messages with a minimal (max_tokens=1) request
// solely to harvest the anthropic-ratelimit-* response headers Anthropic
// attaches to every /v1/messages call, and maps them into a UsageData —
// the board's own on-device replacement for the host daemon's poll_api()
// (see daemon/claude_usage_daemon.py::poll_api).
#pragma once
#include "data.h"

// Polls the API and fills `out` on success. Returns false if:
//  - the board has no fresh/refreshable OAuth login
//    (claude_oauth_ensure_fresh() fails),
//  - the HTTPS request itself failed (DNS/TLS/connect — status <= 0), or
//  - the response is a 401 that survives one refresh-and-retry attempt.
// Any other status > 0 (including non-2xx) still carries the ratelimit
// headers and is treated as a successful poll, matching poll_api's
// behavior of reading headers even off a non-200 response.
bool claude_poll(UsageData& out);
