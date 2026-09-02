#pragma once
#include <Arduino.h>
#include "config.h"

// ─── Persistent configuration ────────────────────────────────────────────────
// Runtime view of everything the user can change. Held in RAM, written to the
// RA4M1 data-flash ("EEPROM") only when the user asks for it — the web popup's
// "Save to EEPROM" button or the telnet 'w' command.

struct NetworkConfig {
    bool      useDhcp;                  // false = static addressing
    IPAddress ip;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns;
    uint8_t   mac[6];
    char      sysName[SYS_NAME_SIZE];   // DHCP / display name, NUL terminated
};

// ─── EEPROM-backed config store ──────────────────────────────────────────────
// The UNO R4 Minima has no filesystem; the 8 KB data-flash block emulated by
// the core's EEPROM library holds one CRC-protected record at address 0.
//
// A record that is blank, of the wrong version, or fails its CRC is treated as
// absent, and the compile-time defaults from config.h are used instead. That
// makes a virgin board and a corrupted board behave identically.

class EepromConfig {
public:
    // Report the emulated EEPROM size to the serial console.
    static void begin();

    // Fill cfg with the compile-time defaults from config.h.
    static void defaults(NetworkConfig& cfg);

    // Load the stored record. Returns false (and applies defaults) when no
    // valid record is present.
    static bool load(NetworkConfig& cfg);

    // Write cfg as the stored record. Returns false if the read-back check
    // fails. Erases/rewrites one data-flash block, so call it sparingly.
    static bool save(const NetworkConfig& cfg);

    // Invalidate the stored record so the next boot falls back to defaults.
    static bool erase();

    // True when a valid record is currently stored.
    static bool isStored();

    // Total emulated EEPROM size and the bytes our record occupies.
    static size_t capacity();
    static size_t recordSize();

    // ── Helpers shared by the web and telnet front-ends ────────────────────

    // Parse dotted-decimal IPv4. Returns false on malformed input.
    static bool parseIP(const char* s, IPAddress& out);

    // Copy a user-supplied system name into cfg, trimming and rejecting
    // control characters. Returns false if the name is empty or too long.
    static bool setSysName(NetworkConfig& cfg, const char* name);

    // Parse "de:ad:be:ef:fe:10" (or with '-'/'.' separators) into mac[6].
    static bool parseMAC(const char* s, uint8_t mac[6]);

    // Format mac[6] as "DE:AD:BE:EF:FE:10" into a >=18 byte buffer.
    static void formatMAC(const uint8_t mac[6], char* out, size_t outLen);

    // Reject configurations that would strand the board off the network.
    // On failure 'why' receives a short human-readable reason.
    static bool validate(const NetworkConfig& cfg, String& why);
};
