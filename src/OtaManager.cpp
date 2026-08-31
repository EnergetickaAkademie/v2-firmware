#include "OtaManager.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

#include "Config.h"
#include "DebugLog.h"
#include "StatusLedManager.h"

#define Serial DebugLog

namespace {
WebServer otaServer(OTA_PORT);

constexpr char OTA_PATH[] = "/ota/firmware";
constexpr char STATUS_PATH[] = "/ota/status";
constexpr char LOG_PATH[] = "/ota/log";
constexpr char AUTH_HEADER[] = "X-OTA-Password";

bool serverStarted = false;
volatile bool updateInProgress = false;
bool uploadAuthorized = false;
bool uploadSucceeded = false;
bool restartPending = false;
uint32_t restartRequestedAt = 0;
Preferences otaPreferences;
String pullJobId;
String pullTargetVersion;
String pullState = "idle";
String pullError;
bool pullUpdateInProgress = false;

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

void setPullFailure(const String& message) {
    pullState = "failed";
    pullError = message;
    pullUpdateInProgress = false;
    otaPreferences.putString("state", pullState);
    otaPreferences.putString("error", pullError);
}

bool isAuthorized() {
    if (strlen(OTA_PASSWORD) < 8 || strcmp(OTA_PASSWORD, "CHANGE_ME") == 0) {
        return false;
    }
    return otaServer.header(AUTH_HEADER) == OTA_PASSWORD;
}

void sendJson(int status, const String& body) {
    otaServer.sendHeader("Cache-Control", "no-store");
    otaServer.send(status, "application/json", body);
}

void handleStatus() {
    if (!isAuthorized()) {
        sendJson(401, "{\"error\":\"unauthorized\"}");
        return;
    }
    String body = "{\"firmware_version\":\"";
    body += FIRMWARE_VERSION;
    body += "\",\"board_id\":\"";
    body += BOARD_USERNAME;
    body += "\",\"hostname\":\"";
    body += OTA_HOSTNAME;
    body += "\",\"ip\":\"";
    body += WiFi.localIP().toString();
    body += "\",\"update_in_progress\":";
    body += updateInProgress ? "true" : "false";
    body += ",\"log_cursor\":";
    body += String(DebugLog.cursor());
    body += ",\"ota_port\":";
    body += String(OTA_PORT);
    body += ",\"config_schema\":1";
    body += "}";
    sendJson(200, body);
}

void handleLog() {
    if (!isAuthorized()) {
        sendJson(401, "{\"error\":\"unauthorized\"}");
        return;
    }

    uint32_t since = 0;
    if (otaServer.hasArg("since")) {
        since = static_cast<uint32_t>(strtoul(otaServer.arg("since").c_str(), nullptr, 10));
    }

    String body;
    uint32_t nextCursor = since;
    bool dropped = false;
    const bool hasData = DebugLog.readSince(since, body, nextCursor, dropped);
    const uint32_t chunkStart =
        nextCursor - static_cast<uint32_t>(body.length());

    otaServer.sendHeader("Cache-Control", "no-store");
    otaServer.sendHeader("X-Log-Cursor", String(nextCursor));
    otaServer.sendHeader("X-Log-Start", String(chunkStart));
    otaServer.sendHeader("X-Log-Dropped", dropped ? "1" : "0");
    otaServer.sendHeader("X-Log-Empty", hasData ? "0" : "1");

    // WebServer::send() logs a warning for a zero-length body. Because core
    // logs are mirrored into this same buffer, an empty polling response used
    // to create a self-sustaining warning loop. Send an ignored placeholder.
    if (!hasData) body = " ";
    otaServer.send(200, "text/plain; charset=utf-8", body);
}

void handleUploadFinished() {
    if (!uploadAuthorized) {
        sendJson(401, "{\"error\":\"unauthorized\"}");
        return;
    }

    if (!uploadSucceeded) {
        String body = "{\"error\":\"update_failed\",\"code\":";
        body += String(Update.getError());
        body += "}";
        sendJson(500, body);
        updateInProgress = false;
        statusLedSetOtaState(StatusOtaState::Failed);
        return;
    }

    sendJson(200, "{\"success\":true,\"message\":\"firmware accepted; rebooting\"}");
    statusLedSetOtaState(StatusOtaState::Succeeded);
    restartPending = true;
    restartRequestedAt = millis();
}

void handleUploadChunk() {
    HTTPUpload& upload = otaServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        uploadAuthorized = isAuthorized();
        uploadSucceeded = false;

        if (!uploadAuthorized) {
            Serial.println("[OTA] Rejected unauthorized firmware upload.");
            return;
        }

        updateInProgress = true;
        statusLedSetOtaState(StatusOtaState::Uploading);
        Serial.printf("[OTA] Starting firmware upload: %s\n", upload.filename.c_str());

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Serial.printf("[OTA] Update.begin failed: %u\n", Update.getError());
            updateInProgress = false;
            statusLedSetOtaState(StatusOtaState::Failed);
        }
        return;
    }

    if (!uploadAuthorized || !updateInProgress) return;

    if (upload.status == UPLOAD_FILE_WRITE) {
        size_t written = Update.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
            Serial.printf("[OTA] Short write: expected %u, wrote %u\n",
                          static_cast<unsigned>(upload.currentSize),
                          static_cast<unsigned>(written));
            Update.abort();
            updateInProgress = false;
            statusLedSetOtaState(StatusOtaState::Failed);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        uploadSucceeded = Update.end(true);

        if (uploadSucceeded) {
            Serial.printf("[OTA] Firmware upload complete: %u bytes.\n",
                          static_cast<unsigned>(upload.totalSize));
        } else {
            updateInProgress = false;
            statusLedSetOtaState(StatusOtaState::Failed);
            Serial.printf("[OTA] Update.end failed: %u\n", Update.getError());
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        updateInProgress = false;
        uploadSucceeded = false;
        statusLedSetOtaState(StatusOtaState::Failed);
        Serial.println("[OTA] Firmware upload aborted.");
    }
}

