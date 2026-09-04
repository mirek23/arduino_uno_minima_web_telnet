#include "web_server.h"
#include "web_assets.h"

// ─── Small helpers ───────────────────────────────────────────────────────────

static String ipToString(const IPAddress& ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return String(buf);
}

// Percent-decode a query-string value, turning '+' back into a space.
static String urlDecode(const String& in) {
    String out;
    out.reserve(in.length());
    for (unsigned int i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.length()) {
            char hex[3] = { in[i+1], in[i+2], '\0' };
            char* end   = nullptr;
            long  v     = strtol(hex, &end, 16);
            if (end && *end == '\0') { out += (char)v; i += 2; }
            else                     { out += c; }
        } else {
            out += c;
        }
    }
    return out;
}

// Pull one key's value out of "a=1&b=2". Returns false if the key is absent.
static bool queryParam(const String& query, const char* key, String& out) {
    String needle = String(key) + "=";
    int    pos    = 0;
    while (pos <= (int)query.length()) {
        int amp   = query.indexOf('&', pos);
        int end   = (amp < 0) ? query.length() : amp;
        String kv = query.substring(pos, end);
        if (kv.startsWith(needle)) {
            out = urlDecode(kv.substring(needle.length()));
            return true;
        }
        if (amp < 0) break;
        pos = amp + 1;
    }
    return false;
}

// JSON string escaping — the system name is user supplied.
static String jsonEscape(const char* s) {
    String out;
    for (const char* p = s; *p; p++) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if (*p < 0x20)          { /* drop control characters */ }
        else                         { out += *p; }
    }
    return out;
}

// ─── Construction ────────────────────────────────────────────────────────────

WebServer::WebServer() : _server(WEB_SERVER_PORT), _state(nullptr),
                         _lastHeartbeat(0) {
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        _clients[i].active          = false;
        _clients[i].sseMode         = false;
        _clients[i].reqLen          = 0;
        _clients[i].haveReqLine     = false;
        _clients[i].overflow        = false;
        _clients[i].crlfState       = 0;
        _clients[i].headersComplete = false;
        _clients[i].lastActivity    = 0;
        _clients[i].sseSince        = 0;
    }
}

void WebServer::begin(AppState* state) {
    _state = state;
    _server.begin();
    Serial.print("[Web] HTTP server listening on port ");
    Serial.println(WEB_SERVER_PORT);
}

// ─── Main update ─────────────────────────────────────────────────────────────

void WebServer::update(bool stateChanged) {
    acceptClients();

    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (_clients[i].active) processClient(_clients[i]);
    }

    // Push on change, and otherwise on a timer. The periodic push is what
    // lets the browser notice that the board went away: after a reboot the
    // W5500 is reset without closing its TCP connections, so the browser's
    // socket stays half-open, onerror never fires, and only a missing
    // heartbeat reveals that the stream is dead.
    if (stateChanged) {
        pushSSE();
    } else if (millis() - _lastHeartbeat >= SSE_HEARTBEAT_MS) {
        pushSSE();
    }
}

// ─── Connection lifecycle ────────────────────────────────────────────────────

void WebServer::acceptClients() {
    EthernetClient newClient = _server.accept();
    if (!newClient) return;

    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        HttpClient& hc = _clients[i];
        if (hc.active) continue;

        hc.client          = newClient;
        hc.active          = true;
        hc.sseMode         = false;
        hc.reqLen          = 0;
        hc.reqLine[0]      = '\0';
        hc.haveReqLine     = false;
        hc.overflow        = false;
        hc.crlfState       = 0;
        hc.headersComplete = false;
        hc.lastActivity    = millis();
        hc.sseSince        = 0;

        // EthernetClient::stop() polls for a graceful close and gives up only
        // after its Stream timeout, 1000 ms by default. That is a full second
        // of stalled loop every time a peer disappears mid-response, which
        // freezes the LCD and the telnet console too. Bound it.
        hc.client.setTimeout(HTTP_CLOSE_TIMEOUT_MS);
        return;
    }

    // Every slot is busy — refuse rather than queue.
    newClient.stop();
}

void WebServer::closeClient(HttpClient& hc) {
    hc.client.stop();
    hc.active          = false;
    hc.sseMode         = false;
    hc.sseSince        = 0;
    hc.reqLen          = 0;
    hc.haveReqLine     = false;
    hc.overflow        = false;
    hc.crlfState       = 0;
    hc.headersComplete = false;
}

