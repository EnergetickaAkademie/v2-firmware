#ifndef NFC_DEBUG_H
#define NFC_DEBUG_H

#include <Arduino.h>

enum class NfcDebugMode : uint8_t {
    Normal = 0,
    Inspect = 1,
    Write = 2,
};

enum class NfcDebugProfileKind : uint8_t {
    None = 0,
    Building = 1,
    Reset = 2,
    Debug = 3,
    Wifi = 4,
};

struct NfcDebugProfile {
    NfcDebugProfileKind kind = NfcDebugProfileKind::None;
    uint8_t buildingType = 0;
    uint8_t wifiSecurity = 0;
    String wifiSsid;
    String wifiPassword;
};

struct NfcDebugEvent {
    String uid;
    String technology;
    String recordType;
    String protocol;
    String detail;
    uint8_t buildingType = 255;
    uint32_t ageMs = 0;
    bool success = false;
    bool write = false;
};

void nfcDebugSetMode(NfcDebugMode mode);
NfcDebugMode nfcDebugMode();
void nfcDebugSetProfile(const NfcDebugProfile& profile);
NfcDebugProfile nfcDebugProfile();
bool nfcDebugSetArmed(bool armed);
bool nfcDebugArmed();
void nfcDebugClearTagEdge();
void nfcDebugRecordEvent(const NfcDebugEvent& event);
bool nfcDebugCurrentTag(NfcDebugEvent& event);
size_t nfcDebugEventCount();
bool nfcDebugEventAt(size_t index, NfcDebugEvent& event);
uint32_t nfcDebugSuccessCount();
uint32_t nfcDebugFailureCount();

#endif
