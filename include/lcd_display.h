#pragma once
#include <Arduino.h>
#include "config.h"

// ─── LCD1602 over an I2C (PCF8574) backpack ──────────────────────────────────
//
// Driven directly rather than through a LiquidCrystal_I2C library: the driver
// is ~100 lines, avoids a third-party dependency on a young core, and lets the
// refresh do a character-level diff instead of rewriting both lines. That
// matters here because every character costs four I2C byte writes (~400 us at
// 100 kHz) and loop() also has to service HTTP and telnet.
//
// Backpack bit mapping (the near-universal PCF8574 wiring):
//   P0 = RS, P1 = RW, P2 = E, P3 = backlight, P4..P7 = D4..D7
//
// Display policy, per the project brief:
//   Boot splash  line 1 the centred system name, line 2 the centred firmware
//                revision, so a boot or reboot shows what is running. It ends
//                once the name has been up for LCD_NAME_HOLD_MS *and* an
//                address is known — in DHCP mode that means the lease has
//                landed, so the name is always shown for at least
//                LCD_NAME_HOLD_MS.
//   Afterwards   line 1 the IP address, line 2 the temperature as NN.NN degC
//                plus the encoder count.
// The splash latches off once, so a later link event cannot bring it back.
//
// If no backpack answers on the bus every method becomes a no-op, so a missing
// or unpowered LCD never stalls the rest of the firmware.

class LcdDisplay {
public:
    LcdDisplay();

    // Probe the bus, initialise the controller. Returns false if absent.
    bool begin();

    // Refresh at most once per LCD_REFRESH_MS. Call every loop() iteration.
    void update();

    // ── Values shown on the display ────────────────────────────────────────
    void setSysName(const char* name);
    void setTemperature(float celsius, bool valid);
    void setCount(int32_t count);

    // ip is only shown once ipValid is true (i.e. a DHCP lease has landed or
    // a static address has been applied).
    void setNetwork(const IPAddress& ip, bool ipValid);

    bool present() const;
    uint8_t address() const;

private:
    // ── Low-level PCF8574 / HD44780 ────────────────────────────────────────
    void expanderWrite(uint8_t data);
    void writeNibble(uint8_t nibble, uint8_t rs);
    void sendByte(uint8_t value, uint8_t rs);
    void command(uint8_t value);
    void setCursor(uint8_t col, uint8_t row);
    bool probe(uint8_t addr);

    // ── Rendering ──────────────────────────────────────────────────────────
    void composeLine1(char* out);
    void composeLine2(char* out);
    void flushLine(uint8_t row, const char* target);

    bool     _present;
    uint8_t  _addr;
    uint8_t  _backlight;

    // Shadow of what the panel currently shows, for the diff-based refresh.
    char     _shadow[LCD_ROWS][LCD_COLS];

    uint32_t _bootMs;
    uint32_t _lastRefresh;
    bool     _splashDone;       // latched once the splash has been retired

    char      _sysName[SYS_NAME_SIZE];
    IPAddress _ip;
    bool      _ipValid;
    float     _temperature;
    bool      _tempValid;
    int32_t   _count;
};
