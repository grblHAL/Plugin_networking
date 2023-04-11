/**
 * @file
 * LWIP HTTP server implementation
 */

/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *         Simon Goldschmidt
 *
 */

/*
 * 2022-08-14: Modified by Terje Io for grblHAL networking.
 * 2022-08-27: Modified by Terje Io for grblHAL VFS
 * 2023-04-11: Modified by Terje Io to improve handling of content encoding
 */

/**
 * @defgroup httpd HTTP server
 * @ingroup apps
 *
 * A simple common
 * gateway interface (CGI) handling mechanism has been added to allow clients
 * to hook functions to particular request URIs.
 *
 * Notes on CGI usage
 * ------------------
 *
 * The simple CGI support offered here works with GET method requests only
 * and can handle up to 16 parameters encoded into the URI. The handler
 * function may not write directly to the HTTP output but must return a
 * filename that the HTTP server will send to the browser as a response to
 * the incoming CGI request.
 *
 *
 * The list of supported file types is quite short, so if makefsdata complains
 * about an unknown extension, make sure to add it (and its doctype) to
 * the 'httpd_headers' list.
 */

#ifdef ARDUINO
#include "../driver.h"
#else
#include "driver.h"
#endif

#if HTTP_ENABLE

#include "httpd.h"

#if LWIP_TCP && LWIP_CALLBACK_API

#include <string.h> /* memset */
#include <stdlib.h> /* atoi */
#include <stdio.h>
#include <stdbool.h>

#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/def.h"

#ifdef LWIP_HOOK_FILENAME
#include LWIP_HOOK_FILENAME
#endif
#if LWIP_HTTPD_TIMING
#include "lwip/sys.h"
#endif /* LWIP_HTTPD_TIMING */

#include "strutils.h"
#include "urldecode.h"

/**/

#if LWIP_HTTPD_DYNAMIC_HEADERS

/* The number of individual strings that comprise the headers sent before each requested file. */
#define NUM_FILE_HDR_STRINGS            8
#define HDR_STRINGS_IDX_HTTP_STATUS     0 /* e.g. "HTTP/1.0 200 OK\r\n" */
#define HDR_STRINGS_IDX_SERVER_NAME     1 /* e.g. "Server: "HTTPD_SERVER_AGENT"\r\n" */
#define HDR_STRINGS_IDX_CONTENT_NEXT    2 /* the content type (or default answer content type including default document) */

/* The dynamically generated Content-Length buffer needs space for CRLF + CRLF + NULL */
#define LWIP_HTTPD_MAX_CONTENT_LEN_OFFSET 5
#ifndef LWIP_HTTPD_MAX_CONTENT_LEN_SIZE
/* The dynamically generated Content-Length buffer shall be able to work with ~953 MB (9 digits) */
#define LWIP_HTTPD_MAX_CONTENT_LEN_SIZE   (9 + LWIP_HTTPD_MAX_CONTENT_LEN_OFFSET)
#endif

// Keep in sync with http_encoding_t!
static const char *httpd_encodings[] = {
    "Content-Encoding: compress\r\n",
    "Content-Encoding: deflate\r\n",
    "Content-Encoding: gzip\r\n"
};

/** This struct is used for a list of HTTP header strings for various filename extensions. */
typedef struct {
  const char *extension;
  const char *content_type;
} http_header_t;

#define HTTP_CONTENT_TYPE(contenttype) "Content-Type: " contenttype "\r\n"
#define HTTP_CONTENT_TYPE_ENCODING(contenttype, encoding) "Content-Type: " contenttype "\r\nContent-Encoding: " encoding "\r\n"

#define HTTP_HDR_HTML           HTTP_CONTENT_TYPE("text/html; charset=UTF-8")
#define HTTP_HDR_SSI            HTTP_CONTENT_TYPE("text/html\r\nExpires: Fri, 10 Apr 2008 14:00:00 GMT\r\nPragma: no-cache")
#define HTTP_HDR_GIF            HTTP_CONTENT_TYPE("image/gif")
#define HTTP_HDR_PNG            HTTP_CONTENT_TYPE("image/png")
#define HTTP_HDR_JPG            HTTP_CONTENT_TYPE("image/jpeg")
#define HTTP_HDR_BMP            HTTP_CONTENT_TYPE("image/bmp")
#define HTTP_HDR_ICO            HTTP_CONTENT_TYPE("image/x-icon")
#define HTTP_HDR_APP            HTTP_CONTENT_TYPE("application/octet-stream")
#define HTTP_HDR_JS             HTTP_CONTENT_TYPE("application/javascript")
#define HTTP_HDR_RA             HTTP_CONTENT_TYPE("application/javascript")
#define HTTP_HDR_CSS            HTTP_CONTENT_TYPE("text/css")
#define HTTP_HDR_SWF            HTTP_CONTENT_TYPE("application/x-shockwave-flash")
#define HTTP_HDR_XML            HTTP_CONTENT_TYPE("text/xml")
#define HTTP_HDR_PDF            HTTP_CONTENT_TYPE("application/pdf")
#define HTTP_HDR_JSON           HTTP_CONTENT_TYPE("application/json")
#define HTTP_HDR_CSV            HTTP_CONTENT_TYPE("text/csv")
#define HTTP_HDR_TSV            HTTP_CONTENT_TYPE("text/tsv")
#define HTTP_HDR_SVG            HTTP_CONTENT_TYPE("image/svg+xml")
#define HTTP_HDR_GZIP           HTTP_CONTENT_TYPE("application/gzip")
#define HTTP_HDR_SVGZ           HTTP_CONTENT_TYPE_ENCODING("image/svg+xml", "gzip")

#define HTTP_HDR_DEFAULT_TYPE   HTTP_CONTENT_TYPE("text/plain")

/** A list of extension-to-HTTP header strings (see outdated RFC 1700 MEDIA TYPES
 * and http://www.iana.org/assignments/media-types for registered content types
 * and subtypes) */
PROGMEM static const http_header_t httpd_headers[] = {
  { "html", HTTP_HDR_HTML},
  { "json", HTTP_HDR_JSON},
  { "htm",  HTTP_HDR_HTML},
  { "gif",  HTTP_HDR_GIF},
  { "png",  HTTP_HDR_PNG},
  { "jpg",  HTTP_HDR_JPG},
  { "bmp",  HTTP_HDR_BMP},
  { "ico",  HTTP_HDR_ICO},
  { "class", HTTP_HDR_APP},
  { "cls",  HTTP_HDR_APP},
  { "js",   HTTP_HDR_JS},
  { "ram",  HTTP_HDR_RA},
  { "css",  HTTP_HDR_CSS},
  { "swf",  HTTP_HDR_SWF},
  { "xml",  HTTP_HDR_XML},
  { "xsl",  HTTP_HDR_XML},
  { "pdf",  HTTP_HDR_PDF},
  { "gz", HTTP_HDR_GZIP}
#ifdef HTTPD_ADDITIONAL_CONTENT_TYPES
  /* If you need to add content types not listed here:
   * #define HTTPD_ADDITIONAL_CONTENT_TYPES {"ct1", HTTP_CONTENT_TYPE("text/ct1")}, {"exe", HTTP_CONTENT_TYPE("application/exe")}
   */
  , HTTPD_ADDITIONAL_CONTENT_TYPES
#endif
};

#define NUM_HTTP_HEADERS LWIP_ARRAYSIZE(httpd_headers)

#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */

// NOTE: Methods list must match http_method_t enumeration entries!
#if LWIP_HTTPD_SUPPORT_POST
#define HTTP_METHODS "HEAD,GET,,POST,,OPTIONS"
#else
#define HTTP_METHODS "HEAD,GET,,,,OPTIONS"
#endif

typedef enum {
    HTTP_Status200 = 200,
    HTTP_Status404 = 404,
    HTTP_Status405 = 405,
    HTTP_Status500 = 500
} http_response_status_t;

typedef enum {
    HTTP_HeaderTypeROM = 0,
    HTTP_HeaderTypeVolatile,
    HTTP_HeaderTypeAllocated,
} http_headertype_t;

typedef struct {
    const char *string[NUM_FILE_HDR_STRINGS]; /* HTTP headers to be sent. */
    http_headertype_t type[NUM_FILE_HDR_STRINGS]; /* HTTP headers to be sent. */
    char content_len[LWIP_HTTPD_MAX_CONTENT_LEN_SIZE];
    u16_t pos;     /* The position of the first unsent header byte in the current string */
    u16_t index;   /* The index of the hdr string currently being sent. */
    u16_t next;    /* The index of the hdr string to add next. */
} http_headers_t;

struct http_state {
#if LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED
    http_state_t *next;
#endif /* LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED */
    vfs_file_t *handle;
    const char *file;       /* Pointer to first unsent byte in buf. */
    const char *uri;       /* Pointer to uri. */
    const char *hdr;       /* Pointer to header. */
    u32_t hdr_len;
    u32_t payload_offset;
    http_method_t method;
    struct altcp_pcb *pcb;
    u32_t left;       /* Number of unsent bytes in buf. */
    u8_t retries;
    uint_fast8_t param_count;
    char *params[LWIP_HTTPD_MAX_CGI_PARAMETERS]; /* Params extracted from the request URI */
    char *param_vals[LWIP_HTTPD_MAX_CGI_PARAMETERS]; /* Values for each extracted param */
#if LWIP_HTTPD_SUPPORT_REQUESTLIST
    struct pbuf *req;
#endif /* LWIP_HTTPD_SUPPORT_REQUESTLIST */
#if LWIP_HTTPD_DYNAMIC_FILE_READ
    char *buf;        /* File read buffer. */
    int buf_len;      /* Size of file read buffer, buf. */
#endif /* LWIP_HTTPD_DYNAMIC_FILE_READ */
#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
    u8_t keepalive;
#endif /* LWIP_HTTPD_SUPPORT_11_KEEPALIVE */
#if LWIP_HTTPD_DYNAMIC_HEADERS
    http_headers_t response_hdr;
#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */
#if LWIP_HTTPD_TIMING
    u32_t time_started;
#endif /* LWIP_HTTPD_TIMING */
    u32_t post_content_len_left;
    http_request_t request;
#if LWIP_HTTPD_POST_MANUAL_WND
    u32_t unrecved_bytes;
    u8_t no_auto_wnd;
    u8_t post_finished;
#endif /* LWIP_HTTPD_POST_MANUAL_WND */
};

/**/

#if LWIP_HTTPD_SSI
#error SSI support has been removed!
#endif

#if LWIP_HTTPD_SUPPORT_V09
#error HTTP v0.9 support has been removed!
#endif

