#pragma once

/*
------------------------------------------------------------
    SFIFO 1.3
------------------------------------------------------------
 * Simple portable lock-free FIFO
 * (c) 2000-2002, David Olofson
 *
 */

#if defined(ARDUINO)
#include "../driver.h"
#else
#include "driver.h"
#endif

#if FTP_ENABLE

#include "lwip/errno.h"

#ifndef EINVAL
#define EINVAL 1
#define ENOMEM 2
#define ENODEV 3
#endif

#define SFIFO_MAX_BUFFER_SIZE   0x7fffffff
#define SFIFO_SIZEMASK(x) ((x)->size - 1)
#define sfifo_used(x) (((x)->writepos - (x)->readpos) & SFIFO_SIZEMASK(x))
#define sfifo_space(x) ((x)->size - 1 - sfifo_used(x))

typedef int sfifo_atomic_t;

typedef struct
{
    char *buffer;
    int size;                           /* Number of bytes */
    volatile sfifo_atomic_t readpos;    /* Read position */
    volatile sfifo_atomic_t writepos;   /* Write position */
} sfifo_t;

int sfifo_init(sfifo_t *f, int size);
int sfifo_write(sfifo_t *f, const void *_buf, int len);
void sfifo_close(sfifo_t *f);

#endif
