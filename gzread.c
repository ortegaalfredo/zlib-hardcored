/* gzread.c -- zlib functions for reading gzip files
 * Copyright (C) 2004-2017 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "gzguts.h"

/* Use read() to load a buffer -- return -1 on error, otherwise 0.  Read from
   state->fd, and update state->eof, state->err, and state->msg as appropriate.
   This function needs to loop on read(), since read() is not guaranteed to
   read the number of bytes requested, depending on the type of descriptor. */
local int gz_load(gz_statep state, unsigned char *buf, unsigned len,
                  unsigned *have) {
    int ret;
    unsigned get, max = ((unsigned)-1 >> 2) + 1;

    *have = 0;
    do {
        get = len - *have;
        if (get > max)
            get = max;
        ret = read(state->fd, buf + *have, get);
        if (ret <= 0)
            break;
        *have += (unsigned)ret;
    } while (*have < len);
    if (ret < 0) {
        gz_error(state, Z_ERRNO, zstrerror());
        return -1;
    }
    if (ret == 0)
        state->eof = 1;
    return 0;
}

/* Load up input buffer and set eof flag if last data loaded -- return -1 on
   error, 0 otherwise.  Note that the eof flag is set when the end of the input
   file is reached, even though there may be unused data in the buffer.  Once
   that data has been used, no more attempts will be made to read the file.
   If strm->avail_in != 0, then the current data is moved to the beginning of
   the input buffer, and then the remainder of the buffer is loaded with the
   available data from the input file. */
local int gz_avail(gz_statep state) {
    unsigned got;
    z_streamp strm = &(state->strm);

    if (state->err != Z_OK && state->err != Z_BUF_ERROR)
        return -1;
    if (state->eof == 0) {
        if (strm->avail_in) {       /* copy what's there to the start */
            unsigned char *p = state->in;
            unsigned const char *q = strm->next_in;
            unsigned n = strm->avail_in;
            do {
                *p++ = *q++;
            } while (--n);
        }
        if (gz_load(state, state->in + strm->avail_in,
                    state->size - strm->avail_in, &got) == -1)
            return -1;
        strm->avail_in += got;
        strm->next_in = state->in;
    }
    return 0;
}

/* Look for gzip header, set up for inflate or copy.  state->x.have must be 0.
   If this is the first time in, allocate required memory.  state->how will be
   left unchanged if there is no more input data available, will be set to COPY
   if there is no gzip header and direct copying will be performed, or it will
   be set to GZIP for decompression.  If direct copying, then leftover input
   data from the input buffer will be copied to the output buffer.  In that
   case, all further file reads will be directly to either the output buffer or
   a user buffer.  If decompressing, the inflate state will be initialized.
   gz_look() will return 0 on success or -1 on failure. */
