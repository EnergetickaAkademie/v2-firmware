#include "DebugPortal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include "Config.h"
#include "DebugLog.h"
#include "GameState.h"
#include "NetworkManager.h"
#include "NfcDebug.h"
#include "MainboardDebug.h"
#include "OtaManager.h"
#include "RuntimeConfig.h"
#include "StatusLedManager.h"
#include "SubstationManager.h"

#define Serial DebugLog

#include "DebugPortalHtml.h"

namespace {
WebServer portalServer(80);
DNSServer dnsServer;
volatile bool requested = false;
volatile bool exitRequested = false;
bool active = false;
bool restartPending = false;
uint32_t restartAtMs = 0;
String requestToken;

constexpr uint32_t DEBUG_OTA_QUIESCE_TIMEOUT_MS = 15000;
const esp_partition_t* stagedFirmwarePartition = nullptr;
esp_ota_handle_t firmwareUploadHandle = 0;
bool firmwareUploadHandleActive = false;
bool firmwareUploadAuthorized = false;
bool firmwareUploadActive = false;
bool firmwareUploadRequestActive = false;
bool firmwareBinarySeen = false;
bool firmwareChecksumSeen = false;
bool firmwareHashMatched = false;
bool stagedFirmwareReady = false;
size_t firmwareUploadBytes = 0;
String firmwareUploadFilename;
String firmwareUploadSha256;
String firmwareExpectedSha256;
String firmwareChecksumText;
String firmwareUploadError;
mbedtls_sha256_context firmwareUploadDigest;
bool firmwareUploadDigestActive = false;

enum class FirmwareUploadPart : uint8_t { None, Binary, Sha256 };
FirmwareUploadPart firmwareUploadPart = FirmwareUploadPart::None;

bool tokenValid();

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_NETMASK(255, 255, 255, 0);

void sendJson(int code, JsonDocument& document) {
    String body;
    serializeJson(document, body);
    portalServer.sendHeader("Cache-Control", "no-store");
    portalServer.send(code, "application/json", body);
}

void sendError(int code, const char* message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(code, doc);
}

String sha256Hex(const uint8_t digest[32]) {
    String result;
    result.reserve(64);
    const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        result += digits[(digest[i] >> 4) & 0x0f];
        result += digits[digest[i] & 0x0f];
    }
    return result;
}

bool isSha256Hex(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

String parseSha256File(const String& input) {
    String text = input;
    text.trim();
    size_t tokenLength = 0;
    while (tokenLength < text.length() && text.charAt(tokenLength) > ' ') {
        ++tokenLength;
    }
    if (tokenLength != 64) return "";
    for (size_t index = 0; index < tokenLength; ++index) {
        if (!isSha256Hex(text.charAt(index))) return "";
    }
    String result = text.substring(0, tokenLength);
    result.toLowerCase();
    return result;
}

void discardStagedFirmware() {
    stagedFirmwarePartition = nullptr;
    stagedFirmwareReady = false;
    firmwareUploadBytes = 0;
    firmwareUploadFilename = "";
    firmwareUploadSha256 = "";
}

void failFirmwareUpload(const String& message) {
    if (firmwareUploadHandleActive) {
        esp_ota_abort(firmwareUploadHandle);
        firmwareUploadHandleActive = false;
    }
    if (firmwareUploadDigestActive) {
        mbedtls_sha256_free(&firmwareUploadDigest);
        firmwareUploadDigestActive = false;
    }
    setOtaDebugUpdateInProgress(false);
    statusLedSetOtaState(StatusOtaState::Failed);
    firmwareUploadActive = false;
    firmwareUploadError = message;
    discardStagedFirmware();
}

bool waitForOtaTasks() {
    const uint32_t startedAt = millis();
    while (!otaTasksPaused() && millis() - startedAt < DEBUG_OTA_QUIESCE_TIMEOUT_MS) {
        delay(10);
    }
    return otaTasksPaused();
}

void handleFirmwareStatusGet() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }
    JsonDocument doc;
    doc["ready"] = stagedFirmwareReady;
    doc["filename"] = firmwareUploadFilename;
    doc["size"] = firmwareUploadBytes;
    doc["sha256"] = firmwareUploadSha256;
    doc["hash_provided"] = firmwareChecksumSeen;
    doc["hash_matched"] = firmwareHashMatched;
    if (firmwareUploadError.length() > 0) doc["error"] = firmwareUploadError;
    sendJson(200, doc);
}

