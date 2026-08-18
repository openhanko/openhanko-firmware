// Compile-time PIV identity.
//
// Copy this file to secrets.h and fill in real PEMs, or just run:
//
//     ./provision.py gen-secrets --output rp2040/secrets.h
//
// which generates the keys and writes a correctly formatted secrets.h for you.
//
// If secrets.h is absent the firmware builds fine and waits to be provisioned
// over the console instead. If it is present but still contains the
// REPLACE_WITH placeholders below, it is ignored with a warning.
//
// An identity in flash — generated on first boot, or pushed over the console —
// overrides whatever is compiled here. Run FACTORY_RESET to clear it.
//
// Note what this costs: the private keys end up in build/ and in the flashed
// .bin, so every device flashed from that image shares one identity, and
// anyone holding the image holds the keys. secrets.h is gitignored.

#pragma once

static const char PIV_CERT_9A_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"REPLACE_WITH_AUTHENTICATION_CERTIFICATE\n"
"-----END CERTIFICATE-----\n";

static const char PIV_PRIVATE_KEY_9A_PEM[] =
"-----BEGIN PRIVATE KEY-----\n"
"REPLACE_WITH_AUTHENTICATION_PRIVATE_KEY\n"
"-----END PRIVATE KEY-----\n";

static const char PIV_CERT_9D_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"REPLACE_WITH_KEY_MANAGEMENT_CERTIFICATE\n"
"-----END CERTIFICATE-----\n";

static const char PIV_PRIVATE_KEY_9D_PEM[] =
"-----BEGIN PRIVATE KEY-----\n"
"REPLACE_WITH_KEY_MANAGEMENT_PRIVATE_KEY\n"
"-----END PRIVATE KEY-----\n";
