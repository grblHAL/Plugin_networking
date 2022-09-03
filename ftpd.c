/*
 * Copyright (c) 2002 Florian Schulze.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the authors nor the names of the contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * ftpd.c - This file is part of the FTP daemon for lwIP
 *
 */

/*
 * 2021-12-28: Modified by Terje Io for grblHAL networking
 * 2022-08-25: Modified by Terje Io for grblHAL VFS
 */

/*
 * Based on this version: https://github.com/toelke/lwip-ftpd
 */

#ifdef ARDUINO
#include "../driver.h"
#include "../grbl/vfs.h"
#else
#include "driver.h"
#include "grbl/vfs.h"
#endif

#if FTP_ENABLE

#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"

#include "ftpd.h"
#include "sfifo.h"

#include "../sdcard/sdcard.h"

#ifndef FTPD_POLL_INTERVAL
#define FTPD_POLL_INTERVAL 4
#endif

#ifdef LWIP_DEBUGF
#undef LWIP_DEBUGF
#endif

#define FTPD_DEBUG 0

#if FTPD_DEBUG

#define LWIP_DEBUGF(debug, message) do { dbg message; } while(0)

void dbg (char *msg, ...)
{
    char buffer[200];
    va_list arg;
    va_start(arg, msg);
    vsprintf(buffer, msg, arg);
    va_end(arg);
    hal.stream.write(buffer);
}

#else
#define LWIP_DEBUGF(debug, message)
#endif

#define TELNET_IAC 255

#ifndef FTP_TXPOLL
#define FTP_TXPOLL 0
#endif

#ifndef EINVAL
#define EINVAL 1
#define ENOMEM 2
#define ENODEV 3
#endif

//PROGMEM static const char *msg110 = "110 MARK %s = %s.";
/*
         110 Restart marker reply.
             In this case, the text is exact and not left to the
             particular implementation; it must read:
                  MARK yyyy = mmmm
             Where yyyy is User-process data stream marker, and mmmm
             server's equivalent marker (note the spaces between markers
             and "=").
*/
//PROGMEM static const char *msg120 = "120 Service ready in nnn minutes.";
//PROGMEM static const char *msg125 = "125 Data connection already open; transfer starting.";
PROGMEM static const char *msg150 = "150 File status okay; about to open data connection.";
PROGMEM static const char *msg150recv = "150 Opening BINARY mode data connection for %s (%i bytes).";
PROGMEM static const char *msg150stor = "150 Opening BINARY mode data connection for %s.";
PROGMEM static const char *msg200 = "200 Command okay.";
//PROGMEM static const char *msg202 = "202 Command not implemented, superfluous at this site.";
//PROGMEM static const char *msg211 = "211 System status, or system help reply.";
//PROGMEM static const char *msg212 = "212 Directory status.";
//PROGMEM static const char *msg213 = "213 File status.";
//PROGMEM static const char *msg214 = "214 %s.";
/*
             214 Help message.
             On how to use the server or the meaning of a particular
             non-standard command.  This reply is useful only to the
             human user.
*/
PROGMEM static const char *msg214SYST = "214 %s system type.";
/*
         215 NAME system type.
             Where NAME is an official system name from the list in the
             Assigned Numbers document.
*/
PROGMEM static const char *msg220 = "220 lwIP FTP Server ready.";
/*
         220 Service ready for new user.
*/
PROGMEM static const char *msg221 = "221 Goodbye.";
/*
         221 Service closing control connection.
             Logged out if appropriate.
*/
//PROGMEM static const char *msg225 = "225 Data connection open; no transfer in progress.";
PROGMEM static const char *msg226 = "226 Closing data connection.";
/*
             Requested file action successful (for example, file
             transfer or file abort).
*/
PROGMEM static const char *msg227 = "227 Entering Passive Mode (%i,%i,%i,%i,%i,%i).";
/*
         227 Entering Passive Mode (h1,h2,h3,h4,p1,p2).
*/
PROGMEM static const char *msg230 = "230 User logged in, proceed.";
PROGMEM static const char *msg250 = "250 Requested file action okay, completed.";
PROGMEM static const char *msg257PWD = "257 \"%s\" is current directory.";
PROGMEM static const char *msg257 = "257 \"%s\" created.";
/*
         257 "PATHNAME" created.
*/
PROGMEM static const char *msg331 = "331 User name okay, need password.";
//PROGMEM static const char *msg332 = "332 Need account for login.";
PROGMEM static const char *msg350 = "350 Requested file action pending further information.";
//PROGMEM static const char *msg421 = "421 Service not available, closing control connection.";
/*
             This may be a reply to any command if the service knows it
             must shut down.
*/
//PROGMEM static const char *msg425 = "425 Can't open data connection.";
//PROGMEM static const char *msg426 = "426 Connection closed; transfer aborted.";
PROGMEM static const char *msg450 = "450 Requested file action not taken.";
/*
             File unavailable (e.g., file busy).
*/
PROGMEM static const char *msg451 = "451 Requested action aborted: local error in processing.";
PROGMEM static const char *msg452 = "452 Requested action not taken.";
/*
             Insufficient storage space in system.
*/
//PROGMEM static const char *msg500 = "500 Syntax error, command unrecognized.";
/*
             This may include errors such as command line too long.
*/
PROGMEM static const char *msg501 = "501 Syntax error in parameters or arguments.";
PROGMEM static const char *msg502 = "502 Command not implemented.";
PROGMEM static const char *msg503 = "503 Bad sequence of commands.";
//PROGMEM static const char *msg504 = "504 Command not implemented for that parameter.";
//PROGMEM static const char *msg530 = "530 Not logged in.";
//PROGMEM static const char *msg532 = "532 Need account for storing files.";
PROGMEM static const char *msg550 = "550 Requested action not taken.";
/*
             File unavailable (e.g., file not found, no access).
*/
//PROGMEM static const char *msg551 = "551 Requested action aborted: page type unknown.";
//PROGMEM static const char *msg552 = "552 Requested file action aborted.";
/*
             Exceeded storage allocation (for current directory or
             dataset).
*/
//PROGMEM static const char *msg553 = "553 Requested action not taken.";
/*
             File name not allowed.
*/

