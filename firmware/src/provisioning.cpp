// firmware/src/provisioning.cpp
#include "provisioning.h"
#include "wifi_creds.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <lvgl.h>

const char* PROVISION_AP_SSID = "Clawdmeter-Setup";

static WebServer  server(80);
static DNSServer  dns;

static String page() {
    String opts;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        opts += "<option value='" + s + "'>" + s + "</option>";
    }
    return String("<!doctype html><meta name=viewport content='width=device-width'>"
        "<h2>Clawdmeter WiFi setup</h2>"
        "<form method=POST action=/save>"
        "<p>Network:<br><select name=ssid>") + opts + "</select>"
        "<p>Password:<br><input name=pass type=password>"
        "<p><button type=submit>Save &amp; connect</button></form>";
}

void provisioning_run() {
    WiFi.mode(WIFI_AP_STA);          // STA active so scanNetworks() works
    WiFi.softAP(PROVISION_AP_SSID);  // open network
    IPAddress ip = WiFi.softAPIP();
    dns.start(53, "*", ip);          // captive: resolve every host to us

    server.onNotFound([]() {         // captive-portal redirect
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString());
        server.send(302, "text/plain", "");
    });
    server.on("/", []() { server.send(200, "text/html", page()); });
    server.on("/save", HTTP_POST, []() {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");
        if (ssid.length()) {
            wifi_creds_save(ssid, pass);
            server.send(200, "text/html",
                        "<h3>Saved. Rebooting to connect&hellip;</h3>");
            delay(1500);
            ESP.restart();
        } else {
            server.send(400, "text/html", "<h3>Pick a network.</h3>");
        }
    });
    server.begin();
    Serial.printf("Provisioning: join WiFi '%s' then open http://%s\n",
                  PROVISION_AP_SSID, ip.toString().c_str());

    // Pump LVGL each iteration so the setup-screen hint painted by
    // net_init()'s pump_ui() stays live (animations, idle handling) for as
    // long as the captive portal blocks here — it never returns except to
    // reboot after creds are saved.
    for (;;) { dns.processNextRequest(); server.handleClient(); lv_timer_handler(); delay(2); }
}
