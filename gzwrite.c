/* gzwrite.c -- zlib functions for writing gzip files
 * Copyright (C) 2004-2019 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "gzguts.h"

/* Initialize state for writing a gzip file.  Mark initialization by setting
   state->size to non-zero.  Return -1 on a memory allocation failure, or 0 on
   success. */
local int gz_init(gz_statep state) {
    int ret;
    z_streamp strm = &(state->strm);

    /* allocate input buffer (double size for gzprintf) */
    state->in = (unsigned char *)malloc(state->want << 1);
    if (state->in == NULL) {
        gz_error(state, Z_MEM_ERROR, "out of memory");
        return -1;
    }

    /* only need output buffer and deflate state if compressing */
    if (!state->direct) {
        /* allocate output buffer */
        state->out = (unsigned char *)malloc(state->want);
        if (state->out == NULL) {
            free(state->in);
            gz_error(state, Z_MEM_ERROR, "out of memory");
            return -1;
        }

        /* allocate deflate memory, set up for gzip compression */
        strm->zalloc = Z_NULL;
        strm->zfree = Z_NULL;
        strm->opaque = Z_NULL;
        ret = deflateInit2(strm, state->level, Z_DEFLATED,
                           MAX_WBITS + 16, DEF_MEM_LEVEL, state->strategy);
        if (ret != Z_OK) {
            free(state->out);
            free(state->in);
            gz_error(state, Z_MEM_ERROR, "out of memory");
            return -1;
        }
        strm->next_in = NULL;
    }

    /* mark state as initialized */
    state->size = state->want;

    /* initialize write buffer if compressing */
    if (!state->direct) {
        strm->avail_out = state->size;
        strm->next_out = state->out;
        state->x.next = strm->next_out;
    }
    return 0;
}

/* Compress whatever is at avail_in and next_in and write to the output file.
   Return -1 if there is an error writing to the output file or if gz_init()
   fails to allocate memory, otherwise 0.  flush is assumed to be a valid
   deflate() flush value.  If flush is Z_FINISH, then the deflate() state is
   reset to start a new gzip stream.  If gz->direct is true, then simply write
   to the output file without compressing, and ignore flush. */
local int gz_comp(gz_statep state, int flush) {
    int ret, writ;
    unsigned have, put, max = ((unsigned)-1 >> 2) + 1;
    z_streamp strm = &(state->strm);

    /* allocate memory if this is the first time through */
    if (state->size == 0 && gz_init(state) == -1)
        return -1;

    /* write directly if requested */
    if (state->direct) {
        while (strm->avail_in) {
            put = strm->avail_in > max ? max : strm->avail_in;
            writ = write(state->fd, strm->next_in, put);
            if (writ < 0) {
                gz_error(state, Z_ERRNO, zstrerror());
                return -1;
            }
            strm->avail_in -= (unsigned)writ;
            strm->next_in += writ;
        }
        return 0;
    }

    /* check for a pending reset */
    if (state->reset) {
        /* don't start a new gzip member unless there is data to write */
        if (strm->avail_in == 0)
            return 0;
        deflateReset(strm);
        state->reset = 0;
    }

    /* run deflate() on provided input until it produces no more output */
    ret = Z_OK;
    do {
        /* write out current buffer contents if full, or if flushing, but if
           doing Z_FINISH then don't write until we get to Z_STREAM_END */
        if (strm->avail_out == 0 || (flush != Z_NO_FLUSH &&
            (flush != Z_FINISH || ret == Z_STREAM_END))) {
            while (strm->next_out > state->x.next) {
                put = strm->next_out - state->x.next > (int)max ? max :
                      (unsigned)(strm->next_out - state->x.next);
                writ = write(state->fd, state->x.next, put);
                if (writ < 0) {
                    gz_error(state, Z_ERRNO, zstrerror());
                    return -1;
                }
                state->x.next += writ;
            }
            if (strm->avail_out == 0) {
                strm->avail_out = state->size;
                strm->next_out = state->out;
                state->x.next = state->out;
            }
        }

        /* compress */
        have = strm->avail_out;
        ret = deflate(strm, flush);
        if (ret == Z_STREAM_ERROR) {
            gz_error(state, Z_STREAM_ERROR,
                      "internal error: deflate stream corrupt");
            return -1;
        }
        have -= strm->avail_out;
    } while (have);

    /* if that completed a deflate stream, allow another to start */
    if (flush == Z_FINISH)
        state->reset = 1;

    /* all done, no errors */
    return 0;
}

