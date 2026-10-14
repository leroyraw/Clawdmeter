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
