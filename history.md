# Session History – UNO R4 Minima Web Telnet Project

**Date:** 2026-09-02
**Project directory:** `/home/mirek/Projects/arduino_uno_minima_web_telnet`

---

## 1 – Project Creation Request

**User:** Create a VS Code + PlatformIO project for an Arduino UNO R4 Minima in
this directory, using `/home/mirek/Projects/pico_web_telnet` as the reference
for the git repository configuration and software structure. Full brief in
`project_description.txt`.

### Hardware from the brief

| Peripheral | Pin |
|---|---|
| LCD1602 + I2C backpack | I2C |
| WIZnet W5500 Lite | SPI, D10 = CS |
| DS18B20 | D7 |
| Encoder channels | A2, D3 |
| Encoder button | A3 |
| Defaults jumper | A5 to GND |

### Requirements from the brief

- Web server showing encoder count (reset on button press) and temperature
- Non-blocking temperature reads, 12-bit resolution
- Popup window for IP configuration: DHCP or static, IP/mask/gateway,
  DHCP system name (default `ALS-lab`)
- Jumper defaults: IP 192.168.1.10, gateway 192.168.1.1, mask 255.255.255.0
- Static addressing by default
- Settings saved to EEPROM on a save button; separate reboot button
- LCD line 1: system name, then the IP after 2 s (min 2 s while DHCP resolves)
- LCD line 2: temperature as NN.NN plus the encoder count
- Telnet server with short commands for count, reset, temperature, IP setup,
  system name, DHCP/static mode, save to EEPROM, reboot, help

---

## 2 – Platform Investigation

Before writing code, the installed Renesas core was inspected to check the
brief against the hardware. Three findings changed the design.

### 2.1 – A5 is the I2C SCL line

`variants/MINIMA/pins_arduino.h`:

```c
#define WIRE_SDA_PIN  18 /* A4 */
#define WIRE_SCL_PIN  19 /* A5 */
```

The Minima has no second I2C bus. Jumpering A5 to GND, as the brief asked,
would hold SCL low and the LCD would never respond.

**Resolution:** the defaults jumper moved to **A1** — free, in the same header
block, and a single `#define DEFAULTS_JUMPER_PIN` in `config.h` if it ever
needs to move back. Flagged to the user.

### 2.2 – Only ten pins can raise interrupts

`EXT_INTERRUPTS_HOWMANY 2` in the variant looked like a hard limit on
`attachInterrupt` calls, but reading `IRQManager.cpp` showed it is only a
compile guard; the real budget is `PROG_IRQ_NUM` (32 NVIC slots shared with
SPI, I2C, UART and USB). Each pin does however have one fixed ICU channel:

| Pin | D0 | D1 | D2 | D3 | D8 | D12 | D13 | A1 | A2 | A3 | A4 | A5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| IRQ | 6 | 5 | 0 | 1 | 9 | 3 | 4 | 6 | 7 | 2 | 1 | 2 |

The brief's encoder pins A2 (IRQ7) and D3 (IRQ1) are distinct, so they work.
The button on A3 is **polled** instead of interrupt driven — a 50 ms debounce
is far slower than the loop period, and it leaves an ICU channel free.

### 2.3 – No filesystem, and no `Print::printf`

The board has no filesystem partition, so the Pico project's LittleFS-served
`data/` directory could not be carried over; and the Renesas core's `Print`
class has no `printf`, which the Pico code used throughout.

---

## 3 – Design Decisions

### Single core

The Pico version ran the sensor on core 1. The RA4M1 has one core, so
everything became one cooperative loop with no blocking call in it — the
DS18B20 conversion, the LCD refresh and both servers all tick per iteration.

### Web assets in flash, generated from `data/`

Keeping editable HTML/CSS/JS mattered more than the delivery mechanism, so
`data/` survived and `tools/gen_web_assets.py` compiles those three files into
`src/web_assets.cpp` as a PlatformIO `pre:` hook. Round-trip fidelity was
verified by compiling the generated arrays on the host and diffing against the
originals — byte-identical, including UTF-8 characters. There is no
`uploadfs` step; `pio run -t upload` ships the UI.

### EEPROM record

