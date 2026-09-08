"""Inject the firmware revision, derived from the git tag, as a build flag.

PlatformIO runs this as a pre-build script (see extra_scripts in
platformio.ini). It defines FIRMWARE_VERSION from

    git describe --tags --dirty --always

so the revision always matches what is actually in the repository:

    R01.00                     built from the exact tag
    R01.00-3-gaba424f          three commits past R01.00
    R01.00-3-gaba424f-dirty    ...with uncommitted changes in the tree
    aba424f                    no tags reachable at all
    unknown                    no git metadata (e.g. a source archive)

Tag the release first, then build, and the number shown on the web page and
by the telnet 'v' command is the tag itself.

Run standalone to see what would be injected:

    python3 tools/gen_version.py
"""

import os
import re
import subprocess
import sys

FALLBACK = "unknown"

# Tag names cannot contain whitespace or quotes, but the string ends up inside
# a -D macro, so refuse anything that is not obviously safe there.
SAFE = re.compile(r"^[A-Za-z0-9._+-]+$")


def git_describe(project_dir):
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--dirty", "--always"],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        # No git, not a repository, or no commits yet.
        return None

    version = out.decode("utf-8", "replace").strip()
    if not version or not SAFE.match(version):
        return None
    return version


def resolve(project_dir):
    version = git_describe(project_dir)
    if version is None:
        sys.stderr.write(
            "gen_version: could not read the git revision, using '%s'\n" % FALLBACK
        )
        return FALLBACK
    return version


# ── Entry point: PlatformIO pre-build hook, or a plain command line run ──────
try:
    Import("env")                                    # noqa: F821  (SCons)
    _project_dir = env["PROJECT_DIR"]                # noqa: F821
    _version = resolve(_project_dir)
    print("gen_version: firmware revision %s" % _version)
    # The inner escaped quotes are what make this a string literal in C.
    env.Append(                                      # noqa: F821
        CPPDEFINES=[("FIRMWARE_VERSION", '\\"%s\\"' % _version)]
    )
except NameError:
    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    print(resolve(_project_dir))
