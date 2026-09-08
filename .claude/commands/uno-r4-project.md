# UNO R4 Minima Web Telnet – Project Skill

You are working on the **UNO R4 Minima Web + Telnet Server** at
`/home/mirek/Projects/arduino_uno_minima_web_telnet`. Use the context below
instead of re-reading every file.

Reference sibling project (the design this was ported from):
`/home/mirek/Projects/pico_web_telnet` (RP2040, dual core, LittleFS).

---

## Purpose

PlatformIO + Arduino firmware for an Arduino UNO R4 Minima (Renesas RA4M1)
providing:
- An HTTP dashboard with real-time SSE updates (encoder count, temperature)
- A modal IP-configuration popup with save-to-EEPROM and reboot buttons
- A terse telnet management console
- A 16x2 I2C LCD showing the system name / IP and temperature / count
- Non-blocking DS18B20 measurement at 12-bit resolution
- Network settings in a CRC-protected record in the 8 KB data flash

---

## Board Facts That Constrain The Design

| Fact | Consequence |
|---|---|
| Single core (48 MHz Cortex-M4) | no `setup1`/`loop1`; one cooperative loop, nothing may block |
| 256 KB flash, 32 KB RAM | comfortable: ~47% flash, ~22% RAM |
| **No filesystem partition** | web assets are compiled into flash; no `uploadfs` target |
| 8 KB data flash as EEPROM | `#include <EEPROM.h>`, `EEPROM.get/put/length` all work |
| `Print::printf` does **not** exist | use `snprintf` into a buffer, then `print()` |
| `Wire` is fixed on A4 (SDA) / A5 (SCL) | **A5 cannot be a GPIO jumper** — this is why the defaults jumper is D9, not A5 as the brief asked |
| Only 10 pins have an ICU IRQ channel | see the table below before moving any interrupt pin |
| Reboot | `NVIC_SystemReset()` |

### Interrupt-capable pins (MINIMA variant)

| Pin | D0 | D1 | D2 | D3 | D8 | D12 | D13 | A1 | A2 | A3 | A4 | A5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| IRQ channel | 6 | 5 | 0 | 1 | 9 | 3 | 4 | 6 | 7 | 2 | 1 | 2 |

Pins sharing a channel cannot both be used. Encoder A2 (IRQ7) + D3 (IRQ1) are
distinct, hence valid. Source of truth:
`~/.platformio/packages/framework-arduinorenesas-uno/variants/MINIMA/pinmux.inc`
(grep for `PIN_INTERRUPT`) and `pins_arduino.h` for `WIRE_SDA_PIN`/`WIRE_SCL_PIN`.

---

## Pin Map

| Pin | Function |
|---|---|
| D3 | encoder channel B (CHANGE IRQ) |
| D7 | DS18B20 data (4.7 kΩ pull-up to 5 V) |
| D9 | defaults jumper, to GND = factory defaults (read once at boot) |
| D10 | W5500 CS |
| D11/D12/D13 | SPI MOSI/MISO/SCK |
| A2 | encoder channel A (CHANGE IRQ) |
| A3 | encoder button (`INPUT_PULLUP`, LOW = pressed, polled) |
| A4/A5 | LCD1602 I2C SDA/SCL (PCF8574 at 0x27 or 0x3F, auto-probed) |

---

## Architecture

```
setup()
  encoder.begin()            attach A2/D3 interrupts, seed decoder state
  EepromConfig::begin()      report data-flash size
  read D9 jumper             jumper wins over the stored record
  load config or defaults
  lcd.begin() + lcd.update() paint the name BEFORE Ethernet (DHCP blocks)
  temperature.begin()        probe, set 12-bit resolution
  startEthernet()            static, or DHCP with static fallback
  web.begin(), telnet.begin()

loop()                       nothing in here blocks
  encoder.update()           button debounce only; channels are interrupt driven
  temperature.update()       IDLE -> CONVERTING -> IDLE state machine
  Ethernet.maintain()        only while DHCP-bound
  lcd.update()               diff vs shadow buffer, 200 ms tick
  web.update(stateChanged)   accept/read/route, push SSE on change
  telnet.update()
  deferred reboot check      NVIC_SystemReset() ~300 ms after a request
```

### Shared state

```cpp
struct AppState {                       // web_server.h, global in main.cpp
    volatile float   temperature;
    volatile bool    tempValid;
    volatile int32_t encoderCount;
    bool      linkUp, dhcpBound;
    IPAddress ip, gateway, subnet, dns; // as actually applied
    NetworkConfig* pending;             // -> g_config, the editable copy
    bool      cfgDirty, cfgStored, defaultsJumper;
};
```

### Edit / save separation (deliberate)

Web popup and telnet setters both mutate `AppState::pending` in RAM only.
Flash is written by **`POST /api/save`** or the telnet **`w`** command; network
settings apply after a reboot. `cfgDirty` surfaces unsaved edits on the
dashboard and in telnet `n`. Every value goes through
`EepromConfig::validate()` first, which rejects non-contiguous netmasks,
off-subnet gateways, network/broadcast host addresses, loopback/multicast IPs
and multicast MACs.

---

## Firmware Revision Comes From The Git Tag

