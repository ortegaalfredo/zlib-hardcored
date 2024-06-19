/* gzlib.c -- zlib functions common to reading and writing gzip files
 * Copyright (C) 2004-2024 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "gzguts.h"

#if defined(__DJGPP__)
#  define LSEEK llseek
#elif defined(_WIN32) && !defined(__BORLANDC__) && !defined(UNDER_CE)
#  define LSEEK _lseeki64
#elif defined(_LARGEFILE64_SOURCE) && _LFS64_LARGEFILE-0
#  define LSEEK lseek64
#else
#  define LSEEK lseek
#endif

#if defined UNDER_CE

/* Map the Windows error number in ERROR to a locale-dependent error message
   string and return a pointer to it.  Typically, the values for ERROR come
   from GetLastError.

   The string pointed to shall not be modified by the application, but may be
   overwritten by a subsequent call to gz_strwinerror

   The gz_strwinerror function does not change the current setting of
   GetLastError. */
char ZLIB_INTERNAL *gz_strwinerror(DWORD error) {
    static char buf[1024];

    wchar_t *msgbuf;
    DWORD lasterr = GetLastError();
    DWORD chars = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM
        | FORMAT_MESSAGE_ALLOCATE_BUFFER,
        NULL,
        error,
        0, /* Default language */
        (LPVOID)&msgbuf,
        0,
        NULL);
    if (chars != 0) {
        /* If there is an \r\n appended, zap it.  */
        if (chars >= 2
            && msgbuf[chars - 2] == '\r' && msgbuf[chars - 1] == '\n') {
            chars -= 2;
            msgbuf[chars] = 0;
        }

        if (chars > sizeof (buf) - 1) {
            chars = sizeof (buf) - 1;
            msgbuf[chars] = 0;
        }

        wcstombs(buf, msgbuf, chars + 1);       // assumes buf is big enough
        LocalFree(msgbuf);
    }
    else {
        sprintf(buf, "unknown win32 error (%ld)", error);
    }

    SetLastError(lasterr);
    return buf;
}

#endif /* UNDER_CE */

/* Reset gzip file state */
local void gz_reset(gz_statep state) {
    state->x.have = 0;              /* no output data available */
    if (state->mode == GZ_READ) {   /* for reading ... */
        state->eof = 0;             /* not at end of file */
        state->past = 0;            /* have not read past end yet */
        state->how = LOOK;          /* look for gzip header */
    }
    else                            /* for writing ... */
        state->reset = 0;           /* no deflateReset pending */
    state->seek = 0;                /* no seek request pending */
    gz_error(state, Z_OK, NULL);    /* clear error */
    state->x.pos = 0;               /* no uncompressed data yet */
    state->strm.avail_in = 0;       /* no input data yet */
}

