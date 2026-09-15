#pragma once

// The firmware's version, as 0.2.0+09f45d2 — release number and commit, with
// ".dirty" when built from uncommitted source. See version.cmake.
#define FIRMWARE_VERSION_PREFIX "OPENHANKO_FW_VERSION="

extern const char firmware_version_marker[];

// The version without its prefix.
static inline const char *firmware_version(void) {
  return firmware_version_marker + sizeof(FIRMWARE_VERSION_PREFIX) - 1;
}