#if LWIP_HTTPD_OMIT_HEADER_FOR_EXTENSIONLESS_URI
#error Support for LWIP_HTTPD_OMIT_HEADER_FOR_EXTENSIONLESS_URI has been removed!
#endif

/** Minimum length for a valid HTTP/0.9 request: "GET /\r\n" -> 7 bytes */
#define MIN_REQ_LEN   7

#define CRLF "\r\n"
#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
#define HTTP11_CONNECTIONKEEPALIVE  "Connection: keep-alive"
#define HTTP11_CONNECTIONKEEPALIVE2 "Connection: Keep-Alive"
#endif

#if LWIP_HTTPD_DYNAMIC_FILE_READ
#define HTTP_IS_DYNAMIC_FILE(hs) ((hs)->buf != NULL)
#else
#define HTTP_IS_DYNAMIC_FILE(hs) 0
#endif

/* This defines checks whether tcp_write has to copy data or not */

#ifndef HTTP_IS_DATA_VOLATILE
/** tcp_write does not have to copy data when sent from rom-file-system directly */
#define HTTP_IS_DATA_VOLATILE(hs)       (HTTP_IS_DYNAMIC_FILE(hs) ? TCP_WRITE_FLAG_COPY : 0)
#endif
/** Default: dynamic headers are sent from ROM (non-dynamic headers are handled like file data) */
#ifndef HTTP_IS_HDR_VOLATILE
#define HTTP_IS_HDR_VOLATILE(hs, ptr)   0
#endif

#define HTTP_HDR_CONTENT_LEN_DIGIT_MAX_LEN  10

/* Return type and values for http_send_*() */
typedef enum {
    HTTPSend_NoData = 0,
    HTTPSend_Continue,
    HTTPSend_Break,
    HTTPSend_Freed
} http_send_state_t;

#ifdef LWIP_DEBUGF
/*
#undef LWIP_DEBUGF

#define LWIP_DEBUGF(debug, message) do { dbg message; } while(0)

#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "driver.h"

void dbg (char *msg, ...)
{
    char buffer[200];
    va_list arg;
    va_start(arg, msg);
    vsprintf(buffer, msg, arg);
    va_end(arg);
    hal.stream.write(buffer);
}
*/
#endif

typedef struct {
  const char *name;
  u8_t shtml;
  http_encoding_t encoding;
} default_filename;

PROGMEM static const default_filename httpd_default_filenames[] = {
    {"/index.html",    0, HTTPEncoding_None },
    {"/index.html.gz", 0, HTTPEncoding_GZIP },
    {"/index.htm",     0, HTTPEncoding_None }
};

http_event_t httpd = {0};

static const char *msg200 = "HTTP/1.1 200 OK" CRLF;
static const char *msg400 = "HTTP/1.1 400 Bad Request" CRLF;
static const char *msg404 = "HTTP/1.1 404 File not found" CRLF;
static const char *msg501 = "HTTP/1.1 501 Not Implemented" CRLF;
static const char *agent = "Server: " HTTPD_SERVER_AGENT CRLF;
static const char *conn_close = "Connection: Close" CRLF CRLF;
static const char *conn_keep = "Connection: keep-alive" CRLF CRLF;
static const char *conn_keep2 = "Connection: keep-alive" CRLF "Content-Length: ";
//static const char *cont_len = "Content-Length: ";
static const char *rsp404 = "<html><body><h2>404: The requested file cannot be found.</h2></body></html>" CRLF;
static const char *http_methods = HTTP_METHODS;

#define NUM_DEFAULT_FILENAMES LWIP_ARRAYSIZE(httpd_default_filenames)

/** HTTP request is copied here from pbufs for simple parsing */
static char httpd_req_buf[LWIP_HTTPD_MAX_REQ_LENGTH + 1];
//#if LWIP_HTTPD_SUPPORT_POST
#if LWIP_HTTPD_POST_MAX_RESPONSE_URI_LEN > LWIP_HTTPD_MAX_REQUEST_URI_LEN
#define LWIP_HTTPD_URI_BUF_LEN LWIP_HTTPD_POST_MAX_RESPONSE_URI_LEN
#endif
//#endif
#ifndef LWIP_HTTPD_URI_BUF_LEN
#define LWIP_HTTPD_URI_BUF_LEN LWIP_HTTPD_MAX_REQUEST_URI_LEN
#endif
#if LWIP_HTTPD_URI_BUF_LEN
/* Filename for response file to send when POST is finished or
 * search for default files when a directory is requested. */
static char http_uri_buf[LWIP_HTTPD_URI_BUF_LEN + 1];
#endif

#if HTTPD_USE_MEM_POOL
LWIP_MEMPOOL_DECLARE(HTTPD_STATE,     MEMP_NUM_PARALLEL_HTTPD_CONNS,     sizeof(http_state_t),     "HTTPD_STATE")
#define HTTP_ALLOC_HTTP_STATE() (http_state_t *)LWIP_MEMPOOL_ALLOC(HTTPD_STATE)
#define HTTP_FREE_HTTP_STATE(x) LWIP_MEMPOOL_FREE(HTTPD_STATE, (x))
#else /* HTTPD_USE_MEM_POOL */
#define HTTP_ALLOC_HTTP_STATE() (http_state_t *)mem_malloc(sizeof(http_state_t))
#define HTTP_FREE_HTTP_STATE(x) mem_free(x)
#endif /* HTTPD_USE_MEM_POOL */

static err_t http_close_conn (struct altcp_pcb *pcb, http_state_t *hs);
static err_t http_close_or_abort_conn (struct altcp_pcb *pcb, http_state_t *hs, u8_t abort_conn);
static err_t http_init_file (http_state_t *hs, vfs_file_t *file, const char *uri, char *params);
static err_t http_poll (void *arg, struct altcp_pcb *pcb);
static bool http_check_eof (struct altcp_pcb *pcb, http_state_t *hs);
static err_t http_process_request (http_state_t *hs, const char *uri);
#if LWIP_HTTPD_FS_ASYNC_READ
static void http_continue (void *connection);
#endif /* LWIP_HTTPD_FS_ASYNC_READ */

/* URI handler information */
static const httpd_uri_handler_t *uri_handlers;
static uint_fast8_t num_uri_handlers;

#if LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED
/** global list of active HTTP connections, use to kill the oldest when running out of memory */
static http_state_t *http_connections;

static void http_add_connection (http_state_t *hs)
{
    /* add the connection to the list */
    hs->next = http_connections;
    http_connections = hs;
}

static void http_remove_connection (http_state_t *hs)
{
    /* take the connection off the list */
    if (http_connections) {
        if (http_connections == hs) {
            http_connections = hs->next;
        } else {
            http_state_t *last;
            for (last = http_connections; last->next != NULL; last = last->next) {
                if (last->next == hs) {
                    last->next = hs->next;
                    break;
                }
            }
        }
    }
}

static void http_kill_oldest_connection (u8_t ssi_required)
{
    http_state_t *hs = http_connections;
    http_state_t *hs_free_next = NULL;

    while (hs && hs->next) {
#if LWIP_HTTPD_SSI
        if (ssi_required) {
            if (hs->next->ssi != NULL) {
            hs_free_next = hs;
            }
        } else
 #else /* LWIP_HTTPD_SSI */
            LWIP_UNUSED_ARG(ssi_required);
#endif /* LWIP_HTTPD_SSI */
        {
            hs_free_next = hs;
        }
        LWIP_ASSERT("broken list", hs != hs->next);
        hs = hs->next;
    }

    if (hs_free_next != NULL) {
        LWIP_ASSERT("hs_free_next->next != NULL", hs_free_next->next != NULL);
        LWIP_ASSERT("hs_free_next->next->pcb != NULL", hs_free_next->next->pcb != NULL);
        /* send RST when killing a connection because of memory shortage */
        http_close_or_abort_conn(hs_free_next->next->pcb, hs_free_next->next, 1); /* this also unlinks the http_state from the list */
    }
}
#else /* LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED */

#define http_add_connection(hs)
#define http_remove_connection(hs)

#endif /* LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED */

/** Initialize a http_state_t.
 */
static void http_state_init (http_state_t *hs)
{
    /* Initialize the structure. */
    memset(hs, 0, sizeof(http_state_t));

    hs->request.handle = hs;

#if LWIP_HTTPD_DYNAMIC_HEADERS
    /* Indicate that the headers are not yet valid */
    hs->response_hdr.index = NUM_FILE_HDR_STRINGS;
#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */
}

/** Allocate a http_state_t. */
static http_state_t *http_state_alloc (void)
{
    http_state_t *ret = HTTP_ALLOC_HTTP_STATE();

#if LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED
    if (ret == NULL) {
        http_kill_oldest_connection(0);
        ret = HTTP_ALLOC_HTTP_STATE();
    }
#endif /* LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED */

    if (ret != NULL) {
        http_state_init(ret);
        http_add_connection(ret);
    }

    return ret;
}

/** Free a http_state_t.
 * Also frees the file data if dynamic.
 */
static void http_state_eof (http_state_t *hs)
{
    if (hs->handle) {
#if LWIP_HTTPD_TIMING
        u32_t ms_needed = sys_now() - hs->time_started;
        u32_t needed = LWIP_MAX(1, (ms_needed / 100));
        LWIP_DEBUGF(HTTPD_DEBUG_TIMING, ("httpd: needed %"U32_F" ms to send file of %d bytes -> %"U32_F" bytes/sec\n",
                                         ms_needed, hs->handle->len, ((((u32_t)hs->handle->len) * 10) / needed)));
#endif /* LWIP_HTTPD_TIMING */
        vfs_close(hs->handle);
        hs->handle = NULL;
    }

    uint_fast8_t i = NUM_FILE_HDR_STRINGS;
    do {
        i--;
        if (hs->response_hdr.type[i] == HTTP_HeaderTypeAllocated && hs->response_hdr.string[i])
            mem_free((void *)hs->response_hdr.string[i]);
    } while(i);

    memset(&hs->response_hdr, 0, sizeof(http_headers_t));

#if LWIP_HTTPD_DYNAMIC_FILE_READ
    if (hs->buf != NULL) {
        mem_free(hs->buf);
        hs->buf = NULL;
    }
#endif /* LWIP_HTTPD_DYNAMIC_FILE_READ */

    if (hs->req) {
        pbuf_free(hs->req);
        hs->req = NULL;
    }
}

void http_set_allowed_methods (const char *methods)
{
    http_methods = methods;
}

/** Free a http_state_t.
 * Also frees the file data if dynamic.
 */
static void http_state_free (http_state_t *hs)
{
    if (hs != NULL) {
        if(hs->request.on_request_completed)
            hs->request.on_request_completed(hs->request.private_data);
        http_state_eof(hs);
        http_remove_connection(hs);
        HTTP_FREE_HTTP_STATE(hs);
    }
}

