// firmware/src/net.h
#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include "data.h"

enum NetState { NET_PROVISIONING, NET_CONNECTING, NET_ONLINE_NEEDS_LOGIN, NET_ONLINE_OK };

void      net_init();
void      net_tick();
NetState  net_state();
IPAddress net_ip();

// Shared post-parse side effects (usage-rate tracking, session-reset chime,
// splash rate-matching, UI refresh) for a freshly-parsed UsageData — used by
// both the /usage POST handler and the on-device 60s poll timer in main.cpp.
void apply_usage(const UsageData& d);
