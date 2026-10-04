#ifndef VNC_MONITOR_TLS_PAIR_H
#define VNC_MONITOR_TLS_PAIR_H

#include <stddef.h>

/*
 * Validate that certificate_file contains a loadable PEM server certificate
 * chain, private_key_file contains a loadable PEM private key, and that the
 * private key matches the leaf certificate's public key.
 *
 * Returns 0 on success, -1 on failure.  On failure, reason receives one of:
 *   certificate-invalid
 *   private-key-invalid
 *   certificate-key-mismatch
 *   internal-error
 */
int tls_pair_validate(const char *certificate_file,
                      const char *private_key_file,
                      char *reason,
                      size_t reason_size);

#endif
