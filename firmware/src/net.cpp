// firmware/src/net.cpp
#include "net.h"
#include "wifi_creds.h"
#include "provisioning.h"
#include "usage_payload.h"
#include "data.h"
#include "ui.h"
#include "usage_rate.h"
#include "splash.h"
#include "hal/sound_hal.h"
#include "claude_oauth.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <lvgl.h>

static WebServer server(80);
static NetState  state = NET_PROVISIONING;

// Pumps LVGL a few times so the panel reflects the latest
// ui_update_net_status() call during the blocking setup/connect stretch in
// net_init(), where loop()'s lv_timer_handler() hasn't started running yet.
static void pump_ui(NetState s) {
    ui_update_net_status(s, IPAddress());
    for (int i = 0; i < 5; i++) { lv_timer_handler(); delay(20); }
}

void apply_usage(const UsageData& d) {
    int g_before = usage_rate_group();
    bool session_reset = usage_rate_sample(d.session_pct);
    int g_after = usage_rate_group();
    // 5-hour session limit refilled → chime so the user knows they can
    // use Claude again (no-op on boards without a buzzer). Gated on the
    // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
    if (session_reset && d.chime) {
        Serial.println("session reset detected — chime");
        sound_hal_play_reset();
    }
    if (g_after != g_before) {
        Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
            g_before, g_after, d.session_pct);
        if (splash_is_active()) splash_pick_for_current_rate();
    }
    ui_update(&d);
}

static void handle_usage() {
    String body = server.arg("plain");
    UsageData d{};
    if (usage_payload_parse(body.c_str(), body.length(), d)) {
        apply_usage(d);
        server.send(200, "text/plain", "ok");
    } else {
        server.send(400, "text/plain", "bad json");
    }
}

void net_init() {
    String ssid, pass;
    if (!wifi_creds_load(ssid, pass)) {
        state = NET_PROVISIONING;
        pump_ui(state);
        provisioning_run();          // never returns (reboots on save)
    }

    state = NET_CONNECTING;
    pump_ui(state);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("WiFi: connecting to '%s'\n", ssid.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(200);
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi: connect failed, re-provisioning");
        state = NET_PROVISIONING;
        pump_ui(state);
        provisioning_run();          // bad creds -> back to portal
    }

    Serial.printf("WiFi: online, IP=%s\n", WiFi.localIP().toString().c_str());

    // SNTP sync so claude_oauth's expiry math (time(nullptr)) has a real
    // wall clock. Async — claude_oauth guards against an unsynced clock by
    // refreshing proactively until time(nullptr) reports a plausible epoch.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    if (MDNS.begin("clawdmeter")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: clawdmeter.local");
    }

    server.on("/usage", HTTP_POST, handle_usage);
    server.on("/", HTTP_GET, []() {
        String ip = WiFi.localIP().toString();
        if (!claude_oauth_have_login()) {
            String page = String(
                "<html><body>"
                "<h1>Clawdmeter</h1>"
                "<p><a href=\"") + claude_oauth_authorize_url() + "\" target=\"_blank\">Log in to Claude</a></p>"
                "<form method=\"POST\" action=\"/login\">"
                "<p>Paste the code from the Anthropic page:</p>"
                "<input type=\"text\" name=\"code\">"
                "<input type=\"submit\" value=\"Submit\">"
                "</form>"
                "</body></html>";
            server.send(200, "text/html", page);
        } else {
            String page = String(
                "<html><body>"
                "<h1>Clawdmeter</h1>"
                "<p>Logged in &mdash; usage shows on the device.</p>"
                "<p>IP=") + ip + "</p>"
                "<form method=\"POST\" action=\"/logout\">"
                "<input type=\"submit\" value=\"Log out\">"
                "</form>"
                "</body></html>";
            server.send(200, "text/html", page);
        }
    });
    server.on("/login", HTTP_POST, []() {
        String code = server.arg("code");
        bool ok = claude_oauth_exchange(code);
        if (ok) {
            server.send(200, "text/plain", "Logged in - return to the device");
        } else {
            server.send(400, "text/plain", "Login failed, try again");
        }
    });
    server.on("/logout", HTTP_POST, []() {
        claude_oauth_logout();
        server.send(200, "text/plain", "Logged out");
    });
    server.begin();
    state = claude_oauth_have_login() ? NET_ONLINE_OK : NET_ONLINE_NEEDS_LOGIN;
}

void net_tick() {
    if (state == NET_PROVISIONING) return;
    if (WiFi.status() != WL_CONNECTED) {
        state = NET_CONNECTING;
        WiFi.reconnect();
        return;
    }
    state = claude_oauth_have_login() ? NET_ONLINE_OK : NET_ONLINE_NEEDS_LOGIN;
    server.handleClient();
}

NetState  net_state() { return state; }
IPAddress net_ip()    { return WiFi.localIP(); }
