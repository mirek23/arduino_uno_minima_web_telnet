#include "temperature.h"

TemperatureSensor::TemperatureSensor()
    : _ow(TEMP_SENSOR_PIN)
    , _sensors(&_ow)
    , _haveAddress(false)
    , _deviceCount(0)
    , _state(State::IDLE)
    , _lastConversionStart(0)
    , _lastReadTime(0)
    , _temperature(0.0f)
    , _valid(false)
    , _changed(false)
    , _errorCount(0)
{
    memset(_address, 0, sizeof(_address));
}

void TemperatureSensor::begin() {
    _sensors.begin();

    // Manage the conversion wait ourselves so the bus calls return at once.
    _sensors.setWaitForConversion(false);

    _deviceCount = _sensors.getDeviceCount();
    _haveAddress = _sensors.getAddress(_address, 0);

    if (_haveAddress) {
        // 12-bit resolution: 0.0625 degC steps, 750 ms conversion time.
        _sensors.setResolution(_address, TEMP_RESOLUTION_BITS);
        Serial.print("[Temp] DS18B20 found, resolution ");
        Serial.print(_sensors.getResolution(_address));
        Serial.println(" bits");
    } else {
        Serial.println("[Temp] WARNING: no DS18B20 on the bus (check the 4.7k pull-up)");
    }

    // Start the first conversion immediately rather than waiting an interval.
    _lastReadTime = millis() - TEMP_READ_INTERVAL;
}

void TemperatureSensor::update() {
    uint32_t now = millis();

    switch (_state) {
        case State::IDLE:
            if (now - _lastReadTime < TEMP_READ_INTERVAL) break;

            // Re-probe until a sensor appears, so hot-plugging one works.
            if (!_haveAddress) {
                _deviceCount = _sensors.getDeviceCount();
                _haveAddress = _sensors.getAddress(_address, 0);
                if (_haveAddress) {
                    _sensors.setResolution(_address, TEMP_RESOLUTION_BITS);
                    _errorCount = 0;
                    Serial.println("[Temp] DS18B20 appeared on the bus");
                } else {
                    _lastReadTime = now;
                    break;
                }
            }

            _sensors.requestTemperatures();
            _lastConversionStart = now;
            _state = State::CONVERTING;
            break;

        case State::CONVERTING:
            if (now - _lastConversionStart < TEMP_CONVERSION_MS) break;

            _lastReadTime = now;
            _state        = State::IDLE;

            float t = _sensors.getTempC(_address);
            if (t == DEVICE_DISCONNECTED_C || t < -55.0f || t > 125.0f) {
                // A single bad read is far more likely to be a marginally
                // timed bit than an unplugged sensor, so ride out a few
                // before giving up on the device.
                if (_errorCount < 0xFF) _errorCount++;
                if (_errorCount >= TEMP_MAX_ERRORS) {
                    _haveAddress = false;   // re-probe on the next tick
                    if (_valid) {
                        _valid   = false;
                        _changed = true;
                        Serial.println("[Temp] DS18B20 lost after repeated bad reads");
                    }
                }
            } else {
                _errorCount = 0;
                if (!_valid || t != _temperature) {
                    _temperature = t;
                    _valid       = true;
                    _changed     = true;
                }
            }
            break;
    }
}

float   TemperatureSensor::get() const         { return _temperature; }
bool    TemperatureSensor::valid() const       { return _valid; }
uint8_t TemperatureSensor::deviceCount() const { return _deviceCount; }
uint8_t TemperatureSensor::errorCount() const  { return _errorCount; }

bool TemperatureSensor::changed() {
    if (_changed) { _changed = false; return true; }
    return false;
}