local int gz_look(gz_statep state) {
    z_streamp strm = &(state->strm);

    /* allocate read buffers and inflate memory */
    if (state->size == 0) {
        /* allocate buffers */
        state->in = (unsigned char *)malloc(state->want);
        state->out = (unsigned char *)malloc(state->want << 1);
        if (state->in == NULL || state->out == NULL) {
            free(state->out);
            free(state->in);
            gz_error(state, Z_MEM_ERROR, "out of memory");
            return -1;
        }
        state->size = state->want;

        /* allocate inflate memory */
        state->strm.zalloc = Z_NULL;
        state->strm.zfree = Z_NULL;
        state->strm.opaque = Z_NULL;
        state->strm.avail_in = 0;
        state->strm.next_in = Z_NULL;
        if (inflateInit2(&(state->strm), 15 + 16) != Z_OK) {    /* gunzip */
            free(state->out);
            free(state->in);
            state->size = 0;
            gz_error(state, Z_MEM_ERROR, "out of memory");
            return -1;
        }
    }

    /* get at least the magic bytes in the input buffer */
    if (strm->avail_in < 2) {
        if (gz_avail(state) == -1)
            return -1;
        if (strm->avail_in == 0)
            return 0;
    }

    /* look for gzip magic bytes -- if there, do gzip decoding (note: there is
       a logical dilemma here when considering the case of a partially written
       gzip file, to wit, if a single 31 byte is written, then we cannot tell
       whether this is a single-byte file, or just a partially written gzip
       file -- for here we assume that if a gzip file is being written, then
       the header will be written in a single operation, so that reading a
       single byte is sufficient indication that it is not a gzip file) */
    if (strm->avail_in > 1 &&
            strm->next_in[0] == 31 && strm->next_in[1] == 139) {
        inflateReset(strm);
        state->how = GZIP;
        state->direct = 0;
        return 0;
    }

    /* no gzip header -- if we were decoding gzip before, then this is trailing
       garbage.  Ignore the trailing garbage and finish. */
    if (state->direct == 0) {
        strm->avail_in = 0;
        state->eof = 1;
        state->x.have = 0;
        return 0;
    }

    /* doing raw i/o, copy any leftover input to output -- this assumes that
       the output buffer is larger than the input buffer, which also assures
       space for gzungetc() */
    state->x.next = state->out;
    memcpy(state->x.next, strm->next_in, strm->avail_in);
    state->x.have = strm->avail_in;
    strm->avail_in = 0;
    state->how = COPY;
    state->direct = 1;
    return 0;
}

/* Decompress from input to the provided next_out and avail_out in the state.
   On return, state->x.have and state->x.next point to the just decompressed
   data.  If the gzip stream completes, state->how is reset to LOOK to look for
   the next gzip stream or raw data, once state->x.have is depleted.  Returns 0
   on success, -1 on failure. */
local int gz_decomp(gz_statep state) {
    int ret = Z_OK;
    unsigned had;
    z_streamp strm = &(state->strm);

    /* llenar el búfer de salida hasta el final del flujo deflate */
    had = strm->avail_out;
    do {
        /* obtener más entrada para inflate() */
        if (strm->avail_in == 0 && gz_avail(state) == -1)
            return -1;
        if (strm->avail_in == 0) {
            gz_error(state, Z_BUF_ERROR, "fin de archivo inesperado");
            break;
        }

        /* descomprimir y manejar errores */
        ret = inflate(strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_NEED_DICT) {
            gz_error(state, Z_STREAM_ERROR,
                     "error interno: flujo de inflación corrupto");
            return -1;
        }
        if (ret == Z_MEM_ERROR) {
            gz_error(state, Z_MEM_ERROR, "sin memoria");
            return -1;
        }
        if (ret == Z_DATA_ERROR) {              /* flujo deflate inválido */
            gz_error(state, Z_DATA_ERROR,
                     strm->msg == NULL ? "error de datos comprimidos" : strm->msg);
            return -1;
        }
    } while (strm->avail_out && ret != Z_STREAM_END);

    /* actualizar salida disponible */
    state->x.have = had - strm->avail_out;
    state->x.next = strm->next_out - state->x.have;

    /* si el flujo gzip se completó con éxito, buscar otro */
    if (ret == Z_STREAM_END)
        state->how = LOOK;

    /* buena descompresión */
    return 0;
}

/* Fetch data and put it in the output buffer.  Assumes state->x.have is 0.
   Data is either copied from the input file or decompressed from the input
   file depending on state->how.  If state->how is LOOK, then a gzip header is
   looked for to determine whether to copy or decompress.  Returns -1 on error,
   otherwise 0.  gz_fetch() will leave state->how as COPY or GZIP unless the
   end of the input file has been reached and all data has been processed.  */
