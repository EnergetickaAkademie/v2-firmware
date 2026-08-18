#include "OtaManager.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

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
    String body = "{\"firmware_version\":\"";
    body += FIRMWARE_VERSION;
    body += "\",\"hostname\":\"";
    body += OTA_HOSTNAME;
    body += "\",\"ip\":\"";
    body += WiFi.localIP().toString();
    body += "\",\"update_in_progress\":";
    body += updateInProgress ? "true" : "false";
    body += ",\"log_cursor\":";
    body += String(DebugLog.cursor());
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
    return updateInProgress;
}
