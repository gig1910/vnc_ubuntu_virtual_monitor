#include "ra2.h"
#include "auth_client.h"
#include "config.h"
#include "io.h"
#include "ra2_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

#include <nettle/eax.h>
#include <nettle/aes.h>

static int
serialize_public_key(
    EVP_PKEY *key,
    uint8_t **out,
    size_t *out_len)
{
    BIGNUM *n = NULL;
    BIGNUM *e = NULL;
    uint8_t *buf = NULL;
    int rc = -1;

    if (
        EVP_PKEY_get_bn_param(
            key,
            OSSL_PKEY_PARAM_RSA_N,
            &n
        ) != 1 ||
        EVP_PKEY_get_bn_param(
            key,
            OSSL_PKEY_PARAM_RSA_E,
            &e
        ) != 1
    )
        goto out;

    int bits = BN_num_bits(n);
    size_t key_bytes = ((size_t)bits + 7) / 8;
    size_t len = 4 + key_bytes * 2;

    buf = calloc(1, len);

    if (!buf)
        goto out;

    io_put_u32_be(buf, (uint32_t)bits);

    if (
        BN_bn2binpad(
            n,
            buf + 4,
            (int)key_bytes
        ) != (int)key_bytes ||
        BN_bn2binpad(
            e,
            buf + 4 + key_bytes,
            (int)key_bytes
        ) != (int)key_bytes
    )
        goto out;

    *out = buf;
    *out_len = len;
    buf = NULL;
    rc = 0;

out:
    BN_free(n);
    BN_free(e);
    free(buf);
    return rc;
}

static EVP_PKEY *
read_public_key(
    int fd,
    uint8_t **wire_out,
    size_t *wire_len_out)
{
    uint8_t bits_buf[4];

    if (io_read_exact(fd, bits_buf, sizeof(bits_buf)) < 0)
        return NULL;

    uint32_t bits = io_get_u32_be(bits_buf);

    if (bits < 512 || bits > 8192) {
        fprintf(stderr, "Unsupported client RSA key size: %u\n", bits);
        return NULL;
    }

    printf("Client RSA key length: %u bits\n", bits);

    size_t key_bytes = ((size_t)bits + 7) / 8;
    size_t wire_len = 4 + key_bytes * 2;

    uint8_t *wire = malloc(wire_len);

    if (!wire)
        return NULL;

    memcpy(wire, bits_buf, 4);

    if (
        io_read_exact(
            fd,
            wire + 4,
            key_bytes * 2
        ) < 0
    ) {
        free(wire);
        return NULL;
    }

    BIGNUM *n =
        BN_bin2bn(wire + 4, (int)key_bytes, NULL);

    BIGNUM *e =
        BN_bin2bn(
            wire + 4 + key_bytes,
            (int)key_bytes,
            NULL
        );

    if (!n || !e) {
        BN_free(n);
        BN_free(e);
        free(wire);
        return NULL;
    }

    EVP_PKEY_CTX *ctx =
        EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);

    OSSL_PARAM_BLD *bld = NULL;
    OSSL_PARAM *params = NULL;
    EVP_PKEY *key = NULL;

    if (!ctx || EVP_PKEY_fromdata_init(ctx) <= 0)
        goto out;

    bld = OSSL_PARAM_BLD_new();

    if (!bld)
        goto out;

    if (
        !OSSL_PARAM_BLD_push_BN(
            bld,
            OSSL_PKEY_PARAM_RSA_N,
            n
        ) ||
        !OSSL_PARAM_BLD_push_BN(
            bld,
            OSSL_PKEY_PARAM_RSA_E,
            e
        )
    )
        goto out;

    params = OSSL_PARAM_BLD_to_param(bld);

    if (
        !params ||
        EVP_PKEY_fromdata(
            ctx,
            &key,
            EVP_PKEY_PUBLIC_KEY,
            params
        ) <= 0
    ) {
        EVP_PKEY_free(key);
        key = NULL;
    }