void handleFirmwareUpdatePost() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }
    if (!stagedFirmwareReady || stagedFirmwarePartition == nullptr) {
        sendError(409, "Upload and verify a firmware image first");
        return;
    }
    if (isOtaInProgress()) {
        sendError(409, "Another firmware operation is already in progress");
        return;
    }
    if (esp_ota_set_boot_partition(stagedFirmwarePartition) != ESP_OK) {
        sendError(500, "Could not select the verified firmware partition");
        return;
    }

    JsonDocument response;
    response["ok"] = true;
    response["rebooting"] = true;
    response["sha256"] = firmwareUploadSha256;
    response["hash_matched"] = firmwareHashMatched;
    sendJson(200, response);
    statusLedSetOtaState(StatusOtaState::Succeeded);
    restartPending = true;
    restartAtMs = millis() + 700;
}

void handleFirmwareUploadFinished() {
    if (!firmwareUploadAuthorized) {
        sendError(401, "Invalid debug session token");
        firmwareUploadRequestActive = false;
        return;
    }
    if (firmwareUploadError.length() > 0) {
        sendError(400, firmwareUploadError.c_str());
        firmwareUploadRequestActive = false;
        firmwareUploadAuthorized = false;
        return;
    }
    if (!firmwareBinarySeen || stagedFirmwarePartition == nullptr) {
        firmwareUploadError = "A .bin firmware file is required";
        sendError(400, firmwareUploadError.c_str());
        firmwareUploadRequestActive = false;
        firmwareUploadAuthorized = false;
        return;
    }
    if (firmwareChecksumSeen) {
        firmwareExpectedSha256 = parseSha256File(firmwareChecksumText);
        if (firmwareExpectedSha256.length() == 0) {
            failFirmwareUpload("The .sha256 file does not contain a valid SHA-256 hash");
            sendError(400, firmwareUploadError.c_str());
            firmwareUploadRequestActive = false;
            firmwareUploadAuthorized = false;
            return;
        }
        firmwareHashMatched = firmwareExpectedSha256 == firmwareUploadSha256;
        if (!firmwareHashMatched) {
            failFirmwareUpload("The .sha256 hash does not match the uploaded firmware");
            sendError(400, firmwareUploadError.c_str());
            firmwareUploadRequestActive = false;
            firmwareUploadAuthorized = false;
            return;
        }
    }
    stagedFirmwareReady = true;

    JsonDocument response;
    response["ok"] = true;
    response["verified"] = true;
    response["filename"] = firmwareUploadFilename;
    response["size"] = firmwareUploadBytes;
    response["sha256"] = firmwareUploadSha256;
    response["hash_provided"] = firmwareChecksumSeen;
    response["hash_matched"] = firmwareHashMatched;
    sendJson(200, response);
    firmwareUploadRequestActive = false;
    firmwareUploadAuthorized = false;
}