enum ftpd_state_e {
    FTPD_USER,
    FTPD_PASS,
    FTPD_IDLE,
    FTPD_NLST,
    FTPD_LIST,
    FTPD_RETR,
    FTPD_RNFR,
    FTPD_STOR,
    FTPD_QUIT
};

PROGMEM static const char *month_table[12] = {
    "Jan",
    "Feb",
    "Mar",
    "Apr",
    "May",
    "Jun",
    "Jul",
    "Aug",
    "Sep",
    "Oct",
    "Nov",
    "Dec"
};

typedef struct {
    int connected;
    int eof;
    vfs_dir_t *vfs_dir;
    vfs_dirent_t *vfs_dirent;
    vfs_file_t *vfs_file;
    sfifo_t fifo;
    struct tcp_pcb *msgpcb;
    struct ftpd_msgstate *msgfs;
} ftpd_datastate_t;

typedef struct  {
    char *text;
    int len;
} ftpd_cmd_t;

typedef struct ftpd_msgstate {
    enum ftpd_state_e state;
    sfifo_t fifo;
    vfs_t *vfs;
    struct ip4_addr dataip;
    u16_t dataport;
    struct tcp_pcb *datalistenpcb;
    struct tcp_pcb *datapcb;
    ftpd_datastate_t *datafs;
    int passive;
    char *renamefrom;
    ftpd_cmd_t cmd;
} ftpd_msgstate_t;

#if FTP_TXPOLL
static struct {
    ftpd_datastate_t *fsd;
    struct tcp_pcb *pcb;
} poll;
#endif

static void send_msg (struct tcp_pcb *pcb, ftpd_msgstate_t *fsm, const char *msg, ...);

static void ftpd_dataerr (void *arg, err_t err)
{
    ftpd_datastate_t *fsd = arg;

    LWIP_DEBUGF(FTPD_DEBUG, ("ftpd_dataerr: %s (%i)\n", lwip_strerr(err), err));
    if (fsd != NULL) {
        fsd->msgfs->datafs = NULL;
        fsd->msgfs->state = FTPD_IDLE;
        free(fsd);
    }
}

