#include "version.h"

#include "openhanko_version.h"
#include "pico/binary_info.h"

// What `picotool info` prints, for a board in BOOTSEL or for a .uf2 on disk.
bi_decl(bi_program_version_string(OPENHANKO_VERSION_FULL))

// The version, behind a prefix nothing else in the image contains.
//
// The macOS app reads the version of the firmware it bundles by scanning the
// .uf2 for this prefix, which is far simpler than parsing binary_info and needs
// no sidecar file that could drift from the image. STATUS reports the tail of
// this same array, so the version a running device states and the version the
// app finds in a file are the same bytes, not two copies kept in step.
const char firmware_version_marker[] = FIRMWARE_VERSION_PREFIX OPENHANKO_VERSION_FULL;