void handleFirmwareUploadChunk() {
    HTTPUpload& upload = portalServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        filename.toLowerCase();

        if (!firmwareUploadRequestActive) {
            firmwareUploadRequestActive = true;
            firmwareUploadAuthorized = tokenValid();
            firmwareUploadActive = false;
            firmwareBinarySeen = false;
            firmwareChecksumSeen = false;
            firmwareHashMatched = false;
            firmwareExpectedSha256 = "";
            firmwareChecksumText = "";
            firmwareUploadError = "";
            discardStagedFirmware();
            if (!firmwareUploadAuthorized) {
                firmwareUploadError = "Invalid debug session token";
                return;
            }
        }
        if (!firmwareUploadAuthorized) return;
        if (filename.endsWith(".sha256")) {
            if (firmwareChecksumSeen) {
                firmwareUploadError = "Only one .sha256 file may be uploaded";
                return;
            }
            firmwareChecksumSeen = true;
            firmwareChecksumText = "";
            firmwareUploadPart = FirmwareUploadPart::Sha256;
            return;
        }
        if (!filename.endsWith(".bin")) {
            firmwareUploadError = "Upload a .bin firmware file and optionally a .sha256 file";
            return;
        }
        if (firmwareBinarySeen || isOtaInProgress()) {
            firmwareUploadError = firmwareBinarySeen
                ? "Only one .bin firmware file may be uploaded"
                : "Another firmware operation is already in progress";
            return;
        }

        firmwareBinarySeen = true;
        firmwareUploadFilename = upload.filename;
        firmwareUploadPart = FirmwareUploadPart::Binary;

        const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
        if (partition == nullptr) {
            firmwareUploadError = "No inactive OTA partition is available";
            return;
        }

        stagedFirmwarePartition = partition;
        setOtaDebugUpdateInProgress(true);
        statusLedSetOtaState(StatusOtaState::Uploading);
        if (!waitForOtaTasks()) {
            failFirmwareUpload("Background tasks did not pause for firmware upload");
            return;
        }
        if (esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &firmwareUploadHandle) != ESP_OK) {
            failFirmwareUpload("Could not open the inactive OTA partition");
            return;
        }
        firmwareUploadHandleActive = true;
        mbedtls_sha256_init(&firmwareUploadDigest);
        if (mbedtls_sha256_starts_ret(&firmwareUploadDigest, 0) != 0) {
            failFirmwareUpload("Could not initialize SHA-256 verification");
            return;
        }
        firmwareUploadDigestActive = true;
        firmwareUploadActive = true;
        return;
    }

    if (!firmwareUploadAuthorized) return;

    if (firmwareUploadPart == FirmwareUploadPart::Sha256) {
        if (upload.status == UPLOAD_FILE_WRITE) {
            if (firmwareChecksumText.length() + upload.currentSize > 256) {
                firmwareUploadError = "The .sha256 file is too large";
                return;
            }
            for (size_t index = 0; index < upload.currentSize; ++index) {
                firmwareChecksumText += static_cast<char>(upload.buf[index]);
            }
        }
        return;
    }

    if (firmwareUploadPart != FirmwareUploadPart::Binary || !firmwareUploadActive) return;

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (firmwareUploadBytes + upload.currentSize > stagedFirmwarePartition->size) {
            failFirmwareUpload("Firmware image is larger than the inactive OTA partition");
            return;
        }
        if (esp_ota_write(firmwareUploadHandle, upload.buf, upload.currentSize) != ESP_OK ||
            mbedtls_sha256_update_ret(&firmwareUploadDigest, upload.buf, upload.currentSize) != 0) {
            failFirmwareUpload("Firmware upload or SHA-256 calculation failed");
            return;
        }
        firmwareUploadBytes += upload.currentSize;
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        uint8_t digest[32] = {};
        if (firmwareUploadBytes == 0 ||
            mbedtls_sha256_finish_ret(&firmwareUploadDigest, digest) != 0) {
            failFirmwareUpload("Firmware image is empty or SHA-256 verification failed");
            return;
        }
        mbedtls_sha256_free(&firmwareUploadDigest);
        firmwareUploadDigestActive = false;
        firmwareUploadSha256 = sha256Hex(digest);

        if (esp_ota_end(firmwareUploadHandle) != ESP_OK) {
            firmwareUploadHandleActive = false;
            failFirmwareUpload("Firmware image validation failed");
            return;
        }
        firmwareUploadHandleActive = false;
        firmwareUploadActive = false;
        setOtaDebugUpdateInProgress(false);
        statusLedSetOtaState(StatusOtaState::Idle);
        firmwareUploadError = "";
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        failFirmwareUpload("Firmware upload was aborted");
    }
}