static void ftpd_dataclose (struct tcp_pcb *pcb, ftpd_datastate_t *fsd)
{
    if(fsd->vfs_file)
        vfs_close(fsd->vfs_file);

    if(fsd->vfs_dir)
        vfs_closedir(fsd->vfs_dir);

    tcp_arg(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_recv(pcb, NULL);

    if (fsd->msgfs) {
        if (fsd->msgfs->datalistenpcb) {
            tcp_arg(fsd->msgfs->datalistenpcb, NULL);
            tcp_accept(fsd->msgfs->datalistenpcb, NULL);
            tcp_close(fsd->msgfs->datalistenpcb);
            fsd->msgfs->datalistenpcb = NULL;
        }
        fsd->msgfs->datafs = NULL;
        fsd->msgfs->state = FTPD_IDLE;
    }

    sfifo_close(&fsd->fifo);
    free(fsd);

    tcp_arg(pcb, NULL);
    tcp_close(pcb);
}

static void send_data (struct tcp_pcb *pcb, ftpd_datastate_t *fsd)
{
    u16_t len;

    if ((len = sfifo_used(&fsd->fifo)) > 0) {

        int i = fsd->fifo.readpos;

        /* We cannot send more data than space available in the send buffer. */
        len = tcp_sndbuf(pcb) < len ? tcp_sndbuf(pcb) : len;

        LWIP_DEBUGF(FTPD_DEBUG, ("send_data: %d %d %d\n", len, i, *(fsd->fifo.buffer + i)));

        if ((i + len) > fsd->fifo.size) {
            if (tcp_write(pcb, fsd->fifo.buffer + i, (u16_t)(fsd->fifo.size - i), 1) != ERR_OK) {
                LWIP_DEBUGF(FTPD_DEBUG, ("send_data: error writing!\n"));
                return;
            }
            len -= fsd->fifo.size - i;
            fsd->fifo.readpos = 0;
            i = 0;
        }

        if (tcp_write(pcb, fsd->fifo.buffer + i, len, 1) != ERR_OK) {
            LWIP_DEBUGF(FTPD_DEBUG, ("send_data: error writing!\n"));
            return;
        }

        fsd->fifo.readpos += len;
        if(fsd->eof && sfifo_used(&fsd->fifo) == 0)
            tcp_output(pcb);
    }
}

static void send_file (ftpd_datastate_t *fsd, struct tcp_pcb *pcb)
{
    if (!fsd->connected)
        return;

    int len = 0;

    if (fsd->vfs_file) {

        static char buffer[512];

        LWIP_DEBUGF(FTPD_DEBUG, ("send_file: %d\n", sfifo_space(&fsd->fifo)));

        while((len = sfifo_space(&fsd->fifo)) > 256) {

            len = vfs_read(buffer, 1, len > sizeof(buffer) ? sizeof(buffer) : len, fsd->vfs_file);

            if (vfs_errno) {
                fsd->vfs_file = NULL; /* FS error */
                break;
            }

            if (len > 0)
                sfifo_write(&fsd->fifo, buffer, len);

            if ((fsd->eof = vfs_eof(fsd->vfs_file))) {
                vfs_close(fsd->vfs_file);
                fsd->vfs_file = NULL;
                break;
            }
        }

        if(len >= 0)
            send_data(pcb, fsd);
    }

    if (!fsd->vfs_file) {

        if (len >= 0 && sfifo_used(&fsd->fifo) > 0) {
            send_data(pcb, fsd);
            return;
        }

        ftpd_msgstate_t *fsm = fsd->msgfs;
        struct tcp_pcb *msgpcb = fsd->msgpcb;
        ftpd_dataclose(pcb, fsd);
        send_msg(msgpcb, fsm, len < 0 ? msg451 : msg226);
    }
}

static void send_next_directory (ftpd_datastate_t *fsd, struct tcp_pcb *pcb, int shortlist)
{
    int len;
    char buffer[512];

    while (1) {
        if (fsd->vfs_dirent == NULL)
            fsd->vfs_dirent = vfs_readdir(fsd->vfs_dir);

        if (fsd->vfs_dirent) {
            if (shortlist) {
                len = sprintf(buffer, "%s\r\n", fsd->vfs_dirent->name);
                if (sfifo_space(&fsd->fifo) < len) {
                    send_data(pcb, fsd);
                    return;
                }
                sfifo_write(&fsd->fifo, buffer, len);
                fsd->vfs_dirent = NULL;
            } else {
                vfs_stat_t st;
                time_t current_time = (time_t)-1;
                int current_year;
                struct tm *s_time;
#ifndef __IMXRT1062__
                time(&current_time);
#endif
                s_time = gmtime(&current_time);
                current_year = s_time->tm_year;

                strcat(strcpy(buffer, "/"), fsd->vfs_dirent->name);

//                vfs_stat(fsd->vfs_dirent->name, &st);
                vfs_stat(fsd->vfs_dirent->name, &st);
#ifdef ESP_PLATFORM
                s_time = gmtime(&st.st_mtim);
#else
                s_time = gmtime(&st.st_mtime);
#endif
                if (s_time->tm_year == current_year)
                    len = sprintf(buffer, "-rw-rw-rw-   1 user     ftp  %11" UINT32SFMT " %s %02i %02i:%02i %s\r\n", (uint32_t)st.st_size, month_table[s_time->tm_mon], s_time->tm_mday, s_time->tm_hour, s_time->tm_min, fsd->vfs_dirent->name);
                else
                    len = sprintf(buffer, "-rw-rw-rw-   1 user     ftp  %11" UINT32SFMT " %s %02i %5i %s\r\n", (uint32_t)st.st_size, month_table[s_time->tm_mon], s_time->tm_mday, s_time->tm_year + 1900, fsd->vfs_dirent->name);

                if (st.st_mode.directory)
                    buffer[0] = 'd';
                if (sfifo_space(&fsd->fifo) < len) {
                    send_data(pcb, fsd);
                    return;
                }
                sfifo_write(&fsd->fifo, buffer, len);
                fsd->vfs_dirent = NULL;
            }
        } else {

            if (sfifo_used(&fsd->fifo) > 0) {
                send_data(pcb, fsd);
                return;
            }

            ftpd_msgstate_t *fsm = fsd->msgfs;
            struct tcp_pcb *msgpcb = fsd->msgpcb;
            ftpd_dataclose(pcb, fsd);
            fsm->datapcb = NULL;
            send_msg(msgpcb, fsm, msg226);
            return;
        }
    }
}

static err_t ftpd_datasent (void *arg, struct tcp_pcb *pcb, u16_t len)
{
    ftpd_datastate_t *fsd = arg;

    switch (fsd->msgfs->state) {
        case FTPD_LIST:
            send_next_directory(fsd, pcb, 0);
            break;
        case FTPD_NLST:
            send_next_directory(fsd, pcb, 1);
            break;
        case FTPD_RETR:
#if FTP_TXPOLL
            poll.fsd = fsd;
            poll.pcb = pcb;
#else
            send_file(fsd, pcb);
#endif
            break;
        default:
            break;
    }

    return ERR_OK;
}

static err_t ftpd_datarecv (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    ftpd_datastate_t *fsd = arg;

    if (err == ERR_OK && p != NULL) {

        u16_t tot_len = 0;

        struct pbuf *q = p;
        do {
            int len;
            len = vfs_write(q->payload, 1, q->len, fsd->vfs_file);
            tot_len += len;
            if (len != q->len)
                break;
        } while((q = q->next));

        /* Inform TCP that we have taken the data. */
        tcp_recved(pcb, tot_len);
        pbuf_free(p);
    }

    if (err == ERR_OK && p == NULL) {
        ftpd_msgstate_t *fsm = fsd->msgfs;
        struct tcp_pcb *msgpcb = fsd->msgpcb;

        ftpd_dataclose(pcb, fsd);
        fsm->datapcb = NULL;
        send_msg(msgpcb, fsm, msg226);
    }

    return ERR_OK;
}

static err_t ftpd_dataconnected (void *arg, struct tcp_pcb *pcb, err_t err)
{
    ftpd_datastate_t *fsd = arg;

    fsd->msgfs->datapcb = pcb;
    fsd->connected = 1;

    /* Tell TCP that we wish to be informed of incoming data by a call
       to the ftpd_datarecv() function. */
    tcp_recv(pcb, ftpd_datarecv);

    /* Tell TCP that we wish be to informed of data that has been
       successfully sent by a call to the ftpd_sent() function. */
    tcp_sent(pcb, ftpd_datasent);
    tcp_err(pcb, ftpd_dataerr);

    switch (fsd->msgfs->state) {
        case FTPD_LIST:
            send_next_directory(fsd, pcb, 0);
            break;
        case FTPD_NLST:
            send_next_directory(fsd, pcb, 1);
            break;
        case FTPD_RETR:
#if FTP_TXPOLL
            poll.fsd = fsd;
            poll.pcb = pcb;
#else
            send_file(fsd, pcb);
#endif
            break;
        default:
            break;
    }

    return ERR_OK;
}

static int open_dataconnection (struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    if (fsm->passive)
        return 0;

    /* Allocate memory for the structure that holds the state of the connection. */
    fsm->datafs = malloc(sizeof(ftpd_datastate_t));

    if (fsm->datafs == NULL) {
        LWIP_DEBUGF(FTPD_DEBUG, ("open_dataconnection: Out of memory\n"));
        send_msg(pcb, fsm, msg451);
        return 1;
    }

    memset(fsm->datafs, 0, sizeof(ftpd_datastate_t));
    fsm->datafs->msgfs = fsm;
    fsm->datafs->msgpcb = pcb;

    if (sfifo_init(&fsm->datafs->fifo, 2000) != 0) {
        free(fsm->datafs);
        send_msg(pcb, fsm, msg451);
        return 1;
    }

    fsm->datapcb = tcp_new();

    if (fsm->datapcb == NULL) {
        sfifo_close(&fsm->datafs->fifo);
        free(fsm->datafs);
        send_msg(pcb, fsm, msg451);
        return 1;
    }

    /* Tell TCP that this is the structure we wish to be passed for our callbacks. */
    tcp_arg(fsm->datapcb, fsm->datafs);
    ip_addr_t dataip;
    IP_SET_TYPE_VAL(dataip, IPADDR_TYPE_V4);
    ip4_addr_copy(*ip_2_ip4(&dataip), fsm->dataip);
    tcp_connect(fsm->datapcb, &dataip, fsm->dataport, ftpd_dataconnected);
    LWIP_DEBUGF(FTPD_DEBUG, ("open con\n"));

    return 0;
}

static void cmd_user (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    send_msg(pcb, fsm, msg331);
    fsm->state = FTPD_PASS;
    /*
       send_msg(pcb, fs, msgLoginFailed);
       fs->state = FTPD_QUIT;
     */
}

static void cmd_pass (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    send_msg(pcb, fsm, msg230);
    fsm->state = FTPD_IDLE;
    /*
       send_msg(pcb, fs, msgLoginFailed);
       fs->state = FTPD_QUIT;
     */
}

static void cmd_port (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    unsigned pHi, pLo;
    unsigned ip[4];

    if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &(ip[0]), &(ip[1]), &(ip[2]), &(ip[3]), &pHi, &pLo) != 6)
        send_msg(pcb, fsm, msg501);
    else {
        IP4_ADDR(&fsm->dataip, (u8_t) ip[0], (u8_t) ip[1], (u8_t) ip[2], (u8_t) ip[3]);
        fsm->dataport = ((u16_t) pHi << 8) | (u16_t) pLo;
        send_msg(pcb, fsm, msg200);
    }
}

