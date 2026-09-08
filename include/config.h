#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════════════════
// Hardware Pin Definitions — Arduino UNO R4 Minima (Renesas RA4M1)
// ═══════════════════════════════════════════════════════════════════════════

// ─── DS18B20 temperature sensor (OneWire) ──────────────────────────────────
// Needs a 4.7 kΩ pull-up between DATA and 5 V.
#define TEMP_SENSOR_PIN         7       // D7  (P107)

// ─── Incremental (quadrature) rotary encoder ───────────────────────────────
// Both channels must be interrupt-capable pins. On the UNO R4 Minima only
// D0, D1, D2, D3, D8, D12, D13, A1, A2, A3 have an ICU IRQ channel, and each
// pin maps to a fixed channel — two pins sharing a channel cannot both be
// used. A2=IRQ7, D3=IRQ1, A3=IRQ2 are distinct, so this trio is valid.
#define ENC_A_PIN               A2      // A2  (P001, IRQ7) channel A
#define ENC_B_PIN               3       // D3  (P104, IRQ1) channel B
#define ENC_BTN_PIN             A3      // A3  (P002) push button -> reset count

// Quadrature edges per mechanical detent. A standard EC11-style encoder
// produces 4 state transitions per click; set to 1 to count every edge.
#define ENC_EDGES_PER_DETENT    4

// ─── "Load factory defaults" jumper ────────────────────────────────────────
// Jumper this pin to GND at power-up to ignore the EEPROM and boot with the
// compile-time defaults below. Read once during setup(), so it does not need
// to be one of the interrupt-capable pins.
//
// NOTE: the project brief specified A5 for this jumper, but on the UNO R4
// Minima A5 (P100) *is* the I2C SCL line for Wire (A4 = SDA, A5 = SCL) and the
// Minima has no second I2C bus. Grounding A5 would hold SCL low and kill the
// LCD, so the jumper lives on D9 instead. Change this one line if you rewire it.
#define DEFAULTS_JUMPER_PIN     9       // D9  (P303), jumper to GND = defaults

// ─── WIZnet W5500 Lite Ethernet (hardware SPI) ─────────────────────────────
// SPI is fixed on the R4 Minima: MOSI=D11, MISO=D12, SCK=D13.
#define ETH_CS_PIN              10      // D10 (P112) chip select

// ─── LCD1602 + I2C backpack (PCF8574) ──────────────────────────────────────
// Wire: SDA = A4 (P101), SCL = A5 (P100).
// 0 = probe the bus for the two common backpack addresses (0x27, 0x3F).
#define LCD_I2C_ADDR            0x00
#define LCD_I2C_CLOCK           100000UL
#define LCD_COLS                16
#define LCD_ROWS                2

// How long the boot splash (system name over firmware revision) stays up
// before the address and readings replace it (ms). Also the *minimum* splash
// time while waiting for a DHCP lease.
#define LCD_NAME_HOLD_MS        4000

// LCD refresh tick (ms). Only characters that actually changed are re-sent.
#define LCD_REFRESH_MS          200

// ═══════════════════════════════════════════════════════════════════════════
// Network Configuration
// ═══════════════════════════════════════════════════════════════════════════

// MAC address (locally administered, unicast). Stored in EEPROM so each board
// on the same LAN can be given a unique address ('mac' telnet command).
#define MAC_ADDR                { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x10 }

// Factory defaults — also what the DEFAULTS_JUMPER_PIN jumper selects.
// The system boots in STATIC mode by default.
#define DEFAULT_USE_DHCP        false
#define DEFAULT_IP              IPAddress(192, 168, 1, 10)
#define DEFAULT_GATEWAY         IPAddress(192, 168, 1, 1)
#define DEFAULT_SUBNET          IPAddress(255, 255, 255, 0)
#define DEFAULT_DNS             IPAddress(192, 168, 1, 1)
#define DEFAULT_SYS_NAME        "ALS-lab"

// Max length of the system name, including the NUL terminator.
#define SYS_NAME_SIZE           24

// DHCP lease acquisition timeout (ms) and renewal check interval (ms).
#define DHCP_TIMEOUT_MS         12000
#define DHCP_MAINTAIN_MS        1000

// Server ports
#define WEB_SERVER_PORT         80
#define TELNET_PORT             23

// ═══════════════════════════════════════════════════════════════════════════
// Temperature Sensor
// ═══════════════════════════════════════════════════════════════════════════

// 12-bit resolution -> 750 ms conversion time. Keep some margin.
#define TEMP_RESOLUTION_BITS    12
#define TEMP_CONVERSION_MS      800

// How often a new conversion is started (ms).
#define TEMP_READ_INTERVAL      2000

// Consecutive failed reads tolerated before the sensor is declared lost.
// The OneWire library has no direct-GPIO backend for the RA4M1 and falls back
// to digitalRead/digitalWrite, which leaves the DS18B20's 15 us read window
// with little margin. A CRC failure therefore has to be treated as a glitch,
// not as an unplugged sensor, or the readout would flicker.
#define TEMP_MAX_ERRORS         3

// ═══════════════════════════════════════════════════════════════════════════
// Button Debounce
// ═══════════════════════════════════════════════════════════════════════════

#define DEBOUNCE_MS             50

// ═══════════════════════════════════════════════════════════════════════════
// Web Server
// ═══════════════════════════════════════════════════════════════════════════

// The W5500 has 8 hardware sockets. Two are consumed by the HTTP and telnet
// listeners, so keep the sum of the client pools at or below six.
#define HTTP_MAX_CLIENTS        4
#define SSE_MAX_CLIENTS         2

// Only the HTTP request line is buffered; the remaining headers are drained
// without being stored, so this need only fit "METHOD <uri> HTTP/1.1".
#define HTTP_REQLINE_SIZE       320

// Drop a half-open HTTP connection after this long with no progress (ms).
#define HTTP_CLIENT_TIMEOUT_MS  8000

// How often the full state is pushed to SSE clients even when nothing has
// changed (ms). This has to be a real data event, not an SSE ": ping"
// comment: comments never reach EventSource.onmessage, so a browser has no
// way to tell a live stream from one whose peer vanished. With a periodic
// data event the page can run a watchdog and reconnect itself — which is what
// has to happen after the board reboots, because the W5500 is reset without
// closing its TCP connections and the browser's socket stays half-open.
#define SSE_HEARTBEAT_MS        5000

// Client-side watchdog window, written into data/app.js for reference; the
// browser reconnects if no event arrives for this long.
#define SSE_WATCHDOG_MS         15000

// Cap on how long EthernetClient::stop() may block waiting for a graceful
// close. The library default is 1000 ms, which stalls the whole cooperative
// loop when a peer disappears mid-response.
#define HTTP_CLOSE_TIMEOUT_MS   150

// ═══════════════════════════════════════════════════════════════════════════
// Telnet Server
// ═══════════════════════════════════════════════════════════════════════════

#define TELNET_MAX_CLIENTS      2
#define TELNET_BUF_SIZE         128
