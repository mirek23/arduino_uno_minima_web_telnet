#pragma once
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

// ─── Non-blocking DS18B20 driver ─────────────────────────────────────────────
//
// The RA4M1 is single-core, so loop() must never stall for the sensor's 750 ms
// 12-bit conversion. This driver uses the DallasTemperature async API and its
// own state machine:
//
//   IDLE       -> once TEMP_READ_INTERVAL has elapsed, request a conversion
//   CONVERTING -> once TEMP_CONVERSION_MS has elapsed, latch the result
//
// Only requestTemperatures() and the scratchpad read touch the bus, and each
// takes a bit over a millisecond; the long wait is never blocking.

class TemperatureSensor {
public:
    TemperatureSensor();

    // Probe the bus, latch the sensor address and program the resolution.
    void begin();

    // Non-blocking tick. Call every loop() iteration.
    void update();

    // Last good reading in degrees Celsius. Only meaningful if valid().
    float get() const;

    // True once a plausible reading has been latched and the sensor is
    // still responding. A run of TEMP_MAX_ERRORS bad reads clears it.
    bool valid() const;

    // Consecutive failed reads since the last good one.
    uint8_t errorCount() const;

    // True (once) if the reading changed since the previous call.
    bool changed();

    // Number of DS18B20 devices found on the bus during begin().
    uint8_t deviceCount() const;

private:
    enum class State { IDLE, CONVERTING };

    OneWire           _ow;
    DallasTemperature _sensors;
    DeviceAddress     _address;
    bool              _haveAddress;
    uint8_t           _deviceCount;

    State    _state;
    uint32_t _lastConversionStart;
    uint32_t _lastReadTime;

    float    _temperature;
    bool     _valid;
    bool     _changed;
    uint8_t  _errorCount;           // consecutive failed reads
};
