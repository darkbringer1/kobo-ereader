#ifndef KD_TLS_H
#define KD_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct KdTlsConn KdTlsConn;

/* Open a TLS 1.2/1.3 client connection over TCP with SNI and CA verification.
 * ca_file may be NULL to auto-discover standard PEM CA bundles.
 * Returns NULL on failure (and writes a concise message into err_buf if provided). */
KdTlsConn *kd_tls_connect(const char *host, const char *port,
                          const char *ca_file, int timeout_sec,
                          char *err_buf, size_t err_sz);

/* Write all bytes over TLS. Returns bytes written or -1 on error. */
ssize_t kd_tls_write(KdTlsConn *conn, const void *buf, size_t len);

/* Read up to len bytes over TLS with poll timeout_ms (-1 = blocking, >=0 = ms).
 * Returns >0 bytes read, 0 on clean EOF, -2 on timeout, -1 on error. */
ssize_t kd_tls_read(KdTlsConn *conn, void *buf, size_t len, int timeout_ms);

/* Underlying TCP fd (for poll loops) and pending decrypted byte check. */
int kd_tls_fd(const KdTlsConn *conn);
bool kd_tls_has_pending(const KdTlsConn *conn);

/* Close TLS session and free resources. */
void kd_tls_close(KdTlsConn *conn);

#endif /* KD_TLS_H */
