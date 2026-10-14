#pragma once
#include <Arduino.h>

String base64url_encode(const uint8_t* data, size_t len);
String pkce_make_verifier();
String pkce_challenge(const String& verifier);
bool   pkce_selftest();   // true if RFC 7636 vector matches
