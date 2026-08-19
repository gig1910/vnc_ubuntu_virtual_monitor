#define _GNU_SOURCE

#include "ra2_identity.h"
#include "config.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static EVP_PKEY *
generate_rsa_key(void)
{
    EVP_PKEY_CTX *ctx =
        EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);

    if (!ctx)
        return NULL;

    EVP_PKEY *key = NULL;

    if (EVP_PKEY_keygen_init(ctx) <= 0)
        goto fail;

    if (
        EVP_PKEY_CTX_set_rsa_keygen_bits(
            ctx,
            RA2_RSA_BITS
        ) <= 0
    )
        goto fail;

    if (EVP_PKEY_keygen(ctx, &key) <= 0)
        goto fail;

    EVP_PKEY_CTX_free(ctx);
    return key;

fail:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    return NULL;
}

static EVP_PKEY *
load_private_key(const char *path)
{
    FILE *fp = fopen(path, "rb");

    if (!fp)
        return NULL;

    EVP_PKEY *key =
        PEM_read_PrivateKey(
            fp,
            NULL,
            NULL,
            NULL
        );

    fclose(fp);

    if (!key) {
        fprintf(
            stderr,
            "Failed to parse RA2 server key: %s\n",
            path
        );
    }

    return key;
}

static int
save_private_key_0600(
    const char *path,
    EVP_PKEY *key)
{
    int fd =
        open(
            path,
            O_WRONLY |
            O_CREAT |
            O_EXCL |
            O_CLOEXEC,
            0600
        );

    if (fd < 0) {
        if (errno == EEXIST)
            return 1;

        perror("create RA2 server key");
        return -1;
    }

    FILE *fp = fdopen(fd, "wb");

    if (!fp) {
        perror("fdopen RA2 server key");
        close(fd);
        unlink(path);
        return -1;
    }

    int ok =
        PEM_write_PrivateKey(
            fp,
            key,
            NULL,
            NULL,
            0,
            NULL,
            NULL
        );

    int flush_ok = fflush(fp) == 0;

    if (flush_ok)
        flush_ok = fsync(fd) == 0;

    int close_ok = fclose(fp) == 0;

    if (!ok || !flush_ok || !close_ok) {
        fprintf(
            stderr,
            "Failed writing RA2 server key: %s\n",
            path
        );

        unlink(path);
        return -1;
    }

    if (chmod(path, 0600) < 0) {
        perror("chmod RA2 server key");
        unlink(path);
        return -1;
    }

    return 0;
}

EVP_PKEY *
ra2_identity_load_or_create(const char *path)
{
    if (!path || !*path)
        return NULL;

    EVP_PKEY *key =
        load_private_key(path);

    if (key) {
        printf(
            "Loaded persistent RA2 server identity: %s\n",
            path
        );

        return key;
    }

    if (errno != ENOENT && access(path, F_OK) == 0)
        return NULL;

    printf(
        "Generating persistent %d-bit RA2 server identity...\n",
        RA2_RSA_BITS
    );

    key = generate_rsa_key();

    if (!key) {
        fprintf(
            stderr,
            "Failed generating RA2 RSA key\n"
        );

        return NULL;
    }

    int save_rc =
        save_private_key_0600(
            path,
            key
        );

    if (save_rc == 0) {
        printf(
            "Saved persistent RA2 server identity: %s (mode 0600)\n",
            path
        );

        return key;
    }

    if (save_rc == 1) {
        /*
         * Another instance created it between our load and create.
         * Prefer the on-disk identity so all instances converge on one key.
         */
        EVP_PKEY_free(key);
        key = load_private_key(path);

        if (key) {
            printf(
                "Loaded RA2 server identity created concurrently: %s\n",
                path
            );
        }

        return key;
    }

    EVP_PKEY_free(key);
    return NULL;
}
