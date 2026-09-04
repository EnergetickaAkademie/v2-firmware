#include "StatusLedManager.h"

#include "Config.h"

namespace {
constexpr uint32_t BOOT_INDICATION_MS = 1500;
constexpr uint32_t SUBSTATION_GRACE_MS = 6000;
constexpr uint32_t API_STALE_MS = 5000;
constexpr uint8_t API_FAILURE_THRESHOLD = 3;
constexpr uint32_t NFC_EVENT_MS = 1200;
constexpr uint32_t NFC_MISSING_REPEAT_MS = 10000;
constexpr uint32_t OTA_FAILURE_INDICATION_MS = 10000;

struct Color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

constexpr Color OFF = {0, 0, 0};
constexpr Color WHITE = {255, 255, 255};
constexpr Color BLUE = {0, 70, 255};
constexpr Color PURPLE = {160, 0, 255};
constexpr Color ORANGE = {255, 70, 0};
constexpr Color RED = {255, 0, 0};
constexpr Color PINK = {255, 0, 100};
constexpr Color CYAN = {0, 255, 255};
constexpr Color YELLOW = {255, 180, 0};
constexpr Color LIGHT_GREEN = {80, 255, 100};
constexpr Color MEDIUM_GREEN = {20, 190, 60};
constexpr Color DARK_GREEN = {0, 100, 25};
constexpr Color TEAL = {0, 255, 160};

struct SharedStatus {
    bool wifiConnected = false;
    bool debugMode = false;
    bool wifiEverConnected = false;
    bool boardRegistered = false;
    bool mqttHealthy = false;
    bool apiEverSucceeded = false;
    bool nfcKnown = false;
    bool nfcAvailable = false;
    uint8_t onlineSubstations = 0;
    uint8_t consecutiveApiFailures = 0;
    uint32_t lastApiSuccessMs = 0;
    uint32_t otaStateSinceMs = 0;
    uint32_t nfcEventSinceMs = 0;
    StatusApiError apiError = StatusApiError::None;
    StatusOtaState otaState = StatusOtaState::Idle;
    StatusNfcEvent nfcEvent = StatusNfcEvent::None;
};

constexpr uint32_t LED_FRAME_INTERVAL_MS = 20;
uint32_t lastFrameMs = 0;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
SharedStatus sharedStatus;
uint32_t setupStartedAtMs = 0;
Color lastRendered = {1, 1, 1};

uint8_t triangleLevel(uint32_t now, uint32_t period, uint8_t minimum) {
    const uint32_t halfPeriod = period / 2;
    uint32_t position = now % period;
    if (position > halfPeriod) position = period - position;

    return minimum + static_cast<uint8_t>(
        (static_cast<uint32_t>(255 - minimum) * position) / halfPeriod);
}

Color scaled(Color color, uint8_t level) {
    return {
        static_cast<uint8_t>((static_cast<uint16_t>(color.red) * level) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(color.green) * level) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(color.blue) * level) / 255),
    };
}

Color breathing(Color color, uint32_t now, uint32_t period, uint8_t minimum = 24) {
    return scaled(color, triangleLevel(now, period, minimum));
}

bool doubleFlashOn(uint32_t now, uint32_t period = 2000) {
    const uint32_t phase = now % period;
    return phase < 150 || (phase >= 300 && phase < 450);
}

bool tripleFlashOn(uint32_t now, uint32_t period = 2000) {
    const uint32_t phase = now % period;
    return phase < 120 ||
           (phase >= 240 && phase < 360) ||
           (phase >= 480 && phase < 600);
}

void render(Color color) {
    if (color.red == lastRendered.red &&
        color.green == lastRendered.green &&
        color.blue == lastRendered.blue) {
        return;
    }

    lastRendered = color;
    const Color output = scaled(color, STATUS_RGB_LED_BRIGHTNESS);
    neopixelWrite(STATUS_RGB_LED_PIN, output.red, output.green, output.blue);
}

SharedStatus snapshotStatus() {
    portENTER_CRITICAL(&statusMux);
    SharedStatus snapshot = sharedStatus;
    portEXIT_CRITICAL(&statusMux);
    return snapshot;
}
} // namespace

void statusLedSetup() {
    setupStartedAtMs = millis();
    render(WHITE);
}

