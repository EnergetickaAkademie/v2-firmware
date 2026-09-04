#include "NfcDebug.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
constexpr size_t HISTORY_SIZE = 24;
SemaphoreHandle_t stateMutex = nullptr;
NfcDebugMode currentMode = NfcDebugMode::Normal;
NfcDebugProfile currentProfile;
bool armed = false;
bool hasCurrent = false;
NfcDebugEvent currentEvent;
uint32_t currentSeenAt = 0;
NfcDebugEvent history[HISTORY_SIZE];
uint32_t historySeenAt[HISTORY_SIZE] = {};
size_t historyCount = 0;
size_t historyHead = 0;
uint32_t successes = 0;
uint32_t failures = 0;

void ensureMutex() {
    if (stateMutex == nullptr) stateMutex = xSemaphoreCreateMutex();
}

struct Lock {
    Lock() { ensureMutex(); if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY); }
    ~Lock() { if (stateMutex) xSemaphoreGive(stateMutex); }
};
}

void nfcDebugSetMode(NfcDebugMode mode) {
    Lock lock;
    currentMode = mode;
    armed = false;
    hasCurrent = false;
}

NfcDebugMode nfcDebugMode() {
    Lock lock;
    return currentMode;
}

void nfcDebugSetProfile(const NfcDebugProfile& profile) {
    Lock lock;
    currentProfile = profile;
    armed = false;
}

NfcDebugProfile nfcDebugProfile() {
    Lock lock;
    return currentProfile;
}

bool nfcDebugSetArmed(bool nextArmed) {
    Lock lock;
    if (nextArmed && currentMode != NfcDebugMode::Write) return false;
    armed = nextArmed;
    hasCurrent = false;
    return true;
}

bool nfcDebugArmed() {
    Lock lock;
    return armed;
}

void nfcDebugClearTagEdge() {
    Lock lock;
    hasCurrent = false;
}

void nfcDebugRecordEvent(const NfcDebugEvent& event) {
    Lock lock;
    const uint32_t now = millis();
    NfcDebugEvent copy = event;
    copy.ageMs = 0;
    currentEvent = copy;
    currentSeenAt = now;
    hasCurrent = true;

    const size_t index = historyHead;
    history[index] = copy;
    historySeenAt[index] = now;
    historyHead = (historyHead + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) ++historyCount;
    if (copy.success) ++successes;
    else ++failures;
}

bool nfcDebugCurrentTag(NfcDebugEvent& event) {
    Lock lock;
    if (!hasCurrent) return false;
    event = currentEvent;
    event.ageMs = millis() - currentSeenAt;
    return true;
}

size_t nfcDebugEventCount() {
    Lock lock;
    return historyCount;
}

bool nfcDebugEventAt(size_t index, NfcDebugEvent& event) {
    Lock lock;
    if (index >= historyCount) return false;
    const size_t oldest = (historyHead + HISTORY_SIZE - historyCount) % HISTORY_SIZE;
    const size_t slot = (oldest + index) % HISTORY_SIZE;
    event = history[slot];
    event.ageMs = millis() - historySeenAt[slot];
    return true;
}

uint32_t nfcDebugSuccessCount() {
    Lock lock;
    return successes;
}

uint32_t nfcDebugFailureCount() {
    Lock lock;
    return failures;
}