/* Compress len zeros to output.  Return -1 on a write error or memory
   allocation failure by gz_comp(), or 0 on success. */
local int gz_zero(gz_statep state, z_off64_t len) {
    int first;
    unsigned n;
    z_streamp strm = &(state->strm);

    /* consume whatever's left in the input buffer */
    if (strm->avail_in && gz_comp(state, Z_NO_FLUSH) == -1)
        return -1;

    /* compress len zeros (len guaranteed > 0) */
    first = 1;
    while (len) {
        n = GT_OFF(state->size) || (z_off64_t)state->size > len ?
            (unsigned)len : state->size;
        if (first) {
            memset(state->in, 0, n);
            first = 0;
        }
        strm->avail_in = n;
        strm->next_in = state->in;
        state->x.pos += n;
        if (gz_comp(state, Z_NO_FLUSH) == -1)
            return -1;
        len -= n;
    }
    return 0;
}

/* Write len bytes from buf to file.  Return the number of bytes written.  If
   the returned value is less than len, then there was an error. */
local z_size_t gz_write(gz_statep state, voidpc buf, z_size_t len) {
    z_size_t put = len;

    /* if len is zero, avoid unnecessary operations */
    if (len == 0)
        return 0;

    /* allocate memory if this is the first time through */
    if (state->size == 0 && gz_init(state) == -1)
        return 0;

    /* check for seek request */
    if (state->seek) {
        state->seek = 0;
        if (gz_zero(state, state->skip) == -1)
            return 0;
    }

    /* for small len, copy to input buffer, otherwise compress directly */
    if (len < state->size) {
        /* copy to input buffer, compress when full */
        do {
            unsigned have, copy;

            if (state->strm.avail_in == 0)
                state->strm.next_in = state->in;
            have = (unsigned)((state->strm.next_in + state->strm.avail_in) -
                              state->in);
            copy = state->size - have;
            if (copy > len)
                copy = (unsigned)len;
            memcpy(state->in + have, buf, copy);
            state->strm.avail_in += copy;
            state->x.pos += copy;
            buf = (const char *)buf + copy;
            len -= copy;
            if (len && gz_comp(state, Z_NO_FLUSH) == -1)
                return 0;
        } while (len);
    }
    else {
        /* consume whatever's left in the input buffer */
        if (state->strm.avail_in && gz_comp(state, Z_NO_FLUSH) == -1)
            return 0;

        /* directly compress user buffer to file */
        state->strm.next_in = (z_const Bytef *)buf;
        do {
            unsigned n = (unsigned)-1;
            if (n > len)
                n = (unsigned)len;
            state->strm.avail_in = n;
            state->x.pos += n;
            if (gz_comp(state, Z_NO_FLUSH) == -1)
                return 0;
            len -= n;
        } while (len);
    }

    /* input was all buffered or compressed */
    return put;
}

/* -- see zlib.h -- */
int ZEXPORT gzwrite(gzFile file, voidpc buf, unsigned len) {
    gz_statep estado;

    /* obtener la estructura interna */
    if (file == NULL)
        return 0;
    estado = (gz_statep)file;

    /* verificar que estamos escribiendo y que no hay error */
    if (estado->mode != GZ_WRITE || estado->err != Z_OK)
        return 0;

    /* dado que se devuelve un int, asegúrese de que len encaje en uno, de lo contrario
       devolver con un error (esto evita un fallo en la interfaz) */
    if ((int)len < 0) {
        gz_error(estado, Z_DATA_ERROR, "la longitud solicitada no cabe en int");
        return 0;
    }

    /* escribir len bytes desde buf (el valor de retorno cabrá en un int) */
    return (int)gz_write(estado, buf, len);
}