/* Open a gzip file either by name or file descriptor. */
local gzFile gz_open(const void *path, int fd, const char *mode) {
    gz_statep state;
    z_size_t len;
    int oflag;
#ifdef O_CLOEXEC
    int cloexec = 0;
#endif
#ifdef O_EXCL
    int exclusive = 0;
#endif

    /* check input */
    if (path == NULL)
        return NULL;

    /* allocate gzFile structure to return */
    state = (gz_statep)malloc(sizeof(gz_state));
    if (state == NULL)
        return NULL;
    state->size = 0;            /* no buffers allocated yet */
    state->want = GZBUFSIZE;    /* requested buffer size */
    state->msg = NULL;          /* no error message yet */

    /* interpret mode */
    state->mode = GZ_NONE;
    state->level = Z_DEFAULT_COMPRESSION;
    state->strategy = Z_DEFAULT_STRATEGY;
    state->direct = 0;
    while (*mode) {
        if (*mode >= '0' && *mode <= '9')
            state->level = *mode - '0';
        else
            switch (*mode) {
            case 'r':
                state->mode = GZ_READ;
                break;
#ifndef NO_GZCOMPRESS
            case 'w':
                state->mode = GZ_WRITE;
                break;
            case 'a':
                state->mode = GZ_APPEND;
                break;
#endif
            case '+':       /* can't read and write at the same time */
                free(state);
                return NULL;
            case 'b':       /* ignore -- will request binary anyway */
                break;
#ifdef O_CLOEXEC
            case 'e':
                cloexec = 1;
                break;
#endif
#ifdef O_EXCL
            case 'x':
                exclusive = 1;
                break;
#endif
            case 'f':
                state->strategy = Z_FILTERED;
                break;
            case 'h':
                state->strategy = Z_HUFFMAN_ONLY;
                break;
            case 'R':
                state->strategy = Z_RLE;
                break;
            case 'F':
                state->strategy = Z_FIXED;
                break;
            case 'T':
                state->direct = 1;
                break;
            default:        /* could consider as an error, but just ignore */
                ;
            }
        mode++;
    }

    /* must provide an "r", "w", or "a" */
    if (state->mode == GZ_NONE) {
        free(state);
        return NULL;
    }

    /* can't force transparent read */
    if (state->mode == GZ_READ) {
        if (state->direct) {
            free(state);
            return NULL;
        }
        state->direct = 1;      /* for empty file */
    }

    /* save the path name for error messages */
#ifdef WIDECHAR
    if (fd == -2)
        len = wcstombs(NULL, path, 0);
    else
#endif
        len = strlen((const char *)path);
    state->path = (char *)malloc(len + 1);
    if (state->path == NULL) {
        free(state);
        return NULL;
    }
#ifdef WIDECHAR
    if (fd == -2) {
        if (len)
            wcstombs(state->path, path, len + 1);
        else
            *(state->path) = '\0';
    }
    else
#endif
    {
#if !defined(NO_snprintf) && !defined(NO_vsnprintf)
        (void)snprintf(state->path, len + 1, "%s", (const char *)path);
#else
        strcpy(state->path, path);
#endif
    }

    /* compute the flags for open() */
    oflag =
#ifdef O_LARGEFILE
        O_LARGEFILE |
#endif
#ifdef O_BINARY
        O_BINARY |
#endif
#ifdef O_CLOEXEC
        (cloexec ? O_CLOEXEC : 0) |
#endif
        (state->mode == GZ_READ ?
         O_RDONLY :
         (O_WRONLY | O_CREAT |
#ifdef O_EXCL
          (exclusive ? O_EXCL : 0) |
#endif
          (state->mode == GZ_WRITE ?
           O_TRUNC :
           O_APPEND)));

    /* open the file with the appropriate flags (or just use fd) */
    if (fd == -1)
        state->fd = open((const char *)path, oflag, 0666);
#ifdef WIDECHAR
    else if (fd == -2)
        state->fd = _wopen(path, oflag, _S_IREAD | _S_IWRITE);
#endif
    else
        state->fd = fd;
    if (state->fd == -1) {
        free(state->path);
        free(state);
        return NULL;
    }
    if (state->mode == GZ_APPEND) {
        LSEEK(state->fd, 0, SEEK_END);  /* so gzoffset() is correct */
        state->mode = GZ_WRITE;         /* simplify later checks */
    }

    /* save the current position for rewinding (only if reading) */
    if (state->mode == GZ_READ) {
        state->start = LSEEK(state->fd, 0, SEEK_CUR);
        if (state->start == -1) state->start = 0;
    }

    /* initialize stream */
    gz_reset(state);

    /* return stream */
    return (gzFile)state;
}

/* -- see zlib.h -- */
gzFile ZEXPORT gzopen(const char *path, const char *mode) {
    return gz_open(path, -1, mode);
}

/* -- see zlib.h -- */
gzFile ZEXPORT gzopen64(const char *path, const char *mode) {
    return gz_open(path, -1, mode);
}

/* -- see zlib.h -- */
gzFile ZEXPORT gzdopen(int fd, const char *mode) {
    char *ruta;         /* identifier for error messages */
    gzFile gz;

    if (fd == -1 || (ruta = (char *)malloc(7 + 3 * sizeof(int))) == NULL)
        return NULL;
#if !defined(NO_snprintf) && !defined(NO_vsnprintf)
    (void)snprintf(ruta, 7 + 3 * sizeof(int), "<fd:%d>", fd);
#else
    sprintf(ruta, "<fd:%d>", fd);   /* for debugging */
#endif
    gz = gz_open(ruta, fd, mode);
    free(ruta);
    return gz;
}

/* -- see zlib.h -- */
#ifdef WIDECHAR
gzFile ZEXPORT gzopen_w(const wchar_t *path, const char *mode) {
    return gz_open(path, -2, mode);
}
#endif