static void cmd_quit (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    send_msg(pcb, fsm, msg221);
    fsm->state = FTPD_QUIT;
}

static void cmd_cwd (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    send_msg(pcb, fsm, vfs_chdir(vfs_fixpath(arg)) ? msg550 : msg250);
}

static void cmd_cdup (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    send_msg(pcb, fsm, vfs_chdir("..") ? msg550 : msg250);
}

static void cmd_pwd (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    char *path;

    if ((path = vfs_getcwd(NULL, 0))) {
        send_msg(pcb, fsm, msg257PWD, path);
        free(path);
    } else
        send_msg(pcb, fsm, msg550);
}

static void cmd_list_common (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm, int shortlist)
{
    vfs_dir_t *vfs_dir;
    char *cwd;

    if (!(cwd = vfs_getcwd(NULL, 0))) {
        send_msg(pcb, fsm, msg451);
        return;
    }

    vfs_dir = vfs_opendir(cwd);
    free(cwd);
    if (!vfs_dir) {
        send_msg(pcb, fsm, msg451);
        return;
    }

    if (open_dataconnection(pcb, fsm) != 0) {
        vfs_closedir(vfs_dir);
        return;
    }

    fsm->datafs->vfs_dir = vfs_dir;
    fsm->datafs->vfs_dirent = NULL;
    fsm->state = shortlist ? FTPD_NLST : FTPD_LIST;

    send_msg(pcb, fsm, msg150);
}