local int gz_fetch(gz_statep state) {
    z_streamp strm = &(state->strm);

    do {
        switch(state->how) {
        case LOOK:      /* -> LOOK, COPY (only if never GZIP), or GZIP */
            if (gz_look(state) == -1)
                return -1;
            if (state->how == LOOK)
                return 0;
            break;
        case COPY:      /* -> COPY */
            if (gz_load(state, state->out, state->size << 1, &(state->x.have))
                    == -1)
                return -1;
            state->x.next = state->out;
            return 0;
        case GZIP:      /* -> GZIP or LOOK (if end of gzip stream) */
            strm->avail_out = state->size << 1;
            strm->next_out = state->out;
            if (gz_decomp(state) == -1)
                return -1;
        }
    } while (state->x.have == 0 && (!state->eof || strm->avail_in));
    return 0;
}

/* Skip len uncompressed bytes of output.  Return -1 on error, 0 on success. */
local int gz_skip(gz_statep state, z_off64_t len) {
    unsigned n;

    /* skip over len bytes or reach end-of-file, whichever comes first */
    while (len)
        /* skip over whatever is in output buffer */
        if (state->x.have) {
            n = GT_OFF(state->x.have) || (z_off64_t)state->x.have > len ?
                (unsigned)len : state->x.have;
            state->x.have -= n;
            state->x.next += n;
            state->x.pos += n;
            len -= n;
        }

        /* output buffer empty -- return if we're at the end of the input */
        else if (state->eof && state->strm.avail_in == 0)
            break;

        /* need more data to skip -- load up output buffer */
        else {
            /* get more output, looking for header if required */
            if (gz_fetch(state) == -1)
                return -1;
        }
    return 0;
}

/* Read len bytes into buf from file, or less than len up to the end of the
   input.  Return the number of bytes read.  If zero is returned, either the
   end of file was reached, or there was an error.  state->err must be
   consulted in that case to determine which. */
local z_size_t gz_read(gz_statep state, voidp buf, z_size_t len) {
    z_size_t got;
    unsigned n;

    /* si len es cero, evitar operaciones innecesarias */
    if (len == 0)
        return 0;

    /* procesar una solicitud de salto */
    if (state->seek) {
        state->seek = 0;
        if (gz_skip(state, state->skip) == -1)
            return 0;
    }

    /* obtener len bytes en buf, o menos de len si estamos al final */
    got = 0;
    do {
        /* establecer n al máximo de len que cabe en un unsigned int */
        n = (unsigned)-1;
        if (n > len)
            n = (unsigned)len;

        /* primero intentar copiar datos del buffer de salida */
        if (state->x.have) {
            if (state->x.have < n)
                n = state->x.have;
            memcpy(buf, state->x.next, n);
            state->x.next += n;
            state->x.have -= n;
        }

        /* buffer de salida vacío -- regresar si estamos al final de la entrada */
        else if (state->eof && state->strm.avail_in == 0) {
            state->past = 1;        /* intentó leer más allá del final */
            break;
        }

        /* necesita datos de salida -- para len pequeño o nueva carga de stream en nuestro buffer de salida */
        else if (state->how == LOOK || n < (state->size << 1)) {
            /* obtener más salida, buscando cabecera si es necesario */
            if (gz_fetch(state) == -1)
                return 0;
            continue;       /* sin progreso aún -- regresar a copiar arriba */
            /* la copia anterior asegura que saldremos con espacio en el
               buffer de salida, permitiendo que al menos un gzungetc() tenga éxito */
        }

        /* len grande -- leer directamente en el buffer del usuario */
        else if (state->how == COPY) {      /* leer directamente */
            if (gz_load(state, (unsigned char *)buf, n, &n) == -1)
                return 0;
        }

        /* len grande -- descomprimir directamente en el buffer del usuario */
        else {  /* state->how == GZIP */
            state->strm.avail_out = n;
            state->strm.next_out = (unsigned char *)buf;
            if (gz_decomp(state) == -1)
                return 0;
            n = state->x.have;
            state->x.have = 0;
        }

        /* actualizar progreso */
        len -= n;
        buf = (char *)buf + n;
        got += n;
        state->x.pos += n;
    } while (len);

    /* devolver el número de bytes leídos en el buffer del usuario */
    return got;
}

