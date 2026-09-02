#pragma once
#include <Arduino.h>
#include <Ethernet.h>
#include "config.h"
#include "eeprom_config.h"

// ─── Shared application state ────────────────────────────────────────────────
// Owned by main.cpp, read by both servers. 'pending' points at the editable
// configuration: the web popup and the telnet setters mutate it, and it is
// only copied to the EEPROM when the user asks for a save.

struct AppState {
    // Live measurements
    volatile float   temperature;
    volatile bool    tempValid;
    volatile int32_t encoderCount;

    // Network status as actually applied at boot
    bool      linkUp;
    bool      dhcpBound;        // DHCP mode and a lease was obtained
    IPAddress ip;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns;

    // Configuration
    NetworkConfig* pending;     // editable working copy
    bool      cfgDirty;         // pending differs from what is in EEPROM
    bool      cfgStored;        // a valid record exists in EEPROM
    bool      defaultsJumper;   // DEFAULTS_JUMPER_PIN was grounded at boot
};

// ─── HTTP client session ─────────────────────────────────────────────────────
struct HttpClient {
    EthernetClient client;
    bool           active;
    bool           sseMode;                     // persistent SSE connection
    char           reqLine[HTTP_REQLINE_SIZE];  // only the request line is kept
    uint16_t       reqLen;
    bool           haveReqLine;
    bool           overflow;                    // request line too long
    uint8_t        crlfState;                   // rolling match for CRLF CRLF
    bool           headersComplete;
    uint32_t       lastActivity;
    uint32_t       lastKeepalive;               // SSE keep-alive timer
};

// ─── Web Server ──────────────────────────────────────────────────────────────
//
// Routes:
//   GET  /                 dashboard
//   GET  /style.css        stylesheet
//   GET  /app.js           client script
//   GET  /events           Server-Sent Events stream of live state
//   GET  /api/status       JSON snapshot of live state
//   GET  /api/netcfg       JSON of the pending config plus what is active
//   POST /api/reset        zero the encoder count
//   POST /api/netcfg?...   validate and stage configuration changes
//   POST /api/save         write the staged configuration to EEPROM
//   POST /api/reboot       restart the board
//
// Assets are compiled into flash (see tools/gen_web_assets.py) because the
// UNO R4 Minima has no filesystem partition.
//
// Only the request line is buffered; remaining headers are drained without
// being stored, so a browser sending a large header block cannot wedge a
// session, and a stalled connection is dropped after HTTP_CLIENT_TIMEOUT_MS.

class WebServer {
public:
    WebServer();

    // Call from setup() once Ethernet is up.
    void begin(AppState* state);

    // Call every loop() iteration. stateChanged triggers an SSE push.
    void update(bool stateChanged);

    // Push the current state to every connected SSE client.
    void pushSSE();

    // ── Action callbacks, supplied by main.cpp ─────────────────────────────
    void (*onResetCount)()                 = nullptr;
    // Writes the pending config to EEPROM. Returns false and fills err on
    // failure.
    bool (*onSaveConfig)(String& err)      = nullptr;
    // Requests a deferred reboot so the HTTP response can be flushed first.
    void (*onReboot)()                     = nullptr;

private:
    EthernetServer _server;
    HttpClient     _clients[HTTP_MAX_CLIENTS];
    AppState*      _state;

    void acceptClients();
    void processClient(HttpClient& hc);
    void routeRequest(HttpClient& hc);
    void closeClient(HttpClient& hc);
    int  sseClientCount() const;

    // ── Handlers ───────────────────────────────────────────────────────────
    void handleStatus(HttpClient& hc);
    void handleNetcfgGet(HttpClient& hc);
    void handleNetcfgPost(HttpClient& hc, const String& query);
    void handleSave(HttpClient& hc);
    void handleReboot(HttpClient& hc);
    void handleReset(HttpClient& hc);

    // ── Response helpers ───────────────────────────────────────────────────
    void sendAsset(HttpClient& hc, const char* contentType,
                   const char* body, size_t len);
    void sendSSEHeaders(HttpClient& hc);
    void sendJSON(HttpClient& hc, const String& json);
    void sendResult(HttpClient& hc, bool ok, const String& message);
    void sendSimple(HttpClient& hc, const char* status, const char* body);

    // ── JSON builders ──────────────────────────────────────────────────────
    String stateJSON();
    String netcfgJSON();
};