void redirectToPortal() {
    portalServer.sendHeader("Location", "http://enak.local/", true);
    portalServer.send(302, "text/plain", "Redirecting to ENAK Debug Portal");
}

bool tokenValid() {
    return requestToken.length() > 0 &&
           portalServer.header("X-Debug-Token") == requestToken;
}

String makeToken() {
    char token[33];
    snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    return String(token);
}

String boardApName() {
    String identity = runtimeBoardUsername();
    if (identity.length() == 0) {
        char fallback[16];
        snprintf(fallback, sizeof(fallback), "%08lx",
                 static_cast<unsigned long>(ESP.getEfuseMac() & 0xffffffffULL));
        identity = fallback;
    }
    String result = "ENAK-" + identity;
    if (result.length() > 32) result.remove(32);
    return result;
}

const char* nfcModeName(NfcDebugMode mode) {
    switch (mode) {
        case NfcDebugMode::Inspect: return "inspect";
        case NfcDebugMode::Write: return "write";
        default: return "normal";
    }
}

const char* nfcProfileName(NfcDebugProfileKind kind) {
    switch (kind) {
        case NfcDebugProfileKind::Building: return "building";
        case NfcDebugProfileKind::Reset: return "reset";
        case NfcDebugProfileKind::Debug: return "debug";
        case NfcDebugProfileKind::Wifi: return "wifi";
        default: return "none";
    }
}

bool parseNfcProfileKind(const String& value, NfcDebugProfileKind& kind) {
    if (value == "building") kind = NfcDebugProfileKind::Building;
    else if (value == "reset") kind = NfcDebugProfileKind::Reset;
    else if (value == "debug") kind = NfcDebugProfileKind::Debug;
    else if (value == "wifi") kind = NfcDebugProfileKind::Wifi;
    else return false;
    return true;
}

void addNfcEvent(JsonObject object, const NfcDebugEvent& event) {
    object["uid"] = event.uid;
    object["technology"] = event.technology;
    object["record_type"] = event.recordType;
    object["protocol"] = event.protocol;
    object["detail"] = event.detail;
    object["building_type"] = event.buildingType;
    object["age_ms"] = event.ageMs;
    object["success"] = event.success;
    object["write"] = event.write;
}

void handleNfcStatusGet() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }
    JsonDocument doc;
    doc["mode"] = nfcModeName(nfcDebugMode());
    doc["armed"] = nfcDebugArmed();
    doc["success_count"] = nfcDebugSuccessCount();
    doc["failure_count"] = nfcDebugFailureCount();
    const NfcDebugProfile profile = nfcDebugProfile();
    JsonObject profileObject = doc["profile"].to<JsonObject>();
    profileObject["kind"] = nfcProfileName(profile.kind);
    profileObject["building_type"] = profile.buildingType;
    profileObject["wifi_security"] = profile.wifiSecurity;
    profileObject["wifi_ssid"] = profile.wifiSsid;
    profileObject["wifi_password_set"] = profile.wifiPassword.length() > 0;

    NfcDebugEvent current;
    if (nfcDebugCurrentTag(current)) {
        JsonObject object = doc["current_tag"].to<JsonObject>();
        addNfcEvent(object, current);
    }
    JsonArray events = doc["history"].to<JsonArray>();
    for (size_t i = 0; i < nfcDebugEventCount(); ++i) {
        NfcDebugEvent event;
        if (nfcDebugEventAt(i, event)) {
            JsonObject object = events.add<JsonObject>();
            addNfcEvent(object, event);
        }
    }
    sendJson(200, doc);
}

void handleBuildingsGet() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }

    JsonDocument doc;
    JsonArray buildings = doc["buildings"].to<JsonArray>();
    const std::vector<ScannedBuilding> snapshot = scannedBuildingsSnapshot();
    for (const auto& building : snapshot) {
        JsonObject object = buildings.add<JsonObject>();
        object["uid"] = building.uid;
        object["building_type"] = building.type;
    }
    sendJson(200, doc);
}