static void cmd_nlst (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    cmd_list_common(arg, pcb, fsm, 1);
}

static void cmd_list (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    cmd_list_common(arg, pcb, fsm, 0);
}

static void cmd_retr (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    vfs_file_t *vfs_file;
    vfs_stat_t st;

    vfs_stat(arg, &st);
    if (st.st_mode.directory) {
        send_msg(pcb, fsm, msg550);
        return;
    }

    if (!(vfs_file = vfs_open(arg, "rb"))) {
        send_msg(pcb, fsm, msg550);
        return;
    }

    send_msg(pcb, fsm, msg150recv, arg, st.st_size);

    if (open_dataconnection(pcb, fsm) != 0) {
        vfs_close(vfs_file);
        return;
    }

    fsm->datafs->vfs_file = vfs_file;
    fsm->state = FTPD_RETR;
}

static void cmd_stor (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    vfs_file_t *vfs_file;

    if (!(vfs_file = vfs_open(arg, "wb"))) {
        send_msg(pcb, fsm, msg550);
        return;
    }

    send_msg(pcb, fsm, msg150stor, arg);

    if (open_dataconnection(pcb, fsm) != 0) {
        vfs_close(vfs_file);
        return;
    }

    fsm->datafs->vfs_file = vfs_file;
    fsm->state = FTPD_STOR;
}

static void cmd_noop (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    send_msg(pcb, fsm, msg200);
}

static void cmd_syst (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    send_msg(pcb, fsm, msg214SYST, "UNIX");
}

static void cmd_pasv (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    static u16_t port = 4096;
    static u16_t start_port = 4096;
    struct tcp_pcb *temppcb;

    /* Allocate memory for the structure that holds the state of the connection. */
    if (!(fsm->datafs = malloc(sizeof(ftpd_datastate_t)))) {
        LWIP_DEBUGF(FTPD_DEBUG, ("cmd_pasv: Out of memory\n"));
        send_msg(pcb, fsm, msg451);
        return;
    }

    memset(fsm->datafs, 0, sizeof(ftpd_datastate_t));

    if (sfifo_init(&fsm->datafs->fifo, 3000) != 0) {
        free(fsm->datafs);
        send_msg(pcb, fsm, msg451);
        return;
    }

    if (!(fsm->datalistenpcb = tcp_new())) {
        free(fsm->datafs);
        sfifo_close(&fsm->datafs->fifo);
        send_msg(pcb, fsm, msg451);
        return;
    }

    start_port = port;

    while (1) {
        err_t err;

        if(++port > 0x7fff)
            port = 4096;

        fsm->dataport = port;

        if ((err = tcp_bind(fsm->datalistenpcb, (ip_addr_t*)&pcb->local_ip, fsm->dataport)) == ERR_OK)
            break;

        if (start_port == port)
            err = ERR_CLSD;

        if (err == ERR_USE)
            continue;
        else {
            ftpd_dataclose(fsm->datalistenpcb, fsm->datafs);
            fsm->datalistenpcb = NULL;
            fsm->datafs = NULL;
            return;
        }
    }

    if (!(temppcb = tcp_listen(fsm->datalistenpcb))) {
        LWIP_DEBUGF(FTPD_DEBUG, ("cmd_pasv: tcp_listen failed\n"));
        ftpd_dataclose(fsm->datalistenpcb, fsm->datafs);
        fsm->datalistenpcb = NULL;
        fsm->datafs = NULL;
        return;
    }

    fsm->datalistenpcb = temppcb;
    fsm->passive = 1;
    fsm->datafs->connected = 0;
    fsm->datafs->msgfs = fsm;
    fsm->datafs->msgpcb = pcb;

    /* Tell TCP that this is the structure we wish to be passed for our callbacks. */
    tcp_arg(fsm->datalistenpcb, fsm->datafs);
    tcp_accept(fsm->datalistenpcb, ftpd_dataconnected);

    send_msg(pcb, fsm, msg227, ip4_addr1(ip_2_ip4(&pcb->local_ip)), ip4_addr2(ip_2_ip4(&pcb->local_ip)), ip4_addr3(ip_2_ip4(&pcb->local_ip)), ip4_addr4(ip_2_ip4(&pcb->local_ip)), (fsm->dataport >> 8) & 0xff, (fsm->dataport) & 0xff);
}

