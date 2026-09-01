#include "DebugLog.h"

#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "rom/ets_sys.h"

namespace {
constexpr size_t LOG_BUFFER_SIZE = 16384;
constexpr size_t MAX_LOG_CHUNK_SIZE = 2048;

char logBuffer[LOG_BUFFER_SIZE];
uint32_t logCursor = 0;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

void appendToLogBuffer(const uint8_t *buffer, size_t size) {
	portENTER_CRITICAL(&logMux);
	for (size_t i = 0; i < size; ++i) {
		logBuffer[logCursor % LOG_BUFFER_SIZE] = static_cast<char>(buffer[i]);
		++logCursor;
	}
	portEXIT_CRITICAL(&logMux);
}

void ARDUINO_ISR_ATTR remoteDebugPutc(char value) {
	// Arduino/IDF can invoke the debug hook from an ISR. Avoid USB and shared
	// buffer work in that context; normal task-context core logs are mirrored.
	if (xPortInIsrContext()) return;

	const uint8_t byte = static_cast<uint8_t>(value);
	// The ESP-IDF primary console already writes core logs to Serial. This hook
	// is the secondary output used by the Wi-Fi log, so writing to Serial here
	// would duplicate every byte on the USB monitor.
	appendToLogBuffer(&byte, 1);
}
} // namespace

RemoteDebugLog DebugLog;

void RemoteDebugLog::begin(unsigned long baud) {
	Serial.begin(baud);
}

void RemoteDebugLog::setDebugOutput(bool enabled) {
	ets_install_putc2(enabled ? remoteDebugPutc : nullptr);
}

size_t RemoteDebugLog::write(uint8_t value) {
	return write(&value, 1);
}

size_t RemoteDebugLog::write(const uint8_t *buffer, size_t size) {
	if (buffer == nullptr || size == 0) return 0;

	Serial.write(buffer, size);
	appendToLogBuffer(buffer, size);
	return size;
}

bool RemoteDebugLog::readSince(uint32_t since, String& output,
		uint32_t& nextCursor, bool& dropped) const {
	char chunk[MAX_LOG_CHUNK_SIZE];
	size_t count = 0;

	portENTER_CRITICAL(&logMux);
	const uint32_t current = logCursor;
	const uint32_t oldest = current > LOG_BUFFER_SIZE
		? current - LOG_BUFFER_SIZE
		: 0;

	dropped = since < oldest || since > current;
	const uint32_t start = dropped ? oldest : since;
	count = std::min<size_t>(current - start, MAX_LOG_CHUNK_SIZE);

	for (size_t i = 0; i < count; ++i) {
		chunk[i] = logBuffer[(start + i) % LOG_BUFFER_SIZE];
	}
	nextCursor = start + count;
	portEXIT_CRITICAL(&logMux);

	output = "";
	output.reserve(count);
	if (count > 0) {
		output.concat(chunk, count);
	}
	return count > 0;
}

uint32_t RemoteDebugLog::cursor() const {
	portENTER_CRITICAL(&logMux);
	const uint32_t value = logCursor;
	portEXIT_CRITICAL(&logMux);
	return value;
}