int WebServer::sseClientCount() const {
    int n = 0;
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (_clients[i].active && _clients[i].sseMode) n++;
    }
    return n;
}

// ─── Request reading ─────────────────────────────────────────────────────────

void WebServer::processClient(HttpClient& hc) {
    if (!hc.client.connected()) { closeClient(hc); return; }

    // An SSE session has no more requests to read; pushSSE() drives it.
    if (hc.sseMode) return;

    while (hc.client.available()) {
        char c = (char)hc.client.read();
        hc.lastActivity = millis();

        // Keep only the request line; later header bytes are just scanned.
        if (!hc.haveReqLine) {
            if (c == '\n') {
                // Strip the trailing CR that precedes this LF.
                while (hc.reqLen > 0 && (hc.reqLine[hc.reqLen-1] == '\r' ||
                                         hc.reqLine[hc.reqLen-1] == ' ')) {
                    hc.reqLen--;
                }
                hc.reqLine[hc.reqLen] = '\0';
                hc.haveReqLine        = true;
            } else if (hc.reqLen < HTTP_REQLINE_SIZE - 1) {
                hc.reqLine[hc.reqLen++] = c;
            } else {
                hc.overflow = true;             // URI longer than we accept
            }
        }

        // Rolling match for the CRLF CRLF that ends the header block.
        switch (hc.crlfState) {
            case 0: hc.crlfState = (c == '\r') ? 1 : 0; break;
            case 1: hc.crlfState = (c == '\n') ? 2 : ((c == '\r') ? 1 : 0); break;
            case 2: hc.crlfState = (c == '\r') ? 3 : 0; break;
            case 3: hc.crlfState = (c == '\n') ? 4 : 0; break;
            default: break;
        }
        if (hc.crlfState == 4) { hc.headersComplete = true; break; }
    }

    if (hc.headersComplete) {
        routeRequest(hc);
        return;
    }

    // Drop a peer that opened a socket and then went quiet.
    if (millis() - hc.lastActivity >= HTTP_CLIENT_TIMEOUT_MS) closeClient(hc);
}

// ─── Routing ─────────────────────────────────────────────────────────────────

void WebServer::routeRequest(HttpClient& hc) {
    if (hc.overflow) {
        sendSimple(hc, "414 URI Too Long", "414 URI Too Long");
        closeClient(hc);
        return;
    }

    // Request line: METHOD SP request-target SP HTTP/1.x
    char method[8] = {0};
    char uri[HTTP_REQLINE_SIZE] = {0};
    if (sscanf(hc.reqLine, "%7s %319s", method, uri) != 2) {
        sendSimple(hc, "400 Bad Request", "400 Bad Request");
        closeClient(hc);
        return;
    }

    String fullURI(uri);
    int    qmark = fullURI.indexOf('?');
    String path  = (qmark >= 0) ? fullURI.substring(0, qmark) : fullURI;
    String query = (qmark >= 0) ? fullURI.substring(qmark + 1) : String("");

    bool isGet  = (strcmp(method, "GET")  == 0);
    bool isPost = (strcmp(method, "POST") == 0);

    if (path == "/" || path.length() == 0) path = "/index.html";

    // ── Event stream: the only route that keeps the socket open ───────────
    if (path == "/events" && isGet) {
        // A reload must always get a stream. Refusing with 503 left the
        // dashboard disconnected whenever the previous socket had not yet
        // finished closing — which is exactly when someone is reloading.
        if (sseClientCount() >= SSE_MAX_CLIENTS) evictOldestSSE();

        sendSSEHeaders(hc);
        hc.sseMode  = true;
        hc.sseSince = millis();
        hc.client.print("data: " + stateJSON() + "\r\n\r\n");
        _lastHeartbeat = millis();
        return;                             // deliberately not closed
    }

    // ── The dashboard ─────────────────────────────────────────────────────
    // The stylesheet and script are inlined into this document, so the whole
    // dashboard is one request.
    if (isGet && path == "/index.html") {
        sendAsset(hc, "text/html; charset=utf-8", WEB_INDEX_HTML, WEB_INDEX_HTML_LEN);

    // ── API ───────────────────────────────────────────────────────────────
    } else if (isGet && path == "/api/status") {
        handleStatus(hc);
    } else if (isGet && path == "/api/netcfg") {
        handleNetcfgGet(hc);
    } else if (isPost && path == "/api/netcfg") {
        handleNetcfgPost(hc, query);
    } else if (isPost && path == "/api/reset") {
        handleReset(hc);
    } else if (isPost && path == "/api/save") {
        handleSave(hc);
    } else if (isPost && path == "/api/reboot") {
        handleReboot(hc);
    } else {
        sendSimple(hc, "404 Not Found", "404 Not Found");
    }

    closeClient(hc);
}

