// ─────────────────────────────────────────────────────────────────────────────
// UNO R4 Minima - Web + Telnet server
// ─────────────────────────────────────────────────────────────────────────────
// Hardware:
//   - Arduino UNO R4 Minima (Renesas RA4M1, single core, 48 MHz)
//   - WIZnet W5500 Lite Ethernet on hardware SPI, CS = D10
//   - LCD1602 with a PCF8574 I2C backpack on A4 (SDA) / A5 (SCL)
//   - DS18B20 temperature sensor on D7 (4.7k pull-up to 5 V)
//   - Quadrature encoder on A2 / D3, push button on A3
//   - D9 to GND at power-up: ignore the EEPROM and boot with the defaults
//
// Unlike the dual-core Pico this design is derived from, everything runs in a
// single cooperative loop(). Nothing in that loop blocks: the DS18B20 uses an
// asynchronous conversion state machine, the LCD only re-sends characters that
// changed, and both servers poll their sockets.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>

#include "config.h"
#include "eeprom_config.h"
#include "web_server.h"       // also defines AppState
#include "telnet_server.h"
#include "temperature.h"
#include "encoder.h"
#include "lcd_display.h"

// ─── Global state ────────────────────────────────────────────────────────────

static NetworkConfig g_config;      // staged/working configuration
static AppState      g_state;

static WebServer         g_web;
static TelnetServer      g_telnet;
static Encoder           g_encoder;
static TemperatureSensor g_temp;
static LcdDisplay        g_lcd;

// Reboots are deferred so the HTTP or telnet response can drain first.
static uint32_t g_rebootAt = 0;

// ─── Action callbacks shared by the web and telnet front-ends ────────────────

static void resetCountCb() {
    g_encoder.resetCount();
    Serial.println("[Main] encoder count reset");
}

static bool saveConfigCb(String& err) {
    if (!EepromConfig::validate(g_config, err)) return false;
    if (!EepromConfig::save(g_config)) {
        err = "EEPROM write failed";
        return false;
    }
    g_state.cfgDirty  = false;
    g_state.cfgStored = true;
    Serial.println("[Main] configuration written to EEPROM");
    return true;
}

static bool eraseConfigCb() {
    if (!EepromConfig::erase()) return false;
    g_state.cfgStored = false;
    // The RAM copy now differs from flash, so flag it as unsaved.
    g_state.cfgDirty  = true;
    Serial.println("[Main] stored configuration erased");
    return true;
}

static void rebootCb() {
    g_rebootAt = millis() + 300;
    if (g_rebootAt == 0) g_rebootAt = 1;    // 0 is the "no reboot pending" value
    Serial.println("[Main] reboot requested");
}

// ─── Network bring-up ────────────────────────────────────────────────────────

static void startEthernet() {
    SPI.begin();
    Ethernet.init(ETH_CS_PIN);

    if (g_config.useDhcp) {
        Serial.println("[Net] requesting a DHCP lease...");
        if (Ethernet.begin(g_config.mac, DHCP_TIMEOUT_MS) == 1) {
            g_state.dhcpBound = true;
            Serial.print("[Net] DHCP lease: ");
            Serial.println(Ethernet.localIP());
        } else {
            // Fall back to a static address so the board stays reachable
            // instead of dropping off the network entirely. The stored static
            // fields are not validated while DHCP is selected, so they may be
            // blank; in that case use the compile-time defaults.
            g_state.dhcpBound = false;
            NetworkConfig fb = g_config;
            fb.useDhcp = false;
            String why;
            if (!EepromConfig::validate(fb, why)) {
                uint8_t mac[6];
                memcpy(mac, g_config.mac, 6);
                EepromConfig::defaults(fb);
                memcpy(fb.mac, mac, 6);     // keep this board's own MAC
                Serial.println("[Net] DHCP failed and the stored static settings are "
                               "unusable - using the built-in defaults");
            } else {
                Serial.println("[Net] DHCP failed - falling back to the static settings");
            }
            Ethernet.begin(fb.mac, fb.ip, fb.dns, fb.gateway, fb.subnet);
        }
    } else {
        Ethernet.begin(g_config.mac, g_config.ip, g_config.dns,
                       g_config.gateway, g_config.subnet);
        Serial.print("[Net] static address: ");
        Serial.println(Ethernet.localIP());
    }

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("[Net] ERROR: no W5500 detected - check the SPI wiring and CS pin");
    }
    if (Ethernet.linkStatus() == LinkOFF) {
        Serial.println("[Net] WARNING: Ethernet cable is not connected");
    }

    g_state.ip      = Ethernet.localIP();
    g_state.gateway = Ethernet.gatewayIP();
    g_state.subnet  = Ethernet.subnetMask();
    g_state.dns     = Ethernet.dnsServerIP();
    g_state.linkUp  = (Ethernet.linkStatus() != LinkOFF);
}

