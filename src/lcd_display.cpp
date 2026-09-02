#include "lcd_display.h"
#include <Wire.h>

// ─── PCF8574 bit assignments ─────────────────────────────────────────────────
#define LCD_BIT_RS          0x01
#define LCD_BIT_RW          0x02
#define LCD_BIT_EN          0x04
#define LCD_BIT_BACKLIGHT   0x08

// ─── HD44780 commands ────────────────────────────────────────────────────────
#define LCD_CMD_CLEAR       0x01
#define LCD_CMD_HOME        0x02
#define LCD_CMD_ENTRY_MODE  0x06    // increment cursor, no display shift
#define LCD_CMD_DISPLAY_OFF 0x08
#define LCD_CMD_DISPLAY_ON  0x0C    // display on, cursor off, blink off
#define LCD_CMD_FUNCTION    0x28    // 4-bit bus, 2 lines, 5x8 font
#define LCD_CMD_SET_DDRAM   0x80

// Degree sign in the HD44780 A00 character ROM.
#define LCD_CHAR_DEGREE     '\xDF'

// Backpack addresses seen in the wild: PCF8574 at 0x27, PCF8574A at 0x3F.
static const uint8_t LCD_PROBE_ADDRS[] = { 0x27, 0x3F };

LcdDisplay::LcdDisplay()
    : _present(false)
    , _addr(0)
    , _backlight(LCD_BIT_BACKLIGHT)
    , _bootMs(0)
    , _lastRefresh(0)
    , _splashDone(false)
    , _ipValid(false)
    , _temperature(0.0f)
    , _tempValid(false)
    , _count(0)
{
    memset(_shadow, ' ', sizeof(_shadow));
    memset(_sysName, 0, sizeof(_sysName));
}

// ─── Bus primitives ──────────────────────────────────────────────────────────

void LcdDisplay::expanderWrite(uint8_t data) {
    Wire.beginTransmission(_addr);
    Wire.write((uint8_t)(data | _backlight));
    Wire.endTransmission();
}

// One 4-bit transfer. The HD44780 latches on the falling edge of E, and the
// PCF8574 presents data and E in the same output byte, so two bus writes are
// enough: raise E with the data already valid, then drop it.
void LcdDisplay::writeNibble(uint8_t nibble, uint8_t rs) {
    uint8_t data = (uint8_t)((nibble & 0x0F) << 4) | rs;
    expanderWrite(data | LCD_BIT_EN);
    delayMicroseconds(1);               // E high >= 450 ns
    expanderWrite(data);
    delayMicroseconds(50);              // most instructions settle in 37 us
}

void LcdDisplay::sendByte(uint8_t value, uint8_t rs) {
    writeNibble((uint8_t)(value >> 4), rs);
    writeNibble((uint8_t)(value & 0x0F), rs);
}

void LcdDisplay::command(uint8_t value) {
    sendByte(value, 0);
}

void LcdDisplay::setCursor(uint8_t col, uint8_t row) {
    // Row 0 starts at DDRAM 0x00, row 1 at 0x40 on a 1602.
    command((uint8_t)(LCD_CMD_SET_DDRAM | (col + (row ? 0x40 : 0x00))));
}

bool LcdDisplay::probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ─── Initialisation ──────────────────────────────────────────────────────────

bool LcdDisplay::begin() {
    _bootMs = millis();
    // Make the first update() paint immediately: setup() calls it once before
    // bringing up Ethernet, and a DHCP request blocks for seconds.
    _lastRefresh = millis() - LCD_REFRESH_MS;

    Wire.begin();
    Wire.setClock(LCD_I2C_CLOCK);

    if (LCD_I2C_ADDR != 0x00) {
        _addr    = LCD_I2C_ADDR;
        _present = probe(_addr);
    } else {
        for (uint8_t i = 0; i < sizeof(LCD_PROBE_ADDRS); i++) {
            if (probe(LCD_PROBE_ADDRS[i])) {
                _addr    = LCD_PROBE_ADDRS[i];
                _present = true;
                break;
            }
        }
    }

    if (!_present) {
        Serial.println("[LCD] no PCF8574 backpack found on I2C - display disabled");
        return false;
    }

    Serial.print("[LCD] backpack at 0x");
    Serial.println(_addr, HEX);

    // HD44780 power-on reset into 4-bit mode (datasheet figure 24).
    delay(50);
    expanderWrite(0x00);                // idle, backlight on
    delay(50);

    writeNibble(0x03, 0); delayMicroseconds(4500);
    writeNibble(0x03, 0); delayMicroseconds(4500);
    writeNibble(0x03, 0); delayMicroseconds(150);
    writeNibble(0x02, 0);               // now in 4-bit mode

    command(LCD_CMD_FUNCTION);
    command(LCD_CMD_DISPLAY_OFF);
    command(LCD_CMD_CLEAR);
    delay(2);                           // clear needs 1.52 ms
    command(LCD_CMD_ENTRY_MODE);
    command(LCD_CMD_DISPLAY_ON);
    command(LCD_CMD_HOME);
    delay(2);

    // The panel is now blank, and _shadow is all spaces to match, so the first
    // update() writes only the characters that actually carry information.
    memset(_shadow, ' ', sizeof(_shadow));
    return true;
}