// ─── API handlers ────────────────────────────────────────────────────────────

void WebServer::handleStatus(HttpClient& hc) {
    sendJSON(hc, stateJSON());
}

void WebServer::handleNetcfgGet(HttpClient& hc) {
    sendJSON(hc, netcfgJSON());
}

void WebServer::handleReset(HttpClient& hc) {
    if (onResetCount) onResetCount();
    sendResult(hc, true, "Encoder count reset");
}

// Stage configuration changes. Every field is optional, so the same endpoint
// serves the full popup form and a single-field update. Nothing is written to
// flash here: that is what /api/save is for.
void WebServer::handleNetcfgPost(HttpClient& hc, const String& query) {
    if (!_state || !_state->pending) {
        sendResult(hc, false, "No configuration available");
        return;
    }

    // Validate into a scratch copy so a bad field cannot half-apply.
    NetworkConfig cfg = *_state->pending;
    String value;

    if (queryParam(query, "dhcp", value)) {
        cfg.useDhcp = (value == "1" || value == "true" || value == "on");
    }
    if (queryParam(query, "ip", value) && !EepromConfig::parseIP(value.c_str(), cfg.ip)) {
        sendResult(hc, false, "Invalid IP address"); return;
    }
    if (queryParam(query, "sn", value) && !EepromConfig::parseIP(value.c_str(), cfg.subnet)) {
        sendResult(hc, false, "Invalid subnet mask"); return;
    }
    if (queryParam(query, "gw", value) && !EepromConfig::parseIP(value.c_str(), cfg.gateway)) {
        sendResult(hc, false, "Invalid gateway"); return;
    }
    if (queryParam(query, "dns", value) && !EepromConfig::parseIP(value.c_str(), cfg.dns)) {
        sendResult(hc, false, "Invalid DNS address"); return;
    }
    if (queryParam(query, "name", value) &&
        !EepromConfig::setSysName(cfg, value.c_str())) {
        sendResult(hc, false, "Invalid system name (1-" + String(SYS_NAME_SIZE - 1) +
                              " chars: letters, digits, '-', '_')");
        return;
    }
    if (queryParam(query, "mac", value) && !EepromConfig::parseMAC(value.c_str(), cfg.mac)) {
        sendResult(hc, false, "Invalid MAC address"); return;
    }

    String why;
    if (!EepromConfig::validate(cfg, why)) {
        sendResult(hc, false, "Rejected: " + why);
        return;
    }

    *_state->pending = cfg;
    _state->cfgDirty = true;
    sendResult(hc, true, "Settings staged - save to EEPROM to keep them");
}

void WebServer::handleSave(HttpClient& hc) {
    if (!onSaveConfig) { sendResult(hc, false, "Saving not available"); return; }
    String err;
    if (onSaveConfig(err)) sendResult(hc, true,  "Saved to EEPROM");
    else                   sendResult(hc, false, err.length() ? err : "EEPROM write failed");
}

void WebServer::handleReboot(HttpClient& hc) {
    sendResult(hc, true, "Rebooting");
    // Let the response drain before main.cpp performs the reset.
    hc.client.flush();
    if (onReboot) onReboot();
}

// ─── Response helpers ────────────────────────────────────────────────────────

void WebServer::sendAsset(HttpClient& hc, const char* contentType,
                          const char* body, size_t len) {
    char hdr[192];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        contentType, (unsigned)len);
    hc.client.print(hdr);

    // Chunk the body: the W5500 socket buffer is 2 KB and write() blocks
    // until the whole span is queued.
    const size_t CHUNK = 512;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
        hc.client.write((const uint8_t*)(body + off), n);
    }
}

void WebServer::sendSSEHeaders(HttpClient& hc) {
    hc.client.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
    );
}