// ═════════════════════════════════════════════════════════════════════════════
// setup
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    // Native USB CDC: give a host that is already attached a moment to open
    // the port, but never wait for one that is not there. Keep this short —
    // it delays the whole boot, and after a reboot the browser is already
    // trying to reconnect. A monitor that re-enumerates after a reset will
    // miss the first lines whatever value is used here.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 300) { /* wait briefly */ }
    Serial.println("\n[Main] UNO R4 Minima web/telnet server booting...");

    // ── Encoder and button ────────────────────────────────────────────────
    g_encoder.begin();

    // ── Configuration: jumper wins over the EEPROM ────────────────────────
    EepromConfig::begin();
    pinMode(DEFAULTS_JUMPER_PIN, INPUT_PULLUP);
    delayMicroseconds(50);                      // let the pull-up settle
    bool jumpered = (digitalRead(DEFAULTS_JUMPER_PIN) == LOW);

    g_state.defaultsJumper = jumpered;
    g_state.cfgStored      = EepromConfig::isStored();

    if (jumpered) {
        EepromConfig::defaults(g_config);
        Serial.println("[Main] defaults jumper grounded - using the built-in defaults");
    } else if (EepromConfig::load(g_config)) {
        Serial.println("[Main] configuration loaded from EEPROM");
    } else {
        Serial.println("[Main] no valid EEPROM record - using the built-in defaults");
    }
    // The staged copy matches flash only if it actually came from flash.
    g_state.cfgDirty = jumpered ? g_state.cfgStored : false;
    g_state.pending  = &g_config;

    Serial.print("[Main] system name: ");
    Serial.println(g_config.sysName);

    // ── LCD ───────────────────────────────────────────────────────────────
    // Paint the system name before Ethernet comes up: a DHCP request blocks
    // for seconds, and the display must already be showing the name by then.
    g_lcd.begin();
    g_lcd.setSysName(g_config.sysName);
    g_lcd.setCount(0);
    g_lcd.setTemperature(0.0f, false);
    g_lcd.setNetwork(IPAddress(0, 0, 0, 0), false);
    g_lcd.update();

    // ── Temperature sensor ────────────────────────────────────────────────
    g_temp.begin();

    // ── Network ───────────────────────────────────────────────────────────
    startEthernet();
    g_lcd.setNetwork(g_state.ip, g_state.ip != IPAddress(0, 0, 0, 0));

    // ── Servers ───────────────────────────────────────────────────────────
    g_web.onResetCount = resetCountCb;
    g_web.onSaveConfig = saveConfigCb;
    g_web.onReboot     = rebootCb;
    g_web.begin(&g_state);

    g_telnet.onResetCount  = resetCountCb;
    g_telnet.onSaveConfig  = saveConfigCb;
    g_telnet.onEraseConfig = eraseConfigCb;
    g_telnet.onReboot      = rebootCb;
    g_telnet.begin(&g_state);

    Serial.println("[Main] ready.");
}

// ═════════════════════════════════════════════════════════════════════════════
// loop
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    // ── Inputs ────────────────────────────────────────────────────────────
    g_encoder.update();
    g_temp.update();

    bool stateChanged = false;

    // The encoder button zeroes the count, exactly like the web button.
    if (g_encoder.buttonPressed()) {
        g_encoder.resetCount();
        Serial.println("[Main] encoder button pressed - count reset");
    }

    if (g_encoder.countChanged()) {
        g_state.encoderCount = g_encoder.count();
        g_lcd.setCount(g_state.encoderCount);
        stateChanged = true;
    }

    if (g_temp.changed()) {
        g_state.temperature = g_temp.get();
        g_state.tempValid   = g_temp.valid();
        g_lcd.setTemperature(g_temp.get(), g_temp.valid());
        stateChanged = true;
    }

    // ── DHCP lease renewal ────────────────────────────────────────────────
    if (g_config.useDhcp && g_state.dhcpBound) {
        static uint32_t lastMaintain = 0;
        uint32_t now = millis();
        if (now - lastMaintain >= DHCP_MAINTAIN_MS) {
            lastMaintain = now;
            int rc = Ethernet.maintain();
            // 1/3 = renew/rebind failed, 2/4 = renew/rebind succeeded.
            if (rc == 2 || rc == 4) {
                g_state.ip      = Ethernet.localIP();
                g_state.gateway = Ethernet.gatewayIP();
                g_state.subnet  = Ethernet.subnetMask();
                g_state.dns     = Ethernet.dnsServerIP();
                g_lcd.setNetwork(g_state.ip, true);
                stateChanged = true;
                Serial.print("[Net] DHCP lease renewed: ");
                Serial.println(g_state.ip);
            } else if (rc == 1 || rc == 3) {
                Serial.println("[Net] DHCP renewal failed");
            }
        }
    }

    // ── Link state ────────────────────────────────────────────────────────
    {
        static uint32_t lastLinkCheck = 0;
        uint32_t now = millis();
        if (now - lastLinkCheck >= 1000) {
            lastLinkCheck = now;
            bool up = (Ethernet.linkStatus() != LinkOFF);
            if (up != g_state.linkUp) {
                g_state.linkUp = up;
                stateChanged   = true;
            }
        }
    }

    // Keep the display in step with a name changed over web or telnet.
    g_lcd.setSysName(g_config.sysName);

    // ── Outputs ───────────────────────────────────────────────────────────
    g_lcd.update();
    g_web.update(stateChanged);
    g_telnet.update();

    // ── Deferred reboot ───────────────────────────────────────────────────
    if (g_rebootAt != 0 && (int32_t)(millis() - g_rebootAt) >= 0) {
        Serial.println("[Main] rebooting now");
        Serial.flush();
        NVIC_SystemReset();
    }
}
