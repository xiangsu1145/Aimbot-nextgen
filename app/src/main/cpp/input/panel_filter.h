// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng input — touch-panel name blacklist
//
//  Devices that advertise ABS_MT_SLOT/X/Y (sometimes even INPUT_PROP_DIRECT)
//  but are NOT the phone's built-in touchscreen. Both discovery paths — live
//  /dev/input enumeration (touch_reader) and the `getevent -p` text fallback
//  (uinput_inject) — drop any candidate whose EVIOCGNAME contains one of these
//  substrings. Without it the first entry below outranks the real panel,
//  because "event12" sorts before "event7" and it would be grabbed instead.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstring>

inline bool isBlacklistedPanelName(const char* name) {
    static constexpr const char* kBlacklist[] = {
        "HUAWEI Band",  // Bluetooth band acting as a direct-touch remote
    };
    if (!name) return false;
    for (const char* needle : kBlacklist) {
        if (strstr(name, needle) != nullptr) return true;
    }
    return false;
}
