// ============================================================
//  secrets.h  –  DeskBuddy WiFi credentials
//
//  !! ADD THIS FILE TO .gitignore !!
//  Never commit this file to a public repository.
//
//  NOTE (v2.0): WiFi credentials are now stored in ESP32
//  NVS flash via the first-boot Captive Portal setup wizard.
//  You no longer need to hardcode them here.
//
//  This file is only needed if you want to skip the captive
//  portal and flash credentials directly. In that case, set
//  USE_HARDCODED_WIFI to 1 in the .ino file and fill in below.
//
//  Normal setup flow (recommended):
//    1. Flash the firmware (no WiFi credentials needed in code)
//    2. Connect your phone/laptop to "DeskBuddy-Setup" WiFi
//    3. A setup page appears — enter your home WiFi details
//    4. DeskBuddy saves them to flash and reboots automatically
//    5. To reset: tap "Reset WiFi" in the Settings tab of the UI
// ============================================================

#ifndef SECRETS_H
#define SECRETS_H

// Only used if USE_HARDCODED_WIFI is defined in the .ino
const char* FALLBACK_SSID     = "wifi name";   // ← your WiFi name
const char* FALLBACK_PASSWORD = "wifi pass";   // ← your WiFi password

#endif // SECRETS_H