ip_addr_t http_get_remote_ip (http_request_t *request)
{
    return request ? request->handle->pcb->remote_ip : (ip_addr_t){0};
}

uint16_t http_get_remote_port (http_request_t *request)
{
    return request ? request->handle->pcb->remote_port : 0;
}

const char *http_get_uri (http_request_t *request)
{
    return request ? request->handle->uri : NULL;
}

uint8_t http_get_param_count (http_request_t *request)
{
    return request ? request->handle->param_count : 0;
}

char *http_get_param_value (http_request_t *request, const char *name, char *value, uint32_t size)
{
    bool found = false;
    http_state_t *hs = request->handle;
    uint_fast8_t idx = hs->param_count;

    *value = '\0';

    if(idx) do {
        if((found = strcmp(name, hs->params[--idx]) == 0))
            urldecode(value, hs->param_vals[idx]);
    } while(idx && !found);

    return found ? value : NULL;
}

int http_get_header_value_len (http_request_t *request, const char *name)
{
    int len = -1;
    char *hdr, *end;
    http_state_t *hs = request->handle;

    if ((hdr = lwip_strnstr(hs->hdr, name, hs->hdr_len))) {
        hdr += strlen(name);
        if(*hdr == ':') {
            hdr++;
            if(*hdr == ' ')
                hdr++;
            if ((end = lwip_strnstr(hdr, CRLF, hs->hdr_len)))
                len = end - hdr;
        }
    }

    return len;
}

char *http_get_header_value (http_request_t *request, const char *name, char *value, uint32_t size)
{
    char *hdr, *end = NULL;
    http_state_t *hs = request->handle;
    size_t len = strlen(name);

    *value = '\0';
    if ((hdr = lwip_strnstr(hs->hdr, name, hs->hdr_len))) {
        hdr += len;
        if(*hdr == ':') {
            hdr++;
            if(*hdr == ' ')
                hdr++;
            if ((end = lwip_strnstr(hdr, CRLF, size + 2)) && end - hdr <= size) {
                len = end - hdr;
                memcpy(value, hdr, len);
                value[len] = '\0';
            }
        }
    }

    return end ? value : NULL;
}

/** Call tcp_write() in a loop trying smaller and smaller length
 *
 * @param pcb altcp_pcb to send
 * @param ptr Data to send
 * @param length Length of data to send (in/out: on return, contains the amount of data sent)
 * @param apiflags directly passed to tcp_write
 * @return the return value of tcp_write
 */
static err_t http_write (struct altcp_pcb *pcb, const void *ptr, u16_t *length, u8_t apiflags)
{
    u16_t len, max_len;
    err_t err;

    //  LWIP_ASSERT("length != NULL", length != NULL);
    len = *length;
    if (len == 0)
        return ERR_OK;

    /* We cannot send more data than space available in the send buffer. */
    max_len = altcp_sndbuf(pcb);
    if (max_len < len)
        len = max_len;

#ifdef HTTPD_MAX_WRITE_LEN
    /* Additional limitation: e.g. don't enqueue more than 2*mss at once */
    max_len = HTTPD_MAX_WRITE_LEN(pcb);
    if (len > max_len)
        len = max_len;
#endif /* HTTPD_MAX_WRITE_LEN */

    do {
    //    LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Trying to send %d bytes\n", len));
        if ((err = altcp_write(pcb, ptr, len, apiflags)) == ERR_MEM) {
            if ((altcp_sndbuf(pcb) == 0) ||
                (altcp_sndqueuelen(pcb) >= TCP_SND_QUEUELEN)) {
                /* no need to try smaller sizes */
                len = 1;
            } else
                len /= 2;
        //      LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Send failed, trying less (%d bytes)\n", len));
        }
    } while ((err == ERR_MEM) && (len > 1));

    if (err == ERR_OK) {
        LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Sent %d bytes\n", len));
        *length = len;
    } else {
        LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Send failed with err %d (\"%s\")\n", err, lwip_strerr(err)));
        *length = 0;
    }

#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
    /* ensure nagle is normally enabled (only disabled for persistent connections
    when all data has been enqueued but the connection stays open for the next
    request */
    altcp_nagle_enable(pcb);
#endif

    return err;
}

/**
 * The connection shall be actively closed (using RST to close from fault states).
 * Reset the sent- and recv-callbacks.
 *
 * @param pcb the tcp pcb to reset callbacks
 * @param hs connection state to free
 */
static err_t http_close_or_abort_conn (struct altcp_pcb *pcb, http_state_t *hs, u8_t abort_conn)
{
    err_t err;
    LWIP_DEBUGF(HTTPD_DEBUG, ("Closing connection %p\n", (void *)pcb));

    if (hs != NULL) {
        if ((hs->post_content_len_left != 0)
            #if LWIP_HTTPD_POST_MANUAL_WND
            || ((hs->no_auto_wnd != 0) && (hs->unrecved_bytes != 0))
            #endif /* LWIP_HTTPD_POST_MANUAL_WND */
        ) {
            /* make sure the post code knows that the connection is closed */
            *http_uri_buf = '\0';
            hs->request.post_finished(&hs->request, http_uri_buf, LWIP_HTTPD_URI_BUF_LEN);
        }
    }

    altcp_arg(pcb, NULL);
    altcp_recv(pcb, NULL);
    altcp_err(pcb, NULL);
    altcp_poll(pcb, NULL, 0);
    altcp_sent(pcb, NULL);
    if (hs != NULL)
        http_state_free(hs);

    if (abort_conn) {
        altcp_abort(pcb);
        return ERR_OK;
    }

    if ((err = altcp_close(pcb)) != ERR_OK) {
        LWIP_DEBUGF(HTTPD_DEBUG, ("Error %d closing %p\n", err, (void *)pcb));
        /* error closing, try again later in poll */
        altcp_poll(pcb, http_poll, HTTPD_POLL_INTERVAL);
    }

    return err;
}

/**
 * The connection shall be actively closed.
 * Reset the sent- and recv-callbacks.
 *
 * @param pcb the tcp pcb to reset callbacks
 * @param hs connection state to free
 */
static err_t http_close_conn (struct altcp_pcb *pcb, http_state_t *hs)
{
  return http_close_or_abort_conn(pcb, hs, 0);
}

/** End of file: either close the connection (Connection: close) or
 * close the file (Connection: keep-alive)
 */
static void http_eof (struct altcp_pcb *pcb, http_state_t *hs)
{
    /* HTTP/1.1 persistent connection? (Not supported for SSI) */
#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
    if (hs->keepalive) {
        http_remove_connection(hs);

        http_state_eof(hs);
        http_state_init(hs);
        /* restore state: */
        hs->pcb = pcb;
        hs->keepalive = 1;
        http_add_connection(hs);
        /* ensure nagle doesn't interfere with sending all data as fast as possible: */
        altcp_nagle_disable(pcb);
    } else
#endif /* LWIP_HTTPD_SUPPORT_11_KEEPALIVE */
    http_close_conn(pcb, hs);
}

/**
 * Extract URI parameters from the parameter-part of an URI in the form
 * "test.cgi?x=y" @todo: better explanation!
 * Pointers to the parameters are stored in hs->param_vals.
 *
 * @param hs http connection state
 * @param params pointer to the NULL-terminated parameter string from the URI
 * @return number of parameters extracted
 */
static uint_fast8_t extract_uri_parameters (http_state_t *hs, char *params)
{
    char *pair, *equals;
    uint_fast8_t loop;

    /* If we have no parameters at all, return immediately. */
    if (!params || (params[0] == '\0'))
        return 0;

    /* Get a pointer to our first parameter */
    pair = params;

    /* Parse up to LWIP_HTTPD_MAX_CGI_PARAMETERS from the passed string and ignore the remainder (if any) */
    for (loop = 0; (loop < LWIP_HTTPD_MAX_CGI_PARAMETERS) && pair; loop++) {

        /* Save the name of the parameter */
        hs->params[loop] = pair;

        /* Remember the start of this name=value pair */
        equals = pair;

        /* Find the start of the next name=value pair and replace the delimiter
         * with a 0 to terminate the previous pair string. */
        if ((pair = strchr(pair, '&')))
            *pair++ = '\0';
        else {
            /* We didn't find a new parameter so find the end of the URI and replace the space with a '\0' */
            if ((pair = strchr(equals, ' ')))
                *pair = '\0';

            /* Revert to NULL so that we exit the loop as expected. */
            pair = NULL;
        }

        /* Now find the '=' in the previous pair, replace it with '\0' and save the parameter value string. */
        if ((equals = strchr(equals, '='))) {
            *equals = '\0';
            hs->param_vals[loop] = equals + 1;
        } else
            hs->param_vals[loop] = NULL;
    }

    return loop;
}

#if LWIP_HTTPD_DYNAMIC_HEADERS

static bool is_response_header_set (http_state_t *hs, const char *name)
{
    bool is_set = false;

    uint_fast8_t i = NUM_FILE_HDR_STRINGS, len = strlen(name);
    do {
        i--;
        is_set = hs->response_hdr.string[i] && !strncmp(name, hs->response_hdr.string[i], len);
    } while(i && !is_set);

    return is_set;
}

bool http_set_response_header (http_request_t *request, const char *name, const char *value)
{
    bool ok;
    http_state_t *hs = request->handle;

    if((ok = hs->response_hdr.next < (NUM_FILE_HDR_STRINGS - 1))) {

        char *hdr;

        if((hdr = mem_malloc(strlen(name) + strlen(value) + 5))) {
            strcat(strcat(strcat(strcpy(hdr, name), ": "), value), CRLF);
            hs->response_hdr.string[hs->response_hdr.next] = hdr;
            hs->response_hdr.type[hs->response_hdr.next++] = HTTP_HeaderTypeAllocated;
        } else
            ok = false;
    }

    return ok;
}

void http_set_response_status (http_request_t *request, const char *status)
{
    http_state_t *hs = request->handle;

    char *hdr;

    if(status && (hdr = mem_malloc(strlen(status) + 12))) {
        strcat(strcat(strcpy(hdr, "HTTP/1.1 "), status), CRLF);
        hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] = hdr;
        hs->response_hdr.type[HDR_STRINGS_IDX_HTTP_STATUS] = HTTP_HeaderTypeAllocated;
    }
}


