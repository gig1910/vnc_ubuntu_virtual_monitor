#ifndef VNC_MONITOR_RA2_IDENTITY_H
#define VNC_MONITOR_RA2_IDENTITY_H

#include <openssl/evp.h>

/*
 * Loads a persistent RSA private key from path.
 * If the file does not exist, creates a new RA2_RSA_BITS key and writes it
 * atomically enough for this single-process test with mode 0600.
 *
 * Caller owns the returned EVP_PKEY and must EVP_PKEY_free() it.
 */
EVP_PKEY *ra2_identity_load_or_create(const char *path);

#endif
