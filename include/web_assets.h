#pragma once
#include <stddef.h>

// ─── Web assets compiled into flash ─────────────────────────────────────────
//
// The UNO R4 Minima has no filesystem partition, so the dashboard cannot be
// uploaded separately the way it would be on a board with LittleFS. Instead
// tools/gen_web_assets.py turns data/index.html, data/style.css and data/app.js
// into src/web_assets.cpp before every build (see extra_scripts in
// platformio.ini), and the web server streams these arrays straight out of
// flash. Edit the files in data/ — never src/web_assets.cpp, which is
// regenerated and overwritten.
//
// On this Cortex-M4 the arrays live in .rodata and are addressed normally, so
// no PROGMEM accessors are needed.

extern const char   WEB_INDEX_HTML[];
extern const size_t WEB_INDEX_HTML_LEN;

extern const char   WEB_STYLE_CSS[];
extern const size_t WEB_STYLE_CSS_LEN;

extern const char   WEB_APP_JS[];
extern const size_t WEB_APP_JS_LEN;