void handleBuildingRemovePost() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }

    JsonDocument doc;
    if (deserializeJson(doc, portalServer.arg("plain"))) {
        sendError(400, "Request must be valid JSON");
        return;
    }
    const String uid = doc["uid"].as<String>();
    if (uid.length() == 0 || uid.length() > 64) {
        sendError(400, "Building UID must be 1-64 characters");
        return;
    }

    setOtaDebugUpdateInProgress(true);
    if (!waitForOtaTasks()) {
        setOtaDebugUpdateInProgress(false);
        sendError(503, "Background tasks did not pause for building removal");
        return;
    }
    const bool removedFromServer = removeBuildingFromServer(uid);
    if (!removedFromServer) {
        setOtaDebugUpdateInProgress(false);
        sendError(502, "Could not remove building from CoreAPI; it was not removed locally");
        return;
    }

    const bool removedLocally = removeScannedBuilding(uid);
    setOtaDebugUpdateInProgress(false);
    JsonDocument response;
    response["ok"] = true;
    response["uid"] = uid;
    response["removed_from_scan"] = removedLocally;
    sendJson(200, response);
}

void handleNfcModePost() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, portalServer.arg("plain"))) { sendError(400, "Request must be valid JSON"); return; }
    const String mode = doc["mode"].as<String>();
    if (mode == "normal") nfcDebugSetMode(NfcDebugMode::Normal);
    else if (mode == "inspect") nfcDebugSetMode(NfcDebugMode::Inspect);
    else if (mode == "write") nfcDebugSetMode(NfcDebugMode::Write);
    else { sendError(400, "NFC mode must be normal, inspect, or write"); return; }
    JsonDocument response; response["ok"] = true; sendJson(200, response);
}

void handleNfcProfilePost() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, portalServer.arg("plain"))) { sendError(400, "Request must be valid JSON"); return; }
    NfcDebugProfile profile;
    if (!parseNfcProfileKind(doc["kind"].as<String>(), profile.kind)) {
        sendError(400, "Unknown NFC write profile"); return;
    }
    profile.buildingType = doc["building_type"].as<int>();
    profile.wifiSecurity = doc["wifi_security"].as<int>();
    profile.wifiSsid = doc["wifi_ssid"].as<String>();
    profile.wifiPassword = doc["wifi_password"].as<String>();
    if (profile.kind == NfcDebugProfileKind::Building && profile.buildingType >= BUILDING_COUNT) {
        sendError(400, "Building type is out of range"); return;
    }
    if (profile.kind == NfcDebugProfileKind::Wifi) {
        if (profile.wifiSsid.length() == 0 || profile.wifiSsid.length() > 32 ||
            profile.wifiSecurity > 1 || profile.wifiPassword.length() > 63 ||
            (profile.wifiSecurity == 1 && profile.wifiPassword.length() < 8) ||
            (profile.wifiSecurity == 0 && profile.wifiPassword.length() != 0)) {
            sendError(400, "Invalid Wi-Fi SSID, security, or password"); return;
        }
    }
    nfcDebugSetProfile(profile);
    JsonDocument response; response["ok"] = true; sendJson(200, response);
}

void handleNfcArmPost() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, portalServer.arg("plain"))) { sendError(400, "Request must be valid JSON"); return; }
    const bool armed = doc["armed"].as<bool>();
    const NfcDebugProfile profile = nfcDebugProfile();
    if (armed && profile.kind == NfcDebugProfileKind::None) {
        sendError(400, "Select a write profile before arming"); return;
    }
    if (!nfcDebugSetArmed(armed)) { sendError(400, "Writer can only be armed in write mode"); return; }
    JsonDocument response; response["ok"] = true; response["armed"] = armed; sendJson(200, response);
}

void stopPortal() {
    nfcDebugSetMode(NfcDebugMode::Normal);
    portalServer.stop();
    dnsServer.stop();
    MDNS.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    active = false;
    requestToken = "";
    statusLedSetDebugMode(false);
    Serial.println("[Debug] Portal stopped; normal NFC mode restored.");
}

