#include "MqttTransport.h"

#include <ArduinoJson.h>
#include <esp_system.h>
#include <mqtt_client.h>
#include <Preferences.h>
#include <cstring>

#include "Config.h"
#include "DebugLog.h"
#include "GameState.h"
#include "NetworkManager.h"
#include "RuntimeConfig.h"
#include "StatusLedManager.h"
#include "OtaManager.h"

#define Serial DebugLog

namespace {
constexpr size_t TELEMETRY_SIZE = 60;
constexpr size_t STATE_SIZE = 214;
// QoS 0 heartbeats can occasionally be delayed on workshop Wi-Fi. Allow a
// short RF/scheduling stall without oscillating back to HTTP immediately.
constexpr uint32_t HEARTBEAT_STALE_MS = 10000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 200;
constexpr uint32_t MAX_JSON_SIZE = 4096;
constexpr uint32_t PUBLISH_ERROR_LOG_INTERVAL_MS = 5000;

esp_mqtt_client_handle_t client = nullptr;
String mqttUri;
String topicRoot;
String stateTopic;
String telemetryTopic;
String availabilityTopic;
String stateAckTopic;
String eventTopic;
String eventAckTopic;
String commandsTopic;
String commandAckTopic;
uint32_t bootId = 0;
uint32_t telemetrySequence = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastPublishErrorLogMs = 0;
uint64_t serverEpoch = 0;
uint32_t appliedRevision = 0;
bool connected = false;
bool serverOnline = false;
bool stateApplied = false;
bool ready = false;
bool stateQueued = false;
bool commandQueued = false;
Preferences outboxPreferences;
bool outboxReady = false;
uint32_t nextEventId = 0;
uint32_t inFlightEventId = 0;
uint32_t acknowledgedEventId = 0;
struct QueuedCommand {
    char jobId[80];
    char version[80];
    char sha256[65];
    char path[220];
    uint32_t size;
};
QueuedCommand queuedCommand{};

struct QueuedState {
    uint64_t epoch;
    uint32_t revision;
    uint8_t flags;
    int32_t values[45];
    uint8_t counts[BUILDING_COUNT];
};
QueuedState queuedState{};
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t readU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

int32_t readI32(const uint8_t* p) { return static_cast<int32_t>(readU32(p)); }

uint64_t readU64(const uint8_t* p) {
    uint64_t value = 0;
    for (uint8_t i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

void writeU32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}

void writeI32(uint8_t* p, int32_t value) { writeU32(p, static_cast<uint32_t>(value)); }

bool newerRevision(uint32_t candidate, uint32_t current) {
    const uint32_t delta = candidate - current;
    return delta != 0 && delta < 0x80000000u;
}

bool topicEquals(const esp_mqtt_event_handle_t event, const String& expected) {
    return event->topic && event->topic_len == static_cast<int>(expected.length()) &&
           memcmp(event->topic, expected.c_str(), expected.length()) == 0;
}

void publishJson(const String& topic, JsonDocument& document, int qos, int retain) {
    String payload;
    if (serializeJson(document, payload) == 0 || payload.length() > MAX_JSON_SIZE || !client) return;
    esp_mqtt_client_publish(client, topic.c_str(), payload.c_str(), payload.length(), qos, retain);
}

void publishAvailability(bool online) {
    if (!client) return;
    JsonDocument document;
    document["v"] = 3;
    document["online"] = online;
    document["boot_id"] = bootId;
    document["firmware_version"] = FIRMWARE_VERSION;
    document["config_schema"] = runtimeConfigSchema();
    document["ota_port"] = runtimeOtaPort();
    document["state"] = pullOtaState();
    if (pullOtaJobId().length() > 0) document["job_id"] = pullOtaJobId();
    if (pullOtaError().length() > 0) document["error"] = pullOtaError();
    char epochText[17];
    snprintf(epochText, sizeof(epochText), "%016llx", static_cast<unsigned long long>(serverEpoch));
    document["epoch"] = epochText;
    publishJson(availabilityTopic, document, 1, 1);
}

void publishStateAck(uint32_t revision, bool applied, const char* reason = nullptr) {
    if (!client) return;
    JsonDocument document;
    document["v"] = 3;
    document["boot_id"] = bootId;
    char epochText[17];
    snprintf(epochText, sizeof(epochText), "%016llx", static_cast<unsigned long long>(serverEpoch));
    document["epoch"] = epochText;
    document["revision"] = revision;
    document["status"] = applied ? "applied" : "rejected";
    if (reason) document["reason"] = reason;
    publishJson(stateAckTopic, document, 1, 0);
}

void applyQueuedState() {
    QueuedState next;
    portENTER_CRITICAL(&stateMux);
    if (!stateQueued) {
        portEXIT_CRITICAL(&stateMux);
        return;
    }
    next = queuedState;
    // Retained messages on different topics are not guaranteed to arrive in
    // the order in which we subscribed. Keep state queued until we know the
    // current server epoch instead of rejecting a perfectly good snapshot.
    if (!serverOnline) {
        portEXIT_CRITICAL(&stateMux);
        return;
    }
    stateQueued = false;
    portEXIT_CRITICAL(&stateMux);

    if (next.epoch != serverEpoch) {
        publishStateAck(next.revision, false, "epoch_mismatch");
        return;
    }
    if (stateApplied && !newerRevision(next.revision, appliedRevision)) {
        publishStateAck(next.revision, false, "stale_revision");
        return;
    }

    // All validation has completed before this point. Commit the full state
    // in one normal control-task operation so callbacks never touch outputs.
    const bool gameActive = (next.flags & 0x01) != 0;
    applyMqttFirmwareMode((next.flags & 0x02) != 0);
    int32_t nextMin[9], nextMax[9];
    uint32_t nextConsumption[BUILDING_COUNT];
    float nextCoefficients[9];
    for (size_t i = 0; i < 9; ++i) {
        nextCoefficients[i] = next.values[i] / 1000.0f;
        nextMin[i] = next.values[9 + i] / 1000;
        nextMax[i] = next.values[18 + i] / 1000;
    }
    for (size_t i = 0; i < BUILDING_COUNT; ++i) {
        if (next.values[27 + i] < 0) {
            publishStateAck(next.revision, false, "negative_consumption");
            return;
        }
        nextConsumption[i] = static_cast<uint32_t>(next.values[27 + i] / 1000);
    }
    setBuildingScanScenarioState(gameActive, !gameActive);
    memcpy(currentCoefficient, nextCoefficients, sizeof(nextCoefficients));
    memcpy(baseMinMW, nextMin, sizeof(nextMin));
    memcpy(baseMaxMW, nextMax, sizeof(nextMax));
    memcpy(buildingConsumptionMW, nextConsumption, sizeof(nextConsumption));
    memcpy(authoritativeBuildingCounts, next.counts, sizeof(next.counts));
    appliedRevision = next.revision;
    stateApplied = true;
    ready = true;
    statusLedSetMqttHealthy(true);
    publishStateAck(next.revision, true);
}

void handleMessage(esp_mqtt_event_handle_t event) {
    if (!event->data || event->data_len < 0 || event->total_data_len != event->data_len ||
        event->current_data_offset != 0 || event->total_data_len > static_cast<int>(MAX_JSON_SIZE)) {
        // State packets are fixed-size and must arrive in one bounded MQTT
        // callback. The broker's 4 KiB limit makes oversized JSON impossible.
        return;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(event->data);
    if (topicEquals(event, "enak/v3/server/availability")) {
        JsonDocument document;
        if (deserializeJson(document, event->data, event->data_len)) return;
        if (!document["online"].is<bool>() || !document["epoch"].is<const char*>()) return;
        serverOnline = document["online"].as<bool>();
        serverEpoch = strtoull(document["epoch"].as<const char*>(), nullptr, 16);
        if (serverOnline) {
            // This packet is itself recent proof that CoreAPI is alive and
            // gives the first live heartbeat time to arrive.
            lastHeartbeatMs = millis();
        } else {
            ready = false;
            stateApplied = false;
            statusLedSetMqttHealthy(false);
        }
        return;
    }
    if (topicEquals(event, "enak/v3/server/heartbeat")) {
        JsonDocument document;
        if (deserializeJson(document, event->data, event->data_len)) return;
        if (document["epoch"].is<const char*>()) {
            const uint64_t epoch = strtoull(document["epoch"].as<const char*>(), nullptr, 16);
            // A live heartbeat can recover from a missed retained
            // availability message. An epoch change requires fresh state.
            if (!serverOnline || epoch != serverEpoch) {
                serverEpoch = epoch;
                serverOnline = true;
                stateApplied = false;
                ready = false;
            }
            lastHeartbeatMs = millis();
            statusLedRecordApiSuccess();
        }
        return;
    }
    if (topicEquals(event, eventAckTopic)) {
        JsonDocument document;
        if (deserializeJson(document, event->data, event->data_len) ||
            !document["event_id"].is<uint32_t>() || !document["status"].is<const char*>()) return;
        const String status = document["status"].as<String>();
        if (status == "applied" || status == "duplicate" || status == "rejected") {
            portENTER_CRITICAL(&stateMux);
            acknowledgedEventId = document["event_id"].as<uint32_t>();
            portEXIT_CRITICAL(&stateMux);
        }
        return;
    }
    if (topicEquals(event, stateTopic)) {
        if (event->data_len != static_cast<int>(STATE_SIZE) || data[0] != 'E' || data[1] != 'A' ||
            data[2] != 3 || (data[3] & ~0x03) != 0) return;
        QueuedState next{};
        next.epoch = readU64(data + 4);
        next.revision = readU32(data + 12);
        next.flags = data[3];
        for (size_t i = 0; i < 45; ++i) next.values[i] = readI32(data + 16 + i * 4);
        memcpy(next.counts, data + 196, BUILDING_COUNT);
        portENTER_CRITICAL(&stateMux);
        if (!stateQueued || next.epoch > queuedState.epoch ||
            (next.epoch == queuedState.epoch && newerRevision(next.revision, queuedState.revision))) {
            queuedState = next;
            stateQueued = true;
        }
        portEXIT_CRITICAL(&stateMux);
        return;
    }
    if (topicEquals(event, commandsTopic)) {
        JsonDocument document;
        if (deserializeJson(document, event->data, event->data_len) ||
            !document["job_id"].is<const char*>() || !document["version"].is<const char*>() ||
            !document["sha256"].is<const char*>() || !document["path"].is<const char*>() ||
            !document["size"].is<uint32_t>()) return;
        const String jobId = document["job_id"].as<String>();
        const String version = document["version"].as<String>();
        const String sha256 = document["sha256"].as<String>();
        const String path = document["path"].as<String>();
        if (jobId.length() == 0 || jobId.length() >= sizeof(queuedCommand.jobId) ||
            version.length() == 0 || version.length() >= sizeof(queuedCommand.version) ||
            sha256.length() != 64 || path.length() == 0 || path.length() >= sizeof(queuedCommand.path) ||
            document["size"].as<uint32_t>() == 0) return;
        portENTER_CRITICAL(&stateMux);
        strlcpy(queuedCommand.jobId, jobId.c_str(), sizeof(queuedCommand.jobId));
        strlcpy(queuedCommand.version, version.c_str(), sizeof(queuedCommand.version));
        strlcpy(queuedCommand.sha256, sha256.c_str(), sizeof(queuedCommand.sha256));
        strlcpy(queuedCommand.path, path.c_str(), sizeof(queuedCommand.path));
        queuedCommand.size = document["size"].as<uint32_t>();
        commandQueued = true;
        portEXIT_CRITICAL(&stateMux);
    }
}

esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            connected = true;
            ready = false;
            stateApplied = false;
            lastHeartbeatMs = millis();
            lastTelemetryMs = 0;
            statusLedSetMqttHealthy(false);
            Serial.println("[MQTT] WebSocket session connected.");
            esp_mqtt_client_subscribe(event->client, "enak/v3/server/availability", 1);
            esp_mqtt_client_subscribe(event->client, "enak/v3/server/heartbeat", 0);
            esp_mqtt_client_subscribe(event->client, stateTopic.c_str(), 1);
            esp_mqtt_client_subscribe(event->client, eventAckTopic.c_str(), 1);
            esp_mqtt_client_subscribe(event->client, commandsTopic.c_str(), 1);
            publishAvailability(true);
            break;
        case MQTT_EVENT_DISCONNECTED:
            connected = false;
            ready = false;
            stateApplied = false;
            statusLedSetMqttHealthy(false);
            Serial.println("[MQTT] WebSocket session disconnected; reconnecting.");
            break;
        case MQTT_EVENT_DATA:
            handleMessage(event);
            break;
        case MQTT_EVENT_ERROR:
            if (event->error_handle) {
                const esp_mqtt_error_codes_t* error = event->error_handle;
                Serial.printf("[MQTT] Client error: type=%d tls=0x%x stack=0x%x verify=0x%x errno=%d connack=%d.\n",
                              static_cast<int>(error->error_type),
                              static_cast<unsigned>(error->esp_tls_last_esp_err),
                              static_cast<unsigned>(error->esp_tls_stack_err),
                              static_cast<unsigned>(error->esp_tls_cert_verify_flags),
                              error->esp_transport_sock_errno,
                              static_cast<int>(error->connect_return_code));
            } else {
                Serial.println("[MQTT] Client reported an unspecified transport error.");
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}
} // namespace

bool mqttTransportStart(const String& uri) {
    mqttTransportStop();
    if (uri.length() == 0) {
        Serial.println("[MQTT] Start refused because the broker URI is empty.");
        return false;
    }
    if (runtimeBoardUsername().length() == 0 || runtimeBoardPassword().length() == 0) {
        Serial.printf("[MQTT] Start refused because credentials are incomplete (user=%u, password=%u).\n",
                      static_cast<unsigned>(runtimeBoardUsername().length()),
                      static_cast<unsigned>(runtimeBoardPassword().length()));
        return false;
    }
    if (uri.startsWith("wss://") && strlen(API_CA_CERT) == 0) {
        Serial.println("[MQTT] WSS refused because API_CA_CERT is not configured.");
        return false;
    }
    mqttUri = uri;
    topicRoot = "enak/v3/boards/" + runtimeBoardUsername();
    stateTopic = topicRoot + "/state";
    telemetryTopic = topicRoot + "/telemetry";
    availabilityTopic = topicRoot + "/availability";
    stateAckTopic = topicRoot + "/state-ack";
    eventTopic = topicRoot + "/events";
    eventAckTopic = topicRoot + "/event-ack";
    commandsTopic = topicRoot + "/commands";
    commandAckTopic = topicRoot + "/command-ack";
    bootId = esp_random();
    telemetrySequence = 0;
    lastTelemetryMs = 0;
    lastHeartbeatMs = 0;
    lastPublishErrorLogMs = 0;
    serverEpoch = 0;
    appliedRevision = 0;
    connected = serverOnline = stateApplied = ready = false;
    statusLedSetMqttHealthy(false);
    outboxReady = outboxPreferences.begin("mqtt_outbox", false);
    nextEventId = outboxReady ? outboxPreferences.getUInt("next_id", 0) : 0;

    esp_mqtt_client_config_t config{};
    config.uri = mqttUri.c_str();
    config.username = runtimeBoardUsername().c_str();
    config.password = runtimeBoardPassword().c_str();
    config.client_id = runtimeBoardUsername().c_str();
    config.event_handle = mqttEventHandler;
    config.disable_clean_session = 1;
    config.keepalive = 10;
    config.reconnect_timeout_ms = 3000;
    config.network_timeout_ms = 10000;
    config.task_stack = 8192;
    config.buffer_size = 1024;
    config.out_buffer_size = 1024;
    config.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    config.lwt_topic = availabilityTopic.c_str();
    config.lwt_msg = "{\"v\":3,\"online\":false}";
    config.lwt_qos = 1;
    config.lwt_retain = 1;
    if (uri.startsWith("wss://")) {
        config.cert_pem = API_CA_CERT;
        config.skip_cert_common_name_check = false;
    }
    Serial.printf("[MQTT] Initializing client (free heap=%u bytes).\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));
    client = esp_mqtt_client_init(&config);
    if (!client) {
        Serial.println("[MQTT] esp_mqtt_client_init returned null.");
        mqttTransportStop();
        return false;
    }
    const esp_err_t startResult = esp_mqtt_client_start(client);
    if (startResult != ESP_OK) {
        Serial.printf("[MQTT] esp_mqtt_client_start failed: %s (0x%x).\n",
                      esp_err_to_name(startResult), static_cast<unsigned>(startResult));
        mqttTransportStop();
        return false;
    }
    Serial.printf("[MQTT] Starting persistent MQTT v3 session at %s.\n", uri.c_str());
    return true;
}

void mqttTransportStop() {
    if (client) {
        if (connected) publishAvailability(false);
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = nullptr;
    }
    connected = serverOnline = stateApplied = ready = false;
    inFlightEventId = 0;
    statusLedSetMqttHealthy(false);
    if (outboxReady) {
        outboxPreferences.end();
        outboxReady = false;
    }
}

bool mqttTransportReady() { return ready && connected && serverOnline && stateApplied &&
    millis() - lastHeartbeatMs <= HEARTBEAT_STALE_MS; }
bool mqttTransportConnected() { return connected; }

void mqttTransportTick(uint32_t now) {
    if (!client) return;
    applyQueuedState();
    QueuedCommand command;
    bool hasCommand = false;
    portENTER_CRITICAL(&stateMux);
    if (commandQueued) {
        command = queuedCommand;
        commandQueued = false;
        hasCommand = true;
    }
    portEXIT_CRITICAL(&stateMux);
    if (hasCommand && connected) {
        JsonDocument accepted;
        accepted["v"] = 3;
        accepted["command_id"] = command.jobId;
        accepted["state"] = "accepted";
        publishJson(commandAckTopic, accepted, 1, 0);
        const bool installed = installPullFirmware(runtimeApiBaseUrl(), jwtToken,
            command.jobId, command.version, command.size, command.sha256, command.path);
        JsonDocument result;
        result["v"] = 3;
        result["command_id"] = command.jobId;
        result["state"] = installed ? "succeeded" : "failed";
        if (!installed) result["error"] = pullOtaError();
        publishJson(commandAckTopic, result, 1, 0);
    }
    uint32_t acked = 0;
    portENTER_CRITICAL(&stateMux);
    acked = acknowledgedEventId;
    acknowledgedEventId = 0;
    portEXIT_CRITICAL(&stateMux);
    if (acked != 0 && acked == inFlightEventId) {
        bool removed = false;
        if (pendingMutex && xSemaphoreTake(pendingMutex, 0) == pdTRUE) {
            if (!pendingBuildings.empty() && pendingBuildings.front().eventId == acked) {
                pendingBuildings.erase(pendingBuildings.begin());
                removed = true;
            }
            xSemaphoreGive(pendingMutex);
        }
        if (removed) persistPendingBuildingQueue();
        inFlightEventId = 0;
    }
    if (mqttTransportReady() && inFlightEventId == 0 && pendingMutex &&
        xSemaphoreTake(pendingMutex, 0) == pdTRUE) {
        bool assignedEventId = false;
        if (!pendingBuildings.empty()) {
            PendingBuilding& pending = pendingBuildings.front();
            if (pending.eventId == 0) {
                pending.eventId = ++nextEventId;
                if (outboxReady) outboxPreferences.putUInt("next_id", nextEventId);
                assignedEventId = true;
            }
            JsonDocument event;
            event["v"] = 3;
            event["boot_id"] = bootId;
            event["event_id"] = pending.eventId;
            event["type"] = "building_add";
            event["uid"] = pending.uid;
            event["building_type"] = pending.type;
            publishJson(eventTopic, event, 1, 0);
            inFlightEventId = pending.eventId;
        }
        xSemaphoreGive(pendingMutex);
        if (assignedEventId) persistPendingBuildingQueue();
    }
    if (connected && (now - lastHeartbeatMs > HEARTBEAT_STALE_MS || !serverOnline)) {
        ready = false;
        statusLedSetMqttHealthy(false);
    }
    if (!connected || now - lastTelemetryMs < TELEMETRY_INTERVAL_MS) return;
    uint8_t payload[TELEMETRY_SIZE]{};
    payload[0] = 'E'; payload[1] = 'A'; payload[2] = 3; payload[3] = 0;
    writeU32(payload + 4, bootId);
    writeU32(payload + 8, ++telemetrySequence);
    writeU32(payload + 12, millis());
    writeI32(payload + 16, currentTotalProduction_MW);
    writeI32(payload + 20, currentTotalConsumption_MW);
    for (size_t i = 0; i < 9; ++i) writeI32(payload + 24 + i * 4, productionByTypeMW[i]);
    const int messageId = esp_mqtt_client_publish(
        client, telemetryTopic.c_str(), reinterpret_cast<char*>(payload), sizeof(payload), 0, 0);
    if (messageId < 0 && now - lastPublishErrorLogMs >= PUBLISH_ERROR_LOG_INTERVAL_MS) {
        lastPublishErrorLogMs = now;
        Serial.printf("[MQTT] Telemetry publish failed (heap=%u); connection will retry.\n",
                      static_cast<unsigned>(ESP.getFreeHeap()));
    }
    lastTelemetryMs = now;
}