/* We are dealing with a particular filename. Look for one other
special case.  We assume that any filename with "404" in it must be
indicative of a 404 server error whereas all other files require
the 200 OK header. */
static void set_content_type (http_state_t *hs, const char *uri)
{
    if(!is_response_header_set(hs, "Content-Type") && hs->response_hdr.next < NUM_FILE_HDR_STRINGS) {

        char *end, *ext;
        bool ext_found = false;
        uint_fast8_t content_type = NUM_HTTP_HEADERS;

        if (!(end = strchr(uri, '?')))
            end = strchr(uri, '\0');

        ext_found = (ext = strrchr(uri, '.')) && ext < end;

        if(end != uri) {
            for (content_type = 0; content_type < NUM_HTTP_HEADERS; content_type++) {
                size_t len = strlen(httpd_headers[content_type].extension);
                ext = end - len;
                if(ext > uri && *(ext - 1) == '.'  && !lwip_strnicmp(httpd_headers[content_type].extension, ext, len))
                    break;
            }
        }

        /* Did we find a matching extension? */
        if (content_type < NUM_HTTP_HEADERS) {
            /* yes, store it */
            hs->response_hdr.string[hs->response_hdr.next++] = httpd_headers[content_type].content_type;
        } else if (!ext_found) {
            /* no, no extension found -> use binary transfer to prevent the browser adding '.txt' on save */
            hs->response_hdr.string[hs->response_hdr.next++] = HTTP_HDR_APP;
        } else {
            const char *content_type;
            if(httpd.on_unknown_content_type && (content_type = httpd.on_unknown_content_type(ext)))
                hs->response_hdr.string[hs->response_hdr.next++] = content_type;
            else /* No - use the default, plain text file type. */
                hs->response_hdr.string[hs->response_hdr.next++] = HTTP_HDR_DEFAULT_TYPE;
        }

        if(hs->request.encoding)
            hs->response_hdr.string[hs->response_hdr.next++] = httpd_encodings[hs->request.encoding - 1];
    }
}

/* Add content-length header? */
static void get_http_content_length (http_state_t *hs, int file_len)
{
    bool add_content_len = false;

    if ((add_content_len = file_len >= 0 && hs->response_hdr.next < (NUM_FILE_HDR_STRINGS - 1))) {
        size_t len;
        lwip_itoa(hs->response_hdr.content_len, (size_t)LWIP_HTTPD_MAX_CONTENT_LEN_SIZE, file_len);
        len = strlen(hs->response_hdr.content_len);
        if ((add_content_len = len <= LWIP_HTTPD_MAX_CONTENT_LEN_SIZE - LWIP_HTTPD_MAX_CONTENT_LEN_OFFSET)) {
            SMEMCPY(&hs->response_hdr.content_len[len], CRLF CRLF, 5);
            hs->response_hdr.string[hs->response_hdr.next + 1] = hs->response_hdr.content_len;
            hs->response_hdr.type[hs->response_hdr.next + 1] = HTTP_HeaderTypeVolatile;
        }
    }

#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
    if (add_content_len) {
        hs->response_hdr.string[hs->response_hdr.next] = conn_keep2;
        hs->response_hdr.next += 2;
    } else {
        hs->response_hdr.string[hs->response_hdr.next++] = conn_close;
        hs->keepalive = 0;
    }
#else /* LWIP_HTTPD_SUPPORT_11_KEEPALIVE */
    if (add_content_len) {
        hs->response_hdr.string[hs->response_hdr.next] = cont_len;
        hs->response_hdr.next += 2;
    }
#endif /* LWIP_HTTPD_SUPPORT_11_KEEPALIVE */
}

/**
 * Generate the relevant HTTP headers for the given filename and write
 * them into the supplied buffer.
 */
static void get_http_headers (http_state_t *hs, const char *uri)
{
    if(hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] == NULL) {

        /* Is this a normal file or the special case we use to send back the default "404: Page not found" response? */
        if (uri == NULL) {

            if(hs->method == HTTP_Post) {
                hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] = msg200;
                hs->response_hdr.string[hs->response_hdr.next++] = conn_keep;
            } else {
                hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] = msg404;
                set_content_type(hs, ".html");
                get_http_content_length(hs, strlen(rsp404));
                hs->response_hdr.string[hs->response_hdr.next++] = rsp404;
            }

        } else  {

            /* We are dealing with a particular filename. Look for one other
            special case.  We assume that any filename with "404" in it must be
            indicative of a 404 server error whereas all other files require
            the 200 OK header. */
            if (strstr(uri, "404"))
                hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] = msg404;
            else if (strstr(uri, "400"))
                hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] = msg400;
            else if (strstr(uri, "501"))
                hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] = msg501;
            else
                hs->response_hdr.string[HDR_STRINGS_IDX_HTTP_STATUS] = msg200;

            set_content_type(hs, uri);
        }

    } else if(uri)
        set_content_type(hs, uri);

    /* Set up to send the first header string. */
    hs->response_hdr.index = 0;
    hs->response_hdr.pos = 0;
}

/** Sub-function of http_send(): send dynamic headers
 *
 * @returns: - HTTPSend_NoData: no new data has been enqueued
 *           - HTTPSend_Continue: continue with sending HTTP body
 *           - HTTPSend_Break: data has been enqueued, headers pending,
 *                                      so don't send HTTP body yet
 *           - HTTPSend_Freed: http_state and pcb are already freed
 */
static http_send_state_t http_send_headers (struct altcp_pcb *pcb, http_state_t *hs)
{
    err_t err;
    u16_t len, hdrlen, sendlen;
    http_send_state_t data_to_send = HTTPSend_NoData;

    if (!is_response_header_set(hs, "Content-Length")) {
        get_http_content_length(hs, hs->handle != NULL ? hs->handle->size : -1);
//        get_http_content_length(hs, (hs->handle != NULL) && (hs->handle->flags & FS_FILE_FLAGS_HEADER_PERSISTENT) ? hs->handle->len : -1);
    }

    if(hs->method == HTTP_Head && hs->handle) {
        vfs_close(hs->handle);
        hs->handle = NULL;
    }

    /* How much data can we send? */
    len = sendlen = altcp_sndbuf(pcb);

    while (len && (hs->response_hdr.index < NUM_FILE_HDR_STRINGS) && sendlen) {
        const void *ptr;
        u16_t old_sendlen;
        u8_t apiflags;
        /* How much do we have to send from the current header? */
        hdrlen = (u16_t)strlen(hs->response_hdr.string[hs->response_hdr.index]);

        /* How much of this can we send? */
        sendlen = (len < (hdrlen - hs->response_hdr.pos)) ? len : (hdrlen - hs->response_hdr.pos);

        /* Send this amount of data or as much as we can given memory constraints. */
        ptr = (const void *)(hs->response_hdr.string[hs->response_hdr.index] + hs->response_hdr.pos);
        old_sendlen = sendlen;
        apiflags = HTTP_IS_HDR_VOLATILE(hs, ptr);

        if (hs->response_hdr.type[hs->response_hdr.index] != HTTP_HeaderTypeROM) {
            /* content-length is always volatile */
            apiflags |= TCP_WRITE_FLAG_COPY;
        }

        if (hs->response_hdr.index < NUM_FILE_HDR_STRINGS - 1)
            apiflags |= TCP_WRITE_FLAG_MORE;

        if (((err = http_write(pcb, ptr, &sendlen, apiflags)) == ERR_OK) && (old_sendlen != sendlen)) {
            /* Remember that we added some more data to be transmitted. */
            data_to_send = HTTPSend_Continue;
        } else if (err != ERR_OK) {
            /* special case: http_write does not try to send 1 byte */
            sendlen = 0;
        }

        /* Fix up the header position for the next time round. */
        hs->response_hdr.pos += sendlen;
        len -= sendlen;

        /* Have we finished sending this string? */
        if (hs->response_hdr.pos == hdrlen) {
            /* Yes - move on to the next one */
            hs->response_hdr.index++;
            /* skip headers that are NULL (not all headers are required) */
            while ((hs->response_hdr.index < NUM_FILE_HDR_STRINGS) && (hs->response_hdr.string[hs->response_hdr.index] == NULL)) {
                hs->response_hdr.index++;
            }
            hs->response_hdr.pos = 0;
        }
    }

    if ((hs->response_hdr.index >= NUM_FILE_HDR_STRINGS) && (hs->file == NULL)) {
        /* When we are at the end of the headers, check for data to send
        * instead of waiting for ACK from remote side to continue
        * (which would happen when sending files from async read). */
        if (http_check_eof(pcb, hs)) {
            data_to_send = HTTPSend_Break;
        } else {
            /* At this point, for non-keepalive connections, hs is deallocated and pcb is closed. */
            return HTTPSend_Freed;
        }
    }

    /* If we get here and there are still header bytes to send, we send
    * the header information we just wrote immediately. If there are no
    * more headers to send, but we do have file data to send, drop through
    * to try to send some file data too. */
    if ((hs->response_hdr.index < NUM_FILE_HDR_STRINGS) || !hs->file) {
        LWIP_DEBUGF(HTTPD_DEBUG, ("tcp_output\n"));
        return HTTPSend_Break;
    }

    return data_to_send;
}
#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */

/** Sub-function of http_send(): end-of-file (or block) is reached,
 * either close the file or read the next block (if supported).
 *
 * @returns: false if the file is finished or no data has been read
 *           true if the file is not finished and data has been read
 */