bool validBoardUsername(const String& value) {
    if (value.length() == 0 || value.length() > 64) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

bool validHostname(const String& value) {
    if (value.length() == 0 || value.length() > 63 ||
        value[0] == '-' || value[value.length() - 1] == '-') return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

void handleConfigGet() {
    ActiveWifiConfig wifi;
    if (!getActiveWifiConfig(wifi)) {
        sendError(503, "Wi-Fi configuration is unavailable");
        return;
    }
    JsonDocument doc;
    doc["board_username"] = runtimeBoardUsername();
    doc["api_base_url"] = runtimeApiBaseUrl();
    doc["wifi_ssid"] = wifi.ssid;
    doc["wifi_security"] = wifi.security;
    doc["wifi_password_set"] = wifi.passwordSet;
    doc["board_password_set"] = runtimeBoardPassword().length() > 0;
    doc["ota_password_set"] = runtimeOtaPassword().length() >= 8;
    doc["debug_ap_password_set"] = runtimeDebugApPassword().length() >= 8;
    doc["ota_hostname"] = runtimeOtaHostname();
    doc["ota_port"] = runtimeOtaPort();
    sendJson(200, doc);
}

void handleStatusGet() {
    SubstationSnapshot snapshots[3] = {};
    getSubstationSnapshots(snapshots);
    JsonDocument doc;
    doc["board_name"] = runtimeBoardUsername().length() > 0
        ? runtimeBoardUsername() : boardApName();
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["uptime_ms"] = millis();
    doc["online_substations"] = getOnlineSubstationCount();
    JsonArray stations = doc["substations"].to<JsonArray>();
    for (size_t i = 0; i < 3; ++i) {
        JsonObject station = stations.add<JsonObject>();
        station["online"] = snapshots[i].online;
        station["last_seen_age_ms"] = snapshots[i].lastSeenAgeMs;
        JsonArray plants = station["plants"].to<JsonArray>();
        for (size_t j = 0; j < 8; ++j) plants.add(snapshots[i].counts[j]);
    }
    JsonArray totals = doc["plant_totals"].to<JsonArray>();
    for (size_t type = 1; type <= 8; ++type) totals.add(connectedCount[type]);
    const char* encoderLabels[] = {"Coal", "Hydro", "Storage", "Nuclear", "Gas"};
    JsonArray encoders = doc["encoders"].to<JsonArray>();
    for (uint8_t i = 0; i < 5; ++i) {
        int32_t minimum = 0;
        int32_t maximum = 0;
        getDebugEncoderRange(i, minimum, maximum);
        JsonObject encoder = encoders.add<JsonObject>();
        encoder["label"] = encoderLabels[i];
        encoder["value_mw"] = encoderValuesMW[i];
        encoder["minimum_mw"] = minimum;
        encoder["maximum_mw"] = maximum;
        encoder["percentage"] = encoderPercentages[i];
    }
    const char* displayLabels[] = {"Consumption", "Production", "Coal", "Hydro", "Storage", "Nuclear", "Gas", "Wind / PV"};
    JsonArray displays = doc["displays"].to<JsonArray>();
    for (uint8_t i = 0; i < 8; ++i) {
        JsonObject display = displays.add<JsonObject>();
        display["label"] = displayLabels[i];
        display["value"] = debugDisplayValues[i];
        display["visible"] = debugDisplayVisible[i];
    }
    sendJson(200, doc);
}

void handleEncoderControlPost() {
    if (!tokenValid()) { sendError(403, "Invalid debug session token"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, portalServer.arg("plain"))) { sendError(400, "Request must be valid JSON"); return; }
    const int index = doc["index"].as<int>();
    const int32_t value = doc["value_mw"].as<int32_t>();
    if (index < 0 || index >= 5 || !setDebugEncoderValue(static_cast<uint8_t>(index), value)) {
        sendError(400, "Encoder value is outside the active range");
        return;
    }
    JsonDocument response;
    response["ok"] = true;
    response["value_mw"] = value;
    sendJson(200, response);
}

void handleConfigPost() {
    if (!tokenValid()) {
        sendError(403, "Invalid debug session token");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, portalServer.arg("plain"))) {
        sendError(400, "Request must be valid JSON");
        return;
    }
    const String nextUsername = doc["board_username"].as<String>();
    String nextApi = doc["api_base_url"].as<String>();
    const String nextSsid = doc["wifi_ssid"].as<String>();
    const int nextSecurity = doc["wifi_security"].as<int>();
    const String wifiPassword = doc["wifi_password"].as<String>();
    const String nextHostname = doc["ota_hostname"].as<String>();
    const String boardPasswordInput = doc["board_password"].as<String>();
    const String otaPasswordInput = doc["ota_password"].as<String>();
    const String debugApPasswordInput = doc["debug_ap_password"].as<String>();
    const int nextPort = doc["ota_port"].as<int>();

    if (!validBoardUsername(nextUsername)) {
        sendError(400, "Board username must use 1-64 letters, digits, dot, dash, or underscore");
        return;
    }
    while (nextApi.endsWith("/")) nextApi.remove(nextApi.length() - 1);
    if (!(nextApi.startsWith("http://") || nextApi.startsWith("https://")) ||
        nextApi.length() == 0 || nextApi.length() > 255) {
        sendError(400, "API endpoint must be an HTTP or HTTPS URL");
        return;
    }
    if (nextSsid.length() == 0 || nextSsid.length() > 32 ||
        nextSecurity < 0 || nextSecurity > 1 || nextPort < 1 || nextPort > 65535 ||
        nextPort == 80 ||
        !validHostname(nextHostname)) {
        sendError(400, "Invalid Wi-Fi, OTA hostname, or port value (OTA port 80 is reserved)");
        return;
    }

    const String nextBoardPassword = boardPasswordInput.length() > 0
        ? boardPasswordInput : runtimeBoardPassword();
    const String nextOtaPassword = otaPasswordInput.length() > 0
        ? otaPasswordInput : runtimeOtaPassword();
    const String nextDebugApPassword = debugApPasswordInput.length() > 0
        ? debugApPasswordInput : runtimeDebugApPassword();
    if (nextBoardPassword.length() == 0 || nextBoardPassword.length() > 127 ||
        nextOtaPassword.length() < 8 || nextOtaPassword.length() > 127 ||
        nextDebugApPassword.length() < 8 || nextDebugApPassword.length() > 63) {
        sendError(400, "Board, OTA, or debug AP password is missing or invalid");
        return;
    }

    ActiveWifiConfig currentWifi;
    if (!getActiveWifiConfig(currentWifi)) {
        sendError(503, "Wi-Fi configuration is unavailable");
        return;
    }
    const bool wifiChanged = nextSsid != currentWifi.ssid ||
                             nextSecurity != currentWifi.security ||
                             wifiPassword.length() > 0;
    if (nextSecurity == 1 && wifiChanged &&
        wifiPassword.length() < 8) {
        sendError(400, "A new protected Wi-Fi network needs an 8-63 character password");
        return;
    }
    if (wifiPassword.length() > 63 || (nextSecurity == 0 && wifiPassword.length() > 0)) {
        sendError(400, "Invalid Wi-Fi password");
        return;
    }

    if (!saveRuntimeConfig(nextApi, nextUsername, nextBoardPassword,
                           nextOtaPassword, nextHostname,
                           static_cast<uint16_t>(nextPort), nextDebugApPassword)) {
        sendError(500, "Could not save device configuration");
        return;
    }
    if (wifiChanged) {
        const String candidatePassword = nextSecurity == 0 ? String() : wifiPassword;
        if (!queueWifiProvisioning(nextSsid, candidatePassword,
                                   static_cast<uint8_t>(nextSecurity))) {
            sendError(500, "Could not stage Wi-Fi configuration");
            return;
        }
    }

    JsonDocument response;
    response["ok"] = true;
    response["rebooting"] = true;
    sendJson(200, response);
    restartPending = true;
    restartAtMs = millis() + 700;
}

void handleExitPost() {
    if (!tokenValid()) {
        sendError(403, "Invalid debug session token");
        return;
    }
    JsonDocument response;
    response["ok"] = true;
    response["rebooting"] = false;
    sendJson(200, response);
    restartPending = false;
    stopPortal();
}

void beginPortal() {
    requested = false;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_NETMASK);
    const String& debugPassword = runtimeDebugApPassword();
    const char* password = debugPassword.length() >= 8 ? debugPassword.c_str() : "enak-debug";
    const String ssid = boardApName();
    const int channel = WiFi.status() == WL_CONNECTED ? WiFi.channel() : 1;
    if (!WiFi.softAP(ssid.c_str(), password, channel, false, 1)) {
        Serial.println("[Debug] Failed to start debug access point.");
        return;
    }

    requestToken = makeToken();
    const char* headers[] = {"X-Debug-Token"};
    portalServer.collectHeaders(headers, 1);
    portalServer.on("/", HTTP_GET, []() {
        String page = FPSTR(PORTAL_HTML);
        page.replace("__TOKEN__", requestToken);
        portalServer.sendHeader("Cache-Control", "no-store");
        portalServer.send(200, "text/html", page);
    });
    portalServer.on("/api/config", HTTP_GET, handleConfigGet);
    portalServer.on("/api/status", HTTP_GET, handleStatusGet);
    portalServer.on("/api/firmware/status", HTTP_GET, handleFirmwareStatusGet);
    portalServer.on("/api/firmware/update", HTTP_POST, handleFirmwareUpdatePost);
    portalServer.on("/api/firmware/upload", HTTP_POST, handleFirmwareUploadFinished,
                    handleFirmwareUploadChunk);
    portalServer.on("/api/encoder", HTTP_POST, handleEncoderControlPost);
    portalServer.on("/api/nfc/status", HTTP_GET, handleNfcStatusGet);
    portalServer.on("/api/buildings", HTTP_GET, handleBuildingsGet);
    portalServer.on("/api/buildings/remove", HTTP_POST, handleBuildingRemovePost);
    portalServer.on("/api/config", HTTP_POST, handleConfigPost);
    portalServer.on("/api/nfc/mode", HTTP_POST, handleNfcModePost);
    portalServer.on("/api/nfc/profile", HTTP_POST, handleNfcProfilePost);
    portalServer.on("/api/nfc/arm", HTTP_POST, handleNfcArmPost);
    portalServer.on("/api/exit", HTTP_POST, handleExitPost);
    portalServer.onNotFound(redirectToPortal);
    portalServer.begin();

    dnsServer.start(53, "*", AP_IP);
    MDNS.end();
    if (!MDNS.begin("enak")) Serial.println("[Debug] mDNS setup failed; use 192.168.4.1.");
    active = true;
    statusLedSetDebugMode(true);
    Serial.printf("[Debug] Portal active at http://enak.local/ (AP %s, IP %s).\n",
                  ssid.c_str(), WiFi.softAPIP().toString().c_str());
    tone(BUZZER_PIN, 1800, 120);
    delay(140);
    tone(BUZZER_PIN, 2400, 180);
}
} // namespace

void requestDebugPortal() { requested = true; }
void requestDebugPortalExit() { exitRequested = true; }
bool isDebugPortalRequested() { return requested; }
bool isDebugPortalActive() { return active; }

void handleDebugPortal() {
    if (exitRequested) {
        exitRequested = false;
        requested = false;
        if (active) stopPortal();
    }
    if (requested && !active) beginPortal();
    if (!active) return;
    dnsServer.processNextRequest();
    portalServer.handleClient();
    if (restartPending && static_cast<int32_t>(millis() - restartAtMs) >= 0) {
        delay(50);
        ESP.restart();
    }
}
