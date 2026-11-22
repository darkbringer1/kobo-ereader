#define _GNU_SOURCE
#include "kd_tls.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

struct KdTlsConn {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
};

static bool g_psa_inited = false;

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = send(fd, buf, len, 0);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n > 0) return (int)n;
        if (n == 0) return 0; /* EOF */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout_ms) {
    int fd = *(int *)ctx;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int to = (timeout_ms == 0) ? -1 : (int)timeout_ms;
    int pr;
    do {
        pr = poll(&pfd, 1, to);
    } while (pr < 0 && errno == EINTR);

    if (pr == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (pr < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return bio_recv(ctx, buf, len);
}

/* Verify callback: always enforce cryptographic chain & hostname,
 * but if the device RTC hasn't synced yet (< 2024), ignore date-only flags
 * so the very first HTTPS sync can bootstrap time from the HTTP Date header! */
static int verify_cb(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)data;
    (void)crt;
    (void)depth;
    if (time(NULL) < 1700000000) {
        *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    }
    return 0;
}

static int connect_tcp(const char *host, const char *port, int timeout_sec) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { timeout_sec > 0 ? timeout_sec : 10, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static bool load_ca_bundle(mbedtls_x509_crt *cacert, const char *ca_file) {
    static const char *fallbacks[] = {
        "/mnt/onboard/.adds/kobodash/certs/cacert.pem",
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        NULL
    };
    if (ca_file && ca_file[0] && access(ca_file, R_OK) == 0) {
        if (mbedtls_x509_crt_parse_file(cacert, ca_file) >= 0) {
            return true;
        }
    }
    for (int i = 0; fallbacks[i] != NULL; i++) {
        if (access(fallbacks[i], R_OK) == 0) {
            if (mbedtls_x509_crt_parse_file(cacert, fallbacks[i]) >= 0) {
                return true;
            }
        }
    }
    return false;
}

KdTlsConn *kd_tls_connect(const char *host, const char *port,
                          const char *ca_file, int timeout_sec,
                          char *err_buf, size_t err_sz) {
    if (err_buf && err_sz > 0) err_buf[0] = '\0';
    if (!host || !host[0] || !port || !port[0]) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "Invalid TLS host/port");
        return NULL;
    }

    if (!g_psa_inited) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "TLS crypto init failed");
            return NULL;
        }
        g_psa_inited = true;
    }

    KdTlsConn *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = -1;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->cacert);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_entropy_init(&c->entropy);

    const char *pers = "kobodash_tls";
    int rc = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                                   (const unsigned char *)pers, strlen(pers));
    if (rc != 0) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "TLS RNG seed failed (%d)", rc);
        kd_tls_close(c);
        return NULL;
    }

    if (!load_ca_bundle(&c->cacert, ca_file)) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "Missing CA bundle (cacert.pem)");
        kd_tls_close(c);
        return NULL;
    }

    c->fd = connect_tcp(host, port, timeout_sec);
    if (c->fd < 0) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "TCP connect failed");
        kd_tls_close(c);
        return NULL;
    }

    rc = mbedtls_ssl_config_defaults(&c->conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "TLS config failed");
        kd_tls_close(c);
        return NULL;
    }

    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
    mbedtls_ssl_conf_verify(&c->conf, verify_cb, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&c->conf, (uint32_t)((timeout_sec > 0 ? timeout_sec : 10) * 1000));

    static const char *alpn_list[] = { "http/1.1", NULL };
    (void)mbedtls_ssl_conf_alpn_protocols(&c->conf, alpn_list);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc != 0) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "TLS setup failed");
        kd_tls_close(c);
        return NULL;
    }

    rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (rc != 0) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "TLS SNI failed");
        kd_tls_close(c);
        return NULL;
    }

    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, bio_recv_timeout);

    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (err_buf && err_sz > 0) {
                char mb[80];
                mbedtls_strerror(rc, mb, sizeof(mb));
                snprintf(err_buf, err_sz, "TLS handshake: %.60s", mb);
            }
            kd_tls_close(c);
            return NULL;
        }
    }

    uint32_t vflags = mbedtls_ssl_get_verify_result(&c->ssl);
    if (vflags != 0) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "TLS cert verify 0x%x", vflags);
        kd_tls_close(c);
        return NULL;
    }

    return c;
}

ssize_t kd_tls_write(KdTlsConn *conn, const void *buf, size_t len) {
    if (!conn || !buf) return -1;
    const unsigned char *p = (const unsigned char *)buf;
    size_t sent = 0;
    while (sent < len) {
        int rc = mbedtls_ssl_write(&conn->ssl, p + sent, len - sent);
        if (rc > 0) {
            sent += (size_t)rc;
        } else if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            return -1;
        }
    }
    return (ssize_t)sent;
}

ssize_t kd_tls_read(KdTlsConn *conn, void *buf, size_t len, int timeout_ms) {
    if (!conn || !buf || len == 0) return -1;
    mbedtls_ssl_conf_read_timeout(&conn->conf, timeout_ms > 0 ? (uint32_t)timeout_ms : 0);
    for (;;) {
        int rc = mbedtls_ssl_read(&conn->ssl, (unsigned char *)buf, len);
        if (rc > 0) return (ssize_t)rc;
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) return -2;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
            rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        return -1;
    }
}

int kd_tls_fd(const KdTlsConn *conn) {
    return conn ? conn->fd : -1;
}

bool kd_tls_has_pending(const KdTlsConn *conn) {
    if (!conn) return false;
    return mbedtls_ssl_get_bytes_avail(&conn->ssl) > 0;
}

void kd_tls_close(KdTlsConn *conn) {
    if (!conn) return;
    if (conn->fd >= 0) {
        (void)mbedtls_ssl_close_notify(&conn->ssl);
        close(conn->fd);
        conn->fd = -1;
    }
    mbedtls_ssl_free(&conn->ssl);
    mbedtls_ssl_config_free(&conn->conf);
    mbedtls_x509_crt_free(&conn->cacert);
    mbedtls_ctr_drbg_free(&conn->ctr_drbg);
    mbedtls_entropy_free(&conn->entropy);
    free(conn);
}
