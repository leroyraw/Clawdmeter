// firmware/src/wifi_creds.h
#pragma once
#include <Arduino.h>
bool wifi_creds_load(String& ssid, String& pass);
void wifi_creds_save(const String& ssid, const String& pass);
void wifi_creds_clear();