out:
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(ctx);
    BN_free(n);
    BN_free(e);

    if (!key) {
        free(wire);
        return NULL;
    }

    *wire_out = wire;
    *wire_len_out = wire_len;
    return key;
}

static int
rsa_encrypt_pkcs1(
    EVP_PKEY *public_key,
    const uint8_t *plain,
    size_t plain_len,
    uint8_t **cipher_out,
    size_t *cipher_len_out)
{
    EVP_PKEY_CTX *ctx =
        EVP_PKEY_CTX_new(public_key, NULL);

    if (!ctx)
        return -1;

    int rc = -1;
    uint8_t *cipher = NULL;
    size_t cipher_len = 0;

    if (
        EVP_PKEY_encrypt_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(
            ctx,
            RSA_PKCS1_PADDING
        ) <= 0 ||
        EVP_PKEY_encrypt(
            ctx,
            NULL,
            &cipher_len,
            plain,
            plain_len
        ) <= 0
    )
        goto out;

    cipher = malloc(cipher_len);

    if (!cipher)
        goto out;

    if (
        EVP_PKEY_encrypt(
            ctx,
            cipher,
            &cipher_len,
            plain,
            plain_len
        ) <= 0
    )
        goto out;

    *cipher_out = cipher;
    *cipher_len_out = cipher_len;
    cipher = NULL;
    rc = 0;

out:
    free(cipher);
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

static int
rsa_decrypt_pkcs1(
    EVP_PKEY *private_key,
    const uint8_t *cipher,
    size_t cipher_len,
    uint8_t *plain,
    size_t *plain_len)
{
    EVP_PKEY_CTX *ctx =
        EVP_PKEY_CTX_new(private_key, NULL);

    if (!ctx)
        return -1;

    int rc = -1;
    uint8_t *tmp = NULL;
    size_t tmp_len = 0;
    size_t tmp_capacity = 0;
    size_t caller_capacity = *plain_len;

    if (
        EVP_PKEY_decrypt_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(
            ctx,
            RSA_PKCS1_PADDING
        ) <= 0 ||
        EVP_PKEY_decrypt(
            ctx,
            NULL,
            &tmp_len,
            cipher,
            cipher_len
        ) <= 0
    )
        goto out;

    tmp_capacity = tmp_len;
    tmp = OPENSSL_malloc(tmp_capacity);

    if (!tmp)
        goto out;

    if (
        EVP_PKEY_decrypt(
            ctx,
            tmp,
            &tmp_len,
            cipher,
            cipher_len
        ) <= 0
    )
        goto out;

    if (tmp_len > caller_capacity) {
        fprintf(
            stderr,
            "RSA plaintext too large: %zu > %zu\n",
            tmp_len,
            caller_capacity
        );
        goto out;
    }

    memcpy(plain, tmp, tmp_len);
    *plain_len = tmp_len;
    rc = 0;

out:
    if (tmp)
        OPENSSL_clear_free(tmp, tmp_capacity);

    EVP_PKEY_CTX_free(ctx);
    return rc;
}

static void
make_nonce(uint64_t counter, uint8_t nonce[16])
{
    memset(nonce, 0, 16);

    for (int i = 0; i < 8; i++)
        nonce[i] = (uint8_t)(counter >> (i * 8));
}

int
ra2_send_record(
    int fd,
    Ra2Direction *dir,
    const uint8_t *plain,
    size_t plain_len)
{
    if (!dir || plain_len > RA2_MAX_RECORD)
        return -1;

    uint8_t header[2];
    io_put_u16_be(header, (uint16_t)plain_len);

    uint8_t nonce[16];
    make_nonce(dir->counter, nonce);

    struct eax_aes128_ctx ctx;
    eax_aes128_set_key(&ctx, dir->key);
    eax_aes128_set_nonce(&ctx, sizeof(nonce), nonce);
    eax_aes128_update(&ctx, sizeof(header), header);

    uint8_t *cipher = malloc(plain_len ? plain_len : 1);

    if (!cipher)
        return -1;

    eax_aes128_encrypt(&ctx, plain_len, cipher, plain);

    uint8_t tag[RA2_TAG_SIZE];
    eax_aes128_digest(&ctx, sizeof(tag), tag);

    int rc =
        io_write_exact(fd, header, sizeof(header)) == 0 &&
        io_write_exact(fd, cipher, plain_len) == 0 &&
        io_write_exact(fd, tag, sizeof(tag)) == 0
            ? 0
            : -1;

    OPENSSL_cleanse(cipher, plain_len);
    free(cipher);

    if (rc == 0)
        dir->counter++;

    return rc;
}

int
ra2_recv_record(
    int fd,
    Ra2Direction *dir,
    uint8_t **plain_out,
    size_t *plain_len_out)
{
    if (!dir || !plain_out || !plain_len_out)
        return -1;

    uint8_t header[2];

    int header_status =
        io_read_exact_status(
            fd,
            header,
            sizeof(header)
        );

    if (header_status == 0) {
        fprintf(
            stderr,
            "RA2 peer closed connection while reading record header\n"
        );
        return -1;
    }

    if (header_status < 0) {
        perror("RA2 read record header");
        return -1;
    }

    size_t len = io_get_u16_be(header);

    uint8_t *cipher = malloc(len ? len : 1);
    uint8_t *plain = malloc(len ? len : 1);

    if (!cipher || !plain) {
        free(cipher);
        free(plain);
        return -1;
    }

    uint8_t received_tag[RA2_TAG_SIZE];

    int payload_status =
        io_read_exact_status(
            fd,
            cipher,
            len
        );

    if (payload_status <= 0) {
        if (payload_status == 0)
            fprintf(
                stderr,
                "RA2 peer closed connection during encrypted payload\n"
            );
        else
            perror("RA2 read encrypted payload");

        free(cipher);
        free(plain);
        return -1;
    }

    int tag_status =
        io_read_exact_status(
            fd,
            received_tag,
            sizeof(received_tag)
        );

    if (tag_status <= 0) {
        if (tag_status == 0)
            fprintf(
                stderr,
                "RA2 peer closed connection during authentication tag\n"
            );
        else
            perror("RA2 read authentication tag");

        free(cipher);
        free(plain);
        return -1;
    }

    uint8_t nonce[16];
    make_nonce(dir->counter, nonce);

    struct eax_aes128_ctx ctx;
    eax_aes128_set_key(&ctx, dir->key);
    eax_aes128_set_nonce(&ctx, sizeof(nonce), nonce);
    eax_aes128_update(&ctx, sizeof(header), header);
    eax_aes128_decrypt(&ctx, len, plain, cipher);

    uint8_t calculated_tag[RA2_TAG_SIZE];
    eax_aes128_digest(
        &ctx,
        sizeof(calculated_tag),
        calculated_tag
    );

    OPENSSL_cleanse(cipher, len);
    free(cipher);

    if (
        CRYPTO_memcmp(
            received_tag,
            calculated_tag,
            RA2_TAG_SIZE
        ) != 0
    ) {
        fprintf(
            stderr,
            "EAX authentication tag mismatch at incoming record #%llu "
            "(payload=%zu bytes)\n",
            (unsigned long long)dir->counter,
            len
        );
        OPENSSL_cleanse(plain, len);
        free(plain);
        return -1;
    }

    dir->counter++;
    *plain_out = plain;
    *plain_len_out = len;
    return 0;
}

static int
sha1_concat(
    const uint8_t *a,
    size_t a_len,
    const uint8_t *b,
    size_t b_len,
    uint8_t out[SHA_DIGEST_LENGTH])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    if (!ctx)
        return -1;

    unsigned int len = 0;
    int rc =
        EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, a, a_len) == 1 &&
        EVP_DigestUpdate(ctx, b, b_len) == 1 &&
        EVP_DigestFinal_ex(ctx, out, &len) == 1 &&
        len == SHA_DIGEST_LENGTH
            ? 0
            : -1;

    EVP_MD_CTX_free(ctx);
    return rc;
}

