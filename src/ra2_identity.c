#define _GNU_SOURCE

#include "ra2_identity.h"
#include "config.h"
#include "log.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static EVP_PKEY *
generate_rsa_key(void)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    if (!ctx)
        return NULL;

    EVP_PKEY *key = NULL;
    if (EVP_PKEY_keygen_init(ctx) <= 0)
        goto fail;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, RA2_RSA_BITS) <= 0)
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

    EVP_PKEY *key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);

    if (!key)
        LOG_ERROR("Failed to parse RA2 server identity: %s", path);

    return key;
}

static int
ensure_parent_directory(const char *path)
{
    char *copy = strdup(path);
    if (!copy)
        return -1;

    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) {
        free(copy);
        return 0;
    }

    *slash = '\0';

    if (mkdir(copy, 0700) < 0 && errno != EEXIST) {
        LOG_ERROR("Could not create RA2 identity directory %s: %s",
                  copy,
                  strerror(errno));
        free(copy);
        return -1;
    }

    struct stat st;
    if (stat(copy, &st) < 0 || !S_ISDIR(st.st_mode)) {
        LOG_ERROR("RA2 identity parent is not a directory: %s", copy);
        free(copy);
        return -1;
    }

    free(copy);
    return 0;
}

static int
save_private_key_0600(const char *path, EVP_PKEY *key)
{
    if (ensure_parent_directory(path) < 0)
        return -1;

    int fd = open(path,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                  0600);
    if (fd < 0) {
        if (errno == EEXIST)
            return 1;

        LOG_ERROR("Could not create RA2 server identity %s: %s",
                  path,
                  strerror(errno));
        return -1;
    }

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        LOG_ERROR("fdopen failed for RA2 identity %s: %s",
                  path,
                  strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }

    int ok = PEM_write_PrivateKey(fp,
                                  key,
                                  NULL,
                                  NULL,
                                  0,
                                  NULL,
                                  NULL);

    int flush_ok = fflush(fp) == 0;
    if (flush_ok)
        flush_ok = fsync(fd) == 0;
    int close_ok = fclose(fp) == 0;

    if (!ok || !flush_ok || !close_ok) {
        LOG_ERROR("Failed writing RA2 server identity: %s", path);
        unlink(path);
        return -1;
    }

    if (chmod(path, 0600) < 0) {
        LOG_ERROR("chmod(0600) failed for RA2 identity %s: %s",
                  path,
                  strerror(errno));
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

    EVP_PKEY *key = load_private_key(path);
    if (key) {
        LOG_DEBUG("Loaded persistent RA2 server identity: %s", path);
        return key;
    }

    if (errno != ENOENT && access(path, F_OK) == 0)
        return NULL;

    LOG_INFO("Generating persistent %d-bit RA2 server identity", RA2_RSA_BITS);
    key = generate_rsa_key();
    if (!key) {
        LOG_ERROR("Failed generating RA2 RSA key");
        return NULL;
    }

    int save_rc = save_private_key_0600(path, key);
    if (save_rc == 0) {
        LOG_INFO("Saved persistent RA2 server identity: %s (mode 0600)", path);
        return key;
    }

    if (save_rc == 1) {
        EVP_PKEY_free(key);
        key = load_private_key(path);
        if (key)
            LOG_DEBUG("Loaded RA2 identity created concurrently: %s", path);
        return key;
    }

    EVP_PKEY_free(key);
    return NULL;
}