void WebServer::sendJSON(HttpClient& hc, const String& json) {
    char hdr[160];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        (unsigned)json.length());
    hc.client.print(hdr);
    hc.client.print(json);
}

void WebServer::sendResult(HttpClient& hc, bool ok, const String& message) {
    String json = String("{\"ok\":") + (ok ? "true" : "false") +
                  ",\"msg\":\"" + jsonEscape(message.c_str()) + "\"" +
                  ",\"dirty\":" + ((_state && _state->cfgDirty) ? "true" : "false") + "}";
    sendJSON(hc, json);
}

void WebServer::sendSimple(HttpClient& hc, const char* status, const char* body) {
    char hdr[192];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, (unsigned)strlen(body));
    hc.client.print(hdr);
    hc.client.print(body);
}

// ─── SSE push ────────────────────────────────────────────────────────────────

void WebServer::pushSSE() {
    _lastHeartbeat = millis();
    if (!_state) return;

    String msg = "data: " + stateJSON() + "\r\n\r\n";
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        HttpClient& hc = _clients[i];
        if (!hc.active || !hc.sseMode) continue;
        if (hc.client.connected()) {
            hc.client.print(msg);
        } else {
            closeClient(hc);
        }
    }
}

void WebServer::evictOldestSSE() {
    int      victim = -1;
    uint32_t oldest = 0;
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        HttpClient& hc = _clients[i];
        if (!hc.active || !hc.sseMode) continue;
        uint32_t age = millis() - hc.sseSince;
        if (victim < 0 || age > oldest) { victim = i; oldest = age; }
    }
    if (victim >= 0) {
        Serial.println("[Web] evicting the oldest event stream to free a slot");
        closeClient(_clients[victim]);
    }
}

// ─── JSON builders ───────────────────────────────────────────────────────────

String WebServer::stateJSON() {
    if (!_state) return "{}";

    char temp[16];
    if (_state->tempValid) snprintf(temp, sizeof(temp), "%.2f", (double)_state->temperature);
    else                   snprintf(temp, sizeof(temp), "null");

    String json = "{";
    json += "\"temp\":";     json += temp;
    json += ",\"tempOk\":";  json += _state->tempValid ? "true" : "false";
    json += ",\"count\":";   json += String((long)_state->encoderCount);
    json += ",\"name\":\"";  json += jsonEscape(_state->pending ? _state->pending->sysName : "");
    json += "\",\"ip\":\"";  json += ipToString(_state->ip);
    json += "\",\"dhcp\":";  json += (_state->pending && _state->pending->useDhcp) ? "true" : "false";
    json += ",\"bound\":";   json += _state->dhcpBound ? "true" : "false";
    json += ",\"link\":";    json += _state->linkUp ? "true" : "false";
    json += ",\"dirty\":";   json += _state->cfgDirty ? "true" : "false";
    json += ",\"up\":";      json += String((unsigned long)(millis() / 1000UL));
    json += "}";
    return json;
}

String WebServer::netcfgJSON() {
    if (!_state || !_state->pending) return "{}";
    const NetworkConfig& c = *_state->pending;

    char mac[18];
    EepromConfig::formatMAC(c.mac, mac, sizeof(mac));

    String json = "{";
    json += "\"dhcp\":";       json += c.useDhcp ? "true" : "false";
    json += ",\"ip\":\"";      json += ipToString(c.ip);
    json += "\",\"sn\":\"";    json += ipToString(c.subnet);
    json += "\",\"gw\":\"";    json += ipToString(c.gateway);
    json += "\",\"dns\":\"";   json += ipToString(c.dns);
    json += "\",\"name\":\"";  json += jsonEscape(c.sysName);
    json += "\",\"mac\":\"";   json += mac;
    json += "\",\"activeIp\":\"";  json += ipToString(_state->ip);
    json += "\",\"activeSn\":\""; json += ipToString(_state->subnet);
    json += "\",\"activeGw\":\""; json += ipToString(_state->gateway);
    json += "\",\"activeDns\":\"";json += ipToString(_state->dns);
    json += "\",\"stored\":";  json += _state->cfgStored ? "true" : "false";
    json += ",\"dirty\":";     json += _state->cfgDirty ? "true" : "false";
    json += ",\"jumper\":";    json += _state->defaultsJumper ? "true" : "false";
    json += ",\"eeBytes\":";   json += String((unsigned long)EepromConfig::capacity());
    json += "}";
    return json;
}