/* -- see zlib.h -- */
int ZEXPORT gzread(gzFile file, voidp buf, unsigned len) {
    gz_statep compadre;

    /* obtener estructura interna */
    if (file == NULL)
        return -1;
    compadre = (gz_statep)file;

    /* verificar que estamos leyendo y que no hay error (serio) */
    if (compadre->mode != GZ_READ ||
            (compadre->err != Z_OK && compadre->err != Z_BUF_ERROR))
        return -1;

    /* dado que se retorna un int, asegurarse que len cabe en uno, de lo contrario
       devolver con un error (esto evita un fallo en la interfaz) */
    if ((int)len < 0) {
        gz_error(compadre, Z_STREAM_ERROR, "la solicitud no cabe en un int");
        return -1;
    }

    /* leer len o menos bytes en buf */
    len = (unsigned)gz_read(compadre, buf, len);

    /* verificar si hay un error */
    if (len == 0 && compadre->err != Z_OK && compadre->err != Z_BUF_ERROR)
        return -1;

    /* devolver el número de bytes leídos (esto está asegurado a caber en un int) */
    return (int)len;
}

/* -- see zlib.h -- */
z_size_t ZEXPORT gzfread(voidp buf, z_size_t size, z_size_t nitems,
                         gzFile file) {
    z_size_t len;
    gz_statep state;

    /* get internal structure */
    if (file == NULL)
        return 0;
    state = (gz_statep)file;

    /* check that we're reading and that there's no (serious) error */
    if (state->mode != GZ_READ ||
            (state->err != Z_OK && state->err != Z_BUF_ERROR))
        return 0;

    /* compute bytes to read -- error on overflow */
    len = nitems * size;
    if (size && len / size != nitems) {
        gz_error(state, Z_STREAM_ERROR, "request does not fit in a size_t");
        return 0;
    }

    /* read len or fewer bytes to buf, return the number of full items read */
    return len ? gz_read(state, buf, len) / size : 0;
}

/* -- see zlib.h -- */
#ifdef Z_PREFIX_SET
#  undef z_gzgetc
#else
#  undef gzgetc
#endif
int ZEXPORT gzgetc(gzFile file) {
    unsigned char buf[1];
    gz_statep state;

    /* get internal structure */
    if (file == NULL)
        return -1;
    state = (gz_statep)file;

    /* check that we're reading and that there's no (serious) error */
    if (state->mode != GZ_READ ||
        (state->err != Z_OK && state->err != Z_BUF_ERROR))
        return -1;

    /* try output buffer (no need to check for skip request) */
    if (state->x.have) {
        state->x.have--;
        state->x.pos++;
        return *(state->x.next)++;
    }

    /* nothing there -- try gz_read() */
    return gz_read(state, buf, 1) < 1 ? -1 : buf[0];
}

int ZEXPORT gzgetc_(gzFile file) {
    return gzgetc(file);
}