// ─── Value setters ───────────────────────────────────────────────────────────

void LcdDisplay::setSysName(const char* name) {
    if (!name) return;
    strncpy(_sysName, name, SYS_NAME_SIZE - 1);
    _sysName[SYS_NAME_SIZE - 1] = '\0';
}

void LcdDisplay::setTemperature(float celsius, bool valid) {
    _temperature = celsius;
    _tempValid   = valid;
}

void LcdDisplay::setCount(int32_t count) {
    _count = count;
}

void LcdDisplay::setNetwork(const IPAddress& ip, bool ipValid) {
    _ip      = ip;
    _ipValid = ipValid;
}

bool    LcdDisplay::present() const { return _present; }
uint8_t LcdDisplay::address() const { return _addr; }

// ─── Line composition ────────────────────────────────────────────────────────
// Both helpers write exactly LCD_COLS characters plus a NUL into out.

static void padTo(char* out, size_t used) {
    for (size_t i = used; i < LCD_COLS; i++) out[i] = ' ';
    out[LCD_COLS] = '\0';
}

// Centre text across all LCD_COLS columns, padding both sides with spaces.
// An odd remainder goes to the right, so a 7-character name on a 16-column
// panel sits at column 4. Text wider than the display is truncated, which is
// what the left-aligned version did too.
static void centerInto(char* out, const char* text) {
    size_t n = strlen(text);
    if (n > LCD_COLS) n = LCD_COLS;
    size_t left = (LCD_COLS - n) / 2;
    memset(out, ' ', LCD_COLS);
    memcpy(out + left, text, n);
    out[LCD_COLS] = '\0';
}

void LcdDisplay::composeLine1(char* out) {
    // The splash shows the system name; afterwards, the address.
    char buf[24];

    if (_splashDone) {
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", _ip[0], _ip[1], _ip[2], _ip[3]);
    } else {
        snprintf(buf, sizeof(buf), "%s", _sysName);
    }

    centerInto(out, buf);
}

void LcdDisplay::composeLine2(char* out) {
    // Blank during the splash, so the first thing on the panel is the system
    // name and nothing else.
    if (!_splashDone) { padTo(out, 0); return; }

    // Layout, exactly 16 columns:  "_NN.NN`C E:_CCCC"
    //   6 temperature + degree + 'C' + space + "E:" + 5 count = 16
    char buf[32];

    // The field widths above assume the count fits in five characters; clamp
    // the *display* only. The web and telnet views always show the true value.
    long shown = (long)_count;
    if (shown >  99999L) shown =  99999L;
    if (shown < -9999L)  shown = -9999L;

    if (_tempValid) {
        snprintf(buf, sizeof(buf), "%6.2f%cC E:%5ld",
                 (double)_temperature, LCD_CHAR_DEGREE, shown);
    } else {
        snprintf(buf, sizeof(buf), " --.--%cC E:%5ld", LCD_CHAR_DEGREE, shown);
    }

    size_t n = strlen(buf);
    if (n > LCD_COLS) n = LCD_COLS;
    memcpy(out, buf, n);
    padTo(out, n);
}

// ─── Refresh ─────────────────────────────────────────────────────────────────

// Send only the characters that differ, coalescing adjacent changes into a
// single cursor positioning plus a run of data writes.
void LcdDisplay::flushLine(uint8_t row, const char* target) {
    uint8_t col = 0;
    while (col < LCD_COLS) {
        if (target[col] == _shadow[row][col]) { col++; continue; }

        uint8_t runStart = col;
        while (col < LCD_COLS && target[col] != _shadow[row][col]) col++;

        setCursor(runStart, row);
        for (uint8_t i = runStart; i < col; i++) {
            sendByte((uint8_t)target[i], LCD_BIT_RS);
            _shadow[row][i] = target[i];
        }
    }
}

void LcdDisplay::update() {
    if (!_present) return;

    uint32_t now = millis();
    if (now - _lastRefresh < LCD_REFRESH_MS) return;
    _lastRefresh = now;

    // Retire the splash once the name has had its minimum time on screen and
    // an address is actually known. Evaluated once per refresh so both lines
    // always agree, and latched so it only ever happens on the way up.
    if (!_splashDone &&
        (now - _bootMs) >= LCD_NAME_HOLD_MS &&
        _ipValid) {
        _splashDone = true;
    }

    char line[LCD_COLS + 1];

    composeLine1(line);
    flushLine(0, line);

    composeLine2(line);
    flushLine(1, line);
}