static void cmd_abrt (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    if (fsm->datafs != NULL) {
        tcp_arg(fsm->datapcb, NULL);
        tcp_sent(fsm->datapcb, NULL);
        tcp_recv(fsm->datapcb, NULL);
        tcp_abort(pcb);
        sfifo_close(&fsm->datafs->fifo);
        free(fsm->datafs);
        fsm->datafs = NULL;
    }
    fsm->state = FTPD_IDLE;
}

static void cmd_type (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    LWIP_DEBUGF(FTPD_DEBUG, ("Got TYPE -%s-\n", arg));
    
//  if(strcmp(arg, "I") != 0) {
//      send_msg(pcb, fsm, msg502);
//      return;
//  }
    
    send_msg(pcb, fsm, msg200);
}

static void cmd_mode (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    LWIP_DEBUGF(FTPD_DEBUG, ("Got MODE -%s-\n", arg));
    send_msg(pcb, fsm, msg502);
}

static void cmd_rnfr (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    if (arg == NULL || *arg == '\0') {
        send_msg(pcb, fsm, msg501);
        return;
    }

    if (fsm->renamefrom)
        free(fsm->renamefrom);

    fsm->renamefrom = malloc(strlen(arg) + 1);
    if (fsm->renamefrom == NULL) {
        LWIP_DEBUGF(FTPD_DEBUG, ("cmd_rnfr: Out of memory\n"));
        send_msg(pcb, fsm, msg451);
        return;
    }

    strcpy(fsm->renamefrom, arg);
    fsm->state = FTPD_RNFR;
    send_msg(pcb, fsm, msg350);
}

static void cmd_rnto (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    if (fsm->state != FTPD_RNFR) {
        send_msg(pcb, fsm, msg503);
        return;
    }
    fsm->state = FTPD_IDLE;
    if (arg == NULL) {
        send_msg(pcb, fsm, msg501);
        return;
    }
    if (*arg == '\0') {
        send_msg(pcb, fsm, msg501);
        return;
    }

    send_msg(pcb, fsm, vfs_rename(fsm->renamefrom, arg) ? msg450 : msg250);
}

static void cmd_mkd (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    if (arg == NULL || *arg == '\0') {
        send_msg(pcb, fsm, msg501);
        return;
    }

    send_msg(pcb, fsm, vfs_mkdir(arg /*, VFS_IRWXU | VFS_IRWXG | VFS_IRWXO*/) ? msg550 : msg257, arg);
}

static void cmd_rmd (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    vfs_stat_t st;

    if (arg == NULL || *arg == '\0') {
        send_msg(pcb, fsm, msg501);
        return;
    }

    if (vfs_stat(vfs_fixpath(arg), &st) != 0) {
        send_msg(pcb, fsm, msg550);
        return;
    }

    if (!st.st_mode.directory) {
        send_msg(pcb, fsm, msg550);
        return;
    }

    send_msg(pcb, fsm, vfs_rmdir(arg) ? msg550 : msg250);
}

static void cmd_dele (char *arg, struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    vfs_stat_t st;

    if (arg == NULL || *arg == '\0') {
        send_msg(pcb, fsm, msg501);
        return;
    }

    if (vfs_stat(arg, &st) != 0) {
        send_msg(pcb, fsm, msg550);
        return;
    }

    if (st.st_mode.directory) {
        send_msg(pcb, fsm, msg550);
        return;
    }

    send_msg(pcb, fsm, vfs_unlink(arg) ? msg550 : msg250);
}

typedef struct {
    char const *const cmd;
    void (*func) (char *arg, struct tcp_pcb * pcb, ftpd_msgstate_t * fsm);
    bool check_busy;
} ftpd_command_t;