static int
derive_session_keys(
    const uint8_t server_random[RA2_RANDOM_SIZE],
    const uint8_t client_random[RA2_RANDOM_SIZE],
    Ra2Direction *client_to_server,
    Ra2Direction *server_to_client)
{
    uint8_t digest[SHA_DIGEST_LENGTH];

    if (
        sha1_concat(
            server_random,
            RA2_RANDOM_SIZE,
            client_random,
            RA2_RANDOM_SIZE,
            digest
        ) < 0
    )
        return -1;

    memcpy(client_to_server->key, digest, 16);

    if (
        sha1_concat(
            client_random,
            RA2_RANDOM_SIZE,
            server_random,
            RA2_RANDOM_SIZE,
            digest
        ) < 0
    ) {
        OPENSSL_cleanse(digest, sizeof(digest));
        return -1;
    }

    memcpy(server_to_client->key, digest, 16);
    OPENSSL_cleanse(digest, sizeof(digest));

    client_to_server->counter = 0;
    server_to_client->counter = 0;
    return 0;
}

static int
ra2r_rekey(
    int fd,
    Ra2Direction *client_to_server,
    Ra2Direction *server_to_client)
{
    uint8_t server_random[RA2_RANDOM_SIZE];

    if (
        RAND_bytes(
            server_random,
            sizeof(server_random)
        ) != 1
    )
        return -1;

    if (
        ra2_send_record(
            fd,
            server_to_client,
            server_random,
            sizeof(server_random)
        ) < 0
    ) {
        OPENSSL_cleanse(
            server_random,
            sizeof(server_random)
        );
        return -1;
    }

    uint8_t *client_msg = NULL;
    size_t client_len = 0;

    if (
        ra2_recv_record(
            fd,
            client_to_server,
            &client_msg,
            &client_len
        ) < 0
    ) {
        OPENSSL_cleanse(
            server_random,
            sizeof(server_random)
        );
        return -1;
    }

    if (client_len != RA2_RANDOM_SIZE) {
        OPENSSL_cleanse(client_msg, client_len);
        free(client_msg);
        OPENSSL_cleanse(
            server_random,
            sizeof(server_random)
        );
        return -1;
    }

    uint8_t client_random[RA2_RANDOM_SIZE];
    memcpy(client_random, client_msg, RA2_RANDOM_SIZE);

    OPENSSL_cleanse(client_msg, client_len);
    free(client_msg);

    int rc =
        derive_session_keys(
            server_random,
            client_random,
            client_to_server,
            server_to_client
        );

    OPENSSL_cleanse(
        server_random,
        sizeof(server_random)
    );

    OPENSSL_cleanse(
        client_random,
        sizeof(client_random)
    );

    return rc;
}

