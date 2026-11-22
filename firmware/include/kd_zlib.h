#ifndef KD_ZLIB_H
#define KD_ZLIB_H

#ifdef __APPLE__
#include <zlib.h>
#else
/* Minimal ABI-exact zlib 1.2.x declarations for ARMv7 Linux libz.so.1 */
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef unsigned char Bytef;
typedef void *voidpf;

typedef struct z_stream_s {
    const Bytef *next_in;
    uInt     avail_in;
    uLong    total_in;

    Bytef    *next_out;
    uInt     avail_out;
    uLong    total_out;

    const char *msg;
    void     *state;

    voidpf   (*zalloc)(voidpf, uInt, uInt);
    void     (*zfree)(voidpf, voidpf);
    voidpf   opaque;

    int      data_type;
    uLong    adler;
    uLong    reserved;
} z_stream;

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_NO_FLUSH      0
#define Z_FINISH        4
#define MAX_WBITS       15

extern const char *zlibVersion(void);
extern int inflateInit2_(z_stream *strm, int windowBits, const char *version, int stream_size);
extern int inflate(z_stream *strm, int flush);
extern int inflateEnd(z_stream *strm);
#endif

#endif /* KD_ZLIB_H */