/* -- see zlib.h -- */
z_size_t ZEXPORT gzfwrite(voidpc buf, z_size_t size, z_size_t nitems,
                          gzFile file) {
    z_size_t len;
    gz_statep estado;

    /* obtener estructura interna */
    if (file == NULL)
        return 0;
    estado = (gz_statep)file;

    /* verifique que estamos escribiendo y que no hay error */
    if (estado->mode != GZ_WRITE || estado->err != Z_OK)
        return 0;

    /* calcular bytes a leer -- error en desbordamiento */
    len = nitems * size;
    if (size && len / size != nitems) {
        gz_error(estado, Z_STREAM_ERROR, "la solicitud no cabe en un size_t");
        return 0;
    }

    /* escriba len bytes a buf, devuelva el número de elementos completos escritos */
    return len ? gz_write(estado, buf, len) / size : 0;
}

/* -- see zlib.h -- */
int ZEXPORT gzputc(gzFile file, int c) {
    unsigned tengo;
    unsigned char buf[1];
    gz_statep estado;
    z_streamp corriente;

    /* obtener estructura interna */
    if (file == NULL)
        return -1;
    estado = (gz_statep)file;
    corriente = &(estado->strm);

    /* verificar que estamos escribiendo y que no hay error */
    if (estado->mode != GZ_WRITE || estado->err != Z_OK)
        return -1;

    /* verificar solicitud de búsqueda */
    if (estado->seek) {
        estado->seek = 0;
        if (gz_zero(estado, estado->skip) == -1)
            return -1;
    }

    /* intentar escribir en el buffer de entrada para velocidad (estado->tamaño == 0 si el buffer no está inicializado) */
    if (estado->size) {
        if (corriente->avail_in == 0)
            corriente->next_in = estado->in;
        tengo = (unsigned)((corriente->next_in + corriente->avail_in) - estado->in);
        if (tengo < estado->size) {
            estado->in[tengo] = (unsigned char)c;
            corriente->avail_in++;
            estado->x.pos++;
            return c & 0xff;
        }
    }

    /* no hay espacio en el buffer o no está inicializado, usar gz_write() */
    buf[0] = (unsigned char)c;
    if (gz_write(estado, buf, 1) != 1)
        return -1;
    return c & 0xff;
}

/* -- see zlib.h -- */
int ZEXPORT gzputs(gzFile file, const char *s) {
    z_size_t len, put;
    gz_statep estado;

    /* obtener estructura interna */
    if (file == NULL)
        return -1;
    estado = (gz_statep)file;

    /* verificar que estamos escribiendo y que no hay error */
    if (estado->mode != GZ_WRITE || estado->err != Z_OK)
        return -1;

    /* escribir cadena */
    len = strlen(s);
    if ((int)len < 0 || (unsigned)len != len) {
        gz_error(estado, Z_STREAM_ERROR, "la longitud de la cadena no cabe en un int");
        return -1;
    }
    put = gz_write(estado, s, len);
    return put < len ? -1 : (int)len;
}

#if defined(STDC) || defined(Z_HAVE_STDARG_H)
#include <stdarg.h>