static int
send_security_result(
    int fd,
    Ra2Direction *server_to_client,
    int ok,
    const char *reason)
{
    if (ok) {
        uint8_t result[4];
        io_put_u32_be(result, 0);

        return ra2_send_record(
            fd,
            server_to_client,
            result,
            sizeof(result)
        );
    }

    if (!reason)
        reason = "Authentication failed";

    size_t reason_len = strlen(reason);

    if (reason_len > 65527)
        reason_len = 65527;

    size_t total = 8 + reason_len;
    uint8_t *msg = malloc(total);

    if (!msg)
        return -1;

    io_put_u32_be(msg, 1);
    io_put_u32_be(msg + 4, (uint32_t)reason_len);
    memcpy(msg + 8, reason, reason_len);

    int rc =
        ra2_send_record(
            fd,
            server_to_client,
            msg,
            total
        );

    OPENSSL_cleanse(msg, total);
    free(msg);
    return rc;
}

int
ra2_server_handshake(
    int fd,
    Ra2Session *session,
    const RuntimeConfig *cfg)
{
    if (!session || !cfg)
        return -1;

    memset(session, 0, sizeof(*session));

    int rc = -1;
    EVP_PKEY *server_key = NULL;
    EVP_PKEY *client_key = NULL;
    uint8_t *server_wire = NULL;
    uint8_t *client_wire = NULL;
    size_t server_wire_len = 0;
    size_t client_wire_len = 0;
    uint8_t *credentials = NULL;
    size_t credentials_len = 0;
    char *password = NULL;

    server_key =
        ra2_identity_load_or_create(
            cfg->ra2_key_file
        );

    if (
        !server_key ||
        serialize_public_key(
            server_key,
            &server_wire,
            &server_wire_len
        ) < 0
    )
        goto out;

    const char rfb_version[] = "RFB 003.008\n";

    if (io_write_exact(fd, rfb_version, 12) < 0)
        goto out;

    char client_version[13] = {0};

    if (io_read_exact(fd, client_version, 12) < 0)
        goto out;

    printf("Client version: %.12s", client_version);

    const uint8_t security[] = {1, 13};

    if (io_write_exact(fd, security, sizeof(security)) < 0)
        goto out;

    uint8_t selected = 0;

    if (
        io_read_exact(fd, &selected, 1) < 0 ||
        selected != 13
    ) {
        fprintf(stderr, "Client did not select RA2r\n");
        goto out;
    }

    printf("Security type selected: 13 (RA2r)\n");

    if (
        io_write_exact(
            fd,
            server_wire,
            server_wire_len
        ) < 0
    )
        goto out;

    client_key =
        read_public_key(
            fd,
            &client_wire,
            &client_wire_len
        );

    if (!client_key)
        goto out;

    uint8_t server_random[RA2_RANDOM_SIZE];

    if (
        RAND_bytes(
            server_random,
            sizeof(server_random)
        ) != 1
    )
        goto out;

    uint8_t *enc_server_random = NULL;
    size_t enc_server_random_len = 0;

    if (
        rsa_encrypt_pkcs1(
            client_key,
            server_random,
            sizeof(server_random),
            &enc_server_random,
            &enc_server_random_len
        ) < 0 ||
        enc_server_random_len > 65535
    ) {
        free(enc_server_random);
        goto out;
    }

    uint8_t random_len[2];
    io_put_u16_be(
        random_len,
        (uint16_t)enc_server_random_len
    );

    if (
        io_write_exact(
            fd,
            random_len,
            sizeof(random_len)
        ) < 0 ||
        io_write_exact(
            fd,
            enc_server_random,
            enc_server_random_len
        ) < 0
    ) {
        OPENSSL_cleanse(
            enc_server_random,
            enc_server_random_len
        );
        free(enc_server_random);
        goto out;
    }

    OPENSSL_cleanse(
        enc_server_random,
        enc_server_random_len
    );
    free(enc_server_random);

    uint8_t enc_len_buf[2];

    if (io_read_exact(fd, enc_len_buf, 2) < 0)
        goto out;

    size_t enc_client_random_len =
        io_get_u16_be(enc_len_buf);

    if (
        enc_client_random_len == 0 ||
        enc_client_random_len > 8192
    )
        goto out;

    uint8_t *enc_client_random =
        malloc(enc_client_random_len);

    if (!enc_client_random)
        goto out;

    if (
        io_read_exact(
            fd,
            enc_client_random,
            enc_client_random_len
        ) < 0
    ) {
        free(enc_client_random);
        goto out;
    }

    uint8_t client_random[RA2_RANDOM_SIZE];
    size_t client_random_len = sizeof(client_random);

    if (
        rsa_decrypt_pkcs1(
            server_key,
            enc_client_random,
            enc_client_random_len,
            client_random,
            &client_random_len
        ) < 0
    ) {
        OPENSSL_cleanse(
            enc_client_random,
            enc_client_random_len
        );
        free(enc_client_random);
        goto out;
    }

    OPENSSL_cleanse(
        enc_client_random,
        enc_client_random_len
    );
    free(enc_client_random);

    if (client_random_len != RA2_RANDOM_SIZE)
        goto out;

    if (
        derive_session_keys(
            server_random,
            client_random,
            &session->client_to_server,
            &session->server_to_client
        ) < 0
    )
        goto out;

    OPENSSL_cleanse(server_random, sizeof(server_random));
    OPENSSL_cleanse(client_random, sizeof(client_random));

    uint8_t server_hash[RA2_HASH_SIZE];
    uint8_t expected_client_hash[RA2_HASH_SIZE];

    if (
        sha1_concat(
            server_wire,
            server_wire_len,
            client_wire,
            client_wire_len,
            server_hash
        ) < 0 ||
        sha1_concat(
            client_wire,
            client_wire_len,
            server_wire,
            server_wire_len,
            expected_client_hash
        ) < 0
    )
        goto out;

    if (
        ra2_send_record(
            fd,
            &session->server_to_client,
            server_hash,
            sizeof(server_hash)
        ) < 0
    )
        goto out;

    uint8_t *client_hash = NULL;
    size_t client_hash_len = 0;

    if (
        ra2_recv_record(
            fd,
            &session->client_to_server,
            &client_hash,
            &client_hash_len
        ) < 0
    )
        goto out;

    if (
        client_hash_len != RA2_HASH_SIZE ||
        CRYPTO_memcmp(
            client_hash,
            expected_client_hash,
            RA2_HASH_SIZE
        ) != 0
    ) {
        OPENSSL_cleanse(client_hash, client_hash_len);
        free(client_hash);
        goto out;
    }

    OPENSSL_cleanse(client_hash, client_hash_len);
    free(client_hash);

    const uint8_t subtype = 1;

    if (
        ra2_send_record(
            fd,
            &session->server_to_client,
            &subtype,
            1
        ) < 0
    )
        goto out;

    if (
        ra2_recv_record(
            fd,
            &session->client_to_server,
            &credentials,
            &credentials_len
        ) < 0 ||
        credentials_len < 2
    )
        goto out;

    size_t pos = 0;
    uint8_t username_len = credentials[pos++];

    if (
        username_len == 0 ||
        pos + username_len + 1 > credentials_len
    )
        goto out;

    memcpy(
        session->username,
        credentials + pos,
        username_len
    );
    session->username[username_len] = '\0';
    pos += username_len;

    uint8_t password_len = credentials[pos++];

    if (pos + password_len > credentials_len)
        goto out;

    password = calloc((size_t)password_len + 1, 1);

    if (!password)
        goto out;

    memcpy(password, credentials + pos, password_len);

    printf(
        "RA2r credentials received for user \"%s\"\n",
        session->username
    );

    int auth =
        auth_client_check(
            cfg->auth_socket,
            session->username,
            password
        );

    OPENSSL_cleanse(
        password,
        (size_t)password_len + 1
    );
    free(password);
    password = NULL;

    OPENSSL_cleanse(credentials, credentials_len);
    free(credentials);
    credentials = NULL;
    credentials_len = 0;

    if (auth < 0)
        goto out;

    /*
     * RA2r performs the second encrypted random exchange before
     * SecurityResult, even when authentication is denied.
     */
    if (
        ra2r_rekey(
            fd,
            &session->client_to_server,
            &session->server_to_client
        ) < 0
    )
        goto out;

    if (!auth) {
        (void)send_security_result(
            fd,
            &session->server_to_client,
            0,
            "Ubuntu authentication failed"
        );
        fprintf(stderr, "Authentication denied\n");
        goto out;
    }

    if (
        send_security_result(
            fd,
            &session->server_to_client,
            1,
            NULL
        ) < 0
    )
        goto out;

    printf(
        "RA2r + auth-helper handshake complete for \"%s\"\n",
        session->username
    );

    rc = 0;

out:
    if (password) {
        OPENSSL_cleanse(password, strlen(password));
        free(password);
    }

    if (credentials) {
        OPENSSL_cleanse(credentials, credentials_len);
        free(credentials);
    }

    EVP_PKEY_free(client_key);
    EVP_PKEY_free(server_key);
    free(client_wire);
    free(server_wire);

    if (rc < 0)
        ra2_session_clear(session);

    return rc;
}

void
ra2_session_clear(Ra2Session *session)
{
    if (!session)
        return;

    OPENSSL_cleanse(session, sizeof(*session));
}
