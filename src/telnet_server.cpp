#include "telnet_server.h"
#include "eeprom_config.h"
#include "version.h"

static String ipToString(const IPAddress& ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return String(buf);
}

TelnetServer::TelnetServer() : _server(TELNET_PORT), _state(nullptr) {
    for (int i = 0; i < TELNET_MAX_CLIENTS; i++) {
        _clients[i].active       = false;
        _clients[i].lineLen      = 0;
        _clients[i].polling      = false;
        _clients[i].pollInterval = 1000;
        _clients[i].lastPollTime = 0;
    }
}

void TelnetServer::begin(AppState* state) {
    _state = state;
    _server.begin();
    Serial.print("[Telnet] server listening on port ");
    Serial.println(TELNET_PORT);
}

// ─── Connection lifecycle ────────────────────────────────────────────────────

void TelnetServer::acceptClients() {
    EthernetClient nc = _server.accept();
    if (!nc) return;

    for (int i = 0; i < TELNET_MAX_CLIENTS; i++) {
        TelnetClient& tc = _clients[i];
        if (tc.active) continue;

        tc.client       = nc;
        tc.active       = true;
        tc.lineLen      = 0;
        tc.polling      = false;
        tc.pollInterval = 1000;
        tc.lastPollTime = 0;

        // Minimal negotiation: we will suppress go-ahead and echo ourselves.
        const uint8_t willSGA[] = { 0xFF, 0xFB, 0x03 };
        const uint8_t willEcho[] = { 0xFF, 0xFB, 0x01 };
        nc.write(willSGA,  sizeof(willSGA));
        nc.write(willEcho, sizeof(willEcho));

        String name = (_state && _state->pending) ? String(_state->pending->sysName)
                                                  : String("UNO R4");
        println(tc, "\r\n=== " + name + " - UNO R4 Minima console ===");
        println(tc, "Firmware " + String(FIRMWARE_VERSION));
        println(tc, "Type '?' for the command list.");
        sendPrompt(tc);
        return;
    }

    nc.print("Sorry, all telnet sessions are in use.\r\n");
    nc.stop();
}

void TelnetServer::update() {
    acceptClients();
    for (int i = 0; i < TELNET_MAX_CLIENTS; i++) {
        if (_clients[i].active) processClient(_clients[i]);
    }
}