static bool http_check_eof (struct altcp_pcb *pcb, http_state_t *hs)
{
    int bytes_left;
#if LWIP_HTTPD_DYNAMIC_FILE_READ
    int count;
  #ifdef HTTPD_MAX_WRITE_LEN
    int max_write_len;
  #endif /* HTTPD_MAX_WRITE_LEN */
#endif /* LWIP_HTTPD_DYNAMIC_FILE_READ */

    /* Do we have a valid file handle? */
    if (hs->handle == NULL) {
        /* No - close the connection. */
        http_eof(pcb, hs);
        return false;
    }

    if ((bytes_left = hs->handle->size - vfs_tell(hs->handle)) <= 0) {
        /* We reached the end of the file so this request is done. */
        LWIP_DEBUGF(HTTPD_DEBUG, ("End of file.\n"));
        http_eof(pcb, hs);
        return false;
    }

#if LWIP_HTTPD_DYNAMIC_FILE_READ
    /* Do we already have a send buffer allocated? */
    if (hs->buf) {
        /* Yes - get the length of the buffer */
        count = LWIP_MIN(hs->buf_len, bytes_left);
    } else {
        /* We don't have a send buffer so allocate one now */
        count = altcp_sndbuf(pcb);
        if (bytes_left < count)
            count = bytes_left;

  #ifdef HTTPD_MAX_WRITE_LEN
        /* Additional limitation: e.g. don't enqueue more than 2*mss at once */
        max_write_len = HTTPD_MAX_WRITE_LEN(pcb);
        if (count > max_write_len)
            count = max_write_len;

 #endif /* HTTPD_MAX_WRITE_LEN */

 #if defined(STM32H743xx)
        /*
         * Ensure read sizes are a multiple of 32 bytes, this helps maintain buffer alignment
         * with L1 cache lines when performing sequential reads through a file.
         *
         * Without correct alignment, the low level drivers fall back to single sector reads,
         * resulting in a significant performance impact.
         */

        if (count != bytes_left) {
            /* buffer size does not reach the end of file, so round down if not a multiple of 32 bytes */
            if (count & 0x1F) {
                count = count & ~0x1F;
            }
        }
 #endif

        do {
            if ((hs->buf = (char *)mem_malloc((mem_size_t)count))) {
                hs->buf_len = count;
                break;
            }
            count = count / 2;
        } while (count > 100);

        /* Did we get a send buffer? If not, return immediately. */
        if (hs->buf == NULL) {
            LWIP_DEBUGF(HTTPD_DEBUG, ("No buff\n"));
            return false;
        }
    }

    /* Read a block of data from the file. */
    LWIP_DEBUGF(HTTPD_DEBUG, ("Trying to read %d bytes.\n", count));

#if LWIP_HTTPD_FS_ASYNC_READ
    count = fs_read_async(hs->handle, hs->buf, count, http_continue, hs);
#else /* LWIP_HTTPD_FS_ASYNC_READ */
    count = vfs_read(hs->buf, 1, count, hs->handle);
#endif /* LWIP_HTTPD_FS_ASYNC_READ */
    if (vfs_errno) {
        if (count == -2 /* FS_READ_DELAYED*/) {
            /* Delayed read, wait for FS to unblock us */
            return false;
        }
        /* We reached the end of the file so this request is done.
        * @todo: close here for HTTP/1.1 when reading file fails */
        LWIP_DEBUGF(HTTPD_DEBUG, ("End of file.\n"));
        http_eof(pcb, hs);

        return false;
    }

    /* Set up to send the block of data we just read */
    LWIP_DEBUGF(HTTPD_DEBUG, ("Read %d bytes.\n", count));
    hs->left = count;
    hs->file = hs->buf;
#if LWIP_HTTPD_SSI
    if (hs->ssi) {
        hs->ssi->parse_left = count;
        hs->ssi->parsed = hs->buf;
    }
#endif /* LWIP_HTTPD_SSI */
#else /* LWIP_HTTPD_DYNAMIC_FILE_READ */
    LWIP_ASSERT("SSI and DYNAMIC_HEADERS turned off but eof not reached", 0);
#endif /* LWIP_HTTPD_SSI || LWIP_HTTPD_DYNAMIC_HEADERS */

    return true;
}

/** Sub-function of http_send(): This is the normal send-routine for non-ssi files
 *
 * @returns: - HTTPSend_Continue: data has been written (so call tcp_ouput)
 *           - HTTPSend_NoData: no data has been written (no need to call tcp_output)
 */
static http_send_state_t http_send_data_nonssi (struct altcp_pcb *pcb, http_state_t *hs)
{
    u16_t len;
    http_send_state_t data_to_send;

    /* We are not processing an SHTML file so no tag checking is necessary.
    * Just send the data as we received it from the file. */
    len = (u16_t)LWIP_MIN(hs->left, 0xffff);

    if ((data_to_send = (http_write(pcb, hs->file, &len, HTTP_IS_DATA_VOLATILE(hs)) == ERR_OK) ? HTTPSend_Continue : HTTPSend_NoData)) {
        hs->file += len;
        hs->left -= len;
    }

    return data_to_send;
}

/**
 * Try to send more data on this pcb.
 *
 * @param pcb the pcb to send data
 * @param hs connection state
 */
static http_send_state_t http_send (struct altcp_pcb *pcb, http_state_t *hs)
{
    http_send_state_t data_to_send = HTTPSend_NoData;

    LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("http_send: pcb=%p hs=%p left=%d\n", (void *)pcb, (void *)hs, hs != NULL ? (int)hs->left : 0));

#if LWIP_HTTPD_POST_MANUAL_WND
    if (hs->unrecved_bytes != 0)
        return HTTPSend_NoData;
#endif /* LWIP_HTTPD_SUPPORT_POST && LWIP_HTTPD_POST_MANUAL_WND */

    /* If we were passed a NULL state structure pointer, ignore the call. */
    if (hs == NULL)
        return HTTPSend_NoData;

#if LWIP_HTTPD_FS_ASYNC_READ
    /* Check if we are allowed to read from this file.
    (e.g. SSI might want to delay sending until data is available) */
    if (!fs_is_file_ready(hs->handle, http_continue, hs))
        return HTTPSend_NoData;

#endif /* LWIP_HTTPD_FS_ASYNC_READ */

#if LWIP_HTTPD_DYNAMIC_HEADERS
    /* Do we have any more header data to send for this file? */
    if (hs->response_hdr.index < NUM_FILE_HDR_STRINGS) {
        data_to_send = http_send_headers(pcb, hs);
        if ((data_to_send == HTTPSend_Freed) || ((data_to_send != HTTPSend_Continue) && (hs->response_hdr.index < NUM_FILE_HDR_STRINGS)))
            return data_to_send;
    }
#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */

    /* Have we run out of file data to send? If so, we need to read the next
    * block from the file. */
    if (hs->left == 0 && !http_check_eof(pcb, hs))
        return HTTPSend_NoData;

    data_to_send = http_send_data_nonssi(pcb, hs);

    if(hs->left == 0 && vfs_eof(hs->handle)) {
        /* We reached the end of the file so this request is done.
        * This adds the FIN flag right into the last data segment. */
        LWIP_DEBUGF(HTTPD_DEBUG, ("End of file.\n"));
        http_eof(pcb, hs);

        return HTTPSend_NoData;
    }

    LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("send_data end.\n"));

    return data_to_send;
}

#if LWIP_HTTPD_SUPPORT_EXTSTATUS
/** Initialize a http connection with a file to send for an error message
 *
 * @param hs http connection state
 * @param error_nr HTTP error number
 * @return ERR_OK if file was found and hs has been initialized correctly
 *         another err_t otherwise
 */
static err_t http_find_error_file (http_state_t *hs, u16_t error_nr)
{
    vfs_file_t *file;
    const char *uri, *uri1, *uri2, *uri3;

    if (error_nr == 501) {
        uri1 = "/501.html";
        uri2 = "/501.htm";
        uri3 = "/501.shtml";
    } else {
        /* 400 (bad request is the default) */
        uri1 = "/400.html";
        uri2 = "/400.htm";
        uri3 = "/400.shtml";
    }

    if((file = vfs_open(uri1, "r")))
        uri = uri1;
    else if((file = vfs_open(uri2, "r")))
        uri = uri2;
    else if((file = vfs_open(uri3, "r")))
        uri = uri3;
    else {
        LWIP_DEBUGF(HTTPD_DEBUG, ("Error page for error %"U16_F" not found\n", error_nr));
        return ERR_ARG;
    }

    return http_init_file(hs, file, uri, NULL);
}
#else /* LWIP_HTTPD_SUPPORT_EXTSTATUS */
#define http_find_error_file(hs, error_nr) ERR_ARG
#endif /* LWIP_HTTPD_SUPPORT_EXTSTATUS */

/**
 * Get the file struct for a 404 error page.
 * Tries some file names and returns NULL if none found.
 *
 * @param uri pointer that receives the actual file name URI
 * @return file struct for the error page or NULL no matching file was found
 */
static vfs_file_t *http_get_404_file (http_state_t *hs, const char **uri)
{
    vfs_file_t *file;

    *uri = "/404.html";
    if ((file = vfs_open(*uri, "r")) == NULL) {
        /* 404.html doesn't exist. Try 404.htm instead. */
        *uri = "/404.htm";
        file = vfs_open(*uri, "r");
    }

    if (file == NULL) {
        /* 404.htm doesn't exist either. Try 404.shtml instead. */
        *uri = "/404.shtml";
        file = vfs_open(*uri, "r");
    }

    if (file == NULL) {
        /* 404.htm doesn't exist either. Indicate to the caller that it should
        * send back a default 404 page.
        */
        *uri = NULL;
    }

    return file;
}

static err_t http_handle_post_finished (http_state_t *hs)
{
#if LWIP_HTTPD_POST_MANUAL_WND
    /* Prevent multiple calls to httpd_post_finished, since it might have already
    been called before from httpd_post_data_recved(). */
    if (hs->post_finished)
        return ERR_OK;
    hs->post_finished = 1;
#endif /* LWIP_HTTPD_POST_MANUAL_WND */

    /* application error or POST finished */
    /* NULL-terminate the buffer */
    *http_uri_buf = '\0';
    hs->request.post_finished(&hs->request, http_uri_buf, LWIP_HTTPD_URI_BUF_LEN);

    const char *uri = NULL;
    vfs_file_t *file = NULL;

    if(*http_uri_buf == '\0')
        get_http_headers(hs, NULL);
    else {
        uri = http_uri_buf;
        if((file = vfs_open(uri, "r")) == NULL)
            file = http_get_404_file(hs, &uri);
    }

    return uri ? http_init_file(hs, file, uri, NULL) : ERR_OK;
}

/** Pass received POST body data to the application and correctly handle
 * returning a response document or closing the connection.
 * ATTENTION: The application is responsible for the pbuf now, so don't free it!
 *
 * @param hs http connection state
 * @param p pbuf to pass to the application
 * @return ERR_OK if passed successfully, another err_t if the response file
 *         hasn't been found (after POST finished)
 */
static err_t http_post_rxpbuf (http_state_t *hs, struct pbuf *p)
{
    err_t err;

    if (p != NULL) {
        /* adjust remaining Content-Length */
        if (hs->post_content_len_left < p->tot_len)
            hs->post_content_len_left = 0;
        else
            hs->post_content_len_left -= p->tot_len;
    }

#if LWIP_HTTPD_POST_MANUAL_WND
    /* prevent connection being closed if httpd_post_data_recved() is called nested */
    hs->unrecved_bytes++;
#endif

    err = p == NULL ? ERR_OK : hs->request.post_receive_data(&hs->request, p);

#if LWIP_HTTPD_POST_MANUAL_WND
        hs->unrecved_bytes--;
#endif

    if (err != ERR_OK)  /* Ignore remaining content in case of application error */
        hs->post_content_len_left = 0;

    if (hs->post_content_len_left == 0) {
#if LWIP_HTTPD_POST_MANUAL_WND
        if (hs->unrecved_bytes != 0)
            return ERR_OK;
#endif /*LWIP_HTTPD_POST_MANUAL_WND */
        /* application error or POST finished */
        return http_handle_post_finished(hs);
    }

    return ERR_OK;
}