static const ftpd_command_t ftpd_commands[] = {
    {"USER", cmd_user, 0},
    {"PASS", cmd_pass, 0},
    {"PORT", cmd_port, 0},
    {"QUIT", cmd_quit, 0},
    {"CWD",  cmd_cwd,  1},
    {"CDUP", cmd_cdup, 1},
    {"PWD",  cmd_pwd,  0},
    {"XPWD", cmd_pwd,  0},
    {"NLST", cmd_nlst, 1},
    {"LIST", cmd_list, 1},
    {"RETR", cmd_retr, 1},
    {"STOR", cmd_stor, 1},
    {"NOOP", cmd_noop, 0},
    {"SYST", cmd_syst, 0},
    {"ABOR", cmd_abrt, 0},
    {"TYPE", cmd_type, 0},
    {"MODE", cmd_mode, 0},
    {"RNFR", cmd_rnfr, 1},
    {"RNTO", cmd_rnto, 1},
    {"MKD",  cmd_mkd,  1},
    {"XMKD", cmd_mkd,  1},
    {"RMD",  cmd_rmd,  1},
    {"XRMD", cmd_rmd,  1},
    {"DELE", cmd_dele, 1},
    {"PASV", cmd_pasv, 0},
    {NULL,   NULL,     0}
};

static void send_msgdata (struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    u16_t len;

    if ((len = sfifo_used(&fsm->fifo)) > 0) {

        int i = fsm->fifo.readpos;

        /* We cannot send more data than space available in the send buffer. */
        len = len > tcp_sndbuf(pcb) ? tcp_sndbuf(pcb) : len;

        if ((i + len) > fsm->fifo.size) {
            if (tcp_write(pcb, fsm->fifo.buffer + i, (u16_t)(fsm->fifo.size - i), 1) != ERR_OK) {
                LWIP_DEBUGF(FTPD_DEBUG, ("send_msgdata: error writing!\n"));
                return;
            }
            len -= fsm->fifo.size - i;
            fsm->fifo.readpos = 0;
            i = 0;
        }

        if (tcp_write(pcb, fsm->fifo.buffer + i, len, 1) != ERR_OK) {
            LWIP_DEBUGF(FTPD_DEBUG, ("send_msgdata: error writing!\n"));
            return;
        }

        fsm->fifo.readpos += len;
    }
}

static void send_msg (struct tcp_pcb *pcb, ftpd_msgstate_t *fsm, const char *msg, ...)
{
    va_list arg;
    char buffer[1024];
    int len;

    va_start(arg, msg);
    vsprintf(buffer, msg, arg);
    va_end(arg);
    strcat(buffer, "\r\n");

    len = strlen(buffer);
    if (sfifo_space(&fsm->fifo) >= len) {
        LWIP_DEBUGF(FTPD_DEBUG, ("response: %s", buffer));
        sfifo_write(&fsm->fifo, buffer, len);
        send_msgdata(pcb, fsm);
    }
}

static void ftpd_msgerr (void *arg, err_t err)
{
    ftpd_msgstate_t *fsm = arg;

    LWIP_DEBUGF(FTPD_DEBUG, ("ftpd_msgerr: %s (%i)\n", lwip_strerr(err), err));

    if (fsm != NULL) {

        if (fsm->datafs)
            ftpd_dataclose(fsm->datapcb, fsm->datafs);

        sfifo_close(&fsm->fifo);
//        vfs_close(fsm->vfs);

        if (fsm->renamefrom)
            free(fsm->renamefrom);

        if (fsm->cmd.text)
            free(fsm->cmd.text);

        free(fsm);
    }
}

