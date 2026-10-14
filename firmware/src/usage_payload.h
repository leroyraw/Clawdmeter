#pragma once
#include <stddef.h>
#include "data.h"

// Parse the daemon's usage payload JSON into a UsageData.
//
// Mirrors main.cpp's parse_json field-for-field:
//   s    -> session_pct        (float, default 0.0)
//   sr   -> session_reset_mins (int, default -1)
//   w    -> weekly_pct         (float, default 0.0)
//   wr   -> weekly_reset_mins  (int, default -1)
//   st   -> status             (string, default "unknown")
//   c    -> chime              (bool, default false)
//   acct -> enterprise         (true when acct == "ent", default "pro")
//   tp   -> time_pct           (int, default 0)
//   pd   -> period_days        (int, default 30)
//   rd   -> reset_date         (string, default "")
//   t    -> clock_epoch        (long, default 0)
//   tf   -> clock_fmt          (int, default 24)
//   ok   -> ok                 (bool, default false)
//   out.valid is set true on any successful JSON parse.
//
// Returns false only when the JSON itself fails to deserialize; missing
// individual fields fall back to their defaults above (matching the
// behavior of the original parser this replaces).
bool usage_payload_parse(const char* json, size_t len, UsageData& out);
