#include "DebugPortal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#include "Config.h"
#include "DebugLog.h"
#include "GameState.h"
#include "NetworkManager.h"
#include "NfcDebug.h"
#include "MainboardDebug.h"
#include "RuntimeConfig.h"
#include "StatusLedManager.h"
#include "SubstationManager.h"

#define Serial DebugLog

#include "DebugPortalHtml.h"

namespace {
WebServer portalServer(80);
DNSServer dnsServer;
volatile bool requested = false;
bool active = false;
bool restartPending = false;
uint32_t restartAtMs = 0;
String requestToken;

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
    if (nextBoardPassword.length() == 0 || nextBoardPassword.length() > 127 ||
        nextOtaPassword.length() < 8 || nextOtaPassword.length() > 127) {
        sendError(400, "Board and OTA passwords are missing or invalid");
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
                           static_cast<uint16_t>(nextPort))) {
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
    const char* password = strlen(DEBUG_AP_PASSWORD) >= 8 ? DEBUG_AP_PASSWORD : "enak-debug";
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
    portalServer.on("/api/encoder", HTTP_POST, handleEncoderControlPost);
    portalServer.on("/api/nfc/status", HTTP_GET, handleNfcStatusGet);
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
bool isDebugPortalRequested() { return requested; }
bool isDebugPortalActive() { return active; }

void handleDebugPortal() {
    if (requested && !active) beginPortal();
    if (!active) return;
    dnsServer.processNextRequest();
    portalServer.handleClient();
    if (restartPending && static_cast<int32_t>(millis() - restartAtMs) >= 0) {
        delay(50);
        ESP.restart();
    }
}