void TelnetServer::processClient(TelnetClient& tc) {
    if (!tc.client.connected()) {
        tc.client.stop();
        tc.active  = false;
        tc.polling = false;
        return;
    }

    if (tc.polling) handlePoll(tc);

    while (tc.client.available()) {
        uint8_t c = (uint8_t)tc.client.read();

        // Swallow telnet IAC option negotiation (IAC + verb + option).
        if (c == 0xFF) {
            if (tc.client.available()) {
                uint8_t verb = (uint8_t)tc.client.read();
                // WILL/WONT/DO/DONT carry one option byte; others do not.
                if (verb >= 0xFB && verb <= 0xFE && tc.client.available()) {
                    tc.client.read();
                }
            }
            continue;
        }

        if (c == 0x00) continue;                    // NUL padding after CR

        if (c == 0x7F || c == 0x08) {               // DEL / BS
            if (tc.lineLen > 0) {
                tc.lineLen--;
                tc.client.write((const uint8_t*)"\b \b", 3);
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            tc.client.write((const uint8_t*)"\r\n", 2);
            tc.lineBuf[tc.lineLen] = '\0';

            const char* line = tc.lineBuf;
            while (*line == ' ' || *line == '\t') line++;

            uint8_t consumed = tc.lineLen;
            tc.lineLen = 0;
            // An empty line just reprints the prompt (and CR LF sends two).
            if (consumed == 0) { sendPrompt(tc); continue; }

            processLine(tc, line);
            if (!tc.active) return;                 // 'q' closed the session
            continue;
        }

        if (c >= 0x20 && tc.lineLen < TELNET_BUF_SIZE - 1) {
            tc.client.write(c);                     // local echo
            tc.lineBuf[tc.lineLen++] = (char)c;
        }
    }
}

// ─── Auto-poll ───────────────────────────────────────────────────────────────

void TelnetServer::handlePoll(TelnetClient& tc) {
    uint32_t now = millis();
    if (now - tc.lastPollTime < tc.pollInterval) return;
    tc.lastPollTime = now;
    sendStatus(tc);
}

// ─── Command dispatch ────────────────────────────────────────────────────────

void TelnetServer::processLine(TelnetClient& tc, const char* line) {
    char cmd[16] = {0};
    char arg[TELNET_BUF_SIZE] = {0};
    sscanf(line, "%15s %100[^\n]", cmd, arg);

    // Trim trailing whitespace from the argument.
    for (int i = (int)strlen(arg) - 1; i >= 0 && (arg[i] == ' ' || arg[i] == '\t'); i--) {
        arg[i] = '\0';
    }

    String c(cmd);
    c.toLowerCase();

    if (c == "?" || c == "h" || c == "help") {
        sendHelp(tc);

    } else if (c == "v") {
        println(tc, "Firmware " + String(FIRMWARE_VERSION));

    } else if (c == "s") {
        sendStatus(tc);
        sendNetwork(tc);

    } else if (c == "c") {
        println(tc, "Count: " + String(_state ? (long)_state->encoderCount : 0L));

    } else if (c == "cr") {
        if (onResetCount) { onResetCount(); println(tc, "Count reset to 0"); }
        else              println(tc, "ERROR: reset not available");

    } else if (c == "t") {
        if (_state && _state->tempValid) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Temperature: %.2f C", (double)_state->temperature);
            println(tc, buf);
        } else {
            println(tc, "Temperature: no sensor");
        }

    } else if (c == "n") {
        sendNetwork(tc);

    } else if (c == "nm" || c == "ni" || c == "ns" || c == "ng" ||
               c == "nd" || c == "nn" || c == "mac") {
        applySetter(tc, c, arg);

    } else if (c == "d") {
        if (_state && _state->pending) {
            EepromConfig::defaults(*_state->pending);
            _state->cfgDirty = true;
            println(tc, "Factory defaults staged (use 'w' to write EEPROM)");
        } else {
            println(tc, "ERROR: no configuration available");
        }

    } else if (c == "w") {
        if (!onSaveConfig) { println(tc, "ERROR: saving not available"); }
        else {
            String err;
            if (onSaveConfig(err)) println(tc, "Configuration written to EEPROM");
            else println(tc, "ERROR: " + (err.length() ? err : String("EEPROM write failed")));
        }

    } else if (c == "e") {
        if (!onEraseConfig) { println(tc, "ERROR: erase not available"); }
        else if (onEraseConfig()) println(tc, "Stored configuration erased - defaults apply after reboot");
        else println(tc, "ERROR: EEPROM erase failed");

    } else if (c == "rb") {
        println(tc, "Rebooting...");
        tc.client.flush();
        if (onReboot) onReboot();

    } else if (c == "p") {
        uint32_t interval = (strlen(arg) > 0) ? (uint32_t)atol(arg) : 1000UL;
        if (interval < 200) interval = 200;
        tc.pollInterval = interval;
        tc.polling      = true;
        tc.lastPollTime = 0;
        println(tc, "Polling every " + String(interval) + " ms - 'x' stops it");
        return;                                     // no prompt while polling

    } else if (c == "x") {
        tc.polling = false;
        println(tc, "Polling stopped");

    } else if (c == "q") {
        println(tc, "Bye.");
        tc.client.flush();
        tc.client.stop();
        tc.active  = false;
        tc.polling = false;
        return;

    } else {
        println(tc, "Unknown command '" + String(cmd) + "' - type '?'");
    }

    sendPrompt(tc);
}

// ─── Configuration setters ───────────────────────────────────────────────────

void TelnetServer::applySetter(TelnetClient& tc, const String& cmd, const char* arg) {
    if (!_state || !_state->pending) {
        println(tc, "ERROR: no configuration available");
        return;
    }
    if (strlen(arg) == 0) {
        println(tc, "Usage: " + cmd + " <value>   (type '?' for the list)");
        return;
    }

    // Validate against a scratch copy so a rejected value changes nothing.
    NetworkConfig cfg = *_state->pending;
    bool parsed = true;

    if      (cmd == "nm")  cfg.useDhcp = (arg[0] == '1');
    else if (cmd == "ni")  parsed = EepromConfig::parseIP(arg, cfg.ip);
    else if (cmd == "ns")  parsed = EepromConfig::parseIP(arg, cfg.subnet);
    else if (cmd == "ng")  parsed = EepromConfig::parseIP(arg, cfg.gateway);
    else if (cmd == "nd")  parsed = EepromConfig::parseIP(arg, cfg.dns);
    else if (cmd == "nn")  parsed = EepromConfig::setSysName(cfg, arg);
    else if (cmd == "mac") parsed = EepromConfig::parseMAC(arg, cfg.mac);

    if (cmd == "nm" && arg[0] != '0' && arg[0] != '1') {
        println(tc, "Usage: nm 0|1   (0 = static, 1 = DHCP)");
        return;
    }
    if (!parsed) {
        if (cmd == "nn") {
            println(tc, "ERROR: name must be 1-" + String(SYS_NAME_SIZE - 1) +
                        " chars of letters, digits, '-' or '_'");
        } else {
            println(tc, "ERROR: could not parse '" + String(arg) + "'");
        }
        return;
    }

    String why;
    if (!EepromConfig::validate(cfg, why)) {
        println(tc, "ERROR: " + why);
        return;
    }

    *_state->pending = cfg;
    _state->cfgDirty = true;
    println(tc, "Staged. Use 'w' to write EEPROM, then 'rb' to apply.");
}