void startServer() {
    static const char* headers[] = {AUTH_HEADER};
    otaServer.collectHeaders(headers, 1);

    otaServer.on(STATUS_PATH, HTTP_GET, handleStatus);
    otaServer.on(LOG_PATH, HTTP_GET, handleLog);
    otaServer.on(OTA_PATH, HTTP_POST, handleUploadFinished, handleUploadChunk);
    otaServer.onNotFound([]() {
        sendJson(404, "{\"error\":\"not_found\"}");
    });
    otaServer.begin();

    if (!MDNS.begin(OTA_HOSTNAME)) {
        Serial.println("[OTA] mDNS setup failed; use the board IP address.");
    } else {
        MDNS.addService("http", "tcp", OTA_PORT);
    }

    serverStarted = true;
    Serial.printf("[OTA] Ready at http://%s:%u%s (IP %s).\n",
                  OTA_HOSTNAME,
                  OTA_PORT,
                  OTA_PATH,
                  WiFi.localIP().toString().c_str());
}
} // namespace

void setupOta() {
    otaPreferences.begin("ota_state", false);
    pullJobId = otaPreferences.getString("job_id", "");
    pullTargetVersion = otaPreferences.getString("version", "");
    pullState = otaPreferences.getString("state", "idle");
    pullError = otaPreferences.getString("error", "");
    if (pullJobId.length() > 0 &&
        (pullState == "downloading" || pullState == "verifying" ||
         pullState == "installing" || pullState == "rebooting")) {
        if (pullState == "rebooting" && pullTargetVersion == FIRMWARE_VERSION) {
            pullState = "succeeded";
            pullError = "";
        } else {
            pullState = "failed";
            pullError = "Firmware version after reboot does not match the requested version";
        }
        otaPreferences.putString("state", pullState);
        otaPreferences.putString("error", pullError);
    }
    if (strlen(OTA_PASSWORD) < 8 || strcmp(OTA_PASSWORD, "CHANGE_ME") == 0) {
        Serial.println("[OTA] Disabled: configure an OTA password with at least 8 characters.");
        return;
    }

    if (WiFi.status() == WL_CONNECTED) startServer();
}

