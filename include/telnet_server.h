#pragma once
#include <Arduino.h>
#include <Ethernet.h>
#include "config.h"
#include "web_server.h"   // for AppState

// ─── Telnet client session ───────────────────────────────────────────────────
struct TelnetClient {
    EthernetClient client;
    bool           active;
    char           lineBuf[TELNET_BUF_SIZE];
    uint8_t        lineLen;

    // Auto-poll state
    bool           polling;
    uint32_t       pollInterval;
    uint32_t       lastPollTime;
};

// ─── Telnet Server ───────────────────────────────────────────────────────────
//
// Deliberately terse commands, since these get typed a lot:
//
//   ?  h          this help
//   v             firmware revision (from the git tag at build time)
//   s             status: count, temperature, network, config state
//   c             encoder count
//   cr            reset the encoder count to zero
//   t             temperature
//   n             show network settings (staged and active)
//   nm 0|1        addressing mode: 0 = static, 1 = DHCP
//   ni <addr>     static IP address
//   ns <mask>     subnet mask
//   ng <addr>     gateway
//   nd <addr>     DNS server
//   nn <name>     system name
//   mac <addr>    MAC address
//   d             load the factory defaults into the staged config
//   w             write the staged config to EEPROM
//   e             erase the stored config (next boot uses defaults)
//   rb            reboot
//   p [ms]        poll status every ms (default 1000)
//   x             stop polling
//   q             disconnect
//
// The n* setters stage into AppState::pending; 'w' is what commits to flash.
// That mirrors the web popup, which likewise separates editing from saving.

class TelnetServer {
public:
    TelnetServer();

    // Call from setup() once Ethernet is up.
    void begin(AppState* state);

    // Call every loop() iteration.
    void update();

    // ── Action callbacks, supplied by main.cpp ─────────────────────────────
    void (*onResetCount)()            = nullptr;
    bool (*onSaveConfig)(String& err) = nullptr;
    bool (*onEraseConfig)()           = nullptr;
    void (*onReboot)()                = nullptr;

private:
    EthernetServer _server;
    TelnetClient   _clients[TELNET_MAX_CLIENTS];
    AppState*      _state;

    void acceptClients();
    void processClient(TelnetClient& tc);
    void processLine(TelnetClient& tc, const char* line);
    void handlePoll(TelnetClient& tc);

    // Applies one "n<x> <value>" setter to the staged config.
    void applySetter(TelnetClient& tc, const String& cmd, const char* arg);

    void sendPrompt(TelnetClient& tc);
    void sendHelp(TelnetClient& tc);
    void sendStatus(TelnetClient& tc);
    void sendNetwork(TelnetClient& tc);
    void println(TelnetClient& tc, const String& s);
    void print(TelnetClient& tc, const String& s);
};
