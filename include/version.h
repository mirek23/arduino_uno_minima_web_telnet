#pragma once

// ─── Firmware revision ───────────────────────────────────────────────────────
//
// FIRMWARE_VERSION is injected at build time by tools/gen_version.py from
//
//     git describe --tags --dirty --always
//
// so it always reflects what is actually in the repository:
//
//   "R01.00"                   built from the exact tag
//   "R01.00-3-gaba424f"        three commits past R01.00
//   "R01.00-3-gaba424f-dirty"  ...with uncommitted changes in the tree
//   "aba424f"                  no tags reachable
//   "unknown"                  no git metadata, e.g. a source archive
//
// Shown at boot on the serial console, in the web IP-configuration popup, and
// by the telnet 'v' command.

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif
