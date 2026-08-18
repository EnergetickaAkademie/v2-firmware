#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <Arduino.h>

class RemoteDebugLog : public Print {
public:
	using Print::write;

	void begin(unsigned long baud);
	void setDebugOutput(bool enabled);
	size_t write(uint8_t value) override;
	size_t write(const uint8_t *buffer, size_t size) override;

	bool readSince(uint32_t since, String& output, uint32_t& nextCursor,
				   bool& dropped) const;
	uint32_t cursor() const;
};

extern RemoteDebugLog DebugLog;

#endif