`tools/gen_version.py` is a `pre:` hook that defines `FIRMWARE_VERSION` from
`git describe --tags --dirty --always` (tags here look like `R01.00`, the
user's convention — do not impose `vX.Y.Z`). `include/version.h` holds the
`"unknown"` fallback. Surfaced at boot on serial, in the web IP-configuration
popup (`fw` field of `/api/netcfg`), and by the telnet `v` command plus the
session banner.

Injected as a `-D`, deliberately **not** a generated source file: a committed
file holding `git describe` output is rewritten after every commit and leaves
the tree permanently dirty. Do not "improve" it into a generated file.

## Web Assets Are Generated

`data/index.html`, `data/style.css`, `data/app.js` are the sources you edit.
`tools/gen_web_assets.py` (a `pre:` `extra_scripts` hook) folds all three into
**one** document in `src/web_assets.cpp` on every build; that file is generated
and committed — **never hand-edit it**. It only rewrites when content changed,
so unchanged assets do not force a recompile.

The single-document layout is not cosmetic: see the socket budget below.

Run it standalone with `python3 tools/gen_web_assets.py`.

---

## Telnet Commands

```
?  h  |  v  |  s  |  c  |  cr  |  t  |  n
nm 0|1  ni <addr>  ns <mask>  ng <addr>  nd <addr>  nn <name>  mac <addr>
d (stage defaults)  w (write EEPROM)  e (erase)  rb (reboot)
p [ms] (poll)  x (stop)  q (quit)
```

## REST API

```
GET  /  /events  /api/status  /api/netcfg
POST /api/reset  /api/netcfg?dhcp&ip&sn&gw&dns&name&mac  /api/save  /api/reboot
```

`/` is one self-contained document: `tools/gen_web_assets.py` inlines
`data/style.css` and `data/app.js` into `data/index.html` at build time, and an
empty `data:` favicon suppresses `/favicon.ico`. There are deliberately **no**
`/style.css` or `/app.js` routes.

---

## Build Commands

PlatformIO is not on `PATH`:

```bash
~/.platformio/penv/bin/pio run                  # build
~/.platformio/penv/bin/pio run -t upload        # flash over USB DFU
~/.platformio/penv/bin/pio device monitor       # serial console @115200
```

There is no `uploadfs` target on this board.

---

## Defaults (include/config.h)

```
Mode:        static          System name:  ALS-lab
IP:          192.168.1.10    MAC:          DE:AD:BE:EF:FE:10
Gateway:     192.168.1.1     Web port:     80
Subnet:      255.255.255.0   Telnet port:  23
DNS:         192.168.1.1

LCD_NAME_HOLD_MS 2000   LCD_REFRESH_MS 200   LCD_I2C_CLOCK 100000
TEMP_RESOLUTION_BITS 12  TEMP_CONVERSION_MS 800  TEMP_READ_INTERVAL 2000
TEMP_MAX_ERRORS 3        DEBOUNCE_MS 50       ENC_EDGES_PER_DETENT 4
HTTP_MAX_CLIENTS 4       SSE_MAX_CLIENTS 2    TELNET_MAX_CLIENTS 2
```

W5500 has 8 sockets; two are listeners, so keep
`HTTP_MAX_CLIENTS + TELNET_MAX_CLIENTS <= 6`. It sits exactly on that ceiling
at 4 + 2, which is why the dashboard must be a single request — a page pulling
four files plus `/events` exceeded the client pool and had a request refused,
showing up as "the page needs reloading two or three times".

---

## Gotchas Already Handled

- The defaults jumper is D9 because A5 is I2C SCL (see above).
- The HTTP reader stores **only the request line** and drains the rest of the
  headers unstored, so a large browser header block cannot wedge a session;
  stalled sockets are dropped after `HTTP_CLIENT_TIMEOUT_MS`.
- The LCD is painted once in `setup()` *before* Ethernet, because a DHCP
  request blocks for seconds and the name must already be visible.
- The boot splash shows **only** the centred system name; line 2 is held
  blank until `_splashDone` latches (name up for `LCD_NAME_HOLD_MS` *and*
  `_ipValid`). The latch is evaluated once per refresh in `update()` so both
  lines always agree, and it is one-way so a link glitch cannot revive it.
- `LcdDisplay::begin()` back-dates `_lastRefresh` so that first paint happens.
- DHCP failure falls back to static; if the stored static fields are blank
  (they are not validated in DHCP mode) it falls back to the compile-time
  defaults while keeping the board's own MAC.
- The deferred-reboot timestamp is nudged off 0, which is the sentinel value.
- OneWire has no RA4M1 direct-GPIO backend and runs in fallback mode
  (PlatformIO's `-w` hides its `#warning`), so DS18B20 read timing is tight;
  `TEMP_MAX_ERRORS` consecutive failures are tolerated before the sensor is
  declared lost. `OneWireNg` is the fallback if it proves unreliable.
- **Surviving a reboot** was the hard part of the web UI, and three things
  make it work; do not undo any of them.
  1. The board pushes full state on change *and* every `SSE_HEARTBEAT_MS` as a
     real `data:` event. An SSE `": ping"` comment does **not** reach
     `EventSource.onmessage`, so it gives the page nothing to measure.
  2. `data/app.js` runs a `WATCHDOG_MS` timer and reconnects when no event
     arrives. `NVIC_SystemReset()` resets the W5500 without closing its TCP
     connections, so the browser keeps a half-open socket, `onerror` never
     fires, and the status dot would stay green forever with no data. The dot
     tracks *data arriving*, not socket state.
  3. `/events` **evicts** the oldest stream when `SSE_MAX_CLIENTS` is reached
     instead of returning 503 — a reload has to win, and it happens exactly
     when the previous socket has not finished closing.
- `EthernetClient::stop()` blocks up to its `Stream` timeout (1000 ms default)
  waiting for a graceful close, which stalls the whole loop. Accepted clients
  get `setTimeout(HTTP_CLOSE_TIMEOUT_MS)`.
- The `while (!Serial)` wait in `setup()` is capped at 300 ms, not 1500: it
  delays every boot, and a re-enumerating monitor misses the first lines anyway.
- Ethernet 2.0.2 hardcodes the DHCP option-12 hostname to `WIZnet<MAC>`; the
  configurable system name is not sent on the wire. Documented, not patched.
