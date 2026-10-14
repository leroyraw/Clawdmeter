#include "https.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

HttpsResult https_post_json(const char* url,
                            const std::vector<std::pair<String,String>>& headers,
                            const String& json_body,
                            const std::vector<String>& capture_headers) {
    HttpsResult r;
    WiFiClientSecure client;
    client.setInsecure();               // v1: skip cert verify (LAN egress; user's own account).
                                        // Hardening follow-up: bundle the Anthropic root CA.
    HTTPClient http;
    if (!http.begin(client, url)) { r.status = -1; return r; }
    for (auto& h : headers) http.addHeader(h.first, h.second);
    if (!capture_headers.empty()) {
        const char* keys[16];
        size_t n = capture_headers.size(); if (n > 16) n = 16;
        for (size_t i = 0; i < n; i++) keys[i] = capture_headers[i].c_str();
        http.collectHeaders(keys, n);
    }
    r.status = http.POST((uint8_t*)json_body.c_str(), json_body.length());
    if (r.status > 0) r.body = http.getString();
    for (auto& name : capture_headers) r.header_values.push_back(http.header(name.c_str()));
    http.end();
    return r;
}