static void ftpd_msgclose (struct tcp_pcb *pcb, ftpd_msgstate_t *fsm)
{
    tcp_arg(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_recv(pcb, NULL);

    if (fsm->datafs)
        ftpd_dataclose(fsm->datapcb, fsm->datafs);

    sfifo_close(&fsm->fifo);
//    vfs_close(fsm->vfs);

    if (fsm->renamefrom)
        free(fsm->renamefrom);

    if (fsm->cmd.text)
        free(fsm->cmd.text);

    free(fsm);

    tcp_arg(pcb, NULL);
    tcp_close(pcb);
}

static err_t ftpd_msgsent (void *arg, struct tcp_pcb *pcb, u16_t len)
{
    ftpd_msgstate_t *fsm = arg;

    if ((sfifo_used(&fsm->fifo) == 0) && (fsm->state == FTPD_QUIT))
        ftpd_msgclose(pcb, fsm);
    else if (pcb->state <= ESTABLISHED)
        send_msgdata(pcb, fsm);

    return ERR_OK;
}

static err_t ftpd_msgrecv (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    ftpd_msgstate_t *fsm = arg;

    if (err == ERR_OK && p != NULL) {

        /* Inform TCP that we have taken the data. */
        tcp_recved(pcb, p->tot_len);

//        LWIP_DEBUGF(FTPD_DEBUG, ("tcp rcvd %d\n", p->tot_len));

        if(fsm->cmd.text) {
            fsm->cmd.text = realloc(fsm->cmd.text, fsm->cmd.len + p->tot_len);
        } else if (*(char *)(p->payload) != TELNET_IAC) { // Ditch packet if telnet commands
            fsm->cmd.len = 0;
            fsm->cmd.text = malloc(p->tot_len + 1);
        }

        if (fsm->cmd.text) {

            char cmd[5];
            char *pt = fsm->cmd.text + (fsm->cmd.len ? fsm->cmd.len - 1 : fsm->cmd.len);

            fsm->cmd.len += p->tot_len + (fsm->cmd.len == 0 ? 1 : 0);

            struct pbuf *q = p;
            do {
                bcopy(q->payload, pt, q->len);
                pt += q->len;
            } while((q = q->next));

            *pt = '\0';
            if((pt = strchr(fsm->cmd.text, '\n'))) {

                while (((*pt == '\r') || (*pt == '\n')) && pt >= fsm->cmd.text)
                    *pt-- = '\0';

                LWIP_DEBUGF(FTPD_DEBUG, ("query: %s\n", fsm->cmd.text));

                strncpy(cmd, fsm->cmd.text, 4);
                for (pt = cmd; isalpha((uint8_t)*pt) && pt < &cmd[4]; pt++)
                    *pt = toupper(*pt);

                *pt = '\0';

                ftpd_command_t *ftpd_cmd = (ftpd_command_t *)ftpd_commands;
                do {
                    if (!strcmp(ftpd_cmd->cmd, cmd))
                        break;
                } while((++ftpd_cmd)->cmd);

                pt = strlen(fsm->cmd.text) < (strlen(cmd) + 1) ? "" :  &fsm->cmd.text[strlen(cmd) + 1];

                if (ftpd_cmd->func) {
                    if(ftpd_cmd->check_busy && sdcard_busy())
                        send_msg(pcb, fsm, msg452);
                    else
                        ftpd_cmd->func(pt, pcb, fsm);
                } else
                    send_msg(pcb, fsm, msg502);

                free(fsm->cmd.text);
                fsm->cmd.text = NULL;
            }
        }
        pbuf_free(p);

    } else if (err == ERR_OK && p == NULL)
        ftpd_msgclose(pcb, fsm);

    return ERR_OK;
}

static err_t ftpd_msgpoll (void *arg, struct tcp_pcb *pcb)
{
    ftpd_msgstate_t *fsm = arg;

    if (fsm != NULL && fsm->datafs && fsm->datafs->connected) {
        switch (fsm->state) {
            case FTPD_LIST:
                send_next_directory(fsm->datafs, fsm->datapcb, 0);
                break;
            case FTPD_NLST:
                send_next_directory(fsm->datafs, fsm->datapcb, 1);
                break;
            case FTPD_RETR:
            //  send_file(fsm->datafs, fsm->datapcb);
                break;
            default:
                break;
        }
    }

    return ERR_OK;
}

static err_t ftpd_msgaccept (void *arg, struct tcp_pcb *pcb, err_t err)
{
    LWIP_PLATFORM_DIAG(("ftpd_msgaccept called"));
    ftpd_msgstate_t *fsm;

    /* Allocate memory for the structure that holds the state of the connection. */
    fsm = malloc(sizeof(ftpd_msgstate_t));

    if (fsm == NULL) {
        LWIP_DEBUGF(FTPD_DEBUG, ("ftpd_msgaccept: Out of memory\n"));
        return ERR_MEM;
    }
    memset(fsm, 0, sizeof(ftpd_msgstate_t));

    /* Initialize the structure. */
    if (sfifo_init(&fsm->fifo, 2000) != 0) {
        free(fsm);
        return ERR_MEM;
    }
    fsm->state = FTPD_IDLE;
    if (fsm->vfs != NULL) {
        sfifo_close(&fsm->fifo);
        free(fsm);
        return ERR_CLSD;
    }

    /* Tell TCP that this is the structure we wish to be passed for our callbacks. */
    tcp_arg(pcb, fsm);

    /* Tell TCP that we wish to be informed of incoming data by a call
       to the ftpd_msgrecv() function. */
    tcp_recv(pcb, ftpd_msgrecv);

    /* Tell TCP that we wish be to informed of data that has been
       successfully sent by a call to the ftpd_sent() function. */
    tcp_sent(pcb, ftpd_msgsent);
    tcp_err(pcb, ftpd_msgerr);
    tcp_poll(pcb, ftpd_msgpoll, FTPD_POLL_INTERVAL);

    send_msg(pcb, fsm, msg220);

    return ERR_OK;
}

void ftpd_poll (void)
{
#if FTP_TXPOLL
    if(poll.pcb) {
        send_file(poll.fsd, poll.pcb);
        poll.pcb = NULL;
    }
#endif
}

bool ftpd_init (uint16_t port)
{
    err_t err;
    struct tcp_pcb *pcb = tcp_new();

    if((err = tcp_bind(pcb, IP_ADDR_ANY, port)) == ERR_OK) {

        vfs_load_plugin(vfs_default_fs);

        pcb = tcp_listen(pcb);
        tcp_accept(pcb, ftpd_msgaccept);

        sdcard_getfs();
    }

    return err == ERR_OK;
}

#endif