void httpd_free_pbuf (http_request_t *request, struct pbuf *p)
{
    altcp_recved(request->handle->pcb, p->tot_len);
    pbuf_free(p);
}

#if LWIP_HTTPD_POST_MANUAL_WND
/**
 * @ingroup httpd
 * A POST implementation can call this function to update the TCP window.
 * This can be used to throttle data reception (e.g. when received data is
 * programmed to flash and data is received faster than programmed).
 *
 * @param connection A connection handle passed to httpd_post_begin for which
 *        httpd_post_finished has *NOT* been called yet!
 * @param recved_len Length of data received (for window update)
 */
void httpd_post_data_recved(void *connection, u16_t recved_len)
{
  http_state_t *hs = (http_state_t *)connection;
  if (hs != NULL) {
    if (hs->no_auto_wnd) {
      u16_t len = recved_len;
      if (hs->unrecved_bytes >= recved_len) {
        hs->unrecved_bytes -= recved_len;
      } else {
        LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_LEVEL_WARNING, ("httpd_post_data_recved: recved_len too big\n"));
        len = (u16_t)hs->unrecved_bytes;
        hs->unrecved_bytes = 0;
      }
      if (hs->pcb != NULL) {
        if (len != 0) {
          altcp_recved(hs->pcb, len);
        }
        if ((hs->post_content_len_left == 0) && (hs->unrecved_bytes == 0)) {
          /* finished handling POST */
          http_handle_post_finished(hs);
          http_send(hs->pcb, hs);
        }
      }
    }
  }
}
#endif /* LWIP_HTTPD_POST_MANUAL_WND */

#if LWIP_HTTPD_FS_ASYNC_READ
/** Try to send more data if file has been blocked before
 * This is a callback function passed to fs_read_async().
 */
static void http_continue(void *connection)
{
  http_state_t *hs = (http_state_t *)connection;
  LWIP_ASSERT_CORE_LOCKED();
  if (hs && (hs->pcb) && (hs->handle)) {
    LWIP_ASSERT("hs->pcb != NULL", hs->pcb != NULL);
    LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("httpd_continue: try to send more data\n"));
    if (http_send(hs->pcb, hs)) {
      /* If we wrote anything to be sent, go ahead and send it now. */
      LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("tcp_output\n"));
      altcp_output(hs->pcb);
    }
  }
}
#endif /* LWIP_HTTPD_FS_ASYNC_READ */

err_t http_get_payload (http_request_t *request, uint32_t len)
{
    http_state_t *hs = request->handle;

    if((hs->post_content_len_left = len) > 0) {

        struct pbuf *q = hs->req;
        u16_t start_offset = hs->payload_offset;

        /* get to the pbuf where the body starts */
        while ((q != NULL) && (q->len <= start_offset)) {
            start_offset -= q->len;
            q = q->next;
        }

        if (q != NULL) {
            /* hide the remaining HTTP header */
#if LWIP_VERSION_MAJOR > 1 && LWIP_VERSION_MINOR > 0
            pbuf_remove_header(q, start_offset);
#else
            pbuf_header(q, -(s16_t)start_offset);
#endif
#if LWIP_HTTPD_POST_MANUAL_WND
            if (!post_auto_wnd) {
                /* already tcp_recved() this data... */
                hs->unrecved_bytes = q->tot_len;
            }
#endif /* LWIP_HTTPD_POST_MANUAL_WND */
            pbuf_ref(q);
            return http_post_rxpbuf(hs, q);
        } else if (hs->post_content_len_left == 0) {
            q = pbuf_alloc(PBUF_RAW, 0, PBUF_REF);
            return http_post_rxpbuf(hs, q);
        } else
            return ERR_OK;
    }

    return ERR_OK;
}

/**
 * When data has been received in the correct state, try to parse it as a HTTP request.
 *
 * @param inp the received pbuf
 * @param hs the connection state
 * @param pcb the altcp_pcb which received this packet
 * @return ERR_OK if request was OK and hs has been initialized correctly
 *         ERR_INPROGRESS if request was OK so far but not fully received
 *         another err_t otherwise
 */
static err_t http_parse_request (struct pbuf *inp, http_state_t *hs, struct altcp_pcb *pcb)
{
    char *data;
    u16_t data_len;
    int clen;
    struct pbuf *p = inp;

    LWIP_UNUSED_ARG(pcb); /* only used for post */
    LWIP_ASSERT("p != NULL", p != NULL);
    LWIP_ASSERT("hs != NULL", hs != NULL);

    if ((hs->handle != NULL) || (hs->file != NULL)) {
        LWIP_DEBUGF(HTTPD_DEBUG, ("Received data while sending a file\n"));
        /* already sending a file */
        /* @todo: abort? */
        return ERR_USE;
    }

    LWIP_DEBUGF(HTTPD_DEBUG, ("Received %"U16_F" bytes\n", p->tot_len));

    /* first check allowed characters in this pbuf? */

    /* enqueue the pbuf */
    if (hs->req == NULL) {
        LWIP_DEBUGF(HTTPD_DEBUG, ("First pbuf\n"));
        hs->req = p;
    } else {
        LWIP_DEBUGF(HTTPD_DEBUG, ("pbuf enqueued\n"));
        pbuf_cat(hs->req, p);
    }
    /* increase pbuf ref counter as it is freed when we return but we want to keep it on the req list */
    pbuf_ref(p);

    if (hs->req->next != NULL) {
        data_len = LWIP_MIN(hs->req->tot_len, LWIP_HTTPD_MAX_REQ_LENGTH);
        pbuf_copy_partial(hs->req, httpd_req_buf, data_len, 0);
        data = httpd_req_buf;
    } else {
        data = (char *)p->payload;
        data_len = p->len;
        if (p->len != p->tot_len) {
            LWIP_DEBUGF(HTTPD_DEBUG, ("Warning: incomplete header due to chained pbufs\n"));
        }
    }

    /* received enough data for minimal request and at least one CRLF? */
    if (data_len >= MIN_REQ_LEN && lwip_strnstr(data, CRLF, data_len)) {

        char *sp1, *sp2;
        u16_t left_len, uri_len;
        LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("CRLF received, parsing request\n"));

        /* parse method */
        int32_t method = -1;
        if((sp1 = strchr(data, ' '))) {
            *sp1 = '\0';
            method = strlookup(data, http_methods, ',');
            if(method >= 0)
                hs->method = (http_method_t)method;
        }

        if(method >= 0) {
            LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Received %s request\"\n", data));
        } else {
            /* unsupported method! */
            LWIP_DEBUGF(HTTPD_DEBUG, ("Unsupported request method (not implemented): \"%s\"\n", data));
            return http_find_error_file(hs, 501);
        }

        *sp1 = ' ';

        /* if we come here, method is OK, parse URI */
        left_len = (u16_t)(data_len - ((sp1 + 1) - data));
        sp2 = lwip_strnstr(sp1 + 1, " ", left_len);

        uri_len = (u16_t)(sp2 - (sp1 + 1));
        if((sp2 != NULL) && (sp2 > sp1)) {
            char *crlfcrlf;
            /* wait for CRLFCRLF (indicating end of HTTP headers) before parsing anything */
            if ((crlfcrlf = lwip_strnstr(data, CRLF CRLF, data_len)) != NULL) {
                char *uri = sp1 + 1;
#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
            /* This is HTTP/1.0 compatible: for strict 1.1, a connection
               would always be persistent unless "close" was specified. */
                hs->keepalive = ((lwip_strnstr(data, HTTP11_CONNECTIONKEEPALIVE, data_len) ||
                                   lwip_strnstr(data, HTTP11_CONNECTIONKEEPALIVE2, data_len)));
#endif /* LWIP_HTTPD_SUPPORT_11_KEEPALIVE */
                /* null-terminate the METHOD (pbuf is freed anyway when returning) */
                *sp1 = '\0';
                uri[uri_len] = '\0';
                LWIP_DEBUGF(HTTPD_DEBUG, ("Received \"%s\" request for URI: \"%s\"\n", data, uri));

                hs->hdr = strstr(sp2 + 1, CRLF) + 2;
                hs->hdr_len = crlfcrlf - hs->hdr + 4;
                hs->payload_offset = crlfcrlf - data + 4;
/*
                hal.stream.write(uri);
                hal.stream.write(" - ");
                hal.stream.write(uitoa(hs->method));
                hal.stream.write(ASCII_EOL);
                hal.stream.write_n(hs->hdr, hs->hdr_len);
                hal.stream.write("!");
                hal.stream.write(ASCII_EOL);
*/
#if LWIP_HTTPD_DYNAMIC_HEADERS
                memset(&hs->response_hdr, 0, sizeof(http_headers_t));
                hs->response_hdr.string[HDR_STRINGS_IDX_SERVER_NAME] = agent;   // In all cases, the second header we send is the server identification so set it here.
                hs->response_hdr.index = NUM_FILE_HDR_STRINGS;                  // Indicate that the headers are not yet valid
                hs->response_hdr.next = HDR_STRINGS_IDX_CONTENT_NEXT;
#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */

                if (hs->method == HTTP_Post) {

                    int content_len = -1;
                    clen = http_get_header_value_len(&hs->request, "Content-Length");

                    if(clen >= 0 && clen < HTTP_HDR_CONTENT_LEN_DIGIT_MAX_LEN) {

                        char clens[HTTP_HDR_CONTENT_LEN_DIGIT_MAX_LEN];
                        http_get_header_value(&hs->request, "Content-Length", clens, clen);
                        content_len = atoi(clens); // use strtol? https://www.cplusplus.com/reference/cstdlib/strtol/
                        if (content_len == 0) {
                            /* if atoi returns 0 on error, fix this */
                            if ((clens[0] != '0') || (clens[1] != '\r'))
                                content_len = -1;
                        }
                    }

                    if(content_len >= 0) {  /* set the Content-Length to be received for this POST */
#if LWIP_HTTPD_POST_MANUAL_WND
                        hs->no_auto_wnd = 1;
#endif /* LWIP_HTTPD_POST_MANUAL_WND */

                        hs->post_content_len_left = (u32_t)content_len;
                    } else {
                        LWIP_DEBUGF(HTTPD_DEBUG, ("POST received invalid Content-Length: %s\n", content_len));
                        goto badrequest;
                    }
                }

                return http_process_request(hs, uri);
            }
        } else {
            LWIP_DEBUGF(HTTPD_DEBUG, ("invalid URI\n"));
        }
    }

    clen = pbuf_clen(hs->req);
    if ((hs->req->tot_len <= LWIP_HTTPD_REQ_BUFSIZE) && (clen <= LWIP_HTTPD_REQ_QUEUELEN)) {
        /* request not fully received (too short or CRLF is missing) */
        return ERR_INPROGRESS;
    } else {
        badrequest:
        LWIP_DEBUGF(HTTPD_DEBUG, ("bad request\n"));
        /* could not parse request */
        return http_find_error_file(hs, 400);
    }
}

