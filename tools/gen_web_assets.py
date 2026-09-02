"""Compile the dashboard in data/ into a C++ source file.

The UNO R4 Minima has no filesystem partition, so the web assets have to live
in program flash. PlatformIO runs this as a pre-build script (see
extra_scripts in platformio.ini); it can also be run by hand:

    python3 tools/gen_web_assets.py

Edit the files in data/. src/web_assets.cpp is generated and any manual edit
to it is overwritten on the next build.
"""

import os
import sys

# ── Asset table: (source file in data/, C identifier) ────────────────────────
ASSETS = [
    ("index.html", "WEB_INDEX_HTML"),
    ("style.css",  "WEB_STYLE_CSS"),
    ("app.js",     "WEB_APP_JS"),
]

OUTPUT = os.path.join("src", "web_assets.cpp")

HEADER = """// ─────────────────────────────────────────────────────────────────────────────
// AUTO-GENERATED FILE - DO NOT EDIT
//
// Produced by tools/gen_web_assets.py from the files in data/.
// Edit data/index.html, data/style.css or data/app.js and rebuild.
// ─────────────────────────────────────────────────────────────────────────────

#include "web_assets.h"

"""


def escape_line(line):
    """Escape one source line for inclusion in a C string literal."""
    out = []
    for ch in line:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            pass                      # normalise CRLF to LF
        elif ch == "\n":
            out.append("\\n")
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            # Anything outside printable ASCII goes out as UTF-8 hex escapes.
            # "\xNN" is closed and reopened so a following hex digit in the
            # text cannot be swallowed into the escape sequence.
            for byte in ch.encode("utf-8"):
                out.append('\\x%02x""' % byte)
    return "".join(out)


def emit(name, text):
    """Render one asset as a C++ array definition plus its length."""
    lines = text.splitlines(keepends=True)
    body = ["const char %s[] =\n" % name]
    if not lines:
        body.append('    ""\n')
    for line in lines:
        body.append('    "%s"\n' % escape_line(line))
    body.append(";\n")
    body.append("const size_t %s_LEN = sizeof(%s) - 1;\n\n" % (name, name))
    return "".join(body)


def generate(project_dir):
    data_dir = os.path.join(project_dir, "data")
    chunks = [HEADER]

    for filename, name in ASSETS:
        path = os.path.join(data_dir, filename)
        if not os.path.isfile(path):
            sys.stderr.write("gen_web_assets: missing %s\n" % path)
            return 1
        with open(path, "r", encoding="utf-8") as handle:
            chunks.append(emit(name, handle.read()))

    generated = "".join(chunks)
    out_path = os.path.join(project_dir, OUTPUT)

    # Only rewrite when the content actually changed, so an unchanged asset
    # does not force a recompile on every build.
    if os.path.isfile(out_path):
        with open(out_path, "r", encoding="utf-8") as handle:
            if handle.read() == generated:
                return 0

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(generated)
    print("gen_web_assets: wrote %s (%d bytes)" % (OUTPUT, len(generated)))
    return 0


# ── Entry point: PlatformIO pre-build hook, or a plain command line run ──────
try:
    Import("env")                                    # noqa: F821  (SCons)
    _project_dir = env["PROJECT_DIR"]                # noqa: F821
except NameError:
    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_status = generate(_project_dir)
if _status:
    sys.exit(_status)
