#pragma once

// Trimmed mbedTLS build. This firmware only needs to parse a PKCS#8 RSA private
// key, run the private-key operation, hash with SHA-256, and base64-decode PEM.
// No TLS, no x509 verification, no ECC — every extra module is flash the
// RP2040 does not have to spare.

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_HAVE_ASM

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
// SHA-1 only to fingerprint public keys for the device label. macOS derives
// kSecAttrApplicationLabel — the hash sc_auth pairs against — the same way, so
// the name on the device matches the entry in `sc_auth list`. Nothing is signed
// with it.
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_BASE64_C

// Required by mbedtls_pk_parse_key() to accept "-----BEGIN PRIVATE KEY-----"
// text. Leaving it out is a quiet trap: certificates still work, because
// decode_pem_cert() in piv.c does its own base64, so the card enumerates and
// macOS reads the identity — and only signing fails, with 6f00.
#define MBEDTLS_PEM_PARSE_C

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

// ECDSA on P-256. Only this one curve: each enabled curve costs flash, and the
// PIV algorithm identifier 0x11 means P-256 specifically.
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
// Key agreement on slot 9D. macOS wraps the login keychain unlock key to a PIV
// key-management key, so without this a smart-card login still demands the
// account password afterwards to open the keychain.
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

// RFC 6979. The RP2040 has no hardware TRNG, and a predictable ECDSA nonce
// recovers the private key from a single signature — so the nonce is derived
// from the key and message instead of being drawn from the RNG at all. This is
// not optional on this part. Needs HMAC_DRBG.
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C

// Writing a self-signed certificate on the device, so the private key never
// has to exist on a workshop machine. See identity.c.
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_PEM_WRITE_C

#define MBEDTLS_ERROR_C

// No MBEDTLS_ENTROPY_C / CTR_DRBG / AES: the firmware passes its own RNG
// callback (piv_rng in piv.c) straight to the RSA code, so mbedTLS never needs
// to build a DRBG of its own.

// No check_config.h here: mbedTLS 3.x derives its MBEDTLS_MD_CAN_* macros in
// build_info.h *after* including this file, so checking from inside reports a
// missing hash algorithm that is in fact configured two lines above.
