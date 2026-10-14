#include "usage_payload.h"
#include <ArduinoJson.h>
#include <string.h>

bool usage_payload_parse(const char* json, size_t len, UsageData& out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) return false;

    out.session_pct = doc["s"] | 0.0f;
    out.session_reset_mins = doc["sr"] | -1;
    out.weekly_pct = doc["w"] | 0.0f;
    out.weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out.status, doc["st"] | "unknown", sizeof(out.status));
    out.chime = doc["c"] | false;   // absent (old daemon / chime off) -> stay silent
    const char* acct = doc["acct"] | "pro";
    out.enterprise = (strcmp(acct, "ent") == 0);
    out.time_pct = doc["tp"] | 0;
    out.period_days = doc["pd"] | 30;
    strlcpy(out.reset_date, doc["rd"] | "", sizeof(out.reset_date));
    out.clock_epoch = doc["t"] | 0L;
    out.clock_fmt = doc["tf"] | 24;
    out.ok = doc["ok"] | false;
    out.valid = true;
    return true;
}