/* -- see zlib.h -- */
int ZEXPORTVA gzvprintf(gzFile file, const char *format, va_list va) {
    int len;
    unsigned left;
    char *next;
    gz_statep estado;
    z_streamp flujo;

    if (file == NULL)
        return Z_STREAM_ERROR;
    estado = (gz_statep)file;
    flujo = &(estado->strm);

    if (estado->mode != GZ_WRITE || estado->err != Z_OK)
        return Z_STREAM_ERROR;

    if (estado->size == 0 && gz_init(estado) == -1)
        return estado->err;

    if (estado->seek) {
        estado->seek = 0;
        if (gz_zero(estado, estado->skip) == -1)
            return estado->err;
    }

    if (flujo->avail_in == 0)
        flujo->next_in = estado->in;
    next = (char *)(estado->in + (flujo->next_in - estado->in) + flujo->avail_in);
    next[estado->size - 1] = 0;
#ifdef NO_vsnprintf
#  ifdef HAS_vsprintf_void
    (void)vsprintf(next, format, va);
    for (len = 0; len < estado->size; len++)
        if (next[len] == 0) break;
#  else
    len = vsprintf(next, format, va);
#  endif
#else
#  ifdef HAS_vsnprintf_void
    (void)vsnprintf(next, estado->size, format, va);
    len = strlen(next);
#  else
    len = vsnprintf(next, estado->size, format, va);
#  endif
#endif

    if (len == 0 || (unsigned)len >= estado->size || next[estado->size - 1] != 0)
        return 0;

    flujo->avail_in += (unsigned)len;
    estado->x.pos += len;
    if (flujo->avail_in >= estado->size) {
        left = flujo->avail_in - estado->size;
        flujo->avail_in = estado->size;
        if (gz_comp(estado, Z_NO_FLUSH) == -1)
            return estado->err;
        memmove(estado->in, estado->in + estado->size, left);
        flujo->next_in = estado->in;
        flujo->avail_in = left;
    }
    return len;
}

int ZEXPORTVA gzprintf(gzFile file, const char *format, ...) {
    va_list cholitas;
    int ret;

    va_start(cholitas, format);
    ret = gzvprintf(file, format, cholitas);
    va_end(cholitas);
    return ret;
}

#else /* !STDC && !Z_HAVE_STDARG_H */