/** Try to find the file specified by uri and, if found, initialize hs accordingly.
 * @param hs the connection state
 * @param uri the HTTP header URI
 * @return ERR_OK if file was found and hs has been initialized correctly
 *         another err_t otherwise
 */
static err_t http_process_request (http_state_t *hs, const char *uri)
{
    char *params = NULL;
    vfs_file_t *file = NULL;
    const httpd_uri_handler_t *uri_handler = NULL;

    /* First, isolate the base URI (without any parameters) */
    if((params = strchr(uri, '?'))) /* URI contains parameters. NULL-terminate the base URI */
        *params = '\0';

    urldecode((char *)uri, uri);

    if(params) { /* URI contains parameters. Move parameters to end of potentially shorter base URI and reinstate the parameter separator. */

        hs->param_count = extract_uri_parameters(hs, params + 1);

        char *s1 = strchr(uri, '\0'), *s2 = params + 1;
        *s1++ = '?';
        while(*s2)
            *s1++ = *s2++;
        *s1 = '\0';
        params = strchr(uri, '?');
    }

    if(num_uri_handlers) {

        uint_fast8_t i;
        bool match = false;

        if(params) /* URI contains parameters. NULL-terminate the base URI */
            *params = '\0';

        /* Does the base URI we have isolated correspond to a handler? */
        for (i = 0; i < num_uri_handlers; i++) {

            uint_fast8_t len = strlen(uri_handlers[i].uri);
            match = !(uri_handlers[i].uri[len - 1] == '*' ? strncmp(uri, uri_handlers[i].uri, len - 1) : strcmp(uri, uri_handlers[i].uri));

            if ((match = (match && uri_handlers[i].method == hs->method))) {
                uri_handler = &uri_handlers[i];
                break;
            }
        }

        if(params) /* URI contains parameters. Reinstate the parameter separator. */
            *params = '?';
    }

    switch(hs->method) {

        case HTTP_Get:
            if(params == NULL) {
            	bool is_dir;
                size_t loop;
                /* Have we been asked for the default file (in root or a directory) ? */
#if LWIP_HTTPD_MAX_REQUEST_URI_LEN
                size_t uri_len = strlen(uri);
                if((is_dir = (uri_len > 0) && (uri[uri_len - 1] == '/') && ((uri != http_uri_buf) || (uri_len == 1)))) {
                    size_t copy_len = LWIP_MIN(sizeof(http_uri_buf) - 1, uri_len - 1);
                    if (copy_len > 0) {
                        MEMCPY(http_uri_buf, uri, copy_len);
                        http_uri_buf[copy_len] = '\0';
                    }
#else /* LWIP_HTTPD_MAX_REQUEST_URI_LEN */
                if((is_dir = (uri[0] == '/') && (uri[1] == '\0'))) {
#endif /* LWIP_HTTPD_MAX_REQUEST_URI_LEN */
                    /* Try each of the configured default filenames until we find one that exists. */
                    for (loop = 0; loop < NUM_DEFAULT_FILENAMES; loop++) {
                        const char *file_name;
#if LWIP_HTTPD_MAX_REQUEST_URI_LEN
                        if (copy_len > 0) {
                            size_t len_left = sizeof(http_uri_buf) - copy_len - 1;
                            if (len_left > 0) {
                                size_t name_len = strlen(httpd_default_filenames[loop].name);
                                size_t name_copy_len = LWIP_MIN(len_left, name_len);
                                MEMCPY(&http_uri_buf[copy_len], httpd_default_filenames[loop].name, name_copy_len);
                                http_uri_buf[copy_len + name_copy_len] = 0;
                            }
                            file_name = http_uri_buf;
                        } else
#endif /* LWIP_HTTPD_MAX_REQUEST_URI_LEN */
                        file_name = httpd_default_filenames[loop].name;
                        LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Looking for %s...\n", file_name));
                        if ((file = vfs_open(file_name, "r")) != NULL) {
                            uri = file_name;
                            hs->request.encoding = httpd_default_filenames[loop].encoding;
                            LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Opened.\n"));
                            break;
                        }
                    }
                }

                if(file == NULL && uri_handler == NULL && !is_dir) {
                    if((file = vfs_open(uri, "r")) == NULL && httpd.on_open_file_failed)
                        uri = httpd.on_open_file_failed(&hs->request, uri, &file, "r");
                }
            } 

            if(file == NULL && uri_handler) {
                if(params)
                    *params = '\0'; /* URI contains parameters. NULL-terminate the base URI */
                hs->uri = uri + strlen(uri_handler->uri) - 2;
                uri = uri_handler->handler(&hs->request);
            }
            break;

        case HTTP_Options:
            {
                char c, *s1, *s2, *allow;
                uint32_t len = strlen(http_methods);

                http_set_response_status(&hs->request, "200 OK");

                if((allow = s2 = malloc(len + 1))) {
                    s1 = (char *)http_methods;
                    while(*s1 == ',')
                        s1++;

                    while((c = *s1++)) {
                        if(!(c == ',' && *s1 == ','))
                            *s2++ = c;
                    }
                    *s2 = '\0';

                    http_set_response_header(&hs->request, "Allow", allow);
                    free(allow);
                } else {
#if LWIP_HTTPD_SUPPORT_POST
                    http_set_response_header(&hs->request, "Allow", "GET,POST,OPTIONS");
#else
                    http_set_response_header(&hs->request, "Allow", "GET,OPTIONS");
#endif
                }

                if(httpd.on_options_report)
                    httpd.on_options_report(&hs->request);
            }

            return http_init_file(hs, NULL, uri, params); //ERR_OK;
            break;

        default:
            if(uri_handler) {
                if(params)
                    *params = '\0'; /* URI contains parameters. NULL-terminate the base URI */
                hs->uri = uri + strlen(uri_handler->uri) - 2;
                uri = uri_handler->handler(&hs->request);
            } else if(httpd.on_unknown_method_process) {

                size_t uri_len = strlen(uri);
                if(uri_len > 0) {
                    size_t copy_len = LWIP_MIN(sizeof(http_uri_buf) - 1, uri_len);
                    if (copy_len > 0) {
                        MEMCPY(http_uri_buf, uri, copy_len);
                        http_uri_buf[copy_len] = '\0';
                    }
                } else
                    *http_uri_buf = '\0';

                if(httpd.on_unknown_method_process(&hs->request, hs->method, http_uri_buf, LWIP_HTTPD_URI_BUF_LEN) == ERR_OK) {
                    if(*http_uri_buf != '\0') {
                        uri = http_uri_buf;
                        if((file = vfs_open(uri, "r")) == NULL)
                            file = http_get_404_file(hs, &uri);
                    }
                }
            }
            break;
    }

    if(file == NULL) switch(hs->method) {

        case HTTP_Get:
        case HTTP_Head:
            if(uri) {
                if(params)
                    *params = '\0'; /* URI contains parameters. NULL-terminate the base URI */
                LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("Opening %s\n", uri));
                if((file = vfs_open(uri, "r")) == NULL) {
                    if(httpd.on_open_file_failed)
                        uri = httpd.on_open_file_failed(&hs->request, uri, &file, "r");
                }
            }
            if(file == NULL)
                file = http_get_404_file(hs, &uri);
            break;

        case HTTP_Post:
            if(uri_handler) {
                if(hs->request.post_receive_data == NULL || hs->request.post_finished == NULL) {
                    // Internal server error!
                }
                if(uri == NULL && hs->post_content_len_left > 0)
                    return http_get_payload(&hs->request, hs->post_content_len_left);
            } else
                file = http_get_404_file(hs, &uri);
            break;

        default:
            break;
    }

    return hs->method == HTTP_Post && uri_handler ? ERR_OK : http_init_file(hs, file, uri, params);
}

/** Initialize a http connection with a file to send (if found).
 * Called by http_find_file and http_find_error_file.
 *
 * @param hs http connection state
 * @param file file structure to send (or NULL if not found)
 * @param uri the HTTP header URI
 * @param tag_check enable SSI tag checking
 * @param params != NULL if URI has parameters (separated by '?')
 * @return ERR_OK if file was found and hs has been initialized correctly
 *         another err_t otherwise
 */
static err_t http_init_file (http_state_t *hs, vfs_file_t *file, const char *uri, char *params)
{
    LWIP_UNUSED_ARG(params);

    if (file != NULL) {
        /* file opened, initialise http_state_t */
#if !LWIP_HTTPD_DYNAMIC_FILE_READ
        /* If dynamic read is disabled, file data must be in one piece and available now */
        LWIP_ASSERT("file->data != NULL", file->data != NULL);
#endif

        hs->handle = file;
        hs->file = NULL;

//        hs->file = file->data;
//        LWIP_ASSERT("File length must be positive!", (file->size >= 0));
#if LWIP_HTTPD_CUSTOM_FILES
        if (file->is_custom_file && (file->data == NULL))
            /* custom file, need to read data first (via fs_read_custom) */
            hs->left = 0;
        else
#endif /* LWIP_HTTPD_CUSTOM_FILES */
            hs->left = (u32_t)file->size;

        hs->retries = 0;

#if LWIP_HTTPD_TIMING
        hs->time_started = sys_now();
#endif /* LWIP_HTTPD_TIMING */
#if !LWIP_HTTPD_DYNAMIC_HEADERS
        LWIP_ASSERT("HTTP headers not included in file system", (hs->handle->flags & FS_FILE_FLAGS_HEADER_INCLUDED) != 0);
#endif /* !LWIP_HTTPD_DYNAMIC_HEADERS */

    } else {
        hs->handle = NULL;
        hs->file = NULL;
        hs->left = 0;
        hs->retries = 0;
    }
#if LWIP_HTTPD_DYNAMIC_HEADERS
    /* Determine the HTTP headers to send based on the file extension of the requested URI. */
//    if ((hs->handle == NULL) /* || ((hs->handle->flags & FS_FILE_FLAGS_HEADER_INCLUDED) == 0)*/)
        get_http_headers(hs, uri);
#else /* LWIP_HTTPD_DYNAMIC_HEADERS */
        LWIP_UNUSED_ARG(uri);
#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */

#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
    if (hs->keepalive) {
  #if LWIP_HTTPD_SSI
        if (hs->ssi != NULL)
            hs->keepalive = 0;
        else
  #endif /* LWIP_HTTPD_SSI */
//        if ((hs->handle != NULL) && ((hs->handle->flags & (FS_FILE_FLAGS_HEADER_INCLUDED | FS_FILE_FLAGS_HEADER_PERSISTENT)) == FS_FILE_FLAGS_HEADER_INCLUDED))
//            hs->keepalive = 0;
    }
#endif /* LWIP_HTTPD_SUPPORT_11_KEEPALIVE */

    return ERR_OK;
}