/* -- see zlib.h -- */
int ZEXPORT gzungetc(int c, gzFile file) {
    gz_statep estado;

    /* obten estructura interna */
    if (file == NULL)
        return -1;
    estado = (gz_statep)file;

    /* en caso este fue apenas abierto, preparar el buffer de entrada */
    if (estado->mode == GZ_READ && estado->how == LOOK && estado->x.have == 0)
        (void)gz_look(estado);

    /* verificar que estamos leyendo y que no hay un error (grave) */
    if (estado->mode != GZ_READ ||
        (estado->err != Z_OK && estado->err != Z_BUF_ERROR))
        return -1;

    /* procesar una solicitud de salto */
    if (estado->seek) {
        estado->seek = 0;
        if (gz_skip(estado, estado->skip) == -1)
            return -1;
    }

    /* no se puede insertar EOF */
    if (c < 0)
        return -1;

    /* si el buffer de salida está vacío, poner byte al final (permite más inserciones) */
    if (estado->x.have == 0) {
        estado->x.have = 1;
        estado->x.next = estado->out + (estado->size << 1) - 1;
        estado->x.next[0] = (unsigned char)c;
        estado->x.pos--;
        estado->past = 0;
        return c;
    }

    /* si no hay espacio, rendirse (debe haber hecho ya un gzungetc()) */
    if (estado->x.have == (estado->size << 1)) {
        gz_error(estado, Z_DATA_ERROR, "sin espacio para insertar caracteres");
        return -1;
    }

    /* deslizar datos de salida si es necesario e insertar byte antes de datos existentes */
    if (estado->x.next == estado->out) {
        unsigned char *src = estado->out + estado->x.have;
        unsigned char *dest = estado->out + (estado->size << 1);
        while (src > estado->out)
            *--dest = *--src;
        estado->x.next = dest;
    }
    estado->x.have++;
    estado->x.next--;
    estado->x.next[0] = (unsigned char)c;
    estado->x.pos--;
    estado->past = 0;
    return c;
}

/* -- see zlib.h -- */
char * ZEXPORT gzgets(gzFile file, char *buf, int len) {
    unsigned izquierda, n;
    char *cadena;
    unsigned char *finlinea;
    gz_statep estado;

    if (file == NULL || buf == NULL || len < 1)
        return NULL;
    estado = (gz_statep)file;

    if (estado->mode != GZ_READ ||
        (estado->err != Z_OK && estado->err != Z_BUF_ERROR))
        return NULL;

    if (estado->seek) {
        estado->seek = 0;
        if (gz_skip(estado, estado->skip) == -1)
            return NULL;
    }

    cadena = buf;
    izquierda = (unsigned)len - 1;
    if (izquierda) do {
        if (estado->x.have == 0 && gz_fetch(estado) == -1)
            return NULL;
        if (estado->x.have == 0) {
            estado->past = 1;
            break;
        }

        n = estado->x.have > izquierda ? izquierda : estado->x.have;
        finlinea = (unsigned char *)memchr(estado->x.next, '\n', n);
        if (finlinea != NULL)
            n = (unsigned)(finlinea - estado->x.next) + 1;

        memcpy(buf, estado->x.next, n);
        estado->x.have -= n;
        estado->x.next += n;
        estado->x.pos += n;
        izquierda -= n;
        buf += n;
    } while (izquierda && finlinea == NULL);

    if (buf == cadena)
        return NULL;
    buf[0] = 0;
    return cadena;
}

/* -- see zlib.h -- */
int ZEXPORT gzdirect(gzFile file) {
    gz_statep estado;

    /* obtener estructura interna */
    if (file == NULL)
        return 0;
    estado = (gz_statep)file;

    /* si el estado no es conocido, pero podemos averiguarlo, entonces hacerlo (esto es 
       principalmente para justo después de un gzopen() o gzdopen()) */
    if (estado->mode == GZ_READ && estado->how == LOOK && estado->x.have == 0)
        (void)gz_look(estado);

    /* devolver 1 si es transparente, 0 si está procesando un flujo gzip */
    return estado->direct;
}

/* -- see zlib.h -- */
int ZEXPORT gzclose_r(gzFile file) {
    int ret, err;
    gz_statep estado;

    /* obtener estructura interna */
    if (file == NULL)
        return Z_STREAM_ERROR;
    estado = (gz_statep)file;

    /* verificar que estamos leyendo */
    if (estado->mode != GZ_READ)
        return Z_STREAM_ERROR;

    /* liberar memoria y cerrar archivo */
    if (estado->size) {
        inflateEnd(&(estado->strm));
        free(estado->out);
        free(estado->in);
    }
    err = estado->err == Z_BUF_ERROR ? Z_BUF_ERROR : Z_OK;
    gz_error(estado, Z_OK, NULL);
    free(estado->path);
    ret = close(estado->fd);
    free(estado);
    return ret ? Z_ERRNO : err;
}
