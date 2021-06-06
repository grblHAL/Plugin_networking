/*
------------------------------------------------------------
    SFIFO 1.3
------------------------------------------------------------
 * Simple portable lock-free FIFO
 * (c) 2000-2002, David Olofson
 *
 * Platform support:
 *  gcc / Linux / x86:      Works
 *  gcc / Linux / x86 kernel:   Works
 *  gcc / FreeBSD / x86:        Works
 *  gcc / NetBSD / x86:     Works
 *  gcc / Mac OS X / PPC:       Works
 *  gcc / Win32 / x86:      Works
 *  Borland C++ / DOS / x86RM:  Works
 *  Borland C++ / Win32 / x86PM16:  Untested
 *  ? / Various Un*ces / ?:     Untested
 *  ? / Mac OS / PPC:       Untested
 *  gcc / BeOS / x86:       Untested
 *  gcc / BeOS / PPC:       Untested
 *  ? / ? / Alpha:          Untested
 *
 * 1.2: Max buffer size halved, to avoid problems with
 *  the sign bit...
 *
 * 1.3: Critical buffer allocation bug fixed! For certain
 *  requested buffer sizes, older version would
 *  allocate a buffer of insufficient size, which
 *  would result in memory thrashing. (Amazing that
 *  I've manage to use this to the extent I have
 *  without running into this... *heh*)
 */

/*
 * Porting note:
 *  Reads and writes of a variable of this type in memory
 *  must be *atomic*! 'int' is *not* atomic on all platforms.
 *  A safe type should be used, and  sfifo should limit the
 *  maximum buffer size accordingly.
 */

/*
 * Newer version here: https://github.com/olofson/sfifo
 */

/*
 * 2021-05-22: Extracted from ftpd.c and modified by Terje Io for grblHAL networking
 */

#include "sfifo.h"

#if FTP_ENABLE

#include <string.h>

/*
 * Alloc buffer, init FIFO etc...
 */
int sfifo_init(sfifo_t *f, int size)
{
    memset(f, 0, sizeof(sfifo_t));

    if(size > SFIFO_MAX_BUFFER_SIZE)
        return -EINVAL;

    /*
     * Set sufficient power-of-2 size.
     *
     * No, there's no bug. If you need
     * room for N bytes, the buffer must
     * be at least N+1 bytes. (The fifo
     * can't tell 'empty' from 'full'
     * without unsafe index manipulations
     * otherwise.)
     */
    f->size = 1;
    for(; f->size <= size; f->size <<= 1);

    /* Get buffer */
    f->buffer = (void *)malloc(f->size + 1);

    return f->buffer ? 0 : -ENOMEM;
}

/*
 * Dealloc buffer etc...
 */
void sfifo_close(sfifo_t *f)
{
    if(f->buffer)
        free(f->buffer);
}

/*
 * Write bytes to a FIFO
 * Return number of bytes written, or an error code
 */
int sfifo_write(sfifo_t *f, const void *_buf, int len)
{
    int total;
    int i;
    const char *buf = (const char *)_buf;

    if(!f->buffer)
        return -ENODEV; /* No buffer! */

    /* total = len = min(space, len) */
    total = sfifo_space(f);
    if(len > total)
        len = total;
    else
        total = len;

    i = f->writepos;
    if(i + len > f->size)
    {
        memcpy(f->buffer + i, buf, f->size - i);
        buf += f->size - i;
        len -= f->size - i;
        i = 0;
    }
    memcpy(f->buffer + i, buf, len);
    f->writepos = i + len;

    return total;
}

#endif
