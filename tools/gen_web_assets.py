"""Compile the dashboard in data/ into a C++ source file.

The UNO R4 Minima has no filesystem partition, so the web assets have to live
in program flash. PlatformIO runs this as a pre-build script (see
extra_scripts in platformio.ini); it can also be run by hand:

    python3 tools/gen_web_assets.py

data/index.html, data/style.css and data/app.js stay separately editable, but
they are combined into a single self-contained document here so that loading
the dashboard costs one TCP connection instead of three.

Edit the files in data/. src/web_assets.cpp is generated and any manual edit
to it is overwritten on the next build.
"""

import os
import sys

# The stylesheet and script are inlined into the HTML rather than served
# separately. The W5500 has only eight sockets, and a page that pulls three
# files makes the browser open three or four parallel connections on top of
# the /events stream — enough to exhaust HTTP_MAX_CLIENTS and have a request
# refused, which shows up as a page that needs reloading two or three times.
# One document means one connection.
SOURCE_HTML = "index.html"
INLINE = [
    # (tag to replace in index.html, file in data/, opening tag, closing tag)
    ('<link rel="stylesheet" href="/style.css"/>', "style.css", "<style>", "</style>"),
    ('<script src="/app.js"></script>',            "app.js",    "<script>", "</script>"),
]
ASSET_NAME = "WEB_INDEX_HTML"

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


def read(path):
    if not os.path.isfile(path):
        sys.stderr.write("gen_web_assets: missing %s\n" % path)
        return None
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def build_document(data_dir):
    """index.html with the stylesheet and script folded in."""
    html = read(os.path.join(data_dir, SOURCE_HTML))
    if html is None:
        return None

    for tag, filename, open_tag, close_tag in INLINE:
        body = read(os.path.join(data_dir, filename))
        if body is None:
            return None
        # A literal "</style>" or "</script>" in the payload would terminate
        # the block early and silently corrupt the page.
        if close_tag in body:
            sys.stderr.write("gen_web_assets: %s contains a literal %s\n"
                             % (filename, close_tag))
            return None
        if tag not in html:
            sys.stderr.write("gen_web_assets: %s has no %s to replace\n"
                             % (SOURCE_HTML, tag))
            return None
        html = html.replace(tag, "%s\n%s%s" % (open_tag, body, close_tag))

    return html


def generate(project_dir):
    data_dir = os.path.join(project_dir, "data")

    document = build_document(data_dir)
    if document is None:
        return 1

    generated = HEADER + emit(ASSET_NAME, document)
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