// ─── Output helpers ──────────────────────────────────────────────────────────

void TelnetServer::sendPrompt(TelnetClient& tc) {
    print(tc, "> ");
}

void TelnetServer::sendHelp(TelnetClient& tc) {
    println(tc,
        "\r\nCommands:\r\n"
        "  ?  h        this help\r\n"
        "  v           firmware revision\r\n"
        "  s           full status\r\n"
        "  c           encoder count\r\n"
        "  cr          reset encoder count\r\n"
        "  t           temperature\r\n"
        "  n           show network settings\r\n"
        "  nm 0|1      mode: 0 = static, 1 = DHCP\r\n"
        "  ni <addr>   static IP address\r\n"
        "  ns <mask>   subnet mask\r\n"
        "  ng <addr>   gateway\r\n"
        "  nd <addr>   DNS server\r\n"
        "  nn <name>   system name\r\n"
        "  mac <addr>  MAC address (de:ad:be:ef:fe:10)\r\n"
        "  d           stage the factory defaults\r\n"
        "  w           write staged settings to EEPROM\r\n"
        "  e           erase stored settings\r\n"
        "  rb          reboot\r\n"
        "  p [ms]      poll status (default 1000 ms)\r\n"
        "  x           stop polling\r\n"
        "  q           disconnect\r\n"
        "\r\n"
        "The n* setters only stage changes; 'w' writes them to EEPROM and\r\n"
        "network changes take effect after 'rb'."
    );
}

void TelnetServer::sendStatus(TelnetClient& tc) {
    if (!_state) { println(tc, "No state available"); return; }

    char buf[128];
    if (_state->tempValid) {
        snprintf(buf, sizeof(buf), "Count: %ld | Temp: %.2f C | Up: %lu s",
                 (long)_state->encoderCount, (double)_state->temperature,
                 (unsigned long)(millis() / 1000UL));
    } else {
        snprintf(buf, sizeof(buf), "Count: %ld | Temp: -- (no sensor) | Up: %lu s",
                 (long)_state->encoderCount, (unsigned long)(millis() / 1000UL));
    }
    println(tc, buf);
}

void TelnetServer::sendNetwork(TelnetClient& tc) {
    if (!_state || !_state->pending) { println(tc, "No configuration"); return; }
    const NetworkConfig& c = *_state->pending;

    char mac[18];
    EepromConfig::formatMAC(c.mac, mac, sizeof(mac));

    println(tc, "Name:   " + String(c.sysName));
    println(tc, "MAC:    " + String(mac));
    println(tc, "Mode:   " + String(c.useDhcp ? "DHCP" : "STATIC") +
                (_state->pending->useDhcp
                    ? (_state->dhcpBound ? " (lease held)" : " (no lease)")
                    : ""));
    println(tc, "Active: ip=" + ipToString(_state->ip) +
                " sn=" + ipToString(_state->subnet) +
                " gw=" + ipToString(_state->gateway) +
                " dns=" + ipToString(_state->dns));
    println(tc, "Staged: ip=" + ipToString(c.ip) +
                " sn=" + ipToString(c.subnet) +
                " gw=" + ipToString(c.gateway) +
                " dns=" + ipToString(c.dns));
    println(tc, "Link:   " + String(_state->linkUp ? "up" : "down") +
                " | EEPROM: " + String(_state->cfgStored ? "stored" : "empty") +
                (_state->cfgDirty ? " | UNSAVED CHANGES" : ""));
    if (_state->defaultsJumper) {
        println(tc, "NOTE:   defaults jumper was grounded at boot - EEPROM settings"
                    " were ignored for this session.");
    }
}

void TelnetServer::println(TelnetClient& tc, const String& s) {
    tc.client.print(s);
    tc.client.print("\r\n");
}

void TelnetServer::print(TelnetClient& tc, const String& s) {
    tc.client.print(s);
}
