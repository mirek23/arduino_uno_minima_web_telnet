# UNO R4 Minima Web + Telnet Server

Arduino UNO R4 Minima firmware providing a web dashboard and a telnet
management console over a WIZnet W5500 Lite Ethernet module, with a 16x2 I2C
LCD, a DS18B20 temperature sensor and a quadrature rotary encoder.

Ported from the `pico_web_telnet` project (RP2040, dual core, LittleFS) and
adapted to a single-core Renesas RA4M1 with no filesystem partition.

---

## Contents

- [Hardware](#hardware) — pin map and the two board constraints that shaped it
- [Memory Layout](#memory-layout) — why the web UI lives in program flash
- [Network](#network) — defaults, boot precedence, edit/save separation
- [MAC addresses](#mac-addresses)
- [LCD Behaviour](#lcd-behaviour)
- [Encoder](#encoder)
- [Temperature](#temperature)
- [Building & Flashing](#building--flashing)
- [Web Dashboard](#web-dashboard) and [REST API](#rest-api)
- [Telnet Console](#telnet-console)
- [Single-Core Architecture](#single-core-architecture)
- [Source Layout](#source-layout)
- [Libraries](#libraries)
- [Known Limitations](#known-limitations)

---

## Hardware

| Peripheral | Pin | Port | Notes |
|---|---|---|---|
| DS18B20 data | D7 | P107 | 4.7 kΩ pull-up to 5 V required |
| Encoder channel A | A2 | P001 | `INPUT_PULLUP`, CHANGE interrupt (IRQ7) |
| Encoder channel B | D3 | P104 | `INPUT_PULLUP`, CHANGE interrupt (IRQ1) |
| Encoder button | A3 | P002 | `INPUT_PULLUP`, LOW = pressed, polled |
| Defaults jumper | **D9** | P303 | Jumper to GND at power-up = factory defaults |
| LCD1602 SDA | A4 | P101 | PCF8574 backpack, address 0x27 or 0x3F |
| LCD1602 SCL | A5 | P100 | |
| W5500 CS | D10 | P112 | |
| W5500 MOSI | D11 | P109 | hardware SPI |
| W5500 MISO | D12 | P110 | hardware SPI |
| W5500 SCK | D13 | P111 | hardware SPI |

### Two board constraints worth knowing

**The defaults jumper is on D9, not A5.** The project brief asked for A5, but on
the UNO R4 Minima `A5` (P100) *is* the I2C SCL line — `Wire` is hard-wired to
A4/SDA and A5/SCL, and the Minima has no second I2C bus. Grounding A5 would hold
SCL low and the LCD would never respond. D9 is free and behaves identically; the
pin is read once at boot, so it does not need to be interrupt-capable. It is a
single `#define` in `include/config.h`:

```c
#define DEFAULTS_JUMPER_PIN     9       // D9
```

**Only some pins can raise interrupts.** The RA4M1's ICU gives each pin a fixed
IRQ channel, and the Minima variant only wires up ten of them:

| Pin | D0 | D1 | D2 | D3 | D8 | D12 | D13 | A1 | A2 | A3 | A4 | A5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| IRQ channel | 6 | 5 | 0 | 1 | 9 | 3 | 4 | 6 | 7 | 2 | 1 | 2 |

Two pins that share a channel cannot both carry an interrupt. The encoder uses
A2 (IRQ7) and D3 (IRQ1), which are distinct, so the assignment is valid. Move
either channel and check this table first.

---

## Memory Layout

| Region | Size |
|---|---|
| Program flash | 256 KB (about 47% used) |
| SRAM | 32 KB (about 22% used) |
| Data flash used as EEPROM | 8 KB |

There is no filesystem partition on this board, so `data/index.html`,
`data/style.css` and `data/app.js` are compiled into program flash by
`tools/gen_web_assets.py`, which PlatformIO runs before every build. **Edit the
files in `data/`** — `src/web_assets.cpp` is generated and gets overwritten.

There is no `uploadfs` step: `pio run -t upload` ships the web UI too.

---

## Network

| Setting | Default |
|---|---|
| Mode | static |
| IP address | 192.168.1.10 |
| Gateway | 192.168.1.1 |
| Subnet mask | 255.255.255.0 |
| DNS | 192.168.1.1 |
| System name | ALS-lab |
| MAC | DE:AD:BE:EF:FE:10 |
| Web port | 80 |
| Telnet port | 23 |

Settings live in one CRC-protected record in the RA4M1 data flash. At boot:

1. If **D9 is jumpered to GND**, the compile-time defaults are used and the
   stored record is ignored for that session.
2. Otherwise the stored record is loaded. A record that is blank, of the wrong
   version, fails its CRC, or fails the sanity checks is treated as absent and
   the defaults apply — so a virgin board and a corrupted board behave alike.

If DHCP is selected but no lease arrives within `DHCP_TIMEOUT_MS`, the board
falls back to a static address rather than dropping off the network (the stored
static settings if they are usable, otherwise the compile-time defaults).

### Editing is separate from saving

Both front-ends stage changes in RAM and only touch flash when you ask:

```text
web:     [edit the popup]  ->  Save to EEPROM   ->  Reboot system
telnet:  ni / ns / ng / nd / nn / nm / mac  ->  w  ->  rb
```

Nothing is written to flash until the save, and network settings take effect
after the reboot. Unsaved edits show as a warning on the dashboard and in the
telnet `n` output. Every value is validated before it is staged: a bad netmask,
an off-subnet gateway, a network or broadcast host address, or a multicast MAC
are all refused with a reason, which makes it hard to save a configuration that
would strand the board.

---

## MAC addresses

Every board flashed from this repository starts with the same MAC. Two of them
on one LAN will fight, so give each board its own:

```text
> mac de:ad:be:ef:fe:11
> w
> rb
```

---

## LCD Behaviour

```text
Line 1   ALS-lab            <- system name, first 2 s
         192.168.1.10       <- then the address
Line 2    23.45°C E:  128   <- temperature and encoder count
```

Line 1 shows the system name for at least `LCD_NAME_HOLD_MS` (2 s) and then the
IP address. In DHCP mode the name stays up until the lease lands, so the 2 s
minimum always holds — the name is already on the display before Ethernet is
brought up, because a DHCP request blocks for seconds.

Line 2 is always exactly 16 columns: temperature as `NN.NN` with two decimals
plus the degree sign, and the encoder count. `--.--` means no sensor.

The driver is a direct PCF8574/HD44780 implementation rather than a library, so
the refresh can diff against a shadow buffer and re-send only the characters
that changed. That matters because each character costs four I2C byte writes
(~400 µs at 100 kHz) and the same loop also serves HTTP and telnet. The
backpack address is probed automatically (0x27 then 0x3F); if nothing answers,
every LCD call becomes a no-op and the rest of the firmware carries on.

---

## Encoder

Channels A and B are decoded with a full quadrature state table on both edges
of both channels, so nothing is lost and a direction reversal inside one detent
cannot produce a phantom count. `ENC_EDGES_PER_DETENT` (4, the usual EC11
figure) converts edges to clicks — set it to 1 to count every edge.

The count is reset by the **encoder button**, the dashboard's **Reset count**
button, or the telnet **`cr`** command.

---

## Temperature

DS18B20 at 12-bit resolution (0.0625 °C steps, 750 ms conversion), read through
a non-blocking state machine:

```text
IDLE  --(TEMP_READ_INTERVAL elapsed)-->  request conversion
CONVERTING  --(TEMP_CONVERSION_MS elapsed)-->  latch result  -->  IDLE
```

Nothing waits on the sensor: `loop()` keeps servicing sockets throughout the
750 ms conversion. A missing sensor is re-probed every cycle, so one can be
plugged in while the board is running.

> **Timing caveat.** OneWire has no direct-register backend for the RA4M1 and
> falls back to `digitalRead`/`digitalWrite`, which leaves the DS18B20's 15 µs
> read window with little margin. (PlatformIO compiles libraries with `-w`, so
> the library's own `#warning` about this is invisible in the build log.) The
> driver therefore tolerates `TEMP_MAX_ERRORS` (3) consecutive bad reads before
> declaring the sensor lost, so a CRC glitch does not make the readout flicker.
> If readings turn out to be unreliable on your hardware, swap OneWire for
> `OneWireNg`, which has a native Renesas RA backend.

---

## Building & Flashing

PlatformIO's CLI is not on `PATH` by default:

```bash
# Build
~/.platformio/penv/bin/pio run

# Flash over USB (DFU)
~/.platformio/penv/bin/pio run -t upload

# Serial console
~/.platformio/penv/bin/pio device monitor
```

Double-tap RESET if the board does not enter the bootloader on its own.

### Linux udev rules (one-time, needs sudo)

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG dialout,plugdev $USER
```

---

## Web Dashboard

Open `http://192.168.1.10/`.

- **Encoder** — live count and a reset button
- **Temperature** — SVG gauge, two decimals, colour coded
- **Network** — system name, address, mode, link state
- **IP configuration** — the header button opens a modal with the addressing
  mode (DHCP or static), IP, mask, gateway, DNS, system name and MAC, plus
  **Save to EEPROM** and **Reboot system**
- **Event log** — timestamped state changes

Updates are event-driven over
[Server-Sent Events](https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events)
(`/events`); the browser reconnects on its own if the link drops.

### REST API

| Method | URL | Description |
|---|---|---|
| GET | `/` | dashboard |
| GET | `/style.css`, `/app.js` | assets, served from flash |
| GET | `/events` | SSE stream |
| GET | `/api/status` | live state snapshot |
| GET | `/api/netcfg` | staged config plus what is currently active |
| POST | `/api/reset` | zero the encoder count |
| POST | `/api/netcfg?...` | validate and stage config changes |
| POST | `/api/save` | write the staged config to EEPROM |
| POST | `/api/reboot` | restart the board |

`POST /api/netcfg` accepts any subset of
`dhcp=0|1`, `ip`, `sn`, `gw`, `dns`, `name`, `mac` as query parameters and
replies `{"ok":true|false,"msg":"...","dirty":bool}`.

SSE payload:

```json
{"temp":23.45,"tempOk":true,"count":128,"name":"ALS-lab","ip":"192.168.1.10",
 "dhcp":false,"bound":false,"link":true,"dirty":false,"up":412}
```

---

## Telnet Console

```bash
telnet 192.168.1.10
```

Commands are deliberately terse:

```text
?  h        this help
s           full status
c           encoder count
cr          reset encoder count
t           temperature
n           show network settings (staged and active)
nm 0|1      mode: 0 = static, 1 = DHCP
ni <addr>   static IP address
ns <mask>   subnet mask
ng <addr>   gateway
nd <addr>   DNS server
nn <name>   system name
mac <addr>  MAC address (de:ad:be:ef:fe:10)
d           stage the factory defaults
w           write staged settings to EEPROM
e           erase stored settings
rb          reboot
p [ms]      poll status (default 1000 ms)
x           stop polling
q           disconnect
```

### Example

```text
> n
Name:   ALS-lab
MAC:    DE:AD:BE:EF:FE:10
Mode:   STATIC
Active: ip=192.168.1.10 sn=255.255.255.0 gw=192.168.1.1 dns=192.168.1.1
Staged: ip=192.168.1.10 sn=255.255.255.0 gw=192.168.1.1 dns=192.168.1.1
Link:   up | EEPROM: stored

> ni 192.168.1.50
Staged. Use 'w' to write EEPROM, then 'rb' to apply.

> ng 10.0.0.1
ERROR: gateway is not on the same subnet as the IP address

> w
Configuration written to EEPROM

> rb
Rebooting...
```

---

## Single-Core Architecture

The Pico version this is derived from used its second core for the sensor. The
RA4M1 has one core, so everything is one cooperative loop with no blocking
call in it:

```text
loop()
  encoder.update()      debounce the button (channels are interrupt driven)
  temperature.update()  tick the async conversion state machine
  Ethernet.maintain()   DHCP lease renewal, when in DHCP mode
  lcd.update()          re-send only changed characters, every 200 ms
  web.update()          accept, read, route; push SSE on change
  telnet.update()       accept, read lines, dispatch commands
```

The HTTP reader buffers **only the request line** and drains the remaining
headers without storing them, so a browser sending a large header block cannot
wedge a session; a connection that stalls is dropped after
`HTTP_CLIENT_TIMEOUT_MS`. Reboots are deferred a few hundred milliseconds so
the HTTP or telnet response can drain first.

The W5500 has 8 sockets. Two are taken by the HTTP and telnet listeners, so
keep `HTTP_MAX_CLIENTS + TELNET_MAX_CLIENTS` at six or below.

---

## Source Layout

| File | Purpose |
|---|---|
| `platformio.ini` | build config, libraries, pre-build asset hook |
| `include/config.h` | every pin, address and timing constant — start here |
| `src/main.cpp` | boot sequence, the single loop, action callbacks |
| `*/eeprom_config.*` | the config record, CRC, validation, parsing helpers |
| `*/encoder.*` | quadrature decode + button debounce |
| `*/temperature.*` | non-blocking DS18B20 |
| `*/lcd_display.*` | PCF8574/HD44780 driver and the two-line display policy |
| `*/web_server.*` | HTTP, SSE, REST API |
| `*/telnet_server.*` | telnet console |
| `include/web_assets.h` | declarations for the flash-resident web assets |
| `src/web_assets.cpp` | **generated** — do not edit |
| `data/` | the dashboard sources you *do* edit |
| `tools/gen_web_assets.py` | turns `data/` into `src/web_assets.cpp` |

---

## Libraries

| Library | Version | Purpose |
|---|---|---|
| `arduino-libraries/Ethernet` | ^2.0.2 | W5500 driver (`EthernetServer::accept()`) |
| `paulstoffregen/OneWire` | ^2.3.8 | 1-Wire bus (generic fallback on RA4M1) |
| `milesburton/DallasTemperature` | ^3.11.0 | DS18B20, async mode |
| `EEPROM`, `Wire`, `SPI` | core | data flash, I2C, SPI |

The LCD has no library dependency; see `src/lcd_display.cpp`.

---

## Known Limitations

- **The DHCP hostname on the wire is not the system name.** Ethernet 2.0.2
  hardcodes `#define HOST_NAME "WIZnet"` in `Dhcp.h` and sends
  `WIZnet` + the last three MAC bytes as DHCP option 12; the library exposes no
  runtime setter. The configurable system name is used on the LCD, in the web
  UI and page title, and in the telnet banner, and it is stored in EEPROM as
  specified — it just is not what a DHCP server registers in DNS. Getting it on
  the wire means patching that one line in a vendored copy of the library,
  which this project deliberately does not do.
- `Print::printf` does not exist on this core; use `snprintf` into a buffer.
  All output paths here already do.
- The LCD clamps the displayed count to five characters (-9999..99999). The web
  and telnet views always show the true value.
