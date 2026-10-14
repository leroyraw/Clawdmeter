#pragma once
#include <Arduino.h>
#include <vector>
#include <utility>

// Result of an HTTPS POST via https_post_json().
// status: HTTP status code (e.g. 200, 401), or a negative HTTPClient error
//         code (see HTTPClient.h's HTTPC_ERROR_* constants) if the request
//         itself failed (DNS, TLS handshake, connect, etc).
// body: response body (only populated when status > 0).
// header_values: values for the headers named in `capture_headers`, in the
//         same order (empty string if a requested header wasn't present).
struct HttpsResult { int status = -1; String body; std::vector<String> header_values; };

// POST `json_body` to `url` over TLS (WiFiClientSecure + HTTPClient), with
// `headers` added as request headers. If `capture_headers` is non-empty,
// the named response headers are collected and returned via
// HttpsResult::header_values (same order).
//
// v1 skips certificate verification (client.setInsecure()) — acceptable for
// now since traffic is the user's own outbound request to Anthropic's API
// from their own device. Hardening follow-up: bundle the Anthropic root CA
// and verify against it.
HttpsResult https_post_json(const char* url,
                            const std::vector<std::pair<String,String>>& headers,
                            const String& json_body,
                            const std::vector<String>& capture_headers = {});
