#include "tls_pair.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdio.h>
#include <string.h>

static void
set_reason(char *reason, size_t reason_size, const char *value)
{
    if (!reason || reason_size == 0)
        return;

    snprintf(reason, reason_size, "%s", value ? value : "internal-error");
}

int
tls_pair_validate(const char *certificate_file,
                  const char *private_key_file,
                  char *reason,
                  size_t reason_size)
{
    if (!certificate_file || !*certificate_file ||
        !private_key_file || !*private_key_file) {
        set_reason(reason, reason_size, "internal-error");
        return -1;
    }

    ERR_clear_error();

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        set_reason(reason, reason_size, "internal-error");
        ERR_clear_error();
        return -1;
    }

    if (SSL_CTX_use_certificate_chain_file(ctx, certificate_file) != 1) {
        set_reason(reason, reason_size, "certificate-invalid");
        SSL_CTX_free(ctx);
        ERR_clear_error();
        return -1;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx,
                                    private_key_file,
                                    SSL_FILETYPE_PEM) != 1) {
        set_reason(reason, reason_size, "private-key-invalid");
        SSL_CTX_free(ctx);
        ERR_clear_error();
        return -1;
    }

    if (SSL_CTX_check_private_key(ctx) != 1) {
        set_reason(reason, reason_size, "certificate-key-mismatch");
        SSL_CTX_free(ctx);
        ERR_clear_error();
        return -1;
    }

    SSL_CTX_free(ctx);
    ERR_clear_error();
    set_reason(reason, reason_size, "ok");
    return 0;
}