/**
 * The pcb had an error and is already deallocated.
 * The argument might still be valid (if != NULL).
 */
static void http_err (void *arg, err_t err)
{
    http_state_t *hs = (http_state_t *)arg;
    LWIP_UNUSED_ARG(err);

    LWIP_DEBUGF(HTTPD_DEBUG, ("http_err: %s", lwip_strerr(err)));

    if (hs != NULL)
        http_state_free(hs);
}

/**
 * Data has been sent and acknowledged by the remote host.
 * This means that more data can be sent.
 */
static err_t http_sent (void *arg, struct altcp_pcb *pcb, u16_t len)
{
  http_state_t *hs = (http_state_t *)arg;

  LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("http_sent %p\n", (void *)pcb));

  LWIP_UNUSED_ARG(len);

  if (hs) {
      hs->retries = 0;
      http_send(pcb, hs);
  }

  return ERR_OK;
}

/**
 * The poll function is called every 2nd second.
 * If there has been no data sent (which resets the retries) in 8 seconds, close.
 * If the last portion of a file has not been sent in 2 seconds, close.
 *
 * This could be increased, but we don't want to waste resources for bad connections.
 */
static err_t http_poll (void *arg, struct altcp_pcb *pcb)
{
    http_state_t *hs = (http_state_t *)arg;
    //  LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("http_poll: pcb=%p hs=%p pcb_state=%s\n",
    //              (void *)pcb, (void *)hs, tcp_debug_state_str(altcp_dbg_get_tcp_state(pcb))));

    if (hs == NULL) {

        err_t closed;
        /* arg is null, close. */
        LWIP_DEBUGF(HTTPD_DEBUG, ("http_poll: arg is NULL, close\n"));
        closed = http_close_conn(pcb, NULL);
        LWIP_UNUSED_ARG(closed);
#if LWIP_HTTPD_ABORT_ON_CLOSE_MEM_ERROR
        if (closed == ERR_MEM) {
            altcp_abort(pcb);
            return ERR_ABRT;
        }
#endif /* LWIP_HTTPD_ABORT_ON_CLOSE_MEM_ERROR */
        return ERR_OK;

    } else {
        hs->retries++;
        if (hs->retries == HTTPD_MAX_RETRIES) {
            LWIP_DEBUGF(HTTPD_DEBUG, ("http_poll: too many retries, close\n"));
            http_close_conn(pcb, hs);
            return ERR_OK;
        }

        /* If this connection has a file open, try to send some more data. If
        * it has not yet received a GET request, don't do this since it will
        * cause the connection to close immediately. */
        if (hs->handle) {
            LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("http_poll: try to send more data\n"));
            if (http_send(pcb, hs)) {
                /* If we wrote anything to be sent, go ahead and send it now. */
                LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("tcp_output\n"));
                altcp_output(pcb);
            }
        }
    }

    return ERR_OK;
}

/**
 * Data has been received on this pcb.
 * For HTTP 1.0, this should normally only happen once (if the request fits in one packet).
 */
static err_t http_recv (void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    http_state_t *hs = (http_state_t *)arg;

    LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("http_recv: pcb=%p pbuf=%p err=%s\n", (void *)pcb, (void *)p, lwip_strerr(err)));

    if ((err != ERR_OK) || (p == NULL) || (hs == NULL)) {
        /* error or closed by other side? */
        if (p != NULL) {
            /* Inform TCP that we have taken the data. */
            altcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        if (hs == NULL) {
            /* this should not happen, only to be robust */
            LWIP_DEBUGF(HTTPD_DEBUG, ("Error, http_recv: hs is NULL, close\n"));
        }
        http_close_conn(pcb, hs);
        return ERR_OK;
    }

#if LWIP_HTTPD_SUPPORT_POST && LWIP_HTTPD_POST_MANUAL_WND
    if (hs->no_auto_wnd)
        hs->unrecved_bytes += p->tot_len;
    else
#endif /* LWIP_HTTPD_SUPPORT_POST && LWIP_HTTPD_POST_MANUAL_WND */
    {
        /* Inform TCP that we have taken the data. */
        altcp_recved(pcb, p->tot_len);
    }

#if LWIP_HTTPD_SUPPORT_POST
    if(hs->request.post_receive_data) {
        if (hs->post_content_len_left > 0) {
            /* reset idle counter when POST data is received */
            hs->retries = 0;
            /* this is data for a POST, pass the complete pbuf to the application */
            http_post_rxpbuf(hs, p);
            /* pbuf is passed to the application, don't free it! */
            if (hs->post_content_len_left == 0) {
                /* all data received, send response or close connection */
                http_send(pcb, hs);
            }
        }

        return ERR_OK;
    }
#endif /* LWIP_HTTPD_SUPPORT_POST */

    if (hs->handle == NULL) {

        err_t parsed = http_parse_request(p, hs, pcb);
        LWIP_ASSERT("http_parse_request: unexpected return value", parsed == ERR_OK || parsed == ERR_INPROGRESS || parsed == ERR_ARG || parsed == ERR_USE);
        if (parsed != ERR_INPROGRESS) {
            /* request fully parsed or error */
            if (hs->req != NULL) {
                pbuf_free(hs->req);
                hs->req = NULL;
            }
        }
        pbuf_free(p);

        if (parsed == ERR_OK) {
#if LWIP_HTTPD_SUPPORT_POST
            if (hs->post_content_len_left == 0)
#endif /* LWIP_HTTPD_SUPPORT_POST */
                {
                    LWIP_DEBUGF(HTTPD_DEBUG | LWIP_DBG_TRACE, ("http_recv: data %p len %"S32_F"\n", (const void *)hs->file, hs->left));
                    http_send(pcb, hs);
                }
        } else if (parsed == ERR_ARG) {
            /* @todo: close on ERR_USE? */
            http_close_conn(pcb, hs);
        }
    } else {
        LWIP_DEBUGF(HTTPD_DEBUG, ("http_recv: already sending data\n"));
        /* already sending but still receiving data, we might want to RST here? */
        pbuf_free(p);
    }

    return ERR_OK;
}

/**
 * A new incoming connection has been accepted.
 */
static err_t http_accept (void *arg, struct altcp_pcb *pcb, err_t err)
{
    http_state_t *hs;

    LWIP_UNUSED_ARG(err);
    LWIP_UNUSED_ARG(arg);
    LWIP_DEBUGF(HTTPD_DEBUG, ("http_accept %p / %p\n", (void *)pcb, arg));

    if ((err != ERR_OK) || (pcb == NULL))
        return ERR_VAL;

    /* Set priority */
    altcp_setprio(pcb, HTTPD_TCP_PRIO);

    /* Allocate memory for the structure that holds the state of the
       connection - initialized by that function. */
    if ((hs = http_state_alloc()) == NULL) {
        LWIP_DEBUGF(HTTPD_DEBUG, ("http_accept: Out of memory, RST\n"));
        return ERR_MEM;
    }
    hs->pcb = pcb;

    /* Tell TCP that this is the structure we wish to be passed for our callbacks. */
    altcp_arg(pcb, hs);

    /* Set up the various callback functions */
    altcp_recv(pcb, http_recv);
    altcp_err(pcb, http_err);
    tcp_poll(pcb, http_poll, HTTPD_POLL_INTERVAL);
    altcp_sent(pcb, http_sent);

    return ERR_OK;
}

static err_t httpd_init_pcb (struct altcp_pcb *pcb, u16_t port)
{
    err_t err = ERR_USE;

    if (pcb) {
        altcp_setprio(pcb, HTTPD_TCP_PRIO);
        /* set SOF_REUSEADDR here to explicitly bind httpd to multiple interfaces */
        err = altcp_bind(pcb, IP_ANY_TYPE, port);
        LWIP_UNUSED_ARG(err); /* in case of LWIP_NOASSERT */
        LWIP_ASSERT("httpd_init: tcp_bind failed", err == ERR_OK);
        pcb = altcp_listen(pcb);
        LWIP_ASSERT("httpd_init: tcp_listen failed", pcb != NULL);
        altcp_accept(pcb, http_accept);
    }

    return err;
}

/**
 * @ingroup httpd
 * Initialize the httpd: set up a listening PCB and bind it to the defined port
 */
bool httpd_init (uint16_t port)
{
    struct altcp_pcb *pcb;

#if HTTPD_USE_MEM_POOL
    LWIP_MEMPOOL_INIT(HTTPD_STATE);
  #if LWIP_HTTPD_SSI
    LWIP_MEMPOOL_INIT(HTTPD_SSI_STATE);
  #endif
#endif
    LWIP_DEBUGF(HTTPD_DEBUG, ("httpd_init\n"));

/* LWIP_ASSERT_CORE_LOCKED(); is checked by tcp_new() */

    pcb = altcp_tcp_new_ip_type(IPADDR_TYPE_ANY);
    LWIP_ASSERT("httpd_init: tcp_new failed", pcb != NULL);

    return httpd_init_pcb(pcb, port) == ERR_OK;
}

#if HTTPD_ENABLE_HTTPS
/**
* @ingroup httpd
* Initialize the httpd: set up a listening PCB and bind it to the defined port.
* Also set up TLS connection handling (HTTPS).
*/
void httpd_inits (struct altcp_tls_config *conf)
{
#if LWIP_ALTCP_TLS
    struct altcp_pcb *pcb_tls = altcp_tls_new(conf, IPADDR_TYPE_ANY);
    LWIP_ASSERT("httpd_init: altcp_tls_new failed", pcb_tls != NULL);
    httpd_init_pcb(pcb_tls, HTTPD_SERVER_PORT_HTTPS);
#else /* LWIP_ALTCP_TLS */
    LWIP_UNUSED_ARG(conf);
#endif /* LWIP_ALTCP_TLS */
}
#endif /* HTTPD_ENABLE_HTTPS */

void httpd_register_uri_handlers(const httpd_uri_handler_t *httpd_uri_handlers, uint_fast8_t httpd_num_uri_handlers)
{
    uri_handlers = httpd_uri_handlers;
    num_uri_handlers = uri_handlers ? httpd_num_uri_handlers : 0;
}

#endif /* LWIP_TCP && LWIP_CALLBACK_API */
#endif // HTTP_ENABLE