One CRC-16/CCITT-protected record at address 0 with a magic number and a
version. Blank, wrong-version, bad-CRC and semantically-invalid records are all
treated as absent, so a virgin board and a corrupted board behave identically.

### Edit and save are separate

The brief asked for a save-to-EEPROM button *and* said settings are saved when
changed. Both front-ends resolve this the same way: setters stage into RAM,
and `POST /api/save` / telnet `w` is what writes flash. Network changes apply
after a reboot. Unsaved edits are surfaced as `cfgDirty` on the dashboard and
in telnet `n`. This also matches the reference project's edit → save → reboot
flow.

### Own LCD driver

A direct PCF8574/HD44780 implementation instead of `LiquidCrystal_I2C`: no
third-party dependency on a young core, exact control of the 4-bit init timing,
automatic address probing (0x27 then 0x3F), and — the reason that mattered — a
refresh that diffs against a shadow buffer and re-sends only changed
characters. At 100 kHz each character costs four I2C byte writes (~400 µs) and
the same loop serves HTTP and telnet, so rewriting both lines every tick would
have been a visible stall.

### Robust HTTP reading

The reference buffered the whole header block into 512 bytes and would wedge a
session if a browser sent more. This version stores **only the request line**
and drains the remaining headers through a rolling CRLF-CRLF matcher without
storing them, and drops a stalled socket after 8 s.

---

## 4 – Verification

The build is clean (no warnings from project sources):

```
RAM:   22.1% (7248 / 32768 bytes)
Flash: 47.1% (123520 / 262144 bytes)
```

Because no hardware was attached, the testable logic was exercised on the host
with small Arduino/EEPROM/Ethernet stubs:

| Area | Checks |
|---|---|
| `EepromConfig` | `parseIP` accept/reject, `parseMAC` both separators and multicast rejection, `formatMAC` round-trip, `setSysName` trimming and character rules, `validate` (non-contiguous mask, off-subnet gateway, gateway 0.0.0.0 allowed, network/broadcast/loopback/multicast IPs, DHCP mode ignoring static fields), save/load round-trip, CRC catching a flipped bit, erase |
| HTTP helpers | `urlDecode` (`+`, `%20`, invalid and truncated escapes), `queryParam` (first/middle/last key, absent key, prefix and suffix keys must not match, empty value, empty query, trailing `&`), `jsonEscape`, `ipToString` |
| LCD line 2 | exactly 16 columns across the full DS18B20 range and the count clamp |
| Web assets | generated arrays diffed byte-for-byte against `data/` |
| Dashboard | every id `app.js` looks up exists in the HTML; every class used in HTML or toggled from JS has a CSS rule; brackets balance |

Everything above passes. **Nothing has been run on real hardware** — the
DS18B20 timing caveat below is the item most likely to need attention there.

---

## 5 – Open Items / Caveats

1. **DS18B20 timing.** OneWire has no direct-register backend for the RA4M1 and
   falls back to `digitalRead`/`digitalWrite`, leaving the 15 µs read window
   with little margin. PlatformIO compiles libraries with `-w`, so the
   library's own warning about this is invisible. Mitigated by tolerating
   `TEMP_MAX_ERRORS` (3) consecutive bad reads before declaring the sensor
   lost. If readings prove unreliable, `OneWireNg` has a native Renesas RA
   backend.
2. **DHCP hostname on the wire.** Ethernet 2.0.2 hardcodes
   `#define HOST_NAME "WIZnet"` in `Dhcp.h` and sends `WIZnet` + the last three
   MAC bytes as DHCP option 12, with no runtime setter. The configurable system
   name is stored in EEPROM and used on the LCD, in the web UI and title, and
   in the telnet banner — but it is not what a DHCP server registers in DNS.
   Fixing that means patching one line in a vendored copy of the library, which
   was deliberately not done.
3. **MAC addresses.** All boards flashed from this repo share
   `DE:AD:BE:EF:FE:10`. Give each board its own with telnet `mac ... ; w ; rb`.
4. **PCF8574 clock.** Left at the part's rated 100 kHz. 400 kHz usually works
   and would quarter the refresh cost; `LCD_I2C_CLOCK` in `config.h`.
