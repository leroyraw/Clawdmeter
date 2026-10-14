#include "pkce.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include <esp_system.h>
#include <vector>

String base64url_encode(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::vector<uint8_t> out(olen + 1, 0);
    mbedtls_base64_encode(out.data(), out.size(), &olen, data, len);
    String s((char*)out.data());
    s.replace('+', '-'); s.replace('/', '_');
    while (s.endsWith("=")) s.remove(s.length() - 1);
    return s;
}

String pkce_make_verifier() {
    uint8_t rnd[32]; esp_fill_random(rnd, sizeof(rnd));
    return base64url_encode(rnd, sizeof(rnd));
}

String pkce_challenge(const String& verifier) {
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t*)verifier.c_str(), verifier.length(), hash, 0);
    return base64url_encode(hash, sizeof(hash));
}

bool pkce_selftest() {
    // RFC 7636 Appendix B vector.
    String v = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    return pkce_challenge(v) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
}