void statusLedUpdate() {
    const uint32_t now = millis();
    if (now - lastFrameMs < LED_FRAME_INTERVAL_MS) {
        return;
    }
    lastFrameMs = now;

    const SharedStatus status = snapshotStatus();

    if (status.otaState == StatusOtaState::Uploading) {
        render(breathing(CYAN, now, 500, 40));
        return;
    }

    if (status.debugMode) {
        render(breathing(TEAL, now, 900, 35));
        return;
    }

    if (status.otaState == StatusOtaState::Succeeded) {
        render(((now / 150) % 2) == 0 ? WHITE : LIGHT_GREEN);
        return;
    }

    if (status.otaState == StatusOtaState::Failed &&
        now - status.otaStateSinceMs < OTA_FAILURE_INDICATION_MS) {
        render(((now / 250) % 2) == 0 ? CYAN : RED);
        return;
    }

    if (now - setupStartedAtMs < BOOT_INDICATION_MS) {
        render(breathing(WHITE, now, 1200, 30));
        return;
    }

    if (!status.wifiConnected) {
        if (status.wifiEverConnected) {
            render(doubleFlashOn(now) ? BLUE : OFF);
        } else {
            render(breathing(BLUE, now, 1800, 24));
        }
        return;
    }

    if (status.apiError == StatusApiError::Authentication ||
        status.apiError == StatusApiError::Registration) {
        render(tripleFlashOn(now) ? ORANGE : OFF);
        return;
    }

    const bool apiStale = status.boardRegistered &&
                          status.apiEverSucceeded &&
                          now - status.lastApiSuccessMs > API_STALE_MS;
    if (status.consecutiveApiFailures >= API_FAILURE_THRESHOLD || apiStale) {
        if (status.apiError == StatusApiError::InvalidResponse) {
            render(((now / 250) % 2) == 0 ? PURPLE : RED);
        } else {
            render(doubleFlashOn(now) ? PURPLE : OFF);
        }
        return;
    }

    if (!status.boardRegistered) {
        render(breathing(PURPLE, now, 1800, 24));
        return;
    }

    if (status.mqttHealthy) {
        render(CYAN);
        return;
    }

    const uint32_t nfcEventAge = now - status.nfcEventSinceMs;
    if (status.nfcEvent != StatusNfcEvent::None && nfcEventAge < NFC_EVENT_MS) {
        const bool on = doubleFlashOn(nfcEventAge, NFC_EVENT_MS);
        render(on ? (status.nfcEvent == StatusNfcEvent::Accepted ? LIGHT_GREEN : RED) : OFF);
        return;
    }

    if (status.nfcKnown && !status.nfcAvailable) {
        const uint32_t phase = now % NFC_MISSING_REPEAT_MS;
        if (phase < 600) {
            render(tripleFlashOn(phase, NFC_MISSING_REPEAT_MS) ? PINK : OFF);
            return;
        }
    }

    if (now - setupStartedAtMs < SUBSTATION_GRACE_MS) {
        render(breathing(WHITE, now, 1200, 30));
        return;
    }

    switch (status.onlineSubstations) {
        case 0:
            render(breathing(YELLOW, now, 1800, 30));
            break;
        case 1:
            render(LIGHT_GREEN);
            break;
        case 2:
            render(MEDIUM_GREEN);
            break;
        default:
            render(DARK_GREEN);
            break;
    }
}

void statusLedSetWifiConnected(bool connected) {
    portENTER_CRITICAL(&statusMux);
    sharedStatus.wifiConnected = connected;
    if (connected) sharedStatus.wifiEverConnected = true;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedSetBoardRegistered(bool registered) {
    portENTER_CRITICAL(&statusMux);
    sharedStatus.boardRegistered = registered;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedSetMqttHealthy(bool healthy) {
    portENTER_CRITICAL(&statusMux);
    sharedStatus.mqttHealthy = healthy;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedRecordApiSuccess() {
    const uint32_t now = millis();
    portENTER_CRITICAL(&statusMux);
    sharedStatus.apiEverSucceeded = true;
    sharedStatus.lastApiSuccessMs = now;
    sharedStatus.consecutiveApiFailures = 0;
    sharedStatus.apiError = StatusApiError::None;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedRecordApiFailure(StatusApiError error) {
    if (error == StatusApiError::None) return;

    portENTER_CRITICAL(&statusMux);
    if (sharedStatus.consecutiveApiFailures < UINT8_MAX) {
        ++sharedStatus.consecutiveApiFailures;
    }
    sharedStatus.apiError = error;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedSetSubstationCount(uint8_t count) {
    portENTER_CRITICAL(&statusMux);
    sharedStatus.onlineSubstations = count > 3 ? 3 : count;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedSetNfcAvailable(bool available) {
    portENTER_CRITICAL(&statusMux);
    sharedStatus.nfcKnown = true;
    sharedStatus.nfcAvailable = available;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedNotifyNfcEvent(StatusNfcEvent event) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&statusMux);
    sharedStatus.nfcEvent = event;
    sharedStatus.nfcEventSinceMs = now;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedSetOtaState(StatusOtaState state) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&statusMux);
    sharedStatus.otaState = state;
    sharedStatus.otaStateSinceMs = now;
    portEXIT_CRITICAL(&statusMux);
}

void statusLedSetDebugMode(bool active) {
    portENTER_CRITICAL(&statusMux);
    sharedStatus.debugMode = active;
    portEXIT_CRITICAL(&statusMux);
}