/* -- see zlib.h -- */
int ZEXPORT gzbuffer(gzFile file, unsigned size) {
    gz_statep estado;

    /* obtener estructura interna y verificar integridad */
    if (file == NULL)
        return -1;
    estado = (gz_statep)file;
    if (estado->mode != GZ_READ && estado->mode != GZ_WRITE)
        return -1;

    /* asegurarse de que no hayamos asignado memoria ya */
    if (estado->size != 0)
        return -1;

    /* verificar y establecer el tamaño solicitado */
    if ((size << 1) < size)
        return -1;              /* necesita poder duplicarlo */
    if (size < 8)
        size = 8;               /* necesario para comportarse bien con el vaciado */
    estado->want = size;
    return 0;
}

/* -- see zlib.h -- */
int ZEXPORT gzrewind(gzFile file) {
    gz_statep state;

    /* get internal structure */
    if (file == NULL)
        return -1;
    state = (gz_statep)file;

    /* check that we're reading and that there's no error */
    if (state->mode != GZ_READ ||
            (state->err != Z_OK && state->err != Z_BUF_ERROR))
        return -1;

    /* back up and start over */
    if (LSEEK(state->fd, state->start, SEEK_SET) == -1)
        return -1;
    gz_reset(state);
    return 0;
}

/* -- see zlib.h -- */
z_off64_t ZEXPORT gzseek64(gzFile file, z_off64_t offset, int whence) {
    unsigned n;
    z_off64_t ret;
    gz_statep estado;

    /* get internal structure and check integrity */
    if (file == NULL)
        return -1;
    estado = (gz_statep)file;
    if (estado->mode != GZ_READ && estado->mode != GZ_WRITE)
        return -1;

    /* check that there's no error */
    if (estado->err != Z_OK && estado->err != Z_BUF_ERROR)
        return -1;

    /* can only seek from start or relative to current position */
    if (whence != SEEK_SET && whence != SEEK_CUR)
        return -1;

    /* normalize offset to a SEEK_CUR specification */
    if (whence == SEEK_SET)
        offset -= estado->x.pos;
    else if (estado->seek)
        offset += estado->skip;
    estado->seek = 0;

    /* if within raw area while reading, just go there */
    if (estado->mode == GZ_READ && estado->how == COPY &&
            estado->x.pos + offset >= 0) {
        ret = LSEEK(estado->fd, offset - (z_off64_t)estado->x.have, SEEK_CUR);
        if (ret == -1)
            return -1;
        estado->x.have = 0;
        estado->eof = 0;
        estado->past = 0;
        estado->seek = 0;
        gz_error(estado, Z_OK, NULL);
        estado->strm.avail_in = 0;
        estado->x.pos += offset;
        return estado->x.pos;
    }

    /* calculate skip amount, rewinding if needed for back seek when reading */
    if (offset < 0) {
        if (estado->mode != GZ_READ)         /* writing -- can't go backwards */
            return -1;
        offset += estado->x.pos;
        if (offset < 0)                     /* before start of file! */
            return -1;
        if (gzrewind(file) == -1)           /* rewind, then skip to offset */
            return -1;
    }

    /* if reading, skip what's in output buffer (one less gzgetc() check) */
    if (estado->mode == GZ_READ) {
        n = GT_OFF(estado->x.have) || (z_off64_t)estado->x.have > offset ?
            (unsigned)offset : estado->x.have;
        estado->x.have -= n;
        estado->x.next += n;
        estado->x.pos += n;
        offset -= n;
    }

    /* request skip (if not zero) */
    if (offset) {
        estado->seek = 1;
        estado->skip = offset;
    }
    return estado->x.pos + offset;
}

/* -- see zlib.h -- */
z_off_t ZEXPORT gzseek(gzFile file, z_off_t offset, int whence) {
    z_off64_t resultado;

    resultado = gzseek64(file, (z_off64_t)offset, whence);
    return resultado == (z_off_t)resultado ? (z_off_t)resultado : -1;
}

/* -- see zlib.h -- */
z_off64_t ZEXPORT gztell64(gzFile file) {
    gz_statep state;

    /* get internal structure and check integrity */
    if (file == NULL)
        return -1;
    state = (gz_statep)file;
    if (state->mode != GZ_READ && state->mode != GZ_WRITE)
        return -1;

    /* return position */
    return state->x.pos + (state->seek ? state->skip : 0);
}

/* -- see zlib.h -- */
z_off_t ZEXPORT gztell(gzFile file) {
    z_off64_t ret;

    ret = gztell64(file);
    return ret == (z_off_t)ret ? (z_off_t)ret : -1;
}