void handleOta() {
    if (!serverStarted && WiFi.status() == WL_CONNECTED) startServer();
    if (!serverStarted) return;

    otaServer.handleClient();

    if (restartPending && millis() - restartRequestedAt >= 750) {
        Serial.println("[OTA] Rebooting into updated firmware.");
        delay(50);
        ESP.restart();
    }
}

bool isOtaInProgress() {
    return updateInProgress || pullUpdateInProgress;
}

bool installPullFirmware(const String& apiBaseUrl, const String& jwtToken,
                         const String& jobId, const String& version,
                         uint32_t expectedSize, const String& expectedSha256,
                         const String& path) {
    if (pullUpdateInProgress || jobId.length() == 0 || version.length() == 0 ||
        expectedSize == 0 || expectedSha256.length() != 64) {
        return false;
    }

    String normalizedSha256 = expectedSha256;
    normalizedSha256.toLowerCase();
    pullUpdateInProgress = true;
    pullJobId = jobId;
    pullTargetVersion = version;
    pullState = "downloading";
    pullError = "";
    otaPreferences.putString("job_id", pullJobId);
    otaPreferences.putString("version", pullTargetVersion);
    otaPreferences.putString("state", pullState);
    otaPreferences.putString("error", "");

    String url = apiBaseUrl;
    if (url.endsWith("/")) url.remove(url.length() - 1);
    url += path;

    WiFiClient client;
    HTTPClient http;
    if (url.startsWith("https://")) {
        if (strlen(API_CA_CERT) == 0) {
            setPullFailure("HTTPS firmware downloads require API_CA_CERT");
            return false;
        }
        if (!http.begin(url, API_CA_CERT)) {
            setPullFailure("Could not initialize firmware download");
            return false;
        }
    } else {
        if (!http.begin(client, url)) {
            setPullFailure("Could not initialize firmware download");
            return false;
        }
    }
    http.addHeader("Authorization", "Bearer " + jwtToken);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        setPullFailure("Firmware download returned HTTP " + String(httpCode));
        http.end();
        return false;
    }
    int contentLength = http.getSize();
    if (contentLength != static_cast<int>(expectedSize)) {
        setPullFailure("Firmware download size does not match metadata");
        http.end();
        return false;
    }

    if (!Update.begin(expectedSize, U_FLASH)) {
        setPullFailure("Could not start OTA partition update");
        http.end();
        return false;
    }

    mbedtls_sha256_context digest;
    mbedtls_sha256_init(&digest);
    if (mbedtls_sha256_starts_ret(&digest, 0) != 0) {
        mbedtls_sha256_free(&digest);
        Update.abort();
        setPullFailure("Could not initialize SHA-256 verification");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[4096];
    uint32_t total = 0;
    bool valid = true;
    while (total < expectedSize) {
        size_t available = stream->available();
        if (available == 0) {
            if (!stream->connected()) {
                valid = false;
                break;
            }
            delay(2);
            continue;
        }
        size_t requested = available > sizeof(buffer) ? sizeof(buffer) : available;
        size_t count = stream->readBytes(buffer, requested);
        if (count == 0 || total + count > expectedSize || Update.write(buffer, count) != count) {
            valid = false;
            break;
        }
        mbedtls_sha256_update_ret(&digest, buffer, count);
        total += count;
    }

    uint8_t hash[32] = {0};
    pullState = "verifying";
    otaPreferences.putString("state", pullState);
    mbedtls_sha256_finish_ret(&digest, hash);
    mbedtls_sha256_free(&digest);
    http.end();

    if (!valid || total != expectedSize || sha256Hex(hash) != normalizedSha256) {
        Update.abort();
        setPullFailure("Firmware SHA-256 verification failed");
        return false;
    }
    pullState = "installing";
    otaPreferences.putString("state", pullState);
    if (!Update.end(true)) {
        setPullFailure("Could not finalize OTA partition update");
        return false;
    }

    pullState = "rebooting";
    otaPreferences.putString("state", pullState);
    pullUpdateInProgress = false;
    delay(100);
    ESP.restart();
    return true;
}

String pullOtaJobId() { return pullJobId; }
String pullOtaState() { return pullState; }
String pullOtaError() { return pullError; }