/* -- see zlib.h -- */
int ZEXPORTVA gzprintf(gzFile file, const char *format, int a1, int a2, int a3,
                       int a4, int a5, int a6, int a7, int a8, int a9, int a10,
                       int a11, int a12, int a13, int a14, int a15, int a16,
                       int a17, int a18, int a19, int a20) {
    unsigned len, left;
    char *next;
    gz_statep state;
    z_streamp strm;

    /* get internal structure */
    if (file == NULL)
        return Z_STREAM_ERROR;
    state = (gz_statep)file;
    strm = &(state->strm);

    /* check that can really pass pointer in ints */
    if (sizeof(int) != sizeof(void *))
        return Z_STREAM_ERROR;

    /* check that we're writing and that there's no error */
    if (state->mode != GZ_WRITE || state->err != Z_OK)
        return Z_STREAM_ERROR;

    /* make sure we have some buffer space */
    if (state->size == 0 && gz_init(state) == -1)
        return state->error;

    /* check for seek request */
    if (state->seek) {
        state->seek = 0;
        if (gz_zero(state, state->skip) == -1)
            return state->error;
    }

    /* do the printf() into the input buffer, put length in len -- the input
       buffer is double-sized just for this function, so there is guaranteed to
       be state->size bytes available after the current contents */
    if (strm->avail_in == 0)
        strm->next_in = state->in;
    next = (char *)(strm->next_in + strm->avail_in);
    next[state->size - 1] = 0;
#ifdef NO_snprintf
#  ifdef HAS_sprintf_void
    sprintf(next, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12,
            a13, a14, a15, a16, a17, a18, a19, a20);
    for (len = 0; len < size; len++)
        if (next[len] == 0)
            break;
#  else
    len = sprintf(next, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11,
                  a12, a13, a14, a15, a16, a17, a18, a19, a20);
#  endif
#else
#  ifdef HAS_snprintf_void
    snprintf(next, state->size, format, a1, a2, a3, a4, a5, a6, a7, a8, a9,
             a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    len = strlen(next);
#  else
    len = snprintf(next, state->size, format, a1, a2, a3, a4, a5, a6, a7, a8,
                   a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
#  endif
#endif

    /* check that printf() results fit in buffer */
    if (len == 0 || len >= state->size || next[state->size - 1] != 0)
        return 0;

    /* update buffer and position, compress first half if past that */
    strm->avail_in += len;
    state->x.pos += len;
    if (strm->avail_in >= state->size) {
        left = strm->avail_in - state->size;
        strm->avail_in = state->size;
        if (gz_comp(state, Z_NO_FLUSH) == -1)
            return state->err;
        memmove(state->in, state->in + state->size, left);
        strm->next_in = state->in;
        strm->avail_in = left;
    }
    return (int)len;
}

#endif

/* -- see zlib.h -- */
int ZEXPORT gzflush(gzFile file, int flush) {
    gz_statep estado;

    /* get internal structure */
    if (file == NULL)
        return Z_STREAM_ERROR;
    estado = (gz_statep)file;

    /* check that we're writing and that there's no error */
    if (estado->mode != GZ_WRITE || estado->err != Z_OK)
        return Z_STREAM_ERROR;

    /* check flush parameter */
    if (flush < 0 || flush > Z_FINISH)
        return Z_STREAM_ERROR;

    /* check for seek request */
    if (estado->seek) {
        estado->seek = 0;
        if (gz_zero(estado, estado->skip) == -1)
            return estado->err;
    }

    /* compress remaining data with requested flush */
    (void)gz_comp(estado, flush);
    return estado->err;
}

/* -- see zlib.h -- */
int ZEXPORT gzsetparams(gzFile file, int level, int strategy) {
    gz_statep estado;
    z_streamp flujo;

    /* obtener estructura interna */
    if (file == NULL)
        return Z_STREAM_ERROR;
    estado = (gz_statep)file;
    flujo = &(estado->strm);

    /* verificar que estamos escribiendo y que no hay error */
    if (estado->mode != GZ_WRITE || estado->err != Z_OK || estado->direct)
        return Z_STREAM_ERROR;

    /* si no se solicita cambio, entonces no hacer nada */
    if (level == estado->level && strategy == estado->strategy)
        return Z_OK;

    /* verificar si hay solicitud de búsqueda */
    if (estado->seek) {
        estado->seek = 0;
        if (gz_zero(estado, estado->skip) == -1)
            return estado->err;
    }

    /* cambiar parámetros de compresión para la entrada subsiguiente */
    if (estado->size) {
        /* vaciar entrada previa con parámetros previos antes de cambiar */
        if (flujo->avail_in && gz_comp(estado, Z_BLOCK) == -1)
            return estado->err;
        deflateParams(flujo, level, strategy);
    }
    estado->level = level;
    estado->strategy = strategy;
    return Z_OK;
}

/* -- see zlib.h -- */
int ZEXPORT gzclose_w(gzFile file) {
    int ret = Z_OK;
    gz_statep estado;

    /* obtener estructura interna */
    if (file == NULL)
        return Z_STREAM_ERROR;
    estado = (gz_statep)file;

    /* verificar que estamos escribiendo */
    if (estado->mode != GZ_WRITE)
        return Z_STREAM_ERROR;

    /* verificar solicitud de seek */
    if (estado->seek) {
        estado->seek = 0;
        if (gz_zero(estado, estado->skip) == -1)
            ret = estado->err;
    }

    /* vaciar, liberar memoria y cerrar archivo */
    if (gz_comp(estado, Z_FINISH) == -1)
        ret = estado->err;
    if (estado->size) {
        if (!estado->direct) {
            (void)deflateEnd(&(estado->strm));
            free(estado->out);
        }
        free(estado->in);
    }
    gz_error(estado, Z_OK, NULL);
    free(estado->path);
    if (close(estado->fd) == -1)
        ret = Z_ERRNO;
    free(estado);
    return ret;
}