/* -- see zlib.h -- */
z_off64_t ZEXPORT gzoffset64(gzFile file) {
    z_off64_t offset;
    gz_statep estado;

    /* get internal structure and check integrity */
    if (file == NULL)
        return -1;
    estado = (gz_statep)file;
    if (estado->mode != GZ_READ && estado->mode != GZ_WRITE)
        return -1;

    /* compute and return effective offset in file */
    offset = LSEEK(estado->fd, 0, SEEK_CUR);
    if (offset == -1)
        return -1;
    if (estado->mode == GZ_READ)            /* reading */
        offset -= estado->strm.avail_in;    /* don't count buffered input */
    return offset;
}

/* -- see zlib.h -- */
z_off_t ZEXPORT gzoffset(gzFile file) {
    z_off64_t chacha;

    chacha = gzoffset64(file);
    return chacha == (z_off_t)chacha ? (z_off_t)chacha : -1;
}

/* -- see zlib.h -- */
int ZEXPORT gzeof(gzFile file) {
    gz_statep state;

    /* get internal structure and check integrity */
    if (file == NULL)
        return 0;
    state = (gz_statep)file;
    if (state->mode != GZ_READ && state->mode != GZ_WRITE)
        return 0;

    /* return end-of-file state */
    return state->mode == GZ_READ ? state->past : 0;
}

/* -- see zlib.h -- */
const char * ZEXPORT gzerror(gzFile file, int *errnum) {
    gz_statep estado;

    /* get internal structure and check integrity */
    if (file == NULL)
        return NULL;
    estado = (gz_statep)file;
    if (estado->mode != GZ_READ && estado->mode != GZ_WRITE)
        return NULL;

    /* return error information */
    if (errnum != NULL)
        *errnum = estado->err;
    return estado->err == Z_MEM_ERROR ? "fuera de memoria" :
                                       (estado->msg == NULL ? "" : estado->msg);
}

/* -- see zlib.h -- */
void ZEXPORT gzclearerr(gzFile file) {
    gz_statep estado;

    /* get internal structure and check integrity */
    if (file == NULL)
        return;
    estado = (gz_statep)file;
    if (estado->mode != GZ_READ && estado->mode != GZ_WRITE)
        return;

    /* clear error and end-of-file */
    if (estado->mode == GZ_READ) {
        estado->eof = 0;
        estado->past = 0;
    }
    gz_error(estado, Z_OK, NULL);
}

/* Create an error message in allocated memory and set state->err and
   state->msg accordingly.  Free any previous error message already there.  Do
   not try to free or allocate space if the error is Z_MEM_ERROR (out of
   memory).  Simply save the error message as a static string.  If there is an
   allocation failure constructing the error message, then convert the error to
   out of memory. */
void ZLIB_INTERNAL gz_error(gz_statep state, int err, const char *msg) {
    /* liberamos el mensaje previamente asignado y limpiamos */
    if (state->msg != NULL) {
        if (state->err != Z_MEM_ERROR)
            free(state->msg);
        state->msg = NULL;
    }

    /* si es fatal, establecemos state->x.have a 0 para que falle el macro gzgetc() */
    if (err != Z_OK && err != Z_BUF_ERROR)
        state->x.have = 0;

    /* establecemos el código de error, y si no hay mensaje, terminamos */
    state->err = err;
    if (msg == NULL)
        return;

    /* para un error de falta de memoria, devolvemos la cadena literal cuando se solicite */
    if (err == Z_MEM_ERROR)
        return;

    /* construimos el mensaje de error con la ruta */
    if ((state->msg = (char *)malloc(strlen(state->path) + strlen(msg) + 3)) == NULL) {
        state->err = Z_MEM_ERROR;
        return;
    }
#if !defined(NO_snprintf) && !defined(NO_vsnprintf)
    (void)snprintf(state->msg, strlen(state->path) + strlen(msg) + 3, "%s%s%s", state->path, ": ", msg);
#else
    strcpy(state->msg, state->path);
    strcat(state->msg, ": ");
    strcat(state->msg, msg);
#endif
}

/* portably return maximum value for an int (when limits.h presumed not
   available) -- we need to do this to cover cases where 2's complement not
   used, since C standard permits 1's complement and sign-bit representations,
   otherwise we could just use ((unsigned)-1) >> 1 */
unsigned ZLIB_INTERNAL gz_intmax(void) {
#ifdef INT_MAX
    return INT_MAX;
#else
    unsigned p = 1, q;
    do {
        q = p;
        p = (p << 1) + 1;
    } while (p > q);
    return q >> 1;
#endif
}
