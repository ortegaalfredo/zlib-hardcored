/* compress.c -- compress a memory buffer
 * Copyright (C) 1995-2005, 2014, 2016 Jean-loup Gailly, Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#define ZLIB_INTERNAL
#include "zlib.h"

/* ===========================================================================
     Compresses the source buffer into the destination buffer. The level
   parameter has the same meaning as in deflateInit.  sourceLen is the byte
   length of the source buffer. Upon entry, destLen is the total size of the
   destination buffer, which must be at least 0.1% larger than sourceLen plus
   12 bytes. Upon exit, destLen is the actual size of the compressed buffer.

     compress2 returns Z_OK if success, Z_MEM_ERROR if there was not enough
   memory, Z_BUF_ERROR if there was not enough room in the output buffer,
   Z_STREAM_ERROR if the level parameter is invalid.
*/
int ZEXPORT compress2(Bytef *dest, uLongf *destLen, const Bytef *source,
                      uLong sourceLen, int level) {
    if (dest == NULL || destLen == NULL || source == NULL) {
        return Z_STREAM_ERROR;  // Return an error if any pointer is NULL
    }

    if (*destLen == 0 || sourceLen == 0) {
        return Z_BUF_ERROR;  // Return an error if lengths are zero
    }

    if (sourceLen > UINT_MAX || *destLen > UINT_MAX) {
        return Z_DATA_ERROR;  // Ensure sourceLen and *destLen fit within uInt limit
    }

    z_stream stream;
    int err;
    const uInt max = (uInt)-1;
    uLong left;

    left = *destLen;
    *destLen = 0;

    stream.zalloc = (alloc_func)0;
    stream.zfree = (free_func)0;
    stream.opaque = (voidpf)0;

    err = deflateInit(&stream, level);
    if (err != Z_OK) return err;

    stream.next_out = dest;
    stream.avail_out = 0;
    stream.next_in = (z_const Bytef *)source;
    stream.avail_in = 0;

    do {
        if (stream.avail_out == 0) {
            if (left > (uLong)UINT_MAX) {
                stream.avail_out = UINT_MAX;
                left -= UINT_MAX;
            } else {
                stream.avail_out = (uInt)left;
                left = 0;
            }
        }
        if (stream.avail_in == 0) {
            if (sourceLen > (uLong)UINT_MAX) {
                stream.avail_in = UINT_MAX;
                sourceLen -= UINT_MAX;
            } else {
                stream.avail_in = (uInt)sourceLen;
                sourceLen = 0;
            }
        }
        err = deflate(&stream, sourceLen ? Z_NO_FLUSH : Z_FINISH);
    } while (err == Z_OK);

    *destLen = stream.total_out;
    deflateEnd(&stream);
    return err == Z_STREAM_END ? Z_OK : err;
}

/* ===========================================================================
 */
int ZEXPORT compress(Bytef *dest, uLongf *destLen, const Bytef *source,
                     uLong sourceLen) {
    if (dest == NULL || destLen == NULL || source == NULL) {
        return Z_MEM_ERROR;
    }
    if (sourceLen >= UINT_MAX || *destLen >= UINT_MAX) {
        return Z_BUF_ERROR;
    }
    return compress2(dest, destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
}

/* ===========================================================================
     If the default memLevel or windowBits for deflateInit() is changed, then
   this function needs to be updated.
 */
uLong ZEXPORT compressBound(uLong sourceLen) {
    if (sourceLen > ULONG_MAX - 13 - (sourceLen >> 12) - (sourceLen >> 14) - (sourceLen >> 25)) {
        return 0; // Indicate overflow or invalid length
    }
    return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) +
           (sourceLen >> 25) + 13;
}
