#ifndef STATUS_LED_MANAGER_H
#define STATUS_LED_MANAGER_H

#include <Arduino.h>

enum class StatusApiError : uint8_t {
    None,
    Unreachable,
    Authentication,
    Registration,
    InvalidResponse,
};

enum class StatusOtaState : uint8_t {
    Idle,
    Uploading,
    Succeeded,
    Failed,
};

enum class StatusNfcEvent : uint8_t {
    None,
    Accepted,
    Rejected,
};

void statusLedSetup();
void statusLedUpdate();

void statusLedSetWifiConnected(bool connected);
void statusLedSetBoardRegistered(bool registered);
void statusLedSetMqttHealthy(bool healthy);
void statusLedRecordApiSuccess();
void statusLedRecordApiFailure(StatusApiError error);
void statusLedSetSubstationCount(uint8_t count);
void statusLedSetNfcAvailable(bool available);
void statusLedNotifyNfcEvent(StatusNfcEvent event);
void statusLedSetOtaState(StatusOtaState state);
void statusLedSetDebugMode(bool active);

#endif
