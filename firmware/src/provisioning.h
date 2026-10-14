// firmware/src/provisioning.h
#pragma once
extern const char* PROVISION_AP_SSID;   // "Clawdmeter-Setup"
void provisioning_run();                 // starts SoftAP + portal; restarts on save
