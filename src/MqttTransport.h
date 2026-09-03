#ifndef MQTT_TRANSPORT_H
#define MQTT_TRANSPORT_H

#include <Arduino.h>

// The implementation uses the ESP-IDF MQTT client already bundled with the
// Arduino ESP32 framework. No second embedded MQTT library is required.
bool mqttTransportStart(const String& uri);
void mqttTransportStop();
void mqttTransportTick(uint32_t now);
bool mqttTransportReady();
bool mqttTransportConnected();

#endif
