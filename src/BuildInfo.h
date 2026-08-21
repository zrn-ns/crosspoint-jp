#pragma once

#include <cstddef>

// CROSSPOINT_BUILD_TIME is the build's position in time as "YYYYMMDD-HHMMSS"
// (UTC), injected by scripts/git_branch.py. It is the OTA comparison key for
// Dev Build releases, whose tags (dev-YYYYMMDD-HHMMSS) carry the same stamp:
// dev-build.yml derives the tag name and this define from one shell variable,
// so a device never sees its own Dev Build as an update.
//
// The format is fixed-width, zero-padded and UTC, which makes strcmp() ordering
// equal to chronological ordering — no 64-bit arithmetic on a 32-bit target.
//
// The fallback matters for builds that bypass the injection: platformio.local.ini
// can pin CROSSPOINT_VERSION and CROSSPOINT_BUILD_TIME to fake a device state for
// OTA testing. "00000000-000000" sorts below every real stamp, so such a build
// treats any Dev Build as newer.
#ifndef CROSSPOINT_BUILD_TIME
#define CROSSPOINT_BUILD_TIME "00000000-000000"
#endif

// Length of a "YYYYMMDD-HHMMSS" stamp, excluding the terminator.
static constexpr size_t CROSSPOINT_BUILD_TIME_LEN = 15;
