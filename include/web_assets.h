#pragma once
#include <stddef.h>

// ─── Web assets compiled into flash ─────────────────────────────────────────
//
// The UNO R4 Minima has no filesystem partition, so the dashboard cannot be
// uploaded separately the way it would be on a board with LittleFS. Instead
// tools/gen_web_assets.py folds data/index.html, data/style.css and
// data/app.js into ONE self-contained document in src/web_assets.cpp before
// every build (see extra_scripts in platformio.ini), and the web server
// streams it straight out of flash. Edit the files in data/ — never
// src/web_assets.cpp, which is regenerated and overwritten.
//
// The stylesheet and script are inlined rather than served as separate routes
// because the W5500 has only eight sockets: a three-file page makes the
// browser open several parallel connections on top of the /events stream,
// which exhausts HTTP_MAX_CLIENTS and gets a request refused — the page then
// needs reloading two or three times before it comes up.
//
// On this Cortex-M4 the array lives in .rodata and is addressed normally, so
// no PROGMEM accessors are needed.

extern const char   WEB_INDEX_HTML[];
extern const size_t WEB_INDEX_HTML_LEN;
